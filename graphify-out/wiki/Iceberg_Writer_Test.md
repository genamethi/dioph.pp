# Iceberg Writer Test

> 11 nodes · cohesion 0.27

## Key Concepts

- **test_iceberg_writer.cc** (8 connections) — `native/tests/test_iceberg_writer.cc`
- **MakePrimesBatch()** (6 connections) — `native/tests/test_iceberg_writer.cc`
- **FinishOrDie()** (5 connections) — `native/tests/test_iceberg_writer.cc`
- **main()** (5 connections) — `native/tests/test_iceberg_writer.cc`
- **Check()** (3 connections) — `native/tests/test_iceberg_writer.cc`
- **shared_ptr** (2 connections)
- **string** (2 connections)
- **Array** (1 connections)
- **RecordBatch** (1 connections)
- **Schema** (1 connections)
- **T** (1 connections)

## Relationships

- [Query/Preset Headers](Query-Preset_Headers.md) (2 shared connections)
- [Query Literal Bounds](Query_Literal_Bounds.md) (1 shared connections)
- [Writer Config + Partition Spec](Writer_Config_%2B_Partition_Spec.md) (1 shared connections)
- [Written File Metadata](Written_File_Metadata.md) (1 shared connections)

## Source Files

- `native/tests/test_iceberg_writer.cc`

## Audit Trail

- EXTRACTED: 19 (95%)
- INFERRED: 1 (5%)
- AMBIGUOUS: 0 (0%)

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*