#include "primeparts/tui/tui_app.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <system_error>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "primeparts/catalog/pp_iceberg_rest.h"

namespace primeparts::tui {

namespace {

std::vector<std::string> config_lines(const App& a) {
  const config::Conf& c = a.conf;
  return {
      "file        " + a.config_path,
      "touched     " + std::string(c.touched ? "yes" : "no"),
      "",
      "rest_uri    " + a.rest_uri,
      "namespace   " + a.ns.ToString(),
      "warehouse   " + a.warehouse,
      "",
      "generate.threads      " + std::to_string(c.generate.threads),
      "generate.chunk_primes " + std::to_string(c.generate.chunk_primes),
      "query.limit           " + std::to_string(c.query.limit),
      "tui.log_limit         " + std::to_string(c.tui.log_limit),
  };
}

std::string editor_command() {
  if (const char* e = std::getenv("EDITOR"); e && *e) return e;
  return "vi";
}

}  // namespace

void draw_config(App* a) {
  unsigned iy, ix, irows, icols;
  frame(a->query, "Configuration", true, &iy, &ix, &irows, &icols);
  if (irows == 0) return;
  ncplane_set_fg_rgb8(a->query, 0x77, 0x77, 0x88);
  ncplane_printf_yx(a->query, (int)iy, (int)ix, "%.*s", (int)icols,
                    "e edit in $EDITOR   F1 back to queries");
  ncplane_set_fg_default(a->query);
  const auto lines = config_lines(*a);
  for (size_t i = 0; i < lines.size() && i + 2 < irows; ++i) {
    ncplane_printf_yx(a->query, (int)(iy + 2 + i), (int)ix, "%.*s", (int)icols,
                      lines[i].c_str());
  }
}

bool reopen_warehouse(App* a, const std::string& path) {
  a->warehouse = path;
  std::string err;
  auto qs = QueryService::Open(path, a->rest_uri, a->ns, &err);
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

void run_janitor(App* a) {
  std::error_code ec;
  fs::path logs = fs::path(a->config_path).parent_path() / "logs";
  if (!fs::exists(logs, ec)) return;
  std::vector<fs::path> files;
  for (auto& e : fs::directory_iterator(logs, ec))
    if (e.is_regular_file(ec)) files.push_back(e.path());
  if ((int64_t)files.size() <= a->conf.tui.log_limit) return;
  std::sort(files.begin(), files.end(), [](const fs::path& x, const fs::path& y) {
    std::error_code e1, e2;
    return fs::last_write_time(x, e1) > fs::last_write_time(y, e2);
  });
  for (size_t i = (size_t)a->conf.tui.log_limit; i < files.size(); ++i)
    fs::remove(files[i], ec);
}

void edit_config(App* a) {
  const std::string cmd = editor_command() + " " + a->config_path;
  while (true) {
    notcurses_stop(a->nc);
    const int rc = std::system(cmd.c_str());
    notcurses_options opts{};
    opts.flags = NCOPTION_SUPPRESS_BANNERS;
    a->nc = notcurses_core_init(&opts, nullptr);
    if (a->nc == nullptr) std::exit(1);
    layout(a);
    if (rc != 0) {
      a->status_glyph = '!';
      a->status_msg = "editor exited " + std::to_string(rc);
      return;
    }
    std::string err;
    config::Conf next;
    if (config::Load(a->config_path, &next, &err)) {
      a->conf = next;
      const std::string old_wh = a->warehouse;
      a->rest_uri = next.core.rest_uri;
      a->ns = catalog::ResolveNamespace(next.core.ns_name);
      if (next.core.warehouse != old_wh) {
        reopen_warehouse(a, next.core.warehouse);
      } else {
        a->status_glyph = 'k';
        a->status_msg = "config reloaded";
      }
      run_janitor(a);
      return;
    }
    a->status_glyph = '!';
    a->status_msg = err;
    redraw(a);
    ncinput in;
    std::fprintf(stderr, "%s\n", err.c_str());
    const uint32_t key = notcurses_get_blocking(a->nc, &in);
    if (key == NCKEY_ESC || key == 'q') return;
  }
}

}  // namespace primeparts::tui
