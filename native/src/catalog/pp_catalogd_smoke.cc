// pp_catalogd_smoke — acceptance test for the pp-catalogd IRC server.
//
// Forks the server in-process (child runs RunCatalogd over a throwaway temp
// warehouse) and drives it with the *unchanged* iceberg-cpp RestCatalog client:
//   GET /v1/config (readiness) -> createNamespace -> createTable -> write a real
//   parquet data file -> FastAppend commit (routes through updateTable) ->
//   reload + scan + assert record count -> dropTable.
// This exercises the create + commit-contract serde paths end to end and is the
// regression test for the server. Returns 0 on success.

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

#include "primeparts/catalog/pp_catalogd.h"
#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/schemas.h"
#include "primeparts/writer.h"

#include "iceberg/catalog.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/partition_spec.h"
#include "iceberg/schema.h"
#include "iceberg/sort_order.h"
#include "iceberg/table.h"
#include "iceberg/table_identifier.h"
#include "iceberg/table_scan.h"
#include "iceberg/update/fast_append.h"

namespace fs = std::filesystem;
namespace ppc = primeparts::catalog;

namespace {

constexpr int kPort = 8231;
const char* kRestUri = "http://127.0.0.1:8231";

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

// A 3-row (p,k,prime_rank,p_bucket_version,p_bucket) batch — same shape the
// writer test uses, matching PrimesSchema + BucketPartitionSpec.
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
  for (int32_t v : {2, 2, 2}) (void)b.Append(v);
  return arrow::RecordBatch::Make(schema, 3,
                                  {FinishOrDie(p), FinishOrDie(k), FinishOrDie(prime_rank),
                                   FinishOrDie(bv), FinishOrDie(b)});
}

// Write one parquet file under <table_loc>/data/... and return its Iceberg
// DataFile. nullptr on failure.
std::shared_ptr<iceberg::DataFile> WriteDataFile(const fs::path& table_loc,
                                                 std::string* error) {
  auto schema = primeparts::PrimesSchema();
  auto arrow_schema = primeparts::IcebergToArrowSchemaWithFieldIds(*schema, error);
  if (!arrow_schema) return nullptr;
  primeparts::WriterConfig cfg;
  cfg.output_dir = table_loc / "data" / "p_bucket_version=1" / "p_bucket=2";
  cfg.schema = schema;
  cfg.table_name = "primes";
  cfg.filename_prefix = "primes";
  cfg.delta_columns = {"p", "prime_rank"};
  cfg.bucket_version = 1;
  cfg.bucket = 2;
  cfg.compression_level = 1;
  auto writer = primeparts::BucketParquetWriter::Make(std::move(cfg), error);
  if (!writer) return nullptr;
  auto batch = MakePrimesBatch(arrow_schema);
  if (!writer->Write(*batch, {.p_min = 3, .p_max = 7, .rank_min = 0, .rank_max = 2}, error))
    return nullptr;
  std::vector<primeparts::WrittenFile> files;
  if (!writer->Close(&files, error)) return nullptr;
  if (files.empty() || !files.front().data_file) {
    *error = "writer produced no DataFile";
    return nullptr;
  }
  return files.front().data_file;
}

bool WaitForServer() {
  httplib::Client cli("127.0.0.1", kPort);
  cli.set_connection_timeout(0, 200000);  // 200ms
  for (int i = 0; i < 100; ++i) {
    if (auto res = cli.Get("/v1/config"); res && res->status == 200) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

// The client round-trip. Returns the failure count.
int RunClient(const fs::path& warehouse) {
  if (!Check(WaitForServer(), "server answered GET /v1/config")) return failures;

  std::string mode, err;
  ppc::RestOptions ropts;
  ropts.rest_uri = kRestUri;
  auto catalog = ppc::MakeCatalog(ropts, warehouse, &mode, &err);
  if (!Check(catalog != nullptr, "RestCatalog client connected: " + err)) return failures;
  Check(mode == "rest", "client is in rest mode");

  const iceberg::Namespace ns{{"primeparts"}};
  Check(ppc::EnsureNamespace(catalog, ns, &err), "createNamespace(primeparts): " + err);

  // createTable via the client -> server.
  auto schema = primeparts::PrimesSchema();
  auto spec = primeparts::BucketPartitionSpec(*schema, &err);
  if (!Check(spec != nullptr, "built BucketPartitionSpec: " + err)) return failures;
  const fs::path table_loc = warehouse / "primeparts" / "smoke_primes";
  std::error_code ec;
  fs::create_directories(table_loc / "metadata", ec);  // arrow FileIO won't mkdir parents
  iceberg::TableIdentifier id{.ns = ns, .name = "smoke_primes"};
  auto created = catalog->CreateTable(id, schema, spec, iceberg::SortOrder::Unsorted(),
                                      table_loc.string(),
                                      {{"write.parquet.compression-codec", "zstd"}});
  if (!Check(created.has_value(),
             std::string("createTable -> server: ") +
                 (created.has_value() ? "" : created.error().message)))
    return failures;
  auto table = created.value();

  // Write a real data file, then FastAppend it — this Commit() routes through
  // RestCatalog::UpdateTable -> POST /v1/.../tables/smoke_primes (the commit
  // contract: {requirements, updates}).
  auto df = WriteDataFile(table_loc, &err);
  if (!Check(df != nullptr, "wrote parquet data file: " + err)) return failures;

  auto fa = table->NewFastAppend();
  if (!Check(fa.has_value(),
             std::string("NewFastAppend: ") + (fa.has_value() ? "" : fa.error().message)))
    return failures;
  fa.value()->AppendFile(df);
  auto commit = fa.value()->Commit();
  Check(commit.has_value(),
        std::string("FastAppend commit -> updateTable: ") +
            (commit.has_value() ? "" : commit.error().message));

  // Reload through the catalog and verify the snapshot landed with 3 records.
  auto reloaded = catalog->LoadTable(id);
  if (!Check(reloaded.has_value(),
             std::string("reload after commit: ") +
                 (reloaded.has_value() ? "" : reloaded.error().message)))
    return failures;
  int64_t records = 0, data_files = 0;
  {
    auto sb = reloaded.value()->NewScan();
    auto scan = sb.has_value() ? sb.value()->Build() : decltype(sb.value()->Build()){};
    if (Check(sb.has_value() && scan.has_value(), "built table scan")) {
      auto tasks = scan.value()->PlanFiles();
      if (Check(tasks.has_value(), "planned files")) {
        for (const auto& t : tasks.value()) {
          ++data_files;
          records += t->data_file()->record_count;
        }
      }
    }
  }
  Check(data_files == 1, "scan sees 1 data file (got " + std::to_string(data_files) + ")");
  Check(records == 3, "scan sees 3 records (got " + std::to_string(records) + ")");

  // dropTable (no purge: keep the shared file; we delete the temp warehouse).
  auto dropped = catalog->DropTable(id, /*purge=*/false);
  Check(dropped.has_value(),
        std::string("dropTable: ") + (dropped.has_value() ? "" : dropped.error().message));
  return failures;
}

}  // namespace

int main() {
  char tmpl[] = "/tmp/ppcatalogd-smoke-XXXXXX";
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
    // Server process. Inherits the temp warehouse.
    ppc::CatalogdOptions opts;
    opts.warehouse = warehouse.string();
    opts.host = "127.0.0.1";
    opts.port = kPort;
    _exit(ppc::RunCatalogd(opts));
  }

  // Parent: drive the client, then stop the server.
  std::printf("== pp-catalogd acceptance smoke ==\n  warehouse: %s\n", warehouse.c_str());
  int fails = RunClient(warehouse);

  kill(child, SIGTERM);
  int status = 0;
  waitpid(child, &status, 0);

  std::error_code ec;
  fs::remove_all(warehouse, ec);

  std::printf("\n== pp-catalogd-smoke %s ==\n", fails == 0 ? "PASS" : "FAIL");
  return fails == 0 ? 0 : 1;
}
