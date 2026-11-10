#include "oracle/plan.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "mc/gbm_kernels.hpp"
#include "oracle/black_scholes.hpp"
#include "oracle/catalog.hpp"
#include "oracle/decode_util.hpp"
#include "oracle/finite_difference.hpp"

namespace aadp {
namespace {

constexpr std::string_view kHeader = "id,instrument,greek,reference,N,B,seed,se_ceiling,b,status";
constexpr std::string_view kDirectivePrefix = "#! ";
constexpr std::string_view kNotYet = "-";
constexpr std::string_view kStatusRun = "run";
constexpr std::string_view kStatusAwaiting = "awaiting-phase3";
constexpr std::string_view kStatusUnresolved = "unresolved";
constexpr std::string_view kFdPrefix = "FD:";

constexpr std::uint64_t kMaxPaths = std::uint64_t{1} << 40;
constexpr std::uint32_t kMaxBatches = 1U << 16;
constexpr std::uint32_t kMaxSteps = 1U << 16;

std::unexpected<decode::DecodeError> Fail(std::size_t line, std::string message) {
  return std::unexpected(decode::DecodeError{.line = line, .message = std::move(message)});
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
  } else if (key == "fd_ceiling_factor") {
    ok = set_double(header.fd_ceiling_factor);
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
  constexpr std::array<std::string_view, 9> kRequired = {
      "K",           "alpha_F",        "B",
      "critical",    "ceiling_factor", "fd_ceiling_factor",
      "fd_b_factor", "pilot_seed",     "measured_seed"};
  return std::ranges::all_of(kRequired, [&](std::string_view key) { return seen.contains(key); });
}

// Whether the Greek and the reference suit the instrument's kind.
std::optional<std::string> ValidateComparison(const PlanRow& row, InstrumentKind kind) {
  const bool finite_difference = row.reference.starts_with(kFdPrefix);
  if (kind == InstrumentKind::kSurface) {
    if (!DecodeSurfaceGreek(row.greek)) return "bad greek";
    if (!finite_difference) return "a surface row needs a finite-difference reference";
    if (!FiniteDifferenceStep(row.reference)) return "bad FD step";
    return std::nullopt;
  }
  const auto greek = DecodeGreek(row.greek);
  if (!greek) return "bad greek";
  if (!finite_difference) {
    if (!decode::IsToken(row.reference)) return "bad reference";
    return std::nullopt;
  }
  if (!FiniteDifferenceStep(row.reference)) return "bad FD step";
  if (*greek == Greek::kValue) return "a value cannot have a finite-difference reference";
  return std::nullopt;
}

std::optional<std::string> ValidateRunRow(const PlanRow& row, const PlanHeader& header) {
  const auto instrument = ResolveInstrument(row.instrument);
  if (!instrument) return "bad instrument";
  if (auto problem = ValidateComparison(row, instrument->kind)) return problem;
  if (row.paths == 0 || row.paths > kMaxPaths) return "bad N";
  if (row.batches != header.batches) return "B differs from the plan's B";
  if (row.paths % row.batches != 0) return "N is not a multiple of B";
  if (row.status == RowStatus::kUnresolved) {
    if (instrument->kind != InstrumentKind::kSurface) return "only a surface row is unresolved";
    return std::nullopt;
  }
  if (!(row.se_ceiling > 0.0)) return "se_ceiling must be positive";
  if (!(row.b >= 0.0)) return "b must not be negative";
  return std::nullopt;
}

// N, seed, se_ceiling and b of a run or unresolved row. An unresolved row fixes no ceiling or b.
std::optional<std::string> DecodeRunFields(const std::vector<std::string_view>& fields,
                                           PlanRow& row) {
  const auto paths = decode::ParseUnsigned(fields[4]);
  if (!paths) return "bad N";
  row.paths = *paths;
  const auto seed = decode::ParseUnsigned(fields[6]);
  if (!seed) return "bad seed";
  row.seed = *seed;
  if (row.status == RowStatus::kUnresolved) {
    if (fields[7] != kNotYet || fields[8] != kNotYet) return "unresolved row fixes a ceiling or b";
    return std::nullopt;
  }
  const auto ceiling = decode::ParseDouble(fields[7]);
  if (!ceiling) return "bad se_ceiling";
  row.se_ceiling = *ceiling;
  const auto b = decode::ParseDouble(fields[8]);
  if (!b) return "bad b";
  row.b = *b;
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
  if (fields[9] == kStatusUnresolved) {
    row.status = RowStatus::kUnresolved;
  } else if (fields[9] != kStatusRun) {
    return std::unexpected("bad status");
  }
  if (const auto problem = DecodeRunFields(fields, row)) return std::unexpected(*problem);
  if (const auto problem = ValidateRunRow(row, header)) return std::unexpected(*problem);
  return row;
}

}  // namespace

namespace {

// The state of one decode. Each method returns why its line is bad, or nothing.
class PlanReader {
 public:
  using Problem = std::optional<std::string>;

  Problem Line(std::string_view line) {
    if (line.size() > decode::kMaxLineBytes) return "line too long";
    // A directive anywhere else would read as binding and change nothing.
    if (line.starts_with("#!")) return Directive(line);
    if (!line.empty() && line.front() == '#') {
      if (!decode::IsPrintable(line)) return "bad comment";
      plan_.comments.emplace_back(line);
      return std::nullopt;
    }
    return header_seen_ ? Row(line) : Columns(line);
  }

  [[nodiscard]] bool header_seen() const { return header_seen_; }
  Plan& plan() { return plan_; }

 private:
  Problem Directive(std::string_view line) {
    if (header_seen_) return "directive after the columns line";
    if (!line.starts_with(kDirectivePrefix)) return "bad directive";
    return ApplyDirective(line.substr(kDirectivePrefix.size()), plan_.header, directives_);
  }

  Problem Columns(std::string_view line) {
    if (line != kHeader) return "bad header";
    if (!HeaderIsComplete(directives_)) return "missing directive";
    if (plan_.header.batches < 2 || plan_.header.batches > kMaxBatches) {
      return "bad B directive";
    }
    header_seen_ = true;
    return std::nullopt;
  }

  Problem Row(std::string_view line) {
    if (plan_.rows.size() >= decode::kMaxRows) return "too many rows";
    auto row = DecodeRow(line, plan_.header);
    if (!row) return row.error();
    if (!ids_.emplace(row->id).second) return "duplicate id";
    plan_.rows.push_back(*std::move(row));
    return std::nullopt;
  }

  Plan plan_;
  std::set<std::string, std::less<>> directives_;
  std::set<std::string, std::less<>> ids_;
  bool header_seen_ = false;
};

}  // namespace

std::expected<Plan, decode::DecodeError> DecodePlan(std::string_view input) {
  if (input.size() > decode::kMaxInputBytes) return Fail(0, "input too large");
  PlanReader reader;
  std::size_t number = 0;
  for (const std::string_view line : decode::Lines(input)) {
    ++number;
    if (auto problem = reader.Line(line)) return Fail(number, *std::move(problem));
  }
  if (!reader.header_seen()) return Fail(0, "missing header");
  return std::move(reader.plan());
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
  directive("fd_ceiling_factor", FormatDouble(h.fd_ceiling_factor));
  directive("fd_b_factor", FormatDouble(h.fd_b_factor));
  directive("pilot_seed", std::to_string(h.pilot_seed));
  directive("measured_seed", std::to_string(h.measured_seed));
  out += kHeader;
  out += '\n';
  for (const PlanRow& row : plan.rows) {
    const bool run = row.status == RowStatus::kRun;
    const bool sized = run || row.status == RowStatus::kUnresolved;
    const std::string dash(kNotYet);
    out += row.id + "," + row.instrument + "," + row.greek + "," + row.reference + ",";
    out += (sized ? std::to_string(row.paths) : dash) + "," + std::to_string(row.batches) + ",";
    out += (sized ? std::to_string(row.seed) : dash) + ",";
    out += (run ? FormatDouble(row.se_ceiling) : dash) + ",";
    out += (run ? FormatDouble(row.b) : dash) + ",";
    if (run) {
      out += kStatusRun;
    } else {
      out += sized ? kStatusUnresolved : kStatusAwaiting;
    }
    out += '\n';
  }
  return out;
}

namespace {

std::optional<Product> DecodeProduct(std::string_view kind) {
  struct Name {
    std::string_view text;
    Product product;
  };
  static constexpr auto kNames = std::to_array<Name>({
      {.text = "european", .product = Product::kEuropean},
      {.text = "arithmetic", .product = Product::kAsianArithmetic},
      {.text = "geometric", .product = Product::kAsianGeometric},
      {.text = "hard", .product = Product::kDownOutHard},
      {.text = "one_step", .product = Product::kDownOutOneStep},
      {.text = "bridge", .product = Product::kDownOutBridge},
  });
  for (const Name& name : kNames) {
    if (name.text == kind) return name.product;
  }
  return std::nullopt;
}

bool ApplyOptionType(std::string_view value, Instrument& out) {
  if (value == "call") {
    out.contract.type = OptionType::kCall;
    return true;
  }
  if (value == "put") {
    out.contract.type = OptionType::kPut;
    return true;
  }
  return false;
}

bool ApplyCount(std::string_view key, std::string_view value, Instrument& out) {
  const auto parsed = decode::ParseUnsigned(value);
  if (!parsed) return false;
  if (key == "m") {
    if (*parsed < 1 || *parsed > kMaxSteps) return false;
    out.contract.steps = static_cast<std::uint32_t>(*parsed);
    return true;
  }
  if (*parsed > 1) return false;
  out.contract.include_spot = *parsed == 1;
  return true;
}

double* NumberField(std::string_view key, Instrument& out) {
  if (key == "S0") return &out.market.spot;
  if (key == "K") return &out.contract.strike;
  if (key == "H") return &out.contract.barrier;
  if (key == "sigma") return &out.market.sigma;
  if (key == "r") return &out.market.r;
  if (key == "q") return &out.market.q;
  if (key == "T") return &out.market.maturity;
  return nullptr;
}

bool ApplyField(std::string_view key, std::string_view value, Instrument& out) {
  if (key == "type") return ApplyOptionType(value, out);
  if (key == "m" || key == "spot") return ApplyCount(key, value, out);
  const auto parsed = decode::ParseDouble(value);
  if (!parsed) return false;
  double* field = NumberField(key, out);
  if (field == nullptr) return false;
  *field = *parsed;
  return true;
}

bool IsPriceable(const Instrument& in) {
  if (!(in.market.spot > 0.0) || !(in.market.sigma > 0.0) || !(in.market.maturity > 0.0) ||
      !(in.contract.strike > 0.0)) {
    return false;
  }
  const bool barrier_kernel = in.contract.product == Product::kDownOutHard ||
                              in.contract.product == Product::kDownOutOneStep ||
                              in.contract.product == Product::kDownOutBridge;
  if (barrier_kernel) return in.contract.barrier > 0.0 && in.contract.barrier < in.market.spot;
  return in.contract.barrier == 0.0;
}

}  // namespace

std::optional<Instrument> DecodeInstrument(std::string_view text) {
  const std::size_t colon = text.find(':');
  if (colon == std::string_view::npos) return std::nullopt;
  const auto product = DecodeProduct(text.substr(0, colon));
  if (!product) return std::nullopt;
  Instrument out;
  out.contract.product = *product;

  std::set<std::string, std::less<>> seen;
  for (const std::string_view item : decode::Split(text.substr(colon + 1), ';')) {
    const std::size_t equals = item.find('=');
    if (equals == std::string_view::npos) return std::nullopt;
    const std::string_view key = item.substr(0, equals);
    if (!seen.emplace(key).second) return std::nullopt;
    if (!ApplyField(key, item.substr(equals + 1), out)) return std::nullopt;
  }
  if (seen.size() != 10) return std::nullopt;
  if (!IsPriceable(out)) return std::nullopt;
  return out;
}

std::string_view InstrumentParameters(std::string_view text) {
  const std::size_t colon = text.find(':');
  return colon == std::string_view::npos ? text : text.substr(colon + 1);
}

std::optional<RowInstrument> ResolveInstrument(std::string_view text) {
  for (const catalog::KnockInEntry& entry : catalog::kKnockIns) {
    if (text != entry.instrument) continue;
    auto knock_out = DecodeInstrument(entry.contract);
    if (!knock_out) return std::nullopt;
    knock_out->knock_in = entry.knock_in;
    return RowInstrument{.kind = InstrumentKind::kKnockIn,
                         .gbm = *knock_out,
                         .parameters = InstrumentParameters(entry.contract),
                         .surface = 0};
  }
  for (std::size_t i = 0; i < catalog::kSurfaceInstruments.size(); ++i) {
    if (text == catalog::kSurfaceInstruments[i]) {
      return RowInstrument{
          .kind = InstrumentKind::kSurface, .gbm = {}, .parameters = {}, .surface = i};
    }
  }
  auto gbm = DecodeInstrument(text);
  if (!gbm) return std::nullopt;
  return RowInstrument{.kind = InstrumentKind::kGbm,
                       .gbm = *gbm,
                       .parameters = InstrumentParameters(text),
                       .surface = 0};
}

std::optional<SurfaceGreek> DecodeSurfaceGreek(std::string_view text) {
  if (!text.starts_with("d_")) return std::nullopt;
  const std::string_view rest = text.substr(2);
  const std::size_t underscore = rest.rfind('_');
  if (underscore == std::string_view::npos) return std::nullopt;
  const std::string_view parameter = rest.substr(0, underscore);
  const std::string_view slice = rest.substr(underscore + 1);
  SurfaceGreek out;
  bool found = false;
  for (std::size_t i = 0; i < catalog::kSurfaceParameters.size(); ++i) {
    if (parameter == catalog::kSurfaceParameters[i]) {
      out.parameter = i;
      found = true;
    }
  }
  if (!found) return std::nullopt;
  for (std::size_t j = 0; j < catalog::kSurfaceSlices.size(); ++j) {
    if (slice == catalog::kSurfaceSlices[j]) {
      out.slice = j;
      return out;
    }
  }
  return std::nullopt;
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
