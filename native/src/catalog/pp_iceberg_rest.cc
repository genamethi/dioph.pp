#include "primeparts/catalog/pp_iceberg_rest.h"

#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <utility>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "iceberg/arrow/arrow_io_util.h"
#include "iceberg/catalog/rest/catalog_properties.h"
#include "iceberg/catalog/rest/rest_catalog.h"
#include "iceberg/catalog/sql/sql_catalog.h"
#include "iceberg/sort_order.h"
#include "iceberg/table.h"
#include "iceberg/update/fast_append.h"

#include "primeparts/catalog/pp_lmdb_store.h"
#include "primeparts/common/arrow_init.h"
#include "primeparts/common/uri.h"
#include "primeparts/scan/table_traits.h"

namespace primeparts::catalog {

namespace {

fs::path NamespaceDir(const fs::path& root, const iceberg::Namespace& ns) {
  fs::path p = root;
  for (const auto& level : ns.levels) p /= level;
  return p;
}

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
  fs::remove(src, ec);
  return true;
}

}  // namespace

std::string NamespaceUrlPath(const iceberg::Namespace& ns) {
  std::string out;
  for (const auto& level : ns.levels) {
    if (!out.empty()) out += "%1F";
    out += level;
  }
  return out;
}

iceberg::Namespace ResolveNamespace(const std::string& name) {
  return iceberg::Namespace{{name}};
}

std::shared_ptr<iceberg::FileIO> LocalIO() {
  return std::shared_ptr<iceberg::FileIO>(iceberg::arrow::MakeLocalFileIO());
}

fs::path StagingDataDir(const fs::path& warehouse, const iceberg::Namespace& ns,
                        const std::string& table_name) {
  const fs::path wh = warehouse.lexically_normal();
  const fs::path parent = wh.has_parent_path() ? wh.parent_path() : wh;
  std::string tag = wh.filename().string();
  if (tag.empty()) tag = "warehouse";
  return NamespaceDir(parent / ".pp-staging" / tag, ns) / table_name;
}

std::shared_ptr<iceberg::Catalog> MakeCatalog(const RestOptions& opts,
                                              const fs::path& warehouse,
                                              std::string* mode,
                                              std::string* error) {
  if (opts.rest_uri.empty()) {
    if (error) *error = "MakeCatalog: rest_uri is required";
    return nullptr;
  }
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
  auto cat = r.value()->AsCatalog();
  if (!cat.has_value()) { *error = cat.error().message; return nullptr; }
  *mode = "rest";
  return std::move(cat.value());
}

LocalCatalog MakeLocalCatalogWithStore(const fs::path& warehouse,
                                       std::string* error) {
  common::EnsureArrowRegistration();

  auto store_r = MakeLmdbCatalogStore(warehouse / "catalog.lmdb", kCatalogName);
  if (!store_r.has_value()) {
    if (error) *error = "MakeLmdbCatalogStore: " + store_r.error().message;
    return {};
  }
  std::shared_ptr<iceberg::sql::CatalogStore> store = store_r.value();
  iceberg::sql::SqlCatalogConfig cfg;
  cfg.name = kCatalogName;
  cfg.warehouse_location = fs::absolute(warehouse).lexically_normal().string();
  auto cat_r = iceberg::sql::SqlCatalog::Make(cfg, LocalIO(), store);
  if (!cat_r.has_value()) {
    if (error) *error = "SqlCatalog::Make: " + cat_r.error().message;
    return {};
  }
  return {std::move(cat_r.value()), std::move(store)};
}

std::shared_ptr<iceberg::Catalog> MakeLocalCatalog(const fs::path& warehouse,
                                                   std::string* error) {
  return MakeLocalCatalogWithStore(warehouse, error).catalog;
}

bool RestServerReachable(const std::string& rest_uri) {
  if (rest_uri.empty()) return false;
  httplib::Client cli(rest_uri);
  cli.set_connection_timeout(1, 0);
  cli.set_read_timeout(2, 0);
  auto res = cli.Get("/v1/config");
  return res && res->status == 200;
}

std::string AdvertisedWarehouse(const std::string& rest_uri) {
  if (rest_uri.empty()) return {};
  httplib::Client cli(rest_uri);
  cli.set_connection_timeout(1, 0);
  cli.set_read_timeout(2, 0);
  auto res = cli.Get("/v1/config");
  if (!res || res->status != 200) return {};
  const auto body = nlohmann::json::parse(res->body, nullptr, false);
  if (body.is_discarded()) return {};
  const auto ov = body.find("overrides");
  if (ov == body.end() || !ov->is_object()) return {};
  const auto wh = ov->find("warehouse");
  if (wh == ov->end() || !wh->is_string()) return {};
  return wh->get<std::string>();
}

std::string_view FieldBoundStateName(FieldBoundState state) {
  switch (state) {
    case FieldBoundState::kTableAbsent:
      return "table-absent";
    case FieldBoundState::kNoSnapshot:
      return "no-snapshot";
    case FieldBoundState::kSnapshotNoBound:
      return "snapshot-no-bound";
    case FieldBoundState::kPresent:
      return "present";
  }
  return "no-snapshot";
}

bool FetchFieldBound(const std::string& rest_uri, const iceberg::Namespace& ns,
                     const std::string& table, const std::string& field,
                     int64_t* out, FieldBoundState* state, std::string* error) {
  *state = FieldBoundState::kNoSnapshot;
  if (rest_uri.empty()) {
    if (error) *error = "empty rest_uri";
    return false;
  }
  httplib::Client cli(rest_uri);
  cli.set_connection_timeout(2, 0);
  cli.set_read_timeout(10, 0);
  const std::string path = "/v1/namespaces/" + NamespaceUrlPath(ns) +
                           "/tables/" + table +
                           "/field-upper-bound?field=" + field;
  auto res = cli.Get(path);
  if (!res) {
    if (error) *error = "no response from " + rest_uri;
    return false;
  }
  if (res->status == 404) {
    *state = FieldBoundState::kTableAbsent;
    return true;
  }
  if (res->status != 200) {
    if (error) *error = "HTTP " + std::to_string(res->status) + ": " + res->body;
    return false;
  }
  try {
    auto body = nlohmann::json::parse(res->body);
    auto sit = body.find("state");
    if (sit != body.end() && sit->is_string()) {
      const std::string name = sit->get<std::string>();
      if (name == "table-absent") *state = FieldBoundState::kTableAbsent;
      else if (name == "no-snapshot") *state = FieldBoundState::kNoSnapshot;
      else if (name == "snapshot-no-bound") *state = FieldBoundState::kSnapshotNoBound;
      else if (name == "present") *state = FieldBoundState::kPresent;
      else {
        if (error) *error = "unknown field-upper-bound state '" + name + "'";
        return false;
      }
    }
    auto it = body.find("upper_bound");
    if (it == body.end() || it->is_null()) {
      if (*state == FieldBoundState::kPresent) {
        if (error) *error = "field-upper-bound reported state=present with null upper_bound";
        return false;
      }
      return true;
    }
    *out = it->get<int64_t>();
    *state = FieldBoundState::kPresent;
    return true;
  } catch (const std::exception& e) {
    if (error) *error = std::string("parse: ") + e.what();
    return false;
  }
}

bool FetchFieldUpperBound(const std::string& rest_uri,
                          const iceberg::Namespace& ns,
                          const std::string& table, const std::string& field,
                          int64_t* out, bool* present, std::string* error) {
  FieldBoundState state = FieldBoundState::kNoSnapshot;
  *present = false;
  if (!FetchFieldBound(rest_uri, ns, table, field, out, &state, error)) return false;
  if (state == FieldBoundState::kTableAbsent) {
    if (error) *error = "table does not exist: " + table;
    return false;
  }
  *present = state == FieldBoundState::kPresent;
  return true;
}

std::shared_ptr<iceberg::Catalog> OpenCatalog(const fs::path& warehouse,
                                              const std::string& rest_uri,
                                              std::string* mode,
                                              std::string* error) {
  const std::string& uri = rest_uri;
  if (uri.empty()) {
    if (error) *error = "no rest_uri given";
    return nullptr;
  }
  if (!RestServerReachable(uri)) {
    if (error) *error = "catalogd unreachable at " + uri + " (start pp-catalogd)";
    return nullptr;
  }
  const std::string served = AdvertisedWarehouse(uri);
  if (!served.empty()) {
    const fs::path want = warehouse.lexically_normal();
    const fs::path have = fs::path(served).lexically_normal();
    if (want != have) {
      if (error)
        *error = "warehouse mismatch: " + uri + " serves " + have.string() +
                 ", this process was given " + want.string() +
                 " (data files and catalog metadata would land in different "
                 "warehouses)";
      return nullptr;
    }
  }
  RestOptions opts;
  opts.rest_uri = uri;
  std::string m;
  std::string rest_err;
  auto cat = MakeCatalog(opts, warehouse, &m, &rest_err);
  if (!cat) {
    if (error) *error = "REST catalog " + uri + " init failed: " + rest_err;
    return nullptr;
  }
  if (mode) *mode = m;
  return cat;
}

fs::path TableMetadataPath(const std::shared_ptr<iceberg::Catalog>& catalog,
                           const iceberg::Namespace& ns,
                           const std::string& table, std::string* error) {
  iceberg::TableIdentifier id{.ns = ns, .name = table};
  auto t = catalog->LoadTable(id);
  if (!t.has_value()) {
    if (error)
      *error = "LoadTable(" + id.ToString() + "): " + t.error().message;
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
               const iceberg::Namespace& ns, const fs::path& warehouse,
               const std::string& table, bool purge, std::string* error) {
  iceberg::TableIdentifier ident{.ns = ns, .name = table};

  auto loaded = catalog->LoadTable(ident);
  if (!loaded.has_value()) {
    if (purge) {
      std::error_code ec;
      fs::remove_all(NamespaceDir(warehouse, ns) / table, ec);
    }
    return true;
  }
  fs::path base(common::StripFileScheme(std::string(loaded.value()->location())));

  auto del = catalog->DropTable(ident, purge);
  if (!del.has_value()) {
    if (error)
      *error = "DropTable(" + ident.ToString() + "): " + del.error().message;
    return false;
  }

  if (purge && !base.empty()) {
    std::error_code ec;
    fs::remove_all(base, ec);
  }
  return true;
}

std::shared_ptr<iceberg::Table> EnsureTable(
    const std::shared_ptr<iceberg::Catalog>& catalog,
    const iceberg::Namespace& ns, const fs::path& warehouse,
    const std::string& table_name,
    const std::shared_ptr<iceberg::Schema>& schema,
    const std::shared_ptr<iceberg::PartitionSpec>& spec,
    const TableDeclaration& declare, std::string* error) {
  iceberg::TableIdentifier ident{.ns = ns, .name = table_name};
  if (!EnsureNamespace(catalog, ident.ns, error)) return nullptr;

  auto loaded = catalog->LoadTable(ident);
  if (loaded.has_value()) return std::move(loaded.value());

  if (!declare.sort_order) {
    if (error) {
      *error = "CreateTable " + table_name +
               ": no sort order declared; pass iceberg::SortOrder::Unsorted() "
               "to create an unordered table";
    }
    return nullptr;
  }
  if (primeparts::scan::CheckSortOrder(*schema, *declare.sort_order, error) !=
      primeparts::scan::SortOrderSupport::kOk) {
    if (error) *error = "CreateTable " + table_name + ": " + *error;
    return nullptr;
  }

  const fs::path table_dir =
      fs::absolute(NamespaceDir(warehouse, ns) / table_name).lexically_normal();
  std::error_code ec;
  fs::create_directories(table_dir / "metadata", ec);
  std::unordered_map<std::string, std::string> properties{
      {"write.parquet.compression-codec", "zstd"},
      {"write.parquet.compression-level", "3"}};
  for (const auto& [k, v] : declare.properties) properties[k] = v;
  auto created = catalog->CreateTable(ident, schema, spec, declare.sort_order,
                                      table_dir.string(), properties);
  if (!created.has_value()) {
    if (error)
      *error = "CreateTable " + table_name + ": " + created.error().message;
    return nullptr;
  }
  return std::move(created.value());
}

bool MoveStagedFilesInto(
    const std::shared_ptr<iceberg::Table>& table, const iceberg::Namespace& ns,
    const fs::path& warehouse, const std::string& table_name,
    const std::vector<std::shared_ptr<iceberg::DataFile>>& files,
    std::string* error) {
  const fs::path data_dir =
      fs::absolute(fs::path(common::StripFileScheme(std::string(
                       table->location()))) /
                   "data")
          .lexically_normal();
  const fs::path staging_root = StagingDataDir(warehouse, ns, table_name);
  for (const auto& f : files) {
    const fs::path src = common::StripFileScheme(f->file_path);
    fs::path rel = src.lexically_relative(staging_root);
    if (rel.empty() || rel.native().rfind("..", 0) == 0) rel = src.filename();
    const fs::path dst = data_dir / rel;
    if (src == dst) continue;
    if (!MoveStagedFile(src, dst, error)) return false;
    f->file_path = dst.string();
  }
  std::error_code ec;
  fs::remove_all(staging_root, ec);
  fs::remove(staging_root.parent_path(), ec);
  return true;
}

bool CommitFiles(const std::shared_ptr<iceberg::Catalog>& catalog,
                 const iceberg::Namespace& ns, const fs::path& warehouse,
                 const std::string& table_name,
                 const std::shared_ptr<iceberg::Schema>& schema,
                 const std::shared_ptr<iceberg::PartitionSpec>& spec,
                 const TableDeclaration& declare,
                 const std::vector<std::shared_ptr<iceberg::DataFile>>& files,
                 std::string* metadata_location, std::string* error) {
  auto table = EnsureTable(catalog, ns, warehouse, table_name, schema, spec,
                           declare, error);
  if (!table) return false;

  if (!files.empty()) {
    if (!MoveStagedFilesInto(table, ns, warehouse, table_name, files, error))
      return false;
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
  }
  *metadata_location = std::string(table->metadata_file_location());
  return true;
}

}  // namespace primeparts::catalog
