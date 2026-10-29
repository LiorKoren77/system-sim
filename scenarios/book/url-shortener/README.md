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
The first draft (6 web servers, one database of a primary and 2 read replicas) failed all three
SLOs: `simsys run` gives it a redirect p99 of 766 ms (max 50 ms), a drop rate of 0.200 (max
0.001) and the database's busiest copy at 1 in its worst hour (max 0.9), and `explain` shows
why: `web  peak offered 19.2k/s vs 18k/s total capacity  (107%, red)` and `primary 8.64k
units/s vs 5k/s (173%, red), each replica 9.683k units/s vs 5k/s (194%, red)`. The fixes are the
book's remedies, sized with its numbers; the SLOs are unchanged.

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
  All three SLOs pass: `PASS p99_latency users redirect 7.49ms 50ms`, `PASS drop_rate 0 0.001`
  and `PASS utilization url_db - 0.6433 0.9 (worst 1h from 13:02)`, exit 0. No node drops
  anything; the busiest are the web tier (peak rho 0.8) and the database's replicas (0.6455).
- **The hour the day hides.** The third SLO, `{metric: utilization, node:
  url_db, max: 0.9, over: 1h}`, is checked on every hour of the run, the hours aligned from the
  end of the 2-minute warmup (00:02-01:02, 01:02-02:02, ...), and the worst hour decides. The
  busiest database copy averages 0.4306 over the day, which a whole-day utilization SLO would
  report; its worst hour is 13:02-14:02, the hour that ends just after the 14:00 peak, at
  **0.6433** (0.9966 of the peak's 0.6455: the sine is nearly flat around its top). With
  `shards: 2` that hour is 0.965 and the SLO fails while the day-long ones still pass (below).
- **The step size.** The scenario runs at `step: 1s` (86,280 observed steps) where the first
  draft ran at 100 ms (862,800 steps), ten times fewer steps and so about a tenth of the time
  (on the author's machine 0.4 s against 4.4 s in a Release build, 6.4 s against 65 s in
  Debug). The fluid step is exact while demand is constant within it, and the
  daily sine moves at most 0.003 % of its peak in a second. Compared field by field in
  `run --report json`, the two runs agree to 1.1e-8 (the day's utilizations, offered rates,
  committed writes), 2e-9 (every latency percentile; the p99 is 7.48975 ms in both) and 1.3e-6
  (the worst hour's utilization, 0.643311 against 0.643312): the same results at a tenth of the
  steps. A spike is different (see the limiter below): there the
  step decides how fast a full token bucket empties.
- **The rate limiter's role.** At the shipped load it refuses nothing (rejected 0): its 20k/s
  is a ceiling just above the 19.2k/s peak. Its job shows with a spike, `spikes: [{at:
  14:00, for: 5m, factor: 5}]` under `users.rate` (96k/s for 5 minutes): the limiter rejects
  22.8M requests (76k/s × 300 s), what passes is 20k/s plus the 2000-token burst, the web
  tier touches red for 1 s (0.5 s at 100 ms steps) and nothing drops: the three SLOs still pass
  (p99 7.52 ms; url_db's worst hour 0.6442). The limiter itself and its `rules_cache` are not
  spared: every request of the spike reaches both, 96k/s against their 100k/s default
  capacity, so each runs at rho 0.96, red for the 300 s (`red_s 300`) without dropping. A limiter is sized for the flood it
  refuses, not for the traffic it lets through. Without the limiter (users → lb directly) the
  same spike hits the web tier head-on: red for 302 s, 21.59M dropped, drop rate 0.01914 and
  redirect p99 66.4 ms: both day-long SLOs fail (url_db's hourly one passes at 0.651: the web
  tier, not the database, sheds the spike).

## What it abstracts away
- Base-62 encoding, 301 vs 302 semantics, collision handling.
- Cache invalidation details (links are immutable, so there are none). The hit ratio is the
  warm steady state from the start: no cold cache after a deploy.
- Storage: 365B rows and their bytes are not counted; shards are sized by write throughput.
- How a request's steps depend on each other, partly. A step after a failed step does not run:
  `shorten`'s database write is sent only the shortens whose id was issued (with a ticket server
  that drops ids, below, url_db's writes fall by its drop rate), and a redirect's miss step goes
  only to the cache's misses (`on: miss`). But the model is a fluid: "did not run" is a smaller
  flow, worked out from the fraction the earlier step delivered in the PREVIOUS simulation step
  (1 s here), so a change of loss reaches the database one step late (plus the delay of the link
  to the failing step's target, in whole steps: none here, a link of 0.25 ms rounds to zero
  steps), and the steps are assumed to fail independently of one another. A failed request
  costs nothing more: no retry, no timeout spent waiting.
- The client's 1 s timeout, partly. Each step, the response time of an op is
  a lognormal fitted to its mean and variance; the share of it slower than 1 s is counted as
  timed out (JSON's `timed_out_per_s`, the table's `timed out/s`), not answered, and the mean
  describes the requests answered in time. A percentile is read off the fit to every delivered
  request, capped at 1 s; one at a quantile q of which more than 1 − q of the delivered requests
  timed out (1 % for the p99) prints `>1s` and fails its SLO. The shipped day never comes near it
  (`timed_out_per_s` 0 for both ops). What it ignores: the servers still do a timed-out
  request's work (nothing cancels it), the client does not retry it, and a timed-out request is
  in no node's `drop_rate` (no node failed it).
- Percentiles are estimated (a lognormal fitted to the mean and variance), and an SLO without
  `over:` is checked over the whole day: a few minutes of red at the peak can hide in the day's
  p99. The hourly utilization SLO checks hours on a fixed grid, so a peak that straddles two
  hours is split between them. On this smooth daily wave a sliding window would find a slightly
  worse hour, 13:30-14:30 at about 0.645 against the grid's 0.6433; on a short burst the gap can be
  large (the schema's `over` paragraph: 0.82 against 0.7).

## Things to try
- **Remove the cache** (delete `url_cache` and its link, and make `redirect: [{to: url_db,
  op: read}]`): the database's replicas go red first and alone. `explain`: `primary 2.88k
  units/s vs 5k/s (57.6%, green), each replica 5.792k units/s vs 5k/s (116%, red)`: every
  redirect is now a database read, 17.47k/s at the peak. In the run (`--series`), url_db
  turns yellow at 07:16, red at 09:17, starts dropping at 10:26 when its queues are full,
  and stays red until 18:42. It drops 40.55M requests: drop rate 0.02698, redirect p99 474
  ms (mean 85.5 ms); all three SLOs fail (exit 2; the hourly utilization at 1, first reached in
  the hour from 11:02). Writes still commit (the primaries are at 58 %),
  but the overloaded replicas fall behind: staleness 111 ms and 15.49 missed writes/s per
  replica. Every other node is unchanged: the cache was the database's protection.
- Set `url_cache.keys` to `1M`: the hit ratio falls to 0.686 and the replicas go yellow at
  the afternoon peak (`3.795k units/s vs 5k/s (75.9%, yellow)`); the run still passes
  (redirect p99 7.92 ms, nothing dropped, url_db's worst hour 0.7564). The cache can shrink 20×
  before the database notices, because the Zipf head is what matters.
- Change `id_gen` to `{type: id_generator, algorithm: ticket_server, replicas: 1}` (one
  auto-increment database, ch. 7; the loader refuses `replicas: 2` for a ticket server): the
  write path gets a hot spot. `explain`: `id_gen  peak offered 1.728k/s vs 1k/s total
  capacity  (173%, red)`. In the run it is red for 15.5 h of the day (55,650 s) and drops 22.96M
  id requests (drop rate 0.2309). Its full queue makes `shorten` slow, past the clients' 1 s
  timeout: of the 1.153k shortens/s offered over the day, 266.1/s lose their id
  (0.2309), 293.6/s are timed out (`run --report json`: `timed_out_per_s` 293.57, a third of
  the 886.6/s the path delivered) and 593.0/s are answered, in 463 ms on average, p50 545 ms,
  p95 and p99 **1 s**, the timeout itself (`>1s` in the table): a third of the delivered shortens
  timed out, far more than the 5 % and 1 % those quantiles leave above them, so the clients never
  saw them answered. The p50 is the client's too: the model times out exactly the shortens the
  fit to every delivered shorten puts past 1 s, so the timed-out third are that fit's slowest and
  its median, 545 ms, is the client's, the same as without the timeout (a refit to the answered
  shortens said 571 ms read at 0.5 / (1 − 0.331), and 329 ms read at 0.5, as if the slow third
  had never been sent). (Without the timeout: all 886.6/s answered, mean 669 ms, p50 545 ms,
  p99 2.42 s.) A shorten whose id was not issued is never
  written: url_db commits 295.5 writes/s per shard over the
  day instead of 384.2, the snowflake run's figure × (1 − 0.2309), and its busiest copy peaks at
  rho 0.4029 instead of 0.6455 (the ticket server caps the writes at its 1k ids/s). Redirects
  never touch the ticket server (p99 7.44 ms, a little under the shipped 7.49 ms: the replicas
  replay fewer writes), so the redirect SLO passes and drop_rate fails (exit 2). Snowflake's
  4.096M ids/s per machine is why the book picks it.
- Remove the `limiter` and add a spike `{at: 14:00, for: 5m, factor: 5}`: the two runs are
  in "The rate limiter's role" above.
- `shards: 2` on `url_db`: the replicas go red at the peak (96.8 %), yet the day-long SLOs pass
  (p99 9.02 ms): the fluid queue only drops above 100 %, and the day's p99 hides the hours near
  the peak. The hourly utilization SLO does not: `FAIL utilization url_db - 0.965 0.9 (worst 1h
  from 13:02)`, exit 2. It says what the zones say: no margin left.
- `web.replicas: 7`: `(91.4%, red)` in explain, p99 11.8 ms in the run, nothing dropped, for
  the same reason.
