#include "primeparts/core.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct worker_args {
    int64_t start_idx;
    int64_t count;
    int status;
    pp_count_result result;
} worker_args;

static void usage(FILE *stream)
{
    fprintf(stream,
            "usage: primeparts-bench --start-idx N --count N [--threads N]\n"
            "\n"
            "Count prime-power partitions without materializing output rows.\n"
            "This benchmarks the native core hot loop only.\n");
}

static int parse_i64(const char *text, int64_t *out)
{
    char *end = NULL;
    long long value;

    errno = 0;
    value = strtoll(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return -1;
    }
    *out = (int64_t)value;
    return 0;
}

static double monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void *worker_main(void *ptr)
{
    worker_args *args = (worker_args *)ptr;
    args->status = pp_count_rank_batch(args->start_idx, args->count, &args->result);
    return NULL;
}

int main(int argc, char **argv)
{
    static const struct option long_options[] = {
        {"start-idx", required_argument, NULL, 1000},
        {"count", required_argument, NULL, 'n'},
        {"threads", required_argument, NULL, 't'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    int opt;
    int threads = 0;
    int64_t start_idx = 0;
    int64_t count = -1;
    pthread_t *thread_ids;
    worker_args *args;
    int64_t base;
    int64_t rem;
    int64_t next_start;
    int64_t processed = 0;
    int64_t partitions = 0;
    double t0;
    double elapsed;
    int i;

    while ((opt = getopt_long(argc, argv, "n:t:h", long_options, NULL)) != -1) {
        switch (opt) {
        case 1000:
            if (parse_i64(optarg, &start_idx) != 0) {
                fprintf(stderr, "invalid --start-idx: %s\n", optarg);
                return 2;
            }
            break;
        case 'n':
            if (parse_i64(optarg, &count) != 0) {
                fprintf(stderr, "invalid --count: %s\n", optarg);
                return 2;
            }
            break;
        case 't': {
            int64_t parsed = 0;
            if (parse_i64(optarg, &parsed) != 0 || parsed <= 0 || parsed > 4096) {
                fprintf(stderr, "invalid --threads: %s\n", optarg);
                return 2;
            }
            threads = (int)parsed;
            break;
        }
        case 'h':
            usage(stdout);
            return 0;
        default:
            usage(stderr);
            return 2;
        }
    }

    if (start_idx <= 0 || count < 0) {
        usage(stderr);
        return 2;
    }
    if (threads <= 0) {
        long nproc = sysconf(_SC_NPROCESSORS_ONLN);
        threads = nproc > 0 ? (int)nproc : 1;
    }
    if ((int64_t)threads > count && count > 0) {
        threads = (int)count;
    }
    if (threads <= 0) {
        threads = 1;
    }

    thread_ids = (pthread_t *)calloc((size_t)threads, sizeof(*thread_ids));
    args = (worker_args *)calloc((size_t)threads, sizeof(*args));
    if (thread_ids == NULL || args == NULL) {
        free(thread_ids);
        free(args);
        return 1;
    }

    base = count / threads;
    rem = count % threads;
    next_start = start_idx;

    t0 = monotonic_seconds();
    for (i = 0; i < threads; i++) {
        args[i].start_idx = next_start;
        args[i].count = base + (i < rem ? 1 : 0);
        next_start += args[i].count;
        if (pthread_create(&thread_ids[i], NULL, worker_main, &args[i]) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            free(thread_ids);
            free(args);
            return 1;
        }
    }

    for (i = 0; i < threads; i++) {
        pthread_join(thread_ids[i], NULL);
        if (args[i].status != PP_OK) {
            fprintf(stderr, "worker %d failed: %s\n", i, pp_status_message(args[i].status));
            free(thread_ids);
            free(args);
            pp_shutdown();
            return 1;
        }
        processed += args[i].result.processed_count;
        partitions += args[i].result.partition_count;
    }
    elapsed = monotonic_seconds() - t0;

    printf("threads=%d processed=%" PRId64 " partitions=%" PRId64
           " elapsed_s=%.6f primes_per_s=%.0f\n",
           threads, processed, partitions, elapsed,
           elapsed > 0.0 ? (double)processed / elapsed : 0.0);

    free(thread_ids);
    free(args);
    pp_shutdown();
    return 0;
}
