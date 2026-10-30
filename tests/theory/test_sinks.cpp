// Whole-simulation checks against closed-form queueing theory (spec §12).
// This file: external sinks, and a worker pool that calls one. One of the nine files the theory
// tests are split into by topic; the scenario builders more than one of them needs live in
// tests/support/fixtures.cppm (module simsys.test_fixtures).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <utility>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.core;
import simsys.engine;
import simsys.graph;
import simsys.model;
import simsys.queueing;
import simsys.report;
import simsys.test_fixtures;

// A note for test authors: `Approx(x).margin(m)` passes when the value is within the
// absolute margin m OR within Approx's default relative epsilon (about 1.19e-5), so a
// margin can only loosen a check, never tighten it. For a tight check use WithinAbs /
// WithinRel, which apply only the tolerance you give them.
using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using simsys::Rate;
using simsys::graph::Graph;
using simsys::model::LinkSpec;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
using simsys::model::Step;
using simsys::model::Workload;
// Shared builders (tests/support/fixtures.cppm).
using simsys::test_support::first_step;
using simsys::test_support::group_of;
using simsys::test_support::published;
using simsys::test_support::records_of;
using simsys::test_support::simulation_of;

// ---- An external sink answers slowly and fails a share of what it serves (spec §19 entry 8) ----
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
  // C++20 designated initializers: `.name = ...` names each member being set, in
  // declaration order, and leaves the rest at their defaults. Replaces positional braces
  // (which break silently when members are reordered) or a default object assigned field
  // by field; here a scenario reads like the YAML it stands for.
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
