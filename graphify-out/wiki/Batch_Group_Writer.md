# Batch Group Writer

> 13 nodes · cohesion 0.17

## Key Concepts

- **write_group_table()** (12 connections) — `native/src/generate.cc`
- **FileGroup** (11 connections) — `native/src/generate.cc`
- **BatchHolder** (7 connections) — `native/src/generate.cc`
- **Schema** (2 connections)
- **vector** (2 connections)
- **batch** (1 connections) — `native/src/generate.cc`
- **batches** (1 connections) — `native/src/generate.cc`
- **first_p** (1 connections) — `native/src/generate.cc`
- **last_p** (1 connections) — `native/src/generate.cc`
- **partitions_rows** (1 connections) — `native/src/generate.cc`
- **prime_rows** (1 connections) — `native/src/generate.cc`
- **processed_count** (1 connections) — `native/src/generate.cc`
- **start_idx** (1 connections) — `native/src/generate.cc`

## Relationships

- [Arrow Array Builders](Arrow_Array_Builders.md) (4 shared connections)
- [Prime Generator + Lua Core](Prime_Generator_%2B_Lua_Core.md) (1 shared connections)
- [Bucket Writer Facade](Bucket_Writer_Facade.md) (1 shared connections)
- [Written File Metadata](Written_File_Metadata.md) (1 shared connections)
- [Generate CLI Args](Generate_CLI_Args.md) (1 shared connections)
- [Generation Pipeline Core](Generation_Pipeline_Core.md) (1 shared connections)

## Source Files

- `native/src/generate.cc`

## Audit Trail

- EXTRACTED: 22 (100%)
- INFERRED: 0 (0%)
- AMBIGUOUS: 0 (0%)

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*