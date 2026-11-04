// Runs the real binaries, because the refusals only matter if they survive the tools around
// them: exit status, the stated reason, and no output file on refusal.
#include <gtest/gtest.h>
#include <sys/resource.h>
#include <sys/wait.h>  // IWYU pragma: keep
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <string_view>

#include "fixtures.hpp"
#include "results/results_file.hpp"

#ifdef __APPLE__
#include <sys/sysctl.h>

#include <cstddef>
#endif

namespace aadp::testing {
namespace {

namespace fs = std::filesystem;

const std::string kFastArgs = " --batches=200 --repetitions=2 --warmup-ms=1 --gb-warmup-s=0.001";

fs::path fresh_dir() {
  const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
  const fs::path dir =
      fs::path(AADP_TEST_SCRATCH) / (std::string(info->test_suite_name()) + "_" + info->name());
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

int run(const std::string& command) {
  const int status = std::system(command.c_str());
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string slurp(const fs::path& path) {
  const std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

std::string quoted(const fs::path& path) { return "'" + path.string() + "'"; }

std::string stderr_to_file(const fs::path& dir) { return " 2>" + quoted(dir / "err.txt"); }

// Every refusal of a tool shares its exit status, so a test of one refusal has to read the reason
// too, or it passes when the tool stops for something else.
void expect_reason(const fs::path& dir, std::string_view reason) {
  const std::string said = slurp(dir / "err.txt");
  EXPECT_NE(said.find(reason), std::string::npos) << "wanted: " << reason << "\nsaid: " << said;
}

std::string short_name() {
  std::array<char, 256> name{};
  gethostname(name.data(), name.size() - 1);
  const std::string_view view(name.data());
  return std::string(view.substr(0, view.find('.')));
}

std::string host_label(const fs::path& dir) {
  EXPECT_EQ(run(std::string(AADP_STAMP_BIN) + " --print-host-label > " + quoted(dir / "label")), 0);
  std::string label = slurp(dir / "label");
  while (label.ends_with('\n')) {
    label.pop_back();
  }
  return label;
}

std::string selftest(const fs::path& out, const std::string& args) {
  return std::string(AADP_SELFTEST_BIN) + " --source-root=" + quoted(out.parent_path()) + args +
         " > " + quoted(out);
}

TEST(HarnessSelftest, WritesACompleteRawFile) {
  const fs::path dir = fresh_dir();
  ASSERT_EQ(run(selftest(dir / "raw.txt", kFastArgs)), 0);
  const auto file = results::parse_results_file(slurp(dir / "raw.txt"));
  ASSERT_TRUE(file.has_value()) << file.error();
  EXPECT_TRUE(results::missing_keys(*file, results::kRunHeaderKeys).empty());
  EXPECT_TRUE(results::missing_keys(*file, results::kPerfHeaderKeys).empty());
  EXPECT_EQ(file->value("claim"), "none");
  EXPECT_NE(file->section("benchmark_json"), nullptr);
  ASSERT_NE(file->section("histogram_harness_selftest"), nullptr);
  EXPECT_EQ(file->value("repetitions"), "2");
  EXPECT_EQ(file->value("batches_per_repetition"), "200");
  // No warm-up batch got into the histogram: exactly repetitions times batches.
  EXPECT_NE(file->section("histogram_harness_selftest")->body.find("batches 400\n"),
            std::string::npos);
  EXPECT_NE(file->value("timer_resolution_ns"), "0.000");
}

// Two increments, so that a constant in the header cannot pass by matching one of them. The test
// may itself run niced, and nice adds to the caller's value up to the kernel's ceiling.
TEST(HarnessSelftest, HeaderRecordsTheNiceValue) {
#ifdef __APPLE__
  constexpr int kMaxNice = 20;
#else
  constexpr int kMaxNice = 19;
#endif
  const fs::path dir = fresh_dir();
  const int caller = getpriority(PRIO_PROCESS, 0);
  for (const int increment : {3, 7}) {
    const std::string text = std::to_string(increment);
    const fs::path raw = dir / ("raw-" + text + ".txt");
    ASSERT_EQ(run("nice -n " + text + " " + selftest(raw, kFastArgs)), 0);
    const auto file = results::parse_results_file(slurp(raw));
    ASSERT_TRUE(file.has_value()) << file.error();
    EXPECT_EQ(file->value("process_nice"), std::to_string(std::min(caller + increment, kMaxNice)))
        << "caller at nice " << caller << ", increment " << increment;
  }
}

// 400 batches take milliseconds, so a count in the billions would be Google Benchmark's own
// start-up, which spins for a second on macOS.
TEST(HarnessSelftest, CountersCoverOnlyTheMeasuredRepetitions) {
  const fs::path dir = fresh_dir();
  ASSERT_EQ(run(selftest(dir / "raw.txt", kFastArgs)), 0);
  const auto file = results::parse_results_file(slurp(dir / "raw.txt"));
  ASSERT_TRUE(file.has_value()) << file.error();
  const auto cycles = file->value("perf_cycles");
  if (!cycles) FAIL() << "no perf_cycles field";
  if (*cycles != "missing") {
    EXPECT_LT(std::stod(std::string(*cycles)), 1e9);
  }
}

// Recorded on every host, enforced on none: the stamp is what requires it on linux-zen1.
TEST(HarnessSelftest, TheHeaderRecordsTheCpusMeantToBeIdle) {
  const fs::path dir = fresh_dir();
  ASSERT_EQ(run(selftest(dir / "none.txt", kFastArgs)), 0);
  const auto none = results::parse_results_file(slurp(dir / "none.txt"));
  ASSERT_TRUE(none.has_value()) << none.error();
  EXPECT_EQ(none->value(results::kMeantIdleCpusKey), "none");
  ASSERT_EQ(run(selftest(dir / "idle.txt", kFastArgs + " --meant-idle-cpus=5")), 0);
  const auto idle = results::parse_results_file(slurp(dir / "idle.txt"));
  ASSERT_TRUE(idle.has_value()) << idle.error();
  EXPECT_EQ(idle->value(results::kMeantIdleCpusKey), "5 (not enforced)");
}

#ifdef __APPLE__
TEST(HarnessSelftest, MacOsHeaderSaysNotPinned) {
  const fs::path dir = fresh_dir();
  ASSERT_EQ(run(selftest(dir / "raw.txt", kFastArgs)), 0);
  const auto file = results::parse_results_file(slurp(dir / "raw.txt"));
  ASSERT_TRUE(file.has_value()) << file.error();
  EXPECT_EQ(file->value("pinning_state"), "not pinned");
  EXPECT_EQ(file->value("pinned_cpus"), "none");
}

// The header end of PerfCounters.MacOsCountersAreProcessWideAndLabelled: real hardware writes
// counts, and a virtual machine whose counters stay still writes them as unavailable, never 0.
TEST(HarnessSelftest, MacOsHeaderNeverWritesAStuckCounterAsACount) {
  int present = 0;
  std::size_t size = sizeof(present);
  const bool virtual_machine =
      sysctlbyname("kern.hv_vmm_present", &present, &size, nullptr, 0) == 0 && present != 0;
  const fs::path dir = fresh_dir();
  ASSERT_EQ(run(selftest(dir / "raw.txt", kFastArgs)), 0);
  const auto file = results::parse_results_file(slurp(dir / "raw.txt"));
  ASSERT_TRUE(file.has_value()) << file.error();
  const std::string note(file->required("perf_counters_note"));
  for (const char* key : {"perf_cycles", "perf_instructions"}) {
    const auto value = file->value(key);
    if (!value) FAIL() << "no " << key << " field";
    EXPECT_NE(*value, "0") << key;
    if (*value == "missing") {
      EXPECT_TRUE(virtual_machine) << key << " missing on real hardware: " << note;
      EXPECT_NE(note.find("unavailable"), std::string::npos) << key << ": " << note;
    }
  }
}

TEST(HarnessSelftest, RefusesToPretendToPinOnMacOs) {
  const fs::path dir = fresh_dir();
  EXPECT_EQ(run(selftest(dir / "raw.txt", kFastArgs + " --pin-cpu=0" + stderr_to_file(dir))), 6);
  expect_reason(dir, "macOS has no way to pin a thread to CPU 0");
}
#endif

#ifdef __linux__
TEST(HarnessSelftest, PinCpuPinsAndTheHeaderReadsItBack) {
  const fs::path dir = fresh_dir();
  ASSERT_EQ(run(selftest(dir / "raw.txt", kFastArgs + " --pin-cpu=0")), 0);
  const auto file = results::parse_results_file(slurp(dir / "raw.txt"));
  ASSERT_TRUE(file.has_value()) << file.error();
  EXPECT_EQ(file->value("pinning_state"), "pinned");
  EXPECT_EQ(file->value("pinned_cpus"), "0");
  EXPECT_EQ(file->value("observed_cpu"), "0");
}

// What a threaded row on linux-zen1 does (taskset -c 4,6), on two CPUs any Linux host has.
TEST(HarnessSelftest, ATwoCpuMaskIsAPinAndTheHeaderNamesBoth) {
  if (sysconf(_SC_NPROCESSORS_ONLN) < 3) {
    FAIL() << "needs three online CPUs, so that two are a pin";
  }
  const fs::path dir = fresh_dir();
  ASSERT_EQ(run("taskset -c 0,2 " + selftest(dir / "raw.txt", kFastArgs + " --meant-idle-cpus=1")),
            0);
  const auto file = results::parse_results_file(slurp(dir / "raw.txt"));
  ASSERT_TRUE(file.has_value()) << file.error();
  EXPECT_EQ(file->value("pinning_state"), "pinned");
  EXPECT_EQ(file->value("pinned_cpus"), "0,2");
  EXPECT_EQ(file->value("pin_method"), "inherited mask, read back");
  EXPECT_EQ(file->value(results::kMeantIdleCpusKey), "1 (not enforced)");
}
#endif

// One unit is 16 multiply-adds, far below the gate on both hosts, and also under sanitizers.
TEST(HarnessSelftest, RefusesAHistogramOfBatchesTooShortToResolve) {
  const fs::path dir = fresh_dir();
  EXPECT_EQ(
      run(selftest(dir / "raw.txt", kFastArgs + " --batch-units=1 2>" + quoted(dir / "err.txt"))),
      5);
  EXPECT_NE(slurp(dir / "err.txt").find("refusing to produce a histogram"), std::string::npos);
  EXPECT_EQ(slurp(dir / "raw.txt").find("@section"), std::string::npos);
}

TEST(HarnessSelftest, ReportableRefusesAnUnknownTree) {
  const fs::path dir = fresh_dir();
  EXPECT_EQ(run(selftest(dir / "raw.txt", kFastArgs + " --reportable" + stderr_to_file(dir))), 4);
  expect_reason(dir, "reportable run refused: tree is unknown");
}

TEST(HarnessSelftest, ReportableRefusesADirtyTree) {
  const fs::path dir = fresh_dir();
  const std::string git = "git -C " + quoted(dir) + " ";
  ASSERT_EQ(run(git + "init -q"), 0);
  ASSERT_EQ(run(git + "-c user.name=t -c user.email=t@example.invalid commit -q --allow-empty "
                      "-m first"),
            0);
  std::ofstream(dir / "dirty.txt") << "x\n";
  EXPECT_EQ(run(selftest(dir / "raw.txt", kFastArgs + " --reportable" + stderr_to_file(dir))), 4);
  expect_reason(dir, "reportable run refused: tree is dirty");
}

// The harness passes what it does not know on to Google Benchmark, so this is that library's text.
TEST(HarnessSelftest, RejectsAnUnknownFlag) {
  const fs::path dir = fresh_dir();
  EXPECT_EQ(run(selftest(dir / "raw.txt", " --no-such-flag" + stderr_to_file(dir))), 2);
  expect_reason(dir, "unrecognized command-line flag: --no-such-flag");
}

std::string stamp_command(const fs::path& dir, const std::string& mode) {
  return std::string(AADP_STAMP_BIN) + " --raw=" + quoted(dir / "raw.txt") +
         " --stderr=" + quoted(dir / "stderr.txt") + " --out=" + quoted(dir / "out.txt") + " " +
         mode + " 2>" + quoted(dir / "err.txt");
}

TEST(Stamp, ToolWritesNothingOnRefusal) {
  const fs::path dir = fresh_dir();
  std::ofstream(dir / "raw.txt") << text_of(raw_file());
  std::ofstream(dir / "stderr.txt") << "nothing useful\n";
  EXPECT_EQ(run(stamp_command(dir, "--reportable")), 1);
  EXPECT_FALSE(fs::exists(dir / "out.txt"));
  EXPECT_FALSE(fs::exists(dir / "out.txt.tmp"));
  expect_reason(dir, "refused: no measure.sh start line");
}

TEST(Stamp, ToolRefusesAFileFromAnotherHost) {
  const fs::path dir = fresh_dir();
  results::ResultsFile raw = raw_file();
  set_field(raw, "host_label", host_label(dir) == "mac-m4max" ? "linux-zen1" : "mac-m4max");
  std::ofstream(dir / "raw.txt") << text_of(raw);
  std::ofstream(dir / "stderr.txt")
      << "measure.sh: start load1=0.1 host=" << short_name() << " at=2026-01-02T03:04:05Z\n"
      << end_line() << "\n";
  EXPECT_EQ(run(stamp_command(dir, "--reportable")), 1);
  EXPECT_NE(slurp(dir / "err.txt").find("stamping on"), std::string::npos);
  EXPECT_FALSE(fs::exists(dir / "out.txt"));
}

TEST(Stamp, ToolScansARealRunAndLeavesNoPrivateText) {
  const fs::path dir = fresh_dir();
  ASSERT_EQ(run(selftest(dir / "raw.txt", kFastArgs)), 0);
  std::ofstream(dir / "stderr.txt") << "";
  ASSERT_EQ(run(stamp_command(dir, "--not-reportable='iteration run'")), 0)
      << slurp(dir / "err.txt");
  const std::string out = slurp(dir / "out.txt");
  EXPECT_EQ(out.find("\"host_name\""), std::string::npos);
  EXPECT_EQ(out.find("\"executable\""), std::string::npos);
  EXPECT_EQ(out.find(short_name()), std::string::npos);
  const auto file = results::parse_results_file(out);
  ASSERT_TRUE(file.has_value()) << file.error();
  EXPECT_TRUE(results::missing_stamped_keys(*file).empty());
  EXPECT_EQ(file->value("reportable"), "no");
}

TEST(Stamp, ToolRefusesAForbiddenString) {
  const fs::path dir = fresh_dir();
  ASSERT_EQ(run(selftest(dir / "raw.txt", kFastArgs)), 0);
  std::ofstream(dir / "stderr.txt") << "";
  // The stamp accepts this input as it stands, so the refusal below can only come from the flag.
  ASSERT_EQ(run(stamp_command(dir, "--not-reportable=x")), 0) << slurp(dir / "err.txt");
  fs::remove(dir / "out.txt");
  EXPECT_EQ(run(stamp_command(dir, "--not-reportable=x --forbid=harness_selftest")), 1);
  expect_reason(dir, "refused: refusing to write: line ");
  expect_reason(dir, " has a forbidden string;");
  EXPECT_FALSE(fs::exists(dir / "out.txt"));
}

results::ResultsFile stamped_file(std::string_view label) {
  StampCase test_case;
  results::ResultsFile raw = raw_file();
  set_field(raw, "host_label", label);
  test_case.raw = text_of(raw);
  test_case.request.current_host_label = std::string(label);
  return *results::parse_results_file(*test_case.run());
}

TEST(Table, ToolPrintsToStdoutOneTablePerHost) {
  const fs::path dir = fresh_dir();
  fs::create_directories(dir / "in");
  std::ofstream(dir / "in" / "a.txt") << text_of(stamped_file("linux-zen1"));
  std::ofstream(dir / "in" / "b.txt") << text_of(stamped_file("mac-m4max"));
  ASSERT_EQ(run(std::string(AADP_TABLE_BIN) + " " + quoted(dir / "in") + " > " +
                quoted(dir / "table.md")),
            0);
  const std::string table = slurp(dir / "table.md");
  EXPECT_NE(table.find("### linux-zen1"), std::string::npos);
  EXPECT_NE(table.find("### mac-m4max"), std::string::npos);
}

TEST(Table, ToolRefusesTwoHostsInOneTable) {
  const fs::path dir = fresh_dir();
  fs::create_directories(dir / "in");
  std::ofstream(dir / "in" / "a.txt") << text_of(stamped_file("linux-zen1"));
  std::ofstream(dir / "in" / "b.txt") << text_of(stamped_file("mac-m4max"));
  EXPECT_EQ(run(std::string(AADP_TABLE_BIN) + " --one-table " + quoted(dir / "in") + " > " +
                quoted(dir / "table.md") + stderr_to_file(dir)),
            1);
  expect_reason(dir, "refusing to put hosts linux-zen1 and mac-m4max in one table");
  EXPECT_EQ(slurp(dir / "table.md"), "");
}

TEST(Table, ToolRefusesAFileUnderTheWrongHostDirectory) {
  const fs::path dir = fresh_dir();
  fs::create_directories(dir / "in" / "mac-m4max");
  std::ofstream(dir / "in" / "mac-m4max" / "a.txt") << text_of(stamped_file("linux-zen1"));
  EXPECT_EQ(run(std::string(AADP_TABLE_BIN) + " " + quoted(dir / "in") + " >/dev/null" +
                stderr_to_file(dir)),
            1);
  expect_reason(dir, "a.txt is under mac-m4max but its header says otherwise");
}

TEST(Table, ToolLeavesOutAFaultReportFoundInADirectoryAndSaysSo) {
  const fs::path dir = fresh_dir();
  fs::create_directories(dir / "in" / "mac-m4max");
  std::ofstream(dir / "in" / "mac-m4max" / "a.txt") << text_of(stamped_file("mac-m4max"));
  std::ofstream(dir / "in" / "mac-m4max" / "fault-injection-1.txt")
      << "aadp-fault-injection 1\nclaim: none\nsummary: caught=1\n";
  ASSERT_EQ(run(std::string(AADP_TABLE_BIN) + " " + quoted(dir / "in") + " > " +
                quoted(dir / "table.md") + stderr_to_file(dir)),
            0);
  const std::string table = slurp(dir / "table.md");
  EXPECT_NE(table.find("### mac-m4max"), std::string::npos);
  EXPECT_EQ(table.find("fault"), std::string::npos);
  expect_reason(dir, "left out fault-injection-1.txt: a fault-injection report");
}

TEST(Table, ToolRefusesAFaultReportNamedDirectly) {
  const fs::path dir = fresh_dir();
  std::ofstream(dir / "fault-injection-1.txt") << "aadp-fault-injection 1\nclaim: none\n";
  EXPECT_EQ(run(std::string(AADP_TABLE_BIN) + " " + quoted(dir / "fault-injection-1.txt") +
                " >/dev/null" + stderr_to_file(dir)),
            1);
  expect_reason(dir, "first line is not the results magic");
}

TEST(Table, ToolRefusesAFaultReportOutsideAHostDirectory) {
  const fs::path dir = fresh_dir();
  fs::create_directories(dir / "in" / "scratch");
  std::ofstream(dir / "in" / "scratch" / "fault-injection-1.txt")
      << "aadp-fault-injection 1\nclaim: none\n";
  EXPECT_EQ(run(std::string(AADP_TABLE_BIN) + " " + quoted(dir / "in") + " >/dev/null" +
                stderr_to_file(dir)),
            1);
  expect_reason(dir, "first line is not the results magic");
}

TEST(Table, ToolStillRefusesAnUnknownFileWhateverItIsCalled) {
  const fs::path dir = fresh_dir();
  fs::create_directories(dir / "in");
  std::ofstream(dir / "in" / "fault-injection-1.txt") << "not a report\n";
  EXPECT_EQ(run(std::string(AADP_TABLE_BIN) + " " + quoted(dir / "in") + " >/dev/null" +
                stderr_to_file(dir)),
            1);
  expect_reason(dir, "first line is not the results magic");
}

}  // namespace
}  // namespace aadp::testing
