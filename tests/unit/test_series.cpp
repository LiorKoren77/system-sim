// The series files and the explain marks of spec §18 entry 13: what a student can check from the
// output alone.
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
#include <filesystem>
#include <fstream>
#include <limits>
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
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions when a
// std header is included after a module that included it in its global module fragment.
import simsys.config;
import simsys.core;
import simsys.engine;
import simsys.fluid; // Zone: a hand-made LinkSample's
import simsys.graph;
import simsys.model;
import simsys.report;
import simsys.test_fixtures;
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
using simsys::model::Step;
using simsys::model::Workload;
using simsys::test_support::check_line;
using simsys::test_support::node_line;
// Shared with test_series / test_report (tests/support/fixtures.cppm, simsys.test_fixtures).
using simsys::test_support::explain_of;
using simsys::test_support::group_of;
using simsys::test_support::piped;
using simsys::test_support::published;

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
// over a client route, which the paper load follows; `uploader` sends 10k/s of `put` to `blobs`, a
// blob store whose `then` writes each upload's metadata to db (an event notification: the paper
// load follows it; before). A blob store notifies only for its writes, so `blobs` lists `put` in
// its `write_ops` (S3's PUT, ObjectCreated): with the default `[write]` a put would be a read and
// notify nobody. `blobs` also has a link to `audit` but no step along it: nothing flows there.
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

// The same two clients and db, but the 10k/s of `post` go to `seeds`, a URL frontier (ch. 9) whose
// `then` sends each post to db as a `write`: followed by the paper load since phase 3 Task 18, so
// db's load is the reader's part plus the frontier's 10k/s. `seeds` also has a link to `audit` but
// no step along it: nothing flows there. (A notification fan-out stood here until phase 3 Task 17
// gave it a model, then the frontier as the last blocking type, until Task 18 gave it one.)
Scenario partly_behind_frontier(double read_rate) {
  Scenario s{.name = "partial-frontier", .version = 1, .duration = 1s, .step = 100ms};
  s.nodes = {
      NodeSpec{
          .id = "reader",
          .type = NodeType::client,
          .workload = Workload{.curve = {.base = Rate{read_rate}}, .op_shares = {{"read", 1.0}}}},
      NodeSpec{.id = "poster",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{10'000}}, .op_shares = {{"post", 1.0}}}},
      NodeSpec{.id = "seeds",
               .type = NodeType::frontier,
               .then = {simsys::model::Step{.to = "db", .op = "write"}}},
      NodeSpec{.id = "db", .type = NodeType::service, .capacity = Rate{2000}},
      NodeSpec{.id = "audit", .type = NodeType::service, .capacity = Rate{2000}}};
  s.links = {LinkSpec{.from = "reader", .to = "db"}, LinkSpec{.from = "poster", .to = "seeds"},
             LinkSpec{.from = "seeds", .to = "db"}, LinkSpec{.from = "seeds", .to = "audit"}};
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

// The text of one CSV line split at commas: `line` is a row, with no newline. A trailing empty
// cell is a field ("a,b," has three): the node series' last column, connections, is empty on
// every row but a ws_gateway's, the queue_lag_s before it on every row but a message queue's, its
// dead_lettered_per_s empty on every row but a worker pool's,
// and std::getline(cells, field, ',') would drop a trailing one silently, so that a row with and
// without the column would count the same.
std::vector<std::string> split_commas(const std::string& line) {
  std::vector<std::string> fields;
  std::size_t begin = 0;
  while (true) {
    const auto comma = line.find(',', begin);
    if (comma == std::string::npos) break;
    fields.push_back(line.substr(begin, comma - begin));
    begin = comma + 1;
  }
  fields.push_back(line.substr(begin));
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
    "t_s,node,demand_per_s,drain_per_s,rho,backlog,drops_per_s,dead_lettered_per_s,rejected_per_s,"
    "deduplicated_per_s,latency_ms,zone,queue_lag_s,connections";
constexpr std::string_view op_header =
    "t_s,client,op,offered_per_s,attempts_per_s,answered_per_s,timed_out_per_s,mean_ms";

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
  REQUIRE(server.size() == 14);
  REQUIRE(server[0] == "0.1");
  REQUIRE(server[1] == "s");
  REQUIRE(server[2] == "1234.5");   // demand_per_s
  REQUIRE(server[3] == "1000.25");  // drain_per_s
  REQUIRE(server[4] == "0.8");      // rho
  REQUIRE(server[5] == "34.25");    // backlog
  REQUIRE(server[6] == "200");      // drops_per_s
  REQUIRE(server[7] == "");         // dead_lettered_per_s: a service has none, the cell is empty
  REQUIRE(server[8] == "7");        // rejected_per_s
  REQUIRE(server[9] == "");         // deduplicated_per_s: a service is no fanout, the cell is empty
  REQUIRE(server[10] == "12.5");    // latency_ms: 0.0125 s
  REQUIRE(server[11] == "green");   // the NodeSample's default zone
  REQUIRE(server[12] == "");        // queue_lag_s: a service has no lag, the cell is empty
  REQUIRE(server[13] == "");        // connections: a service holds none, the cell is empty
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
  // t, node, demand, drain, rho, backlog, drops, dead_lettered, rejected, deduplicated, latency,
  // zone, queue_lag_s, connections: a client's engine sample is all zeros (engine.cppm: "a source
  // has no queue"), its zone is `none` and its dead_lettered and deduplicated cells and its last
  // two (a pool's count, a fanout's count, a message queue's lag, a gateway's level) are empty.
  // C++23 std::string::contains: a substring test in one call, replacing `find(...) !=
  // std::string::npos`.
  REQUIRE(text.contains("\n0,c,0,0,0,0,0,,0,,0,none,,\n"));
}

TEST_CASE("every row of a run has fourteen fields, one row per node per step", "[series]") {
  const auto s = mm1(500);  // 500 ms at 100 ms steps: 5 steps
  const auto g = graph_of(s);
  std::ostringstream out;
  simsys::report::SeriesWriter writer{out, g};
  run_into(s, g, [&](const auto& r) { writer.observe(r); });
  const auto rows = data_rows(out.str());
  REQUIRE(rows.size() == 5 * 2);  // 5 steps x 2 nodes (the client and the server)
  // C++20 std::ranges::all_of: the algorithm takes the whole range and a predicate, replacing
  // the begin()/end() pair of std::all_of.
  REQUIRE(std::ranges::all_of(rows, [](const auto& row) { return row.size() == 14; }));
}

// ---- node series: queue_lag_s ----
//
// A message queue's lag (engine::NodeSample::queue_lag_s: its worst consumer group's wait,
// max(backlog / drain, k x Wq)) is the node series' column `queue_lag_s`, the last but one (a
// ws_gateway's `connections` came after it in phase 3 Task 14), in SECONDS (not
// ms: the lag of a queue is minutes, hours: the column is for plotting it against the backlog). It
// is empty, not 0, on every row of a node that is not a message queue (a 0 lag would be a claim),
// clients, services and bandwidth links' pipes included: the header is one for the whole file.
//
// Picture: u publishes 1500/s to the queue q, whose one group w has 12 workers at 10 ms (exactly
// 1200 jobs/s): the backlog grows by 300/s = 30 a 100 ms step, so the record that starts at
// t = 0.9 s, the tenth step, leaves 300 messages waiting: 300 / 1200 = 0.25 s, above the 0.08 s
// Erlang floor of a saturated 12-worker pool (Wq(12, rho clamped to 0.99, 10 ms)).

namespace {
Scenario queue_and_group() {
  Scenario s{.name = "lag", .version = 1, .duration = 1s, .step = 100ms};
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload =
                          Workload{.curve = {.base = Rate{1500}}, .op_shares = {{"publish", 1.0}}}},
             NodeSpec{.id = "q", .type = NodeType::message_queue},
             NodeSpec{.id = "w",
                      .type = NodeType::worker_pool,
                      .workers = 12,
                      .job_cost = 10ms,
                      .consumes = "q"}};
  s.links = {LinkSpec{.from = "u", .to = "q", .latency = 0s},
             LinkSpec{.from = "q", .to = "w", .latency = 0s}};
  return s;
}
}  // namespace

TEST_CASE("the node series header has queue_lag_s after zone and connections last", "[series]") {
  const auto g = graph_of(queue_and_group());
  std::ostringstream out;
  simsys::report::SeriesWriter writer{out, g};
  const auto header = split_commas(header_of(out.str()));
  REQUIRE(header.size() == 14);
  REQUIRE(header[12] == "queue_lag_s");
  REQUIRE(header.back() == "connections");  // phase 3 Task 14: a ws_gateway's level
  REQUIRE(header[11] == "zone");            // the eleven columns before them keep their places
}

TEST_CASE("a message queue's row ends with the lag of its sample and every other row ends empty",
          "[series]") {
  const auto s = queue_and_group();
  const auto g = graph_of(s);
  std::ostringstream out;
  simsys::report::SeriesWriter writer{out, g};
  std::vector<simsys::engine::NodeSample> nodes(3);  // u 0, q 1, w 2
  nodes[1].queue_lag_s = 1.5;                        // a lag of 1.5 s, exactly printable
  writer.observe(simsys::engine::StepRecord{.t = 100ms, .nodes = nodes, .responses = {}});
  const auto rows = data_rows(out.str());
  REQUIRE(rows.size() == 3);
  REQUIRE(rows[1][1] == "q");
  REQUIRE(rows[1].size() == 14);
  REQUIRE(rows[1][12] == "1.5");
  for (const auto i : {std::size_t{0}, std::size_t{2}}) {
    CAPTURE(rows[i][1]);
    REQUIRE(rows[i].size() == 14);
    REQUIRE(rows[i][12] == "");  // the client's and the pool's: no lag, not a lag of 0
  }
}

TEST_CASE("a run's queue rows hold the lag in seconds and the client and pool rows stay empty",
          "[series]") {
  const auto s = queue_and_group();
  const auto g = graph_of(s);
  std::ostringstream out;
  simsys::report::SeriesWriter writer{out, g};
  run_into(s, g, [&](const auto& r) { writer.observe(r); });
  const auto rows = data_rows(out.str());
  REQUIRE(rows.size() == 10 * 3);  // 10 steps x (u, q, w)
  for (const auto& row : rows) {
    CAPTURE(row[0], row[1]);
    REQUIRE(row.size() == 14);
    if (row[1] == "q") {
      REQUIRE_FALSE(row[12].empty());
      REQUIRE(number(row[12]) >= 0.0);
    } else {
      REQUIRE(row[12].empty());
    }
  }
  // C++20 std::ranges::find_if: the range itself and a predicate, replacing std::find_if's
  // begin/end pair.
  const auto last_q =
      std::ranges::find_if(rows, [](const auto& row) { return row[0] == "0.9" && row[1] == "q"; });
  REQUIRE(last_q != rows.end());
  REQUIRE_THAT(number((*last_q)[12]), WithinAbs(0.25, 1e-6));  // 300 waiting / 1200 a second
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
    rejected += number(row[8]) * dt;
    backlog = number(row[5]);
  }
  REQUIRE_THAT(demand, WithinRel(4000.0, 1e-5));  // 10 steps x 0.1 s x 4000/s
  REQUIRE(rejected > 0.0);                        // the limiter did refuse requests
  REQUIRE(rejected <= drain);                     // ... and they are part of what it drained
  REQUIRE_THAT(demand, WithinRel(drain + drops + backlog, 1e-5));
}

// ---- op series: offered_per_s, attempts_per_s, answered_per_s, timed_out_per_s, mean_ms ----

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
  REQUIRE(rows[0].size() == 8);
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

TEST_CASE(
    "an op series row holds the sample's offered, answered and timed out rates and its mean in ms",
    "[series]") {
  const auto s = mm1(500);
  const auto g = graph_of(s);
  std::ostringstream out;
  simsys::report::OpSeriesWriter writer{out, g};
  simsys::engine::ResponseSample sample{.client = 0,
                                        .op = "op",
                                        .offered = Rate{250.5},
                                        .answered = Rate{125.25},
                                        .timed_out = Rate{4.5},
                                        .response = {.mean_s = 0.0125}};
  // ASSUMED: `ResponseSample::attempts`, a Rate like `offered` (assigned, not in the initializer,
  // so the position of the member does not matter).
  sample.attempts = Rate{300.75};
  writer.observe(simsys::engine::StepRecord{
      .t = 200ms, .nodes = std::vector<simsys::engine::NodeSample>(2), .responses = {sample}});
  const auto rows = data_rows(out.str());
  REQUIRE(rows.size() == 1);
  // t_s, client, op, offered_per_s, attempts_per_s (what the client sent, retries included),
  // answered_per_s, timed_out_per_s (what the client gave up on, delivered after its timeout),
  // mean_ms: 0.0125 s is 12.5 ms.
  REQUIRE(rows[0] ==
          std::vector<std::string>{"0.2", "c", "op", "250.5", "300.75", "125.25", "4.5", "12.5"});
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
  REQUIRE(rows[0].size() == 8);  // checked first: an index past the end is no test failure
  REQUIRE(rows[0][7] == "inf");
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
    const double answered = number(row[5]);
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
    REQUIRE_THAT(number(row[5]), WithinRel(number(row[3]), 1e-9));
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
  REQUIRE_THAT(number(last[5]), WithinRel(2000.0, 1e-6));
}

// ---- op series: attempts_per_s ----
//
// The column right after offered_per_s: what the client SENT, its curve plus the retries of the
// requests it lost or timed out (phase 3 Task 10). u sends 1000/s with `retries: 2` after 100 ms
// into a sink that fails half of what it serves (room to spare), so at 100 ms steps its attempts
// are 1000, 1500, then 1750 a second (1 + 0.5 + 0.25 per request) while it offers 1000.

namespace {
Scenario retrying_into_a_lossy_sink() {
  Scenario s{.name = "retrying", .version = 1, .duration = 1s, .step = 100ms};
  NodeSpec api{
      .id = "api", .type = NodeType::external_sink, .capacity = Rate{100'000}, .latency = 1ms};
  api.failure_rate = 0.5;
  Workload workload{.curve = {.base = Rate{1000}}, .op_shares = {{"call", 1.0}}};
  // ASSUMED names: `Workload::retries` (a count) and `Workload::retry_after` (a duration).
  workload.retries = static_cast<decltype(workload.retries)>(2);
  workload.retry_after = 100ms;
  s.nodes = {NodeSpec{.id = "u", .type = NodeType::client, .workload = std::move(workload)},
             std::move(api)};
  s.links = {LinkSpec{.from = "u", .to = "api", .latency = 0s}};
  return s;
}
}  // namespace

TEST_CASE("the op series has attempts_per_s right after offered_per_s", "[series]") {
  const auto g = graph_of(mm1(500));
  std::ostringstream out;
  simsys::report::OpSeriesWriter writer{out, g};
  const auto header = split_commas(header_of(out.str()));
  REQUIRE(header.size() == 8);
  REQUIRE(header[3] == "offered_per_s");
  REQUIRE(header[4] == "attempts_per_s");
  REQUIRE(header[5] == "answered_per_s");
}

TEST_CASE("a retrying client's attempts column climbs 1000, 1500, 1750 while offered stays 1000",
          "[series]") {
  const auto s = retrying_into_a_lossy_sink();
  const auto g = graph_of(s);
  std::ostringstream out;
  simsys::report::OpSeriesWriter writer{out, g};
  run_into(s, g, [&](const auto& r) { writer.observe(r); });
  const auto rows = data_rows(out.str());
  CAPTURE(out.str());
  REQUIRE(rows.size() == 10);  // 10 steps of 100 ms
  for (const auto& row : rows) REQUIRE_THAT(number(row[3]), WithinRel(1000.0, 1e-9));
  REQUIRE_THAT(number(rows[0][4]), WithinRel(1000.0, 1e-9));
  REQUIRE_THAT(number(rows[1][4]), WithinRel(1500.0, 1e-9));
  for (std::size_t i = 2; i < rows.size(); ++i) {
    CAPTURE(i);
    REQUIRE_THAT(number(rows[i][4]), WithinRel(1750.0, 1e-6));  // the file keeps 6 digits
  }
}

// explain prints the wait a retry really waits. The engine moves in whole steps and rounds the
// backoff UP (a retry never fires before the stated wait), so at 100 ms steps a retry_after of
// 150 ms waits 2 steps, 200 ms, and explain says so after the stated wait; a wait that is a whole
// number of steps prints as before.
TEST_CASE(
    "explain prints a retry_after that is not a whole number of steps with the steps it waits",
    "[series]") {
  auto s = retrying_into_a_lossy_sink();
  s.nodes[0].workload->retry_after = 150ms;
  const auto text = explain_of(s);
  CAPTURE(text);
  // C++20 std::string::ends_with: a suffix test in one call, replacing a compare() on the tail
  // (the line's columns before the retries are padded and pinned elsewhere).
  REQUIRE(node_line(text, "u").ends_with("retries=2 after 150ms (2 steps)"));
}

TEST_CASE("explain prints a retry_after shorter than a step with the one step it waits",
          "[series]") {
  auto s = retrying_into_a_lossy_sink();
  s.nodes[0].workload->retry_after = 50ms;
  const auto text = explain_of(s);
  CAPTURE(text);
  REQUIRE(node_line(text, "u").ends_with("retries=2 after 50ms (1 step)"));
}

TEST_CASE("explain prints a retry_after of whole steps as it is written", "[series]") {
  auto s = retrying_into_a_lossy_sink();
  s.nodes[0].workload->retry_after = 200ms;
  const auto text = explain_of(s);
  CAPTURE(text);
  REQUIRE(node_line(text, "u").ends_with("retries=2 after 200ms"));
}

// Without retries the client sends its curve and nothing else, whatever its path sheds: the
// 1500/s into a 1000/s server with a room of 100 sheds a third, and attempts_per_s is still
// offered_per_s on every row.
TEST_CASE("a client without retries has attempts equal to offered on every row, even when it sheds",
          "[series]") {
  const auto s = mm1(1500, 100);
  const auto g = graph_of(s);
  std::ostringstream out;
  simsys::report::OpSeriesWriter writer{out, g};
  run_into(s, g, [&](const auto& r) { writer.observe(r); });
  const auto rows = data_rows(out.str());
  REQUIRE(rows.size() == 5);
  for (const auto& row : rows) {
    CAPTURE(row[0]);
    REQUIRE(number(row[4]) == number(row[3]));
  }
}

// ---- answered counts what was lost, not what was served in the same step ----
//
// A request that waits in a growing queue is answered later, not lost. The per-step delivered share
// at a queue is therefore 1 − (dropped + rejected) / inflow (engine.cppm, account_rejections): only
// a drop or a refusal makes a request unanswered, and a backlog that grows and drains again is
// neutral. Before it was served / inflow capped at 1, which counted every request still waiting at
// the end of its arrival step as unanswered and never credited it later: the review's probe, this
// spike over a 2-minute run, reported 583/s answered of 667/s offered with nothing dropped.

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
// The paper load (explain.cpp: paper_pass / forward_steps) forwards load from a client's routes,
// from a service or worker over its `ops` steps, through load balancers and rate limiters over
// their links (graph::forwards_by_link) and from caches and CDNs on misses (hits_or_misses). A
// blob store's `then` is followed (its steps are event notifications that fire for every request
// it serves, and its capacity counts megabytes), and so are a message queue's deliveries and the
// `then` of a worker pool, a notification fan-out (phase 3 Task 17) and a URL frontier (Task 18):
// what they feed has its load and its verdict. No type is left unfollowed since then; until it was,
// the nodes only such a type fed showed 0 offered load, `0%, green` before the marks, a verdict on
// a number that is missing, not zero.

// Peak (21:00) of youtube: both clients at base x 1.6 (200 -> 320/s, 100 -> 160/s), 480/s in all,
// of which 2 % are uploads: 9.6/s. original_store's event publishes one message per upload to
// transcode_q, 9.6/s; the 600 transcoders (60 s each: 600 / 60 = 10 jobs/s) are OFFERED all of it,
// since the paper load does not queue: 9.6 / 10 = 0.96, 96 %, red (red is a utilization verdict
// from 90 %; nothing backs up, and transcode_q's lag stays at the Erlang wait, 0.57 s; at the 400
// workers it shipped with, 144 %, the run built a backlog). Per job they publish one message to
// completion_q (9.6/s of 1M/s: 0.00096 %), which completion_handler (4 workers at 5 ms = 800/s) is
// offered: 9.6 / 800 = 1.2 %, green.
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
  REQUIRE(transcoders.contains("peak offered 9.6/s vs 10/s total capacity"));
  REQUIRE(transcoders.contains("(96%, red)"));
  const auto completion_q = check_line(text, "completion_q");
  REQUIRE(completion_q.contains("peak offered 9.6/s vs 1M/s total capacity  (0.00096%, green)"));
  const auto handler = check_line(text, "completion_handler");
  REQUIRE(handler.contains("peak offered 9.6/s vs 800/s total capacity  (1.2%, green)"));
}

// google-drive: api's upload_meta publishes to notify_q, 255.6/s at the peak (the line's own
// number), and both groups consuming it are offered all of it. notification_svc (8 workers at
// 1 ms = 8k/s): 255.6 / 8000 = 3.2 %, green; backup_workers (3 workers at 10 ms = 300/s, sized
// in phase 3 Task 16; the 2 it first shipped with were 200/s, 128 %, red): 255.6 / 300 = 85.2 %,
// yellow; its `then` writes each message's 500 KB (the step's size, the book's
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
  REQUIRE(backup.contains("peak offered 255.6/s vs 300/s total capacity"));
  REQUIRE(backup.contains("(85.2%, yellow)"));
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
// read graph_db once and write 300 feed entries: 2.162k x 300 = 648.6k/s at feed_cache, on top of
// its 12.97k/s of reads: 661.5k/s, a complete number (no lower bound, no mark), against the 10
// replicas x 100k/s it is sized with (one replica, as first shipped, was 662 %, red). graph_db is a
// kv_cluster of 6 nodes of 30k/s keeping N = 3 copies: each read goes to 3 replicas, 3 x 2.162k =
// 6.486k replica-requests/s over 6 x 30k = 180k/s, 3.6 % on average; behind the ring (default 150
// virtual nodes, k = 1 + z_6/sqrt(3 x 150) = 1.0604, z_6 = 1.2816 the expected maximum of 6
// standard normals) the hottest node is sent 1.0604 x 1.081k = 1.146k/s of its 30k/s: 3.821 %,
// printed as 3.82 %, green. The "not checked" note of the phase before the model is gone. The node
// line marks the mode, which the scenario leaves to its default.
TEST_CASE("explain follows news-feed's fanout pool into graph_db and the feed caches", "[series]") {
  const auto text = explain_shipped("news-feed");
  CAPTURE(text);
  const auto fanout = check_line(text, "fanout");
  REQUIRE(fanout.contains("peak offered 2.162k/s vs 6.4k/s total capacity  (33.8%, green)"));
  REQUIRE_FALSE(fanout.contains("load not propagated"));
  const auto graph_db = check_line(text, "graph_db");
  REQUIRE(graph_db.contains(
      "peak offered 6.486k/s (2.162k/s × N 3) vs 180k/s (6 nodes)  (hottest 3.82%, green)"));
  REQUIRE_FALSE(graph_db.contains("not checked"));
  REQUIRE_FALSE(graph_db.contains("worker_pool"));
  REQUIRE_FALSE(graph_db.contains("message_queue"));
  REQUIRE(node_line(text, "graph_db")
              .contains("nodes=6 N=3 W=2 R=2 mode=ap (default; partitions: phase 4 backlog)"));
  const auto feed_cache = check_line(text, "feed_cache");
  REQUIRE(feed_cache.contains("peak offered 661.5k/s vs 1M/s total capacity  (66.2%, green)"));
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
  // The CDNs and the blob store fed straight from clients or caches are checked in MB/s (spec §19
  // entry 2), with a percentage and a zone: no "not checked" note, no propagation mark.
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
// The mark above covered only nodes whose paper load was exactly 0. A node that also receives load
// the paper pass follows got an ordinary verdict on a partial number: db below, fed 1/s by a client
// and 10k/s through a blob store's `then`, printed "(0.05%, green)". Every node with a blocking
// predecessor, direct or further up, is now marked; a reached one keeps its percentage as a lower
// bound ("≥"), with no zone below red. A red lower bound is a true verdict (the real load is at
// least that), so it keeps its "red". The blob store is no blocking type (its `then` is followed),
// so the two db cases below now show the complete number; the lower bound still marks what lies
// behind a worker pool or a message queue (the youtube cases further down).

TEST_CASE("a node fed partly through a blob store counts the store's notifications in full",
          "[series]") {
  const auto text = explain_of(partly_behind_blob_store(1.0));
  CAPTURE(text);
  const auto line = check_line(text, "db");
  // 1/s of reads plus the blob store's 10k/s of writes (one per served upload, factor 1) is 10001/s
  // against 2k/s: 500 %, red, a complete number: no lower bound and no mark (a blob store's then
  // steps are followed).
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

// The frontier's hand-offs add to the load a node gets from elsewhere, as a blob store's events do
// (above): 3000/s of reads plus the frontier's 10k/s of writes is 13000/s against 2k/s, 650 %, a
// complete number. This test pinned a red lower bound ("≥150%, red; load not propagated: ...")
// while the frontier was the one type the paper load did not follow; no type is left to pin it on
// since phase 3 Task 18, so it pins the full number instead.
TEST_CASE("a URL frontier's hand-offs add to the load a node gets from elsewhere", "[series]") {
  const auto text = explain_of(partly_behind_frontier(3000.0));
  CAPTURE(text);
  const auto line = check_line(text, "db");
  REQUIRE(line.contains("(650%, red)"));
  REQUIRE_FALSE(line.contains("load not propagated"));
  REQUIRE_FALSE(line.contains("≥"));
}

// A frontier hands URLs on along its `then` steps, not its bare links: audit is linked from the
// frontier but sent no step, so its 0 % is a real 0 and keeps its verdict, while db, which a step
// reaches, gets the frontier's 10k/s with the reader's 1/s: 500 %. (This test separated a step from
// a link through the "load not propagated" mark while a type was left unfollowed; since phase 3
// Task 18 none is, and the paper load itself makes the same distinction.)
TEST_CASE("a node linked from a URL frontier but sent no step keeps its verdict", "[series]") {
  const auto text = explain_of(partly_behind_frontier(1.0));
  CAPTURE(text);
  const auto line = check_line(text, "audit");
  REQUIRE(line.contains("(0%, green)"));
  REQUIRE_FALSE(line.contains("load not propagated"));
  const auto db = check_line(text, "db");
  REQUIRE(db.contains("(500%, red)"));
  REQUIRE_FALSE(db.contains("load not propagated"));
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

// chat-system's two kv_clusters are checked (they once printed "not checked"):
//  - message_store, 12 nodes of 10k/s, N = 3, vnodes 256 (k = 1 + z_12/sqrt(3 x 256) = 1.0590,
//    z_12 = 1.6350): the peak 31.5k sends/s (90k/s at the 21:00 peak x 0.35), each written ONCE
//    (phase 3 Task 15's decision, from the Task 14 review: the gateway's `fanout: 1.4` multiplies
//    only the steps of a message op that target a message queue, where ch. 12 copies a message once
//    per recipient into the sync queues; id_gen and message_store see the message once), are 31.5k
//    writes/s x 3 = 94.5k replica-requests/s over 120k/s, 78.75 % on average, the hottest node
//    1.0590 x 7.875k = 8.34k/s = 83.4 %: yellow (Task 14 had it at 132.3k, 117 %, red, with the
//    fan-out on every step). The numbers are the shipped scenario's sizing as Task 15 leaves it
//    (12 nodes of 10k/s, the sends' 35 % of 90k/s): a re-sized store changes them, and whoever
//    does says why in the scenario's README;
//  - id_gen, offered the same 31.5k/s (it was 44.1k/s), and message_q, which is where the fan-out
//    is: 31.5k x 1.4 = 44.1k/s published, one copy per recipient;
//  - presence, 4 nodes of the type's default 50k/s, N = 2: the users' heartbeats, sync_workers'
//    read of every message copy the queue holds (44.1k/s) and, since the gateway states a level in
//    Task 15, the gateway's own heartbeats C / 5 s, so at least the 93.6k/s that Task 14 printed
//    (49.5k users' heartbeats + 44.1k reads): how much more is the scenario's sizing, not pinned.
//    (The implementation of Task 15 left the heartbeats to the gateway's level alone, the users
//    send none, and sized presence at 10 nodes of 100k/s: 369k/s per copy at the 21:00 peak, the
//    scenario's README says why.)
// Behind the message queue and the pools the paper load follows, so nothing says "load not
// propagated" and nothing blames a queue or a pool.
TEST_CASE("explain checks chat-system's two kv_clusters at their hottest node", "[series]") {
  const auto text = explain_shipped("chat-system");
  CAPTURE(text);
  const auto store = check_line(text, "message_store");
  REQUIRE(store.contains(
      "peak offered 94.5k/s (31.5k/s × N 3) vs 120k/s (12 nodes)  (hottest 83.4%, yellow)"));
  REQUIRE(check_line(text, "id_gen").contains("peak offered 31.5k/s vs "));
  REQUIRE(check_line(text, "message_q").contains("peak offered 44.1k/s vs "));
  const auto presence = check_line(text, "presence");
  REQUIRE(presence.contains("× N 2)"));
  // "peak offered 187.2k/s (93.6k/s × N 2)": the per-copy figure in the parentheses, in thousands
  // (its k or M suffix applied), is at least what the users and the queue's readers alone offer.
  const auto per_copy_at = presence.find('(', presence.find("peak offered ")) + 1;
  std::size_t digits = 0;
  double per_copy_k = std::stod(presence.substr(per_copy_at), &digits);
  if (presence[per_copy_at + digits] == 'M') per_copy_k *= 1000.0;
  REQUIRE(per_copy_k >= 93.6);
  for (const auto& line : {store, presence}) {
    REQUIRE_FALSE(line.contains("not checked"));
    REQUIRE_FALSE(line.contains("worker_pool"));
    REQUIRE_FALSE(line.contains("message_queue"));
    REQUIRE_FALSE(line.contains("load not propagated"));
  }
}

// chat-system's presence and notification-system's log_db keep N = 2 copies and answer at the first
// reply for reads and writes alike (W = R = 1): fast, and the quorums do not overlap, so a read may
// miss the latest write, which explain now says beside the mode the scenarios leave to its default.
// Their rings: 10 machines, k = 1 + z_10 / sqrt(2 x 150) = 1.0897, printed 1.09 (chat-system's
// presence grew from 4 machines, k = 1.0606, in phase 3 Task 15, to carry the gateway's heartbeats:
// its README says why); 3 machines, 1 + z_3 / sqrt(300) = 1.0502, printed 1.05 (1.08 for both
// with the old 1 + 1/sqrt(150)).
TEST_CASE("explain notes the non-overlapping quorums of chat's presence and notification's log_db",
          "[series]") {
  const auto chat = explain_shipped("chat-system");
  CAPTURE(chat);
  REQUIRE(
      node_line(chat, "presence")
          .contains("nodes=10 N=2 W=1 R=1 (W + R ≤ N: a read may miss the latest write) "
                    "mode=ap (default; partitions: phase 4 backlog)  imbalance=1.09 (vnodes 150)"));
  const auto notification = explain_shipped("notification-system");
  CAPTURE(notification);
  REQUIRE(
      node_line(notification, "log_db")
          .contains("nodes=3 N=2 W=1 R=1 (W + R ≤ N: a read may miss the latest write) "
                    "mode=ap (default; partitions: phase 4 backlog)  imbalance=1.05 (vnodes 150)"));
}

// ---- `on: failure` steps on paper (spec §5) ----
//
// u (1000 writes/s) -> web, whose write is [db.write, log.write on failure]; db holds 500/s, log
// 2000/s. explain's paper load answers "can this ever fit" with nothing queued or dropped, so no
// write fails on paper and the failure step sends nothing: db is offered the whole 1k/s (red), log
// 0/s. How much reaches log is what db fails, which only `run` and `estimate` measure.
namespace {
Scenario failure_logged() {
  Scenario s{.name = "failure-logged", .version = 1, .duration = 1s, .step = 100ms};
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{1000}}, .op_shares = {{"write", 1.0}}}},
      NodeSpec{.id = "web",
               .type = NodeType::service,
               .capacity = Rate{10'000},
               .ops = {{"write",
                        {simsys::model::Step{.to = "db", .op = "write"},
                         simsys::model::Step{.to = "log",
                                             .op = "write",
                                             .on = simsys::model::StepCondition::failure}}}}},
      NodeSpec{.id = "db", .type = NodeType::service, .capacity = Rate{500}},
      NodeSpec{.id = "log", .type = NodeType::service, .capacity = Rate{2000}}};
  s.links = {LinkSpec{.from = "u", .to = "web"}, LinkSpec{.from = "web", .to = "db"},
             LinkSpec{.from = "web", .to = "log"}};
  return s;
}

constexpr std::string_view failure_fed_note =
    "fed by on: failure steps only: sized by run/estimate";
}  // namespace

TEST_CASE("explain's paper load sends nothing along a failure step: db 1k/s, log 0/s", "[series]") {
  const auto text = explain_of(failure_logged());
  CAPTURE(text);
  REQUIRE(
      check_line(text, "db").contains("peak offered 1k/s vs 500/s total capacity  (200%, red)"));
  REQUIRE(check_line(text, "log").contains("peak offered 0/s vs 2k/s total capacity  (0%, green)"));
}

// A 0 % green line for log would read as "log is idle", when log carries whatever db fails: the
// line says the paper load cannot size it. A node an ordinary step reaches (db) gets no note.
TEST_CASE("explain notes that a node fed by failure steps only is sized by run or estimate",
          "[series]") {
  const auto text = explain_of(failure_logged());
  CAPTURE(text);
  REQUIRE(check_line(text, "log").contains(failure_fed_note));
  REQUIRE_FALSE(check_line(text, "db").contains(failure_fed_note));
}

// ---- A link's bandwidth is a pipe: its rows in the series and the JSON ----
//
// A link with a bandwidth is a queue of megabytes and is plotted like a node: one CSV row per
// step with the link's name, `from->to`, in the `node` column, the same fourteen columns, in
// MEGABYTES (a rate of MB/s; engine::StepRecord::links, one LinkSample per bandwidth link). So
// the conservation law holds on those rows too, and a student checks the pipe from the file.
// The JSON gets `links`: one object per bandwidth link (an empty array without any).
//
// Picture: client `a` sends `rate` puts/s of 1 MB to service `b` over a 1 Gbit/s = 125 MB/s link.

namespace {
// The scenario: test_support::piped (tests/support/fixtures.cppm), a(`rate`/s of 1 MB
// `put`s) -> b over a 250 us link of 1 Gbit/s, 3 s at 100 ms steps, the first 500 ms warmup.

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
    "a link row of the node series carries the LinkSample it was fed, in the same fourteen columns",
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
  REQUIRE(row.size() == 14);
  REQUIRE(row[0] == "0.1");
  REQUIRE(row[2] == "100.5");   // demand_per_s, MB/s
  REQUIRE(row[3] == "100.25");  // drain_per_s
  REQUIRE(row[4] == "0.8");     // rho
  REQUIRE(row[5] == "34.25");   // backlog, MB
  REQUIRE(row[6] == "5");       // drops_per_s
  REQUIRE(row[7] == "");        // dead_lettered_per_s: a pipe is not a worker pool
  REQUIRE(row[8] == "0");       // rejected_per_s: a pipe refuses nothing by policy
  REQUIRE(row[9] == "");        // deduplicated_per_s: a pipe is no fanout
  REQUIRE(row[10] == "12.5");   // latency_ms: 0.0125 s
  REQUIRE(row[11] == "yellow");
  REQUIRE(row[12] == "");  // queue_lag_s: a pipe is not a message queue
  REQUIRE(row[13] == "");  // connections: a pipe is not a websocket gateway
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
  REQUIRE(std::ranges::all_of(rows, [](const auto& row) { return row.size() == 14; }));
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
// (Wq 16 ms + 8 ms, see test_bytes_pipes). The latency's key is `latency_s` if it follows the other
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

// What a link without a bandwidth did before pipes existed must not change by a byte.
// `tutorial/chain` (1.5k/s through two services and a database, no bandwidth, no size) is run for
// its whole 2 minutes and its node series and op series hashed (64-bit FNV-1a over the bytes of
// each file); the hashes were taken from the output of the last build before the pipe, with the
// same seed. A pipe's code path that leaks into a link without one changes a number, a row or a
// delay-line slot, and with it the hash. It guards the pipe (and every later change to it) against
// touching plain links; a later task that changes `chain`'s numbers ON PURPOSE (a new default, a
// model change that reaches services or databases) re-pins both hashes in the same commit and says
// why.
//
// Re-pinned on purpose: the node series gained a trailing `queue_lag_s` column (empty on every row
// but a message queue's), so each of the chain's 3,600 node rows ends in a comma and the header in
// `,queue_lag_s`, and nothing else changed. The new node hash was not taken from a build with the
// column but derived from the old output: the series of the binary before it (which reproduces the
// old hash, 0x3d3a6ed1d31da525) with `,queue_lag_s` appended to its header and `,` to each of its
// 3,600 rows, hashed with this function. An implementation whose chain rows differ from that in any
// byte (a number in the new column, a different empty cell) fails here. The op series had no new
// column then: its hash was the old one.
//
// Re-pinned on purpose: the node series gained a `dead_lettered_per_s` column right after
// `drops_per_s` (a worker pool's count of jobs whose last retry failed; empty on every row of a
// node that is not a pool, so on every row of the chain, which has no pool), and nothing else
// changed: the header has `,dead_lettered_per_s` after `drops_per_s` and each of the 3,600 rows an
// empty cell (`,,`) in the same place. Derived like the one above: the series of the binary at
// 61e047c (which reproduces the previous hash, 0xd0b4c7d05759bd57) with that header and that empty
// cell inserted, hashed with this function: 0x6e28178622e2c40f.
//
// Re-pinned on purpose (phase 3 Task 17): the node series gained a `deduplicated_per_s` column
// right after `rejected_per_s` (a notification fanout's count of notifications its dedup step
// dropped; empty on every row of a node that is not a fanout, so on every row of the chain), and
// nothing else changed: the header has `,deduplicated_per_s` after `rejected_per_s` and each of the
// 3,600 rows an empty cell (`,,`) in the same place. Derived like the ones above: the series of the
// binary at 35a5f3f (which reproduces the previous hash, 0x6e28178622e2c40f) with that header and
// that empty cell inserted, hashed with this function: 0xcd39f2a0b1120785.
//
// Re-pinned on purpose: the op series gained a `timed_out_per_s` column after `answered_per_s`, 0
// on every row of the chain (its client has no timeout), and nothing else changed. Derived the same
// way: the op series of the binary at 691a3ef (which reproduces the old hash, 0x9dfc7de84e1afe59)
// with `timed_out_per_s` inserted in its header and `0` in each of its rows before `mean_ms`,
// hashed with this function.
//
// Re-pinned on purpose: the node series gained a last column, `connections` (a ws_gateway's level
// of open connections, phase 3 Task 14; empty on every row of a node that is no gateway, so on
// every row of the chain), and nothing else changed: the header ends in `,connections` and each of
// the 3,600 rows in one more empty cell. Derived like the ones above: the node series of the binary
// at a145019 (which reproduces the previous hash, 0x6e28178622e2c40f) with `,connections` appended
// to its header and `,` to each of its rows, hashed with this function: 0xebb414ef9f9cb564; the
// binary with the column writes exactly that. The op series has no new column: its hash stays.
//
// Both columns together (Task 17 rebased onto Task 14): the node series of the Task 17 binary
// (which reproduces 0xcd39f2a0b1120785) with `,connections` appended to its header and `,` to each
// of its rows, hashed with this function: 0xb3c6f37ad04aa3ca; the binary with both columns writes
// exactly that.
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
  REQUIRE(header_of(nodes_out.str()) == node_header);
  // re-pinned for queue_lag_s, then for dead_lettered_per_s (phase 3 Task 11: an empty cell after
  // drops_per_s on every row, since chain has no worker pool), then for connections (phase 3 Task
  // 14: an empty last cell on every row, since chain has no websocket gateway), then for
  // deduplicated_per_s (Task 17: an empty cell after rejected_per_s on every row, since chain has
  // no fanout)
  REQUIRE(fnv1a(nodes_out.str()) == 0xb3c6f37ad04aa3caULL);
  // re-pinned for timed_out_per_s, then for attempts_per_s (phase 3 Task 10: the column after
  // offered_per_s, equal to it on every row, since chain has no retries)
  REQUIRE(fnv1a(ops_out.str()) == 0x66f6670d063b5f6fULL);
}

// ---- explain on youtube: a pool's capacity is workers / cost ----
//
// (node_line is tests/support/explain_lines.hpp's, shared with test_report.cpp.)

// transcoders: 600 workers at 60 s a video = 600 / 60 s = 10 videos/s (it was the worker_pool
// type's default 1k/s: 2 ms at c = 2; 400 workers, 6.667/s). completion_handler: 4 workers at 5 ms
// = 4 / 0.005 s = 800/s. The capacity figure is workers / cost; the paper load follows the queues
// and fills in the offered one.
TEST_CASE("explain checks youtube's transcoders against workers over cost: 10 a second",
          "[series]") {
  const auto text = explain_shipped("youtube");
  CAPTURE(text);
  const auto transcoders = check_line(text, "transcoders");
  REQUIRE(transcoders.contains("vs 10/s total capacity"));
  REQUIRE_FALSE(transcoders.contains("vs 1k/s"));
  const auto handler = check_line(text, "completion_handler");
  REQUIRE(handler.contains("vs 800/s total capacity"));
}

// What the nodes block says about youtube's queue and pool: 600 transcoders of 60 s a job
// (`cost=1m`), 32 partitions on transcode_q, and no retention stated, so Kafka's default.
TEST_CASE("explain describes youtube's transcode queue and transcoders", "[series]") {
  const auto text = explain_shipped("youtube");
  CAPTURE(text);
  const auto transcoders = node_line(text, "transcoders");
  REQUIRE(transcoders.contains("capacity=10/s (workers / cost)"));
  // 7 days x 10/s = 604,800 s x 600 / 60 = 6,048,000 messages: 6.048M.
  REQUIRE(transcoders.contains("queue_limit=6.048M (retention × capacity)"));
  REQUIRE(transcoders.contains("consumes=transcode_q  workers=600 cost=1m"));
  const auto queue = node_line(text, "transcode_q");
  REQUIRE(queue.contains("retention=7d (default: Kafka's 168 h)"));
  REQUIRE(queue.contains("partitions=32 (not modelled)"));
}

// chat-system scripts one fault: its gateway tier (`chat_servers`) down at 21:30 for a minute, the
// reconnect storm of book ch. 12. `explain` lists the scripted faults so a reader sees them before
// running: a count, then each one as `kind node at for length`. A scenario that scripts none
// prints no line at all (not `faults: none`), so every earlier scenario's explain is unchanged.
TEST_CASE("explain lists chat-system's scripted fault", "[series]") {
  const auto text = explain_shipped("chat-system");
  CAPTURE(text);
  REQUIRE(text.contains("faults: 1 scripted (node_down chat_servers 21:30 for 1m)"));
}

// news-feed's fault with its database's primary down instead of the whole node: the size reads
// `primary`, as the scenario writes it (`replicas: primary`), not `replicas=primary`.
TEST_CASE("explain names a sql_db's primary down as primary", "[series]") {
  auto loaded =
      simsys::config::load(std::string{SIMSYS_SCENARIOS_DIR} + "/book/news-feed/scenario.yaml");
  INFO((loaded ? "" : loaded.error().message));
  REQUIRE(loaded.has_value());
  REQUIRE(loaded->faults.size() == 1);
  loaded->faults[0].primary = true;
  const auto text = simsys::report::render_explain(graph_of(*loaded));
  CAPTURE(text);
  REQUIRE(text.contains("faults: 1 scripted (node_down post_db 15:00 for 30s primary)"));
}

TEST_CASE("explain prints no faults line for a scenario without faults", "[series]") {
  const auto loaded =
      simsys::config::load(std::string{SIMSYS_SCENARIOS_DIR} + "/tutorial/hello/scenario.yaml");
  INFO((loaded ? "" : loaded.error().message));
  REQUIRE(loaded.has_value());
  const auto text = simsys::report::render_explain(graph_of(*loaded));
  CAPTURE(text);
  REQUIRE_FALSE(text.contains("faults:"));
}

// ---- node series: dead_lettered_per_s (phase 3 spec §6.2, §10) ----
//
// A worker pool's dead letters, the jobs whose last retry failed (engine::NodeSample::
// dead_lettered_per_s), are the node series' column right after `drops_per_s`: they are not drops,
// and the column says so by being a column of their own. Like `queue_lag_s` it is empty, not 0, on
// every row of a node that is not a pool (a 0 would claim a dead-letter queue the node does not
// have), the client's and the bandwidth links' rows included; a pool that retries nothing has 0.
//
// Picture: u publishes 1000/s to q; pool w (100 workers at 10 ms) calls the sink api (5k/s, fails
// 2 %) with `retries: 3`, 1 s apart: at the fixed point w dead-letters 1000 x 0.02^4 = 1.6e-4 a
// second and api is sent 1020.408/s, of which it drops 20.408 (its own books, which this file's
// conservation tests read: the failures of calls that will be retried are still the sink's
// drops in the series).

TEST_CASE("the node series header has dead_lettered_per_s right after drops_per_s", "[series]") {
  const auto g = graph_of(queue_and_group());
  std::ostringstream out;
  simsys::report::SeriesWriter writer{out, g};
  const auto header = split_commas(header_of(out.str()));
  REQUIRE(header.size() == 14);
  REQUIRE(header[6] == "drops_per_s");
  REQUIRE(header[7] == "dead_lettered_per_s");
  REQUIRE(header[8] == "rejected_per_s");
}

TEST_CASE("a pool's row carries the dead letters of its sample and every other row is empty there",
          "[series]") {
  const auto s = queue_and_group();
  const auto g = graph_of(s);
  std::ostringstream out;
  simsys::report::SeriesWriter writer{out, g};
  std::vector<simsys::engine::NodeSample> nodes(3);  // u 0, q 1, w 2
  nodes[2].dead_lettered_per_s = 0.25;               // exactly printable
  writer.observe(simsys::engine::StepRecord{.t = 100ms, .nodes = nodes, .responses = {}});
  const auto rows = data_rows(out.str());
  REQUIRE(rows.size() == 3);
  REQUIRE(rows[2][1] == "w");
  REQUIRE(rows[2][7] == "0.25");
  for (const auto i : {std::size_t{0}, std::size_t{1}}) {
    CAPTURE(rows[i][1]);
    REQUIRE(rows[i].size() == 14);
    REQUIRE(rows[i][7] == "");  // the client's and the queue's: not a count of 0
  }
}

namespace {
// u -> q -> w (retries 3, 1 s apart) -> api, the picture above; the pool states `retries` only when
// given. 5 s: 50 steps, past the three backoffs of 10.
Scenario retrying_pool_series(std::optional<int> retries) {
  auto pool = group_of("w", 100, 10ms);
  pool.then = {Step{.to = "api", .op = "send"}};
  if (retries) pool.retries = *retries;
  if (retries) pool.retry_after = 1s;
  auto s = published(1000, {std::move(pool)});
  NodeSpec api{.id = "api", .type = NodeType::external_sink, .capacity = Rate{5000}};
  api.failure_rate = 0.02;
  s.nodes.push_back(std::move(api));
  s.links.push_back(LinkSpec{.from = "w", .to = "api", .latency = 0s});
  s.duration = 5s;
  return s;
}

// The rows of node `id` of a run of `s`, in step order.
std::vector<std::vector<std::string>> rows_of_node(const Scenario& s, std::string_view id) {
  const auto g = graph_of(s);
  std::ostringstream out;
  simsys::report::SeriesWriter writer{out, g};
  run_into(s, g, [&](const auto& r) { writer.observe(r); });
  std::vector<std::vector<std::string>> mine;
  for (auto& row : data_rows(out.str())) {
    if (row[1] == id) mine.push_back(std::move(row));
  }
  return mine;
}
}  // namespace

TEST_CASE("a run's pool rows hold the dead letters, 1.6e-4 a second, and the others stay empty",
          "[series]") {
  const auto s = retrying_pool_series(3);
  const auto pool = rows_of_node(s, "w");
  REQUIRE(pool.size() == 50);
  REQUIRE(pool[0].size() == 14);
  REQUIRE(number(pool[0][7]) == 0.0);  // nobody has used 3 retries at the first step
  REQUIRE_THAT(number(pool.back()[7]), WithinRel(1.6e-4, 1e-6));
  REQUIRE(number(pool.back()[6]) == 0.0);  // drops_per_s: a dead letter is not a drop
  for (const auto* id : {"u", "q", "api"}) {
    CAPTURE(id);
    for (const auto& row : rows_of_node(s, id)) REQUIRE(row[7].empty());
  }
}

TEST_CASE("a pool that retries nothing writes 0 in dead_lettered_per_s on every row", "[series]") {
  for (const auto& row : rows_of_node(retrying_pool_series(std::nullopt), "w")) {
    REQUIRE(row[7] == "0");
  }
}

// The sink's books in the file are the sink's, whatever its callers do with the failures: the
// identity demand = drain + drops holds on its rows (failures move flow from `drain` to `drops`),
// and the drops are the 2 % of everything it was sent, 20.408/s at the fixed point.
TEST_CASE("the sink's rows keep conservation and its drops, 20.408 a second, with retries",
          "[series]") {
  const auto rows = rows_of_node(retrying_pool_series(3), "api");
  REQUIRE(rows.size() == 50);
  constexpr double dt = 0.1;
  double demand = 0.0;
  double drain = 0.0;
  double drops = 0.0;
  for (const auto& row : rows) {
    demand += number(row[2]) * dt;
    drain += number(row[3]) * dt;
    drops += number(row[6]) * dt;
  }
  // The series prints six significant digits (1020.41, 20.4082), so the books read back from the
  // CSV balance to about 1e-6, not to the 1e-9 of the raw samples (test_sinks pins those).
  REQUIRE_THAT(demand, WithinRel(drain + drops, 1e-5));
  REQUIRE_THAT(number(rows.back()[6]), WithinRel(0.02 * 1020.408, 1e-5));
}

// ---- explain: a pool's retries are paper load too ----
//
// notification-system's iOS channel on its own (the shape of the README's probe): 1140/s published
// to q, ios_workers (50 workers at 20 ms: 2500/s) sends each job to apns (an external sink at its
// type's 1k/s quota that fails 2 %), `retries: 3`. The pool drains every attempt, so on paper it
// and its sink are offered the jobs times the attempts per job, the capped geometric series
//   A(f, n) = 1 + f + ... + f^n,  A(0.02, 3) = 1.020408
// (behaviours::WorkerPoolBehaviour): 1140 x 1.020408 = 1163.3/s, 46.5 % of the pool (45.6 % without
// the retries) and 116 % of apns's quota (114 % without).
namespace {
Scenario ios_channel(std::optional<int> retries) {
  auto pool = group_of("ios_workers", 50, 20ms);
  pool.then = {Step{.to = "apns", .op = "send"}};
  pool.retries = retries;
  auto s = published(1140.0, {std::move(pool)});
  NodeSpec apns{.id = "apns", .type = NodeType::external_sink, .latency = 150ms};
  apns.failure_rate = 0.02;
  s.nodes.push_back(std::move(apns));
  s.links.push_back(LinkSpec{.from = "ios_workers", .to = "apns", .latency = 0s});
  return s;
}
}  // namespace

TEST_CASE("explain offers a retrying pool and its sink A(f, n) times the jobs: 46.5 percent",
          "[series]") {
  const auto text = explain_of(ios_channel(3));
  CAPTURE(text);
  REQUIRE(check_line(text, "ios_workers").contains("(46.5%, green)"));
  REQUIRE(check_line(text, "apns").contains("(116%, red)"));
}

TEST_CASE("explain offers a pool without retries its queue's jobs alone: 45.6 percent",
          "[series]") {
  const auto text = explain_of(ios_channel(std::nullopt));
  CAPTURE(text);
  REQUIRE(check_line(text, "ios_workers").contains("(45.6%, green)"));
  REQUIRE(check_line(text, "apns").contains("(114%, red)"));
}

// Two sinks in a row, failing 10 % and 20 %: a job is retried when either fails it, the combined
// share 1 − 0.9 × 0.8 = 0.28, so with one retry the pool drains A(0.28, 1) = 1.28 times its jobs:
// 1000 x 1.28 = 1280/s of its 2500/s, 51.2 %.
TEST_CASE("explain retries a pool calling two sinks at their combined failure share: 51.2 percent",
          "[series]") {
  auto pool = group_of("w", 50, 20ms);
  pool.then = {Step{.to = "a", .op = "send"}, Step{.to = "b", .op = "send"}};
  pool.retries = 1;
  auto s = published(1000.0, {std::move(pool)});
  for (const auto& [id, failure_rate] : {std::pair{"a", 0.1}, std::pair{"b", 0.2}}) {
    NodeSpec sink{.id = id, .type = NodeType::external_sink, .capacity = Rate{5000}};
    sink.failure_rate = failure_rate;
    s.nodes.push_back(std::move(sink));
    s.links.push_back(LinkSpec{.from = "w", .to = id, .latency = 0s});
  }
  const auto text = explain_of(s);
  CAPTURE(text);
  REQUIRE(check_line(text, "w").contains("(51.2%, green)"));
}

// ---- explain: a service discovery's numbers and its failover (phase 3 spec §8.1) ----
//
// A service_discovery is a service whose own key is `failover`, how long callers keep sending to a
// dead replica of a node they looked up (book ch. 12's registry learns of a failure late). Its
// explain line is a service's, the type's numbers marked (default), then `failover=<duration>`
// closing the line the way a search index's `build_interval` does. The default failover, 10 s, is
// filled in by the loader, so every line prints the number the engine uses.

namespace {
// u (1k/s of `call`) -> api -> [disc.lookup, svc.call]; disc states no numbers but its failover
// (set after the designated initializers, so that the field's place in NodeSpec does not matter).
Scenario discovered_scenario(simsys::SimTime failover) {
  Scenario s{.name = "discovered", .version = 1, .duration = 1s, .step = 100ms};
  NodeSpec disc{.id = "disc", .type = NodeType::service_discovery};
  disc.failover = failover;
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{1000}}, .op_shares = {{"call", 1.0}}}},
      NodeSpec{
          .id = "api",
          .type = NodeType::service,
          .capacity = Rate{100'000},
          .ops = {{"call", {Step{.to = "disc", .op = "lookup"}, Step{.to = "svc", .op = "call"}}}}},
      std::move(disc), NodeSpec{.id = "svc", .type = NodeType::service, .capacity = Rate{4000}}};
  s.links = {LinkSpec{.from = "u", .to = "api", .latency = 0s},
             LinkSpec{.from = "api", .to = "disc", .latency = 0s},
             LinkSpec{.from = "api", .to = "svc", .latency = 0s}};
  return s;
}
}  // namespace

// With nothing stated the type's numbers stand, each marked (default): 50k/s and 0.5 ms (book:
// one round trip within the data centre), c = 50,000/s x 0.5 ms = 25 servers (Little's law); and
// the 1k lookups a second of the checks block are 1000 / 50,000 = 2 % of the registry.
TEST_CASE("explain prints a service discovery's defaults, 50k/s and 0.5 ms, and its failover",
          "[series]") {
  const auto text = explain_of(discovered_scenario(30s));
  CAPTURE(text);
  REQUIRE(node_line(text, "disc")
              .contains("dc=main  replicas=1  capacity=50k/s (default)  service_time=500us "
                        "(default)  c=25 (implied)  queue_limit=1000 (default)  variability=1  "
                        "burstiness=1  failover=30s"));
  REQUIRE(node_line(text, "disc").ends_with("failover=30s"));
  REQUIRE(
      check_line(text, "disc").contains("peak offered 1k/s vs 50k/s total capacity  (2%, green)"));
}

// The shipped chat-system's discovery states the book's numbers and failover 10s: the line ends
// with it.
TEST_CASE("explain on chat-system prints its discovery's failover=10s", "[series]") {
  const auto text = explain_shipped("chat-system");
  CAPTURE(text);
  REQUIRE(node_line(text, "discovery")
              .contains("dc=main  replicas=1  capacity=50k/s (scenario)  service_time=500us "
                        "(scenario)  c=25 (implied)  queue_limit=1000 (default)  variability=1  "
                        "burstiness=1  failover=10s"));
  REQUIRE(node_line(text, "discovery").ends_with("burstiness=1  failover=10s"));
}

// A failover is the registry's own: no other node's line carries one.
TEST_CASE("explain prints failover on the service discovery's line alone", "[series]") {
  const auto text = explain_of(discovered_scenario(10s));
  CAPTURE(text);
  for (const auto* id : {"u", "api", "svc"}) {
    CAPTURE(id);
    REQUIRE_FALSE(node_line(text, id).contains("failover"));
  }
  REQUIRE(node_line(text, "disc").contains("failover=10s"));
}

// ---- explain follows a notification fanout's then steps (phase 3 spec §8.3, §10) ----
//
// The fanout was one of the two types whose `then` the paper load could not follow, and every node
// behind it carried `(load not propagated: notification_fanout is not modelled yet)`. It is
// modelled now: the paper load sends each `then` target `factor x (1 - dedup)` of what the fanout
// is offered, the same shares the run applies, and the mark for it is gone.
//
// The probe (as test_queues_pools.cpp's): u routes 1000/s to `fanout` (dedup 0.02, the type's
// 20k/s and 1 ms), whose steps are q1 x 0.6, q2 x 0.3 and log_db x 1 (a kv_cluster, N = 2):
//   q1 1000 x 0.98 x 0.6 = 588/s      q2 1000 x 0.98 x 0.3 = 294/s
//   log_db 980 writes/s, 1960/s of replica requests (980/s x N 2)

namespace {
struct SeriesScratchFile {
  std::filesystem::path path;
  explicit SeriesScratchFile(std::filesystem::path p) : path(std::move(p)) {}
  SeriesScratchFile(const SeriesScratchFile&) = delete;
  SeriesScratchFile& operator=(const SeriesScratchFile&) = delete;
  ~SeriesScratchFile() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
};

constexpr std::string_view fanout_explain_yaml =
    "version: 1\nname: fanout-explain\nduration: 10s\nnodes:\n"
    "  u: {type: client, rate: {base: 1k/s}, ops: {route: 1.0}}\n"
    "  fanout:\n    type: notification_fanout\n    dedup: 0.02\n    channels: {a: q1, b: q2}\n"
    "    then:\n"
    "      - {to: q1, op: publish, factor: 0.6}\n"
    "      - {to: q2, op: publish, factor: 0.3}\n"
    "      - {to: log_db, op: write}\n"
    "  q1: {type: message_queue}\n"
    "  q2: {type: message_queue}\n"
    "  w1: {type: worker_pool, consumes: q1, workers: 100, cost: 10ms}\n"
    "  w2: {type: worker_pool, consumes: q2, workers: 100, cost: 10ms}\n"
    "  log_db: {type: kv_cluster, nodes: 3, replication: 2, write_quorum: 1, read_quorum: 1, "
    "capacity: 20k/s}\n"
    "links:\n"
    "  - {from: u, to: fanout, latency: 0s}\n"
    "  - {from: fanout, to: q1, latency: 0s}\n"
    "  - {from: fanout, to: q2, latency: 0s}\n"
    "  - {from: fanout, to: log_db, latency: 0s}\n"
    "  - {from: q1, to: w1, latency: 0s}\n"
    "  - {from: q2, to: w2, latency: 0s}\n";

std::string explain_of_fanout_probe() {
  const SeriesScratchFile file{std::filesystem::temp_directory_path() /
                               (std::to_string(::getpid()) + "_simsys_series_fanout.yaml")};
  std::ofstream(file.path) << fanout_explain_yaml;
  const auto loaded = simsys::config::load(file.path.string());
  INFO((loaded ? "" : loaded.error().message));
  REQUIRE(loaded.has_value());
  return simsys::report::render_explain(graph_of(*loaded));
}

// The number after "peak offered " on a check line, with its k or M suffix applied.
double offered_in(const std::string& line) {
  const std::string key = "peak offered ";
  const auto at = line.find(key);
  INFO("check line: " << line);
  REQUIRE(at != std::string::npos);
  std::size_t used = 0;
  double value = std::stod(line.substr(at + key.size()), &used);
  const char suffix = line[at + key.size() + used];
  if (suffix == 'k') value *= 1e3;
  if (suffix == 'M') value *= 1e6;
  return value;
}
}  // namespace

TEST_CASE("explain gives a fanout the type's 20k/s and 1 ms, marked as defaults", "[series]") {
  const auto text = explain_of_fanout_probe();
  CAPTURE(text);
  REQUIRE(node_line(text, "fanout")
              .contains("capacity=20k/s (default)  service_time=1ms (default)  c=20 (implied)"));
  REQUIRE(check_line(text, "fanout")
              .contains("peak offered 1k/s vs 20k/s total capacity  (5%, green)"));
}

TEST_CASE("explain sends a fanout's then targets factor x (1 - dedup): 588/s, 294/s, 980 x N 2",
          "[series]") {
  const auto text = explain_of_fanout_probe();
  CAPTURE(text);
  REQUIRE(check_line(text, "q1").contains("peak offered 588/s vs 1M/s total capacity"));
  REQUIRE(check_line(text, "q2").contains("peak offered 294/s vs 1M/s total capacity"));
  REQUIRE(
      check_line(text, "log_db").contains("peak offered 1.96k/s (980/s × N 2) vs 60k/s (3 nodes)"));
  // The pools behind the queues follow too: their jobs are the queues' messages.
  REQUIRE(check_line(text, "w1").contains("peak offered 588/s vs 10k/s total capacity"));
}

TEST_CASE("explain marks no load as not propagated behind a fanout", "[series]") {
  const auto text = explain_of_fanout_probe();
  CAPTURE(text);
  REQUIRE_FALSE(text.contains("load not propagated"));
  REQUIRE_FALSE(text.contains("not modelled yet"));
}

// notification-system (ch. 10): the shipped file's fanout has dedup 0.02 and shares 0.6, 0.3, 0.05
// and 0.3 for ios_q, android_q, sms_q and email_q, then `log_db` (the shares sum to 1.25: a
// notification goes to several channels). explain no longer says "not modelled yet" anywhere in
// the file, and each target's paper load is the fanout's own peak times factor x 0.98 (explain
// prints three or four significant digits, so the ratio is checked to 0.5 %). log_db's replica
// requests are its writes times N = 2. The numbers themselves (the 09:00 blast through the fanout)
// are the scenario's README's and not pinned here: they follow the file's sizing.
TEST_CASE("explain on notification-system follows the fanout into its four queues and log_db",
          "[series]") {
  const auto text = explain_shipped("notification-system");
  CAPTURE(text);
  REQUIRE_FALSE(text.contains("load not propagated"));
  REQUIRE_FALSE(text.contains("notification_fanout is not modelled yet"));
  REQUIRE_FALSE(check_line(text, "fanout").contains("load not propagated"));
  const double fanout = offered_in(check_line(text, "fanout"));
  REQUIRE(fanout > 0.0);
  const std::vector<std::pair<std::string, double>> targets{{"ios_q", 0.6 * 0.98},
                                                            {"android_q", 0.3 * 0.98},
                                                            {"sms_q", 0.05 * 0.98},
                                                            {"email_q", 0.3 * 0.98},
                                                            {"log_db", 2.0 * 0.98}};
  for (const auto& [id, share] : targets) {
    CAPTURE(id, share);
    const auto line = check_line(text, id);
    REQUIRE_FALSE(line.contains("load not propagated"));
    REQUIRE_THAT(offered_in(line), WithinRel(fanout * share, 5e-3));
  }
}

// ---- Web crawler: the frontier's `then` is followed (phase 3 Task 18) ----
//
// The URL frontier runs now, so explain's paper load follows its `then` steps like a worker pool's,
// and the last "load not propagated" mark of the shipped scenarios goes: no node of the web crawler
// is marked on account of the frontier, in particular not the downloader it hands URLs to.
TEST_CASE("explain follows the web crawler's frontier: no node of it is marked as not propagated",
          "[series]") {
  const auto text = explain_shipped("web-crawler");
  CAPTURE(text);
  for (const auto* id : {"frontier", "downloader", "dns", "content_store", "url_seen"}) {
    CAPTURE(id);
    const auto line = check_line(text, id);
    REQUIRE_FALSE(line.empty());  // the node has a check line, so the absence below is not vacuous
    REQUIRE_FALSE(line.contains("load not propagated"));
  }
  REQUIRE_FALSE(text.contains("load not propagated"));
  REQUIRE_FALSE(text.contains("frontier is not modelled yet"));
}

// Phase 3 Task 16: google-drive's long_poll states `connections: 2M` (with max_connections 2M x 4
// replicas, heartbeat 60 s), so explain's gateway line reads Task 14's fixed-level format: the
// level 2M, "(fixed)", the heartbeat load 2M / 60 s = 33.33k/s, the cap 2M x 4 = 8M. Until the YAML
// is edited it reads "connections: none (no connect op reaches it, no fixed connections: run
// refuses it)", the line of a gateway run would refuse.
TEST_CASE("explain on google-drive prints long_poll's fixed 2M level, 33.33k heartbeats and cap 8M",
          "[series]") {
  const auto text = explain_shipped("google-drive");
  CAPTURE(text);
  REQUIRE(node_line(text, "long_poll")
              .ends_with("  connections: 2M (fixed), heartbeat 33.33k/s, cap 8M"));
}

// The paper load now follows every edge of google-drive: no check line says the load stops
// somewhere (`load not propagated`), and no node line says a type is "not modelled yet". The
// phrase "not modelled" alone stays legitimate on notify_q's line, "partitions=8 (not
// modelled)": that is a key the model ignores on purpose, not a node it cannot follow.
// The gateway's check line adds the heartbeats to the pushes and polls it is sent: peak offered
// 33.33k/s + the 511/s of pushes + the 60/s of polls = 33.93k/s (measured on a scratch copy),
// against 4 replicas x 20k/s = 80k/s, so 42.4 %, green. Sized as the shipped file does today;
// the implementer re-measures if the replicas or the capacity change.
TEST_CASE(
    "explain on google-drive marks no load as not propagated and checks the gateway's heartbeats",
    "[series]") {
  const auto text = explain_shipped("google-drive");
  CAPTURE(text);
  REQUIRE_FALSE(text.contains("load not propagated"));
  REQUIRE_FALSE(text.contains("not modelled yet"));
  const auto gateway = check_line(text, "long_poll");
  REQUIRE(gateway.contains("peak offered 33.93k/s vs 80k/s total capacity  (42.4%, green)"));
}

// Task 15's fan-out decision at its source: chat-system's sync_workers hand each message copy the
// queue holds to its recipient's gateway (`deliver`) and 0.3 of them to the push gateway, but do
// not multiply them again: the 1.4 recipients per message are already the copies in the queue
// (the gateway's `fanout` at its publish step), so a `factor: 1.4` on the deliver step would count
// the fan-out twice (Task 14 left it, and explain printed the message store at 117 %).
TEST_CASE("chat-system's sync_workers deliver each queued message copy once: no factor above 1",
          "[series]") {
  const auto loaded =
      simsys::config::load(std::string{SIMSYS_SCENARIOS_DIR} + "/book/chat-system/scenario.yaml");
  INFO((loaded ? "" : loaded.error().message));
  REQUIRE(loaded.has_value());
  // C++20 std::ranges::find with a projection: the node whose `id` member equals "sync_workers",
  // replacing std::find_if with a lambda that reads the member by hand.
  const auto workers =
      std::ranges::find(loaded->nodes, "sync_workers", &simsys::model::NodeSpec::id);
  REQUIRE(workers != loaded->nodes.end());
  bool delivers = false;
  for (const auto& step : workers->then) {
    if (step.to != "chat_servers") continue;
    delivers = true;
    REQUIRE(step.factor == 1.0);
  }
  REQUIRE(delivers);
}
