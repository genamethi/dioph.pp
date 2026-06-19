// Source-table reader for the rewriter. Opens an Iceberg table from a
// sqlite catalog (read-only), uses iceberg-cpp's TableScan to plan
// FileScanTasks, sorts them by `p` lower-bound so the producer streams
// in p-order, and exposes `Next(&batch)` returning arrow RecordBatches
// projected to the caller's column list.
//
// This is the read-side counterpart to writer.h. Both are deliberately
// thin: source_scan owns scan plumbing, writer owns write plumbing,
// the rewriter just connects them with a sort-merge.

#pragma once

#include "iceberg/expression/expression.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <optional>

namespace arrow {
class RecordBatch;
}

namespace primeparts {

namespace fs = std::filesystem;

// Per-file info from manifest stats. Used by the rewriter to plan
// per-bucket worker jobs without holding a reader open or decoding any
// data. p_min/p_max come from the manifest's lower/upper bounds on the
// `p` column (field_id 1 in both source tables); record_count is the
// manifest-declared row count.
struct SourceFileInfo {
  std::string path;       // absolute local-fs path (file:// scheme stripped)
  int64_t p_min = 0;
  int64_t p_max = 0;
  int64_t record_count = 0;
};

class SourceTableReader {
 public:
  // Open directly from an Iceberg metadata JSON path. Metadata is resolved
  // through the catalog seam (MakeLocalCatalog -> LoadTable -> metadata_location)
  // by callers; this reader just scans the given metadata.json.
  //
  // `select_columns` may include the reserved metadata columns "_pos"
  // (absolute ordinal of the row within its source data file) and "_file"
  // (the source data file path). These are not table fields, so they bypass
  // the scan's column projection: the reader appends them to the projected
  // schema and the parquet reader synthesizes their values. Combined with
  // `current_data_file_path()`, "_pos" gives the (file_path, pos) a position
  // delete needs — and the values are correct under merge-on-read (they are
  // the absolute positions of the surviving rows). Callers that don't request
  // them see identical behavior to before.
  //
  // Sharding: with `shard_count > 1`, this reader handles only the planned
  // FileScanTasks where `task_index % shard_count == shard_index` (modulo over
  // the p-sorted task order, so each shard interleaves small/large-p files for
  // load balance). Run N independent instances on N threads for parallel,
  // delete-aware reads — each keeps its own cursor + current_data_file_path()
  // and applies position deletes per task via the same FileScanTaskReader path.
  // The default (0, 1) keeps every task (callers using this for metadata-only
  // discovery, e.g. source_files(), are unaffected).
  static std::unique_ptr<SourceTableReader> OpenMetadata(
      const fs::path& metadata_path,
      const std::vector<std::string>& select_columns,
      std::shared_ptr<iceberg::Expression> filter,
      std::string* error,
      int shard_index = 0, int shard_count = 1);

  ~SourceTableReader();
  SourceTableReader(const SourceTableReader&) = delete;
  SourceTableReader& operator=(const SourceTableReader&) = delete;

  // Read the next batch in p-order. On EOF sets `*out` to nullptr and
  // returns true. On error returns false and sets `*error`. On
  // success `*out` is a non-empty batch.
  bool Next(std::shared_ptr<arrow::RecordBatch>* out, std::string* error);

  // Manifest-aggregated total row count, summed over all data files in
  // the current snapshot. Cheap; reads only manifest list + manifests.
  int64_t total_records() const;

  // Number of FileScanTasks the scan planned (i.e., source data file
  // count). Useful for progress reporting.
  int64_t file_count() const;

  // Data file path (as stored in the manifest, scheme intact) of the task
  // the most recently returned batch was read from. Pair with a projected
  // "_pos" column to form (file_path, pos) for a position delete. Valid only
  // after a successful Next() that returned a non-null batch.
  const std::string& current_data_file_path() const;

  // Per-file info derived from the planned scan, sorted by p_min ASC
  // (matches the streaming order of Next()). Lets the rewriter plan
  // bucket-specific worker jobs from manifest stats alone — no data
  // decode required.
  std::vector<SourceFileInfo> source_files() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit SourceTableReader(std::unique_ptr<Impl> impl);
};

}  // namespace primeparts
