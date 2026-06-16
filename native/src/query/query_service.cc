// primeparts/query/query_service.cc — see header.

#include "primeparts/query/query_service.h"

#include <algorithm>
#include <memory>
#include <utility>

#include <arrow/array.h>
#include <arrow/record_batch.h>

#include "iceberg/catalog.h"
#include "iceberg/expression/expressions.h"
#include "iceberg/expression/literal.h"
#include "iceberg/schema.h"
#include "iceberg/table.h"
#include "iceberg/table_identifier.h"

#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/source_scan.h"

namespace primeparts::query {

namespace {
const iceberg::Namespace kNs{{"primeparts"}};
}  // namespace

struct QueryService::Impl {
  std::shared_ptr<iceberg::Catalog> catalog;
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

}  // namespace primeparts::query
