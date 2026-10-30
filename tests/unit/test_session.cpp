// simsys.session: what `simsys run --replications N` is built on, one Simulation per seed on the
// sweep's worker pool. Properties pinned here: how many summaries come back, which seed each one
// ran, that the workers cannot show in them, and that N = 1 is the plain run.
//
// Names and signatures assumed (the plan's text, made concrete; the implementer may change them
// and these tests with them if the reason is stated):
//   std::vector<report::Summary> session::run_replications(const graph::Graph& g, int seed,
//                                                          std::size_t count, std::size_t jobs);
// Replication k (from 0) runs the scenario of `g` with seed `seed + k`; the `seed` argument, not
// the scenario's own seed, starts the sequence. A Summary names the graph it summarises
// (Summary::graph()), so the returned summaries must stay valid while `g` lives.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

// C++20 `import`: brings a named module's exported names into this translation unit without
// textual inclusion (the compiler reads the module's compiled interface). It replaces
// `#include` plus include guards; placed after the #includes because GCC 14 reports
// redefinitions when a std header is included after a module that included it itself.
import simsys.core;
import simsys.graph;
import simsys.model;
import simsys.report;
import simsys.session;
import simsys.workload;

using namespace std::chrono_literals;
using simsys::Rate;
using simsys::graph::Graph;
using simsys::model::LinkSpec;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
using simsys::model::Workload;
using simsys::report::Summary;

namespace {

// One client at 300/s with seeded noise (sigma 0.3, bursts of about 2 s) into one service of
// 1000/s: an M/M/1 queue at about 30 % utilization whose arrival rate wanders with the seed, so a
// different seed is a different mean latency. 10 s at the default 100 ms step, 2 s of warmup.
Scenario noisy(int seed) {
  // C++20 designated initializers: the members named at the point of use, in declaration order,
  // replacing positional braces where a reader must remember which number is which.
  Scenario s{.name = "noisy", .version = 1, .seed = seed, .duration = 10s, .warmup = 2s};
  s.nodes = {
      NodeSpec{
          .id = "c",
          .type = NodeType::client,
          .workload = Workload{.curve = {.base = Rate{300}},
                               .noise = simsys::workload::Noise{.sigma = 0.3, .correlation = 2s},
                               .op_shares = {{"op", 1.0}}}},
      NodeSpec{.id = "s", .type = NodeType::service, .capacity = Rate{1000}, .latency = 1ms}};
  s.links = {LinkSpec{.from = "c", .to = "s", .dir = simsys::model::Direction::inout}};
  return s;
}

// The same without the noise: the curve exactly, whatever the seed.
Scenario noiseless(int seed) {
  auto s = noisy(seed);
  s.nodes[0].workload->noise.reset();
  return s;
}

Graph graph_of(const Scenario& s) {
  // C++23 std::expected<T, E>: the graph or the builder's refusal; `has_value()` tells which.
  auto g = Graph::build(s);
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  return std::move(*g);
}

// A summary as `simsys run --report json` prints it: every number of it, to the last digit, in
// one string, which is what "the same summary" is compared by.
std::string rendered(const Summary& summary) {
  const auto& slos = summary.graph().scenario().slos;
  return simsys::report::render_json(summary, simsys::report::evaluate_slos(summary, slos));
}

// The plain run of `g`: one simulation, the scenario's own seed.
Summary plain_run(const Graph& g) {
  auto sim = simsys::session::create_simulation(g, "noisy");
  INFO((sim ? "" : sim.error()));
  REQUIRE(sim.has_value());
  return simsys::session::run_summary(g, *sim);
}

std::vector<std::string> rendered_all(const std::vector<Summary>& summaries) {
  std::vector<std::string> texts;
  for (const auto& summary : summaries) texts.push_back(rendered(summary));
  return texts;
}

}  // namespace

TEST_CASE("run_replications returns one summary per replication") {
  const auto g = graph_of(noisy(1));
  REQUIRE(simsys::session::run_replications(g, 1, 5, 1).size() == 5);
  REQUIRE(simsys::session::run_replications(g, 1, 1, 1).size() == 1);
  REQUIRE(simsys::session::run_replications(g, 1, 12, 4).size() == 12);
}

TEST_CASE("replication k runs the seed that starts the sequence plus k") {
  // The graph carries seed 1 but the sequence starts at 7: replication k must equal the plain run
  // of a scenario whose own seed is 7 + k, so the `seed` argument is what counts.
  const auto g = graph_of(noisy(1));
  const auto replications = simsys::session::run_replications(g, 7, 5, 1);
  REQUIRE(replications.size() == 5);
  for (int k = 0; k < 5; ++k) {
    CAPTURE(k);
    const auto expected = graph_of(noisy(7 + k));
    REQUIRE(rendered(replications[static_cast<std::size_t>(k)]) == rendered(plain_run(expected)));
  }
}

TEST_CASE("the replications of a noisy scenario are five different runs") {
  const auto g = graph_of(noisy(1));
  const auto replications = simsys::session::run_replications(g, 1, 5, 1);
  std::vector<double> means;
  for (const auto& summary : replications) means.push_back(summary.ops().at(0).mean_s);
  REQUIRE(means.size() == 5);
  // C++20 std::ranges::adjacent_find: the first place two neighbours are equal, or the end. After
  // sorting, "no two equal means" is "no such place"; the range itself replaces a begin/end pair.
  std::ranges::sort(means);
  REQUIRE(std::ranges::adjacent_find(means) == means.end());
}

TEST_CASE("a scenario without noise gives the same run at every seed") {
  // The reason `run --replications` runs such a scenario once and says so.
  const auto g = graph_of(noiseless(1));
  const auto texts = rendered_all(simsys::session::run_replications(g, 1, 3, 1));
  REQUIRE(texts.size() == 3);
  REQUIRE(texts[1] == texts[0]);
  REQUIRE(texts[2] == texts[0]);
}

TEST_CASE("the summaries do not depend on how many workers ran the replications") {
  // Each replication is its own Simulation and its summary is stored at its index, so which thread
  // ran which seed cannot show: not even in the last digit of a number.
  const auto g = graph_of(noisy(3));
  const auto with_one = rendered_all(simsys::session::run_replications(g, 3, 6, 1));
  const auto with_four = rendered_all(simsys::session::run_replications(g, 3, 6, 4));
  const auto with_more_than_replications =
      rendered_all(simsys::session::run_replications(g, 3, 6, 16));
  REQUIRE(with_one.size() == 6);
  REQUIRE(with_four == with_one);
  REQUIRE(with_more_than_replications == with_one);
}

TEST_CASE("one replication is the plain run of that seed") {
  const auto g = graph_of(noisy(5));
  const auto replications = simsys::session::run_replications(g, 5, 1, 1);
  REQUIRE(replications.size() == 1);
  REQUIRE(rendered(replications[0]) == rendered(plain_run(g)));
}

TEST_CASE("the replication seeds count up from the first one") {
  CHECK(simsys::session::replication_seeds(7, 3) == std::vector<int>{7, 8, 9});
  CHECK(simsys::session::replication_seeds(-1, 3) == std::vector<int>{-1, 0, 1});
  CHECK(simsys::session::replication_seeds(1, 0).empty());
}

TEST_CASE("the replication seeds wrap around past the largest int instead of overflowing") {
  // Signed overflow is undefined behaviour; the seeds are counted in unsigned arithmetic and
  // converted back, which C++20 defines as modular.
  constexpr int largest = std::numeric_limits<int>::max();
  CHECK(simsys::session::replication_seeds(largest, 2) ==
        std::vector<int>{largest, std::numeric_limits<int>::min()});
}

TEST_CASE("a scenario has rate noise when some client's rate has a noise block") {
  CHECK(simsys::session::has_rate_noise(graph_of(noisy(1))));
  CHECK_FALSE(simsys::session::has_rate_noise(graph_of(noiseless(1))));
}
