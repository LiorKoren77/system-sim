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
rho'       = min(rho, 0.99)
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
above; drops are `overflow` plus anything the behaviour rejects (rate limiter, timeouts).

### 3.4 Links

A link adds its latency to the flow's accumulated mean. If the link latency is shorter
than `dt` that is all it does; if it is longer (cross-datacenter links at 150 ms with
`dt` = 100 ms) the flow is also delayed through a ring-buffer delay line of
`ceil(latency / dt)` steps (revised in phase 1 to `round(one_way / dt)`, see §16). Bandwidth, when given, caps bytes/s and queues the excess like a node.

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
| `cdn` | (revised, phase 2b refinement 4) a read-only cache in front of its `origin`: the hit share `h` (`fixed` or `che` from `keys` and the clients' key skew; TTL not modelled) is answered at the edge, the misses `1 − h` go to the origin as the same op (the `read_through` shape of a cache), and writes are refused. Not the earlier draft's "a service's `on: miss` step reads the CDN's hit ratio": a miss step after a CDN is refused, since the CDN fetches its own misses. A byte capacity waits for phase 2c; per-DC instance | 1, 14, 15 |
| `load_balancer` | (revised, see §17) forwards every op over its links, split by link `weight`; the policy sets how the replicas *behind* it queue: `round_robin` / `weighted` independent copies, `least_connections` one pooled M/M/(c·N) queue, `consistent_hash` the hottest copy at `1 + 1/sqrt(vnodes)` × the average, queued apart from the cool ones | all |
| `rate_limiter` | (revised, see §17) `token_bucket` (bucket level is fluid state, bursts pass until it empties), `leaky_bucket` (the node's own queue as one deterministic leak slot), `fixed_window`, `sliding_log`, `sliding_counter`, each stepped once per step; rejected flow is its own channel (`reject_rate`), never forwarded and not a drop; an optional counter `backend` sees every checked request | 4, 8, 10 |
| `service` | per-op cost; downstream steps sequential (latencies add) or parallel (max); `replicas: N` | all |
| `cache` | (revised, see §17) hit ratio from `fixed: h` (phase 2a) or `che` (LRU under Zipf, from `keys` and skew; phase 2b); `write_ops` name the writes, every other op is a read; strategy `cache_aside` (misses fetched by the service's `on: miss` step) / `read_through` / `write_through` / `write_around` (phase 2a) / `write_back` (buffered writes flushed at a rate; phase 2b) | 8, 11–14 |
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
| 9.2 Workload noise | (revised, see §17) Ornstein–Uhlenbeck multiplier `1 + x`, floored at 0, on the rate curve (`sigma`, `correlation`), discretised exactly; one `std::mt19937_64` per client, seeded from the one `seed` and the client's index (phase 2a). Poisson random spikes with random factor and duration: deferred, scripted `spikes` stand in for them | 2a (spikes later) |
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
simsys estimate scenario.yaml [--at 12:00]                  # steady state, instant; --at: phase 2+
simsys run      scenario.yaml [--seed 7] [--duration 24h] [--report table|json|csv] [--series out.csv]
                                                            # --report csv: phase 2+
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
- CI: GCC 14 debug and release builds with `-Wall -Wextra -Werror`, and a clang-format 18
  check (Clang 18 cannot build the project, see `CLAUDE.md` §3). ASan/UBSan job: phase 5.

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
   `1 + 1/sqrt(vnodes)` × the average, a chosen heuristic that understates the hottest of
   many copies).
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
