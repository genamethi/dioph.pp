# Parquet Data File Writer

> 23 nodes · cohesion 0.19

## Key Concepts

- **writer.cc** (24 connections) — `native/src/writer.cc`
- **BuildDataFile()** (10 connections) — `native/src/writer.cc`
- **string** (9 connections)
- **BucketParquetWriter::Make()** (8 connections) — `native/src/writer.cc`
- **CloseCurrent** (7 connections) — `native/src/writer.cc`
- **BucketParquetWriter::Write()** (6 connections) — `native/src/writer.cc`
- **IcebergToArrowSchemaWithFieldIds()** (6 connections) — `native/src/writer.cc`
- **ParquetWriterProperties()** (6 connections) — `native/src/writer.cc`
- **BucketParquetWriter::Close()** (5 connections) — `native/src/writer.cc`
- **OpenIfNeeded** (5 connections) — `native/src/writer.cc`
- **Schema** (5 connections)
- **FilePathFor()** (5 connections) — `native/src/writer.cc`
- **NextFileSeq()** (5 connections) — `native/src/writer.cc`
- **shared_ptr** (4 connections)
- **FieldIdByName()** (4 connections) — `native/src/writer.cc`
- **BucketParquetWriter::BucketParquetWriter()** (3 connections) — `native/src/writer.cc`
- **PartitionSpec** (3 connections)
- **path** (3 connections)
- **string_view** (3 connections)
- **unique_ptr** (3 connections)
- **FileToken()** (2 connections) — `native/src/writer.cc`
- **BatchStats** (1 connections)
- **RecordBatch** (1 connections)

## Relationships

- [Query Literal Bounds](Query_Literal_Bounds.md) (4 shared connections)
- [Writer Config + Partition Spec](Writer_Config_%2B_Partition_Spec.md) (4 shared connections)
- [Parquet Writer Impl](Parquet_Writer_Impl.md) (2 shared connections)
- [Iceberg Schemas + Mersenne](Iceberg_Schemas_%2B_Mersenne.md) (2 shared connections)
- [Written File Metadata](Written_File_Metadata.md) (2 shared connections)
- [Query/Preset Headers](Query-Preset_Headers.md) (1 shared connections)
- [Query Service Validation](Query_Service_Validation.md) (1 shared connections)
- [Row Delta / Deletes](Row_Delta_-_Deletes.md) (1 shared connections)
- [Bucket Writer Facade](Bucket_Writer_Facade.md) (1 shared connections)
- [mdiff Table Builder](mdiff_Table_Builder.md) (1 shared connections)

## Source Files

- `native/src/writer.cc`

## Audit Trail

- EXTRACTED: 67 (99%)
- INFERRED: 1 (1%)
- AMBIGUOUS: 0 (0%)

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*