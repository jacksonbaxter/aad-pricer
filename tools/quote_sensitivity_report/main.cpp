// Market-quote sensitivities through the raw SVI fit to the TAIFEX captures, by the implicit
// function theorem at the fit's constrained optimum, and their bump-and-recalibrate check
// (docs/NOTES.md, "Stage 7b"). Every number it prints is iteration only.
//
//   aadp_quote_sensitivity_report [--options <file>] [--futures <file>]
//                                 [--mode report|check|timing] [--paths <N>] [--threads <T>]
//                                 [--repetitions <R>] [--stride <S>]
//                                 [--refit polished|production]
//   aadp_quote_sensitivity_report --report [the same flags] [run flags]
//
// report  per slice the multipliers, the exact Hessian against Gauss-Newton, and every quote's
//         d theta* / d V_i in both chains; then per svi-surface contract the price, its 15
//         parameter sensitivities, d P / d(F_j, D_j) by common-random-number differences, and
//         the 180 quote sensitivities +- SE, in full and by expiry and strike bucket.
//         N = 2^20, 64 batches, 8 threads by default.
// check   the bump-and-recalibrate check of every quote in both chains, on the parameters and on
//         the three prices on common random numbers. N = 2^16, 8 threads by default. Exit 1 if
//         any comparison fails. --stride S checks only every S-th quote of each slice (the
//         tool test's short run); the registered check is S = 1, every quote. Each bumped
//         slice is refitted by the production fit from the same start; --refit polished (the
//         default) then moves each refit by one Newton step on its own KKT system, to the exact
//         KKT point, and --refit production keeps the refit as it stopped, as pre-registered
//         (docs/NOTES.md, "Stage 7b": the fit stops on its simplex tolerance, and at small h the
//         two refits' stopping errors do not cancel).
// timing  wall time per contract of the price with its 15 sensitivities, and of the quote
//         sensitivities on top, single thread, N = 2^16, 5 repetitions: run it through
//         measure.sh.
// The output is derived from the chain, so it opens with the attribution. Seed 20261115. Each
// mode but timing ends with machine-readable lines at full precision.
//
// --report writes the mode's summaries as a report file instead (src/results/report.hpp), for
// scripts/run_bench.sh and the stamp: the same doubles as the machine-readable lines, every table
// derived from the chain, with the attribution, and the timing mode's table marked as a timing.
// Summaries only (SVI-10): per slice the multipliers, the Hessians' difference and conditions and
// Gauss-Newton against exact; per contract the price, d P / d(F_j, D_j), and the quote
// sensitivities by expiry and strike bucket and over all quotes; the check's tallies, its failing
// counts by contract and its verdict, which stays FAIL in a cell when the check fails. No
// settlement, no per-quote row, no fitted parameter, forward or discount. A written report exits
// 0, its verdict in a cell, since scripts/run_bench.sh stamps only a run that exits 0; the text
// check keeps its exit 1. The run flags are those of src/results/run_header.hpp, and only --report
// takes them.

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <expected>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <ratio>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ad/local_vol_gradient.hpp"
#include "build_info.hpp"
#include "data/attribution.hpp"
#include "data/forwards.hpp"
#include "data/taifex_csv.hpp"
#include "data/taifex_futures.hpp"
#include "data/taifex_options.hpp"
#include "oracle/tolerance.hpp"
#include "pricer/local_vol.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"
#include "svi/arbitrage.hpp"
#include "svi/calibration_adjoint.hpp"
#include "svi/chain_adjoint.hpp"
#include "svi/chain_fit.hpp"
#include "svi/chain_surface.hpp"
#include "svi/fit.hpp"
#include "svi/implied_vol.hpp"
#include "svi/raw_svi.hpp"
#include "svi/surface.hpp"
#if AADP_REPORT_MODE
#include "results/report.hpp"
#include "results/report_run.hpp"
#include "results/results_file.hpp"
#include "results/run_header.hpp"
#endif

namespace {

using aadp::pricer::RunConfig;
using aadp::svi::ChainSurface;
using aadp::svi::HalvingJudgement;
using aadp::svi::MarketVector;
using aadp::svi::QuoteChain;
using aadp::svi::Vector5;

constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;
// A development seed, never 1729 or 271828: 7a's report seed, so the paths are 7a's.
constexpr std::uint64_t kSeed = 20261115;
constexpr std::uint32_t kBatches = 64;
// Halving levels h_l = 2^-l h_0, l = 0..5.
constexpr std::size_t kLevels = 6;
// d P / d F_j and d P / d D_j: h_0 = 2^-12 F_j and 2^-12 D_j.
constexpr double kMarketStep = 0x1p-12;
// The price's own rounding, as a refit error: 2^-44 |P| (docs/NOTES.md, "Stage 7b").
constexpr double kPriceRounding = 0x1p-44;
// The adjoint's rounding: kappa 2^-52 of the slice's largest derivative in that component.
constexpr double kUnitRoundoff = 0x1p-52;
// The parity derivatives' own rounding: 2^-40 of the slice's largest.
constexpr double kParityRounding = 0x1p-40;
constexpr std::array<const char*, 5> kParameterNames = {"a", "b", "rho", "m", "sigma"};
constexpr std::array<const char*, 3> kMarketNames = {"F", "D", "sigma_leg"};
constexpr std::array<double, 5> kBucketEdges = {-0.10, -0.05, -0.02, 0.02, 0.05};
constexpr std::array<const char*, 6> kBucketNames = {"k < -0.10",          "-0.10 <= k < -0.05",
                                                     "-0.05 <= k < -0.02", "-0.02 <= k < 0.02",
                                                     "0.02 <= k < 0.05",   "k >= 0.05"};

enum class Mode : std::uint8_t { kReport, kCheck, kTiming };
enum class RefitKind : std::uint8_t { kPolished, kProduction };

struct Options {
  std::string options_path = "data/DailyMarketReportOpt.csv";
  std::string futures_path = "data/DailyMarketReportFut.csv";
  Mode mode = Mode::kReport;
  std::uint64_t paths = 0;  // 0: the mode's default
  std::uint32_t threads = 0;
  std::uint32_t repetitions = 5;
  std::uint32_t stride = 1;
  RefitKind refit = RefitKind::kPolished;
  bool report = false;
  bool run_flags = false;  // any of the run flags of src/results/run_header.hpp
#if AADP_REPORT_MODE
  aadp::results::RunOptions run;
#endif
};

int Usage() {
  std::cerr << "usage: aadp_quote_sensitivity_report [--report] [--options <file>] "
               "[--futures <file>] [--mode report|check|timing] [--paths <N, a multiple of 64>] "
               "[--threads <T>] [--repetitions <R>] [--stride <S>] [--refit polished|production] "
               "[run flags, with --report only]\n";
  return kExitUsage;
}

std::uint64_t Count(std::string_view text) {
  std::uint64_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc() && end == text.data() + text.size() ? value : 0;
}

std::optional<Mode> ParseMode(std::string_view value) {
  if (value == "report") return Mode::kReport;
  if (value == "check") return Mode::kCheck;
  if (value == "timing") return Mode::kTiming;
  return std::nullopt;
}

std::uint32_t Bounded(std::string_view value, std::uint64_t limit) {
  const std::uint64_t n = Count(value);
  return n <= limit ? static_cast<std::uint32_t>(n) : 0;
}

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
  } else if (flag == "--stride") {
    o.stride = Bounded(value, 1000);
    return o.stride != 0;
  } else if (flag == "--refit") {
    if (value == "polished") {
      o.refit = RefitKind::kPolished;
    } else if (value == "production") {
      o.refit = RefitKind::kProduction;
    } else {
      return false;
    }
  } else if (flag == "--repetitions") {
    o.repetitions = Bounded(value, 1000);
    return o.repetitions != 0;
  } else {
    return false;
  }
  return true;
}

// The flags, run flags included; nothing for a usage error.
std::optional<Options> Parse(int argc, char* const* argv) {
  Options o;
  std::vector<std::string> args;
#if AADP_REPORT_MODE
  auto rest = aadp::results::take_run_flags(argc, argv, o.run);
  if (!rest) {
    std::cerr << "aadp_quote_sensitivity_report: " << rest.error() << '\n';
    return std::nullopt;
  }
  args = std::move(*rest);
  o.run_flags = args.size() + 1 != static_cast<std::size_t>(argc);
#else
  const std::span<char* const> all(argv, static_cast<std::size_t>(argc));
  args.assign(all.begin() + 1, all.end());
#endif
  std::erase_if(args, [&o](const std::string& arg) {
    const bool is_report = arg == "--report";
    o.report = o.report || is_report;
    return is_report;
  });
  for (std::size_t i = 0; i < args.size(); i += 2) {
    if (i + 1 == args.size() || !Apply(o, args[i], args[i + 1])) return std::nullopt;
  }
  // The run flags belong to the report file's header, which the text report does not have.
  if (o.run_flags && !o.report) return std::nullopt;
  const bool report = o.mode == Mode::kReport;
  if (o.paths == 0) o.paths = report ? (std::uint64_t{1} << 20) : (std::uint64_t{1} << 16);
  if (o.threads == 0) o.threads = o.mode == Mode::kTiming ? 1 : 8;
  return o;
}

void Header(const Options& o) {
  aadp::write_attribution(std::cout);
  std::cout
      << "\naadp_quote_sensitivity_report: sensitivities to the calibration settlements through "
         "the raw SVI fit to TAIFEX TXO settlements of trade date 2024-11-15.\n"
         "These are daily settlement prices set by the exchange's rules, not tradable quotes.\n"
         "Maturity reaches 33 days only: nothing is extrapolated past the 202412 expiry.\n"
         "ITERATION ONLY, not a result. Method, chains, KKT treatment and the check: "
         "docs/NOTES.md, \"Stage 7b\".\n";
  std::cout << "compiler: " << aadp::build_info::compiler_id << ' '
            << aadp::build_info::compiler_version << ", " << aadp::build_info::build_type << ", "
            << aadp::build_info::cxx_flags << '\n';
  std::printf("run: seed %llu, paths %llu, batches %u, threads %u\n",
              static_cast<unsigned long long>(kSeed), static_cast<unsigned long long>(o.paths),
              kBatches, o.threads);
  std::fflush(stdout);
}

const char* Expiry(std::size_t j) {
  constexpr std::array<const char*, 3> kNames = {"202411", "202411W4", "202412"};
  static_assert(kNames.size() == aadp::svi::kCalibrationExpiries.size());
  return j < kNames.size() ? kNames[j] : "?";
}

std::array<double, 5> AsArray(const aadp::svi::SviParams& p) {
  return {p.a, p.b, p.rho, p.m, p.sigma};
}

// One slice: its parity points, fit and both chains.
struct Slice {
  const aadp::svi::ChainSlice* chain = nullptr;
  std::vector<aadp::ParityPoint> points;
  aadp::svi::FitResult fit;
  aadp::svi::SliceQuoteSensitivity s;
};

struct Loaded {
  std::vector<aadp::svi::ChainSlice> chain;
  std::vector<Slice> slices;
  ChainSurface surface;
};

std::optional<Loaded> Load(const Options& o, std::span<const aadp::taifex::OptionRow> options,
                           std::span<const aadp::taifex::FuturesRow> futures) {
  auto chain = aadp::svi::BuildChainSlices(options, futures, aadp::svi::kCalibrationExpiries);
  if (!chain) {
    std::cerr << "aadp_quote_sensitivity_report: " << aadp::svi::ChainErrorName(chain.error().code)
              << " on " << chain.error().expiry << '\n';
    return std::nullopt;
  }
  Loaded out;
  out.chain = std::move(*chain);
  const aadp::svi::SurfaceFit fit = aadp::svi::FitChain(out.chain);
  const std::optional<ChainSurface> surface = aadp::svi::SurfaceFromChain(out.chain, fit);
  if (!surface) {
    std::cerr << "aadp_quote_sensitivity_report: a slice did not fit\n";
    return std::nullopt;
  }
  out.surface = *surface;
  for (std::size_t j = 0; j < out.chain.size(); ++j) {
    Slice s;
    s.chain = &out.chain[j];
    s.points = aadp::settlement_parity_points(options, out.chain[j].expiry);
    s.fit = *fit.slices[j].fit;
    const auto q = aadp::svi::SliceQuoteSensitivities(out.chain[j], s.points, s.fit);
    if (!q) {
      std::printf("\n== slice %s: adjoint REFUSED: code %d, %s, value %.6g ==\n", Expiry(j),
                  static_cast<int>(q.error().code),
                  aadp::svi::AdjointErrorName(q.error().adjoint.code), q.error().adjoint.value);
      return std::nullopt;
    }
    s.s = *q;
    out.slices.push_back(std::move(s));
  }
  (void)o;
  return out;
}

RunConfig Run(const Options& o) {
  return {.seed = kSeed, .paths = o.paths, .batches = kBatches, .threads = o.threads};
}

void PrintMatrix(const char* label, const aadp::svi::Matrix5& m) {
  std::printf("  %s\n", label);
  for (std::size_t i = 0; i < 5; ++i) {
    std::printf("    %-6s", kParameterNames[i]);
    for (std::size_t j = 0; j < 5; ++j) std::printf(" %+.6e", m[i][j]);
    std::printf("\n");
  }
}

// The tick is piecewise constant in V_i, so its derivative is 0 except at a boundary, where a
// settlement has one-sided derivatives only.
std::size_t OnTickBoundary(const Slice& s) {
  std::size_t on_boundary = 0;
  for (const aadp::svi::FitQuote& q : s.chain->quotes.quotes) {
    on_boundary += static_cast<std::size_t>(q.price == 10.0 || q.price == 50.0 ||
                                            q.price == 500.0 || q.price == 1000.0);
  }
  return on_boundary;
}

// max_rc |H - GN|_rc / sqrt(GN_rr GN_cc).
double HessianDifference(const aadp::svi::CalibrationAdjoint& a) {
  double worst = 0.0;
  for (std::size_t r = 0; r < 5; ++r) {
    for (std::size_t c = 0; c < 5; ++c) {
      const double scale =
          std::sqrt(a.derivatives.gauss_newton[r][r] * a.derivatives.gauss_newton[c][c]);
      worst = std::max(
          worst, std::abs(a.derivatives.hessian[r][c] - a.derivatives.gauss_newton[r][c]) / scale);
    }
  }
  return worst;
}

// Per parameter, max over quotes |GN - exact| / max |exact| of d theta / d V; a.gauss_newton must
// be set.
Vector5 GaussNewtonAgainstExact(const aadp::svi::CalibrationAdjoint& a) {
  Vector5 out{};
  for (std::size_t p = 0; p < 5; ++p) {
    double diff = 0.0;
    double scale = 0.0;
    for (std::size_t i = 0; i < a.exact.quote.size(); ++i) {
      diff = std::max(diff, std::abs(a.gauss_newton->quote[i][p] - a.exact.quote[i][p]));
      scale = std::max(scale, std::abs(a.exact.quote[i][p]));
    }
    out[p] = diff / scale;
  }
  return out;
}

void PrintAdjoint(std::size_t j, const Slice& s) {
  const aadp::svi::CalibrationAdjoint& a = s.s.adjoint;
  const aadp::svi::FitResult& f = s.fit;
  std::printf("\n== slice %s: %zu quotes, F %.6f, D %.8f, sigma_leg %.6f ==\n", Expiry(j), f.quotes,
              s.chain->parity.forward, s.chain->parity.discount, s.chain->parity.sigma_leg);
  std::printf("  theta              a %.12g  b %.12g  rho %.12g  m %.12g  sigma %.12g\n",
              f.params.a, f.params.b, f.params.rho, f.params.m, f.params.sigma);
  std::printf("  Phi                %.10g (fit), %.10g (tape's Black-76), converged %s\n",
              f.objective, a.derivatives.objective, f.converged ? "yes" : "no");
  std::printf("  settlements on a tick boundary (10, 50, 500, 1000): %zu\n", OnTickBoundary(s));
  std::printf("  active bounds     ");
  for (const aadp::svi::Bound b : f.ActiveBounds()) std::printf(" %s", aadp::svi::BoundName(b));
  std::printf("\n");
  for (const aadp::svi::ActiveConstraint& c : a.constraints) {
    std::printf(
        "  multiplier %-16s mu %.9e (Newton), %.9e (least squares), %.9e (second step); "
        "margin %.3e, significance mu / SE %.4f, quote distance %.4f noise units\n",
        aadp::svi::BoundName(c.bound), c.multiplier, c.multiplier_least_squares,
        c.multiplier_second_step, c.margin, c.significance, c.quote_distance);
  }
  std::printf(
      "  stationarity       |S r| %.3e of |S grad Phi| %.3e (scaled least-squares residual)\n",
      a.stationarity, a.gradient_norm);
  std::printf("  Newton step        a %+.3e  b %+.3e  rho %+.3e  m %+.3e  sigma %+.3e\n",
              a.newton_step[0], a.newton_step[1], a.newton_step[2], a.newton_step[3],
              a.newton_step[4]);
  std::printf("  Hessian asymmetry  %.3e (rounding)\n", a.derivatives.hessian_asymmetry);
  PrintMatrix("exact Hessian of Phi", a.derivatives.hessian);
  PrintMatrix("Gauss-Newton 2 J^T J", a.derivatives.gauss_newton);
  std::printf("  exact - GN         max |H - GN|_rc / sqrt(GN_rr GN_cc) = %.4e\n",
              HessianDifference(a));
  std::printf("  KKT condition      exact %.4e, manifold residual %.2e", a.exact.condition,
              a.exact.manifold_residual);
  if (a.gauss_newton) {
    std::printf("; Gauss-Newton %.4e, manifold residual %.2e\n", a.gauss_newton->condition,
                a.gauss_newton->manifold_residual);
  } else {
    std::printf("; Gauss-Newton REFUSED: %s\n",
                aadp::svi::AdjointErrorName(a.gauss_newton.error().code));
  }
  if (a.gauss_newton) {
    std::printf("  d theta / d V, GN against exact, max over quotes |GN - exact| / max |exact|:");
    const Vector5 relative = GaussNewtonAgainstExact(a);
    for (std::size_t p = 0; p < 5; ++p) std::printf(" %s %.4f", kParameterNames[p], relative[p]);
    std::printf("\n");
  }
}

void PrintQuoteTable(std::size_t j, const Slice& s) {
  const aadp::svi::SliceQuoteSensitivity& q = s.s;
  std::printf("\n-- slice %s, every quote: d theta* / d V_i per point of settlement --\n",
              Expiry(j));
  std::printf(
      "  columns: i K right V noise k leg | exact (a b rho m sigma) | Gauss-Newton (5) | through "
      "parity, exact (5) | d(F D sigma_leg)/dV (3)\n");
  for (std::size_t i = 0; i < q.through_parity.size(); ++i) {
    const aadp::svi::FitQuote& fq = s.chain->quotes.quotes[i];
    std::printf("  quote %s %2zu %7.0f %s %9.2f %6.4f %+.5f %s |", Expiry(j), i, fq.strike,
                fq.type == aadp::svi::OptionType::kCall ? "C" : "P", fq.price, fq.noise,
                std::log(fq.strike / s.chain->parity.forward), q.legs[i].present ? "y" : "n");
    for (const double v : q.adjoint.exact.quote[i]) std::printf(" %+.6e", v);
    std::printf(" |");
    if (q.adjoint.gauss_newton) {
      for (const double v : q.adjoint.gauss_newton->quote[i]) std::printf(" %+.6e", v);
    } else {
      std::printf(" Gauss-Newton refused");
    }
    std::printf(" |");
    for (const double v : q.through_parity[i]) std::printf(" %+.6e", v);
    std::printf(" |");
    for (const double v : q.market[i]) std::printf(" %+.6e", v);
    std::printf("\n");
  }
  std::printf("  d theta* / d(F, D, sigma_leg), exact:\n");
  for (std::size_t p = 0; p < 3; ++p) {
    std::printf("    %-9s", kMarketNames[p]);
    for (const double v : q.adjoint.exact.market[p]) std::printf(" %+.6e", v);
    std::printf("\n");
  }
}

// The surface with slice j's terms and parameters replaced.
struct Surface {
  std::vector<aadp::svi::SliceTerms> terms;
  std::vector<aadp::svi::RawSvi<double>> params;
};

Surface Base(const ChainSurface& s) { return {.terms = s.terms, .params = s.params}; }

std::optional<aadp::pricer::Estimate> PriceOn(const aadp::svi::NamedContract& c, const Surface& s,
                                              const RunConfig& run) {
  const auto r = aadp::pricer::PriceLocalVol(c.contract, s.terms, s.params, run);
  if (!r) return std::nullopt;
  return r->value;
}

// d P / d F_j and d P / d D_j by central differences on common random numbers, h by halving.
struct MarketPriceDerivative {
  std::array<HalvingJudgement, 2> select{};  // F, D
  std::array<std::vector<double>, 2> batch;  // per batch, at the selected level
  std::array<double, 2> error{};             // kHalvingFactor (truncation + refit)
};

// F_j for which = 0, D_j for which = 1.
double& MarketInputOf(Surface& s, std::size_t j, std::size_t which) {
  return which == 0 ? s.terms[j].forward : s.terms[j].discount;
}

struct MarketSelection {
  HalvingJudgement select;
  std::vector<double> batch;
};

std::optional<MarketSelection> MarketLevel(const aadp::svi::NamedContract& c,
                                           const ChainSurface& base, const RunConfig& run,
                                           double price, std::size_t j, std::size_t which) {
  std::array<double, kLevels> h{};
  std::array<double, kLevels> diff{};
  std::array<bool, kLevels> usable{};
  std::array<std::vector<double>, kLevels> batch;
  for (std::size_t l = 0; l < kLevels; ++l) {
    Surface up = Base(base);
    Surface down = Base(base);
    h[l] = std::ldexp(kMarketStep * MarketInputOf(up, j, which), -static_cast<int>(l));
    MarketInputOf(up, j, which) += h[l];
    MarketInputOf(down, j, which) -= h[l];
    const auto pu = PriceOn(c, up, run);
    const auto pd = PriceOn(c, down, run);
    if (!pu || !pd) continue;
    usable[l] = true;
    diff[l] = (pu->mean - pd->mean) / (2.0 * h[l]);
    batch[l].resize(pu->batch_means.size());
    for (std::size_t b = 0; b < batch[l].size(); ++b) {
      batch[l][b] = (pu->batch_means[b] - pd->batch_means[b]) / (2.0 * h[l]);
    }
  }
  const HalvingJudgement sel =
      aadp::svi::SelectByHalving(h, diff, usable, kPriceRounding * std::abs(price));
  if (!sel.resolved) return std::nullopt;
  return MarketSelection{.select = sel, .batch = batch[sel.level]};
}

std::optional<std::vector<MarketPriceDerivative>> MarketDerivatives(
    const aadp::svi::NamedContract& c, const ChainSurface& base, const RunConfig& run,
    double price) {
  std::vector<MarketPriceDerivative> out(base.terms.size());
  for (std::size_t j = 0; j < base.terms.size(); ++j) {
    for (std::size_t which = 0; which < 2; ++which) {
      auto sel = MarketLevel(c, base, run, price, j, which);
      if (!sel) return std::nullopt;
      out[j].select[which] = sel->select;
      out[j].batch[which] = std::move(sel->batch);
      out[j].error[which] =
          aadp::svi::kHalvingFactor * (sel->select.truncation + sel->select.refit);
    }
  }
  return out;
}

int Bucket(double k) {
  int b = 0;
  for (const double edge : kBucketEdges) {
    if (k >= edge) ++b;
  }
  return b;
}

// Per batch, d P / d V_i = sum_j dP/dtheta_j dtheta_j/dV_i (+ dP/dF dF/dV + dP/dD dD/dV).
std::vector<double> QuoteBatches(const aadp::ad::LocalVolGradientEstimate& g, std::size_t slice,
                                 const Vector5& d_theta, const MarketVector* market,
                                 const MarketPriceDerivative* d_market) {
  const std::size_t batches = g.value.batch_means.size();
  std::vector<double> out(batches, 0.0);
  for (std::size_t b = 0; b < batches; ++b) {
    double v = 0.0;
    for (std::size_t p = 0; p < 5; ++p) {
      v += g.gradient[(5 * slice) + p].batch_means[b] * d_theta[p];
    }
    if (market != nullptr && d_market != nullptr) {
      v += d_market->batch[0][b] * (*market)[0];
      v += d_market->batch[1][b] * (*market)[1];
    }
    out[b] = v;
  }
  return out;
}

void PrintContract(const aadp::ad::LocalVolGradientEstimate& g,
                   std::span<const MarketPriceDerivative> dm) {
  std::printf("  price               %.6f +- %.6f (1 SE)\n", g.value.mean, g.value.standard_error);
  for (std::size_t i = 0; i < g.gradient.size(); ++i) {
    std::printf("  d/d %-8s %-5s  %+.6e +- %.3e\n", Expiry(i / 5), kParameterNames[i % 5],
                g.gradient[i].mean, g.gradient[i].standard_error);
  }
  for (std::size_t j = 0; j < dm.size(); ++j) {
    for (std::size_t w = 0; w < 2; ++w) {
      const aadp::BatchStatistics st = aadp::BatchMeansStatistics(dm[j].batch[w]);
      std::printf(
          "  d/d %-8s %-9s  %+.6e +- %.3e (CRN difference, level %zu, truncation %.2e, "
          "rounding %.2e)\n",
          Expiry(j), w == 0 ? "F" : "D", st.mean, st.standard_error, dm[j].select[w].level,
          dm[j].select[w].truncation, dm[j].select[w].refit);
    }
  }
}

constexpr std::size_t kVersionCount = 3;
constexpr std::size_t kBucketCount = 6;
constexpr std::array<const char*, kVersionCount> kVersions = {
    "market fixed, exact", "market fixed, Gauss-Newton", "through parity, exact"};
// The versions in machine-readable lines and report row keys.
constexpr std::array<const char*, kVersionCount> kVersionKeys = {"market-fixed", "market-fixed-gn",
                                                                 "through-parity"};
// Each bucket's lower and upper edge in k, as the report spells them.
constexpr std::array<const char*, kBucketCount> kBucketFrom = {"open",  "-0.10", "-0.05",
                                                               "-0.02", "0.02",  "0.05"};
constexpr std::array<const char*, kBucketCount> kBucketTo = {"-0.10", "-0.05", "-0.02",
                                                             "0.02",  "0.05",  "open"};

// Batch sums per version, expiry and strike bucket, and over every quote.
class Summary {
 public:
  struct Sums {
    std::vector<double> batch;
    std::size_t n = 0;
    double max_abs = 0.0;
  };

  void Add(std::size_t version, std::size_t slice, int bucket, std::span<const double> batch,
           double mean) {
    Sums& s = sums_[version][slice][static_cast<std::size_t>(bucket)];
    Accumulate(s.batch, batch);
    Accumulate(total_[version], batch);
    s.n += 1;
    s.max_abs = std::max(s.max_abs, std::abs(mean));
  }

  [[nodiscard]] const Sums& At(std::size_t version, std::size_t slice, std::size_t bucket) const {
    return sums_[version][slice][bucket];
  }

  [[nodiscard]] std::size_t Counted(std::size_t version, std::size_t slices) const {
    std::size_t counted = 0;
    for (std::size_t j = 0; j < slices; ++j) {
      for (std::size_t b = 0; b < kBucketCount; ++b) counted += sums_[version][j][b].n;
    }
    return counted;
  }

  // The sum over every quote counted; only when one was.
  [[nodiscard]] aadp::BatchStatistics Total(std::size_t version) const {
    return aadp::BatchMeansStatistics(total_[version]);
  }

  void Print(std::size_t slices) const {
    for (std::size_t version = 0; version < kVersionCount; ++version) {
      std::printf("  summary, %s: by expiry and k = ln(K / F) bucket: n, sum +- SE, max |.|\n",
                  kVersions[version]);
      for (std::size_t j = 0; j < slices; ++j) {
        for (std::size_t b = 0; b < kBucketCount; ++b) {
          const Sums& bucket = sums_[version][j][b];
          if (bucket.n == 0) continue;
          const aadp::BatchStatistics st = aadp::BatchMeansStatistics(bucket.batch);
          std::printf("    %-8s %-19s n %2zu  sum %+.6e +- %.2e  max %.4e\n", Expiry(j),
                      kBucketNames[b], bucket.n, st.mean, st.standard_error, bucket.max_abs);
        }
      }
      const std::size_t counted = Counted(version, slices);
      if (counted == 0) {
        std::printf("    no quotes: refused on every slice\n");
        continue;
      }
      const aadp::BatchStatistics st = Total(version);
      std::printf("    all %zu quotes%s, sum %+.6e +- %.2e (every settlement up one point)\n",
                  counted, counted == 180 ? "" : " counted (a slice refused, or not the capture)",
                  st.mean, st.standard_error);
    }
  }

 private:
  static void Accumulate(std::vector<double>& into, std::span<const double> batch) {
    if (into.empty()) into.assign(batch.size(), 0.0);
    for (std::size_t b = 0; b < batch.size(); ++b) into[b] += batch[b];
  }

  std::array<std::array<std::array<Sums, kBucketCount>, 3>, kVersionCount> sums_{};
  std::array<std::vector<double>, kVersionCount> total_{};
};

// One quote's d P / d V_i in the three versions; Gauss-Newton's is empty where it refused.
struct QuoteRow {
  std::size_t slice = 0;
  std::size_t index = 0;
  double k = 0.0;
  std::array<std::optional<aadp::BatchStatistics>, kVersionCount> versions;
};

// One contract of the report: the price with its gradient, d P / d(F_j, D_j), every quote's
// sensitivities and their summary.
struct ContractResult {
  const char* name = "";
  aadp::ad::LocalVolGradientEstimate g;
  std::vector<MarketPriceDerivative> dm;
  std::vector<QuoteRow> quotes;
  Summary summary;
};

// The contract priced, differenced in F and D, and composed with every quote's adjoint; on a
// refusal, the line the text prints for it.
std::expected<ContractResult, std::string> ComputeContract(const Loaded& data,
                                                           const aadp::svi::NamedContract& c,
                                                           const Options& o) {
  const auto g = aadp::ad::PriceLocalVolWithGradient(c.contract, data.surface.terms,
                                                     data.surface.params, Run(o));
  if (!g) {
    return std::unexpected(std::string("REFUSED: ") +
                           aadp::pricer::LocalVolErrorName(g.error().code));
  }
  auto dm = MarketDerivatives(c, data.surface, Run(o), g->value.mean);
  if (!dm) return std::unexpected(std::string("d P / d(F, D): no level resolved"));
  ContractResult out;
  out.name = c.name;
  out.g = *g;
  out.dm = std::move(*dm);
  for (std::size_t j = 0; j < data.slices.size(); ++j) {
    const Slice& s = data.slices[j];
    const bool has_gn = s.s.adjoint.gauss_newton.has_value();
    for (std::size_t i = 0; i < s.s.through_parity.size(); ++i) {
      const aadp::svi::FitQuote& fq = s.chain->quotes.quotes[i];
      QuoteRow row;
      row.slice = j;
      row.index = i;
      row.k = std::log(fq.strike / s.chain->parity.forward);
      const Vector5 gn = has_gn ? s.s.adjoint.gauss_newton->quote[i] : Vector5{};
      const std::array<std::vector<double>, kVersionCount> v = {
          QuoteBatches(out.g, j, s.s.adjoint.exact.quote[i], nullptr, nullptr),
          QuoteBatches(out.g, j, gn, nullptr, nullptr),
          QuoteBatches(out.g, j, s.s.through_parity[i], &s.s.market[i], &out.dm[j])};
      for (std::size_t version = 0; version < kVersionCount; ++version) {
        if (version == 1 && !has_gn) continue;
        const aadp::BatchStatistics st = aadp::BatchMeansStatistics(v[version]);
        row.versions[version] = st;
        out.summary.Add(version, j, Bucket(row.k), v[version], st.mean);
      }
      out.quotes.push_back(row);
    }
  }
  return out;
}

// One row per quote, in the three versions.
void PrintQuoteRows(const Loaded& data, const ContractResult& r) {
  std::printf("  quote sensitivities d P / d V_i (per point of settlement), +- SE:\n");
  std::printf("    columns: slice i K right k | market fixed exact | GN | through parity\n");
  for (const QuoteRow& row : r.quotes) {
    const aadp::svi::FitQuote& fq = data.slices[row.slice].chain->quotes.quotes[row.index];
    std::printf("    %-8s %2zu %7.0f %s %+.5f", Expiry(row.slice), row.index, fq.strike,
                fq.type == aadp::svi::OptionType::kCall ? "C" : "P", row.k);
    for (std::size_t version = 0; version < kVersionCount; ++version) {
      const std::optional<aadp::BatchStatistics>& v = row.versions[version];
      if (!v) {
        std::printf(" | Gauss-Newton refused       ");
        continue;
      }
      std::printf(" | %+.6e +- %.2e", v->mean, v->standard_error);
    }
    std::printf("\n");
  }
}

// A bound as a report row key spells it.
const char* BoundKey(aadp::svi::Bound bound) {
  switch (bound) {
    case aadp::svi::Bound::kRhoLower:
      return "rho-lower";
    case aadp::svi::Bound::kRhoUpper:
      return "rho-upper";
    case aadp::svi::Bound::kBZero:
      return "b-zero";
    case aadp::svi::Bound::kSlope:
      return "slope";
    case aadp::svi::Bound::kAUpper:
      return "a-upper";
    case aadp::svi::Bound::kMLower:
      return "m-lower";
    case aadp::svi::Bound::kMUpper:
      return "m-upper";
    case aadp::svi::Bound::kSigmaLower:
      return "sigma-lower";
    case aadp::svi::Bound::kSigmaUpper:
      return "sigma-upper";
    case aadp::svi::Bound::kMinVariance:
      return "min-variance";
  }
  return "unknown";
}

// The report's figures at full precision, one line each, for comparing its cells with the text:
//   slice,expiry,quotes,objective,on_tick_boundary,active_bounds,stationarity,gradient_norm,
//         hessian_asymmetry,hessian_gn_difference,condition,manifold_residual,newton_a,..,
//         newton_sigma,gauss_newton
//   gauss_newton,expiry,condition,manifold_residual,against_exact_a,..,against_exact_sigma
//   multiplier,expiry,bound,newton,least_squares,second_step,margin,significance,quote_distance
//   price,contract,paths,mean,se
//   market,contract,expiry,dp_df,dp_df_se,dp_df_level,dp_df_truncation,dp_df_rounding,dp_dd,
//          dp_dd_se,dp_dd_level,dp_dd_truncation,dp_dd_rounding
//   bucket,contract,version,expiry,b<i>,n,sum,se,max_abs
//   total,contract,version,n,sum,se
void PrintSliceLines(std::size_t j, const Slice& s) {
  const aadp::svi::CalibrationAdjoint& a = s.s.adjoint;
  std::printf("slice,%s,%zu,%.17g,%zu,%zu,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g", Expiry(j),
              s.fit.quotes, s.fit.objective, OnTickBoundary(s), a.constraints.size(),
              a.stationarity, a.gradient_norm, a.derivatives.hessian_asymmetry,
              HessianDifference(a), a.exact.condition, a.exact.manifold_residual);
  for (const double v : a.newton_step) std::printf(",%.17g", v);
  std::printf(",%s\n",
              a.gauss_newton ? "ok" : aadp::svi::AdjointErrorName(a.gauss_newton.error().code));
  if (a.gauss_newton) {
    std::printf("gauss_newton,%s,%.17g,%.17g", Expiry(j), a.gauss_newton->condition,
                a.gauss_newton->manifold_residual);
    for (const double v : GaussNewtonAgainstExact(a)) std::printf(",%.17g", v);
    std::printf("\n");
  }
  for (const aadp::svi::ActiveConstraint& c : a.constraints) {
    std::printf("multiplier,%s,%s,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g\n", Expiry(j),
                BoundKey(c.bound), c.multiplier, c.multiplier_least_squares,
                c.multiplier_second_step, c.margin, c.significance, c.quote_distance);
  }
}

void PrintSummaryLines(const ContractResult& r, std::size_t slices) {
  for (std::size_t version = 0; version < kVersionCount; ++version) {
    for (std::size_t j = 0; j < slices; ++j) {
      for (std::size_t b = 0; b < kBucketCount; ++b) {
        const Summary::Sums& bucket = r.summary.At(version, j, b);
        if (bucket.n == 0) continue;
        const aadp::BatchStatistics st = aadp::BatchMeansStatistics(bucket.batch);
        std::printf("bucket,%s,%s,%s,b%zu,%zu,%.17g,%.17g,%.17g\n", r.name, kVersionKeys[version],
                    Expiry(j), b, bucket.n, st.mean, st.standard_error, bucket.max_abs);
      }
    }
    const std::size_t counted = r.summary.Counted(version, slices);
    if (counted == 0) continue;
    const aadp::BatchStatistics st = r.summary.Total(version);
    std::printf("total,%s,%s,%zu,%.17g,%.17g\n", r.name, kVersionKeys[version], counted, st.mean,
                st.standard_error);
  }
}

void PrintReportLines(const Loaded& data, std::span<const ContractResult> results,
                      const Options& o) {
  std::printf("\n== machine-readable, full precision ==\n");
  for (std::size_t j = 0; j < data.slices.size(); ++j) PrintSliceLines(j, data.slices[j]);
  for (const ContractResult& r : results) {
    std::printf("price,%s,%llu,%.17g,%.17g\n", r.name, static_cast<unsigned long long>(o.paths),
                r.g.value.mean, r.g.value.standard_error);
    for (std::size_t j = 0; j < r.dm.size(); ++j) {
      std::printf("market,%s,%s", r.name, Expiry(j));
      for (std::size_t w = 0; w < 2; ++w) {
        const aadp::BatchStatistics st = aadp::BatchMeansStatistics(r.dm[j].batch[w]);
        std::printf(",%.17g,%.17g,%zu,%.17g,%.17g", st.mean, st.standard_error,
                    r.dm[j].select[w].level, r.dm[j].select[w].truncation, r.dm[j].select[w].refit);
      }
      std::printf("\n");
    }
    PrintSummaryLines(r, data.slices.size());
  }
}

// Every contract of the report, or nothing once one is refused, after printing why when asked.
std::optional<std::vector<ContractResult>> ComputeContracts(const Loaded& data, const Options& o,
                                                            bool print) {
  std::vector<ContractResult> out;
  const double maturity = data.surface.terms.back().expiry;
  for (const aadp::svi::NamedContract& c : aadp::svi::SurfaceContracts(maturity)) {
    if (print) std::printf("\n== %s ==\n", c.name);
    auto r = ComputeContract(data, c, o);
    if (!r) {
      if (print) {
        std::printf("  %s\n", r.error().c_str());
      } else {
        std::cerr << "aadp_quote_sensitivity_report: " << c.name << ": " << r.error() << '\n';
      }
      return std::nullopt;
    }
    if (print) {
      PrintContract(r->g, r->dm);
      PrintQuoteRows(data, *r);
      r->summary.Print(data.slices.size());
    }
    out.push_back(std::move(*r));
  }
  return out;
}

int Report(const Loaded& data, const Options& o) {
  for (std::size_t j = 0; j < data.slices.size(); ++j) PrintAdjoint(j, data.slices[j]);
  for (std::size_t j = 0; j < data.slices.size(); ++j) PrintQuoteTable(j, data.slices[j]);
  const std::optional<std::vector<ContractResult>> results = ComputeContracts(data, o, true);
  if (!results) return kExitFailure;
  PrintReportLines(data, *results, o);
  return 0;
}

// The fitted parameters and parity outputs of one bumped slice, or empty.
struct Refit {
  bool usable = false;
  Vector5 theta{};
  MarketVector market{};
};

// The production refit of the bumped slice, from the same start; polished, it is then moved by
// the Newton step of its own KKT system, so that both refits are at their exact KKT points.
Refit RefitBumped(const Slice& s, std::size_t i, double h, QuoteChain chain, RefitKind kind) {
  const auto legs = s.s.legs;
  const auto bumped = aadp::svi::BumpQuote(*s.chain, s.points, legs, i, h, chain);
  if (!bumped) return {};
  const auto fit = aadp::svi::FitRawSvi(bumped->quotes);
  if (!fit || !fit->converged || fit->ActiveBounds() != s.fit.ActiveBounds()) return {};
  Vector5 theta = AsArray(fit->params);
  if (kind == RefitKind::kPolished) {
    const auto adjoint =
        aadp::svi::CalibrationAdjointOf(bumped->quotes, *fit, bumped->parity.sigma_leg);
    if (!adjoint) return {};
    for (std::size_t p = 0; p < 5; ++p) theta[p] += adjoint->newton_step[p];
  }
  return {.usable = true,
          .theta = theta,
          .market = {bumped->parity.forward, bumped->parity.discount, bumped->parity.sigma_leg}};
}

// |theta_j(alternative start) - theta_j(fit)|, the larger of two alternative grids.
Vector5 RefitError(const Slice& s) {
  Vector5 out{};
  aadp::svi::FitOptions alt1;
  alt1.grid_m = 8;
  aadp::svi::FitOptions alt2;
  alt2.grid_sigma = 8;
  for (const aadp::svi::FitOptions& alt : {alt1, alt2}) {
    const auto fit = aadp::svi::FitRawSvi(s.chain->quotes, alt);
    if (!fit) return {std::numeric_limits<double>::infinity()};
    const Vector5 a = AsArray(fit->params);
    const Vector5 b = AsArray(s.fit.params);
    for (std::size_t p = 0; p < 5; ++p) out[p] = std::max(out[p], std::abs(a[p] - b[p]));
  }
  return out;
}

struct Tally {
  std::size_t comparisons = 0;
  std::size_t passed = 0;
  std::size_t unresolved = 0;
  double worst_ratio = 0.0;  // error / tolerance

  void Add(const HalvingJudgement& j) {
    ++comparisons;
    if (!j.resolved) {
      ++unresolved;
      return;
    }
    if (j.pass) ++passed;
    worst_ratio = std::max(worst_ratio, j.error / j.tolerance);
  }
  [[nodiscard]] bool AllPass() const { return comparisons > 0 && passed == comparisons; }
};

const char* Verdict(const HalvingJudgement& e) {
  if (!e.resolved) return "UNRESOLVED";
  return e.pass ? "ok" : "FAIL";
}

// " name ok error/tolerance", and on a failure the reference and every level.
void PrintJudgement(bool print, const char* name, const HalvingJudgement& e, double reference,
                    std::span<const double> levels) {
  if (!print) return;
  std::printf(" %s %s %.2e/%.2e", name, Verdict(e), e.error, e.tolerance);
  if (e.pass) return;
  std::printf(" [reference %+.8e, level %zu, truncation %.2e, refit %.2e, levels", reference,
              e.level, e.truncation, e.refit);
  for (const double v : levels) std::printf(" %+.8e", v);
  std::printf("]");
}

void PrintTally(const char* label, const Tally& t) {
  std::printf("  %-58s %zu comparisons, %zu pass, %zu unresolved, worst error / tolerance %.4g\n",
              label, t.comparisons, t.passed, t.unresolved, t.worst_ratio);
}

aadp::svi::RawSvi<double> Params(const Vector5& t) {
  return {.a = t[0], .b = t[1], .rho = t[2], .m = t[3], .sigma = t[4]};
}

// The check's fixed inputs: the contracts, their gradients and d P / d(F, D) on the base paths.
struct CheckBase {
  std::array<aadp::svi::NamedContract, 3> contracts;
  std::vector<aadp::ad::LocalVolGradientEstimate> gradients;
  std::vector<std::vector<MarketPriceDerivative>> market_derivatives;
};

// Per slice: the refit error, the floors and the parity outputs' values.
struct SliceTolerances {
  Vector5 eps{};
  Vector5 floor{};
  MarketVector market_floor{};
  MarketVector parity_value{};
};

SliceTolerances Tolerances(const Slice& s) {
  SliceTolerances out;
  out.eps = RefitError(s);
  out.parity_value = {s.chain->parity.forward, s.chain->parity.discount, s.chain->parity.sigma_leg};
  const double kappa = s.s.adjoint.exact.condition;
  for (std::size_t i = 0; i < s.s.through_parity.size(); ++i) {
    for (std::size_t p = 0; p < 5; ++p) {
      out.floor[p] = std::max(out.floor[p], kappa * kUnitRoundoff *
                                                std::max(std::abs(s.s.adjoint.exact.quote[i][p]),
                                                         std::abs(s.s.through_parity[i][p])));
    }
    for (std::size_t p = 0; p < 3; ++p) {
      out.market_floor[p] =
          std::max(out.market_floor[p], kParityRounding * std::abs(s.s.market[i][p]));
    }
  }
  return out;
}

// One quote and chain's bumped levels.
struct QuoteLevels {
  std::array<double, kLevels> h{};
  std::array<bool, kLevels> usable{};
  std::array<Vector5, kLevels> d_theta{};
  std::array<MarketVector, kLevels> d_market{};
  std::array<std::array<double, kLevels>, 3> d_price{};
  std::size_t unusable = 0;
};

// The three prices' central differences on the two refitted surfaces of slice j; false if one
// is refused.
bool PriceLevel(const Loaded& data, const CheckBase& base, const Options& o, std::size_t j,
                const Refit& up, const Refit& down, std::size_t l, QuoteLevels& out) {
  Surface su = Base(data.surface);
  Surface sd = Base(data.surface);
  su.params[j] = Params(up.theta);
  sd.params[j] = Params(down.theta);
  su.terms[j].forward = up.market[0];
  su.terms[j].discount = up.market[1];
  sd.terms[j].forward = down.market[0];
  sd.terms[j].discount = down.market[1];
  for (std::size_t c = 0; c < base.contracts.size(); ++c) {
    const auto pu = PriceOn(base.contracts[c], su, Run(o));
    const auto pd = PriceOn(base.contracts[c], sd, Run(o));
    if (!pu || !pd) return false;
    out.d_price[c][l] = (pu->mean - pd->mean) / (2.0 * out.h[l]);
  }
  return true;
}

QuoteLevels BumpLevels(const Loaded& data, const CheckBase& base, const Options& o, std::size_t j,
                       std::size_t i, QuoteChain chain) {
  const Slice& s = data.slices[j];
  const aadp::svi::FitQuote& fq = s.chain->quotes.quotes[i];
  const double h0 = std::min(fq.noise, 0.5 * fq.price);
  QuoteLevels out;
  for (std::size_t l = 0; l < kLevels; ++l) {
    out.h[l] = std::ldexp(h0, -static_cast<int>(l));
    const Refit up = RefitBumped(s, i, out.h[l], chain, o.refit);
    const Refit down = RefitBumped(s, i, -out.h[l], chain, o.refit);
    if (!up.usable || !down.usable) {
      ++out.unusable;
      continue;
    }
    for (std::size_t p = 0; p < 5; ++p) {
      out.d_theta[l][p] = (up.theta[p] - down.theta[p]) / (2.0 * out.h[l]);
    }
    for (std::size_t p = 0; p < 3; ++p) {
      out.d_market[l][p] = (up.market[p] - down.market[p]) / (2.0 * out.h[l]);
    }
    out.usable[l] = PriceLevel(data, base, o, j, up, down, l, out);
    if (!out.usable[l]) ++out.unusable;
  }
  return out;
}

// Index: [chain][exact or GN]
struct CheckTallies {
  std::array<std::array<Tally, 2>, 2> theta{};
  Tally market;
  std::array<std::array<Tally, 2>, 2> price{};
  // The exact-Hessian price comparisons again, by contract: [chain][contract].
  std::array<std::array<Tally, 3>, 2> price_by_contract{};
  std::size_t unusable_levels = 0;
  std::size_t levels_total = 0;
};

// A price's reference from the 15-sensitivity gradient on the same paths, composed, with its
// refit error and floor.
struct PriceReference {
  double exact = 0.0;
  double gauss_newton = 0.0;
  double refit = 0.0;
  double floor = 0.0;
};

PriceReference ReferenceOf(const CheckBase& base, std::size_t c, std::size_t j, const Slice& s,
                           std::size_t i, const SliceTolerances& tol, const Vector5& exact,
                           const Vector5& gn, QuoteChain chain) {
  const aadp::ad::LocalVolGradientEstimate& g = base.gradients[c];
  PriceReference out;
  out.refit = kPriceRounding * std::abs(g.value.mean);
  double reference = 0.0;
  for (std::size_t p = 0; p < 5; ++p) {
    const double dp = g.gradient[(5 * j) + p].mean;
    reference += dp * exact[p];
    out.gauss_newton += dp * gn[p];
    out.refit += std::abs(dp) * tol.eps[p];
    out.floor += std::abs(dp) * tol.floor[p];
  }
  out.exact = reference;
  if (chain == QuoteChain::kThroughParity) {
    const MarketPriceDerivative& dm = base.market_derivatives[c][j];
    for (std::size_t w = 0; w < 2; ++w) {
      out.exact += dm.select[w].estimate * s.s.market[i][w];
      out.gauss_newton += dm.select[w].estimate * s.s.market[i][w];
      out.floor += dm.error[w] * std::abs(s.s.market[i][w]);
    }
  }
  return out;
}

// Judges, prints and tallies one quote and chain.
void JudgeQuote(const CheckBase& base, std::size_t j, const Slice& s, std::size_t i,
                const SliceTolerances& tol, QuoteChain chain, const QuoteLevels& q, CheckTallies& t,
                bool print) {
  const std::size_t ci = chain == QuoteChain::kMarketFixed ? 0 : 1;
  const bool through = chain == QuoteChain::kThroughParity;
  const Vector5& exact = through ? s.s.through_parity[i] : s.s.adjoint.exact.quote[i];
  const bool has_gn = s.s.adjoint.gauss_newton.has_value();
  Vector5 gn{};
  if (has_gn) {
    gn = through ? s.s.through_parity_gauss_newton[i] : s.s.adjoint.gauss_newton->quote[i];
  }
  if (print) std::printf("  quote %s %2zu %-14s", Expiry(j), i, aadp::svi::QuoteChainName(chain));
  for (std::size_t p = 0; p < 5; ++p) {
    std::array<double, kLevels> col{};
    for (std::size_t l = 0; l < kLevels; ++l) col[l] = q.d_theta[l][p];
    const HalvingJudgement e =
        aadp::svi::JudgeByHalving(q.h, col, q.usable, tol.eps[p], exact[p], tol.floor[p]);
    t.theta[ci][0].Add(e);
    if (has_gn) {
      t.theta[ci][1].Add(
          aadp::svi::JudgeByHalving(q.h, col, q.usable, tol.eps[p], gn[p], tol.floor[p]));
    }
    PrintJudgement(print, kParameterNames[p], e, exact[p], col);
  }
  if (through) {
    for (std::size_t p = 0; p < 3; ++p) {
      std::array<double, kLevels> col{};
      for (std::size_t l = 0; l < kLevels; ++l) col[l] = q.d_market[l][p];
      const HalvingJudgement e = aadp::svi::JudgeByHalving(
          q.h, col, q.usable, kPriceRounding * std::abs(tol.parity_value[p]), s.s.market[i][p],
          tol.market_floor[p]);
      t.market.Add(e);
      PrintJudgement(print, kMarketNames[p], e, s.s.market[i][p], col);
    }
  }
  constexpr std::array<const char*, 3> kPriceNames = {"P0", "P1", "P2"};
  for (std::size_t c = 0; c < base.contracts.size(); ++c) {
    const PriceReference r = ReferenceOf(base, c, j, s, i, tol, exact, gn, chain);
    const HalvingJudgement e =
        aadp::svi::JudgeByHalving(q.h, q.d_price[c], q.usable, r.refit, r.exact, r.floor);
    t.price[ci][0].Add(e);
    t.price_by_contract[ci][c].Add(e);
    if (has_gn) {
      t.price[ci][1].Add(
          aadp::svi::JudgeByHalving(q.h, q.d_price[c], q.usable, r.refit, r.gauss_newton, r.floor));
    }
    PrintJudgement(print, kPriceNames[c], e, r.exact, q.d_price[c]);
  }
  if (!print) return;
  std::printf("\n");
  std::fflush(stdout);
}

std::optional<CheckBase> PrepareCheck(const Loaded& data, const Options& o, bool print) {
  const double maturity = data.surface.terms.back().expiry;
  CheckBase base{.contracts = aadp::svi::SurfaceContracts(maturity),
                 .gradients = {},
                 .market_derivatives = {}};
  for (const aadp::svi::NamedContract& c : base.contracts) {
    const auto g = aadp::ad::PriceLocalVolWithGradient(c.contract, data.surface.terms,
                                                       data.surface.params, Run(o));
    if (!g) return std::nullopt;
    const auto dm = MarketDerivatives(c, data.surface, Run(o), g->value.mean);
    if (!dm) return std::nullopt;
    if (print) {
      std::printf("  %-34s price %.6f, base for the price-level check\n", c.name, g->value.mean);
    }
    base.gradients.push_back(*g);
    base.market_derivatives.push_back(*dm);
  }
  return base;
}

// The registered rule: every exact-Hessian comparison passes, none unresolved.
bool Passes(const CheckTallies& t) {
  return t.theta[0][0].AllPass() && t.theta[1][0].AllPass() && t.market.AllPass() &&
         t.price[0][0].AllPass() && t.price[1][0].AllPass();
}

// The tallies in the summary's order: key, label, role and the tally.
struct NamedTally {
  const char* key;
  const char* label;
  const char* role;
  const Tally* tally;
};

NamedTally Named(const char* key, const char* label, const char* role, const Tally& tally) {
  return {.key = key, .label = label, .role = role, .tally = std::addressof(tally)};
}

std::array<NamedTally, 9> NamedTallies(const CheckTallies& t) {
  return {
      Named("parameters.market-fixed.exact", "parameters, market fixed, exact Hessian", "required",
            t.theta[0][0]),
      Named("parameters.through-parity.exact", "parameters, through parity, exact Hessian",
            "required", t.theta[1][0]),
      Named("parity.through-parity.exact", "F, D, sigma_leg through parity", "required", t.market),
      Named("prices.market-fixed.exact", "prices, market fixed, exact Hessian", "required",
            t.price[0][0]),
      Named("prices.through-parity.exact", "prices, through parity, exact Hessian", "required",
            t.price[1][0]),
      Named("parameters.market-fixed.gauss-newton",
            "parameters, market fixed, Gauss-Newton (disclosure)", "disclosure", t.theta[0][1]),
      Named("parameters.through-parity.gauss-newton",
            "parameters, through parity, Gauss-Newton (disclosure)", "disclosure", t.theta[1][1]),
      Named("prices.market-fixed.gauss-newton", "prices, market fixed, Gauss-Newton (disclosure)",
            "disclosure", t.price[0][1]),
      Named("prices.through-parity.gauss-newton",
            "prices, through parity, Gauss-Newton (disclosure)", "disclosure", t.price[1][1])};
}

// The chains' keys, in the tallies' order.
constexpr std::array<QuoteChain, 2> kChains = {QuoteChain::kMarketFixed,
                                               QuoteChain::kThroughParity};

bool PrintCheckSummary(const CheckTallies& t, std::span<const aadp::svi::NamedContract> contracts) {
  std::printf("\n== check summary ==\n");
  std::printf(
      "  bumped levels %zu, unusable (a fit failed, did not converge or changed its active "
      "set) %zu\n",
      t.levels_total, t.unusable_levels);
  for (const NamedTally& n : NamedTallies(t)) PrintTally(n.label, *n.tally);
  std::printf("  the exact-Hessian price comparisons by contract:\n");
  for (std::size_t ci = 0; ci < kChains.size(); ++ci) {
    for (std::size_t c = 0; c < contracts.size(); ++c) {
      const std::string label =
          std::string(aadp::svi::QuoteChainName(kChains[ci])) + ", " + contracts[c].name;
      PrintTally(label.c_str(), t.price_by_contract[ci][c]);
    }
  }
  const bool pass = Passes(t);
  std::printf("  verdict: %s\n", pass ? "PASS" : "FAIL");
  return pass;
}

// What the check found: its tallies, each slice's refit error, and the contracts' names.
struct CheckResult {
  CheckTallies t;
  std::vector<Vector5> refit_error;
  std::array<aadp::svi::NamedContract, 3> contracts;
};

// The check, printing every quote's judgements when asked; nothing if the base is refused.
std::optional<CheckResult> RunCheck(const Loaded& data, const Options& o, bool print) {
  if (print) {
    std::printf(
        "\n== bump-and-recalibrate check: levels h_l = 2^-l min(noise_i, V_i / 2), l = 0..5; "
        "every %u%s quote; refits %s ==\n",
        o.stride,
        o.stride == 1 ? "st (all, as registered)" : "th (short run, not the registered check)",
        o.refit == RefitKind::kPolished
            ? "polished by one KKT Newton step (decided after the pre-registration)"
            : "as the production fit stopped (as pre-registered)");
  }
  const std::optional<CheckBase> base = PrepareCheck(data, o, print);
  if (!base) {
    if (!print) std::cerr << "aadp_quote_sensitivity_report: the check's base was refused\n";
    return std::nullopt;
  }
  CheckResult out{.t = {}, .refit_error = {}, .contracts = base->contracts};
  CheckTallies& t = out.t;
  for (std::size_t j = 0; j < data.slices.size(); ++j) {
    const Slice& s = data.slices[j];
    const SliceTolerances tol = Tolerances(s);
    out.refit_error.push_back(tol.eps);
    if (print) {
      std::printf(
          "\n-- slice %s: refit error (alternative grids) a %.3e b %.3e rho %.3e m %.3e "
          "sigma %.3e --\n",
          Expiry(j), tol.eps[0], tol.eps[1], tol.eps[2], tol.eps[3], tol.eps[4]);
    }
    for (std::size_t i = 0; i < s.s.through_parity.size(); i += o.stride) {
      for (const QuoteChain chain : kChains) {
        const QuoteLevels q = BumpLevels(data, *base, o, j, i, chain);
        t.levels_total += kLevels;
        t.unusable_levels += q.unusable;
        JudgeQuote(*base, j, s, i, tol, chain, q, t, print);
      }
    }
  }
  return out;
}

const char* RefitName(RefitKind kind) {
  return kind == RefitKind::kPolished ? "polished" : "production";
}

// The required tallies summed: comparisons, and those that did not pass (unresolved included).
struct Required {
  std::size_t comparisons = 0;
  std::size_t failed = 0;
  std::size_t price_comparisons = 0;
  std::size_t price_failed = 0;
};

Required RequiredCounts(const CheckTallies& t) {
  Required out;
  for (const NamedTally& n : NamedTallies(t)) {
    if (std::string_view(n.role) != "required") continue;
    out.comparisons += n.tally->comparisons;
    out.failed += n.tally->comparisons - n.tally->passed;
  }
  for (std::size_t ci = 0; ci < kChains.size(); ++ci) {
    out.price_comparisons += t.price[ci][0].comparisons;
    out.price_failed += t.price[ci][0].comparisons - t.price[ci][0].passed;
  }
  return out;
}

// The check's figures at full precision, one line each:
//   check,stride,refit,levels,unusable
//   refit_error,expiry,a,b,rho,m,sigma
//   tally,key,role,comparisons,passed,unresolved,failed,worst
//   contract_tally,contract,chain,comparisons,passed,unresolved,failed,worst
//   verdict,PASS|FAIL,comparisons,failed,price_comparisons,price_failed
void PrintCheckLines(const CheckResult& r, const Options& o) {
  const CheckTallies& t = r.t;
  std::printf("\n== machine-readable, full precision ==\n");
  std::printf("check,%u,%s,%zu,%zu\n", o.stride, RefitName(o.refit), t.levels_total,
              t.unusable_levels);
  for (std::size_t j = 0; j < r.refit_error.size(); ++j) {
    std::printf("refit_error,%s", Expiry(j));
    for (const double v : r.refit_error[j]) std::printf(",%.17g", v);
    std::printf("\n");
  }
  for (const NamedTally& n : NamedTallies(t)) {
    std::printf("tally,%s,%s,%zu,%zu,%zu,%zu,%.17g\n", n.key, n.role, n.tally->comparisons,
                n.tally->passed, n.tally->unresolved, n.tally->comparisons - n.tally->passed,
                n.tally->worst_ratio);
  }
  for (std::size_t ci = 0; ci < kChains.size(); ++ci) {
    for (std::size_t c = 0; c < r.contracts.size(); ++c) {
      const Tally& n = t.price_by_contract[ci][c];
      std::printf("contract_tally,%s,%s,%zu,%zu,%zu,%zu,%.17g\n", r.contracts[c].name,
                  aadp::svi::QuoteChainName(kChains[ci]), n.comparisons, n.passed, n.unresolved,
                  n.comparisons - n.passed, n.worst_ratio);
    }
  }
  const Required req = RequiredCounts(t);
  std::printf("verdict,%s,%zu,%zu,%zu,%zu\n", Passes(t) ? "PASS" : "FAIL", req.comparisons,
              req.failed, req.price_comparisons, req.price_failed);
}

int Check(const Loaded& data, const Options& o) {
  const std::optional<CheckResult> r = RunCheck(data, o, true);
  if (!r) return kExitFailure;
  const bool pass = PrintCheckSummary(r->t, r->contracts);
  PrintCheckLines(*r, o);
  return pass ? 0 : kExitFailure;
}

double Median(std::vector<double> v) {
  std::ranges::sort(v);
  const std::size_t n = v.size();
  return n % 2 == 1 ? v[n / 2] : 0.5 * (v[(n / 2) - 1] + v[n / 2]);
}

double Milliseconds(std::chrono::steady_clock::duration d) {
  return std::chrono::duration<double, std::milli>(d).count();
}

// The implicit-function step from the fitted slices, and the composition; empty if refused.
std::optional<double> ImplicitStep(const Loaded& data,
                                   std::span<const aadp::taifex::OptionRow> options,
                                   const aadp::ad::LocalVolGradientEstimate& g) {
  double composed = 0.0;
  for (std::size_t j = 0; j < data.slices.size(); ++j) {
    const Slice& s = data.slices[j];
    const auto points = aadp::settlement_parity_points(options, s.chain->expiry);
    const auto q = aadp::svi::SliceQuoteSensitivities(*s.chain, points, s.fit);
    if (!q) return std::nullopt;
    for (std::size_t i = 0; i < q->through_parity.size(); ++i) {
      for (std::size_t p = 0; p < 5; ++p) {
        composed +=
            g.gradient[(5 * j) + p].mean * (q->adjoint.exact.quote[i][p] + q->through_parity[i][p]);
      }
    }
  }
  return composed;
}

// Through parity also needs d P / d(F_j, D_j): one central difference each.
std::optional<double> MarketStep(const Loaded& data, const aadp::svi::NamedContract& c,
                                 const Options& o) {
  double composed = 0.0;
  for (std::size_t j = 0; j < data.surface.terms.size(); ++j) {
    for (std::size_t w = 0; w < 2; ++w) {
      Surface up = Base(data.surface);
      Surface down = Base(data.surface);
      const double h = std::ldexp(kMarketStep * MarketInputOf(up, j, w), -2);
      MarketInputOf(up, j, w) += h;
      MarketInputOf(down, j, w) -= h;
      const auto pu = PriceOn(c, up, Run(o));
      const auto pd = PriceOn(c, down, Run(o));
      if (!pu || !pd) return std::nullopt;
      composed += (pu->mean - pd->mean) / (2.0 * h);
    }
  }
  return composed;
}

// One contract's times over the repetitions: the price with its 15 sensitivities, the
// implicit-function step and composition on top, and through parity d P / d(F, D).
struct ContractTimes {
  const char* name = "";
  std::vector<double> grad_ms;
  std::vector<double> ift_ms;
  std::vector<double> market_ms;
  double sink = 0.0;
};

// Every contract's times; nothing once one is refused.
std::optional<std::vector<ContractTimes>> TimeContracts(
    const Loaded& data, const Options& o, std::span<const aadp::taifex::OptionRow> options) {
  using Clock = std::chrono::steady_clock;
  std::vector<ContractTimes> out;
  const double maturity = data.surface.terms.back().expiry;
  for (const aadp::svi::NamedContract& c : aadp::svi::SurfaceContracts(maturity)) {
    ContractTimes times;
    times.name = c.name;
    for (std::uint32_t rep = 0; rep < o.repetitions; ++rep) {
      const auto t0 = Clock::now();
      const auto g = aadp::ad::PriceLocalVolWithGradient(c.contract, data.surface.terms,
                                                         data.surface.params, Run(o));
      const auto t1 = Clock::now();
      if (!g) return std::nullopt;
      const std::optional<double> ift = ImplicitStep(data, options, *g);
      const auto t2 = Clock::now();
      const std::optional<double> market = MarketStep(data, c, o);
      const auto t3 = Clock::now();
      if (!ift || !market) return std::nullopt;
      times.sink += *ift + *market + g->value.mean;
      times.grad_ms.push_back(Milliseconds(t1 - t0));
      times.ift_ms.push_back(Milliseconds(t2 - t1));
      times.market_ms.push_back(Milliseconds(t3 - t2));
    }
    out.push_back(std::move(times));
  }
  return out;
}

int Timing(const Loaded& data, const Options& o, std::span<const aadp::taifex::OptionRow> options) {
  std::printf("\n== timing: ITERATION ONLY, not a result ==\n");
  const std::optional<std::vector<ContractTimes>> all = TimeContracts(data, o, options);
  if (!all) return kExitFailure;
  for (const ContractTimes& t : *all) {
    const double grad = Median(t.grad_ms);
    const double ift = Median(t.ift_ms);
    const double market = Median(t.market_ms);
    std::printf(
        "  %-34s price + 15 sensitivities median %.2f ms; implicit-function step and composition "
        "for 180 quotes median %.3f ms (ratio %.4f); through parity adds d P / d(F, D) by 12 "
        "prices, median %.2f ms (ratio %.4f); over %u repetitions (check %.6g)\n",
        t.name, grad, ift, (grad + ift) / grad, market, (grad + ift + market) / grad, o.repetitions,
        t.sink / o.repetitions);
  }
  return 0;
}

#if AADP_REPORT_MODE
using aadp::results::CellKind;
using aadp::results::integer_cell;
using aadp::results::real_cell;
using aadp::results::ReportColumn;
using aadp::results::ReportTable;

std::string CountCell(std::size_t n) { return integer_cell(static_cast<std::int64_t>(n)); }

// Every table is SVI-10's and derived from the chain.
ReportTable Table(std::string name, std::string title, bool timing,
                  std::vector<ReportColumn> columns) {
  return {.name = std::move(name),
          .title = std::move(title),
          .targets = {"SVI-10"},
          .timing = timing,
          .uses_chain = true,
          .columns = std::move(columns),
          .rows = {}};
}

ReportColumn RealColumn(std::string name, std::string unit = "none") {
  return {.name = std::move(name), .kind = CellKind::kReal, .unit = std::move(unit)};
}

ReportColumn CountColumn(std::string name) {
  return {.name = std::move(name), .kind = CellKind::kInteger, .unit = "count"};
}

ReportColumn TextColumn(std::string name) {
  return {.name = std::move(name), .kind = CellKind::kText, .unit = "none"};
}

// A table has rows or is not written: no slice without Gauss-Newton refused has its table.
std::vector<ReportTable> Written(std::vector<ReportTable> tables) {
  std::erase_if(tables, [](const ReportTable& t) { return t.rows.empty(); });
  return tables;
}

// Per slice: the KKT solution's figures, Gauss-Newton beside the exact Hessian, and the
// multipliers.
std::vector<ReportTable> SliceTables(const Loaded& data) {
  std::vector<ReportColumn> slice_columns = {
      CountColumn("quotes"),           RealColumn("objective"),
      CountColumn("on_tick_boundary"), CountColumn("active_bounds"),
      RealColumn("stationarity"),      RealColumn("gradient_norm"),
      RealColumn("hessian_asymmetry"), RealColumn("hessian_gn_difference"),
      RealColumn("condition"),         RealColumn("manifold_residual")};
  for (const char* p : kParameterNames) {
    slice_columns.push_back(RealColumn(std::string("newton_") + p));
  }
  slice_columns.push_back(TextColumn("gauss_newton"));
  ReportTable slices =
      Table("qs_slices", "Per slice, the KKT system of the fit, exact Hessian, and the Newton step",
            false, std::move(slice_columns));
  std::vector<ReportColumn> gn_columns = {RealColumn("condition"), RealColumn("manifold_residual")};
  for (const char* p : kParameterNames) {
    gn_columns.push_back(RealColumn(std::string("against_exact_") + p));
  }
  ReportTable gauss_newton = Table(
      "qs_gauss_newton",
      "Per slice, Gauss-Newton: its KKT condition and d theta / d V against the exact Hessian's",
      false, std::move(gn_columns));
  ReportTable multipliers =
      Table("qs_multipliers",
            "Per slice and active bound, the multiplier and how far it is from zero", false,
            {RealColumn("newton"), RealColumn("least_squares"), RealColumn("second_step"),
             RealColumn("margin"), RealColumn("significance"),
             RealColumn("quote_distance", "noise_units")});
  for (std::size_t j = 0; j < data.slices.size(); ++j) {
    const Slice& s = data.slices[j];
    const aadp::svi::CalibrationAdjoint& a = s.s.adjoint;
    std::vector<std::string> cells = {CountCell(s.fit.quotes),
                                      real_cell(s.fit.objective),
                                      CountCell(OnTickBoundary(s)),
                                      CountCell(a.constraints.size()),
                                      real_cell(a.stationarity),
                                      real_cell(a.gradient_norm),
                                      real_cell(a.derivatives.hessian_asymmetry),
                                      real_cell(HessianDifference(a)),
                                      real_cell(a.exact.condition),
                                      real_cell(a.exact.manifold_residual)};
    for (const double v : a.newton_step) cells.push_back(real_cell(v));
    cells.emplace_back(a.gauss_newton ? "ok"
                                      : aadp::svi::AdjointErrorName(a.gauss_newton.error().code));
    slices.rows.push_back({.key = Expiry(j), .cells = std::move(cells)});
    if (a.gauss_newton) {
      std::vector<std::string> gn = {real_cell(a.gauss_newton->condition),
                                     real_cell(a.gauss_newton->manifold_residual)};
      for (const double v : GaussNewtonAgainstExact(a)) gn.push_back(real_cell(v));
      gauss_newton.rows.push_back({.key = Expiry(j), .cells = std::move(gn)});
    }
    for (const aadp::svi::ActiveConstraint& c : a.constraints) {
      multipliers.rows.push_back(
          {.key = std::string(Expiry(j)) + "." + BoundKey(c.bound),
           .cells = {real_cell(c.multiplier), real_cell(c.multiplier_least_squares),
                     real_cell(c.multiplier_second_step), real_cell(c.margin),
                     real_cell(c.significance), real_cell(c.quote_distance)}});
    }
  }
  return {std::move(slices), std::move(gauss_newton), std::move(multipliers)};
}

// d P / d F_j, then d P / d D_j: each with its SE, the level the halving rule selected, its
// truncation and its rounding.
std::vector<std::string> MarketCells(const MarketPriceDerivative& dm) {
  std::vector<std::string> cells;
  cells.reserve(10);
  for (std::size_t w = 0; w < 2; ++w) {
    const aadp::BatchStatistics st = aadp::BatchMeansStatistics(dm.batch[w]);
    cells.push_back(real_cell(st.mean));
    cells.push_back(real_cell(st.standard_error));
    cells.push_back(CountCell(dm.select[w].level));
    cells.push_back(real_cell(dm.select[w].truncation));
    cells.push_back(real_cell(dm.select[w].refit));
  }
  return cells;
}

// One contract's quote sensitivities by version, expiry and bucket, and over every quote.
void AddSummaryRows(const ContractResult& r, std::size_t slices, ReportTable& buckets,
                    ReportTable& totals) {
  for (std::size_t version = 0; version < kVersionCount; ++version) {
    const std::string prefix = std::string(r.name) + "." + kVersionKeys[version] + ".";
    for (std::size_t j = 0; j < slices; ++j) {
      for (std::size_t b = 0; b < kBucketCount; ++b) {
        const Summary::Sums& bucket = r.summary.At(version, j, b);
        if (bucket.n == 0) continue;
        const aadp::BatchStatistics st = aadp::BatchMeansStatistics(bucket.batch);
        buckets.rows.push_back(
            {.key = prefix + Expiry(j) + ".b" + std::to_string(b),
             .cells = {CountCell(bucket.n), kBucketFrom[b], kBucketTo[b], real_cell(st.mean),
                       real_cell(st.standard_error), real_cell(bucket.max_abs)}});
      }
    }
    const std::size_t counted = r.summary.Counted(version, slices);
    if (counted == 0) continue;
    const aadp::BatchStatistics st = r.summary.Total(version);
    totals.rows.push_back(
        {.key = std::string(r.name) + "." + kVersionKeys[version],
         .cells = {CountCell(counted), real_cell(st.mean), real_cell(st.standard_error)}});
  }
}

// Per contract: the price, d P / d(F_j, D_j), and the quote sensitivities by expiry and bucket
// and over all quotes.
std::vector<ReportTable> ContractTables(std::span<const ContractResult> results, std::size_t slices,
                                        const Options& o) {
  ReportTable prices = Table("qs_price", "Per contract, the price the sensitivities are of", false,
                             {CountColumn("paths"), RealColumn("price", "index_points"),
                              RealColumn("price_se", "index_points")});
  std::vector<ReportColumn> market_columns;
  // d P / d F_j, then d P / d D_j.
  for (const char* w : {"dp_df", "dp_dd"}) {
    const std::string unit =
        std::string_view(w) == "dp_df" ? "index_points/index_point" : "index_points";
    const std::string name = w;
    market_columns.push_back(RealColumn(name, unit));
    market_columns.push_back(RealColumn(name + "_se", unit));
    market_columns.push_back(CountColumn(name + "_level"));
    market_columns.push_back(RealColumn(name + "_truncation", unit));
    market_columns.push_back(RealColumn(name + "_rounding", "index_points"));
  }
  ReportTable market =
      Table("qs_market_derivatives",
            "Per contract and expiry, d P / d(F, D) by common-random-number differences", false,
            std::move(market_columns));
  ReportTable buckets = Table(
      "qs_quote_buckets",
      "Quote sensitivities d P / d V summed by expiry and k = ln(K / F) bucket, three versions",
      false,
      {CountColumn("quotes"), TextColumn("k_from"), TextColumn("k_to"),
       RealColumn("sum", "index_points/index_point"),
       RealColumn("sum_se", "index_points/index_point"),
       RealColumn("max_abs", "index_points/index_point")});
  ReportTable totals =
      Table("qs_quote_totals",
            "Quote sensitivities summed over every quote: every settlement up one point", false,
            {CountColumn("quotes"), RealColumn("sum", "index_points/index_point"),
             RealColumn("sum_se", "index_points/index_point")});
  for (const ContractResult& r : results) {
    const std::string name = r.name;
    prices.rows.push_back({.key = name,
                           .cells = {CountCell(o.paths), real_cell(r.g.value.mean),
                                     real_cell(r.g.value.standard_error)}});
    for (std::size_t j = 0; j < r.dm.size(); ++j) {
      market.rows.push_back({.key = name + "." + Expiry(j), .cells = MarketCells(r.dm[j])});
    }
    AddSummaryRows(r, slices, buckets, totals);
  }
  return {std::move(prices), std::move(market), std::move(buckets), std::move(totals)};
}

// Stage 7b's report run, summarised (SVI-10).
std::vector<ReportTable> ReportTables(const Loaded& data, std::span<const ContractResult> results,
                                      const Options& o) {
  std::vector<ReportTable> tables = SliceTables(data);
  for (ReportTable& table : ContractTables(results, data.slices.size(), o)) {
    tables.push_back(std::move(table));
  }
  return Written(std::move(tables));
}

// The bump-and-recalibrate check, summarised: its run, each slice's refit error, every tally of
// the text's summary, the exact-Hessian price comparisons by contract, and the verdict.
std::vector<ReportTable> CheckTables(const CheckResult& r, const Options& o) {
  const CheckTallies& t = r.t;
  ReportTable run =
      Table("qs_check_run", "The check's run: quotes, refits and bumped levels", false,
            {CountColumn("stride"), TextColumn("refit"), CountColumn("levels"),
             CountColumn("unusable_levels")});
  run.rows.push_back({.key = "check",
                      .cells = {CountCell(o.stride), RefitName(o.refit), CountCell(t.levels_total),
                                CountCell(t.unusable_levels)}});
  std::vector<ReportColumn> refit_columns;
  refit_columns.reserve(kParameterNames.size());
  for (const char* p : kParameterNames) refit_columns.push_back(RealColumn(p));
  ReportTable refit =
      Table("qs_check_refit_error", "Per slice, the refit error over two alternative start grids",
            false, std::move(refit_columns));
  for (std::size_t j = 0; j < r.refit_error.size(); ++j) {
    std::vector<std::string> cells;
    for (const double v : r.refit_error[j]) cells.push_back(real_cell(v));
    refit.rows.push_back({.key = Expiry(j), .cells = std::move(cells)});
  }
  const std::vector<ReportColumn> tally_columns = {
      CountColumn("comparisons"), CountColumn("passed"), CountColumn("unresolved"),
      CountColumn("failed"), RealColumn("worst_ratio")};
  std::vector<ReportColumn> with_role = {TextColumn("role")};
  with_role.insert(with_role.end(), tally_columns.begin(), tally_columns.end());
  ReportTable tallies =
      Table("qs_check_tallies",
            "The check's comparisons; failed counts those not passed, unresolved included", false,
            std::move(with_role));
  const auto tally_cells = [](const Tally& n) {
    return std::vector<std::string>{CountCell(n.comparisons), CountCell(n.passed),
                                    CountCell(n.unresolved), CountCell(n.comparisons - n.passed),
                                    real_cell(n.worst_ratio)};
  };
  for (const NamedTally& n : NamedTallies(t)) {
    std::vector<std::string> cells = {n.role};
    const std::vector<std::string> rest = tally_cells(*n.tally);
    cells.insert(cells.end(), rest.begin(), rest.end());
    tallies.rows.push_back({.key = n.key, .cells = std::move(cells)});
  }
  ReportTable by_contract =
      Table("qs_check_price_failures",
            "The exact-Hessian price comparisons by contract and chain, with those failed", false,
            tally_columns);
  for (std::size_t ci = 0; ci < kChains.size(); ++ci) {
    for (std::size_t c = 0; c < r.contracts.size(); ++c) {
      by_contract.rows.push_back(
          {.key = std::string(r.contracts[c].name) + "." + aadp::svi::QuoteChainName(kChains[ci]),
           .cells = tally_cells(t.price_by_contract[ci][c])});
    }
  }
  const Required req = RequiredCounts(t);
  ReportTable verdict =
      Table("qs_check_verdict",
            "The registered verdict: PASS only if every exact-Hessian comparison passes", false,
            {TextColumn("verdict"), CountColumn("comparisons"), CountColumn("failed"),
             CountColumn("price_comparisons"), CountColumn("price_failed")});
  verdict.rows.push_back(
      {.key = "verdict",
       .cells = {Passes(t) ? "PASS" : "FAIL", CountCell(req.comparisons), CountCell(req.failed),
                 CountCell(req.price_comparisons), CountCell(req.price_failed)}});
  return {std::move(run), std::move(refit), std::move(tallies), std::move(by_contract),
          std::move(verdict)};
}

// SVI-10's cost: per contract the price with its 15 sensitivities, the implicit-function step
// and the composition on top, and through parity the CRN differences, all of them timings.
std::vector<ReportTable> TimingTables(std::span<const ContractTimes> all,
                                      std::uint32_t repetitions) {
  ReportTable timing = Table(
      "qs_time",
      "Per contract, the price with 15 sensitivities, then the quote sensitivities on top", true,
      {RealColumn("gradient_median", "ms"), RealColumn("implicit_median", "ms"),
       RealColumn("implicit_ratio"), RealColumn("parity_median", "ms"), RealColumn("parity_ratio"),
       CountColumn("repetitions")});
  for (const ContractTimes& t : all) {
    const double grad = Median(t.grad_ms);
    const double ift = Median(t.ift_ms);
    const double market = Median(t.market_ms);
    timing.rows.push_back(
        {.key = t.name,
         .cells = {real_cell(grad), real_cell(ift), real_cell((grad + ift) / grad),
                   real_cell(market), real_cell((grad + ift + market) / grad),
                   CountCell(repetitions)}});
  }
  return {std::move(timing)};
}

const char* ModeName(Mode mode) {
  switch (mode) {
    case Mode::kReport:
      return "report";
    case Mode::kCheck:
      return "check";
    case Mode::kTiming:
      return "timing";
  }
  return "unknown";
}

int ReportMain(const Loaded& data, const Options& o,
               std::span<const aadp::taifex::OptionRow> options,
               aadp::results::ReportRun& session) {
  std::vector<ReportTable> tables;
  std::vector<aadp::results::HeaderField> header = {
      {.key = "report_tool",
       .value = std::string("aadp_quote_sensitivity_report --report --mode ") + ModeName(o.mode)},
      {.key = "report_inputs",
       .value = "TAIFEX TXO and TX daily settlement prices of trade date 2024-11-15, not "
                "tradable quotes; the raw SVI fit of expiries 202411, 202411W4 and 202412 and "
                "its 180 calibration settlements, so maturity reaches 33 days only"}};
  const std::string run = "seed " + std::to_string(kSeed) + ", paths " + std::to_string(o.paths) +
                          ", batches " + std::to_string(kBatches) + ", threads " +
                          std::to_string(o.threads) + ", 33 dates, n 4";
  std::int64_t repetitions = 1;
  if (o.mode == Mode::kReport) {
    const std::optional<std::vector<ContractResult>> results = ComputeContracts(data, o, false);
    if (!results) return kExitFailure;
    tables = ReportTables(data, *results, o);
    header.push_back({.key = "report_run", .value = run});
  } else if (o.mode == Mode::kCheck) {
    const std::optional<CheckResult> r = RunCheck(data, o, false);
    if (!r) return kExitFailure;
    tables = CheckTables(*r, o);
    header.push_back(
        {.key = "report_run",
         .value = run + ", stride " + std::to_string(o.stride) + ", refits " + RefitName(o.refit)});
  } else {
    const std::optional<std::vector<ContractTimes>> all = TimeContracts(data, o, options);
    if (!all) return kExitFailure;
    tables = TimingTables(*all, o.repetitions);
    repetitions = o.repetitions;
    header.push_back(
        {.key = "report_timing",
         .value = "per contract, the price with its 15 sensitivities by tape, then the "
                  "implicit-function step and composition for every quote, then through parity "
                  "d P / d(F, D) by 12 prices, steady_clock, " +
                  run + ", " + std::to_string(o.repetitions) + " repetitions"});
  }
  const aadp::results::ReportShape shape{.run = {.claim = "result",
                                                 .repetitions = repetitions,
                                                 .batches_per_repetition = 1,
                                                 .warmup = "none"},
                                         .header = std::move(header)};
  auto text = session.finish(shape, tables, aadp::attribution_text());
  if (!text) {
    std::cerr << "aadp_quote_sensitivity_report: " << text.error().message << '\n';
    return text.error().exit_code;
  }
  std::cout << *text;
  std::cout.flush();
  return std::cout ? 0 : kExitFailure;
}
#endif

}  // namespace

// NOLINTNEXTLINE(misc-const-correctness): the standard fixes the signature of main.
int main(int argc, char** argv) try {
  const std::optional<Options> parsed = Parse(argc, argv);
  if (!parsed) return Usage();
  const Options& o = *parsed;
#if AADP_REPORT_MODE
  // The run's refusals come before any work, as a benchmark binary's do.
  std::optional<aadp::results::ReportRun> session;
  if (o.report) {
    auto begun = aadp::results::ReportRun::begin(o.run);
    if (!begun) {
      std::cerr << "aadp_quote_sensitivity_report: " << begun.error().message << '\n';
      return begun.error().exit_code;
    }
    session.emplace(std::move(*begun));
  }
#else
  if (o.report) {
    std::cerr << "aadp_quote_sensitivity_report: built without the report mode\n";
    return kExitUsage;
  }
#endif

  // The attribution comes first, before any output that the chain could have shaped. A report
  // file carries it as its first section instead.
  if (!o.report) Header(o);
  const auto option_bytes = aadp::taifex::read_binary_file(o.options_path);
  const auto future_bytes = aadp::taifex::read_binary_file(o.futures_path);
  if (!option_bytes || !future_bytes) {
    std::cerr << "aadp_quote_sensitivity_report: "
              << (option_bytes ? future_bytes.error() : option_bytes.error())
              << ". Run scripts/fetch_data.sh.\n";
    return kExitFailure;
  }
  const auto options = aadp::taifex::read_options(*option_bytes);
  const auto futures = aadp::taifex::read_futures(*future_bytes);
  if (!options || !futures) {
    std::cerr << "aadp_quote_sensitivity_report: "
              << aadp::taifex::to_string(options ? futures.error().code : options.error().code)
              << '\n';
    return kExitFailure;
  }
  const std::optional<Loaded> data = Load(o, options->rows, futures->rows);
  if (!data) return kExitFailure;
#if AADP_REPORT_MODE
  if (o.report) return ReportMain(*data, o, options->rows, *session);
#endif
  switch (o.mode) {
    case Mode::kReport:
      return Report(*data, o);
    case Mode::kCheck:
      return Check(*data, o);
    case Mode::kTiming:
      return Timing(*data, o, options->rows);
  }
  return kExitFailure;
} catch (const std::exception& error) {
  std::fprintf(stderr, "aadp_quote_sensitivity_report: %s\n", error.what());
  return kExitFailure;
}
