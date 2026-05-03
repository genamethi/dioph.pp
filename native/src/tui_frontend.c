#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include <notcurses/notcurses.h>

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
    CONFIRM_GENERATE_BG = 2,
} confirm_mode;

typedef struct {
    struct notcurses* nc;
    struct ncplane* master;
    struct ncplane* detail;
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
    char op_lines[6][160];
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

static void draw(app_state* app);
static void refresh_status(app_state* app);

static void set_modal_message(app_state* app, const char* msg) {
    snprintf(app->gen_modal_msg, sizeof(app->gen_modal_msg), "%s", msg ? msg : "");
}

static void sync_generation_buffers(app_state* app) {
    snprintf(app->gen_modal_num, sizeof(app->gen_modal_num), "%" PRId64, app->gen_num_primes);
    snprintf(app->gen_modal_batch, sizeof(app->gen_modal_batch), "%" PRId64, app->gen_batch_size);
    snprintf(app->gen_modal_threads, sizeof(app->gen_modal_threads), "%d", app->gen_threads);
}

static bool parse_generation_buffers(app_state* app) {
    char* end = NULL;
    long long num = strtoll(app->gen_modal_num, &end, 10);
    if (end == app->gen_modal_num || *end != '\0' || num <= 0) {
        set_modal_message(app, "num primes must be a positive integer");
        return false;
    }
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
    for (int i = 0; i < 6; ++i) {
        app->op_lines[i][0] = '\0';
    }
}

static void push_op_line(app_state* app, const char* line) {
    if (!line || !*line || app->op_line_count >= 6) return;
    snprintf(app->op_lines[app->op_line_count], sizeof(app->op_lines[0]), "%s", line);
    app->op_line_count += 1;
}

static int run_shell_capture(app_state* app, const char* cmd) {
    snprintf(app->op_last_cmd, sizeof(app->op_last_cmd), "%s", cmd);
    clear_op_lines(app);
    FILE* fp = popen(cmd, "r");
    if (!fp) {
        app->op_failed = true;
        snprintf(app->op_status, sizeof(app->op_status), "failed to launch command");
        return -1;
    }

    char buf[320];
    while (fgets(buf, sizeof(buf), fp) != NULL && app->op_line_count < 6) {
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

static int launch_generation_bg(app_state* app) {
    char cmd[320];
    snprintf(
        cmd,
        sizeof(cmd),
        "sh -lc 'mkdir -p logs && nohup primeparts -n %" PRId64 " -b %" PRId64 " --threads %d > logs/tui_generate.log 2>&1 &'",
        app->gen_num_primes,
        app->gen_batch_size,
        app->gen_threads);
    snprintf(app->op_last_cmd, sizeof(app->op_last_cmd), "%s", cmd);
    clear_op_lines(app);
    push_op_line(app, "generation launched in background");
    char cfg[160];
    snprintf(
        cfg,
        sizeof(cfg),
        "config: -n %" PRId64 " -b %" PRId64 " --threads %d",
        app->gen_num_primes,
        app->gen_batch_size,
        app->gen_threads);
    push_op_line(app, cfg);
    push_op_line(app, "log: logs/tui_generate.log");
    int rc = system(app->op_last_cmd);
    app->op_failed = (rc != 0);
    snprintf(
        app->op_status,
        sizeof(app->op_status),
        "%s",
        app->op_failed ? "failed to launch generation" : "generation launch requested");
    return rc;
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
    draw(app);
    notcurses_render(nc);
    run_shell_capture(app, cmd);
    refresh_status(app);
    app->op_busy = false;
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
    unsigned split = cols / 3;
    if (split < 20) split = 20;
    if (cols - split < 20) split = cols - 20;
    ncplane_resize_simple(app->master, body_rows, split);
    ncplane_move_yx(app->master, 0, 0);
    ncplane_resize_simple(app->detail, body_rows, cols - split);
    ncplane_move_yx(app->detail, 0, split);
    ncplane_resize_simple(app->status, 1, cols);
    ncplane_move_yx(app->status, rows - 1, 0);
    if (app->modal) {
        if (rows < 11 || cols < 48) {
            app->modal_rows = 0;
            app->modal_cols = 0;
            return 0;
        }
        app->modal_cols = cols > 2 ? cols - 2 : cols;
        if (app->modal_cols > 72) app->modal_cols = 72;
        if (app->modal_cols < 48) app->modal_cols = 48;
        app->modal_rows = rows > 2 ? rows - 2 : rows;
        if (app->modal_rows > 15) app->modal_rows = 15;
        if (app->modal_rows < 11) app->modal_rows = 11;
        unsigned modal_y = (rows > app->modal_rows) ? (rows - app->modal_rows) / 2 : 0;
        unsigned modal_x = (cols > app->modal_cols) ? (cols - app->modal_cols) / 2 : 0;
        ncplane_resize_simple(app->modal, app->modal_rows, app->modal_cols);
        ncplane_move_yx(app->modal, modal_y, modal_x);
    }
    return 0;
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
    if (rows < 11 || cols < 48) return;

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
    ncplane_putstr_yx(app->modal, 2, 2, "Enter values directly, then press Enter to launch.");

    const bool active0 = app->gen_modal_field == 0;
    const bool active1 = app->gen_modal_field == 1;
    const bool active2 = app->gen_modal_field == 2;

    if (active0) {
        ncplane_set_bg_rgb8(app->modal, 80, 80, 110);
    } else {
        ncplane_set_bg_rgb8(app->modal, 40, 40, 48);
    }
    ncplane_putstr_yx(app->modal, 4, 2, "num primes: ");
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
    ncplane_putstr_yx(app->detail, 10, 0, "s sync-hms --dry-run");
    ncplane_putstr_yx(app->detail, 11, 0, "S sync-hms");
    ncplane_putstr_yx(app->detail, 12, 0, "modal: Tab field | digits edit | Enter launch | Esc cancel");
    ncplane_printf_yx(
        app->detail,
        13,
        0,
        "settings: -n=%" PRId64 "  -b=%" PRId64 "  --threads=%d",
        app->gen_num_primes,
        app->gen_batch_size,
        app->gen_threads);
    ncplane_printf_yx(app->detail, 15, 0, "last command: %s", app->op_last_cmd[0] ? app->op_last_cmd : "(none)");
    ncplane_printf_yx(app->detail, 16, 0, "last status: %s", app->op_status[0] ? app->op_status : "(none)");
    for (int i = 0; i < app->op_line_count; ++i) {
        ncplane_printf_yx(app->detail, 18 + i, 0, "> %s", app->op_lines[i]);
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
    draw_detail(app);
    draw_status(app);
    draw_generation_modal(app);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <warehouse_root>\n", argv[0]);
        return 2;
    }
    pp_uic_handle* h = pp_uic_open(argv[1]);
    if (!h) {
        fprintf(stderr, "[!] pp_uic_open(%s) returned NULL\n", argv[1]);
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
    struct ncplane* status = ncplane_create(std, &base);
    struct ncplane* modal = ncplane_create(std, &base);
    if (!master || !detail || !status || !modal) {
        fprintf(stderr, "[!] could not create UI planes\n");
        if (master) ncplane_destroy(master);
        if (detail) ncplane_destroy(detail);
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
        .status = status,
        .modal = modal,
        .handle = h,
        .warehouse_root = argv[1],
        .selected = 0,
        .tables = {
            {.key = "primes"},
            {.key = "decompositions"},
        },
    };
    app.op_status[0] = '\0';
    app.op_last_cmd[0] = '\0';
    app.pending_confirm = CONFIRM_NONE;
    app.gen_num_primes = 1000000;
    app.gen_batch_size = 500000;
    app.gen_threads = 24;
    app.modal_rows = 0;
    app.modal_cols = 0;
    clear_op_lines(&app);

    if (layout(&app) < 0) {
        fprintf(stderr, "[!] terminal too small\n");
        ncplane_destroy(master);
        ncplane_destroy(detail);
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

        if (app.gen_modal_active) {
            if (key == '\t' || key == 'j' || key == NCKEY_DOWN || key == NCKEY_RIGHT) {
                advance_generation_field(&app);
            } else if (key == 'k' || key == NCKEY_UP || key == NCKEY_LEFT) {
                retreat_generation_field(&app);
            } else if (key >= '0' && key <= '9') {
                append_generation_digit(&app, (char)key);
            } else if (key == 127 || key == 8) {
                backspace_generation_field(&app);
            } else if (key == NCKEY_ENTER || key == '\n' || key == '\r') {
                if (parse_generation_buffers(&app)) {
                    close_generation_modal(&app);
                    app.op_busy = true;
                    app.op_failed = false;
                    snprintf(app.op_status, sizeof(app.op_status), "launching generation");
                    draw(&app);
                    notcurses_render(nc);
                    launch_generation_bg(&app);
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
                    run_op_with_refresh(
                        &app,
                        nc,
                        "running sync-hms",
                        "timeout 900s pixi run sync-hms 2>&1");
                } else if (mode == CONFIRM_GENERATE_BG) {
                    app.op_busy = true;
                    app.op_failed = false;
                    snprintf(app.op_status, sizeof(app.op_status), "launching generation");
                    draw(&app);
                    notcurses_render(nc);
                    launch_generation_bg(&app);
                    refresh_status(&app);
                    app.op_busy = false;
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
                    "timeout 300s PYTHONPATH=src python -m primeparts.native_iceberg --check-warehouse 2>&1");
                break;
            case 's':
                run_op_with_refresh(
                    &app,
                    nc,
                    "running sync-hms dry-run",
                    "timeout 600s pixi run sync-hms --dry-run 2>&1");
                break;
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
    ncplane_destroy(status);
    ncplane_destroy(modal);
    notcurses_stop(nc);
    pp_uic_close(h);
    return 0;
}
