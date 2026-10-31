#include "results/json_lite.hpp"

#include <algorithm>
#include <cstddef>

namespace aadp::results {
namespace {

constexpr int kMaxDepth = 32;

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  std::expected<JsonValue, std::string> parse_document() {
    JsonValue value;
    if (!parse_value(value, 0)) {
      return std::unexpected(error_);
    }
    skip_space();
    if (at_ != text_.size()) {
      return std::unexpected("trailing data after the JSON value");
    }
    return value;
  }

 private:
  bool fail(std::string_view message) {
    if (error_.empty()) {
      error_ = std::string(message) + " at byte " + std::to_string(at_);
    }
    return false;
  }

  void skip_space() {
    while (at_ < text_.size() &&
           (text_[at_] == ' ' || text_[at_] == '\n' || text_[at_] == '\t' || text_[at_] == '\r')) {
      ++at_;
    }
  }

  [[nodiscard]] bool peek(char c) const { return at_ < text_.size() && text_[at_] == c; }

  // NOLINTNEXTLINE(misc-no-recursion): parse_value stops at kMaxDepth.
  bool parse_value(JsonValue& out, int depth) {
    if (depth > kMaxDepth) {
      return fail("nesting too deep");
    }
    skip_space();
    if (at_ >= text_.size()) {
      return fail("unexpected end");
    }
    switch (text_[at_]) {
      case '{':
        return parse_object(out, depth);
      case '[':
        return parse_array(out, depth);
      case '"':
        out.kind = JsonValue::Kind::kString;
        return parse_string(out.text);
      default:
        out.kind = JsonValue::Kind::kScalar;
        return parse_scalar(out.text);
    }
  }

  // NOLINTNEXTLINE(misc-no-recursion): parse_value stops at kMaxDepth.
  bool parse_object(JsonValue& out, int depth) {
    out.kind = JsonValue::Kind::kObject;
    ++at_;
    skip_space();
    if (peek('}')) {
      ++at_;
      return true;
    }
    while (true) {
      skip_space();
      if (!peek('"')) {
        return fail("expected a key");
      }
      JsonMember member;
      if (!parse_string(member.key)) {
        return false;
      }
      if (member.key.contains('\\')) {
        return fail("escaped key");
      }
      skip_space();
      if (!peek(':')) {
        return fail("expected a colon");
      }
      ++at_;
      if (!parse_value(member.value, depth + 1)) {
        return false;
      }
      out.members.push_back(std::move(member));
      skip_space();
      if (peek(',')) {
        ++at_;
        continue;
      }
      if (peek('}')) {
        ++at_;
        return true;
      }
      return fail("expected a comma or a closing brace");
    }
  }

  // NOLINTNEXTLINE(misc-no-recursion): parse_value stops at kMaxDepth.
  bool parse_array(JsonValue& out, int depth) {
    out.kind = JsonValue::Kind::kArray;
    ++at_;
    skip_space();
    if (peek(']')) {
      ++at_;
      return true;
    }
    while (true) {
      JsonValue item;
      if (!parse_value(item, depth + 1)) {
        return false;
      }
      out.items.push_back(std::move(item));
      skip_space();
      if (peek(',')) {
        ++at_;
        continue;
      }
      if (peek(']')) {
        ++at_;
        return true;
      }
      return fail("expected a comma or a closing bracket");
    }
  }

  bool parse_string(std::string& out) {
    const std::size_t start = ++at_;
    while (at_ < text_.size()) {
      const char c = text_[at_];
      if (c == '"') {
        out = std::string(text_.substr(start, at_ - start));
        ++at_;
        return true;
      }
      if (static_cast<unsigned char>(c) < 0x20) {
        return fail("control character in a string");
      }
      if (c != '\\') {
        ++at_;
        continue;
      }
      if (at_ + 1 >= text_.size()) {
        return fail("unfinished escape");
      }
      const char escape = text_[at_ + 1];
      if (escape == 'u') {
        if (at_ + 5 >= text_.size() ||
            !std::all_of(text_.begin() + static_cast<std::ptrdiff_t>(at_ + 2),
                         text_.begin() + static_cast<std::ptrdiff_t>(at_ + 6), is_hex)) {
          return fail("bad unicode escape");
        }
        at_ += 6;
      } else if (std::string_view("\"\\/bfnrt").contains(escape)) {
        at_ += 2;
      } else {
        return fail("bad escape");
      }
    }
    return fail("unterminated string");
  }

  bool parse_scalar(std::string& out) {
    const std::size_t start = at_;
    for (const std::string_view literal :
         {std::string_view("true"), std::string_view("false"), std::string_view("null"),
          std::string_view("NaN"), std::string_view("Infinity"), std::string_view("-Infinity")}) {
      if (text_.substr(at_).starts_with(literal)) {
        at_ += literal.size();
        out = std::string(literal);
        return true;
      }
    }
    if (peek('-')) {
      ++at_;
    }
    if (peek('0')) {
      ++at_;
    } else if (!skip_digits()) {
      return fail("expected a value");
    }
    if (peek('.')) {
      ++at_;
      if (!skip_digits()) {
        return fail("bad fraction");
      }
    }
    if (peek('e') || peek('E')) {
      ++at_;
      if (peek('+') || peek('-')) {
        ++at_;
      }
      if (!skip_digits()) {
        return fail("bad exponent");
      }
    }
    out = std::string(text_.substr(start, at_ - start));
    return true;
  }

  bool skip_digits() {
    const std::size_t start = at_;
    while (at_ < text_.size() && is_digit(text_[at_])) {
      ++at_;
    }
    return at_ != start;
  }

  std::string_view text_;
  std::size_t at_ = 0;
  std::string error_;
};

// NOLINTNEXTLINE(misc-no-recursion): the parser caps the depth at kMaxDepth.
void write_value(const JsonValue& value, int indent, std::string& out) {
  const auto pad = [&out](int width) { out.append(static_cast<std::size_t>(width) * 2U, ' '); };
  switch (value.kind) {
    case JsonValue::Kind::kScalar:
      out += value.text;
      return;
    case JsonValue::Kind::kString:
      out += '"';
      out += value.text;
      out += '"';
      return;
    case JsonValue::Kind::kArray:
      if (value.items.empty()) {
        out += "[]";
        return;
      }
      out += "[\n";
      for (std::size_t i = 0; i < value.items.size(); ++i) {
        pad(indent + 1);
        write_value(value.items[i], indent + 1, out);
        out += i + 1 < value.items.size() ? ",\n" : "\n";
      }
      pad(indent);
      out += ']';
      return;
    case JsonValue::Kind::kObject:
      if (value.members.empty()) {
        out += "{}";
        return;
      }
      out += "{\n";
      for (std::size_t i = 0; i < value.members.size(); ++i) {
        pad(indent + 1);
        out += '"';
        out += value.members[i].key;
        out += "\": ";
        write_value(value.members[i].value, indent + 1, out);
        out += i + 1 < value.members.size() ? ",\n" : "\n";
      }
      pad(indent);
      out += '}';
      return;
  }
}

}  // namespace

std::expected<JsonValue, std::string> parse_json(std::string_view text) {
  return Parser(text).parse_document();
}

// NOLINTNEXTLINE(misc-no-recursion): the parser caps the depth at kMaxDepth.
void drop_json_keys(JsonValue& value, std::span<const std::string_view> keys) {
  std::erase_if(value.members, [keys](const JsonMember& member) {
    return std::ranges::find(keys, std::string_view(member.key)) != keys.end();
  });
  for (JsonMember& member : value.members) {
    drop_json_keys(member.value, keys);
  }
  for (JsonValue& item : value.items) {
    drop_json_keys(item, keys);
  }
}

std::string write_json(const JsonValue& value) {
  std::string out;
  write_value(value, 0, out);
  out += '\n';
  return out;
}

const JsonValue* find_member(const JsonValue& object, std::string_view key) {
  for (const JsonMember& member : object.members) {
    if (member.key == key) {
      return &member.value;
    }
  }
  return nullptr;
}

}  // namespace aadp::results
