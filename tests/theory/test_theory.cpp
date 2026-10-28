// Whole-simulation checks against closed-form queueing theory (spec §12).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cstddef>
#include <map>
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

// A note for test authors: `Approx(x).margin(m)` passes when the value is within the
// absolute margin m OR within Approx's default relative epsilon (about 1.19e-5), so a
// margin can only loosen a check, never tighten it. For a tight check use WithinAbs /
// WithinRel, which apply only the tolerance you give them.
using namespace std::chrono_literals;
using Catch::Approx;
using simsys::Rate;
using simsys::engine::Simulation;
using simsys::model::Direction;
using simsys::model::LinkSpec;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
using simsys::model::Workload;

namespace {
// One client, one M/M/1 server: 1000/s capacity, 1 ms service, zero-latency link.
Scenario mm1(double demand) {
  Scenario s{.name = "mm1", .version = 1, .duration = 10s};
  s.nodes = {NodeSpec{.id = "c", .type = NodeType::client,
                      .workload = Workload{.base = Rate{demand}, .op_shares = {{"op", 1.0}}}},
             NodeSpec{.id = "s", .type = NodeType::service, .capacity = Rate{1000}, .latency = 1ms}};
  s.links = {LinkSpec{.from = "c", .to = "s", .dir = Direction::inout, .latency = 0s}};
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
  REQUIRE(node.latency.mean_s == Approx(0.002));
  REQUIRE(r.responses.size() == 1);
  REQUIRE(r.responses[0].client == *g.index_of("c"));
  REQUIRE(r.responses[0].op == "op");
  REQUIRE(r.responses[0].offered == Rate{500});
  REQUIRE(r.responses[0].response.mean_s == Approx(0.002));  // zero-latency link
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

TEST_CASE("estimate reports the long-run state: empty queue below capacity, full above", "[theory]") {
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
  s.nodes = {NodeSpec{.id = "c", .type = NodeType::client,
                      .workload = Workload{.base = Rate{3000}, .op_shares = {{"op", 1.0}}}},
             NodeSpec{.id = "a", .type = NodeType::service, .capacity = Rate{1000}, .latency = 1ms,
                      .advanced = {.queue_limit = 1e-9},
                      .ops = {{"op", {simsys::model::Step{.to = "b", .op = "op"}}}}},
             NodeSpec{.id = "b", .type = NodeType::service, .capacity = Rate{10000}, .latency = 100us}};
  s.links = {LinkSpec{.from = "c", .to = "a", .latency = 0s}, LinkSpec{.from = "a", .to = "b", .latency = 0s}};
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
  REQUIRE(sim.error().message == "node 's' has type 'cache', which run/estimate do not support yet (phase 2)");
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
  s.nodes = {NodeSpec{.id = "c", .type = NodeType::client, .workload = Workload{.base = Rate{3000}, .op_shares = {{"op", 1.0}}}},
             NodeSpec{.id = "a", .type = NodeType::service, .capacity = Rate{10000}, .latency = 100us,
                      .ops = {{"op", {simsys::model::Step{.to = "b", .op = "op"}}}}},
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
  s.nodes = {NodeSpec{.id = "c", .type = NodeType::client, .workload = Workload{.base = Rate{1500}, .op_shares = {{"op", 1.0}}}},
             NodeSpec{.id = "a", .type = NodeType::service, .capacity = Rate{1000}, .latency = 1ms,
                      .ops = {{"op", {simsys::model::Step{.to = "b", .op = "op"}}}}},
             NodeSpec{.id = "b", .type = NodeType::service, .capacity = Rate{10000}, .latency = 100us}};
  s.links = {LinkSpec{.from = "c", .to = "a", .latency = 0s}, LinkSpec{.from = "a", .to = "b", .latency = 0s}};
  const auto g = *simsys::graph::Graph::build(s);
  auto sim = *Simulation::create(g);
  for (int i = 0; i < 10; ++i) (void)sim.step(i * 100ms, 100ms);
  for (int i = 10; i < 20; ++i) {
    const auto r = sim.step(i * 100ms, 100ms);
    REQUIRE(r.nodes[1].drain_per_s > 0.0);
    REQUIRE_THAT(r.nodes[2].demand_per_s, Catch::Matchers::WithinRel(r.nodes[1].drain_per_s, 1e-9));
  }
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
  REQUIRE(r.nodes[1].latency.mean_s == Approx(0.002));  // each copy is the M/M/1 at rho 0.5
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
  // u -> chat -> q -> sync -> chat (push, terminal). Flow round the loop appears at chat one step late.
  Scenario s{.name = "loop", .version = 1, .duration = 1s};
  s.nodes = {NodeSpec{.id = "u", .type = NodeType::client, .workload = Workload{.base = Rate{100}, .op_shares = {{"msg", 1.0}}}},
             NodeSpec{.id = "chat", .type = NodeType::service, .capacity = Rate{10000}, .latency = 100us,
                      .ops = {{"msg", {simsys::model::Step{.to = "q", .op = "publish"}}}}},
             NodeSpec{.id = "q", .type = NodeType::service, .capacity = Rate{10000}, .latency = 100us,
                      .ops = {{"publish", {simsys::model::Step{.to = "sync", .op = "job"}}}}},
             NodeSpec{.id = "sync", .type = NodeType::service, .capacity = Rate{10000}, .latency = 100us,
                      .ops = {{"job", {simsys::model::Step{.to = "chat", .op = "push"}}}}}};
  s.links = {LinkSpec{.from = "u", .to = "chat", .latency = 0s}, LinkSpec{.from = "chat", .to = "q", .latency = 0s},
             LinkSpec{.from = "q", .to = "sync", .latency = 0s}, LinkSpec{.from = "sync", .to = "chat", .latency = 0s}};
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
