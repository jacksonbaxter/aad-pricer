// Parser for the load lines measure.sh prints on stderr, on both hosts.
//
//   macOS: measure.sh: start load1=1.23 host=name at=2026-01-02T03:04:05Z
//          measure.sh: end load1=1.20 exit=0 at=2026-01-02T03:04:09Z
//   Linux: the same, plus cpus= governor= freq_khz= perf_event_paranoid=
//
// sanitized_measure_line() rebuilds a line from the validated fields it knows, so `host=` and
// any field a later measure.sh adds cannot reach a committed file.
#ifndef AADP_RESULTS_MEASURE_LINE_HPP_
#define AADP_RESULTS_MEASURE_LINE_HPP_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aadp::results {

enum class MeasureKind : std::uint8_t { kStart, kEnd };

struct MeasureLine {
  MeasureKind kind = MeasureKind::kStart;
  std::string load1;
  double load1_value = 0;
  std::string at;
  std::optional<int> exit_status;   // End lines only
  std::optional<std::string> host;  // Start lines; absent once sanitized
  std::optional<std::string> cpus;
  std::optional<std::string> governor;
  std::optional<std::string> freq_khz;
  std::optional<std::string> perf_event_paranoid;
};

std::expected<MeasureLine, std::string> parse_measure_line(std::string_view line);

std::string sanitized_measure_line(const MeasureLine& line);

struct MeasureScan {
  std::vector<MeasureLine> starts;
  std::vector<MeasureLine> ends;
  std::vector<std::string> malformed;  // Lines that claim to be start or end but do not parse
  std::size_t other_lines = 0;
};

// Sorts a captured stderr into start lines, end lines and everything else.
MeasureScan scan_measure_stderr(std::string_view captured);

// "4", "0-3,6". Refuses anything else, including ranges that run backward.
std::expected<std::vector<int>, std::string> expand_cpu_list(std::string_view list);

}  // namespace aadp::results

#endif  // AADP_RESULTS_MEASURE_LINE_HPP_
