// Prints the markdown results table for the given files and directories. It is the only way the
// README gets numbers. Each directory under results/ is named for its host, and a file filed
// under another host's directory is refused.
#include <algorithm>
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

#include "results/results_file.hpp"
#include "results/table.hpp"

namespace {

namespace fs = std::filesystem;

constexpr int kExitRefused = 1;
constexpr int kExitUsage = 2;

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

}  // namespace

// NOLINTNEXTLINE(misc-const-correctness): the standard fixes the signature of main.
int main(int argc, char** argv) try {
  bool one_table = false;
  std::vector<fs::path> paths;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--one-table") {
      one_table = true;
    } else if (arg.starts_with("--")) {
      std::cerr << "usage: aadp_results_table [--one-table] <file or directory>...\n";
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
    std::cerr << "usage: aadp_results_table [--one-table] <file or directory>...\n";
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

  const auto table =
      one_table ? aadp::results::render_host_table(files) : aadp::results::render_tables(files);
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
