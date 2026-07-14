# 05_resume

deps: 01 | status: todo

- [ ] `aligned_writer.h:LoadAlignedResume` — new signature `(catalog, ns, tables, reference_table, BucketFields{version_field,bucket_field}, bucket_version, ResumeState*, error)`; caller declares bucket-scheme partition fields
- [ ] impl: LoadTable → current snapshot → ManifestListReader/ManifestReader (pattern: `pp_catalogd.cc:FieldUpperBound`, `source_scan.cc`) → live entries' partition tuples; field positions from table's partition spec by declared name
- [ ] frontier = max bucket at version; fill = Σ file_size_in_bytes in frontier bucket (reference table); next_seq = 1 + max trailing seq from committed basenames (extract basename-seq parse from `NextFileSeq`; delete glob variant if unused)
- [ ] no snapshot → bucket 0 / fill 0 / seq 0; unparseable tuple / missing spec field → hard error, never glob
- [ ] delete fs-glob impl (`aligned_writer.cc:260-301`) + false header claim
- [ ] `generate.cc` — hoist `OpenCatalog` before writer construction, pass catalog; `--temp` = fresh state, no catalog
- [ ] build green

grep gate: `grep -n 'directory_iterator' native/src/aligned_writer.cc` → 0

## notes
