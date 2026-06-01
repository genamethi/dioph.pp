# primeparts/catalog

Catalog management for primeparts native tools, split by the two distinct
backends a native commit has to talk to.

| File | Role |
|---|---|
| `pp_iceberg_rest.{h,cc}` | iceberg REST (IRC) catalog: `MakeCatalog`, `EnsureNamespace`, `PublishTable` (register-or-create+FastAppend), `LatestMetadataJson`, `LocalIO`. Consolidates logic that was duplicated across `drop_bucket_cols_main.cc`, `backfill_prime_rank_main.cc`, and `covering_sieve_main.cc`. |
| `pp_hive_sync.{h,cc}` | Hive engine sync. Deliberately thin: there is no iceberg-cpp API for making the Hive engine adopt an externally-committed snapshot — the mechanism is `ALTER TABLE ... SET TBLPROPERTIES('metadata_location'=...)` over beeline, which is a subprocess to the JVM. So this forks `scripts/hive_register.sh` rather than reimplementing a JDBC client in C++. |
| `pp_catalog_main.cc` | `pp-catalog` binary. Wraps both halves; primary entry point is `--smoke-test`. |

## Why the split, not a `pp-hive.cc` C++ catalog

The IRC side has real iceberg-cpp API surface worth unifying in C++. The Hive
side is inherently "drive the JVM": building a classpath and exec'ing beeline.
A C++ `pp-hive.cc` would just be a `fork/exec` wrapper around the already-working
`scripts/hive_register.sh`, so we call the script as a subprocess instead.

## Two backends, two visibility steps

A native IRC commit (`PublishTable`) writes the HMS *table row* and is enough
for IRC clients. It is **not** enough for the running Hive *engine* to adopt a
*snapshot-advancing* change — that needs the separate Hive sync. Verified
2026-05-31; see `HANDOFF.md` "HMS sync is a separate, required step" and memory
`project-hms-sync-beeline-verified`.

## Smoke test

```
pp-catalog --smoke-test [--rest-uri URI] [--warehouse DIR] [--keep]
```

Run after cluster config changes or image rebuilds. It:

1. checks beeline connectivity (`SELECT 1`);
2. creates a uniquely-named throwaway `primeparts.zz_ppcatalog_smoke_<ts>` with
   3 rows (via beeline DDL);
3. loads it through the IRC `RestCatalog` (`pp_iceberg_rest`);
4. runs the Hive-sync round-trip `3 → (sync to empty 00000) → 0 →
   (sync to current) → 3` via `hive_register.sh` as a subprocess;
5. drops the throwaway (unless `--keep`).

Exits non-zero on any failure. The metadata-file scan skips Hadoop `.crc`
sidecars and other dot-files (a real bug caught on first run).

Other subcommands: `--hive-exec "SQL"`, `--hive-sync DB.TABLE METADATA_URI`.

## Build prerequisites (host)

The beeline subprocess (`scripts/hive_register.sh`) needs **Java 21** and the
locally-built Hive 4.2 dist; see that script's header for the exact classpath
and jline/transport requirements.
