// The raw SVI fit report on the TAIFEX captures (NOTES.md, "Stage 6b").
//
//   aadp_svi_fit [--options <file>] [--futures <file>] [--timing <repetitions>]
//
// Per slice: F, D, T, sigma_leg and the quotes, the fitted parameters with every bound's slack and
// the active ones, the weighted objective, SVI-6 and SVI-11, the input butterfly counts beside the
// fitted slice's violations, the calendar check between adjacent fitted slices and the
// Martini-Mingone class. The output is derived from the chain, so it opens with the attribution.
// --timing prints wall times instead, iteration only and not a result; the report has no timing.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <iostream>
#include <ratio>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
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

namespace {

using aadp::svi::ChainSlice;
using aadp::svi::FitResult;
using aadp::svi::SliceFit;

constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

int Usage() {
  std::cerr << "usage: aadp_svi_fit [--options <file>] [--futures <file>] [--timing <n>]\n";
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
         "Method, weights, bounds and metrics: NOTES.md, \"Stage 6b\". Nothing is repaired.\n";
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
}

double Median(std::vector<double> v) {
  std::ranges::sort(v);
  const std::size_t n = v.size();
  return n % 2 == 1 ? v[n / 2] : 0.5 * (v[(n / 2) - 1] + v[n / 2]);
}

void Timing(std::span<const ChainSlice> slices, int repetitions) {
  using Clock = std::chrono::steady_clock;
  std::printf("\n== timing: ITERATION ONLY, not a result (SVI-7 per slice, SVI-8 surface) ==\n");
  std::vector<std::vector<double>> per_slice(slices.size());
  std::vector<double> surface;
  for (int rep = 0; rep < repetitions; ++rep) {
    for (std::size_t i = 0; i < slices.size(); ++i) {
      const auto t0 = Clock::now();
      const auto fit = aadp::svi::FitRawSvi(slices[i].quotes);
      const auto t1 = Clock::now();
      if (!fit) std::printf("  fit failed in timing run\n");
      per_slice[i].push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    const auto t0 = Clock::now();
    const aadp::svi::SurfaceFit all = aadp::svi::FitChain(slices);
    const auto t1 = Clock::now();
    if (all.slices.size() != slices.size()) std::printf("  surface fit incomplete\n");
    surface.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
  }
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

}  // namespace

int main(int argc, char** argv) try {
  std::string options_path = "data/DailyMarketReportOpt.csv";
  std::string futures_path = "data/DailyMarketReportFut.csv";
  int timing = 0;
  const std::span<char*> args(argv, static_cast<std::size_t>(argc));
  for (std::size_t i = 1; i < args.size(); i += 2) {
    const std::string_view flag = args[i];
    if (i + 1 == args.size()) return Usage();
    if (flag == "--options") {
      options_path = args[i + 1];
    } else if (flag == "--futures") {
      futures_path = args[i + 1];
    } else if (flag == "--timing") {
      timing = Repetitions(args[i + 1]);
      if (timing < 1) return Usage();
    } else {
      return Usage();
    }
  }

  // The attribution comes first, before any output that the chain could have shaped.
  Header();
  const auto option_bytes = aadp::taifex::read_binary_file(options_path);
  const auto future_bytes = aadp::taifex::read_binary_file(futures_path);
  if (!option_bytes || !future_bytes) {
    std::cerr << "aadp_svi_fit: " << (option_bytes ? future_bytes.error() : option_bytes.error())
              << ". Run scripts/fetch_data.sh.\n";
    return kExitFailure;
  }
  const auto options = aadp::taifex::read_options(*option_bytes);
  const auto futures = aadp::taifex::read_futures(*future_bytes);
  if (!options || !futures) {
    std::cerr << "aadp_svi_fit: "
              << aadp::taifex::to_string(options ? futures.error().code : options.error().code)
              << '\n';
    return kExitFailure;
  }
  const auto slices =
      aadp::svi::BuildChainSlices(options->rows, futures->rows, aadp::svi::kCalibrationExpiries);
  if (!slices) {
    std::cerr << "aadp_svi_fit: " << aadp::svi::ChainErrorName(slices.error().code) << " on "
              << slices.error().expiry << '\n';
    return kExitFailure;
  }
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
