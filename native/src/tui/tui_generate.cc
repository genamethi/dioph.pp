// primeparts TUI — F4 Generate screen: the `primeparts-generate` subprocess
// runner + scrollable output pane. See primeparts/tui/tui_app.h.

#include "primeparts/tui/tui_app.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>  // waitpid (generation subprocess)
#include <unistd.h>    // fork/exec, pipe, dup2

namespace primeparts::tui {

// --- Generate screen -------------------------------------------------------
// Four `primeparts-generate` parameters, edited with +/- (coarse steps) or a
// precise numeric modal (Enter). The runner streams the subprocess output.
size_t gen_opt_count() { return 4; }
std::string gen_name(size_t i) {
  static const char* n[] = {"start-idx", "count", "chunk-primes", "threads"};
  return i < 4 ? n[i] : "";
}
int64_t gen_field(const App& a, size_t i) {
  switch (i) {
    case 0: return a.gen_start;
    case 1: return a.gen_count;
    case 2: return a.gen_chunk;
    case 3: return a.gen_threads;
  }
  return 0;
}
std::string gen_value(const App& a, size_t i) {
  if (i == 3 && a.gen_threads == 0) return "auto";
  return std::to_string(gen_field(a, i));
}
void gen_adjust(App* a, size_t i, int d) {
  switch (i) {
    case 0: a->gen_start = std::max<int64_t>(1, a->gen_start + (int64_t)d * 1'000'000); break;
    case 1: a->gen_count = std::max<int64_t>(1, a->gen_count + (int64_t)d * 10'000'000); break;
    case 2: a->gen_chunk = std::max<int64_t>(1000, a->gen_chunk + (int64_t)d * 100'000); break;
    case 3: a->gen_threads = std::max<int64_t>(0, a->gen_threads + d); break;
  }
}

// Append a line to the (mutex-guarded) generation output, trimming to the cap.
void gen_push(App* a, const std::string& line) {
  std::lock_guard<std::mutex> lk(a->gen_mtx);
  a->gen_out.push_back(line);
  while (a->gen_out.size() > kGenMaxLines) a->gen_out.pop_front();
}

// Runs ON THE WORKER THREAD: fork+exec primeparts-generate, stream its merged
// stdout/stderr into gen_out line by line, then publish gen_done (release). The
// UI reads gen_exit only after observing gen_done (acquire).
void gen_run_worker(App* a, std::string exe, std::vector<std::string> argv_s) {
  int fds[2];
  if (::pipe(fds) != 0) {
    gen_push(a, "error: pipe() failed");
    a->gen_exit.store(-1);
    a->gen_done.store(true, std::memory_order_release);
    return;
  }
  pid_t pid = ::fork();
  if (pid < 0) {
    gen_push(a, "error: fork() failed");
    ::close(fds[0]); ::close(fds[1]);
    a->gen_exit.store(-1);
    a->gen_done.store(true, std::memory_order_release);
    return;
  }
  if (pid == 0) {  // child: merge stdout+stderr into the pipe, then exec
    ::dup2(fds[1], STDOUT_FILENO);
    ::dup2(fds[1], STDERR_FILENO);
    ::close(fds[0]); ::close(fds[1]);
    std::vector<char*> av;
    av.reserve(argv_s.size() + 2);
    av.push_back(exe.data());
    for (auto& s : argv_s) av.push_back(s.data());
    av.push_back(nullptr);
    ::execv(exe.c_str(), av.data());
    ::_exit(127);  // exec failed
  }
  ::close(fds[1]);
  a->gen_pid.store(pid);
  if (FILE* fp = ::fdopen(fds[0], "r")) {
    char buf[8192];
    while (std::fgets(buf, sizeof buf, fp)) {
      size_t n = std::strlen(buf);
      while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
      gen_push(a, buf);
    }
    std::fclose(fp);
  } else {
    ::close(fds[0]);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  int ec = WIFEXITED(status)    ? WEXITSTATUS(status)
           : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                 : -1;
  a->gen_pid.store(-1);
  a->gen_exit.store(ec);
  a->gen_done.store(true, std::memory_order_release);
}

// Launch the generation subprocess (UI stays live). No-op if one is running.
void start_generate(App* a) {
  if (a->gen_running.load()) return;
  fs::path exe = binary_dir() / "primeparts-generate";
  if (exe.empty() || !fs::exists(exe)) {
    a->status_glyph = '!';
    a->status_msg = "primeparts-generate not found next to the TUI binary";
    return;
  }
  std::vector<std::string> argv = {"--start-idx", std::to_string(a->gen_start),
                                   "--count", std::to_string(a->gen_count),
                                   "--chunk-primes", std::to_string(a->gen_chunk)};
  if (a->gen_threads > 0) {
    argv.push_back("--threads");
    argv.push_back(std::to_string(a->gen_threads));
  }
  argv.push_back("--warehouse");
  argv.push_back(a->warehouse);
  { std::lock_guard<std::mutex> lk(a->gen_mtx); a->gen_out.clear(); }
  a->gen_scroll = 0;
  std::string cmd = "$ primeparts-generate";
  for (const auto& s : argv) cmd += ' ' + s;
  gen_push(a, cmd);
  gen_push(a, "(commits primes+partitions through the catalog at end of run;"
              " Esc terminates and does NOT commit partial work)");
  a->gen_done.store(false);
  a->gen_exit.store(0);
  a->gen_running.store(true);
  a->focus = Focus::Results;  // follow the streaming output
  a->gen_worker = std::thread(gen_run_worker, a, exe.string(), std::move(argv));
}

// Scroll the output pane. delta > 0 scrolls toward older lines (up), < 0 toward
// the tail (down). gen_scroll is the line distance from the tail (0 = follow).
void gen_scroll_by(App* a, int delta) {
  size_t total;
  { std::lock_guard<std::mutex> lk(a->gen_mtx); total = a->gen_out.size(); }
  int s = (int)a->gen_scroll + delta;
  if (s < 0) s = 0;
  if ((size_t)s > total) s = (int)total;
  a->gen_scroll = (size_t)s;
}
void gen_page(App* a, int dir) {  // dir > 0 = older, < 0 = newer
  unsigned rows, cols; ncplane_dim_yx(a->results, &rows, &cols); (void)cols;
  int pg = (int)rows - 3; if (pg < 1) pg = 1;
  gen_scroll_by(a, dir * pg);
}

void draw_generate(App* a) {
  const bool foc = a->focus == Focus::Query;
  unsigned iy, ix, irows, icols;
  frame(a->query, "Generate", foc, &iy, &ix, &irows, &icols);
  if (irows == 0) return;
  ncplane_set_fg_rgb8(a->query, 0x77, 0x77, 0x88);
  ncplane_printf_yx(a->query, (int)iy, (int)ix, "%.*s", (int)icols,
                    "+/- adjust   Enter edit   g run   Tab output   Esc terminate");
  ncplane_set_fg_default(a->query);
  for (size_t i = 0; i < gen_opt_count() && iy + 1 + i < iy + irows; ++i) {
    char line[96];
    std::snprintf(line, sizeof line, "%-14s %s", gen_name(i).c_str(),
                  gen_value(*a, i).c_str());
    row(a->query, (int)(iy + 1 + i), (int)ix, (int)icols, line,
        i == a->gen_cursor, foc, i);
  }
}

void draw_gen_output(App* a) {
  const bool foc = a->focus == Focus::Results;
  std::vector<std::string> view;
  size_t total;
  {
    std::lock_guard<std::mutex> lk(a->gen_mtx);
    total = a->gen_out.size();
    view.assign(a->gen_out.begin(), a->gen_out.end());
  }
  char title[48];
  std::snprintf(title, sizeof title, "Output (%zu)", total);
  unsigned iy, ix, irows, icols;
  frame(a->results, title, foc, &iy, &ix, &irows, &icols);
  if (irows == 0) return;
  if (view.empty()) {
    ncplane_set_fg_rgb8(a->results, 0x77, 0x77, 0x77);
    ncplane_printf_yx(a->results, (int)iy, (int)ix,
                      "(idle — set parameters above, then press g to run)");
    ncplane_set_fg_default(a->results);
    return;
  }
  const unsigned list_rows = irows;
  const size_t shown = list_rows < total ? list_rows : total;
  const size_t max_scroll = total - shown;
  if (a->gen_scroll > max_scroll) a->gen_scroll = max_scroll;
  const size_t top = total - shown - a->gen_scroll;
  for (unsigned r = 0; r < shown; ++r)
    ncplane_printf_yx(a->results, (int)(iy + r), (int)ix, "%-*.*s", (int)icols,
                      (int)icols, view[top + r].c_str());
  unsigned prows, pcols; ncplane_dim_yx(a->results, &prows, &pcols);
  if (top > 0) ncplane_putstr_yx(a->results, 1, pcols - 1, "▲");
  if (top + shown < total) ncplane_putstr_yx(a->results, prows - 2, pcols - 1, "▼");
}

// Open the shared modal bound to the four generate parameters (precise entry).
void open_gen_modal(App* a) {
  a->modal_kind = ModalKind::GenFields;
  a->modal_buf = {std::to_string(a->gen_start), std::to_string(a->gen_count),
                  std::to_string(a->gen_chunk), std::to_string(a->gen_threads)};
  a->modal_field = a->gen_cursor < 4 ? a->gen_cursor : 0;
  a->modal_on = true;
}

}  // namespace primeparts::tui
