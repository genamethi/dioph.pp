# 03 scrap smokes

deps: 02 | status: todo

Delete all smokes; deleted capability is deleted (no port).

- [ ] delete sources: `src/generate_smoke.cc`, `src/query/query_service_smoke.cc`,
      `src/query/lua_query_smoke.cc`, `src/tui/lua_presets_smoke.cc`, `src/catalog/pp_lmdb_smoke.cc`.
- [ ] remove Makefile `.PHONY: smoke` target, all smoke objects/recipes, and smoke binary rules
      (`primeparts-generate-smoke`, `query-service-smoke`, `primeparts-lmdb-smoke`,
      `primeparts-lua-{presets,query}-smoke`). Leave `lmdb-tools` (not a smoke).
- [ ] log removed coverage as a holes line (07).
- [ ] boundary: `make all test` green; no smoke targets remain.
