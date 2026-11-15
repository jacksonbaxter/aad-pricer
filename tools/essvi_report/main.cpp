// The eSSVI surface beside the raw SVI fit on the TAIFEX captures (docs/NOTES.md, "Stage 7c"): the
// arbitrage-free comparison of BENCHMARKS.md SVI-9, and its prices as a second line beside 7a's.
// Every number it prints is iteration only.
//
//   aadp_essvi_report [--options <file>] [--futures <file>] [--mode fit|price|timing]
//                     [--paths <N>] [--threads <T>] [--repetitions <R>]
//   aadp_essvi_report --report [the same flags] [run flags]
//
// fit     per slice, eSSVI beside raw SVI (stage 6b's fit, refitted here): parameters, the box,
//         the binding constraint, Phi and Phi / (n - k), SVI-6, SVI-11, stage 6a's checks on the
//         converted slices and the raw ones, the triple counts, the constraint slacks, the starts.
// price   7a's three contracts off both surfaces on the same uniforms: each price +- SE, the
//         paired difference eSSVI - raw +- SE from the batch differences, and both floor reports.
//         N = 2^20, 64 batches, 8 threads by default, seed 20261115, as 7a's report.
// timing  SVI-8: wall time of the eSSVI surface fit alone and with every check, 50 repetitions by
//         default: run it through measure.sh.
// The output is derived from the chain, so it opens with the attribution. Each mode ends with
// machine-readable lines at full precision.
//
// --report writes the mode's figures as a report file instead (src/results/report.hpp), for
// scripts/run_bench.sh and the stamp: the same doubles as the machine-readable lines, every table
// derived from the chain, with the attribution, and the timing mode's tables marked as timings.
// Derived figures only: no settlement, no per-strike row, no fitted parameter. A verdict that
// fails is a cell, so a written report exits 0. The run flags are those of
// src/results/run_header.hpp, and only --report takes them.

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
#include <optional>
#include <ratio>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "../common/floor_report.hpp"
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
#if AADP_REPORT_MODE
#include "results/report.hpp"
#include "results/report_run.hpp"
#include "results/results_file.hpp"
#include "results/run_header.hpp"
#endif

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
  bool report = false;
  bool run_flags = false;  // any of the run flags of src/results/run_header.hpp
#if AADP_REPORT_MODE
  aadp::results::RunOptions run;
#endif
};

int Usage() {
  std::cerr << "usage: aadp_essvi_report [--report] [--options <file>] [--futures <file>] "
               "[--mode fit|price|timing] [--paths <N, a multiple of 64>] [--threads <T>] "
               "[--repetitions <R>] [run flags, with --report only]\n";
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

// The flags, run flags included; nothing for a usage error.
std::optional<Options> Parse(int argc, char* const* argv) {
  Options o;
  std::vector<std::string> args;
#if AADP_REPORT_MODE
  auto rest = aadp::results::take_run_flags(argc, argv, o.run);
  if (!rest) {
    std::cerr << "aadp_essvi_report: " << rest.error() << '\n';
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
               "ITERATION ONLY, not a result. Method, constraints and tolerances: docs/NOTES.md, "
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

// Both fits of the chain, from one call each, for the text and for the report.
struct Fits {
  SurfaceFit raw;
  EssviChainFit essvi;
};

std::size_t ActiveConstraints(const EssviFitResult& r) {
  return static_cast<std::size_t>(std::ranges::count_if(
      r.constraints.states, [](const aadp::svi::EssviConstraintState& c) { return c.active; }));
}

// Full precision, for comparing hosts and a report's cells with the text.
void PrintFitLines(std::span<const ChainSlice> slices, const Fits& fits) {
  const EssviFitResult& r = *fits.essvi.fit;
  std::printf("raw,expiry,n,objective,svi6_bp,svi11_vol_points\n");
  for (std::size_t j = 0; j < slices.size(); ++j) {
    const aadp::svi::SliceFit& rs = fits.raw.slices[j];
    if (!rs.fit) continue;
    std::printf("raw,%s,%zu,%.17g,%.17g,%.17g\n", Expiry(j), rs.fit->quotes, rs.fit->objective,
                rs.svi6_bp, rs.svi11_vol_points);
  }
  const auto violations = [](const std::string& where,
                             const std::vector<aadp::svi::Violation>& list) {
    for (std::size_t i = 0; i < list.size(); ++i) {
      const aadp::svi::Violation& v = list[i];
      std::printf("violation,%s,%zu,%s,%.17g,%.17g,%.17g,%.17g,%.17g,%zu\n", where.c_str(), i + 1,
                  aadp::svi::KindName(v.kind), v.k_begin, v.k_end, v.k_worst, v.value, v.bound,
                  v.points);
    }
  };
  const auto arbitrage = [&violations](const std::string& key, const aadp::ButterflyCounts& model,
                                       const aadp::svi::ButterflyReport& b) {
    std::printf("arbitrage,%s,%zu,%zu,%zu,%.17g,%.17g,%s,%zu\n", key.c_str(), model.triples,
                model.non_convex, b.grid_points, b.min_g, b.k_min_g,
                aadp::svi::MmVerdictName(b.classifier.verdict), b.violations.size());
    violations(key, b.violations);
  };
  std::printf(
      "arbitrage,key,model_triples,model_non_convex,grid_points,min_g,k_min_g,martini_mingone,"
      "violations\n");
  for (std::size_t j = 0; j < slices.size(); ++j) {
    arbitrage(std::string(Expiry(j)) + ".essvi", fits.essvi.slices[j].model_butterflies,
              fits.essvi.slices[j].butterfly);
    const aadp::svi::SliceFit& rs = fits.raw.slices[j];
    if (rs.fit) arbitrage(std::string(Expiry(j)) + ".raw", rs.model_butterflies, rs.butterfly);
  }
  std::printf("calendar,key,grid_points,min_gap,k_min_gap,violations\n");
  for (const bool essvi : {true, false}) {
    for (const aadp::svi::CalendarPair& pair : essvi ? fits.essvi.calendar : fits.raw.calendar) {
      const std::string key = std::string(Expiry(pair.earlier)) + "-" + Expiry(pair.later) +
                              (essvi ? ".essvi" : ".raw");
      std::printf("calendar,%s,%zu,%.17g,%.17g,%zu\n", key.c_str(), pair.report.grid_points,
                  pair.report.min_gap, pair.report.k_min_gap, pair.report.violations.size());
      violations(key, pair.report.violations);
    }
  }
  std::printf("constraint,slice,name,slack,active,violated\n");
  for (const aadp::svi::EssviConstraintState& c : r.constraints.states) {
    std::printf("constraint,%s,%s,%.17g,%d,%d\n", Expiry(c.slice),
                aadp::svi::EssviConstraintName(c.constraint), c.slack, c.active ? 1 : 0,
                c.violated ? 1 : 0);
  }
  std::size_t quotes = 0;
  for (const aadp::svi::EssviSliceFit& e : r.slices) quotes += e.quotes;
  std::printf("surface,quotes,essvi_objective\n");
  std::printf("surface,%zu,%.17g\n", quotes, r.objective);
  std::printf(
      "convergence,converged,stop,iterations,gradient,starts_at_best,starts,violated,active\n");
  std::printf("convergence,%d,%s,%zu,%.17g,%zu,%zu,%zu,%zu\n", r.converged ? 1 : 0,
              aadp::svi::EssviStopName(r.stop), r.iterations, r.gradient, r.starts_at_best,
              r.starts.size(), r.constraints.violations, ActiveConstraints(r));
}

int Fit(std::span<const ChainSlice> slices, const Fits& fits) {
  const SurfaceFit& raw = fits.raw;
  const EssviChainFit& essvi = fits.essvi;
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
  PrintFitLines(slices, fits);
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

// Both surfaces, validated, or nothing once the reason is printed.
struct Surfaces {
  ChainSurface raw;
  ChainSurface essvi;
};

std::optional<Surfaces> BuildSurfaces(std::span<const ChainSlice> slices, const Fits& fits) {
  const std::optional<ChainSurface> raw = aadp::svi::SurfaceFromChain(slices, fits.raw);
  const std::optional<ChainSurface> essvi = aadp::svi::SurfaceFromEssvi(slices, fits.essvi);
  if (!raw || !essvi) {
    std::printf("  a fit failed\n");
    return std::nullopt;
  }
  for (const ChainSurface* s : {&*raw, &*essvi}) {
    const aadp::svi::SurfaceError error = aadp::svi::ValidateSurface(s->terms, s->params);
    if (error != aadp::svi::SurfaceError::kNone) {
      std::printf("  surface refused: %s\n", aadp::svi::SurfaceErrorName(error));
      return std::nullopt;
    }
  }
  return Surfaces{.raw = *raw, .essvi = *essvi};
}

// One contract off both surfaces on the same uniforms, and the paired difference.
struct PricePair {
  aadp::svi::NamedContract contract;
  aadp::pricer::LocalVolEstimate raw;
  aadp::pricer::LocalVolEstimate essvi;
  aadp::BatchStatistics paired;
  std::size_t paired_batches = 0;
};

// The pair, or the reason it was refused.
std::expected<PricePair, aadp::pricer::LocalVolFailure> PriceBoth(const aadp::svi::NamedContract& c,
                                                                  const Surfaces& s,
                                                                  const RunConfig& run) {
  const auto pr = aadp::pricer::PriceLocalVol(c.contract, s.raw.terms, s.raw.params, run);
  const auto pe = aadp::pricer::PriceLocalVol(c.contract, s.essvi.terms, s.essvi.params, run);
  if (!pr || !pe) return std::unexpected(!pr ? pr.error() : pe.error());
  std::vector<double> diff;
  diff.reserve(pr->value.batch_means.size());
  for (std::size_t b = 0; b < pr->value.batch_means.size(); ++b) {
    diff.push_back(pe->value.batch_means[b] - pr->value.batch_means[b]);
  }
  return PricePair{.contract = c,
                   .raw = *pr,
                   .essvi = *pe,
                   .paired = aadp::BatchMeansStatistics(diff),
                   .paired_batches = diff.size()};
}

void PrintPriceLines(std::span<const PricePair> pairs, std::size_t segments) {
  std::printf("\n== machine-readable ==\n");
  std::printf(
      "price,contract,raw_mean,raw_se,essvi_mean,essvi_se,paired_mean,paired_se,"
      "paired_batches\n");
  for (const PricePair& p : pairs) {
    std::printf("price,%s,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%zu\n", p.contract.name,
                p.raw.value.mean, p.raw.value.standard_error, p.essvi.value.mean,
                p.essvi.value.standard_error, p.paired.mean, p.paired.standard_error,
                p.paired_batches);
  }
  for (const PricePair& p : pairs) {
    aadp::tools::PrintFloorLines(std::string(p.contract.name) + ".essvi", p.essvi.floors, segments);
    aadp::tools::PrintFloorLines(std::string(p.contract.name) + ".raw", p.raw.floors, segments);
  }
}

int Price(std::span<const ChainSlice> slices, const Fits& fits, const Options& o) {
  const std::optional<Surfaces> surfaces = BuildSurfaces(slices, fits);
  if (!surfaces) return kExitFailure;
  const ChainSurface* raw = &surfaces->raw;
  const ChainSurface* essvi = &surfaces->essvi;
  std::printf(
      "\n== pricing: 7a's contracts off the raw SVI surface (7a's line) and off the eSSVI "
      "surface (second line), same uniforms ==\n");
  PrintSurface("raw SVI", *raw);
  PrintSurface("eSSVI  ", *essvi);
  const RunConfig run{.seed = kSeed, .paths = o.paths, .batches = kBatches, .threads = o.threads};
  const double maturity = raw->terms.back().expiry;
  std::vector<PricePair> pairs;
  for (const aadp::svi::NamedContract& c : aadp::svi::SurfaceContracts(maturity)) {
    const auto pair = PriceBoth(c, *surfaces, run);
    const aadp::pricer::Contract& terms = c.contract.contract;
    std::printf("\n== %s: K %.0f", c.name, terms.strike);
    if (terms.product == aadp::pricer::Product::kBarrier) {
      std::printf(", H %.0f", terms.barrier.level);
    }
    std::printf(", %u dates, n %u, T %.10f ==\n", terms.steps, c.contract.substeps,
                c.contract.maturity);
    if (!pair) {
      std::printf("  REFUSED: %s\n", aadp::pricer::LocalVolErrorName(pair.error().code));
      return kExitFailure;
    }
    const aadp::BatchStatistics& d = pair->paired;
    std::printf("  raw SVI (7a)        %.6f +- %.6f (1 SE)\n", pair->raw.value.mean,
                pair->raw.value.standard_error);
    std::printf("  eSSVI               %.6f +- %.6f (1 SE)\n", pair->essvi.value.mean,
                pair->essvi.value.standard_error);
    std::printf("  paired eSSVI - raw  %+.6f +- %.6f (%+.2f SE), from %zu batch differences\n",
                d.mean, d.standard_error, d.mean / d.standard_error, pair->paired_batches);
    PrintFloors("eSSVI", pair->essvi.floors, essvi->terms.size());
    PrintFloors("raw SVI", pair->raw.floors, raw->terms.size());
    pairs.push_back(*pair);
  }
  PrintPriceLines(pairs, raw->terms.size());
  return 0;
}

double Median(std::vector<double> v) {
  std::ranges::sort(v);
  const std::size_t n = v.size();
  return n % 2 == 1 ? v[n / 2] : 0.5 * (v[(n / 2) - 1] + v[n / 2]);
}

// Wall times in ms of the eSSVI surface fit alone and with every check, per repetition; empty if a
// fit failed.
struct EssviTimes {
  std::vector<double> fit_ms;
  std::vector<double> all_ms;
  double sink = 0.0;
};

std::optional<EssviTimes> TimeEssvi(std::span<const ChainSlice> slices, std::uint32_t repetitions) {
  using Clock = std::chrono::steady_clock;
  EssviTimes times;
  std::vector<aadp::svi::FitSlice> quotes;
  for (const ChainSlice& s : slices) quotes.push_back(s.quotes);
  for (std::uint32_t rep = 0; rep < repetitions; ++rep) {
    const auto t0 = Clock::now();
    const auto fit = aadp::svi::FitEssviSurface(quotes);
    const auto t1 = Clock::now();
    const EssviChainFit chain = aadp::svi::FitChainEssvi(slices);
    const auto t2 = Clock::now();
    if (!fit || !chain.fit) return std::nullopt;
    times.sink += fit->objective + static_cast<double>(chain.fit->constraints.violations);
    times.fit_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    times.all_ms.push_back(std::chrono::duration<double, std::milli>(t2 - t1).count());
  }
  return times;
}

int Timing(std::span<const ChainSlice> slices, const Options& o) {
  std::printf("\n== timing: ITERATION ONLY, not a result (SVI-8, eSSVI surface) ==\n");
  const std::optional<EssviTimes> times = TimeEssvi(slices, o.repetitions);
  if (!times) {
    std::printf("  fit failed in timing run\n");
    return kExitFailure;
  }
  std::printf("  SVI-8 eSSVI fit alone        median %.3f ms, min %.3f ms over %u repetitions\n",
              Median(times->fit_ms), *std::ranges::min_element(times->fit_ms), o.repetitions);
  std::printf(
      "  SVI-8 eSSVI fit and checks   median %.3f ms, min %.3f ms over %u repetitions "
      "(check %.10g)\n",
      Median(times->all_ms), *std::ranges::min_element(times->all_ms), o.repetitions,
      times->sink / o.repetitions);
  return 0;
}

#if AADP_REPORT_MODE
using aadp::results::CellKind;
using aadp::results::real_cell;
using aadp::results::ReportColumn;
using aadp::results::ReportTable;
using aadp::tools::CountCell;

constexpr std::string_view kSvi9 = "SVI-9";

const char* Yes(bool value) { return value ? "yes" : "no"; }

ReportTable Table(std::string name, std::string title, std::string_view target, bool timing,
                  std::vector<ReportColumn> columns) {
  return {.name = std::move(name),
          .title = std::move(title),
          .targets = {std::string(target)},
          .timing = timing,
          .uses_chain = true,
          .columns = std::move(columns),
          .rows = {}};
}

std::vector<ReportColumn> ArbitrageColumns() {
  return {{.name = "model_triples", .kind = CellKind::kInteger, .unit = "count"},
          {.name = "model_non_convex", .kind = CellKind::kInteger, .unit = "count"},
          {.name = "grid_points", .kind = CellKind::kInteger, .unit = "count"},
          {.name = "min_g", .kind = CellKind::kReal, .unit = "none"},
          {.name = "k_min_g", .kind = CellKind::kReal, .unit = "log_moneyness"},
          {.name = "martini_mingone", .kind = CellKind::kText, .unit = "none"},
          {.name = "violations", .kind = CellKind::kInteger, .unit = "count"}};
}

// A violation's grid position: a wing's is +-infinity and an analytic check's may have none, so
// the column is text, holding a real cell's spelling when the position is finite.
std::string Position(double k) {
  if (std::isfinite(k)) return real_cell(k);
  if (std::isnan(k)) return "none";
  return k > 0.0 ? "+inf" : "-inf";
}

void AddViolations(ReportTable& table, const std::string& key,
                   const std::vector<aadp::svi::Violation>& violations) {
  for (std::size_t i = 0; i < violations.size(); ++i) {
    const aadp::svi::Violation& v = violations[i];
    table.rows.push_back({.key = key + "." + std::to_string(i + 1),
                          .cells = {aadp::svi::KindName(v.kind), Position(v.k_begin),
                                    Position(v.k_end), Position(v.k_worst), real_cell(v.value),
                                    real_cell(v.bound), CountCell(v.points)}});
  }
}

void AddArbitrage(ReportTable& arbitrage, ReportTable& violations, const std::string& key,
                  const aadp::ButterflyCounts& model, const aadp::svi::ButterflyReport& b) {
  arbitrage.rows.push_back(
      {.key = key,
       .cells = {CountCell(model.triples), CountCell(model.non_convex), CountCell(b.grid_points),
                 real_cell(b.min_g), real_cell(b.k_min_g),
                 aadp::svi::MmVerdictName(b.classifier.verdict), CountCell(b.violations.size())}});
  AddViolations(violations, key, b.violations);
}

// A table has rows or is not written: a clean surface has no violations.
std::vector<ReportTable> Written(std::vector<ReportTable> tables) {
  std::erase_if(tables, [](const ReportTable& t) { return t.rows.empty(); });
  return tables;
}

// SVI-9's eSSVI side: the cost in fit error, both surfaces' checks and the constraints. The fits
// are those the text prints; a raw slice that did not fit leaves no table to write.
std::optional<std::vector<ReportTable>> FitTables(std::span<const ChainSlice> slices,
                                                  const Fits& fits) {
  if (!fits.essvi.fit) return std::nullopt;
  const EssviFitResult& r = *fits.essvi.fit;
  ReportTable error =
      Table("essvi_fit_error", "eSSVI beside raw SVI per slice: the cost in fit error, settlements",
            kSvi9, false,
            {{.name = "quotes", .kind = CellKind::kInteger, .unit = "count"},
             {.name = "essvi_phi", .kind = CellKind::kReal, .unit = "none"},
             {.name = "essvi_phi_per_dof", .kind = CellKind::kReal, .unit = "none"},
             {.name = "raw_phi", .kind = CellKind::kReal, .unit = "none"},
             {.name = "raw_phi_per_dof", .kind = CellKind::kReal, .unit = "none"},
             {.name = "phi_ratio", .kind = CellKind::kReal, .unit = "none"},
             {.name = "essvi_svi6", .kind = CellKind::kReal, .unit = "bp_of_F"},
             {.name = "raw_svi6", .kind = CellKind::kReal, .unit = "bp_of_F"},
             {.name = "essvi_svi11", .kind = CellKind::kReal, .unit = "vol_points"},
             {.name = "raw_svi11", .kind = CellKind::kReal, .unit = "vol_points"}});
  ReportTable surface =
      Table("essvi_fit_surface", "eSSVI beside raw SVI over the surface: the cost in fit error",
            kSvi9, false,
            {{.name = "quotes", .kind = CellKind::kInteger, .unit = "count"},
             {.name = "essvi_phi", .kind = CellKind::kReal, .unit = "none"},
             {.name = "essvi_phi_per_dof", .kind = CellKind::kReal, .unit = "none"},
             {.name = "raw_phi", .kind = CellKind::kReal, .unit = "none"},
             {.name = "raw_phi_per_dof", .kind = CellKind::kReal, .unit = "none"},
             {.name = "phi_ratio", .kind = CellKind::kReal, .unit = "none"}});
  ReportTable arbitrage =
      Table("essvi_fit_arbitrage", "Butterfly checks of both surfaces' slices, nothing repaired",
            kSvi9, false, ArbitrageColumns());
  ReportTable calendar =
      Table("essvi_fit_calendar", "Calendar checks of both surfaces' adjacent slices", kSvi9, false,
            {{.name = "grid_points", .kind = CellKind::kInteger, .unit = "count"},
             {.name = "min_gap", .kind = CellKind::kReal, .unit = "total_variance"},
             {.name = "k_min_gap", .kind = CellKind::kReal, .unit = "log_moneyness"},
             {.name = "violations", .kind = CellKind::kInteger, .unit = "count"}});
  ReportTable violations = Table(
      "essvi_fit_violations", "Every violation of either surface's slices and pairs", kSvi9, false,
      {{.name = "kind", .kind = CellKind::kText, .unit = "none"},
       {.name = "k_begin", .kind = CellKind::kText, .unit = "log_moneyness"},
       {.name = "k_end", .kind = CellKind::kText, .unit = "log_moneyness"},
       {.name = "k_worst", .kind = CellKind::kText, .unit = "log_moneyness"},
       {.name = "value", .kind = CellKind::kReal, .unit = "none"},
       {.name = "bound", .kind = CellKind::kReal, .unit = "none"},
       {.name = "points", .kind = CellKind::kInteger, .unit = "count"}});
  ReportTable constraints =
      Table("essvi_fit_constraints", "eSSVI constraints B1, B2, N and S on the fitted slices",
            kSvi9, false,
            {{.name = "slack", .kind = CellKind::kReal, .unit = "none"},
             {.name = "active", .kind = CellKind::kText, .unit = "none"},
             {.name = "violated", .kind = CellKind::kText, .unit = "none"}});
  ReportTable convergence =
      Table("essvi_fit_convergence", "eSSVI surface fit: convergence, starts and constraints",
            kSvi9, false,
            {{.name = "converged", .kind = CellKind::kText, .unit = "none"},
             {.name = "stop", .kind = CellKind::kText, .unit = "none"},
             {.name = "iterations", .kind = CellKind::kInteger, .unit = "count"},
             {.name = "scaled_gradient", .kind = CellKind::kReal, .unit = "none"},
             {.name = "starts_at_best", .kind = CellKind::kInteger, .unit = "count"},
             {.name = "starts", .kind = CellKind::kInteger, .unit = "count"},
             {.name = "constraints_violated", .kind = CellKind::kInteger, .unit = "count"},
             {.name = "constraints_active", .kind = CellKind::kInteger, .unit = "count"}});
  double raw_total = 0.0;
  std::size_t quotes = 0;
  for (std::size_t j = 0; j < slices.size(); ++j) {
    const aadp::svi::EssviSliceFit& e = r.slices[j];
    const aadp::svi::EssviChainSlice& ec = fits.essvi.slices[j];
    const aadp::svi::SliceFit& rs = fits.raw.slices[j];
    if (!rs.fit) return std::nullopt;
    const aadp::svi::FitResult& rf = *rs.fit;
    const auto n = static_cast<double>(e.quotes);
    raw_total += rf.objective;
    quotes += e.quotes;
    error.rows.push_back(
        {.key = Expiry(j),
         .cells = {CountCell(e.quotes), real_cell(e.objective), real_cell(e.objective / (n - 3.0)),
                   real_cell(rf.objective), real_cell(rf.objective / (n - 5.0)),
                   real_cell(e.objective / rf.objective), real_cell(ec.svi6_bp),
                   real_cell(rs.svi6_bp), real_cell(ec.svi11_vol_points),
                   real_cell(rs.svi11_vol_points)}});
    AddArbitrage(arbitrage, violations, std::string(Expiry(j)) + ".essvi", ec.model_butterflies,
                 ec.butterfly);
    AddArbitrage(arbitrage, violations, std::string(Expiry(j)) + ".raw", rs.model_butterflies,
                 rs.butterfly);
  }
  const auto n = static_cast<double>(quotes);
  surface.rows.push_back(
      {.key = "surface",
       .cells = {CountCell(quotes), real_cell(r.objective), real_cell(r.objective / (n - 9.0)),
                 real_cell(raw_total), real_cell(raw_total / (n - 15.0)),
                 real_cell(r.objective / raw_total)}});
  for (const bool essvi : {true, false}) {
    for (const aadp::svi::CalendarPair& pair : essvi ? fits.essvi.calendar : fits.raw.calendar) {
      const std::string key = std::string(Expiry(pair.earlier)) + "-" + Expiry(pair.later) +
                              (essvi ? ".essvi" : ".raw");
      calendar.rows.push_back(
          {.key = key,
           .cells = {CountCell(pair.report.grid_points), real_cell(pair.report.min_gap),
                     real_cell(pair.report.k_min_gap), CountCell(pair.report.violations.size())}});
      AddViolations(violations, key, pair.report.violations);
    }
  }
  for (const aadp::svi::EssviConstraintState& c : r.constraints.states) {
    constraints.rows.push_back(
        {.key = std::string(Expiry(c.slice)) + "." + aadp::svi::EssviConstraintName(c.constraint),
         .cells = {real_cell(c.slack), Yes(c.active), Yes(c.violated)}});
  }
  convergence.rows.push_back(
      {.key = "surface",
       .cells = {Yes(r.converged), aadp::svi::EssviStopName(r.stop), CountCell(r.iterations),
                 real_cell(r.gradient), CountCell(r.starts_at_best), CountCell(r.starts.size()),
                 CountCell(r.constraints.violations), CountCell(ActiveConstraints(r))}});
  return Written({std::move(error), std::move(surface), std::move(arbitrage), std::move(calendar),
                  std::move(violations), std::move(constraints), std::move(convergence)});
}

// The second pricing line: 7a's contracts off both surfaces, the paired difference and both floor
// reports, on the paths of the run.
std::vector<ReportTable> PriceTables(std::span<const PricePair> pairs, std::size_t segments,
                                     const RunConfig& run) {
  ReportTable price = Table(
      "essvi_price", "7a's contracts off raw SVI and eSSVI on the same uniforms", kSvi9, false,
      {{.name = "paths", .kind = CellKind::kInteger, .unit = "count"},
       {.name = "raw", .kind = CellKind::kReal, .unit = "index_points"},
       {.name = "raw_se", .kind = CellKind::kReal, .unit = "index_points"},
       {.name = "essvi", .kind = CellKind::kReal, .unit = "index_points"},
       {.name = "essvi_se", .kind = CellKind::kReal, .unit = "index_points"},
       {.name = "paired_difference", .kind = CellKind::kReal, .unit = "index_points"},
       {.name = "paired_se", .kind = CellKind::kReal, .unit = "index_points"},
       {.name = "paired_batches", .kind = CellKind::kInteger, .unit = "count"}});
  ReportTable floors =
      Table("essvi_price_floors", "Local-variance floors and caps per contract and surface", kSvi9,
            false, aadp::tools::FloorColumns());
  ReportTable ranges =
      Table("essvi_price_floor_ranges", "Where the floors and caps fell, in k and t, by segment",
            kSvi9, false, aadp::tools::RangeColumns());
  for (const PricePair& p : pairs) {
    price.rows.push_back(
        {.key = p.contract.name,
         .cells = {CountCell(run.paths), real_cell(p.raw.value.mean),
                   real_cell(p.raw.value.standard_error), real_cell(p.essvi.value.mean),
                   real_cell(p.essvi.value.standard_error), real_cell(p.paired.mean),
                   real_cell(p.paired.standard_error), CountCell(p.paired_batches)}});
    for (const bool essvi : {true, false}) {
      const std::string key = std::string(p.contract.name) + (essvi ? ".essvi" : ".raw");
      const FloorTally& tally = essvi ? p.essvi.floors : p.raw.floors;
      floors.rows.push_back(aadp::tools::FloorRow(key, tally));
      aadp::tools::AddRangeRows(ranges, key, tally, segments);
    }
  }
  return Written({std::move(price), std::move(floors), std::move(ranges)});
}

std::vector<ReportTable> TimingTables(const EssviTimes& times, std::uint32_t repetitions) {
  ReportTable timing =
      Table("essvi_fit_time", "eSSVI surface fit time, alone and with every check, one thread",
            "SVI-8", true,
            {{.name = "median", .kind = CellKind::kReal, .unit = "ms"},
             {.name = "min", .kind = CellKind::kReal, .unit = "ms"},
             {.name = "repetitions", .kind = CellKind::kInteger, .unit = "count"}});
  const auto add = [&timing, repetitions](const char* key, const std::vector<double>& ms) {
    timing.rows.push_back(
        {.key = key,
         .cells = {real_cell(Median(ms)), real_cell(*std::ranges::min_element(ms)),
                   CountCell(repetitions)}});
  };
  add("fit", times.fit_ms);
  add("fit_and_checks", times.all_ms);
  return {std::move(timing)};
}

const char* ModeName(Mode mode) {
  switch (mode) {
    case Mode::kFit:
      return "fit";
    case Mode::kPrice:
      return "price";
    case Mode::kTiming:
      return "timing";
  }
  return "unknown";
}

int ReportMain(std::span<const ChainSlice> slices, const Options& o,
               aadp::results::ReportRun& session) {
  const RunConfig run{.seed = kSeed, .paths = o.paths, .batches = kBatches, .threads = o.threads};
  std::vector<ReportTable> tables;
  std::vector<aadp::results::HeaderField> header = {
      {.key = "report_tool",
       .value = std::string("aadp_essvi_report --report --mode ") + ModeName(o.mode)},
      {.key = "report_inputs",
       .value = "TAIFEX TXO and TX daily settlement prices of trade date 2024-11-15, not "
                "tradable quotes; expiries 202411, 202411W4 and 202412, so maturity reaches 33 "
                "days only"}};
  std::int64_t repetitions = 1;
  if (o.mode == Mode::kTiming) {
    const std::optional<EssviTimes> times = TimeEssvi(slices, o.repetitions);
    if (!times) {
      std::cerr << "aadp_essvi_report: a fit failed in the timed repetitions\n";
      return kExitFailure;
    }
    tables = TimingTables(*times, o.repetitions);
    repetitions = o.repetitions;
    header.push_back({.key = "report_timing",
                      .value = "the eSSVI surface fit alone, then the chain's fit with every "
                               "check, steady_clock, one thread, " +
                               std::to_string(o.repetitions) + " repetitions"});
  } else {
    const Fits fits{.raw = aadp::svi::FitChain(slices), .essvi = aadp::svi::FitChainEssvi(slices)};
    if (o.mode == Mode::kFit) {
      auto fit_tables = FitTables(slices, fits);
      if (!fit_tables) {
        std::cerr << "aadp_essvi_report: a fit failed; a report needs every slice of both\n";
        return kExitFailure;
      }
      tables = std::move(*fit_tables);
    } else {
      const std::optional<Surfaces> surfaces = BuildSurfaces(slices, fits);
      if (!surfaces) return kExitFailure;
      std::vector<PricePair> pairs;
      for (const aadp::svi::NamedContract& c :
           aadp::svi::SurfaceContracts(surfaces->raw.terms.back().expiry)) {
        const auto pair = PriceBoth(c, *surfaces, run);
        if (!pair) {
          std::cerr << "aadp_essvi_report: " << c.name
                    << " refused: " << aadp::pricer::LocalVolErrorName(pair.error().code) << '\n';
          return kExitFailure;
        }
        pairs.push_back(*pair);
      }
      tables = PriceTables(pairs, surfaces->raw.terms.size(), run);
      header.push_back({.key = "report_run",
                        .value = "seed " + std::to_string(kSeed) + ", paths " +
                                 std::to_string(o.paths) + ", batches " + std::to_string(kBatches) +
                                 ", threads " + std::to_string(o.threads) + ", n 4, 33 dates"});
    }
  }
  const aadp::results::ReportShape shape{.run = {.claim = "result",
                                                 .repetitions = repetitions,
                                                 .batches_per_repetition = 1,
                                                 .warmup = "none"},
                                         .header = std::move(header)};
  auto text = session.finish(shape, tables, aadp::attribution_text());
  if (!text) {
    std::cerr << "aadp_essvi_report: " << text.error().message << '\n';
    return text.error().exit_code;
  }
  std::cout << *text;
  std::cout.flush();
  return std::cout ? 0 : kExitFailure;
}
#endif

// The calibration slices from the two captures, or nothing once the reason is printed.
std::optional<std::vector<ChainSlice>> ReadSlices(const Options& o) {
  const auto option_bytes = aadp::taifex::read_binary_file(o.options_path);
  const auto future_bytes = aadp::taifex::read_binary_file(o.futures_path);
  if (!option_bytes || !future_bytes) {
    std::cerr << "aadp_essvi_report: "
              << (option_bytes ? future_bytes.error() : option_bytes.error())
              << ". Run scripts/fetch_data.sh.\n";
    return std::nullopt;
  }
  const auto options = aadp::taifex::read_options(*option_bytes);
  const auto futures = aadp::taifex::read_futures(*future_bytes);
  if (!options || !futures) {
    std::cerr << "aadp_essvi_report: "
              << aadp::taifex::to_string(options ? futures.error().code : options.error().code)
              << '\n';
    return std::nullopt;
  }
  auto slices =
      aadp::svi::BuildChainSlices(options->rows, futures->rows, aadp::svi::kCalibrationExpiries);
  if (!slices) {
    std::cerr << "aadp_essvi_report: " << aadp::svi::ChainErrorName(slices.error().code) << " on "
              << slices.error().expiry << '\n';
    return std::nullopt;
  }
  return std::move(*slices);
}

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
      std::cerr << "aadp_essvi_report: " << begun.error().message << '\n';
      return begun.error().exit_code;
    }
    session.emplace(std::move(*begun));
  }
#else
  if (o.report) {
    std::cerr << "aadp_essvi_report: built without the report mode\n";
    return kExitUsage;
  }
#endif
  // The attribution comes first, before any output that the chain could have shaped. A report
  // file carries it as its first section instead.
  if (!o.report) Header(o);
  const auto slices = ReadSlices(o);
  if (!slices) return kExitFailure;
#if AADP_REPORT_MODE
  if (o.report) return ReportMain(*slices, o, *session);
#endif
  switch (o.mode) {
    case Mode::kFit:
      return Fit(*slices, Fits{.raw = aadp::svi::FitChain(*slices),
                               .essvi = aadp::svi::FitChainEssvi(*slices)});
    case Mode::kPrice:
      return Price(
          *slices,
          Fits{.raw = aadp::svi::FitChain(*slices), .essvi = aadp::svi::FitChainEssvi(*slices)}, o);
    case Mode::kTiming:
      return Timing(*slices, o);
  }
  return kExitFailure;
} catch (const std::exception& error) {
  std::fprintf(stderr, "aadp_essvi_report: %s\n", error.what());
  return kExitFailure;
}
