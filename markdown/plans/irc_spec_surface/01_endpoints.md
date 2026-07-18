# 01 — route table + `/v1/config` `endpoints`

`/v1/config` omitted `endpoints`, so clients assumed the spec's default set
(yaml:105–135). That set both over- and under-described us: it excludes the four
plan routes and the two HEAD verbs we serve, and a client is entitled to
conclude anything outside it is unsupported.

`endpoints` is now derived from the router rather than hand-maintained, per the
decision recorded in `00_overview.md`. `RouteTable` (`pp_catalogd.cc`) wraps
`httplib::Server` and takes the spec path and the advertised verbs at the same
call that binds the handler, so a route cannot be served without stating how it
is advertised. `/v1/config` binds last and captures the accumulated array.

Non-spec extension routes pass an empty advertise list — `field-upper-bound` is
the only one. It stays bound and stays out of the array: `endpoints` is a
statement about spec routes, and a client cannot act on a path the spec does not
define. The empty list is explicit at the call site, so adding a route still
forces the question.

## verification

Server run against a scratch warehouse; `/v1/config` fetched live.

- 20 endpoints advertised, a strict superset of the spec's 14-entry default set —
  checked set-wise, `MISSING = none`, so no client loses a route it previously
  assumed.
- extras are the 4 plan routes and the 2 HEAD verbs, all genuinely bound.
- HEAD exercised because it is now advertised: existing namespace answers,
  missing namespace 404, missing table 404.

## surviving invariants

- A route is advertised only where it is bound. Registration and advertisement
  are one call; they cannot drift.
- `endpoints` must stay a superset of the spec default set. Dropping an entry
  silently withdraws a route from every conformant client.
- Non-spec routes are never advertised.

## found here, deferred to 02

`HEAD` on an existing namespace/table returns 200; the spec lists only 204.
cpp-httplib dispatches HEAD→GET, so both inherit the GET status.
