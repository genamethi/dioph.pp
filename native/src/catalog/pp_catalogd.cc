// primeparts/catalog/pp_catalogd.cc — see header.
//
// Thin IRC HTTP server. Every route follows the same shape:
//   parse request JSON (iceberg-cpp serde) -> call the engine (MakeLocalCatalog)
//   -> serialize the response (iceberg-cpp serde). The engine validates
//   requirements, applies updates, writes metadata.json and CASes the LMDB head
//   pointer; this file owns no metadata logic.
//

#include "primeparts/catalog/pp_catalogd.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "primeparts/catalog/pp_iceberg_rest.h"  // MakeLocalCatalog

#include "iceberg/catalog.h"
#include "iceberg/partition_spec.h"
#include "iceberg/result.h"
#include "iceberg/sort_order.h"
#include "iceberg/table.h"
#include "iceberg/table_identifier.h"
#include "iceberg/table_metadata.h"
#include "iceberg/table_requirement.h"
#include "iceberg/table_update.h"
#include "iceberg/type_fwd.h"

// iceberg-cpp internal serde (compiled into the archives; headers not installed)
#include "iceberg/catalog/rest/json_serde_internal.h"
#include "iceberg/catalog/rest/types.h"
#include "iceberg/json_serde_internal.h"

namespace primeparts::catalog {

namespace {

using json = nlohmann::json;
namespace ir = iceberg::rest;

// ---- response helpers -------------------------------------------------------

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
      return 409;  // optimistic-concurrency / requirement failure
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

// ---- request parsing helpers ------------------------------------------------

// Iceberg multi-level namespaces are %1F-separated in the URL path (httplib
// URL-decodes matched groups, so we split on the unit separator 0x1F).
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

// Parse the request body into a json object; on failure write a 400 and return
// false. We parse with OUR OWN nlohmann (json::parse) rather than the archive's
// iceberg::FromJsonString — see the ABI note on SendTableResult: any
// Result<nlohmann::json> *returned* from the iceberg static archive is miscompiled
// across this TU boundary, so we never consume one. A json passed BY REFERENCE
// into the archive (the *FromJson request parsers below) is fine; only by-value
// Result<json> returns are affected.
bool ParseBody(const httplib::Request& req, httplib::Response& res, json* out) {
  *out = json::parse(req.body, /*cb=*/nullptr, /*allow_exceptions=*/false);
  if (out->is_discarded()) {
    SendError(res, 400, "BadRequest", "request body is not valid JSON");
    return false;
  }
  return true;
}

// Serialize a loaded/created/committed table into the IRC
// LoadTableResult / CommitTableResponse body shape:
//   { "metadata-location": <str>, "metadata": <obj> [, "config": {}] }
//
// ABI WORKAROUND (load-bearing): the iceberg static archive's serde for the
// metadata-bearing IRC models returns Result<nlohmann::json> by value
// (ir::ToJson(LoadTableResult/CommitTableResponse), iceberg::ToJson(TableMetadata)).
// Under the GCC 16 toolchain that builds both the archive and this TU, such a
// std::expected<nlohmann::json, Error> return value is corrupted across the
// archive->server boundary — its `has_value()` discriminant reads garbage, so a
// successful serialization is misread as an error and reporting that bogus error
// dereferences a wild string -> std::bad_alloc. The string-returning serde
// (iceberg::ToJsonString -> Result<std::string>) is NOT affected, so we serialize
// the heavy TableMetadata via that ABI-safe path and assemble the small wrapper
// object with our own nlohmann here. (A minimal std::expected<json,Error> proxy
// does not reproduce, so this is a codegen issue specific to the archive's ToJson;
// revisit if iceberg-cpp / the toolchain is bumped.)
iceberg::Result<std::string> TableResultBody(
    const std::shared_ptr<iceberg::Table>& table, bool with_config) {
  const auto& meta = table->metadata();
  if (!meta) {
    return std::unexpected(
        iceberg::Error{iceberg::ErrorKind::kInvalid, "table metadata is null"});
  }
  auto meta_str = iceberg::ToJsonString(*meta);  // Result<std::string> — ABI-safe
  if (!meta_str.has_value()) return std::unexpected(meta_str.error());
  json body;
  body["metadata-location"] = std::string(table->metadata_file_location());
  body["metadata"] = json::parse(meta_str.value());
  if (with_config) body["config"] = json::object();
  return body.dump();
}

// Send a LoadTableResult/CommitTableResponse for `table`, or a 500 if the
// (ABI-safe) metadata serialization itself fails.
void SendTableResult(httplib::Response& res, int status,
                     const std::shared_ptr<iceberg::Table>& table,
                     bool with_config) {
  auto body = TableResultBody(table, with_config);
  if (!body.has_value()) return SendIcebergError(res, body.error());
  res.status = status;
  res.set_content(body.value(), "application/json");
}

// ---- the server -------------------------------------------------------------

std::atomic<httplib::Server*> g_server{nullptr};

void HandleSignal(int) {
  if (auto* s = g_server.load()) s->stop();
}

}  // namespace

int RunCatalogd(const CatalogdOptions& opts) {
  std::string err;
  auto catalog = MakeLocalCatalog(opts.warehouse, &err);
  if (!catalog) {
    std::fprintf(stderr, "pp-catalogd: open catalog: %s\n", err.c_str());
    return 1;
  }

  httplib::Server svr;

  // GET /v1/config — client handshake. We advertise no defaults/overrides; the
  // client supplies its own warehouse.
  svr.Get("/v1/config", [](const httplib::Request&, httplib::Response& res) {
    SendJson(res, 200, json{{"defaults", json::object()},
                            {"overrides", json::object()}});
  });

  // GET /v1/namespaces — list (optional ?parent=).
  svr.Get("/v1/namespaces", [catalog](const httplib::Request& req,
                                      httplib::Response& res) {
    iceberg::Namespace parent;
    if (req.has_param("parent")) parent = ParseNamespace(req.get_param_value("parent"));
    auto r = catalog->ListNamespaces(parent);
    if (!r.has_value()) return SendIcebergError(res, r.error());
    ir::ListNamespacesResponse body{.namespaces = std::move(r.value())};
    SendJson(res, 200, ir::ToJson(body));
  });

  // POST /v1/namespaces — create.
  svr.Post("/v1/namespaces", [catalog](const httplib::Request& req,
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

  // GET /v1/namespaces/{ns} — load properties.
  svr.Get(R"(/v1/namespaces/([^/]+))", [catalog](const httplib::Request& req,
                                                 httplib::Response& res) {
    auto ns = ParseNamespace(req.matches[1]);
    auto r = catalog->GetNamespaceProperties(ns);
    if (!r.has_value()) return SendIcebergError(res, r.error());
    ir::GetNamespaceResponse resp{.namespace_ = ns, .properties = std::move(r.value())};
    SendJson(res, 200, ir::ToJson(resp));
  });

  // (HEAD /v1/namespaces/{ns} — exists — is served by the GET handler above:
  // cpp-httplib dispatches HEAD to the GET handlers, so a load that succeeds
  // returns 200 and a missing namespace returns 404, which is the exists
  // semantics the IRC client wants.)

  // DELETE /v1/namespaces/{ns} — drop.
  svr.Delete(R"(/v1/namespaces/([^/]+))", [catalog](const httplib::Request& req,
                                                    httplib::Response& res) {
    auto st = catalog->DropNamespace(ParseNamespace(req.matches[1]));
    if (!st.has_value()) return SendIcebergError(res, st.error());
    res.status = 204;
  });

  // POST /v1/namespaces/{ns}/properties — update properties.
  svr.Post(R"(/v1/namespaces/([^/]+)/properties)",
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
             // Engine returns void; report all requested changes as applied.
             ir::UpdateNamespacePropertiesResponse resp;
             for (auto& [k, _] : ur.updates) resp.updated.push_back(k);
             resp.removed = ur.removals;
             SendJson(res, 200, ir::ToJson(resp));
           });

  // GET /v1/namespaces/{ns}/tables — list.
  svr.Get(R"(/v1/namespaces/([^/]+)/tables)",
          [catalog](const httplib::Request& req, httplib::Response& res) {
            auto r = catalog->ListTables(ParseNamespace(req.matches[1]));
            if (!r.has_value()) return SendIcebergError(res, r.error());
            ir::ListTablesResponse body{.identifiers = std::move(r.value())};
            SendJson(res, 200, ir::ToJson(body));
          });

  // POST /v1/namespaces/{ns}/tables — create.
  svr.Post(R"(/v1/namespaces/([^/]+)/tables)",
           [catalog](const httplib::Request& req, httplib::Response& res) {
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
             auto order = cr.write_order ? cr.write_order : iceberg::SortOrder::Unsorted();
             auto r = catalog->CreateTable(id, cr.schema, spec, order, cr.location,
                                           cr.properties);
             if (!r.has_value()) return SendIcebergError(res, r.error());
             SendTableResult(res, 200, r.value(), /*with_config=*/true);
           });

  // POST /v1/namespaces/{ns}/register — register an existing metadata.json.
  svr.Post(R"(/v1/namespaces/([^/]+)/register)",
           [catalog](const httplib::Request& req, httplib::Response& res) {
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
             SendTableResult(res, 200, r.value(), /*with_config=*/true);
           });

  // GET /v1/namespaces/{ns}/tables/{table} — load.
  svr.Get(R"(/v1/namespaces/([^/]+)/tables/([^/]+))",
          [catalog](const httplib::Request& req, httplib::Response& res) {
            iceberg::TableIdentifier id{.ns = ParseNamespace(req.matches[1]),
                                        .name = req.matches[2]};
            auto r = catalog->LoadTable(id);
            if (!r.has_value()) return SendIcebergError(res, r.error());
            SendTableResult(res, 200, r.value(), /*with_config=*/true);
          });

  // (HEAD /v1/namespaces/{ns}/tables/{table} — exists — is served by the GET
  // load handler above via cpp-httplib's HEAD->GET dispatch: 200 if it loads,
  // 404 if absent.)

  // POST /v1/namespaces/{ns}/tables/{table} — COMMIT (updateTable). The
  // load-bearing route: native FastAppend / RowDelta commits arrive here as
  // {requirements, updates}. Deletion vectors are forward-compatible — the
  // delete-file / Puffin specifics ride inside add-snapshot updates, which
  // TableUpdateFromJson + the engine handle without any change to this layer.
  svr.Post(R"(/v1/namespaces/([^/]+)/tables/([^/]+))",
           [catalog](const httplib::Request& req, httplib::Response& res) {
             json body;
             if (!ParseBody(req, res, &body)) return;
             iceberg::TableIdentifier id{.ns = ParseNamespace(req.matches[1]),
                                         .name = req.matches[2]};
             std::vector<std::unique_ptr<iceberg::TableRequirement>> requirements;
             std::vector<std::unique_ptr<iceberg::TableUpdate>> updates;
             if (body.contains("requirements")) {
               for (const auto& jr : body.at("requirements")) {
                 auto r = iceberg::TableRequirementFromJson(jr);
                 if (!r.has_value())
                   return SendError(res, 400, "BadRequest", r.error().message);
                 requirements.push_back(std::move(r.value()));
               }
             }
             if (body.contains("updates")) {
               for (const auto& ju : body.at("updates")) {
                 auto u = iceberg::TableUpdateFromJson(ju);
                 if (!u.has_value())
                   return SendError(res, 400, "BadRequest", u.error().message);
                 updates.push_back(std::move(u.value()));
               }
             }
             auto r = catalog->UpdateTable(id, requirements, updates);
             if (!r.has_value()) return SendIcebergError(res, r.error());
             // CommitTableResponse shape == {metadata-location, metadata} (no config).
             SendTableResult(res, 200, r.value(), /*with_config=*/false);
           });

  // DELETE /v1/namespaces/{ns}/tables/{table} — drop (?purgeRequested=).
  svr.Delete(R"(/v1/namespaces/([^/]+)/tables/([^/]+))",
             [catalog](const httplib::Request& req, httplib::Response& res) {
               iceberg::TableIdentifier id{.ns = ParseNamespace(req.matches[1]),
                                           .name = req.matches[2]};
               bool purge = req.has_param("purgeRequested") &&
                            req.get_param_value("purgeRequested") == "true";
               auto st = catalog->DropTable(id, purge);
               if (!st.has_value()) return SendIcebergError(res, st.error());
               res.status = 204;
             });

  // POST /v1/tables/rename.
  svr.Post("/v1/tables/rename", [catalog](const httplib::Request& req,
                                          httplib::Response& res) {
    json body;
    if (!ParseBody(req, res, &body)) return;
    auto parsed = ir::RenameTableRequestFromJson(body);
    if (!parsed.has_value()) return SendError(res, 400, "BadRequest", parsed.error().message);
    auto& rr = parsed.value();
    auto st = catalog->RenameTable(rr.source, rr.destination);
    if (!st.has_value()) return SendIcebergError(res, st.error());
    res.status = 200;
  });

  // POST /v1/namespaces/{ns}/tables/{table}/metrics — report scan metrics. We
  // accept and discard (the spec allows a 204).
  svr.Post(R"(/v1/namespaces/([^/]+)/tables/([^/]+)/metrics)",
           [](const httplib::Request&, httplib::Response& res) { res.status = 204; });

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
