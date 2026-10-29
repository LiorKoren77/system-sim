# Search autocomplete (ch. 13)

## Book design
- Data-gathering path: query logs → aggregators (weekly/daily) → workers build a trie →
  trie DB (document store or key-value, prefix → top-k suggestions) → trie cache.
- Query path: browser cache → load balancer → API servers → trie cache → trie DB on miss.
- Trie sharded by prefix with a shard-map manager; filtering of unsafe suggestions.

## What the model captures
- One request per keystroke and the 40% browser-cache cut.
- Extreme key skew (Zipf 1.2): a 50M-entry cache serves nearly everything; the trie DB
  is only touched by rare prefixes (the rebuild's own load is not modelled, below).
- The 100 ms latency budget against a 5 ms index read on misses.

## What the model shows
`simsys run` plays the whole day at the shipped 100 ms step (862.8k steps; about 5 s in a Release
build on the author's machine) and exits 0. At the 20:00 peak the users send 24k/s × 1.5 = 36k
keystrokes/s, 95 % suggests and 5 % searches. The numbers below are `explain`'s peak checks and
the run's `peak_rho`:

| node | peak load | of | utilization |
|---|---|---|---|
| `browser_cache` | 34.2k suggests/s | 100k/s | 34.2 % |
| `api` (10 × 5k/s) | 22.32k/s: the 60 % of suggests the browser missed (20.52k) + 1.8k searches | 50k/s | 44.6 % |
| `trie_cache` | 20.52k/s, Che hit ratio h = 0.995 | 100k/s | 20.5 % |
| `trie_db` (4 × 2k/s) | (1 − h) × 20.52k = 103.1/s | 8k/s | 1.29 % |
| `query_log_q` | 1.8k logged searches/s | 1M/s | 0.18 % |
| `aggregators` (6 × 2 ms) | 1.8k jobs/s | 3k/s | 60 % |
| `aggregated_store` | 1.8k rows/s × 1 KB = 1.8 MB/s | 100 MB/s | 1.8 % |

- **The trie cache's hit ratio.** The Che approximation sizes a 50M-entry LRU against Zipf 1.2
  over 100M prefixes: h = 0.995. The browser cache in front of it has a *fixed* ratio (0.4), which
  says nothing about which keys it keeps, so the trie cache sees the users' key distribution
  unfiltered (`explain`: `fed through fixed-ratio cache 'browser_cache': treated as unfiltered`).
  A real browser cache keeps each user's own hot prefixes, so the trie cache would see a somewhat
  flatter stream and hit a little less.
- **The busiest node is on the data-gathering side** (the query log and its aggregation), not on
  the query path: the aggregators at 60 %, then the API tier at 44.6 %. The query log waits
  164 µs at the peak (`peak_lag`: Erlang C for 6 workers at 60 %, C(6, ρ = 0.6) = 0.197 over
  3k − 1.8k jobs/s).
- **The day's result:** `suggest` p99 **6.86 ms** (mean 2.16 ms) against the 100 ms SLO, drop
  rate 0 against 0.0005: both pass. The users give up after `timeout: 100ms`; the lognormal fit
  puts a vanishing tail past it: the table's `timed out/s` column shows `1.794e-08` for
  `users.suggest` (`run --report json`: `timed_out_per_s` 1.794e-08), about 8e-13 of the
  22.79k suggests/s of the day, so the p99 SLO reads the answered latency itself.
- **Why 10 API servers and 4 index replicas.** Neither count is set by load: 5 API servers would
  carry the peak's 22.32k/s at 89 %, and one index replica the 103.1/s of misses at 5.2 % of its
  2k/s. The spares are for availability: with one or two API servers down at the peak the tier
  stays green (49.6 % on 9, 55.8 % on 8), and it stays out of red even with no browser cache (72 %,
  "Things to try"); four index replicas keep the trie readable while one is down or loading the
  week's new trie (the rebuild's load is not modelled, below).

`build_interval: 168h`, the book's weekly rebuild, is read, stored and printed
(`build_interval=168h (rebuild load not modelled)`). The model does not load the index or the
aggregators with the rebuild, and it does not age the answers: a suggestion is as fresh at the end
of the week as just after a rebuild. In the real system the weekly rebuild competes with serving
for the same machines, and a new trending query is invisible until the next one.

## What it abstracts away
- Trie structure, top-k pre-computation, sharding by prefix ranges.
- Real-time trending (the book defers it too).

## Things to try
- **`aggregators.workers: 4`** (2k jobs/s): the peak is 1.8k/2k = **90 %**, exactly where the red
  zone starts, and `explain` marks it `(90%, red)`. The run shows what red means for a pool fed
  by a queue: no job is dropped and no SLO fails (suggest p99 6.86 ms, exit 0), but the query
  log's wait at the peak rises from 164 µs to **3.94 ms** (Erlang C for 4 workers at ρ = 0.9 is
  0.788; wait = 0.788 / (2k − 1.8k)/s), and a burst becomes lag, not drops: 1,000 extra searches
  at the peak queue and add 1,000 / 2k/s = 0.5 s of it. A queue-fed pool is sized by its lag
  budget (youtube's transcoders follow the same rule, at 96 %), and with a weekly rebuild that
  budget is days: **4 workers would do**; the shipped 6 is margin by choice, not a fix. (Whether
  the run counts the peak as red is a rounding accident: at the shipped 100 ms step the peak
  step's utilization comes out as 0.8999999999999999, one rounding error below the threshold,
  and `red_s` is 0; at `step: 1s` it is 0.90000000000000002 and `red_s` is 1. A load exactly on a
  threshold gets either verdict.)
  A sweep shows the wait falling fast as workers are added:

  ```
  $ simsys sweep scenarios/book/search-autocomplete/scenario.yaml \
      --param nodes.aggregators.workers=4..8:3 \
      --metric query_log_q.lag --metric aggregators.mean_rho
  nodes.aggregators.workers  query_log_q.lag  aggregators.mean_rho
  ----------------------------------------------------------------
  4                                   3.94ms                   0.6
  6                                    164us                   0.4
  8                                   15.7us                   0.3
  ```

  The lag is the peak's (Erlang C: 0.788 / 200 = 3.94 ms, 0.197 / 1.2k = 164 µs, 0.0346 / 2.2k =
  15.7 µs); `mean_rho` is the day's average: the mean rate, 24k/s, is two thirds of the 36k/s peak.
  With a weekly rebuild a few milliseconds of lag, or seconds after a burst, cost nothing: more
  workers buy a shorter lag, not a correct system.
- **`browser_cache.hit_ratio: {fixed: 0}`** (no browser caching): every keystroke reaches the
  servers, so `api` is offered 36k/s at the peak, **72 %** (yellow) instead of 44.6 %; the trie
  cache is offered 34.2k/s and `trie_db` 171.8/s (2.15 %). Ten API replicas still carry it: the
  browser cache buys headroom, not survival. The suggest mean rises from 2.16 to 3.22 ms (no answer
  comes from the browser any more), yet the reported p99 *falls*, 6.86 → 5.81 ms: with the browser
  cache the suggests are two populations (40 % answered in under a millisecond, 60 % in about
  3 ms), and the lognormal fitted to that mixture's mean and variance has a longer tail than either
  population (youtube's European streams show the same fit overstating a two-population mix). By
  this reasoning, not by a measurement (the report prints only the fit), the mixture's own p99 is
  the misses' 98.3rd percentile (1 − 0.01/0.6), which should lie below the 5.81 ms p99 of the
  no-cache run, whose misses meet a busier API tier.
- **A smaller trie cache, `trie_cache.keys: 1M`** (50× smaller): the Che hit ratio falls from
  0.995 to 0.952, so `trie_db` is offered 982.9/s at the peak instead of 103.1/s (12.3 %, still
  green), and the suggest p99 rises from 6.86 to 9.27 ms, which still passes the 100 ms SLO. The
  Zipf head carries the cache: at `keys: 1k` h is still 0.727 (`trie_db` at 70.2 %, p99 16.7 ms,
  both SLOs pass). Only at `keys: 100` (h = 0.551) is `trie_db` offered 9.211k/s, 115 % of its
  8k/s: it drops (drop rate 3.8 %), 16.1 % of the delivered suggests time out at 100 ms, and both
  SLOs fail (exit 2). On this skew, cache size is about the database's load long before it is
  about latency.
