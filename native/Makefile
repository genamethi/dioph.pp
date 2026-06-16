PREFIX ?= /usr/local
PKG_CONFIG ?= /usr/bin/pkg-config
PKG_CONFIG_PATH ?= $(PREFIX)/lib/pkgconfig:/usr/lib/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig
CC ?= /usr/bin/gcc
CXX ?= /usr/bin/g++

CPPFLAGS += -Iinclude -I$(PREFIX)/include
CFLAGS += -std=c11 -D_POSIX_C_SOURCE=200809L -O3 -g -fPIC -Wall -Wextra -Wpedantic
CXXFLAGS += -std=c++23 -O3 -g -fPIC -Wall -Wextra -Wpedantic
LDFLAGS += -L$(PREFIX)/lib -Wl,-rpath,$(abspath $(PREFIX)/lib)
EXE_LDFLAGS += -no-pie
LDLIBS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(PKG_CONFIG) --libs primecount primesieve gmp flint) -lpari -lm
NOTCURSES_LDLIBS ?= $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(PKG_CONFIG) --libs notcurses-core 2>/dev/null || PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(PKG_CONFIG) --libs notcurses 2>/dev/null)
NOTCURSES_CPPFLAGS ?= $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(PKG_CONFIG) --cflags notcurses-core 2>/dev/null || PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(PKG_CONFIG) --cflags notcurses 2>/dev/null)
NOTCURSES_RPATH ?= -Wl,-rpath,/usr/local/lib

# Lua 5.5 embedded in the TUI (parses scripts/lua/*.lua presets). Static
# liblua.a at /usr/local keeps the all-static build; Lua needs -lm -ldl.
LUA_CPPFLAGS ?= -I/usr/local/include
LUA_LDLIBS ?= /usr/local/lib/liblua.a -lm -ldl

# iceberg-cpp is built from source in vendor/iceberg-cpp/ and installed
# to $(PREFIX) by hand. We link the static archives directly and pick up
# arrow/parquet from the same prefix via pkg-config so the transitive
# static dep set (brotli, snappy, protobuf, abseil, ...) stays correct.
ICEBERG_CPPFLAGS += -I$(PREFIX)/include
ICEBERG_LDFLAGS +=
ICEBERG_LDLIBS += \
	-liceberg_data -liceberg_bundle -liceberg_rest -liceberg_vendored_cpr \
	-liceberg_vendored_avrocpp -liceberg \
	-liceberg_vendored_nanoarrow -liceberg_vendored_croaring \
	$(PREFIX)/lib/libparquet.a $(PREFIX)/lib/libarrow.a \
	$(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(PKG_CONFIG) --libs --static parquet arrow | sed -e 's/-lparquet[ ]//g' -e 's/-larrow[ ]//g' -e 's/-L[^ ]*[ ]//g') \
	-lcurl -lssl -lcrypto -ldl -lrt

# LMDB is vendored in vendor/lmdb and built into a small static archive used by
# the native catalog store (pp_lmdb_store). Build-time only; it adds no
# transitive dependencies. The upstream SQL-catalog engine lives in the
# separately-installed libiceberg_sql_catalog.a (no SQL connector was built, so
# no sqlpp23/sqlite); we inject the LMDB store through its CatalogStore seam.
LMDB_DIR := vendor/lmdb/libraries/liblmdb
LMDB_CPPFLAGS += -I$(LMDB_DIR)
LMDB_CFLAGS := -O3 -g -fPIC -pthread
SQL_CATALOG_LDLIBS := -liceberg_sql_catalog

BUILD_DIR := build
CORE_OBJ := $(BUILD_DIR)/core.o
BENCH_CORE_OBJ := $(BUILD_DIR)/bench_core.o
BENCH_MATERIALIZE_OBJ := $(BUILD_DIR)/bench_materialize.o
GENERATE_OBJ := $(BUILD_DIR)/generate.o
GENERATE_ABI_OBJ := $(BUILD_DIR)/generate_abi.o
WRITER_OBJ := $(BUILD_DIR)/writer.o
SOURCE_SCAN_OBJ := $(BUILD_DIR)/source_scan.o
PRIMITIVE_FACTORS_OBJ := $(BUILD_DIR)/primitive_factors.o
PRIMITIVE_FACTORS_MAIN_OBJ := $(BUILD_DIR)/primitive_factors_main.o
COVERING_SIEVE_MAIN_OBJ := $(BUILD_DIR)/covering_sieve_main.o
SIEVE_TRIAGE_MAIN_OBJ := $(BUILD_DIR)/sieve_triage_main.o
REWRITE_OBJ := $(BUILD_DIR)/rewrite.o
PP_ICEBERG_REST_OBJ := $(BUILD_DIR)/pp_iceberg_rest.o
PP_HIVE_SYNC_OBJ := $(BUILD_DIR)/pp_hive_sync.o
PP_ROW_DELTA_OBJ := $(BUILD_DIR)/pp_row_delta.o
PP_DELETE_SPIKE_OBJ := $(BUILD_DIR)/pp_delete_spike.o
PP_SIEVE_CLONE_OBJ := $(BUILD_DIR)/pp_sieve_clone.o
PP_LMDB_STORE_OBJ := $(BUILD_DIR)/pp_lmdb_store.o
PP_LMDB_SMOKE_OBJ := $(BUILD_DIR)/pp_lmdb_smoke.o
LMDB_LIB := $(BUILD_DIR)/liblmdb.a
PP_CATALOG_MAIN_OBJ := $(BUILD_DIR)/pp_catalog_main.o
PREFLIGHT_OBJ := $(BUILD_DIR)/preflight.o
UI_ICEBERG_OBJ := $(BUILD_DIR)/ui_iceberg.o
TEST_OBJ := $(BUILD_DIR)/test_core.o
TEST_UI_ICEBERG_OBJ := $(BUILD_DIR)/test_ui_iceberg.o
TEST_ICEBERG_WRITER_OBJ := $(BUILD_DIR)/test_iceberg_writer.o
TEST_PRIMITIVE_FACTORS_OBJ := $(BUILD_DIR)/test_primitive_factors.o
TUI_FRONTEND_OBJ := $(BUILD_DIR)/tui_frontend.o
MERSENNE_SIDECAR_OBJ := $(BUILD_DIR)/mersenne_sidecar.o

.PHONY: all clean test smoke

all: $(BUILD_DIR)/libprimeparts_core.so \
     $(BUILD_DIR)/primeparts-bench-core \
     $(BUILD_DIR)/primeparts-bench-materialize \
     $(BUILD_DIR)/primeparts-generate \
     $(BUILD_DIR)/primeparts-primitive-factors \
     $(BUILD_DIR)/primeparts-covering-sieve \
     $(BUILD_DIR)/primeparts-sieve-triage \
     $(BUILD_DIR)/pp-catalog \
     $(BUILD_DIR)/primeparts-test-ui-iceberg \
     $(BUILD_DIR)/mersenne-sidecar

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(CORE_OBJ): src/core.c include/primeparts/core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BENCH_CORE_OBJ): src/bench.c include/primeparts/core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BENCH_MATERIALIZE_OBJ): src/materialize_bench.c include/primeparts/core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(GENERATE_OBJ): src/generate.cc include/primeparts/core.h include/primeparts/writer.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ICEBERG_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(GENERATE_ABI_OBJ): src/generate.cc include/primeparts/core.h include/primeparts/generate.h include/primeparts/writer.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ICEBERG_CPPFLAGS) $(CXXFLAGS) -DPRIMEPARTS_GENERATE_NO_MAIN -c $< -o $@

$(UI_ICEBERG_OBJ): src/ui_iceberg.cc include/primeparts/ui_iceberg.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ICEBERG_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(WRITER_OBJ): src/writer.cc include/primeparts/writer.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ICEBERG_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(SOURCE_SCAN_OBJ): src/source_scan.cc include/primeparts/source_scan.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ICEBERG_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(PRIMITIVE_FACTORS_OBJ): src/coverings/primitive_factors.cc include/primeparts/primitive_factors.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(PRIMITIVE_FACTORS_MAIN_OBJ): src/coverings/primitive_factors_main.cc include/primeparts/primitive_factors.h include/primeparts/source_scan.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ICEBERG_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(COVERING_SIEVE_MAIN_OBJ): src/coverings/covering_sieve_main.cc include/primeparts/primitive_factors.h include/primeparts/source_scan.h include/primeparts/catalog/pp_iceberg_rest.h include/primeparts/catalog/pp_row_delta.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ICEBERG_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(SIEVE_TRIAGE_MAIN_OBJ): src/coverings/sieve_triage_main.cc include/primeparts/source_scan.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ICEBERG_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/query_smoke.o: src/query/query_smoke.cc include/primeparts/source_scan.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ICEBERG_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/query_service.o: src/query/query_service.cc include/primeparts/query/query_service.h include/primeparts/source_scan.h include/primeparts/catalog/pp_iceberg_rest.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ICEBERG_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/query_service_smoke.o: src/query/query_service_smoke.cc include/primeparts/query/query_service.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ICEBERG_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/tui_main.o: src/tui/tui_main.cc include/primeparts/query/query_service.h include/primeparts/tui/lua_presets.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(NOTCURSES_CPPFLAGS) $(ICEBERG_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/lua_presets.o: src/tui/lua_presets.cc include/primeparts/tui/lua_presets.h include/primeparts/query/query_preset.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(LUA_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/lua_presets_smoke.o: src/tui/lua_presets_smoke.cc include/primeparts/tui/lua_presets.h include/primeparts/query/query_service.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ICEBERG_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/lua_query_module.o: src/query/lua_query_module.cc include/primeparts/query/lua_query_module.h include/primeparts/query/query_service.h include/primeparts/core.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(LUA_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/lua_query_smoke.o: src/query/lua_query_smoke.cc include/primeparts/query/lua_query_module.h include/primeparts/query/query_service.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(LUA_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(REWRITE_OBJ): src/rewriter/rewrite.cc include/primeparts/writer.h include/primeparts/source_scan.h include/primeparts/preflight.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ICEBERG_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(PP_ICEBERG_REST_OBJ): src/catalog/pp_iceberg_rest.cc include/primeparts/catalog/pp_iceberg_rest.h include/primeparts/catalog/pp_lmdb_store.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ICEBERG_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(PP_HIVE_SYNC_OBJ): src/catalog/pp_hive_sync.cc include/primeparts/catalog/pp_hive_sync.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(PP_ROW_DELTA_OBJ): src/catalog/pp_row_delta.cc include/primeparts/catalog/pp_row_delta.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ICEBERG_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(PP_DELETE_SPIKE_OBJ): src/catalog/pp_delete_spike.cc include/primeparts/catalog/pp_delete_spike.h include/primeparts/catalog/pp_row_delta.h include/primeparts/source_scan.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ICEBERG_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(PP_SIEVE_CLONE_OBJ): src/catalog/pp_sieve_clone.cc include/primeparts/catalog/pp_sieve_clone.h include/primeparts/catalog/pp_iceberg_rest.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ICEBERG_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

# --- LMDB catalog store --------------------------------------------------
$(BUILD_DIR)/lmdb_mdb.o: $(LMDB_DIR)/mdb.c | $(BUILD_DIR)
	$(CC) $(LMDB_CFLAGS) -c $< -o $@

$(BUILD_DIR)/lmdb_midl.o: $(LMDB_DIR)/midl.c | $(BUILD_DIR)
	$(CC) $(LMDB_CFLAGS) -c $< -o $@

$(LMDB_LIB): $(BUILD_DIR)/lmdb_mdb.o $(BUILD_DIR)/lmdb_midl.o
	$(AR) rcs $@ $^

$(PP_LMDB_STORE_OBJ): src/catalog/pp_lmdb_store.cc include/primeparts/catalog/pp_lmdb_store.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ICEBERG_CPPFLAGS) $(LMDB_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(PP_LMDB_SMOKE_OBJ): src/catalog/pp_lmdb_smoke.cc include/primeparts/catalog/pp_lmdb_store.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ICEBERG_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(PP_CATALOG_MAIN_OBJ): src/catalog/pp_catalog_main.cc include/primeparts/catalog/pp_iceberg_rest.h include/primeparts/catalog/pp_hive_sync.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ICEBERG_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(PREFLIGHT_OBJ): src/rewriter/preflight.cc include/primeparts/preflight.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ICEBERG_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(TEST_OBJ): tests/test_core.c include/primeparts/core.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(TEST_UI_ICEBERG_OBJ): tests/test_ui_iceberg.c include/primeparts/ui_iceberg.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(TEST_ICEBERG_WRITER_OBJ): tests/test_iceberg_writer.cc include/primeparts/writer.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(ICEBERG_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(TEST_PRIMITIVE_FACTORS_OBJ): tests/test_primitive_factors.cc include/primeparts/primitive_factors.h | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(TUI_FRONTEND_OBJ): src/tui_frontend.c include/primeparts/core.h include/primeparts/ui_iceberg.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(NOTCURSES_CPPFLAGS) $(CFLAGS) -c $< -o $@

$(MERSENNE_SIDECAR_OBJ): src/mersenne_sidecar.cc | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/libprimeparts_core.so: $(CORE_OBJ)
	$(CC) -shared $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/primeparts-bench-core: $(CORE_OBJ) $(BENCH_CORE_OBJ)
	$(CC) $(LDFLAGS) $(EXE_LDFLAGS) $^ $(LDLIBS) -pthread -o $@

$(BUILD_DIR)/primeparts-bench-materialize: $(CORE_OBJ) $(BENCH_MATERIALIZE_OBJ)
	$(CC) $(LDFLAGS) $(EXE_LDFLAGS) $^ $(LDLIBS) -pthread -o $@

$(BUILD_DIR)/primeparts-generate: $(CORE_OBJ) $(GENERATE_OBJ) $(WRITER_OBJ)
	$(CXX) $(ICEBERG_LDFLAGS) $(LDFLAGS) $(EXE_LDFLAGS) $^ $(LDLIBS) $(ICEBERG_LDLIBS) -lparquet -larrow -pthread -o $@

$(BUILD_DIR)/primeparts-rewrite: $(REWRITE_OBJ) $(SOURCE_SCAN_OBJ) $(WRITER_OBJ) $(PREFLIGHT_OBJ)
	$(CXX) $(ICEBERG_LDFLAGS) $(LDFLAGS) $(EXE_LDFLAGS) $^ $(ICEBERG_LDLIBS) -lparquet -larrow -lsqlite3 -pthread -o $@

# pp_iceberg_rest now provides MakeLocalCatalog = SqlCatalog(LmdbStore), so every
# consumer links the SQL-catalog engine + LMDB store. SQL_CATALOG_LDLIBS must
# precede ICEBERG_LDLIBS (static link order); liblmdb.a backs pp_lmdb_store.o.
$(BUILD_DIR)/pp-catalog: $(PP_CATALOG_MAIN_OBJ) $(PP_ICEBERG_REST_OBJ) $(PP_HIVE_SYNC_OBJ) $(PP_ROW_DELTA_OBJ) $(PP_DELETE_SPIKE_OBJ) $(PP_SIEVE_CLONE_OBJ) $(SOURCE_SCAN_OBJ) $(PP_LMDB_STORE_OBJ) $(LMDB_LIB)
	$(CXX) $(ICEBERG_LDFLAGS) $(LDFLAGS) $(EXE_LDFLAGS) $^ $(SQL_CATALOG_LDLIBS) $(ICEBERG_LDLIBS) -lsqlite3 -pthread -o $@

# libiceberg_sql_catalog.a must precede the iceberg core archives so its
# references resolve in static link order; liblmdb.a backs pp_lmdb_store.o.
$(BUILD_DIR)/primeparts-lmdb-smoke: $(PP_LMDB_SMOKE_OBJ) $(PP_LMDB_STORE_OBJ) $(LMDB_LIB)
	$(CXX) $(ICEBERG_LDFLAGS) $(LDFLAGS) $(EXE_LDFLAGS) $(PP_LMDB_SMOKE_OBJ) $(PP_LMDB_STORE_OBJ) \
		$(SQL_CATALOG_LDLIBS) $(ICEBERG_LDLIBS) $(LMDB_LIB) -pthread -o $@

$(BUILD_DIR)/primeparts-primitive-factors: $(PRIMITIVE_FACTORS_MAIN_OBJ) $(PRIMITIVE_FACTORS_OBJ) $(SOURCE_SCAN_OBJ)
	$(CXX) $(ICEBERG_LDFLAGS) $(LDFLAGS) $(EXE_LDFLAGS) $^ $(LDLIBS) $(ICEBERG_LDLIBS) -lparquet -larrow -lsqlite3 -pthread -o $@

$(BUILD_DIR)/primeparts-covering-sieve: $(COVERING_SIEVE_MAIN_OBJ) $(PRIMITIVE_FACTORS_OBJ) $(SOURCE_SCAN_OBJ) $(PP_ICEBERG_REST_OBJ) $(PP_ROW_DELTA_OBJ) $(PP_LMDB_STORE_OBJ) $(LMDB_LIB)
	$(CXX) $(ICEBERG_LDFLAGS) $(LDFLAGS) $(EXE_LDFLAGS) $^ $(LDLIBS) $(SQL_CATALOG_LDLIBS) $(ICEBERG_LDLIBS) -lsqlite3 -pthread -o $@

$(BUILD_DIR)/primeparts-sieve-triage: $(SIEVE_TRIAGE_MAIN_OBJ) $(SOURCE_SCAN_OBJ)
	$(CXX) $(ICEBERG_LDFLAGS) $(LDFLAGS) $(EXE_LDFLAGS) $^ $(LDLIBS) $(ICEBERG_LDLIBS) -lparquet -larrow -lsqlite3 -pthread -o $@

$(BUILD_DIR)/query-smoke: $(BUILD_DIR)/query_smoke.o $(SOURCE_SCAN_OBJ)
	$(CXX) $(ICEBERG_LDFLAGS) $(LDFLAGS) $(EXE_LDFLAGS) $^ $(LDLIBS) $(ICEBERG_LDLIBS) -lparquet -larrow -lsqlite3 -pthread -o $@

# QueryService links the catalog seam (SqlCatalog(LmdbStore)): SQL_CATALOG_LDLIBS
# before ICEBERG_LDLIBS; liblmdb.a backs pp_lmdb_store.o.
$(BUILD_DIR)/query-service-smoke: $(BUILD_DIR)/query_service_smoke.o $(BUILD_DIR)/query_service.o $(PP_ICEBERG_REST_OBJ) $(PP_LMDB_STORE_OBJ) $(SOURCE_SCAN_OBJ) $(LMDB_LIB)
	$(CXX) $(ICEBERG_LDFLAGS) $(LDFLAGS) $(EXE_LDFLAGS) $^ $(LDLIBS) $(SQL_CATALOG_LDLIBS) $(ICEBERG_LDLIBS) -lsqlite3 -pthread -o $@

$(BUILD_DIR)/primeparts-test-ui-iceberg: $(UI_ICEBERG_OBJ) $(TEST_UI_ICEBERG_OBJ)
	$(CXX) $(ICEBERG_LDFLAGS) $(LDFLAGS) $(EXE_LDFLAGS) $^ $(ICEBERG_LDLIBS) -lparquet -larrow -lsqlite3 -o $@

$(BUILD_DIR)/primeparts-test-iceberg-writer: $(TEST_ICEBERG_WRITER_OBJ) $(WRITER_OBJ)
	$(CXX) $(ICEBERG_LDFLAGS) $(LDFLAGS) $(EXE_LDFLAGS) $^ $(ICEBERG_LDLIBS) -lparquet -larrow -pthread -o $@

$(BUILD_DIR)/primeparts-test-primitive-factors: $(TEST_PRIMITIVE_FACTORS_OBJ) $(PRIMITIVE_FACTORS_OBJ)
	$(CXX) $(LDFLAGS) $(EXE_LDFLAGS) $^ $(LDLIBS) -o $@

# New TUI (src/tui/, C++ on QueryService + the LMDB catalog seam). Replaces the
# scrapped tui_frontend.c. SQL_CATALOG_LDLIBS before ICEBERG_LDLIBS; notcurses-core.
$(BUILD_DIR)/primeparts-tui: $(BUILD_DIR)/tui_main.o $(BUILD_DIR)/query_service.o $(BUILD_DIR)/lua_presets.o $(PP_ICEBERG_REST_OBJ) $(PP_LMDB_STORE_OBJ) $(SOURCE_SCAN_OBJ) $(LMDB_LIB)
	$(CXX) $(ICEBERG_LDFLAGS) $(LDFLAGS) $(EXE_LDFLAGS) $(NOTCURSES_RPATH) $^ $(LDLIBS) $(SQL_CATALOG_LDLIBS) $(ICEBERG_LDLIBS) $(NOTCURSES_LDLIBS) $(LUA_LDLIBS) -lsqlite3 -pthread -o $@

$(BUILD_DIR)/primeparts-lua-presets-smoke: $(BUILD_DIR)/lua_presets_smoke.o $(BUILD_DIR)/lua_presets.o $(BUILD_DIR)/query_service.o $(PP_ICEBERG_REST_OBJ) $(PP_LMDB_STORE_OBJ) $(SOURCE_SCAN_OBJ) $(LMDB_LIB)
	$(CXX) $(ICEBERG_LDFLAGS) $(LDFLAGS) $(EXE_LDFLAGS) $^ $(LDLIBS) $(SQL_CATALOG_LDLIBS) $(ICEBERG_LDLIBS) $(LUA_LDLIBS) -lsqlite3 -pthread -o $@

# The reader `query` Lua module + smoke. Links core.o for the number-theory
# bindings (primecount/primesieve/FLINT via $(LDLIBS)).
$(BUILD_DIR)/primeparts-lua-query-smoke: $(BUILD_DIR)/lua_query_smoke.o $(BUILD_DIR)/lua_query_module.o $(BUILD_DIR)/query_service.o $(CORE_OBJ) $(PP_ICEBERG_REST_OBJ) $(PP_LMDB_STORE_OBJ) $(SOURCE_SCAN_OBJ) $(LMDB_LIB)
	$(CXX) $(ICEBERG_LDFLAGS) $(LDFLAGS) $(EXE_LDFLAGS) $^ $(LDLIBS) $(SQL_CATALOG_LDLIBS) $(ICEBERG_LDLIBS) $(LUA_LDLIBS) -lsqlite3 -pthread -o $@

# v0.3.0 arrow pulls mimalloc/brotli via libarrow_bundled_dependencies.a; the
# bare -larrow -lparquet link no longer resolves them. ICEBERG_LDLIBS carries the
# full static transitive set (libarrow.a + libparquet.a + bundled deps).
$(BUILD_DIR)/mersenne-sidecar: $(MERSENNE_SIDECAR_OBJ)
	$(CXX) $(ICEBERG_LDFLAGS) $(LDFLAGS) $(EXE_LDFLAGS) $^ $(LDLIBS) $(ICEBERG_LDLIBS) -o $@

$(BUILD_DIR)/test_core: $(CORE_OBJ) $(TEST_OBJ)
	$(CC) $(LDFLAGS) $(EXE_LDFLAGS) $^ $(LDLIBS) -pthread -o $@

test: $(BUILD_DIR)/test_core \
     $(BUILD_DIR)/primeparts-test-primitive-factors
	$(BUILD_DIR)/test_core
	$(BUILD_DIR)/primeparts-test-primitive-factors

smoke: $(BUILD_DIR)/primeparts-lmdb-smoke
	$(BUILD_DIR)/primeparts-lmdb-smoke

clean:
	rm -rf $(BUILD_DIR)
