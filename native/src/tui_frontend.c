#include <inttypes.h>
#include <limits.h>
#include <time.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include <unistd.h>
#include <notcurses/notcurses.h>

#include "primeparts/core.h"
#include "primeparts/generate.h"
#include "primeparts/ui_iceberg.h"

typedef struct {
    int64_t snapshot_id;
    int64_t sequence_number;
    int64_t timestamp_ms;
    char operation[32];
} snapshot_row;

typedef struct {
    const char* key;
    int64_t max_p;
    int64_t total_rows;
    int snapshot_count;
    int snapshot_rows;
    int64_t current_snapshot_id;
    snapshot_row snapshots[12];
    char last_error[256];
} table_status;

typedef enum {
    VIEW_STATUS = 0,
    VIEW_SNAPSHOTS = 1,
    VIEW_OPS = 2,
} view_mode;

typedef enum {
    CONFIRM_NONE = 0,
    CONFIRM_SYNC_LIVE = 1,
} confirm_mode;

typedef struct {
    struct notcurses* nc;
    struct ncplane* master;
    struct ncplane* detail;
    struct ncplane* output;
    struct ncplane* divider_left;
    struct ncplane* divider_right;
    struct ncplane* status;
    struct ncplane* modal;
    pp_uic_handle* handle;
    const char* warehouse_root;
    int selected;
    view_mode mode;
    table_status tables[2];
    bool had_error;
    bool op_busy;
    bool op_failed;
    char op_status[160];
    char op_last_cmd[384];
    char op_lines[20][160];
    int op_line_count;
    confirm_mode pending_confirm;
    int64_t gen_num_primes;
    int64_t gen_batch_size;
    int gen_threads;
    bool gen_modal_active;
    int gen_modal_field;
    unsigned modal_rows;
    unsigned modal_cols;
    char gen_modal_num[32];
    char gen_modal_batch[32];
    char gen_modal_threads[16];
    char gen_modal_msg[160];
} app_state;

static const int64_t GEN_NUM_SCALE = 100000000;

static void draw(app_state* app);
static void refresh_status(app_state* app);

static const char* skip_ws(const char* s) {
    while (*s == ' ' || *s == '\t') ++s;
    return s;
}

static void rstrip_inplace(char* s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

static bool parse_warehouse_root_from_config(const char* cfg_path, char* out, size_t out_cap) {
    FILE* fp = fopen(cfg_path, "r");
    if (!fp) return false;
    char line[1024];
    bool found = false;
    while (fgets(line, sizeof(line), fp) != NULL) {
        rstrip_inplace(line);
        const char* p = skip_ws(line);
        if (*p == '\0' || *p == '#') continue;
        const char* key = "warehouse_root=";
        size_t keylen = strlen(key);
        if (strncmp(p, key, keylen) == 0) {
            p = skip_ws(p + keylen);
            if (*p == '\0') continue;
            snprintf(out, out_cap, "%s", p);
            found = true;
            break;
        }
    }
    fclose(fp);
    return found;
}

static bool resolve_warehouse_root(int argc, char** argv, char* out, size_t out_cap, char* cfg_path, size_t cfg_cap) {
    if (argc >= 2 && argv[1] && argv[1][0]) {
        snprintf(out, out_cap, "%s", argv[1]);
        cfg_path[0] = '\0';
        return true;
    }

    const char* env_wh = getenv("PRIMEPARTS_WAREHOUSE_ROOT");
    if (env_wh && *env_wh) {
        snprintf(out, out_cap, "%s", env_wh);
        cfg_path[0] = '\0';
        return true;
    }

    const char* home = getenv("HOME");
    if (!home || !*home) return false;
    snprintf(cfg_path, cfg_cap, "%s/.config/primeparts/tui.conf", home);
    return parse_warehouse_root_from_config(cfg_path, out, out_cap);
}

static void set_modal_message(app_state* app, const char* msg) {
    snprintf(app->gen_modal_msg, sizeof(app->gen_modal_msg), "%s", msg ? msg : "");
}

static void sync_generation_buffers(app_state* app) {
    int64_t units = app->gen_num_primes / GEN_NUM_SCALE;
    if (units <= 0) units = 1;
    snprintf(app->gen_modal_num, sizeof(app->gen_modal_num), "%" PRId64, units);
    snprintf(app->gen_modal_batch, sizeof(app->gen_modal_batch), "%" PRId64, app->gen_batch_size);
    snprintf(app->gen_modal_threads, sizeof(app->gen_modal_threads), "%d", app->gen_threads);
}

static bool parse_generation_buffers(app_state* app) {
    char* end = NULL;
    long long num_units = strtoll(app->gen_modal_num, &end, 10);
    if (end == app->gen_modal_num || *end != '\0' || num_units <= 0) {
        set_modal_message(app, "n-units must be a positive integer (n * 100M)");
        return false;
    }
    if (num_units > (LLONG_MAX / GEN_NUM_SCALE)) {
        set_modal_message(app, "n-units too large");
        return false;
    }
    long long num = num_units * GEN_NUM_SCALE;
    long long batch = strtoll(app->gen_modal_batch, &end, 10);
    if (end == app->gen_modal_batch || *end != '\0' || batch <= 0) {
        set_modal_message(app, "batch size must be a positive integer");
        return false;
    }
    long long threads = strtoll(app->gen_modal_threads, &end, 10);
    if (end == app->gen_modal_threads || *end != '\0' || threads <= 0) {
        set_modal_message(app, "threads must be a positive integer");
        return false;
    }
    if (batch > num) {
        batch = num;
    }
    app->gen_num_primes = (int64_t)num;
    app->gen_batch_size = (int64_t)batch;
    app->gen_threads = (int)threads;
    return true;
}

static void open_generation_modal(app_state* app) {
    app->gen_modal_active = true;
    app->gen_modal_field = 0;
    sync_generation_buffers(app);
    set_modal_message(app, "Tab to move fields | Enter to launch | Esc cancels");
    snprintf(app->op_status, sizeof(app->op_status), "editing generation settings");
}

static void close_generation_modal(app_state* app) {
    app->gen_modal_active = false;
    app->gen_modal_field = 0;
    app->gen_modal_msg[0] = '\0';
}

static char* active_generation_field(app_state* app) {
    switch (app->gen_modal_field) {
        case 0: return app->gen_modal_num;
        case 1: return app->gen_modal_batch;
        default: return app->gen_modal_threads;
    }
}

static size_t active_generation_field_cap(app_state* app) {
    switch (app->gen_modal_field) {
        case 0: return sizeof(app->gen_modal_num);
        case 1: return sizeof(app->gen_modal_batch);
        default: return sizeof(app->gen_modal_threads);
    }
}

static void append_generation_digit(app_state* app, char digit) {
    char* field = active_generation_field(app);
    size_t cap = active_generation_field_cap(app);
    size_t len = strlen(field);
    if (len + 1 >= cap) return;
    if (len == 1 && field[0] == '0') {
        field[0] = digit;
        field[1] = '\0';
        return;
    }
    field[len] = digit;
    field[len + 1] = '\0';
}

static void backspace_generation_field(app_state* app) {
    char* field = active_generation_field(app);
    size_t len = strlen(field);
    if (len == 0) return;
    field[len - 1] = '\0';
}

static void advance_generation_field(app_state* app) {
    app->gen_modal_field = (app->gen_modal_field + 1) % 3;
}

static void retreat_generation_field(app_state* app) {
    app->gen_modal_field = (app->gen_modal_field + 2) % 3;
}

static void clear_op_lines(app_state* app) {
    app->op_line_count = 0;
    for (int i = 0; i < 20; ++i) {
        app->op_lines[i][0] = '\0';
    }
}

static void push_op_line(app_state* app, const char* line) {
    if (!line || !*line) return;
    if (app->op_line_count < 20) {
        snprintf(app->op_lines[app->op_line_count], sizeof(app->op_lines[0]), "%.159s", line);
        app->op_line_count += 1;
        return;
    }
    memmove(app->op_lines, app->op_lines + 1, sizeof(app->op_lines[0]) * 19);
    snprintf(app->op_lines[19], sizeof(app->op_lines[0]), "%.159s", line);
}

static int run_shell_capture(app_state* app, const char* cmd) {
    snprintf(app->op_last_cmd, sizeof(app->op_last_cmd), "%s", cmd);
    FILE* fp = popen(cmd, "r");
    if (!fp) {
        app->op_failed = true;
        snprintf(app->op_status, sizeof(app->op_status), "failed to launch command");
        return -1;
    }

    char buf[320];
    while (fgets(buf, sizeof(buf), fp) != NULL && app->op_line_count < 20) {
        size_t n = strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
            buf[--n] = '\0';
        }
        if (n > 0) push_op_line(app, buf);
    }

    int rc = pclose(fp);
    int exit_code = -1;
    if (WIFEXITED(rc)) {
        exit_code = WEXITSTATUS(rc);
    }
    app->op_failed = (exit_code != 0);
    snprintf(
        app->op_status,
        sizeof(app->op_status),
        "%s (exit=%d)",
        app->op_failed ? "operation failed" : "operation ok",
        exit_code);
    return exit_code;
}

static bool resolve_rust_commit_binary(char* out, size_t cap) {
    const char* candidates[] = {
        "primeparts-commit",
        "target/release/primeparts-commit",
        "target/debug/primeparts-commit",
        "../target/release/primeparts-commit",
        "../target/debug/primeparts-commit",
        "crates/target/release/primeparts-commit",
        "crates/target/debug/primeparts-commit",
        "../crates/target/release/primeparts-commit",
        "../crates/target/debug/primeparts-commit",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        if (access(candidates[i], X_OK) == 0) {
            snprintf(out, cap, "%s", candidates[i]);
            return true;
        }
    }
    return false;
}

static void utc_timestamp_compact(char* out, size_t cap) {
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, cap, "%Y%m%d_%H%M%S", &tm);
}

static void generation_log_callback(void* user_data, const char* line) {
    app_state* app = (app_state*)user_data;
    if (!app || !line || !*line) return;
    push_op_line(app, line);
    draw(app);
    notcurses_render(app->nc);
}

static int launch_generation_via_abi(app_state* app) {
    clear_op_lines(app);
    push_op_line(app, "starting native generation (ABI)");
    char cfg[PATH_MAX + 64];
    snprintf(
        cfg,
        sizeof(cfg),
        "config: -n %" PRId64 " --chunk-primes %" PRId64 " --threads %d",
        app->gen_num_primes,
        app->gen_batch_size,
        app->gen_threads);
    push_op_line(app, cfg);
    int64_t max_p = pp_uic_max_p(app->handle, "primes");
    int64_t existing_prime_rows = pp_uic_total_rows(app->handle, "primes");
    if (max_p < 0 && existing_prime_rows < 0) {
        app->op_failed = true;
        snprintf(app->op_status, sizeof(app->op_status), "failed to read generation resume point");
        push_op_line(app, pp_uic_last_error(app->handle));
        return 1;
    }
    int64_t start_idx = 1;
    if (max_p >= 0) {
        int64_t next_p = pp_next_prime(max_p);
        if (next_p < 0) {
            app->op_failed = true;
            snprintf(app->op_status, sizeof(app->op_status), "failed to compute next prime");
            push_op_line(app, "primesieve failed while resolving generation resume point");
            return 1;
        }
        int64_t next_p_rank = pp_prime_pi(next_p);
        if (next_p_rank < 0) {
            app->op_failed = true;
            snprintf(app->op_status, sizeof(app->op_status), "failed to compute next prime rank");
            push_op_line(app, "primecount failed while resolving generation resume point");
            return 1;
        }
        start_idx = next_p_rank;
        if (existing_prime_rows >= 0 && existing_prime_rows + 1 != start_idx) {
            char resume_note[160];
            snprintf(
                resume_note,
                sizeof(resume_note),
                "resume adjusted from row count %" PRId64 " to next prime rank %" PRId64,
                existing_prime_rows + 1,
                start_idx);
            push_op_line(app, resume_note);
        }
    } else if (existing_prime_rows >= 0) {
        start_idx = existing_prime_rows + 1;
    }
    int64_t max_commit_seq = pp_uic_max_commit_seq(app->handle, "primes");
    if (max_commit_seq < 0 && existing_prime_rows > 0) {
        app->op_failed = true;
        snprintf(app->op_status, sizeof(app->op_status), "failed to read max_commit_seq");
        push_op_line(app, pp_uic_last_error(app->handle));
        return 1;
    }
    int32_t commit_seq_start = (max_commit_seq >= 0) ? (int32_t)(max_commit_seq + 1) : 0;
    char ts[32];
    utc_timestamp_compact(ts, sizeof(ts));
    char warehouse_path[PATH_MAX];
    char manifest_path[PATH_MAX];
    snprintf(warehouse_path, sizeof(warehouse_path), "%s/warehouse", app->warehouse_root);
    snprintf(manifest_path, sizeof(manifest_path), "%s/files_tui_%s.jsonl", app->warehouse_root, ts);
    pp_gen_options opts = {
        .start_idx = start_idx,
        .count = app->gen_num_primes,
        .chunk_primes = app->gen_batch_size,
        .threads = app->gen_threads,
        .commit_seq_start = commit_seq_start,
        .temp = 0,
        .warehouse = warehouse_path,
        .manifest = manifest_path,
    };
    pp_gen_callbacks cb = {
        .on_log = generation_log_callback,
        .user_data = app,
    };
    pp_gen_result result = {0};
    int rc = pp_gen_run(&opts, &cb, &result);
    if (rc != 0) {
        app->op_failed = true;
        snprintf(app->op_status, sizeof(app->op_status), "native generation failed");
        push_op_line(app, pp_gen_last_error());
        return rc;
    }
    snprintf(cfg, sizeof(cfg), "native manifest: %s", manifest_path);
    push_op_line(app, cfg);
    char commit_bin[PATH_MAX];
    if (!resolve_rust_commit_binary(commit_bin, sizeof(commit_bin))) {
        app->op_failed = true;
        snprintf(app->op_status, sizeof(app->op_status), "primeparts-commit binary not found");
        push_op_line(app, "build it with: cargo build -p primeparts-commit (from crates/)");
        return 1;
    }
    char cmd[PATH_MAX * 3 + 512];
    char sqlite_uri[PATH_MAX + 16];
    snprintf(sqlite_uri, sizeof(sqlite_uri), "sqlite:///%s/catalog.db", app->warehouse_root);
    snprintf(
        cmd,
        sizeof(cmd),
        "%s --manifest '%s' --warehouse '%s' --sqlite '%s' --warehouse-standing skip 2>&1",
        commit_bin,
        manifest_path,
        warehouse_path,
        sqlite_uri);
    int commit_exit = run_shell_capture(app, cmd);
    refresh_status(app);
    app->op_failed = (commit_exit != 0);
    snprintf(
        app->op_status,
        sizeof(app->op_status),
        "%s",
        app->op_failed ? "native generation ok; commit failed" : "native generation + commit ok");
    return app->op_failed ? 1 : 0;
}

static void set_pending_confirm(app_state* app, confirm_mode mode, const char* message) {
    app->pending_confirm = mode;
    app->op_failed = false;
    snprintf(app->op_status, sizeof(app->op_status), "%s", message);
    clear_op_lines(app);
}

static void clear_pending_confirm(app_state* app) {
    app->pending_confirm = CONFIRM_NONE;
}

static void cancel_pending_confirm(app_state* app) {
    clear_pending_confirm(app);
    app->op_failed = false;
    snprintf(app->op_status, sizeof(app->op_status), "operation cancelled");
    clear_op_lines(app);
}

static void run_op_with_refresh(app_state* app, struct notcurses* nc, const char* status_msg, const char* cmd) {
    app->op_busy = true;
    app->op_failed = false;
    snprintf(app->op_status, sizeof(app->op_status), "%s", status_msg);
    clear_op_lines(app);
    draw(app);
    notcurses_render(nc);
    run_shell_capture(app, cmd);
    refresh_status(app);
    app->op_busy = false;
}

static bool build_rust_hms_sync_cmd(
    app_state* app,
    bool dry_run,
    char* cmd,
    size_t cmd_cap) {
    char commit_bin[PATH_MAX];
    if (!resolve_rust_commit_binary(commit_bin, sizeof(commit_bin))) {
        app->op_failed = true;
        snprintf(app->op_status, sizeof(app->op_status), "primeparts-commit binary not found");
        clear_op_lines(app);
        push_op_line(app, "build it with: cargo build -p primeparts-commit");
        return false;
    }

    char warehouse_path[PATH_MAX];
    char sqlite_uri[PATH_MAX + 16];
    snprintf(warehouse_path, sizeof(warehouse_path), "%s/warehouse", app->warehouse_root);
    snprintf(sqlite_uri, sizeof(sqlite_uri), "sqlite:///%s/catalog.db", app->warehouse_root);
    snprintf(
        cmd,
        cmd_cap,
        "%s --sync-hms-only --warehouse '%s' --sqlite '%s'%s 2>&1",
        commit_bin,
        warehouse_path,
        sqlite_uri,
        dry_run ? " --dry-run" : "");
    return true;
}

static int count_snapshot_entries(const char* json) {
    int count = 0;
    const char* p = json;
    while (p && *p) {
        p = strstr(p, "\"snapshot_id\"");
        if (!p) break;
        ++count;
        ++p;
    }
    return count;
}

static bool extract_i64(const char* src, const char* key, int64_t* out) {
    const char* p = strstr(src, key);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    char* end = NULL;
    long long v = strtoll(p, &end, 10);
    if (end == p) return false;
    *out = (int64_t)v;
    return true;
}

static void extract_operation(const char* src, char* out, size_t out_cap) {
    const char* p = strstr(src, "\"operation\"");
    if (!p) {
        snprintf(out, out_cap, "unknown");
        return;
    }
    p = strchr(p, ':');
    if (!p) {
        snprintf(out, out_cap, "unknown");
        return;
    }
    const char* q1 = strchr(p, '"');
    if (!q1) {
        snprintf(out, out_cap, "unknown");
        return;
    }
    ++q1;
    const char* q2 = strchr(q1, '"');
    if (!q2 || q2 <= q1) {
        snprintf(out, out_cap, "unknown");
        return;
    }
    size_t n = (size_t)(q2 - q1);
    if (n >= out_cap) n = out_cap - 1;
    memcpy(out, q1, n);
    out[n] = '\0';
}

static void parse_snapshots(table_status* st, const char* json) {
    st->snapshot_rows = 0;
    st->current_snapshot_id = -1;
    extract_i64(json, "\"current_snapshot_id\"", &st->current_snapshot_id);

    const char* p = json;
    while (p && *p && st->snapshot_rows < (int)(sizeof(st->snapshots) / sizeof(st->snapshots[0]))) {
        p = strstr(p, "\"snapshot_id\"");
        if (!p) break;
        snapshot_row row = {0};
        if (!extract_i64(p, "\"snapshot_id\"", &row.snapshot_id)) {
            ++p;
            continue;
        }
        extract_i64(p, "\"sequence_number\"", &row.sequence_number);
        extract_i64(p, "\"timestamp_ms\"", &row.timestamp_ms);
        extract_operation(p, row.operation, sizeof(row.operation));
        st->snapshots[st->snapshot_rows++] = row;
        ++p;
    }
}

static void set_error(table_status* st, const char* msg) {
    st->max_p = -1;
    st->total_rows = -1;
    st->snapshot_count = 0;
    st->snapshot_rows = 0;
    st->current_snapshot_id = -1;
    snprintf(st->last_error, sizeof(st->last_error), "%s", msg ? msg : "unknown error");
}

static void refresh_table_status(app_state* app, table_status* st) {
    st->last_error[0] = '\0';
    st->max_p = pp_uic_max_p(app->handle, st->key);
    if (st->max_p < 0) {
        set_error(st, pp_uic_last_error(app->handle));
        app->had_error = true;
        return;
    }
    st->total_rows = pp_uic_total_rows(app->handle, st->key);
    if (st->total_rows < 0) {
        set_error(st, pp_uic_last_error(app->handle));
        app->had_error = true;
        return;
    }
    char* snaps = pp_uic_list_snapshots_json(app->handle, st->key);
    if (!snaps) {
        set_error(st, pp_uic_last_error(app->handle));
        app->had_error = true;
        return;
    }
    st->snapshot_count = count_snapshot_entries(snaps);
    parse_snapshots(st, snaps);
    pp_uic_free_string(snaps);
}

static void refresh_status(app_state* app) {
    app->had_error = false;
    refresh_table_status(app, &app->tables[0]);
    refresh_table_status(app, &app->tables[1]);
}

static int layout(app_state* app) {
    unsigned rows = 0;
    unsigned cols = 0;
    notcurses_term_dim_yx(app->nc, &rows, &cols);
    if (rows < 8 || cols < 40) return -1;
    unsigned body_rows = rows - 1;
    unsigned left = cols / 5;
    if (left < 22) left = 22;
    if (left > 32) left = 32;
    unsigned right = cols / 3;
    if (right < 44) right = 44;
    unsigned needed = left + right + 2 + 28;
    if (cols < needed) {
        if (right > 36) right = 36;
        if (left > 20) left = 20;
    }
    unsigned center = cols - left - right - 2;
    ncplane_resize_simple(app->master, body_rows, left);
    ncplane_move_yx(app->master, 0, 0);
    ncplane_resize_simple(app->divider_left, body_rows, 1);
    ncplane_move_yx(app->divider_left, 0, left);
    ncplane_resize_simple(app->detail, body_rows, center);
    ncplane_move_yx(app->detail, 0, left + 1);
    ncplane_resize_simple(app->divider_right, body_rows, 1);
    ncplane_move_yx(app->divider_right, 0, left + 1 + center);
    ncplane_resize_simple(app->output, body_rows, right);
    ncplane_move_yx(app->output, 0, left + 2 + center);
    ncplane_resize_simple(app->status, 1, cols);
    ncplane_move_yx(app->status, rows - 1, 0);
    if (app->modal) {
        if (rows < 13 || cols < 56) {
            app->modal_rows = 0;
            app->modal_cols = 0;
            return 0;
        }
        app->modal_cols = cols > 2 ? cols - 2 : cols;
        if (app->modal_cols > 72) app->modal_cols = 72;
        if (app->modal_cols < 48) app->modal_cols = 48;
        app->modal_rows = rows > 2 ? rows - 2 : rows;
        if (app->modal_rows > 17) app->modal_rows = 17;
        if (app->modal_rows < 13) app->modal_rows = 13;
        unsigned modal_y = (rows > app->modal_rows) ? (rows - app->modal_rows) / 2 : 0;
        unsigned modal_x = (cols > app->modal_cols) ? (cols - app->modal_cols) / 2 : 0;
        ncplane_resize_simple(app->modal, app->modal_rows, app->modal_cols);
        ncplane_move_yx(app->modal, modal_y, modal_x);
    }
    return 0;
}

static void draw_divider(struct ncplane* p) {
    ncplane_erase(p);
    ncplane_set_fg_rgb8(p, 90, 90, 90);
    ncplane_set_bg_default(p);
    unsigned rows = 0;
    unsigned cols = 0;
    ncplane_dim_yx(p, &rows, &cols);
    (void)cols;
    for (unsigned y = 0; y < rows; ++y) {
        ncplane_putstr_yx(p, y, 0, "|");
    }
}

static void draw_generation_modal(app_state* app) {
    if (!app->modal) return;
    if (!app->gen_modal_active) {
        ncplane_erase(app->modal);
        return;
    }
    ncplane_erase(app->modal);
    ncplane_set_fg_rgb8(app->modal, 240, 240, 240);
    ncplane_set_bg_rgb8(app->modal, 18, 18, 20);
    ncplane_set_styles(app->modal, NCSTYLE_BOLD);

    unsigned rows = app->modal_rows;
    unsigned cols = app->modal_cols;
    if (rows < 13 || cols < 56) return;

    char border[128];
    char middle[128];
    size_t i = 0;
    border[i++] = '+';
    for (; i + 1 < cols && i < sizeof(border) - 1; ++i) border[i] = '-';
    border[i++] = '+';
    border[i] = '\0';

    middle[0] = '|';
    for (unsigned j = 1; j + 1 < cols && j < sizeof(middle) - 1; ++j) middle[j] = ' ';
    middle[(cols > 1 ? cols - 1 : 0)] = '|';
    middle[(cols > 1 ? cols : 1)] = '\0';

    ncplane_putstr_yx(app->modal, 0, 0, border);
    for (unsigned y = 1; y + 1 < rows; ++y) {
        ncplane_putstr_yx(app->modal, y, 0, middle);
    }
    ncplane_putstr_yx(app->modal, rows - 1, 0, border);

    ncplane_putstr_yx(app->modal, 1, 2, "Generation Settings");
    ncplane_putstr_yx(app->modal, 2, 2, "num units are n*100M (1=100M, 10=1B).");

    const bool active0 = app->gen_modal_field == 0;
    const bool active1 = app->gen_modal_field == 1;
    const bool active2 = app->gen_modal_field == 2;

    if (active0) {
        ncplane_set_bg_rgb8(app->modal, 80, 80, 110);
    } else {
        ncplane_set_bg_rgb8(app->modal, 40, 40, 48);
    }
    ncplane_putstr_yx(app->modal, 4, 2, "num units: ");
    ncplane_putstr_yx(app->modal, 4, 14, app->gen_modal_num);

    if (active1) {
        ncplane_set_bg_rgb8(app->modal, 80, 80, 110);
    } else {
        ncplane_set_bg_rgb8(app->modal, 40, 40, 48);
    }
    ncplane_putstr_yx(app->modal, 6, 2, "batch size: ");
    ncplane_putstr_yx(app->modal, 6, 14, app->gen_modal_batch);

    if (active2) {
        ncplane_set_bg_rgb8(app->modal, 80, 80, 110);
    } else {
        ncplane_set_bg_rgb8(app->modal, 40, 40, 48);
    }
    ncplane_putstr_yx(app->modal, 8, 2, "threads:    ");
    ncplane_putstr_yx(app->modal, 8, 14, app->gen_modal_threads);

    ncplane_set_bg_rgb8(app->modal, 18, 18, 20);
    ncplane_putstr_yx(app->modal, rows - 4, 2, app->gen_modal_msg[0] ? app->gen_modal_msg : "");
    ncplane_putstr_yx(app->modal, rows - 3, 2, "Tab/j/k: move  Backspace: delete  Enter: launch  Esc: cancel");
}

static void draw_master(app_state* app) {
    ncplane_erase(app->master);
    ncplane_set_fg_default(app->master);
    ncplane_set_bg_default(app->master);
    ncplane_set_styles(app->master, NCSTYLE_NONE);
    ncplane_putstr_yx(app->master, 0, 0, "Tables");
    for (int i = 0; i < 2; ++i) {
        bool focused = (app->selected == i);
        if (focused) {
            ncplane_set_fg_rgb8(app->master, 0, 0, 0);
            ncplane_set_bg_rgb8(app->master, 240, 240, 240);
            ncplane_set_styles(app->master, NCSTYLE_BOLD);
        } else {
            ncplane_set_fg_rgb8(app->master, 140, 140, 140);
            ncplane_set_bg_default(app->master);
            ncplane_set_styles(app->master, NCSTYLE_NONE);
        }
        ncplane_printf_yx(
            app->master,
            2 + i,
            0,
            "%s %s",
            focused ? ">" : " ",
            app->tables[i].key);
    }

    ncplane_set_fg_rgb8(app->master, 120, 120, 120);
    ncplane_set_bg_default(app->master);
    ncplane_set_styles(app->master, NCSTYLE_NONE);
    ncplane_putstr_yx(app->master, 6, 0, "Views");
    for (int i = 0; i < 3; ++i) {
        bool focused = (app->mode == (view_mode)i);
        if (focused) {
            ncplane_set_fg_rgb8(app->master, 20, 20, 20);
            ncplane_set_bg_rgb8(app->master, 210, 210, 210);
            ncplane_set_styles(app->master, NCSTYLE_BOLD);
        } else {
            ncplane_set_fg_rgb8(app->master, 120, 120, 120);
            ncplane_set_bg_default(app->master);
            ncplane_set_styles(app->master, NCSTYLE_NONE);
        }
        const char* label = (i == VIEW_STATUS) ? "1 status"
                          : (i == VIEW_SNAPSHOTS) ? "2 snapshots"
                                                  : "3 ops/query";
        ncplane_printf_yx(app->master, 7 + i, 0, "%s", label);
    }
}

static void draw_status_view(app_state* app, table_status* st) {
    ncplane_printf_yx(app->detail, 3, 0, "[ok] max_p: %" PRId64, st->max_p);
    ncplane_printf_yx(app->detail, 4, 0, "[ok] total_rows: %" PRId64, st->total_rows);
    ncplane_printf_yx(app->detail, 5, 0, "[ok] snapshots: %d", st->snapshot_count);
}

static void draw_snapshots_view(app_state* app, table_status* st) {
    ncplane_printf_yx(
        app->detail, 3, 0, "current_snapshot_id: %" PRId64, st->current_snapshot_id);
    ncplane_printf_yx(app->detail, 4, 0, "showing %d/%d snapshots", st->snapshot_rows, st->snapshot_count);
    for (int i = 0; i < st->snapshot_rows; ++i) {
        const snapshot_row* row = &st->snapshots[i];
        ncplane_printf_yx(
            app->detail,
            6 + i,
            0,
            "#%d sid=%" PRId64 " seq=%" PRId64 " ts=%" PRId64 " op=%s",
            i + 1,
            row->snapshot_id,
            row->sequence_number,
            row->timestamp_ms,
            row->operation);
    }
}

static void draw_ops_view(app_state* app, table_status* st) {
    table_status* other = &app->tables[(app->selected + 1) % 2];
    ncplane_putstr_yx(app->detail, 3, 0, "Warehouse query checks");
    if (!st->last_error[0] && !other->last_error[0]) {
        int64_t delta = st->max_p - other->max_p;
        ncplane_printf_yx(app->detail, 4, 0, "max_p delta vs %s: %" PRId64, other->key, delta);
        if (st->total_rows > 0) {
            ncplane_printf_yx(
                app->detail,
                5,
                0,
                "rows/snapshot: %" PRId64,
                st->snapshot_count > 0 ? (st->total_rows / st->snapshot_count) : st->total_rows);
        }
    } else {
        ncplane_putstr_yx(app->detail, 4, 0, "cross-table query unavailable due to table error");
    }

    ncplane_putstr_yx(app->detail, 7, 0, "Actions");
    ncplane_putstr_yx(app->detail, 8, 0, "g open generation settings modal");
    ncplane_putstr_yx(app->detail, 9, 0, "c check warehouse");
    ncplane_putstr_yx(app->detail, 10, 0, "s rust hms sync --dry-run");
    ncplane_putstr_yx(app->detail, 11, 0, "S rust hms sync");
    ncplane_putstr_yx(app->detail, 12, 0, "modal: Tab field | digits edit | Enter launch | Esc cancel");
    ncplane_printf_yx(
        app->detail,
        13,
        0,
        "settings: n_units=%" PRId64 " (-n=%" PRId64 ")  --chunk-primes=%" PRId64,
        app->gen_num_primes / GEN_NUM_SCALE,
        app->gen_num_primes,
        app->gen_batch_size);
    ncplane_printf_yx(
        app->detail,
        14,
        0,
        "native: --threads=%d",
        app->gen_threads);
    ncplane_printf_yx(app->detail, 15, 0, "last command: %s", app->op_last_cmd[0] ? app->op_last_cmd : "(none)");
    ncplane_printf_yx(app->detail, 16, 0, "last status: %s", app->op_status[0] ? app->op_status : "(none)");
    for (int i = 0; i < app->op_line_count; ++i) {
        ncplane_printf_yx(app->detail, 18 + i, 0, "> %s", app->op_lines[i]);
    }
}

static void draw_output(app_state* app) {
    ncplane_erase(app->output);
    ncplane_set_fg_default(app->output);
    ncplane_set_bg_default(app->output);
    ncplane_set_styles(app->output, NCSTYLE_NONE);
    ncplane_putstr_yx(app->output, 0, 0, "Output");
    ncplane_printf_yx(app->output, 1, 0, "status: %s", app->op_status[0] ? app->op_status : "(none)");
    ncplane_putstr_yx(app->output, 3, 0, "native/commit output");
    if (app->op_line_count == 0) {
        ncplane_putstr_yx(app->output, 5, 0, "(no output yet)");
        return;
    }
    int shown = app->op_line_count < 12 ? app->op_line_count : 12;
    int start = app->op_line_count - shown;
    for (int i = 0; i < shown; ++i) {
        ncplane_printf_yx(app->output, 5 + i, 0, "%s", app->op_lines[start + i]);
    }
}

static void draw_detail(app_state* app) {
    ncplane_erase(app->detail);
    ncplane_set_fg_default(app->detail);
    ncplane_set_bg_default(app->detail);
    table_status* st = &app->tables[app->selected];
    ncplane_set_styles(app->detail, NCSTYLE_NONE);
    ncplane_printf_yx(app->detail, 0, 0, "Warehouse: %s", app->warehouse_root);
    ncplane_printf_yx(app->detail, 1, 0, "Table: %s", st->key);
    if (st->last_error[0]) {
        ncplane_set_styles(app->detail, NCSTYLE_BOLD);
        ncplane_printf_yx(app->detail, 3, 0, "[!] %s", st->last_error);
        return;
    }
    if (app->mode == VIEW_STATUS) {
        draw_status_view(app, st);
    } else if (app->mode == VIEW_SNAPSHOTS) {
        draw_snapshots_view(app, st);
    } else {
        draw_ops_view(app, st);
    }
}

static void draw_status(app_state* app) {
    ncplane_erase(app->status);
    ncplane_set_fg_rgb8(app->status, 0, 0, 0);
    ncplane_set_bg_rgb8(app->status, 200, 200, 200);
    ncplane_set_styles(app->status, NCSTYLE_BOLD);
    if (app->gen_modal_active) {
        ncplane_putstr_yx(
            app->status,
            0,
            0,
            "[?] generation modal open | Enter launch | Esc cancel | Tab field | q quit");
    } else if (app->pending_confirm != CONFIRM_NONE) {
        ncplane_putstr_yx(
            app->status,
            0,
            0,
            "[?] confirmation pending | y confirm | n/Esc cancel | arrows table | 1/2/3 view | q quit");
    } else if (app->op_busy) {
        ncplane_putstr_yx(
            app->status,
            0,
            0,
            "[..] operation running | arrows table | 1/2/3 view | g/c/s/S run ops | r refresh | q quit");
    } else if (app->had_error || app->op_failed) {
        ncplane_putstr_yx(
            app->status,
            0,
            0,
            "[!] arrows table | 1/2/3 view | g/c/s/S run ops | r refresh | Enter refresh | q quit");
    } else {
        ncplane_putstr_yx(
            app->status,
            0,
            0,
            "[ok] arrows table | 1/2/3 view | g/c/s/S run ops | r refresh | Enter refresh | q quit");
    }
}

static void draw(app_state* app) {
    draw_master(app);
    draw_divider(app->divider_left);
    draw_detail(app);
    draw_divider(app->divider_right);
    draw_output(app);
    draw_status(app);
    draw_generation_modal(app);
}

int main(int argc, char** argv) {
    if (argc > 2) {
        fprintf(stderr, "usage: %s [warehouse_root]\n", argv[0]);
        return 2;
    }
    char warehouse_root[PATH_MAX];
    char config_path[PATH_MAX];
    if (!resolve_warehouse_root(argc, argv, warehouse_root, sizeof(warehouse_root), config_path, sizeof(config_path))) {
        if (config_path[0]) {
            fprintf(stderr,
                    "[!] no warehouse root configured.\n"
                    "Set PRIMEPARTS_WAREHOUSE_ROOT, pass [warehouse_root], or create %s with:\n"
                    "warehouse_root=/media/extssd/research/dioph.pp/data/iceberg\n",
                    config_path);
        } else {
            fprintf(stderr,
                    "[!] no warehouse root configured.\n"
                    "Set PRIMEPARTS_WAREHOUSE_ROOT or pass [warehouse_root].\n");
        }
        return 2;
    }
    pp_uic_handle* h = pp_uic_open(warehouse_root);
    if (!h) {
        fprintf(stderr, "[!] pp_uic_open(%s) returned NULL\n", warehouse_root);
        return 1;
    }
    const char* open_err = pp_uic_last_error(h);
    if (open_err && *open_err) {
        fprintf(stderr, "[!] pp_uic_open: %s\n", open_err);
        pp_uic_close(h);
        return 1;
    }

    struct notcurses_options opts = {0};
    struct notcurses* nc = notcurses_core_init(&opts, NULL);
    if (!nc) {
        fprintf(stderr, "[!] notcurses init failed\n");
        pp_uic_close(h);
        return 1;
    }

    struct ncplane* std = notcurses_stdplane(nc);
    ncplane_options base = {.rows = 1, .cols = 1};
    struct ncplane* master = ncplane_create(std, &base);
    struct ncplane* detail = ncplane_create(std, &base);
    struct ncplane* output = ncplane_create(std, &base);
    struct ncplane* divider_left = ncplane_create(std, &base);
    struct ncplane* divider_right = ncplane_create(std, &base);
    struct ncplane* status = ncplane_create(std, &base);
    struct ncplane* modal = ncplane_create(std, &base);
    if (!master || !detail || !output || !divider_left || !divider_right || !status || !modal) {
        fprintf(stderr, "[!] could not create UI planes\n");
        if (master) ncplane_destroy(master);
        if (detail) ncplane_destroy(detail);
        if (output) ncplane_destroy(output);
        if (divider_left) ncplane_destroy(divider_left);
        if (divider_right) ncplane_destroy(divider_right);
        if (status) ncplane_destroy(status);
        if (modal) ncplane_destroy(modal);
        notcurses_stop(nc);
        pp_uic_close(h);
        return 1;
    }

    app_state app = {
        .nc = nc,
        .master = master,
        .detail = detail,
        .output = output,
        .divider_left = divider_left,
        .divider_right = divider_right,
        .status = status,
        .modal = modal,
        .handle = h,
        .warehouse_root = warehouse_root,
        .selected = 0,
        .tables = {
            {.key = "primes"},
            {.key = "decompositions"},
        },
    };
    app.op_status[0] = '\0';
    app.op_last_cmd[0] = '\0';
    app.pending_confirm = CONFIRM_NONE;
    app.gen_num_primes = 100000000;
    app.gen_batch_size = 1000000;
    long logical_cores = sysconf(_SC_NPROCESSORS_ONLN);
    app.gen_threads = (logical_cores > 1) ? (logical_cores / 2) : 1;
    app.modal_rows = 0;
    app.modal_cols = 0;
    clear_op_lines(&app);

    if (layout(&app) < 0) {
        fprintf(stderr, "[!] terminal too small\n");
        ncplane_destroy(master);
        ncplane_destroy(detail);
        ncplane_destroy(output);
        ncplane_destroy(divider_left);
        ncplane_destroy(divider_right);
        ncplane_destroy(status);
        ncplane_destroy(modal);
        notcurses_stop(nc);
        pp_uic_close(h);
        return 1;
    }
    refresh_status(&app);
    draw(&app);
    notcurses_render(nc);

    bool running = true;
    while (running) {
        ncinput in = {0};
        uint32_t key = notcurses_get_blocking(nc, &in);
        if (key == (uint32_t)-1) break;
        if (in.evtype == NCTYPE_RELEASE || in.evtype == NCTYPE_REPEAT) {
            continue;
        }

        if (app.gen_modal_active) {
            if (key == '\t' || key == 'j' || key == NCKEY_DOWN || key == NCKEY_RIGHT) {
                advance_generation_field(&app);
            } else if (key == 'k' || key == NCKEY_UP || key == NCKEY_LEFT) {
                retreat_generation_field(&app);
            } else if (key >= '0' && key <= '9') {
                append_generation_digit(&app, (char)key);
            } else if (key == NCKEY_BACKSPACE || key == NCKEY_DEL || key == 127 || key == 8) {
                backspace_generation_field(&app);
            } else if (key == NCKEY_ENTER || key == '\n' || key == '\r') {
                if (parse_generation_buffers(&app)) {
                    close_generation_modal(&app);
                    app.op_busy = true;
                    app.op_failed = false;
                    snprintf(app.op_status, sizeof(app.op_status), "launching generation");
                    draw(&app);
                    notcurses_render(nc);
                    launch_generation_via_abi(&app);
                    refresh_status(&app);
                    app.op_busy = false;
                }
            } else if (key == 'q') {
                running = false;
            } else if (key == NCKEY_ESC) {
                close_generation_modal(&app);
                snprintf(app.op_status, sizeof(app.op_status), "generation launch cancelled");
                app.op_failed = false;
            }
            if (!running) break;
            draw(&app);
            notcurses_render(nc);
            continue;
        }

        if (app.pending_confirm != CONFIRM_NONE) {
            if (key == 'y' || key == 'Y') {
                confirm_mode mode = app.pending_confirm;
                clear_pending_confirm(&app);
                if (mode == CONFIRM_SYNC_LIVE) {
                    char cmd[PATH_MAX * 2 + 256];
                    if (build_rust_hms_sync_cmd(&app, false, cmd, sizeof(cmd))) {
                        run_op_with_refresh(&app, nc, "running rust hms sync", cmd);
                    }
                }
            } else if (key == 'n' || key == 'N' || key == NCKEY_ESC) {
                cancel_pending_confirm(&app);
            } else if (key == 'q') {
                running = false;
            }
            if (!running) break;
            draw(&app);
            notcurses_render(nc);
            continue;
        }

        switch (key) {
            case 'q':
                running = false;
                break;
            case 'j':
            case 'l':
            case NCKEY_DOWN:
            case NCKEY_RIGHT:
                app.selected = (app.selected + 1) % 2;
                break;
            case 'k':
            case 'h':
            case NCKEY_UP:
            case NCKEY_LEFT:
                /* Two-table selector: up/left toggles the same as down/right. */
                app.selected = (app.selected + 1) % 2;
                break;
            case '1':
                app.mode = VIEW_STATUS;
                break;
            case '2':
                app.mode = VIEW_SNAPSHOTS;
                break;
            case '3':
                app.mode = VIEW_OPS;
                break;
            case 'r':
            case NCKEY_ENTER:
            case '\n':
            case '\r':
                refresh_status(&app);
                break;
            case 'c':
                run_op_with_refresh(
                    &app,
                    nc,
                    "running warehouse check",
                    "timeout 300s pixi run python -m primeparts.native_iceberg --check-warehouse 2>&1");
                break;
            case 's':
            {
                char cmd[PATH_MAX * 2 + 256];
                if (build_rust_hms_sync_cmd(&app, true, cmd, sizeof(cmd))) {
                    run_op_with_refresh(&app, nc, "running rust hms sync dry-run", cmd);
                }
                break;
            }
            case 'S':
                set_pending_confirm(
                    &app,
                    CONFIRM_SYNC_LIVE,
                    "confirm live HMS sync: press y to run, n to cancel");
                break;
            case 'g':
                open_generation_modal(&app);
                break;
            case NCKEY_RESIZE:
                layout(&app);
                break;
            default:
                break;
        }
        if (!running) break;
        draw(&app);
        notcurses_render(nc);
    }

    ncplane_destroy(master);
    ncplane_destroy(detail);
    ncplane_destroy(output);
    ncplane_destroy(divider_left);
    ncplane_destroy(divider_right);
    ncplane_destroy(status);
    ncplane_destroy(modal);
    notcurses_stop(nc);
    pp_uic_close(h);
    return 0;
}
