#include "primeparts/core.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

static void check_partition(const pp_batch_result *r, size_t i,
                         int64_t p, int32_t m, int32_t n, int64_t q)
{
    CHECK(i < r->partition_count);
    CHECK(r->partition_p[i] == p);
    CHECK(r->partition_m[i] == m);
    CHECK(r->partition_n[i] == n);
    CHECK(r->partition_q[i] == q);
}

int main(void)
{
    pp_batch_result result;
    int status;
    size_t i;
    const int64_t expected_p[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29};
    const int32_t expected_k[] = {0, 0, 1, 2, 3, 3, 2, 3, 2, 3};

    CHECK(pp_init() == PP_OK);
    CHECK(pp_prime_pi(7) == 4);
    CHECK(pp_nth_prime(10) == 29);
    CHECK(pp_previous_prime(10) == 7);
    {
        uint64_t base = 0;
        int32_t exponent = 0;
        CHECK(pp_is_prime_power_u64(64, &base, &exponent) == PP_OK);
        CHECK(base == 2);
        CHECK(exponent == 6);
        CHECK(pp_is_prime_power_u64(81, &base, &exponent) == PP_OK);
        CHECK(base == 3);
        CHECK(exponent == 4);
        CHECK(pp_is_prime_power_u64(12, &base, &exponent) == PP_OK);
        CHECK(exponent == 0);
    }

    pp_batch_result_init(&result);
    status = pp_process_rank_batch(1, 10, &result);
    CHECK(status == PP_OK);
    CHECK(result.processed_count == 10);
    CHECK(result.prime_count == 10);
    CHECK(result.partition_count == 19);

    for (i = 0; i < 10; i++) {
        CHECK(result.prime_p[i] == expected_p[i]);
        CHECK(result.prime_k[i] == expected_k[i]);
    }

    check_partition(&result, 0, 5, 1, 1, 3);
    check_partition(&result, 1, 7, 1, 1, 5);
    check_partition(&result, 2, 7, 2, 1, 3);
    check_partition(&result, 3, 11, 1, 2, 3);
    check_partition(&result, 4, 11, 2, 1, 7);
    check_partition(&result, 5, 11, 3, 1, 3);
    check_partition(&result, 18, 29, 4, 1, 13);

    pp_batch_result_clear(&result);
    {
        pp_count_result counts;
        CHECK(pp_count_rank_batch(1, 10, &counts) == PP_OK);
        CHECK(counts.processed_count == 10);
        CHECK(counts.partition_count == 19);
    }
    pp_shutdown();
    return 0;
}
