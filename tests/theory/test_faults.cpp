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
// gets the identity condition, so a run without faults is the run of phase 2, bit for bit.
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
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>  // getpid

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.caching;
import simsys.config;
import simsys.core;
import simsys.engine;
import simsys.graph;
import simsys.model;
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
// C++20 designated initializers: `{.faults = "..."}` names the members a call sets, in
// declaration order, the rest keep their defaults. Replaces a call with five positional
// arguments (which compiles when two ints are swapped) or a builder class: the call site
// reads like the YAML it stands for.
struct Probe {
  int rate_per_s = 400;
  int replicas = 1;
  std::string faults{};
  std::string timing = "duration: 30s\nstep: 100ms\n";
};

Scenario probe(const Probe& p) {
  return load_yaml("version: 1\nname: faults\n" + p.timing +
                   "nodes:\n"
                   "  c: {type: client, rate: {base: " +
                   std::to_string(p.rate_per_s) +
                   "/s}, ops: {op: 1.0}}\n"
                   "  s: {type: service, capacity: 1k/s, latency: 1ms, replicas: " +
                   std::to_string(p.replicas) +
                   ", advanced: {queue_limit: 100}}\n"
                   "links:\n  - {from: c, to: s, dir: inout, latency: 0s}\n" +
                   p.faults);
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
  return simsys::report::render_json(summary, {});
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
