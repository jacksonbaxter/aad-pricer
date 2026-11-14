// The Lax-Wendroff hand adjoint's gather form (bench/lax_wendroff.hpp) against the scatter form it
// replaced, which is kept here as the definition: the same bits at every size and step count,
// the smallest the function accepts included.
#include "lax_wendroff.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace aadp::bench::lw {
namespace {

// The reverse pass as it was at ce6e805, with the size and the steps as parameters: each point's
// adjoint scattered into its two flux adjoints, then each flux adjoint into its two points.
template <std::size_t N>
void ScatterSteps(std::array<double, N>& qbar, std::uint32_t steps, double c) {
  const double half_c = 0.5 * c;
  std::array<double, N - 1> fbar{};
  for (std::uint32_t step = 0; step < steps; ++step) {
    qbar[1] += qbar[N - 1];
    qbar[N - 1] = 0.0;
    qbar[N - 2] += qbar[0];
    qbar[0] = 0.0;
    fbar.fill(0.0);
    for (std::size_t i = N - 2; i >= 1; --i) {
      fbar[i - 1] += qbar[i];
      fbar[i] -= qbar[i];
    }
    for (std::size_t i = N - 1; i-- > 0;) {
      const double sum_bar = half_c * fbar[i];
      const double difference_bar = c * sum_bar;
      qbar[i] += sum_bar + difference_bar;
      qbar[i + 1] += sum_bar - difference_bar;
    }
  }
}

// The scatter form with each point's two flux contributions added to each other first, which
// rounds differently: what the test must be able to tell from the definition.
template <std::size_t N>
void ReassociatedSteps(std::array<double, N>& qbar, std::uint32_t steps, double c) {
  const double half_c = 0.5 * c;
  std::array<double, N - 1> fbar{};
  for (std::uint32_t step = 0; step < steps; ++step) {
    qbar[1] += qbar[N - 1];
    qbar[N - 1] = 0.0;
    qbar[N - 2] += qbar[0];
    qbar[0] = 0.0;
    fbar.fill(0.0);
    for (std::size_t i = N - 2; i >= 1; --i) {
      fbar[i - 1] += qbar[i];
      fbar[i] -= qbar[i];
    }
    std::array<double, N> in{};
    for (std::size_t i = N - 1; i-- > 0;) {
      const double sum_bar = half_c * fbar[i];
      const double difference_bar = c * sum_bar;
      in[i] += sum_bar + difference_bar;
      in[i + 1] += sum_bar - difference_bar;
    }
    for (std::size_t i = 0; i < N; ++i) qbar[i] += in[i];
  }
}

template <std::size_t N>
std::vector<std::array<double, N>> Seeds() {
  std::vector<std::array<double, N>> seeds;
  std::array<double, N> ramp{};
  std::array<double, N> mixed{};
  std::array<double, N> random{};
  std::mt19937_64 engine(20261114);
  std::uniform_real_distribution<double> uniform(-1.0, 1.0);
  for (std::size_t i = 0; i < N; ++i) {
    ramp[i] = static_cast<double>(i + 1) / static_cast<double>(N);
    // Signs, magnitudes over a few binades, and zeros of both signs.
    const double sign = i % 3 == 0 ? -1.0 : 1.0;
    mixed[i] = i % 4 == 1 ? sign * 0.0 : sign * (0.1 + (0.37 * static_cast<double>(i * i)));
    random[i] = uniform(engine);
  }
  seeds.push_back(ramp);
  seeds.push_back(mixed);
  seeds.push_back(random);
  return seeds;
}

template <std::size_t N>
bool SameBits(const std::array<double, N>& a, const std::array<double, N>& b) {
  for (std::size_t i = 0; i < N; ++i) {
    if (std::bit_cast<std::uint64_t>(a[i]) != std::bit_cast<std::uint64_t>(b[i])) return false;
  }
  return true;
}

template <std::size_t N>
void ExpectGatherIsScatter(std::uint32_t steps) {
  for (const std::array<double, N>& seed : Seeds<N>()) {
    std::array<double, N> scatter = seed;
    std::array<double, N> gather = seed;
    ScatterSteps(scatter, steps, kCourant);
    ReverseSteps(gather, steps, kCourant);
    for (std::size_t i = 0; i < N; ++i) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(gather[i]), std::bit_cast<std::uint64_t>(scatter[i]))
          << "N " << N << ", " << steps << " steps, point " << i << ": " << gather[i] << " against "
          << scatter[i];
    }
  }
}

TEST(LaxWendroffHand, GatherFormHasTheScatterFormsBits) {
  ExpectGatherIsScatter<3>(1);  // The smallest size: one interior point.
  ExpectGatherIsScatter<3>(2);
  ExpectGatherIsScatter<3>(7);
  ExpectGatherIsScatter<4>(1);
  ExpectGatherIsScatter<4>(3);
  ExpectGatherIsScatter<5>(2);
  ExpectGatherIsScatter<6>(3);
  ExpectGatherIsScatter<7>(50);
  ExpectGatherIsScatter<kPoints>(1);
  ExpectGatherIsScatter<kPoints>(kTimeSteps);
}

TEST(LaxWendroffHand, TheBenchGradientIsTheScatterForms) {
  Field<double> gradient{};
  const double value = HandGradient(InitialField(), OutputWeights(), gradient);
  Field<double> scatter = OutputWeights();
  ScatterSteps(scatter, kTimeSteps, kCourant);
  EXPECT_TRUE(SameBits(gradient, scatter));
  EXPECT_EQ(std::bit_cast<std::uint64_t>(value),
            std::bit_cast<std::uint64_t>(Primal(InitialField(), OutputWeights())));
}

// Without this, a gather form that added a point's terms in another order could pass by luck: on
// each input, at the bench's size, another association gives other bits.
TEST(LaxWendroffHand, TheInputsTellAdditionOrdersApart) {
  for (const Field<double>& seed : Seeds<kPoints>()) {
    Field<double> scatter = seed;
    Field<double> other = seed;
    ScatterSteps(scatter, kTimeSteps, kCourant);
    ReassociatedSteps(other, kTimeSteps, kCourant);
    EXPECT_FALSE(SameBits(scatter, other));
  }
}

}  // namespace
}  // namespace aadp::bench::lw
