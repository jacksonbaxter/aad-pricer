#include "ladder.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "oracle/tolerance.hpp"
#include "runner.hpp"
#include "svi/raw_svi.hpp"

namespace aadp::oracle_runner {
namespace {

std::vector<double> Differences(const std::vector<double>& a, const std::vector<double>& b) {
  std::vector<double> out(std::min(a.size(), b.size()));
  for (std::size_t q = 0; q < out.size(); ++q) out[q] = a[q] - b[q];
  return out;
}

}  // namespace

LadderOutcome ApplyLadderRule(const std::array<double, kLadderLevels>& h,
                              const std::array<std::vector<double>, kLadderLevels>& d,
                              double critical) {
  LadderOutcome out;
  out.levels.resize(kLadderLevels);
  std::array<std::vector<double>, kLadderLevels - 1> delta;
  for (std::size_t l = 0; l < kLadderLevels; ++l) {
    out.levels[l].h = h[l];
    out.levels[l].d = BatchMeansStatistics(d[l]);
  }
  for (std::size_t l = 0; l + 1 < kLadderLevels; ++l) {
    delta[l] = Differences(d[l], d[l + 1]);
    out.levels[l].delta = BatchMeansStatistics(delta[l]);
    out.levels[l].has_delta = true;
  }
  for (std::size_t l = 0; l + 2 < kLadderLevels; ++l) {
    std::vector<double> e(std::min(delta[l].size(), delta[l + 1].size()));
    for (std::size_t q = 0; q < e.size(); ++q) e[q] = delta[l + 1][q] - (delta[l][q] / 4.0);
    LadderLevel& level = out.levels[l];
    level.e = BatchMeansStatistics(e);
    level.has_e = true;
    // False on NaN, so a broken level never counts as consistent.
    level.consistent = std::fabs(level.e.mean) <=
                       (critical * level.e.standard_error) + (std::fabs(level.delta.mean) / 16.0);
  }
  for (std::size_t l = 0; l <= kLastChosenLevel; ++l) {
    if (out.levels[l].consistent && out.levels[l + 1].consistent) {
      out.resolved = true;
      out.level = l;
      out.h_star = h[l + 1];
      out.two_h = out.levels[l].delta;
      out.b = LadderB(out.two_h, critical);
      return out;
    }
  }
  out.h_star = h[kLadderLevels - 1];
  return out;
}

double TotalVarianceSlope(const svi::RawSvi<double>& p, std::size_t parameter, double k) {
  const double x = k - p.m;
  const double r = std::sqrt((x * x) + (p.sigma * p.sigma));
  switch (parameter) {
    case 0:
      return 1.0;
    case 1:
      return (p.rho * x) + r;
    case 2:
      return p.b * x;
    case 3:
      return -p.b * (p.rho + (x / r));
    case 4:
      return p.b * p.sigma / r;
    default:
      return std::nan("");
  }
}

double AtTheMoneyTotalVariance(const svi::RawSvi<double>& p) {
  return p.a + (p.b * ((-p.rho * p.m) + std::sqrt((p.m * p.m) + (p.sigma * p.sigma))));
}

double FirstStep(const svi::RawSvi<double>& p, std::size_t parameter) {
  double largest = 0.0;
  const int half = kStepGridPoints / 2;
  for (int i = 0; i < kStepGridPoints; ++i) {
    const double k = kStepGridHalfWidth * static_cast<double>(i - half) / static_cast<double>(half);
    largest = std::max(largest, std::fabs(TotalVarianceSlope(p, parameter, k)));
  }
  return kStepFraction * AtTheMoneyTotalVariance(p) / largest;
}

std::optional<std::string> StepOutsideDomain(const svi::RawSvi<double>& p, std::size_t parameter,
                                             double h) {
  if (!(h > 0.0) || !std::isfinite(h)) return "the step is not a positive finite number";
  if (parameter == 2 && !(std::fabs(p.rho + h) <= 1.0 && std::fabs(p.rho - h) <= 1.0)) {
    return "rho +- h0 leaves [-1, 1]";
  }
  if (parameter == 4 && !(p.sigma - h >= kMinSurfaceSigmaAfterStep)) {
    return "sigma - h0 falls below 1e-8";
  }
  if (parameter == 1 && !(p.b - h >= 0.0)) return "b - h0 falls below 0";
  return std::nullopt;
}

svi::RawSvi<double> Bumped(svi::RawSvi<double> p, std::size_t parameter, double h) {
  switch (parameter) {
    case 0:
      p.a += h;
      break;
    case 1:
      p.b += h;
      break;
    case 2:
      p.rho += h;
      break;
    case 3:
      p.m += h;
      break;
    case 4:
      p.sigma += h;
      break;
    default:
      break;
  }
  return p;
}

}  // namespace aadp::oracle_runner
