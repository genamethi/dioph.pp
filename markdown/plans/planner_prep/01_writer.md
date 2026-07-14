# 01_writer

deps: none | status: in-progress

- [ ] `writer.h:WriterConfig` — add `struct StatColumn { std::string name; bool sorted; }`, `std::vector<StatColumn> stat_columns`; `Make` resolves each to {field_id, iceberg type, arrow index}; unknown name / non-int type → error
- [ ] `writer.h:BucketParquetWriter::Write(batch, error)` — BatchStats param deleted; writer computes bounds (sorted ⇒ first/last, else min/max scan) as `iceberg::Literal` typed by schema field (kInt→Int, kLong→Long)
- [ ] `writer.h:WrittenFile` — `{p_min,p_max,rank_min,rank_max}` → `std::map<int32_t, std::pair<iceberg::Literal,iceberg::Literal>> bounds`
- [ ] `writer.cc:BuildDataFile` — serialize bounds for declared stat columns only; delete `FieldIdByName("p"/"prime_rank")` blocks
- [ ] `writer.cc:122` — delete `(bucket_version,bucket)` partition-tuple fallback; non-empty spec requires explicit `partition_values`; bucket_version/bucket stay filename-only
- [ ] `aligned_writer.h:BoundTable` — add `std::vector<WriterConfig::StatColumn> stat_columns`, forward into per-bucket WriterConfig
- [ ] `aligned_writer.cc:OpenBucketWriters` — build `PartitionValues{Int(policy.bucket_version), Int(bucket)}` per bucket
- [ ] `aligned_writer.cc:WriteSlice` — delete stat computation + `Int64Col(batch,"prime_rank")`
- [ ] `generate.cc` — declare primes `{p sorted, prime_rank sorted}`, partitions `{p sorted, prime_rank sorted, q_k unsorted}`
- [ ] `materialize.cc` — no stat columns; `BatchStats{}` zeros hack gone
- [ ] `tests/test_iceberg_writer.cc` — int32 stat column round-trips typed bounds; unknown stat column errors at Make; partitioned spec w/o partition_values errors
- [ ] build green: `make -C native all test`

grep gate: `grep -rn 'BatchStats\|p_min\|rank_min' native/src native/include` → 0 hits outside this file's notes

## notes
