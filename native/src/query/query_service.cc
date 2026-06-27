// primeparts/query/query_service.cc — see header.

#include "primeparts/query/query_service.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include <arrow/array.h>
#include <arrow/record_batch.h>
#include <arrow/type.h>

#include "iceberg/catalog.h"
#include "iceberg/expression/expressions.h"
#include "iceberg/expression/literal.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/schema.h"
#include "iceberg/schema_field.h"
#include "iceberg/snapshot.h"
#include "iceberg/table.h"
#include "iceberg/table_identifier.h"
#include "iceberg/table_scan.h"

#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/query/materialize.h"
#include "primeparts/source_scan.h"

namespace primeparts::query {

namespace {
const iceberg::Namespace kNs{{"primeparts"}};
}  // namespace

struct QueryService::Impl {
  std::shared_ptr<iceberg::Catalog> catalog;
  fs::path warehouse;  // root, for materialize (CommitFiles needs it)
  std::vector<std::string> schema_fields;  // cached union of base-table fields
  bool schema_loaded = false;

  // Resolve a table's current metadata.json path through the catalog seam
  // (LoadTable). Empty + *error on failure.
  fs::path ResolveMeta(const std::string& table, std::string* error) {
    auto t = catalog->LoadTable(iceberg::TableIdentifier{.ns = kNs, .name = table});
    if (!t.has_value()) {
      if (error) *error = "LoadTable(" + table + "): " + t.error().message;
      return {};
    }
    return fs::path(std::string(t.value()->metadata_file_location()));
  }
};

QueryService::QueryService(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
QueryService::~QueryService() = default;

std::unique_ptr<QueryService> QueryService::Open(const fs::path& warehouse,
                                                 std::string* error) {
  auto impl = std::make_unique<Impl>();
  impl->catalog = catalog::MakeLocalCatalog(warehouse, error);
  if (!impl->catalog) return nullptr;
  impl->warehouse = warehouse;
  return std::unique_ptr<QueryService>(new QueryService(std::move(impl)));
}

std::optional<PrimeInfo> QueryService::LookupPrime(int64_t p, std::string* error,
                                                   const ScanControl& ctl) {
  fs::path meta = impl_->ResolveMeta("primes", error);
  if (meta.empty()) return std::nullopt;

  auto filter = iceberg::Expressions::Equal("p", iceberg::Literal::Long(p));
  std::string e;
  auto reader = primeparts::SourceTableReader::OpenMetadata(
      meta, {"p", "k", "prime_rank"}, filter, &e);
  if (!reader) {
    if (error) *error = "open primes: " + e;
    return std::nullopt;
  }
  const int64_t total = reader->total_records();
  int64_t scanned = 0;

  std::shared_ptr<arrow::RecordBatch> batch;
  while (true) {
    if (ctl.cancel && ctl.cancel->load()) return std::nullopt;
    if (!reader->Next(&batch, &e)) {
      if (error) *error = "scan primes: " + e;
      return std::nullopt;
    }
    if (!batch) break;  // EOF
    auto pa = std::static_pointer_cast<arrow::Int64Array>(batch->GetColumnByName("p"));
    auto ka = std::static_pointer_cast<arrow::Int32Array>(batch->GetColumnByName("k"));
    auto ra = std::static_pointer_cast<arrow::Int64Array>(
        batch->GetColumnByName("prime_rank"));
    for (int64_t i = 0; i < batch->num_rows(); ++i) {
      if (pa->Value(i) == p) {
        return PrimeInfo{.p = p, .k = ka->Value(i), .prime_rank = ra->Value(i)};
      }
    }
    scanned += batch->num_rows();
    if (ctl.progress) ctl.progress(scanned, total);
  }
  return std::nullopt;  // not present
}

std::vector<PartitionTuple> QueryService::LookupPartitions(
    int64_t p, std::string* error, const ScanControl& ctl) {
  std::vector<PartitionTuple> out;
  fs::path meta = impl_->ResolveMeta("partitions", error);
  if (meta.empty()) return out;

  auto filter = iceberg::Expressions::Equal("p", iceberg::Literal::Long(p));
  std::string e;
  auto reader = primeparts::SourceTableReader::OpenMetadata(
      meta, {"p", "m_k", "n_k", "q_k"}, filter, &e);
  if (!reader) {
    if (error) *error = "open partitions: " + e;
    return out;
  }

  std::shared_ptr<arrow::RecordBatch> batch;
  while (true) {
    if (ctl.cancel && ctl.cancel->load()) return out;
    if (!reader->Next(&batch, &e)) {
      if (error) *error = "scan partitions: " + e;
      return out;
    }
    if (!batch) break;
    auto pa = std::static_pointer_cast<arrow::Int64Array>(batch->GetColumnByName("p"));
    auto ma = std::static_pointer_cast<arrow::Int32Array>(batch->GetColumnByName("m_k"));
    auto na = std::static_pointer_cast<arrow::Int32Array>(batch->GetColumnByName("n_k"));
    auto qa = std::static_pointer_cast<arrow::Int64Array>(batch->GetColumnByName("q_k"));
    for (int64_t i = 0; i < batch->num_rows(); ++i) {
      if (pa->Value(i) == p) {
        out.push_back(PartitionTuple{
            .m_k = ma->Value(i), .n_k = na->Value(i), .q_k = qa->Value(i)});
      }
    }
  }
  return out;
}

std::vector<ScanHit> QueryService::ScanByK(int32_t k, int64_t p_lo, int64_t p_hi,
                                           int64_t limit, std::string* error,
                                           const ScanControl& ctl) {
  std::vector<ScanHit> out;
  if (limit <= 0) return out;
  fs::path meta = impl_->ResolveMeta("primes", error);
  if (meta.empty()) return out;

  // Pushdown: k == K, bounded by the p-window [p_lo, p_hi] (open ends when <= 0).
  // TODO(lua-preset): this predicate is the execution seam a Lua preset would
  // drive — e.g. preset.run(f) -> pp.scan_k(f.k, f.p_lo, f.p_hi, f.limit); the
  // hardcoded build here is the provisional stand-in (markdown/arch/tui_app_design.md).
  std::shared_ptr<iceberg::Expression> filter =
      iceberg::Expressions::Equal("k", iceberg::Literal::Int(k));
  if (p_lo > 0) {
    filter = iceberg::Expressions::And(
        filter, iceberg::Expressions::GreaterThanOrEqual(
                    "p", iceberg::Literal::Long(p_lo)));
  }
  if (p_hi > 0) {
    filter = iceberg::Expressions::And(
        filter,
        iceberg::Expressions::LessThanOrEqual("p", iceberg::Literal::Long(p_hi)));
  }

  std::string e;
  auto reader = primeparts::SourceTableReader::OpenMetadata(
      meta, {"p", "k", "prime_rank"}, filter, &e);
  if (!reader) {
    if (error) *error = "open primes: " + e;
    return out;
  }
  const int64_t total = reader->total_records();

  int64_t scanned = 0;
  bool past_window = false;  // p is streamed ascending; stop once p > p_hi
  std::shared_ptr<arrow::RecordBatch> batch;
  while ((int64_t)out.size() < limit && !past_window) {
    if (ctl.cancel && ctl.cancel->load()) break;  // cooperative cancel
    if (!reader->Next(&batch, &e)) {
      if (error) *error = "scan primes: " + e;
      return out;
    }
    if (!batch) break;  // EOF
    auto pa = std::static_pointer_cast<arrow::Int64Array>(batch->GetColumnByName("p"));
    auto ka = std::static_pointer_cast<arrow::Int32Array>(batch->GetColumnByName("k"));
    auto ra = std::static_pointer_cast<arrow::Int64Array>(
        batch->GetColumnByName("prime_rank"));
    for (int64_t i = 0; i < batch->num_rows() && (int64_t)out.size() < limit; ++i) {
      const int64_t pv = pa->Value(i);
      // iceberg-cpp prunes whole files by the predicate but does NOT enforce a
      // row-level residual, so enforce both k and the p-window in C++. p is
      // sorted ascending across the stream -> early-stop once past p_hi.
      if (p_hi > 0 && pv > p_hi) { past_window = true; break; }
      if (p_lo > 0 && pv < p_lo) continue;
      if (ka->Value(i) == k) {
        out.push_back(ScanHit{.p = pv, .prime_rank = ra->Value(i)});
      }
    }
    scanned += batch->num_rows();
    if (ctl.progress) ctl.progress(scanned, total);
  }
  return out;
}

std::vector<GroupCountRow> QueryService::GroupCount(
    const std::string& table, const std::string& column, int64_t p_lo,
    int64_t p_hi, int threads, std::string* error, const ScanControl& ctl) {
  std::vector<GroupCountRow> out;
  fs::path meta = impl_->ResolveMeta(table, error);
  if (meta.empty()) return out;

  // Optional p-window pushdown (file pruning only; both base tables carry `p`).
  std::shared_ptr<iceberg::Expression> filter;
  if (p_lo > 0) {
    filter = iceberg::Expressions::GreaterThanOrEqual(
        "p", iceberg::Literal::Long(p_lo));
  }
  if (p_hi > 0) {
    auto upper =
        iceberg::Expressions::LessThanOrEqual("p", iceberg::Literal::Long(p_hi));
    filter = filter ? iceberg::Expressions::And(filter, upper) : upper;
  }

  if (threads < 1) {
    const unsigned hw = std::thread::hardware_concurrency();
    threads = hw == 0 ? 4 : std::max(1, std::min(8, static_cast<int>(hw)));
  }

  // Grouping key. Besides a raw integer column, a small set of derived
  // ("virtual") keys is supported, computed per row in C++ from `primes`:
  //   "bits" = floor(log2(p))       — the candidate-position count max_m
  //   "r"    = floor(log2(p)) - k   — the # of m where p-2^m is NOT a prime
  //            power (the "misses"); r >= 0 since k <= max_m (one soln per m).
  enum class Key { kColumn, kBits, kR };
  Key key = Key::kColumn;
  if (column == "bits") key = Key::kBits;
  else if (column == "r") key = Key::kR;

  // iceberg prunes whole files by the predicate but does NOT enforce a
  // row-level residual, so a p-window means we read `p` too and bound each row
  // in C++ (mirrors ScanByK). Derived keys also pull their inputs (p, k).
  const bool windowed = (p_lo > 0 || p_hi > 0);
  const bool need_p = windowed || key == Key::kBits || key == Key::kR;
  const bool need_k = key == Key::kR;
  std::vector<std::string> cols;
  auto add_col = [&](const std::string& c) {
    if (std::find(cols.begin(), cols.end(), c) == cols.end()) cols.push_back(c);
  };
  if (key == Key::kColumn) add_col(column);
  if (need_p) add_col("p");
  if (need_k) add_col("k");

  // Total (for progress) via a cheap manifest read on a probe reader.
  int64_t total = 0;
  {
    std::string pe;
    auto probe =
        primeparts::SourceTableReader::OpenMetadata(meta, cols, filter, &pe);
    if (probe) total = probe->total_records();
  }

  std::vector<std::map<int64_t, int64_t>> partials(threads);
  std::atomic<int64_t> scanned{0};
  std::atomic<bool> failed{false};
  std::mutex err_mu;
  std::string first_error;
  auto fail = [&](const std::string& m) {
    if (!failed.exchange(true)) {
      std::lock_guard<std::mutex> lock(err_mu);
      first_error = m;
    }
  };

  auto worker = [&](int t) {
    std::string e;
    auto reader = primeparts::SourceTableReader::OpenMetadata(
        meta, cols, filter, &e, /*shard_index=*/t, /*shard_count=*/threads);
    if (!reader) { fail("open " + table + ": " + e); return; }
    auto& acc = partials[static_cast<size_t>(t)];
    auto bits = [](int64_t p) {
      return static_cast<int64_t>(
          63 - __builtin_clzll(static_cast<unsigned long long>(p)));
    };
    std::shared_ptr<arrow::RecordBatch> batch;
    while (true) {
      if (ctl.cancel && ctl.cancel->load()) return;
      if (!reader->Next(&batch, &e)) { fail("scan " + table + ": " + e); return; }
      if (!batch) break;  // EOF
      const int64_t n = batch->num_rows();

      std::shared_ptr<arrow::Int64Array> pa;
      std::shared_ptr<arrow::Int32Array> ka;
      if (need_p) {
        pa = std::static_pointer_cast<arrow::Int64Array>(
            batch->GetColumnByName("p"));
        if (!pa) { fail("p column missing on " + table); return; }
      }
      if (need_k) {
        ka = std::static_pointer_cast<arrow::Int32Array>(
            batch->GetColumnByName("k"));
        if (!ka) { fail("k column missing on " + table); return; }
      }
      auto in_window = [&](int64_t i) {
        if (!windowed) return true;
        const int64_t pv = pa->Value(i);
        if (p_lo > 0 && pv < p_lo) return false;
        if (p_hi > 0 && pv > p_hi) return false;
        return true;
      };

      if (key == Key::kBits) {
        for (int64_t i = 0; i < n; ++i)
          if (in_window(i)) acc[bits(pa->Value(i))]++;
      } else if (key == Key::kR) {
        for (int64_t i = 0; i < n; ++i)
          if (in_window(i)) acc[bits(pa->Value(i)) - ka->Value(i)]++;
      } else {
        auto arr = batch->GetColumnByName(column);
        if (!arr) { fail("column not found: " + column); return; }
        switch (arr->type_id()) {
          case arrow::Type::INT32: {
            auto a = std::static_pointer_cast<arrow::Int32Array>(arr);
            for (int64_t i = 0; i < n; ++i)
              if (in_window(i)) acc[a->Value(i)]++;
            break;
          }
          case arrow::Type::INT64: {
            auto a = std::static_pointer_cast<arrow::Int64Array>(arr);
            for (int64_t i = 0; i < n; ++i)
              if (in_window(i)) acc[a->Value(i)]++;
            break;
          }
          default:
            fail("unsupported (non-integer) column for group: " + column);
            return;
        }
      }
      const int64_t s = scanned.fetch_add(n) + n;
      // Only thread 0 reports progress, so the (possibly non-thread-safe)
      // callback is never invoked concurrently.
      if (t == 0 && ctl.progress) ctl.progress(s, total);
    }
  };

  std::vector<std::thread> pool;
  pool.reserve(static_cast<size_t>(threads));
  for (int t = 0; t < threads; ++t) pool.emplace_back(worker, t);
  for (auto& th : pool) th.join();

  if (failed.load()) {
    if (error) *error = first_error;
    return out;
  }

  std::map<int64_t, int64_t> merged;
  for (const auto& part : partials)
    for (const auto& [v, c] : part) merged[v] += c;
  out.reserve(merged.size());
  for (const auto& [v, c] : merged) out.push_back(GroupCountRow{.value = v, .count = c});
  return out;
}

bool QueryService::Materialize(const std::string& name,
                               const std::vector<std::string>& col_names,
                               const std::vector<std::vector<int64_t>>& columns,
                               std::string* metadata_location,
                               std::string* error) {
  return MaterializeIntColumns(impl_->catalog, impl_->warehouse, name, col_names,
                               columns, metadata_location, error);
}

TableRows QueryService::ReadTable(const std::string& table,
                                  const std::vector<std::string>& cols_in,
                                  int64_t limit, std::string* error) {
  TableRows out;
  auto t = impl_->catalog->LoadTable(
      iceberg::TableIdentifier{.ns = kNs, .name = table});
  if (!t.has_value()) {
    if (error) *error = "LoadTable(" + table + "): " + t.error().message;
    return out;
  }
  std::vector<std::string> cols = cols_in;
  if (cols.empty()) {
    if (auto sch = t.value()->schema(); sch.has_value())
      for (const auto& f : sch.value()->fields()) cols.emplace_back(f.name());
  }
  if (cols.empty()) { if (error) *error = "no columns to read"; return out; }

  fs::path meta = impl_->ResolveMeta(table, error);
  if (meta.empty()) return out;
  std::string e;
  auto reader =
      primeparts::SourceTableReader::OpenMetadata(meta, cols, nullptr, &e);
  if (!reader) { if (error) *error = "open " + table + ": " + e; return out; }
  out.cols = cols;

  std::shared_ptr<arrow::RecordBatch> batch;
  while (true) {
    if (!reader->Next(&batch, &e)) {
      if (error) *error = "scan " + table + ": " + e;
      out.rows.clear();
      return out;
    }
    if (!batch) break;  // EOF
    const int64_t n = batch->num_rows();
    std::vector<std::shared_ptr<arrow::Array>> arrs;
    for (const auto& c : cols) {
      auto a = batch->GetColumnByName(c);
      if (!a) {
        if (error) *error = "column not found: " + c;
        out.rows.clear();
        return out;
      }
      arrs.push_back(std::move(a));
    }
    for (int64_t i = 0; i < n; ++i) {
      if (limit > 0 && static_cast<int64_t>(out.rows.size()) >= limit) return out;
      std::vector<int64_t> row;
      row.reserve(cols.size());
      for (const auto& a : arrs) {
        if (a->type_id() == arrow::Type::INT32) {
          row.push_back(std::static_pointer_cast<arrow::Int32Array>(a)->Value(i));
        } else if (a->type_id() == arrow::Type::INT64) {
          row.push_back(std::static_pointer_cast<arrow::Int64Array>(a)->Value(i));
        } else {
          if (error) *error = "non-integer column in read: " + table;
          out.rows.clear();
          return out;
        }
      }
      out.rows.push_back(std::move(row));
    }
  }
  return out;
}

const std::vector<std::string>& QueryService::SchemaFields() {
  if (impl_->schema_loaded) return impl_->schema_fields;
  std::vector<std::string>& out = impl_->schema_fields;
  for (const char* tbl : {"primes", "partitions"}) {
    auto t = impl_->catalog->LoadTable(
        iceberg::TableIdentifier{.ns = kNs, .name = tbl});
    if (!t.has_value()) continue;
    auto sch = t.value()->schema();
    if (!sch.has_value()) continue;
    for (const auto& f : sch.value()->fields()) out.emplace_back(f.name());
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  impl_->schema_loaded = true;
  return out;
}

bool QueryService::ValidatePreset(const QueryPreset& p, std::string* error) {
  auto fail = [&](std::string m) { if (error) *error = std::move(m); return false; };
  if (p.id.empty()) return fail("preset id is empty");
  if (p.kind != "by_k" && p.kind != "lookup")
    return fail("unknown query kind: '" + p.kind + "'");
  const auto& fields = SchemaFields();
  auto known = [&](const std::string& n) {
    return std::find(fields.begin(), fields.end(), n) != fields.end();
  };
  for (const auto& a : p.accepts)
    if (!known(a)) return fail("accepts unknown schema field: '" + a + "'");
  if (p.target.empty()) return fail("target is empty");
  if (!known(p.target)) return fail("target unknown schema field: '" + p.target + "'");
  bool target_is_field = false;
  for (const auto& f : p.fields)
    if (f.name == p.target) target_is_field = true;
  if (!target_is_field)
    return fail("target '" + p.target + "' is not a declared field of the preset");
  return true;
}

std::vector<std::string> QueryService::ListTables(std::string* error) {
  auto r = impl_->catalog->ListTables(kNs);
  if (!r.has_value()) {
    if (error) *error = "ListTables: " + r.error().message;
    return {};
  }
  std::vector<std::string> out;
  out.reserve(r.value().size());
  for (const auto& id : r.value()) out.push_back(id.name);
  std::sort(out.begin(), out.end());
  return out;
}

TableExtent QueryService::Extent(const std::string& table, bool with_max_p,
                                 std::string* error, const ScanControl& ctl) {
  TableExtent e;
  e.table = table;
  auto t = impl_->catalog->LoadTable(
      iceberg::TableIdentifier{.ns = kNs, .name = table});
  if (!t.has_value()) {
    if (error) *error = "LoadTable(" + table + "): " + t.error().message;
    return e;
  }
  auto tbl = t.value();
  e.snapshots = static_cast<int64_t>(tbl->snapshots().size());

  // Summary facts come straight from the current snapshot — no scan.
  if (auto snap = tbl->current_snapshot(); snap.has_value() && snap.value()) {
    const auto& s = *snap.value();
    e.snapshot_id = s.snapshot_id;
    e.sequence = s.sequence_number;
    auto num = [&](const std::string& key) -> int64_t {
      auto it = s.summary.find(key);
      return it != s.summary.end() ? std::strtoll(it->second.c_str(), nullptr, 10)
                                   : -1;
    };
    e.row_count = num(iceberg::SnapshotSummaryFields::kTotalRecords);
    e.data_files = num(iceberg::SnapshotSummaryFields::kTotalDataFiles);
    e.file_bytes = num(iceberg::SnapshotSummaryFields::kTotalFileSize);
  }

  // Frontier: max of the "p" column's per-file upper bounds (manifest aggregate,
  // not a row scan). Only for tables that actually have a "p" column.
  if (with_max_p) {
    int32_t pid = -1;
    if (auto sch = tbl->schema(); sch.has_value()) {
      for (const auto& f : sch.value()->fields())
        if (f.name() == "p") { pid = f.field_id(); break; }
    }
    const bool dbg = std::getenv("PP_EXTENT_DEBUG") != nullptr;
    if (dbg) std::fprintf(stderr, "[ext] pid(p)=%d\n", pid);
    if (pid >= 0) {
      auto scan_b = tbl->NewScan();
      if (scan_b.has_value()) {
        auto scan = scan_b.value()->Build();
        if (scan.has_value()) {
          auto tasks = scan.value()->PlanFiles();
          if (dbg && !tasks.has_value())
            std::fprintf(stderr, "[ext] PlanFiles err: %s\n", tasks.error().message.c_str());
          if (tasks.has_value()) {
            if (dbg) std::fprintf(stderr, "[ext] %zu tasks\n", tasks.value().size());
            int64_t mx = INT64_MIN;
            int shown = 0;
            for (const auto& task : tasks.value()) {
              if (ctl.cancel && ctl.cancel->load()) break;
              const auto& ub = task->data_file()->upper_bounds;
              if (dbg && shown < 3) {
                std::fprintf(stderr, "[ext] file ub_size=%zu:", ub.size());
                for (auto& kv : ub) std::fprintf(stderr, " [%d]=%zuB", kv.first, kv.second.size());
                std::fprintf(stderr, "\n"); ++shown;
              }
              auto it = ub.find(pid);
              if (it != ub.end() && it->second.size() >= sizeof(int64_t)) {
                int64_t v;  // iceberg `long` bound: 8-byte little-endian
                std::memcpy(&v, it->second.data(), sizeof(int64_t));
                if (v > mx) mx = v;
              }
            }
            if (mx != INT64_MIN) e.max_p = mx;
          }
        } else if (dbg) {
          std::fprintf(stderr, "[ext] Build err: %s\n", scan.error().message.c_str());
        }
      } else if (dbg) {
        std::fprintf(stderr, "[ext] NewScan err: %s\n", scan_b.error().message.c_str());
      }
    }
  }
  e.ok = true;
  return e;
}

}  // namespace primeparts::query
