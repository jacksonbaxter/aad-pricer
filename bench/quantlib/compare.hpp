// The QuantLib comparison's rows, references, tolerance and metrics (docs/NOTES.md, "Stage 10"),
// with no QuantLib in it, so every build compiles and tests it.
//
// A case is one contract of a protocol row (quantlib-compare/EVIDENCE.md, "Comparison
// protocol"); a comparison is one engine on one case, judged against the case's reference by the
// section 10 rule. There are 42: R1 2, R2 6, R3 2, R4 2, R5 10, R6 5, R7 10, R9a 2, R9b 3. R6 has
// no QuantLib comparison, since QuantLib has no survival-weight mode and its estimator is R5's.
// R8 is a recorded non-comparison, and R10 to R12 are speed or sanity rows (local_vol_rows.hpp).
#ifndef AADP_BENCH_QUANTLIB_COMPARE_HPP_
#define AADP_BENCH_QUANTLIB_COMPARE_HPP_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "oracle/references.hpp"
#include "oracle/tolerance.hpp"
#include "pricer/contract.hpp"

namespace aadp::qlcompare {

// Development seeds, pre-registered; never 1729 or 271828, which the oracle plan reserves.
inline constexpr std::uint64_t kPilotSeed = 20261111;
inline constexpr std::uint64_t kAccuracySeed = 20261112;
inline constexpr std::uint64_t kSpeedSeed = 20261113;
inline constexpr std::uint64_t kSmokeSeed = 20261114;

inline constexpr std::uint64_t kPilotPaths = std::uint64_t{1} << 20;
inline constexpr std::uint64_t kAccuracyPaths = std::uint64_t{1} << 22;
inline constexpr std::uint64_t kSmokePaths = std::uint64_t{1} << 16;
inline constexpr std::uint32_t kBatches = 64;
inline constexpr std::size_t kComparisonCount = 42;
inline constexpr double kCeilingFactor = 1.65;     // against a reference
inline constexpr double kBumpCeilingFactor = 3.0;  // QuantLib's two bumps
inline constexpr double kAsianBumpStep = 0.01;     // R9a, both the reference and QuantLib
inline constexpr double kBarrierBumpStep = 1.0;    // R9b, QuantLib: 1% of S0
inline constexpr double kSmoothingWidth = 0.005;   // R9b, ours: half-width / S0

enum class Row : std::uint8_t { kR1, kR2, kR3, kR4, kR5, kR6, kR7, kR9a, kR9b };
enum class Engine : std::uint8_t { kQuantLib, kOurs, kOursOneStep, kOursSmoothed };

struct Case {
  std::string id;  // R2.K090, R7.H095, R9b
  Row row = Row::kR1;
  double strike = 0.0;
  double barrier = 0.0;  // Barrier rows only.
};

struct Comparison {
  Case contract;
  Engine engine = Engine::kOurs;
  // id/engine, the key of the ceilings file.
  [[nodiscard]] std::string Name() const;
};

std::string_view RowName(Row row);
std::string_view EngineName(Engine engine);

// Every case of R1 to R9, in report order.
std::vector<Case> AccuracyCases();
// The 42 comparisons, in report order.
std::vector<Comparison> Comparisons();

// Dates per path: the step count of both engines.
std::uint32_t Steps(Row row);
bool IsBarrier(Row row);
// QuantLib's R9 runs two NPVs per batch, a central bump of the spot.
bool IsQuantLibBump(const Comparison& comparison);
// R5's QuantLib estimate is one NPV with QuantLib's own error estimate (the seed-5 uniforms).
bool IsSingleRunQuantLib(const Comparison& comparison);
double CeilingFactor(const Comparison& comparison);

// Our pricer's view of a case. Engine picks the treatment where a case has more than one.
pricer::Market<double> OurMarket(const Case& c);
pricer::Contract OurContract(const Case& c, Engine engine);

// R9's references, from tools/quadrature, before any Monte Carlo (docs/NOTES.md, "Stage 10").
struct R9References {
  double asian_delta = 0.0;  // CD(h) of the n = 12 quadrature, h = kAsianBumpStep
  double asian_b = 0.0;      // 2 * 1e-9 / 2h + |CD(2h) - CD(h)|
  double asian_cd_2h = 0.0;
  double barrier_delta = 0.0;   // O6
  double barrier_b = 0.0;       // O6's b
  double barrier_cd = 0.0;      // CD(h) of the discrete recursion, h = kBarrierBumpStep
  double barrier_bump_b = 0.0;  // O6's b + |CD(h) - O6| + 2 * 1e-9 / 2h
};
R9References ComputeR9References(const ReferenceTable& table);

struct RowReference {
  double value = 0.0;
  double b = 0.0;
  std::string source;
};
std::expected<RowReference, std::string> ReferenceFor(const Comparison& comparison,
                                                      const ReferenceTable& table,
                                                      const R9References& r9);

// The pre-registered critical value t(1 - 0.01 / 84, 63).
double Critical();

// ceiling = factor * se_pilot * sqrt(n_pilot / n), rounded up to 3 significant figures.
double CeilingFrom(double se_pilot, std::uint64_t n_pilot, std::uint64_t n, double factor);
double RoundUpThreeFigures(double x);

struct CeilingRow {
  std::string name;
  double se_pilot = 0.0;
  std::uint64_t n_pilot = 0;
  double factor = 0.0;
  double ceiling = 0.0;  // At kAccuracyPaths.
};
std::string RenderCeilings(const std::vector<CeilingRow>& rows, std::string_view header);
// Every comparison exactly once, or why not.
std::expected<std::vector<CeilingRow>, std::string> DecodeCeilings(std::string_view text);

// Metrics (docs/NOTES.md, "Stage 10", QL-1).
struct Timing {
  double median_s = 0.0;
  double min_s = 0.0;
  double max_s = 0.0;
  std::uint64_t paths = 0;
  std::uint32_t steps = 0;
};
double PathsPerSecond(const Timing& t);
double NanosecondsPerPathStep(const Timing& t);
// max(SE^2, d^2) at the accuracy run's N, d = mean - reference.
double MeanSquaredError(double standard_error, double distance);
// RMSE^2 x the time the estimator takes for the accuracy run's N paths.
double ErrorTimesTime(double mse, const Timing& t, std::uint64_t accuracy_paths);

}  // namespace aadp::qlcompare

#endif  // AADP_BENCH_QUANTLIB_COMPARE_HPP_
