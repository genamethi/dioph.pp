# 01 gtest harness wiring

deps: 00 | status: todo

Wire the gtest suite against arrow's bundled gtest. No new dep installed.

- [ ] `config.mk`: `GTEST_CPPFLAGS := -I$(PREFIX)/include/arrow-gtest`;
      `GTEST_LDLIBS := -larrow_gtest_main -larrow_gmock` (rpath already via existing `-Wl,-rpath,$(PREFIX)/lib`).
- [ ] `native/configure`: pin `-DARROW_TESTING=ON` in the arrow build block; after the arrow step,
      loud-check `$PREFIX/include/arrow-gtest/gtest/gtest.h` exists — error, no silent continue.
- [ ] `native/tests/`: gtest suite layout; suite binary links `$(GTEST_LDLIBS)`. `arrow_gtest_main`
      supplies `main()` — no test main authored.
- [ ] Makefile: rebuild `.PHONY: test` around the gtest binary (build + run).
- [ ] boundary: trivial `TEST(Smoke, Builds)` compiles + runs green.
