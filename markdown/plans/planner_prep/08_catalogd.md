# 08_catalogd

status: done (merged 0bb87bd)

Four plan routes (POST plan, GET/DELETE plan/{id}, POST tasks) serve 406 UnsupportedOperationException; `scan-planning-mode: client`. Future server-side lift invokes the 02 planner module (registry). `markdown/data_eng/catalogd_rest_gap.md` route table not updated (user's call).
