# Parquet Writer Impl

> 16 nodes · cohesion 0.12

## Key Concepts

- **BucketParquetWriter::Impl** (26 connections) — `native/src/writer.cc`
- **WriterProperties** (2 connections)
- **FileOutputStream** (1 connections)
- **FileWriter** (1 connections)
- **arrow_schema** (1 connections) — `native/src/writer.cc`
- **closed** (1 connections) — `native/src/writer.cc`
- **config** (1 connections) — `native/src/writer.cc`
- **current_record** (1 connections) — `native/src/writer.cc`
- **done** (1 connections) — `native/src/writer.cc`
- **final_path** (1 connections) — `native/src/writer.cc`
- **next_seq** (1 connections) — `native/src/writer.cc`
- **partition_spec** (1 connections) — `native/src/writer.cc`
- **props** (1 connections) — `native/src/writer.cc`
- **sink** (1 connections) — `native/src/writer.cc`
- **tmp_path** (1 connections) — `native/src/writer.cc`
- **writer** (1 connections) — `native/src/writer.cc`

## Relationships

- [Parquet Data File Writer](Parquet_Data_File_Writer.md) (7 shared connections)
- [Writer Config + Partition Spec](Writer_Config_%2B_Partition_Spec.md) (1 shared connections)
- [Written File Metadata](Written_File_Metadata.md) (1 shared connections)
- [Query Literal Bounds](Query_Literal_Bounds.md) (1 shared connections)

## Source Files

- `native/src/writer.cc`

## Audit Trail

- EXTRACTED: 25 (100%)
- INFERRED: 0 (0%)
- AMBIGUOUS: 0 (0%)

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*