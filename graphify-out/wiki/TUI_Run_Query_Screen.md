# TUI Run Query Screen

> 19 nodes · cohesion 0.22

## Key Concepts

- **main()** (30 connections) — `native/src/tui/tui_main.cc`
- **tui_runquery.cc** (19 connections) — `native/src/tui/tui_runquery.cc`
- **draw_query()** (8 connections) — `native/src/tui/tui_runquery.cc`
- **confirm_modal()** (5 connections) — `native/src/tui/tui_main.cc`
- **option_count()** (5 connections) — `native/src/tui/tui_main.cc`
- **show_history_page()** (5 connections) — `native/src/tui/tui_runquery.cc`
- **start_query()** (5 connections) — `native/src/tui/tui_runquery.cc`
- **confirm_dispatch()** (4 connections) — `native/src/tui/tui_runquery.cc`
- **cycle_value()** (4 connections) — `native/src/tui/tui_runquery.cc`
- **history_back()** (4 connections) — `native/src/tui/tui_runquery.cc`
- **history_fwd()** (4 connections) — `native/src/tui/tui_runquery.cc`
- **nav()** (4 connections) — `native/src/tui/tui_runquery.cc`
- **open_modal()** (4 connections) — `native/src/tui/tui_runquery.cc`
- **option_name()** (4 connections) — `native/src/tui/tui_runquery.cc`
- **option_value()** (4 connections) — `native/src/tui/tui_runquery.cc`
- **string** (3 connections)
- **open_dispatch()** (3 connections) — `native/src/tui/tui_runquery.cc`
- **page_results()** (3 connections) — `native/src/tui/tui_runquery.cc`
- **push_query_history()** (3 connections) — `native/src/tui/tui_runquery.cc`

## Relationships

- [TUI App State](TUI_App_State.md) (16 shared connections)
- [TUI Main Render Loop](TUI_Main_Render_Loop.md) (11 shared connections)
- [TUI Config Screen](TUI_Config_Screen.md) (6 shared connections)
- [TUI Generate Screen](TUI_Generate_Screen.md) (6 shared connections)
- [Query/Preset Headers](Query-Preset_Headers.md) (2 shared connections)
- [TUI Query Worker](TUI_Query_Worker.md) (1 shared connections)
- [Query Service Validation](Query_Service_Validation.md) (1 shared connections)

## Source Files

- `native/src/tui/tui_main.cc`
- `native/src/tui/tui_runquery.cc`

## Audit Trail

- EXTRACTED: 53 (66%)
- INFERRED: 27 (34%)
- AMBIGUOUS: 0 (0%)

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*