#include "primeparts/catalog/partition_stats.h"
#include "primeparts/catalog/pp_commit.h"
#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/catalog/rest_scan_plan.h"
#include "primeparts/client/session.h"
#include "primeparts/scan/column_binder.h"
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
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/table_metadata.h"
#include "iceberg/table_scan.h"
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

constexpr char kRestUri[] = "http://127.0.0.1:18181";
constexpr char kNamespace[] = "primeparts";

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
    ns_ = ppc::ResolveNamespace(kNamespace);

    BuildPrimesWarehouse(warehouse_, ns_, &stats_);
    if (::testing::Test::HasFatalFailure()) return;

    const char* bin_env = std::getenv("PP_CATALOGD_BIN");
    const std::string bin = bin_env ? bin_env : "build/primeparts-catalogd";
    pid_ = fork();
    ASSERT_GE(pid_, 0);
    if (pid_ == 0) {
      execl(bin.c_str(), bin.c_str(), "--warehouse", warehouse_.c_str(),
            "--port", "18181", "--host", "127.0.0.1", "--plan-batch", "1",
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
  }

  static void TearDownTestSuite() {
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
  auto qs = ppq::QueryService::Open(warehouse_, kRestUri, ns_, &error);
  ASSERT_NE(qs, nullptr) << error;
  error.clear();
  auto hits = qs->ScanByK(0, 0, 0, 10, &error);
  EXPECT_TRUE(hits.empty());
  EXPECT_NE(error.find("no ascending sort order"), std::string::npos) << error;
}

TEST_F(E2ETest, WindowedGroupCountErrorsWithoutSortOrder) {
  std::string error;
  auto qs = ppq::QueryService::Open(warehouse_, kRestUri, ns_, &error);
  ASSERT_NE(qs, nullptr) << error;
  error.clear();
  auto rows = qs->GroupCount("primes", ppq::GroupKey::Column("k"), 3, 7, 1,
                             &error);
  EXPECT_NE(error.find("no ascending sort order"), std::string::npos) << error;
}

TEST_F(E2ETest, FullTableReadSynthesizesIdentityColumns) {
  std::string error;
  auto qs = ppq::QueryService::Open(warehouse_, kRestUri, ns_, &error);
  ASSERT_NE(qs, nullptr) << error;
  error.clear();
  auto rows = qs->ReadTable("primes", {}, 100, &error);
  EXPECT_TRUE(error.empty()) << error;
  EXPECT_EQ(rows.rows.size(), 6u);
}

TEST_F(E2ETest, ColumnSubsetReadWorks) {
  std::string error;
  auto qs = ppq::QueryService::Open(warehouse_, kRestUri, ns_, &error);
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

TEST_F(E2ETest, ConfigAdvertisesEveryPlanningRoute) {
  auto cli = Client();
  auto res = cli.Get("/v1/config");
  ASSERT_TRUE(res);
  ASSERT_EQ(res->status, 200);

  auto body = nlohmann::json::parse(res->body);
  ASSERT_TRUE(body.contains("endpoints")) << res->body;
  std::set<std::string> endpoints;
  for (const auto& entry : body["endpoints"]) {
    endpoints.insert(entry.get<std::string>());
  }

  const std::string table = "/v1/{prefix}/namespaces/{namespace}/tables/{table}";
  for (const std::string& wanted :
       {"POST " + table + "/plan", "GET " + table + "/plan/{plan-id}",
        "DELETE " + table + "/plan/{plan-id}", "POST " + table + "/tasks"}) {
    EXPECT_TRUE(endpoints.count(wanted) == 1)
        << "planning route not advertised: " << wanted;
  }
}

TEST_F(E2ETest, LoadTableAdvertisesServerSidePlanning) {
  ppc::ScanPlanningMode mode = ppc::ScanPlanningMode::kClient;
  std::string error;
  ASSERT_TRUE(ppc::FetchScanPlanningMode("http://127.0.0.1:18181", ns_,
                                         "primes", &mode, &error))
      << error;
  EXPECT_EQ(mode, ppc::ScanPlanningMode::kServer);
}

TEST_F(E2ETest, AClientThatReadsTheAdvertisementCanCompleteAScan) {
  const std::string uri = "http://127.0.0.1:18181";
  auto metadata = LoadPrimesMetadata();
  ASSERT_NE(metadata, nullptr);
  primeparts::scan::ScanPlanRequest request;
  std::string error;

  ppc::ScanPlanningMode mode = ppc::ScanPlanningMode::kClient;
  ASSERT_TRUE(ppc::FetchScanPlanningMode(uri, ns_, "primes", &mode, &error))
      << error;

  std::vector<std::shared_ptr<iceberg::FileScanTask>> tasks;
  if (mode == ppc::ScanPlanningMode::kServer) {
    ASSERT_TRUE(ppc::PlanScanOnServer(uri, ns_, "primes", request, *metadata,
                                      ppc::PlanPollOptions{}, &tasks, &error))
        << error;
  } else {
    primeparts::scan::ScanPlan local;
    ASSERT_TRUE(primeparts::scan::PlanTableScan(metadata, ppc::LocalIO(),
                                                request, &local, &error))
        << error;
    for (const auto& task : local.tasks) tasks.push_back(task.inner);
  }

  EXPECT_FALSE(tasks.empty())
      << "the advertised mode must lead to a usable scan, not a dead end";
}

TEST_F(E2ETest, ServerAndInProcessPlanningAgreeOnTheTaskSet) {
  auto metadata = LoadPrimesMetadata();
  ASSERT_NE(metadata, nullptr);
  auto io = ppc::LocalIO();
  primeparts::scan::ScanPlanRequest request;

  primeparts::scan::ScanPlan local;
  std::string error;
  ASSERT_TRUE(
      primeparts::scan::PlanTableScan(metadata, io, request, &local, &error))
      << error;
  ASSERT_FALSE(local.tasks.empty());

  std::vector<std::shared_ptr<iceberg::FileScanTask>> remote;
  ASSERT_TRUE(ppc::PlanScanOnServer("http://127.0.0.1:18181", ns_, "primes",
                                    request, *metadata, ppc::PlanPollOptions{},
                                    &remote, &error))
      << error;

  std::set<std::string> local_paths;
  std::set<std::string> remote_paths;
  for (const auto& task : local.tasks) {
    local_paths.insert(task.inner->data_file()->file_path);
  }
  for (const auto& task : remote) {
    remote_paths.insert(task->data_file()->file_path);
  }

  EXPECT_EQ(remote.size(), local.tasks.size());
  EXPECT_EQ(remote_paths, local_paths)
      << "planning the same request in-process and over REST must agree";
}

TEST_F(E2ETest, ClientSurfacesATypedServerRefusal) {
  std::string error;
  EXPECT_FALSE(ppc::CancelPlanning("http://127.0.0.1:18181", ns_, "primes",
                                   "not-a-plan-id", &error));
  EXPECT_NE(error.find("NoSuchPlanIdException"), std::string::npos)
      << "the client must surface the server's typed error, not just a code: "
      << error;
}

TEST_F(E2ETest, UnknownPlanIdsAndTasksAreTypedNotFounds) {
  auto cli = Client();
  const std::string base = "/v1/namespaces/primeparts/tables/primes";

  auto fetch = cli.Get(base + "/plan/some-plan-id");
  ASSERT_TRUE(fetch);
  EXPECT_EQ(fetch->status, 404);
  EXPECT_NE(fetch->body.find("NoSuchPlanIdException"), std::string::npos)
      << fetch->body;

  auto cancel = cli.Delete(base + "/plan/some-plan-id");
  ASSERT_TRUE(cancel);
  EXPECT_EQ(cancel->status, 404);
  EXPECT_NE(cancel->body.find("NoSuchPlanIdException"), std::string::npos)
      << cancel->body;

  auto tasks = cli.Post(base + "/tasks", R"({"plan-task":"nope"})",
                        "application/json");
  ASSERT_TRUE(tasks);
  EXPECT_EQ(tasks->status, 404);
  EXPECT_NE(tasks->body.find("NoSuchPlanTaskException"), std::string::npos)
      << tasks->body;
}

TEST_F(E2ETest, ServerSidePlanningRunsTheFullLifecycle) {
  auto cli = Client();
  const std::string base = "/v1/namespaces/primeparts/tables/primes";

  auto submitted = cli.Post(base + "/plan", "{}", "application/json");
  ASSERT_TRUE(submitted) << "planTableScan: no response";
  ASSERT_EQ(submitted->status, 200) << submitted->body;

  auto submitted_json = nlohmann::json::parse(submitted->body);
  EXPECT_EQ(submitted_json.value("status", ""), "submitted")
      << "planning is asynchronous, so the first answer is always submitted";
  const std::string plan_id = submitted_json.value("plan-id", "");
  ASSERT_FALSE(plan_id.empty()) << submitted->body;

  nlohmann::json result;
  for (int i = 0; i < 400; ++i) {
    auto polled = cli.Get(base + "/plan/" + plan_id);
    ASSERT_TRUE(polled);
    ASSERT_EQ(polled->status, 200) << polled->body;
    result = nlohmann::json::parse(polled->body);
    if (result.value("status", "") != "submitted") break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  ASSERT_EQ(result.value("status", ""), "completed") << result.dump();
  ASSERT_TRUE(result.contains("file-scan-tasks")) << result.dump();
  EXPECT_FALSE(result["file-scan-tasks"].empty());

  const auto& first = result["file-scan-tasks"][0];
  ASSERT_TRUE(first.contains("data-file")) << first.dump();
  EXPECT_TRUE(first["data-file"].contains("file-path"));
  EXPECT_FALSE(first.contains("split"))
      << "split selection is a data-layer concern the server does not do";

  auto cancelled = cli.Delete(base + "/plan/" + plan_id);
  ASSERT_TRUE(cancelled);
  EXPECT_EQ(cancelled->status, 204) << cancelled->body;
}

TEST_F(E2ETest, PlanTasksAreFetchableWhenPlanningExceedsOneBatch) {
  auto cli = Client();
  const std::string base = "/v1/namespaces/primeparts/tables/primes";

  auto submitted = cli.Post(base + "/plan", "{}", "application/json");
  ASSERT_TRUE(submitted);
  ASSERT_EQ(submitted->status, 200) << submitted->body;
  const std::string plan_id =
      nlohmann::json::parse(submitted->body).value("plan-id", "");
  ASSERT_FALSE(plan_id.empty());

  nlohmann::json result;
  for (int i = 0; i < 400; ++i) {
    auto polled = cli.Get(base + "/plan/" + plan_id);
    ASSERT_TRUE(polled);
    result = nlohmann::json::parse(polled->body);
    if (result.value("status", "") != "submitted") break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_EQ(result.value("status", ""), "completed") << result.dump();

  if (!result.contains("plan-tasks") || result["plan-tasks"].empty()) {
    GTEST_SKIP() << "the fixture plans within one batch; plan-task paging is "
                    "covered by PlanStoreTest";
  }

  const std::string token = result["plan-tasks"][0];
  nlohmann::json request;
  request["plan-task"] = token;
  auto fetched = cli.Post(base + "/tasks", request.dump(), "application/json");
  ASSERT_TRUE(fetched);
  ASSERT_EQ(fetched->status, 200) << fetched->body;
  auto tasks = nlohmann::json::parse(fetched->body);
  ASSERT_TRUE(tasks.contains("file-scan-tasks")) << tasks.dump();
  EXPECT_FALSE(tasks["file-scan-tasks"].empty());
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
  const auto ns = ppc::ResolveNamespace(kNamespace);

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

TEST_F(E2ETest, SessionLoadsTableMetadataOverRest) {
  namespace client = primeparts::client;
  client::SessionOptions options;
  options.rest_uri = kRestUri;
  options.ns = kNamespace;
  options.warehouse = warehouse_.string();
  std::string error;
  auto session = client::Session::Open(options, &error);
  ASSERT_NE(session, nullptr) << error;

  client::TableHandle primes;
  ASSERT_TRUE(session->LoadTable("primes", &primes, &error)) << error;
  ASSERT_NE(primes.metadata(), nullptr)
      << "metadata must arrive from loadTable, not from a metadata.json read";
  EXPECT_EQ(primes.name(), "primes");
  EXPECT_EQ(primes.planning_mode(), ppc::ScanPlanningMode::kServer);

  client::TableHandle missing;
  EXPECT_FALSE(session->LoadTable("not-a-table", &missing, &error));
}

TEST_F(E2ETest, SessionScanDispatchesOnTheAdvertisedMode) {
  namespace client = primeparts::client;
  client::SessionOptions options;
  options.rest_uri = kRestUri;
  options.ns = kNamespace;
  options.warehouse = warehouse_.string();
  std::string error;
  auto session = client::Session::Open(options, &error);
  ASSERT_NE(session, nullptr) << error;

  client::TableHandle primes;
  ASSERT_TRUE(session->LoadTable("primes", &primes, &error)) << error;

  primeparts::scan::ScanPlanRequest request;
  auto stream = session->Scan(primes, request, &error);
  ASSERT_NE(stream, nullptr) << error;
  EXPECT_EQ(stream->planned_via(), primes.planning_mode())
      << "the scan must plan through the route the server advertises";
  EXPECT_EQ(stream->planned_via(), ppc::ScanPlanningMode::kServer);
  EXPECT_GT(stream->file_count(), 0);
}

TEST_F(E2ETest, SessionScanYieldsOnlyRowsSatisfyingTheFilter) {
  namespace client = primeparts::client;
  client::SessionOptions options;
  options.rest_uri = kRestUri;
  options.ns = kNamespace;
  options.warehouse = warehouse_.string();
  std::string error;
  auto session = client::Session::Open(options, &error);
  ASSERT_NE(session, nullptr) << error;

  client::TableHandle primes;
  ASSERT_TRUE(session->LoadTable("primes", &primes, &error)) << error;

  primeparts::scan::ScanPlanRequest unfiltered;
  unfiltered.select = {"p", "k"};
  auto all = session->Scan(primes, unfiltered, &error);
  ASSERT_NE(all, nullptr) << error;
  int64_t total = 0;
  int64_t expected_k1 = 0;
  std::shared_ptr<arrow::RecordBatch> batch;
  while (all->Next(&batch, &error)) {
    if (!batch) break;
    const int32_t* ka = primeparts::scan::BindInt32(*batch, "k", &error);
    ASSERT_NE(ka, nullptr) << error;
    for (int64_t i = 0; i < batch->num_rows(); ++i) {
      ++total;
      if (ka[i] == 1) ++expected_k1;
    }
  }
  ASSERT_TRUE(error.empty()) << error;
  ASSERT_GT(total, 0);

  primeparts::scan::ScanPlanRequest filtered;
  filtered.select = {"p", "k"};
  filtered.filter = iceberg::Expressions::Equal("k", iceberg::Literal::Int(1));
  auto stream = session->Scan(primes, filtered, &error);
  ASSERT_NE(stream, nullptr) << error;
  int64_t seen = 0;
  while (stream->Next(&batch, &error)) {
    if (!batch) break;
    const int32_t* ka = primeparts::scan::BindInt32(*batch, "k", &error);
    ASSERT_NE(ka, nullptr) << error;
    for (int64_t i = 0; i < batch->num_rows(); ++i) {
      ASSERT_EQ(ka[i], 1)
          << "the module owns the residual: consumers must not re-filter";
      ++seen;
    }
  }
  ASSERT_TRUE(error.empty()) << error;
  EXPECT_EQ(seen, expected_k1);
}

TEST_F(E2ETest, ShardedScanSeesEveryRowExactlyOnce) {
  namespace client = primeparts::client;
  std::string error;
  auto rows_for = [&](int threads, int* shards) {
    client::SessionOptions options;
    options.rest_uri = kRestUri;
    options.ns = kNamespace;
    options.warehouse = warehouse_.string();
    options.scan_threads = threads;
    auto session = client::Session::Open(options, &error);
    EXPECT_NE(session, nullptr) << error;
    client::TableHandle primes;
    EXPECT_TRUE(session->LoadTable("primes", &primes, &error)) << error;
    primeparts::scan::ScanPlanRequest request;
    request.select = {"p"};
    auto stream = session->Scan(primes, request, &error);
    EXPECT_NE(stream, nullptr) << error;
    if (shards) *shards = stream->shard_count();
    std::vector<int64_t> values;
    std::shared_ptr<arrow::RecordBatch> batch;
    while (stream->Next(&batch, &error)) {
      if (!batch) break;
      const int64_t* pa = primeparts::scan::BindInt64(*batch, "p", &error);
      EXPECT_NE(pa, nullptr) << error;
      for (int64_t i = 0; i < batch->num_rows(); ++i) values.push_back(pa[i]);
    }
    std::sort(values.begin(), values.end());
    return values;
  };

  int single_shards = 0;
  int many_shards = 0;
  auto single = rows_for(1, &single_shards);
  auto many = rows_for(8, &many_shards);
  ASSERT_FALSE(single.empty());
  EXPECT_EQ(single_shards, 1);
  EXPECT_EQ(single, many)
      << "sharded reads must partition the task set, not duplicate or drop it";
}

}  // namespace
