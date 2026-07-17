# Product

## Register

product

## Platform

terminal

The UI surface is a notcurses terminal TUI (`native/src/tui/`). Web-only tooling (live mode, browser audits, CSS token systems) does not apply; design guidance applies to screens, panels, keybindings, and cell-level layout.

## Users

The primary user is the researcher-author, working interactively in their own truecolor terminal during number-theory research sessions. The secondary users are AI agents, which drive the query layer programmatically through the Lua query API and the catalog daemon rather than the TUI. Whatever a query can do interactively should remain reachable from the scripted surface.

## Product Purpose

primeparts is a warehouse over prime-partitions data: generation, verification, and cataloging of (m, n, q) partitions at billion-row scale, with the TUI as its interactive window. Day to day, the TUI's job is fast conjecture-chasing: pose a question about the data (a k-value, a prime, a range), get an answer, and drill into it in seconds — without writing ad-hoc C++ or SQL. Success is when the distance from "I wonder whether…" to a result on screen is a few keystrokes.

## Positioning

The value is the end-to-end reproducible pipeline — generation, verification, cataloging — and the TUI is a window onto it. Every screen should make the state of that pipeline legible, not compete with it.

## Brand Personality

Fast, terse, keyboard-first — optimized for speed of thought, with single-key verbs and a UI that stays out of the way. Balanced against discoverability and forgiveness: keybindings are visible and learnable in-app, state is clear, and recovery from mistakes is easy. Losing an in-progress query because of a screen switch is the canonical failure; it must never happen. Information density is a tool, not a value in itself — dense where the data warrants it, quiet elsewhere.

## Anti-references

- The old panel-soup TUI (already banned in `markdown/tui/tui_app_design.md`): everything visible at once with no screen structure.
- Ncurses-2005 chrome: heavy double-line box borders, function-key bars, midnight-commander styling.
- Modal-heavy IDEs: no deep nested modal stacks or wizard flows. A single shallow modal (e.g. editing a preset's variable fields) is fine; state stays shallow and visible.

## Design Principles

1. **Never lose the user's work.** Query state survives screen switches and cancellations; recovery from a mistake is one key, not a retype.
2. **Keyboard-first, but discoverable.** Every action has a key and every key is learnable from inside the app. Expert speed never hides an option from the newcomer.
3. **The pipeline is the hero.** The TUI is a window onto the warehouse; chrome earns its cells or goes.
4. **Drill-down follows meaning.** Values carry their schema field name, and exploration moves along mathematically sane edges (the `c`-key dispatch model), not generic navigation.
5. **Long work is honest.** Scans show progress and cancel cleanly; the UI asks the user to bound expensive scans (the p-window) rather than pretending they are cheap.

## Accessibility & Inclusion

No special constraints. The TUI runs on the author's own truecolor terminal; design for that environment rather than spending effort on degraded palettes or tiny terminals.
