// Per-batch latency. One batch is one timed span of work, and the rule that decides whether a
// histogram may exist is stated here once.
//
// A single tick of error is at most 1/kMinResolutionMultiple of a batch at the gate, so every
// published value is good to 10%, and to 1% at the size batches are normally grown to.
#pragma once

#include <hdr/hdr_histogram.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace aadp::bench {

// Never below this on any host: Linux batches anything under about 100 ns.
inline constexpr double kBatchFloorNs = 100.0;
inline constexpr double kMinResolutionMultiple = 10.0;
inline constexpr double kTargetResolutionMultiple = 100.0;

// The shortest batch that may enter a histogram, given the resolution measured on this run.
double min_batch_span_ns(double timer_resolution_ns);
double target_batch_span_ns(double timer_resolution_ns);

struct BatchSizing {
  std::uint64_t units = 0;
  double median_span_ns = 0;
};

// Doubles the units per batch until the median of a few timed batches reaches target_ns.
// `timed_run(units)` returns one batch's span in nanoseconds.
std::expected<BatchSizing, std::string> size_batch(
    const std::function<double(std::uint64_t)>& timed_run, double target_ns);

// The median span at a fixed size, for checking a size that was requested.
double measure_batch(const std::function<double(std::uint64_t)>& timed_run, std::uint64_t units);

struct PublishedValue {
  std::string label;
  std::optional<std::int64_t> ns;  // nullopt prints as `unresolved`
};

// A value below the gate is one the timer cannot resolve, so it is not published.
std::optional<std::int64_t> publishable(std::int64_t value_ns, double gate_ns);

class LatencyHistogram {
 public:
  // Refuses unless every batch reaches the gate. There is no partial histogram: one batch below
  // the gate means the sizing was wrong for this run.
  static std::expected<LatencyHistogram, std::string> from_batches(
      std::span<const std::int64_t> batch_ns, double gate_ns);

  std::vector<PublishedValue> values() const;
  std::int64_t count() const;
  double gate_ns() const { return gate_ns_; }

 private:
  struct Closer {
    void operator()(hdr_histogram* histogram) const { hdr_close(histogram); }
  };
  LatencyHistogram(hdr_histogram* histogram, double gate_ns)
      : histogram_(histogram), gate_ns_(gate_ns) {}

  std::unique_ptr<hdr_histogram, Closer> histogram_;
  double gate_ns_;
};

}  // namespace aadp::bench
