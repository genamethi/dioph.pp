# DRAFT — upstream iceberg-cpp PR/issue (for review before submitting)

Target repo: `apache/iceberg-cpp`. Not yet submitted. Verified against
`origin/main` @ `d5ab9f5` (2026-05-31) and the REST spec cached in this dir.

---

## Title

fix(rest): serialize `assert-ref-snapshot-id` requirement field as `ref`, not `ref-name`

## Type

Bug — REST spec conformance. Affects every snapshot-advancing commit
(`FastAppend`, `RowDelta`, anything attaching an `AssertRefSnapshotID`
requirement) when the catalog server validates against the reference spec.

## Summary

`UpdateRequirement` / `TableRequirement` serialization writes the
`assert-ref-snapshot-id` requirement's ref field as **`ref-name`**, but the
Iceberg REST OpenAPI spec defines that field as **`ref`**. A spec-conformant
server (e.g. Hive 4.2's HMS REST catalog servlet, which uses the Iceberg Java
`UpdateRequirementParser`) rejects the commit:

```
java.lang.IllegalArgumentException: Cannot parse missing string: ref
  at org.apache.iceberg.rest.requests.UpdateTableRequestParser.lambda$fromJson$1(UpdateTableRequestParser.java:97)
  at org.apache.iceberg.UpdateRequirementParser.fromJson(...)
```

The bug is invisible when an iceberg-cpp client commits to an iceberg-cpp-backed
(or lenient) server, because the same wrong key round-trips against itself — so
existing round-trip tests pass.

## Root cause

Two *different* REST objects carry a ref string, and the spec gives them
*different* field names:

| object | spec schema | field |
|---|---|---|
| `SetSnapshotRefUpdate` / `RemoveSnapshotRefUpdate` (metadata updates) | `allOf:[BaseUpdate, SnapshotReference]`, `required:[ref-name]` | **`ref-name`** |
| `AssertRefSnapshotId` (table requirement) | `required:[ref, snapshot-id]` | **`ref`** |

(Refs: `open-api/rest-catalog-open-api.yaml`, schemas `SetSnapshotRefUpdate`,
`RemoveSnapshotRefUpdate`, `AssertRefSnapshotId`. The Java reference mirrors
this: `MetadataUpdateParser` uses `"ref-name"`, `UpdateRequirementParser` uses
`"ref"`.)

`json_serde.cc` defines a single `constexpr kRefName = "ref-name"` and reuses it
for both the updates *and* the requirement. Correct for the updates; wrong for
the requirement.

## Fix

Introduce a distinct constant for the requirement field and use it only in the
`AssertRefSnapshotID` serializer and deserializer; leave the snapshot-ref
*updates* on `ref-name`.

```diff
 constexpr std::string_view kRefName = "ref-name";
+constexpr std::string_view kRef = "ref";

   // ToJson(const TableRequirement&), case kAssertRefSnapshotID:
-      json[kRefName] = r.ref_name();
+      json[kRef] = r.ref_name();

   // requirement FromJson, if (type == kRequirementAssertRefSnapshotID):
-    ICEBERG_ASSIGN_OR_RAISE(auto ref_name, GetJsonValue<std::string>(json, kRefName));
+    ICEBERG_ASSIGN_OR_RAISE(auto ref_name, GetJsonValue<std::string>(json, kRef));
```

## Suggested test

Add an interop-style assertion to the requirement serde test: serialize an
`AssertRefSnapshotID` and assert the JSON key is `ref` (and `snapshot-id`),
matching the spec example, rather than only asserting round-trip equality (which
masks the wrong key). A fixed JSON string from the spec is the strongest guard.

## Relation to #273

#273 marks `AssertRefSnapshotId` and the commit endpoint `[x]`. This is a
conformance fix within that implemented surface, not new coverage.

---

### Submission checklist (for the human)
- [ ] Confirm `kRefName` is not used by any *other* spec object that actually wants `ref-name`.
- [ ] Run the existing rest/json serde tests.
- [ ] Add the spec-literal test above.
- [ ] Check CONTRIBUTING (DCO/sign-off, commit message convention `fix(rest): …`).
- [ ] Search issues once more at submit time in case it was reported meanwhile.
