// Properties, on any input, read as given and again behind a valid header:
// - each line after the header gives exactly one row or one error, in order
// - an error position lies on the error's own line, so inside the input
// - accepted rows have finite numbers, non-negative prices, volume and open interest, a positive
//   strike, and fields and a session that match the raw columns of their own line
// - the calibration filter returns only rows that meet each of its conditions
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../tests/data/taifex_fixture.hpp"
#include "data/calibration_set.hpp"
#include "data/taifex_csv.hpp"
#include "data/taifex_options.hpp"
#include "fuzz_taifex_check.hpp"

namespace {

namespace f = aadp::fuzz;
namespace t = aadp::testing;
using aadp::taifex::OptionRight;
using aadp::taifex::OptionRow;
using aadp::taifex::Session;

void check_row(const OptionRow& row, const f::LineIndex& lines) {
  const auto raw = f::split_commas(lines.text(row.line));
  AADP_FUZZ_CHECK(raw.size() == t::kOptColumnCount);
  f::check_date(row.trade_date, raw[t::kOptDate]);
  f::check_contract(row.contract, raw[t::kOptContract]);
  AADP_FUZZ_CHECK(row.expiry == raw[t::kOptExpiry]);
  AADP_FUZZ_CHECK(row.expiry.size() == 6 || row.expiry.size() == 8);
  AADP_FUZZ_CHECK(std::isfinite(row.strike) && row.strike > 0.0);
  AADP_FUZZ_CHECK(raw[t::kOptRight] == (row.right == OptionRight::kCall ? t::kCall : t::kPut));
  for (const auto* price : {&row.open_price, &row.high_price, &row.low_price, &row.last_trade_price,
                            &row.settlement_price, &row.last_best_bid, &row.last_best_ask,
                            &row.historical_high, &row.historical_low}) {
    f::check_non_negative(*price);
  }
  f::check_count(row.volume);
  f::check_count(row.open_interest);
  f::check_missing_matches(raw[t::kOptVolume], row.volume.has_value());
  f::check_missing_matches(raw[t::kOptSettlement], row.settlement_price.has_value());
  f::check_missing_matches(raw[t::kOptLast], row.last_trade_price.has_value());
  AADP_FUZZ_CHECK(row.news_halt_flag == !raw[t::kOptHalt].empty());
  f::check_session(row.session, raw[t::kOptSession]);
}

void check_calibration(const std::vector<OptionRow>& rows) {
  const std::vector<aadp::ExpiryForward> forwards = {{.expiry = "202401", .forward = 1000.0},
                                                     {.expiry = "202401W2", .forward = 1005.5},
                                                     {.expiry = "202412", .forward = 2000.0}};
  const auto slices = aadp::select_calibration_set(rows, forwards);
  AADP_FUZZ_CHECK(slices.has_value());
  AADP_FUZZ_CHECK(slices->size() == forwards.size());
  for (std::size_t i = 0; i < slices->size(); ++i) {
    const aadp::CalibrationSlice& slice = (*slices)[i];
    AADP_FUZZ_CHECK(slice.expiry == forwards[i].expiry);
    AADP_FUZZ_CHECK(slice.rows.size() <= rows.size());
    for (const OptionRow& row : slice.rows) {
      AADP_FUZZ_CHECK(row.contract == "TXO");
      AADP_FUZZ_CHECK(row.expiry == slice.expiry);
      AADP_FUZZ_CHECK(row.session == Session::kRegular);
      AADP_FUZZ_CHECK(row.right == OptionRight::kPut ? row.strike < slice.forward
                                                     : row.strike > slice.forward);
      AADP_FUZZ_CHECK(row.volume.has_value() && *row.volume > 0);
      AADP_FUZZ_CHECK(row.settlement_price.has_value());
      AADP_FUZZ_CHECK(row.last_trade_price.has_value());
      AADP_FUZZ_CHECK(*row.settlement_price == *row.last_trade_price);
      AADP_FUZZ_CHECK(*row.settlement_price >= aadp::kMinSettlementPoints);
    }
  }
}

void check_read(std::string_view input) {
  const auto file = aadp::taifex::read_options(input);
  if (!file) {
    f::check_header_error(file.error(), input);
    return;
  }
  const f::LineIndex lines(input);
  f::check_accounting(lines, file->rows, file->row_errors);
  for (const OptionRow& row : file->rows) {
    check_row(row, lines);
  }
  check_calibration(file->rows);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  check_read(input);
  // Without the header most inputs stop at line 1.
  std::string with_header = t::kOptionsHeader;
  with_header += "\r\n";
  with_header += input;
  check_read(with_header);
  return 0;
}
