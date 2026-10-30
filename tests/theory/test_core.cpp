// Whole-simulation checks against closed-form queueing theory (spec §12).
// Spec entries pinned here: §16 entries 4-6 (replicas, one-way link latency, feedback edges), §17
// entries 2-9 and 13 (rate curve, noise, op cost, balancer policies, limiters, cache, rejections,
// hot and cool copies, the leaky bucket), §18 entries 2 and 5 (Che, cache then steps).
// This file: the phase 1 and 2a closed forms: M/M/1, chains, delay and feedback links, estimate,
// load balancers, rate limiters, caches and the consistent-hashing lab. One of the nine files the
// theory tests are split into by topic; the scenario builders more than one of them needs live in
// tests/support/fixtures.cppm (module simsys.test_fixtures).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.caching;
import simsys.core;
import simsys.engine;
import simsys.fluid;
import simsys.graph;
import simsys.model;
import simsys.queueing;
import simsys.report;
import simsys.test_fixtures;
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
using simsys::model::HitRatio;
using simsys::model::LbPolicy;
using simsys::model::LimiterAlgorithm;
using simsys::model::LinkSpec;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
using simsys::model::Step;
using simsys::model::StepCondition;
using simsys::model::Workload;
// Shared builders (tests/support/fixtures.cppm).
using simsys::test_support::mm1;
using simsys::test_support::ring_factor;

namespace {
// mm1(demand), one client and one M/M/1 server, is shared with three other theory files: it lives
// in tests/support/fixtures.cppm (module simsys.test_fixtures).

// c(100/s) -> f -> g, and g sends half of what it serves back to f (factor 0.5) over a
// feedback link of the given latency: a loop of gain 0.5.
Scenario half_loop(simsys::SimTime feedback_latency) {
  // C++20 designated initializers: `.name = ...` names each member being set, in
  // declaration order, and leaves the rest at their defaults. Replaces positional braces
  // (which break silently when members are reordered) or a default object assigned field
  // by field; here a scenario reads like the YAML it stands for.
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

// (The example used to be a kv_cluster, then a ws_gateway; both are simulated, so the refusal is
// shown on a notification_fanout, which waits for phase 3.)
TEST_CASE("a simulation refuses node types the phase does not support yet", "[theory]") {
  Scenario s = mm1(1);
  s.nodes[1].type = NodeType::notification_fanout;
  const auto g = *simsys::graph::Graph::build(s);
  const auto sim = Simulation::create(g);
  REQUIRE_FALSE(sim.has_value());
  REQUIRE(sim.error().message ==
          "node 's' has type 'notification_fanout', which run/estimate do not support yet "
          "(phase 3)");
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
                                           simsys::workload::client_seed(s.seed, 0));
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

// The ring's imbalance k, the hottest machine's load over the average:
// test_support::ring_factor (tests/support/fixtures.cppm), k = min(1 + z_M / sqrt(N v),
// M / N); v = 100, M = 4: 1.1049;  v = 150, M = 4: 1.0857;  v = 1, M = 4: 2.0491.

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

// The rate limiter of ch. 4 turns excess requests away instead of queueing them. The client is not
// told: the limiter's own queue is empty and it drops nothing; what it refuses is counted in a
// separate channel (`rejected_per_s`), apart from the drops, and none of it is forwarded, so the
// service behind sees the admitted rate only. The client's `answered` rate is what got through:
// 1000 of the 4000 it offers (spec §17 entry 8).
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
// regular). Erlang C knows no room: at 990/s its M/D/1 wait is (1 + 0) / 2 x 0.99 / 0.01 x 1 ms =
// 49.5 ms, so there the bound is what the wait is. At every load, below the limit, at it and above
// it (where the fluid backlog fills the room and its drain time is the bound itself), the mean wait
// never passes 20 ms.
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

// The client's latency summary averages over the requests ANSWERED (spec §17 entry 8): a rejected
// request never sees the api's queue, so it must not weigh the api's latency. Here the counter
// check makes every answered request pay the counter store's hop, and the client's `answered` rate
// is the admitted 1000/s; the 3000/s refused are not in it.
TEST_CASE(
    "the counter store sees every request a limiter checks; the client is answered only the "
    "admitted",
    "[theory]") {
  auto s = limited(LimiterAlgorithm::token_bucket, 1000, 100, 4000);
  // A service stands in for the counter store, the simplest node that serves a backend's calls:
  // the graph (unlike the loader) takes any node as a backend.
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
// With perfectly even arcs the four would act as one 8M-key cache: 0.962834 by the dense reference.
// With the lab's `vnodes: 1` the hottest server owns k/4 = 0.512283 of the keys (k = 1 +
// z_4/sqrt(1) = 2.049131, ring_factor) and the three others 0.162572 each, so the hot server is a
// 2M / 0.512283 = 3.904093M-key cache over the whole space and the cool ones hold their every key:
// h = 0.512283 × 0.844457 + 0.487717 × 1 = 0.920318. Provenance of 0.962834 and 0.844457: Che's
// equation solved by 100 bisection steps over all 10M ranks one at a time, in double precision and
// with no buckets (the method of test_caching.cpp's dense_che_hit_ratio, run outside the suite: 10M
// ranks x 100 steps take seconds), recomputed 2026-10-29 for the ring heuristic of numerics §8
// (0.848235 at the old k = 2, 4M keys).
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
