# 08_catalogd

deps: none | status: todo

- [ ] `pp_catalogd.cc` — 406 `UnsupportedOperationException` handlers: `POST .../tables/{t}/plan`, `GET/DELETE .../tables/{t}/plan/{plan-id}`, `POST .../tables/{t}/tasks`; keep `scan-planning-mode: client`
- [ ] register routes in 00 holes registry (future impl invokes 02 planner server-side)
- [ ] `markdown/data_eng/catalogd_rest_gap.md` route table — update statuses (allowed: plan tracker owns markdown/plans; gap docs record route reality)
- [ ] build green

grep gate: `curl -s -o /dev/null -w '%{http_code}' -X POST localhost:8181/v1/namespaces/primeparts/tables/primes/plan` → 406

## notes
