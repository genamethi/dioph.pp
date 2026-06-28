// primeparts/tui/tui_app.h
//
// Shared state + cross-file declarations for the multi-screen TUI. The
// implementation is split by screen across several translation units that all
// include this header:
//
//   tui_main.cc      core widgets (topbar/status/frame/row/layout/redraw), the
//                    shared modal + dispatch infra, preset load/save, main().
//   tui_runquery.cc  F1 Run Query: query/results panels, the async worker,
//                    field-edit + c-dispatch, history pages.
//   tui_generate.cc  F4 Generate: the subprocess runner + scrollable output.
//   tui_config.cc    F5 Config: config rows, warehouse modal, config I/O.
//
// All screen functions take `App*` and mutate it; selection/state changes redraw
// only the affected plane, and notcurses_render() is called once per input frame
// (in redraw()). App/Config and the small value structs live here so every TU
// sees the same layout; truly file-local helpers stay in their own .cc.

#pragma once

#include <notcurses/notcurses.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "primeparts/query/query_service.h"
#include "primeparts/tui/lua_presets.h"

namespace primeparts::tui {

namespace fs = std::filesystem;

using ::primeparts::query::QueryService;
// QueryField/QueryPreset are the shared preset model (parsed from Lua presets,
// validated by QueryService). kind is a string: "by_k" | "lookup".
using Field = ::primeparts::query::QueryField;
using Preset = ::primeparts::query::QueryPreset;
using ::primeparts::tui::LuaPresets;

// Cap on the generation output pane (trimmed to the most recent N lines).
constexpr size_t kGenMaxLines = 10000;
// Config row index whose value is a free-text path (Enter opens a text modal).
constexpr size_t kCfgWarehouse = 5;

// Top-level screens (F1..F5). Only RunQuery + Generate + Config are real.
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
  std::string field;  // schema field name ("" = not dispatchable, e.g. header)
  int64_t value = 0;
};

// One entry in the Run-Query history stack ("pages"). A completed query snapshots
// the preset + its field values + the produced rows + selection so you can step
// back to a previous query. Browser semantics: running a new query while parked
// on an older page truncates forward.
struct QueryView {
  size_t preset_idx = 0;
  std::vector<Field> fields;    // preset field values as run
  std::vector<ResultRow> rows;  // rendered result rows
  std::string status_msg;
  char status_glyph = 'i';
  size_t res_sel = 1, res_top = 1;
};

enum class Focus { Query, Results };

// Which value set the shared field-edit modal is bound to. Warehouse is a single
// free-text path box (the others are numeric boxes).
enum class ModalKind { PresetFields, GenFields, Warehouse };

struct App {
  struct notcurses* nc = nullptr;
  struct ncplane* topbar = nullptr;
  struct ncplane* query = nullptr;
  struct ncplane* results = nullptr;
  struct ncplane* status = nullptr;

  std::unique_ptr<QueryService> qs_owned;  // owns the service (swapped on
                                           // warehouse change)
  QueryService* qs = nullptr;              // == qs_owned.get()
  LuaPresets lua;                          // owns the lua_State
  std::string warehouse;                   // warehouse root (catalog + data);
                                           // editable in Config, drives both
                                           // queries and generation
  bool warehouse_dirty = false;            // changed but not yet persisted
  std::string presets_path = "scripts/lua/queries.lua";
  std::string config_path;                 // <config>/config.lua
  std::vector<Preset> presets;
  size_t preset_idx = 0;  // value of the "Preset" option
  size_t cursor = 0;      // cursored option row: 0 = Preset, 1.. = fields
  Focus focus = Focus::Query;

  Screen screen = Screen::RunQuery;
  Config cfg;
  size_t cfg_cursor = 0;  // cursored config option row

  std::vector<ResultRow> result_rows;
  size_t res_top = 0, res_sel = 0;

  // Run-Query history ("pages"): each completed query pushes a QueryView. `[` /
  // `]` step back / forward. hist_pos indexes the currently-shown page.
  std::vector<QueryView> history;
  size_t hist_pos = 0;

  // c-dispatch dialogue (reuses the modal overlay plane).
  bool disp_on = false;
  std::vector<size_t> disp_cands;  // candidate preset indices
  size_t disp_sel = 0;
  std::string disp_field;
  int64_t disp_value = 0;

  std::string status_msg = "ready";
  char status_glyph = 'i';
  bool confirm_quit = false;

  // Field-edit modal: one text box per variable field. Shared between the
  // RunQuery preset fields and the Generate parameters (modal_kind selects).
  struct ncplane* modal = nullptr;
  bool modal_on = false;
  ModalKind modal_kind = ModalKind::PresetFields;
  size_t modal_field = 0;
  std::vector<std::string> modal_buf;

  // Generation runner (F4): a `primeparts-generate` subprocess streamed into a
  // capped, scrollable output pane. Mirrors the async-query worker pattern
  // (worker thread + atomics + non-blocking UI poll), plus SIGTERM cancel.
  int64_t gen_start = 1;            // --start-idx (1-indexed FLINT prime rank)
  int64_t gen_count = 100'000'000;  // --count (primes to generate)
  int64_t gen_chunk = 500'000;      // --chunk-primes (materialization chunk)
  int64_t gen_threads = 0;          // --threads (0 = auto / hw concurrency)
  size_t gen_cursor = 0;            // cursored generate option row (0..3)
  std::thread gen_worker;
  std::atomic<bool> gen_running{false};
  std::atomic<bool> gen_done{false};
  std::atomic<int> gen_pid{-1};   // child pid while running (for SIGTERM)
  std::atomic<int> gen_exit{0};   // child exit code, valid once gen_done
  std::mutex gen_mtx;             // guards gen_out
  std::deque<std::string> gen_out;  // output lines, capped to kGenMaxLines
  size_t gen_scroll = 0;          // lines scrolled up from the tail (0=follow)

  // Async query execution: the query runs on a worker thread with cooperative
  // cancel + progress; the UI stays responsive (polls input, draws progress).
  std::thread worker;
  std::atomic<bool> q_running{false};
  std::atomic<bool> q_done{false};
  std::atomic<bool> q_cancel{false};
  std::atomic<int64_t> q_scanned{0};
  std::atomic<int64_t> q_total{0};
  std::vector<ResultRow> pending_results;  // worker -> UI; read only post q_done
  std::string worker_status;               // ditto
  char worker_glyph = 'i';                 // ditto
};

// ---- core widgets + shared infra (tui_main.cc) ------------------------------
double secs_since(std::chrono::steady_clock::time_point t0);
int64_t fval(const Preset& p, const char* name, int64_t dflt = 0);
size_t option_count(const App& a);
fs::path binary_dir();
fs::path binary_seed_path();
void row(struct ncplane* pl, int y, int x0, int w, const std::string& text,
         bool selected, bool focused, size_t idx);
void frame(struct ncplane* pl, const char* title, bool focused, unsigned* iy,
           unsigned* ix, unsigned* irows, unsigned* icols);
int layout(App* a);
void draw_topbar(App* a);
void draw_status(App* a);
void draw_stub(App* a, const char* title, const char* msg);
void redraw(App* a);
// shared modal + dispatch infra (rendering in tui_main.cc; openers per screen)
size_t modal_field_count(App* a);
void confirm_modal(App* a);
void draw_modal(App* a);
void draw_path_modal(App* a);
void draw_dispatch(App* a);
// preset I/O
std::vector<Preset> built_in_presets();
void load_presets(App* a);
void save_current(App* a);

// ---- F1 Run Query (tui_runquery.cc) -----------------------------------------
std::string option_value(const App& a, size_t oi);
std::string option_name(const App& a, size_t oi);
void draw_query(App* a);
void draw_results(App* a);
void page_results(App* a, int dir);
void nav(App* a, int delta);
void cycle_value(App* a, int delta);
void push_query_history(App* a);
void show_history_page(App* a, size_t pos);
void history_back(App* a);
void history_fwd(App* a);
void run_query_worker(App* a, Preset p);
void start_query(App* a);
void open_modal(App* a);
void open_dispatch(App* a);
void confirm_dispatch(App* a);

// ---- F4 Generate (tui_generate.cc) ------------------------------------------
size_t gen_opt_count();
std::string gen_name(size_t i);
int64_t gen_field(const App& a, size_t i);
std::string gen_value(const App& a, size_t i);
void gen_adjust(App* a, size_t i, int d);
void gen_push(App* a, const std::string& line);
void gen_run_worker(App* a, std::string exe, std::vector<std::string> argv_s);
void start_generate(App* a);
void gen_scroll_by(App* a, int delta);
void gen_page(App* a, int dir);
void draw_generate(App* a);
void draw_gen_output(App* a);
void open_gen_modal(App* a);

// ---- F5 Config (tui_config.cc) ----------------------------------------------
size_t cfg_count();
std::string cfg_name(size_t i);
std::string cfg_value(const App& a, size_t i);
void cfg_adjust(App* a, size_t i, int d);
void draw_config(App* a);
void open_warehouse_modal(App* a);
bool reopen_warehouse(App* a, const std::string& path);
void apply_config_kv(App* a, const std::map<std::string, std::string>& kv);
std::map<std::string, std::string> config_to_kv(const Config& c);
void run_janitor(App* a);
void load_config(App* a);
void save_config(App* a);

}  // namespace primeparts::tui
