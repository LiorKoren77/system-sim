# System Design Simulator — Design Spec

Date: 2026-10-28 (rev. 2, fluid engine) · Status: draft for review · Licence: MIT ·
Repo: github.com/LiorKoren77/system-sim

## 1. Purpose

A deterministic **fluid-flow simulator** in C++23 where a distributed system is a graph of
configurable, capacity-limited nodes. A YAML file declares nodes, their capacities and
latencies, the links between them (with their own latencies) and the datacenters they live
in. Traffic is modelled as *rates* (requests per second, bytes per second) that flow along
the edges and change over time: daily waves, spikes, ramps, noise. Every node reports its
utilization, backlog, latency estimate (mean, p95, p99) and drop rate, with yellow and red
zones. Students run the eight systems from *System Design Interview* vol. 1 plus four
standalone labs (chapters 4–7), break them, and fix them.

Two goals of equal weight:

1. **Teaching system design**: make saturation, queueing, caching, replication, quorum and
   failures visible, reproducible and instantly re-computable after a design change.
2. **Studying and teaching modern C++**: the code base deliberately exercises C++20/23
   features (§10) and each one gets a short study note in `docs/study/`.

Out of scope for v1: a GUI (a later project that reads the time series this engine writes),
request-level discrete-event simulation (§15, deferred), packet-level networking,
real wall-clock execution.

## 2. Decisions taken

| # | Decision | Choice |
|---|---|---|
| 1 | Engine | Time-stepped fluid-flow model with per-node analytic queueing (pointwise stationary fluid flow approximation). Rationale in `docs/approaches.md`. The discrete-event engine is deferred. |
| 2 | Config language | YAML only. |
| 3 | Student-facing knobs | Simple keys with defaults; queueing details under `advanced:`. |
| 4 | Latency model | Every node has a service latency and every edge a link latency, both configurable; defaults from the book's back-of-the-envelope table (§6). |
| 5 | Book coverage | 8 systems + standalone labs for rate limiter, consistent hashing, KV store, unique ID generator. |
| 6 | Toolchain | GCC 14 primary, Clang 18 secondary. C++23, named modules. |
| 7 | Licence / hosting | MIT, public GitHub repo under LiorKoren77. |
| 8 | Randomness | Built in from the start, in layers (§9): analytic per-node randomness in phase 1, workload noise in phase 2, replications and random faults in phase 3. |

## 3. The fluid engine

### 3.1 Time stepping

The simulation advances in fixed steps `dt` (default 100 ms, configurable). At each step
the engine evaluates every node once in topological order of the flow graph (cycles —
e.g. retry feedback — use the previous step's value). A 24-hour scenario with 50 nodes is
≈ 43 million node evaluations and runs in seconds.

### 3.2 Flows

A `Flow` is what travels along an edge during one step: for each operation name, a rate
(ops/s), an average payload size (bytes), and the latency accumulated so far expressed as
mean and variance. Flows are additive: a node's input is the sum of the flows on its
incoming edges.

### 3.3 What a node does each step

Per replica (each of `replicas: N` copies sees `demand / N`), with `s` the service time
and `c` the concurrency (revised, see §16):

```
inflow     = Σ incoming flows                      (per op)
demand     = Σ_op inflow_op × cost_op              (requests/s; cost_op = 1 until the phase 2 op table)
drain      = min(capacity, demand + backlog/dt)    (serve what it can)
backlog   += (demand − drain) × dt                 (grows past capacity, shrinks below)
overflow   = max(0, backlog − queue_limit) / dt;  backlog = min(backlog, queue_limit)
rho        = drain / capacity                      (busy fraction; 1 when capacity is 0)
k          = (burstiness + service_variability) / 2          (Allen–Cunneen / Kingman factor)
rho'       = min(min(demand, drain) / capacity, 0.99)   (the arrivals' load, not the busy
                                                   fraction: equal unless a backlog drains
                                                   after the load fell; §19 entry 2)
Wq         = k × ErlangC(c, rho') × s / (c × (1 − rho'))     (stochastic queueing, §9.1)
wait_mean  = max(backlog / capacity, Wq)           (fluid drain time vs. stochastic wait: the larger)
latency    = wait_mean + s
variance   = k² × Var(Wq at rho') + service_variability × s²  (always the stochastic term)
outflow    = drain, routed to downstream edges by the node's behaviour
```

The two waits are blended by `max`, not added: by Little's law the M/M/1 wait at
rho 0.99 corresponds to about 99 queued requests, and once the fluid backlog is larger
than that the stochastic queue *is* the backlog. The variance keeps the stochastic term
even when the fluid term wins the mean, so percentiles keep rising as overload deepens.
Applying `k²` to the variance is our own heuristic; Allen–Cunneen scales only the mean.

Zones: green below `yellow` (default 0.70 utilization), yellow up to `red` (0.90), red
above; drops are `overflow` (and an external sink's failures); what the behaviour rejects (a rate
limiter) is counted apart, and a request slower than its client's timeout is timed out, neither
dropped nor rejected (plan 2c Task 13: no node failed it).

### 3.4 Links

A link adds its latency to the flow's accumulated mean. If the link latency is shorter
than `dt` that is all it does; if it is longer (cross-datacenter links at 150 ms with
`dt` = 100 ms) the flow is also delayed through a ring-buffer delay line of
`ceil(latency / dt)` steps (revised in phase 1 to `round(one_way / dt)`, see §16). Bandwidth, when given, caps bytes/s and queues the excess like a node (phase 2c: a pipe of one
deterministic server in megabytes that waits for whole requests and drops past a 1000 MB room;
a write pays its whole serialisation, any other op its first megabyte; see §19 entry 3).

### 3.5 End-to-end latency and percentiles

Each hop contributes a mean and a variance (M/M/c waiting-time distribution plus the
service time distribution). Along a path they add. The client fits a lognormal to the
total mean and variance and reports p50 / p95 / p99. Fan-out in parallel takes the maximum
of the branches (approximated by the max of their means plus the largest variance).
All percentiles are labelled "estimated" in reports.

### 3.6 Two evaluation modes

- `estimate`: one steady-state evaluation at a given instant's rates. Instant, no time axis.
- `run`: the time-stepped loop above, producing time series.

Determinism: one seed, one thread per run, floating-point evaluation in fixed order.
Identical input gives byte-identical output. `sweep` runs its points in parallel, each a
simulation of its own, so its output does not depend on the number of threads (§19 entry 15);
replications across seeds are phase 3.

## 4. Core types

- `SimTime`: strong wrapper over `std::chrono::duration<std::int64_t, std::nano>`.
- `Rate`, `Bytes`, `Latency`: strong units with compile-time parsing of `10k/s`, `500MB/s`, `0.5ms`.
- `Flow`: `small_map<OpId, {rate, avg_bytes, latency_mean, latency_var}>`.
- `Link`: `from`, `to`, `direction` (`in` | `out` | `inout`), `ops`, `latency`, optional `bandwidth`, optional `weight`, delay line.
- `DataCenter`: name, nodes, pairwise latency matrix (defaults: intra 0.5 ms, cross 150 ms); up/down and partition flags over time.
- `Capacity`: `service_rate` (ops/s) or `latency` (service time), `concurrency` `c`, `queue_limit`, `yellow`, `red`, `service_variability` (squared coefficient of variation, default 1.0 = exponential).
- `Workload`: a composable rate curve (§8) plus op mix, key skew, payload sizes, timeout and retry policy.
- `Node`: id, datacenter, `Capacity`, backlog state, behaviour, metrics handle.
- `Behaviour`: the pluggable part (§5).

## 5. Extension points (concepts)

```cpp
template <class B>
concept NodeBehaviour = requires(B b, const Flow& in, NodeContext& ctx) {
    { b.route(in, ctx) } -> std::same_as<Routed>;      // outgoing flows per edge + rejected flow
    { b.cost(OpId{}) }   -> std::same_as<double>;      // capacity units per op
};
template <class M> concept HitRatioModel   = requires(M m, const Workload& w, std::size_t keys) { { m.hit_ratio(w, keys) } -> std::same_as<double>; };
template <class P> concept SplitPolicy     = requires(P p, std::span<const Replica> r, const Flow& f) { { p.weights(r, f) } -> std::same_as<Weights>; };
template <class A> concept AdmissionModel  = requires(A a, Rate in, SimTime dt) { { a.admit(in, dt) } -> std::same_as<Rate>; };
template <class C> concept ReplicationModel = requires(C c, const Flow& f) { { c.amplify(f) } -> std::same_as<Flow>; { c.latency_of(std::span<const Latency>{}) } -> std::same_as<Latency>; };
```

Behaviours compose policies at compile time (`Cache<CheLru, WriteThrough>`); a registry
maps YAML strings to instantiations; the graph holds behaviours behind a small type-erased
wrapper so the step loop is uniform.

## 6. Latency and throughput defaults (back-of-the-envelope table)

Shipped as `defaults/latency_numbers.yaml`, loaded before every scenario, overridable per
node or link. Sources: *System Design Interview* ch. 2 and the "latency numbers every
programmer should know" table.

| key | value | default for |
|---|---|---|
| `memory_read_1mb` | 250 µs | cache service time floor |
| `ssd_random_read` | 150 µs | SQL/NoSQL read |
| `disk_seek` | 10 ms | SQL write / HDD read |
| `disk_read_1mb` | 30 ms | blob store per-MB cost |
| `network_read_1mb` | 10 ms | CDN / blob per-MB transfer |
| `compress_1kb` | 10 µs | service CPU cost unit |
| `intra_dc_rtt` | 500 µs | link inside a DC |
| `cross_dc_rtt` | 150 ms | link between DCs; per-pair matrix allowed |
| `third_party_api` | 100 ms, variability 2.0 | external sinks |
| QPS defaults | web 2k, cache 100k, SQL 5k reads / 1k writes, KV 50k, queue 1M msg/s, blob 500 MB/s | `service_rate` when omitted |

`simsys explain` prints every resolved number and where it came from.

A SQL write uses 5 capacity units of the 5k/s read rate unless the scenario gives it a `cost`:
the table's "5k reads / 1k writes" for one machine (§18 entry 7).

The per-MB rows price a **byte-sized node** (§19 entry 2): a `cdn` or `blob_store` whose capacity
is a byte rate counts megabytes, one megabyte taking `network_read_1mb` = 10 ms at a CDN and
`disk_read_1mb + network_read_1mb` = 40 ms at a blob store (chosen serial: no pipelining), and a
request costs its size in MB. `third_party_api` is the external sink's (§19 entry 8, at a chosen
1k/s); the queue's 1M msg/s its append (100 µs chosen), and its `retention` defaults to Kafka's 7
days (§19 entry 4).

## 7. Node catalogue (as flow transfer functions)

| Node | Transfer function and knobs | Chapters |
|---|---|---|
| `client` | (revised, see §19 entries 1 and 12) emits the workload curve per op, each op with an optional size (`sizes`); receives replies, fits percentiles; `timeout` times out the share of each step's fitted response slower than it (answered requests' moments truncated, percentiles capped, a latency SLO failing when its quantile timed out); since phase 3 its `retries` send the lost and timed-out share again as feedback demand (§20 entry 7) | all |
| `cdn` | (revised, see §18 entry 10 and §19 entry 2) a read-only cache in front of a required `origin`, which fetches its own misses: the hit share `h` (`fixed`, `che` over the CDN's `keys` in objects and the clients' key distribution, or none: `h = 0`) is answered at the edge and the misses `1 − h` go to the origin as the same op (the `read_through` shape of a cache). The ops in `write_ops` (default `write`) are refused at build: content changes at the origin. A service's `on: miss` step after a CDN is refused, since it would fetch each miss twice. `ttl` accepted, not modelled; a byte capacity (`100GB/s`) makes it count megabytes, its queue waiting for whole streams and a stream answered at its first megabyte (§19 entry 2); one instance per `dc`, as the scenario writes it | 1, 14, 15 |
| `load_balancer` | (revised, see §17) forwards every op over its links, split by link `weight`; the policy sets how the replicas *behind* it queue: `round_robin` / `weighted` independent copies, `least_connections` one pooled M/M/(c·N) queue, `consistent_hash` the hottest copy at `1 + z_M/sqrt(vnodes)` × the average (z_M the expected maximum of M standard normals, §19 entry 9), queued apart from the cool ones | all |
| `rate_limiter` | (revised, see §17) `token_bucket` (bucket level is fluid state, bursts pass until it empties), `leaky_bucket` (the node's own queue as one deterministic leak slot), `fixed_window`, `sliding_log`, `sliding_counter`, each stepped once per step; rejected flow is its own channel (`reject_rate`), never forwarded and not a drop; an optional counter `backend` sees every checked request | 4, 8, 10 |
| `service` | per-op cost; downstream steps sequential (latencies add) or parallel (max); `replicas: N` | all |
| `cache` | (revised, see §17 and §18) hit ratio from `fixed: h` (phase 2a) or `che` (LRU under the key distribution of the clients that reach it, filtered behind another cache, partitioned behind a hash ring; phase 2b); `write_ops` name the writes, every other op is a read; strategy `cache_aside` (misses fetched by the service's `on: miss` step) / `read_through` / `write_through` / `write_around` (phase 2a) / `write_back` (writes acknowledged from memory and flushed to the backend as an asynchronous hop; phase 2b); `then` steps, `always` or `on: miss` (phase 2b) | 8, 11–14 |
| `sql_db` | (revised, see §18 entries 6–9) one primary and `replicas − 1` read replicas: writes to the primary, reads split over the replicas, every replica replaying the committed writes; a write costs 5 read units by default; `pool` sets `c` and caps the rate by Little's law; `shards` divides the load; staleness = primary sojourn + replication lag + replica sojourn, and a replica that misses committed writes fails the `staleness` SLO | 8, 10–12, 15 |
| `kv_cluster` | (revised, see §19 entry 9) `nodes` machines on a hash ring with virtual nodes, the hottest at `min(1 + z_M/√(N·vnodes), nodes/N)` × the average as its own queue; every request goes to N replicas (N capacity units); a write is answered at the W-th fastest, a read at the R-th, of its own replica set (the hot machine in it with probability `N k / nodes`), computed from each machine's fitted lognormal, and lost when fewer than W (R) come back; a `node_down` of some machines runs since phase 3 (§20 entry 3); partition → unavailable fraction (CP) or stale fraction (AP): the phase 4 backlog | 6, 11–13 |
| `message_queue` | (revised, see §19 entries 4, 6 and 14) the broker's append (the publisher is answered there); every consumer group gets every message and its backlog waits in its pool, whose room is `retention`; lag = the wait of a message published now until its slowest group reads it, `max(min(k × Wq, retention), backlog / capacity)`; `partitions` printed, bounding parallelism in the phase 4 backlog; a worker pool's `retries` re-publish what its sinks failed (§20 entry 8) | 10–12, 14, 15 |
| `worker_pool` | (revised, see §19 entry 5) M/M/c with c = workers and service time = job cost, capacity = workers / job cost; consumes one queue, takes jobs from nothing else; `then` steps fire for every job drained (DAG stages chain through queues), nobody waiting for them | 9, 10, 14, 15 |
| `blob_store` | (revised, see §19 entries 2 and 7) MB/s capacity, counting megabytes: its queue waits for whole objects, a write (`write_ops`) is answered at its last byte, a read at its first megabyte, transfers deterministic; `then` steps are asynchronous event notifications, one per write; multipart uploads (parallel branches) not modelled | 14, 15 |
| `search_index` | (revised, see §19 entry 10) a service with no steps at the type's 5k/s and 5 ms; `build_interval` printed, the build-vs-serve split (rebuild load) not modelled | 13 |
| `ws_gateway` | (revised, see §20 entries 11 and 12) connections as a level `C += (connects − C / session) dt`, filled by a client op named `connect` or fixed by `connections`, capped at `max_connections × replicas`; heartbeats `C / heartbeat` on its `heartbeat` op; `fanout` multiplies a message op's steps to a `message_queue` only; a `node_down` drops its connections, which storm back over `reconnect_within` | 12, 15 |
| `service_discovery` | (revised, see §20 entry 10) a service answering `lookup` (50k/s, 500 µs); a step after a lookup is held on a dead replica for `min(failover, for)` of a `node_down`, losing r / R of that op's requests | 6, 12 |
| `id_generator` | (revised, see §18 entry 12) a `service` whose capacity and service time its `algorithm` supplies: `snowflake` 4.096M IDs/s per replica, `ticket_server` one 1k/s database (exactly one replica), `uuid` unbounded | 7, 8, 12 |
| `external_sink` | (revised, see §19 entry 8) a queue of the `third_party_api` numbers (1k/s, 100 ms, service SCV 2.0) that fails `failure_rate` of what it serves, counted as drops; the per-host politeness rate cap is modelled since phase 3, over the one frontier's `hosts` (§20 entry 15) | 9, 10 |
| `notification_fanout` | (revised, see §20 entry 14) a service (20k/s, 1 ms) whose `then` steps fire for `1 − dedup` of what it served, the rest `deduplicated`; `channels` names the queue of each channel; the retries and the dead-letter count live on the channel pools (`worker_pool` `retries`, §20 entry 8) | 10 |
| `frontier` | (revised, see §20 entry 15) a prioritiser at `capacity` in front of an unbounded queue, drained by its one consumer pool at most `hosts × politeness` (a drain bound in front of the pool, which keeps its workers); its lag is a queue's | 9 |

Consistent hashing, quorum, vector clocks, gossip and leader election appear as their
**capacity and latency consequences** inside `kv_cluster`, `load_balancer` and
`service_discovery`. Mechanisms are explained in the scenario READMEs, not simulated.

Faults (`faults:` list): `node_down`, `dc_down`, `partition`, `latency_spike`,
`traffic_spike`, `cache_flush`, each with `at:` and `for:`; random variants in phase 3.
(Revised, see §20 entries 2–5: phase 3 runs the scripted faults, `partition` and the random
variants are in the phase 4 backlog.)

## 8. Scenario format

```yaml
version: 1
name: news-feed
seed: 42
duration: 24h
step: 100ms
warmup: 5m

datacenters:
  us-east: {}
  eu-west: {}
  latency: [{between: [us-east, eu-west], rtt: 70ms}]

nodes:
  users:
    type: client
    dc: us-east
    rate:
      base: 10k/s
      wave: {shape: sine, period: 24h, amplitude: 0.6, peak_at: 20:00}
      spikes: [{at: 12:00, for: 10m, factor: 3}]
      noise: {sigma: 0.15, correlation: 5m}          # phase 2
    ops: {read_feed: 0.9, post: 0.1}
    keys: {zipf: 1.1, space: 50M}
    timeout: 500ms
    retries: 2                                        # phase 3
  lb:         {type: load_balancer, dc: us-east, policy: least_connections}
  web:        {type: service, dc: us-east, replicas: 8, capacity: 2k/s, latency: 0.5ms,
               ops:
                 read_feed: [{to: feed_cache, op: read}, {to: post_db, op: read, on: miss}]
                 post:      [{to: post_db, op: write}, {to: fanout_q, op: publish}]}
  feed_cache: {type: cache, eviction: lru, hit_ratio: che, keys: 1M, strategy: cache_aside}
  post_db:    {type: sql_db, replicas: 2, replication_lag: 50ms, capacity: 5k/s,
               advanced: {queue_limit: 500, service_variability: 1.5}}
  fanout_q:   {type: message_queue, partitions: 8}
  fanout:     {type: worker_pool, consumes: fanout_q, workers: 16, cost: 2ms,
               then: [{to: feed_cache, op: write}]}

links:
  - {from: users, to: lb, dir: inout, ops: [read_feed, post]}
  - {from: lb, to: web}
  - {from: web, to: feed_cache, latency: 0.3ms}
  - {from: web, to: post_db}
  - {from: web, to: fanout_q}

slo:
  - {metric: p99_latency, node: users, op: read_feed, max: 200ms}
  - {metric: drop_rate, max: 0.001}
  - {metric: queue_lag, node: fanout_q, max: 2m}

faults:
  - {at: 15:00, kind: node_down, node: post_db.primary, for: 30s}
```

Rate curve grammar: `base` plus any of `wave` (`sine` | `diurnal` table | `square`),
`ramp`, `spikes`, `points` (piecewise-linear list), `noise`. Terms multiply, except
`points` which replaces `base`.

Rules: a link's `latency` overrides the DC default; a node's `latency` overrides the type
default; `capacity: N/s` is sugar for `advanced.service_rate`; `replicas: N` is sugar for
N identical nodes behind an implicit equal split.

## 9. Randomness, in layers

| Layer | What | Phase |
|---|---|---|
| 9.1 Analytic per node | Erlang C wait for Poisson arrivals; Kingman / Allen–Cunneen correction using `burstiness` (arrival SCV) and `service_variability` (service SCV); per-hop mean + variance → lognormal percentiles | 1 |
| 9.2 Workload noise | (revised, see §17) Ornstein–Uhlenbeck multiplier `1 + x`, floored at 0, on the rate curve (`sigma`, `correlation`), discretised exactly; one `std::mt19937_64` per client, seeded from the one `seed` and the client's index (phase 2a). Poisson random spikes with random factor and duration: deferred, scripted `spikes` stand in for them | 2a (spikes later) |
| 9.3 Replications | (revised, see §20 entry 9) `--replications N` runs N seeds of the rate noise (9.2) in parallel and reports each number's median and 10–90 % band; each SLO is judged by its own median run, "PASS in k of N"; parameters as distributions wait for phase 4 | 3 |
| 9.4 Random faults | `mtbf` / `mttr` per node, random latency spikes and partitions; chaos mode (phase 3 runs scripted faults only; the random ones are in the phase 4 backlog, §20 entry 16) | 4 |

Not modelled: correlation between individual requests (thundering herd, synchronized
retries). These are provided as scripted faults (`cache_flush`, `timeout_storm`).

## 10. C++20/23 study map

| Feature | Where |
|---|---|
| Named modules | one module per layer; `import simsys.core;` |
| Concepts & `requires` | all extension points (§5) |
| Ranges & views, `ranges::to` | topological order, flow aggregation, report tables |
| Coroutines (`std::generator`) | rate curves as lazy generators of samples; replication/sweep job streams |
| `std::expected` | config loading and validation errors with line/column |
| `std::format`, `std::print` | reporters |
| Deducing `this` | policy mix-ins without CRTP |
| `<=>`, `= default` comparisons | strong unit types |
| Designated initializers, `consteval`, `constexpr` containers | defaults table, unit literals (`10k/s`, `0.5ms`) parsed at compile time |
| `std::span`, `std::string_view`, `std::variant` + `std::visit` | flows, policy dispatch |
| `std::chrono` | `SimTime`, wall-clock-of-day for diurnal curves |
| `std::jthread`, `stop_token`, `latch` | `simsys sweep`'s points on `std::jthread` workers with an atomic work counter (§19 entry 15, `docs/study/threads.md`); `stop_token` and `latch` wait for replications (phase 3) |
| `std::mdspan` (if available in GCC 14; else skip) | DC latency matrix |

Not used on purpose: raw `new`, macros, exceptions for control flow.

## 10a. Educational conventions

Recorded in `CLAUDE.md` so every session follows them:

1. The first use of any C++20/23 construct in a file gets an inline comment: what it is,
   which standard, what it replaces, why it fits here. Longer notes in `docs/study/`.
2. Every node type, policy and formula that models something from the book or from
   queueing theory carries explanatory documentation: what it models, the chapter, what
   it captures, what it ignores, the formula's name and assumptions, and the source of
   every default number.
3. Tests are named after the property they prove; scenario folders carry a README.

## 11. CLI

```
simsys validate scenario.yaml
simsys explain  scenario.yaml                               # resolved graph, defaults applied, sanity checks
simsys estimate scenario.yaml [--at 12:00]                  # steady state, instant; --at: phase 2+
simsys run      scenario.yaml [--seed 7] [--duration 24h] [--report table|json|csv] [--series out.csv]
                                                            # --report csv: phase 2+
simsys run      scenario.yaml --replications 50 [--jobs 8]  # phase 3
simsys sweep    scenario.yaml --param nodes.users.rate.base=1k/s..100k/s:10 --metric users.read.mean
                              [--jobs 8] [--format table|csv]  # §19 entry 15
```

`run` prints a per-node table (peak and mean utilization coloured green/yellow/red, peak
backlog, estimated p50/p95/p99, drops, time spent in red) and an SLO pass/fail block;
exit code reflects the SLO result. `--series` writes one row per step per node: the
contract for the future GUI.

## 12. Testing

- Unit tests per policy and per unit parser (Catch2).
- Theory tests: a single M/M/1 and M/M/c node must reproduce closed-form mean wait and
  Erlang C blocking; Little's law holds per node every step; a constant-rate `run`
  converges to `estimate`.
- Conservation tests: flow in = flow out + drops + Δbacklog, every node, every step.
- Scenario tests: every shipped YAML validates and runs in under 5 s; `fixed` variants
  pass their SLOs, `broken` variants fail them.
- Determinism test: two runs with the same seed produce identical series.
- CI: GCC 14 debug and release builds with `-Wall -Wextra -Werror`, and a clang-format 18
  check (Clang 18 cannot build the project, see `CLAUDE.md` §3). ASan/UBSan job: phase 5.

## 13. Phases and effort (assisted; unassisted in parentheses)

| Phase | Scope | Effort |
|---|---|---|
| 0 | Toolchain spike (GCC 14, Clang 18, CMake named modules), all 12 scenarios drafted by hand in YAML, schema adjusted until they fit | 1 week |
| 1 | Core: units, flows, step loop, links with delay lines, analytic queueing (9.1), config loader, defaults table, metrics, `validate` / `explain` / `estimate` / `run`, theory and conservation tests | 1.5 weeks (3) |
| 2 | Basic node pack (client, LB, rate limiter, service, cache with Che model, SQL, CDN, blob, queue, workers, external sink), workload curves and noise (9.2), `sweep` | 2 weeks (4) |
| 3 | `kv_cluster`, `service_discovery`, `ws_gateway`, faults, retries feedback, replications (9.3) and random faults (9.4) (done, see §20, without random faults) | 1.5 weeks (3) |
| 4 | 8 book systems + 4 labs, broken/fixed variants, SLOs, READMEs explaining the mechanisms the model abstracts | 1 week (2) |
| 5 | Study notes, docs, series format freeze, CI, performance check | 1 week (2) |
| | **Total CLI v1** | **≈ 8 weeks (≈ 15)** |
| later | GUI reading the series file | 2 months |
| later | Discrete-event engine behind the same YAML (§15) | 6–8 weeks |

Phase 2 ran as three parts (2a, 2b, 2c; §17–§19), and two types moved forward from phase 3 into
2c: the **steady state of `kv_cluster`** (ring, replication, quorum latency and losses; §19 entry
9), because news-feed's friendship graph is one and the book system could not run without it,
while its partition half (`mode: ap|cp`, sloppy quorum, hinted handoff) needs faults and stays in
phase 3; and **`search_index`**, which no phase named and which is a service with no steps
(§19 entry 10), so search-autocomplete runs too. Phase 3 keeps `ws_gateway`,
`service_discovery`, `notification_fanout` and `frontier`, with them chat-system, google-drive,
notification-system and web-crawler, and faults, retries and replications. The CLI's `sweep`,
planned for phase 2, landed in 2c (§19 entry 15).

Phase 3 ran as four parts (3a layout, 3b faults, failure steps, retries and replications, 3c the
last four types with their scenarios, 3d docs; §20) and closed its row: `service_discovery`,
`ws_gateway`, `notification_fanout` and `frontier` run, and with them chat-system, google-drive,
notification-system and web-crawler, so all fourteen shipped scenarios run; scripted faults,
client and pool retries, `on: failure` steps and `--replications` work. Random faults (9.4),
kv_cluster partitions and the other items of §20 entry 16 move to phase 4.

## 14. Repository layout

```
CMakeLists.txt  CMakePresets.json  cmake/
src/{core,queueing,flow,fluid,model,defaults,config,engine,report,cli}/
scenarios/{book,labs,tutorial}/
tests/{unit,theory,scenario}/
docs/{approaches.md, scenario-schema.md, study/, superpowers/{specs,plans}/}
```

The defaults live in a `constexpr` C++ table in `src/defaults/`, not in a
`defaults/latency_numbers.yaml` file (§16, refinement 3).

## 15. Deferred: discrete-event engine

Kept out of v1 on purpose. The YAML, node catalogue, defaults and CLI are engine-neutral;
a request-level engine would add `handle(Message&)` next to `route(const Flow&)` on each
behaviour and reuse everything else. It becomes worthwhile when the lesson is a mechanism
rather than a consequence: retry storms from stochastic bunching, vector-clock conflict
resolution, leader election timing. See `docs/approaches.md`.

## 16. Phase 1 refinements (2026-10-28)

Building phase 1 (plan `docs/superpowers/plans/2026-10-28-phase1-fluid-engine.md`)
changed or sharpened the following points. Where they conflict with an earlier section,
this section wins; §3.3 and §3.4 are already updated in place.

1. **Latency is computed on the reply path.** Flows carry rates only (§3.2 had latency
   inside the flow). Each step the engine runs a forward pass in topological order for
   demand, drain and backlog, then a backward pass in reverse order that composes the
   response time of every `(node, op)` from its own sojourn and its downstream hops. The
   §3.5 numbers are unchanged, and fan-out factors can no longer distort latency.
2. **Payload bytes are deferred to phase 2.** Nothing in phase 1 reads them; they arrive
   with `blob_store` and link bandwidth.
3. **Defaults are a `constexpr` C++ table** (`src/defaults/defaults.cppm`), not a YAML
   file loaded at start-up as §6 planned. The table is the single source of truth, every
   entry names its source (book, spec or chosen), `explain` prints each resolved number
   with its provenance, and scenario values still win. A user-editable YAML layer can be
   added later without changing the API.
4. **`replicas: N`** is N independent copies behind an equal split, as round-robin gives:
   each copy sees `demand / N` with the node's own capacity, concurrency and queue limit;
   backlog and drops are reported summed over the copies.
5. **Link `latency` is one way.** The table's datacenter numbers are round trips, so a
   defaulted link uses half of one each way (250 µs inside a DC, 75 ms across), and a
   request/reply pair adds a full round trip.
6. **Feedback edges** (chat: `chat_servers → message_q → sync_workers → chat_servers`) are
   the back edges of a depth-first search and always go through a delay line of at least
   one step: that is what "cycles use the previous step's value" (§3.1) means in code.
7. **`run` and `estimate` support only `client` and `service`** in phase 1; any other type
   fails with a "phase 2" error, while `explain` works for every type. Rate-curve terms
   other than `base` are accepted by the loader and ignored, and `explain` says so.
8. **`advanced.burstiness`** (the squared coefficient of variation of inter-arrival times,
   default 1 = Poisson) joins the schema. With `service_variability` it forms the
   Allen–Cunneen factor `k = (burstiness + service_variability) / 2` of §9.1.
9. **Wait blend: max, not sum.** The fluid wait (`backlog / capacity`) and the stochastic
   wait (`k × Wq` at `min(rho, 0.99)`) describe the same queue once it is long, so adding
   them counted it twice; summing overstated latency in early overload by 50–98 % at
   100 ms steps. The mean takes the larger of the two. The plan first took the variance
   from the winning term too; that made p99 *fall* as a backlog grew (the lognormal fit
   lost its spread faster than the mean rose), so the variance always keeps the
   stochastic term plus the service variance.
10. **Utilization is the busy fraction `drain / capacity`**, not `demand / capacity`. The two
    agree whenever the backlog is empty; a node working off a backlog at full speed is
    busy all the time and is shown red, not green.
11. **`parallel_max` is optimistic.** A parallel group costs the max of its branch means
    and the max of their variances, but E[max] ≥ max E[Xᵢ], and a `factor: 100` step counts
    as one branch, so the tail amplification of a wide fan-out (Dean and Barroso, *The
    Tail at Scale*) is not modelled. The exact rule for n iid branches,
    `quantile_max(p) = quantile_single(p^(1/n))`, needs an inverse normal CDF and is
    recorded for a later phase. Reports therefore understate the latency of wide fan-outs.
12. **Feedback hops are asynchronous, decided per call, not per box.** Two searches answer
    two questions. The node-level DFS, rooted at the clients so the result does not
    depend on YAML order, decides only which link gets a delay line. A second DFS over
    `(node, op)` calls, following unconditional steps from the client routes, finds the
    hops that close a cycle of calls: such a hop still forwards its flow (a step later)
    but adds no latency to its caller, since a request cannot wait for itself. A hop over
    a node-level back edge that closes no call cycle (A.x → B.y and B.z → A.w) is still
    waited for, read one step stale. With several clients entering one loop, the first
    client in file order decides where the loop is cut; no shipped scenario has that.

Further rules fixed while building phase 1:

- **Delay steps round to the nearest step** (§3.4): `delay = round(one_way / step)`, halves
  rounded away from zero, computed on integer nanoseconds. `ceil` delayed every 101 ms hop
  by two steps at 100 ms; rounding keeps the average arrival time honest. A hop shorter
  than half a step only adds latency on the reply path. A link longer than a million
  steps is rejected (its delay line would hold a million flows).
- **Parallel steps form one group per op** wherever they sit among the sequential steps:
  response = own sojourn + Σ sequential hops + slowest parallel hop.
- **Implied concurrency.** Without `advanced.concurrency`, `c = max(1, ceil(capacity ×
  service time))`: Little's law at full load, so that the fluid drain and the Erlang C
  term describe the same machine (the old default `c = 1` contradicted every type
  default and inflated the waits).
- **Client routing.** A client op carried by several links is split by link weight; an
  op whose every link has weight 0 is an error rather than a silent equal split.
- **The schema is v1.1** (`docs/scenario-schema.md`): it lists every loader rule and,
  for each key, whether phase 1 reads it, only checks it, or ignores it.

## 17. Phase 2a refinements (2026-10-28)

Building phase 2a (plan `docs/superpowers/plans/2026-10-28-phase2a-workload-and-first-nodes.md`)
fixed the following points. Where they conflict with an earlier section, this section
wins; §7's `load_balancer`, `rate_limiter` and `cache` rows and §9.2 are updated in place.
The rules a scenario author meets are in `docs/scenario-schema.md` (v1.2).

1. **Simulated time starts at midnight.** `t = 0` is `00:00`; a clock time in `at:`,
   `peak_at:` or a point is an offset from it, and a duration (`5m`) is accepted in the
   same places. A top-level `start: HH:MM` could shift this later.
2. **Rate curve composition.** `rate(t) = level(t) × wave(t) × Π active spike factors ×
   noise(t)`, where the level is the ramp's rate if there is a ramp, else `points`
   (linear, held flat outside the trace), else `base`. The plan wrote the ramp as a
   multiplier `ramp(t)/base`; the code applies the ramp's rate as the level itself, the
   same numbers without a division that overflows for a tiny base.
3. **Noise** is an Ornstein–Uhlenbeck multiplier `1 + x`, floored at 0, stepped exactly:
   `x_{k+1} = x_k·e^{−dt/τ} + σ·sqrt(1 − e^{−2dt/τ})·ξ` with `σ = noise.sigma` (the
   stationary standard deviation) and `τ = noise.correlation`. Each client owns one
   `std::mt19937_64` seeded with `(seed × 0x9E3779B97F4A7C15) XOR node index`, so runs
   are byte-identical with the same standard library and clients are independent;
   `simsys curve --noise` prints the same path.
4. **Op cost.** A `service` or `cache` takes `cost: {op: units}` (default 1): demand is
   `Σ rate_op × cost_op` against a capacity counted in units, and drops and backlog are in
   units too; what the node forwards stays requests.
5. **Load balancer policies.** The balancer splits by link weight whatever its policy; the
   policy becomes the *replica model* of the node behind it: independent copies
   (`round_robin`, `weighted`), one pooled M/M/(c·N) queue (`least_connections`, the
   join-the-shortest-queue ideal), or a hot spot (`consistent_hash`: the hottest copy at
   `1 + 1/sqrt(vnodes)` × the average in phase 2a; since phase 2c `1 + z_M/sqrt(vnodes)`, z_M the
   expected maximum of M standard normals, the kv ring's function with one copy per key, §19
   entry 9).
6. **Rate limiter.** Every algorithm keeps the state ch. 4 describes and is stepped once
   per step `[now, now + dt)` through `route(in, now, dt)`, with `W = limit × window`
   (window 1 s by default); `burst` sizes the two buckets only and defaults to one
   window's worth. Rejected flow is a channel of its own (`rejected_per_s`), never
   forwarded and reported apart from drops; `estimate` admits `min(in, limit)`, and
   effects inside one step are not resolved. `min(in, limit)` is the long-run rate of the
   token bucket and the fixed window, but not of the sliding counter (about 10/11 of the
   limit under a flood at ten steps per window), and it hides the fixed window's sawtooth
   within each window (under a flood the p99 of `run` is far above `estimate`'s), so
   `estimate`'s first line says so when the scenario has a windowed limiter. A windowed
   algorithm's window must be a whole number of steps, at least one (a graph error
   otherwise), so its boundaries fall on step starts.
7. **Cache (2a scope).** Only `hit_ratio: {fixed: h}` runs; `write_ops` (default
   `[write]`) names the writes and every other op is a read. `cache_aside` serves hits and
   leaves the misses to the service's `{to: db, on: miss}` step, which fires for `1 − h`
   of the preceding cache step; `read_through` forwards `1 − h` of each read to its
   `backend`; `write_through` and `write_around` send every write there synchronously (the
   same flow, since `h` is fixed); `che`, `write_back` and a cache's `then` steps wait for
   phase 2b.
8. **Rejections and the client.** Rejected and dropped requests are not answered and add
   no latency. The backward pass carries a *delivered fraction* per (node, op) next to the
   latency: the node's own `served / inflow` times its downstream's (sequential hops
   multiply, splits mix by share), and a client's latency moments are weighted by
   `offered × delivered`.

Refinements that came out of review while building:

9. **Hot and cool copies behind a hash ring.** The node behind a `consistent_hash`
   balancer is two copy groups: one hot copy at `k/N` of the demand and `N − 1` cool copies
   sharing `(N − k)/N`, each group with its own queue. A saturated hot copy sheds only its
   own excess; the node reports the hot copy's rho and zone and, as latency, the mixture a
   request sees.
10. **The replica model, not the policy, must agree.** A node behind several balancers may
    have only one replica model: `round_robin` and `weighted` agree (both independent),
    `round_robin` and `least_connections` do not and are an error. A node reached both
    through a balancer and directly takes the balancer's model for all its traffic.
11. **Drained routes carry nothing.** A link of weight 0 brings no op to its target, closes
    no call cycle and imposes no replica model; an op whose every link is drained is an
    error.
12. **Window algorithms per step.** The fixed window resets when `floor(now / window)`
    changes (so a flood across a boundary gets 2W); the sliding log keeps a ring of the
    last `ceil(window/dt)` steps' admitted counts (never more than W in any window); the
    sliding counter uses `prev·(1 − f) + cur` and admits about 10/11 of the limit under a
    flood at ten steps per window.
13. **The leaky bucket is a deterministic leak slot.** Its queue is the limiter's own: one
    server of service time `1/limit`, SCV 0 (M/D/1), with `queue_limit = burst`, so excess
    waits and only a full bucket's overflow is rejected; `capacity`, `latency`,
    `concurrency` and `queue_limit` on it are errors.
14. **The counter store sees every checked request.** A limiter with a `backend` sends one
    `counter` request per request it checks, admitted or refused, and the hop adds to an
    admitted request's response.
15. **Answered-weighted latency.** Percentiles are averaged over the requests answered,
    each step weighted by the client's offered rate times its delivered fraction, so a
    spike that a limiter mostly refuses weighs by the requests it answers, not by the
    requests offered.
16. **`reject_rate` versus `drop_rate`.** A drop is a node failing (its waiting room was
    full); a rejection is a limiter's policy working (ch. 4's 429). Each is a per-node
    ratio of its node's demand, with its own SLO metric, table column and CSV column.

## 18. Phase 2b refinements (2026-10-28)

Building phase 2b (plan `docs/superpowers/plans/2026-10-28-phase2b-che-sql-cdn.md`) fixed the
following points. The plan's header listed seven refinements; the review rounds of Tasks 2–7
corrected several of them and added the rest, so each entry below is the rule as finally implemented.
Where they conflict with an earlier section, this section wins; §7's `cdn`, `cache`, `sql_db`
and `id_generator` rows are updated in place and §6 points at entry 7. The rules a scenario
author meets are in `docs/scenario-schema.md` (v1.3).

1. **Key distribution resolution.** A client states `keys: {zipf: s, space: N}` or
   `{uniform: N}` (`zipf: 0` is stored as uniform, so the two compare equal). A cache or CDN
   with `hit_ratio: che` takes its popularity from the clients whose **ops** reach it: the
   graph follows each client's calls (a service's steps, miss steps included; a balancer's or
   limiter's routes; a cache's backend fetch and `then` steps; a CDN's origin fetch), not the
   boxes, and leaves out a limiter's `counter` request, which counts per user, not per key.
   Exactly one distinct spec must arrive; no demand, a client without `keys`, or two clients
   with different specs is a graph error naming them. *Why:* the plan walked demand edges box
   to box, and the Task 2 review found that a writer sharing a service with the cache's
   readers then imposed its keys on the cache, and that the limiter's counter hop counted as
   client keys. *Ignores:* several distributions mixed by rate (phase 3), a key space per op,
   any correlation between a client's ops and its keys.
2. **Che approximation** (Che, Tung & Wang 2002; Fricker, Robert & Roberts 2012 for why it
   holds for LRU). With popularities `p_i` and a cache of `C` keys, the characteristic time
   `t_c` solves `Σ_i (1 − e^{−p_i t_c}) = C` and the hit ratio is
   `h = Σ_i p_i (1 − e^{−p_i t_c})`. A Zipf space of up to 1e9 keys is summed over at most 2000
   log-spaced rank buckets (edges `⌊N^{k/2000}⌋`), measured at a relative error of at most
   2e-6 for `s ≤ 3` (the plan's bound was 1e-3); `t_c` is found by 80 bisection steps on
   `log t` from the bracket `t_c ≥ C` (from `1 − e^{−x} ≤ x`). The hit ratio is computed once,
   at build, and from then on a `che` cache runs exactly like a fixed one. §5's
   `HitRatioModel`, `hit_ratio(w, keys)`, became `hit_ratio(capacity) -> double`, each model
   owning its inputs (`CheHitRatio{popularity}`, `FixedHitRatio{value}`), because a fixed
   ratio has no popularity to be handed; the graph keeps the resolved inputs rather than the policy
   variant, which GCC 14.2 cannot hold in `Graph` in a Release build. *Why:* a cache is usually
   sized by hand with the 80/20 rule of thumb (an industry habit, not a figure from the book),
   which describes an ideal cache holding exactly the hottest keys; Che derives the hit ratio
   from the cache size and the key skew for the LRU cache that is really deployed (Zipf 1 over
   1e8 keys, a cache of 20 % of them: 0.92 ideal, 0.88 LRU). *Ignores:* temporal
   locality (the independent reference model), TTL expiry, a cold cache, and eviction other
   than LRU: `eviction: fifo` or `lfu` uses the same model (FIFO is two to four points lower,
   LFU better, up to the ideal hottest-keys share), which the schema and `caching.cppm` say.
3. **Filtered popularity behind a cache** (review of Task 2). A cache whose reads arrive as
   another `che` cache's misses (its `read_through` fetch, its `then` step on miss, a
   service's miss step after it) sees the cold tail: key `i` misses with probability
   `e^{−p_i t}`, so the second cache's popularity is `∝ p_i e^{−p_i t}` (Martina, Garetto &
   Leonardi, INFOCOM 2014). Two 2M-key caches over Zipf 0.9 and 10M keys hit 0.747 and 0.257,
   where the unfiltered popularity gave the second one 0.747 too. A fixed-ratio cache in
   front says how many requests miss but not which, so the popularity passes unfiltered and
   `explain` says so (refusing it would have broken `search-autocomplete`). A cache's reads
   must all come one way (filtered by one cache, or unfiltered), and caches that are each
   behind the next in a loop are refused. *Ignores:* that the misses of an LRU cache are not
   independent requests again (the filter assumes they are).
4. **Partitioned caches behind a hash ring** (review of Task 2). `keys` is per replica.
   Behind a `consistent_hash` balancer each key lives on one replica, so the replicas
   partition the key space; the hottest owns `a_hot = k/N` of it (`k = 1 + 1/√vnodes`, the
   balancer's imbalance of §17 entry 5; `1 + z_M/√vnodes` since §19 entry 9, which gives 0.920 at
   `vnodes: 1`) and the others `(1 − k/N)/(N − 1)` each, and
   `h = Σ_j a_j · Che(keys / a_j)`. The hit ratio is concave in the cache size, so uneven arcs
   hit less than one cache of `keys × N`: the consistent-hashing lab's four 2M-key servers give
   0.924 at `vnodes: 1`. Behind any other policy every replica caches the same hot keys, and
   the size is `keys`. A partitioned cache also sent requests directly is refused (no single
   popularity per replica). *Ignores:* a replica lost to a fault (the hit ratio is resolved
   once; phase 3 re-evaluates it from the same inputs), the real spread of many arcs.
5. **Cache `then` steps and `write_back`** (plan refinement 5, sharpened in review). An
   unconditional `then` step carries everything the cache answered; an `on: miss` one `1 − h`
   of the reads and none of the writes, and several miss steps form one sequential block that
   a miss takes whole. Refused: a `then` step on miss on a `read_through` cache, a service's
   miss step after a cache with a `then` step on miss (either way each miss would be fetched
   twice), a parallel miss step (a miss waits for it), and a `then` step that duplicates the
   backend hop. `write_back` acknowledges a write from memory (the client sees the cache's
   own sojourn) and flushes it to the backend as an asynchronous hop at the same rate, marked
   at build, so the store's load is unchanged. `write_ops` is optional in the model: absent
   means the type's default (`[write]`; a database's `[write, insert, update, delete]`), a
   stated list is exactly what it says. *Why:* ch. 1's CDN workflow and ch. 13's browser
   cache put a cache in front of the next tier, which ch. 1's cache-aside cannot express.
   *Ignores:* the dirty-entry queue and how far the store lags, writes lost when the cache
   dies before a flush, coalescing of repeated writes to one key (the store's load is an
   upper bound).
6. **SQL database** (plan refinement 3, corrected by two reviews). `replicas: N` is one
   primary and `N − 1` read replicas (ch. 1, "database replication"); writes go to the
   primary, reads to the replicas when `N ≥ 2`, and `shards: S` makes `S` such sets, each
   with `1/S` of the load. Each copy is its own queue of the node's capacity, its group
   tagged with a `CopyRole` (`server`, `primary`, `replica`). Every replica replays the
   writes its primary **committed**, `primary drained writes / S`, not the writes the
   clients offered (review C2: a replica replays the log, which holds nothing the primary
   dropped). The node reports the busier copy's `rho` and zone; its demand, drain, drops and
   backlog are the clients' part, so the clients' totals balance while the replicas'
   replication work shows in their `rho`, the reads' latency and the missed writes of entry
   9 (review I4). The database driver, not a balancer, picks the copy, so no balancer imposes
   a replica model on a database (review I1), and a database calls nobody (a link out of one
   is refused). *Ignores:* failover, multi-primary set-ups and conflicts, transactions and
   locks, hot shards and resharding, cross-shard queries, read-your-writes, a replica
   applying its log on a thread of its own (a replicated write waits with the reads).
7. **Write cost 5 by default** (review I3 of Task 4). A `sql_db`'s `capacity` is reads/s,
   and a write op with no stated `cost` costs **5** units, on the primary and on every
   replica that replays it: §6 gives one SQL machine 5k reads/s or 1k writes/s
   (`defaults::sql_write_cost = 5000 / 1000`, priced in one place, `defaults::op_cost`, for
   the engine and `explain` alike). `explain` prints each defaulted write op with its source,
   `cost: write 5 (default: spec §6 5k reads/1k writes)`. *Why:* a default of 1 would let one
   primary absorb the book's write load; with 5, url-shortener's database needs the book's
   own sharding. *Ignores:* a write's longer service time (it uses more capacity, not a
   longer sojourn) and the difference between a log append and a random update.
8. **Connection pool** (review C3 of Task 4, re-review 3). The plan made `pool` sugar for
   `advanced.concurrency`, which would let more connections serve faster than the disk. By
   Little's law (queries in service = throughput × service time) a pool can only cap a copy:
   `c = min(pool, implied c)` and `service rate = min(capacity, pool / service time)`, in
   capacity units (a write at cost 5 holds its connection five service times). 5k/s copies
   of 10 ms with `pool: 4` serve 400/s; 3000 reads/s on one copy drop about 2600/s. `explain`
   prints `capacity=400/s (pool 4 / 10ms)` and `c=4 (pool)` when the pool binds. Both keys
   set `c`, so stating both is refused ("pool sets c and caps the rate by Little's law; drop
   advanced.concurrency"). *Ignores:* waiting for a free connection apart from the copy's
   queue, connection set-up cost.
9. **Staleness and missed writes per replica** (review C1 of Task 4, re-review 1, 2, 4). A
   write reaches a reader after its sojourn at the primary, the `replication_lag` (a
   constant) and its sojourn at a replica; the node's staleness is the sum, 0 without
   replicas, and the `staleness` SLO compares its mean after warmup with `max`. A lag of 0 is
   not a synchronous replica: the write still waits in both queues. A replica too busy to
   replay every committed write **falls behind without bound** (MySQL's relay log and
   Postgres's WAL never skip an entry; the replica is rebuilt once the primary's retention
   runs out); the model drops the excess at `queue_limit`, so the staleness it reports is the
   bounded-queue figure (finite: JSON has no infinity). It reports the writes **one** replica
   misses per second beside the writes one shard's primary committed (summed over replicas,
   the first figure exceeded the committed writes), and the `staleness` SLO fails on any
   missed write with the note `each replica misses 88.89/s of 200/s committed writes
   (44.4%)`. At exactly `rho = 1` both commands keep the queue empty and wait the stochastic
   wait at the 0.99 clamp (200 inserts/s on two 1k/s copies: 250 ms; *corrected by the
   comprehensive 2c review, 5c:* `estimate` used to settle a full queue there, 2.05 s, while
   `run` kept it empty; `FluidQueue::settle` now fills a room only strictly above capacity); the
   docs size examples off that edge. *Ignores:* the lag's own variation, semi-synchronous replication, the
   growing lag itself.
10. **CDN** (plan refinement 4, redesigned in Task 5). Until Task 5 a CDN was a cache whose
    hit ratio a service's `on: miss` step read: the CDN forwarded nothing and its `origin`
    saw load only if some service happened to fetch the misses. Ch. 1's CDN workflow has the
    edge fetch a missing file from the origin itself, so a CDN is now a **read-only cache in
    front of a required `origin`**, the `read_through` shape of a cache whose backend is the
    origin: `1 − h` of each read goes to the origin as the same op, and the response is the
    mixture `{h: own, 1 − h: own + hop}`. `CdnBehaviour` delegates to a `CacheBehaviour`
    (Pattern: Delegation) instead of restating it. `hit_ratio` is fixed, `che` (over the
    CDN's `keys` in objects) or absent (`h = 0`, which runs); an unsized `che` CDN explains as
    an upper bound and does not run. The ops in `write_ops` (default `[write]`) are refused at
    build, naming the op and its sender, since content changes at the origin (review I2: an
    `upload` sent through a CDN was otherwise served from the edge); `explain` lists the ops
    a CDN serves. Also refused: a service's miss step after a CDN, a CDN that is its own
    origin, and an origin that calls back into the CDN. A byte `capacity` was refused by
    `run`/`estimate` in phase 2b (entry 11); since phase 2c it runs, counting megabytes (§19
    entry 2). A CDN
    as another CDN's origin is a tiered CDN, whose second tier's `che` is filtered (entry 3).
    *Ignores:* bytes and bandwidth (until phase 2c: §19 entries 2 and 3), TTL expiry (`ttl` is
    accepted and printed), a cold edge,
    purges, ch. 1's "CDN fallback" (phase 3's faults), the choice of the nearest PoP (the
    client's links say which CDN it uses), the CDN's price.
11. **Byte-derived capacities are not checked** (review I1 of Task 5; superseded in phase 2c by
    §19 entry 2, which counts megabytes and checks them, so no `not checked` line is left). `simsys.defaults`
    converts a byte rate (`capacity: 100GB/s`, a blob store's default 500 MB/s) into
    requests at an assumed 1 MB per object, which no scenario states. The engine refuses to
    run on such a number, and `explain` prints it tagged with where it came from and gives
    its check the load but no percentage and no zone: `(not checked: capacity from 100GB/s at
    1MB/object; byte flows: phase 2c)`. Labelled `(scenario)` with a zone, youtube's
    `cdn_us` read 0.28 % green where its 50 MB streams make it about 14 %, and
    `transcoded_store` 1.58 % green where it is about 79 %. *Ignores:* until phase 2c, every
    byte figure.
12. **ID generator as service sugar** (plan refinement 6; no new behaviour class, no pattern).
    `algorithm` is required; it supplies a `service`'s two numbers: `snowflake` 4.096M/s per
    replica (12 sequence bits: 4096 ids per millisecond) and 10 µs (chosen), `ticket_server`
    1k/s (§6's SQL write rate: an id is a write) and 1 ms, `uuid` 1G/s and 1 µs (nothing to
    coordinate). A stated `capacity` or `latency` wins; `explain` tags the algorithm's
    numbers `(algorithm)`. A ticket server must have `replicas: 1` (one database issues the
    sequence), checked by the loader and, for scenarios built in code, by `Graph::build`.
    *Ignores:* ch. 7's multi-master design, its several ticket servers kept in sync (the
    remedy for the single point of failure), clock skew, and snowflake's 5-bit machine field
    (at most 32 machines per datacenter).
13. **Series and JSON columns** (plan refinement 7, decided). The node series gains
    `demand_per_s,drain_per_s` after `node`, so the conservation law `Σ demand·dt = Σ (drain
    + drops)·dt + backlog` can be checked from the file. What the clients see goes to a second
    file rather than a second section of the first: `run --series-ops` writes
    `t_s,client,op,offered_per_s,answered_per_s,mean_ms` (`timed_out_per_s` before `mean_ms`
    since plan 2c Task 13's review), from an `OpSeriesWriter` that
    observes the same step records as the summary and the node writer (Pattern: Observer).
    `run` checks both paths before truncating either and refuses one file for both. JSON's
    ops gain `answered_per_s`; its nodes `staleness_s`, `replication_dropped_per_s` and
    `committed_writes_per_s` (`null` for nodes that are not databases); each SLO a `note`.
14. **Answered = offered − lost** (review I2 of Task 7). The delivered fraction of §17 entry 8
    took each node's `served / inflow` per step, so a request still waiting at the end of its
    arrival step counted as lost and was never credited (a 3× spike into a deep queue
    reported 583/s answered of 667/s with nothing dropped). A node's own share is now
    `1 − (dropped + rejected) / inflow`, so answered is offered minus what a limiter refused or
    a queue dropped, and a backlog is neutral; in a steady state the two agree, and no pinned
    value moved. Transient runs move: the requests that waited out a surge now count, with
    their wait (the rate-limiter lab's p99 rose from 29.4 ms to 30.5 ms; it was 30.5 ms before
    the stochastic wait read the min(demand, drain) utilisation, §19 entry 2, and is 24.8 ms
    since).
15. **Explain marks load it cannot follow** (Task 7, review I1). In phase 2b the paper load did
    not pass through a blob store's or worker pool's `then`, a message queue's consumers, or a
    fan-out's or frontier's `then`; since phase 2c it follows all of them but the last two
    (§19 entry 6). A node reached only that way prints no percentage and no zone but a mark
    naming each blocking type once, nearest first: `dns  peak offered 0/s vs 100k/s total
    capacity  (load not propagated: frontier is not modelled yet)` (web-crawler). A node that
    also gets load the checks follow keeps its percentage as a **lower bound**, `frontier  peak
    offered 50/s vs 5k/s total capacity  (≥1%; load not propagated: frontier is not modelled
    yet)`, with no zone
    unless it is already red (a red lower bound is red for certain) and no per-copy figures;
    a `not checked` line added `(lower bound; ...)` until phase 2c removed those lines (entry 11). *Why:* "0 %, green" and "(0.05%, green)"
    read as an idle tier when the number is missing. *Ignores:* how much load is missing.

## 19. Phase 2c refinements (2026-10-29)

Building phase 2c (plan `docs/superpowers/plans/2026-10-28-phase2c-bytes-queues-sweep.md`) fixed
the following points. The plan's header listed sixteen refinements; the review rounds of its
tasks corrected several of them, so each entry below is the rule as finally implemented, with the
review that changed it. Where they conflict with an earlier section, this section wins; §7's
`client`, `cdn`, `load_balancer`, `kv_cluster`, `message_queue`, `worker_pool`, `blob_store`,
`search_index` and `external_sink` rows are updated in place, §3.4 points at entry 3, §6 at
entry 2, §11 at entry 15 and §13 says which phase 3 types moved forward. The rules a scenario
author meets are in `docs/scenario-schema.md` (v1.4). Worked numbers are pinned by the theory and
scenario tests or printed by the Release binary.

1. **Request sizes, resolved once.** A client's `sizes: {op: size}` gives each op it sends a size
   in bytes (`200B`, `2KB`, `50MB`, `1.5GB`; decimal, 1 KB = 1000 B, at most 1e15 B, `0B` refused
   as a request size); a step may state `size:`. A request keeps its size while it keeps its op
   name (through balancers and limiters, a cache's or CDN's forwarding, a queue's copies to its
   pools, a same-op step or `then` step); a renamed op without `size:` is unsized. The graph
   resolves one size per (node, op) at build (`Graph::request_bytes`, a size walk beside the key
   walk of §18 entry 1, seeded also with every step that states a size). Two sizes at one
   (node, op) are an error at any node, naming the client or step each came from (review of
   Tasks 1–2, item 6: not `from 'lb' and 'lb'`); an unsized op reaching a byte-sized node or a
   bandwidth link is an error too, checked over every op that arrives (item 1: the two walks
   seeded differently and let some through), with a hint when a pool's same-op step renames other
   jobs. A byte `capacity` is valid only on a `cdn` or `blob_store`, and may be written in bits
   (`40Gbit/s` = 5 GB/s; item 3). A limiter's counter carries no size, so its backend link cannot
   have a bandwidth. *Why static:* every shipped scenario sends one size per (node, op); bytes
   carried in the flow (§3.2) are phase 3. *Ignores:* a size distribution (each op's size is its
   mean), headers and protocol overhead, compression.
2. **Byte-sized nodes count megabytes** (§6's per-MB rows). A node is byte-sized when its capacity
   is a byte rate (any `cdn` or `blob_store` that states one, and a blob store by default: §6's
   500 MB/s). Its unit is **1 MB** (`defaults::byte_unit = 1e6`, replacing 2b's assumed 1 MB per
   object, §18 entry 11): its capacity is MB/s, a request of op `o` costs `request_bytes(o) / 1e6`
   units through the existing op cost (§17 entry 4), and its `latency` is one megabyte's time (a
   CDN `network_read_1mb` = 10 ms; a blob store `disk_read_1mb + network_read_1mb` = 40 ms, chosen
   serial: no pipelining), so Little's law gives `c = MB/s × time per MB` slots (`cdn_us` 100k
   MB/s × 10 ms = 1000; a default blob store 500 × 0.04 = 20). A 100 GB/s CDN saturates at 2k
   streams/s of 50 MB; youtube's 281.6 streams/s at 21:00 are 14.08k MB/s, 14.1 % of `cdn_us`.
   *Corrected by the review of Task 3 and its two fix rounds:*
   - **the queue waits for whole objects** (I1): Erlang C is fed the rate-weighted mean object
     time `s_obj = Σ p_op × MB_op × s_MB` with the same `c`, so `rho` is as in MB/s and the wait
     is a whole object's (at rho 0.9, 50 MB objects at a default store wait 0.28 s, where 1 MB
     units said 11 ms);
   - **a write is answered at its last byte** (I2): an op in the node's `write_ops` (now a key of
     a blob store, default `[write]`; youtube's `original_store` lists `upload`) is answered after
     the wait plus `size × s_MB`, any other op at its first megabyte (the wait plus one MB's time);
     a request under 1 MB takes one megabyte's time either way (an upper bound for small objects);
   - **transfers are deterministic** (second round, I2): a fixed size at a fixed rate has
     per-object SCV 0 (`service_variability` defaults to 0 there), and the Allen–Cunneen factor is
     fed the size mix's SCV, `c_s² = Σ p (1 + cs²) s² / s_obj² − 1` (second round, I1: a
     rate-weighted mean alone let 1000 small reads/s cut the large writes' wait 120×); one size
     waits like M/D/c. Every 300 MB upload therefore takes at least 12 s at 25 MB/s per slot;
   - **the room-drain bound** (first round, C1; renamed in the second, I-2): every queue's
     stochastic wait is `min(k × Wq, queue_limit / capacity)` (1000 MB at 500 MB/s: 2 s), exact
     for a deterministic drain and conservative otherwise (about twice M/M/c/K's mean near
     rho 1); its variance is capped at the room's scale squared over 4, our heuristic. The
     lognormal percentiles ignore the bound (a saturated store's p99 6.88 s against its 4 s,
     pinned as a characterisation test). *Decided by the comprehensive 2c review (5a):* label
     the percentiles, do not truncate them, since only an all-deterministic path has a finite
     maximum; the schema's "How percentiles are estimated, and where they mislead" gives the
     measured cases (two-peaked mixes, hard bounds) and the rule behind a client timeout.
   A byte-sized node's demand, drops and backlog are in MB, its clients' rates in requests.
   *Corrected by the comprehensive 2c review (5b):* the stochastic wait reads the arrivals' load,
   `min(demand, drain) / capacity`, not the busy fraction `drain / capacity`, so a ramp's
   transient wait falls with the backlog (12 → 4 writes/s of 50 MB at a default store: 3.94 s
   at 970 MB down to 2.02 s at 10 MB, where it held 4 s while the room drained; every queue,
   not only a byte-sized one: the rate-limiter lab's p99 24.8 ms, was 30.5 ms untimed).
   *Ignores:* a read's last byte (a download needed whole before use), that a request's megabytes
   arrive together (an M^X batch).
3. **A link's `bandwidth` is a pipe** (§3.4). One server (`c = 1`) in megabytes, draining
   `bandwidth / 1 MB` (1 Gbit/s = 125 MB/s, 8 ms per MB), each request holding it for its whole
   serialisation, deterministic, with a 1000 MB waiting room (chosen: the generic `queue_limit`
   in MB) beyond which it drops; its losses multiply the hop's delivered fraction. 1 MB requests
   at 100/s over 1 Gbit/s: rho 0.8, M/D/1 wait 0.5 × 0.8 / 0.2 × 8 ms = 16 ms. A hop over the pipe
   adds its wait plus the request's answer time on the wire, by entry 2's rule (review of Task 5,
   I-2): a **write** its whole serialisation, any other op its first megabyte
   (`min(size, 1 MB) / bandwidth`): 10 MB at 10/s, a read 160 + 8 = 168 ms, a write 160 + 80 =
   240 ms; a streamed PUT into a blob store pays the wire's and the disk's whole-object times in
   series (an upper bound). Write-ness is decided per (link, op) at build (review of Task 6, I-2):
   an op is a write on a link if a node its body reaches lists it, under its own name or under the
   new name of a step whose `size` equals the incoming request's (reviews of Task 7, I-2, and
   Task 8, I-2: a 1 KB metadata write beside a 10 MB upload is a new request), so a balancer or a
   relaying service in front of the store changes nothing. **Direction** (review of Task 5, I-1):
   the pipe is fed by the demand that crosses the link, each request charged its whole size in
   the direction it is sent; replies add no bytes, so an `inout` link is one pipe and `x->y`,
   `y->x` with a bandwidth are two. Reports get a `links` table, JSON `links`, a series row
   `from->to` and an `explain` check per pipe; a pipe counts in `drop_rate` without `node`.
   youtube's 40 Gbit/s cross-DC origin link carries 2.856k MB/s at 21:00 (57.1 %) and its wait is
   6.66 ms (a 50 MB stream holds the 5 GB/s wire 10 ms: 0.5 × 0.571 / 0.429 × 10 ms), not the
   0.13 ms a 1 MB unit would give (Task 16's measured deviation). *Ignores:* TCP slow start and
   windows, packet loss and retransmission, headers, the true direction of each byte, full
   duplex; the room's drain time (8 s at 1 Gbit/s) caps the wait of requests larger than the
   room (2 GB uploads: 32 s by Pollaczek–Khinchine, 8 s reported).
4. **A message queue is a broker plus its consumers' waiting room** (ch. 1, ch. 10's notification
   queue, ch. 14's transcoding queue; Kafka). The `message_queue` node's own queue is the append
   (§6: 1M msg/s; 100 µs chosen), and a publisher is answered at the append. Every link out of a
   queue must reach a `worker_pool` that `consumes` it, and every consumer group receives every
   message (Kafka's consumer groups); a queue nobody consumes, and a consumer link whose `ops`
   carry nothing the queue receives, are refused (review of Task 6, M-2). What a group has not
   drained waits in its pool's queue, whose room is `retention × the pool's rate` (default 7 days,
   Kafka's `log.retention.hours = 168`; older messages are deleted unconsumed: the pool's drops).
   **The lag** (review of Task 8, I-1: the docs had described the oldest message's age) is how
   long a message published at that step waits until its slowest group reads it, Kafka's
   records-lag over the consume rate: `max(min(k × Wq, retention), backlog / capacity)`, the FIFO
   drain time (review of Task 6, M-7: not Little's law) or the pool's Allen–Cunneen M/M/c wait,
   whichever is larger (our heuristic, the fluid queue's wait). The oldest unread message's age
   trails it by one lag and reaches the same peak that much later. `partitions` is printed, not
   modelled (phase 3: it would cap a group's parallelism). *Ignores:* ordering, delivery
   guarantees and redelivery, a consumer's commit interval, the broker's disk.
5. **A worker pool is M/M/c over consumption** (§7: "capacity = workers / job cost"). `c =
   workers`, service time `cost` (a duration), rate `workers / cost`: youtube's 600 transcoders at
   60 s drain 10 videos/s. 1k msgs/s into 4 workers at 3.333 ms: rho 0.8333, Erlang C 0.6577, Wq
   3.289 ms. `consumes` is required and a pool takes jobs only from that queue (review of Task 6,
   I-1: a pool fed by a service ran as a synchronous M/M/c with a 7-day room); `capacity`,
   `latency`, `replicas`, `advanced.concurrency` and `advanced.queue_limit` are refused. Its
   `then` steps fire for every job it **drains** (Task 7), `factor` requests each, sized by the
   step's `size:` or, same-op, by the job's own size; `on: miss` and `on: failure` are refused. A
   worker is busy for `cost` only: its `then` hops do not hold it. *Ignores:* a worker blocked on
   a slow downstream call (phase 3), autoscaling, poison messages.
6. **Asynchronous completion.** The hops from a queue to its pools and a blob store's events are
   asynchronous (`Graph::async_hops`, as write-back flushes, §18 entry 5): a publisher waits for
   the append alone and an uploader for the store alone; what a pool or a notified node loses
   counts in its own drop rate, not in the client's answered rate, and `explain`'s paper load
   follows these hops (Task 7). *Ignores:* the end-to-end completion time of the asynchronous
   work (upload → playable); the queue lag is what the SLOs check. A request/reply over a queue is
   phase 3.
7. **A blob store's `then` steps are event notifications** (S3 event notifications; ch. 14's
   "transcoding starts when the upload completes"). *Corrected by the review of Task 4 (I1):* they
   fire for every **write** the store serves (an op in its `write_ops`), never for a read, since
   S3 has no event for a GET (1000 views/s of a video start no transcode); a same-op step keeps the
   write's size (Tasks 1–2 review, item 10), a renamed one carries its `size:` or none. `on: miss`,
   `on: failure` and any `mode` are refused (M1). *Ignores:* event batching, delivery delay beyond
   the link's latency, failed notifications; notifying on reads is phase 3.
8. **External sink.** A third party's API (ch. 10's APNs, FCM, SMS and e-mail providers) as a
   queue of §6's `third_party_api` numbers (1k/s chosen: one account's quota; 100 ms; service SCV
   2.0); `failure_rate f` fails that share of what it serves, `dropped += f × served`, `drain =
   (1 − f) × served`, so 500/s at f = 0.02 is answered 490/s with a drop rate of 0.02. Failures
   are drops (the node failing), never rejections. With `f = 1` nothing is answered: JSON prints
   `null` latencies and a latency SLO is `skipped` with `nobody answered c.op: no latency to
   observe` (reviews of Tasks 9–10, minor 8, and of the kv fix round, minor 4). `politeness` is
   printed, not modelled (it needs per-host state: phase 3, with `frontier`). *Ignores:* retries,
   a provider's latency rising with our load.
9. **kv_cluster, steady state** (§7; ch. 6). `nodes` machines of `capacity` each on a ring of
   `vnodes` virtual nodes; every request goes to its key's `replication` N replicas, so a request
   costs N capacity units (one line in `defaults::op_cost`) and the clients' rates stay in
   requests. W = R = min(2, N) by default; `replicas` is refused; `write_ops` names the writes
   (review I-3); `mode: ap|cp` is printed and matters only under partitions (phase 3). *Corrected
   by the review of Tasks 9–10 (C-1, I-1, I-2) and of its fix round (I-2, I-3):*
   - **the ring's hottest machine** carries `k = min(1 + z_M/√(N·vnodes), nodes/N)` × the
     average, z_M the expected maximum of M = `nodes` standard normals (Blom's approximation);
     the consistent-hash balancer's ring uses the same function with N = 1, so the two agree. It
     is a heuristic checked against Monte Carlo rings: within about 1 % from 100 vnodes on, off by
     up to ±16 % at `vnodes: 1` (`docs/study/numerics.md` §8). The old `1 + 1/√vnodes` ignored
     the number of machines and understated the hot spot beyond about 16;
   - **the quorum's latency over the request's real replica set**: a request's N replicas include
     the hot machine with probability `p_hot = N k / nodes`, else are N cool machines; the q-th
     fastest reply (q = R or W) is computed from each machine's own fitted lognormal through the
     survival function, a Poisson-binomial over the two groups integrated on a grid even in ln t
     (`numerics.md` §7), and mixed by the share of the **answered** requests each kind of set
     gives (fix round I-2: a set that loses more weighs less). Rényi's closed form for iid
     exponentials (the k-th of n: `m Σ_{i<k} 1/(n − i)`, 5/6 m for 2 of 3) stays as the teaching
     case and the quadrature's cross-check;
   - **the quorum's losses**: a request is lost when fewer than q of its N replica-requests come
     back, the same Poisson-binomial with each machine's kept share (8 saturated machines dropping
     a ninth lose 3.4 % at q = 2, 0.14 % at q = 1, 29.8 % at q = N, not a ninth at every q).
   The kv-store lab at 16 machines: hottest 0.6094 (k = 1.0834), put p99 2.31 ms. *Ignores:*
   partitions, sloppy quorum and hinted handoff, read repair and anti-entropy, vector clocks,
   the latency of a request that lost a replica-request but met its quorum (timed as if all N
   replied), replies being exactly lognormal and independent, the spread among cool machines.
10. **search_index serves like a service** (§7: "read-mostly capacity"): a `ServiceBehaviour`
    with no steps and the type's numbers, 5k/s and 5 ms (chosen: a prefix lookup in a trie that
    stores each prefix's top k, ch. 13), unless stated, as the id generator is sugar (§18 entry
    12). `build_interval` is printed (`168h`: ch. 13 rebuilds weekly), its load not modelled.
    *Ignores:* index build load, freshness between rebuilds.
11. **A step after a failed step does not run** (2b backlog). In an op's step list, a cache's or a
    pool's `then`, step k is sent `rate × fraction × factor × S_k`, `S_k` the product of the
    delivered fractions `d` of the sequential hops and parallel groups before it (a group delivers
    the product of its branches'; a miss block passes `h + (1 − h) Π d`). 1k/s through `[a, b]`
    with `a` at 0.5 sends `b` 500/s; `[a ∥ b, c]` with 0.5 and 0.8 sends `c` 400/s. The `d` are
    the previous step's, so the thinning is late by one step plus the link's delay (review of
    Task 12, m-1). *Corrected by that review:* `estimate` thins by fractions averaged over its
    settle passes and reaches the unique fixed point (I-1: five calls at 1k/s to a node of
    3686/s with a room of one settle at 10 % dropped, 4095/s offered), while `run` can lock into a
    two-step sawtooth there, which the schema explains and `estimate`'s warning names; the
    parallel steps of an op must be **adjacent** (I-4: a group starts at its first member); a rate
    limiter whose counter check was lost forwards only the checked share (I-3: fails closed).
    *Ignores:* a step that fails after the next one started, partial work of a failed request.
12. **Client timeout** (§7's `client` row). Each step a client op's response is fitted as a
    lognormal (`σ² = ln(1 + v/m²)`, `μ = ln m − σ²/2`); with `timeout: τ` the share `Φ(z)`,
    `z = (ln τ − μ)/σ`, is answered and the rest timed out, and the answered requests' moments are
    the truncated lognormal's (`queueing::lognormal_below`: m = 100 ms, v = 0.01 s², τ = 200 ms →
    89.41 % answered, mean 73.97 ms; `numerics.md` §10 has the formulas and the underflow guard).
    Servers still do a timed-out request's work, nothing retries it, and it is in no `drop_rate`:
    no node failed it (§3.3's zones line). *Corrected by the review of Task 13:* **the C-1 rule**,
    a latency SLO at quantile q fails whatever its `max` when more than 1 − q of the delivered
    requests timed out, observing the timeout with the note `57.3% of delivered requests timed out
    (more than 1%)` (the refit's p99 had passed while 2 % timed out, and adding a timeout could
    turn a FAIL into a PASS); otherwise (s ≤ 1 − q, s the timed-out share) the percentile is the
    **delivered** fit, the untruncated lognormal of every request the path delivered, read at q
    and capped at τ. *Settled by the comprehensive 2c review (C-1),* after two rules that read the
    answered requests' refitted truncated moments, first at q (a 530 ms timeout lowered an M/M/1
    path's p99 from 490 to 404 ms), then at q / (1 − s) (biased and not monotonic in τ: 530, 508.9,
    481.8, 475.5 ms behind 530 ms, 600 ms, 700 ms, 1 s): the engine times out exactly the requests
    the delivered fit puts past τ, so the client's q-quantile is that fit's, which τ does not move.
    The engine keeps those moments (`ResponseSample::delivered_response`, set before
    `give_up_after` cuts `response`), the Summary accumulates them per op weighted by (answered +
    timed out)·dt, and the mean stays the answered requests' mean. The percentile is exact for one
    steady step, never falls as τ grows, and equals the untimed client's when nothing timed out (a
    1 s timeout cutting 9e-7 of the rate-limiter lab's requests now moves no percentile; it moved
    its p99 30.5 → 29.9 ms through the refit). One helper (`percentile_seen_by_client`) serves the
    whole run and every window; `timeout ≤ 0` is refused; `run`'s table gains a `timed out/s`
    column and prints `>1s` where a quantile timed out (I-1); JSON's latencies are `null` when
    nothing was answered in time (I-2); the series `--series-ops` gains `timed_out_per_s`.
    *Ignores:* timeouts at intermediate hops, that a path is not exactly lognormal (the share timed
    out is the fit's tail, not the requests').
13. **SLOs per window: `over: D`.** Any SLO may state `over` (a whole number of steps, at least
    one, at most the run after warmup). The observed span is cut into consecutive windows of D **aligned to the end of
    warmup**; a step belongs to the window its start falls in; a last, shorter window is still
    judged; each window is measured exactly as a whole run (the C-1 rule included); the **worst**
    window decides (a fail before a pass, then the higher observed, the earlier on a tie); every
    window is judged, except that a latency SLO skips a window in which its op was not offered
    (a node draining a queue after its clients stopped is still busy: review of the Task 12/13 fix
    rounds and Task 14, I-1), and an SLO all of whose windows are skipped is `skipped`. The table
    names the window, `(worst 1h from 13:02)` (`HH:MM:SS` off a whole minute; a short last window
    by the length it had); JSON gives `window_start_s`. `estimate` measures no window: `(at 00:00
    only: run checks each 1h)`, no `window_start_s`. A 2k/s service at rho 0.5 for an hour with a 1.8× spike for 10 minutes:
    the whole hour's utilization 0.5667 passes `max: 0.8`, `over: 10m` observes 0.9 and fails,
    `over: 15m` 0.7667. url-shortener ships `{metric: utilization, node: url_db, max: 0.9, over:
    1h}`: 0.6433 in 13:02–14:02, where the day's mean is 0.4306. A zone SLO is not added: a
    windowed utilization says the same in numbers. *Ignores:* sliding windows (a burst straddling
    two fixed windows is split).
14. **`queue_lag` SLO.** `node` optional, a `message_queue` (or a `frontier`, accepted and skipped
    until phase 3); the observed value is the queue's **peak** lag after warmup, read "every
    message published after warmup was read within `max`". JSON nodes gain `peak_queue_lag_s`, the
    series a trailing `queue_lag_s`, the table a `peak_lag` column (`<1us` below a microsecond).
    youtube: `PASS queue_lag transcode_q - 573ms 30m` at 600 workers; at 400, 2.66 h by midnight.
15. **`simsys sweep`** (§11). `simsys sweep <file> --param <dot.path>=<from>..<to>:<n> --metric
    <m>... [--jobs N] [--format table|csv]`: `n` points (2 to 10,000; review of Task 15, I-1),
    evenly spaced, both ends included, both ends numbers in the same unit text (I-2: `1ms...5ms`
    is refused, not read as 0.5 ms), whole at every point for a key the loader reads as a whole
    number. The key is a dot path through maps only and must exist; each point's value replaces the
    file's text before the loader's checks (`config::load(path, overrides)`), every point is loaded
    and built before any runs, and a YAML anchor's value moves with its aliases. Metrics:
    `<client>.<op>.mean`, `<node>.mean_rho`, `<queue>.lag`, and since the comprehensive 2c review
    `<client>.<op>.p50|p95|p99`, `<node>.peak_rho` and `<node>.drop_rate`, the numbers `run
    --report json` prints, formatted as the report formats them (I-3: `1.61ms`, `<1us`, `-` for
    nothing answered, `>1s` for a percentile that is the timeout). A point loads and runs exactly
    as `simsys run` does (`simsys.session`'s `load_graph` and `run_summary`, which the CLI calls
    too; review I-4).
    Points run on `std::jthread` workers pulling indices from one atomic counter, each a
    `Simulation` of its own, so the output is byte-identical for any `--jobs` (1 to 1024, default
    the hardware threads; `docs/study/threads.md`). Exit 0 when every point ran (verdicts are not
    judged), 1 on any error. *Narrower than the plan's header:* the plan listed `reject_rate` as a
    metric too, a column per SLO with its verdict and the point's exit code, and `--report`; the
    implementation has the seven metrics above, no SLO or exit columns (Phase 3 backlog) and
    `--format`. *Ignores:* grids over two keys, geometric spacing, list values,
    replications per point (phase 3).
16. **Step size rule of thumb** (2b backlog; docs only). A token bucket of `B` tokens under an
    offered rate `λ` above its limit `L` empties in `B / (λ − L)`; a longer step passes the whole
    burst in its first step and the bucket's shape vanishes (a 2,000-token burst at `step: 1s`
    does). Rule: `step ≤ B / (λ_peak − L)`, and for queues `step ≤` a tenth of the shortest
    spike. In the schema's `step` row and `docs/study/numerics.md` §6, "Choosing the step", with the
    rate-limiter lab measured at four steps.

## 20. Phase 3: the rules as implemented (2026-10-30)

Phase 3 (design `docs/superpowers/specs/2026-10-30-phase3-faults-gateways-design.md`, "the phase 3
design" below; plan `docs/superpowers/plans/2026-10-30-phase3-faults-gateways.md`) ran as four
parts: 3a split the largest units, 3b added faults, failure steps, retries and replications, 3c
the last four node types with their scenarios, 3d the docs. Every task ended in an adversarial
review recorded in the plan ("Review of Task N"), and most reviews changed a rule. Each entry below
is the rule as finally built, what changed from the phase 3 design's text, and which review changed
it. Where an entry conflicts with an earlier section or with the phase 3 design, this section wins;
§7's rows for the four new types, `client`, `kv_cluster`, `message_queue` and `external_sink`, the
faults line under §7's table, §9's rows 9.3 and 9.4 and §13's phase 3 row point here. The rules a
scenario author meets are in `docs/scenario-schema.md` (v1.5), including its list of the defaults
chosen, not from the book. Worked numbers are pinned by the theory and scenario tests or printed by
the Release binary.

1. **Layout first (Tasks 1–4).** `explain` moved into its own implementation unit of
   `simsys.report`, the loader into one implementation unit per concern, `Graph`'s definitions
   into `graph.cpp`, and the theory tests into nine files with a test module
   (`simsys.test_fixtures`); every scenario's output stayed byte-identical. Changed by GCC 14.2,
   not by a review of the design: the loader's shared helpers are a header included in the module
   purview (`src/config/detail.hpp`) because GCC refuses both partition designs; the header says
   that this departs from the one-definition rule for entities attached to a named module, and
   names the conforming design for a later GCC (`docs/study/modules.md`); a body edit rebuilds one unit only with CMake 3.31 (3.28 still
   rebuilds every importer, each reading a smaller BMI). The 3a review also found CLAUDE.md §3's
   seventh GCC defect, and phase 3 met three more (the eighth to tenth).
2. **The `faults` list (Task 5; phase 3 design §4.1).** Six kinds as designed; every key a kind
   does not read is refused, every message names the entry (`faults[2]: cache_flush takes no
   for`). Changed: `partition` is refused with `partition is not modelled in phase 3 (see the
   backlog)`, not the design's wording, and kv-store's partition fault was dropped from the file;
   the labs' `node_down` gained `replicas: 1`, which they had meant; overlapping `traffic_spike`
   factors on one client are capped at a product of 1e6, as a rate curve's spikes are (review item
   1); a `kind` written as a list says `kind must be a scalar` and a factor written as text
   `factor must be a number` (item 2); `at` is one number and one unit (`90m`, not `1h30m`), and a
   clock time names the first day only; a fault wholly inside `warmup` is accepted and reported by
   its block only. `replicas` on a sql_db counts the copies of each shard: `primary` is the writer,
   a count below the copies takes read replicas (item 4).
3. **Conditions on capacity, latency and rate (Task 6; design §4.2).** A schedule hands every node a
   per-step `engine::Condition`; a run without faults is bit-identical to phase 2. Corrected by the
   review: a request waiting at a node that is down waits a **finite** time, `back_in_s` (the end
   of the zero-capacity faults on it) plus the backlog ahead of it at the restored capacity, and a
   step that delivered nothing adds no latency and no "unavailable" (the design's capacity 0 made
   the wait infinite and a 30 s outage made news-feed's op unavailable for the day, item 1); a
   sql_db's primaries and read replicas are derated apart (`primary` stops the writes, `replicas:
   r` leaves the primary whole, item 2); a kv_cluster's down machines are handled by its quorums,
   a request whose key had j replicas there being answered by its quorum among the N − j live ones
   or lost (item 5, the design's last paragraph of §4.2 made concrete); `curve` applies a
   `traffic_spike` as it applies a curve spike. Faults on one node compose: factors multiply,
   extra latencies add, overlapping outages end with the last of them.
4. **Cache flush and `dc_down` (Task 7; design §4.3).** Corrected by the review: the warm-up counts
   the reads the cache is **actually sent** after the flush (n grows by its inflow each step), not
   `rate × t` frozen at the flush, so a cache flushed in the quiet hours stays cold until reads come
   and one flushed on a rising curve fills faster (items 1, 2). A Che cache's hit ratio at n reads
   is that of a cache of min(`keys`, D(n)) entries, D(n) the distinct keys among them, back at the
   steady ratio exactly when D(n) reaches `keys`; a fixed ratio warms linearly over `keys` reads,
   kept as the simple case and documented as optimistic (item 4); a later flush of the same cache
   ends the earlier one's window (item 5). `dc_down` is a `node_down` of every node in the
   datacenter (clients excepted) and capacity 0 on every link with one end inside it, either way;
   its caches come back warm (item 6). A `cache_flush` needs a cache with `keys` (or a Che ratio)
   and a hit ratio below 1, else the engine refuses it.
5. **The fault block (Task 8; design §4.4).** The three lines per fault stand, but the baseline is
   not "the step before the fault": a scenario with faults is run **twice in lockstep**, the run
   and its fault-free twin (same seeds), and every figure compares the run with the twin's same
   step. Recovery is the step after the LAST step, from the fault's clearing on, at which some
   client op's p99 or some node's rho is outside 1 % of the twin's (relative to the twin's value):
   the block is written at the end of the run, so a step that crosses back within 1 % and leaves it
   again does not count (corrected by the phase 3 review: the first step within 1 % had a gateway
   recovered 0 s after its fault), and a run that ends outside is `not recovered`; the steps are
   judged up to the scenario's next fault, where a record that is back is settled (the twin has no
   fault at all, so the later fault's steps are its own). Why the twin (review items
   2–4): a fault at the run's first step has no step before it, a fault inside another has a
   broken one, and on a diurnal curve the step before is never "back" (a 5-minute outage at 15:00
   reported `not recovered`). The twin doubles a faulted run's cost and leaves a fault-free run
   untouched. `lost` is the run's drops minus the twin's from `at` to recovery, in the node's own
   unit (requests, or MB at a byte-sized node), a `dc_down`'s cut links apart; `(room N dropped the
   rest)` only when the room filled and the node dropped; a flush reports its extra misses to the
   origin; "flipped" is the SLO table's verdict differing from the twin's, and `over its max for`
   the time the run broke an SLO where the twin held it (items 1, 5–7); overlapping faults print
   `(overlaps fault …)` and split nothing. The record types moved to the partitions
   `simsys.engine:records` and `simsys.engine:fault_log` (item 8).
6. **`on: failure` steps (Task 9; design §5).** As designed (a failure step carries `1 − d` of the
   step it reports on, thinned by the steps before it, not by that one), plus three rules the
   review forced: after a cache lookup with `on: miss` steps the failure step reports on the
   lookup and its miss block together, `d = d_c × ((1 − f) + f · Π d_miss)` (item 1); a failure
   step that closes a call cycle is refused, since it is an unbounded retry (`... closes a call
   cycle (an unbounded retry: use the client's retries)`, item 2); a failure step must be
   sequential, may not stand in a `then` list, and its latency is not in the op's response (these
   decisions got their tests after item 4). With a client's retries it fires per failed
   **attempt**.
7. **Client retries (Task 10; design §6.1).** Feedback demand as designed: 1 + q + … + q^n attempts
   per request, 1 − q^(n+1) answered (1.75 attempts and 875/s of 1000/s at q = 0.5, n = 2),
   `offered/s` the curve and `attempts/s` beside it, `estimate` iterating to the fixed point with
   phase 2c's damping and warning when it does not settle. Corrected by the review: timeouts are
   counted **per attempt** for the percentiles (JSON `timed_out_attempts_per_s`; a p99 SLO that
   fails without retries no longer passes with them, item 1); the backoff is `retry_after` / `step`
   rounded **up** in integer time, at least one step, and `explain` prints the steps when they
   differ (item 5); the fault block tells attempts from requests (`lost 8.907k attempts (2.939k
   requests lost for good)`, `retries added 5.968k attempts`, item 2). Documented as ignored: a
   retried request's latency is its last attempt's, without the failed attempts and backoffs
   (item 3; an `error_latency` is in the phase 4 backlog).
8. **Pool retries and dead letters (Task 11; design §6.2).** Corrected by the review: a pool
   retries only what **reached** a sink and failed there, `f = Σ S × (1 − d)` over its sink calls,
   not the whole `then` list's losses (item 1), with `d` the sink's whole delivered share (an
   overflowing sink's sheds are retried like its errors); re-published jobs load the queue and
   the failing pool only, not the other consumer groups (item 4); a sink's `drop_rate` counts its
   drops caller by caller, leaving out the share its retrying pools send (item 3); dead letters
   are judged by a new SLO metric, `dead_letter_rate` (dead letters / jobs published), and swept
   as `<pool>.dead_lettered` (item 5); `explain` offers the pool and its sinks `A(f, n) = Σ f^k`
   times the jobs (item 8). `retries` needs a `then` step to an external sink; a retry replays the
   whole `then` list (documented).
9. **Replications (Task 12; design §7).** `--replications N` (1 to 1000) runs seeds `seed` to
   `seed + N − 1` of the rate noise on the sweep's thread pool, byte-identical for any `--jobs`;
   every cell is the median and the 10–90 % band, nearest rank. Changed by the review (item 1):
   each SLO is judged by **its own median run** (the run whose observed value for it is the
   median), its row names that run's seed and the JSON gives it as `median_seed`; the exit code is
   2 when some SLO's median run fails; the report around the table (summary line, fault blocks,
   JSON) is the first SLO's median run's, and the table's second line names its seed. A scenario
   without noise (or with `sigma: 0`) runs once and says so; `--series` is refused with more than
   one replication.
10. **`service_discovery` (Task 13; design §8.1).** A service at the type's 50k/s and the book's
    500 µs round trip (the type had 10k/s before). Corrected by the review: the lookup rule is
    **per step**, not per op: a step is held on a dead replica when a `lookup` to a discovery
    comes before it in the same step list (item 2); the window is `min(failover, for)` and the
    block prints that one (item 3). No failover behind a balancer, for a `capacity` fault, a
    `dc_down`, or a sql_db's or kv_cluster's partial `node_down`. Documented as ignored: with every
    replica down the requests after the window wait in the node's room, where a registry would
    fail them; a lookup in a `then` list never fails over.
11. **`ws_gateway`'s level (Task 14; design §8.2).** Connections are a level, `C += (connects −
    C/session) dt`, from a client op named `connect` or a fixed `connections` (exactly one: the
    engine refuses neither and both); heartbeats are `C / heartbeat` on the gateway's `heartbeat`
    op; the cap is `max_connections × replicas` and refused connects are rejections. Changed:
    `deliver` with steps is refused, as the design said it is terminal (item 1); the type keeps the
    50k/s and 100 µs it had since phase 1 instead of the plan's 10k/s and 0.5 ms; the reports gain
    the `connections` column, JSON's `connections` (end) and `connections_peak`, and the sweep
    metric `<gateway>.connections` (end of run, item 8). §8.2's fan-out is **narrowed** (item 2,
    decided in Task 15): `fanout` multiplies only a message op's steps that target a
    `message_queue` (a copy per recipient in the sync queues), not every step, so a message is
    stored once and gets one id.
12. **The reconnect storm (Task 15; design §8.2's last paragraph).** A `node_down` of r of R
    gateway replicas drops `C × r / R` connections at its first step (a `dc_down` of its
    datacenter drops all of them; a `capacity` fault none). Corrected by the review: each fault
    keeps its own waiting users and releases them at its own end (item 2); waiting users end their
    sessions at rate 1 / session (exactly, `e^(−dt/session)` per step), so `D0 × e^(−for/session)`
    come back (item 6); connects the clients send while every replica is down wait with them; the
    storm sends the waiting users evenly over `reconnect_within`, and a storm connect the gateway
    refuses or drops is retried after an exponential wait of mean `reconnect_within` (item 5); a
    storm stops only when every replica goes down again (narrower than the decision: a partial
    fault keeps it going to the survivors, item 1); the cap during a fault is the survivors',
    `max_connections × R × (1 − share down)`, the share down being the fraction of the R replicas
    the faults have taken down (`down_share_` in the code; item 3), and the users' own connects
    that this smaller cap refuses are not retried (ignored, and documented so); two faults that
    strike one gateway in the same step split that step's drop in scenario order (pinned); the
    block's peak is the offered utilisation (demand / total capacity, uncapped) read over
    `[end, end + reconnect_within + one step]` and named `peak_offered_rho` in JSON (items 4, 7).
    Corrected by the phase 3 review (decisions 1 and 8): a user waiting for their turn in the
    window goes on ending the session, so the one whose turn is `u` into it is there with
    probability `e^(−u/session)` (the storm's connects `D / W × e^(−u/session)`, a step getting
    their integral over it), and `D × session / W × (1 − e^(−W/session))` come back; before, the
    level overshot the twin's by about `W / 2 / session` of the storm. The block counts the users
    who come back (the storm's connects that are the fault's users, each fault its share of a
    storm that carries several) instead of the connections dropped; JSON's key keeps its name,
    `reconnect_storm_connections`. chat-system's 21:30 minute: 1.551M users storm back over 30 s,
    the tier peaks at 90.5 % at 21:31:29.9, and the run is back within 1 % of its twin at
    21:31:30, as the storm ends (before the review: 1.617M connections, 93.9 %, 21:38:58.7, the
    overshoot's decay).
13. **google-drive (Task 16).** Its long polls are a gateway with a fixed `connections: 2M` (chosen:
    a fifth of the book's 10M daily users online at once) and `heartbeat: 60s` read as the poll's
    timeout, one re-poll a minute per device (33.33k/s);
    `backup_workers` 3, with the 2-worker day kept as the README's exercise. Corrected by the review:
    the backup pool is a modelling addition (an archive copy of each upload), not the book's offline
    backup queue, and the README and YAML say so (item 1).
14. **`notification_fanout` (Task 17; design §8.3).** As designed: a service at 20k/s and 1 ms whose
    `then` steps fire for `1 − dedup` of what it served, the rest counted as `deduplicated_per_s`
    (neither drop nor rejection); `channels` and the steps that publish to a queue must name the
    same queues both ways. The review added the sweep metric `<fanout>.deduplicated` and
    documented that the publishes are sequential (item 3). notification-system was resized to pass
    its three SLOs with pool retries.
15. **`frontier` (Task 18; design §8.4).** Corrected by the review: politeness is a **drain bound in
    front of the pool** (a token bucket at `hosts × politeness`), not a cut of the pool's rate and
    workers, so the pool keeps its real `c` and its utilisation is `drain × cost / workers` (0.25
    at a 1000/s cap with 2000 workers at 500 ms, where the first version read 1, item 1); a
    frontier's pool room is **unbounded** (nothing is deleted unconsumed) and `estimate` reports
    an unbounded lag rather than dropping URLs (item 2); `explain` checks the frontier against
    `min(prioritiser, cap)` (item 3); a second consumer pool is refused (item 5); a sink's
    `politeness` over a frontier without `hosts` is refused (item 6); a sink's own `politeness`
    is the same drain cap in front of it (a sink capped at 1000/s with 5k/s of its own reads rho
    0.2, pinned); the machinery that refused a type without a model was deleted, since none is
    left (item 7). The crawler was resized so that the frontier's lag is the lesson (`internet`
    1k → 1M/s, the prioritiser 5k → 10k/s, the parser 20 → 25 replicas, `content_store`
    400 MB/s → 1 GB/s; with only `internet` and `content_store` resized the prioritiser dropped
    26 % and the parser sat at 95 %). web-crawler exits 2 on purpose:
    its loop gain is above 1, the downloaders sit at utilisation 1 and the frontier's lag reaches
    14.4 h against a 30-minute SLO.
16. **What phase 3 leaves.** The phase 4 backlog (the plan's last section): kv_cluster partitions
    (`mode: ap|cp`, sloppy quorum, hinted handoff) and the `partition` fault kind; random faults
    (`mtbf`/`mttr`, chaos mode, §9.4); "any numeric parameter may be a distribution" of §9.3 (a
    replication varies the rate noise only); queue partitions capping a group's parallelism; a
    worker blocked on its `then` hops; `error_latency`; one series file per seed; sweep SLO columns,
    sweeps over seeds or two keys.
