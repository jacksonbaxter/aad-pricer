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
