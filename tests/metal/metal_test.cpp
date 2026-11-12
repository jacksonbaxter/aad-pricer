// The GPU checks of stage 9, as docs/NOTES.md, "Stage 9", registers them: Philox known answers and
// the production stream on the GPU, the 23-bit uniform, the property kernels, the command-buffer
// split, and GPU-2 on the three contracts.
//
// Every test needs a Metal device. Without one (MTLCreateSystemDefaultDevice gives nil, as on a CI
// virtual machine) each skips with that reason; .github/workflows/ci.yml allows the skip by name,
// and only where its own probe found no device. On the development Mac none may skip.
#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include "f32/contracts.hpp"
#include "f32/cpu_pricer.hpp"
#include "f32/float_kernels.hpp"
#include "f32/paired.hpp"
#include "metal/metal_backend.hpp"
#include "metal/probes.hpp"
#include "pricer/draws.hpp"
#include "rng/philox.hpp"

#ifndef AADP_SMOKE_SIZE
#error "cmake/area_metal.cmake sets AADP_SMOKE_SIZE"
#endif

namespace aadp::metal {
namespace {

constexpr bool kSmoke = AADP_SMOKE_SIZE != 0;
// The pre-registered test size, and the smoke size of every build but Release.
constexpr std::uint64_t kGpu2Paths = kSmoke ? (1ULL << 16U) : (1ULL << 20U);
constexpr std::uint32_t kRandom = 1U << 20U;
constexpr std::uint32_t kThreads = 8;

const std::expected<std::unique_ptr<Context>, std::string>& Created() {
  static const auto created = Context::Create();
  return created;
}

class Gpu : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasDevice()) {
      GTEST_SKIP() << "SKIP no Metal device: MTLCreateSystemDefaultDevice gave nil, and the GPU "
                      "checks need one";
    }
    ASSERT_TRUE(Created().has_value()) << Created().error();
  }
  static Context& Device() { return *Created().value(); }
};

std::vector<std::uint32_t> RunProbe(ProbeKernel kernel, const std::vector<std::uint32_t>& in,
                                    std::size_t in_words, std::size_t out_words) {
  const auto threads = static_cast<std::uint32_t>(in.size() / in_words);
  auto out = Created().value()->Probe(kernel, in, threads * out_words, threads);
  EXPECT_TRUE(out.has_value()) << out.error();
  return out.value_or(std::vector<std::uint32_t>{});
}

struct KnownAnswer {
  Philox4x32Counter counter;
  Philox4x32Key key;
  Philox4x32Output ten;
  Philox4x32Output seven;  // zero where Random123's file has no 7-round line
};

// The vectors of tests/oracle/philox_test.cpp, which cites Random123's file and its hash.
constexpr auto kVectors = std::to_array<KnownAnswer>({
    {.counter = {0x00000000, 0x00000000, 0x00000000, 0x00000000},
     .key = {0x00000000, 0x00000000},
     .ten = {0x6627e8d5, 0xe169c58d, 0xbc57ac4c, 0x9b00dbd8},
     .seven = {0x5f6fb709, 0x0d893f64, 0x4f121f81, 0x4f730a48}},
    {.counter = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff},
     .key = {0xffffffff, 0xffffffff},
     .ten = {0x408f276d, 0x41c83b0e, 0xa20bc7c6, 0x6d5451fd},
     .seven = {}},
    {.counter = {0x243f6a88, 0x85a308d3, 0x13198a2e, 0x03707344},
     .key = {0xa4093822, 0x299f31d0},
     .ten = {0xd16cfe09, 0x94fdcceb, 0x5001e420, 0x24126ea1},
     .seven = {0x4dfccaba, 0x190a87f0, 0xc47362ba, 0xb6b5242a}},
});

TEST_F(Gpu, PhiloxKnownAnswersTenAndSevenRounds) {
  std::vector<std::uint32_t> in;
  std::vector<Philox4x32Output> expected;
  for (const KnownAnswer& v : kVectors) {
    for (const int rounds : {10, 7}) {
      const Philox4x32Output& answer = rounds == 10 ? v.ten : v.seven;
      if (answer == Philox4x32Output{}) continue;
      in.insert(in.end(), v.counter.begin(), v.counter.end());
      in.insert(in.end(), v.key.begin(), v.key.end());
      in.push_back(static_cast<std::uint32_t>(rounds));
      expected.push_back(answer);
    }
  }
  ASSERT_EQ(expected.size(), 5U);
  const std::vector<std::uint32_t> out = RunProbe(ProbeKernel::kPhilox, in, 7, 4);
  ASSERT_EQ(out.size(), 4 * expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ((Philox4x32Output{out[4 * i], out[4 * i + 1], out[4 * i + 2], out[4 * i + 3]}),
              expected[i])
        << "vector " << i;
  }
}

// The pricing kernel's block for (seed; path, step) gives DrawStep's two uniforms, on 2^16
// triples: paths around the 2^32 boundary of the counter and seeds that use their high word.
TEST_F(Gpu, PhiloxStreamIsDrawStepsStream) {
  constexpr std::array<std::uint64_t, 4> kSeeds = {f32::kTestSeed, 0x0000000100000007ULL,
                                                   0xFFFFFFFFFFFFFFFFULL, 0x123456789ABCDEF0ULL};
  std::vector<std::uint32_t> in;
  struct Triple {
    std::uint64_t seed;
    std::uint64_t path;
    std::uint32_t step;
  };
  std::vector<Triple> triples;
  for (std::uint32_t i = 0; i < (1U << 16U); ++i) {
    const std::uint64_t seed = kSeeds[i % kSeeds.size()];
    const std::uint64_t path = 0xFFFFF000ULL + (i / 4) % 8192;
    const std::uint32_t step = (i * 2654435761U) % 300;
    triples.push_back({.seed = seed, .path = path, .step = step});
    in.insert(in.end(),
              {static_cast<std::uint32_t>(seed), static_cast<std::uint32_t>(seed >> 32U),
               static_cast<std::uint32_t>(path), static_cast<std::uint32_t>(path >> 32U), step});
  }
  const std::vector<std::uint32_t> out = RunProbe(ProbeKernel::kDraw, in, 5, 4);
  ASSERT_EQ(out.size(), 4 * triples.size());
  for (std::size_t i = 0; i < triples.size(); ++i) {
    const Triple& t = triples[i];
    const pricer::StepUniforms expected = pricer::DrawStep(t.seed, t.path, t.step);
    const double normal = UniformFromBits((std::uint64_t{out[4 * i]} << 32U) | out[4 * i + 1]);
    const double crossing =
        UniformFromBits((std::uint64_t{out[4 * i + 2]} << 32U) | out[4 * i + 3]);
    ASSERT_EQ(normal, expected.normal) << "seed " << t.seed << " path " << t.path;
    ASSERT_EQ(crossing, expected.crossing) << "seed " << t.seed << " path " << t.path;
  }
}

// The GPU's uniform is the double column's 23-bit uniform, exactly, and never 0 or 1, on the
// constructed words and 2^20 random ones.
TEST_F(Gpu, Uniform23IsExactAndNeverZeroOrOne) {
  std::vector<std::uint32_t> in = {0U,          1U,          511U,        512U,       0xFFFFFDFFU,
                                   0xFFFFFE00U, 0xFFFFFF7FU, 0xFFFFFF80U, 0xFFFFFFFFU};
  for (std::uint32_t i = 0; i < kRandom; ++i) {
    in.push_back(probes::RandomWord(f32::kPropertySeed, i, 30));
  }
  const std::vector<std::uint32_t> out = RunProbe(ProbeKernel::kUniform, in, 1, 1);
  ASSERT_EQ(out.size(), in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    const auto u = std::bit_cast<float>(out[i]);
    ASSERT_EQ(static_cast<double>(u), f32::UniformFromWord(in[i], f32::UniformBits::k23)) << in[i];
    ASSERT_GE(u, 0x1p-24F) << in[i];
    ASSERT_LE(u, 1.0F - 0x1p-24F) << in[i];
  }
}

// + - * / and sqrt round as the CPU's IEEE round to nearest even does, on 2^20 random pairs and
// the constructed ties.
TEST_F(Gpu, ArithmeticRoundsAsTheCpuDoes) {
  const std::vector<std::uint32_t> in = probes::ArithmeticInputs(f32::kPropertySeed, kRandom);
  const std::vector<std::uint32_t> expected = probes::ArithmeticReference(in);
  const std::vector<std::uint32_t> out = RunProbe(ProbeKernel::kArithmetic, in, 2, 5);
  ASSERT_EQ(out.size(), expected.size());
  std::array<int, 5> mismatches{};
  for (std::size_t i = 0; i < out.size(); ++i) {
    if (out[i] != expected[i]) ++mismatches[i % 5];
  }
  std::printf("mismatches: add %d, subtract %d, multiply %d, divide %d, sqrt %d of %zu\n",
              mismatches[0], mismatches[1], mismatches[2], mismatches[3], mismatches[4],
              out.size() / 5);
  EXPECT_EQ(mismatches, (std::array<int, 5>{}));
}

TEST_F(Gpu, NoContraction) {
  const std::vector<std::uint32_t> in = probes::ContractionInputs(f32::kPropertySeed, kRandom);
  const std::vector<std::uint32_t> expected = probes::ContractionReference(in);
  const std::vector<std::uint32_t> out = RunProbe(ProbeKernel::kContraction, in, 3, 1);
  ASSERT_EQ(out.size(), expected.size());
  int mismatches = 0;
  for (std::size_t i = 0; i < out.size(); ++i) mismatches += out[i] != expected[i] ? 1 : 0;
  std::printf("a * b + c: %d of %zu differ from two roundings\n", mismatches, out.size());
  EXPECT_EQ(mismatches, 0);
}

// Measured on the Apple M4 Max at stage 9 and asserted since, so that a change of device or
// driver shows (docs/NOTES.md, "Stage 9 results"): float denormals are flushed to zero as inputs
// (x * 1, x + 0, x > 0 and x != 0 all see zero) and as results (2^-126 * 2^-k and a difference
// that lands below 2^-126 give zero). The words are a * b, a + b, a > 0, a != 0 for the pairs of
// probes::DenormalInputs, in order.
TEST_F(Gpu, DenormalsFlushToZeroAsMeasured) {
  constexpr std::array<std::array<std::uint32_t, 4>, 12> kMeasured = {{
      {0x00000000U, 0x3f800000U, 0U, 0U},
      {0x00000000U, 0x00000000U, 0U, 0U},
      {0x00000000U, 0x3f800000U, 0U, 0U},
      {0x00000000U, 0x00000000U, 0U, 0U},
      {0x00000000U, 0x3f800000U, 0U, 0U},
      {0x00000000U, 0x00000000U, 0U, 0U},
      {0x00000000U, 0x3f800000U, 0U, 0U},
      {0x00000000U, 0x00000000U, 0U, 0U},
      {0x00000000U, 0x3f000000U, 1U, 1U},
      {0x00000000U, 0x3a800000U, 1U, 1U},
      {0x00000000U, 0x34000000U, 1U, 1U},
      {0x80000000U, 0x00000000U, 1U, 1U},
  }};
  const std::vector<std::uint32_t> in = probes::DenormalInputs();
  const std::vector<std::string> names = probes::DenormalNames();
  const std::vector<std::uint32_t> out = RunProbe(ProbeKernel::kPairs, in, 2, 4);
  ASSERT_EQ(out.size(), 4 * kMeasured.size());
  ASSERT_EQ(names.size(), kMeasured.size());
  for (std::size_t i = 0; i < kMeasured.size(); ++i) {
    for (std::size_t j = 0; j < 4; ++j) {
      EXPECT_EQ(out[4 * i + j], kMeasured[i][j]) << names[i] << ", output " << j;
    }
  }
}

// Measured at stage 9: uint to float rounds to nearest even, as the CPU does, so float(k) 2^-32
// is exactly 1.0 for the top 128 words, the hazard Uniform23 avoids.
TEST_F(Gpu, UintToFloatRoundsToNearestEvenAsMeasured) {
  const std::vector<std::uint32_t> in = probes::UintInputs(f32::kPropertySeed, kRandom);
  const std::vector<std::uint32_t> out = RunProbe(ProbeKernel::kUintToFloat, in, 1, 2);
  ASSERT_EQ(out.size(), 2 * in.size());
  int mismatches = 0;
  for (std::size_t i = 0; i < in.size(); ++i) {
    mismatches += out[2 * i] != std::bit_cast<std::uint32_t>(static_cast<float>(in[i])) ? 1 : 0;
  }
  EXPECT_EQ(mismatches, 0);
  // in[2] is 2^32 - 129, in[3] is 2^32 - 128: the first word whose float(k) 2^-32 is 1.0.
  EXPECT_LT(std::bit_cast<float>(out[2 * 2 + 1]), 1.0F);
  EXPECT_EQ(std::bit_cast<float>(out[2 * 3 + 1]), 1.0F);
  EXPECT_EQ(std::bit_cast<float>(out[2 * 4 + 1]), 1.0F);
}

// The exp and log the kernels call, within MSL Table 8.1's 4 ULP on both pre-registered grids.
void ExpectWithinSpec(ProbeKernel kernel, probes::Grid grid, bool is_exp) {
  const std::vector<float> x = probes::GridPoints(grid);
  const std::vector<std::uint32_t> out = RunProbe(kernel, probes::Bits(x), 1, 1);
  const std::vector<float> got = probes::Floats(out);
  ASSERT_EQ(got.size(), x.size());
  const probes::FunctionError e = is_exp ? probes::ExpError(x, got) : probes::LogError(x, got);
  std::printf("%s: %llu points, max %.4f ULP at %.9g, mean %.4f\n", probes::GridName(grid),
              static_cast<unsigned long long>(e.points), e.max_ulp, e.max_ulp_at, e.mean_ulp);
  EXPECT_LE(e.max_ulp, 4.0) << probes::GridName(grid);
}

TEST_F(Gpu, ExpWithinFourUlp) {
  ExpectWithinSpec(ProbeKernel::kExp, probes::Grid::kExpWide, true);
  ExpectWithinSpec(ProbeKernel::kExp, probes::Grid::kExpPricer, true);
}

TEST_F(Gpu, LogWithinFourUlp) {
  ExpectWithinSpec(ProbeKernel::kLog, probes::Grid::kLogWide, false);
  ExpectWithinSpec(ProbeKernel::kLog, probes::Grid::kLogUnit, false);
}

// The kernels' own compensated add, on the case whose plain float sum never moves.
TEST_F(Gpu, CompensationSurvivesCompilation) {
  const std::vector<std::uint32_t> in = {std::bit_cast<std::uint32_t>(1.0F),
                                         std::bit_cast<std::uint32_t>(0.75F * 0x1p-24F), 1U << 16U};
  const std::vector<std::uint32_t> out = RunProbe(ProbeKernel::kCompensated, in, 3, 2);
  ASSERT_EQ(out.size(), 2U);
  f32::Compensated cpu;
  cpu = f32::CompensatedAdd(cpu, 1.0F);
  for (int i = 0; i < (1 << 16); ++i) cpu = f32::CompensatedAdd(cpu, 0.75F * 0x1p-24F);
  EXPECT_EQ(std::bit_cast<float>(out[0]), 1.0F + 0.75F * 0x1p-8F);
  EXPECT_EQ(out[0], std::bit_cast<std::uint32_t>(cpu.sum));
  EXPECT_EQ(std::bit_cast<float>(out[1]), 1.0F);
}

// Splitting a run across command buffers changes no span's bits: each buffer starts at its own
// first path. 7 spans a buffer, so buffers end mid-run and the last one is short.
TEST_F(Gpu, CommandBufferSplitChangesNoBit) {
  const f32::Workload& w = f32::kWorkloads[0];
  const auto params = f32::MakeParams(w.contract, w.market, f32::kTestSeed, f32::kSpan);
  ASSERT_TRUE(params.has_value());
  constexpr std::uint64_t kPaths = std::uint64_t{16} * 1000U;
  const auto whole = Device().Price(*params, kPaths, kPaths, true);
  const auto split = Device().Price(*params, kPaths, std::uint64_t{16} * 7U, true);
  ASSERT_TRUE(whole.has_value()) << whole.error();
  ASSERT_TRUE(split.has_value()) << split.error();
  EXPECT_EQ(whole->buffers.size(), 1U);
  EXPECT_EQ(split->buffers.size(), 143U);
  ASSERT_EQ(whole->spans.size(), split->spans.size());
  EXPECT_EQ(std::memcmp(whole->spans.data(), split->spans.data(),
                        whole->spans.size() * sizeof(f32::SpanSum)),
            0);
  EXPECT_EQ(whole->moments.sum, split->moments.sum);
}

// GPU-2: |D - B| <= 0.01 SE_B, D the GPU's float32 price and B the production double kernels on
// the same 23-bit uniforms.
void ExpectGpu2(const f32::Workload& w) {
  const auto params = f32::MakeParams(w.contract, w.market, f32::kTestSeed, f32::kSpan);
  ASSERT_TRUE(params.has_value());
  const auto gpu = Created().value()->Price(*params, kGpu2Paths, kGpu2Paths, true);
  ASSERT_TRUE(gpu.has_value()) << gpu.error();
  const auto b =
      f32::PriceSpansDouble(w.contract, w.market, f32::kTestSeed, kGpu2Paths / f32::kSpan,
                            f32::kSpan, f32::UniformBits::k23, kThreads);
  ASSERT_TRUE(b.has_value());
  const f32::Column d = f32::FromFloat(gpu->spans, f32::kSpan, f32::Scale(w.market));
  const f32::Estimate eb = f32::ColumnEstimate(*b);
  const f32::Estimate ed = f32::ToEstimate(gpu->moments, f32::Scale(w.market));
  const f32::Difference diff = f32::PairedDifference(d, *b);
  std::printf(
      "%s, N = %llu: B %.9f (SE %.3e), D %.9f, D - B %.3e (paired SE %.3e), "
      "|D - B| / SE_B %.5f, relative %.3e\n",
      std::string(w.name).c_str(), static_cast<unsigned long long>(kGpu2Paths), eb.mean,
      eb.standard_error, ed.mean, diff.mean, diff.standard_error,
      std::fabs(diff.mean) / eb.standard_error, diff.mean / eb.mean);
  EXPECT_LE(std::fabs(ed.mean - eb.mean), f32::kGpu2Factor * eb.standard_error) << w.name;
  EXPECT_TRUE(std::isfinite(ed.mean));
}

TEST_F(Gpu, Gpu2Asian12) { ExpectGpu2(f32::kWorkloads[0]); }
TEST_F(Gpu, Gpu2Asian252) { ExpectGpu2(f32::kWorkloads[1]); }
TEST_F(Gpu, Gpu2Barrier156) { ExpectGpu2(f32::kWorkloads[2]); }

}  // namespace
}  // namespace aadp::metal
