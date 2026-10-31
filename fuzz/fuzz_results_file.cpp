// Properties, beyond not crashing: a parsed file writes out and reads back equal, and whatever
// the stamp accepts holds no dropped key, no host field and nothing the scan would find.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "results/privacy.hpp"
#include "results/results_file.hpp"
#include "results/stamp.hpp"
#include "results/table.hpp"

#define FUZZ_ASSERT(condition) \
  do {                         \
    if (!(condition)) {        \
      __builtin_trap();        \
    }                          \
  } while (false)

namespace {

constexpr std::string_view kCapture =
    "measure.sh: start load1=0.5 host=fuzzbox at=2026-01-02T03:04:05Z cpus=4\n"
    "measure.sh: end load1=0.6 exit=0 at=2026-01-02T03:04:09Z cpus=4\n";

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);

  const auto file = aadp::results::parse_results_file(text);
  if (file) {
    const auto written = aadp::results::write_results_file(*file);
    FUZZ_ASSERT(written.has_value());
    const auto back = aadp::results::parse_results_file(*written);
    FUZZ_ASSERT(back.has_value());
    FUZZ_ASSERT(*back == *file);

    // Exercises the checks a reader makes. A table of any file it accepts must not crash.
    const auto valid = aadp::results::validate_stamped(*file);
    FUZZ_ASSERT(valid.has_value() || !valid.error().empty());
    const std::span<const aadp::results::ResultsFile> one(&*file, 1);
    const auto tables = aadp::results::render_tables(one);
    FUZZ_ASSERT(tables.has_value() || !tables.error().empty());
  }

  aadp::results::StampRequest request;
  request.raw_output = text;
  request.captured_stderr = kCapture;
  request.current_host_label = "linux-zen1";
  request.current_hostname = "fuzzbox";
  for (const bool reportable : {true, false}) {
    request.reportable = reportable;
    request.not_reportable_reason = reportable ? "" : "fuzz";
    const auto stamped = aadp::results::stamp_results(request);
    if (!stamped) {
      continue;
    }
    FUZZ_ASSERT(!stamped->contains("host="));
    FUZZ_ASSERT(!stamped->contains("\"host_name\""));
    FUZZ_ASSERT(!stamped->contains("\"executable\""));
    aadp::results::PrivacyConfig privacy;
    privacy.hostnames = {"fuzzbox"};
    FUZZ_ASSERT(aadp::results::scan_private_text(*stamped, privacy).empty());
    FUZZ_ASSERT(aadp::results::parse_results_file(*stamped).has_value());
  }
  return 0;
}
