#include "primeparts/catalog/partition_stats.h"
#include "primeparts/schemas.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "iceberg/expression/literal.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/row/partition_values.h"
#include "iceberg/snapshot.h"
#include "iceberg/statistics_file.h"
#include "iceberg/util/timepoint.h"

namespace ppc = primeparts::catalog;

namespace {

bool Check(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

std::shared_ptr<iceberg::DataFile> MakeFile(int32_t version, int32_t bucket,
                                            int64_t records, int64_t bytes) {
  auto df = std::make_shared<iceberg::DataFile>();
  df->file_path = "file:///tmp/fake-" + std::to_string(bucket) + ".parquet";
  df->partition = iceberg::PartitionValues(std::vector<iceberg::Literal>{
      iceberg::Literal::Int(version), iceberg::Literal::Int(bucket)});
  df->record_count = records;
  df->file_size_in_bytes = bytes;
  return df;
}

const ppc::PartitionStatsRow* FindRow(const ppc::PartitionStatsSet& stats,
                                      int64_t version, int64_t bucket) {
  for (const auto& row : stats.rows) {
    if (row.partition == std::vector<int64_t>{version, bucket}) return &row;
  }
  return nullptr;
}

}  // namespace

int main() {
  const std::filesystem::path out =
      std::filesystem::temp_directory_path() / "primeparts-test-partition-stats";
  std::error_code ec;
  std::filesystem::remove_all(out, ec);
  std::filesystem::create_directories(out, ec);

  std::string error;
  auto schema = primeparts::PrimesSchema();
  auto spec = primeparts::BucketPartitionSpec(*schema, &error);
  if (!spec) {
    std::cerr << error << "\n";
    return 1;
  }

  ppc::PartitionStatsSet stats;
  if (!ppc::PartitionStatsFields(*schema, *spec, &stats, &error)) {
    std::cerr << error << "\n";
    return 1;
  }
  if (!Check(stats.field_names ==
                 std::vector<std::string>{"p_bucket_version", "p_bucket"},
             "expected bucket fields from spec")) {
    return 1;
  }
  if (!Check(stats.field_ids == std::vector<int32_t>{1000, 1001},
             "expected partition field ids 1000/1001")) {
    return 1;
  }

  iceberg::Snapshot snap1;
  snap1.snapshot_id = 41;
  snap1.sequence_number = 1;
  snap1.timestamp_ms = iceberg::TimePointMsFromUnixMs(1752537600000);

  if (!ppc::MergePartitionStats({MakeFile(2, 0, 100, 4096),
                                 MakeFile(2, 0, 60, 2048),
                                 MakeFile(2, 1, 10, 512)},
                                snap1, &stats, &error)) {
    std::cerr << error << "\n";
    return 1;
  }
  if (!Check(stats.rows.size() == 2, "expected two partition rows")) return 1;
  const auto* b0 = FindRow(stats, 2, 0);
  if (!Check(b0 && b0->data_file_count == 2 && b0->data_record_count == 160 &&
                 b0->total_data_file_size_in_bytes == 6144,
             "expected bucket 0 aggregates (2 files, 160 rows, 6144 bytes)")) {
    return 1;
  }
  if (!Check(b0->last_updated_snapshot_id.value_or(-1) == 41,
             "expected bucket 0 last_updated_snapshot_id=41")) {
    return 1;
  }

  std::shared_ptr<iceberg::PartitionStatisticsFile> file;
  if (!ppc::WritePartitionStatsFile(stats, snap1.snapshot_id,
                                    "file://" + out.string(), &file, &error)) {
    std::cerr << error << "\n";
    return 1;
  }
  if (!Check(file && file->snapshot_id == 41 && file->file_size_in_bytes > 0,
             "expected registered stats file descriptor")) {
    return 1;
  }

  ppc::PartitionStatsSet loaded;
  if (!ppc::PartitionStatsFields(*schema, *spec, &loaded, &error)) {
    std::cerr << error << "\n";
    return 1;
  }
  if (!ppc::ReadPartitionStatsFile(*file, &loaded, &error)) {
    std::cerr << error << "\n";
    return 1;
  }
  if (!Check(loaded.rows.size() == 2, "expected two rows after read")) return 1;
  const auto* r0 = FindRow(loaded, 2, 0);
  const auto* r1 = FindRow(loaded, 2, 1);
  if (!Check(r0 && r1, "expected both partitions after read")) return 1;
  if (!Check(r0->data_file_count == 2 && r0->data_record_count == 160 &&
                 r0->total_data_file_size_in_bytes == 6144 &&
                 r0->total_record_count == 160,
             "expected bucket 0 round-trip")) {
    return 1;
  }
  if (!Check(r1->data_file_count == 1 && r1->data_record_count == 10 &&
                 r1->total_data_file_size_in_bytes == 512,
             "expected bucket 1 round-trip")) {
    return 1;
  }
  if (!Check(r0->last_updated_at.has_value() &&
                 r0->last_updated_snapshot_id.value_or(-1) == 41,
             "expected last-updated round-trip")) {
    return 1;
  }

  iceberg::Snapshot snap2;
  snap2.snapshot_id = 42;
  snap2.sequence_number = 2;
  snap2.timestamp_ms = iceberg::TimePointMsFromUnixMs(1752624000000);
  if (!ppc::MergePartitionStats({MakeFile(2, 1, 5, 256)}, snap2, &loaded,
                                &error)) {
    std::cerr << error << "\n";
    return 1;
  }
  r1 = FindRow(loaded, 2, 1);
  if (!Check(r1 && r1->data_file_count == 2 && r1->data_record_count == 15 &&
                 r1->total_data_file_size_in_bytes == 768 &&
                 r1->last_updated_snapshot_id.value_or(-1) == 42,
             "expected bucket 1 incremental merge")) {
    return 1;
  }
  r0 = FindRow(loaded, 2, 0);
  if (!Check(r0 && r0->last_updated_snapshot_id.value_or(-1) == 41,
             "expected untouched bucket 0 to keep snapshot 41")) {
    return 1;
  }

  std::filesystem::remove_all(out, ec);
  return 0;
}
