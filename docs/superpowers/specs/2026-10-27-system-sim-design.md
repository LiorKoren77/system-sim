# System Design Simulator — Design Spec

Date: 2026-10-27 · Status: draft for review · Licence: MIT · Repo: github.com/LiorKoren77/system-sim

## 1. Purpose

A deterministic discrete-event simulator in C++23 where a distributed system is a graph
of configurable, capacity-limited nodes. A YAML file declares nodes, their capacities and
latencies, the links between them (with their own latencies) and the datacenters they live
in. The engine generates traffic from client nodes, moves requests hop by hop in simulated
time, and every node reports utilization, queue depth, latency percentiles and drops, with
yellow and red zones. Students run the eight systems from *System Design Interview* vol. 1
plus four standalone labs (chapters 4–7), break them, and fix them.

Two goals of equal weight:

1. **Teaching system design**: make saturation, queueing, caching, replication, quorum and
   partitions visible and reproducible.
2. **Studying and teaching modern C++**: the code base deliberately exercises C++20/23
   features (see §9) and each one gets a short study note in `docs/study/`.

Out of scope for v1: GUI (a later project that replays the JSONL trace), packet-level
networking, real wall-clock execution.

## 2. Decisions taken

| # | Decision | Choice |
|---|---|---|
| 1 | Modelling approach | Request-level DES with a `batch` factor; analytical model as `estimate` mode and test oracle. See `docs/approaches.md`. |
| 2 | Config language | YAML only. |
| 3 | Student-facing knobs | Simple keys with defaults; queueing details under `advanced:`. |
| 4 | Latency model | Every node has a service latency and every edge has a link latency, both configurable; defaults come from the book's back-of-the-envelope table (§6). |
| 5 | Book coverage | 8 systems + standalone labs for rate limiter, consistent hashing, KV store, unique ID generator. |
| 6 | Toolchain | GCC 14 primary, Clang 18 secondary (clang-tidy/format, portability). C++23, named modules. |
| 7 | Licence / hosting | MIT, public GitHub repo under LiorKoren77 from day one. |

## 3. Architecture

```
scenario.yaml ─parse/validate─► Config AST ─build via registry─► Topology graph
                                                                        │
            ┌───────────────────────────────────────────────────────────▼──────┐
            │ Engine: event queue ordered by (time, seq), SimClock, seeded RNG │
            │ loop: pop → node.handle(msg) → schedule follow-up events         │
            └──────────────┬─────────────────────────────────┬─────────────────┘
                     samples│                                 │events
            ┌──────────────▼──────────┐          ┌───────────▼──────────┐
            │ Metrics: histograms,    │          │ Trace writer (JSONL) │
            │ time series per node    │          │ contract for the GUI │
            └──────────────┬──────────┘          └──────────────────────┘
            ┌──────────────▼──────────┐
            │ Reporters: table / csv  │
            │ / json, SLO pass-fail   │
            └─────────────────────────┘
```

Module layering (named C++ modules, one per directory):

```
simsys.core      time, event, engine, rng, metrics, trace
simsys.model     message, node, link, capacity, datacenter, topology
simsys.policies  cache/, balancing/, ratelimit/, consistency/ (ring, quorum, vclock, election)
simsys.nodes     one unit per node type
simsys.config    schema, loader, validation, defaults table
simsys.analytic  fluid model (estimate mode, test oracle)
simsys.cli       commands and reporters (executable)
```

Dependencies flow downward only: cli → config/analytic/nodes → policies → model → core.

## 4. Core types

- `SimTime`: strong wrapper over `std::chrono::duration<std::int64_t, std::nano>`. No raw doubles for time.
- `Message`: `id`, `trace_id`, `op` (string-interned), `size_bytes`, `key` (uint64, for cache and ring routing), `origin`, `created_at`, `path` (hops), `deadline`, `batch` (real requests this event represents).
- `Event`: `{at, seq, target, kind, Message}`; strictly ordered by `(at, seq)`.
- `Link`: `from`, `to`, `direction` (`in` | `out` | `inout`), `ops` (allowed op names), `latency`, optional `bandwidth`, optional `weight`. Latency default: intra-DC or cross-DC value from the defaults table, overridable per link.
- `DataCenter`: name, nodes, pairwise latency matrix (defaults: intra 0.5 ms, cross 150 ms). Can be partitioned or taken down by fault injection.
- `Capacity`: `service_rate` (ops/s) **or** `latency` (service time, distribution allowed), `concurrency`, `queue_limit`, `yellow` (0.70), `red` (0.90), `drop_policy` (`tail` | `random_early` | `reject`).
- `Node`: id, datacenter, `Capacity`, bounded inbox, behaviour, metrics handle.
- `Behaviour`: the pluggable part; see §5.

Determinism contract: one thread per run, one seed, event ordering by `(at, seq)`.
Identical input gives byte-identical output. Parallelism only across runs (`sweep`).

## 5. Extension points (concepts)

```cpp
template <class B>
concept NodeBehaviour = requires(B b, Message& m, NodeContext& ctx) {
    { b.on_receive(m, ctx) } -> std::same_as<Outcome>;  // reply | forward | fan_out | drop
    { b.on_tick(ctx) };                                 // gossip, replication lag, TTL expiry
};
template <class P> concept EvictionPolicy    = requires(P p, Key k) { p.touch(k); { p.victim() } -> std::same_as<std::optional<Key>>; };
template <class P> concept BalancingPolicy   = requires(P p, std::span<const NodeId> n, const Message& m) { { p.pick(n, m) } -> std::convertible_to<NodeId>; };
template <class A> concept RateLimitAlgorithm = requires(A a, SimTime t) { { a.try_acquire(t) } -> std::same_as<bool>; };
template <class C> concept ConsistencyPolicy = requires(C c, ReplicaSet r, Message& m) { { c.plan_write(r, m) } -> std::same_as<WritePlan>; };
```

Policies compose at compile time (`Cache<LRU, WriteThrough>`); a registry maps YAML
strings to instantiations. The topology stores behaviours behind a small type-erased
wrapper so the engine loop is uniform. Behaviours may be written in process style with
coroutines (`co_await ctx.send(to, msg)`), built on an event-oriented core.

## 6. Latency and throughput defaults (back-of-the-envelope table)

Shipped as `defaults/latency_numbers.yaml` and loaded before every scenario. Every value
can be overridden per node or per link. Sources: *System Design Interview* ch. 2 and the
"latency numbers every programmer should know" table.

| key | value | used as default for |
|---|---|---|
| `memory_read_1mb` | 250 µs | cache service time floor |
| `ssd_random_read` | 150 µs | SQL/NoSQL read on SSD |
| `disk_seek` | 10 ms | SQL write / HDD read |
| `disk_read_1mb` | 30 ms | blob store per-MB cost |
| `network_read_1mb` | 10 ms | CDN/blob per-MB transfer |
| `compress_1kb` | 10 µs | service CPU cost unit |
| `intra_dc_rtt` | 500 µs | link latency inside a DC |
| `cross_dc_rtt` | 150 ms | link latency between DCs (CA ↔ NL); per-pair matrix allowed |
| `third_party_api` | 100 ms ± 50 | external sinks (APNs, FCM, SMS) |
| QPS defaults | web 2k, cache 100k, SQL 5k reads / 1k writes, KV 50k, queue 1M msg/s, blob 500 MB/s | `service_rate` when omitted |

Each node type declares which keys it defaults to, and `simsys explain` prints the
resolved numbers so students see where every value came from.

## 7. Node catalogue

| Node | Behaviour and knobs | Chapters |
|---|---|---|
| `client` | rate (constant / poisson / diurnal / burst), op mix, keys (uniform / zipf), timeout, retries + backoff, origin DC | all |
| `cdn` | read-only edge cache before an origin; TTL; per-DC instance | 14, 15 |
| `load_balancer` | round_robin / weighted / least_connections / consistent_hash; health checks; sticky | all |
| `rate_limiter` | token_bucket / leaky_bucket / fixed_window / sliding_log / sliding_counter; local or backed by a cache node | 4, 8, 10 |
| `service` | per-op CPU cost, downstream steps (sequential / parallel), `replicas: N` | all |
| `cache` | eviction lru / lfu / fifo / ttl; strategy cache_aside / read_through / write_through / write_around / write_back; capacity in keys | 8, 11–14 |
| `sql_db` | primary + replicas, replication lag, read/write split, connection pool, hash or range sharding | 8, 10–12, 15 |
| `kv_cluster` | hash ring with virtual nodes, replication N, quorum W/R, vector clocks or LWW, hinted handoff, gossip, AP/CP toggle | 6, 11–13 |
| `message_queue` | topics, partitions, consumer groups, retention, at-least-once redelivery | 10–12, 14, 15 |
| `worker_pool` | consumes a queue, per-job cost, DAG stages | 9, 10, 14, 15 |
| `blob_store` | MB/s throughput, multipart, presigned direct upload | 14, 15 |
| `search_index` | trie / inverted index, build vs serve split | 13 |
| `ws_gateway` | stateful connections, presence heartbeats, session fan-out | 12 |
| `service_discovery` | leader election, membership, config watch | 6, 12 |
| `id_generator` | snowflake / ticket_server / uuid; clock-skew faults | 7, 8, 12 |
| `external_sink` | third-party API or "the Internet"; latency + failure rate + per-host politeness | 9, 10 |
| `notification_fanout` | per-channel routing, dedup, retry with DLQ | 10 |
| `frontier` | URL frontier with priority and politeness queues, bloom dedup | 9 |

Consistent hashing, quorum, vector clocks, gossip and leader election are **policies
inside** `kv_cluster`, `load_balancer` and `service_discovery`, never standalone nodes.

Fault injection (`faults:` list): `node_down`, `dc_down`, `partition` between DCs,
`latency_spike`, `traffic_spike`, each with `at:` and `for:`.

## 8. Scenario format

```yaml
version: 1
name: news-feed
seed: 42
duration: 120s
warmup: 10s

datacenters:
  us-east: {}
  eu-west: {}
  latency: [{between: [us-east, eu-west], rtt: 70ms}]   # overrides cross_dc_rtt default

nodes:
  users:       {type: client, dc: us-east, rate: {poisson: 10k/s},
                ops: {read_feed: 0.9, post: 0.1}, keys: {zipf: 1.1}, timeout: 500ms, retries: 2}
  lb:          {type: load_balancer, dc: us-east, policy: least_connections}
  web:         {type: service, dc: us-east, replicas: 8, capacity: 2k/s, latency: 0.5ms,
                ops:
                  read_feed: [{to: feed_cache, op: read}, {to: post_db, op: read, when: miss}]
                  post:      [{to: post_db, op: write}, {to: fanout_q, op: publish}]}
  feed_cache:  {type: cache, eviction: lru, strategy: cache_aside, keys: 1M}
  post_db:     {type: sql_db, replicas: 2, replication_lag: 50ms, capacity: 5k/s,
                advanced: {queue_limit: 500, drop_policy: reject, service_time: {lognormal: [5ms, 0.3]}}}
  fanout_q:    {type: message_queue, partitions: 8}
  fanout:      {type: worker_pool, consumes: fanout_q, workers: 16, cost: 2ms,
                then: [{to: feed_cache, op: write}]}

links:
  - {from: users, to: lb, dir: inout, ops: [read_feed, post]}      # latency: intra_dc default
  - {from: lb, to: web}
  - {from: web, to: feed_cache, latency: 0.3ms}
  - {from: web, to: post_db}
  - {from: web, to: fanout_q}

slo:
  - {metric: p99_latency, node: users, op: read_feed, max: 200ms}
  - {metric: drop_rate, max: 0.001}

faults:
  - {at: 60s, kind: node_down, node: post_db.primary, for: 10s}
```

Rules: a link's `latency` overrides the DC default; a node's `latency` overrides the
type default; `capacity` as `N/s` is sugar for `advanced.service_rate`; `replicas` on a
node is sugar for N identical nodes behind an implicit round-robin.

## 9. C++20/23 study map

Each item is used on purpose and documented in `docs/study/<feature>.md`.

| Feature | Where |
|---|---|
| Named modules | one module per layer; `import simsys.core;` |
| Concepts & `requires` | all extension points (§5) |
| Ranges & views, `ranges::to` | routing filters, replica selection, report tables |
| Coroutines (`std::generator`, custom awaitables) | process-style behaviours, workload generators |
| `std::expected` | config loading and validation errors with line/column |
| `std::format`, `std::print` | reporters |
| Deducing `this` | CRTP-free policy mix-ins |
| `<=>` and `= default` comparisons | `Event` ordering, `SimTime` |
| Designated initializers, `consteval`, `constexpr` containers | defaults table, unit parsing (`10k/s`, `0.5ms`) at compile time |
| `std::span`, `std::string_view`, `std::variant` + `std::visit` | message payloads, policy dispatch |
| `std::chrono` calendar/duration | `SimTime`, diurnal curves |
| `std::jthread`, `stop_token`, `latch` | parallel `sweep` only |
| `std::mdspan` (if GCC 14 ships it; else skip) | DC latency matrix |

Not used on purpose: raw `new`, macros beyond include guards (none with modules), exceptions
for control flow (errors are `std::expected`; exceptions only for programmer errors).

## 10. CLI

```
simsys validate scenario.yaml
simsys explain  scenario.yaml                      # resolved graph, defaults applied, sanity checks
simsys estimate scenario.yaml                      # analytical model, instant
simsys run      scenario.yaml [--seed 7] [--duration 60s] [--report table|json|csv] [--trace out.jsonl]
simsys sweep    scenario.yaml --param nodes.users.rate=1k/s..100k/s:10 --metric users.p99 [--jobs 8]
```

`run` prints a per-node table (throughput, utilization coloured green/yellow/red, queue depth,
p50/p95/p99, drops) and an SLO pass/fail block. Exit code reflects SLO result.

## 11. Testing

- Unit tests per policy (Catch2).
- Theory tests: M/M/1 and M/M/c nodes must match closed-form mean wait within tolerance;
  Little's law per node; DES steady state must agree with `estimate`.
- Scenario tests: every shipped YAML validates, runs under 10 s, and its `fixed` variant
  passes its SLOs while its `broken` variant fails them.
- Determinism test: two runs with the same seed produce identical traces.
- CI: GCC 14 and Clang 18, `-Wall -Wextra -Werror`, ASan/UBSan job.

## 12. Phases and effort (assisted by an AI pair; unassisted in parentheses)

| Phase | Scope | Effort |
|---|---|---|
| 0 | Toolchain spike (GCC 14 + Clang 18 + CMake modules build), all 12 scenarios drafted by hand in YAML, schema adjusted until they fit | 1 week |
| 1 | Core engine, model, config, defaults table, metrics, `validate`/`explain`/`run`, theory tests | 2 weeks (4) |
| 2 | Basic node pack + `estimate` + `sweep` | 2 weeks (4–5) |
| 3 | Distributed-data pack: `kv_cluster`, `service_discovery`, fault injection | 3 weeks (6–8) |
| 4 | 8 book systems + 4 labs, broken/fixed variants, SLOs, READMEs | 1.5 weeks (3) |
| 5 | Study notes, docs, performance pass, trace format freeze, CI | 1 week (2) |
| | **Total CLI v1** | **≈ 10–11 weeks** |
| 6 | GUI replaying the trace (separate project) | 2–3 months |

## 13. Repository layout

```
CMakeLists.txt  cmake/  defaults/latency_numbers.yaml
src/{core,model,policies,nodes,config,analytic,cli}/
scenarios/{book,labs,tutorial}/
tests/{unit,theory,scenario}/
docs/{approaches.md, study/, superpowers/specs/}
```
