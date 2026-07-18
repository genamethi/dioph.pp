# 05 sanitizer build

deps: 04 | status: done

`./configure --sanitize` → ASAN/UBSAN over our code + suite only. Partial coverage accepted.
`SANITIZE` appended to CFLAGS/CXXFLAGS/LDFLAGS, so every object WE compile instruments (all binaries,
not just test/e2e); vendored static arrow/iceberg do not. config.mk is gitignored — the flag never
lands in a commit. Verified: `make test` (16) and `make e2e` (6) both green under ASAN/UBSAN with no
sanitizer reports and no leak reports; no `ASAN_OPTIONS` override needed in practice. If the
uninstrumented-vendor boundary later yields false positives (e.g. container-overflow), set
`ASAN_OPTIONS=detect_container_overflow=0`.

- [x] `native/configure`: `--sanitize` flag writes `SANITIZE := -fsanitize=address,undefined
      -fno-omit-frame-pointer` into `config.mk` (absent by default).
- [x] Makefile: apply `$(SANITIZE)` to compile + link of the test and e2e binaries (both).
- [x] document mixed-boundary caveat (e.g. `ASAN_OPTIONS=detect_container_overflow=0`) where our
      instrumented code crosses into uninstrumented vendored static libs.
- [x] boundary: `./configure --sanitize && make test` builds + runs; record residual noise.
