// Second-order Greeks by forward over reverse, namespace aadp::ad.
//
// Cost model (BENCHMARKS.md AAD-8). The first-order tape (tape.hpp, gradient.hpp) gives, from one
// record and one reverse sweep per path, the value and the five first-order Greeks: delta, vega,
// rho, dividend rho and theta. It gives no second-order Greek. This file's pass records the path
// over Dual<double, N> values, whose N tangent lanes are seeded on N inputs, so every partial on
// the tape is a Dual too, and sweeps it back once with Dual adjoints seeded (1, 0, ..., 0). One
// record and one reverse sweep per path then give the value, the same five first-order Greeks,
// and N rows of the Hessian, d^2 V / d x_k d x_i for each seeded input x_k and every input x_i
// (Naumann et al., NAG TR 5/10, p. 2: a Hessian-vector product at roughly twice the adjoint's
// cost per direction). The set-up is swept once per block, as for the first order.
//   N = 1, seeded on spot:          gamma, vanna, and d^2 V / dS dr, dS dq, dS dT
//   N = 2, seeded on spot and vol:  the same, and volga, and vanna again as d^2 V / dvol dS
// So gamma, vanna and volga take one N = 2 pass, or two N = 1 passes, on top of nothing: the
// pass's own first-order Greeks are the conditional kernel's, bit for bit (below). Vanna comes
// out twice from N = 2, once per lane, which the tests use as a symmetry check.
//
// Which kernel. A pathwise second derivative of a payoff with a kink, such as max(S - K, 0), is
// zero on every path and misses the kink's mass, so it is biased. The pass therefore runs the
// conditional kernels of src/pricer/conditional_kernels.hpp, which take the expectation of the
// payoff over the last step in closed form, and for a barrier, one-step survival: on a discrete
// barrier Gerstner, Harrach and Roth 2018 (arXiv 1804.03975v3), on a continuous one the one-step
// survival Brownian bridge of Gerstner, Harrach and Roth 2019 (arXiv 1906.11002v2, section 2.1,
// Corollary 2.5), whose first derivatives are Lipschitz where the plain bridge's are not. The
// plain bridge weight is never differentiated twice. The first-order headline stays on the
// production kernels; ConditionalGradient below is the first-order tape on the conditional
// kernels, so the second-order pass can be timed against a first-order pass on the same kernel.
//
// The double tape is untouched: this is a separate tape type over Dual values, so tape.hpp,
// tape.cpp and real.hpp are the same bytes as before it existed, and the headline tape's code
// cannot have changed.
//
// Exactness. The value parts of every Dual value and partial are the first-order tape's doubles,
// computed by the same operations, and the reverse sweep's value lane forms the same products and
// sums in the same order. So the pass's value is the conditional double kernel's, and its five
// first-order Greeks are ConditionalGradient's, bit for bit. And lanes never mix, so the spot row
// of an N = 2 pass is the N = 1 pass's, bit for bit.
//
// Rows are raw second derivatives in input order (spot, vol, r, q, T): no theta sign.
#ifndef AADP_AD_SECOND_ORDER_HPP_
#define AADP_AD_SECOND_ORDER_HPP_

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <vector>

#include "ad/dual.hpp"
#include "ad/gradient.hpp"
#include "ad/tape.hpp"
#include "pricer/conditional_kernels.hpp"
#include "pricer/contract.hpp"
#include "pricer/draws.hpp"
#include "pricer/model.hpp"
#include "pricer/pricer.hpp"
#include "pricer/runner.hpp"

namespace aadp::ad {

inline constexpr std::size_t kSpotInput = 0;
inline constexpr std::size_t kVolInput = 1;

[[noreturn]] void SecondOrderFail(const char* what);

template <class D>
class SecondTape;

// The active type of the second-order tape: a Dual value and a tape index. Index 0 is passive,
// and a passive value always has zero tangents.
template <class D>
class SecondReal {
 public:
  constexpr SecondReal() = default;
  // Implicit, so that kernels mix SecondReal and double freely; a double is a constant.
  // NOLINTNEXTLINE(cppcoreguidelines-explicit-constructor,misc-explicit-constructor): see above.
  constexpr SecondReal(double value) : dual_(value) {}

  [[nodiscard]] constexpr const D& dual() const { return dual_; }
  [[nodiscard]] constexpr double value() const { return dual_.value; }
  [[nodiscard]] constexpr Index index() const { return index_; }
  [[nodiscard]] constexpr bool active() const { return index_ != kPassiveIndex; }

 private:
  friend class SecondTape<D>;
  constexpr SecondReal(const D& dual, Index index) : dual_(dual), index_(index) {}

  D dual_{};
  Index index_ = kPassiveIndex;
};

struct SecondPosition {
  std::size_t statements = 0;  // The reserved passive slot 0 included.
  std::size_t operands = 0;
};

// What the tape holds: statements cost 1 byte, operands sizeof(D) + 4, adjoints sizeof(D).
struct SecondMemory {
  std::size_t statements = 0;
  std::size_t operands = 0;
  std::size_t bytes = 0;          // statements + operands * (sizeof(D) + 4)
  std::size_t adjoint_bytes = 0;  // statements * sizeof(D)
};

// Jacobian taping with linear indexing, as tape.hpp, over Dual partials. Vectors rather than
// chunks: they grow while the longest path is first recorded and never after, since Rewind keeps
// their capacity.
template <class D>
class SecondTape {
 public:
  using Active = SecondReal<D>;

  SecondTape() { counts_.push_back(0); }

  [[nodiscard]] Active Register(const D& value) { return PushStatement(value, 0); }

  [[nodiscard]] SecondPosition GetPosition() const {
    return {.statements = counts_.size(), .operands = indices_.size()};
  }
  void Rewind(const SecondPosition& mark) {
    counts_.resize(mark.statements);
    partials_.resize(mark.operands);
    indices_.resize(mark.operands);
  }

  [[nodiscard]] const D& Adjoint(const Active& x) const { return adjoints_[x.index()]; }
  void AddAdjoint(const Active& x, double seed) {
    if (!x.active()) return;
    EnsureAdjoints();
    adjoints_[x.index()].value += seed;
  }
  void ClearAdjoints(const SecondPosition& from, const SecondPosition& stop) {
    for (std::size_t i = stop.statements; i < from.statements && i < adjoints_.size(); ++i) {
      adjoints_[i] = D{};
    }
  }

  // Reverse sweep over [stop, from), last first: each statement's Dual adjoint times each of its
  // Dual partials, added into the operand's. A normal sweep zeroes what it propagated, as
  // Tape::SweepBack does. An absolute Dual sweeps |adjoint| |partial|. With kKeep, which an
  // absolute Dual implies, every adjoint is kept for the caller to read, and the caller must clear
  // them with ClearAdjoints. Keeping changes no result: a statement's adjoint is read once, before
  // anything below it is touched.
  template <bool kKeep = D::kIsAbsolute>
  void SweepBack(const SecondPosition& from, const SecondPosition& stop) {
    EnsureAdjoints();
    std::size_t offset = from.operands;
    D* adjoints = adjoints_.data();
    for (std::size_t i = from.statements; i-- > stop.statements;) {
      const OperandCount count = counts_[i];
      if (count == 0) continue;
      const D adjoint = adjoints[i];
      if constexpr (!kKeep) adjoints[i] = D{};
      offset -= count;
      for (std::size_t j = 0; j < count; ++j) {
        Accumulate(adjoints[indices_[offset + j]], adjoint, partials_[offset + j]);
      }
    }
  }

  [[nodiscard]] SecondMemory Memory() const {
    const std::size_t statements = counts_.size() - 1;
    const std::size_t operands = indices_.size();
    return {.statements = statements,
            .operands = operands,
            .bytes = statements + (operands * (sizeof(D) + sizeof(Index))),
            .adjoint_bytes = statements * sizeof(D)};
  }

  // For the round-off checks: the smallest nonzero magnitude of any partial lane in
  // [stop, from), and of any adjoint lane, as the last absolute sweep left them.
  [[nodiscard]] double SmallestPartial(const SecondPosition& from,
                                       const SecondPosition& stop) const {
    double smallest = std::numeric_limits<double>::infinity();
    for (std::size_t o = stop.operands; o < from.operands; ++o) {
      smallest = Smallest(smallest, partials_[o]);
    }
    return smallest;
  }
  [[nodiscard]] double SmallestAdjoint(const SecondPosition& from,
                                       const SecondPosition& stop) const {
    double smallest = std::numeric_limits<double>::infinity();
    for (std::size_t i = stop.statements; i < from.statements && i < adjoints_.size(); ++i) {
      smallest = Smallest(smallest, adjoints_[i]);
    }
    return smallest;
  }

  // Recording, for the operations below. a and b are active indices.
  Active Push(const D& value, Index a, const D& da) {
    partials_.push_back(da);
    indices_.push_back(a);
    return PushStatement(value, 1);
  }
  Active Push(const D& value, Index a, const D& da, Index b, const D& db) {
    partials_.push_back(da);
    indices_.push_back(a);
    partials_.push_back(db);
    indices_.push_back(b);
    return PushStatement(value, 2);
  }

 private:
  static constexpr std::size_t kMaxStatements = 0xffffffffULL;

  static double Smallest(double smallest, const D& x) {
    const auto take = [&smallest](double v) {
      const double m = v < 0.0 ? -v : v;
      if (m != 0.0 && m < smallest) smallest = m;
    };
    take(x.value);
    for (const double t : x.tangent) take(t);
    return smallest;
  }

  static void Accumulate(D& target, const D& adjoint, const D& partial) {
    if constexpr (D::kIsAbsolute) {
      const double p = partial.value < 0.0 ? -partial.value : partial.value;
      target.value += adjoint.value * p;
      for (std::size_t k = 0; k < D::kLanes; ++k) {
        target.tangent[k] += (adjoint.tangent[k] * p) + (adjoint.value * partial.tangent[k]);
      }
    } else {
      target.value += adjoint.value * partial.value;
      for (std::size_t k = 0; k < D::kLanes; ++k) {
        target.tangent[k] +=
            (adjoint.tangent[k] * partial.value) + (adjoint.value * partial.tangent[k]);
      }
    }
  }

  void EnsureAdjoints() {
    if (adjoints_.size() < counts_.size()) adjoints_.resize(counts_.capacity(), D{});
  }

  Active PushStatement(const D& value, OperandCount count) {
    if (counts_.size() >= kMaxStatements) [[unlikely]] {
      SecondOrderFail("second-order tape index space exhausted");
    }
    const auto index = static_cast<Index>(counts_.size());
    counts_.push_back(count);
    return {value, index};
  }

  std::vector<OperandCount> counts_;
  std::vector<D> partials_;
  std::vector<Index> indices_;
  std::vector<D> adjoints_;
};

namespace second_detail {

template <class D>
SecondTape<D>*& Slot() {
  thread_local SecondTape<D>* slot = nullptr;
  return slot;
}

template <class D>
SecondTape<D>& Active() {
  SecondTape<D>* tape = Slot<D>();
  if (tape == nullptr) [[unlikely]] {
    SecondOrderFail("an operation on an active SecondReal with no active tape on this thread");
  }
  return *tape;
}

// The first-order tape's recording rules (real.hpp, detail::Record), on Dual partials.
template <class D>
SecondReal<D> Record(const D& value, const SecondReal<D>& a, const D& da) {
  if (!a.active()) return {value.value};
  return Active<D>().Push(value, a.index(), da);
}

template <class D>
SecondReal<D> Record(const D& value, const SecondReal<D>& a, const D& da, const SecondReal<D>& b,
                     const D& db) {
  if (a.active()) {
    if (b.active()) return Active<D>().Push(value, a.index(), da, b.index(), db);
    return Active<D>().Push(value, a.index(), da);
  }
  if (b.active()) return Active<D>().Push(value, b.index(), db);
  return {value.value};
}

}  // namespace second_detail

// Makes a second-order tape this thread's active one of its type for the scope's lifetime.
template <class D>
class SecondTapeScope {
 public:
  explicit SecondTapeScope(SecondTape<D>& tape) {
    SecondTape<D>*& slot = second_detail::Slot<D>();
    if (slot != nullptr) SecondOrderFail("a second second-order tape activated on one thread");
    slot = &tape;
  }
  ~SecondTapeScope() { second_detail::Slot<D>() = nullptr; }
  SecondTapeScope(const SecondTapeScope&) = delete;
  SecondTapeScope& operator=(const SecondTapeScope&) = delete;
  SecondTapeScope(SecondTapeScope&&) = delete;
  SecondTapeScope& operator=(SecondTapeScope&&) = delete;
};

// The operations of the active type, everything src/pricer/real_ops.hpp asks of a Real, with the
// partials real.hpp records, as Duals. A passive result is computed on the double alone, so its
// tangents stay zero.
template <class D>
SecondReal<D> operator+(const SecondReal<D>& a) {
  return a;
}
template <class D>
SecondReal<D> operator-(const SecondReal<D>& a) {
  if (!a.active()) return {-a.value()};
  return second_detail::Record(-a.dual(), a, D(-1.0));
}

template <class D>
SecondReal<D> operator+(const SecondReal<D>& a, const SecondReal<D>& b) {
  if (!a.active() && !b.active()) return {a.value() + b.value()};
  return second_detail::Record(a.dual() + b.dual(), a, D(1.0), b, D(1.0));
}
template <class D>
SecondReal<D> operator+(const SecondReal<D>& a, double b) {
  if (!a.active()) return {a.value() + b};
  return second_detail::Record(a.dual() + b, a, D(1.0));
}
template <class D>
SecondReal<D> operator+(double a, const SecondReal<D>& b) {
  if (!b.active()) return {a + b.value()};
  return second_detail::Record(a + b.dual(), b, D(1.0));
}

template <class D>
SecondReal<D> operator-(const SecondReal<D>& a, const SecondReal<D>& b) {
  if (!a.active() && !b.active()) return {a.value() - b.value()};
  return second_detail::Record(a.dual() - b.dual(), a, D(1.0), b, D(-1.0));
}
template <class D>
SecondReal<D> operator-(const SecondReal<D>& a, double b) {
  if (!a.active()) return {a.value() - b};
  return second_detail::Record(a.dual() - b, a, D(1.0));
}
template <class D>
SecondReal<D> operator-(double a, const SecondReal<D>& b) {
  if (!b.active()) return {a - b.value()};
  return second_detail::Record(a - b.dual(), b, D(-1.0));
}

template <class D>
SecondReal<D> operator*(const SecondReal<D>& a, const SecondReal<D>& b) {
  if (!a.active() && !b.active()) return {a.value() * b.value()};
  return second_detail::Record(a.dual() * b.dual(), a, b.dual(), b, a.dual());
}
template <class D>
SecondReal<D> operator*(const SecondReal<D>& a, double b) {
  if (!a.active()) return {a.value() * b};
  return second_detail::Record(a.dual() * b, a, D(b));
}
template <class D>
SecondReal<D> operator*(double a, const SecondReal<D>& b) {
  if (!b.active()) return {a * b.value()};
  return second_detail::Record(a * b.dual(), b, D(a));
}

template <class D>
SecondReal<D> operator/(const SecondReal<D>& a, const SecondReal<D>& b) {
  if (!a.active() && !b.active()) return {a.value() / b.value()};
  return second_detail::Record(a.dual() / b.dual(), a, 1.0 / b.dual(), b,
                               -a.dual() / (b.dual() * b.dual()));
}
template <class D>
SecondReal<D> operator/(const SecondReal<D>& a, double b) {
  if (!a.active()) return {a.value() / b};
  return second_detail::Record(a.dual() / b, a, D(1.0 / b));
}
template <class D>
SecondReal<D> operator/(double a, const SecondReal<D>& b) {
  if (!b.active()) return {a / b.value()};
  return second_detail::Record(a / b.dual(), b, -a / (b.dual() * b.dual()));
}

template <class D>
SecondReal<D>& operator+=(SecondReal<D>& a, const SecondReal<D>& b) {
  return a = a + b;
}
template <class D>
SecondReal<D>& operator-=(SecondReal<D>& a, const SecondReal<D>& b) {
  return a = a - b;
}
template <class D>
SecondReal<D>& operator*=(SecondReal<D>& a, const SecondReal<D>& b) {
  return a = a * b;
}
template <class D>
SecondReal<D>& operator/=(SecondReal<D>& a, const SecondReal<D>& b) {
  return a = a / b;
}

template <class D>
bool operator==(const SecondReal<D>& a, const SecondReal<D>& b) {
  return a.value() == b.value();
}
template <class D>
std::partial_ordering operator<=>(const SecondReal<D>& a, const SecondReal<D>& b) {
  return a.value() <=> b.value();
}
template <class D>
bool operator==(const SecondReal<D>& a, double b) {
  return a.value() == b;
}
template <class D>
std::partial_ordering operator<=>(const SecondReal<D>& a, double b) {
  return a.value() <=> b;
}

template <class D>
SecondReal<D> Exp(const SecondReal<D>& x) {
  if (!x.active()) return {dual_detail::ExpOf(x.value())};
  const D value = Exp(x.dual());
  return second_detail::Record(value, x, value);
}
template <class D>
SecondReal<D> Log(const SecondReal<D>& x) {
  if (!x.active()) return {dual_detail::LogOf(x.value())};
  return second_detail::Record(Log(x.dual()), x, 1.0 / x.dual());
}
template <class D>
SecondReal<D> Sqrt(const SecondReal<D>& x) {
  if (!x.active()) return {dual_detail::SqrtOf(x.value())};
  const D value = Sqrt(x.dual());
  return second_detail::Record(value, x, 0.5 / value);
}
template <class D>
SecondReal<D> NormalCdf(const SecondReal<D>& x) {
  if (!x.active()) return {dual_detail::NormalCdfOf(x.value())};
  return second_detail::Record(NormalCdf(x.dual()), x, NormalPdf(x.dual()));
}
template <class D>
SecondReal<D> InverseNormal(const SecondReal<D>& p) {
  if (!p.active()) return {dual_detail::InverseNormalOf(p.value())};
  const D value = InverseNormal(p.dual());
  return second_detail::Record(value, p, 1.0 / NormalPdf(value));
}

template <class D>
double Value(const SecondReal<D>& x) {
  return x.value();
}

// Lane types of the passes this file instantiates, and their absolute forms for error bounds.
template <std::size_t N>
using HessianDual = Dual<double, N>;
template <std::size_t N>
using AbsoluteHessianDual = Dual<double, N, true>;

// The refusal of a second-order run: a contract or run the pricer refuses, or a treatment with no
// second-order form (pricer::ValidateConditional).
struct SecondOrderError {
  pricer::PriceError price;
  pricer::ConditionalError conditional = pricer::ConditionalError::kNone;
};

// One worker's second-order tape for one contract, on the conditional kernels. Like PathGradient,
// it activates its tape on the constructing thread, and must be built, used and destroyed there.
// directions[k] is the input lane k is seeded on.
template <class D>
class SecondOrderPath {
 public:
  static constexpr std::size_t kLanes = D::kLanes;
  using Directions = std::array<std::size_t, kLanes>;
  using Rows = std::array<std::array<double, kInputs>, kLanes>;

  // Precondition: Validate and ValidateConditional passed.
  SecondOrderPath(const pricer::Contract& contract, const pricer::Market<double>& market,
                  const Directions& directions);

  // Records one path after the mark, sweeps it back to the mark from (1, 0, ...) and rewinds.
  // Returns the discounted payoff.
  double Accumulate(const pricer::PathUniforms& u);
  // The set-up sweep: summed over every path accumulated since the last call, the gradient
  // (value lanes of the input adjoints) and the Hessian rows (lane k of input i's adjoint).
  struct Sums {
    std::array<double, kInputs> gradient{};
    Rows rows{};
  };
  [[nodiscard]] Sums SweepSetup();

  // One path alone, for checks: one sweep from the end to the start that keeps every adjoint.
  // Starts and ends with the set-up adjoints clear.
  struct PathShapeReport {
    double value = 0.0;
    std::size_t statements = 0;  // Set-up and path.
    std::size_t operands = 0;
    double smallest_partial = 0.0;  // Smallest nonzero partial lane on the tape
    double smallest_adjoint = 0.0;  // and adjoint lane after the sweep
  };
  [[nodiscard]] Sums EvaluateOne(const pricer::PathUniforms& u, PathShapeReport& shape);

  [[nodiscard]] SecondReal<D> Record(const pricer::PathUniforms& u);
  void Rewind() { tape_.Rewind(mark_); }
  [[nodiscard]] SecondTape<D>& tape() { return tape_; }
  [[nodiscard]] const SecondTape<D>& tape() const { return tape_; }
  [[nodiscard]] SecondPosition mark() const { return mark_; }

 private:
  [[nodiscard]] Sums ReadInputs() const;

  pricer::Contract contract_;
  SecondTape<D> tape_;
  SecondTapeScope<D> scope_;
  std::array<SecondReal<D>, kInputs> inputs_;
  pricer::GbmModel<SecondReal<D>> model_;
  SecondPosition mark_;
};

template <std::size_t N>
struct HessianEstimate {
  pricer::Estimate value;
  std::array<pricer::Estimate, kInputs> greeks;  // As GradientEstimate: theta = -dV/dT.
  // rows[k][i] = d^2 V / d x_{directions[k]} d x_i, raw: no theta sign.
  std::array<std::array<pricer::Estimate, kInputs>, N> rows;
  std::array<std::size_t, N> directions{};
};

// Value, the five first-order Greeks and N Hessian rows from one forward-over-reverse pass per
// path. Instantiated for N = 1 and 2.
template <std::size_t N>
std::expected<HessianEstimate<N>, SecondOrderError> PriceWithHessian(
    const pricer::Contract& contract, const pricer::Market<double>& market,
    const pricer::RunConfig& config, const std::array<std::size_t, N>& directions);

// The first-order tape on the conditional kernels: what the second-order pass's first-order part
// costs alone, for the benchmark, and the tape whose Greeks the checks difference.
std::expected<GradientEstimate, SecondOrderError> PriceWithConditionalGradient(
    const pricer::Contract& contract, const pricer::Market<double>& market,
    const pricer::RunConfig& config);

// The double conditional kernel through the pricer's runner: the value every second-order and
// conditional first-order run must reproduce, bit for bit.
std::expected<pricer::Estimate, SecondOrderError> PriceConditional(
    const pricer::Contract& contract, const pricer::Market<double>& market,
    const pricer::RunConfig& config);

// One worker's first-order tape on the conditional kernels, as PathGradient on the production
// kernels.
class ConditionalPathGradient {
 public:
  ConditionalPathGradient(const pricer::Contract& contract, const pricer::Market<double>& market);
  double Accumulate(const pricer::PathUniforms& u);
  [[nodiscard]] std::array<double, kInputs> SweepSetup();
  // One path alone, as PathGradient::Evaluate: value, gradient, the absolute sweep, the underflow
  // weights and the tape size. Starts and ends with the set-up adjoints clear.
  [[nodiscard]] PathGradient::PathResult Evaluate(const pricer::PathUniforms& u);

 private:
  [[nodiscard]] std::array<double, kInputs> ReadInputs() const;

  pricer::Contract contract_;
  Tape tape_;
  TapeScope scope_;
  std::array<Real, kInputs> inputs_;
  pricer::GbmModel<Real> model_;
  Position mark_;
};

// One path's forward over reverse with its absolute counterpart, for the round-off checks.
template <std::size_t N>
class HessianCheckPath {
 public:
  HessianCheckPath(const pricer::Contract& contract, const pricer::Market<double>& market,
                   const std::array<std::size_t, N>& directions);

  struct Result {
    double value = 0.0;
    std::array<double, kInputs> gradient{};
    std::array<std::array<double, kInputs>, N> rows{};
    // The same sweep on absolute values: lane k of input i is the sum of |terms| of rows[k][i].
    std::array<std::array<double, kInputs>, N> absolute_rows{};
    std::size_t statements = 0;  // Set-up and path, on the second-order tape.
    std::size_t operands = 0;
    // Smallest nonzero magnitude of any partial lane, and of any absolute adjoint lane: the
    // normal-range precondition of the bound (tests/ad/second_order_test.cpp).
    double smallest_partial = 0.0;
    double smallest_adjoint = 0.0;
  };
  [[nodiscard]] Result Evaluate(const pricer::PathUniforms& u);

 private:
  SecondOrderPath<HessianDual<N>> path_;
  SecondOrderPath<AbsoluteHessianDual<N>> absolute_;
};

// The round-off bound of one entry of a forward-over-reverse row against the exact result of the
// same algorithm on the same recorded data, in the normal range (second_order.cpp states the
// count): gamma_n |terms| (1 + 2 gamma_n), n = 4 T + 2 O + 32.
double HessianRoundoffBound(double absolute, std::size_t statements, std::size_t operands);

}  // namespace aadp::ad

#endif  // AADP_AD_SECOND_ORDER_HPP_
