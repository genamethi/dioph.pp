// pp_commit_smoke — gate test for atomic multi-table commit through pp-catalogd.
//
// Forks the server over a throwaway warehouse and drives the client-side commit
// seam (pp_commit.h):
//   1. Happy path — stage parquet for TWO tables (primes + partitions), commit
//      both in ONE CommitFilesAtomic (daemon transport), verify both snapshots
//      landed with their records.
//   2. Rollback proof — assemble a second valid two-table commit body, tamper the
//      partitions requirement to a stale snapshot id, POST it directly, and assert
//      the server returns 409 AND neither table's head pointer moved (the valid
//      primes change, applied first inside the txn, is rolled back). Atomicity.
// Passing this gates Phase B.

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <arrow/api.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "primeparts/catalog/pp_catalogd.h"
#include "primeparts/catalog/pp_commit.h"
#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/schemas.h"
#include "primeparts/writer.h"

#include "iceberg/catalog.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/partition_spec.h"
#include "iceberg/schema.h"
#include "iceberg/table.h"
#include "iceberg/table_identifier.h"
#include "iceberg/table_scan.h"

namespace fs = std::filesystem;
namespace ppc = primeparts::catalog;
using json = nlohmann::json;

namespace {

constexpr int kPort = 8232;
const char* kRestUri = "http://127.0.0.1:8232";

int failures = 0;
bool Check(bool cond, const std::string& msg) {
  std::printf("[%s] %s\n", cond ? "ok " : "FAIL", msg.c_str());
  if (!cond) ++failures;
  return cond;
}

template <typename T>
std::shared_ptr<arrow::Array> FinishOrDie(T& b) {
  std::shared_ptr<arrow::Array> a;
  if (!b.Finish(&a).ok()) std::exit(1);
  return a;
}

std::shared_ptr<arrow::RecordBatch> MakePrimesBatch(
    const std::shared_ptr<arrow::Schema>& schema) {
  arrow::Int64Builder p;
  arrow::Int32Builder k;
  arrow::Int64Builder prime_rank;
  arrow::Int32Builder bv;
  arrow::Int32Builder b;
  for (int64_t v : {3, 5, 7}) (void)p.Append(v);
  for (int32_t v : {1, 1, 1}) (void)k.Append(v);
  for (int64_t v : {0, 1, 2}) (void)prime_rank.Append(v);
  for (int32_t v : {1, 1, 1}) (void)bv.Append(v);
  for (int32_t v : {0, 0, 0}) (void)b.Append(v);
  return arrow::RecordBatch::Make(
      schema, 3,
      {FinishOrDie(p), FinishOrDie(k), FinishOrDie(prime_rank), FinishOrDie(bv),
       FinishOrDie(b)});
}

std::shared_ptr<arrow::RecordBatch> MakePartitionsBatch(
    const std::shared_ptr<arrow::Schema>& schema) {
  arrow::Int64Builder p;
  arrow::Int32Builder m_k;
  arrow::Int32Builder n_k;
  arrow::Int64Builder q_k;
  arrow::Int64Builder prime_rank;
  arrow::Int32Builder bv;
  arrow::Int32Builder b;
  for (int64_t v : {3, 5, 7}) (void)p.Append(v);
  for (int32_t v : {1, 1, 1}) (void)m_k.Append(v);
  for (int32_t v : {2, 2, 2}) (void)n_k.Append(v);
  for (int64_t v : {6, 10, 14}) (void)q_k.Append(v);
  for (int64_t v : {0, 1, 2}) (void)prime_rank.Append(v);
  for (int32_t v : {1, 1, 1}) (void)bv.Append(v);
  for (int32_t v : {0, 0, 0}) (void)b.Append(v);
  return arrow::RecordBatch::Make(
      schema, 3,
      {FinishOrDie(p), FinishOrDie(m_k), FinishOrDie(n_k), FinishOrDie(q_k),
       FinishOrDie(prime_rank), FinishOrDie(bv), FinishOrDie(b)});
}

// Write one parquet file into the table's STAGING dir (so CommitFilesAtomic
// moves it into the warehouse) and return its Iceberg DataFile.
std::shared_ptr<iceberg::DataFile> WriteStaged(
    const fs::path& warehouse, const std::string& table_name,
    const std::shared_ptr<iceberg::Schema>& schema, bool is_primes,
    int32_t file_seq, std::string* error) {
  auto arrow_schema =
      primeparts::IcebergToArrowSchemaWithFieldIds(*schema, error);
  if (!arrow_schema) return nullptr;
  primeparts::WriterConfig cfg;
  cfg.output_dir = ppc::StagingDataDir(warehouse, table_name) /
                   "p_bucket_version=1" / "p_bucket=0";
  cfg.schema = schema;
  cfg.table_name = table_name;
  cfg.filename_prefix = table_name;
  cfg.delta_columns = {"p", "prime_rank"};
  cfg.bucket_version = 1;
  cfg.bucket = 0;
  cfg.starting_file_seq = file_seq;
  cfg.compression_level = 1;
  auto writer = primeparts::BucketParquetWriter::Make(std::move(cfg), error);
  if (!writer) return nullptr;
  auto batch = is_primes ? MakePrimesBatch(arrow_schema)
                         : MakePartitionsBatch(arrow_schema);
  if (!writer->Write(*batch,
                     {.p_min = 3, .p_max = 7, .rank_min = 0, .rank_max = 2},
                     error))
    return nullptr;
  std::vector<primeparts::WrittenFile> files;
  if (!writer->Close(&files, error)) return nullptr;
  if (files.empty() || !files.front().data_file) {
    *error = "writer produced no DataFile for " + table_name;
    return nullptr;
  }
  return files.front().data_file;
}

bool WaitForServer() {
  httplib::Client cli("127.0.0.1", kPort);
  cli.set_connection_timeout(0, 200000);
  for (int i = 0; i < 100; ++i) {
    if (auto res = cli.Get("/v1/config"); res && res->status == 200) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

// Scan a table through the catalog; returns record count (-1 on failure).
int64_t ScanRecords(const std::shared_ptr<iceberg::Catalog>& catalog,
                    const std::string& name) {
  iceberg::TableIdentifier id{.ns = iceberg::Namespace{{"primeparts"}},
                              .name = name};
  auto loaded = catalog->LoadTable(id);
  if (!loaded.has_value()) return -1;
  auto sb = loaded.value()->NewScan();
  if (!sb.has_value()) return -1;
  auto scan = sb.value()->Build();
  if (!scan.has_value()) return -1;
  auto tasks = scan.value()->PlanFiles();
  if (!tasks.has_value()) return -1;
  int64_t records = 0;
  for (const auto& t : tasks.value()) records += t->data_file()->record_count;
  return records;
}

std::string MetaLoc(const std::shared_ptr<iceberg::Catalog>& catalog,
                    const std::string& name) {
  iceberg::TableIdentifier id{.ns = iceberg::Namespace{{"primeparts"}},
                              .name = name};
  auto loaded = catalog->LoadTable(id);
  if (!loaded.has_value()) return {};
  return std::string(loaded.value()->metadata_file_location());
}

// Replace the requirements of the `partitions` table-change with a stale
// assert-ref so the server rejects the whole transaction.
void TamperPartitionsRequirement(json* body) {
  for (auto& tc : (*body)["table-changes"]) {
    if (tc.contains("identifier") && tc["identifier"].value("name", "") ==
                                         "partitions") {
      tc["requirements"] = json::array(
          {json{{"type", "assert-ref-snapshot-id"},
                {"ref", "main"},
                {"snapshot-id", 999999}}});
    }
  }
}

int RunClient(const fs::path& warehouse) {
  if (!Check(WaitForServer(), "server answered GET /v1/config")) return failures;

  std::string mode, err;
  ppc::RestOptions ropts;
  ropts.rest_uri = kRestUri;
  auto catalog = ppc::MakeCatalog(ropts, warehouse, &mode, &err);
  if (!Check(catalog != nullptr, "RestCatalog client connected: " + err))
    return failures;

  auto primes_schema = primeparts::PrimesSchema();
  auto parts_schema = primeparts::PartitionsSchema();
  auto primes_spec = primeparts::BucketPartitionSpec(*primes_schema, &err);
  auto parts_spec = primeparts::BucketPartitionSpec(*parts_schema, &err);
  if (!Check(primes_spec && parts_spec, "built partition specs: " + err))
    return failures;

  // --- 1. Happy path: two-table atomic commit (daemon transport) ------------
  auto df_p = WriteStaged(warehouse, "primes", primes_schema, true, 0, &err);
  auto df_q = WriteStaged(warehouse, "partitions", parts_schema, false, 0, &err);
  if (!Check(df_p && df_q, "staged parquet for both tables: " + err))
    return failures;

  std::vector<ppc::TableCommitSpec> specs = {
      {"partitions", parts_schema, parts_spec, {df_q}},
      {"primes", primes_schema, primes_spec, {df_p}},
  };
  bool ok = ppc::CommitFilesAtomic(catalog, /*store=*/nullptr, kRestUri,
                                   warehouse, specs, &err);
  Check(ok, "atomic two-table commit: " + err);

  Check(ScanRecords(catalog, "primes") == 3, "primes has 3 records");
  Check(ScanRecords(catalog, "partitions") == 3, "partitions has 3 records");

  const std::string primes_loc_1 = MetaLoc(catalog, "primes");
  const std::string parts_loc_1 = MetaLoc(catalog, "partitions");
  Check(!primes_loc_1.empty() && !parts_loc_1.empty(),
        "captured post-commit metadata locations");

  // --- 2. Rollback proof: stale requirement aborts the whole transaction -----
  auto df_p2 = WriteStaged(warehouse, "primes", primes_schema, true, 1, &err);
  auto df_q2 = WriteStaged(warehouse, "partitions", parts_schema, false, 1, &err);
  if (!Check(df_p2 && df_q2, "staged parquet for round 2: " + err))
    return failures;

  // primes FIRST so its (valid) change is applied inside the txn before the
  // tampered partitions change aborts it — proving the applied change rolls back.
  std::vector<ppc::TableCommitSpec> specs2 = {
      {"primes", primes_schema, primes_spec, {df_p2}},
      {"partitions", parts_schema, parts_spec, {df_q2}},
  };
  std::string body;
  if (!Check(ppc::AssembleTransactionBody(catalog, warehouse, specs2, &body, &err),
             "assembled round-2 transaction body: " + err))
    return failures;

  json j = json::parse(body);
  TamperPartitionsRequirement(&j);

  httplib::Client cli(kRestUri);
  cli.set_read_timeout(30, 0);
  auto res = cli.Post("/v1/transactions/commit", j.dump(), "application/json");
  Check(res && res->status == 409,
        "stale requirement -> HTTP 409 (got " +
            (res ? std::to_string(res->status) : std::string("no response")) +
            ")");

  Check(MetaLoc(catalog, "primes") == primes_loc_1,
        "primes head unmoved after rollback (atomic)");
  Check(MetaLoc(catalog, "partitions") == parts_loc_1,
        "partitions head unmoved after rollback");
  Check(ScanRecords(catalog, "primes") == 3, "primes still 3 records");

  return failures;
}

}  // namespace

int main() {
  char tmpl[] = "/tmp/ppcommit-smoke-XXXXXX";
  const char* dir = mkdtemp(tmpl);
  if (!dir) {
    std::perror("mkdtemp");
    return 1;
  }
  const fs::path warehouse = dir;

  pid_t child = fork();
  if (child < 0) {
    std::perror("fork");
    return 1;
  }
  if (child == 0) {
    ppc::CatalogdOptions opts;
    opts.warehouse = warehouse.string();
    opts.host = "127.0.0.1";
    opts.port = kPort;
    _exit(ppc::RunCatalogd(opts));
  }

  std::printf("== pp-commit atomic-transaction smoke ==\n  warehouse: %s\n",
              warehouse.c_str());
  int fails = RunClient(warehouse);

  kill(child, SIGTERM);
  int status = 0;
  waitpid(child, &status, 0);

  std::error_code ec;
  fs::remove_all(warehouse, ec);

  std::printf("\n== pp-commit-smoke %s ==\n", fails == 0 ? "PASS" : "FAIL");
  return fails == 0 ? 0 : 1;
}
