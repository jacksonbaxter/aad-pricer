# Every dependency is a pinned archive checked by SHA-256, fetched into the build
# tree. SYSTEM keeps its headers out of our warnings. EXCLUDE_FROM_ALL builds only
# what a project target links.
include(FetchContent)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(hwy
  URL      https://github.com/google/highway/releases/download/1.4.0/highway-1.4.0.tar.gz
  URL_HASH SHA256=36f672ab48ddb3c8555e9e89e16fe400cd7d16c6eb455a1a3d0c146a63ababdc
  DOWNLOAD_EXTRACT_TIMESTAMP FALSE
  SYSTEM EXCLUDE_FROM_ALL)
FetchContent_Declare(hdr_histogram
  URL      https://github.com/HdrHistogram/HdrHistogram_c/archive/refs/tags/0.12.1.tar.gz
  URL_HASH SHA256=b2cff564b49f900e7f951c95428100a4aed36561703237ffc5e41b4843d804ce
  DOWNLOAD_EXTRACT_TIMESTAMP FALSE
  SYSTEM EXCLUDE_FROM_ALL)
FetchContent_Declare(googletest
  URL      https://github.com/google/googletest/releases/download/v1.18.0/googletest-1.18.0.tar.gz
  URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
  DOWNLOAD_EXTRACT_TIMESTAMP FALSE
  SYSTEM EXCLUDE_FROM_ALL)
FetchContent_Declare(benchmark
  URL      https://github.com/google/benchmark/archive/refs/tags/v1.10.0.tar.gz
  URL_HASH SHA256=acc5c7c0fbed855c4aaa2d7c3c36fb82b0c15c28f4e151e3f679c2fc0aa404af
  DOWNLOAD_EXTRACT_TIMESTAMP FALSE
  SYSTEM EXCLUDE_FROM_ALL)

# Cache entries, not plain variables: HdrHistogram's cmake_minimum_required is old
# enough that option() would ignore a plain variable.
set(HWY_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(HWY_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(HWY_ENABLE_CONTRIB OFF CACHE BOOL "" FORCE)
set(HWY_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(HDR_HISTOGRAM_CORE_ONLY ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(hwy hdr_histogram)

if(AADP_BUILD_TESTS)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

# The smoke test links benchmark too, to prove it links.
if(AADP_BUILD_TESTS OR AADP_BUILD_BENCHMARKS)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_ASSEMBLY_TESTS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_INSTALL_TOOLS OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)
endif()

# Let's Be Rational, Peter Jäckel's reference implementation of implied Black volatility, for the
# svi area. Revision 1520, the archive of 2024-02-18; its licence is quoted in NOTICE. The host
# serves plain http only, so the SHA-256 is all that protects it. A local copy named by the
# environment variable AADP_LBR_ARCHIVE, which CI restores from its cache, is used instead, and the
# same hash is checked either way. The archive has no CMake build, so SOURCE_SUBDIR names a folder
# it does not have: MakeAvailable then only unpacks it, and aadp_lets_be_rational below builds the
# four files the inversion needs. One identifier is renamed at fetch time, for C++23; see
# patch_lets_be_rational.cmake. Nothing of the archive is committed.
if(NOT DEFINED AADP_AREA_SVI OR AADP_AREA_SVI)
  # One URL, not a list: ExternalProject turns file:// into a path, and a list may not hold paths.
  set(_aadp_lbr_url "http://www.jaeckel.org/LetsBeRational.7z")
  if(DEFINED ENV{AADP_LBR_ARCHIVE} AND IS_ABSOLUTE "$ENV{AADP_LBR_ARCHIVE}"
     AND EXISTS "$ENV{AADP_LBR_ARCHIVE}")
    set(_aadp_lbr_url "$ENV{AADP_LBR_ARCHIVE}")
  endif()
  FetchContent_Declare(lets_be_rational
    URL      "${_aadp_lbr_url}"
    URL_HASH SHA256=da2f6870b213e04ef35b4d309269ee5ce12be5830d9733e5f29542bf7b652470
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    PATCH_COMMAND "${CMAKE_COMMAND}" "-DFILE=<SOURCE_DIR>/normaldistribution.cpp"
                  -P "${CMAKE_CURRENT_LIST_DIR}/patch_lets_be_rational.cmake"
    SOURCE_SUBDIR no-cmake-build-here
    SYSTEM EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(lets_be_rational)

  # Third-party code: it never links aadp_warnings, so -Werror and our warning set do not reach
  # it, and its headers are SYSTEM. It keeps the presets' -ffp-contract=off and the sanitizers.
  # NO_XL_API leaves out the Excel add-in layer.
  add_library(aadp_lets_be_rational STATIC
    "${lets_be_rational_SOURCE_DIR}/lets_be_rational.cpp"
    "${lets_be_rational_SOURCE_DIR}/normaldistribution.cpp"
    "${lets_be_rational_SOURCE_DIR}/rationalcubic.cpp"
    "${lets_be_rational_SOURCE_DIR}/erf_cody.cpp")
  target_compile_definitions(aadp_lets_be_rational PRIVATE NO_XL_API)
  target_include_directories(aadp_lets_be_rational SYSTEM PUBLIC "${lets_be_rational_SOURCE_DIR}")
endif()

# 985 translation units, so optional. QuantLib tag v1.44 peels to commit
# ab4cb476d35f7275834bc05b47d1324403d1823b. Nothing links ql_library yet.
option(AAD_PRICER_BUILD_QUANTLIB_COMPARE "Fetch Boost and QuantLib for the comparison bench" OFF)
if(AAD_PRICER_BUILD_QUANTLIB_COMPARE)
  FetchContent_Declare(boost_headers
    URL      https://github.com/boostorg/boost/releases/download/boost-1.92.0/boost-1.92.0-b2-nodocs.tar.xz
    URL_HASH SHA256=ea7b982002cc9dfbe59b0b217b206f470dc75f3de0bb2973d844118934d82411
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
  FetchContent_Declare(quantlib
    URL      https://github.com/lballabio/QuantLib/releases/download/v1.44/QuantLib-1.44.tar.gz
    URL_HASH SHA256=34d7787a367404447f84be6942b19fd091f1e8e45a380d3f153e6cd8c4bb8ad8
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    SYSTEM EXCLUDE_FROM_ALL)

  FetchContent_MakeAvailable(boost_headers)
  set(BOOST_ROOT "${boost_headers_SOURCE_DIR}")
  set(Boost_NO_SYSTEM_PATHS ON)
  set(Boost_NO_BOOST_CMAKE ON)
  set(QL_BUILD_TEST_SUITE OFF CACHE BOOL "" FORCE)
  set(QL_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(quantlib)

  # QuantLib exports ql_library for install, and CMake rejects an exported include path
  # inside the build tree, which is where this Boost lives.
  foreach(_ql_property IN ITEMS INTERFACE_INCLUDE_DIRECTORIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
    get_target_property(_ql_includes ql_library ${_ql_property})
    list(REMOVE_ITEM _ql_includes "${boost_headers_SOURCE_DIR}")
    set_target_properties(ql_library PROPERTIES ${_ql_property} "${_ql_includes}")
  endforeach()
  target_include_directories(ql_library SYSTEM INTERFACE
    "$<BUILD_INTERFACE:${boost_headers_SOURCE_DIR}>")
endif()
