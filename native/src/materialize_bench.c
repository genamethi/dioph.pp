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

typedef struct materialize_shared {
    int64_t start_idx;
    int64_t count;
    int64_t chunk_primes;
    int64_t total_chunks;
    int64_t next_chunk;

    pthread_mutex_t lock;

    int failed;
    int first_status;
    int64_t chunks_completed;
    int64_t processed;
    int64_t decompositions;
    int64_t first_p;
    int64_t last_p;
    size_t used_bytes_total;
    size_t allocated_bytes_total;
    size_t max_chunk_used_bytes;
    size_t max_chunk_allocated_bytes;
} materialize_shared;

typedef struct materialize_worker {
    int id;
    materialize_shared *shared;
} materialize_worker;

static void usage(FILE *stream)
{
    fprintf(stream,
            "usage: primeparts-materialize-bench --start-idx N --count N [--threads N] [--chunk-primes N]\n"
            "\n"
            "Materialize threaded columnar batches without writing them.\n"
            "Default --chunk-primes is 500000.\n");
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

static void update_stats(materialize_shared *shared, const pp_batch_result *batch)
{
    size_t used_bytes = pp_batch_result_used_bytes(batch);
    size_t allocated_bytes = pp_batch_result_allocated_bytes(batch);

    shared->chunks_completed++;
    shared->processed += batch->processed_count;
    shared->decompositions += (int64_t)batch->decomp_count;
    shared->used_bytes_total += used_bytes;
    shared->allocated_bytes_total += allocated_bytes;
    if (used_bytes > shared->max_chunk_used_bytes) {
        shared->max_chunk_used_bytes = used_bytes;
    }
    if (allocated_bytes > shared->max_chunk_allocated_bytes) {
        shared->max_chunk_allocated_bytes = allocated_bytes;
    }
    if (shared->first_p == 0 || (batch->first_p != 0 && batch->first_p < shared->first_p)) {
        shared->first_p = batch->first_p;
    }
    if (batch->last_p > shared->last_p) {
        shared->last_p = batch->last_p;
    }
}

static void *worker_main(void *ptr)
{
    materialize_worker *worker = (materialize_worker *)ptr;
    materialize_shared *shared = worker->shared;

    (void)worker;
    for (;;) {
        int64_t chunk_id;
        int64_t chunk_start_idx;
        int64_t chunk_count;
        int status;
        pp_batch_result batch;

        pthread_mutex_lock(&shared->lock);
        if (shared->failed || shared->next_chunk >= shared->total_chunks) {
            pthread_mutex_unlock(&shared->lock);
            break;
        }
        chunk_id = shared->next_chunk++;
        pthread_mutex_unlock(&shared->lock);

        chunk_start_idx = shared->start_idx + chunk_id * shared->chunk_primes;
        chunk_count = shared->chunk_primes;
        if (chunk_start_idx + chunk_count > shared->start_idx + shared->count) {
            chunk_count = shared->start_idx + shared->count - chunk_start_idx;
        }

        pp_batch_result_init(&batch);
        status = pp_process_rank_batch(chunk_start_idx, chunk_count, &batch);
        pthread_mutex_lock(&shared->lock);
        if (status != PP_OK) {
            shared->failed = 1;
            if (shared->first_status == PP_OK) {
                shared->first_status = status;
            }
        } else {
            update_stats(shared, &batch);
        }
        pthread_mutex_unlock(&shared->lock);

        pp_batch_result_clear(&batch);
        if (status != PP_OK) {
            break;
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    static const struct option long_options[] = {
        {"start-idx", required_argument, NULL, 1000},
        {"count", required_argument, NULL, 'n'},
        {"threads", required_argument, NULL, 't'},
        {"chunk-primes", required_argument, NULL, 'c'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    int opt;
    int threads = 0;
    int i;
    int64_t start_idx = 0;
    int64_t count = -1;
    int64_t chunk_primes = 500000;
    pthread_t *thread_ids;
    materialize_worker *workers;
    materialize_shared shared;
    double t0;
    double elapsed;
    size_t estimated_peak_worker_allocated;

    while ((opt = getopt_long(argc, argv, "n:t:c:h", long_options, NULL)) != -1) {
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
        case 'c':
            if (parse_i64(optarg, &chunk_primes) != 0 || chunk_primes <= 0) {
                fprintf(stderr, "invalid --chunk-primes: %s\n", optarg);
                return 2;
            }
            break;
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

    memset(&shared, 0, sizeof(shared));
    shared.start_idx = start_idx;
    shared.count = count;
    shared.chunk_primes = chunk_primes;
    shared.total_chunks = (count + chunk_primes - 1) / chunk_primes;
    shared.first_status = PP_OK;
    pthread_mutex_init(&shared.lock, NULL);

    thread_ids = (pthread_t *)calloc((size_t)threads, sizeof(*thread_ids));
    workers = (materialize_worker *)calloc((size_t)threads, sizeof(*workers));
    if (thread_ids == NULL || workers == NULL) {
        free(thread_ids);
        free(workers);
        pthread_mutex_destroy(&shared.lock);
        return 1;
    }

    t0 = monotonic_seconds();
    for (i = 0; i < threads; i++) {
        workers[i].id = i;
        workers[i].shared = &shared;
        if (pthread_create(&thread_ids[i], NULL, worker_main, &workers[i]) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            free(thread_ids);
            free(workers);
            pthread_mutex_destroy(&shared.lock);
            return 1;
        }
    }
    for (i = 0; i < threads; i++) {
        pthread_join(thread_ids[i], NULL);
    }
    elapsed = monotonic_seconds() - t0;

    if (shared.failed) {
        fprintf(stderr, "materialize failed: %s\n", pp_status_message(shared.first_status));
        free(thread_ids);
        free(workers);
        pthread_mutex_destroy(&shared.lock);
        return 1;
    }

    estimated_peak_worker_allocated =
        shared.max_chunk_allocated_bytes
        * (size_t)(shared.chunks_completed < threads ? shared.chunks_completed : threads);
    printf("threads=%d chunk_primes=%" PRId64 " chunks=%" PRId64
           " processed=%" PRId64 " decompositions=%" PRId64
           " first_p=%" PRId64 " last_p=%" PRId64
           " elapsed_s=%.6f primes_per_s=%.0f\n",
           threads, chunk_primes, shared.chunks_completed, shared.processed,
           shared.decompositions, shared.first_p, shared.last_p, elapsed,
           elapsed > 0.0 ? (double)shared.processed / elapsed : 0.0);
    printf("used_bytes_total=%zu allocated_bytes_total=%zu"
           " max_chunk_used_bytes=%zu max_chunk_allocated_bytes=%zu"
           " estimated_peak_worker_allocated_bytes=%zu\n",
           shared.used_bytes_total, shared.allocated_bytes_total,
           shared.max_chunk_used_bytes, shared.max_chunk_allocated_bytes,
           estimated_peak_worker_allocated);

    free(thread_ids);
    free(workers);
    pthread_mutex_destroy(&shared.lock);
    pp_shutdown();
    return 0;
}
