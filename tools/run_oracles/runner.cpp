#include "runner.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>

#include "mc/batch_runner.hpp"
#include "mc/gbm_kernels.hpp"
#include "oracle/catalog.hpp"
#include "oracle/finite_difference.hpp"
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

Table RunGroup(const GroupRequest& request, std::uint32_t threads) {
  const Instrument instrument = *DecodeInstrument(request.instrument);
  const RunConfig config{request.seed, request.paths, request.batches, instrument.contract.steps,
                         threads};
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
  char text[256];
  std::snprintf(text, sizeof(text), format, values...);
  return text;
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

RunSummary RunPlan(const Plan& plan, const ReferenceTable& references, const RunOptions& options,
                   std::ostream& out) {
  RunSummary summary;
  summary.rows = plan.rows.size();
  summary.declared_k = plan.header.declared_k;
  summary.critical = CriticalValue(plan.header.alpha_f, plan.rows.size(), plan.header.batches);
  // Computed, never trusted from the file. Printed to three decimals there is no point being
  // finer than the file's own digits, but a K that moved the constant must show.
  summary.critical_matches_plan = std::fabs(summary.critical - plan.header.critical) < 1e-9;

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

  // One simulation per contract, seed and size. Greeks on one path share it, and Bonferroni
  // needs no independence between them.
  std::map<GroupKey, GroupRequest> groups;
  std::vector<std::optional<std::string>> problems(plan.rows.size());
  const auto key_of = [&](const PlanRow& row) -> GroupKey {
    const std::uint64_t paths =
        options.smoke ? options.smoke_paths_per_batch * row.batches : row.paths;
    const std::uint64_t seed = options.smoke ? plan.header.pilot_seed + 1 : row.seed;
    return {row.instrument, paths, row.batches, seed};
  };
  for (std::size_t i = 0; i < plan.rows.size(); ++i) {
    const PlanRow& row = plan.rows[i];
    if (row.status != RowStatus::kRun) continue;
    problems[i] = ProblemWithRow(row, references);
    if (problems[i]) continue;
    const GroupKey key = key_of(row);
    GroupRequest& group = groups[key];
    group.instrument = row.instrument;
    group.paths = std::get<1>(key);
    group.batches = row.batches;
    group.seed = std::get<3>(key);
    if (const auto h = FiniteDifferenceStep(row.reference)) {
      // One simulation holds one step per Greek, so a second step would silently replace it.
      double& slot = group.fd_h[GreekIndex(*DecodeGreek(row.greek)) - 1];
      if (slot != 0.0 && slot != *h) {
        problems[i] = "another row uses a different step for this Greek on these paths";
        continue;
      }
      slot = *h;
    }
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
    ++summary.run;
    if (problems[i]) {
      ++summary.skipped;
      out << Format("%-18s SKIPPED: %s\n", row.id.c_str(), problems[i]->c_str());
      continue;
    }
    const Greek greek = *DecodeGreek(row.greek);
    const Table& table = results.at(key_of(row));
    const std::vector<double> estimate = Column(table, GreekIndex(greek));
    const ToleranceLimits limits{
        .b = row.b, .se_ceiling = row.se_ceiling, .critical = summary.critical};
    ToleranceResult result;
    if (FiniteDifferenceStep(row.reference)) {
      const std::size_t fd = kFdOffset + GreekIndex(greek) - 1;
      result = CheckPaired(estimate, Column(table, fd), limits);
    } else {
      result = CheckAgainstReference(estimate, references.Find(row.reference)->value, limits);
    }
    const bool pass = options.smoke ? finite_or_report(result) : result.passed();
    (pass ? summary.passed : summary.failed) += 1;
    const char* verdict =
        options.smoke ? (pass ? "finite" : "NONFINITE") : (pass ? "pass" : "FAIL");
    out << Format("%-18s estimate=%.10f reference=%.10f se=%.4e bound=%.4e %s condition=%s\n",
                  row.id.c_str(), result.estimate, result.reference, result.standard_error,
                  result.bound, verdict, options.smoke ? "not-judged" : result.failed_condition());
  }
  out << Format("run=%zu passed=%zu failed=%zu skipped=%zu awaiting=%zu K=%zu\n", summary.run,
                summary.passed, summary.failed, summary.skipped, summary.awaiting, summary.rows);
  return summary;
}

namespace {

constexpr std::uint64_t kDefaultPaths = std::uint64_t{1} << 23;
constexpr std::uint64_t kBarrierPaths = std::uint64_t{1} << 22;

constexpr const char* kFdSpot = "FD:0.01";
constexpr const char* kFdRate = "FD:0.0001";

struct GreekReference {
  const char* greek;
  std::string reference;
};

void AddRows(std::vector<PlanTemplateRow>& rows, const std::string& prefix, const char* instrument,
             std::uint64_t paths, const std::vector<GreekReference>& columns) {
  for (const GreekReference& column : columns) {
    rows.push_back({prefix + "." + column.greek, instrument, column.greek, column.reference, paths,
                    RowStatus::kRun});
  }
}

std::vector<GreekReference> AllClosedForm(const std::string& prefix,
                                          const std::string& published_greek,
                                          const std::string& published_id) {
  std::vector<GreekReference> columns;
  for (const char* greek : {"value", "delta", "vega", "rho", "divrho", "theta"}) {
    columns.push_back({greek, greek == published_greek ? published_id : prefix + "." + greek});
  }
  return columns;
}

std::vector<GreekReference> DiscreteBarrierColumns(const std::string& value_id,
                                                   const std::string& delta_id) {
  return {{"value", value_id}, {"delta", delta_id}, {"vega", kFdRate},
          {"rho", kFdRate},    {"divrho", kFdRate}, {"theta", kFdRate}};
}

}  // namespace

std::vector<PlanTemplateRow> PlanTemplate() {
  namespace cat = catalog;
  std::vector<PlanTemplateRow> rows;
  AddRows(rows, "VA", cat::kVanillaO1, kDefaultPaths, AllClosedForm("VA", "delta", "O1"));
  AddRows(rows, "VB", cat::kVanillaO2, kDefaultPaths, AllClosedForm("VB", "rho", "O2"));
  AddRows(rows, "VC", cat::kVanillaO3, kDefaultPaths, AllClosedForm("VC", "theta", "O3"));
  AddRows(rows, "GA", cat::kGeometricO4, kDefaultPaths, AllClosedForm("GA", "value", "O4"));

  AddRows(rows, "AA50K100", cat::kArithmeticO9, kBarrierPaths,
          {{"value", "O9"},
           {"delta", kFdSpot},
           {"vega", kFdRate},
           {"rho", kFdRate},
           {"divrho", kFdRate},
           {"theta", kFdRate}});
  AddRows(rows, "AA50K090", cat::kArithmeticN50K90, kBarrierPaths, {{"value", "AA50K090"}});
  AddRows(rows, "AA50K110", cat::kArithmeticN50K110, kBarrierPaths, {{"value", "AA50K110"}});
  AddRows(rows, "AA12K090", cat::kArithmeticN12K90, kDefaultPaths, {{"value", "AA12K090"}});
  AddRows(rows, "AA12K100", cat::kArithmeticN12K100, kDefaultPaths, {{"value", "AA12K100"}});
  AddRows(rows, "AA12K110", cat::kArithmeticN12K110, kDefaultPaths, {{"value", "AA12K110"}});
  AddRows(rows, "AA2", cat::kArithmeticO11, kDefaultPaths, {{"value", "O11"}});

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

  // Every SVI parameter of every slice of the three usable expiries, on each path product.
  for (const auto& [code, surface] :
       {std::pair{"AS", "svi-surface:arithmetic"}, std::pair{"DB", "svi-surface:discrete-barrier"},
        std::pair{"CB", "svi-surface:continuous-barrier"}}) {
    for (const char* slice : {"d05", "d12", "d33"}) {
      for (const char* parameter : {"a", "b", "rho", "m", "sigma"}) {
        rows.push_back({std::string("P3.") + code + "." + slice + "." + parameter, surface,
                        std::string("d_") + parameter + "_" + slice, "phase3", 0,
                        RowStatus::kAwaitingPhase3});
      }
    }
  }
  for (const auto& [code, contract] : {std::pair{"KID", "knock-in-parity:discrete"},
                                       std::pair{"KIC", "knock-in-parity:continuous"}}) {
    for (const char* greek : {"value", "delta", "vega", "rho", "divrho", "theta"}) {
      rows.push_back({std::string("P3.") + code + "." + greek, contract, greek, "phase3", 0,
                      RowStatus::kAwaitingPhase3});
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
  for (const PlanTemplateRow& row : rows) {
    if (row.status != RowStatus::kRun) continue;
    const GroupKey key = key_of(row);
    GroupRequest& group = groups[key];
    group.instrument = row.instrument;
    group.paths = std::get<1>(key);
    group.batches = config.batches;
    group.seed = config.seed;
    group.two_h = true;
    if (const auto h = FiniteDifferenceStep(row.reference)) {
      group.fd_h[GreekIndex(*DecodeGreek(row.greek)) - 1] = *h;
    }
  }
  std::map<GroupKey, Table> tables;
  for (const auto& [key, request] : groups) {
    tables[key] = RunGroup(request, config.threads);
    log << "pilot group done: " << request.instrument << "\n";
  }

  std::vector<PilotRow> pilot;
  for (const PlanTemplateRow& row : rows) {
    if (row.status != RowStatus::kRun) continue;
    const GroupKey key = key_of(row);
    const Table& table = tables.at(key);
    const Greek greek = *DecodeGreek(row.greek);
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
      "#   se_ceiling = ceiling_factor * SE_pilot * sqrt(N_pilot / N), ceiling_factor = 1.65\n"
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

namespace {

// Up, so the printed ceiling is never below the rule's value.
double RoundUpToThreeFigures(double value) {
  const double scale = std::pow(10.0, std::floor(std::log10(value)) - 2.0);
  return std::ceil(value / scale) * scale;
}

}  // namespace

Plan BuildPlan(const std::vector<PlanTemplateRow>& rows, const ReferenceTable& references,
               const PilotFile& pilot, std::uint64_t measured_seed) {
  std::map<std::string, const PilotRow*, std::less<>> by_id;
  for (const PilotRow& row : pilot.rows) by_id[row.id] = &row;

  Plan plan;
  PlanHeader& h = plan.header;
  h.declared_k = rows.size();
  h.alpha_f = kFamilyWiseAlpha;
  h.batches = pilot.config.batches;
  h.critical = CriticalValue(h.alpha_f, rows.size(), h.batches);
  h.ceiling_factor = kCeilingFactor;
  h.fd_b_factor = kFdBFactor;
  h.pilot_seed = pilot.config.seed;
  h.measured_seed = measured_seed;
  plan.comments = {
      "# Pre-registered comparison plan, BENCHMARKS.md section 10. Generated by aadp_run_oracles"
      " emit-plan from references/pilot_results.csv, and committed before the first measured run.",
      "# K is the row count, Phase 3 rows included. Never raise a ceiling or b after seeing a"
      " measured result.",
      "# se_ceiling = ceiling_factor * SE_pilot * sqrt(N_pilot / N), rounded up to 3 figures."
      " ceiling_factor 1.65: exp(critical * sqrt(2 / (2 * 63))) = 1.62 is the Bonferroni bound"
      " on the ratio of two SEs from 64 batches, rounded up.",
      "# b, reference rows: the reference's own b. b, FD rows: fd_b_factor * (|mean(FD(2h) -"
      " FD(h))| + critical * SE of that mean) / 3, from the pilot's two-h check."
      " fd_b_factor 2: higher-order terms and one more pilot's worth of noise.",
      "# FD rows compare the Dual estimate with FD(h) on the same paths, and SE is that of the"
      " paired batch difference. The step h is in the reference field.",
      "# Awaiting rows fix nothing yet. They get a ceiling from their own pilot before their first"
      " measured run, and they already count in K.",
  };

  for (const PlanTemplateRow& row : rows) {
    PlanRow out;
    out.id = row.id;
    out.instrument = row.instrument;
    out.greek = row.greek;
    out.reference = row.reference;
    out.batches = h.batches;
    out.status = row.status;
    if (row.status == RowStatus::kRun) {
      const auto found = by_id.find(row.id);
      if (found == by_id.end()) continue;
      const PilotRow& result = *found->second;
      out.paths = row.paths;
      out.seed = measured_seed;
      out.se_ceiling = RoundUpToThreeFigures(
          kCeilingFactor * result.se *
          std::sqrt(static_cast<double>(result.paths) / static_cast<double>(row.paths)));
      if (FiniteDifferenceStep(row.reference)) {
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
