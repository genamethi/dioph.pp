// primeparts/aligned_writer.h
//
// AlignedBucketWriter — the seam-side facade that shapes byte-driven, p-aligned
// parquet buckets for a set of tables that share an atom key (the "p" axis). It
// owns ALL cut/shape policy so generate.cc only streams ordered batches.
//
// Model (sheaf framing): the atom key is the base space; one cut sequence (the
// cover) is chosen once from the size-reference table and applied identically to
// every bound table, so their row groups cover identical p-spans — anti-join
// alignment holds by construction (one cover, many sheaves).
//
// Sizing (why primes is the size reference, not partitions): the hard invariant
// is that PRIMES files land at >= file_target_bytes (~1 GiB). Primes has exactly
// one row per atom (one row per prime p), so a byte-arithmetic provisional cut
// on primes lands on an atom boundary with no snap needed. Bound tables that
// have many rows per atom (partitions: a p spans its whole prime_k run) can NOT
// pin primes, because their bytes/atom ratio varies with prime_k — so they
// FOLLOW the primes cut, aligned by binary-searching their key column to the
// primes cut's p value (no column walk, no p straddle). Partitions row groups
// come out ~2-3x larger as a consequence, exactly p-aligned.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "primeparts/writer.h"  // WrittenFile

namespace arrow {
class RecordBatch;
}
namespace iceberg {
class Schema;
class PartitionSpec;
}

namespace primeparts {

namespace fs = std::filesystem;

// The column whose distinct values are the indivisible atoms no row group may
// split (here "p"). All bound tables must be sorted ascending on it.
struct AtomKey {
  std::string column = "p";
};

// Where to cut a bound (non-reference) table so its row group covers the same
// atoms the reference table's row group does. Given the table's batch, the key
// column, and the reference cut's key VALUE (the last p included), return the
// row index in this batch to cut at (exclusive). Default: upper_bound on the key
// column (first row whose key > cut_key), which keeps every atom whole. Producer
// may override for exotic alignments.
using AlignFn = std::function<int64_t(const arrow::RecordBatch& batch,
                                      const std::string& key_col,
                                      int64_t cut_key)>;

// One table bound into the cover. `reference == true` marks the size-reference
// (primes): its byte budget drives the cut sequence and it must have exactly one
// row per atom. Exactly one bound table must be the reference.
struct BoundTable {
  std::string name;
  std::shared_ptr<iceberg::Schema> schema;
  std::shared_ptr<iceberg::PartitionSpec> spec;
  std::vector<std::string> delta_columns;
  bool reference = false;
  AlignFn align;  // null ⇒ default upper_bound-by-key (see AlignFn)
};

struct ShapePolicy {
  int64_t file_target_bytes = 1LL << 30;   // primes file floor (~1 GiB)
  int rgs_per_file = 4;                     // primes row groups per file
  int64_t bucket_target_bytes = 32LL << 30; // primes bytes per bucket (whole files)
  int32_t bucket_version = 2;
  // Initial reference bytes/row (bytes/atom) before the first flushed row group
  // recalibrates it via EWMA. Primes measures ~1.1 B/row.
  double ref_bytes_per_row_prior = 1.1;
  int64_t rg_target_bytes() const {
    return rgs_per_file > 0 ? file_target_bytes / rgs_per_file : file_target_bytes;
  }
};

// Where a resumed run picks up: the open (frontier) bucket, how many reference
// bytes already sit in it, and each table's next file sequence number.
struct ResumeState {
  int32_t bucket = 0;
  int64_t bucket_fill_bytes = 0;
  std::map<std::string, int32_t> next_seq;
};

struct TableFiles {
  std::string name;
  std::vector<WrittenFile> files;
};

// The output of a full run: every committed-staging file per table, ready for
// CommitFilesAtomic.
struct CommitPlan {
  std::vector<TableFiles> tables;
};

class AlignedBucketWriter {
 public:
  // `tables` order is the order Append() batches arrive in. Exactly one table
  // must have reference==true. `warehouse` roots the staging tree. Returns null +
  // *error on misconfiguration.
  static std::unique_ptr<AlignedBucketWriter> Make(
      const fs::path& warehouse, std::vector<BoundTable> tables, AtomKey atom,
      ShapePolicy policy, ResumeState resume, std::string* error);

  ~AlignedBucketWriter();
  AlignedBucketWriter(const AlignedBucketWriter&) = delete;
  AlignedBucketWriter& operator=(const AlignedBucketWriter&) = delete;

  // One aligned append. batches[i] corresponds to tables[i] (same index), all
  // sorted ascending on the atom key over the SAME p range. Single writer
  // thread. Cuts row groups / rolls files / rolls buckets as byte targets are
  // hit. False + *error on failure.
  bool Append(const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
              std::string* error);

  // Close all open files and collect the CommitPlan (per-table WrittenFile
  // records). After Finish the writer is dead.
  bool Finish(CommitPlan* out, std::string* error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit AlignedBucketWriter(std::unique_ptr<Impl> impl);
};

// Resolve the frontier bucket + per-table next sequence for a resumed run: find
// the max p_bucket the reference table has committed, sum its file bytes for the
// bucket fill, and take each table's next file sequence. Absent ⇒ bucket 0, fill
// 0, seq 0. The spec-proper form reads this from the current snapshot's
// manifests (scan-planning-mode: client); wired in generate against the catalog.
bool LoadAlignedResume(const fs::path& warehouse,
                       const std::vector<std::string>& table_names,
                       const std::string& reference_table, int32_t bucket_version,
                       ResumeState* out, std::string* error);

}  // namespace primeparts
