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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// C++20 `import`: brings a named module's exported names into this translation unit without
// textual inclusion (the compiler reads the module's compiled interface). It replaces
// `#include` plus include guards; placed after the #includes because GCC 14 reports
// redefinitions when a standard header is included after an import (CLAUDE.md §3, eighth defect).
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

TEST_CASE("a noise block of sigma 0 is no rate noise: every seed would give the same run") {
  auto s = noisy(1);
  s.nodes[0].workload->noise->sigma = 0.0;
  const auto g = graph_of(s);
  CHECK_FALSE(simsys::session::has_rate_noise(g));
  // the premise: two seeds of it are the same run, byte for byte
  const auto texts = rendered_all(simsys::session::run_replications(g, 1, 2, 1));
  CHECK(texts[0] == texts[1]);
}

// ---- run --days N (phase 5b spec §5) ------------------------------------------------------------
//
// session::repeat_days(Scenario, int days) -> std::expected<Scenario, std::string>: a 24 h day lived
// `days` times. The properties: the run lasts days × 24 h; every rate spike and scripted fault recurs
// at the same clock time each day; the report observes the last day only; an input that would not
// repeat (a wave period that does not divide the day, a points trace, a ramp longer than a day) is
// refused, so the day the web app loops is really the same day.

namespace {

using simsys::model::Fault;
using simsys::model::FaultKind;
using simsys::workload::Ramp;
using simsys::workload::Spike;
using simsys::workload::Wave;

// One client at 100/s with a 3× spike at 12:00 for 10 min, into one service; a day long, 5 min of
// warmup; faults at 15:00 and 09:00, in that (file) order.
Scenario a_day() {
  Scenario s{.name = "day", .version = 1, .duration = 24h, .warmup = 5min};
  s.nodes = {
      NodeSpec{.id = "users",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{100},
                                              .wave = Wave{.period = 24h},
                                              .spikes = {Spike{.at = 12h, .duration = 10min,
                                                               .factor = 3.0}}},
                                    .op_shares = {{"op", 1.0}}}},
      NodeSpec{.id = "s", .type = NodeType::service, .capacity = Rate{1000}, .latency = 1ms}};
  s.links = {LinkSpec{.from = "users", .to = "s", .dir = simsys::model::Direction::inout}};
  s.faults = {Fault{.kind = FaultKind::node_down, .at = 15h, .duration = 1min, .node = "s"},
              Fault{.kind = FaultKind::node_down, .at = 9h, .duration = 30s, .node = "s"}};
  return s;
}

Scenario repeated(const Scenario& s, int days) {
  auto result = simsys::session::repeat_days(s, days);
  INFO((result ? "" : result.error()));
  REQUIRE(result.has_value());
  return std::move(*result);
}

std::string refusal_of(const Scenario& s, int days) {
  const auto result = simsys::session::repeat_days(s, days);
  REQUIRE_FALSE(result.has_value());
  return result.error();
}

std::vector<simsys::SimTime> spike_times(const Scenario& s) {
  std::vector<simsys::SimTime> times;
  for (const auto& spike : s.nodes[0].workload->curve.spikes) times.push_back(spike.at);
  return times;
}

std::vector<simsys::SimTime> fault_times(const Scenario& s) {
  std::vector<simsys::SimTime> times;
  for (const auto& fault : s.faults) times.push_back(fault.at);
  return times;
}

}  // namespace

TEST_CASE("one day lived once is the scenario as it is") {
  const auto s = a_day();
  const auto once = repeated(s, 1);
  CHECK(once.duration == s.duration);
  CHECK(once.warmup == s.warmup);
  CHECK(spike_times(once) == spike_times(s));
  CHECK(fault_times(once) == fault_times(s));
}

TEST_CASE("a day lived three times lasts three days and observes only the last") {
  const auto s = repeated(a_day(), 3);
  CHECK(s.duration == 72h);
  // (days − 1) × 24 h, the file's 5 min warmup replaced: by the last day the queues are as full as
  // that day makes them.
  CHECK(s.warmup == 48h);
}

TEST_CASE("a rate spike recurs at the same clock time every day") {
  const auto s = repeated(a_day(), 3);
  // 12:00 + k × 24 h: 43200, 129600, 216000 s.
  CHECK(spike_times(s) == std::vector<simsys::SimTime>{12h, 36h, 60h});
  for (const auto& spike : s.nodes[0].workload->curve.spikes) {
    CHECK(spike.duration == 10min);
    CHECK(spike.factor == 3.0);
  }
}

TEST_CASE("a scripted fault recurs every day, day by day in the file's order") {
  const auto s = repeated(a_day(), 3);
  // The file's order (15:00, then 09:00) kept within each day.
  CHECK(fault_times(s) == std::vector<simsys::SimTime>{15h, 9h, 39h, 33h, 63h, 57h});
  for (const auto& fault : s.faults) CHECK(fault.node == "s");
  CHECK(s.faults[2].duration == 1min);
  CHECK(s.faults[3].duration == 30s);
}

TEST_CASE("a wave repeats with the day when its period divides 24 h") {
  for (const auto period : {24h, 12h, 8h}) {
    auto s = a_day();
    s.nodes[0].workload->curve.wave->period = period;
    CHECK(simsys::session::repeat_days(s, 2).has_value());
  }
}

TEST_CASE("a wave whose period does not divide the day is refused") {
  auto s = a_day();
  s.nodes[0].workload->curve.wave->period = 168h;
  CHECK(refusal_of(s, 2) ==
        "--days: client 'users' has a wave period of 168h, which does not divide the day");
  s.nodes[0].workload->curve.wave->period = 5h;
  CHECK(refusal_of(s, 2) ==
        "--days: client 'users' has a wave period of 5h, which does not divide the day");
}

TEST_CASE("only a 24 h scenario can be lived for several days") {
  auto s = a_day();
  s.duration = 6h;
  CHECK(refusal_of(s, 2) == "--days needs a 24h scenario (this one runs 6h)");
}

TEST_CASE("a points trace is refused: it is held flat after its last entry, not periodic") {
  auto s = a_day();
  s.nodes[0].workload->curve.points = simsys::workload::Points{.at = {{0s, Rate{100}}}};
  CHECK(refusal_of(s, 2) == "--days: client 'users' rate.points is not periodic");
}

TEST_CASE("a ramp is kept if it ends within the first day, which is a warm-up anyway") {
  auto s = a_day();
  s.nodes[0].workload->curve.ramp = Ramp{.from = Rate{0}, .to = Rate{100}, .over = 5min};
  CHECK(simsys::session::repeat_days(s, 2).has_value());
  s.nodes[0].workload->curve.ramp->over = 25h;
  CHECK(refusal_of(s, 2) == "--days: client 'users' ramps over 25h, longer than a day");
}

TEST_CASE("the number of days is between 1 and 7") {
  CHECK(refusal_of(a_day(), 0) == "--days must be between 1 and 7");
  CHECK(refusal_of(a_day(), 8) == "--days must be between 1 and 7");
  CHECK(simsys::session::repeat_days(a_day(), 7).has_value());
}

TEST_CASE("noise is kept: each day draws its own, as days do") {
  auto s = a_day();
  s.nodes[0].workload->noise = simsys::workload::Noise{.sigma = 0.3, .correlation = 2s};
  const auto days = repeated(s, 2);
  REQUIRE(days.nodes[0].workload->noise.has_value());
  CHECK(days.nodes[0].workload->noise->sigma == 0.3);
}

namespace {

// The run line of a stream of `s`, `days` as the CLI passes it (absent without --days).
std::string run_line_of(const Scenario& s, std::optional<int> days) {
  const auto g = graph_of(s);
  auto sim = simsys::session::create_simulation(g, "day");
  REQUIRE(sim.has_value());
  std::vector<std::string> lines;
  static_cast<void>(simsys::session::stream_run(
      g, *sim, 1, [&](std::string_view line) { lines.emplace_back(line); }, days));
  REQUIRE_FALSE(lines.empty());
  return lines.front();
}

// a_day() at a 10 min step: 144 steps a day, quick to run twice.
Scenario a_coarse_day() {
  auto s = a_day();
  s.step = 10min;
  return s;
}

}  // namespace

TEST_CASE("a stream of a day lived twice says so, and lasts and observes as the run does") {
  const auto line = run_line_of(repeated(a_coarse_day(), 2), 2);
  CHECK(line.find("\"duration_s\":172800,") != std::string::npos);
  CHECK(line.find("\"warmup_s\":86400,") != std::string::npos);
  CHECK(line.find("\"frames\":288") != std::string::npos);  // 2 × 144 steps, a bucket each
  CHECK(line.ends_with(",\"days\":2}"));
}

TEST_CASE("a stream without --days has no days key: the 5a stream, byte for byte") {
  const auto line = run_line_of(a_coarse_day(), std::nullopt);
  CHECK(line.find("days") == std::string::npos);
}
