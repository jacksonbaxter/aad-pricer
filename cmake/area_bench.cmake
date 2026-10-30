# Targets for the bench area. See cmake/AadpTargets.cmake for the helpers.
aadp_library(aadp_platform
  SOURCES
    src/platform/affinity.cpp
    src/platform/cpu_info.cpp
    src/platform/fp_state.cpp
    src/platform/host_label.cpp
    src/platform/memory_info.cpp
    src/platform/perf_counters.cpp
    src/platform/simd_info.cpp
    src/platform/timer.cpp
  LINK hwy)

aadp_test(aadp_platform_unit
  SOURCES tests/bench/platform_test.cpp tests/bench/timer_test.cpp
  LINK aadp_platform)
