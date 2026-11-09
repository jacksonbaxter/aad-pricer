#include "svi/chain_adjoint.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <expected>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include "data/calibration_set.hpp"
#include "data/forwards.hpp"
#include "data/taifex_options.hpp"
#include "svi/calibration_adjoint.hpp"
#include "svi/chain_fit.hpp"
#include "svi/fit.hpp"

namespace aadp::svi {

std::expected<ParityJacobian, ParityJacobianError> DifferentiateParity(
    std::span<const ParityPoint> points) {
  const auto fit = fit_parity(points);
  if (!fit) return std::unexpected(ParityJacobianError::kParity);
  const std::size_t n = points.size();
  const auto count = static_cast<double>(n);
  double mean_x = 0.0;
  for (const ParityPoint& p : points) mean_x += p.strike;
  mean_x /= count;
  double sxx = 0.0;
  for (const ParityPoint& p : points) sxx += (p.strike - mean_x) * (p.strike - mean_x);
  if (!(fit->rms_residual > 0.0)) return std::unexpected(ParityJacobianError::kExactFit);
  const double slope = -fit->discount;
  const double intercept = fit->forward * fit->discount;
  ParityJacobian out;
  out.strike.reserve(n);
  out.d_market.reserve(n);
  for (const ParityPoint& p : points) {
    const double d_slope = (p.strike - mean_x) / sxx;
    const double d_intercept = (1.0 / count) - (mean_x * d_slope);
    const double d_discount = -d_slope;
    const double d_forward = (d_intercept - (fit->forward * d_discount)) / fit->discount;
    const double residual = p.call_minus_put - intercept - (slope * p.strike);
    const double d_leg = residual / (count * fit->rms_residual * std::numbers::sqrt2);
    out.strike.push_back(p.strike);
    out.d_market.push_back({d_forward, d_discount, d_leg});
  }
  return out;
}

std::vector<QuoteLeg> QuoteLegs(const CalibrationSlice& rows, std::span<const ParityPoint> points) {
  std::vector<QuoteLeg> legs;
  legs.reserve(rows.rows.size());
  for (const taifex::OptionRow& row : rows.rows) {
    QuoteLeg leg;
    for (std::size_t j = 0; j < points.size(); ++j) {
      if (points[j].strike == row.strike) {
        leg = {.present = true,
               .point = j,
               .sign = row.right == taifex::OptionRight::kCall ? 1.0 : -1.0};
        break;
      }
    }
    legs.push_back(leg);
  }
  return legs;
}

const char* QuoteChainName(QuoteChain chain) noexcept {
  switch (chain) {
    case QuoteChain::kMarketFixed:
      return "market-fixed";
    case QuoteChain::kThroughParity:
      return "through-parity";
  }
  return "unknown";
}

std::expected<BumpedSlice, ParityError> BumpQuote(const ChainSlice& slice,
                                                  std::span<const ParityPoint> points,
                                                  std::span<const QuoteLeg> legs, std::size_t quote,
                                                  double h, QuoteChain chain) {
  BumpedSlice out{.quotes = slice.quotes, .parity = slice.parity};
  out.quotes.quotes[quote].price += h;
  if (chain == QuoteChain::kMarketFixed) return out;
  std::vector<ParityPoint> moved(points.begin(), points.end());
  if (legs[quote].present) moved[legs[quote].point].call_minus_put += legs[quote].sign * h;
  const auto parity = fit_parity(moved);
  if (!parity) return std::unexpected(parity.error());
  out.parity = *parity;
  out.quotes.forward = parity->forward;
  out.quotes.discount = parity->discount;
  const double old_leg2 = slice.parity.sigma_leg * slice.parity.sigma_leg;
  const double new_leg2 = parity->sigma_leg * parity->sigma_leg;
  for (std::size_t i = 0; i < out.quotes.quotes.size(); ++i) {
    const double noise = slice.quotes.quotes[i].noise;
    const double tau2 = std::max((noise * noise) - old_leg2, 0.0);
    out.quotes.quotes[i].noise = std::sqrt(new_leg2 + tau2);
  }
  return out;
}

std::expected<SliceQuoteSensitivity, ChainAdjointError> SliceQuoteSensitivities(
    const ChainSlice& slice, std::span<const ParityPoint> points, const FitResult& fit,
    const AdjointOptions& options) {
  auto adjoint = CalibrationAdjointOf(slice.quotes, fit, slice.parity.sigma_leg, options);
  if (!adjoint) {
    return std::unexpected(
        ChainAdjointError{.code = ChainAdjointErrorCode::kAdjoint, .adjoint = adjoint.error()});
  }
  const auto parity = DifferentiateParity(points);
  if (!parity) {
    return std::unexpected(
        ChainAdjointError{.code = ChainAdjointErrorCode::kParity, .adjoint = {}});
  }
  SliceQuoteSensitivity out;
  out.legs = QuoteLegs(slice.rows, points);
  const std::size_t n = slice.quotes.quotes.size();
  if (out.legs.size() != n || adjoint->exact.quote.size() != n) {
    return std::unexpected(ChainAdjointError{.code = ChainAdjointErrorCode::kLegs, .adjoint = {}});
  }
  out.adjoint = std::move(*adjoint);
  const auto compose = [&](const ParameterSensitivity& s, std::vector<Vector5>& total) {
    total.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
      total[i] = s.quote[i];
      for (std::size_t p = 0; p < kMarketInputs; ++p) {
        for (std::size_t j = 0; j < kSviParameters; ++j) {
          total[i][j] += s.market[p][j] * out.market[i][p];
        }
      }
    }
  };
  out.market.resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    const QuoteLeg& leg = out.legs[i];
    if (!leg.present) continue;
    for (std::size_t p = 0; p < kMarketInputs; ++p) {
      out.market[i][p] = leg.sign * parity->d_market[leg.point][p];
    }
  }
  compose(out.adjoint.exact, out.through_parity);
  if (out.adjoint.gauss_newton) compose(*out.adjoint.gauss_newton, out.through_parity_gauss_newton);
  return out;
}

HalvingJudgement SelectByHalving(std::span<const double> h, std::span<const double> differences,
                                 std::span<const bool> usable, double refit_error) {
  HalvingJudgement out;
  const std::size_t levels = std::min({h.size(), differences.size(), usable.size()});
  double best = 0.0;
  // A level's truncation is its largest distance from every finer usable level, so a
  // sequence that settles only at small h is not judged by a coarse level's neighbours.
  for (std::size_t l = 0; l + 1 < levels; ++l) {
    if (!usable[l]) continue;
    double truncation = -1.0;
    for (std::size_t m = l + 1; m < levels; ++m) {
      if (usable[m]) truncation = std::max(truncation, std::abs(differences[l] - differences[m]));
    }
    if (truncation < 0.0) continue;
    const double refit = refit_error / h[l];
    const double total = truncation + refit;
    if (!std::isfinite(total)) continue;
    if (!out.resolved || total < best) {
      out.resolved = true;
      best = total;
      out.level = l;
      out.estimate = differences[l];
      out.truncation = truncation;
      out.refit = refit;
    }
  }
  return out;
}

HalvingJudgement JudgeByHalving(std::span<const double> h, std::span<const double> differences,
                                std::span<const bool> usable, double refit_error, double reference,
                                double floor) {
  HalvingJudgement out = SelectByHalving(h, differences, usable, refit_error);
  if (!out.resolved) return out;
  out.tolerance = (kHalvingFactor * (out.truncation + out.refit)) + floor;
  out.error = std::abs(out.estimate - reference);
  out.pass = out.error <= out.tolerance;
  return out;
}

}  // namespace aadp::svi
