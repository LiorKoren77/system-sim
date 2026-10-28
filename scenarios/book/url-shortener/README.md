# URL shortener (ch. 8)

## Book design
- Two APIs: `POST /shorten` (long → short) and `GET /{short}` (301/302 redirect).
- 7-character base-62 IDs from a unique ID generator (snowflake), not hashing + collision checks.
- Reads dominate 10:1, so a cache in front of the database serves most redirects.
- Rate limiter in front of the web tier to stop abusive clients.
- Relational store (billions of rows) with read replicas; web tier is stateless and scaled out.

## What the model captures
- The read/write split and the cache hit ratio computed from the Zipf key skew and cache size.
- Database saturation when the cache is too small or cold.
- The rate limiter shedding traffic above 20k/s with a 2,000-token burst allowance.
- The ID generator as a capacity-limited node (compare `ticket_server` in the ID lab).

## What it abstracts away
- Base-62 encoding, 301 vs 302 semantics, collision handling.
- Cache invalidation details (links are immutable, so there are none).

## Things to try
- Set `url_cache.keys` to `1M`: watch `url_db` go yellow at the afternoon peak.
- Remove the `limiter` and add a spike `{at: 14:00, for: 5m, factor: 5}`.
- Change `id_gen` to `ticket_server` with `replicas: 1`: the write path gets a hot spot.
