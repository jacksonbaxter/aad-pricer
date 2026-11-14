// The run header every results file carries (results/run_header.hpp) and the report emitter's
// session (results/report_run.hpp), outside any benchmark binary.
#include "results/run_header.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/affinity.hpp"
#include "platform/cpu_info.hpp"
#include "platform/fp_state.hpp"
#include "results/report.hpp"
#include "results/report_run.hpp"
#include "results/results_file.hpp"

namespace aadp::results {
namespace {

std::expected<std::vector<std::string>, std::string> take(std::vector<std::string> args,
                                                          RunOptions& options) {
  args.insert(args.begin(), "tool");
  std::vector<char*> argv;
  argv.reserve(args.size());
  for (std::string& arg : args) {
    argv.push_back(arg.data());
  }
  return take_run_flags(static_cast<int>(argv.size()), argv.data(), options);
}

// The key order of every committed results file, written before the header moved here
// (results/mac-m4max/aad1-barrier-20261104T071059Z.txt without the stamp's keys and the
// benchmark's own fields, which come after run_started_at).
constexpr auto kCommittedOrder = std::to_array<std::string_view>({
    "claim",
    "host_label",
    "cpu_model",
    "logical_cpus",
    "arch",
    "kernel",
    "os",
    "compiler_id",
    "compiler_version",
    "cxx_standard",
    "cxx_flags",
    "build_type",
    "simd_cmake_target",
    "simd_target",
    "simd_double_lanes",
    "pinning_state",
    "pinned_cpus",
    "meant_idle_cpus",
    "pin_method",
    "observed_cpu",
    "qos_class",
    "process_nice",
    "fp_register",
    "fp_state",
    "fp_ftz",
    "fp_daz",
    "timer_source",
    "timer_calibrated_hz",
    "timer_calibration_spread",
    "timer_granularity_ticks",
    "timer_min_delta_ticks",
    "timer_resolution_ns",
    "timer_read_cost_ns",
    "timer_note",
    "clock_source",
    "clock_getres_ns",
    "clock_resolution_ns",
    "repetitions",
    "batches_per_repetition",
    "warmup",
    "commit",
    "commit_source",
    "tree_state",
    "run_started_at",
});
constexpr auto kCommittedLateOrder = std::to_array<std::string_view>({
    "smt_siblings",
    "governor",
    "cpufreq_driver",
    "frequency_khz",
    "max_frequency_khz",
    "turbo",
    "perf_counters_source",
    "perf_counters_scope",
    "perf_cycles",
    "perf_instructions",
    "perf_counters_note",
    "peak_rss_bytes",
});

std::vector<std::string_view> keys_of(const std::vector<HeaderField>& header) {
  std::vector<std::string_view> keys;
  keys.reserve(header.size());
  for (const HeaderField& field : header) {
    keys.emplace_back(field.key);
  }
  return keys;
}

TEST(RunFlags, TakesTheRunFlagsAndKeepsTheRestInOrder) {
  RunOptions options;
  const auto rest = take({"--report", "--pin-cpu=4", "--reportable", "--source-root=src", "x",
                          "--meant-idle-cpus=5,7", "--timing", "3"},
                         options);
  ASSERT_TRUE(rest.has_value()) << rest.error();
  EXPECT_EQ(*rest, (std::vector<std::string>{"--report", "x", "--timing", "3"}));
  EXPECT_EQ(options.pin_cpu, 4);
  EXPECT_TRUE(options.reportable);
  EXPECT_EQ(options.source_root, "src");
  EXPECT_EQ(options.meant_idle_cpus, (std::vector<int>{5, 7}));
}

TEST(RunFlags, DefaultsWithNoFlags) {
  RunOptions options;
  const auto rest = take({}, options);
  ASSERT_TRUE(rest.has_value());
  EXPECT_TRUE(rest->empty());
  EXPECT_FALSE(options.pin_cpu.has_value());
  EXPECT_FALSE(options.reportable);
  EXPECT_EQ(options.source_root, ".");
  EXPECT_TRUE(options.meant_idle_cpus.empty());
}

TEST(RunFlags, RefusesARunFlagWithABadValue) {
  for (const std::string arg :
       {"--pin-cpu=", "--pin-cpu=-1", "--pin-cpu=x", "--pin-cpu=4 ",
        "--meant-idle-cpus=", "--meant-idle-cpus=none", "--meant-idle-cpus=5-"}) {
    RunOptions options;
    const auto rest = take({arg}, options);
    ASSERT_FALSE(rest.has_value()) << arg;
    EXPECT_EQ(rest.error(), "bad option " + arg);
  }
}

TEST(RunFlags, OneFlagAtATime) {
  RunOptions options;
  EXPECT_EQ(apply_run_flag("--reportable", options), true);
  EXPECT_EQ(apply_run_flag("--reportable=yes", options), false);
  EXPECT_EQ(apply_run_flag("--batches=10", options), false);
  EXPECT_EQ(apply_run_flag("--source-root=", options), true);
  EXPECT_EQ(options.source_root, "");
}

TEST(RunHeader, ARunFromATreeThatIsNotCleanIsNotReportable) {
  const RunOptions options{.pin_cpu = std::nullopt,
                           .meant_idle_cpus = {},
                           .reportable = true,
                           .source_root = "no-such-directory-for-run-header-test"};
  const auto start = start_run(options);
  ASSERT_FALSE(start.has_value());
  EXPECT_EQ(start.error().exit_code, kRunExitDirtyTree);
  EXPECT_EQ(start.error().message, "reportable run refused: tree is unknown");
}

// The keys in the order every committed file has them, the run's shape as given.
TEST(RunHeader, WritesTheKeysInTheCommittedOrder) {
  const RunOptions options{
      .pin_cpu = std::nullopt, .meant_idle_cpus = {5}, .reportable = false, .source_root = "."};
  const auto start = start_run(options);
  ASSERT_TRUE(start.has_value()) << start.error().message;
  const RunShape shape{
      .claim = "none", .repetitions = 4, .batches_per_repetition = 9, .warmup = "w"};
  const auto fp = platform::check_fp_state();
  ASSERT_TRUE(fp.has_value());
  std::vector<HeaderField> header =
      run_header_fields(*start, shape, *fp, platform::read_affinity());
  EXPECT_EQ(keys_of(header),
            (std::vector<std::string_view>(kCommittedOrder.begin(), kCommittedOrder.end())));
  ResultsFile file{.header = header, .sections = {}};
  EXPECT_EQ(file.value("claim"), "none");
  EXPECT_EQ(file.value("repetitions"), "4");
  EXPECT_EQ(file.value("batches_per_repetition"), "9");
  EXPECT_EQ(file.value("warmup"), "w");
  EXPECT_EQ(file.value("meant_idle_cpus"), "5" + std::string(kNotEnforced));
  EXPECT_EQ(file.value("run_started_at"), start->started_at);

  const std::size_t early = header.size();
  add_run_late_fields(header, platform::read_frequency_info(0),
                      {.source = "test",
                       .scope = "test",
                       .cycles = 1,
                       .instructions = 2,
                       .time_enabled_ns = 0,
                       .time_running_ns = 0,
                       .note = ""},
                      1234);
  const std::vector<std::string_view> all = keys_of(header);
  EXPECT_EQ(
      std::vector<std::string_view>(all.begin() + static_cast<std::ptrdiff_t>(early), all.end()),
      (std::vector<std::string_view>(kCommittedLateOrder.begin(), kCommittedLateOrder.end())));
  file.header = header;
  EXPECT_TRUE(missing_keys(file, kRunHeaderKeys).empty());
  EXPECT_TRUE(missing_keys(file, kPerfHeaderKeys).empty());
  EXPECT_EQ(file.value("peak_rss_bytes"), "1234");
}

TEST(RunHeader, NumbersAreWrittenAsTheHeaderAlwaysWroteThem) {
  EXPECT_EQ(header_number(1.0), "1.000");
  EXPECT_EQ(header_number(24000000.0, "%.1f"), "24000000.0");
  EXPECT_EQ(header_number(0.5, "%.6f"), "0.500000");
}

// --- the emitter session ---

ReportTable table(bool uses_chain = false) {
  return {.name = "t",
          .title = "A table",
          .targets = {"SVI-6"},
          .timing = false,
          .uses_chain = uses_chain,
          .columns = {{.name = "v", .kind = CellKind::kReal, .unit = "none"}},
          .rows = {{.key = "a", .cells = {real_cell(0.1)}}}};
}

TEST(ReportRun, WritesARawReportFileTheStampCanRead) {
  auto session = ReportRun::begin(RunOptions{});
  ASSERT_TRUE(session.has_value()) << session.error().message;
  const std::vector<ReportTable> tables = {table()};
  const ReportShape shape{
      .run = {.claim = "result", .repetitions = 3, .batches_per_repetition = 1, .warmup = "none"},
      .header = {{.key = "tool_extra", .value = "x"}}};
  const auto text = session->finish(shape, tables, std::nullopt);
  ASSERT_TRUE(text.has_value()) << text.error().message;
  const auto file = parse_results_file(*text);
  ASSERT_TRUE(file.has_value()) << file.error();
  EXPECT_TRUE(missing_keys(*file, kRunHeaderKeys).empty());
  EXPECT_TRUE(missing_keys(*file, kPerfHeaderKeys).empty());
  EXPECT_EQ(file->value("claim"), "result");
  EXPECT_EQ(file->value("repetitions"), "3");
  EXPECT_EQ(file->value("tool_extra"), "x");
  // The emitter's own fields sit between run_started_at and the late fields.
  const std::vector<std::string_view> keys = keys_of(file->header);
  const auto at = std::ranges::find(keys, "tool_extra");
  ASSERT_NE(at, keys.end());
  EXPECT_EQ(*(at - 1), "run_started_at");
  EXPECT_EQ(*(at + 1), "smt_siblings");
  const auto tables_back = read_report(*file);
  ASSERT_TRUE(tables_back.has_value()) << tables_back.error().detail;
  EXPECT_EQ(*tables_back, tables);
}

TEST(ReportRun, PutsTheAttributionFirstAndEndsItInANewline) {
  auto session = ReportRun::begin(RunOptions{});
  ASSERT_TRUE(session.has_value()) << session.error().message;
  std::string attribution;
  for (const std::string_view fragment : kAttributionFragments) {
    attribution += std::string(attribution.empty() ? "" : "\n") + std::string(fragment);
  }
  const std::vector<ReportTable> tables = {table(true)};
  const auto text = session->finish({}, tables, attribution);
  ASSERT_TRUE(text.has_value()) << text.error().message;
  const auto file = parse_results_file(*text);
  ASSERT_TRUE(file.has_value()) << file.error();
  ASSERT_EQ(file->sections.size(), 2U);
  EXPECT_EQ(file->sections.front().name, "attribution");
  EXPECT_EQ(file->sections.front().body, attribution + "\n");
}

TEST(ReportRun, RefusesATableFromTheChainWithoutTheAttribution) {
  auto session = ReportRun::begin(RunOptions{});
  ASSERT_TRUE(session.has_value()) << session.error().message;
  const std::vector<ReportTable> tables = {table(true)};
  const auto text = session->finish({}, tables, std::nullopt);
  ASSERT_FALSE(text.has_value());
  EXPECT_EQ(text.error().exit_code, kRunExitFailure);
  EXPECT_TRUE(text.error().message.contains("no attribution")) << text.error().message;
}

TEST(ReportRun, RefusesANonFiniteCellARepeatedTableAndAKeyItDoesNotOwn) {
  {
    auto session = ReportRun::begin(RunOptions{});
    ASSERT_TRUE(session.has_value());
    ReportTable bad = table();
    bad.rows[0].cells[0] = real_cell(std::numeric_limits<double>::quiet_NaN());
    const std::vector<ReportTable> tables = {bad};
    EXPECT_FALSE(session->finish({}, tables, std::nullopt).has_value());
  }
  {
    auto session = ReportRun::begin(RunOptions{});
    ASSERT_TRUE(session.has_value());
    const std::vector<ReportTable> tables = {table(), table()};
    EXPECT_FALSE(session->finish({}, tables, std::nullopt).has_value());
  }
  {
    auto session = ReportRun::begin(RunOptions{});
    ASSERT_TRUE(session.has_value());
    const std::vector<ReportTable> tables = {table()};
    const ReportShape shape{.run = {}, .header = {{.key = "commit", .value = "x"}}};
    EXPECT_FALSE(session->finish(shape, tables, std::nullopt).has_value());
  }
  {
    auto session = ReportRun::begin(RunOptions{});
    ASSERT_TRUE(session.has_value());
    EXPECT_FALSE(session->finish({}, {}, std::nullopt).has_value());
  }
}

}  // namespace
}  // namespace aadp::results
