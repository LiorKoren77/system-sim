// Implementation unit of `simsys.test_fixtures`: the bodies of the shared test builders. Each
// helper's description is on its declaration in fixtures.cppm. The end of the file works around
// two GCC 14.2 release-link defects (see `detail` there); tests/CMakeLists.txt compiles this file
// with -fno-inline for them, as the top-level CMakeLists.txt does src/model/model.cpp.
module;
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// C++20 module implementation unit: `module simsys.test_fixtures;` (no `export`) makes this file
// part of the named module. It sees every declaration of fixtures.cppm, exports nothing, and can
// include Catch2 and <chrono> in its own global module fragment, like a .cpp behind a header.
module simsys.test_fixtures;

// C++20 `import`: the compiled interfaces of the modules this unit reads from. The interface's
// own imports would be visible anyway; listing them says what is used here.
import simsys.core;
import simsys.engine;
import simsys.graph;
import simsys.model;
import simsys.queueing;
import simsys.report;

using namespace std::chrono_literals;
using simsys::model::Direction;
using simsys::model::LinkSpec;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
using simsys::model::Workload;

namespace simsys::test_support {

Scenario mm1(double demand) {
  // C++20 designated initializers: `.name = ...` names each member being set, in
  // declaration order, and leaves the rest at their defaults. Replaces positional braces
  // (which break silently when members are reordered) or a default object assigned field
  // by field; here a scenario reads like the YAML it stands for.
  Scenario s{.name = "mm1", .version = 1, .duration = 10s};
  s.nodes = {
      NodeSpec{.id = "c",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{demand}}, .op_shares = {{"op", 1.0}}}},
      NodeSpec{.id = "s", .type = NodeType::service, .capacity = Rate{1000}, .latency = 1ms}};
  s.links = {LinkSpec{.from = "c", .to = "s", .dir = Direction::inout, .latency = 0s}};
  return s;
}

engine::StepRecord first_step(const graph::Graph& g) {
  auto sim = engine::Simulation::create(g);
  if (!sim) FAIL(sim.error().message);
  return sim->step(0s, 100ms);
}

std::vector<engine::StepRecord> records_of(const graph::Graph& g, int steps) {
  auto sim = engine::Simulation::create(g);
  if (!sim) FAIL(sim.error().message);
  std::vector<engine::StepRecord> records;
  for (int i = 0; i < steps; ++i) records.push_back(sim->step(i * 100ms, 100ms));
  return records;
}

engine::Simulation simulation_of(const graph::Graph& g) {
  auto sim = engine::Simulation::create(g);
  if (!sim) FAIL(sim.error().message);
  return std::move(*sim);
}

Scenario pipe_over(double rate, Bytes size, std::optional<ByteRate> bandwidth, Direction dir) {
  Scenario s{.name = "pipe", .version = 1, .duration = 60s};
  s.nodes = {NodeSpec{.id = "a",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{rate}},
                                           .op_shares = {{"get", 1.0}},
                                           .sizes = {{"get", size}}}},
             NodeSpec{.id = "b", .type = NodeType::service, .capacity = Rate{1e6}, .latency = 1ms}};
  s.links = {
      LinkSpec{.from = "a", .to = "b", .dir = dir, .latency = 250us, .bandwidth = bandwidth}};
  return s;
}

const engine::LinkSample& only_link(const engine::StepRecord& r) {
  REQUIRE(r.links.size() == 1);
  return r.links.front();
}

Scenario piped(double rate, std::optional<ByteRate> bandwidth) {
  Scenario s{.name = "piped", .version = 1, .duration = 3s, .step = 100ms, .warmup = 500ms};
  s.nodes = {NodeSpec{.id = "a",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{rate}},
                                           .op_shares = {{"put", 1.0}},
                                           .sizes = {{"put", Bytes{1e6}}}}},
             NodeSpec{.id = "b", .type = NodeType::service, .capacity = Rate{1e6}, .latency = 1ms}};
  s.links = {LinkSpec{.from = "a", .to = "b", .latency = 250us, .bandwidth = bandwidth}};
  return s;
}

NodeSpec group_of(std::string id, int workers, SimTime cost) {
  NodeSpec pool{.id = std::move(id), .type = NodeType::worker_pool};
  pool.consumes = "q";
  pool.workers = workers;
  pool.job_cost = cost;
  return pool;
}

Scenario published(double rate, std::vector<NodeSpec> groups, std::optional<SimTime> retention) {
  Scenario s{.name = "published", .version = 1, .duration = 60s};
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload =
                          Workload{.curve = {.base = Rate{rate}}, .op_shares = {{"publish", 1.0}}}},
             NodeSpec{.id = "q", .type = NodeType::message_queue}};
  s.nodes[1].retention = retention;
  s.links = {LinkSpec{.from = "u", .to = "q", .latency = 250us}};
  for (auto& group : groups) {
    s.links.push_back(LinkSpec{.from = "q", .to = group.id, .latency = 0s});
    s.nodes.push_back(std::move(group));
  }
  return s;
}

double ring_factor(int machines, int copies_per_key, int vnodes) {
  // k = min(1 + z_M / sqrt(N v), M / N): the ring imbalance formula (see the declaration for the
  // model); spread = 1 / sqrt(N v) is the relative spread of one machine's share of the keys.
  const double spread = 1.0 / std::sqrt(static_cast<double>(copies_per_key) * vnodes);
  return std::min(1.0 + queueing::expected_normal_maximum(machines) * spread,
                  static_cast<double>(machines) / copies_per_key);
}

std::string explain_of(const Scenario& s) {
  const auto built = graph::Graph::build(s);
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  return report::render_explain(*built);
}

namespace detail {

// GCC 14.2, CLAUDE.md §3's fourth defect, in the release build: a unit that imports simsys.model
// calls compiler-generated special members of std templates over model types out of line without
// emitting them. src/model/model.cpp emits the ones the library's importers need; a test file can
// need more. test_scenarios.cpp did once its scenario helpers came from this module instead of a
// header: "undefined reference to std::vector<simsys::model::Step@simsys.model>::vector(...&&)"
// (verified 2026-10-29). As in model.cpp: each explicit instantiation below calls T's five special
// members, and -fno-inline keeps those calls out of line, so this object file defines them. A new
// undefined reference of this kind in a test executable gets one more line here.
// `dst` is raw storage for one T; `src` is a live T that is copied and moved from.
template <typename T>
void emit_special_members(T* dst, T& src) {
  // C++20 std::construct_at: placement-new as a (constexpr) library function, replacing
  // `::new (static_cast<void*>(dst)) T(src)`. Used here because the project bans raw `new`, and
  // because it names exactly the constructor call we want emitted.
  std::construct_at(dst, src);             // copy constructor
  *dst = src;                              // copy assignment
  *dst = std::move(src);                   // move assignment
  std::destroy_at(dst);                    // destructor
  std::construct_at(dst, std::move(src));  // move constructor
  std::destroy_at(dst);
}
// A NodeSpec's `then` and each of its `ops`' step lists: test_scenarios.cpp's release link.
template void emit_special_members(std::vector<model::Step>*, std::vector<model::Step>&);
// The index lists a scenario helper moves (std::vector<std::size_t>'s move constructor):
// scenario_tests' release link once graph.cppm's definitions lived in graph.cpp (phase 3a,
// Task 3) and this module was rebased onto them, verified 2026-10-30.
template void emit_special_members(std::vector<std::size_t>*, std::vector<std::size_t>&);

// The same defect for the exported `inline constexpr` variables of fixtures.cppm: a unit that binds
// a reference to one (piped's default argument does: std::optional<ByteRate>'s converting
// constructor takes `one_gbit_per_s` by reference) refers to the variable without emitting it,
// trusting the module's object files to define it, and the interface unit's object defines
// one_gbit but not one_gbit_per_s: unit_tests failed to link in Debug too ("undefined reference
// to simsys::test_support::one_gbit_per_s@simsys.test_fixtures", verified 2026-10-29). Each
// function below takes one constant's address, so this object file defines it. A new constant in
// fixtures.cppm that some test binds by reference gets one more line here.
const ByteRate* emit_one_gbit() {
  return &one_gbit;
}
const ByteRate* emit_one_gbit_per_s() {
  return &one_gbit_per_s;
}
const double* emit_pipe_mb_per_s() {
  return &pipe_mb_per_s;
}
}  // namespace detail

}  // namespace simsys::test_support
