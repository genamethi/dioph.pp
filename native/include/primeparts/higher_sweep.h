#ifndef PRIMEPARTS_HIGHER_SWEEP_H
#define PRIMEPARTS_HIGHER_SWEEP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pp_higher_hit {
    int64_t p;
    int64_t q;
    int32_t m;
    int32_t n;
} pp_higher_hit;

typedef struct pp_higher_table {
    pp_higher_hit *hits;
    size_t count;
    size_t capacity;
    size_t cursor;
} pp_higher_table;

void pp_higher_table_init(pp_higher_table *table);
void pp_higher_table_clear(pp_higher_table *table);

int pp_higher_sweep(pp_higher_table *table, uint64_t lo, uint64_t hi);

void pp_higher_seek(pp_higher_table *table, uint64_t p);
const pp_higher_hit *pp_higher_take(pp_higher_table *table, uint64_t p, int32_t m);

#ifdef __cplusplus
}
#endif

#endif
