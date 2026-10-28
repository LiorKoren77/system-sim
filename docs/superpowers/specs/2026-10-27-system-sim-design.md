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

```
inflow     = Σ incoming flows                      (per op)
demand     = Σ_op inflow_op × cost_op              (capacity units/s; cost from op table)
drain      = min(capacity, demand + backlog/dt)    (serve what it can)
backlog   += (demand − drain) × dt                 (grows past capacity, shrinks below)
overflow   = max(0, backlog − queue_limit) / dt;  backlog = min(backlog, queue_limit)
rho        = min(1, demand / capacity)             (utilization)
wait_mean  = backlog / capacity                    (fluid backlog, Little's law)
           + ErlangC(c, rho) × service_time / (c × (1 − rho))   (stochastic queueing, §9.1)
outflow    = drain, routed to downstream edges by the node's behaviour
```

The analytic term uses `min(rho, 0.99)`: once a node is saturated the fluid backlog
carries the wait, and the stochastic term must not double count it.

Zones: green below `yellow` (default 0.70 utilization), yellow up to `red` (0.90), red
above; drops are `overflow` plus anything the behaviour rejects (rate limiter, timeouts).

### 3.4 Links

A link adds its latency to the flow's accumulated mean. If the link latency is shorter
than `dt` that is all it does; if it is longer (cross-datacenter links at 150 ms with
`dt` = 100 ms) the flow is also delayed through a ring-buffer delay line of
`ceil(latency / dt)` steps. Bandwidth, when given, caps bytes/s and queues the excess like a node.

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
Identical input gives byte-identical output. Replications and sweeps run in parallel
across seeds.

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

## 7. Node catalogue (as flow transfer functions)

| Node | Transfer function and knobs | Chapters |
|---|---|---|
| `client` | emits the workload curve per op; receives replies, fits percentiles; `timeout` turns the slow fraction into a retry feedback flow (phase 3) | all |
| `cdn` | reads split by hit ratio (TTL + key skew model); misses go to origin; per-DC instance | 14, 15 |
| `load_balancer` | splits inflow across replicas: equal (`round_robin`), by spare capacity (`least_connections`), by weights, `consistent_hash` with configurable imbalance | all |
| `rate_limiter` | `token_bucket` (bucket level is fluid state, bursts pass until it empties), `leaky_bucket`, `fixed_window`, `sliding_window`; rejected flow counted as drops | 4, 8, 10 |
| `service` | per-op cost; downstream steps sequential (latencies add) or parallel (max); `replicas: N` | all |
| `cache` | hit ratio from `fixed: h` or `che` (LRU under Zipf, from `keys` and skew); strategy `cache_aside` / `read_through` / `write_through` / `write_around` / `write_back` (buffered writes flushed at a rate) | 8, 11–14 |
| `sql_db` | writes to primary, reads split over replicas, replication lag → staleness metric, connection pool as concurrency, sharding divides load | 8, 10–12, 15 |
| `kv_cluster` | hash ring with virtual nodes → load imbalance factor; replication N amplifies writes; quorum W/R → latency is the W-th (R-th) order statistic of replica latencies; partition → unavailable fraction (CP) or stale fraction (AP) | 6, 11–13 |
| `message_queue` | backlog level, consumer drain rate, lag = backlog / drain, partitions bound parallelism | 10–12, 14, 15 |
| `worker_pool` | capacity = workers / job cost; DAG stages chain | 9, 10, 14, 15 |
| `blob_store` | MB/s capacity; flows in bytes; multipart = parallel branches | 14, 15 |
| `search_index` | read-mostly capacity; build-vs-serve split | 13 |
| `ws_gateway` | connections as a level (connect − disconnect rates); fan-out multiplies message flow by session size | 12 |
| `service_discovery` | adds lookup latency; failover time on `node_down` | 6, 12 |
| `id_generator` | capacity in IDs/s; `snowflake` scales with workers, `ticket_server` is a single hot node | 7, 8, 12 |
| `external_sink` | latency distribution + failure fraction + per-host politeness rate cap | 9, 10 |
| `notification_fanout` | per-channel split, dedup fraction, retry feedback with DLQ | 10 |
| `frontier` | URL backlog with politeness rate per host | 9 |

Consistent hashing, quorum, vector clocks, gossip and leader election appear as their
**capacity and latency consequences** inside `kv_cluster`, `load_balancer` and
`service_discovery`. Mechanisms are explained in the scenario READMEs, not simulated.

Faults (`faults:` list): `node_down`, `dc_down`, `partition`, `latency_spike`,
`traffic_spike`, `cache_flush`, each with `at:` and `for:`; random variants in phase 3.

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
| 9.2 Workload noise | Ornstein–Uhlenbeck multiplier on the rate curve (`sigma`, `correlation`); Poisson random spikes with random factor and duration; one `seed` | 2 |
| 9.3 Replications | any numeric parameter may be a distribution; `--replications N` runs seeds in parallel and reports median and 90 % bands; SLO result as "passes in k of N" | 3 |
| 9.4 Random faults | `mtbf` / `mttr` per node, random latency spikes and partitions; chaos mode | 3 |

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
| `std::jthread`, `stop_token`, `latch` | replications and sweeps |
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
simsys estimate scenario.yaml [--at 12:00]                  # steady state, instant
simsys run      scenario.yaml [--seed 7] [--duration 24h] [--report table|json|csv] [--series out.csv]
simsys run      scenario.yaml --replications 50 [--jobs 8]  # phase 3
simsys sweep    scenario.yaml --param nodes.users.rate.base=1k/s..100k/s:10 --metric users.p99
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
- CI: GCC 14 and Clang 18, `-Wall -Wextra -Werror`, ASan/UBSan job.

## 13. Phases and effort (assisted; unassisted in parentheses)

| Phase | Scope | Effort |
|---|---|---|
| 0 | Toolchain spike (GCC 14, Clang 18, CMake named modules), all 12 scenarios drafted by hand in YAML, schema adjusted until they fit | 1 week |
| 1 | Core: units, flows, step loop, links with delay lines, analytic queueing (9.1), config loader, defaults table, metrics, `validate` / `explain` / `estimate` / `run`, theory and conservation tests | 1.5 weeks (3) |
| 2 | Basic node pack (client, LB, rate limiter, service, cache with Che model, SQL, CDN, blob, queue, workers, external sink), workload curves and noise (9.2), `sweep` | 2 weeks (4) |
| 3 | `kv_cluster`, `service_discovery`, `ws_gateway`, faults, retries feedback, replications (9.3) and random faults (9.4) | 1.5 weeks (3) |
| 4 | 8 book systems + 4 labs, broken/fixed variants, SLOs, READMEs explaining the mechanisms the model abstracts | 1 week (2) |
| 5 | Study notes, docs, series format freeze, CI, performance check | 1 week (2) |
| | **Total CLI v1** | **≈ 8 weeks (≈ 15)** |
| later | GUI reading the series file | 2 months |
| later | Discrete-event engine behind the same YAML (§15) | 6–8 weeks |

## 14. Repository layout

```
CMakeLists.txt  cmake/  defaults/latency_numbers.yaml
src/{core,model,policies,nodes,config,cli}/
scenarios/{book,labs,tutorial}/
tests/{unit,theory,scenario}/
docs/{approaches.md, study/, superpowers/specs/}
```

## 15. Deferred: discrete-event engine

Kept out of v1 on purpose. The YAML, node catalogue, defaults and CLI are engine-neutral;
a request-level engine would add `handle(Message&)` next to `route(const Flow&)` on each
behaviour and reuse everything else. It becomes worthwhile when the lesson is a mechanism
rather than a consequence: retry storms from stochastic bunching, vector-clock conflict
resolution, leader election timing. See `docs/approaches.md`.
