# The AAD benchmark (Phase 3, stage 3): bump and revalue, the tape and the hand adjoint against
# the scalar primal. See cmake/AadpTargets.cmake for the helpers. It needs the pricer area's tape
# and the bench area's harness, so it comes after both.
if(NOT TARGET aadp_ad)
  return()
endif()

# The workloads, the h study and the in-run checks. The checks state the association bound of
# src/ad/bounds.hpp. Nothing here reads test code.
aadp_library(aadp_aad_bench_lib
  SOURCES
    bench/aad_workloads.cpp
    bench/aad_checks.cpp
    bench/bump_study.cpp
    bench/rss_probe.cpp
  LINK aadp_ad)
target_include_directories(aadp_aad_bench_lib PUBLIC "${PROJECT_SOURCE_DIR}/bench")

aadp_tool(aadp_bump_study
  SOURCES bench/bump_study_main.cpp
  LINK aadp_aad_bench_lib)

if(TARGET aadp_bench_harness)
  aadp_bench(aadp_bench_aad
    SOURCES bench/aad_bench.cpp
    LINK aadp_aad_bench_lib aadp_bench_harness
    CUSTOM_MAIN)
endif()

aadp_test(aadp_aad_bench_unit
  SOURCES tests/bench/aad_bench_test.cpp tests/bench/bump_study_test.cpp
  LINK aadp_aad_bench_lib
  LABELS unit bench)

if(AADP_BUILD_TESTS)
  # The committed halving evidence must choose the committed steps.
  add_test(NAME aadp_bump_study_committed
    COMMAND aadp_bump_study --verify=bench/bump_size_study.csv
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(aadp_bump_study_committed PROPERTIES LABELS "bench;fast")
endif()

if(AADP_BUILD_TESTS AND TARGET aadp_bench_aad)
  # Every workload and variant at 1/500 of its paths, with three worker threads on the threaded
  # workloads, through the harness: every in-run check must pass. Small enough for the tsan preset,
  # which is where the threaded benchmark code is checked. Its numbers are not results.
  add_test(NAME aadp_bench_aad_smoke_reduced_size_not_a_result
    COMMAND aadp_bench_aad --aadp-scale=500 --aadp-threads=3 --aadp-require-valid --batches=1
      --repetitions=1 --warmup-ms=0 --gb-warmup-s=0.001
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(aadp_bench_aad_smoke_reduced_size_not_a_result PROPERTIES
    LABELS "bench;threads;tsan")
endif()
