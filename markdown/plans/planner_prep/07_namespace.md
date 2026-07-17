# 07_namespace

deps: none | status: done

done: `iceberg::Namespace` formed at composition roots via `catalog::ResolveNamespace(name)` (explicit → `PRIMEPARTS_NAMESPACE` env → `kDefaultNamespace` "primeparts") and threaded as an argument through pp_iceberg_rest.{h,cc} (`TableMetadataPath`, `EnsureTable`, `DropTable`, `CommitFiles`, `MoveStagedFilesInto`, `FetchFieldUpperBound`, `StagingDataDir`; `warehouse/"primeparts"/...` paths now derive from ns), `CommitFilesAtomic`/`AssembleTransactionBody`, `AlignedBucketWriter::Make`, `MaterializeIntColumns`, `QueryService::Open` (Impl stores ns for all LoadTable/ListTables sites), verify_main (`--namespace`/`-N`, RunTable takes ns), generate (`--namespace` flag → env → config.lua `namespace` → default; `pp_gen_options.ns`), pp (`--namespace`), TUI (`App::ns`, config.lua `namespace` key, `reopen_warehouse`). Deleted: `query_service.cc kNs`, Namespace literals in verify_main.cc + pp_iceberg_rest.cc. Build green.

grep gate: `grep -rn '"primeparts"' native/src --include='*.cc' | grep -v smoke | grep -v config.cc | grep -v main` → 0

## notes

- `PublishTable` (listed in the original checkbox) no longer exists — deleted by 06_declare; nothing to thread.
- ns threaded where the checkbox didn't list it but the same literal lived: `FetchFieldUpperBound` (ns → URL path, levels joined with %1F per IRC), `StagingDataDir` (staging tree now namespaced ⇒ `AlignedBucketWriter::Make` and `MaterializeIntColumns` take ns).
- SqlCatalog/LMDB store name is a catalog name, not a namespace: kept as new `kCatalogName` constant ("primeparts", pp_iceberg_rest.h) rather than folding into ns.
- Per zero-comments feedback, comments stripped from every file touched (incl. tui_app.h/tui_main.cc doc blocks; TUI design context lives in markdown/tui/).
- `make all` green; grep gate → 0. `make smoke` fails in generate-commit-smoke, but pre-existing: 916563a (tui-query) added `kMinCount = 1e9` while generate_smoke.cc still passes `--count 1000` — broken since before planner-prep, not a 07 regression. Filed in 00 holes registry.
