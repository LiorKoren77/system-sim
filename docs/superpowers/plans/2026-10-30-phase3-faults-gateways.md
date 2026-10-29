# Phase 3: Layout, Faults, Retries, Replications and the Last Four Node Types — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every shipped scenario runs end to end. The four book systems phase 2 refused (chat-system, google-drive, notification-system, web-crawler) run with their SLOs judged; a scripted fault acts on the running model and the report says what it cost; retries are modelled as the feedback they are and shown as `attempts/s`; `on: failure` steps fire; `simsys run --replications N` turns the rate noise into bands. The three largest source units and the theory test file are split first, so every later diff is small.

**Architecture:** The spec `docs/superpowers/specs/2026-10-30-phase3-faults-gateways-design.md` (approved 2026-10-30) decides everything below; this plan turns it into tasks. Faults are a schedule of per-step `Condition`s (capacity factor, extra latency, rate factor, hit-ratio factor) that the engine applies where a node's own numbers enter the queueing model, so a run without faults stays bit-identical to phase 2. Retries are feedback demand read one step late (like `delivered_of`), damped in `estimate` with the phase 2c scheme. Replications reuse `simsys.session`'s thread pool. The four types are behaviours in `simsys.behaviours` like the fourteen that exist. Sub-phases: **3a** layout (Tasks 1–4), **3b** faults, failure steps, retries, replications (Tasks 5–12), **3c** the four types with their scenarios (Tasks 13–18), **3d** docs and the phase review (Task 19, then the review section).

**Tech Stack:** C++23, GCC 14.2 named modules (the five defects of CLAUDE.md §3 and their workarounds apply to every task; a split that moves code between units re-checks the fourth and fifth), CMake presets `gcc14-debug` / `gcc14-release`, Catch2 3.7.1, yaml-cpp, CLI11, clang-format-18. Each task runs the phase 2 loop: a sonnet test-writer's red commit (tests that compile against today's API where possible, a stub-verified red otherwise), an opus implementer's green commit, an adversarial opus review in a worktree recorded here as "Review of Task N at <sha>", a fix round. Concurrent agents use their own build dirs (`build/gcc14-p3-tN`), stage by file name, and implementers of different tasks work on branches in worktrees when their files overlap.

**Numbers in this plan.** A number in **bold** is pinned by a test before the implementation exists and comes with its derivation; a number marked *measured* is what a README will quote from the Release binary once the scenario runs, and a test pins it only afterwards, the way phase 2's day tests do.

---

## File structure

**3a (moves only; no new behaviour):**
- `src/report/explain.cpp` — new implementation unit `module simsys.report;` holding what `report.cpp`'s `// ---- explain ----` … `// ---- a bandwidth link's pipe on paper ----` sections hold today (lines ~952–2624 at `0e37886`), plus `render_explain`.
- `src/report/report.cpp` — keeps number formatting, SLO evaluation, windows, table, JSON, CSV, `Summary`, the series writers.
- `src/report/detail.hpp` — if the two units need shared non-exported helpers (number formatting, enum names): a header included in both global module fragments, `inline`, under `namespace simsys::report::detail`; else a private module partition `simsys.report:shared`. The implementer picks one and records why (GCC 14's partition support is the risk).
- `src/config/{header,nodes,steps,workload,links,slos,overrides}.cpp` — implementation units of `simsys.config`, one per `// ----` block group of today's `config.cpp` (Top level + Header + Datacenters; Node declarations + the per-type key readers + Node bodies; Steps; Client workload + Client keys; Links; SLOs; Overrides), with `config.cpp` keeping `load` and the pipeline; shared helpers (`Site`, `CheckResult`, the key tables) as above.
- `src/engine/graph.cpp` — new implementation unit `module simsys.graph;` with the definitions of `Graph`'s members and `detail::` functions; `graph.cppm` keeps declarations and the inline one-liners.
- `tests/support/fixtures.cppm` — module `simsys.test_fixtures` (a `simsys_test_support` library target with `FILE_SET CXX_MODULES`) holding the builders several theory files share (`mm1`, `lb_front`, `limited`, `cached_reads`, `ring_factor`, `first_step`, `explain_of`, `piped`, the scenario-helpers of `tests/support/scenario_helpers.hpp` where they need model types).
- `tests/theory/test_{core,database_cdn,bytes_pipes,service_sugar,queues_pools,sinks,kv,failed_steps,rho_one}.cpp` — the nine files `test_theory.cpp` splits into along its `// ----` sections (the line ranges in the 2c plan's Phase 3 backlog), joining `theory_tests` beside `test_timeout.cpp` and `test_windows.cpp`.

**3b:**
- `src/model/model.cppm` — `Fault` (`FaultKind`, `at`, `for`, `node`/`datacenter`, `replicas`, `factor`, `extra`), `Scenario::faults`, `Workload::retries`/`retry_after`, `NodeSpec::retries`/`retry_after` (pools), `NodeSpec::dead_letter`.
- `src/config/slos.cpp` or a new `faults.cpp` — `read_faults`, the kind table, every refusal message of spec §4.1.
- `src/engine/faults.cppm` — new module `simsys.faults`: `Condition`, `FaultSchedule` (built from `model::Scenario`, `condition_at(node, now)` and `link_condition_at`), the warm-up curve of a flushed cache (`caching` call), the per-fault bookkeeping (`FaultRecord`: during/after/recovery numbers).
- `src/engine/engine.cppm`, `src/engine/behaviours.cppm` — conditions applied in `forward_pass` (capacity factor and extra latency into the node's `FluidQueue::set_service`/capacity, the client's rate factor in `ClientBehaviour`, the hit-ratio factor in `CacheBehaviour`/`CdnBehaviour`); `on: failure` blocks in `HopBlocks`; client retries in `ClientBehaviour` (an `attempts` ring buffer over the backoff) and `client_responses`; pool retries and the dead-letter count in `WorkerPoolBehaviour`.
- `src/report/report.cppm`, `report.cpp` — `attempts/s`, `dead_lettered/s`, the fault blocks (table and JSON), the replication table.
- `src/session/session.cppm`, `session.cpp` — `run_replications(graph, seeds, jobs)` on the sweep's pool; `src/cli/main.cpp` `--replications`.
- `src/report/series` — `attempts_per_s`, `dead_lettered_per_s` columns.

**3c:**
- `src/engine/behaviours.cppm` — `ServiceDiscoveryBehaviour`, `WsGatewayBehaviour`, `NotificationFanoutBehaviour`, `FrontierBehaviour`; `src/engine/graph.cppm` — the lookup-before-target rule, the channels map checks, a frontier's consumer group; `src/defaults/defaults.cppm` — the four types' numbers (service_discovery 50k/s 0.5 ms; ws_gateway 10k/s 0.5 ms, `session` 30 min, `reconnect_within` 30 s; notification_fanout 20k/s 1 ms; frontier 5k/s 1 ms), each marked `// chosen, not from the book` where it is; `src/config/nodes.cpp` — the keys (`connections`, `session`, `reconnect_within`, `connect` ops); `src/model/model.cppm` — the fields; `src/report/explain.cpp` — the new lines and the end of the last two "load not propagated" marks.
- `scenarios/book/{chat-system,google-drive,notification-system,web-crawler}/{scenario.yaml,README.md}`, `tests/scenario/test_scenarios.cpp`, `tests/unit/test_series.cpp`.

**3d:** `docs/scenario-schema.md` (v1.5), the spec (§20), `docs/study/*.md`, `README.md`.

---

## 3a — Layout (Tasks 1–4)

The byte-identity check every 3a task runs before its commit, from a scratch directory:

```bash
# before: build the Release binary at the task's base commit into build/base (a worktree), then:
for s in scenarios/*/*/scenario.yaml; do
  for c in validate explain estimate run; do
    build/base/simsys $c $s > /tmp/p3/before/$(basename $(dirname $s)).$c 2>&1; echo $? >> /tmp/p3/before/$(basename $(dirname $s)).$c
    build/gcc14-release/simsys $c $s > /tmp/p3/after/$(basename $(dirname $s)).$c 2>&1; echo $? >> /tmp/p3/after/$(basename $(dirname $s)).$c
  done
done
diff -r /tmp/p3/before /tmp/p3/after && echo identical
```
plus `run --report json` and `sweep` on hello, and `ctest` in both presets (the same test count as before, every name unchanged).

### Task 1: `explain` out of `report.cpp`

**Files:** create `src/report/explain.cpp` (and `src/report/detail.hpp` or the partition); modify `src/report/report.cpp`, `CMakeLists.txt`.

- [x] Tests: none new (a move). Red state: none. Acceptance is the byte-identity check, `ctest` 1163 both presets, warning-free, clang-format clean, the Release link clean (fourth defect: a helper now instantiated in two units may need an `emit_special_members` line) and no "multiple definition" (fifth defect: any lambda handed to a range adaptor in moved code becomes a named `detail::` function). **Done:** 1,676 lines moved into `explain.cpp` (the explain sections, 1,672 lines, and `render_explain`), 153 into the partition; `report.cpp` 3,249 → 1,433 lines. Sharing: the module implementation partition `simsys.report:shared` (`src/report/shared.cppm`), not the header: GCC 14.2 builds and links it in both presets with no workaround (no `emit_special_members` line, no new `detail::` function: the moved code hands no lambda to a range adaptor). It defines the units, the number formatters, `zone_name` and the aligned table, and declares the three helpers whose bodies need the exported `format_duration` (no partition can import its own module's interface): `format_duration(SimTime)` (defined in `report.cpp`), `clock_or_duration` and `window_clock` (in `explain.cpp`). Byte-identical on all 14 scenarios × validate/explain/estimate/run/run --report json and the hello sweep; 1163 tests, names unchanged, both presets.
- [x] Commit `report: explain in its own implementation unit`.

### Task 2: the loader's readers per concern

**Files:** create `src/config/{header,nodes,steps,workload,links,slos,overrides}.cpp`; modify `src/config/config.cpp`, `CMakeLists.txt`.

- [ ] Tests: none new. Acceptance as Task 1; additionally `tests/unit/test_config.cpp` (4,338 lines, every refusal message) passes unchanged, which is the proof no message moved.
- [ ] Commit `config: the readers split per concern`.

### Task 3: `graph.cppm`'s definitions into `graph.cpp`

**Files:** create `src/engine/graph.cpp`; modify `src/engine/graph.cppm`, `CMakeLists.txt`.

- [ ] Tests: none new. Acceptance as Task 1; measure and record the rebuild time of `engine.cppm` after a one-line change in a `Graph` member body before and after (the point of the move), and the BMI size of `simsys.graph`.
- [ ] Commit `graph: declarations in the interface, definitions in graph.cpp`.

### Task 4: `test_theory.cpp` into nine files with a fixtures module

**Files:** create `tests/support/fixtures.cppm`, the nine `tests/theory/test_*.cpp`; modify `tests/CMakeLists.txt` (a `simsys_test_support` module library, the nine sources in `theory_tests`), `tests/unit/test_report.cpp` and `test_series.cpp` (use the module's `explain_of`/`piped` instead of their copies), `tests/support/scenario_helpers.hpp` (what moves into the module), delete `tests/theory/test_theory.cpp`.

- [ ] Tests: the same 1163 cases by name (`ctest -N` before and after, sorted, identical); no test renamed; the suite's Debug wall time at `-j8` not longer than before.
- [ ] Commit `tests: theory tests split by topic; shared builders in a fixtures module`.

- [ ] **3a push.** CI green on both presets; record here the measured rebuild gain of Task 3.

---

## 3b — Faults, failure steps, retries, replications (Tasks 5–12)

### Task 5: the `faults` list, typed and validated

**Files:** `src/model/model.cppm` (`FaultKind {node_down, capacity, latency_spike, cache_flush, traffic_spike, dc_down}`, `Fault`, `Scenario::faults`), `src/config/faults.cpp` (new; `read_faults` replacing `check_faults_is_list`), `src/report/explain.cpp` (`faults: 3 scripted (node_down chat_servers 21:30 for 1m, …)`), tests in `test_config.cpp`, `test_model.cpp`, `test_series.cpp` (chat-system's explain line).

- [ ] Tests (loader; every message verbatim from spec §4.1): each kind loads with its keys; `at` as a clock time (`21:30`) or a duration (`1h30m`) inside the run, else `faults[0]: at must fall inside the run (24h)`; `for` positive and required except for `cache_flush` (`faults[2]: cache_flush takes no for`); `node` a declared node (`faults[0]: node 'x' is not declared`), a client only for `traffic_spike` (`faults[0]: node_down cannot name a client (clients emit, they do not serve)`), a cache or cdn for `cache_flush`; `replicas` only with `node_down`, a positive count at most the node's replicas, `primary` only on a sql_db; `factor` required by `capacity` (in (0, 1): `faults[3]: capacity factor must be between 0 and 1 (a gain is the scenario's capacity)`) and `traffic_spike` (positive); `extra` a positive duration for `latency_spike`; `datacenter` declared for `dc_down`; `kind: partition` → `faults[0]: partition is not modelled in phase 3 (see the backlog)`; unknown keys refused like every map. Model: `Fault` fields and `Scenario::faults` default empty; a scenario without `faults` loads as before. Explain: the line above on chat-system; `faults: none` is not printed.
- [ ] Commit `config: the faults list, typed`.

### Task 6: `FaultSchedule` and the conditions the engine applies

**Files:** create `src/engine/faults.cppm` (`Condition`, `FaultSchedule`, `FaultRecord` skeleton); modify `src/engine/engine.cppm` (`Simulation` owns a schedule; `forward_pass` asks `condition_at(node, now)` and hands it to the node before its step), `src/engine/behaviours.cppm` (`ServiceBehaviour` and every M/M/c-based behaviour take `capacity_factor` and `extra_latency` into their `FluidQueue::set_service`/capacity for the step; `ClientBehaviour` multiplies its rate by `rate_factor`; the hit-ratio factor waits for Task 7), `src/report/report.cpp` (`estimate` prints `faults: 3 scripted (estimate shows the steady state without them; run applies them)`), `CMakeLists.txt`, tests in a new `tests/theory/test_faults.cpp` (registered in `theory_tests`) and `tests/unit/test_engine.cpp`.

- [ ] Tests (theory; one M/M/1 probe: client 400/s → service capacity 1k/s, latency 1 ms, `queue_limit: 100`, step 100 ms, zero-latency link):
  - `node_down` for 10 s from 00:10: during the fault the service's drain is **0/s**, its backlog **100** (the room), dropped **400 × 10 − 100 = 3900** requests over the fault (the first 0.25 s fill the room: 100 / 400), the client's answered **0/s**; after the fault the backlog drains in **100 / (1000 − 400) = 0.1667 s** (two steps at 100 ms: 60 then 0) and the op's mean latency is back at the M/M/1 value `1 / (1 − 0.4) = 1.667 ms` by the third step after the fault clears (the recovery pin: within 1 % of the step before the fault).
  - `node_down` with `replicas: 1` of 4 on a `replicas: 4` service: capacity **× 0.75**, rho **0.4 / 0.75 = 0.5333** during the fault (closed form from `mmc_wait` with c = 3 of 4).
  - `latency_spike` `extra: 1ms`: service time 2 ms, capacity **500/s**, rho **0.8**, M/M/1 mean **2 / (1 − 0.8) = 10 ms** (from 1.667 ms); `capacity` `factor: 0.5`: capacity 500/s with the service time still 1 ms, rho 0.8, mean **1 / (1 − 0.8) = 5 ms** (the two kinds differ by exactly the service time: pin both).
  - `traffic_spike` `factor: 3` for 10 m on the client equals, byte for byte, the same scenario with `spikes: [{at: …, for: 10m, factor: 3}]` on its rate curve (run JSON identical).
  - two faults on one node overlap: factors multiply (`capacity` 0.5 twice → 0.25), extra latencies add.
  - a scenario with faults twice → byte-identical run; without faults → byte-identical to the binary before this task (the identity condition).
  - `estimate` ignores faults and prints the note; `dc_down` is refused for now with `dc_down lands in Task 7` (a stub refusal in `Simulation::create`, replaced in Task 7).
- [ ] Tests (unit, `FaultSchedule`): `condition_at` is the identity outside every fault, a fault is active on `[at, at + for)`, overlapping faults compose, a fault on replica `r` of `R` gives `(R − r) / R`.
- [ ] Commit `engine: scripted faults as per-step conditions on capacity, latency and rate`.

### Task 7: cache flush and `dc_down`

**Files:** `src/engine/faults.cppm` (`hit_ratio_factor` from the warm-up curve: at `t` after the flush the cache holds the keys of `rate × t` requests; `caching::` Che hit ratio of the smaller of `keys` and the distinct keys seen in `t` under the cache's popularity; complete when the cache is full), `src/engine/behaviours.cppm` (`CacheBehaviour`, `CdnBehaviour` multiply their hit ratio by the factor; a fixed-ratio cache warms linearly to its ratio over its fill time, documented as the simple case), `src/engine/engine.cppm` (`dc_down` = `node_down` on every node of the datacenter and a capacity factor 0 on every link leaving it: a pipe's `passed` is 0; a non-pipe link drops what crosses it, counted on the link), tests in `test_faults.cpp`.

- [ ] Tests: a `che` cache of 10k keys over a Zipf 0.8 of 1M keys at 1k/s (steady hit ratio h from `caching::che_hit_ratio`, the value the test computes): flushed at 00:10, its hit ratio at the next step is **0**; at `t` = 2 s it is the Che hit ratio of a cache of **min(10k, distinct keys of 2k requests)** entries (the test computes the distinct-keys count from the popularity model's coupon-collector sum, which `caching` exports for this); back within 1 % of h once the cache is full, and the fault record says when (*the warm-up time is a model output; the test pins it against the same closed form*). A fixed-ratio cache (`hit_ratio: {fixed: 0.9}`, `keys: 1000`) warms linearly: 0.45 after half its fill time. `dc_down` on a two-datacenter chain: the cross-DC link passes 0 during the fault (link row `dropped_per_s` = its inflow), every node of the datacenter has capacity 0, the client's answered is the share that stays in its own datacenter.
- [ ] Commit `engine: cache_flush warms back along the Che curve; dc_down takes a datacenter's nodes and links`.

### Task 8: the fault block in the report

**Files:** `src/engine/faults.cppm` (`FaultRecord` filled by the engine each step: lost, backlog peak, SLO verdicts during; the follow-on (reconnect storm: Task 15, warm-up time, extra attempts: Task 10) and the recovery time: the first step after the fault clears at which every client op's p99 and every node's utilisation are within 1 % of the step before `at`), `src/report/report.cppm`, `report.cpp` (the three-line block per fault after the SLO table; JSON `faults: [{at_s, kind, node, for_s, during: {...}, after: {...}, recovered_at_s | null}]`), tests in `test_report.cpp` and `test_faults.cpp`.

- [ ] Tests: on Task 6's `node_down` probe the block reads `fault 00:10 node_down svc for 10s` / `during:   offered 400/s, served 0/s, lost 3.9k requests, backlog 100 (room 100 dropped the rest)` / `after:    backlog drained in 0.2s` / `recovery: op p99 back within 1% at 00:20:00.3 (0.3s after the fault cleared)` (the exact strings pinned, numbers from Task 6's derivations); JSON `recovered_at_s` **1200.3**; a fault that never recovers in the run prints `not recovered by the end of the run` and JSON `null`; two faults → two blocks in `at` order; no faults → no block and no `faults` key.
- [ ] Commit `report: one block per fault: what it cost, what followed, when the system recovered`.

### Task 9: `on: failure` steps fire

**Files:** `src/engine/behaviours.cppm` (`HopBlocks`: a failure block carries `1 − d` of the step it follows, sequential, not thinned by that step, thinned by earlier ones), `src/engine/graph.cppm` (the rule "belongs to the nearest unconditional step before it"; an unsized `on: failure` step over a bandwidth link refused like any unsized step: `node 'web': step to 'log' over bandwidth link web → log needs a size (on: failure steps are priced like any step)`), `src/engine/engine.cppm` (the size walk no longer skips failure steps), tests in `test_failed_steps.cpp` (3a's file) and `test_graph.cpp`.

- [ ] Tests: `[{to: db, op: write}, {to: log, op: write, on: failure}]` with `db` delivering 0.5 (capacity 500/s, room 1, offered 1k/s): `log` receives **500/s** from the second step on; with `db` lossless `log` receives **0**; after a lossy step before the pair (`a` delivering 0.5 first): `log` receives **0.5 × 0.5 × 1000 = 250/s**; a failure step after a parallel group reports the group's failure share `1 − Π d`; `estimate` settles at the same numbers; the size-walk refusal message; the schema's `on` row updated in Task 19.
- [ ] Commit `nodes: on: failure steps carry the failed share of the step before them`.

### Task 10: client retries as feedback, `attempts/s`

**Files:** `src/model/model.cppm` (`Workload::retries` count, `retry_after` duration), `src/config/workload.cpp` (`retries` a non-negative count ≤ 10: `node 'u': retries must be at most 10`; `retry_after` a positive duration, default `defaults::retry_after` = 100 ms `// chosen, not from the book`; `retry_after` without `retries` refused), `src/engine/behaviours.cppm` (`ClientBehaviour`: a ring of `retries` buckets per op holding the share to resend `k` backoffs later; `offered = curve + Σ pending`), `src/engine/engine.cppm` (`client_responses`: `attempts_per_s` in `ResponseSample`; answered counts a request once; the failed share of attempt `k` feeds bucket `k + 1`; settle mode damps the feedback with the delivered-fraction scheme), `src/report/*` (`attempts/s` column after `offered/s`; JSON `attempts_per_s`; CSV `attempts_per_s`; `explain` prints `retries=2 after 100ms`), tests in `test_failed_steps.cpp`, `test_report.cpp`, `test_series.cpp` (chain's hash re-pinned for the column).

- [ ] Tests: a client at 1k/s with `retries: 2`, `retry_after: 100ms`, step 100 ms, whose path loses 50 % (a sink at `failure_rate: 0.5`): at the fixed point attempts per request **1 + 0.5 + 0.25 = 1.75** (`attempts/s` **1750**), answered **1 − 0.5³ = 0.875** (875/s), lost **125/s**; the sink's demand **1750/s**; `offered/s` stays **1000**; the first step sends 1000 (no history), the second 1500, the third 1750 (the backoff is one step); with `retries: 0` or no losses `attempts/s = offered/s`; `estimate` settles at the same 1.75 (and at 1 + Σ for a 90 % loss: `1 + 0.9 + 0.81 = 2.71`); a `node_down` for 10 s on the path with `retries: 2`: `attempts/s` rises to the retry storm's **3× the curve** during the fault (every attempt fails), falls back to the curve within `2 × retry_after` after the fault clears (the recovery pin), and the fault block's `after:` line says `retries added 20.0k attempts`; a timed-out request retries like a lost one (the client's `timeout` share feeds the same buckets).
- [ ] Commit `nodes: a client's retries are feedback demand, shown as attempts/s`.

### Task 11: pool retries and the dead-letter count

**Files:** `src/model/model.cppm` (`NodeSpec::retries`, `retry_after` on a `worker_pool`), `src/config/nodes.cpp` (keys; default `retry_after` 1 s `// chosen`), `src/engine/behaviours.cppm` (`WorkerPoolBehaviour`: the `then` step's sink failure share re-published to the pool's own queue after the backoff, up to `retries` times; the last attempt's failures → `dead_lettered_per_s`), `src/engine/engine.cppm` (`NodeSample::dead_lettered_per_s`, totals), `src/report/*` (pool row `dead_lettered/s`, JSON `dead_lettered`, `dead_lettered_per_s`, CSV column; the `drop_rate` SLO ignores dead letters; `explain` prints `retries=3 after 1s, then dead-letter`), tests in `test_sinks.cpp` (3a's file), `test_report.cpp`.

- [ ] Tests: a queue fed 1k/s → pool (100 workers, 10 ms) → sink `failure_rate: 0.02`, `retries: 3`, `retry_after: 1s`, step 100 ms: at the fixed point attempts per job **1 + 0.02 + 0.0004 + 0.000008 = 1.020408**, the sink's demand **1020.4/s**, dead-lettered **1000 × 0.02⁴ = 1.6e-4/s** (JSON to 1e-6 relative), the pool's drop rate **0** and the sink's drops **20.4/s** (the attempts that failed and were retried are the sink's drops, as before; the dead letters are the pool's); the queue's lag includes the re-published jobs; `retries: 0` → phase 2 behaviour byte-identical; the SLO `drop_rate max 0.001` passes on notification-system's shape (pinned on a probe with its numbers: 1.9k/s through four channels, the providers' failure rates).
- [ ] Commit `nodes: a worker pool retries a third party's failures and dead-letters the rest`.

### Task 12: `simsys run --replications N`

**Files:** `src/session/session.cppm`, `session.cpp` (`run_replications(const Graph&, seed, N, jobs)` → `std::vector<Summary>` on the sweep's pool: one `Simulation` per seed; `ReplicationSummary` with per-metric median and 10–90 % band and per-SLO `passes_in`), `src/report/report.cppm`, `report.cpp` (the table prints `p99 212ms [198–231]`, `PASS in 9 of 10` with the median run's observed value; JSON `replications: {seeds: [...], per_seed: {...}}`), `src/cli/main.cpp` (`--replications N` (1–1000), `--jobs`; `--series`/`--series-ops` with `--replications` refused: `--series needs a single run (one series file per seed is phase 4 material)`; exit code of the median run), `tests/unit/test_session.cpp` (new), `test_report.cpp`, `tests/cli/replications.cmake` (byte-identical across `--jobs`, the noiseless note).

- [ ] Tests: hello with `noise: {sigma: 0.2}` on its client and `--replications 5`: five different `users.ping.mean` values (the seeds differ), the printed median equals the sorted third value, the band the 10 % and 90 % quantiles by the report's quantile rule (nearest-rank, pinned), `--jobs 1` and `--jobs 4` byte-identical; a scenario without noise prints `replications: the scenario has no noise, so every seed gives the same run` and runs once; `--replications 1` is the plain run byte for byte; an SLO failing in 2 of 5 seeds prints `PASS in 3 of 5`; `--replications 0` and `1001` refused.
- [ ] Commit `cli: run --replications N repeats a run over seeds of the rate noise and prints bands`.

- [ ] **3b push.** CI green; the ten runnable scenarios byte-identical to the 3a push except the new columns (`attempts/s`, `dead_lettered/s`) and the chain hash.

---

## 3c — The four node types and their scenarios (Tasks 13–18)

### Task 13: `service_discovery`

**Files:** `src/engine/behaviours.cppm` (`ServiceDiscoveryBehaviour`: a service with the type's defaults 50k/s, 0.5 ms; answers `lookup`), `src/engine/graph.cppm` (`lookup_before(op, target)`: an op whose step list has a `discovery.lookup` step before its step to `target`), `src/engine/engine.cppm` (during a `node_down` of `r` of `R` replicas of a looked-up target, for `failover` seconds the share `r / R` of that op's requests is lost at the target; `make_node` case), `src/model/model.cppm` (`NodeSpec::failover`), `src/config/nodes.cpp` (`failover` a positive duration, default 10 s `// chosen`), `src/defaults/defaults.cppm`, `src/report/explain.cpp` (`failover=10s`), tests in `test_service_sugar.cpp`, `test_graph.cpp`, `test_faults.cpp`.

- [ ] Tests: a client → api (`[{to: disc, op: lookup}, {to: svc, op: call}]`), svc `replicas: 4`, `node_down` `replicas: 1` for 30 s, `failover: 10s`: for the first 10 s of the fault the client's lost share is **0.25** (250/s of 1k/s) on top of what the 3 survivors drop, from 10 s on it is **0** (the survivors at rho × 4/3), and the fault block's `after:` line says `failover 10s: 2.5k requests sent to the dead replica`; without the lookup step the lost share is 0 throughout (the balancer spreads it); `lookup` latency adds 0.5 ms + 2 × link to the op's mean (closed form on a zero-load path); `failover` refused on other types.
- [ ] Commit `nodes: service_discovery answers lookups and holds callers on a dead replica for failover`.

### Task 14: `ws_gateway`: connections as a level

**Files:** `src/engine/behaviours.cppm` (`WsGatewayBehaviour`: the level `C`, connects from a `connect` op reaching the gateway or a fixed `connections`, disconnects `C / session`, the cap `max_connections × replicas` with the excess rejected, heartbeat load `C / heartbeat` on the `heartbeat` op, `fanout` as a factor on a message op's `then` steps, `deliver` terminal), `src/model/model.cppm` (`max_connections`, `heartbeat`, `fanout`, `connections`, `session`, `reconnect_within`), `src/config/nodes.cpp` (keys and refusals: a gateway with neither a `connect` op reaching it nor `connections` → `node 'gw': a ws_gateway needs a client op named 'connect' or a fixed 'connections'`; both → refused; `fanout` ≥ 1; `heartbeat`, `session`, `reconnect_within` positive durations), `src/defaults/defaults.cppm` (`session` 30 min, `reconnect_within` 30 s, both `// chosen`), `src/engine/engine.cppm` (`NodeSample::connections`; `make_node`), `src/report/*` (`connections` peak/end in the gateway row and JSON; CSV `connections`), `src/report/explain.cpp` (the gateway's line with the level's steady value `C* = connect rate × session`, the heartbeat load, the cap), tests in a new `tests/theory/test_gateways.cpp`, `test_graph.cpp`, `test_config.cpp`.

- [ ] Tests: a client with `ops: {connect: 0.5, send: 0.5}` at 200/s (connect 100/s) → gateway `session: 1000s`, `heartbeat: 5s`, `max_connections: 200k`, `replicas: 1`: the level follows `C' = 100 − C / 1000`, so `C(t) = 100k (1 − e^{−t/1000})`: **63.2k** at t = 1000 s, **100k** at the fixed point (within 1 % at 4600 s); the heartbeat load is **C / 5** (20k/s at the fixed point) on the gateway's `heartbeat` op (visible as its demand); with `max_connections: 50k` the level stops at **50k** and connects are rejected at **100 − 50k / 1000 = 50/s** (`rejected_per_s`); `connections: 2M` fixed → C is 2M from the first step, heartbeat 60 s → **33.33k/s**; a `send` op with `then: [{to: q, op: publish}]` and `fanout: 1.4` sends `q` **1.4 ×** the send rate; `deliver` ops make no hops; `estimate` reports the fixed-point level; the explain line.
- [ ] Commit `nodes: ws_gateway holds connections as a level and fans messages out`.

### Task 15: the reconnect storm; chat-system runs end to end

**Files:** `src/engine/behaviours.cppm`, `faults.cppm` (a `node_down` of `r / R` of a gateway drops `C × r / R` connections; they reconnect evenly over `reconnect_within` from the fault's end: `connects += dropped / reconnect_within` per second for that long; messages to dropped connections are lost; `FaultRecord::after` = the storm's size and the gateway's peak utilisation), `scenarios/book/chat-system/{scenario.yaml,README.md}` (the client gains `connect` in its ops or the gateway a `connections` level: decide from the book's 50M DAU and the README's reasoning, document it; sizing so the day passes its three SLOs with the 21:30 fault as the lesson: the storm's size, the gateway's peak, `send` p99 recovery time, a sweep over `reconnect_within` 10 s..60 s; `push` at 945 % and `lb` at 90 % resolved or explained), `tests/scenario/test_scenarios.cpp` (chat-system leaves the refused list; a day test at a coarser step pins the storm's size `C × 1` for the whole tier, the peak and the recovery within the fault block's numbers), `tests/unit/test_series.cpp`, `README.md`.

- [ ] Tests (theory, on Task 14's probe at the fixed point 100k): `node_down` of the gateway for 60 s from 00:30 with `reconnect_within: 30s`: during the fault `C` falls to **0** (every connection dropped at once: the level is 0 while capacity is 0) and `send` is lost; after it connects run at **100 + 100k / 30 = 3433/s** for 30 s, so the gateway's `connect` demand peaks at **34.3×** its steady value and `C` is back at 100k (minus the 30 s of disconnects, **≈ 97k**, closed form) when the storm ends; the fault block's `after:` line reads `reconnect storm 100k connections over 30s, gw peak 343% at 00:31:00`; with `reconnect_within: 300s` the peak is 4.3× (the sweep's point).
- [ ] Scenario: `simsys run scenarios/book/chat-system/scenario.yaml` exits 0 (or the exit the README states on purpose); README numbers *measured*.
- [ ] Commit `nodes: a gateway's node_down drops its connections and they storm back; chat-system runs end to end`.

### Task 16: google-drive runs end to end

**Files:** `scenarios/book/google-drive/{scenario.yaml,README.md}` (`long_poll` with `connections: 2M`, `heartbeat: 60s`, `fanout: 1`; `backup_workers` sized or kept as the lesson (today rho 1.28 at the peak: the README decides and says why); download p99 vs 2 s, metadata p99 vs 100 ms, drop rate), `tests/scenario/test_scenarios.cpp`, `test_series.cpp`, `README.md`.

- [ ] `run` exits 0; README numbers *measured*; the day test at a coarser step pins the headline peaks and the SLO verdicts.
- [ ] Commit `scenarios: google-drive runs end to end`.

### Task 17: `notification_fanout`; notification-system runs end to end

**Files:** `src/engine/behaviours.cppm` (`NotificationFanoutBehaviour`: a service (20k/s, 1 ms) whose `then` steps fire per routed notification × `1 − dedup`; `deduplicated_per_s`), `src/engine/graph.cppm` (`channels` ↔ `then` targets that are queues: `node 'fanout': channel 'sms' names sms_q, which no then step publishes to` / `… then step publishes to email_q, which no channel names`), `src/model/model.cppm`, `src/config/nodes.cpp` (`dedup` in [0, 1), `channels` a map of names to declared message queues), `src/engine/engine.cppm` (`NodeSample::deduplicated_per_s`; `make_node`), `src/report/*` (`deduplicated/s`, JSON; explain follows a fanout's `then` and the `load not propagated` mark for it goes), `scenarios/book/notification-system/{scenario.yaml,README.md}` (pool `retries: 3` on the four channel pools, the 09:00 blast ×20 through the queues, `user_db` 173 % and `notification_svc` 140 % sized or explained, dead letters vs drops), `tests/theory/test_queues_pools.cpp`, `test_graph.cpp`, `tests/scenario/test_scenarios.cpp`, `test_series.cpp`, `README.md`.

- [ ] Tests: a fanout at 1k/s with `dedup: 0.02` and `then: [{to: q1, factor: 0.6}, {to: q2, factor: 0.3}]`: `q1` receives **588/s**, `q2` **294/s**, `deduplicated/s` **20**; the channels checks' messages; the explain lines on notification-system no longer carry `load not propagated`; the day test pins the blast's queue lags and that the `drop_rate` SLO passes with retries while the dead-letter rate is *measured*.
- [ ] Commit `nodes: notification_fanout routes per channel after dedup; notification-system runs end to end`.

### Task 18: `frontier`; web-crawler runs end to end

**Files:** `src/engine/behaviours.cppm` (`FrontierBehaviour`: a message queue without retention whose consumer pool drains at `min(pool capacity, hosts × politeness)`; `capacity` the prioritiser's own rate in front), `src/engine/engine.cppm` (`connect_consumer_groups` includes a frontier's pools; `queue_lag` on a frontier evaluated; the `internet` sink's `politeness` capped over the one frontier's `hosts`: `node 'internet': politeness needs exactly one frontier in the scenario (found 0)`), `src/model/model.cppm` (`hosts`), `src/config/nodes.cpp` (`hosts` a positive count; `politeness` positive), `src/report/*` (explain follows a frontier's `then`; the last `load not propagated` mark goes), `scenarios/book/web-crawler/{scenario.yaml,README.md}` (`downloader` `retries: 2` for the internet's 5 % failures; the lesson: the frontier's lag under the ×50 link fan-out at 10 % new URLs, politeness as a cap by lowering `hosts`), `tests/theory/test_queues_pools.cpp`, `tests/scenario/test_scenarios.cpp`, `test_series.cpp`, `README.md`.

- [ ] Tests: seeds 50/s → frontier (`hosts: 1000`, `politeness: 1/s`, `capacity: 5k/s`) → pool (2000 workers, 500 ms = 4000/s): the drain is capped at **1000/s**; with seeds at 1500/s the backlog grows at **500/s** and the lag after 60 s is **30k / 1000 = 30 s**; with `hosts: 10M` the cap is 10M/s and the pool's capacity binds; the url_seen feedback at a fixed point: seeds 50/s, ×50 links, 10 % new → the frontier's inflow at the fixed point is **50 / (1 − 50 × 0.1 × d)** where d is the downloader → parser → extractor delivered chain (unbounded when 50 × 0.1 × d ≥ 1: the README's gain-above-one lesson, `explain` says "no steady state"; with `factor: 50` and 90 % seen the gain is 5 × d, so the shipped file is unbounded unless the pool's cap binds: the test pins the bounded case with `factor: 8` and the unbounded verdict with 50); the sink refusal message; `queue_lag` on the frontier evaluated.
- [ ] Scenario: `run` exits 0 or the exit the README states; numbers *measured*.
- [ ] Commit `nodes: frontier drains under per-host politeness; web-crawler runs end to end`.

- [ ] **3c push.** Twelve scenarios run; CI green.

---

## 3d — Docs and the phase review

### Task 19: Docs

- [ ] Schema **v1.5**: "New in v1.5"; "What runs" (fourteen types, twelve scenarios, exit codes); the `faults` table (§4.1 of the spec verbatim with the refusal messages); `retries`/`retry_after` on clients and pools; `on: failure`; the four types' sections with their keys; `connect` ops and `connections`; `--replications`; the fault block, `attempts/s`, `dead_lettered/s`, `deduplicated/s`, `connections`; the JSON key table extended; the CSV columns; one section "Defaults chosen, not from the book" listing `retry_after`, pool `retry_after`, `session`, `reconnect_within`, `failover` with the README sweeps that show their effect.
- [ ] Spec **§20**: the rules as finally implemented with each review's corrections; §7's rows for the four types revised; §9.3 says what `--replications` does and that random faults wait; §13's row closed.
- [ ] Study notes: whatever new constructs the tasks introduce (first-use comments list them; `docs/study/small-features.md` gains sections as needed); `numerics.md` gains the retry series, the connection level's exponential approach and the cache warm-up curve; `modules.md` gains the 3a layout (implementation units, the shared-helper choice).
- [ ] Root README: phase 3 status, twelve runnable scenarios, a fault example with its block, `--replications` example, the test count.
- [ ] Commit `docs: schema v1.5, spec §20 phase 3, study notes, README for the twelve scenarios`.

## Comprehensive phase 3 review, then push

- [ ] An adversarial reviewer (fresh context, own worktree, both presets) checks: warning-free builds and the whole suite; clang-format as CI runs it; conservation from `--series` on all twelve scenarios (node, link and client rows, `attempts`, `dead_lettered`, `deduplicated`, `connections` included); answered ≤ offered and attempts ≥ offered everywhere; `estimate` = `run` on steady probes with retries, failure steps, fanouts, frontiers and gateways; every fault kind's identity when absent (phase 2 bytes); every README number of the four new scenarios and of the eight that moved; `--replications` byte-identical across `--jobs`; the 3a splits' byte-identity still holds; first-use comments; `Pattern:` labels; test names; functions over ~50 lines; the five GCC workarounds; the schema's "chosen" list complete.
- [ ] Fix round recorded here as "Comprehensive phase 3 review at <sha>"; commit; push; CI.

## Phase 4 backlog (left out of phase 3)

- kv_cluster partitions (`mode: ap|cp`, sloppy quorum, hinted handoff, read repair) and the `partition` fault kind.
- Random faults (`mtbf`/`mttr`, chaos mode) as a second generator feeding `FaultSchedule`.
- Queue partitions capping a consumer group's parallelism; request/reply over a queue; a worker blocked on its `then` hops.
- A large read's last byte; batch arrivals; full-duplex links.
- Sweep SLO columns and `reject_rate`; sweeps over seeds or two keys; geometric spacing; `estimate --at`.
- One series file per seed under `--replications`.
- Cutting node cycles at the asynchronous queue → pool edge (unblocked by phase 2's C-1).
- The 2a leftovers still open: M7 unit labels with a `cost`, M9 duplicate `write_ops`, M10 the diurnal table, M12 `estimate`'s table shape, M13 `curve --seed` without `--noise`.

## Self-review

- **Spec coverage.** §3 sub-phases → Tasks 1–4, 5–12, 13–18, 19. §4 faults → Tasks 5–8 (§4.1 schema in 5, §4.2 model in 6, §4.3 flush in 7, §4.4 report in 8). §5 failure steps → 9. §6.1 client retries → 10, §6.2 pool retries → 11. §7 replications → 12. §8.1–8.4 → 13, 14–15, 17, 18. §9 scenarios → 15, 16, 17, 18. §10 report/schema → each task's report bullet and 19. §11 tests → the bullets above. §12 study notes → 19.
- **Placeholder scan.** Every task names files and tests with numbers or a closed form; scenario numbers are marked *measured* on purpose (the binary produces them). No "TBD".
- **Name consistency.** `Condition`, `FaultSchedule`, `FaultRecord` (6, 8, 10, 15); `attempts_per_s` (10, 12, review); `dead_lettered_per_s` (11, 17); `connections` (14, 15, 16); `lookup_before` (13); `read_faults` (5). Existing names checked at `0e37886`: `Simulation::forward_pass`, `client_responses`, `give_up_after`, `delivered_of`, `connect_consumer_groups`, `make_node`, `ResponseSample::delivered_response`, `NodeSample`, `StepRecord`, `HopBlocks`, `ClientBehaviour`, `WorkerPoolBehaviour`, `CacheBehaviour`, `CdnBehaviour`, `FluidQueue::set_service`, `report::Summary::observe`, `session::run_summary`, `RateCurve::spikes`, `caching::che_hit_ratio` (the exact spelling of the Che function is checked by Task 7's test-writer).
