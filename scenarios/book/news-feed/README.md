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
  `media_store` is sized in bytes (`capacity: 1GB/s`), so it counts megabytes (phase 2c): each
  200 KB read costs 0.2 units of 1 MB, and `explain` checks its 324.3 misses/s at the peak in
  MB/s, `media_store  peak offered 64.86 MB/s (324.3/s) vs 1k MB/s total capacity  (6.49%,
  green)`. A read is answered in one megabyte's 40 ms (an upper bound for a 200 KB object: the
  model charges every request at least one unit's service time). The fan-out queue and its
  workers are followed on paper: the peak's 2.162k posts/s reach `fanout` (32 workers at 5 ms,
  6.4k jobs/s: `33.8%, green`), and each post's 300 feed writes take `feed_cache` to 661.5k/s
  of its 100k/s (`662%, red`). Each post's one look-up in `graph_db`, a key-value cluster of 6
  machines keeping N = 3 copies (plan 2c Task 10), is 3 replica-requests, against the machines'
  6 × 30k/s, the hottest of them (1 + z_6/sqrt(3 × 150) = 1.0604 times the average, z_6 = 1.28
  the expected maximum of 6 standard normals) checked:
  `graph_db  peak offered 6.486k/s (2.162k/s × N 3) vs 180k/s (6 nodes)  (hottest 3.82%, green)`.
  `run` goes through the whole day since then (about 14 s), but the scenario is not yet sized for
  its 12:00 peak (plan 2c Task 17): as shipped 59.9 % of the work is dropped (`feed_cache`'s one
  replica first) and `drop_rate` fails (exit 2), while the `read_feed` p99 (89.2 ms of 200 ms)
  and `fanout_q`'s lag (`<1us` of 2 m) pass. A request stops at its first failed step (plan 2c
  Task 12), so the losses also thin what follows them: a `read_feed` whose `feed_cache` lookup was
  dropped hydrates no posts. That keeps `post_cache` and `user_cache` below capacity for all but
  one step of the day (mean rho 0.476 and 0.465; before, they saturated at the peaks). That step
  is the 0.1 s in which the 12:00 spike starts: they reach rho 1 (`peak_rho` 1, `red_s` 0.1) and
  drop 974.8 and 764.8 requests, the whole day's drops after warmup, because that step's
  hydrations are still thinned by the share of reads `feed_cache` delivered in the step BEFORE the
  spike (0.47) instead of the spike's own (0.16): the model's one-step lag (a step's losses thin
  the later steps from the next simulation step on). From the next step on they are back below
  capacity. And a
  `post` whose `post_db` write was dropped is not published, so `fanout` drains at most the
  primary's 1k writes/s (5k units/s at 5 units a write).
- Queue lag at the evening peak as the fan-out workers fall behind.
- A 30-second database outage at 15:00: writes back up in `fanout_q`; reads survive on caches.

## What it abstracts away
- Celebrity handling (fan-out on read) — model as a second client with its own op in a variant.
- Ranking, pagination, inactive-user skipping (which the book uses to shrink fan-out).

## Things to try
- `fanout.workers: 8` — the fan-out queue never drains after the 12:00 spike.
- `post_cache`'s hit ratio (`{fixed: 0.95}`, chosen: `users.keys` are user ids, not the post
  ids the cache holds, so Che has no key space to read for it; see the comment in the YAML).
  `post_db` is red before any read reaches it: the day's peak is the 12:00 spike, 21.6k/s (3×
  a sine at 70 % of its base there), not 20:00's 16k/s, and its 10 % of posts are 2.162k
  writes/s at cost 5, 10.81k units/s against one primary's 5k/s (`primary ... (216%, red)`),
  which every replica replays. At 0.95 the hydration misses add 12.97k reads/s (20 per feed
  read), `each replica 17.3k units/s vs 5k/s (346%, red)`; at `{fixed: 0.8}`, 735 %. The cure
  for the writes is sharding (`shards`), as in the URL shortener; no hit ratio helps them.
- Switch `post` share to 0.3 (a very chatty day) and watch `graph_db`.
