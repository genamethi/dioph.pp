#include "primeparts/core.h"

#include <errno.h>
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

int pp_init(void)
{
    return PP_OK;
}

void pp_shutdown(void)
{
}

/* Core options, set once before a (possibly threaded) batch and read read-only by
 * process_prime/count_prime. g_k_max <= 0 means no upper bound. */
static int32_t g_k_min = 0;
static int32_t g_k_max = 0;          /* <= 0 => no upper bound */
static int g_modular_filter = 1;     /* covering filter on by default */

void pp_set_options(int32_t k_min, int32_t k_max, int modular_filter)
{
    g_k_min = k_min < 0 ? 0 : k_min;
    g_k_max = k_max;
    g_modular_filter = modular_filter ? 1 : 0;
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
    free(result->decomp_p);
    free(result->decomp_m);
    free(result->decomp_n);
    free(result->decomp_q);
    memset(result, 0, sizeof(*result));
}

size_t pp_batch_result_used_bytes(const pp_batch_result *result)
{
    if (result == NULL) {
        return 0;
    }
    return result->prime_count * (sizeof(*result->prime_p) + sizeof(*result->prime_k))
        + result->decomp_count * (
            sizeof(*result->decomp_p)
            + sizeof(*result->decomp_m)
            + sizeof(*result->decomp_n)
            + sizeof(*result->decomp_q));
}

size_t pp_batch_result_allocated_bytes(const pp_batch_result *result)
{
    if (result == NULL) {
        return 0;
    }
    return result->prime_capacity * (sizeof(*result->prime_p) + sizeof(*result->prime_k))
        + result->decomp_capacity * (
            sizeof(*result->decomp_p)
            + sizeof(*result->decomp_m)
            + sizeof(*result->decomp_n)
            + sizeof(*result->decomp_q));
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

static int reserve_decomp_rows(pp_batch_result *result, size_t needed)
{
    int64_t *new_p;
    int32_t *new_m;
    int32_t *new_n;
    int64_t *new_q;
    size_t new_capacity;

    if (needed <= result->decomp_capacity) {
        return PP_OK;
    }
    new_capacity = result->decomp_capacity == 0 ? 1 : result->decomp_capacity;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2) {
            return PP_ERR_OVERFLOW;
        }
        new_capacity *= 2;
    }

    new_p = (int64_t *)realloc(result->decomp_p, new_capacity * sizeof(*new_p));
    if (new_p == NULL) {
        return PP_ERR_ALLOC;
    }
    result->decomp_p = new_p;

    new_m = (int32_t *)realloc(result->decomp_m, new_capacity * sizeof(*new_m));
    if (new_m == NULL) {
        return PP_ERR_ALLOC;
    }
    result->decomp_m = new_m;

    new_n = (int32_t *)realloc(result->decomp_n, new_capacity * sizeof(*new_n));
    if (new_n == NULL) {
        return PP_ERR_ALLOC;
    }
    result->decomp_n = new_n;

    new_q = (int64_t *)realloc(result->decomp_q, new_capacity * sizeof(*new_q));
    if (new_q == NULL) {
        return PP_ERR_ALLOC;
    }
    result->decomp_q = new_q;
    result->decomp_capacity = new_capacity;
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

static int write_decomp(pp_batch_result *result, uint64_t p, int32_t m, int32_t n, uint64_t q)
{
    int status;

    if (p > (uint64_t)INT64_MAX || q > (uint64_t)INT64_MAX) {
        return PP_ERR_OVERFLOW;
    }
    status = reserve_decomp_rows(result, result->decomp_count + 1);
    if (status != PP_OK) {
        return status;
    }
    result->decomp_p[result->decomp_count] = (int64_t)p;
    result->decomp_m[result->decomp_count] = m;
    result->decomp_n[result->decomp_count] = n;
    result->decomp_q[result->decomp_count] = (int64_t)q;
    result->decomp_count++;
    return PP_OK;
}

static bool exhausted_divides(uint64_t q_candidate, const uint64_t *exhausted, size_t n_exhausted)
{
    size_t i;

    for (i = 0; i < n_exhausted; i++) {
        if (exhausted[i] != 0 && q_candidate % exhausted[i] == 0) {
            return true;
        }
    }
    return false;
}

static void increment_hit(uint64_t base, uint64_t *hit_base, unsigned char *hit_count,
                          size_t *n_hits, uint64_t *exhausted, size_t *n_exhausted)
{
    size_t i;

    for (i = 0; i < *n_hits; i++) {
        if (hit_base[i] == base) {
            if (hit_count[i] < UCHAR_MAX) {
                hit_count[i]++;
            }
            if (hit_count[i] >= 2 && *n_exhausted < 64) {
                exhausted[*n_exhausted] = base;
                (*n_exhausted)++;
            }
            return;
        }
    }

    if (*n_hits < 64) {
        hit_base[*n_hits] = base;
        hit_count[*n_hits] = 1;
        (*n_hits)++;
    }
}

/* Modular covering filter (semantics-preserving). q | (p - 2^m) iff p == 2^m (mod q);
 * we test that directly from 2^m mod q tracked per m, exact for any order (ord_11(2)=10
 * and ord_17(2)=8 don't share a fixed period, so no fixed-width mask). >= 2 covering
 * primes dividing p - 2^m => not a prime power; exactly one (q) => base must be q (a
 * cheap exact-power check); none => general predicate. Decompositions are identical to
 * the unfiltered path. */
static const uint64_t kCoverQ[6] = {3, 5, 7, 11, 13, 17};

static int covering_count(const uint64_t *pr, const uint64_t *pm, uint64_t *sole)
{
    int n = 0;
    for (size_t i = 0; i < 6; i++) {
        if (pm[i] == pr[i]) { n++; *sole = kCoverQ[i]; }
    }
    return n;
}

static int exact_power_of(uint64_t r, uint64_t base, int32_t *exponent)
{
    int e = 0;
    while (r % base == 0) { r /= base; e++; }
    if (r == 1 && e > 0) { *exponent = e; return 1; }
    return 0;
}

static int process_prime(pp_batch_result *result, uint64_t p)
{
    int max_m;
    int m;
    int status;
    size_t decomp_start;
    uint64_t power;
    uint64_t hit_base[64];
    unsigned char hit_count[64];
    size_t n_hits = 0;
    uint64_t exhausted[64];
    size_t n_exhausted = 0;

    if (p == 0 || p > (uint64_t)INT64_MAX) {
        return PP_ERR_OVERFLOW;
    }

    max_m = floor_log2_u64(p);
    decomp_start = result->decomp_count;
    power = 2;
    memset(hit_base, 0, sizeof(hit_base));
    memset(hit_count, 0, sizeof(hit_count));
    memset(exhausted, 0, sizeof(exhausted));

    uint64_t pr[6], pm[6];
    for (size_t ci = 0; ci < 6; ci++) { pr[ci] = p % kCoverQ[ci]; pm[ci] = 2 % kCoverQ[ci]; }

    for (m = 1; m <= max_m; m++) {
        uint64_t q_candidate = p - power;
        uint64_t base = 0;
        int32_t exponent = 0;
        int found = 0;

        if (q_candidate >= 2 && !exhausted_divides(q_candidate, exhausted, n_exhausted)) {
            uint64_t sole = 0;
            int nc = g_modular_filter ? covering_count(pr, pm, &sole) : 0;
            if (g_modular_filter && nc >= 2) {
                found = 0;  /* >= 2 distinct covering factors: not a prime power */
            } else if (g_modular_filter && nc == 1) {
                found = exact_power_of(q_candidate, sole, &exponent);
                if (found) base = sole;
            } else {
                status = pp_is_prime_power_u64(q_candidate, &base, &exponent);
                if (status != PP_OK) {
                    return status;
                }
                found = (exponent > 0);
            }
            if (found) {
                status = write_decomp(result, p, (int32_t)m, exponent, base);
                if (status != PP_OK) {
                    return status;
                }
                increment_hit(base, hit_base, hit_count, &n_hits, exhausted, &n_exhausted);
                /* k-range early-out: once k exceeds the upper bound this prime is
                 * excluded, so stop searching its remaining positions. */
                if (g_k_max > 0 && (int32_t)(result->decomp_count - decomp_start) > g_k_max) {
                    break;
                }
            }
        }
        power <<= 1;
        if (g_modular_filter) {
            for (size_t ci = 0; ci < 6; ci++) pm[ci] = (pm[ci] * 2) % kCoverQ[ci];
        }
    }

    {
        int32_t k = (int32_t)(result->decomp_count - decomp_start);
        if (k < g_k_min || (g_k_max > 0 && k > g_k_max)) {
            result->decomp_count = decomp_start;  /* discard: out of k-range */
            return PP_OK;
        }
        return write_prime(result, p, k);
    }
}

static int count_prime(uint64_t p, int64_t *decomp_count)
{
    int max_m;
    int m;
    int status;
    int64_t local_decomps = 0;
    uint64_t power;
    uint64_t hit_base[64];
    unsigned char hit_count[64];
    size_t n_hits = 0;
    uint64_t exhausted[64];
    size_t n_exhausted = 0;

    if (p == 0 || p > (uint64_t)INT64_MAX) {
        return PP_ERR_OVERFLOW;
    }

    max_m = floor_log2_u64(p);
    power = 2;
    memset(hit_base, 0, sizeof(hit_base));
    memset(hit_count, 0, sizeof(hit_count));
    memset(exhausted, 0, sizeof(exhausted));

    uint64_t pr[6], pm[6];
    for (size_t ci = 0; ci < 6; ci++) { pr[ci] = p % kCoverQ[ci]; pm[ci] = 2 % kCoverQ[ci]; }

    for (m = 1; m <= max_m; m++) {
        uint64_t q_candidate = p - power;
        uint64_t base = 0;
        int32_t exponent = 0;

        if (q_candidate >= 2 && !exhausted_divides(q_candidate, exhausted, n_exhausted)) {
            uint64_t sole = 0;
            int nc = g_modular_filter ? covering_count(pr, pm, &sole) : 0;
            int found = 0;
            if (g_modular_filter && nc >= 2) {
                found = 0;
            } else if (g_modular_filter && nc == 1) {
                found = exact_power_of(q_candidate, sole, &exponent);
                if (found) base = sole;
            } else {
                status = pp_is_prime_power_u64(q_candidate, &base, &exponent);
                if (status != PP_OK) {
                    return status;
                }
                found = (exponent > 0);
            }
            if (found) {
                local_decomps++;
                increment_hit(base, hit_base, hit_count, &n_hits, exhausted, &n_exhausted);
            }
        }
        power <<= 1;
        if (g_modular_filter) {
            for (size_t ci = 0; ci < 6; ci++) pm[ci] = (pm[ci] * 2) % kCoverQ[ci];
        }
    }

    *decomp_count += local_decomps;
    return PP_OK;
}

static int prepare_result(pp_batch_result *out, int64_t start_idx, int64_t requested_count, size_t expected_count)
{
    size_t decomp_capacity;
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
    decomp_capacity = (expected_count * 183) / 100 + 1;
    status = reserve_decomp_rows(out, decomp_capacity == 0 ? 1 : decomp_capacity);
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
    /* Drive by primes iterated, not primes written: the k-range filter may discard
     * primes (no write_prime), but each is still processed and counts toward count. */
    int64_t processed = 0;
    while (processed < count) {
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
        processed++;
    }
    primesieve_free_iterator(&it);
    out->processed_count = processed;

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
        status = count_prime(p, &out->decomp_count);
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
