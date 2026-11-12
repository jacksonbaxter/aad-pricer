// Local-volatility prices and their 15 raw SVI sensitivities off the surface fitted to the TAIFEX
// captures (docs/NOTES.md, "Stage 7a"). Every number it prints is iteration only.
//
//   aadp_local_vol_report [--options <file>] [--futures <file>] [--mode report|bias|timing]
//                         [--paths <N>] [--threads <T>] [--repetitions <R>]
//
// report  the surface and curves, then per svi-surface contract price +- SE, the 15 sensitivities
//         +- SE and the floor report; then the European diagnostic at each slice's expiry beside
//         Black-76 at that slice's w_j(k). N = 2^20, 64 batches, 8 threads by default.
// bias    the Euler bias plan: levels n = 1 to 16 on coupled draws, per product and treatment.
// timing  wall time per product, price alone and price with the sensitivities, single thread,
//         N = 2^16, 5 repetitions by default: run it through measure.sh.
// The output is derived from the chain, so it opens with the attribution. Seed 20261115.

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <optional>
#include <ratio>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "ad/local_vol_gradient.hpp"
#include "build_info.hpp"
#include "data/attribution.hpp"
#include "data/taifex_csv.hpp"
#include "data/taifex_futures.hpp"
#include "data/taifex_options.hpp"
#include "oracle/tolerance.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/kernels.hpp"
#include "pricer/local_vol.hpp"
#include "pricer/local_vol_model.hpp"
#include "pricer/normal_cdf.hpp"
#include "pricer/real_ops.hpp"
#include "pricer/runner.hpp"
#include "pricer/substep_kernels.hpp"
#include "svi/chain_fit.hpp"
#include "svi/chain_surface.hpp"
#include "svi/implied_vol.hpp"
#include "svi/local_vol.hpp"
#include "svi/raw_svi.hpp"
#include "svi/surface.hpp"

namespace {

using aadp::pricer::Contract;
using aadp::pricer::FloorRange;
using aadp::pricer::FloorTally;
using aadp::pricer::LocalVolContract;
using aadp::pricer::RunConfig;
using aadp::svi::ChainSurface;

constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;
// A development seed, never 1729 or 271828.
constexpr std::uint64_t kSeed = 20261115;
constexpr std::uint32_t kBatches = 64;
constexpr std::array<const char*, 5> kParameterNames = {"a", "b", "rho", "m", "sigma"};

enum class Mode : std::uint8_t { kReport, kBias, kTiming };

struct Options {
  std::string options_path = "data/DailyMarketReportOpt.csv";
  std::string futures_path = "data/DailyMarketReportFut.csv";
  Mode mode = Mode::kReport;
  std::uint64_t paths = 0;  // 0: the mode's default
  std::uint32_t threads = 0;
  std::uint32_t repetitions = 5;
};

int Usage() {
  std::cerr << "usage: aadp_local_vol_report [--options <file>] [--futures <file>] "
               "[--mode report|bias|timing] [--paths <N, a multiple of 64>] [--threads <T>] "
               "[--repetitions <R>]\n";
  return kExitUsage;
}

// A positive count, or 0 for anything else.
std::uint64_t Count(std::string_view text) {
  std::uint64_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc() && end == text.data() + text.size() ? value : 0;
}

std::optional<Mode> ParseMode(std::string_view value) {
  if (value == "report") return Mode::kReport;
  if (value == "bias") return Mode::kBias;
  if (value == "timing") return Mode::kTiming;
  return std::nullopt;
}

// A count in [1, limit], or 0.
std::uint32_t Bounded(std::string_view value, std::uint64_t limit) {
  const std::uint64_t n = Count(value);
  return n <= limit ? static_cast<std::uint32_t>(n) : 0;
}

// One flag and its value into o; false if either is not valid.
bool Apply(Options& o, std::string_view flag, std::string_view value) {
  if (flag == "--options") {
    o.options_path = value;
  } else if (flag == "--futures") {
    o.futures_path = value;
  } else if (flag == "--mode") {
    const std::optional<Mode> mode = ParseMode(value);
    if (!mode) return false;
    o.mode = *mode;
  } else if (flag == "--paths") {
    o.paths = Count(value);
    return o.paths != 0 && o.paths % kBatches == 0;
  } else if (flag == "--threads") {
    o.threads = Bounded(value, aadp::pricer::kMaxThreads);
    return o.threads != 0;
  } else if (flag == "--repetitions") {
    o.repetitions = Bounded(value, 1000);
    return o.repetitions != 0;
  } else {
    return false;
  }
  return true;
}

std::optional<Options> Parse(std::span<char*> args) {
  Options o;
  for (std::size_t i = 1; i < args.size(); i += 2) {
    if (i + 1 == args.size() || !Apply(o, args[i], args[i + 1])) return std::nullopt;
  }
  const bool timing = o.mode == Mode::kTiming;
  if (o.paths == 0) o.paths = timing ? (std::uint64_t{1} << 16) : (std::uint64_t{1} << 20);
  if (o.threads == 0) o.threads = timing ? 1 : 8;
  return o;
}

void Header(const Options& o) {
  aadp::write_attribution(std::cout);
  std::cout
      << "\naadp_local_vol_report: local volatility off the raw SVI surface fitted to TAIFEX TXO "
         "settlements of trade date 2024-11-15.\n"
         "These are daily settlement prices set by the exchange's rules, not tradable quotes.\n"
         "Maturity reaches 33 days only: nothing is extrapolated past the 202412 expiry.\n"
         "ITERATION ONLY, not a result. Method, floor rule and contracts: docs/NOTES.md, \"Stage "
         "7a\".\n";
  std::cout << "compiler: " << aadp::build_info::compiler_id << ' '
            << aadp::build_info::compiler_version << ", " << aadp::build_info::build_type << ", "
            << aadp::build_info::cxx_flags << '\n';
  std::printf("run: seed %llu, paths %llu, batches %u, threads %u\n",
              static_cast<unsigned long long>(kSeed), static_cast<unsigned long long>(o.paths),
              kBatches, o.threads);
  std::fflush(stdout);
}

// The slice labels, as C strings for printf.
const char* Expiry(std::size_t j) {
  constexpr std::array<const char*, 3> kNames = {"202411", "202411W4", "202412"};
  static_assert(kNames.size() == aadp::svi::kCalibrationExpiries.size());
  return j < kNames.size() ? kNames[j] : "?";
}

const char* SegmentName(std::size_t s) {
  constexpr std::array<const char*, 3> kNames = {"[0, T1)", "[T1, T2)", "[T2, T3]"};
  return s < kNames.size() ? kNames[s] : "?";
}

void PrintSurface(const ChainSurface& s) {
  const aadp::svi::SviSurface<double> surface(s.terms, s.params);
  const aadp::svi::ForwardCurve& curve = surface.Curve();
  std::printf("\n== surface ==\n");
  std::printf("  S0 = D1 F1          %.6f\n", curve.Spot());
  double t0 = 0.0;
  for (std::size_t j = 0; j < s.terms.size(); ++j) {
    const aadp::svi::SliceTerms& t = s.terms[j];
    const aadp::svi::RawSvi<double>& p = s.params[j];
    std::printf(
        "  %-9s T %.10f  F %.6f  D %.8f  a %.10g  b %.10g  rho %.10g  m %.10g  sigma %.10g\n",
        Expiry(j), t.expiry, t.forward, t.discount, p.a, p.b, p.rho, p.m, p.sigma);
    const double dt = t.expiry - t0;
    std::printf("  segment %-9s carry %.6f  rate %.6f  (per year)\n", SegmentName(j),
                (curve.LogForward(t.expiry) - curve.LogForward(t0)) / dt,
                -(curve.LogDiscount(t.expiry) - curve.LogDiscount(t0)) / dt);
    t0 = t.expiry;
  }
}

void PrintRange(const char* label, const FloorRange& r, std::uint64_t steps) {
  if (r.count == 0) {
    std::printf("    %-22s 0\n", label);
    return;
  }
  std::printf("    %-22s %llu (%.4g%% of path-steps), k [%.5f, %.5f], t [%.6f, %.6f]\n", label,
              static_cast<unsigned long long>(r.count),
              100.0 * static_cast<double>(r.count) / static_cast<double>(steps), r.k_min, r.k_max,
              r.t_min, r.t_max);
}

void PrintFloors(const FloorTally& t, std::size_t segments) {
  std::printf("  floors: path-steps %llu over %llu paths\n",
              static_cast<unsigned long long>(t.path_steps),
              static_cast<unsigned long long>(t.paths));
  for (std::size_t c = 0; c < t.by_class.size(); ++c) {
    std::printf("    %-22s %llu\n",
                aadp::svi::LocalVarianceClassName(static_cast<aadp::svi::LocalVarianceClass>(c)),
                static_cast<unsigned long long>(t.by_class[c]));
  }
  std::printf("    paths with a floor     %llu (%.4g%%), with a cap %llu (%.4g%%)\n",
              static_cast<unsigned long long>(t.paths_with_floor),
              100.0 * static_cast<double>(t.paths_with_floor) / static_cast<double>(t.paths),
              static_cast<unsigned long long>(t.paths_with_cap),
              100.0 * static_cast<double>(t.paths_with_cap) / static_cast<double>(t.paths));
  PrintRange("floored, all", FloorTally::Total(t.floored), t.path_steps);
  PrintRange("capped, all", FloorTally::Total(t.capped), t.path_steps);
  for (std::size_t s = 0; s < segments; ++s) {
    const std::string f = std::string("floored, ") + SegmentName(s);
    const std::string c = std::string("capped, ") + SegmentName(s);
    PrintRange(f.c_str(), t.floored[s], t.path_steps);
    PrintRange(c.c_str(), t.capped[s], t.path_steps);
  }
}

RunConfig Run(const Options& o) {
  return {.seed = kSeed, .paths = o.paths, .batches = kBatches, .threads = o.threads};
}

int Report(const ChainSurface& s, const Options& o) {
  PrintSurface(s);
  const double maturity = s.terms.back().expiry;
  for (const aadp::svi::NamedContract& c : aadp::svi::SurfaceContracts(maturity)) {
    const auto r = aadp::ad::PriceLocalVolWithGradient(c.contract, s.terms, s.params, Run(o));
    const Contract& terms = c.contract.contract;
    const bool barrier = terms.product == aadp::pricer::Product::kBarrier;
    std::printf("\n== %s: K %.0f", c.name, terms.strike);
    if (barrier) std::printf(", H %.0f", terms.barrier.level);
    std::printf(", %u dates, n %u, T %.10f ==\n", terms.steps, c.contract.substeps,
                c.contract.maturity);
    if (!r) {
      std::printf("  REFUSED: %s\n", aadp::pricer::LocalVolErrorName(r.error().code));
      return kExitFailure;
    }
    std::printf("  price               %.6f +- %.6f (1 SE)\n", r->value.mean,
                r->value.standard_error);
    for (std::size_t i = 0; i < r->gradient.size(); ++i) {
      std::printf("  d/d %-8s %-5s  %+.6e +- %.3e\n", Expiry(i / 5), kParameterNames[i % 5],
                  r->gradient[i].mean, r->gradient[i].standard_error);
    }
    PrintFloors(r->floors, s.terms.size());
  }

  std::printf("\n== diagnostic, no tolerance: European call K %.0f at each slice's expiry ==\n",
              aadp::svi::kSurfaceStrike);
  for (std::size_t j = 0; j < s.terms.size(); ++j) {
    const aadp::svi::SliceTerms& t = s.terms[j];
    // As near one date a day as the production grid: round(33 T_j / T_3), at least 1.
    const auto dates = std::max<std::uint32_t>(
        1, static_cast<std::uint32_t>(std::lround(aadp::svi::kSurfaceDates * t.expiry / maturity)));
    Contract european;
    european.strike = aadp::svi::kSurfaceStrike;
    european.steps = dates;
    const LocalVolContract lv{
        .contract = european, .maturity = t.expiry, .substeps = aadp::svi::kSurfaceSubsteps};
    const auto r = aadp::pricer::PriceLocalVol(lv, s.terms, s.params, Run(o));
    const double k = std::log(aadp::svi::kSurfaceStrike / t.forward);
    const double w = aadp::svi::EvaluateSlice(aadp::svi::MakeSliceSetup(s.params[j]), k).w;
    const double black =
        aadp::svi::Black76Price(t.forward, t.discount, aadp::svi::kSurfaceStrike, t.expiry,
                                std::sqrt(w / t.expiry), aadp::svi::OptionType::kCall);
    if (!r) {
      std::printf("  %-9s REFUSED: %s\n", Expiry(j),
                  aadp::pricer::LocalVolErrorName(r.error().code));
      return kExitFailure;
    }
    std::printf(
        "  %-9s %u dates: local vol %.6f +- %.6f, Black-76 at w_j(k = %.5f) %.6f, "
        "difference %+.6f (%+.2f SE); floored %llu, capped %llu of %llu path-steps\n",
        Expiry(j), dates, r->value.mean, r->value.standard_error, k, black, r->value.mean - black,
        (r->value.mean - black) / r->value.standard_error,
        static_cast<unsigned long long>(r->floors.Floored()),
        static_cast<unsigned long long>(r->floors.Capped()),
        static_cast<unsigned long long>(r->floors.path_steps));
  }
  return 0;
}

// The bias plan's products and treatments, on the contract terms.
std::vector<aadp::svi::NamedContract> BiasContracts(double maturity) {
  using aadp::pricer::Monitoring;
  using aadp::pricer::Treatment;
  const std::array<aadp::svi::NamedContract, 3> base = aadp::svi::SurfaceContracts(maturity);
  Contract smoothed = aadp::svi::SurfaceDownAndOut(Monitoring::kDiscrete, Treatment::kSmoothed);
  smoothed.barrier.smoothing_width = 0.005;
  const auto lv = [&](const Contract& c) {
    return LocalVolContract{.contract = c, .maturity = maturity, .substeps = 1};
  };
  return {
      {.name = base[0].name, .contract = lv(base[0].contract.contract)},
      {.name = base[1].name, .contract = lv(base[1].contract.contract)},
      {.name = "down-out-call-discrete-hard",
       .contract = lv(aadp::svi::SurfaceDownAndOut(Monitoring::kDiscrete, Treatment::kHard))},
      {.name = "down-out-call-discrete-smoothed-0.005", .contract = lv(smoothed)},
      {.name = base[2].name, .contract = lv(base[2].contract.contract)},
      {.name = "down-out-call-continuous-sampled-crossing",
       .contract = lv(aadp::svi::SurfaceDownAndOut(Monitoring::kContinuous, Treatment::kHard))},
  };
}

// Statistics 0: V(n) on the coarse draws, 1: V(2n) on the production draws at 2n, 2: their
// difference, path by path.
class CoupledPathFn {
 public:
  CoupledPathFn(const Contract& c, std::uint32_t n,
                const aadp::pricer::LocalVolModel<double>& coarse,
                const aadp::pricer::LocalVolModel<double>& fine)
      : contract_(c),
        n_(n),
        coarse_(coarse),
        fine_(fine),
        normal_(std::size_t{c.steps} * n),
        crossing_(aadp::pricer::NeedsCrossingUniforms(c) ? std::size_t{c.steps} * n : 0) {}

  void operator()(const aadp::pricer::PathUniforms& u, std::array<double, 3>& out) {
    // The open grid of UniformFromBits: (i + 1/2) 2^-52.
    constexpr double kLo = 0x1p-53;
    constexpr double kHi = 1.0 - 0x1p-53;
    constexpr double kInvSqrt2 = 0.70710678118654752440;
    for (std::size_t j = 0; j < normal_.size(); ++j) {
      const double z = (aadp::pricer::InverseNormal(u.normal[2 * j]) +
                        aadp::pricer::InverseNormal(u.normal[(2 * j) + 1])) *
                       kInvSqrt2;
      normal_[j] = std::clamp(aadp::pricer::NormalCdf(z), kLo, kHi);
      if (!crossing_.empty()) crossing_[j] = u.crossing[2 * j];
    }
    const aadp::pricer::PathUniforms coarse{.normal = normal_, .crossing = crossing_};
    out[0] = aadp::pricer::EvaluateSubstepped<double>(contract_, n_, coarse_, coarse).value;
    out[1] = aadp::pricer::EvaluateSubstepped<double>(contract_, 2 * n_, fine_, u).value;
    out[2] = out[0] - out[1];
  }

 private:
  Contract contract_;
  std::uint32_t n_;
  aadp::pricer::LocalVolModel<double> coarse_;
  aadp::pricer::LocalVolModel<double> fine_;
  std::vector<double> normal_;
  std::vector<double> crossing_;
};

aadp::BatchStatistics Column(const std::vector<std::array<double, 3>>& table, std::size_t i) {
  std::vector<double> column;
  column.reserve(table.size());
  for (const auto& row : table) column.push_back(row[i]);
  return aadp::BatchMeansStatistics(column);
}

int Bias(const ChainSurface& s, const Options& o) {
  constexpr std::array<std::uint32_t, 4> kLevels = {1, 2, 4, 8};
  std::printf(
      "\n== Euler bias: ITERATION ONLY, no pass mark. Delta(n) = V(n) - V(2n) on coupled draws "
      "==\n");
  for (const aadp::svi::NamedContract& c : BiasContracts(s.terms.back().expiry)) {
    std::printf("\n-- %s --\n", c.name);
    std::array<double, 5> v{};
    std::array<double, 5> v_se{};
    std::array<aadp::BatchStatistics, 4> delta{};
    for (std::size_t l = 0; l < kLevels.size(); ++l) {
      const std::uint32_t n = kLevels[l];
      for (const std::uint32_t level : {n, 2 * n}) {
        LocalVolContract check = c.contract;
        check.substeps = level;
        const auto failure = aadp::pricer::ValidateLocalVol(check, s.terms, s.params, {});
        if (failure.code != aadp::pricer::LocalVolError::kNone) {
          std::printf("  REFUSED at n %u: %s\n", level,
                      aadp::pricer::LocalVolErrorName(failure.code));
          return kExitFailure;
        }
      }
      const Contract& contract = c.contract.contract;
      const double maturity = c.contract.maturity;
      const aadp::pricer::LocalVolModel<double> coarse(
          aadp::svi::SviSurface<double>(s.terms, s.params), maturity, contract.steps * n, {});
      const aadp::pricer::LocalVolModel<double> fine(
          aadp::svi::SviSurface<double>(s.terms, s.params), maturity, contract.steps * 2 * n, {});
      const aadp::pricer::PathShape shape{
          .steps = contract.steps * 2 * n,
          .crossing = aadp::pricer::NeedsCrossingUniforms(contract)};
      const auto table = aadp::pricer::RunBatches<3>(
          Run(o), shape, [&] { return CoupledPathFn(contract, n, coarse, fine); });
      const aadp::BatchStatistics coarse_v = Column(table, 0);
      const aadp::BatchStatistics fine_v = Column(table, 1);
      delta[l] = Column(table, 2);
      if (l == 0) {
        v[0] = coarse_v.mean;
        v_se[0] = coarse_v.standard_error;
      }
      v[l + 1] = fine_v.mean;
      v_se[l + 1] = fine_v.standard_error;
    }
    for (std::size_t l = 0; l < v.size(); ++l) {
      std::printf("  V(n = %2u)           %.6f +- %.6f%s\n", 1U << l, v[l], v_se[l],
                  l == 0 ? " (coarse draws of the n = 1 pair)" : " (production draws)");
    }
    for (std::size_t l = 0; l < delta.size(); ++l) {
      std::printf("  Delta(%u)            %+.6f +- %.6f (%+.2f SE)", kLevels[l], delta[l].mean,
                  delta[l].standard_error, delta[l].mean / delta[l].standard_error);
      if (l + 1 < delta.size()) {
        std::printf(", Delta(%u) / Delta(%u) %.3f", kLevels[l], kLevels[l + 1],
                    delta[l].mean / delta[l + 1].mean);
      }
      std::printf("\n");
    }
    const aadp::BatchStatistics& d4 = delta[2];
    std::printf("  bias left at n = 4  2 Delta(4) = %+.6f +- %.6f%s\n", 2.0 * d4.mean,
                2.0 * d4.standard_error,
                std::fabs(d4.mean) <= 2.0 * d4.standard_error ? ", not resolved at this N" : "");
  }
  return 0;
}

double Median(std::vector<double> v) {
  std::ranges::sort(v);
  const std::size_t n = v.size();
  return n % 2 == 1 ? v[n / 2] : 0.5 * (v[(n / 2) - 1] + v[n / 2]);
}

int Timing(const ChainSurface& s, const Options& o) {
  using Clock = std::chrono::steady_clock;
  std::printf("\n== timing: ITERATION ONLY, not a result ==\n");
  for (const aadp::svi::NamedContract& c : aadp::svi::SurfaceContracts(s.terms.back().expiry)) {
    std::vector<double> price_ms;
    std::vector<double> grad_ms;
    double sink = 0.0;
    for (std::uint32_t rep = 0; rep < o.repetitions; ++rep) {
      const auto t0 = Clock::now();
      const auto p = aadp::pricer::PriceLocalVol(c.contract, s.terms, s.params, Run(o));
      const auto t1 = Clock::now();
      const auto g = aadp::ad::PriceLocalVolWithGradient(c.contract, s.terms, s.params, Run(o));
      const auto t2 = Clock::now();
      if (!p || !g) {
        std::printf("  %s: REFUSED\n", c.name);
        return kExitFailure;
      }
      sink += p->value.mean + g->value.mean;
      price_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
      grad_ms.push_back(std::chrono::duration<double, std::milli>(t2 - t1).count());
    }
    const double price_median = Median(price_ms);
    const double grad_median = Median(grad_ms);
    std::printf(
        "  %-34s price median %.2f ms (min %.2f), with 15 sensitivities median %.2f ms (min "
        "%.2f), ratio %.2f, over %u repetitions (check %.6g)\n",
        c.name, price_median, *std::ranges::min_element(price_ms), grad_median,
        *std::ranges::min_element(grad_ms), grad_median / price_median, o.repetitions,
        sink / (2.0 * o.repetitions));
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) try {
  const std::optional<Options> parsed =
      Parse(std::span<char*>(argv, static_cast<std::size_t>(argc)));
  if (!parsed) return Usage();
  const Options& o = *parsed;

  // The attribution comes first, before any output that the chain could have shaped.
  Header(o);
  const auto option_bytes = aadp::taifex::read_binary_file(o.options_path);
  const auto future_bytes = aadp::taifex::read_binary_file(o.futures_path);
  if (!option_bytes || !future_bytes) {
    std::cerr << "aadp_local_vol_report: "
              << (option_bytes ? future_bytes.error() : option_bytes.error())
              << ". Run scripts/fetch_data.sh.\n";
    return kExitFailure;
  }
  const auto options = aadp::taifex::read_options(*option_bytes);
  const auto futures = aadp::taifex::read_futures(*future_bytes);
  if (!options || !futures) {
    std::cerr << "aadp_local_vol_report: "
              << aadp::taifex::to_string(options ? futures.error().code : options.error().code)
              << '\n';
    return kExitFailure;
  }
  const auto slices =
      aadp::svi::BuildChainSlices(options->rows, futures->rows, aadp::svi::kCalibrationExpiries);
  if (!slices) {
    std::cerr << "aadp_local_vol_report: " << aadp::svi::ChainErrorName(slices.error().code)
              << " on " << slices.error().expiry << '\n';
    return kExitFailure;
  }
  const std::optional<ChainSurface> surface =
      aadp::svi::SurfaceFromChain(*slices, aadp::svi::FitChain(*slices));
  if (!surface) {
    std::cerr << "aadp_local_vol_report: a slice did not fit\n";
    return kExitFailure;
  }
  const aadp::svi::SurfaceError error = aadp::svi::ValidateSurface(surface->terms, surface->params);
  if (error != aadp::svi::SurfaceError::kNone) {
    std::cerr << "aadp_local_vol_report: " << aadp::svi::SurfaceErrorName(error) << '\n';
    return kExitFailure;
  }
  switch (o.mode) {
    case Mode::kReport:
      return Report(*surface, o);
    case Mode::kBias:
      return Bias(*surface, o);
    case Mode::kTiming:
      return Timing(*surface, o);
  }
  return kExitFailure;
} catch (const std::exception& error) {
  std::fprintf(stderr, "aadp_local_vol_report: %s\n", error.what());
  return kExitFailure;
}
