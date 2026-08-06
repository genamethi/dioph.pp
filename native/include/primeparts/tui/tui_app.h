#pragma once

#include <notcurses/notcurses.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "iceberg/table_identifier.h"

#include "primeparts/config.h"
#include "primeparts/query/query_service.h"
#include "primeparts/tui/lua_presets.h"

namespace primeparts::tui {

namespace fs = std::filesystem;

using ::primeparts::query::QueryService;
using Field = ::primeparts::query::QueryField;
using Preset = ::primeparts::query::QueryPreset;
using ::primeparts::tui::LuaPresets;

constexpr size_t kGenMaxLines = 10000;

enum class Screen { RunQuery, MakeQuery, Status, Generate, Config };

struct ResultRow {
  std::string text;
  std::string field;
  int64_t value = 0;
};

struct QueryView {
  size_t preset_idx = 0;
  std::vector<Field> fields;
  std::vector<ResultRow> rows;
  std::string status_msg;
  char status_glyph = 'i';
  size_t res_sel = 1, res_top = 1;
};

enum class Focus { Query, Results };

enum class ModalKind { PresetFields, GenFields };

struct App {
  struct notcurses* nc = nullptr;
  struct ncplane* topbar = nullptr;
  struct ncplane* query = nullptr;
  struct ncplane* results = nullptr;
  struct ncplane* status = nullptr;

  std::unique_ptr<QueryService> qs_owned;
  QueryService* qs = nullptr;
  LuaPresets lua;
  std::string warehouse;
  std::string rest_uri;
  iceberg::Namespace ns;
  std::string presets_path = "scripts/lua/queries.lua";
  std::string config_path;
  config::Conf conf;
  std::vector<Preset> presets;
  size_t preset_idx = 0;
  size_t cursor = 0;
  Focus focus = Focus::Query;

  Screen screen = Screen::RunQuery;

  std::vector<ResultRow> result_rows;
  size_t res_top = 0, res_sel = 0;

  std::vector<QueryView> history;
  size_t hist_pos = 0;

  bool disp_on = false;
  std::vector<size_t> disp_cands;
  size_t disp_sel = 0;
  std::string disp_field;
  int64_t disp_value = 0;

  std::string status_msg = "ready";
  char status_glyph = 'i';
  bool confirm_quit = false;

  struct ncplane* modal = nullptr;
  bool modal_on = false;
  ModalKind modal_kind = ModalKind::PresetFields;
  size_t modal_field = 0;
  std::vector<std::string> modal_buf;

  int64_t gen_start = 1;
  int64_t gen_count = 100'000'000;
  int64_t gen_chunk = 0;
  int64_t gen_threads = 0;
  size_t gen_cursor = 0;
  std::thread gen_worker;
  std::atomic<bool> gen_running{false};
  std::atomic<bool> gen_done{false};
  std::atomic<int> gen_pid{-1};
  std::atomic<int> gen_exit{0};
  std::mutex gen_mtx;
  std::deque<std::string> gen_out;
  size_t gen_scroll = 0;

  std::thread worker;
  std::atomic<bool> q_running{false};
  std::atomic<bool> q_done{false};
  std::atomic<bool> q_cancel{false};
  std::atomic<int64_t> q_scanned{0};
  std::atomic<int64_t> q_total{0};
  std::vector<ResultRow> pending_results;
  std::string worker_status;
  char worker_glyph = 'i';
};

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
size_t modal_field_count(App* a);
void confirm_modal(App* a);
void draw_modal(App* a);
void draw_dispatch(App* a);
std::vector<Preset> built_in_presets();
void load_presets(App* a);
void save_current(App* a);

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

void draw_config(App* a);
bool reopen_warehouse(App* a, const std::string& path);
void run_janitor(App* a);
void edit_config(App* a);

}  // namespace primeparts::tui
