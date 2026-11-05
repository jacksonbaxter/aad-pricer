#include "svi/chain_fit.hpp"

#include <cmath>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "data/calibration_set.hpp"
#include "data/forwards.hpp"
#include "data/taifex_futures.hpp"
#include "data/taifex_options.hpp"
#include "svi/arbitrage.hpp"
#include "svi/fit.hpp"
#include "svi/implied_vol.hpp"

namespace aadp::svi {

namespace {

std::unexpected<ChainError> Fail(ChainErrorCode code, std::string_view expiry,
                                 ParityError parity = ParityError::kTooFewPoints) {
  return std::unexpected(
      ChainError{.code = code, .expiry = std::string(expiry), .parity_error = parity});
}

FitSlice Quotes(const CalibrationSlice& rows, const ParityFit& parity, double years) {
  FitSlice out{
      .forward = parity.forward, .discount = parity.discount, .expiry = years, .quotes = {}};
  out.quotes.reserve(rows.rows.size());
  const double leg2 = parity.sigma_leg * parity.sigma_leg;
  for (const taifex::OptionRow& row : rows.rows) {
    const double price = row.settlement_price.value_or(0.0);
    const double tick = settlement_tick(price);
    out.quotes.push_back(
        {.strike = row.strike,
         .price = price,
         .type = row.right == taifex::OptionRight::kPut ? OptionType::kPut : OptionType::kCall,
         .noise = std::sqrt(leg2 + (tick * tick / 12.0))});
  }
  return out;
}

}  // namespace

const char* ChainErrorName(ChainErrorCode code) noexcept {
  switch (code) {
    case ChainErrorCode::kWrongTradeDate:
      return "wrong-trade-date";
    case ChainErrorCode::kUnknownExpiry:
      return "unknown-expiry";
    case ChainErrorCode::kParity:
      return "parity";
    case ChainErrorCode::kCalibrationSet:
      return "calibration-set";
  }
  return "unknown";
}

std::expected<std::vector<ChainSlice>, ChainError> BuildChainSlices(
    std::span<const taifex::OptionRow> options, std::span<const taifex::FuturesRow> futures,
    std::span<const std::string_view> expiries) {
  std::vector<ChainSlice> out;
  std::vector<ExpiryForward> forwards;
  for (const std::string_view expiry : expiries) {
    for (const taifex::OptionRow& row : options) {
      if (row.contract == kCalibrationContract && row.expiry == expiry &&
          row.trade_date != kSnapshotTradeDate) {
        return Fail(ChainErrorCode::kWrongTradeDate, expiry);
      }
    }
    const std::optional<double> years = txo_years_to_expiry(expiry);
    if (!years) return Fail(ChainErrorCode::kUnknownExpiry, expiry);
    const auto parity = fit_parity(settlement_parity_points(options, expiry));
    if (!parity) return Fail(ChainErrorCode::kParity, expiry, parity.error());
    ChainSlice slice;
    slice.expiry = std::string(expiry);
    slice.years = *years;
    slice.parity = *parity;
    slice.tx_settlement = tx_settlement(futures, expiry);
    out.push_back(std::move(slice));
    forwards.push_back({.expiry = std::string(expiry), .forward = parity->forward});
  }
  auto rows = select_calibration_set(options, forwards);
  if (!rows) return Fail(ChainErrorCode::kCalibrationSet, "");
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i].rows = std::move((*rows)[i]);
    out[i].input_butterflies = count_input_butterflies(out[i].rows);
    out[i].quotes = Quotes(out[i].rows, out[i].parity, out[i].years);
  }
  return out;
}

SurfaceFit FitChain(std::span<const ChainSlice> slices, const FitOptions& options) {
  SurfaceFit out;
  out.slices.reserve(slices.size());
  for (const ChainSlice& slice : slices) {
    SliceFit result;
    result.fit = FitRawSvi(slice.quotes, options);
    if (result.fit) {
      const FitResult& fit = *result.fit;
      result.svi6_bp = MeanAbsPriceErrorBp(slice.quotes, fit.model_price);
      result.svi11_vol_points = ImpliedVolRmse(fit.model_vol, fit.market_vol);
      result.butterfly = CheckButterfly(fit.params);
      std::vector<StrikePrice> model;
      model.reserve(fit.model_price.size());
      for (std::size_t i = 0; i < fit.model_price.size(); ++i) {
        model.push_back({.strike = slice.rows.rows[i].strike,
                         .price = fit.model_price[i],
                         .right = slice.rows.rows[i].right});
      }
      result.model_butterflies = count_butterflies(model);
    }
    out.slices.push_back(std::move(result));
  }
  for (std::size_t i = 0; i + 1 < out.slices.size(); ++i) {
    // Adjacent in the order given; the check needs the shorter expiry first.
    const bool in_order = slices[i].years <= slices[i + 1].years;
    const std::size_t e = in_order ? i : i + 1;
    const std::size_t l = in_order ? i + 1 : i;
    const auto& earlier = out.slices[e].fit;
    const auto& later = out.slices[l].fit;
    if (earlier && later) {
      out.calendar.push_back(
          {.earlier = e, .later = l, .report = CheckCalendar(earlier->params, later->params)});
    }
  }
  return out;
}

}  // namespace aadp::svi
