#include "bump_study.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "aad_workloads.hpp"
#include "lax_wendroff.hpp"
#include "oracle/tolerance.hpp"
#include "pricer/bump.hpp"
#include "pricer/contract.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"

namespace aadp::bench::aad {

namespace {

constexpr double kUnitRoundoff = 0x1p-53;
constexpr std::array<std::string_view, pricer::kInputCount> kInputNames = {"delta", "vega", "rho",
                                                                           "dividend-rho", "theta"};

double FieldOf(const pricer::Market<double>& m, pricer::Input input) {
  switch (input) {
    case pricer::Input::kSpot:
      return m.spot;
    case pricer::Input::kSigma:
      return m.sigma;
    case pricer::Input::kRate:
      return m.r;
    case pricer::Input::kDividend:
      return m.q;
    case pricer::Input::kMaturity:
      return m.maturity;
  }
  return m.spot;
}

pricer::Revaluation Revalue(const Workload& w, const pricer::Market<double>& m,
                            const pricer::RunConfig& config) {
  auto run = pricer::Revalue(w.contract, m, config);
  if (!run) throw std::runtime_error("the study's bumped market was refused");
  return std::move(*run);
}

}  // namespace

double RoundingCount(double statements, std::uint64_t paths_per_batch, std::uint32_t batches) {
  return (kRoundingsPerStatement * statements) + static_cast<double>(paths_per_batch) +
         static_cast<double>(batches);
}

double GammaOf(double n) {
  const double nu = n * kUnitRoundoff;
  return nu / (1.0 - nu);
}

double RoundoffScale(double rounding_count, double absolute_up, double absolute_down,
                     double denominator) {
  return GammaOf(rounding_count) * (absolute_up + absolute_down) / std::fabs(denominator);
}

double TruncationFactor(pricer::BumpScheme scheme) {
  return scheme == pricer::BumpScheme::kCentral ? 4.0 / 3.0 : 2.0;
}

double CheckAllowance(const StepChoice& step, double roundoff, double critical) {
  return step.truncation + (critical * step.bias_se) + roundoff;
}

pricer::BumpSteps InitialSteps(const pricer::Market<double>& market) {
  return {market.spot / 16.0, 1.0 / 64.0, 1.0 / 64.0, 1.0 / 64.0, market.maturity / 16.0};
}

std::vector<StudyRow> Tabulate(std::span<const StudyPoint> points) {
  std::vector<StudyRow> rows(points.size());
  for (std::size_t j = 0; j < points.size(); ++j) {
    StudyRow& row = rows[j];
    row.halvings = static_cast<std::uint32_t>(j);
    row.h = points[j].h;
    row.estimate = points[j].estimate.mean;
    row.se = points[j].estimate.standard_error;
    row.roundoff = points[j].roundoff;
    if (j + 1 == points.size()) continue;
    const std::vector<double>& a = points[j].estimate.batch_means;
    const std::vector<double>& b = points[j + 1].estimate.batch_means;
    row.has_change = true;
    if (a.size() >= 2 && a.size() == b.size()) {
      std::vector<double> difference(a.size());
      for (std::size_t i = 0; i < a.size(); ++i) difference[i] = a[i] - b[i];
      const BatchStatistics stats = BatchMeansStatistics(difference);
      row.change = stats.mean;
      row.change_se = stats.standard_error;
    } else {
      row.change = points[j].estimate.mean - points[j + 1].estimate.mean;
      row.change_se = 0.0;
    }
    row.limit = points[j].roundoff + points[j + 1].roundoff;
    row.stable = std::fabs(row.change) <= row.limit;  // false on NaN
  }
  return rows;
}

StudyChoice ChooseStep(std::span<const StudyRow> rows, pricer::BumpScheme scheme) {
  StudyChoice choice;
  for (std::size_t j = 0; j + 1 < rows.size(); ++j) {
    if (rows[j].has_change && rows[j].stable && rows[j + 1].has_change && rows[j + 1].stable) {
      choice.chosen = j;
      break;
    }
  }
  if (!choice.chosen) return choice;
  const StudyRow& row = rows[*choice.chosen];
  choice.bias = TruncationFactor(scheme) * row.change;
  choice.bias_se = TruncationFactor(scheme) * row.change_se;
  choice.truncation = TruncationFactor(scheme) * row.limit;
  for (std::size_t j = *choice.chosen + 2; j < rows.size(); ++j) {
    if (rows[j].has_change && !rows[j].stable) {
      choice.violation = j;
      break;
    }
  }
  return choice;
}

std::vector<StudyPoint> PricerStudy(const Workload& workload, const pricer::RunConfig& config,
                                    pricer::Input input, pricer::BumpScheme scheme, double h0,
                                    std::uint32_t halvings, double rounding_count) {
  const bool central = scheme == pricer::BumpScheme::kCentral;
  const double sign = input == pricer::Input::kMaturity ? -1.0 : 1.0;
  std::optional<pricer::Revaluation> base;
  if (!central) base = Revalue(workload, workload.market, config);
  std::vector<StudyPoint> points;
  points.reserve(halvings + 1);
  for (std::uint32_t j = 0; j <= halvings; ++j) {
    const double h = std::ldexp(h0, -static_cast<int>(j));
    const pricer::Market<double> up_market = pricer::Bumped(workload.market, input, h);
    const pricer::Market<double> down_market =
        central ? pricer::Bumped(workload.market, input, -h) : workload.market;
    const pricer::Revaluation up = Revalue(workload, up_market, config);
    const pricer::Revaluation down = central ? Revalue(workload, down_market, config) : *base;
    StudyPoint point;
    point.h = h;
    point.denominator = FieldOf(up_market, input) - FieldOf(down_market, input);
    std::vector<double> means(up.value.batch_means.size());
    for (std::size_t b = 0; b < means.size(); ++b) {
      means[b] = sign * ((up.value.batch_means[b] - down.value.batch_means[b]) / point.denominator);
    }
    const BatchStatistics stats = BatchMeansStatistics(means);
    point.estimate.mean = stats.mean;
    point.estimate.standard_error = stats.standard_error;
    point.estimate.batch_means = std::move(means);
    point.roundoff =
        RoundoffScale(rounding_count, up.mean_absolute, down.mean_absolute, point.denominator);
    points.push_back(std::move(point));
  }
  return points;
}

std::vector<StudyRow> LwStudy(pricer::BumpScheme scheme, double h0, std::uint32_t halvings,
                              double rounding_count) {
  const bool central = scheme == pricer::BumpScheme::kCentral;
  const lw::Field<double> q0 = lw::InitialField();
  const lw::Field<double> w = lw::OutputWeights();
  const double base = lw::Primal(q0, w);
  // points[k][j]: input k at h_j.
  std::vector<std::vector<StudyPoint>> points(lw::kPoints);
  for (std::size_t k = 0; k < lw::kPoints; ++k) {
    for (std::uint32_t j = 0; j <= halvings; ++j) {
      const double h = std::ldexp(h0, -static_cast<int>(j));
      lw::Field<double> up = q0;
      up[k] += h;
      lw::Field<double> down = q0;
      if (central) down[k] -= h;
      const double j_up = lw::Primal(up, w);
      const double j_down = central ? lw::Primal(down, w) : base;
      StudyPoint point;
      point.h = h;
      point.denominator = up[k] - down[k];
      point.estimate.mean = (j_up - j_down) / point.denominator;
      point.roundoff =
          RoundoffScale(rounding_count, std::fabs(j_up), std::fabs(j_down), point.denominator);
      points[k].push_back(std::move(point));
    }
  }
  std::vector<std::vector<StudyRow>> per_input(lw::kPoints);
  for (std::size_t k = 0; k < lw::kPoints; ++k) per_input[k] = Tabulate(points[k]);
  std::vector<StudyRow> rows(halvings + 1);
  for (std::size_t j = 0; j < rows.size(); ++j) {
    std::size_t worst = 0;
    double worst_ratio = -1.0;
    bool all_stable = true;
    for (std::size_t k = 0; k < lw::kPoints; ++k) {
      const StudyRow& r = per_input[k][j];
      if (!r.has_change) continue;
      all_stable = all_stable && r.stable;
      const double ratio = std::fabs(r.change) / r.limit;
      if (!(ratio <= worst_ratio)) {  // NaN is the worst of all
        worst_ratio = std::isnan(ratio) ? HUGE_VAL : ratio;
        worst = k;
      }
    }
    rows[j] = per_input[worst][j];
    rows[j].stable = rows[j].has_change && all_stable;
  }
  return rows;
}

std::string_view SchemeName(pricer::BumpScheme scheme) {
  return scheme == pricer::BumpScheme::kCentral ? "central" : "forward";
}

std::string_view InputName(std::size_t input) {
  return input < kInputNames.size() ? kInputNames[input] : "input";
}

}  // namespace aadp::bench::aad
