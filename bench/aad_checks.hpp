// Verification inside the benchmark run (EVIDENCE B33): before any timing counts, every variant of
// a workload runs once at the benchmark's own size, and its results are checked. A failed check
// marks the workload invalid in the results file (`valid no`, and the header's gradient_check),
// and the table renders its rows as invalid.
//
// The checks, each with its bound stated here, before it ran:
//   value-bits       every variant's value batch means are Price()'s, bit for bit: they run the
//                    same double kernel on the same paths and sum in the same order.
//   skip-equals-tape tape-skip's Greeks equal the tape's in value (==, so +0 and -0 match): a
//                    skipped statement has a zero adjoint, so it would only have added zeros.
//   hand-vs-tape     the hand adjoint against the tape. Pricer: batch by batch, within the
//                    association bound of tests/ad/bounds.hpp: both form the same partial
//                    products and differ only in how they add them (src/ad/hand_adjoint.hpp;
//                    tests/ad/dual_agreement_test.cpp, check 8). Per path,
//                    2 gamma_m A (1 + 2 gamma_m) + 2^-1074 W (1 + 2 gamma_m), with A the absolute
//                    sweep of the path, W its operand weight with every input's tangent seeded at
//                    once (which bounds each input's own W, all terms being absolute), and m
//                    counting the path's statements, operands and the block and batch sums.
//                    Lax-Wendroff: input by input, within 2 kTimeSteps gamma_8 4 ||w||_2, a
//                    first-order stability scale, since there the absolute sweep overflows
//                    (bench/lax_wendroff.hpp).
//   central-vs-tape  bump-central against the tape, per Greek, by the tolerance rule of
//   forward-vs-tape  src/oracle/tolerance.hpp on the batch differences d = bump - tape:
//                    |mean d| <= t(1 - alpha_F / (2K), B - 1) SE(d) + b, SE(d) <= SE(tape).
//                    b = truncation + t bias_se + R: the study's bound on the bias at the step,
//                    widened by its SE, and R this run's round-off scale at the step
//                    (bump_study.hpp, CheckAllowance). The ceiling: under common random numbers
//                    d is zero on every path that does not cross a kink within the step, so its
//                    SE is a small part of the Greek's own; one as large as the Greek's means the
//                    bump is not on common random numbers. K = kStatisticalFamily, every such
//                    comparison of the suite, so the family is the same however many workloads
//                    one run selects. Lax-Wendroff is deterministic: |d| <= b, input by input.
#ifndef AADP_BENCH_AAD_CHECKS_HPP_
#define AADP_BENCH_AAD_CHECKS_HPP_

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "aad_workloads.hpp"
#include "pricer/runner.hpp"

namespace aadp::bench::aad {

// Five pricer workloads, two schemes, five Greeks.
inline constexpr std::size_t kStatisticalFamily = 50;
double CheckCriticalValue();

struct CheckResult {
  std::string name;
  bool passed = false;
  double worst = 0.0;  // Largest distance over its bound, 0 for an exact check that held
  std::string detail;  // What failed first, or empty
};

struct WorkloadReport {
  const Workload* workload = nullptr;
  pricer::RunConfig config{};
  TapeShape tape;
  double nonzero_fraction = 0.0;
  double rounding_count = 0.0;
  double critical = 0.0;
  std::vector<VariantResult> results;  // One per variant the workload has, in kVariants order
  std::vector<CheckResult> checks;
  std::vector<std::pair<std::string, std::uint64_t>> peak_rss;  // Variant or "idle", bytes

  [[nodiscard]] bool valid() const;
  [[nodiscard]] const VariantResult* Result(Variant variant) const;
};

// Runs every variant once and checks them, as above.
WorkloadReport Verify(const Workload& workload, const pricer::RunConfig& config);

// The checks alone, on results already computed: what Verify does after running them. Exposed so
// that a test can hand it a broken result.
std::vector<CheckResult> CheckResults(const WorkloadReport& report);

// The body of the workload's results section, `aad_<id>`: one `key value` line per field,
// read by src/results/aad_table.cpp.
std::string RenderSection(const WorkloadReport& report);

// The benchmark name of a variant, `<workload>.<variant>`, which names its histogram section.
std::string BenchmarkName(const Workload& workload, Variant variant);

}  // namespace aadp::bench::aad

#endif  // AADP_BENCH_AAD_CHECKS_HPP_
