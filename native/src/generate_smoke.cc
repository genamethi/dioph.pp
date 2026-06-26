// generate_smoke — regression test for the generate -> pp-catalogd commit path.
//
// Forks the IRC server over a throwaway temp warehouse, then execs the *real*
// primeparts-generate binary with `--rest-uri` pointing at it, so its
// end-of-run commit routes partitions+primes FastAppends through the
// RestCatalog client -> updateTable (the same CommitFiles seam the in-process
// path uses). It then:
//   1. loads primes/partitions through the client and asserts the first run's
//      record counts (1000 primes, >0 partitions, 1 data file each), then
//   2. runs generate again on the next 1000 primes and asserts the catalog
//      *appended* (2 snapshots' worth: 2000 primes / 2 data files) rather than
//      recreating — the load-and-append resume contract.
// Returns 0 on success. This is the regression guard for the generate commit
// wiring; the server's commit-contract serde itself is covered by
// pp_catalogd_smoke.

#include <limits.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

#include <httplib.h>

#include "primeparts/catalog/pp_catalogd.h"
#include "primeparts/catalog/pp_iceberg_rest.h"

#include "iceberg/catalog.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/table.h"
#include "iceberg/table_identifier.h"
#include "iceberg/table_scan.h"

namespace fs = std::filesystem;
namespace ppc = primeparts::catalog;

namespace {

constexpr int kPort = 8232;
const char* kRestUri = "http://127.0.0.1:8232";

int failures = 0;
bool Check(bool cond, const std::string& msg) {
  std::printf("[%s] %s\n", cond ? "ok " : "FAIL", msg.c_str());
  if (!cond) ++failures;
  return cond;
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

// Plan the current snapshot and tally (data_files, records). Returns false if
// the table could not be loaded/scanned.
bool CountTable(const std::shared_ptr<iceberg::Catalog>& catalog,
                const std::string& name, int64_t* data_files,
                int64_t* records) {
  iceberg::TableIdentifier id{.ns = iceberg::Namespace{{"primeparts"}},
                              .name = name};
  auto loaded = catalog->LoadTable(id);
  if (!loaded.has_value()) return false;
  auto sb = loaded.value()->NewScan();
  if (!sb.has_value()) return false;
  auto scan = sb.value()->Build();
  if (!scan.has_value()) return false;
  auto tasks = scan.value()->PlanFiles();
  if (!tasks.has_value()) return false;
  *data_files = 0;
  *records = 0;
  for (const auto& t : tasks.value()) {
    ++*data_files;
    *records += t->data_file()->record_count;
  }
  return true;
}

// The real primeparts-generate binary lives next to this smoke in build/.
fs::path GenerateBinary() {
  char buf[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return "primeparts-generate";
  buf[n] = '\0';
  return fs::path(buf).parent_path() / "primeparts-generate";
}

// Fork+exec the real generate binary with --rest-uri so its end-of-run commit
// routes through the forked server. start_idx is the 1-based prime index.
bool RunGenerate(const fs::path& gen_bin, const fs::path& warehouse,
                 int64_t start_idx) {
  const std::string si = std::to_string(start_idx);
  fflush(nullptr);  // don't duplicate buffered stdout into the forked child
  pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    // Quiet the child's stdout JSON line; failures still surface via exit code.
    freopen("/dev/null", "w", stdout);
    execl(gen_bin.c_str(), gen_bin.c_str(), "--start-idx", si.c_str(),
          "--count", "1000", "--warehouse", warehouse.c_str(), "--rest-uri",
          kRestUri, "--threads", "1", static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int RunClient(const fs::path& warehouse) {
  if (!Check(WaitForServer(), "server answered GET /v1/config")) return failures;

  std::string mode, err;
  ppc::RestOptions ropts;
  ropts.rest_uri = kRestUri;
  auto catalog = ppc::MakeCatalog(ropts, warehouse, &mode, &err);
  if (!Check(catalog != nullptr, "RestCatalog client connected: " + err))
    return failures;

  const fs::path gen_bin = GenerateBinary();
  if (!Check(fs::exists(gen_bin), "found generate binary: " + gen_bin.string()))
    return failures;

  // First run: primes 1..1000 -> commit through the server.
  if (!Check(RunGenerate(gen_bin, warehouse, 1),
             "generate run #1 -> committed via REST"))
    return failures;

  int64_t p_files = 0, p_records = 0, d_files = 0, d_records = 0;
  Check(CountTable(catalog, "primes", &p_files, &p_records),
        "loaded primes through client");
  Check(p_records == 1000,
        "primes has 1000 records (got " + std::to_string(p_records) + ")");
  Check(p_files == 1,
        "primes has 1 data file (got " + std::to_string(p_files) + ")");
  Check(CountTable(catalog, "partitions", &d_files, &d_records),
        "loaded partitions through client");
  Check(d_records > 0,
        "partitions has records (got " + std::to_string(d_records) + ")");

  // Second run: primes 1001..2000 -> the catalog must *append* (load-and-append
  // resume), not recreate. Expect cumulative 2000 records / 2 data files.
  if (!Check(RunGenerate(gen_bin, warehouse, 1001),
             "generate run #2 -> appended via REST"))
    return failures;
  Check(CountTable(catalog, "primes", &p_files, &p_records),
        "reloaded primes after run #2");
  Check(p_records == 2000,
        "primes appended to 2000 records (got " + std::to_string(p_records) + ")");
  Check(p_files == 2,
        "primes has 2 data files (got " + std::to_string(p_files) + ")");
  return failures;
}

}  // namespace

int main() {
  char tmpl[] = "/tmp/ppgen-smoke-XXXXXX";
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

  std::printf("== generate -> pp-catalogd commit smoke ==\n  warehouse: %s\n",
              warehouse.c_str());
  int fails = RunClient(warehouse);

  kill(child, SIGTERM);
  int status = 0;
  waitpid(child, &status, 0);

  std::error_code ec;
  fs::remove_all(warehouse, ec);

  std::printf("\n== generate-commit-smoke %s ==\n", fails == 0 ? "PASS" : "FAIL");
  return fails == 0 ? 0 : 1;
}
