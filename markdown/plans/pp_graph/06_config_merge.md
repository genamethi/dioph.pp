# 06 — config + merge (bracketed)

Downstream. Do not contort earlier phases to serve these.

- [x] **Lua** as persistent config/embedding where CLI flags would be (WH paths,
      threads, tuning parameters) — user configuration that is persistent and
      non-repetitive. Light wiring via the existing embed seam; the interpreter's
      functional/string power is a later bonus, not a driver.
- [ ] **Flight** (cross-machine — storage topology is not assumed) and the **TUI**
      stay bracketed / downstream.
- [ ] **Merge call** toward `tui-query` — the user decides what graduates.
