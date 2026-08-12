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
    free(result->partition_p);
    free(result->partition_m);
    free(result->partition_n);
    free(result->partition_q);
    memset(result, 0, sizeof(*result));
}

size_t pp_batch_result_used_bytes(const pp_batch_result *result)
{
    if (result == NULL) {
        return 0;
    }
    return result->prime_count * (sizeof(*result->prime_p) + sizeof(*result->prime_k))
        + result->partition_count * (
            sizeof(*result->partition_p)
            + sizeof(*result->partition_m)
            + sizeof(*result->partition_n)
            + sizeof(*result->partition_q));
}

size_t pp_batch_result_allocated_bytes(const pp_batch_result *result)
{
    if (result == NULL) {
        return 0;
    }
    return result->prime_capacity * (sizeof(*result->prime_p) + sizeof(*result->prime_k))
        + result->partition_capacity * (
            sizeof(*result->partition_p)
            + sizeof(*result->partition_m)
            + sizeof(*result->partition_n)
            + sizeof(*result->partition_q));
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

static int reserve_prime_rows(pp_batch_result *result, size_t needed)
{
    int64_t *new_p;
    int32_t *new_k;
    size_t new_capacity;

    if (needed <= result->prime_capacity) {
        return PP_OK;
    }
    new_capacity = result->prime_capacity == 0 ? 1 : result->prime_capacity;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2) {
            return PP_ERR_OVERFLOW;
        }
        new_capacity *= 2;
    }

    new_p = (int64_t *)realloc(result->prime_p, new_capacity * sizeof(*new_p));
    if (new_p == NULL) {
        return PP_ERR_ALLOC;
    }
    result->prime_p = new_p;

    new_k = (int32_t *)realloc(result->prime_k, new_capacity * sizeof(*new_k));
    if (new_k == NULL) {
        return PP_ERR_ALLOC;
    }
    result->prime_k = new_k;
    result->prime_capacity = new_capacity;
    return PP_OK;
}

static int reserve_partition_rows(pp_batch_result *result, size_t needed)
{
    int64_t *new_p;
    int32_t *new_m;
    int32_t *new_n;
    int64_t *new_q;
    size_t new_capacity;

    if (needed <= result->partition_capacity) {
        return PP_OK;
    }
    new_capacity = result->partition_capacity == 0 ? 1 : result->partition_capacity;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2) {
            return PP_ERR_OVERFLOW;
        }
        new_capacity *= 2;
    }

    new_p = (int64_t *)realloc(result->partition_p, new_capacity * sizeof(*new_p));
    if (new_p == NULL) {
        return PP_ERR_ALLOC;
    }
    result->partition_p = new_p;

    new_m = (int32_t *)realloc(result->partition_m, new_capacity * sizeof(*new_m));
    if (new_m == NULL) {
        return PP_ERR_ALLOC;
    }
    result->partition_m = new_m;

    new_n = (int32_t *)realloc(result->partition_n, new_capacity * sizeof(*new_n));
    if (new_n == NULL) {
        return PP_ERR_ALLOC;
    }
    result->partition_n = new_n;

    new_q = (int64_t *)realloc(result->partition_q, new_capacity * sizeof(*new_q));
    if (new_q == NULL) {
        return PP_ERR_ALLOC;
    }
    result->partition_q = new_q;
    result->partition_capacity = new_capacity;
    return PP_OK;
}

static int write_prime(pp_batch_result *result, uint64_t p, int32_t k)
{
    int status;

    if (p > (uint64_t)INT64_MAX) {
        return PP_ERR_OVERFLOW;
    }
    status = reserve_prime_rows(result, result->prime_count + 1);
    if (status != PP_OK) {
        return status;
    }
    result->prime_p[result->prime_count] = (int64_t)p;
    result->prime_k[result->prime_count] = k;
    if (result->prime_count == 0) {
        result->first_p = (int64_t)p;
    }
    result->last_p = (int64_t)p;
    result->prime_count++;
    result->processed_count = (int64_t)result->prime_count;
    return PP_OK;
}

static int write_partition(pp_batch_result *result, uint64_t p, int32_t m, int32_t n, uint64_t q)
{
    int status;

    if (p > (uint64_t)INT64_MAX || q > (uint64_t)INT64_MAX) {
        return PP_ERR_OVERFLOW;
    }
    status = reserve_partition_rows(result, result->partition_count + 1);
    if (status != PP_OK) {
        return status;
    }
    result->partition_p[result->partition_count] = (int64_t)p;
    result->partition_m[result->partition_count] = m;
    result->partition_n[result->partition_count] = n;
    result->partition_q[result->partition_count] = (int64_t)q;
    result->partition_count++;
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

static int process_prime(pp_batch_result *result, uint64_t p)
{
    int max_m;
    int m;
    int status;
    int killed_parity;
    size_t partition_start;
    uint64_t power;

    if (p == 0 || p > (uint64_t)INT64_MAX) {
        return PP_ERR_OVERFLOW;
    }

    max_m = floor_log2_u64(p);
    killed_parity = (p % 3 == 2);
    partition_start = result->partition_count;
    power = 2;

    for (m = 1; m <= max_m; m++) {
        uint64_t q_candidate = p - power;
        uint64_t base = 0;
        int32_t exponent = 0;

        if (q_candidate >= 2) {
            if ((m & 1) == killed_parity) {
                /* Set A: 3 | q_candidate; a prime power here can only be 3^n. */
                if (power_of_three_exponent(q_candidate, &exponent)) {
                    base = 3;
                }
            } else {
                status = pp_is_prime_power_u64(q_candidate, &base, &exponent);
                if (status != PP_OK) {
                    return status;
                }
            }
            if (exponent > 0) {
                status = write_partition(result, p, (int32_t)m, exponent, base);
                if (status != PP_OK) {
                    return status;
                }
            }
        }
        power <<= 1;
    }

    return write_prime(result, p, (int32_t)(result->partition_count - partition_start));
}

static int count_prime(uint64_t p, int64_t *partition_count)
{
    int max_m;
    int m;
    int status;
    int killed_parity;
    int64_t local_partitions = 0;
    uint64_t power;

    if (p == 0 || p > (uint64_t)INT64_MAX) {
        return PP_ERR_OVERFLOW;
    }

    max_m = floor_log2_u64(p);
    killed_parity = (p % 3 == 2);
    power = 2;

    for (m = 1; m <= max_m; m++) {
        uint64_t q_candidate = p - power;
        uint64_t base = 0;
        int32_t exponent = 0;

        if (q_candidate >= 2) {
            if ((m & 1) == killed_parity) {
                /* Set A: 3 | q_candidate; a prime power here can only be 3^n. */
                if (power_of_three_exponent(q_candidate, &exponent)) {
                    base = 3;
                }
            } else {
                status = pp_is_prime_power_u64(q_candidate, &base, &exponent);
                if (status != PP_OK) {
                    return status;
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
    size_t partition_capacity;
    int status;

    pp_batch_result_clear(out);
    out->start_idx = start_idx;
    out->requested_count = requested_count;

    status = reserve_prime_rows(out, expected_count == 0 ? 1 : expected_count);
    if (status != PP_OK) {
        return status;
    }

    if (expected_count > (SIZE_MAX - 1) / 183) {
        return PP_ERR_OVERFLOW;
    }
    partition_capacity = (expected_count * 183) / 100 + 1;
    status = reserve_partition_rows(out, partition_capacity == 0 ? 1 : partition_capacity);
    if (status != PP_OK) {
        return status;
    }
    return PP_OK;
}

int pp_process_prime_array(const uint64_t *primes, size_t count, pp_batch_result *out)
{
    size_t i;
    int status;

    if (out == NULL || (primes == NULL && count != 0)) {
        return PP_ERR_INVALID_ARGUMENT;
    }
    status = pp_init();
    if (status != PP_OK) {
        return status;
    }
    status = prepare_result(out, 0, (int64_t)count, count);
    if (status != PP_OK) {
        return status;
    }

    for (i = 0; i < count; i++) {
        status = process_prime(out, primes[i]);
        if (status != PP_OK) {
            return status;
        }
    }
    return PP_OK;
}

int pp_process_rank_batch(int64_t start_idx, int64_t count, pp_batch_result *out)
{
    int64_t first_prime;
    int64_t end_prime;
    int status;
    primesieve_iterator it;

    if (out == NULL || start_idx <= 0 || count < 0) {
        return PP_ERR_INVALID_ARGUMENT;
    }
    if (count == 0) {
        return prepare_result(out, start_idx, count, 0);
    }
    if (start_idx > INT64_MAX - count) {
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

    status = prepare_result(out, start_idx, count, (size_t)count);
    if (status != PP_OK) {
        return status;
    }

    primesieve_init(&it);
    primesieve_jump_to(&it, (uint64_t)first_prime, (uint64_t)end_prime);
    while (out->processed_count < count) {
        uint64_t p = primesieve_next_prime(&it);
        if (p == PRIMESIEVE_ERROR || it.is_error) {
            primesieve_free_iterator(&it);
            return PP_ERR_LIBRARY;
        }
        if (p >= (uint64_t)end_prime) {
            break;
        }
        status = process_prime(out, p);
        if (status != PP_OK) {
            primesieve_free_iterator(&it);
            return status;
        }
    }
    primesieve_free_iterator(&it);

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

    status = pp_init();
    if (status != PP_OK) {
        return status;
    }

    first_prime = pp_nth_prime(start_idx);
    end_prime = pp_nth_prime(start_idx + count);
    if (first_prime <= 0 || end_prime <= 0) {
        return PP_ERR_LIBRARY;
    }

    primesieve_init(&it);
    primesieve_jump_to(&it, (uint64_t)first_prime, (uint64_t)end_prime);
    while (out->processed_count < count) {
        uint64_t p = primesieve_next_prime(&it);
        if (p == PRIMESIEVE_ERROR || it.is_error) {
            primesieve_free_iterator(&it);
            return PP_ERR_LIBRARY;
        }
        if (p >= (uint64_t)end_prime) {
            break;
        }
        status = count_prime(p, &out->partition_count);
        if (status != PP_OK) {
            primesieve_free_iterator(&it);
            return status;
        }
        out->processed_count++;
    }
    primesieve_free_iterator(&it);

    if (out->processed_count != count) {
        return PP_ERR_LIBRARY;
    }
    return PP_OK;
}
