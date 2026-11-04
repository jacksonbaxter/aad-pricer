// Peak RSS of one variant, measured in a process of its own.
//
// getrusage's peak is per process and never falls, so in one process every variant would report
// the peak of whatever ran first. Instead the benchmark starts itself again (posix_spawn, no
// shell) for each variant, the child runs that variant once and exits, and the parent reads the
// child's peak from wait4. So each figure is the peak of a fresh process that ran only that
// variant, and the "idle" child, which runs nothing, gives the binary's own floor. stdout of the
// child goes to /dev/null, so it cannot reach the results file.
#ifndef AADP_BENCH_RSS_PROBE_HPP_
#define AADP_BENCH_RSS_PROBE_HPP_

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace aadp::bench {

// Runs `executable` with `args` (argv[1] onwards) and returns its peak RSS in bytes, or why not.
std::expected<std::uint64_t, std::string> ChildPeakRss(const std::string& executable,
                                                       const std::vector<std::string>& args);

}  // namespace aadp::bench

#endif  // AADP_BENCH_RSS_PROBE_HPP_
