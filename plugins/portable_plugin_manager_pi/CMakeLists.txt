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

add_library(portable_plugin_manager_pi MODULE
  src/action_registry.cpp
  src/manager_dialog.cpp
  src/portable_plugin_manager_pi.cpp
)
set_target_properties(portable_plugin_manager_pi PROPERTIES
  PREFIX "lib"
  OUTPUT_NAME "portable_plugin_manager_pi"
)
target_include_directories(portable_plugin_manager_pi PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/include"
  "${OPENCPN_SOURCE_DIR}/include"
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
target_link_libraries(portable_plugin_manager_pi PRIVATE ${wxWidgets_LIBRARIES})

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
endif()
