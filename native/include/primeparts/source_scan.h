#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "primeparts/scan/scan_plan.h"

namespace arrow {
class RecordBatch;
}

namespace iceberg {
class Expression;
class FileIO;
}

namespace primeparts {

namespace fs = std::filesystem;

class SourceTableReader {
 public:
  static std::unique_ptr<SourceTableReader> OpenMetadata(
      const fs::path& metadata_path,
      const std::vector<std::string>& select_columns,
      std::shared_ptr<iceberg::Expression> filter,
      std::string* error,
      int shard_index = 0, int shard_count = 1);

  static std::unique_ptr<SourceTableReader> OpenIncremental(
      const fs::path& metadata_path,
      const std::vector<std::string>& select_columns,
      std::shared_ptr<iceberg::Expression> filter,
      int64_t from_snapshot_id_exclusive,
      std::string* error,
      int shard_index = 0, int shard_count = 1);

  static std::unique_ptr<SourceTableReader> Open(
      scan::ScanPlan plan, std::shared_ptr<iceberg::FileIO> io,
      std::string* error, int shard_index = 0, int shard_count = 1);

  ~SourceTableReader();
  SourceTableReader(const SourceTableReader&) = delete;
  SourceTableReader& operator=(const SourceTableReader&) = delete;

  bool Next(std::shared_ptr<arrow::RecordBatch>* out, std::string* error);

  int64_t total_records() const;

  void set_total_records(int64_t total);

  int64_t planned_records() const;

  int64_t file_count() const;

  const std::string& current_data_file_path() const;

  const std::shared_ptr<iceberg::Expression>& residual() const;

  const scan::TableReadTraits& traits() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit SourceTableReader(std::unique_ptr<Impl> impl);
};

}  // namespace primeparts
