# Vendored iceberg-cpp local patches

`native/vendor/iceberg-cpp` tracks **apache/iceberg-cpp** directly (submodule,
currently pinned at `v0.3.0`). We carry a small series of local patches in
`native/vendor/patches/*.patch`, applied to the checked-out submodule by
`scripts/apply_vendor_patches.sh` (run automatically by `native/configure`).

The apply script is idempotent (sentinel keyed on the submodule HEAD) and
re-applies after a tag bump. A patch whose `git apply --check` fails has either
been **upstreamed** (retire it — delete the `.patch`, record it below) or needs
a **forward-port**.

## Active patches (vs `v0.3.0`)

| # | File | Purpose | Retire when |
|---|---|---|---|
| 0001 | `CMakeLists.txt` | Honor a user `-DCMAKE_COMPILE_WARNING_AS_ERROR` override instead of hard-forcing it ON (GCC false-positive `-Werror=free-nonheap-object` in `json_serde.cc`). | upstream guards the `set()` or stops defaulting warnings-as-error ON. |
| 0002 | `src/iceberg/table_metadata.cc` | `FreshPartitionSpec` starts `last_partition_field_id` at `kLegacyPartitionDataIdStart - 1` (1000-convention, Hive compat) so partition IDs don't collide with reserved `manifest_entry` IDs. | upstream adopts the 1000-base for fresh partition specs. |
| 0003 | `src/iceberg/arrow/arrow_io.cc` | `ResolvePath` accepts single-slash `file:/abs/path` URIs (Hive/Hadoop) that arrow's `PathFromUri` rejects. Without it, native iceberg-cpp cannot scan any Hive-created table. | `ResolvePath` / arrow `PathFromUri` handles single-slash `file:/`. |

## Retired patches

- **`json_serde` `ref` requirement field** (was patch 4/4 in HANDOFF §9 against
  the pre-0.3 tree). **Upstreamed in `v0.3.0`**: `src/iceberg/json_serde.cc`
  defines `kRef = "ref"` and uses it for the `AssertRefSnapshotId` requirement
  serde (the field that gates snapshot-advancing IRC commits). No longer
  carried.
