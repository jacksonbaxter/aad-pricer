// What the pricer prices, and the check that refuses anything it cannot price.
//
// Dates. A contract has m steps at t_i = i T / m, i = 1..m, the last at T, as in the frozen
// reference kernels in src/mc/. Asians fix there, discrete barriers monitor there, and a
// continuous barrier is bridged between them. The dates scale with T, as theta = -dV/dT needs.
//
// Barrier treatments (docs/NOTES.md, "Barrier treatment"):
//   continuous  kBridge    log-space Brownian-bridge survival weight, prod (1 - p_n)
//               kHard      a sampled crossing per step against a second uniform, then a hard
//                          indicator: the QuantLib MCBarrierEngine estimator of row R5
//   discrete    kOneStep   one-step survival, Gerstner, Harrach, Roth, arXiv 1804.03975v3
//               kHard      hard indicator at the dates: the naive estimator, QuantLib row R7
//               kSmoothed  Savine's smooth barrier: Modern Computational Finance, Wiley 2018,
//                          section 6.3, printed pp. 236-238. A linear ramp in spot between
//                          B - s and B + s at each date, s = width * S0 held constant
// The bridge on a discretely monitored contract is refused: it prices a different contract.
// A knock-in is vanilla minus the knock-out with the same treatment, path by path.
//
// Digitals pay 1 at T, priced by conditional expectation over the last step (Giles, "Vibrato
// Monte Carlo sensitivities", pp. 4-5; oracles E24).
#ifndef AADP_PRICER_CONTRACT_HPP_
#define AADP_PRICER_CONTRACT_HPP_

#include <cstdint>

namespace aadp::pricer {

enum class Product : std::uint8_t {
  kEuropean,
  kDigital,
  kAsianArithmetic,
  kAsianGeometric,
  kBarrier,  // Vanilla call or put at T with a single barrier.
};
inline constexpr std::uint8_t kProductCount = 5;

enum class OptionType : std::uint8_t { kCall, kPut };
enum class Direction : std::uint8_t { kDown, kUp };
enum class Knock : std::uint8_t { kOut, kIn };
enum class Monitoring : std::uint8_t { kContinuous, kDiscrete };
enum class Treatment : std::uint8_t { kBridge, kOneStep, kHard, kSmoothed };
inline constexpr std::uint8_t kTreatmentCount = 4;

struct Barrier {
  Direction direction = Direction::kDown;
  Knock knock = Knock::kOut;
  Monitoring monitoring = Monitoring::kContinuous;
  Treatment treatment = Treatment::kBridge;
  double level = 0.0;
  double smoothing_width = 0.0;  // kSmoothed only: half-width of the ramp as a fraction of S0.
};

struct Contract {
  Product product = Product::kEuropean;
  OptionType type = OptionType::kCall;
  double strike = 0.0;
  std::uint32_t steps = 1;
  bool include_spot = false;  // Asians: S0 is one more point in the average.
  Barrier barrier;            // kBarrier only.
};

template <class Real>
struct Market {
  Real spot;
  Real sigma;
  Real r;
  Real q;
  Real maturity;
};

enum class ContractError : std::uint8_t {
  kNone,
  kProduct,
  kOptionType,
  kSpot,
  kStrike,
  kVolatility,
  kMaturity,
  kRate,
  kSteps,
  kIncludeSpot,
  kBarrierField,
  kBarrierLevel,
  kSmoothingWidth,
  kBridgeOnDiscrete,
  kOneStepOnContinuous,
  kSmoothedOnContinuous,
  kRange,
};

// Limits, chosen so that every value a valid path can produce is finite (see Validate).
inline constexpr double kMinPrice = 1e-8;
inline constexpr double kMaxPrice = 1e8;
inline constexpr double kMinVolatility = 1e-4;
inline constexpr double kMaxVolatility = 10.0;
inline constexpr double kMinMaturity = 1e-6;
inline constexpr double kMaxMaturity = 100.0;
inline constexpr double kMaxRate = 1.0;
inline constexpr std::uint32_t kMaxSteps = 100000;
// A width far below the 2^-52 relative spacing of S0 adds nothing, and keeps 2 s a normal double.
inline constexpr double kMinSmoothingWidth = 1e-6;
inline constexpr double kMaxSmoothingWidth = 0.5;
inline constexpr double kMaxLogExcursion = 340.0;
// Every draw on the Philox grid has |z| < 8.3, and a one-step conditional draw lands within
// 8.6 standard deviations beyond the barrier side it is conditioned on, so 9 covers both.
inline constexpr double kMaxDrawMagnitude = 9.0;

ContractError Validate(const Contract& contract, const Market<double>& market);
const char* ErrorName(ContractError error);

}  // namespace aadp::pricer

#endif  // AADP_PRICER_CONTRACT_HPP_
