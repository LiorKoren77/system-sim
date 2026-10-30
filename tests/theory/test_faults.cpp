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

// ---- dc_down waits for Task 7
// ---------------------------------------------------------------------

// A temporary refusal: Simulation::create says dc_down is not run yet. Task 7 replaces this test
// with the datacenter's real behaviour.
TEST_CASE("Simulation::create refuses a dc_down fault with a message naming Task 7", "[theory]") {
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
  REQUIRE_FALSE(sim.has_value());
  CAPTURE(sim.error().message);
  REQUIRE(sim.error().message.contains("dc_down lands in Task 7"));
}
