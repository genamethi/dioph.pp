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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
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
  std::string id;     // short, shown as the "Preset" option value
  std::string desc;   // human description (shown dim)
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
  size_t preset_idx = 0;  // value of the "Preset" option
  size_t cursor = 0;      // cursored option row: 0 = Preset, 1.. = fields
  Focus focus = Focus::Query;

  std::vector<std::string> result_lines;
  size_t res_top = 0, res_sel = 0;

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
  std::vector<std::string> pending_results;  // worker -> UI; read only post q_done
  std::string worker_status;                 // ditto
  char worker_glyph = 'i';                    // ditto
};

void draw_modal(App* a);
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
  unsigned qh = body * 3 / 10;
  if (qh < 6) qh = 6;
  ncplane_resize_simple(a->query, qh, cols);
  ncplane_move_yx(a->query, 1, 0);
  ncplane_resize_simple(a->results, body - qh, cols);
  ncplane_move_yx(a->results, 1 + qh, 0);
  ncplane_resize_simple(a->status, 1, cols);
  ncplane_move_yx(a->status, rows - 1, 0);
  return 0;
}

void draw_topbar(App* a) {
  ncplane_erase(a->topbar);
  unsigned rows, cols; ncplane_dim_yx(a->topbar, &rows, &cols); (void)rows;
  ncplane_set_bg_rgb8(a->topbar, 0x22, 0x22, 0x2c);
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
                a->result_lines.empty() ? 0 : a->result_lines.size() - 1);
  unsigned iy, ix, irows, icols;
  frame(a->results, title, foc, &iy, &ix, &irows, &icols);
  if (irows == 0) return;

  if (a->result_lines.empty()) {
    ncplane_set_fg_rgb8(a->results, 0x77, 0x77, 0x77);
    ncplane_printf_yx(a->results, (int)iy, (int)ix, "(no results yet)");
    ncplane_set_fg_default(a->results);
    return;
  }
  ncplane_set_styles(a->results, NCSTYLE_BOLD);
  ncplane_set_fg_rgb8(a->results, 0x88, 0xcc, 0xcc);
  ncplane_printf_yx(a->results, (int)iy, (int)ix, "%-*s", (int)icols,
                    a->result_lines[0].c_str());
  ncplane_set_styles(a->results, NCSTYLE_NONE);
  ncplane_set_fg_default(a->results);

  const unsigned list_rows = irows - 1;
  const size_t n = a->result_lines.size();
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
  unsigned prows, pcols; ncplane_dim_yx(a->results, &prows, &pcols);
  if (a->res_top > 1) ncplane_putstr_yx(a->results, 1, pcols - 1, "▲");
  if (a->res_top + list_rows < n)
    ncplane_putstr_yx(a->results, prows - 2, pcols - 1, "▼");
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
  const char* keys = "↑↓/jk:move  +/-:value  Enter:edit  Space/b:page  Tab:panel  q:quit";
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
  if (a->modal_on) draw_modal(a);
  else ncplane_erase(a->modal);  // transparent when closed
  notcurses_render(a->nc);
}

// Scroll the results selection by a page (less-style Space / b / PgUp / PgDn).
void page_results(App* a, int dir) {
  unsigned rows, cols; ncplane_dim_yx(a->results, &rows, &cols); (void)cols;
  int pg = (int)rows - 3; if (pg < 1) pg = 1;
  int n = (int)a->result_lines.size();
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
    int n = (int)a->result_lines.size();
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
  std::vector<std::string> out;
  primeparts::query::ScanControl ctl;
  ctl.cancel = &a->q_cancel;
  ctl.progress = [a](int64_t sc, int64_t tot) {
    a->q_scanned.store(sc, std::memory_order_relaxed);
    a->q_total.store(tot, std::memory_order_relaxed);
  };
  auto t0 = std::chrono::steady_clock::now();
  if (p.kind == Kind::ByK) {
    auto hits = a->qs->ScanByK((int32_t)fval(p, "k"), fval(p, "p_lo"),
                               fval(p, "p_hi"), fval(p, "limit", 10), &err, ctl);
    char hdr[80]; std::snprintf(hdr, sizeof hdr, "%-20s %s", "p", "prime_rank");
    out.emplace_back(hdr);
    for (const auto& h : hits) {
      char r[96]; std::snprintf(r, sizeof r, "%-20lld %lld", (long long)h.p,
                                (long long)h.prime_rank);
      out.emplace_back(r);
    }
    double dt = secs_since(t0);
    char m[128]; std::snprintf(m, sizeof m, "k==%lld -> %zu hits in %.2fs%s",
                               (long long)fval(p, "k"), hits.size(), dt,
                               a->q_cancel.load() ? " (cancelled)" : "");
    a->worker_status = err.empty() ? m : ("error: " + err);
  } else {
    int64_t pv = fval(p, "p", 0);
    auto pi = a->qs->LookupPrime(pv, &err, ctl);
    out.emplace_back("field                value");
    if (pi) {
      char r[96];
      std::snprintf(r, sizeof r, "k                    %d", pi->k);
      out.emplace_back(r);
      std::snprintf(r, sizeof r, "prime_rank           %lld", (long long)pi->prime_rank);
      out.emplace_back(r);
      for (const auto& t : a->qs->LookupPartitions(pv, &err, ctl)) {
        std::snprintf(r, sizeof r, "partition            2^%d + %lld^%d", t.m_k,
                      (long long)t.q_k, t.n_k);
        out.emplace_back(r);
      }
    } else {
      out.emplace_back(err.empty() ? "(not present)" : err);
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
  a->result_lines.clear();
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

std::vector<Preset> make_presets() {
  return {
      Preset{"by-k", "primes where k == {k}, p in [{p_lo},{p_hi}], LIMIT {limit}",
             Kind::ByK, {{"k", 0}, {"p_lo", 0}, {"p_hi", 0}, {"limit", 10}}},
      Preset{"lookup", "prime p == {p}  ->  k + partitions", Kind::Lookup,
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
  app.nc = nc; app.qs = qs.get(); app.presets = make_presets();
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
        app.result_lines = std::move(app.pending_results);
        app.status_msg = app.worker_status;
        app.status_glyph = app.worker_glyph;
        app.res_sel = app.result_lines.size() > 1 ? 1 : 0;
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
