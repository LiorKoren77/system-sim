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
import simsys.config;
import simsys.core;
import simsys.engine;
import simsys.flow;
import simsys.graph;
import simsys.model;

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
        schedule{s} {}
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
