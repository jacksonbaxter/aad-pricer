#include "latency.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace aadp::bench {
namespace {

// One hour, at three significant figures.
constexpr std::int64_t kHighestNs = 3'600'000'000'000;
constexpr int kSignificantFigures = 3;
constexpr int kSizingTrials = 15;
constexpr std::uint64_t kMaxUnits = std::uint64_t{1} << 40;
constexpr std::array<double, 5> kPercentiles = {50.0, 90.0, 99.0, 99.9, 99.99};

}  // namespace

double min_batch_span_ns(double timer_resolution_ns) {
  return std::max(kBatchFloorNs, kMinResolutionMultiple * timer_resolution_ns);
}

double target_batch_span_ns(double timer_resolution_ns) {
  return std::max(kBatchFloorNs, kTargetResolutionMultiple * timer_resolution_ns);
}

double measure_batch(const std::function<double(std::uint64_t)>& timed_run, std::uint64_t units) {
  std::array<double, kSizingTrials> spans{};
  for (double& span : spans) {
    span = timed_run(units);
  }
  std::ranges::sort(spans);
  return spans[kSizingTrials / 2];
}

std::expected<BatchSizing, std::string> size_batch(
    const std::function<double(std::uint64_t)>& timed_run, double target_ns) {
  for (std::uint64_t units = 1; units <= kMaxUnits; units *= 2) {
    const double median = measure_batch(timed_run, units);
    if (median >= target_ns) {
      return BatchSizing{.units = units, .median_span_ns = median};
    }
  }
  return std::unexpected("no batch size reached the target span");
}

std::optional<std::int64_t> publishable(std::int64_t value_ns, double gate_ns) {
  if (static_cast<double>(value_ns) < gate_ns) {
    return std::nullopt;
  }
  return value_ns;
}

std::expected<LatencyHistogram, std::string> LatencyHistogram::from_batches(
    std::span<const std::int64_t> batch_ns, double gate_ns) {
  if (batch_ns.empty()) {
    return std::unexpected("no batches");
  }
  if (!(gate_ns > 0)) {
    return std::unexpected("no resolution gate");
  }
  const std::int64_t shortest = *std::ranges::min_element(batch_ns);
  if (static_cast<double>(shortest) < gate_ns) {
    std::array<char, 160> text{};
    std::snprintf(text.data(), text.size(),
                  "refusing to produce a histogram: a batch took %lld ns, below the %.1f ns the "
                  "timer can resolve on this run; batch more work",
                  static_cast<long long>(shortest), gate_ns);
    return std::unexpected(std::string(text.data()));
  }
  hdr_histogram* raw = nullptr;
  if (hdr_init(1, kHighestNs, kSignificantFigures, &raw) != 0) {
    return std::unexpected("hdr_init failed");
  }
  LatencyHistogram histogram(raw, gate_ns);
  for (const std::int64_t value : batch_ns) {
    if (!hdr_record_value(raw, value)) {
      return std::unexpected("a batch is outside the histogram's range");
    }
  }
  return histogram;
}

std::vector<PublishedValue> LatencyHistogram::values() const {
  const hdr_histogram* raw = histogram_.get();
  std::vector<PublishedValue> out;
  out.push_back({.label = "min", .ns = publishable(hdr_min(raw), gate_ns_)});
  for (const double percentile : kPercentiles) {
    std::array<char, 16> label{};
    std::snprintf(label.data(), label.size(), "p%g", percentile);
    out.push_back({.label = std::string(label.data()),
                   .ns = publishable(hdr_value_at_percentile(raw, percentile), gate_ns_)});
  }
  out.push_back({.label = "max", .ns = publishable(hdr_max(raw), gate_ns_)});
  return out;
}

std::int64_t LatencyHistogram::count() const { return hdr_total_count(histogram_.get()); }

}  // namespace aadp::bench
