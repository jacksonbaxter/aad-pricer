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

  # Stage 7a: the slices joined in time, Dupire local variance with the floor rule, the
  # local-volatility pricer and its gradient in the slice parameters (docs/NOTES.md, "Stage 7a").
  # Header-only templates on Real, with their double parts here; no Let's Be Rational needed.
  aadp_library(aadp_svi_surface
    SOURCES src/svi/surface.cpp src/svi/local_vol.cpp
    LINK aadp_math)
  if(TARGET aadp_ad)
    # Interpolation, the curves and refusal beyond the last slice, in double and on the tape.
    aadp_test(aadp_svi_surface_test
      SOURCES tests/svi/surface_test.cpp
      LINK aadp_svi_surface aadp_ad aadp_test_support
      LABELS svi)
    # The floor rule, and the Dupire check: finite differences of dT w and g, and Dupire in call
    # prices through Let's Be Rational.
    aadp_test(aadp_svi_local_vol_test
      SOURCES tests/svi/local_vol_test.cpp
      LINK aadp_svi_surface aadp_ad aadp_svi aadp_test_support
      LABELS svi)
  endif()
  if(TARGET aadp_pricer AND TARGET aadp_ad)
    aadp_library(aadp_local_vol
      SOURCES src/pricer/local_vol.cpp
      LINK aadp_pricer aadp_svi_surface)
    aadp_library(aadp_local_vol_ad
      SOURCES src/ad/local_vol_gradient.cpp
      LINK aadp_ad aadp_local_vol)
    # The tape's statement order on GBM, local-vol and conditional paths, one pinned constant for
    # every host and compiler (docs/NOTES.md, "Stage 11").
    aadp_test(aadp_ad_statement_order_test
      SOURCES tests/ad/statement_order_test.cpp
      LINK aadp_local_vol_ad
      LABELS ad svi fast)

    # The sub-stepped kernels against EvaluatePath and a hand loop, the flat-surface oracle
    # against GBM, validation, and the same bits for any thread count.
    aadp_test(aadp_pricer_local_vol_test
      SOURCES tests/pricer/local_vol_test.cpp
      LINK aadp_local_vol aadp_test_support
      LABELS pricer svi)
    # Per path, the tape's 15 parameter derivatives against the forward-mode Dual.
    aadp_test(aadp_ad_local_vol_gradient_test
      SOURCES tests/ad/local_vol_gradient_test.cpp
      LINK aadp_local_vol_ad aadp_test_support
      LABELS ad svi)
    # The surface entry point and a short path on the tape, from raw bytes.
    aadp_fuzz(aadp_fuzz_local_vol
      SOURCES fuzz/fuzz_local_vol.cpp
      LINK aadp_local_vol_ad aadp_test_support
      LABELS svi
      CORPUS fuzz/fuzz_local_vol_seeds)
  endif()
  # The local-volatility pricer on the fitted chain, and its report tool. Needs the data area.
  if(TARGET aadp_svi_chain AND TARGET aadp_local_vol_ad)
    # Skips, visibly, when data/ has no capture. See scripts/fetch_data.sh.
    aadp_test(aadp_svi_local_vol_realdata_test
      SOURCES tests/svi/local_vol_realdata_test.cpp
      LINK aadp_svi_chain aadp_local_vol_ad aadp_test_support
      LABELS svi realdata)
    aadp_tool(aadp_local_vol_report
      SOURCES tools/local_vol_report/main.cpp
      LINK aadp_svi_chain aadp_local_vol_ad)
    aadp_test(aadp_svi_local_vol_report_tool_test
      SOURCES tests/svi/local_vol_report_tool_test.cpp
      LINK aadp_svi_chain
      LABELS svi)
    if(AADP_BUILD_TESTS AND TARGET aadp_svi_local_vol_report_tool_test)
      add_dependencies(aadp_svi_local_vol_report_tool_test aadp_local_vol_report)
      target_compile_definitions(aadp_svi_local_vol_report_tool_test PRIVATE
        AADP_LOCAL_VOL_REPORT_PATH="$<TARGET_FILE:aadp_local_vol_report>"
        AADP_LOCAL_VOL_REPORT_TEST_SCRATCH="${PROJECT_BINARY_DIR}/svi_test_scratch/local_vol_report")
    endif()
  endif()

  # Stage 8: the comparison plan's runner with the production estimator beside the reference one,
  # the finite-difference ladder of the surface rows, and the run-2 fingerprint (docs/NOTES.md,
  # "Stage 8").
  if(TARGET aadp_svi_chain AND TARGET aadp_local_vol_ad AND TARGET aadp_run_oracles_lib)
    aadp_library(aadp_run_oracles_stage8
      SOURCES tools/run_oracles/ladder.cpp tools/run_oracles/production.cpp
      LINK aadp_run_oracles_lib aadp_svi_chain aadp_local_vol_ad aadp_ad)
    aadp_tool(aadp_run_oracles
      SOURCES tools/run_oracles/main.cpp
      LINK aadp_run_oracles_stage8)

    if(AADP_BUILD_TESTS)
      # The plan runs in full only where it is evidence. Its seed is fixed, so repeating this reads
      # the same result again and is not another look at the family-wise error rate. The reference
      # estimator judges run 1's 90 rows and lists the other 57.
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
      # Run 2's curation, on a synthetic runner output.
      add_test(NAME aadp_compare_measured_curation
        COMMAND "${Python3_EXECUTABLE}" -I tests/oracle/compare_measured_test.py
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
      set_tests_properties(aadp_compare_measured_curation PROPERTIES LABELS "oracle")
    endif()

    aadp_test(aadp_knock_in_rows_test
      SOURCES tests/oracle/knock_in_rows_test.cpp
      LINK aadp_run_oracles_stage8 aadp_test_support
      LABELS oracle)
    aadp_test(aadp_fd_ladder_test
      SOURCES tests/oracle/fd_ladder_test.cpp
      LINK aadp_run_oracles_stage8 aadp_test_support
      LABELS oracle svi)
    aadp_test(aadp_svi_rows_test
      SOURCES tests/svi/svi_rows_test.cpp
      LINK aadp_run_oracles_stage8 aadp_test_support
      LABELS svi)
    aadp_test(aadp_stage8_plan_test
      SOURCES tests/oracle/stage8_plan_test.cpp
      LINK aadp_run_oracles_stage8 aadp_test_support
      LABELS oracle)
    # Skips, visibly, when data/ has no capture.
    aadp_test(aadp_svi_rows_realdata_test
      SOURCES tests/svi/svi_rows_realdata_test.cpp
      LINK aadp_run_oracles_stage8 aadp_test_support
      LABELS svi realdata)
    foreach(_aadp_sized IN ITEMS aadp_knock_in_rows_test aadp_svi_rows_test)
      if(TARGET ${_aadp_sized})
        # Full size only in release. Everywhere else the rows only have to run and stay finite.
        target_compile_definitions(${_aadp_sized} PRIVATE
          AADP_SMOKE_SIZE=$<IF:$<CONFIG:Release>,0,1>)
      endif()
    endforeach()
    if(CMAKE_BUILD_TYPE STREQUAL "Release" AND TARGET aadp_platform)
      # Run 2's code at the development seed, bit for bit against this host's file.
      aadp_test(aadp_run2_fingerprint
        SOURCES tests/oracle/run2_fingerprint_test.cpp
        LINK aadp_run_oracles_stage8 aadp_platform
        LABELS oracle)
    endif()
  endif()

  # Stage 7b: quote sensitivities through the fit by the implicit function theorem, on the
  # second-order tape (docs/NOTES.md, "Stage 7b").
  if(TARGET aadp_ad)
    aadp_library(aadp_svi_adjoint
      SOURCES src/svi/calibration_adjoint.cpp
      LINK aadp_svi aadp_ad)
    # The KKT / implicit-function entry point on small slices from raw bytes: errors as values,
    # finite results, and every solution solving its KKT system.
    aadp_fuzz(aadp_fuzz_calibration_adjoint
      SOURCES fuzz/fuzz_calibration_adjoint.cpp
      LINK aadp_svi_adjoint
      LABELS svi
      CORPUS fuzz/fuzz_calibration_adjoint_seeds)
  endif()

  # Stage 7b on the fitted chain: the parity regression differentiated, both chains, the bump
  # check's pieces, and the report tool, which prices off the surface. Needs the data area.
  if(TARGET aadp_svi_adjoint AND TARGET aadp_svi_chain AND TARGET aadp_local_vol_ad)
    aadp_library(aadp_svi_chain_adjoint
      SOURCES src/svi/chain_adjoint.cpp
      LINK aadp_svi_chain aadp_svi_adjoint)
    aadp_tool(aadp_quote_sensitivity_report
      SOURCES tools/quote_sensitivity_report/main.cpp
      LINK aadp_svi_chain_adjoint aadp_local_vol_ad)
    # The exact fit against G^-1, the tape against nested forward mode, the slope constraint
    # against the reduced problem, the refusals, the parity derivative, the halving rule, and a
    # small bump-and-refit check that the exact Hessian passes and Gauss-Newton does not.
    aadp_test(aadp_svi_calibration_adjoint_test
      SOURCES tests/svi/calibration_adjoint_test.cpp
      LINK aadp_svi_chain_adjoint aadp_test_support
      LABELS svi)
    # Skips, visibly, when data/ has no capture. See scripts/fetch_data.sh.
    aadp_test(aadp_svi_quote_sensitivity_realdata_test
      SOURCES tests/svi/quote_sensitivity_realdata_test.cpp
      LINK aadp_svi_chain_adjoint aadp_test_support
      LABELS svi realdata)
    aadp_test(aadp_svi_quote_sensitivity_report_tool_test
      SOURCES tests/svi/quote_sensitivity_report_tool_test.cpp
      LINK aadp_svi_chain
      LABELS svi)
    if(AADP_BUILD_TESTS AND TARGET aadp_svi_quote_sensitivity_report_tool_test)
      add_dependencies(aadp_svi_quote_sensitivity_report_tool_test aadp_quote_sensitivity_report)
      target_compile_definitions(aadp_svi_quote_sensitivity_report_tool_test PRIVATE
        AADP_QUOTE_SENSITIVITY_REPORT_PATH="$<TARGET_FILE:aadp_quote_sensitivity_report>"
        AADP_QUOTE_SENSITIVITY_REPORT_TEST_SCRATCH="${PROJECT_BINARY_DIR}/svi_test_scratch/quote_sensitivity_report")
    endif()
  endif()

  # Stage 7c: the arbitrage-free eSSVI surface, Mingone's global parametrisation, fitted to all
  # slices at once, beside the raw SVI fit (docs/NOTES.md, "Stage 7c").
  if(TARGET aadp_ad)
    aadp_library(aadp_svi_essvi
      SOURCES src/svi/essvi.cpp src/svi/essvi_fit.cpp
      LINK aadp_svi aadp_ad)
    # The conversion, the constraints by construction, the clamps, the checker's strictness, and
    # tape against Dual.
    aadp_test(aadp_svi_essvi_test
      SOURCES tests/svi/essvi_test.cpp
      LINK aadp_svi_essvi aadp_rng
      LABELS svi)
    # Recovery of hand-made surfaces, a cap at its end, data outside S, errors as values.
    aadp_test(aadp_svi_essvi_fit_test
      SOURCES tests/svi/essvi_fit_test.cpp
      LINK aadp_svi_essvi aadp_rng
      LABELS svi)
    # The fit and the global map from raw bytes: errors as values, every fit and box point
    # arbitrage-free.
    aadp_fuzz(aadp_fuzz_essvi_fit
      SOURCES fuzz/fuzz_essvi_fit.cpp
      LINK aadp_svi_essvi
      LABELS svi
      CORPUS fuzz/fuzz_essvi_fit_seeds)
  endif()
  # Stage 7c on the fitted chain, and its report tool, which prices off both surfaces.
  if(TARGET aadp_svi_essvi AND TARGET aadp_svi_chain AND TARGET aadp_local_vol_ad)
    aadp_library(aadp_svi_chain_essvi
      SOURCES src/svi/chain_essvi.cpp
      LINK aadp_svi_chain aadp_svi_essvi)
    # Skips, visibly, when data/ has no capture. See scripts/fetch_data.sh.
    aadp_test(aadp_svi_essvi_realdata_test
      SOURCES tests/svi/essvi_realdata_test.cpp
      LINK aadp_svi_chain_essvi aadp_local_vol
      LABELS svi realdata)
    aadp_tool(aadp_essvi_report
      SOURCES tools/essvi_report/main.cpp
      LINK aadp_svi_chain_essvi aadp_local_vol)
    aadp_test(aadp_svi_essvi_report_tool_test
      SOURCES tests/svi/essvi_report_tool_test.cpp
      LINK aadp_svi_chain_essvi
      LABELS svi)
    if(AADP_BUILD_TESTS AND TARGET aadp_svi_essvi_report_tool_test)
      add_dependencies(aadp_svi_essvi_report_tool_test aadp_essvi_report)
      target_compile_definitions(aadp_svi_essvi_report_tool_test PRIVATE
        AADP_ESSVI_REPORT_PATH="$<TARGET_FILE:aadp_essvi_report>"
        AADP_ESSVI_REPORT_TEST_SCRATCH="${PROJECT_BINARY_DIR}/svi_test_scratch/essvi_report")
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
