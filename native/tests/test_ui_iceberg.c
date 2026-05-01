/*
 * Smoke harness for the iceberg-cpp read shim. Argv:
 *   ./primeparts-test-ui-iceberg <warehouse_root>
 *
 * Prints max_p, total_rows, and the snapshot list for each table to
 * stdout, one section per table. Exits 0 on success, non-zero on any
 * read error so it can be wired into `make test`.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "primeparts/ui_iceberg.h"

static int dump_table(pp_uic_handle* h, const char* table) {
    printf("=== %s ===\n", table);

    int64_t max_p = pp_uic_max_p(h, table);
    if (max_p < 0) {
        fprintf(stderr, "[!] max_p(%s): %s\n", table, pp_uic_last_error(h));
        return 1;
    }
    printf("max_p: %lld\n", (long long)max_p);

    int64_t total = pp_uic_total_rows(h, table);
    if (total < 0) {
        fprintf(stderr, "[!] total_rows(%s): %s\n", table, pp_uic_last_error(h));
        return 1;
    }
    printf("total_rows: %lld\n", (long long)total);

    char* snaps = pp_uic_list_snapshots_json(h, table);
    if (!snaps) {
        fprintf(stderr, "[!] list_snapshots(%s): %s\n", table, pp_uic_last_error(h));
        return 1;
    }
    printf("snapshots: %s\n", snaps);
    pp_uic_free_string(snaps);
    return 0;
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
    const char* err = pp_uic_last_error(h);
    if (err && *err) {
        fprintf(stderr, "[!] pp_uic_open: %s\n", err);
        pp_uic_close(h);
        return 1;
    }
    int rc = 0;
    rc |= dump_table(h, "primes");
    rc |= dump_table(h, "decompositions");
    pp_uic_close(h);
    return rc;
}
