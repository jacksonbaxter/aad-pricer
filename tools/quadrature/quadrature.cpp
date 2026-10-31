#include "quadrature.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <string_view>

#include "oracle/barrier.hpp"
#include "oracle/black_scholes.hpp"
#include "oracle/catalog.hpp"
#include "oracle/geometric_asian.hpp"
#include "rng/normal.hpp"
#include "support/dual.hpp"
#include "support/mc_support.hpp"

namespace aadp::quadrature {
namespace {

constexpr double kInverseSqrtTwoPi = 0.5 * std::numbers::sqrt2 * std::numbers::inv_sqrtpi;

struct Rule {
  std::vector<double> nodes;
  std::vector<double> weights;
};

// On [-1, 1]. Newton on the three-term Legendre recurrence.
Rule GaussLegendre(int degree) {
  Rule rule;
  for (int i = 0; i < degree; ++i) {
    double x = std::cos(std::numbers::pi * (i + 0.75) / (degree + 0.5));
    double slope = 0.0;
    for (int iteration = 0; iteration < 100; ++iteration) {
      double previous = 1.0;
      double current = x;
      for (int k = 2; k <= degree; ++k) {
        const double next = ((2 * k - 1) * x * current - (k - 1) * previous) / k;
        previous = current;
        current = next;
      }
      slope = degree * (x * current - previous) / (x * x - 1.0);
      const double step = current / slope;
      x -= step;
      if (std::fabs(step) < 1e-16) break;
    }
    rule.nodes.push_back(x);
    rule.weights.push_back(2.0 / ((1.0 - x * x) * slope * slope));
  }
  return rule;
}

// One discretised interval of the GBM log price.
struct LogStep {
  double drift;
  double sd;
  double discount;

  explicit LogStep(const BarrierInputs& in) {
    const double dt = in.maturity / in.monitors;
    drift = (in.r - in.q - 0.5 * in.sigma * in.sigma) * dt;
    sd = in.sigma * std::sqrt(dt);
    discount = std::exp(-in.r * dt);
  }
};

struct LastStepValues {
  double payoff;  // E[e^{-r dt} (e^X - K)^+ ; X > log max(H, K) | x].
  double share;   // E[e^{-r dt} e^X ; X > log max(H, K) | x], the naive-delta terminal term.
};

LastStepValues LastStep(double x, const LogStep& step, double log_floor, double strike) {
  const double a = (x + step.drift - log_floor) / step.sd;
  const double forward = std::exp(x + step.drift + 0.5 * step.sd * step.sd);
  const double share = step.discount * forward * normal_cdf(a + step.sd);
  return {.payoff = share - step.discount * strike * normal_cdf(a), .share = share};
}

// Banded kernel on non-uniform nodes, stored row by row.
class BandedKernel {
 public:
  BandedKernel(const std::vector<double>& nodes, const std::vector<double>& weights, double drift,
               double sd, double width) {
    const std::size_t size = nodes.size();
    start_.resize(size + 1);
    first_.resize(size);
    const double norm = kInverseSqrtTwoPi / sd;
    for (std::size_t i = 0; i < size; ++i) {
      const double centre = nodes[i] + drift;
      const auto lo = std::ranges::lower_bound(nodes, centre - width * sd);
      const auto hi = std::upper_bound(lo, nodes.end(), centre + width * sd);
      first_[i] = static_cast<std::size_t>(lo - nodes.begin());
      start_[i] = values_.size();
      for (auto it = lo; it != hi; ++it) {
        const double z = (*it - centre) / sd;
        values_.push_back(norm * weights[static_cast<std::size_t>(it - nodes.begin())] *
                          std::exp(-0.5 * z * z));
      }
    }
    start_[size] = values_.size();
  }

  void Apply(const std::vector<double>& in, double scale, std::vector<double>& out) const {
    for (std::size_t i = 0; i < first_.size(); ++i) {
      double sum = 0.0;
      std::size_t column = first_[i];
      for (std::size_t k = start_[i]; k < start_[i + 1]; ++k) sum += values_[k] * in[column++];
      out[i] = scale * sum;
    }
  }

 private:
  std::vector<std::size_t> start_;
  std::vector<std::size_t> first_;
  std::vector<double> values_;
};

}  // namespace

BarrierResult DiscreteDownOutGaussLegendre(const BarrierInputs& in,
                                           const GaussLegendreResolution& resolution) {
  const LogStep step(in);
  const double log_barrier = std::log(in.barrier);
  const double log_spot = std::log(in.spot);
  const double top = log_spot + resolution.top_sd * in.sigma * std::sqrt(in.maturity);
  const auto panels =
      static_cast<std::size_t>(std::ceil((top - log_barrier) * resolution.panels_per_sd / step.sd));
  const double width = (top - log_barrier) / static_cast<double>(panels);
  const Rule rule = GaussLegendre(resolution.degree);

  std::vector<double> nodes;
  std::vector<double> weights;
  for (std::size_t panel = 0; panel < panels; ++panel) {
    const double left = log_barrier + static_cast<double>(panel) * width;
    for (std::size_t i = 0; i < rule.nodes.size(); ++i) {
      nodes.push_back(left + 0.5 * width * (rule.nodes[i] + 1.0));
      weights.push_back(0.5 * width * rule.weights[i]);
    }
  }
  const std::size_t size = nodes.size();
  const BandedKernel kernel(nodes, weights, step.drift, step.sd, resolution.kernel_sd);

  const double log_floor = std::log(std::max(in.barrier, in.strike));
  std::vector<double> value(size);
  std::vector<double> share(size);
  for (std::size_t i = 0; i < size; ++i) {
    const LastStepValues last = LastStep(nodes[i], step, log_floor, in.strike);
    value[i] = last.payoff;
    share[i] = last.share;
  }
  std::vector<double> scratch(size);
  for (std::uint32_t date = 2; date + 1 <= in.monitors; ++date) {
    kernel.Apply(value, step.discount, scratch);
    value.swap(scratch);
    kernel.Apply(share, step.discount, scratch);
    share.swap(scratch);
  }

  // The root is off the grid, and the score of the first Gaussian step gives delta directly.
  double price = 0.0;
  double score = 0.0;
  double naive = 0.0;
  const double norm = kInverseSqrtTwoPi / step.sd;
  for (std::size_t j = 0; j < size; ++j) {
    const double offset = nodes[j] - log_spot - step.drift;
    const double z = offset / step.sd;
    const double density = norm * weights[j] * std::exp(-0.5 * z * z);
    price += density * value[j];
    score += density * value[j] * offset / (step.sd * step.sd);
    naive += density * share[j];
  }
  return {.value = step.discount * price,
          .delta = step.discount * score / in.spot,
          .naive_delta = step.discount * naive / in.spot,
          .nodes = size};
}

namespace {

BarrierResult SimpsonOnGrid(const BarrierInputs& in, const SimpsonResolution& resolution,
                            std::size_t intervals) {
  const LogStep step(in);
  const double log_barrier = std::log(in.barrier);
  const double top = std::log(in.spot) + resolution.top_sd * in.sigma * std::sqrt(in.maturity);
  const double dx = (top - log_barrier) / static_cast<double>(intervals);
  const std::size_t size = intervals + 1;
  std::vector<double> weight(size, dx / 3.0 * 2.0);
  weight.front() = weight.back() = dx / 3.0;
  for (std::size_t i = 1; i < intervals; i += 2) weight[i] = dx / 3.0 * 4.0;
  const auto node = [&](std::size_t i) { return log_barrier + static_cast<double>(i) * dx; };

  // The grid is uniform, so the kernel depends only on the index offset.
  const auto band = static_cast<std::ptrdiff_t>(resolution.kernel_sd * step.sd / dx) + 2;
  std::vector<double> shifted(static_cast<std::size_t>(2 * band + 1));
  for (std::ptrdiff_t d = -band; d <= band; ++d) {
    const double z = (static_cast<double>(d) * dx - step.drift) / step.sd;
    shifted[static_cast<std::size_t>(d + band)] = std::exp(-0.5 * z * z);
  }
  const double norm = step.discount * kInverseSqrtTwoPi / step.sd;

  const double log_floor = std::log(std::max(in.barrier, in.strike));
  std::vector<double> value(size);
  std::vector<double> share(size);
  for (std::size_t i = 0; i < size; ++i) {
    const LastStepValues last = LastStep(node(i), step, log_floor, in.strike);
    value[i] = last.payoff;
    share[i] = last.share;
  }
  const auto last_index = static_cast<std::ptrdiff_t>(intervals);
  std::vector<double> scratch(size);
  const auto backward = [&](std::vector<double>& field) {
    for (std::ptrdiff_t i = 0; i <= last_index; ++i) {
      double sum = 0.0;
      const std::ptrdiff_t lo = std::max<std::ptrdiff_t>(0, i - band);
      const std::ptrdiff_t hi = std::min(last_index, i + band);
      const double* kernel = shifted.data() + (lo - i + band);
      for (std::ptrdiff_t j = lo; j <= hi; ++j) {
        const auto index = static_cast<std::size_t>(j);
        sum += *kernel++ * weight[index] * field[index];
      }
      scratch[static_cast<std::size_t>(i)] = norm * sum;
    }
    field.swap(scratch);
  };
  for (std::uint32_t date = 2; date + 1 <= in.monitors; ++date) {
    backward(value);
    backward(share);
  }

  const auto at_spot = [&](const std::vector<double>& field, double spot) {
    const double centre = std::log(spot) + step.drift;
    double sum = 0.0;
    for (std::size_t j = 0; j < size; ++j) {
      const double z = (node(j) - centre) / step.sd;
      sum += std::exp(-0.5 * z * z) * weight[j] * field[j];
    }
    return norm * sum;
  };
  const double e = resolution.spot_stencil;
  const double delta = (-at_spot(value, in.spot + 2 * e) + 8 * at_spot(value, in.spot + e) -
                        8 * at_spot(value, in.spot - e) + at_spot(value, in.spot - 2 * e)) /
                       (12 * e);
  return {.value = at_spot(value, in.spot),
          .delta = delta,
          .naive_delta = at_spot(share, in.spot) / in.spot,
          .nodes = size};
}

}  // namespace

// Simpson's error has a dx^4 leading term, so one extrapolation on a grid and its half removes
// it at a quarter more work than the fine grid alone.
BarrierResult DiscreteDownOutSimpson(const BarrierInputs& in, const SimpsonResolution& resolution) {
  const LogStep step(in);
  const double range =
      std::log(in.spot / in.barrier) + resolution.top_sd * in.sigma * std::sqrt(in.maturity);
  auto intervals =
      static_cast<std::size_t>(std::ceil(range * resolution.intervals_per_sd / step.sd));
  intervals += intervals % 2;
  const BarrierResult coarse = SimpsonOnGrid(in, resolution, intervals);
  const BarrierResult fine = SimpsonOnGrid(in, resolution, 2 * intervals);
  const auto extrapolate = [](double c, double f) { return (16.0 * f - c) / 15.0; };
  return {.value = extrapolate(coarse.value, fine.value),
          .delta = extrapolate(coarse.delta, fine.delta),
          .naive_delta = extrapolate(coarse.naive_delta, fine.naive_delta),
          .nodes = fine.nodes};
}

AsianResult ArithmeticAsianCall(const AsianInputs& in, const AsianResolution& resolution) {
  const double steps = in.steps;
  const double dt = in.maturity / steps;
  const double drift = (in.r - in.q - 0.5 * in.sigma * in.sigma) * dt;
  const double sd = in.sigma * std::sqrt(dt);
  const double spacing = sd / resolution.nodes_per_sd;
  const double offset_strike = in.strike * (steps + 1.0) / in.spot - 1.0;
  if (!(offset_strike > 0.0) || in.steps < 2) {
    return {.value = std::numeric_limits<double>::quiet_NaN(),
            .mass_error = 0.0,
            .martingale_error = 0.0,
            .nodes = 0};
  }
  const double norm = kInverseSqrtTwoPi / sd;

  const auto make_grid = [&](double mean, double spread) {
    const auto count = static_cast<std::size_t>(2.0 * resolution.grid_sd * spread / spacing) + 1;
    std::vector<double> grid(count);
    for (std::size_t i = 0; i < count; ++i) {
      grid[i] = mean - resolution.grid_sd * spread + static_cast<double>(i) * spacing;
    }
    return grid;
  };

  // Density of log R_n, then of log Y_k for k = n - 1 down to 2.
  std::vector<double> grid = make_grid(drift, sd);
  std::vector<double> density(grid.size());
  for (std::size_t i = 0; i < grid.size(); ++i) {
    const double z = (grid[i] - drift) / sd;
    density[i] = norm * std::exp(-0.5 * z * z);
  }
  std::vector<double> shifted;
  for (std::uint32_t k = in.steps - 1; k >= 2; --k) {
    shifted.resize(grid.size());
    double mean = 0.0;
    for (std::size_t i = 0; i < grid.size(); ++i) {
      shifted[i] = std::log1p(std::exp(grid[i]));
      mean += shifted[i] * density[i];
    }
    mean *= spacing;
    double variance = 0.0;
    for (std::size_t i = 0; i < grid.size(); ++i) {
      variance += (shifted[i] - mean) * (shifted[i] - mean) * density[i];
    }
    variance *= spacing;

    std::vector<double> next_grid = make_grid(drift + mean, std::sqrt(sd * sd + variance));
    std::vector<double> next(next_grid.size());
    for (std::size_t i = 0; i < next_grid.size(); ++i) {
      const double centre = next_grid[i] - drift;
      const auto lo = std::ranges::lower_bound(shifted, centre - resolution.kernel_sd * sd);
      const auto hi = std::upper_bound(lo, shifted.end(), centre + resolution.kernel_sd * sd);
      double sum = 0.0;
      for (auto it = lo; it != hi; ++it) {
        const double z = (centre - *it) / sd;
        sum += std::exp(-0.5 * z * z) * density[static_cast<std::size_t>(it - shifted.begin())];
      }
      next[i] = sum * norm * spacing;
    }
    grid = std::move(next_grid);
    density = std::move(next);
  }

  // E[(R_1 A - c)^+], A = 1 + exp(z), closed form in R_1.
  const double forward = std::exp(drift + 0.5 * sd * sd);
  double total = 0.0;
  double mass = 0.0;
  double first_moment = 0.0;
  for (std::size_t i = 0; i < grid.size(); ++i) {
    const double a = 1.0 + std::exp(grid[i]);
    const double d2 = (drift + std::log(a / offset_strike)) / sd;
    total += density[i] * (a * forward * normal_cdf(d2 + sd) - offset_strike * normal_cdf(d2));
    mass += density[i];
    first_moment += density[i] * a * forward;
  }
  total *= spacing;
  mass *= spacing;
  first_moment *= spacing;
  double exact_moment = 0.0;
  for (std::uint32_t k = 1; k <= in.steps; ++k) exact_moment += std::exp((in.r - in.q) * k * dt);
  return {.value = std::exp(-in.r * in.maturity) * in.spot / (steps + 1.0) * total,
          .mass_error = mass - 1.0,
          .martingale_error = first_moment / exact_moment - 1.0,
          .nodes = grid.size()};
}

double ArithmeticAsianBruteForce3(const AsianInputs& in, int nodes, double span) {
  const double dt = in.maturity / 3.0;
  const double drift = (in.r - in.q - 0.5 * in.sigma * in.sigma) * dt;
  const double sd = in.sigma * std::sqrt(dt);
  const Rule rule = GaussLegendre(nodes);
  std::vector<double> points;
  std::vector<double> masses;
  for (std::size_t i = 0; i < rule.nodes.size(); ++i) {
    const double z = span * rule.nodes[i];
    points.push_back(z);
    masses.push_back(span * rule.weights[i] * normal_pdf(z));
  }
  const double forward = std::exp(drift + 0.5 * sd * sd);
  const double target = in.strike * 4.0 / in.spot;
  double total = 0.0;
  for (std::size_t i = 0; i < points.size(); ++i) {
    const double r1 = std::exp(drift + sd * points[i]);
    for (std::size_t j = 0; j < points.size(); ++j) {
      const double r2 = std::exp(drift + sd * points[j]);
      const double known = 1.0 + r1 + r1 * r2;
      const double scale = r1 * r2;
      const double gap = target - known;
      double payoff = 0.0;
      if (gap <= 0.0) {
        payoff = known - target + scale * forward;
      } else {
        const double d2 = (drift + std::log(scale / gap)) / sd;
        payoff = scale * forward * normal_cdf(d2 + sd) - gap * normal_cdf(d2);
      }
      total += masses[i] * masses[j] * payoff;
    }
  }
  return std::exp(-in.r * in.maturity) * in.spot / 4.0 * total;
}

std::vector<double> ArithmeticAsianRichardson(const AsianInputs& in, std::uint32_t first_steps,
                                              int levels, const AsianResolution& resolution) {
  std::vector<double> column;
  std::vector<double> diagonal;
  for (int level = 0; level < levels; ++level) {
    AsianInputs at_level = in;
    at_level.steps = first_steps * (1 << level);
    double value = ArithmeticAsianCall(at_level, resolution).value;
    std::vector<double> updated{value};
    for (std::size_t k = 1; k <= column.size(); ++k) {
      const double power = std::ldexp(1.0, static_cast<int>(k));
      value = (power * value - column[k - 1]) / (power - 1.0);
      updated.push_back(value);
    }
    column = std::move(updated);
    diagonal.push_back(value);
  }
  return diagonal;
}

double ContinuousDownOutCallBgk(double spot, double strike, double barrier, double r, double q,
                                double sigma, double maturity) {
  const auto call = [&](double s) {
    const MarketParams<double> m{.spot = s, .sigma = sigma, .r = r, .q = q, .maturity = maturity};
    return BlackScholes(OptionType::kCall, strike, m).value;
  };
  const double exponent = 2.0 * (r - q) / (sigma * sigma) - 1.0;
  return call(spot) - std::pow(barrier / spot, exponent) * call(barrier * barrier / spot);
}

namespace {

constexpr const char* kHaugGreeks =
    "Haug 1998 pp. 11-16 via QuantLib 5d6defcc test-suite/europeanoption.cpp";
constexpr const char* kBgk1997 = "BGK 1997 Tables 2.1 and 2.4 (Math. Finance 7(4))";
constexpr const char* kBgk1999 = "BGK 1999 Table 1 (Finance Stoch. 3)";
constexpr const char* kFusaiMeucci = "Fusai and Meucci Table 6 (JBF 2008)";
constexpr const char* kDerived = "derived; see tools/quadrature";

constexpr BarrierInputs kBarrierH95{.spot = 100,
                                    .strike = 100,
                                    .barrier = 95,
                                    .r = 0.10,
                                    .q = 0.0,
                                    .sigma = 0.30,
                                    .maturity = 0.2,
                                    .monitors = 50};
constexpr AsianInputs kFusaiMeucciInputs{.spot = 100,
                                         .strike = 100,
                                         .r = 0.0367,
                                         .q = 0.0,
                                         .sigma = 0.17801,
                                         .maturity = 1.0,
                                         .steps = 50};

void Require(bool ok, const std::string& what, std::vector<std::string>& failures) {
  if (!ok) failures.push_back(what);
}

std::string Describe(const char* label, double a, double b) {
  std::array<char, 160> text{};
  std::snprintf(text.data(), text.size(), "%s: %.12f against %.12f, difference %.3e", label, a, b,
                std::fabs(a - b));
  return text.data();
}

struct GreekColumn {
  const char* suffix;
  Greek greek;
};

constexpr auto kGreekColumns = std::to_array<GreekColumn>({
    {.suffix = "value", .greek = Greek::kValue},
    {.suffix = "delta", .greek = Greek::kDelta},
    {.suffix = "vega", .greek = Greek::kVega},
    {.suffix = "rho", .greek = Greek::kRho},
    {.suffix = "divrho", .greek = Greek::kDividendRho},
    {.suffix = "theta", .greek = Greek::kTheta},
});

std::string StripKind(const char* instrument) {
  const std::string_view text(instrument);
  return std::string(text.substr(text.find(':') + 1));
}

// Six rows of a closed form through Dual. A published id replaces the generated one, so the
// plan's O-rows have one home.
template <class Value>
void AddClosedForm(std::vector<ReferenceRow>& rows, const std::string& prefix,
                   const char* instrument, const MarketParams<double>& m, const Value& value,
                   const std::string& method, const std::string& source,
                   const std::string& override_suffix = "", const std::string& override_id = "") {
  const test::Dual out = value(test::ActiveMarket(m));
  for (const GreekColumn& column : kGreekColumns) {
    std::string id = prefix + "." + column.suffix;
    if (column.suffix == override_suffix) id = override_id;
    rows.push_back({.id = id,
                    .parameters = StripKind(instrument),
                    .value = test::ReadGreek(out, column.greek),
                    .b = 0.0,
                    .method = method,
                    .source = source});
  }
}

void AddDiscreteBarrier(std::vector<ReferenceRow>& rows, std::vector<std::string>& failures,
                        const std::string& prefix, const char* instrument, const BarrierInputs& in,
                        const std::string& value_id, const std::string& delta_id, double value_b,
                        const char* source) {
  const GaussLegendreResolution coarse{};
  const GaussLegendreResolution fine{
      .panels_per_sd = 3.0, .degree = 10, .top_sd = 14.0, .kernel_sd = 15.0};
  const BarrierResult gl = DiscreteDownOutGaussLegendre(in, coarse);
  const BarrierResult gl_fine = DiscreteDownOutGaussLegendre(in, fine);
  const BarrierResult simpson = DiscreteDownOutSimpson(in, SimpsonResolution{});
  constexpr double kAgreement = 1e-9;
  const auto check = [&](const char* label, double a, double b, double limit) {
    Require(std::fabs(a - b) <= limit, prefix + " " + Describe(label, a, b), failures);
  };
  check("value, coarse GL against fine GL", gl.value, gl_fine.value, kAgreement);
  check("delta, coarse GL against fine GL", gl.delta, gl_fine.delta, kAgreement);
  check("naive, coarse GL against fine GL", gl.naive_delta, gl_fine.naive_delta, kAgreement);
  check("value, GL against Simpson", gl_fine.value, simpson.value, kAgreement);
  check("delta, GL against Simpson", gl_fine.delta, simpson.delta, kAgreement);
  check("naive, GL against Simpson", gl_fine.naive_delta, simpson.naive_delta, kAgreement);
  const std::string parameters = StripKind(instrument);
  const std::string method = "backward GL recursion; Simpson cross-check";
  rows.push_back({.id = value_id,
                  .parameters = parameters,
                  .value = gl_fine.value,
                  .b = value_b,
                  .method = method,
                  .source = source});
  rows.push_back({.id = delta_id,
                  .parameters = parameters,
                  .value = gl_fine.delta,
                  .b = std::max(kAgreement, 0.0),
                  .method = method,
                  .source = source});
  rows.push_back({.id = prefix + ".naive",
                  .parameters = parameters,
                  .value = gl_fine.naive_delta,
                  .b = kAgreement,
                  .method = "naive-delta limit of the same recursion",
                  .source = kDerived});
}

void AddAsian(std::vector<ReferenceRow>& rows, std::vector<std::string>& failures,
              const std::string& id, const char* instrument, AsianInputs in) {
  const AsianResult coarse = ArithmeticAsianCall(in, AsianResolution{});
  const AsianResult fine = ArithmeticAsianCall(
      in, AsianResolution{.nodes_per_sd = 6.0, .grid_sd = 14.0, .kernel_sd = 16.0});
  constexpr double kAgreement = 1e-9;
  constexpr double kIdentity = 1e-11;
  Require(std::fabs(coarse.value - fine.value) <= kAgreement,
          id + " " + Describe("coarse against fine", coarse.value, fine.value), failures);
  Require(std::fabs(fine.mass_error) <= kIdentity, id + " density mass", failures);
  Require(std::fabs(fine.martingale_error) <= kIdentity, id + " martingale", failures);
  rows.push_back({.id = id,
                  .parameters = StripKind(instrument),
                  .value = fine.value,
                  .b = kAgreement,
                  .method = "convolution recursion",
                  .source = kFusaiMeucci});
}

}  // namespace

std::vector<ReferenceRow> GenerateReferences(std::vector<std::string>& failures) {
  namespace cat = catalog;
  std::vector<ReferenceRow> rows;
  const auto bs = [](OptionType type, double strike) {
    return [type, strike](const auto& p) { return BlackScholes(type, strike, p).value; };
  };

  AddClosedForm(rows, "VA", cat::kVanillaO1,
                {.spot = 105, .sigma = 0.36, .r = 0.10, .q = 0.10, .maturity = 0.5},
                bs(OptionType::kCall, 100.0), "closed form", kHaugGreeks, "delta", "O1");
  AddClosedForm(rows, "VB", cat::kVanillaO2,
                {.spot = 72, .sigma = 0.19, .r = 0.09, .q = 0.0, .maturity = 1.0},
                bs(OptionType::kCall, 75.0), "closed form", kHaugGreeks, "rho", "O2");
  AddClosedForm(rows, "VC", cat::kVanillaO3,
                {.spot = 430, .sigma = 0.20, .r = 0.07, .q = 0.05, .maturity = 1.0 / 12.0},
                bs(OptionType::kPut, 405.0), "closed form", kHaugGreeks, "theta", "O3");
  AddClosedForm(
      rows, "GA", cat::kGeometricO4,
      {.spot = 100, .sigma = 0.20, .r = 0.06, .q = 0.03, .maturity = 1.0},
      [](const auto& p) { return GeometricAsianValue(OptionType::kCall, 100.0, 10, false, p); },
      "closed form", "QuantLib 5d6defcc test-suite/asianoptions.cpp:429", "value", "O4");

  AddDiscreteBarrier(rows, failures, "DB085", cat::kOneStepH85,
                     {.spot = 100,
                      .strike = 100,
                      .barrier = 85,
                      .r = 0.10,
                      .q = 0.0,
                      .sigma = 0.30,
                      .maturity = 0.2,
                      .monitors = 50},
                     "DB085.value", "DB085.delta", 1e-9, kBgk1997);
  AddDiscreteBarrier(rows, failures, "DB090", cat::kOneStepH90,
                     {.spot = 100,
                      .strike = 100,
                      .barrier = 90,
                      .r = 0.10,
                      .q = 0.0,
                      .sigma = 0.30,
                      .maturity = 0.2,
                      .monitors = 50},
                     "DB090.value", "DB090.delta", 1e-9, kBgk1997);
  AddDiscreteBarrier(rows, failures, "DB095", cat::kOneStepH95, kBarrierH95, "O5", "O6", 1e-9,
                     kBgk1997);
  AddDiscreteBarrier(rows, failures, "DB097", cat::kOneStepH97,
                     {.spot = 100,
                      .strike = 100,
                      .barrier = 97,
                      .r = 0.10,
                      .q = 0.0,
                      .sigma = 0.30,
                      .maturity = 0.2,
                      .monitors = 50},
                     "DB097.value", "DB097.delta", 1e-9, kBgk1997);
  AddDiscreteBarrier(rows, failures, "DB099", cat::kOneStepH99,
                     {.spot = 100,
                      .strike = 100,
                      .barrier = 99,
                      .r = 0.10,
                      .q = 0.0,
                      .sigma = 0.30,
                      .maturity = 0.2,
                      .monitors = 50},
                     "DB099.value", "DB099.delta", 1e-9, kBgk1997);
  // BENCHMARKS.md gives O8 b = 1e-4 around 9.4905. The tool's methods agree far inside that, and
  // the wider figure stays as the stated b.
  AddDiscreteBarrier(rows, failures, "DBS", cat::kOneStepO8,
                     {.spot = 100,
                      .strike = 100,
                      .barrier = 95,
                      .r = 0.10,
                      .q = 0.0,
                      .sigma = 0.60,
                      .maturity = 0.2,
                      .monitors = 4},
                     "O8", "DBS.delta", 1e-4, kBgk1999);

  const auto continuous = [&](const std::string& prefix, const char* instrument,
                              const MarketParams<double>& m, double strike, double barrier,
                              const std::string& override_suffix, const std::string& override_id) {
    AddClosedForm(
        rows, prefix, instrument, m,
        [=](const auto& p) {
          return BarrierOptionValue(BarrierType::kDownOut, OptionType::kCall, strike, barrier, 0.0,
                                    p);
        },
        "Reiner-Rubinstein closed form", "Haug 1998 p. 72 decomposition; BGK 1997 Prop. 3.1",
        override_suffix, override_id);
    if (barrier <= strike) {
      const double reiner =
          BarrierOptionValue(BarrierType::kDownOut, OptionType::kCall, strike, barrier, 0.0, m);
      const double bgk =
          ContinuousDownOutCallBgk(m.spot, strike, barrier, m.r, m.q, m.sigma, m.maturity);
      Require(std::fabs(reiner - bgk) <= 1e-11,
              prefix + " " + Describe("Reiner-Rubinstein against BGK identity", reiner, bgk),
              failures);
    }
  };
  continuous("CB095", cat::kBridgeO7,
             {.spot = 100, .sigma = 0.30, .r = 0.10, .q = 0.0, .maturity = 0.2}, 100, 95, "value",
             "O7");
  continuous("CB085", cat::kBridgeH85,
             {.spot = 100, .sigma = 0.30, .r = 0.10, .q = 0.0, .maturity = 0.2}, 100, 85, "", "");
  continuous("CBK90", cat::kBridgeK90,
             {.spot = 100, .sigma = 0.25, .r = 0.08, .q = 0.04, .maturity = 0.5}, 90, 95, "", "");

  AsianInputs asian = kFusaiMeucciInputs;
  const auto add_asian = [&](const std::string& id, const char* instrument, double strike,
                             std::uint32_t steps) {
    asian.strike = strike;
    asian.steps = steps;
    AddAsian(rows, failures, id, instrument, asian);
  };
  add_asian("AA12K090", cat::kArithmeticN12K90, 90, 12);
  add_asian("AA12K100", cat::kArithmeticN12K100, 100, 12);
  add_asian("AA12K110", cat::kArithmeticN12K110, 110, 12);
  add_asian("AA50K090", cat::kArithmeticN50K90, 90, 50);
  add_asian("O9", cat::kArithmeticO9, 100, 50);
  add_asian("AA50K110", cat::kArithmeticN50K110, 110, 50);

  for (const double strike : {90.0, 100.0, 110.0}) {
    AsianInputs three = kFusaiMeucciInputs;
    three.strike = strike;
    three.steps = 3;
    const double recursion =
        ArithmeticAsianCall(
            three, AsianResolution{.nodes_per_sd = 6.0, .grid_sd = 14.0, .kernel_sd = 16.0})
            .value;
    const double brute = ArithmeticAsianBruteForce3(three);
    Require(std::fabs(recursion - brute) <= 1e-12,
            Describe("n=3 recursion against brute force", recursion, brute), failures);
  }

  const std::vector<double> diagonal = ArithmeticAsianRichardson(
      {.spot = 2.0, .strike = 2.0, .r = 0.05, .q = 0.0, .sigma = 0.50, .maturity = 1.0, .steps = 0},
      64, 4, AsianResolution{});
  Require(std::fabs(diagonal.back() - kLinetskyCase5) <= kRichardsonTolerance,
          Describe("Richardson limit against Linetsky case 5", diagonal.back(), kLinetskyCase5),
          failures);
  rows.push_back({.id = "O10",
                  .parameters = "continuous average;S0=2;K=2;sigma=0.5;r=0.05;q=0;T=1;type=call",
                  .value = kLinetskyCase5,
                  .b = 1e-10,
                  .method = "published; Richardson limit check",
                  .source = "Linetsky 2004 Table 3 p. 866"});

  // (S0 + S_T) / 2 against K is half a vanilla put at 2K - S0 = 84.
  const double two_fixing =
      0.5 *
      BlackScholes(OptionType::kPut, 84.0,
                   MarketParams<double>{
                       .spot = 90, .sigma = 0.13, .r = 0.025, .q = 0.06, .maturity = 11.0 / 12.0})
          .value;
  rows.push_back({.id = "O11",
                  .parameters = StripKind(cat::kArithmeticO11),
                  .value = two_fixing,
                  .b = 0.0,
                  .method = "identity: half a vanilla put at K=84",
                  .source = "corrects QuantLib 5d6defcc test-suite/asianoptions.cpp Levy value"});
  return rows;
}

std::string RenderCsv(const std::vector<ReferenceRow>& rows) {
  std::string out =
      "# Generated by tools/quadrature. Do not edit: a ctest test regenerates and compares it.\n"
      "# Values print to 12 decimals, so libm differences beyond that do not change the file.\n"
      "id,parameters,value,b,method,source\n";
  for (const ReferenceRow& row : rows) {
    std::array<char, 96> number{};
    std::snprintf(number.data(), number.size(), ",%.12f,%.0e,", row.value, row.b);
    out += row.id + "," + row.parameters + number.data() + row.method + "," + row.source + "\n";
  }
  return out;
}

}  // namespace aadp::quadrature
