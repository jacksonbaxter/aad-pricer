#include "platform/timer.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <numeric>
#include <vector>

#ifdef __x86_64__
#include <cpuid.h>
#endif

namespace aadp::platform {
namespace {

constexpr std::size_t kResolutionReads = 100'000;
constexpr std::uint64_t kRoundNs = 10'000'000;
constexpr int kRounds = 5;
constexpr int kBracketTries = 16;
// Every nanosecond figure scales with this rate, so a wide spread makes all of them unreliable.
constexpr double kMaxSpread = 0.01;

struct ClockPoint {
  double ticks = 0;
  double ns = 0;
};

// The clock read sits between two tick reads. The tightest of several tries is kept, so an
// interrupt during one try cannot skew the calibration.
ClockPoint sample_point() {
  std::uint64_t best_width = ~std::uint64_t{0};
  ClockPoint point;
  for (int i = 0; i < kBracketTries; ++i) {
    const std::uint64_t before = read_ticks();
    const std::uint64_t ns = clock_ns();
    const std::uint64_t after = read_ticks();
    if (after >= before && after - before < best_width) {
      best_width = after - before;
      point.ticks = (static_cast<double>(before) + static_cast<double>(after)) / 2.0;
      point.ns = static_cast<double>(ns);
    }
  }
  return point;
}

double calibrate_rate(double& spread) {
  std::array<double, kRounds> rates{};
  for (double& rate : rates) {
    const ClockPoint first = sample_point();
    const std::uint64_t start = clock_ns();
    while (clock_ns() - start < kRoundNs) {
    }
    const ClockPoint last = sample_point();
    rate = (last.ticks - first.ticks) * 1e9 / (last.ns - first.ns);
  }
  std::ranges::sort(rates);
  const double median = rates[kRounds / 2];
  spread = median > 0 ? (rates.back() - rates.front()) / median : 0;
  return median;
}

template <typename Read>
ReadStats sample_reads(Read read, double& cost_ns) {
  std::vector<std::uint64_t> values(kResolutionReads);
  const std::uint64_t start = clock_ns();
  for (auto& value : values) {
    value = read();
  }
  cost_ns = static_cast<double>(clock_ns() - start) / static_cast<double>(values.size());
  return analyse_reads(values);
}

std::string describe_tick_counter() {
  std::array<char, 96> text{"none"};
#ifdef __APPLE__
  mach_timebase_info_data_t base{};
  mach_timebase_info(&base);
  std::snprintf(text.data(), text.size(), "mach_timebase %u/%u, not used", base.numer, base.denom);
#elifdef __x86_64__
  unsigned eax = 0;
  unsigned ebx = 0;
  unsigned ecx = 0;
  unsigned edx = 0;
  const bool invariant =
      __get_cpuid(0x80000007U, &eax, &ebx, &ecx, &edx) != 0 && ((edx >> 8) & 1U) != 0;
  std::snprintf(text.data(), text.size(), "invariant TSC %s", invariant ? "yes" : "no");
#endif
  return text.data();
}

}  // namespace

ReadStats analyse_reads(std::span<const std::uint64_t> reads) {
  ReadStats stats;
  stats.reads = reads.size();
  for (std::size_t i = 1; i < reads.size(); ++i) {
    if (reads[i] == reads[i - 1]) {
      ++stats.repeats;
    } else if (reads[i] < reads[i - 1]) {
      ++stats.backward;
    } else {
      const std::uint64_t delta = reads[i] - reads[i - 1];
      stats.granularity = std::gcd(stats.granularity, delta);
      stats.min_nonzero_delta =
          stats.min_nonzero_delta == 0 ? delta : std::min(stats.min_nonzero_delta, delta);
    }
  }
  return stats;
}

TimerReport calibrate_timer() {
  TimerReport report;
#ifdef __APPLE__
  report.tick_source = "mach_absolute_time";
#elifdef __x86_64__
  report.tick_source = "rdtscp";
#else
  report.tick_source = "cntvct_el0";
#endif
  report.clock_source = "CLOCK_MONOTONIC_RAW";
  report.tick_note = describe_tick_counter();

  report.ticks_per_second = calibrate_rate(report.calibration_spread);
  report.tick_reads = sample_reads([] { return read_ticks(); }, report.tick_read_cost_ns);
  const double ns_per_tick = report.ticks_per_second > 0 ? 1e9 / report.ticks_per_second : 0;
  report.tick_granularity_ns = static_cast<double>(report.tick_reads.granularity) * ns_per_tick;
  report.tick_min_delta_ns = static_cast<double>(report.tick_reads.min_nonzero_delta) * ns_per_tick;
  report.tick_resolution_ns = std::max(report.tick_granularity_ns, report.tick_min_delta_ns);

  timespec res{};
  clock_getres(CLOCK_MONOTONIC_RAW, &res);
  report.clock_getres_ns = static_cast<double>(res.tv_sec) * 1e9 + static_cast<double>(res.tv_nsec);
  double clock_cost = 0;
  report.clock_reads = sample_reads([] { return clock_ns(); }, clock_cost);
  report.clock_resolution_ns = static_cast<double>(
      std::max(report.clock_reads.granularity, report.clock_reads.min_nonzero_delta));
  return report;
}

std::string timer_problem(const TimerReport& report) {
  if (!(report.ticks_per_second > 0)) {
    return "calibration produced no tick rate";
  }
  if (report.tick_reads.granularity == 0) {
    return "the tick counter never moved";
  }
  if (report.tick_reads.backward != 0) {
    return "the tick counter went backward";
  }
  if (report.calibration_spread > kMaxSpread) {
    std::array<char, 96> text{};
    std::snprintf(text.data(), text.size(), "calibration spread %.4f is above the %.2f limit",
                  report.calibration_spread, kMaxSpread);
    return text.data();
  }
  return {};
}

}  // namespace aadp::platform
