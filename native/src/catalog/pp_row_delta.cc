// primeparts/catalog/pp_row_delta.cc — see header.
//
// Mirrors vendor/iceberg-cpp/src/iceberg/update/fast_append.cc, swapping the
// data path for the delete path:
//   * AppendFile           -> AddDeleteFile  (asserts kPositionDeletes content)
//   * WriteDataManifests   -> WriteDeleteManifests
//   * operation() kAppend  -> kDelete
// The parent-manifest carry-forward (SnapshotCache) is identical: a delete
// snapshot keeps all of the parent's data + delete manifests and adds the new
// delete manifests on top.

#include "primeparts/catalog/pp_row_delta.h"

#include <iterator>
#include <utility>

#include "iceberg/manifest/manifest_entry.h"
#include "iceberg/partition_spec.h"
#include "iceberg/schema.h"
#include "iceberg/table.h"
#include "iceberg/table_metadata.h"
#include "iceberg/table_properties.h"
#include "iceberg/transaction.h"

namespace primeparts::catalog {

using iceberg::DataFile;
using iceberg::ErrorKind;  // referenced unqualified by ICEBERG_BUILDER_CHECK
using iceberg::ManifestFile;
using iceberg::PartitionSpec;
using iceberg::Result;
using iceberg::Snapshot;
using iceberg::SnapshotCache;
using iceberg::Table;
using iceberg::TableMetadata;
using iceberg::TransactionContext;
using iceberg::TransactionKind;

Result<std::shared_ptr<RowDelta>> RowDelta::Make(std::shared_ptr<Table> table) {
  auto ctx = TransactionContext::Make(std::move(table), TransactionKind::kUpdate);
  if (!ctx.has_value()) return std::unexpected(ctx.error());
  return Make(std::move(ctx.value()));
}

Result<std::shared_ptr<RowDelta>> RowDelta::Make(
    std::shared_ptr<TransactionContext> ctx) {
  if (ctx == nullptr) {
    return std::unexpected(iceberg::Error{
        .kind = iceberg::ErrorKind::kInvalidArgument,
        .message = "Cannot create RowDelta without a context"});
  }
  return std::shared_ptr<RowDelta>(new RowDelta(std::move(ctx)));
}

RowDelta::RowDelta(std::shared_ptr<TransactionContext> ctx)
    : iceberg::SnapshotUpdate(std::move(ctx)) {}

RowDelta& RowDelta::AddDeleteFile(const std::shared_ptr<DataFile>& file) {
  ICEBERG_BUILDER_CHECK(file != nullptr, "Invalid delete file: null");
  ICEBERG_BUILDER_CHECK(
      file->content == DataFile::Content::kPositionDeletes,
      "RowDelta::AddDeleteFile expects a position-delete file");
  ICEBERG_BUILDER_CHECK(file->partition_spec_id.has_value(),
                        "Delete file must have partition spec ID");

  int32_t spec_id = file->partition_spec_id.value();
  auto spec_r = base().PartitionSpecById(spec_id);
  ICEBERG_BUILDER_CHECK(spec_r.has_value(),
                        "Unknown partition spec id for delete file");

  auto& files = new_delete_files_by_spec_[spec_id];
  auto [iter, inserted] = files.insert(file);
  if (inserted) {
    has_new_files_ = true;
    // A position-delete file is an *added* file of delete content; AddedFile
    // branches on content and updates added-delete-files / added-position-
    // deletes (verified in snapshot.cc UpdateMetrics::AddedFile).
    ICEBERG_BUILDER_RETURN_IF_ERROR(
        summary_builder().AddedFile(*spec_r.value(), *file));
  }
  return *this;
}

std::string RowDelta::operation() { return iceberg::DataOperation::kDelete; }

Result<std::vector<ManifestFile>> RowDelta::WriteNewDeleteManifests() {
  // Clean up any manifests written on a prior (conflicting) Apply attempt.
  if (has_new_files_ && !new_manifests_.empty()) {
    for (const auto& m : new_manifests_) {
      std::ignore = DeleteFile(m.manifest_path);
    }
    new_manifests_.clear();
  }

  if (new_manifests_.empty() && !new_delete_files_by_spec_.empty()) {
    for (const auto& [spec_id, files] : new_delete_files_by_spec_) {
      auto spec_r = base().PartitionSpecById(spec_id);
      if (!spec_r.has_value()) return std::unexpected(spec_r.error());
      auto written = WriteDeleteManifests(files.as_span(), spec_r.value());
      if (!written.has_value()) return std::unexpected(written.error());
      new_manifests_.insert(new_manifests_.end(),
                            std::make_move_iterator(written.value().begin()),
                            std::make_move_iterator(written.value().end()));
    }
    has_new_files_ = false;
  }
  return new_manifests_;
}

Result<std::vector<ManifestFile>> RowDelta::Apply(
    const TableMetadata& /*metadata_to_update*/,
    const std::shared_ptr<Snapshot>& snapshot) {
  std::vector<ManifestFile> manifests;

  auto new_written = WriteNewDeleteManifests();
  if (!new_written.has_value()) return std::unexpected(new_written.error());
  manifests.insert(manifests.end(),
                   std::make_move_iterator(new_written.value().begin()),
                   std::make_move_iterator(new_written.value().end()));

  // Carry forward every manifest (data + delete) from the parent snapshot.
  if (snapshot != nullptr) {
    auto cached = SnapshotCache(snapshot.get());
    auto parent = cached.Manifests(ctx_->table->io());
    if (!parent.has_value()) return std::unexpected(parent.error());
    manifests.insert(manifests.end(), parent.value().begin(),
                     parent.value().end());
  }
  return manifests;
}

std::unordered_map<std::string, std::string> RowDelta::Summary() {
  summary_builder().SetPartitionSummaryLimit(
      base().properties.Get(
          iceberg::TableProperties::kWritePartitionSummaryLimit));
  return summary_builder().Build();
}

void RowDelta::CleanUncommitted(
    const std::unordered_set<std::string>& committed) {
  if (new_manifests_.empty()) return;
  for (const auto& m : new_manifests_) {
    if (!committed.contains(m.manifest_path)) {
      std::ignore = DeleteFile(m.manifest_path);
    }
  }
  new_manifests_.clear();
}

}  // namespace primeparts::catalog
