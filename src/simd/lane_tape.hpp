// The vector tape: one tape per group of N lanes, namespace aadp::simd (docs/NOTES.md, "Vector
// tape: design and oracle"). Statements and operand indices are recorded once per group; each lane
// has its own values, partials and adjoints, and the reverse sweep runs across the lanes.
//
// Layout, per entry, for N = MaxLanes(D):
//   LaneTapeReal  the N values as one vector, a 4-byte index and a 1-byte lane mask, padded to
//                 the vector's alignment
//   statement     1 byte: its operand count, shared by the lanes (linear indexing, as tape.hpp)
//   operand       a 4-byte index, for N > 1 a 1-byte lane mask, both shared, and N 8-byte partials
//                 (struct of arrays). 12 bytes at one lane, which needs no mask; 5 + 8 N above
//   adjoint       N doubles per statement, for the sweep only
// Chunks are tape.hpp's: kChunkEntries statements or operands, allocated once, never resized.
//
// Lane masks. A Real carries the lanes in which it is active, and an operand stores its Real's
// mask. The scalar tape records an operand only where it is active, and Select returns the chosen
// operand itself, so a path's statements depend on its Selects. Here a lane whose bit is clear
// skips the operand, by a select rather than a multiply by zero, so lane i of a sweep does what the
// scalar tape does on path i, in its order, and nothing more.
//
// Indices below base() are another tape's: the set-up, which stays on the worker's scalar tape.
// The sweep never adds into them. It appends each such contribution to the boundary buffer, in
// sweep order, and the caller adds them into the scalar tape lane by lane, in path order
// (lane_path_gradient.hpp).
//
// Divergence. A Select in which lanes choose two different active statements has no single index.
// lane_tape_ops.hpp marks the group divergent there, and the caller drops the group and runs its
// paths on the scalar tape instead.
#ifndef AADP_SIMD_LANE_TAPE_HPP_
#define AADP_SIMD_LANE_TAPE_HPP_

#include <hwy/highway.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "ad/tape.hpp"

HWY_BEFORE_NAMESPACE();

namespace aadp::simd {

namespace hn = hwy::HWY_NAMESPACE;

// Bit i is lane i. Eight lanes at most (AVX-512 doubles).
using LaneBits = std::uint8_t;

template <class D>
class LaneTape;

template <class D>
class LaneTapeReal {
 public:
  using Tag = D;
  using Vec = hn::Vec<D>;

  LaneTapeReal() : v_(hn::Zero(D())) {}
  // A constant in every lane: passive.
  explicit LaneTapeReal(double x) : v_(hn::Set(D(), x)) {}
  // Lane values, passive.
  explicit LaneTapeReal(Vec v) : v_(v) {}

  // A Real of the scalar tape below base(), the set-up: its value in every lane, active in all.
  static LaneTapeReal FromScalar(const ad::Real& x) {
    return x.active() ? LaneTapeReal(hn::Set(D(), x.value()), x.index(), kAll)
                      : LaneTapeReal(x.value());
  }
  // For the operations: lane values with an index and the lanes it is active in. No lane, no index.
  static LaneTapeReal Active(Vec v, ad::Index index, LaneBits lanes) {
    return lanes == 0 ? LaneTapeReal(v) : LaneTapeReal(v, index, lanes);
  }

  [[nodiscard]] Vec vec() const { return v_; }
  [[nodiscard]] ad::Index index() const { return index_; }
  [[nodiscard]] LaneBits lanes() const { return lanes_; }
  [[nodiscard]] bool active() const { return lanes_ != 0; }

 private:
  static constexpr LaneBits kAll = static_cast<LaneBits>((1U << hn::MaxLanes(D())) - 1U);

  LaneTapeReal(Vec v, ad::Index index, LaneBits lanes) : v_(v), index_(index), lanes_(lanes) {}

  Vec v_;
  ad::Index index_ = ad::kPassiveIndex;
  LaneBits lanes_ = 0;
};

namespace lane_tape_detail {

[[noreturn]] inline void Fail(const char* what) {
  std::fprintf(stderr, "aadp::simd: %s\n", what);
  std::abort();
}

// LoadMaskBits reads 8 bytes from where it starts, so a mask array ends 8 bytes later.
inline constexpr std::size_t kMaskPadding = 8;

}  // namespace lane_tape_detail

template <class D>
class LaneTape {
 public:
  static constexpr std::size_t kLanes = hn::MaxLanes(D());
  static_assert(kLanes >= 1 && kLanes <= 8, "a LaneBits holds at most 8 lanes");
  static constexpr bool kMasked = kLanes > 1;
  static constexpr LaneBits kAllLanes = static_cast<LaneBits>((1U << kLanes) - 1U);
  static constexpr std::size_t kStatementBytes = sizeof(ad::OperandCount);
  static constexpr std::size_t kOperandBytes =
      sizeof(ad::Index) + (kMasked ? sizeof(LaneBits) : 0) + (kLanes * sizeof(double));
  using Vec = hn::Vec<D>;
  using Real = LaneTapeReal<D>;
  static_assert(std::is_trivially_copyable_v<Real>);
  // The values, index and mask, padded to the vector: 16 bytes per lane, except one lane on x86,
  // where Highway keeps a lane in a 16-byte register (docs/NOTES.md).
  static_assert(sizeof(Real) <= 16 * kLanes || (kLanes == 1 && sizeof(Real) <= 32),
                "the lane active type is at most 16 bytes per lane");

  // Indices below base are the set-up's, on another tape. 1 for a tape of its own.
  explicit LaneTape(ad::Index base = 1) : base_(base) {
    if (base_ == ad::kPassiveIndex) lane_tape_detail::Fail("a lane tape's base must be above 0");
    SelectOperandChunk(0);
  }
  ~LaneTape() = default;
  LaneTape(const LaneTape&) = delete;
  LaneTape& operator=(const LaneTape&) = delete;
  LaneTape(LaneTape&&) = delete;
  LaneTape& operator=(LaneTape&&) = delete;

  [[nodiscard]] ad::Index base() const { return base_; }

  // A new independent variable, active in every lane.
  [[nodiscard]] Real Register(Vec values) { return PushStatement(values, 0, kAllLanes); }

  [[nodiscard]] ad::Position GetPosition() const {
    return {.statements = statements_, .operand_chunk = op_chunk_, .operand_offset = op_offset_};
  }
  // Drops every statement after the mark. Their adjoints must be zero, as a sweep leaves them.
  void Rewind(const ad::Position& mark) {
    statements_ = mark.statements;
    if ((statements_ & ad::kChunkMask) != 0) {
      counts_ = statement_chunks_[statements_ >> ad::kChunkShift].get();
    }
    SelectOperandChunk(mark.operand_chunk);
    op_offset_ = mark.operand_offset;
  }
  [[nodiscard]] std::size_t Statements() const { return statements_; }

  // Zero for a passive x or one below base.
  [[nodiscard]] Vec Adjoint(const Real& x) const {
    if (!x.active() || x.index() < base_) return hn::Zero(D());
    return hn::LoadU(D(), adjoints_.data() + Slot(x.index()));
  }
  // Adds seed in the lanes x is active in. Below base it goes to the boundary buffer.
  void AddAdjoint(const Real& x, Vec seed) {
    if (!x.active()) return;
    if (x.index() < base_) {
      AppendBoundary(x.index(), x.lanes(), seed);
      return;
    }
    AddMasked(adjoints_.data() + Slot(x.index()), x.lanes(), seed);
  }
  // Zeroes the adjoints of statements [stop, from).
  void ClearAdjoints(const ad::Position& from, const ad::Position& stop) {
    std::fill(adjoints_.begin() + static_cast<std::ptrdiff_t>(stop.statements * kLanes),
              adjoints_.begin() + static_cast<std::ptrdiff_t>(from.statements * kLanes), 0.0);
  }

  // Reverse sweep over [stop, from): tape.hpp's sweep on every lane at once. Each statement's
  // adjoint is read and zeroed, then each operand adds adjoint times partial into its target in
  // the lanes of its mask. A target below base goes to the boundary buffer instead.
  void SweepBack(const ad::Position& from, const ad::Position& stop) {
    const D d;
    std::size_t chunk = from.operand_chunk;
    std::size_t offset = from.operand_offset;
    const OperandChunk* operands = &operand_chunks_[chunk];
    double* adjoints = adjoints_.data();
    for (std::size_t i = from.statements; i-- > stop.statements;) {
      const ad::OperandCount count = CountAt(i);
      if (count == 0) continue;
      const Vec adjoint = hn::LoadU(d, adjoints + (i * kLanes));
      hn::StoreU(hn::Zero(d), d, adjoints + (i * kLanes));
      if (offset == 0) {
        chunk -= 1;
        offset = UsedOf(chunk);
        operands = &operand_chunks_[chunk];
      }
      offset -= count;
      for (std::size_t j = 0; j < count; ++j) {
        const std::size_t k = offset + j;
        const Vec contribution =
            hn::Mul(adjoint, hn::LoadU(d, operands->partials.get() + (k * kLanes)));
        const ad::Index target = operands->indices[k];
        if (target < base_) {
          if constexpr (kMasked) {
            AppendBoundary(target, operands->masks[k], contribution);
          } else {
            AppendBoundary(target, kAllLanes, contribution);
          }
          continue;
        }
        double* slot = adjoints + Slot(target);
        if constexpr (kMasked) {
          const Vec old = hn::LoadU(d, slot);
          const auto mask = hn::LoadMaskBits(d, operands->masks.get() + k);
          hn::StoreU(hn::IfThenElse(mask, hn::Add(old, contribution), old), d, slot);
        } else {
          hn::StoreU(hn::Add(hn::LoadU(d, slot), contribution), d, slot);
        }
      }
    }
  }

  // The boundary buffer: contributions to indices below base, in sweep order.
  [[nodiscard]] std::size_t BoundaryEntries() const { return boundary_count_; }
  // add(index, contribution) for each entry whose mask has lane, in sweep order.
  template <class Add>
  void ForEachBoundary(std::size_t lane, const Add& add) const {
    for (std::size_t e = 0; e < boundary_count_; ++e) {
      if (((static_cast<unsigned>(boundary_masks_[e]) >> lane) & 1U) != 0) {
        add(boundary_indices_[e], boundary_values_[(e * kLanes) + lane]);
      }
    }
  }
  void ClearBoundary() { boundary_count_ = 0; }

  // Forward tangent sweep over [from, end), tape.hpp's on every lane. Tangents of independent
  // variables are the seeds. setup(i) is the tangent of index i below base, the same in every
  // lane. Tangent storage is allocated on first use.
  template <class SetupTangent>
  void SweepForward(const ad::Position& from, const SetupTangent& setup) {
    const D d;
    if (tangents_.size() != adjoints_.size()) tangents_.resize(adjoints_.size(), 0.0);
    std::size_t chunk = from.operand_chunk;
    std::size_t offset = from.operand_offset;
    double* tangents = tangents_.data();
    for (std::size_t i = from.statements; i < statements_; ++i) {
      const ad::OperandCount count = CountAt(i);
      if (count == 0) continue;
      if (offset + count > UsedOf(chunk)) {
        chunk += 1;
        offset = 0;
      }
      const OperandChunk& operands = operand_chunks_[chunk];
      Vec tangent = hn::Zero(d);
      for (std::size_t j = 0; j < count; ++j) {
        const std::size_t k = offset + j;
        const ad::Index target = operands.indices[k];
        const Vec operand =
            target < base_ ? hn::Set(d, setup(target)) : hn::LoadU(d, tangents + Slot(target));
        const Vec sum = hn::Add(
            tangent, hn::Mul(hn::LoadU(d, operands.partials.get() + (k * kLanes)), operand));
        if constexpr (kMasked) {
          tangent = hn::IfThenElse(hn::LoadMaskBits(d, operands.masks.get() + k), sum, tangent);
        } else {
          tangent = sum;
        }
      }
      hn::StoreU(tangent, d, tangents + (i * kLanes));
      offset += count;
    }
  }
  void SetTangent(const Real& x, Vec tangent) {
    if (tangents_.size() != adjoints_.size()) tangents_.resize(adjoints_.size(), 0.0);
    if (x.active() && x.index() >= base_) {
      hn::StoreU(tangent, D(), tangents_.data() + Slot(x.index()));
    }
  }
  [[nodiscard]] Vec Tangent(const Real& x) const {
    if (!x.active() || x.index() < base_ || Slot(x.index()) >= tangents_.size()) {
      return hn::Zero(D());
    }
    return hn::LoadU(D(), tangents_.data() + Slot(x.index()));
  }
  void ClearTangents() { std::ranges::fill(tangents_, 0.0); }

  // Set by a Select whose lanes choose different active statements (lane_tape_ops.hpp).
  void MarkDivergent() { divergent_ = true; }
  [[nodiscard]] bool Divergent() const { return divergent_; }
  void ClearDivergence() { divergent_ = false; }

  // What the tape holds, in tape.hpp's terms: operand_bytes counts kOperandBytes per operand, and
  // allocated_bytes includes the boundary buffer.
  [[nodiscard]] ad::MemoryReport Memory() const {
    ad::MemoryReport report;
    report.statements = statements_;
    for (std::size_t c = 0; c <= op_chunk_; ++c) report.operands += UsedOf(c);
    report.statement_bytes = report.statements * kStatementBytes;
    report.operand_bytes = report.operands * kOperandBytes;
    report.adjoint_bytes = report.statements * kLanes * sizeof(double);
    const std::size_t mask_bytes =
        kMasked ? (ad::kChunkEntries + lane_tape_detail::kMaskPadding) * sizeof(LaneBits) : 0;
    report.allocated_bytes =
        (statement_chunks_.size() * ad::kChunkEntries * kStatementBytes) +
        (operand_chunks_.size() *
         ((ad::kChunkEntries * (sizeof(ad::Index) + (kLanes * sizeof(double)))) + mask_bytes)) +
        ((adjoints_.capacity() + tangents_.capacity() + boundary_values_.capacity()) *
         sizeof(double)) +
        (boundary_indices_.capacity() * sizeof(ad::Index)) +
        (boundary_masks_.capacity() * sizeof(LaneBits));
    return report;
  }

  // Recording, for lane_tape_ops.hpp. a and b are active indices, la and lb their lanes.
  Real Push(Vec value, ad::Index a, LaneBits la, Vec da) {
    if (op_offset_ + 1 > ad::kChunkEntries) [[unlikely]] {
      NextOperandChunk();
    }
    PutOperand(op_offset_, a, la, da);
    op_offset_ += 1;
    return PushStatement(value, 1, la);
  }
  Real Push(Vec value, ad::Index a, LaneBits la, Vec da, ad::Index b, LaneBits lb, Vec db) {
    if (op_offset_ + 2 > ad::kChunkEntries) [[unlikely]] {
      NextOperandChunk();
    }
    PutOperand(op_offset_, a, la, da);
    PutOperand(op_offset_ + 1, b, lb, db);
    op_offset_ += 2;
    return PushStatement(value, 2, static_cast<LaneBits>(la | lb));
  }

 private:
  struct OperandChunk {
    ad::ChunkBuffer<double> partials;  // kLanes per operand
    ad::ChunkBuffer<ad::Index> indices;
    ad::ChunkBuffer<LaneBits> masks;  // Only when kMasked, with kMaskPadding bytes after.
    std::size_t used = 0;             // Valid for chunks before the current one.
  };

  [[nodiscard]] std::size_t Slot(ad::Index index) const {
    return static_cast<std::size_t>(index - base_) * kLanes;
  }

  void PutOperand(std::size_t k, ad::Index index, LaneBits lanes, Vec partial) {
    hn::StoreU(partial, D(), partials_ + (k * kLanes));
    indices_[k] = index;
    if constexpr (kMasked) {
      masks_[k] = lanes;
    } else {
      static_cast<void>(lanes);
    }
  }

  Real PushStatement(Vec value, ad::OperandCount count, LaneBits lanes) {
    if ((statements_ & ad::kChunkMask) == 0) [[unlikely]] {
      EnterStatementChunk();
    }
    counts_[statements_ & ad::kChunkMask] = count;
    const auto index = static_cast<ad::Index>(base_ + statements_);
    statements_ += 1;
    return Real::Active(value, index, lanes);
  }

  void AddMasked(double* slot, LaneBits lanes, Vec value) {
    const D d;
    const Vec old = hn::LoadU(d, slot);
    if constexpr (kMasked) {
      // The mask's bits as LoadMaskBits wants them: 8 readable bytes.
      const std::array<LaneBits, lane_tape_detail::kMaskPadding> bits{lanes};
      const auto mask = hn::LoadMaskBits(d, bits.data());
      hn::StoreU(hn::IfThenElse(mask, hn::Add(old, value), old), d, slot);
    } else {
      static_cast<void>(lanes);
      hn::StoreU(hn::Add(old, value), d, slot);
    }
  }

  void AppendBoundary(ad::Index index, LaneBits lanes, Vec contribution) {
    if (boundary_count_ == boundary_indices_.size()) [[unlikely]] {
      const std::size_t size = std::max<std::size_t>(64, 2 * boundary_indices_.size());
      boundary_indices_.resize(size);
      boundary_masks_.resize(size);
      boundary_values_.resize(size * kLanes);
    }
    boundary_indices_[boundary_count_] = index;
    boundary_masks_[boundary_count_] = lanes;
    hn::StoreU(contribution, D(), boundary_values_.data() + (boundary_count_ * kLanes));
    boundary_count_ += 1;
  }

  void EnterStatementChunk() {
    const std::size_t chunk = statements_ >> ad::kChunkShift;
    if (chunk == statement_chunks_.size()) {
      if (static_cast<std::size_t>(base_) + statements_ >=
          std::numeric_limits<ad::Index>::max() - ad::kChunkEntries) {
        lane_tape_detail::Fail("lane tape index space exhausted");
      }
      statement_chunks_.push_back(ad::NewChunk<ad::OperandCount>());
      adjoints_.resize(statement_chunks_.size() * ad::kChunkEntries * kLanes, 0.0);
      if (!tangents_.empty()) tangents_.resize(adjoints_.size(), 0.0);
    }
    counts_ = statement_chunks_[chunk].get();
  }

  void SelectOperandChunk(std::size_t chunk) {
    if (chunk == operand_chunks_.size()) {
      OperandChunk fresh;
      // NOLINTBEGIN(modernize-avoid-c-arrays): fixed buffers, as tape.hpp's ChunkBuffer.
      fresh.partials = std::make_unique<double[]>(ad::kChunkEntries * kLanes);
      fresh.indices = ad::NewChunk<ad::Index>();
      if constexpr (kMasked) {
        fresh.masks =
            std::make_unique<LaneBits[]>(ad::kChunkEntries + lane_tape_detail::kMaskPadding);
      }
      // NOLINTEND(modernize-avoid-c-arrays)
      operand_chunks_.push_back(std::move(fresh));
    }
    op_chunk_ = chunk;
    partials_ = operand_chunks_[chunk].partials.get();
    indices_ = operand_chunks_[chunk].indices.get();
    masks_ = operand_chunks_[chunk].masks.get();
  }

  void NextOperandChunk() {
    operand_chunks_[op_chunk_].used = op_offset_;
    SelectOperandChunk(op_chunk_ + 1);
    op_offset_ = 0;
  }

  [[nodiscard]] ad::OperandCount CountAt(std::size_t statement) const {
    return statement_chunks_[statement >> ad::kChunkShift][statement & ad::kChunkMask];
  }
  [[nodiscard]] std::size_t UsedOf(std::size_t chunk) const {
    return chunk == op_chunk_ ? op_offset_ : operand_chunks_[chunk].used;
  }

  ad::Index base_;
  std::vector<ad::ChunkBuffer<ad::OperandCount>> statement_chunks_;
  std::vector<OperandChunk> operand_chunks_;
  std::vector<double> adjoints_;  // kLanes per statement slot of every allocated chunk.
  std::vector<double> tangents_;

  std::size_t statements_ = 0;
  ad::OperandCount* counts_ = nullptr;
  std::size_t op_chunk_ = 0;
  std::size_t op_offset_ = 0;
  double* partials_ = nullptr;
  ad::Index* indices_ = nullptr;
  LaneBits* masks_ = nullptr;

  std::vector<ad::Index> boundary_indices_;
  std::vector<LaneBits> boundary_masks_;
  std::vector<double> boundary_values_;
  std::size_t boundary_count_ = 0;

  bool divergent_ = false;
};

namespace lane_tape_detail {

template <class D>
LaneTape<D>*& ActiveSlot() {
  thread_local LaneTape<D>* slot = nullptr;
  return slot;
}

}  // namespace lane_tape_detail

// The thread's active lane tape of this width. One slot per width, apart from the scalar tape's.
template <class D>
LaneTape<D>& ActiveLaneTape() {
  LaneTape<D>* tape = lane_tape_detail::ActiveSlot<D>();
  if (tape == nullptr) [[unlikely]] {
    lane_tape_detail::Fail("an operation on an active lane Real with no active lane tape");
  }
  return *tape;
}

// Makes a lane tape this thread's active one of its width for the scope's lifetime.
template <class D>
class LaneTapeScope {
 public:
  explicit LaneTapeScope(LaneTape<D>& tape) {
    LaneTape<D>*& slot = lane_tape_detail::ActiveSlot<D>();
    if (slot != nullptr) lane_tape_detail::Fail("a second lane tape activated on one thread");
    slot = &tape;
  }
  ~LaneTapeScope() { lane_tape_detail::ActiveSlot<D>() = nullptr; }
  LaneTapeScope(const LaneTapeScope&) = delete;
  LaneTapeScope& operator=(const LaneTapeScope&) = delete;
  LaneTapeScope(LaneTapeScope&&) = delete;
  LaneTapeScope& operator=(LaneTapeScope&&) = delete;
};

}  // namespace aadp::simd

HWY_AFTER_NAMESPACE();

#endif  // AADP_SIMD_LANE_TAPE_HPP_
