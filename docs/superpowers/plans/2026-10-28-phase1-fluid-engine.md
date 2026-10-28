# Phase 1: Fluid Engine Core — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `simsys explain`, `simsys estimate` and `simsys run` work end to end for scenarios made of `client` and `service` nodes, on top of a fluid-flow engine whose per-node numbers are verified against closed-form queueing theory (M/M/1, M/M/c, Erlang C, lognormal percentiles) and whose per-step flow conservation is tested.

**Architecture:** Small named modules in dependency order: `simsys.queueing` (pure formulas) → `simsys.flow` (per-op rates, latency moments) → `simsys.fluid` (one node's capacity/backlog step) → `simsys.model` (typed scenario) ← `simsys.config` (YAML → model) → `simsys.defaults` (back-of-the-envelope table, provenance) → `simsys.graph` (resolved nodes/links, topological order, feedback edges) → `simsys.behaviours` (client, service; the `NodeBehaviour` concept) → `simsys.engine` (forward demand pass, backward latency pass, step loop, Observer) → `simsys.report` (summaries, SLOs, table/JSON/CSV) → CLI. Each step of `run` does a **forward pass** in topological order (demand → drain/backlog/drops per node, flows pushed through link delay lines) and a **backward pass** in reverse order (response latency per `(node, op)` composed from the node's own sojourn and its downstream hops). Clients fit a lognormal to the response moments and report p50/p95/p99.

**Tech Stack:** C++23, GCC 14.2, CMake 3.28 named modules, yaml-cpp 0.8.0, CLI11 2.4.2, Catch2 3.7.1. Same build presets, warnings and CI as Phase 0.

**Conventions (from `CLAUDE.md`):** first use of each C++20/23 construct in a file gets a 2–5 line comment; every formula names itself (Erlang C, Little, Kingman, Che...) and states its assumption; every default says where the number comes from; every design pattern is named at the point of use with `// Pattern: ...`; tests are named after the property they prove; TDD in small batches (the test-writer agent writes the batch's tests from the code below, the implementer makes them pass, the reviewer checks both).

**Refinements of the spec (rev. 2) made here — Task 15 writes them back into the spec:**

1. **Latency is computed on the reply path, not carried forward in flows.** Spec §3.2 puts accumulated latency inside `Flow`. Phase 1 flows carry rates only; the engine computes response latency per `(node, op)` in a backward pass (§3.5 numbers are unchanged, the code is simpler, and fan-out factors cannot distort latency).
2. **Payload bytes are deferred to Phase 2** (needed by `blob_store`/bandwidth, nothing in Phase 1 reads them).
3. **Defaults are a `constexpr` C++ table**, not a YAML file loaded at start-up (§6). The table is the single source of truth, `explain` prints every resolved number with its provenance, and per-node/per-link overrides in the scenario still win. A user-editable `defaults/latency_numbers.yaml` can be layered in later without changing the API.
4. **`replicas: N`** is modelled as N independent copies behind an equal split (what round-robin gives): each copy sees `demand / N` and has the node's `capacity`, `concurrency` and `queue_limit`; backlog and drops are reported summed over copies.
5. **Link `latency` is one-way.** The datacenter defaults are round trips (`intra_dc_rtt` 500 µs, `cross_dc_rtt` 150 ms), so a defaulted link uses `rtt / 2` each way and a request/reply pair adds one rtt.
6. **Feedback edges** (chat: `chat_servers → message_q → sync_workers → chat_servers`; crawler: `frontier → ... → url_seen → frontier`) are detected as DFS back edges and always go through a delay line of at least one step, i.e. "cycles use the previous step's value" (§3.1).
7. **`run` and `estimate` support only `client` and `service` nodes in Phase 1**; `explain` works for every type. Running a scenario with another type fails with a clear "not supported until phase 2" error. Rate curves other than `base` (`wave`, `spikes`, `ramp`, `points`, `noise`) are accepted by the loader and ignored by the Phase 1 workload; `explain` says so.
8. Added node key `advanced.burstiness` (arrival SCV for Kingman, default 1.0) to the schema.

---

## File structure

| Path | Responsibility |
|---|---|
| `src/queueing/queueing.cppm` | Module `simsys.queueing`: Erlang C, M/M/c wait moments, Kingman factor, lognormal quantile |
| `src/flow/flow.cppm` | Module `simsys.flow`: `OpName`, `Flow` (sorted-vector map op → rate), `LatencyEstimate` and its sequential/parallel composition |
| `src/core/core.cppm` | *modify*: `seconds()` / `from_seconds()` conversions |
| `src/fluid/fluid.cppm` | Module `simsys.fluid`: `Capacity`, `Zone`, `FluidQueue::advance` (§3.3), `settle` |
| `src/model/model.cppm` | Module `simsys.model`: `NodeType`, `Step`, `Advanced`, `Workload`, `NodeSpec`, `LinkSpec`, `SloSpec`, `Scenario` |
| `src/config/config.cppm`, `src/config/config.cpp` | *modify*: `load()` returns `model::Scenario`; typed parsing of common, client, service, link and SLO keys; per-type allowed-key tables |
| `src/defaults/defaults.cppm` | Module `simsys.defaults`: `latency_numbers`, per-type defaults, `Resolved<T>` with `Source`, `resolve_*` |
| `src/engine/graph.cppm` | Module `simsys.graph`: `Graph::build`, resolved nodes/links, demand edges, topological order with feedback edges, `ClientRoute` |
| `src/engine/behaviours.cppm` | Module `simsys.behaviours`: `NodeBehaviour`/`RequestSource` concepts, `ServiceBehaviour`, `ClientBehaviour`, `Behaviour` variant |
| `src/engine/engine.cppm` | Module `simsys.engine`: `DelayLine`, `StepRecord`, `StepObserver`, `Simulation::{step, estimate, run}` |
| `src/report/report.cppm`, `src/report/report.cpp` | Module `simsys.report`: `Summary`, SLO evaluation, text table, JSON, CSV series |
| `src/cli/main.cpp` | *modify*: `explain`, `estimate`, `run` subcommands |
| `tests/unit/test_queueing.cpp`, `test_flow.cpp`, `test_fluid.cpp`, `test_model.cpp`, `test_defaults.cpp`, `test_config.cpp` (*modify*), `test_graph.cpp`, `test_behaviours.cpp`, `test_report.cpp` | Unit tests |
| `tests/theory/test_theory.cpp` | Theory and conservation tests on whole simulations |
| `tests/scenario/test_scenarios.cpp` | *modify*: every scenario builds a graph; tutorial scenarios run under 5 s and pass their SLOs |
| `scenarios/tutorial/chain/scenario.yaml`, `README.md` | Client → api → db chain exercising sequential steps |
| `docs/scenario-schema.md`, `docs/superpowers/specs/...design.md`, `README.md`, `docs/study/{concepts,ranges,variant-visit,format-print}.md` | Docs |

All modules register in `CMakeLists.txt` in the `simsys` library's `FILE_SET CXX_MODULES` (interface units) or `PRIVATE` (implementation units). Test executables register in `tests/CMakeLists.txt`.

**Build/test commands used throughout** (run from the repo root):

```bash
cmake --preset gcc14-debug            # once, after adding files to CMake
cmake --build --preset gcc14-debug
ctest --preset gcc14-debug            # all
./build/gcc14-debug/tests/unit_tests "[queueing]"   # one tag
```

**Commit style:** two commits per task, `test(<area>): ... (TDD red)` then `<area>: ...`, message bodies say *why*. Co-author line per the session's attribution reminder.

---

## Part A — Pure modules and the typed scenario

### Task 1: Erlang C and M/M/c waiting-time moments

**Files:**
- Create: `src/queueing/queueing.cppm`
- Create: `tests/unit/test_queueing.cpp`
- Modify: `CMakeLists.txt` (add `src/queueing/queueing.cppm` to the module file set), `tests/CMakeLists.txt` (add `unit/test_queueing.cpp` to `unit_tests`)

- [ ] **Step 1: Write the failing tests**

```cpp
// Closed-form queueing results the fluid engine must reproduce. Values are from the
// textbook formulas (Kleinrock vol. 1, ch. 3) worked by hand in the comments.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

import simsys.queueing;

using Catch::Approx;
using namespace simsys::queueing;

TEST_CASE("erlang_c for one server equals the utilization", "[queueing]") {
  // M/M/1: an arrival waits iff the server is busy, P(busy) = rho.
  REQUIRE(erlang_c(1, 0.5) == Approx(0.5));
  REQUIRE(erlang_c(1, 0.9) == Approx(0.9));
}

TEST_CASE("erlang_c matches hand-computed multi-server values", "[queueing]") {
  // c = 2, rho = 0.5: offered load a = 1 Erlang.
  // terms a^k/k! for k < c: 1, 1; last term a^c/c!/(1-rho) = 0.5/0.5 = 1 -> 1/(1+1+1).
  REQUIRE(erlang_c(2, 0.5) == Approx(1.0 / 3.0));
  // c = 3, rho = 0.8: a = 2.4; sum = 1 + 2.4 + 2.88 = 6.28; last = 2.304/0.2 = 11.52.
  REQUIRE(erlang_c(3, 0.8) == Approx(11.52 / (6.28 + 11.52)).margin(1e-6));
}

TEST_CASE("erlang_c is 0 with no load and 1 when saturated", "[queueing]") {
  REQUIRE(erlang_c(4, 0.0) == 0.0);
  REQUIRE(erlang_c(4, 1.0) == 1.0);
  REQUIRE(erlang_c(4, 1.7) == 1.0);
}

TEST_CASE("mmc_wait reproduces the M/M/1 mean wait rho/(mu - lambda)", "[queueing]") {
  // service time 1 ms (mu = 1000/s), rho = 0.5 (lambda = 500/s): Wq = 0.5/500 = 1 ms.
  const auto w = mmc_wait(1, 0.5, 0.001);
  REQUIRE(w.mean_s == Approx(0.001));
  // Wq is 0 with prob 1-Pw, else exponential with mean s/(c(1-rho)) = 2 ms:
  // Var = Pw(2-Pw) * (2 ms)^2 = 0.75 * 4e-6.
  REQUIRE(w.variance_s2 == Approx(0.75 * 4e-6));
}

TEST_CASE("mmc_wait for two servers scales by Erlang C over c(1 - rho)", "[queueing]") {
  const auto w = mmc_wait(2, 0.5, 0.001);
  REQUIRE(w.mean_s == Approx((1.0 / 3.0) * 0.001 / (2 * 0.5)));
}

TEST_CASE("mmc_wait is infinite at or beyond saturation", "[queueing]") {
  const auto w = mmc_wait(1, 1.0, 0.001);
  REQUIRE(w.mean_s == std::numeric_limits<double>::infinity());
  REQUIRE(w.variance_s2 == std::numeric_limits<double>::infinity());
}
```

- [ ] **Step 2: Register files in CMake and run the tests to verify they fail**

Add `src/queueing/queueing.cppm` to `target_sources(simsys PUBLIC FILE_SET CXX_MODULES FILES ...)` and `unit/test_queueing.cpp` to `add_executable(unit_tests ...)`. Create an empty `queueing.cppm` containing only `export module simsys.queueing;` so the build reaches the test file.

Run: `cmake --preset gcc14-debug && cmake --build --preset gcc14-debug`
Expected: compile error in `test_queueing.cpp`: `erlang_c` was not declared.

- [ ] **Step 3: Implement the module**

```cpp
// Module `simsys.queueing`: closed-form results from queueing theory that the fluid
// engine uses as the stochastic correction on top of its deterministic backlog.
// Written for a reader who has read the book but not studied queueing theory:
// every function names its formula and the assumption it rests on.
module;
#include <cmath>
#include <limits>

export module simsys.queueing;

export namespace simsys::queueing {

// Erlang C: the probability that an arriving request finds all `servers` busy and has
// to wait, in an M/M/c queue (Poisson arrivals, exponential service, c servers).
// `rho` is the utilization lambda / (c * mu) in [0, 1).
//   P_wait = [a^c/c! * 1/(1-rho)] / [sum_{k<c} a^k/k! + a^c/c! * 1/(1-rho)],  a = c*rho
// At or beyond rho = 1 every arrival waits, so we return 1 (the fluid backlog carries
// the actual delay then, see fluid.cppm).
[[nodiscard]] double erlang_c(int servers, double rho) {
  if (rho <= 0.0) return 0.0;
  if (rho >= 1.0) return 1.0;
  const double offered_load = servers * rho;  // "a", in Erlangs
  double term = 1.0;                          // a^k / k!, built iteratively (no factorials)
  double sum_below_c = 0.0;
  for (int k = 0; k < servers; ++k) {
    sum_below_c += term;
    term *= offered_load / (k + 1);
  }
  const double last = term / (1.0 - rho);     // a^c / c! * 1/(1-rho)
  return last / (sum_below_c + last);
}

struct WaitMoments {
  double mean_s;
  double variance_s2;
};

// Mean and variance of the time spent waiting in queue (not being served) in M/M/c.
// The wait is 0 with probability 1 - P_wait, otherwise exponential with mean
// s / (c * (1 - rho)) where s is the service time, so
//   E[Wq]   = P_wait * s / (c (1 - rho))               (Little's law applied to the queue)
//   Var[Wq] = P_wait (2 - P_wait) * (s / (c (1 - rho)))^2
// Infinite at saturation: a queue with rho >= 1 has no steady state.
[[nodiscard]] WaitMoments mmc_wait(int servers, double rho, double service_time_s) {
  if (rho >= 1.0) {
    constexpr double inf = std::numeric_limits<double>::infinity();
    return {inf, inf};
  }
  const double p_wait = erlang_c(servers, rho);
  const double scale = service_time_s / (servers * (1.0 - rho));
  return {.mean_s = p_wait * scale, .variance_s2 = p_wait * (2.0 - p_wait) * scale * scale};
}

}  // namespace simsys::queueing
```

Add the C++20 designated-initializer comment at its first use (`.mean_s = ...`): what it is, that it replaces positional aggregate init that silently swaps same-typed fields, why it fits here.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset gcc14-debug && ./build/gcc14-debug/tests/unit_tests "[queueing]"`
Expected: all 6 cases pass.

- [ ] **Step 5: Commit**

```bash
git add src/queueing/queueing.cppm tests/unit/test_queueing.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "queueing: Erlang C and M/M/c wait moments as closed forms"
```

---

### Task 2: Kingman variability factor and lognormal quantiles

**Files:**
- Modify: `src/queueing/queueing.cppm`
- Modify: `tests/unit/test_queueing.cpp`

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("variability_factor is 1 for Poisson arrivals and exponential service", "[queueing]") {
  REQUIRE(variability_factor(1.0, 1.0) == Approx(1.0));
}

TEST_CASE("variability_factor averages the two squared coefficients of variation", "[queueing]") {
  REQUIRE(variability_factor(2.0, 1.0) == Approx(1.5));  // bursty arrivals
  REQUIRE(variability_factor(0.0, 0.0) == Approx(0.0));  // D/D/c: nobody waits
}

TEST_CASE("lognormal_quantile with zero variance is the mean at every z", "[queueing]") {
  REQUIRE(lognormal_quantile(0.010, 0.0, z_p50) == Approx(0.010));
  REQUIRE(lognormal_quantile(0.010, 0.0, z_p99) == Approx(0.010));
}

TEST_CASE("lognormal_quantile median lies below the mean (right skew)", "[queueing]") {
  // mean 10 ms, sd 5 ms: median = m / sqrt(1 + v/m^2) = 10 / sqrt(1.25) = 8.944 ms
  REQUIRE(lognormal_quantile(0.010, 25e-6, z_p50) == Approx(0.008944).margin(1e-6));
}

TEST_CASE("lognormal_quantile p99 matches a hand-computed value", "[queueing]") {
  // sigma^2 = ln(1.25) = 0.22314, mu = ln(0.01) - sigma^2/2 = -4.71674,
  // q99 = exp(mu + 0.47238 * 2.32635) = exp(-3.6178) = 0.02684
  REQUIRE(lognormal_quantile(0.010, 25e-6, z_p99) == Approx(0.02684).margin(2e-5));
}

TEST_CASE("lognormal_quantile of a zero mean is zero", "[queueing]") {
  REQUIRE(lognormal_quantile(0.0, 0.0, z_p95) == 0.0);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build --preset gcc14-debug`
Expected: `variability_factor` / `lognormal_quantile` / `z_p50` not declared.

- [ ] **Step 3: Implement**

Append inside the exported namespace:

```cpp
// Kingman / Allen–Cunneen approximation: a G/G/c queue waits about
//   (c_a^2 + c_s^2) / 2  times as long as the M/M/c queue with the same rho,
// where c_a^2 is the squared coefficient of variation (variance / mean^2) of
// inter-arrival times ("burstiness") and c_s^2 that of service times
// ("service_variability"). Exact for M/M/c (1, 1 -> 1); a heuristic otherwise.
[[nodiscard]] double variability_factor(double arrival_scv, double service_scv) {
  return (arrival_scv + service_scv) / 2.0;
}

// Standard-normal quantiles for the three percentiles reports show. Constants rather
// than an inverse-error-function implementation: three values is all we need.
inline constexpr double z_p50 = 0.0;
inline constexpr double z_p95 = 1.6448536;
inline constexpr double z_p99 = 2.3263479;

// Lognormal fit by moments. Latency along a path is a sum of positive, right-skewed
// terms; a lognormal with the same mean m and variance v is the usual engineering fit:
//   sigma^2 = ln(1 + v/m^2),  mu = ln m - sigma^2/2,  quantile(z) = exp(mu + sigma z)
// Reports label the result "estimated": it is a fit, not a measurement.
[[nodiscard]] double lognormal_quantile(double mean, double variance, double z_score) {
  if (mean <= 0.0) return 0.0;
  if (variance <= 0.0) return mean;
  const double sigma_sq = std::log1p(variance / (mean * mean));
  const double mu = std::log(mean) - sigma_sq / 2.0;
  return std::exp(mu + std::sqrt(sigma_sq) * z_score);
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build --preset gcc14-debug && ./build/gcc14-debug/tests/unit_tests "[queueing]"`
Expected: 12 cases pass.

- [ ] **Step 5: Commit**

```bash
git commit -am "queueing: Kingman variability factor and lognormal percentile fit"
```

---

### Task 3: `Flow` and `LatencyEstimate`

**Files:**
- Create: `src/flow/flow.cppm`
- Create: `tests/unit/test_flow.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing tests**

```cpp
// Properties of the per-op flow map and of latency-moment composition.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

import simsys.core;
import simsys.flow;

using Catch::Approx;
using simsys::Rate;
using simsys::flow::Flow;
using simsys::flow::LatencyEstimate;

TEST_CASE("Flow starts empty and accumulates rates per op", "[flow]") {
  Flow f;
  REQUIRE(f.empty());
  f.add("read", Rate{100});
  f.add("read", Rate{50});
  f.add("write", Rate{10});
  REQUIRE_FALSE(f.empty());
  REQUIRE(f.rate_of("read").per_second == Approx(150));
  REQUIRE(f.rate_of("write").per_second == Approx(10));
  REQUIRE(f.rate_of("missing").per_second == 0.0);
  REQUIRE(f.total().per_second == Approx(160));
}

TEST_CASE("Flow keeps its ops sorted so iteration order is deterministic", "[flow]") {
  Flow f;
  f.add("zeta", Rate{1});
  f.add("alpha", Rate{2});
  f.add("mid", Rate{3});
  const auto entries = f.entries();
  REQUIRE(entries.size() == 3);
  REQUIRE(entries[0].op == "alpha");
  REQUIRE(entries[1].op == "mid");
  REQUIRE(entries[2].op == "zeta");
}

TEST_CASE("Flow addition merges ops and scaling multiplies every rate", "[flow]") {
  Flow a;
  a.add("read", Rate{100});
  Flow b;
  b.add("read", Rate{20});
  b.add("write", Rate{5});
  a += b;
  REQUIRE(a.rate_of("read").per_second == Approx(120));
  REQUIRE(a.rate_of("write").per_second == Approx(5));
  const Flow half = a.scaled(0.5);
  REQUIRE(half.rate_of("read").per_second == Approx(60));
  REQUIRE(half.rate_of("write").per_second == Approx(2.5));
  REQUIRE(a.rate_of("read").per_second == Approx(120));  // scaled() does not mutate
}

TEST_CASE("Flow::clear empties the map", "[flow]") {
  Flow f;
  f.add("x", Rate{1});
  f.clear();
  REQUIRE(f.empty());
}

TEST_CASE("Sequential hops add latency means and variances", "[flow]") {
  // Independence assumption: Var(X+Y) = Var X + Var Y.
  const LatencyEstimate a{.mean_s = 0.001, .variance_s2 = 1e-6};
  const LatencyEstimate b{.mean_s = 0.002, .variance_s2 = 4e-6};
  const auto sum = a + b;
  REQUIRE(sum.mean_s == Approx(0.003));
  REQUIRE(sum.variance_s2 == Approx(5e-6));
}

TEST_CASE("Parallel branches take the largest mean and the largest variance", "[flow]") {
  const std::array branches{LatencyEstimate{0.001, 9e-6}, LatencyEstimate{0.005, 1e-6}};
  const auto m = simsys::flow::parallel_max(branches);
  REQUIRE(m.mean_s == Approx(0.005));
  REQUIRE(m.variance_s2 == Approx(9e-6));
  REQUIRE(simsys::flow::parallel_max({}) == LatencyEstimate{});
}
```

- [ ] **Step 2: Register in CMake, run to verify failure**

Expected: `simsys.flow` cannot be imported / names not declared.

- [ ] **Step 3: Implement**

```cpp
// Module `simsys.flow`: what travels along an edge during one step — a rate per
// operation name — and the latency moments the backward pass composes.
module;
#include <algorithm>
#include <compare>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module simsys.flow;

import simsys.core;

export namespace simsys::flow {

using OpName = std::string;

// Mean and variance (seconds, seconds^2) of a latency contribution. Moments rather
// than a full distribution: along a path they add (independence), and the client fits
// a lognormal at the end (queueing.cppm). Spec §3.5.
struct LatencyEstimate {
  double mean_s{};
  double variance_s2{};
  // C++20 defaulted comparisons: `= default` on <=> gives all six operators,
  // member-wise, replacing six hand-written functions. Equality is what tests need.
  auto operator<=>(const LatencyEstimate&) const = default;
  LatencyEstimate& operator+=(const LatencyEstimate& other) {
    mean_s += other.mean_s;
    variance_s2 += other.variance_s2;
    return *this;
  }
  friend LatencyEstimate operator+(LatencyEstimate a, const LatencyEstimate& b) { return a += b; }
};

// Fan-out in parallel: the caller waits for the slowest branch. The max of lognormals
// has no closed form; spec §3.5 approximates it by the largest mean and the largest
// variance among the branches (slightly pessimistic on variance).
[[nodiscard]] LatencyEstimate parallel_max(std::span<const LatencyEstimate> branches) {
  LatencyEstimate result{};
  for (const auto& b : branches) {
    result.mean_s = std::max(result.mean_s, b.mean_s);
    result.variance_s2 = std::max(result.variance_s2, b.variance_s2);
  }
  return result;
}

// A flow: rate per op name, additive. Pattern: flat map (sorted vector) — a few ops per
// flow, iterated every step, so a contiguous sorted vector beats std::map (GCC 14 has
// no std::flat_map yet). Sorted order also makes every run byte-identical (§3.6).
class Flow {
 public:
  struct Entry {
    OpName op;
    Rate rate;
  };

  void add(std::string_view op, Rate rate);
  [[nodiscard]] Rate rate_of(std::string_view op) const;
  [[nodiscard]] Rate total() const;
  [[nodiscard]] Flow scaled(double factor) const;
  [[nodiscard]] bool empty() const { return entries_.empty(); }
  [[nodiscard]] std::span<const Entry> entries() const { return entries_; }
  void clear() { entries_.clear(); }
  Flow& operator+=(const Flow& other);

 private:
  // C++20 ranges: std::ranges::lower_bound with a projection (&Entry::op) finds the
  // insertion point without a hand-written comparator lambda.
  [[nodiscard]] auto position_of(std::string_view op) const {
    return std::ranges::lower_bound(entries_, op, {}, &Entry::op);
  }
  std::vector<Entry> entries_;
};

}  // namespace simsys::flow
```

Define the four member functions after the class (`add` inserts or accumulates at `position_of`; `rate_of` returns `Rate{0}` when absent; `total` sums with `std::ranges::fold_left` (C++23, comment it) or a loop; `scaled` copies and multiplies; `operator+=` calls `add` per entry). Note that `std::ranges::lower_bound` with `std::string_view` against `std::string` works because `std::ranges::less` compares them through `operator<=>`.

- [ ] **Step 4: Run tests, expect 6 passes; Step 5: Commit** `flow: per-op flow map and latency moments`

---

### Task 4: `FluidQueue` — one node's step (§3.3)

**Files:**
- Modify: `src/core/core.cppm` (add `seconds`, `from_seconds`)
- Create: `src/fluid/fluid.cppm`
- Create: `tests/unit/test_fluid.cpp`; Modify: `tests/unit/test_units.cpp` (two conversions tests)
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing tests**

In `test_units.cpp`:

```cpp
TEST_CASE("seconds and from_seconds round-trip SimTime through double") {
  REQUIRE(simsys::seconds(1500ms) == Catch::Approx(1.5));
  REQUIRE(simsys::from_seconds(0.0001) == 100us);
  REQUIRE(simsys::seconds(simsys::from_seconds(2.5)) == Catch::Approx(2.5));
}
```

New `tests/unit/test_fluid.cpp`:

```cpp
// The fluid step of a single node (spec §3.3), checked against the formulas and
// against the conservation law flow_in = flow_out + drops + delta_backlog.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>

import simsys.core;
import simsys.fluid;

using Catch::Approx;
using namespace std::chrono_literals;
using simsys::Rate;
using simsys::fluid::Capacity;
using simsys::fluid::FluidQueue;
using simsys::fluid::Zone;

namespace {
// 1000 req/s, 1 ms service, one server, room for 1000 queued requests.
Capacity web() {
  return Capacity{.service_rate = Rate{1000}, .service_time_s = 0.001, .queue_limit = 1000};
}
}  // namespace

TEST_CASE("an underloaded queue drains everything with no backlog", "[fluid]") {
  FluidQueue q{web()};
  const auto s = q.advance(Rate{500}, 100ms);
  REQUIRE(s.drain_per_s == Approx(500));
  REQUIRE(s.dropped_per_s == 0.0);
  REQUIRE(s.backlog == 0.0);
  REQUIRE(s.rho == Approx(0.5));
  REQUIRE(s.zone == Zone::green);
}

TEST_CASE("sojourn under load is M/M/1 wait plus service time", "[fluid]") {
  FluidQueue q{web()};
  const auto s = q.advance(Rate{500}, 100ms);
  // Wq = rho/(mu-lambda) = 1 ms, plus S = 1 ms.
  REQUIRE(s.latency.mean_s == Approx(0.002));
  // Var(Wq) = 0.75 * (2 ms)^2 = 3e-6; Var(S) = cs^2 * S^2 = 1e-6 (exponential).
  REQUIRE(s.latency.variance_s2 == Approx(4e-6));
}

TEST_CASE("overload grows the backlog by (demand - capacity) * dt each step", "[fluid]") {
  FluidQueue q{web()};
  const auto s1 = q.advance(Rate{1500}, 100ms);
  REQUIRE(s1.drain_per_s == Approx(1000));
  REQUIRE(s1.backlog == Approx(50));
  REQUIRE(s1.rho == 1.0);
  REQUIRE(s1.zone == Zone::red);
  const auto s2 = q.advance(Rate{1500}, 100ms);
  REQUIRE(s2.backlog == Approx(100));
  // Fluid wait = backlog / capacity (Little's law on the backlog).
  REQUIRE(s2.latency.mean_s >= 0.1);
}

TEST_CASE("a full backlog overflows into drops at demand minus capacity", "[fluid]") {
  FluidQueue q{web()};
  for (int i = 0; i < 20; ++i) q.advance(Rate{1500}, 100ms);  // backlog reaches 1000
  const auto s = q.advance(Rate{1500}, 100ms);
  REQUIRE(s.backlog == Approx(1000));
  REQUIRE(s.dropped_per_s == Approx(500));
}

TEST_CASE("a backlog drains at capacity once demand stops", "[fluid]") {
  FluidQueue q{web()};
  q.advance(Rate{1500}, 100ms);  // backlog 50
  const auto s = q.advance(Rate{0}, 100ms);
  REQUIRE(s.drain_per_s == Approx(500));  // 50 requests in 0.1 s
  REQUIRE(s.backlog == Approx(0).margin(1e-9));
}

TEST_CASE("flow is conserved every step: in = out + drops + delta backlog", "[fluid]") {
  FluidQueue q{web()};
  double previous_backlog = 0.0;
  const double dt = 0.1;
  for (const double demand : {200.0, 1500.0, 1500.0, 3000.0, 3000.0, 0.0, 0.0}) {
    const auto s = q.advance(Rate{demand}, 100ms);
    const double delta = s.backlog - previous_backlog;
    REQUIRE(demand * dt == Approx((s.drain_per_s + s.dropped_per_s) * dt + delta).margin(1e-9));
    previous_backlog = s.backlog;
  }
}

TEST_CASE("zones follow the yellow and red thresholds", "[fluid]") {
  const auto c = web();  // defaults yellow 0.70, red 0.90
  REQUIRE(simsys::fluid::zone_of(0.69, c) == Zone::green);
  REQUIRE(simsys::fluid::zone_of(0.70, c) == Zone::yellow);
  REQUIRE(simsys::fluid::zone_of(0.90, c) == Zone::red);
}

TEST_CASE("the stochastic wait is capped at rho 0.99 so saturation is not double counted", "[fluid]") {
  FluidQueue q{web()};
  const auto s = q.advance(Rate{1000}, 100ms);  // rho exactly 1, backlog still 0
  REQUIRE(s.latency.mean_s < 1.0);              // finite: Erlang term evaluated at 0.99
}

TEST_CASE("settle jumps to the long-run state: empty below capacity, full above", "[fluid]") {
  FluidQueue under{web()};
  under.settle(Rate{500});
  REQUIRE(under.backlog() == 0.0);
  FluidQueue over{web()};
  over.settle(Rate{1500});
  REQUIRE(over.backlog() == Approx(1000));
}

TEST_CASE("burstiness and service variability scale the stochastic wait (Kingman)", "[fluid]") {
  auto bursty = web();
  bursty.burstiness = 3.0;  // (3 + 1)/2 = 2x the M/M/1 wait
  FluidQueue q{bursty};
  const auto s = q.advance(Rate{500}, 100ms);
  REQUIRE(s.latency.mean_s == Approx(0.002 + 0.001));  // 2 ms wait + 1 ms service
}
```

- [ ] **Step 2: Register files; run; expect compile failures**

- [ ] **Step 3: Implement**

In `core.cppm` (exported, near `SimTime`):

```cpp
// Conversions to/from seconds as double, for arithmetic in the engine. All other code
// stores time as SimTime; doubles appear only inside formulas.
[[nodiscard]] constexpr double seconds(SimTime t) {
  return std::chrono::duration<double>(t).count();
}
[[nodiscard]] SimTime from_seconds(double s) {
  return std::chrono::round<SimTime>(std::chrono::duration<double>(s));
}
```

`fluid.cppm`:

```cpp
// Module `simsys.fluid`: the capacity/backlog mechanics every queue-like node shares.
// This is spec §3.3 — the heart of the fluid engine — kept free of node semantics so it
// can be tested against closed forms on its own.
module;
#include <algorithm>
#include <cmath>
#include <cstdint>

export module simsys.fluid;

import simsys.core;
import simsys.flow;
import simsys.queueing;

export namespace simsys::fluid {

// What one replica of a node can do. The book gives throughput (QPS) and latency as
// separate back-of-the-envelope numbers, so we keep both: `service_rate` bounds the
// fluid drain, `service_time_s` is how long one request occupies one of the
// `concurrency` servers and feeds the Erlang C term.
struct Capacity {
  Rate service_rate;             // requests per second one replica can drain
  double service_time_s{};       // seconds one request occupies a server
  int concurrency{1};            // servers per replica: the c in M/M/c
  double queue_limit{1000};      // requests that may wait; beyond this the fluid is dropped
  double yellow{0.70};           // utilization where the node turns yellow
  double red{0.90};              // ... and red
  double service_variability{1}; // squared coefficient of variation of service time (1 = exponential)
  double burstiness{1};          // squared coefficient of variation of inter-arrival times (1 = Poisson)
};

enum class Zone { green, yellow, red };

[[nodiscard]] Zone zone_of(double rho, const Capacity& c) {
  if (rho >= c.red) return Zone::red;
  if (rho >= c.yellow) return Zone::yellow;
  return Zone::green;
}

// Everything one step produced, for metrics and for routing the drained flow onward.
struct FluidStep {
  double demand_per_s{};
  double drain_per_s{};
  double dropped_per_s{};
  double backlog{};              // requests waiting after this step
  double rho{};                  // utilization, capped at 1
  flow::LatencyEstimate latency; // sojourn = queueing wait + service time
  Zone zone{Zone::green};
};

class FluidQueue {
 public:
  explicit FluidQueue(Capacity capacity) : capacity_{capacity} {}

  // Advance one step of `dt` with `demand` arriving. Spec §3.3:
  //   drain   = min(capacity, demand + backlog/dt)        serve what we can
  //   backlog += (demand - drain) * dt                    grows past capacity, shrinks below
  //   overflow = max(0, backlog - queue_limit) / dt;  backlog = min(backlog, queue_limit)
  //   rho     = min(1, demand / capacity)
  //   wait    = backlog / capacity                        Little's law on the deterministic backlog
  //           + Kingman * M/M/c wait at min(rho, 0.99)   stochastic bunching; capped so a
  //                                                      saturated node is not counted twice
  [[nodiscard]] FluidStep advance(Rate demand, SimTime dt);

  // Jump to the long-run state for the given constant demand (used by `estimate`):
  // below capacity the backlog is empty; at or above it the queue is full.
  void settle(Rate demand);

  [[nodiscard]] double backlog() const { return backlog_; }
  [[nodiscard]] const Capacity& capacity() const { return capacity_; }

 private:
  [[nodiscard]] double drain_for(double demand, double dt_s) const;
  [[nodiscard]] double overflow_after_growth(double dt_s);
  [[nodiscard]] flow::LatencyEstimate sojourn(double rho) const;

  Capacity capacity_;
  double backlog_{};
};

}  // namespace simsys::fluid
```

Implement with one short method per line of the formula. `sojourn`: `fluid_wait = backlog_/capacity`; `wq = mmc_wait(c, min(rho, 0.99), s) `, both moments multiplied by `variability_factor(burstiness, service_variability)` (variance by its square — note the approximation in a comment); service term `{s, service_variability * s * s}`; result `{fluid_wait + wq.mean, wq.var + service_var}` (the deterministic backlog adds no variance). Guard `service_rate.per_second <= 0` by treating the node as saturated (rho 1, drain 0).

- [ ] **Step 4: Run `[fluid]` and `test_units` — all pass. Step 5: Commit** `fluid: one node's capacity/backlog step with Erlang C and Kingman corrections`

---

### Task 5: Typed scenario model; `config::load` returns it

**Files:**
- Create: `src/model/model.cppm`
- Create: `tests/unit/test_model.cpp`
- Modify: `src/config/config.cppm` (remove local `Scenario`/`NodeDecl`/`LinkDecl`/`known_types`; import `simsys.model`; `load` returns `std::expected<model::Scenario, ConfigError>`), `src/config/config.cpp` (fill `NodeSpec{id, type}` via `parse_node_type`, `LinkSpec{from, to}`), `src/cli/main.cpp`, `tests/unit/test_config.cpp`, `tests/scenario/test_scenarios.cpp` (namespace changes only)
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

This task is a refactor guarded by the 51 existing tests; only `test_model.cpp` is new TDD.

- [ ] **Step 1: Write the failing tests** (`tests/unit/test_model.cpp`)

```cpp
// The typed scenario vocabulary: node types are an enum with a two-way string mapping.
#include <catch2/catch_test_macros.hpp>

import simsys.model;

using simsys::model::NodeType;

TEST_CASE("every node type name round-trips through parse_node_type and to_string", "[model]") {
  for (const auto name : simsys::model::node_type_names) {
    const auto type = simsys::model::parse_node_type(name);
    REQUIRE(type.has_value());
    REQUIRE(simsys::model::to_string(*type) == name);
  }
}

TEST_CASE("parse_node_type rejects unknown names", "[model]") {
  REQUIRE_FALSE(simsys::model::parse_node_type("mainframe").has_value());
}

TEST_CASE("the node type table is complete at compile time", "[model]") {
  static_assert(simsys::model::node_type_names.size() == 18);
  static_assert(simsys::model::parse_node_type("service") == NodeType::service);
  static_assert(simsys::model::to_string(NodeType::frontier) == "frontier");
}

TEST_CASE("a default NodeSpec has one replica in datacenter main", "[model]") {
  const simsys::model::NodeSpec n{.id = "x", .type = NodeType::service};
  REQUIRE(n.replicas == 1);
  REQUIRE(n.dc == "main");
  REQUIRE_FALSE(n.capacity.has_value());
}
```

- [ ] **Step 2: Run, expect failure (no module `simsys.model`)**

- [ ] **Step 3: Implement `model.cppm`**

```cpp
// Module `simsys.model`: the typed scenario — what the YAML means once parsed. No
// yaml-cpp here; `simsys.config` fills these structs and the engine reads them.
module;
#include <array>
#include <chrono>      // only chrono_literals for defaults; this interface imports only simsys.core
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

export module simsys.model;

import simsys.core;

export namespace simsys::model {

// C++20 `using enum` would let a switch say `case client:`; here a scoped enum keeps the
// 18 names out of the namespace. The order matches `node_type_names` (checked at compile time).
enum class NodeType : std::uint8_t {
  client, cdn, load_balancer, rate_limiter, service, cache, sql_db, kv_cluster,
  message_queue, worker_pool, blob_store, search_index, ws_gateway, service_discovery,
  id_generator, external_sink, notification_fanout, frontier
};

inline constexpr std::array<std::string_view, 18> node_type_names{
    "client", "cdn", "load_balancer", "rate_limiter", "service", "cache", "sql_db",
    "kv_cluster", "message_queue", "worker_pool", "blob_store", "search_index",
    "ws_gateway", "service_discovery", "id_generator", "external_sink",
    "notification_fanout", "frontier"};

// C++23 std::to_underlying: the enum's integer value without a static_cast that could
// silently accept the wrong type. Pattern: Table-driven method.
[[nodiscard]] constexpr std::string_view to_string(NodeType t) {
  return node_type_names[std::to_underlying(t)];
}

[[nodiscard]] constexpr std::optional<NodeType> parse_node_type(std::string_view name) {
  for (std::size_t i = 0; i < node_type_names.size(); ++i)
    if (node_type_names[i] == name) return static_cast<NodeType>(i);
  return std::nullopt;
}

enum class StepCondition { always, miss, failure };
enum class StepMode { sequential, parallel };

// One thing a node does with an incoming op: forward (a multiple of) it to `to` as `op`.
struct Step {
  std::string to;
  std::string op;                            // defaults to the incoming op name
  StepCondition on{StepCondition::always};
  StepMode mode{StepMode::sequential};
  double factor{1.0};                        // fan-out: one post -> 100 feed writes
};

// The `advanced:` block. Optionals: absent means "use the type default" (defaults.cppm).
struct Advanced {
  std::optional<double> queue_limit;
  std::optional<int> concurrency;
  std::optional<double> service_variability;
  std::optional<double> burstiness;
  std::optional<double> yellow;
  std::optional<double> red;
};

// What a client offers. Phase 1: a constant base rate; curves come in phase 2.
struct Workload {
  Rate base;
  std::vector<std::pair<std::string, double>> op_shares;  // sorted by op, shares sum to 1
  std::optional<SimTime> timeout;
};

struct NodeSpec {
  std::string id;
  NodeType type{};
  std::string dc{"main"};
  int replicas{1};
  std::optional<Rate> capacity;            // per replica; absent -> type default
  std::optional<ByteRate> byte_capacity;   // blob stores (phase 2 uses it)
  std::optional<SimTime> latency;          // service time; absent -> type default
  Advanced advanced;
  std::map<std::string, std::vector<Step>> ops;  // incoming op -> steps (service, ws_gateway)
  std::vector<Step> then;                        // steps after processing (workers, caches...)
  std::optional<Workload> workload;              // clients only
};

enum class Direction { in, out, inout };

struct LinkSpec {
  std::string from;
  std::string to;
  Direction dir{Direction::out};
  std::vector<std::string> ops;            // empty = all ops
  std::optional<SimTime> latency;          // one-way; absent -> datacenter default
  double weight{1.0};                      // share when several links carry the same op
};

enum class SloMetric { p50_latency, p95_latency, p99_latency, drop_rate, utilization, queue_lag, staleness };

struct SloSpec {
  SloMetric metric{};
  std::optional<std::string> node;
  std::optional<std::string> op;
  // C++17 std::variant, visited with C++20-style overload sets in report.cppm: latency
  // SLOs carry a duration, ratio SLOs a plain number; one field, no sentinel values.
  std::variant<SimTime, double> max;
};

struct Scenario {
  std::string name;
  int version{};
  int seed{1};
  SimTime duration{};
  SimTime step{std::chrono::milliseconds{100}};
  SimTime warmup{};
  std::vector<std::string> datacenters{"main"};
  std::vector<NodeSpec> nodes;
  std::vector<LinkSpec> links;
  std::vector<SloSpec> slos;
};

}  // namespace simsys::model
```

`<chrono>` here is only for the `100ms` default; because this interface imports only `simsys.core` (which already includes `<chrono>`) the GCC 14 bug does not trigger — the rule in `CLAUDE.md` §3 is about `<format>`-bearing headers. Verify by building; if the build reports "Bad file data", express the default as `SimTime{100'000'000}` with a comment and drop the include.

Then update `config.cppm`/`config.cpp`/tests/CLI: `load` fills `nodes` with `NodeSpec{.id, .type = *parse_node_type(text)}` (the unknown-type check already exists; map "unknown node type" onto `parse_node_type` returning nullopt) and `links` with `LinkSpec{.from, .to}`. Replace `known_types` uses by `model::node_type_names`. Tests referring to `simsys::config::Scenario`/`NodeDecl::type == "service"` become `simsys::model::Scenario` / `type == NodeType::service`.

- [ ] **Step 4: Run all tests, expect 51 + 4 pass. Step 5: Commit** `model: typed scenario vocabulary; config::load returns it`

---

### Task 6: Defaults table with provenance

**Files:**
- Create: `src/defaults/defaults.cppm`
- Create: `tests/unit/test_defaults.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing tests**

```cpp
// The back-of-the-envelope table (book ch. 2) and how a node's numbers are resolved
// from scenario values first, type defaults second, with provenance for `explain`.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>

import simsys.core;
import simsys.defaults;
import simsys.model;

using namespace std::chrono_literals;
using Catch::Approx;
using simsys::Rate;
using simsys::defaults::Source;
using simsys::model::NodeSpec;
using simsys::model::NodeType;

TEST_CASE("the latency numbers table holds the book's values", "[defaults]") {
  constexpr auto& t = simsys::defaults::latency_numbers;
  static_assert(t.ssd_random_read == 150us);
  static_assert(t.disk_seek == 10ms);
  static_assert(t.intra_dc_rtt == 500us);
  static_assert(t.cross_dc_rtt == 150ms);
  REQUIRE(t.third_party_api == 100ms);
}

TEST_CASE("every non-client type has a positive default service rate and time", "[defaults]") {
  for (const auto name : simsys::model::node_type_names) {
    const auto type = *simsys::model::parse_node_type(name);
    if (type == NodeType::client) continue;
    const auto d = simsys::defaults::for_type(type);
    CAPTURE(name);
    REQUIRE(d.service_rate.per_second > 0);
    REQUIRE(d.service_time > 0ns);
  }
}

TEST_CASE("the web tier default is 2k/s with 1 ms service (spec table)", "[defaults]") {
  const auto d = simsys::defaults::for_type(NodeType::service);
  REQUIRE(d.service_rate == Rate{2000});
  REQUIRE(d.service_time == 1ms);
}

TEST_CASE("a scenario capacity wins over the type default and says so", "[defaults]") {
  const NodeSpec n{.id = "db", .type = NodeType::sql_db, .capacity = Rate{8000}};
  const auto r = simsys::defaults::resolve_service_rate(n);
  REQUIRE(r.value == Rate{8000});
  REQUIRE(r.source == Source::scenario);
  const auto t = simsys::defaults::resolve_service_time(n);
  REQUIRE(t.value == 150us);  // ssd_random_read
  REQUIRE(t.source == Source::type_default);
}

TEST_CASE("resolve_capacity folds advanced keys over defaults", "[defaults]") {
  NodeSpec n{.id = "web", .type = NodeType::service};
  n.advanced.queue_limit = 50;
  n.advanced.burstiness = 2.0;
  const auto c = simsys::defaults::resolve_capacity(n);
  REQUIRE(c.service_rate == Rate{2000});
  REQUIRE(c.service_time_s == Approx(0.001));
  REQUIRE(c.concurrency == 1);
  REQUIRE(c.queue_limit == 50);
  REQUIRE(c.burstiness == 2.0);
  REQUIRE(c.service_variability == 1.0);
  REQUIRE(c.yellow == 0.70);
  REQUIRE(c.red == 0.90);
}

TEST_CASE("external sinks default to the third-party API latency with variability 2", "[defaults]") {
  const auto c = simsys::defaults::resolve_capacity(NodeSpec{.id = "s", .type = NodeType::external_sink});
  REQUIRE(c.service_time_s == Approx(0.1));
  REQUIRE(c.service_variability == 2.0);
}

TEST_CASE("link latency defaults to half the datacenter round trip, one way", "[defaults]") {
  const simsys::model::LinkSpec inside{.from = "a", .to = "b"};
  const auto l = simsys::defaults::resolve_link_latency(inside, /*cross_dc=*/false);
  REQUIRE(l.value == 250us);
  REQUIRE(l.source == Source::dc_default);
  const auto x = simsys::defaults::resolve_link_latency(inside, /*cross_dc=*/true);
  REQUIRE(x.value == 75ms);
  const simsys::model::LinkSpec given{.from = "a", .to = "b", .latency = 3ms};
  REQUIRE(simsys::defaults::resolve_link_latency(given, true).value == 3ms);
  REQUIRE(simsys::defaults::resolve_link_latency(given, true).source == Source::scenario);
}
```

- [ ] **Step 2: Register, run, expect failures**

- [ ] **Step 3: Implement**

```cpp
// Module `simsys.defaults`: the back-of-the-envelope numbers (System Design Interview
// ch. 2, "latency numbers every programmer should know") and the per-type defaults the
// spec §6 derives from them. Each number says where it comes from; `simsys explain`
// prints the resolved values with their Source.
module;
#include <chrono>
#include <optional>

export module simsys.defaults;

import simsys.core;
import simsys.fluid;
import simsys.model;

export namespace simsys::defaults {

using namespace std::chrono_literals;

struct LatencyNumbers {
  SimTime memory_read_1mb, ssd_random_read, disk_seek, disk_read_1mb, network_read_1mb,
      compress_1kb, intra_dc_rtt, cross_dc_rtt, third_party_api;
};

// C++20 designated initializers on a constexpr constant: the table reads like the book's
// table and a misordered field is a compile error, not a silently wrong latency.
inline constexpr LatencyNumbers latency_numbers{
    .memory_read_1mb = 250us,   // book: "read 1 MB sequentially from memory"
    .ssd_random_read = 150us,   // book: "SSD random read"
    .disk_seek = 10ms,          // book: "disk seek"
    .disk_read_1mb = 30ms,      // book: "read 1 MB sequentially from disk"
    .network_read_1mb = 10ms,   // book: "read 1 MB over 1 Gbps network"
    .compress_1kb = 10us,       // book: "compress 1 KB with Zippy"
    .intra_dc_rtt = 500us,      // book: "round trip within the same datacenter"
    .cross_dc_rtt = 150ms,      // book: "send packet CA -> Netherlands -> CA"
    .third_party_api = 100ms,   // spec §6: external APIs, with variability 2.0
};

struct TypeDefaults {
  Rate service_rate;            // per replica
  SimTime service_time;
  double service_variability{1.0};
};

// Pattern: Table-driven method. QPS figures are the spec §6 line "web 2k, cache 100k,
// SQL 5k, KV 50k, queue 1M"; the rest are our own round numbers, marked "chosen".
[[nodiscard]] constexpr TypeDefaults for_type(model::NodeType type) {
  using enum model::NodeType;  // C++20 `using enum`: the enumerators by bare name inside this switch
  switch (type) {
    case client:              return {Rate{0}, 0s};                                   // a source, not a queue
    case cdn:                 return {Rate{100'000}, latency_numbers.memory_read_1mb}; // edge serves from memory
    case load_balancer:       return {Rate{100'000}, 50us};                            // chosen: L7 proxy hop
    case rate_limiter:        return {Rate{100'000}, 50us};                            // chosen: in-memory counter
    case service:             return {Rate{2'000}, 1ms};                               // spec: web 2k; chosen: ~1 ms CPU per request
    case cache:               return {Rate{100'000}, latency_numbers.memory_read_1mb}; // spec: cache 100k
    case sql_db:              return {Rate{5'000}, latency_numbers.ssd_random_read};   // spec: SQL 5k reads; SSD read
    case kv_cluster:          return {Rate{50'000}, latency_numbers.ssd_random_read};  // spec: KV 50k
    case message_queue:       return {Rate{1'000'000}, 100us};                         // spec: queue 1M; chosen: append
    case worker_pool:         return {Rate{1'000}, 2ms};                               // chosen: 2 ms job
    case blob_store:          return {Rate{1'000}, latency_numbers.disk_read_1mb};     // spec: 500 MB/s; phase 1 counts ~0.5 MB objects
    case search_index:        return {Rate{5'000}, 5ms};                               // chosen
    case ws_gateway:          return {Rate{50'000}, 100us};                            // chosen
    case service_discovery:   return {Rate{10'000}, latency_numbers.intra_dc_rtt};     // one lookup round trip
    case id_generator:        return {Rate{10'000}, 100us};                            // chosen; snowflake overrides in scenarios
    case external_sink:       return {Rate{1'000}, latency_numbers.third_party_api, 2.0};
    case notification_fanout: return {Rate{10'000}, 1ms};                              // chosen
    case frontier:            return {Rate{10'000}, 1ms};                              // chosen
  }
  return {Rate{0}, 0s};  // unreachable: every enumerator is handled (-Wswitch keeps it so)
}

enum class Source { scenario, type_default, dc_default };

// A resolved number and where it came from, for `explain`.
template <class T>
struct Resolved {
  T value;
  Source source;
};

[[nodiscard]] Resolved<Rate> resolve_service_rate(const model::NodeSpec& n) {
  if (n.capacity) return {*n.capacity, Source::scenario};
  return {for_type(n.type).service_rate, Source::type_default};
}

[[nodiscard]] Resolved<SimTime> resolve_service_time(const model::NodeSpec& n) {
  if (n.latency) return {*n.latency, Source::scenario};
  return {for_type(n.type).service_time, Source::type_default};
}

// Fold scenario values over type defaults into the engine's Capacity. The fluid
// defaults (queue 1000, yellow 0.70, red 0.90, c = 1) are spec §3.3/§4.
[[nodiscard]] fluid::Capacity resolve_capacity(const model::NodeSpec& n) {
  const auto d = for_type(n.type);
  const auto& a = n.advanced;
  return fluid::Capacity{
      .service_rate = resolve_service_rate(n).value,
      .service_time_s = seconds(resolve_service_time(n).value),
      .concurrency = a.concurrency.value_or(1),
      .queue_limit = a.queue_limit.value_or(1000.0),
      .yellow = a.yellow.value_or(0.70),
      .red = a.red.value_or(0.90),
      .service_variability = a.service_variability.value_or(d.service_variability),
      .burstiness = a.burstiness.value_or(1.0),
  };
}

// One-way link latency: the scenario value, else half the datacenter round trip.
[[nodiscard]] Resolved<SimTime> resolve_link_latency(const model::LinkSpec& l, bool cross_dc) {
  if (l.latency) return {*l.latency, Source::scenario};
  const auto rtt = cross_dc ? latency_numbers.cross_dc_rtt : latency_numbers.intra_dc_rtt;
  return {rtt / 2, Source::dc_default};
}

}  // namespace simsys::defaults
```

`<chrono>` in this interface's global fragment while importing other modules: allowed by the `CLAUDE.md` rule (only `<format>`-bearing headers break GCC 14; `<chrono>` includes `<format>` in libstdc++ 14 — **so this may trigger the bug**). If the build fails with "Bad file data", move everything that needs chrono literals into an implementation unit `src/defaults/defaults.cpp` and keep the interface to declarations plus the `LatencyNumbers` struct, with the constants defined as `SimTime{250'000}` etc. Record which variant worked in the commit message.

- [ ] **Step 4: Run `[defaults]`, 7 pass. Step 5: Commit** `defaults: back-of-the-envelope table and resolution with provenance`

---

### Task 7: Typed common node keys and top-level keys in the loader

**Files:**
- Modify: `src/config/config.cpp`, `tests/unit/test_config.cpp`

- [ ] **Step 1: Write the failing tests** (append to `test_config.cpp`; reuse the existing `TempFile` and `require_rejected` helpers)

```cpp
TEST_CASE("common node keys are typed: dc, replicas, capacity, latency, advanced", "[config]") {
  const TempFile file{R"(
version: 1
name: typed
duration: 1m
datacenters: {eu: {}, us: {}}
nodes:
  users: {type: client, dc: eu, rate: {base: 10/s}, ops: {ping: 1.0}}
  api:
    type: service
    dc: us
    replicas: 4
    capacity: 3k/s
    latency: 0.3ms
    advanced: {queue_limit: 50, concurrency: 8, service_variability: 1.5, burstiness: 2, yellow: 0.6, red: 0.8}
links:
  - {from: users, to: api, dir: inout}
)"};
  const auto s = simsys::config::load(file.path());
  REQUIRE(s.has_value());
  const auto& api = s->nodes[1];
  REQUIRE(api.dc == "us");
  REQUIRE(api.replicas == 4);
  REQUIRE(api.capacity == simsys::Rate{3000});
  REQUIRE(api.latency == 300us);
  REQUIRE(api.advanced.queue_limit == 50.0);
  REQUIRE(api.advanced.concurrency == 8);
  REQUIRE(api.advanced.service_variability == 1.5);
  REQUIRE(api.advanced.burstiness == 2.0);
  REQUIRE(api.advanced.yellow == 0.6);
  REQUIRE(api.advanced.red == 0.8);
  REQUIRE(s->datacenters == std::vector<std::string>{"eu", "us"});
}

TEST_CASE("top-level seed, step and warmup are typed with defaults", "[config]") {
  const TempFile file{"version: 1\nname: t\nduration: 2h\nseed: 7\nstep: 50ms\nwarmup: 1m\n"
                      "nodes: {a: {type: service}}\nlinks: []\n"};
  const auto s = simsys::config::load(file.path());
  REQUIRE(s.has_value());
  REQUIRE(s->seed == 7);
  REQUIRE(s->step == 50ms);
  REQUIRE(s->warmup == 1min);
  const TempFile bare{"version: 1\nname: t\nduration: 2h\nnodes: {a: {type: service}}\nlinks: []\n"};
  const auto b = simsys::config::load(bare.path());
  REQUIRE(b->seed == 1);
  REQUIRE(b->step == 100ms);
  REQUIRE(b->warmup == 0s);
  REQUIRE(b->datacenters == std::vector<std::string>{"main"});
}

TEST_CASE("a node's dc must be a declared datacenter", "[config]") {
  const TempFile file{"version: 1\nname: t\nduration: 1m\ndatacenters: {eu: {}}\n"
                      "nodes: {a: {type: service, dc: mars}}\nlinks: []\n"};
  require_rejected(simsys::config::load(file.path()), "node 'a': unknown datacenter 'mars'");
}

TEST_CASE("replicas must be a positive integer", "[config]") {
  const TempFile file{"version: 1\nname: t\nduration: 1m\nnodes: {a: {type: service, replicas: 0}}\nlinks: []\n"};
  require_rejected(simsys::config::load(file.path()), "node 'a': replicas must be a positive integer");
}

TEST_CASE("advanced thresholds must satisfy 0 < yellow < red <= 1", "[config]") {
  const TempFile file{"version: 1\nname: t\nduration: 1m\n"
                      "nodes: {a: {type: service, advanced: {yellow: 0.9, red: 0.8}}}\nlinks: []\n"};
  require_rejected(simsys::config::load(file.path()), "node 'a': advanced.yellow must be below advanced.red");
}

TEST_CASE("unknown keys are rejected at the top level and inside a node", "[config]") {
  const TempFile top{"version: 1\nname: t\nduration: 1m\ncolour: blue\nnodes: {a: {type: service}}\nlinks: []\n"};
  require_rejected(simsys::config::load(top.path()), "unknown top-level key 'colour'");
  const TempFile node{"version: 1\nname: t\nduration: 1m\nnodes: {a: {type: service, ttl: 5s}}\nlinks: []\n"};
  require_rejected(simsys::config::load(node.path()), "node 'a': key 'ttl' is not valid for type 'service'");
}

TEST_CASE("a byte-rate capacity is stored separately from a request-rate capacity", "[config]") {
  const TempFile file{"version: 1\nname: t\nduration: 1m\nnodes: {b: {type: blob_store, capacity: 500MB/s}}\nlinks: []\n"};
  const auto s = simsys::config::load(file.path());
  REQUIRE(s.has_value());
  REQUIRE_FALSE(s->nodes[0].capacity.has_value());
  REQUIRE(s->nodes[0].byte_capacity == simsys::ByteRate{500e6});
}
```

`require_rejected` in this file takes `(result, message)`; keep its existing signature (check the file).

- [ ] **Step 2: Run, expect failures (fields unset, unknown keys accepted)**

- [ ] **Step 3: Implement in `config.cpp`**

Add, as separate short functions in the validation pipeline: `read_datacenters` (map; names; default `{"main"}`), `read_header_options` (`seed` int, `step`/`warmup` durations), `check_top_level_keys` against `{version, name, seed, duration, step, warmup, datacenters, nodes, links, slo, faults}` ("unknown top-level key 'x'"), `read_common_keys(NodeSpec&, node)` (`dc` in datacenters; `replicas` positive int; `capacity` → `parse_rate` else `parse_byte_rate`; `latency` → `parse_duration`; `advanced` block with ranges: `queue_limit > 0`, `concurrency ≥ 1`, `service_variability ≥ 0`, `burstiness ≥ 0`, `0 < yellow < red ≤ 1` — messages `node '<id>': advanced.<key> must be ...`), and `check_node_keys` using a per-type allowed-key table:

```cpp
// Pattern: Table-driven method. Common keys plus each type's own keys (docs/scenario-schema.md).
constexpr std::array common_keys{"type", "dc", "replicas", "capacity", "latency", "ops", "then", "advanced"};
constexpr std::array<std::span<const std::string_view>, 18> own_keys_by_type = { ... };
```

with the own-key lists copied from the schema table (client: `rate ops keys sizes timeout retries`; cdn: `ttl hit_ratio origin`; load_balancer: `policy vnodes`; rate_limiter: `algorithm limit burst backend`; service: none; cache: `eviction strategy keys hit_ratio backend ttl then`; sql_db: `replication_lag shards pool`; kv_cluster: `nodes vnodes replication write_quorum read_quorum mode`; message_queue: `partitions retention`; worker_pool: `consumes workers cost then`; blob_store: `then`; search_index: `build_interval`; ws_gateway: `max_connections heartbeat fanout`; service_discovery: `failover`; id_generator: `algorithm`; external_sink: `failure_rate politeness`; notification_fanout: `channels dedup`; frontier: `politeness hosts then`). Message: `node '<id>': key '<k>' is not valid for type '<t>'`. Run every shipped scenario after this (`scenario_tests`) — any key they use that the table lacks is a bug in the table, not in the scenario, unless the schema doc says otherwise.

- [ ] **Step 4: Run all tests. Step 5: Commit** `config: type the common node keys and reject unknown keys`

---

### Task 8: Typed client workload, steps, links and SLOs

**Files:**
- Modify: `src/config/config.cpp`, `tests/unit/test_config.cpp`

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("a client's rate.base, op shares and timeout are typed", "[config]") {
  const TempFile file{"version: 1\nname: t\nduration: 1m\n"
                      "nodes:\n  u: {type: client, rate: {base: 12.8k/s}, ops: {redirect: 0.91, shorten: 0.09}, timeout: 1s}\n"
                      "links: []\n"};
  const auto s = simsys::config::load(file.path());
  REQUIRE(s.has_value());
  const auto& w = s->nodes[0].workload;
  REQUIRE(w.has_value());
  REQUIRE(w->base == simsys::Rate{12800});
  REQUIRE(w->op_shares.size() == 2);
  REQUIRE(w->op_shares[0].first == "redirect");
  REQUIRE(w->op_shares[0].second == Catch::Approx(0.91));
  REQUIRE(w->timeout == 1s);
}

TEST_CASE("client op shares must sum to one", "[config]") {
  const TempFile file{"version: 1\nname: t\nduration: 1m\n"
                      "nodes: {u: {type: client, rate: {base: 1/s}, ops: {a: 0.5, b: 0.4}}}\nlinks: []\n"};
  require_rejected(simsys::config::load(file.path()), "node 'u': op shares sum to 0.9, expected 1");
}

TEST_CASE("a client needs rate.base", "[config]") {
  const TempFile file{"version: 1\nname: t\nduration: 1m\nnodes: {u: {type: client, ops: {a: 1}}}\nlinks: []\n"};
  require_rejected(simsys::config::load(file.path()), "node 'u': client needs rate.base");
}

TEST_CASE("service steps are typed with their defaults", "[config]") {
  const TempFile file{"version: 1\nname: t\nduration: 1m\n"
                      "nodes:\n"
                      "  web: {type: service, ops: {read: [{to: cache}, {to: db, op: select, on: miss, mode: parallel, factor: 3}]}}\n"
                      "  cache: {type: cache}\n  db: {type: sql_db}\n"
                      "links: [{from: web, to: cache}, {from: web, to: db}]\n"};
  const auto s = simsys::config::load(file.path());
  REQUIRE(s.has_value());
  const auto& steps = s->nodes[0].ops.at("read");
  REQUIRE(steps.size() == 2);
  REQUIRE(steps[0].to == "cache");
  REQUIRE(steps[0].op == "read");  // defaults to the incoming op
  REQUIRE(steps[0].on == simsys::model::StepCondition::always);
  REQUIRE(steps[0].mode == simsys::model::StepMode::sequential);
  REQUIRE(steps[0].factor == 1.0);
  REQUIRE(steps[1].op == "select");
  REQUIRE(steps[1].on == simsys::model::StepCondition::miss);
  REQUIRE(steps[1].mode == simsys::model::StepMode::parallel);
  REQUIRE(steps[1].factor == 3.0);
}

TEST_CASE("step factor must be positive and on/mode must be known words", "[config]") {
  const TempFile f{"version: 1\nname: t\nduration: 1m\nnodes: {a: {type: service, ops: {x: [{to: a, factor: 0}]}}}\nlinks: []\n"};
  require_rejected(simsys::config::load(f.path()), "node 'a': step factor must be positive");
  const TempFile m{"version: 1\nname: t\nduration: 1m\nnodes: {a: {type: service, ops: {x: [{to: a, mode: sideways}]}}}\nlinks: []\n"};
  require_rejected(simsys::config::load(m.path()), "node 'a': step mode must be sequential or parallel");
}

TEST_CASE("links are typed: direction, ops filter, latency, weight", "[config]") {
  const TempFile file{"version: 1\nname: t\nduration: 1m\nnodes: {a: {type: service}, b: {type: service}}\n"
                      "links:\n  - {from: a, to: b, dir: inout, ops: [read, write], latency: 2ms, weight: 0.5}\n  - {from: b, to: a}\n"};
  const auto s = simsys::config::load(file.path());
  REQUIRE(s.has_value());
  REQUIRE(s->links[0].dir == simsys::model::Direction::inout);
  REQUIRE(s->links[0].ops == std::vector<std::string>{"read", "write"});
  REQUIRE(s->links[0].latency == 2ms);
  REQUIRE(s->links[0].weight == 0.5);
  REQUIRE(s->links[1].dir == simsys::model::Direction::out);
  REQUIRE(s->links[1].ops.empty());
  REQUIRE_FALSE(s->links[1].latency.has_value());
}

TEST_CASE("slo entries are typed; latency maxima are durations, ratios are numbers", "[config]") {
  const TempFile file{"version: 1\nname: t\nduration: 1m\nnodes: {u: {type: client, rate: {base: 1/s}, ops: {a: 1}}}\nlinks: []\n"
                      "slo:\n  - {metric: p99_latency, node: u, op: a, max: 50ms}\n  - {metric: drop_rate, max: 0.001}\n"};
  const auto s = simsys::config::load(file.path());
  REQUIRE(s.has_value());
  REQUIRE(s->slos.size() == 2);
  REQUIRE(s->slos[0].metric == simsys::model::SloMetric::p99_latency);
  REQUIRE(std::get<simsys::SimTime>(s->slos[0].max) == 50ms);
  REQUIRE(s->slos[1].metric == simsys::model::SloMetric::drop_rate);
  REQUIRE(std::get<double>(s->slos[1].max) == 0.001);
}

TEST_CASE("an unknown slo metric is rejected", "[config]") {
  const TempFile file{"version: 1\nname: t\nduration: 1m\nnodes: {a: {type: service}}\nlinks: []\nslo: [{metric: happiness, max: 1}]\n"};
  require_rejected(simsys::config::load(file.path()), "slo: unknown metric 'happiness'");
}
```

- [ ] **Step 2: Run, expect failures**

- [ ] **Step 3: Implement** — short functions `read_workload` (client: `rate` map must have `base`; `ops` map of numbers in (0, 1], sorted by op, sum within 1e-6 of 1 → message with the sum printed via `std::format("{:g}")`; `timeout`), `read_step` (`to`, `op` default incoming, `on`, `mode`, `factor`), `read_steps_for(node, "ops")`/`"then"`, `read_link` (dir words `in|out|inout`, `ops` list of scalars, `latency`, `weight ≥ 0`), `read_slo` (metric table, `node`, `op`, `max` as duration for `*_latency` else number). The existing structural checks (`check_step`, `check_op_steps`...) become the typed readers; delete what they superseded so there is one pass, not two.

- [ ] **Step 4: All tests pass; every shipped scenario still validates. Step 5: Commit** `config: type client workloads, steps, links and SLOs`

---

## Part B — Graph, behaviours, engine, reports, CLI

### Task 9: `Graph::build` — resolved nodes, demand edges, order, feedback

**Files:**
- Create: `src/engine/graph.cppm`
- Create: `tests/unit/test_graph.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

Tests build scenarios in code with a small helper so no YAML is involved.

- [ ] **Step 1: Write the failing tests**

```cpp
// The resolved graph: who sends demand to whom, in what order, through which link.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

import simsys.core;
import simsys.defaults;
import simsys.graph;
import simsys.model;

using namespace std::chrono_literals;
using Catch::Approx;
using simsys::Rate;
using simsys::graph::Graph;
using simsys::model::LinkSpec;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
using simsys::model::Step;
using simsys::model::Workload;

namespace {
NodeSpec client(std::string id, Rate base, std::vector<std::pair<std::string, double>> ops) {
  return NodeSpec{.id = std::move(id), .type = NodeType::client,
                  .workload = Workload{.base = base, .op_shares = std::move(ops)}};
}
NodeSpec service(std::string id, std::map<std::string, std::vector<Step>> ops = {}) {
  return NodeSpec{.id = std::move(id), .type = NodeType::service, .ops = std::move(ops)};
}
Scenario chain() {  // users -> api -> db
  Scenario s{.name = "chain", .version = 1, .duration = 1min};
  s.nodes = {client("users", Rate{100}, {{"get", 1.0}}),
             service("api", {{"get", {Step{.to = "db", .op = "select"}}}}), service("db")};
  s.links = {LinkSpec{.from = "users", .to = "api", .dir = simsys::model::Direction::inout},
             LinkSpec{.from = "api", .to = "db", .dir = simsys::model::Direction::inout}};
  return s;
}
}  // namespace

TEST_CASE("nodes keep scenario order and get resolved capacities", "[graph]") {
  const auto g = Graph::build(chain());
  REQUIRE(g.has_value());
  REQUIRE(g->nodes().size() == 3);
  REQUIRE(g->nodes()[1].spec.id == "api");
  REQUIRE(g->nodes()[1].capacity.service_rate == Rate{2000});  // service default
  REQUIRE(g->nodes()[1].service_rate_source == simsys::defaults::Source::type_default);
  REQUIRE(g->index_of("db") == 2);
  REQUIRE_FALSE(g->index_of("nope").has_value());
}

TEST_CASE("topological order puts every node after the nodes that send to it", "[graph]") {
  Scenario s = chain();
  std::swap(s.nodes[0], s.nodes[2]);  // db, api, users in the file
  const auto g = Graph::build(s);
  REQUIRE(g.has_value());
  const auto order = g->topological_order();
  const auto pos = [&](std::string_view id) {
    return std::ranges::find(order, *g->index_of(id)) - order.begin();
  };
  REQUIRE(pos("users") < pos("api"));
  REQUIRE(pos("api") < pos("db"));
}

TEST_CASE("links resolve one-way latency from the datacenters and compute delay steps", "[graph]") {
  Scenario s = chain();
  s.datacenters = {"us", "eu"};
  s.nodes[0].dc = "us";
  s.nodes[1].dc = "us";
  s.nodes[2].dc = "eu";
  const auto g = Graph::build(s);
  REQUIRE(g.has_value());
  REQUIRE(g->links()[0].one_way == 250us);
  REQUIRE(g->links()[0].delay_steps == 0);                 // 250 us < 100 ms step
  REQUIRE(g->links()[1].one_way == 75ms);
  REQUIRE(g->links()[1].latency_source == simsys::defaults::Source::dc_default);
  REQUIRE(g->links()[1].delay_steps == 1);                 // ceil(75 ms / 100 ms)
  s.links[1].latency = 250ms;
  REQUIRE(Graph::build(s)->links()[1].delay_steps == 3);
}

TEST_CASE("a step without a connecting link is an error", "[graph]") {
  Scenario s = chain();
  s.links.pop_back();  // api -> db missing
  const auto g = Graph::build(s);
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message == "node 'api' sends 'select' to 'db' but no link connects them");
}

TEST_CASE("a step's op must be allowed by the link's ops filter", "[graph]") {
  Scenario s = chain();
  s.links[1].ops = {"insert"};
  const auto g = Graph::build(s);
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message == "link api -> db does not carry op 'select' (ops: insert)");
}

TEST_CASE("client ops are routed to the links that carry them, split by weight", "[graph]") {
  Scenario s;
  s.name = "fanout"; s.version = 1; s.duration = 1min;
  s.nodes = {client("u", Rate{100}, {{"stream", 0.5}, {"meta", 0.5}}), service("cdn"), service("lb"), service("lb2")};
  s.links = {LinkSpec{.from = "u", .to = "cdn", .ops = {"stream"}},
             LinkSpec{.from = "u", .to = "lb", .ops = {"meta"}, .weight = 3},
             LinkSpec{.from = "u", .to = "lb2", .ops = {"meta"}, .weight = 1}};
  const auto g = Graph::build(s);
  REQUIRE(g.has_value());
  const auto routes = g->client_routes(*g->index_of("u"));
  REQUIRE(routes.size() == 3);
  const auto to_lb = std::ranges::find(routes, *g->index_of("lb"), &simsys::graph::ClientRoute::target);
  REQUIRE(to_lb != routes.end());
  REQUIRE(to_lb->op == "meta");
  REQUIRE(to_lb->share == Approx(0.75));
}

TEST_CASE("a client op with no link to carry it is an error", "[graph]") {
  Scenario s = chain();
  s.nodes[0].workload->op_shares = {{"get", 0.5}, {"put", 0.5}};
  s.links[0].ops = {"get"};
  const auto g = Graph::build(s);
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message == "client 'users': no link carries op 'put'");
}

TEST_CASE("a cycle becomes a feedback edge with at least one step of delay", "[graph]") {
  Scenario s;
  s.name = "loop"; s.version = 1; s.duration = 1min;
  s.nodes = {client("u", Rate{1}, {{"msg", 1.0}}),
             service("chat", {{"msg", {Step{.to = "q", .op = "publish"}}}}),
             service("q", {{"publish", {Step{.to = "sync", .op = "job"}}}}),
             service("sync", {{"job", {Step{.to = "chat", .op = "push"}}}})};
  s.links = {LinkSpec{.from = "u", .to = "chat"}, LinkSpec{.from = "chat", .to = "q"},
             LinkSpec{.from = "q", .to = "sync"}, LinkSpec{.from = "sync", .to = "chat"}};
  const auto g = Graph::build(s);
  REQUIRE(g.has_value());
  REQUIRE(g->topological_order().size() == 4);
  const auto back = std::ranges::find_if(g->links(), [](const auto& l) { return l.feedback; });
  REQUIRE(back != g->links().end());
  REQUIRE(back->delay_steps >= 1);
  REQUIRE(std::ranges::count_if(g->links(), [](const auto& l) { return l.feedback; }) == 1);
}
```

- [ ] **Step 2: Register, run, expect failure**

- [ ] **Step 3: Implement**

```cpp
// Module `simsys.graph`: the scenario resolved into something the engine can step —
// node capacities with provenance, link latencies and delay-line lengths, demand edges,
// a topological order, and feedback edges for cycles (spec §3.1: cycles use the previous
// step's value, which a one-step delay line gives us for free).
module;
#include <algorithm>
#include <cmath>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module simsys.graph;

import simsys.core;
import simsys.defaults;
import simsys.fluid;
import simsys.model;

export namespace simsys::graph {

struct EngineError { std::string message; };

struct ResolvedNode {
  model::NodeSpec spec;
  fluid::Capacity capacity;
  defaults::Source service_rate_source;
  defaults::Source service_time_source;
};

struct ResolvedLink {
  std::size_t from, to;
  model::Direction dir;
  SimTime one_way;
  defaults::Source latency_source;
  int delay_steps;          // ceil(one_way / step) when one_way > step, else 0; >= 1 if feedback
  bool feedback{false};
  std::vector<std::string> ops;
  double weight;
};

// Where a client sends one op and which share of that op's rate goes there.
struct ClientRoute {
  std::string op;
  std::size_t target;
  std::size_t link;
  double share;
};

// A demand edge: `from` forwards flow to `to` over `link` because of `step` (or a client route).
struct DemandEdge { std::size_t from, to, link; };

class Graph {
 public:
  [[nodiscard]] static std::expected<Graph, EngineError> build(model::Scenario scenario);

  [[nodiscard]] const model::Scenario& scenario() const { return scenario_; }
  [[nodiscard]] std::span<const ResolvedNode> nodes() const { return nodes_; }
  [[nodiscard]] std::span<const ResolvedLink> links() const { return links_; }
  [[nodiscard]] std::span<const std::size_t> topological_order() const { return order_; }
  [[nodiscard]] std::optional<std::size_t> index_of(std::string_view id) const;
  // The link that carries demand from `from` to `to`: (from,to,out|inout) or (to,from,in).
  [[nodiscard]] std::optional<std::size_t> demand_link(std::size_t from, std::size_t to) const;
  [[nodiscard]] std::span<const ClientRoute> client_routes(std::size_t client) const;

 private:
  // Pattern: Builder steps as private member functions, each one named after the thing
  // it resolves: resolve_nodes, resolve_links, connect_steps, route_clients, order_nodes.
  model::Scenario scenario_;
  std::vector<ResolvedNode> nodes_;
  std::vector<ResolvedLink> links_;
  std::vector<DemandEdge> edges_;
  std::vector<std::vector<ClientRoute>> routes_;   // indexed by node index (empty for non-clients)
  std::vector<std::size_t> order_;
};

}  // namespace simsys::graph
```

Implementation notes:
- `resolve_links`: `cross_dc = nodes[from].dc != nodes[to].dc`; `delay_steps = one_way > step ? ceil(seconds(one_way)/seconds(step)) : 0`.
- `connect_steps`: for every node, every step in `ops` values and `then`: target index (already validated by config), `demand_link` must exist → message `node 'api' sends 'select' to 'db' but no link connects them`; if the link's `ops` is non-empty and does not contain the step op → `link api -> db does not carry op 'select' (ops: insert)` (ops joined with `, `). Push a `DemandEdge`. Use `std::views::values`/`std::views::join` (C++20 ranges; comment the first use) to walk steps.
- `route_clients`: for every client op: eligible = outgoing links `(client, x, out|inout)` whose ops contain the op or are empty; none → `client 'users': no link carries op 'put'`; `share = weight / Σ weights` (all weights 0 → equal shares); push `ClientRoute` and a `DemandEdge`.
- `order_nodes`: iterative DFS over `edges_` in node-index order (determinism). On a back edge mark its link `feedback = true` and `delay_steps = max(delay_steps, 1)`; order = reverse post-order. All nodes are placed (isolated nodes included).

- [ ] **Step 4: Run `[graph]`, 9 pass. Step 5: Commit** `graph: resolve nodes and links, order the demand graph, mark feedback edges`

---

### Task 10: Behaviours and the `NodeBehaviour` concept

**Files:**
- Create: `src/engine/behaviours.cppm`
- Create: `tests/unit/test_behaviours.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing tests**

```cpp
// What client and service nodes do with flow and how they compose response latency.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <map>
#include <string>
#include <vector>

import simsys.behaviours;
import simsys.core;
import simsys.flow;
import simsys.graph;
import simsys.model;

using namespace std::chrono_literals;
using Catch::Approx;
using simsys::Rate;
using simsys::behaviours::ClientBehaviour;
using simsys::behaviours::ServiceBehaviour;
using simsys::flow::Flow;
using simsys::flow::LatencyEstimate;
using simsys::model::LinkSpec;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
using simsys::model::Step;
using simsys::model::StepMode;
using simsys::model::Workload;

namespace {
// users -> web -> {cache, db}; web.get = [cache.read, db.select]
Scenario two_hop(StepMode mode) {
  Scenario s{.name = "t", .version = 1, .duration = 1min};
  s.nodes = {NodeSpec{.id = "users", .type = NodeType::client,
                      .workload = Workload{.base = Rate{100}, .op_shares = {{"get", 0.8}, {"put", 0.2}}}},
             NodeSpec{.id = "web", .type = NodeType::service,
                      .ops = {{"get", {Step{.to = "cache", .op = "read", .mode = mode},
                                       Step{.to = "db", .op = "select", .mode = mode, .factor = 2}}}}},
             NodeSpec{.id = "cache", .type = NodeType::service}, NodeSpec{.id = "db", .type = NodeType::service}};
  s.links = {LinkSpec{.from = "users", .to = "web", .latency = 1ms}, LinkSpec{.from = "web", .to = "cache", .latency = 0s},
             LinkSpec{.from = "web", .to = "db", .latency = 10ms}};
  return s;
}
}  // namespace

static_assert(simsys::behaviours::NodeBehaviour<ServiceBehaviour>);
static_assert(simsys::behaviours::RequestSource<ClientBehaviour>);

TEST_CASE("a client offers base rate times op share along its routes", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(two_hop(StepMode::sequential));
  const ClientBehaviour c{g, *g.index_of("users")};
  const auto routed = c.offer(0s);
  REQUIRE(routed.outgoing.size() == 1);
  REQUIRE(routed.outgoing[0].target == *g.index_of("web"));
  REQUIRE(routed.outgoing[0].flow.rate_of("get").per_second == Approx(80));
  REQUIRE(routed.outgoing[0].flow.rate_of("put").per_second == Approx(20));
}

TEST_CASE("a service forwards each step's op scaled by its factor and serves the rest", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(two_hop(StepMode::sequential));
  const ServiceBehaviour web{g, *g.index_of("web")};
  Flow in;
  in.add("get", Rate{50});
  in.add("put", Rate{5});  // no steps: terminal op, served locally
  const auto routed = web.route(in);
  REQUIRE(routed.outgoing.size() == 2);
  const auto to_db = std::ranges::find(routed.outgoing, *g.index_of("db"), &simsys::behaviours::Outgoing::target);
  REQUIRE(to_db->flow.rate_of("select").per_second == Approx(100));  // factor 2
  REQUIRE(routed.served.total().per_second == Approx(55));
}

TEST_CASE("sequential steps add hop latencies: two link legs plus the downstream response", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(two_hop(StepMode::sequential));
  const ServiceBehaviour web{g, *g.index_of("web")};
  const LatencyEstimate own{0.001, 0.0};
  const auto downstream = [&](std::size_t node, std::string_view) {
    return node == *g.index_of("db") ? LatencyEstimate{0.005, 1e-6} : LatencyEstimate{0.0002, 0.0};
  };
  const auto r = web.response("get", own, downstream);
  // own 1 ms + cache (0 + 0.2 ms + 0) + db (10 ms + 5 ms + 10 ms) = 26.2 ms
  REQUIRE(r.mean_s == Approx(0.0262));
  REQUIRE(r.variance_s2 == Approx(1e-6));
}

TEST_CASE("parallel steps add the slowest branch instead of the sum", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(two_hop(StepMode::parallel));
  const ServiceBehaviour web{g, *g.index_of("web")};
  const auto downstream = [&](std::size_t node, std::string_view) {
    return node == *g.index_of("db") ? LatencyEstimate{0.005, 1e-6} : LatencyEstimate{0.0002, 4e-6};
  };
  const auto r = web.response("get", LatencyEstimate{0.001, 0.0}, downstream);
  REQUIRE(r.mean_s == Approx(0.001 + 0.025));
  REQUIRE(r.variance_s2 == Approx(4e-6));  // largest variance among branches
}

TEST_CASE("an op without steps responds with the node's own sojourn", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(two_hop(StepMode::sequential));
  const ServiceBehaviour web{g, *g.index_of("web")};
  const auto r = web.response("put", LatencyEstimate{0.003, 2e-6}, [](std::size_t, std::string_view) {
    return LatencyEstimate{99.0, 99.0};
  });
  REQUIRE(r == LatencyEstimate{0.003, 2e-6});
}

TEST_CASE("a client's response adds its link's two legs to the first hop's response", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(two_hop(StepMode::sequential));
  const ClientBehaviour c{g, *g.index_of("users")};
  const auto r = c.response("get", [](std::size_t, std::string_view) { return LatencyEstimate{0.004, 1e-6}; });
  REQUIRE(r.mean_s == Approx(0.001 + 0.004 + 0.001));
}
```

- [ ] **Step 2: Register, run, expect failure**

- [ ] **Step 3: Implement**

```cpp
// Module `simsys.behaviours`: what each node type does with flow. Phase 1 ships the two
// behaviours the tutorial scenarios need; phase 2 adds the rest of the catalogue.
module;
#include <concepts>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

export module simsys.behaviours;

import simsys.core;
import simsys.flow;
import simsys.graph;
import simsys.model;

export namespace simsys::behaviours {

struct Outgoing {
  std::size_t target;
  std::size_t link;
  flow::Flow flow;
};

struct Routed {
  std::vector<Outgoing> outgoing;
  flow::Flow served;      // what this node handled itself (every incoming op, after drops)
};

// Anything that can look up the response latency of a downstream (node, op).
template <class F>
concept ResponseLookup = std::invocable<F, std::size_t, std::string_view> &&
    std::same_as<std::invoke_result_t<F, std::size_t, std::string_view>, flow::LatencyEstimate>;

// C++20 concepts: the engine's requirement on a behaviour, stated in code instead of a
// comment or a virtual base class. Satisfied by duck typing at compile time; a type that
// does not fit fails with a message naming the missing member. Spec §5.
template <class B>
concept NodeBehaviour = requires(const B b, const flow::Flow& in, flow::LatencyEstimate own) {
  { b.route(in) } -> std::same_as<Routed>;
  { b.response(std::string_view{}, own, [](std::size_t, std::string_view) { return flow::LatencyEstimate{}; }) }
      -> std::same_as<flow::LatencyEstimate>;
};

template <class S>
concept RequestSource = requires(const S s, SimTime now) {
  { s.offer(now) } -> std::same_as<Routed>;
};

// Pattern: Strategy — the engine holds one of these per node and dispatches with
// std::visit; the variant replaces an abstract base class and keeps every behaviour a
// plain value. Phase 2 appends more alternatives.
class ClientBehaviour;
class ServiceBehaviour;
using Behaviour = std::variant<ClientBehaviour, ServiceBehaviour>;

// Models the `service` node: a stateless tier (book: web servers, API servers). For each
// incoming op it forwards the configured steps and counts the op as served; latency seen
// by its caller is its own sojourn plus, for sequential steps, each hop's two link legs
// and downstream response, and for parallel steps the slowest such hop.
class ServiceBehaviour {
 public:
  ServiceBehaviour(const graph::Graph& g, std::size_t self);
  [[nodiscard]] Routed route(const flow::Flow& in) const;
  [[nodiscard]] flow::LatencyEstimate response(std::string_view op, flow::LatencyEstimate own,
                                               ResponseLookup auto&& downstream) const;
 private:
  struct Hop { std::string op; std::size_t target; std::size_t link; double factor; model::StepMode mode; double link_leg_s; };
  std::vector<std::pair<std::string, std::vector<Hop>>> hops_;  // sorted by incoming op
};

class ClientBehaviour {
 public:
  ClientBehaviour(const graph::Graph& g, std::size_t self);
  [[nodiscard]] Routed offer(SimTime now) const;   // phase 1: constant base rate
  [[nodiscard]] flow::LatencyEstimate response(std::string_view op, ResponseLookup auto&& downstream) const;
  [[nodiscard]] std::span<const std::string> ops() const;
 private:
  ...
};

}  // namespace simsys::behaviours
```

Notes: `on != always` steps are skipped by a service (it has no misses); `response` = `own + Σ sequential hops + parallel_max(parallel hops)` where a hop = `{2·link_leg_s, 0} + downstream(target, hop.op)` (the link leg's variance is 0: fixed propagation delay). Client response per op = Σ over its routes of `share × hop` (a weighted mixture: mean = Σ share·mean; variance via the law of total variance — comment it). `static_assert(NodeBehaviour<ServiceBehaviour>)` at the bottom of the module.

- [ ] **Step 4: Run `[behaviours]`, 6 pass. Step 5: Commit** `behaviours: client and service with the NodeBehaviour concept`

---

### Task 11: `Simulation::step` and `estimate`

**Files:**
- Create: `src/engine/engine.cppm`
- Create: `tests/theory/test_theory.cpp` (first batch), `tests/unit/test_engine.cpp` (delay line)
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt` (new `theory_tests` executable)

- [ ] **Step 1: Write the failing tests**

`tests/unit/test_engine.cpp`:

```cpp
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

import simsys.core;
import simsys.engine;
import simsys.flow;

using simsys::Rate;
using simsys::engine::DelayLine;
using simsys::flow::Flow;

TEST_CASE("a zero-step delay line passes flow through in the same step", "[engine]") {
  DelayLine line{0};
  Flow f; f.add("x", Rate{5});
  line.push(f);
  REQUIRE(line.pop().rate_of("x").per_second == 5);
  REQUIRE(line.pop().empty());
}

TEST_CASE("an n-step delay line returns a flow n pops later", "[engine]") {
  DelayLine line{2};
  Flow f; f.add("x", Rate{5});
  line.push(f);
  REQUIRE(line.pop().empty());  // step 0
  line.push(Flow{});
  REQUIRE(line.pop().empty());  // step 1
  line.push(Flow{});
  REQUIRE(line.pop().rate_of("x").per_second == 5);  // step 2
}
```

`tests/theory/test_theory.cpp` (batch 1):

```cpp
// Whole-simulation checks against closed-form queueing theory (spec §12).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <map>
#include <string>
#include <vector>

import simsys.core;
import simsys.engine;
import simsys.graph;
import simsys.model;

using namespace std::chrono_literals;
using Catch::Approx;
using simsys::Rate;
using simsys::engine::Simulation;
using simsys::model::LinkSpec;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
using simsys::model::Step;
using simsys::model::Workload;

namespace {
// One client, one M/M/1 server: 1000/s capacity, 1 ms service, zero-latency link.
Scenario mm1(double demand) {
  Scenario s{.name = "mm1", .version = 1, .duration = 10s};
  s.nodes = {NodeSpec{.id = "c", .type = NodeType::client, .workload = Workload{.base = Rate{demand}, .op_shares = {{"op", 1.0}}}},
             NodeSpec{.id = "s", .type = NodeType::service, .capacity = Rate{1000}, .latency = 1ms}};
  s.links = {LinkSpec{.from = "c", .to = "s", .dir = simsys::model::Direction::inout, .latency = 0s}};
  return s;
}
}  // namespace

TEST_CASE("one step of an M/M/1 node reproduces rho/(mu - lambda) + service time", "[theory]") {
  const auto g = *simsys::graph::Graph::build(mm1(500));
  Simulation sim{g};
  const auto r = sim.step(0s, 100ms);
  const auto& node = r.nodes[*g.index_of("s")];
  REQUIRE(node.rho == Approx(0.5));
  REQUIRE(node.latency.mean_s == Approx(0.002));
  REQUIRE(r.responses.size() == 1);
  REQUIRE(r.responses[0].op == "op");
  REQUIRE(r.responses[0].response.mean_s == Approx(0.002));  // zero-latency link
}

TEST_CASE("estimate reports the long-run state: empty queue below capacity, full above", "[theory]") {
  const auto under = *simsys::graph::Graph::build(mm1(500));
  REQUIRE(Simulation{under}.estimate().nodes[1].backlog == 0.0);
  const auto over = *simsys::graph::Graph::build(mm1(1500));
  const auto e = Simulation{over}.estimate();
  REQUIRE(e.nodes[1].backlog == Approx(1000));
  REQUIRE(e.nodes[1].dropped_per_s == Approx(500));
  REQUIRE(e.nodes[1].rho == 1.0);
}

TEST_CASE("a simulation refuses node types the phase does not support yet", "[theory]") {
  Scenario s = mm1(1);
  s.nodes[1].type = NodeType::cache;
  const auto g = *simsys::graph::Graph::build(s);
  const auto sim = Simulation::create(g);
  REQUIRE_FALSE(sim.has_value());
  REQUIRE(sim.error().message == "node 's' has type 'cache', which run/estimate do not support yet (phase 2)");
}
```

(`Simulation{g}` in the first two tests is `*Simulation::create(g)`; write it that way.)

- [ ] **Step 2: Register (`theory_tests` links `simsys Catch2::Catch2WithMain simsys_warnings`), run, expect failure**

- [ ] **Step 3: Implement**

```cpp
// Module `simsys.engine`: the step loop (spec §3). One step = a forward pass in
// topological order moving demand through delay lines and fluid queues, then a backward
// pass composing response latency per (node, op). `estimate` is the same evaluation at
// the long-run state. No randomness in phase 1, so runs are byte-identical by construction.
export module simsys.engine;
...
export namespace simsys::engine {

// Ring buffer of flows, one slot per step of link latency (spec §3.4). Pattern: circular buffer.
class DelayLine {
 public:
  explicit DelayLine(int steps);
  void push(flow::Flow arriving);      // what enters the link this step
  [[nodiscard]] flow::Flow pop();      // what leaves the link this step (pushed `steps` steps ago)
};

struct NodeSample {
  double demand_per_s{}, drain_per_s{}, dropped_per_s{}, backlog{}, rho{};
  flow::LatencyEstimate latency;   // own sojourn (0 for clients)
  fluid::Zone zone{};
};
struct ResponseSample { std::size_t client; std::string op; Rate offered; flow::LatencyEstimate response; };
struct StepRecord { SimTime t; std::vector<NodeSample> nodes; std::vector<ResponseSample> responses; };

class Simulation {
 public:
  [[nodiscard]] static std::expected<Simulation, graph::EngineError> create(const graph::Graph&);
  [[nodiscard]] StepRecord step(SimTime now, SimTime dt);
  [[nodiscard]] StepRecord estimate();
 private:
  enum class Mode { advance, settle };
  StepRecord evaluate(SimTime now, SimTime dt, Mode mode);
  void forward_pass(...); void backward_pass(...);
  // per node: Behaviour, optional FluidQueue (services), inflow accumulator, response table
  // per link: DelayLine
};
}
```

Forward pass per node in topological order: client → `offer(now)`; service → `demand = inflow.total()/replicas`; `sample = queue.advance(demand, dt)` or `settle` then an `advance` with the same demand (settle mode); `served_fraction = demand > 0 ? drain/demand : 1`; `routed = behaviour.route(inflow.scaled(served_fraction))`; node sample scaled back by `replicas` for backlog/drain/drops. Each `Outgoing` is pushed into its link's `DelayLine`, and the target's inflow receives `line.pop()` **when the target is evaluated** (pop every link into its target's inflow at the start of that node's evaluation; links not pushed this step get an empty push first — simplest: at the start of each step push nothing, and have `DelayLine::push` be called exactly once per link per step with the accumulated outgoing flow for that link; pops happen when the target is processed; a feedback link's target was processed earlier in the order, so its pop happens next step — this is precisely the "previous step's value" rule).

Backward pass in reverse order: `response_[node][op]` for every op the node received (services) via `behaviour.response(op, sample.latency, lookup)` where `lookup(target, op)` reads the already-computed table (a target later in the order was computed first; a feedback target not yet computed this step uses last step's value — keep the table across steps). Clients produce `ResponseSample` per op with `offered = base × share`.

`create` rejects nodes whose type is not client/service with the message in the test.

- [ ] **Step 4: Run `[engine]` and `[theory]`, pass. Step 5: Commit** `engine: forward demand pass, backward latency pass, estimate`

---

### Task 12: `Simulation::run`, observers, conservation and convergence

**Files:**
- Modify: `src/engine/engine.cppm`, `tests/theory/test_theory.cpp`

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("run emits one record per step from 0 to duration, exclusive", "[theory]") {
  const auto g = *simsys::graph::Graph::build(mm1(500));
  auto sim = *Simulation::create(g);
  std::vector<simsys::SimTime> times;
  sim.run(simsys::engine::RunOptions{.duration = 1s, .step = 100ms},
          [&](const simsys::engine::StepRecord& r) { times.push_back(r.t); });
  REQUIRE(times.size() == 10);
  REQUIRE(times.front() == 0s);
  REQUIRE(times.back() == 900ms);
}

TEST_CASE("flow is conserved at every node in every step of an overloaded chain", "[theory]") {
  // c -> a (fast) -> b (slow, overloaded): in = out + drops + delta backlog, per node, per step.
  Scenario s{.name = "chain", .version = 1, .duration = 5s};
  s.nodes = {NodeSpec{.id = "c", .type = NodeType::client, .workload = Workload{.base = Rate{3000}, .op_shares = {{"op", 1.0}}}},
             NodeSpec{.id = "a", .type = NodeType::service, .capacity = Rate{10000}, .latency = 0.1ms,
                      .ops = {{"op", {Step{.to = "b", .op = "op"}}}}},
             NodeSpec{.id = "b", .type = NodeType::service, .capacity = Rate{1000}, .latency = 1ms, .advanced = {.queue_limit = 200}}};
  s.links = {LinkSpec{.from = "c", .to = "a", .latency = 0s}, LinkSpec{.from = "a", .to = "b", .latency = 0s}};
  const auto g = *simsys::graph::Graph::build(s);
  auto sim = *Simulation::create(g);
  std::vector<double> previous_backlog(3, 0.0);
  sim.run({.duration = 5s, .step = 100ms}, [&](const simsys::engine::StepRecord& r) {
    for (std::size_t i = 1; i < 3; ++i) {
      const auto& n = r.nodes[i];
      const double in = n.demand_per_s * 0.1;
      const double out = (n.drain_per_s + n.dropped_per_s) * 0.1;
      REQUIRE(in == Approx(out + (n.backlog - previous_backlog[i])).margin(1e-6));
      previous_backlog[i] = n.backlog;
    }
  });
}

TEST_CASE("a constant-rate run converges to estimate when no node is saturated", "[theory]") {
  const auto g = *simsys::graph::Graph::build(mm1(800));
  auto sim = *Simulation::create(g);
  simsys::engine::StepRecord last;
  sim.run({.duration = 2s, .step = 100ms}, [&](const simsys::engine::StepRecord& r) { last = r; });
  const auto e = (*Simulation::create(g)).estimate();
  REQUIRE(last.nodes[1].rho == Approx(e.nodes[1].rho));
  REQUIRE(last.nodes[1].latency.mean_s == Approx(e.nodes[1].latency.mean_s));
  REQUIRE(last.responses[0].response.mean_s == Approx(e.responses[0].response.mean_s));
}

TEST_CASE("replicas split the demand: four copies at 1k/s each see rho 0.5 from 2k/s", "[theory]") {
  Scenario s = mm1(2000);
  s.nodes[1].replicas = 4;
  const auto r = (*Simulation::create(*simsys::graph::Graph::build(s))).step(0s, 100ms);
  REQUIRE(r.nodes[1].rho == Approx(0.5));
  REQUIRE(r.nodes[1].drain_per_s == Approx(2000));  // summed over copies
}

TEST_CASE("a cross-datacenter link delays demand by its delay steps", "[theory]") {
  Scenario s = mm1(500);
  s.links[0].latency = 150ms;  // 2 steps at 100 ms
  auto sim = *Simulation::create(*simsys::graph::Graph::build(s));
  REQUIRE(sim.step(0s, 100ms).nodes[1].demand_per_s == 0.0);
  REQUIRE(sim.step(100ms, 100ms).nodes[1].demand_per_s == 0.0);
  REQUIRE(sim.step(200ms, 100ms).nodes[1].demand_per_s == Approx(500));
}

TEST_CASE("two runs of the same scenario produce identical records", "[theory]") {
  const auto g = *simsys::graph::Graph::build(mm1(900));
  auto collect = [&] {
    std::vector<simsys::engine::StepRecord> out;
    (*Simulation::create(g)).run({.duration = 1s, .step = 100ms}, [&](const auto& r) { out.push_back(r); });
    return out;
  };
  const auto a = collect();
  const auto b = collect();
  REQUIRE(a.size() == b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    REQUIRE(a[i].nodes[1].backlog == b[i].nodes[1].backlog);
    REQUIRE(a[i].responses[0].response == b[i].responses[0].response);
  }
}
```

- [ ] **Step 2: Run, expect failure (`run`, `RunOptions` missing)**

- [ ] **Step 3: Implement**

```cpp
struct RunOptions { SimTime duration; SimTime step; };

// Pattern: Observer — the loop hands every StepRecord to a callable and keeps nothing,
// so a 24 h run at 100 ms (864k steps) does not accumulate memory; reports and the
// series writer are observers. C++20 abbreviated function template with a constrained
// `auto` parameter: `StepObserver auto&&` reads as "anything satisfying StepObserver".
template <class F> concept StepObserver = std::invocable<F, const StepRecord&>;

void run(RunOptions options, StepObserver auto&& observer) {
  for (SimTime now{0}; now < options.duration; now += options.step) observer(step(now, options.step));
}
```

- [ ] **Step 4: Run `[theory]`, all pass. Step 5: Commit** `engine: run loop with observers; conservation, convergence and determinism proven`

---

### Task 13: Report — summaries, SLOs, table, JSON, CSV series

**Files:**
- Create: `src/report/report.cppm` (lean interface), `src/report/report.cpp` (uses `<format>`, `<print>`)
- Create: `tests/unit/test_report.cpp`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing tests**

```cpp
// Summaries over a run, SLO evaluation and the three output formats.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <sstream>
#include <string>

import simsys.core;
import simsys.engine;
import simsys.graph;
import simsys.model;
import simsys.report;

using namespace std::chrono_literals;
using Catch::Approx;
using simsys::Rate;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
using simsys::model::Workload;
using simsys::report::Summary;

namespace {
Scenario mm1(double demand, double queue_limit = 1000) {
  Scenario s{.name = "mm1", .version = 1, .duration = 2s, .warmup = 500ms};
  s.nodes = {NodeSpec{.id = "c", .type = NodeType::client, .workload = Workload{.base = Rate{demand}, .op_shares = {{"op", 1.0}}}},
             NodeSpec{.id = "s", .type = NodeType::service, .capacity = Rate{1000}, .latency = 1ms, .advanced = {.queue_limit = queue_limit}}};
  s.links = {simsys::model::LinkSpec{.from = "c", .to = "s", .dir = simsys::model::Direction::inout, .latency = 0s}};
  return s;
}
Summary summarize(const Scenario& s, const simsys::graph::Graph& g) {
  Summary summary{g, s.warmup};
  auto sim = *simsys::engine::Simulation::create(g);
  sim.run({.duration = s.duration, .step = s.step}, [&](const auto& r) { summary.observe(r); });
  return summary;
}
}  // namespace

TEST_CASE("node summaries carry mean and peak utilization, peak backlog, drops and red time", "[report]") {
  const auto s = mm1(1500, 100);
  const auto g = *simsys::graph::Graph::build(s);
  const auto summary = summarize(s, g);
  const auto nodes = summary.nodes();
  REQUIRE(nodes.size() == 2);
  REQUIRE(nodes[1].id == "s");
  REQUIRE(nodes[1].peak_rho == 1.0);
  REQUIRE(nodes[1].mean_rho == Approx(1.0));
  REQUIRE(nodes[1].peak_backlog == Approx(100));
  REQUIRE(nodes[1].total_dropped > 0);
  REQUIRE(nodes[1].red_time_s == Approx(1.5));  // 2 s minus 0.5 s warmup
}

TEST_CASE("op summaries fit percentiles to the time-averaged response moments", "[report]") {
  const auto s = mm1(500);
  const auto g = *simsys::graph::Graph::build(s);
  const auto ops = summarize(s, g).ops();
  REQUIRE(ops.size() == 1);
  REQUIRE(ops[0].client == "c");
  REQUIRE(ops[0].op == "op");
  REQUIRE(ops[0].mean_s == Approx(0.002));
  REQUIRE(ops[0].p50_s < ops[0].mean_s);
  REQUIRE(ops[0].p95_s < ops[0].p99_s);
}

TEST_CASE("warmup steps are excluded from summaries", "[report]") {
  auto s = mm1(500);
  s.warmup = 10s;  // longer than the run: nothing observed
  const auto g = *simsys::graph::Graph::build(s);
  REQUIRE(summarize(s, g).observed_steps() == 0);
}

TEST_CASE("SLOs pass, fail or are skipped when the metric is not available", "[report]") {
  auto s = mm1(500);
  s.slos = {simsys::model::SloSpec{.metric = simsys::model::SloMetric::p99_latency, .node = "c", .op = "op", .max = simsys::SimTime{50ms}},
            simsys::model::SloSpec{.metric = simsys::model::SloMetric::drop_rate, .max = 0.0},
            simsys::model::SloSpec{.metric = simsys::model::SloMetric::utilization, .node = "s", .max = 0.3},
            simsys::model::SloSpec{.metric = simsys::model::SloMetric::queue_lag, .node = "s", .max = 1.0}};
  const auto g = *simsys::graph::Graph::build(s);
  const auto results = simsys::report::evaluate_slos(summarize(s, g), s.slos);
  using simsys::report::SloStatus;
  REQUIRE(results.size() == 4);
  REQUIRE(results[0].status == SloStatus::pass);
  REQUIRE(results[1].status == SloStatus::pass);
  REQUIRE(results[2].status == SloStatus::fail);    // rho 0.5 > 0.3
  REQUIRE(results[3].status == SloStatus::skipped); // queue_lag arrives in phase 2
  REQUIRE(simsys::report::all_pass(results) == false);
}

TEST_CASE("the text table names every node and op and marks percentiles as estimated", "[report]") {
  const auto s = mm1(500);
  const auto g = *simsys::graph::Graph::build(s);
  const auto text = simsys::report::render_table(summarize(s, g), {}, /*color=*/false);
  REQUIRE(text.find("s ") != std::string::npos);
  REQUIRE(text.find("p99") != std::string::npos);
  REQUIRE(text.find("estimated") != std::string::npos);
  REQUIRE(text.find("\x1b[") == std::string::npos);
}

TEST_CASE("the JSON report is parseable-looking and carries the scenario name", "[report]") {
  const auto s = mm1(500);
  const auto g = *simsys::graph::Graph::build(s);
  const auto json = simsys::report::render_json(summarize(s, g), {});
  REQUIRE(json.front() == '{');
  REQUIRE(json.back() == '}');
  REQUIRE(json.find("\"scenario\": \"mm1\"") != std::string::npos);
  REQUIRE(json.find("\"nodes\"") != std::string::npos);
}

TEST_CASE("the series writer emits a header and one row per node per step", "[report]") {
  const auto s = mm1(500);
  const auto g = *simsys::graph::Graph::build(s);
  std::ostringstream out;
  simsys::report::SeriesWriter writer{out, g};
  auto sim = *simsys::engine::Simulation::create(g);
  sim.run({.duration = 300ms, .step = 100ms}, [&](const auto& r) { writer.observe(r); });
  const auto text = out.str();
  REQUIRE(text.starts_with("t_s,node,rho,backlog,drops_per_s,latency_ms,zone\n"));
  REQUIRE(std::ranges::count(text, '\n') == 1 + 3 * 2);
  REQUIRE(text.find("0.100,s,") != std::string::npos);
}
```

- [ ] **Step 2: Register, run, expect failure**

- [ ] **Step 3: Implement**

Interface (`report.cppm`, no `<format>`/`<print>`/`<chrono>` in its global fragment — it imports other modules):

```cpp
export namespace simsys::report {
struct NodeSummary { std::string id; double mean_rho, peak_rho, peak_backlog, total_dropped, red_time_s; };
struct OpSummary { std::string client, op; double offered_per_s, mean_s, p50_s, p95_s, p99_s; };
enum class SloStatus { pass, fail, skipped };
struct SloResult { model::SloSpec slo; std::optional<double> observed; SloStatus status; };

// Running aggregates over StepRecords after warmup. Pattern: Observer (see engine).
class Summary {
 public:
  Summary(const graph::Graph&, SimTime warmup);
  void observe(const engine::StepRecord&);
  [[nodiscard]] std::vector<NodeSummary> nodes() const;
  [[nodiscard]] std::vector<OpSummary> ops() const;
  [[nodiscard]] std::size_t observed_steps() const;
  [[nodiscard]] double drop_rate() const;   // total dropped / total offered, over observed steps
  [[nodiscard]] std::string_view scenario_name() const;
};
[[nodiscard]] std::vector<SloResult> evaluate_slos(const Summary&, std::span<const model::SloSpec>);
[[nodiscard]] bool all_pass(std::span<const SloResult>);   // skipped counts as pass
[[nodiscard]] std::string render_table(const Summary&, std::span<const SloResult>, bool color);
[[nodiscard]] std::string render_json(const Summary&, std::span<const SloResult>);
class SeriesWriter { public: SeriesWriter(std::ostream&, const graph::Graph&); void observe(const engine::StepRecord&); };
}
```

Op aggregation: offered-rate-weighted mixture across steps — `mean = Σ w·m / Σ w`, `E[X²] = Σ w·(v + m²) / Σ w`, `variance = E[X²] − mean²` (law of total variance; comment it). Percentiles from `lognormal_quantile` with `z_p50/95/99`. SLO visiting: `std::visit` with an overload set (`template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };` — C++17 trick, note why it is still the idiom; C++20 removed the need for the deduction guide, say so). `queue_lag`/`staleness` → skipped. Table: `std::format` with width specs, ANSI colours only when `color`. JSON: hand-formatted with `std::format` (no library; numbers via `{:.6g}`), two-space indent. CSV: `{:.3f}` for `t_s`.

- [ ] **Step 4: Run `[report]`, 7 pass. Step 5: Commit** `report: summaries, SLO evaluation, table/JSON/CSV renderers`

---

### Task 14: CLI `explain`, `estimate`, `run`; chain tutorial; scenario tests

**Files:**
- Modify: `src/cli/main.cpp`
- Create: `scenarios/tutorial/chain/scenario.yaml`, `scenarios/tutorial/chain/README.md`
- Modify: `tests/scenario/test_scenarios.cpp`, `src/report/report.cppm`/`.cpp` (add `render_explain`)

- [ ] **Step 1: Write the failing tests** (append to `test_scenarios.cpp`; it already iterates every `scenario.yaml`)

```cpp
TEST_CASE("every shipped scenario resolves into a graph (explain succeeds)", "[scenario]") {
  for (const auto& file : scenario_files()) {
    CAPTURE(file);
    const auto s = simsys::config::load(file);
    REQUIRE(s.has_value());
    const auto g = simsys::graph::Graph::build(*s);
    REQUIRE(g.has_value());
    REQUIRE_FALSE(simsys::report::render_explain(*g).empty());
  }
}

TEST_CASE("tutorial scenarios run in under five seconds and pass their SLOs", "[scenario]") {
  for (const auto& file : scenario_files()) {
    if (file.find("/tutorial/") == std::string::npos) continue;
    CAPTURE(file);
    const auto s = *simsys::config::load(file);
    const auto g = *simsys::graph::Graph::build(s);
    auto sim = *simsys::engine::Simulation::create(g);
    simsys::report::Summary summary{g, s.warmup};
    const auto started = std::chrono::steady_clock::now();
    sim.run({.duration = s.duration, .step = s.step}, [&](const auto& r) { summary.observe(r); });
    REQUIRE(std::chrono::steady_clock::now() - started < 5s);
    REQUIRE(simsys::report::all_pass(simsys::report::evaluate_slos(summary, s.slos)));
  }
}
```

Refactor the existing test to share a `scenario_files()` helper (sorted paths).

`scenarios/tutorial/chain/scenario.yaml`:

```yaml
# Tutorial 2: a request that fans out. Clients call an API tier, which calls a database
# for every read and twice for every write (sequential steps: the latencies add).
version: 1
name: chain
duration: 2m
warmup: 10s
nodes:
  users: {type: client, rate: {base: 1.5k/s}, ops: {read: 0.8, write: 0.2}, timeout: 1s}
  api:
    type: service
    replicas: 2
    capacity: 1k/s
    latency: 0.5ms
    ops:
      read:  [{to: db, op: select}]
      write: [{to: db, op: select}, {to: db, op: insert}]
  db: {type: service, capacity: 5k/s, latency: 2ms, advanced: {queue_limit: 500}}
links:
  - {from: users, to: api, dir: inout}
  - {from: api, to: db, dir: inout}
slo:
  - {metric: p99_latency, node: users, op: read, max: 20ms}
  - {metric: p99_latency, node: users, op: write, max: 40ms}
  - {metric: drop_rate, max: 0}
```

README sections as in the other scenario folders: *Design*, *What the model captures* (rho of api = 1500/2000 = 0.75 → yellow; db sees 1.5k×(0.8×1 + 0.2×2) = 1.8k/s), *What it abstracts away*, *Things to try* (replicas 1 → red; db latency 10ms → write SLO fails).

- [ ] **Step 2: Run, expect failure (`render_explain` missing; chain missing)**

- [ ] **Step 3: Implement**

`render_explain(const Graph&)` in the report module prints, in this shape:

```
scenario chain  version 1  duration 2m  step 100ms  warmup 10s  seed 1
datacenters: main
nodes:
  users  client   dc=main  offered=1.5k/s  ops: read 80%, write 20%  (rate curve: base only in phase 1)
  api    service  dc=main  replicas=2  capacity=1k/s (scenario)  service_time=0.5ms (scenario)  c=1  queue_limit=1000 (default)  variability=1  burstiness=1
  db     service  dc=main  replicas=1  capacity=5k/s (scenario)  service_time=2ms (scenario)  c=1  queue_limit=500 (scenario)  variability=1  burstiness=1
links:
  users -> api   inout  one_way=250us (intra_dc_rtt/2)  delay_steps=0
  api -> db      inout  one_way=250us (intra_dc_rtt/2)  delay_steps=0
order: users, api, db
feedback edges: none
checks:
  api  offered 1.5k/s vs 2k/s total capacity  (75%, yellow)
  db   offered 1.8k/s vs 5k/s total capacity  (36%, green)
```

The `checks` block is a static pass: offered load per node at base rates, propagated through steps and factors in topological order (no queueing) — it is what a student would do by hand on paper, so it prints the same numbers. Format durations/rates with the human units the scenario used (`ms`/`us`, `k/s`), via two small helpers `format_duration`/`format_rate` in `report.cpp`.

`main.cpp`: subcommands `explain <file>`, `estimate <file> [--report table|json] [--no-color]`, `run <file> [--duration D] [--report table|json] [--series out.csv] [--no-color]`. `run` exit code: 0 when `all_pass`, 2 when an SLO fails, 1 on any error. Keep each subcommand a short function (`run_explain`, `run_estimate`, `run_run`) sharing `load_graph(file) -> expected<Graph, string>`.

- [ ] **Step 4: Run all tests; then by hand:**

```bash
./build/gcc14-debug/simsys explain scenarios/tutorial/chain/scenario.yaml
./build/gcc14-debug/simsys estimate scenarios/tutorial/chain/scenario.yaml
./build/gcc14-debug/simsys run scenarios/tutorial/chain/scenario.yaml --series /tmp/chain.csv; echo exit=$?
./build/gcc14-debug/simsys run scenarios/book/url-shortener/scenario.yaml; echo exit=$?   # expect the phase-2 message, exit 1
```

- [ ] **Step 5: Commit** `cli: explain, estimate and run; chain tutorial scenario`

---

### Task 15: Documentation, schema and spec updates, study notes

**Files:**
- Modify: `docs/scenario-schema.md` (add `advanced.burstiness`; note which keys Phase 1 reads; `peak_at`/`at` values like `21:00` must be quoted or they parse as sexagesimal integers in some YAML readers — check what yaml-cpp does and document it), `docs/superpowers/specs/2026-10-27-system-sim-design.md` (append a "Phase 1 refinements" subsection listing the eight items from this plan's header), `README.md` (usage of the four commands with the chain tutorial output)
- Create: `docs/study/concepts.md`, `docs/study/ranges.md`, `docs/study/variant-visit.md`, `docs/study/format-print.md` — each ~80–120 lines in the style of `docs/study/modules.md`: what the feature is, the pre-C++20 way, where this repo uses it (file:function), a pitfall seen while building, further reading.

- [ ] **Step 1: Write the docs. Step 2: Run clang-format-18 on `src tests`. Step 3: Full test run. Step 4: Commit** `docs: phase 1 schema/spec/README updates and four study notes`

---

## Self-review against the spec

- §3.1 time stepping, topological order, cycles → Tasks 9, 11, 12. §3.2 flows → Task 3 (rates only; refinement 1/2). §3.3 node step → Task 4. §3.4 links and delay lines → Tasks 9, 11, 12. §3.5 percentiles → Tasks 2, 10, 13. §3.6 estimate/run/determinism → Tasks 11, 12.
- §4 core types: `SimTime`/`Rate`/`ByteRate` exist; `Flow`, `Capacity`, `Workload`, `Node`, `Link` → Tasks 3–5, 9. `DataCenter` latency matrix is † in the schema; one cross-DC default only (phase 2+).
- §5 concepts: `NodeBehaviour`, `RequestSource`, `ResponseLookup`, `StepObserver` → Tasks 10, 12. `HitRatioModel`/`SplitPolicy`/`AdmissionModel`/`ReplicationModel` belong to phase 2/3 node types.
- §6 defaults with provenance and `explain` → Tasks 6, 14. §9.1 analytic layer → Tasks 1, 2, 4. §11 CLI → Task 14 (`sweep` and `--replications` are phase 2/3; `--seed` is accepted and unused until phase 2 — add it to `run` so scripts written now keep working). §12 testing: theory, conservation, scenario (<5 s, SLOs), determinism → Tasks 12, 14. ASan/UBSan CI job is deferred to phase 5 with the rest of CI hardening.
- Type consistency: `Graph::build(model::Scenario)`, `Simulation::create(const Graph&)`, `Summary{graph, warmup}`, `RunOptions{duration, step}`, `StepRecord{t, nodes, responses}`, `ResponseSample{client, op, offered, response}`, `ClientRoute{op, target, link, share}`, `Outgoing{target, link, flow}` are used with the same names in Tasks 9–14.
