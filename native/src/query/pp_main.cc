#include <lua.hpp>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/query/lua_query_module.h"
#include "primeparts/query/query_service.h"

namespace {

const char* kDefaultWarehouse = "./data/ib-staging";

void Usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s [--warehouse DIR] [--namespace NS] [run FILE.lua | -e CODE]\n"
               "  no script/-e: interactive REPL.\n"
               "  the `query` module is bound to the warehouse, e.g.\n"
               "    for _,r in ipairs(query.hist{col=\"k\"}) do "
               "print(r.k, r.count) end\n",
               argv0);
}

void ReportLuaError(lua_State* L) {
  const char* msg = lua_tostring(L, -1);
  std::fprintf(stderr, "error: %s\n", msg ? msg : "(unknown)");
  lua_pop(L, 1);
}

int RunRepl(lua_State* L) {
  std::string line;
  std::fprintf(stderr, "pp> ");
  while (std::getline(std::cin, line)) {
    if (line == "\\q" || line == "quit" || line == "exit") break;
    if (!line.empty() && luaL_dostring(L, line.c_str()) != LUA_OK) {
      ReportLuaError(L);
    }
    std::fprintf(stderr, "pp> ");
  }
  std::fprintf(stderr, "\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string warehouse;
  std::string ns_name;
  std::string run_file;
  std::string eval_code;
  bool have_eval = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--warehouse") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--warehouse requires a value\n"); return 2; }
      warehouse = argv[++i];
    } else if (arg == "--namespace") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--namespace requires a value\n"); return 2; }
      ns_name = argv[++i];
    } else if (arg == "-e") {
      if (i + 1 >= argc) { std::fprintf(stderr, "-e requires code\n"); return 2; }
      eval_code = argv[++i];
      have_eval = true;
    } else if (arg == "run") {
      if (i + 1 >= argc) { std::fprintf(stderr, "run requires a file\n"); return 2; }
      run_file = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      Usage(argv[0]);
      return 0;
    } else if (run_file.empty() && !have_eval && arg.size() > 4 &&
               arg.rfind(".lua") == arg.size() - 4) {
      run_file = arg;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      Usage(argv[0]);
      return 2;
    }
  }

  if (warehouse.empty()) {
    if (const char* env = std::getenv("PRIMEPARTS_WAREHOUSE_ROOT"); env && *env) {
      warehouse = env;
    } else {
      warehouse = kDefaultWarehouse;
    }
  }

  lua_State* L = luaL_newstate();
  luaL_openlibs(L);

  std::string e;
  auto qs = primeparts::query::QueryService::Open(
      warehouse, primeparts::catalog::ResolveNamespace(ns_name), &e);
  if (!qs) {
    std::fprintf(stderr,
                 "[i] no catalog at %s (%s) — number-theory functions only\n",
                 warehouse.c_str(), e.c_str());
  }
  primeparts::query::RegisterQueryModule(L, qs ? qs.get() : nullptr);

  int rc = 0;
  if (have_eval) {
    if (luaL_dostring(L, eval_code.c_str()) != LUA_OK) { ReportLuaError(L); rc = 1; }
  } else if (!run_file.empty()) {
    if (luaL_dofile(L, run_file.c_str()) != LUA_OK) { ReportLuaError(L); rc = 1; }
  } else {
    rc = RunRepl(L);
  }

  lua_close(L);
  return rc;
}
