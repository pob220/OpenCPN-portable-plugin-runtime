cmake_minimum_required(VERSION 3.16)
project(runtime_host_toolbar_probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

set(OPENCPN_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "OpenCPN source tree containing include/ocpn_plugin.h")

find_package(wxWidgets REQUIRED COMPONENTS base core gl)
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
target_link_libraries(runtime_host_toolbar_probe_pi PRIVATE ${wxWidgets_LIBRARIES})

enable_testing()
add_executable(action_registry_test
  action_registry.cpp
  tests/action_registry_test.cpp
)
target_include_directories(action_registry_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
add_test(NAME action_registry COMMAND action_registry_test)
