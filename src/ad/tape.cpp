#include "ad/tape.hpp"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>

namespace aadp::ad {

namespace {

[[noreturn]] void Fail(const char* what) {
  std::fprintf(stderr, "aadp::ad: %s\n", what);
  std::abort();
}

// Indices are 32 bits. One path of this pricer is a few thousand statements, so reaching this
// means a path that never rewinds.
constexpr std::size_t kMaxStatements = std::numeric_limits<Index>::max();

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

template <bool kAbsolute>
void Tape::Sweep(const Position& from, const Position& stop) {
  std::size_t chunk = from.operand_chunk;
  std::size_t offset = from.operand_offset;
  const double* partials = operand_chunks_[chunk].partials.get();
  const Index* indices = operand_chunks_[chunk].indices.get();
  double* adjoints = adjoints_.data();
  for (std::size_t i = from.statements; i-- > stop.statements;) {
    const OperandCount count = CountAt(i);
    if (count == 0) continue;
    const double adjoint = kAbsolute ? std::fabs(adjoints[i]) : adjoints[i];
    if constexpr (!kAbsolute) adjoints[i] = 0.0;
    if (offset == 0) {
      chunk -= 1;
      offset = UsedOf(chunk);
      partials = operand_chunks_[chunk].partials.get();
      indices = operand_chunks_[chunk].indices.get();
    }
    offset -= count;
    for (std::size_t j = 0; j < count; ++j) {
      const double partial = kAbsolute ? std::fabs(partials[offset + j]) : partials[offset + j];
      adjoints[indices[offset + j]] += adjoint * partial;
    }
  }
}

void Tape::SweepBack(const Position& from, const Position& stop) { Sweep<false>(from, stop); }

void Tape::SweepBackAbsolute(const Position& from, const Position& stop) {
  Sweep<true>(from, stop);
}

void Tape::SweepForward(const Position& from) {
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
    for (std::size_t j = 0; j < count; ++j) {
      tangent += partials[offset + j] * tangents[indices[offset + j]];
    }
    tangents[i] = tangent;
    offset += count;
  }
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

TapeScope::TapeScope(Tape& tape) {
  Tape*& slot = detail::ActiveSlot();
  if (slot != nullptr) Fail("a second tape activated on one thread");
  slot = &tape;
}

TapeScope::~TapeScope() { detail::ActiveSlot() = nullptr; }

}  // namespace aadp::ad
