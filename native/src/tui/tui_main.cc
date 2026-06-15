// primeparts TUI — first pass.
//
// Multi-screen app design: markdown/arch/tui_app_design.md. This first pass is
// the Run Saved Query screen only (F1): two hardcoded presets, +/- to cycle,
// Tab to switch the query/results panels, a key to run, scrollable results,
// q -> (y|N) quit. C++ calling QueryService directly (no C ABI / JSON).
//
// NOT YET (next passes, per the spec): field-edit modal; threaded + cancellable
// + progress queries; the `c` field-name dispatch; the other screens (Make
// Query / Status / Generate / Config) behind F2..F5; Ctrl+L error log; Lua
// presets. Hardcoded presets stand in for the Lua config (see QueryService).
//
// All notcurses calls are from the curated safe surface (verified via the
// notcurses-ux guardrails) + the vetted split-pane / list / status-bar
// skeletons.

#include <notcurses/notcurses.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "primeparts/query/query_service.h"

using primeparts::query::PartitionTuple;
using primeparts::query::PrimeInfo;
using primeparts::query::QueryService;
using primeparts::query::ScanHit;

namespace {

constexpr char kDefaultWarehouse[] =
    "/media/extssd/research/dioph.pp/data/ib-staging";

// --- Preset model (hardcoded stand-in for Lua config) ---------------------
struct Field {
  std::string name;
  int64_t value;
};

enum class Kind { ByK, Lookup };

struct Preset {
  std::string title;
  Kind kind;
  std::vector<Field> fields;  // variable fields, left for the user to set
};

// --- App state ------------------------------------------------------------
enum class Focus { Query, Results };

struct App {
  struct notcurses* nc = nullptr;
  struct ncplane* topbar = nullptr;   // F-key screen bar
  struct ncplane* query = nullptr;    // preset list + fields (top)
  struct ncplane* results = nullptr;  // results table (bottom)
  struct ncplane* status = nullptr;   // reserved bottom row

  QueryService* qs = nullptr;

  std::vector<Preset> presets;
  size_t sel = 0;       // selected preset
  Focus focus = Focus::Query;

  std::vector<std::string> result_lines;  // rendered result rows
  size_t res_top = 0;                     // scroll offset
  size_t res_sel = 0;                     // selected result row

  std::string status_msg = "ready";
  char status_glyph = 'i';  // i / . / k(ok) / !  -> [i]/[..]/[ok]/[!]
  bool confirm_quit = false;
};

double secs_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
      .count();
}

const Field* find_field(const Preset& p, const char* name) {
  for (const auto& f : p.fields)
    if (f.name == name) return &f;
  return nullptr;
}
int64_t fval(const Preset& p, const char* name, int64_t dflt = 0) {
  const Field* f = find_field(p, name);
  return f ? f->value : dflt;
}

// --- Layout (vetted split-pane: reserve top bar + bottom status row) -------
int layout(App* a) {
  unsigned rows, cols;
  notcurses_term_dim_yx(a->nc, &rows, &cols);
  if (rows < 8 || cols < 40) return -1;  // too small

  ncplane_resize_simple(a->topbar, 1, cols);
  ncplane_move_yx(a->topbar, 0, 0);

  const unsigned body = rows - 2;            // minus topbar + status
  unsigned qh = 6;                           // query panel height
  if (qh > body - 3) qh = body - 3;          // keep >=3 rows for results
  ncplane_resize_simple(a->query, qh, cols);
  ncplane_move_yx(a->query, 1, 0);
  ncplane_resize_simple(a->results, body - qh, cols);
  ncplane_move_yx(a->results, 1 + qh, 0);

  ncplane_resize_simple(a->status, 1, cols);
  ncplane_move_yx(a->status, rows - 1, 0);
  return 0;
}

// --- Renders --------------------------------------------------------------
void draw_topbar(App* a) {
  ncplane_erase(a->topbar);
  // F1 active (only Run Query implemented in this pass); others dimmed.
  struct Tab { const char* label; bool active; };
  const Tab tabs[] = {{"F1 Run Query", true},   {"F2 Make", false},
                      {"F3 Status", false},      {"F4 Generate", false},
                      {"F5 Config", false}};
  int x = 0;
  for (const auto& t : tabs) {
    if (t.active) {
      ncplane_set_styles(a->topbar, NCSTYLE_BOLD);
      ncplane_set_bg_rgb8(a->topbar, 0x33, 0x33, 0x55);
    } else {
      ncplane_set_styles(a->topbar, NCSTYLE_NONE);
      ncplane_set_fg_rgb8(a->topbar, 0x88, 0x88, 0x88);  // dim inactive
    }
    ncplane_printf_yx(a->topbar, 0, x, " %s ", t.label);
    ncplane_set_bg_default(a->topbar);
    ncplane_set_fg_default(a->topbar);
    x += (int)std::strlen(t.label) + 2;
  }
  ncplane_set_styles(a->topbar, NCSTYLE_NONE);
}

void draw_query(App* a) {
  ncplane_erase(a->query);
  unsigned rows, cols;
  ncplane_dim_yx(a->query, &rows, &cols);
  const bool focused = a->focus == Focus::Query;

  ncplane_set_styles(a->query, NCSTYLE_BOLD);
  ncplane_printf_yx(a->query, 0, 0, "%sSaved Queries  (+/- cycle, Enter run)",
                    focused ? "> " : "  ");
  ncplane_set_styles(a->query, NCSTYLE_NONE);

  // Preset list (compact: one row each).
  for (size_t i = 0; i < a->presets.size() && (int)i + 1 < (int)rows; ++i) {
    const bool active = (i == a->sel);
    if (active && focused) {
      ncplane_set_styles(a->query, NCSTYLE_BOLD);
      ncplane_set_bg_rgb8(a->query, 0x44, 0x44, 0x44);
    }
    ncplane_printf_yx(a->query, (int)i + 1, 0, "%s%-*s",
                      active ? "> " : "  ", (int)cols - 2,
                      a->presets[i].title.c_str());
    ncplane_set_bg_default(a->query);
    ncplane_set_styles(a->query, NCSTYLE_NONE);
  }

  // Selected preset's current field values (one line).
  const Preset& p = a->presets[a->sel];
  std::string fline;
  for (const auto& f : p.fields) {
    char buf[64];
    if ((f.name == "p_lo" || f.name == "p_hi") && f.value <= 0)
      std::snprintf(buf, sizeof buf, "%s=any  ", f.name.c_str());
    else
      std::snprintf(buf, sizeof buf, "%s=%lld  ", f.name.c_str(),
                    (long long)f.value);
    fline += buf;
  }
  int fy = (int)a->presets.size() + 2;
  if (fy < (int)rows)
    ncplane_printf_yx(a->query, fy, 0, "  %.*s", (int)cols - 2, fline.c_str());
}

void draw_results(App* a) {
  ncplane_erase(a->results);
  unsigned rows, cols;
  ncplane_dim_yx(a->results, &rows, &cols);
  const bool focused = a->focus == Focus::Results;

  ncplane_set_styles(a->results, NCSTYLE_BOLD);
  ncplane_printf_yx(a->results, 0, 0, "%sResults (%zu)", focused ? "> " : "  ",
                    a->result_lines.size());
  ncplane_set_styles(a->results, NCSTYLE_NONE);

  const unsigned list_rows = rows - 1;  // row 0 is the title
  if (a->result_lines.empty()) {
    ncplane_printf_yx(a->results, 1, 0, "  (run a query)");
    return;
  }
  // Keep selection visible.
  if (a->res_sel < a->res_top) a->res_top = a->res_sel;
  else if (a->res_sel >= a->res_top + list_rows)
    a->res_top = a->res_sel - list_rows + 1;

  for (unsigned r = 0; r < list_rows && a->res_top + r < a->result_lines.size();
       ++r) {
    const size_t idx = a->res_top + r;
    const bool active = (idx == a->res_sel);
    if (active && focused) {
      ncplane_set_styles(a->results, NCSTYLE_BOLD);
      ncplane_set_bg_rgb8(a->results, 0x44, 0x44, 0x44);
    }
    ncplane_printf_yx(a->results, (int)r + 1, 0, "%s%-*s", active ? "> " : "  ",
                      (int)cols - 2, a->result_lines[idx].c_str());
    ncplane_set_bg_default(a->results);
    ncplane_set_styles(a->results, NCSTYLE_NONE);
  }
  // Scroll indicators.
  if (a->res_top > 0) ncplane_putstr_yx(a->results, 1, cols - 1, "^");
  if (a->res_top + list_rows < a->result_lines.size())
    ncplane_putstr_yx(a->results, rows - 1, cols - 1, "v");
}

void draw_status(App* a) {
  ncplane_erase(a->status);
  unsigned rows, cols;
  ncplane_dim_yx(a->status, &rows, &cols);
  (void)rows;
  const char* g = a->status_glyph == 'k'   ? "[ok]"
                  : a->status_glyph == '!'  ? "[!]"
                  : a->status_glyph == '.'  ? "[..]"
                                            : "[i]";
  ncplane_set_styles(a->status, NCSTYLE_BOLD);
  ncplane_printf_yx(a->status, 0, 0, "%s ", g);
  ncplane_set_styles(a->status, NCSTYLE_NONE);
  const std::string& msg =
      a->confirm_quit ? std::string("quit? (y/N)") : a->status_msg;
  ncplane_putstr(a->status, msg.c_str());

  const char* keys = "+/-:cycle  Tab:panel  Enter:run  j/k:scroll  q:quit";
  size_t klen = std::strlen(keys);
  if (cols > klen + 6) ncplane_putstr_yx(a->status, 0, (int)(cols - klen), keys);
}

void redraw(App* a) {
  draw_topbar(a);
  draw_query(a);
  draw_results(a);
  draw_status(a);
  notcurses_render(a->nc);
}

// --- Run the selected preset (synchronous for this first pass) -------------
void run_selected(App* a) {
  const Preset& p = a->presets[a->sel];
  a->result_lines.clear();
  a->res_top = a->res_sel = 0;
  a->status_glyph = '.';
  a->status_msg = "running...";
  redraw(a);

  std::string err;
  auto t0 = std::chrono::steady_clock::now();
  if (p.kind == Kind::ByK) {
    auto hits = a->qs->ScanByK((int32_t)fval(p, "k"), fval(p, "p_lo"),
                               fval(p, "p_hi"), fval(p, "limit", 10), &err);
    char hdr[64];
    std::snprintf(hdr, sizeof hdr, "%-18s %s", "p", "prime_rank");
    a->result_lines.emplace_back(hdr);
    for (const auto& h : hits) {
      char row[80];
      std::snprintf(row, sizeof row, "%-18lld %lld", (long long)h.p,
                    (long long)h.prime_rank);
      a->result_lines.emplace_back(row);
    }
    double dt = secs_since(t0);
    char msg[96];
    std::snprintf(msg, sizeof msg, "k==%lld -> %zu hits in %.2fs",
                  (long long)fval(p, "k"), hits.size(), dt);
    a->status_msg = err.empty() ? msg : ("error: " + err);
  } else {  // Lookup
    int64_t pv = fval(p, "p", 0);
    auto pi = a->qs->LookupPrime(pv, &err);
    if (pi) {
      char row[96];
      std::snprintf(row, sizeof row, "p=%lld   k=%d   prime_rank=%lld",
                    (long long)pi->p, pi->k, (long long)pi->prime_rank);
      a->result_lines.emplace_back(row);
      auto parts = a->qs->LookupPartitions(pv, &err);
      a->result_lines.emplace_back("partitions (m_k, n_k, q_k):");
      for (const auto& t : parts) {
        char r2[80];
        std::snprintf(r2, sizeof r2, "  m=%-3d n=%-3d q=%lld", t.m_k, t.n_k,
                      (long long)t.q_k);
        a->result_lines.emplace_back(r2);
      }
    } else {
      a->result_lines.emplace_back(err.empty() ? "(not present)" : err);
    }
    double dt = secs_since(t0);
    char msg[96];
    std::snprintf(msg, sizeof msg, "lookup p=%lld in %.2fs", (long long)pv, dt);
    a->status_msg = err.empty() ? msg : ("error: " + err);
  }
  a->status_glyph = err.empty() ? 'k' : '!';
  a->focus = Focus::Results;
}

// --- Presets (hardcoded; future: scripts/lua/presets.lua) ------------------
std::vector<Preset> make_presets() {
  return {
      Preset{"by-k  ·  primes where k == {k}, p in [{p_lo},{p_hi}], LIMIT {limit}",
             Kind::ByK,
             {{"k", 0}, {"p_lo", 0}, {"p_hi", 0}, {"limit", 10}}},
      Preset{"lookup  ·  prime p == {p}  (k + partitions)", Kind::Lookup,
             {{"p", 11}}},
  };
}

}  // namespace

int main(int argc, char** argv) {
  const std::string warehouse = argc >= 2 ? argv[1] : kDefaultWarehouse;

  std::string err;
  auto qs = QueryService::Open(warehouse, &err);
  if (!qs) {
    std::fprintf(stderr, "QueryService::Open(%s): %s\n", warehouse.c_str(),
                 err.c_str());
    return 1;
  }

  notcurses_options opts{};
  opts.flags = NCOPTION_SUPPRESS_BANNERS;
  struct notcurses* nc = notcurses_core_init(&opts, nullptr);
  if (!nc) {
    std::fprintf(stderr, "notcurses_core_init failed\n");
    return 1;
  }

  App app;
  app.nc = nc;
  app.qs = qs.get();
  app.presets = make_presets();
  struct ncplane* std_ = notcurses_stdplane(nc);
  ncplane_options po{};
  po.rows = 1;
  po.cols = 1;
  app.topbar = ncplane_create(std_, &po);
  app.query = ncplane_create(std_, &po);
  app.results = ncplane_create(std_, &po);
  app.status = ncplane_create(std_, &po);
  if (!app.topbar || !app.query || !app.results || !app.status ||
      layout(&app) < 0) {
    notcurses_stop(nc);
    std::fprintf(stderr, "terminal too small (need >= 8x40)\n");
    return 1;
  }

  redraw(&app);

  bool running = true;
  while (running) {
    ncinput in;
    uint32_t key = notcurses_get_blocking(nc, &in);
    if (in.evtype == NCTYPE_RELEASE) continue;  // presses + repeats only

    if (app.confirm_quit) {
      if (key == 'y' || key == 'Y') running = false;
      else app.confirm_quit = false;  // anything else cancels
      redraw(&app);
      continue;
    }

    switch (key) {
      case 'q':
        app.confirm_quit = true;
        break;
      case NCKEY_RESIZE:
        layout(&app);
        break;
      case NCKEY_TAB:  // 0x09 == '\t'
        app.focus = app.focus == Focus::Query ? Focus::Results : Focus::Query;
        break;
      case '+':
      case '=':  // unshifted '+'
        if (app.focus == Focus::Query && !app.presets.empty())
          app.sel = (app.sel + 1) % app.presets.size();
        break;
      case '-':
        if (app.focus == Focus::Query && !app.presets.empty())
          app.sel = (app.sel + app.presets.size() - 1) % app.presets.size();
        break;
      case NCKEY_ENTER:
      case '\n':
      case '\r':
        run_selected(&app);
        break;
      case 'j':
      case NCKEY_DOWN:
        if (app.focus == Focus::Results && !app.result_lines.empty() &&
            app.res_sel + 1 < app.result_lines.size())
          ++app.res_sel;
        break;
      case 'k':
      case NCKEY_UP:
        if (app.focus == Focus::Results && app.res_sel > 0) --app.res_sel;
        break;
      default:
        break;
    }
    redraw(&app);
  }

  ncplane_destroy(app.topbar);
  ncplane_destroy(app.query);
  ncplane_destroy(app.results);
  ncplane_destroy(app.status);
  notcurses_stop(nc);
  return 0;
}
