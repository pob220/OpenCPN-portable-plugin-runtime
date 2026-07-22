cmake_minimum_required(VERSION 3.16)
project(runtime_host_toolbar_probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

set(OPENCPN_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "OpenCPN source tree containing include/ocpn_plugin.h")
set(RUNTIME_HOST_PROBE_BRIDGE_LIBRARY "" CACHE FILEPATH
    "Optional libocpn_portable_runtime_bridge.a for the Wasmtime lifecycle probe")
set(RUNTIME_HOST_PROBE_BRIDGE_INCLUDE_DIR "" CACHE PATH
    "Directory containing ocpn_portable_runtime.h")

find_package(wxWidgets REQUIRED COMPONENTS base core gl)
find_package(OpenGL REQUIRED)
include(${wxWidgets_USE_FILE})

add_library(runtime_host_toolbar_probe_pi MODULE
  action_registry.cpp
  runtime_host_toolbar_probe_pi.cpp
)
set_target_properties(runtime_host_toolbar_probe_pi PROPERTIES
  PREFIX "lib"
  OUTPUT_NAME "runtime_host_toolbar_probe_pi"
)
target_include_directories(runtime_host_toolbar_probe_pi PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}"
  "${OPENCPN_SOURCE_DIR}/include"
)
target_compile_definitions(runtime_host_toolbar_probe_pi PRIVATE
  EXPERIMENT_ICON_DIR="${CMAKE_CURRENT_LIST_DIR}/icons"
)
target_link_libraries(runtime_host_toolbar_probe_pi PRIVATE
  ${wxWidgets_LIBRARIES} OpenGL::GL)

if(RUNTIME_HOST_PROBE_BRIDGE_LIBRARY)
  if(NOT EXISTS "${RUNTIME_HOST_PROBE_BRIDGE_LIBRARY}")
    message(FATAL_ERROR "Runtime bridge not found: ${RUNTIME_HOST_PROBE_BRIDGE_LIBRARY}")
  endif()
  if(NOT EXISTS "${RUNTIME_HOST_PROBE_BRIDGE_INCLUDE_DIR}/ocpn_portable_runtime.h")
    message(FATAL_ERROR "ocpn_portable_runtime.h not found in bridge include directory")
  endif()
  find_package(Threads REQUIRED)
  target_sources(runtime_host_toolbar_probe_pi PRIVATE runtime_bridge_probe.cpp)
  target_include_directories(runtime_host_toolbar_probe_pi PRIVATE
    "${RUNTIME_HOST_PROBE_BRIDGE_INCLUDE_DIR}")
  target_compile_definitions(runtime_host_toolbar_probe_pi PRIVATE
    RUNTIME_HOST_PROBE_WITH_WASMTIME=1)
  target_link_libraries(runtime_host_toolbar_probe_pi PRIVATE
    "${RUNTIME_HOST_PROBE_BRIDGE_LIBRARY}" Threads::Threads ${CMAKE_DL_LIBS})
  if(UNIX AND NOT APPLE)
    target_link_libraries(runtime_host_toolbar_probe_pi PRIVATE m)
  endif()
endif()

enable_testing()
add_executable(action_registry_test
  action_registry.cpp
  tests/action_registry_test.cpp
)
target_include_directories(action_registry_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
add_test(NAME action_registry COMMAND action_registry_test)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(helper_supervisor_probe tests/helper_supervisor_probe.cpp)
  add_test(NAME helper_supervisor COMMAND helper_supervisor_probe)
endif()
