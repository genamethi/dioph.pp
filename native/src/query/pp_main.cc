#include <lua.hpp>
#include <readline/history.h>
#include <readline/readline.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/config.h"
#include "primeparts/query/lua_query_module.h"
#include "primeparts/query/query_service.h"

namespace {

void Usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s [--config PATH] [--warehouse DIR] [--rest-uri URI]\n"
               "          [--namespace NS] [run FILE.lua | -e CODE]\n"
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

std::string HistoryPath() {
  if (const char* explicit_path = std::getenv("PP_HISTORY")) return explicit_path;
  if (const char* state = std::getenv("XDG_STATE_HOME"); state && *state) {
    return std::string(state) + "/pp/history";
  }
  if (const char* home = std::getenv("HOME"); home && *home) {
    return std::string(home) + "/.pp_history";
  }
  return {};
}

bool IsQuit(const std::string& line) {
  return line == "\\q" || line == "quit" || line == "exit";
}

int RunPipedRepl(lua_State* L) {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (IsQuit(line)) break;
    if (!line.empty() && luaL_dostring(L, line.c_str()) != LUA_OK) {
      ReportLuaError(L);
    }
  }
  return 0;
}

int RunRepl(lua_State* L) {
  if (!isatty(STDIN_FILENO)) return RunPipedRepl(L);

  const std::string history = HistoryPath();
  using_history();
  stifle_history(5000);
  if (!history.empty()) read_history(history.c_str());

  for (;;) {
    char* raw = readline("pp> ");
    if (raw == nullptr) break;
    std::string line(raw);
    std::free(raw);
    if (IsQuit(line)) break;
    if (line.empty()) continue;
    add_history(line.c_str());
    if (luaL_dostring(L, line.c_str()) != LUA_OK) ReportLuaError(L);
  }

  if (!history.empty() && write_history(history.c_str()) != 0) {
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(history).parent_path(), ec);
    if (!ec) write_history(history.c_str());
  }
  std::fprintf(stderr, "\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path;
  std::string warehouse;
  std::string rest_uri;
  std::string ns_name;
  std::string run_file;
  std::string eval_code;
  bool have_eval = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--config") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--config requires a value\n"); return 2; }
      config_path = argv[++i];
    } else if (arg == "--rest-uri") {
      if (i + 1 >= argc) { std::fprintf(stderr, "--rest-uri requires a value\n"); return 2; }
      rest_uri = argv[++i];
    } else if (arg == "--warehouse") {
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

  std::string cfg_err;
  primeparts::config::Conf conf;
  if (!primeparts::config::Load(config_path, &conf, &cfg_err)) {
    std::fprintf(stderr, "error: %s\n", cfg_err.c_str());
    return 2;
  }
  primeparts::config::Announce(conf);
  if (warehouse.empty()) warehouse = conf.core.warehouse;
  if (rest_uri.empty()) rest_uri = conf.core.rest_uri;
  if (ns_name.empty()) ns_name = conf.core.ns_name;

  lua_State* L = luaL_newstate();
  luaL_openlibs(L);

  std::string e;
  auto qs = primeparts::query::QueryService::Open(
      warehouse, rest_uri, primeparts::catalog::ResolveNamespace(ns_name), &e);
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
