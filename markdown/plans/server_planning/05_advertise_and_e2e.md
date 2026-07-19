# 05 — advertise and prove

dep: 04

Flip `scan-planning-mode` to `server` and make the advertisement something a
client actually reads. Today nothing reads it — not our code, not the vendored
client — so a flip on its own would be a claim nobody checks.

- [x] `--scan-planning-mode server|client` on catalogd, default `server`. A
      deployment that wants clients planning locally can still say so honestly.
- [x] `SendTableResult` reports the configured mode rather than a literal.
- [x] `FetchScanPlanningMode` on the client reads the advertisement off
      `loadTable`, so the value is consumable rather than decorative.
- [x] An unrecognized mode is a named error, not a silent default.
- [x] e2e asserts `/v1/config` advertises all four planning routes.
- [x] e2e asserts `loadTable` advertises `server`.
- [x] e2e drives a client that reads the mode, dispatches on it, and reaches the
      same task set as in-process planning.

grep-gate: no literal `"client"` or `"server"` planning mode remains in
`pp_catalogd.cc`; the value comes from options.

## notes

Nothing read `scan-planning-mode` before this phase — not our code, not the
vendored client. Flipping it alone would have been a claim nobody checks, which
is why `FetchScanPlanningMode` lands with the flip rather than after it.

The mode stays configurable rather than becoming a constant. A deployment that
wants clients planning locally can say so, and the value is then true rather
than aspirational.

`AClientThatReadsTheAdvertisementCanCompleteAScan` deliberately keeps the
dispatch in the test rather than in the client. Whether a `PlanScan` entry point
should read the advertisement and route automatically is a real interface
question — it would couple the REST client to the in-process planner — and it is
not settled here. The test shows the interface is sufficient to make that
choice; it does not make it.

In-process consumers (`source_scan.cc`, `query_service.cc`) read metadata off
disk and are not REST clients, so the advertisement does not bind them. That
holds only while metadata is local to the consumer; a deployment that moves
metadata server-side puts them on the REST path.
