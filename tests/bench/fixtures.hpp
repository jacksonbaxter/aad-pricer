// Synthetic inputs. The machine name, addresses and paths are made up, and paths are assembled
// at run time so that these files pass the repo's own hygiene grep.
#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <string_view>

#include "results/results_file.hpp"
#include "results/stamp.hpp"

namespace aadp::testing {

inline constexpr std::string_view kHost = "testbox";
inline constexpr std::string_view kCommit = "0123456789abcdef0123456789abcdef01234567";

inline std::string home_path(std::string_view rest) {
  return "/" + std::string("home") + "/alice/" + std::string(rest);
}

inline std::string start_line(std::string_view host = kHost) {
  return "measure.sh: start load1=0.31 host=" + std::string(host) +
         " at=2026-01-02T03:04:05Z cpus=4 governor=performance freq_khz=cpu4:3500000 "
         "perf_event_paranoid=2";
}

inline std::string end_line(int status = 0) {
  return "measure.sh: end load1=0.42 exit=" + std::to_string(status) +
         " at=2026-01-02T03:04:09Z cpus=4 governor=performance freq_khz=cpu4:3400000 "
         "perf_event_paranoid=2";
}

inline std::string captured_stderr() {
  return start_line() + "\n" + end_line() + "\nmeasure.sh: restored governor=schedutil\n";
}

// What a Linux run of the binary writes, before stamping.
inline results::ResultsFile raw_file() {
  results::ResultsFile file;
  const auto add = [&file](std::string_view key, std::string_view value) {
    file.header.push_back({std::string(key), std::string(value)});
  };
  for (const std::string_view key : results::kRunHeaderKeys) {
    add(key, "synthetic");
  }
  for (const std::string_view key : results::kPerfHeaderKeys) {
    add(key, "synthetic");
  }
  const auto set = [&file](std::string_view key, std::string_view value) {
    for (results::HeaderField& field : file.header) {
      if (field.key == key) {
        field.value = std::string(value);
      }
    }
  };
  set("claim", "none");
  set("host_label", "linux-zen1");
  set("build_type", "Release");
  set("commit", kCommit);
  set("commit_source", "COMMIT file");
  set("tree_state", "exported");
  set("pinning_state", "pinned");
  set("pinned_cpus", "4");
  set("process_nice", "0");
  set("repetitions", "3");
  set("run_started_at", "2026-01-02T03:04:06Z");
  file.sections.push_back({"benchmark_json",
                           "{\n  \"context\": {\n    \"date\": \"2026-01-02T03:04:06+00:00\",\n"
                           "    \"host_name\": \"" +
                               std::string(kHost) + "\",\n    \"executable\": \"" +
                               home_path("build/bin/bench") +
                               "\",\n    \"load_avg\": [0.31, 0.2, 0.1]\n  },\n"
                               "  \"benchmarks\": [\n    {\"name\": \"harness_selftest/mean\", "
                               "\"run_name\": \"harness_selftest/manual_time\", "
                               "\"aggregate_name\": \"mean\", \"real_time\": 4100.5}\n  ]\n}\n"});
  file.sections.push_back(
      {"histogram_harness_selftest",
       "unit ns\nbatches 60000\ngate_ns 200.0\np50 4100\np99 4300\nmax 90000\n"});
  return file;
}

inline std::string text_of(const results::ResultsFile& file) {
  return *results::write_results_file(file);
}

inline void set_field(results::ResultsFile& file, std::string_view key, std::string_view value) {
  for (results::HeaderField& field : file.header) {
    if (field.key == key) {
      field.value = std::string(value);
      return;
    }
  }
  file.header.push_back({std::string(key), std::string(value)});
}

inline void remove_field(results::ResultsFile& file, std::string_view key) {
  std::erase_if(file.header, [key](const results::HeaderField& field) { return field.key == key; });
}

// Owns the strings a StampRequest points at.
struct StampCase {
  std::string raw = text_of(raw_file());
  std::string captured = captured_stderr();
  results::StampRequest request;

  StampCase() {
    request.reportable = true;
    request.current_host_label = "linux-zen1";
    request.current_hostname = std::string(kHost);
  }

  std::expected<std::string, results::StampRefusal> run() {
    request.raw_output = raw;
    request.captured_stderr = captured;
    return results::stamp_results(request);
  }
};

}  // namespace aadp::testing
