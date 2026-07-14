#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "iceberg/expression/literal.h"

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

struct WrittenFile {
  std::string table;
  fs::path path;
  int32_t bucket_version = 0;
  int32_t bucket = 0;
  int64_t rows = 0;
  std::map<int32_t, std::pair<iceberg::Literal, iceberg::Literal>> bounds;
  int64_t bytes = 0;
  std::vector<int64_t> split_offsets;
  std::shared_ptr<iceberg::DataFile> data_file;
};

struct WriterConfig {
  struct StatColumn {
    std::string name;
    bool sorted = false;
  };

  fs::path output_dir;
  std::shared_ptr<iceberg::Schema> schema;
  std::string table_name;
  std::string filename_prefix;
  std::vector<std::string> delta_columns;
  std::vector<StatColumn> stat_columns;
  std::shared_ptr<iceberg::PartitionSpec> partition_spec;
  std::shared_ptr<iceberg::PartitionValues> partition_values;
  int32_t bucket_version = 1;
  int32_t bucket = 0;
  int32_t starting_file_seq = 0;
  int64_t target_rows_per_file = 0;
  int32_t compression_level = 3;
  int64_t data_pagesize = 1 << 20;
  int64_t max_row_group_rows = 240'000'000;
  bool simple_filename = false;
};

std::shared_ptr<arrow::Schema> IcebergToArrowSchemaWithFieldIds(
    const iceberg::Schema& schema, std::string* error,
    const iceberg::PartitionSpec* partition_spec = nullptr);

class BucketParquetWriter {
 public:
  static std::unique_ptr<BucketParquetWriter> Make(WriterConfig config,
                                                   std::string* error);

  ~BucketParquetWriter();
  BucketParquetWriter(const BucketParquetWriter&) = delete;
  BucketParquetWriter& operator=(const BucketParquetWriter&) = delete;

  bool Write(const arrow::RecordBatch& batch, std::string* error);

  bool CutRowGroup(int64_t* flushed_bytes, std::string* error);

  bool RollFile(std::string* error, int64_t* closed_file_bytes = nullptr);

  bool Close(std::vector<WrittenFile>* out, std::string* error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit BucketParquetWriter(std::unique_ptr<Impl> impl);
};

}  // namespace primeparts
