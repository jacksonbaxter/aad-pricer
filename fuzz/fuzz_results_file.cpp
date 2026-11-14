// Properties, beyond not crashing: a parsed file writes out and reads back equal, and whatever
// the stamp accepts holds no dropped key, no host field and nothing the scan would find. For the
// report grammar (results/report.hpp): a section it reads is written back byte for byte, every
// real cell is the one spelling of a finite double, and a stamped report that uses the chain
// carries every line of the attribution.
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "results/privacy.hpp"
#include "results/report.hpp"
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

namespace r = aadp::results;

// A report section read is the section written: the grammar has one spelling of everything.
void check_report_section(const r::ResultsSection& section) {
  const auto table = r::parse_report_section(section);
  if (!table) {
    FUZZ_ASSERT(!table.error().detail.empty());
    return;
  }
  const auto written = r::write_report_section(*table);
  FUZZ_ASSERT(written.has_value());
  const std::string& body = section.body;
  FUZZ_ASSERT(written->body == (body.empty() || body.ends_with('\n') ? body : body + "\n"));
  for (const r::ReportRow& row : table->rows) {
    FUZZ_ASSERT(row.cells.size() == table->columns.size());
    for (std::size_t i = 0; i < row.cells.size(); ++i) {
      if (table->columns[i].kind == r::CellKind::kReal) {
        const auto value = r::read_real_cell(row.cells[i]);
        FUZZ_ASSERT(value.has_value() && r::real_cell(*value) == row.cells[i]);
      }
    }
  }
}

void check_report_file(const r::ResultsFile& file) {
  const auto kind = r::file_kind(file);
  const auto report = r::read_report(file);
  FUZZ_ASSERT(!report.has_value() || (kind.has_value() && *kind == r::FileKind::kReport));
  if (!report) {
    return;
  }
  bool uses_chain = false;
  for (const r::ResultsSection& section : file.sections) {
    if (section.name != r::kAttributionSection) {
      check_report_section(section);
    }
  }
  for (const r::ReportTable& table : *report) {
    uses_chain = uses_chain || table.uses_chain;
  }
  if (uses_chain) {
    const r::ResultsSection* attribution = file.section(r::kAttributionSection);
    FUZZ_ASSERT(attribution != nullptr);
    for (const std::string_view fragment : r::kAttributionFragments) {
      FUZZ_ASSERT(attribution->body.contains(fragment));
    }
  }
  const std::span<const r::ResultsFile> one(&file, 1);
  const auto tables = r::render_report_tables(one, {.iteration = true});
  FUZZ_ASSERT(tables.has_value() || !tables.error().empty());
}

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
    // The iteration table admits more files, so it reaches the AAD tables on more inputs.
    const auto iteration = aadp::results::render_tables(one, {.iteration = true});
    FUZZ_ASSERT(iteration.has_value() || !iteration.error().empty());
    FUZZ_ASSERT(!tables.has_value() || iteration.has_value());
    check_report_file(*file);
  }
  // The input as one report section's body, so that the grammar is reached without a header.
  check_report_section({.name = "report_fuzz", .body = std::string(text)});

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
    const auto back = aadp::results::parse_results_file(*stamped);
    FUZZ_ASSERT(back.has_value());
    // The stamp keeps a report's rules: what it writes reads as a report again.
    if (r::file_kind(*back) == r::FileKind::kReport) {
      FUZZ_ASSERT(r::read_report(*back).has_value());
      check_report_file(*back);
    }
  }
  return 0;
}
