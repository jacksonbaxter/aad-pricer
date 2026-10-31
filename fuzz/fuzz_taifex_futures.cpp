// Properties, on any input, read as given and again behind a valid header:
// - each line after the header gives exactly one row or one error, in order
// - an error position lies on the error's own line, so inside the input
// - accepted rows have finite numbers and non-negative volume, open interest and settlement,
//   negative prices appear only on calendar spreads, and fields and the session match the raw
//   columns of their own line
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "../tests/data/taifex_fixture.hpp"
#include "data/taifex_futures.hpp"
#include "fuzz_taifex_check.hpp"

namespace {

namespace f = aadp::fuzz;
namespace t = aadp::testing;
using aadp::taifex::FuturesRow;

void check_row(const FuturesRow& row, const f::LineIndex& lines) {
  const auto raw = f::split_commas(lines.text(row.line));
  AADP_FUZZ_CHECK(raw.size() == t::kFutColumnCount);
  f::check_date(row.trade_date, raw[t::kFutDate]);
  f::check_contract(row.contract, raw[t::kFutContract]);
  AADP_FUZZ_CHECK(row.expiry == raw[t::kFutExpiry]);
  AADP_FUZZ_CHECK(row.calendar_spread == (raw[t::kFutExpiry].find('/') != std::string_view::npos));
  for (const auto* price :
       {&row.open_price, &row.high_price, &row.low_price, &row.last_trade_price, &row.last_best_bid,
        &row.last_best_ask, &row.historical_high, &row.historical_low}) {
    if (row.calendar_spread) {
      f::check_finite(*price);
    } else {
      f::check_non_negative(*price);
    }
  }
  f::check_finite(row.change);
  f::check_finite(row.change_percent);
  f::check_non_negative(row.settlement_price);
  f::check_count(row.volume);
  f::check_count(row.open_interest);
  f::check_count(row.spread_order_volume);
  f::check_missing_matches(raw[t::kFutVolume], row.volume.has_value());
  if (raw[t::kFutSettlement] == "NULL") {
    AADP_FUZZ_CHECK(!row.settlement_price.has_value());
  } else {
    f::check_missing_matches(raw[t::kFutSettlement], row.settlement_price.has_value());
  }
  AADP_FUZZ_CHECK(row.news_halt_flag == !raw[t::kFutHalt].empty());
  f::check_session(row.session, raw[t::kFutSession]);
}

void check_read(std::string_view input) {
  const auto file = aadp::taifex::read_futures(input);
  if (!file) {
    f::check_header_error(file.error(), input);
    return;
  }
  const f::LineIndex lines(input);
  f::check_accounting(lines, file->rows, file->row_errors);
  for (const FuturesRow& row : file->rows) {
    check_row(row, lines);
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view input(std::bit_cast<const char*>(data), size);
  check_read(input);
  // Without the header most inputs stop at line 1.
  std::string with_header = t::kFuturesHeader;
  with_header += "\r\n";
  with_header += input;
  check_read(with_header);
  return 0;
}
