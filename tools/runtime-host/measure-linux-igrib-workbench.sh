#!/usr/bin/env bash
set -euo pipefail

readonly runtime_root="${OCPN_RUNTIME_HOST_ROOT:-/home/paul/RuntimeHost-OpenCPN}"
readonly launcher="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/launch-linux.sh"
readonly fixture="${PPM_RESOURCE_GRIB_FIXTURE:-/home/paul/Test-OpenCPN/home/environment_igrib_20260723_0454.grb}"
readonly log="${runtime_root}/config/opencpn.log"
readonly report="${PPM_RESOURCE_IGRIB_REPORT:-${runtime_root}/linux-igrib-workbench-resource-evidence.tsv}"
readonly runs="${PPM_RESOURCE_IGRIB_RUNS:-2}"

usage() {
  cat <<EOF
Usage: tools/runtime-host/measure-linux-igrib-workbench.sh

Launches the isolated stock OpenCPN with all three packages enabled, opens the
full iGRIB workbench through its developer-only production action path and
loads a real GRIB fixture. It records time to dataset readiness, settled
process-tree RSS and the sampled peak including supervised helpers.

Output: ${report}
EOF
}

case "${1:-}" in
  "") ;;
  --help|-h) usage; exit 0 ;;
  *) usage >&2; exit 2 ;;
esac

if [[ ! -x "${launcher}" || ! -f "${fixture}" ]]; then
  printf 'The isolated launcher or GRIB fixture is missing.\n' >&2
  exit 1
fi
if ! [[ "${runs}" =~ ^[1-9][0-9]*$ ]] || (( runs > 10 )); then
  printf 'PPM_RESOURCE_IGRIB_RUNS must be an integer in [1, 10].\n' >&2
  exit 1
fi
if pgrep -af "^${runtime_root}/app/bin/opencpn( |$)" >/dev/null; then
  printf 'The isolated RuntimeHost OpenCPN is already running.\n' >&2
  exit 1
fi

current_pid=""
stop_current() {
  if [[ -n "${current_pid}" ]] && kill -0 "${current_pid}" 2>/dev/null; then
    kill -TERM "${current_pid}" 2>/dev/null || true
    for _ in $(seq 1 50); do
      kill -0 "${current_pid}" 2>/dev/null || break
      sleep 0.1
    done
    if kill -0 "${current_pid}" 2>/dev/null; then
      kill -KILL "${current_pid}" 2>/dev/null || true
    fi
    wait "${current_pid}" 2>/dev/null || true
  fi
  current_pid=""
}
cleanup() {
  stop_current
}
trap cleanup EXIT INT TERM

tree_rss() {
  local root_pid="$1"
  local queue=("${root_pid}")
  local index=0
  local total=0
  local pid rss child
  while (( index < ${#queue[@]} )); do
    pid="${queue[${index}]}"
    index=$((index + 1))
    if [[ -r "/proc/${pid}/status" ]]; then
      rss="$(awk '/^VmRSS:/ { print $2; found=1 }
                  END { if (!found) print 0 }' "/proc/${pid}/status" \
                  2>/dev/null || printf '0\n')"
      total=$((total + rss))
      while IFS= read -r child; do
        [[ -n "${child}" ]] && queue+=("${child}")
      done < <(pgrep -P "${pid}" || true)
    fi
  done
  printf '%s\n' "${total}"
}

mkdir -p "$(dirname "${report}")"
{
  printf '# Generated %s\n' "$(date -Ins)"
  printf '# kernel=%s machine=%s\n' "$(uname -r)" "$(uname -m)"
  printf '# fixture_bytes=%s\n' "$(stat -c '%s' "${fixture}")"
  printf 'run\tready_seconds\tsettled_rss_kib\tpeak_rss_kib\tresult\n'
} >"${report}"

for run in $(seq 1 "${runs}"); do
  log_bytes=0
  [[ -f "${log}" ]] && log_bytes="$(stat -c '%s' "${log}")"
  start_ns="$(date +%s%N)"
  OCPN_PPM_DEVELOPER_STARTUP_ACTION="org.opencpn.igrib:igrib.toggle" \
  OCPN_PPM_IGRIB_SMOKE_FIXTURE="${fixture}" \
    "${launcher}" >/dev/null 2>&1 &
  current_pid="$!"
  peak_rss=0
  deadline=$((SECONDS + 90))
  ready=false
  while kill -0 "${current_pid}" 2>/dev/null; do
    rss="$(tree_rss "${current_pid}")"
    (( rss > peak_rss )) && peak_rss="${rss}"
    if [[ -f "${log}" ]] &&
       tail -c "+$((log_bytes + 1))" "${log}" |
         grep -Fq 'PPM iGRIB workbench dataset-ready'; then
      ready=true
      break
    fi
    if (( SECONDS >= deadline )); then break; fi
    sleep 0.05
  done
  if [[ "${ready}" != true ]]; then
    printf 'iGRIB workbench run %s did not reach dataset readiness.\n' \
      "${run}" >&2
    exit 1
  fi
  ready_ns="$(date +%s%N)"
  settled_rss=0
  for _ in $(seq 1 40); do
    rss="$(tree_rss "${current_pid}")"
    (( rss > peak_rss )) && peak_rss="${rss}"
    settled_rss="${rss}"
    sleep 0.05
  done
  ready_ms=$(((ready_ns - start_ns) / 1000000))
  printf '%s\t%s.%03d\t%s\t%s\tcomplete\n' "${run}" \
    "$((ready_ms / 1000))" "$((ready_ms % 1000))" \
    "${settled_rss}" "${peak_rss}" >>"${report}"
  stop_current
  if pgrep -af '/helpers/[^ ]+/(igrib-environment-helper|environmental-grib)( |$)' \
       >/dev/null; then
    printf 'An environmental helper remained after workbench run %s.\n' \
      "${run}" >&2
    exit 1
  fi
done

printf 'Linux iGRIB workbench resource evidence written to %s\n' "${report}"
