# 07_namespace

deps: none | status: todo

- [ ] `iceberg::Namespace` formed at composition roots (default "primeparts", rest_uri-style resolution), passed through: `pp_iceberg_rest.{h,cc}` (`TableMetadataPath`, `EnsureTable`, `DropTable`, `PublishTable`, `CommitFiles`, `MoveStagedFilesInto`; `warehouse/"primeparts"/...` paths derive from ns), `CommitFilesAtomic`/`AssembleTransactionBody`, `QueryService::Open`, `verify_main`, `generate`
- [ ] delete `query_service.cc:39 kNs`, Namespace literals in `verify_main.cc:76` + pp_iceberg_rest.cc
- [ ] build green

grep gate: `grep -rn '"primeparts"' native/src --include='*.cc' | grep -v smoke | grep -v config.cc | grep -v main` → 0

## notes
