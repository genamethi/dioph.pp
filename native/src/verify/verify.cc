#include "primeparts/verify/verify.h"

#include "primeparts/common/thread_pool.h"
#include "primeparts/core.h"
#include "primeparts/source_scan.h"

#include <arrow/api.h>

#include "iceberg/expression/expressions.h"
#include "iceberg/expression/literal.h"

#include <flint/ulong_extras.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace primeparts::verify {

namespace {

const int64_t* I64(const arrow::RecordBatch& b, const char* name) {
  return std::static_pointer_cast<arrow::Int64Array>(b.GetColumnByName(name))
      ->raw_values();
}

const int32_t* I32(const arrow::RecordBatch& b, const char* name) {
  return std::static_pointer_cast<arrow::Int32Array>(b.GetColumnByName(name))
      ->raw_values();
}

__extension__ typedef unsigned __int128 u128;

u128 IPow128(uint64_t base, int32_t exp) {
  u128 r = 1;
  u128 b = base;
  for (int32_t e = 0; e < exp; ++e) r *= b;
  return r;
}

struct PrimeShard : ShardState {
  n_primes_t iter;
  std::string file;
  bool have_file = false;
  int64_t anchor = 0;
  int64_t index = 0;
  std::vector<uint64_t> buf;
  PrimeShard() { n_primes_init(iter); }
  ~PrimeShard() override { n_primes_clear(iter); }
};

class PrimeRankCheck : public Check {
 public:
  PrimeRankCheck() {
    spec_.table = "primes";
    spec_.select = {"p", "prime_rank"};
  }
  const CheckSpec& spec() const override { return spec_; }
  std::unique_ptr<ShardState> NewShard() const override {
    return std::make_unique<PrimeShard>();
  }
  void Eval(const arrow::RecordBatch& batch, const std::string& data_file,
            ShardState& state, CheckResult& out,
            int max_examples) const override {
    auto& st = static_cast<PrimeShard&>(state);
    const int64_t* p = I64(batch, "p");
    const int64_t* rank = I64(batch, "prime_rank");
    const int64_t n = batch.num_rows();
    if (n == 0) return;

    if (!st.have_file || data_file != st.file) {
      st.file = data_file;
      st.have_file = true;
      const uint64_t p0 = static_cast<uint64_t>(p[0]);
      n_primes_jump_after(st.iter, p0 - 1);
      st.anchor = pp_prime_pi(static_cast<int64_t>(p0));
      st.index = 0;
    }

    if (static_cast<int64_t>(st.buf.size()) < n) st.buf.resize(n);
    for (int64_t i = 0; i < n; ++i) st.buf[i] = n_primes_next(st.iter);

    for (int64_t i = 0; i < n; ++i) {
      ++out.rows_checked;
      const uint64_t expect_p = st.buf[i];
      const int64_t expect_rank = st.anchor + st.index + i;
      const bool bad_p = expect_p != static_cast<uint64_t>(p[i]);
      const bool bad_rank = rank[i] != expect_rank;
      if (bad_p || bad_rank) {
        ++out.violations;
        if (static_cast<int>(out.examples.size()) < max_examples) {
          std::string d;
          if (bad_p)
            d = "not the successor prime (expected " + std::to_string(expect_p) +
                ")";
          else
            d = "prime_rank " + std::to_string(rank[i]) +
                " != pi(p)=" + std::to_string(expect_rank);
          out.examples.push_back({p[i], std::move(d)});
        }
      }
    }
    st.index += n;
  }

 private:
  CheckSpec spec_;
};

class PartitionCheck : public Check {
 public:
  PartitionCheck() {
    spec_.table = "partitions";
    spec_.select = {"p", "m_k", "n_k", "q_k"};
  }
  const CheckSpec& spec() const override { return spec_; }
  std::unique_ptr<ShardState> NewShard() const override {
    return std::make_unique<ShardState>();
  }
  void Eval(const arrow::RecordBatch& batch, const std::string&,
            ShardState&, CheckResult& out, int max_examples) const override {
    const int64_t* p = I64(batch, "p");
    const int32_t* m_k = I32(batch, "m_k");
    const int32_t* n_k = I32(batch, "n_k");
    const int64_t* q_k = I64(batch, "q_k");
    const int64_t n = batch.num_rows();

    for (int64_t i = 0; i < n; ++i) {
      ++out.rows_checked;
      const int32_t m = m_k[i];
      const int32_t nn = n_k[i];
      const int64_t q = q_k[i];
      const bool not_allowed = m < 1 || m > 63 || nn < 1 || q < 2;
      const bool composite_q =
          !not_allowed && !n_is_prime(static_cast<ulong>(q));
      const bool unsatisfied =
          !not_allowed &&
          static_cast<u128>(p[i]) !=
              (static_cast<u128>(1) << m) +
                  IPow128(static_cast<uint64_t>(q), nn);
      if (not_allowed || composite_q || unsatisfied) {
        ++out.violations;
        if (static_cast<int>(out.examples.size()) < max_examples) {
          std::string d;
          if (not_allowed)
            d = "not allowed: m_k=" + std::to_string(m) +
                " n_k=" + std::to_string(nn) + " q_k=" + std::to_string(q);
          else if (unsatisfied)
            d = "unsatisfied: p != 2^" + std::to_string(m) + " + " +
                std::to_string(q) + "^" + std::to_string(nn);
          else
            d = "q_k=" + std::to_string(q) + " not prime";
          out.examples.push_back({p[i], std::move(d)});
        }
      }
    }
  }

 private:
  CheckSpec spec_;
};

}

std::shared_ptr<iceberg::Expression> BuildWindowFilter(const Window& w) {
  std::shared_ptr<iceberg::Expression> filter;
  if (w.p_lo > 0)
    filter = iceberg::Expressions::GreaterThanOrEqual(
        "p", iceberg::Literal::Long(w.p_lo));
  if (w.p_hi > 0) {
    auto hi =
        iceberg::Expressions::LessThanOrEqual("p", iceberg::Literal::Long(w.p_hi));
    filter = filter ? iceberg::Expressions::And(filter, hi) : hi;
  }
  return filter;
}

std::unique_ptr<Check> MakePrimeRankCheck() {
  return std::make_unique<PrimeRankCheck>();
}

std::unique_ptr<Check> MakePartitionCheck() {
  return std::make_unique<PartitionCheck>();
}

CheckResult TableVerifier::Run(const std::string& metadata_path,
                               const Check& check,
                               std::shared_ptr<iceberg::Expression> filter,
                               int threads, int64_t limit, int max_examples,
                               std::optional<int64_t> from_snapshot_exclusive,
                               std::string* error) {
  CheckResult result;
  result.table = check.spec().table;

  int n_workers = threads > 0
                      ? threads
                      : static_cast<int>(std::thread::hardware_concurrency());
  if (n_workers < 1) n_workers = 1;
  (void)primeparts::common::SetupArrowThreadPools(n_workers);

  auto open = [&](std::string* e, int shard, int count) {
    return from_snapshot_exclusive
               ? primeparts::SourceTableReader::OpenIncremental(
                     metadata_path, check.spec().select, filter,
                     *from_snapshot_exclusive, e, shard, count)
               : primeparts::SourceTableReader::OpenMetadata(
                     metadata_path, check.spec().select, filter, e, shard,
                     count);
  };

  int64_t total_records = 0;
  {
    std::string e;
    auto probe = open(&e, 0, 1);
    if (!probe) {
      if (error) *error = "open reader: " + e;
      return result;
    }
    total_records = probe->planned_records();
  }

  std::vector<CheckResult> accums(n_workers);
  std::vector<std::string> werr(n_workers);
  std::atomic<int64_t> scanned{0};
  std::atomic<bool> failed{false};
  std::atomic<bool> done{false};

  auto worker = [&](int shard) {
    std::string e;
    auto reader = open(&e, shard, n_workers);
    if (!reader) {
      werr[shard] = "open: " + e;
      failed.store(true);
      return;
    }
    auto st = check.NewShard();
    CheckResult& acc = accums[shard];
    std::shared_ptr<arrow::RecordBatch> batch;
    while (true) {
      if (limit > 0 && scanned.load(std::memory_order_relaxed) >= limit) break;
      if (!reader->Next(&batch, &e)) {
        werr[shard] = "read: " + e;
        failed.store(true);
        return;
      }
      if (!batch) break;
      check.Eval(*batch, reader->current_data_file_path(), *st, acc,
                 max_examples);
      scanned.fetch_add(batch->num_rows(), std::memory_order_relaxed);
    }
  };

  auto t0 = std::chrono::steady_clock::now();
  std::vector<std::thread> workers;
  workers.reserve(n_workers);
  for (int i = 0; i < n_workers; ++i) workers.emplace_back(worker, i);

  std::thread monitor([&] {
    while (!done.load(std::memory_order_relaxed)) {
      const int64_t s = scanned.load(std::memory_order_relaxed);
      const double pct = total_records > 0 ? 100.0 * s / total_records : 0.0;
      std::fprintf(stderr, "\r  verifying %s: %lld / %lld (%.1f%%)   ",
                   result.table.c_str(), static_cast<long long>(s),
                   static_cast<long long>(total_records), pct);
      std::fflush(stderr);
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    std::fprintf(stderr, "\r%70s\r", "");
    std::fflush(stderr);
  });

  for (auto& t : workers) t.join();
  done.store(true, std::memory_order_relaxed);
  monitor.join();

  if (failed.load()) {
    for (int i = 0; i < n_workers; ++i)
      if (!werr[i].empty()) {
        if (error) *error = "shard " + std::to_string(i) + ": " + werr[i];
        break;
      }
    return result;
  }

  for (auto& a : accums) {
    result.rows_checked += a.rows_checked;
    result.violations += a.violations;
    for (auto& v : a.examples)
      if (static_cast<int>(result.examples.size()) < max_examples)
        result.examples.push_back(std::move(v));
  }
  auto t1 = std::chrono::steady_clock::now();
  const double secs = std::chrono::duration<double>(t1 - t0).count();
  std::fprintf(stderr, "  %s: %lld rows in %.1fs (%.0f Mrow/s)\n",
               result.table.c_str(),
               static_cast<long long>(result.rows_checked), secs,
               secs > 0 ? result.rows_checked / secs / 1e6 : 0.0);
  return result;
}

}
