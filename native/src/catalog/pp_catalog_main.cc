// pp-catalog — primeparts local catalog utility.
//
// Thin CLI over the local catalog of record: an in-process
// `iceberg::sql::SqlCatalog` backed by an LMDB `CatalogStore`
// (`MakeLocalCatalog`, primeparts/catalog/pp_iceberg_rest). Subcommands operate
// entirely against the on-disk warehouse + `<warehouse>/catalog.lmdb` — no
// network catalog, no JVM.
//
// Usage:
//   pp-catalog --register     [--warehouse DIR]
//   pp-catalog --clone-sieve  [--warehouse DIR]

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/catalog/pp_sieve_clone.h"

#include "iceberg/catalog.h"
#include "iceberg/table.h"
#include "iceberg/table_identifier.h"

namespace ppc = primeparts::catalog;
namespace fs = std::filesystem;

namespace {

constexpr char kDefaultWarehouse[] =
    "/media/extssd/research/dioph.pp/data/ib-staging";

struct Args {
  bool clone_sieve = false;
  bool register_tables = false;
  std::string warehouse = kDefaultWarehouse;
};

void Usage() {
  std::fprintf(stderr,
    "pp-catalog — primeparts local catalog utility\n\n"
    "  --register [--warehouse DIR]\n"
    "      Register the on-disk base tables into the local LMDB catalog\n"
    "      (SqlCatalog(LmdbStore) at <warehouse>/catalog.lmdb). Idempotent.\n"
    "  --clone-sieve [--warehouse DIR]\n"
    "      Stand up primes_k0_sieve as a v2 MOR shallow clone of primes_k0\n"
    "      (native CreateTable + FastAppend of its data files; verifies v2).\n");
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
    if (f == "--clone-sieve") a->clone_sieve = true;
    else if (f == "--register") a->register_tables = true;
    else if (f == "--warehouse") a->warehouse = next("--warehouse");
    else if (f == "-h" || f == "--help") { Usage(); std::exit(0); }
    else { std::fprintf(stderr, "pp-catalog: unknown flag '%s'\n", f.c_str()); return false; }
  }
  return true;
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

  if (a.clone_sieve) {
    ppc::CloneSieveOptions c;
    c.warehouse = a.warehouse;
    return ppc::RunCloneSieve(c);
  }
  if (a.register_tables) return RunRegister(a);

  Usage();
  return 2;
}
