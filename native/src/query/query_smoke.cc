// De-risk smoke: prove iceberg-cpp predicate pushdown actually returns the
// matching row (and prunes files) before building QueryService on it.
//
// The filter path in source_scan.cc (scan_builder->Filter(filter)) is wired but
// never exercised in the shipped codebase — the sieve always passes nullptr.
// This confirms (a) the Expressions/Literal API compiles, (b) `p == P` returns
// exactly the matching row, (c) it does NOT scan all 23 GB (file pruning works).
//
// Build (manual, mirrors the sieve-triage link in native/Makefile):
//   see scripts/run below; links source_scan.o + iceberg static libs.
//
// Usage: query_smoke <primes_metadata.json> [p]   (p defaults to 2147483647)

#include "primeparts/source_scan.h"

#include "iceberg/expression/expressions.h"
#include "iceberg/expression/literal.h"

#include <arrow/array.h>
#include <arrow/record_batch.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <primes_metadata.json> [p]\n", argv[0]);
    return 2;
  }
  const std::string meta = argv[1];
  const int64_t target_p = (argc >= 3) ? std::strtoll(argv[2], nullptr, 10)
                                       : 2147483647LL;  // 2^31-1, prime

  // Build the predicate:  p == target_p   (p is field_id 1, type long).
  std::shared_ptr<iceberg::Expression> filter =
      iceberg::Expressions::Equal("p", iceberg::Literal::Long(target_p));

  std::string err;
  auto t0 = std::chrono::steady_clock::now();
  auto reader = primeparts::SourceTableReader::OpenMetadata(
      meta, {"p", "k", "prime_rank"}, filter, &err);
  if (!reader) {
    std::fprintf(stderr, "[!] OpenMetadata failed: %s\n", err.c_str());
    return 1;
  }
  std::fprintf(stderr, "[i] planned file_count=%lld total_records=%lld\n",
               (long long)reader->file_count(),
               (long long)reader->total_records());

  int64_t batches = 0, rows_seen = 0, matches = 0;
  int64_t found_k = -1, found_rank = -1;
  std::shared_ptr<arrow::RecordBatch> batch;
  while (true) {
    if (!reader->Next(&batch, &err)) {
      std::fprintf(stderr, "[!] Next failed: %s\n", err.c_str());
      return 1;
    }
    if (!batch) break;  // EOF
    ++batches;
    rows_seen += batch->num_rows();
    auto p_arr =
        std::static_pointer_cast<arrow::Int64Array>(batch->GetColumnByName("p"));
    auto k_arr =
        std::static_pointer_cast<arrow::Int32Array>(batch->GetColumnByName("k"));
    auto r_arr = std::static_pointer_cast<arrow::Int64Array>(
        batch->GetColumnByName("prime_rank"));
    // Re-filter for exact match in C++: robust whether iceberg applies the
    // residual at row level or only prunes at file level.
    for (int64_t i = 0; i < batch->num_rows(); ++i) {
      if (p_arr->Value(i) == target_p) {
        ++matches;
        found_k = k_arr->Value(i);
        found_rank = r_arr->Value(i);
      }
    }
  }
  auto t1 = std::chrono::steady_clock::now();
  double secs = std::chrono::duration<double>(t1 - t0).count();

  std::fprintf(stderr,
               "[i] batches=%lld rows_scanned=%lld elapsed=%.3fs\n",
               (long long)batches, (long long)rows_seen, secs);
  if (matches == 1) {
    std::fprintf(stdout, "[ok] p=%lld -> k=%lld prime_rank=%lld (1 match)\n",
                 (long long)target_p, (long long)found_k, (long long)found_rank);
    std::fprintf(stdout,
                 "[ok] rows_scanned=%lld of total=%lld -> pruning %s\n",
                 (long long)rows_seen, (long long)reader->total_records(),
                 (rows_seen < reader->total_records()) ? "WORKED"
                                                       : "did NOT prune");
    return 0;
  }
  std::fprintf(stderr, "[!] expected exactly 1 match for p=%lld, got %lld\n",
               (long long)target_p, (long long)matches);
  return 1;
}
