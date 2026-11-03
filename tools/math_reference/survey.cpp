// Usage: aadp_math_survey <file>
//
// AS241 against a survey written by `python3 -I tools/math_reference/generate.py --survey <count>
// <file>`: lines of "p hi lo domain", hex floats, exact inverse normal hi + lo. Prints the maximum
// relative error per region for the production kernel and the frozen reference. A wider sample
// than the committed table, for RNG-3. Exits 1 on a malformed line, 2 on usage.
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>

#include "math/inverse_normal.hpp"
#include "rng/normal.hpp"

namespace {

constexpr std::array<const char*, 3> kRegions = {
    "central 0.075 to 0.925", "middle, 0.075 > tail >= exp(-25)", "far tail, tail < exp(-25)"};

struct Worst {
  double error = 0.0;
  double at = 0.0;
  std::size_t count = 0;
};

bool ParseHex(const std::string& text, double& out) {
  if (text.empty()) return false;
  char* end = nullptr;
  out = std::strtod(text.c_str(), &end);
  return end == text.c_str() + text.size() && std::isfinite(out);
}

double RelativeError(double y, double hi, double lo) {
  if (std::isnan(y)) return INFINITY;
  return std::fabs((y - hi) - lo) / std::fabs(hi);
}

}  // namespace

// NOLINTNEXTLINE(misc-const-correctness): the standard fixes the signature of main.
int main(int argc, char** argv) try {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <survey file>\n", argv[0]);
    return 2;
  }
  std::ifstream file(argv[1]);
  if (!file) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 1;
  }
  std::array<std::array<Worst, 3>, 2> worst{};
  std::string line;
  std::size_t number = 0;
  while (std::getline(file, line)) {
    ++number;
    std::istringstream fields(line);
    std::string p_text;
    std::string hi_text;
    std::string lo_text;
    std::size_t region = 3;
    double p = 0.0;
    double hi = 0.0;
    double lo = 0.0;
    if (!(fields >> p_text >> hi_text >> lo_text >> region) || region >= kRegions.size() ||
        !ParseHex(p_text, p) || !ParseHex(hi_text, hi) || !ParseHex(lo_text, lo) || hi == 0.0) {
      std::fprintf(stderr, "%s:%zu: malformed line\n", argv[1], number);
      return 1;
    }
    const std::array<double, 2> errors = {RelativeError(aadp::math::inverse_normal(p), hi, lo),
                                          RelativeError(aadp::inverse_normal(p), hi, lo)};
    for (std::size_t k = 0; k < errors.size(); ++k) {
      Worst& w = worst[k][region];
      ++w.count;
      if (!(errors[k] <= w.error)) {
        w.error = errors[k];
        w.at = p;
      }
    }
  }
  const std::array<const char*, 2> kernels = {"production math::inverse_normal",
                                              "frozen reference src/rng/normal.hpp"};
  for (std::size_t k = 0; k < kernels.size(); ++k) {
    for (std::size_t r = 0; r < kRegions.size(); ++r) {
      std::printf("survey max relative error, %s, %s: %.3e at p = %a (%.6e), %zu points\n",
                  kernels[k], kRegions[r], worst[k][r].error, worst[k][r].at, worst[k][r].at,
                  worst[k][r].count);
    }
  }
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "aadp_math_survey: %s\n", error.what());
  return 2;
}
