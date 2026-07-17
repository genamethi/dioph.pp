# 01_writer

deps: none | status: done

done: WriterConfig.stat_columns (StatColumn{name,sorted}) resolved at Make; Write(batch,error) computes typed Literal bounds (kInt→Int, kLong→Long); WrittenFile.bounds map replaces p_min/p_max/rank_min/rank_max; BuildDataFile serializes declared bounds only; partition-tuple fallback deleted; AlignedBucketWriter builds per-bucket PartitionValues + forwards BoundTable.stat_columns; generate.cc declares stats for both tables; materialize.cc BatchStats hack gone; test_iceberg_writer covers int32 typed bounds + Make error cases and runs under `make test`.

grep gate: `grep -rn 'BatchStats\|p_min\|rank_min' native/src native/include native/tests | grep -v source_scan` → 0 (source_scan's SourceFileInfo.p_min dies in 03)

## notes

- WriterConfig.partition_spec is now REQUIRED (null → Make error); the implicit BucketPartitionSpec default was the same assumption family. writer.cc no longer includes schemas.h.
- pp_commit_smoke.cc + pp_catalogd_smoke.cc DELETED (user: smokes go, not ported); Makefile smoke target now lmdb/generate/lua-presets/lua-query only.
- Comments stripped from writer.{h,cc}, aligned_writer.{h,cc} per zero-comments rule (strip on touch).
