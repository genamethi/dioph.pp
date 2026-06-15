// primeparts TUI — first pass (v2: bordered panes, 30/70, zebra rows).
//
// Multi-screen app design: markdown/arch/tui_app_design.md. This pass is the Run
// Saved Query screen only (F1). C++ calling QueryService directly.
//
// Structure (notcurses-ux vetted surface): each pane owns one ncplane and draws
// a rounded perimeter + title; content sits inside with 1-cell padding; list
// rows are zebra-striped with an inverted+caret focus row. Reserved top F-key
// bar and bottom status row. One notcurses_render() per input frame.
//
// NOT YET (next passes): field-edit modal; threaded+cancellable+progress
// queries; `c` field-name dispatch; F2..F5 screens; Ctrl+L log; Lua presets.

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

struct Field { std::string name; int64_t value; };
enum class Kind { ByK, Lookup };
struct Preset {
  std::string title;
  Kind kind;
  std::vector<Field> fields;
};

enum class Focus { Query, Results };

struct App {
  struct notcurses* nc = nullptr;
  struct ncplane* topbar = nullptr;
  struct ncplane* query = nullptr;
  struct ncplane* results = nullptr;
  struct ncplane* status = nullptr;

  QueryService* qs = nullptr;
  std::vector<Preset> presets;
  size_t sel = 0;
  Focus focus = Focus::Query;

  std::vector<std::string> result_lines;
  size_t res_top = 0, res_sel = 0;

  std::string status_msg = "ready";
  char status_glyph = 'i';
  bool confirm_quit = false;
};

double secs_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
int64_t fval(const Preset& p, const char* name, int64_t dflt = 0) {
  for (const auto& f : p.fields)
    if (f.name == name) return f.value;
  return dflt;
}

// Draw one full-width list row inside a bordered pane: zebra background, or an
// inverted + caret row when it's the focused selection.
void row(struct ncplane* pl, int y, int x0, int w, const std::string& text,
         bool selected, bool focused, size_t idx) {
  if (selected && focused) {
    ncplane_set_styles(pl, NCSTYLE_BOLD);
    ncplane_set_bg_rgb8(pl, 0x2c, 0x44, 0x66);
    ncplane_set_fg_rgb8(pl, 0xff, 0xff, 0xff);
  } else if (idx % 2 == 0) {
    ncplane_set_bg_rgb8(pl, 0x1c, 0x1c, 0x22);  // zebra shade
  }
  ncplane_printf_yx(pl, y, x0, "%s%-*s", selected ? "> " : "  ",
                    w - 2, text.c_str());
  ncplane_set_styles(pl, NCSTYLE_NONE);
  ncplane_set_bg_default(pl);
  ncplane_set_fg_default(pl);
}

// Bordered pane frame + title embedded in the top border. Returns inner geom.
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

// --- Layout: F-key bar (top) + 30/70 query/results + status (bottom) -------
int layout(App* a) {
  unsigned rows, cols;
  notcurses_term_dim_yx(a->nc, &rows, &cols);
  if (rows < 12 || cols < 48) return -1;

  ncplane_resize_simple(a->topbar, 1, cols);
  ncplane_move_yx(a->topbar, 0, 0);

  const unsigned body = rows - 2;
  unsigned qh = body * 3 / 10;          // 30%
  if (qh < 5) qh = 5;                    // room for border + 2 presets + fields
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
  unsigned rows, cols;
  ncplane_dim_yx(a->topbar, &rows, &cols);
  ncplane_set_bg_rgb8(a->topbar, 0x22, 0x22, 0x2c);  // bar fill
  ncplane_printf_yx(a->topbar, 0, 0, "%*s", (int)cols, "");
  struct Tab { const char* label; bool active; };
  const Tab tabs[] = {{"F1 Run Query", true}, {"F2 Make", false},
                      {"F3 Status", false},   {"F4 Generate", false},
                      {"F5 Config", false}};
  int x = 1;
  for (const auto& t : tabs) {
    if (t.active) {
      ncplane_set_styles(a->topbar, NCSTYLE_BOLD);
      ncplane_set_bg_rgb8(a->topbar, 0x3a, 0x55, 0x88);
      ncplane_set_fg_rgb8(a->topbar, 0xff, 0xff, 0xff);
    } else {
      ncplane_set_styles(a->topbar, NCSTYLE_NONE);
      ncplane_set_bg_rgb8(a->topbar, 0x22, 0x22, 0x2c);
      ncplane_set_fg_rgb8(a->topbar, 0x99, 0x99, 0xaa);
    }
    ncplane_printf_yx(a->topbar, 0, x, " %s ", t.label);
    x += (int)std::strlen(t.label) + 3;
  }
  ncplane_set_styles(a->topbar, NCSTYLE_NONE);
  ncplane_set_bg_default(a->topbar);
  ncplane_set_fg_default(a->topbar);
}

void draw_query(App* a) {
  const bool foc = a->focus == Focus::Query;
  unsigned iy, ix, irows, icols;
  frame(a->query, "Saved Queries", foc, &iy, &ix, &irows, &icols);
  if (irows == 0) return;

  unsigned y = iy;
  for (size_t i = 0; i < a->presets.size() && y < iy + irows - 1; ++i, ++y)
    row(a->query, (int)y, (int)ix, (int)icols, a->presets[i].title,
        i == a->sel, foc, i);

  // Selected preset's current field values (highlighted line).
  const Preset& p = a->presets[a->sel];
  std::string fline;
  for (const auto& f : p.fields) {
    char b[64];
    if ((f.name == "p_lo" || f.name == "p_hi") && f.value <= 0)
      std::snprintf(b, sizeof b, "%s=any  ", f.name.c_str());
    else
      std::snprintf(b, sizeof b, "%s=%lld  ", f.name.c_str(), (long long)f.value);
    fline += b;
  }
  if (y < iy + irows) {
    ncplane_set_fg_rgb8(a->query, 0xcc, 0xcc, 0x66);
    ncplane_printf_yx(a->query, (int)(iy + irows - 1), (int)ix, "%.*s",
                      (int)icols, fline.c_str());
    ncplane_set_fg_default(a->query);
  }
}

void draw_results(App* a) {
  const bool foc = a->focus == Focus::Results;
  char title[48];
  std::snprintf(title, sizeof title, "Results (%zu)", a->result_lines.size());
  unsigned iy, ix, irows, icols;
  frame(a->results, title, foc, &iy, &ix, &irows, &icols);
  if (irows == 0) return;

  if (a->result_lines.empty()) {
    ncplane_set_fg_rgb8(a->results, 0x77, 0x77, 0x77);
    ncplane_printf_yx(a->results, (int)iy, (int)ix, "(run a query — Enter)");
    ncplane_set_fg_default(a->results);
    return;
  }
  // The first line is a header; the rest scroll.
  ncplane_set_styles(a->results, NCSTYLE_BOLD);
  ncplane_set_fg_rgb8(a->results, 0x88, 0xcc, 0xcc);
  ncplane_printf_yx(a->results, (int)iy, (int)ix, "%-*s", (int)icols,
                    a->result_lines[0].c_str());
  ncplane_set_styles(a->results, NCSTYLE_NONE);
  ncplane_set_fg_default(a->results);

  const unsigned list_rows = irows - 1;        // minus header
  const size_t n = a->result_lines.size();     // includes header at 0
  // res_sel/res_top index the data rows (1..n-1).
  if (a->res_sel < 1 && n > 1) a->res_sel = 1;
  if (a->res_sel < a->res_top) a->res_top = a->res_sel;
  else if (list_rows && a->res_sel >= a->res_top + list_rows)
    a->res_top = a->res_sel - list_rows + 1;
  if (a->res_top < 1) a->res_top = 1;

  for (unsigned r = 0; r < list_rows && a->res_top + r < n; ++r) {
    const size_t idx = a->res_top + r;
    row(a->results, (int)(iy + 1 + r), (int)ix, (int)icols,
        a->result_lines[idx], idx == a->res_sel, foc, idx);
  }
  // Scroll indicators on the right border.
  unsigned prows, pcols;
  ncplane_dim_yx(a->results, &prows, &pcols);
  if (a->res_top > 1) ncplane_putstr_yx(a->results, 1, pcols - 1, "▲");
  if (a->res_top + list_rows < n)
    ncplane_putstr_yx(a->results, prows - 2, pcols - 1, "▼");
}

void draw_status(App* a) {
  ncplane_erase(a->status);
  unsigned rows, cols;
  ncplane_dim_yx(a->status, &rows, &cols);
  ncplane_set_bg_rgb8(a->status, 0x18, 0x18, 0x1e);
  ncplane_printf_yx(a->status, 0, 0, "%*s", (int)cols, "");
  // Glyph with semantic color (color is decoration; glyph is the signal).
  const char* g;
  unsigned gr = 0xcc, gg = 0xcc, gb = 0xcc;
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
  const char* keys = "+/- ↑↓ hjkl:nav  Tab:panel  Enter:run  q:quit";
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
  draw_query(a);
  draw_results(a);
  draw_status(a);
  notcurses_render(a->nc);
}

// Unified navigation: +/- == arrows == hjkl, moving the focused pane's
// selection (clamped, arrow-key semantics).
void nav(App* a, int delta) {
  if (a->focus == Focus::Query) {
    int n = (int)a->presets.size();
    if (n == 0) return;
    int s = (int)a->sel + delta;
    a->sel = (size_t)(s < 0 ? 0 : s >= n ? n - 1 : s);
  } else {
    int n = (int)a->result_lines.size();
    if (n <= 1) return;
    int s = (int)a->res_sel + delta;
    a->res_sel = (size_t)(s < 1 ? 1 : s >= n ? n - 1 : s);
  }
}

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
    char hdr[80];
    std::snprintf(hdr, sizeof hdr, "%-20s %s", "p", "prime_rank");
    a->result_lines.emplace_back(hdr);
    for (const auto& h : hits) {
      char r[96];
      std::snprintf(r, sizeof r, "%-20lld %lld", (long long)h.p, (long long)h.prime_rank);
      a->result_lines.emplace_back(r);
    }
    double dt = secs_since(t0);
    char m[112];
    std::snprintf(m, sizeof m, "k==%lld -> %zu hits in %.2fs",
                  (long long)fval(p, "k"), hits.size(), dt);
    a->status_msg = err.empty() ? m : ("error: " + err);
  } else {
    int64_t pv = fval(p, "p", 0);
    auto pi = a->qs->LookupPrime(pv, &err);
    a->result_lines.emplace_back("field                value");
    if (pi) {
      char r[96];
      std::snprintf(r, sizeof r, "k                    %d", pi->k);
      a->result_lines.emplace_back(r);
      std::snprintf(r, sizeof r, "prime_rank           %lld", (long long)pi->prime_rank);
      a->result_lines.emplace_back(r);
      auto parts = a->qs->LookupPartitions(pv, &err);
      for (const auto& t : parts) {
        std::snprintf(r, sizeof r, "partition            2^%d + %lld^%d", t.m_k,
                      (long long)t.q_k, t.n_k);
        a->result_lines.emplace_back(r);
      }
    } else {
      a->result_lines.emplace_back(err.empty() ? "(not present)" : err);
    }
    double dt = secs_since(t0);
    char m[96];
    std::snprintf(m, sizeof m, "lookup p=%lld in %.2fs", (long long)pv, dt);
    a->status_msg = err.empty() ? m : ("error: " + err);
  }
  a->status_glyph = err.empty() ? 'k' : '!';
  a->res_sel = a->result_lines.size() > 1 ? 1 : 0;
  a->focus = Focus::Results;
}

std::vector<Preset> make_presets() {
  return {
      Preset{"by-k  ·  primes where k == {k}, p in [{p_lo},{p_hi}], LIMIT {limit}",
             Kind::ByK, {{"k", 0}, {"p_lo", 0}, {"p_hi", 0}, {"limit", 10}}},
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
  app.presets = make_presets();
  struct ncplane* std_ = notcurses_stdplane(nc);
  ncplane_options po{};
  po.rows = 1; po.cols = 1;
  app.topbar = ncplane_create(std_, &po);
  app.query = ncplane_create(std_, &po);
  app.results = ncplane_create(std_, &po);
  app.status = ncplane_create(std_, &po);
  if (!app.topbar || !app.query || !app.results || !app.status || layout(&app) < 0) {
    notcurses_stop(nc);
    std::fprintf(stderr, "terminal too small (need >= 12x48)\n");
    return 1;
  }
  redraw(&app);

  bool running = true;
  while (running) {
    ncinput in;
    uint32_t key = notcurses_get_blocking(nc, &in);
    if (in.evtype == NCTYPE_RELEASE) continue;

    if (app.confirm_quit) {
      if (key == 'y' || key == 'Y') running = false;
      else app.confirm_quit = false;
      redraw(&app);
      continue;
    }
    switch (key) {
      case 'q': app.confirm_quit = true; break;
      case NCKEY_RESIZE: layout(&app); break;
      case NCKEY_TAB:
        app.focus = app.focus == Focus::Query ? Focus::Results : Focus::Query;
        break;
      // Unified navigation: +/- == arrows == hjkl.
      case '+': case '=': case 'j': case 'l': case NCKEY_DOWN: case NCKEY_RIGHT:
        nav(&app, +1); break;
      case '-': case 'k': case 'h': case NCKEY_UP: case NCKEY_LEFT:
        nav(&app, -1); break;
      case NCKEY_ENTER: case '\n': case '\r': run_selected(&app); break;
      default: break;
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
