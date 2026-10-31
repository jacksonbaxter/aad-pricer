#include "data/taifex_options.hpp"

#include <array>

namespace aadp::taifex {

namespace {

// The header, one cp950 byte string per column. Comparing bytes needs no mapping table.
constexpr std::array<std::string_view, 18> kHeader = {
    "\xA4\xE9\xB4\xC1",                                          // 日期
    "\xAB\xB4\xAC\xF9",                                          // 契約
    "\xA8\xEC\xB4\xC1\xA4\xEB\xA5\xF7\x28\xB6\x67\xA7\x4F\x29",  // 到期月份(週別)
    "\xBC\x69\xAC\xF9\xBB\xF9",                                  // 履約價
    "\xB6\x52\xBD\xE6\xC5\x76",                                  // 買賣權
    "\xB6\x7D\xBD\x4C\xBB\xF9",                                  // 開盤價
    "\xB3\xCC\xB0\xAA\xBB\xF9",                                  // 最高價
    "\xB3\xCC\xA7\x43\xBB\xF9",                                  // 最低價
    "\xB3\xCC\xAB\xE1\xA6\xA8\xA5\xE6\xBB\xF9",                  // 最後成交價
    "\xA6\xA8\xA5\xE6\xB6\x71",                                  // 成交量
    "\xB5\xB2\xBA\xE2\xBB\xF9",                                  // 結算價
    "\xA5\xBC\xA8\x52\xBE\x50\xAB\xB4\xAC\xF9\xB6\x71",          // 未沖銷契約量
    "\xB3\xCC\xAB\xE1\xB3\xCC\xA8\xCE\xB6\x52\xBB\xF9",          // 最後最佳買價
    "\xB3\xCC\xAB\xE1\xB3\xCC\xA8\xCE\xBD\xE6\xBB\xF9",          // 最後最佳賣價
    "\xBE\xFA\xA5\x76\xB3\xCC\xB0\xAA\xBB\xF9",                  // 歷史最高價
    "\xBE\xFA\xA5\x76\xB3\xCC\xA7\x43\xBB\xF9",                  // 歷史最低價
    "\xAC\x4F\xA7\x5F\xA6\x5D\xB0\x54\xAE\xA7\xAD\xB1\xBC\xC8\xB0\xB1\xA5\xE6\xA9\xF6",  // 是否因訊息面暫停交易
    "\xA5\xE6\xA9\xF6\xAE\xC9\xAC\x71",                                                  // 交易時段
};

constexpr std::string_view kCallLabel = "\xB6\x52\xC5\x76";  // 買權
constexpr std::string_view kPutLabel = "\xBD\xE6\xC5\x76";   // 賣權

constexpr int kDate = 0;
constexpr int kContract = 1;
constexpr int kExpiry = 2;
constexpr int kStrike = 3;
constexpr int kRight = 4;
constexpr int kOpen = 5;
constexpr int kHigh = 6;
constexpr int kLow = 7;
constexpr int kLastTrade = 8;
constexpr int kVolume = 9;
constexpr int kSettlement = 10;
constexpr int kOpenInterest = 11;
constexpr int kLastBestBid = 12;
constexpr int kLastBestAsk = 13;
constexpr int kHistoricalHigh = 14;
constexpr int kHistoricalLow = 15;
constexpr int kNewsHalt = 16;
constexpr int kSession = 17;

OptionRight parse_right(RowFields& fields) {
  if (fields.ok() && fields.field(kRight) == kPutLabel) {
    return OptionRight::kPut;
  }
  if (fields.ok() && fields.field(kRight) != kCallLabel) {
    fields.fail(ErrorCode::kBadRight, kRight);
  }
  return OptionRight::kCall;
}

OptionRow parse_row(RowFields& fields) {
  constexpr auto kPrice = SignPolicy::kNonNegative;
  OptionRow row;
  row.trade_date = fields.date(kDate);
  row.contract = fields.contract(kContract);
  row.expiry = fields.expiry(kExpiry, /*allow_spread=*/false);
  row.strike = fields.strike(kStrike);
  row.right = parse_right(fields);
  row.open_price = fields.number(kOpen, kPrice);
  row.high_price = fields.number(kHigh, kPrice);
  row.low_price = fields.number(kLow, kPrice);
  row.last_trade_price = fields.number(kLastTrade, kPrice);
  row.volume = fields.count(kVolume);
  row.settlement_price = fields.number(kSettlement, kPrice);
  row.open_interest = fields.count(kOpenInterest);
  row.last_best_bid = fields.number(kLastBestBid, kPrice);
  row.last_best_ask = fields.number(kLastBestAsk, kPrice);
  row.historical_high = fields.number(kHistoricalHigh, kPrice);
  row.historical_low = fields.number(kHistoricalLow, kPrice);
  row.news_halt_flag = fields.ok() && !fields.is_empty(kNewsHalt);
  row.session = fields.session(kSession);
  return row;
}

}  // namespace

std::expected<OptionsFile, ReadError> read_options(std::string_view bytes) {
  RecordReader records(bytes);
  if (auto header = check_header(records, kHeader); !header) {
    return std::unexpected(header.error());
  }
  OptionsFile file;
  read_rows(records, kHeader.size(), parse_row, file.rows, file.row_errors);
  return file;
}

}  // namespace aadp::taifex
