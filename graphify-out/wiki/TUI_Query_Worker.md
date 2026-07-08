# TUI Query Worker

> 6 nodes · cohesion 0.33

## Key Concepts

- **run_query_worker()** (5 connections) — `native/src/tui/tui_runquery.cc`
- **fval()** (3 connections) — `native/src/tui/tui_main.cc`
- **secs_since()** (3 connections) — `native/src/tui/tui_main.cc`
- **Preset** (2 connections)
- **time_point** (1 connections)
- **Preset** (1 connections)

## Relationships

- [TUI App State](TUI_App_State.md) (1 shared connections)

## Source Files

- `native/src/tui/tui_main.cc`
- `native/src/tui/tui_runquery.cc`

## Audit Trail

- EXTRACTED: 4 (67%)
- INFERRED: 2 (33%)
- AMBIGUOUS: 0 (0%)

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*