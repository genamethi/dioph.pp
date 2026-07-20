# build notes — Makefile fragility

## Header dependencies are hand-maintained, not compiler-generated

`native/Makefile` lists each object's header prerequisites by hand, e.g.

```
$(SESSION_OBJ): src/client/session.cc include/primeparts/client/session.h \
    include/primeparts/catalog/rest_scan_plan.h ... | $(BUILD_DIR)
```

Two failure modes follow, and one bit during phase 01→03a:

1. **Missing prerequisite → stale object.** If a `.cc` includes a header the
   rule does not list, editing that header does not rebuild the object. `make`
   reports success and links a stale `.o`.
2. **Struct-layout skew → silent corruption.** When a field was added to
   `ScanPlan` (in `scan_plan.h`), the objects whose rules list `scan_plan.h`
   recompiled with the new `sizeof(ScanPlan)`, but `e2e_test.o` — whose rule
   carries **no** header prerequisites at all — kept the old layout. Linking
   mismatched layouts corrupted the plan poll state and surfaced as a bogus
   "scan planning did not finish within the poll timeout." A clean rebuild
   (`rm -f` the affected objects) made it vanish, which is the tell for an ABI
   skew rather than a logic bug.

## The fix (not yet applied)

Let the compiler generate dependencies. Add to the compile flags:

```
CPPFLAGS += -MMD -MP
```

and at the foot of the Makefile:

```
-include $(wildcard $(BUILD_DIR)/*.d)
```

`-MMD` writes a `build/<obj>.d` fragment listing every header the TU actually
included (user headers; system headers omitted); `-MP` adds phony targets so a
deleted header does not wedge the build. The `-include` pulls them back in on
the next run. After that, no rule needs a hand-written header list, and both
failure modes above become impossible.

Caveat: the object rules currently also carry `| $(BUILD_DIR)` order-only
prerequisites and per-object flag variations (ICEBERG_CPPFLAGS, GINAC_CPPFLAGS,
ICEBERG_SRC_CPPFLAGS). Those stay; only the header lists are replaced by the
generated `.d` includes. The whole Makefile is agent-written and improvised, so
this is a good first place to make it principled.
