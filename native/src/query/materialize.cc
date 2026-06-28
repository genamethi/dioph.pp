// primeparts/query/materialize.cc — see header.

#include "primeparts/query/materialize.h"

#include <arrow/api.h>

#include <stdexcept>
#include <system_error>

#include "iceberg/catalog.h"
#include "iceberg/expression/literal.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/partition_spec.h"
#include "iceberg/row/partition_values.h"
#include "iceberg/schema.h"
#include "iceberg/schema_field.h"
#include "iceberg/table_identifier.h"
#include "iceberg/type.h"

#include "primeparts/catalog/pp_iceberg_rest.h"
#include "primeparts/writer.h"

namespace primeparts::query {

bool MaterializeIntColumns(
    const std::shared_ptr<iceberg::Catalog>& catalog, const fs::path& warehouse,
    const std::string& name, const std::vector<std::string>& col_names,
    const std::vector<std::vector<int64_t>>& columns,
    std::string* metadata_location, std::string* error) {
  auto fail = [&](const std::string& m) { if (error) *error = m; return false; };
  if (col_names.empty() || col_names.size() != columns.size())
    return fail("materialize: col_names/columns size mismatch");
  const int64_t nrows = static_cast<int64_t>(columns[0].size());
  for (const auto& col : columns)
    if (static_cast<int64_t>(col.size()) != nrows)
      return fail("materialize: ragged columns");

  // Ad-hoc iceberg schema: all int64, field ids 1..n.
  std::vector<iceberg::SchemaField> fields;
  fields.reserve(col_names.size());
  for (size_t i = 0; i < col_names.size(); ++i)
    fields.push_back(iceberg::SchemaField::MakeRequired(
        static_cast<int32_t>(i + 1), col_names[i], iceberg::int64()));
  auto schema = std::make_shared<iceberg::Schema>(std::move(fields), 0);
  auto spec = iceberg::PartitionSpec::Unpartitioned();

  // Arrow batch (column names match the iceberg schema; writer stamps field-ids).
  std::vector<std::shared_ptr<arrow::Field>> afields;
  std::vector<std::shared_ptr<arrow::Array>> aarrays;
  for (size_t i = 0; i < col_names.size(); ++i) {
    afields.push_back(arrow::field(col_names[i], arrow::int64()));
    arrow::Int64Builder b;
    if (!b.AppendValues(columns[i]).ok()) return fail("materialize: append failed");
    std::shared_ptr<arrow::Array> arr;
    if (!b.Finish(&arr).ok()) return fail("materialize: finish failed");
    aarrays.push_back(std::move(arr));
  }
  auto batch = arrow::RecordBatch::Make(arrow::schema(afields), nrows, aarrays);

  // Replace semantics: drop the catalog entry and purge old files through the
  // catalog seam (the only sanctioned file-removal path).
  if (!primeparts::catalog::DropTable(catalog, warehouse, name, /*purge=*/true,
                                      error))
    return false;

  primeparts::WriterConfig cfg;
  // Staging dir outside the warehouse; CommitFiles moves into place (seam).
  cfg.output_dir = primeparts::catalog::StagingDataDir(warehouse, name);
  cfg.schema = schema;
  cfg.table_name = name;
  cfg.filename_prefix = name;
  cfg.partition_spec = spec;
  cfg.partition_values =
      std::make_shared<iceberg::PartitionValues>(std::vector<iceberg::Literal>{});
  cfg.simple_filename = true;
  cfg.target_rows_per_file = 0;

  auto writer = primeparts::BucketParquetWriter::Make(cfg, error);
  if (!writer) return false;
  primeparts::BucketParquetWriter::BatchStats st{};  // no p column -> zeros
  if (!writer->Write(*batch, st, error)) return false;
  std::vector<primeparts::WrittenFile> written;
  if (!writer->Close(&written, error)) return false;

  std::vector<std::shared_ptr<iceberg::DataFile>> files;
  for (const auto& wf : written)
    if (wf.data_file) files.push_back(wf.data_file);

  return primeparts::catalog::CommitFiles(catalog, warehouse, name, schema, spec,
                                          files, metadata_location, error);
}

}  // namespace primeparts::query
