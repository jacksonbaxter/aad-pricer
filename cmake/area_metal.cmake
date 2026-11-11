# Targets for the metal area (Phase 3, stage 9): the float32 line on the CPU, built everywhere,
# and the Metal GPU backend, built only with AAD_PRICER_ENABLE_METAL. Prices only: AAD does not run
# on the GPU. See NOTES.md, "Stage 9", and cmake/AadpTargets.cmake for the helpers.
#
# AAD_PRICER_ENABLE_METAL defaults to ON when the platform is Apple and an Objective-C++ compiler
# and the Metal and Foundation frameworks are found, and to OFF otherwise. Asking for it where they
# are missing is an error. Off, the backend's targets do not exist and nothing else changes.
if(NOT TARGET aadp_pricer)
  return()
endif()

# The double columns run the production kernels; the float32 kernels are their own source.
aadp_library(aadp_f32
  SOURCES src/f32/cpu_pricer.cpp src/f32/paired.cpp
  LINK aadp_pricer)

# The shared kernels against the CPU's double ones, the host reduction, the CPU line's thread
# independence and the paired statistics. Every build, every host.
aadp_test(aadp_f32_test
  SOURCES tests/f32/float_kernels_test.cpp tests/f32/cpu_pricer_test.cpp
  LINK aadp_f32
  LABELS f32)

set(_aadp_metal_found OFF)
set(_aadp_metal_why "not an Apple platform")
if(APPLE)
  include(CheckLanguage)
  check_language(OBJCXX)
  find_library(AADP_METAL_FRAMEWORK Metal)
  find_library(AADP_FOUNDATION_FRAMEWORK Foundation)
  if(NOT CMAKE_OBJCXX_COMPILER)
    set(_aadp_metal_why "no Objective-C++ compiler")
  elseif(NOT AADP_METAL_FRAMEWORK OR NOT AADP_FOUNDATION_FRAMEWORK)
    set(_aadp_metal_why "the Metal or Foundation framework was not found")
  else()
    set(_aadp_metal_found ON)
    set(_aadp_metal_why "Apple, with Objective-C++ and the Metal framework")
  endif()
endif()
option(AAD_PRICER_ENABLE_METAL "Build the Metal GPU backend (float32 prices only)"
  ${_aadp_metal_found})
if(AAD_PRICER_ENABLE_METAL AND NOT _aadp_metal_found)
  message(FATAL_ERROR "AAD_PRICER_ENABLE_METAL is ON, but ${_aadp_metal_why}")
endif()
if(NOT AAD_PRICER_ENABLE_METAL)
  message(STATUS "Metal backend: off (${_aadp_metal_why})")
  return()
endif()
message(STATUS "Metal backend: on (${_aadp_metal_why})")

enable_language(OBJCXX)
# The .mm compiles with the C++ flags of the preset, sanitizers and -ffp-contract=off included.
set(CMAKE_OBJCXX_FLAGS "${CMAKE_CXX_FLAGS}")
set(CMAKE_OBJCXX_FLAGS_${_aadp_build_type} "${CMAKE_CXX_FLAGS_${_aadp_build_type}}")

# The library source: the shared kernels, then the Metal entry points, in one string. A change to
# either reconfigures, so the string never goes stale.
set(_aadp_metal_inputs
  "${PROJECT_SOURCE_DIR}/src/f32/float_kernels.hpp"
  "${PROJECT_SOURCE_DIR}/src/metal/kernels.metal"
  "${PROJECT_SOURCE_DIR}/src/metal/library_source.cpp.in")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_aadp_metal_inputs})
file(READ "${PROJECT_SOURCE_DIR}/src/f32/float_kernels.hpp" AADP_F32_KERNELS_SOURCE)
file(READ "${PROJECT_SOURCE_DIR}/src/metal/kernels.metal" AADP_METAL_KERNELS_SOURCE)
foreach(_aadp_text IN ITEMS AADP_F32_KERNELS_SOURCE AADP_METAL_KERNELS_SOURCE)
  if(${_aadp_text} MATCHES "\\)aadp_msl\"")
    message(FATAL_ERROR "${_aadp_text} contains the raw string's closing delimiter")
  endif()
endforeach()
configure_file(src/metal/library_source.cpp.in "${AADP_GENERATED_DIR}/metal_library_source.cpp"
  @ONLY)

aadp_library(aadp_metal
  SOURCES src/metal/metal_backend.mm src/metal/probes.cpp
    "${AADP_GENERATED_DIR}/metal_library_source.cpp"
  LINK aadp_f32 "${AADP_METAL_FRAMEWORK}" "${AADP_FOUNDATION_FRAMEWORK}")
set_source_files_properties(src/metal/metal_backend.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

# The GPU checks: Philox known answers and the stream, the uniform, the property kernels, the
# batch split and GPU-2. Each skips with the reason when the host has no Metal device (a CI virtual
# machine), and ci.yml allows that skip by name only where its own probe found no device.
aadp_test(aadp_metal_test
  SOURCES tests/metal/metal_test.cpp
  LINK aadp_metal
  LABELS metal)
if(TARGET aadp_metal_test)
  # Full size only where the numbers are reported. Everywhere else a smoke test.
  target_compile_definitions(aadp_metal_test PRIVATE AADP_SMOKE_SIZE=$<IF:$<CONFIG:Release>,0,1>)
endif()

# Prints the device or that there is none; exits 0 either way. CI's probe step.
aadp_tool(aadp_metal_info
  SOURCES tools/metal_report/info.cpp
  LINK aadp_metal)

# The stage 9 report: properties, batch size, the table and the GPU-1 timing. Iteration only.
if(TARGET aadp_simd)
  aadp_tool(aadp_metal_report
    SOURCES tools/metal_report/main.cpp
    LINK aadp_metal aadp_simd)
endif()
