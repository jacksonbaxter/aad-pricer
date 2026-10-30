# Target helpers. Call them from an area file with paths relative to the repo root.
#
#   aadp_library(<name> [SOURCES ...] [LINK ...])                     no SOURCES: header-only
#   aadp_tool(<name> SOURCES ... [LINK ...])
#   aadp_test(<name> SOURCES ... [LINK ...] [LABELS ...] [CUSTOM_MAIN])   default label: unit
#   aadp_bench(<name> SOURCES ... [LINK ...] [CUSTOM_MAIN])
#   aadp_fuzz(<name> SOURCES ... [LINK ...] [LABELS ...] [CORPUS <dir>])
#
# Executables land in <build>/bin. Tests run with the repo root as working directory.
# aadp_test, aadp_bench and aadp_fuzz do nothing in a preset that has their kind
# switched off, so area files need no conditions of their own.

set(AADP_GENERATED_DIR "${PROJECT_BINARY_DIR}/generated")

function(_aadp_parse name)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "CUSTOM_MAIN" "CORPUS" "SOURCES;LINK;LABELS")
  if(ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "${name}: unknown arguments: ${ARG_UNPARSED_ARGUMENTS}")
  endif()
  foreach(_field IN ITEMS CUSTOM_MAIN CORPUS SOURCES LINK LABELS)
    set(ARG_${_field} "${ARG_${_field}}" PARENT_SCOPE)
  endforeach()
endfunction()

function(_aadp_executable name)
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "${name}: SOURCES is required")
  endif()
  add_executable(${name} ${ARG_SOURCES})
  target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/src" "${AADP_GENERATED_DIR}")
  target_link_libraries(${name} PRIVATE aadp_warnings ${ARG_LINK})
  set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
endfunction()

function(aadp_library name)
  _aadp_parse(${name} ${ARGN})
  if(ARG_SOURCES)
    add_library(${name} STATIC ${ARG_SOURCES})
    target_include_directories(${name} PUBLIC "${PROJECT_SOURCE_DIR}/src" "${AADP_GENERATED_DIR}")
    target_link_libraries(${name} PUBLIC ${ARG_LINK} PRIVATE aadp_warnings)
  else()
    add_library(${name} INTERFACE)
    target_include_directories(${name} INTERFACE "${PROJECT_SOURCE_DIR}/src" "${AADP_GENERATED_DIR}")
    target_link_libraries(${name} INTERFACE ${ARG_LINK})
  endif()
endfunction()

function(aadp_tool name)
  _aadp_parse(${name} ${ARGN})
  _aadp_executable(${name})
endfunction()

include(GoogleTest)

function(aadp_test name)
  if(NOT AADP_BUILD_TESTS)
    return()
  endif()
  _aadp_parse(${name} ${ARGN})
  _aadp_executable(${name})
  if(ARG_CUSTOM_MAIN)
    target_link_libraries(${name} PRIVATE GTest::gtest)
  else()
    target_link_libraries(${name} PRIVATE GTest::gtest_main)
  endif()
  # PRE_TEST so that building never runs the binary: an avx512 build must link on a
  # host that cannot execute it.
  gtest_discover_tests(${name}
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 60
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  # Its PROPERTIES argument cannot carry a list-valued LABELS, so label the discovered
  # tests from a file ctest includes straight after the discovery.
  if(NOT ARG_LABELS)
    set(ARG_LABELS unit)
  endif()
  set(_script "${PROJECT_BINARY_DIR}/${name}_labels.cmake")
  file(WRITE "${_script}"
    "if(${name}_TESTS)\n"
    "  set_tests_properties(\${${name}_TESTS} PROPERTIES LABELS \"${ARG_LABELS}\")\n"
    "endif()\n")
  set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${_script}")
endfunction()

function(aadp_bench name)
  if(NOT AADP_BUILD_BENCHMARKS)
    return()
  endif()
  _aadp_parse(${name} ${ARGN})
  _aadp_executable(${name})
  if(ARG_CUSTOM_MAIN)
    target_link_libraries(${name} PRIVATE benchmark::benchmark)
  else()
    target_link_libraries(${name} PRIVATE benchmark::benchmark_main)
  endif()
endfunction()

function(aadp_fuzz name)
  if(NOT AADP_BUILD_FUZZERS)
    return()
  endif()
  _aadp_parse(${name} ${ARGN})
  _aadp_executable(${name})
  target_link_options(${name} PRIVATE -fsanitize=fuzzer)
  # -runs=0 replays the corpus without mutating, so ctest proves each target starts
  # and survives its seeds. Campaigns run the binary directly.
  set(_corpus "")
  if(ARG_CORPUS)
    set(_corpus "${PROJECT_SOURCE_DIR}/${ARG_CORPUS}")
  endif()
  add_test(NAME ${name} COMMAND ${name} -runs=0 ${_corpus})
  set_tests_properties(${name} PROPERTIES LABELS "fuzz;${ARG_LABELS}")
endfunction()
