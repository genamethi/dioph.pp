#ifndef PRIMEPARTS_UI_ICEBERG_H
#define PRIMEPARTS_UI_ICEBERG_H

/*
 * C ABI over the iceberg-cpp read path used by the primeparts TUI.
 *
 * Opens a primeparts SqlCatalog warehouse (the directory containing
 * `catalog.db`), resolves the active `metadata.json` for `funbuns.primes`
 * and `funbuns.decompositions` via a single sqlite3 lookup, and exposes
 * snapshot / metric reads through iceberg-cpp.
 *
 * All json strings returned by these functions are heap-allocated and must
 * be freed with `pp_uic_free_string`. Functions that return an int return
 * 0 on success and a negative errno-like code on failure; the last error
 * message can be retrieved with `pp_uic_last_error` (handle-scoped).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pp_uic_handle pp_uic_handle;

/* Open a warehouse rooted at `warehouse_root` (the directory containing
 * `catalog.db`). Returns NULL on failure; the caller can pass NULL into
 * pp_uic_last_error to get a global open-time error. */
pp_uic_handle* pp_uic_open(const char* warehouse_root);

/* Close and free a handle. NULL is a no-op. */
void pp_uic_close(pp_uic_handle* h);

/* Last error message for this handle, or "" if there is none. The pointer
 * is owned by the handle and stable until the next call on it. */
const char* pp_uic_last_error(const pp_uic_handle* h);

/* Free a heap string returned from pp_uic_*_json. */
void pp_uic_free_string(char* s);

/* `table` must be "primes" or "decompositions". */

/* Maximum value of column `p` in the current snapshot. Computed from
 * manifest upper_bounds (no data scan). Returns -1 on error or empty. */
int64_t pp_uic_max_p(pp_uic_handle* h, const char* table);

/* Total record count in the current snapshot, summed from manifest list
 * `added_rows_count + existing_rows_count - deleted_rows_count`. Returns
 * -1 on error. */
int64_t pp_uic_total_rows(pp_uic_handle* h, const char* table);

/* All snapshots for `table`, ordered by sequence_number. JSON shape:
 *   {"snapshots":[
 *     {"snapshot_id":..., "parent_id":..., "sequence_number":...,
 *      "timestamp_ms":..., "operation":"append",
 *      "summary":{"funbuns.max_p":"...","funbuns.max_commit_seq":"...", ...},
 *      "manifest_list":"file:///..."}
 *   ], "current_snapshot_id":...}
 * Returns NULL on error. */
char* pp_uic_list_snapshots_json(pp_uic_handle* h, const char* table);

#ifdef __cplusplus
}
#endif

#endif  /* PRIMEPARTS_UI_ICEBERG_H */
