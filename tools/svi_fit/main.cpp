// The raw SVI fit report on the TAIFEX captures (docs/NOTES.md, "Stage 6b").
//
//   aadp_svi_fit [--options <file>] [--futures <file>] [--timing <repetitions>]
//   aadp_svi_fit --report [--options <file>] [--futures <file>] [--timing <repetitions>]
//                [run flags]
//
// Per slice: F, D, T, sigma_leg and the quotes, the fitted parameters with every bound's slack and
// the active ones, the weighted objective, SVI-6 and SVI-11, the input butterfly counts beside the
// fitted slice's violations, the calendar check between adjacent fitted slices and the
// Martini-Mingone class. The output is derived from the chain, so it opens with the attribution.
// --timing prints wall times instead, iteration only and not a result; the report has no timing.
//
// --report writes a report file instead (src/results/report.hpp), for scripts/run_bench.sh and
// the stamp: the same fit's SVI-6, SVI-11 and SVI-9 figures, the same doubles as the text's
// machine-readable lines, and SVI-7's per-slice fit time over --timing repetitions (default 50)
// as a timed section. Derived figures only: no settlement and no per-strike row. Every section is
// derived from the chain, so the file carries the attribution. The run flags are those of
// src/results/run_header.hpp, and only --report takes them.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <ratio>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "build_info.hpp"
#include "data/attribution.hpp"
#include "data/calibration_set.hpp"
#include "data/forwards.hpp"
#include "data/taifex_csv.hpp"
#include "data/taifex_futures.hpp"
#include "data/taifex_options.hpp"
#include "svi/arbitrage.hpp"
#include "svi/chain_fit.hpp"
#include "svi/fit.hpp"
#include "svi/implied_vol.hpp"
#if AADP_SVI_FIT_REPORT
#include "results/report.hpp"
#include "results/report_run.hpp"
#include "results/run_header.hpp"
#endif

namespace {

using aadp::svi::ChainSlice;
using aadp::svi::FitResult;
using aadp::svi::SliceFit;

constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;
constexpr int kReportRepetitions = 50;

int Usage() {
  std::cerr << "usage: aadp_svi_fit [--options <file>] [--futures <file>] [--timing <n>]\n"
               "       aadp_svi_fit --report [--options <file>] [--futures <file>] [--timing <n>]"
               " [run flags]\n";
  return kExitUsage;
}

// A positive count, or 0 for anything else.
int Repetitions(std::string_view text) {
  int value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc() && end == text.data() + text.size() && value > 0 ? value : 0;
}

void Header() {
  aadp::write_attribution(std::cout);
  std::cout
      << "\naadp_svi_fit: raw SVI fit, one slice per expiry, to TAIFEX TXO settlements of "
         "trade date 2024-11-15.\n"
         "These are daily settlement prices set by the exchange's rules, not tradable "
         "quotes.\n"
         "Maturity reaches 33 days only: expiries 202411, 202411W4 and 202412 (5, 12 and 33 "
         "days).\n"
         "Method, weights, bounds and metrics: docs/NOTES.md, \"Stage 6b\". Nothing is repaired.\n";
  std::cout << "compiler: " << aadp::build_info::compiler_id << ' '
            << aadp::build_info::compiler_version << ", " << aadp::build_info::build_type << ", "
            << aadp::build_info::cxx_flags << '\n';
  std::cout.flush();
}

std::string Active(const FitResult& fit) {
  std::string out;
  for (const aadp::svi::Bound bound : fit.ActiveBounds()) {
    out += out.empty() ? "" : ", ";
    out += aadp::svi::BoundName(bound);
  }
  return out.empty() ? "none" : out;
}

void PrintViolations(const std::vector<aadp::svi::Violation>& violations) {
  if (violations.empty()) {
    std::printf("  violations          none\n");
    return;
  }
  std::printf("  violations          %zu\n", violations.size());
  for (const aadp::svi::Violation& v : violations) {
    std::printf("    %-20s k [%.6g, %.6g], worst k %.6g, value %.6g, bound %.6g, points %zu\n",
                aadp::svi::KindName(v.kind), v.k_begin, v.k_end, v.k_worst, v.value, v.bound,
                v.points);
  }
}

void PrintFit(const ChainSlice& slice, const SliceFit& result) {
  if (!result.fit) {
    std::printf("  fit                 FAILED: %s, quote %zu, inversion %s\n",
                aadp::svi::FitErrorName(result.fit.error().code), result.fit.error().quote,
                aadp::svi::StatusName(result.fit.error().status));
    return;
  }
  const FitResult& fit = *result.fit;
  const aadp::svi::SviParams& q = fit.params;
  std::printf("  parameters          a %.10g  b %.10g  rho %.10g  m %.10g  sigma %.10g\n", q.a, q.b,
              q.rho, q.m, q.sigma);
  std::printf("  active bounds       %s\n", Active(fit).c_str());
  std::printf("  slacks             ");
  for (const aadp::svi::BoundState& b : fit.bounds) {
    std::printf(" %s %.3g;", aadp::svi::BoundName(b.bound), b.slack);
  }
  std::printf("\n  box                 m [%.6g, %.6g], sigma [%.3g, %.3g], a_max %.6g\n", fit.m_lo,
              fit.m_hi, fit.sigma_lo, fit.sigma_hi, fit.a_max);
  std::printf(
      "  objective           Phi %.6g, Phi/(n-5) %.6g, converged %s, starts %zu, at best "
      "%zu, profile evaluations %zu\n",
      fit.objective, fit.objective / static_cast<double>(fit.quotes - 5),
      fit.converged ? "yes" : "NO", fit.starts, fit.starts_at_best, fit.profile_evaluations);
  std::printf("  SVI-6               %.4f bp of F, unweighted (context: 4 bp, eSSVI on SPX mids)\n",
              result.svi6_bp);
  std::printf("  SVI-11              %.4f vol points, unweighted (context: 0.73)\n",
              result.svi11_vol_points);
  const aadp::ButterflyCounts& in = slice.input_butterflies;
  const aadp::ButterflyCounts& model = result.model_butterflies;
  std::printf(
      "  input butterflies   triples %zu, non-convex %zu, beyond half a tick %zu "
      "(settlements)\n",
      in.triples, in.non_convex, in.beyond_half_tick);
  std::printf(
      "  model butterflies   triples %zu, non-convex %zu (model prices at the quoted "
      "strikes)\n",
      model.triples, model.non_convex);
  std::printf(
      "  fitted-slice checks butterfly grid %zu points, min g %.6g at k %.6g; "
      "Martini-Mingone: %s\n",
      result.butterfly.grid_points, result.butterfly.min_g, result.butterfly.k_min_g,
      aadp::svi::MmVerdictName(result.butterfly.classifier.verdict));
  PrintViolations(result.butterfly.violations);
}

void PrintSlice(const ChainSlice& slice, const SliceFit& result) {
  const aadp::ParityFit& p = slice.parity;
  std::size_t puts = 0;
  for (const auto& row : slice.rows.rows) {
    puts += row.right == aadp::taifex::OptionRight::kPut ? 1 : 0;
  }
  std::printf("\n== slice %s ==\n", slice.expiry.c_str());
  std::printf("  T                   %.10f years (ACT/365, seconds)\n", slice.years);
  std::printf("  F                   %.6f (se %.4f), settle_all parity, %zu strikes\n", p.forward,
              p.forward_se, p.points);
  if (slice.tx_settlement) {
    std::printf("  TX settlement       %.2f (F - TX %+.4f)\n", *slice.tx_settlement,
                p.forward - *slice.tx_settlement);
  } else {
    std::printf("  TX settlement       none (no TX month)\n");
  }
  std::printf("  D                   %.8f (se %.2g)\n", p.discount, p.discount_se);
  std::printf("  sigma_leg           %.4f points (residual RMS %.4f / sqrt 2)\n", p.sigma_leg,
              p.rms_residual);
  double k_min = 0.0;
  double k_max = 0.0;
  if (!slice.quotes.quotes.empty()) {
    k_min = std::log(slice.quotes.quotes.front().strike / p.forward);
    k_max = k_min;
    for (const auto& q : slice.quotes.quotes) {
      k_min = std::min(k_min, std::log(q.strike / p.forward));
      k_max = std::max(k_max, std::log(q.strike / p.forward));
    }
  }
  std::printf("  quotes              %zu (%zu puts, %zu calls), k from %.5f to %.5f\n",
              slice.quotes.quotes.size(), puts, slice.quotes.quotes.size() - puts, k_min, k_max);
  PrintFit(slice, result);
}

void PrintCalendar(std::span<const ChainSlice> slices, const aadp::svi::SurfaceFit& surface) {
  std::printf("\n== calendar, adjacent fitted slices ==\n");
  if (surface.calendar.empty()) std::printf("  none compared\n");
  for (const aadp::svi::CalendarPair& pair : surface.calendar) {
    std::printf("  %s -> %s: grid %zu points, min gap %.6g at k %.6g\n",
                slices[pair.earlier].expiry.c_str(), slices[pair.later].expiry.c_str(),
                pair.report.grid_points, pair.report.min_gap, pair.report.k_min_gap);
    PrintViolations(pair.report.violations);
  }
}

// Full precision, for comparing hosts.
void PrintMachineReadable(std::span<const ChainSlice> slices,
                          const aadp::svi::SurfaceFit& surface) {
  std::printf("\n== machine-readable ==\n");
  std::printf(
      "fit,expiry,T,F,D,sigma_leg,n,a,b,rho,m,sigma,objective,svi6_bp,svi11_vol_points,"
      "converged,active\n");
  for (std::size_t i = 0; i < slices.size(); ++i) {
    const ChainSlice& s = slices[i];
    const SliceFit& r = surface.slices[i];
    if (!r.fit) {
      std::printf("fit,%s,failed\n", s.expiry.c_str());
      continue;
    }
    const aadp::svi::SviParams& q = r.fit->params;
    std::printf(
        "fit,%s,%.17g,%.17g,%.17g,%.17g,%zu,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,"
        "%d,%s\n",
        s.expiry.c_str(), s.years, s.parity.forward, s.parity.discount, s.parity.sigma_leg,
        r.fit->quotes, q.a, q.b, q.rho, q.m, q.sigma, r.fit->objective, r.svi6_bp,
        r.svi11_vol_points, r.fit->converged ? 1 : 0, Active(*r.fit).c_str());
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
  std::printf(
      "arbitrage,expiry,model_triples,model_non_convex,grid_points,min_g,k_min_g,"
      "martini_mingone,violations\n");
  for (std::size_t i = 0; i < slices.size(); ++i) {
    const SliceFit& r = surface.slices[i];
    if (!r.fit) continue;
    std::printf("arbitrage,%s,%zu,%zu,%zu,%.17g,%.17g,%s,%zu\n", slices[i].expiry.c_str(),
                r.model_butterflies.triples, r.model_butterflies.non_convex,
                r.butterfly.grid_points, r.butterfly.min_g, r.butterfly.k_min_g,
                aadp::svi::MmVerdictName(r.butterfly.classifier.verdict),
                r.butterfly.violations.size());
    violations(slices[i].expiry, r.butterfly.violations);
  }
  std::printf("calendar,earlier,later,grid_points,min_gap,k_min_gap,violations\n");
  for (const aadp::svi::CalendarPair& pair : surface.calendar) {
    std::printf("calendar,%s,%s,%zu,%.17g,%.17g,%zu\n", slices[pair.earlier].expiry.c_str(),
                slices[pair.later].expiry.c_str(), pair.report.grid_points, pair.report.min_gap,
                pair.report.k_min_gap, pair.report.violations.size());
    violations(slices[pair.earlier].expiry + "-" + slices[pair.later].expiry,
               pair.report.violations);
  }
}

double Median(std::vector<double> v) {
  std::ranges::sort(v);
  const std::size_t n = v.size();
  return n % 2 == 1 ? v[n / 2] : 0.5 * (v[(n / 2) - 1] + v[n / 2]);
}

// Wall times in ms of each slice's fit (SVI-7) and, if asked, of the whole surface with every
// check (SVI-8), per repetition. The text's timing and the report's timed section both read these.
struct FitTimes {
  std::vector<std::vector<double>> per_slice;
  std::vector<double> surface;
  bool all_fitted = true;
};

FitTimes TimeFits(std::span<const ChainSlice> slices, int repetitions, bool with_surface) {
  using Clock = std::chrono::steady_clock;
  FitTimes times;
  times.per_slice.resize(slices.size());
  for (int rep = 0; rep < repetitions; ++rep) {
    for (std::size_t i = 0; i < slices.size(); ++i) {
      const auto t0 = Clock::now();
      const auto fit = aadp::svi::FitRawSvi(slices[i].quotes);
      const auto t1 = Clock::now();
      times.all_fitted = times.all_fitted && fit.has_value();
      times.per_slice[i].push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    if (!with_surface) continue;
    const auto t0 = Clock::now();
    const aadp::svi::SurfaceFit all = aadp::svi::FitChain(slices);
    const auto t1 = Clock::now();
    times.all_fitted = times.all_fitted && all.slices.size() == slices.size();
    times.surface.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
  }
  return times;
}

void Timing(std::span<const ChainSlice> slices, int repetitions) {
  std::printf("\n== timing: ITERATION ONLY, not a result (SVI-7 per slice, SVI-8 surface) ==\n");
  const FitTimes times = TimeFits(slices, repetitions, true);
  const std::vector<std::vector<double>>& per_slice = times.per_slice;
  const std::vector<double>& surface = times.surface;
  if (!times.all_fitted) std::printf("  fit failed in timing run\n");
  for (std::size_t i = 0; i < slices.size(); ++i) {
    std::printf("  SVI-7 %s: fit median %.3f ms, min %.3f ms over %d repetitions\n",
                slices[i].expiry.c_str(), Median(per_slice[i]),
                *std::ranges::min_element(per_slice[i]), repetitions);
  }
  std::printf(
      "  SVI-8 surface (3 fits and every check): median %.3f ms, min %.3f ms over %d "
      "repetitions\n",
      Median(surface), *std::ranges::min_element(surface), repetitions);
}

#if AADP_SVI_FIT_REPORT
using aadp::results::CellKind;
using aadp::results::integer_cell;
using aadp::results::real_cell;
using aadp::results::ReportColumn;
using aadp::results::ReportTable;

std::string Count(std::size_t n) { return integer_cell(static_cast<std::int64_t>(n)); }

// A violation's grid position: a wing's is +-infinity and an analytic check's may have none, so
// the column is text, holding a real cell's spelling when the position is finite.
std::string Position(double k) {
  if (std::isfinite(k)) return real_cell(k);
  if (std::isnan(k)) return "none";
  return k > 0.0 ? "+inf" : "-inf";
}

ReportTable Table(std::string name, std::string title, std::vector<std::string> targets,
                  bool timing, std::vector<ReportColumn> columns) {
  return {.name = std::move(name),
          .title = std::move(title),
          .targets = std::move(targets),
          .timing = timing,
          .uses_chain = true,
          .columns = std::move(columns),
          .rows = {}};
}

void AddViolations(ReportTable& table, const std::string& where,
                   const std::vector<aadp::svi::Violation>& violations) {
  for (std::size_t i = 0; i < violations.size(); ++i) {
    const aadp::svi::Violation& v = violations[i];
    table.rows.push_back(
        {.key = where + "." + std::to_string(i + 1),
         .cells = {aadp::svi::KindName(v.kind), Position(v.k_begin), Position(v.k_end),
                   Position(v.k_worst), real_cell(v.value), real_cell(v.bound), Count(v.points)}});
  }
}

// SVI-6, SVI-11 and SVI-9's fit side from one fit of the chain, and SVI-7 from the timing.
std::vector<ReportTable> ReportTables(std::span<const ChainSlice> slices,
                                      const aadp::svi::SurfaceFit& surface, const FitTimes& times) {
  ReportTable error =
      Table("svi_fit_error", "Raw SVI fit error per slice, against settlements",
            {"SVI-6", "SVI-11"}, false,
            {{.name = "quotes", .kind = CellKind::kInteger, .unit = "count"},
             {.name = "svi6", .kind = CellKind::kReal, .unit = "bp_of_F"},
             {.name = "svi11", .kind = CellKind::kReal, .unit = "vol_points"},
             {.name = "objective_per_dof", .kind = CellKind::kReal, .unit = "none"}});
  ReportTable arbitrage = Table(
      "svi_fit_arbitrage", "Fitted slices: butterfly checks, nothing repaired", {"SVI-9"}, false,
      {{.name = "model_triples", .kind = CellKind::kInteger, .unit = "count"},
       {.name = "model_non_convex", .kind = CellKind::kInteger, .unit = "count"},
       {.name = "grid_points", .kind = CellKind::kInteger, .unit = "count"},
       {.name = "min_g", .kind = CellKind::kReal, .unit = "none"},
       {.name = "k_min_g", .kind = CellKind::kReal, .unit = "log_moneyness"},
       {.name = "martini_mingone", .kind = CellKind::kText, .unit = "none"},
       {.name = "violations", .kind = CellKind::kInteger, .unit = "count"}});
  ReportTable calendar =
      Table("svi_fit_calendar", "Adjacent fitted slices: calendar checks, nothing repaired",
            {"SVI-9"}, false,
            {{.name = "grid_points", .kind = CellKind::kInteger, .unit = "count"},
             {.name = "min_gap", .kind = CellKind::kReal, .unit = "total_variance"},
             {.name = "k_min_gap", .kind = CellKind::kReal, .unit = "log_moneyness"},
             {.name = "violations", .kind = CellKind::kInteger, .unit = "count"}});
  ReportTable violations = Table(
      "svi_fit_violations", "Every violation of the fitted slices and pairs", {"SVI-9"}, false,
      {{.name = "kind", .kind = CellKind::kText, .unit = "none"},
       {.name = "k_begin", .kind = CellKind::kText, .unit = "log_moneyness"},
       {.name = "k_end", .kind = CellKind::kText, .unit = "log_moneyness"},
       {.name = "k_worst", .kind = CellKind::kText, .unit = "log_moneyness"},
       {.name = "value", .kind = CellKind::kReal, .unit = "none"},
       {.name = "bound", .kind = CellKind::kReal, .unit = "none"},
       {.name = "points", .kind = CellKind::kInteger, .unit = "count"}});
  ReportTable timing =
      Table("svi_fit_time", "Raw SVI fit time per slice, one thread", {"SVI-7"}, true,
            {{.name = "median", .kind = CellKind::kReal, .unit = "ms"},
             {.name = "min", .kind = CellKind::kReal, .unit = "ms"},
             {.name = "repetitions", .kind = CellKind::kInteger, .unit = "count"}});
  for (std::size_t i = 0; i < slices.size(); ++i) {
    const SliceFit& r = surface.slices[i];
    const FitResult& fit = *r.fit;
    const std::string& expiry = slices[i].expiry;
    error.rows.push_back(
        {.key = expiry,
         .cells = {Count(fit.quotes), real_cell(r.svi6_bp), real_cell(r.svi11_vol_points),
                   real_cell(fit.objective / static_cast<double>(fit.quotes - 5))}});
    arbitrage.rows.push_back(
        {.key = expiry,
         .cells = {Count(r.model_butterflies.triples), Count(r.model_butterflies.non_convex),
                   Count(r.butterfly.grid_points), real_cell(r.butterfly.min_g),
                   real_cell(r.butterfly.k_min_g),
                   aadp::svi::MmVerdictName(r.butterfly.classifier.verdict),
                   Count(r.butterfly.violations.size())}});
    AddViolations(violations, expiry, r.butterfly.violations);
    const std::vector<double>& ms = times.per_slice[i];
    timing.rows.push_back({.key = expiry,
                           .cells = {real_cell(Median(ms)),
                                     real_cell(*std::ranges::min_element(ms)), Count(ms.size())}});
  }
  for (const aadp::svi::CalendarPair& pair : surface.calendar) {
    const std::string key = slices[pair.earlier].expiry + "-" + slices[pair.later].expiry;
    calendar.rows.push_back(
        {.key = key,
         .cells = {Count(pair.report.grid_points), real_cell(pair.report.min_gap),
                   real_cell(pair.report.k_min_gap), Count(pair.report.violations.size())}});
    AddViolations(violations, key, pair.report.violations);
  }
  // A table has rows or is not written: one slice has no calendar, a clean fit no violations.
  std::vector<ReportTable> tables;
  for (ReportTable* table : {&error, &arbitrage, &calendar, &violations, &timing}) {
    if (!table->rows.empty()) tables.push_back(std::move(*table));
  }
  return tables;
}

int ReportMain(std::span<const ChainSlice> slices, aadp::results::ReportRun& session,
               int repetitions) {
  const aadp::svi::SurfaceFit surface = aadp::svi::FitChain(slices);
  for (std::size_t i = 0; i < slices.size(); ++i) {
    if (!surface.slices[i].fit) {
      std::cerr << "aadp_svi_fit: the fit of " << slices[i].expiry
                << " failed: " << aadp::svi::FitErrorName(surface.slices[i].fit.error().code)
                << "; a report needs every slice\n";
      return kExitFailure;
    }
  }
  const FitTimes times = TimeFits(slices, repetitions, false);
  if (!times.all_fitted) {
    std::cerr << "aadp_svi_fit: a fit failed in the timed repetitions\n";
    return kExitFailure;
  }
  const std::vector<ReportTable> tables = ReportTables(slices, surface, times);
  const aadp::results::ReportShape shape{
      .run = {.claim = "result",
              .repetitions = repetitions,
              .batches_per_repetition = 1,
              .warmup = "none"},
      .header = {{.key = "report_tool", .value = "aadp_svi_fit --report"},
                 {.key = "report_inputs",
                  .value = "TAIFEX TXO and TX daily settlement prices of trade date 2024-11-15, "
                           "not tradable quotes; expiries 202411, 202411W4 and 202412, so "
                           "maturity reaches 33 days only"},
                 {.key = "report_timing",
                  .value = "per slice, the fit alone, steady_clock, one thread, " +
                           std::to_string(repetitions) + " repetitions after the untimed fit"}}};
  auto text = session.finish(shape, tables, aadp::attribution_text());
  if (!text) {
    std::cerr << "aadp_svi_fit: " << text.error().message << '\n';
    return text.error().exit_code;
  }
  std::cout << *text;
  std::cout.flush();
  return std::cout ? 0 : kExitFailure;
}
#endif

struct Args {
  std::string options_path = "data/DailyMarketReportOpt.csv";
  std::string futures_path = "data/DailyMarketReportFut.csv";
  int timing = 0;
  bool report = false;
  bool run_flags = false;  // any of the run flags of src/results/run_header.hpp
#if AADP_SVI_FIT_REPORT
  aadp::results::RunOptions run;
#endif
};

// The flags, run flags included; false for a usage error.
bool ParseArgs(int argc, char* const* argv, Args& out) {
  std::vector<std::string> args;
#if AADP_SVI_FIT_REPORT
  auto rest = aadp::results::take_run_flags(argc, argv, out.run);
  if (!rest) {
    std::cerr << "aadp_svi_fit: " << rest.error() << '\n';
    return false;
  }
  args = std::move(*rest);
  out.run_flags = args.size() + 1 != static_cast<std::size_t>(argc);
#else
  const std::span<char* const> all(argv, static_cast<std::size_t>(argc));
  args.assign(all.begin() + 1, all.end());
#endif
  std::erase_if(args, [&out](const std::string& arg) {
    const bool is_report = arg == "--report";
    out.report = out.report || is_report;
    return is_report;
  });
  for (std::size_t i = 0; i < args.size(); i += 2) {
    const std::string_view flag = args[i];
    if (i + 1 == args.size()) return false;
    if (flag == "--options") {
      out.options_path = args[i + 1];
    } else if (flag == "--futures") {
      out.futures_path = args[i + 1];
    } else if (flag == "--timing") {
      out.timing = Repetitions(args[i + 1]);
      if (out.timing < 1) return false;
    } else {
      return false;
    }
  }
  return true;
}

// The calibration slices from the two captures, or nothing once the reason is printed.
std::optional<std::vector<ChainSlice>> ReadSlices(const Args& args) {
  const auto option_bytes = aadp::taifex::read_binary_file(args.options_path);
  const auto future_bytes = aadp::taifex::read_binary_file(args.futures_path);
  if (!option_bytes || !future_bytes) {
    std::cerr << "aadp_svi_fit: " << (option_bytes ? future_bytes.error() : option_bytes.error())
              << ". Run scripts/fetch_data.sh.\n";
    return std::nullopt;
  }
  const auto options = aadp::taifex::read_options(*option_bytes);
  const auto futures = aadp::taifex::read_futures(*future_bytes);
  if (!options || !futures) {
    std::cerr << "aadp_svi_fit: "
              << aadp::taifex::to_string(options ? futures.error().code : options.error().code)
              << '\n';
    return std::nullopt;
  }
  auto slices =
      aadp::svi::BuildChainSlices(options->rows, futures->rows, aadp::svi::kCalibrationExpiries);
  if (!slices) {
    std::cerr << "aadp_svi_fit: " << aadp::svi::ChainErrorName(slices.error().code) << " on "
              << slices.error().expiry << '\n';
    return std::nullopt;
  }
  return std::move(*slices);
}

}  // namespace

// NOLINTNEXTLINE(misc-const-correctness): the standard fixes the signature of main.
int main(int argc, char** argv) try {
  Args parsed;
  if (!ParseArgs(argc, argv, parsed)) return Usage();
  const bool report = parsed.report;
  const int timing = parsed.timing;
  // The run flags belong to the report file's header, which the text report does not have.
  if (parsed.run_flags && !report) return Usage();
#if AADP_SVI_FIT_REPORT
  // The run's refusals come before any work, as a benchmark binary's do.
  std::optional<aadp::results::ReportRun> session;
  if (report) {
    auto begun = aadp::results::ReportRun::begin(parsed.run);
    if (!begun) {
      std::cerr << "aadp_svi_fit: " << begun.error().message << '\n';
      return begun.error().exit_code;
    }
    session.emplace(std::move(*begun));
  }
#else
  if (report) {
    std::cerr << "aadp_svi_fit: built without the report mode\n";
    return kExitUsage;
  }
#endif

  // The attribution comes first, before any output that the chain could have shaped. A report
  // file carries it as its first section instead.
  if (!report) Header();
  const auto slices = ReadSlices(parsed);
  if (!slices) return kExitFailure;
#if AADP_SVI_FIT_REPORT
  if (report) return ReportMain(*slices, *session, timing > 0 ? timing : kReportRepetitions);
#endif
  if (timing > 0) {
    Timing(*slices, timing);
    return 0;
  }
  const aadp::svi::SurfaceFit surface = aadp::svi::FitChain(*slices);
  for (std::size_t i = 0; i < slices->size(); ++i) PrintSlice((*slices)[i], surface.slices[i]);
  PrintCalendar(*slices, surface);
  PrintMachineReadable(*slices, surface);
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "aadp_svi_fit: %s\n", error.what());
  return kExitFailure;
}
