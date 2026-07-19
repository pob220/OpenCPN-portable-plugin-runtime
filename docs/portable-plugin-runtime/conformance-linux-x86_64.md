# iGRIB conformance report: Linux x86-64

Date: 2026-07-19

Host: Linux 6.18.37-1-lts, x86-64, glibc 2.43. OpenCPN: isolated
Test-OpenCPN 5.14.0 experimental build. Target identifier:
`linux-gnu-x86_64`.

## Automated package/helper run

Command:

```text
python3 portable-runtime/tests/conformance.py \
  --package <build>/org.opencpn.igrib-0.1.0.ocpnp \
  --trusted-key portable-runtime/development-keys/igrib-ed25519-public.pem \
  --fixture <44.8-MB-environment-fixture.grb> --full-generator
```

| Check | Result | Observation |
|---|---:|---|
| signed package verification/fresh install | pass | development Ed25519 key; complete digest inventory |
| exact target helper selection | pass | `linux-gnu-x86_64` |
| malformed input containment | pass | 4.076 ms; structured failure |
| generator protocol negotiation | pass | schema 1; 9.586 ms |
| real fixture inspection | pass | 186.103 ms; 44,821,471 bytes; 387 messages; 67 times |
| first frame decode | pass | 185.944 ms; 5,960 retained samples |
| real generator output | pass | 283.951 ms; validated 44,821,471-byte output using existing-file provider |

These timings are one conformance observation, not statistically useful
benchmarks. Run the repetition/percentile plan in `performance-plan.md` before
choosing service budgets.

## Live Test-OpenCPN run

- iGRIB loaded and opened while native xGRIB was absent from the plugin search
  path.
- The accessible UI exposed file/timeline/playback, five environmental layer
  toggles, settings, download, generation, progress and cancellation.
- The 44.8 MB fixture opened through the native picker and rendered the first
  retained frame; playback advanced to the next forecast time.
- One batched request evaluated four chart-coverage segments.
- The host HTTP test fetched 7,655 bytes and read them through plugin-private
  storage.
- A live minimal GFS job downloaded forecast data under bubblewrap containment,
  published `/tmp/igrib-gui-final.grb` (1,246 bytes, six GRIB messages, two
  forecast times), reopened it and retained 75 samples for display.
- A component trap and a malformed decoder input were contained. OpenCPN
  remained responsive and subsequently completed normal operations.

## Platform status

Only Linux x86-64 passed in this report. Windows x86-64, macOS x86-64,
macOS ARM64, Linux ARM64/Raspberry Pi, Flatpak x86-64 and Flatpak ARM64 are
`untested`, not inferred passes. Each target must run the same script with its
target helper payload and complete the GUI/process-sandbox checklist before it
can be marked supported.
