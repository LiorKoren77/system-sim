# Lab: rate limiter (ch. 4)

## Book design
- Algorithms: token bucket, leaky bucket, fixed window counter, sliding window log,
  sliding window counter. Each trades burst tolerance against memory and accuracy.
- Placement: API gateway / middleware; counters in Redis; rules in config, cached in workers.
- Distributed issues: race conditions (Lua scripts / sorted sets) and synchronization
  (centralised store), 429 responses with `X-Ratelimit-*` headers.

## The scenario
800/s of `call` with three spikes: 5x for 30 s at 5m, 2x for 2 min at 15m, 20x for 5 s at
25m. A limiter of 1k/s guards an `api` of 2 x 600/s (queue 200 per copy). Every checked
request reads and increments its counter in `counters`, a cache with `hit_ratio: {fixed: 1.0}`
(the counters live in memory, so every lookup hits) and `write_ops: [counter]` (the increment
is ch. 4's Redis INCR, a write; the graph refuses a store that would count it as a read). SLOs: `api` utilization at most 90 %, the
client's p99 at most 50 ms.

## What the model captures
- Each algorithm as the state ch. 4 describes, stepped every 100 ms (plan 2a refinement 6):
  the token bucket as a fluid level that starts full; the leaky bucket as the limiter's own
  queue, one deterministic slot leaking at 1k/s with room for `burst`; the fixed window as a
  counter reset every `window`; the sliding log as a ring of the last `window`'s admitted
  counts, one per step; the sliding counter as `prev x (1 - f) + cur`.
- Refused requests are rejected: never forwarded, and counted in `reject_rate`, apart from
  the drops of a node that fails. Latency figures count only the requests that are answered,
  and a request that waits out a surge in the api's queue is answered, late: its wait is in
  the mean and the p99 (only a drop or a refusal makes a request unanswered).
- The counter store round trip (two link legs + the cache's 0.2 ms) in every admitted
  request's latency.

## What it abstracts away
- Per-client keys (the limit here is aggregate), header semantics, rule loading, retries
  of rejected requests, races between limiter replicas.
- Anything inside a 100 ms step: a burst is spread over its step. A window must be a whole
  number of steps (`window: 150ms` is an error at `step: 100ms`), so its boundaries fall on
  step starts.

## Results (`simsys run`, 30 min, after the 1 min warmup)
Same file, one algorithm each: `leaky_bucket` keeps `burst: 500`; `fixed_window`,
`sliding_log` and `sliding_counter` take `window: 1s` instead of `burst` (the windowed
algorithms refuse a burst). The default file is the token bucket with `burst: 100`, which meets both SLOs; the
table below is the comparison at `burst: 500`, the setting the "things to try" section starts from.

| algorithm | rejected | api mean rho | api peak backlog | api drops | client mean | client p99 (est.) | SLOs |
|---|---|---|---|---|---|---|---|
| token_bucket (burst 500) | 14.2 % | 0.68 | 400 | 140 | 6.72 ms | 67.3 ms | p99 FAIL (exit 2) |
| leaky_bucket (burst 500) | 14.2 % | 0.68 | 0 | 0 | 64.1 ms | 624 ms | p99 FAIL (exit 2) |
| fixed_window (1 s) | 14.3 % | 0.68 | 400 | 9.6k | 21.6 ms | 228 ms | p99 FAIL (exit 2) |
| sliding_log (1 s) | 14.3 % | 0.68 | 160 | 0 | 11.9 ms | 129 ms | p99 FAIL (exit 2) |
| sliding_counter (1 s) | 15.1 % | 0.67 | 80 | 0 | 5.53 ms | 25.6 ms | PASS (exit 0) |

What happens at the spikes:
- **Token bucket**: at the start of each spike the full bucket lets its 500 tokens through
  on top of the refill, 600 requests in one 100 ms step, while the api clears 120. The
  response stays above 100 ms for about 2 s per spike. Under the 5x and the 20x spikes the
  queue fills (400) and overflows: the 140 drops are 60 at the 5x spike and 80 at the 20x
  one. Under the 2x spike it reaches 320 and drops nothing. After the surge exactly 1k/s
  gets through (api at 83 %). "A burst passes untouched" is the whole point of the
  algorithm, and the api must be sized for it.
- **Leaky bucket**: the api never sees more than 1k/s (peak rho 0.83, no queue, no drops);
  the waiting happens in the bucket instead. A full bucket of 500 at 1k/s is a 0.5 s wait,
  and during the 2-minute 2x spike the bucket stays full the whole time: 122 s of red on
  the limiter, a p99 of 624 ms. Smooth for the server, slow for the client.
- **Fixed window**: each window's first step takes the whole 1000 (10k/s for 100 ms, the
  fluid's version of ch. 4's "edge" burst), so under every spike the api's queue takes a
  surge once a second, each window anew. Under the 5x spike (30 s) and the 20x spike (5 s)
  it fills and overflows: the 9.6k drops are 7.2k and 2.4k, with 18 s and 2 s red. Under
  the 2x spike (2 min) it reaches 240 every second and drops nothing, but it is red for 96 s
  of the 116 s in all.
- **Sliding log**: never more than 1000 in any second, but the log still holds the 80 per
  step admitted just before a spike, so the spike's first step gets only the rest of the
  window, 280; from then on each step admits exactly what left the window ten steps
  earlier. Under a flood the pattern repeats every second: one step of 280 (the api clears
  120), nine of 80. The api's backlog peaks at 160, below its queue limit, and drains in
  0.4 s: no drops, but red for half of every second of a spike (77.5 s in all) and a p99
  of 129 ms. A smaller lump than the fixed window's, a larger one than the sliding counter's.
- **Sliding counter**: the estimate counts the previous window too, so nothing like a
  window's worth enters at once; under a flood it admits a little below the limit (the
  higher reject rate, 15.1 %). The api peaks briefly (backlog 80, about 0.3 s red per
  spike) and the run meets both SLOs.

The p99 column is estimated from a lognormal fitted to the mean and variance of the
response over the run, so a few seconds of 300 ms responses widen it a lot: the token
bucket's 67 ms comes from about 7 s of saturation in 29 minutes.

## Things to try
- `burst: 5000`: each spike gets through for longer: 11.7k drops at the api, 16.5 s red,
  p99 112 ms.
- `burst: 500` (the table above): the first-step surge of 600 requests overflows the api's
  queue; p99 67.3 ms and the SLO fails. With the default `burst: 100` the surge shrinks to
  200 requests: api backlog 80, no drops, p99 30.5 ms, both SLOs met. The bucket's size is
  the burst the api must absorb.
- `algorithm: sliding_log` with `window: 500ms`: the same 1k/s in half-second windows. The
  lump at each window's start shrinks to 180 (api backlog 60, 62 s red), but it comes twice
  as often: p99 118 ms, still a FAIL.
- `simsys sweep --param nodes.limiter.algorithm=... --metric api.utilization` (phase 2c).
