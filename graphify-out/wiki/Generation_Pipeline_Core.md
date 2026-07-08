# Generation Pipeline Core

> 15 nodes · cohesion 0.24

## Key Concepts

- **run_generation()** (26 connections) — `native/src/generate.cc`
- **string** (9 connections)
- **materialize_group()** (7 connections) — `native/src/generate.cc`
- **pp_gen_run()** (7 connections) — `native/src/generate.cc`
- **append_manifest_file()** (6 connections) — `native/src/generate.cc`
- **main()** (6 connections) — `native/src/generate.cc`
- **json_escape()** (5 connections) — `native/src/generate.cc`
- **set_last_error()** (4 connections) — `native/src/generate.cc`
- **append_manifest_boundary()** (3 connections) — `native/src/generate.cc`
- **log_line()** (3 connections) — `native/src/generate.cc`
- **pp_gen_callbacks** (3 connections)
- **ofstream** (2 connections)
- **pp_gen_result** (2 connections)
- **string_view** (1 connections)
- **pp_gen_options** (1 connections)

## Relationships

- [Prime Generator + Lua Core](Prime_Generator_%2B_Lua_Core.md) (5 shared connections)
- [Generate CLI Args](Generate_CLI_Args.md) (4 shared connections)
- [Progress Counter](Progress_Counter.md) (3 shared connections)
- [Iceberg Schemas + Mersenne](Iceberg_Schemas_%2B_Mersenne.md) (3 shared connections)
- [Batch Group Writer](Batch_Group_Writer.md) (2 shared connections)
- [Generation Bucket Options](Generation_Bucket_Options.md) (2 shared connections)
- [Written File Metadata](Written_File_Metadata.md) (1 shared connections)
- [mdiff Table Builder](mdiff_Table_Builder.md) (1 shared connections)
- [Stop Monitor / Keyboard](Stop_Monitor_-_Keyboard.md) (1 shared connections)
- [Parquet Data File Writer](Parquet_Data_File_Writer.md) (1 shared connections)

## Source Files

- `native/src/generate.cc`

## Audit Trail

- EXTRACTED: 38 (79%)
- INFERRED: 10 (21%)
- AMBIGUOUS: 0 (0%)

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*