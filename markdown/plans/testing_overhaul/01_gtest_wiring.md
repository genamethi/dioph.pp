# 01 gtest harness wiring

deps: 00 | status: done

Wire the gtest suite against arrow's bundled gtest. No new dep installed.

- [x] `config.mk`: `GTEST_CPPFLAGS := -I$(PREFIX)/include/arrow-gtest`;
      `GTEST_LDLIBS := -larrow_gtest_main -larrow_gmock -larrow_gtest` (core lib needed for
      `testing::Test` typeinfo; rpath already via existing `-Wl,-rpath,$(PREFIX)/lib`).
- [x] `native/configure`: pin `-DARROW_TESTING=ON` in the arrow build block; after the arrow step,
      loud-check `$PREFIX/include/arrow-gtest/gtest/gtest.h` exists — error, no silent continue.
- [x] `native/tests/`: gtest suite layout; suite binary links `$(GTEST_LDLIBS)`. `arrow_gtest_main`
      supplies `main()` — no test main authored.
- [x] Makefile: rebuild `.PHONY: test` around the gtest binary (build + run).
- [x] boundary: trivial `TEST(Smoke, Builds)` compiles + runs green.
