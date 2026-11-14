// The report kind of results file (results/report.hpp): each grammar rule on a hand-written
// section, the cells' one spelling, the file's kind, size and attribution.
#include "results/report.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "fixtures.hpp"
#include "results/results_file.hpp"

namespace aadp::results {
namespace {

ResultsSection section_of(std::string_view body, std::string_view name = "report_fit_error") {
  return {.name = std::string(name), .body = std::string(body)};
}

// The fixture's body with one line replaced, `from` taken as a whole line.
std::string with_line(std::string_view from, std::string_view to) {
  std::string body(testing::kReportBody);
  const std::string line = std::string(from) + "\n";
  const auto at = body.find(line);
  EXPECT_NE(at, std::string::npos) << from;
  if (at != std::string::npos) {
    body.replace(at, line.size(), to.empty() ? std::string() : std::string(to) + "\n");
  }
  return body;
}

void expect_problem(std::string_view body, ReportProblem problem) {
  const auto table = parse_report_section(section_of(body));
  ASSERT_FALSE(table.has_value()) << "read a section that should have been refused:\n" << body;
  EXPECT_EQ(table.error().problem, problem) << table.error().detail;
}

ReportTable good_table() {
  auto table = parse_report_section(section_of(testing::kReportBody));
  EXPECT_TRUE(table.has_value()) << table.error().detail;
  return table.value_or(ReportTable{});
}

TEST(ReportCell, RealIsTheShortestSpellingThatReadsBackBitForBit) {
  EXPECT_EQ(real_cell(0.1), "0.1");
  EXPECT_EQ(real_cell(1.0), "1");
  EXPECT_EQ(real_cell(-2.5), "-2.5");
  EXPECT_EQ(real_cell(1e300), "1e+300");
  EXPECT_EQ(real_cell(1.0 / 3.0), "0.3333333333333333");
  for (const double value :
       {0.0, -0.0, 1.0 / 3.0, 2.0 / 3.0, 0.28719839201234567, 6.02214076e23,
        std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest(), std::numeric_limits<double>::min(),
        std::nextafter(1.0, 2.0)}) {
    const std::string text = real_cell(value);
    const auto back = read_real_cell(text);
    ASSERT_TRUE(back.has_value()) << text;
    const double got = back.value_or(std::numeric_limits<double>::quiet_NaN());
    EXPECT_EQ(std::signbit(got), std::signbit(value)) << text;
    EXPECT_EQ(got, value) << text;
  }
}

TEST(ReportCell, NoNonFiniteNumberHasASpelling) {
  EXPECT_EQ(real_cell(std::numeric_limits<double>::infinity()), "");
  EXPECT_EQ(real_cell(-std::numeric_limits<double>::infinity()), "");
  EXPECT_EQ(real_cell(std::numeric_limits<double>::quiet_NaN()), "");
  for (const std::string_view text : {"inf", "-inf", "nan", "NaN", "1e999", "-1e400", "infinity"}) {
    EXPECT_FALSE(read_real_cell(text).has_value()) << text;
  }
}

TEST(ReportCell, ARealHasOneSpellingOnly) {
  for (const std::string_view text :
       {"", "0.10", "+0.1", " 0.1", "0.1 ", ".1", "1.", "1e0", "1E+300", "0x1p0", "1e+0300", "00.1",
        "0.1.", "--1", "1,5", "0.33333333333333331000", "1e+300x"}) {
    EXPECT_FALSE(read_real_cell(text).has_value()) << text;
  }
  EXPECT_EQ(read_real_cell("-0"), -0.0);
  EXPECT_EQ(read_real_cell("1e-05"), 1e-05);
}

TEST(ReportCell, AnIntegerIsPlainDecimal) {
  EXPECT_EQ(read_integer_cell("0"), 0);
  EXPECT_EQ(read_integer_cell("-7"), -7);
  EXPECT_EQ(read_integer_cell("9223372036854775807"), std::numeric_limits<std::int64_t>::max());
  for (const std::string_view text :
       {"", "+1", "01", "-0", "1.0", "1e3", " 1", "1 ", "9223372036854775808", "0x10"}) {
    EXPECT_FALSE(read_integer_cell(text).has_value()) << text;
  }
  EXPECT_EQ(integer_cell(-42), "-42");
}

TEST(ReportSection, ReadsTheHandWrittenSection) {
  const ReportTable table = good_table();
  EXPECT_EQ(table.name, "fit_error");
  EXPECT_EQ(table.title, "Fit error per slice");
  EXPECT_EQ(table.targets, (std::vector<std::string>{"SVI-6", "SVI-11"}));
  EXPECT_FALSE(table.timing);
  EXPECT_FALSE(table.uses_chain);
  ASSERT_EQ(table.columns.size(), 3U);
  EXPECT_EQ(table.columns[0],
            (ReportColumn{.name = "error", .kind = CellKind::kReal, .unit = "bp_of_F"}));
  EXPECT_EQ(table.columns[1].kind, CellKind::kInteger);
  EXPECT_EQ(table.columns[2].kind, CellKind::kText);
  ASSERT_EQ(table.rows.size(), 2U);
  EXPECT_EQ(table.row("s2W4")->cells, (std::vector<std::string>{"1.5", "51", "no-arbitrage"}));
  EXPECT_EQ(table.column("quotes"), 1U);
  EXPECT_EQ(table.row("s3"), nullptr);
}

TEST(ReportSection, WritesWhatItReadsByteForByte) {
  const auto section = write_report_section(good_table());
  ASSERT_TRUE(section.has_value()) << section.error().detail;
  EXPECT_EQ(section->name, "report_fit_error");
  EXPECT_EQ(section->body, testing::kReportBody);
}

TEST(ReportSection, TargetsNoneIsAnEmptyList) {
  const std::string body = with_line("targets SVI-6,SVI-11", "targets none");
  const auto table = parse_report_section(section_of(body));
  ASSERT_TRUE(table.has_value()) << table.error().detail;
  EXPECT_TRUE(table->targets.empty());
  EXPECT_EQ(write_report_section(*table)->body, body);
}

TEST(ReportSection, RefusesAnUnknownType) {
  expect_problem(with_line("type table", "type chart"), ReportProblem::kUnknownType);
}

TEST(ReportSection, RefusesFixedLinesOutOfOrderMissingOrRepeated) {
  expect_problem(with_line("type table", ""), ReportProblem::kMalformed);
  expect_problem(with_line("timing no", ""), ReportProblem::kMalformed);
  expect_problem(with_line("uses_chain no", "uses_chain no\nuses_chain no"),
                 ReportProblem::kMalformed);
  expect_problem(with_line("title Fit error per slice", "targets none\ntitle Fit error per slice"),
                 ReportProblem::kMalformed);
  expect_problem(with_line("timing no", "timing maybe"), ReportProblem::kMalformed);
  expect_problem(with_line("uses_chain no", "uses_chain YES"), ReportProblem::kMalformed);
  expect_problem("type table\n", ReportProblem::kMalformed);
  expect_problem("", ReportProblem::kMalformed);
}

TEST(ReportSection, RefusesATitleOrTargetOutsideItsCharacterSet) {
  for (const std::string_view title :
       {"title Fit | error", "title  Fit", "title Fit ", "title Fit `x`", "title Fit\terror",
        "title Fit <b>", "title [link](x)", "title *"}) {
    expect_problem(with_line("title Fit error per slice", title), ReportProblem::kMalformed);
  }
  expect_problem(with_line("title Fit error per slice", "title " + std::string(161, 'x')),
                 ReportProblem::kMalformed);
  for (const std::string_view targets :
       {"targets svi-6", "targets SVI-6,", "targets SVI-6,SVI-6", "targets SVI 6", "targets "}) {
    expect_problem(with_line("targets SVI-6,SVI-11", targets), ReportProblem::kMalformed);
  }
}

TEST(ReportSection, RefusesABadOrRepeatedColumn) {
  for (const std::string_view column :
       {"column error real bp_of_F", "column Error real none", "column 1error real none",
        "column error-2 real none", "column error2 float none", "column error2 real",
        "column error2 real bp of F", "column error2 real b|p", "column error2 real "}) {
    expect_problem(with_line("column class text none", column), ReportProblem::kBadColumn);
  }
}

TEST(ReportSection, RefusesAColumnAfterTheRowsAndARowBeforeTheColumns) {
  expect_problem(std::string(testing::kReportBody) + "column late real none\n",
                 ReportProblem::kMalformed);
  // A late column with a row that fits it would leave the earlier rows a cell short.
  expect_problem(std::string(testing::kReportBody) + "column late real none\nrow s3 1 2 x 4\n",
                 ReportProblem::kMalformed);
  expect_problem(with_line("column error real bp_of_F", "row s0 1 2 x"), ReportProblem::kMalformed);
}

TEST(ReportSection, RefusesARepeatedOrBadRowKeyAndTheWrongNumberOfCells) {
  expect_problem(with_line("row s2W4 1.5 51 no-arbitrage", "row s1 1.5 51 no-arbitrage"),
                 ReportProblem::kBadRow);
  for (const std::string_view row :
       {"row s2W4 1.5 51", "row s2W4 1.5 51 no-arbitrage extra", "row s2W4", "row s2/W4 1.5 51 x",
        "row  1.5 51 x", "row s2W4 1.5  51 x", "row s2W4 1.5 51 x "}) {
    expect_problem(with_line("row s2W4 1.5 51 no-arbitrage", row), ReportProblem::kBadRow);
  }
}

TEST(ReportSection, RefusesACellItsColumnDoesNotAdmit) {
  const std::string long_text = "row s2W4 1.5 51 " + std::string(65, 'x');
  for (const std::string_view row :
       {std::string_view("row s2W4 inf 51 x"), std::string_view("row s2W4 nan 51 x"),
        std::string_view("row s2W4 1e999 51 x"), std::string_view("row s2W4 1.50 51 x"),
        std::string_view("row s2W4 1.5 51.0 x"), std::string_view("row s2W4 1.5 x 51"),
        std::string_view("row s2W4 1.5 51 a|b"), std::string_view("row s2W4 1.5 51 `x`"),
        std::string_view(long_text)}) {
    expect_problem(with_line("row s2W4 1.5 51 no-arbitrage", row), ReportProblem::kBadCell);
  }
}

TEST(ReportSection, RefusesATableWithoutRowsAndABlankLine) {
  const std::string body(testing::kReportBody);
  expect_problem(body.substr(0, body.find("row ")), ReportProblem::kMalformed);
  expect_problem(std::string(testing::kReportBody) + "\n", ReportProblem::kMalformed);
  expect_problem(with_line("timing no", "timing no\n"), ReportProblem::kMalformed);
}

TEST(ReportSection, RefusesTooManyColumnsOrRows) {
  std::string columns;
  for (std::size_t i = 0; i <= kMaxReportColumns; ++i) {
    columns += "column c" + std::to_string(i) + " integer none\n";
  }
  std::string body = with_line("column error real bp_of_F", "");
  body = body.substr(0, body.find("column ")) + columns + "row r";
  for (std::size_t i = 0; i <= kMaxReportColumns; ++i) {
    body += " 1";
  }
  expect_problem(body + "\n", ReportProblem::kTooLarge);

  std::string rows = std::string(testing::kReportBody);
  for (std::size_t i = 0; i < kMaxReportRows; ++i) {
    rows += "row r" + std::to_string(i) + " 1 1 x\n";
  }
  expect_problem(rows, ReportProblem::kTooLarge);
}

TEST(ReportSection, RefusesABadName) {
  for (const std::string_view name :
       {"report_", "report_Fit", "report_fit-error", "report_fit.x"}) {
    const auto table = parse_report_section(section_of(testing::kReportBody, name));
    EXPECT_FALSE(table.has_value()) << name;
  }
  EXPECT_EQ(parse_report_section(section_of(testing::kReportBody, "fit_error")).error().problem,
            ReportProblem::kUnknownSection);
}

TEST(ReportSection, TheWriterRefusesWhatTheReaderWould) {
  ReportTable table = good_table();
  table.rows[0].cells[0] = real_cell(std::numeric_limits<double>::infinity());
  EXPECT_FALSE(write_report_section(table).has_value());
  table = good_table();
  table.rows[1].key = table.rows[0].key;
  EXPECT_EQ(write_report_section(table).error().problem, ReportProblem::kBadRow);
  table = good_table();
  table.name = "Fit";
  EXPECT_FALSE(write_report_section(table).has_value());
  table = good_table();
  table.title = "a\nrow x 1 2 3";
  EXPECT_FALSE(write_report_section(table).has_value());
  table = good_table();
  table.rows[0].cells[2] = "two words";
  EXPECT_FALSE(write_report_section(table).has_value());
  table = good_table();
  table.rows.clear();
  EXPECT_FALSE(write_report_section(table).has_value());
  // Read back as two targets: the reader takes it, but not as the table it was given.
  table = good_table();
  table.targets = {"SVI-6,SVI-11"};
  const auto two = write_report_section(table);
  ASSERT_FALSE(two.has_value());
  EXPECT_EQ(two.error().detail, "the table does not read back as written");
}

// What is read is written back as the same text, with the last line's newline if it had none.
void expect_read_or_refused(const std::string& body) {
  const auto table = parse_report_section(section_of(body));
  if (!table) {
    return;
  }
  const auto section = write_report_section(*table);
  ASSERT_TRUE(section.has_value()) << body;
  EXPECT_EQ(section->body, body.ends_with('\n') ? body : body + "\n");
}

// A crude fuzz in the test itself: every prefix and every one-byte change of a good body is
// either read, and then written back as itself, or refused with a problem; never a crash.
TEST(ReportSection, EveryPrefixAndByteChangeIsReadOrRefused) {
  const std::string body(testing::kReportBody);
  for (std::size_t n = 0; n <= body.size(); ++n) {
    expect_read_or_refused(body.substr(0, n));
  }
  for (std::size_t at = 0; at < body.size(); ++at) {
    for (const char c : {'\0', ' ', '\n', '|', 'x', '9', '-', '\xff'}) {
      std::string changed = body;
      changed[at] = c;
      expect_read_or_refused(changed);
    }
  }
}

TEST(ReportFile, IsABenchmarkFileOrAReportFileNeverBothNorNeither) {
  EXPECT_EQ(file_kind(testing::raw_file()), FileKind::kBenchmark);
  EXPECT_EQ(file_kind(testing::raw_report_file()), FileKind::kReport);
  ResultsFile both = testing::raw_file();
  both.sections.push_back(section_of(testing::kReportBody));
  const auto both_kind = file_kind(both);
  ASSERT_FALSE(both_kind.has_value());
  EXPECT_EQ(both_kind.error().problem, ReportProblem::kKind);
  ResultsFile neither = testing::raw_file();
  neither.sections.clear();
  neither.sections.push_back({.name = "histogram_x", .body = "p50 1\n"});
  const auto neither_kind = file_kind(neither);
  ASSERT_FALSE(neither_kind.has_value());
  EXPECT_EQ(neither_kind.error().problem, ReportProblem::kKind);
  EXPECT_EQ(read_report(testing::raw_file()).error().problem, ReportProblem::kKind);
}

TEST(ReportFile, ReadsEveryTableInFileOrder) {
  ResultsFile file = testing::raw_report_file();
  file.sections.push_back(section_of(testing::kReportBody, "report_again"));
  const auto tables = read_report(file);
  ASSERT_TRUE(tables.has_value()) << tables.error().detail;
  ASSERT_EQ(tables->size(), 2U);
  EXPECT_EQ((*tables)[0].name, "fit_error");
  EXPECT_EQ((*tables)[1].name, "again");
}

TEST(ReportFile, RefusesAnyOtherSection) {
  ResultsFile file = testing::raw_report_file();
  file.sections.push_back({.name = "histogram_x", .body = "p50 1\n"});
  const auto tables = read_report(file);
  ASSERT_FALSE(tables.has_value());
  EXPECT_EQ(tables.error().problem, ReportProblem::kUnknownSection) << tables.error().detail;
}

TEST(ReportFile, NamesTheSectionABadTableIsIn) {
  const ResultsFile file = testing::raw_report_file(with_line("type table", "type chart"));
  const auto tables = read_report(file);
  ASSERT_FALSE(tables.has_value());
  EXPECT_EQ(tables.error().problem, ReportProblem::kUnknownType);
  EXPECT_TRUE(tables.error().detail.starts_with("section report_fit_error: "))
      << tables.error().detail;
}

TEST(ReportFile, ASectionFromTheChainNeedsTheWholeAttribution) {
  ResultsFile file = testing::raw_report_file(with_line("uses_chain no", "uses_chain yes"));
  EXPECT_EQ(read_report(file).error().problem, ReportProblem::kNoAttribution);
  file.sections.push_back({.name = "attribution", .body = testing::attribution_body()});
  EXPECT_TRUE(read_report(file).has_value()) << read_report(file).error().detail;
  for (const std::string_view fragment : kAttributionFragments) {
    ResultsFile cut = file;
    std::string& body = cut.sections.back().body;
    body.erase(body.find(fragment), fragment.size());
    EXPECT_EQ(read_report(cut).error().problem, ReportProblem::kNoAttribution) << fragment;
  }
}

TEST(ReportFile, AnAttributionIsAllowedWithoutTheChain) {
  ResultsFile file = testing::raw_report_file();
  file.sections.push_back({.name = "attribution", .body = "not required here\n"});
  EXPECT_TRUE(read_report(file).has_value());
}

TEST(ReportFile, RefusesTooManySectionsOrTooManyBytes) {
  ResultsFile file = testing::raw_report_file();
  for (std::size_t i = 0; i < kMaxReportSections; ++i) {
    file.sections.push_back(section_of(testing::kReportBody, "report_t" + std::to_string(i)));
  }
  EXPECT_EQ(read_report(file).error().problem, ReportProblem::kTooLarge);

  ResultsFile large = testing::raw_report_file();
  large.sections.push_back(
      {.name = "attribution", .body = std::string(kMaxReportFileBytes, 'x') + "\n"});
  EXPECT_EQ(read_report(large).error().problem, ReportProblem::kTooLarge);
  // The limit is on the file as written: a file just under it is read.
  const std::size_t header = write_results_file(testing::raw_report_file())->size();
  ResultsFile fits = testing::raw_report_file();
  const std::size_t marker = std::string_view("@section attribution\n").size();
  fits.sections.push_back(
      {.name = "attribution",
       .body = std::string(kMaxReportFileBytes - header - marker - 1, 'x') + "\n"});
  EXPECT_EQ(write_results_file(fits)->size(), kMaxReportFileBytes);
  EXPECT_TRUE(read_report(fits).has_value()) << read_report(fits).error().detail;
}

TEST(ReportFile, NumberCellsReadAsDoublesForTheHeadline) {
  const ReportTable table = good_table();
  EXPECT_EQ(read_number_cell(table.columns[0], "0.25"), 0.25);
  EXPECT_EQ(read_number_cell(table.columns[1], "40"), 40.0);
  EXPECT_FALSE(read_number_cell(table.columns[2], "type-1").has_value());
  EXPECT_FALSE(read_number_cell(table.columns[1], "4.5").has_value());
}

}  // namespace
}  // namespace aadp::results
