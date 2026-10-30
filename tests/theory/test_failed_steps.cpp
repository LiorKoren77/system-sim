// Whole-simulation checks against closed-form queueing theory (spec §12).
// Spec entries pinned here: §19 entry 11 (a step after a failed step does not run).
// This file: a step after a failed step does not run. One of the nine files the theory tests are
// split into by topic; the scenario builders more than one of them needs live in
// tests/support/fixtures.cppm (module simsys.test_fixtures).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
using simsys::Rate;
using simsys::graph::Graph;
using simsys::model::CacheStrategy;
using simsys::model::HitRatio;
using simsys::model::LinkSpec;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
using simsys::model::Step;
using simsys::model::StepCondition;
using simsys::model::Workload;
// Shared builders (tests/support/fixtures.cppm).
using simsys::test_support::group_of;
using simsys::test_support::published;
using simsys::test_support::records_of;
using simsys::test_support::simulation_of;

// ---- A step after a failed step does not run (spec §19 entry 11) ----------------------
//
// A request is answered only if every step of its op was; so a step that comes after one the
// request already failed never fires. The flow sent on step k is
//     rate x fraction x factor x S_k,
// where S_k is the product of the delivered fractions d of the steps before it (d = what the
// target answered: 1 minus the share its drops, and everything it called in turn, lost):
//  - sequential steps thin one another:     S_k = d_1 x d_2 x ... x d_(k-1);
//  - a parallel group fails as a whole:     its survivors product d_a x d_b multiplies what
//  follows,
//    while its own members are all sent the full flow (they start together);
//  - a miss block (`on: miss` after a cache lookup) thins only its miss share: of every request,
//    h hit and are done, 1 - h missed and go on to the block, so the block's survivors are
//    h + (1 - h) x d_block.
// The model ignores the order inside a request in time: it is a fluid, so "does not run" is a
// smaller flow. Its one blind spot is the engine's step lag: the fraction a step reads is the one
// the targets reported at the END OF THE LAST simulation step, so the first step still sends the
// full flow and the thinned flow arrives from the next ones on (the targets of the tests below
// report their steady fraction after a few steps, so every test reads the last record).
//
// Numbers, from one picture: a client sends 1000 x/s to svc (10k/s, nowhere near its limit), whose
// op `x` calls the nodes below. A target with `capacity` units/s and a room of ONE request drains
// `capacity` of what it is offered and drops the rest, so it delivers capacity / offered:
//   a: 500/s of 1000 offered  -> 0.5          b: 800/s of 1000 -> 0.8
// (Node indices: u 0, svc 1, then the targets in the order given.)

namespace {
// A service that drains `capacity_per_s` a second and keeps one request waiting: past that it
// drops, so under a steady overload its delivered fraction is capacity / offered.
NodeSpec lossy_target(std::string id, double capacity_per_s) {
  // C++20 designated initializers: `.id = ...` names each member being set, in
  // declaration order, and leaves the rest at their defaults. Replaces positional braces
  // (which break silently when members are reordered) or a default object assigned field
  // by field; here a node reads like the YAML it stands for.
  return NodeSpec{.id = std::move(id),
                  .type = NodeType::service,
                  .capacity = Rate{capacity_per_s},
                  .latency = 1ms,
                  .advanced = {.queue_limit = 1}};
}

// u (1000/s of `x`) -> svc (10k/s) -> `targets`, all links of zero latency so that what a step
// sends arrives in the same simulation step; svc's op `x` runs `steps`.
Scenario steps_chain(std::vector<Step> steps, std::vector<NodeSpec> targets) {
  Scenario s{.name = "steps-chain", .version = 1, .duration = 2s};
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{1000}}, .op_shares = {{"x", 1.0}}}},
      NodeSpec{.id = "svc",
               .type = NodeType::service,
               .capacity = Rate{10'000},
               .latency = 1ms,
               .ops = {{"x", std::move(steps)}}}};
  s.links = {LinkSpec{.from = "u", .to = "svc", .latency = 0s}};
  for (auto& target : targets) {
    s.links.push_back(LinkSpec{.from = "svc", .to = target.id, .latency = 0s});
    s.nodes.push_back(std::move(target));
  }
  return s;
}

// A request-counting service that never loses anything: the node a thinned flow ends at.
NodeSpec plain_target(std::string id) {
  return NodeSpec{
      .id = std::move(id), .type = NodeType::service, .capacity = Rate{100'000}, .latency = 1ms};
}

double demand_at(const simsys::engine::StepRecord& record, const Graph& g, std::string_view id) {
  return record.nodes[*g.index_of(id)].demand_per_s;
}

// The share of what `id` was sent in this step that it dropped.
double dropped_share_at(const simsys::engine::StepRecord& record, const Graph& g,
                        std::string_view id) {
  const auto& node = record.nodes[*g.index_of(id)];
  return node.dropped_per_s / node.demand_per_s;
}

constexpr auto parallel = simsys::model::StepMode::parallel;
}  // namespace

// a delivers 0.5, so the request that fails at a never reaches b:
//   b's demand = 1000 x 0.5 = 500/s            (before: 1000/s, as if no request ever failed)
// The lag, exactly (100 ms steps, zero-latency links): the first step still sends b 1000/s,
// nothing was reported yet. In it a, starting empty, drains 50 of the 100 requests it is sent and
// keeps one waiting, so it delivers (50 + 1) / 100 = 0.51, and the second step sends b
// 1000 x 0.51 = 510/s. From then on a's room stays full and it drops exactly half: b gets 500/s.
// The long run is `estimate`.
TEST_CASE("a second step is sent only what the first step delivered: 500 of 1000 a second",
          "[theory]") {
  const auto g = *Graph::build(steps_chain({Step{.to = "a", .op = "x"}, Step{.to = "b", .op = "x"}},
                                           {lossy_target("a", 500), plain_target("b")}));
  const auto records = records_of(g, 20);
  REQUIRE_THAT(demand_at(records[0], g, "b"), WithinRel(1000.0, 1e-9));  // nothing reported yet
  REQUIRE_THAT(demand_at(records[1], g, "b"), WithinRel(510.0, 1e-9));   // step 0's 0.51
  for (std::size_t i = 2; i < records.size(); ++i) {
    CAPTURE(i);
    REQUIRE_THAT(dropped_share_at(records[i], g, "a"), WithinRel(0.5, 1e-9));  // a delivers 0.5
    REQUIRE_THAT(demand_at(records[i], g, "a"), WithinRel(1000.0, 1e-9));  // the first step: whole
    REQUIRE_THAT(demand_at(records[i], g, "b"), WithinRel(500.0, 1e-9));   // 1000 x 0.5
  }
  const auto estimate = simulation_of(g).estimate();
  REQUIRE_THAT(estimate.nodes[*g.index_of("b")].demand_per_s, WithinRel(500.0, 1e-4));
}

// An op that calls one overloaded node five times, x: [a, a, a, a, a] at 1k/s, each call sent only
// the requests the calls before it got through. With d the share a delivers, a is offered
//   L(d) = 1000 × (1 + d + d² + d³ + d⁴)          (call k is sent 1000 × d^(k-1))
// and with 3686/s of capacity and a room of one it drops what is over capacity, so d = 3686 / L(d).
// The fixed point is d = 0.9: L = 4095.1, 3686 / 4095.1 = 0.9001 (10 % dropped). The map
// d ↦ 3686 / L(d) is decreasing with slope about −1.79 there, steeper than −1, so feeding each
// d straight back never settles: d = 1 offers 5000 (drops 26 %), d = 0.74 offers 2988 (drops
// nothing), d = 1 again... `run` thins every step by the raw fraction of the step before (it
// cannot know this step's before serving it), so it shows exactly that sawtooth for the whole run:
// the fixed point is not a state any step passes through. `estimate` thins by a damped average of
// its passes' fractions, d̄ ← (1 − w) d̄ + w d, starting at w = 1/2, whose slope there is
// (1 − 1.79) / 2 ≈ −0.4, and settles.
TEST_CASE("five calls to one overloaded node settle at d = 0.9 under estimate: 4095 offered",
          "[theory]") {
  const Step call_a{.to = "a", .op = "x"};
  const auto g = *Graph::build(
      steps_chain({call_a, call_a, call_a, call_a, call_a}, {lossy_target("a", 3686)}));
  const auto estimate = simulation_of(g).estimate();
  REQUIRE(estimate.converged);
  REQUIRE_THAT(demand_at(estimate, g, "a"), WithinRel(4095.1, 1e-3));
  REQUIRE_THAT(dropped_share_at(estimate, g, "a"), WithinRel(0.1, 1e-3));  // d = 0.9
  // The client is answered only when all five calls are: d⁵ = 0.59 of its 1000/s.
  REQUIRE_THAT(estimate.responses[0].answered.per_second,
               WithinRel(1000.0 * std::pow(0.9, 5), 1e-3));
}

// The same op with ten and with twenty calls to `a`, each `a` sized so that d = 0.9 is again the
// fixed point. With n calls
//   L(d) = 1000 × (1 − dⁿ) / (1 − d),   capacity = 0.9 × L(0.9):
//   n = 10: L(0.9) = 6513.2, capacity 5862 (d = 0.900004);  n = 20: L(0.9) = 8784.2, capacity 7906.
// The map f(d) = capacity / L(d) is much steeper there: f'(d) = −d L'(d) / L(d) = −3.65 at ten
// calls and −6.23 at twenty. The plain halfway average has slope 1/2 + f'/2 = −1.32 and −2.62:
// past −1, so it overshot further every pass, and estimate ended at the pass cap with the
// no-steady-state warning and 20.8 % (ten calls) or 0 % (twenty) dropped instead of 10 %.
// Halving the weight at each overshoot (Simulation::DampedFraction) brings the slope 1 − w + w f'
// inside (−1, 1) once w is down to 1/4 at ten calls (−0.16) and to 1/8 at twenty (0.10), and the
// passes settle on d = 0.9. (By hand, the same way: fifty calls at d = 0.95, f' = −14.8, and a
// thousand at d = 0.999, f' = −417, settle too.)
TEST_CASE("ten and twenty calls to one overloaded node settle at d = 0.9 under estimate",
          "[theory]") {
  const Step call_a{.to = "a", .op = "x"};
  struct Case {
    std::size_t calls;
    double capacity;
    double offered;  // L(0.9) for that many calls
  };
  for (const auto& c : {Case{.calls = 10, .capacity = 5862, .offered = 6513.2},
                        Case{.calls = 20, .capacity = 7906, .offered = 8784.2}}) {
    CAPTURE(c.calls);
    const auto g = *Graph::build(
        steps_chain(std::vector<Step>(c.calls, call_a), {lossy_target("a", c.capacity)}));
    const auto estimate = simulation_of(g).estimate();
    REQUIRE(estimate.converged);
    REQUIRE_THAT(demand_at(estimate, g, "a"), WithinRel(c.offered, 1e-3));
    REQUIRE_THAT(dropped_share_at(estimate, g, "a"), WithinRel(0.1, 1e-3));  // d = 0.9
    // Answered only when all n calls are: 0.9ⁿ of the 1000/s.
    REQUIRE_THAT(estimate.responses[0].answered.per_second,
                 WithinRel(1000.0 * std::pow(0.9, static_cast<double>(c.calls)), 1e-3));
  }
}

// Survival multiplies down the chain. a delivers 0.5, so b is offered 1000 x 0.5 = 500/s; b can
// drain 250/s of them, so it delivers 250 / 500 = 0.5 (not 250 / 1000 = 0.25, the fraction it
// would deliver if a failure at a did not stop the request); c is sent
//   1000 x S_c = 1000 x (0.5 x 0.5) = 250/s.
TEST_CASE("three sequential steps thin one another: the third is sent 1000 x 0.5 x 0.5 = 250",
          "[theory]") {
  const auto g = *Graph::build(steps_chain(
      {Step{.to = "a", .op = "x"}, Step{.to = "b", .op = "x"}, Step{.to = "c", .op = "x"}},
      {lossy_target("a", 500), lossy_target("b", 250), plain_target("c")}));
  const auto last = records_of(g, 30).back();
  REQUIRE_THAT(demand_at(last, g, "a"), WithinRel(1000.0, 1e-9));
  REQUIRE_THAT(demand_at(last, g, "b"), WithinRel(500.0, 1e-3));       // 1000 x 0.5
  REQUIRE_THAT(dropped_share_at(last, g, "b"), WithinRel(0.5, 1e-3));  // 250 of the 500 offered
  REQUIRE_THAT(demand_at(last, g, "c"), WithinRel(250.0, 1e-3));       // 1000 x 0.5 x 0.5
}

// [a || b] then c: the group starts together, so a and b are both sent the whole 1000/s, and a
// failure at either fails the request:
//   survivors of the group = d_a x d_b = 0.5 x 0.8 = 0.4
//   c's demand             = 1000 x 0.4 = 400/s
// and the client's requests answered are exactly those that got through everything: 400/s.
TEST_CASE("a parallel group fails as a whole: the step after it is sent 1000 x 0.5 x 0.8 = 400",
          "[theory]") {
  const auto g = *Graph::build(
      steps_chain({Step{.to = "a", .op = "x", .mode = parallel},
                   Step{.to = "b", .op = "x", .mode = parallel}, Step{.to = "c", .op = "x"}},
                  {lossy_target("a", 500), lossy_target("b", 800), plain_target("c")}));
  const auto records = records_of(g, 20);
  REQUIRE_THAT(demand_at(records.front(), g, "c"), WithinRel(1000.0, 1e-9));  // one step late
  const auto& last = records.back();
  REQUIRE_THAT(demand_at(last, g, "a"), WithinRel(1000.0, 1e-9));      // members of a group are not
  REQUIRE_THAT(demand_at(last, g, "b"), WithinRel(1000.0, 1e-9));      // thinned by one another
  REQUIRE_THAT(dropped_share_at(last, g, "a"), WithinRel(0.5, 1e-9));  // 500 of 1000
  REQUIRE_THAT(dropped_share_at(last, g, "b"), WithinRel(0.2, 1e-9));  // 200 of 1000: delivers 0.8
  REQUIRE_THAT(demand_at(last, g, "c"), WithinRel(400.0, 1e-3));       // 1000 x 0.5 x 0.8
  REQUIRE_THAT(last.responses[0].answered.per_second, WithinRel(400.0, 1e-3));
  const auto estimate = simulation_of(g).estimate();
  REQUIRE_THAT(estimate.nodes[*g.index_of("c")].demand_per_s, WithinRel(400.0, 1e-4));
  REQUIRE_THAT(estimate.responses[0].answered.per_second, WithinRel(400.0, 1e-4));
}

// web's read: [cache lookup (hit ratio 0.8), db read on miss, then x]. The block is a miss block:
// only the 1 - 0.8 = 0.2 that missed go to the db, which is offered 1000 x 0.2 = 200/s and can
// drain 100/s, so it delivers 100 / 200 = 0.5. A request that hit never meets the db, so the miss
// block fails only its own 0.2 share, and the survivors are
//   0.8 + 0.2 x 0.5 = 0.9           (not 0.5 x ..., and not 0.8: a half-failed miss share still
//   lives)
// and x is sent 1000 x 0.9 = 900/s, the same 900/s the client has answered.
TEST_CASE("a miss block thins only the miss share: x is sent 0.8 + 0.2 x 0.5 = 0.9 of the rate",
          "[theory]") {
  Scenario s = steps_chain(
      {Step{.to = "cache", .op = "x"}, Step{.to = "db", .op = "x", .on = StepCondition::miss},
       Step{.to = "x_target", .op = "x"}},
      {NodeSpec{.id = "cache",
                .type = NodeType::cache,
                .capacity = Rate{100'000},
                .latency = 100us,
                .hit_ratio = HitRatio{.fixed = 0.8},
                .strategy = CacheStrategy::cache_aside},
       lossy_target("db", 100), plain_target("x_target")});
  const auto g = *Graph::build(s);
  const auto records = records_of(g, 20);
  REQUIRE_THAT(demand_at(records.front(), g, "x_target"),
               WithinRel(1000.0, 1e-9));  // one step late
  const auto& last = records.back();
  REQUIRE_THAT(demand_at(last, g, "db"), WithinRel(200.0, 1e-9));  // 1000 x 0.2: the misses alone
  REQUIRE_THAT(dropped_share_at(last, g, "db"), WithinRel(0.5, 1e-9));   // 100 of the 200
  REQUIRE_THAT(demand_at(last, g, "x_target"), WithinRel(900.0, 1e-3));  // 1000 x (0.8 + 0.2 x 0.5)
  REQUIRE_THAT(last.responses[0].answered.per_second, WithinRel(900.0, 1e-3));
}

// A worker pool's `then` steps are sequential like a service's: 8 jobs/s drained (10 workers at 1 s
// a job: capacity 10/s, rho 0.8). The first step's target drains 4/s of the 8 it is sent, so it
// delivers 4 / 8 = 0.5, and the second step is sent only the jobs whose first step went through:
//   8 x 0.5 = 4/s            (before: 8/s, a completion message for every job, failed or not)
TEST_CASE(
    "a worker pool's second then step is sent only the jobs whose first step delivered: 4 of 8",
    "[theory]") {
  auto pool = group_of("w", 10, 1s);
  pool.then = {Step{.to = "render", .op = "job"}, Step{.to = "done", .op = "job"}};
  auto s = published(8, {std::move(pool)});
  s.nodes.push_back(lossy_target("render", 4));
  s.nodes.push_back(plain_target("done"));
  s.links.push_back(LinkSpec{.from = "w", .to = "render", .latency = 0s});
  s.links.push_back(LinkSpec{.from = "w", .to = "done", .latency = 0s});
  const auto g = *Graph::build(s);
  const auto records = records_of(g, 50);
  REQUIRE_THAT(demand_at(records.front(), g, "done"), WithinRel(8.0, 1e-9));  // one step late
  const auto& last = records.back();
  REQUIRE_THAT(demand_at(last, g, "w"), WithinRel(8.0, 1e-9));
  REQUIRE_THAT(demand_at(last, g, "render"), WithinRel(8.0, 1e-9));  // the first step: every job
  REQUIRE_THAT(dropped_share_at(last, g, "render"), WithinRel(0.5, 1e-9));  // 4 of the 8
  REQUIRE_THAT(demand_at(last, g, "done"), WithinRel(4.0, 1e-3));           // 8 x 0.5
}
