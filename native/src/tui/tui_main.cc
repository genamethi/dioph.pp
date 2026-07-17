#include "primeparts/tui/tui_app.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "primeparts/catalog/pp_iceberg_rest.h"

namespace primeparts::tui {

namespace {

constexpr char kDefaultWarehouse[] =
    "/media/extssd/research/dioph.pp/data/ib-staging";

fs::path config_presets_path() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg) return fs::path(xdg) / "primeparts" / "queries.lua";
  const char* home = std::getenv("HOME");
  if (home && *home) return fs::path(home) / ".config" / "primeparts" / "queries.lua";
  return fs::path(".primeparts-queries.lua");
}

fs::path config_file_path() {
  return config_presets_path().parent_path() / "config.lua";
}

}  // namespace

fs::path binary_dir() {
  char buf[4096];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) return {};
  buf[n] = '\0';
  return fs::path(buf).parent_path();
}

fs::path binary_seed_path() {
  fs::path dir = binary_dir();
  if (dir.empty()) return {};
  std::error_code ec;
  fs::path seed = dir / ".." / ".." / "scripts" / "lua" / "queries.lua";
  fs::path c = fs::weakly_canonical(seed, ec);
  return ec ? seed : c;
}

double secs_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
int64_t fval(const Preset& p, const char* name, int64_t dflt) {
  for (const auto& f : p.fields)
    if (f.name == name) return f.value;
  return dflt;
}
size_t option_count(const App& a) {
  return 1 + a.presets[a.preset_idx].fields.size();
}

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
  if (a->screen == Screen::RunQuery || a->screen == Screen::Generate) {
    unsigned qh = body * 3 / 10;
    const unsigned qmin = a->screen == Screen::Generate ? 8 : 6;
    if (qh < qmin) qh = qmin;
    ncplane_resize_simple(a->query, qh, cols);
    ncplane_move_yx(a->query, 1, 0);
    ncplane_resize_simple(a->results, body - qh, cols);
    ncplane_move_yx(a->results, 1 + qh, 0);
  } else {
    ncplane_resize_simple(a->query, body, cols);
    ncplane_move_yx(a->query, 1, 0);
    ncplane_resize_simple(a->results, 1, cols);
    ncplane_move_yx(a->results, rows - 1, 0);
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
  const int active = static_cast<int>(a->screen);
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
  const char* keys = "jk:move +/-:val Enter:edit c:drill [/]:page s:save R:reload Tab:panel q:quit";
  size_t kl = std::strlen(keys);
  if (cols > kl + 8) {
    ncplane_set_fg_rgb8(a->status, 0x99, 0x99, 0xaa);
    ncplane_putstr_yx(a->status, 0, (int)(cols - kl - 1), keys);
    ncplane_set_fg_default(a->status);
  }
  ncplane_set_bg_default(a->status);
}

size_t modal_field_count(App* a) {
  if (a->modal_kind == ModalKind::Warehouse) return 1;
  return a->modal_kind == ModalKind::GenFields
             ? gen_opt_count()
             : a->presets[a->preset_idx].fields.size();
}

void confirm_modal(App* a) {
  if (a->modal_kind == ModalKind::Warehouse) {
    std::string path = a->modal_buf.empty() ? "" : a->modal_buf[0];
    a->modal_on = false;
    a->modal_kind = ModalKind::PresetFields;
    if (!path.empty() && path != a->warehouse) reopen_warehouse(a, path);
    return;
  }
  if (a->modal_kind == ModalKind::GenFields) {
    auto box = [&](size_t i, int64_t lo) -> int64_t {
      const std::string& b = i < a->modal_buf.size() ? a->modal_buf[i] : "";
      int64_t v = b.empty() ? 0 : std::strtoll(b.c_str(), nullptr, 10);
      return v < lo ? lo : v;
    };
    a->gen_start = box(0, 1);
    a->gen_count = box(1, 1);
    a->gen_chunk = box(2, 1);
    a->gen_threads = box(3, 0);
    a->modal_on = false;
    a->modal_kind = ModalKind::PresetFields;
    return;
  }
  Preset& p = a->presets[a->preset_idx];
  for (size_t i = 0; i < p.fields.size() && i < a->modal_buf.size(); ++i) {
    const std::string& b = a->modal_buf[i];
    p.fields[i].value = b.empty() ? 0 : std::strtoll(b.c_str(), nullptr, 10);
  }
  a->modal_on = false;
  start_query(a);
}

void draw_path_modal(App* a) {
  unsigned trows, tcols;
  notcurses_term_dim_yx(a->nc, &trows, &tcols);
  unsigned w = tcols < 72 ? (tcols > 8 ? tcols - 4 : 8) : 68;
  unsigned h = 6;
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
  ncplane_printf_yx(a->modal, 0, 2, "┤ warehouse path ├");
  ncplane_set_styles(a->modal, NCSTYLE_NONE);
  static const std::string kEmpty;
  const std::string& buf = a->modal_buf.empty() ? kEmpty : a->modal_buf[0];
  int boxw = (int)w - 6;
  std::string shown = buf;
  if ((int)shown.size() > boxw) shown = "…" + shown.substr(shown.size() - (boxw - 1));
  ncplane_set_bg_rgb8(a->modal, 0x2c, 0x44, 0x66);
  ncplane_set_fg_rgb8(a->modal, 0xff, 0xff, 0xff);
  ncplane_printf_yx(a->modal, 2, 2, " %-*s", boxw, shown.c_str());
  ncplane_set_bg_rgb8(a->modal, 0x20, 0x24, 0x30);
  ncplane_set_fg_rgb8(a->modal, 0x99, 0x99, 0xaa);
  ncplane_printf_yx(a->modal, (int)(h - 2), 2,
                    "Enter:apply (reopens queries)  Esc:cancel  Backspace:edit");
  ncplane_set_fg_default(a->modal);
  ncplane_set_bg_default(a->modal);
}

void draw_modal(App* a) {
  if (a->modal_kind == ModalKind::Warehouse) { draw_path_modal(a); return; }
  const bool gen = a->modal_kind == ModalKind::GenFields;
  const Preset* p = gen ? nullptr : &a->presets[a->preset_idx];
  unsigned trows, tcols;
  notcurses_term_dim_yx(a->nc, &trows, &tcols);
  const unsigned nf = (unsigned)modal_field_count(a);
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
  if (gen) ncplane_printf_yx(a->modal, 0, 2, "┤ generate parameters ├");
  else ncplane_printf_yx(a->modal, 0, 2, "┤ set fields: %s ├", p->id.c_str());
  ncplane_set_styles(a->modal, NCSTYLE_NONE);
  for (unsigned i = 0; i < nf; ++i) {
    const bool foc = (i == a->modal_field);
    if (foc) {
      ncplane_set_styles(a->modal, NCSTYLE_BOLD);
      ncplane_set_bg_rgb8(a->modal, 0x2c, 0x44, 0x66);
      ncplane_set_fg_rgb8(a->modal, 0xff, 0xff, 0xff);
    }
    const std::string label = gen ? gen_name(i) : p->fields[i].name;
    ncplane_printf_yx(a->modal, (int)(2 + i), 2, "%s%-12s [%-14s]",
                      foc ? "> " : "  ", label.c_str(),
                      a->modal_buf[i].c_str());
    ncplane_set_styles(a->modal, NCSTYLE_NONE);
    ncplane_set_bg_rgb8(a->modal, 0x20, 0x24, 0x30);
    ncplane_set_fg_default(a->modal);
  }
  ncplane_set_fg_rgb8(a->modal, 0x99, 0x99, 0xaa);
  ncplane_printf_yx(a->modal, (int)(h - 2), 2,
                    gen ? "Enter:set  Esc/b:cancel  digits:edit  j/k:field"
                        : "Enter:run  Esc/b:cancel  digits:edit  j/k:field");
  ncplane_set_fg_default(a->modal);
  ncplane_set_bg_default(a->modal);
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
    case Screen::Generate: draw_generate(a); draw_gen_output(a); break;
  }
  draw_status(a);
  if (a->modal_on) draw_modal(a);
  else if (a->disp_on) draw_dispatch(a);
  else ncplane_erase(a->modal);
  notcurses_render(a->nc);
}

std::vector<Preset> built_in_presets() {
  return {
      Preset{"by-k", "primes where k == {k}, p in [{p_lo},{p_hi}], LIMIT {limit}",
             "by_k", {{"k", 0}, {"p_lo", 0}, {"p_hi", 0}, {"limit", 10}},
             {"k"}, "k"},
      Preset{"lookup", "prime p == {p}  ->  k + partitions", "lookup",
             {{"p", 11}}, {"p", "q_k"}, "p"},
  };
}

void load_presets(App* a) {
  std::vector<std::string> errs;
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
    if (have(p.id)) continue;
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
  if (LuaPresets::SaveAll(a->presets, a->presets_path, &se)) {
    a->status_glyph = 'k';
    a->status_msg = "saved " + std::to_string(a->presets.size()) +
                    " presets -> " + a->presets_path;
  } else {
    a->status_glyph = '!';
    a->status_msg = "save failed: " + se;
  }
}

}  // namespace primeparts::tui

int main(int argc, char** argv) {
  using namespace primeparts::tui;
  App app;
  app.presets_path = config_presets_path().string();
  app.config_path = config_file_path().string();
  app.ns = primeparts::catalog::ResolveNamespace("");
  load_config(&app);

  std::string warehouse = argc >= 2          ? argv[1]
                          : !app.warehouse.empty() ? app.warehouse
                                                   : kDefaultWarehouse;
  std::string err;
  auto qs = QueryService::Open(warehouse, app.ns, &err);
  if (!qs) {
    std::fprintf(stderr, "QueryService::Open(%s): %s\n", warehouse.c_str(), err.c_str());
    return 1;
  }
  notcurses_options opts{};
  opts.flags = NCOPTION_SUPPRESS_BANNERS;
  struct notcurses* nc = notcurses_core_init(&opts, nullptr);
  if (!nc) { std::fprintf(stderr, "notcurses_core_init failed\n"); return 1; }

  app.nc = nc;
  app.qs_owned = std::move(qs);
  app.qs = app.qs_owned.get();
  app.warehouse = warehouse;
  load_presets(&app);
  struct ncplane* std_ = notcurses_stdplane(nc);
  ncplane_options po{}; po.rows = 1; po.cols = 1;
  app.topbar = ncplane_create(std_, &po);
  app.query = ncplane_create(std_, &po);
  app.results = ncplane_create(std_, &po);
  app.status = ncplane_create(std_, &po);
  app.modal = ncplane_create(std_, &po);
  if (!app.topbar || !app.query || !app.results || !app.status || !app.modal ||
      layout(&app) < 0) {
    notcurses_stop(nc);
    std::fprintf(stderr, "terminal too small (need >= 12x48)\n");
    return 1;
  }
  redraw(&app);

  bool running = true;
  while (running) {
    if (app.q_running.load()) {
      struct timespec ts{0, 60'000'000};
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
        app.res_top = 1;
        push_query_history(&app);
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

    if (app.gen_running.load()) {
      struct timespec ts{0, 60'000'000};
      ncinput pin;
      uint32_t pk = notcurses_get(nc, &ts, &pin);
      if (pk != 0 && pin.evtype != NCTYPE_RELEASE) {
        if (pk == NCKEY_ESC) {
          int pid = app.gen_pid.load();
          if (pid > 0) ::kill(pid, SIGTERM);
        } else if (pk == 'k' || pk == NCKEY_UP) {
          gen_scroll_by(&app, +1);
        } else if (pk == 'j' || pk == NCKEY_DOWN) {
          gen_scroll_by(&app, -1);
        } else if (pk == 'b' || pk == NCKEY_PGUP) {
          gen_page(&app, +1);
        } else if (pk == ' ' || pk == NCKEY_PGDOWN) {
          gen_page(&app, -1);
        }
      }
      if (app.gen_done.load(std::memory_order_acquire)) {
        app.gen_worker.join();
        app.gen_running.store(false);
        int ec = app.gen_exit.load();
        app.status_glyph = ec == 0 ? 'k' : '!';
        app.status_msg = ec == 0 ? "generation complete (committed)"
                                 : ("generation exited " + std::to_string(ec));
      } else {
        size_t ln;
        { std::lock_guard<std::mutex> lk(app.gen_mtx); ln = app.gen_out.size(); }
        char m[80];
        std::snprintf(m, sizeof m, "generating... (%zu lines)  — Esc terminate", ln);
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
    if (app.modal_on && app.modal_kind == ModalKind::Warehouse) {
      if (key == NCKEY_ESC) app.modal_on = false;
      else if (key == NCKEY_ENTER || key == '\n' || key == '\r') confirm_modal(&app);
      else if (key == NCKEY_BACKSPACE || key == NCKEY_DEL || key == 127 || key == 8) {
        if (!app.modal_buf.empty() && !app.modal_buf[0].empty())
          app.modal_buf[0].pop_back();
      } else if (key >= 0x20 && key <= 0x7e) {
        if (!app.modal_buf.empty() && app.modal_buf[0].size() < 1024)
          app.modal_buf[0].push_back((char)key);
      }
      redraw(&app); continue;
    }
    if (app.modal_on) {
      const size_t nf = modal_field_count(&app);
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
        app.modal_buf[app.modal_field].push_back('-');
      }
      redraw(&app); continue;
    }
    if (app.disp_on) {
      const size_t ncd = app.disp_cands.size();
      if (key == NCKEY_ESC || key == 'b') app.disp_on = false;
      else if (key == NCKEY_ENTER || key == '\n' || key == '\r') confirm_dispatch(&app);
      else if (key == 'j' || key == NCKEY_DOWN) app.disp_sel = ncd ? (app.disp_sel + 1) % ncd : 0;
      else if (key == 'k' || key == NCKEY_UP)   app.disp_sel = ncd ? (app.disp_sel + ncd - 1) % ncd : 0;
      redraw(&app); continue;
    }
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
    if (app.screen == Screen::Config) {
      const size_t n = cfg_count();
      if ((key == 'j' || key == NCKEY_DOWN) && app.cfg_cursor + 1 < n) ++app.cfg_cursor;
      else if ((key == 'k' || key == NCKEY_UP) && app.cfg_cursor > 0) --app.cfg_cursor;
      else if (key == '+' || key == '=') cfg_adjust(&app, app.cfg_cursor, +1);
      else if (key == '-') cfg_adjust(&app, app.cfg_cursor, -1);
      else if ((key == NCKEY_ENTER || key == '\n' || key == '\r') &&
               app.cfg_cursor == kCfgWarehouse)
        open_warehouse_modal(&app);
      else if (key == 's') save_config(&app);
      redraw(&app); continue;
    }
    if (app.screen == Screen::Generate) {
      switch (key) {
        case NCKEY_TAB:
          app.focus = app.focus == Focus::Query ? Focus::Results : Focus::Query;
          break;
        case 'j': case NCKEY_DOWN:
          if (app.focus == Focus::Query) {
            if (app.gen_cursor + 1 < gen_opt_count()) ++app.gen_cursor;
          } else gen_scroll_by(&app, -1);
          break;
        case 'k': case NCKEY_UP:
          if (app.focus == Focus::Query) {
            if (app.gen_cursor > 0) --app.gen_cursor;
          } else gen_scroll_by(&app, +1);
          break;
        case '+': case '=':
          if (app.focus == Focus::Query) gen_adjust(&app, app.gen_cursor, +1);
          break;
        case '-':
          if (app.focus == Focus::Query) gen_adjust(&app, app.gen_cursor, -1);
          break;
        case ' ': case NCKEY_PGDOWN: gen_page(&app, -1); break;
        case 'b': case NCKEY_PGUP:   gen_page(&app, +1); break;
        case NCKEY_ENTER: case '\n': case '\r':
          if (app.focus == Focus::Query) open_gen_modal(&app);
          break;
        case 'g': start_generate(&app); break;
        default: break;
      }
      redraw(&app); continue;
    }
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
      case NCKEY_ENTER: case '\n': case '\r':
        if (app.focus == Focus::Query) open_modal(&app);
        break;
      case 'c':
        if (app.focus == Focus::Results) open_dispatch(&app);
        break;
      case '[': history_back(&app); break;
      case ']': history_fwd(&app); break;
      case 's': save_current(&app); break;
      case 'R': load_presets(&app); break;
      default: break;
    }
    redraw(&app);
  }

  if (app.worker.joinable()) {
    app.q_cancel.store(true);
    app.worker.join();
  }
  if (app.gen_worker.joinable()) {
    int pid = app.gen_pid.load();
    if (pid > 0) ::kill(pid, SIGTERM);
    app.gen_worker.join();
  }
  ncplane_destroy(app.topbar);
  ncplane_destroy(app.query);
  ncplane_destroy(app.results);
  ncplane_destroy(app.status);
  ncplane_destroy(app.modal);
  notcurses_stop(nc);
  return 0;
}
