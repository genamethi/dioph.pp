# Parquet Aggregation Pass

> 14 nodes · cohesion 0.15

## Key Concepts

- **ProcessFile()** (16 connections) — `native/src/coverings/primitive_factors_main.cc`
- **FlushAggregates()** (12 connections) — `native/src/coverings/primitive_factors_main.cc`
- **OpenParquetFile()** (5 connections) — `native/src/coverings/primitive_factors_main.cc`
- **ResolveColumnIndices()** (5 connections) — `native/src/coverings/primitive_factors_main.cc`
- **AggregateEntryCount()** (3 connections) — `native/src/coverings/primitive_factors_main.cc`
- **atomic** (3 connections)
- **ClearAggregates()** (3 connections) — `native/src/coverings/primitive_factors_main.cc`
- **HasAggregateEntries()** (3 connections) — `native/src/coverings/primitive_factors_main.cc`
- **PaddedSeq()** (3 connections) — `native/src/coverings/primitive_factors_main.cc`
- **vector** (2 connections)
- **metadata** (2 connections) — `native/src/coverings/primitive_factors_main.cc`
- **FileReader** (1 connections)
- **unique_ptr** (1 connections)
- **SchemaDescriptor** (1 connections)

## Relationships

- [Factor Frequency Outputs](Factor_Frequency_Outputs.md) (7 shared connections)
- [Prime Factor Stats](Prime_Factor_Stats.md) (6 shared connections)
- [Aggregation Options](Aggregation_Options.md) (2 shared connections)
- [Progress Rendering](Progress_Rendering.md) (2 shared connections)
- [Mersenne Primitive Factors](Mersenne_Primitive_Factors.md) (1 shared connections)
- [Source Table Reader](Source_Table_Reader.md) (1 shared connections)
- [IntQ Factor Counting](IntQ_Factor_Counting.md) (1 shared connections)

## Source Files

- `native/src/coverings/primitive_factors_main.cc`

## Audit Trail

- EXTRACTED: 34 (100%)
- INFERRED: 0 (0%)
- AMBIGUOUS: 0 (0%)

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*