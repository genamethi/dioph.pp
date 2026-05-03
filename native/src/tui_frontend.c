#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <notcurses/notcurses.h>

#include "primeparts/ui_iceberg.h"

typedef struct {
    const char* key;
    int64_t max_p;
    int64_t total_rows;
    int snapshot_count;
    char last_error[256];
} table_status;

typedef struct {
    struct notcurses* nc;
    struct ncplane* master;
    struct ncplane* detail;
    struct ncplane* status;
    pp_uic_handle* handle;
    const char* warehouse_root;
    int selected;
    table_status tables[2];
    bool had_error;
} app_state;

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

static void set_error(table_status* st, const char* msg) {
    st->max_p = -1;
    st->total_rows = -1;
    st->snapshot_count = 0;
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
    return 0;
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
    ncplane_printf_yx(app->detail, 3, 0, "[ok] max_p: %" PRId64, st->max_p);
    ncplane_printf_yx(app->detail, 4, 0, "[ok] total_rows: %" PRId64, st->total_rows);
    ncplane_printf_yx(app->detail, 5, 0, "[ok] snapshots: %d", st->snapshot_count);
}

static void draw_status(app_state* app) {
    ncplane_erase(app->status);
    ncplane_set_fg_rgb8(app->status, 0, 0, 0);
    ncplane_set_bg_rgb8(app->status, 200, 200, 200);
    ncplane_set_styles(app->status, NCSTYLE_BOLD);
    if (app->had_error) {
        ncplane_putstr_yx(
            app->status,
            0,
            0,
            "[!] h/j/k/l or arrows navigate | Enter refresh | q quit");
    } else {
        ncplane_putstr_yx(
            app->status,
            0,
            0,
            "[ok] h/j/k/l or arrows navigate | Enter refresh | q quit");
    }
}

static void draw(app_state* app) {
    draw_master(app);
    draw_detail(app);
    draw_status(app);
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
    if (!master || !detail || !status) {
        fprintf(stderr, "[!] could not create UI planes\n");
        if (master) ncplane_destroy(master);
        if (detail) ncplane_destroy(detail);
        if (status) ncplane_destroy(status);
        notcurses_stop(nc);
        pp_uic_close(h);
        return 1;
    }

    app_state app = {
        .nc = nc,
        .master = master,
        .detail = detail,
        .status = status,
        .handle = h,
        .warehouse_root = argv[1],
        .selected = 0,
        .tables = {
            {.key = "primes"},
            {.key = "decompositions"},
        },
    };

    if (layout(&app) < 0) {
        fprintf(stderr, "[!] terminal too small\n");
        ncplane_destroy(master);
        ncplane_destroy(detail);
        ncplane_destroy(status);
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
            case NCKEY_ENTER:
            case '\n':
            case '\r':
                refresh_status(&app);
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
    notcurses_stop(nc);
    pp_uic_close(h);
    return 0;
}
