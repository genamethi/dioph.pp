// Shared parquet write primitive used by primeparts-generate. The writer
// takes a stream of arrow::RecordBatches
// and rolls files at a target row count, producing one or more parquet
// files in a directory + a vector of WrittenFile records describing
// them. It owns the iceberg-cpp Schema → arrow-with-PARQUET:field_id
// translation and the per-column DELTA/zstd encoding policy.
//
// The physical writer is parquet::arrow::FileWriter. Iceberg DataFile
// metadata is synthesized after close from the writer's exact file stats,
// keeping catalog publication high-level without putting Iceberg's
// Arrow-C bridge on the row-write hot path.

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
struct DataFile;
class PartitionSpec;
class Schema;
class PartitionValues;
}

namespace primeparts {

namespace fs = std::filesystem;

// One parquet file written by BucketParquetWriter. The JSONL projection
// remains useful as an audit trail, but the Iceberg DataFile metadata is
// the catalog handoff for native rewrites.
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
  std::shared_ptr<iceberg::DataFile> data_file;
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

  // Optional: Custom partition spec and values for the data files.
  // If provided, bucket_version and bucket are ignored for DataFile generation.
  std::shared_ptr<iceberg::PartitionSpec> partition_spec;
  std::shared_ptr<iceberg::PartitionValues> partition_values;

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

  // Max rows per row group before parquet cuts a new one. Default ~256 MiB
  // compressed at the base-table ~1.1 B/row. Tables that physically sort by a
  // low-cardinality key (mdiff: shape) set this smaller so each row group is a
  // narrow key band and readers can prune by its min/max stats.
  int64_t max_row_group_rows = 240'000'000;

  // Simpler filename format `<prefix>_<seq:04d>.parquet`, useful for
  // partitions that don't carry the bucket_version/bucket scheme (e.g.
  // covering_system).
  bool simple_filename = false;
};

// Field-id-aware arrow schema derived from an iceberg::Schema. Public so
// callers can stamp PARQUET:field_id on source-projected schemas before
// writing. Returns nullptr on unsupported type id and sets `*error`.
//
// If `partition_spec` is non-null, fields whose IDs are the source IDs of
// identity-transform partition fields are omitted from the resulting arrow
// schema. Those values live in the manifest's partition tuple and are
// synthesized by iceberg readers at scan time — physically storing them in
// every parquet file is redundant for identity transforms.
std::shared_ptr<arrow::Schema> IcebergToArrowSchemaWithFieldIds(
    const iceberg::Schema& schema, std::string* error,
    const iceberg::PartitionSpec* partition_spec = nullptr);

// Concrete primeparts schemas (PrimesSchema/PartitionsSchema/MdiffSchema/...)
// and the p_bucket partition spec live in "primeparts/schemas.h" — the writer
// is schema-agnostic and only consumes a schema via WriterConfig.

// Default bucket-data-dir layout used by the live writer. Kept here so
// generate.cc and rewrite.cc compute identical primeparts.* paths.
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
