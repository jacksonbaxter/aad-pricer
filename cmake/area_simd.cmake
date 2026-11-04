# Targets for the simd area (Phase 3, stage 5a): the pricer's primal on Highway lanes. See
# cmake/AadpTargets.cmake for the helpers. It instantiates the pricer's kernels and model, so it
# needs the pricer area.
#
# The agreement tests carry the label simd, which the CI lane matrix (simd-lanes in
# .github/workflows/ci.yml) runs at every width: HWY_SCALAR 1, SSE2 2, NEON 2 and AVX2 4, and
# AVX-512 8 where the runner has it, build-only and saying so where it does not.
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

# Check 4: the SIMD kernels against the scalar ones on arbitrary inputs, under libFuzzer, ASan and
# UBSan. The fuzz-avx2 preset builds it for AVX2, for the Linux host.
aadp_fuzz(aadp_fuzz_simd_kernels
  SOURCES fuzz/fuzz_simd_kernels.cpp
  LINK aadp_simd
  LABELS simd
  CORPUS fuzz/fuzz_simd_kernels_seeds)

# The SIMD primal against the scalar pricer, at every width, through the harness. Every SIMD
# estimate and kernel output is checked against the scalar's bits before anything is timed.
if(TARGET aadp_bench_harness)
  aadp_bench(aadp_bench_simd
    SOURCES bench/simd_bench.cpp
    LINK aadp_simd aadp_bench_harness
    CUSTOM_MAIN)
endif()
if(AADP_BUILD_TESTS AND TARGET aadp_bench_simd)
  # Every workload and variant at 1/500 of its size: every check must pass. Not a result.
  add_test(NAME aadp_bench_simd_smoke_reduced_size_not_a_result
    COMMAND aadp_bench_simd --aadp-scale=500 --aadp-require-valid --batches=1 --repetitions=1
      --warmup-ms=0 --gb-warmup-s=0.001
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(aadp_bench_simd_smoke_reduced_size_not_a_result PROPERTIES
    LABELS "bench;simd")
endif()
