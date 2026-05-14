#include "primeparts/source_scan.h"

#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <sqlite3.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "iceberg/arrow/arrow_file_io.h"
#include "iceberg/arrow_c_data.h"
#include "iceberg/avro/avro_register.h"
#include "iceberg/file_io.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/manifest/manifest_list.h"
#include "iceberg/manifest/manifest_reader.h"
#include "iceberg/snapshot.h"
#include "iceberg/parquet/parquet_register.h"
#include "iceberg/partition_spec.h"
#include "iceberg/schema.h"
#include "iceberg/schema_field.h"
#include "iceberg/table_metadata.h"
#include "iceberg/table_scan.h"
#include "iceberg/type.h"

namespace primeparts {

namespace {

constexpr int32_t kPColumnFieldId = 1;  // `p` is field_id 1 in both source tables.

std::string strip_file_scheme(const std::string& uri) {
  if (uri.rfind("file://", 0) == 0) return uri.substr(7);
  return uri;
}

int64_t decode_int64_le(const std::vector<uint8_t>& bytes) {
  int64_t v = 0;
  const size_t n = bytes.size() < 8 ? bytes.size() : 8;
  for (size_t i = 0; i < n; ++i) {
    v |= static_cast<int64_t>(bytes[i]) << (8 * i);
  }
  if (n == 4 && (bytes[3] & 0x80)) {
    v |= ~static_cast<int64_t>(0) << 32;
  }
  return v;
}

void ensure_format_registered() {
  static std::once_flag once;
  std::call_once(once, [] {
    iceberg::avro::RegisterAll();
    iceberg::parquet::RegisterAll();
  });
}

bool sqlite_lookup_metadata_location(const fs::path& sqlite_path,
                                     std::string_view ns,
                                     std::string_view tbl,
                                     std::string* out,
                                     std::string* error) {
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(sqlite_path.c_str(), &db, SQLITE_OPEN_READONLY,
                      nullptr) != SQLITE_OK) {
    if (error) {
      *error = "sqlite3_open_v2 ";
      *error += sqlite_path.string();
      if (db) {
        *error += ": ";
        *error += sqlite3_errmsg(db);
      }
    }
    if (db) sqlite3_close(db);
    return false;
  }
  const char* sql =
      "SELECT metadata_location FROM iceberg_tables "
      "WHERE table_namespace = ?1 AND table_name = ?2 LIMIT 1";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    if (error) {
      *error = "sqlite3_prepare_v2: ";
      *error += sqlite3_errmsg(db);
    }
    sqlite3_close(db);
    return false;
  }
  std::string ns_owned(ns), tbl_owned(tbl);
  sqlite3_bind_text(stmt, 1, ns_owned.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, tbl_owned.c_str(), -1, SQLITE_TRANSIENT);
  bool ok = false;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char* loc = sqlite3_column_text(stmt, 0);
    if (loc) {
      *out = strip_file_scheme(reinterpret_cast<const char*>(loc));
      ok = !out->empty();
    }
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  if (!ok && error) {
    *error = "no iceberg_tables row for ";
    *error += ns;
    *error += '.';
    *error += tbl;
  }
  return ok;
}

}  // namespace

struct SourceTableReader::Impl {
  std::shared_ptr<iceberg::FileIO> io;
  std::shared_ptr<iceberg::TableMetadata> metadata;
  std::shared_ptr<iceberg::Schema> projected_schema;
  std::vector<std::shared_ptr<iceberg::FileScanTask>> tasks;
  size_t cursor = 0;

  // Active task's RecordBatchReader, if one is mid-read.
  std::shared_ptr<arrow::RecordBatchReader> active;

  int64_t total_records = 0;

  bool OpenTaskAtCursor(std::string* error) {
    while (cursor < tasks.size()) {
      auto stream_r = tasks[cursor]->ToArrow(io, projected_schema);
      if (!stream_r.has_value()) {
        if (error) *error = "FileScanTask::ToArrow: " + stream_r.error().message;
        return false;
      }
      ArrowArrayStream stream = stream_r.value();
      auto rdr_r = arrow::ImportRecordBatchReader(&stream);
      if (!rdr_r.ok()) {
        if (error) *error = rdr_r.status().ToString();
        return false;
      }
      active = rdr_r.ValueOrDie();
      return true;
    }
    active.reset();
    return true;  // EOF, no active reader.
  }
};

std::unique_ptr<SourceTableReader> SourceTableReader::Open(
    const fs::path& sqlite_path, std::string_view ns, std::string_view tbl,
    const std::vector<std::string>& select_columns, std::string* error) {
  ensure_format_registered();

  std::string metadata_path;
  if (!sqlite_lookup_metadata_location(sqlite_path, ns, tbl, &metadata_path,
                                       error)) {
    return nullptr;
  }

  auto impl = std::make_unique<Impl>();
  auto unique_io = iceberg::arrow::MakeLocalFileIO();
  impl->io = std::shared_ptr<iceberg::FileIO>(std::move(unique_io));

  auto md_r = iceberg::TableMetadataUtil::Read(*impl->io, metadata_path);
  if (!md_r.has_value()) {
    if (error) {
      *error = "TableMetadata::Read ";
      *error += metadata_path;
      *error += ": " + md_r.error().message;
    }
    return nullptr;
  }
  impl->metadata = std::shared_ptr<iceberg::TableMetadata>(std::move(md_r.value()));

  // Manifest-aggregated total row count: needed cheaply by the
  // rewriter to validate against EXPECTED_N_PRIMES.
  auto snap_r = impl->metadata->Snapshot();  // current snapshot
  if (snap_r.has_value() && snap_r.value()) {
    auto list_r = iceberg::ManifestListReader::Make(
        strip_file_scheme(snap_r.value()->manifest_list), impl->io);
    if (list_r.has_value()) {
      auto files_r = list_r.value()->Files();
      if (files_r.has_value()) {
        for (const auto& mf : files_r.value()) {
          impl->total_records +=
              static_cast<int64_t>(mf.added_rows_count.value_or(0));
          impl->total_records +=
              static_cast<int64_t>(mf.existing_rows_count.value_or(0));
          impl->total_records -=
              static_cast<int64_t>(mf.deleted_rows_count.value_or(0));
        }
      }
    }
  }

  auto builder_r =
      iceberg::TableScanBuilder<iceberg::DataTableScan>::Make(impl->metadata,
                                                              impl->io);
  if (!builder_r.has_value()) {
    if (error) *error = "TableScanBuilder::Make: " + builder_r.error().message;
    return nullptr;
  }
  auto scan_r = builder_r.value()
                    ->Select(select_columns)
                    .IncludeColumnStats({"p"})
                    .Build();
  if (!scan_r.has_value()) {
    if (error) *error = "TableScanBuilder::Build: " + scan_r.error().message;
    return nullptr;
  }
  auto scan = std::move(scan_r.value());

  auto schema_r = scan->schema();
  if (!schema_r.has_value()) {
    if (error) *error = "scan->schema: " + schema_r.error().message;
    return nullptr;
  }
  impl->projected_schema = schema_r.value();

  auto tasks_r = scan->PlanFiles();
  if (!tasks_r.has_value()) {
    if (error) *error = "scan->PlanFiles: " + tasks_r.error().message;
    return nullptr;
  }
  impl->tasks = std::move(tasks_r.value());

  // Sort tasks by `p` lower-bound so the producer streams p-ascending.
  std::sort(impl->tasks.begin(), impl->tasks.end(),
            [](const std::shared_ptr<iceberg::FileScanTask>& a,
               const std::shared_ptr<iceberg::FileScanTask>& b) {
              auto la = a->data_file()->lower_bounds.find(kPColumnFieldId);
              auto lb = b->data_file()->lower_bounds.find(kPColumnFieldId);
              int64_t va =
                  la != a->data_file()->lower_bounds.end()
                      ? decode_int64_le(la->second)
                      : 0;
              int64_t vb =
                  lb != b->data_file()->lower_bounds.end()
                      ? decode_int64_le(lb->second)
                      : 0;
              return va < vb;
            });

  return std::unique_ptr<SourceTableReader>(
      new SourceTableReader(std::move(impl)));
}

SourceTableReader::SourceTableReader(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

SourceTableReader::~SourceTableReader() = default;

bool SourceTableReader::Next(std::shared_ptr<arrow::RecordBatch>* out,
                             std::string* error) {
  if (!impl_->active && !impl_->OpenTaskAtCursor(error)) return false;

  while (impl_->active) {
    std::shared_ptr<arrow::RecordBatch> batch;
    auto st = impl_->active->ReadNext(&batch);
    if (!st.ok()) {
      if (error) *error = st.ToString();
      return false;
    }
    if (batch && batch->num_rows() > 0) {
      *out = batch;
      return true;
    }
    // EOF for this task; advance.
    impl_->active.reset();
    impl_->cursor++;
    if (!impl_->OpenTaskAtCursor(error)) return false;
  }

  *out = nullptr;  // overall EOF
  return true;
}

int64_t SourceTableReader::total_records() const {
  return impl_->total_records;
}

int64_t SourceTableReader::file_count() const {
  return static_cast<int64_t>(impl_->tasks.size());
}

std::vector<SourceFileInfo> SourceTableReader::source_files() const {
  std::vector<SourceFileInfo> out;
  out.reserve(impl_->tasks.size());
  for (const auto& task : impl_->tasks) {
    const auto* df = task->data_file().get();
    SourceFileInfo info;
    info.path = strip_file_scheme(df->file_path);
    info.record_count = static_cast<int64_t>(df->record_count);
    auto lo_it = df->lower_bounds.find(kPColumnFieldId);
    auto hi_it = df->upper_bounds.find(kPColumnFieldId);
    info.p_min = (lo_it != df->lower_bounds.end()) ? decode_int64_le(lo_it->second) : 0;
    info.p_max = (hi_it != df->upper_bounds.end()) ? decode_int64_le(hi_it->second) : 0;
    out.push_back(std::move(info));
  }
  return out;
}

}  // namespace primeparts
