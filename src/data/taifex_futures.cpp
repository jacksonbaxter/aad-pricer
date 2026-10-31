#include "data/taifex_futures.hpp"

#include <array>
#include <expected>
#include <optional>
#include <string_view>

#include "data/decimal.hpp"
#include "data/taifex_csv.hpp"

namespace aadp::taifex {

namespace {

// The header, one cp950 byte string per column. Comparing bytes needs no mapping table.
constexpr std::array<std::string_view, 19> kHeader = {
    "\xA4\xE9\xB4\xC1",                                          // 日期
    "\xAB\xB4\xAC\xF9",                                          // 契約
    "\xA8\xEC\xB4\xC1\xA4\xEB\xA5\xF7\x28\xB6\x67\xA7\x4F\x29",  // 到期月份(週別)
    "\xB6\x7D\xBD\x4C\xBB\xF9",                                  // 開盤價
    "\xB3\xCC\xB0\xAA\xBB\xF9",                                  // 最高價
    "\xB3\xCC\xA7\x43\xBB\xF9",                                  // 最低價
    "\xB3\xCC\xAB\xE1\xA6\xA8\xA5\xE6\xBB\xF9",                  // 最後成交價
    "\xBA\xA6\xB6\x5E\xBB\xF9",                                  // 漲跌價
    "\xBA\xA6\xB6\x5E\x25",                                      // 漲跌%
    "\xA6\x58\xAD\x70\xA6\xA8\xA5\xE6\xB6\x71",                  // 合計成交量
    "\xB5\xB2\xBA\xE2\xBB\xF9",                                  // 結算價
    "\xA5\xBC\xA8\x52\xBE\x50\xAB\xB4\xAC\xF9\xBC\xC6",          // 未沖銷契約數
    "\xB3\xCC\xAB\xE1\xB3\xCC\xA8\xCE\xB6\x52\xBB\xF9",          // 最後最佳買價
    "\xB3\xCC\xAB\xE1\xB3\xCC\xA8\xCE\xBD\xE6\xBB\xF9",          // 最後最佳賣價
    "\xBE\xFA\xA5\x76\xB3\xCC\xB0\xAA\xBB\xF9",                  // 歷史最高價
    "\xBE\xFA\xA5\x76\xB3\xCC\xA7\x43\xBB\xF9",                  // 歷史最低價
    "\xAC\x4F\xA7\x5F\xA6\x5D\xB0\x54\xAE\xA7\xAD\xB1\xBC\xC8\xB0\xB1\xA5\xE6\xA9\xF6",  // 是否因訊息面暫停交易
    "\xA5\xE6\xA9\xF6\xAE\xC9\xAC\x71",                                                  // 交易時段
    "\xBB\xF9\xAE\x74\xB9\xEF\xB3\xE6\xA6\xA1\xA9\x65\xB0\x55\xA6\xA8\xA5\xE6\xB6\x71",  // 價差對單式委託成交量
};

constexpr int kDate = 0;
constexpr int kContract = 1;
constexpr int kExpiry = 2;
constexpr int kOpen = 3;
constexpr int kHigh = 4;
constexpr int kLow = 5;
constexpr int kLastTrade = 6;
constexpr int kChange = 7;
constexpr int kChangePercent = 8;
constexpr int kVolume = 9;
constexpr int kSettlement = 10;
constexpr int kOpenInterest = 11;
constexpr int kLastBestBid = 12;
constexpr int kLastBestAsk = 13;
constexpr int kHistoricalHigh = 14;
constexpr int kHistoricalLow = 15;
constexpr int kNewsHalt = 16;
constexpr int kSession = 17;
constexpr int kSpreadOrderVolume = 18;

// The real file has "NULL" here on after-hours outright rows, and nowhere else in the file. So it
// counts as missing in this column only.
std::optional<double> parse_settlement(RowFields& fields) {
  if (fields.ok() && fields.field(kSettlement) == "NULL") {
    return std::nullopt;
  }
  return fields.number(kSettlement, SignPolicy::kNonNegative);
}

FuturesRow parse_row(RowFields& fields) {
  FuturesRow row;
  row.trade_date = fields.date(kDate);
  row.contract = fields.contract(kContract);
  row.expiry = fields.expiry(kExpiry, /*allow_spread=*/true);
  row.calendar_spread = row.expiry.contains('/');
  const SignPolicy price = row.calendar_spread ? SignPolicy::kSigned : SignPolicy::kNonNegative;
  row.open_price = fields.number(kOpen, price);
  row.high_price = fields.number(kHigh, price);
  row.low_price = fields.number(kLow, price);
  row.last_trade_price = fields.number(kLastTrade, price);
  row.change = fields.number(kChange, SignPolicy::kSigned);
  row.change_percent = fields.percent(kChangePercent);
  row.volume = fields.count(kVolume);
  row.settlement_price = parse_settlement(fields);
  row.open_interest = fields.count(kOpenInterest);
  row.last_best_bid = fields.number(kLastBestBid, price);
  row.last_best_ask = fields.number(kLastBestAsk, price);
  row.historical_high = fields.number(kHistoricalHigh, price);
  row.historical_low = fields.number(kHistoricalLow, price);
  row.news_halt_flag = fields.ok() && !fields.is_empty(kNewsHalt);
  row.session = fields.session(kSession);
  row.spread_order_volume = fields.count(kSpreadOrderVolume);
  return row;
}

}  // namespace

std::expected<FuturesFile, ReadError> read_futures(std::string_view bytes) {
  RecordReader records(bytes);
  if (auto header = check_header(records, kHeader); !header) {
    return std::unexpected(header.error());
  }
  FuturesFile file;
  read_rows(records, kHeader.size(), parse_row, file.rows, file.row_errors);
  return file;
}

}  // namespace aadp::taifex
