# 05_resume

deps: 01 | status: done (reworked post-review)

done: resume state is writer-declared, not reconstructed. `AlignedBucketWriter::Finish` exports `CommitPlan.resume` (bucket_version, bucket, bucket_bytes, next_seq per table; seq via new `BucketParquetWriter::next_file_seq()`, bytes recomputed at bucket close so the final flush counts). generate maps it through `AlignedResumeSummary(resume, table, reference)` into `TableCommitSpec.summary`; pp_commit applies via `SnapshotUpdate::Set` so the `pp.aligned.*` keys land in the snapshot summary atomically with the append. `LoadAlignedResume(catalog, ns, tables, reference_table, bucket_version, out, error)` reads `current_snapshot()->summary`: no table / no snapshot ⇒ fresh; keys missing ⇒ hard error; declared version > requested ⇒ hard error, < requested ⇒ fresh; non-reference bucket > reference frontier ⇒ hard error, < ⇒ seq 0. Keys: `pp.aligned.bucket-version|bucket|bucket-bytes|next-seq` (bytes on reference table only).

history: first version reconstructed state from SnapshotCache::DataManifests → ManifestReader::LiveEntries → partition tuples + filename seq parse; superseded (recovery: `git show ef8b48e:native/src/aligned_writer.cc` era) — writer already holds the state at commit, so reconstruction was hand-wiring. BucketFields and SeqFromFilename deleted with it.

grep gate: `grep -n 'ManifestReader\|SeqFromFilename\|BucketFields\|directory_iterator' native/src/aligned_writer.cc native/src/writer.cc` → 0

## notes

- Semantics change vs manifest walk: resume trusts only the LAST snapshot's declaration per table; a snapshot committed without the keys (pre-rework warehouse, foreign writer) is a loud error, not a recompute — registry hole.
- `ResumeState.bucket_fill_bytes` renamed `bucket_bytes`; gained `bucket_version` (self-describing for summary round-trip).
