// Whole-simulation checks against closed-form queueing theory (spec §12).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.behaviours; // client_seed: the noise seed the engine gives each client
import simsys.caching;
import simsys.core;
import simsys.engine;
import simsys.fluid;
import simsys.graph;
import simsys.model;
import simsys.queueing;
import simsys.report;
import simsys.workload;

// A note for test authors: `Approx(x).margin(m)` passes when the value is within the
// absolute margin m OR within Approx's default relative epsilon (about 1.19e-5), so a
// margin can only loosen a check, never tighten it. For a tight check use WithinAbs /
// WithinRel, which apply only the tolerance you give them.
using namespace std::chrono_literals;
using Catch::Approx;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using simsys::Rate;
using simsys::engine::Simulation;
using simsys::graph::Graph;
using simsys::model::CacheStrategy;
using simsys::model::Direction;
using simsys::model::HitRatio;
using simsys::model::IdAlgorithm;
using simsys::model::LbPolicy;
using simsys::model::LimiterAlgorithm;
using simsys::model::LinkSpec;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
using simsys::model::Step;
using simsys::model::StepCondition;
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

// u(demand/s) -> lb -> a: the service `a` has 1000/s capacity, 1 ms service time and the
// given replicas; both links have zero latency; the balancer keeps its default capacity
// (100k/s) and latency (50 us), so it adds only its own tiny sojourn.
Scenario lb_front(LbPolicy policy, int replicas, double demand) {
  Scenario s{.name = "lb-front", .version = 1, .duration = 10s};
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{demand}}, .op_shares = {{"op", 1.0}}}},
      NodeSpec{.id = "lb", .type = NodeType::load_balancer, .policy = policy},
      NodeSpec{.id = "a",
               .type = NodeType::service,
               .replicas = replicas,
               .capacity = Rate{1000},
               .latency = 1ms}};
  s.links = {LinkSpec{.from = "u", .to = "lb", .latency = 0s},
             LinkSpec{.from = "lb", .to = "a", .latency = 0s}};
  return s;
}
// u(demand/s) -> limiter -> api, zero-latency links; the api (10k/s) is far from saturated, so
// whatever happens in the results comes from the limiter. A token bucket is a pure gate, so
// its own queue is given a huge capacity (nothing queues or drops in it); a leaky bucket IS
// its queue, drained at `limit`, so it keeps no `capacity` of its own.
Scenario limited(LimiterAlgorithm algorithm, double limit, int burst, double demand) {
  Scenario s{.name = "limited", .version = 1, .duration = 2s};
  NodeSpec limiter{.id = "limiter",
                   .type = NodeType::rate_limiter,
                   .algorithm = algorithm,
                   .limit = Rate{limit},
                   .burst = burst};
  if (algorithm != LimiterAlgorithm::leaky_bucket) limiter.capacity = Rate{1e6};
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{demand}}, .op_shares = {{"call", 1.0}}}},
      limiter, NodeSpec{.id = "api", .type = NodeType::service, .capacity = Rate{10'000}}};
  s.links = {LinkSpec{.from = "u", .to = "limiter", .latency = 0s},
             LinkSpec{.from = "limiter", .to = "api", .latency = 0s}};
  return s;
}

// u(1000/s of `read`) -> web -> [cache.read, db.read on miss], all links with zero latency:
// the read path of ch. 1's cache (cache-aside; the book calls it read-through). web (10k/s,
// 0.5 ms) and the cache (100k/s, 0.1 ms) are nowhere near saturation; the db (1000/s, 5 ms:
// five servers) at the full 1000/s would be. Each node has capacity enough that what the results
// show comes from the hit ratio alone.
Scenario cached_reads(double hit) {
  Scenario s{.name = "cached-reads", .version = 1, .duration = 10s};
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{1000}}, .op_shares = {{"read", 1.0}}}},
      NodeSpec{.id = "web",
               .type = NodeType::service,
               .capacity = Rate{10'000},
               .latency = 500us,
               .ops = {{"read",
                        {Step{.to = "cache", .op = "read"},
                         Step{.to = "db", .op = "read", .on = StepCondition::miss}}}}},
      NodeSpec{.id = "cache",
               .type = NodeType::cache,
               .capacity = Rate{100'000},
               .latency = 100us,
               .hit_ratio = HitRatio{.fixed = hit},
               .strategy = CacheStrategy::cache_aside},
      NodeSpec{.id = "db", .type = NodeType::service, .capacity = Rate{1000}, .latency = 5ms}};
  s.links = {LinkSpec{.from = "u", .to = "web", .latency = 0s},
             LinkSpec{.from = "web", .to = "cache", .latency = 0s},
             LinkSpec{.from = "web", .to = "db", .latency = 0s}};
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

// (The example used to be a kv_cluster; it is simulated since plan 2c Task 10, so the refusal is
// shown on a ws_gateway, which waits for phase 3.)
TEST_CASE("a simulation refuses node types the phase does not support yet", "[theory]") {
  Scenario s = mm1(1);
  s.nodes[1].type = NodeType::ws_gateway;
  const auto g = *simsys::graph::Graph::build(s);
  const auto sim = Simulation::create(g);
  REQUIRE_FALSE(sim.has_value());
  REQUIRE(sim.error().message ==
          "node 's' has type 'ws_gateway', which run/estimate do not support yet (phase 2)");
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
    (*Simulation::create(*Graph::build(s)))
        .run({.duration = 2s, .step = 100ms},
             [&](const auto& r) { d.push_back(r.nodes[1].demand_per_s); });
    return d;
  };
  REQUIRE(collect(1) == collect(1));
  REQUIRE(collect(1) != collect(2));
}

TEST_CASE("estimate ignores noise and evaluates the curve at time zero", "[theory]") {
  Scenario s = mm1(500);
  s.nodes[0].workload->noise = simsys::workload::Noise{.sigma = 0.5, .correlation = 1s};
  s.nodes[0].workload->curve.spikes = {
      simsys::workload::Spike{.at = 0s, .duration = 1s, .factor = 2}};
  const auto e = (*Simulation::create(*Graph::build(s))).estimate();
  REQUIRE_THAT(e.nodes[1].demand_per_s, WithinRel(1000.0, 1e-12));
}

TEST_CASE("a points-only client offers the interpolated rate", "[theory]") {
  Scenario s = mm1(0);
  s.nodes[0].workload->curve.points =
      simsys::workload::Points{.at = {{0s, Rate{100}}, {1s, Rate{300}}}};
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

TEST_CASE("an op with cost 3 consumes three times the capacity of a cost-1 op", "[theory]") {
  Scenario s = mm1(300);            // 300 req/s of op `op`
  s.nodes[1].cost = {{"op", 3.0}};  // = 900 units/s against 1000 units/s
  const auto r = (*Simulation::create(*Graph::build(s))).step(0s, 100ms);
  REQUIRE_THAT(r.nodes[1].rho, WithinRel(0.9, 1e-12));
  REQUIRE_THAT(r.nodes[1].demand_per_s, WithinRel(900.0, 1e-12));  // demand is in capacity units
}

TEST_CASE("costs do not change the number of requests forwarded", "[theory]") {
  // c -> a (cost 2 on op) -> b: b still receives one request per request a drained.
  Scenario s{.name = "cost", .version = 1, .duration = 1s};
  s.nodes = {
      NodeSpec{.id = "c",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{300}}, .op_shares = {{"op", 1.0}}}},
      NodeSpec{.id = "a",
               .type = NodeType::service,
               .capacity = Rate{1000},
               .latency = 1ms,
               .ops = {{"op", {Step{.to = "b", .op = "op"}}}},
               .cost = {{"op", 2.0}}},
      NodeSpec{.id = "b", .type = NodeType::service, .capacity = Rate{10000}, .latency = 100us}};
  s.links = {LinkSpec{.from = "c", .to = "a", .latency = 0s},
             LinkSpec{.from = "a", .to = "b", .latency = 0s}};
  const auto r = (*Simulation::create(*Graph::build(s))).step(0s, 100ms);
  REQUIRE_THAT(r.nodes[1].demand_per_s, WithinRel(600.0, 1e-12));  // units
  REQUIRE_THAT(r.nodes[2].demand_per_s, WithinRel(300.0, 1e-12));  // requests (b's cost is 1)
}

TEST_CASE("an overloaded node with mixed costs drains units and forwards the right total",
          "[theory]") {
  // ops x (cost 1) and y (cost 4), 50/50 by rate, into a 1000 units/s node with queue 100:
  // demand 500 + 2000 = 2500 units/s; drained 1000 units/s; the queue is well mixed in units,
  // so the forwarded request mix equals the unit mix of the content: x:y units = 1:4.
  Scenario s{.name = "mix", .version = 1, .duration = 1s};
  s.nodes = {
      NodeSpec{.id = "c",
               .type = NodeType::client,
               .workload =
                   Workload{.curve = {.base = Rate{1000}}, .op_shares = {{"x", 0.5}, {"y", 0.5}}}},
      NodeSpec{.id = "a",
               .type = NodeType::service,
               .capacity = Rate{1000},
               .latency = 1ms,
               .advanced = {.queue_limit = 100},
               .ops = {{"x", {Step{.to = "b", .op = "x"}}}, {"y", {Step{.to = "b", .op = "y"}}}},
               .cost = {{"y", 4.0}}},
      NodeSpec{.id = "b", .type = NodeType::service, .capacity = Rate{100000}, .latency = 10us}};
  s.links = {LinkSpec{.from = "c", .to = "a", .latency = 0s},
             LinkSpec{.from = "a", .to = "b", .latency = 0s}};
  auto sim = *Simulation::create(*Graph::build(s));
  simsys::engine::StepRecord r;
  for (int i = 0; i < 5; ++i) r = sim.step(i * 100ms, 100ms);
  // b receives x and y requests in the ratio of their unit shares divided by cost:
  // (200 units -> 200 req) : (800 units -> 200 req)
  REQUIRE_THAT(r.nodes[2].demand_per_s, WithinRel(400.0, 1e-9));
}

TEST_CASE("estimate after a noisy run equals estimate before it", "[theory]") {
  Scenario s = mm1(500);
  s.nodes[0].workload->noise = simsys::workload::Noise{.sigma = 0.3, .correlation = 1s};
  auto sim = *Simulation::create(*Graph::build(s));
  const auto before = sim.estimate();
  sim.run({.duration = 2s, .step = 100ms}, [](const auto&) {});
  const auto after = sim.estimate();
  REQUIRE(before.nodes[1].demand_per_s == after.nodes[1].demand_per_s);
  REQUIRE(before.responses[0].response == after.responses[0].response);
}

TEST_CASE("replicas, costs and a delay line: the next node receives the unit mix as requests",
          "[theory]") {
  Scenario s{.name = "rc", .version = 1, .duration = 10s};
  s.nodes = {
      NodeSpec{.id = "c",
               .type = NodeType::client,
               .workload =
                   Workload{.curve = {.base = Rate{1000}}, .op_shares = {{"x", 0.5}, {"y", 0.5}}}},
      NodeSpec{.id = "a",
               .type = NodeType::service,
               .replicas = 3,
               .capacity = Rate{400},
               .latency = 1ms,
               .advanced = {.queue_limit = 50},
               .ops = {{"x", {Step{.to = "b", .op = "x"}}}, {"y", {Step{.to = "b", .op = "y"}}}},
               .cost = {{"y", 4.0}}},
      NodeSpec{.id = "b",
               .type = NodeType::service,
               .capacity = Rate{1e6},
               .latency = 10us,
               .cost = {{"y", 10.0}}}};
  s.links = {LinkSpec{.from = "c", .to = "a", .latency = 0s},
             LinkSpec{.from = "a", .to = "b", .latency = 150ms}};
  auto sim = *Simulation::create(*Graph::build(s));
  simsys::engine::StepRecord r;
  for (int k = 0; k < 50; ++k) r = sim.step(k * 100ms, 100ms);
  REQUIRE_THAT(r.nodes[1].demand_per_s, WithinRel(2500.0, 1e-12));  // 500 + 4·500 units
  REQUIRE_THAT(r.nodes[1].drain_per_s, WithinRel(1200.0, 1e-12));   // 3 × 400
  REQUIRE_THAT(r.nodes[1].backlog, WithinRel(150.0, 1e-12));        // 3 × 50 units
  REQUIRE_THAT(r.nodes[2].demand_per_s,
               WithinRel(240.0 + 2400.0, 1e-12));  // 240 x + 240 y requests priced 1 and 10
}

TEST_CASE("estimate in the middle of a noisy run leaves the run's next step unchanged",
          "[theory]") {
  Scenario s = mm1(500);
  s.nodes[0].workload->noise = simsys::workload::Noise{.sigma = 0.3, .correlation = 1s};
  const auto g = *Graph::build(s);
  auto a = *Simulation::create(g);
  auto b = *Simulation::create(g);
  for (int k = 0; k < 7; ++k) {
    (void)a.step(k * 100ms, 100ms);
    (void)b.step(k * 100ms, 100ms);
  }
  (void)a.estimate();
  const auto ra = a.step(700ms, 100ms), rb = b.step(700ms, 100ms);
  REQUIRE(ra.nodes[1].demand_per_s == rb.nodes[1].demand_per_s);
  REQUIRE(ra.responses[0].offered == rb.responses[0].offered);
}

TEST_CASE("the curve command's noisy samples at the scenario step are the run's offered rate",
          "[theory]") {
  Scenario s = mm1(500);
  const simsys::workload::Noise noise{.sigma = 0.3, .correlation = 1s};
  s.nodes[0].workload->noise = noise;
  const auto g = *Graph::build(s);
  auto sim = *Simulation::create(g);
  auto samples = simsys::workload::samples(s.nodes[0].workload->curve, 100ms, 2s, noise,
                                           simsys::behaviours::client_seed(s.seed, 0));
  int k = 0;
  for (const auto& sample : samples)
    REQUIRE(sim.step(k++ * 100ms, 100ms).responses[0].offered.per_second == sample.rate.per_second);
}

// Round robin deals requests out blindly, so each replica has its own queue: two copies at
// 1000/s each under 1800/s total are two independent M/M/1 queues at rho = 900 / 1000 = 0.9,
// each with mean wait rho / (mu - lambda) = 9 ms (book ch. 1: a plain load balancer).
TEST_CASE("round robin keeps replicas as independent queues: each copy is an M/M/1 at its share",
          "[theory]") {
  const auto r = (*Simulation::create(*Graph::build(lb_front(LbPolicy::round_robin, 2, 1800))))
                     .step(0s, 100ms);
  const auto& a = r.nodes[2];
  REQUIRE_THAT(a.rho, WithinRel(0.9, 1e-12));
  const auto w = simsys::queueing::mmc_wait(1, 0.9, 0.001);
  REQUIRE_THAT(a.latency.mean_s, WithinRel(w.mean_s + 0.001, 1e-9));
}

// Least connections sends each request to the copy with the fewest requests in flight, so
// a request waits only when every copy is busy: the replicas behave as one pool, an M/M/c
// queue with c = 2 servers. Its wait is ErlangC(c, rho) * s / (c (1 - rho)), which at the
// same rho is far below the M/M/1 wait of round robin (the pooling effect).
TEST_CASE("least connections pools the replicas: one M/M/c queue with c = replicas", "[theory]") {
  const auto r =
      (*Simulation::create(*Graph::build(lb_front(LbPolicy::least_connections, 2, 1800))))
          .step(0s, 100ms);
  const auto& a = r.nodes[2];
  REQUIRE_THAT(a.rho, WithinRel(0.9, 1e-12));
  const auto w = simsys::queueing::mmc_wait(2, 0.9, 0.001);  // ErlangC(2, 0.9) s / (2 (1 - 0.9))
  REQUIRE_THAT(a.latency.mean_s, WithinRel(w.mean_s + 0.001, 1e-9));
  const auto round_robin =
      (*Simulation::create(*Graph::build(lb_front(LbPolicy::round_robin, 2, 1800))))
          .step(0s, 100ms);
  REQUIRE(a.latency.mean_s < round_robin.nodes[2].latency.mean_s);
}

// The ring's imbalance k, the hottest machine's load over the average (review of plan 2c Tasks
// 9-10, I-2): each of M machines holds the keys of its v virtual nodes' arcs, N copies of each key
// (N = 1 behind a balancer), so its share is a sum of N v arc lengths whose relative spread is
// about 1/sqrt(N v) (book ch. 5: about 10 % at 100 virtual nodes). The hottest of M such shares
// sits z_M standard deviations above the mean, z_M the expected maximum of M standard normals
// (Blom: queueing::expected_normal_maximum, unit-tested against its own figures):
//   k = min(1 + z_M / sqrt(N v), M / N)        (M / N: a machine holds at most every key once)
// v = 100, M = 4: 1 + 1.0491 / 10 = 1.1049;  v = 150, M = 4: 1.0857;  v = 1, M = 4: 2.0491.
namespace {
double ring_factor(int machines, int copies_per_key, int vnodes) {
  const double spread = 1.0 / std::sqrt(static_cast<double>(copies_per_key) * vnodes);
  return std::min(1.0 + simsys::queueing::expected_normal_maximum(machines) * spread,
                  static_cast<double>(machines) / copies_per_key);
}
}  // namespace

// A hash ring with v virtual nodes per server spreads keys unevenly: the busiest of the 4 servers
// holds k = 1 + z_4/sqrt(v) times the average share (ring_factor). With v = 100, k = 1.1049: the
// hottest copy sees 900/s x 1.1049 = 994.4/s, so rho = 0.9944.
TEST_CASE("consistent hashing loads the hottest replica by the imbalance factor", "[theory]") {
  Scenario s = lb_front(LbPolicy::consistent_hash, 4, 3600);  // 900/s per copy on average
  s.nodes[1].vnodes = 100;                                    // hottest copy sees 994.4/s
  const auto r = (*Simulation::create(*Graph::build(s))).step(0s, 100ms);
  REQUIRE_THAT(r.nodes[2].rho, WithinRel(0.9 * ring_factor(4, 1, 100), 1e-12));
  REQUIRE_THAT(r.nodes[2].rho, WithinAbs(0.9944, 1e-4));
  REQUIRE_THAT(r.nodes[2].drain_per_s, WithinRel(3600.0, 1e-12));  // the group still drains all
}

// The work balance of one step at one node (group totals, capacity units):
//   backlog before + demand × dt = (drain + dropped) × dt + backlog after.
// Nothing is created or lost inside a node, however its replicas are modelled.
namespace {
void require_conserved(double backlog_before, const simsys::engine::NodeSample& n, double dt_s) {
  REQUIRE_THAT(backlog_before + n.demand_per_s * dt_s,
               WithinRel((n.drain_per_s + n.dropped_per_s) * dt_s + n.backlog, 1e-12));
}
}  // namespace

// A hash ring with vnodes 1 over 4 copies: the hottest copy holds k = 1 + z_4/sqrt(1) = 2.0491
// times the average share (ring_factor), the other three split what is left. At 3000/s (750/s
// per copy on average) the hot copy is sent 2.0491 × 750 = 1536.8/s against 1000/s capacity, so it
// sheds 536.8/s once its 100-request room is full; each cool copy is sent (4 - k)/(4 - 1) × 750 =
// 487.7/s and serves all of it. The group drains 1000 + 3 × 487.7 = 2463.2/s: only the hot copy's
// excess is lost.
TEST_CASE(
    "a saturated hash ring sheds only the hottest copy's excess; the cooler copies serve their "
    "share",
    "[theory]") {
  Scenario s = lb_front(LbPolicy::consistent_hash, 4, 3000);
  s.nodes[1].vnodes = 1;
  s.nodes[2].advanced.queue_limit = 100;
  const auto g = *Graph::build(s);
  auto sim = *Simulation::create(g);
  double backlog = 0.0;
  simsys::engine::StepRecord last;
  sim.run({.duration = 2s, .step = 100ms}, [&](const auto& r) {
    require_conserved(backlog, r.nodes[2], 0.1);
    backlog = r.nodes[2].backlog;
    last = r;
  });
  const auto& a = last.nodes[2];
  const double hot_excess = 750.0 * ring_factor(4, 1, 1) - 1000.0;
  REQUIRE_THAT(hot_excess, WithinAbs(536.85, 0.01));
  REQUIRE_THAT(a.demand_per_s, WithinRel(3000.0, 1e-12));
  REQUIRE_THAT(a.drain_per_s, WithinRel(3000.0 - hot_excess, 1e-12));
  REQUIRE_THAT(a.dropped_per_s, WithinRel(hot_excess, 1e-12));
  REQUIRE_THAT(a.backlog, WithinRel(100.0, 1e-12));  // the hot copy's room; the cool ones are empty
  REQUIRE(a.rho == 1.0);                             // reported: the hottest copy
  REQUIRE(a.zone == simsys::fluid::Zone::red);
  const auto settled = sim.estimate();
  REQUIRE_THAT(settled.nodes[2].drain_per_s, WithinRel(3000.0 - hot_excess, 1e-12));
  REQUIRE_THAT(settled.nodes[2].dropped_per_s, WithinRel(hot_excess, 1e-12));
}

// A pooled group (least_connections) is ONE queue of N × the capacity and N × the waiting
// room: 3 copies of 1000/s with room for 100 each make a 3000/s pool with room for 300. At
// 4500/s the room fills by 1500 × 0.1 = 150 per step, is full after two steps, and from then
// on the pool sheds 1500/s; no step creates or loses work.
TEST_CASE("a pooled group conserves work every step and its waiting room is replicas × queue_limit",
          "[theory]") {
  Scenario s = lb_front(LbPolicy::least_connections, 3, 4500);
  s.nodes[2].advanced.queue_limit = 100;
  auto sim = *Simulation::create(*Graph::build(s));
  double backlog = 0.0;
  double most_waiting = 0.0;
  simsys::engine::StepRecord last;
  sim.run({.duration = 2s, .step = 100ms}, [&](const auto& r) {
    require_conserved(backlog, r.nodes[2], 0.1);
    backlog = r.nodes[2].backlog;
    most_waiting = std::max(most_waiting, backlog);
    last = r;
  });
  REQUIRE_THAT(most_waiting, WithinRel(300.0, 1e-12));
  REQUIRE_THAT(last.nodes[2].drain_per_s, WithinRel(3000.0, 1e-12));
  REQUIRE_THAT(last.nodes[2].dropped_per_s, WithinRel(1500.0, 1e-12));
}

// Behind a hash ring a node is two kinds of copy: the hottest, which a request reaches with
// probability k/N (it holds k times the average share), and the N - 1 cooler ones, which split
// the rest. A request's sojourn is therefore a mixture: with probability k/N the hot copy's
// M/M/1 sojourn at rho_hot = k × rho, otherwise a cool copy's at
// rho_cool = (N - k)/(N - 1) × rho. Here N = 4, vnodes 150 (k = 1 + z_4/sqrt(150) = 1.0857),
// average rho 0.85 on 1000/s copies with 1 ms service.
TEST_CASE("an imbalanced node's latency is the share-weighted mixture of the hot and cool copies",
          "[theory]") {
  constexpr double copies = 4;
  constexpr double rho = 0.85;
  Scenario s = lb_front(LbPolicy::consistent_hash, 4, rho * copies * 1000);
  s.nodes[1].vnodes = 150;
  const auto r = (*Simulation::create(*Graph::build(s))).step(0s, 100ms);
  const double k = ring_factor(4, 1, 150);
  const double rho_cool = (copies - k) / (copies - 1) * rho;
  const double hot = simsys::queueing::mmc_wait(1, k * rho, 0.001).mean_s + 0.001;
  const double cool = simsys::queueing::mmc_wait(1, rho_cool, 0.001).mean_s + 0.001;
  const double expected = k / copies * hot + (1 - k / copies) * cool;
  REQUIRE_THAT(r.nodes[2].latency.mean_s, WithinRel(expected, 1e-9));
  REQUIRE_THAT(r.nodes[2].rho, WithinRel(k * rho, 1e-12));  // rho and zone: the hottest copy's
}

// The balancer is itself a queue (100k/s, 50 us, so c = ceil(100000 * 50e-6) = 5 servers
// implied); at 900/s its rho is 0.009. Adding it to a chain must add exactly its own
// sojourn, mmc_wait(5, 0.009, 50 us) + 50 us, plus two zero-latency link legs.
TEST_CASE("a load balancer in the chain costs its own service time and nothing else", "[theory]") {
  const auto without = (*Simulation::create(*Graph::build(mm1(900)))).step(0s, 100ms);
  const auto with =
      (*Simulation::create(*Graph::build(lb_front(LbPolicy::round_robin, 1, 900)))).step(0s, 100ms);
  REQUIRE(with.responses.size() == 1);
  REQUIRE(without.responses.size() == 1);
  const auto lb_wait = simsys::queueing::mmc_wait(5, 900.0 / 100000.0, 50e-6);
  const double lb_sojourn = lb_wait.mean_s + 50e-6;
  REQUIRE_THAT(with.responses[0].response.mean_s - without.responses[0].response.mean_s,
               WithinRel(lb_sojourn, 1e-6));
}

// The rate limiter of ch. 4 turns excess requests away instead of queueing them. The client is
// not told: the limiter's own queue is empty and it drops nothing; what it refuses is counted
// in a separate channel (`rejected_per_s`), apart from the drops, and none of it is forwarded,
// so the service behind sees the admitted rate only. The client's `answered` rate is what got
// through: 1000 of the 4000 it offers (plan 2a refinement 8).
TEST_CASE("rejected requests are counted at the limiter and never reach the service", "[theory]") {
  const auto g = *Graph::build(limited(LimiterAlgorithm::token_bucket, 1000, 100, 4000));
  auto sim = *Simulation::create(g);
  simsys::engine::StepRecord last;
  // Ten 100 ms steps: whatever the bucket held at the start (at most burst = 100 tokens, one
  // step's worth) is long spent, so the record is the limited steady state.
  sim.run({.duration = 1s, .step = 100ms}, [&](const auto& r) { last = r; });
  const auto& limiter = last.nodes[*g.index_of("limiter")];
  const auto& api = last.nodes[*g.index_of("api")];
  REQUIRE_THAT(limiter.demand_per_s, WithinRel(4000.0, 1e-9));
  REQUIRE_THAT(limiter.rejected_per_s, WithinRel(3000.0, 1e-9));  // 4000 offered - 1000 admitted
  REQUIRE(limiter.dropped_per_s == 0.0);                          // its own queue never overflows
  REQUIRE_THAT(api.demand_per_s, WithinRel(1000.0, 1e-9));        // only the admitted flow arrives
  REQUIRE(api.rejected_per_s == 0.0);
  REQUIRE(last.responses.size() == 1);
  REQUIRE(last.responses[0].offered == Rate{4000});  // the client still offers all of it
  REQUIRE_THAT(last.responses[0].answered.per_second, WithinRel(1000.0, 1e-9));
}

// Leaky bucket = ONE slot that leaks a request every 1/limit (deterministic) with a waiting
// room of `burst`: 1500/s in against 1000/s out fills it by (1500 - 1000) x 0.1 s = 50 requests
// per step, so a 500-request room is full after ten steps, after which the excess 500/s
// overflows. The policy is "full bucket -> refuse" (ch. 4), so the overflow is REJECTED, not
// dropped: a leaky bucket that turns requests away is doing its job, not failing.
TEST_CASE("a leaky bucket is a queue with capacity limit and room burst", "[theory]") {
  const auto g = *Graph::build(limited(LimiterAlgorithm::leaky_bucket, 1000, 500, 1500));
  auto sim = *Simulation::create(g);
  const auto limiter = *g.index_of("limiter");
  int step = 0;
  sim.run({.duration = 2s, .step = 100ms}, [&](const auto& r) {
    const auto& node = r.nodes[limiter];
    CAPTURE(step);
    REQUIRE(node.dropped_per_s == 0.0);
    REQUIRE_THAT(node.demand_per_s, WithinRel(1500.0, 1e-9));
    REQUIRE_THAT(node.drain_per_s, WithinRel(1000.0, 1e-9));  // the fixed leak rate
    REQUIRE_THAT(r.nodes[*g.index_of("api")].demand_per_s, WithinRel(1000.0, 1e-9));
    if (step < 9) {  // 50 more waiting each step: 50, 100, ... 450
      REQUIRE_THAT(node.backlog, WithinRel(50.0 * (step + 1), 1e-9));
      REQUIRE(node.rejected_per_s == 0.0);
    } else if (step > 10) {  // the room (500) is full: the excess 500/s is refused
      REQUIRE_THAT(node.backlog, WithinRel(500.0, 1e-9));
      REQUIRE_THAT(node.rejected_per_s, WithinRel(500.0, 1e-9));
    }
    ++step;
  });
  REQUIRE(step == 20);
}

// Below its limit a leaky bucket still makes requests wait: arrivals are random (Poisson)
// but the leak is regular, an M/D/1 queue. Its wait is the Pollaczek-Khinchine mean, which
// is the M/M/1 wait times the Allen-Cunneen factor (ca² + cs²)/2 = (1 + 0)/2 = 1/2 (exact for
// M/G/1), plus the leak interval itself, 1/limit = 1 ms. At 800/s on 1000/s, rho = 0.8.
TEST_CASE("a leaky bucket's wait below the limit is M/D/1", "[theory]") {
  const auto g = *Graph::build(limited(LimiterAlgorithm::leaky_bucket, 1000, 500, 800));
  const auto r = (*Simulation::create(g)).step(0s, 100ms);
  const double wait = simsys::queueing::mmc_wait(1, 0.8, 0.001).mean_s *
                      simsys::queueing::variability_factor(1.0, 0.0);
  REQUIRE_THAT(r.nodes[*g.index_of("limiter")].latency.mean_s, WithinRel(wait + 0.001, 1e-9));
}

// A leaky bucket is one deterministic slot leaking a request every 1/limit with a room of `burst`
// (graph::leaky_bucket_capacity), so a request it admits finds at most `burst` ahead of it and
// waits at most burst / limit: 20 / 1000/s = 20 ms (the room-drain bound, exact for a drain this
// regular; review of plan 2c Task 3's second fix round, M-2). Erlang C knows no room: at 990/s
// its M/D/1 wait is (1 + 0) / 2 x 0.99 / 0.01 x 1 ms = 49.5 ms, so there the bound is what the
// wait is. At every load, below the limit, at it and above it (where the fluid backlog fills the
// room and its drain time is the bound itself), the mean wait never passes 20 ms.
TEST_CASE("a leaky bucket's mean wait never exceeds burst over limit, at any load", "[theory]") {
  constexpr double bound_s = 20.0 / 1000.0;  // burst / limit
  constexpr double leak_s = 0.001;           // the slot's service time, 1 / limit
  for (const double demand : {500.0, 900.0, 990.0, 1000.0, 1500.0}) {
    const auto g = *Graph::build(limited(LimiterAlgorithm::leaky_bucket, 1000, 20, demand));
    const auto limiter = *g.index_of("limiter");
    auto sim = *Simulation::create(g);
    double last_wait = 0.0;
    sim.run({.duration = 2s, .step = 100ms}, [&](const auto& r) {
      last_wait = r.nodes[limiter].latency.mean_s - leak_s;
      CAPTURE(demand, last_wait);
      REQUIRE(last_wait <= bound_s + 1e-12);
    });
    if (demand >= 990.0) REQUIRE_THAT(last_wait, WithinRel(bound_s, 1e-9));  // the bound binds
  }
}

// Rejections are policy, drops are failures (D5 of the rate-limiter review): a limiter that
// refuses 3000 of the 4000 requests it is offered each second has a reject rate of 0.75 and
// a drop rate of 0, and the api it protects drops nothing either. Both rates are per node
// (Σ rejected / Σ demand, Σ dropped / Σ demand); without a node, the worst node's.
TEST_CASE("a limiter's reject rate counts its refusals; the nodes it protects drop nothing",
          "[theory]") {
  const auto s = limited(LimiterAlgorithm::token_bucket, 1000, 100, 4000);
  const auto g = *Graph::build(s);
  // A warmup of 1 s keeps the first step, where the bucket may still hold a few tokens, out
  // of the average: the rest of the run is the limited steady state.
  simsys::report::Summary summary{g, simsys::SimTime{1s}};
  (*Simulation::create(g)).run({.duration = s.duration, .step = s.step}, [&](const auto& r) {
    summary.observe(r);
  });
  REQUIRE_THAT(summary.reject_rate("limiter"), WithinRel(0.75, 1e-9));
  REQUIRE(summary.drop_rate("limiter") == 0.0);
  REQUIRE(summary.drop_rate("api") == 0.0);
  REQUIRE(summary.reject_rate("api") == 0.0);
  REQUIRE(summary.drop_rate("u") == 0.0);
  REQUIRE(summary.drop_rate() == 0.0);
  REQUIRE_THAT(summary.reject_rate(), WithinRel(0.75, 1e-9));  // the worst node is the limiter
  REQUIRE(summary.worst_reject_node() == "limiter");
  REQUIRE(summary.worst_drop_node().empty());
}

// The client's latency summary averages over the requests ANSWERED (plan 2a refinement 8): a
// rejected request never sees the api's queue, so it must not weigh the api's latency. Here
// the counter check makes every answered request pay the counter store's hop, and the
// client's `answered` rate is the admitted 1000/s; the 3000/s refused are not in it.
TEST_CASE(
    "the counter store sees every request a limiter checks; the client is answered only the "
    "admitted",
    "[theory]") {
  auto s = limited(LimiterAlgorithm::token_bucket, 1000, 100, 4000);
  // A service stands in for the counter store: caches are not simulated yet, and the
  // graph (unlike the loader) takes any node as a backend.
  s.nodes.push_back(NodeSpec{.id = "counters", .type = NodeType::service, .capacity = Rate{1e6}});
  s.nodes[1].backend = "counters";
  s.links.push_back(LinkSpec{.from = "limiter", .to = "counters", .latency = 0s});
  const auto g = *Graph::build(s);
  simsys::engine::StepRecord last;
  (*Simulation::create(g)).run({.duration = 1s, .step = 100ms}, [&](const auto& r) { last = r; });
  REQUIRE_THAT(last.nodes[*g.index_of("counters")].demand_per_s, WithinRel(4000.0, 1e-9));
  REQUIRE_THAT(last.responses[0].answered.per_second, WithinRel(1000.0, 1e-9));
  const auto settled = (*Simulation::create(g)).estimate();
  REQUIRE_THAT(settled.nodes[*g.index_of("counters")].demand_per_s, WithinRel(4000.0, 1e-9));
  REQUIRE_THAT(settled.responses[0].answered.per_second, WithinRel(1000.0, 1e-9));
}

// Ch. 1, cache: with a hit ratio of 0.8 the cache absorbs four reads in five, so the database
// behind it sees 1000/s x (1 - 0.8) = 200/s while the cache itself is asked about all 1000/s.
// The same holds in the long-run state (estimate).
TEST_CASE(
    "a cache_aside cache in front of a database cuts the database's reads to the miss fraction",
    "[theory]") {
  const auto g = *Graph::build(cached_reads(0.8));
  const auto r = (*Simulation::create(g)).step(0s, 100ms);
  REQUIRE_THAT(r.nodes[*g.index_of("cache")].demand_per_s, WithinRel(1000.0, 1e-9));
  REQUIRE_THAT(r.nodes[*g.index_of("db")].demand_per_s, WithinRel(200.0, 1e-9));
  const auto settled = (*Simulation::create(g)).estimate();
  REQUIRE_THAT(settled.nodes[*g.index_of("cache")].demand_per_s, WithinRel(1000.0, 1e-9));
  REQUIRE_THAT(settled.nodes[*g.index_of("db")].demand_per_s, WithinRel(200.0, 1e-9));
}

// What the client waits for a read: web, then the cache (always), then the db for the 20 % the
// cache missed. With zero link latencies and the node latencies the record reports (each
// node's own sojourn: service time plus queueing wait), the mean is
//   web + cache + 0.2 x db            (expectation of the hit/miss mixture),
// and, the db's time being 0 for a hit and D for a miss, the variance adds the spread between
// the two cases to the variances of the sequential parts:
//   var web + var cache + 0.2 var D + 0.2 x 0.8 x (mean D)².
TEST_CASE("reads served from cache are faster: the client's mean is the hit/miss mixture",
          "[theory]") {
  const auto g = *Graph::build(cached_reads(0.8));
  const auto r = (*Simulation::create(g)).step(0s, 100ms);
  const auto& web = r.nodes[*g.index_of("web")].latency;
  const auto& cache = r.nodes[*g.index_of("cache")].latency;
  const auto& db = r.nodes[*g.index_of("db")].latency;
  REQUIRE(r.responses.size() == 1);
  REQUIRE_THAT(r.responses[0].response.mean_s,
               WithinRel(web.mean_s + cache.mean_s + 0.2 * db.mean_s, 1e-9));
  REQUIRE_THAT(r.responses[0].response.variance_s2,
               WithinRel(web.variance_s2 + cache.variance_s2 + 0.2 * db.variance_s2 +
                             0.2 * 0.8 * db.mean_s * db.mean_s,
                         1e-9));
  // The mixture really is cheaper than sending every read to the database.
  REQUIRE(r.responses[0].response.mean_s < web.mean_s + cache.mean_s + db.mean_s);
  // Every read is answered: nothing here drops or rejects.
  REQUIRE_THAT(r.responses[0].answered.per_second, WithinRel(1000.0, 1e-9));
}

TEST_CASE("a cache hit ratio of 1 sends nothing to the database and adds none of its latency",
          "[theory]") {
  const auto g = *Graph::build(cached_reads(1.0));
  const auto r = (*Simulation::create(g)).step(0s, 100ms);
  REQUIRE(r.nodes[*g.index_of("db")].demand_per_s == 0.0);
  const auto& web = r.nodes[*g.index_of("web")].latency;
  const auto& cache = r.nodes[*g.index_of("cache")].latency;
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(web.mean_s + cache.mean_s, 1e-9));
}

// The write_back strategy (acknowledge a write from the cache, flush it to the store later: a
// queue of dirty entries) and a cache's then steps are run by the engine, so a scenario that uses
// them is accepted. (What they do to the flow and the latency is checked by the cases below.)
TEST_CASE("a scenario with a write_back cache or cache then steps is runnable", "[theory]") {
  SECTION("the write_back strategy") {
    auto s = cached_reads(0.8);
    s.nodes[2].strategy = CacheStrategy::write_back;
    s.nodes[2].backend = "db";
    s.links.push_back(LinkSpec{.from = "cache", .to = "db", .latency = 0s});
    const auto g = Graph::build(s);
    INFO((g ? "" : g.error().message));
    REQUIRE(g.has_value());
    const auto sim = Simulation::create(*g);
    INFO((sim ? "" : sim.error().message));
    REQUIRE(sim.has_value());
  }
  // The cache forwards its own misses, so web no longer fetches them (both would be a double
  // fetch, which the graph refuses).
  SECTION("a cache's then steps") {
    auto s = cached_reads(0.8);
    s.nodes[1].ops["read"].pop_back();  // web.read = [cache.read]
    s.nodes[2].then = {Step{.to = "db", .op = "read", .on = StepCondition::miss}};
    s.links.push_back(LinkSpec{.from = "cache", .to = "db", .latency = 0s});
    const auto g = Graph::build(s);
    INFO((g ? "" : g.error().message));
    REQUIRE(g.has_value());
    const auto sim = Simulation::create(*g);
    INFO((sim ? "" : sim.error().message));
    REQUIRE(sim.has_value());
  }
}

// Conservation through a saturated cache with then steps. 2000 requests/s (get 80 %, write
// 20 %) reach a cache of 1000 capacity units/s where a write costs 2 units: demand 1600 + 2 x 400
// = 2400 units/s, so once its waiting room (100 units) is full it drains f = 1000/2400 of what it
// is sent and drops the rest. What it forwards is what it DRAINS, op for op, in requests:
//   svc   (miss step, factor 2, h = 0.4): 1600 f x 0.6 x 2 = 800/s
//   audit (always step):                   2000 f           = 833.3/s
//   db    (write_back flush of the writes): 400 f           = 166.7/s
// and the cache's own balance holds every step: backlog + demand dt = (drain + dropped) dt +
// backlog after.
TEST_CASE("a saturated cache forwards along its then steps only what it drains", "[theory]") {
  Scenario s{.name = "saturated-cache", .version = 1, .duration = 10s};
  NodeSpec cache{.id = "cache",
                 .type = NodeType::cache,
                 .capacity = Rate{1000},
                 .latency = 1ms,
                 .then = {Step{.to = "svc", .op = "q", .on = StepCondition::miss, .factor = 2.0},
                          Step{.to = "audit", .op = "log"}},
                 .cost = {{"write", 2.0}},
                 .backend = "db",
                 .hit_ratio = HitRatio{.fixed = 0.4},
                 .strategy = CacheStrategy::write_back};
  cache.advanced.queue_limit = 100;
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{2000}},
                                           .op_shares = {{"get", 0.8}, {"write", 0.2}}}},
             std::move(cache),
             NodeSpec{.id = "svc", .type = NodeType::service, .capacity = Rate{10'000}},
             NodeSpec{.id = "audit", .type = NodeType::service, .capacity = Rate{10'000}},
             NodeSpec{.id = "db", .type = NodeType::service, .capacity = Rate{10'000}}};
  s.links = {LinkSpec{.from = "u", .to = "cache", .latency = 0s},
             LinkSpec{.from = "cache", .to = "svc", .latency = 0s},
             LinkSpec{.from = "cache", .to = "audit", .latency = 0s},
             LinkSpec{.from = "cache", .to = "db", .latency = 0s}};
  const auto g = *Graph::build(s);
  auto sim = *Simulation::create(g);
  const auto cache_index = *g.index_of("cache");
  double backlog = 0.0;
  simsys::engine::StepRecord last;
  sim.run({.duration = 2s, .step = 100ms}, [&](const auto& r) {
    require_conserved(backlog, r.nodes[cache_index], 0.1);
    backlog = r.nodes[cache_index].backlog;
    last = r;
  });
  const auto& c = last.nodes[cache_index];
  REQUIRE_THAT(c.demand_per_s, WithinRel(2400.0, 1e-9));
  REQUIRE_THAT(c.drain_per_s, WithinRel(1000.0, 1e-9));
  const double f = 1000.0 / 2400.0;
  REQUIRE_THAT(last.nodes[*g.index_of("svc")].demand_per_s, WithinRel(1600 * f * 0.6 * 2, 1e-9));
  REQUIRE_THAT(last.nodes[*g.index_of("audit")].demand_per_s, WithinRel(2000 * f, 1e-9));
  REQUIRE_THAT(last.nodes[*g.index_of("db")].demand_per_s, WithinRel(400 * f, 1e-9));
}

// Search autocomplete (ch. 13): 1000 suggestions/s hit the browser cache, which answers 40 %
// itself; the other 60 % go on to the load balancer and the service. So the service sees
// 1000 x (1 - 0.4) = 600/s, and a client waits for the cache's own sojourn plus, for the 60 %
// that miss, the lb and service path (zero-latency links), a hit/miss mixture whose mean is
//   cache + 0.6 x (lb + svc).
TEST_CASE("a browser cache in front of a load balancer lets only its misses through", "[theory]") {
  Scenario s{.name = "browser", .version = 1, .duration = 10s};
  s.nodes = {
      NodeSpec{
          .id = "u",
          .type = NodeType::client,
          .workload = Workload{.curve = {.base = Rate{1000}}, .op_shares = {{"suggest", 1.0}}}},
      NodeSpec{.id = "browser_cache",
               .type = NodeType::cache,
               .capacity = Rate{100'000},
               .latency = 100us,
               .then = {Step{.to = "lb", .op = "suggest", .on = StepCondition::miss}},
               .hit_ratio = HitRatio{.fixed = 0.4},
               .strategy = CacheStrategy::cache_aside},
      NodeSpec{.id = "lb", .type = NodeType::load_balancer, .policy = LbPolicy::round_robin},
      NodeSpec{.id = "svc", .type = NodeType::service, .capacity = Rate{10'000}, .latency = 1ms}};
  s.links = {LinkSpec{.from = "u", .to = "browser_cache", .latency = 0s},
             LinkSpec{.from = "browser_cache", .to = "lb", .latency = 0s},
             LinkSpec{.from = "lb", .to = "svc", .latency = 0s}};
  const auto g = *Graph::build(s);
  const auto sim = Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
  const auto r = (*Simulation::create(g)).step(0s, 100ms);
  REQUIRE_THAT(r.nodes[*g.index_of("browser_cache")].demand_per_s, WithinRel(1000.0, 1e-12));
  REQUIRE_THAT(r.nodes[*g.index_of("svc")].demand_per_s, WithinRel(600.0, 1e-12));
  const auto& cache = r.nodes[*g.index_of("browser_cache")].latency;
  const auto& lb = r.nodes[*g.index_of("lb")].latency;
  const auto& svc = r.nodes[*g.index_of("svc")].latency;
  REQUIRE(svc.mean_s > 0.0);
  REQUIRE(r.responses.size() == 1);
  REQUIRE_THAT(r.responses[0].response.mean_s,
               WithinRel(cache.mean_s + 0.6 * (lb.mean_s + svc.mean_s), 1e-9));
  const auto settled = (*Simulation::create(g)).estimate();
  REQUIRE_THAT(settled.nodes[*g.index_of("svc")].demand_per_s, WithinRel(600.0, 1e-12));
}

// Write-back: the cache acknowledges a write from its own memory and flushes it to the store
// later. The store still has to absorb every write (its demand is the full 100/s: an upper
// bound, no coalescing), but the client's write latency is the cache's own sojourn: the slow
// flush is asynchronous and nobody waits for it.
TEST_CASE(
    "a write_back cache keeps the client's write latency to the cache's own sojourn while "
    "the backend still sees the writes",
    "[theory]") {
  Scenario s{.name = "write-back", .version = 1, .duration = 10s};
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{100}}, .op_shares = {{"write", 1.0}}}},
      NodeSpec{.id = "cache",
               .type = NodeType::cache,
               .capacity = Rate{100'000},
               .latency = 100us,
               .backend = "db",
               .hit_ratio = HitRatio{.fixed = 0.8},
               .strategy = CacheStrategy::write_back},
      NodeSpec{.id = "db", .type = NodeType::service, .capacity = Rate{1000}, .latency = 5ms}};
  s.links = {LinkSpec{.from = "u", .to = "cache", .latency = 0s},
             LinkSpec{.from = "cache", .to = "db", .latency = 0s}};
  const auto g = *Graph::build(s);
  const auto sim = Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
  const auto r = (*Simulation::create(g)).step(0s, 100ms);
  REQUIRE_THAT(r.nodes[*g.index_of("db")].demand_per_s, WithinRel(100.0, 1e-12));
  const auto& cache = r.nodes[*g.index_of("cache")].latency;
  REQUIRE(cache.mean_s > 0.0);
  REQUIRE(r.responses.size() == 1);
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(cache.mean_s, 1e-9));
  REQUIRE_THAT(r.responses[0].answered.per_second, WithinRel(100.0, 1e-9));
}

// A scenario built in code may hand a cache an empty HitRatio{} (neither a number nor `che`):
// that is a missing hit ratio, the loader's "cache needs a hit_ratio", not a che one.
TEST_CASE("a cache built in code with an empty hit ratio needs a hit_ratio", "[theory]") {
  auto s = cached_reads(0.8);
  s.nodes[2].hit_ratio = HitRatio{};
  const auto g = Graph::build(s);
  REQUIRE(g.has_value());
  const auto sim = Simulation::create(*g);
  REQUIRE_FALSE(sim.has_value());
  REQUIRE(sim.error().message == "node 'cache': cache needs a hit_ratio");
}

// ch. 8's URL shortener: 100M short links, the few popular ones taking most of the clicks (Zipf
// exponent 1), and a cache that holds 20M of them. The Che approximation turns "20M keys of
// 100M under Zipf(1)" into a hit ratio before the first step, and the fluid engine then treats
// it exactly like a stated one.
TEST_CASE("url-shortener's url_cache resolves to a che hit ratio and the simulation accepts it",
          "[theory]") {
  auto s = cached_reads(0.8);
  s.nodes[0].workload->keys = simsys::model::KeySpec{.zipf = 1.0, .space = 100e6};
  s.nodes[2].hit_ratio = HitRatio{.che = true};
  s.nodes[2].keys = 20'000'000;
  const auto g = Graph::build(s);
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  const auto h = g->hit_ratio(*g->index_of("cache"));
  REQUIRE(h.has_value());
  // Pinned to the dense reference: Che summed over all 100M ranks one by one gives 0.880624
  // (the bucketed sum is within 2e-6 of it, caching.cppm).
  REQUIRE_THAT(*h, WithinAbs(0.880624, 1e-5));
  const auto sim = Simulation::create(*g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
}

// The consistent-hashing lab (zipf 0.9 over 10M keys, four 2M-key cache servers behind a ring).
// With perfectly even arcs the four would act as one 8M-key cache: 0.962834 by the dense
// reference. With the lab's `vnodes: 1` the hottest server owns k/4 = 0.512283 of the keys
// (k = 1 + z_4/sqrt(1) = 2.049131, ring_factor) and the three others 0.162572 each, so the hot
// server is a 2M / 0.512283 = 3.904093M-key cache over the whole space and the cool ones hold
// their every key: h = 0.512283 × 0.844457 + 0.487717 × 1 = 0.920318.
// Provenance of 0.962834 and 0.844457: Che's equation solved by 100 bisection steps over all 10M
// ranks one at a time, in double precision and with no buckets (the method of test_caching.cpp's
// dense_che_hit_ratio, run outside the suite: 10M ranks x 100 steps take seconds), recomputed
// 2026-10-29 for the review of plan 2c Tasks 9-10, I-2 (0.848235 at the old k = 2, 4M keys).
TEST_CASE("the consistent-hashing lab's cache hits 0.963 on even arcs and 0.920 on one vnode",
          "[theory]") {
  REQUIRE_THAT(simsys::caching::che_hit_ratio(8e6, simsys::caching::Popularity::zipf(0.9, 10e6)),
               WithinAbs(0.962834, 1e-5));
  Scenario s{.name = "ring", .version = 1, .duration = 10s};
  NodeSpec cache{.id = "cache",
                 .type = NodeType::cache,
                 .replicas = 4,
                 .hit_ratio = HitRatio{.che = true},
                 .strategy = CacheStrategy::cache_aside};
  cache.keys = 2'000'000;
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{1000}},
                                    .op_shares = {{"get", 1.0}},
                                    .keys = simsys::model::KeySpec{.zipf = 0.9, .space = 10e6}}},
      NodeSpec{.id = "ring",
               .type = NodeType::load_balancer,
               .policy = LbPolicy::consistent_hash,
               .vnodes = 1},
      std::move(cache)};
  s.links = {LinkSpec{.from = "u", .to = "ring"}, LinkSpec{.from = "ring", .to = "cache"}};
  const auto g = Graph::build(s);
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  REQUIRE_THAT(*g->hit_ratio(*g->index_of("cache")), WithinAbs(0.920318, 1e-5));
}

// ---- SQL database: a primary and read replicas (plan 2b refinement 3, book ch. 1) ----

namespace {
// u(reads + writes per second) -> db, a SQL database of `replicas` copies (one primary plus
// replicas - 1 read replicas) in `shards` shards. Every copy has capacity 1000/s and a 1 ms
// service time, the link has zero latency, so a response is the database's own sojourn.
// Node indices: u 0, db 1. An op with no load is left out of the client's mix.
// `pool` and `replication_lag` are set only when given.
// A `write` costs 1 capacity unit here (stated in `cost`), not the database default of 5 (spec
// §6: "SQL 5k reads" vs "1k writes" per second): every derivation below then has one unit per
// query, so its rates are requests. The default has a test of its own (it clears the `cost`).
Scenario sql_front(int replicas, std::optional<int> shards, double reads, double writes,
                   std::optional<int> pool = std::nullopt,
                   std::optional<simsys::SimTime> replication_lag = std::nullopt) {
  Scenario s{.name = "sql-front", .version = 1, .duration = 10s};
  std::vector<std::pair<std::string, double>> shares;
  if (reads > 0) shares.emplace_back("read", reads / (reads + writes));
  if (writes > 0) shares.emplace_back("write", writes / (reads + writes));
  NodeSpec db{.id = "db",
              .type = NodeType::sql_db,
              .replicas = replicas,
              .capacity = Rate{1000},
              .latency = 1ms};
  db.shards = shards;
  db.pool = pool;
  db.replication_lag = replication_lag;
  db.cost = {{"write", 1.0}};
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{reads + writes}},
                                           .op_shares = std::move(shares)}},
             std::move(db)};
  s.links = {LinkSpec{.from = "u", .to = "db", .latency = 0s}};
  return s;
}
// The mean response the client sees for `op` in a one-step run of `s`.
double client_mean_s(const simsys::engine::StepRecord& r, std::string_view op) {
  // C++20 std::ranges::find with a projection: compares `op` with each sample's `op` member
  // (the projection, a pointer to member), replacing std::find_if over begin/end with a lambda
  // that reads the member.
  const auto it = std::ranges::find(r.responses, op, &simsys::engine::ResponseSample::op);
  REQUIRE(it != r.responses.end());
  return it->response.mean_s;
}
}  // namespace

// Book ch. 1 (database replication): writes go to the primary, reads to the replicas, and each
// replica also applies every write it is sent by the primary. With 3 copies that is 1 primary
// (100 writes/s: rho 0.1) and 2 replicas, each with 1000 / 2 reads + 100 replicated writes =
// 600 units/s (rho 0.6); both are independent M/M/1 queues (M/M/c with c = 1, `mmc_wait`).
// The node reports the busier queue's rho, and each op sees the sojourn of its own queue.
TEST_CASE(
    "writes go to the primary and reads to the replicas, each replica also applying every write",
    "[theory]") {
  const auto r =
      (*Simulation::create(*Graph::build(sql_front(3, std::nullopt, 1000, 100)))).step(0s, 100ms);
  const auto& db = r.nodes[1];
  REQUIRE_THAT(db.demand_per_s, WithinRel(1100.0, 1e-9));
  REQUIRE_THAT(db.rho, WithinRel(0.6, 1e-9));  // max(primary 0.1, replica 0.6)
  const auto replica = simsys::queueing::mmc_wait(1, 0.6, 0.001);
  const auto primary = simsys::queueing::mmc_wait(1, 0.1, 0.001);
  REQUIRE_THAT(client_mean_s(r, "read"), WithinRel(replica.mean_s + 0.001, 1e-9));
  REQUIRE_THAT(client_mean_s(r, "write"), WithinRel(primary.mean_s + 0.001, 1e-9));
}

// With one copy there are no replicas: the primary serves reads and writes alike, one M/M/1
// queue at (500 + 100) / 1000 = 0.6, and both ops see its sojourn.
TEST_CASE("with one replica everything is on the primary", "[theory]") {
  const auto r =
      (*Simulation::create(*Graph::build(sql_front(1, std::nullopt, 500, 100)))).step(0s, 100ms);
  REQUIRE_THAT(r.nodes[1].rho, WithinRel(0.6, 1e-9));
  const auto shared = simsys::queueing::mmc_wait(1, 0.6, 0.001);
  REQUIRE_THAT(client_mean_s(r, "read"), WithinRel(shared.mean_s + 0.001, 1e-9));
  REQUIRE_THAT(client_mean_s(r, "write"), WithinRel(shared.mean_s + 0.001, 1e-9));
}

// Sharding (book ch. 1, horizontal scaling): the data is split over S independent sets of the
// same shape, so each shard's primary and replicas see 1/S of the load. Here each replica sees
// (1000 / 2 + 100) / 2 = 300 units/s per shard: rho 0.3 (the primary: 100 / 2 / 1000 = 0.05).
TEST_CASE("shards divide both loads", "[theory]") {
  const auto r = (*Simulation::create(*Graph::build(sql_front(3, 2, 1000, 100)))).step(0s, 100ms);
  REQUIRE_THAT(r.nodes[1].rho, WithinRel(0.3, 1e-9));
  const auto replica = simsys::queueing::mmc_wait(1, 0.3, 0.001);
  const auto primary = simsys::queueing::mmc_wait(1, 0.05, 0.001);
  REQUIRE_THAT(client_mean_s(r, "read"), WithinRel(replica.mean_s + 0.001, 1e-9));
  REQUIRE_THAT(client_mean_s(r, "write"), WithinRel(primary.mean_s + 0.001, 1e-9));
}

// A replica applies a write `replication_lag` after the primary has finished it, and then waits
// its turn in the replica's own queue behind the reads and the other replayed writes, so data a
// replica serves can be as old as: the primary's sojourn (wait + service) + the lag + the
// replica's sojourn. With 3 copies, 1000 reads/s and 100 writes/s the primary runs at rho 0.1
// and each replica at (500 + 100) / 1000 = 0.6 (M/M/1 waits, `mmc_wait` with c = 1). With no
// replica nothing is stale.
TEST_CASE("staleness is the replication lag plus the write's sojourn at the primary and a replica",
          "[theory]") {
  SECTION("three copies: 50 ms lag, the primary at rho 0.1, the replicas at 0.6") {
    const auto r = (*Simulation::create(
                        *Graph::build(sql_front(3, std::nullopt, 1000, 100, std::nullopt, 50ms))))
                       .step(0s, 100ms);
    REQUIRE(r.nodes[1].staleness_s.has_value());
    const auto primary = simsys::queueing::mmc_wait(1, 0.1, 0.001);
    const auto replica = simsys::queueing::mmc_wait(1, 0.6, 0.001);
    REQUIRE_THAT(*r.nodes[1].staleness_s,
                 WithinRel(0.050 + primary.mean_s + 0.001 + replica.mean_s + 0.001, 1e-9));
  }
  SECTION("one copy: nothing is stale") {
    const auto r = (*Simulation::create(
                        *Graph::build(sql_front(1, std::nullopt, 500, 100, std::nullopt, 50ms))))
                       .step(0s, 100ms);
    REQUIRE(r.nodes[1].staleness_s.value_or(0.0) == 0.0);
  }
  SECTION("a node that is not a database has no staleness") {
    const auto r = (*Simulation::create(*Graph::build(mm1(500)))).step(0s, 100ms);
    REQUIRE_FALSE(r.nodes[1].staleness_s.has_value());
  }
}

// The two queues a write crosses on its way to a reader both count, with their waits (review of
// 48d4d0b, C1). 3 copies, 200 reads/s and 800 writes/s: the primary runs at 800 / 1000 = 0.8,
// so a write waits rho / (1 − rho) × s = 4 ms there (M/M/1), four times its 1 ms service; each
// replica applies the 800 writes plus its 100 reads, rho 0.9, and waits 9 ms. Staleness =
// 50 ms lag + (4 + 1) ms at the primary + (9 + 1) ms at the replica = 65 ms.
TEST_CASE("staleness counts the wait in the primary's write queue and the replica's sojourn",
          "[theory]") {
  const auto r =
      (*Simulation::create(*Graph::build(sql_front(3, std::nullopt, 200, 800, std::nullopt, 50ms))))
          .step(0s, 100ms);
  const auto primary = simsys::queueing::mmc_wait(1, 0.8, 0.001);
  const auto replica = simsys::queueing::mmc_wait(1, 0.9, 0.001);
  REQUIRE(r.nodes[1].staleness_s.has_value());
  REQUIRE_THAT(*r.nodes[1].staleness_s,
               WithinRel(0.050 + primary.mean_s + 0.001 + replica.mean_s + 0.001, 1e-9));
  REQUIRE_THAT(*r.nodes[1].staleness_s, WithinRel(0.065, 1e-9));
}

// A replica replays the primary's log, and the log holds only the writes the primary COMMITTED
// (review of 48d4d0b, C2): what the primary dropped never reaches a replica. 2 copies, 100
// reads/s + 1500 writes/s, 1000/s per copy, settled (estimate: both queues full). The primary
// drains 1000 writes/s and drops 500. The replica is sent its 100 reads + the 1000 committed
// writes = 1100/s against 1000/s: it drops 100/s in its queue's mix, 100 × 100/1100 reads (the
// clients' part, in the node's drops) and 100 × 1000/1100 replicated writes (reported apart,
// `replication_dropped_per_s`). Every write costs 1 here (the fixture), so units are requests.
TEST_CASE("replicas replay only the writes the primary committed", "[theory]") {
  const auto r =
      (*Simulation::create(*Graph::build(sql_front(2, std::nullopt, 100, 1500)))).estimate();
  REQUIRE(r.converged);
  REQUIRE_THAT(r.nodes[1].dropped_per_s, WithinRel(500.0 + 100.0 * 100.0 / 1100.0, 1e-6));
  REQUIRE(r.nodes[1].replication_dropped_per_s.has_value());
  REQUIRE_THAT(*r.nodes[1].replication_dropped_per_s, WithinRel(100.0 * 1000.0 / 1100.0, 1e-6));
  // What the replica must replay is what the primary committed, 1000/s, not the 1500/s offered.
  REQUIRE_THAT(r.nodes[1].committed_writes_per_s.value_or(-1.0), WithinRel(1000.0, 1e-6));
}

// The balance demand·dt = (drain + dropped)·dt + Δbacklog holds for the CLIENTS' queries even
// while a replica is overloaded by replication (review of 48d4d0b, I4). 2 copies, 800 reads/s +
// 400 writes/s: the primary commits all 400 writes (rho 0.4); the replica is sent 800 + 400 =
// 1200/s against 1000/s, its queue grows by 20 a step and is full (1000) from step 50 on. Its
// content keeps the arrival mix, 800 : 400, so the backlog the node shows is the clients' two
// thirds of the full queue, 1000 × 800/1200 = 666.7, below queue_limit while it already drops;
// the replicated third of the drops is reported apart.
TEST_CASE("a database's client totals balance while a replica drops replicated writes",
          "[theory]") {
  auto sim = *Simulation::create(*Graph::build(sql_front(2, std::nullopt, 800, 400)));
  double in = 0.0;
  double out = 0.0;
  double backlog = 0.0;
  for (int k = 0; k < 200; ++k) {
    // Kept in a variable of its own: `db` refers into it. Binding `db` to
    // `sim.step(...).nodes[1]` directly would dangle (the temporary StepRecord dies at the end
    // of that full-expression): GCC's -Wdangling-reference warns about it there, and rightly so,
    // the reference is real, not a false positive.
    const auto record = sim.step(k * 100ms, 100ms);
    const auto& db = record.nodes[1];
    in += db.demand_per_s * 0.1;
    out += (db.drain_per_s + db.dropped_per_s) * 0.1;
    backlog = db.backlog;
  }
  REQUIRE_THAT(in, WithinRel(out + backlog, 1e-9));
  REQUIRE_THAT(backlog, WithinRel(1000.0 * 800.0 / 1200.0, 1e-6));
  // 200/s dropped at the replica, a third of it replicated writes.
  REQUIRE_THAT(sim.estimate().nodes[1].replication_dropped_per_s.value(),
               WithinRel(200.0 * 400.0 / 1200.0, 1e-6));
}

// Replicated writes a replica dropped are a database's number: 0 while every replica keeps up,
// empty for a node that keeps no replicated data; the same for the committed writes a replica
// must replay (all 100/s here: the primary keeps up).
TEST_CASE("only a database reports replication drops", "[theory]") {
  SECTION("a database whose replicas keep up drops none") {
    const auto r =
        (*Simulation::create(*Graph::build(sql_front(3, std::nullopt, 1000, 100)))).estimate();
    REQUIRE(r.nodes[1].replication_dropped_per_s.value_or(-1.0) == 0.0);
    REQUIRE_THAT(r.nodes[1].committed_writes_per_s.value_or(-1.0), WithinRel(100.0, 1e-9));
  }
  SECTION("a service has none to report") {
    const auto r = (*Simulation::create(*Graph::build(mm1(500)))).estimate();
    REQUIRE_FALSE(r.nodes[1].replication_dropped_per_s.has_value());
    REQUIRE_FALSE(r.nodes[1].committed_writes_per_s.has_value());
  }
}

// The replication drops are ONE replica's, beside the writes ONE replica must replay (re-review
// of 96848f0): summed over the replicas they could exceed the writes committed, which reads as
// nonsense ("2131/s dropped of 900/s"). 3 copies × 2 shards, 3200 reads/s + 600 writes/s, 1000/s
// per copy, settled. Each shard's primary is sent 600 / 2 = 300 writes/s (rho 0.3) and commits
// them all: committed per shard = 300/s. Each of the (3 − 1) × 2 = 4 replicas is sent
// 3200 / 4 = 800 reads + its shard's 300 committed writes = 1100/s against 1000/s, so it drops
// 100/s in its queue's mix, 100 × 300/1100 = 27.27/s of them replicated writes. That is the
// per-replica figure; the four replicas together drop 4 × 27.27 = 109.1/s, which is not reported.
TEST_CASE("replication drops are one replica's, beside the committed writes per shard",
          "[theory]") {
  const auto r = (*Simulation::create(*Graph::build(sql_front(3, 2, 3200, 600)))).estimate();
  REQUIRE(r.converged);
  REQUIRE_THAT(r.nodes[1].committed_writes_per_s.value_or(-1.0), WithinRel(300.0, 1e-6));
  REQUIRE_THAT(r.nodes[1].replication_dropped_per_s.value_or(-1.0),
               WithinRel(100.0 * 300.0 / 1100.0, 1e-6));
}

// `pool` is the connection pool: how many queries one copy has in service at once, the c of its
// M/M/c queue. Little's law (concurrency in service = throughput × service time) ties it to the
// throughput: 5k/s × 10 ms keeps 50 queries in service, and `pool: 25` lets only 25 run, so a
// copy serves at most 25 / 10 ms = 2500/s with 25 servers. Reads only, 3 copies: each of the 2
// replicas is sent 500/s, rho 500 / 2500 = 0.2, and a read waits Erlang C's wait for c = 25
// plus its 10 ms.
TEST_CASE("a pool below Little's law's concurrency is each queue's c and bounds its rate",
          "[theory]") {
  auto s = sql_front(3, std::nullopt, 1000, 0, 25);
  s.nodes[1].capacity = Rate{5000};
  s.nodes[1].latency = 10ms;
  const auto r = (*Simulation::create(*Graph::build(s))).step(0s, 100ms);
  REQUIRE_THAT(r.nodes[1].rho, WithinRel(0.2, 1e-9));
  const auto pooled = simsys::queueing::mmc_wait(25, 0.2, 0.010);
  REQUIRE_THAT(client_mean_s(r, "read"), WithinRel(pooled.mean_s + 0.010, 1e-9));
}

// A pool at or above the concurrency Little's law implies changes nothing: the disk, not the
// connections, is the limit (review of 48d4d0b, C3). 1000/s × 1 ms implies one query in service;
// `pool: 4` leaves the replicas at M/M/1, rho 0.5, as with no pool at all.
TEST_CASE("a pool above the concurrency Little's law implies changes nothing", "[theory]") {
  const auto r =
      (*Simulation::create(*Graph::build(sql_front(3, std::nullopt, 1000, 0, 4)))).step(0s, 100ms);
  REQUIRE_THAT(r.nodes[1].rho, WithinRel(0.5, 1e-9));
  const auto single = simsys::queueing::mmc_wait(1, 0.5, 0.001);
  REQUIRE_THAT(client_mean_s(r, "read"), WithinRel(single.mean_s + 0.001, 1e-9));
}

// A small pool caps the throughput, it never speeds the disk: 4 connections of a 10 ms query
// carry 4 / 0.010 = 400 queries/s (Little's law), whatever the disk's 5k/s. One copy, 3000
// reads/s: it drains 400/s at rho 1 and, settled with a full queue, drops the other 2600/s.
TEST_CASE("a small pool bounds a database's throughput", "[theory]") {
  auto s = sql_front(1, std::nullopt, 3000, 0, 4);
  s.nodes[1].capacity = Rate{5000};
  s.nodes[1].latency = 10ms;
  const auto r = (*Simulation::create(*Graph::build(s))).estimate();
  REQUIRE_THAT(r.nodes[1].rho, WithinRel(1.0, 1e-12));
  REQUIRE_THAT(r.nodes[1].drain_per_s, WithinRel(400.0, 1e-9));
  REQUIRE_THAT(r.nodes[1].dropped_per_s, WithinRel(2600.0, 1e-9));
}

// A write costs a database 5 capacity units unless the scenario says otherwise: the spec's §6
// table gives one SQL machine "5k reads" or "1k writes" per second (a write's disk seek and
// log flush), 5000 / 1000 = 5 reads' worth. One copy of 1000 units/s, 100 writes/s: 500
// units/s, rho 0.5. A stated `cost` replaces the default.
TEST_CASE("a write costs five units on a database by default", "[theory]") {
  auto s = sql_front(1, std::nullopt, 0, 100);
  s.nodes[1].cost.clear();
  SECTION("the default") {
    const auto r = (*Simulation::create(*Graph::build(s))).step(0s, 100ms);
    REQUIRE_THAT(r.nodes[1].demand_per_s, WithinRel(500.0, 1e-9));
    REQUIRE_THAT(r.nodes[1].rho, WithinRel(0.5, 1e-9));
  }
  SECTION("a stated cost") {
    s.nodes[1].cost = {{"write", 2.0}};
    const auto r = (*Simulation::create(*Graph::build(s))).step(0s, 100ms);
    REQUIRE_THAT(r.nodes[1].rho, WithinRel(0.2, 1e-9));
  }
}

// ---- Request-based CDN (plan 2b refinement 4, Task 5) ----
//
// Book ch. 1: a CDN is a cache at the network's edge. Here it is a read-only cache in front of
// an origin: it answers the share h of the reads itself and sends the misses, as the same op, to
// the origin. Everything but the hit ratio is chosen so the numbers come from it alone.

namespace {
// u (`reads`/s of `read`) -> cdn -> origin. The cdn (10k/s, 1 ms service: c = 10 servers) has the
// given hit ratio (none: it misses every read); the origin (1000/s, 10 ms service: c = 10) is far
// from saturated at the loads used. The u -> cdn link has no latency, the cdn -> origin link
// 10 ms one way (a round trip of 20 ms): the "origin hop" a miss adds is that plus the origin's
// response. Node indices: u 0, cdn 1, origin 2.
Scenario cdn_origin_chain(double reads, std::optional<double> hit) {
  Scenario s{.name = "cdn-origin", .version = 1, .duration = 10s};
  NodeSpec cdn{.id = "cdn", .type = NodeType::cdn, .capacity = Rate{10'000}, .latency = 1ms};
  if (hit) cdn.hit_ratio = HitRatio{.fixed = *hit};
  cdn.origin = "origin";
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{reads}}, .op_shares = {{"read", 1.0}}}},
      std::move(cdn),
      NodeSpec{.id = "origin", .type = NodeType::service, .capacity = Rate{1000}, .latency = 10ms}};
  s.links = {LinkSpec{.from = "u", .to = "cdn", .latency = 0s},
             LinkSpec{.from = "cdn", .to = "origin", .latency = 10ms}};
  return s;
}
}  // namespace

// 1000 reads/s at a CDN that hits 95 % of them:
//   cdn:    demand 1000/s, capacity 10k/s over c = 10 servers (10k/s x 1 ms), rho = 0.1;
//   origin: demand 1000 x (1 - 0.95) = 50/s of the same op, capacity 1000/s, c = 10 (1000/s x
//           10 ms), rho = 50 / 1000 = 0.05.
// Each node's own sojourn is its M/M/c wait plus its service time (Erlang C: mmc_wait):
//   cdn    mmc_wait(10, 0.1, 1 ms)   = 1.25e-11 s  (P(wait) = 1.13e-7, theta = 1 ms / 9)
//   origin mmc_wait(10, 0.05, 10 ms) = 1.8e-13 s   (P(wait) = 1.72e-10, theta = 10 ms / 9.5)
// so almost nothing queues and the sojourns are the service times, 1 ms and 10 ms. A read the cdn
// hits costs its own sojourn; one it misses adds the origin hop (2 x 10 ms of link + the origin's
// sojourn), taken by 5 % of the reads:
//   mean = cdn + 0.05 x (2 x 10 ms + origin) = 1 ms + 0.05 x 30 ms = 2.5 ms (to within 1e-10 s).
TEST_CASE("a cdn answers its hits alone and sends the origin the miss share of the reads",
          "[theory]") {
  const auto g = *Graph::build(cdn_origin_chain(1000, 0.95));
  const auto sim = Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
  const auto r = (*Simulation::create(g)).step(0s, 100ms);
  REQUIRE_THAT(r.nodes[*g.index_of("cdn")].demand_per_s, WithinRel(1000.0, 1e-9));
  REQUIRE_THAT(r.nodes[*g.index_of("origin")].demand_per_s, WithinRel(50.0, 1e-9));
  const auto cdn = simsys::queueing::mmc_wait(10, 0.1, 0.001).mean_s + 0.001;
  const auto origin = simsys::queueing::mmc_wait(10, 0.05, 0.010).mean_s + 0.010;
  REQUIRE_THAT(r.nodes[*g.index_of("cdn")].latency.mean_s, WithinRel(cdn, 1e-9));
  REQUIRE_THAT(r.nodes[*g.index_of("origin")].latency.mean_s, WithinRel(origin, 1e-9));
  REQUIRE(r.responses.size() == 1);
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(cdn + 0.05 * (2 * 0.010 + origin), 1e-9));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(0.0025, 1e-7));  // the napkin number
  REQUIRE_THAT(r.responses[0].answered.per_second, WithinRel(1000.0, 1e-9));
  // The long-run state agrees: nothing here depends on the order of the steps.
  const auto settled = (*Simulation::create(g)).estimate();
  REQUIRE_THAT(settled.nodes[*g.index_of("origin")].demand_per_s, WithinRel(50.0, 1e-9));
}

// A CDN the scenario gave no hit ratio misses every read (h = 0) but still runs and still adds
// its own hop: 400 reads/s all reach the origin (rho = 400 / 1000 = 0.4, c = 10, a queue worth
// mmc_wait(10, 0.4, 10 ms) = 1.47e-5 s of waiting: P(wait) = 0.0088, theta = 10 ms / 6).
TEST_CASE("a cdn without a hit ratio runs and passes every read on to its origin", "[theory]") {
  const auto g = *Graph::build(cdn_origin_chain(400, std::nullopt));
  const auto sim = Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
  const auto r = (*Simulation::create(g)).step(0s, 100ms);
  REQUIRE_THAT(r.nodes[*g.index_of("origin")].demand_per_s, WithinRel(400.0, 1e-9));
  const auto cdn = simsys::queueing::mmc_wait(10, 0.04, 0.001).mean_s + 0.001;  // rho 400/10k
  const auto origin = simsys::queueing::mmc_wait(10, 0.4, 0.010).mean_s + 0.010;
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(cdn + (2 * 0.010 + origin), 1e-9));
}

// A `che` CDN without a `keys` size has no number for h: explain shows the paper load as an upper
// bound (every read a miss), but a run would silently replace the Che approximation the scenario
// asked for by h = 0, so the engine refuses it and says what is missing.
TEST_CASE("a che cdn without a keys size builds but the engine refuses to run it", "[theory]") {
  auto s = cdn_origin_chain(1000, std::nullopt);
  s.nodes[1].hit_ratio = HitRatio{.che = true};
  const auto g = Graph::build(std::move(s));
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  const auto sim = Simulation::create(*g);
  REQUIRE_FALSE(sim.has_value());
  REQUIRE(sim.error().message ==
          "node 'cdn': hit_ratio che needs keys (the cdn's size in objects)");
}

// ---- Byte-sized nodes count megabytes (plan 2c refinement 2, Task 3 and its review) ----
//
// Spec section 6: a blob store does 500 MB/s and a CDN's per-MB transfer takes 10 ms; the book's
// latency table gives a megabyte 30 ms from disk and 10 ms over the network. A node whose
// capacity is a byte rate therefore counts MEGABYTES: capacity = bytes/s / 1e6 units/s, a request
// of op o costs request_bytes(o) / 1e6 units, and its service time s_MB is the time of ONE
// megabyte, so the implied concurrency is Little's law again, c = units/s x s_MB.
//
// The queue waits for whole OBJECTS (review of Task 3 at a8b6cb4, I1): each request is a batch of
// megabytes that holds one of the c slots for its whole transfer, size x s_MB, so Erlang C is fed
// the rate-weighted mean object service time
//   s_obj = sum_o (rate_o x MB_o x s_MB) / sum_o rate_o
// with the same c. rho is unchanged (Little's law: objects/s x s_obj / c = MB/s / capacity), only
// the wait is a whole object's. A request is then answered (I2):
//   a write (an op in the store's write_ops)  at its LAST byte:  Wq + MB_o x s_MB
//   a read or a stream                         at its FIRST MB:   Wq + s_MB
// and a request under 1 MB still takes one megabyte's time either way (the small-object bound).
//
// The second fix round (review at 0248a47): a transfer of a fixed size at a fixed rate takes a
// fixed time, so each object's own service is deterministic (SCV 0) and the spread of the service
// times is the size mix's alone (I2). The queue's Allen-Cunneen factor is (c_a^2 + c_s^2) / 2 with
// c_s^2 the mixture's SCV (I1):
//   E[S]   = sum_o p_o s_o,   E[S^2] = sum_o p_o (1 + cs_o^2) s_o^2   (p_o = rate_o / sum rate)
//   c_s^2  = E[S^2] / E[S]^2 - 1
// so a single size with Poisson arrivals waits (1 + 0) / 2 = HALF the M/M/c wait (M/D/c), and the
// answer adds no variance of its own: the response's variance is the wait's. And the stochastic
// wait is bounded by the room (C1): never longer than queue_limit / capacity, the room-drain
// bound.

namespace {
// u (`rate`/s of `op`, each `size` long) -> store, a zero-latency link, so the client's response
// is the store's sojourn alone. Node indices: u 0, store 1. The store is a blob store with its
// type's defaults (spec section 6: 500 MB/s, 40 ms per MB) unless one is passed.
Scenario blob_store_chain(const std::string& op, double rate, simsys::Bytes size,
                          NodeSpec store = NodeSpec{.id = "store", .type = NodeType::blob_store}) {
  Scenario s{.name = "blob-chain", .version = 1, .duration = 10s};
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{rate}},
                                           .op_shares = {{op, 1.0}},
                                           .sizes = {{op, size}}}},
             std::move(store)};
  s.links = {LinkSpec{.from = "u", .to = "store", .latency = 0s}};
  return s;
}

// A default blob store that counts `op` as a write (its `write_ops`), as youtube's original_store
// counts `upload`.
NodeSpec store_writing(const std::string& op) {
  return NodeSpec{
      .id = "store", .type = NodeType::blob_store, .write_ops = std::vector<std::string>{op}};
}

// The first step of a fresh simulation of `g`, failing the test with the engine's message when it
// refuses the scenario (instead of dereferencing the refusal).
simsys::engine::StepRecord first_step(const Graph& g) {
  auto sim = Simulation::create(g);
  if (!sim) FAIL(sim.error().message);
  return sim->step(0s, 100ms);
}
}  // namespace

// 5 uploads/s of 50 MB into a default blob store that counts `upload` as a write:
//   demand   = 5/s x 50 MB = 250 units/s of 1 MB, against 500 units/s: rho = 250 / 500 = 0.5;
//   c        = 500 units/s x 40 ms per MB = 20 (Little's law);
//   s_obj    = 50 MB x 40 ms = 2 s, the time one upload holds a slot, the same for every upload
//              (a deterministic transfer: c_s^2 = 0);
//   Erlang C(20, 0.5) = 0.0037311 (queueing::erlang_c), so the M/M/c wait would be
//   C x s_obj / (c (1 - rho)) = 0.0037311 x 2 s / (20 x 0.5) = 0.746 ms, and M/D/c's is
//   Wq       = (1 + 0) / 2 x 0.746 ms = 0.373 ms                (Allen-Cunneen, Poisson arrivals)
//   response = Wq + 50 MB x 40 ms = 2.000373 s: a write is answered at its last byte.
// Before the first review the wait was computed over 1 MB units (0.0149 ms) and the response was
// the first megabyte's 40 ms: 50 times too short a wait and an upload acknowledged before its data.
TEST_CASE("a default blob store fed 5 uploads of 50 MB per second runs at rho 0.5 with c 20",
          "[theory]") {
  const auto g =
      *Graph::build(blob_store_chain("upload", 5, simsys::Bytes{50e6}, store_writing("upload")));
  const auto r = first_step(g);
  const auto& store = r.nodes[*g.index_of("store")];
  REQUIRE_THAT(store.demand_per_s, WithinRel(250.0, 1e-12));  // units (MB) per second
  REQUIRE_THAT(store.rho, WithinRel(0.5, 1e-12));
  REQUIRE(store.dropped_per_s == 0.0);
  const auto wait = simsys::queueing::mmc_wait(20, 0.5, 2.0).mean_s / 2.0;  // M/D/c
  REQUIRE_THAT(wait, WithinAbs(3.7311e-4, 1e-8));  // the napkin number: 0.373 ms
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(wait + 2.0, 1e-9));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(2.000373, 1e-6));
  // The node's own latency is its requests' sojourn: here every request is an upload.
  REQUIRE_THAT(store.latency.mean_s, WithinRel(wait + 2.0, 1e-9));
  REQUIRE_THAT(r.responses[0].answered.per_second, WithinRel(5.0, 1e-9));  // requests, not MB
}

// The same queue read instead of written: 5 reads/s of 50 MB wait the same whole-object 0.373 ms
// (the queue does not care which way the bytes go), but a read is answered when its first
// megabyte arrives, Wq + 40 ms = 40.373 ms: the client reads the rest as it streams in.
TEST_CASE("a 50 MB read waits behind whole objects and is answered at its first megabyte",
          "[theory]") {
  const auto g = *Graph::build(blob_store_chain("read", 5, simsys::Bytes{50e6}));
  const auto r = first_step(g);
  REQUIRE_THAT(r.nodes[*g.index_of("store")].rho, WithinRel(0.5, 1e-12));
  const auto wait = simsys::queueing::mmc_wait(20, 0.5, 2.0).mean_s / 2.0;  // M/D/c
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(wait + 0.040, 1e-9));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(0.040373, 1e-6));
}

// The review's example: 9 reads/s of 50 MB at a default store are 450 MB/s, rho 0.9. Over 1 MB
// units the M/M/c wait would be C(20, 0.9) x 40 ms / (20 x 0.1) = 0.5508 x 0.02 s = 11 ms; whole
// 50 MB objects (s_obj = 2 s) wait C(20, 0.9) x 2 s / (20 x 0.1) = 0.5508 s in M/M/c, fifty times
// longer, and as transfers of one fixed time (M/D/c) half of that, 0.2754 s. The read is answered
// 40 ms after that wait: 0.3154 s.
TEST_CASE("at rho 0.9 a 50 MB object waits 0.28 s, half the M/M/c wait and not 11 ms", "[theory]") {
  const auto g = *Graph::build(blob_store_chain("read", 9, simsys::Bytes{50e6}));
  const auto r = first_step(g);
  REQUIRE_THAT(r.nodes[*g.index_of("store")].rho, WithinRel(0.9, 1e-12));
  const auto per_unit = simsys::queueing::mmc_wait(20, 0.9, 0.040).mean_s;
  const auto per_object = simsys::queueing::mmc_wait(20, 0.9, 2.0).mean_s;
  REQUIRE_THAT(per_unit, WithinAbs(0.011015, 1e-6));
  REQUIRE_THAT(per_object, WithinAbs(0.550769, 1e-6));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(per_object / 2.0 + 0.040, 1e-9));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(0.315385, 1e-6));
}

// Book ch. 14's 300 MB upload, counted as a write: 1 upload/s at a default store is 300 MB/s,
// rho 0.6; s_obj = 300 MB x 40 ms = 12 s, so the M/M/c wait would be C(20, 0.6) x 12 s / (20 x 0.4)
// = 0.024134 x 1.5 s = 36.2 ms and M/D/c's is half, Wq = 18.1 ms; the upload is acknowledged at its
// last byte, 12 s + 18.1 ms = 12.0181 s.
TEST_CASE("a 300 MB upload is answered at its last byte, 12 s after its wait", "[theory]") {
  const auto g =
      *Graph::build(blob_store_chain("upload", 1, simsys::Bytes{300e6}, store_writing("upload")));
  const auto r = first_step(g);
  REQUIRE_THAT(r.nodes[*g.index_of("store")].rho, WithinRel(0.6, 1e-12));
  const auto wait = simsys::queueing::mmc_wait(20, 0.6, 12.0).mean_s / 2.0;  // M/D/c
  REQUIRE_THAT(wait, WithinAbs(0.018101, 1e-6));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(12.0 + wait, 1e-9));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(12.0181, 1e-4));
}

// The same upload's spread (second fix round, I2): the 12 s transfer is deterministic, so the
// response's variance is the wait's alone, (1/2)^2 x Var(Wq of M/M/c) (the Allen-Cunneen factor
// applied to the variance, the engine's heuristic) = 0.25 x P (2 - P) theta^2 with P = 0.024134
// and theta = 12 s / (20 x 0.4) = 1.5 s: 0.026824 s^2, a standard deviation of 0.16 s. The
// lognormal fit then puts the p99 at 12.40 s and the p1 at 11.64 s (below the 12 s no upload can
// beat: an artefact of the fit, whose left tail knows no floor): every upload takes more than
// 10 s, so youtube's `timeout: 10s` (plan Task 13) would time out all of them, not the ~42 % an
// exponential transfer (CV 1: variance 144 s^2 more, a p99 of 58.9 s) suggested. Task 16 sizes that
// timeout per the book (resumable uploads).
TEST_CASE("a 300 MB upload's p99 is 12.4 s and its p1 is above 10 s: the transfer is deterministic",
          "[theory]") {
  const auto g =
      *Graph::build(blob_store_chain("upload", 1, simsys::Bytes{300e6}, store_writing("upload")));
  const auto response = first_step(g).responses[0].response;
  const auto mmc = simsys::queueing::mmc_wait(20, 0.6, 12.0);
  REQUIRE_THAT(response.variance_s2, WithinRel(mmc.variance_s2 / 4.0, 1e-9));
  REQUIRE_THAT(response.variance_s2, WithinAbs(0.026824, 1e-6));
  using simsys::queueing::lognormal_quantile;
  using simsys::queueing::z_p99;
  REQUIRE_THAT(lognormal_quantile(response.mean_s, response.variance_s2, z_p99),
               WithinAbs(12.404, 1e-3));
  REQUIRE(lognormal_quantile(response.mean_s, response.variance_s2, -z_p99) > 10.0);
}

// A 2 KB read costs 2e3 B / 1e6 B per unit = 0.002 units: 1000 reads/s are 2 units/s, rho =
// 2 / 500 = 0.004; s_obj = 0.002 x 40 ms = 80 us and nothing waits (Erlang C(20, 0.004) = 4e-41;
// M/D/c halves it),
// and yet each read takes the whole 40 ms of one unit: the documented upper bound for small
// objects (a request under 1 MB still pays one unit's time). Without the bound a 2 KB read would
// be answered in 80 us.
TEST_CASE("a 2 KB read at a blob store costs 0.002 units and is answered in 40 ms", "[theory]") {
  const auto g = *Graph::build(blob_store_chain("read", 1000, simsys::Bytes{2e3}));
  const auto r = first_step(g);
  const auto& store = r.nodes[*g.index_of("store")];
  REQUIRE_THAT(store.demand_per_s, WithinRel(2.0, 1e-12));  // 1000/s x 0.002 units
  REQUIRE_THAT(store.rho, WithinRel(0.004, 1e-12));
  const auto wait = simsys::queueing::mmc_wait(20, 0.004, 80e-6).mean_s / 2.0;
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(wait + 0.040, 1e-9));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(0.040, 1e-9));
}

// The same bound for a small write: its last byte comes with its first megabyte's time, so a 2 KB
// write (the default write op, `write`) is answered in 40 ms, not in 2 KB x 40 ms per MB = 80 us.
TEST_CASE("a 2 KB write is answered in one megabyte's time, not in 80 us", "[theory]") {
  const auto g = *Graph::build(blob_store_chain("write", 1000, simsys::Bytes{2e3}));
  const auto r = first_step(g);
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(0.040, 1e-9));
}

// The size is the client's: the same store fed the same 5/s at 100 MB doubles demand to 500
// units/s (rho 1.0), where 50 MB gave 250. Sizes scale the load, never the number of requests.
TEST_CASE("doubling the request size doubles a byte-sized node's demand and keeps the request rate",
          "[theory]") {
  const auto g = *Graph::build(blob_store_chain("upload", 5, simsys::Bytes{100e6}));
  const auto r = first_step(g);
  REQUIRE_THAT(r.nodes[*g.index_of("store")].demand_per_s, WithinRel(500.0, 1e-12));
  REQUIRE_THAT(r.nodes[*g.index_of("store")].rho, WithinRel(1.0, 1e-12));
}

namespace {
// u sends 20 requests/s, half `write`s of 50 MB and half `read`s of 10 MB, into a default blob
// store (500 MB/s, 40 ms per MB, c = 20, a waiting room of 1000 MB). Zero-latency link. Node
// indices: u 0, store 1.
Scenario mixed_size_store() {
  Scenario s{.name = "blob-mixed", .version = 1, .duration = 60s};
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{20}},
                                           .op_shares = {{"read", 0.5}, {"write", 0.5}},
                                           .sizes = {{"read", simsys::Bytes{10e6}},
                                                     {"write", simsys::Bytes{50e6}}}}},
             NodeSpec{.id = "store", .type = NodeType::blob_store}};
  s.links = {LinkSpec{.from = "u", .to = "store", .latency = 0s}};
  return s;
}

// The client's sample of `op` in a step's responses.
const simsys::engine::ResponseSample& response_to(const simsys::engine::StepRecord& r,
                                                  std::string_view op) {
  const auto found = std::ranges::find(r.responses, op, &simsys::engine::ResponseSample::op);
  REQUIRE(found != r.responses.end());
  return *found;
}
}  // namespace

// Two sizes at one overloaded store: 10 writes/s x 50 MB + 10 reads/s x 10 MB = 600 MB/s against
// 500, an offered rho of 1.2. The 1000 MB waiting room fills in 1000 / (600 - 500) = 10 s; from
// then on the store drains 500 units/s and drops 100. The queue is well-mixed in UNITS (see
// engine::Simulation::serve), so each op loses the same share of its megabytes, 100 / 600 = 1/6,
// and so the same share of its requests: 10 x 5/6 = 8.333 of each op are answered per second,
// whatever their sizes. Units balance at every step (demand dt = (drain + dropped) dt + change in
// backlog). Both ops wait in the same queue; the write is answered at its last byte (50 MB x
// 40 ms = 2 s), the read at its first megabyte (40 ms), so the write's response is 1.96 s longer,
// and the node's own latency is their mixture by requests, half and half.
TEST_CASE("an overloaded blob store answers the same share of large and small requests",
          "[theory]") {
  const auto g = *Graph::build(mixed_size_store());
  auto sim = Simulation::create(g);
  if (!sim) FAIL(sim.error().message);
  const auto store_index = *g.index_of("store");
  simsys::engine::StepRecord last;
  double backlog = 0.0;
  for (int i = 0; i < 300; ++i) {  // 30 s of 100 ms steps
    last = sim->step(i * 100ms, 100ms);
    const auto& store = last.nodes[store_index];
    const double in = store.demand_per_s * 0.1;
    const double out = (store.drain_per_s + store.dropped_per_s) * 0.1 + (store.backlog - backlog);
    REQUIRE_THAT(out, WithinAbs(in, 1e-9 * in));
    backlog = store.backlog;
  }
  const auto& store = last.nodes[store_index];
  REQUIRE_THAT(store.demand_per_s, WithinRel(600.0, 1e-12));
  REQUIRE_THAT(store.drain_per_s, WithinRel(500.0, 1e-9));
  REQUIRE_THAT(store.dropped_per_s, WithinRel(100.0, 1e-6));
  const auto& writes = response_to(last, "write");
  const auto& reads = response_to(last, "read");
  REQUIRE_THAT(writes.answered.per_second, WithinRel(10.0 * 5.0 / 6.0, 1e-6));
  REQUIRE_THAT(reads.answered.per_second, WithinRel(10.0 * 5.0 / 6.0, 1e-6));
  REQUIRE_THAT(writes.response.mean_s - reads.response.mean_s, WithinAbs(1.96, 1e-9));
  REQUIRE_THAT(store.latency.mean_s,
               WithinRel((writes.response.mean_s + reads.response.mean_s) / 2.0, 1e-9));
}

// ---- The second fix round of Task 3 (review at 0248a47): the room bounds the wait (C1), the size
// mix's spread enters Kingman (I1) ----

namespace {
// The last step of `steps` 100 ms steps of a fresh simulation of `g`.
simsys::engine::StepRecord step_until(const Graph& g, int steps) {
  auto sim = Simulation::create(g);
  if (!sim) FAIL(sim.error().message);
  simsys::engine::StepRecord last;
  for (int i = 0; i < steps; ++i) last = sim->step(i * 100ms, 100ms);
  return last;
}
}  // namespace

// 12 writes/s of 50 MB are 600 MB/s against a default store's 500: past saturation. Erlang C at
// the 0.99 clamp with 2 s objects would add C(20, 0.99) x 2 s / (20 x 0.01) x 1/2 = 4.74 s (9.48 s
// with exponential service, what the first fix round reported: an 11.48 s response). But the
// store's waiting room holds 1000 MB and drains at 500 MB/s, so nobody admitted waits more than
// 1000 / 500 = 2 s (the room-drain bound); the full room's fluid wait is exactly that. A write is
// then answered at its last byte, 2 s later: 4 s. The excess, 100 MB/s, is dropped.
TEST_CASE("twelve 50 MB writes per second wait at most the 2 s their room drains in: about 4 s",
          "[theory]") {
  const auto g = *Graph::build(blob_store_chain("write", 12, simsys::Bytes{50e6}));
  const auto last = step_until(g, 300);  // 30 s: the room is full after 10 s
  const auto& store = last.nodes[*g.index_of("store")];
  REQUIRE_THAT(store.backlog, WithinRel(1000.0, 1e-9));
  REQUIRE_THAT(store.dropped_per_s, WithinRel(100.0, 1e-6));
  REQUIRE(last.responses[0].response.mean_s - 2.0 <= 2.0 + 1e-9);  // the wait
  REQUIRE_THAT(last.responses[0].response.mean_s, WithinRel(4.0, 1e-9));
}

// What the room-drain bound does NOT bound: the estimated percentiles (review of plan 2c Task 3's
// second fix round, I-1). The same twelve 50 MB writes per second over a run: every admitted write
// waits at most the room's 2 s and is answered 2 s after it, so none takes longer than 4 s. The
// report's percentiles are a lognormal fitted to the mean (4 s) and the variance (the wait's,
// capped at the room's scale, T^2 / 4 = 1 s^2), and a lognormal has no upper bound: its p99 is
// 6.88 s. A characterisation test: it pins what the model reports today, beyond the hard bound,
// for the comprehensive 2c review ("percentiles respect a hard bound") to change on purpose.
TEST_CASE("a saturated blob store's estimated p99 is 6.88 s, beyond its 4 s hard bound",
          "[theory]") {
  Scenario s = blob_store_chain("write", 12, simsys::Bytes{50e6});
  s.duration = 60s;
  const auto g = *Graph::build(s);
  simsys::report::Summary summary{g, s.warmup};
  (*Simulation::create(g)).run({.duration = s.duration, .step = s.step}, [&](const auto& r) {
    summary.observe(r);
  });
  const auto op = summary.ops().front();
  REQUIRE_THAT(op.mean_s, WithinAbs(4.0, 0.01));
  REQUIRE_THAT(op.p99_s, WithinAbs(6.88, 0.005));
  REQUIRE(op.p99_s > 2.0 + 2.0);  // the room's drain time plus the write's own 2 s
}

// The review's stop transient: 24 requests/s, half 50 MB writes and half 10 MB reads, overload a
// default store (720 MB/s against 500) for 20 s and then stop. From then on nothing arrives, and
// the store's latency is what a read arriving now would see: the wait plus one megabyte (40 ms).
// While the full room drains (1000 MB at 500 MB/s, 2 s), the wait is bounded by the room's drain
// time, 2 s, where the first fix round held it at the clamp's 5.7 s; once the room is empty nothing
// waits and the latency is the 40 ms of one megabyte. It never rises on the way down.
TEST_CASE(
    "after an overload stops a blob store's latency falls to one megabyte's time, never "
    "above the room's drain time",
    "[theory]") {
  Scenario s = blob_store_chain("write", 24, simsys::Bytes{50e6});
  auto& workload = *s.nodes[0].workload;
  workload.curve.points =
      simsys::workload::Points{.at = {{0s, Rate{24}}, {20s, Rate{24}}, {20s, Rate{0}}}};
  workload.op_shares = {{"write", 0.5}, {"read", 0.5}};
  workload.sizes = {{"write", simsys::Bytes{50e6}}, {"read", simsys::Bytes{10e6}}};
  const auto g = *Graph::build(s);
  auto sim = Simulation::create(g);
  if (!sim) FAIL(sim.error().message);
  const auto store = *g.index_of("store");
  for (int i = 0; i < 200; ++i) (void)sim->step(i * 100ms, 100ms);  // 20 s of overload
  double previous = std::numeric_limits<double>::infinity();
  double latency = 0.0;
  for (int i = 200; i < 250; ++i) {  // 5 s after the stop
    latency = sim->step(i * 100ms, 100ms).nodes[store].latency.mean_s;
    CAPTURE(i, latency);
    REQUIRE(latency <= previous + 1e-12);
    REQUIRE(latency <= 2.0 + 0.040 + 1e-9);
    previous = latency;
  }
  REQUIRE_THAT(latency, WithinAbs(0.040, 1e-9));
}

namespace {
// The review's probe (mix.yaml): 1008 requests/s into a default blob store, 99.21 % of them 2 KB
// reads (1000/s) and 0.79 % 50 MB writes (7.9632/s). `variability` is the store's
// advanced.service_variability, the per-object c_s^2, when given (default 0: a deterministic
// transfer).
Scenario small_reads_beside_large_writes(std::optional<double> variability = {}) {
  Scenario s = blob_store_chain("write", 1008, simsys::Bytes{50e6},
                                NodeSpec{.id = "store",
                                         .type = NodeType::blob_store,
                                         .advanced = {.service_variability = variability}});
  auto& workload = *s.nodes[0].workload;
  workload.op_shares = {{"read", 0.9921}, {"write", 0.0079}};
  workload.sizes = {{"read", simsys::Bytes{2e3}}, {"write", simsys::Bytes{50e6}}};
  return s;
}

// What a 50 MB write waits: its response less its 2 s transfer (zero-latency link).
double write_wait(const Graph& g) {
  return response_to(first_step(g), "write").response.mean_s - 2.0;
}
}  // namespace

// The size mix's spread (second fix round, I1). The writes alone, 7.9632/s of 50 MB, are rho
// 0.79632 of a default store and, every one holding a slot the same 2 s (c_s^2 = 0, M/D/c), wait
//   (1 + 0) / 2 x C(20, 0.79632) x 2 s / (20 x 0.20368) = 60.9 ms.
// Add 1000 reads/s of 2 KB (80 us of a slot each): rho rises to 0.80032. A mean object time alone
// (E[S] = 0.9921 x 80 us + 0.0079 x 2 s = 15.88 ms) would cut the writes' wait to 1.02 ms (what the
// first fix round did: the reads look like they make the store faster). The mixture's SCV says
// how spread those times are:
//   E[S^2] = 0.9921 x (80 us)^2 + 0.0079 x (2 s)^2 = 0.0316 s^2   (per-object c_s^2 = 0)
//   c_s^2  = E[S^2] / E[S]^2 - 1 = 0.0316 / 0.01588^2 - 1 = 124.3
//   Wq     = (1 + 124.3) / 2 x C(20, 0.80032) x 15.88 ms / (20 x 0.19968) = 62.66 x 1.021 ms
//          = 64.0 ms
// more than the writes alone: adding load never shortens the wait.
TEST_CASE("small reads beside large writes lengthen the writes' wait: 64.0 ms against 60.9 ms",
          "[theory]") {
  const double alone =
      write_wait(*Graph::build(blob_store_chain("write", 7.9632, simsys::Bytes{50e6})));
  const double mixed = write_wait(*Graph::build(small_reads_beside_large_writes()));
  REQUIRE_THAT(alone, WithinAbs(0.060868, 1e-6));
  REQUIRE_THAT(mixed, WithinAbs(0.063980, 1e-6));
  REQUIRE(mixed > alone);
}

// advanced.service_variability still overrides the per-object c_s^2: at 1 (each transfer
// exponential, M/M/c's assumption) E[S^2] doubles, c_s^2 = 2 x 125.32 - 1 = 249.6, and the writes
// wait (1 + 249.6) / 2 x 1.021 ms = 128.0 ms, the review's ~127 ms; alone they wait the M/M/c
// 121.7 ms.
TEST_CASE("a blob store with service_variability 1 waits as exponential transfers: 128.0 ms",
          "[theory]") {
  const double mixed = write_wait(*Graph::build(small_reads_beside_large_writes(1.0)));
  REQUIRE_THAT(mixed, WithinAbs(0.127960, 1e-6));
  const double alone = write_wait(*Graph::build(blob_store_chain(
      "write", 7.9632, simsys::Bytes{50e6},
      NodeSpec{
          .id = "store", .type = NodeType::blob_store, .advanced = {.service_variability = 1.0}})));
  REQUIRE_THAT(alone, WithinAbs(0.121736, 1e-6));
  REQUIRE(mixed > alone);
}

namespace {
// u (`streams`/s of `stream`, each 50 MB) -> cdn_us (byte capacity `bytes`, every stream a hit so
// the origin sees nothing: fixed hit ratio 1) -> origin (a service). Zero-latency links. Node
// indices: u 0, cdn_us 1, origin 2.
Scenario byte_cdn_chain(double streams, simsys::ByteRate bytes) {
  Scenario s{.name = "byte-cdn", .version = 1, .duration = 10s};
  NodeSpec cdn{.id = "cdn_us",
               .type = NodeType::cdn,
               .byte_capacity = bytes,
               .hit_ratio = HitRatio{.fixed = 1.0}};
  cdn.origin = "origin";
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{streams}},
                                           .op_shares = {{"stream", 1.0}},
                                           .sizes = {{"stream", simsys::Bytes{50e6}}}}},
             std::move(cdn),
             NodeSpec{.id = "origin", .type = NodeType::service, .capacity = Rate{1000}}};
  s.links = {LinkSpec{.from = "u", .to = "cdn_us", .latency = 0s},
             LinkSpec{.from = "cdn_us", .to = "origin", .latency = 0s}};
  return s;
}
}  // namespace

// A CDN sized in bytes per second (`capacity: 100GB/s`) is a byte-sized node (plan 2c refinement
// 2): it counts megabytes, every op that reaches it has a size (Graph::request_bytes), and the
// engine runs it. The numbers it runs on are pinned by the byte-sized tests below. Only the che
// hit ratio without `keys` (next test) still keeps a CDN from running. The node is called cdn_us
// as in book/youtube; its streams are sized as there.
TEST_CASE("a cdn with a byte capacity builds and the engine runs it", "[theory]") {
  const auto g = Graph::build(byte_cdn_chain(1000, simsys::ByteRate{100e9}));
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  const auto sim = Simulation::create(*g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
}

// A 100 GB/s CDN is 100e9 / 1e6 = 100k units/s, and its 10 ms per MB (book: "read 1 MB
// sequentially from the network") make c = 100k/s x 10 ms = 1000 transfers in flight. 1k streams/s
// of 50 MB are 50k units/s: rho = 50k / 100k = 0.5. A stream holds a slot for its whole transfer,
// s_obj = 50 MB x 10 ms = 0.5 s, and Erlang C(1000, 0.5) = 3e-86, so Wq = 3e-86 x 0.5 s / 500:
// nothing waits. A stream is a read, answered at its first megabyte: 10 ms.
TEST_CASE("a 100 GB per second cdn at 1k streams of 50 MB per second is at rho 0.5", "[theory]") {
  const auto g = *Graph::build(byte_cdn_chain(1000, simsys::ByteRate{100e9}));
  const auto r = first_step(g);
  const auto& cdn = r.nodes[*g.index_of("cdn_us")];
  REQUIRE_THAT(cdn.demand_per_s, WithinRel(50'000.0, 1e-12));
  REQUIRE_THAT(cdn.rho, WithinRel(0.5, 1e-12));
  REQUIRE(cdn.dropped_per_s == 0.0);
  const auto wait = simsys::queueing::mmc_wait(1000, 0.5, 0.5).mean_s;
  REQUIRE_THAT(cdn.latency.mean_s, WithinRel(wait + 0.010, 1e-9));
  REQUIRE_THAT(cdn.latency.mean_s, WithinAbs(0.010, 1e-9));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(0.010, 1e-9));
}

// 2k streams/s x 50 MB = 100 GB/s: the cdn's whole capacity, 100k units/s, rho = 1.0, the
// saturation point of the napkin (100e9 / 50e6 = 2k streams/s). Beyond it the node sheds: at
// 2.2k streams/s demand is 110k units/s against a capacity of 100k (an offered rho of 1.1); the
// node drains 100k and, once its 1000-unit queue (the generic default) is full, drops the excess,
// 110k - 100k = 10k units/s, at every step (per step: demand dt = (drain + dropped) dt + change in
// backlog).
TEST_CASE("a 100 GB per second cdn saturates at 2k streams of 50 MB per second", "[theory]") {
  SECTION("2k streams per second is exactly full") {
    const auto g = *Graph::build(byte_cdn_chain(2000, simsys::ByteRate{100e9}));
    const auto r = first_step(g);
    const auto& cdn = r.nodes[*g.index_of("cdn_us")];
    REQUIRE_THAT(cdn.demand_per_s, WithinRel(100'000.0, 1e-12));
    REQUIRE_THAT(cdn.rho, WithinRel(1.0, 1e-12));
  }
  SECTION("2.2k streams per second drops the excess once the queue is full") {
    const auto g = *Graph::build(byte_cdn_chain(2200, simsys::ByteRate{100e9}));
    auto sim = Simulation::create(g);
    if (!sim) FAIL(sim.error().message);
    simsys::engine::StepRecord last;
    for (int i = 0; i < 20; ++i) last = sim->step(i * 100ms, 100ms);
    const auto& cdn = last.nodes[*g.index_of("cdn_us")];
    REQUIRE_THAT(cdn.demand_per_s, WithinRel(110'000.0, 1e-12));  // 2.2k/s x 50 MB
    REQUIRE_THAT(cdn.drain_per_s, WithinRel(100'000.0, 1e-9));    // the whole capacity, no more
    REQUIRE_THAT(cdn.dropped_per_s, WithinRel(10'000.0, 1e-6));
  }
}

// ---- Which byte-sized nodes the engine runs ----

namespace {
// u -> store (a blob store, 50 MB uploads at 1/s) -> svc, with the store's `then` step notifying
// svc: the shape Task 4 turns into an asynchronous event. Node indices: u 0, store 1, svc 2.
Scenario blob_store_with_event() {
  auto s = blob_store_chain("upload", 1, simsys::Bytes{50e6},
                            NodeSpec{.id = "store",
                                     .type = NodeType::blob_store,
                                     .then = {Step{.to = "svc", .op = "notify"}}});
  s.nodes.push_back(NodeSpec{.id = "svc", .type = NodeType::service});
  s.links.push_back(LinkSpec{.from = "store", .to = "svc", .latency = 0s});
  return s;
}
}  // namespace

TEST_CASE("the engine runs a blob store that has no then steps", "[theory]") {
  const auto g = Graph::build(blob_store_chain("upload", 5, simsys::Bytes{50e6}));
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  const auto sim = Simulation::create(*g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
}

TEST_CASE("the engine runs a blob store that has then steps", "[theory]") {
  const auto g = Graph::build(blob_store_with_event());
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  const auto sim = Simulation::create(*g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
}

// ---- A blob store's then steps are asynchronous event notifications (plan 2c refinements 6-7)
// ----
//
// S3 event notifications (book ch. 14: "transcoding starts when the upload completes"): for every
// WRITE the store SERVED (an op in its `write_ops`, default `[write]`: S3's ObjectCreated), each
// `then` step sends its op to its target, and nobody waits for it. A read fires nothing: S3 has
// no event for a GET. So the uploader's response and answered rate end at the store, and the
// target only receives load.
//
// The numbers below come from one picture: a client sends 2 uploads/s of 100 MB to a default blob
// store (spec section 6: 500 MB/s, 40 ms per MB), over a 250 us link (0.5 ms round trip):
//   demand   = 2/s x 100 MB = 200 units (MB) per second against 500: rho = 200 / 500 = 0.4;
//   c        = 500 units/s x 40 ms = 20 slots (Little's law);
//   s_obj    = 100 MB x 40 ms = 4 s, the time one upload holds a slot;
//   Erlang C(20, 0.4) = 0.00026495 (queueing::mmc_wait takes rho per server), so the M/M/c wait
//   would be C x s_obj / (c (1 - rho)) = 0.00026495 x 4 s / (20 x 0.6) = 0.0883 ms, and every
//   upload holding a slot the same 4 s (a deterministic transfer, M/D/c) halves it:
//   Wq       = (1 + 0) / 2 x 0.0883 ms = 0.0442 ms.
// Listed in `write_ops` (as youtube's original_store lists `upload`), the upload is a write,
// answered at its last byte and notifying svc: Wq + 4 s + 0.5 ms = 4.000544 s.
// An op that is not in the store's `write_ops` is a read, answered at its first megabyte and
// notifying nobody: response = Wq + 40 ms + 0.5 ms = 40.544 ms.
// Either way `svc`, which takes 500 ms per notification, adds nothing to the client's response.

namespace {
constexpr double upload_mmc_wait_s = 8.8316e-5;  // M/M/c's wait above, to the derivation's digits
// M/D/c's wait, the store's: half of M/M/c's (the Allen-Cunneen factor (1 + 0) / 2).
[[nodiscard]] double upload_wait_s() {
  return simsys::queueing::mmc_wait(20, 0.4, 4.0).mean_s / 2.0;
}

// The service the store notifies: slow to the client's eyes if it counted (500 ms), so that a
// response that waited for it would be unmistakable.
NodeSpec notified_service() {
  return NodeSpec{.id = "svc", .type = NodeType::service, .capacity = Rate{1000}, .latency = 500ms};
}

// The store's `write_ops` in most tests below: the upload is a write, so it fires the events.
const std::optional<std::vector<std::string>> upload_is_a_write =
    std::vector<std::string>{"upload"};

// u (`rate` uploads/s of 100 MB, 250 us link) -> store (a default blob store) -> svc over a 1 ms
// link, the store's `then` being `then`. `write_ops` is the store's (std::nullopt: the default
// `[write]`, so the upload is a read). Node indices: u 0, store 1, svc 2.
Scenario upload_with_events(std::vector<Step> then,
                            std::optional<std::vector<std::string>> write_ops = upload_is_a_write,
                            NodeSpec svc = notified_service(), double rate = 2.0) {
  Scenario s{.name = "blob-events", .version = 1, .duration = 10s};
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{rate}},
                                           .op_shares = {{"upload", 1.0}},
                                           .sizes = {{"upload", simsys::Bytes{100e6}}}}},
             NodeSpec{.id = "store",
                      .type = NodeType::blob_store,
                      .then = std::move(then),
                      .write_ops = std::move(write_ops)},
             std::move(svc)};
  s.links = {LinkSpec{.from = "u", .to = "store", .latency = 250us},
             LinkSpec{.from = "store", .to = "svc", .latency = 1ms}};
  return s;
}

const std::vector<Step> notify_step{Step{.to = "svc", .op = "notify"}};
}  // namespace

TEST_CASE("a blob store sends its then target one notification per upload it served", "[theory]") {
  const auto g = *Graph::build(upload_with_events(notify_step));
  const auto r = first_step(g);
  const auto& store = r.nodes[*g.index_of("store")];
  REQUIRE_THAT(store.demand_per_s, WithinRel(200.0, 1e-12));  // units (MB) per second
  REQUIRE_THAT(store.rho, WithinRel(0.4, 1e-12));
  REQUIRE_THAT(simsys::queueing::mmc_wait(20, 0.4, 4.0).mean_s, WithinAbs(upload_mmc_wait_s, 1e-9));
  // 2 uploads/s served, 2 notify/s sent: svc counts requests, not megabytes.
  REQUIRE_THAT(r.nodes[*g.index_of("svc")].demand_per_s, WithinRel(2.0, 1e-12));
}

// The upload is a read to this store (`write_ops` does not list it): answered at its first
// megabyte, 0.0442 ms + 40 ms after the wait, plus the 0.5 ms round trip: 40.544 ms. A read fires
// no event (S3 has none for a GET), so svc is sent nothing.
TEST_CASE("a read-style upload at a blob store does not notify and is answered in 40.54 ms",
          "[theory]") {
  const auto g = *Graph::build(upload_with_events(notify_step, std::nullopt));
  const auto r = first_step(g);
  const double wait = upload_wait_s();
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(wait + 0.040 + 0.0005, 1e-9));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(0.040544, 1e-6));
  // The store's own sojourn is the response minus the link round trip.
  REQUIRE_THAT(r.nodes[*g.index_of("store")].latency.mean_s, WithinRel(wait + 0.040, 1e-9));
  REQUIRE(r.nodes[*g.index_of("svc")].demand_per_s == 0.0);
}

// The same upload counted as a write is answered at its last byte, 4 s + Wq + 0.5 ms, and notifies
// svc 2/s. The 500 ms svc is not in the response: the hop to svc is asynchronous.
TEST_CASE("a write-listed upload notifies and is answered at its last byte, svc not included",
          "[theory]") {
  const auto g = *Graph::build(upload_with_events(notify_step));
  const auto r = first_step(g);
  const double wait = upload_wait_s();
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(wait + 4.0 + 0.0005, 1e-9));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(4.000544, 1e-6));
  REQUIRE_THAT(r.nodes[*g.index_of("svc")].demand_per_s, WithinRel(2.0, 1e-12));
}

// The reviewer's probe of Task 4 (I1): 1000 viewers/s read 1 MB videos from the store and one
// uploader/s writes a 100 MB one; the store's `then` asks a transcoder for one job per event. The
// transcoder must see 1 job/s, the uploads, not 1001/s: S3 notifies on ObjectCreated, never on a
// GET. (With reads included, the transcoder of 10 jobs/s was offered 1001/s, 100x its capacity.)
TEST_CASE("1000 reads/s and 1 upload/s at a blob store send its transcoder 1 job/s", "[theory]") {
  Scenario s{.name = "views-and-uploads", .version = 1, .duration = 10s};
  s.nodes = {
      NodeSpec{.id = "viewers",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{1000}},
                                    .op_shares = {{"get", 1.0}},
                                    .sizes = {{"get", simsys::Bytes{1e6}}}}},
      NodeSpec{.id = "uploader",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{1}},
                                    .op_shares = {{"upload", 1.0}},
                                    .sizes = {{"upload", simsys::Bytes{100e6}}}}},
      NodeSpec{.id = "store",
               .type = NodeType::blob_store,
               .byte_capacity = simsys::ByteRate{5e9},
               .then = {Step{.to = "transcode", .op = "job"}},
               .write_ops = std::vector<std::string>{"upload"}},
      NodeSpec{.id = "transcode", .type = NodeType::service, .capacity = Rate{10}, .latency = 1s}};
  s.links = {LinkSpec{.from = "viewers", .to = "store"},
             LinkSpec{.from = "uploader", .to = "store"},
             LinkSpec{.from = "store", .to = "transcode"}};
  const auto g = *Graph::build(s);
  const auto sim = Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
  const auto e = sim->estimate();
  REQUIRE_THAT(e.nodes[*g.index_of("transcode")].demand_per_s, WithinRel(1.0, 1e-9));
}

// svc is a 1/s service with a waiting room of 1, offered 2 notify/s: in the long run it drains
// 1/s and drops the other 1/s, a drop rate of 1 / 2 = 0.5. The client never hears of it: its
// uploads are answered at the store, 2/s of 2/s, at the same 4.000544 s.
TEST_CASE(
    "an undersized notified service drops half its notifications and the uploader still "
    "gets every answer",
    "[theory]") {
  const auto undersized = NodeSpec{.id = "svc",
                                   .type = NodeType::service,
                                   .capacity = Rate{1},
                                   .latency = 10ms,
                                   .advanced = {.queue_limit = 1}};
  const auto g = *Graph::build(upload_with_events(notify_step, upload_is_a_write, undersized));
  const auto sim = Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
  const auto e = sim->estimate();
  const auto& svc = e.nodes[*g.index_of("svc")];
  REQUIRE_THAT(svc.demand_per_s, WithinRel(2.0, 1e-9));
  REQUIRE_THAT(svc.dropped_per_s, WithinRel(1.0, 1e-6));
  REQUIRE_THAT(svc.dropped_per_s / svc.demand_per_s, WithinRel(0.5, 1e-6));
  REQUIRE(e.nodes[*g.index_of("store")].dropped_per_s == 0.0);
  REQUIRE_THAT(e.responses[0].answered.per_second, WithinRel(2.0, 1e-9));
  const double wait = upload_wait_s();
  REQUIRE_THAT(e.responses[0].response.mean_s, WithinRel(wait + 4.0 + 0.0005, 1e-9));
}

// `factor` multiplies a notification: one upload, four events (say four renditions to make).
TEST_CASE("a notification's factor multiplies the notifications sent: 4 per upload is 8 per second",
          "[theory]") {
  const auto g =
      *Graph::build(upload_with_events({Step{.to = "svc", .op = "notify", .factor = 4.0}}));
  const auto r = first_step(g);
  REQUIRE_THAT(r.nodes[*g.index_of("svc")].demand_per_s, WithinRel(8.0, 1e-12));
  // The store is untouched by what it sends: same rho, same answer.
  REQUIRE_THAT(r.nodes[*g.index_of("store")].rho, WithinRel(0.4, 1e-12));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(4.000544, 1e-6));
}

// The size walk (plan 2c refinement 1) is how the graph resolves it, and the engine must accept
// all three shapes: a step that keeps the op name carries the upload's 100 MB (1e8 B), a renamed
// step is unsized unless it states `size:`. svc counts requests, so it takes an unsized notify.
TEST_CASE(
    "a notification carries the upload's size when the op keeps its name and none when "
    "renamed",
    "[theory]") {
  SECTION("same op: the incoming 100 MB") {
    const auto g = *Graph::build(upload_with_events({Step{.to = "svc", .op = "upload"}}));
    REQUIRE(Simulation::create(g).has_value());
    REQUIRE_THAT(g.request_bytes(*g.index_of("svc"), "upload")->count, WithinRel(1e8, 1e-12));
  }
  SECTION("renamed, without size: unsized") {
    const auto g = *Graph::build(upload_with_events(notify_step));
    REQUIRE(Simulation::create(g).has_value());
    REQUIRE_FALSE(g.request_bytes(*g.index_of("svc"), "notify").has_value());
  }
  SECTION("renamed, with size: the step's own 1 KB") {
    const auto g = *Graph::build(
        upload_with_events({Step{.to = "svc", .op = "notify", .size = simsys::Bytes{1e3}}}));
    REQUIRE(Simulation::create(g).has_value());
    REQUIRE_THAT(g.request_bytes(*g.index_of("svc"), "notify")->count, WithinRel(1e3, 1e-12));
  }
}

// 10 uploads/s of 100 MB are 1000 MB/s against 500: the store serves half, 5 uploads/s, and the
// other half is dropped. A notification fires for a request the store SERVED, so svc is sent 5/s,
// not the 10/s offered.
TEST_CASE("an overloaded blob store notifies only for the uploads it served", "[theory]") {
  const auto g =
      *Graph::build(upload_with_events(notify_step, upload_is_a_write, notified_service(), 10.0));
  const auto sim = Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
  const auto e = sim->estimate();
  const auto& store = e.nodes[*g.index_of("store")];
  REQUIRE_THAT(store.demand_per_s, WithinRel(1000.0, 1e-12));
  REQUIRE_THAT(store.drain_per_s, WithinRel(500.0, 1e-9));  // 5 uploads/s of 100 MB
  REQUIRE_THAT(e.nodes[*g.index_of("svc")].demand_per_s, WithinRel(5.0, 1e-9));
}

// estimate is the long-run state of run: constant rates, so the last step of a run equals it.
TEST_CASE("estimate and run agree on a blob store with a notification", "[theory]") {
  const auto g =
      *Graph::build(upload_with_events({Step{.to = "svc", .op = "notify", .factor = 4.0}}));
  const auto sim = Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
  auto running = *Simulation::create(g);
  simsys::engine::StepRecord last;
  running.run({.duration = 2s, .step = 100ms},
              [&](const simsys::engine::StepRecord& r) { last = r; });
  const auto e = sim->estimate();
  for (const auto* id : {"store", "svc"}) {
    CAPTURE(id);
    const auto node = *g.index_of(id);
    REQUIRE_THAT(last.nodes[node].rho, WithinRel(e.nodes[node].rho, 1e-9));
    REQUIRE_THAT(last.nodes[node].demand_per_s, WithinRel(e.nodes[node].demand_per_s, 1e-9));
  }
  REQUIRE_THAT(e.nodes[*g.index_of("svc")].demand_per_s, WithinRel(8.0, 1e-9));
  REQUIRE_THAT(last.responses[0].response.mean_s, WithinRel(e.responses[0].response.mean_s, 1e-9));
  REQUIRE_THAT(last.responses[0].answered.per_second,
               WithinRel(e.responses[0].answered.per_second, 1e-9));
}

// ---- ID generator as service sugar (plan 2b refinement 6; book ch. 7) ----
//
// An id_generator runs exactly as a service whose capacity and service time the algorithm
// supplies (Graph::build). So the numbers below are a service's: the fluid drain, and the M/M/c
// wait of Erlang C (queueing::mmc_wait: the second argument is the per-server utilization rho).

namespace {
// u (`demand`/s of `next_id`) -> ids (an id_generator): zero-latency link, so the client's
// response time is the generator's own sojourn. Capacity, latency and queue limit only when given.
// Node indices: u 0, ids 1.
Scenario id_chain(simsys::model::IdAlgorithm algorithm, double demand, int replicas = 1,
                  std::optional<Rate> capacity = {}, std::optional<simsys::SimTime> latency = {},
                  std::optional<double> queue_limit = {}) {
  Scenario s{.name = "id-chain", .version = 1, .duration = 10s, .warmup = 2s};
  NodeSpec ids{.id = "ids",
               .type = NodeType::id_generator,
               .replicas = replicas,
               .capacity = capacity,
               .latency = latency,
               .advanced = {.queue_limit = queue_limit}};
  ids.id_algorithm = algorithm;
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{demand}},
                                           .op_shares = {{"next_id", 1.0}}}},
             std::move(ids)};
  s.links = {LinkSpec{.from = "u", .to = "ids", .dir = Direction::inout, .latency = 0s}};
  return s;
}
}  // namespace

// 10k ids/s on one snowflake machine, no numbers stated:
//   capacity 4096 ids/ms x 1000 = 4.096M/s, service time 10 us, so rho = 10k / 4.096M = 0.00244
//   and c = ceil(4.096M/s x 10 us) = ceil(40.96) = 41 servers (Little's law, defaults).
// Erlang C at 41 servers and rho = 0.00244 is P(wait) of the order (c rho)^c / c! = 0.1^41 / 41!
// ~ 3e-91: nobody
// ever waits. The sojourn is the service time, 10 us; the wait is nothing (NOT 10 us as the plan
// text puts it: that would take a rho near 0.5).
TEST_CASE("a snowflake at 10k ids per second is almost idle and answers in its 10 us", "[theory]") {
  const auto g = *Graph::build(id_chain(IdAlgorithm::snowflake, 10'000));
  const auto sim = Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
  const auto r = (*Simulation::create(g)).step(0s, 100ms);
  const auto& ids = r.nodes[*g.index_of("ids")];
  const double rho = 10'000.0 / 4'096'000.0;
  REQUIRE_THAT(ids.rho, WithinRel(rho, 1e-9));
  const double sojourn = simsys::queueing::mmc_wait(41, rho, 10e-6).mean_s + 10e-6;
  REQUIRE_THAT(ids.latency.mean_s, WithinRel(sojourn, 1e-9));
  REQUIRE_THAT(ids.latency.mean_s, WithinRel(10e-6, 1e-6));  // the napkin number: no queue
  REQUIRE(ids.dropped_per_s == 0.0);
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(sojourn, 1e-9));  // link latency is 0
}

// "Sugar": the generator is a service with the algorithm's numbers, nothing more. The same
// scenario written with a `service` of 4.096M/s and 10 us gives identical samples.
TEST_CASE("an id generator steps exactly as the service its algorithm stands for", "[theory]") {
  const auto as_generator = *Graph::build(id_chain(IdAlgorithm::snowflake, 10'000));
  auto as_service_scenario = id_chain(IdAlgorithm::snowflake, 10'000);
  as_service_scenario.nodes[1] = NodeSpec{
      .id = "ids", .type = NodeType::service, .capacity = Rate{4'096'000}, .latency = 10us};
  const auto as_service = *Graph::build(std::move(as_service_scenario));
  const auto a = (*Simulation::create(as_generator)).step(0s, 100ms);
  const auto b = (*Simulation::create(as_service)).step(0s, 100ms);
  REQUIRE(a.nodes[1].rho == b.nodes[1].rho);
  REQUIRE(a.nodes[1].latency.mean_s == b.nodes[1].latency.mean_s);
  REQUIRE(a.nodes[1].drain_per_s == b.nodes[1].drain_per_s);
  REQUIRE(a.responses[0].response.mean_s == b.responses[0].response.mean_s);
}

// More replicas share the ids: 3 snowflake machines at 12.288M/s total take 10k/s at
// rho = 10k / (3 x 4.096M) = 8.14e-4 each (the capacity is per replica).
TEST_CASE("a snowflake's replicas split the load: rho is demand over replicas times capacity",
          "[theory]") {
  const auto g = *Graph::build(id_chain(IdAlgorithm::snowflake, 10'000, 3));
  const auto r = (*Simulation::create(g)).step(0s, 100ms);
  REQUIRE_THAT(r.nodes[*g.index_of("ids")].rho, WithinRel(10'000.0 / (3 * 4'096'000.0), 1e-9));
}

// The lab's ticket server: one database of 8k/s (queue limit 1000) offered 10k ids/s.
//   offered 10k/s, capacity 8k/s: the queue fills (1000 / (10k - 8k) = 0.5 s), then in steady
//   state the node serves 8k/s and drops the rest:
//     dropped = 10k - 8k = 2k/s, drop rate = 2k / 10k = 0.2, rho = 1 (saturated).
// The client is answered only what was served: 8k/s.
TEST_CASE("a ticket server offered more than its capacity drops the excess: 2k of 10k ids a second",
          "[theory]") {
  const auto s = id_chain(IdAlgorithm::ticket_server, 10'000, 1, Rate{8000}, 1ms, 1000.0);
  const auto g = *Graph::build(s);
  const auto sim = Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
  const auto e = (*Simulation::create(g)).estimate();
  const auto& ids = e.nodes[*g.index_of("ids")];
  REQUIRE_THAT(ids.drain_per_s, WithinRel(8000.0, 1e-6));
  REQUIRE_THAT(ids.dropped_per_s, WithinRel(2000.0, 1e-6));
  REQUIRE_THAT(ids.rho, WithinRel(1.0, 1e-9));
  REQUIRE_THAT(e.responses[0].answered.per_second, WithinRel(8000.0, 1e-6));
  // The run agrees once the queue has filled (0.5 s, inside the 2 s warmup): the node's drop
  // rate, Σ dropped / Σ demand, is 0.2.
  simsys::report::Summary summary{g, s.warmup};
  (*Simulation::create(g)).run({.duration = s.duration, .step = s.step}, [&](const auto& r) {
    summary.observe(r);
  });
  REQUIRE_THAT(summary.drop_rate("ids"), WithinRel(0.2, 1e-6));
}

// A ticket server with no numbers stated is one SQL box: 1k writes/s (spec §6). 1500 ids/s
// overflow it by 500/s, a third of the load.
TEST_CASE("a ticket server with the algorithm's default capacity saturates at 1k ids a second",
          "[theory]") {
  const auto g = *Graph::build(id_chain(IdAlgorithm::ticket_server, 1500));
  const auto e = (*Simulation::create(g)).estimate();
  REQUIRE_THAT(e.nodes[*g.index_of("ids")].drain_per_s, WithinRel(1000.0, 1e-6));
  REQUIRE_THAT(e.nodes[*g.index_of("ids")].dropped_per_s, WithinRel(500.0, 1e-6));
}

// ---- A link's bandwidth is a pipe that queues and drops megabytes (plan 2c refinement 3, Task 5)
// ----
//
// A link that states `bandwidth` is a pipe (spec section 3.4: it "caps bytes/s and queues the
// excess like a node"): ONE server (c = 1) whose unit is 1 MB, served in 1 MB / bandwidth. At
// 1 Gbit/s = 125 MB/s that is 8 ms per MB, deterministic (service SCV 0 per object; the SCV of
// the size mix when several ops share the pipe, as at a byte-sized node), with a waiting room of
// 1000 MB (the generic queue_limit default read in megabytes) beyond which it drops. The pipe is
// fed by the DEMAND that crosses the link: each request is charged its whole stated size in the
// direction it is sent, whichever way its bytes really travel, and a reply adds no bytes (so an
// `inout` link is one pipe, and `a->b` and `b->a` with a bandwidth each are two).
//
// What a hop over the pipe adds to the round trip, once (review of Task 5, I-2): the pipe's WAIT
// plus the request's answer time on the wire, by refinement 2's rule for the node behind it:
//  - a WRITE (an op in the receiver's write_ops; a service's is `write`) its WHOLE serialisation,
//    size x 1 MB / bandwidth: the receiver acts after the last byte;
//  - any other op its FIRST megabyte, min(size, 1 MB) x 1 MB / bandwidth: the rest streams in
//    while the receiver answers at its first megabyte.
// For a 1 MB request both are 8 ms. The pipe's queue waits for whole requests either way (each
// holds the one server for its whole serialisation). The pipe's losses multiply the hop's
// delivered fraction.
//
// Interfaces these tests propose (the implementer may adjust the shape, not the numbers):
//  - `engine::StepRecord::links`: a std::vector<engine::LinkSample>, ONE entry per link that has
//    a bandwidth, in Graph::links() order (empty for a graph without any bandwidth);
//  - `engine::LinkSample{demand_per_s, drain_per_s, dropped_per_s, backlog, rho, latency, zone}`,
//    as NodeSample's fields of those names, in MEGABYTES (per second for the rates); `latency` is
//    the request-weighted mean of what a request waits in the pipe plus its answer time on the
//    wire (above).
//
// Every number below is derived on paper from one picture: client `a` sends `get`s of 1 MB at
// 100/s to service `b` (capacity 1M/s, 1 ms: no queue of its own) over a one-way 250 us link of
// 1 Gbit/s:
//   demand      = 100/s x 1 MB = 100 MB/s;  capacity = 125 MB/s (1e9 bit/s / 8 bit per byte)
//   rho         = 100 / 125 = 0.8                              (yellow: 0.70 <= rho < 0.90)
//   s_MB        = 1 MB / 125 MB/s = 8 ms;  a 1 MB object holds the pipe s_obj = 8 ms
//   Wq (M/D/1)  = (1 + 0) / 2 x rho / (1 - rho) x s_obj = 0.5 x 0.8 / 0.2 x 8 ms = 16 ms
//                 (Allen-Cunneen / Kingman with Poisson arrivals, c_a^2 = 1, and a deterministic
//                 service, c_s^2 = 0; for one server it is exactly Pollaczek-Khinchine's
//                 Wq = lambda E[S^2] / (2 (1 - rho)) = 100 x (8 ms)^2 / 0.4 = 16 ms)
//   sojourn     = Wq + s_obj = 16 ms + 8 ms = 24 ms
//   round trip  = 2 x 250 us + 24 ms + b's own sojourn (1 ms, plus a wait of ~0 at rho 1e-4)

namespace {
constexpr simsys::ByteRate one_gbit{125e6};  // 1 Gbit/s = 1e9 / 8 bytes per second

// a(`rate`/s of `get`, each `size` long: a read at b) -> b (1M/s, 1 ms) over a 250 us link with the
// given bandwidth, in the given direction. Node indices: a 0, b 1. Steps of 100 ms (the default).
Scenario pipe_over(double rate, simsys::Bytes size,
                   std::optional<simsys::ByteRate> bandwidth = one_gbit,
                   Direction dir = Direction::out) {
  Scenario s{.name = "pipe", .version = 1, .duration = 60s};
  s.nodes = {NodeSpec{.id = "a",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{rate}},
                                           .op_shares = {{"get", 1.0}},
                                           .sizes = {{"get", size}}}},
             NodeSpec{.id = "b", .type = NodeType::service, .capacity = Rate{1e6}, .latency = 1ms}};
  s.links = {
      LinkSpec{.from = "a", .to = "b", .dir = dir, .latency = 250us, .bandwidth = bandwidth}};
  return s;
}

// The one pipe's sample of a step: the graphs here have exactly one link with a bandwidth.
const simsys::engine::LinkSample& only_link(const simsys::engine::StepRecord& r) {
  REQUIRE(r.links.size() == 1);
  return r.links.front();
}

// The first `steps` steps of a fresh simulation of `g`, one record each.
std::vector<simsys::engine::StepRecord> records_of(const Graph& g, int steps) {
  auto sim = Simulation::create(g);
  if (!sim) FAIL(sim.error().message);
  std::vector<simsys::engine::StepRecord> records;
  for (int i = 0; i < steps; ++i) records.push_back(sim->step(i * 100ms, 100ms));
  return records;
}

constexpr double room_mb = 1000.0;     // the pipe's waiting room: the generic queue_limit in MB
constexpr double step_s = 0.1;         // the scenario's step in seconds
constexpr double pipe_mb_per_s = 125;  // 1 Gbit/s in megabytes per second
}  // namespace

TEST_CASE("1 MB gets at 100 per second over 1 Gbit/s run the pipe at rho 0.8 and wait 16 ms",
          "[theory]") {
  const auto g = *Graph::build(pipe_over(100, simsys::Bytes{1e6}));
  const auto r = first_step(g);
  const auto& pipe = only_link(r);
  REQUIRE_THAT(pipe.demand_per_s, WithinRel(100.0, 1e-12));  // MB per second, not requests
  REQUIRE_THAT(pipe.drain_per_s, WithinRel(100.0, 1e-12));
  REQUIRE(pipe.dropped_per_s == 0.0);
  REQUIRE(pipe.backlog == 0.0);
  REQUIRE_THAT(pipe.rho, WithinRel(0.8, 1e-12));
  REQUIRE(pipe.zone == simsys::fluid::Zone::yellow);
  // M/D/1: half of the M/M/1 wait 0.8 / 0.2 x 8 ms = 32 ms (queueing::variability_factor(1, 0)).
  const double wait = simsys::queueing::variability_factor(1.0, 0.0) *
                      simsys::queueing::mmc_wait(1, 0.8, 0.008).mean_s;
  REQUIRE_THAT(wait, WithinAbs(0.016, 1e-12));
  REQUIRE_THAT(pipe.latency.mean_s, WithinRel(wait + 0.008, 1e-9));
  REQUIRE_THAT(pipe.latency.mean_s, WithinAbs(0.024, 1e-9));  // the napkin number: 24 ms
}

// The hop adds the pipe's 24 ms once to the round trip, beside the link's own 2 x 250 us: the
// same scenario with the bandwidth removed differs by exactly that, and b sees the same 100/s.
TEST_CASE("the pipe adds its 24 ms sojourn once to the round trip beside the link's 0.5 ms",
          "[theory]") {
  const auto with = first_step(*Graph::build(pipe_over(100, simsys::Bytes{1e6})));
  const auto without = first_step(*Graph::build(pipe_over(100, simsys::Bytes{1e6}, std::nullopt)));
  const double b_sojourn = with.nodes[1].latency.mean_s;
  REQUIRE_THAT(b_sojourn, WithinAbs(0.001, 1e-9));  // 1 ms, no wait at rho 1e-4
  REQUIRE_THAT(without.nodes[1].latency.mean_s, WithinRel(b_sojourn, 1e-12));
  REQUIRE_THAT(without.responses[0].response.mean_s, WithinRel(0.0005 + b_sojourn, 1e-9));
  REQUIRE_THAT(with.responses[0].response.mean_s, WithinRel(0.0005 + 0.024 + b_sojourn, 1e-9));
  REQUIRE_THAT(with.responses[0].response.mean_s - without.responses[0].response.mean_s,
               WithinAbs(0.024, 1e-9));
  REQUIRE_THAT(with.nodes[1].demand_per_s, WithinRel(100.0, 1e-12));  // requests at b
}

// A 10 MB request at 10/s fills the pipe as much as 1 MB at 100/s (100 MB/s, rho 0.8 again) but
// holds it ten times longer: s_obj = 10 x 8 ms = 80 ms, one whole object at a time.
//   Wq (M/D/1) = 0.5 x 0.8 / 0.2 x 80 ms = 160 ms     (= 10 x (80 ms)^2 / 0.4 by
//                                                     Pollaczek-Khinchine)
// What the hop adds after that wait depends on what b does with the request:
//   a read  (`get`)   = 160 ms + its first megabyte, 8 ms = 168 ms: b answers at its first MB
//                       while the other 9 MB stream in behind it;
//   a write (`write`) = 160 ms + the WHOLE request, 80 ms = 240 ms: b acts after the last byte.
TEST_CASE("a 10 MB read waits 160 ms and its first megabyte takes 8 ms: a 168 ms hop", "[theory]") {
  const auto g = *Graph::build(pipe_over(10, simsys::Bytes{10e6}));
  const auto r = first_step(g);
  const auto& pipe = only_link(r);
  REQUIRE_THAT(pipe.demand_per_s, WithinRel(100.0, 1e-12));  // 10 requests/s x 10 MB
  REQUIRE_THAT(pipe.rho, WithinRel(0.8, 1e-12));
  const double wait = 0.5 * simsys::queueing::mmc_wait(1, 0.8, 0.080).mean_s;
  REQUIRE_THAT(wait, WithinAbs(0.160, 1e-12));
  REQUIRE_THAT(pipe.latency.mean_s, WithinRel(wait + 0.008, 1e-9));
  REQUIRE_THAT(pipe.latency.mean_s, WithinAbs(0.168, 1e-9));
  const double b_sojourn = r.nodes[1].latency.mean_s;
  REQUIRE_THAT(r.nodes[1].demand_per_s, WithinRel(10.0, 1e-12));  // b counts requests
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(0.0005 + 0.168 + b_sojourn, 1e-9));
}

TEST_CASE("a 10 MB write waits 160 ms and is serialised whole for 80 ms: a 240 ms hop",
          "[theory]") {
  auto s = pipe_over(10, simsys::Bytes{10e6});
  s.nodes[0].workload->op_shares = {{"write", 1.0}};
  s.nodes[0].workload->sizes = {{"write", simsys::Bytes{10e6}}};
  const auto g = *Graph::build(s);
  const auto r = first_step(g);
  const auto& pipe = only_link(r);
  REQUIRE_THAT(pipe.rho, WithinRel(0.8, 1e-12));  // the same queue as the read's
  REQUIRE_THAT(pipe.latency.mean_s, WithinAbs(0.240, 1e-9));
  const double b_sojourn = r.nodes[1].latency.mean_s;
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(0.0005 + 0.240 + b_sojourn, 1e-9));
}

// Whether a hop pays a request whole on the wire is decided per (link, op) at build (review of
// plan 2c Task 6, I-2): an op is a write on a link if a node it reaches under its own name lists it
// in `write_ops`, the receiver or any node behind it. A 10 MB `upload` at 5/s over 1 Gbit/s into a
// blob store that lists `upload`:
//   rho = 5 x 10 MB / 125 MB/s = 0.4;  s_obj = 10 x 8 ms = 80 ms
//   Wq (M/D/1) = 0.5 x 0.4 / 0.6 x 80 ms = 26.67 ms;  a write's answer: the whole 80 ms
//   pipe = 106.67 ms, whether the link ends at the store or at a load balancer in front of it.
// Before the fix the balancer (whose own `write_ops` is the default `[write]`) made it a read on
// the wire: 26.67 + 8 = 34.67 ms, 72 ms less than the same bytes straight into the store.
namespace {
// a (5 `upload`s/s of 10 MB) -> [lb ->] store (a default blob store whose write op is `upload`),
// the first hop over 1 Gbit/s.
Scenario upload_over_pipe(bool behind_balancer) {
  auto s = pipe_over(5, simsys::Bytes{10e6});
  s.nodes[0].workload->op_shares = {{"upload", 1.0}};
  s.nodes[0].workload->sizes = {{"upload", simsys::Bytes{10e6}}};
  NodeSpec store{.id = "store", .type = NodeType::blob_store};
  store.write_ops = std::vector<std::string>{"upload"};
  if (!behind_balancer) {
    s.nodes[1] = std::move(store);
    s.links[0].to = "store";
    return s;
  }
  s.nodes[1] = NodeSpec{.id = "lb", .type = NodeType::load_balancer};
  s.nodes.push_back(std::move(store));
  s.links[0].to = "lb";
  s.links.push_back(LinkSpec{.from = "lb", .to = "store", .latency = 250us});
  return s;
}
}  // namespace

TEST_CASE("a 10 MB upload behind a load balancer pays the wire as a write, as straight in",
          "[theory]") {
  const auto straight = first_step(*Graph::build(upload_over_pipe(false)));
  const auto balanced = first_step(*Graph::build(upload_over_pipe(true)));
  const double wait = 0.5 * simsys::queueing::mmc_wait(1, 0.4, 0.080).mean_s;
  REQUIRE_THAT(wait, WithinAbs(0.026667, 1e-6));
  REQUIRE_THAT(only_link(straight).rho, WithinRel(0.4, 1e-12));
  REQUIRE_THAT(only_link(straight).latency.mean_s, WithinRel(wait + 0.080, 1e-9));
  REQUIRE_THAT(only_link(balanced).latency.mean_s,
               WithinRel(only_link(straight).latency.mean_s, 1e-12));
}

// A renamed step that states its own `size:` forwards a payload (review of plan 2c Task 7, I-2):
// the service had the client's whole body before it sent it on, so when the store behind it
// writes the renamed op, the client's hop into the service is a write on the wire as well. 10 MB
// `upload`s at 5/s over 1 Gbit/s into a service whose step is `{to: store, op: put, size: 10MB}`,
// in front of a blob store whose write op is `put`, pay the same 26.67 + 80 = 106.67 ms on the
// wire as `{to: store, op: upload, size: 10MB}` in front of one that writes `upload`, and the
// client gets the same response. Before the fix the walk stopped at the renamed step: the hop was
// paid as a read, 26.67 + 8 = 34.67 ms, 72 ms less for the same bytes.
namespace {
// a (5 `upload`s/s of 10 MB) -> b, over 1 Gbit/s; b's `upload` sends `store_op` (10 MB) on to a
// default blob store that lists `store_op` as its write.
Scenario upload_relayed_as(const std::string& store_op) {
  auto s = pipe_over(5, simsys::Bytes{10e6});
  s.nodes[0].workload->op_shares = {{"upload", 1.0}};
  s.nodes[0].workload->sizes = {{"upload", simsys::Bytes{10e6}}};
  s.nodes[1].ops = {{"upload", {Step{.to = "store", .op = store_op, .size = simsys::Bytes{10e6}}}}};
  NodeSpec store{.id = "store", .type = NodeType::blob_store};
  store.write_ops = std::vector<std::string>{store_op};
  s.nodes.push_back(std::move(store));
  s.links.push_back(LinkSpec{.from = "b", .to = "store", .latency = 250us});
  return s;
}
}  // namespace

TEST_CASE("an upload a service sends on as a sized put pays the client's wire as a write",
          "[theory]") {
  const auto renamed = first_step(*Graph::build(upload_relayed_as("put")));
  const auto same_op = first_step(*Graph::build(upload_relayed_as("upload")));
  const double wait = 0.5 * simsys::queueing::mmc_wait(1, 0.4, 0.080).mean_s;  // 26.67 ms
  REQUIRE_THAT(only_link(same_op).latency.mean_s, WithinRel(wait + 0.080, 1e-9));
  REQUIRE_THAT(only_link(renamed).latency.mean_s, WithinRel(wait + 0.080, 1e-9));
  REQUIRE_THAT(response_to(renamed, "upload").response.mean_s,
               WithinRel(response_to(same_op, "upload").response.mean_s, 1e-12));
}

// Only the body itself, relabelled, makes the client's hop a write (review of plan 2c Task 8,
// I-2): a step of another size is a new request. The same 10 MB uploads, but the service writes a
// 1 KB `put_meta` (the object's metadata) to a store that lists `put_meta`: the store writes 1 KB,
// not the client's 10 MB, so the client's hop stays a read on the wire, answered at its first
// megabyte: 26.67 + 8 = 34.67 ms, not the 106.67 ms of a forwarded body. (Before the fix any
// sized rename counted as forwarding the body, 72 ms too much.)
TEST_CASE("an upload whose service writes a 1 KB put_meta stays a read on the client's wire",
          "[theory]") {
  auto s = upload_relayed_as("put_meta");
  s.nodes[1].ops.at("upload").front().size = simsys::Bytes{1e3};
  const auto g = *Graph::build(s);
  REQUIRE(g.links()[0].write_ops.empty());
  const auto r = first_step(g);
  const double wait = 0.5 * simsys::queueing::mmc_wait(1, 0.4, 0.080).mean_s;  // 26.67 ms
  REQUIRE_THAT(only_link(r).latency.mean_s, WithinRel(wait + 0.008, 1e-9));
  REQUIRE_THAT(only_link(r).latency.mean_s, WithinAbs(0.034667, 1e-6));
}

// The same size written two ways is the same body (review of plan 2c Tasks 9-10, minor 1): the
// client's `2.01MB` and the step's `2010KB` parse to 2.01 x 1e6 = 2009999.9999999998 and
// 2010 x 1e3 = 2010000 bytes, two doubles one rounding apart, so the step carries the upload on and
// the client's hop is a write on the wire. Sizes are compared in whole bytes (std::llround).
TEST_CASE("a 2010KB step carries a 2.01MB upload on: sizes compare in whole bytes", "[theory]") {
  auto s = upload_relayed_as("put");
  const auto upload = *simsys::parse_size("2.01MB");
  const auto step = *simsys::parse_size("2010KB");
  REQUIRE(upload.count != step.count);  // one rounding apart as doubles
  s.nodes[0].workload->sizes = {{"upload", upload}};
  s.nodes[1].ops.at("upload").front().size = step;
  const auto g = *Graph::build(s);
  REQUIRE(g.links()[0].write_ops == std::vector<std::string>{"upload"});
}

// Two ops of different sizes share one pipe: 20/s of `get` (1 MB, 8 ms, a read) and 20/s of `write`
// (4 MB, 32 ms, a write at b) are 20 + 80 = 100 MB/s, rho 0.8 again, but the objects are NOT
// alike. With p = 1/2 each:
//   E[S]   = 0.5 x 8 ms + 0.5 x 32 ms = 20 ms
//   E[S^2] = 0.5 x (8 ms)^2 + 0.5 x (32 ms)^2 = 544e-6 s^2     (each object's own SCV is 0)
//   c_s^2  = E[S^2] / E[S]^2 - 1 = 544e-6 / 400e-6 - 1 = 0.36  (the mixture's SCV)
//   Wq     = (1 + 0.36) / 2 x rho / (1 - rho) x E[S] = 0.68 x 4 x 20 ms = 54.4 ms
//            = lambda E[S^2] / (2 (1 - rho)) = 40 x 544e-6 / 0.4  (Pollaczek-Khinchine, exact for
//            one server): more than the 16 ms of 1 MB alone at the same rho. Both ops wait the
//            same Wq in the one queue; the read is then answered at its first (and only)
//            megabyte, the write after its whole 4 MB:
//   get    = 0.5 ms + 54.4 ms + 8 ms + b;   write = 0.5 ms + 54.4 ms + 32 ms + b
// and the pipe's own latency is the request-weighted mean, 54.4 + 0.5 x 8 + 0.5 x 32 = 74.4 ms.
TEST_CASE("two sizes on one pipe wait 54.4 ms, more than one size at the same rho: the mixture",
          "[theory]") {
  auto s = pipe_over(40, simsys::Bytes{1e6});
  s.nodes[0].workload->op_shares = {{"get", 0.5}, {"write", 0.5}};
  s.nodes[0].workload->sizes = {{"get", simsys::Bytes{1e6}}, {"write", simsys::Bytes{4e6}}};
  const auto g = *Graph::build(s);
  const auto r = first_step(g);
  const auto& pipe = only_link(r);
  REQUIRE_THAT(pipe.demand_per_s, WithinRel(100.0, 1e-12));
  REQUIRE_THAT(pipe.rho, WithinRel(0.8, 1e-12));
  const double mean_s = 0.5 * 0.008 + 0.5 * 0.032;
  const double second_moment = 0.5 * 0.008 * 0.008 + 0.5 * 0.032 * 0.032;
  const double scv = second_moment / (mean_s * mean_s) - 1.0;
  REQUIRE_THAT(scv, WithinAbs(0.36, 1e-12));
  const double wait = simsys::queueing::variability_factor(1.0, scv) *
                      simsys::queueing::mmc_wait(1, 0.8, mean_s).mean_s;
  REQUIRE_THAT(wait, WithinAbs(0.0544, 1e-12));
  REQUIRE_THAT(40.0 * second_moment / (2.0 * (1.0 - 0.8)), WithinAbs(0.0544, 1e-12));  // P-K
  REQUIRE_THAT(pipe.latency.mean_s, WithinAbs(0.0544 + 0.020, 1e-9));
  const double b_sojourn = r.nodes[1].latency.mean_s;
  REQUIRE_THAT(response_to(r, "get").response.mean_s,
               WithinRel(0.0005 + 0.0544 + 0.008 + b_sojourn, 1e-9));
  REQUIRE_THAT(response_to(r, "write").response.mean_s,
               WithinRel(0.0005 + 0.0544 + 0.032 + b_sojourn, 1e-9));
  // The same 100 MB/s as one size waits 16 ms: the spread of sizes is what lengthens the wait.
  const auto alone = first_step(*Graph::build(pipe_over(100, simsys::Bytes{1e6})));
  const double alone_wait = only_link(alone).latency.mean_s - 0.008;  // 16 ms
  REQUIRE_THAT(alone_wait, WithinAbs(0.016, 1e-9));
  REQUIRE(wait > 3.0 * alone_wait);  // 54.4 ms against 16 ms
}

// A 40 Gbit/s link (youtube's cross-datacenter one, Task 16) is 5 GB/s = 5000 MB/s, 0.2 ms per
// MB. 100 MB/s of it is rho = 0.02: Wq = 0.5 x 0.02 / 0.98 x 0.2 ms = 2.04 us, and the hop adds
// 0.2 ms + 2.04 us. A wide pipe is nearly free; the model must not invent delay for it.
TEST_CASE("a 40 Gbit/s pipe at 100 MB per second adds 0.2 ms: rho 0.02, a wait of 2 us",
          "[theory]") {
  const auto g = *Graph::build(pipe_over(100, simsys::Bytes{1e6}, simsys::ByteRate{5e9}));
  const auto r = first_step(g);
  const auto& pipe = only_link(r);
  REQUIRE_THAT(pipe.rho, WithinRel(0.02, 1e-12));
  REQUIRE(pipe.zone == simsys::fluid::Zone::green);
  const double wait = 0.5 * simsys::queueing::mmc_wait(1, 0.02, 0.0002).mean_s;
  REQUIRE_THAT(wait, WithinAbs(2.0408e-6, 1e-10));
  REQUIRE_THAT(pipe.latency.mean_s, WithinRel(wait + 0.0002, 1e-9));
  const auto without = first_step(*Graph::build(pipe_over(100, simsys::Bytes{1e6}, std::nullopt)));
  REQUIRE_THAT(r.responses[0].response.mean_s - without.responses[0].response.mean_s,
               WithinRel(wait + 0.0002, 1e-6));
}

// 150 MB/s into a 125 MB/s pipe: rho is offered 1.2. The pipe drains 125/s, so the backlog grows
// by (150 - 125) x 0.1 s = 2.5 MB per step and the 1000 MB room is full after 1000 / 25 = 40 s,
// i.e. 400 steps. Until then nothing is dropped and every request is answered (a backlog is only
// a wait); from then on the pipe drops 150 - 125 = 25 MB/s. The room's drain time bounds the
// wait: 1000 MB / 125 MB/s = 8 s, so the hop's latency tops out at 8 s + 8 ms serialisation, and
// the client's delivered fraction is 125 / 150 = 0.8333 in the steady state.
TEST_CASE(
    "at 150 per second the pipe fills its 1000 MB room in 40 s and then drops 25 MB per second",
    "[theory]") {
  const auto g = *Graph::build(pipe_over(150, simsys::Bytes{1e6}));
  const auto records = records_of(g, 600);  // 60 s
  std::size_t first_drop = records.size();
  double latency_so_far = 0.0;
  for (std::size_t i = 0; i < records.size(); ++i) {
    const auto& pipe = only_link(records[i]);
    CAPTURE(i);
    REQUIRE_THAT(pipe.drain_per_s, WithinRel(pipe_mb_per_s, 1e-9));  // the pipe is saturated
    REQUIRE(pipe.backlog <= room_mb + 1e-9);
    REQUIRE(pipe.latency.mean_s >= latency_so_far - 1e-12);                  // the wait only grows
    REQUIRE(pipe.latency.mean_s <= room_mb / pipe_mb_per_s + 0.008 + 1e-9);  // 8 s + 8 ms
    latency_so_far = pipe.latency.mean_s;
    if (first_drop == records.size() && pipe.dropped_per_s > 1e-6) first_drop = i;
  }
  // The room is full after step 399 (400 x 2.5 MB = 1000 MB, t = 40 s): the first drop is the
  // next step; one step either way absorbs the rounding of the accumulated backlog.
  REQUIRE(first_drop >= 399);
  REQUIRE(first_drop <= 401);
  // Before: 20 s in, 200 steps x 2.5 MB = 500 MB wait, nothing lost, every request answered.
  const auto& filling = records[199];
  REQUIRE_THAT(only_link(filling).backlog, WithinAbs(500.0, 1e-6));
  REQUIRE(only_link(filling).dropped_per_s == 0.0);
  REQUIRE_THAT(filling.responses[0].answered.per_second,
               WithinRel(filling.responses[0].offered.per_second, 1e-12));
  // After: the steady state at 60 s.
  const auto& last = records.back();
  const auto& pipe = only_link(last);
  REQUIRE_THAT(pipe.demand_per_s, WithinRel(150.0, 1e-12));
  REQUIRE_THAT(pipe.dropped_per_s, WithinRel(25.0, 1e-6));
  REQUIRE_THAT(pipe.backlog, WithinAbs(room_mb, 1e-6));
  REQUIRE_THAT(pipe.rho, WithinRel(1.0, 1e-9));
  REQUIRE(pipe.zone == simsys::fluid::Zone::red);
  REQUIRE_THAT(pipe.latency.mean_s, WithinRel(8.008, 1e-9));
  const auto& answer = last.responses[0];
  REQUIRE_THAT(answer.offered.per_second, WithinRel(150.0, 1e-12));
  REQUIRE_THAT(answer.answered.per_second / answer.offered.per_second,
               WithinRel(125.0 / 150.0, 1e-6));  // 0.8333
  REQUIRE_THAT(answer.answered.per_second, WithinRel(125.0, 1e-6));
  // b is sent what the pipe drained, 125/s, and loses none of it.
  REQUIRE_THAT(last.nodes[1].demand_per_s, WithinRel(125.0, 1e-6));
}

// The fluid queue's balance on the link, every step of the run above: what came in was served,
// dropped or is waiting, demand.dt = (drain + dropped).dt + (backlog - the previous backlog).
TEST_CASE(
    "the pipe conserves megabytes at every step: demand x dt = served + dropped + change in "
    "backlog",
    "[theory]") {
  const auto records = records_of(*Graph::build(pipe_over(150, simsys::Bytes{1e6})), 600);
  double previous_backlog = 0.0;
  bool grew = false;
  bool dropped = false;
  for (std::size_t i = 0; i < records.size(); ++i) {
    const auto& pipe = only_link(records[i]);
    CAPTURE(i);
    const double in_mb = pipe.demand_per_s * step_s;
    const double out_mb = (pipe.drain_per_s + pipe.dropped_per_s) * step_s;
    REQUIRE_THAT(in_mb, WithinAbs(out_mb + (pipe.backlog - previous_backlog), 1e-9));
    grew = grew || pipe.backlog > previous_backlog;
    dropped = dropped || pipe.dropped_per_s > 0.0;
    previous_backlog = pipe.backlog;
  }
  REQUIRE(grew);     // not vacuous: the backlog really filled ...
  REQUIRE(dropped);  // ... and the room really overflowed
}

// The pipe's losses multiply the hop's delivered fraction (plan: "its losses multiply the hop's
// delivered fraction"), as any node's drops do. Here b is the weaker one: a capacity of 100/s.
// The pipe passes 125 of 150 MB/s (5/6); b is sent 125/s, serves 100 and, once its own room of
// 1000 requests has filled, drops 25: it passes 100 / 125 = 0.8. The client is answered
// 150 x 5/6 x 0.8 = 100/s, b's capacity, a delivered fraction of 2/3. (Both rooms are full after
// 40 s and 1000 / 25 = 40 s: by 100 s the steady state holds.)
TEST_CASE("the pipe's drops and the next node's drops multiply in the client's delivered fraction",
          "[theory]") {
  auto s = pipe_over(150, simsys::Bytes{1e6});
  s.nodes[1].capacity = Rate{100};
  const auto g = *Graph::build(s);
  const auto records = records_of(g, 1000);  // 100 s
  const auto& last = records.back();
  REQUIRE_THAT(only_link(last).dropped_per_s, WithinRel(25.0, 1e-6));
  REQUIRE_THAT(last.nodes[1].dropped_per_s, WithinRel(25.0, 1e-6));
  REQUIRE_THAT(last.responses[0].answered.per_second, WithinRel(100.0, 1e-6));
  REQUIRE_THAT(last.responses[0].answered.per_second / last.responses[0].offered.per_second,
               WithinRel(2.0 / 3.0, 1e-6));
}

// estimate() is the long-run state `run` reaches: below capacity the pipe is empty and waits its
// 16 ms (the settle passes read it without the delay line, as for every node); above it the
// backlog is the full room, the pipe drops 25 MB/s and the client is answered 5/6 of its load.
TEST_CASE("estimate and run agree on the pipe, below capacity and above it", "[theory]") {
  SECTION("rho 0.8") {
    const auto g = *Graph::build(pipe_over(100, simsys::Bytes{1e6}));
    const auto e = (*Simulation::create(g)).estimate();
    const auto ran = records_of(g, 20).back();
    REQUIRE(e.converged);
    const auto& settled = only_link(e);
    const auto& stepped = only_link(ran);
    REQUIRE_THAT(settled.demand_per_s, WithinRel(100.0, 1e-9));
    REQUIRE_THAT(settled.drain_per_s, WithinRel(stepped.drain_per_s, 1e-9));
    REQUIRE_THAT(settled.rho, WithinRel(0.8, 1e-9));
    REQUIRE(settled.backlog == 0.0);
    REQUIRE_THAT(settled.latency.mean_s, WithinAbs(0.024, 1e-9));
    REQUIRE_THAT(settled.latency.mean_s, WithinRel(stepped.latency.mean_s, 1e-9));
    REQUIRE_THAT(e.responses[0].response.mean_s, WithinRel(ran.responses[0].response.mean_s, 1e-9));
  }
  SECTION("rho 1.2") {
    const auto e =
        (*Simulation::create(*Graph::build(pipe_over(150, simsys::Bytes{1e6})))).estimate();
    REQUIRE(e.converged);
    const auto& settled = only_link(e);
    REQUIRE_THAT(settled.drain_per_s, WithinRel(125.0, 1e-9));
    REQUIRE_THAT(settled.dropped_per_s, WithinRel(25.0, 1e-9));
    REQUIRE_THAT(settled.backlog, WithinAbs(room_mb, 1e-9));
    REQUIRE_THAT(settled.latency.mean_s, WithinRel(8.008, 1e-9));
    REQUIRE_THAT(e.responses[0].answered.per_second, WithinRel(125.0, 1e-9));
  }
}

// The pipe is fed by the requests alone, each charged its stated size in the direction it is
// sent: a reply adds no bytes, so an `inout` link (requests out, replies back on the same link) is
// ONE pipe with the same load as an `out` link, not two and not a doubled load. (Two links `a->b`
// and `b->a` that each state a bandwidth are two pipes, each fed by its own direction's demand.)
TEST_CASE("an inout link is one pipe fed by its requests alone: replies add no bytes", "[theory]") {
  const auto out = first_step(*Graph::build(pipe_over(100, simsys::Bytes{1e6})));
  const auto both =
      first_step(*Graph::build(pipe_over(100, simsys::Bytes{1e6}, one_gbit, Direction::inout)));
  REQUIRE(both.links.size() == 1);
  REQUIRE_THAT(only_link(both).demand_per_s, WithinRel(100.0, 1e-12));
  REQUIRE_THAT(only_link(both).latency.mean_s, WithinRel(only_link(out).latency.mean_s, 1e-12));
  REQUIRE_THAT(both.responses[0].response.mean_s,
               WithinRel(out.responses[0].response.mean_s, 1e-12));
}

// A link without `bandwidth` behaves exactly as before Task 5: no sample, and the round trip is
// the link's own 2 x 250 us plus b's sojourn, as in the blob store tests above (the same 0.5 ms),
// whether or not the client states a size.
TEST_CASE("a link without a bandwidth has no pipe: no link sample and only its own latency",
          "[theory]") {
  SECTION("sized requests") {
    const auto r = first_step(*Graph::build(pipe_over(100, simsys::Bytes{1e6}, std::nullopt)));
    REQUIRE(r.links.empty());
    REQUIRE_THAT(r.responses[0].response.mean_s,
                 WithinRel(0.0005 + r.nodes[1].latency.mean_s, 1e-9));
    REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(0.0015, 1e-9));  // 0.5 ms + 1 ms
  }
  SECTION("unsized requests") {
    auto s = pipe_over(100, simsys::Bytes{1e6}, std::nullopt);
    s.nodes[0].workload->sizes.clear();
    const auto r = first_step(*Graph::build(s));
    REQUIRE(r.links.empty());
    REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(0.0015, 1e-9));
  }
  SECTION("the long-run estimate has none either") {
    const auto e = (*Simulation::create(*Graph::build(mm1(500)))).estimate();
    REQUIRE(e.links.empty());
  }
}

// A pipe needs a size for every op it carries: an unsized one is refused at build, naming the
// link, the op and its sender, and what to add. (The graph tests pin more senders.)
TEST_CASE("an unsized op on a bandwidth link is refused with the advice to add a size",
          "[theory]") {
  Scenario s = pipe_over(100, simsys::Bytes{1e6});
  s.nodes[0].workload->op_shares = {{"ping", 1.0}};
  s.nodes[0].workload->sizes.clear();
  const auto built = Graph::build(s);
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "link 'a'->'b' has a bandwidth, but op 'ping' from 'a' has no size: add size: to the "
          "step or sizes: to the client");
}

// ---- Message queues hold what worker pools have not drained; pools are M/M/c over consumption
// (plan 2c refinements 4-6, Task 6) ----
//
// The broker (book ch. 14's transcode_q, Kafka in practice) answers its publisher once the message
// is appended; the message then goes to every consumer group, and what a group has not drained
// waits in that group's pool queue, whose room is `retention x the pool's capacity` (a message
// older than `retention` is deleted unconsumed: the pool's drop). Nobody upstream waits for a
// worker: the hop is asynchronous, so a pool's backlog and drops never reach the publisher's
// response or answered rate.
//
// Numbers, from one picture: a client publishes 1000 messages/s to the queue over a 250 us link
// (0.5 ms round trip); the queue's default append is 1M/s at 100 us (spec section 6, c = 1M/s x
// 100 us = 100); one group drains it with 4 workers at 3.333333 ms a job:
//   pool capacity = workers / cost = 4 / 3.333333 ms = 1200.00012 jobs/s ~ 1.2k/s (Little's law
//                   again: c / service time)
//   rho           = 1000 / 1200 = 0.8333      (offered load a = 1000 x 3.3333 ms = 3.3333 erlangs
//                                              over 4 servers)
//   Erlang C(4, a = 3.3333) = 0.6577          (the chance a message finds all 4 workers busy)
//   Wq   (M/M/c)  = C x s / (c (1 - rho)) = 0.6577 x 3.3333 ms / (4 x 0.16667) = 3.289 ms
//   sojourn       = Wq + s = 3.289 + 3.333 = 6.622 ms      (the pool's service SCV is 1: the
//                   default, an exponential job time, and Poisson arrivals: Allen-Cunneen's
//                   factor (1 + 1) / 2 = 1, so M/M/c exactly)
// The publisher's response is none of that:
//   append sojourn = 100 us (+ a wait of Erlang C(100, rho 0.001) x ... ~ 1e-259 s: none)
//   response       = 100 us + the link's round trip 2 x 250 us = 0.6 ms
namespace {
// A pool `id` of `workers` workers at `cost` a job, draining queue `q`.
NodeSpec group_of(std::string id, int workers, simsys::SimTime cost) {
  NodeSpec pool{.id = std::move(id), .type = NodeType::worker_pool};
  pool.consumes = "q";
  pool.workers = workers;
  pool.job_cost = cost;
  return pool;
}

// u (`rate`/s of `publish`, 250 us link) -> q (a message queue, `retention` as given, the type's
// default when none) -> every group in `groups` over zero-latency links, so that everything a
// step sends arrives in the same step. Node indices: u 0, q 1, then the groups in order.
Scenario published(double rate, std::vector<NodeSpec> groups,
                   std::optional<simsys::SimTime> retention = std::nullopt) {
  Scenario s{.name = "published", .version = 1, .duration = 60s};
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload =
                          Workload{.curve = {.base = Rate{rate}}, .op_shares = {{"publish", 1.0}}}},
             NodeSpec{.id = "q", .type = NodeType::message_queue}};
  s.nodes[1].retention = retention;
  s.links = {LinkSpec{.from = "u", .to = "q", .latency = 250us}};
  for (auto& group : groups) {
    s.links.push_back(LinkSpec{.from = "q", .to = group.id, .latency = 0s});
    s.nodes.push_back(std::move(group));
  }
  return s;
}

// 4 workers at 3.333333 ms: the picture above.
NodeSpec four_workers() {
  return group_of("w", 4, simsys::SimTime{3'333'333});
}
// 12 workers at 10 ms: also 1200 jobs/s, but EXACTLY (12 / 0.01 s), so the backlog arithmetic of
// the burst tests below is exact; c = 12.
NodeSpec twelve_workers() {
  return group_of("w", 12, 10ms);
}
constexpr double pool_cost_s = 3.333333e-3;
}  // namespace

TEST_CASE(
    "a pool of 4 workers at 3.333 ms fed 1k messages a second runs at rho 0.8333 and waits "
    "3.289 ms",
    "[theory]") {
  const auto g = *Graph::build(published(1000, {four_workers()}));
  const auto r = first_step(g);
  const auto& pool = r.nodes[*g.index_of("w")];
  const double capacity = 4.0 / pool_cost_s;  // 1200.00012/s
  REQUIRE_THAT(pool.demand_per_s, WithinRel(1000.0, 1e-9));
  REQUIRE_THAT(pool.rho, WithinRel(1000.0 / capacity, 1e-9));
  REQUIRE_THAT(pool.rho, WithinAbs(0.8333, 1e-4));
  const double wait = simsys::queueing::mmc_wait(4, pool.rho, pool_cost_s).mean_s;
  REQUIRE_THAT(wait, WithinAbs(0.003289, 1e-6));  // the napkin number: 3.289 ms
  REQUIRE_THAT(pool.latency.mean_s, WithinRel(wait + pool_cost_s, 1e-9));
  REQUIRE_THAT(pool.latency.mean_s, WithinAbs(0.006622, 1e-6));  // sojourn 6.622 ms
}

// The Erlang C value itself, so the 3.289 ms is not taken on trust: P(wait) = 0.6577 at c = 4 and
// 3.3333 erlangs (queueing::erlang_c takes the per-server utilisation).
TEST_CASE("the chance a message waits for one of 4 workers at rho 0.8333 is 0.6577", "[theory]") {
  REQUIRE_THAT(simsys::queueing::erlang_c(4, 1000.0 * pool_cost_s / 4.0), WithinAbs(0.6577, 1e-4));
}

// The publisher is answered at the append: 100 us (the broker's c = 100 servers at rho 0.001 make
// the wait vanish) + the 0.5 ms round trip = 0.6 ms. The worker's 6.6 ms is not in it, nor is its
// loss: all 1000/s are answered.
TEST_CASE("the publisher is answered when the message is appended: 0.6 ms, not the worker's 6.6",
          "[theory]") {
  const auto g = *Graph::build(published(1000, {four_workers()}));
  const auto r = first_step(g);
  const auto& broker = r.nodes[*g.index_of("q")];
  REQUIRE_THAT(broker.rho, WithinRel(1000.0 / 1e6, 1e-9));  // 0.001: the append's rho
  REQUIRE_THAT(broker.latency.mean_s, WithinAbs(100e-6, 1e-9));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(broker.latency.mean_s + 0.0005, 1e-9));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(0.0006, 1e-9));
  REQUIRE_THAT(r.responses[0].answered.per_second, WithinRel(1000.0, 1e-9));
}

// Every consumer group receives every message: two pools on one queue are each offered the
// queue's whole 1000/s (a split would give each 500/s), and the broker still appends 1000/s.
TEST_CASE("two worker pools consuming one queue each receive all 1k messages a second",
          "[theory]") {
  auto second = group_of("w2", 10, 5ms);  // 2000 jobs/s: no queue to speak of
  const auto g = *Graph::build(published(1000, {four_workers(), std::move(second)}));
  const auto r = first_step(g);
  REQUIRE_THAT(r.nodes[*g.index_of("q")].demand_per_s, WithinRel(1000.0, 1e-9));
  REQUIRE_THAT(r.nodes[*g.index_of("w")].demand_per_s, WithinRel(1000.0, 1e-9));
  REQUIRE_THAT(r.nodes[*g.index_of("w2")].demand_per_s, WithinRel(1000.0, 1e-9));
  // w2: rho = 1000 / 2000 = 0.5 per server (10 workers, a = 5 erlangs).
  REQUIRE_THAT(r.nodes[*g.index_of("w2")].rho, WithinRel(0.5, 1e-9));
  // The publisher is still answered by the append alone, however many groups listen.
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(0.0006, 1e-9));
}

TEST_CASE("the engine runs a queue and a pool that has no then steps", "[theory]") {
  const auto g = *Graph::build(published(1000, {four_workers()}));
  const auto sim = Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
}

// Task 6 refused a pool's `then` steps ("run in phase 2c's next step") rather than drop them
// silently; Task 7 runs them, so the refusal is now its opposite: the engine accepts a pool that
// hands its drained jobs on (the pins on what it sends are the section at the end of this file).
TEST_CASE("the engine runs a worker pool that has then steps", "[theory]") {
  auto pool = four_workers();
  pool.then = {Step{.to = "svc", .op = "done"}};
  auto s = published(1000, {std::move(pool)});
  s.nodes.push_back(NodeSpec{.id = "svc", .type = NodeType::service});
  s.links.push_back(LinkSpec{.from = "w", .to = "svc"});
  const auto g = *Graph::build(s);
  const auto sim = Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
}

// ---- A backlog grows at (inflow - capacity) and drains at (capacity - inflow) ----
//
// A pool of 12 workers at 10 ms is exactly 1200 jobs/s. The client publishes 1000/s with a spike
// of x1.5 for the first 200 s: 1500/s, so the pool's backlog grows by 1500 - 1200 = 300 messages
// a second to 200 s x 300 = 60,000 messages (the FIFO drain time: a message published at 200 s
// waits 60,000 / 1200 = 50 s for the ones ahead of it). The default 7-day retention keeps all of
// them (a room of 725.76 million). Then the inflow falls to 1000/s and the pool drains the backlog
// at 1200 - 1000 = 200/s: empty after 60,000 / 200 = 300 s more, at t = 500 s. Steps of 100 ms:
// record i ends at t = (i + 1) x 0.1 s.

namespace {
Scenario burst_then_calm() {
  auto s = published(1000, {twelve_workers()});
  s.nodes[0].workload->curve.spikes = {
      simsys::workload::Spike{.at = 0s, .duration = 200s, .factor = 1.5}};
  return s;
}
}  // namespace

TEST_CASE("a 1.5k per second burst for 200 s grows the pool's backlog by 300 a second to 60000",
          "[theory]") {
  const auto g = *Graph::build(burst_then_calm());
  const auto records = records_of(g, 2000);  // the 200 s of the burst
  const auto pool = *g.index_of("w");
  REQUIRE_THAT(records[0].nodes[pool].backlog, WithinAbs(30.0, 1e-6));        // 300/s x 0.1 s
  REQUIRE_THAT(records[999].nodes[pool].backlog, WithinAbs(30'000.0, 1e-3));  // t = 100 s
  const auto& peak = records[1999].nodes[pool];                               // t = 200 s
  REQUIRE_THAT(peak.backlog, WithinAbs(60'000.0, 1e-3));
  REQUIRE_THAT(peak.drain_per_s, WithinRel(1200.0, 1e-9));  // saturated: it drains its capacity
  REQUIRE(peak.dropped_per_s == 0.0);                       // the 7-day room has not filled
  REQUIRE_THAT(peak.demand_per_s, WithinRel(1500.0, 1e-9));
  // The publisher never notices: every message is appended and answered, in the same 0.6 ms.
  for (const auto i : {std::size_t{0}, std::size_t{999}, std::size_t{1999}}) {
    CAPTURE(i);
    const auto& answer = records[i].responses[0];
    REQUIRE_THAT(answer.answered.per_second, WithinRel(1500.0, 1e-9));
    REQUIRE_THAT(answer.answered.per_second, WithinRel(answer.offered.per_second, 1e-12));
    REQUIRE_THAT(answer.response.mean_s, WithinAbs(0.0006, 1e-9));
  }
}

TEST_CASE("after the burst the pool drains the backlog at 200 a second and empties at 500 s",
          "[theory]") {
  const auto g = *Graph::build(burst_then_calm());
  const auto records = records_of(g, 5000);  // 500 s
  const auto pool = *g.index_of("w");
  REQUIRE_THAT(records[1999].nodes[pool].backlog, WithinAbs(60'000.0, 1e-3));  // t = 200 s
  REQUIRE_THAT(records[2000].nodes[pool].demand_per_s, WithinRel(1000.0, 1e-9));
  REQUIRE_THAT(records[2000].nodes[pool].backlog, WithinAbs(59'980.0, 1e-3));  // - 200 x 0.1 s
  REQUIRE_THAT(records[3499].nodes[pool].backlog, WithinAbs(30'000.0, 1e-3));  // t = 350 s
  REQUIRE_THAT(records[3499].nodes[pool].drain_per_s, WithinRel(1200.0, 1e-9));
  REQUIRE_THAT(records[4999].nodes[pool].backlog, WithinAbs(0.0, 1e-3));  // t = 500 s
  for (std::size_t i = 2000; i < records.size(); ++i) {
    REQUIRE(records[i].nodes[pool].dropped_per_s == 0.0);
  }
}

// ---- Retention bounds the backlog: the pool drops what is older than the room ----
//
// The same 1500/s into the same 1200/s pool, but the queue keeps messages for 10 s: the room is
// retention x capacity = 10 s x 1200/s = 12,000 messages, full after 12,000 / 300 = 40 s (step
// 399 ends at t = 40 s); from then on the pool drops the 300/s that arrive beyond its drain
// ("deleted unconsumed"), and the publisher is still answered 1500/s: asynchronous completion.

namespace {
Scenario retained_for_10_s() {
  auto s = published(1500, {twelve_workers()}, 10s);
  s.duration = 100s;
  return s;
}
}  // namespace

TEST_CASE("with 10 s of retention the pool's room is 12000 messages and fills after 40 s",
          "[theory]") {
  const auto g = *Graph::build(retained_for_10_s());
  const auto pool = *g.index_of("w");
  REQUIRE_THAT(g.nodes()[pool].capacity.queue_limit, WithinRel(12'000.0, 1e-9));
  const auto records = records_of(g, 600);  // 60 s
  std::size_t first_drop = records.size();
  for (std::size_t i = 0; i < records.size(); ++i) {
    CAPTURE(i);
    REQUIRE(records[i].nodes[pool].backlog <= 12'000.0 + 1e-6);
    if (first_drop == records.size() && records[i].nodes[pool].dropped_per_s > 1e-6) first_drop = i;
  }
  // Step 399 ends at t = 40 s with 400 x 30 = 12,000 waiting: the first drop is the next step; one
  // step either way absorbs the rounding of the accumulated backlog.
  REQUIRE(first_drop >= 399);
  REQUIRE(first_drop <= 401);
  REQUIRE_THAT(records[199].nodes[pool].backlog, WithinAbs(6'000.0, 1e-3));  // t = 20 s
  REQUIRE(records[199].nodes[pool].dropped_per_s == 0.0);
}

TEST_CASE(
    "once the room is full the pool drops 300 a second while the publisher is still "
    "answered 1500",
    "[theory]") {
  const auto g = *Graph::build(retained_for_10_s());
  const auto records = records_of(g, 600);
  const auto& last = records.back();
  const auto& pool = last.nodes[*g.index_of("w")];
  REQUIRE_THAT(pool.dropped_per_s, WithinRel(300.0, 1e-6));  // inflow - capacity
  REQUIRE_THAT(pool.drain_per_s, WithinRel(1200.0, 1e-9));
  REQUIRE_THAT(pool.backlog, WithinAbs(12'000.0, 1e-6));
  REQUIRE_THAT(pool.rho, WithinRel(1.0, 1e-9));
  // Asynchronous completion: the pool's drops are its own; the broker dropped nothing and the
  // client is answered all 1500/s in 0.6 ms.
  REQUIRE(last.nodes[*g.index_of("q")].dropped_per_s == 0.0);
  REQUIRE_THAT(last.responses[0].answered.per_second, WithinRel(1500.0, 1e-9));
  REQUIRE_THAT(last.responses[0].answered.per_second / last.responses[0].offered.per_second,
               WithinRel(1.0, 1e-9));
  REQUIRE_THAT(last.responses[0].response.mean_s, WithinAbs(0.0006, 1e-9));
}

// estimate() is the long-run state: a pool fed above its capacity ends with its room full and
// drops what it cannot drain, inflow - capacity = 1500 - 1200 = 300/s.
TEST_CASE("estimate of a pool fed above capacity settles with a full room and drops the excess",
          "[theory]") {
  const auto g = *Graph::build(retained_for_10_s());
  const auto sim = Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
  const auto e = sim->estimate();
  REQUIRE(e.converged);
  const auto& pool = e.nodes[*g.index_of("w")];
  REQUIRE_THAT(pool.backlog, WithinAbs(12'000.0, 1e-6));
  REQUIRE_THAT(pool.drain_per_s, WithinRel(1200.0, 1e-9));
  REQUIRE_THAT(pool.dropped_per_s, WithinRel(300.0, 1e-6));
  REQUIRE_THAT(e.responses[0].answered.per_second, WithinRel(1500.0, 1e-9));
}

// Below capacity the long-run state is the state `run` reaches: constant rates, so the last step
// of a run equals it, for the pool, the broker and the publisher's response.
TEST_CASE("estimate and run agree on a queue and its pool below capacity", "[theory]") {
  const auto g = *Graph::build(published(1000, {four_workers()}));
  auto running = *Simulation::create(g);
  simsys::engine::StepRecord last;
  running.run({.duration = 2s, .step = 100ms},
              [&](const simsys::engine::StepRecord& r) { last = r; });
  const auto e = (*Simulation::create(g)).estimate();
  REQUIRE(e.converged);
  for (const auto* id : {"q", "w"}) {
    CAPTURE(id);
    const auto node = *g.index_of(id);
    REQUIRE_THAT(last.nodes[node].demand_per_s, WithinRel(e.nodes[node].demand_per_s, 1e-9));
    REQUIRE_THAT(last.nodes[node].rho, WithinRel(e.nodes[node].rho, 1e-9));
    REQUIRE_THAT(last.nodes[node].latency.mean_s, WithinRel(e.nodes[node].latency.mean_s, 1e-9));
    REQUIRE(e.nodes[node].dropped_per_s == 0.0);
    REQUIRE(e.nodes[node].backlog == 0.0);
  }
  REQUIRE_THAT(e.nodes[*g.index_of("w")].latency.mean_s, WithinAbs(0.006622, 1e-6));
  REQUIRE_THAT(last.responses[0].response.mean_s, WithinRel(e.responses[0].response.mean_s, 1e-9));
  REQUIRE_THAT(e.responses[0].response.mean_s, WithinAbs(0.0006, 1e-9));
}

// The fluid queue's balance at the broker and at the pool, every step of an overloaded run (the
// 10 s retention scenario, 60 s): what came in was drained, dropped or is waiting,
// demand.dt = (drain + dropped).dt + (backlog - the previous backlog).
TEST_CASE(
    "the pool conserves messages at every step: demand x dt = drained + dropped + change "
    "in backlog",
    "[theory]") {
  const auto g = *Graph::build(retained_for_10_s());
  const auto records = records_of(g, 600);
  const double dt = 0.1;
  for (const auto* id : {"q", "w"}) {
    CAPTURE(id);
    const auto node = *g.index_of(id);
    double previous_backlog = 0.0;
    bool grew = false;
    bool dropped = false;
    for (std::size_t i = 0; i < records.size(); ++i) {
      const auto& sample = records[i].nodes[node];
      CAPTURE(i);
      const double in = sample.demand_per_s * dt;
      const double out = (sample.drain_per_s + sample.dropped_per_s) * dt;
      REQUIRE_THAT(in, WithinAbs(out + (sample.backlog - previous_backlog), 1e-9));
      grew = grew || sample.backlog > previous_backlog;
      dropped = dropped || sample.dropped_per_s > 0.0;
      previous_backlog = sample.backlog;
    }
    // Not vacuous for the pool: its backlog really filled and its room really overflowed. The
    // broker, at rho 0.0015, never queues.
    REQUIRE(grew == (std::string_view{id} == "w"));
    REQUIRE(dropped == (std::string_view{id} == "w"));
  }
}

// ---- A worker pool's then steps carry each DRAINED job on (plan 2c refinement 5, Task 7) ----
//
// A pool's `then` steps are the work a finished job hands on, like a cache's unconditional `then`
// (book ch. 14: a transcoding worker writes its renditions to the transcoded store and publishes a
// completion message). Three rules, each pinned below:
//  - they fire per DRAINED job, what the pool served, not what arrived: a saturated pool leaves
//    the excess in its backlog (or drops it), and the excess starts no downstream work;
//  - a worker is busy for `cost` only, so a slow `then` target does not hold it: the pool's rho and
//    sojourn are M/M/c over `workers` and `cost` whatever it hands on to;
//  - nobody waits on the pool (the hop from its queue is asynchronous), so the then-hops add
//    nothing to any client's response, whether the steps are sequential or parallel.
//
// Numbers, from one picture: a client publishes `rate` messages/s to q (default queue: append
// 100 us, c = 100) over a 250 us link; pool w drains q with 10 workers at 1 s a job:
//   capacity = workers / cost = 10 / 1 s = 10 jobs/s       (Little's law: c / service time)
//   a        = 8/s x 1 s = 8 erlangs over 10 servers: rho = 0.8
//   Erlang C(10, a = 8) = 0.40918, Wq = C x s / (c (1 - rho)) = 0.40918 x 1 s / (10 x 0.2)
//            = 0.20459 s, sojourn = Wq + s = 1.20459 s   (service SCV 1: M/M/c exactly)
// w's then steps (zero-latency links, so a step's sends arrive in the same step):
//   {to: store, op: write, factor: 4, size: 50MB}: a blob store of 5 GB/s
//       (5000 MB/s, c = 5000 x 40 ms = 200 slots) receives 4 writes of 50 MB per job
//   {to: done_q, op: publish}: a message queue consumed by a second pool, w2 (100 workers at
//       10 ms = 10k jobs/s), receives one message per job
// The publisher's response is q's append and the round trip alone:
//   100 us (the append: rho 8e-6 at c = 100, no wait) + 2 x 250 us = 0.6 ms
// (Node indices: u 0, q 1, w 2, store 3, done_q 4, w2 5.)

namespace {
// The picture above: `rate` messages/s into w, whose `then` is `then`, and a store of
// `store_capacity` bytes/s.
Scenario worker_chain(double rate, std::vector<Step> then,
                      simsys::ByteRate store_capacity = simsys::ByteRate{5e9}) {
  auto pool = group_of("w", 10, 1s);
  pool.then = std::move(then);
  auto s = published(rate, {std::move(pool)});
  auto done_consumers = group_of("w2", 100, 10ms);
  done_consumers.consumes = "done_q";
  s.nodes.push_back(
      NodeSpec{.id = "store", .type = NodeType::blob_store, .byte_capacity = store_capacity});
  s.nodes.push_back(NodeSpec{.id = "done_q", .type = NodeType::message_queue});
  s.nodes.push_back(std::move(done_consumers));
  s.links.push_back(LinkSpec{.from = "w", .to = "store", .latency = 0s});
  s.links.push_back(LinkSpec{.from = "w", .to = "done_q", .latency = 0s});
  s.links.push_back(LinkSpec{.from = "done_q", .to = "w2", .latency = 0s});
  return s;
}

// A fresh simulation of `g`, failing the test with the engine's message when it refuses the
// scenario (instead of dereferencing the refusal).
Simulation simulation_of(const Graph& g) {
  auto sim = Simulation::create(g);
  if (!sim) FAIL(sim.error().message);
  return std::move(*sim);
}

// 4 renditions of 50 MB per job into the store, one completion message per job.
std::vector<Step> store_and_done() {
  return {Step{.to = "store", .op = "write", .factor = 4.0, .size = simsys::Bytes{50e6}},
          Step{.to = "done_q", .op = "publish"}};
}

const std::vector<Step> store_and_done_in_parallel{
    Step{.to = "store",
         .op = "write",
         .mode = simsys::model::StepMode::parallel,
         .factor = 4.0,
         .size = simsys::Bytes{50e6}},
    Step{.to = "done_q", .op = "publish", .mode = simsys::model::StepMode::parallel}};

// 10 workers at 1 s a job fed 8/s: Erlang C(10, 8 erlangs) x 1 s / (10 x 0.2) + 1 s.
constexpr double pool_sojourn_8_per_s = 1.20459;
}  // namespace

// 8 jobs/s drained x 4 writes x 50 MB = 1600 MB/s: the store counts megabytes, so its demand is
// 32 writes/s x 50 = 1600 units/s of 5000 (rho 0.32); done_q gets one message per job, 8/s, and
// hands each to w2.
TEST_CASE("a pool draining 8 jobs a second sends its blob store 32 writes a second: 1.6k MB",
          "[theory]") {
  const auto g = *Graph::build(worker_chain(8, store_and_done()));
  const auto r = first_step(g);
  const auto& pool = r.nodes[*g.index_of("w")];
  REQUIRE_THAT(pool.demand_per_s, WithinRel(8.0, 1e-9));
  REQUIRE_THAT(pool.rho, WithinRel(0.8, 1e-9));
  REQUIRE_THAT(r.nodes[*g.index_of("store")].demand_per_s, WithinRel(1600.0, 1e-9));
  REQUIRE_THAT(r.nodes[*g.index_of("store")].rho, WithinRel(0.32, 1e-9));
  REQUIRE_THAT(r.nodes[*g.index_of("done_q")].demand_per_s, WithinRel(8.0, 1e-9));
  REQUIRE_THAT(r.nodes[*g.index_of("w2")].demand_per_s, WithinRel(8.0, 1e-9));
  REQUIRE_THAT(pool.latency.mean_s, WithinAbs(pool_sojourn_8_per_s, 1e-4));
}

// Offered 12 jobs/s the pool drains 10 (its capacity) and queues the other 2/s: its backlog after
// 5 s (50 steps of 0.1 s) is 10 jobs. The then targets are sent what it DRAINED: 10 jobs x 4 x
// 50 MB = 2000 MB/s (40 writes/s) and 10 messages/s, not the 12 jobs/s (2400 MB/s, 12/s) that
// arrived. The room (7 days of retention x 10/s) is nowhere near full: nothing is dropped.
TEST_CASE("a saturated pool hands on what it drained, 10 jobs a second, not the 12 offered",
          "[theory]") {
  const auto g = *Graph::build(worker_chain(12, store_and_done()));
  const auto records = records_of(g, 50);
  const auto& last = records.back();
  const auto& pool = last.nodes[*g.index_of("w")];
  REQUIRE_THAT(pool.demand_per_s, WithinRel(12.0, 1e-9));
  REQUIRE_THAT(pool.drain_per_s, WithinRel(10.0, 1e-9));
  REQUIRE_THAT(pool.backlog, WithinAbs(10.0, 1e-6));  // (12 - 10) per second x 5 s
  REQUIRE(pool.dropped_per_s == 0.0);
  REQUIRE_THAT(last.nodes[*g.index_of("store")].demand_per_s, WithinRel(2000.0, 1e-9));
  REQUIRE_THAT(last.nodes[*g.index_of("done_q")].demand_per_s, WithinRel(10.0, 1e-9));
  REQUIRE_THAT(last.nodes[*g.index_of("w2")].demand_per_s, WithinRel(10.0, 1e-9));
}

// The long-run state says the same: the estimate settles with the pool saturated, so the targets'
// demand is 10 x 4 x 50 = 2000 MB/s and 10 messages/s however long the backlog has grown.
TEST_CASE("the estimate of a saturated pool sends its targets the drained 10 jobs a second",
          "[theory]") {
  const auto g = *Graph::build(worker_chain(12, store_and_done()));
  const auto sim = Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
  const auto e = sim->estimate();
  REQUIRE_THAT(e.nodes[*g.index_of("w")].drain_per_s, WithinRel(10.0, 1e-9));
  REQUIRE_THAT(e.nodes[*g.index_of("store")].demand_per_s, WithinRel(2000.0, 1e-9));
  REQUIRE_THAT(e.nodes[*g.index_of("done_q")].demand_per_s, WithinRel(10.0, 1e-9));
}

// The publisher is answered by q's append: 0.6 ms (see the picture), with or without the pool's
// then steps. The pool's own sojourn (1.2 s) and its hand-offs are nowhere in it, and all 8
// messages/s are answered.
TEST_CASE("the then hops add nothing to the publisher's response: 0.6 ms with or without them",
          "[theory]") {
  const auto with_then = *Graph::build(worker_chain(8, store_and_done()));
  const auto without_then = *Graph::build(published(8, {group_of("w", 10, 1s)}));
  const auto a = first_step(with_then);
  const auto b = first_step(without_then);
  REQUIRE_THAT(a.responses[0].response.mean_s, WithinAbs(0.0006, 1e-9));
  REQUIRE_THAT(a.responses[0].response.mean_s, WithinRel(b.responses[0].response.mean_s, 1e-12));
  REQUIRE_THAT(a.responses[0].answered.per_second, WithinRel(8.0, 1e-9));
  REQUIRE_THAT(a.responses[0].answered.per_second / a.responses[0].offered.per_second,
               WithinRel(1.0, 1e-9));
}

// A target that cannot keep up drops its own load and nobody upstream hears of it: the store here
// is 100 MB/s (c = 100 x 40 ms = 4 slots) fed 1600 MB/s, so in the long run it serves 100 and
// drops 1500 MB/s (1600 - 100), 93.75 % of what it is sent; the publisher is still answered 8/s
// in 0.6 ms, and the pool still drains its 8 jobs/s (it needs its workers for `cost` only).
TEST_CASE("a then target that drops most of its load leaves the publisher and the pool untouched",
          "[theory]") {
  const auto g = *Graph::build(worker_chain(8, store_and_done(), simsys::ByteRate{100e6}));
  const auto sim = Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
  const auto e = sim->estimate();
  const auto& store = e.nodes[*g.index_of("store")];
  REQUIRE_THAT(store.demand_per_s, WithinRel(1600.0, 1e-9));
  REQUIRE_THAT(store.drain_per_s, WithinRel(100.0, 1e-6));
  REQUIRE_THAT(store.dropped_per_s, WithinRel(1500.0, 1e-6));
  REQUIRE_THAT(e.nodes[*g.index_of("w")].drain_per_s, WithinRel(8.0, 1e-9));
  REQUIRE_THAT(e.responses[0].answered.per_second, WithinRel(8.0, 1e-9));
  REQUIRE_THAT(e.responses[0].response.mean_s, WithinAbs(0.0006, 1e-9));
}

// A worker is busy for `cost` only (plan 2c refinement 5, ignored: a worker blocked on a slow
// downstream call, phase 3's backlog): the pool hands a job on to a service that takes 5 s and
// serves 1/s, and its own rho (0.8) and sojourn (1.20459 s, M/M/10) are what they are without any
// then step at all.
TEST_CASE("a slow then target does not hold the workers: the pool stays M/M/10 at rho 0.8",
          "[theory]") {
  auto pool = group_of("w", 10, 1s);
  pool.then = {Step{.to = "slow", .op = "work"}};
  auto s = published(8, {std::move(pool)});
  s.nodes.push_back(NodeSpec{.id = "slow",
                             .type = NodeType::service,
                             .capacity = Rate{1},
                             .latency = 5s,
                             .advanced = {.queue_limit = 1}});
  s.links.push_back(LinkSpec{.from = "w", .to = "slow", .latency = 0s});
  const auto g = *Graph::build(s);
  const auto bare = *Graph::build(published(8, {group_of("w", 10, 1s)}));
  const auto r = first_step(g);
  const auto reference = first_step(bare);
  const auto& pool_sample = r.nodes[*g.index_of("w")];
  REQUIRE_THAT(pool_sample.rho, WithinRel(0.8, 1e-9));
  REQUIRE_THAT(pool_sample.latency.mean_s, WithinAbs(pool_sojourn_8_per_s, 1e-4));
  REQUIRE_THAT(pool_sample.latency.mean_s,
               WithinRel(reference.nodes[*bare.index_of("w")].latency.mean_s, 1e-12));
  REQUIRE_THAT(r.nodes[*g.index_of("slow")].demand_per_s, WithinRel(8.0, 1e-9));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(0.0006, 1e-9));
}

// Nobody waits for a pool, so how its steps are ordered means nothing to the client: a sequential
// group and a parallel one send the same load (1600 MB/s, 8/s) and leave the same 0.6 ms answer
// and the same pool sojourn. Both fire.
TEST_CASE("a sequential and a parallel then group fire the same and change nothing for the client",
          "[theory]") {
  const auto sequential = first_step(*Graph::build(worker_chain(8, store_and_done())));
  const auto g = *Graph::build(worker_chain(8, store_and_done_in_parallel));
  const auto parallel = first_step(g);
  REQUIRE_THAT(parallel.nodes[*g.index_of("store")].demand_per_s, WithinRel(1600.0, 1e-9));
  REQUIRE_THAT(parallel.nodes[*g.index_of("done_q")].demand_per_s, WithinRel(8.0, 1e-9));
  REQUIRE_THAT(parallel.responses[0].response.mean_s,
               WithinRel(sequential.responses[0].response.mean_s, 1e-12));
  REQUIRE_THAT(parallel.responses[0].response.mean_s, WithinAbs(0.0006, 1e-9));
  REQUIRE_THAT(parallel.nodes[*g.index_of("w")].latency.mean_s,
               WithinRel(sequential.nodes[*g.index_of("w")].latency.mean_s, 1e-12));
}

// Below capacity the long-run state is the state `run` reaches: constant rates, so the last step
// of a run equals the estimate, at the pool and at both targets (and for the publisher).
TEST_CASE("estimate and run agree on a pool with then steps below capacity", "[theory]") {
  const auto g = *Graph::build(worker_chain(8, store_and_done()));
  auto running = simulation_of(g);
  simsys::engine::StepRecord last;
  running.run({.duration = 2s, .step = 100ms},
              [&](const simsys::engine::StepRecord& r) { last = r; });
  const auto e = simulation_of(g).estimate();
  REQUIRE(e.converged);
  for (const auto* id : {"w", "store", "done_q", "w2"}) {
    CAPTURE(id);
    const auto node = *g.index_of(id);
    REQUIRE_THAT(last.nodes[node].demand_per_s, WithinRel(e.nodes[node].demand_per_s, 1e-9));
    REQUIRE_THAT(last.nodes[node].rho, WithinRel(e.nodes[node].rho, 1e-9));
    REQUIRE_THAT(last.nodes[node].latency.mean_s, WithinRel(e.nodes[node].latency.mean_s, 1e-9));
  }
  REQUIRE_THAT(e.nodes[*g.index_of("store")].demand_per_s, WithinRel(1600.0, 1e-9));
  REQUIRE_THAT(last.responses[0].response.mean_s, WithinRel(e.responses[0].response.mean_s, 1e-9));
}

// The fluid balance at the pool and at both targets, every step of the saturated run: what came
// in was drained, dropped or is waiting, demand.dt = (drain + dropped).dt + (backlog - previous).
// The pool's backlog really grows (2 jobs/s), so the law is not met vacuously there; the targets
// run below capacity, so theirs is the plain demand = drain.
TEST_CASE("a pool and its then targets conserve their load at every step of a saturated run",
          "[theory]") {
  const auto g = *Graph::build(worker_chain(12, store_and_done()));
  const auto records = records_of(g, 50);
  const double dt = 0.1;
  for (const auto* id : {"w", "store", "done_q"}) {
    CAPTURE(id);
    const auto node = *g.index_of(id);
    double previous_backlog = 0.0;
    bool grew = false;
    for (std::size_t i = 0; i < records.size(); ++i) {
      const auto& sample = records[i].nodes[node];
      CAPTURE(i);
      const double in = sample.demand_per_s * dt;
      const double out = (sample.drain_per_s + sample.dropped_per_s) * dt;
      REQUIRE_THAT(in, WithinAbs(out + (sample.backlog - previous_backlog), 1e-9));
      grew = grew || sample.backlog > previous_backlog;
      previous_backlog = sample.backlog;
    }
    REQUIRE(grew == (std::string_view{id} == "w"));
  }
}

// `on: miss` names a cache's miss, and a pool has no lookup to miss: refused at build with the
// reason (the message is pinned in test_graph).
TEST_CASE("an on-miss then step on a worker pool is refused", "[theory]") {
  auto s = worker_chain(8, {Step{.to = "done_q", .op = "publish", .on = StepCondition::miss}});
  const auto built = Graph::build(s);
  REQUIRE_FALSE(built.has_value());
  // C++23 std::string::contains: a substring test in one call, replacing
  // `find(...) != std::string::npos`; the exact wording is pinned in test_graph.
  REQUIRE(built.error().message.contains("node 'w': a worker pool's then steps"));
}

// ---- Queue lag: how far a message queue trails its consumers (plan 2c refinement 4, Task 8) ----
//
// A message queue's lag is how long a message published now waits until its consumer group reads
// it (Kafka's records-lag divided by the group's consume rate): the queue's own broker never waits
// (it appends at 1M/s), the messages wait in each group's pool, so the lag is read off the pool.
// For one group,
//   lag = max(backlog / drain, k x Wq)            (the max of the two: our heuristic, FluidQueue)
//   backlog / drain  the FIFO drain time: a message published now is served after everything
//                    ahead of it, which the pool drains at `drain` a second (the "fluid" part,
//                    exact when the pool is saturated, since then drain = capacity);
//   k x Wq           the stochastic part: Allen-Cunneen's scaling k of the M/M/c wait Wq (Erlang
//                    C) of the pool, what an arriving message waits even with no backlog because
//                    all c workers happen to be busy. k = 1 here: Poisson arrivals, exponential
//                    jobs. Erlang C is clamped at rho 0.99, as everywhere in the fluid model.
// and the queue's lag is its WORST group's: a consumer group that is far behind is a queue that
// is far behind. It is the wait only: the pool's own `latency` is wait + `cost`, the lag has no
// cost in it. (It is the same wait fluid::FluidQueue computes for the pool; Task 8 exposes it on
// the queue's sample, `NodeSample::queue_lag_s`, empty on every node that is not a message queue.)
//
// Numbers, with the Task 6 pictures above: 12 workers at 10 ms are 1200 jobs/s exactly, and the
// burst's 1500/s grows the backlog by 300/s = 30 per 100 ms step, so after step i (the record
// whose t ends at (i + 1) x 0.1 s) the backlog is (i + 1) x 30 messages and the fluid lag
//   (i + 1) x 30 / 1200 = (i + 1) x 0.025 s            (50 s at i = 1999, t = 200 s: 60,000 / 1200)
// The stochastic part of a SATURATED pool (rho 1, clamped to 0.99 for Erlang C) is
//   Wq(c = 12, rho = 0.99, s = 10 ms) = C(12, 11.88) x 10 ms / (12 x 0.01) = 0.0800 s
// so the first steps (fluid lag 0.025, 0.05, 0.075 s) report that 80 ms floor, and from step 3
// (0.1 s) on the backlog is the larger term. After the backlog drains (rho 1000/1200 = 0.8333)
// only the stochastic part is left: Wq(12, 0.8333, 10 ms) = 2.247 ms.

namespace {
// The lag the queue `q` reports in `record`; the test fails when it reports none.
double queue_lag_of(const simsys::engine::StepRecord& record, const Graph& g) {
  const auto& lag = record.nodes[*g.index_of("q")].queue_lag_s;
  REQUIRE(lag.has_value());
  return *lag;
}

// 12 workers at 10 ms: Erlang C of a saturated pool, rho clamped to 0.99
// (fluid::max_stochastic_rho).
double saturated_floor_s() {
  return simsys::queueing::mmc_wait(12, 0.99, 0.01).mean_s;
}
}  // namespace

TEST_CASE("the burst leaves the queue 50 s behind at t = 200 s: 60000 messages over 1200 a second",
          "[theory]") {
  const auto g = *Graph::build(burst_then_calm());
  const auto records = records_of(g, 2000);  // the 200 s of the burst
  REQUIRE_THAT(queue_lag_of(records[1999], g), WithinRel(50.0, 1e-6));
  REQUIRE_THAT(queue_lag_of(records[999], g), WithinRel(25.0, 1e-6));  // t = 100 s: 30,000 / 1200
  // The peak of the whole burst is its last step: the lag never falls while the pool is behind.
  double peak = 0.0;
  for (const auto& record : records) peak = std::max(peak, queue_lag_of(record, g));
  REQUIRE_THAT(peak, WithinRel(50.0, 1e-6));
}

TEST_CASE(
    "every step of the burst reports the larger of the 80 ms Erlang wait and backlog over drain",
    "[theory]") {
  const auto g = *Graph::build(burst_then_calm());
  const auto records = records_of(g, 2000);
  REQUIRE_THAT(saturated_floor_s(), WithinAbs(0.0800, 1e-4));  // the napkin number
  for (std::size_t i = 0; i < records.size(); ++i) {
    CAPTURE(i);
    const double backlog_over_drain = static_cast<double>(i + 1) * 30.0 / 1200.0;
    REQUIRE_THAT(queue_lag_of(records[i], g),
                 WithinAbs(std::max(saturated_floor_s(), backlog_over_drain), 1e-6));
  }
  // Steps 0 to 2 are the stochastic floor (0.025, 0.05, 0.075 s of backlog are under it); step 3
  // is the first with the fluid part larger: 0.1 s.
  REQUIRE_THAT(queue_lag_of(records[2], g), WithinAbs(saturated_floor_s(), 1e-6));
  REQUIRE_THAT(queue_lag_of(records[3], g), WithinAbs(0.1, 1e-6));
}

// After the burst the pool drains 200 a second faster than the inflow: the lag falls with the
// backlog, 60,000 - 200 x 100 s = 40,000 at t = 300 s (33.33 s), 30,000 at t = 350 s (25 s), and
// once the backlog is gone only the stochastic wait of an 0.8333-loaded pool is left.
TEST_CASE("after the burst the lag falls with the backlog back to the pool's stochastic wait",
          "[theory]") {
  const auto g = *Graph::build(burst_then_calm());
  const auto records = records_of(g, 5100);  // 510 s: the backlog is empty from t = 500 s
  REQUIRE_THAT(queue_lag_of(records[2999], g), WithinRel(40'000.0 / 1200.0, 1e-6));  // t = 300 s
  REQUIRE_THAT(queue_lag_of(records[3499], g), WithinRel(25.0, 1e-6));               // t = 350 s
  const auto& pool = records[5099].nodes[*g.index_of("w")];
  REQUIRE_THAT(pool.rho, WithinRel(1000.0 / 1200.0, 1e-9));
  const double wait = simsys::queueing::mmc_wait(12, pool.rho, 0.01).mean_s;
  REQUIRE_THAT(wait, WithinAbs(0.002247, 1e-6));  // Wq(12, 0.8333, 10 ms) = 2.247 ms
  REQUIRE_THAT(queue_lag_of(records[5099], g), WithinRel(wait, 1e-9));
}

// The steady lag of the Task 6 picture (1000/s into 4 workers at 3.333 ms, rho 0.8333, no
// backlog) is the pool's Erlang wait alone, 3.289 ms: the lag leaves out the 3.333 ms of `cost`
// that the pool's 6.622 ms sojourn includes.
TEST_CASE("a queue with no backlog is lagging by the pool's stochastic wait: 3.289 ms",
          "[theory]") {
  const auto g = *Graph::build(published(1000, {four_workers()}));
  const auto r = first_step(g);
  REQUIRE_THAT(queue_lag_of(r, g), WithinAbs(0.003289, 1e-6));
  const auto& pool = r.nodes[*g.index_of("w")];
  REQUIRE_THAT(queue_lag_of(r, g), WithinAbs(pool.latency.mean_s - pool_cost_s, 1e-9));
}

// One queue, two consumer groups, each receiving every message: the lag is the slower group's.
// "fast" is 10 workers at 1 ms (10,000 jobs/s: rho 0.15 at the burst's 1500/s, no backlog, a wait
// of about 1e-11 s), "slow" the burst's 12 workers at 10 ms; the queue is 50 s behind whichever
// order they are declared in, never the fast group's 0 and not a sum.
TEST_CASE("a queue with two consumer groups reports the slower group's lag", "[theory]") {
  const auto lag_at_peak = [](std::vector<NodeSpec> groups) {
    auto s = published(1000, std::move(groups));
    s.nodes[0].workload->curve.spikes = {
        simsys::workload::Spike{.at = 0s, .duration = 200s, .factor = 1.5}};
    const auto g = *Graph::build(s);
    const auto records = records_of(g, 2000);
    return queue_lag_of(records[1999], g);
  };
  const auto fast = [] { return group_of("fast", 10, 1ms); };
  const auto slow = [] { return group_of("slow", 12, 10ms); };
  REQUIRE_THAT(lag_at_peak({fast(), slow()}), WithinRel(50.0, 1e-6));
  REQUIRE_THAT(lag_at_peak({slow(), fast()}), WithinRel(50.0, 1e-6));
}

// The same maximum over the stochastic parts alone (no backlog): `w` waits 3.289 ms (above) and
// `w2`, 10 workers at 5 ms at 1000/s (a = 5 erlangs, rho 0.5), 0.036 ms: the queue's lag is w's.
TEST_CASE("with no backlog the queue's lag is the larger of its groups' Erlang waits", "[theory]") {
  const auto g = *Graph::build(published(1000, {four_workers(), group_of("w2", 10, 5ms)}));
  const auto r = first_step(g);
  const double w_wait = r.nodes[*g.index_of("w")].latency.mean_s - pool_cost_s;
  const double w2_wait = r.nodes[*g.index_of("w2")].latency.mean_s - 0.005;
  REQUIRE_THAT(w_wait, WithinAbs(0.003289, 1e-6));
  REQUIRE_THAT(w2_wait, WithinAbs(0.0000361, 1e-7));  // Wq(10, 0.5, 5 ms) = 36.1 us
  REQUIRE_THAT(queue_lag_of(r, g), WithinRel(w_wait, 1e-9));
}

// A pool saturated for the whole run (1500/s into 1200/s, room = retention 10 s x 1200 = 12,000
// messages): the backlog grows 30 a step to the room at step 399 (t = 40 s) and the lag with it,
// max(0.08 s floor, (i + 1) x 0.025 s): 2.5 s at t = 10 s, 5 s at t = 20 s, then the full room's
// drain time, 12,000 / 1200 = retention = 10 s, where it stays: a message older than `retention`
// is deleted, so a queue's lag cannot pass its retention.
TEST_CASE("a pool saturated for the whole run reports a lag that grows to the retention and stays",
          "[theory]") {
  const auto g = *Graph::build(retained_for_10_s());
  const auto records = records_of(g, 600);                            // 60 s
  REQUIRE_THAT(queue_lag_of(records[99], g), WithinAbs(2.5, 1e-6));   // t = 10 s: 3,000 / 1200
  REQUIRE_THAT(queue_lag_of(records[199], g), WithinAbs(5.0, 1e-6));  // t = 20 s: 6,000 / 1200
  double previous = 0.0;
  for (std::size_t i = 0; i < records.size(); ++i) {
    CAPTURE(i);
    const double lag = queue_lag_of(records[i], g);
    REQUIRE(lag >= previous);     // it never falls while the inflow exceeds the capacity
    REQUIRE(lag <= 10.0 + 1e-6);  // and never passes the retention
    if (i < 400) {
      const double backlog_over_drain = static_cast<double>(i + 1) * 30.0 / 1200.0;
      REQUIRE_THAT(lag, WithinAbs(std::max(saturated_floor_s(), backlog_over_drain), 1e-6));
    } else {
      REQUIRE_THAT(lag, WithinAbs(10.0, 1e-6));  // the room is full from step 399 on
    }
    previous = lag;
  }
}

// Only a message queue has a lag: the client that publishes and the pool that drains report
// none (`queue_lag_s` is empty, not 0), in a run and in estimate().
TEST_CASE("queue_lag_s is present on the message queue and absent on the nodes around it",
          "[theory]") {
  const auto g = *Graph::build(published(1000, {four_workers()}));
  const auto r = first_step(g);
  REQUIRE(r.nodes[*g.index_of("q")].queue_lag_s.has_value());
  REQUIRE_FALSE(r.nodes[*g.index_of("u")].queue_lag_s.has_value());
  REQUIRE_FALSE(r.nodes[*g.index_of("w")].queue_lag_s.has_value());
  const auto e = (*Simulation::create(g)).estimate();
  REQUIRE(e.nodes[*g.index_of("q")].queue_lag_s.has_value());
  REQUIRE_FALSE(e.nodes[*g.index_of("u")].queue_lag_s.has_value());
  REQUIRE_FALSE(e.nodes[*g.index_of("w")].queue_lag_s.has_value());
}

// estimate() is the long-run state: below capacity the 3.289 ms Erlang wait, and for a pool fed
// above its capacity the settled room, 12,000 messages over 1200 a second = 10 s.
TEST_CASE("estimate reports the steady lag: 3.289 ms below capacity, the retention above it",
          "[theory]") {
  const auto steady = *Graph::build(published(1000, {four_workers()}));
  const auto e = (*Simulation::create(steady)).estimate();
  REQUIRE(e.converged);
  REQUIRE_THAT(*e.nodes[*steady.index_of("q")].queue_lag_s, WithinAbs(0.003289, 1e-6));

  const auto overloaded = *Graph::build(retained_for_10_s());
  const auto settled = (*Simulation::create(overloaded)).estimate();
  REQUIRE(settled.converged);
  REQUIRE_THAT(*settled.nodes[*overloaded.index_of("q")].queue_lag_s, WithinAbs(10.0, 1e-6));
}

// ---- An external sink answers slowly and fails a share of what it serves (plan 2c refinement 8,
// Task 9) ----
//
// A third party's API (book ch. 10: APNs, FCM, an SMS provider) is a queue of the type's numbers
// whose server sometimes answers with a 5xx. The numbers, from one picture: a client sends `rate`
// calls/s over a 250 us one-way link to an external sink with the type's defaults (spec section 6,
// `third_party_api`):
//   capacity 1k/s       (chosen: the quota a provider grants one account)
//   service time 100 ms and service SCV 2.0   (spec section 6: slow and erratic, more irregular
//                       than the exponential's 1)
//   c = capacity x service time = 1000/s x 100 ms = 100 servers       (Little's law)
// and `failure_rate` f = 0.02: the share of what the sink SERVES that fails. At 500 calls/s:
//   rho            = 500 / 1000 = 0.5          (the sink works on a failing call as long as on
//                                               any other: rho counts what it served, 500, not
//                                               the 490 that succeeded)
//   Erlang C(100, 0.5) = 3.26e-10              (the chance a call finds all 100 servers busy: no
//                                               wait to speak of)
//   sojourn mean   = Wq + s = ~0 + 100 ms = 100 ms
//   sojourn var    = SCV x s^2 = 2.0 x (0.1 s)^2 = 0.02 s^2          (the wait's variance ~1e-20)
//   served         = 500/s;  dropped = f x served = 0.02 x 500 = 10/s;  drain = (1 - f) x served
//                  = 490/s;  demand = drain + dropped = 500 = 490 + 10, so nothing is lost from
//                  the books
//   delivered      = 1 - (dropped + rejected) / inflow = 1 - 10 / 500 = 0.98: answered 490/s
//   the caller     = 100 ms + the link's round trip 2 x 250 us = 100.5 ms
// A failure is a DROP (the node failing), never a rejection (a policy working): rejected stays 0.
// (Node indices: u 0, api 1.)

namespace {
// u (`rate`/s of `call`, `link` one way) -> api, an external sink that fails `failure_rate` of
// what it serves, with `queue_limit` units of waiting room when given (else the type's 1000).
Scenario sink_called(double rate, double failure_rate, simsys::SimTime link = 250us,
                     std::optional<double> queue_limit = std::nullopt) {
  Scenario s{.name = "sink-called", .version = 1, .duration = 10s};
  NodeSpec api{
      .id = "api", .type = NodeType::external_sink, .advanced = {.queue_limit = queue_limit}};
  api.failure_rate = failure_rate;
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{rate}}, .op_shares = {{"call", 1.0}}}},
      std::move(api)};
  s.links = {LinkSpec{.from = "u", .to = "api", .latency = link}};
  return s;
}
}  // namespace

TEST_CASE("500 calls a second into a sink that fails 2 percent are answered 490 a second",
          "[theory]") {
  const auto g = *Graph::build(sink_called(500, 0.02));
  const auto r = first_step(g);
  const auto& api = r.nodes[*g.index_of("api")];
  REQUIRE_THAT(api.demand_per_s, WithinRel(500.0, 1e-9));
  REQUIRE_THAT(api.drain_per_s, WithinRel(490.0, 1e-9));   // (1 - 0.02) x 500
  REQUIRE_THAT(api.dropped_per_s, WithinRel(10.0, 1e-9));  // 0.02 x 500
  REQUIRE(api.rejected_per_s == 0.0);                      // a failure is a drop, not a policy
  REQUIRE_THAT(api.backlog, WithinAbs(0.0, 1e-9));
  REQUIRE_THAT(api.rho, WithinRel(0.5, 1e-9));  // it served 500 of its 1000/s
  REQUIRE(r.responses.size() == 1);
  REQUIRE_THAT(r.responses[0].offered.per_second, WithinRel(500.0, 1e-9));
  REQUIRE_THAT(r.responses[0].answered.per_second, WithinRel(490.0, 1e-9));
}

// The Erlang C value itself, so "no wait to speak of" is not taken on trust (erlang_c takes the
// per-server utilisation): 3.26e-10 at c = 100 and rho 0.5.
TEST_CASE("the chance a call waits for one of the sink's 100 servers at rho 0.5 is 3.3e-10",
          "[theory]") {
  REQUIRE_THAT(simsys::queueing::erlang_c(100, 0.5), WithinRel(3.26e-10, 1e-2));
}

TEST_CASE("a sink's sojourn is 100 ms with variance 0.02 and the caller waits 100.5 ms",
          "[theory]") {
  const auto g = *Graph::build(sink_called(500, 0.02));
  const auto r = first_step(g);
  const auto& api = r.nodes[*g.index_of("api")];
  REQUIRE_THAT(api.latency.mean_s, WithinRel(0.1, 1e-6));        // the wait is ~1e-12 s
  REQUIRE_THAT(api.latency.variance_s2, WithinRel(0.02, 1e-6));  // SCV 2.0 x (0.1 s)^2
  // Failures do not shorten or lengthen the answer: the caller's mean is the sojourn plus the
  // round trip, 100 ms + 2 x 250 us, with the same variance (a link adds none).
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(0.1005, 1e-6));
  REQUIRE_THAT(r.responses[0].response.variance_s2, WithinRel(0.02, 1e-6));
}

// Over a run the report's drop rate is Sum dropped / Sum demand = 10 / 500 = 0.02 at the sink and
// its reject rate 0: the failures show up where an SLO `drop_rate` looks, not under `reject_rate`.
TEST_CASE("the report counts the sink's failures in its drop rate, 0.02, and none as rejections",
          "[theory]") {
  const auto g = *Graph::build(sink_called(500, 0.02));
  simsys::report::Summary summary{g, 0s};
  simulation_of(g).run({.duration = 5s, .step = 100ms},
                       [&](const simsys::engine::StepRecord& r) { summary.observe(r); });
  REQUIRE_THAT(summary.drop_rate("api"), WithinRel(0.02, 1e-9));
  REQUIRE(summary.reject_rate("api") == 0.0);
  REQUIRE_THAT(summary.drop_rate(), WithinRel(0.02, 1e-9));  // the worst node, here the only one
}

// A boundary: a sink that fails everything still works on every call (rho 0.5 at 500/s), drains
// nothing and answers nothing: demand 500 = drain 0 + dropped 500.
TEST_CASE("a sink that fails every call drains nothing, drops all 500 and answers none",
          "[theory]") {
  const auto g = *Graph::build(sink_called(500, 1.0));
  const auto r = first_step(g);
  const auto& api = r.nodes[*g.index_of("api")];
  REQUIRE_THAT(api.drain_per_s, WithinAbs(0.0, 1e-9));
  REQUIRE_THAT(api.dropped_per_s, WithinRel(500.0, 1e-9));
  REQUIRE_THAT(api.rho, WithinRel(0.5, 1e-9));
  REQUIRE_THAT(r.responses[0].answered.per_second, WithinAbs(0.0, 1e-9));
}

// demand x dt = (drain + dropped) x dt + (backlog - previous backlog), every step, below
// capacity: the failures move flow from `drain` to `dropped`, they do not make any disappear.
TEST_CASE("a sink conserves its calls at every step: failures move drain to dropped", "[theory]") {
  const auto g = *Graph::build(sink_called(500, 0.02));
  const auto records = records_of(g, 30);
  const auto api = *g.index_of("api");
  const double dt = 0.1;
  double previous_backlog = 0.0;
  for (std::size_t i = 0; i < records.size(); ++i) {
    const auto& sample = records[i].nodes[api];
    CAPTURE(i);
    REQUIRE_THAT(sample.dropped_per_s, WithinRel(10.0, 1e-9));  // not vacuous: it fails each step
    const double in = sample.demand_per_s * dt;
    const double out = (sample.drain_per_s + sample.dropped_per_s) * dt;
    REQUIRE_THAT(in, WithinAbs(out + (sample.backlog - previous_backlog), 1e-9));
    previous_backlog = sample.backlog;
  }
}

// With no failures a sink is exactly the queue it was: the same samples, bit for bit, as a service
// given the sink's numbers (1k/s, 100 ms, service SCV 2.0) behind the same link.
TEST_CASE("a sink that fails nothing is the plain queue: samples identical to a service",
          "[theory]") {
  auto service = sink_called(500, 0.0);
  service.nodes[1] = NodeSpec{.id = "api",
                              .type = NodeType::service,
                              .capacity = Rate{1000},
                              .latency = 100ms,
                              .advanced = {.service_variability = 2.0}};
  const auto sink_g = *Graph::build(sink_called(500, 0.0));
  const auto service_g = *Graph::build(service);
  const auto sink_records = records_of(sink_g, 20);
  const auto service_records = records_of(service_g, 20);
  for (std::size_t i = 0; i < sink_records.size(); ++i) {
    CAPTURE(i);
    const auto& a = sink_records[i].nodes[1];
    const auto& b = service_records[i].nodes[1];
    REQUIRE(a.demand_per_s == b.demand_per_s);
    REQUIRE(a.drain_per_s == b.drain_per_s);
    REQUIRE(a.dropped_per_s == b.dropped_per_s);
    REQUIRE(a.rejected_per_s == b.rejected_per_s);
    REQUIRE(a.backlog == b.backlog);
    REQUIRE(a.rho == b.rho);
    REQUIRE(a.latency == b.latency);
    REQUIRE(a.zone == b.zone);
    REQUIRE(sink_records[i].responses[0].answered == service_records[i].responses[0].answered);
    REQUIRE(sink_records[i].responses[0].response == service_records[i].responses[0].response);
  }
}

// Over capacity the sink sheds the overflow AND fails f of what it served. 1200 calls/s into
// 1000/s with a room of 10 (queue_limit), steps of 0.1 s, f = 0.02:
//   step 0: the queue serves min(1200, 1000) = 1000; the backlog would grow (1200 - 1000) x 0.1 =
//           20 and the room holds 10, so (20 - 10) / 0.1 = 100/s overflows. Failures: 0.02 x 1000 =
//           20/s. dropped = 100 + 20 = 120, drain = 980, backlog 10:
//           demand x dt = 120 = (980 + 120) x 0.1 + 10.
//   later:  the 10 waiting plus 1200 arrive, 1000 are served, the backlog would reach 10 + 20 = 30,
//           (30 - 10) / 0.1 = 200/s overflows: dropped = 200 + 20 = 220, drain = 980, the backlog
//           stays 10: 1200 = 980 + 220.
// The delivered share is 1 - 220 / 1200 = 0.8167: 980 of the 1200 offered are answered, which is
// the drain, and rho = 1: the sink serves 1000/s of 1000.
TEST_CASE("a sink over capacity drops the overflow plus 2 percent of what it served", "[theory]") {
  const auto g = *Graph::build(sink_called(1200, 0.02, 0s, 10.0));
  const auto records = records_of(g, 30);
  const auto api = *g.index_of("api");
  const auto& first = records[0].nodes[api];
  REQUIRE_THAT(first.drain_per_s, WithinRel(980.0, 1e-9));
  REQUIRE_THAT(first.dropped_per_s, WithinRel(120.0, 1e-9));
  REQUIRE_THAT(first.backlog, WithinAbs(10.0, 1e-9));
  for (std::size_t i = 1; i < records.size(); ++i) {
    const auto& sample = records[i].nodes[api];
    CAPTURE(i);
    REQUIRE_THAT(sample.demand_per_s, WithinRel(1200.0, 1e-9));
    REQUIRE_THAT(sample.drain_per_s, WithinRel(980.0, 1e-9));
    REQUIRE_THAT(sample.dropped_per_s, WithinRel(220.0, 1e-9));
    REQUIRE(sample.rejected_per_s == 0.0);
    REQUIRE_THAT(sample.backlog, WithinAbs(10.0, 1e-9));
    REQUIRE_THAT(sample.rho, WithinRel(1.0, 1e-9));
    REQUIRE_THAT(records[i].responses[0].answered.per_second, WithinRel(980.0, 1e-9));
  }
}

TEST_CASE("an overloaded sink conserves its calls at every step, failures included", "[theory]") {
  const auto g = *Graph::build(sink_called(1200, 0.02, 0s, 10.0));
  const auto records = records_of(g, 30);
  const auto api = *g.index_of("api");
  const double dt = 0.1;
  double previous_backlog = 0.0;
  bool grew = false;
  for (std::size_t i = 0; i < records.size(); ++i) {
    const auto& sample = records[i].nodes[api];
    CAPTURE(i);
    const double in = sample.demand_per_s * dt;
    const double out = (sample.drain_per_s + sample.dropped_per_s) * dt;
    REQUIRE_THAT(in, WithinAbs(out + (sample.backlog - previous_backlog), 1e-9));
    grew = grew || sample.backlog > previous_backlog;
    previous_backlog = sample.backlog;
  }
  REQUIRE(grew);  // the room really filled (at step 0): the law is not met vacuously
}

// The long-run state of the overloaded sink is the run's steady state: the settled backlog is the
// room, the overflow 200/s and the failures 20/s, so drain 980, dropped 220, answered 980.
TEST_CASE("the estimate of an overloaded sink settles at drain 980 and dropped 220", "[theory]") {
  const auto g = *Graph::build(sink_called(1200, 0.02, 0s, 10.0));
  const auto e = simulation_of(g).estimate();
  const auto& api = e.nodes[*g.index_of("api")];
  REQUIRE_THAT(api.drain_per_s, WithinRel(980.0, 1e-9));
  REQUIRE_THAT(api.dropped_per_s, WithinRel(220.0, 1e-9));
  REQUIRE_THAT(e.responses[0].answered.per_second, WithinRel(980.0, 1e-9));
}

// Below capacity the long-run state is what `run` reaches: constant rates, so the last step of a
// run equals the estimate (the failures are the same 2 % of what is served in both).
TEST_CASE("estimate and run agree on a sink that fails 2 percent below capacity", "[theory]") {
  const auto g = *Graph::build(sink_called(500, 0.02));
  simsys::engine::StepRecord last;
  simulation_of(g).run({.duration = 5s, .step = 100ms},
                       [&](const simsys::engine::StepRecord& r) { last = r; });
  const auto e = simulation_of(g).estimate();
  REQUIRE(e.converged);
  const auto api = *g.index_of("api");
  REQUIRE_THAT(e.nodes[api].drain_per_s, WithinRel(490.0, 1e-9));
  REQUIRE_THAT(e.nodes[api].dropped_per_s, WithinRel(10.0, 1e-9));
  REQUIRE_THAT(last.nodes[api].demand_per_s, WithinRel(e.nodes[api].demand_per_s, 1e-9));
  REQUIRE_THAT(last.nodes[api].drain_per_s, WithinRel(e.nodes[api].drain_per_s, 1e-9));
  REQUIRE_THAT(last.nodes[api].dropped_per_s, WithinRel(e.nodes[api].dropped_per_s, 1e-9));
  REQUIRE(last.nodes[api].rejected_per_s == e.nodes[api].rejected_per_s);
  REQUIRE_THAT(last.nodes[api].rho, WithinRel(e.nodes[api].rho, 1e-9));
  REQUIRE_THAT(last.nodes[api].latency.mean_s, WithinRel(e.nodes[api].latency.mean_s, 1e-9));
  REQUIRE_THAT(last.nodes[api].latency.variance_s2,
               WithinRel(e.nodes[api].latency.variance_s2, 1e-9));
  REQUIRE_THAT(last.responses[0].answered.per_second,
               WithinRel(e.responses[0].answered.per_second, 1e-9));
  REQUIRE_THAT(e.responses[0].answered.per_second, WithinRel(490.0, 1e-9));
  REQUIRE_THAT(last.responses[0].response.mean_s, WithinRel(e.responses[0].response.mean_s, 1e-9));
}

// ---- A worker pool that calls a sink loses f of those calls and nobody upstream hears of it ----
//
// A publisher sends 100 messages/s to q (the default queue) over a 250 us link; pool w (4 workers
// at 10 ms = 400 jobs/s, rho 0.25) drains q and hands each job on to the sink `api` ({to: api,
// op: send}, a zero-latency link). The pool is asynchronous (nobody waits on a pool: the hop from
// the queue into it is asynchronous), so the sink's failures fail no publisher's request:
//   api: demand 100/s, served 100, dropped 0.02 x 100 = 2/s, drain 98/s
//   w:   drains its 100 jobs/s whatever the sink does: a worker is busy for `cost` only
//   the publisher: answered 100/s in 0.6 ms (the append's 100 us + the 0.5 ms round trip), exactly
//   as with a sink that never fails.
// (Node indices: u 0, q 1, w 2, api 3.)
namespace {
Scenario pool_calling_sink(double failure_rate) {
  auto pool = group_of("w", 4, 10ms);
  pool.then = {Step{.to = "api", .op = "send"}};
  auto s = published(100, {std::move(pool)});
  NodeSpec api{.id = "api", .type = NodeType::external_sink};
  api.failure_rate = failure_rate;
  s.nodes.push_back(std::move(api));
  s.links.push_back(LinkSpec{.from = "w", .to = "api", .latency = 0s});
  return s;
}
}  // namespace

TEST_CASE("a pool that calls a 2 percent sink loses 2 of 100 calls a second and no publisher's",
          "[theory]") {
  const auto g = *Graph::build(pool_calling_sink(0.02));
  const auto reference = *Graph::build(pool_calling_sink(0.0));
  const auto r = first_step(g);
  const auto base = first_step(reference);
  const auto& api = r.nodes[*g.index_of("api")];
  REQUIRE_THAT(api.demand_per_s, WithinRel(100.0, 1e-9));
  REQUIRE_THAT(api.dropped_per_s, WithinRel(2.0, 1e-9));
  REQUIRE_THAT(api.drain_per_s, WithinRel(98.0, 1e-9));
  const auto& pool = r.nodes[*g.index_of("w")];
  REQUIRE_THAT(pool.drain_per_s, WithinRel(100.0, 1e-9));
  REQUIRE(pool.dropped_per_s == 0.0);
  // The publisher: answered 100/s in 0.6 ms, and identical to the run with a sink that never fails.
  REQUIRE_THAT(r.responses[0].answered.per_second, WithinRel(100.0, 1e-9));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(0.0006, 1e-9));
  REQUIRE_THAT(r.responses[0].answered.per_second,
               WithinRel(base.responses[0].answered.per_second, 1e-12));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(base.responses[0].response.mean_s, 1e-12));
  // The estimate says the same.
  const auto e = simulation_of(g).estimate();
  REQUIRE_THAT(e.nodes[*g.index_of("api")].dropped_per_s, WithinRel(2.0, 1e-9));
  REQUIRE_THAT(e.nodes[*g.index_of("api")].drain_per_s, WithinRel(98.0, 1e-9));
  REQUIRE_THAT(e.responses[0].answered.per_second, WithinRel(100.0, 1e-9));
}

// ---- kv_cluster in the steady state (plan 2c refinement 9, Task 10; book ch. 6, Dynamo) ----
//
// `nodes` machines of `capacity` each on a consistent-hash ring; the coordinator sends every
// read and write to the key's N replicas (ch. 6 "quorum consensus"), so the cluster is sent N
// times the requests. The sample counts that load in REPLICA-REQUESTS: `demand_per_s`,
// `drain_per_s` and `dropped_per_s` are N x the client-level requests (the unit the nodes'
// queues work in, so the balance demand = drain + dropped + d(backlog) holds as everywhere),
// while the clients' offered and answered rates stay in requests. With N < nodes the hottest
// machine carries k = 1 + z_M/sqrt(N vnodes) x the average (ring_factor), as hot and cool copy
// groups. Since the review of Tasks 9-10 (C-1, I-1) a request's answer is taken over its REAL
// replica set: with probability p_hot = N k / M its N replicas are the hot machine and N - 1 cool
// ones, otherwise N cool ones; a read is answered at the R-th fastest reply, a write at the W-th,
// each machine's sojourn a lognormal fitted to its own mean and variance
// (queueing::lognormal_order_statistic), and the request is lost when fewer than q of its N
// replica-requests come back (queueing::probability_fewer_than).

namespace {
// u(reads + writes per second) -> store, a kv_cluster of `nodes` machines, `replication` N copies
// per key and the quorums W and R. Every node has `capacity` (default 10k/s) and a service time
// `latency` (default 1 ms, so c = 10 servers per node by Little's law), the link has zero
// latency: a response is the cluster's own. Node indices: u 0, store 1. Ops are `read` and
// `write` (the default write op), left out of the client's mix when their rate is 0.
struct KvFront {
  int nodes{};
  int replication{};
  int write_quorum{};
  int read_quorum{};
  double reads{};
  double writes{};
  double capacity{10'000};
  simsys::SimTime latency{1ms};
  std::optional<int> vnodes{};
};

Scenario kv_front(const KvFront& shape) {
  Scenario s{.name = "kv-front", .version = 1, .duration = 10s};
  std::vector<std::pair<std::string, double>> shares;
  const double total = shape.reads + shape.writes;
  if (shape.reads > 0) shares.emplace_back("read", shape.reads / total);
  if (shape.writes > 0) shares.emplace_back("write", shape.writes / total);
  NodeSpec store{.id = "store",
                 .type = NodeType::kv_cluster,
                 .capacity = Rate{shape.capacity},
                 .latency = shape.latency};
  store.kv_nodes = shape.nodes;
  store.replication = shape.replication;
  store.write_quorum = shape.write_quorum;
  store.read_quorum = shape.read_quorum;
  store.vnodes = shape.vnodes;
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload =
                          Workload{.curve = {.base = Rate{total}}, .op_shares = std::move(shares)}},
             std::move(store)};
  s.links = {LinkSpec{.from = "u", .to = "store", .latency = 0s}};
  return s;
}

// What the client saw for `op` in record `r`.
const simsys::engine::ResponseSample& response_of(const simsys::engine::StepRecord& r,
                                                  std::string_view op) {
  const auto it = std::ranges::find(r.responses, op, &simsys::engine::ResponseSample::op);
  REQUIRE(it != r.responses.end());
  return *it;
}

// The kv ring's imbalance at the default 150 virtual nodes: M = 8, N = 3 gives
// 1 + 1.4342/sqrt(450) = 1.06761; M = 16, N = 3 gives 1.08338; M = 4, N = 2 gives 1.06057.
double kv_ring_k(int machines, int replication) {
  return ring_factor(machines, replication, simsys::model::default_vnodes);
}

// A machine's sojourn in the M/M/c queue that one copy of the cluster is: c = 10 servers of
// 1 ms (the kv_front defaults) at utilization rho, its wait from Erlang C plus one exponential
// service (variance s^2).
simsys::queueing::TimeMoments machine_sojourn(double rho) {
  const auto wait = simsys::queueing::mmc_wait(10, rho, 0.001);
  return {.mean_s = wait.mean_s + 0.001, .variance_s2 = wait.variance_s2 + 1e-6};
}
}  // namespace

// nodes = 3, N = 3: every node holds every key, so every node is sent every request: 1k reads/s
// make 1k replica-requests/s at each of the three, 3k in all. Each node: capacity 10k/s x 1 ms
// service = c = 10 servers, rho = 1000 / 10000 = 0.1 (utilization per server), and the chance a
// request waits is Erlang C(c = 10, rho = 0.1) = 1.126e-7, a mean wait of C x s / (c (1 - rho)) =
// 1.25e-11 s: nothing next to the 1 ms service (relative 1.3e-8), so each replica's sojourn has
// mean m = 1 ms and variance s^2 = 1 ms^2 (SCV 1) to a part in 1e8. The read's response is the
// R-th fastest of three lognormals of those moments (queueing::lognormal_order_statistic, whose
// unit tests pin these figures): 0.4210, 0.8262 and 1.7528 ms for R = 1, 2, 3. Renyi's
// exponentials of the same mean would give 1/3, 5/6 and 11/6 ms; the fit, not the queue, differs.
TEST_CASE("a three-node cluster at replication three answers a read in the R-th fastest replica",
          "[theory]") {
  REQUIRE(simsys::queueing::erlang_c(10, 0.1) < 2e-7);  // the wait is negligible, as derived
  const auto read_with = [](int read_quorum) {
    const auto g = *Graph::build(kv_front({.nodes = 3,
                                           .replication = 3,
                                           .write_quorum = 2,
                                           .read_quorum = read_quorum,
                                           .reads = 1000,
                                           .writes = 0}));
    return simulation_of(g).step(0s, 100ms);
  };
  SECTION("R = 1: the fastest of three, 0.4210 ms") {
    const auto r = read_with(1);
    REQUIRE_THAT(response_of(r, "read").response.mean_s, WithinRel(4.2099631630e-4, 1e-6));
    REQUIRE_THAT(response_of(r, "read").response.variance_s2, WithinRel(7.3174194295e-8, 1e-6));
  }
  SECTION("R = 2: the quorum of two, 0.8262 ms") {
    const auto r = read_with(2);
    REQUIRE_THAT(response_of(r, "read").response.mean_s, WithinRel(8.2618489437e-4, 1e-6));
    REQUIRE_THAT(response_of(r, "read").response.variance_s2, WithinRel(2.5078568374e-7, 1e-6));
  }
  SECTION("R = 3: the slowest of three, 1.7528 ms") {
    const auto r = read_with(3);
    REQUIRE_THAT(response_of(r, "read").response.mean_s, WithinRel(1.7528187893e-3, 1e-6));
    REQUIRE_THAT(response_of(r, "read").response.variance_s2, WithinRel(1.7438470357e-6, 1e-6));
  }
}

// The same cluster at 1k reads/s: each node is sent 1k x N / nodes = 1000/s, so the node is at
// rho = 0.1 (the average, and the hottest too: N = nodes has no imbalance) and the sample counts
// the cluster's 3 x 1000 = 3000 replica-requests/s, which it all serves.
TEST_CASE("a kv_cluster's sample counts N times the requests in replica-requests", "[theory]") {
  const auto g = *Graph::build(kv_front({.nodes = 3,
                                         .replication = 3,
                                         .write_quorum = 2,
                                         .read_quorum = 2,
                                         .reads = 1000,
                                         .writes = 0}));
  const auto r = simulation_of(g).step(0s, 100ms);
  REQUIRE_THAT(r.nodes[1].demand_per_s, WithinRel(3000.0, 1e-12));
  REQUIRE_THAT(r.nodes[1].drain_per_s, WithinRel(3000.0, 1e-9));
  REQUIRE(r.nodes[1].dropped_per_s == 0.0);
  REQUIRE_THAT(r.nodes[1].rho, WithinRel(0.1, 1e-12));
  // The clients' side stays in requests: 1000 offered, all answered.
  REQUIRE_THAT(response_of(r, "read").offered.per_second, WithinRel(1000.0, 1e-12));
  REQUIRE_THAT(response_of(r, "read").answered.per_second, WithinRel(1000.0, 1e-9));
}

// A different N over a bigger ring: 4 nodes, N = 2, 4k reads/s: the cluster is sent 2 x 4000 =
// 8000 replica-requests/s, 2000 per node on average (rho 0.2), and the hottest node
// k = 1 + z_4/sqrt(2 x 150) = 1.0606 times that: rho_hot = 0.2121, the node's reported rho.
TEST_CASE("each request is sent to N replicas whatever the number of nodes", "[theory]") {
  const auto g = *Graph::build(kv_front({.nodes = 4,
                                         .replication = 2,
                                         .write_quorum = 1,
                                         .read_quorum = 1,
                                         .reads = 4000,
                                         .writes = 0}));
  const auto r = simulation_of(g).step(0s, 100ms);
  REQUIRE_THAT(r.nodes[1].demand_per_s, WithinRel(8000.0, 1e-12));
  REQUIRE_THAT(r.nodes[1].rho, WithinRel(kv_ring_k(4, 2) * 0.2, 1e-9));
  REQUIRE_THAT(r.nodes[1].rho, WithinAbs(0.2121, 1e-4));
}

// Writes wait for the W-th replica, reads for the R-th, from the same replicas: reads and writes
// 500/s each at nodes = 3, N = 3 are 1000 replica-requests/s per node again (m = 1 ms, SCV 1),
// so with R = 1 and W = 3 the read answers in 0.4210 ms and the write in 1.7528 ms, four times
// slower: the price of acknowledging at every replica.
TEST_CASE("a write is answered at the W-th fastest replica and a read at the R-th", "[theory]") {
  const auto g = *Graph::build(kv_front({.nodes = 3,
                                         .replication = 3,
                                         .write_quorum = 3,
                                         .read_quorum = 1,
                                         .reads = 500,
                                         .writes = 500}));
  const auto r = simulation_of(g).step(0s, 100ms);
  REQUIRE_THAT(response_of(r, "read").response.mean_s, WithinRel(4.2099631630e-4, 1e-6));
  REQUIRE_THAT(response_of(r, "write").response.mean_s, WithinRel(1.7528187893e-3, 1e-6));
}

TEST_CASE("a write quorum of two answers a write in 0.8262 ms", "[theory]") {
  const auto g = *Graph::build(kv_front({.nodes = 3,
                                         .replication = 3,
                                         .write_quorum = 2,
                                         .read_quorum = 3,
                                         .reads = 0,
                                         .writes = 1000}));
  const auto r = simulation_of(g).step(0s, 100ms);
  REQUIRE_THAT(response_of(r, "write").response.mean_s, WithinRel(8.2618489437e-4, 1e-6));
}

// Which ops are writes is the node's `write_ops` (model::write_ops_of; absent: just `write`). With
// write_ops = [put], a `put` waits for W = 3 and a `get` for R = 1, the same replicas as before.
TEST_CASE("the ops a kv_cluster lists in write_ops use the write quorum", "[theory]") {
  Scenario s = kv_front({.nodes = 3,
                         .replication = 3,
                         .write_quorum = 3,
                         .read_quorum = 1,
                         .reads = 500,
                         .writes = 500});
  s.nodes[0].workload->op_shares = {{"get", 0.5}, {"put", 0.5}};
  s.nodes[1].write_ops = std::vector<std::string>{"put"};
  const auto r = simulation_of(*Graph::build(s)).step(0s, 100ms);
  REQUIRE_THAT(response_of(r, "get").response.mean_s, WithinRel(4.2099631630e-4, 1e-6));
  REQUIRE_THAT(response_of(r, "put").response.mean_s, WithinRel(1.7528187893e-3, 1e-6));
}

// With one copy per key (N = 1) a request goes to one machine and waits for it: the quorum of one
// is that machine's own sojourn, and over the ring a request reaches the hot machine with
// probability p_hot = N k / M = k / M, the share the node's own latency already gives it. So the
// cluster answers exactly like the plain queue whose latency the node reports: on one machine,
// and on 8 machines behind the ring (k = 1.1171 at N = 1, hot rho 0.5585, cool 0.4933).
TEST_CASE("a cluster with one copy per key answers like the plain queue", "[theory]") {
  for (const int machines : {1, 8}) {
    CAPTURE(machines);
    const auto g = *Graph::build(kv_front({.nodes = machines,
                                           .replication = 1,
                                           .write_quorum = 1,
                                           .read_quorum = 1,
                                           .reads = 5'000.0 * machines,
                                           .writes = 0}));
    const auto r = simulation_of(g).step(0s, 100ms);
    const auto& plain = r.nodes[1].latency;
    REQUIRE_THAT(response_of(r, "read").response.mean_s, WithinRel(plain.mean_s, 1e-9));
    REQUIRE_THAT(response_of(r, "read").response.variance_s2, WithinRel(plain.variance_s2, 1e-8));
  }
}

// A request meets the hot machine only when its key's replica set holds it. nodes = 8, N = 3, 20k
// reads/s at 10k/s per node, 1 ms (c = 10): 60k replica-requests/s, 7.5k per node on average,
// rho = 0.75. The ring (k = kv_ring_k(8, 3) = 1.06761): the hot node carries k x 7.5k = 8.007k
// (rho_hot = 0.8007) and each of the 7 others (8 - k)/(8 x 7) x 60k = 7.428k (rho_cool = 0.7428).
// The hot node is sent k/8 of the 60k replica-requests, one from each request whose key it holds,
// so p_hot = 3 k / 8 = 0.40035 of the requests have it among their three replicas:
//   read = p_hot x 2nd of {hot, cool, cool}  +  (1 - p_hot) x 2nd of {cool, cool, cool}
// mixed by the law of total variance (behaviours::mixture). 0.94958 ms, against the 0.9402 ms of
// the old model (5/6 of the copies' request-weighted mean sojourn, as if every replica were an
// exponential of that mean). The node reports the hot copy's rho.
TEST_CASE("a read sees the hot machine only when its key's replicas include it", "[theory]") {
  const auto g = *Graph::build(kv_front({.nodes = 8,
                                         .replication = 3,
                                         .write_quorum = 2,
                                         .read_quorum = 2,
                                         .reads = 20'000,
                                         .writes = 0}));
  const auto r = simulation_of(g).step(0s, 100ms);
  constexpr double nodes = 8;
  const double k = kv_ring_k(8, 3);
  const double rho_hot = k * 0.75;
  const double rho_cool = (nodes - k) / (nodes - 1) * 0.75;
  using simsys::queueing::ReplicaGroup;
  const std::array with_hot{ReplicaGroup{.replicas = 1, .sojourn = machine_sojourn(rho_hot)},
                            ReplicaGroup{.replicas = 2, .sojourn = machine_sojourn(rho_cool)}};
  const std::array all_cool{ReplicaGroup{.replicas = 3, .sojourn = machine_sojourn(rho_cool)}};
  const auto hot_read = simsys::queueing::lognormal_order_statistic(with_hot, 2);
  const auto cool_read = simsys::queueing::lognormal_order_statistic(all_cool, 2);
  const double p_hot = 3.0 * k / nodes;
  const double mean = p_hot * hot_read.mean_s + (1 - p_hot) * cool_read.mean_s;
  const double variance =
      p_hot * (hot_read.variance_s2 + (hot_read.mean_s - mean) * (hot_read.mean_s - mean)) +
      (1 - p_hot) * (cool_read.variance_s2 + (cool_read.mean_s - mean) * (cool_read.mean_s - mean));
  REQUIRE_THAT(response_of(r, "read").response.mean_s, WithinRel(mean, 1e-9));
  REQUIRE_THAT(response_of(r, "read").response.variance_s2, WithinRel(variance, 1e-8));
  REQUIRE_THAT(response_of(r, "read").response.mean_s, WithinRel(9.4958356507e-4, 1e-6));
  REQUIRE_THAT(r.nodes[1].rho, WithinRel(rho_hot, 1e-12));
  REQUIRE_THAT(r.nodes[1].demand_per_s, WithinRel(60'000.0, 1e-12));
}

// The kv-store lab with `vnodes: 1` at 16 machines (0.3 ms each, c = 3; 21k reads and 9k writes,
// W = R = 2): the ring is as uneven as it gets, k = 1 + z_16/sqrt(3) = 2.0212, so the hot machine
// is sent 2.0212 x 5.625k = 11.37k/s against its 10k/s, fills its room and answers in about 100 ms,
// dropping 12 % of what it is sent. Yet a key's three replicas hold it with probability
// p_hot = 3 k / 16 = 0.379, and then the two cool replicas (rho 0.524) answer first: the quorum of
// two never waits for the hot one and never misses its drops. The store answers in 0.3775 ms
// (lognormal order statistics), nothing lost. A Monte Carlo of the same replicas with their exact
// M/M/3 sojourns (the review's, and ours: 0.385-0.389 ms) lands within 3 %: the residue is the
// lognormal's shape, not the quorum. The old model took the order statistic over the copies'
// request-weighted mean (10.7 ms) and failed the lab's p99.
TEST_CASE("with one virtual node the quorum of two hides the hot machine", "[theory]") {
  const auto g = *Graph::build(kv_front({.nodes = 16,
                                         .replication = 3,
                                         .write_quorum = 2,
                                         .read_quorum = 2,
                                         .reads = 21'000,
                                         .writes = 9'000,
                                         .capacity = 10'000,
                                         .latency = 300us,
                                         .vnodes = 1}));
  const auto settled = simulation_of(g).estimate();
  REQUIRE(settled.nodes[1].rho == 1.0);  // the hot machine is saturated
  REQUIRE(settled.nodes[1].dropped_per_s > 1'000.0);
  for (const auto* op : {"read", "write"}) {
    CAPTURE(op);
    const auto& answer = response_of(settled, op);
    REQUIRE_THAT(answer.response.mean_s, WithinRel(3.7751979967e-4, 1e-5));
    REQUIRE_THAT(answer.response.mean_s, WithinRel(0.385e-3, 0.03));  // the Monte Carlo
    REQUIRE_THAT(answer.answered.per_second, WithinRel(answer.offered.per_second, 1e-12));
  }
}

// A request is lost when fewer than q of its N replica-requests come back (I-1). Three machines,
// N = 3, each sent 11.25k/s against its 10k/s: each keeps 8/9 of what it is sent once its room is
// full. A read needing R replies is lost with P(Bin(3, 8/9) < R)
// (queueing::probability_fewer_than):
//   R = 1  1/729 = 0.14 %,   R = 2  25/729 = 3.4 %,   R = 3  1 - 512/729 = 29.8 %
// where the old rule lost the replica-requests' share, 1/9 = 11.1 %, at every R: it over-counted
// a forgiving quorum and under-counted a strict one.
TEST_CASE("a quorum of q of three saturated replicas is lost with the binomial's probability",
          "[theory]") {
  const auto delivered_with = [](int read_quorum) {
    const auto g = *Graph::build(kv_front({.nodes = 3,
                                           .replication = 3,
                                           .write_quorum = read_quorum,
                                           .read_quorum = read_quorum,
                                           .reads = 11'250,
                                           .writes = 0}));
    const auto settled = simulation_of(g).estimate();
    REQUIRE_THAT(settled.nodes[1].dropped_per_s, WithinRel(3'750.0, 1e-9));  // 1/9 of 33.75k
    const auto& read = response_of(settled, "read");
    return read.answered.per_second / read.offered.per_second;
  };
  REQUIRE_THAT(delivered_with(1), WithinRel(1.0 - 1.0 / 729.0, 1e-9));
  REQUIRE_THAT(delivered_with(2), WithinRel(1.0 - 25.0 / 729.0, 1e-9));
  REQUIRE_THAT(delivered_with(3), WithinRel(512.0 / 729.0, 1e-9));
}

// The book's lab as the plan first shipped it: nodes = 8, N = 3, vnodes 150, 30k requests/s of
// capacity-10k/s nodes. The cluster is sent 3 x 30k = 90k replica-requests/s against 8 x 10k =
// 80k: 11.25k per node on average (112.5 %), the hottest k = 1.0676 x that = 12.01k (120.1 %,
// red) and each of the others (8 - k)/56 x 90k = 11.14k (111.4 %): every node is over capacity
// and every queue ends full. The cluster serves 80k and drops 10k replica-requests/s, one ninth:
// the hot machine keeps 10k/12.01k = 0.8326 of what it is sent, a cool one 10k/11.14k = 0.8976.
// A request is lost when fewer than two of its three replicas answer (I-1), over its real replica
// set: with p_hot = 3k/8 = 0.4004, P(< 2 of {hot, cool, cool}), otherwise P(< 2 of three cool):
// 3.41 % lost, 96.59 % answered, not the 8/9 of the old "share of replica-requests dropped".
TEST_CASE("eight nodes at replication three overload at 30k requests a second", "[theory]") {
  const auto g = *Graph::build(kv_front({.nodes = 8,
                                         .replication = 3,
                                         .write_quorum = 2,
                                         .read_quorum = 2,
                                         .reads = 21'000,
                                         .writes = 9'000,
                                         .vnodes = 150}));
  const auto settled = simulation_of(g).estimate();
  REQUIRE(settled.converged);
  const auto& store = settled.nodes[1];
  REQUIRE_THAT(store.demand_per_s, WithinRel(90'000.0, 1e-9));
  REQUIRE_THAT(store.drain_per_s, WithinRel(80'000.0, 1e-9));
  REQUIRE_THAT(store.dropped_per_s, WithinRel(10'000.0, 1e-9));
  REQUIRE_THAT(store.rho, WithinRel(1.0, 1e-9));
  REQUIRE(store.zone == simsys::fluid::Zone::red);
  const double k = kv_ring_k(8, 3);
  const double kept_hot = 10'000.0 / (90'000.0 * k / 8.0);
  const double kept_cool = 10'000.0 / (90'000.0 * (8.0 - k) / 56.0);
  using simsys::queueing::TrialGroup;
  const std::array with_hot{TrialGroup{.trials = 1, .success = kept_hot},
                            TrialGroup{.trials = 2, .success = kept_cool}};
  const std::array all_cool{TrialGroup{.trials = 3, .success = kept_cool}};
  const double p_hot = 3.0 * k / 8.0;
  const double lost = p_hot * simsys::queueing::probability_fewer_than(with_hot, 2) +
                      (1.0 - p_hot) * simsys::queueing::probability_fewer_than(all_cool, 2);
  for (const auto* op : {"read", "write"}) {
    CAPTURE(op);
    const auto& answered = response_of(settled, op);
    const double delivered = answered.answered.per_second / answered.offered.per_second;
    REQUIRE_THAT(delivered, WithinRel(1.0 - lost, 1e-9));
    REQUIRE_THAT(delivered, WithinRel(0.9658844059, 1e-8));
  }
}

// The average load can be under capacity while the hottest node already sheds: nodes = 8, N = 3,
// 26k requests/s make 78k replica-requests/s, 9.75k per node on average (97.5 %: under 10k/s).
// The hottest node, sent k/8 = 13.35 % of them (10.41k/s, 104.1 %), drops 78k x k/8 - 10k =
// 409.2 replica-requests/s once its queue is full; each of the others is sent (8 - k)/56 x 78k =
// 9.656k/s and serves all of it. What the clients lose depends on the quorum: a read (R = 2) is
// answered by the two cool replicas whenever the hot one drops it, so nothing is lost; a write that
// needs all three (W = 3) is lost whenever the hot one drops it, p_hot x (1 - kept_hot) = 1.57 %.
TEST_CASE("the hottest node of a ring drops while the average node is under capacity", "[theory]") {
  const auto g = *Graph::build(kv_front({.nodes = 8,
                                         .replication = 3,
                                         .write_quorum = 3,
                                         .read_quorum = 2,
                                         .reads = 20'000,
                                         .writes = 6'000}));
  const auto settled = simulation_of(g).estimate();
  const double k = kv_ring_k(8, 3);
  const double dropped = 78'000.0 * k / 8.0 - 10'000.0;
  REQUIRE_THAT(dropped, WithinRel(409.19, 1e-4));  // the derivation's number
  const auto& store = settled.nodes[1];
  REQUIRE_THAT(store.dropped_per_s, WithinRel(dropped, 1e-6));
  REQUIRE_THAT(store.drain_per_s, WithinRel(78'000.0 - dropped, 1e-9));
  REQUIRE_THAT(store.rho, WithinRel(1.0, 1e-9));  // the hottest node's
  const auto& read = response_of(settled, "read");
  REQUIRE_THAT(read.answered.per_second, WithinRel(read.offered.per_second, 1e-12));
  const double kept_hot = 10'000.0 / (78'000.0 * k / 8.0);
  const auto& write = response_of(settled, "write");
  REQUIRE_THAT(write.answered.per_second / write.offered.per_second,
               WithinRel(1.0 - 3.0 * k / 8.0 * (1.0 - kept_hot), 1e-9));
  REQUIRE_THAT(write.answered.per_second / write.offered.per_second, WithinRel(0.9842620682, 1e-8));
}

// The fix: double the nodes. 16 nodes of 10k/s against the same 90k replica-requests/s: 5.625k per
// node on average (56.25 %), the hottest k = 1.08338 x 5.625k = 6.094k (60.9 %, green), nothing
// dropped, every request answered.
TEST_CASE("sixteen nodes carry the same load with the hottest node at 60.9 percent", "[theory]") {
  const auto g = *Graph::build(kv_front({.nodes = 16,
                                         .replication = 3,
                                         .write_quorum = 2,
                                         .read_quorum = 2,
                                         .reads = 21'000,
                                         .writes = 9'000,
                                         .vnodes = 150}));
  const auto settled = simulation_of(g).estimate();
  const auto& store = settled.nodes[1];
  REQUIRE_THAT(store.demand_per_s, WithinRel(90'000.0, 1e-9));
  REQUIRE_THAT(store.drain_per_s, WithinRel(90'000.0, 1e-9));
  REQUIRE(store.dropped_per_s == 0.0);
  REQUIRE_THAT(store.rho, WithinRel(kv_ring_k(16, 3) * 0.5625, 1e-9));
  REQUIRE_THAT(store.rho, WithinRel(0.6094, 1e-4));
  REQUIRE(store.zone == simsys::fluid::Zone::green);
  for (const auto* op : {"read", "write"}) {
    CAPTURE(op);
    REQUIRE_THAT(response_of(settled, op).answered.per_second,
                 WithinRel(response_of(settled, op).offered.per_second, 1e-9));
  }
}

// estimate is the long-run state of run: below capacity the last step of a run equals it, for
// the even cluster (N = nodes) and for the ring, in the node's numbers and in each op's response.
TEST_CASE("estimate and run agree on a kv_cluster below capacity", "[theory]") {
  const auto agree = [](const KvFront& shape) {
    const auto g = *Graph::build(kv_front(shape));
    auto running = simulation_of(g);
    simsys::engine::StepRecord last;
    running.run({.duration = 2s, .step = 100ms},
                [&](const simsys::engine::StepRecord& r) { last = r; });
    const auto e = simulation_of(g).estimate();
    REQUIRE_THAT(last.nodes[1].rho, WithinRel(e.nodes[1].rho, 1e-9));
    REQUIRE_THAT(last.nodes[1].demand_per_s, WithinRel(e.nodes[1].demand_per_s, 1e-9));
    REQUIRE_THAT(last.nodes[1].drain_per_s, WithinRel(e.nodes[1].drain_per_s, 1e-9));
    for (const auto* op : {"read", "write"}) {
      CAPTURE(op);
      REQUIRE_THAT(response_of(last, op).response.mean_s,
                   WithinRel(response_of(e, op).response.mean_s, 1e-9));
      REQUIRE_THAT(response_of(last, op).answered.per_second,
                   WithinRel(response_of(e, op).answered.per_second, 1e-9));
    }
  };
  SECTION("three nodes, N = 3: every node alike") {
    agree({.nodes = 3,
           .replication = 3,
           .write_quorum = 3,
           .read_quorum = 1,
           .reads = 500,
           .writes = 500});
  }
  SECTION("sixteen nodes, N = 3: a ring") {
    agree({.nodes = 16,
           .replication = 3,
           .write_quorum = 2,
           .read_quorum = 2,
           .reads = 21'000,
           .writes = 9'000});
  }
}

// Conservation in replica-requests, every step of the overloaded cluster: what the nodes were
// sent (N x the requests) is what they served plus what they dropped plus what they added to
// their queues. Nothing is created or lost by the replication, and the saturated run's last step
// is the settled state: the queues fill within 5 to 9 steps (1000 of room per node, the excess
// 112 to 217 per step).
TEST_CASE("a kv_cluster conserves its replica-requests at every step of an overload", "[theory]") {
  const auto g = *Graph::build(kv_front({.nodes = 8,
                                         .replication = 3,
                                         .write_quorum = 2,
                                         .read_quorum = 2,
                                         .reads = 21'000,
                                         .writes = 9'000}));
  auto sim = simulation_of(g);
  double backlog = 0.0;
  simsys::engine::StepRecord last;
  sim.run({.duration = 3s, .step = 100ms}, [&](const simsys::engine::StepRecord& r) {
    const auto& n = r.nodes[1];
    REQUIRE_THAT(backlog + n.demand_per_s * 0.1,
                 WithinRel((n.drain_per_s + n.dropped_per_s) * 0.1 + n.backlog, 1e-9));
    REQUIRE_THAT(n.demand_per_s, WithinRel(90'000.0, 1e-9));  // 3 x 30k, every step
    backlog = n.backlog;
    last = r;
  });
  const auto settled = sim.estimate();
  REQUIRE_THAT(last.nodes[1].drain_per_s, WithinRel(settled.nodes[1].drain_per_s, 1e-9));
  REQUIRE_THAT(last.nodes[1].dropped_per_s, WithinRel(settled.nodes[1].dropped_per_s, 1e-9));
  REQUIRE_THAT(backlog, WithinRel(8.0 * 1000.0, 1e-9));  // 8 full queues of 1000
}

// ---- Search index as service sugar (plan 2c refinement 10, Task 11; book ch. 13) ----
//
// A search_index runs exactly as a service of the same keys: no steps, the fluid drain and the
// M/M/c wait of Erlang C (queueing::mmc_wait). The scenario is search-autocomplete's trie, cut
// to its core: u -> trie_cache (read_through) -> trie_db.

namespace {
// u (60k/s of `suggest`) -> trie_cache (fixed hit ratio 0.9, read_through, capacity 100k/s) ->
// the trie: `trie_type` (a search_index, or the service it must equal) with 4 replicas, 2k/s each
// and 5 ms, all links with zero latency so the client's response is the nodes' own sojourns.
// Node indices: u 0, trie_cache 1, trie_db 2.
Scenario search_chain(NodeType trie_type) {
  Scenario s{.name = "search-chain", .version = 1, .duration = 10s, .warmup = 2s};
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{60'000}},
                                           .op_shares = {{"suggest", 1.0}}}},
             NodeSpec{.id = "trie_cache",
                      .type = NodeType::cache,
                      .capacity = Rate{100'000},
                      .latency = 250us,
                      .backend = "trie_db",
                      .hit_ratio = HitRatio{.fixed = 0.9},
                      .strategy = CacheStrategy::read_through},
             NodeSpec{.id = "trie_db",
                      .type = trie_type,
                      .replicas = 4,
                      .capacity = Rate{2000},
                      .latency = 5ms}};
  s.links = {
      LinkSpec{.from = "u", .to = "trie_cache", .dir = Direction::inout, .latency = 0s},
      LinkSpec{.from = "trie_cache", .to = "trie_db", .dir = Direction::inout, .latency = 0s}};
  return s;
}
}  // namespace

// The index behind the cache is offered the misses: (1 - 0.9) x 60k = 6k/s over 4 copies of 2k/s,
// per-copy rho = 6000 / 8000 = 0.75, with c = 2000/s x 5 ms = 10 servers per copy (Little's law).
// Erlang C at c = 10 and rho = 0.75 gives its wait; the sojourn is that plus the 5 ms service.
TEST_CASE(
    "a search index behind a read_through cache answers with the Erlang C sojourn of its copies",
    "[theory]") {
  const auto g = *Graph::build(search_chain(NodeType::search_index));
  const auto sim = Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
  const auto r = (*Simulation::create(g)).step(0s, 100ms);
  const auto& index = r.nodes[*g.index_of("trie_db")];
  REQUIRE_THAT(index.demand_per_s, WithinRel(6000.0, 1e-9));
  REQUIRE_THAT(index.rho, WithinRel(0.75, 1e-9));
  const double sojourn = simsys::queueing::mmc_wait(10, 0.75, 5e-3).mean_s + 5e-3;
  REQUIRE_THAT(index.latency.mean_s, WithinRel(sojourn, 1e-9));
  REQUIRE(index.dropped_per_s == 0.0);
}

// "Sugar": the same scenario written with a `service` of the same keys gives identical samples,
// bit for bit (== on doubles, not a tolerance): the engine runs one behaviour for both.
TEST_CASE("a search index steps exactly as the service of the same keys", "[theory]") {
  const auto as_index = *Graph::build(search_chain(NodeType::search_index));
  const auto as_service = *Graph::build(search_chain(NodeType::service));
  auto index_created = Simulation::create(as_index);
  INFO((index_created ? "" : index_created.error().message));
  REQUIRE(index_created.has_value());  // checked before the dereference below, so a refusal fails
  auto service_created = Simulation::create(as_service);
  REQUIRE(service_created.has_value());
  auto& index_sim = *index_created;
  auto& service_sim = *service_created;
  for (int i = 0; i < 5; ++i) {
    const auto a = index_sim.step(i * 100ms, 100ms);
    const auto b = service_sim.step(i * 100ms, 100ms);
    for (std::size_t node = 0; node < a.nodes.size(); ++node) {
      CAPTURE(i, node);
      REQUIRE(a.nodes[node].demand_per_s == b.nodes[node].demand_per_s);
      REQUIRE(a.nodes[node].rho == b.nodes[node].rho);
      REQUIRE(a.nodes[node].drain_per_s == b.nodes[node].drain_per_s);
      REQUIRE(a.nodes[node].latency.mean_s == b.nodes[node].latency.mean_s);
      REQUIRE(a.nodes[node].latency.variance_s2 == b.nodes[node].latency.variance_s2);
    }
    REQUIRE(a.responses[0].response.mean_s == b.responses[0].response.mean_s);
    REQUIRE(a.responses[0].answered.per_second == b.responses[0].answered.per_second);
  }
}
