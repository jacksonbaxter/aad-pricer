// Check 5: tape bytes per path and peak tape memory on every product at 50 and 252 steps, against
// BENCHMARKS.md MEM-1 to MEM-4. Accuracy and memory data, not a timed benchmark.
//
// Expectations, stated before measuring:
//   MEM-1  16-byte Real, 1-byte statement, 12-byte operand: static_asserts in src/ad/tape.hpp.
//   MEM-2  tape bytes per statement are exactly 1 + 12 k, k the mean operand count: the layout
//          stores nothing else. The adjoint vector, 8 bytes per statement, is reported beside it.
//   counts one node per operation and no operand for a constant, so per path, with m steps,
//          the largest path records: European 3 m + 3 statements and 5 m + 4 operands,
//          arithmetic Asian 5 m + 3 and 8 m + 3, geometric Asian 4 m + 4 and 7 m + 4, one more
//          operand for either Asian with S0 in the average (per step: the drift, sd z with z a
//          constant, the sum; then exp, the average, the strike and the discount).
//   bound  the tape never holds more than the set-up and one path: after many paths the position
//          is the mark and the allocated bytes are what the first path needed.
//   MEM-4  every path's tape is below 50 MB.
//   MEM-3  peak tape memory at 252 steps is below 1.9 MB. That is NAG's whole-process peak with
//          n = 10 inputs and 360 steps; ours is the tape alone, with n = 5, so it is a loose
//          comparison, stated as such. Process peak RSS is printed for context only.
#include <gtest/gtest.h>
#include <sys/resource.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "ad/gradient.hpp"
#include "ad/tape.hpp"
#include "cases.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"

namespace aadp::ad {
namespace {

// Development seed, never 1729 or 271828.
constexpr std::uint64_t kSeed = 20261113;
constexpr std::uint64_t kPaths = 200;
constexpr double kMem3Bytes = 1.9e6;
constexpr double kMem4Bytes = 50e6;

struct Row {
  std::size_t setup_statements = 0;
  std::size_t statements = 0;  // per path, the largest seen
  std::size_t operands = 0;
  std::size_t allocated_first = 0;
  std::size_t allocated_last = 0;
};

Row Measure(const testing::Case& c) {
  Row row;
  PathGradient path(c.contract, testing::kMarket);
  Tape& tape = path.tape();
  const MemoryReport setup = tape.Memory();
  row.setup_statements = setup.statements;
  std::vector<double> normal(c.contract.steps);
  std::vector<double> crossing(c.contract.steps);
  const pricer::PathUniforms u{.normal = normal, .crossing = crossing};
  for (std::uint64_t p = 0; p < kPaths; ++p) {
    pricer::FillPath(kSeed, p, normal, crossing);
    const Real payoff = path.Record(u);
    const MemoryReport m = tape.Memory();
    row.statements = std::max(row.statements, m.statements - setup.statements);
    row.operands = std::max(row.operands, m.operands - setup.operands);
    tape.AddAdjoint(payoff, 1.0);
    tape.SweepBack(tape.GetPosition(), path.mark());
    path.Rewind();
    EXPECT_EQ(tape.GetPosition(), path.mark()) << c.name;
    if (p == 0) row.allocated_first = tape.Memory().allocated_bytes;
  }
  row.allocated_last = tape.Memory().allocated_bytes;
  static_cast<void>(path.SweepSetup());
  return row;
}

TEST(TapeMemory, BytesPerPathAndPeakAgainstMem1ToMem4) {
  std::printf("MEM-1: sizeof(Real) %zu, statement %zu, operand %zu (partial %zu + index %zu)\n",
              sizeof(Real), kStatementBytes, kOperandBytes, sizeof(double), sizeof(Index));
  std::printf("%-40s %5s %6s %8s %8s %6s %9s %10s %10s %11s %10s\n", "product", "steps", "setup",
              "stmts", "operands", "k", "B/stmt", "tape B", "adjoint B", "B/path", "allocated");
  double worst_path = 0.0;
  double worst_allocated = 0.0;
  for (const std::uint32_t steps : {50U, 252U}) {
    for (const testing::Case& c : testing::AllCases(steps)) {
      const Row row = Measure(c);
      const double k = static_cast<double>(row.operands) / static_cast<double>(row.statements);
      const std::size_t tape_bytes =
          (row.statements * kStatementBytes) + (row.operands * kOperandBytes);
      const double per_statement =
          static_cast<double>(tape_bytes) / static_cast<double>(row.statements);
      const std::size_t adjoint_bytes = row.statements * sizeof(double);
      const std::size_t path_bytes = tape_bytes + adjoint_bytes;
      std::printf("%-40s %5u %6zu %8zu %8zu %6.3f %9.3f %10zu %10zu %11zu %10zu\n", c.name.c_str(),
                  steps, row.setup_statements, row.statements, row.operands, k, per_statement,
                  tape_bytes, adjoint_bytes, path_bytes, row.allocated_last);
      // MEM-2: 1 + 12 k, to the rounding of the two divisions.
      EXPECT_NEAR(per_statement, 1.0 + (12.0 * k), 1e-12) << c.name;
      EXPECT_EQ(row.allocated_last, row.allocated_first) << c.name << ": tape grew across paths";
      worst_path = std::max(worst_path, static_cast<double>(path_bytes));
      worst_allocated = std::max(worst_allocated, static_cast<double>(row.allocated_last));
      const std::size_t m = steps;
      const std::size_t spot = c.contract.include_spot ? 1 : 0;
      if (c.contract.product == pricer::Product::kEuropean) {
        EXPECT_EQ(row.statements, (3 * m) + 3) << c.name;
        EXPECT_EQ(row.operands, (5 * m) + 4) << c.name;
      }
      if (c.contract.product == pricer::Product::kAsianArithmetic) {
        EXPECT_EQ(row.statements, (5 * m) + 3) << c.name;
        EXPECT_EQ(row.operands, (8 * m) + 3 + spot) << c.name;
      }
      if (c.contract.product == pricer::Product::kAsianGeometric) {
        EXPECT_EQ(row.statements, (4 * m) + 4) << c.name;
        EXPECT_EQ(row.operands, (7 * m) + 4 + spot) << c.name;
      }
    }
  }
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  std::printf(
      "measured: largest path %.0f B against MEM-4 %.0f B; peak tape memory %.0f B against "
      "MEM-3 %.0f B (loose: see the header); process peak RSS %ld B, context only\n",
      worst_path, kMem4Bytes, worst_allocated, kMem3Bytes, usage.ru_maxrss);
  EXPECT_LT(worst_path, kMem4Bytes);
  EXPECT_LT(worst_allocated, kMem3Bytes);
}

}  // namespace
}  // namespace aadp::ad
