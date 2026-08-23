#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <lua.h>

void pp_openlibs(lua_State *L);

void pp_set_config_path(const char *path);

#ifdef __cplusplus
}
#endif
