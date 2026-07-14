# 09_merge

deps: all | status: todo

- [ ] `make -C native all test` green; `make -C native smoke` (generate-smoke pre-broken at base: kMinCount vs --count — not a gate)
- [ ] grep gates from every phase file re-run clean
- [ ] 00 holes registry audited: each hole still deliberate, named, NotImplemented (no quiet fallbacks crept in)
- [ ] zero-comment check on all new/modified files
- [ ] user runs: `pp-declare-sort` both tables → `primeparts-verify --p-lo X --p-hi Y` (planner + slicing) → TUI ScanByK/LookupPrime; pre-migration paths fail with named no-sort-order error
- [ ] merge `planner-prep` → `tui-query`

## notes
