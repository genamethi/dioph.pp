#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <lua.h>

int luaopen_nt(lua_State *L);

#ifdef __cplusplus
}

#include <cstdint>

namespace primeparts::nt {

struct Part {
  int32_t m;
  int32_t n;
  int64_t q;
};

inline constexpr int kMaxParts = 64;

int PartsOf(uint64_t p, Part *out);

}  // namespace primeparts::nt
#endif
