// Bias of each barrier treatment, with its SE: an accuracy table, not a benchmark.
//
//   aadp_pricer_bias [--paths <N>] [--seed <s>] [--threads <n>] [--references <file>]
//
// One contract, BGK's down-and-out call (S0 = K = 100, H = 95, sigma 0.3, r 0.1, q 0, T 0.2, 50
// dates), priced by every treatment, each against the reference for the contract it prices: the
// Reiner-Rubinstein closed form for continuous monitoring and the BGK recursion for discrete
// (references/oracle_values.csv). Delta is the pathwise derivative through the forward-mode Dual,
// so the hard indicator shows the naive failure. The bridge is also run against the discrete
// reference, which is the misuse the pricer refuses (NOTES.md: never on a discrete contract).
//
// Expectations, stated before the first run, each judged by |bias| against t SE with t the
// Bonferroni critical value over the expectation rows (alpha_F 0.01, B 64):
//   unbiased   bridge and sampled crossing against the continuous value, one-step and hard
//              against the discrete value, the bridge and one-step deltas against theirs, and the
//              hard delta against the naive limit of tools/quadrature
//   biased     the bridge against the discrete value, and the hard delta against the true delta
//   measured   smoothing at widths 0.005, 0.01 and 0.02 of S0, value and delta: the trend is
//              the result, so nothing is asserted about it
// Exit codes: 0 every expectation holds, 1 one does not, 2 usage or a missing reference.
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "oracle/references.hpp"
#include "oracle/tolerance.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/dual_ops.hpp"
#include "pricer/kernels.hpp"
#include "pricer/model.hpp"
#include "pricer/runner.hpp"
#include "support/dual.hpp"
#include "support/mc_support.hpp"

namespace aadp::pricer {
namespace {

enum class Expect : std::uint8_t { kUnbiased, kBiased, kMeasured };

struct Options {
  std::uint64_t paths = std::uint64_t{1} << 22;
  // Development seed, never 1729 or 271828.
  std::uint64_t seed = 20261109;
  std::uint32_t threads = 8;
  std::string references = "references/oracle_values.csv";
};

constexpr std::uint32_t kBatches = 64;
constexpr std::uint32_t kDates = 50;
constexpr double kLevel = 95.0;
constexpr Market<double> kMarket{.spot = 100.0, .sigma = 0.3, .r = 0.1, .q = 0.0, .maturity = 0.2};

struct Leg {
  const char* reference_id;
  Expect expect;
};

struct Row {
  std::string treatment;
  std::string against;
  Contract contract;
  Leg value;
  Leg delta;
};

Contract DownOut(Monitoring monitoring, Treatment treatment, double width = 0.0) {
  Contract c;
  c.product = Product::kBarrier;
  c.type = OptionType::kCall;
  c.strike = 100.0;
  c.steps = kDates;
  c.barrier = {.direction = Direction::kDown,
               .knock = Knock::kOut,
               .monitoring = monitoring,
               .treatment = treatment,
               .level = kLevel,
               .smoothing_width = width};
  return c;
}

std::vector<Row> Rows() {
  using enum Monitoring;
  using enum Treatment;
  using enum Expect;
  std::vector<Row> rows = {
      {"bridge",
       "continuous",
       DownOut(kContinuous, kBridge),
       {"O7", kUnbiased},
       {"CB095.delta", kUnbiased}},
      {"sampled crossing, hard",
       "continuous",
       DownOut(kContinuous, kHard),
       {"O7", kUnbiased},
       {"CB095.delta", kMeasured}},
      {"one-step survival",
       "discrete",
       DownOut(kDiscrete, kOneStep),
       {"O5", kUnbiased},
       {"O6", kUnbiased}},
      {"hard indicator", "discrete", DownOut(kDiscrete, kHard), {"O5", kUnbiased}, {"O6", kBiased}},
      {"hard indicator, naive delta limit",
       "discrete",
       DownOut(kDiscrete, kHard),
       {"O5", kMeasured},
       {"DB095.naive", kUnbiased}},
      // Priced as the continuous contract, judged against the discrete one: the refused misuse.
      {"bridge on the discrete contract",
       "discrete",
       DownOut(kContinuous, kBridge),
       {"O5", kBiased},
       {"O6", kMeasured}},
  };
  for (const double width : {0.005, 0.01, 0.02}) {
    char name[64];
    std::snprintf(name, sizeof(name), "smoothed, width %.3f S0", width);
    rows.push_back({name,
                    "discrete",
                    DownOut(kDiscrete, kSmoothed, width),
                    {"O5", kMeasured},
                    {"O6", kMeasured}});
  }
  return rows;
}

std::size_t ExpectationCount(const std::vector<Row>& rows) {
  std::size_t count = 0;
  for (const Row& row : rows) {
    count += row.value.expect != Expect::kMeasured ? 1 : 0;
    count += row.delta.expect != Expect::kMeasured ? 1 : 0;
  }
  return count;
}

// Batch means of value and delta, the pathwise delta through the Dual's spot tangent.
std::array<BatchStatistics, 2> Run(const Contract& c, const Options& options) {
  const GbmModel<test::Dual> model({.spot = test::Dual::Variable(kMarket.spot, test::kSlotSpot),
                                    .sigma = kMarket.sigma,
                                    .r = kMarket.r,
                                    .q = kMarket.q,
                                    .maturity = kMarket.maturity},
                                   c.steps);
  const auto table = RunBatches<2>({.seed = options.seed,
                                    .paths = options.paths,
                                    .batches = kBatches,
                                    .threads = options.threads},
                                   {.steps = c.steps, .crossing = NeedsCrossingUniforms(c)}, [&] {
                                     return [&](const PathUniforms& u, std::array<double, 2>& out) {
                                       const test::Dual payoff =
                                           DiscountedPayoff<test::Dual>(c, model, u);
                                       out[0] = payoff.value;
                                       out[1] = payoff.tangent[test::kSlotSpot];
                                     };
                                   });
  std::array<std::vector<double>, 2> means;
  for (const auto& batch : table) {
    means[0].push_back(batch[0]);
    means[1].push_back(batch[1]);
  }
  return {BatchMeansStatistics(means[0]), BatchMeansStatistics(means[1])};
}

const char* ExpectName(Expect e) {
  switch (e) {
    case Expect::kUnbiased:
      return "unbiased";
    case Expect::kBiased:
      return "biased";
    case Expect::kMeasured:
      return "measured";
  }
  return "";
}

// True when the leg meets its expectation.
bool Judge(const BatchStatistics& s, double reference, Expect expect, double critical) {
  const bool within = std::fabs(s.mean - reference) <= critical * s.standard_error;
  switch (expect) {
    case Expect::kUnbiased:
      return within;
    case Expect::kBiased:
      return !within;
    case Expect::kMeasured:
      return true;
  }
  return false;
}

bool ParseUnsigned(std::string_view text, std::uint64_t& out) {
  if (text.empty() || text.size() > 19) return false;
  std::uint64_t value = 0;
  for (const char ch : text) {
    if (ch < '0' || ch > '9') return false;
    value = (value * 10) + static_cast<std::uint64_t>(ch - '0');
  }
  out = value;
  return true;
}

int Usage(const char* message) {
  std::fprintf(stderr,
               "aadp_pricer_bias: %s\nusage: aadp_pricer_bias [--paths <N>] [--seed <s>] "
               "[--threads <n>] [--references <file>]\n",
               message);
  return 2;
}

int Main(const std::vector<std::string_view>& args) {
  Options options;
  for (std::size_t i = 0; i < args.size(); i += 2) {
    if (i + 1 >= args.size()) return Usage("an option has no value");
    std::uint64_t number = 0;
    if (args[i] == "--references") {
      options.references = std::string(args[i + 1]);
    } else if (!ParseUnsigned(args[i + 1], number)) {
      return Usage("not a whole number");
    } else if (args[i] == "--paths") {
      options.paths = number;
    } else if (args[i] == "--seed") {
      options.seed = number;
    } else if (args[i] == "--threads") {
      options.threads = static_cast<std::uint32_t>(number);
      if (number > kMaxThreads) return Usage("too many threads");
    } else {
      return Usage("unknown option");
    }
  }
  if (options.seed == 1729 || options.seed == 271828) {
    return Usage("1729 and 271828 are reserved for the oracle runs");
  }
  if (ValidateRun({.seed = options.seed,
                   .paths = options.paths,
                   .batches = kBatches,
                   .threads = options.threads}) != RunError::kNone) {
    return Usage("paths must be a positive multiple of 64, threads 1 to 256");
  }

  std::ifstream in(options.references);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const auto table = DecodeReferences(text);
  if (!in || !table) return Usage("cannot read the references");

  const std::vector<Row> rows = Rows();
  const std::size_t expectations = ExpectationCount(rows);
  const double critical = CriticalValue(kFamilyWiseAlpha, expectations, kBatches);
  std::printf(
      "# Bias of each barrier treatment. Accuracy table, not a benchmark. aadp_pricer_bias.\n"
      "# Contract: down-and-out call, S0 100, K 100, H 95, sigma 0.3, r 0.1, q 0, T 0.2, "
      "%u dates t_i = i T / %u, the discrete one monitored there.\n"
      "# seed %llu, N %llu paths, B %u batch means, %u threads. Delta: pathwise, forward-mode "
      "Dual on the spot.\n"
      "# References: %s. t = %.4f, Bonferroni over %zu expectation rows, alpha_F 0.01.\n",
      kDates, kDates, static_cast<unsigned long long>(options.seed),
      static_cast<unsigned long long>(options.paths), kBatches, options.threads,
      options.references.c_str(), critical, expectations);
  std::printf(
      "| treatment | judged against | value | SE | reference | bias | bias / SE | expected | "
      "delta | SE | reference | bias | bias / SE | expected |\n"
      "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n");

  int failures = 0;
  for (const Row& row : rows) {
    const Reference* value_ref = table->Find(row.value.reference_id);
    const Reference* delta_ref = table->Find(row.delta.reference_id);
    if (value_ref == nullptr || delta_ref == nullptr) return Usage("a reference id is missing");
    const auto stats = Run(row.contract, options);
    const std::array<const Reference*, 2> refs = {value_ref, delta_ref};
    const std::array<Expect, 2> expects = {row.value.expect, row.delta.expect};
    std::printf("| %s | %s %s", row.treatment.c_str(), row.against.c_str(), row.value.reference_id);
    for (std::size_t leg = 0; leg < 2; ++leg) {
      const double bias = stats[leg].mean - refs[leg]->value;
      const bool ok = Judge(stats[leg], refs[leg]->value, expects[leg], critical);
      failures += ok ? 0 : 1;
      std::printf(" | %.6f | %.2g | %.6f (%s) | %+.6f | %+.2f | %s%s", stats[leg].mean,
                  stats[leg].standard_error, refs[leg]->value, refs[leg]->id.c_str(), bias,
                  bias / stats[leg].standard_error, ExpectName(expects[leg]), ok ? "" : ", FAILED");
      if (!ok) {
        std::fprintf(stderr, "aadp_pricer_bias: expectation failed: %s, %s\n",
                     row.treatment.c_str(), leg == 0 ? "value" : "delta");
      }
    }
    std::printf(" |\n");
    std::fflush(stdout);
  }
  std::printf("# %s\n", failures == 0 ? "every expectation holds" : "an expectation FAILED");
  return failures == 0 ? 0 : 1;
}

}  // namespace
}  // namespace aadp::pricer

int main(int argc, char** argv) {
  try {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    return aadp::pricer::Main(args);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "aadp_pricer_bias: %s\n", error.what());
    return 2;
  }
}
