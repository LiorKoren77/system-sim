# Scenario schema v1.2 (phase 2a)

v1 was fitted to the 12 book and lab drafts plus the first tutorial; v1.1 wrote down the
rules the phase 1 loader enforces. **v1.2** is the language as phase 2a reads it: the
loader (`src/config/config.cpp`), the graph builder (`src/engine/graph.cppm`) and the
node behaviours (`src/engine/behaviours.cppm`). All 14 files under `scenarios/` load
under it.

New in v1.2:

- every key of a client's [`rate`](#rate-curve-clientrate) block is read: waves, ramps,
  spikes, points and seeded noise; clock times are offsets from midnight;
- a per-op [`cost`](#node-common-keys) on `service` and `cache`;
- [`load_balancer`](#load_balancer) (`policy`, `vnodes`), [`rate_limiter`](#rate_limiter)
  (`algorithm`, `limit`, `burst`, `window`, `backend`) and [`cache`](#cache) (`hit_ratio`,
  `strategy`, `write_ops`, `backend`, `keys`, `eviction`, `ttl`) are typed and run;
- [rejections](#drops-and-rejections), apart from drops: the SLO metric `reject_rate`,
  the `rejected` column of `run`'s table, the CSV column `rejected_per_s`;
- the top-level `seed` is read (it seeds the noise).

Two kinds of marks:

- † : specified ahead of need; no scenario exercises it yet.
- **Status** column: **read** means `explain`, `estimate` or `run` uses the value;
  **checked** means the loader validates it but nothing uses it yet; **ignored** means the
  key is accepted (a misspelt key is still an error) and its value is not looked at.

## What runs in phase 2a

`simsys validate` runs the loader only. `explain`, `estimate` and `run` also build the
graph, which adds the checks marked *(graph)* below. `explain` works for every node type.
`estimate` and `run` step these types:

| type | runs in phase 2a |
|---|---|
| `client` | the whole rate curve, with its seeded noise |
| `service` | ops, steps, per-op `cost` |
| `load_balancer` | every policy |
| `rate_limiter` | every algorithm, with or without a counter `backend` |
| `cache` | a **fixed** hit ratio, the four synchronous strategies (`cache_aside`, `read_through`, `write_through`, `write_around`), no `then` steps |

Everything else is refused before the first step, with exit code 1:

| what | message | runs from |
|---|---|---|
| a cache with `hit_ratio: che` | `node 'c': hit_ratio che (the Che approximation) is computed from phase 2b on; run/estimate need {fixed: h} until then` | 2b |
| a cache with `strategy: write_back` | `node 'c': strategy write_back (writes flushed later) runs from phase 2b on` | 2b |
| a cache with `then` steps | `node 'c': a cache's then steps run from phase 2b on` | 2b |
| `sql_db`, `cdn` | `node 'x' has type 'cdn', which run/estimate do not support yet (phase 2)` | 2b |
| `blob_store`, `message_queue`, `worker_pool`, `external_sink` | the same message | 2c |
| `kv_cluster`, `ws_gateway`, `service_discovery` | the same message | 3 |
| `search_index`, `id_generator`, `notification_fanout`, `frontier` | the same message | not yet assigned to a phase |
| `faults` | none: the list is accepted and nothing happens | 3 |

The type message says "(phase 2)" for every type; the last column is the plan.

**`estimate` versus `explain`.** `estimate` evaluates the long-run state once, at
t = 0 (00:00): the rate curve's value there, without noise. Its first line says so:
`estimate at t = 0 (00:00): the curve without noise; explain's checks use the peak`.
In `estimate` a rate limiter other than a leaky bucket admits its long-run rate
`min(in, limit)` (a settle pass at t = 0 would otherwise never leave the first window, or
would keep spending a full bucket); a leaky bucket is a queue and settles like one.
`explain`'s `checks` block instead uses each client's **peak** over the run (see
[Rate curve](#rate-curve-clientrate)), and passes at most `limit` on behind a rate limiter.

## Values

| kind | grammar | examples |
|---|---|---|
| duration | non-negative number + `ns us ms s m h`; rounded to whole nanoseconds (below 1 ns is an error) | `250us`, `0.5ms`, `2m` |
| rate | number + optional `k M G` (decimal) + `/s` | `1.5k/s`, `500/s` |
| byte rate | number + optional `K M G` (decimal, capital K) + `B/s` | `500MB/s` |
| size † | `B KB MB GB` | `200B`, `2KB` |
| clock time | `HH:MM`, for `at:` / `peak_at:` / a point's time; hours and minutes after midnight (t = 0) | `20:00`, `"20:00"` |
| count | a positive whole number with an optional `k`, `M` or `G` (× 1e3, 1e6, 1e9), unquoted; no fraction (`1.5k` is an error) | `500`, `10k`, `20M` |
| integer | a plain whole number: no quotes, no `+`, no `0x`, no `3.0`, no `1e3` | `4` |
| number | a plain decimal; `.nan`, `.inf` and `nan`/`inf` are rejected | `0.8`, `1e-3` |

### YAML rules the loader enforces

- **Unknown keys are errors** everywhere: top level, node, `advanced`, step, link, SLO,
  `rate`. A misspelt optional key would otherwise silently fall back to its default.
- **Duplicate keys are errors** in every map (`duplicate key 'x'`, `duplicate node id 'x'`).
  yaml-cpp keeps both entries, and a lookup would see the first while an iteration sees
  both, so the two kinds of reader would disagree about what the file says.
- **Quoted scalars never count as numbers.** `replicas: "4"`, `version: "1"`,
  `factor: "2"` and `max: "0.01"` are strings and are rejected, as a typed language
  rejects the string `"4"` where an int is expected. Unit strings (durations, rates) are
  text anyway and may be quoted or not.
- **A key with nothing after it is "empty"**: `links:` gives "links must not be empty
  (write [] for none)"; the hint appears wherever an empty list or map is valid.
- **Anchors and aliases** (`&steps`, `*steps`) are allowed. The loader caps the expanded
  document at **1,000,000 entries** (maps, lists and scalars, counted once per place
  they are used): a few kilobytes of nested aliases can otherwise expand to millions of
  steps ("billion laughs"). An alias that contains itself is rejected the same way.
- **Merge keys (`<<: *base`) are not supported**: yaml-cpp 0.8 does not implement them,
  so `<<` is an ordinary key and is rejected as unknown (`key '<<' is not valid for type
  'service'`).
- **Clock times**: yaml-cpp reads an unquoted `20:00` as the text `20:00`. YAML 1.1
  readers (PyYAML, for example) read it as the sexagesimal integer 1200, so quote clock
  times (`peak_at: "20:00"`) if the file is also read by other tools.

## Top level

| key | type | required | status | meaning |
|---|---|---|---|---|
| `version` | integer | yes | read | always `1` (unquoted) |
| `name` | string | yes | read | non-empty; shown in reports |
| `seed` | integer | no | read | default 1; within the range of `int`. Printed by `explain`; seeds every client's `rate.noise` (`run --seed N` and `curve --seed N` override it). Client `i` (its index among all nodes, in file order) gets the engine seed `(seed × 0x9E3779B97F4A7C15) XOR i` mod 2^64, so adding a client never changes another client's noise |
| `duration` | duration | yes | read | simulated span of `run`; must be positive |
| `step` | duration | no | read | default `100ms`; must be positive |
| `warmup` | duration | no | read | steps before it are left out of the report; default 0; must be shorter than `duration`, so the report never covers an empty window (`run --duration` obeys the same rule) |
| `datacenters` | map | no | read | see below; default: one datacenter `main` |
| `nodes` | map id → node | yes | read | non-empty; ids are non-empty and unique |
| `links` | list | yes | read | `[]` for none |
| `slo` | list | no | read | |
| `faults` | list | no | ignored | must be a list; contents are typed in phase 3 |

## `datacenters`

A map of datacenter name → settings. The settings map must be `{}`: no keys are defined
yet, so any key inside it is an error (`'us-east' has unknown key 'x'`). If the section is
present it must declare at least one datacenter.

The one reserved name is `latency`: a list of `{between: [a, b], rtt: duration}` †
overriding the cross-datacenter round trip. The loader checks only that it is a list and
otherwise ignores it: every cross-DC link uses the `cross_dc_rtt` default (150 ms).

A node without `dc` lives in the **first declared** datacenter; `main` exists only when
the file declares no datacenters. (The news-feed scenario relies on this.)

## Node (common keys)

| key | type | on | status | meaning |
|---|---|---|---|---|
| `type` | enum | all | read | one of the catalogue below |
| `dc` | string | all | read | a declared datacenter; default as above. Decides whether a link without `latency` is intra- or cross-DC |
| `replicas` | integer > 0 | all | read | N identical copies: each copy sees its share of the demand with the node's full capacity, concurrency and queue limit; backlog and drops are reported summed. How the demand is shared depends on a load balancer in front ([below](#load_balancer)); without one, `demand / N` each. No effect on a client |
| `capacity` | rate or byte rate | all but `client` | read | service rate **per replica**; default per type (§6 of the spec; 100k/s for `load_balancer`, `rate_limiter` and `cache`). A byte rate (`500MB/s`) is read as objects of 1 MB per second. At most `1e15/s` (`1e15B/s` for a byte rate). An error on a `leaky_bucket` limiter |
| `latency` | duration | all but `client` | read | service time of one request; default per type (50 µs for `load_balancer` and `rate_limiter`, 250 µs for `cache`: the spec's memory read). An error on a `leaky_bucket` limiter |
| `advanced` | map | all but `client` | read | see next table |
| `ops` | map | `client`, `service`, `ws_gateway` | read (client, service); checked (ws_gateway) | client: op → share; otherwise op → step list (below) |
| `cost` | map op → number | `service`, `cache` | read | **new in v1.2**: the capacity units one request of that op consumes; an op not listed costs 1. Each value is a positive finite number (`cost 'write' must be a positive finite number`, also for a quoted `"3"`) of at most 1e6 (`cost 'write' must be at most 1e6`); anything but a map is `cost must be a map of op to units`, a bare `cost:` is `cost must not be empty (write {} for none)`. `capacity` is then a rate of units: demand is `Σ rate_op × cost_op`, and the node's demand, drops and backlog are reported in units. What it forwards stays requests (a drained `write` of cost 3 is one `write` downstream). A load balancer and a rate limiter charge 1 per request, and `cost` is not a key of theirs. A `worker_pool`'s `cost` is a different key (a duration per job, phase 2c) |
| `then` | list of steps | `cache`, `worker_pool`, `blob_store`, `notification_fanout`, `frontier` | checked | steps after processing; on a `cache`, steps with `on: miss` receive the flow it did not serve. Validated like any step and drawn by `explain`; `run`/`estimate` refuse a cache's `then` until phase 2b |

A key outside its row is an error, e.g. `key 'capacity' is not valid for type 'client'`
(a client is a traffic source with no service rate, service time or waiting room) or
`key 'then' is not valid for type 'service'` (a service's steps are per op).

### `advanced`

| key | type | default | meaning |
|---|---|---|---|
| `queue_limit` | integer > 0 | 1000 (chosen) | units of work that may wait per replica (requests when every cost is 1; a request of `cost` 3 takes three); beyond it the excess is dropped. An error on a `leaky_bucket` limiter, whose `burst` is its waiting room |
| `concurrency` | integer in [1, 100000] | implied: `max(1, ceil(capacity × latency))`, capped at 100000 | servers per replica, the c of M/M/c. The cap exists because the Erlang C term costs O(c) per node per step. The default is Little's law at full load (busy servers = throughput × time in service), which keeps the rate and the Erlang C term describing one machine. An error on a `leaky_bucket` limiter (one leak slot) |
| `service_variability` | number ≥ 0 | 1.0 (2.0 for `external_sink`) | squared coefficient of variation (SCV) of service time; 1 = exponential. On a `leaky_bucket` limiter it is accepted and replaced by 0 (the leak is deterministic) |
| `burstiness` | number ≥ 0 | 1.0 | SCV of inter-arrival times; 1 = Poisson. With `service_variability` it forms the Allen–Cunneen factor `(burstiness + service_variability) / 2` that scales the queueing wait (Kingman's correction) |
| `yellow` | number in (0, 1] | 0.70 | utilization where the node turns yellow |
| `red` | number in (0, 1] | 0.90 | ... and red. `yellow` must be below `red`; a lone one is compared with the other's default |

`drop_policy` † of v1 is **not accepted** (`unknown advanced key 'drop_policy'`); it
returns when a node type needs a choice of what to shed.

### Steps

A **step** is `{to: <node id>, op: <op name>, on: always|miss|failure, mode: sequential|parallel, factor: <number>}`.

| key | default | status | rule |
|---|---|---|---|
| `to` | required | read | a declared node, **not the node itself** (a retry, the one reason to call yourself, is `client.retries`); *(graph)* not a client (clients only send) |
| `op` | the incoming op | read | required in `then` steps, which have no incoming op to inherit |
| `on` | `always` | read | `miss` applies to the flow a cache/CDN did not serve, `failure` to failed requests. In an op's step list, an `on: miss` step belongs to the nearest unconditional step before it, which must target a `cache` or `cdn` (*(graph)* `node 'web': an 'on: miss' step must follow a cache or cdn step`); it carries the miss fraction `1 − h` of the requests (`h` the cache's fixed hit ratio; a `che` cache or a CDN counts as `h = 0` in `explain` until phase 2b), and costs the response `1 − h` × its hop on average, with the hit/miss spread in the variance. Several `on: miss` steps in a row all belong to the same lookup. A cache step with `factor: k` before a miss step still fires that step for `1 − h` of the requests (a request's lookups are assumed to hit or miss together). `failure` steps never fire before phase 2c |
| `mode` | `sequential` | read | sequential steps add their latencies; **all parallel steps of an op form one group wherever they sit** among the sequential ones, and the group costs its slowest branch |
| `factor` | 1 | read | finite, positive, at most `1e6`; multiplies the flow (fan-out: one post → 100 feed writes) |

An op with an empty step list (`deliver: []`) is terminal: the node serves it and forwards
nothing. *(graph)* Every step needs a link that carries demand from the node to its
target and, if that link lists `ops`, lists the step's op.

## Node types

| type | own keys | status |
|---|---|---|
| `client` | `rate` (rate curve), `ops` (map op → share), `keys` `{zipf: s, space: N}` or `{uniform: N}`, `sizes` (map op → size), `timeout`, `retries` † | `rate` (every term, see [Rate curve](#rate-curve-clientrate)) and `ops` read; `timeout` checked (a duration), not yet used; `keys`, `sizes`, `retries` ignored |
| `cdn` | `ttl`, `hit_ratio` (`fixed: h` or `model` †), `origin` (node id) | `origin` checked (a declared node); others ignored. Runs from phase 2b |
| `load_balancer` | `policy`, `vnodes` | read and run, see [`load_balancer`](#load_balancer) |
| `rate_limiter` | `algorithm`, `limit`, `burst`, `window`, `backend` | read and run, see [`rate_limiter`](#rate_limiter) |
| `service` | `ops`, `cost` | read and run |
| `cache` | `hit_ratio`, `strategy`, `write_ops`, `backend`, `keys`, `eviction`, `ttl`, `then`, `cost` | read and run (fixed hit ratio), see [`cache`](#cache) |
| `sql_db` | `replication_lag`, `shards` (int), `pool` (int connections) † | ignored; runs from phase 2b |
| `kv_cluster` | `nodes` (int), `vnodes`, `replication` N, `write_quorum` W, `read_quorum` R, `mode` `ap|cp` | ignored; phase 3 |
| `message_queue` | `partitions`, `retention` † | ignored; phase 2c |
| `worker_pool` | `consumes` (queue node id), `workers`, `cost` (duration per job), `then` | `consumes` checked; others ignored; phase 2c |
| `blob_store` | `then` (its byte-rate `capacity` is the common key) | ignored; phase 2c |
| `search_index` | `build_interval` | ignored |
| `ws_gateway` | `max_connections`, `heartbeat` (duration), `fanout` (avg recipients per message), `ops` | `ops` checked as step lists; others ignored; phase 3 |
| `service_discovery` | `failover` (duration) | ignored; phase 3 |
| `id_generator` | `algorithm` `snowflake|ticket_server|uuid` | ignored |
| `external_sink` | `failure_rate` (0–1), `politeness` (rate per host) | ignored; phase 2c |
| `notification_fanout` | `channels` (map name → node id), `dedup` (0–1), `then` | ignored |
| `frontier` | `politeness` (rate per host), `hosts` (int), `then` | ignored |

"Ignored" own keys are still checked by name, so a misspelling is caught today; their
values are typed in the phase that simulates the type.

### Client `ops`

A map op → share. Every share is a number in (0, 1] (leave out an op that never happens),
there is at least one op, and the shares sum to 1 within 1e-6. Order does not matter.

### `load_balancer`

Models the load balancer of ch. 1 and the hash ring of ch. 5. The balancer is a queue of its
own (default 100k/s, 50 µs: an L7 proxy hop) and forwards every op it receives over the
links that carry it, split by link `weight`. Its **policy does not change the split**; it
changes how the replicas **behind** it queue (the node's *replica model*).

| key | rule | message on violation |
|---|---|---|
| `policy` | `round_robin` (default), `least_connections`, `weighted`, `consistent_hash` | `policy must be round_robin, least_connections, weighted or consistent_hash` |
| `vnodes` | a positive integer, only with `consistent_hash`; default 150 | `vnodes must be a positive integer`, `vnodes applies only to consistent_hash` |

| policy | replica model of the node behind | queueing |
|---|---|---|
| `round_robin`, `weighted` | independent | N independent M/M/c copies, each at `demand / N` (phase 1's `replicas`) |
| `least_connections` | pooled | one M/M/(c·N) queue with N × the capacity: join-the-shortest-queue, idealised |
| `consistent_hash` | imbalanced | the hottest copy carries `k = 1 + 1/sqrt(vnodes)` × the average share (capped at N) and is queued apart from the N − 1 cool copies, which share the rest; the node reports the hot copy's rho and zone and, as latency, the mixture a request sees (hot with probability k/N) |

`explain` prints the factor on the node behind (`imbalance=1.1 (vnodes 100)`) and a
`hottest copy` figure in its checks. The factor is a chosen heuristic (the mean plus one
standard deviation of a server's share; ch. 5 gives about 10 % at 100 vnodes) and
understates the hottest of many copies. *(graph)* rules:

- the balancer needs an outgoing link (`node 'lb': a load balancer needs an outgoing link`)
  and every op reaching it a link that carries it with a positive weight;
- a node behind two balancers must get **one replica model**, not one policy:
  `round_robin` and `weighted` agree; `round_robin` and `least_connections` do not
  (`node 's' is behind load balancers with different replica models ('a' independent, 'b' pooled)`).
  Two rings with different `vnodes` keep the larger imbalance;
- a drained route (weight 0) imposes no model;
- a node reached both through a balancer and directly gets the balancer's model for all
  its traffic: the model belongs to the replica group, not to one path into it.

Not modelled: health checks and failover, sticky sessions, L4 versus L7, TLS set-up, and
per-key routing (a `consistent_hash` balancer splits by weight like the others; only its
imbalance reaches the node behind).

### `rate_limiter`

Models the rate limiter of ch. 4. The limiter is a queue of its own (default 100k/s, 50 µs:
an in-memory counter check) that sees **every** request. What it admits is forwarded like a
load balancer's flow (by link weight, over every link but the backend's); what it refuses
is **rejected**: answered at once (the book's 429), never forwarded, and counted apart from
drops ([below](#drops-and-rejections)).

| key | rule | message on violation |
|---|---|---|
| `limit` | required; a rate, positive, at most `1e15/s`. The limiter's total, shared by its replicas | `rate_limiter needs a limit`, `limit must be positive`, `limit must be at most 1e15/s` |
| `algorithm` | `token_bucket` (default), `leaky_bucket`, `fixed_window`, `sliding_log`, `sliding_counter` | `algorithm must be token_bucket, leaky_bucket, fixed_window, sliding_log or sliding_counter` |
| `window` | a positive duration, default `1s`. A windowed algorithm's window; on a bucket it only sizes the default `burst` | `window must be positive` |
| `burst` | a positive integer, **buckets only**; default `limit × window` rounded up (one window's worth) | `burst must be a positive integer`, `burst applies only to token_bucket and leaky_bucket`, `limit x window is too large for a burst; give burst explicitly` |
| `backend` | a declared node of type `cache`: the counter store | `backend refers to unknown node 'x'`, `backend must be a cache` |

A `leaky_bucket` refuses `capacity`, `latency`, `advanced.queue_limit` and
`advanced.concurrency`: `leaky_bucket takes limit and burst; capacity, latency, concurrency
and queue_limit are not valid`. Its `advanced.service_variability` is accepted and replaced
by 0.

Each algorithm keeps the state ch. 4 describes, stepped once per step `[t, t + dt)`, with
W = `limit × window` and `in` the inflow in requests per second:

| algorithm | state and admission per step | under a flood |
|---|---|---|
| `token_bucket` | a level `L` in [0, `burst`], starting **full**: `admitted = min(in, limit + L/dt)`, then `L += (limit − admitted)·dt`, clamped | a burst of up to `burst` extra passes, then exactly `limit` |
| `fixed_window` | `used` resets when `floor(t / window)` changes; admits `min(in·dt, W − used)/dt` | the window's first step takes all of W; a flood across a boundary gets 2W (ch. 4's edge burst) |
| `sliding_log` | a ring of the admitted counts of the last `ceil(window/dt)` steps; admits `min(in·dt, W − Σ of the previous ceil(window/dt) − 1 steps)/dt` | never more than W in any window |
| `sliding_counter` | `estimate = prev·(1 − f) + cur`, `f = (t mod window)/window`; admits up to `W − estimate` | about 10/11 of `limit` at ten steps per window |
| `leaky_bucket` | the limiter's own queue becomes the bucket: one deterministic leak slot (service time `1/limit`, concurrency 1, SCV 0: M/D/1) with `queue_limit` = `burst`; with N replicas each copy leaks `limit/N` and holds `burst/N` | excess **waits**; a full bucket's overflow is rejected |

**Backend.** With a `backend`, every request the limiter checks, admitted or refused, sends
one `counter` request there, and the hop (two link legs + the store's response) adds to
an admitted request's response. *(graph)* The backend needs a link from the limiter
(`node 'rl': backend 'c' needs a link from the limiter`) that carries op `counter`
(`link rl -> c does not carry op 'counter' (ops: x)`).

**Terminal gate.** A limiter whose only outgoing link is its backend answers every admitted
request itself. Its caller still calls its own later steps at the full rate; the refused
share shows in the gate's reject rate and in the client's answered rate.

Not resolved: anything inside a step (a window boundary inside a step is seen at the next
step's start; a window shorter than the step is not resolved; a burst is spread over its
step). Not modelled: per-client keys (one aggregate limit), races between limiter
replicas, rate-limit headers and retries of rejected requests, soft limits.

### `cache`

Models the cache tier of ch. 1 and its uses in ch. 8 and 11–14. The cache is a queue of its
own (default 100k/s, 250 µs) that answers every request it receives, hit or miss, at its
`cost` per op.

| key | rule | message on violation |
|---|---|---|
| `hit_ratio` | required: `{fixed: h}` with `0 ≤ h ≤ 1`, or `che` (computed in phase 2b; `run`/`estimate` refuse it until then) | `cache needs a hit_ratio`, `hit_ratio must be che or {fixed: h}`, `hit_ratio.fixed must be between 0 and 1` (also for a quoted `"0.5"`) |
| `strategy` | `cache_aside` (default), `read_through`, `write_through`, `write_around`, `write_back` | `strategy must be cache_aside, read_through, write_through, write_around or write_back` |
| `backend` | a declared node; required by every strategy but `cache_aside` (allowed and unused there) | `strategy read_through needs a backend`, `backend refers to unknown node 'x'`; *(graph)* `backend 'db' needs a link from the cache`, and the link must carry every op the cache passes on (`link c -> db does not carry op 'read' (ops: write)`) |
| `write_ops` | a non-empty list of op names; default `[write]`. Every other op is a read | `write_ops must be a list of op names`, `write_ops must name at least one op` |
| `keys` | a [count](#values) (`20M`), for the Che approximation | `keys must be a positive integer` |
| `eviction` | `lru`, `lfu`, `fifo`, `ttl` | `eviction must be lru, lfu, fifo or ttl` |
| `ttl` | a positive duration | `ttl must be positive` |

`keys`, `eviction` and `ttl` are read and printed by `explain`; phase 2b uses them.
What each strategy passes to its `backend`, as the same op:

| strategy | reads | writes |
|---|---|---|
| `cache_aside` | nothing: the application's `{to: db, on: miss}` step reads the store for `1 − h` of the requests | nothing (the application writes the store) |
| `read_through` | `1 − h` of each read; a read's response is the mixture `{h: own, 1 − h: own + hop}` | nothing |
| `write_through` | nothing (a miss is the application's `on: miss` step) | every write, synchronously: own + hop |
| `write_around` | as `write_through` | as `write_through`: the same flow. The difference is what the cache holds afterwards, which shows in the hit ratio, and `h` is fixed here |
| `write_back` | refused by `run`/`estimate` until phase 2b (a queue of dirty entries flushed later) | |

Not modelled: key popularity (h is a number, not a function of the cache size and the key
skew), TTL expiry, eviction order, a cold start, the thundering herd of a hot key's misses,
invalidation traffic.

## Rate curve (`client.rate`)

```yaml
rate:
  base: 10k/s                                # or points; exactly one of the two
  wave:   {shape: sine|square|diurnal, period: 24h, amplitude: 0.6, peak_at: 20:00}
  ramp:   {from: 0/s, to: 10k/s, over: 5m}  # †
  spikes: [{at: 12:00, for: 10m, factor: 3}]
  points: [[0s, 1k/s], [1h, 5k/s]]          # replaces base †
  noise:  {sigma: 0.15, correlation: 5m}    # †
```
All keys are **read**: the loader types them, `explain` and `simsys curve` print them, and
`run` offers the curve (with its noise) at every step. `estimate` evaluates it once, at t = 0, without noise
(see [What runs](#what-runs-in-phase-2a)).

Composition: `rate(t) = level(t) × wave(t) × Π active spike factors × noise(t)`, where
`level` is the ramp's rate `from + (to − from) × min(t / over, 1)` when there is a ramp,
else `points` interpolated linearly (held flat before the first and after the last entry),
else `base`. (The ramp is the spec's multiplier `ramp/base` times `base`, applied as the
level itself: the same numbers, without a division that overflows for a tiny base.) Every
accepted curve is finite: each level is at most `1e15/s`, the wave at most 2× and the
product of the spikes active at any moment at most `1e6`. t = 0 is midnight: a time point (`peak_at`, `at`, a point's time) is a
clock time `HH:MM` (quoted or not) or a duration from the start (`5m`).

| Key | Rule | Message on violation |
|---|---|---|
| `base` / `points` | exactly one | `client needs rate.base or rate.points`, `rate.base and rate.points are alternatives` |
| `base` | a rate, at most `1e15/s` | `rate.base must be at most 1e15/s` |
| `points` | a non-empty list of `[time, rate]` pairs, sorted by time (equal times make a step), rates at most `1e15/s` | `rate.points must have at least one entry`, `rate.points entries are [time, rate] pairs`, `rate.points must be sorted by time` |
| `wave` | all of `shape` (`sine`, `square`, `diurnal`), `period` > 0, `amplitude` in [0, 1], `peak_at` | `rate.wave needs shape, period, amplitude and peak_at`, `rate.wave.amplitude must be between 0 and 1`, ... |
| `ramp` | all of `from`, `to` (rates, at most `1e15/s`), `over` > 0; needs a `base` above `0/s` and no `points`. The ramp sets the level, so `base` is not used in the arithmetic; the rule is kept so a file always states the client's nominal rate | `rate.ramp needs from, to and over`, `rate.ramp needs rate.base`, `rate.ramp.to must be at most 1e15/s` |
| `spikes` | a list; each spike has `at`, `for` > 0 and `factor` in (0, 1e6]; the factors of the spikes active at any one moment multiply to at most `1e6` | `rate.spikes[2].factor must be positive` (numbered from 1), `rate.spikes: overlapping factors multiply to more than 1e6` |
| `noise` | `sigma` in [0, 10] (stationary standard deviation), `correlation` > 0 (τ) | `rate.noise needs sigma and correlation`, `rate.noise.sigma must be at most 10` |

Every block rejects unknown keys (`unknown rate.wave key 'colour'`,
`unknown rate.spikes[1] key 'why'`). A numeric key says what is wrong with its value:
`rate.wave.amplitude must not be empty` (a bare key), `... must be a number` (text, or a
quoted `"0.5"`), or its range message (`.nan` and `.inf` included). `rate` itself is a map;
the phase-1 spelling `rate: 100/s` gives `rate must be a map (write rate: {base: 100/s})`.

`explain`'s `checks` block uses each client's **peak** rate over the run (the curve sampled
every minute, plus every spike start and point time; no noise), summed as if every client
peaked at once: the safe side of a back-of-the-envelope check ("peak QPS", ch. 2). The
header names the span: `checks (peak offered load over the first 24h, no queueing):`.

## Links

`{from, to, dir: in|out|inout (default out), ops: [..] (default all), latency, bandwidth †, weight}`.

| key | status | rule |
|---|---|---|
| `from`, `to` | read | declared nodes, **different** from each other (a node does not call itself over the network) |
| `dir` | read | the direction **demand** travels. `out`: `from` sends to `to`. `in`: `to` sends to `from`; the link is written from the receiver's side. `inout`: like `out`, and replies flow back on the same link (the normal request/response case). The engine charges every hop a request leg and a reply leg (2 × one-way) whatever `dir` says |
| `ops` | read | a non-empty list of distinct op names; absent means all ops. `ops: []` is an error (it would load as "all", the opposite of what it says) |
| `latency` | read | **one way**. Default: half the round trip, `intra_dc_rtt / 2` = 250 µs inside a datacenter, `cross_dc_rtt / 2` = 75 ms across |
| `bandwidth` † | ignored | byte flows arrive with `blob_store` (phase 2c) |
| `weight` | read | finite, ≥ 0, at most 1e9; default 1. When several links from a **client, load balancer or rate limiter** carry the same op, the op is split `weight_i / Σ weight` over them. 0 means a drained backend: the route carries nothing, so it brings no op to its target, closes no call cycle and imposes no replica model (a balancer's policy, below). *(graph)* An op whose every link has weight 0 is an error (`client 'u': every link carrying op 'read' has weight 0`; `node 'lb': ...` for a balancer). A service's steps and a cache's or limiter's backend name their target, so a weight there has no effect |

**One link per pair and demand direction.** A second link between the same two nodes in
the same demand direction is rejected ("duplicate link (put several ops or a weight on
one link)"); `{from: a, to: b}` and `{from: b, to: a, dir: in}` are the same direction.
Several ops go in one link's `ops`.

**Delay lines** *(graph)*: a link delays its flow by `round(one_way / step)` steps, halves
rounded away from zero (this replaces the spec's original `ceil` rule, see spec §16).
At the default 100 ms step a 250 µs hop passes in the same step, a 75 ms cross-DC hop
arrives one step late. A link that closes a loop of nodes is delayed at least one step.
A link longer than **1,000,000 steps** is an error: use a coarser `step`.

## SLO

`{metric, node, op, max}`; `metric` and `max` are required.

| metric | `node` | `op` | `max` | evaluates |
|---|---|---|---|---|
| `p50_latency`, `p95_latency`, `p99_latency` | required, a **client** | optional, one the client sends | duration | the client's estimated end-to-end percentile over the **answered** requests; without `op`, the worst of its ops |
| `drop_rate` | optional | not allowed | number in [0, 1] | the node's dropped / the work sent to it, after warmup; without `node`, the worst node's |
| `reject_rate` | optional | not allowed | number in [0, 1] | **new in v1.2**: the node's rejected (a rate limiter's refusals, a leaky bucket's overflow included) / the requests sent to it, after warmup; without `node`, the worst node's |
| `utilization` | required, not a client | not allowed | number in [0, 1] | the node's mean busy fraction (drain / capacity) after warmup |
| `queue_lag` | optional | not allowed | duration | reported `skipped` until `message_queue` runs (phase 2c) |
| `staleness` | optional | not allowed | duration | reported `skipped` until replication lag is modelled (`sql_db`, phase 2b) |

The kind of `max` follows the metric, so `0.01` can never be read as 0.01 seconds
(`slo: reject_rate max must be between 0 and 1`).
`op` is only valid for latency metrics (`slo: op is only valid for latency metrics`): drop
rate, reject rate and utilization are measured per node, not per op.

### Drops and rejections

Two different promises, kept apart on purpose:

- a **drop** is a node **failing**: its waiting room was full, so the excess is shed.
  `drop_rate` = Σ dropped / Σ demand **of that node** (in capacity units when the node has
  a `cost`), so it stays in [0, 1] even when fan-out multiplies the clients' requests
  before they reach it (one post → 100 feed writes would make "dropped anywhere / offered
  by clients" 90). Without `node` it is the worst node's, since every node must keep the
  bound.
- a **rejection** is a rate limiter's **policy working** (ch. 4's 429): what a token bucket or a
  window algorithm refuses, and a leaky bucket's overflow. `reject_rate` = Σ rejected / Σ
  demand of that node, with the same rules.

Neither kind of request is answered, so neither contributes latency: each step's response
is weighted by the client's offered rate times the fraction its path **delivers** (a
node's own served / inflow, times its downstream's: sequential parts multiply, splits mix
by share). `run`'s table header shows `drop rate`, and `reject rate X% (worst: node)` once
some node refused anything; the node table has `dropped` and `rejected` columns; JSON has
`drop_rate`, `worst_drop_node`, `reject_rate`, `worst_reject_node` and per node `dropped`,
`rejected`.

`run --series` writes one CSV row per node per step:
`t_s,node,rho,backlog,drops_per_s,rejected_per_s,latency_ms,zone` (`rejected_per_s` new in
v1.2; a client row has zeros and zone `none`).
When no step was observed every SLO is `skipped` and `run` exits 1: no data is an error,
not a pass.

## Faults

`{at: clock-or-duration, for: duration, kind: node_down|dc_down †|partition|latency_spike †|traffic_spike †|cache_flush †, node|dc|between †, fraction, factor †}`.

`partition` takes either `between: [dc-a, dc-b]` † (cut two datacenters apart) or `node: <id>, fraction: <0–1>`
(split a clustered node into two groups of the given proportion that cannot see each other;
`fraction: 0.5` is two halves).

The loader checks only that `faults` is a list; faults are typed and simulated in phase 3.
