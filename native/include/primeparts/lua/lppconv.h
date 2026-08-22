#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <sol/sol.hpp>

#include "primeparts/config.h"

namespace primeparts::query {

class QueryService;
}

namespace primeparts::lua {

struct Connection {
  std::string warehouse;
  std::string rest_uri;
  std::string ns_name;
  std::string mode;
  std::shared_ptr<query::QueryService> service;
};

void SetConf(config::Conf conf);
const config::Conf& Conf();

void SetConnection(Connection conn);
const Connection& Current();
query::QueryService& RequireCatalog(const char* fn);
const Connection& RequireConnection(const char* fn);

sol::table Spec(sol::this_state ts, const sol::optional<sol::table>& arg);

std::optional<int64_t> OptInt(const sol::table& t, const char* key);
std::optional<bool> OptBool(const sol::table& t, const char* key);
std::optional<std::string> OptStr(const sol::table& t, const char* key);

int64_t IntOr(const sol::table& t, const char* key, int64_t dflt);
bool BoolOr(const sol::table& t, const char* key, bool dflt);
std::string StrOr(const sol::table& t, const char* key, std::string dflt);

int64_t ReqInt(const sol::table& t, const char* key, const char* fn);
std::string ReqStr(const sol::table& t, const char* key, const char* fn);
std::vector<std::string> StrArray(const sol::table& t, const char* key);

std::optional<std::pair<int64_t, int64_t>> OptInterval(const sol::table& t,
                                                       const char* key,
                                                       const char* fn);

[[noreturn]] void Fail(const char* fn, const std::string& msg);
[[noreturn]] void Unimplemented(const char* fn, const std::string& needs);
void Check(const std::string& err, const char* fn);

}  // namespace primeparts::lua
