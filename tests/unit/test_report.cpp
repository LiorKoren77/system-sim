// Summaries over a run, SLO evaluation and the three output formats.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>  // getpid

#include "../support/explain_lines.hpp"

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.config;
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
using simsys::test_support::check_line;
using simsys::test_support::node_line;

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
  REQUIRE(results[2].status == SloStatus::fail);  // rho 0.5 > 0.3
  // queue_lag names a service, which has no queue lag: nothing to observe, so it is skipped (the
  // loader refuses such a file: "slo: queue_lag needs a message_queue node", plan 2c Task 8).
  REQUIRE(results[3].status == SloStatus::skipped);
  REQUIRE_FALSE(results[3].observed.has_value());
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
  REQUIRE(
      text.starts_with("t_s,node,demand_per_s,drain_per_s,rho,backlog,drops_per_s,rejected_per_s,"
                       "latency_ms,zone,queue_lag_s\n"));
  // C++20 std::ranges::count: counts the matches in a whole range (here the characters of
  // the string), replacing std::count(text.begin(), text.end(), '\n').
  REQUIRE(std::ranges::count(text, '\n') == 1 + 3 * 2);
  REQUIRE(text.find("\n0.1,s,") != std::string::npos);  // shortest round-trip form of 0.1 s
  // A client is a source: its numeric columns are 0 (machines keep a number) and its zone
  // is `none` (it has no queue to colour).
  REQUIRE(text.find("\n0,c,0,0,0,0,0,0,0,none,\n") != std::string::npos);  // no lag: empty cell
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

// The loop's totals grow every pass until they overflow, so the last finite pass printed
// "peak offered 5.618e+307/s" (review of plan 2c Task 7, minor 6): a number no reader can use.
// Every node whose load was still growing when the passes stopped is "unbounded" instead: a and b
// here, both on the loop.
TEST_CASE("explain prints the nodes of a loop of gain >= 1 as unbounded, not as a huge number",
          "[report]") {
  const auto text = simsys::report::render_explain(*simsys::graph::Graph::build(loop(2.0)));
  CAPTURE(text);
  REQUIRE(check_line(text, "a").ends_with(
      "peak offered unbounded vs 10k/s total capacity  (no steady state, red)"));
  REQUIRE(check_line(text, "b").ends_with(
      "peak offered unbounded vs 10k/s total capacity  (no steady state, red)"));
  REQUIRE_FALSE(text.contains("e+30"));
}

// A bandwidth link out of such a loop carries what its sender sends, which has no figure either
// (review of plan 2c Task 8, minor 1: its line printed "8.988e+307 MB/s"). a sends b `job`s of
// 1 MB over 1 GB/s; the line says unbounded, as the nodes' do.
TEST_CASE("explain prints a bandwidth link fed by a loop of gain >= 1 as unbounded", "[report]") {
  auto s = loop(2.0);
  s.nodes[1].ops.at("msg").front().size = simsys::Bytes{1e6};
  s.links[1].bandwidth = simsys::ByteRate{1e9};
  const auto text = simsys::report::render_explain(*simsys::graph::Graph::build(s));
  CAPTURE(text);
  REQUIRE(check_line(text, "a->b")
              .ends_with("peak offered unbounded vs 1k MB/s  (no steady state, red)"));
  REQUIRE_FALSE(text.contains("e+30"));
}

// A loop of gain 0.9999 has a fixed point, a = 100 / (1 - 0.9999) = 1M/s, but the passes reach it
// only after about log(1e-12) / log(0.9999) = 276,000 laps, beyond the 100,000-pass cap. Its
// increments still shrink by 0.9999 a pass, so the paper load sums the rest of the geometric
// series instead of calling the loop unbounded (review of plan 2c Task 8, minor 2): a is offered
// 1M/s, b 0.9999 x that, and no "no steady state" note.
TEST_CASE("explain sums a slowly converging loop to its fixed point rather than call it unbounded",
          "[report]") {
  const auto text = simsys::report::render_explain(*simsys::graph::Graph::build(loop(0.9999)));
  CAPTURE(text);
  REQUIRE(check_line(text, "a").contains("peak offered 1M/s vs 10k/s total capacity"));
  REQUIRE(check_line(text, "b").contains("peak offered 999.9k/s vs 10k/s total capacity"));
  REQUIRE_FALSE(text.contains("unbounded"));
  REQUIRE_FALSE(text.contains("no steady state"));
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
  // eleven cells: demand_per_s and drain_per_s after node (plan 2b ref. 7), queue_lag_s last (plan
  // 2c Task 8). getline drops the last one, which is empty on a limiter's row, so the comma count
  // is what says there are eleven.
  REQUIRE(std::ranges::count(line, ',') == 10);
  REQUIRE(fields.size() == 10);
  REQUIRE(fields[6] == "0");     // drops_per_s
  REQUIRE(fields[7] == "3000");  // rejected_per_s
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
// `counters` a cache that answers every request (fixed hit ratio 1.0, cache-aside) and counts
// `counter` as a write (an INCR; Graph::build refuses a store that counts it as a read).
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
               .strategy = simsys::model::CacheStrategy::cache_aside,
               .write_ops = std::vector<std::string>{"counter"}}};
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
    REQUIRE(text.find("hit_ratio=1 (fixed) strategy=cache_aside write_ops=counter") !=
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
          "t_s,node,demand_per_s,drain_per_s,rho,backlog,drops_per_s,rejected_per_s,latency_ms,"
          "zone,queue_lag_s\n");
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
// CDN in front of url_db, which it fetches its own misses from.) The note does not say "che":
// a cache built in code with no hit ratio at all gets it too (below).
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
  REQUIRE(db.contains(" (hit ratio unknown: counted as 0 — an upper bound)"));
  // The CDN's own load does not depend on its hit ratio: no note there.
  REQUIRE(check_line(text, "url_cache").contains("peak offered 1k/s"));
  REQUIRE_FALSE(check_line(text, "url_cache").contains("unknown"));
  // A resolved che cache has a number: no note anywhere.
  const auto resolved = simsys::report::render_explain(*simsys::graph::Graph::build(che_cached()));
  CAPTURE(resolved);
  REQUIRE_FALSE(resolved.contains("hit ratio unknown"));
}

// The other road to an unknown h: a cache built in code with no hit ratio (the loader requires
// one; the engine refuses to run it). A service's `on: miss` step after it is taken by every
// request on paper, so the database behind the step is an upper bound too, and says so (review
// of 4d539b0, M5: the CDN case above goes through the origin edge, not a service's miss step).
TEST_CASE("explain marks the target of a miss step after a cache with no hit ratio", "[report]") {
  auto s = che_cached();
  s.nodes[2].hit_ratio = std::nullopt;
  s.nodes[2].keys = std::nullopt;
  const auto built = simsys::graph::Graph::build(s);
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  const auto text = simsys::report::render_explain(*built);
  CAPTURE(text);
  const auto db = check_line(text, "url_db");
  REQUIRE(db.contains("peak offered 1k/s"));
  REQUIRE(db.contains(" (hit ratio unknown: counted as 0 — an upper bound)"));
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
    // The primary commits all 100 writes/s; each replica replays them.
    REQUIRE_THAT(json_value(json, "committed_writes_per_s"), WithinRel(100.0, 1e-9));
  }
}

// A replica that cannot keep up with the writes it replays falls behind without bound: a real
// one keeps the excess in its relay log (MySQL) or WAL (Postgres) and its lag grows until it is
// rebuilt; the model drops the excess at queue_limit, so its staleness stays finite. No staleness
// bound holds either way (review of 48d4d0b, C1; re-review of 96848f0). 2 copies, 800 reads/s +
// 400 writes/s at 1000/s per copy: the primary commits all 400 writes (rho 0.4); the replica is
// sent 1200/s, fills its queue by 5 s and then drops 200/s in its queue's mix, a third of it
// (66.67/s) replicated writes, 66.67 / 400 = 16.7 % of the committed writes. The staleness SLO
// fails on them whatever the mean staleness (here within its 10 s bound), and says why; the
// table and the JSON carry both numbers.
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
  REQUIRE_THAT(summary.nodes()[1].mean_committed_writes_per_s.value(), WithinRel(400.0, 1e-6));
  const auto results = simsys::report::evaluate_slos(summary, s.slos);
  REQUIRE(results[0].status == simsys::report::SloStatus::fail);
  const std::string note = "each replica misses 66.67/s of 400/s committed writes (16.7%)";
  REQUIRE(results[0].note == note);
  const auto table = simsys::report::render_table(summary, results, false);
  CAPTURE(table);
  REQUIRE(table.contains(note));
  const auto json = simsys::report::render_json(summary, results);
  CAPTURE(json);
  REQUIRE_THAT(json_value(json, "replication_dropped_per_s"), WithinRel(200.0 / 3.0, 1e-6));
  REQUIRE_THAT(json_value(json, "committed_writes_per_s"), WithinRel(400.0, 1e-6));
  REQUIRE(json.contains("\"note\": \"" + note + "\""));
}

// The missed writes are ONE replica's, against the writes ONE replica must replay (re-review of
// 96848f0): summed over replicas they exceeded the writes committed (2131/s "dropped" of 900/s).
// 6 copies (a primary and 5 replicas), 5000 reads/s + 900 writes/s at cost 1, 1000/s per copy:
// the primary commits all 900 writes (rho 0.9); each replica is sent 5000 / 5 = 1000 reads + 900
// replayed writes = 1900/s against 1000/s and drops 900/s in its queue's mix, 900 × 900/1900 =
// 426.3/s of them replicated writes: 47.4 % of the 900/s committed. (The five together: 2132/s.)
// The replication section shows both numbers per replica and per shard.
TEST_CASE("the staleness note counts one replica's missed writes against the committed writes",
          "[report]") {
  auto s = replicated_db();
  s.duration = 10s;
  s.warmup = 6s;  // the replicas' queues are full after about 1.1 s
  s.nodes[0].workload =
      Workload{.curve = {.base = Rate{5900}},
               .op_shares = {{"read", 5000.0 / 5900.0}, {"write", 900.0 / 5900.0}}};
  s.nodes[1].replicas = 6;
  s.slos = {simsys::model::SloSpec{
      .metric = simsys::model::SloMetric::staleness, .node = "db", .max = 100ms}};
  const auto g = *simsys::graph::Graph::build(s);
  const auto summary = summarize(s, g);
  REQUIRE_THAT(summary.nodes()[1].mean_replication_dropped_per_s.value(),
               WithinRel(900.0 * 900.0 / 1900.0, 1e-6));
  const auto results = simsys::report::evaluate_slos(summary, s.slos);
  REQUIRE(results[0].status == simsys::report::SloStatus::fail);
  REQUIRE(results[0].note == "each replica misses 426.3/s of 900/s committed writes (47.4%)");
  const auto table = simsys::report::render_table(summary, results, false);
  CAPTURE(table);
  REQUIRE(table.contains("committed writes/s per shard"));
  REQUIRE(table.contains("missed writes/s per replica"));
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
      text.contains("primary 50 units/s vs 1k/s (5%, green), each replica 300 units/s vs 1k/s "
                    "(30%, green)"));
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
  REQUIRE(text.contains("each replica 600 units/s vs 1k/s (60%, green)"));
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
  REQUIRE(text.contains("each replica 1.1k units/s vs 1k/s (110%, red)"));
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

// A database's write op costs 5 capacity units unless `cost` lists it (spec §6: one SQL machine
// does "5k reads" or "1k writes" per second), and a default that changes the load fivefold must be
// visible: explain prints each defaulted write op that reaches the database with its cost and
// where the 5 comes from, beside the costs the scenario states (re-review of 96848f0). The
// check's per-copy numbers are then capacity units, not requests, and say so: 1000 reads/s and
// 100 writes/s × 5 = 500 units/s; the primary carries 500, each of the 2 replicas
// 1000 / 2 + 500 = 1000 units/s against 1k/s.
TEST_CASE("explain prints a database's defaulted write cost and labels its checks in units",
          "[report]") {
  auto s = replicated_db();
  s.nodes[1].cost = {};
  SECTION("only the default") {
    const auto text = explain_of(s);
    CAPTURE(text);
    REQUIRE(text.contains("  cost: write 5 (default: spec §6 5k reads/1k writes)"));
    REQUIRE(text.contains(
        "primary 500 units/s vs 1k/s (50%, green), each replica 1k units/s vs 1k/s (100%, red)"));
    // The headline is in units too (1000 reads + 100 writes × 5), labelled as the copies are.
    REQUIRE(text.contains("peak offered 1.5k units/s vs 3k/s total capacity"));
  }
  SECTION("several defaulted write ops name the source once") {
    s.nodes[0].workload->op_shares = {
        {"read", 1000.0 / 1100.0}, {"insert", 50.0 / 1100.0}, {"update", 50.0 / 1100.0}};
    const auto text = explain_of(s);
    CAPTURE(text);
    REQUIRE(text.contains("  cost: insert 5, update 5 (defaults: spec §6 5k reads/1k writes)"));
  }
  SECTION("beside a stated cost") {
    // The stated costs first, then the defaulted ones with their source: the source belongs to
    // the group after the semicolon only.
    s.nodes[1].cost = {{"read", 2.0}};
    const auto text = explain_of(s);
    CAPTURE(text);
    REQUIRE(text.contains("  cost: read 2; write 5 (default: spec §6 5k reads/1k writes)"));
  }
  SECTION("a stated write cost is not a default") {
    s.nodes[1].cost = {{"write", 1.0}};
    const auto text = explain_of(s);
    CAPTURE(text);
    REQUIRE(text.contains("  cost: write 1"));
    REQUIRE_FALSE(text.contains("(default: spec §6"));
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
// u(1k/s, the given ops: `read` by default) -> `cdn_id` (a CDN, fixed hit ratio 0.95, origin
// `origin`) -> origin (a service of 5k/s). The CDN has the given byte capacity when one is passed.
// Every op is 1 MB, the object size 2b assumed: a byte-sized node refuses an unsized request
// (plan 2c refinement 1), and at that size one request is one unit of a node counting megabytes
// (plan 2c refinement 2), so the numbers below read the same in requests and in MB.
Scenario cdn_reported(std::string cdn_id = "cdn", std::optional<simsys::ByteRate> bytes = {},
                      std::vector<std::pair<std::string, double>> ops = {{"read", 1.0}}) {
  Scenario s{.name = "cdn-report", .version = 1, .duration = 1min};
  std::map<std::string, simsys::Bytes> sizes;
  for (const auto& op_share : ops) sizes.emplace(op_share.first, simsys::Bytes{1e6});
  NodeSpec cdn{.id = cdn_id,
               .type = NodeType::cdn,
               .byte_capacity = bytes,
               .hit_ratio = simsys::model::HitRatio{.fixed = 0.95}};
  cdn.origin = "origin";
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{1000}},
                                           .op_shares = std::move(ops),
                                           .sizes = std::move(sizes)}},
             std::move(cdn),
             NodeSpec{.id = "origin", .type = NodeType::service, .capacity = Rate{5000}}};
  s.links = {LinkSpec{.from = "u", .to = cdn_id}, LinkSpec{.from = cdn_id, .to = "origin"}};
  return s;
}
}  // namespace

// Explain shows a CDN as it shows a cache: the hit ratio and where it comes from, and, because
// a CDN forwards its own misses, where they go ("misses → origin").
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

// A CDN sized in bytes (`capacity: 100GB/s`) builds and explains with its hit ratio and its miss
// edge, as one sized in requests does; it counts megabytes (plan 2c refinement 2), which the
// checks below pin.
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

// ---- kv_cluster's check line and node line (plan 2c refinement 9, Task 10; book ch. 6) ----
//
// A kv_cluster is `nodes` machines of `capacity` each, every request going to N of them, so the
// paper load it is offered is N times the requests, and its total capacity `nodes` times one
// node's. The check line says both: "peak offered 90k/s (30k/s x N 3) vs 80k/s (8 nodes)", the
// requests beside the replica-requests, the machines beside the total. Behind a ring (N < nodes)
// the average hides the node that breaks first, so the verdict is the HOTTEST node's, k = 1 +
// z_M/sqrt(N vnodes) times the average (the balancer's ring heuristic, z_M the expected maximum of
// M standard normals: 1.0676 for 8 machines at N = 3 and the default 150 virtual nodes, 1.0834
// for 16): "(hottest 120%, red)"; with N = nodes every node holds every key and the verdict is the
// plain average. This replaces the phase-2c "not checked: kv_cluster nodes, replication and
// quorums: phase 3" note, which said the numbers had no model to be checked against.

namespace {
// u(`rate`/s of `get`) -> store, a kv_cluster of `nodes` machines of 10k/s each keeping N = 3
// copies (Dynamo's default; the other keys are left to their defaults).
Scenario kv_explained(int nodes, double rate) {
  Scenario s{.name = "kv", .version = 1, .duration = 1min};
  NodeSpec store{.id = "store", .type = NodeType::kv_cluster, .capacity = Rate{10'000}};
  store.kv_nodes = nodes;
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{rate}}, .op_shares = {{"get", 1.0}}}},
      std::move(store)};
  s.links = {LinkSpec{.from = "u", .to = "store"}};
  return s;
}
}  // namespace

// nodes = 8: 30k requests/s x N 3 = 90k replica-requests/s over 8 x 10k = 80k/s: 112.5 % on
// average, and the hottest node k = 1.0676 x 11.25k = 12.01k/s: 120.1 %, printed to three digits
// as 120 %, red.
TEST_CASE("explain checks eight kv nodes at 30k requests a second: the hottest node is red",
          "[report]") {
  const auto text = explain_of(kv_explained(8, 30'000));
  CAPTURE(text);
  const auto line = check_line(text, "store");
  REQUIRE(
      line.contains("peak offered 90k/s (30k/s × N 3) vs 80k/s (8 nodes)  (hottest 120%, red)"));
  REQUIRE_FALSE(line.contains("not checked"));
}

// nodes = 16: the same 90k/s over 160k/s, 56.25 % on average, the hottest 1.0834 x that = 60.94 %:
// 60.9 %, green.
TEST_CASE("explain checks sixteen kv nodes at 30k requests a second: the hottest node is green",
          "[report]") {
  const auto text = explain_of(kv_explained(16, 30'000));
  CAPTURE(text);
  REQUIRE(check_line(text, "store")
              .contains("peak offered 90k/s (30k/s × N 3) vs 160k/s (16 nodes)  "
                        "(hottest 60.9%, green)"));
}

// A paper load far beyond capacity prints its percentage in full, not in exponent notation
// (review of plan 2c Tasks 9-10, minor 11): 1M requests/s x 3 over 3 x 10k = 30k/s is 10000 %,
// which three significant digits used to print as "1e+04%".
TEST_CASE("explain prints a percentage beyond a thousand in full", "[report]") {
  const auto text = explain_of(kv_explained(3, 1'000'000));
  CAPTURE(text);
  REQUIRE(check_line(text, "store").contains("(10000%, red)"));
  REQUIRE_FALSE(text.contains("e+0"));
}

// N = nodes = 3: every node holds every key, so the verdict is the plain average and no node is
// called the hottest: 1k requests/s x 3 = 3k replica-requests/s over 3 x 10k = 30k/s, 10 %.
TEST_CASE("explain gives a kv_cluster that replicates to every node a plain percentage",
          "[report]") {
  const auto text = explain_of(kv_explained(3, 1000));
  CAPTURE(text);
  const auto line = check_line(text, "store");
  REQUIRE(line.contains("peak offered 3k/s (1k/s × N 3) vs 30k/s (3 nodes)  (10%, green)"));
  REQUIRE_FALSE(line.contains("hottest"));
}

// The node line prints the resolved shape and what it does not yet model. Defaults applied
// (N = 3, W = 2, R = 2, ap: Dynamo's classic quorum, ch. 6), each marked "(default)" as explain
// marks every number the scenario did not state (review of plan 2c Tasks 9-10, minor 9).
TEST_CASE("explain prints a kv_cluster's nodes, replication, quorums and mode", "[report]") {
  SECTION("defaults for everything but the nodes") {
    const auto text = explain_of(kv_explained(3, 1000));
    CAPTURE(text);
    REQUIRE(node_line(text, "store")
                .contains("nodes=3 N=3 (default) W=2 (default) R=2 (default) mode=ap (default; "
                          "partitions: phase 3)"));
  }
  SECTION("stated values, cp mode") {
    auto s = kv_explained(5, 1000);
    s.nodes[1].replication = 5;
    s.nodes[1].write_quorum = 3;
    s.nodes[1].read_quorum = 3;
    s.nodes[1].kv_mode = simsys::model::KvMode::cp;
    const auto text = explain_of(s);
    CAPTURE(text);
    REQUIRE(node_line(text, "store").contains("nodes=5 N=5 W=3 R=3 mode=cp (partitions: phase 3)"));
  }
}

// W + R > N is what makes a quorum read see the latest write (ch. 6: a read quorum and a write
// quorum must share a replica). explain says when a scenario's quorums do not overlap, and that
// `mode: cp` (consistency over availability) asks for exactly that overlap, so cp with W + R <= N
// contradicts itself (review of plan 2c Tasks 9-10, minor 6). Nothing is modelled about it: the
// model has no versions of a key, only the time and the loss of a quorum.
TEST_CASE("explain warns when the read and write quorums do not overlap", "[report]") {
  auto s = kv_explained(3, 1000);
  s.nodes[1].replication = 3;
  s.nodes[1].write_quorum = 1;
  s.nodes[1].read_quorum = 2;
  s.nodes[1].kv_mode = simsys::model::KvMode::ap;
  SECTION("ap: the note alone") {
    const auto text = explain_of(s);
    CAPTURE(text);
    REQUIRE(node_line(text, "store")
                .contains("N=3 W=1 R=2 (W + R ≤ N: a read may miss the latest write) mode=ap "
                          "(partitions: phase 3)"));
  }
  SECTION("cp: and the contradiction") {
    s.nodes[1].kv_mode = simsys::model::KvMode::cp;
    const auto text = explain_of(s);
    CAPTURE(text);
    REQUIRE(
        node_line(text, "store")
            .contains(
                "mode=cp (partitions: phase 3; cp needs W + R > N: these quorums contradict it)"));
  }
  SECTION("overlapping quorums: no note") {
    s.nodes[1].read_quorum = 3;
    const auto text = explain_of(s);
    CAPTURE(text);
    REQUIRE_FALSE(node_line(text, "store").contains("W + R"));
  }
}

// Plan 2c refinement 2: a capacity stated in bytes is checked in bytes. The node counts
// megabytes, so its load is the requests' size summed, and the check line prints it in MB/s with
// the request rate beside it: 1k reads/s of 1 MB at a 100 GB/s cdn are 1k MB/s (1k/s) against
// 100k MB/s, 1 %; its 5 % misses, 50/s of 1 MB, are 50 MB/s at a blob store's default 500 MB/s
// (spec section 6), 10 %. The verdict is a real one (a zone), and nothing says "not checked".
TEST_CASE("explain checks a capacity stated in bytes in MB per second", "[report]") {
  SECTION("a cdn sized in bytes") {
    const auto text = explain_of(cdn_reported("cdn_us", simsys::ByteRate{100e9}));
    CAPTURE(text);
    const auto line = check_line(text, "cdn_us");
    REQUIRE(line.contains("peak offered 1k MB/s (1k/s) vs 100k MB/s total capacity  (1%, green)"));
    REQUIRE_FALSE(line.contains("not checked"));
  }
  SECTION("a blob store origin with its type's default capacity") {
    auto s = cdn_reported();
    s.nodes[2] = NodeSpec{.id = "origin", .type = NodeType::blob_store};
    const auto text = explain_of(s);
    CAPTURE(text);
    const auto line = check_line(text, "origin");
    REQUIRE(line.contains("peak offered 50 MB/s (50/s) vs 500 MB/s total capacity  (10%, green)"));
    REQUIRE_FALSE(line.contains("not checked"));
  }
  SECTION("a blob store origin sized in bytes") {
    auto s = cdn_reported();
    s.nodes[2] = NodeSpec{
        .id = "origin", .type = NodeType::blob_store, .byte_capacity = simsys::ByteRate{1e9}};
    const auto text = explain_of(s);
    CAPTURE(text);
    // 50 MB/s of 1000 MB/s: 5 %.
    REQUIRE(check_line(text, "origin")
                .contains("peak offered 50 MB/s (50/s) vs 1k MB/s total capacity  (5%, green)"));
  }
  SECTION("a capacity stated in requests is still checked in requests") {
    const auto text = explain_of(cdn_reported());
    CAPTURE(text);
    const auto line = check_line(text, "cdn");
    REQUIRE(line.contains("(1%, green)"));
    REQUIRE_FALSE(line.contains("MB/s"));
  }
}

// A CDN caches every op that reaches it and is not one of its write ops (model::write_ops_of:
// `write` unless the scenario lists its own `write_ops`). An `upload` a scenario forgot to declare
// a write would be answered from the edge for the hit share of it, so explain lists the ops the
// CDN serves from its cache: the slip is in plain sight (review of 4d539b0, I2).
TEST_CASE("explain lists the ops a cdn serves from its cache", "[report]") {
  const auto text = explain_of(cdn_reported("cdn", {}, {{"read", 0.8}, {"upload", 0.2}}));
  CAPTURE(text);
  REQUIRE(text.contains("serves: read, upload"));
}

// A CDN the scenario gave no hit ratio misses every read, and that is what runs (h = 0 is the
// scenario's statement, not a number nobody knows): its line says so, and the origin's check is
// a plain check, not an upper bound (review of 4d539b0, M4).
TEST_CASE("a cdn without a hit ratio is explained as h = 0, not as unknown", "[report]") {
  auto s = cdn_reported();
  s.nodes[1].hit_ratio = std::nullopt;
  const auto text = explain_of(s);
  CAPTURE(text);
  REQUIRE(text.contains("hit_ratio=none (every read misses: h = 0)"));
  const auto origin = check_line(text, "origin");
  REQUIRE(origin.contains("peak offered 1k/s"));
  REQUIRE_FALSE(origin.contains("unknown"));
}

// ---- ID generator as service sugar (plan 2b refinement 6; book ch. 7) ----
//
// explain prints the algorithm and what it stands for, so a student sees WHY the node has the
// capacity it has: a snowflake's 12 sequence bits, a ticket server's single database.

namespace {
// u (10/s of `next_id`) -> ids: an id_generator of the given algorithm, with the numbers the
// scenario states, if any.
Scenario id_reported(simsys::model::IdAlgorithm algorithm, int replicas = 1,
                     std::optional<Rate> capacity = {}) {
  Scenario s{.name = "id-report", .version = 1, .duration = 1min};
  NodeSpec ids{
      .id = "ids", .type = NodeType::id_generator, .replicas = replicas, .capacity = capacity};
  ids.id_algorithm = algorithm;
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{10}}, .op_shares = {{"next_id", 1.0}}}},
      std::move(ids)};
  s.links = {LinkSpec{.from = "u", .to = "ids", .dir = simsys::model::Direction::inout}};
  return s;
}

}  // namespace

TEST_CASE("explain says what a snowflake generator stands for", "[report]") {
  const auto text = explain_of(id_reported(simsys::model::IdAlgorithm::snowflake));
  CAPTURE(text);
  const auto line = node_line(text, "ids");
  REQUIRE(line.contains("algorithm=snowflake (4.096M/s per replica by design)"));
  // The algorithm supplied these numbers, the scenario did not, and the tag says so: not
  // "(default)", which elsewhere means the node TYPE's number (review of 3adc555, M5).
  REQUIRE(line.contains("capacity=4.096M/s (algorithm)"));
  REQUIRE(line.contains("service_time=10us (algorithm)"));
}

TEST_CASE("explain says a ticket server is one database doing 1k writes a second", "[report]") {
  const auto text = explain_of(id_reported(simsys::model::IdAlgorithm::ticket_server));
  CAPTURE(text);
  const auto line = node_line(text, "ids");
  REQUIRE(line.contains("algorithm=ticket_server (one database: 1k/s, spec §6 writes)"));
  REQUIRE(line.contains("capacity=1k/s (algorithm)"));
  REQUIRE(line.contains("service_time=1ms (algorithm)"));
}

TEST_CASE("explain says a uuid generator is unbounded in practice", "[report]") {
  const auto text = explain_of(id_reported(simsys::model::IdAlgorithm::uuid));
  CAPTURE(text);
  const auto line = node_line(text, "ids");
  REQUIRE(line.contains("algorithm=uuid (unbounded: 1G/s)"));
  REQUIRE(line.contains("capacity=1G/s"));
  REQUIRE(line.contains("service_time=1us"));
}

TEST_CASE("explain tags a capacity the scenario stated on an id generator as scenario",
          "[report]") {
  const auto text =
      explain_of(id_reported(simsys::model::IdAlgorithm::ticket_server, 1, Rate{8000}));
  CAPTURE(text);
  const auto line = node_line(text, "ids");
  REQUIRE(line.contains("capacity=8k/s (scenario)"));
  // Beside a stated capacity the algorithm's own rate is the default it overrides, and the
  // wording says so rather than reading as a second capacity (review of 3adc555, M5).
  REQUIRE(line.contains("algorithm=ticket_server (one database; default 1k/s, spec §6 writes)"));
  // The service time was not stated: the algorithm's 1 ms stands, tagged as the algorithm's.
  REQUIRE(line.contains("service_time=1ms (algorithm)"));
}

// The paper load is checked against the node's total: 3 snowflake machines x 4.096M/s =
// 12.288M/s, printed with four significant digits.
TEST_CASE("explain checks an id generator against its replicas' total capacity", "[report]") {
  const auto text = explain_of(id_reported(simsys::model::IdAlgorithm::snowflake, 3));
  CAPTURE(text);
  REQUIRE(check_line(text, "ids").contains("peak offered 10/s vs 12.29M/s total capacity"));
}

// ---- Request sizes on byte-sized nodes (plan 2c refinement 1, Task 2) ----

namespace {
// u (sending `stream` at 1k/s, each request `stream_size` long) -> cdn (fixed hit ratio 0.95,
// the given capacity) -> origin (a service of 5k/s, which counts requests).
Scenario sized_cdn(simsys::Bytes stream_size, std::optional<simsys::ByteRate> bytes,
                   std::optional<Rate> requests) {
  Scenario s{.name = "sized-cdn", .version = 1, .duration = 1min};
  NodeSpec cdn{.id = "cdn_us",
               .type = NodeType::cdn,
               .capacity = requests,
               .byte_capacity = bytes,
               .hit_ratio = simsys::model::HitRatio{.fixed = 0.95}};
  cdn.origin = "origin";
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{1000}},
                                           .op_shares = {{"stream", 1.0}},
                                           .sizes = {{"stream", stream_size}}}},
             std::move(cdn),
             NodeSpec{.id = "origin", .type = NodeType::service, .capacity = Rate{5000}}};
  s.links = {LinkSpec{.from = "u", .to = "cdn_us"}, LinkSpec{.from = "cdn_us", .to = "origin"}};
  return s;
}
}  // namespace

// A byte-sized node counts megabytes, so the size of each op it serves is the number behind its
// capacity: explain prints it, 50 MB = 5e7 B written in the scenario's own grammar.
TEST_CASE("explain prints the sizes of the ops a byte-sized node serves", "[report]") {
  const auto text = explain_of(sized_cdn(simsys::Bytes{50e6}, simsys::ByteRate{100e9}, {}));
  CAPTURE(text);
  REQUIRE(node_line(text, "cdn_us").contains("sizes: stream 50MB"));
}

// Nothing to print where requests are counted: the sizes would be a number the node never uses.
TEST_CASE("explain prints no sizes on a node that counts requests", "[report]") {
  SECTION("a cdn with a request capacity") {
    const auto text = explain_of(sized_cdn(simsys::Bytes{50e6}, {}, Rate{100'000}));
    CAPTURE(text);
    REQUIRE_FALSE(node_line(text, "cdn_us").empty());
    REQUIRE_FALSE(node_line(text, "cdn_us").contains("sizes:"));
  }
  SECTION("the service behind a byte-sized cdn") {
    const auto text = explain_of(sized_cdn(simsys::Bytes{50e6}, simsys::ByteRate{100e9}, {}));
    CAPTURE(text);
    REQUIRE_FALSE(node_line(text, "origin").empty());
    REQUIRE_FALSE(node_line(text, "origin").contains("sizes:"));
  }
}

// A byte-sized node's `service_time` is the time of one megabyte (plan 2c refinement 2), so
// explain says "per MB": the CDN's 10 ms is the book's "read 1 MB sequentially from the network",
// the blob store's 40 ms the disk read (30 ms) plus the transfer (10 ms). A node that counts
// requests has a time per request, with no unit after it.
TEST_CASE("explain prints a byte-sized node's service time per megabyte", "[report]") {
  auto s = cdn_reported("cdn_us", simsys::ByteRate{100e9});
  s.nodes[2] = NodeSpec{.id = "origin", .type = NodeType::blob_store};
  const auto text = explain_of(s);
  CAPTURE(text);
  REQUIRE(node_line(text, "cdn_us").contains("service_time=10ms per MB (default)"));
  REQUIRE(node_line(text, "origin").contains("service_time=40ms per MB (default)"));
  const auto counting = explain_of(cdn_reported());
  REQUIRE_FALSE(node_line(counting, "cdn").contains("per MB"));
}

// A byte-sized node's capacity units are megabytes, so its waiting room is too, and its c servers
// are slots that each hold one object for its whole transfer (Little's law: c = MB/s x the time of
// one MB; review of plan 2c Task 3, I1, and of its fix round, M4). explain labels both:
// `c=1000 slots (1 object each) (implied)` on a 100 GB/s CDN at 10 ms per MB, `c=20 slots (1 object
// each)` on a default blob store (500 MB/s x 40 ms), and `queue_limit=1000 MB (default)`, the
// generic waiting room of 1000 units read in MB. A transfer of a fixed size at a fixed rate is
// deterministic (second fix round, I2), so the per-object variability is 0 there. A node that
// counts requests keeps its bare numbers.
TEST_CASE("explain labels a byte-sized node's c in object slots and queue_limit in megabytes",
          "[report]") {
  auto s = cdn_reported("cdn_us", simsys::ByteRate{100e9});
  s.nodes[2] = NodeSpec{.id = "origin", .type = NodeType::blob_store};
  const auto text = explain_of(s);
  CAPTURE(text);
  REQUIRE(node_line(text, "cdn_us")
              .contains("c=1000 slots (1 object each) (implied)  queue_limit=1000 MB (default)  "
                        "variability=0"));
  REQUIRE(node_line(text, "origin")
              .contains("c=20 slots (1 object each) (implied)  queue_limit=1000 MB (default)  "
                        "variability=0"));
  const auto counting = explain_of(cdn_reported());
  REQUIRE(node_line(counting, "cdn").contains("c=1000 (implied)  queue_limit=1000 (default)"));
}

namespace {
// u (1 request/s of `op`, 300 MB) -> store, a blob store with the given write ops (its type's
// default, `write`, when none are given).
Scenario blob_store_reported(const std::string& op,
                             std::optional<std::vector<std::string>> write_ops = {}) {
  Scenario s{.name = "blob-report", .version = 1, .duration = 1min};
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{1}},
                                    .op_shares = {{op, 1.0}},
                                    .sizes = {{op, simsys::Bytes{300e6}}}}},
      NodeSpec{.id = "store", .type = NodeType::blob_store, .write_ops = std::move(write_ops)}};
  s.links = {LinkSpec{.from = "u", .to = "store"}};
  return s;
}
}  // namespace

// A blob store answers its writes at their last byte and everything else at its first megabyte
// (review of plan 2c Task 3, I2), so explain says which ops are its writes, as it does for a
// cache: the scenario's `write_ops`, else the default `write`, with a note when none of them
// reaches the store (an upload not listed would be answered as a read).
TEST_CASE("explain lists a blob store's write ops", "[report]") {
  const auto stated = explain_of(blob_store_reported("upload", std::vector<std::string>{"upload"}));
  CAPTURE(stated);
  REQUIRE(node_line(stated, "store").contains("write_ops=upload"));
  REQUIRE_FALSE(node_line(stated, "store").contains("never arrives"));
  const auto defaulted = explain_of(blob_store_reported("upload"));
  CAPTURE(defaulted);
  REQUIRE(node_line(defaulted, "store").contains("write_ops=write (never arrives)"));
}

// ---- A link's bandwidth is a pipe: what the reports say about it (plan 2c Task 5) ----
//
// A link with a bandwidth is a queue of megabytes, so it gets a row of its own, named after the
// link, `from->to`, wherever a node does: the summary table, the JSON `links`, the CSV series
// (test_series.cpp) and explain's checks. Numbers on a link are in MEGABYTES (a rate of MB/s).
//
// One picture throughout: client `a` sends 100 `put`/s of 1 MB to service `b` over a 1 Gbit/s
// link = 125 MB/s, so the pipe's rho is 100 / 125 = 0.8, yellow. A pipe whose utilization the
// run reports is the NodeSample's conservation law again, so no new idea, only a new row.
//
// Proposed forms (the implementer may adjust the words, not the facts):
//  - table: the link's row has the node table's columns, so its first cells after the name are
//    mean_rho and peak_rho;
//  - explain, links block: `bandwidth=1Gbit/s (125MB/s)`, the bits form first (a network is rated
//    in bits) with the bytes after it; 5e9 B/s reads `40Gbit/s (5GB/s)`;
//  - explain, checks block: `a->b  peak offered 100 MB/s vs 125 MB/s  (80%, yellow)`.

namespace {
// a(`rate`/s of `put`, 1 MB each) -> b (a service of 1M/s) over a link of the given bandwidth
// (none: a plain link). 2 s at 100 ms steps, the first 500 ms warmup.
Scenario piped(double rate, std::optional<simsys::ByteRate> bandwidth) {
  Scenario s{.name = "piped", .version = 1, .duration = 2s, .warmup = 500ms};
  s.nodes = {NodeSpec{.id = "a",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{rate}},
                                           .op_shares = {{"put", 1.0}},
                                           .sizes = {{"put", simsys::Bytes{1e6}}}}},
             NodeSpec{.id = "b", .type = NodeType::service, .capacity = Rate{1e6}, .latency = 1ms}};
  s.links = {LinkSpec{.from = "a", .to = "b", .latency = 250us, .bandwidth = bandwidth}};
  return s;
}

// The cells of the first line of `text` whose first cell is `first`, split at whitespace; empty
// when no line starts with it. The table is aligned with spaces, so this reads a row back.
std::vector<std::string> row_starting_with(const std::string& text, const std::string& first) {
  std::istringstream lines{text};
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream cells{line};
    std::vector<std::string> row;
    std::string cell;
    while (cells >> cell) row.push_back(cell);
    if (!row.empty() && row.front() == first) return row;
  }
  return {};
}
}  // namespace

TEST_CASE("the table has a row for a bandwidth link, named from->to, with the pipe's utilization",
          "[report]") {
  const auto s = piped(100, simsys::ByteRate{125e6});
  const auto g = *simsys::graph::Graph::build(s);
  const auto text = simsys::report::render_table(summarize(s, g), {}, /*color=*/false);
  CAPTURE(text);
  const auto row = row_starting_with(text, "a->b");
  REQUIRE(row.size() >= 3);
  REQUIRE(row[1] == "0.8");  // mean_rho: 100 MB/s over 125 MB/s
  REQUIRE(row[2] == "0.8");  // peak_rho: constant load, so the same
}

TEST_CASE("a scenario without a bandwidth has no link rows in its table", "[report]") {
  const auto s = piped(100, std::nullopt);
  const auto g = *simsys::graph::Graph::build(s);
  const auto text = simsys::report::render_table(summarize(s, g), {}, /*color=*/false);
  CAPTURE(text);
  REQUIRE_FALSE(text.contains("a->b"));
  REQUIRE_FALSE(text.contains("->"));
  REQUIRE(text.contains("a.put"));  // the op table is unchanged
}

// A pipe above its capacity fills its 1000 MB room and drops: 1000 puts/s of 1 MB offer 1000 MB/s
// to a 125 MB/s pipe, whose backlog grows by 875 x 0.1 s = 87.5 MB per step and overflows in the
// 12th step (1.1 s), inside the observed part of a 5 s run. The row shows a saturated pipe (rho 1),
// a backlog and dropped megabytes, in the node table's columns; and since a pipe's losses fail
// requests as a node's drops do, the header's worst drop rate names the pipe (b, at 1M/s, drops
// nothing).
TEST_CASE("a pipe that overflows shows its dropped megabytes in its row and the header",
          "[report]") {
  auto s = piped(1000, simsys::ByteRate{125e6});
  s.duration = 5s;
  const auto g = *simsys::graph::Graph::build(s);
  const auto text = simsys::report::render_table(summarize(s, g), {}, /*color=*/false);
  CAPTURE(text);
  const auto row = row_starting_with(text, "a->b");
  REQUIRE(row.size() >= 6);
  REQUIRE(row[1] == "1");  // mean_rho: the pipe is saturated, 125 MB/s drained of 125
  REQUIRE(row[3] != "0");  // peak_backlog: the room filled
  REQUIRE(row[4] != "0");  // dropped
  const auto header = text.substr(0, text.find('\n'));
  REQUIRE(header.contains("drop rate "));
  REQUIRE(header.contains("(worst: a->b)"));
}

TEST_CASE("explain prints a link's bandwidth in bits with the bytes after it", "[report]") {
  SECTION("1 Gbit/s") {
    const auto text = explain_of(piped(100, simsys::ByteRate{125e6}));
    CAPTURE(text);
    REQUIRE(text.contains("bandwidth=1Gbit/s (125MB/s)"));
  }
  SECTION("40 Gbit/s") {
    const auto text = explain_of(piped(100, simsys::ByteRate{5e9}));
    CAPTURE(text);
    REQUIRE(text.contains("bandwidth=40Gbit/s (5GB/s)"));
  }
  SECTION("1.5 Gbit/s has no whole number of Gbit/s, so it is written in Mbit/s") {
    const auto text = explain_of(piped(100, simsys::ByteRate{187.5e6}));
    CAPTURE(text);
    REQUIRE(text.contains("bandwidth=1500Mbit/s (187.5MB/s)"));
  }
  SECTION("a rate no bit prefix writes whole is given in bytes alone") {
    const auto text = explain_of(piped(100, simsys::ByteRate{1000.5}));  // 8004 bit/s
    CAPTURE(text);
    REQUIRE(text.contains("bandwidth=1.0005KB/s"));
    REQUIRE_FALSE(text.contains("bit/s"));
  }
  SECTION("no bandwidth, nothing said") {
    const auto text = explain_of(piped(100, std::nullopt));
    CAPTURE(text);
    REQUIRE_FALSE(text.contains("bandwidth"));
  }
}

// The paper check for the pipe: 100 puts/s x 1 MB = 100 MB/s offered against 125 MB/s, 80 %,
// yellow (0.70 <= rho < 0.90): the verdict a student reaches before running; at 40 Gbit/s = 5k MB/s
// the same load is 2 %.
TEST_CASE("explain checks a bandwidth link's peak offered MB per second against its capacity",
          "[report]") {
  SECTION("1 Gbit/s") {
    const auto text = explain_of(piped(100, simsys::ByteRate{125e6}));
    CAPTURE(text);
    REQUIRE(check_line(text, "a->b")
                .contains("a->b  peak offered 100 MB/s vs 125 MB/s  (80%, yellow)"));
  }
  SECTION("40 Gbit/s") {
    const auto text = explain_of(piped(100, simsys::ByteRate{5e9}));
    CAPTURE(text);
    REQUIRE(check_line(text, "a->b").contains("peak offered 100 MB/s vs 5k MB/s  (2%, green)"));
  }
  SECTION("above the capacity") {
    const auto text = explain_of(piped(200, simsys::ByteRate{125e6}));
    CAPTURE(text);
    REQUIRE(check_line(text, "a->b").contains("peak offered 200 MB/s vs 125 MB/s  (160%, red)"));
  }
  SECTION("a link without a bandwidth has no check") {
    const auto text = explain_of(piped(100, std::nullopt));
    CAPTURE(text);
    REQUIRE(check_line(text, "a->b").empty());
  }
}

// Two marks on a pipe's check that the node lines carry too (review of Task 6, M-3):
//  - behind a che CDN with no `keys` size (no hit ratio to resolve, so every read counts as a miss
//    on paper) the pipe to its origin is checked against its worst case, and says so: 10 streams/s
//    of 50 MB, all of them missing, are 500 MB/s of 5k MB/s, 10 %, an upper bound;
//  - a pipe whose sender is a type the paper load does not follow (a notification fan-out) and
//    that it therefore sends nothing on paper prints the mark without a percentage, as a node
//    line does: no "0%, green" verdict on a load that is missing, not zero.
TEST_CASE("a pipe behind a cdn of unknown hit ratio is checked as an upper bound", "[report]") {
  Scenario s{.name = "pipe-upper", .version = 1, .duration = 1min};
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{10}},
                                           .op_shares = {{"stream", 1.0}},
                                           .sizes = {{"stream", simsys::Bytes{50e6}}}}},
             NodeSpec{.id = "cdn",
                      .type = NodeType::cdn,
                      .hit_ratio = simsys::model::HitRatio{.fixed = std::nullopt, .che = true},
                      .origin = "store"},
             NodeSpec{.id = "store", .type = NodeType::blob_store}};
  s.links = {LinkSpec{.from = "u", .to = "cdn"},
             LinkSpec{.from = "cdn", .to = "store", .bandwidth = simsys::ByteRate{5e9}}};
  const auto text = explain_of(s);
  CAPTURE(text);
  REQUIRE(check_line(text, "cdn->store")
              .contains("peak offered 500 MB/s vs 5k MB/s  (10%, green) (hit ratio unknown: "
                        "counted as 0 — an upper bound)"));
}

TEST_CASE("a pipe that a blocking type sends nothing on paper has no percentage", "[report]") {
  Scenario s{.name = "pipe-unloaded", .version = 1, .duration = 1min};
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{10}}, .op_shares = {{"fire", 1.0}}}},
      NodeSpec{.id = "fan",
               .type = NodeType::notification_fanout,
               .then = {Step{.to = "svc", .op = "push", .size = simsys::Bytes{1e6}}}},
      NodeSpec{.id = "svc", .type = NodeType::service}};
  s.links = {LinkSpec{.from = "u", .to = "fan"},
             LinkSpec{.from = "fan", .to = "svc", .bandwidth = simsys::ByteRate{125e6}}};
  const auto text = explain_of(s);
  CAPTURE(text);
  const auto line = check_line(text, "fan->svc");
  REQUIRE(line.contains(
      "peak offered 0 MB/s vs 125 MB/s  (load not propagated: notification_fanout is not "
      "modelled yet)"));
  REQUIRE_FALSE(line.contains("%"));
}

// ---- Message queues and worker pools in explain (plan 2c refinements 4-5, Task 6) ----
//
// explain says what the two types stand for, in the words of the plan:
//  - a worker pool: `capacity=1.2k/s (workers / cost)` and `consumes=q  workers=12 cost=10ms`:
//    the capacity is not a number the scenario states but 12 workers / 10 ms = 1200 jobs/s, and
//    the line says so, once (review of Task 6, M-6: it used to repeat it after `cost=`);
//  - a message queue: `retention=7d (default: Kafka's 168 h)` when the scenario gives none (the
//    book gives no figure; 168 h is Kafka's `log.retention.hours`), the stated value without the
//    tag otherwise, and `partitions=8 (not modelled)`: accepted and printed, not simulated.
// (The plan's example `cost=3.333ms` prints as `3.33ms`: explain writes durations with three
// significant digits. The pins use costs that print exactly.)

namespace {
// u -> q -> w: a queue with the given extra keys, a pool of 12 workers at 10 ms draining it.
Scenario queue_and_pool_reported(std::optional<simsys::SimTime> retention,
                                 std::optional<int> partitions) {
  Scenario s{.name = "queue-report", .version = 1, .duration = 1min};
  NodeSpec queue{.id = "q", .type = NodeType::message_queue};
  queue.retention = retention;
  queue.partitions = partitions;
  NodeSpec pool{.id = "w", .type = NodeType::worker_pool};
  pool.consumes = "q";
  pool.workers = 12;
  pool.job_cost = 10ms;
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{100}}, .op_shares = {{"publish", 1.0}}}},
      std::move(queue), std::move(pool)};
  s.links = {LinkSpec{.from = "u", .to = "q"}, LinkSpec{.from = "q", .to = "w"}};
  return s;
}
}  // namespace

TEST_CASE("explain says a worker pool's capacity is workers over cost, once", "[report]") {
  const auto text = explain_of(queue_and_pool_reported(std::nullopt, 8));
  CAPTURE(text);
  const auto line = node_line(text, "w");
  REQUIRE(line.contains("capacity=1.2k/s (workers / cost)"));
  REQUIRE(line.ends_with("consumes=q  workers=12 cost=10ms"));
  REQUIRE(line.find("1.2k/s") == line.rfind("1.2k/s"));  // the capacity is stated once
}

// A pool's room is retention x its rate (graph::Graph::connect_consumer): Kafka's 7 days of
// 1.2k jobs/s are 604,800 s x 1200/s = 725,760,000 messages, printed with an SI prefix to four
// digits, 725.8M, rather than six digits' "7.2576e+08" (review of Task 6, M-1); a room under a
// million keeps its digits (10 s x 1200/s = 12000).
TEST_CASE("explain prints a pool's room of millions with an SI prefix", "[report]") {
  const auto days = explain_of(queue_and_pool_reported(std::nullopt, std::nullopt));
  CAPTURE(days);
  REQUIRE(node_line(days, "w").contains("queue_limit=725.8M (retention × capacity)"));
  const auto seconds = explain_of(queue_and_pool_reported(10s, std::nullopt));
  CAPTURE(seconds);
  REQUIRE(node_line(seconds, "w").contains("queue_limit=12000 (retention × capacity)"));
}

// Six significant digits round a room in [999,999.5, 1e6) up to a million, which they print as
// "1e+06" (review of plan 2c Task 7, minor 8): such a room is printed as the million it rounds to,
// "1M", and the room just below keeps its six digits. 833.3333 s x 1200/s = 999,999.96 messages;
// 833.332 s x 1200/s = 999,998.4.
TEST_CASE("explain prints a room that rounds to a million as 1M, not 1e+06", "[report]") {
  using namespace std::chrono_literals;
  const auto rounds_up = explain_of(queue_and_pool_reported(833'333'300us, std::nullopt));
  CAPTURE(rounds_up);
  REQUIRE(node_line(rounds_up, "w").contains("queue_limit=1M (retention × capacity)"));
  const auto below = explain_of(queue_and_pool_reported(833'332ms, std::nullopt));
  CAPTURE(below);
  REQUIRE(node_line(below, "w").contains("queue_limit=999998 (retention × capacity)"));
}

TEST_CASE(
    "explain marks an absent retention as Kafka's default and prints partitions as not "
    "modelled",
    "[report]") {
  const auto text = explain_of(queue_and_pool_reported(std::nullopt, 8));
  CAPTURE(text);
  const auto line = node_line(text, "q");
  REQUIRE(line.contains("retention=7d (default: Kafka's 168 h)"));
  REQUIRE(line.contains("partitions=8 (not modelled)"));
}

TEST_CASE("explain prints a stated retention without the default tag", "[report]") {
  const auto text = explain_of(queue_and_pool_reported(10s, 8));
  CAPTURE(text);
  const auto line = node_line(text, "q");
  REQUIRE(line.contains("retention=10s"));
  REQUIRE_FALSE(line.contains("Kafka"));
}

// ---- explain's paper load follows queues and pools (plan 2c Task 7) ----
//
// The paper load is the peak offered load, forwarded along the graph with no queueing. Since
// Task 7 it follows a message queue into its consumers and a worker pool's `then` steps (each job
// it is OFFERED: the paper load does not queue, so a pool offered more than its capacity still
// passes all of it on), instead of marking everything behind them "load not propagated".
//
// Picture: u sends `rate`/s of `publish` to q; pool w (12 workers at 10 ms = 1.2k jobs/s) drains
// q and, per job, sends 4 `work` requests to svc (a 10k/s service) and one `publish` to done_q,
// which pool w2 (10 workers at 5 ms = 2k jobs/s) drains.

namespace {
Scenario queue_pool_chain(double rate) {
  Scenario s{.name = "paper-chain", .version = 1, .duration = 1min};
  NodeSpec pool{.id = "w",
                .type = NodeType::worker_pool,
                .then = {Step{.to = "svc", .op = "work", .factor = 4.0},
                         Step{.to = "done_q", .op = "publish"}}};
  pool.consumes = "q";
  pool.workers = 12;
  pool.job_cost = 10ms;
  NodeSpec done_pool{.id = "w2", .type = NodeType::worker_pool};
  done_pool.consumes = "done_q";
  done_pool.workers = 10;
  done_pool.job_cost = 5ms;
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload =
                          Workload{.curve = {.base = Rate{rate}}, .op_shares = {{"publish", 1.0}}}},
             NodeSpec{.id = "q", .type = NodeType::message_queue},
             std::move(pool),
             NodeSpec{.id = "svc", .type = NodeType::service, .capacity = Rate{10'000}},
             NodeSpec{.id = "done_q", .type = NodeType::message_queue},
             std::move(done_pool)};
  s.links = {LinkSpec{.from = "u", .to = "q"}, LinkSpec{.from = "q", .to = "w"},
             LinkSpec{.from = "w", .to = "svc"}, LinkSpec{.from = "w", .to = "done_q"},
             LinkSpec{.from = "done_q", .to = "w2"}};
  return s;
}
}  // namespace

// 100/s into q: q 100/s of 1M/s; w 100/s of 1.2k/s (100 / 1200 = 8.33 %); svc 100 x 4 = 400/s of
// 10k/s (4 %); done_q 100/s of 1M/s; w2 100/s of 2k/s (5 %). Every node has a verdict, none a
// mark.
TEST_CASE("explain follows a queue into its pool and the pool's then steps into their targets",
          "[report]") {
  const auto text = explain_of(queue_pool_chain(100));
  CAPTURE(text);
  REQUIRE_FALSE(text.contains("load not propagated"));
  REQUIRE(check_line(text, "q").contains("peak offered 100/s vs 1M/s total capacity"));
  REQUIRE(check_line(text, "w").contains(
      "peak offered 100/s vs 1.2k/s total capacity  (8.33%, green)"));
  REQUIRE(
      check_line(text, "svc").contains("peak offered 400/s vs 10k/s total capacity  (4%, green)"));
  REQUIRE(check_line(text, "done_q").contains("peak offered 100/s vs 1M/s total capacity"));
  REQUIRE(
      check_line(text, "w2").contains("peak offered 100/s vs 2k/s total capacity  (5%, green)"));
}

// 2000/s into a pool of 1.2k/s: 2000 / 1200 = 167 %, red. The paper load does not queue, so the
// pool is offered, and passes on, all 2000 jobs/s: svc is offered 2000 x 4 = 8000/s of 10k/s
// (80 %), done_q 2000/s and w2 2000/s of 2k/s (100 %). (What the engine sends on is what the pool
// drained, 1.2k/s: Task 7's theory tests. Explain is the no-queue bound, "can this ever fit".)
TEST_CASE("explain passes a saturated pool's whole arrival on: the paper load does not queue",
          "[report]") {
  const auto text = explain_of(queue_pool_chain(2000));
  CAPTURE(text);
  REQUIRE(
      check_line(text, "w").contains("peak offered 2k/s vs 1.2k/s total capacity  (167%, red)"));
  REQUIRE(check_line(text, "svc").contains("peak offered 8k/s vs 10k/s total capacity"));
  REQUIRE(check_line(text, "w2").contains("peak offered 2k/s vs 2k/s total capacity"));
}

// The mark that remains is for the types with no phase yet: a pool's target behind a notification
// fan-out is marked by the fan-out alone, and the mark does not name the queue or the pool on the
// way (they are followed now). The fixture is the one the lower-bound cases use: the fan-out's
// `then` sends to q, whose pool w sends on to svc.
TEST_CASE("a node behind a notification fan-out and a queue and pool is marked by the fan-out only",
          "[report]") {
  auto s = queue_pool_chain(100);
  s.nodes[0].workload->op_shares = {{"fire", 1.0}};
  s.nodes.insert(s.nodes.begin() + 1, NodeSpec{.id = "fan",
                                               .type = NodeType::notification_fanout,
                                               .then = {Step{.to = "q", .op = "publish"}}});
  s.links[0] = LinkSpec{.from = "u", .to = "fan"};
  s.links.push_back(LinkSpec{.from = "fan", .to = "q"});
  const auto text = explain_of(s);
  CAPTURE(text);
  for (const auto* id : {"q", "w", "svc", "done_q", "w2"}) {
    CAPTURE(id);
    const auto line = check_line(text, id);
    REQUIRE(line.contains("(load not propagated: notification_fanout is not modelled yet)"));
    REQUIRE_FALSE(line.contains("worker_pool"));
    REQUIRE_FALSE(line.contains("message_queue"));
  }
}

// ---- Queue lag: the queue_lag SLO and its columns (plan 2c refinement 4 and 14, Task 8) ----
//
// A message queue's lag is how far its slowest consumer group trails it: max(backlog / drain,
// k x Wq) of the group's pool (see the theory tests for the formula). The queue_lag SLO bounds
// the PEAK lag after warmup: a lag bound of "5 minutes" is a promise about the worst moment, not
// about the average one, since a notification that arrives 20 minutes late is late whatever the
// mean was.
//
// Picture: u publishes 1000/s to the queue q, one group w of 12 workers at 10 ms (exactly 1200
// jobs/s). For the first 200 s u bursts to 1500/s, so w falls behind by 300 messages a second:
// after the burst's 200 s there are 60,000 waiting, and a message published then waits for all of
// them to drain, 60,000 / 1200 = 50 s: the peak lag (the oldest unread message reaches that age
// one lag later, at 250 s; the SLO bounds the wait of every message published after warmup).
// Afterwards w drains 200 a second faster than the inflow, so the lag falls (40,000 / 1200 = 33.3 s
// at t = 300 s).

namespace {
// Adds a publisher u<tag> (1000/s of `publish`, 1500/s for the first 200 s when `bursting`), its
// message queue q<tag> and the queue's one consumer group w<tag>: 12 workers at 10 ms a job.
void add_lane(Scenario& s, const std::string& tag, bool bursting) {
  Workload workload{.curve = {.base = Rate{1000}}, .op_shares = {{"publish", 1.0}}};
  if (bursting) {
    workload.curve.spikes = {simsys::workload::Spike{.at = 0s, .duration = 200s, .factor = 1.5}};
  }
  s.nodes.push_back(NodeSpec{.id = "u" + tag, .type = NodeType::client, .workload = workload});
  s.nodes.push_back(NodeSpec{.id = "q" + tag, .type = NodeType::message_queue});
  s.nodes.push_back(NodeSpec{.id = "w" + tag,
                             .type = NodeType::worker_pool,
                             .workers = 12,
                             .job_cost = 10ms,
                             .consumes = "q" + tag});
  s.links.push_back(LinkSpec{.from = "u" + tag, .to = "q" + tag, .latency = 0s});
  s.links.push_back(LinkSpec{.from = "q" + tag, .to = "w" + tag, .latency = 0s});
}

// The burst picture above, run for `duration`.
Scenario bursting_queue(simsys::SimTime duration = 200s) {
  Scenario s{.name = "lag", .version = 1, .duration = duration};
  add_lane(s, "", true);
  return s;
}

simsys::model::SloSpec queue_lag_slo(std::optional<std::string> node, simsys::SimTime max) {
  return simsys::model::SloSpec{
      .metric = simsys::model::SloMetric::queue_lag, .node = std::move(node), .max = max};
}

simsys::report::SloResult only_result(Scenario s, simsys::model::SloSpec slo) {
  s.slos = {std::move(slo)};
  const auto g = *simsys::graph::Graph::build(s);
  const auto results = simsys::report::evaluate_slos(summarize(s, g), s.slos);
  REQUIRE(results.size() == 1);
  return results[0];
}

// The whitespace-separated words of the first line of `text` that starts with `first_word`.
std::vector<std::string> words_of_line_starting(const std::string& text,
                                                std::string_view first_word) {
  std::istringstream lines{text};
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream words{line};
    std::string word;
    std::vector<std::string> found;
    while (words >> word) found.push_back(word);
    if (!found.empty() && found.front() == first_word) return found;
  }
  FAIL("no line starts with " << first_word << " in:\n" << text);
  return {};
}

const simsys::report::NodeSummary& node_summary(
    const std::vector<simsys::report::NodeSummary>& nodes, std::string_view id) {
  const auto found = std::ranges::find(nodes, id, &simsys::report::NodeSummary::id);
  REQUIRE(found != nodes.end());
  return *found;
}
}  // namespace

TEST_CASE("the queue_lag SLO passes within 1m and fails within 30s: the burst peaks at 50 s",
          "[report]") {
  using simsys::report::SloStatus;
  SECTION("max 1m passes with the peak, 50 s, observed") {
    const auto result = only_result(bursting_queue(), queue_lag_slo("q", 1min));
    REQUIRE(result.status == SloStatus::pass);
    REQUIRE(result.observed.has_value());
    REQUIRE_THAT(*result.observed, WithinRel(50.0, 1e-6));  // 60,000 waiting / 1200 a second
  }
  SECTION("max 30s fails, with the same 50 s observed") {
    const auto result = only_result(bursting_queue(), queue_lag_slo("q", 30s));
    REQUIRE(result.status == SloStatus::fail);
    REQUIRE(result.observed.has_value());
    REQUIRE_THAT(*result.observed, WithinRel(50.0, 1e-6));
  }
}

// The table's slo block, one line a verdict: status, metric, node, op, observed, max, as the
// staleness line is (`-` for the op a queue_lag has none of).
TEST_CASE("the SLO table shows the queue_lag verdict with its observed peak: PASS queue_lag q 50s",
          "[report]") {
  auto s = bursting_queue();
  s.slos = {queue_lag_slo("q", 1min), queue_lag_slo("q", 30s)};
  const auto g = *simsys::graph::Graph::build(s);
  const auto summary = summarize(s, g);
  const auto table =
      simsys::report::render_table(summary, simsys::report::evaluate_slos(summary, s.slos), false);
  CAPTURE(table);
  const std::vector<std::string> pass{"PASS", "queue_lag", "q", "-", "50s", "1m"};
  const std::vector<std::string> fail{"FAIL", "queue_lag", "q", "-", "50s", "30s"};
  REQUIRE(words_of_line_starting(table, "PASS") == pass);
  REQUIRE(words_of_line_starting(table, "FAIL") == fail);
}

// 250 s: 50 s after the backlog peaked at t = 200 s the pool has drained 200 x 50 = 10,000 more
// than arrived, so the lag at the end is (60,000 - 10,000) / 1200 = 41.67 s; the SLO still sees
// the 50 s it was.
TEST_CASE("the queue_lag SLO observes the peak, not the lag at the end of the run", "[report]") {
  const auto s = bursting_queue(250s);
  const auto g = *simsys::graph::Graph::build(s);
  const auto summary = summarize(s, g);
  const auto nodes = summary.nodes();  // a copy: node_summary returns a reference into it
  const auto& queue = node_summary(nodes, "q");
  REQUIRE(queue.peak_queue_lag_s.has_value());
  REQUIRE_THAT(*queue.peak_queue_lag_s, WithinRel(50.0, 1e-6));
  // What the last step reports, from the engine's own sample, is lower:
  auto sim = *simsys::engine::Simulation::create(g);
  simsys::engine::StepRecord last;
  sim.run({.duration = s.duration, .step = s.step}, [&](const auto& r) { last = r; });
  REQUIRE_THAT(*last.nodes[*g.index_of("q")].queue_lag_s, WithinRel(50'000.0 / 1200.0, 1e-6));
}

// Only the steps after warmup count (like every metric): with 220 s of warmup the first observed
// step is the one starting at t = 220 s, ending at 220.1 s with 60,000 - 200 x 20.1 = 55,980
// waiting: 55,980 / 1200 = 46.65 s, and the 50 s of t = 200 s is not in the window.
TEST_CASE("the observed queue lag is the peak after warmup only", "[report]") {
  auto s = bursting_queue(250s);
  s.warmup = 220s;
  const auto g = *simsys::graph::Graph::build(s);
  const auto summary = summarize(s, g);
  REQUIRE_THAT(*node_summary(summary.nodes(), "q").peak_queue_lag_s, WithinRel(46.65, 2e-3));
}

// Two queues, one calm (1000/s into 1200/s: a lag of the Erlang wait, 2.247 ms) and one bursting
// (50 s), the lagging one declared SECOND so that "the first queue" would be wrong. Without
// `node` the SLO bounds every queue's peak, so the worst one decides: 50 s.
TEST_CASE("without a node the queue_lag SLO takes the worst queue", "[report]") {
  using simsys::report::SloStatus;
  Scenario s{.name = "two queues", .version = 1, .duration = 200s};
  add_lane(s, "_calm", false);
  add_lane(s, "_burst", true);
  const auto passes = only_result(s, queue_lag_slo(std::nullopt, 1min));
  REQUIRE(passes.status == SloStatus::pass);
  REQUIRE_THAT(*passes.observed, WithinRel(50.0, 1e-6));
  const auto fails = only_result(s, queue_lag_slo(std::nullopt, 30s));
  REQUIRE(fails.status == SloStatus::fail);
  REQUIRE_THAT(*fails.observed, WithinRel(50.0, 1e-6));
  // Named, the calm queue is checked alone: 2.247 ms, a pass at a bound the burst would break.
  const auto calm = only_result(s, queue_lag_slo("q_calm", 1s));
  REQUIRE(calm.status == SloStatus::pass);
  REQUIRE_THAT(*calm.observed, WithinRel(0.002247, 1e-3));  // Wq(12, 1000/1200, 10 ms)
}

TEST_CASE("a queue_lag SLO with no message queue in the run is skipped, nothing observed",
          "[report]") {
  const auto result = only_result(mm1(500), queue_lag_slo(std::nullopt, 1s));
  REQUIRE(result.status == simsys::report::SloStatus::skipped);
  REQUIRE_FALSE(result.observed.has_value());
}

TEST_CASE("only a message queue has a peak lag in the node summaries", "[report]") {
  const auto s = bursting_queue();
  const auto g = *simsys::graph::Graph::build(s);
  const auto summary = summarize(s, g);
  const auto nodes = summary.nodes();
  REQUIRE_FALSE(node_summary(nodes, "u").peak_queue_lag_s.has_value());
  REQUIRE(node_summary(nodes, "q").peak_queue_lag_s.has_value());
  REQUIRE_FALSE(node_summary(nodes, "w").peak_queue_lag_s.has_value());
}

// The node table's new column, `peak_lag`, holds the queue's peak lag in the table's duration
// notation (50 s: "50s") on its row.
TEST_CASE("the node table has a peak_lag column holding the queue's peak lag", "[report]") {
  const auto s = bursting_queue();
  const auto g = *simsys::graph::Graph::build(s);
  const auto table = simsys::report::render_table(summarize(s, g), {}, false);
  CAPTURE(table);
  const auto header = words_of_line_starting(table, "node");
  const auto column = std::ranges::find(header, "peak_lag");
  REQUIRE(column != header.end());
  const auto queue_row = words_of_line_starting(table, "q");
  const auto at = static_cast<std::size_t>(column - header.begin());
  REQUIRE(queue_row.size() > at);
  REQUIRE(queue_row[at] == "50s");
}

// A group that is almost never busy waits a vanishing time: 1 message/s into the 12 workers of
// 1200 jobs/s is rho 1/1200, and Erlang C's chance that all 12 are busy is about 1e-33, so the lag
// is a tiny fraction of a nanosecond. The node table and the SLO line print it as "<1us" rather
// than such a figure (review of plan 2c Task 8, minor 6: youtube's completion_q printed
// "0.0632ns"); the summary, the JSON and the CSV keep the number.
TEST_CASE("a queue lag below a microsecond prints as under one microsecond", "[report]") {
  Scenario s{.name = "quiet", .version = 1, .duration = 10s};
  add_lane(s, "", false);
  s.nodes[0].workload->curve.base = Rate{1};
  s.slos = {queue_lag_slo("q", 1s)};
  const auto g = *simsys::graph::Graph::build(s);
  const auto summary = summarize(s, g);
  const auto lag = node_summary(summary.nodes(), "q").peak_queue_lag_s;
  REQUIRE(lag.has_value());
  REQUIRE(*lag > 0.0);
  REQUIRE(*lag < 1e-9);
  const auto results = simsys::report::evaluate_slos(summary, s.slos);
  const auto table = simsys::report::render_table(summary, results, false);
  CAPTURE(table);
  REQUIRE(words_of_line_starting(table, "q").back() == "<1us");
  const auto slo_line = words_of_line_starting(table, "PASS");
  REQUIRE(std::ranges::contains(slo_line, std::string{"<1us"}));
}

TEST_CASE("the JSON nodes carry peak_queue_lag_s: the queue's peak, null on every other node",
          "[report]") {
  const auto s = bursting_queue();
  const auto g = *simsys::graph::Graph::build(s);
  const auto json = simsys::report::render_json(summarize(s, g), {});
  CAPTURE(json);
  REQUIRE_THAT(json_value(json, "peak_queue_lag_s"), WithinRel(50.0, 1e-6));  // seconds
  std::size_t nulls = 0;
  for (auto at = json.find("\"peak_queue_lag_s\": null"); at != std::string::npos;
       at = json.find("\"peak_queue_lag_s\": null", at + 1)) {
    ++nulls;
  }
  REQUIRE(nulls == 2);  // u and w; q's is the number
}

// ---- External sinks in explain (plan 2c refinement 8, Task 9) ----
//
// explain says what the two keys of an external sink mean for the model:
//  - `failure_rate=2%`: the share of what the sink serves that fails (a 5xx), which the engine
//    counts as drops;
//  - `politeness=1/s (not modelled: phase 3)`: stored and printed, never simulated (it needs a
//    per-host state, which arrives with the crawler's frontier).
// A sink that states no politeness prints no note about one.

namespace {
// u -> apns: a client calling an external sink with the given failure rate and politeness.
Scenario sink_reported(double failure_rate, std::optional<Rate> politeness) {
  Scenario s{.name = "sink-report", .version = 1, .duration = 1min};
  NodeSpec sink{.id = "apns", .type = NodeType::external_sink};
  sink.failure_rate = failure_rate;
  sink.politeness = politeness;
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{100}}, .op_shares = {{"send", 1.0}}}},
      std::move(sink)};
  s.links = {LinkSpec{.from = "u", .to = "apns"}};
  return s;
}
}  // namespace

TEST_CASE("explain prints a sink's failure rate as a percentage and its politeness as not modelled",
          "[report]") {
  const auto text = explain_of(sink_reported(0.02, Rate{1.0}));
  CAPTURE(text);
  const auto line = node_line(text, "apns");
  REQUIRE(line.contains("failure_rate=2%"));
  REQUIRE(line.contains("politeness=1/s (not modelled: phase 3)"));
}

// A sink that fails everything it serves answers nobody: there is no latency to report, and the op
// table prints `-` rather than the sojourn of requests that all failed (review of plan 2c Tasks
// 9-10, minor 8).
TEST_CASE("the op table prints no latency for an op nobody answered", "[report]") {
  const auto s = sink_reported(1.0, std::nullopt);
  const auto g = *simsys::graph::Graph::build(s);
  const auto summary = summarize(s, g);
  REQUIRE(summary.ops().front().answered_per_s == 0.0);
  const auto text = simsys::report::render_table(summary, {}, /*color=*/false);
  CAPTURE(text);
  const auto row = text.substr(text.find("u.send"));
  const auto line = row.substr(0, row.find('\n'));
  REQUIRE_FALSE(line.contains("ms"));
  REQUIRE(line.contains(" -"));
}

TEST_CASE("explain prints no politeness note for a sink that states none", "[report]") {
  const auto text = explain_of(sink_reported(0.05, std::nullopt));
  CAPTURE(text);
  const auto line = node_line(text, "apns");
  REQUIRE(line.contains("failure_rate=5%"));
  REQUIRE_FALSE(line.contains("politeness"));
}

// ---- Search index as service sugar (plan 2c refinement 10, Task 11; book ch. 13) ----
//
// A search index's `build_interval` is how often the index is rebuilt from the aggregated logs. The
// model does not load the index with that rebuild and does not age its answers, and explain says so
// where it prints the interval, so a student does not read the number as modelled.

namespace {
// u (10/s of `suggest`) -> trie_db: a search_index of 4 replicas, 2k/s and 5 ms, with the
// `build_interval` only when given.
Scenario search_reported(std::optional<simsys::SimTime> build_interval) {
  Scenario s{.name = "search-report", .version = 1, .duration = 1min};
  NodeSpec index{.id = "trie_db",
                 .type = NodeType::search_index,
                 .replicas = 4,
                 .capacity = Rate{2000},
                 .latency = 5ms};
  index.build_interval = build_interval;
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{10}}, .op_shares = {{"suggest", 1.0}}}},
      std::move(index)};
  s.links = {LinkSpec{.from = "u", .to = "trie_db", .dir = simsys::model::Direction::inout}};
  return s;
}
}  // namespace

TEST_CASE("explain prints a search index's build_interval and says the rebuild is not modelled",
          "[report]") {
  const auto text = explain_of(search_reported(24h));
  CAPTURE(text);
  REQUIRE(node_line(text, "trie_db").contains("build_interval=24h (rebuild load not modelled)"));
}

TEST_CASE("explain prints no build_interval for a search index that states none", "[report]") {
  const auto text = explain_of(search_reported(std::nullopt));
  CAPTURE(text);
  const auto line = node_line(text, "trie_db");
  REQUIRE_FALSE(line.contains("build_interval"));
  REQUIRE_FALSE(line.contains("rebuild"));
}

// ---- A client's timeout in the reports (plan 2c Task 13) ----
//
// mm1(990): one M/M/1 server at rho 0.99 answers in 100 ms on average with variance 0.01 s^2 (an
// exponential sojourn: the standard deviation is the mean). A client `timeout` of 200 ms answers
// the lognormal's share below it, 0.894137, so of the 990 requests per second 885.2 are answered
// and 0.105863 x 990 = 104.8 per second time out; the answered mean is 73.97 ms. Percentiles are
// capped at the timeout: the p99 of the answered part would be 236.7 ms by the refitted lognormal,
// but no answered request took longer than 200 ms. (The theory tests derive the same numbers
// through the engine; these pin what the report prints.)

namespace {
// mm1(990) whose client has the given timeout (none: the client waits for ever).
Scenario timing_out(std::optional<simsys::SimTime> timeout) {
  auto s = mm1(990);
  s.nodes[0].workload->timeout = timeout;
  return s;
}
}  // namespace

TEST_CASE("the JSON op reports 0 timed out requests per second for a client without a timeout",
          "[report]") {
  const auto s = timing_out(std::nullopt);
  const auto g = *simsys::graph::Graph::build(s);
  const auto json = simsys::report::render_json(summarize(s, g), {});
  CAPTURE(json);
  REQUIRE(json.find("\"timed_out_per_s\": 0") != std::string::npos);
  REQUIRE(json_value(json, "timed_out_per_s") == 0.0);
}

TEST_CASE("the JSON op reports the requests that outlast the timeout: 104.8 of 990 per second",
          "[report]") {
  const auto s = timing_out(200ms);
  const auto g = *simsys::graph::Graph::build(s);
  const auto json = simsys::report::render_json(summarize(s, g), {});
  CAPTURE(json);
  const double offered = json_value(json, "offered_per_s");
  const double answered = json_value(json, "answered_per_s");
  const double timed_out = json_value(json, "timed_out_per_s");
  REQUIRE_THAT(timed_out, WithinRel(0.105863 * 990.0, 1e-5));
  REQUIRE_THAT(answered, WithinRel(0.894137 * 990.0, 1e-5));
  REQUIRE_THAT(answered + timed_out, WithinRel(offered, 1e-9));
  REQUIRE_THAT(json_value(json, "mean_s"), WithinRel(0.073970, 1e-5));
}

TEST_CASE(
    "the JSON p99 of a client with a timeout is the timeout when the answered tail reaches it",
    "[report]") {
  const auto s = timing_out(200ms);
  const auto g = *simsys::graph::Graph::build(s);
  const auto json = simsys::report::render_json(summarize(s, g), {});
  CAPTURE(json);
  REQUIRE_THAT(json_value(json, "p99_s"), WithinRel(0.2, 1e-12));
}

TEST_CASE("an op's percentiles never exceed the client's timeout, whatever its size", "[report]") {
  for (const auto timeout : {50ms, 200ms, 1000ms}) {
    CAPTURE(timeout.count());
    const auto s = timing_out(timeout);
    const auto g = *simsys::graph::Graph::build(s);
    const auto ops = summarize(s, g).ops();
    REQUIRE(ops.size() == 1);
    const double limit = std::chrono::duration<double>(timeout).count();
    REQUIRE(ops[0].p50_s <= limit);
    REQUIRE(ops[0].p95_s <= limit);
    REQUIRE(ops[0].p99_s <= limit);
  }
}

// 10.59 % of the delivered requests time out, more than the 5 % a p95 and the 1 % a p99 leave
// above them, so the client never saw those quantiles answered: the table prints `>200ms` there
// (review of plan 2c Task 13, I-1), not the 490 ms of the uncapped fit nor a capped number that
// looks measured. The p50 (half of the requests) was answered: a number. The `timed out/s` column
// shows the 104.8/s; the table has it because the client has a timeout.
TEST_CASE("the op table prints >200ms for a p99 more than 1 percent of whose requests timed out",
          "[report]") {
  const auto s = timing_out(200ms);
  const auto g = *simsys::graph::Graph::build(s);
  const auto table = simsys::report::render_table(summarize(s, g), {}, /*color=*/false);
  CAPTURE(table);
  REQUIRE(table.contains("timed out/s"));
  // Words of the row: `c.op offered timed_out mean p50 p95 p99`.
  const auto row = words_of_line_starting(table, "c.op");
  REQUIRE(row.size() == 7);
  REQUIRE(row[2] == "104.8");
  REQUIRE_FALSE(row[4].starts_with(">"));
  REQUIRE(row[5] == ">200ms");
  REQUIRE(row[6] == ">200ms");
}

// Without a timeout nothing times out, and the op table keeps its old six columns.
TEST_CASE("the op table has no timed out column when no client has a timeout", "[report]") {
  const auto s = timing_out(std::nullopt);
  const auto g = *simsys::graph::Graph::build(s);
  const auto table = simsys::report::render_table(summarize(s, g), {}, /*color=*/false);
  CAPTURE(table);
  REQUIRE_FALSE(table.contains("timed out"));
  REQUIRE(words_of_line_starting(table, "c.op").size() == 6);
}

// ---- SLOs per window: what the report prints (plan 2c Task 14) ----
//
// An SLO with `over: <duration>` is checked on every window of that length, aligned from the end
// of warmup, and the worst window decides (test_windows.cpp derives the numbers). The report must
// say WHICH window decided, or a failing SLO leaves the reader guessing which hour to look at:
//   table: the SLO's line ends with `(worst 10m from 00:20)`: the window's length (`over`) and its
//          start as a clock time of the first day, the form `explain` already prints times in;
//   JSON:  `"window_start_s": 1200`, the start in seconds from the start of the run (not from
//          the end of warmup), a key that only an SLO with `over` has (absent, not null, on the
//          others). Built through the loader, the one way to spell `over` today.
//
// The scenario: a client offering 1k/s to a service of 2k/s (rho 0.5) with a 1.8x spike from
// 00:20 (rho 0.9), step 1 s, as in test_windows.cpp.

namespace {
Scenario windowed(std::string_view timing, std::string_view rate, std::string_view slos) {
  const auto path = std::filesystem::temp_directory_path() /
                    (std::to_string(::getpid()) + "_simsys_report_windows.yaml");
  std::ofstream(path) << "version: 1\nname: windowed\n"
                      << timing << "nodes:\n  u: {type: client, rate: " << rate
                      << ", ops: {get: 1}}\n  s: {type: service, capacity: 2k/s, latency: 1ms}\n"
                         "links:\n  - {from: u, to: s, dir: inout, latency: 0s}\nslo:\n"
                      << slos;
  auto loaded = simsys::config::load(path.string());
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  if (!loaded) FAIL(loaded.error().message);
  return std::move(*loaded);
}

struct Printed {
  std::string table;
  std::string json;
  std::vector<simsys::report::SloResult> results;
};

Printed printed(const Scenario& s) {
  const auto g = *simsys::graph::Graph::build(s);
  const auto summary = summarize(s, g);
  auto results = simsys::report::evaluate_slos(summary, s.slos);
  return {simsys::report::render_table(summary, results, /*color=*/false),
          simsys::report::render_json(summary, results), std::move(results)};
}

// The first line of `text` that contains `needle`.
std::string line_containing(const std::string& text, std::string_view needle) {
  std::istringstream lines{text};
  std::string line;
  while (std::getline(lines, line)) {
    if (line.contains(needle)) return line;
  }
  FAIL("no line contains " << needle << " in:\n" << text);
  return {};
}

constexpr std::string_view the_spike = "{base: 1k/s, spikes: [{at: 00:20, for: 10m, factor: 1.8}]}";
constexpr std::string_view an_hour = "duration: 1h\nstep: 1s\n";
}  // namespace

TEST_CASE("the SLO table says which window was the worst: (worst 10m from 00:20)", "[report]") {
  const auto out = printed(
      windowed(an_hour, the_spike, "  - {metric: utilization, node: s, max: 0.8, over: 10m}\n"));
  CAPTURE(out.table);
  const auto line = line_containing(out.table, "utilization");
  REQUIRE(line.contains("FAIL"));
  REQUIRE(line.contains("(worst 10m from 00:20)"));
}

// 15 m windows: the spike straddles 00:15-00:30 and 00:30-00:45 (see test_windows.cpp); the worst
// starts at 00:15, which the line names in the same form.
TEST_CASE("the SLO table names the window's length as written in over and its start as a clock",
          "[report]") {
  const auto out = printed(
      windowed(an_hour, the_spike, "  - {metric: utilization, node: s, max: 0.8, over: 15m}\n"));
  CAPTURE(out.table);
  const auto line = line_containing(out.table, "utilization");
  REQUIRE(line.contains("PASS"));
  REQUIRE(line.contains("(worst 15m from 00:15)"));
}

// With 5 m of warmup the windows start at 00:05, 00:15, ...; the worst of a spike over 00:20-00:28
// is 00:15-00:25. The table and the JSON both count from the START OF THE RUN (00:15, 900 s), the
// same clock as the spike's `at`, not from the end of warmup (which would say 10 m, 600 s).
TEST_CASE("the window's start is counted from the start of the run, warmup included", "[report]") {
  const auto out = printed(windowed("duration: 1h\nstep: 1s\nwarmup: 5m\n",
                                    "{base: 1k/s, spikes: [{at: 00:20, for: 8m, factor: 1.8}]}",
                                    "  - {metric: utilization, node: s, max: 0.8, over: 10m}\n"));
  CAPTURE(out.table, out.json);
  REQUIRE(line_containing(out.table, "utilization").contains("(worst 10m from 00:15)"));
  REQUIRE(
      line_containing(out.json, "\"metric\": \"utilization\"").contains("\"window_start_s\": 900"));
}

TEST_CASE("the JSON gives an SLO with over its window_start_s, in seconds from the run's start",
          "[report]") {
  const auto out = printed(
      windowed(an_hour, the_spike, "  - {metric: utilization, node: s, max: 0.8, over: 10m}\n"));
  CAPTURE(out.json);
  const auto line = line_containing(out.json, "\"metric\": \"utilization\"");
  REQUIRE(line.contains("\"window_start_s\": 1200"));
  // The other fields of the entry are the ones every SLO has.
  REQUIRE(line.contains("\"status\": \"fail\""));
  REQUIRE(line.contains("\"max\": 0.8"));
}

// What does not change: an SLO without `over` prints as before, with no window text and no key.
// This one passes today and must keep passing.
TEST_CASE("an SLO without over prints no window: no worst text in the table, no key in the JSON",
          "[report]") {
  const auto out =
      printed(windowed(an_hour, the_spike, "  - {metric: utilization, node: s, max: 0.8}\n"));
  CAPTURE(out.table, out.json);
  // The whole hour: PASS at (0.5 x 50 + 0.9 x 10) / 60 = 0.5667.
  REQUIRE(out.results.size() == 1);
  REQUIRE(out.results[0].status == simsys::report::SloStatus::pass);
  REQUIRE_THAT(*out.results[0].observed, WithinRel(34.0 / 60.0, 1e-9));
  REQUIRE_FALSE(line_containing(out.table, "PASS").contains("worst"));
  REQUIRE_FALSE(
      line_containing(out.json, "\"metric\": \"utilization\"").contains("window_start_s"));
}

// An SLO whose every window offered nothing is skipped (test_windows.cpp), and a skipped verdict
// names no window: the table prints SKIP with the observed `-` and no "worst".
TEST_CASE("a windowed SLO that was skipped names no window in the table", "[report]") {
  const auto s = windowed("duration: 10m\nstep: 1s\n", "{points: [[0s, 0/s], [10m, 0/s]]}",
                          "  - {metric: p99_latency, node: u, max: 50ms, over: 5m}\n");
  const auto out = printed(s);
  CAPTURE(out.table);
  REQUIRE(out.results.size() == 1);
  REQUIRE(out.results[0].status == simsys::report::SloStatus::skipped);
  const auto line = line_containing(out.table, "p99_latency");
  REQUIRE(line.contains("SKIP"));
  REQUIRE_FALSE(line.contains("worst"));
}
