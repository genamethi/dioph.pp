// primeparts/query/lua_query_module.cc — see header.

#include "primeparts/query/lua_query_module.h"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <lua.hpp>

#include "primeparts/core.h"  // pp_init + number-theory (primecount/primesieve/FLINT)
#include "primeparts/query/query_service.h"

namespace primeparts::query {
namespace {

QueryService* qs_upvalue(lua_State* L) {
  return static_cast<QueryService*>(lua_touserdata(L, lua_upvalueindex(1)));
}

// Read an optional integer field from a spec table (index 1). present=false if
// absent/non-numeric.
int64_t opt_int(lua_State* L, const char* key, int64_t dflt, bool* present) {
  lua_getfield(L, 1, key);
  int64_t v = dflt;
  bool p = false;
  if (lua_isinteger(L, -1)) { v = (int64_t)lua_tointeger(L, -1); p = true; }
  else if (lua_isnumber(L, -1)) { v = (int64_t)lua_tonumber(L, -1); p = true; }
  lua_pop(L, 1);
  if (present) *present = p;
  return v;
}

// Read an optional string field from the spec table (index 1).
std::string opt_str(lua_State* L, const char* key, const char* dflt,
                    bool* present = nullptr) {
  lua_getfield(L, 1, key);
  std::string v = dflt ? dflt : "";
  bool p = false;
  if (lua_isstring(L, -1)) { v = lua_tostring(L, -1); p = true; }
  lua_pop(L, 1);
  if (present) *present = p;
  return v;
}

// --- number theory (no catalog needed) ---
int nt_pi(lua_State* L) { lua_pushinteger(L, pp_prime_pi(luaL_checkinteger(L, 1))); return 1; }
int nt_nth(lua_State* L) { lua_pushinteger(L, pp_nth_prime(luaL_checkinteger(L, 1))); return 1; }
int nt_next(lua_State* L) { lua_pushinteger(L, pp_next_prime(luaL_checkinteger(L, 1))); return 1; }
int nt_prev(lua_State* L) { lua_pushinteger(L, pp_previous_prime(luaL_checkinteger(L, 1))); return 1; }
// pp_is_prime_power_u64 returns PP_OK (0) on success and reports the result via
// *exponent: 1 = prime, >1 = proper prime power, 0 = not a prime power; nonzero
// return = error.
int nt_is_prime(lua_State* L) {
  uint64_t base; int32_t exp = 0;
  int rc = pp_is_prime_power_u64((uint64_t)luaL_checkinteger(L, 1), &base, &exp);
  lua_pushboolean(L, rc == PP_OK && exp == 1);
  return 1;
}
int nt_is_prime_power(lua_State* L) {  // -> base, exp   |   nil
  uint64_t base; int32_t exp = 0;
  int rc = pp_is_prime_power_u64((uint64_t)luaL_checkinteger(L, 1), &base, &exp);
  if (rc != PP_OK || exp <= 0) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushinteger(L, (lua_Integer)base);
  lua_pushinteger(L, exp);
  return 2;
}

// --- data (need the catalog-backed QueryService) ---
// query.pget{ p=, [q_k=], [m_k=], [n_k=] } -> { p, k, prime_rank, partitions } | nil
int q_pget(lua_State* L) {
  QueryService* qs = qs_upvalue(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  bool has_p = false;
  int64_t p = opt_int(L, "p", 0, &has_p);
  if (!has_p) return luaL_error(L, "query.pget requires p");
  if (qs == nullptr) { lua_pushnil(L); return 1; }

  std::string e;
  auto info = qs->LookupPrime(p, &e);
  if (!info) { lua_pushnil(L); return 1; }

  bool fq, fm, fn;
  int64_t qv = opt_int(L, "q_k", 0, &fq);
  int64_t mv = opt_int(L, "m_k", 0, &fm);
  int64_t nv = opt_int(L, "n_k", 0, &fn);
  auto parts = qs->LookupPartitions(p, &e);

  lua_newtable(L);
  lua_pushinteger(L, info->p); lua_setfield(L, -2, "p");
  lua_pushinteger(L, info->k); lua_setfield(L, -2, "k");
  lua_pushinteger(L, info->prime_rank); lua_setfield(L, -2, "prime_rank");
  lua_newtable(L);  // partitions
  int idx = 1;
  for (const auto& t : parts) {
    if (fq && t.q_k != qv) continue;
    if (fm && t.m_k != mv) continue;
    if (fn && t.n_k != nv) continue;
    lua_newtable(L);
    lua_pushinteger(L, t.m_k); lua_setfield(L, -2, "m");
    lua_pushinteger(L, t.n_k); lua_setfield(L, -2, "n");
    lua_pushinteger(L, t.q_k); lua_setfield(L, -2, "q");
    lua_seti(L, -2, idx++);
  }
  lua_setfield(L, -2, "partitions");
  return 1;
}

// query.kget{ k=, [init=], [end=]/[hi=], [limit=] } -> { {p, k, prime_rank}, ... }
int q_kget(lua_State* L) {
  QueryService* qs = qs_upvalue(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  bool has_k = false;
  int64_t k = opt_int(L, "k", 0, &has_k);
  if (!has_k) return luaL_error(L, "query.kget requires k");
  lua_newtable(L);  // result array (empty if qs null)
  if (qs == nullptr) return 1;

  bool dummy;
  int64_t lo = opt_int(L, "init", 0, &dummy);
  int64_t hi = opt_int(L, "end", 0, &dummy);
  if (hi == 0) hi = opt_int(L, "hi", 0, &dummy);
  int64_t lim = opt_int(L, "limit", 10, &dummy);
  if (lim <= 0) lim = 10;

  std::string e;
  auto hits = qs->ScanByK((int32_t)k, lo, hi, lim, &e);
  int idx = 1;
  for (const auto& h : hits) {
    lua_newtable(L);
    lua_pushinteger(L, h.p); lua_setfield(L, -2, "p");
    lua_pushinteger(L, (lua_Integer)k); lua_setfield(L, -2, "k");
    lua_pushinteger(L, h.prime_rank); lua_setfield(L, -2, "prime_rank");
    lua_seti(L, -2, idx++);
  }
  return 1;
}

// query.hist{ col=, [table="primes"], [init=], [end=]/[hi=], [threads=] }
//   -> { { [col]=value, count= }, ... }   (ordered by value)
// General group-by-value count over an integer column — the per-k histogram is
// query.hist{ col = "k" }. Reads QueryService::GroupCount.
int q_hist(lua_State* L) {
  QueryService* qs = qs_upvalue(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  bool has_col = false;
  std::string col = opt_str(L, "col", "", &has_col);
  if (!has_col || col.empty()) return luaL_error(L, "query.hist requires col");
  std::string table = opt_str(L, "table", "primes");

  bool d;
  int64_t lo = opt_int(L, "init", 0, &d);
  int64_t hi = opt_int(L, "end", 0, &d);
  if (hi == 0) hi = opt_int(L, "hi", 0, &d);
  int64_t threads = opt_int(L, "threads", 0, &d);

  lua_newtable(L);  // result array (empty if qs null)
  if (qs == nullptr) return 1;

  std::string e;
  auto rows = qs->GroupCount(table, col, lo, hi, static_cast<int>(threads), &e);
  if (!e.empty()) return luaL_error(L, "query.hist: %s", e.c_str());
  int idx = 1;
  for (const auto& r : rows) {
    lua_newtable(L);
    lua_pushinteger(L, r.value); lua_setfield(L, -2, col.c_str());
    lua_pushinteger(L, r.count); lua_setfield(L, -2, "count");
    lua_seti(L, -2, idx++);
  }
  return 1;
}

}  // namespace

void RegisterQueryModule(lua_State* L, QueryService* qs) {
  static std::once_flag init_once;
  std::call_once(init_once, [] { pp_init(); });  // FLINT/primecount setup

  lua_newtable(L);
  auto reg = [&](const char* name, lua_CFunction fn) {
    lua_pushlightuserdata(L, qs);
    lua_pushcclosure(L, fn, 1);
    lua_setfield(L, -2, name);
  };
  reg("pi", nt_pi);
  reg("nth_prime", nt_nth);
  reg("next_prime", nt_next);
  reg("prev_prime", nt_prev);
  reg("is_prime", nt_is_prime);
  reg("is_prime_power", nt_is_prime_power);
  reg("pget", q_pget);
  reg("kget", q_kget);
  reg("hist", q_hist);
  lua_setglobal(L, "query");
}

}  // namespace primeparts::query
