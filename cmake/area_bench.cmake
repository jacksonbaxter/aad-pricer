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

aadp_tool(aadp_results_stamp
  SOURCES tools/results_stamp/main.cpp
  LINK aadp_results aadp_platform)

aadp_tool(aadp_results_table
  SOURCES tools/results_table/main.cpp
  LINK aadp_results)

aadp_test(aadp_bench_cli
  SOURCES tests/bench/cli_test.cpp
  LINK aadp_results
  LABELS unit cli)

if(TARGET aadp_bench_cli)
  target_compile_definitions(aadp_bench_cli PRIVATE
    AADP_TEST_SCRATCH="${_aadp_bench_scratch}/cli"
    AADP_SELFTEST_BIN="$<TARGET_FILE:aadp_harness_selftest>"
    AADP_STAMP_BIN="$<TARGET_FILE:aadp_results_stamp>"
    AADP_TABLE_BIN="$<TARGET_FILE:aadp_results_table>")
  add_dependencies(aadp_bench_cli aadp_harness_selftest aadp_results_stamp aadp_results_table)

  add_test(NAME aadp_run_bench_script
    COMMAND bash "${PROJECT_SOURCE_DIR}/tests/bench/run_bench_test.sh"
      "$<TARGET_FILE:aadp_harness_selftest>" "${_aadp_bench_scratch}/script"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(aadp_run_bench_script PROPERTIES LABELS "unit;cli")
endif()

aadp_fuzz(fuzz_measure_line
  SOURCES fuzz/fuzz_measure_line.cpp
  LINK aadp_results
  CORPUS fuzz/fuzz_measure_line_seeds)

aadp_fuzz(fuzz_results_file
  SOURCES fuzz/fuzz_results_file.cpp
  LINK aadp_results
  CORPUS fuzz/fuzz_results_file_seeds)

aadp_fuzz(fuzz_results_json
  SOURCES fuzz/fuzz_results_json.cpp
  LINK aadp_results
  CORPUS fuzz/fuzz_results_json_seeds)
