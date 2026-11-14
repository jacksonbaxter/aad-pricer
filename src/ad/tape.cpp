#include "ad/tape.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <span>
#include <thread>

#include "tape_config.hpp"

namespace aadp::ad {

namespace {

[[noreturn]] void Fail(const char* what) {
  std::fprintf(stderr, "aadp::ad: %s\n", what);
  std::abort();
}

// Indices are 32 bits. One path of this pricer is a few thousand statements, so reaching this
// means a path that never rewinds.
constexpr std::size_t kMaxStatements = std::numeric_limits<Index>::max();

// What a reverse sweep adds for one operand: the statement's adjoint times the partial, or its
// magnitude for the absolute sweep (whose adjoint is already a magnitude). Every path of Sweep
// adds this and nothing else.
template <bool kAbsolute>
double Contribution(double adjoint, double partial) {
  return adjoint * (kAbsolute ? std::fabs(partial) : partial);
}

// Adds one statement's contributions to its operands' adjoints, first operand first, straight-line
// for one and two operands and by a loop for more. With kCarry, returns the adjoint of statement
// `previous` as it then stands: the last sum stored there, or `held` if no operand is that
// statement; without, returns `held`.
template <bool kAbsolute, bool kCarry>
[[gnu::always_inline]] inline double Propagate(double* adjoints, const Index* x, const double* p,
                                               std::size_t count, double adjoint,
                                               std::size_t previous, double held) {
  const auto add = [&](Index t, double partial) {
    const double sum = adjoints[t] + Contribution<kAbsolute>(adjoint, partial);
    adjoints[t] = sum;
    if constexpr (kCarry) held = t == previous ? sum : held;
  };
  if (count == 2) {
    add(x[0], p[0]);
    add(x[1], p[1]);
  } else if (count == 1) {
    add(x[0], p[0]);
  } else {
    for (std::size_t j = 0; j < count; ++j) add(x[j], p[j]);
  }
  return held;
}

}  // namespace

namespace detail {

void NoActiveTape() { Fail("an operation on an active Real with no active tape on this thread"); }

}  // namespace detail

Tape::Tape() {
  SelectOperandChunk(0);
  // Slot 0 is the passive index: a statement with no operands that nothing ever refers to.
  static_cast<void>(PushStatement(0.0, 0));
}

Tape::~Tape() = default;

Real Tape::Register(double value) { return PushStatement(value, 0); }

Real Tape::PushStatement(double value, OperandCount count) {
  if ((statements_ & kChunkMask) == 0) EnterStatementChunk();
  counts_[statements_ & kChunkMask] = count;
  const auto index = static_cast<Index>(statements_);
  statements_ += 1;
  return {value, index};
}

Real Tape::PushEnteringChunk(double value, Index a, double da) {
  if (!OperandsFit(op_offset_, 1)) NextOperandChunk();
  partials_[op_offset_] = da;
  indices_[op_offset_] = a;
  op_offset_ += 1;
  return PushStatement(value, 1);
}

Real Tape::PushEnteringChunk(double value, Index a, double da, Index b, double db) {
  if (!OperandsFit(op_offset_, 2)) NextOperandChunk();
  partials_[op_offset_] = da;
  indices_[op_offset_] = a;
  partials_[op_offset_ + 1] = db;
  indices_[op_offset_ + 1] = b;
  op_offset_ += 2;
  return PushStatement(value, 2);
}

Real Tape::Push(double value, std::span<const Index> operands, std::span<const double> partials) {
  const std::size_t count = operands.size();
  if (count == 0 || count > kMaxOperands || partials.size() != count) {
    Fail("a statement takes 1 to 255 operands, each with one partial");
  }
  if (!OperandsFit(op_offset_, count)) NextOperandChunk();
  std::ranges::copy(partials, partials_ + op_offset_);
  std::ranges::copy(operands, indices_ + op_offset_);
  op_offset_ += count;
  return PushStatement(value, static_cast<OperandCount>(count));
}

void Tape::EnterStatementChunk() {
  const std::size_t chunk = statements_ >> kChunkShift;
  if (chunk == statement_chunks_.size()) {
    if (statements_ >= kMaxStatements - kChunkEntries) Fail("tape index space exhausted");
    statement_chunks_.push_back(NewChunk<OperandCount>());
    adjoints_.resize(statement_chunks_.size() * kChunkEntries, 0.0);
    if (!tangents_.empty()) tangents_.resize(adjoints_.size(), 0.0);
  }
  counts_ = statement_chunks_[chunk].get();
}

void Tape::SelectOperandChunk(std::size_t chunk) {
  if (chunk == operand_chunks_.size()) {
    operand_chunks_.push_back(
        {.partials = NewChunk<double>(), .indices = NewChunk<Index>(), .used = 0});
  }
  op_chunk_ = chunk;
  partials_ = operand_chunks_[chunk].partials.get();
  indices_ = operand_chunks_[chunk].indices.get();
}

void Tape::NextOperandChunk() {
  operand_chunks_[op_chunk_].used = op_offset_;
  SelectOperandChunk(op_chunk_ + 1);
  op_offset_ = 0;
}

void Tape::Rewind(const Position& mark) {
  statements_ = mark.statements;
  if ((statements_ & kChunkMask) != 0) {
    counts_ = statement_chunks_[statements_ >> kChunkShift].get();
  }
  SelectOperandChunk(mark.operand_chunk);
  op_offset_ = mark.operand_offset;
}

void Tape::ClearAdjoints(const Position& from, const Position& stop) {
  for (std::size_t i = stop.statements; i < from.statements; ++i) adjoints_[i] = 0.0;
}

// Two things make the sweep faster than one generic loop over each statement's operands, and
// neither changes an operation or the order of any two (docs/NOTES.md, "Stage 11"):
// - Statements of one and two operands, all that real.hpp records, are straight-line code, and
//   wider ones take the loop. Each adds its operands' contributions first operand first.
// - The adjoint of the statement before, the next one swept, is carried in a register. A
//   statement's result is most often an operand of the one recorded after it, so without this
//   the sweep would read that adjoint back from memory just after storing it, every statement.
//   `held` is adjoints[i] at the top of each iteration: loaded as statement i + 1 begins, then
//   replaced by each sum statement i + 1 stores there. Memory is written exactly as before, so
//   `held` is only a copy of what memory holds. The skip sweep does not carry it and reads each
//   adjoint from memory: on Zen 1 the carry made the skip slower.
template <bool kAbsolute, bool kSkipZero>
void Tape::Sweep(const Position& from, const Position& stop) {
  constexpr bool kCarry = !kSkipZero;
  swept_.statements += from.statements - stop.statements;
  const std::size_t setup_top = std::min(from.statements, setup_end_);
  if (setup_top > stop.statements) swept_.setup_statements += setup_top - stop.statements;
  if (from.statements <= stop.statements) return;
  std::size_t chunk = from.operand_chunk;
  std::size_t offset = from.operand_offset;
  const double* partials = operand_chunks_[chunk].partials.get();
  const Index* indices = operand_chunks_[chunk].indices.get();
  double* adjoints = adjoints_.data();
  double held = kCarry ? adjoints[from.statements - 1] : 0.0;
  for (std::size_t i = from.statements; i-- > stop.statements;) {
    const OperandCount count = CountAt(i);
    if (count == 0) {
      // An independent variable keeps its adjoint; the statement before it needs its own.
      if (kCarry && i != 0) held = adjoints[i - 1];
      continue;
    }
    const double own = kCarry ? held : adjoints[i];
    const double adjoint = kAbsolute ? std::fabs(own) : own;
    if constexpr (!kAbsolute) adjoints[i] = 0.0;
    const std::size_t previous = i - 1;  // Statement 0 is the passive slot, with no operands.
    if constexpr (kCarry) held = adjoints[previous];
    if (offset == 0) {
      chunk -= 1;
      offset = UsedOf(chunk);
      partials = operand_chunks_[chunk].partials.get();
      indices = operand_chunks_[chunk].indices.get();
    }
    offset -= count;
    if constexpr (kSkipZero) {
      if (adjoint == 0.0) continue;
    }
    held = Propagate<kAbsolute, kCarry>(adjoints, indices + offset, partials + offset, count,
                                        adjoint, previous, held);
  }
}

void Tape::SweepBack(const Position& from, const Position& stop) { Sweep<false>(from, stop); }

void Tape::SweepBackSkippingZeros(const Position& from, const Position& stop) {
  Sweep<false, true>(from, stop);
}

void Tape::SweepBackAbsolute(const Position& from, const Position& stop) {
  Sweep<true>(from, stop);
}

template <bool kAbsolute>
void Tape::Forward(const Position& from) {
  if (tangents_.size() != adjoints_.size()) tangents_.resize(adjoints_.size(), 0.0);
  std::size_t chunk = from.operand_chunk;
  std::size_t offset = from.operand_offset;
  double* tangents = tangents_.data();
  for (std::size_t i = from.statements; i < statements_; ++i) {
    const OperandCount count = CountAt(i);
    if (count == 0) continue;
    if (offset + count > UsedOf(chunk)) {
      chunk += 1;
      offset = 0;
    }
    const double* partials = operand_chunks_[chunk].partials.get();
    const Index* indices = operand_chunks_[chunk].indices.get();
    double tangent = 0.0;
    if constexpr (kAbsolute) {
      for (std::size_t j = 0; j < count; ++j) {
        tangent += std::fabs(partials[offset + j]) * std::fabs(tangents[indices[offset + j]]);
      }
    } else {
      for (std::size_t j = 0; j < count; ++j) {
        tangent += partials[offset + j] * tangents[indices[offset + j]];
      }
    }
    tangents[i] = tangent;
    offset += count;
  }
}

void Tape::SweepForward(const Position& from) { Forward<false>(from); }

void Tape::SweepForwardAbsolute(const Position& from) { Forward<true>(from); }

Tape::OperandSums Tape::SumOverOperands(const Position& from, const Position& stop) const {
  OperandSums sums;
  std::size_t chunk = stop.operand_chunk;
  std::size_t offset = stop.operand_offset;
  const bool tangents = tangents_.size() == adjoints_.size();
  for (std::size_t i = stop.statements; i < from.statements; ++i) {
    const OperandCount count = CountAt(i);
    if (count == 0) continue;
    if (offset + count > UsedOf(chunk)) {
      chunk += 1;
      offset = 0;
    }
    const Index* indices = operand_chunks_[chunk].indices.get();
    for (std::size_t j = 0; j < count; ++j) {
      sums.adjoints += std::fabs(adjoints_[i]);
      if (tangents) sums.tangents += std::fabs(tangents_[indices[offset + j]]);
    }
    offset += count;
  }
  return sums;
}

void Tape::SetTangent(const Real& x, double tangent) {
  if (tangents_.size() != adjoints_.size()) tangents_.resize(adjoints_.size(), 0.0);
  if (x.active()) tangents_[x.index()] = tangent;
}

double Tape::Tangent(const Real& x) const {
  return x.active() && x.index() < tangents_.size() ? tangents_[x.index()] : 0.0;
}

void Tape::ClearTangents() {
  for (double& t : tangents_) t = 0.0;
}

MemoryReport Tape::Memory() const {
  MemoryReport report;
  report.statements = Statements();
  for (std::size_t c = 0; c <= op_chunk_; ++c) report.operands += UsedOf(c);
  report.statement_bytes = report.statements * kStatementBytes;
  report.operand_bytes = report.operands * kOperandBytes;
  report.adjoint_bytes = report.statements * sizeof(double);
  report.allocated_bytes = (statement_chunks_.size() * kChunkEntries * kStatementBytes) +
                           (operand_chunks_.size() * kChunkEntries * kOperandBytes) +
                           (adjoints_.capacity() * sizeof(double)) +
                           (tangents_.capacity() * sizeof(double));
  return report;
}

namespace detail {

void ClaimStaticSlot() {
  static std::atomic<std::thread::id> owner{};
  std::thread::id expected{};
  const std::thread::id self = std::this_thread::get_id();
  if (!owner.compare_exchange_strong(expected, self) && expected != self) {
    Fail(
        "a tape activated on a second thread, in a build whose active tape is a plain static "
        "(AAD_PRICER_TAPE_SLOT_STATIC), which is for a single-threaded timing only");
  }
}

}  // namespace detail

TapeScope::TapeScope(Tape& tape) {
#if AADP_TAPE_SLOT_STATIC
  detail::ClaimStaticSlot();
#endif
  Tape*& slot = detail::ActiveSlot();
  if (slot != nullptr) Fail("a second tape activated on one thread");
  slot = &tape;
}

TapeScope::~TapeScope() { detail::ActiveSlot() = nullptr; }

}  // namespace aadp::ad
