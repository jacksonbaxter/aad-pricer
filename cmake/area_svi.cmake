# Targets for the svi area (stage 6a): Black-76 implied volatility through Let's Be Rational, raw
# SVI templated on Real, and the static-arbitrage checks. See cmake/AadpTargets.cmake for the
# helpers. aadp_lets_be_rational comes from cmake/AadpDependencies.cmake. The tape test needs the
# pricer area's tape, and the price oracle the oracle area's closed form.
if(TARGET aadp_math AND TARGET aadp_lets_be_rational)
  aadp_library(aadp_svi
    SOURCES src/svi/implied_vol.cpp src/svi/arbitrage.cpp src/svi/fit.cpp
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

  # The raw SVI fit on hand-made slices: recovery, rho at its bound, no repair, metrics, errors.
  aadp_test(aadp_svi_fit_test
    SOURCES tests/svi/fit_test.cpp
    LINK aadp_svi
    LABELS svi)

  # The inversion and every check on raw bytes: no crash, no UB, outputs valid or named errors.
  aadp_fuzz(aadp_fuzz_svi
    SOURCES fuzz/fuzz_svi.cpp
    LINK aadp_svi
    LABELS svi
    CORPUS fuzz/fuzz_svi_seeds)

  # The fit on small quote sets from raw bytes: no crash, no exception, errors as values.
  aadp_fuzz(aadp_fuzz_svi_fit
    SOURCES fuzz/fuzz_svi_fit.cpp
    LINK aadp_svi
    LABELS svi
    CORPUS fuzz/fuzz_svi_fit_seeds)

  # Stage 6b: the TAIFEX chain into the fit, and the fit report tool. Needs the data area.
  if(TARGET aadp_data)
    aadp_library(aadp_svi_chain
      SOURCES src/svi/chain_fit.cpp
      LINK aadp_svi aadp_data)
    aadp_test(aadp_svi_chain_fit_test
      SOURCES tests/svi/chain_fit_test.cpp
      LINK aadp_svi_chain
      LABELS svi)
    # Skips, visibly, when data/ has no capture. See scripts/fetch_data.sh.
    aadp_test(aadp_svi_chain_realdata_test
      SOURCES tests/svi/chain_realdata_test.cpp
      LINK aadp_svi_chain
      LABELS svi realdata)
    aadp_tool(aadp_svi_fit
      SOURCES tools/svi_fit/main.cpp
      LINK aadp_svi_chain)
    aadp_test(aadp_svi_fit_tool_test
      SOURCES tests/svi/svi_fit_tool_test.cpp
      LINK aadp_svi_chain
      LABELS svi)
    if(AADP_BUILD_TESTS AND TARGET aadp_svi_fit_tool_test)
      add_dependencies(aadp_svi_fit_tool_test aadp_svi_fit)
      target_compile_definitions(aadp_svi_fit_tool_test PRIVATE
        AADP_SVI_FIT_PATH="$<TARGET_FILE:aadp_svi_fit>"
        AADP_SVI_FIT_TEST_SCRATCH="${PROJECT_BINARY_DIR}/svi_test_scratch/svi_fit")
    endif()
  endif()

  # SVI-2, iteration only: no measurement plan or results file yet.
  aadp_bench(aadp_bench_implied_vol
    SOURCES bench/implied_vol_bench.cpp
    LINK aadp_svi)
  if(AADP_BUILD_TESTS AND TARGET aadp_bench_implied_vol)
    # Every benchmark once, briefly: each quote must invert before the timing starts. Not a result.
    add_test(NAME aadp_bench_implied_vol_smoke_not_a_result
      COMMAND aadp_bench_implied_vol --benchmark_min_time=0.001s)
    set_tests_properties(aadp_bench_implied_vol_smoke_not_a_result PROPERTIES
      LABELS "bench;svi"
      FAIL_REGULAR_EXPRESSION "ERROR OCCURRED")
  endif()
endif()
