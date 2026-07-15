# Design Context

Product and design context for this repo lives in `PRODUCT.md` (root). Read it before any UI work.

Short version: primeparts is a prime-partitions warehouse (generation, verification, cataloging); the UI surface is the notcurses terminal TUI in `native/src/tui/`, specced in `markdown/tui/tui_app_design.md`. Register is product, platform is terminal — web tooling does not apply. Guiding principles: never lose the user's query state, keyboard-first but discoverable, the pipeline is the hero, drill-down follows schema field names, long scans show progress and cancel cleanly.

Terminology: the (m, n, q) objects are **partitions** — never "decompositions".
