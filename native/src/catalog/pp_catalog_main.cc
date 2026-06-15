// pp-catalog — primeparts catalog utility.
//
// Wraps the two halves of primeparts catalog management:
//   * IRC (iceberg REST) commit/register via primeparts/catalog/pp_iceberg_rest
//   * Hive engine sync via scripts/hive_register.sh (subprocess), wrapped in
//     primeparts/catalog/pp_hive_sync
//
// Primary purpose right now is `--smoke-test`: a self-contained environment
// check that exercises BOTH paths against the live cluster on a throwaway
// table, then cleans up. Run it after cluster config changes or image rebuilds
// to confirm the catalog plumbing still works end to end.
//
// Usage:
//   pp-catalog --smoke-test [--rest-uri URI] [--warehouse DIR] [--keep]
//   pp-catalog --hive-exec "SQL"
//   pp-catalog --hive-sync DB.TABLE METADATA_URI
//
// Defaults target the documented local deployment (IRC at :9090, HS2 via the
// beeline script). --keep leaves the throwaway table for inspection.

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include "primeparts/catalog/pp_delete_spike.h"
#include "primeparts/catalog/pp_hive_sync.h"
#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/catalog/pp_sieve_clone.h"

#include "iceberg/catalog.h"
#include "iceberg/table.h"
#include "iceberg/table_identifier.h"

namespace ppc = primeparts::catalog;
namespace fs = std::filesystem;

namespace {

constexpr char kDefaultRestUri[] = "http://192.168.1.202:9090/iceberg";
constexpr char kDefaultWarehouse[] =
    "/media/extssd/research/dioph.pp/data/ib-staging";

struct Args {
  bool smoke_test = false;
  bool delete_spike = false;
  bool mor_verify = false;
  bool clone_sieve = false;
  bool register_tables = false;
  bool keep = false;
  std::string rest_uri = kDefaultRestUri;
  std::string warehouse = kDefaultWarehouse;
  std::string hive_exec;
  bool do_hive_exec = false;
  std::string hive_sync_table;
  std::string hive_sync_uri;
  bool do_hive_sync = false;
};

void Usage() {
  std::fprintf(stderr,
    "pp-catalog — primeparts catalog utility\n\n"
    "  --smoke-test [--rest-uri URI] [--warehouse DIR] [--keep]\n"
    "      Exercise IRC create/register + Hive beeline sync on a throwaway\n"
    "      table, verify a 3->0->3 metadata_location swap, then drop it.\n"
    "  --delete-spike [--rest-uri URI] [--warehouse DIR] [--keep]\n"
    "      Prove native position-delete write->commit->reopen->read on a\n"
    "      throwaway table (5 rows, delete 2, expect 3).\n"
    "  --mor-verify [--rest-uri URI] [--warehouse DIR] [--keep]\n"
    "      De-risk the sieve: native [p,_pos] projection, native MOR read-back\n"
    "      (no beeline), and rapid back-to-back commit cadence.\n"
    "  --clone-sieve [--rest-uri URI] [--warehouse DIR]\n"
    "      Stand up primes_k0_sieve as a v2 MOR shallow clone of primes_k0\n"
    "      (native CreateTable + FastAppend of its data files; verifies v2).\n"
    "  --register [--warehouse DIR]\n"
    "      Cutover: register the on-disk base tables into the local LMDB\n"
    "      catalog (SqlCatalog(LmdbStore) at <warehouse>/catalog.lmdb). Idempotent.\n"
    "  --hive-exec \"SQL\"          run SQL via scripts/hive_register.sh\n"
    "  --hive-sync DB.TABLE URI    ALTER metadata_location via beeline\n");
}

bool ParseArgs(int argc, char** argv, Args* a) {
  for (int i = 1; i < argc; ++i) {
    std::string f = argv[i];
    auto next = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "pp-catalog: %s needs an argument\n", name);
        std::exit(2);
      }
      return argv[++i];
    };
    if (f == "--smoke-test") a->smoke_test = true;
    else if (f == "--delete-spike") a->delete_spike = true;
    else if (f == "--mor-verify") a->mor_verify = true;
    else if (f == "--clone-sieve") a->clone_sieve = true;
    else if (f == "--register") a->register_tables = true;
    else if (f == "--keep") a->keep = true;
    else if (f == "--rest-uri") a->rest_uri = next("--rest-uri");
    else if (f == "--warehouse") a->warehouse = next("--warehouse");
    else if (f == "--hive-exec") { a->hive_exec = next("--hive-exec"); a->do_hive_exec = true; }
    else if (f == "--hive-sync") {
      a->hive_sync_table = next("--hive-sync");
      a->hive_sync_uri = next("--hive-sync URI");
      a->do_hive_sync = true;
    } else if (f == "-h" || f == "--help") { Usage(); std::exit(0); }
    else { std::fprintf(stderr, "pp-catalog: unknown flag '%s'\n", f.c_str()); return false; }
  }
  return true;
}

int RunSmokeTest(const Args& a) {
  std::printf("== pp-catalog smoke test ==\n");
  std::printf("  rest-uri : %s\n", a.rest_uri.c_str());
  std::printf("  warehouse: %s\n", a.warehouse.c_str());

  // Unique throwaway name so concurrent/aborted runs don't collide.
  char name[64];
  std::snprintf(name, sizeof(name), "zz_ppcatalog_smoke_%ld",
                static_cast<long>(std::time(nullptr)));
  const std::string table = name;
  const std::string db_table = "primeparts." + table;

  ppc::HiveSyncOptions hopts;  // resolve script relative to this exe
  std::string out;
  bool ok = true;

  // --- Part 1: Hive connectivity (read-only) -------------------------------
  std::printf("\n[1/5] beeline connectivity (SELECT 1) ... ");
  if (ppc::HiveExec(hopts, "SELECT 1", &out)) {
    std::printf("OK\n");
  } else {
    std::printf("FAIL\n%s\n", out.c_str());
    return 1;  // nothing else will work
  }

  // --- Part 2: IRC reachability + create throwaway via beeline DDL ----------
  // We create the table through beeline (so it's a real Hive-managed iceberg
  // table with an on-disk metadata.json), then drive IRC + sync against it.
  std::printf("[2/5] create throwaway %s + insert 3 rows ... ", db_table.c_str());
  if (ppc::HiveExec(hopts,
        "DROP TABLE IF EXISTS " + db_table + "; "
        "CREATE TABLE " + db_table + " (id bigint) STORED BY ICEBERG STORED AS PARQUET; "
        "INSERT INTO " + db_table + " VALUES (1),(2),(3)", &out)) {
    std::printf("OK\n");
  } else {
    std::printf("FAIL\n%s\n", out.c_str());
    return 1;
  }

  // --- Part 3: IRC loadTable sees the table --------------------------------
  std::printf("[3/5] IRC RestCatalog loadTable ... ");
  {
    std::string mode, err;
    ppc::RestOptions ropts;
    ropts.rest_uri = a.rest_uri;
    auto cat = ppc::MakeCatalog(ropts, a.warehouse, &mode, &err);
    if (!cat) {
      std::printf("FAIL (MakeCatalog: %s)\n", err.c_str());
      ok = false;
    } else {
      iceberg::TableIdentifier id{
          .ns = iceberg::Namespace{{"primeparts"}}, .name = table};
      auto loaded = cat->LoadTable(id);
      if (loaded.has_value()) {
        std::printf("OK (mode=%s, metadata=%s)\n", mode.c_str(),
                    std::string(loaded.value()->metadata_file_location()).c_str());
      } else {
        std::printf("FAIL (LoadTable: %s)\n", loaded.error().message.c_str());
        ok = false;
      }
    }
  }

  // --- Part 4: Hive sync round-trip (3 -> 0 -> 3) --------------------------
  // Capture current metadata_location (M0, 3 rows) and the table's own initial
  // empty metadata (00000) by listing the metadata dir.
  std::printf("[4/5] beeline metadata_location swap 3->0->3 ...\n");
  {
    fs::path md_dir = fs::path(a.warehouse) / "primeparts.db" / table / "metadata";
    std::string lerr;
    fs::path m_cur = ppc::LatestMetadataJson(md_dir, &lerr);
    // The empty initial snapshot is the lexicographically-smallest 00000 file.
    fs::path m_empty;
    if (fs::exists(md_dir)) {
      const std::string suffix = ".metadata.json";
      std::string smallest;
      for (auto& e : fs::directory_iterator(md_dir)) {
        auto n = e.path().filename().string();
        // Real metadata files end in ".metadata.json"; skip Hadoop CRC sidecars
        // (".<name>.metadata.json.crc") and any other hidden files.
        if (n.empty() || n.front() == '.') continue;
        if (n.size() < suffix.size() ||
            n.compare(n.size() - suffix.size(), suffix.size(), suffix) != 0) {
          continue;
        }
        if (smallest.empty() || n < smallest) { smallest = n; m_empty = e.path(); }
      }
    }
    if (m_cur.empty() || m_empty.empty()) {
      std::printf("      FAIL (could not locate metadata files in %s: %s)\n",
                  md_dir.c_str(), lerr.c_str());
      ok = false;
    } else {
      auto uri = [](const fs::path& p){ return "file:" + p.string(); };
      auto count = [&](const char* label) {
        std::string o;
        ppc::HiveExec(hopts, "SELECT COUNT(*) FROM " + db_table, &o);
        std::printf("      %s: %s", label, o.c_str());
      };
      count("baseline");
      std::printf("      sync -> empty (%s)\n", m_empty.filename().c_str());
      if (!ppc::HiveSync(hopts, db_table, uri(m_empty), &out)) {
        std::printf("      FAIL sync->empty\n%s\n", out.c_str()); ok = false;
      }
      count("after-empty");
      std::printf("      sync -> restore (%s)\n", m_cur.filename().c_str());
      if (!ppc::HiveSync(hopts, db_table, uri(m_cur), &out)) {
        std::printf("      FAIL sync->restore\n%s\n", out.c_str()); ok = false;
      }
      count("after-restore");
      std::printf("      (verify: baseline & after-restore should be 3, after-empty 0)\n");
    }
  }

  // --- Part 5: cleanup ------------------------------------------------------
  if (a.keep) {
    std::printf("[5/5] --keep set; leaving %s in place\n", db_table.c_str());
  } else {
    std::printf("[5/5] drop throwaway %s ... ", db_table.c_str());
    if (ppc::HiveExec(hopts, "DROP TABLE IF EXISTS " + db_table, &out)) {
      std::printf("OK\n");
    } else {
      std::printf("FAIL\n%s\n", out.c_str());
      ok = false;
    }
  }

  std::printf("\n== smoke test %s ==\n", ok ? "PASSED" : "FAILED");
  return ok ? 0 : 1;
}

// Cutover: register the existing on-disk base tables into the local LMDB
// catalog. Idempotent (RegisterTable -> kAlreadyExists on re-run). All tables
// land in one logical "primeparts" namespace; the Hive-era primeparts.db/
// physical split is collapsed since metadata_location is just a pointer.
int RunRegister(const Args& a) {
  const fs::path wh = a.warehouse;
  std::string err;
  auto cat = ppc::MakeLocalCatalog(wh, &err);
  if (!cat) {
    std::printf("FAIL (MakeLocalCatalog: %s)\n", err.c_str());
    return 1;
  }
  const iceberg::Namespace ns{{"primeparts"}};
  if (!ppc::EnsureNamespace(cat, ns, &err)) {
    std::printf("FAIL (EnsureNamespace: %s)\n", err.c_str());
    return 1;
  }

  struct Reg { const char* name; const char* rel_dir; };
  const std::vector<Reg> tables = {
      {"primes", "primeparts/primes/metadata"},
      {"partitions", "primeparts/partitions/metadata"},
      {"boundaries", "primeparts/boundaries/metadata"},
      {"primes_k0", "primeparts.db/primes_k0/metadata"},
      {"primes_k0_sieve", "primeparts.db/primes_k0_sieve/metadata"},
      {"q_k_freq_lo", "primeparts.db/q_k_freq_lo/metadata"},
      {"q_k_freq_mid", "primeparts.db/q_k_freq_mid/metadata"},
      {"q_k_histogram", "primeparts.db/q_k_histogram/metadata"},
  };

  int registered = 0, skipped = 0, failed = 0;
  for (const auto& t : tables) {
    std::string ferr;
    fs::path latest = ppc::LatestMetadataJson(wh / t.rel_dir, &ferr);
    if (latest.empty()) {
      std::printf("[skip] %-16s (%s)\n", t.name, ferr.c_str());
      ++skipped;
      continue;
    }
    iceberg::TableIdentifier id{.ns = ns, .name = t.name};
    auto r = cat->RegisterTable(id, latest.string());
    if (r.has_value()) {
      std::printf("[ok]   %-16s -> %s\n", t.name,
                  latest.filename().string().c_str());
      ++registered;
    } else if (r.error().kind == iceberg::ErrorKind::kAlreadyExists) {
      std::printf("[have] %-16s (already registered)\n", t.name);
      ++skipped;
    } else {
      std::printf("[FAIL] %-16s : %s\n", t.name, r.error().message.c_str());
      ++failed;
    }
  }

  // Verify the seam end-to-end: LoadTable resolves through LMDB.
  auto loaded = cat->LoadTable(iceberg::TableIdentifier{.ns = ns, .name = "primes"});
  if (loaded.has_value()) {
    std::printf("[verify] LoadTable(primes) OK -> %s\n",
                std::string(loaded.value()->metadata_file_location()).c_str());
  } else {
    std::printf("[verify] LoadTable(primes) FAILED: %s\n",
                loaded.error().message.c_str());
    ++failed;
  }

  std::printf("\n== register: %d registered, %d skipped, %d failed -> %s ==\n",
              registered, skipped, failed,
              (wh / "catalog.lmdb").string().c_str());
  return failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!ParseArgs(argc, argv, &a)) { Usage(); return 2; }

  if (a.do_hive_exec) {
    ppc::HiveSyncOptions hopts;
    std::string out;
    bool ok = ppc::HiveExec(hopts, a.hive_exec, &out);
    std::fputs(out.c_str(), stdout);
    return ok ? 0 : 1;
  }
  if (a.do_hive_sync) {
    ppc::HiveSyncOptions hopts;
    std::string out;
    bool ok = ppc::HiveSync(hopts, a.hive_sync_table, a.hive_sync_uri, &out);
    std::fputs(out.c_str(), stdout);
    return ok ? 0 : 1;
  }
  if (a.smoke_test) return RunSmokeTest(a);
  if (a.delete_spike) {
    ppc::DeleteSpikeOptions d;
    d.rest_uri = a.rest_uri;
    d.warehouse = a.warehouse;
    d.keep = a.keep;
    return ppc::RunDeleteSpike(d);
  }
  if (a.mor_verify) {
    ppc::DeleteSpikeOptions d;
    d.rest_uri = a.rest_uri;
    d.warehouse = a.warehouse;
    d.keep = a.keep;
    return ppc::RunMorVerify(d);
  }
  if (a.clone_sieve) {
    ppc::CloneSieveOptions c;
    c.rest_uri = a.rest_uri;
    c.warehouse = a.warehouse;
    return ppc::RunCloneSieve(c);
  }
  if (a.register_tables) return RunRegister(a);

  Usage();
  return 2;
}
