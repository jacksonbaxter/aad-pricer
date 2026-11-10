#include "runner.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "mc/batch_runner.hpp"
#include "mc/gbm_kernels.hpp"
#include "oracle/black_scholes.hpp"
#include "oracle/catalog.hpp"
#include "oracle/decode_util.hpp"
#include "oracle/finite_difference.hpp"
#include "oracle/plan.hpp"
#include "oracle/references.hpp"
#include "oracle/tolerance.hpp"
#include "support/dual.hpp"
#include "support/mc_support.hpp"

namespace aadp::oracle_runner {
namespace {

using test::ActiveMarket;
using test::Dual;
using test::ReadGreek;

// Value and five Dual Greeks, then per Greek FD(h), then per Greek FD(2h).
constexpr std::size_t kFdOffset = 6;
constexpr std::size_t kTwoHOffset = 11;
constexpr std::size_t kStats = 16;

constexpr std::array<Greek, kFirstOrderGreeks> kGreeks = {Greek::kDelta, Greek::kVega, Greek::kRho,
                                                          Greek::kDividendRho, Greek::kTheta};

std::size_t GreekIndex(Greek greek) { return static_cast<std::size_t>(greek); }

struct GroupRequest {
  std::string instrument;
  std::uint64_t paths = 0;
  std::uint32_t batches = 0;
  std::uint64_t seed = 0;
  std::array<double, kFirstOrderGreeks> fd_h{};  // Zero: no difference requested.
  bool two_h = false;
};

using Table = std::vector<std::array<double, kStats>>;

// The plan decoder has proved both of these valid, so a failure here is a broken invariant.
Instrument RequireInstrument(std::string_view text) {
  auto instrument = DecodeInstrument(text);
  if (!instrument) throw std::logic_error("undecodable instrument in a decoded plan");
  return *instrument;
}

Greek RequireGreek(std::string_view text) {
  const auto greek = DecodeGreek(text);
  if (!greek) throw std::logic_error("undecodable greek in a decoded plan");
  return *greek;
}

Table RunGroup(const GroupRequest& request, std::uint32_t threads) {
  const Instrument instrument = RequireInstrument(request.instrument);
  const RunConfig config{.seed = request.seed,
                         .paths = request.paths,
                         .batches = request.batches,
                         .steps = instrument.contract.steps,
                         .threads = threads};
  const MarketParams<Dual> active = ActiveMarket(instrument.market);
  return RunBatches<kStats>(
      config, [&](std::span<const double> u, std::array<double, kStats>& stats) {
        const Dual path = DiscountedPayoff<Dual>(instrument.contract, active, u);
        stats[0] = path.value;
        for (std::size_t i = 0; i < kGreeks.size(); ++i) {
          stats[i + 1] = ReadGreek(path, kGreeks[i]);
          const double h = request.fd_h[i];
          if (h == 0.0) continue;
          stats[kFdOffset + i] =
              PathCentralDifference(instrument.contract, instrument.market, kGreeks[i], h, u);
          if (request.two_h) {
            stats[kTwoHOffset + i] = PathCentralDifference(instrument.contract, instrument.market,
                                                           kGreeks[i], 2.0 * h, u) -
                                     stats[kFdOffset + i];
          }
        }
      });
}

std::vector<double> Column(const Table& table, std::size_t index) {
  std::vector<double> column;
  column.reserve(table.size());
  for (const auto& row : table) column.push_back(row[index]);
  return column;
}

using GroupKey = std::tuple<std::string, std::uint64_t, std::uint32_t, std::uint64_t>;

std::string Format(const char* format, auto... values) {
  std::array<char, 256> text{};
  std::snprintf(text.data(), text.size(), format, values...);
  return text.data();
}

// Why a row cannot run, or nothing. Decoded rows are valid in themselves, so this covers what
// depends on the other file.
std::optional<std::string> ProblemWithRow(const PlanRow& row, const ReferenceTable& references) {
  if (FiniteDifferenceStep(row.reference)) return std::nullopt;
  const Reference* reference = references.Find(row.reference);
  if (reference == nullptr) return "reference id not found";
  if (reference->parameters != InstrumentParameters(row.instrument)) {
    return "reference parameters differ from the instrument";
  }
  if (reference->b != row.b) return "b differs from the reference file";
  return std::nullopt;
}

}  // namespace

namespace {

GroupKey KeyOf(const Plan& plan, const RunOptions& options, const PlanRow& row) {
  const std::uint64_t paths =
      options.smoke ? options.smoke_paths_per_batch * row.batches : row.paths;
  const std::uint64_t seed = options.smoke ? plan.header.pilot_seed + 1 : row.seed;
  return {row.instrument, paths, row.batches, seed};
}

// Adds the row to its group. One simulation holds one step per Greek, so a second step would
// silently replace it.
std::optional<std::string> AddToGroup(const PlanRow& row, const GroupKey& key,
                                      GroupRequest& group) {
  group.instrument = row.instrument;
  group.paths = std::get<1>(key);
  group.batches = row.batches;
  group.seed = std::get<3>(key);
  if (const auto h = FiniteDifferenceStep(row.reference)) {
    double& slot = group.fd_h[GreekIndex(RequireGreek(row.greek)) - 1];
    if (slot != 0.0 && slot != *h) {
      return "another row uses a different step for this Greek on these paths";
    }
    slot = *h;
  }
  return std::nullopt;
}

ToleranceResult JudgeRow(const PlanRow& row, const ReferenceTable& references, const Table& table,
                         double critical) {
  const Greek greek = RequireGreek(row.greek);
  const std::vector<double> estimate = Column(table, GreekIndex(greek));
  const ToleranceLimits limits{.b = row.b, .se_ceiling = row.se_ceiling, .critical = critical};
  if (FiniteDifferenceStep(row.reference)) {
    const std::size_t fd = kFdOffset + GreekIndex(greek) - 1;
    return CheckPaired(estimate, Column(table, fd), limits);
  }
  return CheckAgainstReference(estimate, references.Find(row.reference)->value, limits);
}

void ReportHeader(const Plan& plan, const RunOptions& options, const RunSummary& summary,
                  std::ostream& out) {
  if (options.smoke) {
    out << "SMOKE RUN: reduced size, not a statistical result. No row is judged.\n";
  }
  out << Format("K=%zu declared=%zu alpha_F=%g B=%u critical=%.6f\n", summary.rows,
                summary.declared_k, plan.header.alpha_f, plan.header.batches, summary.critical);
  if (summary.rows != summary.declared_k) {
    out << "ERROR: row count differs from the plan's K\n";
  }
  if (!summary.critical_matches_plan) {
    out << "ERROR: critical value differs from the plan's\n";
  }
}

const char* Verdict(bool smoke, bool pass) {
  if (smoke) return pass ? "finite" : "NONFINITE";
  return pass ? "pass" : "FAIL";
}

}  // namespace

RunSummary RunPlan(const Plan& plan, const ReferenceTable& references, const RunOptions& options,
                   std::ostream& out) {
  RunSummary summary;
  summary.rows = plan.rows.size();
  summary.declared_k = plan.header.declared_k;
  summary.critical = CriticalValue(plan.header.alpha_f, plan.rows.size(), plan.header.batches);
  // Computed, never trusted from the file. Printed to three decimals there is no point being
  // finer than the file's own digits, but a K that moved the constant must show.
  summary.critical_matches_plan = std::fabs(summary.critical - plan.header.critical) < 1e-9;
  ReportHeader(plan, options, summary, out);

  // One simulation per contract, seed and size. Greeks on one path share it, and Bonferroni
  // needs no independence between them.
  std::map<GroupKey, GroupRequest> groups;
  std::vector<std::optional<std::string>> problems(plan.rows.size());
  for (std::size_t i = 0; i < plan.rows.size(); ++i) {
    const PlanRow& row = plan.rows[i];
    if (!InRunOne(row)) continue;
    problems[i] = ProblemWithRow(row, references);
    if (problems[i]) continue;
    const GroupKey key = KeyOf(plan, options, row);
    problems[i] = AddToGroup(row, key, groups[key]);
  }
  std::map<GroupKey, Table> results;
  for (const auto& [key, request] : groups) results[key] = RunGroup(request, options.threads);

  const auto finite_or_report = [&](const ToleranceResult& r) {
    return std::isfinite(r.estimate) && std::isfinite(r.reference) &&
           std::isfinite(r.standard_error);
  };
  for (std::size_t i = 0; i < plan.rows.size(); ++i) {
    const PlanRow& row = plan.rows[i];
    if (row.status == RowStatus::kAwaitingPhase3) {
      ++summary.awaiting;
      out << Format("%-18s awaiting-phase3\n", row.id.c_str());
      continue;
    }
    if (!InRunOne(row)) {
      ++summary.run2_only;
      out << Format("%-18s not-in-run-1\n", row.id.c_str());
      continue;
    }
    ++summary.run;
    if (const std::optional<std::string>& problem = problems[i]) {
      ++summary.skipped;
      out << Format("%-18s SKIPPED: %s\n", row.id.c_str(), problem->c_str());
      continue;
    }
    const ToleranceResult result =
        JudgeRow(row, references, results.at(KeyOf(plan, options, row)), summary.critical);
    const bool pass = options.smoke ? finite_or_report(result) : result.passed();
    (pass ? summary.passed : summary.failed) += 1;
    out << Format("%-18s estimate=%.10f reference=%.10f se=%.4e bound=%.4e %s condition=%s\n",
                  row.id.c_str(), result.estimate, result.reference, result.standard_error,
                  result.bound, Verdict(options.smoke, pass),
                  options.smoke ? "not-judged" : result.failed_condition());
  }
  out << Format("run=%zu passed=%zu failed=%zu skipped=%zu awaiting=%zu K=%zu\n", summary.run,
                summary.passed, summary.failed, summary.skipped, summary.awaiting, summary.rows);
  out << Format("unresolved=%zu run2_only=%zu\n", summary.unresolved, summary.run2_only);
  return summary;
}

bool InRunOne(const PlanRow& row) {
  if (row.status != RowStatus::kRun) return false;
  const auto instrument = ResolveInstrument(row.instrument);
  return instrument && instrument->kind == InstrumentKind::kGbm;
}

namespace {

constexpr std::uint64_t kDefaultPaths = std::uint64_t{1} << 23;
constexpr std::uint64_t kBarrierPaths = std::uint64_t{1} << 22;

constexpr const char* kFdSpot = "FD:0.01";
constexpr const char* kFdRate = "FD:0.0001";
constexpr const char* kFdAsianTheta = "FD:0.001";

struct GreekReference {
  const char* greek;
  std::string reference;
};

void AddRows(std::vector<PlanTemplateRow>& rows, const std::string& prefix, const char* instrument,
             std::uint64_t paths, const std::vector<GreekReference>& columns) {
  for (const GreekReference& column : columns) {
    rows.push_back({.id = prefix + "." + column.greek,
                    .instrument = instrument,
                    .greek = column.greek,
                    .reference = column.reference,
                    .paths = paths,
                    .status = RowStatus::kRun});
  }
}

std::vector<GreekReference> AllClosedForm(const std::string& prefix,
                                          const std::string& published_greek,
                                          const std::string& published_id) {
  std::vector<GreekReference> columns;
  for (const char* greek : {"value", "delta", "vega", "rho", "divrho", "theta"}) {
    columns.push_back(
        {.greek = greek,
         .reference = greek == published_greek ? published_id : prefix + "." + greek});
  }
  return columns;
}

std::vector<GreekReference> DiscreteBarrierColumns(const std::string& value_id,
                                                   const std::string& delta_id) {
  return {{.greek = "value", .reference = value_id}, {.greek = "delta", .reference = delta_id},
          {.greek = "vega", .reference = kFdRate},   {.greek = "rho", .reference = kFdRate},
          {.greek = "divrho", .reference = kFdRate}, {.greek = "theta", .reference = kFdRate}};
}

}  // namespace

std::vector<PlanTemplateRow> PlanTemplate() {
  namespace cat = catalog;
  std::vector<PlanTemplateRow> rows;
  AddRows(rows, "VA", cat::kVanillaO1, kDefaultPaths, AllClosedForm("VA", "delta", "O1"));
  AddRows(rows, "VB", cat::kVanillaO2, kDefaultPaths, AllClosedForm("VB", "rho", "O2"));
  AddRows(rows, "VC", cat::kVanillaO3, kDefaultPaths, AllClosedForm("VC", "theta", "O3"));
  AddRows(rows, "GA", cat::kGeometricO4, kDefaultPaths, AllClosedForm("GA", "value", "O4"));

  // At an ATM Asian dA/dT nearly cancels on a path at the kink, so h = 1e-4 leaves about 12
  // crossings in the whole run and an unstable SE. 1e-3 is the smallest step with SE stable across
  // four seeds (max/min 1.30, limit 1.62).
  AddRows(rows, "AA50K100", cat::kArithmeticO9, kBarrierPaths,
          {{.greek = "value", .reference = "O9"},
           {.greek = "delta", .reference = kFdSpot},
           {.greek = "vega", .reference = kFdRate},
           {.greek = "rho", .reference = kFdRate},
           {.greek = "divrho", .reference = kFdRate},
           {.greek = "theta", .reference = kFdAsianTheta}});
  AddRows(rows, "AA50K090", cat::kArithmeticN50K90, kBarrierPaths,
          {{.greek = "value", .reference = "AA50K090"}});
  AddRows(rows, "AA50K110", cat::kArithmeticN50K110, kBarrierPaths,
          {{.greek = "value", .reference = "AA50K110"}});
  AddRows(rows, "AA12K090", cat::kArithmeticN12K90, kDefaultPaths,
          {{.greek = "value", .reference = "AA12K090"}});
  AddRows(rows, "AA12K100", cat::kArithmeticN12K100, kDefaultPaths,
          {{.greek = "value", .reference = "AA12K100"}});
  AddRows(rows, "AA12K110", cat::kArithmeticN12K110, kDefaultPaths,
          {{.greek = "value", .reference = "AA12K110"}});
  AddRows(rows, "AA2", cat::kArithmeticO11, kDefaultPaths,
          {{.greek = "value", .reference = "O11"}});

  AddRows(rows, "DB085", cat::kOneStepH85, kBarrierPaths,
          DiscreteBarrierColumns("DB085.value", "DB085.delta"));
  AddRows(rows, "DB090", cat::kOneStepH90, kBarrierPaths,
          DiscreteBarrierColumns("DB090.value", "DB090.delta"));
  AddRows(rows, "DB095", cat::kOneStepH95, kBarrierPaths, DiscreteBarrierColumns("O5", "O6"));
  AddRows(rows, "DB097", cat::kOneStepH97, kBarrierPaths,
          DiscreteBarrierColumns("DB097.value", "DB097.delta"));
  AddRows(rows, "DB099", cat::kOneStepH99, kBarrierPaths,
          DiscreteBarrierColumns("DB099.value", "DB099.delta"));
  AddRows(rows, "DBS", cat::kOneStepO8, kDefaultPaths, DiscreteBarrierColumns("O8", "DBS.delta"));

  AddRows(rows, "CB095", cat::kBridgeO7, kBarrierPaths, AllClosedForm("CB095", "value", "O7"));
  AddRows(rows, "CB085", cat::kBridgeH85, kBarrierPaths, AllClosedForm("CB085", "", ""));
  AddRows(rows, "CBK90", cat::kBridgeK90, kBarrierPaths, AllClosedForm("CBK90", "", ""));

  // Every SVI parameter of every slice of the three usable expiries, on each path product. N is
  // the barrier count, and the reference step comes from the ladder (NOTES.md, "Stage 8").
  for (const auto& [code, surface] :
       {std::pair{"AS", "svi-surface:arithmetic"}, std::pair{"DB", "svi-surface:discrete-barrier"},
        std::pair{"CB", "svi-surface:continuous-barrier"}}) {
    for (const char* slice : {"d05", "d12", "d33"}) {
      for (const char* parameter : {"a", "b", "rho", "m", "sigma"}) {
        rows.push_back({.id = std::string("P3.") + code + "." + slice + "." + parameter,
                        .instrument = surface,
                        .greek = std::string("d_") + parameter + "_" + slice,
                        .reference = kPhase3Placeholder,
                        .paths = kBarrierPaths,
                        .status = RowStatus::kRun,
                        .stage8 = true});
      }
    }
  }
  // At the barrier count, so a knock-in row reads the uniforms of its knock-out row.
  for (const auto& [code, contract] : {std::pair{"KID", "knock-in-parity:discrete"},
                                       std::pair{"KIC", "knock-in-parity:continuous"}}) {
    for (const char* greek : {"value", "delta", "vega", "rho", "divrho", "theta"}) {
      const std::string id = std::string("P3.") + code + "." + greek;
      rows.push_back({.id = id,
                      .instrument = contract,
                      .greek = greek,
                      .reference = id,
                      .paths = kBarrierPaths,
                      .status = RowStatus::kRun,
                      .stage8 = true});
    }
  }
  return rows;
}

std::vector<PilotRow> RunPilot(const std::vector<PlanTemplateRow>& rows,
                               const ReferenceTable& references, const PilotConfig& config,
                               std::ostream& log) {
  const double critical = CriticalValue(kFamilyWiseAlpha, rows.size(), config.batches);
  std::map<GroupKey, GroupRequest> groups;
  const auto key_of = [&](const PlanTemplateRow& row) -> GroupKey {
    const std::uint64_t paths = row.paths / config.paths_divisor / config.batches * config.batches;
    return {row.instrument, paths, config.batches, config.seed};
  };
  // Run 1's rows only; stage 8's have their own pilot (production.hpp).
  for (const PlanTemplateRow& row : rows) {
    if (row.status != RowStatus::kRun || row.stage8) continue;
    const GroupKey key = key_of(row);
    GroupRequest& group = groups[key];
    group.instrument = row.instrument;
    group.paths = std::get<1>(key);
    group.batches = config.batches;
    group.seed = config.seed;
    group.two_h = true;
    if (const auto h = FiniteDifferenceStep(row.reference)) {
      group.fd_h[GreekIndex(RequireGreek(row.greek)) - 1] = *h;
    }
  }
  std::map<GroupKey, Table> tables;
  for (const auto& [key, request] : groups) {
    tables[key] = RunGroup(request, config.threads);
    log << "pilot group done: " << request.instrument << "\n";
  }

  std::vector<PilotRow> pilot;
  for (const PlanTemplateRow& row : rows) {
    if (row.status != RowStatus::kRun || row.stage8) continue;
    const GroupKey key = key_of(row);
    const Table& table = tables.at(key);
    const Greek greek = RequireGreek(row.greek);
    const std::vector<double> estimate = Column(table, GreekIndex(greek));
    PilotRow out;
    out.id = row.id;
    out.paths = std::get<1>(key);
    const ToleranceLimits report{.b = 0.0, .se_ceiling = 1e300, .critical = critical};
    ToleranceResult result;
    if (FiniteDifferenceStep(row.reference)) {
      const std::size_t slot = GreekIndex(greek) - 1;
      result = CheckPaired(estimate, Column(table, kFdOffset + slot), report);
      const BatchStatistics two_h = BatchMeansStatistics(Column(table, kTwoHOffset + slot));
      out.two_h_mean = two_h.mean;
      out.two_h_se = two_h.standard_error;
    } else {
      const Reference* reference = references.Find(row.reference);
      if (reference == nullptr) {
        log << "pilot: no reference " << row.reference << "\n";
        continue;
      }
      result = CheckAgainstReference(estimate, reference->value, report);
    }
    out.estimate = result.estimate;
    out.reference = result.reference;
    out.se = result.standard_error;
    pilot.push_back(out);
    log << Format("%-18s estimate=%.10f reference=%.10f se=%.4e distance/se=%.2f\n", row.id.c_str(),
                  out.estimate, out.reference, out.se,
                  std::fabs(out.estimate - out.reference) / out.se);
  }
  return pilot;
}

std::string EncodePilot(const std::vector<PilotRow>& rows, const PilotConfig& config) {
  using decode::FormatDouble;
  std::string out =
      "# Pilot results, from aadp_run_oracles pilot. Seed differs from any measured seed.\n"
      "# Rule that turns these into the plan, stated with the results it produced:\n"
      "#   se_ceiling = factor * SE_pilot * sqrt(N_pilot / N), rounded up to 3 figures\n"
      "#   factor on reference rows = 1.65: batch means are Gaussian, so two SEs from 64 batches\n"
      "#   have a ratio bounded by exp(critical * sqrt(2 / 126)) = 1.62 at the Bonferroni level\n"
      "#   factor on FD rows = 3: paired differences are mostly zero with spikes where a bump\n"
      "#   straddles the kink, so SE is less stable. sd of log(SE ratio) between pilot seeds 1729\n"
      "#   and 12 was 0.25, not the Gaussian 0.126, and exp(critical * 0.25) = 2.9\n"
      "#   b, reference rows = the reference's own b\n"
      "#   b, FD rows = fd_b_factor * (|mean(FD(2h) - FD(h))| + critical * SE of that mean) / 3,\n"
      "#   fd_b_factor = 2, where FD(2h) - FD(h) = 3 c h^2 for central differences\n"
      "# Code: tools/run_oracles/runner.hpp, BuildPlan. FD rows report Dual against FD(h) on the\n"
      "# same paths, so se there is the SE of the paired batch difference.\n";
  out += "#! seed=" + std::to_string(config.seed) + "\n";
  out += "#! batches=" + std::to_string(config.batches) + "\n";
  out += "#! paths_divisor=" + std::to_string(config.paths_divisor) + "\n";
  out += "id,paths,estimate,reference,se,two_h_mean,two_h_se\n";
  for (const PilotRow& row : rows) {
    out += row.id + "," + std::to_string(row.paths) + "," + FormatDouble(row.estimate) + "," +
           FormatDouble(row.reference) + "," + FormatDouble(row.se) + "," +
           FormatDouble(row.two_h_mean) + "," + FormatDouble(row.two_h_se) + "\n";
  }
  return out;
}

bool DecodePilot(std::string_view text, PilotFile& out) {
  bool header_seen = false;
  for (const std::string_view line : decode::Lines(text)) {
    if (line.starts_with("#! ")) {
      const std::string_view body = line.substr(3);
      const std::size_t equals = body.find('=');
      if (equals == std::string_view::npos) return false;
      const auto value = decode::ParseUnsigned(body.substr(equals + 1));
      if (!value) return false;
      const std::string_view key = body.substr(0, equals);
      if (key == "seed") {
        out.config.seed = *value;
      } else if (key == "batches") {
        out.config.batches = static_cast<std::uint32_t>(*value);
      } else if (key == "paths_divisor") {
        out.config.paths_divisor = *value;
      } else {
        return false;
      }
      continue;
    }
    if (line.empty() || line.front() == '#') continue;
    if (!header_seen) {
      header_seen = true;
      continue;
    }
    const auto fields = decode::Split(line, ',');
    if (fields.size() != 7) return false;
    PilotRow row;
    row.id = fields[0];
    const auto paths = decode::ParseUnsigned(fields[1]);
    const auto estimate = decode::ParseDouble(fields[2]);
    const auto reference = decode::ParseDouble(fields[3]);
    const auto se = decode::ParseDouble(fields[4]);
    const auto two_h_mean = decode::ParseDouble(fields[5]);
    const auto two_h_se = decode::ParseDouble(fields[6]);
    if (!paths || !estimate || !reference || !se || !two_h_mean || !two_h_se) return false;
    row.paths = *paths;
    row.estimate = *estimate;
    row.reference = *reference;
    row.se = *se;
    row.two_h_mean = *two_h_mean;
    row.two_h_se = *two_h_se;
    out.rows.push_back(row);
  }
  return header_seen;
}

double RoundUpToThreeFigures(double value) {
  if (value == 0.0) return 0.0;
  const double scale = std::pow(10.0, std::floor(std::log10(value)) - 2.0);
  return std::ceil(value / scale) * scale;
}

double LadderB(const BatchStatistics& two_h, double critical) {
  const double truncation = (std::fabs(two_h.mean) + critical * two_h.standard_error) / 3.0;
  return RoundUpToThreeFigures(kFdBFactor * truncation);
}

std::string EncodeStage8Pilot(const std::vector<PilotRow>& rows, const PilotConfig& config) {
  using decode::FormatDouble;
  std::string out =
      "# Stage 8 pilot, from aadp_run_oracles pilot --rows stage8 --estimator production. Seed\n"
      "# differs from any measured seed. NOTES.md, \"Stage 8\", registers the rule.\n"
      "# Estimator: the production pricer's tape, ad::PriceWithGradient on the 12 knock-in rows\n"
      "# and ad::PriceLocalVolWithGradient on the 45 surface rows.\n"
      "# Knock-in rows: reference is the value in references/oracle_values.csv, se the tape's,\n"
      "# two-h columns 0. Ceiling factor 1.65.\n"
      "# Surface rows: reference is the mean of the central difference D_{h*} of PriceLocalVol on\n"
      "# the same paths, se that of the paired batch difference, two-h columns those of\n"
      "# Delta_l* = D_l* - D_{l*+1}. h* and the ladder are in references/svi_fd_ladder.csv.\n"
      "# Ceiling factor 3. An unresolved row reports D_8 and the paired SE at h_8, two-h 0;\n"
      "# one whose first step left the domain reports 0 in all three.\n"
      "# The surface rows rest on TAIFEX settlements of trade date 2024-11-15, not tradable\n"
      "# quotes, reaching 33 days. Data: TAIFEX via data.gov.tw, Open Government Data License\n"
      "# (NOTICE).\n";
  out += "#! seed=" + std::to_string(config.seed) + "\n";
  out += "#! batches=" + std::to_string(config.batches) + "\n";
  out += "#! paths_divisor=" + std::to_string(config.paths_divisor) + "\n";
  out += "id,paths,estimate,reference,se,two_h_mean,two_h_se\n";
  for (const PilotRow& row : rows) {
    out += row.id + "," + std::to_string(row.paths) + "," + FormatDouble(row.estimate) + "," +
           FormatDouble(row.reference) + "," + FormatDouble(row.se) + "," +
           FormatDouble(row.two_h_mean) + "," + FormatDouble(row.two_h_se) + "\n";
  }
  return out;
}

std::optional<std::string> RefusePilotSeed(std::uint64_t seed, const PlanHeader& header) {
  if (seed == 0) return "the pilot needs a seed";
  if (seed == header.measured_seed) return "the pilot may not run on the plan's measured seed";
  return std::nullopt;
}

namespace {

// One choice record of the ladder file, or nothing if it is malformed.
std::optional<LadderChoice> DecodeLadderChoice(const std::vector<std::string_view>& fields) {
  LadderChoice choice;
  choice.id = fields[0];
  choice.resolved = fields[1] == "chosen";
  const auto h = decode::ParseDouble(fields[3]);
  const auto h0 = decode::ParseDouble(fields[15]);
  if (!h || !h0 || !(*h > 0.0) || !(*h0 > 0.0)) return std::nullopt;
  choice.h_star = *h;
  choice.h0 = *h0;
  if (choice.resolved) {
    const auto level = decode::ParseUnsigned(fields[2]);
    const auto b = decode::ParseDouble(fields[16]);
    if (!level || !b || *level > 5 || !(*b >= 0.0)) return std::nullopt;
    choice.level = static_cast<std::size_t>(*level);
    choice.b = *b;
  }
  choice.note = fields[17];
  return choice;
}

}  // namespace

std::optional<std::vector<LadderChoice>> DecodeLadderChoices(std::string_view text) {
  constexpr std::size_t kColumns = 18;
  std::vector<LadderChoice> out;
  bool header_seen = false;
  for (const std::string_view line : decode::Lines(text)) {
    if (line.empty() || line.front() == '#') continue;
    const auto fields = decode::Split(line, ',');
    if (fields.size() != kColumns) return std::nullopt;
    if (!header_seen) {
      if (fields[0] != "id" || fields[1] != "record") return std::nullopt;
      header_seen = true;
      continue;
    }
    if (fields[1] == "level") continue;
    if (fields[1] != "chosen" && fields[1] != "unresolved") return std::nullopt;
    auto choice = DecodeLadderChoice(fields);
    if (!choice) return std::nullopt;
    out.push_back(*std::move(choice));
  }
  if (!header_seen) return std::nullopt;
  return out;
}

namespace {

// NOLINTBEGIN(bugprone-suspicious-missing-comma): each entry is one long line split for width.
constexpr std::array<std::string_view, 7> kRunOneComments = {
    "# Pre-registered comparison plan, BENCHMARKS.md section 10. Generated by aadp_run_oracles"
    " emit-plan from references/pilot_results.csv, and committed before the first measured run.",
    "# K is the row count, Phase 3 rows included. Never raise a ceiling or b after seeing a"
    " measured result.",
    "# se_ceiling = factor * SE_pilot * sqrt(N_pilot / N), rounded up to 3 figures."
    " ceiling_factor 1.65 on reference rows: exp(critical * sqrt(2 / 126)) = 1.62 is the"
    " Bonferroni bound on the ratio of two SEs from 64 Gaussian batch means.",
    "# fd_ceiling_factor 3 on FD rows: paired differences are mostly zero with spikes at the"
    " kink. sd of log(SE ratio) between pilot seeds was 0.25 against the Gaussian 0.126, and"
    " exp(critical * 0.25) = 2.9.",
    "# b, reference rows: the reference's own b. b, FD rows: fd_b_factor * (|mean(FD(2h) -"
    " FD(h))| + critical * SE of that mean) / 3, from the pilot's two-h check."
    " fd_b_factor 2: higher-order terms and one more pilot's worth of noise.",
    "# FD rows compare the Dual estimate with FD(h) on the same paths, and SE is that of the"
    " paired batch difference. The step h is in the reference field.",
    "# Awaiting rows fix nothing yet. They get a ceiling from their own pilot before their first"
    " measured run, and they already count in K.",
};
constexpr std::array<std::string_view, 2> kStage8Comments = {
    "# Stage 8 knock-in rows (P3.KID, P3.KIC; NOTES.md, \"Stage 8\"): the production pricer's tape"
    " against the reference in references/oracle_values.csv. Ceilings from"
    " references/pilot_results_stage8.csv with ceiling_factor, b the reference's.",
    "# Stage 8 surface rows (P3.AS, P3.DB, P3.CB): the production tape against the central"
    " difference of PriceLocalVol at FD:<h*> on the same paths, h* and b from the ladder in"
    " references/svi_fd_ladder.csv, ceilings with fd_ceiling_factor. An unresolved row runs at"
    " FD:<h_8> and never passes. The surface is the fit to TAIFEX settlements of 2024-11-15, not"
    " tradable quotes, reaching 33 days (NOTICE).",
};
// NOLINTEND(bugprone-suspicious-missing-comma)

double Ceiling(double factor, const PilotRow& result, std::uint64_t paths) {
  return RoundUpToThreeFigures(
      factor * result.se *
      std::sqrt(static_cast<double>(result.paths) / static_cast<double>(paths)));
}

// A stage 8 row from its pilot and ladder. Throws if the inputs do not cover it.
PlanRow Stage8Row(const PlanTemplateRow& row, PlanRow out, const ReferenceTable& references,
                  const Stage8Inputs& stage8, std::uint64_t measured_seed, double critical) {
  const PilotRow* result = nullptr;
  for (const PilotRow& p : stage8.pilot.rows) {
    if (p.id == row.id) result = &p;
  }
  if (result == nullptr) throw std::runtime_error("no stage 8 pilot row for " + row.id);
  out.paths = row.paths;
  out.seed = measured_seed;
  const auto instrument = ResolveInstrument(row.instrument);
  if (!instrument) throw std::runtime_error("unresolvable instrument for " + row.id);
  if (instrument->kind == InstrumentKind::kKnockIn) {
    const Reference* reference = references.Find(row.reference);
    if (reference == nullptr) throw std::runtime_error("no reference for " + row.id);
    out.se_ceiling = Ceiling(kCeilingFactor, *result, row.paths);
    out.b = reference->b;
    return out;
  }
  const LadderChoice* choice = nullptr;
  for (const LadderChoice& c : stage8.ladder) {
    if (c.id == row.id) choice = &c;
  }
  if (choice == nullptr) throw std::runtime_error("no ladder choice for " + row.id);
  out.reference = "FD:" + decode::FormatDouble(choice->h_star);
  if (!choice->resolved) {
    out.status = RowStatus::kUnresolved;
    return out;
  }
  out.se_ceiling = Ceiling(kFdCeilingFactor, *result, row.paths);
  out.b = LadderB({.mean = result->two_h_mean, .standard_error = result->two_h_se}, critical);
  if (out.b != choice->b) {
    throw std::runtime_error("b of " + row.id + " differs between the pilot and the ladder");
  }
  return out;
}

}  // namespace

Plan BuildPlan(const std::vector<PlanTemplateRow>& rows, const ReferenceTable& references,
               const PilotFile& pilot, std::uint64_t measured_seed, const Stage8Inputs* stage8) {
  std::map<std::string, const PilotRow*, std::less<>> by_id;
  for (const PilotRow& row : pilot.rows) by_id[row.id] = &row;

  Plan plan;
  PlanHeader& h = plan.header;
  h.declared_k = rows.size();
  h.alpha_f = kFamilyWiseAlpha;
  h.batches = pilot.config.batches;
  h.critical = CriticalValue(h.alpha_f, rows.size(), h.batches);
  h.ceiling_factor = kCeilingFactor;
  h.fd_ceiling_factor = kFdCeilingFactor;
  h.fd_b_factor = kFdBFactor;
  h.pilot_seed = pilot.config.seed;
  h.measured_seed = measured_seed;
  plan.comments.assign(kRunOneComments.begin(), kRunOneComments.end());
  if (stage8 != nullptr) {
    plan.comments.insert(plan.comments.end(), kStage8Comments.begin(), kStage8Comments.end());
  }

  for (const PlanTemplateRow& row : rows) {
    PlanRow out;
    out.id = row.id;
    out.instrument = row.instrument;
    out.greek = row.greek;
    out.reference = row.reference;
    out.batches = h.batches;
    out.status = row.status;
    if (row.stage8) {
      if (stage8 == nullptr) {
        out.reference = kPhase3Placeholder;
        out.status = RowStatus::kAwaitingPhase3;
      } else {
        out = Stage8Row(row, out, references, *stage8, measured_seed, h.critical);
      }
      plan.rows.push_back(std::move(out));
      continue;
    }
    if (row.status == RowStatus::kRun) {
      const auto found = by_id.find(row.id);
      if (found == by_id.end()) continue;
      const PilotRow& result = *found->second;
      out.paths = row.paths;
      out.seed = measured_seed;
      const bool finite_difference = FiniteDifferenceStep(row.reference).has_value();
      out.se_ceiling =
          Ceiling(finite_difference ? kFdCeilingFactor : kCeilingFactor, result, row.paths);
      if (finite_difference) {
        const double truncation =
            (std::fabs(result.two_h_mean) + h.critical * result.two_h_se) / 3.0;
        out.b = RoundUpToThreeFigures(kFdBFactor * truncation);
      } else if (const Reference* reference = references.Find(row.reference)) {
        out.b = reference->b;
      }
    }
    plan.rows.push_back(std::move(out));
  }
  return plan;
}

}  // namespace aadp::oracle_runner
