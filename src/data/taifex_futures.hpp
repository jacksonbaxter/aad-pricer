#ifndef AADP_DATA_TAIFEX_FUTURES_HPP_
#define AADP_DATA_TAIFEX_FUTURES_HPP_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "data/taifex_csv.hpp"

namespace aadp::taifex {

// One row of the TAIFEX daily futures report, a cross-check on the parity forwards. An optional is
// empty where the file has "-" or nothing, or "NULL" in the settlement column.
struct FuturesRow {
  std::size_t line = 0;
  std::int32_t trade_date = 0;  // YYYYMMDD.
  std::string contract;
  std::string expiry;  // "202411", "202411W4", or "202411/202412" for a calendar spread.
  bool calendar_spread = false;
  // Prices of a calendar spread are differences and can be negative. Outright prices cannot.
  std::optional<double> open_price;
  std::optional<double> high_price;
  std::optional<double> low_price;
  std::optional<double> last_trade_price;
  std::optional<double> change;
  std::optional<double> change_percent;  // In percent: 1.10 for "1.10%".
  std::optional<std::int64_t> volume;
  std::optional<double> settlement_price;
  std::optional<std::int64_t> open_interest;
  // Kept because the file has them. Calibration does not use these two.
  std::optional<double> last_best_bid;
  std::optional<double> last_best_ask;
  std::optional<double> historical_high;
  std::optional<double> historical_low;
  bool news_halt_flag = false;
  Session session = Session::kRegular;  // Never drop this. See Session.
  std::optional<std::int64_t> spread_order_volume;
};

struct FuturesFile {
  std::vector<FuturesRow> rows;
  std::vector<ReadError> row_errors;
};

// Parses a whole file from memory. A bad header or an empty input is the only failure. A bad row
// lands in row_errors and the read goes on.
std::expected<FuturesFile, ReadError> read_futures(std::string_view bytes);

}  // namespace aadp::taifex

#endif  // AADP_DATA_TAIFEX_FUTURES_HPP_
