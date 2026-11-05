# Targets for the svi area (stage 6a): Black-76 implied volatility through Let's Be Rational, raw
# SVI templated on Real, and the static-arbitrage checks. See cmake/AadpTargets.cmake for the
# helpers. aadp_lets_be_rational comes from cmake/AadpDependencies.cmake. The tape test needs the
# pricer area's tape, and the price oracle the oracle area's closed form.
if(TARGET aadp_math AND TARGET aadp_lets_be_rational)
  aadp_library(aadp_svi
    SOURCES src/svi/implied_vol.cpp src/svi/arbitrage.cpp
    LINK aadp_math aadp_lets_be_rational)

  # SVI-1: round trips over a stated grid, error values, and the price against an independent
  # closed form.
  if(TARGET aadp_oracle_lib)
    aadp_test(aadp_svi_implied_vol_test
      SOURCES tests/svi/implied_vol_test.cpp
      LINK aadp_svi aadp_oracle_lib
      LABELS svi)
  endif()
  # Builds only if src/svi/lets_be_rational_api.hpp declares what the archive's header declares.
  aadp_test(aadp_svi_lets_be_rational_api_test
    SOURCES tests/svi/lets_be_rational_api_test.cpp
    LINK aadp_svi
    LABELS svi fast)
  # w, w', w'', g and d+- in double and on the tape.
  if(TARGET aadp_ad)
    aadp_test(aadp_svi_raw_svi_test
      SOURCES tests/svi/raw_svi_test.cpp
      LINK aadp_svi aadp_ad
      LABELS svi)
  endif()
  # The butterfly and calendar checks on known violators and published arbitrage-free slices.
  aadp_test(aadp_svi_arbitrage_test
    SOURCES tests/svi/arbitrage_test.cpp
    LINK aadp_svi
    LABELS svi)
endif()
