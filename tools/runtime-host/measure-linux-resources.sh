#!/usr/bin/env bash
set -euo pipefail

readonly runtime_root="${OCPN_RUNTIME_HOST_ROOT:-/home/paul/RuntimeHost-OpenCPN}"
readonly script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly application="${runtime_root}/app/bin/opencpn"
readonly cli="${runtime_root}/build-ppm/ppm_package_cli"
readonly store="${runtime_root}/config/portable-runtime"
readonly trust="${runtime_root}/config/share/opencpn/plugins/portable_plugin_manager_pi/trust"
readonly plugin_dir="${runtime_root}/config/plugins/lib"
readonly log_file="${runtime_root}/config/opencpn.log"
readonly report="${PPM_RESOURCE_REPORT:-${runtime_root}/linux-resource-evidence.tsv}"
readonly measurement_temp="$(mktemp -d /tmp/ppm-resources-XXXXXX)"
readonly empty_plugin_dir="${measurement_temp}/empty-plugins"

usage() {
  cat <<EOF
Usage: tools/runtime-host/measure-linux-resources.sh

Measures stock OpenCPN, the idle Manager, and cumulative enabled-package
startup/RSS in the isolated RuntimeHost profile. Each case runs twice so the
report distinguishes first and warm filesystem-cache launches.

Output: ${report}
EOF
}

case "${1:-}" in
  "") ;;
  --help|-h) usage; exit 0 ;;
  *) usage >&2; exit 2 ;;
esac

for required in "${application}" "${cli}" "${script_dir}/launch-linux.sh"; do
  if [[ ! -x "${required}" ]]; then
    printf 'Required executable is missing: %s\n' "${required}" >&2
    exit 1
  fi
done
if pgrep -af "^${application}( |$)" >/dev/null; then
  printf 'The isolated RuntimeHost OpenCPN is already running.\n' >&2
  exit 1
fi
mkdir -p "${empty_plugin_dir}" "$(dirname "${report}")"

package_ids=(
  org.opencpn.igrib
  org.opencpn.iweather-routing
  org.opencpn.ipolars
)
declare -A original_state=()
while IFS=$'\t' read -r package_id _version state _approval; do
  original_state["${package_id}"]="${state}"
done < <("${cli}" --root "${store}" --trust "${trust}" --developer list)

current_pid=""
restore() {
  if [[ -n "${current_pid}" ]] && kill -0 "${current_pid}" 2>/dev/null; then
    kill -INT "${current_pid}" 2>/dev/null || true
    wait "${current_pid}" 2>/dev/null || true
  fi
  current_pid=""
  for package_id in "${package_ids[@]}"; do
    if [[ "${original_state[${package_id}]:-disabled}" == "enabled" ]]; then
      "${cli}" --root "${store}" --trust "${trust}" --developer \
        state "${package_id}" on >/dev/null || true
    else
      "${cli}" --root "${store}" --trust "${trust}" --developer \
        state "${package_id}" off >/dev/null || true
    fi
  done
  cmake -E remove_directory "${measurement_temp}"
}
trap restore EXIT INT TERM

set_packages() {
  local igrib="$1"
  local routing="$2"
  local polars="$3"
  # Disable consumers first, then their provider.
  "${cli}" --root "${store}" --trust "${trust}" --developer \
    state org.opencpn.iweather-routing off >/dev/null
  "${cli}" --root "${store}" --trust "${trust}" --developer \
    state org.opencpn.ipolars off >/dev/null
  "${cli}" --root "${store}" --trust "${trust}" --developer \
    state org.opencpn.igrib off >/dev/null
  if [[ "${igrib}" == 1 ]]; then
    "${cli}" --root "${store}" --trust "${trust}" --developer \
      state org.opencpn.igrib on >/dev/null
  fi
  if [[ "${routing}" == 1 ]]; then
    "${cli}" --root "${store}" --trust "${trust}" --developer \
      state org.opencpn.iweather-routing on >/dev/null
  fi
  if [[ "${polars}" == 1 ]]; then
    "${cli}" --root "${store}" --trust "${trust}" --developer \
      state org.opencpn.ipolars on >/dev/null
  fi
}

rss_kib() {
  local pid="$1"
  awk '/^VmRSS:/ { print $2; found=1 } END { if (!found) print 0 }' \
    "/proc/${pid}/status" 2>/dev/null || printf '0\n'
}

measure_launch() {
  local label="$1"
  local run="$2"
  local selected_plugin_dir="$3"
  local expected_log="$4"
  local forbidden_log="$5"
  local before=0
  if [[ -f "${log_file}" ]]; then
    before="$(stat -c '%s' "${log_file}")"
  fi
  local start_ns
  start_ns="$(date +%s%N)"
  local output="${measurement_temp}/${label}-${run}.output"
  OCPN_RUNTIME_HOST_PLUGIN_DIRS="${selected_plugin_dir}" \
    "${script_dir}/launch-linux.sh" >"${output}" 2>&1 &
  current_pid="$!"

  local peak=0
  local idle=0
  local initialized_ns=0
  local settled_samples=0
  for _attempt in $(seq 1 400); do
    if ! kill -0 "${current_pid}" 2>/dev/null; then
      printf 'OpenCPN exited before resource measurement completed (%s).\n' \
        "${label}" >&2
      return 1
    fi
    local current
    current="$(rss_kib "${current_pid}")"
    if (( current > peak )); then
      peak="${current}"
    fi
    if (( initialized_ns == 0 )) &&
       tail -c "+$((before + 1))" "${log_file}" 2>/dev/null |
         grep -Fq 'OpenCPN Initialized'; then
      initialized_ns="$(date +%s%N)"
    fi
    if (( initialized_ns != 0 )); then
      idle="${current}"
      settled_samples=$((settled_samples + 1))
      if (( settled_samples >= 20 )); then
        break
      fi
    fi
    sleep 0.1
  done
  if (( initialized_ns == 0 )); then
    printf 'OpenCPN did not initialize within 40 seconds (%s).\n' \
      "${label}" >&2
    return 1
  fi
  local new_log
  new_log="$(tail -c "+$((before + 1))" "${log_file}")"
  if [[ -n "${expected_log}" ]] &&
     ! grep -Fq "${expected_log}" <<<"${new_log}"; then
    printf 'Expected startup evidence is missing for %s: %s\n' \
      "${label}" "${expected_log}" >&2
    return 1
  fi
  if [[ -n "${forbidden_log}" ]] &&
     grep -Fq "${forbidden_log}" <<<"${new_log}"; then
    printf 'Unexpected startup evidence was present for %s: %s\n' \
      "${label}" "${forbidden_log}" >&2
    return 1
  fi
  local startup_ms=$(((initialized_ns - start_ns) / 1000000))
  printf '%s\t%s\t%s\t%s\t%s\n' \
    "${label}" "${run}" "${startup_ms}" "${idle}" "${peak}" >>"${report}"

  kill -INT "${current_pid}" 2>/dev/null || true
  for _attempt in $(seq 1 50); do
    if ! kill -0 "${current_pid}" 2>/dev/null; then
      break
    fi
    sleep 0.1
  done
  if kill -0 "${current_pid}" 2>/dev/null; then
    kill -TERM "${current_pid}" 2>/dev/null || true
  fi
  wait "${current_pid}" 2>/dev/null || true
  current_pid=""
}

{
  printf '# Generated %s\n' "$(date -Ins)"
  printf '# kernel=%s machine=%s\n' "$(uname -r)" "$(uname -m)"
  printf '# manager_module_bytes=%s\n' \
    "$(stat -c '%s' "${plugin_dir}/libportable_plugin_manager_pi.so")"
  for package_id in "${package_ids[@]}"; do
    printf '# installed_%s_bytes=%s\n' "${package_id}" \
      "$(du -sb "${store}/packages/${package_id}" | cut -f1)"
  done
  printf 'case\trun\tstartup_ms\tidle_rss_kib\tstartup_peak_rss_kib\n'
} >"${report}"

set_packages 0 0 0
measure_launch stock 1 "${empty_plugin_dir}" '' 'PPM event=init'
measure_launch stock 2 "${empty_plugin_dir}" '' 'PPM event=init'
measure_launch manager-only 1 "${plugin_dir}" 'PPM event=init' \
  'PPM package-enabled'
measure_launch manager-only 2 "${plugin_dir}" 'PPM event=init' \
  'PPM package-enabled'

set_packages 1 0 0
measure_launch igrib 1 "${plugin_dir}" \
  'PPM package-enabled id=org.opencpn.igrib' \
  'PPM package-enabled id=org.opencpn.iweather-routing'
measure_launch igrib 2 "${plugin_dir}" \
  'PPM package-enabled id=org.opencpn.igrib' \
  'PPM package-enabled id=org.opencpn.iweather-routing'

set_packages 1 1 0
measure_launch igrib-routing 1 "${plugin_dir}" \
  'PPM package-enabled id=org.opencpn.iweather-routing' \
  'PPM package-enabled id=org.opencpn.ipolars'
measure_launch igrib-routing 2 "${plugin_dir}" \
  'PPM package-enabled id=org.opencpn.iweather-routing' \
  'PPM package-enabled id=org.opencpn.ipolars'

set_packages 1 1 1
measure_launch all-packages 1 "${plugin_dir}" \
  'PPM package-enabled id=org.opencpn.ipolars' ''
measure_launch all-packages 2 "${plugin_dir}" \
  'PPM package-enabled id=org.opencpn.ipolars' ''

printf 'Linux resource evidence written to %s\n' "${report}"
