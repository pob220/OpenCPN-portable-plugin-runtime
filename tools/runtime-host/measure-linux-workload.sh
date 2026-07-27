#!/usr/bin/env bash
set -euo pipefail

readonly runtime_root="${OCPN_RUNTIME_HOST_ROOT:-/home/paul/RuntimeHost-OpenCPN}"
readonly probe="${runtime_root}/build-ppm/ppm_routing_probe"
readonly store="${runtime_root}/config/portable-runtime"
readonly fixture="${PPM_RESOURCE_GRIB_FIXTURE:-/home/paul/Test-OpenCPN/home/environment_igrib_20260723_0454.grb}"
readonly polar="${PPM_RESOURCE_POLAR:-${store}/packages/org.opencpn.iweather-routing/resources/Nicholson35_Mk1_cruising_realistic.pol}"
readonly cm93_root="${PPM_RESOURCE_CM93_ROOT:-/home/paul/Charts/cm93_2015}"
readonly departure_unix="${PPM_RESOURCE_DEPARTURE_UNIX:-1784800800}"
readonly report="${PPM_RESOURCE_WORKLOAD_REPORT:-${runtime_root}/linux-workload-resource-evidence.tsv}"
readonly runs="${PPM_RESOURCE_WORKLOAD_RUNS:-2}"

usage() {
  cat <<EOF
Usage: tools/runtime-host/measure-linux-workload.sh

Runs the installed-package Holyhead-to-Dun-Laoghaire route probe and
records elapsed time and sampled process-tree maximum resident size. The probe loads
the signed iGRIB/iWeatherRouting packages, the real environmental fixture and
the bundled cruising polar, and uses the authoritative CM93 chart snapshot.

Environment overrides:
  PPM_RESOURCE_WORKLOAD_RUNS       Number of runs (default: ${runs})
  PPM_RESOURCE_GRIB_FIXTURE       GRIB input (default: ${fixture})
  PPM_RESOURCE_POLAR              Polar input (default: ${polar})
  PPM_RESOURCE_CM93_ROOT          CM93 chart root (default: ${cm93_root})
  PPM_RESOURCE_DEPARTURE_UNIX     UTC departure timestamp (default: ${departure_unix})

Output: ${report}
EOF
}

case "${1:-}" in
  "") ;;
  --help|-h) usage; exit 0 ;;
  *) usage >&2; exit 2 ;;
esac

if [[ ! -x "${probe}" || ! -f "${fixture}" || ! -f "${polar}" ||
      ! -d "${cm93_root}" ]]; then
  printf 'The route probe, GRIB fixture, polar or CM93 root is missing.\n' >&2
  exit 1
fi
if ! [[ "${departure_unix}" =~ ^[0-9]+$ ]]; then
  printf 'PPM_RESOURCE_DEPARTURE_UNIX must be a Unix timestamp.\n' >&2
  exit 1
fi
if ! [[ "${runs}" =~ ^[1-9][0-9]*$ ]] || (( runs > 10 )); then
  printf 'PPM_RESOURCE_WORKLOAD_RUNS must be an integer in [1, 10].\n' >&2
  exit 1
fi
if pgrep -af "^${runtime_root}/app/bin/opencpn( |$)" >/dev/null; then
  printf 'The isolated RuntimeHost OpenCPN is already running.\n' >&2
  exit 1
fi
mkdir -p "$(dirname "${report}")"

{
  printf '# Generated %s\n' "$(date -Ins)"
  printf '# kernel=%s machine=%s\n' "$(uname -r)" "$(uname -m)"
  printf '# fixture_bytes=%s\n' "$(stat -c '%s' "${fixture}")"
  printf '# fixture_sha256=%s\n' "$(sha256sum "${fixture}" | cut -d' ' -f1)"
  printf '# departure_unix=%s\n' "${departure_unix}"
  printf '# cm93_root=%s\n' "${cm93_root}"
  printf 'run\telapsed_seconds\tmax_rss_kib\tresult\n'
} >"${report}"

for run in $(seq 1 "${runs}"); do
  output="${runtime_root}/route-resource-run-${run}.log"
  start_ns="$(date +%s%N)"
  PPM_TEST_CM93_ROOT="${cm93_root}" stdbuf -oL -eL \
    "${probe}" "${store}" "${fixture}" "${polar}" \
    "${departure_unix}" 53.336985 -4.607470 53.308408 -6.119702 \
    >"${output}" 2>&1 &
  probe_pid="$!"
  peak_rss=0
  while kill -0 "${probe_pid}" 2>/dev/null; do
    queue=("${probe_pid}")
    total_rss=0
    index=0
    while (( index < ${#queue[@]} )); do
      pid="${queue[${index}]}"
      index=$((index + 1))
      if [[ -r "/proc/${pid}/status" ]]; then
        # A helper can exit after the readability check but before awk opens
        # the file.  Treat that normal /proc race as a zero-sized sample.
        rss="$(awk '/^VmRSS:/ { print $2; found=1 }
                    END { if (!found) print 0 }' "/proc/${pid}/status" \
                    2>/dev/null || printf '0\n')"
        total_rss=$((total_rss + rss))
        while IFS= read -r child; do
          if [[ -n "${child}" ]]; then
            queue+=("${child}")
          fi
        done < <(pgrep -P "${pid}" || true)
      fi
    done
    if (( total_rss > peak_rss )); then
      peak_rss="${total_rss}"
    fi
    sleep 0.05
  done
  if ! wait "${probe_pid}"; then
    printf 'Route workload run %s failed; see %s.\n' "${run}" "${output}" >&2
    exit 1
  fi
  end_ns="$(date +%s%N)"
  elapsed_ms=$(((end_ns - start_ns) / 1000000))
  if ! grep -Fq 'route=complete ' "${output}" ||
     ! grep -Fq 'Authoritative final chart/depth corridor validation' "${output}" ||
     ! grep -Fq 'independently validated' "${output}"; then
    printf 'Route workload run %s did not produce the expected validated route.\n' \
      "${run}" >&2
    exit 1
  fi
  printf '%s\t%s.%03d\t%s\tcomplete\n' "${run}" \
    "$((elapsed_ms / 1000))" "$((elapsed_ms % 1000))" "${peak_rss}" \
    >>"${report}"
done

printf 'Linux workload resource evidence written to %s\n' "${report}"
