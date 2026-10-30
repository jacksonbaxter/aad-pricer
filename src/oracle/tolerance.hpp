// The tolerance rule, BENCHMARKS.md section 10, and the only implementation of it.
//
// A comparison passes only if both conditions hold:
//   1. |xbar - ref| <= t(1 - alpha_F / (2K), B - 1) * SE + b
//   2. SE <= a ceiling fixed per comparison before the run
// SE comes from B batch means, b is the reference or finite-difference truncation allowance,
// K is the number of comparisons in the suite and alpha_F the family-wise error rate.
#ifndef AADP_ORACLE_TOLERANCE_HPP_
#define AADP_ORACLE_TOLERANCE_HPP_

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <vector>

#include "oracle/student_t.hpp"

namespace aadp {

inline constexpr double kFamilyWiseAlpha = 0.01;

// NaN unless batches >= 2 and comparisons >= 1.
inline double CriticalValue(double alpha_f, std::size_t comparisons, std::size_t batches) {
  if (batches < 2 || comparisons < 1 || !(alpha_f > 0.0 && alpha_f < 1.0)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return StudentTQuantile(1.0 - alpha_f / (2.0 * static_cast<double>(comparisons)),
                          static_cast<double>(batches - 1));
}

struct BatchStatistics {
  double mean = std::numeric_limits<double>::quiet_NaN();
  double standard_error = std::numeric_limits<double>::quiet_NaN();
};

inline BatchStatistics BatchMeansStatistics(std::span<const double> batch_means) {
  BatchStatistics stats;
  const std::size_t count = batch_means.size();
  if (count < 2) return stats;
  double sum = 0.0;
  for (const double x : batch_means) sum += x;
  const double b = static_cast<double>(count);
  const double mean = sum / b;
  double squares = 0.0;
  for (const double x : batch_means) squares += (x - mean) * (x - mean);
  stats.mean = mean;
  stats.standard_error = std::sqrt(squares / (b - 1.0) / b);
  return stats;
}

struct ToleranceLimits {
  double b = 0.0;
  double se_ceiling = 0.0;
  double critical = 0.0;
};

struct ToleranceResult {
  double estimate = 0.0;
  double reference = 0.0;
  double standard_error = 0.0;
  double bound = 0.0;  // Right-hand side of condition 1.
  double se_ceiling = 0.0;
  bool within_bound = false;
  bool within_ceiling = false;

  constexpr bool passed() const { return within_bound && within_ceiling; }
  // Which condition failed, for the one-line report.
  constexpr const char* failed_condition() const {
    if (passed()) return "none";
    if (!within_bound && !within_ceiling) return "both";
    return within_bound ? "se-ceiling" : "distance";
  }
};

// Every comparison below is false on NaN, so a NaN anywhere fails rather than passes.
inline ToleranceResult ApplyTolerance(const BatchStatistics& stats, double reference,
                                      const ToleranceLimits& limits) {
  ToleranceResult result;
  result.estimate = stats.mean;
  result.reference = reference;
  result.standard_error = stats.standard_error;
  result.bound = limits.critical * stats.standard_error + limits.b;
  result.se_ceiling = limits.se_ceiling;
  result.within_bound = std::fabs(stats.mean - reference) <= result.bound;
  result.within_ceiling = stats.standard_error <= limits.se_ceiling;
  return result;
}

// Against a fixed reference.
inline ToleranceResult CheckAgainstReference(std::span<const double> batch_means, double reference,
                                             const ToleranceLimits& limits) {
  return ApplyTolerance(BatchMeansStatistics(batch_means), reference, limits);
}

// Against a reference estimated from the same paths, batch by batch, so the SE is that of the
// paired difference. Reported estimate and reference are the two means.
inline ToleranceResult CheckPaired(std::span<const double> batch_means,
                                   std::span<const double> reference_batch_means,
                                   const ToleranceLimits& limits) {
  const std::size_t count = std::min(batch_means.size(), reference_batch_means.size());
  std::vector<double> differences(count);
  for (std::size_t i = 0; i < count; ++i) {
    differences[i] = batch_means[i] - reference_batch_means[i];
  }
  ToleranceResult result = ApplyTolerance(BatchMeansStatistics(differences), 0.0, limits);
  if (batch_means.size() != reference_batch_means.size()) {
    result.within_bound = false;
    result.within_ceiling = false;
  }
  const BatchStatistics estimate = BatchMeansStatistics(batch_means);
  const BatchStatistics reference = BatchMeansStatistics(reference_batch_means);
  result.estimate = estimate.mean;
  result.reference = reference.mean;
  return result;
}

}  // namespace aadp

#endif  // AADP_ORACLE_TOLERANCE_HPP_
