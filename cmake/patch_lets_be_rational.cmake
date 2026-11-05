# Build-time patch of the fetched Let's Be Rational sources, run by FetchContent once the archive
# has passed its SHA-256 check. Nothing of it is committed.
#
# normaldistribution.cpp names a function inverse_norm_cdfm½_for_midrange_probabilities. U+00BD
# was allowed in identifiers up to C++20's Annex E but is not XID_Continue, so C++23 (P1949,
# UAX #31) rejects it, and clang rejects it in every language mode. The patch renames that one
# identifier, in its three places, to ..._cdfm_half_...; no other byte changes. The licence
# permits modification ("Permission to use, copy, modify, and distribute"), and NOTICE says the
# file is patched.
#
#   cmake -DFILE=<path to normaldistribution.cpp> -P patch_lets_be_rational.cmake
if(NOT DEFINED FILE OR NOT EXISTS "${FILE}")
  message(FATAL_ERROR "patch_lets_be_rational: FILE is not a file: ${FILE}")
endif()
file(READ "${FILE}" _text)
set(_from "inverse_norm_cdfm½_for_midrange_probabilities")
set(_to "inverse_norm_cdfm_half_for_midrange_probabilities")
string(REGEX MATCHALL "inverse_norm_cdfm½_for_midrange_probabilities" _found "${_text}")
list(LENGTH _found _count)
string(FIND "${_text}" "${_to}" _already)
if(_count EQUAL 0 AND NOT _already EQUAL -1)
  return()
endif()
if(NOT _count EQUAL 3)
  message(FATAL_ERROR "patch_lets_be_rational: expected 3 uses of ${_from}, found ${_count}")
endif()
string(REPLACE "${_from}" "${_to}" _text "${_text}")
file(WRITE "${FILE}" "${_text}")
message(STATUS "patch_lets_be_rational: renamed ${_from} in ${FILE}")
