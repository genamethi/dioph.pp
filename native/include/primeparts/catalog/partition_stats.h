#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "iceberg/type_fwd.h"

namespace primeparts::catalog {

struct PartitionStatsRow {
  std::vector<int64_t> partition;
  int32_t spec_id = 0;
  int64_t data_record_count = 0;
  int32_t data_file_count = 0;
  int64_t total_data_file_size_in_bytes = 0;
  int64_t position_delete_record_count = 0;
  int32_t position_delete_file_count = 0;
  int64_t equality_delete_record_count = 0;
  int32_t equality_delete_file_count = 0;
  int64_t total_record_count = 0;
  std::optional<int64_t> last_updated_at;
  std::optional<int64_t> last_updated_snapshot_id;
};

struct PartitionStatsSet {
  int32_t spec_id = 0;
  std::vector<std::string> field_names;
  std::vector<iceberg::TypeId> field_types;
  std::vector<int32_t> field_ids;
  std::vector<PartitionStatsRow> rows;
};

bool PartitionStatsFields(const iceberg::Schema& schema,
                          const iceberg::PartitionSpec& spec,
                          PartitionStatsSet* out, std::string* error);

bool ComputePartitionStats(const iceberg::Table& table,
                           const iceberg::Snapshot& snapshot,
                           PartitionStatsSet* out, std::string* error);

bool MergePartitionStats(
    const std::vector<std::shared_ptr<iceberg::DataFile>>& appended,
    const iceberg::Snapshot& snapshot, PartitionStatsSet* stats,
    std::string* error);

bool WritePartitionStatsFile(
    const PartitionStatsSet& stats, int64_t snapshot_id,
    const std::string& metadata_dir_uri,
    const std::shared_ptr<iceberg::FileIO>& io,
    std::shared_ptr<iceberg::PartitionStatisticsFile>* out, std::string* error);

bool ReadPartitionStatsFile(const iceberg::PartitionStatisticsFile& file,
                            const std::shared_ptr<iceberg::FileIO>& io,
                            PartitionStatsSet* stats, std::string* error);

bool LoadPartitionStats(const iceberg::Table& table, PartitionStatsSet* out,
                        std::string* error);

std::shared_ptr<iceberg::PartitionStatisticsFile> BuildPartitionStatsForAppend(
    const iceberg::Table& table, const iceberg::Snapshot& new_snapshot,
    const std::vector<std::shared_ptr<iceberg::DataFile>>& appended,
    std::string* error);

}  // namespace primeparts::catalog
