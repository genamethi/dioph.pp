// Smoke for the embedded-Lua preset facility: round-trip (serialize -> load),
// seed-file load, and reader-side validation (good accepted, bad rejected).
//
// Usage: lua-presets-smoke [warehouse_root]   (run from the repo root so the
// seed scripts/lua/queries.lua is found).

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

  // 1) round-trip: serialize -> temp file -> load -> compare.
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

  // 2) load the seed file (run from repo root).
  {
    std::vector<std::string> e2;
    auto seed = lp.Load("scripts/lua/queries.lua", &e2);
    const bool ok = seed.size() == 2 && e2.empty();
    std::printf("[%s] seed load: %zu presets, %zu errors\n", ok ? "ok" : "!!",
                seed.size(), e2.size());
    for (auto& m : e2) std::printf("      err: %s\n", m.c_str());
    if (!ok) ++fail;
  }

  // 3) reader-side validation against the catalog schema.
  {
    const std::string wh =
        argc >= 2 ? argv[1] : "/media/extssd/research/dioph.pp/data/ib-staging";
    std::string oe;
    auto qs = QueryService::Open(wh, &oe);
    if (!qs) {
      std::printf("[skip] validation (QueryService::Open: %s)\n", oe.c_str());
    } else {
      std::string ve;
      bool good = qs->ValidatePreset(p, &ve);  // accepts k, target k -> valid
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

  std::printf("\n== lua-presets-smoke %s (%d failures) ==\n",
              fail == 0 ? "PASS" : "FAIL", fail);
  return fail == 0 ? 0 : 1;
}
