// aadp_bench_quantlib: QuantLib v1.44 against this project's pricer (NOTES.md, "Stage 10").
//
//   aadp_bench_quantlib smoke [--out <file>]
//       every comparison at N = 2^16 on seed 20261114 against its reference, condition 1 only,
//       every check and the R10 to R12 sanity lines. A reduced size, never a result.
//   aadp_bench_quantlib pilot --out <csv>
//       every comparison at N = 2^20 on seed 20261111: the SE ceilings for the accuracy run.
//   aadp_bench_quantlib accuracy --ceilings <csv> --out <file>
//       every comparison at N = 2^22 on seed 20261112 by the section 10 rule (QL-2), every check,
//       and the R10 to R12 sanity lines at 2^18.
//   aadp_bench_quantlib speed --out <file> [--ql default|tuned|none] [--ours yes|no]
//       [--repetitions <n>]: one timed call per repetition after one warm-up, single thread
//       (QL-1), at 2^20 paths on R1 to R9 and 2^18 on R10 to R12, seed 20261113.
//   Every mode also takes --commit <hash>, recorded in the header.
//
// The checks, each a reason to mark the run INVALID: ours on lanes gives the scalar bits;
// QuantLib's tuned column gives the default's estimate to rounding; GridModel's rule gives
// FixedLocalVolSurface's volatility at 10,000 points.
//
// Exit: 0 every comparison passed and every check held, 3 a comparison failed, 4 a check failed,
// 2 could not run.
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <expected>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "build_info.hpp"
#include "oracle/decode_util.hpp"
#include "oracle/references.hpp"
#include "oracle/tolerance.hpp"
#include "platform/cpu_info.hpp"
#include "platform/host_label.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"
#include "quantlib/compare.hpp"
#include "quantlib/local_vol_rows.hpp"
#include "quantlib/ours.hpp"
#include "quantlib/quantlib_side.hpp"
#include "quantlib_build.hpp"
#include "rng/philox.hpp"
#include "simd/simd_pricer.hpp"

namespace aadp::qlcompare {
namespace {

constexpr std::uint64_t kSpeedPaths = std::uint64_t{1} << 20;
constexpr int kExitFailed = 3;
constexpr int kExitInvalid = 4;

struct Options {
  std::string mode;
  std::string out;
  std::string ceilings;
  std::string commit = "unrecorded";
  std::string ql = "default";
  bool ours = true;
  int repetitions = 3;
};

std::optional<Options> ParseOptions(int argc, const char* const* argv) {
  if (argc < 2) return std::nullopt;
  Options o;
  o.mode = argv[1];
  for (int i = 2; i + 1 < argc; i += 2) {
    const std::string_view key = argv[i];
    const std::string value = argv[i + 1];
    if (key == "--out") {
      o.out = value;
    } else if (key == "--ceilings") {
      o.ceilings = value;
    } else if (key == "--commit") {
      o.commit = value;
    } else if (key == "--ql") {
      o.ql = value;
    } else if (key == "--ours") {
      o.ours = value == "yes";
    } else if (key == "--repetitions") {
      o.repetitions = std::max(1, std::stoi(value));
    } else {
      return std::nullopt;
    }
  }
  if ((argc % 2) != 0) return std::nullopt;
  return o;
}

std::string Format(double x) {
  std::array<char, 40> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "%.10g", x);
  return {buffer.data()};
}

bool Lto() { return build_info::cxx_flags.contains("-flto"); }

std::string Header(const Options& o) {
  std::ostringstream h;
  const platform::CpuInfo cpu = platform::read_cpu_info();
  h << "# aadp_bench_quantlib " << o.mode << " (NOTES.md, Stage 10). ITERATION ONLY, not a result\n"
    << "# commit: " << o.commit << "\n"
    << "# host: " << platform::detect_host_label() << "\n"
    << "# cpu: " << cpu.cpu_model << "\n"
    << "# os: " << cpu.os << "; kernel " << cpu.kernel << "; " << cpu.arch << "\n"
    << "# compiler: " << build_info::compiler_id << " " << build_info::compiler_version << "\n"
    << "# flags: " << build_info::cxx_flags << " (" << build_info::build_type << ", C++"
    << build_info::cxx_standard << "), the same for QuantLib (scripts/check_quantlib_flags.py)\n"
    << "# link: QuantLib " << quantlib_build::kLinkMode << " library; LTO "
    << (Lto() ? "yes (-flto in CMAKE_CXX_FLAGS)" : "no") << "\n"
    << "# simd: " << simd::TargetName() << ", " << simd::MaxLanes() << " double lanes\n"
    << "# threads: 1 on both sides\n";
  for (const auto& [key, value] : ql::VersionRecord()) h << "# " << key << ": " << value << "\n";
  for (const std::string_view line : decode::Lines(quantlib_build::kCacheValues)) {
    if (!line.empty()) h << "# cache " << line << "\n";
  }
  return h.str();
}

class Output {
 public:
  explicit Output(const std::string& path) {
    if (!path.empty()) file_.open(path, std::ios::binary);
  }
  [[nodiscard]] bool good() const { return !file_.is_open() || file_.good(); }
  void Line(const std::string& text) {
    std::cout << text << '\n' << std::flush;
    if (file_.is_open()) file_ << text << '\n' << std::flush;
  }
  void Block(const std::string& text) {
    for (const std::string_view line : decode::Lines(text)) {
      if (!line.empty()) Line(std::string(line));
    }
  }

 private:
  std::ofstream file_;
};

std::uint64_t QuantLibSeed(std::uint64_t base, std::uint32_t batch) {
  return 100 * base + 1 + batch;
}

// One QuantLib comparison: B NPVs (or B bumps) on their own seeds, or R5's single NPV.
BatchStatistics RunQuantLib(const Comparison& c, const ql::Settings& settings, std::uint64_t seed,
                            std::uint64_t paths) {
  if (IsSingleRunQuantLib(c)) {
    ql::Pricer pricer = ql::MakeCasePricer(c.contract, settings, paths);
    pricer.SetSeed(100 * seed);
    const ql::Npv npv = pricer.Price();
    return {.mean = npv.value, .standard_error = npv.error};
  }
  ql::Pricer pricer = ql::MakeCasePricer(c.contract, settings, paths / kBatches);
  std::vector<double> means;
  means.reserve(kBatches);
  const double h = c.contract.row == Row::kR9a ? kAsianBumpStep : kBarrierBumpStep;
  for (std::uint32_t b = 0; b < kBatches; ++b) {
    pricer.SetSeed(QuantLibSeed(seed, b));
    means.push_back(IsQuantLibBump(c) ? pricer.CentralBump(h) : pricer.Price().value);
  }
  return BatchMeansStatistics(means);
}

bool SameBits(const pricer::Estimate& a, const pricer::Estimate& b) {
  if (a.batch_means.size() != b.batch_means.size()) return false;
  for (std::size_t i = 0; i < a.batch_means.size(); ++i) {
    if (std::bit_cast<std::uint64_t>(a.batch_means[i]) !=
        std::bit_cast<std::uint64_t>(b.batch_means[i])) {
      return false;
    }
  }
  return std::bit_cast<std::uint64_t>(a.mean) == std::bit_cast<std::uint64_t>(b.mean);
}

struct Judged {
  ToleranceResult result;
  bool check_held = true;
  std::string check;
};

// The estimate and the comparison's own check: lanes against scalar for ours, tuned against
// default for QuantLib.
std::expected<Judged, std::string> Estimate(const Comparison& c, std::uint64_t seed,
                                            std::uint64_t paths, const RowReference& ref,
                                            const ToleranceLimits& limits) {
  Judged j;
  if (c.engine == Engine::kQuantLib) {
    const BatchStatistics base = RunQuantLib(c, ql::Settings{.tuned = false}, seed, paths);
    const BatchStatistics tuned = RunQuantLib(c, ql::Settings{.tuned = true}, seed, paths);
    const double gap = std::fabs(tuned.mean - base.mean);
    j.check_held = gap <= 1e-12 * std::fabs(base.mean) + 1e-15;
    j.check = std::string(j.check_held ? "tuned-equals-default" : "tuned-equals-default DIFFER") +
              " |gap| " + Format(gap);
    j.result = ApplyTolerance(base, ref.value, limits);
    return j;
  }
  const auto scalar = RunOurs(c, OurVariant::kScalar, seed, paths, kBatches);
  if (!scalar) return std::unexpected(scalar.error());
  if (HasLaneForm(c)) {
    const auto lanes = RunOurs(c, OurVariant::kLanes, seed, paths, kBatches);
    if (!lanes) return std::unexpected(lanes.error());
    j.check_held = SameBits(*scalar, *lanes);
    j.check = j.check_held ? "lanes-equal-scalar bits" : "lanes-equal-scalar DIFFER";
  } else {
    j.check = "no lane form";
  }
  j.result = CheckAgainstReference(scalar->batch_means, ref.value, limits);
  return j;
}

struct RunContext {
  ReferenceTable references;
  R9References r9;
};

std::expected<RunContext, std::string> LoadContext() {
  const std::ifstream file("references/oracle_values.csv", std::ios::binary);
  if (!file) return std::unexpected("cannot open references/oracle_values.csv (run from the repo)");
  std::ostringstream text;
  text << file.rdbuf();
  auto table = DecodeReferences(text.str());
  if (!table) return std::unexpected("cannot decode references/oracle_values.csv");
  const R9References r9 = ComputeR9References(*table);
  return RunContext{.references = std::move(*table), .r9 = r9};
}

void PrintR9(Output& out, const R9References& r9) {
  out.Line("# R9a reference: CD(0.01) of the n = 12 quadrature " + Format(r9.asian_delta) +
           ", CD(0.02) " + Format(r9.asian_cd_2h) + ", b " + Format(r9.asian_b));
  out.Line("# R9b reference: O6 " + Format(r9.barrier_delta) + ", b " + Format(r9.barrier_b) +
           "; QuantLib's bump expectation CD(1.0) of the recursion " + Format(r9.barrier_cd) +
           ", its b " + Format(r9.barrier_bump_b));
}

// GridModel's rule against FixedLocalVolSurface at grid times, between them, outside them, and
// outside the spot range. Returns the largest relative difference.
double GridCheck(const LocalVolGrid& grid) {
  const ql::FixedSurface surface(grid);
  double worst = 0.0;
  for (std::uint32_t i = 0; i < 10000; ++i) {
    const Philox4x32Output u =
        Philox4x32_10({i, 0U, 0U, 0U}, {static_cast<std::uint32_t>(kSmokeSeed), 0U});
    const double a = static_cast<double>(u[0]) * 0x1p-32;
    const double b = static_cast<double>(u[1]) * 0x1p-32;
    double t = 1.02 * a;  // QuantLib refuses t < 0; above the grid it clamps.
    if ((u[2] & 3U) == 0U) t = grid.times[u[3] % grid.times.size()];
    const double s = 10.0 + 300.0 * b;
    const double ours = grid.Vol(t, s);
    const double theirs = surface.Vol(t, s);
    worst = std::max(worst, std::fabs(ours - theirs) / std::max(1e-300, std::fabs(theirs)));
  }
  return worst;
}

struct Tally {
  int failed = 0;
  int invalid = 0;
};

void JudgeAll(Output& out, const RunContext& context, std::uint64_t seed, std::uint64_t paths,
              const std::map<std::string, double>* ceilings, Tally& tally) {
  out.Line("# critical " + Format(Critical()) + ", K " + std::to_string(kComparisonCount) + ", B " +
           std::to_string(kBatches) + ", N " + std::to_string(paths) + ", seed " +
           std::to_string(seed));
  out.Line("accuracy,name,estimate,se,reference,b,ceiling,bound,z,verdict,check,reference_source");
  for (const Comparison& c : Comparisons()) {
    const auto ref = ReferenceFor(c, context.references, context.r9);
    if (!ref) {
      out.Line("error," + c.Name() + "," + ref.error());
      ++tally.invalid;
      continue;
    }
    ToleranceLimits limits{
        .b = ref->b, .se_ceiling = std::numeric_limits<double>::infinity(), .critical = Critical()};
    if (ceilings != nullptr) limits.se_ceiling = ceilings->at(c.Name());
    const auto judged = Estimate(c, seed, paths, *ref, limits);
    if (!judged) {
      out.Line("error," + c.Name() + "," + judged.error());
      ++tally.invalid;
      continue;
    }
    const ToleranceResult& r = judged->result;
    if (!r.passed()) ++tally.failed;
    if (!judged->check_held) ++tally.invalid;
    out.Line("accuracy," + c.Name() + "," + Format(r.estimate) + "," + Format(r.standard_error) +
             "," + Format(r.reference) + "," + Format(ref->b) + "," + Format(r.se_ceiling) + "," +
             Format(r.bound) + "," + Format((r.estimate - r.reference) / r.standard_error) + "," +
             (r.passed() ? "pass" : std::string("FAIL ") + r.failed_condition()) + "," +
             (judged->check_held ? "" : "CHECK FAILED ") + judged->check + "," + ref->source);
  }
}

BatchStatistics QuantLibBatches(ql::Pricer& pricer, std::uint64_t seed) {
  std::vector<double> means;
  for (std::uint32_t b = 0; b < kBatches; ++b) {
    pricer.SetSeed(QuantLibSeed(seed, b));
    means.push_back(pricer.Price().value);
  }
  return BatchMeansStatistics(means);
}

std::string MeanSe(const BatchStatistics& s) {
  return Format(s.mean) + " +- " + Format(s.standard_error);
}

void CheckGrid(Output& out, Tally& tally) {
  const auto grid = BuildGrid(MakeSyntheticSurface());
  if (!grid) {
    out.Line("error,grid," + grid.error());
    ++tally.invalid;
    return;
  }
  const double worst = GridCheck(*grid);
  const bool held = worst <= 1e-14;
  if (!held) ++tally.invalid;
  out.Line(std::string("check,grid-rule-equals-FixedLocalVolSurface,") +
           (held ? "held" : "FAILED") + ",largest relative difference " + Format(worst));
}

void SanityRows(Output& out, std::uint64_t seed, Tally& tally) {
  const SyntheticSurface surface = MakeSyntheticSurface();
  const auto grid = BuildGrid(surface);
  if (!grid) {
    out.Line("error,grid," + grid.error());
    ++tally.invalid;
    return;
  }
  const pricer::RunConfig config{
      .seed = seed, .paths = kLocalVolPaths, .batches = kBatches, .threads = 1};
  const std::uint64_t per_batch = kLocalVolPaths / kBatches;
  for (const LocalVolRow row : {LocalVolRow::kR10, LocalVolRow::kR11}) {
    const char* name = row == LocalVolRow::kR10 ? "R10" : "R11";
    const auto ours = PriceOnGrid(row, *grid, config);
    ql::Pricer pricer = ql::MakeLocalVolPricer(row, *grid, ql::Settings{}, per_batch);
    const BatchStatistics theirs = QuantLibBatches(pricer, seed);
    if (!ours) {
      out.Line(std::string("error,") + name + "," + ours.error());
      ++tally.invalid;
      continue;
    }
    out.Line(std::string("sanity,") + name + ",speed-only row: ours " +
             MeanSe({.mean = ours->mean, .standard_error = ours->standard_error}) + ", QuantLib " +
             MeanSe(theirs) + ", N " + std::to_string(kLocalVolPaths) +
             " (same contract on the same grid; not an accuracy measure)");
  }
  const double reference = R12Reference(surface);
  const auto ours = PriceR12(surface, config);
  ql::Pricer pricer = ql::MakeR12Pricer(surface, ql::Settings{}, per_batch);
  const BatchStatistics theirs = QuantLibBatches(pricer, seed);
  if (!ours) {
    out.Line("error,R12," + ours.error());
    ++tally.invalid;
    return;
  }
  out.Line("sanity,R12,Black at the SVI implied vol " + Format(reference) + "; ours " +
           MeanSe({.mean = ours->mean, .standard_error = ours->standard_error}) + " (" +
           Format((ours->mean - reference) / ours->standard_error) + " SE); QuantLib " +
           MeanSe(theirs) + " (" + Format((theirs.mean - reference) / theirs.standard_error) +
           " SE); no pass mark");
}

int Finish(Output& out, const Tally& tally) {
  out.Line("# comparisons failed " + std::to_string(tally.failed) + ", checks failed " +
           std::to_string(tally.invalid));
  if (tally.invalid != 0) {
    out.Line("# INVALID: a check failed");
    return kExitInvalid;
  }
  return tally.failed != 0 ? kExitFailed : 0;
}

int RunJudged(const Options& o, std::uint64_t seed, std::uint64_t paths, bool sanity) {
  Output out(o.out);
  if (!out.good()) return 2;
  out.Block(Header(o));
  const auto context = LoadContext();
  if (!context) {
    std::cerr << context.error() << '\n';
    return 2;
  }
  PrintR9(out, context->r9);
  std::map<std::string, double> ceilings;
  if (!o.ceilings.empty()) {
    const std::ifstream file(o.ceilings, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    const auto rows = DecodeCeilings(text.str());
    if (!rows) {
      std::cerr << "ceilings: " << rows.error() << '\n';
      return 2;
    }
    for (const CeilingRow& row : *rows) ceilings[row.name] = row.ceiling;
    out.Line("# ceilings: " + o.ceilings);
  } else {
    out.Line("# no SE ceilings: condition 1 only (a reduced-size check, never a result)");
  }
  Tally tally;
  JudgeAll(out, *context, seed, paths, ceilings.empty() ? nullptr : &ceilings, tally);
  CheckGrid(out, tally);
  if (sanity) SanityRows(out, seed, tally);
  return Finish(out, tally);
}

int RunPilot(const Options& o) {
  if (o.out.empty()) return 2;
  const auto context = LoadContext();
  if (!context) {
    std::cerr << context.error() << '\n';
    return 2;
  }
  std::vector<CeilingRow> rows;
  for (const Comparison& c : Comparisons()) {
    BatchStatistics stats;
    if (c.engine == Engine::kQuantLib) {
      stats = RunQuantLib(c, ql::Settings{}, kPilotSeed, kPilotPaths);
    } else {
      const auto ours = RunOurs(c, OurVariant::kScalar, kPilotSeed, kPilotPaths, kBatches);
      if (!ours) {
        std::cerr << c.Name() << ": " << ours.error() << '\n';
        return 2;
      }
      stats = {.mean = ours->mean, .standard_error = ours->standard_error};
    }
    const double factor = CeilingFactor(c);
    rows.push_back(
        {.name = c.Name(),
         .se_pilot = stats.standard_error,
         .n_pilot = kPilotPaths,
         .factor = factor,
         .ceiling = CeilingFrom(stats.standard_error, kPilotPaths, kAccuracyPaths, factor)});
    std::cout << c.Name() << " se " << Format(stats.standard_error) << " ceiling "
              << Format(rows.back().ceiling) << '\n'
              << std::flush;
  }
  const std::string header =
      "SE ceilings for the stage 10 accuracy run (NOTES.md, \"Stage 10\"), committed before it.\n"
      "Pilot: aadp_bench_quantlib pilot, seed 20261111, N_pilot = 2^20 per comparison, commit " +
      o.commit +
      ".\nceiling = factor * se_pilot * sqrt(N_pilot / 2^22), rounded up to 3 figures; factor "
      "1.65 against a reference, 3 on QuantLib's two bumps (references/comparison_plan.csv).";
  std::ofstream file(o.out, std::ios::binary);
  file << RenderCeilings(rows, header);
  return file.good() ? 0 : 2;
}

// Speed.

// A timed estimator: every call, the warm-up included, must give a finite estimate.
struct TimedRun {
  Timing timing;
  bool ok = true;
};

// call() returns the estimate it timed, or NaN if the estimator refused.
template <class F>
TimedRun Time(const F& call, std::uint64_t paths, std::uint32_t steps, int repetitions) {
  bool ok = std::isfinite(call());  // warm-up
  std::vector<double> seconds;
  for (int i = 0; i < repetitions; ++i) {
    const auto start = std::chrono::steady_clock::now();
    const double estimate = call();
    const auto stop = std::chrono::steady_clock::now();
    ok = ok && std::isfinite(estimate);
    seconds.push_back(std::chrono::duration<double>(stop - start).count());
  }
  std::ranges::sort(seconds);
  return {.timing = {.median_s = seconds[seconds.size() / 2],
                     .min_s = seconds.front(),
                     .max_s = seconds.back(),
                     .paths = paths,
                     .steps = steps},
          .ok = ok};
}

double MeanOr(const std::expected<pricer::Estimate, std::string>& e) {
  return e ? e->mean : std::nan("");
}

void SpeedLine(Output& out, const std::string& row, std::string_view column, const TimedRun& run,
               bool& ok) {
  const Timing& t = run.timing;
  if (!run.ok) {
    ok = false;
    out.Line("error,speed," + row + "," + std::string(column) + ",the estimator refused");
    return;
  }
  out.Line("speed," + row + "," + std::string(column) + "," + std::to_string(t.paths) + "," +
           std::to_string(t.steps) + "," + Format(t.median_s) + "," + Format(t.min_s) + "," +
           Format(t.max_s) + "," + Format(PathsPerSecond(t)) + "," +
           Format(NanosecondsPerPathStep(t)));
}

std::string_view QlColumn(const Options& o) {
  if (o.ql == "default") return "ql-default";
  return Lto() ? "ql-tuned" : "ql-tuned-nolto";
}

void SpeedCase(Output& out, const Options& o, const Case& c, const std::vector<Engine>& ours,
               bool& ok) {
  const std::uint32_t steps = Steps(c.row);
  if (o.ql != "none" && c.row != Row::kR6) {
    ql::Pricer pricer = ql::MakeCasePricer(c, ql::Settings{.tuned = o.ql == "tuned"}, kSpeedPaths);
    pricer.SetSeed(100 * kSpeedSeed);
    const bool bump = c.row == Row::kR9a || c.row == Row::kR9b;
    const double h = c.row == Row::kR9a ? kAsianBumpStep : kBarrierBumpStep;
    const TimedRun t = Time([&] { return bump ? pricer.CentralBump(h) : pricer.Price().value; },
                            kSpeedPaths, steps, o.repetitions);
    SpeedLine(out, c.id, QlColumn(o), t, ok);
  }
  if (!o.ours) return;
  for (const Engine engine : ours) {
    const Comparison comparison{.contract = c, .engine = engine};
    const std::string suffix =
        engine == Engine::kOurs ? "" : std::string("-") + std::string(EngineName(engine));
    for (const OurVariant v : {OurVariant::kScalar, OurVariant::kLanes}) {
      if (v == OurVariant::kLanes && !HasLaneForm(comparison)) continue;
      const TimedRun t =
          Time([&] { return MeanOr(RunOurs(comparison, v, kSpeedSeed, kSpeedPaths, kBatches)); },
               kSpeedPaths, steps, o.repetitions);
      SpeedLine(out, c.id, (v == OurVariant::kScalar ? "ours-scalar" : "ours-lanes") + suffix, t,
                ok);
    }
  }
}

void SpeedLocalVol(Output& out, const Options& o, const LocalVolGrid& grid,
                   const SyntheticSurface& surface, bool& ok) {
  const pricer::RunConfig config{
      .seed = kSpeedSeed, .paths = kLocalVolPaths, .batches = kBatches, .threads = 1};
  for (const LocalVolRow row : {LocalVolRow::kR10, LocalVolRow::kR11}) {
    const std::string name = row == LocalVolRow::kR10 ? "R10" : "R11";
    if (o.ql != "none") {
      ql::Pricer pricer =
          ql::MakeLocalVolPricer(row, grid, ql::Settings{.tuned = o.ql == "tuned"}, kLocalVolPaths);
      pricer.SetSeed(100 * kSpeedSeed);
      SpeedLine(out, name, QlColumn(o),
                Time([&] { return pricer.Price().value; }, kLocalVolPaths, 50, o.repetitions), ok);
    }
    if (o.ours) {
      SpeedLine(out, name, "ours-scalar",
                Time([&] { return MeanOr(PriceOnGrid(row, grid, config)); }, kLocalVolPaths, 50,
                     o.repetitions),
                ok);
    }
  }
  if (o.ql != "none") {
    ql::Pricer pricer =
        ql::MakeR12Pricer(surface, ql::Settings{.tuned = o.ql == "tuned"}, kLocalVolPaths);
    pricer.SetSeed(100 * kSpeedSeed);
    SpeedLine(out, "R12", QlColumn(o),
              Time([&] { return pricer.Price().value; }, kLocalVolPaths, kR12Steps, o.repetitions),
              ok);
  }
  if (o.ours) {
    SpeedLine(out, "R12", "ours-scalar",
              Time([&] { return MeanOr(PriceR12(surface, config)); }, kLocalVolPaths, kR12Steps,
                   o.repetitions),
              ok);
  }
}

int RunSpeed(const Options& o) {
  Output out(o.out);
  if (!out.good()) return 2;
  out.Block(Header(o));
  out.Line("# seed " + std::to_string(kSpeedSeed) + ", " + std::to_string(o.repetitions) +
           " repetitions after one warm-up, median with min and max; QuantLib column " +
           std::string(QlColumn(o)));
  bool ok = true;
  out.Line("speed,row,column,paths,steps,median_s,min_s,max_s,paths_per_s,ns_per_path_step");
  for (const Case& c : AccuracyCases()) {
    const bool first_strike = c.id == "R2.K100" || c.row != Row::kR2;
    const bool h95 = !IsBarrier(c.row) || c.barrier == 95.0;
    if (!first_strike || !h95) continue;
    std::vector<Engine> ours{Engine::kOurs};
    if (c.row == Row::kR9b) ours = {Engine::kOursOneStep, Engine::kOursSmoothed};
    SpeedCase(out, o, c, ours, ok);
  }
  const SyntheticSurface surface = MakeSyntheticSurface();
  const auto grid = BuildGrid(surface);
  if (!grid) return 2;
  SpeedLocalVol(out, o, *grid, surface, ok);
  return ok ? 0 : 2;
}

int Run(int argc, const char* const* argv) {
  const std::optional<Options> o = ParseOptions(argc, argv);
  if (!o) {
    std::cerr << "usage: aadp_bench_quantlib smoke|pilot|accuracy|speed [--out f] [--ceilings f]"
                 " [--commit h] [--ql default|tuned|none] [--ours yes|no] [--repetitions n]\n";
    return 2;
  }
  if (o->mode == "smoke") return RunJudged(*o, kSmokeSeed, kSmokePaths, true);
  if (o->mode == "pilot") return RunPilot(*o);
  if (o->mode == "accuracy") {
    if (o->ceilings.empty()) return 2;
    return RunJudged(*o, kAccuracySeed, kAccuracyPaths, true);
  }
  if (o->mode == "speed") return RunSpeed(*o);
  return 2;
}

}  // namespace
}  // namespace aadp::qlcompare

// NOLINTNEXTLINE(misc-const-correctness): the standard fixes the signature of main.
int main(int argc, char** argv) try {
  return aadp::qlcompare::Run(argc, argv);
} catch (const std::exception& error) {
  std::cerr << "aadp_bench_quantlib: " << error.what() << '\n';
  return 2;
}
