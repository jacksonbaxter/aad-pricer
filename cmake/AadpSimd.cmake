# AADP_SIMD_TARGET picks one Highway target per build. Flags go into the C and C++
# flags rather than onto targets so dependencies, and later QuantLib, see the same ISA.

set(AADP_SIMD_TARGET baseline CACHE STRING "Highway static target: baseline, scalar, avx2 or avx512")
set_property(CACHE AADP_SIMD_TARGET PROPERTY STRINGS baseline scalar avx2 avx512)

set(_aadp_simd_flags "-DHWY_COMPILE_ONLY_STATIC")
if(AADP_SIMD_TARGET STREQUAL "baseline")
  # NEON on arm64, SSE2 on x86-64.
elseif(AADP_SIMD_TARGET STREQUAL "scalar")
  # Highway rejects HWY_COMPILE_ONLY_STATIC together with HWY_COMPILE_ONLY_SCALAR.
  set(_aadp_simd_flags "-DHWY_COMPILE_ONLY_SCALAR")
elseif(AADP_SIMD_TARGET MATCHES "^avx(2|512)$")
  if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    message(FATAL_ERROR "AADP_SIMD_TARGET=${AADP_SIMD_TARGET} needs x86-64, "
                        "not ${CMAKE_SYSTEM_PROCESSOR}")
  endif()
  # Highway's AVX2 and AVX-512 baselines also require AES and PCLMUL, which the
  # x86-64-v3 and v4 levels leave out. Without them it silently falls back to SSE2.
  if(AADP_SIMD_TARGET STREQUAL "avx2")
    string(APPEND _aadp_simd_flags " -march=x86-64-v3 -maes -mpclmul")
  else()
    string(APPEND _aadp_simd_flags " -march=x86-64-v4 -maes -mpclmul")
  endif()
else()
  message(FATAL_ERROR "AADP_SIMD_TARGET must be baseline, scalar, avx2 or avx512, "
                      "not '${AADP_SIMD_TARGET}'")
endif()

string(APPEND CMAKE_C_FLAGS " ${_aadp_simd_flags}")
string(APPEND CMAKE_CXX_FLAGS " ${_aadp_simd_flags}")
