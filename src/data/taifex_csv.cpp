#include "data/taifex_csv.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <ios>
#include <iterator>
#include <utility>

#include "data/cp950.hpp"

namespace aadp::taifex {

namespace {

// Labels as cp950 byte strings, since the decoder carries no mapping table.
constexpr std::string_view kRegularLabel = "\xA4\x40\xAF\xEB";     // 一般
constexpr std::string_view kAfterHoursLabel = "\xBD\x4C\xAB\xE1";  // 盤後

constexpr std::size_t kMaxContractLength = 8;

constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool all_digits(std::string_view text) { return std::ranges::all_of(text, is_digit); }

// An empty last field at the end of the input sits one past the last byte.
std::size_t inside(std::string_view input, std::size_t position) {
  return input.empty() ? 0 : std::min(position, input.size() - 1);
}

// YYYYMM, or YYYYMMWn for a weekly. Returns false for anything else.
bool is_single_expiry(std::string_view text) {
  if (text.size() != 6 && text.size() != 8) {
    return false;
  }
  if (!all_digits(text.substr(0, 6))) {
    return false;
  }
  const int month = (text[4] - '0') * 10 + (text[5] - '0');
  if (month < 1 || month > 12) {
    return false;
  }
  return text.size() == 6 || (text[6] == 'W' && text[7] >= '1' && text[7] <= '5');
}

}  // namespace

std::string_view to_string(ErrorCode code) {
  switch (code) {
    case ErrorCode::kEmptyInput:
      return "empty input";
    case ErrorCode::kHeaderMismatch:
      return "header mismatch";
    case ErrorCode::kInvalidEncoding:
      return "invalid cp950 encoding";
    case ErrorCode::kControlCharacter:
      return "control character";
    case ErrorCode::kWrongColumnCount:
      return "wrong column count";
    case ErrorCode::kMissingField:
      return "missing required field";
    case ErrorCode::kBadDate:
      return "bad date";
    case ErrorCode::kBadContract:
      return "bad contract";
    case ErrorCode::kBadExpiry:
      return "bad expiry";
    case ErrorCode::kBadRight:
      return "bad call or put label";
    case ErrorCode::kBadSession:
      return "bad session label";
    case ErrorCode::kMalformedNumber:
      return "malformed number";
    case ErrorCode::kNumberOutOfRange:
      return "number out of range";
    case ErrorCode::kNegativeNumber:
      return "negative number";
  }
  return "unknown error";
}

RecordReader::RecordReader(std::string_view input) : input_(input) {}

void RecordReader::add_field(std::size_t end) {
  fields_.push_back(input_.substr(field_begin_, end - field_begin_));
}

void RecordReader::fail_line(ErrorCode code, std::size_t position) {
  error_ = ReadError{.code = code, .line = line_, .position = position, .column = kNoColumn};
  // A raw scan is safe here, unlike inside a line: LF is below every trail byte, so it can only
  // be a line end, and a bad pair must not swallow the line after it.
  const auto lf = input_.find('\n', position);
  next_offset_ = lf == std::string_view::npos ? input_.size() : lf + 1;
}

bool RecordReader::next() {
  if (next_offset_ >= input_.size()) {
    return false;
  }
  ++line_;
  line_begin_ = next_offset_;
  field_begin_ = next_offset_;
  fields_.clear();
  error_.reset();

  std::size_t offset = next_offset_;
  while (offset < input_.size()) {
    const auto unit = cp950::decode_unit(input_, offset);
    if (!unit) {
      fail_line(ErrorCode::kInvalidEncoding, unit.error().position);
      return true;
    }
    if (unit->length == 1) {
      const char c = input_[offset];
      if (c == ',') {
        add_field(offset);
        field_begin_ = offset + 1;
      } else if (c == '\n') {
        add_field(offset);
        next_offset_ = offset + 1;
        return true;
      } else if (c == '\r' && offset + 1 < input_.size() && input_[offset + 1] == '\n') {
        add_field(offset);
        next_offset_ = offset + 2;
        return true;
      } else if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
        // Includes NUL, tab and a bare CR, none of which belongs in these files.
        fail_line(ErrorCode::kControlCharacter, offset);
        return true;
      }
    }
    offset += unit->length;
  }
  add_field(offset);
  next_offset_ = offset;
  return true;
}

std::expected<void, ReadError> check_header(RecordReader& records,
                                            std::span<const std::string_view> labels) {
  const std::string_view input = records.input();
  if (!records.next()) {
    return std::unexpected(
        ReadError{.code = ErrorCode::kEmptyInput, .line = 0, .position = 0, .column = kNoColumn});
  }
  if (records.error()) {
    return std::unexpected(*records.error());
  }
  const auto fields = records.fields();
  if (fields.size() != labels.size()) {
    return std::unexpected(ReadError{.code = ErrorCode::kHeaderMismatch,
                                     .line = records.line(),
                                     .position = inside(input, records.line_begin()),
                                     .column = kNoColumn});
  }
  for (std::size_t i = 0; i < labels.size(); ++i) {
    if (fields[i] != labels[i]) {
      const auto position = static_cast<std::size_t>(fields[i].data() - input.data());
      return std::unexpected(ReadError{.code = ErrorCode::kHeaderMismatch,
                                       .line = records.line(),
                                       .position = inside(input, position),
                                       .column = static_cast<int>(i)});
    }
  }
  return {};
}

RowFields::RowFields(const RecordReader& records, std::size_t column_count) : records_(&records) {
  if (records.error()) {
    error_ = records.error();
  } else if (records.fields().size() != column_count) {
    error_ = ReadError{.code = ErrorCode::kWrongColumnCount,
                       .line = records.line(),
                       .position = inside(records.input(), records.line_begin()),
                       .column = kNoColumn};
  }
}

std::string_view RowFields::field(int column) const {
  return records_->fields()[static_cast<std::size_t>(column)];
}

void RowFields::fail(ErrorCode code, int column) {
  if (error_) {
    return;
  }
  const std::string_view input = records_->input();
  const auto position = static_cast<std::size_t>(field(column).data() - input.data());
  error_ = ReadError{.code = code,
                     .line = records_->line(),
                     .position = inside(input, position),
                     .column = column};
}

void RowFields::fail_number(NumberError error, int column) {
  switch (error) {
    case NumberError::kMalformed:
      fail(ErrorCode::kMalformedNumber, column);
      break;
    case NumberError::kOutOfRange:
      fail(ErrorCode::kNumberOutOfRange, column);
      break;
    case NumberError::kNegative:
      fail(ErrorCode::kNegativeNumber, column);
      break;
  }
}

std::int32_t RowFields::date(int column) {
  if (!ok()) {
    return 0;
  }
  const std::string_view text = field(column);
  if (text.size() != 8 || !all_digits(text)) {
    fail(ErrorCode::kBadDate, column);
    return 0;
  }
  std::int32_t value = 0;
  for (const char c : text) {
    value = value * 10 + (c - '0');
  }
  const std::chrono::year_month_day ymd{
      std::chrono::year{value / 10000},
      std::chrono::month{static_cast<unsigned>(value / 100 % 100)},
      std::chrono::day{static_cast<unsigned>(value % 100)}};
  if (!ymd.ok()) {
    fail(ErrorCode::kBadDate, column);
    return 0;
  }
  return value;
}

std::string RowFields::contract(int column) {
  if (!ok()) {
    return {};
  }
  const std::string_view text = field(column);
  const bool valid =
      !text.empty() && text.size() <= kMaxContractLength &&
      std::ranges::all_of(text, [](char c) { return (c >= 'A' && c <= 'Z') || is_digit(c); });
  if (!valid) {
    fail(ErrorCode::kBadContract, column);
    return {};
  }
  return std::string(text);
}

std::string RowFields::expiry(int column, bool allow_spread) {
  if (!ok()) {
    return {};
  }
  const std::string_view text = field(column);
  const auto slash = text.find('/');
  const bool valid = slash == std::string_view::npos
                         ? is_single_expiry(text)
                         : allow_spread && is_single_expiry(text.substr(0, slash)) &&
                               is_single_expiry(text.substr(slash + 1));
  if (!valid) {
    fail(ErrorCode::kBadExpiry, column);
    return {};
  }
  return std::string(text);
}

Session RowFields::session(int column) {
  if (!ok()) {
    return Session::kRegular;
  }
  const std::string_view text = field(column);
  if (text == kRegularLabel) {
    return Session::kRegular;
  }
  if (text == kAfterHoursLabel) {
    return Session::kAfterHours;
  }
  fail(ErrorCode::kBadSession, column);
  return Session::kRegular;
}

double RowFields::strike(int column) {
  if (!ok()) {
    return 0.0;
  }
  const std::string_view text = field(column);
  if (is_missing(text)) {
    fail(ErrorCode::kMissingField, column);
    return 0.0;
  }
  const auto value = parse_decimal(text, SignPolicy::kNonNegative);
  if (!value) {
    fail_number(value.error(), column);
    return 0.0;
  }
  if (*value <= 0.0) {
    fail(ErrorCode::kMalformedNumber, column);
    return 0.0;
  }
  return *value;
}

std::optional<double> RowFields::number(int column, SignPolicy sign) {
  if (!ok() || is_missing(field(column))) {
    return std::nullopt;
  }
  const auto value = parse_decimal(field(column), sign);
  if (!value) {
    fail_number(value.error(), column);
    return std::nullopt;
  }
  return *value;
}

std::optional<double> RowFields::percent(int column) {
  if (!ok() || is_missing(field(column))) {
    return std::nullopt;
  }
  std::string_view text = field(column);
  if (text.back() != '%') {
    fail(ErrorCode::kMalformedNumber, column);
    return std::nullopt;
  }
  text.remove_suffix(1);
  const auto value = parse_decimal(text, SignPolicy::kSigned);
  if (!value) {
    fail_number(value.error(), column);
    return std::nullopt;
  }
  return *value;
}

std::optional<std::int64_t> RowFields::count(int column) {
  if (!ok() || is_missing(field(column))) {
    return std::nullopt;
  }
  const auto value = parse_count(field(column));
  if (!value) {
    fail_number(value.error(), column);
    return std::nullopt;
  }
  return *value;
}

std::expected<std::string, std::string> read_binary_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::unexpected("cannot open " + path.string());
  }
  std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) {
    return std::unexpected("cannot read " + path.string());
  }
  return bytes;
}

}  // namespace aadp::taifex
