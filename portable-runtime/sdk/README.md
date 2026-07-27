# OpenCPN Portable Plugin author kit (OPP API 0.5)

This directory is the plugin-author boundary. A portable plugin needs a Rust
toolchain with the `wasm32-wasip2` target; it does not need the OpenCPN source
tree, wxWidgets, a platform compiler, or direct access to OpenCPN internals.

Create a self-contained project (including a pinned copy of the WIT contract)
and build it with:

```sh
python3 tools/portable_plugin.py new \
  --id org.example.my-plugin --name "My plugin" my-plugin
rustup target add wasm32-wasip2
cargo build --locked --release --target wasm32-wasip2 \
  --manifest-path my-plugin/Cargo.toml
python3 tools/portable_plugin.py lint my-plugin/package/manifest.json
python3 tools/portable_plugin.py package \
  --manifest my-plugin/package/manifest.json \
  --component my-plugin/target/wasm32-wasip2/release/portable_plugin_template.wasm \
  --output my-plugin.ocpnp
```

The host grants only manifest-declared capabilities. Native pointers, raw
sockets, the OpenCPN object model and renderer APIs are intentionally absent.
Navigation mutations always require user confirmation. NMEA output is
validated and rate-limited by the host.

The generated project demonstrates a toolbar and chart-context action,
host-owned declarative surface, persistent setting and atomic private value,
retained interactive scene, timer and typed RPC service. The frozen
[`API-0.3.md`](API-0.3.md) describes the preceding compatibility profile;
[`OPP-API-0.5.md`](OPP-API-0.5.md) is the current universal author guide and
the authoritative WIT source is `contracts/0.5`. The frozen
[`OPP-API-0.4.md`](OPP-API-0.4.md) remains available for compatibility.

`ppm_portable_fake_host` is a deterministic, headless local runner built by
the manager's CMake test configuration. It executes the OPP API 0.5 template
through the production Wasmtime bridge and checks lifecycle, action, atomic
storage, scene, timer and service-registration calls:

```sh
ctest --test-dir BUILD_DIR -R ppm_opp_api_v05_fake_host
```

The fake host does not claim chart or navigation-safety parity with OpenCPN;
integration tests must still run against the stock host plugin. Raw sockets,
native pointers and direct renderer APIs remain intentionally unavailable.

`portable_plugin.py package` creates a deterministic **development** archive
with checksums. It does not hold or invent a publisher identity. Catalogue
publication must use the repository's separate signing and multi-target
release workflow.
