# The tape's build knobs (src/ad/tape.hpp), for timing studies only. At their defaults the build is
# the tape every test, oracle and results file assumes, the same as with no knobs at all. Included
# by CMakeLists.txt; it also runs alone, as the check of its defaults and range:
#   cmake [-DAAD_PRICER_TAPE_CHUNK_SHIFT=<n>] -P cmake/AadpTapeKnobs.cmake
#
# AAD_PRICER_TAPE_CHUNK_SHIFT: log2 of the entries in a statement or an operand chunk (EVIDENCE
# B4). Any value but the default is for the chunk-size sweep only.

# Not cached, so a changed default reaches tape_config.hpp and TapeConfig.TheKnobsAreAtTheirDefaults.
set(AADP_TAPE_CHUNK_SHIFT_DEFAULT 13)
set(AAD_PRICER_TAPE_CHUNK_SHIFT ${AADP_TAPE_CHUNK_SHIFT_DEFAULT} CACHE STRING
    "log2 of the tape's chunk entries, 8 to 20 (default 13); any other value is for the chunk-size sweep only")
# 2^8 = 256 is the smallest chunk that holds a statement's 255 operands; 2^20 makes an operand
# chunk 12 MiB.
if(NOT AAD_PRICER_TAPE_CHUNK_SHIFT MATCHES "^[0-9]+$" OR AAD_PRICER_TAPE_CHUNK_SHIFT LESS 8
   OR AAD_PRICER_TAPE_CHUNK_SHIFT GREATER 20)
  message(FATAL_ERROR "AAD_PRICER_TAPE_CHUNK_SHIFT must be an integer from 8 to 20, not "
                      "'${AAD_PRICER_TAPE_CHUNK_SHIFT}'")
endif()

set(_aadp_tape_study "")
if(NOT AAD_PRICER_TAPE_CHUNK_SHIFT EQUAL AADP_TAPE_CHUNK_SHIFT_DEFAULT)
  list(APPEND _aadp_tape_study "AAD_PRICER_TAPE_CHUNK_SHIFT=${AAD_PRICER_TAPE_CHUNK_SHIFT}")
endif()
if(_aadp_tape_study)
  message(WARNING "${_aadp_tape_study}: a build for a tape timing study only. Only aadp_bench_aad "
                  "and single-threaded drivers that time it are supported here. The tests assume "
                  "the defaults, and some fail by design, TapeConfig.TheKnobsAreAtTheirDefaults "
                  "first.")
endif()
