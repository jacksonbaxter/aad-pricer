// aadp_run_oracles run --plan <file> --references <file> [--smoke] [--threads <n>]
// aadp_run_oracles pilot --references <file> --seed <n> --out <file> [--divisor <n>] [--threads
// <n>] aadp_run_oracles emit-plan --references <file> --pilot <file> --measured-seed <n> --out
// <file>
//
// run exits non-zero on any failed or skipped run row, or if the row count differs from K.
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include "runner.hpp"

namespace {

using Arguments = std::map<std::string, std::string, std::less<>>;

Arguments ParseArguments(int argc, char** argv) {
  Arguments arguments;
  for (int i = 2; i < argc; ++i) {
    const std::string_view key = argv[i];
    if (key == "--smoke") {
      arguments.emplace("smoke", "1");
    } else if (key.starts_with("--") && i + 1 < argc) {
      arguments.emplace(key.substr(2), argv[++i]);
    } else {
      throw std::runtime_error("bad argument " + std::string(argv[i]));
    }
  }
  return arguments;
}

std::string Required(const Arguments& arguments, std::string_view key) {
  const auto found = arguments.find(key);
  if (found == arguments.end()) {
    throw std::runtime_error("missing --" + std::string(key));
  }
  return found->second;
}

std::string ReadFile(const std::string& path) {
  const std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("cannot open " + path);
  }
  std::ostringstream text;
  text << file.rdbuf();
  return text.str();
}

void WriteFile(const std::string& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary);
  file << text;
  if (!file.good()) {
    throw std::runtime_error("cannot write " + path);
  }
}

std::uint64_t Number(const Arguments& arguments, std::string_view key, std::uint64_t fallback) {
  const auto found = arguments.find(key);
  if (found == arguments.end()) return fallback;
  std::uint64_t value = 0;
  const std::string& text = found->second;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) {
    throw std::runtime_error("bad number for --" + std::string(key));
  }
  return value;
}

std::uint32_t DefaultThreads() {
  return std::clamp<std::uint32_t>(std::thread::hardware_concurrency(), 1U, 8U);
}

aadp::ReferenceTable LoadReferences(const Arguments& arguments) {
  auto references = aadp::DecodeReferences(ReadFile(Required(arguments, "references")));
  if (!references) {
    throw std::runtime_error("references line " + std::to_string(references.error().line) + ": " +
                             references.error().message);
  }
  return *std::move(references);
}

}  // namespace

int main(int argc, char** argv) try {
  using namespace aadp::oracle_runner;
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s run|pilot|emit-plan ...\n", argv[0]);
    return 2;
  }
  const std::string_view command = argv[1];
  const Arguments arguments = ParseArguments(argc, argv);
  const auto threads = static_cast<std::uint32_t>(Number(arguments, "threads", DefaultThreads()));
  if (threads == 0) {
    std::fprintf(stderr, "--threads must be positive\n");
    return 2;
  }

  if (command == "run") {
    const auto plan = aadp::DecodePlan(ReadFile(Required(arguments, "plan")));
    if (!plan) {
      std::fprintf(stderr, "plan line %zu: %s\n", plan.error().line, plan.error().message.c_str());
      return 2;
    }
    RunOptions options;
    options.smoke = arguments.contains("smoke");
    options.threads = threads;
    return ExitCode(RunPlan(*plan, LoadReferences(arguments), options, std::cout));
  }
  if (command == "pilot") {
    PilotConfig config;
    config.seed = Number(arguments, "seed", 0);
    config.paths_divisor = Number(arguments, "divisor", 4);
    config.threads = threads;
    if (config.seed == 0 || config.paths_divisor == 0) {
      std::fprintf(stderr, "pilot needs --seed and a positive --divisor\n");
      return 2;
    }
    const auto rows = RunPilot(PlanTemplate(), LoadReferences(arguments), config, std::cout);
    WriteFile(Required(arguments, "out"), EncodePilot(rows, config));
    return 0;
  }
  if (command == "emit-plan") {
    PilotFile pilot;
    if (!DecodePilot(ReadFile(Required(arguments, "pilot")), pilot)) {
      std::fprintf(stderr, "bad pilot file\n");
      return 2;
    }
    const std::uint64_t measured_seed = Number(arguments, "measured-seed", 0);
    if (measured_seed == 0 || measured_seed == pilot.config.seed) {
      std::fprintf(stderr, "measured seed must be set and must differ from the pilot's\n");
      return 2;
    }
    const auto plan = BuildPlan(PlanTemplate(), LoadReferences(arguments), pilot, measured_seed);
    WriteFile(Required(arguments, "out"), aadp::EncodePlan(plan));
    return 0;
  }
  std::fprintf(stderr, "unknown command %s\n", argv[1]);
  return 2;
} catch (const std::exception& error) {
  std::fprintf(stderr, "aadp_run_oracles: %s\n", error.what());
  return 2;
}
