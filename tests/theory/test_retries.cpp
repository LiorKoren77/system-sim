// A client's retries against closed-form results (phase 3 spec §6.1; book ch. 10's retry mechanism
// for the notification system's providers and ch. 4's `Retry-After` of a rate limiter's clients).
// Model under test: a client with `retries: n` sends every request it lost or timed out again
// after `retry_after`, up to n extra attempts. In a fluid engine that is FEEDBACK DEMAND:
//   attempts_t = curve_t + sum over k = 1..n of failed_(t - k b)           (b = backoff in steps)
// where failed_(t - k b) is the share of attempt k - 1 that was lost or timed out at its step: the
// retries of the attempts that failed. Every attempt fails at the current step's rate of the path,
// so a path that loses a share q of what it is offered gives each request the geometric series
//   attempts per request = 1 + q + q^2 + ... + q^n = (1 - q^(n+1)) / (1 - q)
//   answered             = 1 - q^(n+1)           (a request is answered when ANY attempt is)
//   lost                 = q^(n+1)               (its last attempt failed too, counted once)
// The servers do every attempt's work, so the path's demand is the ATTEMPTS, not the curve;
// `offered/s` stays the curve's rate and the storm shows as `attempts/s` above it. Ignored on
// purpose: jitter and exponential backoff (every retry waits exactly `retry_after`), retry
// budgets, and a client that gives up on a retry because the user left.
//
// One step is 100 ms and `retry_after` is 100 ms, so the backoff is b = 1 step: the failed share
// of step t is sent again in step t + 1, and the share of THAT attempt that fails in step t + 2.
// The ramp of attempts per second therefore climbs one attempt a step:
//   step 0: 1000      (no history: only the curve)
//   step 1: 1000 + 1000 q                  = 1500 at q = 0.5
//   step 2: 1000 + 1000 q + 1000 q^2       = 1750, and from then on 1750 for ever.
// The engine's one-step lag in `delivered` (a step after a failed step reads the fractions of the
// END OF THE LAST STEP, test_failed_steps.cpp) does NOT move this ramp when the client calls the
// sink itself: the client's answered share is composed from the sink's delivered fraction of the
// SAME step (the backward pass runs before the client is read), and a sink with room to spare
// loses exactly its failure_rate from the very first step. The lag would enter only through a lossy
// step BEFORE another one in the same op (the second step's flow is thinned by the first's
// delivered share of the step before), which these probes do not have; it is not pinned here.
//
// The fault block (Task 8's, engine::FaultRecord) tells the attempts from the requests: on the
// storm below its `during:` line counts the node's drops as ATTEMPTS, with the requests lost for
// good beside them, and its `after:` line adds the attempts the retries cost, the 5968 this file
// counts (`retries added 5.968k attempts`).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>  // getpid

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.config;
import simsys.core;
import simsys.engine;
import simsys.graph;
import simsys.model;
import simsys.queueing;
import simsys.report;
import simsys.session;
import simsys.test_fixtures;

// A note for test authors: `Approx(x).margin(m)` passes when the value is within the absolute
// margin m OR within Approx's default relative epsilon, so a margin can only loosen a check.
// For a tight check use WithinAbs / WithinRel, which apply only the tolerance you give them.
using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using simsys::Rate;
using simsys::engine::ResponseSample;
using simsys::engine::StepRecord;
using simsys::graph::Graph;
using simsys::model::LinkSpec;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
using simsys::model::Workload;
using simsys::test_support::records_of;
using simsys::test_support::simulation_of;

namespace {

// ---- Reading a step ----------------------------------------------------------------------------

// The attempts per second of one client op's sample: ASSUMED to be `ResponseSample::attempts`, a
// Rate like `offered` and `answered`, declared right after `offered`. If the implementation names
// it differently, this is the one place that changes.
double attempts_of(const ResponseSample& sample) {
  return sample.attempts.per_second;
}

// The probes have one client with one op: its sample is the record's only response.
const ResponseSample& op_of(const StepRecord& record) {
  return record.responses.at(0);
}

// Offered minus answered minus timed out: the requests the client counts as lost this step (the
// report's `lost`, "offered 1000, answered 875, lost 125").
double lost_of(const ResponseSample& sample) {
  return sample.offered.per_second - sample.answered.per_second - sample.timed_out.per_second;
}

// ---- The sink probe: a path that loses a fixed share, whatever it is offered
// ---------------------

// u (`rate`/s of `call`, `retries` retries after 100 ms) -> api, an external sink that fails
// `failure_rate` of what it serves. The sink is given room to spare (100k/s at 1 ms: c = 100
// servers, rho under 3 % at the busiest) so that it NEVER saturates however many attempts arrive:
// its delivered fraction is exactly 1 - failure_rate in every step (test_sinks.cpp derives the
// 10 of 500 of the same sink), the fixed loss rate the closed forms above assume. The link has no
// latency, so what the client sends in a step is what the sink is offered in that step.
Scenario retried_sink(double failure_rate, int retries, double rate = 1000.0) {
  // C++20 designated initializers: `.name = ...` names each member being set, in declaration
  // order, and leaves the rest at their defaults. Replaces positional braces (which break
  // silently when members are reordered): a scenario reads like the YAML it stands for.
  Scenario s{.name = "retried-sink", .version = 1, .duration = 2s, .step = 100ms};
  NodeSpec api{
      .id = "api", .type = NodeType::external_sink, .capacity = Rate{100'000}, .latency = 1ms};
  api.failure_rate = failure_rate;
  Workload workload{.curve = {.base = Rate{rate}}, .op_shares = {{"call", 1.0}}};
  // ASSUMED: `Workload::retries` (a count, default 0) and `Workload::retry_after` (a duration;
  // the loader fills in 100 ms) exist; set by assignment so the order of the members does not
  // matter. The cast keeps the call independent of the count's integer type.
  workload.retries = static_cast<decltype(workload.retries)>(retries);
  workload.retry_after = 100ms;
  s.nodes = {NodeSpec{.id = "u", .type = NodeType::client, .workload = std::move(workload)},
             std::move(api)};
  s.links = {LinkSpec{.from = "u", .to = "api", .latency = 0s}};
  return s;
}

constexpr std::size_t sink_node = 1;  // u is node 0

// A whole probe of `steps` 100 ms steps.
std::vector<StepRecord> run_sink(double failure_rate, int retries, int steps = 20) {
  const auto g = *Graph::build(retried_sink(failure_rate, retries));
  return records_of(g, steps);
}

// ---- Loading text ------------------------------------------------------------------------------

// A scenario file in the system temp directory that removes itself (RAII: also when a REQUIRE
// unwinds the test). The process id is part of the name so two test processes never collide.
struct TempFile {
  std::filesystem::path path;
  explicit TempFile(std::filesystem::path p) : path(std::move(p)) {}
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  ~TempFile() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
};

Scenario load_yaml(std::string_view yaml) {
  const TempFile file{std::filesystem::temp_directory_path() /
                      (std::to_string(::getpid()) + "_simsys_retries.yaml")};
  std::ofstream(file.path) << yaml;
  // C++23 std::expected<T, E>: the loader returns a Scenario or a ConfigError, and the caller
  // must look at which. It replaces exceptions and error codes; the error carries the file,
  // line and message, so a test that cannot load its scenario says why.
  auto loaded = simsys::config::load(file.path.string());
  if (!loaded) {
    FAIL(loaded.error().file << ":" << loaded.error().line << ": " << loaded.error().message);
  }
  return std::move(*loaded);
}

Graph graph_of(Scenario s) {
  auto g = Graph::build(std::move(s));
  if (!g) FAIL(g.error().message);
  return std::move(*g);
}

// The whole scenario run, summarised, as the JSON the `run` command prints.
std::string run_json(const Scenario& s) {
  const auto g = graph_of(s);
  simsys::report::Summary summary{g, s.warmup};
  auto sim = simulation_of(g);
  sim.run({.duration = s.duration, .step = s.step}, [&](const auto& r) { summary.observe(r); });
  return simsys::report::render_json(summary, {});
}

// The number after the first `"key": ` of the JSON (the probes have one client op).
double json_number(const std::string& json, std::string_view key) {
  const std::string field = "\"" + std::string{key} + "\": ";
  const auto at = json.find(field);
  CAPTURE(json);
  REQUIRE(at != std::string::npos);
  return std::stod(json.substr(at + field.size()));
}

constexpr double dt_s = 0.1;  // the step, in seconds
}  // namespace

// ---- 50 percent loss, retries 2: the geometric series 1 + 0.5 + 0.25 ----------------------------
//
// q = 0.5, n = 2, 1000 requests a second:
//   attempt 0 sends 1000, of which 500 fail;   attempt 1 sends those 500, of which 250 fail;
//   attempt 2 sends those 250, of which 125 fail and are lost for good.
// attempts per request = 1 + 0.5 + 0.25 = 1.75: 1750 a second reach the sink, which fails half of
// them (875) and answers the other 875; every request that got an answer on any attempt is
// answered once: 1000 - 125 = 875 = 1000 x (1 - 0.5^3).

TEST_CASE(
    "a client with retries 2 into a path that loses half sends 1000, 1500, then 1750 a second",
    "[theory]") {
  const auto records = run_sink(0.5, 2);
  REQUIRE(records.size() == 20);
  REQUIRE_THAT(attempts_of(op_of(records[0])), WithinRel(1000.0, 1e-9));  // no history yet
  REQUIRE_THAT(attempts_of(op_of(records[1])), WithinRel(1500.0, 1e-9));  // 1000 + 0.5 x 1000
  for (std::size_t i = 2; i < records.size(); ++i) {
    CAPTURE(i);
    REQUIRE_THAT(attempts_of(op_of(records[i])), WithinRel(1750.0, 1e-9));  // 1000 x 1.75
  }
  // offered/s is the curve: the retries are the client's own doing, not new users.
  for (std::size_t i = 0; i < records.size(); ++i) {
    CAPTURE(i);
    REQUIRE_THAT(op_of(records[i]).offered.per_second, WithinRel(1000.0, 1e-12));
  }
}

// The servers do the work of every attempt (nothing cancels a failed one): the sink is offered
// the attempts of the same step, 1000, 1500, 1750, and fails half of whatever it is offered.
TEST_CASE("the sink is offered every attempt: 1000, 1500, then 1750 a second, half of them failing",
          "[theory]") {
  const auto records = run_sink(0.5, 2);
  REQUIRE_THAT(records[0].nodes[sink_node].demand_per_s, WithinRel(1000.0, 1e-9));
  REQUIRE_THAT(records[1].nodes[sink_node].demand_per_s, WithinRel(1500.0, 1e-9));
  for (std::size_t i = 2; i < records.size(); ++i) {
    CAPTURE(i);
    const auto& sink = records[i].nodes[sink_node];
    REQUIRE_THAT(sink.demand_per_s, WithinRel(1750.0, 1e-9));
    REQUIRE_THAT(sink.dropped_per_s, WithinRel(875.0, 1e-9));  // 0.5 x 1750: the failures
    REQUIRE_THAT(sink.drain_per_s, WithinRel(875.0, 1e-9));
  }
}

TEST_CASE("at the fixed point 875 of 1000 requests a second are answered and 125 are lost",
          "[theory]") {
  const auto records = run_sink(0.5, 2);
  for (std::size_t i = 2; i < records.size(); ++i) {
    CAPTURE(i);
    const auto& op = op_of(records[i]);
    REQUIRE_THAT(op.answered.per_second, WithinRel(875.0, 1e-9));  // 1000 x (1 - 0.5^3)
    REQUIRE_THAT(lost_of(op), WithinRel(125.0, 1e-9));             // 1000 x 0.5^3: the last attempt
    REQUIRE(op.timed_out.per_second == 0.0);                       // no timeout, nothing times out
  }
}

// ---- Nothing to retry: attempts are the offered load
// ----------------------------------------------

TEST_CASE("a client with retries 0 offers exactly its curve whatever its path loses", "[theory]") {
  const auto records = run_sink(0.5, 0);
  for (std::size_t i = 0; i < records.size(); ++i) {
    CAPTURE(i);
    const auto& op = op_of(records[i]);
    REQUIRE_THAT(attempts_of(op), WithinRel(op.offered.per_second, 1e-12));
    REQUIRE_THAT(op.answered.per_second, WithinRel(500.0, 1e-9));  // half of 1000: no second chance
    REQUIRE_THAT(records[i].nodes[sink_node].demand_per_s, WithinRel(1000.0, 1e-9));
  }
}

TEST_CASE("a client with retries 2 into a path that loses nothing has attempts equal to offered",
          "[theory]") {
  const auto records = run_sink(0.0, 2);
  for (std::size_t i = 0; i < records.size(); ++i) {
    CAPTURE(i);
    const auto& op = op_of(records[i]);
    REQUIRE_THAT(attempts_of(op), WithinRel(op.offered.per_second, 1e-12));
    REQUIRE_THAT(op.answered.per_second, WithinRel(1000.0, 1e-9));
  }
}

// ---- estimate: the same fixed point
// -----------------------------------------------------------------
//
// `estimate` iterates the feedback to its fixed point (damped like the delivered fractions) and
// stops when nothing moves by more than 1e-12 of its value, so the exact closed forms hold to 1e-6.

TEST_CASE("estimate settles the 50 percent loss at 1.75 attempts a request: 1750 a second",
          "[theory]") {
  const auto g = graph_of(retried_sink(0.5, 2));
  const auto estimate = simulation_of(g).estimate();
  REQUIRE(estimate.converged);
  const auto& op = op_of(estimate);
  REQUIRE_THAT(op.offered.per_second, WithinRel(1000.0, 1e-9));
  REQUIRE_THAT(attempts_of(op), WithinRel(1750.0, 1e-6));
  REQUIRE_THAT(estimate.nodes[sink_node].demand_per_s, WithinRel(1750.0, 1e-6));
  REQUIRE_THAT(op.answered.per_second, WithinRel(875.0, 1e-6));
  REQUIRE_THAT(lost_of(op), WithinRel(125.0, 1e-6));
}

// q = 0.9: attempts per request 1 + 0.9 + 0.81 = 2.71, answered 1 - 0.9^3 = 0.271.
TEST_CASE("estimate settles the 90 percent loss at 2.71 attempts a request: 2710 a second",
          "[theory]") {
  const auto g = graph_of(retried_sink(0.9, 2));
  const auto estimate = simulation_of(g).estimate();
  REQUIRE(estimate.converged);
  const auto& op = op_of(estimate);
  REQUIRE_THAT(attempts_of(op), WithinRel(2710.0, 1e-6));
  REQUIRE_THAT(estimate.nodes[sink_node].demand_per_s, WithinRel(2710.0, 1e-6));
  REQUIRE_THAT(op.answered.per_second, WithinRel(271.0, 1e-6));
}

// The retries cap the series: n retries give (1 - q^(n+1)) / (1 - q) attempts, 1, 1.5, 1.75 and
// 2 - 2^-10 = 1.9990234375 at q = 0.5 (n = 0, 1, 2, 10), and answer 1 - q^(n+1).
TEST_CASE("n retries cap the attempts per request at 1 + q + ... + q^n: 1, 1.5, 1.75, 1.999",
          "[theory]") {
  struct Case {
    int retries;
    double attempts_per_request;
  };
  for (const auto& c : {Case{.retries = 0, .attempts_per_request = 1.0},
                        Case{.retries = 1, .attempts_per_request = 1.5},
                        Case{.retries = 2, .attempts_per_request = 1.75},
                        Case{.retries = 10, .attempts_per_request = 1.9990234375}}) {
    CAPTURE(c.retries);
    const auto g = graph_of(retried_sink(0.5, c.retries));
    const auto estimate = simulation_of(g).estimate();
    REQUIRE(estimate.converged);
    const auto& op = op_of(estimate);
    REQUIRE_THAT(attempts_of(op), WithinRel(1000.0 * c.attempts_per_request, 1e-6));
    REQUIRE_THAT(op.answered.per_second,
                 WithinRel(1000.0 * (1.0 - std::pow(0.5, c.retries + 1)), 1e-6));
  }
}

// ---- The backoff in whole steps, rounded up
// ------------------------------------------------------------
//
// The engine moves in steps, so a backoff is a whole number of them: retry_after / step rounded UP
// (at least one), so that a retry never fires before the wait the scenario states. At 100 ms steps
// 150 ms waits 2 steps (200 ms) and 250 ms 3 steps (300 ms). The first failures happen at t = 0,
// so the first retries are sent at t = 0.2 s and t = 0.3 s: the attempts stay 1000 until then and
// rise to 1500 (1000 + half of the first step's 1000) at that step.

namespace {
std::vector<StepRecord> run_sink_with_backoff(simsys::SimTime retry_after) {
  auto s = retried_sink(0.5, 2);
  s.nodes[0].workload->retry_after = retry_after;
  return records_of(graph_of(std::move(s)), 10);
}

// The time of the first step whose attempts exceed the curve: when the first retries are sent.
double first_retry_at(const std::vector<StepRecord>& records) {
  for (const auto& record : records) {
    if (attempts_of(op_of(record)) > 1000.0 * (1.0 + 1e-9)) return simsys::seconds(record.t);
  }
  return -1.0;
}
}  // namespace

TEST_CASE("a retry_after of 150 ms at 100 ms steps waits 2 steps: the retries start at t = 0.2 s",
          "[theory]") {
  const auto records = run_sink_with_backoff(150ms);
  REQUIRE_THAT(first_retry_at(records), WithinAbs(0.2, 1e-12));
  REQUIRE_THAT(attempts_of(op_of(records[2])), WithinRel(1500.0, 1e-9));
}

TEST_CASE("a retry_after of 250 ms at 100 ms steps waits 3 steps: the retries start at t = 0.3 s",
          "[theory]") {
  const auto records = run_sink_with_backoff(250ms);
  REQUIRE_THAT(first_retry_at(records), WithinAbs(0.3, 1e-12));
  REQUIRE_THAT(attempts_of(op_of(records[3])), WithinRel(1500.0, 1e-9));
}

TEST_CASE("a retry_after of exactly one step waits that step: the retries start at t = 0.1 s",
          "[theory]") {
  REQUIRE_THAT(first_retry_at(run_sink_with_backoff(100ms)), WithinAbs(0.1, 1e-12));
}

// ---- A node_down: the retry storm
// ---------------------------------------------------------------------
//
// c (300/s, retries 2 after 100 ms) -> s (1k/s, 1 ms: c = 1 server) over a zero-latency link, with
// a room of ONE request (queue_limit 1) so that a down node fills it in a step and loses every
// later request. s is down for 10 s from t = 10 s (steps 100 to 199, 100 ms steps). The queue's
// own numbers, read off the same probe without retries:
//   step 100:  300/s x 0.1 s = 30 requests arrive, the room keeps ONE, 29 are lost: delivered 1/30
//   steps 101-199: the room is full, capacity 0: delivered 0
//   step 200:  the node is back: it serves up to 100 a step, the room's 1 included
// and a request is answered when it is not dropped (one still waiting in the room is answered
// later), so d = 1/30 at step 100 and 0 until the node returns. With the backoff of one step the
// attempts of step t are 300 + failed(t-1) of attempt 0 + failed(t-1) of attempt 1:
//   step  99 and before: 300                      (nothing fails)
//   step 100: 300                                 (no history)
//   step 101: 300 + 300 x (1 - 1/30)  = 300 + 290 = 590
//   step 102: 300 + 300 x 1 + 290 x 1 = 890       (d = 0 at step 101: attempt 0 failed whole)
//   step 103 to 199: 300 + 300 + 300  = 900       = 3 x the curve: every attempt fails
// The storm is capped at 3 x the curve by retries = 2, not by anything the server does. After the
// fault clears the retries already in flight arrive at step 200 (900/s: they are the failures of
// step 199, 900 x 0.1 = 90 requests, which the node now serves, with the one in the room: it can
// serve 100 a step), so nothing fails at step 200 and nothing is retried at step 201:
//   step 200: 900  (the last wave of retries, all answered)      step 201 on: 300 (the curve)
// The plan allows 2 x retry_after (two steps) for this; the model gives ONE step after the clear
// because the retries that fired at step 200 succeed, so no attempt-2 wave follows them. The
// storm (900/s) stays under the node's 1000/s, so the recovery is the retries' alone, not a
// metastable failure.

namespace {
constexpr std::string_view storm_probe_head =
    "version: 1\nname: storm\nduration: 30s\nstep: 100ms\n"
    "nodes:\n"
    "  c: {type: client, rate: {base: 300/s}, ops: {op: 1.0}";
constexpr std::string_view storm_probe_tail =
    "}\n"
    "  s: {type: service, capacity: 1k/s, latency: 1ms, advanced: {queue_limit: 1}}\n"
    "links:\n  - {from: c, to: s, dir: inout, latency: 0s}\n"
    "faults:\n  - {at: 10s, kind: node_down, node: s, for: 10s}\n";

Scenario storm_probe(std::string_view client_extra = ", retries: 2, retry_after: 100ms") {
  return load_yaml(std::string{storm_probe_head} + std::string{client_extra} +
                   std::string{storm_probe_tail});
}
}  // namespace

TEST_CASE("a node_down with retries 2 lifts attempts to 3 times the curve, 900 of 300 a second",
          "[theory]") {
  const auto g = graph_of(storm_probe());
  const auto records = records_of(g, 300);
  for (std::size_t i = 0; i < records.size(); ++i) {
    CAPTURE(i);
    REQUIRE_THAT(op_of(records[i]).offered.per_second, WithinRel(300.0, 1e-12));  // the curve
  }
  for (std::size_t i = 0; i < 100; ++i) {  // before the fault: nothing fails, nothing is retried
    CAPTURE(i);
    REQUIRE_THAT(attempts_of(op_of(records[i])), WithinRel(300.0, 1e-9));
  }
  REQUIRE_THAT(attempts_of(op_of(records[100])), WithinRel(300.0, 1e-9));  // first step down
  REQUIRE_THAT(attempts_of(op_of(records[101])), WithinRel(590.0, 1e-9));  // 300 + 290
  REQUIRE_THAT(attempts_of(op_of(records[102])), WithinRel(890.0, 1e-9));  // 300 + 300 + 290
  for (std::size_t i = 103; i < 200; ++i) {
    CAPTURE(i);
    REQUIRE_THAT(attempts_of(op_of(records[i])), WithinRel(900.0, 1e-9));  // 3 x 300
    REQUIRE_THAT(op_of(records[i]).answered.per_second, WithinAbs(0.0, 1e-9));
  }
}

TEST_CASE("the attempts fall back to the curve one step after the fault clears: 900 then 300",
          "[theory]") {
  const auto g = graph_of(storm_probe());
  const auto records = records_of(g, 300);
  REQUIRE_THAT(attempts_of(op_of(records[199])), WithinRel(900.0, 1e-9));  // last step down
  REQUIRE_THAT(attempts_of(op_of(records[200])), WithinRel(900.0, 1e-9));  // the retries in flight
  // The node is back and serves the wave of 900/s (90 requests and the one in the room, of the
  // 100 a step it can): nothing is lost at step 200, so nothing is retried from step 201 on.
  REQUIRE_THAT(op_of(records[200]).answered.per_second, WithinRel(900.0, 1e-9));
  for (std::size_t i = 201; i < records.size(); ++i) {
    CAPTURE(i);
    REQUIRE_THAT(attempts_of(op_of(records[i])), WithinRel(300.0, 1e-9));  // the curve again
    REQUIRE_THAT(op_of(records[i]).answered.per_second, WithinRel(300.0, 1e-9));
  }
}

// The run's report: the extra attempts the fault cost, summed over the steps,
//   (290 + 590 + 97 x 600 + 600) per second x 0.1 s = 59680 x 0.1 = 5968 attempts
// (steps 101, 102, 103-199 and 200 above), so over the 30 s run attempts_per_s averages
// (300 x 30 + 5968) / 30 = 498.9333 and offered_per_s 300. Task 8's fault block prints the 5968.
TEST_CASE(
    "the storm costs 5968 extra attempts, which the JSON's attempts_per_s averages over the run",
    "[theory]") {
  const auto json = run_json(storm_probe());
  CAPTURE(json);
  REQUIRE_THAT(json_number(json, "offered_per_s"), WithinRel(300.0, 1e-9));
  REQUIRE_THAT(json_number(json, "attempts_per_s"),
               WithinRel((300.0 * 30.0 + 5968.0) / 30.0, 1e-9));
}

TEST_CASE("the same node_down without retries leaves attempts equal to offered", "[theory]") {
  const auto g = graph_of(storm_probe(""));
  const auto records = records_of(g, 300);
  for (std::size_t i = 0; i < records.size(); ++i) {
    CAPTURE(i);
    REQUIRE_THAT(attempts_of(op_of(records[i])), WithinRel(300.0, 1e-12));
  }
}

// ---- The storm in the fault block: attempts against requests lost for good
// ---------------------------------
//
// Ch. 1's retry storm, as the incident report must tell it. The node's drops are ATTEMPTS: every
// attempt the down node turned away, 29 at step 100 (30 arrive, the room keeps 1), then 59, 89 and
// 97 x 90 as the attempts climb to 900/s:
//   29 + 59 + 89 + 97 x 90 = 8907 attempts lost       (the twin loses none)
// which is the servers' side of the story. The users' side is the requests whose LAST attempt
// failed, `ResponseSample::lost`: attempt 2 of a request, sent two steps after its first, is its
// last, so the requests lost for good are attempt 2's rate whenever the node drops everything:
//   step 102: 290/s (the failures of attempt 1 at step 101, themselves 290/s)
//   steps 103-199: 300/s                        step 200: the node is back, attempt 2 answered
//   (290 + 97 x 300) x 0.1 s = 29 + 2910 = 2939 requests lost for good
// against 2999 without retries (29 + 99 x 30, every attempt being the last): the retries saved 60
// requests, those first sent in the outage's last two steps (2 x 30), whose later attempts reached
// s after it came back, and cost 5968 extra attempts (above). The block prints both figures, the
// attempts first because the backlog, the room and the rates beside them are in attempts too:
//   during: offered 890.8/s (the mean demand at s: (300 + 590 + 890 + 97 x 900) / 100 steps),
//           served 0/s, lost 8.907k attempts (2.939k requests lost for good),
//           backlog 1 (room 1 dropped the rest)
//   after:  the room's 1 is served in the step at 20 s (91 of the 100 a step s can serve), so the
//           backlog drained in 0.1s; the retries added 5968 attempts from `at` to the recovery
//   recovery: at 20.1 s every rho is the twin's again (s served 900/s at 20.0: rho 0.91, not 0.3)
// The probe without retries prints the block it always did: requests, no parenthesis, no retries,
// and the same drain and recovery (its room's 1 is served at 20.0 s, with 30 of the curve's: rho
// 0.31, more than 1 % above the twin's 0.3).

namespace {
// What `simsys run` prints for the scenario (session::run_summary: the run, its fault-free twin,
// and the fault blocks set from both), as the table and as the JSON.
struct Reports {
  std::string table;
  std::string json;
};

Reports reports_of(const Scenario& s) {
  const auto g = graph_of(s);
  auto sim = simulation_of(g);
  const auto summary = simsys::session::run_summary(g, sim);
  return {simsys::report::render_table(summary, {}, /*color=*/false),
          simsys::report::render_json(summary, {})};
}

constexpr std::string_view storm_recovery =
    "  recovery: back within 1% (every op's p99, every node's rho) at 00:00:20.1 (0.1s after the "
    "fault cleared)\n";
}  // namespace

TEST_CASE(
    "each step's lost counts the requests whose last attempt was lost: 290, then 300 a second",
    "[theory]") {
  const auto g = graph_of(storm_probe());
  const auto records = records_of(g, 300);
  double lost_for_good = 0.0;
  for (std::size_t i = 0; i < records.size(); ++i) {
    CAPTURE(i);
    const double lost = op_of(records[i]).lost.per_second;
    if (i < 102 || i >= 200) REQUIRE_THAT(lost, WithinAbs(0.0, 1e-9));  // no last attempt failed
    if (i == 102) REQUIRE_THAT(lost, WithinRel(290.0, 1e-9));
    if (i > 102 && i < 200) REQUIRE_THAT(lost, WithinRel(300.0, 1e-9));
    lost_for_good += lost * dt_s;
  }
  REQUIRE_THAT(lost_for_good, WithinRel(2939.0, 1e-9));
}

// Without retries every attempt is a request's last: lost is offered x (1 - delivered), 290/s at
// step 100 (d = 1/30) and 300/s from then on while the node is down, 2999 requests in all.
TEST_CASE("without retries each step's lost is offered times the share not delivered: 2999 in all",
          "[theory]") {
  const auto g = graph_of(storm_probe(""));
  const auto records = records_of(g, 300);
  REQUIRE_THAT(op_of(records[100]).lost.per_second, WithinRel(290.0, 1e-9));
  double lost_for_good = 0.0;
  for (const auto& record : records) lost_for_good += op_of(record).lost.per_second * dt_s;
  REQUIRE_THAT(lost_for_good, WithinRel(2999.0, 1e-9));
}

TEST_CASE(
    "the storm's block counts 8.907k attempts lost, 2.939k requests lost for good, and the "
    "5.968k attempts the retries added",
    "[theory]") {
  const auto out = reports_of(storm_probe());
  CAPTURE(out.table);
  REQUIRE(out.table.ends_with(
      "fault 00:00:10 node_down s for 10s\n"
      "  during:   offered 890.8/s, served 0/s, lost 8.907k attempts (2.939k requests lost for "
      "good), backlog 1 (room 1 dropped the rest)\n"
      "  after:    backlog drained in 0.1s, retries added 5.968k attempts\n" +
      std::string{storm_recovery}));
}

TEST_CASE(
    "the storm's JSON has lost in attempts, lost_requests 2939 and retries_added_attempts 5968",
    "[theory]") {
  const auto json = reports_of(storm_probe()).json;
  CAPTURE(json);
  REQUIRE_THAT(json_number(json, "lost"), WithinRel(8907.0, 1e-9));
  REQUIRE(json.contains("\"lost_unit\": \"attempts\", \"lost_requests\": "));
  REQUIRE_THAT(json_number(json, "lost_requests"), WithinRel(2939.0, 1e-9));
  REQUIRE_THAT(json_number(json, "retries_added_attempts"), WithinRel(5968.0, 1e-9));
  REQUIRE(json.contains("\"pool_republished_jobs\": null"));
}

// The identity: no retries, no attempts. The block and the JSON are what they were before the
// fault block knew of retries: the node's drops in requests (29 + 99 x 30 = 2999), no parenthesis,
// nothing added after, and the new keys null.
TEST_CASE("the same node_down without retries prints requests, no lost for good and nothing added",
          "[theory]") {
  const auto out = reports_of(storm_probe(""));
  CAPTURE(out.table, out.json);
  REQUIRE(out.table.ends_with(
      "fault 00:00:10 node_down s for 10s\n"
      "  during:   offered 300/s, served 0/s, lost 2.999k requests, backlog 1 (room 1 dropped the "
      "rest)\n"
      "  after:    backlog drained in 0.1s\n" +
      std::string{storm_recovery}));
  REQUIRE(out.json.contains("\"lost_unit\": \"requests\", \"lost_requests\": null"));
  REQUIRE(out.json.contains("\"retries_added_attempts\": null, \"pool_republished_jobs\": null"));
}

// ---- A timeout is a failure like a loss
// -------------------------------------------------------------------
//
// c (400/s, timeout 200 ms, retries 2) -> s (1k/s at 100 ms: c = 100 servers, rho 0.447 with the
// attempts, wait ~ 0) over a zero-latency link. The service never loses a request, but its
// response time is exponential with mean 100 ms and variance 0.01 s^2 (SCV 1: the M/M/c service
// time, and the wait is ~1e-14 s), the 100 ms / 0.01 s^2 path of test_timeout.cpp's worked
// example: 89.4137 % of the responses fit in 200 ms and
//   q = 1 - 0.894137 = 0.105863
// time out, the same share at every attempt (the path's latency does not depend on the extra
// load; it is a leaf with c = 100 servers at rho 0.45). A timed-out request is retried like a lost
// one, so the attempts per request are 1 + q + q^2 = 1.117071, answered 1 - q^3 = 0.998814 (each
// attempt answers with probability 1 - q, and a request needs one) and the requests whose last
// attempt timed out are timed out once: q^3 = 0.0011865 of the offered load.

namespace {
Scenario timing_out_with_retries() {
  Scenario s{.name = "timeout-retries", .version = 1, .duration = 2s, .step = 100ms};
  Workload workload{.curve = {.base = Rate{400}}, .op_shares = {{"op", 1.0}}, .timeout = 200ms};
  workload.retries = static_cast<decltype(workload.retries)>(2);
  workload.retry_after = 100ms;
  s.nodes = {
      NodeSpec{.id = "c", .type = NodeType::client, .workload = std::move(workload)},
      NodeSpec{.id = "s", .type = NodeType::service, .capacity = Rate{1000}, .latency = 100ms}};
  s.links = {LinkSpec{.from = "c", .to = "s", .latency = 0s}};
  return s;
}

// The share of responses slower than the 200 ms timeout on the 100 ms, 0.01 s^2 path: one minus
// what queueing::lognormal_below says is answered (the fit the reports and the engine share).
double timed_out_share() {
  return 1.0 - simsys::queueing::lognormal_below(0.1, 0.01, 0.2).fraction;
}
}  // namespace

TEST_CASE("a timed out request retries like a lost one: attempts 1, 1 + q, then 1 + q + q^2",
          "[theory]") {
  const double q = timed_out_share();
  REQUIRE_THAT(
      q, WithinAbs(0.105863, 5e-7));  // the hand value, so the closed forms below are not circular
  const auto g = graph_of(timing_out_with_retries());
  const auto records = records_of(g, 20);
  REQUIRE_THAT(attempts_of(op_of(records[0])), WithinRel(400.0, 1e-6));
  REQUIRE_THAT(attempts_of(op_of(records[1])), WithinRel(400.0 * (1.0 + q), 1e-6));
  for (std::size_t i = 2; i < records.size(); ++i) {
    CAPTURE(i);
    REQUIRE_THAT(attempts_of(op_of(records[i])), WithinRel(400.0 * (1.0 + q + q * q), 1e-6));
    REQUIRE_THAT(op_of(records[i]).offered.per_second, WithinRel(400.0, 1e-12));
  }
}

// answered + timed out + lost = offered at the fixed point: the whole 400/s is accounted for,
// 1 - q^3 answered, q^3 timed out (its last attempt timed out too), nothing lost (the service
// drops nothing).
TEST_CASE(
    "at the fixed point the requests whose last attempt timed out are timed out once: q cubed",
    "[theory]") {
  const double q = timed_out_share();
  const auto g = graph_of(timing_out_with_retries());
  const auto records = records_of(g, 20);
  for (std::size_t i = 2; i < records.size(); ++i) {
    CAPTURE(i);
    const auto& op = op_of(records[i]);
    REQUIRE_THAT(op.answered.per_second, WithinRel(400.0 * (1.0 - q * q * q), 1e-6));
    REQUIRE_THAT(op.timed_out.per_second, WithinRel(400.0 * q * q * q, 1e-6));
    REQUIRE_THAT(lost_of(op), WithinAbs(0.0, 1e-6));  // offered - answered - timed out
  }
}

// ---- The timed-out share a percentile reads counts every attempt
// ----------------------------------------
//
// The same probe observed after a 1 s warmup (the ramp of steps 0 and 1 left out), so every
// observed step is the fixed point above. Of the attempts the service delivered, 1 + q + q^2 per
// request, the share q = 10.6 % timed out, as without retries: the path does not care which
// attempt a request is on. That is the share a percentile asks about (a p95 at more than 5 %, a
// p99 at more than 1 %: report.cpp's percentile_seen_by_client): every delivered attempt is a
// response the client waited for, and more than 1 − q of them came back after the 200 ms timeout,
// so the client never saw its p95 or p99 answered. Counting only the requests given up on for good
// (q^3 = 0.12 % of the offered load) would read p99 = 200 ms and pass a p99 SLO of 250 ms on
// responses the client never got: the retries hide the timeouts from the verdict, not from the
// user. So the engine keeps both: `timed_out` (final, q^3) for the answered + timed out + lost =
// offered balance, and `timed_out_attempts` (every attempt, q × attempts) for the percentiles.

namespace {
Scenario timing_out_with_retries_after_warmup() {
  auto s = timing_out_with_retries();
  s.duration = 10s;
  s.warmup = 1s;
  s.slos = {simsys::model::SloSpec{.metric = simsys::model::SloMetric::p99_latency,
                                   .node = "c",
                                   .op = "op",
                                   .max = simsys::SimTime{250ms}}};
  return s;
}

struct ObservedRun {
  simsys::report::Summary summary;
  std::vector<simsys::report::SloResult> slos;
};

ObservedRun observe(const Graph& g) {
  ObservedRun run{.summary = simsys::report::Summary{g, g.scenario().warmup}, .slos = {}};
  auto sim = simulation_of(g);
  sim.run({.duration = g.scenario().duration, .step = g.scenario().step},
          [&](const auto& r) { run.summary.observe(r); });
  run.slos = simsys::report::evaluate_slos(run.summary, g.scenario().slos);
  return run;
}

// The words of the table line that starts with `start` (the op table's row of one client op).
std::vector<std::string> words_of_row(const std::string& table, std::string_view start) {
  std::istringstream lines{table};
  for (std::string line; std::getline(lines, line);) {
    std::istringstream words{line};
    std::vector<std::string> row;
    for (std::string word; words >> word;) row.push_back(word);
    if (!row.empty() && row[0] == start) return row;
  }
  return {};
}
}  // namespace

TEST_CASE("each step counts the attempts that timed out, q of every attempt, beside the final ones",
          "[theory]") {
  const double q = timed_out_share();
  const auto g = graph_of(timing_out_with_retries());
  const auto records = records_of(g, 20);
  for (std::size_t i = 2; i < records.size(); ++i) {
    CAPTURE(i);
    const auto& op = op_of(records[i]);
    REQUIRE_THAT(op.timed_out_attempts.per_second, WithinRel(attempts_of(op) * q, 1e-6));
    REQUIRE_THAT(op.timed_out.per_second, WithinRel(400.0 * q * q * q, 1e-6));
  }
}

TEST_CASE(
    "with retries 2 a p99 SLO of 250 ms fails: 10.6 percent of the delivered attempts time out",
    "[theory]") {
  const auto g = graph_of(timing_out_with_retries_after_warmup());
  const auto run = observe(g);
  REQUIRE(run.slos.size() == 1);
  CAPTURE(run.slos[0].note);
  REQUIRE(run.slos[0].status == simsys::report::SloStatus::fail);
  REQUIRE(run.slos[0].observed == 0.2);  // the timeout: the p99 was never seen answered
  REQUIRE(run.slos[0].note == "10.6% of delivered requests timed out (more than 1%)");
}

TEST_CASE("with retries 2 the op table prints the p95 and the p99 as beyond the 200 ms timeout",
          "[theory]") {
  const auto g = graph_of(timing_out_with_retries_after_warmup());
  const auto run = observe(g);
  const auto table = simsys::report::render_table(run.summary, run.slos, /*color=*/false);
  CAPTURE(table);
  const auto row = words_of_row(table, "c.op");
  // c.op, offered, attempts, timed out, mean, p50, p95, p99
  REQUIRE(row.size() == 8);
  REQUIRE(row[6] == ">200ms");
  REQUIRE(row[7] == ">200ms");
  REQUIRE(row[5] != ">200ms");  // the median was answered: 89.4 % of the attempts were in time
}

// The JSON keeps both counts: `timed_out_per_s` the requests given up on for good, 400 q^3, so that
// answered + timed out = offered still balances, and `timed_out_attempts_per_s` every attempt that
// timed out, 400 (q + q^2 + q^3) = the attempts 400 (1 + q + q^2) times q.
TEST_CASE("the JSON op has the final timeouts, 400 q cubed, and the timed out attempts beside them",
          "[theory]") {
  const double q = timed_out_share();
  const auto json = run_json(timing_out_with_retries_after_warmup());
  CAPTURE(json);
  REQUIRE_THAT(json_number(json, "timed_out_per_s"), WithinRel(400.0 * q * q * q, 1e-6));
  REQUIRE_THAT(json_number(json, "timed_out_attempts_per_s"),
               WithinRel(400.0 * (q + q * q + q * q * q), 1e-6));
  REQUIRE_THAT(json_number(json, "answered_per_s") + json_number(json, "timed_out_per_s"),
               WithinRel(400.0, 1e-9));
}

TEST_CASE("estimate settles a timing out client at 1 + q + q^2 attempts a request", "[theory]") {
  const double q = timed_out_share();
  const auto g = graph_of(timing_out_with_retries());
  const auto estimate = simulation_of(g).estimate();
  REQUIRE(estimate.converged);
  REQUIRE_THAT(attempts_of(op_of(estimate)), WithinRel(400.0 * (1.0 + q + q * q), 1e-6));
  REQUIRE_THAT(op_of(estimate).timed_out.per_second, WithinRel(400.0 * q * q * q, 1e-6));
}

// ---- The identity: retries with nothing to retry change nothing
// -----------------------------------------

namespace {
// The shipped tutorial/hello scenario, with `extra` spliced into its client's flow map.
Scenario hello_with(std::string_view extra) {
  std::ifstream file{std::string{SIMSYS_SCENARIOS_DIR} + "/tutorial/hello/scenario.yaml"};
  std::ostringstream text;
  text << file.rdbuf();
  std::string yaml = text.str();
  const std::string anchor = "ops: {ping: 1.0}";
  const auto at = yaml.find(anchor);
  REQUIRE(at != std::string::npos);
  yaml.insert(at + anchor.size(), extra);
  return load_yaml(yaml);
}

// A run's node series and op series, as the CSV files `--series` and `--series-ops` write.
struct Series {
  std::string nodes;
  std::string ops;
};

Series series_of(const Scenario& s) {
  const auto g = graph_of(s);
  std::ostringstream nodes_out;
  std::ostringstream ops_out;
  simsys::report::SeriesWriter nodes{nodes_out, g};
  simsys::report::OpSeriesWriter ops{ops_out, g};
  auto sim = simulation_of(g);
  sim.run({.duration = s.duration, .step = s.step}, [&](const auto& r) {
    nodes.observe(r);
    ops.observe(r);
  });
  return {nodes_out.str(), ops_out.str()};
}
}  // namespace

// hello loses nothing and its client has no timeout, so `retries: 2` has nothing to send again:
// the run is the run without it, byte for byte, the new attempts_per_s field included (equal to
// offered_per_s in both: 100/s).
TEST_CASE("hello with retries 2 gives the same run JSON as without: nothing to retry", "[theory]") {
  const auto plain = run_json(hello_with(""));
  const auto retrying = run_json(hello_with(", retries: 2"));
  CAPTURE(plain);
  REQUIRE(retrying == plain);
  REQUIRE_THAT(json_number(retrying, "attempts_per_s"), WithinRel(100.0, 1e-9));
  REQUIRE(json_number(retrying, "attempts_per_s") == json_number(retrying, "offered_per_s"));
}

TEST_CASE("hello with retries 2 writes the same node and op series as without", "[theory]") {
  const auto plain = series_of(hello_with(""));
  const auto retrying = series_of(hello_with(", retries: 2, retry_after: 250ms"));
  REQUIRE(retrying.nodes == plain.nodes);
  REQUIRE(retrying.ops == plain.ops);
  // Not vacuous: the op series has the new column. C++20 std::string::starts_with: a prefix test
  // in one call, replacing `compare(0, prefix.size(), prefix) == 0` and its length bookkeeping.
  REQUIRE(retrying.ops.starts_with("t_s,client,op,offered_per_s,attempts_per_s,"));
}
