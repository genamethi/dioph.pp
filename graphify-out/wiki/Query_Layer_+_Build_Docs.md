# Query Layer + Build Docs

> 6 nodes · cohesion 0.33

## Key Concepts

- **QueryService (LookupPrime/LookupPartitions/ScanByK/GroupCount)** (6 connections) — `markdown/arch/tui_query_design.md`
- **TUI data-query layer design (QueryService, two read paths)** (4 connections) — `markdown/arch/tui_query_design.md`
- **Native build & rootless provisioning (BUILD.md)** (3 connections) — `BUILD.md`
- **Native binaries inventory (generate, sieve, catalogd, tui, pp)** (3 connections) — `native/README.md`
- **native/configure provisioning script** (2 connections) — `BUILD.md`
- **TUI data-query layer design (tui copy)** (1 connections) — `markdown/tui/tui_query_design.md`

## Relationships

- [Covering Filter + NT Core](Covering_Filter_%2B_NT_Core.md) (1 shared connections)
- [Primes Table + Read Paths](Primes_Table_%2B_Read_Paths.md) (1 shared connections)
- [Query Service Validation](Query_Service_Validation.md) (1 shared connections)
- [Source Table Reader](Source_Table_Reader.md) (1 shared connections)

## Source Files

- `BUILD.md`
- `markdown/arch/tui_query_design.md`
- `markdown/tui/tui_query_design.md`
- `native/README.md`

## Audit Trail

- EXTRACTED: 7 (78%)
- INFERRED: 2 (22%)
- AMBIGUOUS: 0 (0%)

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*