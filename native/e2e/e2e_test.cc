#include "primeparts/catalog/partition_stats.h"
#include "primeparts/catalog/pp_commit.h"
#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/common/arrow_init.h"
#include "primeparts/query/query_service.h"
#include "primeparts/scan/scan_planner.h"
#include "primeparts/schemas.h"
#include "primeparts/writer.h"

#include <arrow/api.h>
#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "iceberg/catalog.h"
#include "iceberg/expression/expressions.h"
#include "iceberg/expression/literal.h"
#include "iceberg/table_metadata.h"
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
  static std::shared_ptr<iceberg::TableMetadata> LoadPrimesMetadata() {
    primeparts::common::EnsureArrowRegistration();
    fs::path dir = warehouse_;
    for (const auto& level : ns_.levels) dir /= level;
    dir = dir / "primes" / "metadata";

    fs::path latest;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      const std::string name = entry.path().filename().string();
      if (name.size() > 14 &&
          name.compare(name.size() - 14, 14, ".metadata.json") == 0 &&
          name > latest.filename().string()) {
        latest = entry.path();
      }
    }
    EXPECT_FALSE(latest.empty()) << "no metadata json under " << dir;
    if (latest.empty()) return nullptr;

    auto io = ppc::LocalIO();
    auto md = iceberg::TableMetadataUtil::Read(*io, latest.string());
    EXPECT_TRUE(md.has_value()) << (md.has_value() ? "" : md.error().message);
    if (!md.has_value()) return nullptr;
    return std::move(md.value());
  }

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

TEST_F(E2ETest, PlanRoutesMatchSpecStatuses) {
  auto cli = Client();
  const char* body = "{}";
  const std::string base = "/v1/namespaces/primeparts/tables/primes";

  struct Route {
    std::string method;
    std::string path;
    int status;
    std::string type;
  };
  const std::vector<Route> routes = {
      {"POST", base + "/plan", 406, "UnsupportedOperationException"},
      {"GET", base + "/plan/some-plan-id", 404, "NoSuchPlanIdException"},
      {"DELETE", base + "/plan/some-plan-id", 404, "NoSuchPlanIdException"},
      {"POST", base + "/tasks", 404, "NoSuchPlanTaskException"},
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
    EXPECT_EQ(res->status, r.status) << r.method << " " << r.path;
    EXPECT_NE(res->body.find(r.type), std::string::npos)
        << r.method << " " << r.path << ": " << res->body;
  }
}

TEST_F(E2ETest, ExistenceChecksReturn204) {
  auto cli = Client();

  auto ns = cli.Head("/v1/namespaces/primeparts");
  ASSERT_TRUE(ns) << "HEAD namespace: no response";
  EXPECT_EQ(ns->status, 204);

  auto table = cli.Head("/v1/namespaces/primeparts/tables/primes");
  ASSERT_TRUE(table) << "HEAD table: no response";
  EXPECT_EQ(table->status, 204);

  auto missing_ns = cli.Head("/v1/namespaces/no-such-namespace");
  ASSERT_TRUE(missing_ns) << "HEAD missing namespace: no response";
  EXPECT_EQ(missing_ns->status, 404);

  auto missing_table = cli.Head("/v1/namespaces/primeparts/tables/no-such-table");
  ASSERT_TRUE(missing_table) << "HEAD missing table: no response";
  EXPECT_EQ(missing_table->status, 404);
}

TEST_F(E2ETest, MinRowsRequestedStopsPlanningEarly) {
  auto metadata = LoadPrimesMetadata();
  ASSERT_NE(metadata, nullptr);
  auto io = ppc::LocalIO();
  std::string error;

  primeparts::scan::ScanPlanRequest full_request;
  primeparts::scan::ScanPlan full;
  ASSERT_TRUE(primeparts::scan::PlanTableScan(metadata, io, full_request, &full,
                                              &error))
      << error;
  ASSERT_GT(full.tasks.size(), 1u) << "fixture must plan more than one task";

  primeparts::scan::ScanPlanRequest capped_request;
  capped_request.min_rows_requested = 1;
  primeparts::scan::ScanPlan capped;
  ASSERT_TRUE(primeparts::scan::PlanTableScan(metadata, io, capped_request,
                                              &capped, &error))
      << error;

  EXPECT_LT(capped.tasks.size(), full.tasks.size());
  EXPECT_GE(capped.planned_rows, 1);
  EXPECT_LT(capped.planned_rows, full.planned_rows);
}

TEST_F(E2ETest, MinRowsRequestedDoesNotStopOnUnprovenRowCounts) {
  auto metadata = LoadPrimesMetadata();
  ASSERT_NE(metadata, nullptr);
  auto io = ppc::LocalIO();
  std::string error;

  auto filter = iceberg::Expressions::Equal("k", iceberg::Literal::Int(1));

  primeparts::scan::ScanPlanRequest unbounded;
  unbounded.filter = filter;
  primeparts::scan::ScanPlan unbounded_plan;
  ASSERT_TRUE(primeparts::scan::PlanTableScan(metadata, io, unbounded,
                                              &unbounded_plan, &error))
      << error;
  ASSERT_GT(unbounded_plan.tasks.size(), 1u)
      << "fixture must plan more than one task, or the comparison below is "
         "vacuous";

  primeparts::scan::ScanPlanRequest bounded;
  bounded.filter = filter;
  bounded.min_rows_requested = 1;
  primeparts::scan::ScanPlan bounded_plan;
  ASSERT_TRUE(primeparts::scan::PlanTableScan(metadata, io, bounded,
                                              &bounded_plan, &error))
      << error;

  EXPECT_EQ(bounded_plan.tasks.size(), unbounded_plan.tasks.size())
      << "row counts under a non-trivial residual are upper bounds, so "
         "min-rows-requested must not stop planning early";
  EXPECT_EQ(bounded_plan.planned_rows, unbounded_plan.planned_rows);
}

TEST_F(E2ETest, ResidualStaysCompleteAndKeyWindowIsDerived) {
  auto metadata = LoadPrimesMetadata();
  ASSERT_NE(metadata, nullptr);
  auto io = ppc::LocalIO();
  std::string error;

  primeparts::scan::ScanPlanRequest request;
  request.filter = iceberg::Expressions::And(
      iceberg::Expressions::Equal("k", iceberg::Literal::Int(1)),
      iceberg::Expressions::GreaterThanOrEqual("p", iceberg::Literal::Long(5)));

  primeparts::scan::ScanPlan plan;
  ASSERT_TRUE(
      primeparts::scan::PlanTableScan(metadata, io, request, &plan, &error))
      << error;

  ASSERT_NE(plan.residual, nullptr);
  EXPECT_EQ(plan.residual->ToString(), request.filter->ToString())
      << "the key conjunct must remain in the residual: a consumer that "
         "declines the key window has to stay correct";
}

TEST_F(E2ETest, ConfigAdvertisesSupersetOfSpecDefaultEndpoints) {
  auto cli = Client();
  auto res = cli.Get("/v1/config");
  ASSERT_TRUE(res) << "GET /v1/config: no response";
  ASSERT_EQ(res->status, 200);

  auto body = nlohmann::json::parse(res->body, nullptr, false);
  ASSERT_FALSE(body.is_discarded()) << res->body;
  ASSERT_TRUE(body.contains("endpoints")) << res->body;

  std::set<std::string> advertised;
  for (const auto& e : body["endpoints"]) advertised.insert(e.get<std::string>());

  const std::vector<std::string> spec_default = {
      "GET /v1/{prefix}/namespaces",
      "POST /v1/{prefix}/namespaces",
      "GET /v1/{prefix}/namespaces/{namespace}",
      "DELETE /v1/{prefix}/namespaces/{namespace}",
      "POST /v1/{prefix}/namespaces/{namespace}/properties",
      "GET /v1/{prefix}/namespaces/{namespace}/tables",
      "POST /v1/{prefix}/namespaces/{namespace}/tables",
      "GET /v1/{prefix}/namespaces/{namespace}/tables/{table}",
      "POST /v1/{prefix}/namespaces/{namespace}/tables/{table}",
      "DELETE /v1/{prefix}/namespaces/{namespace}/tables/{table}",
      "POST /v1/{prefix}/namespaces/{namespace}/register",
      "POST /v1/{prefix}/namespaces/{namespace}/tables/{table}/metrics",
      "POST /v1/{prefix}/tables/rename",
      "POST /v1/{prefix}/transactions/commit",
  };
  for (const auto& e : spec_default) {
    EXPECT_TRUE(advertised.count(e) == 1)
        << "advertising `endpoints` withdraws this spec-default route: " << e;
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
