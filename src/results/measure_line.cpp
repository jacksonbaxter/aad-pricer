#include "results/measure_line.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace aadp::results {
namespace {

constexpr std::string_view kStart = "measure.sh: start";
constexpr std::string_view kEnd = "measure.sh: end";
constexpr std::size_t kMaxLoadChars = 32;
constexpr int kMaxCpu = 4096;

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }

bool all_digits(std::string_view text) {
  return !text.empty() && std::ranges::all_of(text, is_digit);
}

// Digits, one dot, digits. Everything strtod would also take, such as inf or hex, is out.
bool valid_load(std::string_view text) {
  if (text.empty() || text.size() > kMaxLoadChars) {
    return false;
  }
  const auto dot = text.find('.');
  if (dot == std::string_view::npos) {
    return all_digits(text);
  }
  return all_digits(text.substr(0, dot)) && all_digits(text.substr(dot + 1));
}

int two_digits(std::string_view text, std::size_t at) {
  return (text[at] - '0') * 10 + (text[at + 1] - '0');
}

// date -u +%FT%TZ. A fixed format, so lexical order is time order, which the stamp relies on.
bool valid_timestamp(std::string_view text) {
  constexpr std::string_view kShape = "dddd-dd-ddTdd:dd:ddZ";
  if (text.size() != kShape.size()) {
    return false;
  }
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (kShape[i] == 'd' ? !is_digit(text[i]) : text[i] != kShape[i]) {
      return false;
    }
  }
  const int month = two_digits(text, 5);
  const int day = two_digits(text, 8);
  return month >= 1 && month <= 12 && day >= 1 && day <= 31 && two_digits(text, 11) <= 23 &&
         two_digits(text, 14) <= 59 && two_digits(text, 17) <= 60;
}

bool valid_hostname(std::string_view text) {
  return !text.empty() && std::ranges::all_of(text, [](char c) {
    return is_digit(c) || is_lower(c) || is_upper(c) || c == '-' || c == '.' || c == '_';
  });
}

bool valid_cpus(std::string_view text) {
  return std::ranges::all_of(text, [](char c) { return is_digit(c) || c == ',' || c == '-'; });
}

bool valid_governor(std::string_view text) {
  return std::ranges::all_of(
      text, [](char c) { return is_digit(c) || is_lower(c) || c == '_' || c == ','; });
}

bool valid_freq(std::string_view text) {
  return std::ranges::all_of(
      text, [](char c) { return is_digit(c) || is_lower(c) || c == ':' || c == ','; });
}

bool valid_paranoid(std::string_view text) {
  if (text.starts_with('-')) {
    text.remove_prefix(1);
  }
  return all_digits(text);
}

std::optional<MeasureKind> kind_of(std::string_view line) {
  const auto matches = [line](std::string_view prefix) {
    return line == prefix || (line.starts_with(prefix) && line[prefix.size()] == ' ');
  };
  if (matches(kStart)) {
    return MeasureKind::kStart;
  }
  if (matches(kEnd)) {
    return MeasureKind::kEnd;
  }
  return std::nullopt;
}

std::string_view strip_cr(std::string_view line) {
  if (line.ends_with('\r')) {
    line.remove_suffix(1);
  }
  return line;
}

}  // namespace

std::expected<MeasureLine, std::string> parse_measure_line(std::string_view raw_line) {
  const std::string_view line = strip_cr(raw_line);
  const auto kind = kind_of(line);
  if (!kind) {
    return std::unexpected("not a measure.sh start or end line");
  }
  MeasureLine out;
  out.kind = *kind;

  std::string_view rest = line.substr((*kind == MeasureKind::kStart ? kStart : kEnd).size());
  std::array<bool, 8> seen{};
  const auto once = [&seen](std::size_t slot) { return !std::exchange(seen[slot], true); };

  while (!rest.empty()) {
    const auto space = rest.find(' ');
    const std::string_view token = rest.substr(0, space);
    rest = space == std::string_view::npos ? std::string_view{} : rest.substr(space + 1);
    if (token.empty()) {
      continue;
    }
    const auto equals = token.find('=');
    if (equals == std::string_view::npos || equals == 0) {
      return std::unexpected("expected key=value");
    }
    const std::string_view key = token.substr(0, equals);
    const std::string_view value = token.substr(equals + 1);

    if (key == "load1") {
      if (!once(0)) {
        return std::unexpected("load1 repeated");
      }
      if (!valid_load(value)) {
        return std::unexpected("load1 is not a number");
      }
      const std::string copy(value);
      out.load1 = copy;
      out.load1_value = std::strtod(copy.c_str(), nullptr);
      if (!std::isfinite(out.load1_value)) {
        return std::unexpected("load1 is out of range");
      }
    } else if (key == "at") {
      if (!once(1)) {
        return std::unexpected("at repeated");
      }
      if (!valid_timestamp(value)) {
        return std::unexpected("at is not a UTC timestamp");
      }
      out.at = std::string(value);
    } else if (key == "exit") {
      if (!once(2)) {
        return std::unexpected("exit repeated");
      }
      if (*kind != MeasureKind::kEnd) {
        return std::unexpected("exit on a start line");
      }
      int status = 0;
      const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), status);
      if (!all_digits(value) || error != std::errc{} || end != value.data() + value.size() ||
          status > 255) {
        return std::unexpected("exit is not a status from 0 to 255");
      }
      out.exit_status = status;
    } else if (key == "host") {
      if (!once(3)) {
        return std::unexpected("host repeated");
      }
      if (*kind != MeasureKind::kStart) {
        return std::unexpected("host on an end line");
      }
      if (!valid_hostname(value)) {
        return std::unexpected("host is empty or malformed");
      }
      out.host = std::string(value);
    } else if (key == "cpus" || key == "governor" || key == "freq_khz" ||
               key == "perf_event_paranoid") {
      const std::size_t slot = key == "cpus"       ? 4U
                               : key == "governor" ? 5U
                               : key == "freq_khz" ? 6U
                                                   : 7U;
      if (!once(slot)) {
        return std::unexpected(std::string(key) + " repeated");
      }
      // Empty means measure.sh could not read it, as when /proc lacks the file.
      if (value.empty()) {
        continue;
      }
      const bool ok = slot == 4U   ? valid_cpus(value)
                      : slot == 5U ? valid_governor(value)
                      : slot == 6U ? valid_freq(value)
                                   : valid_paranoid(value);
      if (!ok) {
        return std::unexpected(std::string(key) + " is malformed");
      }
      auto& field = slot == 4U   ? out.cpus
                    : slot == 5U ? out.governor
                    : slot == 6U ? out.freq_khz
                                 : out.perf_event_paranoid;
      field = std::string(value);
    }
    // Unknown keys are skipped and never stored, so they cannot leak.
  }

  if (!seen[0]) {
    return std::unexpected("no load1");
  }
  if (!seen[1]) {
    return std::unexpected("no at");
  }
  if (*kind == MeasureKind::kEnd && !out.exit_status) {
    return std::unexpected("no exit");
  }
  return out;
}

std::string sanitized_measure_line(const MeasureLine& line) {
  std::string out(line.kind == MeasureKind::kStart ? kStart : kEnd);
  out += " load1=" + line.load1;
  if (line.exit_status) {
    out += " exit=" + std::to_string(*line.exit_status);
  }
  out += " at=" + line.at;
  const auto add = [&out](std::string_view key, const std::optional<std::string>& value) {
    if (value) {
      out += ' ';
      out += key;
      out += '=';
      out += *value;
    }
  };
  add("cpus", line.cpus);
  add("governor", line.governor);
  add("freq_khz", line.freq_khz);
  add("perf_event_paranoid", line.perf_event_paranoid);
  return out;
}

MeasureScan scan_measure_stderr(std::string_view captured) {
  MeasureScan scan;
  while (!captured.empty()) {
    const auto end = captured.find('\n');
    const std::string_view line = strip_cr(captured.substr(0, end));
    captured = end == std::string_view::npos ? std::string_view{} : captured.substr(end + 1);
    if (!kind_of(line)) {
      ++scan.other_lines;
      continue;
    }
    auto parsed = parse_measure_line(line);
    if (!parsed) {
      scan.malformed.push_back(parsed.error());
    } else if (parsed->kind == MeasureKind::kStart) {
      scan.starts.push_back(std::move(*parsed));
    } else {
      scan.ends.push_back(std::move(*parsed));
    }
  }
  return scan;
}

std::expected<std::vector<int>, std::string> expand_cpu_list(std::string_view list) {
  std::vector<int> cpus;
  if (list.empty()) {
    return std::unexpected("empty CPU list");
  }
  const auto number = [](std::string_view text) -> std::optional<int> {
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (!all_digits(text) || error != std::errc{} || end != text.data() + text.size() ||
        value >= kMaxCpu) {
      return std::nullopt;
    }
    return value;
  };
  while (true) {
    const auto comma = list.find(',');
    const std::string_view part = list.substr(0, comma);
    const auto dash = part.find('-');
    const auto first = number(part.substr(0, dash));
    const auto last = dash == std::string_view::npos ? first : number(part.substr(dash + 1));
    if (!first || !last || *last < *first ||
        cpus.size() + static_cast<std::size_t>(*last - *first) >= kMaxCpu) {
      return std::unexpected("malformed CPU list");
    }
    for (int cpu = *first; cpu <= *last; ++cpu) {
      cpus.push_back(cpu);
    }
    if (comma == std::string_view::npos) {
      return cpus;
    }
    list.remove_prefix(comma + 1);
  }
}

}  // namespace aadp::results
