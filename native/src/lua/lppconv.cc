#include "primeparts/lua/lppconv.h"

#include <stdexcept>
#include <utility>

#include "primeparts/query/query_service.h"

namespace primeparts::lua {
namespace {

Connection g_conn;
config::Conf g_conf;

}  // namespace

void SetConf(config::Conf conf) { g_conf = std::move(conf); }

const config::Conf& Conf() { return g_conf; }

void SetConnection(Connection conn) { g_conn = std::move(conn); }

const Connection& Current() { return g_conn; }

query::QueryService& RequireCatalog(const char* fn) {
  if (!g_conn.service) Fail(fn, "no catalog attached; call irc.open first");
  return *g_conn.service;
}

const Connection& RequireConnection(const char* fn) {
  if (g_conn.rest_uri.empty() && g_conn.warehouse.empty()) {
    Fail(fn, "no catalog attached; call irc.open first");
  }
  return g_conn;
}

sol::table Spec(sol::this_state ts, const sol::optional<sol::table>& arg) {
  if (arg) return *arg;
  return sol::state_view(ts).create_table();
}

std::optional<int64_t> OptInt(const sol::table& t, const char* key) {
  if (sol::optional<int64_t> v = t[key]) return *v;
  return std::nullopt;
}

std::optional<bool> OptBool(const sol::table& t, const char* key) {
  if (sol::optional<bool> v = t[key]) return *v;
  return std::nullopt;
}

std::optional<std::string> OptStr(const sol::table& t, const char* key) {
  if (sol::optional<std::string> v = t[key]) return *v;
  return std::nullopt;
}

int64_t IntOr(const sol::table& t, const char* key, int64_t dflt) {
  return OptInt(t, key).value_or(dflt);
}

bool BoolOr(const sol::table& t, const char* key, bool dflt) {
  return OptBool(t, key).value_or(dflt);
}

std::string StrOr(const sol::table& t, const char* key, std::string dflt) {
  return OptStr(t, key).value_or(std::move(dflt));
}

int64_t ReqInt(const sol::table& t, const char* key, const char* fn) {
  if (std::optional<int64_t> v = OptInt(t, key)) return *v;
  Fail(fn, std::string("requires ") + key);
}

std::string ReqStr(const sol::table& t, const char* key, const char* fn) {
  std::optional<std::string> v = OptStr(t, key);
  if (!v || v->empty()) Fail(fn, std::string("requires ") + key);
  return *v;
}

std::vector<std::string> StrArray(const sol::table& t, const char* key) {
  std::vector<std::string> out;
  sol::optional<sol::table> arr = t[key];
  if (!arr) return out;
  for (std::size_t i = 1; i <= arr->size(); ++i) {
    if (sol::optional<std::string> s = (*arr)[i]) out.push_back(*s);
  }
  return out;
}

void Fail(const char* fn, const std::string& msg) {
  throw std::runtime_error(std::string(fn) + ": " + msg);
}

void Unimplemented(const char* fn, const std::string& needs) {
  throw std::runtime_error(std::string(fn) + ": not implemented; needs " + needs);
}

void Check(const std::string& err, const char* fn) {
  if (!err.empty()) Fail(fn, err);
}

}  // namespace primeparts::lua
