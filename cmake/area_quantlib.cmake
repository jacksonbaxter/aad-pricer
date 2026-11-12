# Targets for the QuantLib comparison (Phase 3, stage 10; docs/NOTES.md, "Stage 10"). The rows, the
# references, the tolerance and our side build everywhere, with no QuantLib, and so do their tests
# and the flag check's self-test. The bench that links QuantLib builds only with
# AAD_PRICER_BUILD_QUANTLIB_COMPARE (cmake/AadpDependencies.cmake), in the quantlib and
# quantlib-lto presets. See cmake/AadpTargets.cmake for the helpers.
if(NOT TARGET aadp_simd_ad OR NOT TARGET aadp_local_vol OR NOT TARGET aadp_quadrature_lib
   OR NOT TARGET aadp_oracle_lib)
  if(AAD_PRICER_BUILD_QUANTLIB_COMPARE)
    message(FATAL_ERROR "the QuantLib comparison needs the pricer, simd, svi and oracle areas")
  endif()
  return()
endif()

aadp_library(aadp_qlcompare_lib
  SOURCES bench/quantlib/compare.cpp bench/quantlib/ours.cpp bench/quantlib/local_vol_rows.cpp
  LINK aadp_simd_ad aadp_local_vol aadp_quadrature_lib aadp_oracle_lib)
target_include_directories(aadp_qlcompare_lib PUBLIC "${PROJECT_SOURCE_DIR}/bench")

# The comparisons and their references, R3's control variate, the grid rule, the ceilings file and
# the metrics, with no QuantLib.
aadp_test(aadp_qlcompare_test
  SOURCES tests/bench/qlcompare_test.cpp
  LINK aadp_qlcompare_lib
  LABELS unit bench)

if(AADP_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME aadp_quantlib_flag_check_selftest
    COMMAND "${Python3_EXECUTABLE}" -I tests/bench/quantlib_flags_test.py
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(aadp_quantlib_flag_check_selftest PROPERTIES LABELS "unit;bench")
endif()

if(NOT AAD_PRICER_BUILD_QUANTLIB_COMPARE OR NOT AADP_BUILD_BENCHMARKS)
  return()
endif()

# The version record for the results header: every QL_ cache value and the link mode.
get_cmake_property(_aadp_cache_variables CACHE_VARIABLES)
set(AADP_QL_CACHE_VALUES "")
foreach(_aadp_variable IN LISTS _aadp_cache_variables)
  if(_aadp_variable MATCHES "^QL_")
    string(APPEND AADP_QL_CACHE_VALUES "${_aadp_variable}=${${_aadp_variable}}\n")
  endif()
endforeach()
get_target_property(_aadp_ql_type ql_library TYPE)
if(_aadp_ql_type STREQUAL "STATIC_LIBRARY")
  set(AADP_QL_LINK_MODE "static")
else()
  set(AADP_QL_LINK_MODE "shared")
endif()
configure_file(cmake/quantlib_build.hpp.in "${AADP_GENERATED_DIR}/quantlib_build.hpp" @ONLY)

# Fails the bench's build unless a sampled QuantLib unit and the bench's units were compiled with
# the same optimisation and architecture flags, read from the compile database. QuantLib's sources
# are wherever FetchContent put them, which FETCHCONTENT_SOURCE_DIR_QUANTLIB can move.
add_custom_target(aadp_quantlib_flag_check ALL
  COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/scripts/check_quantlib_flags.py"
    "${PROJECT_BINARY_DIR}/compile_commands.json" "${quantlib_SOURCE_DIR}"
  COMMENT "Checking that QuantLib and the bench share their code-generation flags"
  VERBATIM)

aadp_bench(aadp_bench_quantlib
  SOURCES bench/quantlib/main.cpp bench/quantlib/quantlib_side.cpp
  LINK aadp_qlcompare_lib aadp_platform ql_library
  CUSTOM_MAIN)
add_dependencies(aadp_bench_quantlib aadp_quantlib_flag_check)

if(AADP_BUILD_TESTS)
  # Every comparison at N = 2^16 against its reference, and every check. Not a result.
  add_test(NAME aadp_bench_quantlib_smoke_reduced_size_not_a_result
    COMMAND aadp_bench_quantlib smoke
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(aadp_bench_quantlib_smoke_reduced_size_not_a_result PROPERTIES
    LABELS "bench;quantlib" TIMEOUT 900)
  add_test(NAME aadp_quantlib_flag_check
    COMMAND "${Python3_EXECUTABLE}" -I scripts/check_quantlib_flags.py
      "${PROJECT_BINARY_DIR}/compile_commands.json" "${quantlib_SOURCE_DIR}"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(aadp_quantlib_flag_check PROPERTIES LABELS "bench;quantlib")
endif()
