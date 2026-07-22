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

namespace {

std::shared_ptr<iceberg::Type> IcebergType(ColumnType t) {
  switch (t) {
    case ColumnType::kInt:
      return iceberg::int32();
    case ColumnType::kString:
      return iceberg::string();
    case ColumnType::kLong:
    default:
      return iceberg::int64();
  }
}

std::shared_ptr<arrow::DataType> ArrowType(ColumnType t) {
  switch (t) {
    case ColumnType::kInt:
      return arrow::int32();
    case ColumnType::kString:
      return arrow::utf8();
    case ColumnType::kLong:
    default:
      return arrow::int64();
  }
}

int64_t ColumnRows(const MaterializeColumn& c) {
  return c.type == ColumnType::kString
             ? static_cast<int64_t>(c.strings.size())
             : static_cast<int64_t>(c.ints.size());
}

bool BuildArray(const MaterializeColumn& c,
                std::shared_ptr<arrow::Array>* out, std::string* error) {
  auto fail = [&](const std::string& m) { if (error) *error = m; return false; };
  if (c.type == ColumnType::kString) {
    arrow::StringBuilder b;
    if (!b.AppendValues(c.strings).ok())
      return fail("materialize: string append failed for " + c.name);
    return b.Finish(out).ok() || fail("materialize: finish failed for " + c.name);
  }
  if (c.type == ColumnType::kInt) {
    arrow::Int32Builder b;
    for (int64_t v : c.ints)
      if (!b.Append(static_cast<int32_t>(v)).ok())
        return fail("materialize: int append failed for " + c.name);
    return b.Finish(out).ok() || fail("materialize: finish failed for " + c.name);
  }
  arrow::Int64Builder b;
  if (!b.AppendValues(c.ints).ok())
    return fail("materialize: long append failed for " + c.name);
  return b.Finish(out).ok() || fail("materialize: finish failed for " + c.name);
}

}  // namespace

bool MaterializeColumns(const std::shared_ptr<iceberg::Catalog>& catalog,
                        const iceberg::Namespace& ns, const fs::path& warehouse,
                        const std::string& name,
                        const std::vector<MaterializeColumn>& columns,
                        std::string* metadata_location, std::string* error) {
  auto fail = [&](const std::string& m) { if (error) *error = m; return false; };
  if (columns.empty()) return fail("materialize: no columns");
  const int64_t nrows = ColumnRows(columns[0]);
  for (const auto& c : columns)
    if (ColumnRows(c) != nrows) return fail("materialize: ragged columns");

  std::vector<iceberg::SchemaField> fields;
  fields.reserve(columns.size());
  std::vector<std::shared_ptr<arrow::Field>> afields;
  std::vector<std::shared_ptr<arrow::Array>> aarrays;
  std::vector<primeparts::WriterConfig::StatColumn> stat_columns;
  for (size_t i = 0; i < columns.size(); ++i) {
    const auto& c = columns[i];
    fields.push_back(iceberg::SchemaField::MakeRequired(
        static_cast<int32_t>(i + 1), c.name, IcebergType(c.type)));
    afields.push_back(arrow::field(c.name, ArrowType(c.type)));
    std::shared_ptr<arrow::Array> arr;
    if (!BuildArray(c, &arr, error)) return false;
    aarrays.push_back(std::move(arr));
    if (c.stat) stat_columns.push_back({c.name, false});
  }
  auto schema = std::make_shared<iceberg::Schema>(std::move(fields), 0);
  auto spec = iceberg::PartitionSpec::Unpartitioned();
  auto batch = arrow::RecordBatch::Make(arrow::schema(afields), nrows, aarrays);

  if (!primeparts::catalog::DropTable(catalog, ns, warehouse, name, true, error))
    return false;

  primeparts::WriterConfig cfg;
  cfg.output_dir = primeparts::catalog::StagingDataDir(warehouse, ns, name);
  cfg.schema = schema;
  cfg.table_name = name;
  cfg.filename_prefix = name;
  cfg.partition_spec = spec;
  cfg.partition_values =
      std::make_shared<iceberg::PartitionValues>(std::vector<iceberg::Literal>{});
  cfg.stat_columns = std::move(stat_columns);
  cfg.simple_filename = true;
  cfg.target_rows_per_file = 0;

  auto writer = primeparts::BucketParquetWriter::Make(cfg, error);
  if (!writer) return false;
  if (!writer->Write(*batch, error)) return false;
  std::vector<primeparts::WrittenFile> written;
  if (!writer->Close(&written, error)) return false;

  std::vector<std::shared_ptr<iceberg::DataFile>> files;
  for (const auto& wf : written)
    if (wf.data_file) files.push_back(wf.data_file);

  return primeparts::catalog::CommitFiles(catalog, ns, warehouse, name, schema,
                                          spec,
                                          primeparts::catalog::TableDeclaration{},
                                          files, metadata_location, error);
}

bool MaterializeIntColumns(
    const std::shared_ptr<iceberg::Catalog>& catalog,
    const iceberg::Namespace& ns, const fs::path& warehouse,
    const std::string& name, const std::vector<std::string>& col_names,
    const std::vector<std::vector<int64_t>>& columns,
    std::string* metadata_location, std::string* error) {
  if (col_names.size() != columns.size()) {
    if (error) *error = "materialize: col_names/columns size mismatch";
    return false;
  }
  std::vector<MaterializeColumn> cols;
  cols.reserve(col_names.size());
  for (size_t i = 0; i < col_names.size(); ++i)
    cols.push_back({col_names[i], ColumnType::kLong, columns[i], {}, false});
  return MaterializeColumns(catalog, ns, warehouse, name, cols,
                            metadata_location, error);
}

}  // namespace primeparts::query
