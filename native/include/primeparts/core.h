#ifndef PRIMEPARTS_CORE_H
#define PRIMEPARTS_CORE_H

#include <stddef.h>
#include <stdint.h>

#include <gmp.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PP_OK = 0,
    PP_ERR_INVALID_ARGUMENT = -1,
    PP_ERR_OVERFLOW = -2,
    PP_ERR_ALLOC = -3,
    PP_ERR_LIBRARY = -4
};

typedef struct pp_batch_result {
    int64_t *prime_p;
    int32_t *prime_k;
    int64_t *decomp_p;
    int32_t *decomp_m;
    int32_t *decomp_n;
    int64_t *decomp_q;

    size_t prime_count;
    size_t decomp_count;
    size_t prime_capacity;
    size_t decomp_capacity;

    int64_t processed_count;
    int64_t start_idx;
    int64_t requested_count;
    int interrupted;
    int64_t first_p;
    int64_t last_p;
} pp_batch_result;

typedef struct pp_count_result {
    int64_t processed_count;
    int64_t decomp_count;
    int64_t start_idx;
    int64_t requested_count;
} pp_count_result;

const char *pp_status_message(int status);

int pp_init(void);
void pp_shutdown(void);

/* Core options (set once before a batch): keep primes with k_min <= k <= k_max
 * (k_max <= 0 = no upper bound); modular_filter != 0 enables the covering filter. */
void pp_set_options(int32_t k_min, int32_t k_max, int modular_filter);

void pp_batch_result_init(pp_batch_result *result);
void pp_batch_result_clear(pp_batch_result *result);
size_t pp_batch_result_used_bytes(const pp_batch_result *result);
size_t pp_batch_result_allocated_bytes(const pp_batch_result *result);

int64_t pp_prime_pi(int64_t n);
int64_t pp_nth_prime(int64_t n);
int64_t pp_next_prime(int64_t n);
int64_t pp_previous_prime(int64_t n);

int pp_is_prime_power_u64(uint64_t n, uint64_t *base, int32_t *exponent);
int pp_is_prime_power_mpz(const mpz_t n, mpz_t base, int32_t *exponent);

int pp_process_rank_batch(int64_t start_idx, int64_t count, pp_batch_result *out);
int pp_process_prime_array(const uint64_t *primes, size_t count, pp_batch_result *out);
int pp_count_rank_batch(int64_t start_idx, int64_t count, pp_count_result *out);

#ifdef __cplusplus
}
#endif

#endif
