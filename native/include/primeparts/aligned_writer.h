#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "primeparts/writer.h"

namespace arrow {
class RecordBatch;
}
namespace iceberg {
class Catalog;
class Schema;
class PartitionSpec;
struct Namespace;
}

namespace primeparts {

namespace fs = std::filesystem;

struct AtomKey {
  std::string column = "p";
};

using AlignFn = std::function<int64_t(const arrow::RecordBatch& batch,
                                      const std::string& key_col,
                                      int64_t cut_key)>;

struct BoundTable {
  std::string name;
  std::shared_ptr<iceberg::Schema> schema;
  std::shared_ptr<iceberg::PartitionSpec> spec;
  std::vector<std::string> delta_columns;
  std::vector<WriterConfig::StatColumn> stat_columns;
  bool reference = false;
  AlignFn align;
};

struct ShapePolicy {
  int64_t file_target_bytes = 1LL << 30;
  int rgs_per_file = 4;
  int64_t bucket_target_bytes = 32LL << 30;
  int32_t bucket_version = 2;
  double ref_bytes_per_row_prior = 1.1;
  int64_t rg_target_bytes() const {
    return rgs_per_file > 0 ? file_target_bytes / rgs_per_file : file_target_bytes;
  }
};

struct ResumeState {
  int32_t bucket = 0;
  int64_t bucket_fill_bytes = 0;
  std::map<std::string, int32_t> next_seq;
};

struct TableFiles {
  std::string name;
  std::vector<WrittenFile> files;
};

struct CommitPlan {
  std::vector<TableFiles> tables;
};

class AlignedBucketWriter {
 public:
  static std::unique_ptr<AlignedBucketWriter> Make(
      const fs::path& warehouse, std::vector<BoundTable> tables, AtomKey atom,
      ShapePolicy policy, ResumeState resume, std::string* error);

  ~AlignedBucketWriter();
  AlignedBucketWriter(const AlignedBucketWriter&) = delete;
  AlignedBucketWriter& operator=(const AlignedBucketWriter&) = delete;

  bool Append(const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
              std::string* error);

  bool Finish(CommitPlan* out, std::string* error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit AlignedBucketWriter(std::unique_ptr<Impl> impl);
};

struct BucketFields {
  std::string version_field;
  std::string bucket_field;
};

bool LoadAlignedResume(const std::shared_ptr<iceberg::Catalog>& catalog,
                       const iceberg::Namespace& ns,
                       const std::vector<std::string>& table_names,
                       const std::string& reference_table,
                       const BucketFields& bucket_fields,
                       int32_t bucket_version, ResumeState* out,
                       std::string* error);

}  // namespace primeparts
