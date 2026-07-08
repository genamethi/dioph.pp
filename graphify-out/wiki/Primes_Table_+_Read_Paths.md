# Primes Table + Read Paths

> 7 nodes · cohesion 0.29

## Key Concepts

- **primeparts.primes table (p, k, prime_rank)** (6 connections) — `markdown/data_eng/iceberg_data_setup.md`
- **Two read paths (k-scan early-stop vs direct-parquet point lookup)** (3 connections) — `markdown/arch/tui_query_design.md`
- **p-scan window as predicate pushdown on p** (2 connections) — `markdown/arch/tui_app_design.md`
- **Future sparse prime_rank->(file,rg,offset) index** (2 connections) — `markdown/arch/tui_query_design.md`
- **prime_rank = pi(p) semantics** (2 connections) — `markdown/data_eng/iceberg_data_setup.md`
- **identity bucket partitioning (p_bucket_version, p_bucket)** (1 connections) — `markdown/data_eng/iceberg_data_setup.md`
- **primeparts.primes_k0 derived base table + MOR sieve clone** (1 connections) — `markdown/data_eng/iceberg_data_setup.md`

## Relationships

- [Parquet Data File Writer](Parquet_Data_File_Writer.md) (1 shared connections)

## Source Files

- `markdown/arch/tui_app_design.md`
- `markdown/arch/tui_query_design.md`
- `markdown/data_eng/iceberg_data_setup.md`

## Audit Trail

- EXTRACTED: 5 (71%)
- INFERRED: 2 (29%)
- AMBIGUOUS: 0 (0%)

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*