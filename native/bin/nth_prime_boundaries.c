/*
 * nth_prime_boundaries — one-shot helper that maps a sorted list of
 * 1-indexed prime ranks (FLINT convention: rank 1 = 2) to their primes,
 * in a single forward iteration via FLINT's n_primes_t.
 *
 * Input:  argv[1] = path to a text file with one ulong rank per line,
 *                   strictly ascending, all >= 1.
 * Output: stdout, one "<rank>\t<prime>\n" line per input rank in order.
 *
 * The ranks are monotone, so a single pass with n_primes_next hits all
 * of them; n_nth_prime would re-sieve from scratch on each call.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <flint/flint.h>
#include <flint/ulong_extras.h>

static int read_ranks(const char *path, ulong **out, size_t *n_out) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return -1;
    }
    size_t cap = 16, n = 0;
    ulong *buf = malloc(cap * sizeof(*buf));
    if (!buf) { fclose(f); return -1; }

    char line[64];
    while (fgets(line, sizeof line, f)) {
        char *end;
        errno = 0;
        unsigned long long v = strtoull(line, &end, 10);
        if (end == line) continue;          /* blank / comment line */
        if (errno || v == 0) {
            fprintf(stderr, "bad rank in %s: %s", path, line);
            free(buf); fclose(f); return -1;
        }
        if (n && (ulong)v <= buf[n - 1]) {
            fprintf(stderr, "ranks must be strictly ascending (%llu after %lu)\n",
                    v, (unsigned long)buf[n - 1]);
            free(buf); fclose(f); return -1;
        }
        if (n == cap) {
            cap *= 2;
            ulong *grown = realloc(buf, cap * sizeof(*buf));
            if (!grown) { free(buf); fclose(f); return -1; }
            buf = grown;
        }
        buf[n++] = (ulong)v;
    }
    fclose(f);
    *out = buf;
    *n_out = n;
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <ranks.txt>\n", argv[0]);
        return 2;
    }

    ulong *ranks = NULL;
    size_t n_ranks = 0;
    if (read_ranks(argv[1], &ranks, &n_ranks) != 0) return 1;
    if (n_ranks == 0) {
        fprintf(stderr, "no ranks in %s\n", argv[1]);
        free(ranks);
        return 1;
    }

    n_primes_t it;
    n_primes_init(it);

    ulong target_idx = 0;           /* next rank we are looking for */
    ulong count = 0;                /* number of primes seen so far */
    while (target_idx < n_ranks) {
        ulong p = n_primes_next(it);
        count++;
        while (target_idx < n_ranks && count == ranks[target_idx]) {
            printf("%lu\t%lu\n",
                   (unsigned long)ranks[target_idx],
                   (unsigned long)p);
            fflush(stdout);
            target_idx++;
        }
    }

    n_primes_clear(it);
    free(ranks);
    return 0;
}
