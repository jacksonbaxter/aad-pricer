// Prints per-expiry counts for the TAIFEX captures, through the readers.
//
//   chain_profile [--options <file>] [--futures <file>]
//
// The output is derived from the chain, so it opens with the attribution.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iomanip>
#include <iostream>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "data/attribution.hpp"
#include "data/calibration_set.hpp"
#include "data/taifex_csv.hpp"
#include "data/taifex_futures.hpp"
#include "data/taifex_options.hpp"

namespace {

using aadp::taifex::OptionRight;
using aadp::taifex::OptionRow;
using aadp::taifex::ReadError;
using aadp::taifex::Session;

// Published in NOTES.md, "Recommended calibration set". The regression behind them is Phase 3.
std::vector<aadp::ExpiryForward> published_forwards() {
  return {{.expiry = "202411", .forward = 22761.47},
          {.expiry = "202411W4", .forward = 22786.07},
          {.expiry = "202412", .forward = 22792.92}};
}

constexpr int kMaxErrorsShown = 20;

struct ExpiryCounts {
  int regular = 0;
  int after_hours = 0;
  int settled = 0;
  int traded = 0;
  int settlement_is_last_trade = 0;
};

void print_errors(std::string_view label, std::span<const ReadError> errors) {
  std::cout << label << " row errors: " << errors.size() << '\n';
  int shown = 0;
  for (const ReadError& error : errors) {
    if (shown++ == kMaxErrorsShown) {
      std::cout << "  ...\n";
      break;
    }
    std::cout << "  line " << error.line << ", column " << error.column << ": "
              << aadp::taifex::to_string(error.code) << '\n';
  }
}

void print_options(const aadp::taifex::OptionsFile& file) {
  std::map<std::int32_t, int> dates;
  std::map<std::string, ExpiryCounts> expiries;
  int txo_rows = 0;
  for (const OptionRow& row : file.rows) {
    ++dates[row.trade_date];
    if (row.contract != aadp::kCalibrationContract) {
      continue;
    }
    ++txo_rows;
    ExpiryCounts& counts = expiries[row.expiry];
    if (row.session == Session::kAfterHours) {
      ++counts.after_hours;
      continue;
    }
    ++counts.regular;
    counts.settled += row.settlement_price.has_value() ? 1 : 0;
    const bool traded = row.volume.value_or(0) > 0;
    counts.traded += traded ? 1 : 0;
    counts.settlement_is_last_trade +=
        traded && row.settlement_price.has_value() && row.settlement_price == row.last_trade_price
            ? 1
            : 0;
  }

  std::cout << "options rows: " << file.rows.size() << '\n';
  for (const auto& [date, count] : dates) {
    std::cout << "  trade date " << date << ": " << count << '\n';
  }
  print_errors("options", file.row_errors);

  std::cout << '\n' << aadp::kCalibrationContract << " rows: " << txo_rows << '\n';
  std::cout << std::left << std::setw(10) << "expiry" << std::right << std::setw(9) << "regular"
            << std::setw(12) << "after-hours" << std::setw(9) << "settled" << std::setw(8)
            << "traded" << std::setw(15) << "settle==last" << '\n';
  int regular = 0;
  int after_hours = 0;
  for (const auto& [expiry, counts] : expiries) {
    regular += counts.regular;
    after_hours += counts.after_hours;
    std::cout << std::left << std::setw(10) << expiry << std::right << std::setw(9)
              << counts.regular << std::setw(12) << counts.after_hours << std::setw(9)
              << counts.settled << std::setw(8) << counts.traded << std::setw(15)
              << counts.settlement_is_last_trade << '\n';
  }
  std::cout << std::left << std::setw(10) << "total" << std::right << std::setw(9) << regular
            << std::setw(12) << after_hours << '\n';
  std::cout << "Counts in the last three columns are regular-session rows only.\n";
}

void print_calibration_set(const aadp::taifex::OptionsFile& file) {
  const auto slices = aadp::select_calibration_set(file.rows, published_forwards());
  if (!slices) {
    std::cout << "\ncalibration set: bad forwards\n";
    return;
  }
  std::cout << "\nCalibration set, from settlements, which are not tradable prices.\n";
  std::cout << std::left << std::setw(10) << "expiry" << std::right << std::setw(11) << "forward"
            << std::setw(5) << "n" << std::setw(6) << "puts" << std::setw(7) << "calls" << '\n';
  for (const aadp::CalibrationSlice& slice : *slices) {
    int puts = 0;
    for (const OptionRow& row : slice.rows) {
      puts += row.right == OptionRight::kPut ? 1 : 0;
    }
    const int total = static_cast<int>(slice.rows.size());
    std::cout << std::left << std::setw(10) << slice.expiry << std::right << std::setw(11)
              << std::fixed << std::setprecision(2) << slice.forward << std::setw(5) << total
              << std::setw(6) << puts << std::setw(7) << total - puts << '\n';
  }
}

void print_futures(const aadp::taifex::FuturesFile& file) {
  int spreads = 0;
  int regular = 0;
  for (const aadp::taifex::FuturesRow& row : file.rows) {
    spreads += row.calendar_spread ? 1 : 0;
    regular += row.session == Session::kRegular ? 1 : 0;
  }
  std::cout << "\nfutures rows: " << file.rows.size() << ", regular " << regular << ", after-hours "
            << static_cast<int>(file.rows.size()) - regular << ", calendar spreads " << spreads
            << '\n';
  print_errors("futures", file.row_errors);
}

int usage() {
  std::cerr << "usage: chain_profile [--options <file>] [--futures <file>]\n";
  return 2;
}

}  // namespace

int main(int argc, char** argv) try {
  std::string options_path = "data/DailyMarketReportOpt.csv";
  std::string futures_path = "data/DailyMarketReportFut.csv";
  const std::span<char*> args(argv, static_cast<std::size_t>(argc));
  for (std::size_t i = 1; i < args.size(); i += 2) {
    const std::string_view flag = args[i];
    if (i + 1 == args.size()) {
      return usage();
    }
    if (flag == "--options") {
      options_path = args[i + 1];
    } else if (flag == "--futures") {
      futures_path = args[i + 1];
    } else {
      return usage();
    }
  }

  const auto option_bytes = aadp::taifex::read_binary_file(options_path);
  if (!option_bytes) {
    std::cerr << "chain_profile: " << option_bytes.error() << ". Run scripts/fetch_data.sh.\n";
    return 1;
  }
  const auto future_bytes = aadp::taifex::read_binary_file(futures_path);
  if (!future_bytes) {
    std::cerr << "chain_profile: " << future_bytes.error() << ". Run scripts/fetch_data.sh.\n";
    return 1;
  }
  const auto options = aadp::taifex::read_options(*option_bytes);
  if (!options) {
    std::cerr << "chain_profile: " << options_path << ": "
              << aadp::taifex::to_string(options.error().code) << '\n';
    return 1;
  }
  const auto futures = aadp::taifex::read_futures(*future_bytes);
  if (!futures) {
    std::cerr << "chain_profile: " << futures_path << ": "
              << aadp::taifex::to_string(futures.error().code) << '\n';
    return 1;
  }

  aadp::write_attribution(std::cout);
  std::cout << '\n';
  print_options(*options);
  print_calibration_set(*options);
  print_futures(*futures);
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "chain_profile: %s\n", error.what());
  return 2;
}
