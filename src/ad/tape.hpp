// Tape v1, namespace aadp::ad: one node per operation, Jacobian taping with linear indexing.
//
// Layout (BENCHMARKS.md MEM-1 and MEM-2, the design of Sagebaum, Albring and Gauger, arXiv
// 1709.07229, section 4, written here from the paper, not from any code):
//   Real        16 bytes: the value and a 4-byte index, padded. Index 0 is passive.
//   statement    1 byte: its operand count. Its result index is its position, so nothing else
//                is stored (linear indexing).
//   operand     12 bytes: an 8-byte partial in one array and a 4-byte index in another
//                (struct of arrays, so neither pays padding).
// So a statement with k operands costs 1 + 12 k bytes on the tape, MEM-2's 12 k + 1. The adjoint
// vector, 8 bytes per statement, exists only for the sweep and is reported separately.
//
// Storage is chunked: statement and operand chunks of kChunkEntries are allocated once and reused,
// so nothing relocates while recording and nothing is allocated once the tape has reached its
// longest path (MEM-6). A statement's operands never straddle two chunks.
//
// Mark and rewind. GetPosition() is a mark; Rewind(mark) drops what came after it and keeps the
// memory. The pricer records its set-up (the market inputs and the model built from them) once,
// marks, and then records, sweeps and rewinds one path at a time, so the tape never holds more
// than the set-up and one path.
//
// Sweeps. SweepBack(from, stop) propagates adjoints from the statements in [stop, from), last
// first, then zeroes each one it propagated. Independent variables (statements with no operand)
// keep theirs: that is the result. A path's sweep stops at the mark, so its adjoints add into the
// set-up's, and one sweep from the mark to the start serves every path accumulated since.
//
// Threads. Each thread has at most one active tape, set by a TapeScope, and an operation on an
// active Real records onto it. A second activation on a thread is an error (EVIDENCE B10).
#ifndef AADP_AD_TAPE_HPP_
#define AADP_AD_TAPE_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

namespace aadp::ad {

using Index = std::uint32_t;
using OperandCount = std::uint8_t;
inline constexpr Index kPassiveIndex = 0;

// 2^13 entries: a statement chunk is 8 KiB and an operand chunk 96 KiB, so the 252-step paths of
// this pricer fit in one of each, and a short path does not reserve megabytes. Not yet tuned by
// a sweep (EVIDENCE B4); the benchmark stage does that.
inline constexpr std::size_t kChunkShift = 13;
inline constexpr std::size_t kChunkEntries = std::size_t{1} << kChunkShift;
inline constexpr std::size_t kChunkMask = kChunkEntries - 1;

inline constexpr std::size_t kStatementBytes = sizeof(OperandCount);
inline constexpr std::size_t kOperandBytes = sizeof(double) + sizeof(Index);
static_assert(kStatementBytes <= 8, "MEM-1: a statement is at most 8 bytes");
static_assert(kOperandBytes <= 12, "MEM-1: an operand is at most 12 bytes");

class Tape;

// A chunk is a fixed buffer of kChunkEntries, allocated once and never resized, so it is an
// owning pointer to an array: a vector would carry a size and a capacity it never uses.
template <class T>
using ChunkBuffer = std::unique_ptr<T[]>;  // NOLINT(modernize-avoid-c-arrays): see above.

template <class T>
ChunkBuffer<T> NewChunk() {
  return std::make_unique<T[]>(kChunkEntries);  // NOLINT(modernize-avoid-c-arrays): see above.
}

// The active type. A Real made from a double is passive: it records nothing, and an operation
// whose operands are all passive records nothing either.
class Real {
 public:
  constexpr Real() = default;
  // Implicit, so that kernels mix Real and double freely; a double is a constant.
  // NOLINTNEXTLINE(cppcoreguidelines-explicit-constructor,misc-explicit-constructor): see above.
  constexpr Real(double value) : value_(value) {}

  [[nodiscard]] constexpr double value() const { return value_; }
  [[nodiscard]] constexpr Index index() const { return index_; }
  [[nodiscard]] constexpr bool active() const { return index_ != kPassiveIndex; }

 private:
  friend class Tape;
  constexpr Real(double value, Index index) : value_(value), index_(index) {}

  double value_ = 0.0;
  Index index_ = kPassiveIndex;
};
static_assert(sizeof(Real) == 16, "MEM-1: the active type is 16 bytes");
static_assert(std::is_trivially_copyable_v<Real>);

struct Position {
  std::size_t statements = 0;  // Statements recorded, the reserved passive slot 0 included.
  std::size_t operand_chunk = 0;
  std::size_t operand_offset = 0;

  friend bool operator==(const Position&, const Position&) = default;
};

// What the tape holds, reported by the tape itself (EVIDENCE B3).
struct MemoryReport {
  std::size_t statements = 0;  // Excluding the reserved slot.
  std::size_t operands = 0;
  std::size_t statement_bytes = 0;  // statements * kStatementBytes
  std::size_t operand_bytes = 0;    // operands * kOperandBytes
  std::size_t adjoint_bytes = 0;    // statements * 8: the adjoint vector the sweep reads
  std::size_t allocated_bytes = 0;  // every chunk, adjoint and tangent buffer the tape owns
};

class Tape {
 public:
  Tape();
  ~Tape();
  Tape(const Tape&) = delete;
  Tape& operator=(const Tape&) = delete;
  Tape(Tape&&) = delete;
  Tape& operator=(Tape&&) = delete;

  // A new independent variable.
  [[nodiscard]] Real Register(double value);

  [[nodiscard]] Position GetPosition() const {
    return {.statements = statements_, .operand_chunk = op_chunk_, .operand_offset = op_offset_};
  }
  // Drops every statement after the mark. Their adjoints must be zero, as a sweep leaves them.
  void Rewind(const Position& mark);
  [[nodiscard]] std::size_t Statements() const { return statements_ - 1; }

  [[nodiscard]] double Adjoint(const Real& x) const { return adjoints_[x.index()]; }
  // A passive x has no adjoint, so seeding it does nothing.
  void AddAdjoint(const Real& x, double adjoint) {
    if (x.active()) adjoints_[x.index()] += adjoint;
  }
  [[nodiscard]] double AdjointAt(Index i) const { return adjoints_[i]; }
  // Zeroes the adjoints of statements [stop, from).
  void ClearAdjoints(const Position& from, const Position& stop);

  // Reverse sweep over [stop, from), described above.
  void SweepBack(const Position& from, const Position& stop);
  // The same sweep on |partial| from |seed|, for error bounds. It keeps every adjoint it computes,
  // so the caller can read them, and must clear them with ClearAdjoints.
  void SweepBackAbsolute(const Position& from, const Position& stop);

  // Forward tangent sweep over [from, end). Tangents of independent variables are the seeds,
  // every other tangent is overwritten. Tangent storage is allocated on first use.
  void SweepForward(const Position& from);
  void SetTangent(const Real& x, double tangent);
  [[nodiscard]] double Tangent(const Real& x) const;
  void ClearTangents();

  [[nodiscard]] MemoryReport Memory() const;

  // Recording, for the operations in real.hpp. a and b are active indices.
  Real Push(double value, Index a, double da) {
    if (op_offset_ + 1 > kChunkEntries) [[unlikely]] {
      NextOperandChunk();
    }
    partials_[op_offset_] = da;
    indices_[op_offset_] = a;
    op_offset_ += 1;
    return PushStatement(value, 1);
  }
  Real Push(double value, Index a, double da, Index b, double db) {
    if (op_offset_ + 2 > kChunkEntries) [[unlikely]] {
      NextOperandChunk();
    }
    partials_[op_offset_] = da;
    indices_[op_offset_] = a;
    partials_[op_offset_ + 1] = db;
    indices_[op_offset_ + 1] = b;
    op_offset_ += 2;
    return PushStatement(value, 2);
  }

 private:
  struct OperandChunk {
    ChunkBuffer<double> partials;
    ChunkBuffer<Index> indices;
    std::size_t used = 0;  // Valid for chunks before the current one.
  };

  Real PushStatement(double value, OperandCount count) {
    if ((statements_ & kChunkMask) == 0) [[unlikely]] {
      EnterStatementChunk();
    }
    counts_[statements_ & kChunkMask] = count;
    const auto index = static_cast<Index>(statements_);
    statements_ += 1;
    return {value, index};
  }

  void EnterStatementChunk();
  void NextOperandChunk();
  void SelectOperandChunk(std::size_t chunk);
  [[nodiscard]] OperandCount CountAt(std::size_t statement) const {
    return statement_chunks_[statement >> kChunkShift][statement & kChunkMask];
  }
  [[nodiscard]] std::size_t UsedOf(std::size_t chunk) const {
    return chunk == op_chunk_ ? op_offset_ : operand_chunks_[chunk].used;
  }
  template <bool kAbsolute>
  void Sweep(const Position& from, const Position& stop);

  std::vector<ChunkBuffer<OperandCount>> statement_chunks_;
  std::vector<OperandChunk> operand_chunks_;
  std::vector<double> adjoints_;  // One per statement slot of every allocated chunk.
  std::vector<double> tangents_;

  std::size_t statements_ = 0;
  OperandCount* counts_ = nullptr;  // The chunk statements_ falls in.
  std::size_t op_chunk_ = 0;
  std::size_t op_offset_ = 0;
  double* partials_ = nullptr;
  Index* indices_ = nullptr;
};

namespace detail {

inline Tape*& ActiveSlot() {
  thread_local Tape* slot = nullptr;
  return slot;
}

// An active Real with no active tape outlived its scope or crossed a thread.
[[noreturn]] void NoActiveTape();

}  // namespace detail

inline Tape& ActiveTape() {
  Tape* tape = detail::ActiveSlot();
  if (tape == nullptr) [[unlikely]] {
    detail::NoActiveTape();
  }
  return *tape;
}

// Makes a tape this thread's active tape for the scope's lifetime.
class TapeScope {
 public:
  explicit TapeScope(Tape& tape);
  ~TapeScope();
  TapeScope(const TapeScope&) = delete;
  TapeScope& operator=(const TapeScope&) = delete;
  TapeScope(TapeScope&&) = delete;
  TapeScope& operator=(TapeScope&&) = delete;
};

}  // namespace aadp::ad

#endif  // AADP_AD_TAPE_HPP_
