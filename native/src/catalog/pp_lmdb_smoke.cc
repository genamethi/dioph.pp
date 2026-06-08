// primeparts/catalog/pp_lmdb_smoke.cc
//
// Round-trip proof for the LMDB catalog backend. Two parts:
//
//   Part A - store contract: drives the iceberg::sql::CatalogStore interface
//            directly against the LMDB store. No FileIO, no parquet; this is the
//            authoritative test of the persistence semantics SqlCatalog relies
//            on (unique-violation -> AlreadyExists, the optimistic CAS, the
//            namespace union, transactional commit/rollback).
//
//   Part B - catalog integration: builds an in-process iceberg::sql::SqlCatalog
//            on top of the LMDB store with a local FileIO and runs the full
//            CreateNamespace -> CreateTable -> LoadTable -> RenameTable ->
//            DropTable -> DropNamespace path, proving the engine commits its
//            metadata.json pointers through LMDB end to end.
//
// Exit code 0 iff every check passes.

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "iceberg/arrow/arrow_file_io.h"
#include "iceberg/catalog/sql/catalog_store.h"
#include "iceberg/catalog/sql/sql_catalog.h"
#include "iceberg/file_io.h"
#include "iceberg/partition_spec.h"
#include "iceberg/result.h"
#include "iceberg/schema.h"
#include "iceberg/schema_field.h"
#include "iceberg/sort_order.h"
#include "iceberg/table.h"
#include "iceberg/table_identifier.h"
#include "iceberg/type.h"

#include "primeparts/catalog/pp_lmdb_store.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_failures;
}

// Convenience: report a Result/Status that was expected to succeed.
template <typename T>
bool Ok(const iceberg::Result<T>& r, const char* what) {
  bool ok = r.has_value();
  if (!ok) std::printf("  [FAIL] %s: %s\n", what, r.error().message.c_str());
  else std::printf("  [ok  ] %s\n", what);
  if (!ok) ++g_failures;
  return ok;
}

bool OkStatus(const iceberg::Status& s, const char* what) {
  bool ok = s.has_value();
  if (!ok) std::printf("  [FAIL] %s: %s\n", what, s.error().message.c_str());
  else std::printf("  [ok  ] %s\n", what);
  if (!ok) ++g_failures;
  return ok;
}

template <typename T>
bool IsAlreadyExists(const std::expected<T, iceberg::Error>& r) {
  return !r.has_value() && r.error().kind == iceberg::ErrorKind::kAlreadyExists;
}

// --------------------------------------------------------------------------
// Part A - CatalogStore contract
// --------------------------------------------------------------------------
void PartA(const std::filesystem::path& dir) {
  std::printf("\n== Part A: CatalogStore contract (LMDB direct) ==\n");
  auto store_r = primeparts::catalog::MakeLmdbCatalogStore(dir, "primeparts");
  if (!Ok(store_r, "MakeLmdbCatalogStore")) return;
  auto store = store_r.value();
  if (!OkStatus(store->Initialize(), "Initialize")) return;

  // Namespace properties + unique violation.
  OkStatus(store->InsertNamespaceProperty("a.b", "exists", "true"),
           "insert nsprop a.b/exists");
  Check(IsAlreadyExists(store->InsertNamespaceProperty("a.b", "exists", "true")),
        "duplicate nsprop -> AlreadyExists");
  OkStatus(store->InsertNamespaceProperty("a.b", "owner", "ann"),
           "insert nsprop a.b/owner");

  auto props = store->GetNamespaceProperties("a.b");
  Ok(props, "get nsprops a.b");
  Check(props.has_value() && props->size() == 2, "a.b has 2 property rows");

  // Tables + unique violation. c.d exists only via a table row.
  OkStatus(store->InsertTable("a.b", "t1", "/wh/a.b/t1/v1.json"), "insert a.b.t1");
  Check(IsAlreadyExists(store->InsertTable("a.b", "t1", "/wh/x")),
        "duplicate table -> AlreadyExists");
  OkStatus(store->InsertTable("c.d", "t9", "/wh/c.d/t9/v1.json"), "insert c.d.t9");

  // ListNamespaceNames unions nsprops namespaces with table namespaces.
  auto names = store->ListNamespaceNames();
  Ok(names, "list namespace names");
  bool has_ab = false, has_cd = false;
  if (names.has_value())
    for (const auto& n : *names) {
      has_ab |= (n == "a.b");
      has_cd |= (n == "c.d");
    }
  Check(has_ab && has_cd, "namespaces unioned: {a.b (props), c.d (table-only)}");

  auto tnames = store->ListTableNames("a.b");
  Check(tnames.has_value() && tnames->size() == 1 && tnames->front() == "t1",
        "list tables a.b == {t1}");

  auto exists = store->TableExists("a.b", "t1");
  Check(exists.has_value() && *exists, "TableExists a.b.t1 == true");
  auto nexists = store->TableExists("a.b", "nope");
  Check(nexists.has_value() && !*nexists, "TableExists a.b.nope == false");

  auto loc = store->GetTableMetadataLocation("a.b", "t1");
  Check(loc.has_value() && *loc == "/wh/a.b/t1/v1.json", "metadata loc == v1");

  // Optimistic CAS: matching base advances and returns 1.
  auto cas_ok = store->UpdateTableMetadataLocation(
      "a.b", "t1", "/wh/a.b/t1/v2.json", "/wh/a.b/t1/v1.json", "/wh/a.b/t1/v1.json");
  Check(cas_ok.has_value() && *cas_ok == 1, "CAS with fresh base -> 1 row");
  auto loc2 = store->GetTableMetadataLocation("a.b", "t1");
  Check(loc2.has_value() && *loc2 == "/wh/a.b/t1/v2.json", "metadata loc advanced to v2");

  // Stale base must not apply and returns 0.
  auto cas_stale = store->UpdateTableMetadataLocation(
      "a.b", "t1", "/wh/a.b/t1/v3.json", "/wh/a.b/t1/v2.json", "/wh/a.b/t1/v1.json");
  Check(cas_stale.has_value() && *cas_stale == 0, "CAS with stale base -> 0 rows");
  auto loc3 = store->GetTableMetadataLocation("a.b", "t1");
  Check(loc3.has_value() && *loc3 == "/wh/a.b/t1/v2.json", "metadata loc unchanged (still v2)");

  // Rename moves the row (and its value) and frees the old key.
  auto ren = store->RenameTable("a.b", "t1", "a.b", "t2");
  Check(ren.has_value() && *ren == 1, "rename t1 -> t2 -> 1 row");
  auto loc4 = store->GetTableMetadataLocation("a.b", "t2");
  Check(loc4.has_value() && *loc4 == "/wh/a.b/t1/v2.json", "t2 carries v2 metadata loc");
  auto gone = store->TableExists("a.b", "t1");
  Check(gone.has_value() && !*gone, "old name t1 gone after rename");

  // Rename onto an existing target is a unique violation.
  OkStatus(store->InsertTable("a.b", "keep", "/wh/a.b/keep/v1.json"), "insert a.b.keep");
  Check(IsAlreadyExists(store->RenameTable("a.b", "t2", "a.b", "keep")),
        "rename onto existing -> AlreadyExists");

  auto del1 = store->DeleteTable("a.b", "t2");
  Check(del1.has_value() && *del1 == 1, "delete t2 -> 1 row");
  auto del0 = store->DeleteTable("a.b", "t2");
  Check(del0.has_value() && *del0 == 0, "delete t2 again -> 0 rows");

  // Transaction: commit makes both inserts durable.
  auto tx_ok = store->RunInTransaction([&]() -> iceberg::Status {
    if (auto s = store->InsertTable("tx.ns", "one", "/wh/one"); !s.has_value()) return s;
    if (auto s = store->InsertTable("tx.ns", "two", "/wh/two"); !s.has_value()) return s;
    return {};
  });
  OkStatus(tx_ok, "RunInTransaction(commit)");
  auto one = store->TableExists("tx.ns", "one");
  auto two = store->TableExists("tx.ns", "two");
  Check(one.has_value() && *one && two.has_value() && *two,
        "committed tx: both tx.ns.one and tx.ns.two present");

  // Transaction: a failing body rolls everything back.
  auto tx_rb = store->RunInTransaction([&]() -> iceberg::Status {
    if (auto s = store->InsertTable("rb.ns", "ghost", "/wh/ghost"); !s.has_value()) return s;
    return iceberg::InvalidArgument("intentional abort");
  });
  Check(!tx_rb.has_value(), "RunInTransaction(rollback) returns the body error");
  auto ghost = store->TableExists("rb.ns", "ghost");
  Check(ghost.has_value() && !*ghost, "rolled-back tx left no rb.ns.ghost row");

  // DeleteNamespace removes all property rows and reports the count.
  auto dn = store->DeleteNamespace("a.b");
  Check(dn.has_value() && *dn == 2, "DeleteNamespace a.b -> 2 property rows removed");
  auto names2 = store->ListNamespaceNames();
  bool still_ab_props = false;
  // a.b still appears if it owns a table (a.b.keep) -> union keeps it visible.
  bool still_ab = false;
  if (names2.has_value())
    for (const auto& n : *names2) still_ab |= (n == "a.b");
  (void)still_ab_props;
  Check(still_ab, "a.b still listed via its remaining table (a.b.keep)");
}

// --------------------------------------------------------------------------
// Part B - SqlCatalog integration
// --------------------------------------------------------------------------
void PartB(const std::filesystem::path& dir, const std::filesystem::path& warehouse) {
  std::printf("\n== Part B: SqlCatalog integration (engine over LMDB) ==\n");
  std::filesystem::create_directories(warehouse);

  auto io = std::shared_ptr<iceberg::FileIO>(iceberg::arrow::MakeLocalFileIO());
  auto store_r = primeparts::catalog::MakeLmdbCatalogStore(dir, "primeparts");
  if (!Ok(store_r, "MakeLmdbCatalogStore(B)")) return;

  iceberg::sql::SqlCatalogConfig cfg;
  cfg.name = "primeparts";
  cfg.warehouse_location = warehouse.string();

  auto cat_r = iceberg::sql::SqlCatalog::Make(cfg, io, store_r.value());
  if (!Ok(cat_r, "SqlCatalog::Make")) return;
  auto cat = cat_r.value();

  const iceberg::Namespace ns{{"primeparts"}};
  OkStatus(cat->CreateNamespace(ns, {}), "CreateNamespace primeparts");
  auto nsx = cat->NamespaceExists(ns);
  Check(nsx.has_value() && *nsx, "NamespaceExists primeparts == true");

  auto schema = std::make_shared<iceberg::Schema>(std::vector<iceberg::SchemaField>{
      iceberg::SchemaField::MakeRequired(1, "p", iceberg::int64()),
      iceberg::SchemaField::MakeRequired(2, "k", iceberg::int32()),
  });

  const iceberg::TableIdentifier id{.ns = ns, .name = "demo"};
  const std::string loc = (warehouse / "primeparts" / "demo").string();
  // The arrow local FileIO does not create parent directories; the engine
  // writes metadata.json under <loc>/metadata/, so make that path first.
  std::filesystem::create_directories(std::filesystem::path(loc) / "metadata");

  auto created =
      cat->CreateTable(id, schema, iceberg::PartitionSpec::Unpartitioned(),
                       iceberg::SortOrder::Unsorted(), loc, {});
  Ok(created, "CreateTable primeparts.demo");

  auto loaded = cat->LoadTable(id);
  Ok(loaded, "LoadTable primeparts.demo");

  const iceberg::TableIdentifier id2{.ns = ns, .name = "demo2"};
  OkStatus(cat->RenameTable(id, id2), "RenameTable demo -> demo2");
  auto load2 = cat->LoadTable(id2);
  Ok(load2, "LoadTable primeparts.demo2 after rename");
  auto load_old = cat->LoadTable(id);
  Check(!load_old.has_value() &&
            load_old.error().kind == iceberg::ErrorKind::kNoSuchTable,
        "old name demo -> NoSuchTable");

  OkStatus(cat->DropTable(id2, /*purge=*/false), "DropTable primeparts.demo2");
  auto tx = cat->TableExists(id2);
  Check(tx.has_value() && !*tx, "TableExists demo2 == false after drop");

  OkStatus(cat->DropNamespace(ns), "DropNamespace primeparts");
}

}  // namespace

int main() {
  std::error_code ec;
  const auto root =
      std::filesystem::temp_directory_path() / "pp_lmdb_smoke" /
      std::to_string(static_cast<long long>(::getpid()));
  std::filesystem::remove_all(root, ec);

  PartA(root / "store");
  PartB(root / "catalog", root / "warehouse");

  std::filesystem::remove_all(root, ec);

  std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures,
              g_failures == 1 ? "" : "s");
  return g_failures == 0 ? 0 : 1;
}
