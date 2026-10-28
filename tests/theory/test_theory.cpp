// Whole-simulation checks against closed-form queueing theory (spec §12).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cstddef>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.core;
import simsys.engine;
import simsys.fluid;
import simsys.graph;
import simsys.model;
import simsys.workload;

// A note for test authors: `Approx(x).margin(m)` passes when the value is within the
// absolute margin m OR within Approx's default relative epsilon (about 1.19e-5), so a
// margin can only loosen a check, never tighten it. For a tight check use WithinAbs /
// WithinRel, which apply only the tolerance you give them.
using namespace std::chrono_literals;
using Catch::Approx;
using Catch::Matchers::WithinRel;
using simsys::Rate;
using simsys::engine::Simulation;
using simsys::graph::Graph;
using simsys::model::Direction;
using simsys::model::LinkSpec;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
using simsys::model::Step;
using simsys::model::Workload;

namespace {
// One client, one M/M/1 server: 1000/s capacity, 1 ms service, zero-latency link.
Scenario mm1(double demand) {
  // C++20 designated initializers: `.name = ...` names each member being set, in
  // declaration order, and leaves the rest at their defaults. Replaces positional braces
  // (which break silently when members are reordered) or a default object assigned field
  // by field; here a scenario reads like the YAML it stands for.
  Scenario s{.name = "mm1", .version = 1, .duration = 10s};
  s.nodes = {
      NodeSpec{.id = "c",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{demand}}, .op_shares = {{"op", 1.0}}}},
      NodeSpec{.id = "s", .type = NodeType::service, .capacity = Rate{1000}, .latency = 1ms}};
  s.links = {LinkSpec{.from = "c", .to = "s", .dir = Direction::inout, .latency = 0s}};
  return s;
}

// c(100/s) -> f -> g, and g sends half of what it serves back to f (factor 0.5) over a
// feedback link of the given latency: a loop of gain 0.5.
Scenario half_loop(simsys::SimTime feedback_latency) {
  Scenario s{.name = "half-loop", .version = 1, .duration = 2s};
  s.nodes = {
      NodeSpec{.id = "c",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{100}}, .op_shares = {{"x", 1.0}}}},
      NodeSpec{.id = "f",
               .type = NodeType::service,
               .capacity = Rate{1e6},
               .latency = 100us,
               .ops = {{"x", {Step{.to = "g", .op = "x"}}}}},
      NodeSpec{.id = "g",
               .type = NodeType::service,
               .capacity = Rate{1e6},
               .latency = 100us,
               .ops = {{"x", {Step{.to = "f", .op = "x", .factor = 0.5}}}}}};
  s.links = {LinkSpec{.from = "c", .to = "f", .latency = 0s},
             LinkSpec{.from = "f", .to = "g", .latency = 0s},
             LinkSpec{.from = "g", .to = "f", .latency = feedback_latency}};
  return s;
}
}  // namespace

TEST_CASE("one step of an M/M/1 node reproduces rho/(mu - lambda) + service time", "[theory]") {
  const auto g = *simsys::graph::Graph::build(mm1(500));
  auto sim = *Simulation::create(g);
  const auto r = sim.step(0s, 100ms);
  const auto& node = r.nodes[*g.index_of("s")];
  REQUIRE(node.demand_per_s == Approx(500));
  REQUIRE(node.rho == Approx(0.5));
  REQUIRE_THAT(node.latency.mean_s, WithinRel(0.002, 1e-12));
  REQUIRE(r.responses.size() == 1);
  REQUIRE(r.responses[0].client == *g.index_of("c"));
  REQUIRE(r.responses[0].op == "op");
  REQUIRE(r.responses[0].offered == Rate{500});
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(0.002, 1e-12));  // zero-latency link
}

// Little's law, L = lambda * W: the mean number of requests in a stable system equals the
// arrival rate times the mean time each spends in it, whatever the distributions. For an
// M/M/1 queue (Poisson arrivals, exponential service, one server) the mean number in
// system is L = rho / (1 - rho), which is 1 at rho = 0.5. The engine's sojourn time W
// (latency.mean_s, wait plus service) times the demand lambda must therefore give 1. The
// fluid backlog is not L: below capacity it is 0, the stochastic queue lives in W alone.
TEST_CASE("Little's law holds for the stochastic term: demand x mean latency = rho / (1 - rho)",
          "[theory]") {
  const auto g = *simsys::graph::Graph::build(mm1(500));  // rho = 500 / 1000 = 0.5
  const auto r = (*Simulation::create(g)).step(0s, 100ms);
  const auto& node = r.nodes[*g.index_of("s")];
  const double rho = 0.5;
  REQUIRE_THAT(node.demand_per_s * node.latency.mean_s, WithinRel(rho / (1.0 - rho), 1e-9));
}

TEST_CASE("a client's node sample is a source: no demand, no queue, green", "[theory]") {
  const auto g = *simsys::graph::Graph::build(mm1(500));
  const auto r = (*Simulation::create(g)).step(0s, 100ms);
  const auto& client = r.nodes[*g.index_of("c")];
  REQUIRE(client.demand_per_s == 0.0);
  REQUIRE(client.backlog == 0.0);
  REQUIRE(client.rho == 0.0);
  REQUIRE(client.zone == simsys::fluid::Zone::green);
}

TEST_CASE("estimate reports the long-run state: empty queue below capacity, full above",
          "[theory]") {
  const auto under = *simsys::graph::Graph::build(mm1(500));
  REQUIRE((*Simulation::create(under)).estimate().nodes[1].backlog == 0.0);
  const auto over = *simsys::graph::Graph::build(mm1(1500));
  const auto e = (*Simulation::create(over)).estimate();
  REQUIRE(e.nodes[1].backlog == Approx(1000));
  REQUIRE(e.nodes[1].dropped_per_s == Approx(500));
  REQUIRE(e.nodes[1].rho == 1.0);
  REQUIRE(e.t == 0s);
}

TEST_CASE("dropped flow does not propagate downstream", "[theory]") {
  // c -> a (overloaded, queue 0) -> b : b must see only what a drained.
  Scenario s{.name = "drop", .version = 1, .duration = 1s};
  s.nodes = {
      NodeSpec{.id = "c",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{3000}}, .op_shares = {{"op", 1.0}}}},
      NodeSpec{.id = "a",
               .type = NodeType::service,
               .capacity = Rate{1000},
               .latency = 1ms,
               .advanced = {.queue_limit = 1e-9},
               .ops = {{"op", {simsys::model::Step{.to = "b", .op = "op"}}}}},
      NodeSpec{.id = "b", .type = NodeType::service, .capacity = Rate{10000}, .latency = 100us}};
  s.links = {LinkSpec{.from = "c", .to = "a", .latency = 0s},
             LinkSpec{.from = "a", .to = "b", .latency = 0s}};
  const auto g = *simsys::graph::Graph::build(s);
  const auto r = (*Simulation::create(g)).step(0s, 100ms);
  REQUIRE(r.nodes[1].dropped_per_s == Approx(2000).epsilon(1e-6));
  REQUIRE(r.nodes[2].demand_per_s == Approx(1000).epsilon(1e-6));
}

TEST_CASE("a simulation refuses node types the phase does not support yet", "[theory]") {
  Scenario s = mm1(1);
  s.nodes[1].type = NodeType::cache;
  const auto g = *simsys::graph::Graph::build(s);
  const auto sim = Simulation::create(g);
  REQUIRE_FALSE(sim.has_value());
  REQUIRE(sim.error().message ==
          "node 's' has type 'cache', which run/estimate do not support yet (phase 2)");
}

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
  s.nodes = {
      NodeSpec{.id = "c",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{3000}}, .op_shares = {{"op", 1.0}}}},
      NodeSpec{.id = "a",
               .type = NodeType::service,
               .capacity = Rate{10000},
               .latency = 100us,
               .ops = {{"op", {simsys::model::Step{.to = "b", .op = "op"}}}}},
      NodeSpec{.id = "b",
               .type = NodeType::service,
               .capacity = Rate{1000},
               .latency = 1ms,
               .advanced = {.queue_limit = 200}}};
  s.links = {LinkSpec{.from = "c", .to = "a", .latency = 0s},
             LinkSpec{.from = "a", .to = "b", .latency = 0s}};
  const auto g = *simsys::graph::Graph::build(s);
  auto sim = *Simulation::create(g);
  std::vector<double> previous_backlog(3, 0.0);
  sim.run({.duration = 5s, .step = 100ms}, [&](const simsys::engine::StepRecord& r) {
    for (std::size_t i = 1; i < 3; ++i) {
      const auto& n = r.nodes[i];
      const double in = n.demand_per_s * 0.1;
      const double out = (n.drain_per_s + n.dropped_per_s) * 0.1;
      REQUIRE_THAT(in, Catch::Matchers::WithinAbs(out + (n.backlog - previous_backlog[i]), 1e-9));
      previous_backlog[i] = n.backlog;
    }
  });
}

TEST_CASE("what a node drains is what the next node receives", "[theory]") {
  // c -> a (overloaded) -> b. The client never stops: after 10 steps a holds a steady backlog
  // of 500 and drains at its capacity, 1000/s. Whatever a drains, b must receive in the same
  // step: queued requests do not vanish and are not invented.
  Scenario s{.name = "drain", .version = 1, .duration = 2s};
  s.nodes = {
      NodeSpec{.id = "c",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{1500}}, .op_shares = {{"op", 1.0}}}},
      NodeSpec{.id = "a",
               .type = NodeType::service,
               .capacity = Rate{1000},
               .latency = 1ms,
               .ops = {{"op", {simsys::model::Step{.to = "b", .op = "op"}}}}},
      NodeSpec{.id = "b", .type = NodeType::service, .capacity = Rate{10000}, .latency = 100us}};
  s.links = {LinkSpec{.from = "c", .to = "a", .latency = 0s},
             LinkSpec{.from = "a", .to = "b", .latency = 0s}};
  const auto g = *simsys::graph::Graph::build(s);
  auto sim = *Simulation::create(g);
  for (int i = 0; i < 10; ++i) (void)sim.step(i * 100ms, 100ms);
  for (int i = 10; i < 20; ++i) {
    const auto r = sim.step(i * 100ms, 100ms);
    REQUIRE(r.nodes[1].drain_per_s > 0.0);
    REQUIRE_THAT(r.nodes[2].demand_per_s, Catch::Matchers::WithinRel(r.nodes[1].drain_per_s, 1e-9));
  }
}

TEST_CASE("the drained flow keeps the queue's op mix", "[theory]") {
  // c sends x and y half and half at 3000/s into a (1000/s, queue 200): a is saturated,
  // its queue holds both ops half and half, and only x goes on to b (y ends at a). After
  // 10 steps b must see exactly the x half of what a drains, backlog included.
  Scenario s{.name = "mix", .version = 1, .duration = 2s};
  s.nodes = {
      NodeSpec{.id = "c",
               .type = NodeType::client,
               .workload =
                   Workload{.curve = {.base = Rate{3000}}, .op_shares = {{"x", 0.5}, {"y", 0.5}}}},
      NodeSpec{.id = "a",
               .type = NodeType::service,
               .capacity = Rate{1000},
               .latency = 1ms,
               .advanced = {.queue_limit = 200},
               .ops = {{"x", {simsys::model::Step{.to = "b", .op = "x"}}}}},
      NodeSpec{.id = "b", .type = NodeType::service, .capacity = Rate{10000}, .latency = 100us}};
  s.links = {LinkSpec{.from = "c", .to = "a", .latency = 0s},
             LinkSpec{.from = "a", .to = "b", .latency = 0s}};
  const auto g = *simsys::graph::Graph::build(s);
  auto sim = *Simulation::create(g);
  for (int i = 0; i < 9; ++i) (void)sim.step(i * 100ms, 100ms);
  const auto r = sim.step(900ms, 100ms);
  REQUIRE(r.nodes[1].drain_per_s > 0.0);
  REQUIRE_THAT(r.nodes[2].demand_per_s,
               Catch::Matchers::WithinRel(0.5 * r.nodes[1].drain_per_s, 1e-9));
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
  // Each copy is the M/M/1 at rho 0.5: 1 ms service plus rho / (mu - lambda) = 1 ms wait.
  REQUIRE_THAT(r.nodes[1].latency.mean_s, WithinRel(0.002, 1e-12));
}

TEST_CASE("a cross-datacenter link delays demand by its delay steps", "[theory]") {
  Scenario s = mm1(500);
  s.links[0].latency = 150ms;  // round(1.5) = 2 steps at 100 ms
  auto sim = *Simulation::create(*simsys::graph::Graph::build(s));
  REQUIRE(sim.step(0s, 100ms).nodes[1].demand_per_s == 0.0);
  REQUIRE(sim.step(100ms, 100ms).nodes[1].demand_per_s == 0.0);
  REQUIRE(sim.step(200ms, 100ms).nodes[1].demand_per_s == Approx(500));
}

TEST_CASE("a feedback loop re-injects flow one step later and settles under estimate", "[theory]") {
  // u -> chat -> q -> sync -> chat (push, terminal). Flow round the loop appears at chat one step
  // late.
  Scenario s{.name = "loop", .version = 1, .duration = 1s};
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{100}}, .op_shares = {{"msg", 1.0}}}},
      NodeSpec{.id = "chat",
               .type = NodeType::service,
               .capacity = Rate{10000},
               .latency = 100us,
               .ops = {{"msg", {simsys::model::Step{.to = "q", .op = "publish"}}}}},
      NodeSpec{.id = "q",
               .type = NodeType::service,
               .capacity = Rate{10000},
               .latency = 100us,
               .ops = {{"publish", {simsys::model::Step{.to = "sync", .op = "job"}}}}},
      NodeSpec{.id = "sync",
               .type = NodeType::service,
               .capacity = Rate{10000},
               .latency = 100us,
               .ops = {{"job", {simsys::model::Step{.to = "chat", .op = "push"}}}}}};
  s.links = {LinkSpec{.from = "u", .to = "chat", .latency = 0s},
             LinkSpec{.from = "chat", .to = "q", .latency = 0s},
             LinkSpec{.from = "q", .to = "sync", .latency = 0s},
             LinkSpec{.from = "sync", .to = "chat", .latency = 0s}};
  const auto g = *simsys::graph::Graph::build(s);
  auto sim = *Simulation::create(g);
  const auto first = sim.step(0s, 100ms);
  REQUIRE(first.nodes[1].demand_per_s == Approx(100));  // only the client's msg this step
  const auto second = sim.step(100ms, 100ms);
  REQUIRE(second.nodes[1].demand_per_s == Approx(200));  // plus last step's push
  const auto e = (*Simulation::create(g)).estimate();
  REQUIRE(e.nodes[1].demand_per_s == Approx(200));
}

TEST_CASE("two runs of the same scenario produce identical records", "[theory]") {
  const auto g = *simsys::graph::Graph::build(mm1(900));
  auto collect = [&] {
    std::vector<simsys::engine::StepRecord> out;
    (*Simulation::create(g)).run({.duration = 1s, .step = 100ms}, [&](const auto& r) {
      out.push_back(r);
    });
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

TEST_CASE(
    "estimate finds the fixed point base/(1-g) of a loop that feeds back half its flow, whatever "
    "ran before",
    "[theory]") {
  // f's demand d satisfies d = 100 + 0.5 d, so d = 100 / (1 - 0.5) = 200 (a geometric series:
  // each lap returns half of the previous one).
  const auto g = *Graph::build(half_loop(0s));
  auto sim = *Simulation::create(g);
  REQUIRE_THAT(sim.estimate().nodes[1].demand_per_s, WithinRel(200.0, 1e-9));
  for (int i = 0; i < 5; ++i) (void)sim.step(i * 100ms, 100ms);
  REQUIRE_THAT(sim.estimate().nodes[1].demand_per_s, WithinRel(200.0, 1e-9));
}

TEST_CASE("a three-step feedback link re-injects flow exactly three steps later", "[theory]") {
  // f(k) = 100 + 0.5 * g.drain(k - 3): the client's 100/s, plus half of what went round three
  // steps ago.
  auto sim = *Simulation::create(*Graph::build(half_loop(300ms)));
  const double expected[] = {100, 100, 100, 150, 150, 150, 175};
  for (int k = 0; k < 7; ++k) {
    REQUIRE_THAT(sim.step(k * 100ms, 100ms).nodes[1].demand_per_s, WithinRel(expected[k], 1e-12));
  }
}

TEST_CASE("a client's first response already includes the hops its flow has not reached yet",
          "[theory]") {
  // c -(150 ms)-> s: 2 delay steps. At step 0 nothing has reached s, but the response the
  // client is promised already has both 150 ms legs and s's empty-queue response, its 1 ms
  // service time. Once the flow arrives (step 2) s runs at rho 0.5 and the response is the
  // steady one: 0.300 + 0.002 (M/M/1: rho / (mu - lambda) + 1 / mu).
  Scenario s = mm1(500);
  s.links[0].latency = 150ms;
  const auto g = *Graph::build(s);
  auto sim = *Simulation::create(g);
  const double steady = sim.estimate().responses[0].response.mean_s;
  REQUIRE_THAT(steady, WithinRel(0.302, 1e-9));
  REQUIRE_THAT(sim.step(0s, 100ms).responses[0].response.mean_s, WithinRel(0.301, 1e-9));
  REQUIRE_THAT(sim.step(100ms, 100ms).responses[0].response.mean_s, WithinRel(0.301, 1e-9));
  REQUIRE_THAT(sim.step(200ms, 100ms).responses[0].response.mean_s, WithinRel(steady, 1e-9));
}

TEST_CASE(
    "run and step refuse a step that differs from the scenario's, since delay lines are "
    "sized for it",
    "[theory]") {
  auto sim = *Simulation::create(*Graph::build(mm1(500)));  // scenario step: 100 ms
  REQUIRE_THROWS_AS(sim.run({.duration = 1s, .step = 10ms}, [](const auto&) {}),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(sim.step(0s, 10ms), std::invalid_argument);
}

TEST_CASE("a loop of gain one settles at saturation: the entry node sheds what the client adds",
          "[theory]") {
  // g returns everything it serves, so f's demand grows by the client's 100/s per lap until
  // f saturates at 1e6/s. That is a steady state of the fluid model: f drains 1e6/s, g
  // serves exactly its capacity and returns all of it, and f drops the client's 100/s.
  Scenario s = half_loop(0s);
  s.nodes[2].ops["x"][0].factor = 1.0;
  const auto e = (*Simulation::create(*Graph::build(s))).estimate();
  REQUIRE(e.converged);
  REQUIRE_THAT(e.nodes[1].demand_per_s, WithinRel(1e6 + 100, 1e-12));
  REQUIRE_THAT(e.nodes[1].drain_per_s, WithinRel(1e6, 1e-12));
  REQUIRE_THAT(e.nodes[1].dropped_per_s, WithinRel(100.0, 1e-9));
  REQUIRE_THAT(e.nodes[2].demand_per_s, WithinRel(1e6, 1e-12));
}

TEST_CASE("estimate reports no steady state for a loop of gain one that nothing saturates",
          "[theory]") {
  // As above, but with capacities no lap can reach (1e12/s; concurrency 1 keeps the
  // implied server count small): the demand grows by 100/s every pass and never stops
  // changing, so estimate hits its pass cap and says so instead of returning a number.
  Scenario s = half_loop(0s);
  s.nodes[2].ops["x"][0].factor = 1.0;
  for (const std::size_t i : {1U, 2U}) {
    s.nodes[i].capacity = Rate{1e12};
    s.nodes[i].advanced.concurrency = 1;
  }
  const auto e = (*Simulation::create(*Graph::build(s))).estimate();
  REQUIRE_FALSE(e.converged);
}

TEST_CASE("run emits ceil(duration / step) records", "[theory]") {
  auto sim = *Simulation::create(*Graph::build(mm1(500)));
  int n = 0;
  sim.run({.duration = 1s, .step = 100ms}, [&](const auto&) { ++n; });
  REQUIRE(n == 10);
  Scenario s = mm1(500);
  s.step = 300ms;
  auto sim2 = *Simulation::create(*Graph::build(s));
  n = 0;
  sim2.run({.duration = 1s, .step = 300ms}, [&](const auto&) { ++n; });
  REQUIRE(n == 4);  // 0, 300, 600, 900 ms: the last step runs past the duration
}

TEST_CASE("demand follows a sine wave through the engine", "[theory]") {
  Scenario s = mm1(500);
  s.nodes[0].workload->curve.wave = simsys::workload::Wave{
      .shape = simsys::workload::WaveShape::sine, .period = 1s, .amplitude = 0.5, .peak_at = 0s};
  auto sim = *Simulation::create(*Graph::build(s));
  REQUIRE_THAT(sim.step(0s, 100ms).nodes[1].demand_per_s, WithinRel(750.0, 1e-9));
  for (int i = 1; i < 5; ++i) (void)sim.step(i * 100ms, 100ms);
  REQUIRE_THAT(sim.step(500ms, 100ms).nodes[1].demand_per_s, WithinRel(250.0, 1e-9));
}

TEST_CASE("two runs with the same seed are identical and a different seed is not", "[theory]") {
  Scenario s = mm1(500);
  s.nodes[0].workload->noise = simsys::workload::Noise{.sigma = 0.2, .correlation = 1s};
  const auto collect = [&](int seed) {
    s.seed = seed;
    std::vector<double> d;
    (*Simulation::create(*Graph::build(s))).run({.duration = 2s, .step = 100ms},
                                                [&](const auto& r) { d.push_back(r.nodes[1].demand_per_s); });
    return d;
  };
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

TEST_CASE("a points-only client offers the interpolated rate", "[theory]") {
  Scenario s = mm1(0);
  s.nodes[0].workload->curve.points = simsys::workload::Points{.at = {{0s, Rate{100}}, {1s, Rate{300}}}};
  auto sim = *Simulation::create(*Graph::build(s));
  REQUIRE_THAT(sim.step(0s, 100ms).nodes[1].demand_per_s, WithinRel(100.0, 1e-12));
  for (int i = 1; i < 5; ++i) (void)sim.step(i * 100ms, 100ms);
  REQUIRE_THAT(sim.step(500ms, 100ms).nodes[1].demand_per_s, WithinRel(200.0, 1e-12));
}

TEST_CASE("noise never produces a negative demand", "[theory]") {
  Scenario s = mm1(500);
  s.nodes[0].workload->noise = simsys::workload::Noise{.sigma = 3.0, .correlation = 100ms};
  (*Simulation::create(*Graph::build(s))).run({.duration = 10s, .step = 100ms}, [&](const auto& r) {
    REQUIRE(r.nodes[1].demand_per_s >= 0.0);
  });
}
