// The link delay line: a ring buffer of flows, one slot per step of latency (spec §3.4), and the
// fault schedule: the condition (capacity, latency, rate) each node is in at a time (phase 3, spec
// §4.2).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <unistd.h>  // getpid

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.caching;
import simsys.config;
import simsys.core;
import simsys.engine;
import simsys.flow;
import simsys.graph;
import simsys.model;
import simsys.test_fixtures;

using namespace std::chrono_literals;
using Catch::Matchers::WithinRel;
using simsys::Rate;
using simsys::SimTime;
using simsys::engine::Condition;
using simsys::engine::DelayLine;
using simsys::engine::FaultSchedule;
using simsys::flow::Flow;
using simsys::graph::Graph;
using simsys::model::Scenario;

TEST_CASE("a zero-step delay line passes flow through in the same step", "[engine]") {
  DelayLine line{0};
  Flow f;
  f.add("x", Rate{5});
  line.push(f);
  REQUIRE(line.pop().rate_of("x").per_second == 5);
  line.push(Flow{});
  REQUIRE(line.pop().empty());
}

TEST_CASE("an n-step delay line returns a flow n pops later", "[engine]") {
  DelayLine line{2};
  Flow f;
  f.add("x", Rate{5});
  line.push(f);
  REQUIRE(line.pop().empty());  // step 0
  line.push(Flow{});
  REQUIRE(line.pop().empty());  // step 1
  line.push(Flow{});
  REQUIRE(line.pop().rate_of("x").per_second == 5);  // step 2
}

TEST_CASE("a delay line that is popped before any push yields nothing", "[engine]") {
  DelayLine line{1};
  REQUIRE(line.pop().empty());
}

// ---- FaultSchedule: the condition each node is in at a time -----------------------------------
//
// What the tests below assume of the engine's API (Task 6 of the phase 3 plan):
//   struct Condition { double capacity_factor = 1; SimTime extra_latency{};
//                      double rate_factor = 1; double hit_ratio_factor = 1; };
//   FaultSchedule(const model::Scenario&)                 // built from the scenario's typed faults
//     (Task 7 made it FaultSchedule(const graph::Graph&, flush reads), which the engine builds:
//     the helpers below take it from Simulation::fault_schedule_of(graph))
//   Condition FaultSchedule::condition_at(std::size_t node, SimTime now) const
// where `node` is the index the graph gives the node (Graph::index_of), the engine's own.
//
// A schedule is built from a scenario the loader typed, so the scenarios here are YAML text.
namespace {
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
                      (std::to_string(::getpid()) + "_simsys_schedule.yaml")};
  std::ofstream(file.path) << yaml;
  // C++23 std::expected<T, E>: the loader returns a Scenario or a ConfigError, and the caller
  // must look at which. It replaces exceptions and error codes; the error carries the file,
  // line and message, so a test that cannot load its scenario says why.
  auto loaded = simsys::config::load(file.path.string());
  if (!loaded)
    FAIL(loaded.error().file << ":" << loaded.error().line << ": " << loaded.error().message);
  return std::move(*loaded);
}

// A client `c` and three services of 1k/s: `s` and `u` with 4 replicas each (r of them can go
// down), `t` with 3; one minute at 100 ms. `faults` is the text of a `faults:` block.
struct Scheduled {
  Graph graph;
  FaultSchedule schedule;

  explicit Scheduled(std::string_view faults)
      : Scheduled{load_yaml("version: 1\nname: schedule\nduration: 1m\nstep: 100ms\n"
                            "nodes:\n"
                            "  c: {type: client, rate: {base: 100/s}, ops: {op: 1.0}}\n"
                            "  s: {type: service, capacity: 1k/s, latency: 1ms, replicas: 4}\n"
                            "  t: {type: service, capacity: 1k/s, latency: 1ms}\n"
                            "  u: {type: service, capacity: 1k/s, latency: 1ms, replicas: 3}\n"
                            "links:\n"
                            "  - {from: c, to: s, dir: inout, latency: 0s}\n"
                            "  - {from: c, to: t, dir: inout, latency: 0s}\n"
                            "  - {from: c, to: u, dir: inout, latency: 0s}\n" +
                            std::string{faults})} {}

  std::size_t node(std::string_view id) const { return *graph.index_of(id); }
  Condition at(std::string_view id, SimTime now) const {
    return schedule.condition_at(node(id), now);
  }

 private:
  explicit Scheduled(Scenario s)
      : graph{[&] {
          auto built = Graph::build(s);
          if (!built) FAIL(built.error().message);
          return std::move(*built);
        }()},
        schedule{simsys::engine::Simulation::fault_schedule_of(graph)} {}
};

// The condition that changes nothing: what a node gets outside every fault.
void require_identity(const Condition& c) {
  REQUIRE(c.capacity_factor == 1.0);
  REQUIRE(c.extra_latency == SimTime{0});
  REQUIRE(c.rate_factor == 1.0);
  REQUIRE(c.hit_ratio_factor == 1.0);
}
}  // namespace

// A capacity fault on `s` from 10 s for 10 s. Every other node, and `s` itself before and after,
// gets the identity: capacity x 1, no extra latency, rate x 1, hit ratio x 1.
TEST_CASE("a fault schedule gives the identity condition outside every fault", "[engine]") {
  const Scheduled x{"faults:\n  - {at: 10s, kind: capacity, node: s, factor: 0.5, for: 10s}\n"};
  for (const SimTime now : {SimTime{0s}, SimTime{9s}, SimTime{20s}, SimTime{59s}}) {
    CAPTURE(now.count());
    require_identity(x.at("s", now));
  }
  for (const auto* other : {"c", "t", "u"}) {  // another node never sees the fault
    CAPTURE(other);
    require_identity(x.at(other, 15s));
  }
}

// [at, at + for): the step that starts at `at` is in the fault, the instant `at + for` is out.
// SimTime counts nanoseconds, so one tick before each edge is the nearest instant outside / inside.
TEST_CASE("a fault is active on the half-open interval from at to at plus for", "[engine]") {
  const Scheduled x{"faults:\n  - {at: 10s, kind: capacity, node: s, factor: 0.5, for: 10s}\n"};
  require_identity(x.at("s", SimTime{10s} - 1ns));
  REQUIRE(x.at("s", 10s).capacity_factor == 0.5);
  REQUIRE(x.at("s", 15s).capacity_factor == 0.5);
  REQUIRE(x.at("s", SimTime{20s} - 1ns).capacity_factor == 0.5);
  require_identity(x.at("s", 20s));
}

// Two capacity faults of 0.5 over [10 s, 20 s) and [15 s, 25 s) overlap on [15 s, 20 s):
//   0.5 before 15 s, 0.5 x 0.5 = 0.25 on the overlap, 0.5 again from 20 s to 25 s: factors
//   multiply.
// Two latency spikes, 1 ms over [10 s, 20 s) and 2 ms over [15 s, 25 s): 1 ms, 1 + 2 = 3 ms on the
// overlap, 2 ms: extra latencies add. A node_down of 1 of 4 replicas (0.75) with a capacity fault
// of 0.5 on the same node composes to 0.75 x 0.5 = 0.375.
TEST_CASE("overlapping faults on one node compose: factors multiply, extra latencies add",
          "[engine]") {
  const Scheduled capacity{
      "faults:\n"
      "  - {at: 10s, kind: capacity, node: s, factor: 0.5, for: 10s}\n"
      "  - {at: 15s, kind: capacity, node: s, factor: 0.5, for: 10s}\n"};
  REQUIRE(capacity.at("s", 12s).capacity_factor == 0.5);
  REQUIRE(capacity.at("s", 17s).capacity_factor == 0.25);
  REQUIRE(capacity.at("s", 22s).capacity_factor == 0.5);
  require_identity(capacity.at("s", 25s));

  const Scheduled latency{
      "faults:\n"
      "  - {at: 10s, kind: latency_spike, node: s, extra: 1ms, for: 10s}\n"
      "  - {at: 15s, kind: latency_spike, node: s, extra: 2ms, for: 10s}\n"};
  REQUIRE(latency.at("s", 12s).extra_latency == SimTime{1ms});
  REQUIRE(latency.at("s", 17s).extra_latency == SimTime{3ms});
  REQUIRE(latency.at("s", 22s).extra_latency == SimTime{2ms});
  REQUIRE(latency.at("s", 17s).capacity_factor == 1.0);  // a latency spike leaves the factor alone

  const Scheduled mixed{
      "faults:\n"
      "  - {at: 10s, kind: node_down, node: s, replicas: 1, for: 10s}\n"
      "  - {at: 10s, kind: capacity, node: s, factor: 0.5, for: 10s}\n"};
  REQUIRE(mixed.at("s", 15s).capacity_factor == 0.375);
}

// A node_down of r of R replicas leaves (R - r) / R of the capacity: 1 of 4 is 3/4, 3 of 4 is
// 1/4, 1 of 3 is 2/3, all of them (no `replicas` key) is 0.
TEST_CASE("a node_down of r of R replicas gives capacity factor (R - r) over R", "[engine]") {
  const Scheduled x{
      "faults:\n"
      "  - {at: 1s, kind: node_down, node: s, replicas: 1, for: 1s}\n"  // 4 replicas: 3/4
      "  - {at: 3s, kind: node_down, node: s, replicas: 3, for: 1s}\n"  // 4 replicas: 1/4
      "  - {at: 5s, kind: node_down, node: u, replicas: 1, for: 1s}\n"  // 3 replicas: 2/3
      "  - {at: 7s, kind: node_down, node: s, for: 1s}\n"               // all of them: 0
      "  - {at: 9s, kind: node_down, node: t, for: 1s}\n"};             // the one replica: 0
  REQUIRE_THAT(x.at("s", 1s).capacity_factor, WithinRel(0.75, 1e-12));
  REQUIRE_THAT(x.at("s", 3s).capacity_factor, WithinRel(0.25, 1e-12));
  REQUIRE_THAT(x.at("u", 5s).capacity_factor, WithinRel(2.0 / 3.0, 1e-12));
  REQUIRE(x.at("s", 7s).capacity_factor == 0.0);
  REQUIRE(x.at("t", 9s).capacity_factor == 0.0);
  require_identity(x.at("s", 2s));  // between the faults
}

// A traffic_spike is a fault on the client: its rate is x factor and nothing else changes.
TEST_CASE("a traffic_spike gives the client its rate factor and leaves the other fields alone",
          "[engine]") {
  const Scheduled x{"faults:\n  - {at: 10s, kind: traffic_spike, node: c, factor: 3, for: 10s}\n"};
  const auto during = x.at("c", 15s);
  REQUIRE(during.rate_factor == 3.0);
  REQUIRE(during.capacity_factor == 1.0);
  REQUIRE(during.extra_latency == SimTime{0});
  REQUIRE(during.hit_ratio_factor == 1.0);
  require_identity(x.at("c", 20s));
}

// ---- cache_flush and dc_down (phase 3 Task 7) ---------------------------------------------------
//
// What the tests below assume of the engine's API, beyond Task 6's (above):
//   FaultSchedule(const model::Scenario&) stays the constructor. A che warm-up needs the cache's
//   popularity, which the scenario states (the client's `keys`) and the graph resolves, so Task 7
//   may well need the Graph too: `ScheduledScenario` below is the one place that builds a schedule,
//   and the one line to change then.
//   Condition::hit_ratio_factor is h(t) / h, the warm hit ratio over the steady one:
//     che cache   warm_hit_ratio(popularity, keys, rate x t) / che_hit_ratio(keys, popularity)
//     fixed ratio min(1, t / (keys / rate))                     (linear over the fill time)
//   0 at the flush instant `at`, 1 before it and from the moment the cache is full on; the other
//   three fields stay the identity throughout. A flush has no `for`, so the factor is not a
//   window of [at, at + for): it is 0 at `at` itself.
//   There is no `FaultSchedule::link_condition_at`: a dc_down's factor 0 on the links across the
//   datacenter's edge is tested through the run (tests/theory/test_faults.cpp), not here.
namespace {
// A graph and the schedule of its scenario, from a whole scenario text.
struct ScheduledScenario {
  Graph graph;
  FaultSchedule schedule;

  explicit ScheduledScenario(std::string_view yaml) : ScheduledScenario{load_yaml(yaml)} {}

  std::size_t node(std::string_view id) const { return *graph.index_of(id); }
  Condition at(std::string_view id, SimTime now) const {
    return schedule.condition_at(node(id), now);
  }

 private:
  explicit ScheduledScenario(Scenario s)
      : graph{[&] {
          auto built = Graph::build(s);
          if (!built) FAIL(built.error().message);
          return std::move(*built);
        }()},
        schedule{simsys::engine::Simulation::fault_schedule_of(graph)} {}
};

// u (`rate`/s of `get`, with the client's `keys` text) -> cache -> origin, with `cache` the node
// entry's text and the flush at 10 min.
std::string flushed_cache(std::string_view rate, std::string_view client_keys,
                          std::string_view cache) {
  return "version: 1\nname: flush\nduration: 11m\nstep: 100ms\n"
         "nodes:\n"
         "  u: {type: client, rate: {base: " +
         std::string{rate} + "}, ops: {get: 1.0}" + std::string{client_keys} +
         "}\n"
         "  cache: {" +
         std::string{cache} +
         "}\n"
         "  origin: {type: service, capacity: 100k/s, latency: 1ms}\n"
         "links:\n"
         "  - {from: u, to: cache, dir: inout, latency: 0s}\n"
         "  - {from: cache, to: origin, dir: inout, latency: 0s}\n"
         "faults:\n  - {at: 10m, kind: cache_flush, node: cache}\n";
}

// Only the hit ratio is touched: the three other fields are the identity.
void require_only_hit_ratio_changes(const Condition& c) {
  REQUIRE(c.capacity_factor == 1.0);
  REQUIRE(c.extra_latency == SimTime{0});
  REQUIRE(c.rate_factor == 1.0);
}
}  // namespace

// 1k/s over Zipf 0.8 of 1M keys into a 10k-key cache, flushed at 10 min:
//   before the flush  1 (the cache is warm: the identity)
//   at the flush      0 (a step that starts at `at` finds it empty)
//   2 s after         warm_hit_ratio(2000 requests) / h = che(1820.3 keys) / che(10k keys) = 0.1268
//   / 0.2319 = 0.547 13 s after        1 (t_c = 12,106 requests = 12.1 s: D(rate t) has reached the
//   10k keys)
// The factor is a ratio of two Che hit ratios, so it lies strictly between 0 and 1 while the cache
// fills, and rises with t.
TEST_CASE(
    "a cache_flush gives hit ratio factor 0 at the flush, the Che warm-up ratio while "
    "filling, 1 once full",
    "[engine]") {
  const ScheduledScenario x{flushed_cache(
      "1k/s", ", keys: {zipf: 0.8, space: 1M}",
      "type: cache, strategy: read_through, backend: origin, keys: 10k, hit_ratio: che")};
  const auto popularity = simsys::caching::Popularity::zipf(0.8, 1e6);
  const double steady = simsys::caching::che_hit_ratio(10'000, popularity);

  const SimTime at{10min};
  REQUIRE(x.at("cache", at - 1ns).hit_ratio_factor == 1.0);  // before the flush
  REQUIRE(x.at("cache", 0s).hit_ratio_factor == 1.0);
  REQUIRE(x.at("cache", at).hit_ratio_factor == 0.0);
  require_only_hit_ratio_changes(x.at("cache", at));

  double previous = 0.0;
  for (const double after_s : {0.1, 0.5, 2.0, 5.0, 10.0, 12.0}) {
    CAPTURE(after_s);
    const auto now = at + SimTime{std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::duration<double>(after_s))};
    const auto c = x.at("cache", now);
    const double expected =
        simsys::test_support::warm_hit_ratio(popularity, 10'000, 1000.0 * after_s) / steady;
    REQUIRE(c.hit_ratio_factor > 0.0);
    REQUIRE(c.hit_ratio_factor < 1.0);
    REQUIRE_THAT(c.hit_ratio_factor, WithinRel(expected, 1e-9));
    REQUIRE(c.hit_ratio_factor > previous);
    previous = c.hit_ratio_factor;
    require_only_hit_ratio_changes(c);
  }
  for (const SimTime later : {SimTime{at + 13s}, SimTime{at + 1min}, SimTime{at + 30s}}) {
    CAPTURE(later.count());
    REQUIRE_THAT(x.at("cache", later).hit_ratio_factor, WithinRel(1.0, 1e-12));
    require_only_hit_ratio_changes(x.at("cache", later));
  }
  require_identity(x.at("u", at + 2s));  // the flush acts on the cache alone
  require_identity(x.at("origin", at + 2s));
}

// A fixed hit ratio has no curve: the cache is full after keys / rate seconds (1000 keys at
// 100/s: 10 s) and warms linearly, so the factor is t / 10 s: 0, 0.25 at 2.5 s, 0.5 at 5 s, 1 at
// 10 s and for ever after. The ratio 0.9 itself is the engine's to multiply in (the factor is
// relative to it), so it does not appear here.
TEST_CASE(
    "a cache_flush on a fixed-ratio cache gives a hit ratio factor rising linearly over keys "
    "divided by rate",
    "[engine]") {
  const ScheduledScenario x{flushed_cache(
      "100/s", "",
      "type: cache, strategy: read_through, backend: origin, keys: 1000, hit_ratio: {fixed: 0.9}")};
  const SimTime at{10min};
  REQUIRE(x.at("cache", at - 1ns).hit_ratio_factor == 1.0);
  REQUIRE(x.at("cache", at).hit_ratio_factor == 0.0);
  const std::pair<SimTime, double> points[] = {
      {SimTime{2500ms}, 0.25}, {SimTime{5s}, 0.5}, {SimTime{7500ms}, 0.75}};
  for (const auto& [after, expected] : points) {
    CAPTURE(after.count());
    REQUIRE_THAT(x.at("cache", at + after).hit_ratio_factor, WithinRel(expected, 1e-12));
    require_only_hit_ratio_changes(x.at("cache", at + after));
  }
  REQUIRE_THAT(x.at("cache", at + 10s).hit_ratio_factor, WithinRel(1.0, 1e-12));
  REQUIRE_THAT(x.at("cache", at + 1min).hit_ratio_factor, WithinRel(1.0, 1e-12));
}

// dc_down eu: every node whose `dc` is eu is in a node_down (capacity factor 0) from 10 s for
// 10 s, as an interval [at, at + for); the nodes of us never see it. The rest of the condition is
// the identity: a datacenter going down changes capacity and nothing else on the node.
TEST_CASE(
    "a dc_down gives capacity factor 0 to every node of the datacenter and leaves the others "
    "alone",
    "[engine]") {
  const ScheduledScenario x{
      "version: 1\nname: dc\nduration: 1m\nstep: 100ms\n"
      "datacenters:\n  us: {}\n  eu: {}\n"
      "nodes:\n"
      "  c: {type: client, dc: us, rate: {base: 100/s}, ops: {op: 1.0}}\n"
      "  a: {type: service, dc: us, capacity: 1k/s, latency: 1ms}\n"
      "  x: {type: service, dc: eu, capacity: 1k/s, latency: 1ms, replicas: 2}\n"
      "  y: {type: service, dc: eu, capacity: 1k/s, latency: 1ms}\n"
      "links:\n"
      "  - {from: c, to: a, dir: inout, latency: 0s}\n"
      "  - {from: c, to: x, dir: inout, latency: 0s}\n"
      "  - {from: c, to: y, dir: inout, latency: 0s}\n"
      "faults:\n  - {at: 10s, kind: dc_down, datacenter: eu, for: 10s}\n"};
  for (const auto* eu_node : {"x", "y"}) {  // x has two replicas: all of them go down, not one
    CAPTURE(eu_node);
    require_identity(x.at(eu_node, SimTime{10s} - 1ns));
    for (const SimTime now : {SimTime{10s}, SimTime{15s}, SimTime{20s} - 1ns}) {
      CAPTURE(now.count());
      const auto c = x.at(eu_node, now);
      REQUIRE(c.capacity_factor == 0.0);
      REQUIRE(c.extra_latency == SimTime{0});
      REQUIRE(c.rate_factor == 1.0);
      REQUIRE(c.hit_ratio_factor == 1.0);
    }
    require_identity(x.at(eu_node, 20s));
  }
  for (const auto* us_node : {"c", "a"}) {
    CAPTURE(us_node);
    for (const SimTime now : {SimTime{0s}, SimTime{10s}, SimTime{15s}, SimTime{20s}}) {
      CAPTURE(now.count());
      require_identity(x.at(us_node, now));
    }
  }
}
