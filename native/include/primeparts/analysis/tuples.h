// primeparts/analysis/tuples.h
//
// Pure-math helpers for the m-tuple statistics tools (`tally`, `cluster`). An
// m-tuple is the set of exponents m for a prime p = 2^m + q^n; the m values are
// distinct, so a tuple is a strictly increasing set encoded as a bitmask (bit m
// set iff exponent m is present). m ranges 1..38 (= floor(log2(p_max))).
//
// This header carries NO iceberg/arrow/query dependency on purpose, so its
// translation unit links cheaply into both binaries. Storage encode/decode
// (mask <-> the b1..b38 + shift columns of PresenceSchema) and the arithmetic-
// progression / clustering primitives live here; catalog I/O does not.

#pragma once

#include <cstdint>
#include <vector>

namespace primeparts::analysis {

// Width of the presence/shape vector = floor(log2(p_max)); m in [1, kTupleWidth].
// p_max ≈ 5.646e11 and 2^39 = 5.498e11 < p_max < 2^40, so max m is 39. A shape
// spans at most kTupleWidth-1 bits (m_max - m_min, e.g. {1,39} -> 38). Must match
// the b-column count in PresenceSchema().
constexpr int kTupleWidth = 39;

// k range we build/analyze: one mtuple_k{K} table per K.
constexpr int kMinK = 2;
constexpr int kMaxK = 16;

// --- bitmask <-> positions -------------------------------------------------

// Sorted ascending m values (bit positions) set in `mask`.
std::vector<int> MaskToPositions(uint64_t mask);

// OR of (1<<m) over the positions.
uint64_t PositionsToMask(const std::vector<int>& positions);

inline int Popcount(uint64_t mask) { return __builtin_popcountll(mask); }

// --- storage form (PresenceSchema: b1..b38, shift) -------------------------

// Trailing-zero count of the original mask = m_min. Undefined for mask==0.
inline int ShiftOf(uint64_t mask) { return __builtin_ctzll(mask); }

// Base-shifted shape: mask >> ctz(mask). Bit 0 is always set. This is the
// translation-invariant representative (grouping key for the shifted view).
inline uint64_t ShapeOf(uint64_t mask) { return mask >> ShiftOf(mask); }

// Fill `bits[0..kTupleWidth-1]` (0/1) from the shape of `mask` and set *shift to
// ctz(mask). bits[j] is column b(j+1). Recover a position: m = shift + j.
void MaskToColumns(uint64_t mask, int32_t* bits, int32_t* shift);

// Inverse: original mask from stored shape bits + shift.
uint64_t ColumnsToMask(const int32_t* bits, int32_t shift);

// --- arithmetic-progression / clustering primitives ------------------------

// True iff the sorted positions are an exact arithmetic progression (all
// consecutive differences equal). Trivially true for size <= 2.
bool IsExactAP(const std::vector<int>& positions);

// gcd of the consecutive differences = the common modulus the positions share
// (every position lies in one residue class mod this value). 0 for size < 2.
int GcdDiffs(const std::vector<int>& positions);

// Common difference d = (max - min) / (k - 1) rounded to nearest int (the
// best-fit AP step); 0 for size < 2.
int BestStep(const std::vector<int>& positions);

// Max absolute deviation of any position from the best-fit integer AP
// (positions[0] + i*BestStep). 0 for an exact AP; grows as the tuple departs
// from a progression. -1 for size < 2 (undefined).
int NearAPDeviation(const std::vector<int>& positions);

// Standardized Poisson residual (count - expected) / sqrt(expected). 0 when
// expected <= 0. The shared core of both significance measures.
double StdResidual(int64_t count, double expected);

// Uniform null over `n_distinct` representatives sharing `total` primes:
// expected = total / n_distinct. Flags reps above a flat share.
double UniformResidual(int64_t count, int64_t total, int64_t n_distinct);

// Position-independence null: expected = total * product of the single-position
// marginal frequencies of the rep's positions (`marginals[pos]` = fraction of
// `total` carrying position `pos`). Flags reps whose positions CO-OCCUR beyond
// what their individual frequencies predict — i.e. genuine clustering, net of
// some positions just being common. `positions` are bit indices into `marginals`.
double IndependenceResidual(int64_t count, int64_t total,
                            const std::vector<int>& positions,
                            const std::vector<double>& marginals);

}  // namespace primeparts::analysis
