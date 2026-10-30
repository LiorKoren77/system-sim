// Whole-simulation checks against closed-form queueing theory (spec §12).
// Spec entries pinned here: §19 entries 4-6 (message queues and their lag, worker pools,
// asynchronous completion).
// This file: message queues and worker pools: backlog growth, retention, a pool's then steps, queue
// lag. One of the nine files the theory tests are split into by topic; the scenario builders more
// than one of them needs live in tests/support/fixtures.cppm (module simsys.test_fixtures).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
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
import simsys.queueing;
import simsys.test_fixtures;
import simsys.workload;

// A note for test authors: `Approx(x).margin(m)` passes when the value is within the
// absolute margin m OR within Approx's default relative epsilon (about 1.19e-5), so a
// margin can only loosen a check, never tighten it. For a tight check use WithinAbs /
// WithinRel, which apply only the tolerance you give them.
using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using simsys::Rate;
using simsys::engine::Simulation;
using simsys::graph::Graph;
using simsys::model::LinkSpec;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
using simsys::model::Step;
// Shared builders (tests/support/fixtures.cppm).
using simsys::test_support::first_step;
using simsys::test_support::group_of;
using simsys::test_support::published;
using simsys::test_support::records_of;
using simsys::test_support::simulation_of;

// ---- Message queues hold what worker pools have not drained; pools are M/M/c over consumption
// (spec §19 entries 4-6) ----
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
// group_of(id, workers, cost), a pool draining `q`, and published(rate, groups, retention), the
// client -> q -> groups picture, are shared with the sink and failed-step tests:
// tests/support/fixtures.cppm.

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

TEST_CASE("Simulation::create accepts a message queue consumed by a pool without then steps",
          "[theory]") {
  const auto g = *Graph::build(published(1000, {four_workers()}));
  const auto sim = Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
}

// An earlier version refused a pool's `then` steps ("run in phase 2c's next step") rather than drop
// them silently; they run now, so the refusal is now its opposite: the engine accepts a pool that
// hands its drained jobs on (what it sends is pinned in the section on a pool's then steps below).
TEST_CASE("Simulation::create accepts a worker pool that hands its jobs on through then steps",
          "[theory]") {
  auto pool = four_workers();
  // C++20 designated initializers: `.to = ...` names each member being set, in
  // declaration order, and leaves the rest at their defaults. Replaces positional braces
  // (which break silently when members are reordered) or a default object assigned field
  // by field; here a step reads like the YAML it stands for.
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

// ---- A worker pool's then steps carry each DRAINED job on (spec §19 entry 5) ----
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

// simulation_of(g), a fresh simulation that fails the test when the engine refuses `g`, is shared
// with the sink, kv and failed-step tests: tests/support/fixtures.cppm.

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

// A worker is busy for `cost` only (spec §19 entry 5, ignored: a worker blocked on a slow
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

// ---- Queue lag: how far a message queue trails its consumers (spec §19 entry 4) ----
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
// and the queue's lag is its WORST group's: a consumer group that is far behind is a queue that is
// far behind. It is the wait only: the pool's own `latency` is wait + `cost`, the lag has no cost
// in it. (It is the same wait fluid::FluidQueue computes for the pool, `NodeSample::queue_lag_s`,
// empty on every node that is not a message queue.)
//
// Numbers, with the pictures above: 12 workers at 10 ms are 1200 jobs/s exactly, and the burst's
// 1500/s grows the backlog by 300/s = 30 per 100 ms step, so after step i (the record whose t ends
// at (i + 1) x 0.1 s) the backlog is (i + 1) x 30 messages and the fluid lag
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

// The steady lag of the picture above (1000/s into 4 workers at 3.333 ms, rho 0.8333, no backlog)
// is the pool's Erlang wait alone, 3.289 ms: the lag leaves out the 3.333 ms of `cost` that the
// pool's 6.622 ms sojourn includes.
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
