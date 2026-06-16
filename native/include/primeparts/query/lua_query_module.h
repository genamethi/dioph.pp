// primeparts/query/lua_query_module.h
//
// The reader/catalog `query` Lua module (see markdown/arch/lua_query_api.md).
// Installs a global `query` table into a caller-provided lua_State, bound to a
// QueryService. Defined by the reader; hosted by whoever owns the lua_State
// (the TUI's interactive mode, preset run functions, or a standalone shell).
//
// Number-theory functions (pi/nth_prime/next_prime/prev_prime/is_prime/
// is_prime_power) are thin bindings to primecount/primesieve/FLINT (via core.h)
// and work even when qs is null. Data functions (pget/kget) need qs.

#pragma once

struct lua_State;

namespace primeparts::query {

class QueryService;

void RegisterQueryModule(struct lua_State* L, QueryService* qs);

}  // namespace primeparts::query
