#include "primeparts/source_scan.h"

#include <arrow/api.h>
#include <arrow/c/bridge.h>

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

#include "primeparts/common/arrow_init.h"
#include "primeparts/common/uri.h"

#include "iceberg/arrow/arrow_io_util.h"
#include "iceberg/arrow_c_data.h"
#include "iceberg/data/file_scan_task_reader.h"
#include "iceberg/file_io.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/manifest/manifest_list.h"
#include "iceberg/manifest/manifest_reader.h"
#include "iceberg/metadata_columns.h"
#include "iceberg/snapshot.h"
#include "iceberg/partition_spec.h"
#include "iceberg/schema.h"
#include "iceberg/schema_field.h"
#include "iceberg/table_metadata.h"
#include "iceberg/table_scan.h"
#include "iceberg/type.h"

namespace primeparts {

namespace {

constexpr int32_t kPColumnFieldId = 1;

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

}

struct SourceTableReader::Impl {
  std::shared_ptr<iceberg::FileIO> io;
  std::shared_ptr<iceberg::TableMetadata> metadata;
  std::shared_ptr<iceberg::Schema> projected_schema;
  std::vector<std::shared_ptr<iceberg::FileScanTask>> tasks;
  std::unique_ptr<iceberg::FileScanTaskReader> task_reader;
  size_t cursor = 0;

  std::shared_ptr<arrow::RecordBatchReader> active;

  std::string current_file_path;

  int64_t total_records = 0;

  bool EnsureTaskReader(std::string* error) {
    if (task_reader) return true;
    iceberg::FileScanTaskReader::Options opts;
    opts.io = io;
    auto sch_r = metadata->Schema();
    if (!sch_r.has_value()) {
      if (error) *error = "metadata->Schema: " + sch_r.error().message;
      return false;
    }
    opts.table_schema = sch_r.value();
    opts.projected_schema = projected_schema;
    auto rdr_r = iceberg::FileScanTaskReader::Make(std::move(opts));
    if (!rdr_r.has_value()) {
      if (error) *error = "FileScanTaskReader::Make: " + rdr_r.error().message;
      return false;
    }
    task_reader = std::move(rdr_r.value());
    return true;
  }

  bool OpenTaskAtCursor(std::string* error) {
    if (!EnsureTaskReader(error)) return false;
    while (cursor < tasks.size()) {
      current_file_path = tasks[cursor]->data_file()->file_path;
      auto stream_r = task_reader->Open(*tasks[cursor]);
      if (!stream_r.has_value()) {
        if (error) *error = "FileScanTaskReader::Open: " + stream_r.error().message;
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
    return true;
  }
};

std::unique_ptr<SourceTableReader::Impl> SourceTableReader::BuildImpl(
    const fs::path& metadata_path,
    const std::vector<std::string>& select_columns,
    std::shared_ptr<iceberg::Expression> filter,
    std::optional<int64_t> from_snapshot_id_exclusive, std::string* error,
    int shard_index, int shard_count) {
  primeparts::common::EnsureArrowRegistration();

  auto impl = std::make_unique<SourceTableReader::Impl>();
  auto unique_io = iceberg::arrow::MakeLocalFileIO();
  impl->io = std::shared_ptr<iceberg::FileIO>(std::move(unique_io));

  auto md_r = iceberg::TableMetadataUtil::Read(*impl->io, metadata_path.string());
  if (!md_r.has_value()) {
    if (error) {
      *error = "TableMetadata::Read ";
      *error += metadata_path.string();
      *error += ": " + md_r.error().message;
    }
    return nullptr;
  }
  impl->metadata = std::shared_ptr<iceberg::TableMetadata>(std::move(md_r.value()));

  auto snap_r = impl->metadata->Snapshot();
  if (snap_r.has_value() && snap_r.value()) {
    auto list_r = iceberg::ManifestListReader::Make(
        primeparts::common::StripFileScheme(snap_r.value()->manifest_list), impl->io);
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

  std::vector<std::string> regular_columns;
  bool want_pos = false, want_file = false;
  for (const auto& c : select_columns) {
    if (c == "_pos") { want_pos = true; }
    else if (c == "_file") { want_file = true; }
    else { regular_columns.push_back(c); }
  }

  std::shared_ptr<iceberg::Schema> scan_schema;
  std::vector<std::shared_ptr<iceberg::FileScanTask>> planned;

  if (from_snapshot_id_exclusive.has_value()) {
    if (!snap_r.has_value() || !snap_r.value()) {
      if (error) *error = "no current snapshot for incremental scan";
      return nullptr;
    }
    auto builder_r =
        iceberg::TableScanBuilder<iceberg::IncrementalAppendScan>::Make(
            impl->metadata, impl->io);
    if (!builder_r.has_value()) {
      if (error) *error = "TableScanBuilder::Make: " + builder_r.error().message;
      return nullptr;
    }
    auto scan_builder = std::move(builder_r.value());
    scan_builder->Select(regular_columns).IncludeColumnStats({"p"});
    if (filter) scan_builder->Filter(filter);
    scan_builder->FromSnapshot(*from_snapshot_id_exclusive, false)
        .ToSnapshot(snap_r.value()->snapshot_id);
    auto scan_r = scan_builder->Build();
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
    scan_schema = schema_r.value();
    auto tasks_r = scan->PlanFiles();
    if (!tasks_r.has_value()) {
      if (error) *error = "scan->PlanFiles: " + tasks_r.error().message;
      return nullptr;
    }
    planned = std::move(tasks_r.value());
  } else {
    auto builder_r =
        iceberg::TableScanBuilder<iceberg::DataTableScan>::Make(impl->metadata,
                                                                impl->io);
    if (!builder_r.has_value()) {
      if (error) *error = "TableScanBuilder::Make: " + builder_r.error().message;
      return nullptr;
    }
    auto scan_builder = std::move(builder_r.value());
    scan_builder->Select(regular_columns).IncludeColumnStats({"p"});
    if (filter) scan_builder->Filter(filter);
    auto scan_r = scan_builder->Build();
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
    scan_schema = schema_r.value();
    auto tasks_r = scan->PlanFiles();
    if (!tasks_r.has_value()) {
      if (error) *error = "scan->PlanFiles: " + tasks_r.error().message;
      return nullptr;
    }
    planned = std::move(tasks_r.value());
  }

  if (want_pos || want_file) {
    std::vector<iceberg::SchemaField> aug(scan_schema->fields().begin(),
                                          scan_schema->fields().end());
    if (want_file) aug.push_back(iceberg::MetadataColumns::kFilePath);
    if (want_pos) aug.push_back(iceberg::MetadataColumns::kRowPosition);
    impl->projected_schema =
        std::make_shared<iceberg::Schema>(std::move(aug), scan_schema->schema_id());
  } else {
    impl->projected_schema = scan_schema;
  }

  impl->tasks = std::move(planned);

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

  if (shard_count > 1) {
    std::vector<std::shared_ptr<iceberg::FileScanTask>> mine;
    mine.reserve(impl->tasks.size() / static_cast<size_t>(shard_count) + 1);
    for (size_t i = 0; i < impl->tasks.size(); ++i) {
      if (static_cast<int>(i % static_cast<size_t>(shard_count)) == shard_index) {
        mine.push_back(std::move(impl->tasks[i]));
      }
    }
    impl->tasks = std::move(mine);
  }

  return impl;
}

std::unique_ptr<SourceTableReader> SourceTableReader::OpenMetadata(
    const fs::path& metadata_path,
    const std::vector<std::string>& select_columns,
    std::shared_ptr<iceberg::Expression> filter,
    std::string* error,
    int shard_index, int shard_count) {
  auto impl = BuildImpl(metadata_path, select_columns, std::move(filter),
                        std::nullopt, error, shard_index, shard_count);
  if (!impl) return nullptr;
  return std::unique_ptr<SourceTableReader>(
      new SourceTableReader(std::move(impl)));
}

std::unique_ptr<SourceTableReader> SourceTableReader::OpenIncremental(
    const fs::path& metadata_path,
    const std::vector<std::string>& select_columns,
    std::shared_ptr<iceberg::Expression> filter,
    int64_t from_snapshot_id_exclusive,
    std::string* error,
    int shard_index, int shard_count) {
  auto impl = BuildImpl(metadata_path, select_columns, std::move(filter),
                        from_snapshot_id_exclusive, error, shard_index,
                        shard_count);
  if (!impl) return nullptr;
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
    impl_->active.reset();
    impl_->cursor++;
    if (!impl_->OpenTaskAtCursor(error)) return false;
  }

  *out = nullptr;
  return true;
}

int64_t SourceTableReader::total_records() const {
  return impl_->total_records;
}

int64_t SourceTableReader::file_count() const {
  return static_cast<int64_t>(impl_->tasks.size());
}

const std::string& SourceTableReader::current_data_file_path() const {
  return impl_->current_file_path;
}

std::vector<SourceFileInfo> SourceTableReader::source_files() const {
  std::vector<SourceFileInfo> out;
  out.reserve(impl_->tasks.size());
  for (const auto& task : impl_->tasks) {
    const auto* df = task->data_file().get();
    SourceFileInfo info;
    info.path = primeparts::common::StripFileScheme(df->file_path);
    info.record_count = static_cast<int64_t>(df->record_count);
    auto lo_it = df->lower_bounds.find(kPColumnFieldId);
    auto hi_it = df->upper_bounds.find(kPColumnFieldId);
    info.p_min = (lo_it != df->lower_bounds.end()) ? decode_int64_le(lo_it->second) : 0;
    info.p_max = (hi_it != df->upper_bounds.end()) ? decode_int64_le(hi_it->second) : 0;
    out.push_back(std::move(info));
  }
  return out;
}

}
