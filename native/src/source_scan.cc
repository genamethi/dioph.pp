#include "primeparts/source_scan.h"

#include <arrow/api.h>
#include <arrow/c/bridge.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "primeparts/common/arrow_init.h"
#include "primeparts/common/uri.h"
#include "primeparts/scan/column_binder.h"
#include "primeparts/scan/scan_planner.h"

#include "iceberg/arrow/arrow_io_util.h"
#include "iceberg/arrow_c_data.h"
#include "iceberg/data/file_scan_task_reader.h"
#include "iceberg/expression/expression.h"
#include "iceberg/file_io.h"
#include "iceberg/file_reader.h"
#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/manifest/manifest_list.h"
#include "iceberg/manifest/manifest_reader.h"
#include "iceberg/schema.h"
#include "iceberg/schema_field.h"
#include "iceberg/snapshot.h"
#include "iceberg/table_metadata.h"
#include "iceberg/table_scan.h"
#include "iceberg/type.h"

namespace primeparts {

namespace {

class IcebergReaderBatchReader : public arrow::RecordBatchReader {
 public:
  IcebergReaderBatchReader(std::unique_ptr<iceberg::Reader> reader,
                           std::shared_ptr<arrow::Schema> schema)
      : reader_(std::move(reader)), schema_(std::move(schema)) {}

  std::shared_ptr<arrow::Schema> schema() const override { return schema_; }

  arrow::Status ReadNext(std::shared_ptr<arrow::RecordBatch>* batch) override {
    auto next_r = reader_->Next();
    if (!next_r.has_value()) {
      return arrow::Status::IOError(next_r.error().message);
    }
    if (!next_r.value().has_value()) {
      *batch = nullptr;
      return arrow::Status::OK();
    }
    ArrowArray array = std::move(next_r.value().value());
    auto batch_r = arrow::ImportRecordBatch(&array, schema_);
    if (!batch_r.ok()) return batch_r.status();
    *batch = std::move(batch_r).ValueOrDie();
    return arrow::Status::OK();
  }

 private:
  std::unique_ptr<iceberg::Reader> reader_;
  std::shared_ptr<arrow::Schema> schema_;
};

}  // namespace

struct SourceTableReader::Impl {
  std::shared_ptr<iceberg::FileIO> io;
  scan::ScanPlan plan;
  std::vector<scan::FileScanTask> tasks;
  std::unique_ptr<iceberg::FileScanTaskReader> task_reader;
  size_t cursor = 0;

  std::shared_ptr<arrow::RecordBatchReader> active;

  std::string current_file_path;
  int64_t total_records = 0;
  int64_t planned_records = 0;

  std::string key_name;
  bool key_is_int32 = false;
  bool key_sliceable = false;

  static std::optional<int32_t> KeyAsInt32(
      const std::optional<iceberg::Literal>& bound) {
    if (!bound) return std::nullopt;
    if (const auto* v = std::get_if<int32_t>(&bound->value())) return *v;
    return std::nullopt;
  }

  static std::optional<int64_t> KeyAsInt64(
      const std::optional<iceberg::Literal>& bound) {
    if (!bound) return std::nullopt;
    if (const auto* v = std::get_if<int64_t>(&bound->value())) return *v;
    return std::nullopt;
  }

  bool ResolveKey(std::string* error) {
    if (!plan.key_lo && !plan.key_hi) return true;
    if (plan.traits.sort_keys.empty()) {
      if (error) *error = "scan plan has a key window but no sort key";
      return false;
    }
    const auto& key = plan.traits.sort_keys.front();
    key_name = key.name;
    for (const auto& f : plan.table_schema->fields()) {
      if (f.field_id() == key.field_id) {
        const auto type = f.type()->type_id();
        key_is_int32 = type == iceberg::TypeId::kInt;
        key_sliceable =
            type == iceberg::TypeId::kInt || type == iceberg::TypeId::kLong;
        return true;
      }
    }
    if (error) *error = "sort key " + key.name + " not in table schema";
    return false;
  }

  bool EnsureTaskReader(std::string* error) {
    if (task_reader) return true;
    iceberg::FileScanTaskReader::Options opts;
    opts.io = io;
    opts.table_schema = plan.table_schema;
    opts.projected_schema = plan.projected_schema;
    if (plan.read_batch_size > 0) {
      opts.properties["read.batch-size"] =
          std::to_string(plan.read_batch_size);
    }
    auto rdr_r = iceberg::FileScanTaskReader::Make(std::move(opts));
    if (!rdr_r.has_value()) {
      if (error) *error = "FileScanTaskReader::Make: " + rdr_r.error().message;
      return false;
    }
    task_reader = std::move(rdr_r.value());
    return true;
  }

  bool OpenMorTask(const scan::FileScanTask& task, std::string* error) {
    if (!EnsureTaskReader(error)) return false;
    auto stream_r = task_reader->Open(*task.inner);
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

  bool OpenSplitTask(const scan::FileScanTask& task, std::string* error) {
    iceberg::ReaderOptions opts;
    opts.path = task.inner->data_file()->file_path;
    opts.length =
        static_cast<size_t>(task.inner->data_file()->file_size_in_bytes);
    opts.split = task.split;
    opts.io = io;
    opts.projection = plan.projected_schema;
    auto reader_r = iceberg::ReaderFactoryRegistry::Open(
        task.inner->data_file()->file_format, opts);
    if (!reader_r.has_value()) {
      if (error) {
        *error = "ReaderFactoryRegistry::Open " + opts.path + ": " +
                 reader_r.error().message;
      }
      return false;
    }
    auto reader = std::move(reader_r.value());
    auto schema_r = reader->Schema();
    if (!schema_r.has_value()) {
      if (error) *error = "Reader::Schema: " + schema_r.error().message;
      return false;
    }
    ArrowSchema c_schema = schema_r.value();
    auto arrow_schema_r = arrow::ImportSchema(&c_schema);
    if (!arrow_schema_r.ok()) {
      if (error) *error = arrow_schema_r.status().ToString();
      return false;
    }
    active = std::make_shared<IcebergReaderBatchReader>(
        std::move(reader), std::move(arrow_schema_r).ValueOrDie());
    return true;
  }

  bool OpenTaskAtCursor(std::string* error) {
    active.reset();
    while (cursor < tasks.size()) {
      const auto& task = tasks[cursor];
      current_file_path = task.inner->data_file()->file_path;
      const bool mor = !task.inner->delete_files().empty();
      if (!mor && task.split.has_value()) {
        return OpenSplitTask(task, error);
      }
      return OpenMorTask(task, error);
    }
    return true;
  }

  bool SliceToKeyWindow(std::shared_ptr<arrow::RecordBatch>* batch,
                        std::string* error) {
    if (!plan.key_lo && !plan.key_hi) return true;
    if (!key_sliceable) return true;
    const int64_t n = (*batch)->num_rows();
    if (n == 0) return true;
    int64_t lower = 0;
    int64_t upper = n;
    if (key_is_int32) {
      const int32_t* v = scan::BindInt32(**batch, key_name, error);
      if (!v) return false;
      if (auto lo = KeyAsInt32(plan.key_lo)) {
        lower = std::lower_bound(v, v + n, *lo) - v;
      }
      if (auto hi = KeyAsInt32(plan.key_hi)) {
        upper = std::upper_bound(v, v + n, *hi) - v;
      }
    } else {
      const int64_t* v = scan::BindInt64(**batch, key_name, error);
      if (!v) return false;
      if (auto lo = KeyAsInt64(plan.key_lo)) {
        lower = std::lower_bound(v, v + n, *lo) - v;
      }
      if (auto hi = KeyAsInt64(plan.key_hi)) {
        upper = std::upper_bound(v, v + n, *hi) - v;
      }
    }
    if (upper <= lower) {
      *batch = (*batch)->Slice(0, 0);
      return true;
    }
    if (lower == 0 && upper == n) return true;
    *batch = (*batch)->Slice(lower, upper - lower);
    return true;
  }
};

std::unique_ptr<SourceTableReader> SourceTableReader::Open(
    scan::ScanPlan plan, std::shared_ptr<iceberg::FileIO> io,
    std::string* error, int shard_index, int shard_count) {
  primeparts::common::EnsureArrowRegistration();
  auto impl = std::make_unique<Impl>();
  impl->io = std::move(io);
  impl->plan = std::move(plan);
  if (!impl->ResolveKey(error)) return nullptr;

  if (shard_count > 1) {
    std::vector<scan::FileScanTask> mine;
    mine.reserve(impl->plan.tasks.size() / static_cast<size_t>(shard_count) + 1);
    for (size_t i = 0; i < impl->plan.tasks.size(); ++i) {
      if (static_cast<int>(i % static_cast<size_t>(shard_count)) == shard_index) {
        mine.push_back(std::move(impl->plan.tasks[i]));
      }
    }
    impl->tasks = std::move(mine);
  } else {
    impl->tasks = std::move(impl->plan.tasks);
  }
  impl->plan.tasks.clear();
  for (const auto& t : impl->tasks) impl->planned_records += t.planned_rows;
  impl->total_records = impl->planned_records;
  return std::unique_ptr<SourceTableReader>(
      new SourceTableReader(std::move(impl)));
}

namespace {

std::unique_ptr<SourceTableReader> OpenFromMetadata(
    const fs::path& metadata_path,
    const std::vector<std::string>& select_columns,
    std::shared_ptr<iceberg::Expression> filter,
    std::optional<int64_t> from_snapshot_id_exclusive, std::string* error,
    int shard_index, int shard_count) {
  primeparts::common::EnsureArrowRegistration();

  auto unique_io = iceberg::arrow::MakeLocalFileIO();
  std::shared_ptr<iceberg::FileIO> io = std::move(unique_io);

  auto md_r = iceberg::TableMetadataUtil::Read(*io, metadata_path.string());
  if (!md_r.has_value()) {
    if (error) {
      *error = "TableMetadata::Read " + metadata_path.string() + ": " +
               md_r.error().message;
    }
    return nullptr;
  }
  std::shared_ptr<iceberg::TableMetadata> metadata = std::move(md_r.value());

  int64_t manifest_total = 0;
  auto snap_r = metadata->Snapshot();
  if (snap_r.has_value() && snap_r.value()) {
    auto list_r = iceberg::ManifestListReader::Make(
        primeparts::common::StripFileScheme(snap_r.value()->manifest_list), io);
    if (list_r.has_value()) {
      auto files_r = list_r.value()->Files();
      if (files_r.has_value()) {
        for (const auto& mf : files_r.value()) {
          manifest_total += static_cast<int64_t>(mf.added_rows_count.value_or(0));
          manifest_total +=
              static_cast<int64_t>(mf.existing_rows_count.value_or(0));
          manifest_total -=
              static_cast<int64_t>(mf.deleted_rows_count.value_or(0));
        }
      }
    }
  }

  scan::ScanPlanRequest request;
  request.select = select_columns;
  request.filter = std::move(filter);
  if (from_snapshot_id_exclusive.has_value()) {
    if (!snap_r.has_value() || !snap_r.value()) {
      if (error) *error = "no current snapshot for incremental scan";
      return nullptr;
    }
    request.start_snapshot_id = *from_snapshot_id_exclusive;
    request.end_snapshot_id = snap_r.value()->snapshot_id;
  }

  scan::ScanPlan plan;
  if (!scan::PlanTableScan(metadata, io, request, &plan, error)) {
    return nullptr;
  }
  if (!scan::RefineSplits(&plan, io, request.case_sensitive, error)) {
    return nullptr;
  }

  auto reader = SourceTableReader::Open(std::move(plan), io, error, shard_index,
                                        shard_count);
  if (reader) reader->set_total_records(manifest_total);
  return reader;
}

}  // namespace

std::unique_ptr<SourceTableReader> SourceTableReader::OpenMetadata(
    const fs::path& metadata_path,
    const std::vector<std::string>& select_columns,
    std::shared_ptr<iceberg::Expression> filter,
    std::string* error,
    int shard_index, int shard_count) {
  return OpenFromMetadata(metadata_path, select_columns, std::move(filter),
                          std::nullopt, error, shard_index, shard_count);
}

std::unique_ptr<SourceTableReader> SourceTableReader::OpenIncremental(
    const fs::path& metadata_path,
    const std::vector<std::string>& select_columns,
    std::shared_ptr<iceberg::Expression> filter,
    int64_t from_snapshot_id_exclusive,
    std::string* error,
    int shard_index, int shard_count) {
  return OpenFromMetadata(metadata_path, select_columns, std::move(filter),
                          from_snapshot_id_exclusive, error, shard_index,
                          shard_count);
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
    if (batch) {
      if (!impl_->SliceToKeyWindow(&batch, error)) return false;
      if (batch->num_rows() > 0) {
        *out = batch;
        return true;
      }
      continue;
    }
    impl_->cursor++;
    if (!impl_->OpenTaskAtCursor(error)) return false;
  }

  *out = nullptr;
  return true;
}

int64_t SourceTableReader::total_records() const {
  return impl_->total_records;
}

int64_t SourceTableReader::planned_records() const {
  return impl_->planned_records;
}

int64_t SourceTableReader::file_count() const {
  return static_cast<int64_t>(impl_->tasks.size());
}

const std::string& SourceTableReader::current_data_file_path() const {
  return impl_->current_file_path;
}

const std::shared_ptr<iceberg::Expression>& SourceTableReader::residual() const {
  return impl_->plan.residual;
}

const scan::TableReadTraits& SourceTableReader::traits() const {
  return impl_->plan.traits;
}

void SourceTableReader::set_total_records(int64_t total) {
  impl_->total_records = total;
}

}  // namespace primeparts
