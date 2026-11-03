# Targets for the pricer area. See cmake/AadpTargets.cmake for the helpers.
#
# src/math/ holds the pricer's own exp, log, sqrt and inverse normal, so results do not depend on
# the host's libm. Header-only.
aadp_library(aadp_math)

aadp_test(aadp_math_exp_log_test
  SOURCES tests/math/exp_log_test.cpp
  LINK aadp_math
  LABELS math)
aadp_test(aadp_math_host_independence_test
  SOURCES tests/math/host_independence_test.cpp
  LINK aadp_math
  LABELS math fast)
# Compares with the frozen reference in src/rng/, which the oracle area provides.
if(TARGET aadp_rng)
  aadp_test(aadp_math_inverse_normal_test
    SOURCES tests/math/inverse_normal_test.cpp
    LINK aadp_math aadp_rng
    LABELS math)
  # A wider AS241 sample than the committed table: see tools/math_reference/generate.py --survey.
  aadp_tool(aadp_math_survey
    SOURCES tools/math_reference/survey.cpp
    LINK aadp_math aadp_rng)
endif()
if(TARGET aadp_math_exp_log_test)
  # Full sweeps only where the numbers are reported. Everywhere else they are a smoke test.
  target_compile_definitions(aadp_math_exp_log_test PRIVATE
    AADP_SMOKE_SIZE=$<IF:$<CONFIG:Release>,0,1>)
endif()

if(AADP_BUILD_TESTS)
  # The committed coefficients and reference table must be what the generator produces now.
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME aadp_math_reference_current
    COMMAND "${Python3_EXECUTABLE}" -I tools/math_reference/generate.py --check
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(aadp_math_reference_current PROPERTIES LABELS "math")
endif()

# src/pricer/ is the production scalar Monte Carlo pricer. Its kernels and runner are templates;
# the library holds validation and the double entry point. It reads src/rng/philox.hpp and the
# batch statistics of src/oracle/tolerance.hpp, which the oracle area provides.
if(TARGET aadp_rng)
  aadp_library(aadp_pricer
    SOURCES src/pricer/pricer.cpp
    LINK aadp_math aadp_rng)

  aadp_test(aadp_pricer_normal_cdf_test
    SOURCES tests/pricer/normal_cdf_test.cpp
    LINK aadp_pricer aadp_test_support
    LABELS pricer fast)
  aadp_test(aadp_pricer_contract_test
    SOURCES tests/pricer/contract_test.cpp
    LINK aadp_pricer
    LABELS pricer fast)

  # Check 1: path by path against the frozen reference kernels in src/mc/.
  aadp_test(aadp_pricer_path_agreement_test
    SOURCES tests/pricer/path_agreement_test.cpp
    LINK aadp_pricer aadp_test_support
    LABELS pricer)

  foreach(_aadp_sized IN ITEMS aadp_pricer_path_agreement_test)
    if(TARGET ${_aadp_sized})
      # Full size only where the numbers are reported. Everywhere else a smoke test.
      target_compile_definitions(${_aadp_sized} PRIVATE
        AADP_SMOKE_SIZE=$<IF:$<CONFIG:Release>,0,1>)
    endif()
  endforeach()

  if(AADP_BUILD_TESTS)
    add_test(NAME aadp_pricer_normal_cdf_reference_current
      COMMAND "${Python3_EXECUTABLE}" -I tests/pricer/normal_cdf_reference.py --check
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    set_tests_properties(aadp_pricer_normal_cdf_reference_current PROPERTIES LABELS "pricer")
  endif()
endif()
