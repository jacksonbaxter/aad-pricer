// The bump-size study: h for each Greek, chosen by halving (NOTES.md, "Bump size").
//
// With a barrier treatment or a Lipschitz payoff the estimator is continuous in each input, so
// under common random numbers the difference quotient does not grow noisier as h falls, and h is
// limited by round-off. So, per workload, input and scheme, start from h_0 and halve:
//   D_j    the difference quotient at h_j = h_0 2^-j, on the workload's own seed and path count
//   Delta_j = D_j - D_{j+1}, batch by batch, with its SE
//   R_j    the round-off scale of D_j (below)
//   stable(j)  |Delta_j| <= R_j + R_{j+1}: halving changed the estimate by no more than the
//              round-off of the two estimates
// The chosen step is h_j for the first j with stable(j) and stable(j + 1): the largest step whose
// estimate agrees with both of the next two within round-off. One stable pair alone can be a sign
// change of the truncation passing through zero; two in a row cannot, since truncation scales by
// 4 (central) or 2 (forward) per halving.
//
// Bias left at the chosen step. Truncation is c h^2 (central) or c h (forward) to leading order,
// so it is (4/3) Delta_j or 2 Delta_j, and that estimate, with its SE, is what the study reports
// as the bump's bias. By the rule it is at most (4/3) or 2 times R_j + R_{j+1}.
//
// The round-off scale. D_j is (V+ - V-) / (x+ - x-) of two batch means. Each path's value passes
// through at most T statements (the tape's count for that path), and each statement's result is
// within kRoundingsPerStatement units of round-off of its exact value given its operands: one for
// + - * /, 1.23 for exp (0.613 ULP), 1.68 for log (0.840 ULP), 7.6 for the inverse normal
// (8.39e-16 relative), all from stage 1a, so 8 covers every kernel operation. A batch mean adds
// P - 1 additions and a division, and the mean over batches B more. So with
// n = 8 T + P + B and gamma_n = n u / (1 - n u), u = 2^-53,
//   R = gamma_n (mean|V+| + mean|V-|) / |x+ - x-|.
// That is a first-order count of one loss per operation at the scale of the values, not a proof:
// it does not follow how an early loss is amplified along the path. So the study also checks it:
// after the chosen step, halving continues to h_0 2^-J, deep into round-off, and every later pair
// must stay stable. A pair that does not means the scale was too small (or the estimator is not
// continuous), and the study says so (violation), which fails its test.
//
// The in-run check (aad_checks.hpp) compares a bump at the chosen step with the tape. Their means
// differ by the truncation plus the run's round-off. The truncation does not shrink with the
// run's size, but R does (n counts the paths of a batch), so the allowance carries the study's
// bound from its own size: TruncationFactor (R_j + R_{j+1}), which bounds |bias| by the rule, and
// widened by the critical value times bias_se, since the study's change is itself an estimate of
// the expected one. So b = truncation + t bias_se + R, R the run's own round-off scale at the step
// (CheckAllowance below). A run at the study's size and seed sees the study's own paths.
#ifndef AADP_BENCH_BUMP_STUDY_HPP_
#define AADP_BENCH_BUMP_STUDY_HPP_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "aad_workloads.hpp"
#include "pricer/bump.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"

namespace aadp::bench::aad {

inline constexpr double kRoundingsPerStatement = 8.0;
inline constexpr std::uint32_t kStudyHalvings = 32;

// n in gamma_n, for a path of `statements` statements in batches of `paths_per_batch`.
double RoundingCount(double statements, std::uint64_t paths_per_batch, std::uint32_t batches);
double GammaOf(double n);
double RoundoffScale(double rounding_count, double absolute_up, double absolute_down,
                     double denominator);
double TruncationFactor(pricer::BumpScheme scheme);  // 4/3 or 2
// b of the in-run check, for a run whose own round-off scale at the step is `roundoff`.
double CheckAllowance(const StepChoice& step, double roundoff, double critical);

// The first step of the study per input, in that input's units: S0 / 16, 1 / 64 of a unit of
// vol, rate and yield, T / 16. Powers of two times the input, so every halving is exact.
pricer::BumpSteps InitialSteps(const pricer::Market<double>& market);
inline constexpr double kLwInitialStep = 0.0625;

// One step of the study: D at h, with its batch means, and R at h.
struct StudyPoint {
  double h = 0.0;
  double denominator = 0.0;
  pricer::Estimate estimate;
  double roundoff = 0.0;
};

struct StudyRow {
  std::uint32_t halvings = 0;
  double h = 0.0;
  double estimate = 0.0;
  double se = 0.0;
  double roundoff = 0.0;
  bool has_change = false;  // false on the last row
  double change = 0.0;      // D_j - D_{j+1}
  double change_se = 0.0;
  double limit = 0.0;  // R_j + R_{j+1}
  bool stable = false;
};

// Rows from points at h_0 2^-j, j = 0, 1, ...
std::vector<StudyRow> Tabulate(std::span<const StudyPoint> points);

struct StudyChoice {
  std::optional<std::size_t> chosen;
  double bias = 0.0;
  double bias_se = 0.0;
  double truncation = 0.0;  // TruncationFactor (R_j + R_{j+1}): the rule's bound on |bias|
  std::optional<std::size_t> violation;  // First unstable row after the chosen pair
};

// The rule above, on rows whose change, limit and stable fields are set.
StudyChoice ChooseStep(std::span<const StudyRow> rows, pricer::BumpScheme scheme);

// The study of one input of a pricer workload, at `config`, which must be its benchmark's.
std::vector<StudyPoint> PricerStudy(const Workload& workload, const pricer::RunConfig& config,
                                    pricer::Input input, pricer::BumpScheme scheme, double h0,
                                    std::uint32_t halvings, double rounding_count);

// The Lax-Wendroff study: one row per step, for the input whose |change| / limit is largest, and
// stable only if every input is. Deterministic, so every SE is zero.
std::vector<StudyRow> LwStudy(pricer::BumpScheme scheme, double h0, std::uint32_t halvings,
                              double rounding_count);

std::string_view SchemeName(pricer::BumpScheme scheme);
std::string_view InputName(std::size_t input);

}  // namespace aadp::bench::aad

#endif  // AADP_BENCH_BUMP_STUDY_HPP_
