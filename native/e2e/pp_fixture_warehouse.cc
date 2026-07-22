#include "primeparts/catalog/partition_stats.h"
#include "primeparts/catalog/pp_commit.h"
#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/schemas.h"
#include "primeparts/writer.h"

#include <arrow/api.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "iceberg/catalog.h"
#include "iceberg/partition_spec.h"
#include "iceberg/row/partition_values.h"
#include "iceberg/schema.h"
#include "iceberg/table.h"

namespace ppc = primeparts::catalog;
namespace fs = std::filesystem;

namespace {

std::shared_ptr<arrow::RecordBatch> MakePrimesBatch(
    const std::shared_ptr<arrow::Schema>& schema) {
  arrow::Int64Builder p;
  arrow::Int32Builder k;
  arrow::Int64Builder prime_rank;
  arrow::Int32Builder bucket_version;
  arrow::Int32Builder bucket;
  for (int64_t value : {3, 5, 7}) (void)p.Append(value);
  for (int32_t value : {2, 1, 3}) (void)k.Append(value);
  for (int64_t value : {0, 1, 2}) (void)prime_rank.Append(value);
  for (int32_t value : {1, 1, 1}) (void)bucket_version.Append(value);
  for (int32_t value : {2, 2, 2}) (void)bucket.Append(value);
  std::shared_ptr<arrow::Array> pa, ka, ra, va, ba;
  (void)p.Finish(&pa);
  (void)k.Finish(&ka);
  (void)prime_rank.Finish(&ra);
  (void)bucket_version.Finish(&va);
  (void)bucket.Finish(&ba);
  return arrow::RecordBatch::Make(schema, 3, {pa, ka, ra, va, ba});
}

std::vector<std::shared_ptr<iceberg::DataFile>> WritePrimesFiles(
    const fs::path& warehouse, const iceberg::Namespace& ns,
    const std::shared_ptr<iceberg::Schema>& schema,
    const std::shared_ptr<iceberg::PartitionSpec>& spec,
    const std::shared_ptr<arrow::Schema>& arrow_schema, std::string* error) {
  primeparts::WriterConfig cfg;
  cfg.output_dir = ppc::StagingDataDir(warehouse, ns, "primes") /
                   "p_bucket_version=1" / "p_bucket=2";
  cfg.schema = schema;
  cfg.table_name = "primes";
  cfg.filename_prefix = "primes";
  cfg.delta_columns = {"p", "prime_rank"};
  cfg.stat_columns = {{"p", true}, {"prime_rank", true}};
  cfg.partition_spec = spec;
  cfg.partition_values = std::make_shared<iceberg::PartitionValues>(
      std::vector<iceberg::Literal>{iceberg::Literal::Int(1),
                                    iceberg::Literal::Int(2)});
  cfg.bucket_version = 1;
  cfg.bucket = 2;
  cfg.compression_level = 1;

  std::vector<std::shared_ptr<iceberg::DataFile>> files;
  auto writer = primeparts::BucketParquetWriter::Make(std::move(cfg), error);
  if (!writer) return files;
  auto batch = MakePrimesBatch(arrow_schema);
  if (!writer->Write(*batch, error)) return files;
  std::vector<primeparts::WrittenFile> written;
  if (!writer->Close(&written, error)) return files;
  for (auto& wf : written) files.push_back(wf.data_file);
  return files;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <warehouse-path>\n", argv[0]);
    return 2;
  }
  fs::path warehouse = argv[1];
  std::error_code ec;
  fs::create_directories(warehouse, ec);
  const auto ns = ppc::ResolveNamespace("");

  std::string error;
  auto local = ppc::MakeLocalCatalogWithStore(warehouse, &error);
  if (!local.catalog || !local.store) {
    std::fprintf(stderr, "catalog: %s\n", error.c_str());
    return 1;
  }
  auto schema = primeparts::PrimesSchema();
  auto spec = primeparts::BucketPartitionSpec(*schema, &error);
  if (!spec) {
    std::fprintf(stderr, "spec: %s\n", error.c_str());
    return 1;
  }
  auto arrow_schema =
      primeparts::IcebergToArrowSchemaWithFieldIds(*schema, &error);
  if (!arrow_schema) {
    std::fprintf(stderr, "arrow schema: %s\n", error.c_str());
    return 1;
  }

  ppc::TableCommitSpec cspec;
  cspec.table_name = "primes";
  cspec.schema = schema;
  cspec.spec = spec;
  cspec.files = WritePrimesFiles(warehouse, ns, schema, spec, arrow_schema,
                                 &error);
  if (cspec.files.empty()) {
    std::fprintf(stderr, "write: %s\n", error.c_str());
    return 1;
  }
  std::vector<ppc::TableCommitSpec> specs;
  specs.push_back(std::move(cspec));
  if (!ppc::CommitFilesAtomic(local.catalog, local.store, "", ns, warehouse,
                              specs, &error)) {
    std::fprintf(stderr, "commit: %s\n", error.c_str());
    return 1;
  }

  auto table = local.catalog->LoadTable(
      iceberg::TableIdentifier{.ns = ns, .name = "primes"});
  if (!table.has_value()) {
    std::fprintf(stderr, "load: %s\n", table.error().message.c_str());
    return 1;
  }
  std::printf("fixture warehouse ready: %s (namespace=primeparts table=primes)\n",
              warehouse.c_str());
  return 0;
}
