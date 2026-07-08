# TUI Main Render Loop

> 24 nodes · cohesion 0.16

## Key Concepts

- **tui_main.cc** (26 connections) — `native/src/tui/tui_main.cc`
- **redraw()** (13 connections) — `native/src/tui/tui_main.cc`
- **frame()** (7 connections) — `native/src/tui/tui_main.cc`
- **draw_modal()** (6 connections) — `native/src/tui/tui_main.cc`
- **load_presets()** (6 connections) — `native/src/tui/tui_main.cc`
- **row()** (6 connections) — `native/src/tui/tui_main.cc`
- **path** (5 connections)
- **modal_field_count()** (5 connections) — `native/src/tui/tui_main.cc`
- **draw_results()** (5 connections) — `native/src/tui/tui_runquery.cc`
- **draw_gen_output()** (4 connections) — `native/src/tui/tui_generate.cc`
- **binary_dir()** (4 connections) — `native/src/tui/tui_main.cc`
- **binary_seed_path()** (4 connections) — `native/src/tui/tui_main.cc`
- **built_in_presets()** (4 connections) — `native/src/tui/tui_main.cc`
- **config_file_path()** (4 connections) — `native/src/tui/tui_main.cc`
- **config_presets_path()** (4 connections) — `native/src/tui/tui_main.cc`
- **draw_status()** (4 connections) — `native/src/tui/tui_main.cc`
- **draw_stub()** (4 connections) — `native/src/tui/tui_main.cc`
- **save_current()** (4 connections) — `native/src/tui/tui_main.cc`
- **string** (3 connections)
- **draw_dispatch()** (3 connections) — `native/src/tui/tui_main.cc`
- **draw_path_modal()** (3 connections) — `native/src/tui/tui_main.cc`
- **draw_topbar()** (3 connections) — `native/src/tui/tui_main.cc`
- **layout()** (3 connections) — `native/src/tui/tui_main.cc`
- **vector** (1 connections)

## Relationships

- [TUI App State](TUI_App_State.md) (13 shared connections)
- [TUI Run Query Screen](TUI_Run_Query_Screen.md) (4 shared connections)
- [TUI Query Worker](TUI_Query_Worker.md) (3 shared connections)
- [TUI Generate Screen](TUI_Generate_Screen.md) (3 shared connections)
- [Query/Preset Headers](Query-Preset_Headers.md) (2 shared connections)
- [Query Service Validation](Query_Service_Validation.md) (1 shared connections)
- [TUI Config Screen](TUI_Config_Screen.md) (1 shared connections)
- [Lua Query Presets](Lua_Query_Presets.md) (1 shared connections)

## Source Files

- `native/src/tui/tui_generate.cc`
- `native/src/tui/tui_main.cc`
- `native/src/tui/tui_runquery.cc`

## Audit Trail

- EXTRACTED: 61 (86%)
- INFERRED: 10 (14%)
- AMBIGUOUS: 0 (0%)

---

*Part of the graphify knowledge wiki. See [index](index.md) to navigate.*