// Whole-simulation checks against closed-form queueing theory (spec §12).
// This file: estimate equals run at exactly rho 1. One of the nine files the theory tests are split
// into by topic; the scenario builders more than one of them needs live in
// tests/support/fixtures.cppm (module simsys.test_fixtures).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <utility>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.core;
import simsys.engine;
import simsys.graph;
import simsys.model;
import simsys.test_fixtures;

// A note for test authors: `Approx(x).margin(m)` passes when the value is within the
// absolute margin m OR within Approx's default relative epsilon (about 1.19e-5), so a
// margin can only loosen a check, never tighten it. For a tight check use WithinAbs /
// WithinRel, which apply only the tolerance you give them.
using namespace std::chrono_literals;
using Catch::Matchers::WithinRel;
using simsys::engine::Simulation;
using simsys::graph::Graph;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
// Shared builders (tests/support/fixtures.cppm).
using simsys::test_support::mm1;
using simsys::test_support::only_link;
using simsys::test_support::pipe_mb_per_s;
using simsys::test_support::pipe_over;
using simsys::test_support::records_of;

// ---- estimate = run at exactly rho 1 ----
//
// At demand = capacity the fluid neither grows nor drains: a room that starts empty stays empty,
// and the stochastic wait is the 0.99 clamp's (fluid::max_stochastic_rho). `run` steps from an
// empty room; `estimate` settles one, and settles it empty too (FluidQueue::settle is strict), so
// both report the same node. A real queue at rho 1 has no steady state (its backlog wanders, about
// half its room on average): size a system off rho 1, which both commands mark red.
namespace {
// estimate()'s record and the 50th step's (5 s) of fresh simulations of `g`.
std::pair<simsys::engine::StepRecord, simsys::engine::StepRecord> estimate_and_run(const Graph& g) {
  const auto estimated = (*Simulation::create(g)).estimate();
  const auto records = records_of(g, 50);
  return {estimated, records.back()};
}
}  // namespace

// M/M/1 at the clamp: Wq = rho / (1 − rho) × s = 0.99 / 0.01 × 1 ms = 99 ms, plus the 1 ms service.
TEST_CASE("a service offered exactly its capacity reports the same empty room in estimate and run",
          "[theory]") {
  const auto g = *Graph::build(mm1(1000));
  const auto [estimated, ran] = estimate_and_run(g);
  for (const auto& r : {estimated, ran}) {
    REQUIRE(r.nodes[1].backlog == 0.0);
    REQUIRE(r.nodes[1].rho == 1.0);
    REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(0.099 + 0.001, 1e-12));
  }
}

// 125 gets/s of 1 MB on 1 Gbit/s (125 MB/s): one deterministic server of 8 ms per MB at the clamp,
// M/D/1 (Pollaczek–Khinchine): Wq = (1 + 0) / 2 × 0.99 / 0.01 × 8 ms = 396 ms; the hop adds that
// and the first megabyte's 8 ms: 404 ms. (Settling a full room said 8.05 s.)
TEST_CASE("a pipe offered exactly its bandwidth reports the same empty room in estimate and run",
          "[theory]") {
  const auto g = *Graph::build(pipe_over(pipe_mb_per_s, simsys::Bytes{1e6}));
  const auto [estimated, ran] = estimate_and_run(g);
  for (const auto& r : {estimated, ran}) {
    REQUIRE(only_link(r).backlog == 0.0);
    REQUIRE(only_link(r).rho == 1.0);
    REQUIRE_THAT(only_link(r).latency.mean_s, WithinRel(0.396 + 0.008, 1e-9));
  }
}

// An external sink at its default 1k/s offered 1k/s (the Phase 3 backlog's probe): the room stays
// empty in both, where settling filled it (peak_backlog 1k against run's 0).
TEST_CASE("an external sink offered exactly its capacity reports the same empty room in both",
          "[theory]") {
  Scenario s = mm1(1000);
  // C++20 designated initializers: `.id = ...` names each member being set, in
  // declaration order, and leaves the rest at their defaults. Replaces positional braces
  // (which break silently when members are reordered) or a default object assigned field
  // by field; here a node reads like the YAML it stands for.
  s.nodes[1] = NodeSpec{.id = "s", .type = NodeType::external_sink};
  const auto g = *Graph::build(s);
  const auto [estimated, ran] = estimate_and_run(g);
  REQUIRE(estimated.nodes[1].backlog == 0.0);
  REQUIRE(ran.nodes[1].backlog == 0.0);
  REQUIRE(estimated.nodes[1].rho == 1.0);
  REQUIRE_THAT(estimated.nodes[1].latency.mean_s, WithinRel(ran.nodes[1].latency.mean_s, 1e-12));
  REQUIRE_THAT(estimated.responses[0].response.mean_s,
               WithinRel(ran.responses[0].response.mean_s, 1e-12));
}
