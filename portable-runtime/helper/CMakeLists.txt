cmake_minimum_required(VERSION 3.20)
project(opencpn_portable_environment_helpers LANGUAGES CXX)

include(GNUInstallDirs)
include(CTest)

set(OCPN_PORTABLE_HELPER_TARGET "" CACHE STRING
    "Exact portable helper target directory")
if (NOT OCPN_PORTABLE_HELPER_TARGET MATCHES
    "^(linux-gnu-(x86_64|aarch64)|flatpak-(x86_64|aarch64)|macos-(x86_64|aarch64)|windows-x86_64)$")
  message(FATAL_ERROR "Set OCPN_PORTABLE_HELPER_TARGET to a supported target")
endif ()

if (WIN32)
  find_package(eccodes CONFIG REQUIRED)
  find_package(jsoncpp CONFIG REQUIRED)
  set(_decoder_libraries eccodes JsonCpp::JsonCpp)
else ()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(ECCODES REQUIRED IMPORTED_TARGET eccodes)
  pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)
  set(_decoder_libraries PkgConfig::ECCODES PkgConfig::JSONCPP)
endif ()

add_executable(igrib-environment-helper igrib_environment_helper.cpp)
target_compile_features(igrib-environment-helper PRIVATE cxx_std_17)
target_link_libraries(igrib-environment-helper PRIVATE ${_decoder_libraries})
if (MSVC)
  target_compile_options(igrib-environment-helper PRIVATE /W4)
else ()
  target_compile_options(igrib-environment-helper PRIVATE
                         -Wall -Wextra -Wpedantic)
endif ()

add_subdirectory(../vendor/environmental-grib-generator
                 environmental-grib-generator)

set(_payload "helpers/${OCPN_PORTABLE_HELPER_TARGET}")
if (OCPN_PORTABLE_HELPER_TARGET MATCHES "^flatpak-")
  set_target_properties(igrib-environment-helper environmental-grib PROPERTIES
                        INSTALL_RPATH "$ORIGIN/lib")
  # Use DT_RPATH rather than DT_RUNPATH for this sealed helper payload.  The
  # former is inherited while resolving dependencies of bundled libraries;
  # without it, a library such as NetCDF can be found beside the executable
  # while its HDF5 dependency is incorrectly searched only in the Flatpak SDK.
  # The path is fixed relative to the signed target directory, never supplied
  # by the plugin or inherited from the user's environment.
  target_link_options(igrib-environment-helper PRIVATE
                      "-Wl,--disable-new-dtags")
  target_link_options(environmental-grib PRIVATE "-Wl,--disable-new-dtags")
endif ()

if (WIN32 OR OCPN_PORTABLE_HELPER_TARGET MATCHES "^flatpak-")
  install(TARGETS igrib-environment-helper environmental-grib
          RUNTIME_DEPENDENCY_SET portable_environment_helper_dependencies
          RUNTIME DESTINATION "${_payload}")
else ()
  install(TARGETS igrib-environment-helper environmental-grib
          RUNTIME DESTINATION "${_payload}")
endif ()

if (WIN32)
  install(RUNTIME_DEPENDENCY_SET portable_environment_helper_dependencies
          DESTINATION "${_payload}"
          DIRECTORIES "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin"
          PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*"
          POST_EXCLUDE_REGEXES ".*[Ww]indows[/\\\\][Ss]ystem32.*")
elseif (OCPN_PORTABLE_HELPER_TARGET MATCHES "^flatpak-")
  install(RUNTIME_DEPENDENCY_SET portable_environment_helper_dependencies
          DESTINATION "${_payload}/lib"
          POST_EXCLUDE_REGEXES
            "^/(usr/)?lib.*/ld-linux.*"
            "^/(usr/)?lib.*/lib(c|m|dl|pthread|rt|resolv|util)\\.so.*"
            "^/usr/lib.*/libSystem\\.B\\.dylib$")
endif ()

if (WIN32 OR OCPN_PORTABLE_HELPER_TARGET MATCHES "^flatpak-")
  find_program(ECCODES_INFO_EXECUTABLE NAMES codes_info REQUIRED)
  execute_process(COMMAND "${ECCODES_INFO_EXECUTABLE}" -d
                  OUTPUT_VARIABLE ECCODES_DEFINITION_DIRECTORY
                  OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  execute_process(COMMAND "${ECCODES_INFO_EXECUTABLE}" -s
                  OUTPUT_VARIABLE ECCODES_SAMPLE_DIRECTORY
                  OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  install(DIRECTORY "${ECCODES_DEFINITION_DIRECTORY}/"
          DESTINATION "${_payload}/share/eccodes/definitions"
          PATTERN "bufr" EXCLUDE PATTERN "grib3" EXCLUDE
          PATTERN "metar" EXCLUDE PATTERN "taf" EXCLUDE
          PATTERN "cdf" EXCLUDE PATTERN "gts" EXCLUDE
          PATTERN "hdf5" EXCLUDE PATTERN "tide" EXCLUDE
          PATTERN "wrap" EXCLUDE PATTERN "diag" EXCLUDE
          PATTERN "budg" EXCLUDE)
  install(DIRECTORY "${ECCODES_SAMPLE_DIRECTORY}/"
          DESTINATION "${_payload}/share/eccodes/samples")
endif ()

# InstallRequiredSystemLibraries is used by the generator on MSVC for the
# OpenMP/runtime DLLs which its dependency graph requires. Keep these beside
# the helper, never in OpenCPN's native plugin directory.
if (WIN32 AND CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
  install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
          DESTINATION "${_payload}")
endif ()
