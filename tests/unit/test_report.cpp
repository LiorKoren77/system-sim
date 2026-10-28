// Summaries over a run, SLO evaluation and the three output formats.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.core;
import simsys.engine;
import simsys.graph;
import simsys.model;
import simsys.report;

using namespace std::chrono_literals;
using Catch::Approx;
using simsys::Rate;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
using simsys::model::Workload;
using simsys::report::Summary;

namespace {
// queue_limit is set only when given, so explain's provenance test sees "(default)".
Scenario mm1(double demand, std::optional<double> queue_limit = std::nullopt) {
  Scenario s{.name = "mm1", .version = 1, .duration = 2s, .warmup = 500ms};
  s.nodes = {NodeSpec{.id = "c",
                      .type = NodeType::client,
                      .workload = Workload{.base = Rate{demand}, .op_shares = {{"op", 1.0}}}},
             NodeSpec{.id = "s",
                      .type = NodeType::service,
                      .capacity = Rate{1000},
                      .latency = 1ms,
                      .advanced = {.queue_limit = queue_limit}}};
  s.links = {simsys::model::LinkSpec{
      .from = "c", .to = "s", .dir = simsys::model::Direction::inout, .latency = 0s}};
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

TEST_CASE("the series writer emits a header and one row per node per step", "[report]") {
  const auto s = mm1(500);
  const auto g = *simsys::graph::Graph::build(s);
  std::ostringstream out;
  simsys::report::SeriesWriter writer{out, g};
  auto sim = *simsys::engine::Simulation::create(g);
  sim.run({.duration = 300ms, .step = 100ms}, [&](const auto& r) { writer.observe(r); });
  const auto text = out.str();
  REQUIRE(text.starts_with("t_s,node,rho,backlog,drops_per_s,latency_ms,zone\n"));
  REQUIRE(std::ranges::count(text, '\n') == 1 + 3 * 2);
  REQUIRE(text.find("0.100,s,") != std::string::npos);
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
