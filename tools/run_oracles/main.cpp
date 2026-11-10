// aadp_run_oracles run --plan <file> --references <file> [--estimator reference|production]
//     [--data <dir>] [--smoke] [--threads <n>]
// aadp_run_oracles pilot --references <file> --seed <n> --out <file> [--divisor <n>]
//     [--threads <n>]
// aadp_run_oracles pilot --rows stage8 --estimator production --plan <file> --references <file>
//     --data <dir> --seed <n> --out <file> --ladder <file> [--second-seed <n>] [--divisor <n>]
//     [--threads <n>]
// aadp_run_oracles emit-plan --references <file> --pilot <file> --measured-seed <n> --out <file>
//     [--pilot-stage8 <file> --ladder <file>]
// aadp_run_oracles fingerprint --plan <file> --references <file> --out <file> [--data <dir>]
//     [--threads <n>]
//
// run exits non-zero on any failed, skipped or unresolved run row, or if the row count differs
// from K. The reference estimator runs the rows of run 1 and lists the others; the production
// estimator runs every row and refuses a plan with an awaiting row.
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "oracle/plan.hpp"
#include "oracle/references.hpp"
#include "production.hpp"
#include "runner.hpp"
#include "svi/chain_surface.hpp"

namespace {

using Arguments = std::map<std::string, std::string, std::less<>>;

// The surface rows' lines rest on the chain, so the file carries its attribution.
constexpr std::string_view kFingerprintHeader =
    "# Run 2's development-seed fingerprint, from aadp_run_oracles fingerprint: every plan row\n"
    "# through the production estimator at seed 20261128 and N / 1024 paths, as hex floats.\n"
    "# NOTES.md, \"Stage 8\", registers it; each host keeps its own file. The surface rows rest\n"
    "# on TAIFEX settlements of trade date 2024-11-15, not tradable quotes, reaching 33 days.\n"
    "# Data: TAIFEX via data.gov.tw, Open Government Data License (NOTICE).\n";

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

std::string Optional(const Arguments& arguments, std::string_view key,
                     const std::string& fallback) {
  const auto found = arguments.find(key);
  return found == arguments.end() ? fallback : found->second;
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

aadp::Plan LoadPlan(const Arguments& arguments) {
  auto plan = aadp::DecodePlan(ReadFile(Required(arguments, "plan")));
  if (!plan) {
    throw std::runtime_error("plan line " + std::to_string(plan.error().line) + ": " +
                             plan.error().message);
  }
  return *std::move(plan);
}

// The fitted chain, or nothing without --data. A --data that does not load is an error.
std::optional<aadp::svi::ChainSurface> LoadSurface(const Arguments& arguments) {
  const auto found = arguments.find("data");
  if (found == arguments.end()) return std::nullopt;
  auto surface = aadp::oracle_runner::LoadChainSurface(found->second);
  if (!surface) throw std::runtime_error("data: " + surface.error());
  return *std::move(surface);
}

int Run(const Arguments& arguments, std::uint32_t threads) {
  using namespace aadp::oracle_runner;
  const aadp::Plan plan = LoadPlan(arguments);
  const std::string estimator = Optional(arguments, "estimator", "reference");
  if (estimator == "reference") {
    RunOptions options;
    options.smoke = arguments.contains("smoke");
    options.threads = threads;
    return ExitCode(RunPlan(plan, LoadReferences(arguments), options, std::cout));
  }
  if (estimator != "production") throw std::runtime_error("unknown --estimator " + estimator);
  const auto surface = LoadSurface(arguments);
  if (!surface) std::cout << "NO DATA: the surface rows are skipped and the run fails.\n";
  ProductionOptions options;
  options.smoke = arguments.contains("smoke");
  options.threads = threads;
  return ExitCode(RunPlanProduction(plan, LoadReferences(arguments), surface ? &*surface : nullptr,
                                    options, std::cout));
}

int PilotStage8(const Arguments& arguments, aadp::oracle_runner::PilotConfig config) {
  using namespace aadp::oracle_runner;
  if (Optional(arguments, "estimator", "") != "production") {
    throw std::runtime_error("pilot --rows stage8 needs --estimator production");
  }
  const aadp::Plan plan = LoadPlan(arguments);
  if (const auto refusal = RefusePilotSeed(config.seed, plan.header)) {
    std::fprintf(stderr, "%s\n", refusal->c_str());
    return 2;
  }
  Stage8PilotConfig stage8{.pilot = config, .second_seed = Number(arguments, "second-seed", 0)};
  if (stage8.second_seed != 0) {
    if (const auto refusal = RefusePilotSeed(stage8.second_seed, plan.header)) {
      std::fprintf(stderr, "second seed: %s\n", refusal->c_str());
      return 2;
    }
  }
  config.batches = plan.header.batches;
  stage8.pilot.batches = plan.header.batches;
  const auto surface = LoadSurface(arguments);
  if (!surface) throw std::runtime_error("pilot --rows stage8 needs --data");
  const Stage8Pilot pilot = RunStage8Pilot(PlanTemplate(), LoadReferences(arguments), *surface,
                                           stage8, plan.header.critical, std::cout);
  WriteFile(Required(arguments, "out"), EncodeStage8Pilot(pilot.rows, config));
  WriteFile(Required(arguments, "ladder"), EncodeLadder(pilot.ladders));
  return 0;
}

int EmitPlan(const Arguments& arguments) {
  using namespace aadp::oracle_runner;
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
  std::optional<Stage8Inputs> stage8;
  if (arguments.contains("pilot-stage8")) {
    stage8.emplace();
    if (!DecodePilot(ReadFile(Required(arguments, "pilot-stage8")), stage8->pilot)) {
      std::fprintf(stderr, "bad stage 8 pilot file\n");
      return 2;
    }
    if (stage8->pilot.config.seed == measured_seed) {
      std::fprintf(stderr, "measured seed must differ from the stage 8 pilot's\n");
      return 2;
    }
    auto ladder = DecodeLadderChoices(ReadFile(Required(arguments, "ladder")));
    if (!ladder) {
      std::fprintf(stderr, "bad ladder file\n");
      return 2;
    }
    stage8->ladder = *std::move(ladder);
  }
  const auto plan = BuildPlan(PlanTemplate(), LoadReferences(arguments), pilot, measured_seed,
                              stage8 ? &*stage8 : nullptr);
  WriteFile(Required(arguments, "out"), aadp::EncodePlan(plan));
  return 0;
}

}  // namespace

int main(int argc, char** argv) try {
  using namespace aadp::oracle_runner;
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s run|pilot|emit-plan|fingerprint ...\n", argv[0]);
    return 2;
  }
  const std::string_view command = argv[1];
  const Arguments arguments = ParseArguments(argc, argv);
  const auto threads = static_cast<std::uint32_t>(Number(arguments, "threads", DefaultThreads()));
  if (threads == 0) {
    std::fprintf(stderr, "--threads must be positive\n");
    return 2;
  }

  if (command == "run") return Run(arguments, threads);
  if (command == "pilot") {
    PilotConfig config;
    config.seed = Number(arguments, "seed", 0);
    config.paths_divisor = Number(arguments, "divisor", 4);
    config.threads = threads;
    if (config.seed == 0 || config.paths_divisor == 0) {
      std::fprintf(stderr, "pilot needs --seed and a positive --divisor\n");
      return 2;
    }
    const std::string rows = Optional(arguments, "rows", "run1");
    if (rows == "stage8") return PilotStage8(arguments, config);
    if (rows != "run1") throw std::runtime_error("unknown --rows " + rows);
    const auto pilot = RunPilot(PlanTemplate(), LoadReferences(arguments), config, std::cout);
    WriteFile(Required(arguments, "out"), EncodePilot(pilot, config));
    return 0;
  }
  if (command == "emit-plan") return EmitPlan(arguments);
  if (command == "fingerprint") {
    const auto surface = LoadSurface(arguments);
    WriteFile(Required(arguments, "out"),
              std::string(kFingerprintHeader) +
                  Fingerprint(LoadPlan(arguments), LoadReferences(arguments),
                              surface ? &*surface : nullptr, threads));
    return 0;
  }
  std::fprintf(stderr, "unknown command %s\n", argv[1]);
  return 2;
} catch (const std::exception& error) {
  std::fprintf(stderr, "aadp_run_oracles: %s\n", error.what());
  return 2;
}
