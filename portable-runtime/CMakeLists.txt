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
file(GLOB_RECURSE _portable_api_v05_sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/contracts/0.5/*.wit")

option(OCPN_IGRIB_BUILD_NATIVE_HELPER
       "Build the experimental ecCodes iGRIB decode helper" ON)
option(OCPN_IGRIB_BUILD_GENERATOR_HELPER
       "Build the shared environmental GRIB generator sidecar" ON)
set(OCPN_IGRIB_GENERATOR_HELPER "" CACHE FILEPATH
    "Optional prebuilt environmental-grib sidecar (overrides the shared source build)")

if (OCPN_IGRIB_BUILD_NATIVE_HELPER)
  if (WIN32)
    find_package(eccodes CONFIG REQUIRED)
    find_package(jsoncpp CONFIG REQUIRED)
    set(_igrib_decoder_libraries eccodes JsonCpp::JsonCpp)
  else ()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(IGRIB_ECCODES REQUIRED IMPORTED_TARGET eccodes)
    # Ubuntu multiarch packages can publish an eccodes.pc includedir relative
    # to the pkg-config directory (for example /usr/lib/include).  Resolve the
    # installed public header and reject the dependency if it is not real.
    find_path(IGRIB_ECCODES_HEADER_DIRECTORY NAMES eccodes.h
              HINTS ${IGRIB_ECCODES_INCLUDE_DIRS})
    if (NOT IGRIB_ECCODES_HEADER_DIRECTORY)
      message(FATAL_ERROR "Could not locate the ecCodes public headers")
    endif ()
    set_property(TARGET PkgConfig::IGRIB_ECCODES PROPERTY
                 INTERFACE_INCLUDE_DIRECTORIES
                 "${IGRIB_ECCODES_HEADER_DIRECTORY}")
    pkg_check_modules(IGRIB_JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)
    set(_igrib_decoder_libraries PkgConfig::IGRIB_ECCODES
                                 PkgConfig::IGRIB_JSONCPP)
  endif ()
  add_executable(igrib_environment_helper
                 helper/igrib_environment_helper.cpp)
  target_compile_features(igrib_environment_helper PRIVATE cxx_std_17)
  if (MSVC)
    target_compile_options(igrib_environment_helper PRIVATE /W4)
  else ()
    target_compile_options(igrib_environment_helper PRIVATE
                           -Wall -Wextra -Wpedantic)
  endif ()
  target_link_libraries(igrib_environment_helper PRIVATE
                        ${_igrib_decoder_libraries})
endif ()

# This is deliberately a separately maintained helper process rather than
# provider code linked into OpenCPN.  Building the exact vendored revision as
# part of the portable-runtime graph makes packages reproducible and prevents
# a stale executable elsewhere on a developer machine being bundled by
# accident.  A prebuilt helper remains available for cross-compilation jobs.
if (OCPN_IGRIB_BUILD_GENERATOR_HELPER AND NOT OCPN_IGRIB_GENERATOR_HELPER)
  add_subdirectory(vendor/environmental-grib-generator
                   environmental-grib-generator EXCLUDE_FROM_ALL)
endif ()

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
  set(_igrib_executable_suffix ".exe")
else ()
  set(_bridge_library
      "${CMAKE_CURRENT_BINARY_DIR}/cargo-target/release/libocpn_portable_runtime_bridge.a")
  set(_igrib_executable_suffix "")
endif ()

file(GLOB_RECURSE _bridge_sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/bridge/src/*.rs"
     "${CMAKE_CURRENT_SOURCE_DIR}/wit/*.wit"
     "${CMAKE_CURRENT_SOURCE_DIR}/contracts/*.wit")
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
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
  if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(_igrib_helper_target "linux-gnu-aarch64")
  elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(_igrib_helper_target "linux-gnu-x86_64")
  else ()
    message(FATAL_ERROR
            "Unsupported iGRIB helper architecture: ${CMAKE_SYSTEM_PROCESSOR}")
  endif ()
elseif (APPLE)
  if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(_igrib_helper_target "macos-aarch64")
  else ()
    set(_igrib_helper_target "macos-x86_64")
  endif ()
elseif (WIN32)
  set(_igrib_helper_target "windows-x86_64")
else ()
  message(FATAL_ERROR "Unsupported iGRIB helper platform: ${CMAKE_SYSTEM_NAME}")
endif ()
set(_igrib_helpers
    "${_igrib_package}/helpers/${_igrib_helper_target}")
set(_igrib_decoder_copy "")
if (OCPN_IGRIB_BUILD_NATIVE_HELPER)
  set(_igrib_decoder_copy
      "${_igrib_helpers}/igrib-environment-helper${_igrib_executable_suffix}")
  add_custom_command(
    OUTPUT "${_igrib_decoder_copy}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_igrib_helpers}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:igrib_environment_helper>" "${_igrib_decoder_copy}"
    DEPENDS igrib_environment_helper
    COMMENT "Adding the ecCodes decoder sidecar to iGRIB"
    VERBATIM
  )
endif ()
set(_igrib_generator_copy "")
if (OCPN_IGRIB_GENERATOR_HELPER)
  if (NOT EXISTS "${OCPN_IGRIB_GENERATOR_HELPER}")
    message(FATAL_ERROR
            "OCPN_IGRIB_GENERATOR_HELPER does not exist: ${OCPN_IGRIB_GENERATOR_HELPER}")
  endif ()
  set(_igrib_generator_copy
      "${_igrib_helpers}/environmental-grib${_igrib_executable_suffix}")
  add_custom_command(
    OUTPUT "${_igrib_generator_copy}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_igrib_helpers}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OCPN_IGRIB_GENERATOR_HELPER}" "${_igrib_generator_copy}"
    DEPENDS "${OCPN_IGRIB_GENERATOR_HELPER}"
    COMMENT "Adding the environmental generator sidecar to iGRIB"
    VERBATIM
  )
elseif (OCPN_IGRIB_BUILD_GENERATOR_HELPER)
  set(_igrib_generator_copy
      "${_igrib_helpers}/environmental-grib${_igrib_executable_suffix}")
  add_custom_command(
    OUTPUT "${_igrib_generator_copy}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_igrib_helpers}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:environmental-grib>" "${_igrib_generator_copy}"
    DEPENDS environmental-grib
    COMMENT "Adding the shared environmental generator sidecar to iGRIB"
    VERBATIM
  )
endif ()
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
          "${_igrib_package}/resources/controls"
          "${_igrib_package}/interfaces" "${_igrib_package}/licenses"
          "${_igrib_package}/ui"
          "${_igrib_helpers}"
  COMMAND ${CMAKE_COMMAND} -E rm -f "${_igrib_package}/.assembled"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_igrib_target_dir}/wasm32-wasip2/release/igrib_portable_component.wasm"
          "${_igrib_component}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_igrib_source}/package/manifest.json"
          "${_igrib_package}/manifest.json"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_igrib_source}/package/igrib.svg"
          "${_igrib_package}/resources/igrib.svg"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/plugins/grib_pi/data/prev.svg"
          "${_igrib_package}/resources/controls/prev.svg"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/plugins/grib_pi/data/next.svg"
          "${_igrib_package}/resources/controls/next.svg"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/plugins/grib_pi/data/play.svg"
          "${_igrib_package}/resources/controls/play.svg"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/plugins/grib_pi/data/stop.svg"
          "${_igrib_package}/resources/controls/stop.svg"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/plugins/grib_pi/data/now.svg"
          "${_igrib_package}/resources/controls/now.svg"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/plugins/grib_pi/data/openfile.svg"
          "${_igrib_package}/resources/controls/openfile.svg"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/plugins/grib_pi/data/setting.svg"
          "${_igrib_package}/resources/controls/setting.svg"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/plugins/grib_pi/data/curdata.svg"
          "${_igrib_package}/resources/controls/curdata.svg"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/plugins/grib_pi/data/request.svg"
          "${_igrib_package}/resources/controls/request.svg"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/plugins/grib_pi/data/ncurdata.svg"
          "${_igrib_package}/resources/controls/ncurdata.svg"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/data/svg/markicons/Hazard-Warning.svg"
          "${_igrib_package}/resources/fault-test.svg"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/data/svg/traditional/emblem-download.svg"
          "${_igrib_package}/resources/http-download.svg"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_igrib_source}/package/igrib-viewer.ui.json"
          "${_igrib_package}/ui/igrib-viewer.ui.json"
  COMMAND ${CMAKE_COMMAND} -E remove_directory
          "${_igrib_package}/interfaces"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/contracts/0.5"
          "${_igrib_package}/interfaces/opencpn-opp-0.5"
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
          "${_igrib_source}/package/igrib-viewer.ui.json"
          "${_igrib_source}/package/igrib.svg"
          "${CMAKE_SOURCE_DIR}/plugins/grib_pi/data/prev.svg"
          "${CMAKE_SOURCE_DIR}/plugins/grib_pi/data/next.svg"
          "${CMAKE_SOURCE_DIR}/plugins/grib_pi/data/play.svg"
          "${CMAKE_SOURCE_DIR}/plugins/grib_pi/data/stop.svg"
          "${CMAKE_SOURCE_DIR}/plugins/grib_pi/data/now.svg"
          "${CMAKE_SOURCE_DIR}/plugins/grib_pi/data/openfile.svg"
          "${CMAKE_SOURCE_DIR}/plugins/grib_pi/data/setting.svg"
          "${CMAKE_SOURCE_DIR}/plugins/grib_pi/data/curdata.svg"
          "${CMAKE_SOURCE_DIR}/plugins/grib_pi/data/request.svg"
          "${CMAKE_SOURCE_DIR}/plugins/grib_pi/data/ncurdata.svg"
          "${CMAKE_SOURCE_DIR}/data/svg/markicons/Hazard-Warning.svg"
          "${CMAKE_SOURCE_DIR}/data/svg/traditional/emblem-download.svg"
          "${CMAKE_SOURCE_DIR}/COPYING.gplv2"
          "${_igrib_source}/README.md"
          ${_portable_api_v05_sources}
          ${_igrib_decoder_copy}
          ${_igrib_generator_copy}
  COMMENT "Building and assembling the portable iGRIB package"
  VERBATIM
)
set(_igrib_archive
    "${CMAKE_CURRENT_BINARY_DIR}/packages/org.opencpn.igrib-0.2.0.ocpnp")
add_custom_command(
  OUTPUT "${_igrib_archive}"
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_package.py"
          --root "${_igrib_package}" --output "${_igrib_archive}"
          --signing-key
          "${CMAKE_CURRENT_SOURCE_DIR}/development-keys/igrib-ed25519-private.pem"
          --key-id "org.opencpn.development.igrib-2026"
  DEPENDS "${_igrib_package_stamp}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_package.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/development-keys/igrib-ed25519-private.pem"
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  COMMENT "Creating deterministic iGRIB .ocpnp package"
  VERBATIM
)
add_custom_target(portable_igrib_package ALL
                  DEPENDS "${_igrib_package_stamp}" "${_igrib_archive}")
if (OCPN_BUILD_TEST AND OCPN_IGRIB_BUILD_GENERATOR_HELPER AND
    NOT OCPN_IGRIB_GENERATOR_HELPER)
  add_dependencies(portable_igrib_package environmental_grib_tests
                   environmental_grib_xtd_tests
                   environmental_grib_merge_tests)
endif ()
set(OCPN_PORTABLE_IGRIB_PACKAGE "${_igrib_package}" CACHE INTERNAL
    "Build-tree iGRIB portable package")
set(OCPN_PORTABLE_IGRIB_ARCHIVE "${_igrib_archive}" CACHE INTERNAL
    "Build-tree iGRIB portable package archive")

set(_iwr_source "${CMAKE_SOURCE_DIR}/portable-plugins/iweather-routing")
set(_iwr_target_dir "${CMAKE_CURRENT_BINARY_DIR}/iwr-cargo-target")
set(_iwr_package "${CMAKE_CURRENT_BINARY_DIR}/packages/iweather-routing")
set(_iwr_component "${_iwr_package}/component/iweather-routing.wasm")
set(_iwr_stamp "${CMAKE_CURRENT_BINARY_DIR}/packages/iweather-routing.assembled")
file(GLOB_RECURSE _iwr_sources CONFIGURE_DEPENDS
     "${_iwr_source}/component/src/*.rs")
add_custom_command(
  OUTPUT "${_iwr_stamp}"
  BYPRODUCTS "${_iwr_component}"
  COMMAND ${CMAKE_COMMAND} -E env ${_portable_env}
          "CARGO_TARGET_DIR=${_iwr_target_dir}"
          ${OCPN_PORTABLE_CARGO} build --locked --offline --release
          --target wasm32-wasip2
          --manifest-path "${_iwr_source}/component/Cargo.toml"
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "${_iwr_package}/component" "${_iwr_package}/resources"
          "${_iwr_package}/interfaces" "${_iwr_package}/licenses"
          "${_iwr_package}/ui"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_iwr_target_dir}/wasm32-wasip2/release/iweather_routing_portable_component.wasm"
          "${_iwr_component}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_iwr_source}/package/manifest.json" "${_iwr_package}/manifest.json"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_iwr_source}/package/iweather-routing.svg"
          "${_iwr_package}/resources/iweather-routing.svg"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_iwr_source}/package/Nicholson35_Mk1_cruising_realistic.pol"
          "${_iwr_package}/resources/Nicholson35_Mk1_cruising_realistic.pol"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_iwr_source}/package/iweather-routing.ui.json"
          "${_iwr_package}/ui/iweather-routing.ui.json"
  COMMAND ${CMAKE_COMMAND} -E remove_directory
          "${_iwr_package}/interfaces"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/contracts/0.5"
          "${_iwr_package}/interfaces/opencpn-opp-0.5"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/COPYING.gplv2"
          "${_iwr_package}/licenses/GPL-2.0.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_iwr_source}/README.md" "${_iwr_package}/README.md"
  COMMAND ${CMAKE_COMMAND} -E touch "${_iwr_stamp}"
  DEPENDS ${_iwr_sources} "${_iwr_source}/component/Cargo.toml"
          "${_iwr_source}/component/Cargo.lock"
          "${_iwr_source}/package/manifest.json"
          "${_iwr_source}/package/iweather-routing.svg"
          "${_iwr_source}/package/Nicholson35_Mk1_cruising_realistic.pol"
          "${_iwr_source}/package/iweather-routing.ui.json"
          "${_iwr_source}/README.md"
          ${_portable_api_v05_sources}
  COMMENT "Building and assembling the portable iWeatherRouting package"
  VERBATIM)
set(_iwr_archive
    "${CMAKE_CURRENT_BINARY_DIR}/packages/org.opencpn.iweather-routing-0.2.0.ocpnp")
add_custom_command(
  OUTPUT "${_iwr_archive}"
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_package.py"
          --root "${_iwr_package}" --output "${_iwr_archive}"
          --signing-key
          "${CMAKE_CURRENT_SOURCE_DIR}/development-keys/igrib-ed25519-private.pem"
          --key-id "org.opencpn.development.portable-reference-2026"
  DEPENDS "${_iwr_stamp}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_package.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/development-keys/igrib-ed25519-private.pem"
  COMMENT "Creating deterministic iWeatherRouting .ocpnp package"
  VERBATIM)
add_custom_target(portable_iweather_routing_package ALL
                  DEPENDS "${_iwr_stamp}" "${_iwr_archive}")
set(OCPN_PORTABLE_IWR_PACKAGE "${_iwr_package}" CACHE INTERNAL
    "Build-tree iWeatherRouting portable package")
set(OCPN_PORTABLE_IWR_ARCHIVE "${_iwr_archive}" CACHE INTERNAL
    "Build-tree iWeatherRouting portable package archive")

if (OCPN_BUILD_TEST)
  find_package(Threads REQUIRED)
  add_executable(portable_service_version_test
                 tests/portable_service_version_test.cpp)
  target_compile_features(portable_service_version_test PRIVATE cxx_std_17)
  target_compile_options(portable_service_version_test PRIVATE
                         -Wall -Wextra -Wpedantic)
  target_include_directories(
    portable_service_version_test PRIVATE
    "${CMAKE_SOURCE_DIR}/plugins/portable_plugin_manager_pi/include"
  )
  add_test(NAME portable_service_version COMMAND portable_service_version_test)

  add_executable(
    portable_polar_test
    tests/portable_polar_test.cpp
    "${CMAKE_SOURCE_DIR}/plugins/portable_plugin_manager_pi/src/portable_polar.cpp"
    "${CMAKE_SOURCE_DIR}/libs/pugixml/pugixml.cpp"
  )
  target_compile_features(portable_polar_test PRIVATE cxx_std_17)
  target_compile_options(portable_polar_test PRIVATE -Wall -Wextra -Wpedantic)
  target_include_directories(
    portable_polar_test PRIVATE
    "${CMAKE_SOURCE_DIR}/plugins/portable_plugin_manager_pi/include"
    "${CMAKE_SOURCE_DIR}/libs/pugixml"
  )
  add_test(
    NAME portable_polar
    COMMAND portable_polar_test
            "${_iwr_source}/package/Nicholson35_Mk1_cruising_realistic.pol"
  )
  add_executable(
    portable_ui_menu_test
    tests/portable_ui_menu_test.cpp
    "${CMAKE_SOURCE_DIR}/plugins/portable_plugin_manager_pi/src/portable_ui_menu.cpp"
  )
  target_compile_features(portable_ui_menu_test PRIVATE cxx_std_17)
  if (MSVC)
    target_compile_options(portable_ui_menu_test PRIVATE /W4)
  else ()
    target_compile_options(portable_ui_menu_test PRIVATE
                           -Wall -Wextra -Wpedantic)
  endif ()
  target_include_directories(
    portable_ui_menu_test PRIVATE
    "${CMAKE_SOURCE_DIR}/plugins/portable_plugin_manager_pi/include"
  )
  target_link_libraries(portable_ui_menu_test PRIVATE
                        ocpn::wxjson ${wxWidgets_LIBRARIES})
  add_test(
    NAME portable_ui_menu
    COMMAND portable_ui_menu_test
            "${_iwr_source}/package/iweather-routing.ui.json"
  )
  add_executable(
    portable_departure_time_test
    tests/portable_departure_time_test.cpp
    "${CMAKE_SOURCE_DIR}/plugins/portable_plugin_manager_pi/src/portable_departure_time.cpp"
  )
  target_compile_features(portable_departure_time_test PRIVATE cxx_std_17)
  if (MSVC)
    target_compile_options(portable_departure_time_test PRIVATE /W4)
  else ()
    target_compile_options(portable_departure_time_test PRIVATE
                           -Wall -Wextra -Wpedantic)
  endif ()
  target_include_directories(
    portable_departure_time_test PRIVATE
    "${CMAKE_SOURCE_DIR}/plugins/portable_plugin_manager_pi/include"
  )
  target_link_libraries(portable_departure_time_test PRIVATE
                        ${wxWidgets_LIBRARIES})
  add_test(NAME portable_departure_time COMMAND portable_departure_time_test)
  add_executable(portable_runtime_smoke_test tests/bridge_smoke.cpp)
  set(_legacy_v01_source
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/legacy-v01-component")
  set(_legacy_v01_target_dir
      "${CMAKE_CURRENT_BINARY_DIR}/legacy-v01-cargo-target")
  set(_legacy_v01_component
      "${_legacy_v01_target_dir}/wasm32-wasip2/release/legacy_v01_portable_component.wasm")
  file(GLOB_RECURSE _legacy_v01_sources CONFIGURE_DEPENDS
       "${_legacy_v01_source}/src/*.rs")
  add_custom_command(
    OUTPUT "${_legacy_v01_component}"
    COMMAND ${CMAKE_COMMAND} -E env
            "CARGO_TARGET_DIR=${_legacy_v01_target_dir}"
            "${OCPN_PORTABLE_CARGO}" build --locked --offline --release
            --target wasm32-wasip2
            --manifest-path "${_legacy_v01_source}/Cargo.toml"
    DEPENDS ${_legacy_v01_sources}
            "${_legacy_v01_source}/Cargo.toml"
            "${_legacy_v01_source}/Cargo.lock"
            "${CMAKE_CURRENT_SOURCE_DIR}/wit/opencpn-portable.wit"
    COMMENT "Building the portable API 0.1 compatibility fixture"
    VERBATIM)
  add_custom_target(portable_legacy_v01_component
                    DEPENDS "${_legacy_v01_component}")
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
  add_dependencies(portable_runtime_smoke_test portable_igrib_package
                    portable_iweather_routing_package
                    portable_legacy_v01_component)
  add_test(
    NAME portable_runtime_smoke
    COMMAND portable_runtime_smoke_test "${_igrib_component}" "${_iwr_component}"
            "${_legacy_v01_component}"
  )
  add_test(
    NAME portable_package_tools
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_tools_test.py"
  )
  add_test(
    NAME portable_beta_tools
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/beta_tools_test.py"
  )
  if (OCPN_IGRIB_BUILD_NATIVE_HELPER)
    if (_igrib_generator_copy)
      add_test(
        NAME portable_environment_helper_fields
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/environment_helper_test.py"
                "$<TARGET_FILE:igrib_environment_helper>"
                "${_igrib_generator_copy}"
      )
    else ()
      add_test(
        NAME portable_environment_helper_fields
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/environment_helper_test.py"
                "$<TARGET_FILE:igrib_environment_helper>"
      )
    endif ()
    add_test(
      NAME portable_igrib_conformance
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance.py"
              --package "${_igrib_archive}"
              --trusted-key
              "${CMAKE_CURRENT_SOURCE_DIR}/development-keys/igrib-ed25519-public.pem"
    )
    add_test(
      NAME portable_iweather_routing_environment_fixture
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tests/routing_environment_fixture_test.py"
              "$<TARGET_FILE:igrib_environment_helper>"
    )
  endif ()
  add_test(
    NAME portable_iweather_routing_conformance
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/routing_package_conformance.py"
            --package "${_iwr_archive}"
            --trusted-key
            "${CMAKE_CURRENT_SOURCE_DIR}/development-keys/igrib-ed25519-public.pem"
  )
endif ()
