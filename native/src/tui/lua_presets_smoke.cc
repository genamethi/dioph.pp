#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/query/query_service.h"
#include "primeparts/tui/lua_presets.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using primeparts::query::QueryPreset;
using primeparts::query::QueryService;
using primeparts::tui::LuaPresets;

int main(int argc, char** argv) {
  int fail = 0;
  LuaPresets lp;

  QueryPreset p;
  p.id = "t";
  p.desc = "primes where k == {k}";
  p.kind = "by_k";
  p.fields = {{"k", 16}, {"p_lo", 1000000000}, {"p_hi", 2000000000}, {"limit", 5}};
  p.accepts = {"k"};
  p.target = "k";

  auto tmp = std::filesystem::temp_directory_path() / "pp_lua_rt.lua";
  std::filesystem::remove(tmp);
  std::string e;
  if (!LuaPresets::Save(p, tmp, &e)) { std::printf("[!] save: %s\n", e.c_str()); ++fail; }
  std::vector<std::string> errs;
  auto loaded = lp.Load(tmp, &errs);
  if (loaded.size() != 1) {
    std::printf("[!] round-trip expected 1 preset, got %zu\n", loaded.size());
    ++fail;
  } else {
    const auto& q = loaded[0];
    const bool ok = q.id == "t" && q.kind == "by_k" && q.target == "k" &&
                    q.field("k") == 16 && q.field("p_hi") == 2000000000 &&
                    q.accepts.size() == 1 && q.accepts[0] == "k";
    std::printf("[%s] round-trip: id=%s kind=%s k=%lld p_hi=%lld accepts=%zu\n",
                ok ? "ok" : "!!", q.id.c_str(), q.kind.c_str(),
                (long long)q.field("k"), (long long)q.field("p_hi"),
                q.accepts.size());
    if (!ok) ++fail;
  }

  {
    std::vector<std::string> e2;
    auto seed = lp.Load("scripts/lua/queries.lua", &e2);
    const bool ok = seed.size() == 2 && e2.empty();
    std::printf("[%s] seed load: %zu presets, %zu errors\n", ok ? "ok" : "!!",
                seed.size(), e2.size());
    for (auto& m : e2) std::printf("      err: %s\n", m.c_str());
    if (!ok) ++fail;
  }

  {
    const std::string wh =
        argc >= 2 ? argv[1] : "/media/extssd/research/dioph.pp/data/ib-staging";
    std::string oe;
    auto qs = QueryService::Open(wh, primeparts::catalog::ResolveNamespace(""), &oe);
    if (!qs) {
      std::printf("[skip] validation (QueryService::Open: %s)\n", oe.c_str());
    } else {
      std::string ve;
      bool good = qs->ValidatePreset(p, &ve);
      std::printf("[%s] valid preset accepted%s%s\n", good ? "ok" : "!!",
                  good ? "" : ": ", ve.c_str());
      if (!good) ++fail;

      QueryPreset bad_kind = p; bad_kind.kind = "bogus";
      std::string be1;
      bool rej1 = !qs->ValidatePreset(bad_kind, &be1);
      std::printf("[%s] bad kind rejected: %s\n", rej1 ? "ok" : "!!", be1.c_str());
      if (!rej1) ++fail;

      QueryPreset bad_field = p; bad_field.accepts = {"zzz"};
      std::string be2;
      bool rej2 = !qs->ValidatePreset(bad_field, &be2);
      std::printf("[%s] unknown accept field rejected: %s\n", rej2 ? "ok" : "!!",
                  be2.c_str());
      if (!rej2) ++fail;
    }
  }

  {
    std::map<std::string, std::string> kv = {
        {"log_limit", "500"}, {"log_format", "json"}, {"autosave", "true"}};
    auto cfgtmp = std::filesystem::temp_directory_path() / "pp_cfg.lua";
    std::filesystem::remove(cfgtmp);
    std::string ce;
    if (!LuaPresets::SaveConfig(kv, cfgtmp, &ce)) {
      std::printf("[!] SaveConfig: %s\n", ce.c_str()); ++fail;
    }
    std::vector<std::string> ce2;
    auto rk = lp.LoadConfig(cfgtmp, &ce2);
    const bool ok = rk["log_limit"] == "500" && rk["log_format"] == "json" &&
                    rk["autosave"] == "true";
    std::printf("[%s] config round-trip: log_limit=%s log_format=%s autosave=%s\n",
                ok ? "ok" : "!!", rk["log_limit"].c_str(),
                rk["log_format"].c_str(), rk["autosave"].c_str());
    if (!ok) ++fail;
  }

  std::printf("\n== lua-presets-smoke %s (%d failures) ==\n",
              fail == 0 ? "PASS" : "FAIL", fail);
  return fail == 0 ? 0 : 1;
}
