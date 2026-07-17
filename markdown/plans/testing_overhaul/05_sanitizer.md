# 05 sanitizer build

deps: 04 | status: todo

`./configure --sanitize` → ASAN/UBSAN over our code + suite only. Partial coverage accepted.

- [ ] `native/configure`: `--sanitize` flag writes `SANITIZE := -fsanitize=address,undefined
      -fno-omit-frame-pointer` into `config.mk` (absent by default).
- [ ] Makefile: apply `$(SANITIZE)` to compile + link of the test and e2e binaries (both).
- [ ] document mixed-boundary caveat (e.g. `ASAN_OPTIONS=detect_container_overflow=0`) where our
      instrumented code crosses into uninstrumented vendored static libs.
- [ ] boundary: `./configure --sanitize && make test` builds + runs; record residual noise.
