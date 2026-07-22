# Source ledger

Access date for the initial ledger: 2026-07-22.

| Source | Revision | Role |
|---|---|---|
| Local portable-runtime study base | `f85835669f97e3d913e4b26a531e47ae1f061b00` | Modified-core reference and study branch base |
| OpenCPN 5.14.0 release | `91f3b674366068a6ecd61a5e9aba204bba85f57e` | Stock baseline for core-change inventory and compatibility experiments |
| OpenCPN upstream `master` | `bc0e1ededbb10cd44feffc26b7989b6b990fa6eb` | Current public API and implementation comparison |
| Portable repository remote branch | `d9bd9b8b9999f12473b8a849bebd628a9e13fa73` | Remote-state record; deliberately not used as the study base because it trails local work |
| Environmental GRIB generator | `f7311bb340f6c942080b5ad18061c1a8b6b5f000` | Native-helper and environmental-service implementation |

## Local platform

- OpenCPN source version: `Release_5.14.0-44-gf85835669`
- Public plugin API in study base: 1.21
- Host: Linux x86-64, kernel 6.18.37-1-lts
- Compiler: GCC 16.1.1
- CMake: 4.3.4
- Python: 3.14.6
- Rust: 1.97.1 (`8bab26f4f`, repository-local stable toolchain)
- Cargo: 1.97.1 (`c980f4866`, repository-local stable toolchain)

Further plugin repositories, official documentation and platform sources are
added only after their authoritative branch and commit have been verified.
