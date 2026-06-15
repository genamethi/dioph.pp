// primeparts/query/query_service.cc — see header.

#include "primeparts/query/query_service.h"

#include <memory>
#include <utility>

#include <arrow/array.h>
#include <arrow/record_batch.h>

#include "iceberg/catalog.h"
#include "iceberg/expression/expressions.h"
#include "iceberg/expression/literal.h"
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

std::optional<PrimeInfo> QueryService::LookupPrime(int64_t p, std::string* error) {
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

  std::shared_ptr<arrow::RecordBatch> batch;
  while (true) {
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
  }
  return std::nullopt;  // not present
}

std::vector<PartitionTuple> QueryService::LookupPartitions(int64_t p,
                                                           std::string* error) {
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

std::vector<ScanHit> QueryService::ScanByK(int32_t k, int64_t limit,
                                           std::string* error) {
  std::vector<ScanHit> out;
  if (limit <= 0) return out;
  fs::path meta = impl_->ResolveMeta("primes", error);
  if (meta.empty()) return out;

  auto filter = iceberg::Expressions::Equal("k", iceberg::Literal::Int(k));
  std::string e;
  auto reader = primeparts::SourceTableReader::OpenMetadata(
      meta, {"p", "k", "prime_rank"}, filter, &e);
  if (!reader) {
    if (error) *error = "open primes: " + e;
    return out;
  }

  std::shared_ptr<arrow::RecordBatch> batch;
  while ((int64_t)out.size() < limit) {
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
      // Re-check k: the scan filter prunes files/row-groups but does not
      // guarantee a row-level residual, so confirm the exact match.
      if (ka->Value(i) == k) {
        out.push_back(ScanHit{.p = pa->Value(i), .prime_rank = ra->Value(i)});
      }
    }
  }
  return out;
}

}  // namespace primeparts::query
