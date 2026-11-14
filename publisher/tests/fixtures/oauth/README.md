# oauth usage bodies

`usage-five-hour-null.json` is a `GET /api/oauth/usage` body read from kai on
2026-11-14 01:41Z, byte for byte as the API returned it (one line, compact).
It is the shape behind korg WI 4623: `"five_hour":null`, a `seven_day`
object, and `limits[]` holding only the weekly windows. Usage numbers only;
the request's bearer token is not in it.

`usage-five-hour-object.json` is the same body with `five_hour` replaced by an
object of `seven_day`'s shape (utilization 22.0, a reset five hours out) —
what a plan that still has a five-hour window returns. Hand-made from the
captured one so the two differ in that one field and nothing else.
