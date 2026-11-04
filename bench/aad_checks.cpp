#include "aad_checks.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aad_workloads.hpp"
#include "ad/bounds.hpp"
#include "ad/gradient.hpp"
#include "ad/tape.hpp"
#include "bump_study.hpp"
#include "lax_wendroff.hpp"
#include "oracle/tolerance.hpp"
#include "pricer/bump.hpp"
#include "pricer/draws.hpp"
#include "pricer/kernels.hpp"
#include "pricer/runner.hpp"

namespace aadp::bench::aad {

namespace {

std::string Number(double x, const char* format = "%.17g") {
  std::array<char, 48> text{};
  std::snprintf(text.data(), text.size(), format, x);
  return text.data();
}

bool SameBits(double a, double b) {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

void Fail(CheckResult& check, std::string detail) {
  if (check.passed) check.detail = std::move(detail);
  check.passed = false;
}

// ConfigFor makes the paths a multiple of the batches, so these divisions are exact.
std::uint64_t PathsPerBatch(const pricer::RunConfig& config) {
  return config.paths / config.batches;
}
std::uint64_t BlocksPerBatch(const pricer::RunConfig& config) {
  return (PathsPerBatch(config) + pricer::kBlockPaths - 1) / pricer::kBlockPaths;
}

// The association bound's per-path terms for the five Greeks, summed over each batch by
// RunBatches: the per-batch bound of tests/ad/dual_agreement_test.cpp, check 8, with W seeded on
// every input at once.
class BoundPathFn {
 public:
  BoundPathFn(const Workload& w, const pricer::RunConfig& config)
      : path_(w.contract, w.market),
        paths_per_batch_(static_cast<double>(PathsPerBatch(config))),
        blocks_per_batch_(static_cast<double>(BlocksPerBatch(config))) {}

  void operator()(const pricer::PathUniforms& u, std::array<double, ad::kInputs>& stats) {
    ad::Tape& tape = path_.tape();
    const ad::Real payoff = path_.Record(u);
    const ad::Position end = tape.GetPosition();
    const ad::Position start{};
    tape.AddAdjoint(payoff, 1.0);
    tape.SweepBackAbsolute(end, start);
    std::array<double, ad::kInputs> absolute{};
    for (std::size_t i = 0; i < ad::kInputs; ++i) absolute[i] = tape.Adjoint(path_.inputs()[i]);
    tape.ClearTangents();
    for (const ad::Real& input : path_.inputs()) tape.SetTangent(input, 1.0);
    tape.SweepForwardAbsolute(start);
    const ad::Tape::OperandSums sums = tape.SumOverOperands(end, start);
    const ad::MemoryReport memory = tape.Memory();
    const auto p = static_cast<double>(pricer::kBlockPaths);
    const double m = (2.0 * static_cast<double>(memory.statements)) +
                     ((p + 2.0) * static_cast<double>(memory.operands)) + paths_per_batch_ +
                     (2.0 * blocks_per_batch_) + 4.0;
    const double gamma = ad::bounds::Gamma(m);
    const double underflow = 2.0 * ad::bounds::UnderflowTerm(sums.adjoints + sums.tangents);
    for (std::size_t i = 0; i < ad::kInputs; ++i) {
      stats[i] = ((2.0 * gamma * absolute[i]) + underflow) * (1.0 + (2.0 * gamma));
    }
    tape.ClearTangents();
    tape.ClearAdjoints(end, start);
    path_.Rewind();
  }

 private:
  ad::PathGradient path_;
  double paths_per_batch_;
  double blocks_per_batch_;
};

void CheckValueBits(const WorkloadReport& report, CheckResult& check) {
  const VariantResult* price = report.Result(Variant::kPrice);
  for (const VariantResult& r : report.results) {
    if (r.variant == Variant::kPrice) continue;
    const std::string name(VariantName(r.variant));
    if (report.workload->kind == Kind::kLaxWendroff) {
      if (!SameBits(r.value.mean, price->value.mean)) {
        Fail(check, name + " J " + Number(r.value.mean) + ", primal " + Number(price->value.mean));
      }
      continue;
    }
    if (r.value.batch_means.size() != price->value.batch_means.size()) {
      Fail(check, name + " has another batch count");
      continue;
    }
    for (std::size_t b = 0; b < r.value.batch_means.size(); ++b) {
      if (!SameBits(r.value.batch_means[b], price->value.batch_means[b])) {
        Fail(check, name + " value batch " + std::to_string(b) + " is not Price()'s");
        break;
      }
    }
  }
}

void CheckSkip(const WorkloadReport& report, CheckResult& check) {
  const VariantResult* tape = report.Result(Variant::kTape);
  const VariantResult* skip = report.Result(Variant::kTapeSkip);
  if (report.workload->kind == Kind::kLaxWendroff) {
    for (std::size_t i = 0; i < lw::kPoints; ++i) {
      if (!(skip->gradient[i] == tape->gradient[i])) {
        Fail(check, "input " + std::to_string(i) + ": skip " + Number(skip->gradient[i]) +
                        ", tape " + Number(tape->gradient[i]));
        return;
      }
    }
    return;
  }
  for (std::size_t g = 0; g < pricer::kInputCount; ++g) {
    for (std::size_t b = 0; b < tape->greeks[g].batch_means.size(); ++b) {
      if (!(skip->greeks[g].batch_means[b] == tape->greeks[g].batch_means[b])) {
        Fail(check, std::string(InputName(g)) + " batch " + std::to_string(b) + ": skip " +
                        Number(skip->greeks[g].batch_means[b]) + ", tape " +
                        Number(tape->greeks[g].batch_means[b]));
        return;
      }
    }
  }
}

void CheckHandPricer(const WorkloadReport& report, CheckResult& check) {
  const VariantResult* tape = report.Result(Variant::kTape);
  const VariantResult* hand = report.Result(Variant::kHand);
  const Workload& w = *report.workload;
  const pricer::PathShape shape{.steps = w.contract.steps,
                                .crossing = pricer::NeedsCrossingUniforms(w.contract)};
  const auto bounds = pricer::RunBatches<ad::kInputs>(
      report.config, shape, [&] { return BoundPathFn(w, report.config); });
  for (std::size_t b = 0; b < bounds.size(); ++b) {
    for (std::size_t g = 0; g < pricer::kInputCount; ++g) {
      const double h = hand->greeks[g].batch_means[b];
      const double t = tape->greeks[g].batch_means[b];
      const double limit = (bounds[b][g] * (1.0 + 0x1p-50)) + ad::bounds::kBoundRounding +
                           (0x1p-52 * (std::fabs(h) + std::fabs(t)));
      const double distance = std::fabs(h - t);
      if (!(distance <= limit)) {
        Fail(check, std::string(InputName(g)) + " batch " + std::to_string(b) + ": hand " +
                        Number(h) + ", tape " + Number(t) + ", bound " + Number(limit));
      }
      check.worst = std::max(check.worst, distance / limit);
    }
  }
}

void CheckHandLw(const WorkloadReport& report, CheckResult& check) {
  const VariantResult* tape_result = report.Result(Variant::kTape);
  const VariantResult* hand = report.Result(Variant::kHand);
  const lw::Field<double> w = lw::OutputWeights();
  double norm = 0.0;
  for (const double x : w) norm += x * x;
  norm = std::sqrt(norm) * (1.0 + 0x1p-50);
  const double limit = lw::kHandTapePasses * static_cast<double>(lw::kTimeSteps) *
                       GammaOf(lw::kReverseRoundings) * lw::kReverseAbsoluteRowSum * norm;
  for (std::size_t i = 0; i < lw::kPoints; ++i) {
    const double distance = std::fabs(hand->gradient[i] - tape_result->gradient[i]);
    if (!(distance <= limit)) {
      Fail(check, "input " + std::to_string(i) + ": hand " + Number(hand->gradient[i]) + ", tape " +
                      Number(tape_result->gradient[i]) + ", bound " + Number(limit));
    }
    check.worst = std::max(check.worst, distance / limit);
  }
}

void CheckBump(const WorkloadReport& report, Variant variant, CheckResult& check) {
  const VariantResult* tape = report.Result(Variant::kTape);
  const VariantResult* bump = report.Result(variant);
  const pricer::BumpScheme scheme = variant == Variant::kBumpCentral ? pricer::BumpScheme::kCentral
                                                                     : pricer::BumpScheme::kForward;
  const StepChoices& steps =
      scheme == pricer::BumpScheme::kCentral ? report.workload->central : report.workload->forward;
  if (report.workload->kind == Kind::kLaxWendroff) {
    for (std::size_t i = 0; i < lw::kPoints; ++i) {
      // Deterministic: no SE, so the critical value does not enter.
      const double b = CheckAllowance(steps[0],
                                      RoundoffScale(report.rounding_count, bump->absolute_up[i],
                                                    bump->absolute_down[i], bump->denominator[i]),
                                      0.0);
      const double distance = std::fabs(bump->gradient[i] - tape->gradient[i]);
      if (!(distance <= b)) {
        Fail(check, "input " + std::to_string(i) + ": bump " + Number(bump->gradient[i]) +
                        ", tape " + Number(tape->gradient[i]) + ", b " + Number(b));
      }
      check.worst = std::max(check.worst, distance / b);
    }
    return;
  }
  for (std::size_t g = 0; g < pricer::kInputCount; ++g) {
    const std::vector<double>& x = bump->greeks[g].batch_means;
    const std::vector<double>& y = tape->greeks[g].batch_means;
    std::vector<double> difference(x.size());
    for (std::size_t b = 0; b < x.size(); ++b) difference[b] = x[b] - y[b];
    const BatchStatistics stats = BatchMeansStatistics(difference);
    const double b = CheckAllowance(steps[g],
                                    RoundoffScale(report.rounding_count, bump->absolute_up[g],
                                                  bump->absolute_down[g], bump->denominator[g]),
                                    report.critical);
    const ToleranceResult result = ApplyTolerance(
        stats, 0.0,
        {.b = b, .se_ceiling = tape->greeks[g].standard_error, .critical = report.critical});
    if (!result.passed()) {
      Fail(check, std::string(InputName(g)) + ": bump " + Number(bump->greeks[g].mean) + ", tape " +
                      Number(tape->greeks[g].mean) + ", mean difference " + Number(stats.mean) +
                      ", bound " + Number(result.bound) + ", SE of the difference " +
                      Number(stats.standard_error) + ", tape SE " +
                      Number(tape->greeks[g].standard_error) +
                      ", failed: " + result.failed_condition());
    }
    check.worst = std::max(check.worst, std::fabs(stats.mean) / result.bound);
  }
}

std::string Escape(std::string_view text) {
  std::string out(text);
  std::ranges::replace_if(out, [](char c) { return c == '\n' || c == '\r'; }, ' ');
  return out;
}

std::string Line(std::string_view key, const std::string& value) {
  return std::string(key) + " " + Escape(value) + "\n";
}

std::string ResultLines(const Workload& w, const VariantResult& r) {
  const std::string variant(VariantName(r.variant));
  std::string out = Line("variant", variant + " " + BenchmarkName(w, r.variant));
  if (w.kind == Kind::kLaxWendroff) {
    return out + Line("estimate", variant + " value " + Number(r.value.mean) + " 0");
  }
  out += Line("estimate",
              variant + " value " + Number(r.value.mean) + " " + Number(r.value.standard_error));
  if (!r.has_greeks) return out;
  for (std::size_t g = 0; g < pricer::kInputCount; ++g) {
    out +=
        Line("estimate", variant + " " + std::string(InputName(g)) + " " +
                             Number(r.greeks[g].mean) + " " + Number(r.greeks[g].standard_error));
  }
  return out;
}

std::string StepLines(const Workload& w) {
  const bool lw = w.kind == Kind::kLaxWendroff;
  const std::size_t inputs = lw ? 1 : pricer::kInputCount;
  std::string out;
  for (const pricer::BumpScheme scheme :
       {pricer::BumpScheme::kCentral, pricer::BumpScheme::kForward}) {
    const StepChoices& choices = scheme == pricer::BumpScheme::kCentral ? w.central : w.forward;
    for (std::size_t g = 0; g < inputs; ++g) {
      out += Line("step", std::string(SchemeName(scheme)) + " " +
                              (lw ? std::string("all") : std::string(InputName(g))) + " " +
                              Number(choices[g].h) + " " + Number(choices[g].bias) + " " +
                              Number(choices[g].bias_se) + " " + Number(choices[g].truncation));
    }
  }
  return out;
}

}  // namespace

double CheckCriticalValue() {
  return CriticalValue(kFamilyWiseAlpha, kStatisticalFamily, kMcBatches);
}

bool WorkloadReport::valid() const {
  return !checks.empty() &&
         std::ranges::all_of(checks, [](const CheckResult& c) { return c.passed; });
}

const VariantResult* WorkloadReport::Result(Variant variant) const {
  for (const VariantResult& r : results) {
    if (r.variant == variant) return &r;
  }
  return nullptr;
}

std::vector<CheckResult> CheckResults(const WorkloadReport& report) {
  std::vector<CheckResult> checks;
  const auto add = [&checks](std::string name) -> CheckResult& {
    checks.push_back({.name = std::move(name), .passed = true, .worst = 0.0, .detail = {}});
    return checks.back();
  };
  CheckValueBits(report, add("value-bits"));
  CheckSkip(report, add("skip-equals-tape"));
  if (report.workload->hand) {
    CheckResult& hand = add("hand-vs-tape");
    if (report.workload->kind == Kind::kLaxWendroff) {
      CheckHandLw(report, hand);
    } else {
      CheckHandPricer(report, hand);
    }
  }
  CheckBump(report, Variant::kBumpCentral, add("central-vs-tape"));
  CheckBump(report, Variant::kBumpForward, add("forward-vs-tape"));
  return checks;
}

WorkloadReport Verify(const Workload& workload, const pricer::RunConfig& config) {
  WorkloadReport report;
  report.workload = &workload;
  report.config = config;
  report.tape = MeasureTape(workload, config);
  report.nonzero_fraction = NonzeroPayoffFraction(workload, config);
  const auto statements = static_cast<double>(report.tape.statements_per_path);
  report.rounding_count =
      workload.kind == Kind::kLaxWendroff
          ? RoundingCount(statements, 0, 0)
          : RoundingCount(statements, config.paths / config.batches, config.batches);
  report.critical = CheckCriticalValue();
  for (const Variant variant : kVariants) {
    if (!HasVariant(workload, variant)) continue;
    report.results.push_back(MakeRunner(workload, variant, config)());
  }
  report.checks = CheckResults(report);
  return report;
}

std::string BenchmarkName(const Workload& workload, Variant variant) {
  return std::string(workload.id) + "." + std::string(VariantName(variant));
}

std::string RenderSection(const WorkloadReport& report) {
  const Workload& w = *report.workload;
  const bool lw = w.kind == Kind::kLaxWendroff;
  std::string out;
  const auto line = [&out](std::string_view key, const std::string& value) {
    out += Line(key, value);
  };
  line("workload", std::string(w.id));
  line("targets", std::string(w.targets));
  line("description", std::string(w.description));
  line("differences", std::string(w.differences));
  line("kind", lw ? "lax-wendroff" : "pricer");
  line("paths", std::to_string(report.config.paths));
  line("batches", std::to_string(report.config.batches));
  line("threads", std::to_string(report.config.threads));
  line("seed", std::to_string(report.config.seed));
  line("steps", std::to_string(lw ? lw::kTimeSteps : w.contract.steps));
  line("inputs", std::to_string(lw ? lw::kPoints : pricer::kInputCount));
  line("tape_setup_statements", std::to_string(report.tape.setup_statements));
  line("tape_statements_per_path", std::to_string(report.tape.statements_per_path));
  line("tape_mean_statements_per_path", Number(report.tape.mean_statements_per_path, "%.1f"));
  line("tape_operands_per_path", std::to_string(report.tape.operands_per_path));
  line("tape_bytes_per_path", std::to_string(report.tape.bytes_per_path));
  line("tape_adjoint_bytes_per_path", std::to_string(report.tape.adjoint_bytes_per_path));
  line("tape_allocated_bytes", std::to_string(report.tape.allocated_bytes));
  line("tape_paths_sampled", std::to_string(report.tape.paths_sampled));
  line("nonzero_payoff_fraction", Number(report.nonzero_fraction, "%.6f"));
  line("rounding_count", Number(report.rounding_count, "%.1f"));
  line("critical_value", Number(report.critical, "%.4f"));
  line("valid", report.valid() ? "yes" : "no");
  for (const CheckResult& c : report.checks) {
    line("check", c.name + " " + (c.passed ? "pass" : "fail") + " " + Number(c.worst, "%.4g") +
                      (c.detail.empty() ? "" : " " + c.detail));
  }
  for (const auto& [variant, bytes] : report.peak_rss) {
    line("peak_rss", variant + " " + std::to_string(bytes));
  }
  for (const VariantResult& r : report.results) out += ResultLines(w, r);
  return out + StepLines(w);
}

}  // namespace aadp::bench::aad
