# Exercises all five helpers, so a broken helper fails here and not in an area.
aadp_library(aadp_smoke_lib
  SOURCES src/smoke/smoke.cpp
  LINK hwy)

aadp_test(aadp_smoke
  SOURCES tests/smoke/smoke_test.cpp
  LINK aadp_smoke_lib benchmark::benchmark hdr_histogram::hdr_histogram_core_static
  LABELS smoke fast)

aadp_tool(aadp_smoke_tool
  SOURCES tests/smoke/smoke_tool.cpp
  LINK aadp_smoke_lib)

aadp_bench(aadp_smoke_bench
  SOURCES tests/smoke/smoke_bench.cpp
  LINK aadp_smoke_lib)

aadp_fuzz(aadp_smoke_fuzz
  SOURCES tests/smoke/smoke_fuzz.cpp
  LINK aadp_smoke_lib
  LABELS smoke)
