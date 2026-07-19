# Experimental OpenCPN portable-plugin runtime.
#
# This is included only when OCPN_ENABLE_PORTABLE_PLUGINS is enabled.  Rust is
# therefore not a dependency of stock OpenCPN builds.

set(OCPN_PORTABLE_CARGO "cargo" CACHE FILEPATH
    "Path to cargo used for the experimental portable runtime")
set(OCPN_PORTABLE_RUSTUP_HOME "" CACHE PATH
    "Optional RUSTUP_HOME for the experimental portable runtime")
set(OCPN_PORTABLE_CARGO_HOME "" CACHE PATH
    "Optional CARGO_HOME for the experimental portable runtime")
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(_portable_env
    "CARGO_TARGET_DIR=${CMAKE_CURRENT_BINARY_DIR}/cargo-target")
if (OCPN_PORTABLE_RUSTUP_HOME)
  list(APPEND _portable_env "RUSTUP_HOME=${OCPN_PORTABLE_RUSTUP_HOME}")
endif ()
if (OCPN_PORTABLE_CARGO_HOME)
  list(APPEND _portable_env "CARGO_HOME=${OCPN_PORTABLE_CARGO_HOME}")
endif ()

if (WIN32)
  set(_bridge_library
      "${CMAKE_CURRENT_BINARY_DIR}/cargo-target/release/ocpn_portable_runtime_bridge.lib")
else ()
  set(_bridge_library
      "${CMAKE_CURRENT_BINARY_DIR}/cargo-target/release/libocpn_portable_runtime_bridge.a")
endif ()

file(GLOB_RECURSE _bridge_sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/bridge/src/*.rs"
     "${CMAKE_CURRENT_SOURCE_DIR}/wit/*.wit")
add_custom_command(
  OUTPUT "${_bridge_library}"
  COMMAND ${CMAKE_COMMAND} -E env ${_portable_env}
          ${OCPN_PORTABLE_CARGO} build --locked --offline --release
          --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/bridge/Cargo.toml"
  DEPENDS ${_bridge_sources}
          "${CMAKE_CURRENT_SOURCE_DIR}/bridge/Cargo.toml"
          "${CMAKE_CURRENT_SOURCE_DIR}/bridge/Cargo.lock"
  COMMENT "Building the experimental Wasmtime portable-plugin bridge"
  VERBATIM
)
add_custom_target(ocpn_portable_runtime_bridge_build
                  DEPENDS "${_bridge_library}")
add_library(ocpn_portable_runtime_bridge STATIC IMPORTED GLOBAL)
set_target_properties(ocpn_portable_runtime_bridge PROPERTIES
                      IMPORTED_LOCATION "${_bridge_library}")
add_dependencies(ocpn_portable_runtime_bridge
                 ocpn_portable_runtime_bridge_build)

set(_igrib_source "${CMAKE_SOURCE_DIR}/portable-plugins/igrib")
set(_igrib_target_dir "${CMAKE_CURRENT_BINARY_DIR}/igrib-cargo-target")
set(_igrib_package "${CMAKE_CURRENT_BINARY_DIR}/packages/igrib")
set(_igrib_component "${_igrib_package}/component/igrib.wasm")
set(_igrib_package_stamp "${CMAKE_CURRENT_BINARY_DIR}/packages/igrib.assembled")
file(GLOB_RECURSE _igrib_sources CONFIGURE_DEPENDS
     "${_igrib_source}/component/src/*.rs")
add_custom_command(
  OUTPUT "${_igrib_package_stamp}"
  BYPRODUCTS "${_igrib_component}"
  COMMAND ${CMAKE_COMMAND} -E env
          ${_portable_env}
          "CARGO_TARGET_DIR=${_igrib_target_dir}"
          ${OCPN_PORTABLE_CARGO} build --locked --offline --release
          --target wasm32-wasip2
          --manifest-path "${_igrib_source}/component/Cargo.toml"
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "${_igrib_package}/component" "${_igrib_package}/resources"
          "${_igrib_package}/interfaces" "${_igrib_package}/licenses"
  COMMAND ${CMAKE_COMMAND} -E rm -f "${_igrib_package}/.assembled"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_igrib_target_dir}/wasm32-wasip2/release/igrib_portable_component.wasm"
          "${_igrib_component}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_igrib_source}/package/manifest.json"
          "${_igrib_package}/manifest.json"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/plugins/grib_pi/data/grib.svg"
          "${_igrib_package}/resources/grib.svg"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/wit/opencpn-portable.wit"
          "${_igrib_package}/interfaces/opencpn-portable.wit"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/COPYING.gplv2"
          "${_igrib_package}/licenses/GPL-2.0.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_igrib_source}/README.md"
          "${_igrib_package}/README.md"
  COMMAND ${CMAKE_COMMAND} -E touch "${_igrib_package_stamp}"
  DEPENDS ${_igrib_sources}
          "${_igrib_source}/component/Cargo.toml"
          "${_igrib_source}/component/Cargo.lock"
          "${_igrib_source}/package/manifest.json"
          "${CMAKE_SOURCE_DIR}/plugins/grib_pi/data/grib.svg"
          "${CMAKE_SOURCE_DIR}/COPYING.gplv2"
          "${_igrib_source}/README.md"
          "${CMAKE_CURRENT_SOURCE_DIR}/wit/opencpn-portable.wit"
  COMMENT "Building and assembling the portable iGRIB package"
  VERBATIM
)
set(_igrib_archive
    "${CMAKE_CURRENT_BINARY_DIR}/packages/org.opencpn.igrib-0.1.0.ocpnp")
add_custom_command(
  OUTPUT "${_igrib_archive}"
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_package.py"
          --root "${_igrib_package}" --output "${_igrib_archive}"
  DEPENDS "${_igrib_package_stamp}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_package.py"
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  COMMENT "Creating deterministic iGRIB .ocpnp package"
  VERBATIM
)
add_custom_target(portable_igrib_package ALL
                  DEPENDS "${_igrib_package_stamp}" "${_igrib_archive}")
set(OCPN_PORTABLE_IGRIB_PACKAGE "${_igrib_package}" CACHE INTERNAL
    "Build-tree iGRIB portable package")
set(OCPN_PORTABLE_IGRIB_ARCHIVE "${_igrib_archive}" CACHE INTERNAL
    "Build-tree iGRIB portable package archive")

if (OCPN_BUILD_TEST)
  find_package(Threads REQUIRED)
  add_executable(portable_runtime_smoke_test tests/bridge_smoke.cpp)
  target_compile_features(portable_runtime_smoke_test PRIVATE cxx_std_17)
  target_include_directories(
    portable_runtime_smoke_test
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/bridge/include"
  )
  target_link_libraries(
    portable_runtime_smoke_test
    PRIVATE ocpn_portable_runtime_bridge Threads::Threads ${CMAKE_DL_LIBS}
  )
  if (UNIX AND NOT APPLE)
    target_link_libraries(portable_runtime_smoke_test PRIVATE m)
  endif ()
  add_dependencies(portable_runtime_smoke_test portable_igrib_package)
  add_test(
    NAME portable_runtime_smoke
    COMMAND portable_runtime_smoke_test "${_igrib_component}"
  )
  add_test(
    NAME portable_package_tools
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_tools_test.py"
  )
endif ()
