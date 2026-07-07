// primeparts/catalog/pp_iceberg_rest.cc — see header for rationale.

#include "primeparts/catalog/pp_iceberg_rest.h"

#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <utility>

#include <httplib.h>

#include "iceberg/arrow/arrow_io_util.h"
#include "iceberg/catalog/memory/in_memory_catalog.h"
#include "iceberg/catalog/rest/catalog_properties.h"
#include "iceberg/catalog/rest/rest_catalog.h"
#include "iceberg/catalog/sql/sql_catalog.h"
#include "iceberg/sort_order.h"
#include "iceberg/table.h"
#include "iceberg/update/fast_append.h"

#include "primeparts/catalog/pp_lmdb_store.h"
#include "primeparts/common/arrow_init.h"
#include "primeparts/common/uri.h"

namespace primeparts::catalog {

namespace {

// Move one staged file into its committed destination. Atomic rename on the
// common case (staging is on the same filesystem as the warehouse); falls back
// to copy+remove if the rename crosses a device boundary.
bool MoveStagedFile(const fs::path& src, const fs::path& dst, std::string* error) {
  std::error_code ec;
  fs::create_directories(dst.parent_path(), ec);
  fs::rename(src, dst, ec);
  if (!ec) return true;
  ec.clear();
  fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
  if (ec) {
    if (error)
      *error = "stage move " + src.string() + " -> " + dst.string() + ": " +
               ec.message();
    return false;
  }
  fs::remove(src, ec);  // best-effort; the catalog already has the copy
  return true;
}

}  // namespace

std::shared_ptr<iceberg::FileIO> LocalIO() {
  return std::shared_ptr<iceberg::FileIO>(iceberg::arrow::MakeLocalFileIO());
}

fs::path StagingDataDir(const fs::path& warehouse, const std::string& table_name) {
  const fs::path wh = warehouse.lexically_normal();
  const fs::path parent = wh.has_parent_path() ? wh.parent_path() : wh;
  std::string tag = wh.filename().string();
  if (tag.empty()) tag = "warehouse";
  return parent / ".pp-staging" / tag / table_name;
}

fs::path LatestMetadataJson(const fs::path& metadata_dir, std::string* error) {
  if (!fs::exists(metadata_dir)) {
    if (error) *error = "metadata dir not found: " + metadata_dir.string();
    return {};
  }
  fs::path best;
  std::string best_name;
  for (auto& entry : fs::directory_iterator(metadata_dir)) {
    auto name = entry.path().filename().string();
    if (name.size() < 6 || name.find(".metadata.json") == std::string::npos) {
      continue;
    }
    if (name > best_name) {
      best_name = name;
      best = entry.path();
    }
  }
  if (best.empty()) {
    if (error) *error = "no *.metadata.json under " + metadata_dir.string();
  }
  return best;
}

std::shared_ptr<iceberg::Catalog> MakeCatalog(const RestOptions& opts,
                                              const fs::path& warehouse,
                                              std::string* mode,
                                              std::string* error) {
  if (!opts.rest_uri.empty()) {
    common::EnsureArrowRegistration();
    auto config = iceberg::rest::RestCatalogProperties::default_properties();
    config.Set(iceberg::rest::RestCatalogProperties::kUri, opts.rest_uri)
        .Set(iceberg::rest::RestCatalogProperties::kName, opts.rest_name)
        .Set(iceberg::rest::RestCatalogProperties::kWarehouse,
             opts.rest_warehouse.empty() ? warehouse.string()
                                         : opts.rest_warehouse);
    if (!opts.rest_prefix.empty()) {
      config.Set(iceberg::rest::RestCatalogProperties::kPrefix,
                 opts.rest_prefix);
    }
    auto r = iceberg::rest::RestCatalog::Make(config);
    if (!r.has_value()) { *error = r.error().message; return nullptr; }
    // RestCatalog is a SessionCatalog root, not a Catalog; bind its default
    // session to get the standard Catalog view the rest of the project uses.
    auto cat = r.value()->AsCatalog();
    if (!cat.has_value()) { *error = cat.error().message; return nullptr; }
    *mode = "rest";
    return std::move(cat.value());
  }
  *mode = "in-memory";
  return std::make_shared<iceberg::InMemoryCatalog>(
      "primeparts-staging", LocalIO(), warehouse.string(),
      std::unordered_map<std::string, std::string>{});
}

std::shared_ptr<iceberg::Catalog> MakeLocalCatalog(const fs::path& warehouse,
                                                   std::string* error) {
  common::EnsureArrowRegistration();

  auto store_r = MakeLmdbCatalogStore(warehouse / "catalog.lmdb", "primeparts");
  if (!store_r.has_value()) {
    if (error) *error = "MakeLmdbCatalogStore: " + store_r.error().message;
    return nullptr;
  }
  iceberg::sql::SqlCatalogConfig cfg;
  cfg.name = "primeparts";
  cfg.warehouse_location = warehouse.string();
  auto cat_r =
      iceberg::sql::SqlCatalog::Make(cfg, LocalIO(), std::move(store_r.value()));
  if (!cat_r.has_value()) {
    if (error) *error = "SqlCatalog::Make: " + cat_r.error().message;
    return nullptr;
  }
  return std::move(cat_r.value());
}

bool RestServerReachable(const std::string& rest_uri) {
  if (rest_uri.empty()) return false;
  // httplib accepts a "scheme://host:port" base; the RestCatalog client appends
  // /v1/... so rest_uri must carry no context path (matches MakeCatalog's contract).
  httplib::Client cli(rest_uri);
  cli.set_connection_timeout(1, 0);  // 1s connect
  cli.set_read_timeout(2, 0);        // 2s read
  auto res = cli.Get("/v1/config");
  return res && res->status == 200;
}

std::shared_ptr<iceberg::Catalog> OpenCatalog(const fs::path& warehouse,
                                              const std::string& rest_uri,
                                              std::string* mode,
                                              std::string* error) {
  std::string uri = rest_uri;
  if (uri.empty()) {
    if (const char* env = std::getenv("PRIMEPARTS_REST_URI")) uri = env;
  }
  if (uri.empty()) uri = kDefaultRestUri;  // REST is the default channel; the
                                           // arg/env only OVERRIDE the endpoint.
                                           // LMDB is the unreachable-fallback below.
  std::string m;  // local mode sink so `mode` may be null
  if (!uri.empty()) {
    if (RestServerReachable(uri)) {
      RestOptions opts;
      opts.rest_uri = uri;
      std::string rest_err;
      auto cat = MakeCatalog(opts, warehouse, &m, &rest_err);  // sets m="rest"
      if (cat) {
        if (mode) *mode = m;
        return cat;
      }
      // A live server whose client init failed: report, then fall back to local.
      std::fprintf(stderr,
                   "pp: REST catalog %s init failed (%s); using local catalog\n",
                   uri.c_str(), rest_err.c_str());
    } else {
      std::fprintf(stderr,
                   "pp: REST catalog %s unreachable; using local catalog of record\n",
                   uri.c_str());
    }
  }
  auto local = MakeLocalCatalog(warehouse, error);
  if (local && mode) *mode = "local";
  return local;
}

fs::path TableMetadataPath(const std::shared_ptr<iceberg::Catalog>& catalog,
                           const std::string& table, std::string* error) {
  iceberg::TableIdentifier id{.ns = iceberg::Namespace{{"primeparts"}},
                              .name = table};
  auto t = catalog->LoadTable(id);
  if (!t.has_value()) {
    if (error) *error = "LoadTable(primeparts." + table + "): " + t.error().message;
    return {};
  }
  return fs::path(common::StripFileScheme(t.value()->metadata_file_location()));
}

bool EnsureNamespace(const std::shared_ptr<iceberg::Catalog>& catalog,
                     const iceberg::Namespace& ns, std::string* error) {
  auto exists = catalog->NamespaceExists(ns);
  if (!exists.has_value()) { *error = exists.error().message; return false; }
  if (exists.value()) return true;
  auto status = catalog->CreateNamespace(ns, {});
  if (!status.has_value()) { *error = status.error().message; return false; }
  return true;
}

bool DropTable(const std::shared_ptr<iceberg::Catalog>& catalog,
               const fs::path& warehouse, const std::string& table, bool purge,
               std::string* error) {
  iceberg::TableIdentifier ident{
      .ns = iceberg::Namespace{{"primeparts"}}, .name = table};

  // Resolve the table's base location through the catalog before dropping, so a
  // purge removes exactly the directory tree the catalog says this table owns.
  auto loaded = catalog->LoadTable(ident);
  if (!loaded.has_value()) {
    // Not registered. A clean slate for the catalog — but a build killed after a
    // prior DropTable (catalog row gone) but before CommitFiles leaves orphan
    // data files under the conventional location. Complete the purge contract by
    // reclaiming that directory too, so the next build starts clean. This is the
    // conventional layout CommitFiles writes (warehouse/primeparts/<table>).
    if (purge) {
      std::error_code ec;
      fs::remove_all(warehouse / "primeparts" / table, ec);
    }
    return true;
  }
  fs::path base(common::StripFileScheme(std::string(loaded.value()->location())));

  auto del = catalog->DropTable(ident, purge);
  if (!del.has_value()) {
    if (error) *error = "DropTable(primeparts." + table + "): " + del.error().message;
    return false;
  }

  // Complete the purgeRequested contract: the vendored SqlCatalog FileIO does
  // not delete physical files, so the catalog-adapter layer does it here. This
  // is the single sanctioned point of warehouse file removal.
  if (purge && !base.empty()) {
    std::error_code ec;
    fs::remove_all(base, ec);
  }
  return true;
}

bool PublishTable(const std::shared_ptr<iceberg::Catalog>& catalog,
                  const fs::path& warehouse, const std::string& table_name,
                  const std::shared_ptr<iceberg::Schema>& schema,
                  const std::shared_ptr<iceberg::PartitionSpec>& spec,
                  const std::vector<std::shared_ptr<iceberg::DataFile>>& files,
                  std::string* metadata_location, std::string* error) {
  fs::path md_dir = warehouse / "primeparts" / table_name / "metadata";
  iceberg::TableIdentifier ident{
      .ns = iceberg::Namespace{{"primeparts"}}, .name = table_name};

  std::string find_err;
  fs::path existing = LatestMetadataJson(md_dir, &find_err);
  if (!existing.empty()) {
    auto loaded = catalog->LoadTable(ident);
    if (loaded.has_value() &&
        std::string(loaded.value()->metadata_file_location()) ==
            existing.string()) {
      *metadata_location = std::string(loaded.value()->metadata_file_location());
      return true;
    }
    auto del = catalog->DropTable(ident, false);
    (void)del;  // NotFound is fine — fresh table path.
    auto reg = catalog->RegisterTable(ident, existing.string());
    if (!reg.has_value()) {
      *error = "RegisterTable " + table_name + ": " + reg.error().message;
      return false;
    }
    *metadata_location = std::string(reg.value()->metadata_file_location());
    return true;
  }

  // No on-disk metadata.json: fresh create + append. Shared with CommitFiles.
  return CommitFiles(catalog, warehouse, table_name, schema, spec, files,
                     metadata_location, error);
}

bool CommitFiles(const std::shared_ptr<iceberg::Catalog>& catalog,
                 const fs::path& warehouse, const std::string& table_name,
                 const std::shared_ptr<iceberg::Schema>& schema,
                 const std::shared_ptr<iceberg::PartitionSpec>& spec,
                 const std::vector<std::shared_ptr<iceberg::DataFile>>& files,
                 std::string* metadata_location, std::string* error) {
  iceberg::TableIdentifier ident{
      .ns = iceberg::Namespace{{"primeparts"}}, .name = table_name};
  if (!EnsureNamespace(catalog, ident.ns, error)) return false;

  // Load the table if the catalog already knows it (incremental append onto
  // its snapshot history); otherwise create it fresh. We probe with LoadTable
  // rather than disk state so this works identically over a RestCatalog client.
  std::shared_ptr<iceberg::Table> table;
  auto loaded = catalog->LoadTable(ident);
  if (loaded.has_value()) {
    table = std::move(loaded.value());
  } else {
    // FileIO does not mkdir parents — pre-create the metadata dir.
    std::error_code ec;
    fs::create_directories(warehouse / "primeparts" / table_name / "metadata",
                           ec);
    auto created = catalog->CreateTable(
        ident, schema, spec, iceberg::SortOrder::Unsorted(),
        (warehouse / "primeparts" / table_name).string(),
        {{"write.parquet.compression-codec", "zstd"},
         {"write.parquet.compression-level", "3"}});
    if (!created.has_value()) {
      *error = "CreateTable " + table_name + ": " + created.error().message;
      return false;
    }
    table = std::move(created.value());
  }

  if (!files.empty()) {
    // Catalog seam: the files were written to a staging dir outside the table
    // tree. Move each into the catalog-chosen destination (<table>/data/) and
    // rewrite its file_path BEFORE the append, so the manifest only ever records
    // committed in-warehouse paths and a killed run can't pollute the table dir.
    const fs::path data_dir =
        fs::path(common::StripFileScheme(std::string(table->location()))) / "data";
    // Preserve whatever sub-layout the writer used under the table's staging root
    // (e.g. p_bucket_version=N/p_bucket=M/...) by moving each file to its path
    // RELATIVE to the staging root, reproduced under <table>/data/. This keeps
    // CommitFiles ignorant of partition semantics: the client owns its own
    // partitioning (it chose the staging sub-path), the catalog owns the table
    // location. Files not under the staging root fall back to a flat basename.
    const fs::path staging_root = StagingDataDir(warehouse, table_name);
    for (const auto& f : files) {
      const fs::path src = common::StripFileScheme(f->file_path);
      fs::path rel = src.lexically_relative(staging_root);
      if (rel.empty() || rel.native().rfind("..", 0) == 0) rel = src.filename();
      const fs::path dst = data_dir / rel;
      if (src == dst) continue;  // already in place (idempotent)
      if (!MoveStagedFile(src, dst, error)) return false;
      f->file_path = dst.string();
    }

    auto app_r = table->NewFastAppend();
    if (!app_r.has_value()) {
      *error = "NewFastAppend " + table_name + ": " + app_r.error().message;
      return false;
    }
    auto app = std::move(app_r.value());
    for (const auto& f : files) app->AppendFile(f);
    auto cs = app->Commit();
    if (!cs.has_value()) {
      *error = "Commit " + table_name + ": " + cs.error().message;
      return false;
    }
    auto rs = table->Refresh();
    if (!rs.has_value()) {
      *error = "Refresh " + table_name + ": " + rs.error().message;
      return false;
    }
    // Best-effort: all committed files have been moved out, so drop this table's
    // staging tree (and the parent .pp-staging/<warehouse-name> if now empty).
    std::error_code ec;
    fs::remove_all(staging_root, ec);
    fs::remove(staging_root.parent_path(), ec);  // removes only if empty
  }
  *metadata_location = std::string(table->metadata_file_location());
  return true;
}

}  // namespace primeparts::catalog
