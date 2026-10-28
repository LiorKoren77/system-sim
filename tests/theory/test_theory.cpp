// Whole-simulation checks against closed-form queueing theory (spec §12).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
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
             NodeSpec{.id = "b", .type = NodeType::service, .capacity = Rate{10000}, .latency = 0.1ms}};
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
