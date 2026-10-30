#ifndef AADP_FUZZ_FUZZ_TAIFEX_CHECK_HPP_
#define AADP_FUZZ_FUZZ_TAIFEX_CHECK_HPP_

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <vector>

#include "data/taifex_csv.hpp"

// A failed property aborts, so libFuzzer keeps the input.
#define AADP_FUZZ_CHECK(condition)                                                           \
  do {                                                                                       \
    if (!(condition)) {                                                                      \
      std::fprintf(stderr, "property failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); \
      std::abort();                                                                          \
    }                                                                                        \
  } while (false)

namespace aadp::fuzz {

// Line boundaries from a plain scan for LF, independent of RecordReader. LF is below every trail
// byte, so this agrees with a decoded walk on any input, valid or not.
class LineIndex {
 public:
  explicit LineIndex(std::string_view input) : input_(input) {
    begins_.push_back(0);
    for (std::size_t i = 0; i < input.size(); ++i) {
      if (input[i] == '\n') {
        begins_.push_back(i + 1);
      }
    }
    // Input that ends on LF has no further line. An empty input has none at all.
    if (begins_.back() == input.size()) {
      begins_.pop_back();
    }
  }

  std::size_t count() const { return begins_.size(); }

  // Half-open byte range of a 1-based line, with its terminator.
  std::size_t begin(std::size_t line) const { return begins_[line - 1]; }
  std::size_t end(std::size_t line) const {
    return line == count() ? input_.size() : begins_[line];
  }

  // The line without its terminator.
  std::string_view text(std::size_t line) const {
    std::string_view text = input_.substr(begin(line), end(line) - begin(line));
    if (!text.empty() && text.back() == '\n') {
      text.remove_suffix(1);
    }
    if (!text.empty() && text.back() == '\r') {
      text.remove_suffix(1);
    }
    return text;
  }

 private:
  std::string_view input_;
  std::vector<std::size_t> begins_;
};

// Comma is below every trail byte, so a raw split is a fair model of an accepted line.
inline std::vector<std::string_view> split_commas(std::string_view line) {
  std::vector<std::string_view> fields;
  std::size_t begin = 0;
  while (true) {
    const std::size_t comma = line.find(',', begin);
    if (comma == std::string_view::npos) {
      fields.push_back(line.substr(begin));
      return fields;
    }
    fields.push_back(line.substr(begin, comma - begin));
    begin = comma + 1;
  }
}

inline void check_finite(const std::optional<double>& value) {
  AADP_FUZZ_CHECK(!value || std::isfinite(*value));
}

inline void check_non_negative(const std::optional<double>& value) {
  AADP_FUZZ_CHECK(!value || (std::isfinite(*value) && *value >= 0.0));
}

inline void check_count(const std::optional<std::int64_t>& value) {
  AADP_FUZZ_CHECK(!value || *value >= 0);
}

inline void check_missing_matches(std::string_view raw, bool has_value) {
  AADP_FUZZ_CHECK(has_value == !(raw.empty() || raw == "-"));
}

inline void check_contract(std::string_view contract, std::string_view raw) {
  AADP_FUZZ_CHECK(contract == raw);
  AADP_FUZZ_CHECK(!contract.empty() && contract.size() <= 8);
  for (const char c : contract) {
    AADP_FUZZ_CHECK((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'));
  }
}

inline void check_date(std::int32_t date, std::string_view raw) {
  AADP_FUZZ_CHECK(raw.size() == 8);
  std::int32_t value = 0;
  for (const char c : raw) {
    AADP_FUZZ_CHECK(c >= '0' && c <= '9');
    value = value * 10 + (c - '0');
  }
  AADP_FUZZ_CHECK(date == value);
  AADP_FUZZ_CHECK(date % 100 >= 1 && date % 100 <= 31);
  AADP_FUZZ_CHECK(date / 100 % 100 >= 1 && date / 100 % 100 <= 12);
}

// The session must come from the bytes of the row's own last-but-n column.
inline void check_session(taifex::Session session, std::string_view raw) {
  constexpr std::string_view kRegular = "\xA4\x40\xAF\xEB";
  constexpr std::string_view kAfterHours = "\xBD\x4C\xAB\xE1";
  AADP_FUZZ_CHECK(session == taifex::Session::kRegular || session == taifex::Session::kAfterHours);
  AADP_FUZZ_CHECK(raw == (session == taifex::Session::kRegular ? kRegular : kAfterHours));
}

// Everything that holds for any successful read, whichever file it is. Each line after the header
// gives exactly one row or one error, in order, and every error lies on its own line.
template <typename Rows>
void check_accounting(const LineIndex& lines, const Rows& rows,
                      const std::vector<taifex::ReadError>& errors) {
  AADP_FUZZ_CHECK(lines.count() >= 1);
  AADP_FUZZ_CHECK(rows.size() + errors.size() == lines.count() - 1);
  std::vector<char> seen(lines.count() + 1, 0);
  std::size_t previous = 1;
  for (const auto& row : rows) {
    AADP_FUZZ_CHECK(row.line > previous && row.line <= lines.count());
    previous = row.line;
    seen[row.line] = 1;
  }
  previous = 1;
  for (const taifex::ReadError& error : errors) {
    AADP_FUZZ_CHECK(error.line > previous && error.line <= lines.count());
    AADP_FUZZ_CHECK(seen[error.line] == 0);
    AADP_FUZZ_CHECK(error.position >= lines.begin(error.line));
    AADP_FUZZ_CHECK(error.position < lines.end(error.line));
    AADP_FUZZ_CHECK(!taifex::to_string(error.code).empty());
    previous = error.line;
  }
}

inline void check_header_error(const taifex::ReadError& error, std::string_view input) {
  if (input.empty()) {
    AADP_FUZZ_CHECK(error.code == taifex::ErrorCode::kEmptyInput);
    AADP_FUZZ_CHECK(error.position == 0);
    return;
  }
  AADP_FUZZ_CHECK(error.line == 1);
  AADP_FUZZ_CHECK(error.position < input.size());
}

}  // namespace aadp::fuzz

#endif  // AADP_FUZZ_FUZZ_TAIFEX_CHECK_HPP_
