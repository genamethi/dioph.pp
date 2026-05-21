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

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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
  // Open. `sqlite_path` is the catalog DB (read-only); `namespace_name`
  // / `table_name` identify the table; `select_columns` is the
  // projection — the reader returns only these columns, in this order.
  // Returns nullptr on error and sets `*error`.
  static std::unique_ptr<SourceTableReader> Open(
      const fs::path& sqlite_path, std::string_view namespace_name,
      std::string_view table_name,
      const std::vector<std::string>& select_columns, std::string* error);

  // Open directly from an Iceberg metadata JSON path. This is useful for
  // staging tables that are already materialized but not registered in the
  // local SQLite catalog.
  static std::unique_ptr<SourceTableReader> OpenMetadata(
      const fs::path& metadata_path,
      const std::vector<std::string>& select_columns, std::string* error);

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
