# Phase 2a: Workload Curves, Noise, Op Costs, Load Balancer, Rate Limiter, Cache — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Clients follow configurable rate curves (sine/square/diurnal waves, ramps, scripted spikes, piecewise points) with seeded Ornstein–Uhlenbeck noise; nodes charge per-op costs; three more node types run — `load_balancer`, `rate_limiter`, `cache` (fixed hit ratio) — so that `labs/rate-limiter` runs end to end and the rate-limiter chapter can be taught with real numbers. A `simsys curve` command prints any client's rate curve.

**Architecture:** One new module `simsys.workload` (pure curve maths, the noise process, a `std::generator` sample stream). The loader types the `rate:` block and the new node keys. `ClientBehaviour` gains a per-step `tick` (noise state) and asks the curve for the rate. `ServiceBehaviour` learns op costs. Three new behaviours join the `Behaviour` variant; the graph resolves what they need statically (link-weight splits for any node, hit ratios, pooled/imbalanced downstreams). The engine gains a `rejected_per_s` channel for admission control and a "pooled replicas" queue mode. Phase 2b adds the Che cache model, SQL and CDN; 2c adds blob/queue/workers/external sink and `sweep`.

**Tech Stack:** unchanged (C++23, GCC 14.2, CMake 3.28 modules, Catch2 3.7.1, yaml-cpp, CLI11). New standard facilities: `<generator>` (C++23 `std::generator`, available in libstdc++ 14), `<random>` (`std::mt19937_64`, `std::normal_distribution`), `std::chrono::hh_mm_ss` for clock times.

**Conventions:** as in the phase-1 plan — CLAUDE.md §1–4, TDD with the test-writer / implementer / adversarial-reviewer split, two commits per task, **each concurrent agent in its own build directory, staging by file name only**, tight numeric checks with `WithinRel`/`WithinAbs`, the four GCC 14 workarounds already in the tree (no `<chrono>`/`<format>` in importing interfaces; `std::to_chars` for numbers; `-flarge-source-files`; `-fno-ipa-sra`; `model.cpp` special-member emission — add a line there if Release linking breaks on a new type).

**Spec refinements made here (write into spec §16 in Task 12):**

1. **Simulated time starts at midnight.** `t = 0` is `00:00`; clock times in `at:`/`peak_at:` are offsets from it. A later top-level `start: HH:MM` can shift this; not now.
2. **Rate curve composition:** `rate(t) = points(t)` if `points` is given, else `base`; then multiplied by `wave(t)`, `ramp(t)`, every active `spike`, and `noise`. `ramp: {from, to, over}` is a multiplier `from/base → to/base` linear over `over`, then `to/base` forever.
3. **Noise** is an Ornstein–Uhlenbeck multiplier `1 + x`, floored at 0: `x_{k+1} = x_k·e^{−dt/τ} + σ·sqrt(1 − e^{−2dt/τ})·ξ`, with `σ = noise.sigma` (stationary standard deviation) and `τ = noise.correlation`. One `std::mt19937_64` per client, seeded from `scenario.seed` and the client's index, so runs are byte-identical and clients are independent.
4. **Op cost:** a node key `cost: {op: units}` (default 1 per op) — the capacity units one request of that op consumes. Demand is `Σ rate_op × cost_op`; the queue's composition is kept in units and converted back to requests when forwarding.
5. **Load balancer policies in the fluid model:** `round_robin` → the downstream's replicas stay independent copies (N × M/M/1); `least_connections` → the downstream is *pooled* (one M/M/(c·N) queue with N× capacity, the join-the-shortest-queue idealisation); `weighted` → the LB splits each op across its outgoing links by `weight` (same mechanism as client routes); `consistent_hash` with `vnodes: v` → the hottest downstream replica sees `1 + 1/sqrt(v)` times the average (chosen; the Dynamo/Chord literature gives the load's standard deviation shrinking like `1/sqrt(v)`), so the downstream's utilisation and wait use that replica.
6. **Rate limiter in the fluid model:** every algorithm keeps the state ch. 4 describes, stepped once per step `[now, now + dt)` (`route(in, now, dt)` for every behaviour); W = `limit × window` (window 1 s by default, key `window`). `token_bucket`: level `L ∈ [0, burst]`, `admitted = min(in, limit + L/dt)`, `L += (limit − admitted)·dt`, clamped. `fixed_window`: `used` resets when `floor(now/window)` changes; admits `min(in·dt, W − used)/dt`. `sliding_log`: a ring of the last `ceil(window/dt)` steps' admitted counts; admits `min(in·dt, W − Σ previous steps)/dt` (never more than W in any window). `sliding_counter`: `estimate = prev·(1 − f) + cur`, `f = (now mod window)/window`; admits up to `W − estimate`. `leaky_bucket`: the limiter's queue is one deterministic leak slot (service time `1/limit`, concurrency 1, SCV 0: M/D/1) with `queue_limit = burst`; requests wait, a full bucket's overflow is rejected; `capacity`/`latency`/`queue_limit`/`concurrency` on it are errors. `burst` applies to the two buckets only (an error on a windowed algorithm). Rejected flow is the channel `rejected_per_s`, never forwarded, reported apart from drops (`reject_rate`). The `backend` hop (counter store) gets one `counter` request per CHECKED request (the limiter's whole inflow) and adds its latency to an admitted request's response. `estimate` uses each algorithm's long-run rate `min(in, limit)`. Sub-step effects (a boundary inside a step, a window shorter than a step) are not resolved.
7. **Cache (2a scope):** `hit_ratio: {fixed: h}` only; `eviction` is accepted and only matters for `che` (2b). Reads vs writes: new key `write_ops: [..]` (default `[write]`), everything else is a read. `cache_aside`: the cache serves hits and reports misses; the *service* step `{to: db, on: miss}` fires for the miss fraction of the preceding cache step. `read_through`: the cache itself forwards misses to `backend` (op unchanged) and the miss latency includes the backend; `write_through`: writes go to the backend synchronously; `write_around`: writes go to the backend and are not cached (same flow, documented difference is in hit ratio, which is fixed here); `write_back`: 2b. A cache has its own capacity/latency (hits cost 1 unit, a miss costs 1 unit plus the hop).
8. **Rejections and the client**: rejected and dropped requests are not answered and contribute no latency. The backward pass carries a *delivered fraction* per (node, op) next to the latency: a node's own `served / inflow` (a limiter's admitted/offered, a queue's drain/demand, capped at 1) × its downstream's (sequential hops multiply, splits mix by share); `ResponseSample.answered = offered × delivered`, and `Summary` weights each op's latency moments by `answered`. `drop_rate` per node is dropped/demand only (failures); `reject_rate` is rejected/demand (policy), an SLO metric of its own.

---

## File structure

| Path | Responsibility |
|---|---|
| `src/core/core.cppm` | *modify*: `parse_clock_time("20:00")` → `SimTime` since midnight |
| `src/workload/workload.cppm` | Module `simsys.workload`: `Wave`, `Ramp`, `Spike`, `Points`, `RateCurve::rate_at(t)`, `NoiseProcess`, `samples()` generator |
| `src/model/model.cppm` | *modify*: `Workload` gains `curve: RateCurveSpec`, `noise`; `NodeSpec` gains `cost`, `write_ops`, `policy`, `vnodes`, `algorithm`, `limit`, `burst`, `window`, `backend`, `hit_ratio`, `strategy` |
| `src/config/config.cpp` | *modify*: typed `rate:` block, clock times, new node keys; export of metric/direction name tables moved to model (backlog item) |
| `src/engine/graph.cppm` | *modify*: splitter routes for any node with weighted outgoing links; `pooled`/`imbalance` resolved on downstream nodes; static hit ratio per cache; `backend` edges |
| `src/engine/behaviours.cppm` | *modify*: `ClientBehaviour::tick/offer` with curve+noise; `ServiceBehaviour` costs; new `LoadBalancerBehaviour`, `RateLimiterBehaviour`, `CacheBehaviour`; `Routed` gains `rejected`; `NodeBehaviour` gains `cost(op)` |
| `src/engine/engine.cppm` | *modify*: composition in units; `rejected_per_s`; pooled-queue mode; imbalance factor; client `tick` |
| `src/report/report.cppm/.cpp` | *modify*: `rejected` column, drop rate includes rejections, explain prints curves/policies |
| `src/cli/main.cpp` | *modify*: `curve <file> <client> [--step D] [--until D]` |
| `tests/unit/test_workload.cpp`, `test_units.cpp`, `test_config.cpp`, `test_graph.cpp`, `test_behaviours.cpp`, `test_report.cpp`; `tests/theory/test_theory.cpp`; `tests/scenario/test_scenarios.cpp` | Tests |
| `scenarios/labs/rate-limiter/README.md` | *modify*: numbers from real runs |
| `docs/scenario-schema.md`, spec §16, `docs/study/{generator,random}.md`, `README.md` | Docs |

Build/test commands and commit style as in the phase-1 plan.

---

## Part A — Workload

### Task 1: Clock times in `simsys.core`

**Files:** modify `src/core/core.cppm`, `tests/unit/test_units.cpp`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("parse_clock_time reads HH:MM as an offset from midnight") {
  REQUIRE(simsys::parse_clock_time("00:00").value() == 0s);
  REQUIRE(simsys::parse_clock_time("20:00").value() == 20h);
  REQUIRE(simsys::parse_clock_time("09:30").value() == 9h + 30min);
  REQUIRE(simsys::parse_clock_time("23:59").value() == 23h + 59min);
}

TEST_CASE("parse_clock_time rejects malformed and out-of-range times with a structured error") {
  require_rejected(simsys::parse_clock_time("24:00"), "24:00", "hour must be 00-23");
  require_rejected(simsys::parse_clock_time("12:60"), "12:60", "minute must be 00-59");
  require_rejected(simsys::parse_clock_time("1200"), "1200", "expected HH:MM");
  require_rejected(simsys::parse_clock_time("12:0"), "12:0", "expected HH:MM");
}

TEST_CASE("parse_time_point accepts a clock time or a duration") {
  // `at:` and `peak_at:` take either form; both yield an offset from midnight (t = 0).
  REQUIRE(simsys::parse_time_point("12:00").value() == 12h);
  REQUIRE(simsys::parse_time_point("5m").value() == 5min);
  require_rejected(simsys::parse_time_point("noon"), "noon", "expected HH:MM or a duration");
}
```

- [ ] **Step 2: Run, expect `parse_clock_time` undeclared**
- [ ] **Step 3: Implement** in `core.cppm` next to `parse_duration`: `[[nodiscard]] std::expected<SimTime, ParseError> parse_clock_time(std::string_view)` (exactly `DD:DD`, `std::from_chars` on both halves; use `std::chrono::hh_mm_ss` only in a comment — it is for the other direction) and `parse_time_point` (try clock time when the text contains `:`, else `parse_duration`; the error text as pinned). Comment: simulated time starts at midnight (refinement 1).
- [ ] **Step 4: Tests pass. Step 5: Commit** `core: clock times as offsets from midnight`

---

### Task 2: Deterministic rate curve terms

**Files:** create `src/workload/workload.cppm`, `tests/unit/test_workload.cpp`; CMake registration.

- [ ] **Step 1: Write the failing tests**

```cpp
// The rate curve a client follows: a base rate shaped by multiplicative terms (spec §8).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <vector>

import simsys.core;
import simsys.workload;

using namespace std::chrono_literals;
using namespace simsys::literals;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using simsys::Rate;
using simsys::workload::Points;
using simsys::workload::Ramp;
using simsys::workload::RateCurve;
using simsys::workload::Spike;
using simsys::workload::Wave;
using simsys::workload::WaveShape;

TEST_CASE("a bare base rate is constant", "[workload]") {
  const RateCurve c{.base = Rate{100}};
  REQUIRE(c.rate_at(0s).per_second == 100);
  REQUIRE(c.rate_at(23h).per_second == 100);
}

TEST_CASE("a sine wave peaks at peak_at and troughs half a period later", "[workload]") {
  // 1 + A cos(2 pi (t - peak) / P): the peak is base x (1 + A), the trough base x (1 - A).
  RateCurve c{.base = Rate{1000}};
  c.wave = Wave{.shape = WaveShape::sine, .period = 24h, .amplitude = 0.5, .peak_at = 20h};
  REQUIRE_THAT(c.rate_at(20h).per_second, WithinRel(1500.0, 1e-12));
  REQUIRE_THAT(c.rate_at(8h).per_second, WithinRel(500.0, 1e-12));
  REQUIRE_THAT(c.rate_at(14h).per_second, WithinRel(1000.0, 1e-9));  // quarter period: the mean
}

TEST_CASE("a square wave is high for the half period centred on the peak", "[workload]") {
  RateCurve c{.base = Rate{1000}};
  c.wave = Wave{.shape = WaveShape::square, .period = 24h, .amplitude = 0.5, .peak_at = 12h};
  REQUIRE(c.rate_at(12h).per_second == 1500);
  REQUIRE(c.rate_at(6h).per_second == 1500);        // 12h - 6h: the edge belongs to the high half
  REQUIRE(c.rate_at(5h + 59min).per_second == 500);
  REQUIRE(c.rate_at(0s).per_second == 500);
}

TEST_CASE("the diurnal shape has its minimum at night and averages one over a day", "[workload]") {
  RateCurve c{.base = Rate{1000}};
  c.wave = Wave{.shape = WaveShape::diurnal, .period = 24h, .amplitude = 1.0, .peak_at = 20h};
  REQUIRE(c.rate_at(4h).per_second < c.rate_at(20h).per_second);
  double sum = 0;
  for (int m = 0; m < 24 * 60; ++m) sum += c.rate_at(std::chrono::minutes{m}).per_second;
  REQUIRE_THAT(sum / (24 * 60), WithinRel(1000.0, 1e-3));  // the table is normalised to mean 1
}

TEST_CASE("a ramp moves linearly from `from` to `to` over `over`, then holds", "[workload]") {
  RateCurve c{.base = Rate{1000}};
  c.ramp = Ramp{.from = Rate{0}, .to = Rate{2000}, .over = 10min};
  REQUIRE(c.rate_at(0s).per_second == 0);
  REQUIRE_THAT(c.rate_at(5min).per_second, WithinRel(1000.0, 1e-12));
  REQUIRE(c.rate_at(10min).per_second == 2000);
  REQUIRE(c.rate_at(3h).per_second == 2000);
}

TEST_CASE("scripted spikes multiply during [at, at + for) and overlapping spikes compound", "[workload]") {
  RateCurve c{.base = Rate{100}};
  c.spikes = {Spike{.at = 5min, .duration = 30s, .factor = 5}, Spike{.at = 5min + 10s, .duration = 10s, .factor = 2}};
  REQUIRE(c.rate_at(4min + 59s).per_second == 100);
  REQUIRE(c.rate_at(5min).per_second == 500);
  REQUIRE(c.rate_at(5min + 15s).per_second == 1000);
  REQUIRE(c.rate_at(5min + 30s).per_second == 100);  // the end is exclusive
}

TEST_CASE("points replace the base with a piecewise-linear rate and hold the last value", "[workload]") {
  RateCurve c{.base = Rate{999}};
  c.points = Points{{{0s, Rate{1000}}, {1h, Rate{5000}}}};
  REQUIRE(c.rate_at(0s).per_second == 1000);
  REQUIRE_THAT(c.rate_at(30min).per_second, WithinRel(3000.0, 1e-12));
  REQUIRE(c.rate_at(2h).per_second == 5000);
}

TEST_CASE("terms multiply: a wave on top of a spike on top of points", "[workload]") {
  RateCurve c{.base = Rate{0}};
  c.points = Points{{{0s, Rate{1000}}, {1h, Rate{1000}}}};
  c.wave = Wave{.shape = WaveShape::sine, .period = 24h, .amplitude = 0.5, .peak_at = 0s};
  c.spikes = {Spike{.at = 0s, .duration = 1min, .factor = 2}};
  REQUIRE_THAT(c.rate_at(0s).per_second, WithinRel(1000.0 * 1.5 * 2, 1e-12));
}

TEST_CASE("time before zero and past 24 h wrap the wave by its period", "[workload]") {
  RateCurve c{.base = Rate{1000}};
  c.wave = Wave{.shape = WaveShape::sine, .period = 24h, .amplitude = 0.5, .peak_at = 20h};
  REQUIRE_THAT(c.rate_at(44h).per_second, WithinRel(1500.0, 1e-12));
}
```

- [ ] **Step 2: Register (`src/workload/workload.cppm` in the module file set after `fluid.cppm`; the test in `unit_tests`), run, expect failure**

- [ ] **Step 3: Implement** — interface (no `<chrono>`: imports `simsys.core`; times via `seconds()`/`.count()`):

```cpp
export namespace simsys::workload {
enum class WaveShape { sine, square, diurnal };
struct Wave { WaveShape shape{WaveShape::sine}; SimTime period{24_h}; double amplitude{0.5}; SimTime peak_at{}; };
struct Ramp { Rate from; Rate to; SimTime over; };
struct Spike { SimTime at; SimTime duration; double factor{1.0}; };
struct Points { std::vector<std::pair<SimTime, Rate>> at; };   // sorted by time
// Spec §8: base x wave x ramp x spikes (x noise, applied by the client); points replace base.
struct RateCurve {
  Rate base;
  std::optional<Points> points;
  std::optional<Wave> wave;
  std::optional<Ramp> ramp;
  std::vector<Spike> spikes;
  [[nodiscard]] Rate rate_at(SimTime t) const;
};
[[nodiscard]] double wave_multiplier(const Wave&, SimTime t);   // exposed for the curve command and tests
}
```
`24_h` does not exist in core's literals (`_ns _us _ms _s`); write `SimTime{86'400'000'000'000}` with a comment or add `_min`/`_h` literals to core in this task (preferred: add `operator""_min` and `operator""_h` to core with tests in `test_units.cpp`). Diurnal: a 24-entry table (one per hour, linear interpolation between hours), normalised to mean 1, with amplitude scaling the deviation from 1: `1 + amplitude × (table(h) − 1)`; the table is "chosen": a textbook consumer-web day — night trough 0.4 at 04:00, morning rise, evening peak 1.6 at 20:00 — document it as a teaching default, not data. Square: high when `|wrap(t − peak)| ≤ P/4` (edges inclusive). Wrap with `fmod` on seconds; negative t allowed.

- [ ] **Step 4: Tests pass. Step 5: Commit** `workload: deterministic rate curve terms (wave, ramp, spikes, points)`

---

### Task 3: Ornstein–Uhlenbeck noise, seeded

**Files:** modify `src/workload/workload.cppm`, `tests/unit/test_workload.cpp`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("noise with sigma zero is exactly one", "[workload]") {
  simsys::workload::NoiseProcess n{simsys::workload::Noise{.sigma = 0.0, .correlation = 5min}, /*seed=*/7};
  for (int i = 0; i < 10; ++i) REQUIRE(n.next(100ms) == 1.0);
}

TEST_CASE("the same seed gives the same noise path; a different seed differs", "[workload]") {
  const simsys::workload::Noise spec{.sigma = 0.2, .correlation = 5min};
  simsys::workload::NoiseProcess a{spec, 7}, b{spec, 7}, c{spec, 8};
  bool any_difference = false;
  for (int i = 0; i < 100; ++i) {
    const double x = a.next(100ms);
    REQUIRE(x == b.next(100ms));
    if (x != c.next(100ms)) any_difference = true;
  }
  REQUIRE(any_difference);
}

TEST_CASE("the noise multiplier is never negative", "[workload]") {
  simsys::workload::NoiseProcess n{simsys::workload::Noise{.sigma = 2.0, .correlation = 1s}, 1};
  for (int i = 0; i < 10'000; ++i) REQUIRE(n.next(100ms) >= 0.0);
}

TEST_CASE("over a long run the multiplier averages one with standard deviation sigma", "[workload]") {
  // Ornstein-Uhlenbeck with stationary sd sigma; sampled every correlation time the values are
  // nearly independent, so 20k samples pin the moments to a few percent.
  simsys::workload::NoiseProcess n{simsys::workload::Noise{.sigma = 0.1, .correlation = 1s}, 42};
  double sum = 0, sum2 = 0;
  const int samples = 20'000;
  for (int i = 0; i < samples; ++i) { const double x = n.next(1s); sum += x; sum2 += x * x; }
  const double mean = sum / samples, var = sum2 / samples - mean * mean;
  REQUIRE_THAT(mean, WithinAbs(1.0, 0.01));
  REQUIRE_THAT(std::sqrt(var), WithinAbs(0.1, 0.01));
}

TEST_CASE("consecutive samples are correlated over the correlation time", "[workload]") {
  simsys::workload::NoiseProcess n{simsys::workload::Noise{.sigma = 0.3, .correlation = 10s}, 3};
  double prev = n.next(100ms), cov = 0, var = 0;
  const int samples = 50'000;
  for (int i = 0; i < samples; ++i) { const double x = n.next(100ms); cov += (x - 1) * (prev - 1); var += (x - 1) * (x - 1); prev = x; }
  REQUIRE(cov / var > 0.95);  // e^{-0.1/10} = 0.990
}
```

- [ ] **Step 2: Run, expect failure. Step 3: Implement**

```cpp
struct Noise { double sigma{}; SimTime correlation{}; };
// Ornstein-Uhlenbeck multiplier (spec §9.2): x_{k+1} = x_k e^{-dt/tau} + sigma sqrt(1 - e^{-2 dt/tau}) xi,
// multiplier = max(0, 1 + x). Mean-reverting Gaussian noise: bursts last about `correlation`.
// C++11 <random>: std::mt19937_64 seeded per client, std::normal_distribution — comment the
// determinism contract (same seed, same path; the engine never shares a generator across clients).
class NoiseProcess { public: NoiseProcess(Noise spec, std::uint64_t seed); [[nodiscard]] double next(SimTime dt); private: ... };
```
Note on `std::normal_distribution`: libstdc++ caches a second value (Box–Muller); the determinism test covers it. `sigma == 0` returns exactly 1.0 without drawing.

- [ ] **Step 4: Tests pass. Step 5: Commit** `workload: seeded Ornstein-Uhlenbeck noise multiplier`

---

### Task 4: `std::generator` sample stream and `simsys curve`

**Files:** modify `src/workload/workload.cppm`, `tests/unit/test_workload.cpp`, `src/cli/main.cpp`, `src/report/report.cpp` (reuse number formatting), `README.md`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("samples yields one rate per step from zero up to but excluding `until`", "[workload]") {
  RateCurve c{.base = Rate{100}};
  c.spikes = {Spike{.at = 300ms, .duration = 100ms, .factor = 3}};
  std::vector<double> got;
  // C++23 std::generator: a lazy coroutine range; the curve is sampled only as far as consumed.
  for (const auto& s : simsys::workload::samples(c, 100ms, 500ms)) got.push_back(s.rate.per_second);
  REQUIRE(got == std::vector<double>{100, 100, 100, 300, 100});
}

TEST_CASE("samples carries the time of each sample", "[workload]") {
  const RateCurve c{.base = Rate{1}};
  auto gen = simsys::workload::samples(c, 1s, 3s);
  auto it = gen.begin();
  REQUIRE(it->t == 0s);
  ++it;
  REQUIRE(it->t == 1s);
}
```
(`samples` returns `std::generator<Sample>` with `struct Sample { SimTime t; Rate rate; }`; the generator header is `<generator>`; it is allowed in this interface's global module fragment? **Check**: `<generator>` includes `<coroutine>` and bits of `<memory>`; it does not include `<format>`/`<chrono>` — verify with the three-file probe pattern before relying on it; if it breaks the BMI, put `samples` in an implementation unit `src/workload/workload.cpp` and declare it in the interface with `std::generator` forward-declared via `<generator>` only there — a coroutine's return type must be complete at the declaration, so if the interface cannot include `<generator>`, expose `for_each_sample(curve, step, until, callback)` instead and keep `std::generator` in the implementation unit for the CLI; document whichever you had to do.)

- [ ] **Step 2: Run, expect failure. Step 3: Implement** `samples` with `co_yield` (C++20 coroutines, C++23 `std::generator`; 3–5 line comment: what a coroutine is, what `co_yield` suspends, why a lazy range fits "print the curve for a day at 1 s steps without allocating 86400 samples"). CLI: `curve <file> <client-id> [--step 1m] [--until <scenario duration>] [--seed N]` printing `t_s,rate_per_s` CSV via the report module's number formatting (`to_chars`); with `--seed` the noise is applied (`NoiseProcess` seeded as the engine would: `seed ^ client_index` — define this in Task 6 and reuse). Reads the client's `Workload::curve` (Task 5 fills it; until then the command reads `base` only — order the tasks so Task 5 lands before this CLI step, or make this task's CLI step the last step of Task 5).

- [ ] **Step 4: Tests pass; `./build/gcc14-debug/simsys curve scenarios/labs/rate-limiter/scenario.yaml clients --step 10s | head`. Step 5: Commit** `workload: lazy sample stream; cli: curve command`

---

### Task 5: Typed `rate:` block in the loader

**Files:** modify `src/model/model.cppm` (`Workload` gains `workload::RateCurve curve` — **dependency**: `simsys.model` would import `simsys.workload`; keep the dependency direction clean: `simsys.workload` must NOT import `simsys.model`, and the curve structs live in `simsys.workload`, which imports only `simsys.core` — so `model` imports `workload`: fine), `src/config/config.cpp`, `tests/unit/test_config.cpp`, `docs/scenario-schema.md` (mark rate keys as read).

- [ ] **Step 1: Write the failing tests** (loader style of the file):

1. "a client's rate curve is typed: wave, ramp, spikes, points, noise": YAML with every term; assert each field (`wave->shape == WaveShape::sine`, `period == 24h`, `amplitude == 0.6`, `peak_at == 20h` from `"20:00"` quoted AND unquoted `20:00` — yaml-cpp reads both as text; `ramp`, two `spikes` with `at: 12:00` and `at: 5m`, `points` sorted, `noise.sigma == 0.15`, `noise.correlation == 5min`).
2. "rate curve values are range-checked": `amplitude: 1.5` → `node 'u': rate.wave.amplitude must be between 0 and 1`; `period: 0s` → `... rate.wave.period must be positive`; `factor: 0` on a spike → `... rate.spikes[1].factor must be positive`; `points` not sorted → `... rate.points must be sorted by time`; `points: []` → `... rate.points must have at least one entry`; `sigma: -1` → `... rate.noise.sigma must be non-negative`; `correlation: 0s` → `... rate.noise.correlation must be positive`; `shape: triangle` → `... rate.wave.shape must be sine, square or diurnal`; `ramp` without `over` → `... rate.ramp needs from, to and over`.
3. "base is optional when points are given": `rate: {points: [[0s, 1k/s]]}` loads; `rate: {wave: {...}}` alone → `node 'u': client needs rate.base or rate.points`.
4. "an unknown rate key is rejected" (exists; keep).

- [ ] **Step 2: Run, expect failure. Step 3: Implement** readers `read_wave`, `read_ramp`, `read_spike(s)`, `read_points`, `read_noise` with `parse_time_point` for `at`/`peak_at`; `Workload::curve` filled; `explain` (Task 10) will print the curve summary. Move the metric/direction name tables next to their enums in `model.cppm` (export `to_string(SloMetric)`, `to_string(Direction)`, parsers) and use them from config and report (backlog item; small).

- [ ] **Step 4: All config tests, scenario tests (all 14 validate). Step 5: Commit** `config: type the client rate curve; name tables live with their enums`

---

### Task 6: Clients follow the curve and the noise in the engine

**Files:** modify `src/engine/behaviours.cppm` (`ClientBehaviour{g, idx, seed}`; `void tick(SimTime now, SimTime dt)` advances the noise; `offer(now)` = `curve.rate_at(now) × noise_multiplier × shares`; `RequestSource` concept gains `tick`), `src/engine/engine.cppm` (call `tick` before `offer` in advance mode; in settle mode use the curve at `t` with noise 1 — `estimate --at` comes in 2c; `estimate` evaluates at `t = 0`), `tests/unit/test_behaviours.cpp`, `tests/theory/test_theory.cpp`.

- [ ] **Step 1: Write the failing tests**

```cpp
// test_behaviours.cpp
TEST_CASE("a client offers the curve's rate at the step's time", "[behaviours]") {
  Scenario s = two_hop(StepMode::sequential);
  s.nodes[0].workload->curve.spikes = {simsys::workload::Spike{.at = 1s, .duration = 1s, .factor = 3}};
  const auto g = *simsys::graph::Graph::build(s);
  ClientBehaviour c{g, *g.index_of("users"), /*seed=*/1};
  REQUIRE(c.offer(0s).outgoing[0].flow.rate_of("get").per_second == Approx(80));
  REQUIRE(c.offer(1s).outgoing[0].flow.rate_of("get").per_second == Approx(240));
}

TEST_CASE("noise is applied only after a tick and is deterministic per seed", "[behaviours]") {
  Scenario s = two_hop(StepMode::sequential);
  s.nodes[0].workload->noise = simsys::workload::Noise{.sigma = 0.3, .correlation = 5s};
  const auto g = *simsys::graph::Graph::build(s);
  ClientBehaviour a{g, 0, 9}, b{g, 0, 9};
  REQUIRE(a.offer(0s).outgoing[0].flow.total().per_second == Approx(100));  // no tick yet: multiplier 1
  a.tick(0s, 100ms); b.tick(0s, 100ms);
  REQUIRE(a.offer(0s).outgoing[0].flow.total() == b.offer(0s).outgoing[0].flow.total());
}

// test_theory.cpp
TEST_CASE("demand follows a sine wave through the engine", "[theory]") {
  Scenario s = mm1(500);
  s.nodes[0].workload->curve.wave = simsys::workload::Wave{.shape = simsys::workload::WaveShape::sine, .period = 1s, .amplitude = 0.5, .peak_at = 0s};
  auto sim = *Simulation::create(*Graph::build(s));
  REQUIRE_THAT(sim.step(0s, 100ms).nodes[1].demand_per_s, WithinRel(750.0, 1e-9));
  REQUIRE_THAT(sim.step(500ms, 100ms).nodes[1].demand_per_s, WithinRel(250.0, 1e-9));
}

TEST_CASE("two runs with the same seed are identical and a different seed is not", "[theory]") {
  Scenario s = mm1(500);
  s.nodes[0].workload->noise = simsys::workload::Noise{.sigma = 0.2, .correlation = 1s};
  auto collect = [&](int seed) { s.seed = seed; std::vector<double> d; (*Simulation::create(*Graph::build(s))).run({.duration = 2s, .step = 100ms}, [&](const auto& r) { d.push_back(r.nodes[1].demand_per_s); }); return d; };
  REQUIRE(collect(1) == collect(1));
  REQUIRE(collect(1) != collect(2));
}

TEST_CASE("estimate ignores noise and evaluates the curve at time zero", "[theory]") {
  Scenario s = mm1(500);
  s.nodes[0].workload->noise = simsys::workload::Noise{.sigma = 0.5, .correlation = 1s};
  s.nodes[0].workload->curve.spikes = {simsys::workload::Spike{.at = 0s, .duration = 1s, .factor = 2}};
  const auto e = (*Simulation::create(*Graph::build(s))).estimate();
  REQUIRE_THAT(e.nodes[1].demand_per_s, WithinRel(1000.0, 1e-12));
}
```
Seed derivation (document in one place, `behaviours.cppm`): `client_seed = std::uint64_t(scenario.seed) * 0x9E3779B97F4A7C15 ^ client_index` (a splitmix-style hash so seeds 1 and 2 give unrelated streams; comment it).

- [ ] **Step 2–5:** implement, tests pass, commit `engine: clients follow rate curves with seeded noise`

---

## Part B — Costs and the first three node types

### Task 7: Per-op cost

**Files:** modify `src/model/model.cppm` (`NodeSpec::cost` map), `src/config/config.cpp` (key `cost` on service, cache, sql_db, blob_store, worker_pool? — in 2a: service and cache; schema row), `src/engine/behaviours.cppm` (`NodeBehaviour` gains `{ b.cost(std::string_view{}) } -> std::same_as<double>`; `ServiceBehaviour::cost(op)` from the spec, default 1), `src/engine/engine.cppm` (demand = Σ rate × cost; composition kept in units; forwarded requests = units / cost), tests.

- [ ] **Step 1: Write the failing tests**

```cpp
// test_config.cpp: "a node's per-op cost is typed and must be positive"
//   service `cost: {write: 3, read: 1}` -> nodes[0].cost == {{"read",1},{"write",3}}; `cost: {x: 0}` -> "node 'a': cost 'x' must be positive"
// test_theory.cpp:
TEST_CASE("an op with cost 3 consumes three times the capacity of a cost-1 op", "[theory]") {
  Scenario s = mm1(300);                      // 300 req/s of op `op`
  s.nodes[1].cost = {{"op", 3.0}};            // = 900 units/s against 1000 units/s
  const auto r = (*Simulation::create(*Graph::build(s))).step(0s, 100ms);
  REQUIRE_THAT(r.nodes[1].rho, WithinRel(0.9, 1e-12));
  REQUIRE_THAT(r.nodes[1].demand_per_s, WithinRel(900.0, 1e-12));   // demand is in capacity units
}

TEST_CASE("costs do not change the number of requests forwarded", "[theory]") {
  // c -> a (cost 2 on op) -> b: b still receives one request per request a drained.
  ...assert b.demand == a.drain / 2 (b counts requests at cost 1) ...
}
```
Report the unit on the `demand_per_s` column as "units/s" in the table header when any node has a non-unit cost (explain prints the cost map).

- [ ] **Steps 2–5:** implement, tests pass, commit `engine: per-op capacity cost`

---

### Task 8: Load balancer

**Files:** modify `src/model/model.cppm` (`policy`, `vnodes`), `src/config/config.cpp` (keys; `policy` enum), `src/engine/graph.cppm` (splitter routes for any non-client node with no `ops` steps and ≥ 1 outgoing links — reuse `route_client_op` generalised to `route_splitter(node)`; `ResolvedNode` gains `pooled` and `imbalance` set on the *downstream* of an LB with `least_connections` / `consistent_hash`), `src/engine/behaviours.cppm` (`LoadBalancerBehaviour`: route = split incoming ops across its routes; response = share-weighted mixture like a client + own sojourn; `cost` 1), `src/engine/engine.cppm` (pooled queue: one `FluidQueue` with `capacity × replicas`, `concurrency × replicas`; imbalance: the busiest copy sees `demand/replicas × imbalance` — use it for rho/latency, and cap drain accordingly), `src/report/report.cpp` (explain prints `policy=`, `pooled`, `imbalance=`), tests.

- [ ] **Step 1: Write the failing tests** — graph: LB routes by weight/ops filter; `least_connections` marks downstream pooled; `consistent_hash vnodes: 100` → imbalance `1 + 1/sqrt(100) = 1.1`; an LB with `ops` steps is a loader error (`node 'lb': key 'ops' is not valid for type 'load_balancer'` — already). Theory: 2 copies at 1000/s, demand 1800/s: round robin → each copy rho 0.9, wait M/M/1 at 0.9 = 9 ms; least_connections → pooled M/M/2 at rho 0.9: Wq = ErlangC(2, 0.9)·s/(2·0.1) = 0.8526·1ms/0.2 = 4.26 ms (compute exactly in the test with `erlang_c`); consistent_hash vnodes 100 → the hot copy sees 990/s → rho 0.99. Behaviours: an LB with two weighted links splits 3:1; its response mixes by share.

- [ ] **Steps 2–5:** implement, tests pass; `labs/consistent-hashing` needs `sql_db` (2b) — not runnable yet; commit `nodes: load balancer (round robin, least connections, weighted, consistent hash)`

---

### Task 9: Rate limiter

**Files:** modify model/config (`algorithm`, `limit`, `burst`, `window` (duration, default 1s), `backend`), `src/engine/behaviours.cppm` (`RateLimiterBehaviour` with mutable token state → `route` must mutate: add `tick(now, dt)` to `NodeBehaviour` as an optional hook, or make `route` non-const for stateful behaviours — decide: `route(const Flow&, SimTime dt)` non-const for all behaviours (services ignore dt); update the concept and ServiceBehaviour accordingly), `Routed` gains `flow::Flow rejected`, `src/engine/engine.cppm` (`NodeSample::rejected_per_s`; rejected flow is dropped, never forwarded, counted in the node's drop rate), `src/report/*` (column `rejected`, drop rate = (dropped + rejected)/demand), tests, `scenarios/labs/rate-limiter/README.md`.

- [ ] **Step 1: Write the failing tests** — unit (`test_behaviours.cpp`): token bucket at limit 1000/s, burst 500, dt 100 ms: in 800/s → admitted 800, tokens fill by 20/step to 500; a 5× spike (4000/s) → admitted = min(4000, 1000 + 500/0.1 = 6000) = 4000 for the first step (bucket drains 300 → 200), then min(4000, 1000 + 200/0.1 = 3000) = 3000, then 1000 + 0 = 1000 thereafter; rejected = in − admitted; leaky bucket: queue with capacity 1000 and limit 500 → backlog grows, nothing rejected until full; fixed_window: burst = limit × window. Theory: `labs/rate-limiter` runs, the `api` stays ≤ its SLO when the limiter is in place, and the client's drop rate equals the limiter's rejection ratio within 1e-9; backend hop: `counters` receives one `counter` op per admitted request. Report: `rejected` column non-zero; JSON has `rejected`.

- [ ] **Steps 2–5:** implement, tests pass, README numbers from the real run (token bucket vs leaky bucket vs fixed window on the three spikes), commit `nodes: rate limiter (token bucket, leaky bucket, windowed) with rejection accounting`

---

### Task 10: Cache with a fixed hit ratio

**Files:** modify model/config (`hit_ratio: {fixed: h}` typed; `che` accepted but rejected by `Simulation::create` with "phase 2b"; `strategy`, `backend`, `write_ops`, `then` with `on: miss`), `src/engine/graph.cppm` (`hit_ratio(node)` static; `backend` edge requires a link; a service step `{to: cache, ...}` followed by `{to: db, on: miss}` resolves the miss fraction from the cache's hit ratio — store on the service's hop: `miss_fraction_of_previous`), `src/engine/behaviours.cppm` (`CacheBehaviour`: reads: `h` served, `1 − h` → misses (read_through: forwarded to backend; cache_aside: *reported* as served-with-miss so the service's `on: miss` step fires — the service computes it from the static ratio, the cache just serves everything in cache_aside); writes per strategy; `cost` 1; response: hit → own; miss (read_through) → own + backend hop; cache_aside → own (the service adds the db hop for the miss fraction, weighted mixture `h·own + (1−h)·(own + db hop)` computed in `ServiceBehaviour::response` via the `on: miss` hop's fraction)), engine (no change beyond `on: miss` support in service routing: forward `rate × (1 − h)` for miss steps), report/explain (`hit_ratio=`, `strategy=`), tests.

- [ ] **Step 1: Write the failing tests** — graph: a service step `{to: cache}` then `{to: db, on: miss}` gives the db hop fraction 0.2 for `fixed: 0.8`; an `on: miss` step not preceded by a cache/CDN step → `EngineError{"node 'web': an 'on: miss' step must follow a cache or cdn step"}`. Behaviours: cache_aside: `route` serves all, forwards nothing; service response for `get` = own + cache hop + 0.2 × db hop (mean) with the mixture variance; read_through: cache forwards 20 % to backend as the same op and its response = own + 0.2 × backend hop (mixture); write_through: a `write` op is forwarded 100 % to backend synchronously; write_around: same flow (documented). Theory: url-shortener-like chain c → web → cache(0.8) → db: db demand = 0.2 × reads; p99 for reads below the no-cache variant. Scenario test: `labs/rate-limiter` now runs (its `counters` cache has `hit_ratio: {fixed: 1.0}`).

- [ ] **Steps 2–5:** implement, tests pass, commit `nodes: cache with fixed hit ratio and the five strategies (write-back pending)`

---

### Task 11: Report and explain for the new nodes; scenario tests

**Files:** `src/report/report.cpp`, `tests/unit/test_report.cpp`, `tests/scenario/test_scenarios.cpp`.

- [ ] Tests: explain prints `rate: base 800/s, spikes 3, noise σ=0.15 τ=5m` for a client; `policy=least_connections (downstream pooled)`; `algorithm=token_bucket limit=1k/s burst=500`; `hit_ratio=0.8 (fixed) strategy=cache_aside write_ops=write`; the table has a `rejected` column; "labs scenarios that use only phase-2a types run and pass their SLOs" (rate-limiter); book scenarios still refused with a message naming the first unsupported type.
- [ ] Commit `report: explain and tables for curves, load balancers, rate limiters, caches`

---

### Task 12: Docs

**Files:** `docs/scenario-schema.md` (v1.2: rate curve keys read; `cost`, `write_ops`, `window`; LB/limiter/cache keys read), spec §16 (refinements 1–8 above), `docs/study/generator.md` (coroutines and `std::generator`), `docs/study/random.md` (`<random>`, seeding and determinism), `README.md` (curve command; phase 2a status), `scenarios/labs/rate-limiter/README.md` (if not done in Task 9).

- [ ] Commit `docs: schema v1.2, spec §16 phase 2a, study notes on generators and random`

---

## Self-review

- Spec coverage: §8 rate grammar (Tasks 2, 5), §9.2 noise + seed (3, 6), §7 `load_balancer`/`rate_limiter`/`cache` rows minus `che`/`write_back`/`ttl` (8–10; deferred items named), §11 nothing new except `curve` (4) — `sweep` is 2c. Op cost (7) is spec §3.3's `cost_op`.
- Placeholders: none; every task has test code and an interface.
- Type consistency: `RateCurve::rate_at(SimTime) -> Rate`; `NoiseProcess::next(SimTime) -> double`; `ClientBehaviour{g, idx, seed}.tick(now, dt)`/`offer(now)`; `route(const Flow&, SimTime dt)` non-const for every behaviour from Task 9 on (Task 8's LB can start const and change in Task 9; say so in Task 9's brief); `Routed{outgoing, served, rejected}`; `NodeSample::rejected_per_s`; `ResolvedNode::{pooled, imbalance}`; `Graph::hit_ratio(node)`.
- Settled by probe (2026-10-28): `<generator>` in the global module fragment of an interface that imports another module compiles, and an importer that includes `<format>`/`<print>` consumes the generator fine under GCC 14.2 (scratch probe `gen-probe/`). `samples()` can live in the interface.

## Phase 2b backlog (from the comprehensive review)

Found by the comprehensive review of phase 2a (2026-10-28) and left for phase 2b; the
findings that had to be fixed before the push (I1–I4, I6, I7, M1–M4, and I5's sentence in
the docs) are done.

- **I5 series columns.** `run --series` and the JSON report gain `demand_per_s` and
  `drain_per_s` per node, and `offered` and `answered` per (client, op), so the per-step
  identity `demand·dt = (drain + dropped)·dt + Δbacklog` (engine::NodeSample, schema
  "Drops and rejections") can be checked from the output alone.
- **M5 long functions** (over 25 lines, added or grown in 2a): `Graph::mark_balanced_targets`
  (40 lines), `Graph::collect_arriving_ops` (37), `Graph::resolve_nodes` (33),
  `RateLimiterBehaviour::route` (26), the CLI's `main` (52).
- **M6 duplicated helpers.** The per-op cost lookup three times (`ServiceBehaviour::cost`,
  `CacheBehaviour::cost`, report's `cost_of`); spike activity twice (`workload.cppm` and
  `config.cpp`); `BackendHop` twice in `behaviours.cppm`; the `"node '" + id + "': "`
  message prefix built by hand dozens of times.
- **M7 unit labels.** Comments in `report.cppm` say "requests" where the values are
  capacity units; the table header and explain's "peak offered" carry no unit label when a
  node has a `cost`, although plan Task 7 asked for one.
- **M8 utilization SLO behind a hash ring.** The schema's `utilization` row says "the
  node's mean busy fraction"; behind a `consistent_hash` balancer it is the hottest copy's.
- **M9 duplicate `write_ops`.** `write_ops: [write, write]` is accepted, while a link's
  `ops` must be distinct.
- **M10 diurnal table in the schema.** The schema never describes it: peak 1.6 at 20:00,
  trough 0.4 at 04:00, chosen rather than measured, and the period scaled onto 24 table hours.
- **M11 transient answered-weight bias.** In a step where a queue builds up, `passed =
  drain / demand`, and the later credit is capped at 1, so the requests that waited in the
  backlog are under-weighted and p99 is slightly optimistic around spikes. Documented only in
  `engine.cppm`.
- **M12 `estimate` table shape.** Its table presents one step's worth of `rejected`,
  `dropped` and `red_s` as totals, under a header that says "1 steps observed".
- **M13 `curve --seed` without `--noise`** is silently ignored.
- **M14 stale comments.** "phase 1 does not model" in `report.cpp`; queue_lag's "phase 2".
- **M15 test nits.** "every enum name table round-trips" (`test_model.cpp`) skips four
  tables and repeats static_asserts the module already makes; "consecutive samples are
  correlated over the correlation time" checks only the lag of one 100 ms step; "labs/
  rate-limiter runs in phase 2a" is not named as a property.
- **Explain note for nodes fed through unsupported types.** Explain prints `0/s ... green`
  for every node after `frontier` in web-crawler, and for the queues, workers and sinks in
  notification-system, news-feed, youtube, chat-system and google-drive, because those types
  do not propagate load yet (older than 2a). Say so on the check line.
- **Test gaps** (numbered as in the review; gap 1 is I1's test, gap 4 is done):
  2. per-node, per-step conservation with a token-bucket limiter and with saturated hot and
     cool copies (it holds today, but nothing pins it);
  3. `run` converging to `estimate` on balancer, limiter and cache paths, plus the
     documented sliding-counter shortfall;
  4. ~~the noise mean multiplier at σ = 1~~ (done: "the noise floor lifts the mean
     multiplier: about 1.08 at sigma one");
  5. determinism at the CSV level (spec §12 says "identical series"; the current test
     compares one node's demand only);
  6. pinning the transient answered weighting of M11.
