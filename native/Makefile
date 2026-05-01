PREFIX ?= ../.pixi/envs/default
PKG_CONFIG ?= $(PREFIX)/bin/pkg-config
PKG_CONFIG_PATH ?= $(PREFIX)/lib/pkgconfig
CC = $(PREFIX)/bin/cc
CXX = $(PREFIX)/bin/c++
ICEBERG_CPP_BUILD ?= vendor/iceberg-cpp-build
ICEBERG_CPP_INSTALL ?= vendor/iceberg-cpp-install

CPPFLAGS += -Iinclude -I$(PREFIX)/include
CFLAGS += -std=c11 -D_POSIX_C_SOURCE=200809L -O3 -g -fPIC -Wall -Wextra -Wpedantic
CXXFLAGS += -std=c++23 -O3 -g -fPIC -Wall -Wextra -Wpedantic
LDFLAGS += -L$(PREFIX)/lib -Wl,-rpath,$(abspath $(PREFIX)/lib)
LDLIBS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(PKG_CONFIG) --libs primecount primesieve gmp flint) -lpari -lm
ICEBERG_CPPFLAGS += -I$(ICEBERG_CPP_INSTALL)/include -I$(ICEBERG_CPP_BUILD)/_deps/nanoarrow-src/src
ICEBERG_LDFLAGS += -L$(ICEBERG_CPP_BUILD)/src/iceberg -L$(ICEBERG_CPP_BUILD)/_deps/nanoarrow-build \
	-Wl,-rpath,$(abspath $(ICEBERG_CPP_BUILD)/src/iceberg) \
	-Wl,-rpath,$(abspath $(ICEBERG_CPP_BUILD)/_deps/nanoarrow-build)
ICEBERG_LDLIBS += -liceberg_bundle -liceberg -lnanoarrow_shared -lparquet -larrow -lzstd -lsnappy -lz

BUILD_DIR := build
CORE_OBJ := $(BUILD_DIR)/core.o
BENCH_CORE_OBJ := $(BUILD_DIR)/bench_core.o
BENCH_MATERIALIZE_OBJ := $(BUILD_DIR)/bench_materialize.o
GENERATE_OBJ := $(BUILD_DIR)/generate.o
UI_ICEBERG_OBJ := $(BUILD_DIR)/ui_iceberg.o
TEST_OBJ := $(BUILD_DIR)/test_core.o
TEST_UI_ICEBERG_OBJ := $(BUILD_DIR)/test_ui_iceberg.o

.PHONY: all clean test

all: $(BUILD_DIR)/libprimeparts_core.so \
     $(BUILD_DIR)/primeparts-bench-core \
     $(BUILD_DIR)/primeparts-bench-materialize \
     $(BUILD_DIR)/primeparts-generate \
     $(BUILD_DIR)/primeparts-test-ui-iceberg

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

$(UI_ICEBERG_OBJ): src/ui_iceberg.cc include/primeparts/ui_iceberg.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ICEBERG_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(TEST_OBJ): tests/test_core.c include/primeparts/core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(TEST_UI_ICEBERG_OBJ): tests/test_ui_iceberg.c include/primeparts/ui_iceberg.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/libprimeparts_core.so: $(CORE_OBJ)
	$(CC) -shared $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/primeparts-bench-core: $(CORE_OBJ) $(BENCH_CORE_OBJ)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -pthread -o $@

$(BUILD_DIR)/primeparts-bench-materialize: $(CORE_OBJ) $(BENCH_MATERIALIZE_OBJ)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -pthread -o $@

$(BUILD_DIR)/primeparts-generate: $(CORE_OBJ) $(GENERATE_OBJ)
	$(CXX) $(LDFLAGS) $(ICEBERG_LDFLAGS) $^ $(LDLIBS) $(ICEBERG_LDLIBS) -pthread -o $@

$(BUILD_DIR)/primeparts-test-ui-iceberg: $(UI_ICEBERG_OBJ) $(TEST_UI_ICEBERG_OBJ)
	$(CXX) $(LDFLAGS) $(ICEBERG_LDFLAGS) $^ $(ICEBERG_LDLIBS) -lsqlite3 -o $@

$(BUILD_DIR)/test_core: $(CORE_OBJ) $(TEST_OBJ)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -pthread -o $@

test: $(BUILD_DIR)/test_core \
      $(BUILD_DIR)/primeparts-bench-core \
      $(BUILD_DIR)/primeparts-bench-materialize \
      $(BUILD_DIR)/primeparts-generate \
      $(BUILD_DIR)/primeparts-test-ui-iceberg
	$(BUILD_DIR)/test_core
	$(BUILD_DIR)/primeparts-bench-core --start-idx 1 --count 1000 --threads 2 >/tmp/primeparts-bench-core-smoke.txt
	$(BUILD_DIR)/primeparts-bench-materialize --start-idx 1 --count 1000 --threads 2 --chunk-primes 500 >/tmp/primeparts-bench-materialize-smoke.txt
	$(BUILD_DIR)/primeparts-generate --start-idx 1 --count 1000 --temp --chunk-primes 500 --chunks-per-file 1 >/tmp/primeparts-generate-smoke.json
	WAREHOUSE=$$(python3 -c "import json,sys; d=json.loads(open('/tmp/primeparts-generate-smoke.json').read()); print(d['warehouse_root'])"); \
	  $(BUILD_DIR)/primeparts-test-ui-iceberg "$$WAREHOUSE" >/tmp/primeparts-test-ui-iceberg-smoke.txt

clean:
	rm -rf $(BUILD_DIR)
