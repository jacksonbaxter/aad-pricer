#include "results/aad_table.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "results/json_lite.hpp"
#include "results/results_file.hpp"
#include "results/table.hpp"

namespace aadp::results {
namespace {

constexpr std::array<std::string_view, 7> kVariantOrder = {
    "price", "tape", "tape-skip", "tape-record", "hand", "bump-central", "bump-forward"};
constexpr double kNanosecondsPerSecond = 1e9;
constexpr double kNanosecondsPerMillisecond = 1e6;
constexpr double kBytesPerMebibyte = 1024.0 * 1024.0;

struct Check {
  std::string name;
  bool passed = false;
  std::string worst;
};

struct Estimate {
  std::string variant;
  std::string quantity;
  double mean = 0.0;
  double se = 0.0;
};

struct Step {
  std::string scheme;
  std::string input;
  double h = 0.0;
  double bias = 0.0;
  double bias_se = 0.0;
};

struct Section {
  std::map<std::string, std::string, std::less<>> fields;
  std::vector<Check> checks;
  std::map<std::string, double, std::less<>> rss;
  std::map<std::string, std::string, std::less<>> benchmarks;  // variant -> benchmark name
  std::vector<Estimate> estimates;
  std::vector<Step> steps;

  [[nodiscard]] std::string_view field(std::string_view key) const {
    const auto found = fields.find(key);
    return found == fields.end() ? std::string_view{} : std::string_view(found->second);
  }
};

// strtod, not std::from_chars: libc++ keeps the double overload in a dylib that older macOS
// releases lack (src/data/decimal.hpp).
std::optional<double> parse_number(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  const std::string copy(text);
  char* end = nullptr;
  const double value = std::strtod(copy.c_str(), &end);
  if (end != copy.c_str() + copy.size()) {
    return std::nullopt;
  }
  return value;
}

std::vector<std::string_view> words(std::string_view line) {
  std::vector<std::string_view> out;
  while (!line.empty()) {
    const std::size_t space = line.find(' ');
    const std::string_view word = line.substr(0, space);
    if (!word.empty()) {
      out.push_back(word);
    }
    if (space == std::string_view::npos) {
      break;
    }
    line.remove_prefix(space + 1);
  }
  return out;
}

std::expected<void, std::string> parse_line(std::string_view line, Section& out) {
  const std::size_t space = line.find(' ');
  if (space == std::string_view::npos) {
    return std::unexpected("a line with no value: " + std::string(line));
  }
  const std::string_view key = line.substr(0, space);
  const std::string_view rest = line.substr(space + 1);
  const std::vector<std::string_view> w = words(rest);
  const auto bad = [&line] { return std::unexpected("a malformed line: " + std::string(line)); };
  if (key == "check") {
    if (w.size() < 3 || (w[1] != "pass" && w[1] != "fail")) {
      return bad();
    }
    out.checks.push_back(
        {.name = std::string(w[0]), .passed = w[1] == "pass", .worst = std::string(w[2])});
  } else if (key == "peak_rss") {
    const auto bytes = w.size() == 2 ? parse_number(w[1]) : std::nullopt;
    if (!bytes) {
      return bad();
    }
    out.rss[std::string(w[0])] = *bytes;
  } else if (key == "variant") {
    if (w.size() != 2) {
      return bad();
    }
    out.benchmarks[std::string(w[0])] = std::string(w[1]);
  } else if (key == "estimate") {
    const auto mean = w.size() == 4 ? parse_number(w[2]) : std::nullopt;
    const auto se = w.size() == 4 ? parse_number(w[3]) : std::nullopt;
    if (!mean || !se) {
      return bad();
    }
    out.estimates.push_back(
        {.variant = std::string(w[0]), .quantity = std::string(w[1]), .mean = *mean, .se = *se});
  } else if (key == "step") {
    const bool sized = w.size() == 5 || w.size() == 6;
    const auto h = sized ? parse_number(w[2]) : std::nullopt;
    const auto bias = sized ? parse_number(w[3]) : std::nullopt;
    const auto bias_se = sized ? parse_number(w[4]) : std::nullopt;
    if (!h || !bias || !bias_se) {
      return bad();
    }
    out.steps.push_back({.scheme = std::string(w[0]),
                         .input = std::string(w[1]),
                         .h = *h,
                         .bias = *bias,
                         .bias_se = *bias_se});
  } else if (!out.fields.emplace(std::string(key), std::string(rest)).second) {
    return std::unexpected("a repeated key: " + std::string(key));
  }
  return {};
}

std::expected<Section, std::string> parse_section(std::string_view body) {
  Section out;
  while (!body.empty()) {
    const std::size_t newline = body.find('\n');
    const std::string_view line = body.substr(0, newline);
    if (!line.empty()) {
      if (auto parsed = parse_line(line, out); !parsed) {
        return std::unexpected(parsed.error());
      }
    }
    if (newline == std::string_view::npos) {
      break;
    }
    body.remove_prefix(newline + 1);
  }
  for (const std::string_view key : {"workload", "targets", "paths", "inputs", "valid",
                                     "tape_bytes_per_path", "nonzero_payoff_fraction"}) {
    if (out.field(key).empty()) {
      return std::unexpected("an AAD section has no " + std::string(key));
    }
  }
  if (!out.benchmarks.contains("price")) {
    return std::unexpected("an AAD section has no price variant");
  }
  return out;
}

std::string format(const char* spec, double x) {
  std::array<char, 64> buffer{};
  std::snprintf(buffer.data(), buffer.size(), spec, x);
  return buffer.data();
}

std::string cell(std::string_view text) {
  std::string out(text);
  std::ranges::replace(out, '|', '/');
  return out;
}

std::string header_value(const ResultsFile& file, std::string_view key) {
  return cell(file.value(key).value_or("absent"));
}

bool section_valid(const Section& section) {
  return section.field("valid") == "yes" && !section.checks.empty() &&
         std::ranges::all_of(section.checks, [](const Check& c) { return c.passed; });
}

const Step* find_step(const Section& section, std::string_view variant, std::string_view quantity) {
  const std::string_view scheme = variant == "bump-central"   ? "central"
                                  : variant == "bump-forward" ? "forward"
                                                              : "";
  for (const Step& step : section.steps) {
    if (step.scheme == scheme && (step.input == quantity || step.input == "all")) {
      return &step;
    }
  }
  return nullptr;
}

std::string variant_note(const Section& section, std::string_view variant,
                         const std::map<std::string, double, std::less<>>& times) {
  const double inputs = parse_number(section.field("inputs")).value_or(0.0);
  if (variant == "tape") {
    const auto record = times.find("tape-record");
    if (record == times.end()) {
      return "value and every Greek";
    }
    const double reverse = times.at("tape") - record->second;
    return "value and every Greek; reverse sweep (tape - tape-record) " +
           format("%.4g", reverse / kNanosecondsPerMillisecond) + " ms, " +
           format("%.3g", reverse / times.at("price")) + " x primal";
  }
  if (variant == "tape-skip") {
    return "the tape with the zero-adjoint skip (B9)";
  }
  if (variant == "tape-record") {
    return "record and rewind, no reverse sweep";
  }
  if (variant == "hand") {
    return "hand-written adjoint";
  }
  if (variant == "bump-central") {
    return format("%.0f", (2.0 * inputs) + 1.0) + " pricings expected (2N + 1)";
  }
  if (variant == "bump-forward") {
    return format("%.0f", inputs + 1.0) + " pricings expected (N + 1)";
  }
  return "value only";
}

std::string cost_table(const Section& section,
                       const std::map<std::string, double, std::less<>>& times) {
  const bool valid = section_valid(section);
  const double paths = parse_number(section.field("paths")).value_or(0.0);
  const double primal = times.at("price");
  const auto primal_rss = section.rss.find("price");
  std::string out =
      "| variant | time per run (ms) | paths/s | x scalar primal | x fastest primal | "
      "peak RSS (MiB) | RSS / primal | tape B/path | note |\n"
      "|---|---|---|---|---|---|---|---|---|\n";
  for (const std::string_view variant : kVariantOrder) {
    const auto time = times.find(variant);
    if (time == times.end()) {
      continue;
    }
    const auto rss = section.rss.find(variant);
    const bool taped = variant.starts_with("tape");
    out += "| " + std::string(variant) + " | " +
           format("%.4g", time->second / kNanosecondsPerMillisecond) + " | " +
           format("%.4g", paths * kNanosecondsPerSecond / time->second) + " | " +
           (valid ? format("%.3g", time->second / primal) : std::string("INVALID")) + " | " +
           (valid ? "pending stage 5" : "INVALID") + " | " +
           (rss == section.rss.end() ? std::string("absent")
                                     : format("%.1f", rss->second / kBytesPerMebibyte)) +
           " | " +
           (rss == section.rss.end() || primal_rss == section.rss.end()
                ? std::string("absent")
                : format("%.2f", rss->second / primal_rss->second)) +
           " | " + (taped ? cell(section.field("tape_bytes_per_path")) : std::string("-")) + " | " +
           variant_note(section, variant, times) + " |\n";
  }
  return out;
}

std::string accuracy_row(std::string_view quantity, std::string_view variant,
                         std::string_view estimate, double se, const Step* step, double seconds) {
  const double bias = step == nullptr ? 0.0 : step->bias;
  const double rmse_squared = (se * se) + (bias * bias);
  return "| " + std::string(quantity) + " | " + std::string(variant) + " | " +
         std::string(estimate) + " | " + format("%.4g", se) + " | " +
         (step == nullptr ? std::string("0 (pathwise)") : format("%.3g", step->bias)) + " | " +
         (step == nullptr ? std::string("-") : format("%.2g", step->bias_se)) + " | " +
         (step == nullptr ? std::string("-") : format("%.4g", step->h)) + " | " +
         format("%.3g", rmse_squared * seconds) + " |\n";
}

std::string accuracy_table(const Section& section,
                           const std::map<std::string, double, std::less<>>& times) {
  std::string out =
      "| quantity | variant | estimate | SE | bias (h study) | bias SE | h | "
      "RMSE^2 x time (s) |\n"
      "|---|---|---|---|---|---|---|---|\n";
  const auto seconds = [&times](std::string_view variant) {
    const auto found = times.find(variant);
    return found == times.end() ? 0.0 : found->second / kNanosecondsPerSecond;
  };
  if (section.field("kind") == "lax-wendroff") {
    // Deterministic: no SE, and the bias is the study's at the worst input.
    for (const std::string_view variant : {"tape", "hand", "bump-central", "bump-forward"}) {
      if (times.contains(variant)) {
        out += accuracy_row("gradient (worst input)", variant, "-", 0.0,
                            find_step(section, variant, "all"), seconds(variant));
      }
    }
    return out;
  }
  for (const Estimate& e : section.estimates) {
    const bool value_row = e.quantity == "value";
    // The value is Price()'s bits in every variant, and tape-skip's Greeks are the tape's.
    if ((value_row && e.variant != "price") || e.variant == "tape-skip") {
      continue;
    }
    out += accuracy_row(e.quantity, e.variant, format("%.8g", e.mean), e.se,
                        value_row ? nullptr : find_step(section, e.variant, e.quantity),
                        seconds(e.variant));
  }
  return out;
}

std::expected<std::string, std::string> file_tables(const ResultsFile& file) {
  std::vector<const ResultsSection*> sections;
  for (const ResultsSection& section : file.sections) {
    if (section.name.starts_with(kAadSectionPrefix)) {
      sections.push_back(&section);
    }
  }
  if (sections.empty()) {
    return std::string();
  }
  const ResultsSection* benchmark = file.section(kBenchmarkSection);
  if (benchmark == nullptr) {
    return std::unexpected("no benchmark section");
  }
  auto json = parse_json(benchmark->body);
  if (!json) {
    return std::unexpected("benchmark JSON: " + json.error());
  }
  std::string out = "\n#### AAD cost and accuracy";
  out += file.required("reportable") == "yes" ? "" : " (ITERATION ONLY: not results)";
  out += "\n\n";
  for (const std::string_view key :
       {"bench_scale", "bench_threads", "bench_thread_setup", "bench_instruction_set",
        "bench_fastest_primal", "tape_config", "bench_rss_probe", "gradient_check"}) {
    out += "- " + std::string(key) + ": " + header_value(file, key) + "\n";
  }
  out += "- commit: " + header_value(file, "commit") + "\n";
  for (const ResultsSection* raw : sections) {
    auto section = parse_section(raw->body);
    if (!section) {
      return std::unexpected(raw->name + ": " + section.error());
    }
    std::map<std::string, double, std::less<>> times;
    for (const auto& [variant, name] : section->benchmarks) {
      const auto mean = benchmark_mean_time(*json, name);
      const auto ns = mean ? parse_number(*mean) : std::nullopt;
      if (!ns || !(*ns > 0.0)) {
        return std::unexpected("no benchmark entry for " + name);
      }
      times[variant] = *ns;
    }
    const bool valid = section_valid(*section);
    out += "\n##### " + cell(section->field("workload")) + " (" + cell(section->field("targets")) +
           ")" + (valid ? "" : ": INVALID") + "\n\n";
    out += cell(section->field("description")) +
           ". Differences from the target: " + cell(section->field("differences")) + ".\n\n";
    out += "Paths " + cell(section->field("paths")) + " in " + cell(section->field("batches")) +
           " batches, " + cell(section->field("threads")) + " thread(s), seed " +
           cell(section->field("seed")) + "; nonzero-payoff fraction " +
           cell(section->field("nonzero_payoff_fraction")) + "; tape " +
           cell(section->field("tape_statements_per_path")) + " statements per path.\n\n";
    out += "Checks:";
    for (const Check& c : section->checks) {
      out += " " + cell(c.name) + " " + (c.passed ? "pass" : "FAIL") + " (" + cell(c.worst) +
             " of its bound);";
    }
    out += "\n\n" + cost_table(*section, times) + "\n" + accuracy_table(*section, times);
  }
  return out;
}

}  // namespace

std::expected<std::string, std::string> render_aad_tables(std::span<const ResultsFile> files) {
  std::string out;
  for (const ResultsFile& file : files) {
    auto tables = file_tables(file);
    if (!tables) {
      return std::unexpected(tables.error());
    }
    out += *tables;
  }
  return out;
}

}  // namespace aadp::results
