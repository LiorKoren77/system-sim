# Lab: rate limiter (ch. 4)

## Book design
- Algorithms: token bucket, leaky bucket, fixed window counter, sliding window log,
  sliding window counter. Each trades burst tolerance against memory and accuracy.
- Placement: API gateway / middleware; counters in Redis; rules in config, cached in workers.
- Distributed issues: race conditions (Lua scripts / sorted sets) and synchronization
  (centralised store), 429 responses with `X-Ratelimit-*` headers.

## What the model captures
- The token bucket as a fluid level: a 500-token burst passes untouched, longer bursts are
  shaved to 1k/s; leaky bucket smooths everything; fixed windows let 2x through at edges.
- The protected API (2 × 600/s) staying under 90% while the client spikes 5–20x.
- Counter store round trips as part of every request's latency.

## What it abstracts away
- Per-client keys (the limit here is aggregate), header semantics, rule loading.

## Things to try
- `simsys sweep --param nodes.limiter.algorithm=... --metric api.utilization` (phase 2).
- `burst: 5000` — the 30-second spike gets through and `api` turns red.
