#include "quantlib/compare.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "oracle/barrier.hpp"
#include "oracle/black_scholes.hpp"
#include "oracle/decode_util.hpp"
#include "oracle/references.hpp"
#include "oracle/tolerance.hpp"
#include "pricer/contract.hpp"
#include "quadrature.hpp"

namespace aadp::qlcompare {

namespace {

constexpr std::array<double, 5> kBarriers = {85.0, 90.0, 95.0, 97.0, 99.0};
constexpr std::array<double, 3> kAsianStrikes = {90.0, 100.0, 110.0};
// The quadrature's b on a value (references/oracle_values.csv, AA12K100 and O5).
constexpr double kQuadratureB = 1e-9;

std::string Suffix(char letter, double level) {
  std::array<char, 16> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "%c%03d", letter, static_cast<int>(level));
  return {buffer.data()};
}

const quadrature::AsianResolution kFineAsian{
    .nodes_per_sd = 6.0, .grid_sd = 14.0, .kernel_sd = 16.0};

double AsianQuadrature(double spot) {
  const quadrature::AsianInputs in{.spot = spot,
                                   .strike = 100.0,
                                   .r = 0.0367,
                                   .q = 0.0,
                                   .sigma = 0.17801,
                                   .maturity = 1.0,
                                   .steps = 12};
  return quadrature::ArithmeticAsianCall(in, kFineAsian).value;
}

double BarrierQuadrature(double spot) {
  quadrature::BarrierInputs in = quadrature::kKnockInTerms;
  in.spot = spot;
  return quadrature::DiscreteDownOutGaussLegendre(in, quadrature::kFineGaussLegendre).value;
}

std::expected<RowReference, std::string> FromTable(const ReferenceTable& table,
                                                   std::string_view id) {
  const Reference* row = table.Find(id);
  if (row == nullptr) return std::unexpected("no reference " + std::string(id));
  return RowReference{
      .value = row->value, .b = row->b, .source = "references/oracle_values.csv " + row->id};
}

std::expected<RowReference, std::string> ContinuousReference(const ReferenceTable& table,
                                                             double barrier) {
  if (barrier == 95.0) return FromTable(table, "O7");
  if (barrier == 85.0) return FromTable(table, "CB085.value");
  const MarketParams<double> m{.spot = 100.0, .sigma = 0.30, .r = 0.10, .q = 0.0, .maturity = 0.2};
  return RowReference{
      .value = BarrierOptionValue(BarrierType::kDownOut, OptionType::kCall, 100.0, barrier, 0.0, m),
      .b = 0.0,
      .source = "closed form, src/oracle/barrier.hpp"};
}

std::expected<RowReference, std::string> DiscreteReference(const ReferenceTable& table,
                                                           double barrier) {
  if (barrier == 95.0) return FromTable(table, "O5");
  return FromTable(table, "D" + Suffix('B', barrier) + ".value");
}

}  // namespace

std::string Comparison::Name() const { return contract.id + "/" + std::string(EngineName(engine)); }

std::string_view RowName(Row row) {
  switch (row) {
    case Row::kR1:
      return "R1";
    case Row::kR2:
      return "R2";
    case Row::kR3:
      return "R3";
    case Row::kR4:
      return "R4";
    case Row::kR5:
      return "R5";
    case Row::kR6:
      return "R6";
    case Row::kR7:
      return "R7";
    case Row::kR9a:
      return "R9a";
    case Row::kR9b:
      return "R9b";
  }
  return "unknown";
}

std::string_view EngineName(Engine engine) {
  switch (engine) {
    case Engine::kQuantLib:
      return "quantlib";
    case Engine::kOurs:
      return "ours";
    case Engine::kOursOneStep:
      return "ours-one-step";
    case Engine::kOursSmoothed:
      return "ours-smoothed";
  }
  return "unknown";
}

std::vector<Case> AccuracyCases() {
  std::vector<Case> cases;
  cases.push_back({.id = "R1", .row = Row::kR1, .strike = 100.0});
  for (const double k : kAsianStrikes) {
    cases.push_back({.id = "R2." + Suffix('K', k), .row = Row::kR2, .strike = k});
  }
  cases.push_back({.id = "R3", .row = Row::kR3, .strike = 100.0});
  cases.push_back({.id = "R4", .row = Row::kR4, .strike = 100.0});
  for (const Row row : {Row::kR5, Row::kR6, Row::kR7}) {
    for (const double h : kBarriers) {
      cases.push_back({.id = std::string(RowName(row)) + "." + Suffix('H', h),
                       .row = row,
                       .strike = 100.0,
                       .barrier = h});
    }
  }
  cases.push_back({.id = "R9a", .row = Row::kR9a, .strike = 100.0});
  cases.push_back({.id = "R9b", .row = Row::kR9b, .strike = 100.0, .barrier = 95.0});
  return cases;
}

std::vector<Comparison> Comparisons() {
  std::vector<Comparison> out;
  for (const Case& c : AccuracyCases()) {
    if (c.row != Row::kR6) out.push_back({.contract = c, .engine = Engine::kQuantLib});
    if (c.row == Row::kR9b) {
      out.push_back({.contract = c, .engine = Engine::kOursOneStep});
      out.push_back({.contract = c, .engine = Engine::kOursSmoothed});
    } else {
      out.push_back({.contract = c, .engine = Engine::kOurs});
    }
  }
  return out;
}

std::uint32_t Steps(Row row) {
  switch (row) {
    case Row::kR1:
      return 10;
    case Row::kR2:
    case Row::kR3:
    case Row::kR9a:
      return 12;
    case Row::kR4:
    case Row::kR5:
    case Row::kR6:
    case Row::kR7:
    case Row::kR9b:
      return 50;
  }
  return 0;
}

bool IsBarrier(Row row) {
  return row == Row::kR5 || row == Row::kR6 || row == Row::kR7 || row == Row::kR9b;
}

bool IsQuantLibBump(const Comparison& comparison) {
  return comparison.engine == Engine::kQuantLib &&
         (comparison.contract.row == Row::kR9a || comparison.contract.row == Row::kR9b);
}

bool IsSingleRunQuantLib(const Comparison& comparison) {
  return comparison.engine == Engine::kQuantLib && comparison.contract.row == Row::kR5;
}

double CeilingFactor(const Comparison& comparison) {
  return IsQuantLibBump(comparison) ? kBumpCeilingFactor : kCeilingFactor;
}

pricer::Market<double> OurMarket(const Case& c) {
  switch (c.row) {
    case Row::kR1:
      return {.spot = 100.0, .sigma = 0.20, .r = 0.06, .q = 0.03, .maturity = 1.0};
    case Row::kR2:
    case Row::kR3:
    case Row::kR4:
    case Row::kR9a:
      return {.spot = 100.0, .sigma = 0.17801, .r = 0.0367, .q = 0.0, .maturity = 1.0};
    case Row::kR5:
    case Row::kR6:
    case Row::kR7:
    case Row::kR9b:
      return {.spot = 100.0, .sigma = 0.30, .r = 0.10, .q = 0.0, .maturity = 0.2};
  }
  return {};
}

pricer::Contract OurContract(const Case& c, Engine engine) {
  pricer::Contract out;
  out.strike = c.strike;
  out.steps = Steps(c.row);
  switch (c.row) {
    case Row::kR1:
      out.product = pricer::Product::kAsianGeometric;
      return out;
    case Row::kR2:
    case Row::kR3:
    case Row::kR4:
    case Row::kR9a:
      out.product = pricer::Product::kAsianArithmetic;
      out.include_spot = true;
      return out;
    case Row::kR5:
    case Row::kR6:
    case Row::kR7:
    case Row::kR9b:
      break;
  }
  out.product = pricer::Product::kBarrier;
  out.barrier.direction = pricer::Direction::kDown;
  out.barrier.knock = pricer::Knock::kOut;
  out.barrier.level = c.barrier;
  if (c.row == Row::kR5 || c.row == Row::kR6) {
    out.barrier.monitoring = pricer::Monitoring::kContinuous;
    out.barrier.treatment =
        c.row == Row::kR5 ? pricer::Treatment::kHard : pricer::Treatment::kBridge;
    return out;
  }
  out.barrier.monitoring = pricer::Monitoring::kDiscrete;
  out.barrier.treatment = pricer::Treatment::kHard;
  if (engine == Engine::kOursOneStep) out.barrier.treatment = pricer::Treatment::kOneStep;
  if (engine == Engine::kOursSmoothed) {
    out.barrier.treatment = pricer::Treatment::kSmoothed;
    out.barrier.smoothing_width = kSmoothingWidth;
  }
  return out;
}

R9References ComputeR9References(const ReferenceTable& table) {
  R9References r;
  const double h = kAsianBumpStep;
  const double cd = (AsianQuadrature(100.0 + h) - AsianQuadrature(100.0 - h)) / (2.0 * h);
  const double cd_2h =
      (AsianQuadrature(100.0 + 2.0 * h) - AsianQuadrature(100.0 - 2.0 * h)) / (4.0 * h);
  r.asian_delta = cd;
  r.asian_cd_2h = cd_2h;
  r.asian_b = 2.0 * kQuadratureB / (2.0 * h) + std::fabs(cd_2h - cd);

  const Reference* o6 = table.Find("O6");
  r.barrier_delta = o6 != nullptr ? o6->value : std::nan("");
  r.barrier_b = o6 != nullptr ? o6->b : std::nan("");
  const double hb = kBarrierBumpStep;
  r.barrier_cd = (BarrierQuadrature(100.0 + hb) - BarrierQuadrature(100.0 - hb)) / (2.0 * hb);
  r.barrier_bump_b =
      r.barrier_b + std::fabs(r.barrier_cd - r.barrier_delta) + 2.0 * kQuadratureB / (2.0 * hb);
  return r;
}

std::expected<RowReference, std::string> ReferenceFor(const Comparison& comparison,
                                                      const ReferenceTable& table,
                                                      const R9References& r9) {
  const Case& c = comparison.contract;
  switch (c.row) {
    case Row::kR1:
      return FromTable(table, "O4");
    case Row::kR2:
      return FromTable(table, "AA12" + Suffix('K', c.strike));
    case Row::kR3:
      return FromTable(table, "AA12K100");
    case Row::kR4:
      return FromTable(table, "O9");
    case Row::kR5:
    case Row::kR6:
      return ContinuousReference(table, c.barrier);
    case Row::kR7:
      return DiscreteReference(table, c.barrier);
    case Row::kR9a:
      return RowReference{.value = r9.asian_delta,
                          .b = r9.asian_b,
                          .source = "central difference of tools/quadrature, h 0.01"};
    case Row::kR9b:
      if (comparison.engine == Engine::kQuantLib) {
        return RowReference{.value = r9.barrier_delta,
                            .b = r9.barrier_bump_b,
                            .source = "O6, b with the bump's truncation from tools/quadrature"};
      }
      return RowReference{.value = r9.barrier_delta,
                          .b = r9.barrier_b,
                          .source = "references/oracle_values.csv O6"};
  }
  return std::unexpected("unknown row");
}

double Critical() { return CriticalValue(kFamilyWiseAlpha, kComparisonCount, kBatches); }

double RoundUpThreeFigures(double x) {
  if (!(x > 0.0) || !std::isfinite(x)) return x;
  const double scale = std::pow(10.0, std::floor(std::log10(x)) - 2.0);
  return std::ceil(x / scale) * scale;
}

double CeilingFrom(double se_pilot, std::uint64_t n_pilot, std::uint64_t n, double factor) {
  return RoundUpThreeFigures(factor * se_pilot *
                             std::sqrt(static_cast<double>(n_pilot) / static_cast<double>(n)));
}

std::string RenderCeilings(const std::vector<CeilingRow>& rows, std::string_view header) {
  std::string out;
  for (const std::string_view line : decode::Lines(header)) {
    out += "# ";
    out += line;
    out += '\n';
  }
  out += "name,se_pilot,n_pilot,factor,ceiling\n";
  for (const CeilingRow& row : rows) {
    std::array<char, 256> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%s,%.17g,%llu,%.17g,%.17g\n", row.name.c_str(),
                  row.se_pilot, static_cast<unsigned long long>(row.n_pilot), row.factor,
                  row.ceiling);
    out += buffer.data();
  }
  return out;
}

std::expected<std::vector<CeilingRow>, std::string> DecodeCeilings(std::string_view text) {
  std::vector<CeilingRow> rows;
  bool header = false;
  std::set<std::string> seen;
  for (const std::string_view line : decode::Lines(text)) {
    if (line.empty() || line.starts_with('#')) continue;
    if (!header) {
      if (line != "name,se_pilot,n_pilot,factor,ceiling") return std::unexpected("bad header");
      header = true;
      continue;
    }
    const std::vector<std::string_view> fields = decode::Split(line, ',');
    if (fields.size() != 5) return std::unexpected("bad row: " + std::string(line));
    const auto se = decode::ParseDouble(fields[1]);
    const auto n = decode::ParseUnsigned(fields[2]);
    const auto factor = decode::ParseDouble(fields[3]);
    const auto ceiling = decode::ParseDouble(fields[4]);
    if (!se || !n || !factor || !ceiling) {
      return std::unexpected("bad number: " + std::string(line));
    }
    const std::string name(fields[0]);
    if (!seen.insert(name).second) return std::unexpected("duplicate " + name);
    rows.push_back(
        {.name = name, .se_pilot = *se, .n_pilot = *n, .factor = *factor, .ceiling = *ceiling});
  }
  const std::vector<Comparison> expected = Comparisons();
  if (rows.size() != expected.size()) return std::unexpected("not one row per comparison");
  for (const Comparison& c : expected) {
    if (!seen.contains(c.Name())) return std::unexpected("missing " + c.Name());
  }
  return rows;
}

double PathsPerSecond(const Timing& t) { return static_cast<double>(t.paths) / t.median_s; }

double NanosecondsPerPathStep(const Timing& t) {
  return t.median_s * 1e9 / (static_cast<double>(t.paths) * static_cast<double>(t.steps));
}

double MeanSquaredError(double standard_error, double distance) {
  return std::max(standard_error * standard_error, distance * distance);
}

double ErrorTimesTime(double mse, const Timing& t, std::uint64_t accuracy_paths) {
  return mse * t.median_s / static_cast<double>(t.paths) * static_cast<double>(accuracy_paths);
}

}  // namespace aadp::qlcompare
