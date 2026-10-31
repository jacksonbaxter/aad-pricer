// Properties, beyond not crashing: a parsed line sanitizes to text with no host in it, the
// sanitized text parses back to itself, and every parsed value is in its stated range.
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "results/measure_line.hpp"

#define FUZZ_ASSERT(condition) \
  do {                         \
    if (!(condition)) {        \
      __builtin_trap();        \
    }                          \
  } while (false)

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view text(std::bit_cast<const char*>(data), size);

  const auto line = aadp::results::parse_measure_line(text);
  if (line) {
    FUZZ_ASSERT(std::isfinite(line->load1_value) && line->load1_value >= 0);
    FUZZ_ASSERT(line->at.size() == 20);
    if (line->exit_status) {
      FUZZ_ASSERT(*line->exit_status >= 0 && *line->exit_status <= 255);
    }
    FUZZ_ASSERT(line->kind == aadp::results::MeasureKind::kStart || line->exit_status.has_value());

    const std::string clean = aadp::results::sanitized_measure_line(*line);
    FUZZ_ASSERT(clean.find("host=") == std::string::npos);
    const auto again = aadp::results::parse_measure_line(clean);
    FUZZ_ASSERT(again.has_value());
    FUZZ_ASSERT(aadp::results::sanitized_measure_line(*again) == clean);
    FUZZ_ASSERT(again->kind == line->kind);
    FUZZ_ASSERT(again->load1 == line->load1);
    FUZZ_ASSERT(again->exit_status == line->exit_status);
  }

  const auto scan = aadp::results::scan_measure_stderr(text);
  for (const auto& start : scan.starts) {
    FUZZ_ASSERT(start.kind == aadp::results::MeasureKind::kStart);
  }
  for (const auto& end : scan.ends) {
    FUZZ_ASSERT(end.kind == aadp::results::MeasureKind::kEnd);
    FUZZ_ASSERT(end.exit_status.has_value());
  }

  const auto cpus = aadp::results::expand_cpu_list(text);
  if (cpus) {
    FUZZ_ASSERT(!cpus->empty());
  }
  return 0;
}
