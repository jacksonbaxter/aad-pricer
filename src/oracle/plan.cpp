#include "oracle/plan.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <type_traits>

namespace aadp {
namespace {

constexpr std::string_view kHeader = "id,instrument,greek,reference,N,B,seed,se_ceiling,b,status";
constexpr std::string_view kDirectivePrefix = "#! ";
constexpr std::string_view kNotYet = "-";
constexpr std::string_view kStatusRun = "run";
constexpr std::string_view kStatusAwaiting = "awaiting-phase3";
constexpr std::string_view kFdPrefix = "FD:";

constexpr std::uint64_t kMaxPaths = std::uint64_t{1} << 40;
constexpr std::uint32_t kMaxBatches = 1U << 16;
constexpr std::uint32_t kMaxSteps = 1U << 16;

std::unexpected<decode::DecodeError> Fail(std::size_t line, std::string message) {
  return std::unexpected(decode::DecodeError{line, std::move(message)});
}

std::optional<std::string> ApplyDirective(std::string_view text, PlanHeader& header,
                                          std::set<std::string, std::less<>>& seen) {
  const std::size_t equals = text.find('=');
  if (equals == std::string_view::npos) return "directive without '='";
  const std::string_view key = text.substr(0, equals);
  const std::string_view value = text.substr(equals + 1);
  if (!seen.emplace(key).second) return "repeated directive";
  const auto set_unsigned = [&](auto& field) {
    const auto parsed = decode::ParseUnsigned(value);
    if (!parsed) return false;
    field = static_cast<std::remove_reference_t<decltype(field)>>(*parsed);
    return *parsed == static_cast<std::uint64_t>(field);
  };
  const auto set_double = [&](double& field) {
    const auto parsed = decode::ParseDouble(value);
    if (!parsed) return false;
    field = *parsed;
    return true;
  };
  bool ok = true;
  if (key == "K") {
    ok = set_unsigned(header.declared_k);
  } else if (key == "alpha_F") {
    ok = set_double(header.alpha_f);
  } else if (key == "B") {
    ok = set_unsigned(header.batches);
  } else if (key == "critical") {
    ok = set_double(header.critical);
  } else if (key == "ceiling_factor") {
    ok = set_double(header.ceiling_factor);
  } else if (key == "fd_b_factor") {
    ok = set_double(header.fd_b_factor);
  } else if (key == "pilot_seed") {
    ok = set_unsigned(header.pilot_seed);
  } else if (key == "measured_seed") {
    ok = set_unsigned(header.measured_seed);
  } else {
    return "unknown directive";
  }
  if (!ok) return "bad directive value";
  return std::nullopt;
}

bool HeaderIsComplete(const std::set<std::string, std::less<>>& seen) {
  constexpr std::array<std::string_view, 8> kRequired = {
      "K",          "alpha_F",      "B", "critical", "ceiling_factor", "fd_b_factor",
      "pilot_seed", "measured_seed"};
  return std::ranges::all_of(kRequired, [&](std::string_view key) { return seen.contains(key); });
}

std::optional<std::string> ValidateRunRow(const PlanRow& row, const PlanHeader& header) {
  if (!DecodeInstrument(row.instrument)) return "bad instrument";
  const auto greek = DecodeGreek(row.greek);
  if (!greek) return "bad greek";
  if (row.reference.starts_with(kFdPrefix)) {
    if (!FiniteDifferenceStep(row.reference)) return "bad FD step";
    if (*greek == Greek::kValue) return "a value cannot have a finite-difference reference";
  } else if (!decode::IsToken(row.reference)) {
    return "bad reference";
  }
  if (row.paths == 0 || row.paths > kMaxPaths) return "bad N";
  if (row.batches != header.batches) return "B differs from the plan's B";
  if (row.paths % row.batches != 0) return "N is not a multiple of B";
  if (!(row.se_ceiling > 0.0)) return "se_ceiling must be positive";
  if (!(row.b >= 0.0)) return "b must not be negative";
  return std::nullopt;
}

std::expected<PlanRow, std::string> DecodeRow(std::string_view line, const PlanHeader& header) {
  const auto fields = decode::Split(line, ',');
  if (fields.size() != 10) return std::unexpected("expected 10 columns");
  PlanRow row;
  if (!decode::IsToken(fields[0])) return std::unexpected("bad id");
  for (const std::size_t column : {std::size_t{1}, std::size_t{2}, std::size_t{3}}) {
    if (fields[column].empty() || !decode::IsField(fields[column])) {
      return std::unexpected("bad text field");
    }
  }
  row.id = fields[0];
  row.instrument = fields[1];
  row.greek = fields[2];
  row.reference = fields[3];
  const auto batches = decode::ParseUnsigned(fields[5]);
  if (!batches || *batches < 2 || *batches > kMaxBatches) return std::unexpected("bad B");
  row.batches = static_cast<std::uint32_t>(*batches);

  if (fields[9] == kStatusAwaiting) {
    row.status = RowStatus::kAwaitingPhase3;
    if (row.batches != header.batches) return std::unexpected("B differs from the plan's B");
    for (const std::size_t column :
         {std::size_t{4}, std::size_t{6}, std::size_t{7}, std::size_t{8}}) {
      if (fields[column] != kNotYet) return std::unexpected("awaiting row fixes a run field");
    }
    return row;
  }
  if (fields[9] != kStatusRun) return std::unexpected("bad status");
  const auto paths = decode::ParseUnsigned(fields[4]);
  if (!paths) return std::unexpected("bad N");
  row.paths = *paths;
  const auto seed = decode::ParseUnsigned(fields[6]);
  if (!seed) return std::unexpected("bad seed");
  row.seed = *seed;
  const auto ceiling = decode::ParseDouble(fields[7]);
  if (!ceiling) return std::unexpected("bad se_ceiling");
  row.se_ceiling = *ceiling;
  const auto b = decode::ParseDouble(fields[8]);
  if (!b) return std::unexpected("bad b");
  row.b = *b;
  if (const auto problem = ValidateRunRow(row, header)) return std::unexpected(*problem);
  return row;
}

}  // namespace

std::expected<Plan, decode::DecodeError> DecodePlan(std::string_view input) {
  if (input.size() > decode::kMaxInputBytes) return Fail(0, "input too large");
  Plan plan;
  std::set<std::string, std::less<>> directives;
  std::set<std::string, std::less<>> ids;
  bool header_seen = false;
  std::size_t number = 0;
  for (const std::string_view line : decode::Lines(input)) {
    ++number;
    if (line.size() > decode::kMaxLineBytes) return Fail(number, "line too long");
    if (!header_seen && line.starts_with(kDirectivePrefix)) {
      if (const auto problem =
              ApplyDirective(line.substr(kDirectivePrefix.size()), plan.header, directives)) {
        return Fail(number, *problem);
      }
      continue;
    }
    if (!line.empty() && line.front() == '#') {
      for (const char c : line) {
        if (c < ' ' || c > '~') return Fail(number, "bad comment");
      }
      plan.comments.emplace_back(line);
      continue;
    }
    if (!header_seen) {
      if (line != kHeader) return Fail(number, "bad header");
      if (!HeaderIsComplete(directives)) return Fail(number, "missing directive");
      if (plan.header.batches < 2 || plan.header.batches > kMaxBatches) {
        return Fail(number, "bad B directive");
      }
      header_seen = true;
      continue;
    }
    if (plan.rows.size() >= decode::kMaxRows) return Fail(number, "too many rows");
    auto row = DecodeRow(line, plan.header);
    if (!row) return Fail(number, row.error());
    if (!ids.emplace(row->id).second) return Fail(number, "duplicate id");
    plan.rows.push_back(*std::move(row));
  }
  if (!header_seen) return Fail(0, "missing header");
  return plan;
}

std::string EncodePlan(const Plan& plan) {
  using decode::FormatDouble;
  const PlanHeader& h = plan.header;
  std::string out;
  for (const std::string& comment : plan.comments) out += comment + "\n";
  const auto directive = [&](std::string_view key, const std::string& value) {
    out += kDirectivePrefix;
    out += key;
    out += "=" + value + "\n";
  };
  directive("K", std::to_string(h.declared_k));
  directive("alpha_F", FormatDouble(h.alpha_f));
  directive("B", std::to_string(h.batches));
  directive("critical", FormatDouble(h.critical));
  directive("ceiling_factor", FormatDouble(h.ceiling_factor));
  directive("fd_b_factor", FormatDouble(h.fd_b_factor));
  directive("pilot_seed", std::to_string(h.pilot_seed));
  directive("measured_seed", std::to_string(h.measured_seed));
  out += kHeader;
  out += "\n";
  for (const PlanRow& row : plan.rows) {
    const bool run = row.status == RowStatus::kRun;
    const std::string dash(kNotYet);
    out += row.id + "," + row.instrument + "," + row.greek + "," + row.reference + ",";
    out += (run ? std::to_string(row.paths) : dash) + "," + std::to_string(row.batches) + ",";
    out += (run ? std::to_string(row.seed) : dash) + ",";
    out += (run ? FormatDouble(row.se_ceiling) : dash) + ",";
    out += (run ? FormatDouble(row.b) : dash) + ",";
    out += run ? kStatusRun : kStatusAwaiting;
    out += "\n";
  }
  return out;
}

std::optional<Instrument> DecodeInstrument(std::string_view text) {
  const std::size_t colon = text.find(':');
  if (colon == std::string_view::npos) return std::nullopt;
  const std::string_view kind = text.substr(0, colon);
  Instrument out;
  if (kind == "european") {
    out.contract.product = Product::kEuropean;
  } else if (kind == "arithmetic") {
    out.contract.product = Product::kAsianArithmetic;
  } else if (kind == "geometric") {
    out.contract.product = Product::kAsianGeometric;
  } else if (kind == "hard") {
    out.contract.product = Product::kDownOutHard;
  } else if (kind == "one_step") {
    out.contract.product = Product::kDownOutOneStep;
  } else if (kind == "bridge") {
    out.contract.product = Product::kDownOutBridge;
  } else {
    return std::nullopt;
  }

  std::set<std::string, std::less<>> seen;
  for (const std::string_view item : decode::Split(text.substr(colon + 1), ';')) {
    const std::size_t equals = item.find('=');
    if (equals == std::string_view::npos) return std::nullopt;
    const std::string_view key = item.substr(0, equals);
    const std::string_view value = item.substr(equals + 1);
    if (!seen.emplace(key).second) return std::nullopt;
    if (key == "type") {
      if (value == "call") {
        out.contract.type = OptionType::kCall;
      } else if (value == "put") {
        out.contract.type = OptionType::kPut;
      } else {
        return std::nullopt;
      }
    } else if (key == "m" || key == "spot") {
      const auto parsed = decode::ParseUnsigned(value);
      if (!parsed) return std::nullopt;
      if (key == "m") {
        if (*parsed < 1 || *parsed > kMaxSteps) return std::nullopt;
        out.contract.steps = static_cast<std::uint32_t>(*parsed);
      } else {
        if (*parsed > 1) return std::nullopt;
        out.contract.include_spot = *parsed == 1;
      }
    } else {
      const auto parsed = decode::ParseDouble(value);
      if (!parsed) return std::nullopt;
      if (key == "S0") {
        out.market.spot = *parsed;
      } else if (key == "K") {
        out.contract.strike = *parsed;
      } else if (key == "H") {
        out.contract.barrier = *parsed;
      } else if (key == "sigma") {
        out.market.sigma = *parsed;
      } else if (key == "r") {
        out.market.r = *parsed;
      } else if (key == "q") {
        out.market.q = *parsed;
      } else if (key == "T") {
        out.market.maturity = *parsed;
      } else {
        return std::nullopt;
      }
    }
  }
  if (seen.size() != 10) return std::nullopt;
  if (!(out.market.spot > 0.0) || !(out.market.sigma > 0.0) || !(out.market.maturity > 0.0) ||
      !(out.contract.strike > 0.0)) {
    return std::nullopt;
  }
  const bool barrier_kernel = out.contract.product == Product::kDownOutHard ||
                              out.contract.product == Product::kDownOutOneStep ||
                              out.contract.product == Product::kDownOutBridge;
  if (barrier_kernel && !(out.contract.barrier > 0.0 && out.contract.barrier < out.market.spot)) {
    return std::nullopt;
  }
  if (!barrier_kernel && out.contract.barrier != 0.0) return std::nullopt;
  return out;
}

std::string_view InstrumentParameters(std::string_view text) {
  const std::size_t colon = text.find(':');
  return colon == std::string_view::npos ? text : text.substr(colon + 1);
}

std::optional<Greek> DecodeGreek(std::string_view text) {
  for (const Greek greek : {Greek::kValue, Greek::kDelta, Greek::kVega, Greek::kRho,
                            Greek::kDividendRho, Greek::kTheta}) {
    if (text == GreekName(greek)) return greek;
  }
  return std::nullopt;
}

const char* GreekName(Greek greek) {
  switch (greek) {
    case Greek::kValue:
      return "value";
    case Greek::kDelta:
      return "delta";
    case Greek::kVega:
      return "vega";
    case Greek::kRho:
      return "rho";
    case Greek::kDividendRho:
      return "divrho";
    case Greek::kTheta:
      return "theta";
  }
  return "";
}

std::optional<double> FiniteDifferenceStep(std::string_view reference) {
  if (!reference.starts_with(kFdPrefix)) return std::nullopt;
  const auto step = decode::ParseDouble(reference.substr(kFdPrefix.size()));
  if (!step || !(*step > 0.0)) return std::nullopt;
  return step;
}

}  // namespace aadp
