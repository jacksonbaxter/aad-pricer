// QuantLib's side of the comparison (NOTES.md, "Stage 10"). QuantLib v1.44 is linked, through its
// public headers, and never copied or patched; only this file's .cpp includes it.
//
// Every engine runs PseudoRandom (Mersenne Twister and Acklam's inverse), no Brownian bridge, no
// antithetic paths, a fixed sample count and a nonzero seed, on a fixed evaluation date with flat
// continuous curves and a constant Black volatility (quantlib-compare/EVIDENCE.md, "Comparison
// protocol"). Default: QuantLib's statistics class, which stores every sample, and no
// extrapolation flag. Tuned: IncrementalStatistics and enableExtrapolation() on every term
// structure the engine reads (E38, E41); its LTO comes from the build, not from here.
#ifndef AADP_BENCH_QUANTLIB_QUANTLIB_SIDE_HPP_
#define AADP_BENCH_QUANTLIB_QUANTLIB_SIDE_HPP_

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "quantlib/compare.hpp"
#include "quantlib/local_vol_rows.hpp"

namespace aadp::qlcompare::ql {

struct Settings {
  bool tuned = false;
};

struct Npv {
  double value = 0.0;
  double error = 0.0;  // QuantLib's errorEstimate()
};

// One instrument with its engine, built outside anything timed. Each Price() forces a fresh
// calculation, so a repeat on the same seed runs the same paths again (EVIDENCE E28).
class Pricer {
 public:
  struct Impl;
  explicit Pricer(std::unique_ptr<Impl> impl);
  Pricer(Pricer&&) noexcept;
  Pricer& operator=(Pricer&&) noexcept;
  Pricer(const Pricer&) = delete;
  Pricer& operator=(const Pricer&) = delete;
  ~Pricer();

  // A new engine on this seed, which must not be 0 (a clock seed, EVIDENCE E5).
  void SetSeed(std::uint64_t seed);
  Npv Price();
  // (NPV(S0 + h) - NPV(S0 - h)) / 2h on the current seed: common random numbers. Leaves S0.
  double CentralBump(double h);

 private:
  std::unique_ptr<Impl> impl_;
};

// R1 to R9's QuantLib engine for a case, at samples paths per NPV.
Pricer MakeCasePricer(const Case& c, const Settings& settings, std::uint64_t samples);
// R10 and R11 on the grid, through FixedLocalVolSurface and the 5-argument process.
Pricer MakeLocalVolPricer(LocalVolRow row, const LocalVolGrid& grid, const Settings& settings,
                          std::uint64_t samples);
// R12: SviSmileSection per slice, PiecewiseBlackVarianceSurface, NoExceptLocalVolSurface.
Pricer MakeR12Pricer(const SyntheticSurface& surface, const Settings& settings,
                     std::uint64_t samples);

// QuantLib's FixedLocalVolSurface on the grid at (t, s), for the check of GridModel's rule.
class FixedSurface {
 public:
  explicit FixedSurface(const LocalVolGrid& grid);
  FixedSurface(FixedSurface&&) noexcept;
  FixedSurface& operator=(FixedSurface&&) noexcept;
  FixedSurface(const FixedSurface&) = delete;
  FixedSurface& operator=(const FixedSurface&) = delete;
  ~FixedSurface();
  [[nodiscard]] double Vol(double t, double s) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// QL_VERSION, QL_HEX_VERSION, the generated ql/config.hpp's switches and Boost's version.
std::vector<std::pair<std::string, std::string>> VersionRecord();

}  // namespace aadp::qlcompare::ql

#endif  // AADP_BENCH_QUANTLIB_QUANTLIB_SIDE_HPP_
