// Market-quote sensitivities through the raw SVI fit to the TAIFEX captures, by the implicit
// function theorem at the fit's constrained optimum, and their bump-and-recalibrate check
// (docs/NOTES.md, "Stage 7b"). Every number it prints is iteration only.
//
//   aadp_quote_sensitivity_report [--options <file>] [--futures <file>]
//                                 [--mode report|check|timing] [--paths <N>] [--threads <T>]
//                                 [--repetitions <R>] [--stride <S>]
//                                 [--refit polished|production]
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
#include <expected>
#include <iostream>
#include <limits>
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
};

int Usage() {
  std::cerr << "usage: aadp_quote_sensitivity_report [--options <file>] [--futures <file>] "
               "[--mode report|check|timing] [--paths <N, a multiple of 64>] [--threads <T>] "
               "[--repetitions <R>] [--stride <S>] [--refit polished|production]\n";
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

std::optional<Options> Parse(std::span<char*> args) {
  Options o;
  for (std::size_t i = 1; i < args.size(); i += 2) {
    if (i + 1 == args.size() || !Apply(o, args[i], args[i + 1])) return std::nullopt;
  }
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

void PrintAdjoint(std::size_t j, const Slice& s) {
  const aadp::svi::CalibrationAdjoint& a = s.s.adjoint;
  const aadp::svi::FitResult& f = s.fit;
  std::printf("\n== slice %s: %zu quotes, F %.6f, D %.8f, sigma_leg %.6f ==\n", Expiry(j), f.quotes,
              s.chain->parity.forward, s.chain->parity.discount, s.chain->parity.sigma_leg);
  std::printf("  theta              a %.12g  b %.12g  rho %.12g  m %.12g  sigma %.12g\n",
              f.params.a, f.params.b, f.params.rho, f.params.m, f.params.sigma);
  std::printf("  Phi                %.10g (fit), %.10g (tape's Black-76), converged %s\n",
              f.objective, a.derivatives.objective, f.converged ? "yes" : "no");
  // The tick is piecewise constant in V_i, so its derivative is 0 except at a boundary, where a
  // settlement has one-sided derivatives only.
  std::size_t on_boundary = 0;
  for (const aadp::svi::FitQuote& q : s.chain->quotes.quotes) {
    on_boundary += static_cast<std::size_t>(q.price == 10.0 || q.price == 50.0 ||
                                            q.price == 500.0 || q.price == 1000.0);
  }
  std::printf("  settlements on a tick boundary (10, 50, 500, 1000): %zu\n", on_boundary);
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
  double worst = 0.0;
  for (std::size_t r = 0; r < 5; ++r) {
    for (std::size_t c = 0; c < 5; ++c) {
      const double scale =
          std::sqrt(a.derivatives.gauss_newton[r][r] * a.derivatives.gauss_newton[c][c]);
      worst = std::max(
          worst, std::abs(a.derivatives.hessian[r][c] - a.derivatives.gauss_newton[r][c]) / scale);
    }
  }
  std::printf("  exact - GN         max |H - GN|_rc / sqrt(GN_rr GN_cc) = %.4e\n", worst);
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
    for (std::size_t p = 0; p < 5; ++p) {
      double diff = 0.0;
      double scale = 0.0;
      for (std::size_t i = 0; i < a.exact.quote.size(); ++i) {
        diff = std::max(diff, std::abs(a.gauss_newton->quote[i][p] - a.exact.quote[i][p]));
        scale = std::max(scale, std::abs(a.exact.quote[i][p]));
      }
      std::printf(" %s %.4f", kParameterNames[p], diff / scale);
    }
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

// Batch sums per version, expiry and strike bucket, and over every quote.
class Summary {
 public:
  void Add(std::size_t version, std::size_t slice, int bucket, std::span<const double> batch,
           double mean) {
    Sums& s = sums_[version][slice][static_cast<std::size_t>(bucket)];
    Accumulate(s.batch, batch);
    Accumulate(total_[version], batch);
    s.n += 1;
    s.max_abs = std::max(s.max_abs, std::abs(mean));
  }

  void Print(std::size_t slices) const {
    for (std::size_t version = 0; version < kVersionCount; ++version) {
      std::printf("  summary, %s: by expiry and k = ln(K / F) bucket: n, sum +- SE, max |.|\n",
                  kVersions[version]);
      std::size_t counted = 0;
      for (std::size_t j = 0; j < slices; ++j) {
        for (std::size_t b = 0; b < kBucketCount; ++b) {
          const Sums& bucket = sums_[version][j][b];
          counted += bucket.n;
          if (bucket.n == 0) continue;
          const aadp::BatchStatistics st = aadp::BatchMeansStatistics(bucket.batch);
          std::printf("    %-8s %-19s n %2zu  sum %+.6e +- %.2e  max %.4e\n", Expiry(j),
                      kBucketNames[b], bucket.n, st.mean, st.standard_error, bucket.max_abs);
        }
      }
      if (counted == 0) {
        std::printf("    no quotes: refused on every slice\n");
        continue;
      }
      const aadp::BatchStatistics st = aadp::BatchMeansStatistics(total_[version]);
      std::printf("    all %zu quotes%s, sum %+.6e +- %.2e (every settlement up one point)\n",
                  counted, counted == 180 ? "" : " counted (a slice refused, or not the capture)",
                  st.mean, st.standard_error);
    }
  }

 private:
  struct Sums {
    std::vector<double> batch;
    std::size_t n = 0;
    double max_abs = 0.0;
  };

  static void Accumulate(std::vector<double>& into, std::span<const double> batch) {
    if (into.empty()) into.assign(batch.size(), 0.0);
    for (std::size_t b = 0; b < batch.size(); ++b) into[b] += batch[b];
  }

  std::array<std::array<std::array<Sums, kBucketCount>, 3>, kVersionCount> sums_{};
  std::array<std::vector<double>, kVersionCount> total_{};
};

// One row per quote, in the three versions, and into the summary.
void PrintQuoteRows(const Loaded& data, const aadp::ad::LocalVolGradientEstimate& g,
                    std::span<const MarketPriceDerivative> dm, Summary& summary) {
  std::printf("  quote sensitivities d P / d V_i (per point of settlement), +- SE:\n");
  std::printf("    columns: slice i K right k | market fixed exact | GN | through parity\n");
  for (std::size_t j = 0; j < data.slices.size(); ++j) {
    const Slice& s = data.slices[j];
    const bool has_gn = s.s.adjoint.gauss_newton.has_value();
    for (std::size_t i = 0; i < s.s.through_parity.size(); ++i) {
      const aadp::svi::FitQuote& fq = s.chain->quotes.quotes[i];
      const double k = std::log(fq.strike / s.chain->parity.forward);
      const Vector5 gn = has_gn ? s.s.adjoint.gauss_newton->quote[i] : Vector5{};
      const std::array<std::vector<double>, kVersionCount> v = {
          QuoteBatches(g, j, s.s.adjoint.exact.quote[i], nullptr, nullptr),
          QuoteBatches(g, j, gn, nullptr, nullptr),
          QuoteBatches(g, j, s.s.through_parity[i], &s.s.market[i], &dm[j])};
      std::printf("    %-8s %2zu %7.0f %s %+.5f", Expiry(j), i, fq.strike,
                  fq.type == aadp::svi::OptionType::kCall ? "C" : "P", k);
      for (std::size_t version = 0; version < kVersionCount; ++version) {
        if (version == 1 && !has_gn) {
          std::printf(" | Gauss-Newton refused       ");
          continue;
        }
        const aadp::BatchStatistics st = aadp::BatchMeansStatistics(v[version]);
        std::printf(" | %+.6e +- %.2e", st.mean, st.standard_error);
        summary.Add(version, j, Bucket(k), v[version], st.mean);
      }
      std::printf("\n");
    }
  }
}

int Report(const Loaded& data, const Options& o) {
  for (std::size_t j = 0; j < data.slices.size(); ++j) PrintAdjoint(j, data.slices[j]);
  for (std::size_t j = 0; j < data.slices.size(); ++j) PrintQuoteTable(j, data.slices[j]);
  const double maturity = data.surface.terms.back().expiry;
  for (const aadp::svi::NamedContract& c : aadp::svi::SurfaceContracts(maturity)) {
    const auto g = aadp::ad::PriceLocalVolWithGradient(c.contract, data.surface.terms,
                                                       data.surface.params, Run(o));
    std::printf("\n== %s ==\n", c.name);
    if (!g) {
      std::printf("  REFUSED: %s\n", aadp::pricer::LocalVolErrorName(g.error().code));
      return kExitFailure;
    }
    const auto dm = MarketDerivatives(c, data.surface, Run(o), g->value.mean);
    if (!dm) {
      std::printf("  d P / d(F, D): no level resolved\n");
      return kExitFailure;
    }
    PrintContract(*g, *dm);
    Summary summary;
    PrintQuoteRows(data, *g, *dm, summary);
    summary.Print(data.slices.size());
  }
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
void PrintJudgement(const char* name, const HalvingJudgement& e, double reference,
                    std::span<const double> levels) {
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
                const SliceTolerances& tol, QuoteChain chain, const QuoteLevels& q,
                CheckTallies& t) {
  const std::size_t ci = chain == QuoteChain::kMarketFixed ? 0 : 1;
  const bool through = chain == QuoteChain::kThroughParity;
  const Vector5& exact = through ? s.s.through_parity[i] : s.s.adjoint.exact.quote[i];
  const bool has_gn = s.s.adjoint.gauss_newton.has_value();
  Vector5 gn{};
  if (has_gn) {
    gn = through ? s.s.through_parity_gauss_newton[i] : s.s.adjoint.gauss_newton->quote[i];
  }
  std::printf("  quote %s %2zu %-14s", Expiry(j), i, aadp::svi::QuoteChainName(chain));
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
    PrintJudgement(kParameterNames[p], e, exact[p], col);
  }
  if (through) {
    for (std::size_t p = 0; p < 3; ++p) {
      std::array<double, kLevels> col{};
      for (std::size_t l = 0; l < kLevels; ++l) col[l] = q.d_market[l][p];
      const HalvingJudgement e = aadp::svi::JudgeByHalving(
          q.h, col, q.usable, kPriceRounding * std::abs(tol.parity_value[p]), s.s.market[i][p],
          tol.market_floor[p]);
      t.market.Add(e);
      PrintJudgement(kMarketNames[p], e, s.s.market[i][p], col);
    }
  }
  constexpr std::array<const char*, 3> kPriceNames = {"P0", "P1", "P2"};
  for (std::size_t c = 0; c < base.contracts.size(); ++c) {
    const PriceReference r = ReferenceOf(base, c, j, s, i, tol, exact, gn, chain);
    const HalvingJudgement e =
        aadp::svi::JudgeByHalving(q.h, q.d_price[c], q.usable, r.refit, r.exact, r.floor);
    t.price[ci][0].Add(e);
    if (has_gn) {
      t.price[ci][1].Add(
          aadp::svi::JudgeByHalving(q.h, q.d_price[c], q.usable, r.refit, r.gauss_newton, r.floor));
    }
    PrintJudgement(kPriceNames[c], e, r.exact, q.d_price[c]);
  }
  std::printf("\n");
  std::fflush(stdout);
}

std::optional<CheckBase> PrepareCheck(const Loaded& data, const Options& o) {
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
    std::printf("  %-34s price %.6f, base for the price-level check\n", c.name, g->value.mean);
    base.gradients.push_back(*g);
    base.market_derivatives.push_back(*dm);
  }
  return base;
}

bool PrintCheckSummary(const CheckTallies& t) {
  std::printf("\n== check summary ==\n");
  std::printf(
      "  bumped levels %zu, unusable (a fit failed, did not converge or changed its active "
      "set) %zu\n",
      t.levels_total, t.unusable_levels);
  PrintTally("parameters, market fixed, exact Hessian", t.theta[0][0]);
  PrintTally("parameters, through parity, exact Hessian", t.theta[1][0]);
  PrintTally("F, D, sigma_leg through parity", t.market);
  PrintTally("prices, market fixed, exact Hessian", t.price[0][0]);
  PrintTally("prices, through parity, exact Hessian", t.price[1][0]);
  PrintTally("parameters, market fixed, Gauss-Newton (disclosure)", t.theta[0][1]);
  PrintTally("parameters, through parity, Gauss-Newton (disclosure)", t.theta[1][1]);
  PrintTally("prices, market fixed, Gauss-Newton (disclosure)", t.price[0][1]);
  PrintTally("prices, through parity, Gauss-Newton (disclosure)", t.price[1][1]);
  const bool pass = t.theta[0][0].AllPass() && t.theta[1][0].AllPass() && t.market.AllPass() &&
                    t.price[0][0].AllPass() && t.price[1][0].AllPass();
  std::printf("  verdict: %s\n", pass ? "PASS" : "FAIL");
  return pass;
}

int Check(const Loaded& data, const Options& o) {
  std::printf(
      "\n== bump-and-recalibrate check: levels h_l = 2^-l min(noise_i, V_i / 2), l = 0..5; "
      "every %u%s quote; refits %s ==\n",
      o.stride,
      o.stride == 1 ? "st (all, as registered)" : "th (short run, not the registered check)",
      o.refit == RefitKind::kPolished
          ? "polished by one KKT Newton step (decided after the pre-registration)"
          : "as the production fit stopped (as pre-registered)");
  const std::optional<CheckBase> base = PrepareCheck(data, o);
  if (!base) return kExitFailure;
  CheckTallies t;
  for (std::size_t j = 0; j < data.slices.size(); ++j) {
    const Slice& s = data.slices[j];
    const SliceTolerances tol = Tolerances(s);
    std::printf(
        "\n-- slice %s: refit error (alternative grids) a %.3e b %.3e rho %.3e m %.3e "
        "sigma %.3e --\n",
        Expiry(j), tol.eps[0], tol.eps[1], tol.eps[2], tol.eps[3], tol.eps[4]);
    for (std::size_t i = 0; i < s.s.through_parity.size(); i += o.stride) {
      for (const QuoteChain chain : {QuoteChain::kMarketFixed, QuoteChain::kThroughParity}) {
        const QuoteLevels q = BumpLevels(data, *base, o, j, i, chain);
        t.levels_total += kLevels;
        t.unusable_levels += q.unusable;
        JudgeQuote(*base, j, s, i, tol, chain, q, t);
      }
    }
  }
  return PrintCheckSummary(t) ? 0 : kExitFailure;
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

int Timing(const Loaded& data, const Options& o, std::span<const aadp::taifex::OptionRow> options) {
  using Clock = std::chrono::steady_clock;
  std::printf("\n== timing: ITERATION ONLY, not a result ==\n");
  const double maturity = data.surface.terms.back().expiry;
  for (const aadp::svi::NamedContract& c : aadp::svi::SurfaceContracts(maturity)) {
    std::vector<double> grad_ms;
    std::vector<double> ift_ms;
    std::vector<double> market_ms;
    double sink = 0.0;
    for (std::uint32_t rep = 0; rep < o.repetitions; ++rep) {
      const auto t0 = Clock::now();
      const auto g = aadp::ad::PriceLocalVolWithGradient(c.contract, data.surface.terms,
                                                         data.surface.params, Run(o));
      const auto t1 = Clock::now();
      if (!g) return kExitFailure;
      const std::optional<double> ift = ImplicitStep(data, options, *g);
      const auto t2 = Clock::now();
      const std::optional<double> market = MarketStep(data, c, o);
      const auto t3 = Clock::now();
      if (!ift || !market) return kExitFailure;
      sink += *ift + *market + g->value.mean;
      grad_ms.push_back(Milliseconds(t1 - t0));
      ift_ms.push_back(Milliseconds(t2 - t1));
      market_ms.push_back(Milliseconds(t3 - t2));
    }
    const double grad = Median(grad_ms);
    const double ift = Median(ift_ms);
    const double market = Median(market_ms);
    std::printf(
        "  %-34s price + 15 sensitivities median %.2f ms; implicit-function step and composition "
        "for 180 quotes median %.3f ms (ratio %.4f); through parity adds d P / d(F, D) by 12 "
        "prices, median %.2f ms (ratio %.4f); over %u repetitions (check %.6g)\n",
        c.name, grad, ift, (grad + ift) / grad, market, (grad + ift + market) / grad, o.repetitions,
        sink / o.repetitions);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) try {
  const std::optional<Options> parsed =
      Parse(std::span<char*>(argv, static_cast<std::size_t>(argc)));
  if (!parsed) return Usage();
  const Options& o = *parsed;
  Header(o);
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
