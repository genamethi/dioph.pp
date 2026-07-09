#include "primeparts/verify/verify.h"

#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/common/uri.h"

#include "iceberg/catalog.h"
#include "iceberg/snapshot.h"
#include "iceberg/table.h"
#include "iceberg/table_identifier.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <getopt.h>
#include <string>
#include <vector>

namespace ppc = primeparts::catalog;
namespace ppv = primeparts::verify;

namespace {

constexpr char kDefaultWarehouse[] =
    "/media/extssd/research/dioph.pp/data/ib-staging";

struct Options {
  std::string warehouse = kDefaultWarehouse;
  std::string rest_uri;
  std::string table = "both";
  ppv::Window window;
  int threads = 0;
  int max_examples = 20;
  std::string log_path;
};

void Usage(const char* argv0) {
  std::fprintf(stderr,
    "usage: %s [options]\n"
    "  Verify warehouse math through the catalog seam: pi(p)==prime_rank and p\n"
    "  prime (primes), p==2^m_k+q_k^n_k and q_k prime (partitions). Vectorized,\n"
    "  sharded over buckets, all cores.\n"
    "  --table T          primes | partitions | both (default both)\n"
    "  --p-lo N           only rows with p >= N (iceberg pushdown)\n"
    "  --p-hi N           only rows with p <= N (iceberg pushdown)\n"
    "  --limit N          stop after ~N rows total\n"
    "  --threads N        shard threads (default: hw concurrency)\n"
    "  --max-examples N   violating rows to record (default 20)\n"
    "  --warehouse DIR    warehouse root (default %s)\n"
    "  --rest-uri URI     IRC endpoint override (default %s)\n"
    "  --log PATH         run-log path (default verify-<time>.log)\n",
    argv0, kDefaultWarehouse, ppc::kDefaultRestUri);
}

struct Logger {
  std::ofstream file;
  void Line(const std::string& s) {
    std::printf("%s\n", s.c_str());
    if (file.is_open()) file << s << "\n";
  }
};

int64_t LatestSnapshotId(const std::shared_ptr<iceberg::Table>& tbl) {
  const auto& snaps = tbl->snapshots();
  if (snaps.empty()) return -1;
  return snaps.back()->snapshot_id;
}

int RunTable(const std::shared_ptr<iceberg::Catalog>& catalog,
             const std::string& table, const Options& opts, Logger& log) {
  iceberg::TableIdentifier ident{.ns = iceberg::Namespace{{"primeparts"}},
                                 .name = table};
  auto loaded = catalog->LoadTable(ident);
  if (!loaded.has_value()) {
    log.Line("FAIL " + table + ": LoadTable: " + loaded.error().message);
    return 1;
  }
  auto tbl = loaded.value();
  const std::string meta = primeparts::common::StripFileScheme(
      tbl->metadata_file_location());

  auto check = table == "primes" ? ppv::MakePrimeRankCheck()
                                 : ppv::MakePartitionFormCheck();
  auto filter = ppv::BuildWindowFilter(opts.window);

  std::string err;
  auto r = ppv::TableVerifier::Run(meta, *check, filter, opts.threads,
                                   opts.window.limit, opts.max_examples, &err);
  if (!err.empty()) {
    log.Line("FAIL " + table + ": " + err);
    return 1;
  }

  log.Line("table      : " + table);
  log.Line("  snapshot : " + std::to_string(LatestSnapshotId(tbl)) +
           " (" + std::to_string(tbl->snapshots().size()) + " total)");
  log.Line("  rows     : " + std::to_string(r.rows_checked));
  log.Line("  result   : " +
           std::string(r.ok() ? "PASS" : "FAIL") + " (" +
           std::to_string(r.violations) + " violations)");
  for (const auto& v : r.examples)
    log.Line("    p=" + std::to_string(v.p) + ": " + v.detail);
  return r.ok() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  Options opts;
  static struct option long_opts[] = {
      {"table", required_argument, nullptr, 't'},
      {"p-lo", required_argument, nullptr, 'L'},
      {"p-hi", required_argument, nullptr, 'H'},
      {"limit", required_argument, nullptr, 'n'},
      {"threads", required_argument, nullptr, 'j'},
      {"max-examples", required_argument, nullptr, 'e'},
      {"warehouse", required_argument, nullptr, 'w'},
      {"rest-uri", required_argument, nullptr, 'r'},
      {"log", required_argument, nullptr, 'o'},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0}};
  int o;
  while ((o = getopt_long(argc, argv, "t:L:H:n:j:e:w:r:o:h", long_opts,
                          nullptr)) != -1) {
    switch (o) {
      case 't': opts.table = optarg; break;
      case 'L': opts.window.p_lo = std::strtoll(optarg, nullptr, 10); break;
      case 'H': opts.window.p_hi = std::strtoll(optarg, nullptr, 10); break;
      case 'n': opts.window.limit = std::strtoll(optarg, nullptr, 10); break;
      case 'j': opts.threads = std::atoi(optarg); break;
      case 'e': opts.max_examples = std::atoi(optarg); break;
      case 'w': opts.warehouse = optarg; break;
      case 'r': opts.rest_uri = optarg; break;
      case 'o': opts.log_path = optarg; break;
      case 'h': Usage(argv[0]); return 0;
      default: Usage(argv[0]); return 2;
    }
  }
  if (opts.table != "primes" && opts.table != "partitions" &&
      opts.table != "both") {
    std::fprintf(stderr, "error: --table must be primes|partitions|both\n");
    return 2;
  }

  if (opts.log_path.empty())
    opts.log_path = "verify-" + std::to_string(std::time(nullptr)) + ".log";
  Logger log;
  log.file.open(opts.log_path);

  std::string mode, err;
  auto catalog = ppc::OpenCatalog(opts.warehouse, opts.rest_uri, &mode, &err);
  if (!catalog) {
    std::fprintf(stderr, "error: OpenCatalog: %s\n", err.c_str());
    return 1;
  }
  log.Line("== primeparts-verify ==");
  log.Line("warehouse  : " + opts.warehouse);
  log.Line("catalog    : " + mode);
  if (opts.window.p_lo > 0 || opts.window.p_hi > 0 || opts.window.limit > 0)
    log.Line("window     : p_lo=" + std::to_string(opts.window.p_lo) +
             " p_hi=" + std::to_string(opts.window.p_hi) +
             " limit=" + std::to_string(opts.window.limit));

  int rc = 0;
  if (opts.table == "both") {
    rc |= RunTable(catalog, "primes", opts, log);
    rc |= RunTable(catalog, "partitions", opts, log);
  } else {
    rc = RunTable(catalog, opts.table, opts, log);
  }
  log.Line(rc == 0 ? "OVERALL: PASS" : "OVERALL: FAIL");
  std::fprintf(stderr, "log written to %s\n", opts.log_path.c_str());
  return rc == 0 ? 0 : 1;
}
