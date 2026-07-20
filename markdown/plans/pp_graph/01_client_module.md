# 01 — shared client module, header first

The reusable idiom every consumer (pp-graph first; generate/verify/pp are
follow-on candidates, out of scope here) speaks through. One module, one
header, authored and reviewed before any implementation.

- [ ] Author `include/primeparts/client/session.h` and review with the user
      before writing a line of implementation. Surface to propose:
      - `Session` — resolved rest-uri + namespace + catalog handle
        (`OpenCatalog`), `/v1/config` read once, advertised
        `scan-planning-mode` cached.
      - `TableHandle` — identifier + `TableMetadata` obtained via
        `loadTable` over REST (`Catalog::LoadTable` → `Table::metadata()`);
        no filesystem metadata reads anywhere in the module.
      - `Scan(TableHandle, scan::ScanPlanRequest)` → batch stream. Dispatches
        on the advertised mode: `server` → `PlanScanOnServer` (the four
        routes); `client` → in-process `PlanTableScan`. This is the single
        planning entry point the holes registry records as missing.
      - Shard fan-out: the server's task set partitioned across N reader
        threads via the existing `SourceTableReader` shard parameters;
        thread count a `Session` knob, 64-bit counters only.
- [ ] Implement behind the header; reuse `rest_scan_plan.{h,cc}`,
      `source_scan`, `scan_planner` as-is — the module composes them, it does
      not fork them.
- [ ] Rewire `pp_graph_exp.cc` to consume the module; delete its direct
      disk-metadata read and hand-rolled dispatch.
- [ ] e2e coverage following the existing suite pattern: dispatch honors the
      advertisement (server mode drives the routes; client mode never touches
      them), and the module completes a filtered scan against a forked
      catalogd.

Open question carried to review: whether `field-upper-bound` (the non-spec
extension) belongs on `Session` or stays a free function.
