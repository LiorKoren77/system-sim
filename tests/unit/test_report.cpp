// Summaries over a run, SLO evaluation and the three output formats.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.core;
import simsys.engine;
import simsys.graph;
import simsys.model;
import simsys.report;
import simsys.workload;

using namespace std::chrono_literals;
using Catch::Approx;
using Catch::Matchers::WithinRel;
using simsys::Rate;
using simsys::model::KeySpec;
using simsys::model::LimiterAlgorithm;
using simsys::model::LinkSpec;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
using simsys::model::Step;
using simsys::model::Workload;
using simsys::report::Summary;

namespace {
// queue_limit is set only when given, so explain's provenance test sees "(default)".
Scenario mm1(double demand, std::optional<double> queue_limit = std::nullopt) {
  // C++20 designated initializers: `.name = ...` names each member being set, in
  // declaration order, and leaves the rest at their defaults. Replaces positional braces,
  // which break silently when members are reordered; a scenario reads like its YAML.
  Scenario s{.name = "mm1", .version = 1, .duration = 2s, .warmup = 500ms};
  s.nodes = {
      NodeSpec{.id = "c",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{demand}}, .op_shares = {{"op", 1.0}}}},
      NodeSpec{.id = "s",
               .type = NodeType::service,
               .capacity = Rate{1000},
               .latency = 1ms,
               .advanced = {.queue_limit = queue_limit}}};
  s.links = {simsys::model::LinkSpec{
      .from = "c", .to = "s", .dir = simsys::model::Direction::inout, .latency = 0s}};
  return s;
}
// Ch. 11's fan-out on write: every post to `api` becomes 100 writes to `feed`, so 100
// posts/s offer feed 10k writes/s against a capacity of 1k/s. Feed drops about 90% of
// what it is sent, while the client offers only 100 requests/s: a drop rate of "dropped
// anywhere / offered by clients" would be 90, not a ratio.
Scenario fan_out() {
  Scenario s{.name = "fanout", .version = 1, .duration = 5s};
  s.nodes = {
      NodeSpec{.id = "users",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{100}}, .op_shares = {{"post", 1.0}}}},
      NodeSpec{.id = "api",
               .type = NodeType::service,
               .capacity = Rate{10'000},
               .ops = {{"post", {Step{.to = "feed", .op = "write", .factor = 100}}}}},
      NodeSpec{.id = "feed", .type = NodeType::service, .capacity = Rate{1000}, .latency = 1ms}};
  s.links = {LinkSpec{.from = "users", .to = "api", .dir = simsys::model::Direction::inout},
             LinkSpec{.from = "api", .to = "feed"}};
  return s;
}

// Two services in a loop: `a` sends half of its `msg` flow to `b` as `job`, `b` sends every
// job back to `a`. A loop of gain `gain`: a's offered load is 100 / (1 - gain).
Scenario loop(double gain) {
  Scenario s{.name = "loop", .version = 1, .duration = 10s};
  s.nodes = {
      NodeSpec{.id = "users",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{100}}, .op_shares = {{"msg", 1.0}}}},
      NodeSpec{.id = "a",
               .type = NodeType::service,
               .capacity = Rate{10'000},
               .ops = {{"msg", {Step{.to = "b", .op = "job", .factor = gain}}}}},
      NodeSpec{.id = "b",
               .type = NodeType::service,
               .capacity = Rate{10'000},
               .ops = {{"job", {Step{.to = "a", .op = "msg"}}}}}};
  s.links = {LinkSpec{.from = "users", .to = "a", .dir = simsys::model::Direction::inout},
             LinkSpec{.from = "a", .to = "b"}, LinkSpec{.from = "b", .to = "a"}};
  return s;
}

// u(4000/s) -> limiter (token bucket, 1000/s, burst 100) -> api (10k/s): after the first step
// the limiter admits 1000/s and refuses 3000/s of the 4000 it is offered. The 500 ms warmup
// keeps the first step, where the bucket may still hold tokens, out of the summary.
Scenario limited() {
  Scenario s{.name = "limited", .version = 1, .duration = 1s, .warmup = 500ms};
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{4000}}, .op_shares = {{"call", 1.0}}}},
      NodeSpec{.id = "limiter",
               .type = NodeType::rate_limiter,
               .capacity = Rate{1e6},
               .algorithm = LimiterAlgorithm::token_bucket,
               .limit = Rate{1000},
               .burst = 100},
      NodeSpec{.id = "api", .type = NodeType::service, .capacity = Rate{10'000}}};
  s.links = {LinkSpec{.from = "u", .to = "limiter", .latency = 0s},
             LinkSpec{.from = "limiter", .to = "api", .latency = 0s}};
  return s;
}

Summary summarize(const Scenario& s, const simsys::graph::Graph& g) {
  Summary summary{g, s.warmup};
  auto sim = *simsys::engine::Simulation::create(g);
  sim.run({.duration = s.duration, .step = s.step}, [&](const auto& r) { summary.observe(r); });
  return summary;
}
}  // namespace

TEST_CASE("node summaries carry mean and peak utilization, peak backlog, drops and red time",
          "[report]") {
  const auto s = mm1(1500, 100);
  const auto g = *simsys::graph::Graph::build(s);
  const auto summary = summarize(s, g);
  const auto nodes = summary.nodes();
  REQUIRE(nodes.size() == 2);
  REQUIRE(nodes[1].id == "s");
  REQUIRE(nodes[1].peak_rho == 1.0);
  REQUIRE(nodes[1].mean_rho == Approx(1.0));
  REQUIRE(nodes[1].peak_backlog == Approx(100));
  REQUIRE(nodes[1].total_dropped > 0);
  REQUIRE(nodes[1].red_time_s == Approx(1.5));  // 2 s minus 0.5 s warmup
}

TEST_CASE("op summaries fit percentiles to the time-averaged response moments", "[report]") {
  const auto s = mm1(500);
  const auto g = *simsys::graph::Graph::build(s);
  const auto ops = summarize(s, g).ops();
  REQUIRE(ops.size() == 1);
  REQUIRE(ops[0].client == "c");
  REQUIRE(ops[0].op == "op");
  REQUIRE(ops[0].offered_per_s == Approx(500));
  REQUIRE(ops[0].mean_s == Approx(0.002));
  REQUIRE(ops[0].p50_s < ops[0].mean_s);
  REQUIRE(ops[0].p95_s < ops[0].p99_s);
}

TEST_CASE("warmup steps are excluded from summaries", "[report]") {
  auto s = mm1(500);
  s.warmup = 10s;  // longer than the run: nothing observed
  const auto g = *simsys::graph::Graph::build(s);
  REQUIRE(summarize(s, g).observed_steps() == 0);
}

TEST_CASE("the drop rate is total dropped over total offered after warmup", "[report]") {
  const auto s = mm1(1500, 100);
  const auto g = *simsys::graph::Graph::build(s);
  const auto summary = summarize(s, g);
  REQUIRE(summary.drop_rate() ==
          Approx(500.0 / 1500.0).epsilon(1e-3));  // queue full well before warmup ends
}

TEST_CASE("the drop rate is per node: its dropped over its own demand, the worst node by default",
          "[report]") {
  const auto s = fan_out();
  const auto g = *simsys::graph::Graph::build(s);
  // The feed's own ratio, summed independently of Summary from the same records.
  const auto feed = *g.index_of("feed");
  double feed_dropped = 0.0;
  double feed_demand = 0.0;
  Summary summary{g, s.warmup};
  auto sim = *simsys::engine::Simulation::create(g);
  sim.run({.duration = s.duration, .step = s.step}, [&](const simsys::engine::StepRecord& r) {
    summary.observe(r);
    feed_dropped += r.nodes[feed].dropped_per_s * 0.1;  // 100 ms steps
    feed_demand += r.nodes[feed].demand_per_s * 0.1;
  });
  REQUIRE(feed_demand > 0.0);
  REQUIRE_THAT(summary.drop_rate("feed"), WithinRel(feed_dropped / feed_demand, 1e-12));
  REQUIRE(summary.drop_rate("api") == 0.0);
  REQUIRE(summary.drop_rate("users") == 0.0);  // a client offers; it never drops
  REQUIRE(summary.drop_rate() == summary.drop_rate("feed"));
  REQUIRE(summary.drop_rate() > 0.8);
  REQUIRE(summary.drop_rate() <= 1.0);
  REQUIRE(summary.worst_drop_node() == "feed");

  const std::vector<simsys::model::SloSpec> slos{
      {.metric = simsys::model::SloMetric::drop_rate, .max = 1.0},
      {.metric = simsys::model::SloMetric::drop_rate, .node = "api", .max = 0.0}};
  const auto results = simsys::report::evaluate_slos(summary, slos);
  REQUIRE(results[0].observed == summary.drop_rate("feed"));
  REQUIRE(results[1].observed == 0.0);
  REQUIRE(simsys::report::all_pass(results));

  const auto text = simsys::report::render_table(summary, results, /*color=*/false);
  const auto header = text.substr(0, text.find('\n'));
  REQUIRE(header.find("drop rate") != std::string::npos);
  REQUIRE(header.find("feed") != std::string::npos);
}

// No data is an error, not a pass: a window with no observed step evaluates nothing, so
// every SLO is skipped with no observed value. all_pass of all-skipped stays true (a
// skipped SLO never fails); `simsys run` exits 1 when zero steps were observed. That exit
// code needs a process to test, so the library half of the contract is pinned here.
TEST_CASE("an empty observation window evaluates nothing: every SLO is skipped, none observed",
          "[report]") {
  auto s = mm1(500);
  s.warmup = s.duration;  // the loader refuses this; the library must still not pass it
  s.slos = {simsys::model::SloSpec{.metric = simsys::model::SloMetric::p99_latency,
                                   .node = "c",
                                   .max = simsys::SimTime{50ms}},
            simsys::model::SloSpec{.metric = simsys::model::SloMetric::drop_rate, .max = 0.0},
            simsys::model::SloSpec{
                .metric = simsys::model::SloMetric::utilization, .node = "s", .max = 0.9}};
  const auto g = *simsys::graph::Graph::build(s);
  const auto summary = summarize(s, g);
  REQUIRE(summary.observed_steps() == 0);
  const auto results = simsys::report::evaluate_slos(summary, s.slos);
  REQUIRE(results.size() == 3);
  for (const auto& r : results) {
    REQUIRE(r.status == simsys::report::SloStatus::skipped);
    REQUIRE_FALSE(r.observed.has_value());
  }
  REQUIRE(simsys::report::all_pass(results));
  const auto text = simsys::report::render_table(summary, results, /*color=*/false);
  REQUIRE(text.find("0 steps observed: nothing to evaluate") != std::string::npos);
  const auto json = simsys::report::render_json(summary, results);
  REQUIRE(json.find("\"steps\": 0") != std::string::npos);
  REQUIRE(json.find("\"status\": \"pass\"") == std::string::npos);
}

TEST_CASE("SLOs pass, fail or are skipped when the metric is not available", "[report]") {
  auto s = mm1(500);
  s.slos = {
      simsys::model::SloSpec{.metric = simsys::model::SloMetric::p99_latency,
                             .node = "c",
                             .op = "op",
                             .max = simsys::SimTime{50ms}},
      simsys::model::SloSpec{.metric = simsys::model::SloMetric::drop_rate, .max = 0.0},
      simsys::model::SloSpec{
          .metric = simsys::model::SloMetric::utilization, .node = "s", .max = 0.3},
      simsys::model::SloSpec{
          .metric = simsys::model::SloMetric::queue_lag, .node = "s", .max = simsys::SimTime{1s}}};
  const auto g = *simsys::graph::Graph::build(s);
  const auto results = simsys::report::evaluate_slos(summarize(s, g), s.slos);
  using simsys::report::SloStatus;
  REQUIRE(results.size() == 4);
  REQUIRE(results[0].status == SloStatus::pass);
  REQUIRE(results[0].observed.has_value());
  REQUIRE(results[1].status == SloStatus::pass);
  REQUIRE(results[2].status == SloStatus::fail);     // rho 0.5 > 0.3
  REQUIRE(results[3].status == SloStatus::skipped);  // queue_lag arrives in phase 2
  REQUIRE(simsys::report::all_pass(results) == false);
}

TEST_CASE("a latency SLO without an op applies to every op of the client", "[report]") {
  auto s = mm1(500);
  s.nodes[0].workload->op_shares = {{"a", 0.5}, {"b", 0.5}};
  s.slos = {simsys::model::SloSpec{
      .metric = simsys::model::SloMetric::p99_latency, .node = "c", .max = simsys::SimTime{1us}}};
  const auto g = *simsys::graph::Graph::build(s);
  const auto results = simsys::report::evaluate_slos(summarize(s, g), s.slos);
  REQUIRE(results.size() == 1);
  REQUIRE(results[0].status == simsys::report::SloStatus::fail);  // 2 ms > 1 us for both ops
}

TEST_CASE("the text table names every node and op and marks percentiles as estimated", "[report]") {
  const auto s = mm1(500);
  const auto g = *simsys::graph::Graph::build(s);
  const auto text = simsys::report::render_table(summarize(s, g), {}, /*color=*/false);
  REQUIRE(text.find("s ") != std::string::npos);
  REQUIRE(text.find("p99") != std::string::npos);
  REQUIRE(text.find("estimated") != std::string::npos);
  REQUIRE(text.find("\x1b[") == std::string::npos);
  REQUIRE(simsys::report::render_table(summarize(s, g), {}, /*color=*/true).find("\x1b[") !=
          std::string::npos);
}

TEST_CASE("the JSON report carries the scenario name, nodes, ops and slo results", "[report]") {
  auto s = mm1(500);
  s.slos = {simsys::model::SloSpec{.metric = simsys::model::SloMetric::drop_rate, .max = 0.0}};
  const auto g = *simsys::graph::Graph::build(s);
  const auto summary = summarize(s, g);
  const auto json =
      simsys::report::render_json(summary, simsys::report::evaluate_slos(summary, s.slos));
  REQUIRE(json.front() == '{');
  REQUIRE(json.back() == '}');
  REQUIRE(json.find("\"scenario\": \"mm1\"") != std::string::npos);
  REQUIRE(json.find("\"nodes\"") != std::string::npos);
  REQUIRE(json.find("\"ops\"") != std::string::npos);
  REQUIRE(json.find("\"slo\"") != std::string::npos);
  REQUIRE(json.find("\"status\": \"pass\"") != std::string::npos);
}

TEST_CASE("infinite latency is reported as unavailable, not as a number", "[report]") {
  auto s = mm1(500);
  s.nodes[1].capacity = Rate{0};  // a dead server: the fluid queue reports an unbounded wait
  const auto g = *simsys::graph::Graph::build(s);
  const auto summary = summarize(s, g);
  const auto text = simsys::report::render_table(summary, {}, false);
  REQUIRE(text.find("unavailable") != std::string::npos);
  REQUIRE(text.find("inf") == std::string::npos);
  const auto json = simsys::report::render_json(summary, {});
  REQUIRE(json.find("inf") == std::string::npos);
  REQUIRE(json.find("null") != std::string::npos);
}

TEST_CASE("the series writer emits a header and one row per node per step; clients have no zone",
          "[report]") {
  const auto s = mm1(500);
  const auto g = *simsys::graph::Graph::build(s);
  std::ostringstream out;
  simsys::report::SeriesWriter writer{out, g};
  auto sim = *simsys::engine::Simulation::create(g);
  sim.run({.duration = 300ms, .step = 100ms}, [&](const auto& r) { writer.observe(r); });
  const auto text = out.str();
  // C++20 std::string::starts_with (and its twin ends_with): a prefix test in one call,
  // replacing `compare(0, prefix.size(), prefix) == 0` and its length bookkeeping.
  REQUIRE(text.starts_with("t_s,node,rho,backlog,drops_per_s,rejected_per_s,latency_ms,zone\n"));
  // C++20 std::ranges::count: counts the matches in a whole range (here the characters of
  // the string), replacing std::count(text.begin(), text.end(), '\n').
  REQUIRE(std::ranges::count(text, '\n') == 1 + 3 * 2);
  REQUIRE(text.find("\n0.1,s,") != std::string::npos);  // shortest round-trip form of 0.1 s
  // A client is a source: its numeric columns are 0 (machines keep a number) and its zone
  // is `none` (it has no queue to colour).
  REQUIRE(text.find("\n0,c,0,0,0,0,0,none\n") != std::string::npos);
}

TEST_CASE("render_explain lists every node with its resolved numbers and provenance", "[report]") {
  const auto s = mm1(500);
  const auto g = *simsys::graph::Graph::build(s);
  const auto text = simsys::report::render_explain(g);
  REQUIRE(text.find("scenario mm1") != std::string::npos);
  REQUIRE(text.find("capacity=1k/s (scenario)") != std::string::npos);
  REQUIRE(text.find("service_time=1ms (scenario)") != std::string::npos);
  REQUIRE(text.find("queue_limit=1000 (default)") != std::string::npos);
  REQUIRE(text.find("c -> s") != std::string::npos);
  REQUIRE(text.find("order: c, s") != std::string::npos);
  REQUIRE(text.find("offered 500/s") != std::string::npos);  // the static checks block
}

TEST_CASE("explain's paper load follows a loop to its fixed point: gain 0.5 doubles the entry",
          "[report]") {
  const auto g = *simsys::graph::Graph::build(loop(0.5));
  const auto text = simsys::report::render_explain(g);
  CAPTURE(text);
  // a = 100 + 0.5 a, so a = 200/s (2% of 10k/s); b = 0.5 a = 100/s.
  REQUIRE(text.find("offered 200/s vs 10k/s total capacity  (2%, green)") != std::string::npos);
  REQUIRE(text.find("offered 100/s vs 10k/s total capacity  (1%, green)") != std::string::npos);
  REQUIRE(text.find("no steady state") == std::string::npos);
}

TEST_CASE("explain says when a loop of gain >= 1 has no steady state", "[report]") {
  const auto g = *simsys::graph::Graph::build(loop(2.0));
  const auto text = simsys::report::render_explain(g);
  CAPTURE(text);
  REQUIRE(text.ends_with("  (no steady state: a loop of gain >= 1)\n"));
}

TEST_CASE("explain summarises the client's curve and checks the peak offered load", "[report]") {
  // A daily sine around 1k/s peaking at noon offers 1.5k/s at 12:00: a service of 1.2k/s
  // looks fine at t = 0 (500/s, midnight's trough) and is overloaded at the peak, which is
  // what a back-of-the-envelope check must catch ("peak QPS", ch. 2).
  Scenario s{.name = "peak", .version = 1, .duration = 24h};
  s.nodes = {
      NodeSpec{.id = "users",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{1000},
                                              .wave =
                                                  simsys::workload::Wave{
                                                      .shape = simsys::workload::WaveShape::sine,
                                                      .period = 24h,
                                                      .amplitude = 0.5,
                                                      .peak_at = 12h}},
                                    .op_shares = {{"op", 1.0}}}},
      NodeSpec{.id = "api", .type = NodeType::service, .capacity = Rate{1200}, .latency = 1ms}};
  s.links = {LinkSpec{.from = "users", .to = "api", .dir = simsys::model::Direction::inout}};
  const auto g = *simsys::graph::Graph::build(s);
  const auto text = simsys::report::render_explain(g);
  CAPTURE(text);
  REQUIRE(text.find("rate: base 1k/s, wave sine P=24h A=0.5 peak 12:00") != std::string::npos);
  REQUIRE(text.find("checks (peak offered load over the first 24h, no queueing):\n") !=
          std::string::npos);
  REQUIRE(text.find("  api  peak offered 1.5k/s vs 1.2k/s total capacity  (125%, red)\n") !=
          std::string::npos);
}

// A limiter refuses requests without dropping them from a queue, so the report needs a column
// of its own for them and a reject rate apart from the drop rate (D5 of the rate-limiter
// review): a refusal is the policy working, a drop is a node failing. A limiter that sheds 75%
// of its input by policy has a reject rate of 0.75 and a drop rate of 0.
TEST_CASE("the table and JSON report a rejected column and a reject rate apart from the drop rate",
          "[report]") {
  const auto s = limited();
  const auto g = *simsys::graph::Graph::build(s);
  const auto summary = summarize(s, g);
  const auto text = simsys::report::render_table(summary, {}, /*color=*/false);
  CAPTURE(text);
  REQUIRE(text.find("rejected") != std::string::npos);
  REQUIRE(text.find("reject rate 75% (worst: limiter)") != std::string::npos);
  const auto json = simsys::report::render_json(summary, {});
  CAPTURE(json);
  REQUIRE(json.find("\"rejected\"") != std::string::npos);
  REQUIRE(json.find("\"reject_rate\": 0.7") != std::string::npos);
  REQUIRE(json.find("\"worst_reject_node\": \"limiter\"") != std::string::npos);
  REQUIRE_THAT(summary.reject_rate("limiter"), WithinRel(0.75, 1e-9));  // 3000 refused of 4000
  REQUIRE(summary.drop_rate("limiter") == 0.0);
  REQUIRE(summary.drop_rate("api") == 0.0);
  REQUIRE(summary.worst_reject_node() == "limiter");
  REQUIRE(summary.worst_drop_node().empty());
}

TEST_CASE("reject_rate is a separate SLO metric", "[report]") {
  const auto s = limited();
  const auto g = *simsys::graph::Graph::build(s);
  const auto summary = summarize(s, g);
  using simsys::model::SloMetric;
  const std::vector<simsys::model::SloSpec> slos{
      {.metric = SloMetric::reject_rate, .node = "limiter", .max = 0.5},
      {.metric = SloMetric::reject_rate, .max = 0.8},  // the worst node's: the limiter's 0.75
      {.metric = SloMetric::drop_rate, .node = "limiter", .max = 0.0}};
  const auto results = simsys::report::evaluate_slos(summary, slos);
  REQUIRE(results[0].status == simsys::report::SloStatus::fail);
  REQUIRE_THAT(*results[0].observed, WithinRel(0.75, 1e-9));
  REQUIRE(results[1].status == simsys::report::SloStatus::pass);
  REQUIRE(results[2].status == simsys::report::SloStatus::pass);  // refusals are not drops
}

TEST_CASE("the series writer emits rejected_per_s", "[report]") {
  const auto s = limited();
  const auto g = *simsys::graph::Graph::build(s);
  std::ostringstream out;
  simsys::report::SeriesWriter writer{out, g};
  auto sim = *simsys::engine::Simulation::create(g);
  sim.run({.duration = s.duration, .step = s.step}, [&](const auto& r) { writer.observe(r); });
  const auto text = out.str();
  CAPTURE(text);
  // The limiter's row at t = 0.9 s: its queue drops nothing, its policy refuses 3000/s.
  const auto row = text.find("\n0.9,limiter,");
  REQUIRE(row != std::string::npos);
  const auto line = text.substr(row + 1, text.find('\n', row + 1) - row - 1);
  std::vector<std::string> fields;
  std::string field;
  std::istringstream cells{line};
  while (std::getline(cells, field, ',')) fields.push_back(field);
  REQUIRE(fields.size() == 8);
  REQUIRE(fields[4] == "0");     // drops_per_s
  REQUIRE(fields[5] == "3000");  // rejected_per_s
}

// Plan 2a refinement 8: a request that was rejected or dropped is not answered and sees no
// latency, so the client's latency moments are weighted by the requests ANSWERED in each
// step, not by those offered. Two steps of 100 offered: the first answers all of them in
// 1 ms, the second none (a 100 ms latency nobody experienced); the mean is 1 ms, where an
// offered-weighted mean would report (1 + 100) / 2 = 50.5 ms.
TEST_CASE("op latency is weighted by the requests answered, not offered", "[report]") {
  const auto s = mm1(500);
  const auto g = *simsys::graph::Graph::build(s);
  Summary summary{g, simsys::SimTime{0}};
  const auto record = [](simsys::SimTime t, double answered, double latency_s) {
    return simsys::engine::StepRecord{
        .t = t,
        .nodes = std::vector<simsys::engine::NodeSample>(2),
        .responses = {simsys::engine::ResponseSample{.client = 0,
                                                     .op = "op",
                                                     .offered = Rate{100},
                                                     .answered = Rate{answered},
                                                     .response = {latency_s, 0.0}}}};
  };
  summary.observe(record(simsys::SimTime{0}, 100, 0.001));
  summary.observe(record(simsys::SimTime{100ms}, 0, 0.1));
  const auto ops = summary.ops();
  REQUIRE(ops.size() == 1);
  REQUIRE_THAT(ops[0].mean_s, WithinRel(0.001, 1e-12));
  REQUIRE_THAT(ops[0].offered_per_s, WithinRel(100.0, 1e-12));  // offered still counts all
}

namespace {
// u(100/s) -> lb -> api (2 replicas): the balancer's policy decides how api's replicas queue.
Scenario balanced(simsys::model::LbPolicy policy) {
  Scenario s{.name = "balanced", .version = 1, .duration = 1s};
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{100}}, .op_shares = {{"call", 1.0}}}},
      NodeSpec{.id = "lb", .type = NodeType::load_balancer, .policy = policy},
      NodeSpec{.id = "api", .type = NodeType::service, .replicas = 2, .capacity = Rate{1000}}};
  s.links = {LinkSpec{.from = "u", .to = "lb"}, LinkSpec{.from = "lb", .to = "api"}};
  return s;
}

// u(100/s) -> limiter (token bucket 1k/s, burst 100, counters kept in `counters`) -> api, with
// `counters` a cache that answers every read (fixed hit ratio 1.0, cache-aside).
Scenario limited_with_counters() {
  Scenario s{.name = "counted", .version = 1, .duration = 1s};
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{100}}, .op_shares = {{"call", 1.0}}}},
      NodeSpec{.id = "limiter",
               .type = NodeType::rate_limiter,
               .capacity = Rate{1e6},
               .algorithm = LimiterAlgorithm::token_bucket,
               .limit = Rate{1000},
               .burst = 100,
               .backend = "counters"},
      NodeSpec{.id = "api", .type = NodeType::service, .capacity = Rate{10'000}},
      NodeSpec{.id = "counters",
               .type = NodeType::cache,
               .capacity = Rate{100'000},
               .hit_ratio = simsys::model::HitRatio{.fixed = 1.0},
               .strategy = simsys::model::CacheStrategy::cache_aside}};
  s.links = {LinkSpec{.from = "u", .to = "limiter"}, LinkSpec{.from = "limiter", .to = "api"},
             LinkSpec{.from = "limiter", .to = "counters"}};
  return s;
}
}  // namespace

TEST_CASE("explain prints each node type's policy line", "[report]") {
  SECTION("a load balancer names its policy and the node behind it is pooled") {
    const auto g =
        *simsys::graph::Graph::build(balanced(simsys::model::LbPolicy::least_connections));
    const auto text = simsys::report::render_explain(g);
    CAPTURE(text);
    REQUIRE(text.find("policy=least_connections") != std::string::npos);
    REQUIRE(text.find("pooled") != std::string::npos);
    REQUIRE(text.find("rate: base 100/s") != std::string::npos);  // the client's line
  }
  SECTION(
      "a rate limiter shows its algorithm, limit, burst and counter backend; a cache its model") {
    const auto g = *simsys::graph::Graph::build(limited_with_counters());
    const auto text = simsys::report::render_explain(g);
    CAPTURE(text);
    REQUIRE(text.find("algorithm=token_bucket limit=1k/s burst=100 backend=counters") !=
            std::string::npos);
    REQUIRE(text.find("hit_ratio=1 (fixed) strategy=cache_aside write_ops=write") !=
            std::string::npos);
  }
}

// A leaky bucket's queue is not configured like a server's: the graph derives all four
// numbers from the limiter (rate `limit`, one deterministic leak slot, room `burst`), so
// explain names that source instead of "(scenario)", "(implied)" or "(default)".
TEST_CASE("explain says a leaky bucket's queue comes from its limit, leak slot and burst",
          "[report]") {
  auto s = limited_with_counters();
  s.nodes[1].algorithm = LimiterAlgorithm::leaky_bucket;
  s.nodes[1].capacity = std::nullopt;  // a leaky bucket refuses a capacity of its own
  s.nodes[1].burst = 500;
  const auto text = simsys::report::render_explain(*simsys::graph::Graph::build(s));
  CAPTURE(text);
  REQUIRE(text.find("capacity=1k/s (limit)") != std::string::npos);
  REQUIRE(text.find("service_time=1ms (limit)") != std::string::npos);
  REQUIRE(text.find("c=1 (leak slot)") != std::string::npos);
  REQUIRE(text.find("queue_limit=500 (burst)") != std::string::npos);
}

// Provenance is about presence, not value: a number the scenario left out says "(default)",
// a stated one says nothing more, even when it equals the default.
TEST_CASE("explain marks a defaulted burst, window, policy and vnodes as default", "[report]") {
  const auto explain = [](const Scenario& s) {
    return simsys::report::render_explain(*simsys::graph::Graph::build(s));
  };
  SECTION("a bucket's burst") {
    auto s = limited_with_counters();
    s.nodes[1].burst = std::nullopt;
    const auto text = explain(s);
    CAPTURE(text);
    REQUIRE(text.find("burst=1000 (default) backend=counters") != std::string::npos);
  }
  SECTION("a windowed algorithm's window, stated or not") {
    auto s = limited_with_counters();
    s.nodes[1].algorithm = LimiterAlgorithm::sliding_log;
    s.nodes[1].burst = std::nullopt;
    const auto defaulted = explain(s);
    CAPTURE(defaulted);
    REQUIRE(defaulted.find("window=1s (default) backend=counters") != std::string::npos);
    s.nodes[1].window = 1s;
    const auto stated = explain(s);
    CAPTURE(stated);
    REQUIRE(stated.find("window=1s backend=counters") != std::string::npos);
  }
  SECTION("a balancer's policy and vnodes") {
    auto s = balanced(simsys::model::LbPolicy::round_robin);
    REQUIRE(explain(s).find("policy=round_robin\n") != std::string::npos);
    s.nodes[1].policy = std::nullopt;
    REQUIRE(explain(s).find("policy=round_robin (default)\n") != std::string::npos);
    s.nodes[1].policy = simsys::model::LbPolicy::consistent_hash;
    REQUIRE(explain(s).find("policy=consistent_hash vnodes=150 (default)\n") != std::string::npos);
    s.nodes[1].vnodes = 150;
    REQUIRE(explain(s).find("policy=consistent_hash vnodes=150\n") != std::string::npos);
  }
}

TEST_CASE("explain prints the client's curve summary with wave, spikes and noise", "[report]") {
  using namespace simsys::workload;
  auto s = mm1(100);
  s.nodes[0].workload->curve = {
      .base = Rate{800},
      .wave = Wave{.shape = WaveShape::sine, .period = 24h, .amplitude = 0.6, .peak_at = 20h},
      .spikes = {Spike{.at = 1h, .duration = 5min, .factor = 2.0},
                 Spike{.at = 2h, .duration = 5min, .factor = 2.0},
                 Spike{.at = 3h, .duration = 5min, .factor = 2.0}}};
  s.nodes[0].workload->noise = Noise{.sigma = 0.15, .correlation = 5min};
  const auto text = simsys::report::render_explain(*simsys::graph::Graph::build(s));
  CAPTURE(text);
  // The code prints the Greek letters sigma and tau as UTF-8: pinned as it is.
  REQUIRE(text.find("wave sine P=24h A=0.6 peak 20:00") != std::string::npos);
  REQUIRE(text.find("spikes 3") != std::string::npos);
  REQUIRE(text.find("noise \u03c3=0.15 \u03c4=5m") != std::string::npos);
}

// The node table always has a `rejected` column (a number per node), but the header line
// mentions a reject rate only when some rate limiter refused anything: a scenario without a
// limiter should not grow a "reject rate 0%" it has no meaning for.
TEST_CASE("the table has a reject rate only when something was refused", "[report]") {
  const auto plain = mm1(500);
  const auto plain_graph = *simsys::graph::Graph::build(plain);
  const auto plain_text =
      simsys::report::render_table(summarize(plain, plain_graph), {}, /*color=*/false);
  CAPTURE(plain_text);
  REQUIRE(plain_text.find("reject rate") == std::string::npos);

  const auto overloaded = limited();
  const auto overloaded_graph = *simsys::graph::Graph::build(overloaded);
  const auto overloaded_text =
      simsys::report::render_table(summarize(overloaded, overloaded_graph), {}, /*color=*/false);
  CAPTURE(overloaded_text);
  REQUIRE(overloaded_text.find("rejected") != std::string::npos);
  REQUIRE(overloaded_text.find("reject rate") != std::string::npos);
}

TEST_CASE("the JSON always carries reject_rate and worst_reject_node", "[report]") {
  const auto s = mm1(500);
  const auto g = *simsys::graph::Graph::build(s);
  const auto json = simsys::report::render_json(summarize(s, g), {});
  CAPTURE(json);
  // No refusals: the rate is the number 0 and there is no worst node (null, not "").
  REQUIRE(json.find("\"reject_rate\": 0,\n") != std::string::npos);
  REQUIRE(json.find("\"worst_reject_node\": null") != std::string::npos);
}

TEST_CASE("the CSV series has the rejected_per_s column right after drops_per_s", "[report]") {
  const auto s = mm1(500);
  const auto g = *simsys::graph::Graph::build(s);
  std::ostringstream out;
  simsys::report::SeriesWriter writer{out, g};
  const auto text = out.str();  // the header is written on construction, before any step
  REQUIRE(text.substr(0, text.find('\n') + 1) ==
          "t_s,node,rho,backlog,drops_per_s,rejected_per_s,latency_ms,zone\n");
}

namespace {
// Ch. 13's autocomplete: the browser caches suggestions and only its misses reach the servers.
// users(1k/s suggest) -> browser_cache (fixed 0.4, then: lb.suggest on miss) -> lb -> api.
Scenario browser_cached() {
  Scenario s{.name = "autocomplete", .version = 1, .duration = 1min};
  s.nodes = {NodeSpec{.id = "users",
                      .type = NodeType::client,
                      .workload =
                          Workload{.curve = {.base = Rate{1000}}, .op_shares = {{"suggest", 1.0}}}},
             NodeSpec{.id = "browser_cache",
                      .type = NodeType::cache,
                      .then = {Step{
                          .to = "lb", .op = "suggest", .on = simsys::model::StepCondition::miss}},
                      .hit_ratio = simsys::model::HitRatio{.fixed = 0.4}},
             NodeSpec{.id = "lb", .type = NodeType::load_balancer},
             NodeSpec{.id = "api", .type = NodeType::service, .capacity = Rate{5000}}};
  s.links = {LinkSpec{.from = "users", .to = "browser_cache"},
             LinkSpec{.from = "browser_cache", .to = "lb"}, LinkSpec{.from = "lb", .to = "api"}};
  return s;
}
// Ch. 8's redirect path with a cache whose hit ratio is `che`: users(1k/s, Zipf 1 over 100M
// links) -> web -> url_cache (20M keys), and web -> url_db for the misses.
Scenario che_cached() {
  Scenario s{.name = "shortener", .version = 1, .duration = 1min};
  s.nodes = {
      NodeSpec{.id = "users",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{1000}},
                                    .op_shares = {{"redirect", 1.0}},
                                    .keys = KeySpec{.zipf = 1.0, .space = 100e6}}},
      NodeSpec{
          .id = "web",
          .type = NodeType::service,
          .capacity = Rate{10'000},
          .ops = {{"redirect",
                   {Step{.to = "url_cache", .op = "read"},
                    Step{
                        .to = "url_db", .op = "read", .on = simsys::model::StepCondition::miss}}}}},
      NodeSpec{.id = "url_cache",
               .type = NodeType::cache,
               .hit_ratio = simsys::model::HitRatio{.fixed = std::nullopt, .che = true},
               .keys = 20'000'000},
      NodeSpec{.id = "url_db", .type = NodeType::service, .capacity = Rate{5000}}};
  s.links = {LinkSpec{.from = "users", .to = "web"}, LinkSpec{.from = "web", .to = "url_cache"},
             LinkSpec{.from = "web", .to = "url_db"}};
  return s;
}
// The line of explain's checks block (after "checks (") that starts, after its indent, with
// `id` and a space; empty when there is none.
std::string check_line(const std::string& text, const std::string& id) {
  const auto at = text.find("\n  " + id + " ", text.find("checks ("));
  if (at == std::string::npos) return {};
  const auto end = text.find('\n', at + 1);
  return text.substr(at + 1, end - at - 1);
}
}  // namespace

// A cache's `then` steps are what happens after it answers: `on: miss` ones carry the share it
// missed (1 - h), unconditional ones everything. The paper load must follow them, or every
// node behind a browser cache would look idle.
TEST_CASE("explain forwards a cache's then-steps: misses reach the next node", "[report]") {
  const auto text = simsys::report::render_explain(*simsys::graph::Graph::build(browser_cached()));
  CAPTURE(text);
  // 60 % of 1k/s: the misses of a 0.4 hit ratio.
  // C++23 std::string::contains: a substring test in one call, replacing
  // `find(...) != std::string::npos`; the check line has more on it than this test is about.
  REQUIRE(check_line(text, "lb").contains("peak offered 600/s vs 100k/s"));
  REQUIRE(check_line(text, "api").contains("peak offered 600/s vs 5k/s"));
}

// With no number for h (a `che` CDN without a `keys` size) the paper load counts every read as
// a miss: the origin behind it is checked against its worst case. The line says so, or a student
// would read a red database as a real problem of the design. (The shortener's cache becomes a
// CDN in front of url_db, which it fetches its own misses from.)
TEST_CASE("explain annotates checks that count an unknown hit ratio as zero", "[report]") {
  auto s = che_cached();
  s.nodes[1].ops["redirect"] = {Step{.to = "url_cache", .op = "read"}};
  s.nodes[2].type = NodeType::cdn;
  s.nodes[2].keys = std::nullopt;
  s.nodes[2].origin = "url_db";
  s.links.push_back(LinkSpec{.from = "url_cache", .to = "url_db"});
  const auto built = simsys::graph::Graph::build(s);
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  const auto text = simsys::report::render_explain(*built);
  CAPTURE(text);
  const auto db = check_line(text, "url_db");
  REQUIRE(db.contains("peak offered 1k/s"));
  REQUIRE(db.contains(" (hit ratio unknown: che counted as 0 — an upper bound)"));
  // The CDN's own load does not depend on its hit ratio: no note there.
  REQUIRE(check_line(text, "url_cache").contains("peak offered 1k/s"));
  REQUIRE_FALSE(check_line(text, "url_cache").contains("unknown"));
  // A resolved che cache has a number: no note anywhere.
  const auto resolved = simsys::report::render_explain(*simsys::graph::Graph::build(che_cached()));
  CAPTURE(resolved);
  REQUIRE_FALSE(resolved.contains("hit ratio unknown"));
}

// A cache's write strategy acts on the ops in `write_ops` (default: `write`). When no op that
// reaches the cache is one of them, the strategy never acts on anything: a note, not an error
// (a read-only cache is legitimate, but a misnamed op is a common slip).
TEST_CASE("explain notes write_ops that never arrive at the cache", "[report]") {
  const auto text = simsys::report::render_explain(*simsys::graph::Graph::build(che_cached()));
  CAPTURE(text);
  REQUIRE(text.contains("write_ops=write (never arrives)"));
  auto suggestions = browser_cached();
  suggestions.nodes[1].write_ops = {"suggest"};  // an op that does arrive
  const auto written = simsys::report::render_explain(*simsys::graph::Graph::build(suggestions));
  CAPTURE(written);
  REQUIRE(written.contains("write_ops=suggest"));
  REQUIRE_FALSE(written.contains("never arrives"));
}

// A che hit ratio is computed, not stated, so explain shows where its inputs came from: the
// key distribution, and which client sent it.
TEST_CASE("explain prints a che hit ratio with its provenance", "[report]") {
  Scenario s{.name = "che", .version = 1, .duration = 1s};
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{100}},
                                           .op_shares = {{"get", 1.0}},
                                           .keys = KeySpec{.zipf = 1.0, .space = 10'000}}},
             NodeSpec{.id = "web",
                      .type = NodeType::service,
                      .ops = {{"get", {Step{.to = "cache", .op = "read"}}}}},
             NodeSpec{.id = "cache",
                      .type = NodeType::cache,
                      .hit_ratio = simsys::model::HitRatio{.che = true},
                      .strategy = simsys::model::CacheStrategy::cache_aside,
                      .keys = 1000}};
  s.links = {LinkSpec{.from = "u", .to = "web"}, LinkSpec{.from = "web", .to = "cache"}};
  const auto built = simsys::graph::Graph::build(s);
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  const auto text = simsys::report::render_explain(*built);
  CAPTURE(text);
  const auto number_at = text.find("hit_ratio=0.");
  const auto provenance_at = text.find(" (che: zipf 1 over 10k keys from u)");
  REQUIRE(number_at != std::string::npos);
  REQUIRE(provenance_at != std::string::npos);
  REQUIRE(provenance_at > number_at);
  // Between "hit_ratio=" and the provenance there is only the number: "0." and digits.
  const auto number = text.substr(number_at + 10, provenance_at - (number_at + 10));
  REQUIRE(std::ranges::all_of(number, [](char c) { return c == '.' || (c >= '0' && c <= '9'); }));
}

// ---- Che provenance: partitions, caches behind caches, large key spaces ----

namespace {
// u (the given keys) -> lb (`policy`) -> cache (che, 10k keys, 4 replicas).
Scenario replicated_che(simsys::model::LbPolicy policy) {
  Scenario s{.name = "replicated", .version = 1, .duration = 1min};
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{100}},
                                           .op_shares = {{"get", 1.0}},
                                           .keys = KeySpec{.zipf = 1.0, .space = 100'000}}},
             NodeSpec{.id = "lb", .type = NodeType::load_balancer, .policy = policy},
             NodeSpec{.id = "cache",
                      .type = NodeType::cache,
                      .replicas = 4,
                      .hit_ratio = simsys::model::HitRatio{.che = true},
                      .strategy = simsys::model::CacheStrategy::cache_aside,
                      .keys = 10'000}};
  s.links = {LinkSpec{.from = "u", .to = "lb"}, LinkSpec{.from = "lb", .to = "cache"}};
  return s;
}
// u (zipf 0.9 over 10M) -> l1 (`l1_ratio`, read_through) -> l2 (che, read_through) -> db.
Scenario cache_behind_cache(simsys::model::HitRatio l1_ratio) {
  Scenario s{.name = "c2c", .version = 1, .duration = 1min};
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{100}},
                                           .op_shares = {{"get", 1.0}},
                                           .keys = KeySpec{.zipf = 0.9, .space = 10e6}}},
             NodeSpec{.id = "l1",
                      .type = NodeType::cache,
                      .backend = "l2",
                      .hit_ratio = l1_ratio,
                      .strategy = simsys::model::CacheStrategy::read_through,
                      .keys = 2'000'000},
             NodeSpec{.id = "l2",
                      .type = NodeType::cache,
                      .backend = "db",
                      .hit_ratio = simsys::model::HitRatio{.che = true},
                      .strategy = simsys::model::CacheStrategy::read_through,
                      .keys = 2'000'000},
             NodeSpec{.id = "db", .type = NodeType::service}};
  s.links = {LinkSpec{.from = "u", .to = "l1"}, LinkSpec{.from = "l1", .to = "l2"},
             LinkSpec{.from = "l2", .to = "db"}};
  return s;
}
std::string explain_of(const Scenario& s) {
  const auto built = simsys::graph::Graph::build(s);
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  return simsys::report::render_explain(*built);
}
}  // namespace

// The provenance names the capacity the ring multiplied, and that its arcs are uneven, so a
// reader can see why the number differs from what the cache's own `keys` suggests.
TEST_CASE(
    "explain shows a ring-partitioned che cache's capacity as keys × replicas over uneven arcs",
    "[report]") {
  const auto ring_text = explain_of(replicated_che(simsys::model::LbPolicy::consistent_hash));
  CAPTURE(ring_text);
  REQUIRE(
      ring_text.contains(" (che: zipf 1 over 100k keys from u; 10k keys x 4 partitioned, "
                         "uneven arcs)"));
  const auto round_robin_text = explain_of(replicated_che(simsys::model::LbPolicy::round_robin));
  CAPTURE(round_robin_text);
  REQUIRE(round_robin_text.contains(" (che: zipf 1 over 100k keys from u)"));
}

TEST_CASE("explain names the cache whose misses a che cache's keys are", "[report]") {
  const auto text = explain_of(cache_behind_cache(simsys::model::HitRatio{.che = true}));
  CAPTURE(text);
  REQUIRE(text.contains(" (che: zipf 0.9 over 10M keys from u, filtered by l1)"));
}

TEST_CASE("explain says a che cache behind a fixed-ratio cache uses the unfiltered keys",
          "[report]") {
  const auto text = explain_of(cache_behind_cache(simsys::model::HitRatio{.fixed = 0.5}));
  CAPTURE(text);
  REQUIRE(
      text.contains(" (che: zipf 0.9 over 10M keys from u; fed through fixed-ratio cache "
                    "'l1': treated as unfiltered)"));
}

// The scenario grammar's largest count suffix is G; a key space past 9999G is written out in
// digits, as a count a reader can compare, rather than in scientific notation.
TEST_CASE("explain writes a key space beyond the G prefix in plain digits", "[report]") {
  auto s = replicated_che(simsys::model::LbPolicy::round_robin);
  s.nodes[0].workload->keys = KeySpec{.zipf = std::nullopt, .space = 2e13};
  const auto text = explain_of(s);
  CAPTURE(text);
  REQUIRE(text.contains(" (che: uniform over 20000000000000 keys from u)"));
}

// ---- SQL database: staleness SLO and explain line (plan 2b, Task 4) ----

namespace {
// u(1000 reads/s + 100 writes/s) -> db: a SQL database of 3 copies (1 primary + 2 replicas),
// capacity 1000/s per copy, 1 ms service time, a replication lag of 50 ms, zero-latency link.
// A `write` costs 1 unit (stated: the database default is 5), so loads are requests: the primary
// runs at 100/s (rho 0.1) and each replica at 500 + 100 = 600/s (rho 0.6). The staleness the
// node reports is then 50 ms + the write's sojourn at the primary (1.11 ms) and at a replica
// (2.5 ms): M/M/1 sojourns rho / (1 − rho) × s + s.
Scenario replicated_db() {
  Scenario s{.name = "replicated", .version = 1, .duration = 2s, .warmup = 500ms};
  NodeSpec db{
      .id = "db", .type = NodeType::sql_db, .replicas = 3, .capacity = Rate{1000}, .latency = 1ms};
  db.replication_lag = 50ms;
  db.cost = {{"write", 1.0}};
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{1100}},
                                           .op_shares = {{"read", 1000.0 / 1100.0},
                                                         {"write", 100.0 / 1100.0}}}},
             std::move(db)};
  s.links = {LinkSpec{.from = "u", .to = "db", .latency = 0s}};
  return s;
}

// The number after the first `"key": ` in `json` (the first node that has the key: a database's,
// as the other nodes print null for these).
double json_value(const std::string& json, std::string_view key) {
  const std::string field = "\"" + std::string{key} + "\": ";
  auto at = json.find(field);
  while (at != std::string::npos && json.compare(at + field.size(), 4, "null") == 0) {
    at = json.find(field, at + 1);
  }
  REQUIRE(at != std::string::npos);
  return std::stod(json.substr(at + field.size()));
}
}  // namespace

// The staleness SLO bounds how old the data a read may return is: the replication lag plus the
// primary's write sojourn (book ch. 1: replicas lag behind the primary). 50 ms of lag fits a
// 100 ms bound and breaks a 40 ms one, whatever the write sojourn adds.
TEST_CASE("the staleness SLO compares the replication lag plus write sojourn with max",
          "[report]") {
  auto s = replicated_db();
  const auto check = [&](simsys::SimTime max) {
    s.slos = {simsys::model::SloSpec{
        .metric = simsys::model::SloMetric::staleness, .node = "db", .max = max}};
    const auto g = *simsys::graph::Graph::build(s);
    const auto results = simsys::report::evaluate_slos(summarize(s, g), s.slos);
    REQUIRE(results.size() == 1);
    return results[0];
  };
  SECTION("100 ms passes") {
    const auto result = check(100ms);
    REQUIRE(result.status == simsys::report::SloStatus::pass);
    REQUIRE(result.observed.has_value());
    REQUIRE(*result.observed > 0.050);
    REQUIRE(*result.observed < 0.100);
  }
  SECTION("40 ms fails") {
    REQUIRE(check(40ms).status == simsys::report::SloStatus::fail);
  }
  SECTION("the JSON carries staleness_s for the database, with its value") {
    s.slos = {};
    const auto g = *simsys::graph::Graph::build(s);
    const auto summary = summarize(s, g);
    const auto json =
        simsys::report::render_json(summary, simsys::report::evaluate_slos(summary, s.slos));
    CAPTURE(json);
    // M/M/1 sojourn rho / (1 − rho) × s + s at the primary (rho 0.1) and at a replica (0.6).
    const double primary = 0.1 / 0.9 * 0.001 + 0.001;
    const double replica = 0.6 / 0.4 * 0.001 + 0.001;
    REQUIRE_THAT(json_value(json, "staleness_s"), WithinRel(0.050 + primary + replica, 1e-9));
    REQUIRE(json_value(json, "replication_dropped_per_s") == 0.0);
  }
}

// A replica that cannot keep up with the writes it replays drops some of them, and from then on
// it never holds what the primary holds: no staleness bound means anything (review of 48d4d0b,
// C1). 2 copies, 800 reads/s + 400 writes/s at 1000/s per copy: the replica is sent 1200/s,
// fills its queue by 5 s and then drops 200/s in its queue's mix, a third of it (66.7/s)
// replicated writes. The staleness SLO fails on them whatever the mean staleness (here within
// its 10 s bound), and says why;
// the table and the JSON carry the number.
TEST_CASE("the staleness SLO fails when a replica drops replicated writes", "[report]") {
  auto s = replicated_db();
  s.duration = 10s;
  s.warmup = 6s;  // after the replica's queue is full: a steady 200/s of drops
  s.nodes[0].workload =
      Workload{.curve = {.base = Rate{1200}},
               .op_shares = {{"read", 800.0 / 1200.0}, {"write", 400.0 / 1200.0}}};
  s.nodes[1].replicas = 2;
  s.slos = {simsys::model::SloSpec{
      .metric = simsys::model::SloMetric::staleness, .node = "db", .max = 10s}};
  const auto g = *simsys::graph::Graph::build(s);
  const auto summary = summarize(s, g);
  // The mean staleness (about 1.05 s: the replica's full queue drains in 1 s) is within 10 s.
  REQUIRE(*summary.nodes()[1].mean_staleness_s < 10.0);
  REQUIRE_THAT(summary.nodes()[1].mean_replication_dropped_per_s.value(),
               WithinRel(200.0 * 400.0 / 1200.0, 1e-6));
  const auto results = simsys::report::evaluate_slos(summary, s.slos);
  REQUIRE(results[0].status == simsys::report::SloStatus::fail);
  REQUIRE(results[0].note == "replica diverges: drops 66.67/s replicated writes");
  const auto table = simsys::report::render_table(summary, results, false);
  CAPTURE(table);
  REQUIRE(table.contains("replica diverges: drops 66.67/s replicated writes"));
  const auto json = simsys::report::render_json(summary, results);
  CAPTURE(json);
  REQUIRE_THAT(json_value(json, "replication_dropped_per_s"), WithinRel(200.0 / 3.0, 1e-6));
  REQUIRE(json.contains("\"note\": \"replica diverges: drops 66.67/s replicated writes\""));
}

// Shards × replicas (review of 48d4d0b): explain's total counts N × S copies, 3 × 2 of 1k/s, and
// its per-copy split divides each load by the shards: one primary is sent 100 / 2 = 50 writes/s,
// one replica 1000 / (2 × 2) + 100 / 2 = 300/s.
TEST_CASE("explain counts every copy of every shard and splits the load per copy", "[report]") {
  auto s = replicated_db();
  s.nodes[1].shards = 2;
  const auto text = simsys::report::render_explain(*simsys::graph::Graph::build(s));
  CAPTURE(text);
  REQUIRE(text.contains("vs 6k/s total capacity"));
  REQUIRE(
      text.contains("primary 50/s vs 1k/s (5%, green), each replica 300/s vs 1k/s (30%, green)"));
}

// The database driver, not the balancer, decides where a query goes (writes to the primary,
// reads to a replica), so a hash ring in front of a database imposes no hot copy: explain
// splits its load like any database's and has no hottest-copy line (review of 48d4d0b, I1).
TEST_CASE("a hash ring in front of a database leaves its copies even in explain", "[report]") {
  auto s = replicated_db();
  s.nodes.push_back(NodeSpec{.id = "ring",
                             .type = NodeType::load_balancer,
                             .policy = simsys::model::LbPolicy::consistent_hash,
                             .vnodes = 1});
  s.links = {LinkSpec{.from = "u", .to = "ring", .latency = 0s},
             LinkSpec{.from = "ring", .to = "db", .latency = 0s}};
  const auto text = explain_of(s);
  CAPTURE(text);
  REQUIRE_FALSE(text.contains("hottest copy"));
  REQUIRE(text.contains("each replica 600/s vs 1k/s (60%, green)"));
}

// A database's total capacity counts primaries that serve no reads, so its average can look
// green while every replica is red: the headline's zone is the busiest copy's. 2 copies, 1000
// reads/s + 100 writes/s: 1.1k/s of 2k/s is 55 % on average, but the replica carries all 1.1k/s
// against 1k/s, 110 %, red.
TEST_CASE("explain gives a database the zone of its busiest copy", "[report]") {
  auto s = replicated_db();
  s.nodes[1].replicas = 2;
  const auto text = explain_of(s);
  CAPTURE(text);
  REQUIRE(text.contains("vs 2k/s total capacity  (55%, red at the busiest copy)"));
  REQUIRE(text.contains("each replica 1.1k/s vs 1k/s (110%, red)"));
}

// `replication_lag` describes how far replicas run behind; with `replicas: 1` there is none, so
// the lag is accepted (a scenario may keep it while trying one copy) and marked unused.
TEST_CASE("explain marks a replication lag without replicas as unused", "[report]") {
  auto s = replicated_db();
  s.nodes[1].replicas = 1;
  const auto text = explain_of(s);
  CAPTURE(text);
  REQUIRE(text.contains("lag=50ms (unused: no replicas)"));
}

// Little's law: a copy has at most `pool` queries in service, each for one service time, so it
// serves at most pool / service time. 4 connections of 10 ms are 400/s: explain shows the bound
// and where it came from. A pool above the concurrency the disk implies (5k/s × 150 us < 1) binds
// nothing, and c stays implied.
TEST_CASE("explain shows the throughput a small pool allows", "[report]") {
  auto s = replicated_db();
  SECTION("pool 4 of 10 ms queries") {
    s.nodes[1].capacity = Rate{5000};
    s.nodes[1].latency = 10ms;
    s.nodes[1].pool = 4;
    const auto text = explain_of(s);
    CAPTURE(text);
    REQUIRE(text.contains("capacity=400/s (pool 4 / 10ms)"));
    REQUIRE(text.contains("c=4 (pool)"));
  }
  SECTION("a pool that binds nothing") {
    s.nodes[1].pool = 32;
    const auto text = explain_of(s);
    CAPTURE(text);
    REQUIRE(text.contains("capacity=1k/s (scenario)"));
    REQUIRE(text.contains("c=1 (implied)"));
    REQUIRE(text.contains("pool=32"));
  }
}

TEST_CASE("explain prints a sql database as primary plus replicas with shards, lag and pool",
          "[report]") {
  auto s = replicated_db();
  s.nodes[1].shards = 4;
  s.nodes[1].pool = 32;
  const auto built = simsys::graph::Graph::build(s);
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  const auto text = simsys::report::render_explain(*built);
  CAPTURE(text);
  REQUIRE(text.find("primary+2 replicas") != std::string::npos);
  REQUIRE(text.find("shards=4") != std::string::npos);
  REQUIRE(text.find("lag=50ms") != std::string::npos);
  REQUIRE(text.find("pool=32") != std::string::npos);
}

// ---- Request-based CDN (plan 2b refinement 4, Task 5) ----

namespace {
// u(1k/s read) -> `cdn_id` (a CDN, fixed hit ratio 0.95, origin `origin`) -> origin (a service of
// 5k/s). The CDN has the given byte capacity when one is passed.
Scenario cdn_reported(std::string cdn_id = "cdn", std::optional<simsys::ByteRate> bytes = {}) {
  Scenario s{.name = "cdn-report", .version = 1, .duration = 1min};
  NodeSpec cdn{.id = cdn_id,
               .type = NodeType::cdn,
               .byte_capacity = bytes,
               .hit_ratio = simsys::model::HitRatio{.fixed = 0.95}};
  cdn.origin = "origin";
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{1000}}, .op_shares = {{"read", 1.0}}}},
      std::move(cdn), NodeSpec{.id = "origin", .type = NodeType::service, .capacity = Rate{5000}}};
  s.links = {LinkSpec{.from = "u", .to = cdn_id}, LinkSpec{.from = cdn_id, .to = "origin"}};
  return s;
}
}  // namespace

// Explain shows a CDN as it shows a cache: the hit ratio and where it comes from, and, because
// a CDN forwards its own misses, where they go. The exact wording ("misses → origin", after the
// hit ratio and before the ttl) is this test's proposal; the implementer may adjust it and move
// these expectations with it.
TEST_CASE("explain shows a cdn's hit ratio and where its misses go", "[report]") {
  const auto built = simsys::graph::Graph::build(cdn_reported());
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  const auto text = simsys::report::render_explain(*built);
  CAPTURE(text);
  REQUIRE(text.contains("hit_ratio=0.95 (fixed)"));
  REQUIRE(text.contains("misses → origin"));
}

// The paper load follows the miss edge: the origin is checked against 5 % of the 1k/s reads
// (1 - 0.95 = 0.05, so 50/s), not against zero (an edge that explain ignored) or all 1k/s.
TEST_CASE("explain's paper load sends the origin the cdn's miss share", "[report]") {
  const auto text = simsys::report::render_explain(*simsys::graph::Graph::build(cdn_reported()));
  CAPTURE(text);
  REQUIRE(check_line(text, "cdn").contains("peak offered 1k/s"));
  REQUIRE(check_line(text, "origin").contains("peak offered 50/s"));
}

// A CDN sized in bytes (`capacity: 100GB/s`) builds and explains; only the engine refuses to run
// it until phase 2c models byte flows, as it refuses a cache without a hit ratio.
TEST_CASE("explain still renders a cdn that has a byte capacity", "[report]") {
  const auto built = simsys::graph::Graph::build(cdn_reported("cdn_us", simsys::ByteRate{100e9}));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  const auto text = simsys::report::render_explain(*built);
  CAPTURE(text);
  REQUIRE(text.contains("cdn_us"));
  REQUIRE(text.contains("hit_ratio=0.95 (fixed)"));
  REQUIRE(text.contains("misses → origin"));
}
