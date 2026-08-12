#include "primeparts/higher_sweep.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <flint/ulong_extras.h>

#include "primeparts/core.h"

enum {
    PP_HIGHER_MIN_BASE = 5,
    PP_HIGHER_MIN_EXPONENT = 2
};

void pp_higher_table_init(pp_higher_table *table)
{
    if (table != NULL) {
        memset(table, 0, sizeof(*table));
    }
}

void pp_higher_table_clear(pp_higher_table *table)
{
    if (table == NULL) {
        return;
    }
    free(table->hits);
    memset(table, 0, sizeof(*table));
}

static uint64_t root_floor(uint64_t x, int n)
{
    return (uint64_t)n_root((ulong)x, (ulong)n);
}

static uint64_t root_ceil(uint64_t x, int n)
{
    ulong remainder = 0;
    ulong root = n_rootrem(&remainder, (ulong)x, (ulong)n);

    return remainder == 0 ? (uint64_t)root : (uint64_t)root + 1;
}

static int table_push(pp_higher_table *table, uint64_t p, int32_t m, int32_t n, uint64_t q)
{
    if (table->count == table->capacity) {
        size_t capacity = table->capacity == 0 ? 64 : table->capacity * 2;
        pp_higher_hit *hits;

        if (capacity > SIZE_MAX / sizeof(*hits)) {
            return PP_ERR_OVERFLOW;
        }
        hits = (pp_higher_hit *)realloc(table->hits, capacity * sizeof(*hits));
        if (hits == NULL) {
            return PP_ERR_ALLOC;
        }
        table->hits = hits;
        table->capacity = capacity;
    }
    table->hits[table->count].p = (int64_t)p;
    table->hits[table->count].q = (int64_t)q;
    table->hits[table->count].m = m;
    table->hits[table->count].n = n;
    table->count++;
    return PP_OK;
}

static int hit_compare(const void *va, const void *vb)
{
    const pp_higher_hit *a = (const pp_higher_hit *)va;
    const pp_higher_hit *b = (const pp_higher_hit *)vb;

    if (a->p != b->p) {
        return a->p < b->p ? -1 : 1;
    }
    return a->m - b->m;
}

static int sweep_slot(pp_higher_table *table, uint64_t lo, uint64_t hi,
                      uint64_t pow2, int32_t m, int32_t n)
{
    uint64_t base_lo;
    uint64_t base_hi;
    uint64_t base;

    base_hi = root_floor(hi - pow2, n);
    if (base_hi < PP_HIGHER_MIN_BASE) {
        return PP_OK;
    }
    base_lo = lo > pow2 ? root_ceil(lo - pow2, n) : PP_HIGHER_MIN_BASE;
    if (base_lo < PP_HIGHER_MIN_BASE) {
        base_lo = PP_HIGHER_MIN_BASE;
    }
    base_lo |= 1;

    for (base = base_lo; base <= base_hi; base += 2) {
        uint64_t power;
        uint64_t p;
        int status;

        if (base % 3 == 0 || !n_is_prime((ulong)base)) {
            continue;
        }
        power = n_pow((ulong)base, (ulong)n);
        p = power + pow2;
        if (p < lo || p > hi || !n_is_prime((ulong)p)) {
            continue;
        }
        status = table_push(table, p, m, n, base);
        if (status != PP_OK) {
            return status;
        }
    }
    return PP_OK;
}

int pp_higher_sweep(pp_higher_table *table, uint64_t lo, uint64_t hi)
{
    int n;

    if (table == NULL) {
        return PP_ERR_INVALID_ARGUMENT;
    }
    table->count = 0;
    table->cursor = 0;

    if (hi > (uint64_t)INT64_MAX) {
        hi = (uint64_t)INT64_MAX;
    }
    if (lo > hi) {
        return PP_OK;
    }

    for (n = PP_HIGHER_MIN_EXPONENT; root_floor(hi, n) >= PP_HIGHER_MIN_BASE; n++) {
        uint64_t pow2 = 2;
        int32_t m;

        for (m = 1; pow2 < hi; m++) {
            int status = sweep_slot(table, lo, hi, pow2, m, (int32_t)n);

            if (status != PP_OK) {
                return status;
            }
            pow2 <<= 1;
        }
    }

    qsort(table->hits, table->count, sizeof(*table->hits), hit_compare);
    return PP_OK;
}

void pp_higher_seek(pp_higher_table *table, uint64_t p)
{
    while (table->cursor < table->count && (uint64_t)table->hits[table->cursor].p < p) {
        table->cursor++;
    }
}

const pp_higher_hit *pp_higher_take(pp_higher_table *table, uint64_t p, int32_t m)
{
    const pp_higher_hit *hit;

    if (table->cursor >= table->count) {
        return NULL;
    }
    hit = &table->hits[table->cursor];
    if ((uint64_t)hit->p != p || hit->m != m) {
        return NULL;
    }
    table->cursor++;
    return hit;
}
