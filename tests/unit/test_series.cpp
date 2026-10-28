// The series files and the explain marks of phase 2b's refinement 7: what a student can check
// from the output alone.
//  - the node series carries each node's demand and drain, so the conservation law
//    Σ demand·dt = Σ (drain + drops)·dt + backlog can be checked from the CSV;
//  - a second file carries, per client op, what was offered and what came back answered;
//  - the JSON op entries carry `answered_per_s` beside `offered_per_s`;
//  - explain stops printing a green "0 %" for a node whose load it cannot follow yet.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions when a
// std header is included after a module that included it in its global module fragment.
import simsys.config;
import simsys.core;
import simsys.engine;
import simsys.graph;
import simsys.model;
import simsys.report;

using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using simsys::Rate;
using simsys::model::LimiterAlgorithm;
using simsys::model::LinkSpec;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
using simsys::model::Workload;

namespace {

// c(`demand`/s, op "op") -> s (1k/s, 1 ms), with an optional waiting-room limit. At 1500/s
// the server is overloaded by 500/s, so a queue_limit of 100 fills and sheds.
Scenario mm1(double demand, std::optional<double> queue_limit = std::nullopt) {
  // C++20 designated initializers: `.name = ...` names each member being set, in declaration
  // order, and leaves the rest at their defaults; a scenario built in code reads like its YAML
  // and does not break silently when members are reordered, as positional braces would.
  Scenario s{.name = "mm1", .version = 1, .duration = 500ms, .step = 100ms};
  s.nodes = {
      NodeSpec{.id = "c",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{demand}}, .op_shares = {{"op", 1.0}}}},
      NodeSpec{.id = "s",
               .type = NodeType::service,
               .capacity = Rate{1000},
               .latency = 1ms,
               .advanced = {.queue_limit = queue_limit}}};
  s.links = {LinkSpec{.from = "c", .to = "s", .latency = 0s}};
  return s;
}

// u(4000/s) -> limiter (token bucket, `limit`/s, burst 100) -> api (10k/s): after the first
// step (whose bucket still holds its burst) the limiter admits `limit`/s and refuses the rest.
Scenario limited(double limit) {
  Scenario s{.name = "limited", .version = 1, .duration = 1s, .step = 100ms, .warmup = 500ms};
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{4000}}, .op_shares = {{"call", 1.0}}}},
      NodeSpec{.id = "limiter",
               .type = NodeType::rate_limiter,
               .capacity = Rate{1e6},
               .algorithm = LimiterAlgorithm::token_bucket,
               .limit = Rate{limit},
               .burst = 100},
      NodeSpec{.id = "api", .type = NodeType::service, .capacity = Rate{10'000}}};
  s.links = {LinkSpec{.from = "u", .to = "limiter", .latency = 0s},
             LinkSpec{.from = "limiter", .to = "api", .latency = 0s}};
  return s;
}

simsys::graph::Graph graph_of(const Scenario& s) {
  auto built = simsys::graph::Graph::build(s);
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  return *built;
}

// Runs the scenario and hands every step's record to `observer`, the Observer pattern the
// CLI uses (src/cli/main.cpp): the writers and the summary never know of one another.
template <class Observer>
void run_into(const Scenario& s, const simsys::graph::Graph& g, Observer&& observer) {
  auto sim = *simsys::engine::Simulation::create(g);
  sim.run({.duration = s.duration, .step = s.step}, observer);
}

// The text of one CSV line split at commas: `line` is a row, with no newline.
std::vector<std::string> split_commas(const std::string& line) {
  std::vector<std::string> fields;
  std::string field;
  std::istringstream cells{line};
  while (std::getline(cells, field, ',')) fields.push_back(field);
  return fields;
}

// The data rows of a series file (everything after the header), each split into fields.
std::vector<std::vector<std::string>> data_rows(const std::string& text) {
  std::vector<std::vector<std::string>> rows;
  std::istringstream lines{text};
  std::string line;
  std::getline(lines, line);  // the header
  while (std::getline(lines, line)) rows.push_back(split_commas(line));
  return rows;
}

// The header line of a series file, without its newline.
std::string header_of(const std::string& text) {
  return text.substr(0, text.find('\n'));
}

// The number a CSV cell holds. std::from_chars (C++17, <charconv>) parses without locale or
// exceptions and reports failure in its result; a cell that is not a number fails the test.
double number(const std::string& cell) {
  double value{};
  const auto parsed = std::from_chars(cell.data(), cell.data() + cell.size(), value);
  INFO("cell: '" << cell << "'");
  REQUIRE(parsed.ec == std::errc{});
  return value;
}

constexpr std::string_view node_header =
    "t_s,node,demand_per_s,drain_per_s,rho,backlog,drops_per_s,rejected_per_s,latency_ms,zone";
constexpr std::string_view op_header = "t_s,client,op,offered_per_s,answered_per_s,mean_ms";

// The line of explain's checks block (after "checks (") that starts, after its indent, with
// `id` and a space; empty when there is none.
std::string check_line(const std::string& text, const std::string& id) {
  const auto at = text.find("\n  " + id + " ", text.find("checks ("));
  if (at == std::string::npos) return {};
  const auto end = text.find('\n', at + 1);
  return text.substr(at + 1, end - at - 1);
}

// `explain` of a shipped book scenario, as `simsys explain scenarios/book/<name>/scenario.yaml`
// prints it. SIMSYS_SCENARIOS_DIR is injected by tests/CMakeLists.txt, like in scenario_tests.
std::string explain_shipped(const std::string& name) {
  const auto loaded =
      simsys::config::load(std::string{SIMSYS_SCENARIOS_DIR} + "/book/" + name + "/scenario.yaml");
  INFO((loaded ? "" : loaded.error().message));
  REQUIRE(loaded.has_value());
  return simsys::report::render_explain(graph_of(*loaded));
}

// The first number after `key` (a quoted JSON key, with its colon) at or after `from`, and the
// position just past it; the JSON writer prints `"key": 123`.
struct Found {
  double value{};
  std::size_t end{};
};
Found number_after(const std::string& json, std::string_view key, std::size_t from) {
  const auto at = json.find(key, from);
  INFO("key: " << key);
  REQUIRE(at != std::string::npos);
  const auto begin = json.data() + at + key.size();
  Found found{};
  const auto parsed = std::from_chars(begin, json.data() + json.size(), found.value);
  REQUIRE(parsed.ec == std::errc{});
  found.end = static_cast<std::size_t>(parsed.ptr - json.data());
  return found;
}

// A run's JSON report, SLOs left out.
std::string json_of(const Scenario& s) {
  const auto g = graph_of(s);
  simsys::report::Summary summary{g, s.warmup};
  run_into(s, g, [&](const auto& r) { summary.observe(r); });
  return simsys::report::render_json(summary, {});
}
}  // namespace

// ---- node series: demand_per_s and drain_per_s ----

TEST_CASE("the node series header has demand_per_s and drain_per_s right after node", "[series]") {
  const auto s = mm1(500);
  const auto g = graph_of(s);
  std::ostringstream out;
  simsys::report::SeriesWriter writer{out, g};  // the header is written on construction
  REQUIRE(header_of(out.str()) == node_header);
}

TEST_CASE("a node series row carries the demand and drain of the NodeSample it was fed",
          "[series]") {
  const auto s = mm1(500);
  const auto g = graph_of(s);
  std::ostringstream out;
  simsys::report::SeriesWriter writer{out, g};
  // A hand-made record, so that every column holds a different, exactly printable number:
  // c is the client (index 0, all zeros), s the server (index 1).
  std::vector<simsys::engine::NodeSample> nodes(2);
  nodes[1] = simsys::engine::NodeSample{.demand_per_s = 1234.5,
                                        .drain_per_s = 1000.25,
                                        .dropped_per_s = 200,
                                        .rejected_per_s = 7,
                                        .backlog = 34.25,
                                        .rho = 0.8,
                                        .latency = {.mean_s = 0.0125, .variance_s2 = 0.0}};
  writer.observe(simsys::engine::StepRecord{.t = 100ms, .nodes = nodes, .responses = {}});
  const auto rows = data_rows(out.str());
  REQUIRE(rows.size() == 2);  // one row per node: the client's and the server's
  const auto& server = rows[1];
  REQUIRE(server.size() == 10);
  REQUIRE(server[0] == "0.1");
  REQUIRE(server[1] == "s");
  REQUIRE(server[2] == "1234.5");   // demand_per_s
  REQUIRE(server[3] == "1000.25");  // drain_per_s
  REQUIRE(server[4] == "0.8");      // rho
  REQUIRE(server[5] == "34.25");    // backlog
  REQUIRE(server[6] == "200");      // drops_per_s
  REQUIRE(server[7] == "7");        // rejected_per_s
  REQUIRE(server[8] == "12.5");     // latency_ms: 0.0125 s
  REQUIRE(server[9] == "green");    // the NodeSample's default zone
}

TEST_CASE("a client row has zeros for demand and drain: a source queues and serves nothing",
          "[series]") {
  const auto s = mm1(500);
  const auto g = graph_of(s);
  std::ostringstream out;
  simsys::report::SeriesWriter writer{out, g};
  run_into(s, g, [&](const auto& r) { writer.observe(r); });
  const auto text = out.str();
  CAPTURE(text);
  // t, node, demand, drain, rho, backlog, drops, rejected, latency, zone: a client's engine
  // sample is all zeros (engine.cppm: "a source has no queue") and its zone is `none`.
  // C++23 std::string::contains: a substring test in one call, replacing
  // `find(...) != std::string::npos`.
  REQUIRE(text.contains("\n0,c,0,0,0,0,0,0,0,none\n"));
}

TEST_CASE("every row of a run has ten fields, one row per node per step", "[series]") {
  const auto s = mm1(500);  // 500 ms at 100 ms steps: 5 steps
  const auto g = graph_of(s);
  std::ostringstream out;
  simsys::report::SeriesWriter writer{out, g};
  run_into(s, g, [&](const auto& r) { writer.observe(r); });
  const auto rows = data_rows(out.str());
  REQUIRE(rows.size() == 5 * 2);  // 5 steps x 2 nodes (the client and the server)
  // C++20 std::ranges::all_of: the algorithm takes the whole range and a predicate, replacing
  // the begin()/end() pair of std::all_of.
  REQUIRE(std::ranges::all_of(rows, [](const auto& row) { return row.size() == 10; }));
}

// The fluid queue's balance, step by step: demand·dt = (drain + dropped)·dt + Δbacklog. Summed
// over a run that starts empty it says: everything that came in was served, shed, or is still
// waiting. With the demand and drain columns a student checks it from the file alone.
TEST_CASE("the conservation identity holds when read back from the series file", "[series]") {
  // 1500/s into a 1000/s server with a waiting room of 100: the backlog grows by
  // (1500 - 1000) x 0.1 s = 50 per step, is full (100) after two steps, and from then on the
  // excess 500/s is shed: the run has all three terms of the identity (served, shed, waiting).
  const auto s = mm1(1500, 100);
  const auto g = graph_of(s);
  std::ostringstream out;
  simsys::report::SeriesWriter writer{out, g};
  run_into(s, g, [&](const auto& r) { writer.observe(r); });
  const auto rows = data_rows(out.str());
  CAPTURE(out.str());

  constexpr double dt = 0.1;  // the step, in seconds
  double demand = 0.0;
  double drain = 0.0;
  double drops = 0.0;
  double backlog = 0.0;
  for (const auto& row : rows) {
    if (row[1] != "s") continue;
    demand += number(row[2]) * dt;
    drain += number(row[3]) * dt;
    drops += number(row[6]) * dt;
    backlog = number(row[5]);  // the last row's: what is left waiting at the end
  }
  REQUIRE_THAT(demand, WithinRel(750.0, 1e-5));  // 5 steps x 0.1 s x 1500/s
  REQUIRE(drops > 0.0);                          // not vacuous: the queue really shed
  REQUIRE(backlog > 0.0);                        // ... and really holds some
  REQUIRE_THAT(demand, WithinRel(drain + drops + backlog, 1e-5));
}

// A token-family limiter refuses a request after its queue drained it, so its rejections are
// INSIDE `drain` (engine.cppm, NodeSample): the identity needs no rejected term.
TEST_CASE("a limiter's rejections are inside its drain, so the identity needs no rejected term",
          "[series]") {
  const auto s = limited(1000);
  const auto g = graph_of(s);
  std::ostringstream out;
  simsys::report::SeriesWriter writer{out, g};
  run_into(s, g, [&](const auto& r) { writer.observe(r); });
  constexpr double dt = 0.1;
  double demand = 0.0;
  double drain = 0.0;
  double drops = 0.0;
  double rejected = 0.0;
  double backlog = 0.0;
  for (const auto& row : data_rows(out.str())) {
    if (row[1] != "limiter") continue;
    demand += number(row[2]) * dt;
    drain += number(row[3]) * dt;
    drops += number(row[6]) * dt;
    rejected += number(row[7]) * dt;
    backlog = number(row[5]);
  }
  REQUIRE_THAT(demand, WithinRel(4000.0, 1e-5));  // 10 steps x 0.1 s x 4000/s
  REQUIRE(rejected > 0.0);                        // the limiter did refuse requests
  REQUIRE(rejected <= drain);                     // ... and they are part of what it drained
  REQUIRE_THAT(demand, WithinRel(drain + drops + backlog, 1e-5));
}

// ---- op series: offered_per_s, answered_per_s, mean_ms ----

TEST_CASE("the op series has its own writer and header, written on construction", "[series]") {
  const auto s = mm1(500);
  const auto g = graph_of(s);
  std::ostringstream out;
  simsys::report::OpSeriesWriter writer{out, g};
  REQUIRE(out.str() == std::string{op_header} + "\n");
}

TEST_CASE("the op series has one row per client op per step", "[series]") {
  const auto s = mm1(500);  // one client, one op, 5 steps
  const auto g = graph_of(s);
  std::ostringstream out;
  simsys::report::OpSeriesWriter writer{out, g};
  run_into(s, g, [&](const auto& r) { writer.observe(r); });
  const auto rows = data_rows(out.str());
  CAPTURE(out.str());
  REQUIRE(rows.size() == 5);  // 5 steps x 1 (client, op)
  REQUIRE(rows[0].size() == 6);
  REQUIRE(rows[0][0] == "0");
  REQUIRE(rows[0][1] == "c");
  REQUIRE(rows[0][2] == "op");
  REQUIRE(rows[1][0] == "0.1");

  // Two ops of the same client in one step are two rows, in the record's order (by name).
  std::ostringstream two;
  simsys::report::OpSeriesWriter two_writer{two, g};
  two_writer.observe(simsys::engine::StepRecord{
      .t = 100ms,
      .nodes = std::vector<simsys::engine::NodeSample>(2),
      .responses = {simsys::engine::ResponseSample{.client = 0,
                                                   .op = "get",
                                                   .offered = Rate{100},
                                                   .answered = Rate{100},
                                                   .response = {.mean_s = 0.001}},
                    simsys::engine::ResponseSample{.client = 0,
                                                   .op = "put",
                                                   .offered = Rate{10},
                                                   .answered = Rate{5},
                                                   .response = {.mean_s = 0.002}}}});
  const auto pair = data_rows(two.str());
  REQUIRE(pair.size() == 2);
  REQUIRE(pair[0][2] == "get");
  REQUIRE(pair[1][2] == "put");
}

TEST_CASE("an op series row holds the sample's offered and answered rates and its mean in ms",
          "[series]") {
  const auto s = mm1(500);
  const auto g = graph_of(s);
  std::ostringstream out;
  simsys::report::OpSeriesWriter writer{out, g};
  writer.observe(simsys::engine::StepRecord{
      .t = 200ms,
      .nodes = std::vector<simsys::engine::NodeSample>(2),
      .responses = {simsys::engine::ResponseSample{.client = 0,
                                                   .op = "op",
                                                   .offered = Rate{250.5},
                                                   .answered = Rate{125.25},
                                                   .response = {.mean_s = 0.0125}}}});
  const auto rows = data_rows(out.str());
  REQUIRE(rows.size() == 1);
  // t_s, client, op, offered_per_s, answered_per_s, mean_ms: 0.0125 s is 12.5 ms.
  REQUIRE(rows[0] == std::vector<std::string>{"0.2", "c", "op", "250.5", "125.25", "12.5"});
}

// Same convention as the node series (report.cpp, SeriesWriter::observe): a CSV cell is
// always a number a plotting tool parses; an infinite latency is `inf`, not blank or null.
TEST_CASE("an op series row with an infinite mean writes inf", "[series]") {
  const auto s = mm1(500);
  const auto g = graph_of(s);
  std::ostringstream out;
  simsys::report::OpSeriesWriter writer{out, g};
  writer.observe(simsys::engine::StepRecord{
      .t = 100ms,
      .nodes = std::vector<simsys::engine::NodeSample>(2),
      .responses = {simsys::engine::ResponseSample{
          .client = 0,
          .op = "op",
          .offered = Rate{100},
          .answered = Rate{0},
          .response = {.mean_s = std::numeric_limits<double>::infinity()}}}});
  const auto rows = data_rows(out.str());
  REQUIRE(rows.size() == 1);
  REQUIRE(rows[0][5] == "inf");
}

TEST_CASE("answered never exceeds offered, and falls below it when a queue sheds", "[series]") {
  // 1500/s into a 1000/s server with a waiting room of 100: from the third step on, a third of
  // the requests are dropped ((1500 - 1000) / 1500), so answered is about 1000/s of 1500/s.
  const auto s = mm1(1500, 100);
  const auto g = graph_of(s);
  std::ostringstream out;
  simsys::report::OpSeriesWriter writer{out, g};
  run_into(s, g, [&](const auto& r) { writer.observe(r); });
  const auto rows = data_rows(out.str());
  CAPTURE(out.str());
  REQUIRE(rows.size() == 5);
  bool some_below = false;
  for (const auto& row : rows) {
    const double offered = number(row[3]);
    const double answered = number(row[4]);
    REQUIRE_THAT(offered, WithinRel(1500.0, 1e-9));
    REQUIRE(answered <= offered * (1.0 + 1e-9));
    some_below = some_below || answered < offered * (1.0 - 1e-3);
  }
  REQUIRE(some_below);
}

TEST_CASE("answered equals offered when nothing is shed", "[series]") {
  const auto s = mm1(500);  // 500/s into 1000/s: rho 0.5, no drops, no refusals
  const auto g = graph_of(s);
  std::ostringstream out;
  simsys::report::OpSeriesWriter writer{out, g};
  run_into(s, g, [&](const auto& r) { writer.observe(r); });
  for (const auto& row : data_rows(out.str())) {
    REQUIRE_THAT(number(row[4]), WithinRel(number(row[3]), 1e-9));
    REQUIRE_THAT(number(row[3]), WithinRel(500.0, 1e-9));
  }
}

TEST_CASE("a limiter that refuses half of the requests shows answered = offered / 2", "[series]") {
  // u offers 4000/s; the limiter admits 2000/s once its burst is gone (after the first step),
  // so the delivered fraction is 2000 / 4000 = 1/2 and answered = 4000 x 1/2 = 2000/s; the api
  // (10k/s) drops nothing on the way.
  const auto s = limited(2000);
  const auto g = graph_of(s);
  std::ostringstream out;
  simsys::report::OpSeriesWriter writer{out, g};
  run_into(s, g, [&](const auto& r) { writer.observe(r); });
  const auto text = out.str();
  CAPTURE(text);
  const auto rows = data_rows(text);
  REQUIRE(rows.size() == 10);  // 10 steps of 100 ms
  const auto& last = rows.back();
  REQUIRE(last[0] == "0.9");
  REQUIRE(last[1] == "u");
  REQUIRE(last[2] == "call");
  REQUIRE_THAT(number(last[3]), WithinRel(4000.0, 1e-9));
  REQUIRE_THAT(number(last[4]), WithinRel(2000.0, 1e-6));
}

// ---- JSON: answered_per_s beside offered_per_s ----

TEST_CASE("every op in the JSON has answered_per_s, never above offered_per_s", "[series]") {
  const auto json = json_of(limited(2000));
  CAPTURE(json);
  const auto ops_at = json.find("\"ops\"");
  REQUIRE(ops_at != std::string::npos);
  const auto offered = number_after(json, "\"offered_per_s\": ", ops_at);
  // The new field sits next to offered_per_s in the same op entry.
  const auto answered = number_after(json, "\"answered_per_s\": ", ops_at);
  REQUIRE(answered.value <= offered.value * (1.0 + 1e-9));
  // 4000/s offered, the limiter admits 2000/s (see the op series test): half is answered,
  // averaged over the steps after the 500 ms warmup, all of them steady.
  REQUIRE_THAT(offered.value, WithinRel(4000.0, 1e-6));
  REQUIRE_THAT(answered.value, WithinRel(2000.0, 1e-3));
}

TEST_CASE("the JSON's answered_per_s equals offered_per_s when nothing is dropped", "[series]") {
  const auto json = json_of(mm1(500));
  CAPTURE(json);
  const auto ops_at = json.find("\"ops\"");
  REQUIRE(ops_at != std::string::npos);
  const auto offered = number_after(json, "\"offered_per_s\": ", ops_at);
  const auto answered = number_after(json, "\"answered_per_s\": ", ops_at);
  REQUIRE_THAT(offered.value, WithinRel(500.0, 1e-9));
  REQUIRE_THAT(answered.value, WithinRel(offered.value, 1e-9));
}

// ---- explain: load that the paper pass cannot follow is not a green verdict ----
//
// The paper load (report.cpp: paper_pass / forward_steps) forwards load from a client's routes,
// from a service or worker over its `ops` steps, through load balancers and rate limiters over
// their links (graph::forwards_by_link) and from caches and CDNs on misses (hits_or_misses).
// A blob store's `then`, a message queue's consumers and a worker pool's `then` are not
// followed yet (phase 2c), so the nodes that only they feed show 0 offered load: before this
// change, `0%, green`, a verdict on a number that is missing, not zero.

TEST_CASE("explain marks the youtube nodes behind the blob store and queues as not propagated",
          "[series]") {
  const auto text = explain_shipped("youtube");
  CAPTURE(text);
  // original_store (blob_store) receives the uploads from the clients directly, and its `then`
  // publishes to transcode_q, which the paper load does not follow: transcode_q shows 0/s.
  const auto queue = check_line(text, "transcode_q");
  REQUIRE_FALSE(queue.empty());
  REQUIRE(queue.contains("(load not propagated: blob_store is phase 2c)"));
  REQUIRE_FALSE(queue.contains("green"));
  REQUIRE_FALSE(queue.contains("%"));
  // transcoders consume transcode_q, a message queue, which does not forward either; and
  // completion_handler sits behind completion_q behind the transcoders. None has a verdict.
  for (const auto* id : {"transcoders", "completion_q", "completion_handler"}) {
    CAPTURE(id);
    const auto line = check_line(text, id);
    REQUIRE_FALSE(line.empty());
    REQUIRE(line.contains("load not propagated"));
    REQUIRE_FALSE(line.contains("green"));
  }
}

TEST_CASE("explain marks the google-drive consumers of notify_q, and keeps notify_q's verdict",
          "[series]") {
  const auto text = explain_shipped("google-drive");
  CAPTURE(text);
  // api (a service) forwards `publish` to notify_q over its ops steps: notify_q has its load
  // and keeps its verdict. Its consumers are fed only through the queue.
  const auto queue = check_line(text, "notify_q");
  REQUIRE(queue.contains("green"));
  REQUIRE_FALSE(queue.contains("load not propagated"));
  for (const auto* id : {"notification_svc", "backup_workers"}) {
    CAPTURE(id);
    const auto line = check_line(text, id);
    REQUIRE_FALSE(line.empty());
    REQUIRE(line.contains("(load not propagated: message_queue is phase 2c)"));
    REQUIRE_FALSE(line.contains("green"));
  }
}

TEST_CASE("nodes the paper load does reach keep their verdict and are not marked", "[series]") {
  const auto text = explain_shipped("youtube");
  CAPTURE(text);
  // lb (a load balancer), api (a service behind it) and metadata_cache (a cache behind api's
  // steps) are reached through forwarding types: a percentage and a zone, no mark.
  for (const auto* id : {"lb", "api", "metadata_cache"}) {
    CAPTURE(id);
    const auto line = check_line(text, id);
    REQUIRE_FALSE(line.empty());
    REQUIRE(line.contains("green"));
    REQUIRE_FALSE(line.contains("load not propagated"));
  }
  // The CDNs and the blob stores fed straight from clients or caches carry their own note
  // (capacity from bytes: no verdict), not the propagation mark.
  for (const auto* id : {"cdn_us", "original_store"}) {
    CAPTURE(id);
    const auto line = check_line(text, id);
    REQUIRE(line.contains("not checked: capacity from"));
    REQUIRE_FALSE(line.contains("load not propagated"));
  }
}
