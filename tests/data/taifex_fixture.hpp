#ifndef AADP_TESTS_DATA_TAIFEX_FIXTURE_HPP_
#define AADP_TESTS_DATA_TAIFEX_FIXTURE_HPP_

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

// Synthetic rows with made-up values. Nothing here comes from a TAIFEX file. The headers are
// spelled out again rather than taken from the readers, so a typo in a reader's constant fails
// here too.
namespace aadp::testing {

inline const std::string kOptionsHeader =
    "\xA4\xE9\xB4\xC1,\xAB\xB4\xAC\xF9,\xA8\xEC\xB4\xC1\xA4\xEB\xA5\xF7\x28\xB6\x67\xA7\x4F\x29,"
    "\xBC\x69\xAC\xF9\xBB\xF9,\xB6\x52\xBD\xE6\xC5\x76,\xB6\x7D\xBD\x4C\xBB\xF9,"
    "\xB3\xCC\xB0\xAA\xBB\xF9,\xB3\xCC\xA7\x43\xBB\xF9,\xB3\xCC\xAB\xE1\xA6\xA8\xA5\xE6\xBB\xF9,"
    "\xA6\xA8\xA5\xE6\xB6\x71,\xB5\xB2\xBA\xE2\xBB\xF9,"
    "\xA5\xBC\xA8\x52\xBE\x50\xAB\xB4\xAC\xF9\xB6\x71,"
    "\xB3\xCC\xAB\xE1\xB3\xCC\xA8\xCE\xB6\x52\xBB\xF9,"
    "\xB3\xCC\xAB\xE1\xB3\xCC\xA8\xCE\xBD\xE6\xBB\xF9,"
    "\xBE\xFA\xA5\x76\xB3\xCC\xB0\xAA\xBB\xF9,\xBE\xFA\xA5\x76\xB3\xCC\xA7\x43\xBB\xF9,"
    "\xAC\x4F\xA7\x5F\xA6\x5D\xB0\x54\xAE\xA7\xAD\xB1\xBC\xC8\xB0\xB1\xA5\xE6\xA9\xF6,"
    "\xA5\xE6\xA9\xF6\xAE\xC9\xAC\x71";

inline const std::string kFuturesHeader =
    "\xA4\xE9\xB4\xC1,\xAB\xB4\xAC\xF9,\xA8\xEC\xB4\xC1\xA4\xEB\xA5\xF7\x28\xB6\x67\xA7\x4F\x29,"
    "\xB6\x7D\xBD\x4C\xBB\xF9,\xB3\xCC\xB0\xAA\xBB\xF9,\xB3\xCC\xA7\x43\xBB\xF9,"
    "\xB3\xCC\xAB\xE1\xA6\xA8\xA5\xE6\xBB\xF9,\xBA\xA6\xB6\x5E\xBB\xF9,\xBA\xA6\xB6\x5E\x25,"
    "\xA6\x58\xAD\x70\xA6\xA8\xA5\xE6\xB6\x71,\xB5\xB2\xBA\xE2\xBB\xF9,"
    "\xA5\xBC\xA8\x52\xBE\x50\xAB\xB4\xAC\xF9\xBC\xC6,"
    "\xB3\xCC\xAB\xE1\xB3\xCC\xA8\xCE\xB6\x52\xBB\xF9,"
    "\xB3\xCC\xAB\xE1\xB3\xCC\xA8\xCE\xBD\xE6\xBB\xF9,"
    "\xBE\xFA\xA5\x76\xB3\xCC\xB0\xAA\xBB\xF9,\xBE\xFA\xA5\x76\xB3\xCC\xA7\x43\xBB\xF9,"
    "\xAC\x4F\xA7\x5F\xA6\x5D\xB0\x54\xAE\xA7\xAD\xB1\xBC\xC8\xB0\xB1\xA5\xE6\xA9\xF6,"
    "\xA5\xE6\xA9\xF6\xAE\xC9\xAC\x71,"
    "\xBB\xF9\xAE\x74\xB9\xEF\xB3\xE6\xA6\xA1\xA9\x65\xB0\x55\xA6\xA8\xA5\xE6\xB6\x71";

inline constexpr std::string_view kRegular = "\xA4\x40\xAF\xEB";     // 一般
inline constexpr std::string_view kAfterHours = "\xBD\x4C\xAB\xE1";  // 盤後
inline constexpr std::string_view kCall = "\xB6\x52\xC5\x76";        // 買權
inline constexpr std::string_view kPut = "\xBD\xE6\xC5\x76";         // 賣權

// Column indices, for tests that spoil one field.
enum OptionColumn : std::size_t {
  kOptDate,
  kOptContract,
  kOptExpiry,
  kOptStrike,
  kOptRight,
  kOptOpen,
  kOptHigh,
  kOptLow,
  kOptLast,
  kOptVolume,
  kOptSettlement,
  kOptOpenInterest,
  kOptBid,
  kOptAsk,
  kOptHistHigh,
  kOptHistLow,
  kOptHalt,
  kOptSession,
  kOptColumnCount,
};

enum FuturesColumn : std::size_t {
  kFutDate,
  kFutContract,
  kFutExpiry,
  kFutOpen,
  kFutHigh,
  kFutLow,
  kFutLast,
  kFutChange,
  kFutChangePercent,
  kFutVolume,
  kFutSettlement,
  kFutOpenInterest,
  kFutBid,
  kFutAsk,
  kFutHistHigh,
  kFutHistLow,
  kFutHalt,
  kFutSession,
  kFutSpreadVolume,
  kFutColumnCount,
};

using OptionFields = std::array<std::string, kOptColumnCount>;
using FuturesFields = std::array<std::string, kFutColumnCount>;

inline OptionFields option_fields() {
  return {"20240103",
          "ABO",
          "202401",
          "1000",
          std::string(kCall),
          "10.5",
          "11",
          "10",
          "10.5",
          "7",
          "10.5",
          "42",
          "10",
          "11",
          "12",
          "9",
          "",
          std::string(kRegular)};
}

inline FuturesFields futures_fields() {
  return {"20240103", "ABF",  "202401", "1000", "1010", "990",  "1005", "5", "0.50%",
          "7",        "1005", "42",     "1004", "1006", "1100", "900",  "",  std::string(kRegular),
          ""};
}

template <std::size_t N>
std::string join(const std::array<std::string, N>& fields) {
  std::string line;
  for (std::size_t i = 0; i < N; ++i) {
    if (i != 0) {
      line += ',';
    }
    line += fields[i];
  }
  return line;
}

}  // namespace aadp::testing

#endif  // AADP_TESTS_DATA_TAIFEX_FIXTURE_HPP_
