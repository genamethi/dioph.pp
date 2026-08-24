#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <lua.h>

int luaopen_nt(lua_State *L);

#ifdef __cplusplus
}

#include <cstdint>
#include <vector>

namespace primeparts::nt {

struct Edge {
  int32_t m = 0;
  int32_t n = 0;
  int64_t q = 0;
  int64_t p = 0;
};

inline constexpr int kMaxEdges = 64;

int PartsOf(uint64_t p, Edge *out);

void InvOf(uint64_t q, uint64_t hi, std::vector<Edge> *out);

}  // namespace primeparts::nt
#endif
