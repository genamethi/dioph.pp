# Native Iceberg Rewrite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the native staging rewrite produce Iceberg-owned Parquet data-file metadata instead of relying on a custom `files.jsonl` handoff as catalog truth.

**Architecture:** Patch vendored `iceberg-cpp` so its Parquet `DataWriter` forwards the same physical layout properties currently used by `BucketParquetWriter`. Then move the native rewrite file sink to the patched Iceberg writer path and let Iceberg construct canonical `DataFile` metadata for later manifest/snapshot/catalog publication.

**Tech Stack:** C++23, Arrow C Data Interface, Arrow/Parquet C++, vendored `iceberg-cpp`, native Primeparts writer/rewrite code.

---

### Task 1: Parquet Writer Property Pass-Through

**Files:**
- Modify: `native/vendor/iceberg-cpp/src/iceberg/file_writer.h`
- Modify: `native/vendor/iceberg-cpp/src/iceberg/parquet/parquet_writer.cc`
- Modify: `native/vendor/iceberg-cpp/src/iceberg/test/parquet_test.cc`

- [ ] **Step 1: Write the failing test**

Add a `ParquetReadWrite` test that writes a small two-column file through `WriterFactoryRegistry::Open(FileFormatType::kParquet, ...)` with:

```cpp
writer_properties.Set(WriterProperties::kParquetCompression,
                      std::string("uncompressed"));
writer_properties.Set(WriterProperties::kParquetMaxRowGroupLength, int64_t{2});
writer_properties.Set(WriterProperties::kParquetDataPageSize, int64_t{1024});
writer_properties.mutable_configs()
    [std::string(WriterProperties::kParquetEncodingColumnPrefix.key()) + "id"] =
        "DELTA_BINARY_PACKED";
writer_properties.mutable_configs()
    [std::string(WriterProperties::kParquetDictionaryEnabledColumnPrefix.key()) + "id"] =
        "false";
```

The test opens Parquet metadata and asserts two row groups for three rows, and that the `id` column encoding stats include `DELTA_BINARY_PACKED` without `RLE_DICTIONARY`.

- [ ] **Step 2: Run the test to verify it fails**

Run:

```bash
cmake -S native/vendor/iceberg-cpp -B /tmp/primeparts-iceberg-cpp-build -DICEBERG_BUILD_TESTS=ON -DICEBERG_BUILD_REST=OFF
cmake --build /tmp/primeparts-iceberg-cpp-build --target parquet_test -j2
/tmp/primeparts-iceberg-cpp-build/src/iceberg/test/parquet_test --gtest_filter='ParquetReadWrite.WriterAppliesParquetLayoutProperties'
```

Expected: compile failure before the patch because the new writer property entries do not exist, or assertion failure because row group/encoding properties are ignored.

- [ ] **Step 3: Implement minimal vendor patch**

Add these `WriterProperties` entries:

```cpp
inline static Entry<int64_t> kParquetDataPageSize{
    "write.parquet.page-size-bytes", 1024 * 1024};
inline static Entry<int64_t> kParquetMaxRowGroupLength{
    "write.parquet.max-row-group-length", 64 * 1024 * 1024};
inline static Entry<std::string> kParquetEncodingColumnPrefix{
    "write.parquet.encoding.column.", ""};
inline static Entry<std::string> kParquetDictionaryEnabledColumnPrefix{
    "write.parquet.dictionary-enabled.column.", ""};
```

In `parquet_writer.cc`, parse and apply:

```cpp
properties_builder.data_pagesize(options.properties.Get(
    WriterProperties::kParquetDataPageSize));
properties_builder.max_row_group_length(options.properties.Get(
    WriterProperties::kParquetMaxRowGroupLength));
for (const auto& [column, value] :
     options.properties.Extract(WriterProperties::kParquetDictionaryEnabledColumnPrefix.key())) {
  if (value == "false") properties_builder.disable_dictionary(column);
  if (value == "true") properties_builder.enable_dictionary(column);
}
for (const auto& [column, value] :
     options.properties.Extract(WriterProperties::kParquetEncodingColumnPrefix.key())) {
  properties_builder.encoding(column, ParseParquetEncoding(value).value());
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run:

```bash
cmake --build /tmp/primeparts-iceberg-cpp-build --target parquet_test -j2
/tmp/primeparts-iceberg-cpp-build/src/iceberg/test/parquet_test --gtest_filter='ParquetReadWrite.WriterAppliesParquetLayoutProperties'
```

Expected: PASS.

### Task 2: Primeparts Iceberg Writer Adapter

**Files:**
- Modify: `native/include/primeparts/writer.h`
- Modify: `native/src/writer.cc`
- Modify: `native/Makefile`

- [ ] **Step 1: Add a focused native test or smoke binary**

Add a small native test target that writes a `primes` batch through the Iceberg writer adapter, closes it, and verifies the returned file metadata has the expected row count, final path, partition fields, and byte size.

- [ ] **Step 2: Run the test to verify it fails**

Run:

```bash
make -C native build/primeparts-test-iceberg-writer CC=cc CXX=c++
native/build/primeparts-test-iceberg-writer
```

Expected: fail to compile because the adapter does not exist yet.

- [ ] **Step 3: Implement the adapter**

Add an Iceberg-backed sink that accepts the existing `WriterConfig`, Arrow schema, batch, and batch stats shape, but opens an `iceberg::data::DataWriter` with the patched Parquet properties. Keep the current path planning and file-boundary ownership in Primeparts.

- [ ] **Step 4: Run the test to verify it passes**

Run:

```bash
make -C native build/primeparts-test-iceberg-writer CC=cc CXX=c++
native/build/primeparts-test-iceberg-writer
```

Expected: PASS.

### Task 3: Native Rewrite Catalog Metadata Path

**Files:**
- Modify: `native/src/rewrite.cc`
- Modify: `native/include/primeparts/writer.h`
- Modify: `native/src/writer.cc`

- [ ] **Step 1: Add rewrite smoke coverage**

Add or extend native smoke coverage so a tiny rewrite staging run produces Iceberg `DataFile` metadata for `primeparts.primes`, `primeparts.partitions`, and copied `primeparts.boundaries`.

- [ ] **Step 2: Run it to verify it fails**

Run the focused native rewrite smoke target after building `primeparts-rewrite`.

- [ ] **Step 3: Wire rewrite output to Iceberg-owned metadata**

Replace the rewrite-only authoritative `files.jsonl` path with returned Iceberg `DataFile` metadata from the adapter. Keep `files.jsonl` as a temporary audit artifact only while full manifest/snapshot/catalog registration is being finished.

- [ ] **Step 4: Run smoke verification**

Run:

```bash
make -C native build/primeparts-rewrite CC=cc CXX=c++
```

Expected: compile succeeds and the focused smoke output contains Iceberg-owned data-file metadata.

### Task 4: Catalog Publication Boundary

**Files:**
- Modify: `native/src/rewrite.cc`
- Add or modify: native catalog publication files after confirming the Iceberg C++ catalog APIs in this checkout

- [x] **Step 1: Confirm local catalog APIs**

Inspect the installed and vendored Iceberg C++ catalog APIs for metadata JSON creation, manifest writing, snapshot creation, SQLite/HMS/REST registration, and table creation.

- [x] **Step 2: Implement the narrowest native catalog publication path**

Use Iceberg C++ metadata/manifests/snapshots as the source of truth for a fresh `primeparts.*` staging catalog. Metadata-only local publication remains available for smoke tests, and `--rest-uri` / `PRIMEPARTS_REST_CATALOG_URI` switches the same rewrite result to Iceberg REST catalog publication when the catalog service is available.

- [ ] **Step 3: Verify the catalog result**

Use native or existing read-only status tooling to load the staged metadata and verify table identity, schema, partition spec, data-file count, row totals, and copied boundaries.
