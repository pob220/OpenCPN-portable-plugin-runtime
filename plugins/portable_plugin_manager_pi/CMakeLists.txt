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
  src/declarative_ui.cpp
  src/job_scheduler.cpp
  src/manager_dialog.cpp
  src/package_store.cpp
  src/permission_store.cpp
  src/portable_plugin_manager_pi.cpp
  src/runtime_engine.cpp
  src/serial_executor.cpp
  src/surface_dialog.cpp
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

  add_executable(ppm_declarative_ui_test
    src/declarative_ui.cpp
    tests/declarative_ui_test.cpp
  )
  target_include_directories(ppm_declarative_ui_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${OPENCPN_SOURCE_DIR}/libs/wxJSON/include")
  target_compile_definitions(ppm_declarative_ui_test PRIVATE
    PPM_TEST_ROUTING_UI="${OPENCPN_SOURCE_DIR}/portable-plugins/iweather-routing/package/iweather-routing.ui.json"
    PPM_TEST_IGRIB_UI="${OPENCPN_SOURCE_DIR}/portable-plugins/igrib/package/igrib-viewer.ui.json"
    PPM_TEST_IPOLARS_UI="${OPENCPN_SOURCE_DIR}/portable-plugins/ipolars/package/ui/ipolars.ui.json")
  target_link_libraries(ppm_declarative_ui_test PRIVATE
    ${wxWidgets_LIBRARIES}
    ocpn::wxjson)
  if(MSVC)
    target_compile_options(ppm_declarative_ui_test PRIVATE /W4)
  else()
    target_compile_options(ppm_declarative_ui_test PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME ppm_declarative_ui COMMAND ppm_declarative_ui_test)

  add_executable(ppm_job_scheduler_test
    src/job_scheduler.cpp
    tests/job_scheduler_test.cpp
  )
  target_include_directories(ppm_job_scheduler_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include")
  target_link_libraries(ppm_job_scheduler_test PRIVATE Threads::Threads)
  if(MSVC)
    target_compile_options(ppm_job_scheduler_test PRIVATE /W4)
  else()
    target_compile_options(ppm_job_scheduler_test PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME ppm_job_scheduler COMMAND ppm_job_scheduler_test)

  add_executable(ppm_serial_executor_test
    src/serial_executor.cpp
    tests/serial_executor_test.cpp
  )
  target_include_directories(ppm_serial_executor_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include")
  target_link_libraries(ppm_serial_executor_test PRIVATE Threads::Threads)
  if(MSVC)
    target_compile_options(ppm_serial_executor_test PRIVATE /W4)
  else()
    target_compile_options(ppm_serial_executor_test PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME ppm_serial_executor COMMAND ppm_serial_executor_test)

  add_executable(ppm_service_version_test
    tests/service_version_test.cpp
  )
  target_include_directories(ppm_service_version_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include")
  if(MSVC)
    target_compile_options(ppm_service_version_test PRIVATE /W4)
  else()
    target_compile_options(ppm_service_version_test PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME ppm_service_version COMMAND ppm_service_version_test)

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

  add_executable(ppm_permission_store_test
    src/permission_store.cpp
    tests/permission_store_test.cpp
  )
  target_include_directories(ppm_permission_store_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include")
  if(MSVC)
    target_compile_options(ppm_permission_store_test PRIVATE /W4)
  else()
    target_compile_options(ppm_permission_store_test PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME ppm_permission_store COMMAND ppm_permission_store_test)

  add_executable(ppm_package_cli
    src/package_store.cpp
    src/permission_store.cpp
    tools/package_cli.cpp
  )
  target_include_directories(ppm_package_cli PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${OPENCPN_SOURCE_DIR}/libs/wxJSON/include")
  target_link_libraries(ppm_package_cli PRIVATE
    ${wxWidgets_LIBRARIES}
    ocpn::wxjson
    LibArchive::LibArchive
    OpenSSL::Crypto)
  if(MSVC)
    target_compile_options(ppm_package_cli PRIVATE /W4)
  else()
    target_compile_options(ppm_package_cli PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()

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

  set(PPM_TEST_IWEATHER_TARGET_DIR
      "${CMAKE_CURRENT_BINARY_DIR}/ppm-test-iweather-target")
  set(PPM_TEST_IWEATHER_WASM
      "${PPM_TEST_IWEATHER_TARGET_DIR}/wasm32-wasip2/release/iweather_routing_portable_component.wasm")
  file(GLOB_RECURSE PPM_TEST_IWEATHER_SOURCES CONFIGURE_DEPENDS
    "${OPENCPN_SOURCE_DIR}/portable-plugins/iweather-routing/component/src/*.rs")
  add_custom_command(
    OUTPUT "${PPM_TEST_IWEATHER_WASM}"
    COMMAND ${CMAKE_COMMAND} -E env
            "CARGO_TARGET_DIR=${PPM_TEST_IWEATHER_TARGET_DIR}"
            "${PPM_CARGO_EXECUTABLE}" build --locked --offline --release
            --target wasm32-wasip2
            --manifest-path
            "${OPENCPN_SOURCE_DIR}/portable-plugins/iweather-routing/component/Cargo.toml"
    DEPENDS ${PPM_TEST_IWEATHER_SOURCES}
            "${OPENCPN_SOURCE_DIR}/portable-plugins/iweather-routing/component/Cargo.toml"
            "${OPENCPN_SOURCE_DIR}/portable-plugins/iweather-routing/component/Cargo.lock"
            "${OPENCPN_SOURCE_DIR}/portable-runtime/wit/opencpn-portable.wit"
    COMMENT "Building the iWeatherRouting lifecycle test component"
    VERBATIM)
  add_custom_target(ppm_test_iweather_component
                    DEPENDS "${PPM_TEST_IWEATHER_WASM}")

  set(PPM_TEST_IPOLARS_TARGET_DIR
      "${CMAKE_CURRENT_BINARY_DIR}/ppm-test-ipolars-target")
  set(PPM_TEST_IPOLARS_WASM
      "${PPM_TEST_IPOLARS_TARGET_DIR}/wasm32-wasip2/release/ipolars_portable_component.wasm")
  file(GLOB_RECURSE PPM_TEST_IPOLARS_SOURCES CONFIGURE_DEPENDS
    "${OPENCPN_SOURCE_DIR}/portable-plugins/ipolars/component/src/*.rs")
  add_custom_command(
    OUTPUT "${PPM_TEST_IPOLARS_WASM}"
    COMMAND ${CMAKE_COMMAND} -E env
            "CARGO_TARGET_DIR=${PPM_TEST_IPOLARS_TARGET_DIR}"
            "${PPM_CARGO_EXECUTABLE}" build --locked --offline --release
            --target wasm32-wasip2
            --manifest-path
            "${OPENCPN_SOURCE_DIR}/portable-plugins/ipolars/component/Cargo.toml"
    DEPENDS ${PPM_TEST_IPOLARS_SOURCES}
            "${OPENCPN_SOURCE_DIR}/portable-plugins/ipolars/component/Cargo.toml"
            "${OPENCPN_SOURCE_DIR}/portable-plugins/ipolars/component/Cargo.lock"
            "${OPENCPN_SOURCE_DIR}/portable-runtime/wit/opencpn-portable.wit"
    COMMENT "Building the iPolars lifecycle test component"
    VERBATIM)
  add_custom_target(ppm_test_ipolars_component
                    DEPENDS "${PPM_TEST_IPOLARS_WASM}")

  add_executable(ppm_runtime_engine_test
    src/declarative_ui.cpp
    src/job_scheduler.cpp
    src/permission_store.cpp
    src/runtime_engine.cpp
    src/serial_executor.cpp
    tests/runtime_engine_test.cpp
  )
  add_dependencies(ppm_runtime_engine_test ppm_test_igrib_component
                   ppm_test_iweather_component)
  target_include_directories(ppm_runtime_engine_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${OPENCPN_SOURCE_DIR}/include"
    "${OPENCPN_SOURCE_DIR}/libs/wxJSON/include"
    "${PPM_RUNTIME_SOURCE_DIR}/bridge/include")
  target_compile_definitions(ppm_runtime_engine_test PRIVATE
    PPM_TEST_IGRIB_WASM="${PPM_TEST_IGRIB_WASM}"
    PPM_TEST_IGRIB_UI="${OPENCPN_SOURCE_DIR}/portable-plugins/igrib/package/igrib-viewer.ui.json"
    PPM_TEST_IWEATHER_WASM="${PPM_TEST_IWEATHER_WASM}"
    PPM_TEST_IWEATHER_UI="${OPENCPN_SOURCE_DIR}/portable-plugins/iweather-routing/package/iweather-routing.ui.json")
  target_link_libraries(ppm_runtime_engine_test PRIVATE
    ${wxWidgets_LIBRARIES}
    ocpn::wxjson
    OpenSSL::Crypto
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

  add_executable(ppm_ipolars_runtime_test
    src/declarative_ui.cpp
    src/job_scheduler.cpp
    src/permission_store.cpp
    src/runtime_engine.cpp
    src/serial_executor.cpp
    tests/ipolars_runtime_test.cpp
  )
  add_dependencies(ppm_ipolars_runtime_test ppm_test_ipolars_component)
  target_include_directories(ppm_ipolars_runtime_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${OPENCPN_SOURCE_DIR}/include"
    "${OPENCPN_SOURCE_DIR}/libs/wxJSON/include"
    "${PPM_RUNTIME_SOURCE_DIR}/bridge/include")
  target_compile_definitions(ppm_ipolars_runtime_test PRIVATE
    PPM_TEST_IPOLARS_WASM="${PPM_TEST_IPOLARS_WASM}"
    PPM_TEST_IPOLARS_UI="${OPENCPN_SOURCE_DIR}/portable-plugins/ipolars/package/ui/ipolars.ui.json")
  target_link_libraries(ppm_ipolars_runtime_test PRIVATE
    ${wxWidgets_LIBRARIES}
    ocpn::wxjson
    OpenSSL::Crypto
    ppm_runtime_bridge
    Threads::Threads
    ${CMAKE_DL_LIBS})
  if(UNIX AND NOT APPLE)
    target_link_libraries(ppm_ipolars_runtime_test PRIVATE m)
  endif()
  if(MSVC)
    target_compile_options(ppm_ipolars_runtime_test PRIVATE /W4)
  else()
    target_compile_options(ppm_ipolars_runtime_test PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME ppm_ipolars_runtime COMMAND ppm_ipolars_runtime_test)
endif()
