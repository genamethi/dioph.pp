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

struct SourceFileInfo {
  std::string path;
  int64_t p_min = 0;
  int64_t p_max = 0;
  int64_t record_count = 0;
};

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

  ~SourceTableReader();
  SourceTableReader(const SourceTableReader&) = delete;
  SourceTableReader& operator=(const SourceTableReader&) = delete;

  bool Next(std::shared_ptr<arrow::RecordBatch>* out, std::string* error);

  int64_t total_records() const;

  int64_t file_count() const;

  const std::string& current_data_file_path() const;

  std::vector<SourceFileInfo> source_files() const;

 private:
  struct Impl;
  static std::unique_ptr<Impl> BuildImpl(
      const fs::path& metadata_path,
      const std::vector<std::string>& select_columns,
      std::shared_ptr<iceberg::Expression> filter,
      std::optional<int64_t> from_snapshot_id_exclusive, std::string* error,
      int shard_index, int shard_count);
  std::unique_ptr<Impl> impl_;
  explicit SourceTableReader(std::unique_ptr<Impl> impl);
};

}
