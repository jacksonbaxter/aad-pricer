# Targets for the data area. See cmake/AadpTargets.cmake for the helpers.
aadp_library(aadp_data
  SOURCES
    src/data/cp950.cpp
    src/data/decimal.cpp
    src/data/taifex_csv.cpp
    src/data/taifex_futures.cpp
    src/data/taifex_options.cpp)

aadp_test(aadp_data_tests
  SOURCES
    tests/data/cp950_test.cpp
    tests/data/decimal_test.cpp
    tests/data/taifex_futures_test.cpp
    tests/data/taifex_options_test.cpp
  LINK aadp_data)

if(AADP_BUILD_TESTS)
  add_test(NAME data_fetch_script
    COMMAND bash "${PROJECT_SOURCE_DIR}/tests/data/fetch_data_test.sh"
            "${PROJECT_BINARY_DIR}/data_test_scratch/fetch_data"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(data_fetch_script PROPERTIES LABELS "unit;fast")
endif()
