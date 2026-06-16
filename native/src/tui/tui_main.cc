// primeparts TUI — first pass (v3: option-row query panel, correct key model).
//
// Multi-screen app design: markdown/arch/tui_app_design.md. This pass is the Run
// Saved Query screen only (F1). C++ calling QueryService directly.
//
// KEY MODEL (per the user, do not re-derive — see feedback_dont_assume):
//   * arrows / hjkl  = NAVIGATE (move the cursor among option rows; scroll
//                      results). +/- is NEVER navigation.
//   * + / -          = cycle the VALUE of the option the cursor is on, shown to
//                      the right of the option name (e.g. the Preset option
//                      cycles by-k <-> lookup). Numeric fields edit via a modal
//                      (NOT YET — next pass).
//   * Tab            = switch the query / results panel focus.
//   * Enter          = reserved for "change view / detailed options". The RUN
//                      trigger is UNSPECIFIED; Enter currently runs ONLY as a
//                      flagged placeholder pending the user's choice of key.
//   * q              = quit (y/N).
//
// NOT YET: field-edit modal; threaded+cancellable+progress queries; `c`
// field-name dispatch; F2..F5 screens; Ctrl+L log; Lua presets.

#include <notcurses/notcurses.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>  // readlink (binary-relative seed path)

#include "primeparts/query/query_service.h"
#include "primeparts/tui/lua_presets.h"

using primeparts::query::PartitionTuple;
using primeparts::query::PrimeInfo;
using primeparts::query::QueryService;
using primeparts::query::ScanHit;

namespace {

namespace fs = std::filesystem;

constexpr char kDefaultWarehouse[] =
    "/media/extssd/research/dioph.pp/data/ib-staging";

// Where saved presets live: an actual config dir, not CWD. XDG_CONFIG_HOME (or
// ~/.config) / primeparts / queries.lua.
fs::path config_presets_path() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg) return fs::path(xdg) / "primeparts" / "queries.lua";
  const char* home = std::getenv("HOME");
  if (home && *home) return fs::path(home) / ".config" / "primeparts" / "queries.lua";
  return fs::path(".primeparts-queries.lua");
}

fs::path config_file_path() {  // <config>/config.lua, alongside queries.lua
  return config_presets_path().parent_path() / "config.lua";
}

// The shipped seed presets, found relative to the binary
// (<bindir>/../../scripts/lua/queries.lua) — read on first run before any save.
fs::path binary_seed_path() {
  char buf[4096];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return {};
  buf[n] = '\0';
  std::error_code ec;
  fs::path seed = fs::path(buf).parent_path() / ".." / ".." / "scripts" / "lua" /
                  "queries.lua";
  fs::path c = fs::weakly_canonical(seed, ec);
  return ec ? seed : c;
}

// Preset/Field are the shared QueryPreset/QueryField — parsed from Lua presets
// (scripts/lua/queries.lua) and validated by the reader. kind is a string:
// "by_k" (QueryService::ScanByK) | "lookup" (LookupPrime + partitions).
using Field = primeparts::query::QueryField;
using Preset = primeparts::query::QueryPreset;
using primeparts::tui::LuaPresets;

// Top-level screens (F1..F5). Only RunQuery + Config are real in this pass.
enum class Screen { RunQuery, MakeQuery, Status, Generate, Config };

// App configuration (persisted as a Lua table at the config path).
struct Config {
  int64_t log_limit = 200;          // run-log entries kept (janitor target)
  int64_t gen_threads = 0;          // 0 = auto (hw concurrency)
  int64_t default_limit = 10;       // default query LIMIT for new runs
  std::string log_format = "flat";  // "flat" | "json"
  bool autosave = false;            // rewrite presets file on every edit
};

// A rendered result row that also carries the schema field name + value it
// represents, so `c` can dispatch the value into a compatible query.
struct ResultRow {
  std::string text;
  std::string field;   // schema field name ("" = not dispatchable, e.g. header)
  int64_t value = 0;
};

enum class Focus { Query, Results };

struct App {
  struct notcurses* nc = nullptr;
  struct ncplane* topbar = nullptr;
  struct ncplane* query = nullptr;
  struct ncplane* results = nullptr;
  struct ncplane* status = nullptr;

  QueryService* qs = nullptr;
  LuaPresets lua;                              // owns the lua_State
  std::string presets_path = "scripts/lua/queries.lua";
  std::string config_path;                     // <config>/config.lua
  std::vector<Preset> presets;
  size_t preset_idx = 0;  // value of the "Preset" option
  size_t cursor = 0;      // cursored option row: 0 = Preset, 1.. = fields
  Focus focus = Focus::Query;

  Screen screen = Screen::RunQuery;
  Config cfg;
  size_t cfg_cursor = 0;  // cursored config option row

  std::vector<ResultRow> result_rows;
  size_t res_top = 0, res_sel = 0;

  // c-dispatch dialogue (reuses the modal overlay plane).
  bool disp_on = false;
  std::vector<size_t> disp_cands;  // candidate preset indices
  size_t disp_sel = 0;
  std::string disp_field;
  int64_t disp_value = 0;

  std::string status_msg = "ready";
  char status_glyph = 'i';
  bool confirm_quit = false;

  // Field-edit modal: one text box per variable field of the current preset.
  struct ncplane* modal = nullptr;
  bool modal_on = false;
  size_t modal_field = 0;
  std::vector<std::string> modal_buf;

  // Async query execution: the query runs on a worker thread with cooperative
  // cancel + progress; the UI stays responsive (polls input, draws progress).
  std::thread worker;
  std::atomic<bool> q_running{false};
  std::atomic<bool> q_done{false};
  std::atomic<bool> q_cancel{false};
  std::atomic<int64_t> q_scanned{0};
  std::atomic<int64_t> q_total{0};
  std::vector<ResultRow> pending_results;    // worker -> UI; read only post q_done
  std::string worker_status;                 // ditto
  char worker_glyph = 'i';                    // ditto
};

void draw_modal(App* a);
void draw_dispatch(App* a);
void start_query(App* a);

double secs_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
int64_t fval(const Preset& p, const char* name, int64_t dflt = 0) {
  for (const auto& f : p.fields)
    if (f.name == name) return f.value;
  return dflt;
}
// option rows in the query panel = 1 (Preset) + fields.
size_t option_count(const App& a) {
  return 1 + a.presets[a.preset_idx].fields.size();
}

// One full-width list row inside a bordered pane: zebra bg, or inverted+caret
// when it is the focused selection.
void row(struct ncplane* pl, int y, int x0, int w, const std::string& text,
         bool selected, bool focused, size_t idx) {
  if (selected && focused) {
    ncplane_set_styles(pl, NCSTYLE_BOLD);
    ncplane_set_bg_rgb8(pl, 0x2c, 0x44, 0x66);
    ncplane_set_fg_rgb8(pl, 0xff, 0xff, 0xff);
  } else if (idx % 2 == 0) {
    ncplane_set_bg_rgb8(pl, 0x1c, 0x1c, 0x22);
  }
  ncplane_printf_yx(pl, y, x0, "%s%-*s", selected ? "> " : "  ", w - 2,
                    text.c_str());
  ncplane_set_styles(pl, NCSTYLE_NONE);
  ncplane_set_bg_default(pl);
  ncplane_set_fg_default(pl);
}

void frame(struct ncplane* pl, const char* title, bool focused, unsigned* iy,
           unsigned* ix, unsigned* irows, unsigned* icols) {
  ncplane_erase(pl);
  unsigned rows, cols;
  ncplane_dim_yx(pl, &rows, &cols);
  ncplane_perimeter_rounded(pl, focused ? NCSTYLE_BOLD : NCSTYLE_NONE, 0, 0);
  if (focused) ncplane_set_styles(pl, NCSTYLE_BOLD);
  ncplane_printf_yx(pl, 0, 2, focused ? "┤ %s ├" : "  %s  ", title);
  ncplane_set_styles(pl, NCSTYLE_NONE);
  *iy = 1; *ix = 2;
  *irows = rows >= 2 ? rows - 2 : 0;
  *icols = cols >= 4 ? cols - 4 : 0;
}

int layout(App* a) {
  unsigned rows, cols;
  notcurses_term_dim_yx(a->nc, &rows, &cols);
  if (rows < 12 || cols < 48) return -1;
  ncplane_resize_simple(a->topbar, 1, cols);
  ncplane_move_yx(a->topbar, 0, 0);
  const unsigned body = rows - 2;
  if (a->screen == Screen::RunQuery) {
    unsigned qh = body * 3 / 10;
    if (qh < 6) qh = 6;
    ncplane_resize_simple(a->query, qh, cols);
    ncplane_move_yx(a->query, 1, 0);
    ncplane_resize_simple(a->results, body - qh, cols);
    ncplane_move_yx(a->results, 1 + qh, 0);
  } else {  // other screens use the whole body in `query`; results is hidden
    ncplane_resize_simple(a->query, body, cols);
    ncplane_move_yx(a->query, 1, 0);
    ncplane_resize_simple(a->results, 1, cols);
    ncplane_move_yx(a->results, rows - 1, 0);  // under the status row (covered)
  }
  ncplane_resize_simple(a->status, 1, cols);
  ncplane_move_yx(a->status, rows - 1, 0);
  return 0;
}

void draw_topbar(App* a) {
  ncplane_erase(a->topbar);
  unsigned rows, cols; ncplane_dim_yx(a->topbar, &rows, &cols); (void)rows;
  ncplane_set_bg_rgb8(a->topbar, 0x22, 0x22, 0x2c);
  ncplane_printf_yx(a->topbar, 0, 0, "%*s", (int)cols, "");
  const char* labels[] = {"F1 Run Query", "F2 Make", "F3 Status",
                          "F4 Generate", "F5 Config"};
  const int active = static_cast<int>(a->screen);  // enum order matches labels
  int x = 1;
  for (int ti = 0; ti < 5; ++ti) {
    const char* label = labels[ti];
    if (ti == active) {
      ncplane_set_styles(a->topbar, NCSTYLE_BOLD);
      ncplane_set_bg_rgb8(a->topbar, 0x3a, 0x55, 0x88);
      ncplane_set_fg_rgb8(a->topbar, 0xff, 0xff, 0xff);
    } else {
      ncplane_set_styles(a->topbar, NCSTYLE_NONE);
      ncplane_set_bg_rgb8(a->topbar, 0x22, 0x22, 0x2c);
      ncplane_set_fg_rgb8(a->topbar, 0x99, 0x99, 0xaa);
    }
    ncplane_printf_yx(a->topbar, 0, x, " %s ", label);
    x += (int)std::strlen(label) + 3;
  }
  ncplane_set_styles(a->topbar, NCSTYLE_NONE);
  ncplane_set_bg_default(a->topbar);
  ncplane_set_fg_default(a->topbar);
}

// value string for option row `oi` (0 = Preset, 1.. = field).
std::string option_value(const App& a, size_t oi) {
  const Preset& p = a.presets[a.preset_idx];
  if (oi == 0) return p.id;
  const Field& f = p.fields[oi - 1];
  if ((f.name == "p_lo" || f.name == "p_hi") && f.value <= 0) return "any";
  return std::to_string(f.value);
}
std::string option_name(const App& a, size_t oi) {
  return oi == 0 ? "Preset" : a.presets[a.preset_idx].fields[oi - 1].name;
}

void draw_query(App* a) {
  const bool foc = a->focus == Focus::Query;
  unsigned iy, ix, irows, icols;
  frame(a->query, "Query", foc, &iy, &ix, &irows, &icols);
  if (irows == 0) return;

  // Description of the selected preset (dim), then the option rows.
  ncplane_set_fg_rgb8(a->query, 0x77, 0x77, 0x88);
  ncplane_printf_yx(a->query, (int)iy, (int)ix, "%.*s", (int)icols,
                    a->presets[a->preset_idx].desc.c_str());
  ncplane_set_fg_default(a->query);

  const size_t nopt = option_count(*a);
  for (size_t oi = 0; oi < nopt && iy + 1 + oi < iy + irows; ++oi) {
    char line[128];
    std::snprintf(line, sizeof line, "%-10s %s", option_name(*a, oi).c_str(),
                  option_value(*a, oi).c_str());
    row(a->query, (int)(iy + 1 + oi), (int)ix, (int)icols, line,
        oi == a->cursor, foc, oi);
  }
}

void draw_results(App* a) {
  const bool foc = a->focus == Focus::Results;
  char title[48];
  std::snprintf(title, sizeof title, "Results (%zu)",
                a->result_rows.empty() ? 0 : a->result_rows.size() - 1);
  unsigned iy, ix, irows, icols;
  frame(a->results, title, foc, &iy, &ix, &irows, &icols);
  if (irows == 0) return;

  if (a->result_rows.empty()) {
    ncplane_set_fg_rgb8(a->results, 0x77, 0x77, 0x77);
    ncplane_printf_yx(a->results, (int)iy, (int)ix, "(no results yet)");
    ncplane_set_fg_default(a->results);
    return;
  }
  ncplane_set_styles(a->results, NCSTYLE_BOLD);
  ncplane_set_fg_rgb8(a->results, 0x88, 0xcc, 0xcc);
  ncplane_printf_yx(a->results, (int)iy, (int)ix, "%-*s", (int)icols,
                    a->result_rows[0].text.c_str());
  ncplane_set_styles(a->results, NCSTYLE_NONE);
  ncplane_set_fg_default(a->results);

  const unsigned list_rows = irows - 1;
  const size_t n = a->result_rows.size();
  if (a->res_sel < 1 && n > 1) a->res_sel = 1;
  if (a->res_sel < a->res_top) a->res_top = a->res_sel;
  else if (list_rows && a->res_sel >= a->res_top + list_rows)
    a->res_top = a->res_sel - list_rows + 1;
  if (a->res_top < 1) a->res_top = 1;

  for (unsigned r = 0; r < list_rows && a->res_top + r < n; ++r) {
    const size_t idx = a->res_top + r;
    row(a->results, (int)(iy + 1 + r), (int)ix, (int)icols,
        a->result_rows[idx].text, idx == a->res_sel, foc, idx);
  }
  unsigned prows, pcols; ncplane_dim_yx(a->results, &prows, &pcols);
  if (a->res_top > 1) ncplane_putstr_yx(a->results, 1, pcols - 1, "▲");
  if (a->res_top + list_rows < n)
    ncplane_putstr_yx(a->results, prows - 2, pcols - 1, "▼");
}

// --- Config screen ---------------------------------------------------------
size_t cfg_count() { return 5; }
std::string cfg_name(size_t i) {
  static const char* n[] = {"log limit", "gen threads", "default limit",
                            "log format", "autosave"};
  return i < 5 ? n[i] : "";
}
std::string cfg_value(const App& a, size_t i) {
  switch (i) {
    case 0: return std::to_string(a.cfg.log_limit);
    case 1: return a.cfg.gen_threads == 0 ? "auto" : std::to_string(a.cfg.gen_threads);
    case 2: return std::to_string(a.cfg.default_limit);
    case 3: return a.cfg.log_format;
    case 4: return a.cfg.autosave ? "on" : "off";
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
  }
}

void draw_config(App* a) {
  unsigned iy, ix, irows, icols;
  frame(a->query, "Configuration", true, &iy, &ix, &irows, &icols);
  if (irows == 0) return;
  ncplane_set_fg_rgb8(a->query, 0x77, 0x77, 0x88);
  ncplane_printf_yx(a->query, (int)iy, (int)ix, "%.*s", (int)icols,
                    "+/- change   s save   F1 back to queries");
  ncplane_set_fg_default(a->query);
  for (size_t i = 0; i < cfg_count() && iy + 2 + i < iy + irows; ++i) {
    char line[96];
    std::snprintf(line, sizeof line, "%-16s %s", cfg_name(i).c_str(),
                  cfg_value(*a, i).c_str());
    row(a->query, (int)(iy + 2 + i), (int)ix, (int)icols, line,
        i == a->cfg_cursor, true, i);
  }
}

void draw_stub(App* a, const char* title, const char* msg) {
  unsigned iy, ix, irows, icols;
  frame(a->query, title, true, &iy, &ix, &irows, &icols);
  if (irows == 0) return;
  ncplane_set_fg_rgb8(a->query, 0x77, 0x77, 0x77);
  ncplane_printf_yx(a->query, (int)iy, (int)ix, "%s", msg);
  ncplane_set_fg_default(a->query);
}

void draw_status(App* a) {
  ncplane_erase(a->status);
  unsigned rows, cols; ncplane_dim_yx(a->status, &rows, &cols); (void)rows;
  ncplane_set_bg_rgb8(a->status, 0x18, 0x18, 0x1e);
  ncplane_printf_yx(a->status, 0, 0, "%*s", (int)cols, "");
  const char* g; unsigned gr = 0xcc, gg = 0xcc, gb = 0xcc;
  if (a->status_glyph == 'k')      { g = "[ok]"; gr = 0x66; gg = 0xcc; gb = 0x66; }
  else if (a->status_glyph == '!') { g = "[!] "; gr = 0xdd; gg = 0x55; gb = 0x55; }
  else if (a->status_glyph == '.') { g = "[..]"; gr = 0xcc; gg = 0xcc; gb = 0x66; }
  else                             { g = "[i] "; }
  ncplane_set_styles(a->status, NCSTYLE_BOLD);
  ncplane_set_fg_rgb8(a->status, gr, gg, gb);
  ncplane_printf_yx(a->status, 0, 1, "%s", g);
  ncplane_set_fg_default(a->status);
  ncplane_set_styles(a->status, NCSTYLE_NONE);
  const std::string& msg = a->confirm_quit ? std::string("quit? (y/N)") : a->status_msg;
  ncplane_printf_yx(a->status, 0, 6, "%s", msg.c_str());
  const char* keys = "jk:move +/-:val Enter:edit c:drill s:save R:reload Tab:panel q:quit";
  size_t kl = std::strlen(keys);
  if (cols > kl + 8) {
    ncplane_set_fg_rgb8(a->status, 0x99, 0x99, 0xaa);
    ncplane_putstr_yx(a->status, 0, (int)(cols - kl - 1), keys);
    ncplane_set_fg_default(a->status);
  }
  ncplane_set_bg_default(a->status);
}

void redraw(App* a) {
  draw_topbar(a);
  switch (a->screen) {
    case Screen::RunQuery: draw_query(a); draw_results(a); break;
    case Screen::Config: draw_config(a); ncplane_erase(a->results); break;
    case Screen::MakeQuery:
      draw_stub(a, "Make Query", "ad-hoc queries + save — coming soon");
      ncplane_erase(a->results); break;
    case Screen::Status:
      draw_stub(a, "Status", "warehouse status (max_p, rows, snapshots) — coming soon");
      ncplane_erase(a->results); break;
    case Screen::Generate:
      draw_stub(a, "Generate", "generation runner (subprocess) — coming soon");
      ncplane_erase(a->results); break;
  }
  draw_status(a);
  if (a->modal_on) draw_modal(a);
  else if (a->disp_on) draw_dispatch(a);
  else ncplane_erase(a->modal);  // transparent when closed
  notcurses_render(a->nc);
}

// Scroll the results selection by a page (less-style Space / b / PgUp / PgDn).
void page_results(App* a, int dir) {
  unsigned rows, cols; ncplane_dim_yx(a->results, &rows, &cols); (void)cols;
  int pg = (int)rows - 3; if (pg < 1) pg = 1;
  int n = (int)a->result_rows.size();
  if (n <= 1) return;
  int s = (int)a->res_sel + dir * pg;
  a->res_sel = (size_t)(s < 1 ? 1 : s >= n ? n - 1 : s);
}

// arrows / hjkl: move cursor among query options, or scroll results.
void nav(App* a, int delta) {
  if (a->focus == Focus::Query) {
    int n = (int)option_count(*a);
    int c = (int)a->cursor + delta;
    a->cursor = (size_t)(c < 0 ? 0 : c >= n ? n - 1 : c);
  } else {
    int n = (int)a->result_rows.size();
    if (n <= 1) return;
    int s = (int)a->res_sel + delta;
    a->res_sel = (size_t)(s < 1 ? 1 : s >= n ? n - 1 : s);
  }
}

// + / - : cycle the VALUE of the cursored option (query panel only). The Preset
// option cycles presets (wrap). Numeric fields edit via a modal (NOT YET), so
// +/- is a no-op on them for now.
void cycle_value(App* a, int delta) {
  if (a->focus != Focus::Query) return;
  if (a->cursor == 0) {
    int n = (int)a->presets.size();
    a->preset_idx = (size_t)(((int)a->preset_idx + delta % n + n) % n);
    if (a->cursor >= option_count(*a)) a->cursor = option_count(*a) - 1;
    a->status_glyph = 'i';
    a->status_msg = "preset: " + a->presets[a->preset_idx].id;
  } else {
    a->status_glyph = 'i';
    a->status_msg = "numeric field — modal edit not built yet";
  }
}

// Runs ON THE WORKER THREAD. Writes pending_results / worker_status /
// worker_glyph, then publishes q_done (release) — the UI reads those only after
// observing q_done (acquire), so no lock is needed. Progress + cancel flow
// through the atomics via ScanControl.
void run_query_worker(App* a, Preset p) {
  std::string err;
  std::vector<ResultRow> out;
  primeparts::query::ScanControl ctl;
  ctl.cancel = &a->q_cancel;
  ctl.progress = [a](int64_t sc, int64_t tot) {
    a->q_scanned.store(sc, std::memory_order_relaxed);
    a->q_total.store(tot, std::memory_order_relaxed);
  };
  auto t0 = std::chrono::steady_clock::now();
  if (p.kind == "by_k") {
    auto hits = a->qs->ScanByK((int32_t)fval(p, "k"), fval(p, "p_lo"),
                               fval(p, "p_hi"), fval(p, "limit", 10), &err, ctl);
    char hdr[80]; std::snprintf(hdr, sizeof hdr, "%-20s %s", "p", "prime_rank");
    out.push_back({hdr, "", 0});
    for (const auto& h : hits) {
      char r[96]; std::snprintf(r, sizeof r, "%-20lld %lld", (long long)h.p,
                                (long long)h.prime_rank);
      out.push_back({r, "p", h.p});  // p is dispatchable (-> lookup)
    }
    double dt = secs_since(t0);
    char m[128]; std::snprintf(m, sizeof m, "k==%lld -> %zu hits in %.2fs%s",
                               (long long)fval(p, "k"), hits.size(), dt,
                               a->q_cancel.load() ? " (cancelled)" : "");
    a->worker_status = err.empty() ? m : ("error: " + err);
  } else {
    int64_t pv = fval(p, "p", 0);
    auto pi = a->qs->LookupPrime(pv, &err, ctl);
    out.push_back({"field                value", "", 0});
    if (pi) {
      char r[96];
      std::snprintf(r, sizeof r, "k                    %d", pi->k);
      out.push_back({r, "k", pi->k});  // k is dispatchable (-> by-k)
      std::snprintf(r, sizeof r, "prime_rank           %lld", (long long)pi->prime_rank);
      out.push_back({r, "", 0});
      for (const auto& t : a->qs->LookupPartitions(pv, &err, ctl)) {
        std::snprintf(r, sizeof r, "partition            2^%d + %lld^%d", t.m_k,
                      (long long)t.q_k, t.n_k);
        out.push_back({r, "q_k", t.q_k});  // q_k is a prime (-> lookup)
      }
    } else {
      out.push_back({err.empty() ? "(not present)" : err, "", 0});
    }
    double dt = secs_since(t0);
    char m[112]; std::snprintf(m, sizeof m, "lookup p=%lld in %.2fs%s",
                               (long long)pv, dt,
                               a->q_cancel.load() ? " (cancelled)" : "");
    a->worker_status = err.empty() ? m : ("error: " + err);
  }
  a->worker_glyph = err.empty() ? (a->q_cancel.load() ? '!' : 'k') : '!';
  a->pending_results = std::move(out);
  a->q_done.store(true, std::memory_order_release);
}

// Launch the query on a worker thread (UI stays live). No-op if one is running.
void start_query(App* a) {
  if (a->q_running.load()) return;
  a->q_cancel.store(false);
  a->q_done.store(false);
  a->q_scanned.store(0);
  a->q_total.store(0);
  a->result_rows.clear();
  a->res_top = a->res_sel = 0;
  a->status_glyph = '.';
  a->status_msg = "running...";
  a->focus = Focus::Results;
  a->q_running.store(true);
  a->worker = std::thread(run_query_worker, a, a->presets[a->preset_idx]);
}

// Enter on the query panel: open a field-edit modal (one box per variable
// field). If the preset has no fields, run directly.
void open_modal(App* a) {
  const Preset& p = a->presets[a->preset_idx];
  if (p.fields.empty()) { start_query(a); return; }
  a->modal_buf.clear();
  for (const auto& f : p.fields) a->modal_buf.push_back(std::to_string(f.value));
  a->modal_field = a->cursor > 0 ? a->cursor - 1 : 0;
  if (a->modal_field >= p.fields.size()) a->modal_field = 0;
  a->modal_on = true;
}

// Confirm the modal: parse each box back into its field, then run.
void confirm_modal(App* a) {
  Preset& p = a->presets[a->preset_idx];
  for (size_t i = 0; i < p.fields.size() && i < a->modal_buf.size(); ++i) {
    const std::string& b = a->modal_buf[i];
    p.fields[i].value = b.empty() ? 0 : std::strtoll(b.c_str(), nullptr, 10);
  }
  a->modal_on = false;
  start_query(a);
}

void draw_modal(App* a) {
  const Preset& p = a->presets[a->preset_idx];
  unsigned trows, tcols;
  notcurses_term_dim_yx(a->nc, &trows, &tcols);
  const unsigned nf = (unsigned)p.fields.size();
  unsigned w = tcols < 52 ? (tcols > 8 ? tcols - 4 : 8) : 48;
  unsigned h = nf + 5;
  unsigned y0 = trows > h ? (trows - h) / 2 : 1;
  unsigned x0 = tcols > w ? (tcols - w) / 2 : 1;
  ncplane_resize_simple(a->modal, h, w);
  ncplane_move_yx(a->modal, (int)y0, (int)x0);
  ncplane_erase(a->modal);
  ncplane_set_bg_rgb8(a->modal, 0x20, 0x24, 0x30);
  for (unsigned r = 0; r < h; ++r)
    ncplane_printf_yx(a->modal, (int)r, 0, "%*s", (int)w, "");
  ncplane_perimeter_rounded(a->modal, NCSTYLE_BOLD, 0, 0);
  ncplane_set_styles(a->modal, NCSTYLE_BOLD);
  ncplane_printf_yx(a->modal, 0, 2, "┤ set fields: %s ├", p.id.c_str());
  ncplane_set_styles(a->modal, NCSTYLE_NONE);
  for (unsigned i = 0; i < nf; ++i) {
    const bool foc = (i == a->modal_field);
    if (foc) {
      ncplane_set_styles(a->modal, NCSTYLE_BOLD);
      ncplane_set_bg_rgb8(a->modal, 0x2c, 0x44, 0x66);
      ncplane_set_fg_rgb8(a->modal, 0xff, 0xff, 0xff);
    }
    ncplane_printf_yx(a->modal, (int)(2 + i), 2, "%s%-8s [%-14s]",
                      foc ? "> " : "  ", p.fields[i].name.c_str(),
                      a->modal_buf[i].c_str());
    ncplane_set_styles(a->modal, NCSTYLE_NONE);
    ncplane_set_bg_rgb8(a->modal, 0x20, 0x24, 0x30);
    ncplane_set_fg_default(a->modal);
  }
  ncplane_set_fg_rgb8(a->modal, 0x99, 0x99, 0xaa);
  ncplane_printf_yx(a->modal, (int)(h - 2), 2,
                    "Enter:run  Esc/b:cancel  digits:edit  j/k:field");
  ncplane_set_fg_default(a->modal);
  ncplane_set_bg_default(a->modal);
}

// `c` on a result cell: gather the queries whose accept-set contains the cell's
// schema field name, and open a chooser. Field-name based — never a type guess.
void open_dispatch(App* a) {
  if (a->focus != Focus::Results || a->res_sel == 0 ||
      a->res_sel >= a->result_rows.size())
    return;
  const ResultRow& rr = a->result_rows[a->res_sel];
  if (rr.field.empty()) {
    a->status_glyph = 'i';
    a->status_msg = "nothing to dispatch on this row";
    return;
  }
  a->disp_cands.clear();
  for (size_t i = 0; i < a->presets.size(); ++i) {
    const auto& acc = a->presets[i].accepts;
    if (std::find(acc.begin(), acc.end(), rr.field) != acc.end())
      a->disp_cands.push_back(i);
  }
  if (a->disp_cands.empty()) {
    a->status_glyph = 'i';
    a->status_msg = "no queries accept " + rr.field;
    return;
  }
  a->disp_field = rr.field;
  a->disp_value = rr.value;
  a->disp_sel = 0;
  a->disp_on = true;
}

void confirm_dispatch(App* a) {
  const size_t pi = a->disp_cands[a->disp_sel];
  a->preset_idx = pi;
  Preset& p = a->presets[pi];
  for (auto& f : p.fields)
    if (f.name == p.target) f.value = a->disp_value;  // feed the value in
  a->cursor = 0;
  a->disp_on = false;
  start_query(a);
}

void draw_dispatch(App* a) {
  unsigned trows, tcols;
  notcurses_term_dim_yx(a->nc, &trows, &tcols);
  const unsigned ncand = (unsigned)a->disp_cands.size();
  unsigned w = (tcols > 4 && 56u > tcols - 4) ? tcols - 4 : 56;
  unsigned h = ncand + 5;
  unsigned y0 = trows > h ? (trows - h) / 2 : 1;
  unsigned x0 = tcols > w ? (tcols - w) / 2 : 1;
  ncplane_resize_simple(a->modal, h, w);
  ncplane_move_yx(a->modal, (int)y0, (int)x0);
  ncplane_erase(a->modal);
  ncplane_set_bg_rgb8(a->modal, 0x20, 0x24, 0x30);
  for (unsigned r = 0; r < h; ++r)
    ncplane_printf_yx(a->modal, (int)r, 0, "%*s", (int)w, "");
  ncplane_perimeter_rounded(a->modal, NCSTYLE_BOLD, 0, 0);
  ncplane_set_styles(a->modal, NCSTYLE_BOLD);
  ncplane_printf_yx(a->modal, 0, 2, "┤ %s = %lld → run ├", a->disp_field.c_str(),
                    (long long)a->disp_value);
  ncplane_set_styles(a->modal, NCSTYLE_NONE);
  for (unsigned i = 0; i < ncand; ++i) {
    const bool foc = (i == a->disp_sel);
    if (foc) {
      ncplane_set_styles(a->modal, NCSTYLE_BOLD);
      ncplane_set_bg_rgb8(a->modal, 0x2c, 0x44, 0x66);
      ncplane_set_fg_rgb8(a->modal, 0xff, 0xff, 0xff);
    }
    const Preset& p = a->presets[a->disp_cands[i]];
    ncplane_printf_yx(a->modal, (int)(2 + i), 2, "%s%-10s %.*s", foc ? "> " : "  ",
                      p.id.c_str(), (int)w - 16, p.desc.c_str());
    ncplane_set_styles(a->modal, NCSTYLE_NONE);
    ncplane_set_bg_rgb8(a->modal, 0x20, 0x24, 0x30);
    ncplane_set_fg_default(a->modal);
  }
  ncplane_set_fg_rgb8(a->modal, 0x99, 0x99, 0xaa);
  ncplane_printf_yx(a->modal, (int)(h - 2), 2, "Enter:run  Esc/b:cancel  j/k:choose");
  ncplane_set_fg_default(a->modal);
  ncplane_set_bg_default(a->modal);
}

std::vector<Preset> built_in_presets() {  // fallback if the Lua file is missing
  return {
      Preset{"by-k", "primes where k == {k}, p in [{p_lo},{p_hi}], LIMIT {limit}",
             "by_k", {{"k", 0}, {"p_lo", 0}, {"p_hi", 0}, {"limit", 10}},
             {"k"}, "k"},
      Preset{"lookup", "prime p == {p}  ->  k + partitions", "lookup",
             {{"p", 11}}, {"p", "q_k"}, "p"},
  };
}

// Load presets from the Lua file, validate each via the reader, keep the valid
// ones; fall back to the built-ins if the file is missing/empty/all-invalid.
void load_presets(App* a) {
  std::vector<std::string> errs;
  // Config file if it exists, else the shipped seed (binary-relative).
  fs::path src = fs::exists(a->presets_path) ? fs::path(a->presets_path)
                                             : binary_seed_path();
  std::vector<Preset> loaded;
  if (!src.empty() && fs::exists(src)) loaded = a->lua.Load(src, &errs);
  std::vector<Preset> valid;
  int rejected = 0;
  auto have = [&](const std::string& id) {
    for (const auto& v : valid)
      if (v.id == id) return true;
    return false;
  };
  for (auto& p : loaded) {
    if (have(p.id)) continue;  // dedup by id (first wins)
    std::string ve;
    if (a->qs && a->qs->ValidatePreset(p, &ve)) valid.push_back(std::move(p));
    else ++rejected;
  }
  if (valid.empty()) {
    a->presets = built_in_presets();
    a->status_glyph = loaded.empty() ? 'i' : '!';
    a->status_msg = loaded.empty() ? "no presets file — using built-ins"
                                   : "all presets invalid — using built-ins";
  } else {
    a->presets = std::move(valid);
    char m[96];
    std::snprintf(m, sizeof m, "loaded %zu presets%s", a->presets.size(),
                  rejected ? " (some rejected)" : "");
    a->status_glyph = rejected ? '!' : 'i';
    a->status_msg = m;
  }
  if (a->preset_idx >= a->presets.size()) a->preset_idx = 0;
  a->cursor = 0;
}

// Save the current preset (with its current field values) to the Lua file.
void save_current(App* a) {
  if (a->presets.empty()) return;
  const Preset& p = a->presets[a->preset_idx];
  std::string ve;
  if (a->qs && !a->qs->ValidatePreset(p, &ve)) {
    a->status_glyph = '!';
    a->status_msg = "invalid, not saved: " + ve;
    return;
  }
  std::string se;
  // Rewrite the whole (id-unique) table -> deduped, persists field edits, and
  // lands in the config path (created if needed).
  if (LuaPresets::SaveAll(a->presets, a->presets_path, &se)) {
    a->status_glyph = 'k';
    a->status_msg = "saved " + std::to_string(a->presets.size()) +
                    " presets -> " + a->presets_path;
  } else {
    a->status_glyph = '!';
    a->status_msg = "save failed: " + se;
  }
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
  if (LuaPresets::SaveConfig(config_to_kv(a->cfg), a->config_path, &se)) {
    a->status_glyph = 'k';
    a->status_msg = "saved config -> " + a->config_path;
    run_janitor(a);
  } else {
    a->status_glyph = '!';
    a->status_msg = "config save failed: " + se;
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string warehouse = argc >= 2 ? argv[1] : kDefaultWarehouse;
  std::string err;
  auto qs = QueryService::Open(warehouse, &err);
  if (!qs) {
    std::fprintf(stderr, "QueryService::Open(%s): %s\n", warehouse.c_str(), err.c_str());
    return 1;
  }
  notcurses_options opts{};
  opts.flags = NCOPTION_SUPPRESS_BANNERS;
  struct notcurses* nc = notcurses_core_init(&opts, nullptr);
  if (!nc) { std::fprintf(stderr, "notcurses_core_init failed\n"); return 1; }

  App app;
  app.nc = nc;
  app.qs = qs.get();
  app.presets_path = config_presets_path().string();  // ~/.config/primeparts/...
  app.config_path = config_file_path().string();
  load_config(&app);   // config.lua if present, else defaults
  load_presets(&app);  // config file, else shipped seed, else built-ins
  struct ncplane* std_ = notcurses_stdplane(nc);
  ncplane_options po{}; po.rows = 1; po.cols = 1;
  app.topbar = ncplane_create(std_, &po);
  app.query = ncplane_create(std_, &po);
  app.results = ncplane_create(std_, &po);
  app.status = ncplane_create(std_, &po);
  app.modal = ncplane_create(std_, &po);  // created last -> top z-order
  if (!app.topbar || !app.query || !app.results || !app.status || !app.modal ||
      layout(&app) < 0) {
    notcurses_stop(nc);
    std::fprintf(stderr, "terminal too small (need >= 12x48)\n");
    return 1;
  }
  redraw(&app);

  bool running = true;
  while (running) {
    // While a query runs on the worker thread, poll input non-blocking so the
    // UI stays live: show progress, let Esc cancel.
    if (app.q_running.load()) {
      struct timespec ts{0, 60'000'000};  // 60 ms
      ncinput pin;
      uint32_t pk = notcurses_get(nc, &ts, &pin);
      if (pk != 0 && pin.evtype != NCTYPE_RELEASE && pk == NCKEY_ESC)
        app.q_cancel.store(true);
      if (app.q_done.load(std::memory_order_acquire)) {
        app.worker.join();
        app.q_running.store(false);
        app.result_rows = std::move(app.pending_results);
        app.status_msg = app.worker_status;
        app.status_glyph = app.worker_glyph;
        app.res_sel = app.result_rows.size() > 1 ? 1 : 0;
      } else if (app.q_cancel.load()) {
        app.status_glyph = '.'; app.status_msg = "cancelling...";
      } else {
        int64_t sc = app.q_scanned.load(), tot = app.q_total.load();
        char m[96];
        if (tot > 0)
          std::snprintf(m, sizeof m, "running %.1f%%  (%lld / %lld rows)  — Esc cancel",
                        100.0 * (double)sc / (double)tot, (long long)sc, (long long)tot);
        else
          std::snprintf(m, sizeof m, "running...  — Esc cancel");
        app.status_glyph = '.'; app.status_msg = m;
      }
      redraw(&app);
      continue;
    }

    ncinput in;
    uint32_t key = notcurses_get_blocking(nc, &in);
    if (in.evtype == NCTYPE_RELEASE) continue;
    if (app.confirm_quit) {
      if (key == 'y' || key == 'Y') running = false;
      else app.confirm_quit = false;
      redraw(&app); continue;
    }
    // Modal field editor captures input while open.
    if (app.modal_on) {
      const size_t nf = app.presets[app.preset_idx].fields.size();
      if (key == NCKEY_ESC || key == 'b') app.modal_on = false;
      else if (key == NCKEY_ENTER || key == '\n' || key == '\r') confirm_modal(&app);
      else if (key == 'j' || key == NCKEY_DOWN || key == NCKEY_TAB)
        app.modal_field = nf ? (app.modal_field + 1) % nf : 0;
      else if (key == 'k' || key == NCKEY_UP)
        app.modal_field = nf ? (app.modal_field + nf - 1) % nf : 0;
      else if (key == NCKEY_BACKSPACE || key == NCKEY_DEL || key == 127 || key == 8) {
        if (!app.modal_buf[app.modal_field].empty())
          app.modal_buf[app.modal_field].pop_back();
      } else if (key >= '0' && key <= '9') {
        if (app.modal_buf[app.modal_field].size() < 18)
          app.modal_buf[app.modal_field].push_back((char)key);
      } else if (key == '-' && app.modal_buf[app.modal_field].empty()) {
        app.modal_buf[app.modal_field].push_back('-');  // allow negative entry
      }
      redraw(&app); continue;
    }
    // Dispatch chooser captures input while open.
    if (app.disp_on) {
      const size_t ncd = app.disp_cands.size();
      if (key == NCKEY_ESC || key == 'b') app.disp_on = false;
      else if (key == NCKEY_ENTER || key == '\n' || key == '\r') confirm_dispatch(&app);
      else if (key == 'j' || key == NCKEY_DOWN) app.disp_sel = ncd ? (app.disp_sel + 1) % ncd : 0;
      else if (key == 'k' || key == NCKEY_UP)   app.disp_sel = ncd ? (app.disp_sel + ncd - 1) % ncd : 0;
      redraw(&app); continue;
    }
    // Global: F1..F5 switch screens; q quits; resize relayouts.
    if (key == NCKEY_F01 || key == NCKEY_F02 || key == NCKEY_F03 ||
        key == NCKEY_F04 || key == NCKEY_F05) {
      app.screen = key == NCKEY_F01   ? Screen::RunQuery
                   : key == NCKEY_F02 ? Screen::MakeQuery
                   : key == NCKEY_F03 ? Screen::Status
                   : key == NCKEY_F04 ? Screen::Generate
                                      : Screen::Config;
      app.cfg_cursor = 0;
      layout(&app); redraw(&app); continue;
    }
    if (key == 'q') { app.confirm_quit = true; redraw(&app); continue; }
    if (key == NCKEY_RESIZE) { layout(&app); redraw(&app); continue; }
    // Config screen input.
    if (app.screen == Screen::Config) {
      const size_t n = cfg_count();
      if ((key == 'j' || key == NCKEY_DOWN) && app.cfg_cursor + 1 < n) ++app.cfg_cursor;
      else if ((key == 'k' || key == NCKEY_UP) && app.cfg_cursor > 0) --app.cfg_cursor;
      else if (key == '+' || key == '=') cfg_adjust(&app, app.cfg_cursor, +1);
      else if (key == '-') cfg_adjust(&app, app.cfg_cursor, -1);
      else if (key == 's') save_config(&app);
      redraw(&app); continue;
    }
    // Stub screens consume nothing but the globals above.
    if (app.screen != Screen::RunQuery) { redraw(&app); continue; }
    switch (key) {
      case 'q': app.confirm_quit = true; break;
      case NCKEY_RESIZE: layout(&app); break;
      case NCKEY_TAB:
        app.focus = app.focus == Focus::Query ? Focus::Results : Focus::Query;
        break;
      case 'j': case NCKEY_DOWN: nav(&app, +1); break;
      case 'k': case NCKEY_UP:   nav(&app, -1); break;
      case '+': case '=':        cycle_value(&app, +1); break;
      case '-':                  cycle_value(&app, -1); break;
      case ' ': case NCKEY_PGDOWN: page_results(&app, +1); break;
      case 'b': case NCKEY_PGUP:   page_results(&app, -1); break;
      // Enter = select: on the query panel, open the field-edit modal (which
      // runs on confirm). In results, reserved for `c`-dispatch later.
      case NCKEY_ENTER: case '\n': case '\r':
        if (app.focus == Focus::Query) open_modal(&app);
        break;
      case 'c':  // dispatch the cursored result value into a compatible query
        if (app.focus == Focus::Results) open_dispatch(&app);
        break;
      case 's': save_current(&app); break;   // save current query to Lua
      case 'R': load_presets(&app); break;   // reload presets from Lua
      default: break;
    }
    redraw(&app);
  }

  if (app.worker.joinable()) {  // cancel + join any in-flight query
    app.q_cancel.store(true);
    app.worker.join();
  }
  ncplane_destroy(app.topbar);
  ncplane_destroy(app.query);
  ncplane_destroy(app.results);
  ncplane_destroy(app.status);
  ncplane_destroy(app.modal);
  notcurses_stop(nc);
  return 0;
}
