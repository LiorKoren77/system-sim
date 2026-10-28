# Phase 2b: Che Cache Model, SQL Database, CDN, ID Generator — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The first book system runs end to end. After 2b, `book/url-shortener` (client, rate limiter, cache with `hit_ratio: che`, load balancer, service, id generator, SQL database) and the labs `consistent-hashing` and `unique-id` run and report against their SLOs; caches get the Che/LRU hit-ratio model from the clients' key distribution; SQL databases model a primary with read replicas, shards and replication lag (the `staleness` SLO); request-based CDNs serve from the edge and miss to their origin; the CSV/JSON series carry the demand, drain, offered and answered columns the GUI will need.

**Architecture:** One new pure-maths module `simsys.caching` (Zipf popularities, the Che approximation). The graph resolves each cache's/CDN's key distribution from the clients upstream and computes a static hit ratio at build time. Three new behaviours (`SqlDbBehaviour` with two queues, `CdnBehaviour`, `IdGeneratorBehaviour` as service sugar) join the variant. Caches gain `then` steps and `write_back`. Reports gain `staleness` and the new columns. Phase 2c adds byte flows (blob, byte-capacity CDN), queues/workers, external sinks and `sweep`.

**Tech Stack:** unchanged. New standard facilities: none of note (`std::ranges::fold_left`, `std::numbers`).

**Conventions:** as in the 2a plan (TDD split; own build dirs for concurrent agents; stage by name; no `[`/`]` in test names; `WithinRel`/`WithinAbs`; the four GCC 14 workarounds; a new `model.cpp` line when Release linking breaks).

**Spec refinements made here (spec §18 in Task 9):**

1. **Key distribution resolution.** A cache or CDN with `hit_ratio: che` takes its request key distribution from the clients that reach it (walking demand edges upstream). Exactly one distinct `keys` spec must reach it (`{zipf: s, space: N}` or `{uniform: N}`); none → error "`che` needs a client with `keys` upstream"; different specs → error naming the clients. Phase 2c may mix them by rate.
2. **Che approximation (Che, Tung, Wang 2002; Fricker, Robert, Roberts 2012 for the LRU justification):** with popularities `p_i` and cache size `C` keys, the characteristic time `t_c` solves `Σ_i (1 − e^{−p_i t_c}) = C`; the hit ratio is `h = Σ_i p_i (1 − e^{−p_i t_c})`. Zipf sums over up to 1e9 keys are evaluated by grouping ranks into log-spaced buckets (2000 buckets; relative error ≤ 1e-3, verified against a dense sum in tests). `eviction` other than `lru` uses the same model with a documented caveat (FIFO is within a few percent; LFU is better than Che says; `ttl` caps nothing yet).
3. **SQL database:** `replicas: N` = one primary plus `N − 1` read replicas (book ch. 1). Writes (`write_ops`, default `write insert update delete`) go to the primary; reads go to the replicas when `N ≥ 2`, else to the primary; every replica also applies every write (replication load). `shards: S` divides both loads by `S` (each shard is a primary + replicas set of the same size). `pool` = `advanced.concurrency` sugar. `replication_lag` is a constant; **staleness** reported for the node = `replication_lag + the primary's write sojourn` (a read may see data that old); the `staleness` SLO compares it with `max`. Two queues per shard set: primary and replica pool (independent copies); reported `rho` = the busier of the two; latency per op from its own queue.
4. **CDN (request-based):** `hit_ratio` fixed or che; misses go to `origin` as the same op (demand edge); `ttl` accepted, not modelled; a CDN with a byte capacity (`100GB/s`) is refused until 2c ("needs byte flows"); one instance per `dc` as written.
5. **Cache `then` steps and `write_back`:** `then` steps with `on: miss` receive the miss fraction of reads (the cache's own backend fetch under read_through is still exclusive with a `then` miss step — error); `on: always` then-steps receive everything; `write_back` absorbs writes (served, nothing forwarded synchronously) and flushes them to the backend as an asynchronous hop (no latency charged) at the same rate — the backend's load is unchanged, the client's write latency is the cache's own.
6. **ID generator:** `algorithm: snowflake` = a service whose capacity is `4096 × 1000 × replicas`/s by design (sequence bits per ms) unless the scenario says otherwise; `ticket_server` = a single-replica service (its `replicas` must be 1 — error otherwise); `uuid` = unbounded (capacity 1e9/s, 1 µs). Modelled as `ServiceBehaviour` with defaults — no new behaviour class (Pattern: sugar resolved at build).
7. **Series/JSON columns:** CSV gains `demand_per_s,drain_per_s` after `node` and the op rows get `offered_per_s,answered_per_s`; JSON ops gain `answered_per_s`.

---

## File structure

| Path | Responsibility |
|---|---|
| `src/caching/caching.cppm` | Module `simsys.caching`: `Popularity` (zipf/uniform), `zipf_buckets`, `che_hit_ratio(C, popularity)` |
| `src/model/model.cppm` | *modify*: `KeySpec{zipf s, space N}` / uniform on `Workload`; `NodeSpec::{shards, pool, replication_lag, origin, algorithm (id)}`; `SloMetric::staleness` already exists |
| `src/config/config.cpp` | *modify*: client `keys`, sql keys, cdn keys, id keys |
| `src/engine/graph.cppm` | *modify*: upstream key resolution, che hit ratios, origin edges, cache then-steps, sql/id resolution |
| `src/engine/behaviours.cppm` | *modify*: `CacheBehaviour` then/write_back; `SqlDbBehaviour`; `CdnBehaviour` |
| `src/engine/engine.cppm` | *modify*: two-queue nodes (sql), staleness sample, new columns |
| `src/report/*` | *modify*: staleness SLO, columns, explain lines |
| `tests/unit/test_caching.cpp` + the usual | Tests |
| `scenarios/book/url-shortener/README.md`, `labs/consistent-hashing/README.md`, `labs/unique-id/README.md` | Real numbers |
| docs | schema v1.3, spec §18, study note `numerics.md` (bucketed sums, root finding, floating point in models) |

---

### Task 1: `simsys.caching` — Zipf popularities and the Che approximation

**Files:** create `src/caching/caching.cppm`, `tests/unit/test_caching.cpp`; CMake.

- [ ] **Step 1: Write the failing tests**

```cpp
// The Che approximation for an LRU cache under a Zipf or uniform request distribution.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <vector>
import simsys.caching;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using simsys::caching::che_hit_ratio;
using simsys::caching::Popularity;

TEST_CASE("under a uniform distribution the hit ratio is the cache's share of the key space", "[caching]") {
  // p_i = 1/N: the Che equation gives 1 - e^{-t/N} = C/N exactly, so h = C/N.
  REQUIRE_THAT(che_hit_ratio(500, Popularity::uniform(1000)), WithinRel(0.5, 1e-9));
  REQUIRE_THAT(che_hit_ratio(1000, Popularity::uniform(1000)), WithinRel(1.0, 1e-9));
  REQUIRE_THAT(che_hit_ratio(0, Popularity::uniform(1000)), WithinAbs(0.0, 1e-12));
}

TEST_CASE("a cache larger than the key space hits everything", "[caching]") {
  REQUIRE_THAT(che_hit_ratio(5000, Popularity::zipf(1.0, 1000)), WithinRel(1.0, 1e-12));
}

TEST_CASE("the bucketed Zipf sum matches a dense sum to a tenth of a percent", "[caching]") {
  // Dense reference: p_i = i^-s / H, 10k keys, C = 100, solved by bisection here in the test.
  const double s = 1.0; const int n = 10'000; const double cap = 100;
  std::vector<double> p(n); double h_n = 0;
  for (int i = 1; i <= n; ++i) { p[i - 1] = std::pow(i, -s); h_n += p[i - 1]; }
  for (auto& x : p) x /= h_n;
  auto in_cache = [&](double t) { double sum = 0; for (double x : p) sum += 1 - std::exp(-x * t); return sum; };
  double lo = 0, hi = 1e12;
  for (int k = 0; k < 200; ++k) { const double mid = (lo + hi) / 2; (in_cache(mid) < cap ? lo : hi) = mid; }
  double h_ref = 0; for (double x : p) h_ref += x * (1 - std::exp(-x * lo));
  REQUIRE_THAT(che_hit_ratio(cap, Popularity::zipf(s, n)), WithinRel(h_ref, 1e-3));
}

TEST_CASE("a hotter key space (larger zipf exponent) hits more", "[caching]") {
  const auto cold = che_hit_ratio(1000, Popularity::zipf(0.6, 1'000'000));
  const auto hot = che_hit_ratio(1000, Popularity::zipf(1.2, 1'000'000));
  REQUIRE(hot > cold);
  REQUIRE(cold > che_hit_ratio(1000, Popularity::uniform(1'000'000)));
}

TEST_CASE("a hundred-million-key zipf space evaluates in well under a second", "[caching]") {
  const auto h = che_hit_ratio(20e6, Popularity::zipf(1.0, 100'000'000));   // url-shortener's url_cache
  REQUIRE(h > 0.5);
  REQUIRE(h < 1.0);
}

TEST_CASE("invalid popularities are programmer errors", "[caching]") {
  REQUIRE_THROWS_AS(Popularity::zipf(-0.1, 10), std::invalid_argument);
  REQUIRE_THROWS_AS(Popularity::uniform(0), std::invalid_argument);
  REQUIRE_THROWS_AS(che_hit_ratio(-1, Popularity::uniform(10)), std::invalid_argument);
}
```
(Add a timing assertion with `std::chrono::steady_clock` under 500 ms for the 1e8 case.)

- [ ] **Step 2–5:** register; implement (`Popularity` = `{double exponent; double space; bool uniform;}` with factories; `zipf_buckets(s, N)` → vector of `{count, mean_p}` over log-spaced rank ranges (bucket edges `⌊N^(k/B)⌋`, B = 2000, the mean probability from the closed-form integral of `x^{−s}` over the bucket; normalise by the bucketed `H_{N,s}`); `che_hit_ratio` solves `Σ count·(1 − e^{−p t}) = C` by bisection on `log t` (60 iterations), returns `Σ count·p·(1 − e^{−p t})`; C ≥ Σcount → 1; C ≤ 0 → 0). Comments: name the approximation and its assumptions (IRM: independent reference model — no temporal locality; LRU), the bucket error bound, and that this is the "cache with 20 % of the hot keys" back-of-the-envelope the book does by hand. Commit `caching: Zipf popularities and the Che approximation (bucketed)`.

---

### Task 2: Client `keys` and upstream key resolution; che hit ratios at build

**Files:** `src/model/model.cppm` (`KeySpec` on `Workload`), `src/config/config.cpp`, `src/engine/graph.cppm`, `src/report/report.cpp` (explain prints `hit_ratio=0.73 (che: zipf 1.0 over 100M keys from users)`), tests.

- [ ] Tests: loader — `keys: {zipf: 1.0, space: 100M}` and `{uniform: 10M}` typed; `zipf` without `space` → `node 'u': keys needs zipf and space, or uniform`; `zipf: -1` → `node 'u': keys.zipf must be non-negative`; both forms → error. Graph — a che cache fed by one client resolves `hit_ratio` to `che_hit_ratio(keys, popularity)` (pin against the module for a small space); two clients with different specs → `node 'c': hit_ratio che: clients 'a' and 'b' send different key distributions`; no client with keys → `node 'c': hit_ratio che needs a client with keys upstream`; a che cache behind an LB and a limiter still resolves (walk through forwarders). Theory — url-shortener's `url_cache` resolves to a number in (0.5, 1); `Simulation::create` no longer refuses che. Explain line as above.
- [ ] Commit `graph: che hit ratios from the clients' key distribution`

---

### Task 3: Cache `then` steps and `write_back`

**Files:** behaviours, graph, engine, tests (`search-autocomplete`'s `browser_cache` then-steps; crawler's caches).

- [ ] Tests: a cache with `then: [{to: lb, op: suggest, on: miss}]` forwards `1−h` of reads to `lb` (behaviour `route`), response = own + (1−h) × hop mixture; `on: always` then-steps forward everything; `read_through` + a then miss step → error `node 'c': a then step on miss and a read_through backend both fetch the miss`; `write_back`: writes served locally, forwarded to the backend as an asynchronous hop (the backend's demand includes them; the client's write response = the cache's own sojourn); `Simulation::create` accepts caches with then-steps and write_back. Theory: a client → browser_cache (0.4) → lb → service chain: the service sees 60 %.
- [ ] Commit `nodes: cache then-steps and write-back`

---

### Task 4: SQL database

**Files:** model (`shards`, `pool`, `replication_lag`, `write_ops` reused), config, graph, behaviours (`SqlDbBehaviour`), engine (two queues; `NodeSample::staleness_s`), report (staleness SLO; explain `primary+2 replicas, shards=4, lag=50ms, pool=32`), tests, `scenarios/labs/consistent-hashing/README.md`.

- [ ] Tests (theory, pin with `mmc_wait`): `replicas: 3`, 1000 reads/s + 100 writes/s, capacity 1k/s per copy, 1 ms service: primary demand 100 (writes), replica pool demand = 1000/2 + 100 per copy = 600/s each; rho reported = max(primary 0.1, replica 0.6) = 0.6; read latency = Wq(replica copy) + s; write latency = Wq(primary) + s; staleness = 50 ms + write sojourn; `shards: 2` halves both; `replicas: 1`: reads and writes on the primary; `pool: 8` → concurrency 8 on both queues. Loader: `shards` positive int; `pool` positive int ≤ max_concurrency; `replication_lag` duration; `write_ops`. Report: `staleness` SLO passes/fails; JSON `staleness_s`. Explain line. Scenario: `labs/consistent-hashing` runs (cache che + LB consistent_hash + sql) and its README gets real numbers (vnodes 1 vs 100 vs 1000).
- [ ] Commit `nodes: sql database with a primary, read replicas, shards and replication lag`

**Review of 48d4d0b (adversarial, REQUEST_CHANGES) — fix round:**

Correct and pinned by the reviewer: the plan numbers (replica 600/s, reads 3.0 ms, writes 1.61 ms, staleness 51.1 ms), shards/N=1/pool wiring, estimate = run, every lab README number, the ch. 5 quote, client-share conservation, per-op `own` for other node types.

- [x] **C1 staleness ignores the replica's queue.** `staleness_s = lag + primary sojourn + replica sojourn` (a replayed write waits in the replica's queue too). A replica overloaded by replication drops replicated writes nobody sees; report them in `NodeSample::replication_dropped_per_s` (same optional convention as `staleness_s`), print them in the summary/JSON, and fail the `staleness` SLO when they are > 0 with "replica diverges: drops N/s replicated writes". Update the `NodeSample::staleness_s` comment, the `SqlDbBehaviour` header and the schema's Staleness paragraph.
- [x] **C2 replicas replay the writes clients offered, not the ones the primary committed.** Serve the primary group first (groups are already primary-first), then send each replica `primary drained writes / S` instead of `writes / S`. Test A.
- [x] **C3 `pool` breaks Little's law.** `c = min(pool, implied_concurrency)`, `service_rate = min(capacity, pool / service_time)`: a pool bounds throughput, never speeds the disk. Explain prints `capacity=400/s (pool 4 / 10ms)` when the pool binds. Comment `// Little's law: concurrency in service = throughput × service time`. Test: 5k/s, 10 ms, `pool: 4`, 3000 reads/s on `replicas: 1` → rho 1, drops ≈ 2600/s.
- [x] **I1** a `consistent_hash` balancer's imbalance is not applied to `sql_db` (explain test: no `hottest copy`).
- [x] **I2** `write_ops` is optional with type defaults (landed with the then-step round); drop the graph-level widening and the schema's "price" paragraph.
- [x] **I3** `cost` is a valid `sql_db` key; write ops default to cost 5 (spec §6: SQL 5k reads/s vs 1k writes/s) with the citation; fix `behaviours.cppm`'s "costs more capacity via cost" claim to match.
- [x] **I4** schema states backlog and drops are the clients' part; test D (client totals balance while a replica drops replicated writes).
- [x] Minor: doc/comment example zone `(60%, green)`; README staleness 50.2 ms and a note that no write flows in that lab; "vnodes 1 fails only the cache SLO" in README and scenario header; `replication_lag` on `replicas: 1` explains as `(unused: no replicas)`; an outgoing link from a `sql_db` is refused ("a database calls nobody"); one graph-level `database_copy_loads(spec, reads, writes)` used by `make_database` and `database_check`; `record_sojourns` split (read mixture / write mixture / mix by rate); non-database fast path in `split_writes`/`group_arrivals`/`clients_part`; explain headline zone = worst copy for `sql_db`; JSON test checks the staleness value; tests A–D.
- [x] Commit `nodes: sql database fixes from review — committed-write replay, replica staleness, pool bounds throughput`

**Re-review of 96848f0 (adversarial, REQUEST_CHANGES, no critical) — text/explain fix round.** Verified by the reviewer: tests A/B/pool numbers, replay at cost 5 with shards, each copy's own pool, estimate = run, conservation (2e-10 over 400 steps, two clients, shards 2 × replicas 3), lab README numbers, explain texts, JSON fields; write cost 5 as the default is the right teaching decision (spec §6's 5k/1k; a default of 1 would let one primary absorb the book's write load; `url_db` at 173 % is the book's own sharding lesson).

- [x] **1 Replicated drops per replica.** `replication_dropped_per_s` is summed over replicas and can exceed the committed writes (6 copies, 5000 reads + 900 writes: 2131/s "dropped" of 900 committed). Report per replica: `total / ((N−1)·S)` beside `committed / S`; SLO note "each replica misses 426/s of 900/s committed writes (47%)".
- [x] **2 "Diverges for good" is not what replicas do.** MySQL relay log / Postgres WAL keep unapplied events: an overloaded replica falls behind without bound and is rebuilt when retention runs out. Reword in `behaviours.cppm`, `engine.cppm`, `report.cpp`, schema: "falls behind without bound (the model drops the excess at queue_limit; a real replica keeps it in its relay log/WAL until it is rebuilt)"; the reported staleness is the bounded-queue figure, said so in the schema. Keep `staleness_s` finite (JSON carries no infinity); the SLO fails through the note.
- [x] **3 `pool` ≠ `advanced.concurrency` now.** Keep refusing both together; message "pool sets c and caps the rate by Little's law; drop advanced.concurrency"; schema row and `model.cppm` comment updated.
- [x] **4 Lag default.** "default 0 (a synchronous replica)" is wrong (lag 0 still reports both sojourns): "default 0: shipping takes no time; the write still waits in both queues".
- [x] **5 Explain prints the defaulted write cost**: `cost: write 5 (default: spec §6 5k reads/1k writes)` per defaulted write op; database checks labelled in units or with requests beside; schema sentence: a `sql_db`'s `capacity` is reads/s.
- [x] Minor: the `sql_db` skip in `Graph::build` moves above the "different replica models" conflict check (two balancers of different models may front a database); the `FluidQueue::settle` tie at rho = 1 exactly (estimate 2.05 s vs run 1.15 s in a write-only probe) mentioned in the schema; `apply_database_keys` and schema say the pool caps capacity units (a write holds its connection 5 × service time); the worked example in `make_database`'s comment and the schema moves off rho = 1 (1000 reads + 100 writes at cost 5 on 1k/s copies); `enum class CopyRole { server, primary, replica }` on `CopyGroup` instead of inferring from `request_share > 0`; extract `replay_committed(...)` from `serve()`; `record_sojourns` takes `node` only; test D's comment says the dangling reference is real.
- [x] Commit `nodes: sql database — per-replica lag drops, honest replica wording, pool and lag docs, explain shows the default write cost`

---

### Task 5: CDN (request-based)

**Files:** model (`origin`), config, graph (origin edge; che via upstream clients; byte capacity → refusal "needs byte flows (phase 2c)"), behaviours (`CdnBehaviour` = a cache with `read_through`-like miss forwarding to `origin`, no write path: writes are an error at build `node 'cdn': a cdn does not take writes`), engine, report, tests. `news-feed`'s `cdn` (fixed 0.95, origin media_store) must explain and the miss flow must reach `media_store` in the paper load.
- [x] Commit `nodes: request-based cdn with an origin`

**Review of 4d539b0 (adversarial, REQUEST_CHANGES, no critical) — fix round.** Verified by the reviewer: the behaviour mixture (1.35 ms, var 2.4175e-6), theory 2.5 ms, delivered 0.8775, news-feed 324.3/s, youtube che 0.642/0.594 (independent Python), delegation byte-identical to a read_through cache incl. a saturated origin and a call cycle, no write path, estimate = run across cross-dc / cdn→cache / cache→cdn / cdn→cdn / ring probes, validation of origin variants, schema and READMEs, first-use comments, test names.

- [x] **I1 Byte-sized capacities in explain.** The engine refuses to run a byte-capacity cdn because converting bytes at an assumed object size "would invent the number", yet explain does exactly that (`defaults::blob_object_bytes = 1e6`) and labels it `(scenario)`: youtube `cdn_us capacity=100k/s (scenario)` at 0.28 % green, really ≈14 % by the scenario's 50 MB streams; `transcoded_store` 1.58 % green, really ≈79 % (yellow); news-feed `media_store` 32.4 %, really ≈6.5 %. Fix: tag every check whose capacity came from a byte rate (`capacity from 100GB/s at 1 MB/object; byte flows: phase 2c`) or skip its percentage; say so in the youtube and news-feed READMEs; reword the `cdn_not_runnable` comment.
- [x] **I2 Any non-`write` op reaching a cdn is cached as a read.** `ops: {read: 0.8, upload: 0.2}` → cdn serves 90 % of uploads from the edge. Accept `write_ops` on a cdn in the loader (schema says it applies); explain prints the ops a cdn caches (`serves: read, upload`); document.
- [x] Minor: M1 stale comments describing cdn then-steps/miss-step lookups (`graph.cppm` then_fraction, forwards_own_misses, ~1151, ~1765, ~1805, ~2264, ~2298; `report.cpp` ~1200); M2 stale TDD "proposal" notes in tests (test_graph ~1624/~1694, test_behaviours ~35/~1435, test_config ~3215/~3231, test_report ~1131); M3 message "must follow a cache or cdn step" → "must follow a cache step" (+ test, schema `on` row); M4 a cdn with no hit_ratio gets the che "upper bound" note although h = 0 is what runs (limit `unknown_misses` to che-without-number); M5 add a code-built-cache case for the service-miss-step branch of `unknown_miss_targets`; M6 refuse `origin` = the cdn itself in loader and `invalid_cdn`; M7 wording (che filter error says "cache" for a cdn: use the node's type; "a cdn does not take writes" names op and sender; one client wording); M8 one `connect_store(node, role)` for cache backend and cdn origin, export `hits_or_misses` from graph for the report; M9 `invalid_cdn` refuses a non-default strategy/backend (write_back on a cdn skips the call-cycle check); M10 guard the `declared.ids.find(id)->second` in `read_cdn_origin`; M11 `CdnBehaviour` header: paraphrase not in quotes, che assumes LRU + independent requests, ch. 1 "CDN fallback", tiered cdn → cdn works; M12 tests: pin youtube 0.642/0.594 and `transcoded_store` 157.8/s, youtube's "phase 2" assertion, `ttl` stored, call-cycle wording; M13 clang-format broke a variance comment in test_behaviours ~1484; M14 pre-existing test name with `[0, 1]` in test_config ~1694 (fix: ctest discovery).
- [x] Commit `nodes: cdn fixes from review — byte-derived capacities tagged in explain, write_ops on a cdn, honest comments`

---

### Task 6: ID generator as service sugar

**Files:** model (`id_algorithm`), config (`algorithm` on id_generator: snowflake|ticket_server|uuid), graph (resolve defaults: snowflake capacity `4096 × 1000`/s per replica, 10 µs; ticket_server `replicas == 1` else error; uuid 1e9/s, 1 µs), engine (`create` accepts id_generator as a service), report (explain `algorithm=snowflake (4.1M/s per replica by design)`), tests, `scenarios/labs/unique-id/README.md` real numbers (ticket server at 8k/s saturates at ... vs snowflake).
- [ ] Commit `nodes: id generator (snowflake, ticket server, uuid) as service sugar`

---

### Task 7: Series and JSON columns; explain propagation note

**Files:** report, tests. CSV header `t_s,node,demand_per_s,drain_per_s,rho,backlog,drops_per_s,rejected_per_s,latency_ms,zone`; op rows (a second CSV section? keep one file: node rows only; add `--series-ops out.csv` with `t_s,client,op,offered_per_s,answered_per_s,mean_ms`) — decide: two files is cleaner; `main.cpp` gains `--series-ops`. JSON ops gain `answered_per_s`. Explain: nodes reached only through types that do not yet propagate load get `(load not propagated: <type> is phase 2c/3)` instead of a green 0 %.
- [ ] Commit `report: demand/drain/offered/answered columns; explain marks unpropagated load`

---

### Task 8: The first book system runs

**Files:** `tests/scenario/test_scenarios.cpp` (the runnable set becomes hello, chain, lab-rate-limiter, lab-consistent-hashing, lab-unique-id, url-shortener), `scenarios/book/url-shortener/README.md` (real numbers: cache hit ratio from Che, p99 per op, the 10:1 read/write split at the db, what fails when the cache is removed), `README.md`.
- [ ] `simsys run scenarios/book/url-shortener/scenario.yaml` exits 0 (if an SLO fails on the shipped numbers, judge: fix the scenario's sizing with the book's figures, not the SLO; say so).
- [ ] Commit `scenarios: url-shortener runs end to end; READMEs with real numbers`

---

### Task 9: Docs

Schema v1.3 (keys, sql, cdn, id keys; che resolution; staleness; columns; runnable set), spec §18 (refinements 1–7), `docs/study/numerics.md`, README. Commit `docs: schema v1.3, spec §18, numerics note`.

---

## Self-review

- Spec coverage: §7 `cache` che (T1–2), `sql_db` (T4), `cdn` (T5, request-based; bytes 2c), `id_generator` (T6); §5 `HitRatioModel` concept — introduce it in T1 as `concept HitRatioModel` satisfied by `che_hit_ratio` wrappers? Keep: a `HitRatioModel` concept with `Fixed` and `Che` policy types in `simsys.caching`, dispatched at build (Pattern: Strategy/Policy) — add to T1's interface so students see the §5 concept.
- Backlog items from 2a folded in: columns (T7), explain propagation note (T7). The rest stays in the 2a plan's backlog list.
- Type consistency: `Popularity`, `che_hit_ratio(C, Popularity)`, `Graph::hit_ratio(node)` unchanged in shape; `NodeSample::staleness_s`; `SqlDbBehaviour`; `CdnBehaviour`.
