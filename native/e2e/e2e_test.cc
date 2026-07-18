#include "primeparts/catalog/partition_stats.h"
#include "primeparts/catalog/pp_commit.h"
#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/common/arrow_init.h"
#include "primeparts/query/query_service.h"
#include "primeparts/schemas.h"
#include "primeparts/writer.h"

#include <arrow/api.h>
#include <gtest/gtest.h>
#include <httplib.h>

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "iceberg/catalog.h"
#include "iceberg/expression/literal.h"
#include "iceberg/partition_spec.h"
#include "iceberg/row/partition_values.h"
#include "iceberg/schema.h"
#include "iceberg/schema_field.h"
#include "iceberg/table.h"
#include "iceberg/type.h"

namespace ppc = primeparts::catalog;
namespace ppq = primeparts::query;
namespace fs = std::filesystem;

namespace {

std::shared_ptr<arrow::RecordBatch> MakePrimesBatch(
    const std::shared_ptr<arrow::Schema>& schema) {
  arrow::Int64Builder p;
  arrow::Int32Builder k;
  arrow::Int64Builder prime_rank;
  arrow::Int32Builder bucket_version;
  arrow::Int32Builder bucket;
  for (int64_t value : {3, 5, 7}) EXPECT_TRUE(p.Append(value).ok());
  for (int32_t value : {2, 1, 3}) EXPECT_TRUE(k.Append(value).ok());
  for (int64_t value : {0, 1, 2}) EXPECT_TRUE(prime_rank.Append(value).ok());
  for (int32_t value : {1, 1, 1}) EXPECT_TRUE(bucket_version.Append(value).ok());
  for (int32_t value : {2, 2, 2}) EXPECT_TRUE(bucket.Append(value).ok());
  std::shared_ptr<arrow::Array> pa, ka, ra, va, ba;
  EXPECT_TRUE(p.Finish(&pa).ok());
  EXPECT_TRUE(k.Finish(&ka).ok());
  EXPECT_TRUE(prime_rank.Finish(&ra).ok());
  EXPECT_TRUE(bucket_version.Finish(&va).ok());
  EXPECT_TRUE(bucket.Finish(&ba).ok());
  return arrow::RecordBatch::Make(schema, 3, {pa, ka, ra, va, ba});
}

std::vector<std::shared_ptr<iceberg::DataFile>> WritePrimesFiles(
    const fs::path& warehouse, const iceberg::Namespace& ns,
    const std::shared_ptr<iceberg::Schema>& schema,
    const std::shared_ptr<iceberg::PartitionSpec>& spec,
    const std::shared_ptr<arrow::Schema>& arrow_schema) {
  std::string error;
  primeparts::WriterConfig cfg;
  cfg.output_dir = ppc::StagingDataDir(warehouse, ns, "primes") /
                   "p_bucket_version=1" / "p_bucket=2";
  cfg.schema = schema;
  cfg.table_name = "primes";
  cfg.filename_prefix = "primes";
  cfg.delta_columns = {"p", "prime_rank"};
  cfg.stat_columns = {{"p", true}, {"prime_rank", true}};
  cfg.partition_spec = spec;
  cfg.partition_values = std::make_shared<iceberg::PartitionValues>(
      std::vector<iceberg::Literal>{iceberg::Literal::Int(1),
                                    iceberg::Literal::Int(2)});
  cfg.bucket_version = 1;
  cfg.bucket = 2;
  cfg.compression_level = 1;

  std::vector<std::shared_ptr<iceberg::DataFile>> files;
  auto writer = primeparts::BucketParquetWriter::Make(std::move(cfg), &error);
  EXPECT_NE(writer, nullptr) << error;
  if (!writer) return files;
  auto batch = MakePrimesBatch(arrow_schema);
  EXPECT_TRUE(writer->Write(*batch, &error)) << error;
  std::vector<primeparts::WrittenFile> written;
  EXPECT_TRUE(writer->Close(&written, &error)) << error;
  for (auto& wf : written) files.push_back(wf.data_file);
  return files;
}

void BuildPrimesWarehouse(const fs::path& warehouse,
                          const iceberg::Namespace& ns,
                          ppc::PartitionStatsSet* out_stats) {
  std::string error;
  auto local = ppc::MakeLocalCatalogWithStore(warehouse, &error);
  ASSERT_NE(local.catalog, nullptr) << error;
  ASSERT_NE(local.store, nullptr) << error;

  auto schema = primeparts::PrimesSchema();
  auto spec = primeparts::BucketPartitionSpec(*schema, &error);
  ASSERT_NE(spec, nullptr) << error;
  auto arrow_schema =
      primeparts::IcebergToArrowSchemaWithFieldIds(*schema, &error);
  ASSERT_NE(arrow_schema, nullptr) << error;

  ppc::TableDeclaration declare;
  auto seed = WritePrimesFiles(warehouse, ns, schema, spec, arrow_schema);
  ASSERT_FALSE(seed.empty());
  std::string meta;
  ASSERT_TRUE(ppc::CommitFiles(local.catalog, ns, warehouse, "primes", schema,
                               spec, declare, seed, &meta, &error))
      << error;

  ppc::TableCommitSpec cspec;
  cspec.table_name = "primes";
  cspec.schema = schema;
  cspec.spec = spec;
  cspec.files = WritePrimesFiles(warehouse, ns, schema, spec, arrow_schema);
  ASSERT_FALSE(cspec.files.empty());
  std::vector<ppc::TableCommitSpec> specs;
  specs.push_back(std::move(cspec));
  ASSERT_TRUE(ppc::CommitFilesAtomic(local.catalog, local.store, "", ns,
                                     warehouse, specs, &error))
      << error;

  auto table = local.catalog->LoadTable(
      iceberg::TableIdentifier{.ns = ns, .name = "primes"});
  ASSERT_TRUE(table.has_value()) << table.error().message;
  ASSERT_TRUE(ppc::LoadPartitionStats(*table.value(), out_stats, &error))
      << error;
}

class E2ETest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    warehouse_ = fs::temp_directory_path() / "primeparts-e2e-warehouse";
    std::error_code ec;
    fs::remove_all(warehouse_, ec);
    fs::create_directories(warehouse_, ec);
    ns_ = ppc::ResolveNamespace("");

    BuildPrimesWarehouse(warehouse_, ns_, &stats_);
    if (::testing::Test::HasFatalFailure()) return;

    const char* bin_env = std::getenv("PP_CATALOGD_BIN");
    const std::string bin = bin_env ? bin_env : "build/primeparts-catalogd";
    pid_ = fork();
    ASSERT_GE(pid_, 0);
    if (pid_ == 0) {
      execl(bin.c_str(), bin.c_str(), "--warehouse", warehouse_.c_str(),
            "--port", "18181", "--host", "127.0.0.1",
            static_cast<char*>(nullptr));
      _exit(127);
    }

    httplib::Client probe("127.0.0.1", 18181);
    probe.set_connection_timeout(1, 0);
    for (int i = 0; i < 100; ++i) {
      if (auto r = probe.Get("/v1/config"); r && r->status == 200) {
        ready_ = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_TRUE(ready_) << "catalogd did not become ready on :18181 (bin=" << bin
                        << ")";
    setenv("PRIMEPARTS_REST_URI", "http://127.0.0.1:18181", 1);
  }

  static void TearDownTestSuite() {
    unsetenv("PRIMEPARTS_REST_URI");
    if (pid_ > 0) {
      kill(pid_, SIGTERM);
      int status = 0;
      waitpid(pid_, &status, 0);
    }
    std::error_code ec;
    fs::remove_all(warehouse_, ec);
  }

  httplib::Client Client() { return httplib::Client("127.0.0.1", 18181); }

  static fs::path warehouse_;
  static iceberg::Namespace ns_;
  static pid_t pid_;
  static bool ready_;
  static ppc::PartitionStatsSet stats_;
};

fs::path E2ETest::warehouse_;
iceberg::Namespace E2ETest::ns_;
pid_t E2ETest::pid_ = -1;
bool E2ETest::ready_ = false;
ppc::PartitionStatsSet E2ETest::stats_;

TEST_F(E2ETest, ScanByKErrorsWithoutSortOrder) {
  std::string error;
  auto qs = ppq::QueryService::Open(warehouse_, ns_, &error);
  ASSERT_NE(qs, nullptr) << error;
  error.clear();
  auto hits = qs->ScanByK(0, 0, 0, 10, &error);
  EXPECT_TRUE(hits.empty());
  EXPECT_NE(error.find("no ascending sort order"), std::string::npos) << error;
}

TEST_F(E2ETest, WindowedGroupCountErrorsWithoutSortOrder) {
  std::string error;
  auto qs = ppq::QueryService::Open(warehouse_, ns_, &error);
  ASSERT_NE(qs, nullptr) << error;
  error.clear();
  auto rows = qs->GroupCount("primes", ppq::GroupKey::Column("k"), 3, 7, 1,
                             &error);
  EXPECT_NE(error.find("no ascending sort order"), std::string::npos) << error;
}

TEST_F(E2ETest, FullTableReadSynthesizesIdentityColumns) {
  std::string error;
  auto qs = ppq::QueryService::Open(warehouse_, ns_, &error);
  ASSERT_NE(qs, nullptr) << error;
  error.clear();
  auto rows = qs->ReadTable("primes", {}, 100, &error);
  EXPECT_TRUE(error.empty()) << error;
  EXPECT_EQ(rows.rows.size(), 6u);
}

TEST_F(E2ETest, ColumnSubsetReadWorks) {
  std::string error;
  auto qs = ppq::QueryService::Open(warehouse_, ns_, &error);
  ASSERT_NE(qs, nullptr) << error;
  error.clear();
  auto rows = qs->ReadTable("primes", {"p", "k"}, 100, &error);
  EXPECT_TRUE(error.empty()) << error;
  EXPECT_EQ(rows.rows.size(), 6u);
}

TEST_F(E2ETest, PartitionStatsPresentAfterCommit) {
  ASSERT_FALSE(stats_.rows.empty())
      << "partition statistics missing after commit";
  const ppc::PartitionStatsRow* row = nullptr;
  for (const auto& r : stats_.rows) {
    if (r.partition == std::vector<int64_t>{1, 2}) {
      row = &r;
      break;
    }
  }
  ASSERT_NE(row, nullptr) << "no partition stats row for (1, 2)";
  EXPECT_EQ(row->data_file_count, 2);
  EXPECT_EQ(row->data_record_count, 6);
}

TEST_F(E2ETest, PlanRoutesReturn406) {
  auto cli = Client();
  const char* body = "{}";
  const std::string base = "/v1/namespaces/primeparts/tables/primes";

  struct Route {
    std::string method;
    std::string path;
  };
  const std::vector<Route> routes = {
      {"POST", base + "/plan"},
      {"GET", base + "/plan/some-plan-id"},
      {"DELETE", base + "/plan/some-plan-id"},
      {"POST", base + "/tasks"},
  };

  for (const auto& r : routes) {
    httplib::Result res;
    if (r.method == "POST") {
      res = cli.Post(r.path, body, "application/json");
    } else if (r.method == "GET") {
      res = cli.Get(r.path);
    } else {
      res = cli.Delete(r.path);
    }
    ASSERT_TRUE(res) << r.method << " " << r.path << " no response";
    EXPECT_EQ(res->status, 406) << r.method << " " << r.path;
    EXPECT_NE(res->body.find("UnsupportedOperationException"), std::string::npos)
        << r.method << " " << r.path << ": " << res->body;
  }
}

TEST(E2EFreshCommit, PartitionStatsPresentOnFirstCommit) {
  const fs::path wh = fs::temp_directory_path() / "primeparts-e2e-fresh";
  std::error_code ec;
  fs::remove_all(wh, ec);
  fs::create_directories(wh, ec);
  const auto ns = ppc::ResolveNamespace("");

  std::string error;
  auto local = ppc::MakeLocalCatalogWithStore(wh, &error);
  ASSERT_NE(local.catalog, nullptr) << error;
  ASSERT_NE(local.store, nullptr) << error;
  auto schema = primeparts::PrimesSchema();
  auto spec = primeparts::BucketPartitionSpec(*schema, &error);
  ASSERT_NE(spec, nullptr) << error;
  auto arrow_schema =
      primeparts::IcebergToArrowSchemaWithFieldIds(*schema, &error);
  ASSERT_NE(arrow_schema, nullptr) << error;

  ppc::TableCommitSpec cspec;
  cspec.table_name = "primes";
  cspec.schema = schema;
  cspec.spec = spec;
  cspec.files = WritePrimesFiles(wh, ns, schema, spec, arrow_schema);
  ASSERT_FALSE(cspec.files.empty());
  std::vector<ppc::TableCommitSpec> specs;
  specs.push_back(std::move(cspec));
  ASSERT_TRUE(ppc::CommitFilesAtomic(local.catalog, local.store, "", ns, wh,
                                     specs, &error))
      << error;

  auto table = local.catalog->LoadTable(
      iceberg::TableIdentifier{.ns = ns, .name = "primes"});
  ASSERT_TRUE(table.has_value()) << table.error().message;
  ppc::PartitionStatsSet stats;
  ASSERT_TRUE(ppc::LoadPartitionStats(*table.value(), &stats, &error)) << error;
  ASSERT_FALSE(stats.rows.empty())
      << "partition statistics missing on first commit to a fresh table";
  fs::remove_all(wh, ec);
}

}  // namespace
