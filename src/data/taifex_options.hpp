#ifndef AADP_DATA_TAIFEX_OPTIONS_HPP_
#define AADP_DATA_TAIFEX_OPTIONS_HPP_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "data/taifex_csv.hpp"

namespace aadp::taifex {

enum class OptionRight : std::uint8_t { kCall, kPut };

// One row of the TAIFEX daily options report. Fields are settlements and last trades, as the
// exchange reports them: none of this is a tradable price. An optional is empty where the file
// has "-" or nothing.
struct OptionRow {
  std::size_t line = 0;
  std::int32_t trade_date = 0;  // YYYYMMDD.
  std::string contract;
  std::string expiry;  // "202411", or "202411W4" for a weekly.
  double strike = 0.0;
  OptionRight right = OptionRight::kCall;
  std::optional<double> open_price;
  std::optional<double> high_price;
  std::optional<double> low_price;
  std::optional<double> last_trade_price;
  std::optional<std::int64_t> volume;
  std::optional<double> settlement_price;
  std::optional<std::int64_t> open_interest;
  // Kept because the file has them. Calibration does not use these two: they are blank on most
  // untraded series, and their parity error is far larger than the settlements'.
  std::optional<double> last_best_bid;
  std::optional<double> last_best_ask;
  std::optional<double> historical_high;
  std::optional<double> historical_low;
  bool news_halt_flag = false;
  Session session = Session::kRegular;  // Never drop this. See Session.
};

struct OptionsFile {
  std::vector<OptionRow> rows;
  std::vector<ReadError> row_errors;
};

// Parses a whole file from memory. A bad header or an empty input is the only failure. A bad row
// lands in row_errors and the read goes on.
std::expected<OptionsFile, ReadError> read_options(std::string_view bytes);

}  // namespace aadp::taifex

#endif  // AADP_DATA_TAIFEX_OPTIONS_HPP_
