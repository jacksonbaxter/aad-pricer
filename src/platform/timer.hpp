// Timers. read_ticks() is the batch timer: mach_absolute_time on macOS, rdtscp on x86-64 Linux,
// cntvct_el0 on arm64 Linux. clock_ns() reads CLOCK_MONOTONIC_RAW and is the reference the ticks
// are calibrated against, on every run. No frequency is assumed anywhere.
#pragma once

#include <time.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

namespace aadp::platform {

inline std::uint64_t read_ticks() noexcept {
#if defined(__APPLE__)
  return mach_absolute_time();
#elif defined(__linux__) && defined(__x86_64__)
  // rdtscp waits for earlier instructions to finish; plain rdtsc does not.
  std::uint32_t low = 0;
  std::uint32_t high = 0;
  std::uint32_t aux = 0;
  __asm__ volatile("rdtscp" : "=a"(low), "=d"(high), "=c"(aux));
  return (static_cast<std::uint64_t>(high) << 32) | low;
#elif defined(__linux__) && defined(__aarch64__)
  std::uint64_t value = 0;
  __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(value));
  return value;
#else
#error "Only macOS and Linux on arm64 or x86-64 are supported"
#endif
}

inline std::uint64_t clock_ns() noexcept {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC_RAW, &now);
  return static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000U +
         static_cast<std::uint64_t>(now.tv_nsec);
}

// What a run of consecutive timer reads shows.
struct ReadStats {
  std::size_t reads = 0;
  std::size_t repeats = 0;              // Same value twice: the read is faster than the tick
  std::size_t backward = 0;             // Later read gave a smaller value
  std::uint64_t min_nonzero_delta = 0;  // 0 if the timer never moved
  std::uint64_t granularity = 0;        // GCD of the forward deltas; 0 if the timer never moved
};

ReadStats analyse_reads(std::span<const std::uint64_t> reads);

struct TimerReport {
  const char* tick_source = "";
  double ticks_per_second = 0;
  double calibration_spread = 0;  // (max - min) / median over the calibration rounds
  ReadStats tick_reads;
  double tick_granularity_ns = 0;
  double tick_min_delta_ns = 0;
  // The smallest interval this timer can tell apart: the larger of its granularity and of the
  // smallest step two consecutive reads produce.
  double tick_resolution_ns = 0;
  double tick_read_cost_ns = 0;
  std::string tick_note;  // What the OS or CPU states about the counter, for comparison only

  const char* clock_source = "";
  double clock_getres_ns = 0;
  ReadStats clock_reads;
  double clock_resolution_ns = 0;
};

// Takes about 60 ms. Run it after pinning, because the reads should happen where the work will.
TimerReport calibrate_timer();

// Empty if the report can be used. Otherwise why not.
std::string timer_problem(const TimerReport& report);

}  // namespace aadp::platform
