# Targets for the data area. See cmake/AadpTargets.cmake for the helpers.
if(AADP_BUILD_TESTS)
  add_test(NAME data_fetch_script
    COMMAND bash "${PROJECT_SOURCE_DIR}/tests/data/fetch_data_test.sh"
            "${PROJECT_BINARY_DIR}/data_test_scratch/fetch_data"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(data_fetch_script PROPERTIES LABELS "unit;fast")
endif()
