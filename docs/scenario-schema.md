# Scenario schema v1.4 (phase 2c)

v1 was fitted to the 12 book and lab drafts plus the first tutorial; v1.1 wrote down the
rules the phase 1 loader enforces; v1.2 added phase 2a's rate curves, balancers, limiters and
fixed-ratio caches; v1.3 phase 2b's Che caches, SQL databases, CDNs and id generators.
**v1.4** is the language as phase 2c reads it: the loader (`src/config/`, one implementation
unit per concern: `header.cpp`, `nodes.cpp`, `steps.cpp`, `workload.cpp`, `links.cpp`,
`slos.cpp`, `faults.cpp`, `overrides.cpp`), the graph builder (`src/engine/graph.cpp`), the node behaviours
(`src/engine/behaviours.cppm`), the reports (`src/report/report.cpp`, `explain.cpp`) and the
sweep (`src/sweep/sweep.cpp`). All 14 files under
`scenarios/` load under it, and eleven of them run end to end ([below](#what-runs-in-phase-2c)):
the ten of phase 2c and, since phase 3's [`notification_fanout`](#notification_fanout),
`book/notification-system`.
The design decisions behind the new rules, with what each ignores, are spec §18 (phase 2b) and
§19 (phase 2c) (`docs/superpowers/specs/2026-10-27-system-sim-design.md`).

New in v1.4:

- **bytes**: a client's [`sizes`](#node-types) (`{stream: 50MB}`) and a step's
  [`size`](#steps) give requests a size, resolved once per (node, op) at build
  ([Request sizes](#steps)); a byte [`capacity`](#node-common-keys) on a `cdn` or a
  `blob_store` (and a blob store's default) makes the node count **megabytes**: its queue waits
  for whole objects, a write is answered at its last byte and a read at its first megabyte; a
  [byte rate](#values) may be written in bits (`40Gbit/s`), and sizes (`200B`, `1.5GB`) are a
  value kind of their own;
- a link's [`bandwidth`](#links) is a **pipe**: one deterministic server in megabytes that queues
  and drops, with a row of its own in `run`'s `links` table, JSON's `links`, the series and
  `explain`'s checks;
- [`message_queue`](#message_queue-and-worker_pool) (`retention`, default 7 days; `partitions`
  printed, not modelled) and [`worker_pool`](#message_queue-and-worker_pool) (`consumes`,
  `workers`, `cost` as a duration per job, `then` steps that fire for every job drained): a
  publisher is answered at the append, every consumer group gets every message, and nobody waits
  for a pool; a queue's [lag](#queue-lag), the `queue_lag` SLO, the table's `peak_lag` column,
  JSON's `peak_queue_lag_s` and the series' `queue_lag_s`;
- a `blob_store`'s `then` steps run as asynchronous **event notifications**, one per write it
  stores ([Node types](#node-types)), and `write_ops` on a blob store says which ops are writes;
- [`external_sink`](#node-types): a third party's API that fails `failure_rate` of what it serves,
  counted as drops (`politeness` printed, not modelled);
- [`kv_cluster`](#kv_cluster): `nodes`, `vnodes`, `replication`, `write_quorum`, `read_quorum`,
  `mode` (printed, not modelled) and `write_ops`; the ring's hottest machine as its own queue, a
  quorum's latency and losses over its request's real replica set;
- [`search_index`](#search_index), served like a service (`build_interval` printed, not modelled);
- **a step after a failed step does not run** ([Steps](#steps)): each step is sent only what the
  steps before it delivered, and the parallel steps of an op must be adjacent;
- a client's [`timeout`](#drops-and-rejections): the slow tail of each step's response is timed
  out, not answered (`timed_out_per_s` in JSON, the series and `run`'s `timed out/s` column),
  a percentile at q is the fit to every delivered request's latency (before the timeout cut it)
  read at q and capped at the timeout, and a latency SLO whose quantile timed out fails;
- an SLO's [`over`](#slo): the SLO is checked per window and the worst window decides
  (`(worst 1h from 13:02)`, JSON's `window_start_s`);
- [`simsys sweep`](#command-line-simsys-sweep-phase-2c): one scenario over a range of one key, the
  points run in parallel;
- the duration unit `d` (a day), the [step rule of thumb](#top-level) in the `step` row, and the
  runnable set: the six of v1.3 plus `book/youtube`, `book/news-feed`,
  `book/search-autocomplete` and `labs/kv-store`.

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
  its percentage as a lower bound `(≥p%; ...)` ([below](#what-runs-in-phase-2c)), and a node
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

## What runs in phase 2c

`simsys validate` runs the loader only. `explain`, `estimate` and `run` also build the
graph, which adds the checks marked *(graph)* below. `explain` works for every node type.
`estimate` and `run` (and so `sweep`) step these types:

| type | runs in phase 2c |
|---|---|
| `client` | the whole rate curve, with its seeded noise |
| `service` | ops, steps, per-op `cost` |
| `load_balancer` | every policy |
| `rate_limiter` | every algorithm, with or without a counter `backend` |
| `cache` | a fixed or [`che`](#che-hit-ratios) hit ratio, every strategy (`write_back` with an asynchronous flush), [`then` steps](#then-steps-on-a-cache) |
| `sql_db` | a primary and read replicas, `shards`, `pool`, `replication_lag` and the `staleness` SLO, see [`sql_db`](#sql_db) |
| `cdn` | a fixed or `che` hit ratio (or none: every read misses), misses fetched from its `origin`, see [`cdn`](#cdn); with a byte `capacity` it counts megabytes (phase 2c, see `capacity` in [Node (common keys)](#node-common-keys)) |
| `blob_store` | every op, counting megabytes (phase 2c, see `capacity` in [Node (common keys)](#node-common-keys)): its queue waits for whole objects, a write (`write_ops`) is answered at its last byte, a read at its first megabyte; `then` steps as asynchronous event notifications (see [Node types](#node-types)). A store whose `capacity` is a request rate (`capacity: 1k/s`) counts **requests** instead, as a service does: unsized ops are accepted and each takes the type's 40 ms (`c=40` at 1k/s) |
| `id_generator` | as a `service` whose capacity and service time its `algorithm` supplies, see [`id_generator`](#id_generator) |
| `search_index` | as a `service` with no steps and the type's 5k/s and 5 ms unless stated; `build_interval` stored and printed, see [`search_index`](#search_index) (phase 2c) |
| `service_discovery` | as a `service` answering `lookup` at the type's 50k/s and 0.5 ms unless stated; a `node_down` of a node its callers look up holds them on the dead replicas for `failover`, see [`service_discovery`](#service_discovery) (phase 3) |
| `message_queue` | the broker's append, every message copied to each consumer group, the publisher answered at the append (phase 2c, see [`message_queue` and `worker_pool`](#message_queue-and-worker_pool)) |
| `worker_pool` | M/M/c over consumption, `workers / cost` jobs a second, its room its queue's `retention`; its `then` steps fire for every job it drains, nobody waiting for them (phase 2c, same section) |
| `external_sink` | a third party's API: a queue of the type's numbers (1k/s, 100 ms, service SCV 2.0) that fails `failure_rate` of what it serves, counted as drops (phase 2c, see [Node types](#node-types)) |
| `kv_cluster` | `nodes` machines on a ring, every request sent to `replication` N of them, answered at the W-th (write) or R-th (read) fastest of its own replica set and lost when fewer come back; the ring's hottest machine as its own queue (phase 2c, see [`kv_cluster`](#kv_cluster)) |
| `ws_gateway` | its open connections as a **level** that a client op named `connect` fills or a fixed `connections` states, capped at `max_connections × replicas`, loaded by `C / heartbeat` pings, its message ops fanned out by `fanout` (phase 3, see [`ws_gateway`](#ws_gateway)) |
| `notification_fanout` | a service (the type's 20k/s at 1 ms) whose `then` steps fire for the `1 − dedup` of what it serves that its dedup step lets through, the rest counted as `deduplicated_per_s`; `channels` names the queue behind each channel (phase 3, see [`notification_fanout`](#notification_fanout)) |

Everything else is refused before the first step, with exit code 1:

| what | message | runs from |
|---|---|---|
| `frontier` | `node 'x' has type 'frontier', which run/estimate do not support yet (phase 3)` | 3 |
| a `ws_gateway` that no `connect` op reaches and that states no `connections` | `node 'x': a ws_gateway needs a client op named 'connect' or a fixed 'connections'` | now, once it has one |
| a `ws_gateway` with both | `node 'x': a ws_gateway takes a 'connect' op or a fixed 'connections', not both` | now, with one |
| a `cdn` with `hit_ratio: che` and no `keys` | `node 'x': hit_ratio che needs keys (the cdn's size in objects)` | now, once it states `keys` |
| a `cache_flush` on a cache with a fixed `hit_ratio` and no `keys` | `node 'x': cache_flush needs keys (a fixed hit ratio is back once keys requests have refilled it)` | now, once it states `keys` |
| a `cache_flush` on a cache whose hit ratio is 1 | `node 'x': cache_flush on a cache that always hits (hit ratio 1): it has no miss path to warm through` | now, with a hit ratio below 1 |

The type message names the phase that plans the type's model, phase 3 for the one left. The type
check runs over the whole file first, and names the first node
in file order whose type cannot run; only then are the nodes of runnable types checked (a CDN's
`keys`, a gateway's level), in file order: google-drive is refused at its `ws_gateway`,
`long_poll`, which states no level yet (`node 'long_poll': a ws_gateway needs a client op named
'connect' or a fixed 'connections'`), chat-system at its `ws_gateway`, `chat_servers`, for the
same reason (its `service_discovery` runs since phase 3), and web-crawler at its `frontier`. Of the
14 shipped scenarios eleven run (`exit` is `simsys run`'s exit code, Release build):

| scenario | exit | what it shows |
|---|---|---|
| `tutorial/hello` | 0 | one M/M/1 service |
| `tutorial/chain` | 0 | two services in a chain |
| `labs/rate-limiter` | 0 | ch. 4's token bucket against three spikes |
| `labs/consistent-hashing` | 0 | ch. 5's ring over four Che caches |
| `labs/unique-id` | **2** | ch. 7's ticket server overloaded **on purpose**: its `drop_rate` SLO fails (0.3391) and so does its p99, every delivered id timing out at the 50 ms timeout |
| `labs/kv-store` | 0 | ch. 6's Dynamo-style store on 16 machines |
| `book/url-shortener` | 0 | ch. 8 over a day, with a windowed utilization SLO |
| `book/youtube` | 0 | ch. 14 in bytes: CDNs, blob stores, a bandwidth link, a transcoding queue and pool |
| `book/news-feed` | 0 | ch. 11's fan-out on write through a queue, with a kv_cluster for the graph |
| `book/search-autocomplete` | 0 | ch. 13's trie cache, search index and query-log pipeline |
| `book/notification-system` | 0 | ch. 10's fan-out with dedup into four channel queues, pools that retry their providers and dead-letter the rest, a 20× marketing blast drained within the lag SLOs |

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

**A node at exactly rho = 1** (its load equal to its capacity, on any node or bandwidth link)
neither fills nor empties: the fluid backlog grows only above capacity. Both commands keep its
waiting room empty, as a run that starts empty does, and report the stochastic wait at the 0.99
clamp (a 1 ms M/M/1 node waits 99 ms there, a 1 Gbit/s link carrying 125 MB/s of 1 MB requests
396 ms), so `estimate` and `run` agree. A real queue at rho 1 has no steady state: its backlog
wanders, about half its room on average (an M/M/1/K queue at rho 1 holds every length from 0 to K
equally often), so its wait can be anything up to the room's drain time; the model's figure is
the low end of that. Size a system off rho = 1, which both commands colour red.

**Load `explain` cannot follow yet.** The checks follow the load as the engine would: from
the clients' routes, along `ops` steps, through load balancers and rate limiters, and on from
caches and CDNs (their misses, their store, a cache's `then`), along a `blob_store`'s `then`
(its event notifications, one per write it serves), from a `message_queue` into **each** of its
consumer groups (a copy of every message, as the engine delivers) and along a `worker_pool`'s
`then` (every step for every job the pool is **offered**: the paper load does not queue, so a
pool offered more than its `workers / cost` is red on its own line and still passes all of it on;
youtube's 600 transcoders are offered 9.6 uploads/s at the 21:00 peak, `9.6/s vs 10/s  (96%,
red)`, and their renditions bring `transcoded_store` to `9.811k MB/s (196.2/s) vs 20k MB/s
(49.1%, green)`; at the 400 workers of its README's sweep the pool reads `9.6/s vs 6.667/s
(144%, red)` and still hands on all 9.6/s on paper, while the engine forwards what a pool drains,
at most its capacity) and along a `notification_fanout`'s `then` (phase 3: each target is sent
the fan-out's peak × `factor` × (1 − `dedup`), a `kv_cluster` that times N, as the engine
publishes). They do **not** follow the `then` of a `frontier` (a type of phase 3 with no model
yet). A node whose load can only come
through one gets 0 on paper: a number that is missing, not zero. Its check line therefore
has no percentage and no zone, but a mark naming each such type on the way to it, nearest first:
`dns  peak offered 0/s vs 100k/s total capacity  (load not propagated: frontier is not modelled
yet)`. The mark follows such a node's `then` steps (a frontier's links when it has none), not
every link it has. A node that also receives load the checks do follow gets its percentage as a
**lower bound**, with no zone unless even that is red (the real load is higher, so green or yellow
might not hold, but red does): `frontier  peak offered 50/s vs 5k/s total capacity  (≥1%; load
not propagated: frontier is not modelled yet)` (the web crawler's frontier, fed back through
itself), `(≥150%, red; ...)`. A `kv_cluster` behind such a type keeps its own head (see
[`kv_cluster`](#kv_cluster)) and takes the mark like any node.
A lower-bound line has no per-copy figures (a database's primary/replica split, a ring's hottest
copy or machine): they would be lower bounds too.

## Values

| kind | grammar | examples |
|---|---|---|
| duration | non-negative number + `ns us ms s m h d` (`d`, a day of 24 h, from phase 2c: a queue's `retention` is quoted in days); rounded to whole nanoseconds (below 1 ns is an error) | `250us`, `0.5ms`, `2m`, `7d` |
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
| `step` | duration | no | read | default `100ms`; must be positive. A rule of thumb (spec §19 entry 16; the derivation and a measured lab are in [numerics, "Choosing the step"](study/numerics.md)): a step is a lump the model cannot see inside, so make it shorter than what you want to see. A token bucket of `burst` B under a peak offered rate `λ` above its `limit` L empties in `B / (λ − L)`, and a longer step passes the whole burst in its first step (a 2,000-token burst at `step: 1s` does), so `step ≤ B / (λ_peak − L)`; for a queue, `step ≤` a tenth of the shortest spike. Shorter steps cost run time, not accuracy of the totals: conservation holds at any step |
| `warmup` | duration | no | read | steps before it are left out of the report; default 0; must be shorter than `duration`, so the report never covers an empty window (`run --duration` obeys the same rule, and leaves room for every SLO's `over`) |
| `datacenters` | map | no | read | see below; default: one datacenter `main` |
| `nodes` | map id → node | yes | read | non-empty; ids are non-empty and unique |
| `links` | list | yes | read | `[]` for none |
| `slo` | list | no | read | |
| `faults` | list | no | read | scripted failures, see [`faults`](#faults); `run` applies them, `estimate` leaves them out |

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
| `replicas` | integer > 0 | all but `kv_cluster` and `worker_pool` (refused there: a cluster's machines are its `nodes`, a pool's more workers are more `workers`; see [`kv_cluster`](#kv_cluster) and [`worker_pool`](#message_queue-and-worker_pool)) | read | N identical copies: each copy sees its share of the demand with the node's full capacity, concurrency and queue limit; backlog and drops are reported summed. How the demand is shared depends on a load balancer in front ([below](#load_balancer)); without one, `demand / N` each. On a `sql_db` the copies are one primary and `N − 1` read replicas, split by op whatever stands in front (see [`sql_db`](#sql_db)); on a `ticket_server` id generator it must be 1. No effect on a client |
| `capacity` | rate or byte rate | all but `client` and `worker_pool` (a pool's rate is `workers / cost`: `node 'w': a worker pool's rate is workers / cost`) | read | service rate **per replica**; default per type (§6 of the spec; 100k/s for `load_balancer`, `rate_limiter` and `cache`). A byte rate (`100GB/s`, or in bits as a network is rated: `40Gbit/s` = 5 GB/s, printed `5GB/s`) on a `cdn` or `blob_store`, and a `blob_store`'s default (spec §6's 500 MB/s), makes the node count **megabytes**: its capacity is that many MB/s, a request costs its size in MB (the client's `sizes:` or the step's `size:`), and its service time is one megabyte's (10 ms at a CDN, 40 ms at a blob store), so by Little's law it has `c = MB/s × that time` slots, each moving one object at a time: `capacity=100k MB/s (scenario)  service_time=10ms per MB (default)  c=1000 slots (1 object each) (implied)  queue_limit=1000 MB (default)  variability=0`. Each request is a **batch** of megabytes that holds one slot for its whole transfer (size × the time of one MB: a 50 MB object 2 s at a blob store), a fixed time for a fixed size (so `service_variability` defaults to **0** there: deterministic transfers), and the node's queue waits for whole objects: Erlang C is fed the mean object time `s_obj = Σ p_op × MB_op × time per MB` (`p_op` = the op's share of the requests, recomputed every step from the arrivals) with the same `c`, which leaves `rho` as in MB/s and makes the wait a whole object's, and the Allen–Cunneen factor `(burstiness + c_s²) / 2` is fed the size mix's squared coefficient of variation `c_s² = Σ p_op (1 + service_variability) s_op² / s_obj² − 1`. One size waits like M/D/c, half the M/M/c wait: at `rho` 0.9, 50 MB objects at a default blob store (20 slots) wait `0.5 × C(20, 0.9) × 2 s / (20 × 0.1)` = 0.28 s (0.55 s with `service_variability: 1`), where counting 1 MB units as independent requests, as the first version of the model did, said 11 ms; small requests beside large ones raise `c_s²` and so lengthen the large ones' wait (1000 reads/s of 2 KB beside 8 writes/s of 50 MB: 67 ms against 64 ms alone). Like every queue's, the wait is never longer than the waiting room takes to drain, `queue_limit / capacity` (1000 MB at 500 MB/s: 2 s). A request is then answered after that wait at its **last byte** if it is a write (an op in a blob store's `write_ops`, default `write`): its whole transfer, so every 300 MB upload takes at least 12 s at 25 MB/s per slot (40 ms per MB), and a client `timeout` under that cuts all of them; and at its **first megabyte** otherwise (a read or a stream, which the client consumes as it arrives; this understates a large read needed whole before use). Either way a request under 1 MB takes one megabyte's time (an upper bound for small objects: a 2 KB read holds its slot 80 µs but is answered in 40 ms). `explain` checks it in MB/s with the requests beside: `cdn_us  peak offered 14.08k MB/s (281.6/s) vs 100k MB/s total capacity  (14.1%, green)`. A byte rate on any other type is an error, since those types count requests: `a service counts requests; a byte capacity belongs on a cdn or blob_store`. At most `1e15/s` (`1e15B/s` for a byte rate). An error on a `leaky_bucket` limiter |
| `latency` | duration | all but `client` and `worker_pool` (its `cost` is the time per job) | read | service time of one request; default per type (50 µs for `load_balancer` and `rate_limiter`, 250 µs for `cache`: the spec's memory read). An error on a `leaky_bucket` limiter |
| `advanced` | map | all but `client` | read | see next table |
| `ops` | map | `client`, `service`, `ws_gateway` | read | client: op → share; otherwise op → step list (below; a `ws_gateway`'s message ops fanned out, see [`ws_gateway`](#ws_gateway)) |
| `cost` | map op → number | `service`, `cache`, `sql_db` | read | **new in v1.2**: the capacity units one request of that op consumes; an op not listed costs 1. Each value is a positive finite number (`cost 'write' must be a positive finite number`, also for a quoted `"3"`) of at most 1e6 (`cost 'write' must be at most 1e6`); anything but a map is `cost must be a map of op to units`, a bare `cost:` is `cost must not be empty (write {} for none)`. `capacity` is then a rate of units: demand is `Σ rate_op × cost_op`, and the node's demand, drops and backlog are reported in units. What it forwards stays requests (a drained `write` of cost 3 is one `write` downstream). A `sql_db` charges its write ops 5 unless `cost` lists them (see [`sql_db`](#sql_db)). A load balancer and a rate limiter charge 1 per request, and `cost` is not a key of theirs. A `worker_pool`'s `cost` is a different key, a duration per job (see [`message_queue` and `worker_pool`](#message_queue-and-worker_pool)) |
| `then` | list of steps | `cache`, `worker_pool`, `blob_store`, `notification_fanout`, `frontier` | read (cache, blob_store, worker_pool, notification_fanout); checked (frontier) | steps after processing; on a `cache`, steps with `on: miss` receive the flow it did not serve. Validated like any step; a cache's `then` steps run (an unconditional one carries everything the cache answered, an `on: miss` one `1 − h` of the reads and none of the writes; see [`then` steps on a cache](#then-steps-on-a-cache)), and `explain`'s paper load follows them the same way; a `blob_store`'s run as asynchronous event notifications, one per write it serves and none per read (see [Node types](#node-types)); a `worker_pool`'s fire for every job it drains (see [`message_queue` and `worker_pool`](#message_queue-and-worker_pool)); a `notification_fanout`'s fire for the `1 − dedup` of what it serves (see [`notification_fanout`](#notification_fanout)) |

A key outside its row is an error, e.g. `key 'capacity' is not valid for type 'client'`
(a client is a traffic source with no service rate, service time or waiting room) or
`key 'then' is not valid for type 'service'` (a service's steps are per op).

### `advanced`

| key | type | default | meaning |
|---|---|---|---|
| `queue_limit` | integer > 0 | 1000 (chosen) | units of work that may wait per replica (requests when every cost is 1; a request of `cost` 3 takes three); beyond it the excess is dropped, so nobody admitted waits longer than the full room takes to drain, `queue_limit / capacity` (the room-drain bound: exact for a deterministic drain such as a leaky bucket's, conservative for random arrivals and service, about twice M/M/c/K's mean wait near rho 1; with the default room it binds at a node that counts requests only when the arrivals' variability factor exceeds 10, 5 for the variance it caps at the room's scale, `(queue_limit / capacity)² / 4`, our heuristic; an estimated percentile is a lognormal fit that ignores the bound and can exceed it: [How percentiles are estimated, and where they mislead](#how-percentiles-are-estimated-and-where-they-mislead)). On a node that counts megabytes (a byte `capacity`, see [Node (common keys)](#node-common-keys)) the units are MB: the default waiting room there is 1000 MB (chosen: the generic default read in the node's unit, twenty 50 MB streams), printed `queue_limit=1000 MB`. An error on a `leaky_bucket` limiter, whose `burst` is its waiting room, and on a `worker_pool`, which waits in its queue (`node 'w': a worker pool waits in its queue: set the queue's retention`) |
| `concurrency` | integer in [1, 100000] | implied: `max(1, ceil(capacity × latency))`, capped at 100000 | servers per replica, the c of M/M/c. The cap exists because the Erlang C term costs O(c) per node per step. The default is Little's law at full load (busy servers = throughput × time in service), which keeps the rate and the Erlang C term describing one machine. An error on a `leaky_bucket` limiter (one leak slot) and on a `worker_pool`, whose c is its `workers` (`node 'w': a worker pool's rate is workers / cost`) |
| `service_variability` | number ≥ 0 | 1.0 (2.0 for `external_sink`; 0 on a node that counts megabytes, see `capacity`) | squared coefficient of variation (SCV) of service time; 1 = exponential, 0 = every request takes the same time. On a node that counts megabytes it is one object's (a transfer of a fixed size is deterministic, hence the 0), and the queue uses the SCV of the size mix built on it. An error on a `leaky_bucket` limiter (the leak is deterministic: SCV 0) |
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
| `on` | `always` | read | `miss` applies to the flow a cache did not serve, `failure` to failed requests. In an op's step list, an `on: miss` step belongs to the nearest unconditional step before it, its lookup, which must ask a `cache` for a read, leave the miss to the application, and be waited for. *(graph)* `node 'web': an 'on: miss' step must follow a cache step`; `... cannot follow a write to cache 'c' (writes do not miss)`; `... cannot follow read_through cache 'c' (the cache already fetched the miss)`; `... cannot follow cdn 'edge' (the cdn already fetched the miss from its origin)` (a CDN fetches its own misses, see [`cdn`](#cdn)); `... and the cache step it follows must be sequential (the miss step waits for the cache's answer)`. A miss step is taken by the miss fraction `1 − h` of the requests (`h` the cache's hit ratio, stated or [resolved at build](#che-hit-ratios); a cache built in code without one counts as `h = 0`, and `explain` marks the checks that rest on it). After a lookup of `factor: k` it sends `k(1 − h)` requests per request: every missed lookup is fetched. Several `on: miss` steps after one lookup form one block that a miss takes whole, one step after another: the response adds `1 − h` × the sum of their hops on average, with the hit/miss spread in the variance, and a miss is answered only if every fetch is. (For latency, a request's `k` lookups are assumed to hit or miss together.) **`on: failure` steps fire** (phase 3, spec §5): `[{to: db, op: write}, {to: log, op: write, on: failure}]` sends `log` the requests whose `db` write failed (lost at `db`, or at a hop inside its own steps), the dead-letter or compensation path of a write that failed. A failure step belongs to the nearest unconditional step before it, after a parallel group to the group as a whole, and two failure steps in a row both belong to that step. It fires per attempt: with a client's [`retries`](#node-types), every failed attempt is logged (a client sending 1750 attempts/s of which half fail sends `log` 875/s, not the 125/s of requests lost for good). After a cache lookup that `on: miss` steps follow, it belongs to the lookup together with its miss block (a read whose miss the database failed is a failed read): `d = d_c × ((1 − f) + f · Π d_miss)`, `f` the miss share and `Π d_miss` over the lookup's miss steps, those after the failure step included (a failure step does not split a miss block: the miss steps of one lookup are one block on both sides of it). So `[{to: c, op: read}, {to: db, op: read, on: miss}, {to: log, op: write, on: failure}]` with `c` hitting half the reads and `db` answering half its misses sends `log` `1000 × 0.5 × (1 − 0.5)` = 250/s of 1000. It is sent `rate × S × (1 − d) × factor`, with `d` the delivered fraction of the step it reports on (`Π d` of a group's members), read one simulation step late like every thinning (so `log` is sent nothing at the first step), and `S` the share the steps before that step let through: it is thinned by them, not by the step it reports on (a request that failed there is exactly the one sent on). After a step that loses nothing it sends nothing. It is a sequential hop, and its own losses fail nothing: they thin no later step and fail no request, and its latency is not in the op's response (no answered request takes it). It may not close a call cycle: a failure step that calls back into its own op (`web.write: [{to: db}, {to: retrier, op: retry, on: failure}]`, `retrier.retry: [{to: web, op: write}]`) retries every failure, and every failed retry again, with no bound; *(graph)* `node 'web': an on: failure step to 'retrier' closes a call cycle (an unbounded retry: use the client's retries)`. One that publishes to a `message_queue` whose worker pool calls the op again builds (the queue answers at the append, so the call cycle ends there), and is a loop of flow that retries every failure for ever, with no limit and no backoff (ch. 10's notification system retries a few times, then gives up): a pool's `retries` (phase 3) is the modelled form. `explain`'s paper load, where nothing fails, counts it as sending nothing, and the check line of a node only failure steps reach says so: `peak offered 0/s vs 2k/s total capacity  (0%, green) (fed by on: failure steps only: sized by run/estimate)`. The size walk prices it like any step: an unsized failure step over a bandwidth link is refused like any unsized step (`link 'web'->'log' has a bandwidth, but op 'write' from 'web' has no size: add size: to the step or sizes: to the client`). *(graph)* `node 'web': an on: failure step needs a step before it`; `node 'web': an on: failure step must be sequential (it runs after the step it reports on has failed)`. In a `then` list a failure step is refused: at a cache `node 'c': a then step on failure is not modelled at a cache (an on: failure step reports on a step of an op's step list)`, and at a blob store and a worker pool (below) |
| `mode` | `sequential` | read | sequential steps add their latencies; **the parallel steps of an op form one group** and must be adjacent (before, between or after the sequential ones), and the group costs its slowest branch. *(graph)* `node 'web': the parallel steps of op 'get' must be adjacent (they form one group)`; for `then` steps, `node 'c': the parallel then steps must be adjacent (they form one group)` |
| `factor` | 1 | read | finite, positive, at most `1e6`; multiplies the flow (fan-out: one post → 100 feed writes) |
| `size` | none | read | a [size](#values): the payload this step's request carries (a transcoder's 50 MB rendition), what a byte-sized node charges the request, in MB; positive (`step size must be a size (B, KB, MB or GB: 50MB): missing size unit`, the parser's reason after the colon; `step size must be positive` for `0B`) |

**A step after a failed step does not run** *(phase 2c)*: a request is answered only if every
step it waits for is, so each step is sent only the requests the steps before it delivered
(sequential steps thin one another, the parallel group fails as a whole and thins the steps
after it, a miss block fails only its misses), at a service, at a
cache (store hop and `then` steps) and at a worker pool (its `then` steps). The fractions are the
previous simulation step's, so the thinning is late by the delay of the link to the failing step's
target plus one step: the first step sends every step the full flow, and behind a 500 ms link at
100 ms steps the later steps receive the full flow until 0.6 s. When an op calls one overloaded
node several times (`[a, a, a, a, a]`), each call thinned by the node's own loss, that lag can lock
`run` into a sawtooth: one step offers the node much more than it drains (red, dropping), the next
much less (thinned by the previous step's drops), and so on for the whole run. `estimate` thins by
fractions averaged over its passes, d̄ ← (1 − w) d̄ + w d, and halves a fraction's weight w
(from 1/2) each time its average overshoots, so it settles at the true fixed point (five calls at
1k/s to a node of 3686/s and a room of one: 10 % dropped, 4095/s offered, where the delivered
fraction's slope against itself is −1.79; ten and twenty calls to a node sized so that 10 % is
dropped again, 5862/s and 7906/s, slopes −3.65 and −6.23, as well). The one limit: w stops at
1/1024, which settles any slope down to −2047 (at the same 10 % sizing twenty calls are −6.2;
a thousand calls, sized to settle at 99.9 % delivered, are −417). If the fractions still do not
settle, it warns `no steady state (a loop of
gain >= 1 or a retry storm that no capacity stops, or delivered fractions that did not settle: a
step after a failed step)`. A client's [`retries`](#node-types) are such a loop: each pass sends
again what the pass before failed, read off the damped fractions, until the attempts stop moving,
and `estimate` warns if they do not settle; so are a worker pool's
[`retries`](#message_queue-and-worker_pool), each pass re-publishing to the queue what its sinks
failed in the pass before.

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
| `client` | `rate` (rate curve), `ops` (map op → share), `keys` `{zipf: s, space: N}` or `{uniform: N}`, `sizes` (map op → size), `timeout`, `retries`, `retry_after` † | `rate` (every term, see [Rate curve](#rate-curve-clientrate)) and `ops` read; `keys` read by the [`che` caches](#che-hit-ratios) downstream (`keys must be a map`, `keys needs zipf and space, or uniform`, `keys takes zipf+space or uniform, not both`, `keys.zipf must be a finite non-negative number`, `keys.space must be a positive count (a whole number, optionally with k, M or G: 500, 10k, 100M)`, likewise `keys.uniform`; `space` and `uniform` are [counts](#values): `100M`); `timeout` read (a duration; absent: the client waits for ever): each step the client's response to an op is fitted as a lognormal, and the share slower than the timeout is timed out instead of answered (`timed_out_per_s` in JSON and in `run`'s `timed out/s` column, see [Drops and rejections](#drops-and-rejections)), the mean is the answered share's (the lognormal's truncated moments), a percentile at q is the fit to every delivered request's latency (the moments before the cut, the fit that decided who timed out) read at q and capped at the timeout, or is the timeout itself (`>1s` in the table) when more than its share of the requests timed out; it must be positive (`node 'u': timeout must be positive`); the servers still do a timed-out request's work, and only `retries` sends it again; `sizes` checked: each names an op the client sends and is a [size](#values) (`sizes names op 'x', which the client does not send`, `sizes 'stream' must be a size (B, KB, MB or GB: 50MB): size must be at most 1e15B`, the parser's reason after the colon; `sizes 'stream' must be positive` for `0B`, a size but no request), read by byte-sized nodes (each request costs its size in MB); `retries` read (phase 3; a whole number from 0 to 10, default 0: `node 'u': retries must be a non-negative integer` for -1, 1.5, a word or a quoted `"2"`, `node 'u': retries must be at most 10`) with `retry_after` (a positive duration, `node 'u': retry_after must be positive`, and only beside `retries`: `node 'u': retry_after needs retries`; absent: 100 ms, **chosen, not from the book**: one default step, about the latency table's cross-continent round trip): every request the client lost on its path or timed out is sent again `retry_after` later, up to `retries` more times; the engine moves in whole steps, so the wait is `retry_after` / `step` rounded **up**, at least one step (a retry never fires before the stated wait: 150 ms at 100 ms steps waits 2 steps, 200 ms; 250 ms waits 3), and `explain` prints the steps after a wait that is not whole (`retries=2 after 150ms (2 steps)`); each attempt fails at the share the path fails in the step it is sent, so a path that fails a share q is sent 1 + q + … + q^n attempts per request and answers 1 − q^(n+1) of the requests (n = 2 at q = 0.5: 1.75 attempts, 87.5 % answered); the servers do every attempt's work; `offered/s` stays the rate curve and `run`'s `attempts/s` column (JSON's and the op series' `attempts_per_s`) shows the retries above it; a request is answered once, and a request whose last attempt failed is lost (or timed out) once (`timed_out_per_s`), while the share of timed-out responses a percentile reads counts every attempt (`timed_out_attempts_per_s` ÷ the attempts delivered: a client that retried a 10.6 % tail still waited past its timeout on 10.6 % of its attempts, so its p99 prints `>200ms` and a p99 SLO fails); `estimate` iterates the retries to their fixed point and warns if they do not settle; ignored: jitter, exponential backoff, retry budgets and idempotency (a retried write that had succeeded), and the time a retried request spent on its failed attempts: its latency is its last attempt's, without the k failed attempts' waits (τ for a timed-out one, ~0 for a lost one) and the k backoffs before it; a proper model would make the response a mixture over the attempt a request is answered on (attempt k + 1 with probability q^k (1 − q), taking k × (τ or 0) + k × `retry_after` more), phase 4 material |
| `cdn` | `origin` (node id, required), `hit_ratio` (`{fixed: h}` or `che`, optional), `keys`, `ttl`, `write_ops` | read and run (phase 2b), see [`cdn`](#cdn) |
| `load_balancer` | `policy`, `vnodes` | read and run, see [`load_balancer`](#load_balancer) |
| `rate_limiter` | `algorithm`, `limit`, `burst`, `window`, `backend` | read and run, see [`rate_limiter`](#rate_limiter) |
| `service` | `ops`, `cost` | read and run |
| `cache` | `hit_ratio`, `strategy`, `write_ops`, `backend`, `keys`, `eviction`, `ttl`, `then`, `cost` | read and run (fixed or `che` hit ratio), see [`cache`](#cache) |
| `sql_db` | `replication_lag`, `shards` (int), `pool` (int connections), `write_ops`, `cost` | read and run (phase 2b), see [`sql_db`](#sql_db) |
| `kv_cluster` | `nodes` (int, required), `vnodes`, `replication` N, `write_quorum` W, `read_quorum` R, `mode` `ap|cp`, `write_ops` | read and run (phase 2c), see [`kv_cluster`](#kv_cluster); `mode` printed, not modelled (partitions are phase 3) |
| `message_queue` | `partitions`, `retention` | read and run (phase 2c), see [`message_queue` and `worker_pool`](#message_queue-and-worker_pool); `partitions` printed, not modelled |
| `worker_pool` | `consumes` (queue node id, required), `workers`, `cost` (duration per job), `then`, `retries`, `retry_after` | `consumes`, `workers`, `cost` and `then` read and run (phase 2c), same section: the steps fire for every job it drains, `on: miss` and `on: failure` refused; `retries` and `retry_after` read and run (phase 3), same section: the jobs whose call to an external sink failed go back on the pool's queue, read again by this pool only, and the last attempt's failures are dead letters (the `dead_letter_rate` SLO) |
| `blob_store` | `then`, `write_ops` (its byte-rate `capacity` is the common key) | the store runs, counting megabytes; `write_ops` read as a [cache's](#cache) (default `[write]`): the ops it acknowledges at their last byte, every other op answered at its first megabyte (youtube's `original_store` lists `upload`), shown by `explain` as `write_ops=upload`, with `(never arrives)` when none of them reaches the store. Its `then` steps run as **event notifications** (S3 event notifications; ch. 14's "transcoding starts when the upload completes"): every **write** the store serves (an op in its `write_ops`; not one it drops) sends `factor` requests of the step's op to its target, and a read sends nothing (S3 notifies on ObjectCreated and ObjectRemoved, never on a GET: 1000 views/s of a video start no transcode; notifying on reads is phase 3). They are **asynchronous**: the uploader's response and answered rate end at the store, and what the notified node drops counts in that node's drop rate only; `explain` follows them. A step `on: miss` (`node 'store': a blob store's then steps fire for every write it stores (on: miss needs a cache)`) or `on: failure` (`... (on: failure: an event reports no failure before phase 3)`) is refused, and so is any `mode` (`node 'store': a blob store's then steps are event notifications nobody waits for (mode orders the calls a request waits for)`); an event keeps the request's size when the step keeps its op, else it carries the step's `size:` or none. Ignored: event batching, the delivery delay (an event arrives after the link's latency), failed notifications |
| `search_index` | `build_interval` (positive duration) | read and run (phase 2c), see [`search_index`](#search_index); `build_interval` printed, not modelled |
| `ws_gateway` | `max_connections` (count), `heartbeat` (duration), `fanout` (avg recipients per message, ≥ 1), `connections` (count: a fixed level), `session` (duration), `reconnect_within` (duration), `ops` | read and run (phase 3), see [`ws_gateway`](#ws_gateway); `reconnect_within` printed and stored for the reconnect storm (phase 3 Task 15) |
| `service_discovery` | `failover` (positive duration) | read and run (phase 3), see [`service_discovery`](#service_discovery): a service answering `lookup`; `failover` (default 10 s, chosen) is how long its callers stay on a dead replica after a `node_down`; `node 'disc': failover must be positive`; on any other type `node 'api': key 'failover' is not valid for type 'service'` |
| `id_generator` | `algorithm` `snowflake|ticket_server|uuid` (required) | read and run (phase 2b), see [`id_generator`](#id_generator) |
| `external_sink` | `failure_rate` (0–1), `politeness` (rate per host) | read and run (phase 2c): a third party's API (ch. 10's APNs, FCM, SMS and e-mail providers), a leaf queue of the type's numbers (1k/s, 100 ms, service SCV 2.0) whose served requests fail at `failure_rate` f (default 0; outside [0, 1]: `node 's': failure_rate must be between 0 and 1`): `dropped += f × served`, `drain = (1 − f) × served`, so 500/s at f = 0.02 is answered 490/s and the failures count in the node's drop rate, never as rejections. `politeness` (a positive rate) is printed by `explain` as `politeness=1/s (not modelled: phase 3)`: it needs per-host state. `then` and `ops` stay refused; a client's `retries` and a [worker pool's](#message_queue-and-worker_pool) send a failure again (the sink's drop rate leaves out the share of its drops that came from retrying pools) |
| `notification_fanout` | `channels` (map channel name → message queue id), `dedup` (in [0, 1)), `then` | read and run (phase 3), see [`notification_fanout`](#notification_fanout) |
| `frontier` | `politeness` (rate per host), `hosts` (int), `then` | `then` checked as [steps](#steps); the others ignored; phase 3 |

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
| `consistent_hash` | imbalanced | the hottest of the node's M copies carries `k = 1 + z_M/sqrt(vnodes)` × the average share, `z_M` the expected maximum of M standard normals (Blom: `Φ⁻¹((M − 0.375)/(M + 0.25))`, 1.05 for 4 replicas; capped at M; the kv_cluster's ring below with one copy per key, see [numerics, section 8](study/numerics.md) for the derivation and its Monte Carlo check), and is queued apart from the M − 1 cool copies, which share the rest; the node reports the hot copy's rho and zone and, as latency, the mixture a request sees (hot with probability k/M) |

`explain` prints the factor on the node behind (`imbalance=1.1 (vnodes 100)`) and a
`hottest copy` figure in its checks. The factor is a heuristic: a server's share spreads about
1/√vnodes around the average (ch. 5 gives about 10 % at 100 vnodes), and the hottest of M
servers sits z_M such spreads above it, so the factor grows with the number of servers (for 4:
1.105 at 100 vnodes, 1.086 at 150). Monte Carlo rings put it within about 1 % at
100 vnodes and more; at `vnodes: 1` it is off by up to ±16 % (two servers: exactly 1.5 against
1.589), [numerics, section 8](study/numerics.md). *(graph)* rules:

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
| `backend` | a declared node of type `cache`: the counter store | `backend refers to unknown node 'x'`, `backend must be a cache`; *(graph)* the limiter waits for its counter, so a store that calls back into the limiter (through its own backend or steps) is refused: `node 'lim': backend 'counters' closes a call cycle` |

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
request itself. Its caller sends its own later steps only the requests the gate let through
(a step after a failed step does not run, see [Steps](#steps)), one simulation step plus the
link's delay late; the
refused share shows in the gate's reject rate and in the client's answered rate.

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
| `backend` | a declared node; required by every strategy but `cache_aside`, and refused there (the application reads the store; the backend would never be called) | `strategy read_through needs a backend`, `backend is unused with strategy cache_aside`, `backend refers to unknown node 'x'`; *(graph)* `backend 'db' needs a link from the cache`, and the link must carry every op the cache passes on (`link c -> db does not carry op 'read' (ops: write)`); a fetch on any call cycle is refused, since a fetch is always waited for: a store that calls back into the cache (`db.read -> c.read -> db.read`), whether the clients call the cache, the store or a service in front: `node 'c': backend 'db' closes a call cycle` |
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
| `failure` | refused (`node 'c': a then step on failure is not modelled at a cache (an on: failure step reports on a step of an op's step list)`): failure steps fire in an op's step list (see [steps](#steps)) | — | — |

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
together. The ring's arcs are uneven: with imbalance `k = 1 + z_M/√vnodes` (the load balancer's
model, M the replicas) the hottest of the `M` replicas owns `a_hot = k/M` of the keys and the
others `a_cool = (1 − k/M)/(M − 1)` each. A replica owning a share `a` of a random partition is,
for Che, one cache of `keys / a` over the whole space, and serves `a` of the requests, so
`h = Σ_j a_j · Che(keys / a_j)`. The hit ratio is concave in the cache size, so uneven arcs hit
less than one cache of `keys × M`: the consistent-hashing lab's four 2M-key servers with
`vnodes: 1` (`k = 2.05`) give 0.920, where even arcs would give 0.963. A cache the ring partitions
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
| `origin` | required; a declared node that is not a client and not the CDN itself, with a link from the CDN. Another CDN is fine: a tiered CDN (edge → regional → origin), whose second tier's `che` is computed over the first tier's misses | `node 'edge': a cdn needs an origin`; `node 'edge': origin refers to unknown node 'x'`; `node 'edge': origin 'u' is a client; clients only send`; `node 'edge': a cdn cannot be its own origin`; *(graph)* `node 'edge': origin 'x' needs a link from the cdn`; `node 'edge': origin 'x' closes a call cycle` (the origin calls back into the CDN, so a miss would wait for itself; refused wherever the clients enter the loop) |
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

The wire behind a CDN is a link's business: give the link to the origin a `bandwidth` and it is
a pipe of its own ([Links](#links)), as youtube's cross-DC `cdn_eu->transcoded_store` is.

Not modelled: the transfer time of a stream beyond its first megabyte,
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

A copy at **exactly** rho = 1 (its load equal to its capacity) keeps its queue empty in both
commands (see [a node at exactly rho = 1](#what-runs-in-phase-2c)) and waits the stochastic wait at
the 0.99 clamp: 200 inserts/s on 2 copies of 1k/s puts both the primary and the replica at exactly
1, and `estimate` and `run` both report a staleness of 250 ms (a 100 ms sojourn at each, + 50 ms).
A real database there would wander anywhere up to two full queues (2.05 s): size a database off
rho = 1, as the examples here are.

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

### `kv_cluster`

Book ch. 6, "Design a key-value store", Dynamo-style (Amazon's Dynamo, Cassandra), in the steady
state (spec §19 entry 9). `nodes` machines sit on a consistent-hash ring (ch. 5) of `vnodes`
virtual nodes each; every key lives on `replication` N of them, the N machines that follow it on
the ring; the coordinator sends every read and every write to all N replicas and answers a write
once W = `write_quorum` of them have acknowledged it and a read once R = `read_quorum` have replied
(ch. 6, "quorum consensus"). `capacity`, `latency` and `advanced` describe **one machine** (default
50k/s, 150 µs: spec §6's "KV 50k" and a NoSQL read).

| key | rule | message on violation |
|---|---|---|
| `nodes` | required, a positive integer: the machines | `node 'store': kv_cluster needs nodes`, `nodes must be a positive integer` |
| `vnodes` | a positive integer; default 150 (the balancer's, ch. 5) | `vnodes must be a positive integer` |
| `replication` | a positive integer of at most `nodes`; default **3** (ch. 6's N) | `replication must be a positive integer`, `replication must be at most nodes (3 > 2)` (the default counts: a two-node cluster must lower it) |
| `write_quorum`, `read_quorum` | positive integers of at most `replication`; default **min(2, N)**: ch. 6's W = R = 2, lowered to 1 when `replication: 1`. W + R ≤ N is accepted: it is the eventually consistent end of ch. 6's dial (W = 1 a fast write, R = 1 a fast read; a read may miss the latest write) | `write_quorum must be a positive integer`, `write_quorum must be at most replication (4 > 3)`, the same for `read_quorum` |
| `mode` | `ap` (default) or `cp`: what a partition costs, staleness or unavailability (ch. 6, "CAP theorem"). Read and printed; it changes nothing until partitions exist (phase 3) | `mode must be ap or cp` |
| `replicas` | refused: a cluster scales by `nodes` and keeps `replication` copies of a key, two things `replicas` could mean | `node 'store': a kv_cluster's machines are nodes; replicas is not a key here` |
| `write_ops` | a list of op names, as on a [cache](#cache): the ops that wait for the **write** quorum W; every other op waits for R. Default `[write]` (the default of every node but a database). A client that sends `put` straight to the cluster lists it here | `write_ops must be a list of op names`, `write_ops must name at least one op` |

`Graph::build` refuses the same shapes in the same words for a scenario built in code.

**N times the requests.** Every request is N replica-requests: a request costs N capacity units
(as a database write costs 5), so the node's demand, drain, drops and backlog, and the paper load,
count replica-requests, N times what the clients sent, against `nodes ×` one machine's
`capacity`. The clients' offered and answered rates stay in requests. The kv-store lab's 30k
requests/s at N = 3 are 90k replica-requests/s: over 8 machines of 10k/s 11.25k/s each, 112.5 %;
over 16, 5.625k/s, 56.25 %.

**The ring's hottest machine.** With N < `nodes` each machine holds the keys of the arcs before
its virtual nodes, N times over (it is one of the N successive replicas of each), so its share is
a sum of N × `vnodes` arcs and spreads about 1/√(N·vnodes) around the average. The hottest of the
M = `nodes` machines sits z_M such spreads above it, z_M the expected maximum of M standard
normals (Blom's approximation, `Φ⁻¹((M − 0.375)/(M + 0.25))`: 1.43 for 8 machines, 1.77 for 16,
2.58 for 128), so it carries

```
k = min(1 + z_M / √(N · vnodes), nodes / N)
```

times the average (1.068 for 8 machines, 1.083 for 16 at N = 3 and 150 vnodes), at most
`nodes / N` (a machine is one of a key's N replicas at most once). The same function gives the
consistent-hash balancer's ring with N = 1 ([`load_balancer`](#load_balancer)), so the two agree.
It is a heuristic, checked against Monte Carlo rings ([numerics, section 8](study/numerics.md):
the expected hottest over the average is 1.073, 1.091 and 1.133 for 8, 16 and 128 machines at
N = 3 and 150 vnodes, where k gives 1.068, 1.083 and 1.122): within about 1 % from 100 vnodes on,
but off by up to ±16 % at `vnodes: 1`, where a machine's share is a few whole arcs (16 machines:
1.962 against k = 2.021; 128: 2.943 against 2.492). The old `1 + 1/√vnodes` ignored the
number of machines and understated the hot spot beyond about 16. The engine runs the hot machine
and `nodes − 1` cool ones as two copy groups, as behind a balancer's ring: the node's `rho` and
zone are the hottest machine's, so a `utilization` SLO reads it (the lab at 16 nodes: 1.0834 ×
0.5625 = **0.6094**). With N = `nodes` every machine holds every key: k = 1, no hot machine.

**The quorum's latency, over the request's real replica set.** A read is answered at the
**R-th fastest** of its N replicas, a write at the **W-th** (an op in `write_ops`). The N replicas
are N different machines, so at most one is the hot one; it is sent k / `nodes` of the
replica-requests, one from each request whose key it holds, so a request's set includes it with
probability

```
p_hot = N · k / nodes            (16 machines, N = 3, k = 1.083: 0.203)
```

and is otherwise N cool machines. Each machine's reply time is a lognormal fitted to its own copy
group's sojourn mean and variance (wait + service), and the q-th fastest of the set comes from
its survival function: `S_q(t)` = P(fewer than q of the N have answered by t), a
Poisson-binomial over the hot and cool replicas, integrated as `E[X] = ∫ S_q dt`,
`E[X²] = ∫ 2t S_q dt` on a grid even in ln t ([numerics, section 7](study/numerics.md): under
1e-8 relative error for a sojourn SCV from 1e-4 to 100). The response is the latency of the
requests ANSWERED: it mixes the two kinds of set (the law of total variance) by the share of the
answered requests each gives, `p_hot (1 − L_hot)` and `(1 − p_hot)(1 − L_cool)` over `1 − L`,
with the losses `L` below, so a set that loses more weighs less, and a set that answers nothing
not at all. So a quorum of 2 of 3 answers from the two cool replicas when the hot machine's queue
is full: the kv-store lab with `vnodes: 1` (the hot machine at 114 %) answers in 0.378 ms and
passes its p99, where a quorum of 3 waits for the full queue. For
replicas all alike and exponential of mean m the closed form is Rényi's (the k-th smallest of n:
`m × Σ_{i<k} 1/(n − i)`, 1/3, 5/6 and 11/6 of m for n = 3), which the quadrature reproduces and
the lognormal fit departs from a little (0.421, 0.826 and 1.753 m at SCV 1).

**The quorum's losses.** A request is lost when fewer than q of its N replica-requests come back,
each kept with its machine's kept share this step (1 − dropped / sent): the Poisson-binomial
again, mixed by p_hot. Eight saturated machines that each drop about a ninth lose 3.4 % of the
W = R = 2 requests (0.14 % at q = 1, 29.8 % at q = N), not the ninth that "the share of
replica-requests dropped" gives at every q: that rule over-counted a forgiving quorum and
under-counted a strict one.

`explain` prints the shape, `nodes=16 N=3 W=2 R=2 mode=ap (partitions: phase 3)  imbalance=1.08
(vnodes 150)` (no `replicas=`), each of N, W, R and the mode marked `(default)` when the scenario
left it (`N=3 (default) W=2 (default) R=2 (default) mode=ap (default; partitions: phase 3)`).
Quorums that do not overlap add `(W + R ≤ N: a read may miss the latest write)` after R, and
`mode: cp` with them adds `cp needs W + R > N: these quorums contradict it` to the mode's note.
It checks the replica-requests against the machines, the hottest machine's percentage when
N < `nodes`: `peak offered 90k/s (30k/s × N 3) vs 80k/s (8 nodes)  (hottest 120%, red)`; with
N = `nodes`, the plain average, `peak offered 3k/s (1k/s × N 3) vs 30k/s (3 nodes)  (10%,
green)`.

Not modelled: network partitions and what `ap` or `cp` does under them, sloppy quorum and hinted
handoff, node failures (faults: phase 3), read repair and anti-entropy (Merkle tree) traffic,
vector clocks and conflict resolution, gossip, the storage engine (commit log, memtable,
SSTables, compaction), the latency of a request that lost a replica-request but still met its
quorum (timed as if all N replied), the replicas' reply times being exactly lognormal (a Monte
Carlo of the lab's exact M/M/3 sojourns is 2.8 % slower) and independent (one hot queue serves
many keys), the spread among the cool machines (one group), and which keys are hot (a Zipf hot
key is the caches' business). The coordinator is no part of the node: the lab's `coordinator`
service is its hop.

### `id_generator`

Book ch. 7, "Design a unique ID generator in distributed systems" (spec §18 entry 12). An id
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

### `search_index`

Book ch. 13, "Design a search autocomplete system" (spec §19 entry 10). A search index is
**served like a service**: no steps, `replicas` copies of the type's 5k/s and 5 ms (chosen: a
prefix lookup in a trie that reads the prefix's stored top k, ch. 13) unless the scenario states
its own `capacity` and `latency`, resolved as for any node. `build_interval` (a positive
duration, `build_interval: 168h`) is stored and printed by `explain` as
`build_interval=168h (rebuild load not modelled)`; the rebuild's load on the index and the
freshness of its answers between rebuilds are ignored. `ops` stays refused: an index answers a
query and calls nobody.

### `ws_gateway`

Book ch. 12, "Design a chat system" (phase 3 spec §8.2): the **stateful** chat servers every
online user keeps one websocket open to, which relay messages, carry the presence heartbeats and
push messages to their recipients; and ch. 15's notification long polls (google-drive). A request
leaves a node once served; a connection **stays**, so a gateway holds a **level** C of open
connections, not a queue of work:

    C(k+1) = min(C(k) + (connects − C(k) / session) × dt, cap)       one Euler step per step

`connects` are the requests of the clients' op named **`connect`** that reach the gateway (its
share of the client's rate, directly or through a load balancer) and that its queue served;
each open connection ends at rate `1 / session`, so C is a first-order lag that rises from 0
(`C(t) = λ × session × (1 − e^(−t / session))`, 63 % of the way after one session) to the fixed
point **`C* = connect rate × session`** (Little's law: 100 connects/s that stay 1000 s hold 100k
connections), which `estimate` reports. The record of a step holds the level the step **started**
with (0 at the first step). The keys:

| key | type | meaning |
|---|---|---|
| `max_connections` | count (`100k`, `2M`) | the connections one replica holds at most; the **cap** is `max_connections × replicas`. A connect that would take the level past it is **refused**: a rejection (`rejected_per_s`, the node table's `rejected`), not a drop, and not answered, so the client's `connect` is answered for the ones that got in (at the cap: `connects − cap / session`, 50 of 100 a second at a cap of 50k and a session of 1000 s). Absent: no cap. `node 'gw': max_connections must be a positive integer` |
| `heartbeat` | positive duration | every open connection pings this often, so the gateway serves **`C / heartbeat`** pings a second on its `heartbeat` op, added to what the clients send under that name, and its `heartbeat` steps (to a presence service) carry them on: 100k connections at the book's 5 s are 20k/s. Absent: no heartbeat load. `node 'gw': heartbeat must be positive` |
| `fanout` | number ≥ 1 | the average recipients of a message: every step of a **message op** (every op but `connect`, `heartbeat` and `deliver`) sends `fanout` × its own `factor` requests (1.4: 100 sends/s are 140/s to the step's target). `deliver` is terminal (see `ops`). Absent: 1. `node 'gw': fanout must be at least 1` (also for 0), `node 'gw': fanout must be at most 1e6` |
| `connections` | count | a **fixed** level, for a gateway no client connects to (google-drive's 2M long polls, held by devices nobody here simulates): C is that number on every step, with no connects, disconnects or refusals; only its heartbeats load anything. `node 'gw': connections must be a positive integer` (also for `1.5`) |
| `ops` | map | a service's op → step list, three names with a meaning of their own: `connect` (raises the level; no steps needed), `heartbeat` (the open connections' pings arrive under it; its steps, to a presence service, carry them on) and `deliver`, a message pushed to its recipient over the open connection, which ends there: write `deliver: []`. Every other op is a message, fanned out. `node 'gw': op 'deliver' is terminal on a ws_gateway (write deliver: [])` for a `deliver` with steps (on any other type `deliver` is an ordinary op) |
| `session` | positive duration | how long a user stays connected on average. Absent: **30 minutes, chosen, not from the book** (ch. 12 never says how long a connection lasts; half an hour is a typical mobile chat session). `node 'gw': session must be positive` |
| `reconnect_within` | positive duration | how long the users of a fallen gateway take to come back. Absent: **30 seconds, chosen, not from the book** (a few of ch. 12's 5 s heartbeats of a client's backoff). Stored and printed for the reconnect storm of a `node_down` (phase 3 Task 15); nothing reads it yet. `node 'gw': reconnect_within must be positive` |

`connections`, `session` and `reconnect_within` on any other type are refused like every key
outside its row (`node 'svc': key 'session' is not valid for type 'service'`). The loader does not
decide whether a `connect` op reaches a gateway, which is the graph's fact: `run` and `estimate`
refuse a gateway with **neither** level (`node 'gw': a ws_gateway needs a client op named 'connect'
or a fixed 'connections'`) or with **both** (`node 'gw': a ws_gateway takes a 'connect' op or a
fixed 'connections', not both`), while `validate` and `explain` take it.

Its queue is the gateway relaying frames: the type's 50k/s and 100 µs unless stated (the phase 3
plan said 10k/s and 0.5 ms; the numbers the type had since phase 1 were kept), and its demand is
every request that reaches it: connects, messages, deliveries and heartbeats. A fault derates it as
any node's queue; the reconnect storm after a `node_down` is phase 3 Task 15.

`explain` ends the gateway's line with its level, heartbeat load and cap:
`  connections: C*=100k at the peak connect rate (connect 100/s × session 16.7m), heartbeat 20k/s,
cap 200k` (the connect rate the clients' **peak** `connect` that reaches it, so off the peak of a
curve the level is lower; `session 30m (default)` when left out; `= 100k, capped` after a fixed
point the cap holds below it). The level is a first-order lag of time constant `session`, 95 % of
the way after 3 of them (1 − e^−3), so when the scenario's `duration` is shorter than 3 sessions
and the fixed point is not capped, the line ends `; settles in about 3 sessions (50m); this run
lasts 1m`: the reports' `connections` will be below C*. A fixed level reads `  connections: 2M
(fixed), heartbeat 33.33k/s, cap 2M`, and the line says so for a gateway `run` refuses. Its checks count the
heartbeats in the gateway's and in its presence service's paper load, and the fan-out on the
message steps' targets.

`run`'s node table gets a **`connections`** column after `peak_lag` when the scenario has a
gateway (the level at the end of the run on the gateway's row, `-` elsewhere); JSON's node objects
get `connections` (the level at the end) and `connections_peak` (the highest of any observed step),
`null` off gateways, also only when the scenario has one; the series' last column is
`connections` (below). Ignored: the memory a connection costs (the cap stands for it), which server
a user is on (the service-discovery hand-off of ch. 12, phase 3 Task 13: every replica holds an
equal share), the reconnect storm (Task 15), users who vanish without a disconnect (every session
ends at rate `1 / session`) and the bytes of a frame.

### `service_discovery`

Book ch. 12, "Design a chat system" (phase 3 spec §8.1). The chat client does not reach a chat
server through a load balancer: it asks the **service discovery** (ZooKeeper) for one, and then
talks to the server it was handed. A service discovery is **served like a service**: `replicas`
copies of the type's 50k/s (spec: a lookup is a small read from an in-memory registry, so one
registry answers a whole tier) and 0.5 ms (the latency table's round trip within the data centre)
unless the scenario states its own `capacity` and `latency`; `c = 50,000/s × 0.5 ms = 25` servers
by Little's law. It answers the **`lookup`** op its callers send, and a lookup is a step like any
other: it adds the discovery's sojourn and the link's round trip to the caller's response.

**The lookup rule.** An op reaches a node *through a lookup* when its step list has a step to a
`service_discovery` asking `lookup` anywhere before its (first) step to that node:
`api: {ops: {call: [{to: disc, op: lookup}, {to: svc, op: call}]}}` looks `svc` up; `[{to: svc,
op: call}, {to: disc, op: lookup}]` does not (the caller had the address already), and neither does
a step asking a plain service for an op named `lookup` (a cache's lookup is the cache's business).
The rule is per op: another op of `api` that calls `svc` without a lookup is not held.

**`failover`** (a positive duration, default **10 s, chosen, not from the book**: a registry learns
of a death when the dead server's session or health check times out, which ZooKeeper's session
timeouts and the usual health-check settings put at about ten seconds; filled in by the loader, so
`explain` ends the line with `failover=10s`). When a `node_down` takes r of the R replicas of a
node that some op reaches through a lookup, the registry still hands out the dead replicas for
`min(failover, for)` from the fault's start: the share **r / R of that op's requests** is sent to
them and **lost at the node**, where it counts as demand and as drops, never as drain, on top of
whatever the survivors cannot take. The other (R − r) / R reach the survivors, which keep the load
per copy they had (their capacity is (R − r) / R too). When the window closes, every request goes
to the survivors, which then carry R / (R − r) times the load per copy and queue. The probe of
`tests/theory/test_faults.cpp`: 1k/s through `[{to: disc, op: lookup}, {to: svc, op: call}]` into
4 replicas of 1k/s at 1 ms, one replica down from 10 s for 30 s, `failover: 10s`: for 10 s 250/s
are lost (svc: demand 1000/s, dropped 250/s, drain 750/s, rho 0.25), then nothing is lost and rho is
1/3 (1.5 ms instead of 1.33 ms), and when the replica is back rho is 0.25 again. A node_down of
**every** replica makes the share 1: for the window each looked-up request is lost at once rather
than waiting in the node's room for it to come back.

No failover without a lookup: a `node_down` of a node reached directly (a load balancer in front,
or no registry) just removes capacity, and the balancer spreads the load over the survivors from
the first step. Nor for a `capacity` fault (every copy stays alive), a `dc_down` (its links are
cut), or a `sql_db`'s or `kv_cluster`'s partial `node_down` (their clients find the live copies by
the database's own replication and quorums). The fault's block says what the failover cost (see
[Faults](#faults)): `after:    no backlog to drain, failover 10s: 2.5k requests sent to the dead
replica`.

Ignored: the registry's own availability (a `node_down` of the discovery itself is a plain loss of
its capacity: its lookups queue or are dropped like any service's),
partial knowledge (every caller learns of the death at the same moment, `failover` after it), a
client sending a failed request to another server (that is the client's `retries`, which see the
lost share like any loss), and the link's delay for the dead share (whether a request goes to a
dead replica is decided when it is sent).

### `message_queue` and `worker_pool`

Book ch. 1 ("Message queue"), the transcoding queue of ch. 14 and the notification queue of
ch. 15, Kafka in practice (spec §19 entries 4-6). A `message_queue` is the **broker**: its own
queue is the append (spec §6: 1M messages/s; 100 µs chosen, so `c = 1M/s × 100 µs = 100` by
Little's law), and a publisher is answered **when its message is appended**, whatever happens to
it later. Every link out of a queue must go to a `worker_pool` that `consumes` it (a consumer
group), and every group receives **every** message (Kafka's consumer groups each read the whole
log; google-drive's two groups on `notify_q` each get all of it), as the same op. The hop from a
queue to a pool is **asynchronous**: the publisher's response and answered rate end at the append,
and what a pool loses counts in its own drop rate only. A `worker_pool` is **M/M/c over
consumption** (spec §7: "capacity = workers / job cost"): `c = workers`, service time `cost`,
rate `workers / cost` (youtube's 600 transcoders at `cost: 60s` drain 600 / 60 s = 10
videos/s). What a group has not drained waits in **its pool's queue**, whose waiting room is the
queue's `retention` × the pool's rate (a message older than `retention` is deleted unconsumed:
those are the pool's drops); each group's room is its own. Worked: 1k messages/s into 4 workers
at 3.333 ms: rho = 1000 / 1200 = 0.8333, Erlang C(4, 3.333 erlangs) = 0.6577, Wq = 0.6577 ×
3.333 ms / (4 × 0.1667) = 3.289 ms, sojourn 6.622 ms, while the publisher is answered in the
append's 100 µs plus its link's round trip. A 1.5k/s burst into 1.2k/s grows the backlog by
300 messages a second; with `retention: 10s` the room is 12,000 messages, full after 40 s, and the
pool then drops 300/s while the publisher is still answered 1.5k/s.

A pool's **`then` steps** are the work a finished job hands on (ch. 14's transcoder writes its
renditions to the transcoded store and publishes a completion message): every step fires for
every job the pool **drains**, `factor` requests of the step's op each, whatever op the queue
brought the job under, sized by the step's `size:` or, for a same-op step, by the job's own size.
A same-op step keeps only the size of the jobs that arrive under its op: when jobs of several ops
reach the pool, it renames the others, and a renamed request without `size:` has none, so a
byte-sized target refuses them with a note (`... has no size: add size: to the step or sizes: to
the client (a worker pool's then step keeps the size of the jobs that arrive as 'upload' only: it
renames every other job, and a renamed job has no size)`; the note appears only when the step's
op is one the pool's jobs arrive as: a step that renames every job gets the plain advice); give
such a step its own `size:`.
A pool of 10 jobs/s draining 8/s with `then: [{to: store, op: write, factor: 4, size: 50MB}, {to:
done_q, op: publish}]` sends the store 32 writes/s (1.6k MB/s) and `done_q` 8 messages/s;
offered 12/s it drains 10/s, so its targets get 10 jobs' worth (2k MB/s, 10/s), and the excess
waits in its backlog. A worker is busy for `cost` only: a slow target does not hold it, so the
pool's rho and sojourn stay M/M/c over `workers` and `cost`, and the target's own queue absorbs or
drops the load. The hops are ordinary calls from the pool's side, but **nobody waits for a pool**
(the hop into it from its queue is asynchronous), so they add nothing to any client's response and
fail no client's request, sequential or `mode: parallel` alike; what a target drops counts in its
own drop rate. `on: miss` (`node 'w': a worker pool's then steps fire for every job it drains
(on: miss needs a cache)`) and `on: failure` (`... (on: failure: a job's then steps follow no
step to report on; failure steps fire in an op's step list)`) are refused. A pool takes jobs only from the queue it consumes, so a `then` step may not
target another pool: it publishes to that pool's queue instead.

A pool's **`retries`** (phase 3; book ch. 10's notification workers: a send that APNs, FCM or an
SMS gateway answers with an error is put back on the queue with a retry count, and a message that
has used its retries goes to a **dead-letter queue** the on-call team watches) are how many more
times the pool calls an external sink for a job whose call failed: the job is re-published to the
pool's own queue `retry_after` later (rounded **up** to whole steps, at least one, as a client's
wait), up to `retries` times. The re-published jobs go through the queue like any message, so the
queue's demand and lag, the pool's demand and rho and the sink's demand all include them; but
only the pool that failed them reads them again: another consumer group of the queue handled
those jobs the first time (ch. 10 gives each channel its own queue, and a design that shares a
topic gives each consumer group its own retry topic), so its demand stays what was published (a
second group beside a pool that retries half its 1000 jobs/s three times stays at 1000/s while
the queue and the retrying pool see 1875/s). Only the jobs that **reached** a sink and failed
there are retried: with S the share of the jobs the `then` steps before a sink call let through
and d that call's delivered share (its `failure_rate` when it has room), the pool retries
**f = Σ S × (1 − d)** over its sink calls (a parallel group's sink calls once, `S × (1 − Π d)`), so
a job an earlier step lost (a saturated database before the provider) is that step's drop and is
not retried. With n retries a job that reaches the sink is attempted **1 + f + … + f^n** times (a
geometric series cut after n + 1 terms) and its last attempt fails with probability
**f^(n+1)**: that job is a **dead letter**, counted in the pool's `dead_lettered` total and
`dead_lettered_per_s` rate and **never as a drop**, since nothing overflowed and nobody lost it
for good (it is parked for an operator). Worked: 1000 jobs/s, f = 0.02, `retries: 3`: the sink is
called 1000 × 1.020408 = 1020.408 times a second, fails 20.408 of them (its own drops, as before:
its books balance), answers 999.99984, and the pool dead-letters 1000 × 0.02⁴ = 1.6e-4 jobs a
second (one every 1.7 hours); at f = 0.5 with 2 retries the sink is sent 1750/s, answers 875 and
125 a second are dead letters. Every job has one outcome: answered + lost before the sink +
dead-lettered = published (a pool writing to an 800/s database before a provider that fails 10 %
answers 720/s, loses about 280/s at the database and dead-letters the rest of its 1000/s). The
failed calls a pool retries are **not losses**: a sink's [drop rate](#drops-and-rejections)
counts only the drops of its callers that do not retry; the jobs that run out of retries are
judged by the [`dead_letter_rate` SLO](#slo) instead (and swept as `<pool>.dead_lettered`).
`explain` ends the pool's line with `retries=3 after 1s, then dead-letter`, and offers the pool
and its sinks A(f, n) = 1 + f + … + f^n times the jobs, f the sinks' `failure_rate`s combined
(1 − Π (1 − failure_rate)): iOS's 1140/s into 50 workers at 20 ms with `retries: 3` and a 2 % APNs
prints 46.5 % for the pool where it would print 45.6 %. Ignored: exponential backoff and jitter
(every wait is `retry_after`), a retry when the pool itself is overloaded (a job its room drops
is gone), idempotency (a retry replays the **whole** `then` list, so a step that had answered
before the sink failed, a log write or another provider, is called again for the job: a
duplicate push the model counts as load and does not tell apart, as a client's retried write),
and a sink that overflows while the pool retries: the pool's d is the sink's whole delivered
share, so it retries a call the sink shed for being busy as it retries an error (and its
retries load the quota further: `dead_letter_rate` is what shows it).

| key | on | rule | message |
|---|---|---|---|
| `retention` | `message_queue` | a positive [duration](#values) (`7d` = 168 h); default **7 days**, Kafka's `log.retention.hours = 168` (an industry default: the book gives none) | `node 'q': retention must be positive` |
| `partitions` | `message_queue` | a positive integer, printed by `explain` and **not modelled** (it would cap a group's parallelism: phase 3) | `node 'q': partitions must be a positive integer` |
| `consumes` | `worker_pool` | **required**: the queue (or URL `frontier`) the pool drains, its only source of jobs; *(graph)* it must be a `message_queue` or a `frontier` and link to the pool, and no other node may send to the pool (a client, a service's step, another pool's `then`: a pool is no server anyone calls) | `node 'w': worker_pool needs consumes`, `node 'w': consumes 'svc' (type service); a worker pool consumes a message_queue or a frontier`, `node 'w': consumes 'q', but no link from 'q' delivers its messages to it`, `node 'w': a worker pool takes jobs only from the queue it consumes ('svc' sends to it)` |
| `workers` | `worker_pool` | required, a positive integer, at most 100000 (Erlang C costs O(c) per step) | `node 'w': worker_pool needs workers`, `workers must be a positive integer`, `workers must be at most 100000` |
| `cost` | `worker_pool` | required, a positive duration: the time one worker spends on one job (a service's `cost` is a map of capacity units; same spelling, read by type) | `node 'w': worker_pool needs cost`, `cost must be positive` |
| `capacity`, `latency`, `replicas`, `advanced.concurrency` | `worker_pool` | refused: each would restate `workers` or `cost` | `node 'w': a worker pool's rate is workers / cost` |
| `advanced.queue_limit` | `worker_pool` | refused: the pool waits in its queue | `node 'w': a worker pool waits in its queue: set the queue's retention` |
| `retries` | `worker_pool` | optional (phase 3), a whole number from 0 to 10 (absent: no retries, phase 2's pool; `retries: 0` is the same pool: no `dead_lettered/s` column, and its sinks' failures are their drops); only on a pool with a `then` step to an `external_sink` (one among several suffices), whose failures it retries | `node 'w': retries must be a non-negative integer` (-1, 2.5, a word, a quoted `"2"`), `node 'w': retries must be at most 10`, `node 'w': retries need a then step to an external sink` |
| `retry_after` | `worker_pool` | optional (phase 3), a positive duration, only beside `retries`; absent: **1 s, chosen, not from the book** (long enough for a provider's blip to pass, short beside an outage's minutes, the first wait of the usual exponential backoff), filled in by the loader so `explain` prints it | `node 'w': retry_after must be positive`, `node 'w': retry_after needs retries` |

*(graph)* A link from a queue to anything but a pool that consumes it is refused: `node 'q': a
message queue delivers only to the worker pools that consume it ('svc' does not)`; so is a queue no
pool consumes (`node 'q': no worker pool consumes this message queue (its messages would be kept
for nobody)`) and a consumer link whose `ops` filter carries none of the ops the queue receives
(`link 'q'->'w' carries none of the ops 'q' receives (link ops: audit; it receives: publish)`); a
link's `ops` otherwise selects what its group reads, and a link of `weight: 0` is a group that reads
nothing. A `bandwidth` on a queue → pool link is a pipe like any other: once its 1000 MB room is
full it **drops** messages, where a real consumer pulls at its own pace and only falls behind (a
Kafka group lags, it loses nothing before `retention`); leave queue → pool links without one.
`explain` says what each number is: `retention=7d (default: Kafka's 168 h) partitions=32 (not
modelled)` on the queue (a retention of whole days in days, any other as other durations),
`capacity=10/s (workers / cost)  service_time=1m (cost)  c=600 (workers)  queue_limit=6.048M
(retention × capacity) ... consumes=transcode_q  workers=600 cost=1m` on the pool (a room of a
million or more with an SI prefix, as a count). A queue's
**lag**, how long a message published now waits until its slowest group reads it, is read off its
pools every step
(see [Queue lag](#queue-lag), the `queue_lag` SLO).
Ignored: ordering, delivery guarantees and redelivery, a consumer's commit interval, the broker's
disk; a worker blocked on a slow downstream call (an external sink's 150 ms would hold a worker:
phase 3), autoscaling, poison messages, the end-to-end completion time of the asynchronous work.

### `notification_fanout`

Book ch. 10, "Design a notification system" (phase 3 spec §8.3). The step between the
notification servers and the per-channel queues: it drops the duplicates (the book's dedup on the
event id) and publishes each remaining notification once to the queue of every channel it goes
out on, then records it in the notification log.

```yaml
fanout:
  type: notification_fanout
  dedup: 0.02
  channels: {ios: ios_q, sms: sms_q}
  then:
    - {to: ios_q, op: publish, factor: 0.6}
    - {to: sms_q, op: publish, factor: 0.05}
    - {to: log_db, op: write}
```

The fan-out is a **service**: `replicas` copies of the type's **20k/s at 1 ms** (chosen: a dedup
lookup and a few in-memory publishes, a service's 1 ms; c = 20 by Little's law) unless the
scenario states `capacity` and `latency`. It serves every notification it is routed, duplicates
included (finding one is work too), and its `then` steps fire for the share its dedup step lets
through: at 1k/s with `dedup: 0.02`, `ios_q` is sent 1000 × 0.98 × 0.6 = 588/s, `sms_q`
1000 × 0.98 × 0.05 = 49/s and `log_db` 980 writes/s (1960/s of replica requests at N = 2). The
2 % it drops, **20 a second, are deduplicated**: neither dropped nor rejected (the caller's request
succeeded; the duplicate was meant not to go out), so they stay out of `drop_rate` and
`reject_rate` and are reported on their own. The fan-out waits for its publishes as a service waits
for its steps (a queue answers at the append), each step taken by `1 − dedup` of the requests; a
step after a failed one is sent only what survived. `explain` follows the steps with the same
shares and prints `dedup=2% channels: ios=ios_q sms=sms_q` on the node's line.

| key | rule | refusals |
|---|---|---|
| `dedup` | optional, a number in [0, 1): the share dropped as duplicates (absent: 0, none). 1 would publish nothing, which a missing link says more plainly | `node 'fanout': dedup must be between 0 and 1` |
| `channels` | optional map of channel name → message queue: the student-facing list of the channels, while the `then` steps carry each one's share (`factor`). Each value must be a declared `message_queue`, and two channels may not name one queue | `node 'fanout': channel 'sms' names 'x', which is not declared`, `node 'fanout': channel 'sms' names 'api', which is a service (channels name message queues)` (the node's own type: `which is a client`, ...), `channels must be a map of channel names to message queues`, `channel 'b' names 'q', which channel 'a' names too` |
| `then` | [steps](#steps), each fired for `1 − dedup` of what the fan-out serves; `on: miss` and `on: failure` refused *(graph)*, a parallel group allowed (it publishes to its channels at once) | `node 'fanout': a fanout's then steps fire for every notification it routes (on: miss needs a cache)` |

*(graph)* `channels` and the `then` steps that publish to a `message_queue` name the same queues,
in both directions: `node 'fanout': channel 'email' names email_q, which no then step publishes to`,
`node 'fanout': then step publishes to email_q, which no channel names`. A step to anything else
(the notification log, an audit service) is no channel and is not checked; a fan-out with no
`channels` (absent or `{}`) is accepted as long as no step publishes to a queue. Retries and the
dead letters live on the channels' worker pools (`retries`, see
[`message_queue` and `worker_pool`](#message_queue-and-worker_pool)).

**What the reports add.** When the run has a fan-out, the node table has a `deduplicated/s`
column right after `rejected` (and after `dead_lettered/s`, beside `dropped`, when a pool
retries): the fan-out's mean rate, `-` on every other row; a run without one prints the table it
always did. JSON gives every node a `deduplicated_per_s` (the mean rate over the observed steps;
0 on a fan-out with `dedup: 0`, `null` on every other node), and `run --series` a
`deduplicated_per_s` column after `rejected_per_s`, empty on every row but a fan-out's:

```
t_s,node,demand_per_s,drain_per_s,rho,backlog,drops_per_s,dead_lettered_per_s,rejected_per_s,deduplicated_per_s,latency_ms,zone,queue_lag_s
```

A fan-out's `drain_per_s` includes its duplicates (it served them), so the series' balance holds
without the new column. Ignored: what makes two notifications duplicates and how long the ids are
kept (the book's dedup cache with a TTL: the share is stated, not derived from the traffic), the
lookup's own cost beyond the service time, per-user settings that turn a channel off (fold them
into the `factor`s), ordering.

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
(see [What runs](#what-runs-in-phase-2c)).

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

`{from, to, dir: in|out|inout (default out), ops: [..] (default all), latency, bandwidth, weight}`.

| key | status | rule |
|---|---|---|
| `from`, `to` | read | declared nodes, **different** from each other (a node does not call itself over the network) |
| `dir` | read | the direction **demand** travels. `out`: `from` sends to `to`. `in`: `to` sends to `from`; the link is written from the receiver's side. `inout`: like `out`, and replies flow back on the same link (the normal request/response case). The engine charges every hop a request leg and a reply leg (2 × one-way) whatever `dir` says |
| `ops` | read | a non-empty list of distinct op names; absent means all ops. `ops: []` is an error (it would load as "all", the opposite of what it says) |
| `latency` | read | **one way**. Default: half the round trip, `intra_dc_rtt / 2` = 250 µs inside a datacenter, `cross_dc_rtt / 2` = 75 ms across |
| `bandwidth` | read | a positive [byte rate](#values), in bytes or bits (`40Gbit/s` = 5e9 B/s): `bandwidth must be positive`, `bandwidth must be at most 1e15B/s`, `bandwidth is a byte rate (B/s, or bit/s for a network: 40Gbit/s)`. From phase 2c the link is a **pipe** that queues and drops megabytes (below). *(graph)* Every op the link carries needs a size: `link 'a'->'b' has a bandwidth, but op 'ping' from 'a' has no size: add size: to the step or sizes: to the client` (the sender named is the client or the renaming step that set or dropped the size). `explain` prints `bandwidth=1Gbit/s (125MB/s)` (the bit form with the largest prefix, G, M or k, of which bytes × 8 is a whole number: `1.5Gbit/s` prints `1500Mbit/s (187.5MB/s)`; the bytes alone when none is). A limiter's counter cannot cross a pipe (it has no size): `link 'limiter'->'store' has a bandwidth, but 'limiter' sends its counter over it: a limiter's counter carries no size, so its backend link cannot have a bandwidth; remove it` |
| `weight` | read | finite, ≥ 0, at most 1e9; default 1. When several links from a **client, load balancer or rate limiter** carry the same op, the op is split `weight_i / Σ weight` over them. 0 means a drained backend: the route carries nothing, so it brings no op to its target, closes no call cycle and imposes no replica model (a balancer's policy, below). *(graph)* An op whose every link has weight 0 is an error (`client 'u': every link carrying op 'read' has weight 0`; `node 'lb': ...` for a balancer). A service's steps and a cache's or limiter's backend name their target, so a weight there has no effect |

**One link per pair and demand direction.** A second link between the same two nodes in
the same demand direction is rejected ("duplicate link (put several ops or a weight on
one link)"); `{from: a, to: b}` and `{from: b, to: a, dir: in}` are the same direction.
Several ops go in one link's `ops`.

**A bandwidth is a pipe** (phase 2c; spec §3.4: it "caps bytes/s and queues the excess like a
node"). The link gets one queue in **megabytes**, like a [byte-sized node](#node-common-keys): one
server (`c = 1`: a link sends one byte stream at a time) draining `bandwidth / 1 MB` (1 Gbit/s =
125 MB/s), one megabyte taking `1 MB / bandwidth` (8 ms at 1 Gbit/s), each request holding it for
its whole serialisation, `size × 1 MB / bandwidth`, deterministic (the spread is the size mix's),
with a waiting room of **1000 MB** (chosen: the generic `queue_limit` read in megabytes; a link has
no `advanced` block) beyond which it drops. Fed what the sender puts on the link, before the delay
line: 1 MB requests at 100/s over 1 Gbit/s are rho 0.8, an M/D/1 wait of 0.5 × 0.8 / 0.2 × 8 ms =
16 ms. A hop over the pipe adds, once, beside the link's two fixed legs, the pipe's wait plus the
request's answer time on the wire, by the rule of a [byte-sized node](#node-common-keys): a
**write** pays its **whole serialisation**, since the receiver acts after its last byte, and **any
other op its first megabyte** (`min(size, 1 MB) × 1 MB / bandwidth`), since the receiver answers at
its first megabyte while the rest streams in. Whether an op is a write is decided per link and op
when the graph is built: it is one if a node its **body** reaches lists it in `write_ops` (`write`
for a node that lists none), the receiver or any node behind it. The body goes on under the op's
own name, and under the new name of a step whose `size:` equals the incoming request's (it
forwards the body itself, relabelled); a renamed step without a size, or of another size, is a
new request (a lookup, a notification, a 1 KB metadata write beside a 10 MB upload) and does not
count. So a
10 MB `upload` through a load balancer into a blob store that lists `upload` pays its whole 80 ms
on the client's link exactly as it would straight into the store, and so does one a service sends
on as `{to: store, op: put, size: 10MB}` to a store that writes `put` (a balancer or a service
passing a body on has it all before it forwards it). So: 16 + 8 = 24 ms there; for 10 MB requests
at 10/s, 160 + 8 = 168 ms for a read and 160 + 80 = 240 ms for a write. A streamed upload into a blob store thus pays the wire's
whole-object time and then the store's in series (an upper bound: a real PUT overlaps them).
Requests of several sizes wait longer than one size at the same rho (1 MB and 4 MB at 20/s each:
54.4 ms, Pollaczek–Khinchine). Like a node's, the wait is never longer than the room takes to
drain, 1000 MB / bandwidth (8 s at 1 Gbit/s), which understates the wait of requests larger than
the room itself (2 GB uploads at 0.05/s: Pollaczek–Khinchine 32 s, reported 8 s); they are not
refused, since 1.5 GB uploads are legitimate. Past the bandwidth the room fills (150 MB/s into
125: after 40 s), then the pipe drops the excess (25 MB/s), which fails the requests that cross it
as a node's drops do (the client is answered 5/6 of them). **Direction:** the pipe is fed by the
**demand** that crosses the link, each request charged its whole stated size in the direction it
is sent, whichever way its bytes really travel (a 50 MB `stream` loads the viewer's link to the
CDN although the video comes back), and a reply adds no bytes: an `inout` link is one pipe fed by
its requests, and `{from: x, to: y}` and `{from: y, to: x}` with a bandwidth each are two pipes,
each fed by its own direction's demand. Ignored: TCP slow start and windows, packet loss and
retransmission, protocol headers, propagation delay growing with load, and the true direction of
each byte (above). `run` adds a `links` table (the node table's columns but `peak_lag`, which
only a message queue has, in MB) with a row `a->b`
per pipe (the link as written, `from->to`, also for a `dir: in` link, whose demand and so whose
megabytes travel from `to` to `from`), JSON a `links` array (`name`, `demand_per_s`, `drain_per_s`, `dropped_per_s`,
`backlog`, `rho`, `latency_s`, `zone`, `peak_rho`, `peak_backlog`, `red_s`; `[]` without a
bandwidth), `--series` a row per pipe and step; `explain` checks it on paper: `a->b  peak offered
100 MB/s vs 125 MB/s  (80%, yellow)`, with the node lines' marks when its sender's load is
incomplete (`load not propagated: ...`, no percentage when none reached it) or rests on an unknown
hit ratio (`(hit ratio unknown: counted as 0 — an upper bound)`).

**Delay lines** *(graph)*: a link delays its flow by `round(one_way / step)` steps, halves
rounded away from zero (this replaces the spec's original `ceil` rule, see spec §16).
At the default 100 ms step a 250 µs hop passes in the same step, a 75 ms cross-DC hop
arrives one step late. A link that closes a loop of nodes is delayed at least one step.
A link longer than **1,000,000 steps** is an error: use a coarser `step`.

## SLO

`{metric, node, op, max, over}`; `metric` and `max` are required.

| metric | `node` | `op` | `max` | evaluates |
|---|---|---|---|---|
| `p50_latency`, `p95_latency`, `p99_latency` | required, a **client** | optional, one the client sends | duration | the client's estimated end-to-end percentile, fitted to the requests the path **delivered** (a refused or dropped request saw no latency); without `op`, the worst of its ops. Behind a client `timeout` the fit is to every delivered request before the timeout cut it, read at q and capped at the timeout; a percentile at quantile q of whose op more than 1 − q of the delivered requests timed out (more than 1 % for a p99) fails whatever its `max`, observing the timeout, with a note: `57.3% of delivered requests timed out (more than 1%)`, or, for an SLO without `op`, naming the op with the largest timed-out share, `57.3% of delivered upload requests timed out (more than 1%)` (see [Drops and rejections](#drops-and-rejections) and [How percentiles are estimated](#how-percentiles-are-estimated-and-where-they-mislead)) |
| `drop_rate` | optional | not allowed | number in [0, 1] | the node's dropped / the work sent to it, after warmup (a sink's failed calls that worker pools retry are not counted, caller by caller: [Drops and rejections](#drops-and-rejections)); without `node`, the worst node's or bandwidth link pipe's (a `node` names a node only) |
| `dead_letter_rate` | required, a **`worker_pool`** | not allowed | number in [0, 1] | phase 3: the pool's [dead letters](#message_queue-and-worker_pool) / the jobs published to its queue (the queue's demand less the re-published retries), after warmup: the share of the work given up on once its retries ran out, which the drop rate does not count. 1.6e-7 for 1000/s into a sink failing 2 % with `retries: 3` (0.02⁴); 0.118 when the sink's capacity is 900/s for 1000/s offered with `retries: 10` (it answers at most 0.98 × 900 = 882/s, so 118/s run out of retries), where `drop_rate` reads 0. Refused on any other node (`slo: dead_letter_rate needs a worker_pool node ('q' is a message_queue)`) and without one (`slo: dead_letter_rate needs a node`) |
| `reject_rate` | optional | not allowed | number in [0, 1] | **new in v1.2**: the node's rejected (a rate limiter's refusals, a leaky bucket's overflow included) / the requests sent to it, after warmup; without `node`, the worst node's |
| `utilization` | required, not a client | not allowed | number in [0, 1] | the node's mean busy fraction (drain / capacity) after warmup, of the copy the node reports: behind a `consistent_hash` balancer the **hottest copy** (`k = 1 + z_M/√vnodes` × the average share, z_M the expected maximum of M standard normals, see [`load_balancer`](#load_balancer)), on a `kv_cluster` its hottest machine (`k = min(1 + z_M/√(N·vnodes), nodes/N)`, see [`kv_cluster`](#kv_cluster)), on a `sql_db` the busier kind of copy (primary or replica); a byte-sized node's in megabytes |
| `queue_lag` | optional; a `message_queue` | not allowed | duration | phase 2c: the named message queue's **peak** lag after warmup, its slowest consumer group's wait (see [Queue lag](#queue-lag)); without `node`, the worst queue's; `skipped` when no message queue ran. A `frontier` is accepted (the web crawler's queue) and skipped until it runs (phase 3); any other node is refused (`slo: queue_lag needs a message_queue node ('svc' is a service)`) |
| `staleness` | optional; name a `sql_db` | not allowed | duration | phase 2b: the named database's mean staleness after warmup, its write sojourn at the primary + `replication_lag` + its sojourn at a replica (see [`sql_db`](#sql_db)); fails whatever that mean when a replica misses committed writes (it falls behind without bound; the note says how many per replica, of how many committed); `skipped` without a `node` or on a node that keeps no replicated data (any type but `sql_db`); a `sql_db` with `replicas: 1` reports 0 |

The kind of `max` follows the metric, so `0.01` can never be read as 0.01 seconds
(`slo: reject_rate max must be between 0 and 1`).
`op` is only valid for latency metrics (`slo: op is only valid for latency metrics`): drop
rate, reject rate and utilization are measured per node, not per op.

**`over`** *(phase 2c)*: a duration; the SLO is checked on every window of that length instead of
the whole run, and the **worst** window decides (a fail before a pass, then the highest observed
value). The windows are aligned from the end of warmup and follow one another (`warmup: 2m`,
`over: 1h`: 00:02-01:02, 01:02-02:02, ...); a step belongs to the window its start falls in; a
last window shorter than `over` is still checked. Every window is judged: a node still draining a
queue after its clients stopped is busy, and its utilization, drops or lag count. Only a latency
SLO skips a window in which its client op was not offered (no request, no latency), and one all of
whose windows are skipped is `skipped`. Each window is measured exactly as a whole run is (its
percentiles read and capped, or set to, a client's timeout by the same rule). The table names the
window that decided, `(worst 10m from 00:20)` (its start on the run's clock, warmup included:
`HH:MM`, or `HH:MM:SS` when it is not on a whole minute, `00:19:59`; past the first day the exact
duration from the start); a last window cut short by the end of the run is named with the length
it had (url-shortener, `run --duration 870m`: `(worst 28m from 14:02, the run's short last
window)`). JSON gives the start as
`window_start_s` (seconds from the start of the run; `null` when skipped), a key only an SLO with
`over` has. `estimate` measures no window (one steady state at t = 0): it checks an SLO with
`over` on that state and says so, `(at 00:00 only: run checks each 1h)`, with no `window_start_s`.
`over` must be a whole number of steps, at least one, and at most duration − warmup: `slo: over
must be at least one step (50ms < step 100ms)`, `slo: over must be a whole number of steps
(1500ms, step 1s)` (windows of two steps and of one in turn otherwise), `slo: over must fit in the
run after warmup (2h > duration 1h - warmup 0s)`. `run --duration` is held to the same rule with
the duration it gives (youtube, `--duration 10m`: `error: --duration 10m: over must fit in the run
after warmup (1h > duration 10m - warmup 5m)`, exit 1). Fixed windows split a burst that straddles
two of them, and a sliding window can find a much worse one: an 8-minute spike to 0.9 from 00:20
on a load of 0.5, checked `over: 10m` after a 5-minute warmup, is cut into 0.7 and 0.62 by the
grid (00:15-00:25, 00:25-00:35), where the sliding window 00:20-00:30 holds all of it, 0.82.

### Queue lag

A message queue's **lag** (phase 2c) is how long a message published at that step waits until
its slowest consumer group reads it: Kafka's consumer lag in records divided by the group's
consume rate. The broker holds nothing back (it appends at 1M/s); what a group has not read
waits in its worker pool's queue, so every step the lag is read off the pools:

```
lag(group) = max(min(k × Wq, retention), backlog / capacity)      lag(queue) = its worst group's
```

`backlog / capacity` is the FIFO drain time: a message published now is read after everything
ahead of it, which the pool works off at its capacity (`workers / cost`) while it is behind.
`k × Wq` is the stochastic part: the pool's Erlang C (M/M/c) wait scaled by Allen–Cunneen's `k`,
at rho clamped to 0.99, what a message waits with no backlog because every worker happens to be
busy. Neither passes the retention: an older message is deleted unconsumed. It is the wait alone,
the job's `cost` is not lag (a message is read once a worker takes it). Worked: 1k msgs/s into 4
workers at 3.333 ms lag 3.289 ms (no backlog); a 1.5k/s burst for 200 s into 12 workers at 10 ms
(1.2k/s) leaves 60,000 messages waiting and the queue **50 s** behind at t = 200 s (60,000 /
1200), 33.3 s at 300 s as the backlog drains; a pool saturated for the whole run climbs to its
retention and stays there. Two groups on one queue: the slower one's lag. Taking the larger of
the two terms is this simulator's heuristic (the fluid queue's wait, used for every node), not a
published result. The lag is not the age of the oldest unread message, which some dashboards show:
under FIFO the message published now is the oldest one lag later, so the age trails the lag by one
lag and reaches the same peak that much later. The `queue_lag` SLO bounds the peak lag after
warmup, so it reads "every message published after warmup was read within `max`": with `warmup:
220s` on the burst above it observes 46.6 s (55,980 / 1200: the first observed step ends at
220.1 s, when 60,000 − 200 × 20.1 messages wait), while the oldest unread message is 44 s old
then and 50 s old at 250 s, when the burst's
last message is read.

The `queue_lag` SLO observes the **peak** lag after warmup, not the mean or the last step's: a lag
bound is a promise about the worst moment (`PASS queue_lag q - 50s 1m`, `FAIL queue_lag q - 50s
30s`). The node table's `peak_lag` column shows each queue's (`-` on other rows), JSON's nodes
carry `peak_queue_lag_s` in seconds (`null` on other nodes), and `run --series` ends each row with
`queue_lag_s`, the step's lag in seconds (empty on every other row, clients and links included).
youtube's 600 transcoders (10 videos/s) never build a backlog: at the 21:00 peak (9.6 uploads/s,
rho 0.96) the lag is the Erlang C wait alone, `PASS queue_lag transcode_q - 573ms 30m`. With 400
(6.667/s, its README's sweep) the uploads exceed the pool from 15:43 and it falls 2.66 h behind
by midnight (63.76k videos at 6.667/s = 9,564 s), so the 30-minute SLO fails; a run that stops
while the backlog still grows understates the day's peak (about 3.0 h near 02:17 there,
`--duration 27h` shows it). Ignored: ordering across partitions, a consumer's
commit interval (a group's lag counts from the message's append, not from its last commit).

### Drops and rejections

Two different promises, kept apart on purpose:

- a **drop** is a node **failing**: its waiting room was full, so the excess is shed, or, at an
  `external_sink`, the provider answered `failure_rate` of what it served with an error.
  `drop_rate` = Σ dropped / Σ demand **of that node** (in capacity units when the node has
  a `cost` or counts megabytes), so it stays in [0, 1] even when fan-out multiplies the clients' requests
  before they reach it (one post → 100 feed writes would make "dropped anywhere / offered
  by clients" 90). Without `node` it is the worst node's, since every node must keep the
  bound, and a bandwidth link's pipe counts as one (megabytes dropped over megabytes sent; the
  header then names it `worst: a->b`). A failed call that a [worker pool
  retries](#message_queue-and-worker_pool) is no loss (phase 3): it is attempted again, or ends as
  the pool's dead letter, which is counted apart, never as a drop, and judged by the
  `dead_letter_rate` SLO. So an `external_sink` counts, step by step, its drops times the share of
  its demand that came from callers that do **not** retry (a sink fails a share of everything it
  serves, so each caller's part of its drops is its part of its demand): 0 when only pools with
  `retries` call it, and 0.02 × 10 / 1030.4 = 1.94e-4 when a client sends it 10/s beside a
  retrying pool's 1020.4/s, not the 0.02 of every failed call. Its `dropped` and the series'
  `drops_per_s` keep every failed call (its books still balance).
- a **rejection** is a rate limiter's **policy working** (ch. 4's 429): what a token bucket or a
  window algorithm refuses, and a leaky bucket's overflow. `reject_rate` = Σ rejected / Σ
  demand of that node, with the same rules.

Per node and per step the numbers balance, `demand·dt = (drain + dropped)·dt + Δbacklog`:
whatever reaches a node is served, shed or left waiting. A rejection is not a term of its
own: a token bucket's or a window algorithm's rejections are inside `drain` (the limiter's
queue checked them before refusing them), and a leaky bucket's overflow is the `dropped`
term, reported as `rejected` instead. `run --series` writes demand and drain (phase 2b), so the
identity can be checked from the file: summed over a run that starts empty,
Σ demand·dt = Σ (drain + drops)·dt + the backlog left at the end, and at a leaky bucket,
whose overflow the file writes in `rejected_per_s` rather than `drops_per_s`,
Σ demand·dt = Σ (drain + drops + rejected_per_s)·dt + the backlog left at the end.

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
for other nodes, and (phase 2c) `peak_queue_lag_s`, a message queue's peak lag in seconds
([Queue lag](#queue-lag)), `null` for other nodes, and (phase 3) `dead_lettered` and
`dead_lettered_per_s`, a worker pool's [dead letters](#message_queue-and-worker_pool), `null` for
other nodes, and (phase 3) `deduplicated_per_s`, a notification fan-out's
[duplicates](#notification_fanout) per second, `null` for other nodes. When some pool has
`retries`, the node table has a `dead_lettered/s` column right after `dropped` (the pool's mean
rate, `-` on every other row), and when the run has a fan-out a `deduplicated/s` column right after
`rejected`; without them the table is the one it always was. Likewise a scenario with a
[`ws_gateway`](#ws_gateway) gets a `connections` column after `peak_lag` and the JSON keys
`connections` and `connections_peak`; a gateway's refused connects are its `rejected`. Each SLO object has a `note`: `null`, or why the verdict is not `observed`
against `max` alone (a replica that falls behind without bound).

**`run --report json` (and `estimate --report json`), key by key.** Every figure is over the
observed steps (after warmup). A node's `dropped` and `rejected` are **totals** over those steps,
a link's `dropped_per_s` a **rate**: compare a link with a node by multiplying the link's rate by
the observed seconds (`steps` × `step`).

| object | key | unit |
|---|---|---|
| top level | `scenario`, `steps` | the name; the number of observed steps |
| top level | `drop_rate`, `reject_rate` | ratios in [0, 1], the worst node's (a pipe's counts for drops); `null` when no step was observed |
| top level | `worst_drop_node`, `worst_reject_node` | a node id or `from->to`; `null` when nothing was dropped or refused |
| `nodes[]` | `id`, `mean_rho`, `peak_rho` | the node; time-averaged and highest busy fraction of one copy |
| `nodes[]` | `peak_backlog` | the most work waiting at the end of any step: requests, cost units at a node with a `cost`, MB at a byte-sized node |
| `nodes[]` | `dropped`, `rejected` | **totals** over the observed steps, in the same units as `peak_backlog` (`rejected` in requests) |
| `nodes[]` | `dead_lettered`, `dead_lettered_per_s` | jobs: the **total** over the observed steps and its mean rate per second (phase 3: a `worker_pool`'s jobs whose last retry failed; 0 on a pool without `retries`, `null` on every other node) |
| `nodes[]` | `deduplicated_per_s` | notifications per second, the mean over the observed steps (phase 3: a `notification_fanout`'s duplicates, drain × `dedup`; 0 with `dedup: 0`, `null` on every other node) |
| `nodes[]` | `red_s`, `unavailable` | seconds spent red; `true` when the node's latency was infinite in some step |
| `nodes[]` | `staleness_s`, `replication_dropped_per_s`, `committed_writes_per_s` | seconds; writes per second per replica; writes per second per shard (a `sql_db` only, else `null`) |
| `nodes[]` | `peak_queue_lag_s` | seconds (a `message_queue` only, else `null`) |
| `nodes[]` | `connections`, `connections_peak` | open connections (a level, not a rate): at the end of the run, and the highest of any observed step (phase 3: a `ws_gateway` only, else `null`; both keys only when the scenario has a gateway) |
| `links[]` | `name` | `from->to` as written |
| `links[]` | `demand_per_s`, `drain_per_s`, `dropped_per_s` | MB per second, **time-averaged rates** |
| `links[]` | `backlog`, `peak_backlog` | MB, time-averaged and highest |
| `links[]` | `rho`, `peak_rho`, `latency_s`, `zone`, `red_s` | busy fraction, mean and highest; seconds (a request's wait plus its time on the wire); the worst zone of any step; seconds red |
| `ops[]` | `client`, `op`, `offered_per_s`, `attempts_per_s`, `answered_per_s`, `timed_out_per_s`, `timed_out_attempts_per_s` | requests per second, time-averaged; `attempts_per_s` (phase 3) is what the client sent, the offered rate plus its [`retries`](#node-types), equal to `offered_per_s` without them; `timed_out_per_s` counts the requests given up on for good (their last attempt timed out), `timed_out_attempts_per_s` (phase 3) every attempt that timed out, the share a percentile reads (equal to `timed_out_per_s` without retries) |
| `ops[]` | `mean_s`, `p50_s`, `p95_s`, `p99_s` | seconds; `null` when nothing was answered (in time) or the path never answers |
| `slo[]` | `metric`, `node`, `op`, `status`, `note` | as written (`null` when absent); `pass`, `fail` or `skipped`; `null` or the reason |
| `slo[]` | `observed`, `max` | seconds for a latency, `staleness` or `queue_lag` SLO, a ratio otherwise |
| `slo[]` | `window_start_s` | seconds from the start of the run; only an SLO with `over` that `run` judged |
| `faults[]` | `at_s`, `kind`, `node`, `for_s` | seconds from the start of the run; the kind; the node (`null` for a `dc_down`, which has `datacenter` after it); seconds, `null` for a `cache_flush` |
| `faults[]` | `overlaps_at_s` | the `at_s` of every earlier fault on the same node (or datacenter) still counting when this one struck; `[]` for none |
| `faults[]` | `during` | `offered_per_s`, `served_per_s` (means over the fault), `lost` (against the twin, from `at` to the recovery) in `lost_unit` (`"requests"` or `"MB"`), `lost_on_cut_links` (a `dc_down`'s, in requests; `null` for every other kind), `backlog_peak`, `room` (in `lost_unit`; `room` `null` for a `cache_flush`), `extra_misses` and `origin` (a `cache_flush`'s, in the origin's unit; `null` for every other kind), `flipped`, the SLOs whose verdict differs from the twin's, as the text names them (`["p99_latency c op"]`; the verdict is the `slo` array's), and `over_max`, each SLO broken during the fault where the twin held it, `[{"slo": "p99_latency c op", "for_s": 0.3}]` |
| `faults[]` | `after` | `backlog_node` (whose backlog: the node, the datacenter, a `traffic_spike`'s downstream node; `null` when the fault left none), `backlog_drained_in_s`, `cache_warm_in_s`: seconds, `null` where the text says there is none; a `cache_flush`'s also has `interrupted_by_flush_at_s` after `cache_warm_in_s` (seconds from the start of the run of the next flush that emptied its cache first; `null` when none did), a key no other kind has |
| `faults[]` | `recovered_at_s` | seconds from the start of the run; `null` when not recovered by the end of the run |

`faults` is the last key, after `slo`, and only a run with faults has it (`estimate`, which leaves
the faults out, never does): one object per fault in `at` order with the numbers of its text block
(see [Faults](#faults)), unrounded and in seconds, so that a tool can read the recovery time a
README quotes (`"recovered_at_s": 20.2` is the text's `00:00:20.2`). Every figure is measured
against the fault-free twin run (below).

At a node that counts megabytes (a byte `capacity`, see [Node (common keys)](#node-common-keys))
the node's own numbers are in MB: the table's `dropped`, JSON's `dropped`, and the series'
`demand_per_s`, `drain_per_s`, `drops_per_s` (MB/s) and `backlog` (MB), as its capacity and
`queue_limit` are; what the clients offered and got answered (`offered_per_s`,
`answered_per_s`) stays requests. Its `latency_ms` is its requests' own sojourns mixed by their
rates, each answered at its last byte (a write) or its first megabyte (a read).

`run --series` writes one CSV row per node per step:
`t_s,node,demand_per_s,drain_per_s,rho,backlog,drops_per_s,dead_lettered_per_s,rejected_per_s,deduplicated_per_s,latency_ms,zone,queue_lag_s,connections`
(`rejected_per_s` new in v1.2, `demand_per_s` and `drain_per_s` in phase 2b, `queue_lag_s` in
phase 2c: a message queue's lag in seconds, empty on every other row; `dead_lettered_per_s` in
phase 3: a worker pool's dead letters per second, 0 without `retries`, empty on every other row and
outside the balance below, since the pool drained those jobs; `deduplicated_per_s` in phase 3: a
[notification fan-out's](#notification_fanout) duplicates per second, 0 with `dedup: 0`, empty on
every other row and inside `drain_per_s`, since the fan-out served them; demand, drain, drops and
backlog in capacity units, megabytes at a byte-sized node; a sink's `drops_per_s` keeps the
failures a pool retries; `connections` in phase 3: a [`ws_gateway`](#ws_gateway)'s level of open
connections at the start of the step, 6 significant digits, empty on every other row; a client row
has zeros, zone `none`, and no dead letters, no duplicates, no lag and no connections), then one
row per [bandwidth link](#links) and step, its `from->to` in the `node`
column, in megabytes, `rejected_per_s` 0, `deduplicated_per_s` empty and `latency_ms` a request's wait plus its answer time
on the wire (a write's whole serialisation, any other op's first megabyte; with no request in the
step, the ops the link carries weighed alike).
`run --series-ops` writes one CSV row per client op per step (phase 2b):
`t_s,client,op,offered_per_s,attempts_per_s,answered_per_s,timed_out_per_s,mean_ms`, where
`attempts_per_s` (phase 3) is what the client sent, the offered rate plus the retries due that
step (equal to `offered_per_s` for a client without [`retries`](#node-types)), and
`answered_per_s` is the part that came back answered (the attempts × the fraction the path
delivered: offered minus what a limiter refused or a queue dropped on the way, never above
offered without retries, while a wave of retries answered as an outage ends can lift it above
offered for a step; requests
still waiting in a queue count as answered, later; requests slower than the client's `timeout`
do not: they are `timed_out_per_s`, 0 without a timeout, a column added in phase 2c) and
`mean_ms` the mean response of an answered request (`inf` when the path never answers). Numbers have 6 significant digits, `t_s` its shortest exact form. JSON's op entries
carry `answered_per_s` beside `offered_per_s`, and `timed_out_per_s` (phase 2c), and
`attempts_per_s` right after `offered_per_s` (phase 3).

A **timeout** is a third kind of unanswered request, the client's own: a client with a `timeout`
gives up on the requests its path delivered later than that (the share of the step's fitted
lognormal response above the timeout). JSON's `timed_out_per_s` counts them per client op (0
without a `timeout`), so `answered_per_s` = `offered_per_s` − what was dropped or refused on the
way − `timed_out_per_s`. A timed-out request is **not** in any `drop_rate`: no node failed it
(the servers still did its work, and their demand and utilization include it), so `drop_rate`
stays a property of the nodes. The op's mean describes the requests answered in time. A
percentile at quantile q is what the client saw there: when more than 1 − q of the delivered
requests timed out (more than 1 % for the p99, 5 % for the p95, half for the p50), no request at q
was answered, so the percentile is the timeout, printed `>1s` in `run`'s table, and a latency SLO
on it **fails** whatever its `max` (it observes the timeout; the note says how many timed out:
`57.3% of delivered requests timed out (more than 1%)`, and which op when the SLO names none,
`57.3% of delivered upload requests timed out (more than 1%)`): the client cannot vouch for a
quantile it never saw answered. Otherwise, with a share s ≤ 1 − q timed out, the percentile is read off the
fit to **every delivered request's** latency, the moments each step had before the timeout cut
them, at q itself, and capped at the timeout. The engine times out exactly the requests that fit
puts past the timeout, so the fastest q of the delivered requests are all answered ones and the
client's q-quantile is that fit's: the timeout does not move it. An M/M/1 path of p99 490.5 ms
behind a 530 ms timeout (0.78 % timed out) shows a p99 of 490.5 ms, exactly as without the
timeout, and fails a 420 ms SLO. So a percentile never falls as the timeout grows, meets the
timeout from below as s rises to 1 − q, and is what the client without a timeout sees once the
timeout cuts nothing. (An earlier rule refitted a lognormal to the
answered requests' truncated moments and read it at q / (1 − s); that refit has a tail past the
cut the truncated distribution lacks, so its percentiles were neither exact nor monotonic in the
timeout: 508.9 ms behind 600 ms, 475.5 ms behind 1 s, on the same path.) The cap only matters for a
run that mixes steps: a single steady step has s ≤ 1 − q exactly when the fit's q-quantile is at
or below the timeout. With [`retries`](#node-types) the share s counts **attempts**: of the
attempts the path delivered, the ones that came back after the timeout (JSON's
`timed_out_attempts_per_s`), since the client waited past its timeout on each of them, whether or
not a later attempt was answered; `timed_out_per_s` keeps the requests given up on for good, the
ones whose last attempt timed out. The note still says `requests`: an attempt is a request on the
wire.
`run`'s op table has an `attempts/s` column right after `offered/s` (phase 3: what the clients
sent, their [`retries`](#node-types) included, so a retry storm is the row where it rises above
`offered/s`), and a `timed out/s` column after it when some client has a timeout; JSON prints `null`
for the latencies of an op of which nothing was answered in time (every delivered request timed
out).
When no step was observed every SLO is `skipped` and `run` exits 1: no data is an error,
not a pass.

### How percentiles are estimated, and where they mislead

The engine follows rates, not requests, so it never sees one request's latency. Each step it
carries two numbers for every client op: the **mean** and the **variance** of the response, built
hop by hop (means add along a path, variances add for independent hops, a cache's hit or miss and
a balancer's split mix them by share; [numerics §5](study/numerics.md)). The **mean is the
model's own number**: every queue's wait formula (Erlang C with Kingman's correction, the fluid
drain, the room-drain bound) is a formula for a mean, and the op's mean is their sum along its
path, as good as the queueing model under it. A **percentile is one step further**: the report
fits a lognormal with that mean m and variance v (σ² = ln(1 + v/m²), μ = ln m − σ²/2) and reads it
at q, `exp(μ + σ z_q)`; the op table says so in its heading, `(percentiles estimated: lognormal
fit to the response moments)`, and in its `(estimated)` column. Two moments cannot carry a shape.
The fit is good when the true latency is one right-skewed hump, which is what a path through a
few busy queues looks like, and it misleads in two cases, both measured with `simsys run`:

- **Two populations.** An op that takes one of two very different times has two peaks, and a
  lognormal smears them into one long tail. youtube's European streams are a hit at the edge,
  answered in 10.5 ms (59.4 % of them), or a miss that crosses the Atlantic, waits for the pipe
  and reads its first megabyte at the store, about 207 ms (40.6 %). No stream takes longer than
  about 207 ms, yet the fitted p99 is **465 ms**; the mean, 89.3 ms, is right
  ([youtube's README](../scenarios/book/youtube/README.md), "The day's run"). A cache's
  hit/miss mix does the same at a smaller scale (numerics §5: a p95 of 22.6 ms where every miss
  takes 51 ms).
- **Hard bounds.** A full waiting room bounds the wait: nobody admitted waits longer than the room
  takes to drain, `queue_limit / capacity` (the room-drain bound of
  [`advanced.queue_limit`](#advanced)). The model applies it to the mean and caps the wait's
  variance at the room's scale, `(queue_limit / capacity)² / 4` (our heuristic), but a lognormal
  has no upper end, so its percentiles pass the bound:

  | probe (links of `latency: 0s`, a 1-minute run) | mean | p99 (estimated) | slowest real request |
  |---|---|---|---|
  | 12 writes/s of 50 MB into a default `blob_store` (600 MB/s against 500: the room stays full) | 4 s | **6.88 s** | 4 s: the room's 2 s and the write's own 2 s, both deterministic |
  | 1.2k/s through a `leaky_bucket` of `limit: 1k/s`, `burst: 20`, to a service of 10k/s and 200 µs | 21.2 ms | **54.4 ms** | about 21.2 ms: 20 ms in the full bucket and its 1 ms leak, both deterministic, and the service's 0.2 ms on average |
  | 1.2k/s into a service of 1k/s, 1 ms, `queue_limit: 10` | 11 ms | **27.9 ms** | no hard bound (the service time is exponential), but the true p99 is about 20 ms: a request behind the full room waits for ten exponential services and then takes its own, an Erlang distribution of 11 phases, whose p99 is 20.1 ms |

  A finite slowest request exists only where every hop is deterministic (a fixed-size transfer, a
  leak); anywhere a service time or an arrival process is random there is a scale but no hard
  maximum, and clipping the fit at a bound would be a second heuristic stacked on the first. So
  the reports **label** the percentiles instead of truncating them (the theory test `a saturated
  blob store's percentiles are labelled, not truncated: its estimated p99 is 6.88 s past a 4 s
  bound` pins the first row).

How to read them: size off the mean and the utilization; read a percentile beside the mean; and
where a p99 sits far above the slowest path you can add up by hand (youtube's does), treat it as
the fit's tail, not as a request anybody waited for.

**Behind a client `timeout`** a percentile follows the rule of [Drops and
rejections](#drops-and-rejections): it is read off the fit to every request the path
**delivered**, the moments each step had before the timeout cut them (the fit that decided which
requests timed out, so its fastest q are all answered), at q, and capped at the timeout; when more
than 1 − q of the delivered requests timed out (more than 1 % for a p99) no request at q was
answered, the percentile is the timeout itself, printed `>τ` in the table (`>1s`, `>50ms`), and a
latency SLO on it fails. The mean stays the answered requests' mean (the truncated lognormal's).

## Faults

A list of scripted failures (spec §4.1), each typed into a `model::Fault` by `src/config/faults.cpp`
and listed by `explain` (`faults: 1 scripted (node_down chat_servers 21:30 for 1m)`; no line
without faults). `run` applies them (phase 3 spec §4.2, `engine::FaultSchedule`): for the steps a
fault covers, from the step that starts at `at` to the last one that starts before `at + for`, it
changes the numbers the node's queue is built from, and nothing else:

| kind | while it lasts |
|---|---|
| `node_down` | the node's capacity × (R − r) / R, r of its R replicas down (a node some op reaches through a service discovery's lookup also loses r / R of that op's requests for the discovery's `failover`: see [`service_discovery`](#service_discovery)); every replica down: capacity 0, so nothing drains, the room fills to `queue_limit` and the rest is dropped, and the backlog drains when the fault ends. A request waiting in that room waits for the node to come back, then for the backlog ahead of it at the node's own capacity: `(at + for − now) + backlog / capacity` (10 s left and 40 requests at 1k/s: 10.04 s), so a client `timeout` counts it timed out and a step that delivered nothing weighs no latency (a node whose `capacity` is 0 by configuration never comes back: its ops stay `unavailable`). On a `sql_db` the two kinds of copy apart: `primary` stops the primaries (the writes wait and are dropped, the reads go on at the replicas), `replicas: r` leaves its N − 1 read replicas (N − 1 − r) / (N − 1) of their capacity and the primary whole. On a `kv_cluster` r of its machines are down: the survivors keep their own load, the coordinator sends nothing to a down machine, and a request whose key had j of its N replicas there is answered by its quorum among the N − j live ones, or lost when N − j is below W (writes) or R (reads) |
| `capacity` | capacity × `factor`; a request still takes its service time. Every queue of the node is derated, a leaky-bucket limiter's leak (`limit`) included: `limit: 50/s` at `factor: 0.5` passes 25/s |
| `latency_spike` | service time + `extra`, so the node's servers drain c / (s + extra) a second. On a node sized by its defaults the servers are few and short: a load balancer's c = 5 at 50 µs (100k/s) drains 5 / 1.05 ms = 4.76k/s under a 1 ms spike, 95 % of its capacity gone |
| `traffic_spike` | the client's rate × `factor`, exactly as a `spikes` entry on its rate curve |
| `cache_flush` | an instant, not a window: the cache's hit ratio is 0 at the step that starts at `at` and warms back (below) |
| `dc_down` | a `node_down` of every node whose `dc` is the datacenter (clients excepted: they keep sending), and capacity 0 on every link with one end inside it and the other outside, either way. A link with a `bandwidth` is a queue of megabytes: it drains nothing, fills its room (1000 MB) and drops the rest, so nothing crosses it once the room is full. A plain link has no queue: it drops at once what it is sent, and every request over it is lost (it has no row in the link table to count them in). A link with both ends inside is not cut. Requests already in flight on a link when the cut begins still arrive. Its caches come back warm at `at + for` (the outage takes their capacity, not their contents); script a `cache_flush` at `at + for` for a cold restart |

**The warm-up after a `cache_flush`** (phase 3 spec §4.3; `engine::FaultSchedule`;
[the derivation](study/numerics.md#11-a-flushed-caches-warm-up)). A flush empties the cache at
`at`; it then refills from the reads it is actually sent: after every step the engine adds that
step's reads (its arrivals, write ops left out) to n, the reads one copy has seen since the flush
(each of its replicas sees its share), and the next step's hit ratio is read at that n. At a
constant rate n = rate × t; on a curve it is the curve's integral, so a cache flushed while nobody
reads it stays empty until the reads come (a client whose rate rises from 0/s at 600 s to 1k/s
at 601 s fills a cache flushed at 600 s by 612.7 s), and one flushed as the rate climbs fills faster as it climbs (100/s
rising to 2000/s over 100 s: full 30.9 s after the flush, not the 121 s of a rate held at 100/s).
A `che` cache then holds the D(n) distinct keys among them, D(n) = Σᵢ (1 − e^(−pᵢ n)) (the
coupon-collector sum over the clients' key popularity), and hits like a Che cache of
min(`keys`, D(n)) entries; it is full again, and back at its steady hit ratio exactly, once D(n)
reaches `keys`, at n = t_c, Che's characteristic time (1k reads/s over Zipf 0.8 of 1M keys into
10k keys: h 0.232 steady, 0.127 after 2 s, within 1 % at 11.8 s, full at 12.1 s). A fixed ratio is
a number, not a curve: the warm-up is the simple case, linear in the reads, h × min(1, n / `keys`),
full after `keys` reads. It is optimistic: it has every read add an entry, but once the cache hits
h · n / `keys` only its misses do, and the consistent model fills at n = −(`keys` / h) ln(1 − h),
2.56 × `keys` at h = 0.9 (the derivation above has it). While it is cold, a read_through cache or
a CDN sends its store the extra misses, and for a `cache_aside` cache the service's `on: miss`
steps do. A later flush of the same cache empties it again and starts its own warm-up: the earlier
flush's ends there, never warm (its block says `interrupted by the flush at 00:10:05`). Ignored:
that an LRU cache refills in recency order, the thundering herd on the store, and what writes put
back.

Faults on one node compose: factors multiply, extra latencies add, a kv_cluster's down machines
add. The survivors of a `node_down` share the node's load evenly (every replica's capacity is
scaled; a kv_cluster's survivors keep their own); the node comes back at full capacity the instant
the fault ends, and overlapping faults that each take all of it make one outage that ends with the
last of them. `estimate` is the steady state without the faults and says
so: `faults: 1 scripted (estimate shows the steady state without them; run applies them)`.

```yaml
faults:
  - {at: 21:30, kind: node_down, node: chat_servers, for: 1m}
  - {at: 15:00, kind: node_down, node: post_db, replicas: 1, for: 30s}
  - {at: 12:00, kind: latency_spike, node: user_db, extra: 50ms, for: 5m}
  - {at: 03:00, kind: capacity, node: cdn_us, factor: 0.5, for: 20m}
  - {at: 09:00, kind: cache_flush, node: feed_cache}
  - {at: 18:00, kind: traffic_spike, node: users, factor: 3, for: 10m}
  - {at: 02:00, kind: dc_down, datacenter: eu, for: 2m}
```

| key | required | meaning |
|---|---|---|
| `at` | yes | a clock time (`21:30`) or a duration from the run's start (`90m`), the rate curve's syntax; must fall inside the run (`faults[0]: at must fall inside the run (24h)`); missing: `faults[0]: needs an at`. A clock time names the first day only (t = 0 is midnight): a 48 h run reaches its second day with a duration (`30h` is 06:00 on day two) |
| `kind` | yes | `node_down`, `capacity`, `latency_spike`, `cache_flush`, `traffic_spike` or `dc_down`; `partition` is refused (`partition is not modelled in phase 3 (see the backlog)`); a list or map is `kind must be a scalar` |
| `for` | all kinds but `cache_flush` | a positive duration; the fault ends at `at + for`, and may run past the end of the run (it is cut there: the run ends with the fault still on). A flush is an instant: `cache_flush takes no for` |
| `node` | all kinds but `dc_down` | a declared node; `traffic_spike` needs a client, `cache_flush` a cache or cdn, the others any node but a client (`node_down cannot name a client (clients emit, they do not serve)`) |
| `replicas` | `node_down` only, optional | how many of the node's copies go down (default: all), 1 up to its `replicas` (beyond: `replicas must be at most 4`); for a kv_cluster, of its `nodes`. On a sql_db a count of copies per shard (each shard is a primary and `replicas - 1` read replicas: news-feed's `post_db` has `shards: 6`): `replicas: r` below `replicas` takes r of the read replicas and leaves the primary (all of them: the whole node), and `primary` takes the writer, whose writes stop while the reads go on (the engine tells the two apart; the primary comes back at `at + for`, no replica is promoted). On a sql_db of one copy `primary` and `replicas: 1` are both the whole node, like a `node_down` without `replicas` |
| `factor` | `capacity`, `traffic_spike` | a number (`three`, `"3"`, `2k`, `50%`: `factor must be a number`); `capacity`: strictly between 0 and 1; `traffic_spike`: positive, at most 1e6 |
| `extra` | `latency_spike` | a positive duration added to the node's service time |
| `datacenter` | `dc_down` | a declared datacenter |

A key the entry's kind does not read is refused (`replicas applies to node_down only`), and so is
a misspelt one (`faults[0]: unknown key 'nod'`). Every message names the entry by its position in
the list.

Faults that overlap in time on one target combine: their `factor`s multiply (two capacity faults of
0.5 leave a quarter; two traffic spikes of 3 send nine times the rate) and their `extra`s add. The
traffic spikes of one client may multiply to at most 1e6 at any moment, the rate curve's cap on its
own `rate.spikes`: `faults[2]: overlapping traffic_spike factors on 'users' multiply to more than
1e6`, naming the later entry. The two caps are checked separately: a fault's factor is not
multiplied with the client's curve spikes.

A fault that lies wholly inside `warmup` (it ends before the warmup does) is accepted; the node,
op and SLO tables leave the warmup out, so only its fault block (below) says what it did.

**What `run` reports per fault** (phase 3 spec §4.4; `engine::FaultRecord`, `engine::FaultLog`,
`report::FaultBlock`). After the SLO table, one block per fault in `at` order, the same three lines
for every kind, an incident report: what the fault cost while it lasted, what followed, when the
system was itself again:

```
fault 00:00:10 node_down s for 10s
  during:   offered 400/s, served 0/s, lost 3.9k requests, backlog 100 (room 100 dropped the rest), p99_latency c op over its max for 300ms, flipped to FAIL
  after:    backlog drained in 0.2s
  recovery: back within 1% (every op's p99, every node's rho) at 00:00:20.2 (0.2s after the fault cleared)
```

**The baseline is a twin.** A scenario with faults is run twice in lockstep: the run, and a *twin*,
the same scenario with its faults stripped (the same seeds, so the same noise: until the first
fault the two are one run, bit for bit). Every figure of a block compares the run with the twin's
same step, as an incident report compares the day of the incident with the day that would have
been: the minute before the fault is no baseline on a rate that moves (a diurnal curve), at the
run's first step there is none, and inside another fault it is broken itself. A run with faults
therefore costs twice what it would without them; a scenario without faults has no twin, and its
run and its reports are byte for byte what they were before blocks existed.

The header has the fault's clock time (`HH:MM` on a whole minute, `HH:MM:SS` on a whole second,
else to the tenth: `00:00:20.2`), its kind, its node (a `dc_down`'s datacenter) and `for` as
written (none for a `cache_flush`, an instant), and, when it strikes while an earlier fault on the
same target is still counting (from that fault's `at` to its recovery), `(overlaps fault
00:00:10)`: each block counts from its own `at` and the two split nothing, so their `lost` both
hold the steps they share and are not to be added. The same target is the same node, the same
datacenter, or a `dc_down` and a fault on a node whose `dc` is its datacenter, whichever came
first (a `node_down cache_eu` inside a `dc_down eu` counts cache_eu's losses that the `dc_down`
counts too).

`during` reads the faulted part over the fault's steps (a `dc_down`: its datacenter's nodes,
clients excepted; a `cache_flush`: the cache over its warm-up; a `traffic_spike`: the client, in
requests, offered at the spiked rate, served as answered, lost as its requests lost for good, their
last attempt lost on the way or timed out, with no backlog of its own): the means of its demand and drain, then `lost`, its drops
minus the twin's from `at` to the recovery step (to the end of the run without one), so the drain's
losses after the fault are in it and an overload the twin has too is not. The figures are in the
node's own unit, as its capacity is written: requests (capacity units: requests when every cost is
1) or, at a node that counts megabytes, MB (`offered 5k MB/s, served 0 MB/s, lost 24k MB, backlog 1k
MB`). A `dc_down` keeps the requests its cut links lost apart, never added to its nodes' figure
(`lost 24k MB at its nodes and 500 requests on cut links`); a datacenter whose nodes mix requests
and megabytes counts its request-counting nodes only. Then the peak backlog and, only when it
reached the room (`queue_limit` × copies) and the node itself dropped, `(room 100 dropped the
rest)`. A `cache_flush` builds no backlog of its own; its cost is the reads its empty cache sent
on, `extra misses 454.5 to origin`: the origin's demand minus the twin's over the warm-up (the
origin is where the cache's misses go: its `backend`, a CDN's `origin`, or the `on: miss` steps
after a lookup of it), in the origin's unit. Last, what the fault did to the SLOs, each named by
the SLO table's metric, node and op: `over its max for 300ms`, the time of the fault's steps at
which the run breaks the SLO and the twin's same step does not (each step judged alone by the SLO
table's own rules, an SLO with `over` as if it had none), and `flipped to FAIL`, when the SLO
table's verdict (the run's, or for an SLO with `over` its worst window's) differs from the twin's.
A flip belongs to the run, so it is named on each fault that broke the SLO during its window, or on
every fault when none did.

**Retries: attempts and requests lost for good.** A client's [`retries`](#node-types) and a
[worker pool's](#message_queue-and-worker_pool) send a failure again, and a failing node is then
sent every request several times: ch. 1's retry storm. Its drops count **attempts**, the servers'
work, and a request dropped twice and answered on its third attempt cost its user nothing. When a
retrying client's paths reach the faulted node (or datacenter), or a retrying pool calls into it,
`during` says both: `lost 8.907k attempts (2.939k requests lost for good)`, the node's drops minus
the twin's in attempts (and its rates, backlog and room in attempts too; a node that counts MB
keeps MB), then the requests the users lost: the final losses of the clients whose paths reach it
(their last attempts lost on the way or timed out), minus the twin's, over the same steps. A pool's
failed calls lose no user request (the queue answered the publish), so a sink's outage under a
retrying pool reads `lost 999 attempts (0 requests lost for good)`; the jobs come back on the queue
(`after`, below) and the last attempts' failures are the pool's dead letters (the node table's
`dead_lettered/s`). The storm probe of `tests/theory/test_retries.cpp` (300/s, `retries: 2` after
100 ms, into a node with `queue_limit: 1` down for 10 s):

```
fault 00:00:10 node_down s for 10s
  during:   offered 890.8/s, served 0/s, lost 8.907k attempts (2.939k requests lost for good), backlog 1 (room 1 dropped the rest)
  after:    backlog drained in 0.1s, retries added 5.968k attempts
  recovery: back within 1% (every op's p99, every node's rho) at 00:00:20.1 (0.1s after the fault cleared)
```

Without retries the same outage loses 2.999k requests: the retries saved 60 and cost 5,968
attempts. A scenario where no retrying client or pool reaches the faulted node prints the block
without either figure, as before retries existed.

`after` is a flushed cache's warm-up (`cache warm again in 12.1s`, the time its hit ratio is whole
again; `interrupted by the flush at 00:10:05` when the next flush of the cache emptied it first,
which ends this flush's window there: its extra misses are those up to that flush) or, for every
other kind, the time from the end of the fault to the end of the first step
at which the backlog it left is back at the twin's (`no backlog to drain` without one, `backlog
not drained by the end of the run`); a `traffic_spike`'s backlog is downstream, and the line names
the node with the most work left (`s backlog drained in 0.2s`). Then what the retries cost beyond
the twin's from `at` to the recovery, each only when there was some: `retries added 5.968k
attempts`, the attempts the clients whose paths reach the faulted part (a `traffic_spike`: its
client) sent beyond their offered load, and `pool w re-published 999 jobs` (`pools v, w ...` for
several), the jobs the retrying pools that call into it put back on their queues. A `node_down` on
a node that some op reaches through a [service discovery](#service_discovery) adds what its
failover cost, the requests sent to the dead replicas from `at` to the recovery (they are in `lost`
too): `failover 10s: 2.5k requests sent to the dead replica` (`replicas` when the fault took
several).

In `run --report json` each fault's `during` has `lost_unit` (`"requests"`, `"MB"` or
`"attempts"`) and `lost_requests` (the requests lost for good; `null` when no retries are
involved), and its `after` has `retries_added_attempts` and `pool_republished_jobs` (`null` when
the text prints none) and, in a scenario with a `service_discovery`, `failover_lost` (the requests
sent to dead replicas; `null` when the fault has no failover). A scenario without a discovery has
no `failover_lost` key, as its report had none before.

`recovery` is the start of the first step after the fault clears (a `cache_flush`: from the flush)
at which every client op's p99 (the step's lognormal fit) and every node's rho are within 1 % of
the twin's same step, with how long after the end of the fault (`after the flush`) that was, or
`not recovered by the end of the run`. Durations in the body are seconds with at most one decimal
(`0.2s`, `10s`), but `over its max for`, a duration as the tables print one (`300ms`), and a
`failover`, as `explain` prints it. The
criterion is "back where it would have been", not "the SLOs pass again": an SLO with room to spare
passes while the backlog still drains, and one that fails without the fault never passes.

## Command line: `simsys run --replications` (phase 3)

```
simsys run scenario.yaml --replications N [--jobs J] [--seed S] [--duration D] [--report table|json]
```

A run is deterministic for its seed, and the clients' [`rate.noise`](#rate-curve-clientrate) is its
one random input, so one run is one sample of the noise: its p99 says what that afternoon of
traffic did. `--replications N` runs the scenario N times with seeds `seed`, `seed + 1`, ...,
`seed + N − 1` (`seed` is the file's, or `--seed`'s; past 2147483647 the seeds wrap around to
-2147483648) and prints, for every number of the report, how far the N runs spread: the "how sure
are we" of a capacity plan (spec §7).

| option | rule | refused with |
|---|---|---|
| `--replications` | 1 to 1000; default 1. 1 is the plain run, byte for byte, `--series` included | `<file>: error: --replications must be between 1 and 1000` |
| `--jobs` | worker threads for the replications, 1 to 1024, ignored without `--replications`; default: the machine's hardware threads. Each replication is its own simulation, written into its own slot, so the output is the same bytes whatever `--jobs` says (the pool is `simsys sweep`'s, [threads](study/threads.md)) | CLI11's range message |
| `--series`, `--series-ops` | a single run's file: refused with more than one replication, before anything runs or any file is opened | `<file>: error: --series needs a single run (one series file per seed is phase 4 material)` (`--series-ops` likewise) |

**The cells.** Every number of the node, link, database and op tables becomes the median of the N
runs' values, then the 10–90 % band in brackets: `5.69ms [5.64–5.75]`. The band's ends leave out
their unit when both have the median's; when either is in another unit, or is not a number, both
are printed whole (`1.2ms [998us–1.4ms]`, `1.2ms [1.1ms–unavailable]`). A cell whose three texts
are the same prints once (`0`, `-`), and a node's `unavailable` mark becomes `unavailable in 2 of
5` when only some runs had it. The quantiles are **nearest rank**: the p-quantile of n values is
the value of rank ceil(p·n) in ascending order, so for 5 runs the band is the smallest and the
largest value and the median the 3rd, for 10 runs the 1st, the 9th and the 5th (for an even count
the lower of the two middle values). Every cell is therefore a number some run printed, which
interpolation would not give; with the handful of runs a replication has, an interpolated
quantile's extra digits would be precision the sample does not have. The band is the spread of
the runs, not a confidence interval of their mean.

**The SLOs.** Each SLO is judged by its own **median run**, the run whose observed value for it is
the median: `PASS in 3 of 5` (the median run passed, and 3 of the 5 runs did) or `FAIL (passed in 2
of 5)`, with the median run's observed value, and its seed at the end of the row (`median run: seed
5`). Two SLOs can have two different median runs. The exit code is the median runs': 2 when some
SLO's median run fails, whatever the seeds that passed; 0 when every one passes, 1 on an error (or
a run that observed no step).

**The run that stands for all** is needed where one run's report is: the summary line (steps, drop
rate), the fault blocks and, in JSON, everything around `replications`. It is the first SLO's
median run (so that verdict, its observed value and the report around them are one run's); without
SLOs the median run of the first op's mean latency. The table's first two lines say what the cells
are and which seed it is (`the summary line and the fault blocks are seed 2's`). Scripted [faults](#faults) are the same in every
replication (they are part of the scenario, not of the noise), so their block is printed once,
with the median run's numbers.

**Without noise** (no client's rate has a `noise` block with a `sigma` above 0: `sigma: 0`
multiplies the rate by exactly 1 at every step) every seed gives the same run: `--replications N`
runs once and prints
`replications: the scenario has no noise, so every seed gives the same run` as its first line,
then the plain run's table, byte for byte, with its exit code. Under `--report json` stdout is the
plain run's JSON alone and the note goes to stderr.

On hello with `rate: {base: 100/s, noise: {sigma: 0.2, correlation: 10s}}` and an SLO `p99_latency
users ping max 5.7ms` (seeds 1 and 3 fail it):

```
$ simsys run probe.yaml --replications 5
replications: 5 runs, seeds 1..5; a cell is the median across the runs [its 10–90 % band], nearest rank
each SLO is judged by its own median run; the summary line and the fault blocks are seed 2's
scenario hello  600 steps observed  drop rate 0%
nodes
  node                 mean_rho                peak_rho  peak_backlog  dropped  rejected  red_s  peak_lag
  users                       -                       -             -        -         -      -         -
  api    0.103 [0.09377–0.1124]  0.1388 [0.1305–0.1477]             0        0         0      0         -
ops (percentiles estimated: lognormal fit to the response moments)
  client.op           offered/s         attempts/s                mean                 p50                 p95                 p99  (estimated)
  users.ping  103 [93.77–112.4]  103 [93.77–112.4]  1.62ms [1.61–1.63]  1.33ms [1.32–1.34]  3.72ms [3.69–3.75]  5.69ms [5.64–5.75]
slo (each: its median run's verdict and observed value, and the runs that passed it)
  status          metric       node   op    observed  max
  PASS in 3 of 5  p99_latency  users  ping  5.69ms    5.7ms  median run: seed 2
```

With a second SLO, `utilization api max 0.1163 over 10s`, its worst 10 s windows over seeds 1..5
are 0.1327, 0.1163, 0.1210, 0.1084 and 0.1164: its median run is seed 5, which fails it, so the run
exits 2 while the p99 SLO is still judged by seed 2:

```
  status                   metric       node   op    observed  max
  PASS in 3 of 5           p99_latency  users  ping  5.69ms    5.7ms   median run: seed 2
  FAIL (passed in 2 of 5)  utilization  api    -     0.1164    0.1163  (worst 10s from 00:00:40)  median run: seed 5
```

**JSON** (`--report json`) is the report of the run that stands for all, its `slo` array holding
the replicated verdicts (each SLO's median run's, with one more field, `median_seed`, that run's
seed), plus one key after all of a run's own (after `faults`, when the scenario has some),
`replications`: `seeds`, the N seeds in order, `median_seed`, the seed of the run whose report
surrounds it, and `per_seed`, with one object per op (`client`, `op`, then `offered_per_s`,
`attempts_per_s`, `answered_per_s`, `timed_out_per_s`, `timed_out_attempts_per_s`, `mean_s`,
`p50_s`, `p95_s`, `p99_s`), per node (`id`, then `mean_rho`,
`peak_rho`, `peak_backlog`, `dropped`, `dead_lettered`, `dead_lettered_per_s`, `rejected`, `red_s`,
`unavailable`, `staleness_s`,
`replication_dropped_per_s`, `committed_writes_per_s`, `peak_queue_lag_s`, then, only when the
scenario has a [`ws_gateway`](#ws_gateway), `connections` and `connections_peak`, as in the run's
own node objects) and per SLO (`metric`,
`node`, `op`, `observed`, `status`), each number a list with one entry per seed, in the order of
`seeds`, written exactly as that seed's own `run --seed S --report json` writes it (null where it
writes null). The bandwidth links' per-seed numbers are not in it (the median run's `links` are).

```
  "replications": {
    "seeds": [1, 2, 3, 4, 5],
    "median_seed": 2,
    "per_seed": {
      "ops": [
        {"client": "users", "op": "ping", "offered_per_s": [104.97255928866007, ...], ..., "p99_s": [0.005712278457367183, 0.005693737870385378, ...]}
      ],
      "nodes": [ ... ],
      "slo": [
        {"metric": "p99_latency", "node": "users", "op": "ping", "observed": [...], "status": ["fail", "pass", "fail", "pass", "pass"]}
      ]
    }
  }
```

Ignored: confidence intervals proper (a t-interval of the mean, batch means), random or
correlated faults across replications, a series file per seed (phase 4), and replications of a
sweep's points.

## Command line: `simsys sweep` (phase 2c)

```
simsys sweep scenario.yaml --param <dot.path>=<from>..<to>:<n> --metric <m> [--metric <m> ...]
             [--jobs N] [--format table|csv]
```

Runs the scenario once per point, each run exactly as `simsys run` would (the file's duration,
step, warmup and seed), with one key replaced, and prints one row per point: the point's value,
then one column per `--metric`, in the order given (spec §19 entry 15). It answers the question
a design review asks after `explain`: *how much of this do I need?*

| option | rule | refused with |
|---|---|---|
| `--param` | required. `<dot.path>=<from>..<to>:<n>`: the key is a dot path through **maps only** (`nodes.users.rate.base`, `nodes.transcoders.workers`), which must exist in the file and hold a single value; a path through a list (`links.0.latency`) is refused, since list entries have no names, and a node whose id contains a dot cannot be swept or measured (the dot splits the path). The range gives `n` points, at least 2 and at most 10,000, evenly spaced from `from` to `to`, both included; both ends are numbers (each starts with a digit or `-`, so a third dot is an error) with the same unit text (`100/s..900/s`, `1ms..5ms`, `400..600`; `100/s..1k/s` is refused, not converted): `100/s..900/s:3` is 100/s, 500/s and 900/s. A key the loader reads as a whole number (a node's `workers`, `replicas`, `partitions`, `keys`, ..., an `advanced` block's `concurrency`) must get a whole number at every point, whole as written: the number before a `k`, `M` or `G` must be whole, as in a [count](#values) (`keys=1k..2k:3` gives 1.5k and is refused although 1500 keys are whole; write `1000..2000:3`) | (each after `<file>: error: `, as every refusal of an option) `--param: nodes.users.rate.bse: no such key in the scenario`; `--param: links.0.latency: '0' would index a list; a dot path goes through maps only (a list entry has no name)`; `--param: workers must be whole (400..600:4 gives 466.67)`; `--param: 1/s..2/s:20000: at most 10000 points`; `--param: 100/s..1k/s:2: both ends must be the same kind of value in the same unit (100/s and 1k/s)`; `--param: '.5ms' is not a number followed by a unit` (for `1ms...5ms`) |
| `--metric` | required, repeatable (`--metric a --metric b`, or several names after one: `--metric a b`): `<client>.<op>.mean` (the op's mean latency as the client sees it, over the answered requests), `<client>.<op>.p50`, `.p95` or `.p99` (the op table's estimated percentiles; where more than 1 − q of the delivered requests timed out, the timeout itself, printed `>1s` in the table as in `run`'s), `<client>.<op>.attempts` (what the client sent per second, its [`retries`](#node-types) included: JSON's `attempts_per_s`, printed `1.75k/s`) or `.answered` (the part answered per second: `answered_per_s`), `<node>.mean_rho` (a server's time-averaged utilization), `<node>.peak_rho` (its highest utilization of any observed step), `<node>.drop_rate` (the share of the work sent to it that it dropped, the node's own `drop_rate`), `<queue>.lag` (a message queue's peak lag after warmup, what the `queue_lag` SLO observes), `<pool>.dead_lettered` (phase 3: a worker pool's dead letters per second, JSON's `dead_lettered_per_s`, printed as a rate: `62.5/s`) or `<gateway>.connections` (phase 3: a [`ws_gateway`](#ws_gateway)'s open connections at the **end** of the run, the node table's and JSON's `connections`, printed as a count: `2k`; the end, not JSON's `connections_peak`, so that one name is one number: under a steady connect rate the level only rises and the two agree). They are the numbers `run --report json` prints, after warmup. Each name is checked against the file before any point runs | after `<file>: error: `, a name outside the grammar, `--metric: bogus: not a metric (write <client>.<op>.mean\|p50\|p95\|p99\|attempts\|answered, <node>.mean_rho\|peak_rho\|drop_rate, <queue>.lag, <pool>.dead_lettered or <gateway>.connections)`, or one that does not fit the file: `--metric: api.lag: 'api' has type service, not message_queue: only a message queue has a lag`; `--metric: api.connections: 'api' has type service, not ws_gateway: only a ws_gateway has connections`; `--metric: api.dead_lettered: 'api' has type service, not worker_pool: only a worker pool has dead letters`; `--metric: users.drop_rate: 'users' is a client, which serves nothing, so it has no drop rate`; `--metric: nope.mean_rho: the scenario has no node 'nope'` |
| `--jobs` | worker threads, 1 to 1024; default: the machine's hardware threads | CLI11's range message |
| `--format` | `table` (latencies and lags as the report's tables print them: `1.61ms`, `<1us`, `>1s`; ratios to three significant digits) or `csv` (seconds and ratios, each number the shortest text that reads back to the same double) | |

**Each point is a scenario of its own.** The point's value replaces the file's text **before**
the loader's checks, so it is validated like a value written in the file, and every point is
loaded and its graph built before any is simulated: a value the loader refuses at the ninth point
costs no run of the first eight, and names its point (`--param nodes.users.rate.base=-1/s..1/s:3`
on hello: `point 1 (nodes.users.rate.base=-1/s): scenarios/tutorial/hello/scenario.yaml:7:38: error:
node 'users': rate.base: expected a non-negative finite number`; a missing key or a bad path is reported once, without a point: every point would fail alike). A
value under a YAML anchor (`&a`) is shared with every alias (`*a`): sweeping it through either path
sweeps it everywhere the anchor is used. A number a run did not produce (a latency of an op of
which nothing was answered) is `-` in the table and an empty cell in the CSV. A sweep has no SLO
columns and no exit code per point (planned for phase 3).

**Parallel, and the same bytes for any `--jobs`.** The points run on `std::jthread` workers that
take the next point index from one atomic counter; each point owns its YAML tree, graph,
simulation and summary, and writes its row into its own slot, so nothing is shared while they run
and the output is the same, byte for byte, whatever `--jobs` says ([threads](study/threads.md)).

**Exit codes.** A sweep reports numbers and judges none: it exits 0 when every point ran, even if
an SLO fails at some point, and 1 on any error (the file, `--param`, `--metric`, or a point the
loader refuses), with the message on stderr and nothing on stdout.

How many transcoders youtube needs (Release build, about 46 s on 14 threads; each point a 24-hour
day):

```
$ simsys sweep scenarios/book/youtube/scenario.yaml --param nodes.transcoders.workers=400..600:3 \
      --metric transcode_q.lag --metric viewers_eu.stream.mean --metric transcoders.mean_rho
nodes.transcoders.workers  transcode_q.lag  viewers_eu.stream.mean  transcoders.mean_rho
----------------------------------------------------------------------------------------
400                                  2.66h                  89.3ms                 0.789
500                                  39.5m                  89.3ms                 0.692
600                                  573ms                  89.3ms                 0.599
```

Only 600 keeps the queue within its 30-minute SLO; the viewers never notice, since nobody waits
for a transcoder (the hop from the queue to the pool is asynchronous).

Ignored: grids over two keys, geometric spacing, list values, replications per point (phase 3).
