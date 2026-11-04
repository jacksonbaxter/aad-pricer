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
    SOURCES src/pricer/pricer.cpp src/pricer/bump.cpp
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

  # Check 2: bit-identical for any thread count. Small enough for the tsan preset.
  aadp_test(aadp_pricer_thread_test
    SOURCES tests/pricer/thread_test.cpp
    LINK aadp_pricer
    LABELS pricer fast threads tsan)
  # Check 3: closed forms and committed references, and the knock-in parity identity.
  aadp_test(aadp_pricer_closed_form_test
    SOURCES tests/pricer/closed_form_test.cpp
    LINK aadp_pricer aadp_oracle_lib
    LABELS pricer)

  foreach(_aadp_sized IN ITEMS aadp_pricer_path_agreement_test aadp_pricer_closed_form_test)
    if(TARGET ${_aadp_sized})
      # Full size only where the numbers are reported. Everywhere else a smoke test.
      target_compile_definitions(${_aadp_sized} PRIVATE
        AADP_SMOKE_SIZE=$<IF:$<CONFIG:Release>,0,1>)
    endif()
  endforeach()

  # Check 4: the bias table of every barrier treatment. Reported runs use the default size; ctest
  # runs it small, so its expectations are checked on every build but its numbers are not results.
  if(TARGET aadp_oracle_lib)
    aadp_tool(aadp_pricer_bias
      SOURCES tools/pricer_bias/main.cpp
      LINK aadp_pricer aadp_oracle_lib aadp_test_support)
    if(AADP_BUILD_TESTS)
      add_test(NAME aadp_pricer_bias_smoke_reduced_size_not_a_result
        COMMAND aadp_pricer_bias --paths 65536 --threads 4
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
      set_tests_properties(aadp_pricer_bias_smoke_reduced_size_not_a_result PROPERTIES
        LABELS "pricer;smoke")
    endif()
  endif()

  # Check 5: the pricer entry point under libFuzzer, ASan and UBSan (EVIDENCE B23).
  aadp_fuzz(aadp_fuzz_pricer
    SOURCES fuzz/fuzz_pricer.cpp
    LINK aadp_pricer
    LABELS pricer
    CORPUS fuzz/fuzz_pricer_seeds)

  # src/ad/ is the pricer's own tape (stage 2a). It instantiates the pricer kernels on its
  # active type, so it lives in this area.
  aadp_library(aadp_ad
    SOURCES src/ad/tape.cpp src/ad/gradient.cpp
    LINK aadp_pricer)

  aadp_test(aadp_ad_tape_test
    SOURCES tests/ad/tape_test.cpp
    LINK aadp_ad
    LABELS ad fast tsan)
  # Thread-local tapes: independent across threads, and the same bits for any thread count.
  aadp_test(aadp_ad_thread_test
    SOURCES tests/ad/thread_test.cpp
    LINK aadp_ad aadp_test_support
    LABELS ad fast threads tsan)
  # Check 1: per path against the forward-mode Dual, on every product and treatment.
  aadp_test(aadp_ad_dual_agreement_test
    SOURCES tests/ad/dual_agreement_test.cpp
    LINK aadp_ad aadp_test_support
    LABELS ad)
  # Check 2: the tangent-adjoint dot-product identity, on random programs and priced paths.
  aadp_test(aadp_ad_dot_product_test
    SOURCES tests/ad/dot_product_test.cpp
    LINK aadp_ad aadp_test_support
    LABELS ad)

  # Check 5: tape bytes per path and peak tape memory, against MEM-1 to MEM-4.
  aadp_test(aadp_ad_memory_test
    SOURCES tests/ad/memory_test.cpp
    LINK aadp_ad aadp_test_support
    LABELS ad)
  # Check 6: the one-step survival adjoint at p near 1.
  aadp_test(aadp_ad_one_step_corner_test
    SOURCES tests/ad/one_step_corner_test.cpp
    LINK aadp_ad aadp_test_support
    LABELS ad)
  # Check 7: the oracle plan's run rows with the tape in place of the Dual, at a dev seed.
  if(TARGET aadp_oracle_lib)
    aadp_test(aadp_ad_oracle_equivalence_test
      SOURCES tests/ad/oracle_equivalence_test.cpp
      LINK aadp_ad aadp_oracle_lib aadp_test_support
      LABELS ad)
    if(TARGET aadp_ad_oracle_equivalence_test)
      target_compile_definitions(aadp_ad_oracle_equivalence_test PRIVATE
        AADP_SMOKE_SIZE=$<IF:$<CONFIG:Release>,0,1>)
    endif()
  endif()
  # MEM-6: no heap allocation after warm-up, by a counting operator new. Its own binary, since
  # it replaces the global allocation functions.
  #
  # Left out of TSan builds on Linux, and only there. The Linux TSan runtime is linked statically,
  # and libclang_rt.tsan_cxx.a defines the same replaceable operator new and delete, so the link
  # fails with multiple definitions; there is no way to replace them twice. On macOS the runtime
  # is a dylib, the executable's definitions win, and the test links and passes under TSan. Every
  # other preset runs it. Thread safety of the tape stays under TSan in aadp_ad_thread_test,
  # which replaces nothing.
  if(NOT (AADP_TSAN AND CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    aadp_test(aadp_ad_allocation_test
      SOURCES tests/ad/allocation_test.cpp
      LINK aadp_ad aadp_test_support
      LABELS ad)
  endif()

  # Bump and revalue against closed-form difference quotients, with the tape SE as the ceiling.
  if(TARGET aadp_oracle_lib)
    aadp_test(aadp_pricer_bump_test
      SOURCES tests/pricer/bump_test.cpp
      LINK aadp_ad aadp_oracle_lib
      LABELS pricer bump)
  endif()

  # Check 3: operation sequences on the tape against central differences.
  aadp_fuzz(aadp_fuzz_tape_operations
    SOURCES fuzz/fuzz_tape_operations.cpp
    LINK aadp_ad aadp_test_support
    LABELS ad
    CORPUS fuzz/fuzz_tape_operations_seeds)
  # Check 4: the pricer entry point on the tape, with gradient invariants. Same input layout and
  # seed corpus as aadp_fuzz_pricer.
  aadp_fuzz(aadp_fuzz_pricer_gradient
    SOURCES fuzz/fuzz_pricer_gradient.cpp
    LINK aadp_ad aadp_test_support
    LABELS ad
    CORPUS fuzz/fuzz_pricer_seeds)

  if(AADP_BUILD_TESTS)
    add_test(NAME aadp_pricer_normal_cdf_reference_current
      COMMAND "${Python3_EXECUTABLE}" -I tests/pricer/normal_cdf_reference.py --check
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    set_tests_properties(aadp_pricer_normal_cdf_reference_current PROPERTIES LABELS "pricer")
  endif()
endif()
