// Runs the bump-size study (bump_study.hpp) and checks its committed table.
//
//   aadp_bump_study [--workload=<id>] [--scale=<d>] [--threads=<n>] [--halvings=<J>] [--out=<csv>]
//       Runs the study of every workload that owns one (or the one named) at 1 / d of its path
//       count, and writes the halving evidence as CSV. Without --out, to stdout. The numbers do
//       not depend on the thread count (pricer/runner.hpp) or the host (src/math/).
//   aadp_bump_study --verify=<csv>
//       Re-applies the rule to the committed table and checks that bench/aad_steps.hpp holds the
//       steps and biases it chooses, that every study is there, and that no study shows a
//       violation of its round-off scale.
//   aadp_bump_study --steps-header=<csv>
//       Prints bench/aad_steps.hpp for that table.
//
// Exit codes: 0 pass, 1 a check failed, 2 usage or input error.
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "aad_workloads.hpp"
#include "bump_study.hpp"
#include "pricer/bump.hpp"

namespace {

using aadp::bench::aad::StudyRow;
using aadp::bench::aad::Workload;
namespace aad = aadp::bench::aad;
using aadp::pricer::BumpScheme;

constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;
constexpr std::string_view kColumns =
    "workload,input,scheme,halvings,h,estimate,se,change,change_se,roundoff,limit,stable,chosen";

std::string Number(double x) {
  std::array<char, 40> text{};
  std::snprintf(text.data(), text.size(), "%.17g", x);
  return text.data();
}

bool ParseUnsigned(std::string_view text, std::uint64_t& out) {
  if (text.empty()) return false;
  out = 0;
  for (const char c : text) {
    if (c < '0' || c > '9' || out > (UINT64_MAX - 9) / 10) return false;
    out = (out * 10) + static_cast<std::uint64_t>(c - '0');
  }
  return true;
}

bool ParseDouble(const std::string& text, double& out) {
  if (text.empty()) return false;
  char* end = nullptr;
  out = std::strtod(text.c_str(), &end);
  return *end == '\0';
}

std::vector<const Workload*> StudyWorkloads(std::string_view only) {
  std::vector<const Workload*> out;
  for (const Workload& w : aad::Workloads()) {
    if (w.study == w.id && (only.empty() || w.id == only)) out.push_back(&w);
  }
  return out;
}

struct Key {
  std::string workload;
  std::string input;
  std::string scheme;
  auto operator<=>(const Key&) const = default;
};

struct CsvRow {
  StudyRow row;
  bool chosen = false;
};

void WriteRows(std::ostream& out, std::string_view workload, std::string_view input,
               BumpScheme scheme, const std::vector<StudyRow>& rows,
               const aad::StudyChoice& choice) {
  for (std::size_t j = 0; j < rows.size(); ++j) {
    const StudyRow& r = rows[j];
    out << workload << ',' << input << ',' << aad::SchemeName(scheme) << ',' << r.halvings << ','
        << Number(r.h) << ',' << Number(r.estimate) << ',' << Number(r.se) << ','
        << (r.has_change ? Number(r.change) : "") << ','
        << (r.has_change ? Number(r.change_se) : "") << ',' << Number(r.roundoff) << ','
        << (r.has_change ? Number(r.limit) : "") << ','
        << (r.has_change ? (r.stable ? "1" : "0") : "") << ',' << (choice.chosen == j ? "1" : "0")
        << '\n';
  }
}

void Summarise(std::string_view workload, std::string_view input, BumpScheme scheme,
               const std::vector<StudyRow>& rows, const aad::StudyChoice& choice) {
  std::cerr << "aadp_bump_study: " << workload << ' ' << input << ' ' << aad::SchemeName(scheme)
            << ": ";
  if (!choice.chosen) {
    std::cerr << "no stable step\n";
    return;
  }
  const StudyRow& r = rows[*choice.chosen];
  std::cerr << "h " << Number(r.h) << " (2^-" << r.halvings << " h0), estimate "
            << Number(r.estimate) << " SE " << Number(r.se) << ", bias " << Number(choice.bias)
            << " SE " << Number(choice.bias_se)
            << (choice.violation ? ", ROUND-OFF SCALE VIOLATED" : "") << "\n";
}

int RunStudy(std::string_view only, std::uint64_t scale, std::uint32_t threads,
             std::uint32_t halvings, const std::string& out_path) {
  std::ostringstream out;
  out << "# aadp_bump_study: seed " << aad::kSeed << ", path counts / " << scale << ", "
      << aad::kMcBatches << " batches, " << halvings << " halvings from h0\n";
  out << "# rule: bench/bump_study.hpp. change = D(h) - D(h/2); limit = R(h) + R(h/2); stable = "
         "|change| <= limit; chosen = first of two stable rows in a row\n";
  out << kColumns << '\n';
  bool ok = true;
  for (const Workload* w : StudyWorkloads(only)) {
    if (w->kind == aad::Kind::kLaxWendroff) {
      const aad::TapeShape shape = aad::MeasureTape(*w, {});
      const double n = aad::RoundingCount(static_cast<double>(shape.statements_per_path), 0, 0);
      out << "# rounding_count " << w->id << ' ' << Number(n) << '\n';
      for (const BumpScheme scheme : {BumpScheme::kCentral, BumpScheme::kForward}) {
        const std::vector<StudyRow> rows = aad::LwStudy(scheme, aad::kLwInitialStep, halvings, n);
        const aad::StudyChoice choice = aad::ChooseStep(rows, scheme);
        WriteRows(out, w->id, "all", scheme, rows, choice);
        Summarise(w->id, "all", scheme, rows, choice);
        ok = ok && choice.chosen && !choice.violation;
      }
      continue;
    }
    auto config = aad::ConfigFor(*w, scale, threads);
    if (!config) {
      std::cerr << "aadp_bump_study: " << w->id << ": the scale does not divide its batches\n";
      return kExitUsage;
    }
    config->threads = threads;
    const aad::TapeShape shape = aad::MeasureTape(*w, *config);
    const double n = aad::RoundingCount(static_cast<double>(shape.statements_per_path),
                                        config->paths / config->batches, config->batches);
    out << "# rounding_count " << w->id << ' ' << Number(n) << '\n';
    const aadp::pricer::BumpSteps h0 = aad::InitialSteps(w->market);
    for (std::size_t i = 0; i < aadp::pricer::kInputCount; ++i) {
      for (const BumpScheme scheme : {BumpScheme::kCentral, BumpScheme::kForward}) {
        const auto points = aad::PricerStudy(*w, *config, static_cast<aadp::pricer::Input>(i),
                                             scheme, h0[i], halvings, n);
        const std::vector<StudyRow> rows = aad::Tabulate(points);
        const aad::StudyChoice choice = aad::ChooseStep(rows, scheme);
        WriteRows(out, w->id, aad::InputName(i), scheme, rows, choice);
        Summarise(w->id, aad::InputName(i), scheme, rows, choice);
        ok = ok && choice.chosen && !choice.violation;
      }
    }
  }
  if (out_path.empty()) {
    std::cout << out.str();
  } else {
    std::ofstream file(out_path, std::ios::binary);
    file << out.str();
    if (!file) {
      std::cerr << "aadp_bump_study: cannot write the table\n";
      return kExitUsage;
    }
  }
  return ok ? 0 : kExitFailed;
}

std::vector<std::string> SplitCommas(const std::string& line) {
  std::vector<std::string> fields;
  std::string field;
  for (const char c : line) {
    if (c == ',') {
      fields.push_back(field);
      field.clear();
    } else {
      field.push_back(c);
    }
  }
  fields.push_back(field);
  return fields;
}

std::optional<std::map<Key, std::vector<CsvRow>>> ReadTable(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::cerr << "aadp_bump_study: cannot read " << path << "\n";
    return std::nullopt;
  }
  std::map<Key, std::vector<CsvRow>> table;
  std::string line;
  bool header = false;
  while (std::getline(in, line)) {
    if (line.empty() || line.starts_with('#')) continue;
    if (!header) {
      if (line != kColumns) {
        std::cerr << "aadp_bump_study: unexpected columns\n";
        return std::nullopt;
      }
      header = true;
      continue;
    }
    const std::vector<std::string> f = SplitCommas(line);
    if (f.size() != 13) {
      std::cerr << "aadp_bump_study: a row does not have 13 fields\n";
      return std::nullopt;
    }
    CsvRow row;
    std::uint64_t halvings = 0;
    const bool has_change = !f[7].empty();
    row.row.has_change = has_change;
    bool ok = ParseUnsigned(f[3], halvings) && ParseDouble(f[4], row.row.h) &&
              ParseDouble(f[5], row.row.estimate) && ParseDouble(f[6], row.row.se) &&
              ParseDouble(f[9], row.row.roundoff) && (f[12] == "0" || f[12] == "1");
    if (has_change) {
      ok = ok && ParseDouble(f[7], row.row.change) && ParseDouble(f[8], row.row.change_se) &&
           ParseDouble(f[10], row.row.limit) && (f[11] == "0" || f[11] == "1");
      row.row.stable = f[11] == "1";
    }
    if (!ok) {
      std::cerr << "aadp_bump_study: a row does not parse\n";
      return std::nullopt;
    }
    row.row.halvings = static_cast<std::uint32_t>(halvings);
    row.chosen = f[12] == "1";
    table[{.workload = f[0], .input = f[1], .scheme = f[2]}].push_back(row);
  }
  return table;
}

std::optional<aad::StudyChoice> ChoiceOf(const std::vector<CsvRow>& csv, BumpScheme scheme,
                                         const Key& key) {
  std::vector<StudyRow> rows;
  rows.reserve(csv.size());
  for (const CsvRow& r : csv) rows.push_back(r.row);
  for (std::size_t j = 0; j < rows.size(); ++j) {
    const StudyRow& r = rows[j];
    if (r.halvings != j || (r.has_change && (std::fabs(r.change) <= r.limit) != r.stable)) {
      std::cerr << "aadp_bump_study: " << key.workload << ' ' << key.input << ' ' << key.scheme
                << ": row " << j << " is out of order or its stable flag is wrong\n";
      return std::nullopt;
    }
  }
  const aad::StudyChoice choice = aad::ChooseStep(rows, scheme);
  for (std::size_t j = 0; j < csv.size(); ++j) {
    if (csv[j].chosen != (choice.chosen == j)) {
      std::cerr << "aadp_bump_study: " << key.workload << ' ' << key.input << ' ' << key.scheme
                << ": the chosen row is not the one the rule chooses\n";
      return std::nullopt;
    }
  }
  if (!choice.chosen || choice.violation) {
    std::cerr << "aadp_bump_study: " << key.workload << ' ' << key.input << ' ' << key.scheme
              << (choice.chosen ? ": its round-off scale is violated\n" : ": no stable step\n");
    return std::nullopt;
  }
  return choice;
}

bool SameChoice(const aad::StepChoice& committed, double h, const aad::StudyChoice& choice) {
  return committed.h == h && committed.bias == choice.bias && committed.bias_se == choice.bias_se &&
         committed.truncation == choice.truncation;
}

int Verify(const std::string& path) {
  const auto table = ReadTable(path);
  if (!table) return kExitUsage;
  bool ok = true;
  for (const Workload& w : aad::Workloads()) {
    const bool lw = w.kind == aad::Kind::kLaxWendroff;
    const std::size_t inputs = lw ? 1 : aadp::pricer::kInputCount;
    for (std::size_t i = 0; i < inputs; ++i) {
      for (const BumpScheme scheme : {BumpScheme::kCentral, BumpScheme::kForward}) {
        const Key key{.workload = std::string(w.study),
                      .input = lw ? "all" : std::string(aad::InputName(i)),
                      .scheme = std::string(aad::SchemeName(scheme))};
        const auto found = table->find(key);
        if (found == table->end()) {
          std::cerr << "aadp_bump_study: no study for " << key.workload << ' ' << key.input << ' '
                    << key.scheme << "\n";
          ok = false;
          continue;
        }
        const auto choice = ChoiceOf(found->second, scheme, key);
        if (!choice) {
          ok = false;
          continue;
        }
        const double h = found->second[*choice->chosen].row.h;
        const aad::StepChoice& committed =
            scheme == BumpScheme::kCentral ? w.central[i] : w.forward[i];
        if (!SameChoice(committed, h, *choice)) {
          std::cerr << "aadp_bump_study: " << w.id << ' ' << key.input << ' ' << key.scheme
                    << ": bench/aad_steps.hpp has h " << Number(committed.h) << ", bias "
                    << Number(committed.bias) << "; the table chooses h " << Number(h) << ", bias "
                    << Number(choice->bias) << "\n";
          ok = false;
        }
      }
    }
  }
  std::cout << (ok ? "aadp_bump_study: the committed table and steps agree\n"
                   : "aadp_bump_study: FAILED: the committed table and steps disagree\n");
  return ok ? 0 : kExitFailed;
}

std::string ConstantName(std::string_view study, BumpScheme scheme) {
  std::string name = study == "aad1-barrier"   ? "kAad1"
                     : study == "aad4-asian12" ? "kAsian12"
                     : study == "mc-asian252"  ? "kAsian252"
                                               : "kLw";
  return name + (scheme == BumpScheme::kCentral ? "Central" : "Forward");
}

int StepsHeader(const std::string& path) {
  const auto table = ReadTable(path);
  if (!table) return kExitUsage;
  std::cout
      << "// Bump steps chosen by the h study (bump_study.hpp), with the study's bias at each,\n"
         "// its SE and the rule's bound on it. Generated by aadp_bump_study --steps-header from\n"
         "// bench/bump_size_study.csv, and checked against it by aadp_bump_study --verify.\n"
         "#ifndef AADP_BENCH_AAD_STEPS_HPP_\n#define AADP_BENCH_AAD_STEPS_HPP_\n\n"
         "#include \"aad_workloads.hpp\"\n\nnamespace aadp::bench::aad {\n\n";
  for (const Workload* w : StudyWorkloads("")) {
    const bool lw = w->kind == aad::Kind::kLaxWendroff;
    for (const BumpScheme scheme : {BumpScheme::kCentral, BumpScheme::kForward}) {
      std::cout << "inline constexpr StepChoices " << ConstantName(w->id, scheme) << "{{\n";
      const std::size_t inputs = lw ? 1 : aadp::pricer::kInputCount;
      for (std::size_t i = 0; i < inputs; ++i) {
        const Key key{.workload = std::string(w->id),
                      .input = lw ? "all" : std::string(aad::InputName(i)),
                      .scheme = std::string(aad::SchemeName(scheme))};
        const auto found = table->find(key);
        if (found == table->end()) return kExitFailed;
        const auto choice = ChoiceOf(found->second, scheme, key);
        if (!choice) return kExitFailed;
        std::cout << "    {.h = " << Number(found->second[*choice->chosen].row.h)
                  << ", .bias = " << Number(choice->bias)
                  << ", .bias_se = " << Number(choice->bias_se)
                  << ", .truncation = " << Number(choice->truncation) << "},  // " << key.input
                  << "\n";
      }
      std::cout << "}};\n";
    }
  }
  std::cout << "\n}  // namespace aadp::bench::aad\n\n#endif  // AADP_BENCH_AAD_STEPS_HPP_\n";
  return 0;
}

}  // namespace

// NOLINTNEXTLINE(misc-const-correctness): the standard fixes the signature of main.
int main(int argc, char** argv) try {
  std::string only;
  std::uint64_t scale = 1;
  std::uint64_t threads = 8;
  std::uint64_t halvings = aad::kStudyHalvings;
  std::string out_path;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto value = [arg](std::string_view flag) -> std::optional<std::string_view> {
      return arg.starts_with(flag) ? std::optional(arg.substr(flag.size())) : std::nullopt;
    };
    if (const auto v = value("--verify=")) return Verify(std::string(*v));
    if (const auto v = value("--steps-header=")) return StepsHeader(std::string(*v));
    bool ok = true;
    if (const auto v = value("--workload=")) {
      only = *v;
      ok = aad::FindWorkload(only) != nullptr;
    } else if (const auto scale_text = value("--scale=")) {
      ok = ParseUnsigned(*scale_text, scale) && scale >= 1;
    } else if (const auto threads_text = value("--threads=")) {
      ok = ParseUnsigned(*threads_text, threads) && threads >= 1 &&
           threads <= aadp::pricer::kMaxThreads;
    } else if (const auto halvings_text = value("--halvings=")) {
      ok = ParseUnsigned(*halvings_text, halvings) && halvings >= 2 && halvings <= 60;
    } else if (const auto out_text = value("--out=")) {
      out_path = *out_text;
    } else {
      ok = false;
    }
    if (!ok) {
      std::cerr << "usage: aadp_bump_study [--workload=<id>] [--scale=<d>] [--threads=<n>] "
                   "[--halvings=<J>] [--out=<csv>] | --verify=<csv> | --steps-header=<csv>\n";
      return kExitUsage;
    }
  }
  return RunStudy(only, scale, static_cast<std::uint32_t>(threads),
                  static_cast<std::uint32_t>(halvings), out_path);
} catch (const std::exception& error) {
  std::fprintf(stderr, "aadp_bump_study: %s\n", error.what());
  return kExitUsage;
}
