#include "results/results_file.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "fixtures.hpp"

namespace aadp::results {
namespace {

TEST(ResultsFile, RoundTrips) {
  const ResultsFile file = testing::raw_file();
  const auto text = write_results_file(file);
  ASSERT_TRUE(text.has_value()) << text.error();
  const auto back = parse_results_file(*text);
  ASSERT_TRUE(back.has_value()) << back.error();
  EXPECT_EQ(*back, file);
}

TEST(ResultsFile, ReaderRefusesBrokenStructure) {
  const std::string magic = std::string(kResultsMagic) + "\n";
  for (const std::string& bad : std::vector<std::string>{
           "",
           "not results\n",
           magic,
           magic + "key: value\n",
           magic + "key value\n\n",
           magic + "Bad-Key: value\n\n",
           magic + ": value\n\n",
           magic + "key: value\nkey: again\n\n",
           magic + "key: tab\there\n\n",
           magic + "\nstray line\n",
           magic + "\n@section Bad Name\n",
           magic + "\n@section \n",
           magic + "\n@section a\n@section a\n",
           magic + std::string("key: nul\0x\n\n", 12),
       }) {
    EXPECT_FALSE(parse_results_file(bad).has_value()) << bad;
  }
}

TEST(ResultsFile, ReaderBoundsTheHeaderAndTheSections) {
  std::string header = std::string(kResultsMagic) + "\n";
  for (int i = 0; i < 1025; ++i) {
    header += "key" + std::to_string(i) + ": value\n";
  }
  EXPECT_FALSE(parse_results_file(header + "\n").has_value());

  std::string sections = std::string(kResultsMagic) + "\n\n";
  for (int i = 0; i < 1025; ++i) {
    sections += "@section s" + std::to_string(i) + "\n";
  }
  EXPECT_FALSE(parse_results_file(sections).has_value());
}

TEST(ResultsFile, WriterRefusesWhatTheReaderCouldNotRead) {
  ResultsFile file;
  file.header.push_back({.key = "key", .value = "line\nbreak"});
  EXPECT_FALSE(write_results_file(file).has_value());

  file = {};
  file.header.push_back({.key = "Key", .value = "value"});
  EXPECT_FALSE(write_results_file(file).has_value());

  file = {};
  file.header.push_back({.key = "key", .value = "a"});
  file.header.push_back({.key = "key", .value = "b"});
  EXPECT_FALSE(write_results_file(file).has_value());

  file = {};
  file.sections.push_back({.name = "a", .body = "no newline"});
  EXPECT_FALSE(write_results_file(file).has_value());

  file = {};
  file.sections.push_back({.name = "a", .body = "@section b\n"});
  EXPECT_FALSE(write_results_file(file).has_value());
}

// Header completeness: each required key, taken away alone, must be noticed.
TEST(ResultsFile, EveryRequiredKeyIsCheckedOneAtATime) {
  for (const std::string_view key : kRunHeaderKeys) {
    ResultsFile file = testing::raw_file();
    testing::remove_field(file, key);
    const auto missing = missing_keys(file, kRunHeaderKeys);
    ASSERT_EQ(missing.size(), 1U) << key;
    EXPECT_EQ(missing.front(), key);
  }
}

TEST(ResultsFile, AnEmptyValueCountsAsMissing) {
  ResultsFile file = testing::raw_file();
  testing::set_field(file, "cpu_model", "");
  EXPECT_EQ(missing_keys(file, kRunHeaderKeys), std::vector<std::string_view>{"cpu_model"});
}

TEST(ResultsFile, RequiredKeysCoverTheBrief) {
  // The fields the results header must carry, by the name a reader would look for.
  for (const std::string_view key : {"host_label",
                                     "cpu_model",
                                     "kernel",
                                     "os",
                                     "compiler_id",
                                     "compiler_version",
                                     "cxx_flags",
                                     "build_type",
                                     "simd_target",
                                     "simd_double_lanes",
                                     "pinning_state",
                                     "pinned_cpus",
                                     "governor",
                                     "frequency_khz",
                                     "turbo",
                                     "fp_state",
                                     "timer_source",
                                     "timer_calibrated_hz",
                                     "timer_resolution_ns",
                                     "repetitions",
                                     "warmup",
                                     "commit",
                                     "peak_rss_bytes"}) {
    EXPECT_NE(std::ranges::find(kRunHeaderKeys, key), kRunHeaderKeys.end()) << key;
  }
}

TEST(CommitHash, TakesFullLowerCaseHashesOnly) {
  EXPECT_TRUE(is_commit_hash(testing::kCommit));
  EXPECT_FALSE(is_commit_hash("unknown"));
  EXPECT_FALSE(is_commit_hash(testing::kCommit.substr(1)));
  EXPECT_FALSE(is_commit_hash(std::string(40, 'A')));
}

}  // namespace
}  // namespace aadp::results
