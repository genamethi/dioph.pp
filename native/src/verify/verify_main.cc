#include "primeparts/verify/verify.h"

#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/config.h"
#include "primeparts/common/uri.h"
#include "primeparts/scan/table_traits.h"

#include "iceberg/catalog.h"
#include "iceberg/snapshot.h"
#include "iceberg/table.h"
#include "iceberg/table_identifier.h"
#include "iceberg/table_metadata.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <getopt.h>
#include <optional>
#include <string>
#include <vector>

namespace ppc = primeparts::catalog;
namespace ppv = primeparts::verify;

namespace {

struct Options {
  std::string config_path;
  std::string warehouse;
  std::string rest_uri;
  std::string ns_name;
  std::string table = "both";
  ppv::Window window;
  int threads = -1;
  int max_examples = -1;
  int tail = 0;
  std::string log_path;
};

void Usage(const char* argv0) {
  std::fprintf(stderr,
    "usage: %s [options]\n"
    "  Verify warehouse math through the catalog seam: pi(p)==prime_rank and p\n"
    "  prime (primes), p==2^m+q and q prime for every set bit of hit_mask\n"
    "  (flat_parts), p==2^m_k+q_k^n_k with n_k>=2 and q_k prime (higher_parts).\n"
    "  Vectorized, sharded over buckets, all cores.\n"
    "  --table T          primes | flat_parts | higher_parts | both\n"
    "                     (default both = every check)\n"
    "  --p-lo N           only rows with p >= N (iceberg pushdown)\n"
    "  --p-hi N           only rows with p <= N (iceberg pushdown)\n"
    "  --limit N          stop after ~N rows total\n"
    "  --tail N           only rows added in the last N snapshots (incremental scan)\n"
    "  --threads N        shard threads (default: conf.verify.threads, 0 = hw)\n"
    "  --max-examples N   violating rows to record\n"
    "                     (default: conf.verify.max_examples)\n"
    "  -c, --config PATH  config file (default: ./config.lua, then XDG, then\n"
    "                     ~/.config/primeparts/config.lua, else seeded next to\n"
    "                     this binary)\n"
    "  --warehouse DIR    warehouse root (default: conf.core.warehouse)\n"
    "  --rest-uri URI     IRC endpoint (default: conf.core.rest_uri)\n"
    "  --namespace NS     catalog namespace (default: conf.core.namespace)\n"
    "  --log PATH         run-log path (default verify-<time>.log)\n",
    argv0);
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
             const iceberg::Namespace& ns, const ppv::Check& check,
             const Options& opts, Logger& log) {
  const std::string& table = check.spec().table;
  iceberg::TableIdentifier ident{.ns = ns, .name = table};
  auto loaded = catalog->LoadTable(ident);
  if (!loaded.has_value()) {
    log.Line("FAIL " + table + ": LoadTable: " + loaded.error().message);
    return 1;
  }
  auto tbl = loaded.value();
  const std::string meta = primeparts::common::StripFileScheme(
      tbl->metadata_file_location());

  if (!check.spec().requires_ascending.empty()) {
    primeparts::scan::TableReadTraits traits;
    std::string terr;
    const auto& metadata = tbl->metadata();
    if (!metadata ||
        !primeparts::scan::TableReadTraits::FromMetadata(*metadata, &traits,
                                                         &terr)) {
      log.Line("FAIL " + table + ": traits: " + terr);
      return 1;
    }
    const auto& req = check.spec().requires_ascending;
    const bool declared = traits.sorted() &&
                          traits.sort_keys.front().ascending &&
                          traits.sort_keys.front().name == req;
    if (!declared) {
      log.Line("FAIL " + table +
               ": check requires an ascending sort order on '" + req +
               "' declared in the catalog");
      return 1;
    }
  }

  auto filter = ppv::BuildWindowFilter(opts.window);

  std::optional<int64_t> from_snap;
  if (opts.tail > 0) {
    auto snaps = tbl->snapshots();
    std::sort(snaps.begin(), snaps.end(),
              [](const std::shared_ptr<iceberg::Snapshot>& a,
                 const std::shared_ptr<iceberg::Snapshot>& b) {
                return a->sequence_number < b->sequence_number;
              });
    const int idx = static_cast<int>(snaps.size()) - 1 - opts.tail;
    if (idx >= 0) from_snap = snaps[idx]->snapshot_id;
  }

  std::string err;
  auto r = ppv::TableVerifier::Run(meta, check, filter, opts.threads,
                                   opts.window.limit, opts.max_examples,
                                   from_snap, &err);
  if (!err.empty()) {
    log.Line("FAIL " + table + ": " + err);
    return 1;
  }

  log.Line("table      : " + table);
  log.Line("  snapshot : " + std::to_string(LatestSnapshotId(tbl)) +
           " (" + std::to_string(tbl->snapshots().size()) + " total)");
  if (from_snap)
    log.Line("  tail     : last " + std::to_string(opts.tail) +
             " snapshots (from_excl=" + std::to_string(*from_snap) + ")");
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
      {"tail", required_argument, nullptr, 'T'},
      {"threads", required_argument, nullptr, 'j'},
      {"max-examples", required_argument, nullptr, 'e'},
      {"warehouse", required_argument, nullptr, 'w'},
      {"rest-uri", required_argument, nullptr, 'r'},
      {"namespace", required_argument, nullptr, 'N'},
      {"log", required_argument, nullptr, 'o'},
      {"config", required_argument, nullptr, 1000},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0}};
  int o;
  while ((o = getopt_long(argc, argv, "t:L:H:n:T:j:e:w:r:N:o:c:h", long_opts,
                          nullptr)) != -1) {
    switch (o) {
      case 't': opts.table = optarg; break;
      case 'L': opts.window.p_lo = std::strtoll(optarg, nullptr, 10); break;
      case 'H': opts.window.p_hi = std::strtoll(optarg, nullptr, 10); break;
      case 'n': opts.window.limit = std::strtoll(optarg, nullptr, 10); break;
      case 'T': opts.tail = std::atoi(optarg); break;
      case 'j': opts.threads = std::atoi(optarg); break;
      case 'e': opts.max_examples = std::atoi(optarg); break;
      case 'w': opts.warehouse = optarg; break;
      case 'r': opts.rest_uri = optarg; break;
      case 'N': opts.ns_name = optarg; break;
      case 'o': opts.log_path = optarg; break;
      case 'c': case 1000: opts.config_path = optarg; break;
      case 'h': Usage(argv[0]); return 0;
      default: Usage(argv[0]); return 2;
    }
  }

  auto checks = ppv::AllChecks();
  if (opts.table != "both") {
    bool known = false;
    for (const auto& c : checks)
      if (c->spec().table == opts.table) known = true;
    if (!known) {
      std::string tables;
      for (const auto& c : checks) {
        if (!tables.empty()) tables += "|";
        tables += c->spec().table;
      }
      std::fprintf(stderr, "error: --table must be %s|both\n", tables.c_str());
      return 2;
    }
  }

  std::string cfg_err;
  primeparts::config::Conf conf;
  if (!primeparts::config::Load(opts.config_path, &conf, &cfg_err)) {
    std::fprintf(stderr, "error: %s\n", cfg_err.c_str());
    return 2;
  }
  primeparts::config::Announce(conf);
  if (opts.warehouse.empty()) opts.warehouse = conf.core.warehouse;
  if (opts.rest_uri.empty()) opts.rest_uri = conf.core.rest_uri;
  if (opts.ns_name.empty()) opts.ns_name = conf.core.ns_name;
  if (opts.threads < 0) opts.threads = static_cast<int>(conf.verify.threads);
  if (opts.max_examples < 0)
    opts.max_examples = static_cast<int>(conf.verify.max_examples);

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
  const iceberg::Namespace ns = ppc::ResolveNamespace(opts.ns_name);
  log.Line("== primeparts-verify ==");
  log.Line("warehouse  : " + opts.warehouse);
  log.Line("catalog    : " + mode);
  log.Line("namespace  : " + ns.ToString());
  if (opts.window.p_lo > 0 || opts.window.p_hi > 0 || opts.window.limit > 0)
    log.Line("window     : p_lo=" + std::to_string(opts.window.p_lo) +
             " p_hi=" + std::to_string(opts.window.p_hi) +
             " limit=" + std::to_string(opts.window.limit));

  int rc = 0;
  for (const auto& check : checks) {
    if (opts.table != "both" && check->spec().table != opts.table) continue;
    rc |= RunTable(catalog, ns, *check, opts, log);
  }
  log.Line(rc == 0 ? "OVERALL: PASS" : "OVERALL: FAIL");
  std::fprintf(stderr, "log written to %s\n", opts.log_path.c_str());
  return rc == 0 ? 0 : 1;
}
