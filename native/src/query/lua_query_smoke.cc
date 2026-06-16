// Smoke for the reader `query` Lua module: register it into a lua_State and run
// a Lua script. Number-theory works without a catalog; pget/kget need one.
//
// Usage: lua-query-smoke [warehouse_root]

#include <cstdio>
#include <string>

#include <lua.hpp>

#include "primeparts/query/lua_query_module.h"
#include "primeparts/query/query_service.h"

int main(int argc, char** argv) {
  lua_State* L = luaL_newstate();
  luaL_openlibs(L);

  const std::string wh =
      argc >= 2 ? argv[1] : "/media/extssd/research/dioph.pp/data/ib-staging";
  std::string e;
  auto qs = primeparts::query::QueryService::Open(wh, &e);
  if (!qs) std::fprintf(stderr, "[i] no catalog (%s) — number-theory only\n", e.c_str());
  primeparts::query::RegisterQueryModule(L, qs.get());

  const char* script = R"LUA(
    assert(query.pi(100) == 25, "pi(100) should be 25")
    assert(query.nth_prime(5) == 11, "nth_prime(5) should be 11")
    assert(query.next_prime(10) == 11, "next_prime(10)")
    assert(query.prev_prime(12) == 11, "prev_prime(12)")
    assert(query.is_prime(11) == true, "is_prime(11)")
    assert(query.is_prime(12) == false, "is_prime(12)")
    local b, ex = query.is_prime_power(8)
    assert(b == 2 and ex == 3, "is_prime_power(8) = 2^3")
    print("number-theory OK: pi(100)=25  nth_prime(5)=11  8=2^3")

    local pr = query.pget{ p = 11 }
    if pr then
      assert(pr.k == 3, "pget(11).k == 3")
      assert(#pr.partitions == 3, "pget(11) has 3 partitions")
      print("pget(11): k="..pr.k.."  rank="..pr.prime_rank.."  parts="..#pr.partitions)
      local q3 = query.pget{ p = 11, q_k = 3 }
      print("pget{p=11,q_k=3}: "..#q3.partitions.." filtered partitions")
      local hits = query.kget{ k = 0, limit = 5 }
      assert(#hits == 5, "kget{k=0,limit=5} should be 5")
      print("kget{k=0,limit=5}: first p="..hits[1].p.."  last p="..hits[#hits].p)
    else
      print("pget(11): nil (no catalog) — data functions skipped")
    end
    print("LUA_OK")
  )LUA";

  if (luaL_dostring(L, script) != LUA_OK) {
    std::fprintf(stderr, "[!] lua: %s\n", lua_tostring(L, -1));
    lua_close(L);
    return 1;
  }
  lua_close(L);
  std::printf("\n== lua-query-smoke PASS ==\n");
  return 0;
}
