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
- Load imbalance across the four cache servers as a function of `vnodes` (the standard
  deviation of partition sizes shrinks roughly with 1/sqrt(vnodes)); the hottest server's
  utilization is what the SLO checks.
- The cache tier's hit ratio (`hit_ratio: che`, from the clients' zipf 0.9 over 10M keys). The
  ring sends each key to one server, so the four servers hold different keys: 2M each, 8M
  together. With perfectly even arcs they would hit like one 8M-key LRU cache, 0.963. The arcs
  are not even: with imbalance k (1 + 1/sqrt(vnodes), the hottest server's share of the load
  over the average) the hottest server owns k/4 of the keys and the three others share the
  rest. Each server is an LRU cache of 2M keys over its own arc, which for the Che
  approximation is one cache of 2M / (its share) over the whole space, and it serves its share
  of the requests: h = Σ share × Che(2M / share). With `vnodes: 1`, k = 2: the hot server
  owns half the keys (a 4M-key cache over the space: 0.848) and each cool one a sixth (12M >
  10M keys: every key fits, 1), so h = 0.5 × 0.848 + 0.5 × 1 = **0.924**, and the DB gets
  1 − 0.924 of the gets instead of 1 − 0.963: twice the misses. Hit ratio is concave in cache
  size, so what the hot server loses outweighs what the cool ones gain. `explain` prints
  `hit_ratio=0.924 (che: ...; 2M keys x 4 partitioned, uneven arcs)`.
- The database as ch. 1 draws it (plan 2b refinement 3): the cache servers' misses are reads,
  and with `replicas: 2` they all go to the one read replica; the primary takes the writes. In
  this lab no write reaches the database (under `read_through` a `set` is the cache's own), so
  the primary idles and the replica carries the misses: `explain` prints `primary 0/s vs 5k/s
  (0%, green), each replica 1.345k/s vs 5k/s (26.9%, green)`.
- The fault at 10 m (one server removed) is accepted and ignored: faults are typed in phase 3.
  Then the server's capacity will go and the hit ratio, resolved once at build today, will be
  re-evaluated (the keys that move to the next server arrive cold).

## What it abstracts away
- The hash function, ring data structure and lookup (binary search).
- Replication across neighbours on the ring (see the KV store lab).

## Results (`simsys run`, 20 min, after the 1 min warmup)

| setting | imbalance k | hit ratio | hottest cache server rho | cache drops | db rho | get mean / p99 (est.) | SLOs |
|---|---|---|---|---|---|---|---|
| `vnodes: 150` (shipped) | 1.08 | 0.963 | 0.72 | 0 | 0.27 | 1.37 ms / 2.22 ms | PASS (exit 0) |
| `vnodes: 1000` | 1.03 | 0.963 | 0.69 | 0 | 0.27 | 1.37 ms / 2.21 ms | PASS |
| `vnodes: 100` | 1.1 | 0.963 | 0.73 | 0 | 0.27 | 1.37 ms / 2.23 ms | PASS |
| `vnodes: 1` | 2 | 0.924 | 1 (20k/s vs 15k/s: 133 %) | 5.7M (12.5 %) | 0.48 | 34.7 ms / 166 ms | cache FAIL (exit 2) |

- **`vnodes: 1`**: the hottest server owns half the ring, 20k/s against its 15k/s. Its queue
  fills (1k waiting, red for the whole run) and it sheds the excess, 5k/s, an eighth of all
  requests; a request that does get through waits about 70 ms in that full queue, which is the
  34.7 ms mean (half the requests are on the hot server). The three cool servers sit at 44 %.
  Uneven arcs also cost hit ratio, 0.924 instead of 0.963: twice the misses on paper (`explain`
  offers the database's replica 2.73k/s, 55 %); the hot server's drops never reach it, so it
  runs at 0.48 instead of 0.27.
- **More virtual nodes**: from 100 on, the hottest server is within 10 % of the average, and
  the hit ratio is the even-arc value to three digits. Going from 150 to 1000 buys 3 points of
  utilization on the hottest server, at the cost of ring memory (not modelled).

## Things to try
- `vnodes: 1` — the table's last row: the hottest server at 133 %, both SLOs' story above.
- `replicas: 2` on the cache servers — half the servers, the same 40k/s: 144 % on the hottest
  even with `vnodes: 150` (hit ratio 0.848, so the database's replica gets 5.5k/s against its
  5k/s and fails its SLO too: 0.82). With `vnodes: 1` the imbalance is total: k = 2 = N, the hot
  server owns every key, and the hit ratio falls to one 2M-key cache's, 0.747; the cache
  servers shed 62.5 % of the load, which is why the database, behind them, passes (0.68).
- On the database: `replication_lag: 50ms` and an SLO `{metric: staleness, node: db, max:
  100ms}` — the staleness is 50 ms plus the primary's write sojourn (no writes reach it, so
  just its 150 µs service time): `staleness 50.1ms`, PASS. `shards: 2` halves the replica's
  load (0.13). `replicas: 1` puts the reads on the primary: the same 0.27, since no writes
  compete with them here.
