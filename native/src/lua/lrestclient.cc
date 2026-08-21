#include "primeparts/lua/lrestclient.h"

#include <memory>
#include <string>
#include <utility>

#include <sol/sol.hpp>

#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/config.h"
#include "primeparts/lua/lppconv.h"
#include "primeparts/query/query_service.h"

namespace {

using primeparts::lua::Conf;
using primeparts::lua::Connection;
using primeparts::lua::Current;
using primeparts::lua::Fail;
using primeparts::lua::RequireCatalog;
using primeparts::lua::RequireConnection;
using primeparts::lua::Spec;
using primeparts::lua::StrOr;
using primeparts::lua::Unimplemented;

sol::table Describe(sol::state_view lua, const Connection& conn) {
  sol::table out = lua.create_table();
  out["warehouse"] = conn.warehouse;
  out["uri"] = conn.rest_uri;
  out["namespace"] = conn.ns_name;
  out["mode"] = conn.mode;
  out["attached"] = conn.service != nullptr;
  return out;
}

sol::table Open(sol::this_state ts, sol::optional<sol::table> arg) {
  static const char kFn[] = "irc.open";
  sol::state_view lua(ts);
  const sol::table spec = Spec(ts, arg);
  const primeparts::config::Core& core = Conf().core;

  Connection conn;
  conn.warehouse = StrOr(spec, "warehouse", core.warehouse);
  conn.rest_uri = StrOr(spec, "uri", core.rest_uri);
  conn.ns_name = StrOr(spec, "namespace", core.ns_name);

  std::string error;
  std::unique_ptr<primeparts::query::QueryService> qs =
      primeparts::query::QueryService::Open(
          conn.warehouse, conn.rest_uri,
          primeparts::catalog::ResolveNamespace(conn.ns_name), &error);
  if (!qs) Fail(kFn, error.empty() ? "cannot open catalog" : error);

  conn.mode = conn.rest_uri.empty() ? "local" : "rest";
  conn.service = std::shared_ptr<primeparts::query::QueryService>(std::move(qs));
  primeparts::lua::SetConnection(conn);
  return Describe(lua, Current());
}

sol::table Info(sol::this_state ts) {
  return Describe(sol::state_view(ts), Current());
}

void Close() { primeparts::lua::SetConnection(Connection{}); }

bool Reachable(sol::optional<std::string> uri) {
  const std::string target = uri ? *uri : Current().rest_uri;
  return primeparts::catalog::RestServerReachable(target);
}

std::string Warehouse(sol::optional<std::string> uri) {
  const std::string target = uri ? *uri : Current().rest_uri;
  return primeparts::catalog::AdvertisedWarehouse(target);
}

std::string NamespacePath(sol::optional<std::string> name) {
  const std::string target = name ? *name : Current().ns_name;
  return primeparts::catalog::NamespaceUrlPath(
      primeparts::catalog::ResolveNamespace(target));
}

sol::table Tables(sol::this_state ts) {
  static const char kFn[] = "irc.tables";
  std::string error;
  const std::vector<std::string> names = RequireCatalog(kFn).ListTables(&error);
  primeparts::lua::Check(error, kFn);

  sol::state_view lua(ts);
  sol::table out = lua.create_table(static_cast<int>(names.size()), 0);
  int idx = 1;
  for (const std::string& name : names) out[idx++] = name;
  return out;
}

sol::variadic_results Bound(sol::this_state ts, sol::optional<sol::table> arg) {
  static const char kFn[] = "irc.bound";
  sol::state_view lua(ts);
  const sol::table spec = Spec(ts, arg);
  const Connection& conn = RequireConnection(kFn);
  const std::string table = primeparts::lua::ReqStr(spec, "table", kFn);
  const std::string field = primeparts::lua::ReqStr(spec, "field", kFn);

  int64_t value = 0;
  primeparts::catalog::FieldBoundState state{};
  std::string error;
  const bool ok = primeparts::catalog::FetchFieldBound(
      conn.rest_uri, primeparts::catalog::ResolveNamespace(conn.ns_name), table,
      field, &value, &state, &error);
  primeparts::lua::Check(error, kFn);

  sol::variadic_results out;
  const std::string_view name = primeparts::catalog::FieldBoundStateName(state);
  if (!ok || state != primeparts::catalog::FieldBoundState::kPresent) {
    out.push_back({lua, sol::in_place, sol::lua_nil});
  } else {
    out.push_back({lua, sol::in_place, value});
  }
  out.push_back({lua, sol::in_place, std::string(name)});
  return out;
}

sol::variadic_results UpperBound(sol::this_state ts,
                                 sol::optional<sol::table> arg) {
  static const char kFn[] = "irc.upper_bound";
  sol::state_view lua(ts);
  const sol::table spec = Spec(ts, arg);
  const Connection& conn = RequireConnection(kFn);
  const std::string table = primeparts::lua::ReqStr(spec, "table", kFn);
  const std::string field = primeparts::lua::ReqStr(spec, "field", kFn);

  int64_t value = 0;
  bool present = false;
  std::string error;
  const bool ok = primeparts::catalog::FetchFieldUpperBound(
      conn.rest_uri, primeparts::catalog::ResolveNamespace(conn.ns_name), table,
      field, &value, &present, &error);
  primeparts::lua::Check(error, kFn);

  sol::variadic_results out;
  if (!ok || !present) {
    out.push_back({lua, sol::in_place, sol::lua_nil});
  } else {
    out.push_back({lua, sol::in_place, value});
  }
  out.push_back({lua, sol::in_place, present});
  return out;
}

sol::table MetadataPath(sol::this_state, sol::optional<sol::table>) {
  Unimplemented("irc.metadata_path",
                "an iceberg::Catalog handle; QueryService owns it privately");
}

sol::table EnsureNamespace(sol::this_state, sol::optional<sol::table>) {
  Unimplemented("irc.ensure_namespace",
                "an iceberg::Catalog handle; QueryService owns it privately");
}

sol::table EnsureTable(sol::this_state, sol::optional<sol::table>) {
  Unimplemented("irc.ensure_table",
                "an iceberg::Schema and PartitionSpec bound from Lua");
}

sol::table MoveStaged(sol::this_state, sol::optional<sol::table>) {
  Unimplemented("irc.move_staged",
                "an iceberg::DataFile list bound from Lua");
}

}  // namespace

extern "C" int luaopen_irc(lua_State *L) {
  sol::state_view lua(L);
  sol::table irc = lua.create_table();
  irc.set_function("open", &Open);
  irc.set_function("info", &Info);
  irc.set_function("close", &Close);
  irc.set_function("reachable", &Reachable);
  irc.set_function("warehouse", &Warehouse);
  irc.set_function("namespace_path", &NamespacePath);
  irc.set_function("tables", &Tables);
  irc.set_function("bound", &Bound);
  irc.set_function("upper_bound", &UpperBound);
  irc.set_function("metadata_path", &MetadataPath);
  irc.set_function("ensure_namespace", &EnsureNamespace);
  irc.set_function("ensure_table", &EnsureTable);
  irc.set_function("move_staged", &MoveStaged);
  irc.push();
  return 1;
}
