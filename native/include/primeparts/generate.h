#ifndef PRIMEPARTS_GENERATE_H
#define PRIMEPARTS_GENERATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*pp_gen_log_fn)(void* user_data, const char* line);

typedef struct {
  pp_gen_log_fn on_log;
  void* user_data;
} pp_gen_callbacks;

typedef struct {
  int64_t start_idx;
  int64_t count;
  int64_t chunk_primes;
  int64_t threads;
  int32_t commit_seq_start;
  int temp;
  const char* warehouse;
  const char* manifest;
} pp_gen_options;

typedef struct {
  int64_t start_idx;
  int64_t count;
  int64_t prime_rows;
  int64_t decomp_rows;
  int64_t files_written;
  int64_t bytes_written;
  int64_t first_p;
  int64_t last_p;
  int stop_requested;
  double elapsed_s;
  double primes_per_s;
} pp_gen_result;

/* Returns 0 on success. On failure returns non-zero and sets pp_gen_last_error(). */
int pp_gen_run(const pp_gen_options* options, const pp_gen_callbacks* callbacks, pp_gen_result* out);

/* Thread-local error string for the most recent pp_gen_run() failure. */
const char* pp_gen_last_error(void);

#ifdef __cplusplus
}
#endif

#endif
