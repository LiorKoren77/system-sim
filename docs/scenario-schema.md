# Scenario schema v1.1 (phase 1)

v1 was fitted to the 12 book and lab drafts plus the first tutorial. v1.1 is the same
language with the rules the phase 1 loader (`src/config/config.cpp`) and graph builder
(`src/engine/graph.cppm`) enforce written down, `advanced.burstiness` added and
`advanced.drop_policy` withdrawn. All 14 files under `scenarios/` load under it.

Two kinds of marks:

- † : specified ahead of need; no scenario exercises it yet.
- **Phase 1** column: **read** means `explain`, `estimate` or `run` uses the value;
  **checked** means the loader validates it but nothing uses it yet; **ignored** means the
  key is accepted (a misspelt key is still an error) and its value is not looked at.

`simsys validate` runs the loader only. `explain`, `estimate` and `run` also build the
graph, which adds the checks marked *(graph)* below. `estimate` and `run` accept only
`client` and `service` nodes in phase 1; any other type fails with "... which run/estimate
do not support yet (phase 2)". `explain` works for every type.

## Values

| kind | grammar | examples |
|---|---|---|
| duration | non-negative number + `ns us ms s m h`; rounded to whole nanoseconds (below 1 ns is an error) | `250us`, `0.5ms`, `2m` |
| rate | number + optional `k M G` (decimal) + `/s` | `1.5k/s`, `500/s` |
| byte rate | number + optional `K M G` (decimal, capital K) + `B/s` | `500MB/s` |
| size † | `B KB MB GB` | `200B`, `2KB` |
| clock time † | `HH:MM`, for `at:` / `peak_at:` | `"20:00"` |
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
  times (`peak_at: "20:00"`) if the file is also read by other tools. Phase 1 reads no
  clock times.

## Top level

| key | type | required | phase 1 | meaning |
|---|---|---|---|---|
| `version` | integer | yes | read | always `1` (unquoted) |
| `name` | string | yes | read | non-empty; shown in reports |
| `seed` | integer | no | checked | default 1; within the range of `int`. Printed by `explain`, used from phase 2 (noise) |
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
overriding the cross-datacenter round trip. Phase 1 checks only that it is a list and
otherwise ignores it: every cross-DC link uses the `cross_dc_rtt` default (150 ms).

A node without `dc` lives in the **first declared** datacenter; `main` exists only when
the file declares no datacenters. (The news-feed scenario relies on this.)

## Node (common keys)

| key | type | on | phase 1 | meaning |
|---|---|---|---|---|
| `type` | enum | all | read | one of the catalogue below |
| `dc` | string | all | read | a declared datacenter; default as above. Decides whether a link without `latency` is intra- or cross-DC |
| `replicas` | integer > 0 | all | read | N identical copies behind an equal split: each copy sees `demand / N` with the node's full capacity, concurrency and queue limit; backlog and drops are reported summed. No effect on a client |
| `capacity` | rate or byte rate | all but `client` | read | service rate **per replica**; default per type (§6 of the spec). A byte rate (`500MB/s`) is read as objects of 1 MB per second. At most `1e15/s` (`1e15B/s` for a byte rate) |
| `latency` | duration | all but `client` | read | service time of one request; default per type |
| `advanced` | map | all but `client` | read | see next table |
| `ops` | map | `client`, `service`, `ws_gateway` | read (client, service); checked (ws_gateway) | client: op → share; otherwise op → step list (below) |
| `then` | list of steps | `cache`, `worker_pool`, `blob_store`, `notification_fanout`, `frontier` | checked | steps after processing; on a `cache`, steps with `on: miss` receive the flow it did not serve. Validated like any step and drawn by `explain`; simulated with these types from phase 2 |

A key outside its row is an error, e.g. `key 'capacity' is not valid for type 'client'`
(a client is a traffic source with no service rate, service time or waiting room) or
`key 'then' is not valid for type 'service'` (a service's steps are per op).

### `advanced`

| key | type | default | meaning |
|---|---|---|---|
| `queue_limit` | integer > 0 | 1000 (chosen) | requests that may wait per replica; beyond it the excess is dropped |
| `concurrency` | integer in [1, 100000] | implied: `max(1, ceil(capacity × latency))`, capped at 100000 | servers per replica, the c of M/M/c. The cap exists because the Erlang C term costs O(c) per node per step. The default is Little's law at full load (busy servers = throughput × time in service), which keeps the rate and the Erlang C term describing one machine |
| `service_variability` | number ≥ 0 | 1.0 (2.0 for `external_sink`) | squared coefficient of variation (SCV) of service time; 1 = exponential |
| `burstiness` | number ≥ 0 | 1.0 | **new in v1.1**: SCV of inter-arrival times; 1 = Poisson. With `service_variability` it forms the Allen–Cunneen factor `(burstiness + service_variability) / 2` that scales the queueing wait (Kingman's correction) |
| `yellow` | number in (0, 1] | 0.70 | utilization where the node turns yellow |
| `red` | number in (0, 1] | 0.90 | ... and red. `yellow` must be below `red`; a lone one is compared with the other's default |

`drop_policy` † of v1 is **not accepted** (`unknown advanced key 'drop_policy'`); it
returns when a node type needs a choice of what to shed.

### Steps

A **step** is `{to: <node id>, op: <op name>, on: always|miss|failure, mode: sequential|parallel, factor: <number>}`.

| key | default | phase 1 | rule |
|---|---|---|---|
| `to` | required | read | a declared node, **not the node itself** (a retry, the one reason to call yourself, is `client.retries`); *(graph)* not a client (clients only send) |
| `op` | the incoming op | read | required in `then` steps, which have no incoming op to inherit |
| `on` | `always` | read | `miss` applies to the flow a cache/CDN did not serve, `failure` to failed requests. In phase 1 only `always` steps fire: services have nothing to miss and no failures are modelled |
| `mode` | `sequential` | read | sequential steps add their latencies; **all parallel steps of an op form one group wherever they sit** among the sequential ones, and the group costs its slowest branch |
| `factor` | 1 | read | finite, positive, at most `1e6`; multiplies the flow (fan-out: one post → 100 feed writes) |

An op with an empty step list (`deliver: []`) is terminal: the node serves it and forwards
nothing. *(graph)* Every step needs a link that carries demand from the node to its
target and, if that link lists `ops`, lists the step's op.

## Node types

| type | own keys | phase 1 |
|---|---|---|
| `client` | `rate` (rate curve), `ops` (map op → share), `keys` `{zipf: s, space: N}` or `{uniform: N}`, `sizes` (map op → size), `timeout`, `retries` † | `rate.base` (at most `1e15/s`) and `ops` read; `timeout` checked (a duration), not yet used; `keys`, `sizes`, `retries` ignored |
| `cdn` | `ttl`, `hit_ratio` (`fixed: h` or `model` †), `origin` (node id) | `origin` checked (a declared node); others ignored |
| `load_balancer` | `policy` `round_robin|least_connections|weighted|consistent_hash`, `vnodes` (int, consistent_hash) | ignored |
| `rate_limiter` | `algorithm` `token_bucket|leaky_bucket|fixed_window|sliding_log|sliding_counter`, `limit` (rate), `burst` (int), `backend` (node id of a cache holding counters) | `backend` checked; others ignored |
| `service` | `ops` | read |
| `cache` | `eviction` `lru|lfu|fifo|ttl`, `strategy` `cache_aside|read_through|write_through|write_around|write_back`, `keys` (int capacity), `hit_ratio` (`fixed: h` or `che`), `backend` (node id for read/write-through), `ttl`, `then` | `backend` checked; others ignored |
| `sql_db` | `replication_lag`, `shards` (int), `pool` (int connections) † | ignored |
| `kv_cluster` | `nodes` (int), `vnodes`, `replication` N, `write_quorum` W, `read_quorum` R, `mode` `ap|cp` | ignored |
| `message_queue` | `partitions`, `retention` † | ignored |
| `worker_pool` | `consumes` (queue node id), `workers`, `cost` (duration per job), `then` | `consumes` checked; others ignored |
| `blob_store` | `then` (its byte-rate `capacity` is the common key) | ignored |
| `search_index` | `build_interval` | ignored |
| `ws_gateway` | `max_connections`, `heartbeat` (duration), `fanout` (avg recipients per message), `ops` | `ops` checked as step lists; others ignored |
| `service_discovery` | `failover` (duration) | ignored |
| `id_generator` | `algorithm` `snowflake|ticket_server|uuid` | ignored |
| `external_sink` | `failure_rate` (0–1), `politeness` (rate per host) | ignored |
| `notification_fanout` | `channels` (map name → node id), `dedup` (0–1), `then` | ignored |
| `frontier` | `politeness` (rate per host), `hosts` (int), `then` | ignored |

"Ignored" own keys are still checked by name, so a misspelling is caught today; their
values are typed in the phase that simulates the type.

### Client `ops`

A map op → share. Every share is a number in (0, 1] (leave out an op that never happens),
there is at least one op, and the shares sum to 1 within 1e-6. Order does not matter.

## Rate curve (`client.rate`)

```yaml
rate:
  base: 10k/s                                         # required; the only term phase 1 reads
  wave:   {shape: sine|square|diurnal, period: 24h, amplitude: 0.6, peak_at: "20:00"}
  ramp:   {from: 0, to: 10k/s, over: 5m}     # †
  spikes: [{at: "12:00", for: 10m, factor: 3}]
  points: [[0s, 1k/s], [1h, 5k/s]]          # replaces base †
  noise:  {sigma: 0.15, correlation: 5m}     # phase 2 †
```
Terms multiply. Phase 1 checks the key names and ignores every term but `base`;
`explain` prints "(rate curve: base only in phase 1)".

## Links

`{from, to, dir: in|out|inout (default out), ops: [..] (default all), latency, bandwidth †, weight}`.

| key | phase 1 | rule |
|---|---|---|
| `from`, `to` | read | declared nodes, **different** from each other (a node does not call itself over the network) |
| `dir` | read | the direction **demand** travels. `out`: `from` sends to `to`. `in`: `to` sends to `from`; the link is written from the receiver's side. `inout`: like `out`, and replies flow back on the same link (the normal request/response case). Phase 1 charges every hop a request leg and a reply leg (2 × one-way) whatever `dir` says |
| `ops` | read | a non-empty list of distinct op names; absent means all ops. `ops: []` is an error (it would load as "all", the opposite of what it says) |
| `latency` | read | **one way**. Default: half the round trip, `intra_dc_rtt / 2` = 250 µs inside a datacenter, `cross_dc_rtt / 2` = 75 ms across |
| `bandwidth` † | ignored | phase 2 models byte flows |
| `weight` | read | finite, ≥ 0, at most 1e9; default 1. When several links from a client carry the same op, the op is split `weight_i / Σ weight`. 0 means a drained backend. *(graph)* An op whose every link has weight 0 is an error. On other links it has no effect in phase 1 |

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

| metric | `node` | `op` | `max` | phase 1 evaluates |
|---|---|---|---|---|
| `p50_latency`, `p95_latency`, `p99_latency` | required, a **client** | optional, one the client sends | duration | the client's estimated end-to-end percentile; without `op`, the worst of its ops |
| `drop_rate` | optional | not allowed | number in [0, 1] | the node's dropped / the requests sent to it, after warmup; without `node`, the worst node's (see note) |
| `utilization` | required, not a client | not allowed | number in [0, 1] | the node's mean busy fraction (drain / capacity) after warmup |
| `queue_lag` | optional | not allowed | duration | reported `skipped` until phase 2 |
| `staleness` | optional | not allowed | duration | reported `skipped` until phase 2 |

The kind of `max` follows the metric, so `0.01` can never be read as 0.01 seconds.
`op` is only valid for latency metrics (`slo: op is only valid for latency metrics`): drop
rate and utilization are measured per node, not per op.

Note: `drop_rate` is per node, Σ dropped / Σ demand of that node, so it stays in [0, 1]
even when fan-out multiplies the clients' requests before they reach it (one post → 100
feed writes would make "dropped anywhere / offered by clients" 90). Without `node` it is
the worst node's, since every node must keep the bound; the table header names that node.
When no step was observed every SLO is `skipped` and `run` exits 1: no data is an error,
not a pass.

## Faults

`{at: clock-or-duration, for: duration, kind: node_down|dc_down †|partition|latency_spike †|traffic_spike †|cache_flush †, node|dc|between †, fraction, factor †}`.

`partition` takes either `between: [dc-a, dc-b]` † (cut two datacenters apart) or `node: <id>, fraction: <0–1>`
(split a clustered node into two groups of the given proportion that cannot see each other;
`fraction: 0.5` is two halves).

Phase 1 checks only that `faults` is a list; faults are typed and simulated in phase 3.
