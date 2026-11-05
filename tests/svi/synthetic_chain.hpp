#ifndef AADP_TESTS_SVI_SYNTHETIC_CHAIN_HPP_
#define AADP_TESTS_SVI_SYNTHETIC_CHAIN_HPP_

#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "data/forwards.hpp"
#include "data/taifex_csv.hpp"
#include "data/taifex_options.hpp"
#include "svi/arbitrage.hpp"
#include "svi/implied_vol.hpp"
#include "svi/raw_svi.hpp"

// TXO rows made up from hand-written SVI parameters: Black-76 prices of a call and a put at every
// strike, rounded to 0.1 point, traded once with the settlement equal to the last trade. Nothing
// here comes from a TAIFEX file; only the expiry labels are real, because T is defined for them.
namespace aadp::testing {

struct SyntheticExpiry {
  std::string expiry;
  double forward = 0.0;
  double discount = 1.0;
  svi::SviParams params;
};

inline double RoundToTenth(double price) { return std::round(price * 10.0) / 10.0; }

inline std::vector<taifex::OptionRow> SyntheticRows(std::span<const SyntheticExpiry> expiries,
                                                    double first_strike, double last_strike,
                                                    double step,
                                                    std::int32_t trade_date = kSnapshotTradeDate) {
  std::vector<taifex::OptionRow> rows;
  for (const SyntheticExpiry& e : expiries) {
    const double years = txo_years_to_expiry(e.expiry).value_or(1.0);
    for (int i = 0;; ++i) {
      const double strike = first_strike + (step * i);
      if (strike > last_strike) break;
      const double w = svi::TotalVariance(e.params, std::log(strike / e.forward));
      for (const svi::OptionType type : {svi::OptionType::kCall, svi::OptionType::kPut}) {
        const double price = RoundToTenth(
            svi::Black76Price(e.forward, e.discount, strike, years, std::sqrt(w / years), type));
        taifex::OptionRow row;
        row.trade_date = trade_date;
        row.contract = "TXO";
        row.expiry = e.expiry;
        row.strike = strike;
        row.right =
            type == svi::OptionType::kCall ? taifex::OptionRight::kCall : taifex::OptionRight::kPut;
        row.last_trade_price = price;
        row.settlement_price = price;
        row.volume = 1;
        row.open_interest = 1;
        row.session = taifex::Session::kRegular;
        rows.push_back(row);
      }
    }
  }
  return rows;
}

}  // namespace aadp::testing

#endif  // AADP_TESTS_SVI_SYNTHETIC_CHAIN_HPP_
