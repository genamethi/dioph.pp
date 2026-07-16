#include "primeparts/catalog/pp_commit.h"

#include <memory>
#include <utility>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "iceberg/catalog.h"
#include "iceberg/catalog/sql/catalog_store.h"
#include "iceberg/json_serde_internal.h"
#include "iceberg/result.h"
#include "iceberg/snapshot.h"
#include "iceberg/table.h"
#include "iceberg/table_identifier.h"
#include "iceberg/table_metadata.h"
#include "iceberg/table_requirement.h"
#include "iceberg/table_requirements.h"
#include "iceberg/table_update.h"
#include "iceberg/update/fast_append.h"

#include "primeparts/catalog/pp_iceberg_rest.h"

namespace primeparts::catalog {

namespace {

using json = nlohmann::json;

struct TableChange {
  iceberg::TableIdentifier id;
  std::vector<std::unique_ptr<iceberg::TableRequirement>> reqs;
  std::vector<std::unique_ptr<iceberg::TableUpdate>> updates;
};

bool AssembleChange(const std::shared_ptr<iceberg::Catalog>& catalog,
                    const iceberg::Namespace& ns, const fs::path& warehouse,
                    TableCommitSpec& spec, TableChange* out,
                    std::string* error) {
  auto table = EnsureTable(catalog, ns, warehouse, spec.table_name, spec.schema,
                           spec.spec, spec.declare, error);
  if (!table) return false;
  out->id = table->name();
  if (spec.files.empty()) return true;

  if (!MoveStagedFilesInto(table, ns, warehouse, spec.table_name, spec.files,
                           error))
    return false;

  auto app_r = table->NewFastAppend();
  if (!app_r.has_value()) {
    *error = "NewFastAppend " + spec.table_name + ": " + app_r.error().message;
    return false;
  }
  auto app = std::move(app_r.value());
  for (const auto& f : spec.files) app->AppendFile(f);
  for (const auto& [key, value] : spec.summary) app->Set(key, value);

  auto applied = app->iceberg::SnapshotUpdate::Apply();
  if (!applied.has_value()) {
    *error = "Apply " + spec.table_name + ": " + applied.error().message;
    return false;
  }
  const auto& snapshot = applied.value().snapshot;
  const std::string& branch = applied.value().target_branch;

  out->updates.push_back(std::make_unique<iceberg::table::AddSnapshot>(snapshot));
  out->updates.push_back(std::make_unique<iceberg::table::SetSnapshotRef>(
      branch, snapshot->snapshot_id, iceberg::SnapshotRefType::kBranch));

  auto reqs =
      iceberg::TableRequirements::ForUpdateTable(*table->metadata(), out->updates);
  if (!reqs.has_value()) {
    *error = "ForUpdateTable " + spec.table_name + ": " + reqs.error().message;
    return false;
  }
  out->reqs = std::move(reqs.value());
  return true;
}

json IdentifierToJson(const iceberg::TableIdentifier& id) {
  json levels = json::array();
  for (const auto& lvl : id.ns.levels) levels.push_back(lvl);
  return json{{"namespace", std::move(levels)}, {"name", id.name}};
}

bool ChangeToJson(const TableChange& c, json* out, std::string* error) {
  json reqs = json::array();
  for (const auto& r : c.reqs) reqs.push_back(iceberg::ToJson(*r));
  json ups = json::array();
  for (const auto& u : c.updates) {
    auto j = iceberg::ToJson(*u);
    if (!j.has_value()) {
      *error = "serialize update: " + j.error().message;
      return false;
    }
    ups.push_back(std::move(j.value()));
  }
  *out = json{{"identifier", IdentifierToJson(c.id)},
              {"requirements", std::move(reqs)},
              {"updates", std::move(ups)}};
  return true;
}

bool AssembleChanges(const std::shared_ptr<iceberg::Catalog>& catalog,
                     const iceberg::Namespace& ns, const fs::path& warehouse,
                     std::vector<TableCommitSpec>& specs,
                     std::vector<TableChange>* out, std::string* error) {
  out->reserve(specs.size());
  for (auto& spec : specs) {
    TableChange c;
    if (!AssembleChange(catalog, ns, warehouse, spec, &c, error)) return false;
    if (!c.updates.empty()) out->push_back(std::move(c));
  }
  return true;
}

bool ChangesToBody(const std::vector<TableChange>& changes, json* body,
                   std::string* error) {
  json table_changes = json::array();
  for (const auto& c : changes) {
    json tc;
    if (!ChangeToJson(c, &tc, error)) return false;
    table_changes.push_back(std::move(tc));
  }
  *body = json{{"table-changes", std::move(table_changes)}};
  return true;
}

}  // namespace

bool AssembleTransactionBody(const std::shared_ptr<iceberg::Catalog>& catalog,
                             const iceberg::Namespace& ns,
                             const fs::path& warehouse,
                             std::vector<TableCommitSpec>& specs,
                             std::string* body_json, std::string* error) {
  std::vector<TableChange> changes;
  if (!AssembleChanges(catalog, ns, warehouse, specs, &changes, error))
    return false;
  json body;
  if (!ChangesToBody(changes, &body, error)) return false;
  *body_json = body.dump();
  return true;
}

bool CommitFilesAtomic(const std::shared_ptr<iceberg::Catalog>& catalog,
                       const std::shared_ptr<iceberg::sql::CatalogStore>& store,
                       const std::string& rest_uri,
                       const iceberg::Namespace& ns, const fs::path& warehouse,
                       std::vector<TableCommitSpec>& specs, std::string* error) {
  std::vector<TableChange> changes;
  if (!AssembleChanges(catalog, ns, warehouse, specs, &changes, error))
    return false;
  if (changes.empty()) return true;

  if (store) {
    auto st = store->RunInTransaction([&]() -> iceberg::Status {
      for (auto& c : changes) {
        auto r = catalog->UpdateTable(c.id, c.reqs, c.updates);
        if (!r.has_value()) return std::unexpected(r.error());
      }
      return {};
    });
    if (!st.has_value()) {
      *error = "commit transaction: " + st.error().message;
      return false;
    }
    return true;
  }

  json body;
  if (!ChangesToBody(changes, &body, error)) return false;
  httplib::Client cli(rest_uri);
  cli.set_read_timeout(30, 0);
  auto res = cli.Post("/v1/transactions/commit", body.dump(), "application/json");
  if (!res) {
    *error = "POST /v1/transactions/commit: no response from " + rest_uri;
    return false;
  }
  if (res->status != 204) {
    *error = "commit transaction HTTP " + std::to_string(res->status) + ": " +
             res->body;
    return false;
  }
  return true;
}

}  // namespace primeparts::catalog
