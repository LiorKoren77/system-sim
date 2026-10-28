# Scenario schema v1.3 (phase 2b)

v1 was fitted to the 12 book and lab drafts plus the first tutorial; v1.1 wrote down the
rules the phase 1 loader enforces; v1.2 added phase 2a's rate curves, balancers, limiters and
fixed-ratio caches. **v1.3** is the language as phase 2b reads it: the loader
(`src/config/config.cpp`), the graph builder (`src/engine/graph.cppm`), the node behaviours
(`src/engine/behaviours.cppm`) and the reports (`src/report/report.cpp`). All 14 files under
`scenarios/` load under it, and six of them run end to end ([below](#what-runs-in-phase-2b)).
The design decisions behind the new rules, with what each ignores, are spec §18
(`docs/superpowers/specs/2026-10-27-system-sim-design.md`).

New in v1.3:

- a client's [`keys`](#node-types) (`{zipf: s, space: N}` or `{uniform: N}`) and
  [`hit_ratio: che`](#che-hit-ratios) on a cache or CDN: the hit ratio is computed at build with
  the Che approximation from the cache's `keys` and the key distribution of the clients whose
  ops reach it; behind another `che` cache the popularity is the front cache's misses (filtered
  popularity), and behind a `consistent_hash` balancer the replicas split the key space over
  uneven arcs;
- a cache's [`then` steps](#then-steps-on-a-cache) (`on: always` and `on: miss`) and the
  `write_back` strategy, whose flush to the store is an asynchronous hop;
- [`sql_db`](#sql_db): a primary and `replicas − 1` read replicas, `shards`, a connection
  `pool` that sets `c` and caps the rate by Little's law, `replication_lag`, `write_ops`, and a
  `cost` whose write ops default to **5** (spec §6: 5k reads/s or 1k writes/s per machine);
  replicas replay the writes the primary committed; the `staleness` SLO (primary sojourn + lag +
  replica sojourn) fails when a replica misses committed writes;
- [`cdn`](#cdn): a read-only cache in front of a required `origin`, which fetches its own misses
  as the same op; `hit_ratio` fixed, `che` or none; `write_ops` names the ops it refuses;
- [`id_generator`](#id_generator): `algorithm: snowflake | ticket_server | uuid`, a `service`
  whose capacity and service time the algorithm supplies;
- the CSV columns `demand_per_s` and `drain_per_s`, the second series file
  [`run --series-ops`](#drops-and-rejections) (`offered_per_s`, `answered_per_s`, `mean_ms` per
  client op), and JSON's `answered_per_s`, `staleness_s`, `replication_dropped_per_s`,
  `committed_writes_per_s` and the SLO `note`; `answered` is offered minus what was lost
  (dropped or rejected), so a request waiting in a growing queue still counts;
- `explain`'s marks: a node whose load comes only through a type that does not propagate it yet
  is marked `(load not propagated: ...)` with no percentage, a node that is partly fed so gets
  its percentage as a lower bound `(≥p%; ...)` ([below](#what-runs-in-phase-2b)), and a node
  whose capacity is a byte rate is checked in MB/s (from phase 2c; see `capacity` in
  [Node (common keys)](#node-common-keys));
- the runnable set: `tutorial/hello`, `tutorial/chain`, `labs/rate-limiter`,
  `labs/consistent-hashing`, `labs/unique-id` and `book/url-shortener`.

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

## What runs in phase 2b

`simsys validate` runs the loader only. `explain`, `estimate` and `run` also build the
graph, which adds the checks marked *(graph)* below. `explain` works for every node type.
`estimate` and `run` step these types:

| type | runs in phase 2b |
|---|---|
| `client` | the whole rate curve, with its seeded noise |
| `service` | ops, steps, per-op `cost` |
| `load_balancer` | every policy |
| `rate_limiter` | every algorithm, with or without a counter `backend` |
| `cache` | a fixed or [`che`](#che-hit-ratios) hit ratio, every strategy (`write_back` with an asynchronous flush), [`then` steps](#then-steps-on-a-cache) |
| `sql_db` | a primary and read replicas, `shards`, `pool`, `replication_lag` and the `staleness` SLO, see [`sql_db`](#sql_db) |
| `cdn` | a fixed or `che` hit ratio (or none: every read misses), misses fetched from its `origin`, see [`cdn`](#cdn); with a byte `capacity` it counts megabytes (phase 2c, see `capacity` in [Node (common keys)](#node-common-keys)) |
| `blob_store` | every op, counting megabytes (phase 2c, see `capacity` in [Node (common keys)](#node-common-keys)): its queue waits for whole objects, a write (`write_ops`) is answered at its last byte, a read at its first megabyte; not with `then` steps |
| `id_generator` | as a `service` whose capacity and service time its `algorithm` supplies, see [`id_generator`](#id_generator) |

Everything else is refused before the first step, with exit code 1:

| what | message | runs from |
|---|---|---|
| `message_queue`, `worker_pool`, `external_sink` | `node 'x' has type 'message_queue', which run/estimate do not support yet (phase 2)` | 2c |
| `kv_cluster`, `ws_gateway`, `service_discovery` | the same message | 3 |
| `search_index`, `notification_fanout`, `frontier` | the same message | not yet assigned to a phase |
| a `blob_store` with `then` steps | `node 'x': a blob store's then steps are event notifications, which run/estimate do not support yet (phase 2c)` | 2c |
| a `cdn` with `hit_ratio: che` and no `keys` | `node 'x': hit_ratio che needs keys (the cdn's size in objects)` | now, once it states `keys` |
| `faults` | none: the list is accepted and nothing happens | 3 |

The type message says "(phase 2)" for every type; the last column is the plan. The first
node in file order that cannot run is the one named, and the type check comes before the CDN
and blob store checks. Of the 14 shipped scenarios six run: `tutorial/hello`, `tutorial/chain`,
`labs/rate-limiter`, `labs/consistent-hashing`, `labs/unique-id` (which fails its `drop_rate` SLO
on purpose, exit 2) and `book/url-shortener`.

**`estimate` versus `explain`.** `estimate` evaluates the long-run state once, at
t = 0 (00:00): the rate curve's value there, without noise. Its first line says so:
`estimate at t = 0 (00:00): the curve without noise; explain's checks use the peak`.
In `estimate` a rate limiter other than a leaky bucket admits `min(in, limit)` (a settle
pass at t = 0 would otherwise never leave the first window, or would keep spending a full
bucket); a leaky bucket is a queue and settles like one. `min(in, limit)` is the long-run
rate of the token bucket and of the fixed window, but **not** of the sliding counter, which
admits less under a flood (about 10/11 of `limit` at ten steps per window). And a long-run
rate says nothing about the shape inside a window: under a flood the fixed window lets all
of W through in each window's first step and nothing after, so `run`'s p99 there is far
above what `estimate` shows. When the scenario has a windowed limiter, `estimate`'s first
line adds `; windowed limiters are evaluated at min(in, limit) — run them to see the
sawtooth`.
`explain`'s `checks` block instead uses each client's **peak** over the run (see
[Rate curve](#rate-curve-clientrate)), and passes at most `limit` on behind a rate limiter.

**Load `explain` cannot follow yet.** The checks follow the load as the engine would: from
the clients' routes, along `ops` steps, through load balancers and rate limiters, and on from
caches and CDNs (their misses, their store, a cache's `then`). They do **not** follow yet a
`blob_store`'s or a `worker_pool`'s `then`, nor a `message_queue`'s consumers (phase 2c), nor
the `then` of a `notification_fanout` or a `frontier`. A node whose load can only come through one of those
gets 0 on paper: a number that is missing, not zero. Its check line therefore has no
percentage and no zone, but a mark naming each such type on the way to it, nearest first:
`transcode_q  peak offered 0/s vs 1M/s total capacity  (load not propagated: blob_store is
phase 2c)`, or `dns ... (load not propagated: worker_pool is phase 2c, frontier is not modelled
yet)`. The mark follows such a node's `then` steps (a message queue's links, which is all it
has), not every link it has. A node that also receives load the checks do follow gets its
percentage as a **lower bound**, with no zone unless even that is red (the real load is higher,
so green or yellow might not hold, but red does): `metadata_db ... (≥0.559%; load not
propagated: worker_pool, message_queue and blob_store are phase 2c)`, `(≥150%, red; ...)`. A check
that is already `not checked` (a `kv_cluster`'s) keeps that mark and adds the bound after it:
`message_store ... (not checked: kv_cluster nodes, replication and quorums: phase 3)  (lower
bound; load not propagated: worker_pool and message_queue are phase 2c)`.
A lower-bound line has no per-copy figures (a database's primary/replica split, a ring's hottest
copy): they would be lower bounds too.

## Values

| kind | grammar | examples |
|---|---|---|
| duration | non-negative number + `ns us ms s m h`; rounded to whole nanoseconds (below 1 ns is an error) | `250us`, `0.5ms`, `2m` |
| rate | number + optional `k M G` (decimal) + `/s` | `1.5k/s`, `500/s` |
| byte rate | number + optional `K M G` (decimal, capital K) + `B/s`; or, as a network is rated, number + optional `k M G` + `bit/s`, read as bytes ÷ 8 | `500MB/s`, `40Gbit/s` (= 5e9 B/s) |
| size | non-negative number + `B KB MB GB`, decimal (1 KB = 1000 B, as the book and cloud vendors count; `KiB` and `kB` are errors), at most 1e15 B | `200B`, `2KB`, `1.5GB` |
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
| `seed` | integer | no | read | default 1; within the range of `int`. Printed by `explain`; seeds every client's `rate.noise` (`run --seed N` and `curve --seed N` override it). Client `i` (its index among all nodes, in file order) gets the engine seed `(seed × 0x9E3779B97F4A7C15) XOR i` mod 2^64, so appending a node at the end of the file never changes another client's noise (inserting one before a client shifts that client's index, and so its noise) |
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
| `replicas` | integer > 0 | all | read | N identical copies: each copy sees its share of the demand with the node's full capacity, concurrency and queue limit; backlog and drops are reported summed. How the demand is shared depends on a load balancer in front ([below](#load_balancer)); without one, `demand / N` each. On a `sql_db` the copies are one primary and `N − 1` read replicas, split by op whatever stands in front (see [`sql_db`](#sql_db)); on a `ticket_server` id generator it must be 1. No effect on a client |
| `capacity` | rate or byte rate | all but `client` | read | service rate **per replica**; default per type (§6 of the spec; 100k/s for `load_balancer`, `rate_limiter` and `cache`). A byte rate (`100GB/s`, or in bits as a network is rated: `40Gbit/s` = 5 GB/s, printed `5GB/s`) on a `cdn` or `blob_store`, and a `blob_store`'s default (spec §6's 500 MB/s), makes the node count **megabytes**: its capacity is that many MB/s, a request costs its size in MB (the client's `sizes:` or the step's `size:`), and its service time is one megabyte's (10 ms at a CDN, 40 ms at a blob store), so by Little's law it has `c = MB/s × that time` megabyte slots: `capacity=100k MB/s (scenario)  service_time=10ms per MB (default)  c=1000 MB slots (implied)  queue_limit=1000 MB (default)`. Each request is a **batch** of megabytes that holds one slot for its whole transfer (size × the time of one MB: a 50 MB object 2 s at a blob store), so the node's queue waits for whole objects: Erlang C (M/M/c) is fed the rate-weighted mean object time `s_obj = Σ rate_op × MB_op × time per MB / Σ rate_op` (recomputed every step from the arrivals) with the same `c`, which leaves `rho` as in MB/s and makes the wait a whole object's. At `rho` 0.9, 50 MB objects at a default blob store (20 slots) wait `C(20, 0.9) × 2 s / (20 × 0.1)` = 0.55 s, where 1 MB units would say 11 ms. A request is then answered after that wait at its **last byte** if it is a write (an op in a blob store's `write_ops`, default `write`): its whole transfer, a 300 MB upload at 40 ms per MB 12 s; and at its **first megabyte** otherwise (a read or a stream, which the client consumes as it arrives; this understates a large read needed whole before use). Either way a request under 1 MB takes one megabyte's time (an upper bound for small objects). Ignored: the spread of object sizes in one queue (M/M/c at their mean; mixed sizes wait longer). `explain` checks it in MB/s with the requests beside: `cdn_us  peak offered 14.08k MB/s (281.6/s) vs 100k MB/s total capacity  (14.1%, green)`. A byte rate on any other type is an error, since those types count requests: `a service counts requests; a byte capacity belongs on a cdn or blob_store`. At most `1e15/s` (`1e15B/s` for a byte rate). An error on a `leaky_bucket` limiter |
| `latency` | duration | all but `client` | read | service time of one request; default per type (50 µs for `load_balancer` and `rate_limiter`, 250 µs for `cache`: the spec's memory read). An error on a `leaky_bucket` limiter |
| `advanced` | map | all but `client` | read | see next table |
| `ops` | map | `client`, `service`, `ws_gateway` | read (client, service); checked (ws_gateway) | client: op → share; otherwise op → step list (below) |
| `cost` | map op → number | `service`, `cache`, `sql_db` | read | **new in v1.2**: the capacity units one request of that op consumes; an op not listed costs 1. Each value is a positive finite number (`cost 'write' must be a positive finite number`, also for a quoted `"3"`) of at most 1e6 (`cost 'write' must be at most 1e6`); anything but a map is `cost must be a map of op to units`, a bare `cost:` is `cost must not be empty (write {} for none)`. `capacity` is then a rate of units: demand is `Σ rate_op × cost_op`, and the node's demand, drops and backlog are reported in units. What it forwards stays requests (a drained `write` of cost 3 is one `write` downstream). A `sql_db` charges its write ops 5 unless `cost` lists them (see [`sql_db`](#sql_db)). A load balancer and a rate limiter charge 1 per request, and `cost` is not a key of theirs. A `worker_pool`'s `cost` is a different key (a duration per job, phase 2c) |
| `then` | list of steps | `cache`, `worker_pool`, `blob_store`, `notification_fanout`, `frontier` | read (cache); checked (others) | steps after processing; on a `cache`, steps with `on: miss` receive the flow it did not serve. Validated like any step; a cache's `then` steps run (an unconditional one carries everything the cache answered, an `on: miss` one `1 − h` of the reads and none of the writes; see [`then` steps on a cache](#then-steps-on-a-cache)), and `explain`'s paper load follows them the same way |

A key outside its row is an error, e.g. `key 'capacity' is not valid for type 'client'`
(a client is a traffic source with no service rate, service time or waiting room) or
`key 'then' is not valid for type 'service'` (a service's steps are per op).

### `advanced`

| key | type | default | meaning |
|---|---|---|---|
| `queue_limit` | integer > 0 | 1000 (chosen) | units of work that may wait per replica (requests when every cost is 1; a request of `cost` 3 takes three); beyond it the excess is dropped. On a node that counts megabytes (a byte `capacity`, see [Node (common keys)](#node-common-keys)) the units are MB: the default waiting room there is 1000 MB (chosen: the generic default read in the node's unit, twenty 50 MB streams), printed `queue_limit=1000 MB`. An error on a `leaky_bucket` limiter, whose `burst` is its waiting room |
| `concurrency` | integer in [1, 100000] | implied: `max(1, ceil(capacity × latency))`, capped at 100000 | servers per replica, the c of M/M/c. The cap exists because the Erlang C term costs O(c) per node per step. The default is Little's law at full load (busy servers = throughput × time in service), which keeps the rate and the Erlang C term describing one machine. An error on a `leaky_bucket` limiter (one leak slot) |
| `service_variability` | number ≥ 0 | 1.0 (2.0 for `external_sink`) | squared coefficient of variation (SCV) of service time; 1 = exponential. An error on a `leaky_bucket` limiter (the leak is deterministic: SCV 0) |
| `burstiness` | number ≥ 0 | 1.0 | SCV of inter-arrival times; 1 = Poisson. With `service_variability` it forms the Allen–Cunneen factor `(burstiness + service_variability) / 2` that scales the queueing wait (Kingman's correction) |
| `yellow` | number in (0, 1] | 0.70 | utilization where the node turns yellow |
| `red` | number in (0, 1] | 0.90 | ... and red. `yellow` must be below `red`; a lone one is compared with the other's default |

`drop_policy` † of v1 is **not accepted** (`unknown advanced key 'drop_policy'`); it
returns when a node type needs a choice of what to shed.

### Steps

A **step** is `{to: <node id>, op: <op name>, on: always|miss|failure, mode: sequential|parallel, factor: <number>, size: <size>}`.

| key | default | status | rule |
|---|---|---|---|
| `to` | required | read | a declared node, **not the node itself** (a retry, the one reason to call yourself, is `client.retries`); *(graph)* not a client (clients only send) |
| `op` | the incoming op | read | required in `then` steps, which have no incoming op to inherit |
| `on` | `always` | read | `miss` applies to the flow a cache did not serve, `failure` to failed requests. In an op's step list, an `on: miss` step belongs to the nearest unconditional step before it, its lookup, which must ask a `cache` for a read, leave the miss to the application, and be waited for. *(graph)* `node 'web': an 'on: miss' step must follow a cache step`; `... cannot follow a write to cache 'c' (writes do not miss)`; `... cannot follow read_through cache 'c' (the cache already fetched the miss)`; `... cannot follow cdn 'edge' (the cdn already fetched the miss from its origin)` (a CDN fetches its own misses, see [`cdn`](#cdn)); `... and the cache step it follows must be sequential (the miss step waits for the cache's answer)`. A miss step is taken by the miss fraction `1 − h` of the requests (`h` the cache's hit ratio, stated or [resolved at build](#che-hit-ratios); a cache built in code without one counts as `h = 0`, and `explain` marks the checks that rest on it). After a lookup of `factor: k` it sends `k(1 − h)` requests per request: every missed lookup is fetched. Several `on: miss` steps after one lookup form one block that a miss takes whole, one step after another: the response adds `1 − h` × the sum of their hops on average, with the hit/miss spread in the variance, and a miss is answered only if every fetch is. (For latency, a request's `k` lookups are assumed to hit or miss together.) `failure` steps never fire before phase 2c |
| `mode` | `sequential` | read | sequential steps add their latencies; **all parallel steps of an op form one group wherever they sit** among the sequential ones, and the group costs its slowest branch |
| `factor` | 1 | read | finite, positive, at most `1e6`; multiplies the flow (fan-out: one post → 100 feed writes) |
| `size` | none | checked | a [size](#values): the payload this step's request carries (a transcoder's 50 MB rendition), what a byte-sized node charges the request, in MB; positive (`step size must be a size (B, KB, MB or GB: 50MB): missing size unit`, the parser's reason after the colon; `step size must be positive` for `0B`) |

An op with an empty step list (`deliver: []`) is terminal: the node serves it and forwards
nothing. *(graph)* Every step needs a link that carries demand from the node to its
target and, if that link lists `ops`, lists the step's op.

**Request sizes** *(graph, phase 2c)*. Each (node, op) gets one size at build, from the
client's `sizes` or a step's `size`: a request keeps its size while it keeps its op name
(through balancers, limiters, a cache's or CDN's forwarding to its store or origin, a message
queue's links to its worker pools, and any step or `then` step that does not rename the op), and
a step that renames the op without `size` sends an unsized request. Two different sizes for one
(node, op) are an error at any node, `node 'store': op 'get' arrives with two sizes, 1MB from 'a'
and 2MB from 'b'`, and so is an unsized op reaching a node that counts bytes (a `cdn` or
`blob_store` whose capacity is a byte rate, or a `blob_store` with no `capacity`): `node 'store'
counts bytes (capacity 500MB/s, default), but op 'fetch' from 'svc' has no size: add size: to the
step or sizes: to the client`. A node that counts requests needs no size; `explain` prints a
byte-sized node's sizes, `sizes: stream 50MB`.

## Node types

| type | own keys | status |
|---|---|---|
| `client` | `rate` (rate curve), `ops` (map op → share), `keys` `{zipf: s, space: N}` or `{uniform: N}`, `sizes` (map op → size), `timeout`, `retries` † | `rate` (every term, see [Rate curve](#rate-curve-clientrate)) and `ops` read; `keys` read by the [`che` caches](#che-hit-ratios) downstream (`keys must be a map`, `keys needs zipf and space, or uniform`, `keys takes zipf+space or uniform, not both`, `keys.zipf must be a finite non-negative number`, `keys.space must be a positive count (a whole number, optionally with k, M or G: 500, 10k, 100M)`, likewise `keys.uniform`; `space` and `uniform` are [counts](#values): `100M`); `timeout` checked (a duration), not yet used; `sizes` checked: each names an op the client sends and is a [size](#values) (`sizes names op 'x', which the client does not send`, `sizes 'stream' must be a size (B, KB, MB or GB: 50MB): size must be at most 1e15B`, the parser's reason after the colon; `sizes 'stream' must be positive` for `0B`, a size but no request), read by byte-sized nodes (each request costs its size in MB); `retries` ignored |
| `cdn` | `origin` (node id, required), `hit_ratio` (`{fixed: h}` or `che`, optional), `keys`, `ttl`, `write_ops` | read and run (phase 2b), see [`cdn`](#cdn) |
| `load_balancer` | `policy`, `vnodes` | read and run, see [`load_balancer`](#load_balancer) |
| `rate_limiter` | `algorithm`, `limit`, `burst`, `window`, `backend` | read and run, see [`rate_limiter`](#rate_limiter) |
| `service` | `ops`, `cost` | read and run |
| `cache` | `hit_ratio`, `strategy`, `write_ops`, `backend`, `keys`, `eviction`, `ttl`, `then`, `cost` | read and run (fixed or `che` hit ratio), see [`cache`](#cache) |
| `sql_db` | `replication_lag`, `shards` (int), `pool` (int connections), `write_ops`, `cost` | read and run (phase 2b), see [`sql_db`](#sql_db) |
| `kv_cluster` | `nodes` (int), `vnodes`, `replication` N, `write_quorum` W, `read_quorum` R, `mode` `ap|cp` | ignored; phase 3 |
| `message_queue` | `partitions`, `retention` † | ignored; phase 2c |
| `worker_pool` | `consumes` (queue node id), `workers`, `cost` (duration per job), `then` | `consumes` checked; others ignored; phase 2c |
| `blob_store` | `then`, `write_ops` (its byte-rate `capacity` is the common key) | the store runs, counting megabytes; `write_ops` read as a [cache's](#cache) (default `[write]`): the ops it acknowledges at their last byte, every other op answered at its first megabyte (youtube's `original_store` lists `upload`), shown by `explain` as `write_ops=upload`, with `(never arrives)` when none of them reaches the store; `then` checked, refused by run/estimate until phase 2c's events |
| `search_index` | `build_interval` | ignored |
| `ws_gateway` | `max_connections`, `heartbeat` (duration), `fanout` (avg recipients per message), `ops` | `ops` checked as step lists; others ignored; phase 3 |
| `service_discovery` | `failover` (duration) | ignored; phase 3 |
| `id_generator` | `algorithm` `snowflake|ticket_server|uuid` (required) | read and run (phase 2b), see [`id_generator`](#id_generator) |
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
| `window` | a positive duration, default `1s`. A windowed algorithm's window; on a bucket it only sizes the default `burst`. *(graph)* For `fixed_window`, `sliding_log` and `sliding_counter` (the default `1s` included), a whole number of steps, at least one: the fluid step sees a window boundary only where a step starts | `window must be positive`; *(graph)* `node 'x': window must be a positive whole number of steps (window 150ms, step 100ms)` |
| `burst` | a positive integer, **buckets only**; default `limit × window` rounded up (one window's worth) | `burst must be a positive integer`, `burst applies only to token_bucket and leaky_bucket`, `limit x window is too large for a burst; give burst explicitly` |
| `backend` | a declared node of type `cache`: the counter store | `backend refers to unknown node 'x'`, `backend must be a cache` |

A `leaky_bucket` refuses `capacity`, `latency`, `advanced.queue_limit`,
`advanced.concurrency` and `advanced.service_variability` (the leak is deterministic):
`leaky_bucket takes limit and burst; capacity, latency, concurrency, queue_limit and
service_variability are not valid`.

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
(`link rl -> c does not carry op 'counter' (ops: x)`). `counter` is an INCR, a write, so the
store lists it in its `write_ops` (`counters: {type: cache, ..., write_ops: [counter]}`); a store
that would count it as a read is refused: `node 'rl': sends write 'counter' to 'c', which counts
'counter' as a read; add it to c's write_ops`. A `che` hit ratio does not fit a counter store
that only the limiter reaches: counters are keyed by user or rule, not by the clients' `keys`
(`node 'c': hit_ratio che: the counter requests of rate limiter 'rl' carry no client keys ...`);
give it `{fixed: h}`.

**Terminal gate.** A limiter whose only outgoing link is its backend answers every admitted
request itself. Its caller still calls its own later steps at the full rate; the refused
share shows in the gate's reject rate and in the client's answered rate.

Not resolved: anything inside a step (a window is a whole number of steps, so its
boundaries fall on step starts; the requests of one step arrive as one lump; a burst is
spread over its step). Not modelled: per-client keys (one aggregate limit), races between limiter
replicas, rate-limit headers and retries of rejected requests, soft limits.

### `cache`

Models the cache tier of ch. 1 and its uses in ch. 8 and 11–14. The cache is a queue of its
own (default 100k/s, 250 µs) that answers every request it receives, hit or miss, at its
`cost` per op.

| key | rule | message on violation |
|---|---|---|
| `hit_ratio` | required: `{fixed: h}` with `0 ≤ h ≤ 1`, or `che` (computed at build from `keys` and the clients' key distribution, see [`che` hit ratios](#che-hit-ratios)) | `cache needs a hit_ratio`, `hit_ratio must be che or {fixed: h}`, `hit_ratio.fixed must be between 0 and 1` (also for a quoted `"0.5"`) |
| `strategy` | `cache_aside` (default), `read_through`, `write_through`, `write_around`, `write_back` | `strategy must be cache_aside, read_through, write_through, write_around or write_back` |
| `backend` | a declared node; required by every strategy but `cache_aside`, and refused there (the application reads the store; the backend would never be called) | `strategy read_through needs a backend`, `backend is unused with strategy cache_aside`, `backend refers to unknown node 'x'`; *(graph)* `backend 'db' needs a link from the cache`, and the link must carry every op the cache passes on (`link c -> db does not carry op 'read' (ops: write)`); a store that calls back into the cache (`db.read -> c.read -> db.read`) is refused, since a fetch is always waited for: `node 'c': backend 'db' closes a call cycle` |
| `write_ops` | a non-empty list of op names; default `[write]`. Every other op is a read. A stated list is taken exactly as written (it replaces the default) | `write_ops must be a list of op names`, `write_ops must name at least one op`; *(graph)* a write the cache passes to a `sql_db` or `cache` backend must be a write there too, or the store would serve it as a read (a database from a replica, its primary idle): `node 'c': sends write 'set' to 'db', which counts 'set' as a read; add it to db's write_ops` |
| `keys` | a [count](#values) (`20M`): the entries the cache holds, the Che approximation's cache size; required by `hit_ratio: che` | `keys must be a positive integer`; *(graph)* `node 'c': hit_ratio che needs keys (the cache's capacity in keys)` |
| `eviction` | `lru`, `lfu`, `fifo`, `ttl` | `eviction must be lru, lfu, fifo or ttl` |
| `ttl` | a positive duration | `ttl must be positive` |

`keys`, `eviction` and `ttl` are read and printed by `explain`; `keys` sizes a `che` cache, and
`eviction` and `ttl` wait for later refinements (the Che model is LRU's). `explain`
prints `write_ops=write (never arrives)` when no op that reaches the cache is a write op: the
write strategy then never acts (a note, not an error). Names: ch. 1 draws one pattern, the
application reading the cache and fetching a miss itself, and calls it "read-through"; its
industry name is cache-aside, which is ours. Our `read_through` is the cache-driven fetch; the
write strategies are general caching practice, not the book's.
What each strategy passes to its `backend`, as the same op:

| strategy | reads | writes |
|---|---|---|
| `cache_aside` | nothing: the application's `{to: db, on: miss}` step reads the store for `1 − h` of the requests | nothing (the application writes the store) |
| `read_through` | `1 − h` of each read; a read's response is the mixture `{h: own, 1 − h: own + hop}` | nothing |
| `write_through` | nothing (a miss is the application's `on: miss` step) | every write, synchronously: own + hop |
| `write_around` | as `write_through` | as `write_through`: the same flow. The difference is what the cache holds afterwards, which shows in the hit ratio, and `h` is fixed here |
| `write_back` | nothing (a miss is the application's `on: miss` step, as under `cache_aside`) | every write, **asynchronously**: the cache acknowledges it from memory (response = own sojourn) and flushes it to the store later. The store still absorbs every write (an upper bound: no coalescing of repeated writes to one key); the flush adds no latency and cannot fail a client request. Not modelled: the dirty-entry queue, how far the store lags, writes lost when the cache dies before a flush |

The write-back flush is asynchronous **by design**, not because it closes a call cycle: the graph
marks every `(cache, write) -> (backend, write)` hop of a `write_back` cache as an asynchronous
hop at build. It needs a `backend` and a link to it like every other strategy but `cache_aside`.

#### `then` steps on a cache

A cache's `then` steps say what happens after it answers, when the cache sits **in front of** the
next tier instead of beside the application: the inline placement, in which the cache forwards
its own misses (read-through in spirit). That is **not** ch. 1's cache-aside, where the web server
queries the database itself and the cache forwards nothing (a service's `on: miss` step). The
book's caches in front: ch. 1's CDN workflow (a CDN without the file fetches it from the origin)
and ch. 13's "Browser caching" (suggestions kept in the browser cache for an hour, hence the
scenario's `ttl: 1h`; `then: [{to: lb, op: suggest, on: miss}]`: a hit is answered in the
browser, a miss goes on to the load balancer).

| `on` | flow (per request the cache handles) | response | delivered |
|---|---|---|---|
| `always` (default) | every op, `× factor`, as `step.op` | own + the hop (two link legs + the target's response) for every request; the `mode: parallel` ones form **one group**, the slowest of which is waited for after the store hop and the sequential steps (they overlap one another, never the store hop) | × the target's delivered fraction |
| `miss` | `(1 − h) × factor` of each **read**; writes do not miss and send nothing | the mixture `{h: own, 1 − h: own + hop}`; several miss steps form one block that a miss takes in turn, so each must be sequential | `(1 − f) + f × Π d` over the block, `f = 1 − h` |
| `failure` | nothing before phase 2c | — | — |

Refused at build *(graph)*:

- a `then` step on miss on a `read_through` cache, which already fetches its misses from its
  `backend` (each miss would be fetched twice): `node 'c': a then step on miss and a read_through
  backend both fetch the miss`;
- a service's `on: miss` step after a cache that has a `then` step on miss, the same
  double fetch from the other side: `node 'api': an 'on: miss' step cannot follow cache 'c',
  whose own then step on miss already fetches the miss`;
- a parallel `then` step on miss (a miss waits for it): `node 'c': a then step on miss must be
  sequential (a miss waits for it)`;
- a `then` step that sends the `backend` the op the store hop already sends it (a write under
  `write_through`, `write_around` or `write_back`, a read under `read_through`), which would
  reach the store twice: `node 'c': then step to 'db' duplicates its backend hop`. Another op to
  the same node (`{to: db, op: audit}`) is a call of its own.

A `then` step must be carried by a link like any step. A `then` step on miss carries only the
ops that miss, so a cache that receives only writes sends nothing along it, and its op need not be
carried past the next node.

Not modelled: TTL expiry, eviction orders other than LRU (a `che` cache is LRU's model whatever
its `eviction`), temporal locality, a cold start, the thundering herd of a hot key's misses,
invalidation traffic. A fixed `h` is a number, not a function of the cache size and the key
skew; a `che` one is (below).

#### `che` hit ratios

`hit_ratio: che` asks the graph to compute `h` once, at build, with the Che approximation of an
LRU cache (`src/caching/caching.cppm`): the cache holds `keys` entries, and the key popularity
is the `keys` distribution of the clients whose requests reach the cache. Keys travel with
calls, so the graph follows each client's **ops**, not the boxes: from the client's routes along
every call an op makes (a service's steps, `on: miss` ones included whatever `h` turns out to be;
a load balancer's or rate limiter's routes; a cache's fetch from its `backend` and its `then`
steps). A client whose ops never call the cache brings none of its keys there, even through a
service it shares with the cache's readers. One hop is left out: a rate limiter's `counter`
request to its store, which counts per user (or rule), not per key the request is about. The
model assumes every other hop between a client and the cache carries the client's key.

- no client's ops reach the cache: *(graph)* `node 'c': hit_ratio che: no client's demand reaches it`;
- a client whose ops reach it states no `keys`: *(graph)* `node 'c': hit_ratio che: client 'b' reaches it without keys`;
- two clients with different `keys`: *(graph)* `node 'c': hit_ratio che: clients 'a' and 'b' send different key distributions` (the first two in file order that disagree);
- `che` caches that are each behind the next in a loop, so no filter can be computed first: *(graph)* `node 'a': hit_ratio che: its keys loop back through itself (each cache of the loop is behind the next)`;
- clients with equal `keys` share it. `{zipf: 0, space: N}` is `{uniform: N}` (the loader stores it so), and the two agree.

**Caches behind caches.** Requests that reach the cache as the misses of another `che` cache (its
`read_through` fetch from its backend, its `then` step on miss, or a service's `on: miss` step
after a lookup at it) are the cold tail of the popularity: the hot keys hit in front. Their
popularity is the front cache's filtered by its characteristic time `t`: key `i` misses with
probability `e^{−p_i t}`, so the misses ask for `i` with probability `∝ p_i e^{−p_i t}`
(Martina, Garetto & Leonardi, INFOCOM 2014). Two 2M-key caches in a row over zipf 0.9 and 10M keys:
the first hits 0.747, the second 0.257, not 0.747. A chain filters once per cache. The cache's
reads must all come one way (`node 'l2': hit_ratio che: it receives both the misses of cache 'l1'
and requests no cache filtered`); writes do not count, as they neither hit nor miss. A front
cache with a **fixed** ratio says how many requests miss but not which: the popularity is used
unfiltered, and `explain` says so.

**Effective capacity.** `keys` is per replica. Behind a `consistent_hash` load balancer each key
hashes to one replica, so the replicas hold disjoint parts of the key space, `keys × replicas`
together. The ring's arcs are uneven: with imbalance `k = 1 + 1/√vnodes` (the load balancer's
model) the hottest of `N` replicas owns `a_hot = k/N` of the keys and the others
`a_cool = (1 − k/N)/(N − 1)` each. A replica owning a share `a` of a random partition is, for
Che, one cache of `keys / a` over the whole space, and serves `a` of the requests, so
`h = Σ_j a_j · Che(keys / a_j)`. The hit ratio is concave in the cache size, so uneven arcs hit
less than one cache of `keys × N`: the consistent-hashing lab's four 2M-key servers with
`vnodes: 1` (`k = 2`) give 0.924, where even arcs would give 0.963. A cache the ring partitions
must be reached through rings only: *(graph)* `node 'c': keys are partitioned by ring 'lb' but
'web' also sends to it directly`. Behind any other policy, or with no balancer, every replica can
see every key and fills itself with the same hot keys, so the cache size is `keys`. A replica
lost to a `node_down` fault does not change the hit ratio before phase 3 (it is resolved once,
at build; phase 3 will re-evaluate it from the same inputs).

`explain` prints the number and where it came from: `hit_ratio=0.881 (che: zipf 1 over 100M keys
from users)`; behind another che cache `(che: zipf 0.9 over 10M keys from u, filtered by l1)`;
behind a fixed one `(che: ... from users; fed through fixed-ratio cache 'browser_cache': treated
as unfiltered)`; behind a ring `hit_ratio=0.924 (che: zipf 0.9 over 10M keys from clients; 2M keys
x 4 partitioned, uneven arcs)`. A key space past `9999G` is written in digits. From then on a
`che` cache runs exactly like a fixed one with that `h`.

The code's `HitRatioModel` concept is `hit_ratio(capacity) -> double`, each model owning its
inputs (`CheHitRatio{popularity}`), where spec §5 wrote `hit_ratio(workload, keys)` (spec §18
entry 2).

### `cdn`

Models the CDN of ch. 1 ("a network of geographically dispersed servers used to deliver static
content") and ch. 14 (the CDN that streams the videos); ch. 13's browser cache sits in the same
place. A CDN is a **read-only cache in front of its `origin`**: it answers the share `h` of the
reads it hits itself and fetches the misses `1 − h` from the origin as the **same op** (ch. 1:
a CDN server without the file requests it from the origin, stores it and returns it). It is the
`read_through` shape of a cache whose backend is the origin:

| | per read sent to the CDN |
|---|---|
| flow to the origin | `1 − h` of it, as the same op, over the link `cdn → origin` |
| response | the mixture `{h: own, 1 − h: own + two link legs + the origin's response}` |
| delivered | `own × (h + (1 − h) × the origin's delivered fraction)` |

`capacity`, `latency` and `advanced` describe the edge's own queue (default 100k/s, 10 ms: one
~1 MB object over the network).

| key | rule | message on violation |
|---|---|---|
| `origin` | required; a declared node that is not a client and not the CDN itself, with a link from the CDN. Another CDN is fine: a tiered CDN (edge → regional → origin), whose second tier's `che` is computed over the first tier's misses | `node 'edge': a cdn needs an origin`; `node 'edge': origin refers to unknown node 'x'`; `node 'edge': origin 'u' is a client; clients only send`; `node 'edge': a cdn cannot be its own origin`; *(graph)* `node 'edge': origin 'x' needs a link from the cdn`; `node 'edge': origin 'x' closes a call cycle` (the origin calls back into the CDN, so a miss would wait for itself) |
| `hit_ratio` | `{fixed: h}` (`0 ≤ h ≤ 1`) or `che`, as a cache's; optional: without one every read misses (`h = 0`) and still runs | `node 'edge': hit_ratio must be che or {fixed: h}` |
| `keys` | the CDN's size in **objects** (not bytes), a positive [count](#values); a `che` hit ratio resolves from it and the `keys` of the clients that reach the CDN, exactly as a [cache's](#che-hit-ratios) | `node 'edge': keys must be a positive integer` |
| `ttl` | a positive duration, printed and not modelled | `node 'edge': ttl must be positive` |
| `write_ops` | the ops that are writes, as a [cache's](#cache) (default `[write]`): a CDN **refuses** them (below) and caches every other op as a read. Name an `upload` or a `purge` here when a client sends it through the CDN, or the CDN answers the hit share of it from the edge; `explain` lists what the CDN serves, `serves: read, upload` | `node 'edge': write_ops must name at least one op` |

Refused:

- *(graph)* a write reaching the CDN (an op in `write_ops`, `write` by default), from a client or
  a step: the CDN has no write path, content changes at the origin: `node 'edge': a cdn does not
  take writes ('upload' from 'users')`, naming the op and a node that sends it;
- *(graph)* a service's `on: miss` step after a CDN lookup, which would fetch every miss a second
  time (see [Steps](#steps)); a CDN built in code with `then` steps: `node 'edge': a cdn has no
  then steps (it fetches its misses from its origin)`, with a cache `strategy`: `node 'edge': a
  cdn has no strategy (it reads through to its origin)`, or with a `backend`: `node 'edge': a cdn
  has no backend (its origin is the store behind it)`;
- *(run, estimate)* `hit_ratio: che` without `keys`: `explain` shows `hit_ratio=che`, counts every
  read as a miss and marks the checks behind it as an upper bound; a run refuses it (`node 'edge':
  hit_ratio che needs keys (the cdn's size in objects)`) rather than silently run `h = 0`.

`explain` shows a CDN as it shows a cache, plus where the misses go and what it serves:
`hit_ratio=0.95 (fixed) misses → media_store ttl=1h  serves: read_media`, and the origin's check
line counts the miss share (news-feed's `media_store` is offered 5 % of the CDN's reads). A CDN
without a hit ratio reads `hit_ratio=none (every read misses: h = 0)`: that is what runs, so the
origin's check is a plain one, not the upper bound of an unsized `che`.

A `che` CDN is the Che approximation over its `keys` objects: LRU eviction and independent
requests from the clients' key popularity, as for a cache.

A CDN with a byte `capacity` counts megabytes (see `capacity` in
[Node (common keys)](#node-common-keys)): a 100 GB/s edge at 10 ms per MB has 1000 megabyte slots
and saturates at 2k streams/s of 50 MB; its queue waits for whole streams (one holds a slot
0.5 s) and a stream, a read, is answered at its first megabyte.

Not modelled: link bandwidth (phase 2c), the transfer time of a stream beyond its first megabyte,
TTL expiry and the stale content it serves, a cold
edge, invalidation and purges, the choice of the nearest point of presence (the client's links
say which CDN it uses), and the CDN's price.

### `sql_db`

Models the relational database of ch. 1 ("database replication", "sharding") and the DB box of
the later designs. `replicas: N` is one **primary** and `N − 1` **read replicas**; `capacity`,
`latency` and `advanced` describe one copy (default 5k/s, 150 µs: an SSD random read). A
`sql_db`'s `capacity` is **reads/s**: a read costs 1 unit of it and a write 5 by default (see
`cost` below), so a copy of 5k/s does 5k reads/s or 1k writes/s.

| key | rule | message on violation |
|---|---|---|
| `shards` | a positive integer; default 1. The data is split over that many independent sets of `N` copies each | `shards must be a positive integer` |
| `pool` | a positive integer of at most 100000: the connection pool, the queries one copy has in service at once. By Little's law (queries in service = throughput × service time) a copy then serves at most `pool / latency` **capacity units** per second: below the `c` its capacity implies (`capacity × latency`, rounded up), the pool is the copy's `c` and its rate becomes `pool / latency` (4 connections of 10 ms queries: 400 units/s, whatever the disk's 5k/s); at or above it, it changes nothing (more connections never make the disk faster). A write at the default cost of 5 holds its connection 5 × the service time, so it uses five of those units. Unlike `advanced.concurrency`, which only sets `c`, the pool also caps the rate; both set `c`, so stating both is refused | `pool must be a positive integer`, `pool must be at most 100000`, `pool sets c and caps the rate by Little's law; drop advanced.concurrency` |
| `replication_lag` | a non-negative duration; default 0: shipping takes no time; the write still waits in both queues (the primary's and a replica's, so the staleness is not 0). Accepted with `replicas: 1`, where nothing replicates (`explain` marks it `(unused: no replicas)`) | the duration parser's (`replication_lag: expected a non-negative finite number`) |
| `write_ops` | as a cache's; default `[write, insert, update, delete]`. These ops go to the primary; every other op is a read | as a cache's |
| `cost` | as a service's; a write op it does not list costs **5**: spec §6 gives one SQL machine "5k reads" or "1k writes" per second, so a write takes five reads' capacity (on the primary and on every replica that replays it). Reads cost 1 | as a service's |

An absent `write_ops` is the database default; a stated one is exactly what it says, so
`write_ops: [write]` makes `write` the only write op (`insert` is then a read). The model keeps
"absent" apart from any list, so a database built in code without `write_ops` gets the database
default too.

A database answers every query itself and calls nobody: a link that would carry demand out of
one (`{from: db, ...}`, or `{to: db, dir: in}`) is refused, `node 'db': a database calls nobody`.

How the load is split, with `S` shards (each copy its own queue of the node's capacity):

| copy | how many | one copy is sent | answers |
|---|---|---|---|
| primary | `S` | writes / `S` | the writes |
| replica | `(N − 1) S` | reads / `((N − 1) S)` **+ committed writes / `S`** | the reads |

Every replica replays every write its primary **committed** (the primary's log holds nothing it
dropped), so replicas scale reads, not writes: with `N = 3`, 1000 reads/s and 60 writes/s at the
default write cost of 5 (300 units/s) on copies of 1k/s, the primary runs at 300 units/s (30 %)
and each replica at 500 + 300 = 800 units/s (80 %): under 6 % of the requests are writes, but
they are three eighths of each replica's work. With `N = 1` the `S` primaries take reads and
writes alike. A write's
response is the primary's sojourn, a read's a replica's. The node reports the busier kind's
`rho` and zone. Its demand, drain, drops and backlog are **the clients' part**: the replicas'
replication work shows in their `rho`, in the reads' latency and in the replicated writes they
drop (below), not in those four, so a replica full of replicated writes drops before the backlog
shown reaches `queue_limit` × copies, and the clients' totals balance
(`demand·dt = (drain + dropped)·dt + Δbacklog`). A load balancer in front changes nothing: the
database driver, not a balancer, sends writes to the primary and reads to a replica, so a
`least_connections` or `consistent_hash` balancer imposes no pool and no hot copy on a database.

**Staleness**: a write reaches a reader after its sojourn at the primary (wait + service, until it
is committed), the `replication_lag` (shipping it) and its sojourn at a replica (waiting behind
the reads and the other replicated writes, then applied). Their sum is the node's staleness, 0
without replicas; the `staleness` SLO compares its mean over the run with `max`, and JSON reports
it per node as `staleness_s` (`null` for every other node). In the example above with
`replication_lag: 50ms` and 1 ms queries, `run` reports 56.4 ms.

A replica too busy to replay every committed write **falls behind without bound** (the model
drops the excess at `queue_limit`; a real replica keeps it in its relay log/WAL until it is
rebuilt): MySQL's relay log and Postgres's WAL never skip an entry, so a real replica's lag grows
linearly for as long as the overload lasts, and it is rebuilt from a fresh copy once the primary's
log retention runs out. The staleness reported is the **bounded-queue figure** (finite: JSON has no
infinity), not that growing lag. The node reports the writes ONE replica misses per second as
`replication_dropped_per_s` and the writes one shard's primary committed per second, which each of
its replicas must replay, as `committed_writes_per_s` (JSON; `0` missed while the replicas keep
up, both `null` for every other node). The `staleness` SLO **fails** on any missed write,
whatever the mean staleness, with the reason after the limit. 2 copies, 800 reads/s + 400
inserts/s (2000 units/s, at cost 5) on copies of 1k/s: the primary commits 200 inserts/s, and the
replica, sent 800 + 1000 units/s, misses 88.89 of them:
`FAIL staleness db - 2.05s 100ms each replica misses 88.89/s of 200/s committed writes (44.4%)`
(JSON: the SLO's `note`). The text output prints the numbers in a `replication` section after the
node table (`database  staleness  committed writes/s per shard  missed writes/s per replica`).

A copy at **exactly** rho = 1 (its load equal to its capacity) is a knife edge where `run` and
`estimate` disagree: a run keeps an empty queue empty (drain = demand) and reports the stochastic
wait, capped at rho 0.99, while the steady-state estimate takes the queue as full
(`FluidQueue::settle`). 200 inserts/s on 2 copies of 1k/s puts both the primary and the replica at
exactly 1: `estimate` reports a staleness of 2.05 s (two full queues of 1 s, + 50 ms), `run` 250 ms.
Size a database off rho = 1, as the examples here are.

`explain` prints the model, `primary+2 replicas  shards=4  lag=50ms  pool=32
write_ops=write,insert` (`(default)` after a shard count or lag the scenario left out, `(unused:
no replicas)` after a lag with `replicas: 1`; `pool` only when stated, and in the queue numbers
`capacity=400/s (pool 4 / 10ms)  ...  c=4 (pool)` when it binds). Every write op that reaches the
database with no stated cost is printed with its default, after the costs the scenario states,
and where the default comes from once: `cost: insert 5 (default: spec §6 5k reads/1k writes)`,
`cost: read 2; insert 5, update 5 (defaults: spec §6 5k reads/1k writes)`. Its check splits the
paper load in capacity units, and says so; for the example above (1000 selects/s and 60 inserts/s
on copies of 1k/s): `primary 300 units/s vs 1k/s (30%, green), each replica 800 units/s vs 1k/s
(80%, yellow)`. The total capacity counts primaries that serve no reads, so when its average's
zone is milder than the busiest copy's, the headline gives the busiest copy's: `peak offered
1.3k units/s vs 3k/s total capacity  (43.3%, yellow at the busiest copy)`.

Not modelled: replication conflicts and multi-primary set-ups, failover, transactions and
locks, waiting for a free connection beyond the pool's queue, cross-shard queries, hot shards
(the sharding key is perfect), resharding, per-op service times (a write uses more capacity, 5
units by default, not a longer service time), the replica applying its log on a thread of its
own (a replicated write waits in the same queue as the reads), read-your-writes.

### `id_generator`

Book ch. 7, "Design a unique ID generator in distributed systems" (plan 2b refinement 6). An id
generator is **sugar for a `service`**: no behaviour of its own, only the capacity and service
time of the design its `algorithm` names, resolved at build. A `capacity` or `latency` the
scenario states wins, as on any node; `replicas`, `advanced` and the other common keys work as
on a service.

| `algorithm` | capacity per replica | service time | where the number comes from |
|---|---|---|---|
| `snowflake` | 4.096M/s | 10us | 12 sequence bits: 4096 ids per millisecond per machine, × 1000; the time is chosen (a clock read and a counter) |
| `ticket_server` | 1k/s | 1ms | one auto-increment database, and an id is a write: spec §6's "SQL 1k writes"; the time is chosen (a one-row commit) |
| `uuid` | 1G/s | 1us | generated locally, nothing to coordinate: unbounded in practice; both chosen |

| key | rule | message |
|---|---|---|
| `algorithm` | required, one of the three words (`multi_master`, the chapter's fourth design, is not modelled) | `node 'ids': id_generator needs algorithm: snowflake, ticket_server or uuid` (missing or unknown) |
| `replicas` | exactly 1 on a `ticket_server`: one database issues the sequence; a second copy would repeat it (the chapter's remedy for its single point of failure, several ticket servers kept in sync, is not a mode of the model). Checked by the loader, and by `Graph::build` for scenarios built in code | `node 'ids': a ticket server is one auto-increment database: replicas must be 1 (ch. 7; for more ids per second use snowflake)` |

`explain` says what the algorithm stands for, `algorithm=snowflake (4.096M/s per replica by
design)`, `algorithm=ticket_server (one database: 1k/s, spec §6 writes)`, `algorithm=uuid
(unbounded: 1G/s)`, tags the numbers the algorithm supplied `(algorithm)` (`capacity=4.096M/s
(algorithm)`), calls the algorithm's figure a default beside a capacity the scenario states
(`capacity=8k/s (scenario) ... algorithm=ticket_server (one database; default 1k/s, spec §6
writes)`), and checks the peak load against replicas × capacity (`vs 12.29M/s total
capacity` for three snowflake machines). The [unique-id lab](../scenarios/labs/unique-id/)
compares the ticket server with snowflake.

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
(see [What runs](#what-runs-in-phase-2b)).

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
| `noise` | `sigma` in [0, 10] (stationary standard deviation), `correlation` > 0 (τ). The multiplier is `max(0, 1 + x)`, and the floor lifts its mean to `Φ(1/σ) + σφ(1/σ)`: 1.004 at σ 0.5, 1.08 at 1, 1.40 at 2, 4.5 at 10 | `rate.noise needs sigma and correlation`, `rate.noise.sigma must be at most 10` |

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
| `bandwidth` † | checked | a positive [byte rate](#values), in bytes or bits (`40Gbit/s` = 5e9 B/s): `bandwidth must be positive`, `bandwidth must be at most 1e15B/s`, `bandwidth is a byte rate (B/s, or bit/s for a network: 40Gbit/s)`. The pipe it caps is modelled from phase 2c |
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
| `staleness` | optional; name a `sql_db` | not allowed | duration | phase 2b: the named database's mean staleness after warmup, its write sojourn at the primary + `replication_lag` + its sojourn at a replica (see [`sql_db`](#sql_db)); fails whatever that mean when a replica misses committed writes (it falls behind without bound; the note says how many per replica, of how many committed); `skipped` without a `node` or on a node that keeps no replicated data (any type but `sql_db`); a `sql_db` with `replicas: 1` reports 0 |

The kind of `max` follows the metric, so `0.01` can never be read as 0.01 seconds
(`slo: reject_rate max must be between 0 and 1`).
`op` is only valid for latency metrics (`slo: op is only valid for latency metrics`): drop
rate, reject rate and utilization are measured per node, not per op.

### Drops and rejections

Two different promises, kept apart on purpose:

- a **drop** is a node **failing**: its waiting room was full, so the excess is shed.
  `drop_rate` = Σ dropped / Σ demand **of that node** (in capacity units when the node has
  a `cost` or counts megabytes), so it stays in [0, 1] even when fan-out multiplies the clients' requests
  before they reach it (one post → 100 feed writes would make "dropped anywhere / offered
  by clients" 90). Without `node` it is the worst node's, since every node must keep the
  bound.
- a **rejection** is a rate limiter's **policy working** (ch. 4's 429): what a token bucket or a
  window algorithm refuses, and a leaky bucket's overflow. `reject_rate` = Σ rejected / Σ
  demand of that node, with the same rules.

Per node and per step the numbers balance, `demand·dt = (drain + dropped)·dt + Δbacklog`:
whatever reaches a node is served, shed or left waiting. A rejection is not a term of its
own: a token bucket's or a window algorithm's rejections are inside `drain` (the limiter's
queue checked them before refusing them), and a leaky bucket's overflow is the `dropped`
term, reported as `rejected` instead. `run --series` writes demand and drain (phase 2b), so the
identity can be checked from the file: summed over a run that starts empty,
Σ demand·dt = Σ (drain + drops)·dt + the backlog left at the end.

Neither kind of request is answered, so neither contributes latency: each step's response
is weighted by the client's offered rate times the fraction its path **delivers** (a
node's own share of the step's arrivals it did not lose, 1 − (dropped + rejected) / inflow,
times its downstream's: sequential parts multiply, splits mix by share). A request that waits
in a growing queue is not lost: it is answered later, so a backlog alone leaves the delivered
fraction at 1. `run`'s table header shows `drop rate`, and `reject rate X% (worst: node)` once
some node refused anything; the node table has `dropped` and `rejected` columns; JSON has
`drop_rate`, `worst_drop_node`, `reject_rate`, `worst_reject_node` and per node `dropped`,
`rejected` and (phase 2b) `staleness_s`, a database's mean staleness in seconds,
`replication_dropped_per_s`, the committed writes one replica missed per second, and
`committed_writes_per_s`, the writes one shard's primary committed per second, all three `null`
for other nodes. Each SLO object has a `note`: `null`, or why the verdict is not `observed`
against `max` alone (a replica that falls behind without bound).
At a node that counts megabytes (a byte `capacity`, see [Node (common keys)](#node-common-keys))
the node's own numbers are in MB: the table's `dropped`, JSON's `dropped`, and the series'
`demand_per_s`, `drain_per_s`, `drops_per_s` (MB/s) and `backlog` (MB), as its capacity and
`queue_limit` are; what the clients offered and got answered (`offered_per_s`,
`answered_per_s`) stays requests. Its `latency_ms` is its requests' own sojourns mixed by their
rates, each answered at its last byte (a write) or its first megabyte (a read).

`run --series` writes one CSV row per node per step:
`t_s,node,demand_per_s,drain_per_s,rho,backlog,drops_per_s,rejected_per_s,latency_ms,zone`
(`rejected_per_s` new in v1.2, `demand_per_s` and `drain_per_s` in phase 2b; demand, drain,
drops and backlog in capacity units, megabytes at a byte-sized node; a client row has zeros and
zone `none`).
`run --series-ops` writes one CSV row per client op per step (phase 2b):
`t_s,client,op,offered_per_s,answered_per_s,mean_ms`, where `answered_per_s` is the part of
the offered rate that came back answered (offered × the fraction the path delivered: offered
minus what a limiter refused or a queue dropped on the way, never above offered; requests
still waiting in a queue count as answered, later) and `mean_ms` the mean response of an
answered request (`inf` when the path never answers). Numbers have 6 significant digits, `t_s`
its shortest exact form. JSON's op entries carry `answered_per_s` beside `offered_per_s`.
When no step was observed every SLO is `skipped` and `run` exits 1: no data is an error,
not a pass.

## Faults

`{at: clock-or-duration, for: duration, kind: node_down|dc_down †|partition|latency_spike †|traffic_spike †|cache_flush †, node|dc|between †, fraction, factor †}`.

`partition` takes either `between: [dc-a, dc-b]` † (cut two datacenters apart) or `node: <id>, fraction: <0–1>`
(split a clustered node into two groups of the given proportion that cannot see each other;
`fraction: 0.5` is two halves).

The loader checks only that `faults` is a list; faults are typed and simulated in phase 3.
