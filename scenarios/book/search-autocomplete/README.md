# Search autocomplete (ch. 13)

## Book design
- Data-gathering path: query logs → aggregators (weekly/daily) → workers build a trie →
  trie DB (document store or key-value, prefix → top-k suggestions) → trie cache.
- Query path: browser cache → load balancer → API servers → trie cache → trie DB on miss.
- Trie sharded by prefix with a shard-map manager; filtering of unsafe suggestions.

## What the model captures
- One request per keystroke and the 40% browser-cache cut.
- Extreme key skew (Zipf 1.2): a 50M-entry cache serves nearly everything; the trie DB
  is only touched by rare prefixes and rebuilds.
- The 100 ms latency budget against a 5 ms index read on misses.

## What the model shows
`simsys run` (about 4.5 s in Release, exit 0) plays the whole day at the shipped 100 ms step.
At the 20:00 peak the users send 36k keystrokes/s; the browser cache absorbs 40 % of the
suggests, the trie cache (Che hit ratio 0.995) absorbs nearly all of the rest, and `explain` finds
`trie_db` offered 103.1/s against its 8k/s (4 replicas x 2k/s): 1.29 %, green. The search side is
the tighter one: `aggregators` are offered 1.8k/s against the 2k/s of 4 workers at 2 ms, 90 %,
red in `explain` (the zone starts at 90 %) but never above it, so the run drops nothing. The
aggregated rows cost the store `aggregated_store` 1.8 MB/s of its 100 MB/s (1.8 %, green). The
day's result: `suggest` p99 6.86 ms against the 100 ms SLO, drop rate 0 against 0.0005; both pass.

`build_interval: 24h` is read, stored and printed (`build_interval=24h (rebuild load not
modelled)`). The model does not load the index or the aggregators with the rebuild, and it does
not age the answers: a suggestion is as fresh at 23:59 as at 00:01. In the real system the daily
rebuild competes with serving for the same machines, and a new trending query is invisible until
the next one.

## What it abstracts away
- Trie structure, top-k pre-computation, sharding by prefix ranges.
- Real-time trending (the book defers it too).

## Things to try
- `trie_cache.keys: 1M` — misses hit `trie_db` and p99 fails the SLO at 20:00.
- `browser_cache.hit_ratio: 0` — the API tier needs twice the replicas.
