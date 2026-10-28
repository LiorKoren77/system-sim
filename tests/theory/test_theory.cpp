// Whole-simulation checks against closed-form queueing theory (spec §12).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
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

TEST_CASE("a simulation refuses node types the phase does not support yet", "[theory]") {
  Scenario s = mm1(1);
  s.nodes[1].type = NodeType::kv_cluster;
  const auto g = *simsys::graph::Graph::build(s);
  const auto sim = Simulation::create(g);
  REQUIRE_FALSE(sim.has_value());
  REQUIRE(sim.error().message ==
          "node 's' has type 'kv_cluster', which run/estimate do not support yet (phase 2)");
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

// A hash ring with v virtual nodes per server spreads keys unevenly: the busiest server
// holds about 1 + 1/sqrt(v) times the average share (a heuristic from the book ch. 5).
// With v = 100 the hottest of 4 copies sees 900/s * 1.1 = 990/s, so rho = 0.99.
TEST_CASE("consistent hashing loads the hottest replica by the imbalance factor", "[theory]") {
  Scenario s = lb_front(LbPolicy::consistent_hash, 4, 3600);  // 900/s per copy on average
  s.nodes[1].vnodes = 100;                                    // hottest copy sees 990/s
  const auto r = (*Simulation::create(*Graph::build(s))).step(0s, 100ms);
  REQUIRE_THAT(r.nodes[2].rho, WithinRel(0.99, 1e-12));
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

// A hash ring with vnodes 1 over 4 copies: the hottest copy holds k = min(1 + 1/sqrt(1), 4) = 2
// times the average share, the other three split what is left. At 3000/s (750/s per copy on
// average) the hot copy is sent 2 × 750 = 1500/s against 1000/s capacity, so it sheds 500/s
// once its 100-request room is full; each cool copy is sent (4 - 2)/(4 - 1) × 750 = 500/s and
// serves all of it. The group drains 1000 + 3 × 500 = 2500/s: only the hot copy's excess is lost.
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
  REQUIRE_THAT(a.demand_per_s, WithinRel(3000.0, 1e-12));
  REQUIRE_THAT(a.drain_per_s, WithinRel(2500.0, 1e-12));
  REQUIRE_THAT(a.dropped_per_s, WithinRel(500.0, 1e-12));
  REQUIRE_THAT(a.backlog, WithinRel(100.0, 1e-12));  // the hot copy's room; the cool ones are empty
  REQUIRE(a.rho == 1.0);                             // reported: the hottest copy
  REQUIRE(a.zone == simsys::fluid::Zone::red);
  const auto settled = sim.estimate();
  REQUIRE_THAT(settled.nodes[2].drain_per_s, WithinRel(2500.0, 1e-12));
  REQUIRE_THAT(settled.nodes[2].dropped_per_s, WithinRel(500.0, 1e-12));
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
// rho_cool = (N - k)/(N - 1) × rho. Here N = 4, vnodes 150 (k = 1 + 1/sqrt(150) ≈ 1.082),
// average rho 0.85 on 1000/s copies with 1 ms service.
TEST_CASE("an imbalanced node's latency is the share-weighted mixture of the hot and cool copies",
          "[theory]") {
  constexpr double copies = 4;
  constexpr double rho = 0.85;
  Scenario s = lb_front(LbPolicy::consistent_hash, 4, rho * copies * 1000);
  s.nodes[1].vnodes = 150;
  const auto r = (*Simulation::create(*Graph::build(s))).step(0s, 100ms);
  const double k = 1.0 + 1.0 / std::sqrt(150.0);
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
// reference. With the lab's `vnodes: 1` the hottest server owns half the keys (k = 2) and the
// three others a sixth each, so the hot server is a 4M-key cache over the whole space and the
// cool ones hold their every key: h = 0.5 × 0.848235 + 0.5 × 1 = 0.924118.
// Provenance of 0.962834 and 0.848235: Che's equation solved by 100 bisection steps over all 10M
// ranks one at a time, in double precision and with no buckets (the method of test_caching.cpp's
// dense_che_hit_ratio, run outside the suite: 10M ranks x 100 steps take seconds), recomputed
// 2026-10-28.
TEST_CASE("the consistent-hashing lab's cache hits 0.963 on even arcs and 0.924 on one vnode",
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
  REQUIRE_THAT(*g->hit_ratio(*g->index_of("cache")), WithinAbs(0.924118, 1e-5));
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
//   s_obj    = 50 MB x 40 ms = 2 s, the time one upload holds a slot;
//   Erlang C(20, 0.5) = 0.0037311 (queueing::erlang_c), so the wait is
//   Wq       = C x s_obj / (c (1 - rho)) = 0.0037311 x 2 s / (20 x 0.5) = 0.746 ms
//   response = Wq + 50 MB x 40 ms = 2.000746 s: a write is answered at its last byte.
// Before the review the wait was computed over 1 MB units (0.0149 ms) and the response was the
// first megabyte's 40 ms: 50 times too short a wait and an upload acknowledged before its data.
TEST_CASE("a default blob store fed 5 uploads of 50 MB per second runs at rho 0.5 with c 20",
          "[theory]") {
  const auto g =
      *Graph::build(blob_store_chain("upload", 5, simsys::Bytes{50e6}, store_writing("upload")));
  const auto r = first_step(g);
  const auto& store = r.nodes[*g.index_of("store")];
  REQUIRE_THAT(store.demand_per_s, WithinRel(250.0, 1e-12));  // units (MB) per second
  REQUIRE_THAT(store.rho, WithinRel(0.5, 1e-12));
  REQUIRE(store.dropped_per_s == 0.0);
  const auto wait = simsys::queueing::mmc_wait(20, 0.5, 2.0).mean_s;
  REQUIRE_THAT(wait, WithinAbs(7.4623e-4, 1e-8));  // the napkin number: 0.746 ms
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(wait + 2.0, 1e-9));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(2.000746, 1e-6));
  // The node's own latency is its requests' sojourn: here every request is an upload.
  REQUIRE_THAT(store.latency.mean_s, WithinRel(wait + 2.0, 1e-9));
  REQUIRE_THAT(r.responses[0].answered.per_second, WithinRel(5.0, 1e-9));  // requests, not MB
}

// The same queue read instead of written: 5 reads/s of 50 MB wait the same whole-object 0.746 ms
// (the queue does not care which way the bytes go), but a read is answered when its first
// megabyte arrives, Wq + 40 ms = 40.746 ms: the client reads the rest as it streams in.
TEST_CASE("a 50 MB read waits behind whole objects and is answered at its first megabyte",
          "[theory]") {
  const auto g = *Graph::build(blob_store_chain("read", 5, simsys::Bytes{50e6}));
  const auto r = first_step(g);
  REQUIRE_THAT(r.nodes[*g.index_of("store")].rho, WithinRel(0.5, 1e-12));
  const auto wait = simsys::queueing::mmc_wait(20, 0.5, 2.0).mean_s;
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(wait + 0.040, 1e-9));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(0.040746, 1e-6));
}

// The review's example: 9 reads/s of 50 MB at a default store are 450 MB/s, rho 0.9. Over 1 MB
// units the wait would be C(20, 0.9) x 40 ms / (20 x 0.1) = 0.5508 x 0.02 s = 11 ms; whole 50 MB
// objects (s_obj = 2 s) wait C(20, 0.9) x 2 s / (20 x 0.1) = 0.5508 s, fifty times longer. The
// read is answered 40 ms after that wait: 0.5908 s.
TEST_CASE("at rho 0.9 a 50 MB object waits about half a second, not 11 ms", "[theory]") {
  const auto g = *Graph::build(blob_store_chain("read", 9, simsys::Bytes{50e6}));
  const auto r = first_step(g);
  REQUIRE_THAT(r.nodes[*g.index_of("store")].rho, WithinRel(0.9, 1e-12));
  const auto per_unit = simsys::queueing::mmc_wait(20, 0.9, 0.040).mean_s;
  const auto per_object = simsys::queueing::mmc_wait(20, 0.9, 2.0).mean_s;
  REQUIRE_THAT(per_unit, WithinAbs(0.011015, 1e-6));
  REQUIRE_THAT(per_object, WithinAbs(0.550769, 1e-6));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(per_object + 0.040, 1e-9));
}

// Book ch. 14's 300 MB upload, counted as a write: 1 upload/s at a default store is 300 MB/s,
// rho 0.6; s_obj = 300 MB x 40 ms = 12 s, so Wq = C(20, 0.6) x 12 s / (20 x 0.4) =
// 0.024134 x 1.5 s = 36.2 ms, and the upload is acknowledged at its last byte, 12 s + 36.2 ms =
// 12.0362 s. (A client `timeout: 10s` must therefore see it time out: plan Task 13.)
TEST_CASE("a 300 MB upload is answered at its last byte, 12 s after its wait", "[theory]") {
  const auto g =
      *Graph::build(blob_store_chain("upload", 1, simsys::Bytes{300e6}, store_writing("upload")));
  const auto r = first_step(g);
  REQUIRE_THAT(r.nodes[*g.index_of("store")].rho, WithinRel(0.6, 1e-12));
  const auto wait = simsys::queueing::mmc_wait(20, 0.6, 12.0).mean_s;
  REQUIRE_THAT(wait, WithinAbs(0.036202, 1e-6));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(12.0 + wait, 1e-9));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(12.0362, 1e-4));
}

// A 2 KB read costs 2e3 B / 1e6 B per unit = 0.002 units: 1000 reads/s are 2 units/s, rho =
// 2 / 500 = 0.004; s_obj = 0.002 x 40 ms = 80 us and nothing waits (Erlang C(20, 0.004) = 4e-41),
// and yet each read takes the whole 40 ms of one unit: the documented upper bound for small
// objects (a request under 1 MB still pays one unit's time). Without the bound a 2 KB read would
// be answered in 80 us.
TEST_CASE("a 2 KB read at a blob store costs 0.002 units and is answered in 40 ms", "[theory]") {
  const auto g = *Graph::build(blob_store_chain("read", 1000, simsys::Bytes{2e3}));
  const auto r = first_step(g);
  const auto& store = r.nodes[*g.index_of("store")];
  REQUIRE_THAT(store.demand_per_s, WithinRel(2.0, 1e-12));  // 1000/s x 0.002 units
  REQUIRE_THAT(store.rho, WithinRel(0.004, 1e-12));
  const auto wait = simsys::queueing::mmc_wait(20, 0.004, 80e-6).mean_s;
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

// A blob store's `then` steps are event notifications (S3 event notifications; book ch. 14's
// "transcoding starts when the upload completes"): asynchronous work, which arrives with phase 2c
// Task 4. Until then a store with one is refused, and the message (pinned whole, as the schema's
// table of refusals quotes it) says which node, what is pending and when it comes.
TEST_CASE("the engine refuses a blob store with then steps until phase 2c's events", "[theory]") {
  const auto g = Graph::build(blob_store_with_event());
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  const auto sim = Simulation::create(*g);
  REQUIRE_FALSE(sim.has_value());
  REQUIRE(sim.error().message ==
          "node 'store': a blob store's then steps are event notifications, which run/estimate "
          "do not support yet (phase 2c)");
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
