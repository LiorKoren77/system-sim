// Whole-simulation checks against closed-form queueing theory (spec §12).
// Spec entries pinned here: §19 entry 8 (the external sink); phase 3 spec §6.2 (pool retries and
// the dead-letter count). This file: external sinks, a worker pool that calls one, and a pool that
// retries what the sink failed. One of the nine files the theory tests are split into by topic; the
// scenario builders more than one of them needs live in tests/support/fixtures.cppm (module
// simsys.test_fixtures).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <optional>
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
using simsys::model::SloMetric;
using simsys::model::SloSpec;
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

// ---- A pool retries the sink's failures after a backoff and dead-letters the last attempt's ----
// (phase 3 spec §6.2; book ch. 10: a notification worker that gets a 5xx from APNs puts the message
// back and tries again later, and one that has tried enough parks it in a dead-letter queue: the
// number the on-call team watches)
//
// The picture: a queue q fed 1000 messages/s; pool w (100 workers at 10 ms = 10k jobs/s, rho 0.1)
// drains it and hands each job on to the sink `api` ({to: api, op: send}); `api` fails the share
// f = 0.02 of what it serves. With `retries: n` and `retry_after: b` the pool puts the jobs whose
// call failed back on its own queue, b later, up to n times. The sink gets 5k/s of capacity here
// (chosen: its type's 1k/s quota is exactly the 1000/s offered, rho 1, and any retry would then
// overload the sink itself; these tests are about the failures, not about its quota).
//
// The fluid model's closed forms (shares, not individual jobs: every attempt fails with the same
// share f, independently of the others):
//   A job makes its k-th retry only if every earlier attempt failed, which happens with
//   probability f^k, so the attempts per job are a geometric series cut after n + 1 terms:
//     A(f, n) = 1 + f + f^2 + ... + f^n = (1 - f^(n+1)) / (1 - f)
//   n = 3, f = 0.02: 1 + 0.02 + 0.0004 + 0.000008 = 1.020408.
//   A job is dead-lettered when its last attempt fails as well: f^(n+1) of the jobs.
//     n = 3: 0.02^4 = 1.6e-7, so 1000/s x 1.6e-7 = 1.6e-4 dead letters a second.
//   Everything the pool sends the sink is a call, so the sink's demand is lambda x A = 1020.408/s
//   and, as before, the failed share of it is the SINK's drops: 0.02 x 1020.408 = 20.408/s (the
//   attempts that failed and were retried are drops of the sink, the dead letters are the pool's).
//   The sink answers (1 - f) x 1020.408 = lambda (1 - f^(n+1)) = 999.99984/s: every job is answered
//   once or dead-lettered once, answered + dead-lettered = lambda.
// The backoff in steps is retry_after / step = 1 s / 100 ms = 10: the share that failed at step t
// is back on the pool's queue at step t + 10 (the transient test below). (Node indices: u 0, q 1, w
// 2, api 3.)

namespace {
double attempts_per_job(double failure_rate, int retries) {
  double sum = 0.0;
  double chance_of_reaching_this_attempt = 1.0;  // f^k for the k-th retry
  for (int k = 0; k <= retries; ++k) {
    sum += chance_of_reaching_this_attempt;
    chance_of_reaching_this_attempt *= failure_rate;
  }
  return sum;
}

double dead_letter_share(double failure_rate, int retries) {
  return std::pow(failure_rate, retries + 1);
}

constexpr double publish_rate = 1000.0;  // jobs a second
constexpr std::size_t q_index = 1;
constexpr std::size_t w_index = 2;
constexpr std::size_t api_index = 3;

// The sink of the retry pictures: `failure_rate`, 5k/s of capacity (see above), the type's 100 ms.
NodeSpec provider(std::string id, double failure_rate,
                  std::optional<simsys::SimTime> latency = {}) {
  NodeSpec sink{.id = std::move(id), .type = NodeType::external_sink, .capacity = Rate{5000}};
  sink.latency = latency;
  sink.failure_rate = failure_rate;
  return sink;
}

// The picture above: q fed `publish_rate`, pool w (100 workers, 10 ms) calling `api`, which fails
// `failure_rate`; the pool retries `retries` times `retry_after` later when they are given (a pool
// that states none is phase 2's).
Scenario retrying_pool(double failure_rate, std::optional<int> retries,
                       std::optional<simsys::SimTime> retry_after = std::nullopt) {
  auto pool = group_of("w", 100, 10ms);
  pool.then = {Step{.to = "api", .op = "send"}};
  if (retries) pool.retries = *retries;
  if (retry_after) pool.retry_after = *retry_after;
  auto s = published(publish_rate, {std::move(pool)});
  s.nodes.push_back(provider("api", failure_rate));
  s.links.push_back(LinkSpec{.from = "w", .to = "api", .latency = 0s});
  return s;
}
}  // namespace

TEST_CASE("a pool that retries 3 times calls a 2 percent sink 1.020408 times per job", "[theory]") {
  const auto g = *Graph::build(retrying_pool(0.02, 3, 1s));
  const auto records = records_of(g, 60);  // well past the 3 backoffs of 10 steps
  const auto& last = records.back();
  const double attempts = attempts_per_job(0.02, 3);
  REQUIRE_THAT(attempts, WithinRel(1.020408, 1e-6));  // the napkin number
  const auto& pool = last.nodes[w_index];
  const auto& api = last.nodes[api_index];
  REQUIRE_THAT(api.demand_per_s, WithinRel(publish_rate * attempts, 1e-9));
  REQUIRE_THAT(api.demand_per_s, WithinRel(1020.408, 1e-6));
  REQUIRE_THAT(pool.demand_per_s, WithinRel(1020.408, 1e-6));  // it drains every attempt
  REQUIRE_THAT(pool.drain_per_s, WithinRel(1020.408, 1e-6));
  // The re-published jobs are published to the pool's own queue again: q appends them too.
  REQUIRE_THAT(last.nodes[q_index].demand_per_s, WithinRel(1020.408, 1e-6));
  // The sink's failures are its drops, as without retries: 0.02 x 1020.408; it answers the rest.
  REQUIRE_THAT(api.dropped_per_s, WithinRel(0.02 * 1020.408, 1e-6));              // 20.408/s
  REQUIRE_THAT(api.drain_per_s, WithinRel(publish_rate * (1.0 - 1.6e-7), 1e-9));  // 999.99984/s
  REQUIRE(api.rejected_per_s == 0.0);
}

// A job whose last attempt failed is neither the pool's drop nor the sink's: it is a dead letter,
// 1000 x 0.02^4 = 1.6e-4 a second, and every job is answered or dead-lettered exactly once.
TEST_CASE("the pool dead-letters 1000 x 0.02^4 = 1.6e-4 jobs a second and drops none", "[theory]") {
  const auto g = *Graph::build(retrying_pool(0.02, 3, 1s));
  const auto records = records_of(g, 60);
  const auto& last = records.back();
  const auto& pool = last.nodes[w_index];
  REQUIRE(pool.dead_lettered_per_s.has_value());
  REQUIRE_THAT(*pool.dead_lettered_per_s, WithinRel(1.6e-4, 1e-6));
  REQUIRE_THAT(*pool.dead_lettered_per_s,
               WithinRel(publish_rate * dead_letter_share(0.02, 3), 1e-9));
  REQUIRE(pool.dropped_per_s == 0.0);  // a dead letter is not a drop
  REQUIRE(pool.rejected_per_s == 0.0);
  // Answered + dead-lettered = the jobs published, 1000/s: nothing else was lost.
  REQUIRE_THAT(last.nodes[api_index].drain_per_s + *pool.dead_lettered_per_s,
               WithinRel(publish_rate, 1e-9));
}

// The estimate settles on the same fixed point as the run (spec §11: "estimate settles").
TEST_CASE("estimate settles on the retry fixed point: sink demand 1020.408, dead letters 1.6e-4",
          "[theory]") {
  const auto g = *Graph::build(retrying_pool(0.02, 3, 1s));
  const auto e = simulation_of(g).estimate();
  REQUIRE(e.converged);
  REQUIRE_THAT(e.nodes[api_index].demand_per_s, WithinRel(1020.408, 1e-6));
  REQUIRE_THAT(e.nodes[api_index].dropped_per_s, WithinRel(20.408, 1e-5));
  REQUIRE_THAT(e.nodes[q_index].demand_per_s, WithinRel(1020.408, 1e-6));
  REQUIRE(e.nodes[w_index].dead_lettered_per_s.has_value());
  REQUIRE_THAT(*e.nodes[w_index].dead_lettered_per_s, WithinRel(1.6e-4, 1e-6));
  REQUIRE(e.nodes[w_index].dropped_per_s == 0.0);
}

// The re-published share is back one backoff after it failed, so the sink's demand climbs in
// levels, one per backoff of 10 steps (1 s / 100 ms), each level the previous one plus the failures
// of the attempts that were new a backoff ago:
//   steps  0..9   1000                       (the first attempts only)
//   steps 10..19  1000 + 1000 x 0.02         = 1020
//   steps 20..29  1000 + 20 + 20 x 0.02      = 1020.4
//   steps 30..    1000 + 20 + 0.4 + 0.4 x 0.02 = 1020.408
// The last level holds the third retry's attempts, the last ones: what they fail is dead-lettered
// (0.008 x 0.02 = 1.6e-4), not published again.
// The pool reads the sink's failure share one simulation step late, like every thinning in the
// engine (a call's delivered fraction is the end of the LAST step's: the first step, which has no
// last step, sees none fail), so the first failures can only be re-published one step after the
// ones a backoff would give: the levels may start at step 10 or at step 11. The first step of each
// level (10, 20, 30) is therefore pinned to lie between the two levels it separates; every other
// step is pinned to its level.
TEST_CASE(
    "the failed share comes back one backoff later: the sink's demand steps up every 10 steps",
    "[theory]") {
  const auto g = *Graph::build(retrying_pool(0.02, 3, 1s));
  const auto records = records_of(g, 50);
  const auto demand = [&](std::size_t step) { return records[step].nodes[api_index].demand_per_s; };
  const auto between = [&](std::size_t step, double below, double above) {
    CAPTURE(step);
    REQUIRE(demand(step) >= below * (1.0 - 1e-9));
    REQUIRE(demand(step) <= above * (1.0 + 1e-9));
  };
  const auto level = [&](std::size_t from, std::size_t to, double expected) {
    for (std::size_t step = from; step <= to; ++step) {
      CAPTURE(step);
      REQUIRE_THAT(demand(step), WithinRel(expected, 1e-9));
    }
  };
  level(0, 9, 1000.0);
  between(10, 1000.0, 1020.0);
  level(11, 19, 1020.0);
  between(20, 1020.0, 1020.4);
  level(21, 29, 1020.4);
  between(30, 1020.4, 1020.408);
  level(31, 49, 1020.408);
}

// Nobody is dead-lettered before a job has used its three retries: the earliest third retry is at
// step 30, so steps 0..29 hold none (and a count of exactly 0, not a vanishing one); from step 34
// on the count is the fixed point's 1.6e-4 (the same one-step slack as above, plus the step the
// sink takes to report the last attempt's failure).
TEST_CASE("no job is dead-lettered before its third retry, then 1.6e-4 a second", "[theory]") {
  const auto g = *Graph::build(retrying_pool(0.02, 3, 1s));
  const auto records = records_of(g, 50);
  for (std::size_t step = 0; step < records.size(); ++step) {
    CAPTURE(step);
    const auto& pool = records[step].nodes[w_index];
    REQUIRE(pool.dead_lettered_per_s.has_value());
    if (step < 30) REQUIRE(*pool.dead_lettered_per_s == 0.0);
    if (step >= 34) REQUIRE_THAT(*pool.dead_lettered_per_s, WithinRel(1.6e-4, 1e-9));
  }
}

// The queue's lag is the pool's wait (NodeSample::queue_lag_s): the re-published jobs are more
// load on the same 100 workers, 1020.408 / 10k = 0.10204 where 1000 / 10k = 0.1 was, and Erlang C
// at c = 100 is steep in rho, so the lag of the queue with retries is Wq(100, 0.10204, 10 ms) and
// above the one of the same queue without (Wq(100, 0.1, 10 ms), a vanishing wait but not zero).
TEST_CASE("the queue's lag includes the re-published jobs: the pool runs at rho 0.10204, not 0.1",
          "[theory]") {
  const auto with = *Graph::build(retrying_pool(0.02, 3, 1s));
  const auto without = *Graph::build(retrying_pool(0.02, std::nullopt));
  const auto lag_with = records_of(with, 60).back().nodes[q_index].queue_lag_s;
  const auto lag_without = records_of(without, 60).back().nodes[q_index].queue_lag_s;
  REQUIRE(lag_with.has_value());
  REQUIRE(lag_without.has_value());
  const double rho_with = publish_rate * attempts_per_job(0.02, 3) * 0.010 / 100.0;
  const double expected_with = simsys::queueing::mmc_wait(100, rho_with, 0.010).mean_s;
  const double expected_without = simsys::queueing::mmc_wait(100, 0.1, 0.010).mean_s;
  REQUIRE(expected_without > 0.0);  // not vacuous: the lag above is a real number to beat
  REQUIRE_THAT(*lag_without, WithinRel(expected_without, 1e-6));
  REQUIRE_THAT(*lag_with, WithinRel(expected_with, 1e-6));
  REQUIRE(*lag_with > *lag_without);
}

// Other retry counts and a failing provider: n retries make A(f, n) attempts and dead-letter
// f^(n+1) of the jobs, and answered + dead-lettered is every job. The last case is the one the
// phase 3 spec uses for client retries, 50 % loss with 2 retries: 1 + 0.5 + 0.25 = 1.75 attempts
// per job, 0.875 of them answered, 0.125 dead-lettered (f^3 = 0.125).
TEST_CASE("n retries make 1 + f + ... + f^n attempts per job and dead-letter f^(n+1) of them",
          "[theory]") {
  struct Case {
    double failure_rate;
    int retries;
  };
  for (const auto& [f, n] : {Case{0.02, 1}, Case{0.02, 2}, Case{0.5, 2}}) {
    CAPTURE(f, n);
    const auto g = *Graph::build(retrying_pool(f, n, 1s));
    const auto records = records_of(g, 60);
    const auto& last = records.back();
    const auto& api = last.nodes[api_index];
    REQUIRE_THAT(api.demand_per_s, WithinRel(publish_rate * attempts_per_job(f, n), 1e-9));
    REQUIRE_THAT(api.drain_per_s, WithinRel(publish_rate * (1.0 - dead_letter_share(f, n)), 1e-9));
    REQUIRE(last.nodes[w_index].dead_lettered_per_s.has_value());
    REQUIRE_THAT(*last.nodes[w_index].dead_lettered_per_s,
                 WithinRel(publish_rate * dead_letter_share(f, n), 1e-9));
    REQUIRE(last.nodes[w_index].dropped_per_s == 0.0);
  }
  const auto g = *Graph::build(retrying_pool(0.5, 2, 1s));
  const auto records = records_of(g, 60);
  const auto& last = records.back();
  REQUIRE_THAT(last.nodes[api_index].demand_per_s, WithinRel(1750.0, 1e-9));
  REQUIRE_THAT(last.nodes[api_index].drain_per_s, WithinRel(875.0, 1e-9));
  REQUIRE_THAT(*last.nodes[w_index].dead_lettered_per_s, WithinRel(125.0, 1e-9));
}

// The pool's own books: a dead letter is not a drop, so the pool keeps the fluid queue's balance
// demand x dt = (drain + dropped) x dt + the change in backlog at every step, the re-published
// jobs in its demand like any others, and the sink keeps its own.
TEST_CASE("a retrying pool and its sink conserve their jobs at every step", "[theory]") {
  const auto g = *Graph::build(retrying_pool(0.02, 3, 1s));
  const auto records = records_of(g, 60);
  const double dt = 0.1;
  for (const std::size_t node : {w_index, api_index}) {
    double previous_backlog = 0.0;
    for (std::size_t step = 0; step < records.size(); ++step) {
      CAPTURE(node, step);
      const auto& sample = records[step].nodes[node];
      const double in = sample.demand_per_s * dt;
      const double out = (sample.drain_per_s + sample.dropped_per_s) * dt;
      REQUIRE_THAT(in, WithinAbs(out + (sample.backlog - previous_backlog), 1e-9));
      previous_backlog = sample.backlog;
    }
  }
  // Not vacuous: the sink really failed jobs and the pool really had a re-published share.
  REQUIRE(records.back().nodes[api_index].dropped_per_s > 20.0);
  REQUIRE(records.back().nodes[w_index].demand_per_s > publish_rate);
}

namespace {
// Both runs' samples, every field, every step, bit for bit (the same standard as the sink that
// fails nothing against the plain queue): the two scenarios may not differ in any number.
void require_identical_runs(const Scenario& a, const Scenario& b, int steps) {
  const auto ga = *Graph::build(a);
  const auto gb = *Graph::build(b);
  const auto ra = records_of(ga, steps);
  const auto rb = records_of(gb, steps);
  for (std::size_t i = 0; i < ra.size(); ++i) {
    for (std::size_t n = 0; n < ra[i].nodes.size(); ++n) {
      CAPTURE(i, n);
      const auto& x = ra[i].nodes[n];
      const auto& y = rb[i].nodes[n];
      REQUIRE(x.demand_per_s == y.demand_per_s);
      REQUIRE(x.drain_per_s == y.drain_per_s);
      REQUIRE(x.dropped_per_s == y.dropped_per_s);
      REQUIRE(x.rejected_per_s == y.rejected_per_s);
      REQUIRE(x.backlog == y.backlog);
      REQUIRE(x.rho == y.rho);
      REQUIRE(x.latency == y.latency);
      REQUIRE(x.zone == y.zone);
      REQUIRE(x.queue_lag_s == y.queue_lag_s);
      REQUIRE(x.dead_lettered_per_s == y.dead_lettered_per_s);
    }
    REQUIRE(ra[i].responses[0].answered == rb[i].responses[0].answered);
    REQUIRE(ra[i].responses[0].response == rb[i].responses[0].response);
  }
}
}  // namespace

// `retries: 0` is the absent key: phase 2's pool, whose failures at the sink are the sink's drops
// and nobody's dead letters. The absent run is also pinned to phase 2's numbers (the sink is sent
// 1000/s at every step and drops 20/s), so the equality below is not one of two broken runs.
TEST_CASE("retries 0 and no retries are the same run: phase 2, 20 of 1000 calls a second dropped",
          "[theory]") {
  const auto absent = retrying_pool(0.02, std::nullopt);
  require_identical_runs(absent, retrying_pool(0.02, 0), 40);
  const auto g = *Graph::build(absent);
  for (const auto& record : records_of(g, 40)) {
    REQUIRE_THAT(record.nodes[api_index].demand_per_s, WithinRel(1000.0, 1e-9));
    REQUIRE_THAT(record.nodes[api_index].dropped_per_s, WithinRel(20.0, 1e-9));
    REQUIRE(record.nodes[w_index].dead_lettered_per_s.has_value());
    REQUIRE(*record.nodes[w_index].dead_lettered_per_s == 0.0);  // none to dead-letter
  }
}

TEST_CASE("retries against a sink that never fails change nothing: no job is published again",
          "[theory]") {
  require_identical_runs(retrying_pool(0.0, 3, 1s), retrying_pool(0.0, std::nullopt), 40);
}

// Only a worker pool has dead letters: the sample says 0 on a pool that retries nothing (it has the
// counter, at 0) and says nothing, not 0, on the nodes around it (the way `queue_lag_s` is only a
// queue's), in a run and in estimate().
TEST_CASE("dead_lettered_per_s is present on the pool and absent on the nodes around it",
          "[theory]") {
  for (const auto retries : {std::optional<int>{}, std::optional<int>{3}}) {
    const auto g = *Graph::build(retrying_pool(0.02, retries, 1s));
    const auto r = first_step(g);
    REQUIRE(r.nodes[w_index].dead_lettered_per_s.has_value());
    REQUIRE_FALSE(r.nodes[0].dead_lettered_per_s.has_value());  // the client u
    REQUIRE_FALSE(r.nodes[q_index].dead_lettered_per_s.has_value());
    REQUIRE_FALSE(r.nodes[api_index].dead_lettered_per_s.has_value());
    const auto e = simulation_of(g).estimate();
    REQUIRE(e.nodes[w_index].dead_lettered_per_s.has_value());
    REQUIRE_FALSE(e.nodes[api_index].dead_lettered_per_s.has_value());
  }
}

// ---- What the drop_rate SLO counts (phase 3 spec §6.2) ----
//
// "notification-system's drop_rate SLO is judged on real losses". A failure the pool will retry is
// not a loss: the job is attempted again, and the only loss is the dead letter, which the pool
// counts apart ("never as a drop"). So the report's drop rate of a sink does not count the failures
// of calls from a pool that retries them, whatever the sink's own books say (its samples still
// show demand = drain + dropped, 20.408/s dropped: conservation does not change, and the CSV
// series reads it). Without retries the sink's failures are final: phase 2's drop rate, 0.02.
// (What the plan does not say and is pinned here: the sink's drop_rate, not only the SLO's worst
// node, is 0 with retries; the 1.6e-4/s that really are lost are the pool's dead_lettered_per_s.)

namespace {
// 20 s of the retry picture observed after 5 s of warmup (past the transient: 3 backoffs of 1 s).
// The Summary refers to `g`, which the caller keeps alive.
simsys::report::Summary summarised(const Graph& g) {
  simsys::report::Summary summary{g, 5s};
  simulation_of(g).run({.duration = 20s, .step = 100ms},
                       [&](const simsys::engine::StepRecord& r) { summary.observe(r); });
  return summary;
}

SloSpec drop_rate_at_most(double max) {
  return SloSpec{.metric = SloMetric::drop_rate, .max = max};
}

// C++20 std::span: evaluate_slos takes a non-owning view of the SLOs, and a std::array converts to
// it implicitly (replacing a vector copy or a pointer and a length), so one verdict is judged on
// the spot.
std::vector<simsys::report::SloResult> verdict_of(const simsys::report::Summary& summary,
                                                  const SloSpec& slo) {
  return simsys::report::evaluate_slos(summary, std::array{slo});
}
}  // namespace

TEST_CASE("with retries the sink's drop rate is 0 and the drop_rate SLO of 0.001 passes",
          "[theory]") {
  const auto g = *Graph::build(retrying_pool(0.02, 3, 1s));
  const auto summary = summarised(g);
  REQUIRE_THAT(summary.drop_rate("api"), WithinAbs(0.0, 1e-12));
  REQUIRE_THAT(summary.drop_rate("w"), WithinAbs(0.0, 1e-12));  // a dead letter is no drop
  REQUIRE_THAT(summary.drop_rate(), WithinAbs(0.0, 1e-12));
  const auto results = verdict_of(summary, drop_rate_at_most(0.001));
  REQUIRE(results.size() == 1);
  REQUIRE(results[0].status == simsys::report::SloStatus::pass);
}

TEST_CASE("without retries the sink's failures are drops, 0.02, and the same SLO fails",
          "[theory]") {
  const auto g = *Graph::build(retrying_pool(0.02, std::nullopt));
  const auto summary = summarised(g);
  REQUIRE_THAT(summary.drop_rate("api"), WithinRel(0.02, 1e-9));
  REQUIRE_THAT(summary.drop_rate(), WithinRel(0.02, 1e-9));
  const auto results = verdict_of(summary, drop_rate_at_most(0.001));
  REQUIRE(results.size() == 1);
  REQUIRE(results[0].status == simsys::report::SloStatus::fail);
}

// ---- notification-system's shape: four channels, four providers, one SLO (book ch. 10) ----
//
// The numbers of scenarios/book/notification-system, without the parts phase 3 adds later (the
// fan-out's dedup and the cache, database and limiter in front of it): 1.9k notifications/s (the
// scenario's average) reach a fan-out that publishes one message per channel with the scenario's
// factors, ios 0.6, android 0.3, sms 0.05, email 0.3, so the queues get 1140, 570, 95 and 570 a
// second; each channel's pool calls its provider, which fails the scenario's share (APNs and FCM
// 2 %, the SMS gateway 5 %, e-mail 1 %). The providers get 3k/s of capacity where the scenario
// gives them the type's 1k/s quota: at 1140/s APNs would shed on its own, a sizing question for
// the scenario (its README's), and these tests are about what the failures count as.
//
// With `retries: 3` the pool of channel i dead-letters rate_i x f_i^4:
//   ios      1140 x 0.02^4 = 1.824e-4        android  570 x 0.02^4 = 9.12e-5
//   sms        95 x 0.05^4 = 5.9375e-4       email    570 x 0.01^4 = 5.7e-6
// 8.7305e-4 jobs a second in all, about one a quarter of an hour: the number the on-call team
// would watch, where the drop rate SLO reads 0.

namespace {
struct Channel {
  std::string_view name;
  double share;  // of the notifications: the fan-out's factor
  int workers;
  simsys::SimTime job_cost;
  simsys::SimTime provider_latency;
  double failure_rate;
};

constexpr double notification_rate = 1900.0;
constexpr std::array channels{
    Channel{"ios", 0.6, 50, 20ms, 150ms, 0.02}, Channel{"android", 0.3, 50, 20ms, 150ms, 0.02},
    Channel{"sms", 0.05, 20, 50ms, 500ms, 0.05}, Channel{"email", 0.3, 20, 30ms, 300ms, 0.01}};

// u -> fanout -> one queue per channel -> its pool -> its provider; the pools retry `retries`
// times a second later when it is given.
Scenario notification_shaped(std::optional<int> retries) {
  Scenario s{.name = "notification-shaped", .version = 1, .duration = 20s};
  std::vector<Step> publishes;
  for (const auto& channel : channels) {
    publishes.push_back(
        Step{.to = std::string{channel.name} + "_q", .op = "publish", .factor = channel.share});
  }
  s.nodes.push_back(NodeSpec{.id = "u",
                             .type = NodeType::client,
                             .workload = Workload{.curve = {.base = Rate{notification_rate}},
                                                  .op_shares = {{"notify", 1.0}}}});
  s.nodes.push_back(NodeSpec{.id = "fanout",
                             .type = NodeType::service,
                             .capacity = Rate{20'000},
                             .ops = {{"notify", publishes}}});
  s.links.push_back(LinkSpec{.from = "u", .to = "fanout", .latency = 0s});
  for (const auto& channel : channels) {
    const std::string name{channel.name};
    auto pool = group_of(name + "_workers", channel.workers, channel.job_cost);
    pool.consumes = name + "_q";
    pool.then = {Step{.to = name + "_provider", .op = "send"}};
    if (retries) {
      pool.retries = *retries;
      pool.retry_after = 1s;
    }
    NodeSpec provider_node =
        provider(name + "_provider", channel.failure_rate,
                 std::chrono::duration_cast<simsys::SimTime>(channel.provider_latency));
    provider_node.capacity = Rate{3000};
    s.nodes.push_back(NodeSpec{.id = name + "_q", .type = NodeType::message_queue});
    s.nodes.push_back(std::move(pool));
    s.nodes.push_back(std::move(provider_node));
    s.links.push_back(LinkSpec{.from = "fanout", .to = name + "_q", .latency = 0s});
    s.links.push_back(LinkSpec{.from = name + "_q", .to = name + "_workers", .latency = 0s});
    s.links.push_back(LinkSpec{.from = name + "_workers", .to = name + "_provider", .latency = 0s});
  }
  return s;
}

// The number after `"key": ` in the JSON object of node `id` (which starts `{"id": "<id>"`).
double node_number(const std::string& json, std::string_view id, std::string_view key) {
  const auto start = json.find("{\"id\": \"" + std::string{id} + "\"");
  REQUIRE(start != std::string::npos);
  const std::string object = json.substr(start, json.find('}', start) - start);
  const std::string field = "\"" + std::string{key} + "\": ";
  const auto at = object.find(field);
  INFO("key " << key << " of node " << id << ": " << object);
  REQUIRE(at != std::string::npos);
  return std::stod(object.substr(at + field.size()));
}
}  // namespace

TEST_CASE("notification-system's shape with retries: every channel dead-letters rate x f^4",
          "[theory]") {
  const auto g = *Graph::build(notification_shaped(3));
  const auto json = simsys::report::render_json(summarised(g), {});
  CAPTURE(json);
  double total = 0.0;
  double expected_total = 0.0;
  for (const auto& channel : channels) {
    const double rate = notification_rate * channel.share;
    const double expected = rate * dead_letter_share(channel.failure_rate, 3);
    const double got =
        node_number(json, std::string{channel.name} + "_workers", "dead_lettered_per_s");
    CAPTURE(channel.name, rate, expected);
    REQUIRE_THAT(got, WithinRel(expected, 1e-6));
    total += got;
    expected_total += expected;
  }
  REQUIRE_THAT(expected_total, WithinRel(8.7305e-4, 1e-6));  // the napkin number above
  REQUIRE_THAT(total, WithinRel(8.7305e-4, 1e-6));
}

TEST_CASE("notification-system's shape: drop_rate max 0.001 passes with retries, fails without",
          "[theory]") {
  const auto with = *Graph::build(notification_shaped(3));
  const auto with_summary = summarised(with);
  REQUIRE_THAT(with_summary.drop_rate(), WithinAbs(0.0, 1e-12));  // the worst node of all ten
  const auto passes = verdict_of(with_summary, drop_rate_at_most(0.001));
  REQUIRE(passes.size() == 1);
  REQUIRE(passes[0].status == simsys::report::SloStatus::pass);

  // Without retries the providers' failures are final and the worst node is the SMS gateway's
  // 5 %: the scenario's README says these keep the SLO from passing until phase 3.
  const auto without = *Graph::build(notification_shaped(std::nullopt));
  const auto without_summary = summarised(without);
  REQUIRE_THAT(without_summary.drop_rate("sms_provider"), WithinRel(0.05, 1e-9));
  REQUIRE_THAT(without_summary.drop_rate(), WithinRel(0.05, 1e-9));
  const auto fails = verdict_of(without_summary, drop_rate_at_most(0.001));
  REQUIRE(fails.size() == 1);
  REQUIRE(fails[0].status == simsys::report::SloStatus::fail);
}
