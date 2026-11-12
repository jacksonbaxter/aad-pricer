# Targets for the data area. See cmake/AadpTargets.cmake for the helpers.
aadp_library(aadp_data
  SOURCES
    src/data/attribution.cpp
    src/data/calibration_set.cpp
    src/data/cp950.cpp
    src/data/decimal.cpp
    src/data/forwards.cpp
    src/data/taifex_csv.cpp
    src/data/taifex_futures.cpp
    src/data/taifex_options.cpp)

aadp_test(aadp_data_tests
  SOURCES
    tests/data/attribution_test.cpp
    tests/data/calibration_set_test.cpp
    tests/data/cp950_test.cpp
    tests/data/decimal_test.cpp
    tests/data/forwards_test.cpp
    tests/data/taifex_futures_test.cpp
    tests/data/taifex_options_test.cpp
  LINK aadp_data)

# Skips, visibly, when data/ has no capture. See scripts/fetch_data.sh.
aadp_test(aadp_data_realdata_tests
  SOURCES tests/data/realdata_test.cpp
  LINK aadp_data
  LABELS realdata)

aadp_tool(chain_profile
  SOURCES tools/chain_profile/main.cpp
  LINK aadp_data)

aadp_test(aadp_chain_profile_tests
  SOURCES tests/data/chain_profile_test.cpp
  LINK aadp_data)

if(AADP_BUILD_TESTS)
  add_dependencies(aadp_chain_profile_tests chain_profile)
  target_compile_definitions(aadp_chain_profile_tests PRIVATE
    AADP_CHAIN_PROFILE_PATH="$<TARGET_FILE:chain_profile>"
    AADP_DATA_TEST_SCRATCH="${PROJECT_BINARY_DIR}/data_test_scratch/chain_profile")

  add_test(NAME data_fetch_script
    COMMAND bash "${PROJECT_SOURCE_DIR}/tests/data/fetch_data_test.sh"
            "${PROJECT_BINARY_DIR}/data_test_scratch/fetch_data"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(data_fetch_script PROPERTIES LABELS "unit;fast")

  # The repo scan's notes-file rule: a tracked agent notes file fails it at any path.
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME data_repo_scan_script
    COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/tests/data/scan_repo_test.py"
            "${PROJECT_BINARY_DIR}/data_test_scratch/scan_repo"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(data_repo_scan_script PROPERTIES LABELS "unit;fast")
endif()

aadp_fuzz(fuzz_cp950
  SOURCES fuzz/fuzz_cp950.cpp
  LINK aadp_data
  CORPUS fuzz/fuzz_cp950_corpus)

aadp_fuzz(fuzz_taifex_options
  SOURCES fuzz/fuzz_taifex_options.cpp
  LINK aadp_data
  CORPUS fuzz/fuzz_taifex_options_corpus)

aadp_fuzz(fuzz_taifex_futures
  SOURCES fuzz/fuzz_taifex_futures.cpp
  LINK aadp_data
  CORPUS fuzz/fuzz_taifex_futures_corpus)
