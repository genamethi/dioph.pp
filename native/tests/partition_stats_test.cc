#include "primeparts/catalog/partition_stats.h"
#include "primeparts/common/arrow_init.h"
#include "primeparts/schemas.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "iceberg/arrow/arrow_io_util.h"
#include "iceberg/expression/literal.h"
#include "iceberg/file_io.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/partition_spec.h"
#include "iceberg/row/partition_values.h"
#include "iceberg/schema.h"
#include "iceberg/snapshot.h"
#include "iceberg/statistics_file.h"
#include "iceberg/transform.h"
#include "iceberg/util/timepoint.h"

namespace ppc = primeparts::catalog;

namespace {

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

class PartitionStatsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    schema_ = primeparts::PrimesSchema();
    std::string error;
    spec_ = primeparts::BucketPartitionSpec(*schema_, &error);
    ASSERT_NE(spec_, nullptr) << error;
  }

  ppc::PartitionStatsSet SeedStats(const iceberg::Snapshot& snap) {
    std::string error;
    ppc::PartitionStatsSet stats;
    EXPECT_TRUE(ppc::PartitionStatsFields(*schema_, *spec_, &stats, &error))
        << error;
    EXPECT_TRUE(ppc::MergePartitionStats({MakeFile(2, 0, 100, 4096),
                                          MakeFile(2, 0, 60, 2048),
                                          MakeFile(2, 1, 10, 512)},
                                         snap, &stats, &error))
        << error;
    return stats;
  }

  std::shared_ptr<iceberg::Schema> schema_;
  std::shared_ptr<iceberg::PartitionSpec> spec_;
};

TEST_F(PartitionStatsTest, FieldsFromSpec) {
  std::string error;
  ppc::PartitionStatsSet stats;
  ASSERT_TRUE(ppc::PartitionStatsFields(*schema_, *spec_, &stats, &error))
      << error;
  EXPECT_EQ(stats.field_names,
            (std::vector<std::string>{"p_bucket_version", "p_bucket"}));
  EXPECT_EQ(stats.field_ids, (std::vector<int32_t>{1000, 1001}));
}

TEST_F(PartitionStatsTest, MergeAggregates) {
  iceberg::Snapshot snap1;
  snap1.snapshot_id = 41;
  snap1.sequence_number = 1;
  snap1.timestamp_ms = iceberg::TimePointMsFromUnixMs(1752537600000);

  auto stats = SeedStats(snap1);
  ASSERT_EQ(stats.rows.size(), 2u);
  const auto* b0 = FindRow(stats, 2, 0);
  ASSERT_NE(b0, nullptr);
  EXPECT_EQ(b0->data_file_count, 2);
  EXPECT_EQ(b0->data_record_count, 160);
  EXPECT_EQ(b0->total_data_file_size_in_bytes, 6144);
  EXPECT_EQ(b0->last_updated_snapshot_id.value_or(-1), 41);
}

TEST_F(PartitionStatsTest, RoundTripAndIncrementalMerge) {
  const std::filesystem::path out =
      std::filesystem::temp_directory_path() / "primeparts-test-partition-stats";
  std::error_code ec;
  std::filesystem::remove_all(out, ec);
  std::filesystem::create_directories(out, ec);

  iceberg::Snapshot snap1;
  snap1.snapshot_id = 41;
  snap1.sequence_number = 1;
  snap1.timestamp_ms = iceberg::TimePointMsFromUnixMs(1752537600000);
  auto stats = SeedStats(snap1);

  std::string error;
  primeparts::common::EnsureArrowRegistration();
  std::shared_ptr<iceberg::FileIO> io = iceberg::arrow::MakeLocalFileIO();

  std::shared_ptr<iceberg::PartitionStatisticsFile> file;
  ASSERT_TRUE(ppc::WritePartitionStatsFile(stats, snap1.snapshot_id,
                                           "file://" + out.string(), io, &file,
                                           &error))
      << error;
  ASSERT_NE(file, nullptr);
  EXPECT_EQ(file->snapshot_id, 41);
  EXPECT_GT(file->file_size_in_bytes, 0);

  ppc::PartitionStatsSet loaded;
  ASSERT_TRUE(ppc::PartitionStatsFields(*schema_, *spec_, &loaded, &error))
      << error;
  ASSERT_TRUE(ppc::ReadPartitionStatsFile(*file, io, &loaded, &error)) << error;
  ASSERT_EQ(loaded.rows.size(), 2u);
  const auto* r0 = FindRow(loaded, 2, 0);
  const auto* r1 = FindRow(loaded, 2, 1);
  ASSERT_NE(r0, nullptr);
  ASSERT_NE(r1, nullptr);
  EXPECT_EQ(r0->data_file_count, 2);
  EXPECT_EQ(r0->data_record_count, 160);
  EXPECT_EQ(r0->total_data_file_size_in_bytes, 6144);
  EXPECT_EQ(r0->total_record_count, 160);
  EXPECT_EQ(r1->data_file_count, 1);
  EXPECT_EQ(r1->data_record_count, 10);
  EXPECT_EQ(r1->total_data_file_size_in_bytes, 512);
  EXPECT_TRUE(r0->last_updated_at.has_value());
  EXPECT_EQ(r0->last_updated_snapshot_id.value_or(-1), 41);

  iceberg::Snapshot snap2;
  snap2.snapshot_id = 42;
  snap2.sequence_number = 2;
  snap2.timestamp_ms = iceberg::TimePointMsFromUnixMs(1752624000000);
  ASSERT_TRUE(ppc::MergePartitionStats({MakeFile(2, 1, 5, 256)}, snap2, &loaded,
                                       &error))
      << error;
  r1 = FindRow(loaded, 2, 1);
  ASSERT_NE(r1, nullptr);
  EXPECT_EQ(r1->data_file_count, 2);
  EXPECT_EQ(r1->data_record_count, 15);
  EXPECT_EQ(r1->total_data_file_size_in_bytes, 768);
  EXPECT_EQ(r1->last_updated_snapshot_id.value_or(-1), 42);
  r0 = FindRow(loaded, 2, 0);
  ASSERT_NE(r0, nullptr);
  EXPECT_EQ(r0->last_updated_snapshot_id.value_or(-1), 41);

  std::filesystem::remove_all(out, ec);
}

TEST_F(PartitionStatsTest, BucketTransformResolves) {
  const int32_t p_id = 1;
  auto spec_r = iceberg::PartitionSpec::Make(
      *schema_, iceberg::PartitionSpec::kInitialSpecId,
      {iceberg::PartitionField(p_id, 1000, "p_bucketed",
                               iceberg::Transform::Bucket(16))},
      false);
  ASSERT_TRUE(spec_r.has_value()) << spec_r.error().message;

  ppc::PartitionStatsSet stats;
  std::string error;
  EXPECT_TRUE(ppc::PartitionStatsFields(*schema_, *spec_r.value(), &stats,
                                        &error))
      << error;
}

TEST_F(PartitionStatsTest, TruncateTransformResolves) {
  const int32_t p_id = 1;
  auto spec_r = iceberg::PartitionSpec::Make(
      *schema_, iceberg::PartitionSpec::kInitialSpecId,
      {iceberg::PartitionField(p_id, 1000, "p_truncated",
                               iceberg::Transform::Truncate(1000))},
      false);
  ASSERT_TRUE(spec_r.has_value()) << spec_r.error().message;

  ppc::PartitionStatsSet stats;
  std::string error;
  EXPECT_TRUE(ppc::PartitionStatsFields(*schema_, *spec_r.value(), &stats,
                                        &error))
      << error;
}

}  // namespace
