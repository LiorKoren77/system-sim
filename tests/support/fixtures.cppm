// Module `simsys.test_fixtures`: the scenarios, numbers and helpers more than one test file
// needs. The theory tests (tests/theory/test_*.cpp) share the M/M/1 probe, the pipe and the
// message-queue pictures and the "fresh simulation" helpers; test_scenarios shares the ring
// imbalance formula with test_core; test_report and test_series share the piped scenario and
// `explain` of a scenario built in code. A helper one file alone uses stays in that file, next to
// the tests that read it.
//
// C++20 named module, interface unit (src/core/core.cppm has the longer note on modules versus
// headers). A module instead of a header: each builder is compiled once, in fixtures.cpp, rather
// than once per test file that includes it, and an importer sees only what is exported here.
// Only declarations live here; the bodies, which need Catch2's macros (REQUIRE, FAIL) and
// <chrono>'s literals, are in fixtures.cpp.
module;
// NO <chrono>/<format>/<print>/<filesystem> here: this interface imports other modules (GCC
// 14.2 'Bad file data' bug, CLAUDE.md §3). Catch2 and <chrono> go in fixtures.cpp.
#include <optional>
#include <string>
#include <vector>

// C++20 `export module`: this unit is the interface of the named module simsys.test_fixtures;
// what it marks `export` is visible to every unit that imports the module.
export module simsys.test_fixtures;

// C++20 `import`: the compiled interfaces of the modules whose types the declarations below
// name, read without re-parsing any header text.
import simsys.core;
import simsys.engine;
import simsys.graph;
import simsys.model;

export namespace simsys::test_support {

// ---- The M/M/1 probe ----------------------------------------------------------------------------

// One client, one M/M/1 server: 1000/s capacity, 1 ms service, zero-latency link. 10 s at the
// default step.
model::Scenario mm1(double demand);

// ---- Fresh simulations --------------------------------------------------------------------------

// The first step of a fresh simulation of `g`, failing the test with the engine's message when it
// refuses the scenario (instead of dereferencing the refusal).
engine::StepRecord first_step(const graph::Graph& g);

// The first `steps` steps of a fresh simulation of `g`, one record each.
std::vector<engine::StepRecord> records_of(const graph::Graph& g, int steps);

// A fresh simulation of `g`, failing the test with the engine's message when it refuses the
// scenario (instead of dereferencing the refusal).
engine::Simulation simulation_of(const graph::Graph& g);

// ---- A link's bandwidth as a pipe (test_bytes_pipes.cpp has the picture and its numbers) --------

inline constexpr ByteRate one_gbit{125e6};    // 1 Gbit/s = 1e9 / 8 bytes per second
inline constexpr double pipe_mb_per_s = 125;  // 1 Gbit/s in megabytes per second

// a(`rate`/s of `get`, each `size` long: a read at b) -> b (1M/s, 1 ms) over a 250 us link with the
// given bandwidth, in the given direction. Node indices: a 0, b 1. Steps of 100 ms (the default).
model::Scenario pipe_over(double rate, Bytes size, std::optional<ByteRate> bandwidth = one_gbit,
                          model::Direction dir = model::Direction::out);

// The one pipe's sample of a step: the graphs here have exactly one link with a bandwidth.
const engine::LinkSample& only_link(const engine::StepRecord& r);

inline constexpr ByteRate one_gbit_per_s{125e6};  // piped's default link

// a(`rate`/s of `put`, 1 MB each) -> b (a service of 1M/s) over a 250 us link of the given
// bandwidth (1 Gbit/s = 125 MB/s unless given; none: a plain link). 3 s at 100 ms steps, the first
// 500 ms warmup.
model::Scenario piped(double rate, std::optional<ByteRate> bandwidth = one_gbit_per_s);

// ---- Message queues and worker pools (test_queues_pools.cpp has the picture and its numbers) ----

// A pool `id` of `workers` workers at `cost` a job, draining queue `q`.
model::NodeSpec group_of(std::string id, int workers, SimTime cost);

// u (`rate`/s of `publish`, 250 us link) -> q (a message queue, `retention` as given, the type's
// default when none) -> every group in `groups` over zero-latency links, so that everything a
// step sends arrives in the same step. Node indices: u 0, q 1, then the groups in order.
model::Scenario published(double rate, std::vector<model::NodeSpec> groups,
                          std::optional<SimTime> retention = std::nullopt);

// ---- A consistent-hash ring's imbalance ---------------------------------------------------------

// A consistent-hash ring's imbalance k, the hottest machine's load over the average: each of M
// machines holds the keys of its v virtual nodes' arcs, N copies of each key (N = 1 behind a
// balancer), so its share is a sum of N v arc lengths whose relative spread is about 1/sqrt(N v)
// (book ch. 5: about 10 % at 100 virtual nodes). The hottest of M such shares sits z_M standard
// deviations above the mean, z_M the expected maximum of M standard normals (Blom:
// queueing::expected_normal_maximum):
//   k = min(1 + z_M / sqrt(N v), M / N)        (M / N: a machine holds at most every key once)
// v = 100, M = 4: 1 + 1.0491 / 10 = 1.1049;  v = 150, M = 4: 1.0857;  v = 1, M = 4: 2.0491.
double ring_factor(int machines, int copies_per_key, int vnodes);

// ---- explain ------------------------------------------------------------------------------------

// `simsys explain`'s text for a scenario built in code; the test fails here, with the graph's
// message, when the scenario does not build.
std::string explain_of(const model::Scenario& s);

}  // namespace simsys::test_support
