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
inline constexpr std::size_t kOptDate = 0;
inline constexpr std::size_t kOptContract = 1;
inline constexpr std::size_t kOptExpiry = 2;
inline constexpr std::size_t kOptStrike = 3;
inline constexpr std::size_t kOptRight = 4;
inline constexpr std::size_t kOptOpen = 5;
inline constexpr std::size_t kOptHigh = 6;
inline constexpr std::size_t kOptLow = 7;
inline constexpr std::size_t kOptLast = 8;
inline constexpr std::size_t kOptVolume = 9;
inline constexpr std::size_t kOptSettlement = 10;
inline constexpr std::size_t kOptOpenInterest = 11;
inline constexpr std::size_t kOptBid = 12;
inline constexpr std::size_t kOptAsk = 13;
inline constexpr std::size_t kOptHistHigh = 14;
inline constexpr std::size_t kOptHistLow = 15;
inline constexpr std::size_t kOptHalt = 16;
inline constexpr std::size_t kOptSession = 17;
inline constexpr std::size_t kOptColumnCount = 18;

inline constexpr std::size_t kFutDate = 0;
inline constexpr std::size_t kFutContract = 1;
inline constexpr std::size_t kFutExpiry = 2;
inline constexpr std::size_t kFutOpen = 3;
inline constexpr std::size_t kFutHigh = 4;
inline constexpr std::size_t kFutLow = 5;
inline constexpr std::size_t kFutLast = 6;
inline constexpr std::size_t kFutChange = 7;
inline constexpr std::size_t kFutChangePercent = 8;
inline constexpr std::size_t kFutVolume = 9;
inline constexpr std::size_t kFutSettlement = 10;
inline constexpr std::size_t kFutOpenInterest = 11;
inline constexpr std::size_t kFutBid = 12;
inline constexpr std::size_t kFutAsk = 13;
inline constexpr std::size_t kFutHistHigh = 14;
inline constexpr std::size_t kFutHistLow = 15;
inline constexpr std::size_t kFutHalt = 16;
inline constexpr std::size_t kFutSession = 17;
inline constexpr std::size_t kFutSpreadVolume = 18;
inline constexpr std::size_t kFutColumnCount = 19;

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
