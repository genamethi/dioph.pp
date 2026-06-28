// primeparts TUI — F1 Run Query screen: query + results panels, the async
// (threaded, cancellable, progress) query worker, field-edit + `c`-dispatch,
// and the history "pages" stack. See tui_app.h.

#include "primeparts/tui/tui_app.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace primeparts::tui {

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
  char title[64];
  const size_t nres = a->result_rows.empty() ? 0 : a->result_rows.size() - 1;
  if (a->history.size() > 1)
    std::snprintf(title, sizeof title, "Results (%zu)  page %zu/%zu", nres,
                  a->hist_pos + 1, a->history.size());
  else
    std::snprintf(title, sizeof title, "Results (%zu)", nres);
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

// Scroll the results selection by a page (less-style Space / b / PgUp / PgDn).
void page_results(App* a, int dir) {
  unsigned rows, cols; ncplane_dim_yx(a->results, &rows, &cols); (void)cols;
  int pg = (int)rows - 3; if (pg < 1) pg = 1;
  int n = (int)a->result_rows.size();
  if (n <= 1) return;
  int s = (int)a->res_sel + dir * pg;
  a->res_sel = (size_t)(s < 1 ? 1 : s >= n ? n - 1 : s);
}

// --- Run-Query history ("pages") -------------------------------------------
// Snapshot the just-completed query as a new page. Browser semantics: if we are
// parked on an older page, the forward pages are dropped before pushing.
void push_query_history(App* a) {
  QueryView v;
  v.preset_idx = a->preset_idx;
  if (a->preset_idx < a->presets.size()) v.fields = a->presets[a->preset_idx].fields;
  v.rows = a->result_rows;
  v.status_msg = a->status_msg;
  v.status_glyph = a->status_glyph;
  v.res_sel = a->res_sel;
  v.res_top = a->res_top;
  if (!a->history.empty() && a->hist_pos + 1 < a->history.size())
    a->history.resize(a->hist_pos + 1);  // drop forward pages
  a->history.push_back(std::move(v));
  a->hist_pos = a->history.size() - 1;
}

// Restore history page `pos`: its preset + field values + rows + selection.
void show_history_page(App* a, size_t pos) {
  if (pos >= a->history.size()) return;
  a->hist_pos = pos;
  const QueryView& v = a->history[pos];
  a->preset_idx = v.preset_idx < a->presets.size() ? v.preset_idx : 0;
  if (a->preset_idx < a->presets.size()) a->presets[a->preset_idx].fields = v.fields;
  a->result_rows = v.rows;
  a->res_sel = v.res_sel;
  a->res_top = v.res_top;
  a->cursor = 0;
  a->focus = Focus::Results;
  a->status_glyph = v.status_glyph;
  char tag[64];
  std::snprintf(tag, sizeof tag, "page %zu/%zu  ", pos + 1, a->history.size());
  a->status_msg = std::string(tag) + v.status_msg;
}

void history_back(App* a) {  // `[`
  if (a->history.empty() || a->hist_pos == 0) {
    a->status_glyph = 'i';
    a->status_msg = a->history.empty() ? "no query history yet"
                                       : "at oldest page (1/" +
                                             std::to_string(a->history.size()) + ")";
    return;
  }
  show_history_page(a, a->hist_pos - 1);
}
void history_fwd(App* a) {  // `]`
  if (a->history.empty() || a->hist_pos + 1 >= a->history.size()) {
    a->status_glyph = 'i';
    a->status_msg = a->history.empty()
                        ? "no query history yet"
                        : "at newest page (" + std::to_string(a->history.size()) +
                              "/" + std::to_string(a->history.size()) + ")";
    return;
  }
  show_history_page(a, a->hist_pos + 1);
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
  a->modal_kind = ModalKind::PresetFields;
  a->modal_buf.clear();
  for (const auto& f : p.fields) a->modal_buf.push_back(std::to_string(f.value));
  a->modal_field = a->cursor > 0 ? a->cursor - 1 : 0;
  if (a->modal_field >= p.fields.size()) a->modal_field = 0;
  a->modal_on = true;
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

}  // namespace primeparts::tui
