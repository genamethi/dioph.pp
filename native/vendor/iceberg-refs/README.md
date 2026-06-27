# iceberg-refs — cached authoritative Iceberg references

Local cache of the **authoritative** Iceberg REST/spec references, so native
catalog work is grounded in the actual contract instead of guesswork. Cached
2026-05-31. These are upstream documents — **read-only references, do not edit**;
refresh them from source (below) rather than hand-editing.

## Contents

| File | Source URL | What it is |
|---|---|---|
| `rest-catalog-open-api.yaml` | https://raw.githubusercontent.com/apache/iceberg/refs/heads/main/open-api/rest-catalog-open-api.yaml | The authoritative Iceberg REST Catalog **wire contract** (OpenAPI). Field names, required fields, request/response schemas. This is ground truth for what a conformant client/server sends. |
| `iceberg-cpp-issue-273.{json,md}` | https://github.com/apache/iceberg-cpp/issues/273 | iceberg-cpp's **REST conformance tracker** (OPEN). Per-endpoint / per-schema `[x]/[ ]` checklist of what iceberg-cpp implements. Use it to tell "not implemented yet" from "implemented but buggy" before diagnosing. |

## Why these are cached here

The vendored iceberg-cpp (`../iceberg-cpp`) is mid-maturity; several REST
behaviors are implemented-but-non-conformant or unimplemented. When something
fails (e.g. a commit rejected by HMS), the question is always: *does the spec
require X, and does iceberg-cpp claim to do X?* These two files answer both
without a network round-trip. See `../PATCHES.md` for local conformance fixes
we carry on top of the vendored lib.

Worked example (the reason this cache exists): a `RowDelta`/`FastAppend` commit
to Hive 4.2's HMS REST servlet failed with `Cannot parse missing string: ref`.
- Spec (`rest-catalog-open-api.yaml`, schema `AssertRefSnapshotId`): the
  requirement field is **`ref`**, `required: [ref, snapshot-id]`.
- Tracker (#273): `AssertRefSnapshotId` is marked `[x]` implemented.
- iceberg-cpp actually emits `ref-name` (`json_serde.cc` `kRefName`) → a
  conformance bug, not a missing feature. (See PATCHES.md.)

## Refreshing

Both come from `main`/live upstream and drift over time. Re-pull with:

```bash
cd native/vendor/iceberg-refs
./refresh.sh        # re-fetches both; prints a diff summary vs the cached copy
```

Or manually:
```bash
curl -fsSL -o rest-catalog-open-api.yaml \
  https://raw.githubusercontent.com/apache/iceberg/refs/heads/main/open-api/rest-catalog-open-api.yaml
gh issue view 273 --repo apache/iceberg-cpp \
  --json title,state,url,updatedAt,body,comments > iceberg-cpp-issue-273.json
```

The OpenAPI yaml tracks `apache/iceberg@main` (not a release tag) — pin to a tag
if a specific server version's contract is needed. Issue #273's `updatedAt` (in
the JSON) tells you whether the checklist moved since last cache.
