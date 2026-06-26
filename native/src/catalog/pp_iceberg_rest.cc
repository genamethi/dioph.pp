// primeparts/catalog/pp_iceberg_rest.cc — see header for rationale.

#include "primeparts/catalog/pp_iceberg_rest.h"

#include <unordered_map>
#include <utility>

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

std::shared_ptr<iceberg::FileIO> LocalIO() {
  return std::shared_ptr<iceberg::FileIO>(iceberg::arrow::MakeLocalFileIO());
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
    *mode = "rest";
    return std::move(r.value());
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

  std::error_code ec;
  fs::create_directories(md_dir, ec);
  auto created = catalog->CreateTable(
      ident, schema, spec, iceberg::SortOrder::Unsorted(),
      (warehouse / "primeparts" / table_name).string(),
      {{"write.parquet.compression-codec", "zstd"},
       {"write.parquet.compression-level", "3"}});
  if (!created.has_value()) {
    *error = "CreateTable " + table_name + ": " + created.error().message;
    return false;
  }
  auto table = std::move(created.value());
  if (!files.empty()) {
    auto app_r = table->NewFastAppend();
    if (!app_r.has_value()) {
      *error = "NewFastAppend: " + app_r.error().message;
      return false;
    }
    auto app = std::move(app_r.value());
    for (const auto& f : files) app->AppendFile(f);
    auto cs = app->Commit();
    if (!cs.has_value()) {
      *error = "Commit: " + cs.error().message;
      return false;
    }
    auto rs = table->Refresh();
    if (!rs.has_value()) {
      *error = "Refresh: " + rs.error().message;
      return false;
    }
  }
  *metadata_location = std::string(table->metadata_file_location());
  return true;
}

}  // namespace primeparts::catalog
