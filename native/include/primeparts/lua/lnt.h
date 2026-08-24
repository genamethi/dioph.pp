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

struct Part {
  int32_t m;
  int32_t n;
  int64_t q;
};

struct Edge {
  int32_t m;
  int32_t n;
  int64_t p;
};

inline constexpr int kMaxParts = 64;

int PartsOf(uint64_t p, Part *out);

void InvOf(uint64_t q, uint64_t hi, std::vector<Edge> *out);

}  // namespace primeparts::nt
#endif
