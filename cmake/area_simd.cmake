# Targets for the simd area (Phase 3, stage 5a): the pricer's primal on Highway lanes. See
# cmake/AadpTargets.cmake for the helpers. It instantiates the pricer's kernels and model, so it
# needs the pricer area.
#
# The agreement tests carry the label simd, which the CI lane matrix (simd-lanes in
# .github/workflows/ci.yml) runs at every width: HWY_SCALAR 1, SSE2 2, NEON 2 and AVX2 4, and
# AVX-512 8, natively where the runner has x86-64-v4 and otherwise under Intel SDE as a Skylake-X
# (-skx). Neither development host can run AVX-512, so there it is build-only.
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

# Stage 5b: the vector tape, gradients on lanes. It records onto the pricer's kernels on lanes and
# keeps the set-up on the scalar tape, so it needs the ad library too.
if(TARGET aadp_ad)
  aadp_library(aadp_simd_ad
    SOURCES src/simd/lane_gradient.cpp
    LINK aadp_simd aadp_ad)
  # B16 and the dot product on lanes: random programs against one scalar sweep per lane.
  aadp_test(aadp_simd_lane_tape_test
    SOURCES tests/simd/lane_tape_test.cpp
    LINK aadp_simd_ad aadp_test_support
    LABELS simd ad)
  # Per path, block sums on 1, 2, 3 and 8 threads, the dot product on priced paths and the
  # counted fallback, all against the scalar tape, bit for bit.
  aadp_test(aadp_simd_lane_gradient_test
    SOURCES tests/simd/lane_gradient_test.cpp
    LINK aadp_simd_ad aadp_test_support
    LABELS simd ad threads tsan)
  # Tape bytes per path at every width, beside the scalar tape's.
  aadp_test(aadp_simd_lane_tape_memory_test
    SOURCES tests/simd/lane_tape_memory_test.cpp
    LINK aadp_simd_ad aadp_test_support
    LABELS simd ad)
  foreach(_aadp_sized IN ITEMS aadp_simd_lane_tape_test aadp_simd_lane_gradient_test)
    if(TARGET ${_aadp_sized})
      target_compile_definitions(${_aadp_sized} PRIVATE
        AADP_SMOKE_SIZE=$<IF:$<CONFIG:Release>,0,1>)
    endif()
  endforeach()
  # The vector tape against the scalar tape on arbitrary contracts, under libFuzzer, ASan and
  # UBSan. Same input layout and seed corpus as aadp_fuzz_pricer. fuzz-avx2 builds it for AVX2.
  aadp_fuzz(aadp_fuzz_lane_gradient
    SOURCES fuzz/fuzz_lane_gradient.cpp
    LINK aadp_simd_ad
    LABELS simd ad
    CORPUS fuzz/fuzz_pricer_seeds)
  # MEM-6 on lanes, in the allocation test's own binary (cmake/area_pricer.cmake says which
  # presets build it).
  if(TARGET aadp_ad_allocation_test)
    target_sources(aadp_ad_allocation_test PRIVATE tests/simd/lane_allocation_test.cpp)
    target_link_libraries(aadp_ad_allocation_test PRIVATE aadp_simd_ad)
  endif()
endif()

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

# Test-side, not a product benchmark: what a fallback to the scalar tape costs, through the
# test-only diverging kernel (tests/simd/diverging_kernel.hpp). Iteration only; never --reportable.
if(AADP_BUILD_TESTS AND TARGET aadp_bench_harness AND TARGET aadp_simd_ad)
  aadp_bench(aadp_simd_fallback_cost
    SOURCES tests/simd/fallback_cost_bench.cpp
    LINK aadp_simd_ad aadp_test_support aadp_bench_harness
    CUSTOM_MAIN)
endif()
if(TARGET aadp_simd_fallback_cost)
  add_test(NAME aadp_simd_fallback_cost_smoke_not_a_result
    COMMAND aadp_simd_fallback_cost --aadp-scale=32 --aadp-require-valid --batches=1
      --repetitions=1 --warmup-ms=0 --gb-warmup-s=0.001
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(aadp_simd_fallback_cost_smoke_not_a_result PROPERTIES
    LABELS "bench;simd")
endif()
