PREFIX ?= /usr/local
PKG_CONFIG ?= /usr/bin/pkg-config
PKG_CONFIG_PATH ?= $(PREFIX)/lib/pkgconfig:/usr/lib/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig
CC ?= /usr/bin/gcc
CXX ?= /usr/bin/g++
ICEBERG_CPP_BUILD ?= vendor/iceberg-cpp-build
ICEBERG_CPP_INSTALL ?= vendor/iceberg-cpp-install
ICEBERG_CPP_CMAKE_ARGS ?= \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_C_COMPILER=$(CC) \
	-DCMAKE_CXX_COMPILER=$(CXX) \
	-DICEBERG_BUILD_TESTS=OFF \
	-DICEBERG_BUILD_REST=OFF \
	-DICEBERG_BUILD_REST_INTEGRATION_TESTS=OFF \
	-DICEBERG_BUILD_STATIC=ON \
	-DICEBERG_BUILD_SHARED=ON \
	-DICEBERG_BUILD_BUNDLE=ON \
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	-DFETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER \
	-DARROW_WITH_SNAPPY=ON \
	-DARROW_WITH_ZSTD=ON \
	-DCMAKE_INSTALL_PREFIX=$(abspath $(ICEBERG_CPP_INSTALL))

CPPFLAGS += -Iinclude -I$(PREFIX)/include
CFLAGS += -std=c11 -D_POSIX_C_SOURCE=200809L -O3 -g -fPIC -Wall -Wextra -Wpedantic
CXXFLAGS += -std=c++23 -O3 -g -fPIC -Wall -Wextra -Wpedantic
LDFLAGS += -L$(PREFIX)/lib -Wl,-rpath,$(abspath $(PREFIX)/lib)
EXE_LDFLAGS += -no-pie
LDLIBS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(PKG_CONFIG) --libs primecount primesieve gmp flint) -lpari -lm
NOTCURSES_LDLIBS ?= $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(PKG_CONFIG) --libs notcurses-core 2>/dev/null || PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(PKG_CONFIG) --libs notcurses 2>/dev/null)
NOTCURSES_CPPFLAGS ?= $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(PKG_CONFIG) --cflags notcurses-core 2>/dev/null || PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(PKG_CONFIG) --cflags notcurses 2>/dev/null)
NOTCURSES_RPATH ?= -Wl,-rpath,/usr/local/lib
ICEBERG_CPPFLAGS += -I$(ICEBERG_CPP_INSTALL)/include -I$(ICEBERG_CPP_BUILD)/_deps/nanoarrow-src/src
ICEBERG_LDFLAGS += -L$(ICEBERG_CPP_BUILD)/src/iceberg -L$(ICEBERG_CPP_BUILD)/_deps/nanoarrow-build \
	-Wl,-rpath,$(abspath $(ICEBERG_CPP_BUILD)/src/iceberg) \
	-Wl,-rpath,$(abspath $(ICEBERG_CPP_BUILD)/_deps/nanoarrow-build)
ICEBERG_LDLIBS += -liceberg_bundle -liceberg_vendored_avrocpp -liceberg -lnanoarrow_shared -lzstd -lsnappy -lz

BUILD_DIR := build
CORE_OBJ := $(BUILD_DIR)/core.o
BENCH_CORE_OBJ := $(BUILD_DIR)/bench_core.o
BENCH_MATERIALIZE_OBJ := $(BUILD_DIR)/bench_materialize.o
GENERATE_OBJ := $(BUILD_DIR)/generate.o
GENERATE_ABI_OBJ := $(BUILD_DIR)/generate_abi.o
UI_ICEBERG_OBJ := $(BUILD_DIR)/ui_iceberg.o
TEST_OBJ := $(BUILD_DIR)/test_core.o
TEST_UI_ICEBERG_OBJ := $(BUILD_DIR)/test_ui_iceberg.o
TUI_FRONTEND_OBJ := $(BUILD_DIR)/tui_frontend.o

.PHONY: all clean test iceberg-cpp-rebuild

all: $(BUILD_DIR)/libprimeparts_core.so \
     $(BUILD_DIR)/primeparts-bench-core \
     $(BUILD_DIR)/primeparts-bench-materialize \
     $(BUILD_DIR)/primeparts-generate \
     $(BUILD_DIR)/primeparts-test-ui-iceberg \
     $(BUILD_DIR)/nth_prime_boundaries

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(CORE_OBJ): src/core.c include/primeparts/core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BENCH_CORE_OBJ): src/bench.c include/primeparts/core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BENCH_MATERIALIZE_OBJ): src/materialize_bench.c include/primeparts/core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(GENERATE_OBJ): src/generate.cc include/primeparts/core.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ICEBERG_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(GENERATE_ABI_OBJ): src/generate.cc include/primeparts/core.h include/primeparts/generate.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ICEBERG_CPPFLAGS) $(CXXFLAGS) -DPRIMEPARTS_GENERATE_NO_MAIN -c $< -o $@

$(UI_ICEBERG_OBJ): src/ui_iceberg.cc include/primeparts/ui_iceberg.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ICEBERG_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(TEST_OBJ): tests/test_core.c include/primeparts/core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(TEST_UI_ICEBERG_OBJ): tests/test_ui_iceberg.c include/primeparts/ui_iceberg.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(TUI_FRONTEND_OBJ): src/tui_frontend.c include/primeparts/ui_iceberg.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NOTCURSES_CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/libprimeparts_core.so: $(CORE_OBJ)
	$(CC) -shared $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/primeparts-bench-core: $(CORE_OBJ) $(BENCH_CORE_OBJ)
	$(CC) $(LDFLAGS) $(EXE_LDFLAGS) $^ $(LDLIBS) -pthread -o $@

$(BUILD_DIR)/primeparts-bench-materialize: $(CORE_OBJ) $(BENCH_MATERIALIZE_OBJ)
	$(CC) $(LDFLAGS) $(EXE_LDFLAGS) $^ $(LDLIBS) -pthread -o $@

$(BUILD_DIR)/primeparts-generate: $(CORE_OBJ) $(GENERATE_OBJ)
	$(CXX) $(ICEBERG_LDFLAGS) $(LDFLAGS) $(EXE_LDFLAGS) $^ $(LDLIBS) $(ICEBERG_LDLIBS) -pthread -o $@

$(BUILD_DIR)/primeparts-test-ui-iceberg: $(UI_ICEBERG_OBJ) $(TEST_UI_ICEBERG_OBJ)
	$(CXX) $(ICEBERG_LDFLAGS) $(LDFLAGS) $(EXE_LDFLAGS) $^ $(ICEBERG_LDLIBS) -lsqlite3 -o $@

$(BUILD_DIR)/primeparts-tui: $(CORE_OBJ) $(GENERATE_ABI_OBJ) $(UI_ICEBERG_OBJ) $(TUI_FRONTEND_OBJ)
	$(CXX) $(ICEBERG_LDFLAGS) $(LDFLAGS) $(EXE_LDFLAGS) $(NOTCURSES_RPATH) $^ $(LDLIBS) $(ICEBERG_LDLIBS) $(NOTCURSES_LDLIBS) -lsqlite3 -pthread -o $@

$(BUILD_DIR)/test_core: $(CORE_OBJ) $(TEST_OBJ)
	$(CC) $(LDFLAGS) $(EXE_LDFLAGS) $^ $(LDLIBS) -pthread -o $@

test: $(BUILD_DIR)/test_core \
      $(BUILD_DIR)/primeparts-bench-core \
      $(BUILD_DIR)/primeparts-bench-materialize \
      $(BUILD_DIR)/primeparts-generate \
      $(BUILD_DIR)/primeparts-test-ui-iceberg
	$(BUILD_DIR)/test_core
	$(BUILD_DIR)/primeparts-bench-core --start-idx 1 --count 1000 --threads 2 >/tmp/primeparts-bench-core-smoke.txt
	$(BUILD_DIR)/primeparts-bench-materialize --start-idx 1 --count 1000 --threads 2 --chunk-primes 500 >/tmp/primeparts-bench-materialize-smoke.txt
	$(BUILD_DIR)/primeparts-generate --start-idx 1 --count 1000 --temp --chunk-primes 500 >/tmp/primeparts-generate-smoke.json
	MANIFEST=$$(python3 -c "import json; d=json.loads(open('/tmp/primeparts-generate-smoke.json').read()); print(d['manifest'])"); \
	  ICEBERG_DIR=$$(dirname "$$MANIFEST"); \
	  PYTHONPATH=../src python -m primeparts.native_iceberg --manifest "$$MANIFEST" --temp >/tmp/primeparts-native-commit-smoke.json; \
	  $(BUILD_DIR)/primeparts-test-ui-iceberg "$$ICEBERG_DIR" >/tmp/primeparts-test-ui-iceberg-smoke.txt

iceberg-cpp-rebuild:
	cmake -S vendor/iceberg-cpp -B $(ICEBERG_CPP_BUILD) $(ICEBERG_CPP_CMAKE_ARGS)
	cmake --build $(ICEBERG_CPP_BUILD) -j
	cmake --install $(ICEBERG_CPP_BUILD)

$(BUILD_DIR)/nth_prime_boundaries.o: bin/nth_prime_boundaries.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/nth_prime_boundaries: $(BUILD_DIR)/nth_prime_boundaries.o
	$(CC) $(LDFLAGS) $(EXE_LDFLAGS) $^ $(LDLIBS) -o $@

clean:
	rm -rf $(BUILD_DIR)
