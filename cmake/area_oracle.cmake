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
