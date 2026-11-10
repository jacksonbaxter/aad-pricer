// The finite-difference ladder of the 45 surface rows and the rule that picks its level, as
// NOTES.md, "Stage 8", registers them. The rule judges from the differences alone, never the tape.
//
// Step rule. For slice j's parameter i, h0 = 2^-8 w_j(0) / M, with w_j(0) the fitted
// at-the-money total variance and M the largest |dw_j / dtheta_i (k)| on k = -0.5, -0.499, ...,
// 0.5, from the closed forms. Levels h_l = 2^-l h0, l = 0..8. A step that would leave the
// validated domain on either side is refused, never clipped.
//
// Ladder. Per batch q, D_{l,q} is the central difference at h_l on common random numbers,
// Delta_l = D_l - D_{l+1} (l = 0..7) and E_l = Delta_{l+1} - Delta_l / 4 (l = 0..6), each with
// its batch-means mean and SE. Pair l is consistent with h^2 truncation when
//   |mean E_l| <= c SE(E_l) + |mean Delta_l| / 16.
// The level l* is the smallest l in 0..5 with pairs l and l + 1 both consistent; the reference
// step is h* = h_{l*+1} and b = RoundUp3(2 (|mean Delta_l*| + c SE(Delta_l*)) / 3). With no such
// level the row is unresolved.
#ifndef AADP_TOOLS_RUN_ORACLES_LADDER_HPP_
#define AADP_TOOLS_RUN_ORACLES_LADDER_HPP_

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "oracle/tolerance.hpp"
#include "runner.hpp"
#include "svi/raw_svi.hpp"

namespace aadp::oracle_runner {

inline constexpr std::size_t kLadderLevels = 9;
inline constexpr std::size_t kLastChosenLevel = 5;
inline constexpr double kStepFraction = 0x1p-8;
inline constexpr double kStepGridHalfWidth = 0.5;
inline constexpr int kStepGridPoints = 1001;  // k = -0.5 + 0.001 i, i = 0..1000
inline constexpr double kMinSurfaceSigmaAfterStep = 1e-8;

struct LadderLevel {
  double h = 0.0;
  BatchStatistics d;
  BatchStatistics delta;  // l <= 7
  BatchStatistics e;      // l <= 6
  bool has_delta = false;
  bool has_e = false;
  bool consistent = false;  // pair l, l <= 6
};

struct LadderOutcome {
  std::vector<LadderLevel> levels;
  bool resolved = false;
  std::size_t level = 0;  // l*
  double h_star = 0.0;    // h_{l*+1}, or h_8 when unresolved
  double b = 0.0;
  BatchStatistics two_h;  // Delta_l*
};

// d[l] holds the batch values D_{l,q} at h[l], l = 0..8, the same number of batches each.
// critical is the plan's critical value.
LadderOutcome ApplyLadderRule(const std::array<double, kLadderLevels>& h,
                              const std::array<std::vector<double>, kLadderLevels>& d,
                              double critical);

// dw / dtheta_i at k, the closed forms of NOTES.md, "Stage 8": parameter i in (a, b, rho, m,
// sigma).
double TotalVarianceSlope(const svi::RawSvi<double>& p, std::size_t parameter, double k);
double AtTheMoneyTotalVariance(const svi::RawSvi<double>& p);
// h0 for one slice parameter.
double FirstStep(const svi::RawSvi<double>& p, std::size_t parameter);
// Why theta +- h leaves the validated domain, or nothing.
std::optional<std::string> StepOutsideDomain(const svi::RawSvi<double>& p, std::size_t parameter,
                                             double h);
svi::RawSvi<double> Bumped(svi::RawSvi<double> p, std::size_t parameter, double h);

}  // namespace aadp::oracle_runner

#endif  // AADP_TOOLS_RUN_ORACLES_LADDER_HPP_
