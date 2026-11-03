# Targets for the pricer area. See cmake/AadpTargets.cmake for the helpers.
#
# src/math/ holds the pricer's own exp, log, sqrt and inverse normal, so results do not depend on
# the host's libm. Header-only.
aadp_library(aadp_math)

aadp_test(aadp_math_exp_log_test
  SOURCES tests/math/exp_log_test.cpp
  LINK aadp_math
  LABELS math)
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
