cmake_minimum_required(VERSION 3.16)
project(portable_plugin_manager_pi VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

set(OPENCPN_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Stock OpenCPN source tree containing include/ocpn_plugin.h")
set(PPM_PLUGIN_LIBRARY_DIR "lib/opencpn" CACHE STRING
    "Install directory for the OpenCPN plugin module")
set(PPM_PLUGIN_DATA_DIR
    "share/opencpn/plugins/portable_plugin_manager_pi" CACHE STRING
    "Install directory for Portable Plugin Manager data")

if(NOT EXISTS "${OPENCPN_SOURCE_DIR}/include/ocpn_plugin.h")
  message(FATAL_ERROR
    "OPENCPN_SOURCE_DIR does not contain include/ocpn_plugin.h")
endif()

find_package(wxWidgets REQUIRED COMPONENTS base core gl)
include(${wxWidgets_USE_FILE})
find_package(Threads REQUIRED)
find_package(LibArchive REQUIRED)
find_package(OpenSSL REQUIRED)
find_program(PPM_CARGO_EXECUTABLE cargo REQUIRED)

set(PPM_RUNTIME_SOURCE_DIR "${OPENCPN_SOURCE_DIR}/portable-runtime" CACHE PATH
    "Portable runtime bridge source directory")
set(PPM_CARGO_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/cargo-target")
if(WIN32)
  set(PPM_RUNTIME_BRIDGE
      "${PPM_CARGO_TARGET_DIR}/release/ocpn_portable_runtime_bridge.lib")
else()
  set(PPM_RUNTIME_BRIDGE
      "${PPM_CARGO_TARGET_DIR}/release/libocpn_portable_runtime_bridge.a")
endif()
file(GLOB_RECURSE PPM_RUNTIME_BRIDGE_SOURCES CONFIGURE_DEPENDS
  "${PPM_RUNTIME_SOURCE_DIR}/bridge/src/*.rs"
  "${PPM_RUNTIME_SOURCE_DIR}/wit/*.wit")
add_custom_command(
  OUTPUT "${PPM_RUNTIME_BRIDGE}"
  COMMAND ${CMAKE_COMMAND} -E env
          "CARGO_TARGET_DIR=${PPM_CARGO_TARGET_DIR}"
          "${PPM_CARGO_EXECUTABLE}" build --locked --offline --release
          --manifest-path "${PPM_RUNTIME_SOURCE_DIR}/bridge/Cargo.toml"
  DEPENDS ${PPM_RUNTIME_BRIDGE_SOURCES}
          "${PPM_RUNTIME_SOURCE_DIR}/bridge/Cargo.toml"
          "${PPM_RUNTIME_SOURCE_DIR}/bridge/Cargo.lock"
  COMMENT "Building the private Wasmtime portable-runtime bridge"
  VERBATIM)
add_custom_target(ppm_runtime_bridge_build DEPENDS "${PPM_RUNTIME_BRIDGE}")
add_library(ppm_runtime_bridge STATIC IMPORTED GLOBAL)
set_target_properties(ppm_runtime_bridge PROPERTIES
  IMPORTED_LOCATION "${PPM_RUNTIME_BRIDGE}")
add_dependencies(ppm_runtime_bridge ppm_runtime_bridge_build)

add_subdirectory("${OPENCPN_SOURCE_DIR}/libs/wxJSON"
                 "${CMAKE_CURRENT_BINARY_DIR}/wxjson")

add_library(portable_plugin_manager_pi MODULE
  src/action_registry.cpp
  src/manager_dialog.cpp
  src/package_store.cpp
  src/portable_plugin_manager_pi.cpp
  src/runtime_engine.cpp
)
set_target_properties(portable_plugin_manager_pi PROPERTIES
  PREFIX "lib"
  OUTPUT_NAME "portable_plugin_manager_pi"
)
target_include_directories(portable_plugin_manager_pi PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/include"
  "${OPENCPN_SOURCE_DIR}/include"
  "${PPM_RUNTIME_SOURCE_DIR}/bridge/include"
)
target_compile_definitions(portable_plugin_manager_pi PRIVATE
  PPM_SOURCE_DATA_DIR="${CMAKE_CURRENT_LIST_DIR}/data"
)
if(MSVC)
  target_compile_options(portable_plugin_manager_pi PRIVATE /W4)
else()
  target_compile_options(portable_plugin_manager_pi PRIVATE
    -Wall -Wextra -Wpedantic)
endif()
target_link_libraries(portable_plugin_manager_pi PRIVATE
  ${wxWidgets_LIBRARIES}
  ocpn::wxjson
  LibArchive::LibArchive
  OpenSSL::Crypto
  ppm_runtime_bridge
  Threads::Threads
  ${CMAKE_DL_LIBS})
if(UNIX AND NOT APPLE)
  target_link_libraries(portable_plugin_manager_pi PRIVATE m)
endif()

install(TARGETS portable_plugin_manager_pi
  LIBRARY DESTINATION "${PPM_PLUGIN_LIBRARY_DIR}"
  RUNTIME DESTINATION "${PPM_PLUGIN_LIBRARY_DIR}")
install(DIRECTORY data/
  DESTINATION "${PPM_PLUGIN_DATA_DIR}")

include(CTest)
if(BUILD_TESTING)
  add_executable(ppm_action_registry_test
    src/action_registry.cpp
    tests/action_registry_test.cpp
  )
  target_include_directories(ppm_action_registry_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include")
  if(MSVC)
    target_compile_options(ppm_action_registry_test PRIVATE /W4)
  else()
    target_compile_options(ppm_action_registry_test PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME ppm_action_registry COMMAND ppm_action_registry_test)

  add_executable(ppm_package_store_test
    src/package_store.cpp
    tests/package_store_test.cpp
  )
  target_include_directories(ppm_package_store_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${OPENCPN_SOURCE_DIR}/libs/wxJSON/include")
  target_link_libraries(ppm_package_store_test PRIVATE
    ${wxWidgets_LIBRARIES}
    ocpn::wxjson
    LibArchive::LibArchive
    OpenSSL::Crypto)
  if(MSVC)
    target_compile_options(ppm_package_store_test PRIVATE /W4)
  else()
    target_compile_options(ppm_package_store_test PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME ppm_package_store COMMAND ppm_package_store_test)

  set(PPM_TEST_IGRIB_TARGET_DIR
      "${CMAKE_CURRENT_BINARY_DIR}/ppm-test-igrib-target")
  set(PPM_TEST_IGRIB_WASM
      "${PPM_TEST_IGRIB_TARGET_DIR}/wasm32-wasip2/release/igrib_portable_component.wasm")
  file(GLOB_RECURSE PPM_TEST_IGRIB_SOURCES CONFIGURE_DEPENDS
    "${OPENCPN_SOURCE_DIR}/portable-plugins/igrib/component/src/*.rs")
  add_custom_command(
    OUTPUT "${PPM_TEST_IGRIB_WASM}"
    COMMAND ${CMAKE_COMMAND} -E env
            "CARGO_TARGET_DIR=${PPM_TEST_IGRIB_TARGET_DIR}"
            "${PPM_CARGO_EXECUTABLE}" build --locked --offline --release
            --target wasm32-wasip2
            --manifest-path
            "${OPENCPN_SOURCE_DIR}/portable-plugins/igrib/component/Cargo.toml"
    DEPENDS ${PPM_TEST_IGRIB_SOURCES}
            "${OPENCPN_SOURCE_DIR}/portable-plugins/igrib/component/Cargo.toml"
            "${OPENCPN_SOURCE_DIR}/portable-plugins/igrib/component/Cargo.lock"
            "${OPENCPN_SOURCE_DIR}/portable-runtime/wit/opencpn-portable.wit"
    COMMENT "Building the portable lifecycle test component"
    VERBATIM)
  add_custom_target(ppm_test_igrib_component
                    DEPENDS "${PPM_TEST_IGRIB_WASM}")

  add_executable(ppm_runtime_engine_test
    src/runtime_engine.cpp
    tests/runtime_engine_test.cpp
  )
  add_dependencies(ppm_runtime_engine_test ppm_test_igrib_component)
  target_include_directories(ppm_runtime_engine_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${OPENCPN_SOURCE_DIR}/include"
    "${OPENCPN_SOURCE_DIR}/libs/wxJSON/include"
    "${PPM_RUNTIME_SOURCE_DIR}/bridge/include")
  target_compile_definitions(ppm_runtime_engine_test PRIVATE
    PPM_TEST_IGRIB_WASM="${PPM_TEST_IGRIB_WASM}")
  target_link_libraries(ppm_runtime_engine_test PRIVATE
    ${wxWidgets_LIBRARIES}
    ocpn::wxjson
    ppm_runtime_bridge
    Threads::Threads
    ${CMAKE_DL_LIBS})
  if(UNIX AND NOT APPLE)
    target_link_libraries(ppm_runtime_engine_test PRIVATE m)
  endif()
  if(MSVC)
    target_compile_options(ppm_runtime_engine_test PRIVATE /W4)
  else()
    target_compile_options(ppm_runtime_engine_test PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME ppm_runtime_engine COMMAND ppm_runtime_engine_test)
endif()
