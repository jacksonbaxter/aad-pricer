// The declarations in src/svi/lets_be_rational_api.hpp against the archive's own header. Both are
// included here, so a parameter or return type that differs is a conflicting declaration of a C
// function and this file does not compile. The test then checks one value of each function by
// hand, so a declaration that compiles but binds the wrong symbol fails too.
#include <gtest/gtest.h>

#include <cmath>

// The archive's header, through the SYSTEM include directory of aadp_lets_be_rational.
#include "lets_be_rational.h"
#include "svi/lets_be_rational_api.hpp"

namespace {

TEST(LetsBeRationalApi, DeclarationsMatchTheArchiveAndBindTheRightSymbols) {
  // At the money, Black(F, F, sigma, T) = F erf(sigma sqrt(T) / sqrt(8)) (Jäckel's Black()).
  const double atm = Black(100.0, 100.0, 0.2, 1.0, 1.0);
  EXPECT_NEAR(atm, 100.0 * std::erf(0.2 / std::sqrt(8.0)), 1e-12);
  EXPECT_NEAR(ImpliedBlackVolatility(atm, 100.0, 100.0, 1.0, 1.0), 0.2, 1e-15);
  // At the money, s b'(s) / b(s) = s phi(s/2) / erf(s / sqrt(8)) for the normalised price.
  const double s = 0.2;
  const double phi = std::exp(-0.125 * s * s) / std::sqrt(2.0 * std::acos(-1.0));
  EXPECT_NEAR(BlackAccuracyFactor(0.0, s, 1.0), s * phi / std::erf(s / std::sqrt(8.0)), 1e-14);
}

}  // namespace
