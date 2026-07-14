# 08_catalogd

deps: none | status: done

done: pp_catalogd.cc serves 406 `UnsupportedOperationException` on `POST .../tables/{t}/plan`, `GET/DELETE .../tables/{t}/plan/{plan-id}`, `POST .../tables/{t}/tasks`; `scan-planning-mode: client` still advertised. Verified live against a scratch catalogd: all four routes → 406 with IcebergErrorResponse body.

grep gate: `curl -s -o /dev/null -w '%{http_code}' -X POST <catalogd>/v1/namespaces/primeparts/tables/primes/plan` → 406

## notes

- Registered in 00 holes: the future implementation invokes the 02 planner module server-side.
- `markdown/data_eng/catalogd_rest_gap.md` route table NOT updated (markdown/ untouchable per brief; the four rows should read "406 stub" — user's call).
