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

## What it abstracts away
- Trie structure, top-k pre-computation, sharding by prefix ranges.
- Real-time trending (the book defers it too).

## Things to try
- `trie_cache.keys: 1M` — misses hit `trie_db` and p99 fails the SLO at 20:00.
- `browser_cache.hit_ratio: 0` — the API tier needs twice the replicas.
