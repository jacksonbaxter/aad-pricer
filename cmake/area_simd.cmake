# Targets for the simd area (Phase 3, stage 5a): the pricer's primal on Highway lanes. See
# cmake/AadpTargets.cmake for the helpers. It instantiates the pricer's kernels and model, so it
# needs the pricer area.
#
# The agreement tests carry the label simd, which the CI lane matrix runs on every width that can
# run (.github/workflows/ci.yml): NEON on the macOS jobs, AVX2 on linux-avx2, AVX-512 where the
# runner has it.
if(NOT TARGET aadp_pricer)
  return()
endif()

aadp_library(aadp_simd
  SOURCES src/simd/simd_pricer.cpp
  LINK aadp_pricer hwy)

# Check 1: each math kernel against its scalar twin, bit for bit.
aadp_test(aadp_simd_math_agreement_test
  SOURCES tests/simd/math_agreement_test.cpp
  LINK aadp_simd
  LABELS simd)
# Check 2: Philox known answers on lanes, and lane i drawing scalar path i's draws.
aadp_test(aadp_simd_philox_lanes_test
  SOURCES tests/simd/philox_lanes_test.cpp
  LINK aadp_simd
  LABELS simd fast)
# Check 1, path by path, on every product and treatment.
aadp_test(aadp_simd_path_agreement_test
  SOURCES tests/simd/path_agreement_test.cpp
  LINK aadp_simd
  LABELS simd)
# Check 3: prices against Price() for every width and 1, 2, 3 and 8 threads.
aadp_test(aadp_simd_price_agreement_test
  SOURCES tests/simd/price_agreement_test.cpp
  LINK aadp_simd
  LABELS simd threads)

foreach(_aadp_sized IN ITEMS aadp_simd_math_agreement_test aadp_simd_path_agreement_test
                             aadp_simd_price_agreement_test)
  if(TARGET ${_aadp_sized})
    # Full size only where the numbers are reported. Everywhere else a smoke test.
    target_compile_definitions(${_aadp_sized} PRIVATE
      AADP_SMOKE_SIZE=$<IF:$<CONFIG:Release>,0,1>)
  endif()
endforeach()
