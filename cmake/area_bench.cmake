# Targets for the bench area. See cmake/AadpTargets.cmake for the helpers.
aadp_library(aadp_platform
  SOURCES
    src/platform/affinity.cpp
    src/platform/cpu_info.cpp
    src/platform/fp_state.cpp
    src/platform/host_label.cpp
    src/platform/memory_info.cpp
    src/platform/perf_counters.cpp
    src/platform/simd_info.cpp
    src/platform/timer.cpp
  LINK hwy)

aadp_test(aadp_platform_unit
  SOURCES tests/bench/platform_test.cpp tests/bench/timer_test.cpp
  LINK aadp_platform)

aadp_library(aadp_results
  SOURCES
    src/results/json_lite.cpp
    src/results/measure_line.cpp
    src/results/privacy.cpp
    src/results/provenance.cpp
    src/results/results_file.cpp
    src/results/stamp.cpp
    src/results/table.cpp)

set(_aadp_bench_scratch "${PROJECT_BINARY_DIR}/bench-test-scratch")

aadp_test(aadp_results_unit
  SOURCES
    tests/bench/json_test.cpp
    tests/bench/measure_line_test.cpp
    tests/bench/privacy_test.cpp
    tests/bench/provenance_test.cpp
    tests/bench/results_file_test.cpp
    tests/bench/stamp_test.cpp
    tests/bench/table_test.cpp
  LINK aadp_results)

if(TARGET aadp_results_unit)
  target_compile_definitions(aadp_results_unit PRIVATE
    AADP_TEST_SCRATCH="${_aadp_bench_scratch}/results")
endif()

# Tests use the histogram gate, so this library exists whenever either kind is on.
if(AADP_BUILD_TESTS OR AADP_BUILD_BENCHMARKS)
  aadp_library(aadp_bench_harness
    SOURCES bench/harness.cpp bench/latency.cpp
    LINK aadp_platform aadp_results benchmark::benchmark
         hdr_histogram::hdr_histogram_core_static)
  target_include_directories(aadp_bench_harness PUBLIC "${PROJECT_SOURCE_DIR}/bench")
endif()

aadp_bench(aadp_harness_selftest
  SOURCES bench/harness_selftest.cpp
  LINK aadp_bench_harness
  CUSTOM_MAIN)

aadp_test(aadp_bench_unit
  SOURCES tests/bench/histogram_test.cpp tests/bench/options_test.cpp
  LINK aadp_bench_harness)
