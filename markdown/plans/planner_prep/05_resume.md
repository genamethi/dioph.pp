# 05_resume

deps: 01 | status: done

done: LoadAlignedResume rebuilt over the catalog — `(catalog, ns, tables, reference_table, BucketFields{version_field,bucket_field}, bucket_version, ResumeState*, error)`; caller declares the bucket-scheme partition fields. Per table: LoadTable (NoSuchTable ⇒ fresh) → current snapshot (none ⇒ fresh) → SnapshotCache::DataManifests → ManifestReader::LiveEntries → partition tuples read at the declared fields' spec positions (non-int32 or missing ⇒ hard error); frontier = max bucket at version; fill = Σ file_size_in_bytes (reference table); next_seq = 1 + max trailing seq from committed basenames (unparseable ⇒ hard error). fs-glob implementation and the false header claim deleted. generate.cc opens the catalog before writer construction (skipped under --temp ⇒ fresh state) and reuses it for the commit. NextFileSeq deleted (unused after glob removal).

grep gate: `grep -n 'directory_iterator' native/src/aligned_writer.cc native/src/writer.cc` → 0

## notes

- LoadAlignedResume takes `iceberg::Namespace` already (07 threading arrived early for this function); generate passes `{"primeparts"}` at the root.
- Semantics change vs glob: resume state now reflects only COMMITTED files (manifest truth); uncommitted staging debris no longer influences next_seq — writer filename tokens already prevent collisions.
