#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "platform/affinity.hpp"
#include "platform/cacheline.hpp"
#include "platform/cpu_info.hpp"
#include "platform/fp_state.hpp"
#include "platform/host_label.hpp"
#include "platform/memory_info.hpp"
#include "platform/perf_counters.hpp"
#include "platform/simd_info.hpp"

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#ifdef __linux__
#include <sched.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#endif

namespace aadp::platform {
namespace {

TEST(HostLabel, RecognisesTheTwoHosts) {
  EXPECT_EQ(classify_host("macOS", "Apple M4 Max"), kHostMacM4Max);
  EXPECT_EQ(classify_host("Linux", "AMD Ryzen Embedded V1605B with Radeon Vega Gfx"),
            kHostLinuxZen1);
  EXPECT_EQ(classify_host("Linux", "AMD Ryzen Embedded V1605B"), kHostLinuxZen1);
}

TEST(HostLabel, EverythingElseIsOther) {
  EXPECT_EQ(classify_host("macOS", "Apple M4 Pro"), kHostOther);
  EXPECT_EQ(classify_host("macOS", "Apple M4"), kHostOther);
  EXPECT_EQ(classify_host("macOS", "AMD Ryzen Embedded V1605B"), kHostOther);
  EXPECT_EQ(classify_host("Linux", "Apple M4 Max"), kHostOther);
  EXPECT_EQ(classify_host("Linux", "AMD Ryzen Embedded V1605BX"), kHostOther);
  EXPECT_EQ(classify_host("Linux", "unknown"), kHostOther);
  EXPECT_EQ(classify_host("", ""), kHostOther);
}

TEST(CpuInfoParse, ReadsTheX86ModelName) {
  EXPECT_EQ(parse_cpuinfo_model("processor\t: 0\nvendor_id\t: AuthenticAMD\n"
                                "model name\t: AMD Ryzen Embedded V1605B with Radeon Vega Gfx\n"
                                "stepping\t: 1\n\nprocessor\t: 1\nmodel name\t: other\n"),
            "AMD Ryzen Embedded V1605B with Radeon Vega Gfx");
}

TEST(CpuInfoParse, FallsBackToImplementerAndPartOnArm64) {
  EXPECT_EQ(parse_cpuinfo_model("processor\t: 0\nCPU implementer\t: 0x41\nCPU part\t: 0xd0b\n"),
            "arm64 implementer 0x41 part 0xd0b");
}

TEST(CpuInfoParse, ReturnsEmptyWhenThereIsNothing) {
  EXPECT_EQ(parse_cpuinfo_model(""), "");
  EXPECT_EQ(parse_cpuinfo_model("processor\t: 0\nmodel name\t:\n"), "");
}

TEST(Midr, FormatsAndRefuses) {
  EXPECT_EQ(describe_midr("0x00000000410fd0b1\n"), "arm64 midr_el1 0x00000000410fd0b1");
  EXPECT_FALSE(describe_midr("").has_value());
  EXPECT_FALSE(describe_midr("garbage").has_value());
  EXPECT_FALSE(describe_midr("0x").has_value());
}

TEST(OsDescription, ReadsOsRelease) {
  const std::string text = "NAME=\"X\"\nPRETTY_NAME=\"Some Linux 13\"\nVERSION_ID=13\n";
  EXPECT_EQ(parse_os_release_field(text, "PRETTY_NAME"), "Some Linux 13");
  EXPECT_EQ(parse_os_release_field(text, "VERSION_ID"), "13");
  EXPECT_EQ(parse_os_release_field(text, "MISSING"), "");
}

TEST(OsDescription, WithholdsADistributionNamedLikeTheHost) {
  EXPECT_EQ(describe_linux_os("Some Linux 13", "13", "box"), "Some Linux 13");
  const std::string withheld = describe_linux_os("Tuxbox GNU/Linux 13 (x)", "13", "tuxbox");
  EXPECT_EQ(withheld.find("Tuxbox"), std::string::npos);
  EXPECT_EQ(withheld.find("tuxbox"), std::string::npos);
  EXPECT_NE(withheld.find("13"), std::string::npos);
  EXPECT_EQ(describe_linux_os("", "", "box"), "Linux");
}

TEST(CpuInfo, ReadsThisMachine) {
  const CpuInfo info = read_cpu_info();
  EXPECT_FALSE(info.os.empty());
  EXPECT_FALSE(info.kernel.empty());
  EXPECT_FALSE(info.arch.empty());
  EXPECT_NE(info.cpu_model, "unknown");
  EXPECT_GT(info.logical_cpus, 0);
}

// Zen and Zen+ (family 17h below model 30h) split 256-bit operations; Zen 2 on and Intel do not.
TEST(FpDatapath, NamesZen1AndNothingElse) {
  const std::string zen1 = "Zen 1: 128-bit FP datapath";
  EXPECT_EQ(fp_datapath_note(X86Model{.family = 0x17, .model = 0x11}), zen1);  // V1605B
  EXPECT_EQ(fp_datapath_note(X86Model{.family = 0x17, .model = 0x01}), zen1);
  EXPECT_EQ(fp_datapath_note(X86Model{.family = 0x17, .model = 0x08}), zen1);  // Zen+
  EXPECT_EQ(fp_datapath_note(X86Model{.family = 0x17, .model = 0x2F}), zen1);
  EXPECT_EQ(fp_datapath_note(X86Model{.family = 0x17, .model = 0x30}), "");  // Zen 2
  EXPECT_EQ(fp_datapath_note(X86Model{.family = 0x17, .model = 0x71}), "");
  EXPECT_EQ(fp_datapath_note(X86Model{.family = 0x19, .model = 0x21}), "");  // Zen 3
  EXPECT_EQ(fp_datapath_note(X86Model{.family = 0x06, .model = 0x9E}), "");  // Intel
  EXPECT_EQ(fp_datapath_note(std::nullopt), "");
}

TEST(FpDatapath, ReadsTheFamilyOnX86AndNothingElsewhere) {
  const auto cpu = read_x86_model();
#if defined(__x86_64__) || defined(__i386__)
  ASSERT_TRUE(cpu.has_value());
  const X86Model model = cpu.value_or(X86Model{});
  EXPECT_GE(model.family, 6U);
  // The Linux host is the Zen 1 this note exists for.
  if (read_cpu_info().cpu_model.contains("V1605B")) {
    EXPECT_EQ(model.family, 0x17U);
    EXPECT_EQ(model.model, 0x11U);
  }
#else
  EXPECT_FALSE(cpu.has_value());
#endif
}

TEST(CpuInfo, KernelFieldNeverHoldsTheNodeName) {
  const std::string hostname = short_hostname();
  ASSERT_FALSE(hostname.empty());
  EXPECT_EQ(read_cpu_info().kernel.find(hostname), std::string::npos);
}

TEST(Cacheline, MatchesWhatTheMachineReports) {
#ifdef __APPLE__
  std::int64_t line = 0;
  std::size_t size = sizeof(line);
  ASSERT_EQ(sysctlbyname("hw.cachelinesize", &line, &size, nullptr, 0), 0);
  EXPECT_EQ(static_cast<std::size_t>(line), kCacheLineBytes);
#else
  std::ifstream in("/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size");
  std::size_t line = 0;
  if (in >> line) {
    EXPECT_EQ(line, kCacheLineBytes);
  }
#endif
}

TEST(FpState, DecodesFpcr) {
  EXPECT_FALSE(decode_fpcr(0).ftz);
  EXPECT_FALSE(decode_fpcr(0).daz);
  EXPECT_TRUE(decode_fpcr(1U << 24).ftz);
  EXPECT_TRUE(decode_fpcr(1U << 24).daz);
  EXPECT_FALSE(decode_fpcr(1U).ftz);
  EXPECT_TRUE(decode_fpcr(1U).daz);
  // Rounding mode and default NaN bits are not flush bits.
  EXPECT_FALSE(decode_fpcr((3U << 22) | (1U << 25)).ftz);
  EXPECT_EQ(decode_fpcr(0).register_name, "FPCR");
}

TEST(FpState, DecodesMxcsr) {
  EXPECT_FALSE(decode_mxcsr(0x1f80).ftz);
  EXPECT_FALSE(decode_mxcsr(0x1f80).daz);
  EXPECT_TRUE(decode_mxcsr(0x1f80 | (1U << 15)).ftz);
  EXPECT_FALSE(decode_mxcsr(0x1f80 | (1U << 15)).daz);
  EXPECT_TRUE(decode_mxcsr(0x1f80 | (1U << 6)).daz);
  EXPECT_EQ(decode_mxcsr(0).register_name, "MXCSR");
}

TEST(FpState, DefaultStateIsClean) {
  const auto state = check_fp_state();
  ASSERT_TRUE(state.has_value()) << state.error();
#ifdef __x86_64__
  EXPECT_EQ(state->register_name, "MXCSR");
#else
  EXPECT_EQ(state->register_name, "FPCR");
#endif
}

// Sets the real register, so this shows both the read and the failure.
TEST(FpState, FailsWithFlushToZeroSet) {
  const std::uint64_t saved = read_fp_state().raw;
#ifdef __x86_64__
  const std::uint64_t flush_bit = std::uint64_t{1} << 15;
#else
  const std::uint64_t flush_bit = std::uint64_t{1} << 24;
#endif
  write_fp_register(saved | flush_bit);
  const FpState seen = read_fp_state();
  const auto checked = check_fp_state();
  const volatile double smallest_normal = std::numeric_limits<double>::min();
  const double flushed = smallest_normal * 0.5;
  write_fp_register(saved);

  EXPECT_TRUE(seen.ftz);
  ASSERT_FALSE(checked.has_value());
  EXPECT_NE(checked.error().find("FTZ"), std::string::npos);
  EXPECT_EQ(flushed, 0.0) << "the register was set but did not flush";
  EXPECT_TRUE(check_fp_state().has_value());
}

TEST(Simd, ReportsATargetAndLanes) {
  EXPECT_FALSE(simd_target_name().empty());
  EXPECT_GT(simd_double_lanes(), 0U);
}

TEST(MemoryInfo, PeakRssIsAtLeastWhatThisTestTouches) {
  constexpr std::size_t kBytes = std::size_t{1} << 24;
  std::vector<char> block(kBytes);
  volatile char* touch = block.data();
  for (std::size_t i = 0; i < kBytes; i += 4096) {
    touch[i] = 1;
  }
  const std::uint64_t peak = peak_rss_bytes();
  EXPECT_GE(peak, kBytes) << "unit error, or the block was not touched";
  EXPECT_LT(peak, std::uint64_t{1} << 40) << "units are wrong";
}

TEST(PerfCounters, ASumHasACounterOnlyIfEveryRegionHadIt) {
  PerfCounterReport total;
  PerfCounterReport region;
  region.source = "s";
  region.cycles = 10;
  region.instructions = 20;
  region.time_enabled_ns = 5;
  region.time_running_ns = 5;
  accumulate(total, region);
  accumulate(total, region);
  EXPECT_EQ(total.cycles, 20U);
  EXPECT_EQ(total.instructions, 40U);
  EXPECT_NE(describe_counters(total).find("not multiplexed"), std::string::npos);

  PerfCounterReport missing;
  missing.source = "s";
  missing.note = "denied";
  accumulate(total, missing);
  EXPECT_FALSE(total.cycles.has_value());
  EXPECT_FALSE(total.instructions.has_value());
  EXPECT_NE(describe_counters(total).find("denied"), std::string::npos);
}

// What a macOS virtual machine does: both counters stay at zero.
TEST(PerfCounters, ADarwinCounterThatDidNotMoveIsUnavailableNotZero) {
  PerfCounterReport stuck;
  set_darwin_region(stuck, {}, {}, true);
  EXPECT_FALSE(stuck.cycles.has_value());
  EXPECT_FALSE(stuck.instructions.has_value());
  const std::string note = describe_counters(stuck);
  EXPECT_NE(note.find("ri_cycles unavailable: it stayed at 0"), std::string::npos) << note;
  EXPECT_NE(note.find("ri_instructions unavailable: it stayed at 0"), std::string::npos) << note;
  EXPECT_NE(note.find("this is a virtual machine"), std::string::npos) << note;

  PerfCounterReport one;
  set_darwin_region(one, {.cycles = 5, .instructions = 7}, {.cycles = 5, .instructions = 1007},
                    false);
  EXPECT_FALSE(one.cycles.has_value());
  EXPECT_EQ(one.instructions, 1000U);
  EXPECT_NE(one.note.find("ri_cycles unavailable: it stayed at 5"), std::string::npos);
  EXPECT_EQ(one.note.find("ri_instructions"), std::string::npos);
  EXPECT_NE(one.note.find("not a virtual machine"), std::string::npos);

  PerfCounterReport moved;
  set_darwin_region(moved, {.cycles = 5, .instructions = 7}, {.cycles = 105, .instructions = 1007},
                    true);
  EXPECT_EQ(moved.cycles, 100U);
  EXPECT_EQ(moved.instructions, 1000U);
  EXPECT_EQ(describe_counters(moved), "none");
}

TEST(PerfCounters, SaysWhenTheGroupWasMultiplexedAndDoesNotScale) {
  PerfCounterReport report;
  report.cycles = 100;
  report.time_enabled_ns = 10;
  report.time_running_ns = 5;
  EXPECT_NE(describe_counters(report).find("MULTIPLEXED"), std::string::npos);
  EXPECT_EQ(report.cycles, 100U);
}

TEST(Affinity, FormatsCpuLists) {
  EXPECT_EQ(format_cpu_list(std::vector<int>{}), "none");
  EXPECT_EQ(format_cpu_list(std::vector<int>{4}), "4");
  EXPECT_EQ(format_cpu_list(std::vector<int>{0, 1, 2, 3}), "0-3");
  EXPECT_EQ(format_cpu_list(std::vector<int>{0, 2, 3, 7}), "0,2-3,7");
}

TEST(Affinity, OneOrTwoCpusOutOfMoreArePinned) {
  EXPECT_TRUE(counts_as_pinned(std::vector<int>{4}, 8));
  EXPECT_TRUE(counts_as_pinned(std::vector<int>{4, 6}, 8));
  EXPECT_FALSE(counts_as_pinned(std::vector<int>{4, 5, 6}, 8));
  EXPECT_FALSE(counts_as_pinned(std::vector<int>{}, 8));
  EXPECT_FALSE(counts_as_pinned(std::vector<int>{0, 1}, 2));
  EXPECT_FALSE(counts_as_pinned(std::vector<int>{0}, 1));
}

#ifdef __APPLE__
TEST(Affinity, MacOsNeverClaimsAPin) {
  const AffinityState state = read_affinity();
  EXPECT_FALSE(state.pinned);
  EXPECT_EQ(pinning_state_text(state), "not pinned");
  EXPECT_TRUE(state.cpus.empty());
  EXPECT_NE(state.qos_class.find("not a pin"), std::string::npos);
  EXPECT_FALSE(pin_to_cpu(0).has_value());
}

// Read here rather than through the code under test, so that code cannot excuse itself.
bool in_virtual_machine() {
  int present = 0;
  std::size_t size = sizeof(present);
  return sysctlbyname("kern.hv_vmm_present", &present, &size, nullptr, 0) == 0 && present != 0;
}

// On real hardware the counters must count. A virtual machine may leave them still, as GitHub's
// macOS runners do; then each that did not move must be unavailable and say why.
TEST(PerfCounters, MacOsCountersAreProcessWideAndLabelled) {
  PerfCounterSession session;
  session.start();
  volatile double sum = 0;
  for (int i = 0; i < 1'000'000; ++i) {
    sum = sum + 1.0;
  }
  const PerfCounterReport report = session.stop();
  EXPECT_EQ(report.source, "proc_pid_rusage");
  EXPECT_NE(report.scope.find("process-wide"), std::string::npos);
  if (!in_virtual_machine()) {
    ASSERT_TRUE(report.cycles.has_value()) << report.note;
    if (!report.instructions) FAIL() << report.note;
    EXPECT_GT(*report.instructions, 1'000'000U);
    return;
  }
  std::printf("kern.hv_vmm_present is 1; counters: %s\n", describe_counters(report).c_str());
  if (!report.cycles) {
    EXPECT_NE(report.note.find("ri_cycles unavailable"), std::string::npos) << report.note;
  }
  if (report.instructions) {
    EXPECT_GT(*report.instructions, 1'000'000U);
  } else {
    EXPECT_NE(report.note.find("ri_instructions unavailable"), std::string::npos) << report.note;
  }
  if (!report.cycles || !report.instructions) {
    EXPECT_NE(report.note.find("this is a virtual machine"), std::string::npos) << report.note;
  }
}
#endif

#ifdef __linux__
// Found on debian: the times kept growing across regions, while the counts did not.
TEST(PerfCounters, EachRegionReportsItsOwnTimes) {
  PerfCounterSession session;
  const auto spin = [](std::uint64_t ns) {
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::nanoseconds(ns)) {
    }
  };
  session.start();
  spin(30'000'000);
  const PerfCounterReport first = session.stop();
  if (!first.cycles) {
    GTEST_SKIP() << "counters unavailable: " << first.note;
  }
  session.start();
  spin(1'000'000);
  const PerfCounterReport second = session.stop();
  if (!second.cycles) {
    FAIL() << second.note;
  }
  // Perf times come from the scheduler clock and the spin from CLOCK_MONOTONIC, which differ by
  // about 0.14% on debian. The bound allows 1%, the limit timer calibration works to.
  EXPECT_GE(first.time_enabled_ns, 29'700'000U);
  EXPECT_LT(second.time_enabled_ns, first.time_enabled_ns / 2);
  EXPECT_LT(*second.cycles, *first.cycles / 2);
}

TEST(Affinity, ReadsTheRealMask) {
  const AffinityState state = read_affinity();
  EXPECT_FALSE(state.cpus.empty());
  ASSERT_TRUE(state.current.has_value());
  EXPECT_EQ(state.pinned, counts_as_pinned(state.cpus, sysconf(_SC_NPROCESSORS_ONLN)));
}

TEST(Affinity, PinsAndReadsBackThenRestores) {
  const AffinityState before = read_affinity();
  const int cpu = before.cpus.back();
  const auto pinned = pin_to_cpu(cpu);
  ASSERT_TRUE(pinned.has_value()) << pinned.error();
  EXPECT_TRUE(pinned->pinned);
  EXPECT_EQ(pinned->cpus, std::vector<int>{cpu});
  EXPECT_EQ(pinned->current, cpu);

  cpu_set_t mask;
  CPU_ZERO(&mask);
  for (const int allowed : before.cpus) {
    CPU_SET(static_cast<std::size_t>(allowed), &mask);
  }
  ASSERT_EQ(sched_setaffinity(0, sizeof(mask), &mask), 0);
}

TEST(Affinity, RefusesACpuThatDoesNotExist) {
  EXPECT_FALSE(pin_to_cpu(100000).has_value());
  EXPECT_FALSE(pin_to_cpu(CPU_SETSIZE - 1).has_value());
}

// Passes either way on a host that forbids counters, but only with a stated reason.
TEST(PerfCounters, LinuxCountersCountOrSayWhyNot) {
  PerfCounterSession session;
  session.start();
  volatile double sum = 0;
  for (int i = 0; i < 1'000'000; ++i) {
    sum = sum + 1.0;
  }
  const PerfCounterReport report = session.stop();
  EXPECT_EQ(report.source, "perf_event_open");
  EXPECT_NE(report.scope.find("user space only"), std::string::npos);
  if (report.cycles && report.instructions) {
    EXPECT_GT(*report.instructions, 1'000'000U);
    EXPECT_GT(*report.cycles, 100'000U);
    EXPECT_GT(report.time_running_ns, 0U);
    EXPECT_GE(report.time_enabled_ns, report.time_running_ns);
  } else {
    EXPECT_FALSE(report.cycles.has_value());
    EXPECT_FALSE(report.instructions.has_value());
    EXPECT_FALSE(report.note.empty());
  }
}
#endif

}  // namespace
}  // namespace aadp::platform
