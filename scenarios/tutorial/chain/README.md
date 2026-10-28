# Tutorial 2: a chain of services

## Design
- Clients send 1.5k requests/s: 80 % reads, 20 % writes.
- A two-replica API tier answers every request by calling a database.
- A read costs one database call (`select`); a write costs two (`select`, then `insert`).
- The steps of one op run one after the other, so their latencies add up.

## What the model captures
- Load amplification: the API sees 1.5k/s but the database sees
  1.5k × (0.8 × 1 + 0.2 × 2) = 1.8k/s, because writes fan out into two calls.
- Utilization per tier: api rho = 1500 / (2 × 1000) = 0.75, which is yellow; db rho =
  1800 / 5000 = 0.36 = 36 %, which is green.
- Sequential steps add latencies: a write waits for two database calls, so its response
  time is the API's own time plus two database round trips, and it is slower than a read.
- Queueing delay: the busier a tier, the more of each request's time is spent waiting.

## What it abstracts away
- Connection pools between the API and the database.
- Transactions: the `select` and `insert` of a write are independent calls here.
- Per-op CPU cost: every call costs one unit of capacity (phase 2's cost table will let
  an `insert` cost more than a `select`).

## Things to try
- Set `replicas: 1` on `api`: rho becomes 1.5, the node goes red and the read SLO fails.
- Set `latency: 10ms` on `db`: a write pays it twice, so the write SLO fails first.
- Set `queue_limit: 50` on `db` and raise the client rate: the database starts shedding
  load sooner, so latency stays bounded but `drop_rate` rises above zero.
