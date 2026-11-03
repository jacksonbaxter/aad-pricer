cmake_minimum_required(VERSION 3.25)

project(aad_pricer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# One value for us and for any subproject, so QuantLib cannot differ from the bench.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(AADP_BUILD_TESTS "Build test targets" ON)
option(AADP_BUILD_BENCHMARKS "Build benchmark targets" ON)
option(AADP_BUILD_FUZZERS "Build libFuzzer targets (fuzz preset only)" OFF)

# Flags live in the presets so subprojects inherit them, which leaves a bare
# `cmake -B` silently without the FP contract. Refuse that here.
if(NOT CMAKE_CXX_FLAGS MATCHES "-ffp-contract=off" OR NOT CMAKE_C_FLAGS MATCHES "-ffp-contract=off")
  message(FATAL_ERROR "-ffp-contract=off is missing from the C or C++ flags. Configure with a "
                      "preset: cmake --preset release")
endif()
string(TOUPPER "${CMAKE_BUILD_TYPE}" _aadp_build_type)
if("${CMAKE_C_FLAGS} ${CMAKE_CXX_FLAGS} ${CMAKE_C_FLAGS_${_aadp_build_type}} ${CMAKE_CXX_FLAGS_${_aadp_build_type}}"
   MATCHES "-ffast-math|-Ofast")
  message(FATAL_ERROR "Fast-math flags are forbidden in this project")
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(AadpSimd)
include(AadpWarnings)
include(AadpDependencies)
include(AadpTargets)

string(STRIP "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_aadp_build_type}}" AADP_BUILD_CXX_FLAGS)
configure_file(cmake/build_info.hpp.in "${AADP_GENERATED_DIR}/build_info.hpp" @ONLY)

enable_testing()

include(AadpSmoke)

# Each area file adds its own targets. Nothing else in the build system changes
# when an area grows.
foreach(_aadp_area IN ITEMS bench oracle data pricer)
  string(TOUPPER "${_aadp_area}" _aadp_area_upper)
  option(AADP_AREA_${_aadp_area_upper} "Build the ${_aadp_area} area" ON)
  if(AADP_AREA_${_aadp_area_upper})
    include(cmake/area_${_aadp_area}.cmake)
  endif()
endforeach()
