// Peak RSS of one variant, measured in a process of its own.
//
// getrusage's peak is per process and never falls, so in one process every variant would report
// the peak of whatever ran first. Instead the benchmark starts itself again (posix_spawn, no
// shell) for each variant, the child runs that variant once, reports its own peak on stdout and
// exits. So each figure is the peak of a fresh process that ran only that variant, and the "idle"
// child, which runs nothing, gives the binary's own floor. The parent reads the child's stdout
// through a pipe and keeps only the report line, so nothing else of the child's can reach the
// results file.
//
// Why the child reports and the parent does not read wait4's ru_maxrss: on Linux that figure is
// the process's signal->maxrss, and exec folds the high-water RSS of the old address space into
// it. glibc's posix_spawn runs the child in the parent's address space until exec (CLONE_VM |
// CLONE_VFORK), so every child "peaked" at the parent's RSS. Found on linux-zen1 in stage 3b:
// every variant and the idle child read the same 4.1 MiB. VmHWM in /proc/self/status belongs to
// the address space exec made, so it is the child's own. macOS has no such inheritance, and there
// the child reports getrusage(RUSAGE_SELF), the figure wait4 gave.
#ifndef AADP_BENCH_RSS_PROBE_HPP_
#define AADP_BENCH_RSS_PROBE_HPP_

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace aadp::bench {

inline constexpr std::string_view kPeakReportPrefix = "aadp-rss-probe-peak-bytes ";

// Runs `executable` with `args` (argv[1] onwards) and returns the peak RSS in bytes the child
// reported with ReportOwnPeakRss, or why not.
std::expected<std::uint64_t, std::string> ChildPeakRss(const std::string& executable,
                                                       const std::vector<std::string>& args);

// In the child, just before it exits: this process's own peak RSS, written to stdout as one
// line, kPeakReportPrefix and the bytes. False if the peak cannot be read or written.
bool ReportOwnPeakRss();

// Bytes from the VmHWM line of a /proc/<pid>/status text (Linux).
std::expected<std::uint64_t, std::string> ParseVmHwm(std::string_view status);

// The value of the last report line in a child's stdout.
std::expected<std::uint64_t, std::string> ParsePeakReport(std::string_view output);

}  // namespace aadp::bench

#endif  // AADP_BENCH_RSS_PROBE_HPP_
