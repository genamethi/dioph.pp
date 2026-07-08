# Generate CLI Args

> 14 nodes · cohesion 0.23

## Key Concepts

- **generate.cc** (45 connections) — `native/src/generate.cc`
- **parse_args()** (7 connections) — `native/src/generate.cc`
- **default_temp_root()** (5 connections) — `native/src/generate.cc`
- **resolve_bucket_state()** (5 connections) — `native/src/generate.cc`
- **path** (4 connections)
- **parse_i64()** (4 connections) — `native/src/generate.cc`
- **deque** (3 connections)
- **parse_i32()** (3 connections) — `native/src/generate.cc`
- **usage()** (3 connections) — `native/src/generate.cc`
- **utc_timestamp_compact()** (3 connections) — `native/src/generate.cc`
- **pp_gen_last_error()** (2 connections) — `native/src/generate.cc`
- **generate.h** (1 connections) — `native/include/primeparts/generate.h`
- **FILE** (1 connections)
- **gen_stdout_log()** (1 connections) — `native/src/generate.cc`

## Relationships

- [Generation Pipeline Core](Generation_Pipeline_Core.md) (10 shared connections)
- [Arrow Array Builders](Arrow_Array_Builders.md) (7 shared connections)
- [Batch Group Writer](Batch_Group_Writer.md) (4 shared connections)
- [Generation Bucket Options](Generation_Bucket_Options.md) (3 shared connections)
- [Query/Preset Headers](Query-Preset_Headers.md) (2 shared connections)
- [Query Service Validation](Query_Service_Validation.md) (2 shared connections)
- [Stop Monitor / Keyboard](Stop_Monitor_-_Keyboard.md) (2 shared connections)
- [Prime Generator + Lua Core](Prime_Generator_%2B_Lua_Core.md) (1 shared connections)
- [Covering Sieve](Covering_Sieve.md) (1 shared connections)
- [Source Table Reader](Source_Table_Reader.md) (1 shared connections)
- [Progress Counter](Progress_Counter.md) (1 shared connections)
- [Iceberg REST Catalog](Iceberg_REST_Catalog.md) (1 shared connections)

## Source Files

- `native/include/primeparts/generate.h`
- `native/src/generate.cc`

## Audit Trail

- EXTRACTED: 58 (100%)
- INFERRED: 0 (0%)
- AMBIGUOUS: 0 (0%)

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*