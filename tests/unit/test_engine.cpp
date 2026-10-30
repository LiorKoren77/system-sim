// The link delay line: a ring buffer of flows, one slot per step of latency (spec §3.4), and the
// fault schedule: the condition (capacity, latency, rate) each node is in at a time (phase 3, spec
// §4.2).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
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
import simsys.flow;
import simsys.graph;
import simsys.model;
import simsys.test_fixtures;

using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;
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
//     (Task 7 made it FaultSchedule(const graph::Graph&), which the engine builds: the helpers
//     below take it from Simulation::fault_schedule_of(graph))
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
  REQUIRE(c.primary_factor == 1.0);
  REQUIRE(c.replica_factor == 1.0);
  REQUIRE(c.machines_down == 0);
  REQUIRE(c.back_in_s == 0.0);
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
//   Condition::hit_ratio_factor is h(n) / h, the warm hit ratio over the steady one, n the reads
//   the cache was handed since the flush (FaultSchedule::count_reads, every step: Task 7's review
//   replaced a read rate frozen at the flush by the reads the cache actually gets; at a constant
//   rate n = rate x t, the curve of before):
//     che cache   warm_hit_ratio(popularity, keys, n) / che_hit_ratio(keys, popularity)
//     fixed ratio min(1, n / keys)                              (linear over the fill)
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

  // The schedule as the engine leaves it at `now` after handing it, at every 100 ms step from
  // t = 0 that ends by `now`, the reads cache `id` (all its copies) received in that step:
  // reads_per_s(t) at the step that starts at t (Simulation::count_flushed_reads does this after
  // each step's forward pass, from the cache's arrivals). A copy: the scenario's schedule stays
  // unfed, so each call starts from the flush.
  FaultSchedule fed_schedule(std::string_view id, SimTime now,
                             const std::function<double(SimTime)>& reads_per_s) const {
    FaultSchedule fed = schedule;
    for (SimTime t{0}; t + SimTime{100ms} <= now; t += SimTime{100ms}) {
      fed.count_reads(node(id), t, SimTime{100ms}, reads_per_s(t));
    }
    return fed;
  }
  // The condition of cache `id` at `now`, fed `reads_per_s` reads a second at every step before.
  Condition fed(std::string_view id, SimTime now, double reads_per_s) const {
    return fed_schedule(id, now, [=](SimTime) { return reads_per_s; }).condition_at(node(id), now);
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
    const auto c = x.fed("cache", now, 1000.0);
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
    REQUIRE_THAT(x.fed("cache", later, 1000.0).hit_ratio_factor, WithinRel(1.0, 1e-12));
    require_only_hit_ratio_changes(x.fed("cache", later, 1000.0));
  }
  require_identity(x.at("u", at + 2s));  // the flush acts on the cache alone
  require_identity(x.at("origin", at + 2s));
}

// A fixed hit ratio has no curve: the cache is full after `keys` reads, keys / rate seconds at a
// constant rate (1000 keys at 100/s: 10 s), and warms linearly, so the factor is n / 1000 =
// t / 10 s: 0, 0.25 at 2.5 s, 0.5 at 5 s, 1 at 10 s and for ever after. The ratio 0.9 itself is the
// engine's to multiply in (the factor is relative to it), so it does not appear here.
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
    REQUIRE_THAT(x.fed("cache", at + after, 100.0).hit_ratio_factor, WithinRel(expected, 1e-12));
    require_only_hit_ratio_changes(x.fed("cache", at + after, 100.0));
  }
  REQUIRE_THAT(x.fed("cache", at + 10s, 100.0).hit_ratio_factor, WithinRel(1.0, 1e-12));
  REQUIRE_THAT(x.fed("cache", at + 1min, 100.0).hit_ratio_factor, WithinRel(1.0, 1e-12));
}

// ---- The warm-up counts the reads the cache gets (Task 7's review, findings 1 and 2) ------------
//
// The schedule used to turn one number, the read rate at the flush, into "reads seen" for the
// whole warm-up: a cache flushed while nobody read it stayed cold for ever, and one flushed on a
// rising curve warmed at the rate of the flush. Now the engine hands it, every step, the reads the
// cache received (FaultSchedule::count_reads), and n is their sum since the flush.

// Nobody reads the cache for five minutes after the flush: it has seen nothing, so it is still
// empty (factor 0), and it has no warm-up time yet.
TEST_CASE("a flushed cache that is handed no reads stays cold and has no warm-up time",
          "[engine]") {
  const ScheduledScenario x{flushed_cache(
      "1k/s", ", keys: {zipf: 0.8, space: 1M}",
      "type: cache, strategy: read_through, backend: origin, keys: 10k, hit_ratio: che")};
  const SimTime at{10min};
  const SimTime later = at + SimTime{5min};
  const auto idle = x.fed_schedule("cache", later, [](SimTime) { return 0.0; });
  REQUIRE(idle.condition_at(x.node("cache"), later).hit_ratio_factor == 0.0);
  REQUIRE_FALSE(idle.warm_up_s(x.node("cache"), at).has_value());
}

// No reads for the first 10 s after the flush, then 1k/s: at 12 s the cache has seen 2000 reads,
// not 12,000, so its factor is warm_hit_ratio(2000) / h = 0.547 (the first test's 2 s point); it
// is full when the reads since the flush reach t_c = 12,106, 10 s + 12.106 s after it. The time
// falls inside a step (between 22.1 and 22.2 s): the schedule finds it within the step, where the
// sum crosses t_c, so a constant rate gives t_c / rate exactly.
TEST_CASE(
    "the warm-up follows the reads handed since the flush: a cache idle for 10 s is full "
    "10 s later",
    "[engine]") {
  const ScheduledScenario x{flushed_cache(
      "1k/s", ", keys: {zipf: 0.8, space: 1M}",
      "type: cache, strategy: read_through, backend: origin, keys: 10k, hit_ratio: che")};
  const auto popularity = simsys::caching::Popularity::zipf(0.8, 1e6);
  const double steady = simsys::caching::che_hit_ratio(10'000, popularity);
  const double t_c = simsys::caching::che_characteristic_time(10'000, popularity);
  const SimTime at{10min};
  const auto idle_then_busy = [at](SimTime t) { return t < at + SimTime{10s} ? 0.0 : 1000.0; };

  const auto at_12s = x.fed_schedule("cache", at + SimTime{12s}, idle_then_busy);
  REQUIRE_THAT(
      at_12s.condition_at(x.node("cache"), at + SimTime{12s}).hit_ratio_factor,
      WithinRel(simsys::test_support::warm_hit_ratio(popularity, 10'000, 2000) / steady, 1e-9));
  REQUIRE_FALSE(at_12s.warm_up_s(x.node("cache"), at).has_value());  // not full yet

  const auto at_30s = x.fed_schedule("cache", at + SimTime{30s}, idle_then_busy);
  REQUIRE(at_30s.condition_at(x.node("cache"), at + SimTime{30s}).hit_ratio_factor == 1.0);
  REQUIRE(at_30s.warm_up_s(x.node("cache"), at).has_value());
  REQUIRE_THAT(*at_30s.warm_up_s(x.node("cache"), at), WithinRel(10.0 + t_c / 1000.0, 1e-9));

  const auto constant = x.fed_schedule("cache", at + SimTime{30s}, [](SimTime) { return 1000.0; });
  REQUIRE_THAT(*constant.warm_up_s(x.node("cache"), at), WithinRel(t_c / 1000.0, 1e-9));
}

// dc_down eu: every node whose `dc` is eu is in a node_down (capacity factor 0) from 10 s for
// 10 s, as an interval [at, at + for); the nodes of us never see it. The rest of the condition is
// the identity, but for the time until the node is back (back_in_s, 20 s − now): a datacenter going
// down changes capacity and nothing else on the node.
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
      // an outage: the node is back at 20 s (finding 1 of Task 6's review)
      REQUIRE_THAT(c.back_in_s, WithinRel(simsys::seconds(SimTime{20s} - now), 1e-12));
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

// ---- When a down node is back (phase 3 Task 6's review, finding 1) ------------------------------
//
// A node at capacity 0 still admits requests to its room, and they are served once it is back: a
// request admitted at `now` waits until the zero-capacity faults on the node end. The condition
// carries that time, back_in_s = (at + for) − now, for a fault that takes all of a node's capacity
// (a node_down without `replicas`, every copy of a dc_down; on a sql_db a `primary` or every read
// replica down). Faults that overlap or touch chain into one outage: the latest end of the chain.
// A fault that leaves some capacity (1 of 4 replicas down, a capacity factor) is not an outage:
// back_in_s stays 0.
TEST_CASE("a node_down of every copy gives back_in_s, the seconds until the node is back",
          "[engine]") {
  const Scheduled x{
      "faults:\n"
      "  - {at: 10s, kind: node_down, node: t, for: 10s}\n"
      "  - {at: 10s, kind: node_down, node: s, replicas: 1, for: 10s}\n"};
  REQUIRE(x.at("t", 10s).back_in_s == 10.0);
  REQUIRE(x.at("t", 12s).back_in_s == 8.0);
  REQUIRE_THAT(x.at("t", 19500ms).back_in_s, WithinRel(0.5, 1e-12));
  REQUIRE(x.at("t", 12s).capacity_factor == 0.0);
  require_identity(x.at("t", 20s));
  REQUIRE(x.at("s", 12s).back_in_s == 0.0);  // 3 of 4 replicas still serve: no outage
  REQUIRE_THAT(x.at("s", 12s).capacity_factor, WithinRel(0.75, 1e-12));
}

// t down over [10 s, 20 s) and again over [15 s, 30 s): one outage from 10 s to 30 s, so at 12 s
// (only the first is on yet) the node is back in 30 − 12 = 18 s, at 17 s in 13 s, at 25 s in 5 s.
// A capacity fault over [10 s, 40 s) leaves capacity (0.5): it does not stretch the outage. u down
// over [10 s, 20 s) and [25 s, 30 s): a gap between them, so at 12 s it is back in 8 s.
TEST_CASE("overlapping zero-capacity faults give back_in_s up to the latest end of the chain",
          "[engine]") {
  const Scheduled x{
      "faults:\n"
      "  - {at: 10s, kind: node_down, node: t, for: 10s}\n"
      "  - {at: 15s, kind: node_down, node: t, for: 15s}\n"
      "  - {at: 10s, kind: capacity, node: t, factor: 0.5, for: 30s}\n"
      "  - {at: 10s, kind: node_down, node: u, for: 10s}\n"
      "  - {at: 25s, kind: node_down, node: u, for: 5s}\n"};
  REQUIRE(x.at("t", 12s).back_in_s == 18.0);
  REQUIRE(x.at("t", 17s).back_in_s == 13.0);
  REQUIRE(x.at("t", 25s).back_in_s == 5.0);
  REQUIRE(x.at("t", 35s).back_in_s == 0.0);  // only the capacity fault is left
  REQUIRE(x.at("t", 35s).capacity_factor == 0.5);
  REQUIRE(x.at("u", 12s).back_in_s == 8.0);
  REQUIRE(x.at("u", 26s).back_in_s == 4.0);
}

// ---- A sql_db's primary and its read replicas (finding 2) ---------------------------------------
//
// Book ch. 1, "database replication": the primary takes every write, the read replicas every read.
// A node_down on a sql_db of N = `replicas` copies per shard therefore says which of them:
//   replicas: primary   the primaries' capacity x 0: writes stop (until a replica is promoted,
//   which
//                       the model leaves out: the primary is back at `at + for`), reads go on
//   replicas: r < N     the read replicas' capacity x (N − 1 − r) / (N − 1), the primary untouched
//   replicas: N         every copy: the whole node x 0, like a node_down without `replicas`
// On a sql_db of one copy the primary is the whole node, so `primary` and `replicas: 1` are both
// capacity x 0.
namespace {
const ScheduledScenario& databases() {
  static const ScheduledScenario x{
      "version: 1\nname: dbs\nduration: 1m\nstep: 100ms\n"
      "nodes:\n"
      "  c: {type: client, rate: {base: 100/s}, ops: {read: 0.8, write: 0.2}}\n"
      "  db3: {type: sql_db, capacity: 10k/s, latency: 1ms, replicas: 3}\n"
      "  db1: {type: sql_db, capacity: 10k/s, latency: 1ms}\n"
      "links:\n"
      "  - {from: c, to: db3, dir: inout, latency: 0s}\n"
      "  - {from: c, to: db1, dir: inout, latency: 0s}\n"
      "faults:\n"
      "  - {at: 10s, kind: node_down, node: db3, replicas: primary, for: 5s}\n"
      "  - {at: 20s, kind: node_down, node: db3, replicas: 1, for: 5s}\n"
      "  - {at: 30s, kind: node_down, node: db3, replicas: 2, for: 5s}\n"
      "  - {at: 40s, kind: node_down, node: db3, replicas: 3, for: 5s}\n"
      "  - {at: 10s, kind: node_down, node: db1, replicas: primary, for: 5s}\n"
      "  - {at: 20s, kind: node_down, node: db1, replicas: 1, for: 5s}\n"};
  return x;
}
}  // namespace

TEST_CASE("replicas primary on a sql_db stops its primaries and leaves its read replicas",
          "[engine]") {
  const auto c = databases().at("db3", 12s);
  REQUIRE(c.primary_factor == 0.0);
  REQUIRE(c.replica_factor == 1.0);
  REQUIRE(c.capacity_factor == 1.0);
  REQUIRE(c.back_in_s == 3.0);  // the primaries are an outage: back at 15 s
  require_identity(databases().at("db3", 15s));
}

TEST_CASE("replicas r on a sql_db of N copies leaves its read replicas (N - 1 - r) over (N - 1)",
          "[engine]") {
  const auto one = databases().at("db3", 22s);  // 1 of the 2 read replicas down
  REQUIRE_THAT(one.replica_factor, WithinRel(0.5, 1e-12));
  REQUIRE(one.primary_factor == 1.0);
  REQUIRE(one.capacity_factor == 1.0);
  REQUIRE(one.back_in_s == 0.0);                // one read replica still serves the reads
  const auto two = databases().at("db3", 32s);  // both read replicas: reads stop, writes go on
  REQUIRE(two.replica_factor == 0.0);
  REQUIRE(two.primary_factor == 1.0);
  REQUIRE(two.back_in_s == 3.0);
  const auto all = databases().at("db3", 42s);  // every copy: the whole node
  REQUIRE(all.capacity_factor == 0.0);
  REQUIRE(all.back_in_s == 3.0);
}

TEST_CASE("on a sql_db of one copy replicas primary and replicas 1 are both the whole node",
          "[engine]") {
  for (const SimTime now : {SimTime{12s}, SimTime{22s}}) {
    CAPTURE(now.count());
    const auto c = databases().at("db1", now);
    REQUIRE(c.capacity_factor == 0.0);
    REQUIRE(c.primary_factor == 1.0);
    REQUIRE(c.replica_factor == 1.0);
    REQUIRE(c.back_in_s == 3.0);
  }
}

// ---- A kv_cluster's machines (finding 5) --------------------------------------------------------
//
// A node_down of r of a kv_cluster's M machines counts the machines down (Condition::machines_down)
// instead of scaling every machine's capacity: the survivors keep their own keys and load (strict
// quorum: no machine takes over a dead one's replicas, ch. 6), and the requests whose replica set
// held a dead machine meet their quorum among fewer replicas (behaviours::KvClusterBehaviour). All
// M down is the whole node, capacity x 0. Overlapping faults add their machines.
TEST_CASE("a node_down of r of a kv_cluster's machines counts r machines down and keeps capacity",
          "[engine]") {
  const ScheduledScenario x{
      "version: 1\nname: kv\nduration: 1m\nstep: 100ms\n"
      "nodes:\n"
      "  c: {type: client, rate: {base: 100/s}, ops: {read: 1.0}}\n"
      "  store: {type: kv_cluster, nodes: 6, capacity: 10k/s, latency: 1ms}\n"
      "links:\n  - {from: c, to: store, dir: inout, latency: 0s}\n"
      "faults:\n"
      "  - {at: 10s, kind: node_down, node: store, replicas: 1, for: 10s}\n"
      "  - {at: 15s, kind: node_down, node: store, replicas: 1, for: 10s}\n"
      "  - {at: 30s, kind: node_down, node: store, replicas: 6, for: 5s}\n"};
  const auto one = x.at("store", 12s);
  REQUIRE(one.machines_down == 1);
  REQUIRE(one.capacity_factor == 1.0);
  REQUIRE(one.back_in_s == 0.0);
  REQUIRE(x.at("store", 17s).machines_down == 2);
  REQUIRE(x.at("store", 22s).machines_down == 1);
  const auto all = x.at("store", 32s);
  REQUIRE(all.capacity_factor == 0.0);
  REQUIRE(all.machines_down == 0);
  REQUIRE(all.back_in_s == 3.0);
  require_identity(x.at("store", 25s));
}

// ---- FaultRecord: what the engine keeps per fault, for the report's block
// ------------------------
//
// What the tests below assume of the engine's API (Task 8 of the phase 3 plan and its review):
//   struct FaultAfter {                       // what followed the fault; each entry empty when it
//     std::string backlog_node;               // does not apply or never happened in the run:
//     std::optional<double> backlog_drained_in_s;  // whose backlog, and seconds from the end of
//     std::optional<double> cache_warm_in_s;       // the fault until it is the twin's again
//   };
//   struct FaultRecord {                      // a value type, members in this order (designated
//     model::FaultKind kind;                  // initializers need it)
//     std::string node;                       // as the fault names it (empty for a dc_down)
//     SimTime at, duration;                   // duration 0 for a cache_flush
//     double offered_per_s, served_per_s;     // the faulted node's demand and drain, means over
//                                             // [at, at + for) (over the steps seen so far)
//     double lost; std::string lost_unit;     // its drops minus the twin's, from `at` to the
//     double lost_on_cut_links;               // recovery, in its unit; a dc_down's cut links'
//     double backlog_peak, room;              // the peak backlog, the room (queue_limit) it fills
//     double extra_misses; std::string origin, origin_unit;  // a cache_flush's: vs the twin
//     std::optional<double> flush_window_s;   // a cache_flush's "during", once the run finds it
//     FaultAfter after;
//     std::optional<SimTime> recovered_at;    // start of the first step at or after the end of
//     std::string datacenter;                 // the fault with p99 and rho within 1 % of the
//   };                                        // twin's same step
//   std::span<const FaultRecord> Simulation::fault_records() const   // in `at` order, one per
//                                                                    // fault
//   const StepRecord* Simulation::twin_record() const  // the fault-free twin's latest step; null
//                                                      // without faults
// A fault that has not started when the records are read has a record of zeros; the tests read
// only faults that have. Which SLOs a fault flipped is the report's (report::FaultBlock).
namespace {
// The M/M/1 probe of tests/theory/test_faults.cpp (client c 400/s -> service s 1k/s, 1 ms, room
// 100, zero-latency link; 30 s at 100 ms), with the SLOs and faults the caller gives.
Scenario fault_probe(std::string_view slos_and_faults) {
  return load_yaml(
      "version: 1\nname: records\nduration: 30s\nstep: 100ms\n"
      "nodes:\n"
      "  c: {type: client, rate: {base: 400/s}, ops: {op: 1.0}}\n"
      "  s: {type: service, capacity: 1k/s, latency: 1ms, advanced: {queue_limit: 100}}\n"
      "links:\n  - {from: c, to: s, dir: inout, latency: 0s}\n" +
      std::string{slos_and_faults});
}

// The records after `steps` steps (100 ms each) of the scenario.
std::vector<simsys::engine::FaultRecord> records_after(const Scenario& s, int steps) {
  const auto g = simsys::graph::Graph::build(s);
  if (!g) FAIL(g.error().message);
  auto sim = simsys::test_support::simulation_of(*g);
  for (int i = 0; i < steps; ++i) (void)sim.step(SimTime{100ms} * i, SimTime{100ms});
  const auto records = sim.fault_records();
  return {records.begin(), records.end()};
}
}  // namespace

// The probe's ten second node_down at 10 s, whole run (every number derived in test_faults.cpp):
// 400/s offered and 0/s served over the 10 s, 3900 requests lost (the room of 100 kept 100 of the
// 4000; the twin lost none), a backlog peaking at the room, the backlog drained 0.2 s after the
// fault, the system recovered at 20.2 s. (The SLOs are the report's business: the engine keeps no
// verdict.)
TEST_CASE("the engine keeps one record per fault: lost 3900, backlog peak 100, recovered at 20.2",
          "[engine]") {
  const auto records =
      records_after(fault_probe("slo:\n"
                                "  - {metric: p99_latency, node: c, op: op, max: 100ms}\n"
                                "  - {metric: p99_latency, node: c, op: op, max: 1ms}\n"
                                "  - {metric: p99_latency, node: c, op: op, max: 1h}\n"
                                "faults:\n  - {at: 10s, kind: node_down, node: s, for: 10s}\n"),
                    300);
  REQUIRE(records.size() == 1);
  const auto& r = records[0];
  REQUIRE(r.kind == simsys::model::FaultKind::node_down);
  REQUIRE(r.node == "s");
  REQUIRE(r.at == SimTime{10s});
  REQUIRE(r.duration == SimTime{10s});
  REQUIRE_THAT(r.offered_per_s, WithinRel(400.0, 1e-9));
  REQUIRE_THAT(r.served_per_s, WithinAbs(0.0, 1e-9));
  REQUIRE_THAT(r.lost, WithinRel(3900.0, 1e-9));
  REQUIRE(r.lost_unit == "requests");
  REQUIRE(r.lost_on_cut_links == 0.0);
  REQUIRE_THAT(r.backlog_peak, WithinRel(100.0, 1e-9));
  REQUIRE_THAT(r.room, WithinRel(100.0, 1e-9));
  REQUIRE(r.after.backlog_node == "s");
  REQUIRE(r.after.backlog_drained_in_s.has_value());
  REQUIRE_THAT(*r.after.backlog_drained_in_s, WithinAbs(0.2, 1e-9));
  REQUIRE_FALSE(r.after.cache_warm_in_s.has_value());
  REQUIRE(r.recovered_at.has_value());
  REQUIRE(*r.recovered_at == SimTime{20200ms});
}

// Read while the fault is still on (15 s of the run seen, 5 s of the 10 s outage: steps 100 to
// 149): 5 s x 400 offered, 100 kept, 1900 lost so far; nothing has cleared, so there is no drain
// time and no recovery yet.
TEST_CASE("a fault still on when the records are read has no drain time and no recovery yet",
          "[engine]") {
  const auto records = records_after(
      fault_probe("faults:\n  - {at: 10s, kind: node_down, node: s, for: 10s}\n"), 150);
  REQUIRE(records.size() == 1);
  REQUIRE_THAT(records[0].lost, WithinRel(1900.0, 1e-9));
  REQUIRE_THAT(records[0].backlog_peak, WithinRel(100.0, 1e-9));
  REQUIRE_FALSE(records[0].after.backlog_drained_in_s.has_value());
  REQUIRE_FALSE(records[0].recovered_at.has_value());
}

// Without faults there is nothing to keep.
TEST_CASE("a simulation without faults keeps no fault records", "[engine]") {
  REQUIRE(records_after(fault_probe(""), 50).empty());
}

// Two faults give two records in at order, whatever the order they were declared in: the plan's
// "two faults, two blocks in `at` order" starts here.
TEST_CASE("fault records come in at order, whatever the order the faults were declared in",
          "[engine]") {
  const auto records =
      records_after(fault_probe("faults:\n"
                                "  - {at: 20s, kind: latency_spike, node: s, extra: 1ms, for: 5s}\n"
                                "  - {at: 10s, kind: node_down, node: s, for: 5s}\n"),
                    300);
  REQUIRE(records.size() == 2);
  REQUIRE(records[0].kind == simsys::model::FaultKind::node_down);
  REQUIRE(records[0].at == SimTime{10s});
  REQUIRE(records[1].kind == simsys::model::FaultKind::latency_spike);
  REQUIRE(records[1].at == SimTime{20s});
  // The spike loses nothing and queues nothing: rho 0.8 against 400/s offered.
  REQUIRE(records[1].lost == 0.0);
  REQUIRE(records[1].backlog_peak == 0.0);
  REQUIRE_THAT(records[1].served_per_s, WithinRel(400.0, 1e-9));
  REQUIRE(records[1].after.backlog_node.empty());  // no backlog: nothing to drain
  // Back at the instant it ends: the first step after it is the twin's same step.
  REQUIRE(records[1].recovered_at.has_value());
  REQUIRE(*records[1].recovered_at == SimTime{25s});
}

// The baseline of every record (FaultRecord's header: the counterfactual) is a twin: the same
// scenario with its faults stripped, stepped with the run. Its record of each step is the record
// of a run of the scenario without the `faults:` key, bit for bit (the same seeds, so the same
// noise), and a scenario without faults has none (no twin: the run costs what it did).
TEST_CASE("a simulation with faults steps a fault-free twin whose records are the run without them",
          "[engine]") {
  const auto with_faults = simsys::graph::Graph::build(
      fault_probe("faults:\n  - {at: 1s, kind: node_down, node: s, for: 1s}\n"));
  const auto without = simsys::graph::Graph::build(fault_probe(""));
  REQUIRE(with_faults.has_value());
  REQUIRE(without.has_value());
  auto faulted = simsys::test_support::simulation_of(*with_faults);
  auto plain = simsys::test_support::simulation_of(*without);
  REQUIRE(plain.twin_record() == nullptr);
  for (int i = 0; i < 30; ++i) {
    CAPTURE(i);
    const SimTime now = SimTime{100ms} * i;
    const auto run = faulted.step(now, SimTime{100ms});
    const auto expected = plain.step(now, SimTime{100ms});
    REQUIRE(plain.twin_record() == nullptr);
    const auto* twin = faulted.twin_record();
    REQUIRE(twin != nullptr);
    REQUIRE(twin->t == expected.t);
    REQUIRE(twin->nodes.at(1).drain_per_s == expected.nodes.at(1).drain_per_s);
    REQUIRE(twin->nodes.at(1).backlog == expected.nodes.at(1).backlog);
    REQUIRE(twin->responses.at(0).response.mean_s == expected.responses.at(0).response.mean_s);
    if (i >= 10 && i < 20) REQUIRE(run.nodes.at(1).drain_per_s == 0.0);  // the run is down
  }
}

// Finding 2 of the review: a fault at the run's first step had no step before it and was never
// judged recovered. Against the twin it is: down from 0 to 5 s, the room's 100 drain in two steps,
// back at 5.2 s.
TEST_CASE("a fault at the run's first step is judged recovered against the twin", "[engine]") {
  const auto records =
      records_after(fault_probe("faults:\n  - {at: 0s, kind: node_down, node: s, for: 5s}\n"), 100);
  REQUIRE(records.size() == 1);
  REQUIRE(records[0].recovered_at.has_value());
  REQUIRE(*records[0].recovered_at == SimTime{5200ms});
}
