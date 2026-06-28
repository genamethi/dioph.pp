// primeparts TUI — F5 Config screen: config rows (+/- and a warehouse-path
// modal), plus the Lua config load/save + log janitor. See tui_app.h.

#include "primeparts/tui/tui_app.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace primeparts::tui {

// --- Config screen ---------------------------------------------------------
// Row kCfgWarehouse is a free-text path (Enter opens a text modal); the rest are
// numeric / toggle rows driven by +/-.
size_t cfg_count() { return 6; }
std::string cfg_name(size_t i) {
  static const char* n[] = {"log limit", "gen threads", "default limit",
                            "log format", "autosave", "warehouse"};
  return i < 6 ? n[i] : "";
}
std::string cfg_value(const App& a, size_t i) {
  switch (i) {
    case 0: return std::to_string(a.cfg.log_limit);
    case 1: return a.cfg.gen_threads == 0 ? "auto" : std::to_string(a.cfg.gen_threads);
    case 2: return std::to_string(a.cfg.default_limit);
    case 3: return a.cfg.log_format;
    case 4: return a.cfg.autosave ? "on" : "off";
    case kCfgWarehouse: return a.warehouse + (a.warehouse_dirty ? " *" : "");
  }
  return "";
}
void cfg_adjust(App* a, size_t i, int d) {
  switch (i) {
    case 0: a->cfg.log_limit = std::max<int64_t>(0, a->cfg.log_limit + d * 50); break;
    case 1: a->cfg.gen_threads = std::max<int64_t>(0, a->cfg.gen_threads + d); break;
    case 2: a->cfg.default_limit = std::max<int64_t>(1, a->cfg.default_limit + d * 5); break;
    case 3: a->cfg.log_format = (a->cfg.log_format == "flat") ? "json" : "flat"; break;
    case 4: a->cfg.autosave = !a->cfg.autosave; break;
    case kCfgWarehouse: break;  // edited via Enter (text modal), not +/-
  }
}

void draw_config(App* a) {
  unsigned iy, ix, irows, icols;
  frame(a->query, "Configuration", true, &iy, &ix, &irows, &icols);
  if (irows == 0) return;
  ncplane_set_fg_rgb8(a->query, 0x77, 0x77, 0x88);
  ncplane_printf_yx(a->query, (int)iy, (int)ix, "%.*s", (int)icols,
                    "+/- change   Enter edit path   s save   F1 back to queries");
  ncplane_set_fg_default(a->query);
  for (size_t i = 0; i < cfg_count() && iy + 2 + i < iy + irows; ++i) {
    char line[96];
    std::snprintf(line, sizeof line, "%-16s %s", cfg_name(i).c_str(),
                  cfg_value(*a, i).c_str());
    row(a->query, (int)(iy + 2 + i), (int)ix, (int)icols, line,
        i == a->cfg_cursor, true, i);
  }
}

// Open the single-box free-text modal bound to the warehouse path.
void open_warehouse_modal(App* a) {
  a->modal_kind = ModalKind::Warehouse;
  a->modal_buf = {a->warehouse};
  a->modal_field = 0;
  a->modal_on = true;
}

// Re-point the warehouse at `path`: try to reopen QueryService; on success swap
// the live service (so F1 queries hit the new warehouse) and adopt the path. On
// failure keep the existing service but still adopt the path for generation —
// timing a from-scratch build means pointing at a dir with no tables to query
// yet. Returns whether queries are now live against `path`.
bool reopen_warehouse(App* a, const std::string& path) {
  a->warehouse = path;
  a->warehouse_dirty = true;
  std::string err;
  auto qs = QueryService::Open(path, &err);
  if (qs) {
    a->qs_owned = std::move(qs);
    a->qs = a->qs_owned.get();
    a->status_glyph = 'k';
    a->status_msg = "warehouse -> " + path + " (queries live)";
    return true;
  }
  a->status_glyph = '!';
  a->status_msg = "warehouse -> " + path + " (generation only; query open failed: " + err + ")";
  return false;
}

// --- Config load / save / janitor ------------------------------------------
void apply_config_kv(App* a, const std::map<std::string, std::string>& kv) {
  auto geti = [&](const char* k, int64_t d) {
    auto it = kv.find(k);
    return it != kv.end() ? std::strtoll(it->second.c_str(), nullptr, 10) : d;
  };
  a->cfg.log_limit = geti("log_limit", a->cfg.log_limit);
  a->cfg.gen_threads = geti("gen_threads", a->cfg.gen_threads);
  a->cfg.default_limit = geti("default_limit", a->cfg.default_limit);
  auto sit = kv.find("log_format");
  if (sit != kv.end() && (sit->second == "json" || sit->second == "flat"))
    a->cfg.log_format = sit->second;
  auto bit = kv.find("autosave");
  if (bit != kv.end()) a->cfg.autosave = (bit->second == "true");
  auto wit = kv.find("warehouse");
  if (wit != kv.end() && !wit->second.empty()) a->warehouse = wit->second;
}

std::map<std::string, std::string> config_to_kv(const Config& c) {
  return {{"log_limit", std::to_string(c.log_limit)},
          {"gen_threads", std::to_string(c.gen_threads)},
          {"default_limit", std::to_string(c.default_limit)},
          {"log_format", c.log_format},
          {"autosave", c.autosave ? "true" : "false"}};
}

// Prune the run-log dir (<config>/logs) to the most-recent log_limit files.
void run_janitor(App* a) {
  std::error_code ec;
  fs::path logs = fs::path(a->config_path).parent_path() / "logs";
  if (!fs::exists(logs, ec)) return;
  std::vector<fs::path> files;
  for (auto& e : fs::directory_iterator(logs, ec))
    if (e.is_regular_file(ec)) files.push_back(e.path());
  if ((int64_t)files.size() <= a->cfg.log_limit) return;
  std::sort(files.begin(), files.end(), [](const fs::path& x, const fs::path& y) {
    std::error_code e1, e2;
    return fs::last_write_time(x, e1) > fs::last_write_time(y, e2);
  });
  for (size_t i = (size_t)a->cfg.log_limit; i < files.size(); ++i)
    fs::remove(files[i], ec);
}

void load_config(App* a) {
  if (!fs::exists(a->config_path)) return;  // keep defaults
  std::vector<std::string> errs;
  apply_config_kv(a, a->lua.LoadConfig(a->config_path, &errs));
}

void save_config(App* a) {
  std::string se;
  auto kv = config_to_kv(a->cfg);
  kv["warehouse"] = a->warehouse;  // persisted alongside the numeric/toggle cfg
  if (LuaPresets::SaveConfig(kv, a->config_path, &se)) {
    a->warehouse_dirty = false;
    a->status_glyph = 'k';
    a->status_msg = "saved config -> " + a->config_path;
    run_janitor(a);
  } else {
    a->status_glyph = '!';
    a->status_msg = "config save failed: " + se;
  }
}

}  // namespace primeparts::tui
