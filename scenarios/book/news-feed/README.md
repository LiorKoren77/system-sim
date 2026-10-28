# News feed (ch. 11)

## Book design
- Publishing: web → post DB + post cache → fan-out queue → fan-out workers look up the
  social graph and write the post id into each friend's feed cache (fan-out on write).
- Reading: web → feed cache (post ids) → post cache / user cache for hydration → DB on miss.
- Hybrid: celebrities use fan-out on read; the rest fan-out on write.
- CDN for media; caches layered: feed, post, user, social graph, counters.

## What the model captures
- The 300x write amplification of fan-out on write hitting `feed_cache` and `graph_db`.
- Read path fan-out (20 posts hydrated per feed read) multiplying cache traffic.
- Media reads (30% of requests, 200 KB each) go to the CDN and only misses reach `media_store`.
  `media_store` is sized in bytes (`capacity: 1GB/s`), so `explain` shows its 324.3 misses/s at
  the peak but does not check them (`not checked: capacity from 1GB/s at 1MB/object; byte flows:
  phase 2c`): at an assumed 1 MB per object the store would be 32.4 % busy, at the scenario's
  200 KB per read it is 65 MB/s of 1GB/s, about 6.5 %. Phase 2c's byte flows will check it.
- Queue lag at the evening peak as the fan-out workers fall behind.
- A 30-second database outage at 15:00: writes back up in `fanout_q`; reads survive on caches.

## What it abstracts away
- Celebrity handling (fan-out on read) — model as a second client with its own op in a variant.
- Ranking, pagination, inactive-user skipping (which the book uses to shrink fan-out).

## Things to try
- `fanout.workers: 8` — the fan-out queue never drains after the 12:00 spike.
- `post_cache.keys: 5M` — hydration misses push `post_db` into the red at 20:00.
- Switch `post` share to 0.3 (a very chatty day) and watch `graph_db`.
