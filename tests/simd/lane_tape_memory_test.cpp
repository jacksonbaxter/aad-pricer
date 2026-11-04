// Tape bytes per path on the vector tape at every lane width this build can run, beside the
// scalar tape's, on every product and treatment at 50 and 252 steps (tests/ad/memory_test.cpp's
// sizes). Memory data, not a timed benchmark. Bytes per path count what the harness's TapeShape
// counts (bench/aad_workloads.cpp, MeasureTape): statement and operand bytes of the largest group,
// over its lanes; the adjoints apart.
//
// Expectations, stated before measuring:
//   layout   operand bytes are 12 at one lane and 5 + 8 N above it (lane_tape.hpp, NOTES.md);
//   union    a group records the union of its lanes' statements and operands, so it holds at
//            least as many as its largest lane does on the scalar tape;
//   bound    the tape never holds more than one group: after many groups the allocated bytes are
//            what the first group needed;
//   MEM-4    every group's tape is below 50 MB.
#include <gtest/gtest.h>
#include <hwy/highway.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "ad/cases.hpp"
#include "ad/gradient.hpp"
#include "ad/tape.hpp"
#include "lane_widths.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/kernels.hpp"
#include "simd/lane_kernels.hpp"
#include "simd/lane_path_gradient.hpp"
#include "simd/lane_real.hpp"
#include "simd/lane_runner.hpp"
#include "simd/lane_tape.hpp"

namespace aadp::simd {
namespace {

namespace hn = hwy::HWY_NAMESPACE;

// Development seed, never 1729 or 271828.
constexpr std::uint64_t kSeed = 20261125;
constexpr std::uint64_t kGroups = 64;
constexpr double kMem4Bytes = 50e6;

struct Row {
  std::size_t statements = 0;  // per group, the largest seen
  std::size_t operands = 0;
  std::size_t boundary = 0;
  std::size_t scalar_statements = 0;  // per path on the scalar tape, the largest seen
  std::size_t scalar_operands = 0;
  std::size_t allocated_first = 0;
  std::size_t allocated_last = 0;
  bool union_holds = true;
};

template <class D>
Row Measure(const pricer::Contract& c) {
  constexpr std::size_t kLanes = hn::MaxLanes(D());
  Row row;
  std::vector<std::size_t> scalar_statements(kGroups * kLanes);
  std::vector<std::size_t> scalar_operands(kGroups * kLanes);
  {
    ad::PathGradient path(c, ad::testing::kMarket);
    const ad::MemoryReport setup = path.tape().Memory();
    std::vector<double> normal(c.steps);
    std::vector<double> crossing(c.steps);
    for (std::uint64_t p = 0; p < kGroups * kLanes; ++p) {
      pricer::FillPath(kSeed, p, normal, crossing);
      static_cast<void>(path.Record({.normal = normal, .crossing = crossing}));
      const ad::MemoryReport m = path.tape().Memory();
      scalar_statements[p] = m.statements - setup.statements;
      scalar_operands[p] = m.operands - setup.operands;
      path.Rewind();
    }
  }
  row.scalar_statements = *std::ranges::max_element(scalar_statements);
  row.scalar_operands = *std::ranges::max_element(scalar_operands);

  LanePathGradient<D> lanes(c, ad::testing::kMarket);
  LaneTape<D>& tape = lanes.tape();
  std::vector<LaneReal<D>> normal(c.steps);
  std::vector<LaneReal<D>> crossing(pricer::NeedsCrossingUniforms(c) ? c.steps : 0);
  const LaneUniforms<LaneReal<D>> u{.normal = normal, .crossing = crossing};
  for (std::uint64_t g = 0; g < kGroups; ++g) {
    FillLanes(D(), kSeed, g * kLanes, std::span(normal), std::span(crossing));
    const LaneTapeReal<D> payoff = lanes.Record(u);
    const ad::MemoryReport m = tape.Memory();
    std::size_t lane_statements = 0;
    std::size_t lane_operands = 0;
    for (std::size_t lane = 0; lane < kLanes; ++lane) {
      lane_statements = std::max(lane_statements, scalar_statements[(g * kLanes) + lane]);
      lane_operands = std::max(lane_operands, scalar_operands[(g * kLanes) + lane]);
    }
    row.union_holds =
        row.union_holds && m.statements >= lane_statements && m.operands >= lane_operands;
    row.statements = std::max(row.statements, m.statements);
    row.operands = std::max(row.operands, m.operands);
    tape.AddAdjoint(payoff, hn::Set(D(), 1.0));
    tape.SweepBack(tape.GetPosition(), lanes.mark());
    row.boundary = std::max(row.boundary, tape.BoundaryEntries());
    for (std::size_t lane = 0; lane < kLanes; ++lane) lanes.Replay(lane);
    lanes.ClearGroup();
    if (g == 0) row.allocated_first = tape.Memory().allocated_bytes;
  }
  row.allocated_last = tape.Memory().allocated_bytes;
  static_cast<void>(lanes.SweepSetup());
  return row;
}

TEST(LaneTapeMemory, BytesPerPathAtEveryWidthBesideTheScalarTape) {
  testing::ForEachWidth([](auto d) {
    using D = decltype(d);
    using Tape = LaneTape<D>;
    constexpr std::size_t kLanes = Tape::kLanes;
    std::printf("%zu lanes: active type %zu B, statement %zu B, operand %zu B\n", kLanes,
                sizeof(LaneTapeReal<D>), Tape::kStatementBytes, Tape::kOperandBytes);
    std::printf("%-36s %5s %5s %8s %8s %9s %10s %10s %10s %9s %10s\n", "product", "steps", "lanes",
                "stmts", "operands", "boundary", "tape B", "B/path", "scalar B", "adj B/p",
                "allocated");
    double worst_group = 0.0;
    for (const std::uint32_t steps : {50U, 252U}) {
      for (const ad::testing::Case& c : ad::testing::AllCases(steps)) {
        const Row row = Measure<D>(c.contract);
        const std::size_t tape_bytes =
            (row.statements * Tape::kStatementBytes) + (row.operands * Tape::kOperandBytes);
        const std::size_t scalar_bytes = (row.scalar_statements * ad::kStatementBytes) +
                                         (row.scalar_operands * ad::kOperandBytes);
        const double per_path = static_cast<double>(tape_bytes) / static_cast<double>(kLanes);
        const auto adjoint_per_path = static_cast<double>(row.statements * sizeof(double));
        std::printf("%-36s %5u %5zu %8zu %8zu %9zu %10zu %10.1f %10zu %9.0f %10zu\n",
                    c.name.c_str(), steps, kLanes, row.statements, row.operands, row.boundary,
                    tape_bytes, per_path, scalar_bytes, adjoint_per_path, row.allocated_last);
        EXPECT_TRUE(row.union_holds) << c.name << ": a group held fewer than its largest lane";
        EXPECT_EQ(row.allocated_last, row.allocated_first) << c.name << ": tape grew";
        worst_group = std::max(worst_group, static_cast<double>(tape_bytes));
      }
    }
    std::printf("measured, %zu lanes: largest group %.0f B against MEM-4 %.0f B\n", kLanes,
                worst_group, kMem4Bytes);
    EXPECT_LT(worst_group, kMem4Bytes);
  });
}

}  // namespace
}  // namespace aadp::simd
