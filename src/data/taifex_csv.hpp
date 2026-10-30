#ifndef AADP_DATA_TAIFEX_CSV_HPP_
#define AADP_DATA_TAIFEX_CSV_HPP_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "data/decimal.hpp"

// What the two TAIFEX readers share: errors, sessions, tokenising and field validation. The files
// are cp950 CSV with CRLF line ends and no quoting.
namespace aadp::taifex {

enum class Session : std::uint8_t {
  kRegular,
  // Rows dated 20241115 with this session ran before that day's regular session, so a filter on
  // the date alone mixes two market states.
  kAfterHours,
};

enum class ErrorCode : std::uint8_t {
  kEmptyInput,
  kHeaderMismatch,
  kInvalidEncoding,
  kControlCharacter,
  kWrongColumnCount,
  kMissingField,
  kBadDate,
  kBadContract,
  kBadExpiry,
  kBadRight,
  kBadSession,
  kMalformedNumber,
  kNumberOutOfRange,
  kNegativeNumber,
};

std::string_view to_string(ErrorCode code);

struct ReadError {
  ErrorCode code;
  std::size_t line;      // 1-based. The header is line 1.
  std::size_t position;  // Byte offset, inside the input unless the input is empty.
  int column;            // 0-based, or kNoColumn when the error is not in one field.
};

inline constexpr int kNoColumn = -1;

// Splits an input into lines and each line into fields. Every step goes through the cp950 decoder.
class RecordReader {
 public:
  explicit RecordReader(std::string_view input);

  // Moves to the next line. Returns false at the end of the input.
  bool next();

  std::string_view input() const { return input_; }
  std::size_t line() const { return line_; }
  std::size_t line_begin() const { return line_begin_; }
  std::span<const std::string_view> fields() const { return fields_; }
  // The encoding or control-character error on this line, if any. The line's fields are then
  // incomplete, and the reader has already resumed at the next line.
  const std::optional<ReadError>& error() const { return error_; }

 private:
  void add_field(std::size_t end);
  void fail_line(ErrorCode code, std::size_t position);

  std::string_view input_;
  std::size_t next_offset_ = 0;
  std::size_t line_ = 0;
  std::size_t line_begin_ = 0;
  std::size_t field_begin_ = 0;
  std::vector<std::string_view> fields_;
  std::optional<ReadError> error_;
};

// Reads the header line and requires exactly these labels.
std::expected<void, ReadError> check_header(RecordReader& records,
                                            std::span<const std::string_view> labels);

// Validates the fields of one row. The first failure sticks, and later calls return a default.
class RowFields {
 public:
  RowFields(const RecordReader& records, std::size_t column_count);

  bool ok() const { return !error_.has_value(); }
  const ReadError& error() const { return *error_; }

  std::int32_t date(int column);
  std::string contract(int column);
  // YYYYMM with an optional weekly suffix, or with allow_spread two of those joined by '/'.
  std::string expiry(int column, bool allow_spread);
  Session session(int column);
  bool is_empty(int column) const { return field(column).empty(); }
  std::string_view field(int column) const;

  // Required and strictly positive.
  double strike(int column);
  // These treat an empty field or "-" as missing.
  std::optional<double> number(int column, SignPolicy sign);
  std::optional<double> percent(int column);
  std::optional<std::int64_t> count(int column);

  void fail(ErrorCode code, int column);

 private:
  void fail_number(NumberError error, int column);

  const RecordReader& records_;
  std::optional<ReadError> error_;
};

inline bool is_missing(std::string_view field) { return field.empty() || field == "-"; }

// Every line after the header, one row or one error each. Bad rows do not stop the read.
template <typename Row, typename ParseRow>
void read_rows(RecordReader& records, std::size_t column_count, ParseRow parse_row,
               std::vector<Row>& rows, std::vector<ReadError>& errors) {
  while (records.next()) {
    RowFields fields(records, column_count);
    Row row = fields.ok() ? parse_row(fields) : Row{};
    if (fields.ok()) {
      row.line = records.line();
      rows.push_back(std::move(row));
    } else {
      errors.push_back(fields.error());
    }
  }
}

// Reads a whole file as raw bytes, for the callers of the readers. Returns a message on failure.
std::expected<std::string, std::string> read_binary_file(const std::filesystem::path& path);

}  // namespace aadp::taifex

#endif  // AADP_DATA_TAIFEX_CSV_HPP_
