# URL shortener (ch. 8)

## Book design
- Two APIs: `POST /shorten` (long → short) and `GET /{short}` (301/302 redirect).
- 7-character base-62 IDs from a unique ID generator (snowflake), not hashing + collision checks.
- Reads dominate 10:1, so a cache in front of the database serves most redirects.
- Rate limiter in front of the web tier to stop abusive clients.
- Relational store (billions of rows) with read replicas; web tier is stateless and scaled out.

The book's numbers: 100M URLs written a day (≈1,160/s), ten reads per write (≈11,600/s),
365B rows over 10 years. The scenario's client offers their sum, 12.8k/s, as 91 % `redirect`
and 9 % `shorten`, on a daily sine of ±50 % that peaks at 14:00 with 19.2k/s (12.8k × 1.5).

### How it is sized
The first draft (6 web servers, one database of a primary and 2 read replicas) failed both
SLOs: `simsys run` gave a redirect p99 of 766 ms (max 50 ms) and a drop rate of 0.200 (max
0.001), and `explain` showed why: `web  peak offered 19.2k/s vs 18k/s total capacity (107%,
red)` and `primary 8.64k units/s vs 5k/s (173%, red), each replica 9.683k units/s vs 5k/s
(194%, red)`. The fixes are the book's remedies, sized with its numbers; the SLOs are unchanged.

- **Web tier scaled out, 6 → 8 servers** (ch. 1 "stateless web tier"): 19.2k/s over 8 × 3k/s
  is `(80%, yellow)` at the peak. 7 would be `(91.4%, red)`. 8 also puts the tier's 24k/s
  above the limiter's 20k/s, so the limiter can never let the web tier go red.
- **Database sharded, `shards: 3`** (ch. 1 "sharding", the database part of ch. 8's wrap-up):
  writes at the peak are 9 % of 19.2k/s = 1.728k/s, each costing 5 capacity units (spec §6: one
  SQL box does 5k reads/s or 1k writes/s), so 8.64k units/s against one primary's 5k/s. Read
  replicas cannot help with that: each replica replays every write of its shard. Three
  shards, each a primary and 2 read replicas, give `primary 2.88k units/s vs 5k/s (57.6%,
  green), each replica 3.228k units/s vs 5k/s (64.6%, green)`. Two shards would leave the
  replicas at `4.841k units/s vs 5k/s (96.8%, red)`. (Storage alone forces sharding too:
  365B rows do not fit one box; the model does not count storage.)
- **The cache and the limiter stay as they were.** The cache already absorbed the reads
  (h = 0.881, below). The limiter (20k/s, burst 2000) never sheds at the peak: 19.2k/s is
  under its limit, and the run reports 0 rejected. What shed the 20 % was the web tier and the
  database.

## What the model captures
All numbers are from `simsys explain` and `simsys run` on `scenario.yaml` (24 h, 2 min
warmup).

- **The cache hit ratio from the key skew** (Che's approximation for LRU, ch. 8's "cache"
  and the 80/20 rule made precise): 20M slots over the clients' Zipf(1.0) space of 100M keys.
  `explain` prints `hit_ratio=0.881 (che: zipf 1 over 100M keys from users)`; an independent
  computation of Che's fixed point gives 0.8806. A cache of 20 % of the keys serves 88 % of
  the redirects. With `keys: 1M` (1 % of the keys) it is `hit_ratio=0.686`.
- **The 10:1 split, before and after the cache.** At the client, 91:9. At the database, at
  the peak: reads are the cache's misses, 17.47k/s × 0.119 = 2.08k reads/s; writes are all
  of `shorten`, 1.728k/s. So the cache turns 10:1 into about 1.2:1, and since a write costs 5
  units, the writes are 8.64k of the database's 10.73k units/s (`url_db  peak offered 10.73k
  units/s vs 45k/s total capacity`): 80 % of its work. The cache solves the read problem;
  what is left at the database is the write problem, which is why it is sharded.
- **The load of each copy, per shard** at the peak: a primary takes its shard's writes,
  1.728k × 5 / 3 = 2.88k units/s (57.6 %); each of the 6 replicas takes a sixth of the reads,
  2.08k / 6 = 347/s, plus every write of its shard, 2.88k: 3.228k units/s (64.6 %). Over the
  day each primary commits 384.2 writes/s (`committed writes/s per shard`), and a read is
  stale by `staleness 50.5ms` (the 50 ms replication lag plus the two sojourns).
- **Latency per op** (`run`, percentiles from a lognormal fitted to each op's mean and
  variance):

  | op | offered/s (day mean) | mean | p50 | p95 | p99 |
  |---|---|---|---|---|---|
  | `redirect` | 11.66k | 4.10 ms | 3.95 ms | 6.21 ms | 7.49 ms |
  | `shorten` | 1.153k | 4.54 ms | 4.41 ms | 6.61 ms | 7.82 ms |

  Most of it is wire: five round trips of 2 × 250 µs (users → limiter, limiter →
  rules_cache, limiter → lb, lb → web, web → url_cache) are 2.5 ms of the redirect's 4.10 ms;
  the rest is service times and the web tier's queueing (80 % at the peak, 53 % on average).
  Both SLOs pass: `PASS p99_latency users redirect 7.49ms 50ms`, `PASS drop_rate 0 0.001`,
  exit 0. No node drops anything; the busiest are the web tier (peak rho 0.8) and the
  database's replicas (0.6455).
- **The step size.** The scenario runs at `step: 1s` (86,280 observed steps; 0.4 s in a
  Release build, 6.4 s in Debug) where the first draft ran at 100 ms (862,800 steps; 4.4 s in
  Release, 65 s in Debug). The fluid step is exact while demand is constant within it, and the
  daily sine moves at most 0.003 % of its peak in a second. Compared field by field in
  `run --report json`, the two runs agree to 1.1e-8 (utilizations, offered rates, committed
  writes) and 2e-9 (every latency percentile; the p99 is 7.48975 ms in both): the same
  results at a tenth of the steps. A spike is different (see the limiter below): there the
  step decides how fast a full token bucket empties.
- **The rate limiter's role.** At the shipped load it refuses nothing (rejected 0): its 20k/s
  is a ceiling just above the 19.2k/s peak. Its job shows with a spike, `spikes: [{at:
  14:00, for: 5m, factor: 5}]` under `users.rate` (96k/s for 5 minutes): the limiter rejects
  22.8M requests (76k/s × 300 s), what passes is 20k/s plus the 2000-token burst, the web
  tier touches red for 1 s (0.5 s at 100 ms steps) and nothing drops: both SLOs still pass
  (p99 7.52 ms). The limiter itself and its `rules_cache` are not spared: every request of the
  spike reaches both, 96k/s against their 100k/s default capacity, so each runs at rho 0.96,
  red for the 300 s (`red_s 300`) without dropping. A limiter is sized for the flood it
  refuses, not for the traffic it lets through. Without the limiter (users → lb directly) the
  same spike hits the web tier head-on: red for 302 s, 21.59M dropped, drop rate 0.01914 and
  redirect p99 66.4 ms: both SLOs fail.

## What it abstracts away
- Base-62 encoding, 301 vs 302 semantics, collision handling.
- Cache invalidation details (links are immutable, so there are none). The hit ratio is the
  warm steady state from the start: no cold cache after a deploy.
- Storage: 365B rows and their bytes are not counted; shards are sized by write throughput.
- How a request's steps depend on each other: each step of an op gets its share of the op's
  traffic independently. With a ticket server that drops ids (below), the database still
  receives every `shorten`'s write, where a real web server would not insert a row without an
  id. A redirect's miss step does depend on the cache (`on: miss`).
- The client's 1 s timeout does not cut latencies short: a response slower than that is
  still counted, not turned into a retry or an error.
- Percentiles are estimated (a lognormal fitted to the mean and variance), and the SLOs are
  checked over the whole day, not per minute: a few minutes of red at the peak can hide in
  the day's p99.

## Things to try
- **Remove the cache** (delete `url_cache` and its link, and make `redirect: [{to: url_db,
  op: read}]`): the database's replicas go red first and alone. `explain`: `primary 2.88k
  units/s vs 5k/s (57.6%, green), each replica 5.792k units/s vs 5k/s (116%, red)`: every
  redirect is now a database read, 17.47k/s at the peak. In the run (`--series`), url_db
  turns yellow at 07:16, red at 09:17, starts dropping at 10:26 when its queues are full,
  and stays red until 18:42. It drops 40.55M requests: drop rate 0.02698, redirect p99 474
  ms (mean 85.5 ms); both SLOs fail (exit 2). Writes still commit (the primaries are at 58 %),
  but the overloaded replicas fall behind: staleness 111 ms and 15.49 missed writes/s per
  replica. Every other node is unchanged: the cache was the database's protection.
- Set `url_cache.keys` to `1M`: the hit ratio falls to 0.686 and the replicas go yellow at
  the afternoon peak (`3.795k units/s vs 5k/s (75.9%, yellow)`); the run still passes
  (redirect p99 7.92 ms, nothing dropped). The cache can shrink 20× before the database
  notices, because the Zipf head is what matters.
- Change `id_gen` to `{type: id_generator, algorithm: ticket_server, replicas: 1}` (one
  auto-increment database, ch. 7; the loader refuses `replicas: 2` for a ticket server): the
  write path gets a hot spot. `explain`: `id_gen  peak offered 1.728k/s vs 1k/s total
  capacity (173%, red)`. In the run it is red for 15.5 h of the day (55,650 s), drops 22.96M
  id requests (drop rate 0.2309), and `shorten` takes 669 ms on average, p99 2.42 s.
  Redirects never touch it (p99 still 7.49 ms), so the redirect SLO passes and drop_rate
  fails (exit 2). Snowflake's 4.096M ids/s per machine is why the book picks it.
- Remove the `limiter` and add a spike `{at: 14:00, for: 5m, factor: 5}`: the two runs are
  in "The rate limiter's role" above.
- `shards: 2` on `url_db`: the replicas go red at the peak (96.8 %), yet the SLOs pass (p99
  9.02 ms): the fluid queue only drops above 100 %, and the day's p99 hides the hours near
  the peak. The zones say what the SLOs do not: no margin left.
- `web.replicas: 7`: `(91.4%, red)` in explain, p99 11.8 ms in the run, nothing dropped, for
  the same reason.
