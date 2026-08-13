# 05_resume

status: done (merged 0bb87bd; reworked three times post-review)

Resume rides spec partition statistics files. Module `catalog/partition_stats.{h,cc}` (the PartitionStatsHandler iceberg-cpp lacks): compute-from-manifests bootstrap, incremental merge, write/read via vendored Writer/Reader registries through FileIO (atomicity from the catalog transaction, no rename); pp_commit registers `SetPartitionStatistics` in the same atomic updateTable as AddSnapshot/SetSnapshotRef. `LoadAlignedResume` reads stats rows: frontier bucket, bucket bytes, next_seq = frontier row data_file_count.

Load-bearing invariants:
- next_seq = data_file_count leans on file seqs dense from 0 per bucket; only the aligned writer commits into these tables.
- Resume reflects COMMITTED files only; failed transactions can orphan stats parquets (registry: no expiry/cleanup surface); a retried same-snapshot commit overwrites its orphan (deterministic filename).
- Pre-stats warehouses self-heal: unregistered snapshot ⇒ compute from manifests, next commit registers a real file.

History: v1 bespoke manifest walk, v2 `pp.aligned.*` summary keys, v3 spec stats file, v3.1 FileIO-native via vendored registries — git log on partition_stats.cc / aligned_writer.cc.
