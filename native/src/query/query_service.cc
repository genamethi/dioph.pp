#include "primeparts/query/query_service.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <variant>

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
#include "iceberg/table_metadata.h"
#include "iceberg/table_scan.h"
#include "iceberg/type.h"

#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/query/materialize.h"
#include "primeparts/scan/column_binder.h"
#include "primeparts/scan/scan_planner.h"
#include "primeparts/source_scan.h"

namespace primeparts::query {

namespace {

struct WidenedColumn {
  const int64_t* i64 = nullptr;
  const int32_t* i32 = nullptr;

  int64_t Value(int64_t row) const { return i64 ? i64[row] : i32[row]; }

  static bool Bind(const arrow::RecordBatch& batch, const std::string& name,
                   WidenedColumn* out, std::string* error) {
    *out = WidenedColumn{};
    auto col = batch.GetColumnByName(name);
    if (!col) {
      if (error) *error = "column not in batch: " + name;
      return false;
    }
    if (col->type_id() == arrow::Type::INT64) {
      out->i64 = scan::BindInt64(batch, name, error);
      return out->i64 != nullptr;
    }
    if (col->type_id() == arrow::Type::INT32) {
      out->i32 = scan::BindInt32(batch, name, error);
      return out->i32 != nullptr;
    }
    if (error) {
      *error = "column " + name + " is not an integer type: " +
               col->type()->ToString();
    }
    return false;
  }
};

bool RequireSorted(const primeparts::SourceTableReader& reader,
                   const std::string& table, const char* what,
                   std::string* error) {
  if (reader.traits().sorted() && reader.traits().sort_keys.front().ascending) {
    return true;
  }
  if (error) {
    *error = "table '" + table +
             "' declares no ascending sort order in the catalog; " + what +
             " requires one";
  }
  return false;
}

}  // namespace

struct QueryService::Impl {
  std::shared_ptr<iceberg::Catalog> catalog;
  fs::path warehouse;
  iceberg::Namespace ns;
  std::vector<std::string> schema_fields;
  bool schema_loaded = false;

  fs::path ResolveMeta(const std::string& table, std::string* error) {
    auto t = catalog->LoadTable(iceberg::TableIdentifier{.ns = ns, .name = table});
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
                                                 const iceberg::Namespace& ns,
                                                 std::string* error) {
  auto impl = std::make_unique<Impl>();
  std::string mode;
  impl->catalog = catalog::OpenCatalog(warehouse, "", &mode, error);
  if (!impl->catalog) return nullptr;
  impl->warehouse = warehouse;
  impl->ns = ns;
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
  const int64_t total = reader->planned_records();
  int64_t scanned = 0;

  std::shared_ptr<arrow::RecordBatch> batch;
  while (true) {
    if (ctl.cancel && ctl.cancel->load()) return std::nullopt;
    if (!reader->Next(&batch, &e)) {
      if (error) *error = "scan primes: " + e;
      return std::nullopt;
    }
    if (!batch) break;
    const int64_t* pa = scan::BindInt64(*batch, "p", &e);
    const int32_t* ka = scan::BindInt32(*batch, "k", &e);
    const int64_t* ra = scan::BindInt64(*batch, "prime_rank", &e);
    if (!pa || !ka || !ra) {
      if (error) *error = "primes batch: " + e;
      return std::nullopt;
    }
    for (int64_t i = 0; i < batch->num_rows(); ++i) {
      if (pa[i] == p) {
        return PrimeInfo{.p = p, .k = ka[i], .prime_rank = ra[i]};
      }
    }
    scanned += batch->num_rows();
    if (ctl.progress) ctl.progress(scanned, total);
  }
  return std::nullopt;
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
    const int64_t* pa = scan::BindInt64(*batch, "p", &e);
    const int32_t* ma = scan::BindInt32(*batch, "m_k", &e);
    const int32_t* na = scan::BindInt32(*batch, "n_k", &e);
    const int64_t* qa = scan::BindInt64(*batch, "q_k", &e);
    if (!pa || !ma || !na || !qa) {
      if (error) *error = "partitions batch: " + e;
      return out;
    }
    for (int64_t i = 0; i < batch->num_rows(); ++i) {
      if (pa[i] == p) {
        out.push_back(PartitionTuple{.m_k = ma[i], .n_k = na[i], .q_k = qa[i]});
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
  if (!RequireSorted(*reader, "primes", "ScanByK", error)) return out;
  const int64_t total = reader->planned_records();

  int64_t scanned = 0;
  std::shared_ptr<arrow::RecordBatch> batch;
  while ((int64_t)out.size() < limit) {
    if (ctl.cancel && ctl.cancel->load()) break;
    if (!reader->Next(&batch, &e)) {
      if (error) *error = "scan primes: " + e;
      return out;
    }
    if (!batch) break;
    const int64_t* pa = scan::BindInt64(*batch, "p", &e);
    const int32_t* ka = scan::BindInt32(*batch, "k", &e);
    const int64_t* ra = scan::BindInt64(*batch, "prime_rank", &e);
    if (!pa || !ka || !ra) {
      if (error) *error = "primes batch: " + e;
      return out;
    }
    for (int64_t i = 0; i < batch->num_rows() && (int64_t)out.size() < limit;
         ++i) {
      if (ka[i] == k) {
        out.push_back(ScanHit{.p = pa[i], .prime_rank = ra[i]});
      }
    }
    scanned += batch->num_rows();
    if (ctl.progress) ctl.progress(scanned, total);
  }
  return out;
}

std::vector<GroupCountRow> QueryService::GroupCount(
    const std::string& table, const GroupKey& key, int64_t p_lo, int64_t p_hi,
    int threads, std::string* error, const ScanControl& ctl) {
  std::vector<GroupCountRow> out;
  if (!key.derived() && key.column.empty()) {
    if (error) *error = "GroupCount: empty group key";
    return out;
  }
  if (key.derived() && key.inputs.empty()) {
    if (error) *error = "GroupCount: derived group key declares no inputs";
    return out;
  }
  fs::path meta = impl_->ResolveMeta(table, error);
  if (meta.empty()) return out;

  const bool windowed = (p_lo > 0 || p_hi > 0);
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

  std::vector<std::string> cols =
      key.derived() ? key.inputs : std::vector<std::string>{key.column};

  int64_t total = 0;
  {
    std::string pe;
    auto probe =
        primeparts::SourceTableReader::OpenMetadata(meta, cols, filter, &pe);
    if (!probe) {
      if (error) *error = "open " + table + ": " + pe;
      return out;
    }
    if (windowed && !RequireSorted(*probe, table, "a p-window", error)) {
      return out;
    }
    if (windowed) {
      const std::string& key_col = probe->traits().sort_keys.front().name;
      if (std::find(cols.begin(), cols.end(), key_col) == cols.end()) {
        cols.push_back(key_col);
      }
    }
    total = probe->planned_records();
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
        meta, cols, filter, &e, t, threads);
    if (!reader) {
      fail("open " + table + ": " + e);
      return;
    }
    auto& acc = partials[static_cast<size_t>(t)];
    std::shared_ptr<arrow::RecordBatch> batch;
    std::vector<WidenedColumn> bound(key.derived() ? key.inputs.size() : 1);
    std::vector<int64_t> vals(bound.size());
    while (true) {
      if (ctl.cancel && ctl.cancel->load()) return;
      if (!reader->Next(&batch, &e)) {
        fail("scan " + table + ": " + e);
        return;
      }
      if (!batch) break;
      const int64_t n = batch->num_rows();

      if (key.derived()) {
        for (size_t j = 0; j < key.inputs.size(); ++j) {
          if (!WidenedColumn::Bind(*batch, key.inputs[j], &bound[j], &e)) {
            fail(table + ": " + e);
            return;
          }
        }
        for (int64_t i = 0; i < n; ++i) {
          for (size_t j = 0; j < bound.size(); ++j) vals[j] = bound[j].Value(i);
          acc[key.fn(vals.data())]++;
        }
      } else {
        if (!WidenedColumn::Bind(*batch, key.column, &bound[0], &e)) {
          fail(table + ": " + e);
          return;
        }
        for (int64_t i = 0; i < n; ++i) acc[bound[0].Value(i)]++;
      }

      const int64_t s = scanned.fetch_add(n) + n;
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
  return MaterializeIntColumns(impl_->catalog, impl_->ns, impl_->warehouse, name,
                               col_names, columns, metadata_location, error);
}

TableRows QueryService::ReadTable(const std::string& table,
                                  const std::vector<std::string>& cols_in,
                                  int64_t limit, std::string* error) {
  TableRows out;
  auto t = impl_->catalog->LoadTable(
      iceberg::TableIdentifier{.ns = impl_->ns, .name = table});
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
  std::vector<WidenedColumn> bound(cols.size());
  while (true) {
    if (!reader->Next(&batch, &e)) {
      if (error) *error = "scan " + table + ": " + e;
      out.rows.clear();
      return out;
    }
    if (!batch) break;
    const int64_t n = batch->num_rows();
    for (size_t j = 0; j < cols.size(); ++j) {
      if (!WidenedColumn::Bind(*batch, cols[j], &bound[j], &e)) {
        if (error) *error = table + ": " + e;
        out.rows.clear();
        return out;
      }
    }
    for (int64_t i = 0; i < n; ++i) {
      if (limit > 0 && static_cast<int64_t>(out.rows.size()) >= limit) return out;
      std::vector<int64_t> row;
      row.reserve(cols.size());
      for (const auto& b : bound) row.push_back(b.Value(i));
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
        iceberg::TableIdentifier{.ns = impl_->ns, .name = tbl});
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
  const auto& schema = SchemaFields();
  auto declared = [&](const std::string& n) {
    for (const auto& f : p.fields)
      if (f.name == n) return true;
    return false;
  };
  auto known = [&](const std::string& n) {
    if (schema.empty()) return declared(n);
    return std::find(schema.begin(), schema.end(), n) != schema.end();
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
  auto r = impl_->catalog->ListTables(impl_->ns);
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

TableExtent QueryService::Extent(const std::string& table, bool with_key_max,
                                 std::string* error, const ScanControl& ctl) {
  TableExtent e;
  e.table = table;
  auto t = impl_->catalog->LoadTable(
      iceberg::TableIdentifier{.ns = impl_->ns, .name = table});
  if (!t.has_value()) {
    if (error) *error = "LoadTable(" + table + "): " + t.error().message;
    return e;
  }
  auto tbl = t.value();
  e.snapshots = static_cast<int64_t>(tbl->snapshots().size());

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

  const auto& metadata = tbl->metadata();
  scan::TableReadTraits traits;
  std::string te;
  if (metadata && scan::TableReadTraits::FromMetadata(*metadata, &traits, &te) &&
      traits.sorted()) {
    e.key_name = traits.sort_keys.front().name;
  }

  if (with_key_max && !e.key_name.empty() && e.snapshot_id >= 0) {
    const auto& key = traits.sort_keys.front();
    scan::ScanPlanRequest request;
    request.select = {key.name};
    request.stats_fields = {key.name};
    scan::ScanPlan plan;
    std::string pe;
    if (!scan::PlanTableScan(metadata, tbl->io(), request, &plan, &pe)) {
      if (error) *error = "plan " + table + ": " + pe;
      return e;
    }
    const iceberg::SchemaField* field = nullptr;
    if (plan.table_schema) {
      for (const auto& f : plan.table_schema->fields()) {
        if (f.field_id() == key.field_id) {
          field = &f;
          break;
        }
      }
    }
    if (field) {
      const auto type = field->type()->type_id();
      auto prim = type == iceberg::TypeId::kInt
                      ? std::static_pointer_cast<iceberg::PrimitiveType>(
                            iceberg::int32())
                      : std::static_pointer_cast<iceberg::PrimitiveType>(
                            iceberg::int64());
      int64_t mx = INT64_MIN;
      for (const auto& task : plan.tasks) {
        if (ctl.cancel && ctl.cancel->load()) break;
        const auto& ub = task.inner->data_file()->upper_bounds;
        auto it = ub.find(key.field_id);
        if (it == ub.end()) continue;
        auto lit = iceberg::Literal::Deserialize(it->second, prim);
        if (!lit.has_value()) continue;
        int64_t v = type == iceberg::TypeId::kInt
                        ? std::get<int32_t>(lit.value().value())
                        : std::get<int64_t>(lit.value().value());
        if (v > mx) mx = v;
      }
      if (mx != INT64_MIN) e.key_max = mx;
    }
  }
  e.ok = true;
  return e;
}

}  // namespace primeparts::query
