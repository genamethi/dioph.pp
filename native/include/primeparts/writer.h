// Shared parquet write primitive used by both primeparts-generate and
// primeparts-rewrite. The writer takes a stream of arrow::RecordBatches
// and rolls files at a target row count, producing one or more parquet
// files in a directory + a vector of WrittenFile records describing
// them. It owns the iceberg-cpp Schema → arrow-with-PARQUET:field_id
// translation and the per-column DELTA/zstd encoding policy.
//
// Per-column DELTA encoding is the reason we bypass iceberg-cpp's own
// parquet writer here: iceberg::ParquetWriterProperties doesn't expose
// per-column DELTA_BINARY_PACKED yet, while parquet::arrow::FileWriter
// does.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace arrow {
class RecordBatch;
class Schema;
}

namespace iceberg {
class Schema;
}

namespace primeparts {

namespace fs = std::filesystem;

// One parquet file written by BucketParquetWriter. Same shape that
// generate.cc has emitted into files.jsonl since the project began;
// rewriter emits the same format so the commit binary doesn't care
// whether the bytes came from FLINT or from a rewrite pass.
struct WrittenFile {
  std::string table;          // logical table name: "primes" or "partitions"
  fs::path path;              // absolute on-disk path
  int32_t bucket_version = 0;
  int32_t bucket = 0;
  int64_t rows = 0;
  int64_t p_min = 0;
  int64_t p_max = 0;
  int64_t rank_min = 0;
  int64_t rank_max = 0;
  int64_t bytes = 0;
};

struct WriterConfig {
  // Directory the writer creates files under. Caller is responsible for
  // computing this from warehouse + bucket; writer creates intermediate
  // dirs if missing.
  fs::path output_dir;

  // Iceberg schema is the field-id source of truth. The writer derives
  // an arrow::Schema with PARQUET:field_id metadata on every column.
  std::shared_ptr<iceberg::Schema> schema;

  // Table name stamped onto each WrittenFile record.
  std::string table_name;

  // Filename prefix; full name is `<prefix>_v<NNNN>_b<NNNNNN>_<SSSS>.parquet`
  // where v=bucket_version, b=bucket, S=file_seq.
  std::string filename_prefix;

  // Columns to encode with DELTA_BINARY_PACKED (dictionary disabled).
  // For primeparts these are the monotone integer columns: {"p",
  // "prime_rank", "q_k"}. Members not in the schema are ignored.
  std::vector<std::string> delta_columns;

  // Partition values written into the path (Hive-style) and stamped on
  // every row of every batch.
  int32_t bucket_version = 1;
  int32_t bucket = 0;

  // File seq for the first file the writer opens. Discoverable via
  // BucketParquetWriter::NextFileSeq(output_dir, filename_prefix).
  int32_t starting_file_seq = 0;

  // Target number of rows per file before rolling. The current file is
  // closed when its accumulated row count reaches this threshold; the
  // batch that crossed the threshold lands in the next file. If 0, the
  // writer never rolls (single file per writer; used by the live
  // generator's group-shaped flow).
  int64_t target_rows_per_file = 0;

  // Parquet writer tunables. Defaults match what generate.cc has used:
  // zstd level 3, 1 MiB data pages.
  int32_t compression_level = 3;
  int64_t data_pagesize = 1 << 20;
};

// Field-id-aware arrow schema derived from an iceberg::Schema. Public
// because the rewriter also wants to stamp PARQUET:field_id on its
// source-projected schemas before re-writing. Returns nullptr on
// unsupported type id and sets `*error`.
std::shared_ptr<arrow::Schema> IcebergToArrowSchemaWithFieldIds(
    const iceberg::Schema& schema, std::string* error);

// New staging-warehouse schemas, spec-aligned. Field IDs inherited
// from the source tables' schema-evolution history:
//   primes:     p=1, k=2, [3 retired: commit_seq], prime_rank=4,
//               p_bucket_version=5, p_bucket=6
//   partitions: p=1, m_k=2, n_k=3, q_k=4, [5 retired: commit_seq],
//               prime_rank=6, p_bucket_version=7, p_bucket=8
// Field IDs 3 (primes) and 5 (partitions) are intentionally skipped:
// they held commit_seq in the source schemas; Iceberg forbids reuse,
// so the slots are retired in the new tables.
std::shared_ptr<iceberg::Schema> PrimesSchema();
std::shared_ptr<iceberg::Schema> PartitionsSchema();

// Default bucket-data-dir layout used by the live writer. Kept here so
// generate.cc and rewrite.cc compute identical paths.
fs::path BucketDataDir(const fs::path& warehouse, std::string_view table,
                       int32_t bucket_version, int32_t bucket);

// Scan `output_dir` for existing files matching the prefix pattern;
// return one past the highest seq number found, or 0 if the directory
// doesn't exist / is empty. Lets the generator pick up where a previous
// run stopped without overwriting files.
int32_t NextFileSeq(const fs::path& output_dir, std::string_view prefix);

class BucketParquetWriter {
 public:
  // Construct. Validates the schema, builds the arrow schema with
  // field-ids, prebuilds parquet::WriterProperties. Returns nullptr on
  // error and sets `*error`.
  static std::unique_ptr<BucketParquetWriter> Make(WriterConfig config,
                                                   std::string* error);

  ~BucketParquetWriter();
  BucketParquetWriter(const BucketParquetWriter&) = delete;
  BucketParquetWriter& operator=(const BucketParquetWriter&) = delete;

  // Per-batch stats the caller computes from the row source. The
  // writer doesn't peek into batches to compute these because the
  // column names that carry `p` / `prime_rank` are caller-policy. Stats
  // accumulate into the *current file*; a roll triggered after this
  // Write freezes them on the closed file's WrittenFile record.
  struct BatchStats {
    int64_t p_min;
    int64_t p_max;
    int64_t rank_min;
    int64_t rank_max;
  };

  // Append a batch. The batch's columns are re-wrapped under the
  // writer's target schema (so PARQUET:field_id flows through). A roll
  // is triggered before the *next* Write() if accumulated rows in the
  // current file reach target_rows_per_file (so rolls land on batch
  // boundaries; caller controls precision via batch size). Returns
  // false and sets `*error` on parquet error.
  bool Write(const arrow::RecordBatch& batch, BatchStats stats,
             std::string* error);

  // Close the current file (if any). Returns the WrittenFile records
  // for every file produced over the writer's lifetime. After Close()
  // the writer is dead; further Write() returns false.
  bool Close(std::vector<WrittenFile>* out, std::string* error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit BucketParquetWriter(std::unique_ptr<Impl> impl);
};

}  // namespace primeparts
