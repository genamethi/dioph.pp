// primeparts/catalog/pp_row_delta.h
//
// RowDelta — a committable position-delete snapshot update.
//
// The vendored iceberg-cpp (v0.2.0-123-g3fed150) ships a first-class
// PositionDeleteWriter and an implemented SnapshotUpdate::WriteDeleteManifests,
// but exposes NO committable delete update class and no Transaction accessor for
// one (Transaction has NewFastAppend but nothing for deletes). This fills that
// gap *without modifying the vendored library*: RowDelta subclasses the
// installed, exported SnapshotUpdate and calls its protected delete-manifest
// path. It commits through the generic PendingUpdate::Commit ->
// Transaction::ApplyUpdateSnapshot dispatch, so no Transaction-core change is
// needed.
//
// This is the spike vehicle for "native position-delete write->commit->reopen->
// read" (markdown/data_eng/delete_primitive_spike.md). It mirrors FastAppend
// (vendor/iceberg-cpp/src/iceberg/update/fast_append.{h,cc}) with the data path
// swapped for the delete path.
//
// Scope note: intended for the covering-sieve's fixed-source delete model
// (delete only from a pre-existing primes_k0 copy), so the
// snapshot_update.cc:212 data_sequence_number FIXME does not apply — deletes
// always post-date the data and correctly inherit the new snapshot's sequence
// number.

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "iceberg/result.h"
#include "iceberg/snapshot.h"
#include "iceberg/type_fwd.h"
#include "iceberg/update/snapshot_update.h"
#include "iceberg/util/data_file_set.h"

namespace primeparts::catalog {

/// Accumulates position-delete DataFiles and commits them as a single `delete`
/// snapshot, carrying forward all manifests from the parent snapshot.
class RowDelta : public iceberg::SnapshotUpdate {
 public:
  /// Build a RowDelta against a loaded table. Internally creates a
  /// TransactionContext (kUpdate); Commit() will drive a temporary transaction.
  static iceberg::Result<std::shared_ptr<RowDelta>> Make(
      std::shared_ptr<iceberg::Table> table);

  /// Build a RowDelta on an existing transaction context (when batching with
  /// other updates in one transaction).
  static iceberg::Result<std::shared_ptr<RowDelta>> Make(
      std::shared_ptr<iceberg::TransactionContext> ctx);

  /// Add a position-delete file (DataFile with content == kPositionDeletes).
  RowDelta& AddDeleteFile(const std::shared_ptr<iceberg::DataFile>& file);

  // --- SnapshotUpdate contract ---
  std::string operation() override;
  iceberg::Result<std::vector<iceberg::ManifestFile>> Apply(
      const iceberg::TableMetadata& metadata_to_update,
      const std::shared_ptr<iceberg::Snapshot>& snapshot) override;
  std::unordered_map<std::string, std::string> Summary() override;
  // v0.3.0: base CleanUncommitted return type changed void -> Status.
  iceberg::Status CleanUncommitted(
      const std::unordered_set<std::string>& committed) override;

 private:
  explicit RowDelta(std::shared_ptr<iceberg::TransactionContext> ctx);

  // Write delete manifests for the accumulated delete files (idempotent across
  // retries: cleans previously-written-but-uncommitted manifests first).
  iceberg::Result<std::vector<iceberg::ManifestFile>> WriteNewDeleteManifests();

  std::unordered_map<int32_t, iceberg::DataFileSet> new_delete_files_by_spec_;
  std::vector<iceberg::ManifestFile> new_manifests_;
  bool has_new_files_{false};
};

}  // namespace primeparts::catalog
