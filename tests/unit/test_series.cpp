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
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions when a
// std header is included after a module that included it in its global module fragment.
import simsys.config;
import simsys.core;
import simsys.engine;
import simsys.fluid; // Zone: a hand-made LinkSample's
import simsys.graph;
import simsys.model;
import simsys.report;
import simsys.workload; // Spike: the probe's 20 s of triple load

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
// step (whose bucket still holds its burst) the limiter admits `limit`/s and refuses the rest,
// the same share of every op (`ops`, sorted by name: one op `call` unless a test asks for more).
Scenario limited(double limit, std::vector<std::pair<std::string, double>> ops = {{"call", 1.0}}) {
  Scenario s{.name = "limited", .version = 1, .duration = 1s, .step = 100ms, .warmup = 500ms};
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{4000}}, .op_shares = std::move(ops)}},
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

// c(500/s, op "op", tripled for 20 s from t = 10 s) -> s (1k/s, 1 ms, a waiting room of 1M):
// the spike brings 1500/s for 20 s, 500/s more than s serves, so the backlog grows to
// 500/s x 20 s = 10k requests and then drains at 1000 - 500 = 500/s for another 20 s. The room
// never fills: nothing is dropped, every request is answered, some only after a long wait.
Scenario spike_into_big_queue() {
  Scenario s{.name = "spike", .version = 1, .duration = 60s, .step = 1s};
  s.nodes = {
      NodeSpec{.id = "c",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{500},
                                              .spikes = {simsys::workload::Spike{
                                                  .at = 10s, .duration = 20s, .factor = 3.0}}},
                                    .op_shares = {{"op", 1.0}}}},
      NodeSpec{.id = "s",
               .type = NodeType::service,
               .capacity = Rate{1000},
               .latency = 1ms,
               .advanced = {.queue_limit = 1e6}}};
  s.links = {LinkSpec{.from = "c", .to = "s", .latency = 0s}};
  return s;
}

// Two clients feed `db` (a service of 2k/s): `reader` sends `read_rate` of `read` straight to it
// over a client route, which the paper load follows; `uploader` sends 10k/s of `put` to `blobs`,
// a blob store whose `then` writes each upload's metadata to db (an event notification: the paper
// load follows it since plan 2c Task 4; before, it did not and db's load was only the reader's
// part: the probe of the review of 3e23477, I1). A blob store notifies only for its writes (review
// of Task 4, I1), so `blobs` lists `put` in its `write_ops` (S3's PUT, ObjectCreated): with the
// default `[write]` a put would be a read and notify nobody. `blobs` also has a link to `audit`
// but no step along it: nothing flows there.
Scenario partly_behind_blob_store(double read_rate) {
  Scenario s{.name = "partial", .version = 1, .duration = 1s, .step = 100ms};
  s.nodes = {
      NodeSpec{
          .id = "reader",
          .type = NodeType::client,
          .workload = Workload{.curve = {.base = Rate{read_rate}}, .op_shares = {{"read", 1.0}}}},
      NodeSpec{.id = "uploader",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{10'000}}, .op_shares = {{"put", 1.0}}}},
      NodeSpec{.id = "blobs",
               .type = NodeType::blob_store,
               .capacity = Rate{1e6},
               .then = {simsys::model::Step{.to = "db", .op = "write"}},
               .write_ops = std::vector<std::string>{"put"}},
      NodeSpec{.id = "db", .type = NodeType::service, .capacity = Rate{2000}},
      NodeSpec{.id = "audit", .type = NodeType::service, .capacity = Rate{2000}}};
  s.links = {LinkSpec{.from = "reader", .to = "db"}, LinkSpec{.from = "uploader", .to = "blobs"},
             LinkSpec{.from = "blobs", .to = "db"}, LinkSpec{.from = "blobs", .to = "audit"}};
  return s;
}

// The same two clients and db, but the 10k/s of `post` go to `fanout`, a notification fan-out
// (ch. 10) whose `then` sends each post to db as a `write`: a type the paper load does not follow
// ("not modelled yet"), so db's load is only the reader's part, a lower bound. `fanout` also has
// a link to `audit` but no step along it: nothing would flow there.
Scenario partly_behind_fanout(double read_rate) {
  Scenario s{.name = "partial-fanout", .version = 1, .duration = 1s, .step = 100ms};
  s.nodes = {
      NodeSpec{
          .id = "reader",
          .type = NodeType::client,
          .workload = Workload{.curve = {.base = Rate{read_rate}}, .op_shares = {{"read", 1.0}}}},
      NodeSpec{.id = "poster",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{10'000}}, .op_shares = {{"post", 1.0}}}},
      NodeSpec{.id = "fanout",
               .type = NodeType::notification_fanout,
               .then = {simsys::model::Step{.to = "db", .op = "write"}}},
      NodeSpec{.id = "db", .type = NodeType::service, .capacity = Rate{2000}},
      NodeSpec{.id = "audit", .type = NodeType::service, .capacity = Rate{2000}}};
  s.links = {LinkSpec{.from = "reader", .to = "db"}, LinkSpec{.from = "poster", .to = "fanout"},
             LinkSpec{.from = "fanout", .to = "db"}, LinkSpec{.from = "fanout", .to = "audit"}};
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

// `explain` of a scenario built in code.
std::string explain_of(const Scenario& s) {
  return simsys::report::render_explain(graph_of(s));
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

// ---- answered counts what was lost, not what was served in the same step ----
//
// A request that waits in a growing queue is answered later, not lost. The per-step delivered
// share at a queue is therefore 1 − (dropped + rejected) / inflow (engine.cppm,
// account_rejections): only a drop or a refusal makes a request unanswered, and a backlog that
// grows and drains again is neutral. Before (review of 3e23477, I2) it was served / inflow capped
// at 1, which counted every request still waiting at the end of its arrival step as unanswered
// and never credited it later: the review's probe, this spike over a 2-minute run, reported
// 583/s answered of 667/s offered with nothing dropped.

TEST_CASE("a backlog that grows and drains without drops leaves answered equal to offered",
          "[series]") {
  const auto s = spike_into_big_queue();
  const auto g = graph_of(s);
  double most_waiting = 0.0;
  double dropped = 0.0;
  int steps = 0;
  run_into(s, g, [&](const simsys::engine::StepRecord& r) {
    ++steps;
    most_waiting = std::max(most_waiting, r.nodes[1].backlog);
    dropped += r.nodes[1].dropped_per_s;
    REQUIRE(r.responses.size() == 1);
    const auto& op = r.responses[0];
    CAPTURE(std::chrono::duration<double>(r.t).count(), op.offered.per_second,
            op.answered.per_second, r.nodes[1].backlog);
    REQUIRE_THAT(op.answered.per_second, WithinRel(op.offered.per_second, 1e-9));
  });
  REQUIRE(steps == 60);
  // Not vacuous: the queue really held the spike's excess, 500/s x 20 s, and dropped none.
  REQUIRE_THAT(most_waiting, WithinRel(10'000.0, 1e-6));
  REQUIRE(dropped == 0.0);
}

TEST_CASE("a saturated queue answers what it was offered minus what it dropped, every step",
          "[series]") {
  // 1500/s into 1000/s with a waiting room of 100: the room fills in two steps (50 per step),
  // and from then on 500/s is dropped. In the filling steps nothing is dropped, so everything
  // offered is answered (the 50 that wait are answered later); after that 1500 - 500 = 1000/s.
  // Every request costs one capacity unit, so drops_per_s counts requests.
  const auto s = mm1(1500, 100);
  const auto g = graph_of(s);
  bool some_dropped = false;
  run_into(s, g, [&](const simsys::engine::StepRecord& r) {
    const auto& op = r.responses.at(0);
    const double dropped = r.nodes[1].dropped_per_s;
    CAPTURE(std::chrono::duration<double>(r.t).count(), op.offered.per_second,
            op.answered.per_second, dropped);
    REQUIRE_THAT(op.answered.per_second, WithinRel(op.offered.per_second - dropped, 1e-9));
    some_dropped = some_dropped || dropped > 0.0;
  });
  REQUIRE(some_dropped);
}

// ---- JSON: answered_per_s beside offered_per_s ----

TEST_CASE("every op in the JSON has answered_per_s, never above offered_per_s", "[series]") {
  // Two ops, 3000/s of `call` and 1000/s of `ping`: the limiter admits 2000/s of the 4000/s, the
  // same half of each op (see the op series test), so 1500/s and 500/s are answered, averaged
  // over the steps after the 500 ms warmup, all of them steady.
  const auto json = json_of(limited(2000, {{"call", 0.75}, {"ping", 0.25}}));
  CAPTURE(json);
  const auto ops_at = json.find("\"ops\"");
  REQUIRE(ops_at != std::string::npos);
  const std::vector<double> offered_rates{3000.0, 1000.0};  // call, ping: sorted by op name
  std::size_t from = ops_at;
  for (const double offered_rate : offered_rates) {
    CAPTURE(offered_rate);
    const auto offered = number_after(json, "\"offered_per_s\": ", from);
    // The new field sits next to offered_per_s in the same op entry.
    const auto answered = number_after(json, "\"answered_per_s\": ", from);
    REQUIRE(answered.value <= offered.value * (1.0 + 1e-9));
    REQUIRE_THAT(offered.value, WithinRel(offered_rate, 1e-6));
    REQUIRE_THAT(answered.value, WithinRel(offered_rate / 2.0, 1e-3));
    from = std::max(offered.end, answered.end);
  }
  // And no third op entry: the loop above saw every one.
  REQUIRE(json.find("\"offered_per_s\": ", from) == std::string::npos);
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
// A notification fan-out's and a URL frontier's `then` are not followed (types with no phase yet),
// so the nodes that only they feed show 0 offered load: before this change, `0%, green`, a verdict
// on a number that is missing, not zero. A blob store's `then` is followed since plan 2c Task 4
// (its steps are event notifications that fire for every request it serves, and its capacity
// counts megabytes), and a message queue's deliveries and a worker pool's `then` since Task 7:
// what they feed has its load and its verdict.

// Peak (21:00) of youtube: both clients at base x 1.6 (200 -> 320/s, 100 -> 160/s), 480/s in all,
// of which 2 % are uploads: 9.6/s. original_store's event publishes one message per upload to
// transcode_q, 9.6/s; the 400 transcoders (60 s each: 400 / 60 = 6.667 jobs/s) are OFFERED all of
// it, since the paper load does not queue: 9.6 / 6.667 = 1.44, 144 %, red (the real run saturates
// them and builds a backlog: README's "transcoders.workers: 200" lesson). Per job they publish one
// message to completion_q (9.6/s of 1M/s: 0.00096 %), which completion_handler (4 workers at 5 ms
// = 800/s) is offered: 9.6 / 800 = 1.2 %, green.
TEST_CASE("explain follows youtube from the blob store through the queues and pools: no mark",
          "[series]") {
  const auto text = explain_shipped("youtube");
  CAPTURE(text);
  REQUIRE_FALSE(text.contains("load not propagated"));
  const auto queue = check_line(text, "transcode_q");
  REQUIRE_FALSE(queue.empty());
  REQUIRE(queue.contains("peak offered 9.6/s"));
  REQUIRE(queue.contains("green"));
  const auto transcoders = check_line(text, "transcoders");
  REQUIRE(transcoders.contains("peak offered 9.6/s vs 6.667/s total capacity"));
  REQUIRE(transcoders.contains("(144%, red)"));
  const auto completion_q = check_line(text, "completion_q");
  REQUIRE(completion_q.contains("peak offered 9.6/s vs 1M/s total capacity  (0.00096%, green)"));
  const auto handler = check_line(text, "completion_handler");
  REQUIRE(handler.contains("peak offered 9.6/s vs 800/s total capacity  (1.2%, green)"));
}

// google-drive: api's upload_meta publishes to notify_q, 255.6/s at the peak (the line's own
// number), and both groups consuming it are offered all of it. notification_svc (8 workers at
// 1 ms = 8k/s): 255.6 / 8000 = 3.2 %, green; backup_workers (2 workers at 10 ms = 200/s):
// 255.6 / 200 = 128 %, red; its `then` writes each message's 500 KB (the step's size, the book's
// average file) to cold_storage: 255.6 x 0.5 MB = 127.8 MB/s of 200 MB/s, 63.9 %, green. None
// is marked by a queue or a pool any more. long_poll (a ws_gateway) is fed by the notification
// service's pushes too; whatever it still says about its own type, it blames no queue or pool.
TEST_CASE("explain follows google-drive's notify_q into its consumers and the backup store",
          "[series]") {
  const auto text = explain_shipped("google-drive");
  CAPTURE(text);
  const auto queue = check_line(text, "notify_q");
  REQUIRE(queue.contains("peak offered 255.6/s"));
  REQUIRE(queue.contains("green"));
  REQUIRE_FALSE(queue.contains("load not propagated"));
  const auto notifier = check_line(text, "notification_svc");
  REQUIRE(notifier.contains("peak offered 255.6/s vs 8k/s total capacity"));
  REQUIRE(notifier.contains(", green)"));
  REQUIRE_FALSE(notifier.contains("load not propagated"));
  const auto backup = check_line(text, "backup_workers");
  REQUIRE(backup.contains("peak offered 255.6/s vs 200/s total capacity"));
  REQUIRE(backup.contains(", red)"));
  REQUIRE_FALSE(backup.contains("load not propagated"));
  const auto cold = check_line(text, "cold_storage");
  REQUIRE(cold.contains("peak offered 127.8 MB/s (255.6/s) vs 200 MB/s total capacity"));
  REQUIRE(cold.contains("(63.9%, green)"));
  REQUIRE_FALSE(cold.contains("load not propagated"));
  const auto gateway = check_line(text, "long_poll");
  REQUIRE_FALSE(gateway.contains("worker_pool"));
  REQUIRE_FALSE(gateway.contains("message_queue"));
}

// news-feed: posts are 10 % of the peak, 2.162k/s (fanout_q's own line), and the 32 workers at 5 ms
// (32 / 0.005 s = 6.4k jobs/s) are offered all of it: 2162 / 6400 = 33.8 %, green. Per job they
// read graph_db once (2.162k/s of its 30k/s) and write 300 feed entries: 2.162k x 300 = 648.6k/s
// at feed_cache, on top of its 12.97k/s of reads, far over its 100k/s: red, complete (no lower
// bound, no mark). graph_db is a kv_cluster, whose nodes, replication and quorums are not modelled
// until plan 2c Task 10, so it gets its load and "not checked", marked by its own type and no
// queue or pool.
TEST_CASE("explain follows news-feed's fanout pool into graph_db and the feed caches", "[series]") {
  const auto text = explain_shipped("news-feed");
  CAPTURE(text);
  const auto fanout = check_line(text, "fanout");
  REQUIRE(fanout.contains("peak offered 2.162k/s vs 6.4k/s total capacity  (33.8%, green)"));
  REQUIRE_FALSE(fanout.contains("load not propagated"));
  const auto graph_db = check_line(text, "graph_db");
  REQUIRE(graph_db.contains(
      "peak offered 2.162k/s vs 30k/s total capacity  (not checked: kv_cluster nodes, "
      "replication and quorums: phase 3)"));
  REQUIRE_FALSE(graph_db.contains("worker_pool"));
  REQUIRE_FALSE(graph_db.contains("message_queue"));
  const auto feed_cache = check_line(text, "feed_cache");
  REQUIRE(feed_cache.contains("%, red)"));
  REQUIRE_FALSE(feed_cache.contains("load not propagated"));
  REQUIRE_FALSE(feed_cache.contains("≥"));
}

TEST_CASE("nodes the paper load does reach keep their verdict and are not marked", "[series]") {
  const auto text = explain_shipped("youtube");
  CAPTURE(text);
  // lb (a load balancer) and api (a service behind it) are reached only through forwarding
  // types: a percentage and a zone, no mark. (metadata_cache and metadata_db are reached too,
  // but also take the completion handler's writes: see the lower-bound cases below.)
  for (const auto* id : {"lb", "api"}) {
    CAPTURE(id);
    const auto line = check_line(text, id);
    REQUIRE_FALSE(line.empty());
    REQUIRE(line.contains("green"));
    REQUIRE_FALSE(line.contains("load not propagated"));
  }
  // The CDNs and the blob store fed straight from clients or caches are checked in MB/s (plan 2c
  // refinement 2), with a percentage and a zone: no "not checked" note, no propagation mark.
  for (const auto* id : {"cdn_us", "original_store"}) {
    CAPTURE(id);
    const auto line = check_line(text, id);
    REQUIRE(line.contains("MB/s"));
    REQUIRE(line.contains("green"));
    REQUIRE_FALSE(line.contains("not checked"));
    REQUIRE_FALSE(line.contains("load not propagated"));
  }
}

// ---- explain: a node fed partly through such a type gets a lower bound, not a verdict ----
//
// The mark above covered only nodes whose paper load was exactly 0. A node that also receives
// load the paper pass follows got an ordinary verdict on a partial number (review of 3e23477,
// I1): db below, fed 1/s by a client and 10k/s through a blob store's `then`, printed
// "(0.05%, green)". Every node with a blocking predecessor, direct or further up, is now marked;
// a reached one keeps its percentage as a lower bound ("≥"), with no zone below red. A red lower
// bound is a true verdict (the real load is at least that), so it keeps its "red". The blob store
// stopped being a blocking type in plan 2c Task 4 (its `then` is followed), so the two db cases
// below now show the complete number; the lower bound still marks what lies behind a worker pool
// or a message queue (the youtube cases further down).

TEST_CASE("a node fed partly through a blob store counts the store's notifications in full",
          "[series]") {
  const auto text = explain_of(partly_behind_blob_store(1.0));
  CAPTURE(text);
  const auto line = check_line(text, "db");
  // 1/s of reads plus the blob store's 10k/s of writes (one per served upload, factor 1) is
  // 10001/s against 2k/s: 500 %, red, a complete number: no lower bound and no mark (plan 2c
  // Task 4: a blob store's then steps are followed).
  REQUIRE(line.contains("(500%, red)"));
  REQUIRE_FALSE(line.contains("load not propagated"));
  REQUIRE_FALSE(line.contains("≥"));
}

TEST_CASE("a blob store's notifications add to the load a node gets from elsewhere", "[series]") {
  const auto text = explain_of(partly_behind_blob_store(3000.0));
  CAPTURE(text);
  // 3000/s of reads plus 10k/s of writes: 13000/s against 2k/s: 650 %.
  const auto line = check_line(text, "db");
  REQUIRE(line.contains("(650%, red)"));
  REQUIRE_FALSE(line.contains("load not propagated"));
}

// A red lower bound is a true verdict: 3000/s of reads alone against 2k/s is 150 %, so db is
// overloaded whatever else the fan-out adds. Its line keeps the percentage as a lower bound
// ("≥"), keeps "red", and names the type whose flow is missing. (Re-pinned on a type that stays
// blocking: the blob store this test used before is followed since plan 2c Task 4.)
TEST_CASE("a red lower bound stays red", "[series]") {
  const auto text = explain_of(partly_behind_fanout(3000.0));
  CAPTURE(text);
  REQUIRE(
      check_line(text, "db")
          .contains("(≥150%, red; load not propagated: notification_fanout is not modelled yet)"));
}

// The mark follows a blocking type's `then` steps, not its bare links: audit is linked from the
// fan-out but sent no step, so its 0 % is a real 0 and keeps its verdict, while db, which a step
// reaches, is marked. (Rebased from the blob store, which no longer blocks, onto a type that does,
// so the test still separates a step from a link.)
TEST_CASE("a node linked from a notification fan-out but sent no step keeps its verdict",
          "[series]") {
  const auto text = explain_of(partly_behind_fanout(1.0));
  CAPTURE(text);
  const auto line = check_line(text, "audit");
  REQUIRE(line.contains("(0%, green)"));
  REQUIRE_FALSE(line.contains("load not propagated"));
  REQUIRE(check_line(text, "db")
              .contains("(≥0.05%; load not propagated: notification_fanout is not modelled yet)"));
}

// metadata_db at the peak: api's metadata reads that miss the cache and its upload_meta writes
// were 50.28 units/s (9.6 writes/s x 5 units = 48, plus the reads' 2.28); the completion handler
// now writes once per message it is offered, 9.6/s, which is 9.6 x 5 = 48 more units: 98.28
// units/s of 9k (3 replicas x 3k/s): 98.28 / 9000 = 1.092 %, green (it was a lower bound,
// "≥0.559%"). metadata_cache gets the handler's 9.6 writes/s on top of api's 38.4 reads/s: 48/s
// of 100k/s, 0.048 %.
TEST_CASE("explain gives youtube's metadata_db and metadata_cache the completion handler's writes",
          "[series]") {
  const auto text = explain_shipped("youtube");
  CAPTURE(text);
  const auto db = check_line(text, "metadata_db");
  REQUIRE(db.contains("peak offered 98.28 units/s vs 9k/s total capacity  (1.09%, green)"));
  REQUIRE_FALSE(db.contains("≥"));
  REQUIRE_FALSE(db.contains("load not propagated"));
  const auto cache = check_line(text, "metadata_cache");
  REQUIRE(cache.contains("peak offered 48/s vs 100k/s total capacity  (0.048%, green)"));
  REQUIRE_FALSE(cache.contains("≥"));
  REQUIRE_FALSE(cache.contains("load not propagated"));
}

// chat-system's message_store is a kv_cluster whose capacity gets no verdict (kv_cluster nodes are
// not modelled, test_report): its line says why. It printed "(≥315%, red; ...)" before: a red
// verdict on the one-box capacity (review of c9d5f8c). Behind the message queue and the pools the
// paper load follows since Task 7, nothing it says blames a queue or a pool any more.
TEST_CASE("explain gives chat-system's kv_cluster no verdict and blames no queue or pool",
          "[series]") {
  const auto text = explain_shipped("chat-system");
  CAPTURE(text);
  const auto line = check_line(text, "message_store");
  REQUIRE(line.contains("(not checked: kv_cluster nodes, replication and quorums: phase 3)"));
  REQUIRE_FALSE(line.contains(", red"));  // ("peak offered" contains "red")
  REQUIRE_FALSE(line.contains("%"));
  REQUIRE_FALSE(line.contains("worker_pool"));
  REQUIRE_FALSE(line.contains("message_queue"));
}

// Web crawler (ch. 9): the URL frontier hands URLs to the downloader along its `then` (a type
// the model has no phase for yet), and the downloader, a worker pool, sends on along its own,
// which the paper load follows since plan 2c Task 7. dns is behind both, but only the frontier is
// a type whose onward flow is missing: the mark names it and no longer the worker pool. The
// frontier is reached by the seeds (50/s) and also fed back by url_seen's misses, which come
// through the downloader and the frontier itself: a lower bound as well.
TEST_CASE("explain names only the frontier on the way to a web-crawler node", "[series]") {
  const auto text = explain_shipped("web-crawler");
  CAPTURE(text);
  REQUIRE(check_line(text, "dns").contains("(load not propagated: frontier is not modelled yet)"));
  REQUIRE(check_line(text, "downloader")
              .contains("(load not propagated: frontier is not modelled yet)"));
  REQUIRE(check_line(text, "frontier")
              .contains("(≥1%; load not propagated: frontier is not modelled yet)"));
}

// Two types with no phase yet on the way to one node are both named, nearest first, each once, in
// one group as they stay unfollowed for the same reason (the web crawler no longer shows it: it has
// one such type, since pools are followed). A
// notification fan-out feeds a frontier that hands URLs to db, and nothing else reaches db.
TEST_CASE("explain names every blocking type on the way to a node, nearest first", "[series]") {
  Scenario s{.name = "two-blockers", .version = 1, .duration = 1s, .step = 100ms};
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{10}}, .op_shares = {{"fire", 1.0}}}},
      NodeSpec{.id = "fan",
               .type = NodeType::notification_fanout,
               .then = {simsys::model::Step{.to = "seeds", .op = "url"}}},
      NodeSpec{.id = "seeds",
               .type = NodeType::frontier,
               .then = {simsys::model::Step{.to = "db", .op = "write"}}},
      NodeSpec{.id = "db", .type = NodeType::service, .capacity = Rate{2000}}};
  s.links = {LinkSpec{.from = "u", .to = "fan"}, LinkSpec{.from = "fan", .to = "seeds"},
             LinkSpec{.from = "seeds", .to = "db"}};
  const auto text = explain_of(s);
  CAPTURE(text);
  REQUIRE(check_line(text, "db")
              .contains("(load not propagated: frontier and notification_fanout are not modelled "
                        "yet)"));
}

// ---- A link's bandwidth is a pipe: its rows in the series and the JSON (plan 2c Task 5) ----
//
// A link with a bandwidth is a queue of megabytes and is plotted like a node: one CSV row per
// step with the link's name, `from->to`, in the `node` column, the same ten columns, in
// MEGABYTES (a rate of MB/s; engine::StepRecord::links, one LinkSample per bandwidth link). So
// the conservation law holds on those rows too, and a student checks the pipe from the file.
// The JSON gets `links`: one object per bandwidth link (an empty array without any).
//
// Picture: client `a` sends `rate` puts/s of 1 MB to service `b` over a 1 Gbit/s = 125 MB/s link.

namespace {
// a(`rate`/s of 1 MB `put`s) -> b over a 250 us link of 1 Gbit/s (none when `bandwidth` is
// empty). 3 s at 100 ms steps, the first 500 ms warmup.
Scenario piped(double rate, std::optional<simsys::ByteRate> bandwidth = simsys::ByteRate{125e6}) {
  Scenario s{.name = "piped", .version = 1, .duration = 3s, .step = 100ms, .warmup = 500ms};
  s.nodes = {NodeSpec{.id = "a",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{rate}},
                                           .op_shares = {{"put", 1.0}},
                                           .sizes = {{"put", simsys::Bytes{1e6}}}}},
             NodeSpec{.id = "b", .type = NodeType::service, .capacity = Rate{1e6}, .latency = 1ms}};
  s.links = {LinkSpec{.from = "a", .to = "b", .latency = 250us, .bandwidth = bandwidth}};
  return s;
}

// The text of the JSON object that starts at the first `"name": "<name>"` (up to its closing
// brace), or empty: the keys of one link or node, without those of the next.
std::string json_object_named(const std::string& json, std::string_view name) {
  const auto at = json.find("\"name\": \"" + std::string{name} + "\"");
  if (at == std::string::npos) return {};
  return json.substr(at, json.find('}', at) - at);
}

// The number after the first key that starts with `key_prefix` (a quoted key without its closing
// quote, `"latency`), whatever its suffix (`latency` or `latency_s`).
double number_of_key(const std::string& object, std::string_view key_prefix) {
  const auto at = object.find(key_prefix);
  INFO("key: " << key_prefix << " in " << object);
  REQUIRE(at != std::string::npos);
  const auto colon = object.find(": ", at);
  REQUIRE(colon != std::string::npos);
  return number_after(object, ": ", colon).value;
}
}  // namespace

TEST_CASE(
    "a link row of the node series carries the LinkSample it was fed, in the same ten columns",
    "[series]") {
  const auto s = piped(100);
  const auto g = graph_of(s);
  std::ostringstream out;
  simsys::report::SeriesWriter writer{out, g};
  REQUIRE(header_of(out.str()) == node_header);  // no column is added for links
  // A hand-made record, every column a different, exactly printable number: the nodes' samples
  // stay zero (a client and b), the pipe's are made up.
  simsys::engine::StepRecord record{
      .t = 100ms, .nodes = std::vector<simsys::engine::NodeSample>(2), .responses = {}};
  simsys::engine::LinkSample link{};
  link.demand_per_s = 100.5;
  link.drain_per_s = 100.25;
  link.dropped_per_s = 5;
  link.backlog = 34.25;
  link.rho = 0.8;
  link.latency = {.mean_s = 0.0125, .variance_s2 = 0.0};
  link.zone = simsys::fluid::Zone::yellow;
  record.links = {link};
  writer.observe(record);
  const auto rows = data_rows(out.str());
  REQUIRE(rows.size() == 3);  // the two nodes' rows and the link's, in whatever order
  const auto found = std::ranges::find_if(rows, [](const auto& row) { return row[1] == "a->b"; });
  REQUIRE(found != rows.end());
  const auto& row = *found;
  REQUIRE(row.size() == 10);
  REQUIRE(row[0] == "0.1");
  REQUIRE(row[2] == "100.5");   // demand_per_s, MB/s
  REQUIRE(row[3] == "100.25");  // drain_per_s
  REQUIRE(row[4] == "0.8");     // rho
  REQUIRE(row[5] == "34.25");   // backlog, MB
  REQUIRE(row[6] == "5");       // drops_per_s
  REQUIRE(row[7] == "0");       // rejected_per_s: a pipe refuses nothing by policy
  REQUIRE(row[8] == "12.5");    // latency_ms: 0.0125 s
  REQUIRE(row[9] == "yellow");
}

// 1000 puts/s of 1 MB offer the pipe 1000 MB/s against 125 MB/s: the backlog grows by
// 875 x 0.1 s = 87.5 MB per step to 962.5 MB after 11 steps, the 12th overflows the 1000 MB room
// (1050 - 1000 = 50 MB, 500 MB/s over the step) and from then on the pipe drops 875 MB/s: all
// three terms of the identity. 30 steps of 1000 MB/s x 0.1 s are 3000 MB offered.
TEST_CASE("the conservation identity holds on the link rows read back from the series file",
          "[series]") {
  const auto s = piped(1000);
  const auto g = graph_of(s);
  std::ostringstream out;
  simsys::report::SeriesWriter writer{out, g};
  run_into(s, g, [&](const auto& r) { writer.observe(r); });
  const auto rows = data_rows(out.str());
  CAPTURE(out.str());
  REQUIRE(rows.size() == 30 * 3);  // 30 steps x (the client, b and the link)
  REQUIRE(std::ranges::all_of(rows, [](const auto& row) { return row.size() == 10; }));
  constexpr double dt = 0.1;
  double demand = 0.0;
  double drain = 0.0;
  double drops = 0.0;
  double backlog = 0.0;
  int link_rows = 0;
  for (const auto& row : rows) {
    if (row[1] != "a->b") continue;
    ++link_rows;
    demand += number(row[2]) * dt;
    drain += number(row[3]) * dt;
    drops += number(row[6]) * dt;
    backlog = number(row[5]);  // the last row's: what is left waiting at the end
  }
  REQUIRE(link_rows == 30);
  REQUIRE_THAT(demand, WithinRel(3000.0, 1e-5));
  REQUIRE(drops > 0.0);                            // not vacuous: the pipe really dropped ...
  REQUIRE(backlog > 0.0);                          // ... and really holds megabytes
  REQUIRE_THAT(backlog, WithinRel(1000.0, 1e-5));  // the room is full at the end
  REQUIRE_THAT(demand, WithinRel(drain + drops + backlog, 1e-5));
  // b is sent what the pipe drained: 125 MB/s of 1 MB puts, 125 requests/s, in every row it has.
  for (const auto& row : rows) {
    if (row[1] == "b") REQUIRE_THAT(number(row[2]), WithinRel(125.0, 1e-5));
  }
}

// The JSON `links`: one object per bandwidth link. Constant load, so every step of the run has
// the same numbers and any way of summarising them (mean, peak, last) reads the same:
// 100 MB/s offered and drained, nothing dropped or waiting, rho 0.8 (yellow), a sojourn of 24 ms
// (Wq 16 ms + 8 ms, see test_theory). The latency's key is `latency_s` if it follows the other
// `_s` keys, `latency` as the plan words it: either is read here; its value is in seconds.
TEST_CASE("the JSON has links with the pipe's demand, drain, drops, backlog, rho, latency and zone",
          "[series]") {
  const auto json = json_of(piped(100));
  CAPTURE(json);
  REQUIRE(json.contains("\"links\": [\n"));
  const auto link = json_object_named(json, "a->b");
  REQUIRE_FALSE(link.empty());
  REQUIRE_THAT(number_of_key(link, "\"demand_per_s\""), WithinRel(100.0, 1e-9));
  REQUIRE_THAT(number_of_key(link, "\"drain_per_s\""), WithinRel(100.0, 1e-9));
  REQUIRE(number_of_key(link, "\"dropped_per_s\"") == 0.0);
  REQUIRE(number_of_key(link, "\"backlog\"") == 0.0);
  REQUIRE_THAT(number_of_key(link, "\"rho\""), WithinRel(0.8, 1e-9));
  REQUIRE_THAT(number_of_key(link, "\"latency"), WithinAbs(0.024, 1e-9));
  REQUIRE(link.contains("\"zone\": \"yellow\""));
}

TEST_CASE("the JSON has an empty links array when no link has a bandwidth", "[series]") {
  const auto json = json_of(piped(100, std::nullopt));
  CAPTURE(json);
  REQUIRE(json.contains("\"links\": []"));
  REQUIRE_FALSE(json.contains("a->b"));
}

// What a link without a bandwidth did before Task 5 must not change by a byte. `tutorial/chain`
// (1.5k/s through two services and a database, no bandwidth, no size) is run for its whole 2
// minutes and its node series and op series hashed (64-bit FNV-1a over the bytes of each file);
// the hashes were taken from the output at 0097309, the commit before the pipe, with the same
// seed. A pipe's code path that leaks into a link without one changes a number, a row or a
// delay-line slot, and with it the hash. It guards Task 5 (and every later change to the pipe)
// against touching plain links; a later task that changes `chain`'s numbers ON PURPOSE (a new
// default, a model change that reaches services or databases) re-pins both hashes in the same
// commit and says why.
namespace {
std::uint64_t fnv1a(const std::string& text) {
  std::uint64_t hash = 0xcbf29ce484222325ULL;  // the 64-bit FNV offset basis
  for (const char c : text) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 0x100000001b3ULL;  // the 64-bit FNV prime
  }
  return hash;
}
}  // namespace

TEST_CASE("the tutorial chain's series are byte for byte what they were before link bandwidths",
          "[series]") {
  const auto loaded =
      simsys::config::load(std::string{SIMSYS_SCENARIOS_DIR} + "/tutorial/chain/scenario.yaml");
  INFO((loaded ? "" : loaded.error().message));
  REQUIRE(loaded.has_value());
  const auto g = graph_of(*loaded);
  std::ostringstream nodes_out;
  std::ostringstream ops_out;
  simsys::report::SeriesWriter nodes{nodes_out, g};
  simsys::report::OpSeriesWriter ops{ops_out, g};
  run_into(*loaded, g, [&](const auto& r) {
    nodes.observe(r);
    ops.observe(r);
  });
  REQUIRE(nodes_out.str().find("->") == std::string::npos);  // no link rows
  REQUIRE(data_rows(nodes_out.str()).size() == 1200 * 3);    // 2 min of 100 ms steps x 3 nodes
  REQUIRE(fnv1a(nodes_out.str()) == 0x3d3a6ed1d31da525ULL);
  REQUIRE(fnv1a(ops_out.str()) == 0x9dfc7de84e1afe59ULL);
}

// ---- explain on youtube: a pool's capacity is workers / cost (plan 2c Task 6) ----
//
// The line of the nodes block that starts, after its indent, with `id`.
namespace {
std::string node_line_of(const std::string& text, const std::string& id) {
  const auto at = text.find("\n  " + id + " ", text.find("nodes:"));
  if (at == std::string::npos) return {};
  return text.substr(at + 1, text.find('\n', at + 1) - at - 1);
}
}  // namespace

// transcoders: 400 workers at 60 s a video = 400 / 60 s = 6.667 videos/s (it was the worker_pool
// type's default 1k/s: 2 ms at c = 2). completion_handler: 4 workers at 5 ms = 4 / 0.005 s = 800/s.
// Only the capacity figure moves in Task 6 (README and schema quoted `transcoders  peak offered
// 0/s vs 6.667/s`); Task 7 follows the load through the queues and fills in the offered one.
TEST_CASE("explain checks youtube's transcoders against workers over cost: 6.667 a second",
          "[series]") {
  const auto text = explain_shipped("youtube");
  CAPTURE(text);
  const auto transcoders = check_line(text, "transcoders");
  REQUIRE(transcoders.contains("vs 6.667/s total capacity"));
  REQUIRE_FALSE(transcoders.contains("vs 1k/s"));
  const auto handler = check_line(text, "completion_handler");
  REQUIRE(handler.contains("vs 800/s total capacity"));
}

// What the nodes block says about youtube's queue and pool: 400 transcoders of 60 s a job
// (`cost=1m`), 32 partitions on transcode_q, and no retention stated, so Kafka's default.
TEST_CASE("explain describes youtube's transcode queue and transcoders", "[series]") {
  const auto text = explain_shipped("youtube");
  CAPTURE(text);
  REQUIRE(node_line_of(text, "transcoders")
              .contains("workers=400 cost=1m (capacity 6.667/s: workers / cost)"));
  const auto queue = node_line_of(text, "transcode_q");
  REQUIRE(queue.contains("retention=7d (default: Kafka's 168 h)"));
  REQUIRE(queue.contains("partitions=32 (not modelled)"));
}
