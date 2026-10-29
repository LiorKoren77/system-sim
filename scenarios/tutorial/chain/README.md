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
- Per-op CPU cost: every call here costs one unit of capacity, although an `insert`
  usually costs the database more than a `select` (see "Things to try" for the `cost` key).

## Things to try
- Set `replicas: 1` on `api`: `explain` reports it offered 150 % of its capacity
  (1.5k/s against 1k/s), red. `run` shows a busy fraction of 100 % (it cannot be busier
  than always busy), a backlog stuck at the 1000-request queue limit and a third of the
  requests dropped (drop rate 0.3333): all three SLOs fail, exit code 2. A full queue of
  1000 requests drained at 1k/s makes each request wait about a second, which is exactly
  the clients' `timeout: 1s`. Of the requests the API answers, 51.8 % of the reads and
  53.8 % of the writes come back too late and are counted as timed out, not answered
  (`timed out/s` 414.4 and 107.6). With that many past the timeout, every percentile is
  the timeout itself and prints `>1s`; the mean (962 ms for a read) is the mean of the
  requests answered in time. Delete `timeout: 1s` and the same run reports what the
  clients would wait: p99 1.13 s. The database calms down to 24 % because the saturated
  API passes on only 1k/s.
- Raise `latency` on `db`: a write pays it twice, but its SLO is twice as loose, and two
  calls in a row partly average out (the mean doubles, the spread grows only by about
  √2), so a write's p99 is less than twice a read's. The read SLO fails first, between
  3 and 4 ms (at 4 ms: read p99 23 ms vs 20 ms, write 31.9 ms vs 40 ms); the write SLO
  follows between 5 and 6 ms, and at 10 ms both fail.
- Add `cost: {insert: 3}` to `db`: an insert now uses three units of its 5k/s, so the
  database's load is 1.5k selects + 3 × 300 inserts = 2.4k units/s and its rho rises from
  0.36 to 0.48, while the api tier and the latencies barely move (write p99 19 ms).
- Set `queue_limit: 50` on `db` and raise the client rate: the database starts shedding
  load sooner, so latency stays bounded but `drop_rate` rises above zero.
