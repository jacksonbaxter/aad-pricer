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
