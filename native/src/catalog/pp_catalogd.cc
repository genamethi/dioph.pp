
#include "primeparts/catalog/pp_catalogd.h"

#include <atomic>
#include <csignal>
#include <chrono>
#include <unordered_map>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/catalog/plan_store.h"
#include "primeparts/scan/scan_planner.h"
#include "primeparts/scan/table_traits.h"

#include "iceberg/catalog.h"
#include "iceberg/catalog/sql/catalog_store.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/manifest/manifest_list.h"
#include "iceberg/manifest/manifest_reader.h"
#include "iceberg/partition_spec.h"
#include "iceberg/result.h"
#include "iceberg/schema.h"
#include "iceberg/schema_field.h"
#include "iceberg/snapshot.h"
#include "iceberg/sort_order.h"
#include "iceberg/table.h"
#include "iceberg/table_identifier.h"
#include "iceberg/table_metadata.h"
#include "iceberg/table_requirement.h"
#include "iceberg/table_update.h"
#include "iceberg/type_fwd.h"

#include "iceberg/catalog/rest/json_serde_internal.h"
#include "iceberg/catalog/rest/types.h"
#include "iceberg/json_serde_internal.h"

namespace primeparts::catalog {

namespace {

using json = nlohmann::json;
namespace ir = iceberg::rest;


void SendJson(httplib::Response& res, int status, const json& body) {
  res.status = status;
  res.set_content(body.dump(), "application/json");
}

void SendError(httplib::Response& res, int status, std::string_view type,
               std::string_view message) {
  json body = {{"error",
                {{"message", message}, {"type", type}, {"code", status}}}};
  SendJson(res, status, body);
}

int HttpStatusFor(iceberg::ErrorKind kind) {
  switch (kind) {
    case iceberg::ErrorKind::kAlreadyExists:
      return 409;
    case iceberg::ErrorKind::kNamespaceNotEmpty:
      return 409;
    case iceberg::ErrorKind::kCommitFailed:
      return 409;
    case iceberg::ErrorKind::kNoSuchTable:
    case iceberg::ErrorKind::kNoSuchNamespace:
    case iceberg::ErrorKind::kNoSuchView:
    case iceberg::ErrorKind::kNotFound:
      return 404;
    case iceberg::ErrorKind::kBadRequest:
    case iceberg::ErrorKind::kInvalid:
    case iceberg::ErrorKind::kInvalidArgument:
    case iceberg::ErrorKind::kValidationFailed:
    case iceberg::ErrorKind::kJsonParseError:
      return 400;
    case iceberg::ErrorKind::kNotAuthorized:
    case iceberg::ErrorKind::kTokenExpired:
      return 401;
    case iceberg::ErrorKind::kForbidden:
      return 403;
    case iceberg::ErrorKind::kNotImplemented:
    case iceberg::ErrorKind::kNotSupported:
      return 501;
    case iceberg::ErrorKind::kServiceUnavailable:
      return 503;
    default:
      return 500;
  }
}

template <typename E>
void SendIcebergError(httplib::Response& res, const E& err) {
  int status = HttpStatusFor(err.kind);
  SendError(res, status, "IcebergError", err.message);
}


iceberg::Namespace ParseNamespace(const std::string& encoded) {
  iceberg::Namespace ns;
  std::string level;
  for (char c : encoded) {
    if (c == '\x1f') {
      ns.levels.push_back(level);
      level.clear();
    } else {
      level.push_back(c);
    }
  }
  ns.levels.push_back(level);
  return ns;
}

iceberg::TableIdentifier ParseIdentifier(const json& j) {
  iceberg::TableIdentifier id;
  if (auto it = j.find("namespace"); it != j.end() && it->is_array()) {
    for (const auto& lvl : *it) id.ns.levels.push_back(lvl.get<std::string>());
  }
  if (auto it = j.find("name"); it != j.end()) id.name = it->get<std::string>();
  return id;
}

bool ParseReqsUpdates(
    const json& body,
    std::vector<std::unique_ptr<iceberg::TableRequirement>>* reqs,
    std::vector<std::unique_ptr<iceberg::TableUpdate>>* updates,
    std::string* error) {
  if (auto it = body.find("requirements"); it != body.end()) {
    for (const auto& jr : *it) {
      auto r = iceberg::TableRequirementFromJson(jr);
      if (!r.has_value()) { *error = r.error().message; return false; }
      reqs->push_back(std::move(r.value()));
    }
  }
  if (auto it = body.find("updates"); it != body.end()) {
    for (const auto& ju : *it) {
      auto u = iceberg::TableUpdateFromJson(ju);
      if (!u.has_value()) { *error = u.error().message; return false; }
      updates->push_back(std::move(u.value()));
    }
  }
  return true;
}

int StatusForSortOrder(primeparts::scan::SortOrderSupport support) {
  return support == primeparts::scan::SortOrderSupport::kUnsupported ? 501 : 400;
}

const char* TypeForStatus(int status) {
  return status == 501 ? "NotImplemented" : "BadRequest";
}

bool SortOrdersInUpdatesResolvable(
    const iceberg::TableMetadata* metadata,
    const std::vector<std::unique_ptr<iceberg::TableUpdate>>& updates,
    int* status, std::string* error) {
  std::vector<std::shared_ptr<iceberg::Schema>> schemas;
  std::vector<std::shared_ptr<iceberg::SortOrder>> orders;
  if (metadata) {
    schemas = metadata->schemas;
    orders = metadata->sort_orders;
  }

  bool touches_sort_order = false;
  std::shared_ptr<iceberg::Schema> last_added_schema;
  int32_t current_schema_id = metadata ? metadata->current_schema_id : -1;
  for (const auto& u : updates) {
    switch (u->kind()) {
      case iceberg::TableUpdate::Kind::kAddSchema: {
        const auto& add = static_cast<const iceberg::table::AddSchema&>(*u);
        if (add.schema()) {
          last_added_schema = add.schema();
          schemas.push_back(add.schema());
        }
        break;
      }
      case iceberg::TableUpdate::Kind::kSetCurrentSchema: {
        const auto& set =
            static_cast<const iceberg::table::SetCurrentSchema&>(*u);
        current_schema_id = set.schema_id() == -1 && last_added_schema
                                ? last_added_schema->schema_id()
                                : set.schema_id();
        break;
      }
      case iceberg::TableUpdate::Kind::kAddSortOrder:
      case iceberg::TableUpdate::Kind::kSetDefaultSortOrder:
        touches_sort_order = true;
        break;
      default:
        break;
    }
  }
  if (!touches_sort_order) return true;

  std::shared_ptr<iceberg::Schema> schema;
  for (const auto& s : schemas) {
    if (s && s->schema_id() == current_schema_id) schema = s;
  }
  if (!schema) return true;

  std::shared_ptr<iceberg::SortOrder> last_added_order;
  for (const auto& u : updates) {
    if (u->kind() == iceberg::TableUpdate::Kind::kAddSortOrder) {
      const auto& add = static_cast<const iceberg::table::AddSortOrder&>(*u);
      if (!add.sort_order()) continue;
      const auto support =
          primeparts::scan::CheckSortOrder(*schema, *add.sort_order(), error);
      if (support != primeparts::scan::SortOrderSupport::kOk) {
        *status = StatusForSortOrder(support);
        return false;
      }
      last_added_order = add.sort_order();
      orders.push_back(add.sort_order());
      continue;
    }
    if (u->kind() != iceberg::TableUpdate::Kind::kSetDefaultSortOrder) continue;

    const auto& set =
        static_cast<const iceberg::table::SetDefaultSortOrder&>(*u);
    std::shared_ptr<iceberg::SortOrder> target;
    if (set.sort_order_id() == -1) {
      target = last_added_order;
    } else {
      for (const auto& o : orders) {
        if (o && o->order_id() == set.sort_order_id()) target = o;
      }
    }
    if (!target) {
      if (set.sort_order_id() == iceberg::SortOrder::kUnsortedOrderId) continue;
      if (error) {
        *error = "set-default-sort-order references sort order id " +
                 std::to_string(set.sort_order_id()) +
                 " which this update does not add and the table does not have";
      }
      *status = 400;
      return false;
    }
    const auto support =
        primeparts::scan::CheckSortOrder(*schema, *target, error);
    if (support != primeparts::scan::SortOrderSupport::kOk) {
      *status = StatusForSortOrder(support);
      return false;
    }
  }
  return true;
}

bool ParseBody(const httplib::Request& req, httplib::Response& res, json* out) {
  *out = json::parse(req.body, nullptr, false);
  if (out->is_discarded()) {
    SendError(res, 400, "BadRequest", "request body is not valid JSON");
    return false;
  }
  return true;
}

iceberg::Result<std::string> TableResultBody(
    const std::shared_ptr<iceberg::Table>& table,
    const std::string& scan_planning_mode) {
  const auto& meta = table->metadata();
  if (!meta) {
    return std::unexpected(
        iceberg::Error{iceberg::ErrorKind::kInvalid, "table metadata is null"});
  }
  auto meta_str = iceberg::ToJsonString(*meta);
  if (!meta_str.has_value()) return std::unexpected(meta_str.error());
  json body;
  body["metadata-location"] = std::string(table->metadata_file_location());
  body["metadata"] = json::parse(meta_str.value());
  if (!scan_planning_mode.empty()) {
    body["config"] = json{{"scan-planning-mode", scan_planning_mode}};
  }
  return body.dump();
}

void SendTableResult(httplib::Response& res, int status,
                     const std::shared_ptr<iceberg::Table>& table,
                     const std::string& scan_planning_mode) {
  auto body = TableResultBody(table, scan_planning_mode);
  if (!body.has_value()) return SendIcebergError(res, body.error());
  res.status = status;
  res.set_content(body.value(), "application/json");
}


void SendPlanJson(httplib::Response& res, int status,
                  const iceberg::Result<json>& body,
                  const iceberg::Status& valid) {
  if (!valid.has_value()) return SendIcebergError(res, valid.error());
  if (!body.has_value()) return SendIcebergError(res, body.error());
  SendJson(res, status, body.value());
}

std::unordered_map<int32_t, std::shared_ptr<iceberg::PartitionSpec>>
PartitionSpecsById(const iceberg::TableMetadata& metadata) {
  std::unordered_map<int32_t, std::shared_ptr<iceberg::PartitionSpec>> specs;
  for (const auto& spec : metadata.partition_specs) {
    if (spec) specs.emplace(spec->spec_id(), spec);
  }
  return specs;
}

primeparts::scan::ScanPlanRequest ToScanPlanRequest(
    const ir::PlanTableScanRequest& in) {
  primeparts::scan::ScanPlanRequest out;
  out.snapshot_id = in.snapshot_id;
  out.select = in.select;
  out.filter = in.filter;
  out.min_rows_requested = in.min_rows_requested;
  out.case_sensitive = in.case_sensitive;
  out.use_snapshot_schema = in.use_snapshot_schema;
  out.start_snapshot_id = in.start_snapshot_id;
  out.end_snapshot_id = in.end_snapshot_id;
  out.stats_fields = in.stats_fields;
  return out;
}

std::vector<std::shared_ptr<iceberg::FileScanTask>> InnerTasks(
    const std::vector<primeparts::scan::FileScanTask>& tasks) {
  std::vector<std::shared_ptr<iceberg::FileScanTask>> inner;
  inner.reserve(tasks.size());
  for (const auto& task : tasks) {
    if (task.inner) inner.push_back(task.inner);
  }
  return inner;
}

ir::PlanStatus WireStatus(PlanStatus status) {
  switch (status) {
    case PlanStatus::kSubmitted:
      return ir::PlanStatus::kSubmitted;
    case PlanStatus::kCompleted:
      return ir::PlanStatus::kCompleted;
    case PlanStatus::kCancelled:
      return ir::PlanStatus::kCancelled;
    case PlanStatus::kFailed:
      return ir::PlanStatus::kFailed;
  }
  return ir::PlanStatus::kFailed;
}

std::shared_ptr<iceberg::Table> LoadTableOr404(
    const std::shared_ptr<iceberg::Catalog>& catalog,
    const httplib::Request& req, httplib::Response& res) {
  iceberg::TableIdentifier id{.ns = ParseNamespace(req.matches[1]),
                              .name = req.matches[2]};
  auto r = catalog->LoadTable(id);
  if (!r.has_value()) {
    SendIcebergError(res, r.error());
    return nullptr;
  }
  return r.value();
}

std::atomic<httplib::Server*> g_server{nullptr};

void HandleSignal(int) {
  if (auto* s = g_server.load()) s->stop();
}

int32_t FieldIdByName(const iceberg::Schema& schema, std::string_view name) {
  for (const auto& f : schema.fields()) {
    if (f.name() == name) return f.field_id();
  }
  return -1;
}

bool FieldUpperBound(const std::shared_ptr<iceberg::Catalog>& catalog,
                     const iceberg::TableIdentifier& id, const std::string& field,
                     int64_t* out, bool* present, FieldBoundState* state,
                     std::string* error) {
  *present = false;
  *state = FieldBoundState::kNoSnapshot;
  auto tbl = catalog->LoadTable(id);
  if (!tbl.has_value()) {
    *error = tbl.error().message;
    if (tbl.error().kind == iceberg::ErrorKind::kNoSuchTable) {
      *state = FieldBoundState::kTableAbsent;
    }
    return false;
  }
  auto table = tbl.value();

  auto snap = table->current_snapshot();
  if (!snap.has_value() || snap.value() == nullptr) return true;
  *state = FieldBoundState::kSnapshotNoBound;

  auto schema = table->schema();
  if (!schema.has_value()) {
    *error = schema.error().message;
    return false;
  }
  const int32_t fid = FieldIdByName(*schema.value(), field);
  if (fid < 0) {
    *error = "unknown field: " + field;
    return false;
  }

  auto spec = table->spec();
  if (!spec.has_value()) {
    *error = spec.error().message;
    return false;
  }

  iceberg::SnapshotCache cache(snap.value().get());
  auto manifests = cache.DataManifests(table->io());
  if (!manifests.has_value()) {
    *error = manifests.error().message;
    return false;
  }
  const int64_t snap_id = snap.value()->snapshot_id;
  int64_t best = 0;
  for (const auto& m : manifests.value()) {
    if (m.added_snapshot_id != snap_id) continue;

    auto reader = iceberg::ManifestReader::Make(m, table->io(), schema.value(),
                                                spec.value());
    if (!reader.has_value()) {
      *error = reader.error().message;
      return false;
    }
    auto entries = reader.value()->Select({"upper_bounds"}).LiveEntries();
    if (!entries.has_value()) {
      *error = entries.error().message;
      return false;
    }
    for (const auto& entry : entries.value()) {
      const auto& df = entry.data_file;
      if (df == nullptr) continue;
      auto it = df->upper_bounds.find(fid);
      if (it == df->upper_bounds.end() || it->second.size() < sizeof(int64_t)) {
        continue;
      }
      int64_t v = 0;
      std::memcpy(&v, it->second.data(), sizeof(int64_t));
      if (!*present || v > best) {
        best = v;
        *present = true;
      }
    }
  }
  if (*present) {
    *out = best;
    *state = FieldBoundState::kPresent;
  }
  return true;
}

class RouteTable {
 public:
  explicit RouteTable(httplib::Server& server) : svr_(server) {}

  void Get(const std::string& pattern, const std::string& spec_path,
           const std::vector<std::string>& advertise,
           httplib::Server::Handler handler) {
    Record(spec_path, advertise);
    svr_.Get(pattern, std::move(handler));
  }

  void Post(const std::string& pattern, const std::string& spec_path,
            const std::vector<std::string>& advertise,
            httplib::Server::Handler handler) {
    Record(spec_path, advertise);
    svr_.Post(pattern, std::move(handler));
  }

  void Delete(const std::string& pattern, const std::string& spec_path,
              const std::vector<std::string>& advertise,
              httplib::Server::Handler handler) {
    Record(spec_path, advertise);
    svr_.Delete(pattern, std::move(handler));
  }

  const json& endpoints() const { return endpoints_; }

 private:
  void Record(const std::string& spec_path,
              const std::vector<std::string>& advertise) {
    for (const auto& method : advertise) {
      endpoints_.push_back(method + " " + spec_path);
    }
  }

  httplib::Server& svr_;
  json endpoints_ = json::array();
};

}  // namespace

int RunCatalogd(const CatalogdOptions& opts) {
  std::string err;
  auto local = MakeLocalCatalogWithStore(opts.warehouse, &err);
  if (!local.catalog) {
    std::fprintf(stderr, "pp-catalogd: open catalog: %s\n", err.c_str());
    return 1;
  }
  auto catalog = local.catalog;
  auto store = local.store;
  const std::string planning_mode = opts.scan_planning_mode;
  auto plans = std::make_shared<PlanStore>(PlanStore::Config{
      .batch_tasks = opts.plan_batch_tasks,
      .ttl = std::chrono::seconds(opts.plan_ttl_seconds)});

  httplib::Server svr;
  svr.set_socket_options([](int sock) {
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const void*>(&yes), sizeof(yes));
  });
  RouteTable routes(svr);

  routes.Get("/v1/namespaces", "/v1/{prefix}/namespaces", {"GET"},
             [catalog](const httplib::Request& req,
                                      httplib::Response& res) {
    iceberg::Namespace parent;
    if (req.has_param("parent")) parent = ParseNamespace(req.get_param_value("parent"));
    auto r = catalog->ListNamespaces(parent);
    if (!r.has_value()) return SendIcebergError(res, r.error());
    ir::ListNamespacesResponse body{.namespaces = std::move(r.value())};
    SendJson(res, 200, ir::ToJson(body));
  });

  routes.Post("/v1/namespaces", "/v1/{prefix}/namespaces", {"POST"},
              [catalog](const httplib::Request& req,
                                       httplib::Response& res) {
    json body;
    if (!ParseBody(req, res, &body)) return;
    auto parsed = ir::CreateNamespaceRequestFromJson(body);
    if (!parsed.has_value()) return SendError(res, 400, "BadRequest", parsed.error().message);
    auto& cr = parsed.value();
    auto st = catalog->CreateNamespace(cr.namespace_, cr.properties);
    if (!st.has_value()) return SendIcebergError(res, st.error());
    ir::CreateNamespaceResponse resp{.namespace_ = cr.namespace_,
                                     .properties = cr.properties};
    SendJson(res, 200, ir::ToJson(resp));
  });

  routes.Get(R"(/v1/namespaces/([^/]+))", "/v1/{prefix}/namespaces/{namespace}",
             {"GET", "HEAD"}, [catalog](const httplib::Request& req,
                                                 httplib::Response& res) {
    auto ns = ParseNamespace(req.matches[1]);
    auto r = catalog->GetNamespaceProperties(ns);
    if (!r.has_value()) return SendIcebergError(res, r.error());
    if (req.method == "HEAD") {
      res.status = 204;
      return;
    }
    ir::GetNamespaceResponse resp{.namespace_ = ns, .properties = std::move(r.value())};
    SendJson(res, 200, ir::ToJson(resp));
  });


  routes.Delete(R"(/v1/namespaces/([^/]+))",
                "/v1/{prefix}/namespaces/{namespace}", {"DELETE"},
                [catalog](const httplib::Request& req,
                                                    httplib::Response& res) {
    auto st = catalog->DropNamespace(ParseNamespace(req.matches[1]));
    if (!st.has_value()) return SendIcebergError(res, st.error());
    res.status = 204;
  });

  routes.Post(R"(/v1/namespaces/([^/]+)/properties)",
              "/v1/{prefix}/namespaces/{namespace}/properties", {"POST"},
              [catalog](const httplib::Request& req, httplib::Response& res) {
             json body;
             if (!ParseBody(req, res, &body)) return;
             auto parsed = ir::UpdateNamespacePropertiesRequestFromJson(body);
             if (!parsed.has_value())
               return SendError(res, 400, "BadRequest", parsed.error().message);
             auto& ur = parsed.value();
             std::unordered_set<std::string> removals(ur.removals.begin(),
                                                      ur.removals.end());
             auto st = catalog->UpdateNamespaceProperties(
                 ParseNamespace(req.matches[1]), ur.updates, removals);
             if (!st.has_value()) return SendIcebergError(res, st.error());
             ir::UpdateNamespacePropertiesResponse resp;
             for (auto& [k, _] : ur.updates) resp.updated.push_back(k);
             resp.removed = ur.removals;
             SendJson(res, 200, ir::ToJson(resp));
           });

  routes.Get(R"(/v1/namespaces/([^/]+)/tables)",
             "/v1/{prefix}/namespaces/{namespace}/tables", {"GET"},
             [catalog](const httplib::Request& req, httplib::Response& res) {
            auto r = catalog->ListTables(ParseNamespace(req.matches[1]));
            if (!r.has_value()) return SendIcebergError(res, r.error());
            ir::ListTablesResponse body{.identifiers = std::move(r.value())};
            SendJson(res, 200, ir::ToJson(body));
          });

  routes.Post(R"(/v1/namespaces/([^/]+)/tables)",
              "/v1/{prefix}/namespaces/{namespace}/tables", {"POST"},
              [catalog, planning_mode](const httplib::Request& req, httplib::Response& res) {
             json body;
             if (!ParseBody(req, res, &body)) return;
             auto parsed = ir::CreateTableRequestFromJson(body);
             if (!parsed.has_value())
               return SendError(res, 400, "BadRequest", parsed.error().message);
             auto& cr = parsed.value();
             iceberg::TableIdentifier id{.ns = ParseNamespace(req.matches[1]),
                                         .name = cr.name};
             auto spec = cr.partition_spec ? cr.partition_spec
                                           : iceberg::PartitionSpec::Unpartitioned();
             if (!cr.write_order)
               return SendError(res, 400, "BadRequest",
                                "create-table requires write-order; send an "
                                "unsorted order to create an unordered table");
             if (!cr.schema)
               return SendError(res, 400, "BadRequest",
                                "create-table requires schema");
             std::string oerr;
             const auto support = primeparts::scan::CheckSortOrder(
                 *cr.schema, *cr.write_order, &oerr);
             if (support != primeparts::scan::SortOrderSupport::kOk) {
               const int status = StatusForSortOrder(support);
               return SendError(res, status, TypeForStatus(status), oerr);
             }
             auto r = catalog->CreateTable(id, cr.schema, spec, cr.write_order,
                                           cr.location, cr.properties);
             if (!r.has_value()) return SendIcebergError(res, r.error());
             SendTableResult(res, 200, r.value(), planning_mode);
           });

  routes.Post(R"(/v1/namespaces/([^/]+)/register)",
              "/v1/{prefix}/namespaces/{namespace}/register", {"POST"},
              [catalog, planning_mode](const httplib::Request& req, httplib::Response& res) {
             json body;
             if (!ParseBody(req, res, &body)) return;
             auto parsed = ir::RegisterTableRequestFromJson(body);
             if (!parsed.has_value())
               return SendError(res, 400, "BadRequest", parsed.error().message);
             auto& rr = parsed.value();
             iceberg::TableIdentifier id{.ns = ParseNamespace(req.matches[1]),
                                         .name = rr.name};
             auto r = catalog->RegisterTable(id, rr.metadata_location);
             if (!r.has_value()) return SendIcebergError(res, r.error());
             SendTableResult(res, 200, r.value(), planning_mode);
           });

  routes.Get(R"(/v1/namespaces/([^/]+)/tables/([^/]+))",
             "/v1/{prefix}/namespaces/{namespace}/tables/{table}",
             {"GET", "HEAD"},
             [catalog, planning_mode](const httplib::Request& req, httplib::Response& res) {
            iceberg::TableIdentifier id{.ns = ParseNamespace(req.matches[1]),
                                        .name = req.matches[2]};
            auto r = catalog->LoadTable(id);
            if (!r.has_value()) return SendIcebergError(res, r.error());
            if (req.method == "HEAD") {
              res.status = 204;
              return;
            }
            SendTableResult(res, 200, r.value(), planning_mode);
          });


  routes.Get(R"(/v1/namespaces/([^/]+)/tables/([^/]+)/field-upper-bound)",
             "/v1/{prefix}/namespaces/{namespace}/tables/{table}/field-upper-bound",
             {},
             [catalog](const httplib::Request& req, httplib::Response& res) {
            if (!req.has_param("field"))
              return SendError(res, 400, "BadRequest", "missing field param");
            const std::string field = req.get_param_value("field");
            iceberg::TableIdentifier id{.ns = ParseNamespace(req.matches[1]),
                                        .name = req.matches[2]};
            int64_t ub = 0;
            bool present = false;
            FieldBoundState state = FieldBoundState::kNoSnapshot;
            std::string err;
            if (!FieldUpperBound(catalog, id, field, &ub, &present, &state, &err)) {
              if (state == FieldBoundState::kTableAbsent)
                return SendError(res, 404, "NoSuchTableException", err);
              return SendError(res, 400, "BadRequest", err);
            }
            json body = {{"field", field},
                         {"state", FieldBoundStateName(state)}};
            if (present) body["upper_bound"] = ub;
            else body["upper_bound"] = nullptr;
            SendJson(res, 200, body);
          });

  routes.Post(
      R"(/v1/namespaces/([^/]+)/tables/([^/]+)/plan)",
      "/v1/{prefix}/namespaces/{namespace}/tables/{table}/plan", {"POST"},
      [catalog, plans](const httplib::Request& req, httplib::Response& res) {
        plans->ExpireIdle(PlanStore::Clock::now());
        json body;
        if (!ParseBody(req, res, &body)) return;
        auto parsed = ir::PlanTableScanRequestFromJson(body);
        if (!parsed.has_value()) return SendIcebergError(res, parsed.error());

        auto table = LoadTableOr404(catalog, req, res);
        if (!table) return;
        auto metadata = table->metadata();
        if (!metadata) {
          return SendError(res, 500, "ServerError", "table metadata is null");
        }
        auto schema = metadata->Schema();
        if (!schema.has_value()) return SendIcebergError(res, schema.error());

        auto request = ToScanPlanRequest(parsed.value());
        auto io = table->io();
        auto plan_id = plans->Submit(
            [metadata, io, request](primeparts::scan::ScanPlan* plan,
                                    std::string* error) {
              return primeparts::scan::PlanTableScan(metadata, io, request,
                                                     plan, error);
            });

        ir::PlanTableScanResponse response;
        response.plan_status = ir::PlanStatus::kSubmitted;
        response.plan_id = plan_id;
        SendPlanJson(res, 200,
                     ir::ToJson(response,
                                           PartitionSpecsById(*metadata),
                                           *schema.value()),
                     response.Validate());
      });

  routes.Get(
      R"(/v1/namespaces/([^/]+)/tables/([^/]+)/plan/([^/]+))",
      "/v1/{prefix}/namespaces/{namespace}/tables/{table}/plan/{plan-id}",
      {"GET"},
      [catalog, plans](const httplib::Request& req, httplib::Response& res) {
        plans->ExpireIdle(PlanStore::Clock::now());
        auto table = LoadTableOr404(catalog, req, res);
        if (!table) return;
        auto metadata = table->metadata();
        if (!metadata) {
          return SendError(res, 500, "ServerError", "table metadata is null");
        }
        auto schema = metadata->Schema();
        if (!schema.has_value()) return SendIcebergError(res, schema.error());

        PlanStore::Snapshot snap;
        if (!plans->Fetch(req.matches[3], &snap)) {
          return SendError(
              res, 404, "NoSuchPlanIdException",
              "unknown plan-id '" + std::string(req.matches[3]) + "'");
        }

        ir::FetchPlanningResultResponse response;
        response.plan_status = WireStatus(snap.status);
        if (snap.status == PlanStatus::kCompleted) {
          response.file_scan_tasks = InnerTasks(snap.tasks);
          if (!snap.plan_tasks.empty()) response.plan_tasks = snap.plan_tasks;
        }
        if (snap.status == PlanStatus::kFailed) {
          response.error = ir::ErrorResponse{
              .code = 500,
              .type = "ServerError",
              .message = snap.error.empty() ? "scan planning failed"
                                            : snap.error};
        }
        SendPlanJson(res, 200,
                     ir::ToJson(response,
                                           PartitionSpecsById(*metadata),
                                           *schema.value()),
                     response.Validate());
      });

  routes.Delete(
      R"(/v1/namespaces/([^/]+)/tables/([^/]+)/plan/([^/]+))",
      "/v1/{prefix}/namespaces/{namespace}/tables/{table}/plan/{plan-id}",
      {"DELETE"},
      [catalog, plans](const httplib::Request& req, httplib::Response& res) {
        plans->ExpireIdle(PlanStore::Clock::now());
        if (!LoadTableOr404(catalog, req, res)) return;
        if (!plans->Cancel(req.matches[3])) {
          return SendError(
              res, 404, "NoSuchPlanIdException",
              "unknown plan-id '" + std::string(req.matches[3]) + "'");
        }
        res.status = 204;
      });

  routes.Post(
      R"(/v1/namespaces/([^/]+)/tables/([^/]+)/tasks)",
      "/v1/{prefix}/namespaces/{namespace}/tables/{table}/tasks", {"POST"},
      [catalog, plans](const httplib::Request& req, httplib::Response& res) {
        plans->ExpireIdle(PlanStore::Clock::now());
        json body;
        if (!ParseBody(req, res, &body)) return;
        auto parsed = ir::FetchScanTasksRequestFromJson(body);
        if (!parsed.has_value()) return SendIcebergError(res, parsed.error());

        auto table = LoadTableOr404(catalog, req, res);
        if (!table) return;
        auto metadata = table->metadata();
        if (!metadata) {
          return SendError(res, 500, "ServerError", "table metadata is null");
        }
        auto schema = metadata->Schema();
        if (!schema.has_value()) return SendIcebergError(res, schema.error());

        std::vector<primeparts::scan::FileScanTask> tasks;
        if (!plans->FetchTasks(parsed.value().planTask, &tasks)) {
          return SendError(res, 404, "NoSuchPlanTaskException",
                           "unknown plan-task '" + parsed.value().planTask +
                               "'");
        }

        ir::FetchScanTasksResponse response;
        response.file_scan_tasks = InnerTasks(tasks);
        SendPlanJson(res, 200,
                     ir::ToJson(response,
                                           PartitionSpecsById(*metadata),
                                           *schema.value()),
                     response.Validate());
      });

  routes.Post(R"(/v1/namespaces/([^/]+)/tables/([^/]+))",
              "/v1/{prefix}/namespaces/{namespace}/tables/{table}", {"POST"},
              [catalog](const httplib::Request& req, httplib::Response& res) {
             json body;
             if (!ParseBody(req, res, &body)) return;
             iceberg::TableIdentifier id{.ns = ParseNamespace(req.matches[1]),
                                         .name = req.matches[2]};
             std::vector<std::unique_ptr<iceberg::TableRequirement>> requirements;
             std::vector<std::unique_ptr<iceberg::TableUpdate>> updates;
             std::string perr;
             if (!ParseReqsUpdates(body, &requirements, &updates, &perr))
               return SendError(res, 400, "BadRequest", perr);
             int ostatus = 400;
             const iceberg::TableMetadata* meta = nullptr;
             auto existing = catalog->LoadTable(id);
             if (existing.has_value() && existing.value())
               meta = existing.value()->metadata().get();
             if (!SortOrdersInUpdatesResolvable(meta, updates, &ostatus, &perr))
               return SendError(res, ostatus, TypeForStatus(ostatus), perr);
             auto r = catalog->UpdateTable(id, requirements, updates);
             if (!r.has_value()) return SendIcebergError(res, r.error());
             SendTableResult(res, 200, r.value(), std::string());
           });

  routes.Delete(R"(/v1/namespaces/([^/]+)/tables/([^/]+))",
                "/v1/{prefix}/namespaces/{namespace}/tables/{table}",
                {"DELETE"},
                [catalog](const httplib::Request& req, httplib::Response& res) {
               iceberg::TableIdentifier id{.ns = ParseNamespace(req.matches[1]),
                                           .name = req.matches[2]};
               bool purge = req.has_param("purgeRequested") &&
                            req.get_param_value("purgeRequested") == "true";
               auto st = catalog->DropTable(id, purge);
               if (!st.has_value()) return SendIcebergError(res, st.error());
               res.status = 204;
             });

  routes.Post("/v1/tables/rename", "/v1/{prefix}/tables/rename", {"POST"},
              [catalog](const httplib::Request& req,
                                          httplib::Response& res) {
    json body;
    if (!ParseBody(req, res, &body)) return;
    auto parsed = ir::RenameTableRequestFromJson(body);
    if (!parsed.has_value()) return SendError(res, 400, "BadRequest", parsed.error().message);
    auto& rr = parsed.value();
    auto st = catalog->RenameTable(rr.source, rr.destination);
    if (!st.has_value()) return SendIcebergError(res, st.error());
    res.status = 204;
  });

  routes.Post(R"(/v1/namespaces/([^/]+)/tables/([^/]+)/metrics)",
              "/v1/{prefix}/namespaces/{namespace}/tables/{table}/metrics",
              {"POST"},
              [](const httplib::Request&, httplib::Response& res) { res.status = 204; });

  routes.Post("/v1/transactions/commit", "/v1/{prefix}/transactions/commit",
              {"POST"},
              [catalog, store](const httplib::Request& req, httplib::Response& res) {
             json body;
             if (!ParseBody(req, res, &body)) return;
             auto tc = body.find("table-changes");
             if (tc == body.end() || !tc->is_array())
               return SendError(res, 400, "BadRequest",
                                "commit transaction requires table-changes[]");
             struct Change {
               iceberg::TableIdentifier id;
               std::vector<std::unique_ptr<iceberg::TableRequirement>> reqs;
               std::vector<std::unique_ptr<iceberg::TableUpdate>> updates;
             };
             std::vector<Change> changes;
             for (const auto& ch : *tc) {
               Change c;
               if (auto it = ch.find("identifier"); it != ch.end())
                 c.id = ParseIdentifier(*it);
               std::string perr;
               if (!ParseReqsUpdates(ch, &c.reqs, &c.updates, &perr))
                 return SendError(res, 400, "BadRequest", perr);
               int ostatus = 400;
               const iceberg::TableMetadata* meta = nullptr;
               auto existing = catalog->LoadTable(c.id);
               if (existing.has_value() && existing.value())
                 meta = existing.value()->metadata().get();
               if (!SortOrdersInUpdatesResolvable(meta, c.updates, &ostatus,
                                                  &perr))
                 return SendError(res, ostatus, TypeForStatus(ostatus), perr);
               changes.push_back(std::move(c));
             }
             auto st = store->RunInTransaction([&]() -> iceberg::Status {
               for (auto& c : changes) {
                 auto r = catalog->UpdateTable(c.id, c.reqs, c.updates);
                 if (!r.has_value()) return std::unexpected(r.error());
               }
               return {};
             });
             if (!st.has_value()) return SendIcebergError(res, st.error());
             res.status = 204;
           });

  svr.Get("/v1/config", [endpoints = routes.endpoints()](
                            const httplib::Request&, httplib::Response& res) {
    SendJson(res, 200,
             json{{"defaults", json::object()},
                  {"overrides", json::object()},
                  {"endpoints", endpoints}});
  });

  svr.set_exception_handler(
      [](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        std::string msg = "unhandled exception";
        try {
          if (ep) std::rethrow_exception(ep);
        } catch (const std::exception& e) {
          msg = e.what();
        } catch (...) {
        }
        SendError(res, 500, "InternalServerError", msg);
      });

  g_server.store(&svr);
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  std::printf("pp-catalogd: serving IRC on http://%s:%d/v1  (warehouse=%s)\n",
              opts.host.c_str(), opts.port, opts.warehouse.c_str());
  std::fflush(stdout);

  bool ok = svr.listen(opts.host, opts.port);
  g_server.store(nullptr);
  if (!ok) {
    std::fprintf(stderr, "pp-catalogd: failed to bind %s:%d\n", opts.host.c_str(),
                 opts.port);
    return 1;
  }
  return 0;
}

}  // namespace primeparts::catalog
