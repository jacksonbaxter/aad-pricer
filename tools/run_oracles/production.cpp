#include "production.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <limits>
#include <map>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "ad/gradient.hpp"
#include "ad/local_vol_gradient.hpp"
#include "data/taifex_csv.hpp"
#include "data/taifex_futures.hpp"
#include "data/taifex_options.hpp"
#include "ladder.hpp"
#include "oracle/black_scholes.hpp"
#include "oracle/catalog.hpp"
#include "oracle/decode_util.hpp"
#include "oracle/finite_difference.hpp"
#include "oracle/plan.hpp"
#include "oracle/references.hpp"
#include "oracle/tolerance.hpp"
#include "pricer/contract.hpp"
#include "pricer/from_plan.hpp"
#include "pricer/local_vol.hpp"
#include "pricer/local_vol_model.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"
#include "runner.hpp"
#include "svi/chain_fit.hpp"
#include "svi/chain_surface.hpp"
#include "svi/local_vol.hpp"
#include "svi/raw_svi.hpp"
#include "svi/surface.hpp"

namespace aadp::oracle_runner {
namespace {

constexpr std::array<Greek, kFirstOrderGreeks> kGreeks = {Greek::kDelta, Greek::kVega, Greek::kRho,
                                                          Greek::kDividendRho, Greek::kTheta};
constexpr double kNoCeiling = std::numeric_limits<double>::infinity();
// A knock-in SE ratio above exp(critical sqrt(2 / 126)) = 1.62, or a surface one above 2.9, is a
// risk to that row's ceiling (NOTES.md, "Stage 8"); reported, never acted on.
constexpr double kKnockInRatioRisk = 1.62;
constexpr double kSurfaceRatioRisk = 2.9;

std::string Format(const char* format, auto... values) {
  std::array<char, 512> text{};
  std::snprintf(text.data(), text.size(), format, values...);
  return text.data();
}

std::size_t GreekSlot(Greek greek) { return static_cast<std::size_t>(greek) - 1; }

std::uint64_t Butterfly(const pricer::FloorTally& t) {
  return t.by_class[static_cast<std::size_t>(svi::LocalVarianceClass::kButterfly)];
}

std::int64_t Change(std::uint64_t bumped, std::uint64_t base) {
  return static_cast<std::int64_t>(bumped) - static_cast<std::int64_t>(base);
}

pricer::Contract ProductionContract(const Instrument& in) {
  pricer::Contract c = pricer::FromPlan(in.contract);
  if (in.knock_in) c.barrier.knock = pricer::Knock::kIn;
  return c;
}

std::expected<std::vector<double>, std::string> PriceBatches(const pricer::Contract& contract,
                                                             const MarketParams<double>& market,
                                                             const pricer::RunConfig& config) {
  const auto price = pricer::Price(contract, pricer::FromPlan(market), config);
  if (!price) return std::unexpected(std::string("the pricer refused a bumped market"));
  return price->batch_means;
}

// The value and five tape Greeks of one GBM contract, and the central differences asked for.
struct GbmTable {
  GbmBatches estimate;
  std::array<std::vector<double>, kFirstOrderGreeks> difference;
};

std::expected<GbmTable, std::string> RunGbmGroup(const Instrument& in,
                                                 const pricer::RunConfig& config,
                                                 const std::array<double, kFirstOrderGreeks>& h) {
  auto tape = GbmTapeBatches(in, config);
  if (!tape) return std::unexpected(tape.error());
  const pricer::Contract contract = ProductionContract(in);
  GbmTable table;
  table.estimate = *std::move(tape);
  for (std::size_t i = 0; i < kFirstOrderGreeks; ++i) {
    if (h[i] == 0.0) continue;
    const auto up = PriceBatches(contract, BumpFor(kGreeks[i], in.market, h[i]), config);
    const auto down = PriceBatches(contract, BumpFor(kGreeks[i], in.market, -h[i]), config);
    if (!up || !down) return std::unexpected(up ? down.error() : up.error());
    std::vector<double>& d = table.difference[i];
    d.resize(up->size());
    for (std::size_t q = 0; q < d.size(); ++q) {
      const double slope = ((*up)[q] - (*down)[q]) / (2.0 * h[i]);
      d[q] = kGreeks[i] == Greek::kTheta ? -slope : slope;
    }
  }
  return table;
}

using GroupKey = std::tuple<std::string, std::uint64_t, std::uint32_t, std::uint64_t>;

struct RowPlan {
  RowInstrument instrument;
  GroupKey key;
  std::optional<double> h;
  std::string problem;
};

GroupKey KeyOf(const Plan& plan, const ProductionOptions& options, const PlanRow& row) {
  std::uint64_t paths = row.paths;
  std::uint64_t seed = row.seed;
  if (options.smoke) {
    paths = options.smoke_paths_per_batch * row.batches;
    seed = plan.header.pilot_seed + 1;
  } else if (options.seed_override != 0) {
    paths = row.paths / options.paths_divisor / row.batches * row.batches;
    seed = options.seed_override;
  }
  return {row.instrument, paths, row.batches, seed};
}

pricer::RunConfig ConfigOf(const GroupKey& key, std::uint32_t threads) {
  return {.seed = std::get<3>(key),
          .paths = std::get<1>(key),
          .batches = std::get<2>(key),
          .threads = threads};
}

// What a row needs from the other files, or why it cannot run. Empty if nothing is missing.
std::string_view ProblemWithRow(const PlanRow& row, const RowInstrument& instrument,
                                const ReferenceTable& references,
                                const svi::ChainSurface* surface) {
  if (row.status == RowStatus::kAwaitingPhase3) {
    return "awaiting-phase3: the production estimator refuses such a plan";
  }
  if (instrument.kind == InstrumentKind::kSurface) return surface == nullptr ? "no data" : "";
  if (FiniteDifferenceStep(row.reference)) return "";
  const Reference* reference = references.Find(row.reference);
  if (reference == nullptr) return "reference id not found";
  if (reference->parameters != instrument.parameters) {
    return "reference parameters differ from the instrument";
  }
  if (reference->b != row.b) return "b differs from the reference file";
  return "";
}

// The rows grouped so that each contract, size and seed is priced once.
struct Groups {
  std::vector<RowPlan> rows;
  std::map<GroupKey, std::array<double, kFirstOrderGreeks>> gbm_steps;
  std::map<GroupKey, std::size_t> surfaces;
};

void PlanRowInto(const Plan& plan, const PlanRow& row, const ReferenceTable& references,
                 const svi::ChainSurface* surface, const ProductionOptions& options, RowPlan& p,
                 Groups& groups) {
  const auto instrument = ResolveInstrument(row.instrument);
  if (!instrument) {
    p.problem = "unresolvable instrument";
    return;
  }
  p.instrument = *instrument;
  p.problem = ProblemWithRow(row, *instrument, references, surface);
  if (!p.problem.empty()) return;
  p.key = KeyOf(plan, options, row);
  p.h = FiniteDifferenceStep(row.reference);
  if (instrument->kind == InstrumentKind::kSurface) {
    groups.surfaces[p.key] = instrument->surface;
    return;
  }
  auto& steps = groups.gbm_steps[p.key];
  const auto greek = DecodeGreek(row.greek);
  if (!p.h || !greek) return;
  double& slot = steps[GreekSlot(*greek)];
  if (slot != 0.0 && slot != *p.h) {
    p.problem = "another row uses a different step for this Greek on these paths";
    return;
  }
  slot = *p.h;
}

Groups GroupRows(const Plan& plan, const ReferenceTable& references,
                 const svi::ChainSurface* surface, const ProductionOptions& options) {
  Groups groups;
  groups.rows.resize(plan.rows.size());
  for (std::size_t i = 0; i < plan.rows.size(); ++i) {
    PlanRowInto(plan, plan.rows[i], references, surface, options, groups.rows[i], groups);
  }
  return groups;
}

using SurfaceTape = std::expected<ad::LocalVolGradientEstimate, pricer::LocalVolFailure>;

struct GroupResults {
  std::map<GroupKey, std::expected<GbmTable, std::string>> gbm;
  std::map<GroupKey, SurfaceTape> surfaces;
};

GroupResults RunGroups(const Groups& groups, const svi::ChainSurface* surface,
                       std::uint32_t threads) {
  GroupResults results;
  for (const auto& [key, steps] : groups.gbm_steps) {
    const auto instrument = ResolveInstrument(std::get<0>(key));
    if (!instrument) continue;  // GroupRows let no such row through.
    results.gbm.emplace(key, RunGbmGroup(instrument->gbm, ConfigOf(key, threads), steps));
  }
  if (surface == nullptr) return results;
  for (const auto& [key, index] : groups.surfaces) {
    results.surfaces.emplace(
        key, ad::PriceLocalVolWithGradient(SurfaceContract(index, *surface), surface->terms,
                                           surface->params, ConfigOf(key, threads)));
  }
  return results;
}

struct Comparison {
  std::vector<double> estimate;
  std::vector<double> reference;  // FD rows: the difference, batch by batch
};

// A GBM row's estimate and, for an FD row, its difference; or why not.
std::string CompareGbm(const PlanRow& row, const RowPlan& p, const GroupResults& results,
                       Comparison& c) {
  const auto& table = results.gbm.at(p.key);
  if (!table) return table.error();
  const auto greek = DecodeGreek(row.greek);
  if (!greek) return "bad greek";
  c.estimate = table->estimate[static_cast<std::size_t>(*greek)];
  if (p.h) c.reference = table->difference[GreekSlot(*greek)];
  return "";
}

// A surface row's tape estimate and its central difference at the plan's h, with the tallies.
std::string CompareSurface(const PlanRow& row, const RowPlan& p, const GroupResults& results,
                           const svi::ChainSurface& surface, std::uint32_t threads,
                           ProductionRow& record, Comparison& c) {
  const SurfaceTape& tape = results.surfaces.at(p.key);
  if (!tape) {
    return std::string("PriceLocalVolWithGradient refused: ") +
           pricer::LocalVolErrorName(tape.error().code);
  }
  const auto greek = DecodeSurfaceGreek(row.greek);
  if (!greek || !p.h) return "bad greek or step";
  const std::size_t input = greek->Index();
  c.estimate = tape->gradient.at(input).batch_means;
  record.base = tape->floors;
  const auto d =
      SurfaceCentralDifference(SurfaceContract(p.instrument.surface, surface), surface.terms,
                               surface.params, input, *p.h, ConfigOf(p.key, threads), tape->floors);
  if (!d) return d.error();
  c.reference = d->batches;
  record.tally = d->tally;
  return "";
}

const char* Verdict(const ProductionOptions& options, const PlanRow& row, bool pass) {
  if (options.smoke) return pass ? "finite" : "NONFINITE";
  if (row.status == RowStatus::kUnresolved) return "UNRESOLVED";
  return pass ? "pass" : "FAIL";
}

bool Finite(const ToleranceResult& r) {
  return std::isfinite(r.estimate) && std::isfinite(r.reference) && std::isfinite(r.standard_error);
}

const char* Condition(const ProductionOptions& options, const PlanRow& row,
                      const ToleranceResult& r) {
  if (options.smoke) return "not-judged";
  if (row.status == RowStatus::kUnresolved) return "unresolved";
  return r.failed_condition();
}

// Judges one row and counts it. An unresolved row has no b and no ceiling, and never passes.
void Judge(const PlanRow& row, const RowPlan& p, const Comparison& c,
           const ReferenceTable& references, const ProductionOptions& options, RunSummary& summary,
           ProductionRow& record) {
  const bool unresolved = row.status == RowStatus::kUnresolved;
  const ToleranceLimits limits{.b = unresolved ? 0.0 : row.b,
                               .se_ceiling = unresolved ? kNoCeiling : row.se_ceiling,
                               .critical = summary.critical};
  if (p.h) {
    record.result = CheckPaired(c.estimate, c.reference, limits);
  } else {
    record.result =
        CheckAgainstReference(c.estimate, references.Find(row.reference)->value, limits);
  }
  const bool pass = options.smoke ? Finite(record.result) : record.result.passed();
  if (unresolved && !options.smoke) {
    ++summary.unresolved;
  } else {
    (pass ? summary.passed : summary.failed) += 1;
  }
  record.verdict = Verdict(options, row, pass);
}

void PrintRow(const PlanRow& row, const ProductionRow& record, const ProductionOptions& options,
              std::ostream& out) {
  out << Format("%-18s estimate=%.10f reference=%.10f se=%.4e bound=%.4e %s condition=%s\n",
                row.id.c_str(), record.result.estimate, record.result.reference,
                record.result.standard_error, record.result.bound, record.verdict.c_str(),
                Condition(options, row, record.result));
  if (!record.surface) return;
  out << Format(
      "  tally %s: base butterfly %llu capped %llu; +h butterfly %+lld capped %+lld; -h "
      "butterfly %+lld capped %+lld\n",
      row.id.c_str(), static_cast<unsigned long long>(Butterfly(record.base)),
      static_cast<unsigned long long>(record.base.Capped()),
      static_cast<long long>(record.tally.butterfly_plus),
      static_cast<long long>(record.tally.capped_plus),
      static_cast<long long>(record.tally.butterfly_minus),
      static_cast<long long>(record.tally.capped_minus));
}

}  // namespace

std::expected<GbmBatches, std::string> GbmTapeBatches(const Instrument& instrument,
                                                      const pricer::RunConfig& config) {
  const auto tape = ad::PriceWithGradient(ProductionContract(instrument),
                                          pricer::FromPlan(instrument.market), config);
  if (!tape) {
    return std::unexpected(std::string("the pricer refused: ") +
                           pricer::ErrorName(tape.error().contract));
  }
  GbmBatches out;
  out[0] = tape->value.batch_means;
  for (std::size_t i = 0; i < kFirstOrderGreeks; ++i) out[1 + i] = tape->greeks[i].batch_means;
  return out;
}

std::expected<svi::ChainSurface, std::string> LoadChainSurface(const std::string& data_dir) {
  const auto option_bytes = taifex::read_binary_file(data_dir + "/DailyMarketReportOpt.csv");
  const auto future_bytes = taifex::read_binary_file(data_dir + "/DailyMarketReportFut.csv");
  if (!option_bytes || !future_bytes) {
    return std::unexpected(option_bytes ? future_bytes.error() : option_bytes.error());
  }
  const auto options = taifex::read_options(*option_bytes);
  const auto futures = taifex::read_futures(*future_bytes);
  if (!options || !futures) {
    return std::unexpected(
        std::string(taifex::to_string(options ? futures.error().code : options.error().code)));
  }
  const auto slices =
      svi::BuildChainSlices(options->rows, futures->rows, svi::kCalibrationExpiries);
  if (!slices) {
    return std::unexpected(std::string(svi::ChainErrorName(slices.error().code)) + " on " +
                           slices.error().expiry);
  }
  auto surface = svi::SurfaceFromChain(*slices, svi::FitChain(*slices));
  if (!surface) return std::unexpected(std::string("a slice did not fit"));
  const svi::SurfaceError error = svi::ValidateSurface(surface->terms, surface->params);
  if (error != svi::SurfaceError::kNone) {
    return std::unexpected(std::string(svi::SurfaceErrorName(error)));
  }
  return *std::move(surface);
}

pricer::LocalVolContract SurfaceContract(std::size_t index, const svi::ChainSurface& surface) {
  return svi::SurfaceContracts(surface.terms.back().expiry).at(index).contract;
}

std::expected<SurfaceDifference, std::string> SurfaceCentralDifference(
    const pricer::LocalVolContract& contract, std::span<const svi::SliceTerms> terms,
    std::span<const svi::RawSvi<double>> params, std::size_t input, double h,
    const pricer::RunConfig& config, const pricer::FloorTally& base) {
  const std::size_t slice = input / ad::kParametersPerSlice;
  const std::size_t parameter = input % ad::kParametersPerSlice;
  if (slice >= params.size()) return std::unexpected(std::string("no such slice"));
  std::vector<svi::RawSvi<double>> up(params.begin(), params.end());
  std::vector<svi::RawSvi<double>> down(params.begin(), params.end());
  up[slice] = Bumped(params[slice], parameter, h);
  down[slice] = Bumped(params[slice], parameter, -h);
  const auto plus = pricer::PriceLocalVol(contract, terms, up, config);
  const auto minus = pricer::PriceLocalVol(contract, terms, down, config);
  if (!plus || !minus) {
    return std::unexpected(
        std::string("PriceLocalVol refused a bumped surface: ") +
        pricer::LocalVolErrorName(plus ? minus.error().code : plus.error().code));
  }
  SurfaceDifference out;
  out.batches.resize(plus->value.batch_means.size());
  for (std::size_t q = 0; q < out.batches.size(); ++q) {
    out.batches[q] = (plus->value.batch_means[q] - minus->value.batch_means[q]) / (2.0 * h);
  }
  out.tally = {.butterfly_plus = Change(Butterfly(plus->floors), Butterfly(base)),
               .butterfly_minus = Change(Butterfly(minus->floors), Butterfly(base)),
               .capped_plus = Change(plus->floors.Capped(), base.Capped()),
               .capped_minus = Change(minus->floors.Capped(), base.Capped())};
  return out;
}

RunSummary RunPlanProduction(const Plan& plan, const ReferenceTable& references,
                             const svi::ChainSurface* surface, const ProductionOptions& options,
                             std::ostream& out, std::vector<ProductionRow>* rows_out) {
  RunSummary summary;
  summary.rows = plan.rows.size();
  summary.declared_k = plan.header.declared_k;
  summary.critical = CriticalValue(plan.header.alpha_f, plan.rows.size(), plan.header.batches);
  summary.critical_matches_plan = std::fabs(summary.critical - plan.header.critical) < 1e-9;
  if (options.smoke) {
    out << "SMOKE RUN: reduced size, not a statistical result. No row is judged.\n";
  }
  out << "ESTIMATOR: production (ad::PriceWithGradient, ad::PriceLocalVolWithGradient; FD rows "
         "against pricer::Price and pricer::PriceLocalVol on the same paths)\n";
  out << Format("K=%zu declared=%zu alpha_F=%g B=%u critical=%.6f\n", summary.rows,
                summary.declared_k, plan.header.alpha_f, plan.header.batches, summary.critical);
  if (summary.rows != summary.declared_k) out << "ERROR: row count differs from the plan's K\n";
  if (!summary.critical_matches_plan) out << "ERROR: critical value differs from the plan's\n";

  const Groups groups = GroupRows(plan, references, surface, options);
  const GroupResults results = RunGroups(groups, surface, options.threads);
  for (std::size_t i = 0; i < plan.rows.size(); ++i) {
    const PlanRow& row = plan.rows[i];
    const RowPlan& p = groups.rows[i];
    ProductionRow record;
    record.id = row.id;
    record.surface = p.instrument.kind == InstrumentKind::kSurface;
    ++summary.run;
    Comparison c;
    std::string problem = p.problem;
    if (problem.empty()) {
      problem = record.surface
                    ? CompareSurface(row, p, results, *surface, options.threads, record, c)
                    : CompareGbm(row, p, results, c);
    }
    if (problem.empty()) {
      Judge(row, p, c, references, options, summary, record);
      PrintRow(row, record, options, out);
    } else {
      ++summary.skipped;
      record.verdict = "SKIPPED";
      record.problem = problem;
      out << Format("%-18s SKIPPED: %s\n", row.id.c_str(), problem.c_str());
    }
    if (rows_out != nullptr) rows_out->push_back(std::move(record));
  }
  out << Format("run=%zu passed=%zu failed=%zu skipped=%zu awaiting=%zu K=%zu\n", summary.run,
                summary.passed, summary.failed, summary.skipped, summary.awaiting, summary.rows);
  out << Format("unresolved=%zu run2_only=%zu\n", summary.unresolved, summary.run2_only);
  return summary;
}

std::expected<SurfacePilot, std::string> PilotSurface(const pricer::LocalVolContract& contract,
                                                      std::span<const svi::SliceTerms> terms,
                                                      std::span<const svi::RawSvi<double>> params,
                                                      const pricer::RunConfig& config,
                                                      double critical,
                                                      std::span<const std::size_t> inputs) {
  const auto tape = ad::PriceLocalVolWithGradient(contract, terms, params, config);
  if (!tape) {
    return std::unexpected(std::string("PriceLocalVolWithGradient refused: ") +
                           pricer::LocalVolErrorName(tape.error().code));
  }
  SurfacePilot pilot;
  pilot.value = tape->value;
  pilot.gradient = tape->gradient;
  pilot.base = tape->floors;
  for (const std::size_t input : inputs) {
    SurfaceLadder row;
    row.input = input;
    const svi::RawSvi<double>& p = params[input / ad::kParametersPerSlice];
    const std::size_t parameter = input % ad::kParametersPerSlice;
    row.h0 = FirstStep(p, parameter);
    std::array<double, kLadderLevels> h{};
    for (std::size_t l = 0; l < kLadderLevels; ++l) h[l] = std::ldexp(row.h0, -static_cast<int>(l));
    if (const auto outside = StepOutsideDomain(p, parameter, row.h0)) {
      row.refused = *outside;
      row.outcome.h_star = h[kLadderLevels - 1];
      pilot.rows.push_back(std::move(row));
      continue;
    }
    std::array<std::vector<double>, kLadderLevels> d;
    for (std::size_t l = 0; l < kLadderLevels && row.refused.empty(); ++l) {
      const auto difference =
          SurfaceCentralDifference(contract, terms, params, input, h[l], config, pilot.base);
      if (!difference) {
        row.refused = difference.error();
        break;
      }
      d[l] = difference->batches;
      row.tallies[l] = difference->tally;
    }
    if (!row.refused.empty()) {
      row.outcome.h_star = h[kLadderLevels - 1];
      pilot.rows.push_back(std::move(row));
      continue;
    }
    row.outcome = ApplyLadderRule(h, d, critical);
    row.d_star = row.outcome.resolved ? d[row.outcome.level + 1] : d[kLadderLevels - 1];
    pilot.rows.push_back(std::move(row));
  }
  return pilot;
}

namespace {

const char* PairVerdict(const LadderLevel& level) {
  if (!level.has_e) return "-";
  return level.consistent ? "consistent" : "inconsistent";
}

void LogLadder(const std::string& id, const SurfaceLadder& row, std::ostream& log) {
  log << Format("  ladder %s: h0 %.6e%s%s\n", id.c_str(), row.h0,
                row.refused.empty() ? "" : ", refused: ", row.refused.c_str());
  for (std::size_t l = 0; l < row.outcome.levels.size(); ++l) {
    const LadderLevel& level = row.outcome.levels[l];
    const TallyChange& t = row.tallies[l];
    log << Format(
        "    l=%zu h=%.6e D=%+.10e (se %.3e) Delta=%+.3e (se %.3e) E=%+.3e (se %.3e) %s tally "
        "bfly %+lld/%+lld cap %+lld/%+lld\n",
        l, level.h, level.d.mean, level.d.standard_error, level.has_delta ? level.delta.mean : 0.0,
        level.has_delta ? level.delta.standard_error : 0.0, level.has_e ? level.e.mean : 0.0,
        level.has_e ? level.e.standard_error : 0.0, PairVerdict(level),
        static_cast<long long>(t.butterfly_plus), static_cast<long long>(t.butterfly_minus),
        static_cast<long long>(t.capped_plus), static_cast<long long>(t.capped_minus));
  }
}

// What the stage 8 pilot runs with: the plan's critical value, the run configuration, the log.
struct PilotContext {
  const Stage8PilotConfig* config;
  double critical;
  std::ostream* log;

  [[nodiscard]] std::uint64_t PathsOf(const PlanTemplateRow& row) const {
    const PilotConfig& pc = config->pilot;
    return row.paths / pc.paths_divisor / pc.batches * pc.batches;
  }
  [[nodiscard]] pricer::RunConfig Run(std::uint64_t paths, std::uint64_t seed) const {
    return {.seed = seed,
            .paths = paths,
            .batches = config->pilot.batches,
            .threads = config->pilot.threads};
  }
};

std::string RatioRisk(double ratio, double limit) {
  return ratio > limit ? Format(" RISK: above %.2f", limit) : std::string();
}

// One knock-in contract: one tape run, its rows against their references, the second seed's SEs.
void PilotKnockIn(const std::string& text, const std::vector<const PlanTemplateRow*>& members,
                  const ReferenceTable& references, const PilotContext& ctx,
                  std::map<std::string, PilotRow>& by_id) {
  const auto instrument = ResolveInstrument(text);
  if (!instrument) throw std::runtime_error("unresolvable instrument " + text);
  const std::uint64_t paths = ctx.PathsOf(*members.front());
  const auto table = GbmTapeBatches(instrument->gbm, ctx.Run(paths, ctx.config->pilot.seed));
  if (!table) throw std::runtime_error(text + ": " + table.error());
  std::optional<GbmBatches> second;
  if (ctx.config->second_seed != 0) {
    auto t = GbmTapeBatches(instrument->gbm, ctx.Run(paths, ctx.config->second_seed));
    if (!t) throw std::runtime_error(text + ": " + t.error());
    second = *std::move(t);
  }
  *ctx.log << "pilot group done: " << text << "\n";
  const ToleranceLimits report{.b = 0.0, .se_ceiling = kNoCeiling, .critical = ctx.critical};
  for (const PlanTemplateRow* row : members) {
    const Reference* reference = references.Find(row->reference);
    const auto greek = DecodeGreek(row->greek);
    if (reference == nullptr || !greek) throw std::runtime_error("bad stage 8 row " + row->id);
    const auto column = static_cast<std::size_t>(*greek);
    const ToleranceResult r = CheckAgainstReference((*table)[column], reference->value, report);
    by_id[row->id] = {.id = row->id,
                      .paths = paths,
                      .estimate = r.estimate,
                      .reference = r.reference,
                      .se = r.standard_error,
                      .two_h_mean = 0.0,
                      .two_h_se = 0.0};
    *ctx.log << Format("%-18s estimate=%.10f reference=%.10f se=%.4e distance/se=%.2f\n",
                       row->id.c_str(), r.estimate, r.reference, r.standard_error,
                       std::fabs(r.estimate - r.reference) / r.standard_error);
    if (!second) continue;
    const double ratio = BatchMeansStatistics((*second)[column]).standard_error / r.standard_error;
    *ctx.log << Format("  second seed %llu: se ratio to the pilot's %.3f%s\n",
                       static_cast<unsigned long long>(ctx.config->second_seed), ratio,
                       RatioRisk(ratio, kKnockInRatioRisk).c_str());
  }
}

// The disclosure line of one surface row, and its pilot row.
PilotRow DiscloseSurfaceRow(const PlanTemplateRow& row, const SurfaceLadder& ladder,
                            const std::vector<double>& tape, std::uint64_t paths,
                            const PilotContext& ctx) {
  PilotRow out{.id = row.id, .paths = paths};
  if (!ladder.refused.empty()) {
    out.estimate = BatchMeansStatistics(tape).mean;
    *ctx.log << Format("%-18s UNRESOLVED: %s\n", row.id.c_str(), ladder.refused.c_str());
    return out;
  }
  const ToleranceLimits report{.b = 0.0, .se_ceiling = kNoCeiling, .critical = ctx.critical};
  const ToleranceResult r = CheckPaired(tape, ladder.d_star, report);
  out.estimate = r.estimate;
  out.reference = r.reference;
  out.se = r.standard_error;
  const LadderOutcome& o = ladder.outcome;
  if (o.resolved) {
    out.two_h_mean = o.two_h.mean;
    out.two_h_se = o.two_h.standard_error;
  }
  const TallyChange& t = ladder.tallies[o.resolved ? o.level + 1 : kLadderLevels - 1];
  const std::string level = o.resolved ? std::to_string(o.level) : "-";
  const double c_se = ctx.critical * r.standard_error;
  *ctx.log << Format(
      "%-18s estimate=%.10e reference=%.10e se=%.4e distance/se=%.2f %s l*=%s h*=%.6e b=%.3e "
      "b/(c se)=%.3f resolution=%.3e tally at h* bfly %+lld/%+lld cap %+lld/%+lld\n",
      row.id.c_str(), r.estimate, r.reference, r.standard_error,
      std::fabs(r.estimate - r.reference) / r.standard_error,
      o.resolved ? "resolved" : "UNRESOLVED", level.c_str(), o.h_star, o.b, o.b / c_se,
      (c_se + o.b) / std::fabs(r.estimate), static_cast<long long>(t.butterfly_plus),
      static_cast<long long>(t.butterfly_minus), static_cast<long long>(t.capped_plus),
      static_cast<long long>(t.capped_minus));
  return out;
}

// One surface contract: one tape run and the full ladder of its rows, then the second seed's
// paired SEs at h*.
void PilotSurfaceContract(std::size_t index, const std::vector<const PlanTemplateRow*>& members,
                          const svi::ChainSurface& surface, const PilotContext& ctx,
                          std::map<std::string, PilotRow>& by_id,
                          std::map<std::string, SurfaceLadder>& ladders) {
  const pricer::LocalVolContract contract = SurfaceContract(index, surface);
  const std::uint64_t paths = ctx.PathsOf(*members.front());
  std::vector<std::size_t> inputs;
  inputs.reserve(members.size());
  for (const PlanTemplateRow* row : members) {
    const auto greek = DecodeSurfaceGreek(row->greek);
    if (!greek) throw std::runtime_error("bad stage 8 row " + row->id);
    inputs.push_back(greek->Index());
  }
  const auto result = PilotSurface(contract, surface.terms, surface.params,
                                   ctx.Run(paths, ctx.config->pilot.seed), ctx.critical, inputs);
  if (!result) {
    throw std::runtime_error(std::string(catalog::kSurfaceInstruments.at(index)) + ": " +
                             result.error());
  }
  *ctx.log << Format(
      "pilot group done: %s, price %.6f +- %.6f, base tally butterfly %llu capped %llu of %llu "
      "path-steps\n",
      catalog::kSurfaceInstruments.at(index), result->value.mean, result->value.standard_error,
      static_cast<unsigned long long>(Butterfly(result->base)),
      static_cast<unsigned long long>(result->base.Capped()),
      static_cast<unsigned long long>(result->base.path_steps));
  for (std::size_t m = 0; m < members.size(); ++m) {
    const SurfaceLadder& ladder = result->rows[m];
    LogLadder(members[m]->id, ladder, *ctx.log);
    by_id[members[m]->id] = DiscloseSurfaceRow(
        *members[m], ladder, result->gradient[ladder.input].batch_means, paths, ctx);
    ladders[members[m]->id] = ladder;
  }
  if (ctx.config->second_seed == 0) return;
  const pricer::RunConfig second = ctx.Run(paths, ctx.config->second_seed);
  const auto tape = ad::PriceLocalVolWithGradient(contract, surface.terms, surface.params, second);
  if (!tape) throw std::runtime_error("second seed: the tape run was refused");
  const ToleranceLimits report{.b = 0.0, .se_ceiling = kNoCeiling, .critical = ctx.critical};
  for (std::size_t m = 0; m < members.size(); ++m) {
    const SurfaceLadder& ladder = result->rows[m];
    if (!ladder.refused.empty()) continue;
    const auto d = SurfaceCentralDifference(contract, surface.terms, surface.params, ladder.input,
                                            ladder.outcome.h_star, second, tape->floors);
    if (!d) throw std::runtime_error("second seed: " + d.error());
    const double se =
        CheckPaired(tape->gradient[ladder.input].batch_means, d->batches, report).standard_error;
    const double ratio = se / by_id[members[m]->id].se;
    *ctx.log << Format("  %-18s second seed %llu: paired se %.4e, ratio to the pilot's %.3f%s\n",
                       members[m]->id.c_str(),
                       static_cast<unsigned long long>(ctx.config->second_seed), se, ratio,
                       RatioRisk(ratio, kSurfaceRatioRisk).c_str());
  }
}

}  // namespace

Stage8Pilot RunStage8Pilot(const std::vector<PlanTemplateRow>& rows,
                           const ReferenceTable& references, const svi::ChainSurface& surface,
                           const Stage8PilotConfig& config, double critical, std::ostream& log) {
  const PilotContext ctx{.config = &config, .critical = critical, .log = &log};
  std::map<std::string, std::vector<const PlanTemplateRow*>> knock_ins;
  std::map<std::size_t, std::vector<const PlanTemplateRow*>> surfaces;
  for (const PlanTemplateRow& row : rows) {
    if (!row.stage8) continue;
    const auto instrument = ResolveInstrument(row.instrument);
    if (!instrument) throw std::runtime_error("unresolvable instrument " + row.instrument);
    if (instrument->kind == InstrumentKind::kKnockIn) {
      knock_ins[row.instrument].push_back(&row);
    } else {
      surfaces[instrument->surface].push_back(&row);
    }
  }
  std::map<std::string, PilotRow> by_id;
  std::map<std::string, SurfaceLadder> ladders;
  for (const auto& [text, members] : knock_ins) {
    PilotKnockIn(text, members, references, ctx, by_id);
  }
  for (const auto& [index, members] : surfaces) {
    PilotSurfaceContract(index, members, surface, ctx, by_id, ladders);
  }
  Stage8Pilot pilot;
  for (const PlanTemplateRow& row : rows) {
    if (!row.stage8) continue;
    pilot.rows.push_back(by_id.at(row.id));
    if (const auto found = ladders.find(row.id); found != ladders.end()) {
      pilot.ladders.emplace_back(row.id, found->second);
    }
  }
  return pilot;
}

namespace {

std::string PairOrDashes(bool present, const BatchStatistics& s) {
  if (!present) return "-,-";
  return decode::FormatDouble(s.mean) + "," + decode::FormatDouble(s.standard_error);
}

const char* ConsistencyField(const LadderLevel& level) {
  if (!level.has_e) return "-";
  return level.consistent ? "yes" : "no";
}

std::string LevelRecord(const std::string& id, std::size_t l, const LadderLevel& level,
                        const TallyChange& t) {
  std::string out = id;
  out += ",level," + std::to_string(l) + "," + decode::FormatDouble(level.h) + ",";
  out += PairOrDashes(true, level.d) + ",";
  out += PairOrDashes(level.has_delta, level.delta) + ",";
  out += PairOrDashes(level.has_e, level.e) + ",";
  out += ConsistencyField(level);
  out += "," + std::to_string(t.butterfly_plus);
  out += "," + std::to_string(t.butterfly_minus);
  out += "," + std::to_string(t.capped_plus);
  out += "," + std::to_string(t.capped_minus);
  out += ",-,-,-\n";
  return out;
}

std::string_view ChoiceNote(const SurfaceLadder& row, bool resolved) {
  if (!row.refused.empty()) return row.refused;
  if (resolved) return "-";
  return "no level l <= 5 with pairs l and l + 1 consistent";
}

std::string ChoiceRecord(const std::string& id, const SurfaceLadder& row) {
  const LadderOutcome& o = row.outcome;
  const bool resolved = row.refused.empty() && o.resolved;
  std::string out = id;
  out += resolved ? ",chosen," + std::to_string(o.level) : std::string(",unresolved,-");
  out += "," + decode::FormatDouble(o.h_star) + ",";
  out += PairOrDashes(resolved, BatchMeansStatistics(row.d_star)) + ",";
  out += PairOrDashes(resolved, o.two_h);
  out += ",-,-,-,-,-,-,-,";
  out += decode::FormatDouble(row.h0) + ",";
  out += resolved ? decode::FormatDouble(o.b) : std::string("-");
  std::string note(ChoiceNote(row, resolved));
  std::ranges::replace(note, ',', ';');
  out += "," + note + "\n";
  return out;
}

}  // namespace

std::string EncodeLadder(const std::vector<std::pair<std::string, SurfaceLadder>>& ladders) {
  std::string out =
      "# Stage 8 finite-difference ladder of the 45 surface rows, from aadp_run_oracles pilot\n"
      "# --rows stage8. NOTES.md, \"Stage 8\", registers the step rule and the level rule.\n"
      "# level rows: h_l, the mean and SE of D_l, Delta_l = D_l - D_{l+1}, E_l = Delta_{l+1} -\n"
      "# Delta_l / 4, whether pair l is consistent with h^2 truncation, and the change of the\n"
      "# butterfly-floored and capped path-step counts at theta + h and theta - h against the\n"
      "# base run. chosen rows: l*, h* = h_{l*+1}, D at h*, Delta_l*, h0 and b. unresolved rows:\n"
      "# h_8, h0 and the reason. The surface is the fit to TAIFEX settlements of trade date\n"
      "# 2024-11-15, not tradable quotes, reaching 33 days. Data: TAIFEX via data.gov.tw, Open\n"
      "# Government Data License (NOTICE).\n"
      "id,record,level,h,d_mean,d_se,delta_mean,delta_se,e_mean,e_se,consistent,"
      "butterfly_plus,butterfly_minus,capped_plus,capped_minus,h0,b,note\n";
  for (const auto& [id, row] : ladders) {
    for (std::size_t l = 0; l < row.outcome.levels.size(); ++l) {
      out += LevelRecord(id, l, row.outcome.levels[l], row.tallies[l]);
    }
    out += ChoiceRecord(id, row);
  }
  return out;
}

std::string Fingerprint(const Plan& plan, const ReferenceTable& references,
                        const svi::ChainSurface* surface, std::uint32_t threads) {
  std::vector<ProductionRow> rows;
  std::ostringstream discard;
  const ProductionOptions options{
      .threads = threads, .seed_override = kFingerprintSeed, .paths_divisor = kFingerprintDivisor};
  RunPlanProduction(plan, references, surface, options, discard, &rows);
  std::string out;
  for (const ProductionRow& row : rows) {
    if (row.verdict == "SKIPPED") {
      if (row.surface && surface == nullptr) continue;
      out += row.id + " SKIPPED\n";
      continue;
    }
    out += Format("%s estimate=%a reference=%a se=%a\n", row.id.c_str(), row.result.estimate,
                  row.result.reference, row.result.standard_error);
  }
  return out;
}

}  // namespace aadp::oracle_runner
