// Deterministic reference values for the oracles, C++ and the standard library only.
//
// Discrete down-and-out: backward recursion on [log H, top], two ways that share no quadrature
// code (composite Gauss-Legendre with a score delta, and uniform-grid Simpson with a spot
// stencil). Discrete arithmetic Asian: density of log Y_k by convolution recursion, where
// S_0 + ... + S_n = S_0 (1 + R_1 (1 + Y_2)) and Y_k = R_k (1 + Y_{k+1}), with brute-force n = 3
// as the second method. The step into maturity is closed form in both, so the payoff kink never
// reaches a quadrature rule.
#ifndef AADP_TOOLS_QUADRATURE_QUADRATURE_HPP_
#define AADP_TOOLS_QUADRATURE_QUADRATURE_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "oracle/black_scholes.hpp"
#include "support/dual.hpp"

namespace aadp::quadrature {

struct BarrierInputs {
  double spot;
  double strike;
  double barrier;
  double r;
  double q;
  double sigma;
  double maturity;
  std::uint32_t monitors;  // Two or more. Dates i T / m, i = 1..m.
};

template <class Real>
struct BarrierResultOf {
  Real value;
  Real delta;
  Real naive_delta;  // Limit of the pathwise delta that ignores the survival indicator.
  std::size_t nodes;
};
using BarrierResult = BarrierResultOf<double>;
using DualBarrierResult = BarrierResultOf<test::Dual>;

struct GaussLegendreResolution {
  double panels_per_sd = 2.0;
  int degree = 8;
  double top_sd = 12.0;
  double kernel_sd = 13.0;
};

struct SimpsonResolution {
  double intervals_per_sd = 24.0;  // The coarser of the two grids.
  double top_sd = 10.0;
  double kernel_sd = 11.0;
  double spot_stencil = 0.02;  // Five-point delta at spot +- this and twice this.
};

// The resolution of every published discrete row and of the knock-in references.
inline constexpr GaussLegendreResolution kFineGaussLegendre{
    .panels_per_sd = 3.0, .degree = 10, .top_sd = 14.0, .kernel_sd = 15.0};

BarrierResult DiscreteDownOutGaussLegendre(const BarrierInputs& in,
                                           const GaussLegendreResolution& resolution);
BarrierResult DiscreteDownOutSimpson(const BarrierInputs& in, const SimpsonResolution& resolution);

// The same recursions over Dual (NOTES.md, "Stage 8"). The grids are those of the double call on
// `in`: nodes, weights, panel and interval counts, top and band. Only each step's drift, sd and
// discount, the closed-form last step and the root's log spot carry the tangents of `market`,
// whose values must be in's. The double functions are these at double, so their bits stand.
DualBarrierResult DiscreteDownOutGaussLegendre(const BarrierInputs& in,
                                               const MarketParams<test::Dual>& market,
                                               const GaussLegendreResolution& resolution);
DualBarrierResult DiscreteDownOutSimpson(const BarrierInputs& in,
                                         const MarketParams<test::Dual>& market,
                                         const SimpsonResolution& resolution);

// The knock-in rows' terms, DB095's and CB095's: S0 = K = 100, H = 95, sigma = 0.3, r = 0.1,
// q = 0, T = 0.2, m = 50.
inline constexpr BarrierInputs kKnockInTerms{.spot = 100,
                                             .strike = 100,
                                             .barrier = 95,
                                             .r = 0.10,
                                             .q = 0.0,
                                             .sigma = 0.30,
                                             .maturity = 0.2,
                                             .monitors = 50};
// The b of the discrete knock-in rows: O5's and O6's for value and delta, 1e-8 for the Greeks
// whose integrands carry polynomial factors (NOTES.md, "Stage 8"). The continuous rows have 0.
inline constexpr double kKnockInValueB = 1e-9;
inline constexpr double kKnockInGreekB = 1e-8;
// Parity and the BGK identity, both closed forms in double.
inline constexpr double kKnockInIdentity = 1e-11;

// El Babsiri and Noel's down-and-in calls via QuantLib (EVIDENCE E31): S = K = 100, H = 90,
// r = 0.05, q = 0.02, T = 1, no rebate. A guard on the closed form's knock-in branch, within
// QuantLib's own analytic tolerance.
struct BabsiriRow {
  double sigma;
  double value;
};
inline constexpr auto kBabsiriDownIn =
    std::to_array<BabsiriRow>({{.sigma = 0.10, .value = 0.07187},
                               {.sigma = 0.15, .value = 0.60638},
                               {.sigma = 0.20, .value = 1.64005},
                               {.sigma = 0.25, .value = 2.98495},
                               {.sigma = 0.30, .value = 4.50952}});
inline constexpr double kBabsiriTolerance = 1e-5;

struct AsianInputs {
  double spot;
  double strike;
  double r;
  double q;
  double sigma;
  double maturity;
  std::uint32_t steps;  // n: the average runs over k = 0..n, so n + 1 points including S_0.
};

struct AsianResolution {
  double nodes_per_sd = 4.0;
  double grid_sd = 12.0;
  double kernel_sd = 14.0;
};

struct AsianResult {
  double value;
  double mass_error;        // Integral of the density, minus 1.
  double martingale_error;  // Relative error of E[sum S_k].
  std::size_t nodes;
};

AsianResult ArithmeticAsianCall(const AsianInputs& in, const AsianResolution& resolution);

// n = 3 only. Plain 2-D Gauss-Legendre on the first two normals, last step closed form.
double ArithmeticAsianBruteForce3(const AsianInputs& in, int nodes = 160, double span = 9.0);

// Neville table on n, 2n, 4n, ...: the discrete average has an expansion in powers of 1/n.
// Returns the diagonal, least to most extrapolated.
std::vector<double> ArithmeticAsianRichardson(const AsianInputs& in, std::uint32_t first_steps,
                                              int levels, const AsianResolution& resolution);

// BGK 1997 Proposition 3.1 with a dividend yield, H <= K: Black-Scholes minus the reflected
// term. Written apart from src/oracle/barrier.hpp so one can check the other.
double ContinuousDownOutCallBgk(double spot, double strike, double barrier, double r, double q,
                                double sigma, double maturity);

// Fixed before measuring, BENCHMARKS.md O10: Linetsky case 5 with S0 = K = 2.
inline constexpr double kLinetskyCase5 = 0.2464156905;
inline constexpr double kRichardsonTolerance = 1e-7;

struct ReferenceRow {
  std::string id;
  std::string parameters;
  double value;
  double b;
  std::string method;
  std::string source;
};

// All rows, in file order. A method disagreement is appended to failures and fails the tool.
std::vector<ReferenceRow> GenerateReferences(std::vector<std::string>& failures);

std::string RenderCsv(const std::vector<ReferenceRow>& rows);

}  // namespace aadp::quadrature

#endif  // AADP_TOOLS_QUADRATURE_QUADRATURE_HPP_
