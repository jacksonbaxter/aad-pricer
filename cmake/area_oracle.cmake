# Targets for the oracle area. See cmake/AadpTargets.cmake for the helpers.
aadp_library(aadp_rng)

aadp_test(aadp_philox_test
  SOURCES tests/oracle/philox_test.cpp
  LINK aadp_rng
  LABELS oracle fast)
aadp_test(aadp_inverse_normal_test
  SOURCES tests/oracle/inverse_normal_test.cpp
  LINK aadp_rng
  LABELS oracle fast)

# dual.hpp is a test stand-in, so it reaches src-side code only through this target.
aadp_library(aadp_test_support)
target_include_directories(aadp_test_support INTERFACE "${PROJECT_SOURCE_DIR}/tests")

aadp_test(aadp_dual_test
  SOURCES tests/oracle/dual_test.cpp
  LINK aadp_rng aadp_test_support
  LABELS oracle fast)
aadp_test(aadp_closed_form_test
  SOURCES tests/oracle/closed_form_test.cpp
  LINK aadp_rng aadp_test_support
  LABELS oracle fast)
aadp_test(aadp_tolerance_test
  SOURCES tests/oracle/tolerance_test.cpp
  LABELS oracle fast)
aadp_test(aadp_kernels_test
  SOURCES tests/oracle/kernels_test.cpp
  LINK aadp_rng aadp_test_support
  LABELS oracle fast)
aadp_test(aadp_thread_invariance_test
  SOURCES tests/oracle/thread_invariance_test.cpp
  LINK aadp_rng aadp_test_support
  LABELS oracle fast threads)

aadp_library(aadp_quadrature_lib
  SOURCES tools/quadrature/quadrature.cpp
  LINK aadp_rng aadp_test_support)
target_include_directories(aadp_quadrature_lib PUBLIC "${PROJECT_SOURCE_DIR}/tools/quadrature")
aadp_tool(aadp_quadrature
  SOURCES tools/quadrature/main.cpp
  LINK aadp_quadrature_lib)
if(AADP_BUILD_TESTS)
  add_test(NAME aadp_oracle_values_current
    COMMAND aadp_quadrature --check references/oracle_values.csv
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(aadp_oracle_values_current PROPERTIES LABELS "oracle")
endif()

aadp_library(aadp_oracle_lib
  SOURCES src/oracle/plan.cpp src/oracle/references.cpp
  LINK aadp_rng)

aadp_test(aadp_quadrature_test
  SOURCES tests/oracle/quadrature_test.cpp
  LINK aadp_quadrature_lib
  LABELS oracle)
aadp_test(aadp_decoder_test
  SOURCES tests/oracle/decoder_test.cpp
  LINK aadp_oracle_lib
  LABELS oracle fast)

aadp_library(aadp_run_oracles_lib
  SOURCES tools/run_oracles/runner.cpp
  LINK aadp_oracle_lib aadp_test_support)
target_include_directories(aadp_run_oracles_lib PUBLIC "${PROJECT_SOURCE_DIR}/tools/run_oracles")
aadp_tool(aadp_run_oracles
  SOURCES tools/run_oracles/main.cpp
  LINK aadp_run_oracles_lib)

if(AADP_BUILD_TESTS AND TARGET aadp_run_oracles)
  # The plan runs in full only where it is evidence. Its seed is fixed, so repeating this reads the
  # same result again and is not another look at the family-wise error rate.
  set(_aadp_plan_arguments run
    --plan references/comparison_plan.csv
    --references references/oracle_values.csv
    --threads 8)
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_test(NAME aadp_oracle_plan_measured
      COMMAND aadp_run_oracles ${_aadp_plan_arguments}
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    set_tests_properties(aadp_oracle_plan_measured PROPERTIES
      LABELS "oracle" TIMEOUT 900 RUN_SERIAL TRUE)
  else()
    add_test(NAME aadp_oracle_plan_smoke_reduced_size_not_a_result
      COMMAND aadp_run_oracles ${_aadp_plan_arguments} --smoke
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    set_tests_properties(aadp_oracle_plan_smoke_reduced_size_not_a_result PROPERTIES
      LABELS "oracle;smoke")
  endif()
endif()

aadp_fuzz(aadp_fuzz_plan
  SOURCES fuzz/fuzz_plan.cpp
  LINK aadp_oracle_lib
  LABELS oracle
  CORPUS fuzz/fuzz_plan_seeds)
aadp_fuzz(aadp_fuzz_references
  SOURCES fuzz/fuzz_references.cpp
  LINK aadp_oracle_lib
  LABELS oracle
  CORPUS fuzz/fuzz_references_seeds)

aadp_test(aadp_runner_test
  SOURCES tests/oracle/runner_test.cpp
  LINK aadp_run_oracles_lib
  LABELS oracle fast)
aadp_test(aadp_naive_barrier_test
  SOURCES tests/oracle/naive_barrier_test.cpp
  LINK aadp_run_oracles_lib
  LABELS oracle)
if(TARGET aadp_naive_barrier_test)
  # Full size only where it is meant as evidence. Everywhere else it is a smoke test.
  target_compile_definitions(aadp_naive_barrier_test PRIVATE
    AADP_SMOKE_SIZE=$<IF:$<CONFIG:Release>,0,1>)
endif()
