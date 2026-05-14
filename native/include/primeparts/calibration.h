// Locked v1 calibration constants. These are duplicated from the Rust
// planner at crates/primeparts-compact/src/planner.rs (see also
// markdown/log_bucket_repartition_spec.md §Encoding-Pinned Calibration).
// Any update here must update the Rust copy in lockstep; the Rust
// planner is the authoritative source for the math because both sides
// emit JSON inspection outputs that must agree.

#pragma once

#include <cstdint>

namespace primeparts {
namespace calibration {

inline constexpr double kBprPrimes = 1.114;
// Post-encoding-pin (DELTA_BINARY_PACKED + zstd-3 on p and prime_rank).
// `p` in partitions is mostly-zero deltas within a prime + a single jump
// at prime boundaries; `prime_rank` similarly. Empirically the partitions
// B/row lands near the primes B/row, not the 4.0 figure from the pre-pin
// spec calibration. The Rust planner copy in
// crates/primeparts-compact/src/planner.rs still has 4.0 — update it in
// lockstep when that path is exercised again.
inline constexpr double kBprPartitions = 4.3044;
inline constexpr double kKMean = 1.882;
inline constexpr int64_t kRpb = 3'855'446'405;
inline constexpr int64_t kTargetFileBytes = int64_t{1} << 30;
inline constexpr int64_t kExpectedNPrimes = 21'699'850'257;
inline constexpr int64_t kExpectedPMin0 = 3;

}  // namespace calibration
}  // namespace primeparts
