// A client's `timeout` against closed-form results (plan 2c Task 13; book ch. 1 and ch. 8: a
// client that gives up on a slow request). Model under test: the client's response time is fitted
// as a lognormal with the path's mean and variance (the fit the reports already use); the share
// below the timeout is answered, with the truncated moments; the rest is timed out. The servers
// still do the work of a timed-out request: nothing cancels it, so their demand is unchanged.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.config;
import simsys.core;
import simsys.engine;
import simsys.graph;
import simsys.model;
import simsys.report;

// A note for test authors: `Approx(x).margin(m)` passes when the value is within the absolute
// margin m OR within Approx's default relative epsilon, so a margin can only loosen a check.
// For a tight check use WithinAbs / WithinRel, which apply only the tolerance you give them.
using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using simsys::Rate;
using simsys::engine::Simulation;
using simsys::graph::Graph;
using simsys::model::Direction;
using simsys::model::LinkSpec;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
using simsys::model::Workload;

namespace {
// One client, one M/M/1 server (1000/s, 1 ms service), a zero-latency link. At 990 requests/s
// the server runs at rho = 0.99, so the client's response is the server's sojourn time:
//   mean     W = s / (1 - rho) = 1 ms / 0.01 = 100 ms                       (M/M/1 sojourn)
//   variance   = W^2 = 0.01 s^2: an M/M/1 sojourn time is exponential, so its standard deviation
//                equals its mean (the engine adds the wait's variance rho (2 - rho) theta^2, theta
//                = W, to the service's s^2: 0.0099990 + 0.000001 = 0.01 exactly).
// That is the 100 ms / 0.01 s^2 path of the plan's worked example (CV 1, sigma^2 = ln 2), reached
// without a variability key. `timeout` is the client's, absent for none.
constexpr double demand_per_s = 990.0;

Scenario slow_exponential_path(std::optional<simsys::SimTime> timeout) {
  // C++20 designated initializers: `.name = ...` names each member being set, in declaration
  // order, and leaves the rest at their defaults. Replaces positional braces (which break
  // silently when members are reordered): a scenario reads like the YAML it stands for.
  Scenario s{.name = "timeout", .version = 1, .duration = 1s};
  s.nodes = {
      NodeSpec{.id = "c",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{demand_per_s}},
                                    .op_shares = {{"op", 1.0}},
                                    .timeout = timeout}},
      NodeSpec{.id = "s", .type = NodeType::service, .capacity = Rate{1000}, .latency = 1ms}};
  s.links = {LinkSpec{.from = "c", .to = "s", .dir = Direction::inout, .latency = 0s}};
  return s;
}

// The first step of a fresh simulation of `s`.
simsys::engine::StepRecord first_step(const Scenario& s, const Graph& g) {
  auto sim = Simulation::create(g);
  if (!sim) FAIL(sim.error().message);
  return sim->step(0s, s.step);
}

// The whole scenario run and summarised from its first step (no warmup).
simsys::report::Summary summarize(const Graph& g, simsys::SimTime duration, simsys::SimTime step) {
  simsys::report::Summary summary{g, 0s};
  auto sim = Simulation::create(g);
  if (!sim) FAIL(sim.error().message);
  sim->run({.duration = duration, .step = step}, [&](const auto& r) { summary.observe(r); });
  return summary;
}

// The number after `"key": ` in the JSON line of one client op (render_json prints each op as
// one `{"client": ..., "op": ..., ...}` line). The test fails when the op or the key is missing.
double op_field(const std::string& json, std::string_view client, std::string_view op,
                std::string_view key) {
  const std::string head =
      "{\"client\": \"" + std::string{client} + "\", \"op\": \"" + std::string{op} + "\"";
  const auto start = json.find(head);
  REQUIRE(start != std::string::npos);
  const auto end = json.find('\n', start);
  const std::string line = json.substr(start, end - start);
  const std::string field = "\"" + std::string{key} + "\": ";
  const auto at = line.find(field);
  CAPTURE(line);
  REQUIRE(at != std::string::npos);
  return std::stod(line.substr(at + field.size()));
}
}  // namespace

// Precondition of every test below: the path is what the fixture says it is. An M/M/1 server at
// rho 0.99 answers in 100 ms on average with a variance of 0.01 s^2 (hand-derived above); the
// engine reports exactly that when the client has no timeout.
TEST_CASE("an M/M/1 server at rho 0.99 gives the client a 100 ms response with variance 0.01 s2",
          "[theory]") {
  const auto s = slow_exponential_path(std::nullopt);
  const auto g = *Graph::build(s);
  const auto r = first_step(s, g);
  REQUIRE(r.responses.size() == 1);
  REQUIRE_THAT(r.nodes[*g.index_of("s")].rho, WithinRel(0.99, 1e-12));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(0.1, 1e-9));
  REQUIRE_THAT(r.responses[0].response.variance_s2, WithinRel(0.01, 1e-9));
}

// timeout 200 ms on a path of mean 100 ms and variance 0.01 s^2 (the worked example of
// lognormal_below in test_queueing.cpp): sigma^2 = ln(1 + 0.01 / 0.1^2) = ln 2, mu = -2.649159,
//   answered share = Phi((ln 0.2 - mu) / sigma) = Phi(1.248832) = 0.894137,
//   answered mean  = E[X | X < 200 ms] = 0.073970 s,   answered variance = 0.0021022 s^2.
// Of 990 requests/s: 885.2 answered, 104.8 timed out. Percentiles are capped at the timeout: the
// refitted lognormal of the answered part (mean 73.97 ms, SCV 0.384, so sigma = 0.570 and
// mu = -2.767) would put its p99 at exp(mu + 2.3263 sigma) = 236.7 ms, past the 200 ms nothing
// can take, so the p99 IS 200 ms; the p95 (160.6 ms) is under the cap and is not pinned.
TEST_CASE("a 200 ms timeout on a 100 ms exponential path answers 89.4 percent of the requests",
          "[theory]") {
  const auto s = slow_exponential_path(200ms);
  const auto g = *Graph::build(s);
  const auto r = first_step(s, g);
  REQUIRE(r.responses.size() == 1);
  REQUIRE_THAT(r.responses[0].offered.per_second, WithinRel(demand_per_s, 1e-12));
  REQUIRE_THAT(r.responses[0].answered.per_second, WithinRel(0.894137 * demand_per_s, 1e-5));
}

TEST_CASE(
    "the answered requests' latency is the lognormal's truncated moments: 73.97 ms, 0.0021022 s2",
    "[theory]") {
  const auto s = slow_exponential_path(200ms);
  const auto g = *Graph::build(s);
  const auto response = first_step(s, g).responses[0].response;
  REQUIRE_THAT(response.mean_s, WithinAbs(0.073970, 5e-7));
  REQUIRE_THAT(response.variance_s2, WithinAbs(0.0021022, 5e-8));
}

// timed_out_per_s is read from the JSON report, where the plan puts it: 0.105863 x 990 = 104.81/s
// (the complement of the answered share), and answered + timed out = offered (no request is lost
// to the server here: it never drops at rho 0.99, its queue is empty in the fluid sense).
TEST_CASE("the JSON report counts 10.59 percent of the offered requests as timed out", "[theory]") {
  const auto s = slow_exponential_path(200ms);
  const auto g = *Graph::build(s);
  const auto json = simsys::report::render_json(summarize(g, 1s, s.step), {});
  CAPTURE(json);
  const double offered = op_field(json, "c", "op", "offered_per_s");
  const double answered = op_field(json, "c", "op", "answered_per_s");
  const double timed_out = op_field(json, "c", "op", "timed_out_per_s");
  REQUIRE_THAT(offered, WithinRel(demand_per_s, 1e-9));
  REQUIRE_THAT(timed_out, WithinRel(0.105863 * demand_per_s, 1e-5));
  REQUIRE_THAT(answered + timed_out, WithinRel(offered, 1e-9));
  const double mean = op_field(json, "c", "op", "mean_s");
  REQUIRE_THAT(mean, WithinAbs(0.073970, 5e-7));
}

TEST_CASE("the reported p99 of a client with a 200 ms timeout is capped at 200 ms", "[theory]") {
  const auto s = slow_exponential_path(200ms);
  const auto g = *Graph::build(s);
  const auto ops = summarize(g, 1s, s.step).ops();
  REQUIRE(ops.size() == 1);
  REQUIRE_THAT(ops[0].p99_s, WithinRel(0.2, 1e-12));
  REQUIRE(ops[0].p50_s <= ops[0].p95_s);
  REQUIRE(ops[0].p95_s <= ops[0].p99_s);
  // Without the timeout the same path's p99 is the lognormal's 490 ms: the cap is what changed it.
  const auto free = summarize(*Graph::build(slow_exponential_path(std::nullopt)), 1s, s.step).ops();
  REQUIRE_THAT(free[0].p99_s, WithinAbs(0.490492, 1e-6));
}

// Nothing cancels a request the client gave up on: the server still receives, queues and serves
// it, so its demand and utilisation are the same with and without the timeout. Only what the
// client counts as answered changes.
TEST_CASE("a timeout leaves the server's demand and utilisation as they were and answers fewer",
          "[theory]") {
  const auto with_timeout_s = slow_exponential_path(200ms);
  const auto with_timeout_g = *Graph::build(with_timeout_s);
  const auto with_timeout = first_step(with_timeout_s, with_timeout_g);
  const auto without_s = slow_exponential_path(std::nullopt);
  const auto without_g = *Graph::build(without_s);
  const auto without = first_step(without_s, without_g);
  const auto& served = with_timeout.nodes[*with_timeout_g.index_of("s")];
  const auto& reference = without.nodes[*without_g.index_of("s")];
  REQUIRE_THAT(served.demand_per_s, WithinRel(demand_per_s, 1e-12));
  REQUIRE(served.demand_per_s == reference.demand_per_s);
  REQUIRE(served.rho == reference.rho);
  REQUIRE(served.latency.mean_s == reference.latency.mean_s);
  REQUIRE(with_timeout.responses[0].answered.per_second < without.responses[0].answered.per_second);
}

// A tighter timeout cuts more: 50 ms on the same path answers Phi(-0.416277) = 0.338604 of the
// requests (the 33.86 % of the unit test), with the truncated mean 31.265 ms.
TEST_CASE("a 50 ms timeout on the same path answers 33.86 percent and a 1 s timeout 99.93 percent",
          "[theory]") {
  const auto tight_s = slow_exponential_path(50ms);
  const auto tight = first_step(tight_s, *Graph::build(tight_s)).responses[0];
  REQUIRE_THAT(tight.answered.per_second, WithinRel(0.338604 * demand_per_s, 1e-5));
  REQUIRE_THAT(tight.response.mean_s, WithinAbs(0.031265, 5e-7));
  const auto loose_s = slow_exponential_path(1s);
  const auto loose = first_step(loose_s, *Graph::build(loose_s)).responses[0];
  REQUIRE_THAT(loose.answered.per_second, WithinRel(0.999269 * demand_per_s, 1e-5));
}

// Without a `timeout` the client waits for ever: every request is answered, nothing times out and
// the path's own moments come through (the report's JSON says 0 for the timed-out rate).
TEST_CASE(
    "a client without a timeout answers everything it offers and reports no timed out requests",
    "[theory]") {
  const auto s = slow_exponential_path(std::nullopt);
  const auto g = *Graph::build(s);
  const auto response = first_step(s, g).responses[0];
  REQUIRE(response.answered.per_second == response.offered.per_second);
  REQUIRE_THAT(response.response.mean_s, WithinRel(0.1, 1e-9));
  const auto json = simsys::report::render_json(summarize(g, 1s, s.step), {});
  CAPTURE(json);
  // Locals, not nested calls: op_field itself REQUIREs, and Catch2 cannot nest an assertion inside
  // another one's expression.
  const double timed_out = op_field(json, "c", "op", "timed_out_per_s");
  const double answered = op_field(json, "c", "op", "answered_per_s");
  REQUIRE(timed_out == 0.0);
  REQUIRE_THAT(answered, WithinRel(demand_per_s, 1e-9));
}

// ---- A latency SLO against a timeout (review of plan 2c Task 13, C-1) ----
//
// The same exponential path, mean 100 ms (lognormal fit: sigma^2 = ln 2, sigma = 0.83255,
// mu = ln 0.1 - sigma^2 / 2 = -2.64916). The share of it slower than a timeout L is
//   P(X > L) = 1 - Phi((ln L - mu) / sigma)
//   L = 391 ms:  z = (-0.93905 + 2.64916) / 0.83255 = 2.0541, P = 0.0200: 2 % time out
//   L = 604 ms:  z = (-0.50418 + 2.64916) / 0.83255 = 2.5764, P = 0.0050: 0.5 % time out
// At 391 ms more than the 1 % a p99 leaves above it time out: no request at the 99th percentile
// was answered, so the client's p99 is the timeout and a p99 SLO fails whatever its max. The
// answered requests' own refit has its p99 at 357 ms (0.91 of the timeout), so without the rule a
// max of 380 ms would pass on requests nobody answered (it failed at 490 ms before the timeout).
// At 604 ms only 0.5 % time out: the p99 and the p50 were answered, and their capped percentiles
// pass any max at or above the timeout.
namespace {
simsys::model::SloSpec latency_slo(simsys::model::SloMetric metric, simsys::SimTime max) {
  return simsys::model::SloSpec{.metric = metric, .node = "c", .op = "op", .max = max};
}

std::vector<simsys::report::SloResult> verdicts(simsys::SimTime timeout,
                                                std::vector<simsys::model::SloSpec> slos) {
  const auto s = slow_exponential_path(timeout);
  const auto g = *Graph::build(s);
  return simsys::report::evaluate_slos(summarize(g, 1s, s.step), slos);
}
}  // namespace

TEST_CASE("a p99 SLO of 380 ms fails when 2 percent time out at 391 ms: it observes the timeout",
          "[theory]") {
  using simsys::model::SloMetric;
  const auto results = verdicts(391ms, {latency_slo(SloMetric::p99_latency, 380ms)});
  REQUIRE(results.size() == 1);
  CAPTURE(results[0].note);
  REQUIRE(results[0].status == simsys::report::SloStatus::fail);
  REQUIRE(results[0].observed == 0.391);
  // C++20 std::string::ends_with: a suffix test in one call, replacing a compare() on the tail
  // (or rfind arithmetic); the share's digits in front are whatever the fit gives (about 2).
  REQUIRE(results[0].note.ends_with("% of delivered requests timed out (more than 1%)"));
}

TEST_CASE("p99 and p50 SLOs at the timeout pass when only 0.5 percent time out at 604 ms",
          "[theory]") {
  using simsys::model::SloMetric;
  const auto results = verdicts(
      604ms, {latency_slo(SloMetric::p99_latency, 604ms),
              latency_slo(SloMetric::p50_latency, 604ms), latency_slo(SloMetric::p99_latency, 1s)});
  REQUIRE(results.size() == 3);
  for (const auto& result : results) {
    CAPTURE(result.note);
    REQUIRE(result.status == simsys::report::SloStatus::pass);
    REQUIRE(result.note.empty());
    REQUIRE(*result.observed <= 0.604);
  }
}

// ---- The shipped youtube scenario: 300 MB uploads against a 10 s timeout ----
//
// An upload is a write: the blob store acknowledges it at its last byte, 300 MB x 40 ms per MB =
// 12 s after its wait (response p99 12.40 s, see test_theory.cpp's "a 300 MB upload's p99 is 12.4
// s"), and the transfer is deterministic, so its response has almost no spread (variance 0.027 s^2:
// sigma = 0.0137). The share below the clients' `timeout: 10s` is
//   Phi((ln 10 - mu) / sigma) = Phi(-13.5) ~ 1e-41: no upload is answered in time.
// This pin documents the CURRENT state of the scenario: Task 16 may change the timeout or the
// upload path (resumable uploads, as the book describes them), and this test then moves with it.
// SIMSYS_SCENARIOS_DIR is injected by CMake (a compile definition: the one macro we accept,
// because it carries a build-time path no language feature can).
TEST_CASE("youtube's 300 MB uploads all time out against the clients' 10 s timeout", "[theory]") {
  auto loaded =
      simsys::config::load(std::string{SIMSYS_SCENARIOS_DIR} + "/book/youtube/scenario.yaml");
  if (!loaded)
    FAIL(loaded.error().file << ":" << loaded.error().line << ": " << loaded.error().message);
  const auto built = Graph::build(*loaded);
  if (!built) FAIL(built.error().message);
  // The scenario runs 24 h at 100 ms; its first ten seconds say the same (the diurnal wave barely
  // moves, and a response is the sum of the path's hops). The 5 m warmup would hide all of it.
  simsys::report::Summary summary{*built, 3s};
  auto sim = Simulation::create(*built);
  if (!sim) FAIL(sim.error().message);
  sim->run({.duration = 10s, .step = loaded->step}, [&](const auto& r) { summary.observe(r); });
  const auto json = simsys::report::render_json(summary, {});
  CAPTURE(json);
  for (const std::string_view client : {"viewers_us", "viewers_eu"}) {
    CAPTURE(client);
    const double offered = op_field(json, client, "upload", "offered_per_s");
    REQUIRE(offered > 1.0);  // 2 % of 200/s and of 100/s, times the curve's phase
    const double timed_out = op_field(json, client, "upload", "timed_out_per_s");
    const double answered = op_field(json, client, "upload", "answered_per_s");
    REQUIRE_THAT(timed_out, WithinRel(offered, 1e-6));
    REQUIRE(answered < 1e-6 * offered);
  }
}
