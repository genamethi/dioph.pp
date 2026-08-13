#include "primeparts/core.h"

#include <errno.h>
#include <pthread.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <flint/ulong_extras.h>
#include <gmp.h>
#include <primecount.h>
#include <primesieve.h>

#include "primeparts/higher_sweep.h"

const char *pp_status_message(int status)
{
    switch (status) {
    case PP_OK:
        return "ok";
    case PP_ERR_INVALID_ARGUMENT:
        return "invalid argument";
    case PP_ERR_OVERFLOW:
        return "integer overflow or unsupported range";
    case PP_ERR_ALLOC:
        return "allocation failed";
    case PP_ERR_LIBRARY:
        return "native math library error";
    default:
        return "unknown error";
    }
}

static void pow3_table_init(void);

static pthread_once_t g_tables_once = PTHREAD_ONCE_INIT;

int pp_init(void)
{
    pthread_once(&g_tables_once, pow3_table_init);
    return PP_OK;
}

void pp_shutdown(void)
{
}

void pp_batch_result_init(pp_batch_result *result)
{
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }
}

void pp_batch_result_clear(pp_batch_result *result)
{
    if (result == NULL) {
        return;
    }
    free(result->prime_p);
    free(result->prime_k);
    free(result->prime_flat_mask);
    free(result->higher_p);
    free(result->higher_m);
    free(result->higher_n);
    free(result->higher_q);
    memset(result, 0, sizeof(*result));
}

static size_t prime_row_bytes(const pp_batch_result *result)
{
    return sizeof(*result->prime_p) + sizeof(*result->prime_k)
        + sizeof(*result->prime_flat_mask);
}

static size_t higher_row_bytes(const pp_batch_result *result)
{
    return sizeof(*result->higher_p) + sizeof(*result->higher_m)
        + sizeof(*result->higher_n) + sizeof(*result->higher_q);
}

size_t pp_batch_result_used_bytes(const pp_batch_result *result)
{
    if (result == NULL) {
        return 0;
    }
    return result->prime_count * prime_row_bytes(result)
        + result->higher_count * higher_row_bytes(result);
}

size_t pp_batch_result_allocated_bytes(const pp_batch_result *result)
{
    if (result == NULL) {
        return 0;
    }
    return result->prime_capacity * prime_row_bytes(result)
        + result->higher_capacity * higher_row_bytes(result);
}

void pp_set_nth_prime_threads(int threads)
{
    if (threads > 0) {
        primecount_set_num_threads(threads);
    }
}

int64_t pp_prime_pi(int64_t n)
{
    if (n < 0) {
        return -1;
    }
    return primecount_pi(n);
}

int64_t pp_nth_prime(int64_t n)
{
    if (n <= 0) {
        return -1;
    }
    return primecount_nth_prime(n);
}

int64_t pp_next_prime(int64_t n)
{
    uint64_t p;

    if (n < 0) {
        return -1;
    }
    p = primesieve_nth_prime(1, (uint64_t)n);
    if (p == PRIMESIEVE_ERROR || p > (uint64_t)INT64_MAX) {
        return -1;
    }
    return (int64_t)p;
}

int64_t pp_previous_prime(int64_t n)
{
    uint64_t p;

    if (n <= 2) {
        return -1;
    }
    p = primesieve_nth_prime(-1, (uint64_t)n);
    if (p == PRIMESIEVE_ERROR || p > (uint64_t)INT64_MAX) {
        return -1;
    }
    return (int64_t)p;
}

static int floor_log2_u64(uint64_t x)
{
#if defined(__GNUC__) || defined(__clang__)
    return 63 - __builtin_clzll(x);
#else
    int log = -1;
    while (x != 0) {
        x >>= 1;
        log++;
    }
    return log;
#endif
}

int pp_is_prime_power_u64(uint64_t n, uint64_t *base, int32_t *exponent)
{
    ulong root = 0;
    int e;
    uint64_t sub_base = 0;
    int32_t sub_exp = 0;

    if (base == NULL || exponent == NULL) {
        return PP_ERR_INVALID_ARGUMENT;
    }
    *base = 0;
    *exponent = 0;

    if (n < 2 || n > (uint64_t)ULONG_MAX) {
        return PP_OK;
    }
    if (n_is_prime((ulong)n)) {
        *base = n;
        *exponent = 1;
        return PP_OK;
    }

    e = n_is_perfect_power(&root, (ulong)n);
    if (e <= 1 || root < 2) {
        return PP_OK;
    }

    if (pp_is_prime_power_u64((uint64_t)root, &sub_base, &sub_exp) != PP_OK) {
        return PP_ERR_LIBRARY;
    }
    if (sub_exp <= 0) {
        return PP_OK;
    }
    if (sub_exp > INT32_MAX / e) {
        return PP_ERR_OVERFLOW;
    }
    *base = sub_base;
    *exponent = sub_exp * e;
    return PP_OK;
}

int pp_is_prime_power_mpz(const mpz_t n, mpz_t base, int32_t *exponent)
{
    size_t bits;
    unsigned long k;

    if (base == NULL || exponent == NULL) {
        return PP_ERR_INVALID_ARGUMENT;
    }
    mpz_set_ui(base, 0);
    *exponent = 0;

    if (mpz_cmp_ui(n, 2) < 0) {
        return PP_OK;
    }
    if (mpz_fits_ulong_p(n)) {
        uint64_t b = 0;
        int32_t e = 0;
        int status = pp_is_prime_power_u64((uint64_t)mpz_get_ui(n), &b, &e);
        if (status != PP_OK) {
            return status;
        }
        if (e > 0) {
            mpz_set_ui(base, (unsigned long)b);
            *exponent = e;
        }
        return PP_OK;
    }
    if (mpz_probab_prime_p(n, 25) > 0) {
        mpz_set(base, n);
        *exponent = 1;
        return PP_OK;
    }
    if (!mpz_perfect_power_p(n)) {
        return PP_OK;
    }

    bits = mpz_sizeinbase(n, 2);
    for (k = (unsigned long)bits; k >= 2; k--) {
        mpz_t root;
        int exact;

        mpz_init(root);
        exact = mpz_root(root, n, k);
        if (exact && mpz_probab_prime_p(root, 25) > 0) {
            if (k > (unsigned long)INT32_MAX) {
                mpz_clear(root);
                return PP_ERR_OVERFLOW;
            }
            mpz_set(base, root);
            *exponent = (int32_t)k;
            mpz_clear(root);
            return PP_OK;
        }
        mpz_clear(root);
    }
    return PP_OK;
}

static int set_prime_capacity(pp_batch_result *result, size_t want)
{
    int64_t *new_p;
    int32_t *new_k;
    uint64_t *new_mask;

    if (want <= result->prime_capacity) {
        return PP_OK;
    }
    if (want > SIZE_MAX / sizeof(*new_p)) {
        return PP_ERR_OVERFLOW;
    }

    new_p = (int64_t *)realloc(result->prime_p, want * sizeof(*new_p));
    if (new_p == NULL) {
        return PP_ERR_ALLOC;
    }
    result->prime_p = new_p;

    new_k = (int32_t *)realloc(result->prime_k, want * sizeof(*new_k));
    if (new_k == NULL) {
        return PP_ERR_ALLOC;
    }
    result->prime_k = new_k;

    new_mask = (uint64_t *)realloc(result->prime_flat_mask, want * sizeof(*new_mask));
    if (new_mask == NULL) {
        return PP_ERR_ALLOC;
    }
    result->prime_flat_mask = new_mask;
    result->prime_capacity = want;
    return PP_OK;
}

static int set_higher_capacity(pp_batch_result *result, size_t want)
{
    int64_t *new_p;
    int32_t *new_m;
    int32_t *new_n;
    int64_t *new_q;

    if (want <= result->higher_capacity) {
        return PP_OK;
    }
    if (want > SIZE_MAX / sizeof(*new_p)) {
        return PP_ERR_OVERFLOW;
    }

    new_p = (int64_t *)realloc(result->higher_p, want * sizeof(*new_p));
    if (new_p == NULL) {
        return PP_ERR_ALLOC;
    }
    result->higher_p = new_p;

    new_m = (int32_t *)realloc(result->higher_m, want * sizeof(*new_m));
    if (new_m == NULL) {
        return PP_ERR_ALLOC;
    }
    result->higher_m = new_m;

    new_n = (int32_t *)realloc(result->higher_n, want * sizeof(*new_n));
    if (new_n == NULL) {
        return PP_ERR_ALLOC;
    }
    result->higher_n = new_n;

    new_q = (int64_t *)realloc(result->higher_q, want * sizeof(*new_q));
    if (new_q == NULL) {
        return PP_ERR_ALLOC;
    }
    result->higher_q = new_q;
    result->higher_capacity = want;
    return PP_OK;
}

static int grow_prime_rows(pp_batch_result *result, size_t needed)
{
    size_t want;

    if (needed <= result->prime_capacity) {
        return PP_OK;
    }
    want = result->prime_capacity == 0 ? needed : result->prime_capacity;
    while (want < needed) {
        if (want > SIZE_MAX / 2) {
            return PP_ERR_OVERFLOW;
        }
        want *= 2;
    }
    return set_prime_capacity(result, want);
}

static int grow_higher_rows(pp_batch_result *result, size_t needed)
{
    size_t want;

    if (needed <= result->higher_capacity) {
        return PP_OK;
    }
    want = result->higher_capacity == 0 ? needed : result->higher_capacity;
    while (want < needed) {
        if (want > SIZE_MAX / 2) {
            return PP_ERR_OVERFLOW;
        }
        want *= 2;
    }
    return set_higher_capacity(result, want);
}

static int write_prime(pp_batch_result *result, uint64_t p, int32_t k, uint64_t flat_mask)
{
    int status;

    status = grow_prime_rows(result, result->prime_count + 1);
    if (status != PP_OK) {
        return status;
    }
    result->prime_p[result->prime_count] = (int64_t)p;
    result->prime_k[result->prime_count] = k;
    result->prime_flat_mask[result->prime_count] = flat_mask;
    result->prime_count++;
    if (flat_mask != 0) {
        result->flat_prime_count++;
    }
    return PP_OK;
}

static void finalize_result(pp_batch_result *result)
{
    result->processed_count = (int64_t)result->prime_count;
    if (result->prime_count > 0) {
        result->first_p = result->prime_p[0];
        result->last_p = result->prime_p[result->prime_count - 1];
    }
}

static int write_higher(pp_batch_result *result, uint64_t p, int32_t m, int32_t n, uint64_t q)
{
    int status;

    status = grow_higher_rows(result, result->higher_count + 1);
    if (status != PP_OK) {
        return status;
    }
    result->higher_p[result->higher_count] = (int64_t)p;
    result->higher_m[result->higher_count] = m;
    result->higher_n[result->higher_count] = n;
    result->higher_q[result->higher_count] = (int64_t)q;
    result->higher_count++;
    return PP_OK;
}

#define PP_POW3_MAX 40

static uint64_t g_pow3[PP_POW3_MAX];
static signed char g_pow3_at_bits[65];

static void pow3_table_init(void)
{
    uint64_t v = 1;
    int n;
    int i;

    for (i = 0; i < 65; i++) {
        g_pow3_at_bits[i] = -1;
    }
    for (n = 0; n < PP_POW3_MAX; n++) {
        int bits = 64 - __builtin_clzll(v);
        g_pow3[n] = v;
        g_pow3_at_bits[bits] = (signed char)n;
        if (v > UINT64_MAX / 3) {
            break;
        }
        v *= 3;
    }
}

static int power_of_three_exponent(uint64_t q, int32_t *exponent)
{
    int bits = 64 - __builtin_clzll(q);
    int n = g_pow3_at_bits[bits];

    if (n > 0 && g_pow3[n] == q) {
        *exponent = n;
        return 1;
    }
    return 0;
}

static int process_prime(pp_batch_result *result, pp_higher_table *higher, uint64_t p)
{
    int max_m;
    int m;
    int status;
    int killed_parity;
    int32_t k = 0;
    uint64_t flat_mask = 0;
    uint64_t power;

    max_m = floor_log2_u64(p);
    killed_parity = (p % 3 == 2);
    power = 2;
    pp_higher_seek(higher, p);

    for (m = 1; m <= max_m; m++) {
        uint64_t q_candidate = p - power;
        uint64_t base = 0;
        int32_t exponent = 0;

        power <<= 1;
        if (q_candidate < 2) {
            continue;
        }
        if ((m & 1) == killed_parity) {
            if (power_of_three_exponent(q_candidate, &exponent)) {
                base = 3;
            }
        } else {
            const pp_higher_hit *hit = pp_higher_take(higher, p, (int32_t)m);

            if (hit != NULL) {
                base = (uint64_t)hit->q;
                exponent = hit->n;
            } else if (n_is_prime((ulong)q_candidate)) {
                base = q_candidate;
                exponent = 1;
            }
        }
        if (exponent <= 0) {
            continue;
        }
        if (exponent == 1) {
            flat_mask |= (uint64_t)1 << m;
        } else {
            status = write_higher(result, p, (int32_t)m, exponent, base);
            if (status != PP_OK) {
                return status;
            }
        }
        k++;
    }

    return write_prime(result, p, k, flat_mask);
}

static int count_prime(pp_higher_table *higher, uint64_t p, int64_t *partition_count)
{
    int max_m;
    int m;
    int killed_parity;
    int64_t local_partitions = 0;
    uint64_t power;

    if (p == 0 || p > (uint64_t)INT64_MAX) {
        return PP_ERR_OVERFLOW;
    }

    max_m = floor_log2_u64(p);
    killed_parity = (p % 3 == 2);
    power = 2;
    pp_higher_seek(higher, p);

    for (m = 1; m <= max_m; m++) {
        uint64_t q_candidate = p - power;
        int32_t exponent = 0;

        if (q_candidate >= 2) {
            if ((m & 1) == killed_parity) {
                /* Set A: 3 | q_candidate; a prime power here can only be 3^n. */
                power_of_three_exponent(q_candidate, &exponent);
            } else {
                const pp_higher_hit *hit = pp_higher_take(higher, p, (int32_t)m);

                if (hit != NULL) {
                    exponent = hit->n;
                } else if (n_is_prime((ulong)q_candidate)) {
                    exponent = 1;
                }
            }
            if (exponent > 0) {
                local_partitions++;
            }
        }
        power <<= 1;
    }

    *partition_count += local_partitions;
    return PP_OK;
}

static int prepare_result(pp_batch_result *out, int64_t start_idx, int64_t requested_count, size_t expected_count)
{
    size_t want;
    int status;

    out->prime_count = 0;
    out->flat_prime_count = 0;
    out->higher_count = 0;
    out->processed_count = 0;
    out->interrupted = 0;
    out->first_p = 0;
    out->last_p = 0;
    out->start_idx = start_idx;
    out->requested_count = requested_count;

    want = expected_count == 0 ? 1 : expected_count;
    if (want > SIZE_MAX / PP_SPAN_SLACK_NUM) {
        return PP_ERR_OVERFLOW;
    }
    status = set_prime_capacity(out, want * PP_SPAN_SLACK_NUM / PP_SPAN_SLACK_DEN);
    if (status != PP_OK) {
        return status;
    }
    return set_higher_capacity(out, PP_HIGHER_SEED_ROWS);
}

int pp_process_prime_array(const uint64_t *primes, size_t count, pp_batch_result *out)
{
    pp_higher_table higher;
    size_t i;
    int status;

    if (out == NULL || (primes == NULL && count != 0)) {
        return PP_ERR_INVALID_ARGUMENT;
    }
    for (i = 0; i < count; i++) {
        if (primes[i] < 2 || primes[i] > (uint64_t)PP_MAX_PRIME) {
            return PP_ERR_OVERFLOW;
        }
    }
    status = pp_init();
    if (status != PP_OK) {
        return status;
    }
    status = prepare_result(out, 0, (int64_t)count, count);
    if (status != PP_OK) {
        return status;
    }
    if (count == 0) {
        return PP_OK;
    }

    pp_higher_table_init(&higher);
    for (i = 0; i < count; i++) {
        status = pp_higher_sweep(&higher, primes[i], primes[i]);
        if (status != PP_OK) {
            pp_higher_table_clear(&higher);
            return status;
        }
        status = process_prime(out, &higher, primes[i]);
        if (status != PP_OK) {
            pp_higher_table_clear(&higher);
            return status;
        }
    }
    pp_higher_table_clear(&higher);
    finalize_result(out);
    return PP_OK;
}

int pp_process_prime_span(int64_t first_prime, int64_t end_prime,
                          int64_t expected_primes, pp_batch_result *out)
{
    int status;
    primesieve_iterator it;
    pp_higher_table higher;

    if (out == NULL || first_prime < 2 || end_prime < first_prime || expected_primes < 0) {
        return PP_ERR_INVALID_ARGUMENT;
    }
    if (end_prime > PP_MAX_PRIME + 1) {
        return PP_ERR_OVERFLOW;
    }

    status = pp_init();
    if (status != PP_OK) {
        return status;
    }

    status = prepare_result(out, 0, expected_primes, (size_t)expected_primes);
    if (status != PP_OK) {
        return status;
    }
    if (end_prime == first_prime) {
        return PP_OK;
    }

    pp_higher_table_init(&higher);
    status = pp_higher_sweep(&higher, (uint64_t)first_prime, (uint64_t)end_prime - 1);
    if (status != PP_OK) {
        pp_higher_table_clear(&higher);
        return status;
    }

    primesieve_init(&it);
    primesieve_jump_to(&it, (uint64_t)first_prime, (uint64_t)end_prime);
    for (;;) {
        uint64_t p = primesieve_next_prime(&it);
        if (p == PRIMESIEVE_ERROR || it.is_error) {
            primesieve_free_iterator(&it);
            pp_higher_table_clear(&higher);
            return PP_ERR_LIBRARY;
        }
        if (p >= (uint64_t)end_prime) {
            break;
        }
        status = process_prime(out, &higher, p);
        if (status != PP_OK) {
            primesieve_free_iterator(&it);
            pp_higher_table_clear(&higher);
            return status;
        }
    }
    primesieve_free_iterator(&it);
    pp_higher_table_clear(&higher);
    finalize_result(out);
    return PP_OK;
}

int pp_process_rank_batch(int64_t start_idx, int64_t count, pp_batch_result *out)
{
    int64_t first_prime;
    int64_t end_prime;
    int status;

    if (out == NULL || start_idx <= 0 || count < 0) {
        return PP_ERR_INVALID_ARGUMENT;
    }
    if (count == 0) {
        return prepare_result(out, start_idx, count, 0);
    }
    if (start_idx > INT64_MAX - count) {
        return PP_ERR_OVERFLOW;
    }
    if (start_idx + count - 1 > PP_MAX_PRIME_RANK) {
        return PP_ERR_OVERFLOW;
    }

    status = pp_init();
    if (status != PP_OK) {
        return status;
    }

    first_prime = pp_nth_prime(start_idx);
    end_prime = pp_nth_prime(start_idx + count);
    if (first_prime <= 0 || end_prime <= 0) {
        return PP_ERR_LIBRARY;
    }

    status = pp_process_prime_span(first_prime, end_prime, count, out);
    if (status != PP_OK) {
        return status;
    }
    out->start_idx = start_idx;
    out->requested_count = count;
    if (out->processed_count != count) {
        return PP_ERR_LIBRARY;
    }
    return PP_OK;
}

int pp_count_rank_batch(int64_t start_idx, int64_t count, pp_count_result *out)
{
    int64_t first_prime;
    int64_t end_prime;
    int status;
    primesieve_iterator it;
    pp_higher_table higher;

    if (out == NULL || start_idx <= 0 || count < 0) {
        return PP_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    out->start_idx = start_idx;
    out->requested_count = count;
    if (count == 0) {
        return PP_OK;
    }
    if (start_idx > INT64_MAX - count) {
        return PP_ERR_OVERFLOW;
    }
    if (start_idx + count - 1 > PP_MAX_PRIME_RANK) {
        return PP_ERR_OVERFLOW;
    }

    status = pp_init();
    if (status != PP_OK) {
        return status;
    }

    first_prime = pp_nth_prime(start_idx);
    end_prime = pp_nth_prime(start_idx + count);
    if (first_prime <= 0 || end_prime <= 0) {
        return PP_ERR_LIBRARY;
    }

    pp_higher_table_init(&higher);
    status = pp_higher_sweep(&higher, (uint64_t)first_prime, (uint64_t)end_prime - 1);
    if (status != PP_OK) {
        pp_higher_table_clear(&higher);
        return status;
    }

    primesieve_init(&it);
    primesieve_jump_to(&it, (uint64_t)first_prime, (uint64_t)end_prime);
    while (out->processed_count < count) {
        uint64_t p = primesieve_next_prime(&it);
        if (p == PRIMESIEVE_ERROR || it.is_error) {
            primesieve_free_iterator(&it);
            pp_higher_table_clear(&higher);
            return PP_ERR_LIBRARY;
        }
        if (p >= (uint64_t)end_prime) {
            break;
        }
        status = count_prime(&higher, p, &out->partition_count);
        if (status != PP_OK) {
            primesieve_free_iterator(&it);
            pp_higher_table_clear(&higher);
            return status;
        }
        out->processed_count++;
    }
    primesieve_free_iterator(&it);
    pp_higher_table_clear(&higher);

    if (out->processed_count != count) {
        return PP_ERR_LIBRARY;
    }
    return PP_OK;
}
