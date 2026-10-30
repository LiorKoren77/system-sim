# Lab: consistent hashing (ch. 5)

## Book design
- Hash both servers and keys onto a ring; a key belongs to the first server clockwise.
- Adding/removing a server moves only the keys between it and its predecessor.
- Problems with few servers: uneven partition sizes and non-uniform key distribution.
  Fix: virtual nodes (e.g. 100–200 per server) — more even, at the cost of ring memory.

## The scenario
40k/s from the clients (90 % `get`, 10 % `set`, zipf 0.9 over 10M keys) through a
`consistent_hash` load balancer onto four cache servers of 15k/s and 2M keys each
(`read_through`: a missed `get` reads the database itself; a `set` updates the cache), and a
`sql_db` behind them: one primary and one read replica of 5k/s each. SLOs: the cache servers'
utilization at most 0.85 (the hottest server's: the model reports the copy that breaks first),
the database's at most 0.7.

The shipped file uses `vnodes: 150` and meets both SLOs; `vnodes: 1` is the experiment, and it
fails by design (below). Every runnable scenario is also an SLO test
(`tests/scenario/test_scenarios.cpp`), so the default must pass. The servers have 15k/s each,
not the draft's 12k/s: at 12k/s the four would run at 83 % on average, and ch. 5's own numbers
(the standard deviation of a server's load is about 5–10 % of the mean with 100–200 virtual
nodes) put the busiest one above that: 90 % at `vnodes: 150`, a failed SLO with the
recommended setting. 15k/s gives 67 % on average and room for the spread.

## What the model captures
- Load imbalance across the four cache servers as a function of `vnodes`: a server's share of
  the keys spreads about 1/sqrt(vnodes) around the average, and the hottest of 4 sits z_4 = 1.05
  such spreads above it (the expected maximum of 4 standard normals, by Blom's approximation),
  so it carries
  k = 1 + 1.05/sqrt(vnodes) times the average: **1.086** at 150 (a heuristic that grows with the
  number of servers, checked against Monte Carlo rings in
  [numerics, section 8](../../../docs/study/numerics.md); the kv-store lab's ring uses the same
  one). The hottest server's utilization is what the SLO checks.
- The cache tier's hit ratio (`hit_ratio: che`, from the clients' zipf 0.9 over 10M keys). The
  ring sends each key to one server, so the four servers hold different keys: 2M each, 8M
  together. With perfectly even arcs they would hit like one 8M-key LRU cache, 0.963. The arcs
  are not even: with imbalance k (above, the hottest server's share of the load over the
  average) the hottest server owns k/4 of the keys and the three others share the rest. Each
  server is an LRU cache of 2M keys over its own arc, which for the Che approximation is one
  cache of 2M / (its share) over the whole space, and it serves its share of the requests:
  h = Σ share × Che(2M / share). With `vnodes: 1`, k = 1 + 1.05/sqrt(1) = 2.05: the hot server
  owns 51.2 % of the keys (a 3.90M-key cache over the space: 0.844) and each cool one 16.3 %
  (12.3M > 10M keys: every key fits, 1), so h = 0.512 × 0.844 + 0.488 × 1 = **0.920**, and the
  DB gets 1 − 0.920 of the gets instead of 1 − 0.963: twice the misses. Hit ratio is concave in
  cache size, so what the hot server loses outweighs what the cool ones gain. `explain` prints
  `hit_ratio=0.92 (che: ...; 2M keys x 4 partitioned, uneven arcs)`.
- The database as ch. 1 draws it (spec §18 entry 6): the cache servers' misses are reads,
  and with `replicas: 2` they all go to the one read replica; the primary takes the writes. In
  this lab no write reaches the database (under `read_through` a `set` is the cache's own), so
  the primary idles and the replica carries the misses: `explain` prints `primary 0 units/s vs
  5k/s (0%, green), each replica 1.345k units/s vs 5k/s (26.9%, green)` (capacity units: a read
  costs 1, so here they are reads).
- **A server down.** The lab scripts one of the four servers down for 5 minutes from 10 m
  (`replicas: 1`). The model takes its quarter of the tier's capacity: every server serves 3/4 of
  its 15k/s while it lasts, the survivors sharing the load evenly, so the hottest runs at 0.7238 /
  0.75 = **0.965** for those 5 minutes (red: `red_s 300`), and the SLO's mean over the 19 observed
  minutes is 0.7238 × (14 + 5 / 0.75) / 19 = **0.7873**, still a PASS against 0.85. The gets'
  p99 over the run rises from 2.22 ms to 4.08 ms (mean 1.37 → 1.54 ms). Not modelled yet: that
  the removed server's arcs go to the servers after it on the ring, which then carry more than
  the even 3/4 share says, and that its keys arrive there cold (the hit ratio is resolved once,
  at build). The run's fault block (after the SLO table) shows what the run's mean hides: at
  0.965 the utilization SLO of 0.85 fails on every step of the 5 minutes taken alone, though it
  held before them and passes over the run:

  ```
  fault 00:10 node_down cache_servers for 5m
    during:   offered 40k/s, served 40k/s, lost 0 requests, backlog 0, utilization cache_servers flipped to FAIL
    after:    no backlog to drain
    recovery: op p99 back within 1% at 00:15 (0s after the fault cleared)
  ```

## What it abstracts away
- The hash function, ring data structure and lookup (binary search).
- Replication across neighbours on the ring (see the KV store lab).

## Results (`simsys run`, 20 min, after the 1 min warmup)

The rows are runs without the fault (delete the `faults` entry), so that each shows its `vnodes`
alone; as shipped, the fault raises the first row's hottest server to a mean of 0.787 and the get
to 1.54 ms / 4.08 ms, still PASS (exit 0).

| setting | imbalance k | hit ratio | hottest cache server rho | cache drops | db rho | get mean / p99 (est.) | SLOs |
|---|---|---|---|---|---|---|---|
| `vnodes: 150` (shipped) | 1.09 | 0.963 | 0.72 | 0 | 0.27 | 1.37 ms / 2.22 ms | PASS (exit 0) |
| `vnodes: 1000` | 1.03 | 0.963 | 0.69 | 0 | 0.27 | 1.37 ms / 2.21 ms | PASS |
| `vnodes: 100` | 1.1 | 0.963 | 0.74 | 0 | 0.27 | 1.37 ms / 2.23 ms | PASS |
| `vnodes: 1` | 2.05 | 0.92 | 1 (20.49k/s vs 15k/s: 137 %) | 6.26M (13.7 %) | 0.495 | 34.3 ms / 166 ms | cache FAIL (exit 2) |

- **`vnodes: 1`**: the hottest server owns 51 % of the ring, 20.49k/s against its 15k/s. Its
  queue fills (1k waiting, red for the whole run) and it sheds the excess, 5.49k/s, 13.7 % of
  all requests; a request that does get through waits about 67 ms in that full queue (1000
  requests drained at 15k/s), which is most of the 34.3 ms mean (51 % of the requests are on
  the hot server). The clients' `timeout: 200ms` gives up on 0.5 % of the
  requests that get through (`timed_out_per_s` 162.9 of the gets, 18.1 of the sets: s = 0.52 %
  of the gets delivered), and the mean is that of the requests answered in time (35.5 ms before
  the timeout applied). The p99 is what the client saw: the model times out exactly the requests
  its fitted lognormal puts past 200 ms, so the fastest 99 % of the delivered requests are answered
  ones, and the client's p99 is that fit's, 166 ms, as without the timeout (earlier rules refitted
  the answered requests' truncated moments, which said 174 ms read at 0.99 / (1 − s) and 143 ms
  read at 0.99). That 0.5 % is the tail of the one
  lognormal fitted to a two-peaked mixture
  (51 % of the requests at about 67 ms on the hot server, 49 % at about 1 ms on the cool ones),
  whose spread reaches past 200 ms; the two peaks themselves are far below it, so the share is an
  artefact of the fit more than a measurement. The three cool servers sit at 43 %. Uneven arcs
  also cost hit ratio, 0.920
  instead of 0.963: twice the misses on paper (`explain` offers the database's replica
  2.87k/s, 57 %); the hot server's drops never reach it, so it runs at 0.495 instead of 0.27.
- **More virtual nodes**: from 100 on, the hottest server is within about 10 % of the average,
  and the hit ratio is the even-arc value to three digits. Going from 150 to 1000 buys 3.5
  points of utilization on the hottest server, at the cost of ring memory (not modelled).

## Things to try
- `vnodes: 1` — the table's last row: the hottest server at 137 % fails the cache servers' SLO;
  the database's passes (0.495 against its 0.7), since the hot server's drops never reach it.
- `replicas: 2` on the cache servers — half the servers, the same 40k/s: 140 % on the hottest
  even with `vnodes: 150` (k = 1 + z_2/sqrt(150) = 1.048; hit ratio 0.848, so the database's
  replica gets 5.47k/s against its 5k/s and fails its SLO too: 0.82). With `vnodes: 1` the
  hotter of two servers holds k = 1 + 0.59 = 1.59 times the average, 79.5 % of the keys: the hit
  ratio falls to 0.824, the hottest server is at 212 % and the cache servers shed 42 % of the
  load, yet what gets through still misses enough to put the database's replica at 6.35k/s
  against its 5k/s: it fails too (0.74). Here the ring heuristic is at its least accurate: two
  servers of one virtual node each split the ring at two random points, so the hotter one's
  expected share is exactly 3/4, k = 1.5, not 1.59 (the heuristic is within 1 % at 100 virtual
  nodes and more, off by up to ±16 % at one; [numerics, section 8](../../../docs/study/numerics.md)).
  The verdict holds: with k = 1.5 the hit ratio is 0.831, the hottest server at 200 %, and the
  database's replica still fails, at 0.759.
- On the database: `replication_lag: 50ms` and an SLO `{metric: staleness, node: db, max:
  100ms}` — the staleness is a write's sojourn at the primary (its 150 µs service time: no
  write waits there) + the 50 ms lag + its sojourn at the replica (150 µs + a 55 µs wait at
  rho 0.27): `staleness 50.4ms`, PASS. No write flows to the database in this lab (a `set`
  stays in the cache), so the figure is the bound a write would see, not the age of any write
  that happened. `shards: 2` halves the replica's load (0.13; staleness 50.3ms). `replicas: 1`
  puts the reads on the primary: the same 0.27, since no writes compete with them here, and
  nothing is stale (`explain` marks the lag `(unused: no replicas)`).
