cmake_minimum_required(VERSION 3.16)
project(portable_plugin_manager_pi VERSION 0.2.1 LANGUAGES CXX)

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
set(PPM_DEVELOPMENT_TRUST_KEY "" CACHE FILEPATH
    "Optional local public key used for isolated development package trust")

if(NOT EXISTS "${OPENCPN_SOURCE_DIR}/include/ocpn_plugin.h")
  message(FATAL_ERROR
    "OPENCPN_SOURCE_DIR does not contain include/ocpn_plugin.h")
endif()

find_package(wxWidgets REQUIRED COMPONENTS base core gl)
include(${wxWidgets_USE_FILE})
find_package(Threads REQUIRED)
find_package(LibArchive REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
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
  "${PPM_RUNTIME_SOURCE_DIR}/wit/*.wit"
  "${PPM_RUNTIME_SOURCE_DIR}/contracts/*.wit")
file(GLOB_RECURSE PPM_PORTABLE_API_V02_SOURCES CONFIGURE_DEPENDS
  "${OPENCPN_SOURCE_DIR}/portable-runtime/contracts/0.2/*.wit")
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
add_subdirectory("${OPENCPN_SOURCE_DIR}/libs/pugixml"
                 "${CMAKE_CURRENT_BINARY_DIR}/pugixml")

add_library(portable_plugin_manager_pi MODULE
  src/action_registry.cpp
  src/capability_event_broker.cpp
  src/chart_safety_service.cpp
  src/cm93_semantic_reader.cpp
  src/declarative_ui.cpp
  src/environment_service.cpp
  src/environment_provider.cpp
  src/environment_workbench.cpp
  src/job_scheduler.cpp
  src/https_service.cpp
  src/manager_dialog.cpp
  src/package_store.cpp
  src/permission_store.cpp
  src/portable_departure_time.cpp
  src/portable_polar.cpp
  src/portable_plugin_manager_pi.cpp
  src/portable_ui_menu.cpp
  src/runtime_engine.cpp
  src/serial_executor.cpp
  src/surface_dialog.cpp
  src/weather_routing_host.cpp
  src/weather_routing_departure_coordinator.cpp
  src/weather_routing_stability.cpp
  src/wind_barb_geometry.cpp
  src/window_activation.cpp
  src/weather_routing_display.cpp
)
set_target_properties(portable_plugin_manager_pi PROPERTIES
  PREFIX "lib"
  OUTPUT_NAME "portable_plugin_manager_pi"
)
target_include_directories(portable_plugin_manager_pi PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/include"
  "${OPENCPN_SOURCE_DIR}/include"
  "${OPENCPN_SOURCE_DIR}/libs/picosha2"
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
  ocpn::pugixml
  LibArchive::LibArchive
  OpenSSL::Crypto
  CURL::libcurl
  ppm_runtime_bridge
  Threads::Threads
  ${CMAKE_DL_LIBS})
if(UNIX AND NOT APPLE)
  target_link_libraries(portable_plugin_manager_pi PRIVATE m)
endif()

install(TARGETS portable_plugin_manager_pi
  LIBRARY DESTINATION "${PPM_PLUGIN_LIBRARY_DIR}"
  RUNTIME DESTINATION "${PPM_PLUGIN_LIBRARY_DIR}")
if(PPM_DEVELOPMENT_TRUST_KEY)
  if(NOT EXISTS "${PPM_DEVELOPMENT_TRUST_KEY}")
    message(FATAL_ERROR
      "PPM_DEVELOPMENT_TRUST_KEY does not exist: ${PPM_DEVELOPMENT_TRUST_KEY}")
  endif()
  install(DIRECTORY data/
    DESTINATION "${PPM_PLUGIN_DATA_DIR}"
    PATTERN "trust" EXCLUDE)
  install(FILES "${PPM_DEVELOPMENT_TRUST_KEY}"
    DESTINATION "${PPM_PLUGIN_DATA_DIR}/trust"
    RENAME "org.opencpn.development.igrib-2026.pem")
  install(FILES "${PPM_DEVELOPMENT_TRUST_KEY}"
    DESTINATION "${PPM_PLUGIN_DATA_DIR}/trust"
    RENAME "org.opencpn.development.portable-reference-2026.pem")
else()
  install(DIRECTORY data/
    DESTINATION "${PPM_PLUGIN_DATA_DIR}")
endif()

include(CTest)
if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME ppm_portable_sdk_tooling
           COMMAND "${Python3_EXECUTABLE}"
                   "${PPM_RUNTIME_SOURCE_DIR}/sdk/tests/tooling_test.py")

  set(PPM_SDK_TEMPLATE_DIR
      "${PPM_RUNTIME_SOURCE_DIR}/sdk/rust-template")
  set(PPM_SDK_TEMPLATE_TARGET_DIR
      "${CMAKE_CURRENT_BINARY_DIR}/sdk-template-target")
  set(PPM_SDK_TEMPLATE_COMPONENT
      "${PPM_SDK_TEMPLATE_TARGET_DIR}/wasm32-wasip2/release/portable_plugin_template.wasm")
  file(GLOB_RECURSE PPM_SDK_TEMPLATE_SOURCES CONFIGURE_DEPENDS
    "${PPM_SDK_TEMPLATE_DIR}/src/*.rs"
    "${PPM_RUNTIME_SOURCE_DIR}/contracts/0.4/*.wit")
  add_custom_command(
    OUTPUT "${PPM_SDK_TEMPLATE_COMPONENT}"
    COMMAND ${CMAKE_COMMAND} -E env
            "CARGO_TARGET_DIR=${PPM_SDK_TEMPLATE_TARGET_DIR}"
            "${PPM_CARGO_EXECUTABLE}" build --locked --offline --release
            --target wasm32-wasip2
            --manifest-path "${PPM_SDK_TEMPLATE_DIR}/Cargo.toml"
    DEPENDS ${PPM_SDK_TEMPLATE_SOURCES}
            "${PPM_SDK_TEMPLATE_DIR}/Cargo.toml"
            "${PPM_SDK_TEMPLATE_DIR}/Cargo.lock"
    COMMENT "Building the standalone OPP API 0.4 author template"
    VERBATIM)
  add_custom_target(ppm_sdk_template_build
                    DEPENDS "${PPM_SDK_TEMPLATE_COMPONENT}")
  add_executable(ppm_portable_fake_host
    "${PPM_RUNTIME_SOURCE_DIR}/sdk/fake-host/main.cpp")
  target_include_directories(ppm_portable_fake_host PRIVATE
    "${PPM_RUNTIME_SOURCE_DIR}/bridge/include")
  target_link_libraries(ppm_portable_fake_host PRIVATE
    ppm_runtime_bridge Threads::Threads ${CMAKE_DL_LIBS})
  add_dependencies(ppm_portable_fake_host ppm_sdk_template_build)
  add_test(NAME ppm_opp_api_v04_fake_host
           COMMAND ppm_portable_fake_host "${PPM_SDK_TEMPLATE_COMPONENT}")

  add_executable(ppm_cm93_semantic_reader_test
    src/cm93_semantic_reader.cpp
    tests/cm93_semantic_reader_test.cpp
  )
  target_include_directories(ppm_cm93_semantic_reader_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include")
  if(MSVC)
    target_compile_options(ppm_cm93_semantic_reader_test PRIVATE /W4)
  else()
    target_compile_options(ppm_cm93_semantic_reader_test PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME ppm_cm93_semantic_reader
           COMMAND ppm_cm93_semantic_reader_test)

  add_executable(ppm_capability_event_broker_test
    src/capability_event_broker.cpp
    tests/capability_event_broker_test.cpp
  )
  target_include_directories(ppm_capability_event_broker_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include")
  if(MSVC)
    target_compile_options(ppm_capability_event_broker_test PRIVATE /W4)
  else()
    target_compile_options(ppm_capability_event_broker_test PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME ppm_capability_event_broker
           COMMAND ppm_capability_event_broker_test)

  add_executable(ppm_wind_barb_geometry_test
    src/wind_barb_geometry.cpp
    tests/wind_barb_geometry_test.cpp
  )
  target_include_directories(ppm_wind_barb_geometry_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include")
  if(MSVC)
    target_compile_options(ppm_wind_barb_geometry_test PRIVATE /W4)
  else()
    target_compile_options(ppm_wind_barb_geometry_test PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME ppm_wind_barb_geometry
           COMMAND ppm_wind_barb_geometry_test)

  add_executable(ppm_weather_routing_display_test
    src/weather_routing_display.cpp
    tests/weather_routing_display_test.cpp
  )
  target_include_directories(ppm_weather_routing_display_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include")
  if(MSVC)
    target_compile_options(ppm_weather_routing_display_test PRIVATE /W4)
  else()
    target_compile_options(ppm_weather_routing_display_test PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME ppm_weather_routing_display
           COMMAND ppm_weather_routing_display_test)

  add_executable(ppm_weather_routing_departure_coordinator_test
    src/portable_departure_time.cpp
    src/weather_routing_departure_coordinator.cpp
    tests/weather_routing_departure_coordinator_test.cpp
  )
  target_include_directories(
    ppm_weather_routing_departure_coordinator_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${PPM_RUNTIME_SOURCE_DIR}/bridge/include")
  target_link_libraries(ppm_weather_routing_departure_coordinator_test PRIVATE
    ${wxWidgets_LIBRARIES} Threads::Threads)
  if(MSVC)
    target_compile_options(
      ppm_weather_routing_departure_coordinator_test PRIVATE /W4)
  else()
    target_compile_options(
      ppm_weather_routing_departure_coordinator_test PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME ppm_weather_routing_departure_coordinator
           COMMAND ppm_weather_routing_departure_coordinator_test)

  add_executable(ppm_weather_routing_stability_test
    src/weather_routing_stability.cpp
    tests/weather_routing_stability_test.cpp
  )
  target_include_directories(ppm_weather_routing_stability_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include")
  if(MSVC)
    target_compile_options(ppm_weather_routing_stability_test PRIVATE /W4)
  else()
    target_compile_options(ppm_weather_routing_stability_test PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME ppm_weather_routing_stability
           COMMAND ppm_weather_routing_stability_test)

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

  add_executable(ppm_https_service_test
    src/https_service.cpp
    tests/https_service_test.cpp
  )
  target_include_directories(ppm_https_service_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include")
  target_link_libraries(ppm_https_service_test PRIVATE CURL::libcurl)
  if(MSVC)
    target_compile_options(ppm_https_service_test PRIVATE /W4)
  else()
    target_compile_options(ppm_https_service_test PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME ppm_https_service COMMAND ppm_https_service_test)

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

  add_executable(ppm_portable_polar_test
    src/portable_polar.cpp
    "${PPM_RUNTIME_SOURCE_DIR}/tests/portable_polar_test.cpp"
  )
  target_include_directories(ppm_portable_polar_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${OPENCPN_SOURCE_DIR}/libs/pugixml")
  target_link_libraries(ppm_portable_polar_test PRIVATE ocpn::pugixml)
  if(MSVC)
    target_compile_options(ppm_portable_polar_test PRIVATE /W4)
  else()
    target_compile_options(ppm_portable_polar_test PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME ppm_portable_polar COMMAND ppm_portable_polar_test
    "${OPENCPN_SOURCE_DIR}/portable-plugins/iweather-routing/package/Nicholson35_Mk1_cruising_realistic.pol")

  add_executable(ppm_portable_departure_time_test
    src/portable_departure_time.cpp
    "${PPM_RUNTIME_SOURCE_DIR}/tests/portable_departure_time_test.cpp"
  )
  target_include_directories(ppm_portable_departure_time_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include")
  target_link_libraries(ppm_portable_departure_time_test PRIVATE
    ${wxWidgets_LIBRARIES})
  if(MSVC)
    target_compile_options(ppm_portable_departure_time_test PRIVATE /W4)
  else()
    target_compile_options(ppm_portable_departure_time_test PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME ppm_portable_departure_time
           COMMAND ppm_portable_departure_time_test)

  add_executable(ppm_portable_ui_menu_test
    src/portable_ui_menu.cpp
    "${PPM_RUNTIME_SOURCE_DIR}/tests/portable_ui_menu_test.cpp"
  )
  target_include_directories(ppm_portable_ui_menu_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${OPENCPN_SOURCE_DIR}/libs/wxJSON/include")
  target_link_libraries(ppm_portable_ui_menu_test PRIVATE
    ${wxWidgets_LIBRARIES} ocpn::wxjson)
  if(MSVC)
    target_compile_options(ppm_portable_ui_menu_test PRIVATE /W4)
  else()
    target_compile_options(ppm_portable_ui_menu_test PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME ppm_portable_ui_menu COMMAND ppm_portable_ui_menu_test
    "${OPENCPN_SOURCE_DIR}/portable-plugins/iweather-routing/package/iweather-routing.ui.json")

  add_executable(ppm_environment_service_test
    src/environment_service.cpp
    tests/environment_service_test.cpp
  )
  target_include_directories(ppm_environment_service_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include")
  if(MSVC)
    target_compile_options(ppm_environment_service_test PRIVATE /W4)
  else()
    target_compile_options(ppm_environment_service_test PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME ppm_environment_service COMMAND ppm_environment_service_test)

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

  add_executable(ppm_environment_probe
    src/environment_service.cpp
    tools/environment_probe.cpp
  )
  target_include_directories(ppm_environment_probe PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include")
  if(MSVC)
    target_compile_options(ppm_environment_probe PRIVATE /W4)
  else()
    target_compile_options(ppm_environment_probe PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()

  add_executable(ppm_environment_provider_probe
    src/environment_provider.cpp
    src/environment_service.cpp
    tools/environment_provider_probe.cpp
  )
  target_include_directories(ppm_environment_provider_probe PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${OPENCPN_SOURCE_DIR}/libs/wxJSON/include")
  target_link_libraries(ppm_environment_provider_probe PRIVATE
    ${wxWidgets_LIBRARIES}
    ocpn::wxjson
    Threads::Threads)
  if(MSVC)
    target_compile_options(ppm_environment_provider_probe PRIVATE /W4)
  else()
    target_compile_options(ppm_environment_provider_probe PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()

  add_executable(ppm_routing_probe
    src/capability_event_broker.cpp
    src/chart_safety_service.cpp
    src/cm93_semantic_reader.cpp
    src/declarative_ui.cpp
    src/environment_provider.cpp
    src/environment_service.cpp
    src/job_scheduler.cpp
    src/https_service.cpp
    src/permission_store.cpp
    src/portable_polar.cpp
    src/runtime_engine.cpp
    src/serial_executor.cpp
    tools/routing_probe.cpp
  )
  target_include_directories(ppm_routing_probe PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${OPENCPN_SOURCE_DIR}/include"
    "${OPENCPN_SOURCE_DIR}/libs/pugixml"
    "${OPENCPN_SOURCE_DIR}/libs/wxJSON/include"
    "${PPM_RUNTIME_SOURCE_DIR}/bridge/include")
  target_link_libraries(ppm_routing_probe PRIVATE
    ${wxWidgets_LIBRARIES}
    ocpn::pugixml
    ocpn::wxjson
    OpenSSL::Crypto
    CURL::libcurl
    ppm_runtime_bridge
    Threads::Threads
    ${CMAKE_DL_LIBS})
  if(UNIX AND NOT APPLE)
    target_link_libraries(ppm_routing_probe PRIVATE m)
  endif()
  if(MSVC)
    target_compile_options(ppm_routing_probe PRIVATE /W4)
  else()
    target_compile_options(ppm_routing_probe PRIVATE
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
            ${PPM_PORTABLE_API_V02_SOURCES}
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
            ${PPM_PORTABLE_API_V02_SOURCES}
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
            ${PPM_PORTABLE_API_V02_SOURCES}
    COMMENT "Building the iPolars lifecycle test component"
    VERBATIM)
  add_custom_target(ppm_test_ipolars_component
                    DEPENDS "${PPM_TEST_IPOLARS_WASM}")

  set(PPM_TEST_CAPABILITY_TARGET_DIR
      "${CMAKE_CURRENT_BINARY_DIR}/ppm-test-capability-target")
  set(PPM_TEST_CAPABILITY_WASM
      "${PPM_TEST_CAPABILITY_TARGET_DIR}/wasm32-wasip2/release/capability_lab_portable_component.wasm")
  file(GLOB_RECURSE PPM_TEST_CAPABILITY_SOURCES CONFIGURE_DEPENDS
    "${OPENCPN_SOURCE_DIR}/portable-plugins/capability-lab/component/src/*.rs")
  add_custom_command(
    OUTPUT "${PPM_TEST_CAPABILITY_WASM}"
    COMMAND ${CMAKE_COMMAND} -E env
            "CARGO_TARGET_DIR=${PPM_TEST_CAPABILITY_TARGET_DIR}"
            "${PPM_CARGO_EXECUTABLE}" build --locked --offline --release
            --target wasm32-wasip2
            --manifest-path
            "${OPENCPN_SOURCE_DIR}/portable-plugins/capability-lab/component/Cargo.toml"
    DEPENDS ${PPM_TEST_CAPABILITY_SOURCES}
            "${OPENCPN_SOURCE_DIR}/portable-plugins/capability-lab/component/Cargo.toml"
            "${OPENCPN_SOURCE_DIR}/portable-plugins/capability-lab/component/Cargo.lock"
            ${PPM_PORTABLE_API_V02_SOURCES}
    COMMENT "Building the portable capability-lab component"
    VERBATIM)
  add_custom_target(ppm_test_capability_component
                    DEPENDS "${PPM_TEST_CAPABILITY_WASM}")

  set(PPM_TEST_LEGACY_V01_SOURCE
      "${OPENCPN_SOURCE_DIR}/portable-runtime/tests/legacy-v01-component")
  set(PPM_TEST_LEGACY_V01_TARGET_DIR
      "${CMAKE_CURRENT_BINARY_DIR}/ppm-test-legacy-v01-target")
  set(PPM_TEST_LEGACY_V01_WASM
      "${PPM_TEST_LEGACY_V01_TARGET_DIR}/wasm32-wasip2/release/legacy_v01_portable_component.wasm")
  file(GLOB_RECURSE PPM_TEST_LEGACY_V01_SOURCES CONFIGURE_DEPENDS
    "${PPM_TEST_LEGACY_V01_SOURCE}/src/*.rs")
  add_custom_command(
    OUTPUT "${PPM_TEST_LEGACY_V01_WASM}"
    COMMAND ${CMAKE_COMMAND} -E env
            "CARGO_TARGET_DIR=${PPM_TEST_LEGACY_V01_TARGET_DIR}"
            "${PPM_CARGO_EXECUTABLE}" build --locked --offline --release
            --target wasm32-wasip2
            --manifest-path "${PPM_TEST_LEGACY_V01_SOURCE}/Cargo.toml"
    DEPENDS ${PPM_TEST_LEGACY_V01_SOURCES}
            "${PPM_TEST_LEGACY_V01_SOURCE}/Cargo.toml"
            "${PPM_TEST_LEGACY_V01_SOURCE}/Cargo.lock"
            "${OPENCPN_SOURCE_DIR}/portable-runtime/wit/opencpn-portable.wit"
    COMMENT "Building the portable API 0.1 compatibility fixture"
    VERBATIM)
  add_custom_target(ppm_test_legacy_v01_component
                    DEPENDS "${PPM_TEST_LEGACY_V01_WASM}")

  add_executable(ppm_runtime_bridge_smoke
    "${OPENCPN_SOURCE_DIR}/portable-runtime/tests/bridge_smoke.cpp")
  add_dependencies(ppm_runtime_bridge_smoke ppm_test_igrib_component
                   ppm_test_iweather_component
                   ppm_test_legacy_v01_component)
  target_include_directories(ppm_runtime_bridge_smoke PRIVATE
    "${PPM_RUNTIME_SOURCE_DIR}/bridge/include")
  target_link_libraries(ppm_runtime_bridge_smoke PRIVATE
    ppm_runtime_bridge Threads::Threads ${CMAKE_DL_LIBS})
  if(UNIX AND NOT APPLE)
    target_link_libraries(ppm_runtime_bridge_smoke PRIVATE m)
  endif()
  target_compile_features(ppm_runtime_bridge_smoke PRIVATE cxx_std_17)
  add_test(
    NAME ppm_runtime_bridge_smoke
    COMMAND ppm_runtime_bridge_smoke "${PPM_TEST_IGRIB_WASM}"
            "${PPM_TEST_IWEATHER_WASM}" "${PPM_TEST_LEGACY_V01_WASM}")

  add_executable(ppm_capability_runtime_test
    src/capability_event_broker.cpp
    src/chart_safety_service.cpp
    src/cm93_semantic_reader.cpp
    src/declarative_ui.cpp
    src/environment_provider.cpp
    src/environment_service.cpp
    src/job_scheduler.cpp
    src/https_service.cpp
    src/permission_store.cpp
    src/runtime_engine.cpp
    src/serial_executor.cpp
    tests/capability_runtime_test.cpp
  )
  add_dependencies(ppm_capability_runtime_test
                   ppm_test_capability_component)
  target_include_directories(ppm_capability_runtime_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${OPENCPN_SOURCE_DIR}/include"
    "${OPENCPN_SOURCE_DIR}/libs/wxJSON/include"
    "${PPM_RUNTIME_SOURCE_DIR}/bridge/include")
  target_compile_definitions(ppm_capability_runtime_test PRIVATE
    PPM_TEST_CAPABILITY_WASM="${PPM_TEST_CAPABILITY_WASM}")
  target_link_libraries(ppm_capability_runtime_test PRIVATE
    ${wxWidgets_LIBRARIES}
    ocpn::wxjson
    OpenSSL::Crypto
    CURL::libcurl
    ppm_runtime_bridge
    Threads::Threads
    ${CMAKE_DL_LIBS})
  if(UNIX AND NOT APPLE)
    target_link_libraries(ppm_capability_runtime_test PRIVATE m)
  endif()
  if(MSVC)
    target_compile_options(ppm_capability_runtime_test PRIVATE /W4)
  else()
    target_compile_options(ppm_capability_runtime_test PRIVATE
      -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME ppm_capability_runtime COMMAND ppm_capability_runtime_test)

  add_executable(ppm_runtime_engine_test
    src/capability_event_broker.cpp
    src/chart_safety_service.cpp
    src/cm93_semantic_reader.cpp
    src/declarative_ui.cpp
    src/environment_provider.cpp
    src/environment_service.cpp
    src/job_scheduler.cpp
    src/https_service.cpp
    src/permission_store.cpp
    src/runtime_engine.cpp
    src/serial_executor.cpp
    tests/runtime_engine_test.cpp
  )
  add_dependencies(ppm_runtime_engine_test ppm_test_igrib_component
                   ppm_test_iweather_component ppm_sdk_template_build)
  target_include_directories(ppm_runtime_engine_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${OPENCPN_SOURCE_DIR}/include"
    "${OPENCPN_SOURCE_DIR}/libs/wxJSON/include"
    "${PPM_RUNTIME_SOURCE_DIR}/bridge/include")
  target_compile_definitions(ppm_runtime_engine_test PRIVATE
    PPM_TEST_IGRIB_WASM="${PPM_TEST_IGRIB_WASM}"
    PPM_TEST_IGRIB_UI="${OPENCPN_SOURCE_DIR}/portable-plugins/igrib/package/igrib-viewer.ui.json"
    PPM_TEST_IWEATHER_WASM="${PPM_TEST_IWEATHER_WASM}"
    PPM_TEST_IWEATHER_UI="${OPENCPN_SOURCE_DIR}/portable-plugins/iweather-routing/package/iweather-routing.ui.json"
    PPM_TEST_API_V04_WASM="${PPM_SDK_TEMPLATE_COMPONENT}"
    PPM_TEST_API_V04_UI="${PPM_RUNTIME_SOURCE_DIR}/sdk/rust-template/package/ui/template.ui.json")
  target_link_libraries(ppm_runtime_engine_test PRIVATE
    ${wxWidgets_LIBRARIES}
    ocpn::wxjson
    OpenSSL::Crypto
    CURL::libcurl
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
    src/capability_event_broker.cpp
    src/chart_safety_service.cpp
    src/cm93_semantic_reader.cpp
    src/declarative_ui.cpp
    src/environment_provider.cpp
    src/environment_service.cpp
    src/job_scheduler.cpp
    src/https_service.cpp
    src/permission_store.cpp
    src/portable_polar.cpp
    src/runtime_engine.cpp
    src/serial_executor.cpp
    tests/ipolars_runtime_test.cpp
  )
  add_dependencies(ppm_ipolars_runtime_test ppm_test_ipolars_component)
  target_include_directories(ppm_ipolars_runtime_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${OPENCPN_SOURCE_DIR}/include"
    "${OPENCPN_SOURCE_DIR}/libs/pugixml"
    "${OPENCPN_SOURCE_DIR}/libs/wxJSON/include"
    "${PPM_RUNTIME_SOURCE_DIR}/bridge/include")
  target_compile_definitions(ppm_ipolars_runtime_test PRIVATE
    PPM_TEST_IPOLARS_WASM="${PPM_TEST_IPOLARS_WASM}"
    PPM_TEST_IPOLARS_UI="${OPENCPN_SOURCE_DIR}/portable-plugins/ipolars/package/ui/ipolars.ui.json"
    PPM_TEST_IPOLARS_MANIFEST="${OPENCPN_SOURCE_DIR}/portable-plugins/ipolars/package/manifest.json")
  target_link_libraries(ppm_ipolars_runtime_test PRIVATE
    ${wxWidgets_LIBRARIES}
    ocpn::pugixml
    ocpn::wxjson
    OpenSSL::Crypto
    CURL::libcurl
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
