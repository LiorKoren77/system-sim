// Scripted faults (book ch. 1 and ch. 12: what the system does while a part of it is down, and how
// long it takes to come back). Model under test: a fault changes, for the steps it covers, the
// numbers a node's queue is built from, and nothing else:
//   node_down        capacity x (R - r) / R  (r of its R replicas down; all of them: x 0)
//   capacity         capacity x factor       (the service time per request is unchanged)
//   latency_spike    service time + extra    (so the capacity of its servers falls to c / (s +
//   extra)) traffic_spike    the client's rate x factor, the code path of a `spikes` entry on its
//   curve
// Faults on one node compose: factors multiply, extra latencies add. A fault is active on
// [at, at + for): the step that STARTS inside the interval sees it. Outside every fault a node
// gets the identity condition, so a run without faults is the run of phase 2, bit for bit. A
// request in the room of a node at capacity 0 waits until the node is back, then for the backlog
// ahead of it at the node's own capacity (Task 6's review); a sql_db's primary and read replicas go
// down apart, and a kv_cluster's down machines cost the quorum of the keys they held.
//
// Ignored on purpose: partitions (refused by the loader), a real node's restart time (a node comes
// back at full capacity the instant the fault ends), and what the clients do about failures
// (retries are Task 10): the backlog and the recovery below are the queue's own.
//
// Every number is derived by hand in the comment above its test. The probe is an M/M/1 queue:
// 1k/s of capacity and a 1 ms service time make c = 1 server, so at utilization rho
//   mean sojourn W = s / (1 - rho)    (the M/M/1 mean response time, queueing theory)
//   drain         = min(demand + backlog / dt, capacity)   (the fluid queue of spec §3.3)
// and the client's mean response is W: its link has no latency.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>  // getpid

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.behaviours;
import simsys.caching;
import simsys.config;
import simsys.core;
import simsys.engine;
import simsys.flow;
import simsys.graph;
import simsys.model;
import simsys.queueing;
import simsys.report;
import simsys.test_fixtures;

using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using simsys::engine::Simulation;
using simsys::engine::StepRecord;
using simsys::graph::Graph;
using simsys::model::Scenario;
using simsys::test_support::records_of;
using simsys::test_support::simulation_of;

namespace {

// ---- Loading the probe from text ---------------------------------------------------------------

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
                      (std::to_string(::getpid()) + "_simsys_faults.yaml")};
  std::ofstream(file.path) << yaml;
  // C++23 std::expected<T, E>: the loader returns a Scenario or a ConfigError, and the caller
  // must look at which. It replaces exceptions and error codes; the error carries the file,
  // line and message, so a test that cannot load its scenario says why.
  auto loaded = simsys::config::load(file.path.string());
  if (!loaded)
    FAIL(loaded.error().file << ":" << loaded.error().line << ": " << loaded.error().message);
  return std::move(*loaded);
}

// The M/M/1 probe: client `c` (one op, `op`) -> service `s` (capacity 1k/s, latency 1 ms, so
// c = 1 server; queue_limit 100, the room) over a zero-latency link. What a test varies:
//   rate_per_s  the client's rate (400/s: rho 0.4 at one replica)
//   replicas    the service's replicas R (1k/s EACH: `capacity` is per replica, and the clients'
//               rate is split evenly over them, so R replicas at 1600/s each see 400/s)
//   faults      the YAML text of a `faults:` block (empty: the key is left out)
//   timing      duration and step; the default is 30 s at 100 ms, 300 steps, the step of
//               records_of (tests/support/fixtures.cppm has the fixture; its mm1 builds the
//               probe in code, which has no room for `queue_limit` or `faults`, so the probe is
//               text here)
//   client      more keys of the client's flow map (", timeout: 50ms"); empty: none
//   slos        the YAML text of an `slo:` block; empty: none
// C++20 designated initializers: `{.faults = "..."}` names the members a call sets, in
// declaration order, the rest keep their defaults. Replaces a call with five positional
// arguments (which compiles when two ints are swapped) or a builder class: the call site
// reads like the YAML it stands for.
struct Probe {
  int rate_per_s = 400;
  int replicas = 1;
  std::string faults{};
  std::string timing = "duration: 30s\nstep: 100ms\n";
  std::string client{};
  std::string slos{};
};

Scenario probe(const Probe& p) {
  return load_yaml("version: 1\nname: faults\n" + p.timing +
                   "nodes:\n"
                   "  c: {type: client, rate: {base: " +
                   std::to_string(p.rate_per_s) + "/s}, ops: {op: 1.0}" + p.client +
                   "}\n"
                   "  s: {type: service, capacity: 1k/s, latency: 1ms, replicas: " +
                   std::to_string(p.replicas) +
                   ", advanced: {queue_limit: 100}}\n"
                   "links:\n  - {from: c, to: s, dir: inout, latency: 0s}\n" +
                   p.slos + p.faults);
}

// One `faults:` block with a single entry, the entry's text as the flow map between the braces.
std::string one_fault(std::string_view entry) {
  return "faults:\n  - {" + std::string{entry} + "}\n";
}

// The ten seconds from 00:10 s on which most tests below act: steps 100 to 199 at 100 ms.
// (`at: 10s`, not `00:10`: a clock time reads HH:MM from midnight, so 00:10 would be ten MINUTES.)
constexpr std::string_view ten_seconds_from_ten = "at: 10s, kind: node_down, node: s, for: 10s";

// ---- Running it and reading the numbers --------------------------------------------------------

Graph graph_of(Scenario s) {
  auto g = Graph::build(std::move(s));
  if (!g) FAIL(g.error().message);
  return std::move(*g);
}

// The probe's two nodes, in declaration order: the client, then the service.
constexpr std::size_t service_node = 1;

// The record of the step that starts at `seconds` (100 ms steps from t = 0).
const StepRecord& step_at(const std::vector<StepRecord>& records, double seconds) {
  return records.at(static_cast<std::size_t>(std::lround(seconds * 10.0)));
}

const simsys::engine::NodeSample& service_of(const StepRecord& r) {
  return r.nodes[service_node];
}
double mean_s_of(const StepRecord& r) {
  return r.responses.at(0).response.mean_s;
}

// The M/M/1 mean sojourn time s / (1 - rho): the closed form every latency pin below is
// derived from (stated here once, so each pin's comment can say the numbers going into it).
double mm1_mean_s(double service_time_s, double rho) {
  return service_time_s / (1.0 - rho);
}

// The whole scenario run for its duration and rendered as JSON, the report a run prints.
std::string run_json(const Scenario& s) {
  const auto g = graph_of(s);
  simsys::report::Summary summary{g, s.warmup};
  auto sim = simulation_of(g);
  sim.run({.duration = s.duration, .step = s.step}, [&](const auto& r) { summary.observe(r); });
  summary.set_faults(
      sim.fault_records());  // as session::run_summary does: the report's fault block
  return simsys::report::render_json(summary, {});
}

// What a whole run reports: every op's summary and every SLO's verdict.
struct RunReport {
  std::vector<simsys::report::OpSummary> ops;
  std::vector<simsys::report::SloResult> slos;
};

RunReport run_report(const Scenario& s) {
  const auto g = graph_of(s);
  simsys::report::Summary summary{g, s.warmup};
  auto sim = simulation_of(g);
  sim.run({.duration = s.duration, .step = s.step}, [&](const auto& r) { summary.observe(r); });
  return {summary.ops(), simsys::report::evaluate_slos(summary, s.slos)};
}
}  // namespace

// ---- The probe is what it says ------------------------------------------------------------------

// Precondition of every test below: 400/s into a 1k/s, 1 ms M/M/1 queue is rho = 0.4 and
// W = 1 ms / (1 - 0.4) = 1.6667 ms, with an empty queue. Without a fault the engine says so.
TEST_CASE("the probe without faults is an M/M/1 queue at rho 0.4 answering in 1.667 ms",
          "[theory]") {
  const auto records = records_of(graph_of(probe({})), 300);
  const auto& r = step_at(records, 15.0);
  REQUIRE_THAT(service_of(r).rho, WithinRel(0.4, 1e-12));
  REQUIRE_THAT(service_of(r).drain_per_s, WithinRel(400.0, 1e-12));
  REQUIRE(service_of(r).backlog == 0.0);
  REQUIRE_THAT(mean_s_of(r), WithinRel(mm1_mean_s(0.001, 0.4), 1e-9));  // 1 / 0.6 ms = 1.6667 ms
}

// ---- node_down: the queue fills, drops, and drains ----------------------------------------------

// Capacity 0 for 10 s from 10 s: nothing is served (drain 0/s), so every request that arrives
// waits. 400/s x 0.1 s = 40 requests arrive per step and the room is 100:
//   backlog after the step at 10.0 s: 40;  at 10.1 s: 80;  at 10.2 s: 120 > 100, so it is 100
//   and the 20 over spill; from then on the room is full and the whole step's 40 are dropped.
// The room fills in 100 / 400 = 0.25 s, inside the third step.
TEST_CASE("during a ten second node_down the service drains nothing and its backlog is the room",
          "[theory]") {
  const auto records =
      records_of(graph_of(probe({.faults = one_fault(ten_seconds_from_ten)})), 300);
  for (double t = 10.0; t < 19.95; t += 0.1) {
    CAPTURE(t);
    REQUIRE_THAT(service_of(step_at(records, t)).drain_per_s, WithinAbs(0.0, 1e-9));
  }
  REQUIRE_THAT(service_of(step_at(records, 10.0)).backlog, WithinRel(40.0, 1e-9));
  REQUIRE_THAT(service_of(step_at(records, 10.1)).backlog, WithinRel(80.0, 1e-9));
  for (double t = 10.2; t < 19.95; t += 0.1) {
    CAPTURE(t);
    REQUIRE_THAT(service_of(step_at(records, t)).backlog, WithinRel(100.0, 1e-9));  // the room
  }
  // the step before the fault is untouched: rho 0.4, empty queue
  REQUIRE_THAT(service_of(step_at(records, 9.9)).drain_per_s, WithinRel(400.0, 1e-12));
  REQUIRE(service_of(step_at(records, 9.9)).backlog == 0.0);
}

// Over the 10 s, 400/s x 10 s = 4000 requests arrive and the room keeps the first 100, so
//   dropped = 4000 - 100 = 3900  requests (the plan's 400 x 10 - 100)
// the step by step shape being 0, 0, 200/s (20 of 40 spill), then 400/s (all 40) for 97 steps:
// 0 + 0 + 20 + 97 x 40 = 3900. Nothing is dropped before or after the fault.
TEST_CASE("a ten second node_down at 400 per second drops 3900 requests: the room keeps 100",
          "[theory]") {
  const auto records =
      records_of(graph_of(probe({.faults = one_fault(ten_seconds_from_ten)})), 300);
  double dropped_in_fault = 0;
  double dropped_elsewhere = 0;
  for (std::size_t i = 0; i < records.size(); ++i) {
    const double dropped = service_of(records[i]).dropped_per_s * 0.1;  // per second x 100 ms
    (i >= 100 && i < 200 ? dropped_in_fault : dropped_elsewhere) += dropped;
  }
  REQUIRE_THAT(dropped_in_fault, WithinRel(3900.0, 1e-9));
  REQUIRE_THAT(dropped_elsewhere, WithinAbs(0.0, 1e-9));
  REQUIRE_THAT(service_of(step_at(records, 10.1)).dropped_per_s, WithinAbs(0.0, 1e-9));
  REQUIRE_THAT(service_of(step_at(records, 10.2)).dropped_per_s, WithinRel(200.0, 1e-9));
  REQUIRE_THAT(service_of(step_at(records, 10.3)).dropped_per_s, WithinRel(400.0, 1e-9));
}

// Once the room is full (from 10.3 s) every request is dropped at the service, so the share of
// the client's requests that is answered is 1 - 400/400 = 0: answered 0/s while the client keeps
// offering 400/s. Before the fault all 400/s are answered. (The first 0.25 s, while the room is
// still filling, are not pinned: those requests are queued, neither answered nor lost.)
TEST_CASE("during a ten second node_down the client is answered 0 per second once the room is full",
          "[theory]") {
  const auto records =
      records_of(graph_of(probe({.faults = one_fault(ten_seconds_from_ten)})), 300);
  REQUIRE_THAT(step_at(records, 9.9).responses.at(0).answered.per_second, WithinRel(400.0, 1e-9));
  for (double t = 10.3; t < 19.95; t += 0.1) {
    CAPTURE(t);
    const auto& response = step_at(records, t).responses.at(0);
    REQUIRE_THAT(response.offered.per_second, WithinRel(400.0, 1e-12));
    REQUIRE_THAT(response.answered.per_second, WithinAbs(0.0, 1e-9));
  }
}

// After the fault the capacity is back at 1000/s against 400/s of demand, so the backlog of 100
// drains at 1000 - 400 = 600/s: 100 / 600 = 0.1667 s, two steps of 100 ms.
//   step at 20.0 s: drain = min(400 + 100 / 0.1, 1000) = 1000/s; backlog 100 + (400 - 1000) 0.1 =
//   40 step at 20.1 s: drain = min(400 +  40 / 0.1, 1000) =  800/s; backlog  40 + (400 -  800) 0.1
//   = 0 step at 20.2 s: drain 400/s, nothing left. (The plan's "60 then 0" is the 60 the first step
//   removes: 100 - 60 = 40 remain, then the last 40.) Nothing is dropped.
TEST_CASE("after a node_down the backlog of 100 drains in two steps: 40 left, then 0", "[theory]") {
  const auto records =
      records_of(graph_of(probe({.faults = one_fault(ten_seconds_from_ten)})), 300);
  REQUIRE_THAT(service_of(step_at(records, 20.0)).drain_per_s, WithinRel(1000.0, 1e-9));
  REQUIRE_THAT(service_of(step_at(records, 20.0)).backlog, WithinRel(40.0, 1e-9));
  REQUIRE_THAT(service_of(step_at(records, 20.1)).drain_per_s, WithinRel(800.0, 1e-9));
  REQUIRE_THAT(service_of(step_at(records, 20.1)).backlog, WithinAbs(0.0, 1e-9));
  REQUIRE_THAT(service_of(step_at(records, 20.2)).drain_per_s, WithinRel(400.0, 1e-9));
  for (double t = 20.0; t < 29.95; t += 0.1) {
    CAPTURE(t);
    REQUIRE_THAT(service_of(step_at(records, t)).dropped_per_s, WithinAbs(0.0, 1e-9));
  }
}

// The recovery pin: the step before the fault has the M/M/1 mean 1 / (1 - 0.4) = 1.6667 ms. The
// step that clears the fault still carries a backlog (40 requests waiting at 1000/s is 40 ms,
// far above 1.67 ms), and the third step after it, at 20.2 s, is within 1 % of the pre-fault mean.
TEST_CASE("the op's mean latency is back within 1 percent by the third step after the fault",
          "[theory]") {
  const auto records =
      records_of(graph_of(probe({.faults = one_fault(ten_seconds_from_ten)})), 300);
  const double before = mean_s_of(step_at(records, 9.9));
  REQUIRE_THAT(before, WithinRel(mm1_mean_s(0.001, 0.4), 1e-9));  // 1.6667 ms
  REQUIRE(mean_s_of(step_at(records, 20.0)) > 1.01 * before);     // not instantaneous
  REQUIRE_THAT(mean_s_of(step_at(records, 20.2)), WithinRel(before, 0.01));
  REQUIRE_THAT(mean_s_of(step_at(records, 25.0)), WithinRel(before, 1e-9));
}

// ---- A down node is waited for: the wait of a request in its room ------------------------------
//
// Phase 3 Task 6's review (finding 1): a node at capacity 0 still admits requests to its room
// (40, 80, then 100 above), and those requests are answered once it is back, so what one of them
// experiences is the rest of the outage and then the backlog ahead of it, drained at the node's
// nominal capacity:
//   wait = back_in + backlog / 1000/s       back_in = (at + for) − t = 20 s − t
// (the fluid rule backlog / capacity read an infinite wait at capacity 0, which made the op
// `unavailable` for a whole run over a 10 s outage). The sojourn adds the 1 ms service:
//   step 10.0  10 s + 40 / 1000 + 1 ms  = 10.041 s
//   step 10.1  9.9 s + 80 / 1000 + 1 ms =  9.981 s
//   step 15.0  5 s + 100 / 1000 + 1 ms  =  5.101 s
//   step 19.9  0.1 s + 100 / 1000 + 1 ms = 0.201 s
// (the Erlang term at the 0.99 clamp, 99 ms, is below each). The client's link has no latency, so
// the client's mean response is the node's sojourn.
TEST_CASE(
    "a request in the room of a down node waits until the node is back and the backlog ahead of it "
    "drains",
    "[theory]") {
  const auto records =
      records_of(graph_of(probe({.faults = one_fault(ten_seconds_from_ten)})), 300);
  const std::pair<double, double> expected[] = {{10.0, 10.0 + 0.040 + 0.001},
                                                {10.1, 9.9 + 0.080 + 0.001},
                                                {15.0, 5.0 + 0.100 + 0.001},
                                                {19.9, 0.1 + 0.100 + 0.001}};
  for (const auto& [t, sojourn_s] : expected) {
    CAPTURE(t);
    REQUIRE(std::isfinite(service_of(step_at(records, t)).latency.mean_s));
    REQUIRE_THAT(service_of(step_at(records, t)).latency.mean_s, WithinRel(sojourn_s, 1e-9));
    REQUIRE_THAT(mean_s_of(step_at(records, t)), WithinRel(sojourn_s, 1e-9));
  }
}

// The same outage seen by a client that gives up after 50 ms (`timeout: 50ms`). What the path
// delivered over the run (answered or timed out: a request is lost only where it is dropped):
//   outside the outage  400/s x 20 s                            = 8000
//   in the outage       the room's 40, 40 and 20 of its first three steps = 100 (the rest dropped)
// The 100 wait about 10 s (above): every one times out. Outside the outage the response is the
// M/M/1 one, 1.667 ms with an exponential's spread, whose lognormal fit puts 3.4e-6 of it past
// 50 ms: 0.03 of the 8000 (and after the outage the 40 left drain in 41 ms, under the timeout). So
//   timed-out share = 100.03 / 8100 = 1.235 %    (100 / 8100 to 3e-4)
// more than 1 % of the delivered requests, so no request at the 99th percentile was answered: the
// p99 is the timeout itself, 50 ms (the table's `>50ms`), finite. The op is not unavailable.
TEST_CASE(
    "with a 50 ms timeout a ten second node_down times out the 100 requests in the room and caps "
    "the p99 at the timeout",
    "[theory]") {
  const auto report =
      run_report(probe({.faults = one_fault(ten_seconds_from_ten), .client = ", timeout: 50ms"}));
  REQUIRE(report.ops.size() == 1);
  const auto& op = report.ops[0];
  REQUIRE_THAT(op.timed_out_share, WithinRel(100.0 / 8100.0, 1e-3));
  REQUIRE_THAT(op.timed_out_per_s, WithinRel(100.0 / 30.0, 1e-3));  // 100 over the 30 s observed
  REQUIRE_THAT(op.answered_per_s, WithinRel(8000.0 / 30.0, 1e-3));
  REQUIRE(op.p99_s == 0.05);
  REQUIRE(std::isfinite(op.mean_s));
  REQUIRE(op.mean_s < 0.05);  // the answered requests' mean: each was answered within the timeout
}

// The two ways to state the p99 SLO agree on the outage: over the whole run, and over 10 s windows
// (the worst of [0, 10), [10, 20), [20, 30) s). The window of the outage delivered its 100 requests
// and timed every one out, so both fail at the timeout, 50 ms (a quantile no answered request
// reached). Before the fix the whole run read an infinite latency (`unavailable`).
TEST_CASE("the whole run's p99 SLO and its 10 s windows both fail a node_down at the timeout",
          "[theory]") {
  const auto report = run_report(
      probe({.faults = one_fault(ten_seconds_from_ten),
             .client = ", timeout: 50ms",
             .slos = "slo:\n"
                     "  - {metric: p99_latency, node: c, op: op, max: 1s}\n"
                     "  - {metric: p99_latency, node: c, op: op, max: 1s, over: 10s}\n"}));
  REQUIRE(report.slos.size() == 2);
  for (const auto& slo : report.slos) {
    CAPTURE(slo.slo.over.has_value());
    REQUIRE(slo.status == simsys::report::SloStatus::fail);
    REQUIRE(slo.observed.has_value());
    REQUIRE(*slo.observed == 0.05);
  }
}

// The identity's other side: a service whose nominal capacity is 0 by configuration (the loader
// accepts `capacity: 0/s`) has no capacity to come back to, so its requests still wait for ever:
// the op is `unavailable` (an infinite latency), as before the fix, which touches only a node
// derated to 0 by a fault.
TEST_CASE("a service configured with capacity 0 still makes its op unavailable", "[theory]") {
  const auto s = load_yaml(
      "version: 1\nname: zero\nduration: 10s\nstep: 100ms\n"
      "nodes:\n"
      "  c: {type: client, rate: {base: 100/s}, ops: {op: 1.0}}\n"
      "  s: {type: service, capacity: 0/s, latency: 1ms}\n"
      "links:\n  - {from: c, to: s, dir: inout, latency: 0s}\n");
  const auto report = run_report(s);
  REQUIRE(std::isinf(report.ops.at(0).p99_s));
  REQUIRE(std::isinf(report.ops.at(0).mean_s));
}

// ---- node_down of some replicas -----------------------------------------------------------------

// 4 replicas of 1k/s each (capacity is per replica) behind an even split, 1600/s offered: each
// copy sees 400/s, rho = 0.4, W = 1.6667 ms as in the probe. One replica down for 10 s leaves 3,
// capacity x (4 - 1) / 4 = x 0.75 of the group's 4k/s = 3k/s, so
//   rho = 1600 / 3000 = 0.5333  (= 0.4 / 0.75: each surviving copy sees 1600 / 3 = 533.3/s)
//   W   = s / (1 - rho) = 1 ms / 0.4667 = 2.1429 ms        (15/7 ms)
// The group still serves everything: drain 1600/s, no drops, no backlog (rho < 1).
// The plan's "closed form from mmc_wait with c = 3 of 4" does not apply: replicas without a
// balancer are independent M/M/1 copies (each its own queue, Simulation::independent_copies),
// not one M/M/3 pool, so the formula pinned is the M/M/1 one at the survivors' rho.
TEST_CASE("a node_down of 1 of 4 replicas scales capacity by 0.75: rho 0.5333, mean 2.143 ms",
          "[theory]") {
  const auto s = probe({.rate_per_s = 1600,
                        .replicas = 4,
                        .faults = one_fault("at: 10s, kind: node_down, node: s, replicas: 1, "
                                            "for: 10s")});
  const auto records = records_of(graph_of(s), 300);
  const auto& before = step_at(records, 9.9);
  REQUIRE_THAT(service_of(before).rho, WithinRel(0.4, 1e-9));
  REQUIRE_THAT(mean_s_of(before), WithinRel(mm1_mean_s(0.001, 0.4), 1e-9));
  for (const double t : {10.0, 15.0, 19.9}) {
    CAPTURE(t);
    const auto& during = step_at(records, t);
    REQUIRE_THAT(service_of(during).rho, WithinRel(1600.0 / 3000.0, 1e-9));
    REQUIRE_THAT(service_of(during).drain_per_s, WithinRel(1600.0, 1e-9));
    REQUIRE_THAT(service_of(during).dropped_per_s, WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(mean_s_of(during), WithinRel(mm1_mean_s(0.001, 1600.0 / 3000.0), 1e-9));
  }
  REQUIRE_THAT(mean_s_of(step_at(records, 15.0)), WithinRel(15.0 / 7.0 * 1e-3, 1e-9));
  const auto& after = step_at(records, 20.0);
  REQUIRE_THAT(service_of(after).rho, WithinRel(0.4, 1e-9));
  REQUIRE_THAT(mean_s_of(after), WithinRel(mm1_mean_s(0.001, 0.4), 1e-9));
}

// ---- latency_spike and capacity: two ways to double rho, which differ by the service time -------

// latency_spike extra 1 ms: the service time is 1 + 1 = 2 ms, so one server drains
// 1 / 2 ms = 500/s; rho = 400 / 500 = 0.8; W = 2 ms / (1 - 0.8) = 10 ms.
TEST_CASE("a latency_spike of 1 ms makes the service time 2 ms: capacity 500, rho 0.8, mean 10 ms",
          "[theory]") {
  const auto records = records_of(
      graph_of(probe({.faults = one_fault("at: 10s, kind: latency_spike, node: s, extra: 1ms, "
                                          "for: 10s")})),
      300);
  for (const double t : {10.0, 15.0, 19.9}) {
    CAPTURE(t);
    const auto& during = step_at(records, t);
    REQUIRE_THAT(service_of(during).rho, WithinRel(0.8, 1e-9));
    REQUIRE_THAT(service_of(during).drain_per_s, WithinRel(400.0, 1e-9));      // still all served
    REQUIRE_THAT(mean_s_of(during), WithinRel(mm1_mean_s(0.002, 0.8), 1e-9));  // 10 ms
  }
  REQUIRE_THAT(mean_s_of(step_at(records, 15.0)), WithinRel(0.010, 1e-9));
  REQUIRE_THAT(mean_s_of(step_at(records, 9.9)), WithinRel(mm1_mean_s(0.001, 0.4), 1e-9));
  REQUIRE_THAT(mean_s_of(step_at(records, 20.0)), WithinRel(mm1_mean_s(0.001, 0.4), 1e-9));
}

// capacity factor 0.5: the server drains 1000 x 0.5 = 500/s but a request still takes 1 ms of it
// (the factor is capacity lost, not a slower request): rho = 400 / 500 = 0.8 as above, but
// W = 1 ms / (1 - 0.8) = 5 ms, half of the latency spike's 10 ms.
TEST_CASE(
    "a capacity fault of 0.5 gives capacity 500, rho 0.8 and a 5 ms mean: the service time stays 1 "
    "ms",
    "[theory]") {
  const auto records = records_of(
      graph_of(probe({.faults = one_fault("at: 10s, kind: capacity, node: s, factor: 0.5, "
                                          "for: 10s")})),
      300);
  for (const double t : {10.0, 15.0, 19.9}) {
    CAPTURE(t);
    const auto& during = step_at(records, t);
    REQUIRE_THAT(service_of(during).rho, WithinRel(0.8, 1e-9));
    REQUIRE_THAT(service_of(during).drain_per_s, WithinRel(400.0, 1e-9));
    REQUIRE_THAT(mean_s_of(during), WithinRel(mm1_mean_s(0.001, 0.8), 1e-9));  // 5 ms
  }
  REQUIRE_THAT(mean_s_of(step_at(records, 20.0)), WithinRel(mm1_mean_s(0.001, 0.4), 1e-9));
}

// The same rho, the means 10 ms and 5 ms: their ratio is the ratio of the service times, 2 / 1.
TEST_CASE("the latency spike and the capacity fault of equal rho differ by the service time",
          "[theory]") {
  const auto spike = records_of(
      graph_of(probe({.faults = one_fault("at: 10s, kind: latency_spike, node: s, extra: 1ms, "
                                          "for: 10s")})),
      300);
  const auto lost = records_of(
      graph_of(probe({.faults = one_fault("at: 10s, kind: capacity, node: s, factor: 0.5, "
                                          "for: 10s")})),
      300);
  REQUIRE_THAT(service_of(step_at(spike, 15.0)).rho,
               WithinRel(service_of(step_at(lost, 15.0)).rho, 1e-9));
  REQUIRE_THAT(mean_s_of(step_at(spike, 15.0)) / mean_s_of(step_at(lost, 15.0)),
               WithinRel(2.0, 1e-9));
}

// ---- traffic_spike is a spike on the rate curve
// --------------------------------------------------

// A traffic_spike of factor 3 on the client for 10 m from 10 m is, by the plan's definition, the
// code path of `rate: {spikes: [{at, for, factor}]}`: the client's rate x 3 on [10 m, 20 m).
// At 400 x 3 = 1200/s the service (1k/s) saturates and drops, so the two JSON reports are not
// trivially equal: identical bytes mean the same numbers on every node and op. (When the fault
// block lands in the report, a `faults` key appears in the first report: compare without it
// then; the numbers are what this pins.)
TEST_CASE("a traffic_spike fault of factor 3 equals a spikes entry of factor 3 on the rate curve",
          "[theory]") {
  const std::string long_run = "duration: 30m\nstep: 1s\n";
  const auto by_fault = probe({.faults = one_fault("at: 10m, kind: traffic_spike, node: c, "
                                                   "factor: 3, for: 10m"),
                               .timing = long_run});
  // the same scenario with the spike on the curve instead: the client's line rewritten
  const auto by_curve = load_yaml(
      "version: 1\nname: faults\n" + long_run +
      "nodes:\n"
      "  c: {type: client, rate: {base: 400/s, spikes: [{at: 10m, for: 10m, factor: 3}]}, "
      "ops: {op: 1.0}}\n"
      "  s: {type: service, capacity: 1k/s, latency: 1ms, advanced: {queue_limit: 100}}\n"
      "links:\n  - {from: c, to: s, dir: inout, latency: 0s}\n");
  const auto json_by_fault = run_json(by_fault);
  REQUIRE(json_by_fault == run_json(by_curve));
  // not vacuous: the spike is in the report (a run without it is not the same bytes)
  REQUIRE(json_by_fault != run_json(probe({.timing = long_run})));
}

// ---- Overlapping faults compose
// ------------------------------------------------------------------

// capacity 0.5 twice on one node: factors multiply, 0.5 x 0.5 = 0.25 (not 0.5 or 0). Capacity
// 1000 x 0.25 = 250/s against 400/s: rho = 400 / 250 = 1.6, saturated, so the queue drains at
// its capacity, 250/s, with the room full (100) and the other 400 - 250 = 150/s dropped.
TEST_CASE("two capacity faults of 0.5 on one node multiply to 0.25: it drains 250 per second",
          "[theory]") {
  const auto twice =
      "faults:\n"
      "  - {at: 10s, kind: capacity, node: s, factor: 0.5, for: 10s}\n"
      "  - {at: 10s, kind: capacity, node: s, factor: 0.5, for: 10s}\n";
  const auto records = records_of(graph_of(probe({.faults = twice})), 300);
  for (const double t : {15.0, 19.9}) {  // the room (100 / 150 = 0.67 s) is full by then
    CAPTURE(t);
    const auto& during = step_at(records, t);
    REQUIRE_THAT(service_of(during).drain_per_s, WithinRel(250.0, 1e-9));
    REQUIRE_THAT(service_of(during).dropped_per_s, WithinRel(150.0, 1e-9));
    REQUIRE_THAT(service_of(during).rho, WithinRel(1.0, 1e-9));  // busy the whole time
    REQUIRE_THAT(service_of(during).backlog, WithinRel(100.0, 1e-9));
  }
}

// latency_spike 1 ms twice: extra latencies add, 1 + 1 + 1 = 3 ms of service time (not 2 or 4).
// At 200/s (a rate the 3 ms server can carry: capacity 1 / 3 ms = 333/s) one spike gives
//   s = 2 ms, rho = 200 x 0.002 = 0.4, W = 2 ms / 0.6 = 3.333 ms
// and two give
//   s = 3 ms, rho = 200 x 0.003 = 0.6, W = 3 ms / 0.4 = 7.5 ms.
TEST_CASE("two latency spikes of 1 ms on one node add to a 3 ms service time: rho 0.6, mean 7.5 ms",
          "[theory]") {
  const auto once = records_of(
      graph_of(probe({.rate_per_s = 200,
                      .faults = one_fault("at: 10s, kind: latency_spike, node: s, extra: 1ms, "
                                          "for: 10s")})),
      300);
  REQUIRE_THAT(service_of(step_at(once, 15.0)).rho, WithinRel(0.4, 1e-9));
  REQUIRE_THAT(mean_s_of(step_at(once, 15.0)),
               WithinRel(mm1_mean_s(0.002, 0.4), 1e-9));  // 3.333 ms

  const auto twice =
      "faults:\n"
      "  - {at: 10s, kind: latency_spike, node: s, extra: 1ms, for: 10s}\n"
      "  - {at: 10s, kind: latency_spike, node: s, extra: 1ms, for: 10s}\n";
  const auto both = records_of(graph_of(probe({.rate_per_s = 200, .faults = twice})), 300);
  REQUIRE_THAT(service_of(step_at(both, 15.0)).rho, WithinRel(0.6, 1e-9));
  REQUIRE_THAT(mean_s_of(step_at(both, 15.0)), WithinRel(mm1_mean_s(0.003, 0.6), 1e-9));  // 7.5 ms
  REQUIRE_THAT(mean_s_of(step_at(both, 15.0)), WithinRel(0.0075, 1e-9));
}

// ---- Determinism and the identity
// ----------------------------------------------------------------

TEST_CASE("a scenario with faults run twice gives byte-identical JSON", "[theory]") {
  const auto s = probe({.faults = one_fault(ten_seconds_from_ten)});
  const auto first = run_json(s);
  REQUIRE(first == run_json(s));
  REQUIRE(first != run_json(probe({})));  // and the fault is in it: not vacuous
}

// The identity condition: a scenario with `faults: []` is the scenario without the key, byte for
// byte. (That the run without faults is unchanged from before faults existed is what the whole
// suite's pinned numbers already say; this pins the empty list to it.)
TEST_CASE("a scenario with an empty faults list is byte-identical to one without the key",
          "[theory]") {
  REQUIRE(run_json(probe({.faults = "faults: []\n"})) == run_json(probe({})));
}

// ---- estimate ignores faults and says so
// ---------------------------------------------------------

// `estimate` is the steady state without the faults: a node_down that is active at t = 0 (the
// instant estimate evaluates) must not show in it, and the report says it was left out. The
// numbers are the no-fault estimate's, field for field.
TEST_CASE("estimate ignores the faults, prints the note and equals the estimate without them",
          "[theory]") {
  const auto with_fault = graph_of(probe({.faults = one_fault("at: 0s, kind: node_down, node: s, "
                                                              "for: 10s")}));
  const auto without = graph_of(probe({}));
  const auto faulted = simulation_of(with_fault).estimate();
  const auto plain = simulation_of(without).estimate();

  REQUIRE(faulted.nodes.size() == plain.nodes.size());
  for (std::size_t i = 0; i < plain.nodes.size(); ++i) {
    CAPTURE(i);
    REQUIRE(faulted.nodes[i].demand_per_s == plain.nodes[i].demand_per_s);
    REQUIRE(faulted.nodes[i].drain_per_s == plain.nodes[i].drain_per_s);
    REQUIRE(faulted.nodes[i].dropped_per_s == plain.nodes[i].dropped_per_s);
    REQUIRE(faulted.nodes[i].backlog == plain.nodes[i].backlog);
    REQUIRE(faulted.nodes[i].rho == plain.nodes[i].rho);
  }
  REQUIRE_THAT(service_of(faulted).rho, WithinRel(0.4, 1e-9));  // the steady state of the probe
  REQUIRE(faulted.responses.at(0).answered.per_second == plain.responses.at(0).answered.per_second);
  REQUIRE(faulted.responses.at(0).response.mean_s == plain.responses.at(0).response.mean_s);

  // C++23 std::string::contains(sv): "is this text in the string?" in one call. Replaces
  // `find(x) != npos`, whose npos comparison is easy to write backwards.
  const auto table = simsys::report::render_table(
      simsys::report::Summary::of_estimate(with_fault, faulted), {}, /*color=*/false);
  CAPTURE(table);
  REQUIRE(table.contains(
      "faults: 1 scripted (estimate shows the steady state without them; run applies them)"));
  const auto plain_table = simsys::report::render_table(
      simsys::report::Summary::of_estimate(without, plain), {}, /*color=*/false);
  REQUIRE_FALSE(plain_table.contains("scripted"));
}

// `run` applies the faults, so its report has no such note: the note is estimate's caveat.
TEST_CASE("a run's table does not carry estimate's note about scripted faults", "[theory]") {
  const auto s = probe({.faults = one_fault(ten_seconds_from_ten)});
  const auto g = graph_of(s);
  simsys::report::Summary summary{g, s.warmup};
  auto sim = simulation_of(g);
  sim.run({.duration = s.duration, .step = s.step}, [&](const auto& r) { summary.observe(r); });
  REQUIRE_FALSE(simsys::report::render_table(summary, {}, /*color=*/false).contains("scripted"));
}

// ---- cache_flush: a cold cache warming back along the Che curve ---------------------------------
//
// Book ch. 1 (the cache tier) and ch. 8, 11 and 14 (the caches those designs lean on): what happens
// when the cache is lost, a restart or a deploy, and every read goes to the store behind it until
// the cache has refilled. Model under test (phase 3 Task 7): a flush empties the cache at `at`;
// from then on it refills from the requests it sees. After t seconds at `rate` requests a second
// it has seen n = rate x t requests and holds the distinct keys among them,
//   D(n) = sum_i (1 - e^{-p_i n})      (the expected number of distinct keys in n requests: Che's
//                                       own sum with t read as a count of requests)
// and its hit ratio is the Che hit ratio of a cache with that many entries:
//   h(t) = che_hit_ratio(min(keys, D(rate t)), popularity)
// The cache is full, and h back at its steady value, when D reaches `keys`: at n = t_c requests,
// t_c = che_characteristic_time(keys, popularity) (the definition of the characteristic time is
// D(t_c) = keys), t_c / rate seconds after the flush. Ignored on purpose: that real LRU caches
// refill in recency order (the warm keys are the recent ones, a little better than "distinct"),
// the thundering herd on the store, and the writes a flush loses.
//
// The numbers: 1k/s over Zipf 0.8 of 1M keys into a cache of 10k keys (computed here from
// caching::, and cross-checked once by a dense sum over all 1M ranks in plain Python, 2026-10-30):
//   steady hit ratio h = 0.23191        characteristic time t_c = 12,106 requests = 12.11 s
//   D(2000) = 1820.3 keys (fewer than the 10k, so the cache is still filling at 2 s)
// The test helpers distinct_keys and warm_hit_ratio (tests/support/fixtures.cppm) are D(n) and h(t)
// above.
//
// What the run exposes. A StepRecord has no per-cache hit ratio, so the tests read it off the
// origin behind the cache: the cache (strategy read_through) or cdn sends the misses, 1 - h of
// the reads, on to its origin as the same op, so
//   origin.demand_per_s = rate x (1 - h(t))      and so      h(t) = 1 - origin.demand_per_s / rate
// at a step that starts at t after the flush (a link of zero latency delivers within the step).
// The step that STARTS at `at` is the first one in the flush: its hit ratio is 0. (The flush is
// written `at: 10m`, a duration from the start of the run: `at: 00:10` would be a clock time,
// ten minutes past midnight, which is the same instant, but the duration form cannot be misread.)

namespace {

// The origin behind the cache or cdn `edge`: u -> edge -> origin, links of zero latency. The origin
// (100k/s, 1 ms: c = 100 servers) is nowhere near saturated at the loads used. Node indices: u 0,
// edge 1, origin 2.
struct FlushProbe {
  int rate_per_s = 1000;
  std::string client_keys = ", keys: {zipf: 0.8, space: 1M}";
  std::string edge =
      "type: cache, strategy: read_through, backend: origin, keys: 10k, hit_ratio: che";
  std::string flush_at = "10m";
  std::string duration = "11m";
};

constexpr std::size_t origin_node = 2;

Scenario flush_probe(const FlushProbe& p) {
  return load_yaml("version: 1\nname: flush\nduration: " + p.duration +
                   "\nstep: 100ms\n"
                   "nodes:\n"
                   "  u: {type: client, rate: {base: " +
                   std::to_string(p.rate_per_s) + "/s}, ops: {get: 1.0}" + p.client_keys +
                   "}\n"
                   "  edge: {" +
                   p.edge +
                   "}\n"
                   "  origin: {type: service, capacity: 100k/s, latency: 1ms}\n"
                   "links:\n"
                   "  - {from: u, to: edge, dir: inout, latency: 0s}\n"
                   "  - {from: edge, to: origin, dir: inout, latency: 0s}\n"
                   "faults:\n  - {at: " +
                   p.flush_at + ", kind: cache_flush, node: edge}\n");
}

double origin_demand(const StepRecord& r) {
  return r.nodes[origin_node].demand_per_s;
}

// The cache's hit ratio at a step, read off the origin's demand (see the section header).
double hit_ratio_at(const StepRecord& r, double rate_per_s) {
  return 1.0 - origin_demand(r) / rate_per_s;
}

constexpr double che_rate = 1000;  // requests per second reaching the che cache
constexpr double che_keys = 10'000;
constexpr std::size_t flush_step = 6000;  // the step that starts at 10 min = 600 s, at 100 ms
constexpr int run_steps = 6300;           // 630 s: ten minutes steady, the flush, 30 s of warming

// Zipf 0.8 over 1M keys: the popularity the che cache's hit ratio is computed from.
const simsys::caching::Popularity& zipf_popularity() {
  static const auto popularity = simsys::caching::Popularity::zipf(0.8, 1e6);
  return popularity;
}

// h: the cache's steady hit ratio, the Che hit ratio of its 10k keys.
double steady_hit_ratio() {
  return simsys::caching::che_hit_ratio(che_keys, zipf_popularity());
}

// h(t) of the model above, t seconds after the flush.
double expected_hit_ratio_after(double seconds) {
  return simsys::test_support::warm_hit_ratio(zipf_popularity(), che_keys, che_rate * seconds);
}

// The che cache's whole run, computed once for the tests that read it (6300 steps).
const std::vector<StepRecord>& che_cache_run() {
  static const auto records = records_of(graph_of(flush_probe({})), run_steps);
  return records;
}

// The record of the step that starts `k` steps after the flush.
const StepRecord& after_flush(const std::vector<StepRecord>& records, std::size_t k) {
  return records.at(flush_step + k);
}
}  // namespace

// Precondition of the tests below: before the flush the cache holds its 10k hottest-enough keys and
// the origin sees the miss share, 1000 x (1 - 0.23191) = 768.09/s. h is pinned twice: against
// caching::che_hit_ratio (what the engine is built on) and against a dense sum over all 1M ranks
// done once outside the suite (0.231905), so a wrong popularity cannot hide behind a shared helper.
TEST_CASE("a che cache of 10k keys over Zipf 0.8 hits 23.2 percent before the flush", "[theory]") {
  const double h = steady_hit_ratio();
  REQUIRE_THAT(h, WithinAbs(0.2319, 5e-4));
  const auto& records = che_cache_run();
  for (const double t : {1.0, 300.0, 599.9}) {
    CAPTURE(t);
    REQUIRE_THAT(hit_ratio_at(step_at(records, t), che_rate), WithinRel(h, 1e-9));
  }
  REQUIRE_THAT(origin_demand(step_at(records, 300.0)), WithinRel(1000.0 * (1.0 - h), 1e-9));
}

// The flush at 10 min: the step that starts at 600.0 s finds an empty cache, so every read misses
// and the origin is offered the whole 1000/s (hit ratio 0). The step before is untouched.
TEST_CASE("a cache_flush drops the che cache's hit ratio to 0 at the step that starts at the flush",
          "[theory]") {
  const auto& records = che_cache_run();
  REQUIRE_THAT(hit_ratio_at(step_at(records, 599.9), che_rate),
               WithinRel(steady_hit_ratio(), 1e-9));
  REQUIRE_THAT(origin_demand(step_at(records, 600.0)), WithinRel(1000.0, 1e-12));
  REQUIRE_THAT(hit_ratio_at(step_at(records, 600.0), che_rate), WithinAbs(0.0, 1e-12));
}

// Two seconds after the flush the cache has seen 1000 x 2 = 2000 requests, which hold
// D(2000) = 1820.3 distinct keys (fewer than 2000: the hot keys repeat), well under the 10k it can
// hold, so its hit ratio is the Che hit ratio of a cache of 1820.3 entries (the equation is
// continuous in the capacity, so a fractional one is fine): 0.12677, about half of
// h = 0.23191.
TEST_CASE(
    "two seconds after a flush the che cache hits like a cache of the distinct keys of 2000 "
    "requests",
    "[theory]") {
  const double distinct = simsys::test_support::distinct_keys(zipf_popularity(), 2000);
  REQUIRE_THAT(distinct, WithinAbs(1820.3, 0.1));  // dense sum, plain Python, 2026-10-30
  REQUIRE(distinct < che_keys);                    // so min(keys, D) is D: still filling
  const double expected = simsys::caching::che_hit_ratio(distinct, zipf_popularity());
  REQUIRE(expected > 0.0);
  REQUIRE(expected < steady_hit_ratio());
  REQUIRE_THAT(hit_ratio_at(step_at(che_cache_run(), 602.0), che_rate), WithinRel(expected, 1e-9));
  REQUIRE_THAT(origin_demand(step_at(che_cache_run(), 602.0)),
               WithinRel(1000.0 * (1.0 - expected), 1e-9));
}

// Every step of the warm-up, from the flush to a little after the characteristic time, follows the
// curve h(t) = che_hit_ratio(min(keys, D(rate t))): not just the points above. The hit ratio never
// falls while the cache fills.
TEST_CASE("the che cache's hit ratio follows the warm-up curve at every step until it is full",
          "[theory]") {
  const auto& records = che_cache_run();
  double previous = -1.0;
  for (std::size_t k = 0; k <= 125; ++k) {
    CAPTURE(k);
    const double seconds = 0.1 * static_cast<double>(k);
    const double observed = hit_ratio_at(after_flush(records, k), che_rate);
    REQUIRE_THAT(observed, WithinRel(expected_hit_ratio_after(seconds), 1e-9) ||
                               WithinAbs(expected_hit_ratio_after(seconds), 1e-12));
    REQUIRE(observed >= previous);
    previous = observed;
  }
}

// Full: D(n) reaches the 10k keys at n = t_c = che_characteristic_time(10k, popularity) = 12,106
// requests (the definition of the characteristic time), 12.1 s at 1k/s. From the first step that
// starts at or after it the hit ratio is exactly the steady h, and it stays there.
TEST_CASE(
    "the che cache is back at its steady hit ratio once the keys of rate times t requests "
    "fill it",
    "[theory]") {
  const double t_c = simsys::caching::che_characteristic_time(che_keys, zipf_popularity());
  REQUIRE_THAT(t_c, WithinAbs(12106.0, 2.0));  // requests; plain Python bisection, 2026-10-30
  const double full_after_s =
      t_c / che_rate;  // 12.106 s: the first step to start after it is 12.2 s
  const auto& records = che_cache_run();
  for (std::size_t k = 122; k < 300; ++k) {
    CAPTURE(k);
    REQUIRE(0.1 * static_cast<double>(k) >= full_after_s);
    REQUIRE_THAT(hit_ratio_at(after_flush(records, k), che_rate),
                 WithinRel(steady_hit_ratio(), 1e-9));
  }
  // and just before it (12.0 s: 12,000 requests, 106 short) it is below h by a hair, not at h
  REQUIRE(hit_ratio_at(after_flush(records, 120), che_rate) < steady_hit_ratio());
}

// When is the cache "back"? The first step at which its hit ratio is within 1 % of h. The curve
// h(t) is concave and flattens as D approaches 10k, so it gets within 1 % of h BEFORE the cache is
// full: the answer is a model output, pinned against the closed form (the scan below, over the same
// h(t) the engine must follow) and bracketed: after the flush, before the full time of 12.11 s,
// and the step before it is outside the band.
TEST_CASE(
    "the che cache first returns within 1 percent of its steady hit ratio at the step the "
    "closed form names",
    "[theory]") {
  const double h = steady_hit_ratio();
  const auto within_one_percent = [h](double hit) { return std::abs(hit - h) <= 0.01 * h; };
  std::size_t expected_step = 0;
  for (std::size_t k = 0; k <= 125; ++k) {
    if (within_one_percent(expected_hit_ratio_after(0.1 * static_cast<double>(k)))) {
      expected_step = k;
      break;
    }
  }
  CAPTURE(expected_step);
  // 11.8 s after the flush: h(11.8 s) / h = 0.99206, the step before (11.7 s) is 0.98943. Pinned as
  // a number too, so a change of the closed form is noticed (the scan above is the model; this is
  // its answer, from caching:: on 2026-10-30), and it is before the characteristic time (12.11 s).
  REQUIRE(expected_step == 118);

  const auto& records = che_cache_run();
  std::size_t observed_step = 0;
  for (std::size_t k = 0; k <= 125; ++k) {
    if (within_one_percent(hit_ratio_at(after_flush(records, k), che_rate))) {
      observed_step = k;
      break;
    }
  }
  REQUIRE(observed_step == expected_step);
  REQUIRE_FALSE(
      within_one_percent(hit_ratio_at(after_flush(records, expected_step - 1), che_rate)));
}

// A fixed hit ratio is a number, not a function of the cache's size, so the model has no curve to
// follow: the simple case. The cache refills at one key per request, so it is full after
// keys / rate seconds (1000 keys at 100/s: 10 s), and its hit ratio warms LINEARLY from 0 to the
// stated 0.9 over that time:
//   h(t) = 0.9 x min(1, t / 10 s)      0, 0.09 at 1 s, 0.225 at 2.5 s, 0.45 at 5 s (half), 0.9 at
//   10 s
// The origin sees 100 x (1 - h(t)): 100, 91, 77.5, 55, 10 per second.
TEST_CASE("a fixed-ratio cache warms linearly to its ratio over keys divided by rate: 0.45 at half",
          "[theory]") {
  const auto records = records_of(
      graph_of(flush_probe({.rate_per_s = 100,
                            .client_keys = "",
                            .edge = "type: cache, strategy: read_through, backend: origin, "
                                    "keys: 1000, hit_ratio: {fixed: 0.9}",
                            .flush_at = "10s",
                            .duration = "30s"})),
      300);
  REQUIRE_THAT(hit_ratio_at(step_at(records, 9.9), 100.0), WithinRel(0.9, 1e-9));  // before: 0.9
  const double fill_time_s = 1000.0 / 100.0;
  for (const double after : {0.0, 1.0, 2.5, 5.0, 7.5, 10.0, 12.0, 19.9}) {
    CAPTURE(after);
    const double expected = 0.9 * std::min(1.0, after / fill_time_s);
    const auto& r = step_at(records, 10.0 + after);
    REQUIRE_THAT(origin_demand(r), WithinRel(100.0 * (1.0 - expected), 1e-9));
  }
  REQUIRE_THAT(hit_ratio_at(step_at(records, 15.0), 100.0),
               WithinRel(0.45, 1e-9));  // half the fill
  REQUIRE_THAT(origin_demand(step_at(records, 15.0)), WithinRel(55.0, 1e-9));
  REQUIRE_THAT(origin_demand(step_at(records, 10.0)), WithinRel(100.0, 1e-12));  // empty: all miss
}

// A cdn is a read-only cache in front of its origin (ch. 1, ch. 14), so a flush acts on it the same
// way: hit ratio 0 at the step that starts at the flush, the steady h before it, and the same
// warm-up curve after it (checked at 2 s).
TEST_CASE("a cache_flush on a cdn drops its hit ratio to 0 and warms it back like a cache's",
          "[theory]") {
  const auto records = records_of(
      graph_of(flush_probe({.edge = "type: cdn, origin: origin, keys: 10k, hit_ratio: che"})),
      6030);
  REQUIRE_THAT(hit_ratio_at(step_at(records, 599.9), che_rate),
               WithinRel(steady_hit_ratio(), 1e-9));
  REQUIRE_THAT(origin_demand(step_at(records, 600.0)), WithinRel(1000.0, 1e-12));
  REQUIRE_THAT(hit_ratio_at(step_at(records, 602.0), che_rate),
               WithinRel(expected_hit_ratio_after(2.0), 1e-9));
}

// ---- dc_down: a datacenter's nodes and the links across its edge
// ---------------------------------
//
// Book ch. 1 ("data center" and "geoDNS": if a data center goes offline, all traffic is routed to
// the healthy one) and ch. 14's multi-region CDN. Model under test (phase 3 Task 7): dc_down names
// a datacenter and is, while it lasts,
//   node_down on every node whose `dc` it is              capacity x 0: nothing drains, the room
//                                                         fills, the rest is dropped
//   capacity factor 0 on every link that crosses its edge  a link with one end in the datacenter
//                                                         and one outside, either direction
// A pipe (a link with a bandwidth) is a fluid queue of megabytes, so factor 0 makes it drain
// nothing: it fills its 1000 MB room and then drops everything that arrives, so its `passed` is 0.
// A plain link has no queue, so it drops what crosses it at once. A link with both ends inside the
// datacenter is not cut (its nodes are down anyway), and a client has no capacity, so a client in
// the datacenter keeps emitting.
// Assumed, where the plan text is silent: "every link leaving it" is read as "every link crossing
// its edge" (the plan's own probe sends a us client's requests INTO eu over the cross-DC link and
// expects it to pass 0); the two directions are pinned in separate tests below.
//
// The probe, 400/s per client, every service 1k/s with a room of 100 and zero-latency links:
//   c_us (us): `local` 200/s -> s_us (us),  `remote` 200/s -> s_eu (eu) over the cross-DC link
//   c_eu (eu): `eu_op` 400/s -> s_eu inside eu
// dc_down eu for 10 s from 10 s. s_eu is offered 400/s by c_eu alone (the 200/s of `remote` is cut
// on the link before it arrives), drains 0, so its backlog is the node_down staircase of the
// existing node_down test: 40, 80, then the room 100, dropping 0, 0, 200/s (20 of 40 spill), then
// 400/s. The only way the backlog reaches the room at exactly that pace is if 400/s arrives, which
// is what tells a cut link from an uncut one (an uncut one would make it 60, 100).

namespace {

constexpr std::string_view eu_down_for_ten_seconds =
    "faults:\n  - {at: 10s, kind: dc_down, datacenter: eu, for: 10s}\n";

Scenario two_datacenters(std::string_view faults) {
  return load_yaml(
      "version: 1\nname: dc\nduration: 30s\nstep: 100ms\n"
      "datacenters:\n  us: {}\n  eu: {}\n"
      "nodes:\n"
      "  c_us: {type: client, dc: us, rate: {base: 400/s}, ops: {local: 0.5, remote: 0.5}}\n"
      "  s_us: {type: service, dc: us, capacity: 1k/s, latency: 1ms, advanced: {queue_limit: "
      "100}}\n"
      "  s_eu: {type: service, dc: eu, capacity: 1k/s, latency: 1ms, advanced: {queue_limit: "
      "100}}\n"
      "  c_eu: {type: client, dc: eu, rate: {base: 400/s}, ops: {eu_op: 1.0}}\n"
      "links:\n"
      "  - {from: c_us, to: s_us, ops: [local], dir: inout, latency: 0s}\n"
      "  - {from: c_us, to: s_eu, ops: [remote], dir: inout, latency: 0s}\n"  // crosses us -> eu
      "  - {from: c_eu, to: s_eu, dir: inout, latency: 0s}\n" +               // inside eu
      std::string{faults});
}

// The probe's nodes, in declaration order.
constexpr std::size_t s_us_node = 1;
constexpr std::size_t s_eu_node = 2;

const simsys::engine::NodeSample& node_of(const StepRecord& r, std::size_t node) {
  return r.nodes.at(node);
}

// The client's sample for `op` (the ops of the probes are unique across their clients).
const simsys::engine::ResponseSample& response_of(const StepRecord& r, std::string_view op) {
  for (const auto& response : r.responses) {
    if (response.op == op) return response;
  }
  FAIL("no client offers op " << op);
  // C++23 std::unreachable(): tells the compiler control never gets here (FAIL throws), so the
  // function needs no return value after the loop. Replaces __builtin_unreachable(), GCC's own
  // spelling; reaching it is undefined behaviour.
  std::unreachable();
}

const std::vector<StepRecord>& dc_down_run() {
  static const auto records = records_of(graph_of(two_datacenters(eu_down_for_ten_seconds)), 300);
  return records;
}
}  // namespace

// The node half: every node of eu has capacity 0 for the ten seconds. The one service in eu drains
// nothing, its backlog climbs the staircase to the room, and it drops once the room is full; its
// own client keeps offering 400/s and is answered 0 once the room is full (as at a node_down). The
// service in us is not touched.
TEST_CASE(
    "during a dc_down every node of the datacenter drains nothing and its backlog is the room",
    "[theory]") {
  const auto& records = dc_down_run();
  for (double t = 10.0; t < 19.95; t += 0.1) {
    CAPTURE(t);
    const auto& s_eu = node_of(step_at(records, t), s_eu_node);
    REQUIRE_THAT(s_eu.drain_per_s, WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(s_eu.demand_per_s, WithinRel(400.0, 1e-9));  // c_eu's; `remote` is cut on the link
  }
  REQUIRE_THAT(node_of(step_at(records, 10.0), s_eu_node).backlog, WithinRel(40.0, 1e-9));
  REQUIRE_THAT(node_of(step_at(records, 10.1), s_eu_node).backlog, WithinRel(80.0, 1e-9));
  for (double t = 10.2; t < 19.95; t += 0.1) {
    CAPTURE(t);
    REQUIRE_THAT(node_of(step_at(records, t), s_eu_node).backlog, WithinRel(100.0, 1e-9));
  }
  REQUIRE_THAT(node_of(step_at(records, 10.1), s_eu_node).dropped_per_s, WithinAbs(0.0, 1e-9));
  REQUIRE_THAT(node_of(step_at(records, 10.2), s_eu_node).dropped_per_s, WithinRel(200.0, 1e-9));
  REQUIRE_THAT(node_of(step_at(records, 10.3), s_eu_node).dropped_per_s, WithinRel(400.0, 1e-9));
  for (double t = 10.3; t < 19.95; t += 0.1) {  // c_eu offers 400/s and is answered nothing
    CAPTURE(t);
    REQUIRE_THAT(response_of(step_at(records, t), "eu_op").offered.per_second,
                 WithinRel(400.0, 1e-12));
    REQUIRE_THAT(response_of(step_at(records, t), "eu_op").answered.per_second,
                 WithinAbs(0.0, 1e-9));
  }
  // before the fault: all 600/s (400 + the cross-DC 200) are served, nothing waits
  const auto& before = node_of(step_at(records, 9.9), s_eu_node);
  REQUIRE_THAT(before.demand_per_s, WithinRel(600.0, 1e-9));
  REQUIRE_THAT(before.drain_per_s, WithinRel(600.0, 1e-9));
  REQUIRE(before.backlog == 0.0);
}

// The link half, seen from the client in us: the cross-DC link passes 0, so of the 400/s it
// offers, the 200/s of `local` that stays in us is answered (the service in us never notices) and
// the 200/s of `remote` that crosses to eu is not, from the first step of the fault to the last.
// A plain link has no queue, so it drops at once, with no staircase. After the fault both are
// answered again.
TEST_CASE("during a dc_down the client in us is answered only the share that stays in us",
          "[theory]") {
  const auto& records = dc_down_run();
  for (double t = 10.0; t < 19.95; t += 0.1) {
    CAPTURE(t);
    const auto& r = step_at(records, t);
    REQUIRE_THAT(response_of(r, "local").offered.per_second, WithinRel(200.0, 1e-12));
    REQUIRE_THAT(response_of(r, "local").answered.per_second, WithinRel(200.0, 1e-9));
    REQUIRE_THAT(response_of(r, "remote").offered.per_second, WithinRel(200.0, 1e-12));
    REQUIRE_THAT(response_of(r, "remote").answered.per_second, WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(node_of(r, s_us_node).demand_per_s, WithinRel(200.0, 1e-9));
    REQUIRE_THAT(node_of(r, s_us_node).drain_per_s, WithinRel(200.0, 1e-9));
    REQUIRE(node_of(r, s_us_node).backlog == 0.0);
  }
  const auto& before = step_at(records, 9.9);
  REQUIRE_THAT(response_of(before, "local").answered.per_second, WithinRel(200.0, 1e-9));
  REQUIRE_THAT(response_of(before, "remote").answered.per_second, WithinRel(200.0, 1e-9));
  for (double t = 20.0; t < 29.95; t += 0.1) {
    CAPTURE(t);
    REQUIRE_THAT(response_of(step_at(records, t), "remote").answered.per_second,
                 WithinRel(200.0, 1e-9));
  }
}

// Recovery: the capacity is back at 1000/s the instant the fault ends, the cross-DC link passes
// again, so s_eu is offered 400 + 200 = 600/s plus the 100 waiting in its room. By the fluid queue,
// drain = min(demand + backlog / dt, capacity) and backlog' = backlog + (demand - drain) dt:
//   step 20.0: drain min(600 + 1000, 1000) = 1000   backlog 100 + (600 - 1000) 0.1 = 60
//   step 20.1: drain min(600 +  600, 1000) = 1000   backlog  60 + (600 - 1000) 0.1 = 20
//   step 20.2: drain min(600 +  200, 1000) =  800   backlog  20 + (600 -  800) 0.1 =  0
//   step 20.3: drain 600, nothing left.       Nothing is dropped after the fault.
TEST_CASE(
    "after a dc_down the datacenter's backlog drains in three steps and the link passes again",
    "[theory]") {
  const auto& records = dc_down_run();
  const double drains[] = {1000.0, 1000.0, 800.0, 600.0};
  const double backlogs[] = {60.0, 20.0, 0.0, 0.0};
  for (std::size_t i = 0; i < 4; ++i) {
    CAPTURE(i);
    const auto& s_eu = node_of(step_at(records, 20.0 + 0.1 * static_cast<double>(i)), s_eu_node);
    REQUIRE_THAT(s_eu.demand_per_s, WithinRel(600.0, 1e-9));  // the cross-DC 200/s are back
    REQUIRE_THAT(s_eu.drain_per_s, WithinRel(drains[i], 1e-9));
    REQUIRE_THAT(s_eu.backlog, WithinAbs(backlogs[i], 1e-9));
  }
  for (double t = 20.0; t < 29.95; t += 0.1) {
    CAPTURE(t);
    REQUIRE_THAT(node_of(step_at(records, t), s_eu_node).dropped_per_s, WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(response_of(step_at(records, t), "eu_op").answered.per_second,
                 WithinRel(400.0, 1e-9));
  }
}

// The other direction: a client INSIDE the datacenter sending to a service outside it crosses the
// edge going out. Read as "a link leaving the datacenter", the plan's own wording: it drops what
// the client puts on it, so the service in us receives nothing from eu for the ten seconds and the
// client is answered 0 from the first step. Before and after, 400/s flow.
TEST_CASE(
    "during a dc_down a link leaving the datacenter drops what a client inside sends across it",
    "[theory]") {
  const auto s = load_yaml(
      "version: 1\nname: dc\nduration: 30s\nstep: 100ms\n"
      "datacenters:\n  us: {}\n  eu: {}\n"
      "nodes:\n"
      "  c_eu: {type: client, dc: eu, rate: {base: 400/s}, ops: {op: 1.0}}\n"
      "  s_us: {type: service, dc: us, capacity: 1k/s, latency: 1ms, advanced: {queue_limit: "
      "100}}\n"
      "links:\n  - {from: c_eu, to: s_us, dir: inout, latency: 0s}\n" +
      std::string{eu_down_for_ten_seconds});
  const auto records = records_of(graph_of(s), 300);
  for (double t = 10.0; t < 19.95; t += 0.1) {
    CAPTURE(t);
    const auto& r = step_at(records, t);
    REQUIRE_THAT(node_of(r, 1).demand_per_s, WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(response_of(r, "op").offered.per_second, WithinRel(400.0, 1e-12));
    REQUIRE_THAT(response_of(r, "op").answered.per_second, WithinAbs(0.0, 1e-9));
  }
  for (const double t : {9.9, 20.0, 25.0}) {
    CAPTURE(t);
    REQUIRE_THAT(node_of(step_at(records, t), 1).demand_per_s, WithinRel(400.0, 1e-9));
    REQUIRE_THAT(response_of(step_at(records, t), "op").answered.per_second,
                 WithinRel(400.0, 1e-9));
  }
}

// A pipe is a link with a bandwidth: a fluid queue in megabytes. Two pipes cross eu's edge, one
// each way: c_us -> s_eu (into eu, link row 0) and c_eu -> s_us (out of eu, link row 1), each
// carrying 15k/s of 1 MB requests = 15,000 MB/s over 400 Gbit/s = 50,000 MB/s (rho 0.3), with the
// 1000 MB room of every pipe (the rows come in the order of the links in the file). Factor 0 on a
// pipe: it drains nothing, so each step's 1500 MB arrive on a pipe that
//   step 10.0: keeps 1000 MB (the room), drops 500 MB = 5000 MB/s, drains 0
//   step 10.1 on: is full: drops everything that arrives, 15,000 MB/s = its demand, drains 0
// ("its inflow" in the plan's words), and from then on a request over it never gets through: its
// `passed` is 0, which the client sees as answered 0. After the fault the backlog of 1000 MB drains
// at once: drain = min(15,000 + 1000 / 0.1, 50,000) = 25,000 MB/s, backlog 0; then 15,000 again.
namespace {
Scenario pipes_across_eu() {
  return load_yaml(
      "version: 1\nname: dc\nduration: 30s\nstep: 100ms\n"
      "datacenters:\n  us: {}\n  eu: {}\n"
      "nodes:\n"
      "  c_us: {type: client, dc: us, rate: {base: 15k/s}, ops: {to_eu: 1.0}, sizes: {to_eu: "
      "1MB}}\n"
      "  s_eu: {type: service, dc: eu, capacity: 1M/s, latency: 1ms}\n"
      "  c_eu: {type: client, dc: eu, rate: {base: 15k/s}, ops: {to_us: 1.0}, sizes: {to_us: "
      "1MB}}\n"
      "  s_us: {type: service, dc: us, capacity: 1M/s, latency: 1ms}\n"
      "links:\n"
      "  - {from: c_us, to: s_eu, latency: 0s, bandwidth: 400Gbit/s}\n"
      "  - {from: c_eu, to: s_us, latency: 0s, bandwidth: 400Gbit/s}\n" +
      std::string{eu_down_for_ten_seconds});
}
}  // namespace

TEST_CASE(
    "during a dc_down a pipe across the datacenter's edge drains nothing, fills its room and "
    "drops everything",
    "[theory]") {
  const auto records = records_of(graph_of(pipes_across_eu()), 300);
  for (const std::size_t link : {std::size_t{0}, std::size_t{1}}) {  // into eu, out of eu
    CAPTURE(link);
    const auto& before = step_at(records, 9.9).links.at(link);
    REQUIRE_THAT(before.demand_per_s, WithinRel(15000.0, 1e-9));
    REQUIRE_THAT(before.drain_per_s, WithinRel(15000.0, 1e-9));
    REQUIRE_THAT(before.dropped_per_s, WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(before.backlog, WithinAbs(0.0, 1e-9));

    const auto& first = step_at(records, 10.0).links.at(link);
    REQUIRE_THAT(first.drain_per_s, WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(first.backlog, WithinRel(1000.0, 1e-9));  // the room
    REQUIRE_THAT(first.dropped_per_s, WithinRel(5000.0, 1e-9));
    for (double t = 10.1; t < 19.95; t += 0.1) {
      CAPTURE(t);
      const auto& row = step_at(records, t).links.at(link);
      REQUIRE_THAT(row.demand_per_s, WithinRel(15000.0, 1e-9));
      REQUIRE_THAT(row.drain_per_s, WithinAbs(0.0, 1e-9));
      REQUIRE_THAT(row.dropped_per_s, WithinRel(row.demand_per_s, 1e-9));  // dropped = its inflow
      REQUIRE_THAT(row.backlog, WithinRel(1000.0, 1e-9));
    }
  }
}

// `passed` 0: with the room full, no request gets across, so neither client is answered anything
// while it keeps offering 15k/s; the pipe's recovery step drains the 1000 MB backlog in one step.
TEST_CASE(
    "a pipe across the datacenter's edge passes nothing during a dc_down and recovers after it",
    "[theory]") {
  const auto records = records_of(graph_of(pipes_across_eu()), 300);
  for (const auto* op : {"to_eu", "to_us"}) {
    CAPTURE(op);
    REQUIRE_THAT(response_of(step_at(records, 9.9), op).answered.per_second,
                 WithinRel(15000.0, 1e-9));
    for (double t = 10.1; t < 19.95; t += 0.1) {
      CAPTURE(t);
      REQUIRE_THAT(response_of(step_at(records, t), op).offered.per_second,
                   WithinRel(15000.0, 1e-12));
      REQUIRE_THAT(response_of(step_at(records, t), op).answered.per_second, WithinAbs(0.0, 1e-9));
    }
    REQUIRE_THAT(response_of(step_at(records, 20.0), op).answered.per_second,
                 WithinRel(15000.0, 1e-9));
  }
  for (const std::size_t link : {std::size_t{0}, std::size_t{1}}) {
    CAPTURE(link);
    const auto& recovering = step_at(records, 20.0).links.at(link);
    REQUIRE_THAT(recovering.drain_per_s, WithinRel(25000.0, 1e-9));
    REQUIRE_THAT(recovering.backlog, WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(recovering.dropped_per_s, WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(step_at(records, 20.1).links.at(link).drain_per_s, WithinRel(15000.0, 1e-9));
    REQUIRE_THAT(step_at(records, 20.1).links.at(link).backlog, WithinAbs(0.0, 1e-9));
  }
}

// The wait during a dc_down is finite (Task 6's review, finding 1): a request admitted to a room
// that will drain once the datacenter is back waits for the rest of the outage, then for the
// backlog ahead of it at the nominal capacity. At 15.0 s, 5 s before the end:
//   s_eu (two_datacenters)   5 s + 100 / 1000/s, then its 1 ms service    = 5.101 s
//   a pipe across the edge   5 s + 1000 MB / 50,000 MB/s = 5.02 s of wait, then the request's first
//                            megabyte, 1 / 50,000 s: the link's latency    = 5.02002 s
// (Before, both were infinite: backlog / 0.) The pins of the dc_down tests above are unchanged.
TEST_CASE("during a dc_down a request waits for the datacenter's node and the cut pipe to be back",
          "[theory]") {
  const auto& s_eu = node_of(step_at(dc_down_run(), 15.0), s_eu_node);
  REQUIRE_THAT(s_eu.latency.mean_s, WithinRel(5.0 + 0.1 + 0.001, 1e-9));
  const auto records = records_of(graph_of(pipes_across_eu()), 300);
  for (const std::size_t link : {std::size_t{0}, std::size_t{1}}) {
    CAPTURE(link);
    const auto& row = step_at(records, 15.0).links.at(link);
    REQUIRE(std::isfinite(row.latency.mean_s));
    REQUIRE_THAT(row.latency.mean_s, WithinRel(5.0 + 1000.0 / 50'000.0 + 1.0 / 50'000.0, 1e-9));
  }
}

// ---- dc_down is run, no longer refused ----------------------------------------------------------

// Phase 3 Task 6 refused dc_down (`dc_down lands in Task 7`); Task 7 runs it. The scenario of the
// refusal's test, with every node in `a` (a client and a service of 1k/s, default room 1000):
// Simulation::create accepts it, the service drains nothing for the 5 s from 10 s, and its backlog
// starts up the staircase 400/s x 0.1 s = 40, 80 per step (the client, in `a`, keeps emitting).
TEST_CASE("Simulation::create accepts a dc_down fault and the datacenter's service serves nothing",
          "[theory]") {
  const auto s = load_yaml(
      "version: 1\nname: faults\nduration: 30s\nstep: 100ms\n"
      "datacenters:\n  a: {}\n  b: {}\n"
      "nodes:\n"
      "  c: {type: client, dc: a, rate: {base: 400/s}, ops: {op: 1.0}}\n"
      "  s: {type: service, dc: a, capacity: 1k/s, latency: 1ms}\n"
      "links:\n  - {from: c, to: s, dir: inout, latency: 0s}\n"
      "faults:\n  - {at: 10s, kind: dc_down, datacenter: a, for: 5s}\n");
  const auto g = graph_of(s);
  const auto sim = Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());

  const auto records = records_of(g, 300);
  REQUIRE_THAT(service_of(step_at(records, 9.9)).drain_per_s, WithinRel(400.0, 1e-12));
  for (double t = 10.0; t < 14.95; t += 0.1) {
    CAPTURE(t);
    REQUIRE_THAT(service_of(step_at(records, t)).drain_per_s, WithinAbs(0.0, 1e-9));
  }
  REQUIRE_THAT(service_of(step_at(records, 10.0)).backlog, WithinRel(40.0, 1e-9));
  REQUIRE_THAT(service_of(step_at(records, 10.1)).backlog, WithinRel(80.0, 1e-9));
  REQUIRE_THAT(service_of(step_at(records, 15.0)).drain_per_s, WithinRel(1000.0, 1e-9));  // back
}

// ---- A sql_db's primary and its read replicas (Task 6's review, finding 2) ---------------------
//
// Book ch. 1, "database replication": writes go to the primary, reads to the read replicas, and
// "if the master database goes offline, a slave database will be promoted". The probe: c sends
// 1k/s, 800 reads and 200 writes, to `db`, a sql_db of N = 3 copies (a primary and 2 read
// replicas), each 10k units/s with a 1 ms service (c = 10 servers) and a room of 100 units. A write
// costs 5 units (defaults::sql_write_cost). Before any fault:
//   primary        200 writes x 5 = 1000 units/s                         rho 1000 / 10k = 0.10
//   each replica   800 / 2 = 400 reads + the 1000 replayed write units  rho 1400 / 10k = 0.14
// (the node's rho is its busiest group's: 0.14).
namespace {
Scenario database_probe(std::string_view fault) {
  return load_yaml(
      "version: 1\nname: db\nduration: 30s\nstep: 100ms\n"
      "nodes:\n"
      "  c: {type: client, rate: {base: 1k/s}, ops: {read: 0.8, write: 0.2}}\n"
      "  db: {type: sql_db, capacity: 10k/s, latency: 1ms, replicas: 3, advanced: {queue_limit: "
      "100}}\n"
      "links:\n  - {from: c, to: db, dir: inout, latency: 0s}\n" +
      one_fault(fault));
}
}  // namespace

// `replicas: primary` for 10 s from 10 s: the primary's capacity is 0, its replicas untouched.
// The writes fill the primary's room in one step (200 writes/s x 5 units x 0.1 s = 100 units, the
// room exactly) and from 10.1 s every write is dropped: answered 0. A write the room took waits
// until the primary is back, then for the room's 100 units at 10k/s, then 1 ms: at 15.0 s 5 s +
// 0.01 s + 1 ms = 5.011 s. The reads go on: answered 800/s at the replicas, whose load falls to
// their 400 reads (the primary commits nothing, so they replay nothing): rho 0.04, a read's mean
// 1 ms (Erlang C at c = 10, rho 0.04 waits nothing measurable). Before the fix a `primary` was one
// copy of three down, every group at 2/3 of its capacity, and the writes were all answered.
TEST_CASE("replicas primary on a sql_db stops its writes and leaves its reads answered",
          "[theory]") {
  const auto records =
      records_of(graph_of(database_probe("at: 10s, kind: node_down, node: db, replicas: primary, "
                                         "for: 10s")),
                 300);
  const auto& before = step_at(records, 9.9);
  REQUIRE_THAT(response_of(before, "write").answered.per_second, WithinRel(200.0, 1e-9));
  REQUIRE_THAT(response_of(before, "read").answered.per_second, WithinRel(800.0, 1e-9));
  for (double t = 10.1; t < 19.95; t += 0.1) {
    CAPTURE(t);
    const auto& r = step_at(records, t);
    REQUIRE_THAT(response_of(r, "write").offered.per_second, WithinRel(200.0, 1e-9));
    REQUIRE_THAT(response_of(r, "write").answered.per_second, WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(response_of(r, "read").answered.per_second, WithinRel(800.0, 1e-9));
    REQUIRE_THAT(response_of(r, "read").response.mean_s, WithinRel(0.001, 1e-6));
    REQUIRE_THAT(node_of(r, 1).committed_writes_per_s.value_or(-1.0), WithinAbs(0.0, 1e-9));
  }
  REQUIRE_THAT(response_of(step_at(records, 15.0), "write").response.mean_s,
               WithinRel(5.0 + 0.01 + 0.001, 1e-9));
  REQUIRE_THAT(response_of(step_at(records, 25.0), "write").answered.per_second,
               WithinRel(200.0, 1e-9));
}

// `replicas: 1` for 10 s from 10 s: one of the 2 read replicas down, the primary untouched. Each
// replica copy keeps its share of the load (400 reads and the 1000 replayed units) on (3 − 1 − 1)
// / (3 − 1) = half its capacity, the survivors sharing the load evenly as fluid::Derating scales
// them: rho = 1400 / 5000 = 0.28, twice 0.14 (the reads' 2/1 survivors' ratio; the replay is scaled
// with them, though in fact each replica replays its shard's writes once, see Derating), while the
// primary stays at 0.10. Nothing is lost: reads and writes are all answered. Before the fix every
// group ran at 2/3 of its capacity (primary 0.15, replicas 0.21).
TEST_CASE("replicas 1 on a sql_db of 3 halves its read replicas and leaves its primary",
          "[theory]") {
  const auto records = records_of(
      graph_of(database_probe("at: 10s, kind: node_down, node: db, replicas: 1, for: 10s")), 300);
  REQUIRE_THAT(node_of(step_at(records, 9.9), 1).rho, WithinRel(0.14, 1e-9));
  for (const double t : {10.0, 15.0, 19.9}) {
    CAPTURE(t);
    const auto& r = step_at(records, t);
    REQUIRE_THAT(node_of(r, 1).rho, WithinRel(1400.0 / 5000.0, 1e-9));
    REQUIRE_THAT(node_of(r, 1).dropped_per_s, WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(response_of(r, "read").answered.per_second, WithinRel(800.0, 1e-9));
    REQUIRE_THAT(response_of(r, "write").answered.per_second, WithinRel(200.0, 1e-9));
    // the write's sojourn is the primary's, still at rho 0.1: about its 1 ms service
    REQUIRE_THAT(response_of(r, "write").response.mean_s, WithinRel(0.001, 1e-6));
  }
  REQUIRE_THAT(node_of(step_at(records, 20.0), 1).rho, WithinRel(0.14, 1e-9));
}

// ---- A kv_cluster's quorum loss under machine failure (Task 6's review, finding 5) --------------
//
// Book ch. 6 (key-value store, "handling failures"): every key lives on N of the cluster's M
// machines, and a request is answered once W (a write) or R (a read) of its key's N replicas
// reply. When r machines are down, the keys whose replica set holds j of them have N − j live
// replicas. A request on such a key is LOST when N − j is below its quorum (strict quorum: the
// coordinator waits for no other machine), and otherwise answered by the quorum's fastest replies
// among the live ones: the q-th of N − j, not of N. The share of the keys with j of the r down
// machines in their set is hypergeometric (the set is N distinct machines of the M):
//   P(j) = C(r, j) C(M − r, N − j) / C(M, N)
// so one machine is in N / M of the sets, and two machines are both in (N / M)(N − 1)/(M − 1) of
// them. With N < M the ring has one hot machine (k times the average load), and the machines that
// go down are taken from the cool ones: a set is {hot, N − 1 cool} with probability p_hot = N k / M
// and {N cool} otherwise, its cool slots drawn from the M − 1 cool machines.
namespace {
// u -> store: a kv_cluster of `machines` machines, N = 3, W = R = 2 (the defaults), 10k/s and 1 ms
// each (c = 10). u sends 1k reads/s.
Scenario kv_probe(int machines, std::string_view faults) {
  return load_yaml(
      "version: 1\nname: kv\nduration: 30s\nstep: 100ms\n"
      "nodes:\n"
      "  u: {type: client, rate: {base: 1k/s}, ops: {read: 1.0}}\n"
      "  store: {type: kv_cluster, nodes: " +
      std::to_string(machines) +
      ", capacity: 10k/s, latency: 1ms}\n"
      "links:\n  - {from: u, to: store, dir: inout, latency: 0s}\n" +
      std::string{faults});
}

using simsys::behaviours::KvClusterBehaviour;
using simsys::behaviours::MachineGroup;
using simsys::flow::LatencyEstimate;

// The q = 2nd fastest reply of `hot` replicas on the hot machine and `cool` on cool ones.
LatencyEstimate second_of(const MachineGroup& hot_machine, int hot,
                          const MachineGroup& cool_machine, int cool) {
  using simsys::queueing::ReplicaGroup;
  const std::array set{ReplicaGroup{.replicas = hot,
                                    .sojourn = {.mean_s = hot_machine.sojourn.mean_s,
                                                .variance_s2 = hot_machine.sojourn.variance_s2}},
                       ReplicaGroup{.replicas = cool,
                                    .sojourn = {.mean_s = cool_machine.sojourn.mean_s,
                                                .variance_s2 = cool_machine.sojourn.variance_s2}}};
  const auto reply = simsys::queueing::lognormal_order_statistic(set, 2);
  return {reply.mean_s, reply.variance_s2};
}

// A mixture of latencies by share (the shares sum to 1), by the law of total variance: the mean of
// the means, and the mean of the variances plus the variance of the means.
// C++20 std::span<const T>: a read-only view of contiguous (share, latency) pairs, so the caller
// passes a std::array of any length without a template or a (pointer, count) pair.
LatencyEstimate mixed(std::span<const std::pair<double, LatencyEstimate>> parts) {
  double mean = 0.0;
  for (const auto& [share, latency] : parts) mean += share * latency.mean_s;
  double variance = 0.0;
  for (const auto& [share, latency] : parts) {
    const double apart = latency.mean_s - mean;
    variance += share * (latency.variance_s2 + apart * apart);
  }
  return {mean, variance};
}

LatencyEstimate kv_read_response(const KvClusterBehaviour& kv) {
  return kv.response("read", LatencyEstimate{},
                     [](std::size_t, std::string_view) { return LatencyEstimate{}; });
}

double kv_read_delivered(const KvClusterBehaviour& kv) {
  return kv.delivered("read", 1.0, [](std::size_t, std::string_view) { return 1.0; });
}

// A hot machine slower than the cool ones, both keeping all they are sent.
const MachineGroup slow_hot{.sojourn = {0.002, 4e-6}, .kept = 1.0};
const MachineGroup fast_cool{.sojourn = {0.001, 1e-6}, .kept = 1.0};
}  // namespace

// The behaviour's formula, driven by hand. M = 6, N = 3, W = R = 2: the cool pool is the 5 cool
// machines. Two of them down (MachineGroup::down = 2):
//   {hot, 2 cool}  (p_hot)       j = 0: C(3,2)/C(5,2) = 3/10   live {hot, 2 cool}: the 2nd of 3
//                                j = 1: C(2,1)C(3,1)/10 = 6/10 live {hot, 1 cool}: the 2nd of 2
//                                j = 2: C(2,2)/10 = 1/10       live {hot}: 1 < 2, LOST
//   {3 cool}       (1 − p_hot)   j = 0: C(3,3)/C(5,3) = 1/10   live 3 cool: the 2nd of 3
//                                j = 1: C(2,1)C(3,2)/10 = 6/10 live 2 cool: the 2nd of 2
//                                j = 2: C(2,2)C(3,1)/10 = 3/10 live 1 cool: LOST
// so the loss is L = p_hot / 10 + 3 (1 − p_hot) / 10 (two machines both in a set of 3 cool slots
// of 5: (3/5)(2/4) = 3/10, the formula of the comment above with M − 1 = 5), and the response is
// the four answered branches mixed by their shares over 1 − L. One machine down: every set keeps
// at least 2 live replicas, nothing is lost (j = 1 of a {3 cool} set: C(1,1)C(4,2)/C(5,3) = 6/10).
TEST_CASE("a kv_cluster with machines down answers from the live replicas and loses below quorum",
          "[theory]") {
  const auto g = graph_of(kv_probe(6, ""));
  const std::size_t store = *g.index_of("store");
  const double p_hot = 3.0 * g.nodes()[store].imbalance / 6.0;
  REQUIRE(p_hot > 0.5);  // k = 1.0604 (ring_factor(6, 3, 150))
  REQUIRE(p_hot < 0.54);
  KvClusterBehaviour kv{g, store};

  MachineGroup two_down = fast_cool;
  two_down.down = 2;
  kv.observe(std::array{slow_hot, two_down});
  const double loss = p_hot * 0.1 + (1.0 - p_hot) * 0.3;
  REQUIRE_THAT(kv_read_delivered(kv), WithinRel(1.0 - loss, 1e-12));
  const std::array answered{
      std::pair{p_hot * 0.3 / (1.0 - loss), second_of(slow_hot, 1, fast_cool, 2)},
      std::pair{p_hot * 0.6 / (1.0 - loss), second_of(slow_hot, 1, fast_cool, 1)},
      std::pair{(1.0 - p_hot) * 0.1 / (1.0 - loss), second_of(slow_hot, 0, fast_cool, 3)},
      std::pair{(1.0 - p_hot) * 0.6 / (1.0 - loss), second_of(slow_hot, 0, fast_cool, 2)}};
  const auto expected = mixed(answered);
  REQUIRE_THAT(kv_read_response(kv).mean_s, WithinRel(expected.mean_s, 1e-9));
  REQUIRE_THAT(kv_read_response(kv).variance_s2, WithinRel(expected.variance_s2, 1e-9));

  MachineGroup one_down = fast_cool;
  one_down.down = 1;
  kv.observe(std::array{slow_hot, one_down});
  REQUIRE(kv_read_delivered(kv) == 1.0);
  const std::array all_answered{
      std::pair{p_hot * 0.6, second_of(slow_hot, 1, fast_cool, 2)},  // C(4,2)/C(5,2)
      std::pair{p_hot * 0.4, second_of(slow_hot, 1, fast_cool, 1)},
      std::pair{(1.0 - p_hot) * 0.4, second_of(slow_hot, 0, fast_cool, 3)},  // C(4,3)/C(5,3)
      std::pair{(1.0 - p_hot) * 0.6, second_of(slow_hot, 0, fast_cool, 2)}};
  REQUIRE_THAT(kv_read_response(kv).mean_s, WithinRel(mixed(all_answered).mean_s, 1e-9));
}

// With N = M = 3 the ring is even (every machine holds every key, no hot machine) and every set is
// the three machines. One down: every key keeps 2 live replicas, W = R = 2 still met, each read
// answered at the 2nd of 2. Two down: 1 live replica, below the quorum: every read lost (N/M (N −
// 1)/(M − 1) = 1: both down machines are in every set).
TEST_CASE("a kv_cluster of three machines keeping three copies loses every read with two down",
          "[theory]") {
  const auto g = graph_of(kv_probe(3, ""));
  KvClusterBehaviour kv{g, *g.index_of("store")};
  MachineGroup one_down = fast_cool;
  one_down.down = 1;
  kv.observe(std::array{one_down});
  REQUIRE(kv_read_delivered(kv) == 1.0);
  REQUIRE_THAT(kv_read_response(kv).mean_s,
               WithinRel(second_of(slow_hot, 0, fast_cool, 2).mean_s, 1e-12));
  MachineGroup two_down = fast_cool;
  two_down.down = 2;
  kv.observe(std::array{two_down});
  REQUIRE(kv_read_delivered(kv) == 0.0);
}

// The same in a run. M = 6, two machines down for 10 s from 10 s (`replicas: 2`): the client is
// answered 1 − L of its 1k reads/s (L above, the machines keep all they are sent at this load), and
// the coordinator sends nothing to a down machine while the survivors keep their own share: the
// store's demand, in replica-requests, is the hot machine's 3000 k / 6 plus 3 of its 5 cool
// machines' 3000 (6 − k) / 30 each, against all 5 before (3000 in all). One machine down
// (`replicas: 1`): nothing lost. Before the fix a node_down of r machines scaled every machine to
// (M − r) / M of its capacity and lost nothing.
TEST_CASE("a node_down of two of a kv_cluster's six machines loses the reads whose quorum is gone",
          "[theory]") {
  const auto two = graph_of(
      kv_probe(6, one_fault("at: 10s, kind: node_down, node: store, replicas: 2, for: 10s")));
  const double k = two.nodes()[*two.index_of("store")].imbalance;
  const double p_hot = 3.0 * k / 6.0;
  const double loss = p_hot * 0.1 + (1.0 - p_hot) * 0.3;
  const auto records = records_of(two, 300);
  REQUIRE_THAT(response_of(step_at(records, 9.9), "read").answered.per_second,
               WithinRel(1000.0, 1e-9));
  REQUIRE_THAT(node_of(step_at(records, 9.9), 1).demand_per_s, WithinRel(3000.0, 1e-9));
  for (const double t : {10.0, 15.0, 19.9}) {
    CAPTURE(t);
    const auto& r = step_at(records, t);
    REQUIRE_THAT(response_of(r, "read").answered.per_second,
                 WithinRel(1000.0 * (1.0 - loss), 1e-9));
    REQUIRE_THAT(node_of(r, 1).demand_per_s,
                 WithinRel(3000.0 * k / 6.0 + 3.0 * 3000.0 * (6.0 - k) / 30.0, 1e-9));
    REQUIRE_THAT(node_of(r, 1).dropped_per_s, WithinAbs(0.0, 1e-9));
  }
  REQUIRE_THAT(response_of(step_at(records, 20.0), "read").answered.per_second,
               WithinRel(1000.0, 1e-9));

  const auto one = records_of(
      graph_of(kv_probe(6, one_fault("at: 10s, kind: node_down, node: store, replicas: 1, "
                                     "for: 10s"))),
      300);
  REQUIRE_THAT(response_of(step_at(one, 15.0), "read").answered.per_second,
               WithinRel(1000.0, 1e-9));
}

// ---- The fault block: what the fault cost, what followed, when the system recovered
// --------------
//
// Book ch. 1 and ch. 12 ask of every failure three things: what did it cost while it lasted, what
// came after it (the backlog to work off, the cache to refill), and when was the system itself
// again. The report answers them in a block of four lines per fault, after the SLO table (spec
// §4.4; the lines are the same for every kind of fault):
//   fault <at> <kind> <node> for <for>        the clock time `at` as the SLO windows print it
//     during:   offered, served, lost, backlog   what the faulted node was sent, what it served
//     after:    what followed                    (means over the fault), the requests its room
//     recovery: when everything was back         dropped, its backlog's peak, SLOs that flipped
// Model under test (phase 3 Task 8), the engine's FaultRecord per fault, read by the report:
//   offered, served  the faulted node's demand and drain, per second, averaged over [at, at + for)
//   lost             the requests its full room dropped over the fault (sum of dropped x dt)
//   backlog, room    the peak backlog over the fault, and the room (queue_limit) it filled
//   flipped          an SLO that held at the step before the fault and is broken at some step
//                    inside it, judged on that step alone (a p99_latency SLO: the op's p99 at the
//                    step, from the moments the step answered with)
//   after            a node_down's backlog drained: from the end of the fault to the end of the
//                    first step whose backlog is empty; a flushed cache's warm-up time: keys read
//                    off the same curve as Task 7 (t_c / rate for a che cache, keys / rate for a
//                    fixed ratio)
//   recovery         the first step starting at or after the end of the fault at which every
//                    client op's p99 and every node's rho are within 1 % of the step before `at`
//                    (relative to it): the time of the START of that step. Measured, not
//                    estimated: on the M/M/1 probe below the third step after the fault is the
//                    first with rho back at 0.4 (steps 20.0 and 20.1 s have rho 1 and 0.8), so
//                    recovery is at 20.2 s, 0.2 s after the fault cleared. (The plan estimated
//                    20.3 s; its 20.3 is the END of that step. The tests pin the start: the system
//                    IS back from 20.2 s, the backlog having been empty since that instant, and it
//                    agrees with the drain time of the line above.)
// Formats chosen here: the clock is window_clock's (a whole minute `00:10`, a whole second
// `00:00:10`) and, for a time that is not a whole second, HH:MM:SS with a tenth (`00:00:20.2`:
// window_clock would print the exact duration, `20200ms`); durations after `in`, `for` and `after`
// are seconds with at most one decimal (`0.2s`, `12.1s`, `0s`), except the header's `for`, which is
// the scenario's own (`10s`, `200ms`). Ignored on purpose: the follow-ons of Tasks 10 and 15 (the
// client's extra attempts, a reconnect storm), which add their own `after:` text.

namespace {
// What a run reports with the fault records its engine kept (session::run_summary does the same).
struct FaultRun {
  std::string table;
  std::string json;
  std::vector<simsys::engine::FaultRecord> faults;
};

FaultRun fault_run(const Scenario& s) {
  const auto g = graph_of(s);
  simsys::report::Summary summary{g, s.warmup};
  auto sim = simulation_of(g);
  sim.run({.duration = s.duration, .step = s.step}, [&](const auto& r) { summary.observe(r); });
  const auto records = sim.fault_records();
  summary.set_faults(records);
  const auto verdicts = simsys::report::evaluate_slos(summary, s.slos);
  return {simsys::report::render_table(summary, verdicts, /*color=*/false),
          simsys::report::render_json(summary, verdicts),
          std::vector<simsys::engine::FaultRecord>{records.begin(), records.end()}};
}

// The probe's ten second node_down, whose block is pinned in full by the tests below.
constexpr std::string_view node_down_block =
    "fault 00:00:10 node_down s for 10s\n"
    "  during:   offered 400/s, served 0/s, lost 3.9k requests, backlog 100 (room 100 dropped the "
    "rest)\n"
    "  after:    backlog drained in 0.2s\n"
    "  recovery: op p99 back within 1% at 00:00:20.2 (0.2s after the fault cleared)\n";

// Three p99 SLOs on the op: one the outage breaks (100 ms; the probe's p99 is 8.2 ms before it),
// one that already fails before it (1 ms), one it never breaks (1 h).
constexpr std::string_view three_p99_slos =
    "slo:\n"
    "  - {metric: p99_latency, node: c, op: op, max: 100ms}\n"
    "  - {metric: p99_latency, node: c, op: op, max: 1ms}\n"
    "  - {metric: p99_latency, node: c, op: op, max: 1h}\n";

// The text of the JSON from `key` on, and the number after `key` (a key written `"name": `).
double number_after(const std::string& text, std::string_view key) {
  const auto at = text.find(key);
  CAPTURE(key);
  REQUIRE(at != std::string::npos);
  return std::stod(text.substr(at + key.size()));
}

// The keys in the order given, each after the one before (the layout of an object, without
// pinning the numbers between them).
void require_in_order(const std::string& text, std::initializer_list<std::string_view> keys) {
  std::size_t from = 0;
  for (const auto key : keys) {
    CAPTURE(key);
    const auto at = text.find(key, from);
    REQUIRE(at != std::string::npos);
    from = at + key.size();
  }
}
}  // namespace

// The probe's outage, whole: 400/s offered and nothing served for 10 s, the room (100) kept the
// first 100 of the 4000 and dropped 3900 (above), the backlog was the room, drained in two steps
// (0.2 s) once the capacity was back, and from the step at 20.2 s the op's p99 and every rho are
// the pre-fault ones. The block is the last thing in the table when there is no SLO.
TEST_CASE("a ten second node_down prints its block: lost 3.9k, drained in 0.2s, recovered at 20.2",
          "[theory]") {
  const auto run = fault_run(probe({.faults = one_fault(ten_seconds_from_ten)}));
  CAPTURE(run.table);
  // C++20 std::string::ends_with: a suffix test in one call, replacing
  // `s.size() >= n && s.compare(s.size() - n, n, x) == 0`.
  REQUIRE(run.table.ends_with(node_down_block));
  REQUIRE(run.table.find("fault ") == run.table.rfind("fault "));  // one block, not two
}

// The block follows the SLO table (the table's last section), not the op table.
TEST_CASE("the fault block comes after the SLO table", "[theory]") {
  const auto run = fault_run(
      probe({.faults = one_fault(ten_seconds_from_ten), .slos = std::string{three_p99_slos}}));
  CAPTURE(run.table);
  const auto slo_header = run.table.find("\nslo\n");
  REQUIRE(slo_header != std::string::npos);
  REQUIRE(run.table.find("fault 00:00:10 node_down s for 10s\n") > slo_header);
  REQUIRE(run.table.contains("  after:    backlog drained in 0.2s\n"));
}

// The SLO the outage breaks is named at the end of the during line, in the words of the SLO table's
// columns (metric, node, op) and its verdict strings: `p99_latency c op flipped to FAIL`. The one
// that already failed before the fault (1 ms) and the one it never broke (1 h) are not named.
TEST_CASE("the during line ends with the SLO the outage flipped to FAIL and names no other",
          "[theory]") {
  const auto run = fault_run(
      probe({.faults = one_fault(ten_seconds_from_ten), .slos = std::string{three_p99_slos}}));
  CAPTURE(run.table);
  REQUIRE(run.table.contains(
      "  during:   offered 400/s, served 0/s, lost 3.9k requests, backlog 100 (room 100 dropped "
      "the rest), p99_latency c op flipped to FAIL\n"));
  REQUIRE(run.table.find("flipped to FAIL") == run.table.rfind("flipped to FAIL"));  // only one
  REQUIRE(run.faults.size() == 1);
  REQUIRE(run.faults[0].flipped == std::vector<std::size_t>{0});  // the first SLO of the list
}

// An outage shorter than the room: 200 ms is two steps of 40 arrivals, so 80 wait (below the room
// of 100) and nothing is dropped. The during line then has no parenthesis (the room did not fill),
// and says `lost 0 requests`. After the fault, drain = min(400 + backlog / 0.1 s, 1000)/s:
//   10.2 s: min(400 + 800, 1000) = 1000, backlog 80 + (400 - 1000) 0.1 = 20
//   10.3 s: min(400 + 200, 1000) =  600, backlog 20 + (400 -  600) 0.1 = 0     rho 1, then 0.6
//   10.4 s: drain 400, rho 0.4: the first step with every rho back, 0.2 s after the fault.
// The backlog is empty from 10.4 s: drained in 0.2 s.
TEST_CASE("a 200 ms node_down loses nothing: backlog 80 stays under the room of 100", "[theory]") {
  const auto run = fault_run(probe({.faults = one_fault("at: 10s, kind: node_down, node: s, "
                                                        "for: 200ms")}));
  CAPTURE(run.table);
  REQUIRE(run.table.ends_with(
      "fault 00:00:10 node_down s for 200ms\n"
      "  during:   offered 400/s, served 0/s, lost 0 requests, backlog 80\n"
      "  after:    backlog drained in 0.2s\n"
      "  recovery: op p99 back within 1% at 00:00:10.4 (0.2s after the fault cleared)\n"));
}

// Nothing is lost to a latency spike or a capacity fault: a latency_spike of 1 ms makes the
// service time 2 ms (capacity 500/s, rho 0.8) and a capacity factor of 0.5 does the same to the
// capacity (500/s, rho 0.8), both above the 400/s offered, so the node serves everything and keeps
// no backlog. The line keeps its shape, with `lost 0 requests` and `backlog 0`, and `after` says
// there was nothing to drain. The first step after the fault is the pre-fault step again (rho 0.4,
// the same p99), so the system is recovered at the instant the fault ends, 0s after it cleared.
TEST_CASE("a latency_spike loses nothing: lost 0 requests, nothing to drain, recovered at once",
          "[theory]") {
  const auto run = fault_run(
      probe({.faults = one_fault("at: 20s, kind: latency_spike, node: s, extra: 1ms, for: 5s")}));
  CAPTURE(run.table);
  REQUIRE(run.table.ends_with(
      "fault 00:00:20 latency_spike s for 5s\n"
      "  during:   offered 400/s, served 400/s, lost 0 requests, backlog 0\n"
      "  after:    no backlog to drain\n"
      "  recovery: op p99 back within 1% at 00:00:25 (0s after the fault cleared)\n"));
}

TEST_CASE("a capacity fault loses nothing either: same lines, served 400 per second", "[theory]") {
  const auto run = fault_run(
      probe({.faults = one_fault("at: 20s, kind: capacity, node: s, factor: 0.5, for: 5s")}));
  CAPTURE(run.table);
  REQUIRE(run.table.ends_with(
      "fault 00:00:20 capacity s for 5s\n"
      "  during:   offered 400/s, served 400/s, lost 0 requests, backlog 0\n"
      "  after:    no backlog to drain\n"
      "  recovery: op p99 back within 1% at 00:00:25 (0s after the fault cleared)\n"));
}

// A run that ends before the system is back: the same outage, 20.1 s of run. The fault clears at
// 20 s and the one step left (20.0 s) is the first of the drain: 40 requests still waiting (above),
// rho 1. The backlog is not empty and the rho is not back by the end of the run.
TEST_CASE("a node_down that ends 0.1 s before the run does is not recovered by the end of the run",
          "[theory]") {
  const auto run = fault_run(probe(
      {.faults = one_fault(ten_seconds_from_ten), .timing = "duration: 20100ms\nstep: 100ms\n"}));
  CAPTURE(run.table);
  REQUIRE(run.table.ends_with(
      "fault 00:00:10 node_down s for 10s\n"
      "  during:   offered 400/s, served 0/s, lost 3.9k requests, backlog 100 (room 100 dropped "
      "the rest)\n"
      "  after:    backlog not drained by the end of the run\n"
      "  recovery: not recovered by the end of the run\n"));
  REQUIRE(run.faults.size() == 1);
  REQUIRE_FALSE(run.faults[0].recovered_at.has_value());
  REQUIRE_FALSE(run.faults[0].after.backlog_drained_in_s.has_value());
}

// Two faults declared latest first print earliest first. The node_down of 5 s at 10 s: the room
// keeps 100 of 2000 offered, 1900 are lost, the block is as the probe's with the 5 s shifted; the
// latency spike at 20 s is the one above.
TEST_CASE("two faults declared out of order print in at order, one block each", "[theory]") {
  const auto run = fault_run(
      probe({.faults = "faults:\n"
                       "  - {at: 20s, kind: latency_spike, node: s, extra: 1ms, for: 5s}\n"
                       "  - {at: 10s, kind: node_down, node: s, for: 5s}\n"}));
  CAPTURE(run.table);
  REQUIRE(run.table.ends_with(
      "fault 00:00:10 node_down s for 5s\n"
      "  during:   offered 400/s, served 0/s, lost 1.9k requests, backlog 100 (room 100 dropped "
      "the rest)\n"
      "  after:    backlog drained in 0.2s\n"
      "  recovery: op p99 back within 1% at 00:00:15.2 (0.2s after the fault cleared)\n"
      "fault 00:00:20 latency_spike s for 5s\n"
      "  during:   offered 400/s, served 400/s, lost 0 requests, backlog 0\n"
      "  after:    no backlog to drain\n"
      "  recovery: op p99 back within 1% at 00:00:25 (0s after the fault cleared)\n"));
  REQUIRE(run.faults.size() == 2);
  require_in_order(run.json,
                   {"\"faults\": [", "\"at_s\": 10", "node_down", "\"at_s\": 20", "latency_spike"});
}

// A flushed cache: the block's header has no `for` (a flush is an instant: model::lasts), and the
// after line is the warm-up time of Task 7's curve, t_c / rate = 12,106 requests / 1000/s = 12.1 s
// on the che probe above (the flush at 10 minutes, `00:10` on the clock: a whole minute). The
// during and recovery lines are there, and their numbers are not pinned (what a flush costs is
// the hit ratio, which the origin's demand carries, not a loss).
TEST_CASE("a cache_flush on the che probe prints: cache warm again in 12.1s", "[theory]") {
  const auto run = fault_run(flush_probe({}));
  CAPTURE(run.table);
  REQUIRE(run.table.contains("fault 00:10 cache_flush edge\n"));
  REQUIRE(run.table.contains("  during:   "));
  REQUIRE(run.table.contains("  after:    cache warm again in 12.1s\n"));
  REQUIRE(run.table.contains("  recovery: "));
  REQUIRE(run.faults.size() == 1);
  REQUIRE(run.faults[0].duration == simsys::SimTime{0});
  REQUIRE(run.faults[0].after.cache_warm_in_s.has_value());
  REQUIRE_THAT(*run.faults[0].after.cache_warm_in_s, WithinAbs(12.106, 0.01));
  REQUIRE_FALSE(run.faults[0].after.backlog_drained_in_s.has_value());
}

// A fixed-ratio cache refills linearly in keys / rate = 1000 / 100/s = 10 s (above), so its block
// reads `cache warm again in 10s`: a whole number of seconds prints without a decimal.
TEST_CASE("a fixed-ratio cache_flush prints: cache warm again in 10s", "[theory]") {
  const auto run =
      fault_run(flush_probe({.rate_per_s = 100,
                             .client_keys = "",
                             .edge = "type: cache, strategy: read_through, backend: origin, "
                                     "keys: 1000, hit_ratio: {fixed: 0.9}",
                             .flush_at = "10s",
                             .duration = "30s"}));
  CAPTURE(run.table);
  REQUIRE(run.table.contains("fault 00:00:10 cache_flush edge\n"));
  REQUIRE(run.table.contains("  after:    cache warm again in 10s\n"));
}

// ---- The fault block in the JSON
// ------------------------------------------------------------------

// `faults` is the last key (after `slo`): one object per fault in at order,
//   {at_s, kind, node, for_s, during: {offered_per_s, served_per_s, lost, backlog_peak, room,
//    flipped: [text of the table's words]}, after: {backlog_drained_in_s, cache_warm_in_s},
//    recovered_at_s}
// seconds as doubles (the same numbers as the text, unrounded), null where the text says there is
// none: `recovered_at_s` when not recovered, `for_s` for a flush, and each `after` entry that does
// not apply.
TEST_CASE("the JSON has a faults array with the node_down's numbers in seconds", "[theory]") {
  const auto run = fault_run(
      probe({.faults = one_fault(ten_seconds_from_ten), .slos = std::string{three_p99_slos}}));
  const auto& json = run.json;
  CAPTURE(json);
  require_in_order(
      json,
      {"\"slo\": [", "\"faults\": [", "\"at_s\": 10", "\"kind\": \"node_down\"", "\"node\": \"s\"",
       "\"for_s\": 10", "\"during\": {", "\"offered_per_s\": ", "\"served_per_s\": ", "\"lost\": ",
       "\"backlog_peak\": ", "\"room\": ", "\"flipped\": [\"p99_latency c op\"]", "\"after\": {",
       "\"backlog_drained_in_s\": ", "\"cache_warm_in_s\": null", "\"recovered_at_s\": "});
  REQUIRE_THAT(number_after(json, "\"offered_per_s\": "), WithinRel(400.0, 1e-9));
  REQUIRE_THAT(number_after(json, "\"served_per_s\": "), WithinAbs(0.0, 1e-9));
  REQUIRE_THAT(number_after(json, "\"lost\": "), WithinRel(3900.0, 1e-9));
  REQUIRE_THAT(number_after(json, "\"backlog_peak\": "), WithinRel(100.0, 1e-9));
  REQUIRE_THAT(number_after(json, "\"room\": "), WithinRel(100.0, 1e-9));
  REQUIRE_THAT(number_after(json, "\"backlog_drained_in_s\": "), WithinAbs(0.2, 1e-9));
  REQUIRE_THAT(number_after(json, "\"recovered_at_s\": "), WithinAbs(20.2, 1e-9));
  REQUIRE(json.ends_with("}\n  ]\n}"));  // the faults array is the last thing in the report
}

TEST_CASE("the JSON of a fault never recovered has recovered_at_s null and no drain time",
          "[theory]") {
  const auto run = fault_run(probe(
      {.faults = one_fault(ten_seconds_from_ten), .timing = "duration: 20100ms\nstep: 100ms\n"}));
  CAPTURE(run.json);
  REQUIRE(run.json.contains("\"recovered_at_s\": null"));
  REQUIRE(run.json.contains("\"backlog_drained_in_s\": null"));
  REQUIRE(run.json.contains("\"flipped\": []"));  // no SLO in this scenario
}

TEST_CASE("the JSON of a cache_flush has for_s null and the warm-up time in seconds", "[theory]") {
  const auto run = fault_run(flush_probe({}));
  CAPTURE(run.json);
  REQUIRE(run.json.contains("\"kind\": \"cache_flush\""));
  REQUIRE(run.json.contains("\"for_s\": null"));
  REQUIRE(run.json.contains("\"backlog_drained_in_s\": null"));
  REQUIRE_THAT(number_after(run.json, "\"cache_warm_in_s\": "), WithinAbs(12.106, 0.01));
  REQUIRE_THAT(number_after(run.json, "\"at_s\": "), WithinAbs(600.0, 1e-9));
}

// No faults, no block and no key: a report without faults is what it was before the block existed.
// (The probe's JSON without faults and with `faults: []` are one text; neither has the key.)
TEST_CASE("a run without faults prints no fault block and no faults key", "[theory]") {
  for (const auto* faults : {"", "faults: []\n"}) {
    CAPTURE(faults);
    const auto run = fault_run(probe({.faults = faults, .slos = std::string{three_p99_slos}}));
    REQUIRE_FALSE(run.table.contains("fault "));
    REQUIRE_FALSE(run.table.contains("during:"));
    REQUIRE_FALSE(run.json.contains("\"faults\": "));  // (the probe's scenario is named faults)
    REQUIRE(run.json.ends_with("  ]\n}"));             // `slo` is still the last key
    REQUIRE(run.faults.empty());
  }
}
