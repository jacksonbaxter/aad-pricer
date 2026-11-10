// The eSSVI surface beside the raw SVI fit on the TAIFEX captures (NOTES.md, "Stage 7c"): the
// arbitrage-free comparison of BENCHMARKS.md SVI-9, and its prices as a second line beside 7a's.
// Every number it prints is iteration only.
//
//   aadp_essvi_report [--options <file>] [--futures <file>] [--mode fit|price|timing]
//                     [--paths <N>] [--threads <T>] [--repetitions <R>]
//
// fit     per slice, eSSVI beside raw SVI (stage 6b's fit, refitted here): parameters, the box,
//         the binding constraint, Phi and Phi / (n - k), SVI-6, SVI-11, stage 6a's checks on the
//         converted slices and the raw ones, the triple counts, the constraint slacks, the starts.
// price   7a's three contracts off both surfaces on the same uniforms: each price +- SE, the
//         paired difference eSSVI - raw +- SE from the batch differences, and both floor reports.
//         N = 2^20, 64 batches, 8 threads by default, seed 20261115, as 7a's report.
// timing  SVI-8: wall time of the eSSVI surface fit alone and with every check, 50 repetitions by
//         default: run it through measure.sh.
// The output is derived from the chain, so it opens with the attribution.

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

#include "build_info.hpp"
#include "data/attribution.hpp"
#include "data/calibration_set.hpp"
#include "data/taifex_csv.hpp"
#include "data/taifex_futures.hpp"
#include "data/taifex_options.hpp"
#include "oracle/tolerance.hpp"
#include "pricer/contract.hpp"
#include "pricer/local_vol.hpp"
#include "pricer/local_vol_model.hpp"
#include "pricer/runner.hpp"
#include "svi/arbitrage.hpp"
#include "svi/chain_essvi.hpp"
#include "svi/chain_fit.hpp"
#include "svi/chain_surface.hpp"
#include "svi/essvi.hpp"
#include "svi/essvi_fit.hpp"
#include "svi/fit.hpp"
#include "svi/local_vol.hpp"
#include "svi/raw_svi.hpp"
#include "svi/surface.hpp"

namespace {

using aadp::pricer::FloorRange;
using aadp::pricer::FloorTally;
using aadp::pricer::RunConfig;
using aadp::svi::ChainSlice;
using aadp::svi::ChainSurface;
using aadp::svi::EssviChainFit;
using aadp::svi::EssviFitResult;
using aadp::svi::SurfaceFit;

constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;
// A development seed, never 1729 or 271828: 7a's, so the raw line is 7a's and the pairs share
// their uniforms.
constexpr std::uint64_t kSeed = 20261115;
constexpr std::uint32_t kBatches = 64;

enum class Mode : std::uint8_t { kFit, kPrice, kTiming };

struct Options {
  std::string options_path = "data/DailyMarketReportOpt.csv";
  std::string futures_path = "data/DailyMarketReportFut.csv";
  Mode mode = Mode::kFit;
  std::uint64_t paths = std::uint64_t{1} << 20;
  std::uint32_t threads = 8;
  std::uint32_t repetitions = 50;
};

int Usage() {
  std::cerr << "usage: aadp_essvi_report [--options <file>] [--futures <file>] "
               "[--mode fit|price|timing] [--paths <N, a multiple of 64>] [--threads <T>] "
               "[--repetitions <R>]\n";
  return kExitUsage;
}

std::uint64_t Count(std::string_view text) {
  std::uint64_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc() && end == text.data() + text.size() ? value : 0;
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
    if (value == "fit") {
      o.mode = Mode::kFit;
    } else if (value == "price") {
      o.mode = Mode::kPrice;
    } else if (value == "timing") {
      o.mode = Mode::kTiming;
    } else {
      return false;
    }
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
  return o;
}

void Header(const Options& o) {
  aadp::write_attribution(std::cout);
  std::cout << "\naadp_essvi_report: an arbitrage-free eSSVI surface beside the raw SVI fit, on "
               "TAIFEX TXO settlements of trade date 2024-11-15.\n"
               "These are daily settlement prices set by the exchange's rules, not tradable "
               "quotes.\n"
               "Maturity reaches 33 days only: expiries 202411, 202411W4 and 202412 (5, 12 and "
               "33 days).\n"
               "ITERATION ONLY, not a result. Method, constraints and tolerances: NOTES.md, "
               "\"Stage 7c\". Nothing is repaired.\n";
  std::cout << "compiler: " << aadp::build_info::compiler_id << ' '
            << aadp::build_info::compiler_version << ", " << aadp::build_info::build_type << ", "
            << aadp::build_info::cxx_flags << '\n';
  if (o.mode == Mode::kPrice) {
    std::printf("run: seed %llu, paths %llu, batches %u, threads %u\n",
                static_cast<unsigned long long>(kSeed), static_cast<unsigned long long>(o.paths),
                kBatches, o.threads);
  }
  std::fflush(stdout);
}

const char* Expiry(std::size_t j) {
  constexpr std::array<const char*, 3> kNames = {"202411", "202411W4", "202412"};
  static_assert(kNames.size() == aadp::svi::kCalibrationExpiries.size());
  return j < kNames.size() ? kNames[j] : "?";
}

void PrintViolations(const char* label, const std::vector<aadp::svi::Violation>& violations) {
  if (violations.empty()) {
    std::printf("  %-26s none\n", label);
    return;
  }
  std::printf("  %-26s %zu\n", label, violations.size());
  for (const aadp::svi::Violation& v : violations) {
    std::printf("    %-20s k [%.6g, %.6g], worst k %.6g, value %.6g, bound %.6g, points %zu\n",
                aadp::svi::KindName(v.kind), v.k_begin, v.k_end, v.k_worst, v.value, v.bound,
                v.points);
  }
}

std::string ActiveRaw(const aadp::svi::FitResult& fit) {
  std::string out;
  for (const aadp::svi::Bound bound : fit.ActiveBounds()) {
    out += out.empty() ? "" : ", ";
    out += aadp::svi::BoundName(bound);
  }
  return out.empty() ? "none" : out;
}

const char* EndMark(const aadp::svi::EssviCoordinateState& c) {
  if (c.at_lower) return "  AT LOWER END";
  return c.at_upper ? "  AT UPPER END" : "";
}

void PrintStarts(const EssviFitResult& r) {
  std::printf(
      "  result              Phi %.10g, converged %s, stop %s, iterations %zu, scaled "
      "gradient %.3g, starts at best %zu of %zu\n",
      r.objective, r.converged ? "yes" : "NO", aadp::svi::EssviStopName(r.stop), r.iterations,
      r.gradient, r.starts_at_best, r.starts.size());
  for (std::size_t s = 0; s < r.starts.size(); ++s) {
    const aadp::svi::EssviStart& st = r.starts[s];
    std::printf("    start %2zu%s Phi %.10g, stop %s, iterations %zu, scaled gradient %.3g\n", s,
                s == 0 ? " (Mingone's)" : "            ", st.objective,
                aadp::svi::EssviStopName(st.stop), st.iterations, st.gradient);
  }
  std::printf("  coordinates (box ends within 1e-9 of the width are marked)\n");
  for (const aadp::svi::EssviCoordinateState& c : r.coordinates) {
    std::printf("    %-8s slice %zu  %.12g  in [%.6g, %.6g]%s\n",
                aadp::svi::EssviCoordinateName(c.coordinate), c.slice, c.value, c.lower, c.upper,
                EndMark(c));
  }
  std::printf("  constraints on the fitted (theta, rho, psi): %zu violated\n",
              r.constraints.violations);
  for (const aadp::svi::EssviConstraintState& c : r.constraints.states) {
    std::printf("    %-10s slice %zu  slack %.6g%s%s\n",
                aadp::svi::EssviConstraintName(c.constraint), c.slice, c.slack,
                c.active ? "  ACTIVE" : "", c.violated ? "  VIOLATED" : "");
  }
}

int Fit(std::span<const ChainSlice> slices) {
  const SurfaceFit raw = aadp::svi::FitChain(slices);
  const EssviChainFit essvi = aadp::svi::FitChainEssvi(slices);
  std::printf(
      "\n== eSSVI surface fit: all slices at once, Mingone's global parametrisation, "
      "butterfly B1 and B2, calendar N and S ==\n");
  if (!essvi.fit) {
    std::printf("  FAILED: %s, slice %zu, %s\n",
                aadp::svi::EssviFitErrorName(essvi.fit.error().code), essvi.fit.error().slice,
                aadp::svi::FitErrorName(essvi.fit.error().fit.code));
    return kExitFailure;
  }
  const EssviFitResult& r = *essvi.fit;
  PrintStarts(r);
  double raw_total = 0.0;
  std::size_t quotes = 0;
  bool raw_ok = true;
  for (std::size_t j = 0; j < slices.size(); ++j) {
    const ChainSlice& slice = slices[j];
    const aadp::svi::EssviSliceFit& e = r.slices[j];
    const aadp::svi::EssviChainSlice& ec = essvi.slices[j];
    const auto n = static_cast<double>(e.quotes);
    quotes += e.quotes;
    std::printf("\n== slice %s: n %zu, T %.10f, F %.6f, D %.8f ==\n", slice.expiry.c_str(),
                e.quotes, slice.years, slice.parity.forward, slice.parity.discount);
    std::printf("  eSSVI               theta %.10g  rho %.10g  psi %.10g\n", e.params.theta,
                e.params.rho, e.params.psi);
    std::printf("  eSSVI as raw SVI    a %.10g  b %.10g  rho %.10g  m %.10g  sigma %.10g\n",
                e.raw.a, e.raw.b, e.raw.rho, e.raw.m, e.raw.sigma);
    std::printf("  psi interval        [%.10g, %.10g], cap %s%s\n", e.interval.lower,
                e.interval.upper, aadp::svi::PsiCapName(e.interval.cap),
                e.interval.cap == aadp::svi::PsiCap::kLaterSlice
                    ? (std::string(" (slice ") + Expiry(e.interval.cap_slice) + ")").c_str()
                    : "");
    const aadp::svi::SliceFit& rs = raw.slices[j];
    if (!rs.fit) {
      std::printf("  raw SVI             FAILED: %s\n",
                  aadp::svi::FitErrorName(rs.fit.error().code));
      raw_ok = false;
      continue;
    }
    const aadp::svi::FitResult& rf = *rs.fit;
    raw_total += rf.objective;
    std::printf(
        "  raw SVI (6b)        a %.10g  b %.10g  rho %.10g  m %.10g  sigma %.10g; active "
        "%s\n",
        rf.params.a, rf.params.b, rf.params.rho, rf.params.m, rf.params.sigma,
        ActiveRaw(rf).c_str());
    std::printf(
        "  Phi                 eSSVI %.6g, Phi/(n-3) %.6g | raw SVI %.6g, Phi/(n-5) %.6g "
        "| ratio %.4g\n",
        e.objective, e.objective / (n - 3.0), rf.objective, rf.objective / (n - 5.0),
        e.objective / rf.objective);
    std::printf("  SVI-6 (bp of F)     eSSVI %.4f | raw SVI %.4f | difference %+.4f\n", ec.svi6_bp,
                rs.svi6_bp, ec.svi6_bp - rs.svi6_bp);
    std::printf("  SVI-11 (vol points) eSSVI %.4f | raw SVI %.4f | difference %+.4f\n",
                ec.svi11_vol_points, rs.svi11_vol_points,
                ec.svi11_vol_points - rs.svi11_vol_points);
    std::printf(
        "  model triples       eSSVI non-convex %zu of %zu | raw SVI %zu of %zu | input "
        "settlements %zu of %zu\n",
        ec.model_butterflies.non_convex, ec.model_butterflies.triples,
        rs.model_butterflies.non_convex, rs.model_butterflies.triples,
        slice.input_butterflies.non_convex, slice.input_butterflies.triples);
    std::printf("  eSSVI checks        grid %zu points, min g %.6g at k %.6g, Martini-Mingone %s\n",
                ec.butterfly.grid_points, ec.butterfly.min_g, ec.butterfly.k_min_g,
                aadp::svi::MmVerdictName(ec.butterfly.classifier.verdict));
    PrintViolations("eSSVI butterfly", ec.butterfly.violations);
    std::printf("  raw SVI checks      grid %zu points, min g %.6g at k %.6g, Martini-Mingone %s\n",
                rs.butterfly.grid_points, rs.butterfly.min_g, rs.butterfly.k_min_g,
                aadp::svi::MmVerdictName(rs.butterfly.classifier.verdict));
    PrintViolations("raw SVI butterfly", rs.butterfly.violations);
  }
  std::printf("\n== calendar, adjacent slices ==\n");
  for (const aadp::svi::CalendarPair& pair : essvi.calendar) {
    std::printf("  eSSVI   %s -> %s: grid %zu points, min gap %.6g at k %.6g\n",
                Expiry(pair.earlier), Expiry(pair.later), pair.report.grid_points,
                pair.report.min_gap, pair.report.k_min_gap);
    PrintViolations("eSSVI calendar", pair.report.violations);
  }
  for (const aadp::svi::CalendarPair& pair : raw.calendar) {
    std::printf("  raw SVI %s -> %s: grid %zu points, min gap %.6g at k %.6g\n",
                Expiry(pair.earlier), Expiry(pair.later), pair.report.grid_points,
                pair.report.min_gap, pair.report.k_min_gap);
    PrintViolations("raw SVI calendar", pair.report.violations);
  }
  const auto n = static_cast<double>(quotes);
  std::printf("\n== surface ==\n");
  std::printf(
      "  Phi                 eSSVI %.6g, Phi/(%zu-9) %.6g | raw SVI %.6g, Phi/(%zu-15) "
      "%.6g | ratio %.4g\n",
      r.objective, quotes, r.objective / (n - 9.0), raw_total, quotes, raw_total / (n - 15.0),
      r.objective / raw_total);
  std::printf("\n== machine-readable ==\n");
  std::printf(
      "essvi,expiry,n,theta,rho,psi,a,b,rho,m,sigma,objective,svi6_bp,svi11_vol_points,"
      "butterfly_violations\n");
  for (std::size_t j = 0; j < r.slices.size(); ++j) {
    const aadp::svi::EssviSliceFit& e = r.slices[j];
    std::printf(
        "essvi,%s,%zu,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,"
        "%zu\n",
        Expiry(j), e.quotes, e.params.theta, e.params.rho, e.params.psi, e.raw.a, e.raw.b,
        e.raw.rho, e.raw.m, e.raw.sigma, e.objective, essvi.slices[j].svi6_bp,
        essvi.slices[j].svi11_vol_points, essvi.slices[j].butterfly.violations.size());
  }
  return raw_ok && r.converged ? 0 : kExitFailure;
}

void PrintRange(const char* label, const FloorRange& r, std::uint64_t steps) {
  if (r.count == 0) {
    std::printf("      %-22s 0\n", label);
    return;
  }
  std::printf("      %-22s %llu (%.4g%% of path-steps), k [%.5f, %.5f], t [%.6f, %.6f]\n", label,
              static_cast<unsigned long long>(r.count),
              100.0 * static_cast<double>(r.count) / static_cast<double>(steps), r.k_min, r.k_max,
              r.t_min, r.t_max);
}

void PrintFloors(const char* label, const FloorTally& t, std::size_t segments) {
  constexpr std::array<const char*, 3> kSegments = {"[0, T1)", "[T1, T2)", "[T2, T3]"};
  std::printf("    floors, %s: path-steps %llu over %llu paths\n", label,
              static_cast<unsigned long long>(t.path_steps),
              static_cast<unsigned long long>(t.paths));
  for (std::size_t c = 0; c < t.by_class.size(); ++c) {
    std::printf("      %-22s %llu\n",
                aadp::svi::LocalVarianceClassName(static_cast<aadp::svi::LocalVarianceClass>(c)),
                static_cast<unsigned long long>(t.by_class[c]));
  }
  std::printf("      paths with a floor     %llu, with a cap %llu\n",
              static_cast<unsigned long long>(t.paths_with_floor),
              static_cast<unsigned long long>(t.paths_with_cap));
  PrintRange("floored, all", FloorTally::Total(t.floored), t.path_steps);
  PrintRange("capped, all", FloorTally::Total(t.capped), t.path_steps);
  for (std::size_t s = 0; s < segments && s < kSegments.size(); ++s) {
    const std::string f = std::string("floored, ") + kSegments[s];
    const std::string c = std::string("capped, ") + kSegments[s];
    PrintRange(f.c_str(), t.floored[s], t.path_steps);
    PrintRange(c.c_str(), t.capped[s], t.path_steps);
  }
}

void PrintSurface(const char* label, const ChainSurface& s) {
  const aadp::svi::SviSurface<double> surface(s.terms, s.params);
  std::printf("  %s surface, S0 = D1 F1 = %.6f\n", label, surface.Curve().Spot());
  for (std::size_t j = 0; j < s.params.size(); ++j) {
    const aadp::svi::RawSvi<double>& p = s.params[j];
    std::printf("    %-9s a %.10g  b %.10g  rho %.10g  m %.10g  sigma %.10g\n", Expiry(j), p.a, p.b,
                p.rho, p.m, p.sigma);
  }
}

int Price(std::span<const ChainSlice> slices, const Options& o) {
  const std::optional<ChainSurface> raw =
      aadp::svi::SurfaceFromChain(slices, aadp::svi::FitChain(slices));
  const std::optional<ChainSurface> essvi =
      aadp::svi::SurfaceFromEssvi(slices, aadp::svi::FitChainEssvi(slices));
  if (!raw || !essvi) {
    std::printf("  a fit failed\n");
    return kExitFailure;
  }
  for (const ChainSurface* s : {&*raw, &*essvi}) {
    const aadp::svi::SurfaceError error = aadp::svi::ValidateSurface(s->terms, s->params);
    if (error != aadp::svi::SurfaceError::kNone) {
      std::printf("  surface refused: %s\n", aadp::svi::SurfaceErrorName(error));
      return kExitFailure;
    }
  }
  std::printf(
      "\n== pricing: 7a's contracts off the raw SVI surface (7a's line) and off the eSSVI "
      "surface (second line), same uniforms ==\n");
  PrintSurface("raw SVI", *raw);
  PrintSurface("eSSVI  ", *essvi);
  const RunConfig run{.seed = kSeed, .paths = o.paths, .batches = kBatches, .threads = o.threads};
  const double maturity = raw->terms.back().expiry;
  for (const aadp::svi::NamedContract& c : aadp::svi::SurfaceContracts(maturity)) {
    const auto pr = aadp::pricer::PriceLocalVol(c.contract, raw->terms, raw->params, run);
    const auto pe = aadp::pricer::PriceLocalVol(c.contract, essvi->terms, essvi->params, run);
    const aadp::pricer::Contract& terms = c.contract.contract;
    std::printf("\n== %s: K %.0f", c.name, terms.strike);
    if (terms.product == aadp::pricer::Product::kBarrier) {
      std::printf(", H %.0f", terms.barrier.level);
    }
    std::printf(", %u dates, n %u, T %.10f ==\n", terms.steps, c.contract.substeps,
                c.contract.maturity);
    if (!pr || !pe) {
      std::printf("  REFUSED: %s\n",
                  aadp::pricer::LocalVolErrorName((!pr ? pr.error() : pe.error()).code));
      return kExitFailure;
    }
    std::vector<double> diff;
    diff.reserve(pr->value.batch_means.size());
    for (std::size_t b = 0; b < pr->value.batch_means.size(); ++b) {
      diff.push_back(pe->value.batch_means[b] - pr->value.batch_means[b]);
    }
    const aadp::BatchStatistics d = aadp::BatchMeansStatistics(diff);
    std::printf("  raw SVI (7a)        %.6f +- %.6f (1 SE)\n", pr->value.mean,
                pr->value.standard_error);
    std::printf("  eSSVI               %.6f +- %.6f (1 SE)\n", pe->value.mean,
                pe->value.standard_error);
    std::printf("  paired eSSVI - raw  %+.6f +- %.6f (%+.2f SE), from %zu batch differences\n",
                d.mean, d.standard_error, d.mean / d.standard_error, diff.size());
    PrintFloors("eSSVI", pe->floors, essvi->terms.size());
    PrintFloors("raw SVI", pr->floors, raw->terms.size());
  }
  return 0;
}

double Median(std::vector<double> v) {
  std::ranges::sort(v);
  const std::size_t n = v.size();
  return n % 2 == 1 ? v[n / 2] : 0.5 * (v[(n / 2) - 1] + v[n / 2]);
}

int Timing(std::span<const ChainSlice> slices, const Options& o) {
  using Clock = std::chrono::steady_clock;
  std::printf("\n== timing: ITERATION ONLY, not a result (SVI-8, eSSVI surface) ==\n");
  std::vector<double> fit_ms;
  std::vector<double> all_ms;
  std::vector<aadp::svi::FitSlice> quotes;
  for (const ChainSlice& s : slices) quotes.push_back(s.quotes);
  double sink = 0.0;
  for (std::uint32_t rep = 0; rep < o.repetitions; ++rep) {
    const auto t0 = Clock::now();
    const auto fit = aadp::svi::FitEssviSurface(quotes);
    const auto t1 = Clock::now();
    const EssviChainFit chain = aadp::svi::FitChainEssvi(slices);
    const auto t2 = Clock::now();
    if (!fit || !chain.fit) {
      std::printf("  fit failed in timing run\n");
      return kExitFailure;
    }
    sink += fit->objective + static_cast<double>(chain.fit->constraints.violations);
    fit_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    all_ms.push_back(std::chrono::duration<double, std::milli>(t2 - t1).count());
  }
  std::printf("  SVI-8 eSSVI fit alone        median %.3f ms, min %.3f ms over %u repetitions\n",
              Median(fit_ms), *std::ranges::min_element(fit_ms), o.repetitions);
  std::printf(
      "  SVI-8 eSSVI fit and checks   median %.3f ms, min %.3f ms over %u repetitions "
      "(check %.10g)\n",
      Median(all_ms), *std::ranges::min_element(all_ms), o.repetitions, sink / o.repetitions);
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
    std::cerr << "aadp_essvi_report: "
              << (option_bytes ? future_bytes.error() : option_bytes.error())
              << ". Run scripts/fetch_data.sh.\n";
    return kExitFailure;
  }
  const auto options = aadp::taifex::read_options(*option_bytes);
  const auto futures = aadp::taifex::read_futures(*future_bytes);
  if (!options || !futures) {
    std::cerr << "aadp_essvi_report: "
              << aadp::taifex::to_string(options ? futures.error().code : options.error().code)
              << '\n';
    return kExitFailure;
  }
  const auto slices =
      aadp::svi::BuildChainSlices(options->rows, futures->rows, aadp::svi::kCalibrationExpiries);
  if (!slices) {
    std::cerr << "aadp_essvi_report: " << aadp::svi::ChainErrorName(slices.error().code) << " on "
              << slices.error().expiry << '\n';
    return kExitFailure;
  }
  switch (o.mode) {
    case Mode::kFit:
      return Fit(*slices);
    case Mode::kPrice:
      return Price(*slices, o);
    case Mode::kTiming:
      return Timing(*slices, o);
  }
  return kExitFailure;
} catch (const std::exception& error) {
  std::fprintf(stderr, "aadp_essvi_report: %s\n", error.what());
  return kExitFailure;
}
