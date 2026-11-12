// Prints the markdown results table for the given files and directories, and with --docs writes
// or checks RESULTS.md and the README's headline (results/docs.hpp). It is the only way the docs
// get numbers. Each directory under results/ is named for its host, and a file filed under
// another host's directory is refused.
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "results/docs.hpp"
#include "results/results_file.hpp"
#include "results/table.hpp"

namespace {

namespace fs = std::filesystem;

constexpr int kExitRefused = 1;
constexpr int kExitUsage = 2;
// --iteration admits files that are not reportable, under a banner: for looking at a run while
// working on it, never for the docs.
constexpr std::string_view kUsage =
    "usage: aadp_results_table [--one-table] [--iteration] <file or directory>...\n"
    "       aadp_results_table --docs <repo root> --check | --write\n";
constexpr std::string_view kSpecName = "headline.csv";

bool is_host_directory(std::string_view name) {
  return name == "mac-m4max" || name == "linux-zen1" || name == "other";
}

// scripts/fault_injection.py files its report beside the host's results, and it holds no
// benchmark. The first line decides, not the name, so a results file cannot leave the table by
// being renamed, and any other file in the directory is still refused. Outside a host directory
// nothing is left out.
bool is_fault_report(const fs::path& path) {
  if (!is_host_directory(path.parent_path().filename().string())) {
    return false;
  }
  std::ifstream in(path, std::ios::binary);
  std::string first_line;
  std::getline(in, first_line);
  return first_line == "aadp-fault-injection 1";
}

void collect(const fs::path& path, std::vector<fs::path>& out) {
  if (fs::is_directory(path)) {
    for (const auto& entry : fs::recursive_directory_iterator(path)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".txt") {
        continue;
      }
      if (is_fault_report(entry.path())) {
        std::cerr << "results_table: left out " << entry.path().filename().string()
                  << ": a fault-injection report, not a benchmark result\n";
      } else {
        out.push_back(entry.path());
      }
    }
  } else {
    out.push_back(path);
  }
}

std::string slurp(const fs::path& path) {
  const std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

// results/ holds the spec and one directory per host, each of .txt files, and nothing else.
bool read_results_tree(const fs::path& results, aadp::results::DocsInputs& inputs) {
  std::vector<fs::path> entries;
  for (const auto& entry : fs::directory_iterator(results)) {
    entries.push_back(entry.path());
  }
  std::ranges::sort(entries);
  for (const fs::path& path : entries) {
    const std::string name = path.filename().string();
    if (fs::is_regular_file(path) && name == kSpecName) {
      continue;
    }
    if (!fs::is_directory(path) || !is_host_directory(name)) {
      std::cerr << "results_table: results/" << name << " is neither the spec nor a host\n";
      return false;
    }
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(path)) {
      files.push_back(entry.path());
    }
    std::ranges::sort(files);
    for (const fs::path& file : files) {
      if (!fs::is_regular_file(file) || file.extension() != ".txt") {
        std::cerr << "results_table: results/" << name << "/" << file.filename().string()
                  << " is not a results file\n";
        return false;
      }
      inputs.files.push_back(
          {.host_dir = name, .name = file.filename().string(), .text = slurp(file)});
    }
  }
  return true;
}

// Compares or writes one generated file. Returns false on a difference.
bool settle(const fs::path& path, std::string_view label, const std::string& wanted, bool write) {
  const std::string current = fs::exists(path) ? slurp(path) : std::string();
  if (write) {
    std::ofstream(path, std::ios::binary) << wanted;
    return true;
  }
  const std::size_t line = aadp::results::first_difference_line(current, wanted);
  if (line != 0) {
    std::cerr << "results_table: " << label << " differs from a fresh generation, first at line "
              << line << ". Run aadp_results_table --docs . --write and commit the result\n";
    return false;
  }
  return true;
}

int run_docs(const fs::path& root, bool write) {
  aadp::results::DocsInputs inputs;
  inputs.spec = slurp(root / "results" / kSpecName);
  inputs.benchmarks = slurp(root / "BENCHMARKS.md");
  if (!read_results_tree(root / "results", inputs)) {
    return kExitRefused;
  }
  const auto spec = aadp::results::parse_headline_spec(inputs.spec);
  if (!spec) {
    std::cerr << "results_table: " << spec.error() << "\n";
    return kExitRefused;
  }
  for (const auto& run : spec->measured_runs) {
    inputs.measured_runs[run.source] = slurp(root / run.source);
  }
  const auto docs = aadp::results::render_docs(inputs);
  if (!docs) {
    std::cerr << "results_table: " << docs.error() << "\n";
    return kExitRefused;
  }
  const fs::path readme_path = root / "README.md";
  const auto readme = aadp::results::splice_headline(slurp(readme_path), docs->headline);
  if (!readme) {
    std::cerr << "results_table: " << readme.error() << "\n";
    return kExitRefused;
  }
  const bool readme_ok = settle(readme_path, "README.md headline", *readme, write);
  const bool results_ok = settle(root / "RESULTS.md", "RESULTS.md", docs->results_md, write);
  if (!readme_ok || !results_ok) {
    return kExitRefused;
  }
  std::cout << "results_table: README.md headline and RESULTS.md "
            << (write ? "written" : "match a fresh generation") << "\n";
  return 0;
}

}  // namespace

// NOLINTNEXTLINE(misc-const-correctness): the standard fixes the signature of main.
int main(int argc, char** argv) try {
  if (argc == 4 && std::string_view(argv[1]) == "--docs") {
    const std::string_view mode = argv[3];
    if (mode != "--check" && mode != "--write") {
      std::cerr << kUsage;
      return kExitUsage;
    }
    return run_docs(argv[2], mode == "--write");
  }
  bool one_table = false;
  aadp::results::TableOptions options;
  std::vector<fs::path> paths;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--one-table") {
      one_table = true;
    } else if (arg == "--iteration") {
      options.iteration = true;
    } else if (arg.starts_with("--")) {
      std::cerr << kUsage;
      return kExitUsage;
    } else {
      std::error_code error;
      if (!fs::exists(arg, error)) {
        std::cerr << "results_table: no such path: " << arg << "\n";
        return kExitRefused;
      }
      collect(arg, paths);
    }
  }
  if (paths.empty()) {
    std::cerr << kUsage;
    return kExitUsage;
  }
  std::ranges::sort(paths);

  std::vector<aadp::results::ResultsFile> files;
  for (const fs::path& path : paths) {
    const std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    auto file = aadp::results::parse_results_file(text.str());
    if (!file) {
      std::cerr << "results_table: " << path.filename().string() << ": " << file.error() << "\n";
      return kExitRefused;
    }
    const std::string parent = path.parent_path().filename().string();
    if (is_host_directory(parent) && file->value("host_label") != parent) {
      std::cerr << "results_table: " << path.filename().string() << " is under " << parent
                << " but its header says otherwise\n";
      return kExitRefused;
    }
    files.push_back(std::move(*file));
  }

  const auto table = one_table ? aadp::results::render_host_table(files, options)
                               : aadp::results::render_tables(files, options);
  if (!table) {
    std::cerr << "results_table: " << table.error() << "\n";
    return kExitRefused;
  }
  std::cout << *table;
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "results_table: %s\n", error.what());
  return kExitRefused;
}
