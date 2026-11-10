// The output contract a frontend reads (phase 5a, spec 2026-11-10-phase5a-streaming-design.md):
// the format_version every JSON document carries, the report on one line, the resolved graph as
// JSON, the frames of `run --stream` and the stream's lines. Lines of JSON are parsed with
// yaml-cpp: JSON is YAML 1.2's flow syntax, so a JSON object loads as a YAML map and a test
// compares trees, not text.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>  // getpid

#include <yaml-cpp/yaml.h>

// C++20 `import`: the compiled interfaces of the modules under test, after every #include (the
// eighth GCC 14.2 defect: a std header included after an import clashes with the module's).
import simsys.config;
import simsys.core;
import simsys.engine;
import simsys.fluid;
import simsys.graph;
import simsys.model;
import simsys.report;
import simsys.session;
import simsys.test_fixtures;

using namespace std::chrono_literals;
using Catch::Matchers::WithinRel;
using simsys::engine::NodeSample;
using simsys::engine::StepRecord;
using simsys::report::Frame;
using simsys::report::FrameAggregator;

namespace {

std::string shipped(std::string_view relative) {
  return std::string{SIMSYS_SCENARIOS_DIR} + "/" + std::string{relative};
}

// A scratch scenario file of this process, written fresh by each test that needs one.
std::string scratch_scenario(std::string_view name, std::string_view yaml) {
  const auto dir =
      std::filesystem::temp_directory_path() / ("simsys_stream_" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  const auto path = dir / (std::string{name} + ".yaml");
  std::ofstream{path} << yaml;
  return path.string();
}

// A minute of 100 MB/s over a 1 Gbit/s pipe into a service that is down from 20 s to 30 s: a link
// row, a fault block and an SLO in one small report.
constexpr std::string_view probe_yaml = R"(version: 1
name: probe
duration: 1m
warmup: 5s
nodes:
  users: {type: client, rate: {base: 100/s}, ops: {get: 1}, sizes: {get: 1MB}}
  api: {type: service, capacity: 1k/s, latency: 1ms}
links:
  - {from: users, to: api, dir: inout, bandwidth: 1Gbit/s}
faults: [{at: 20s, kind: node_down, node: api, for: 10s}]
slo:
  - {metric: drop_rate, max: 0}
)";

simsys::graph::Graph graph_of(const std::string& path) {
  auto g = simsys::session::load_graph(path);
  if (!g) FAIL(g.error());
  return std::move(*g);
}

// The run's JSON report, pretty (`run --report json`) and on one line (the stream's summary).
struct BothReports {
  std::string pretty;
  std::string line;
};

// C++20 designated initializers below (`{.pretty = ..., .line = ...}`): each member named where the
// value is built, replacing positional braces where a reader must remember which string is which.
BothReports reports_of(const std::string& path) {
  const auto g = graph_of(path);
  const auto summary = simsys::session::run_summary(g, path);
  if (!summary) FAIL(summary.error());
  const auto slos = simsys::report::evaluate_slos(*summary, g.scenario().slos);
  return {.pretty = simsys::report::render_json(*summary, slos),
          .line = simsys::report::render_json_line(*summary, slos)};
}

// Two parsed JSON documents hold the same values: the same keys in the same order, the same
// scalars as text, the same lengths.
bool same_tree(const YAML::Node& a, const YAML::Node& b) {
  if (a.Type() != b.Type()) return false;
  if (a.IsScalar()) return a.Scalar() == b.Scalar();
  if (a.size() != b.size()) return false;
  if (a.IsSequence()) {
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (!same_tree(a[i], b[i])) return false;
    }
    return true;
  }
  if (a.IsMap()) {
    auto ia = a.begin();
    auto ib = b.begin();
    for (; ia != a.end(); ++ia, ++ib) {
      if (ia->first.Scalar() != ib->first.Scalar()) return false;
      if (!same_tree(ia->second, ib->second)) return false;
    }
    return true;
  }
  return true;  // two nulls
}

std::size_t occurrences(std::string_view text, std::string_view word) {
  std::size_t count = 0;
  for (auto at = text.find(word); at != std::string_view::npos; at = text.find(word, at + 1)) {
    ++count;
  }
  return count;
}

}  // namespace

// ---- format_version and the report on one line (Task 1) -----------------------------------------

TEST_CASE("a run's JSON report names the output format it follows, first", "[stream]") {
  const auto json = reports_of(shipped("tutorial/chain/scenario.yaml")).pretty;
  // C++20 std::string::starts_with: a prefix test without the old `rfind(prefix, 0) == 0` or a
  // compare on a substring.
  CHECK(json.starts_with("{\n  \"format_version\": 1,\n  \"scenario\": \"chain\","));
  CHECK(occurrences(json, "format_version") == 1);
  CHECK(simsys::report::format_version == 1);
}

TEST_CASE("an estimate's JSON report names the output format it follows, first", "[stream]") {
  const auto g = graph_of(shipped("tutorial/chain/scenario.yaml"));
  auto sim = simsys::session::create_simulation(g, "chain");
  REQUIRE(sim);
  const auto summary = simsys::report::Summary::of_estimate(g, sim->estimate());
  const auto json = simsys::report::render_json(summary, {});
  CHECK(json.starts_with("{\n  \"format_version\": 1,\n  \"scenario\": \"chain\","));
}

TEST_CASE("replications' JSON names the output format once, at the top", "[stream]") {
  const auto path = scratch_scenario("noisy", R"(version: 1
name: noisy
duration: 10s
nodes:
  users: {type: client, rate: {base: 100/s, noise: {sigma: 0.2, correlation: 1s}}, ops: {ping: 1}}
  api: {type: service, capacity: 1k/s, latency: 1ms}
links:
  - {from: users, to: api, dir: inout}
)");
  const auto g = graph_of(path);
  const auto runs = simsys::session::run_replications(g, 1, 3, 1);
  const auto replicated =
      simsys::report::summarize_replications(runs, simsys::session::replication_seeds(1, 3));
  const auto json = simsys::report::render_replications_json(replicated, runs);
  CHECK(json.starts_with("{\n  \"format_version\": 1,\n  \"scenario\": \"noisy\","));
  CHECK(occurrences(json, "format_version") == 1);
}

TEST_CASE("the report on one line holds exactly the values of the pretty report", "[stream]") {
  for (const auto& path :
       {shipped("tutorial/chain/scenario.yaml"), shipped("labs/rate-limiter/scenario.yaml"),
        scratch_scenario("probe", probe_yaml)}) {
    INFO(path);
    const auto reports = reports_of(path);
    CHECK(reports.line.find('\n') == std::string::npos);
    CHECK(reports.line.starts_with("{\"format_version\":1,"));
    CHECK(same_tree(YAML::Load(reports.line), YAML::Load(reports.pretty)));
  }
}

TEST_CASE("the probe's one-line report keeps its link, its fault block and its SLO", "[stream]") {
  const auto line = reports_of(scratch_scenario("probe", probe_yaml)).line;
  const auto doc = YAML::Load(line);
  CHECK(doc["links"].size() == 1);
  CHECK(doc["faults"].size() == 1);
  CHECK(doc["slo"].size() == 1);
}

TEST_CASE("a space inside a JSON string survives the one-line layout", "[stream]") {
  // Only the layout's whitespace goes: a scenario named with spaces keeps them.
  std::string yaml{probe_yaml};
  yaml.replace(yaml.find("name: probe"), 11, "name: a probe  with spaces");
  const auto reports = reports_of(scratch_scenario("spaced", yaml));
  CHECK(YAML::Load(reports.line)["scenario"].Scalar() == "a probe  with spaces");
  CHECK(same_tree(YAML::Load(reports.line), YAML::Load(reports.pretty)));
}

// ---- the resolved graph as JSON (Task 2) --------------------------------------------------------

namespace {

YAML::Node graph_json_of(std::string_view relative) {
  return YAML::Load(simsys::report::render_graph_json(graph_of(shipped(relative))));
}

// The entry of `list` whose `key` is `value` (a node by id, a link by its ends); fails the test
// when none is.
YAML::Node entry(const YAML::Node& list, std::string_view key, std::string_view value) {
  for (const auto& item : list) {
    if (item[std::string{key}].Scalar() == value) return item;
  }
  FAIL("no entry with " << key << " = " << value);
  return {};
}

YAML::Node link_between(const YAML::Node& links, std::string_view from, std::string_view to) {
  for (const auto& link : links) {
    if (link["from"].Scalar() == from && link["to"].Scalar() == to) return link;
  }
  FAIL("no link " << from << " -> " << to);
  return {};
}

std::vector<std::string> scalars(const YAML::Node& list) {
  std::vector<std::string> out;
  for (const auto& item : list) out.push_back(item.Scalar());
  return out;
}

}  // namespace

TEST_CASE("explain's JSON lists the chain's nodes in the engine's order with their capacities",
          "[stream]") {
  const auto doc = graph_json_of("tutorial/chain/scenario.yaml");
  CHECK(doc["format_version"].as<int>() == 1);
  CHECK(doc["scenario"].Scalar() == "chain");
  REQUIRE(doc["nodes"].size() == 3);
  CHECK(doc["nodes"][0]["id"].Scalar() == "users");
  CHECK(doc["nodes"][1]["id"].Scalar() == "api");
  CHECK(doc["nodes"][2]["id"].Scalar() == "db");
  const auto users = doc["nodes"][0];
  CHECK(users["type"].Scalar() == "client");
  CHECK(users["capacity_per_s"].IsNull());
  CHECK(users["unit"].IsNull());
  const auto api = doc["nodes"][1];
  CHECK(api["type"].Scalar() == "service");
  CHECK(api["datacenter"].Scalar() == "main");
  CHECK(api["replicas"].as<int>() == 2);
  CHECK(api["capacity_per_s"].as<double>() == 2000);  // 2 replicas x 1k/s
  CHECK(api["unit"].Scalar() == "requests");
  CHECK(api["yellow"].as<double>() == 0.7);  // the defaults of every queue (spec §3.3)
  CHECK(api["red"].as<double>() == 0.9);
  CHECK(api["queue_limit"].as<double>() == 1000);             // the default
  CHECK(doc["nodes"][2]["queue_limit"].as<double>() == 500);  // the scenario's
  REQUIRE(doc["datacenters"].size() == 1);
  CHECK(doc["datacenters"][0]["name"].Scalar() == "main");
  CHECK(api["shards"].IsNull());
}

TEST_CASE("explain's JSON gives each link its direction, one-way latency and bandwidth",
          "[stream]") {
  const auto doc = graph_json_of("tutorial/chain/scenario.yaml");
  REQUIRE(doc["links"].size() == 2);
  const auto link = link_between(doc["links"], "users", "api");
  CHECK(link["direction"].Scalar() == "inout");
  CHECK(link["one_way_s"].as<double>() == 0.00025);  // intra_dc_rtt / 2 = 0.5 ms / 2
  CHECK(link["bandwidth_mb_s"].IsNull());
  CHECK(link["ops"].size() == 0);  // every op
}

TEST_CASE("explain's JSON lists each client op with its share, in the frames' order", "[stream]") {
  const auto doc = graph_json_of("tutorial/chain/scenario.yaml");
  REQUIRE(doc["ops"].size() == 2);
  CHECK(doc["ops"][0]["client"].Scalar() == "users");
  CHECK(doc["ops"][0]["op"].Scalar() == "read");
  CHECK(doc["ops"][0]["share"].as<double>() == 0.8);
  CHECK(doc["ops"][1]["op"].Scalar() == "write");
  CHECK(doc["ops"][1]["share"].as<double>() == 0.2);
}

TEST_CASE("explain's JSON says which nodes count megabytes, capacity units or replica-requests",
          "[stream]") {
  const auto youtube = graph_json_of("book/youtube/scenario.yaml");
  CHECK(entry(youtube["nodes"], "id", "cdn_us")["unit"].Scalar() == "MB");
  CHECK(entry(youtube["nodes"], "id", "api")["unit"].Scalar() == "requests");
  // 40 Gbit/s / 8 = 5 GB/s = 5000 MB/s (explain's check line: 5k MB/s)
  CHECK(
      link_between(youtube["links"], "cdn_eu", "transcoded_store")["bandwidth_mb_s"].as<double>() ==
      5000);
  CHECK(entry(youtube["nodes"], "id", "transcoders")["workers"].as<int>() == 600);
  const auto shortener = graph_json_of("book/url-shortener/scenario.yaml");
  CHECK(entry(shortener["nodes"], "id", "url_db")["unit"].Scalar() == "units");  // writes cost 5
  const auto kv = graph_json_of("labs/kv-store/scenario.yaml");
  CHECK(entry(kv["nodes"], "id", "store")["unit"].Scalar() == "replica-requests");
}

TEST_CASE("explain's JSON gives a websocket gateway its connection cap", "[stream]") {
  const auto doc = graph_json_of("book/chat-system/scenario.yaml");
  // max_connections 100k per replica x 20 replicas
  CHECK(entry(doc["nodes"], "id", "chat_servers")["connection_cap"].as<double>() == 2'000'000);
}

TEST_CASE("explain's JSON lists the scripted faults and the SLOs, so a timeline can mark them",
          "[stream]") {
  const auto doc = graph_json_of("book/news-feed/scenario.yaml");
  REQUIRE(doc["faults"].size() == 1);
  const auto fault = doc["faults"][0];
  CHECK(fault["kind"].Scalar() == "node_down");
  CHECK(fault["target"].Scalar() == "post_db");
  CHECK(fault["at_s"].as<double>() == 54000);  // 15:00
  CHECK(fault["for_s"].as<double>() == 30);
  CHECK(doc["slos"].size() == 3);
  CHECK(doc["slos"][0]["metric"].IsScalar());
}

TEST_CASE("explain's JSON names the columns of every frame row: the stream's contract",
          "[stream]") {
  const auto doc = graph_json_of("tutorial/hello/scenario.yaml");
  CHECK(scalars(doc["node_columns"]) ==
        std::vector<std::string>{"demand_per_s", "drain_per_s", "dropped_per_s", "rejected_per_s",
                                 "backlog", "peak_backlog", "mean_rho", "peak_rho", "zone", "red_s",
                                 "latency_ms", "queue_lag_s", "connections", "dead_lettered_per_s",
                                 "deduplicated_per_s"});
  CHECK(scalars(doc["link_columns"]) ==
        std::vector<std::string>{"demand_per_s", "drain_per_s", "dropped_per_s", "backlog",
                                 "peak_backlog", "mean_rho", "peak_rho", "zone", "red_s",
                                 "latency_ms"});
  CHECK(scalars(doc["op_columns"]) == std::vector<std::string>{"offered_per_s", "attempts_per_s",
                                                               "answered_per_s", "timed_out_per_s",
                                                               "mean_ms", "p99_ms"});
  CHECK(scalars(doc["estimated_columns"]) == std::vector<std::string>{"p99_ms"});
  CHECK(scalars(doc["zones"]) == std::vector<std::string>{"green", "yellow", "red"});
}

TEST_CASE("every shipped scenario's graph JSON parses, one node per node of the engine",
          "[stream]") {
  for (const auto* relative :
       {"tutorial/hello/scenario.yaml", "tutorial/chain/scenario.yaml",
        "labs/rate-limiter/scenario.yaml", "labs/consistent-hashing/scenario.yaml",
        "labs/unique-id/scenario.yaml", "labs/kv-store/scenario.yaml",
        "book/url-shortener/scenario.yaml", "book/youtube/scenario.yaml",
        "book/news-feed/scenario.yaml", "book/search-autocomplete/scenario.yaml",
        "book/notification-system/scenario.yaml", "book/web-crawler/scenario.yaml",
        "book/google-drive/scenario.yaml", "book/chat-system/scenario.yaml"}) {
    INFO(relative);
    const auto g = graph_of(shipped(relative));
    const auto doc = YAML::Load(simsys::report::render_graph_json(g));
    CHECK(doc["nodes"].size() == g.nodes().size());
    std::size_t piped = 0;
    for (const auto& link : g.links()) piped += link.pipe ? 1U : 0U;
    std::size_t with_bandwidth = 0;
    for (const auto& link : doc["links"]) {
      with_bandwidth += link["bandwidth_mb_s"].IsNull() ? 0U : 1U;
    }
    CHECK(with_bandwidth == piped);
  }
}

// ---- the frames: node buckets (Task 3)
// -----------------------------------------------------------
//
// The aggregator only reads StepRecords, so its rules are pinned on records written by hand for the
// M/M/1 probe's graph (node 0 the client `c`, node 1 the service `s`; steps of 100 ms).

namespace {

const simsys::graph::Graph& probe_graph() {
  static const auto g = *simsys::graph::Graph::build(simsys::test_support::mm1(500));
  return g;
}

// A record at step k (t = k x 100 ms) whose service sample is `s`; the client's sample is empty.
StepRecord record_at(int k, const NodeSample& s) {
  return StepRecord{.t = k * 100ms, .nodes = {NodeSample{}, s}};
}

NodeSample serving(double rho, double drain = 0.0, double latency_s = 0.001) {
  NodeSample sample{};
  sample.drain_per_s = drain;
  sample.rho = rho;
  sample.latency.mean_s = latency_s;
  return sample;
}

std::vector<Frame> frames_of(std::span<const StepRecord> records, std::size_t steps_per_bucket,
                             const simsys::graph::Graph& g = probe_graph()) {
  std::vector<Frame> frames;
  FrameAggregator aggregator{g, steps_per_bucket, [&](const Frame& f) { frames.push_back(f); }};
  for (const auto& record : records) aggregator.observe(record);
  aggregator.finish();
  return frames;
}

}  // namespace

TEST_CASE("a bucket's utilization is the mean of its steps' and its peak the largest", "[stream]") {
  // Ten steps at rho 0.25, then ten at 0.75 (a client stepping from 500/s to 1.5k/s into 2k/s):
  // mean (10 x 0.25 + 10 x 0.75) / 20 = 0.5, peak 0.75.
  std::vector<StepRecord> records;
  for (int k = 0; k < 20; ++k) records.push_back(record_at(k, serving(k < 10 ? 0.25 : 0.75)));
  const auto frames = frames_of(records, 20);
  REQUIRE(frames.size() == 1);
  CHECK_THAT(*frames[0].nodes[1].mean_rho, WithinRel(0.5, 1e-12));
  CHECK(*frames[0].nodes[1].peak_rho == 0.75);
  CHECK_THAT(frames[0].dt_s, WithinRel(2.0, 1e-12));
}

TEST_CASE("one red step makes its bucket red, with that step's seconds in red", "[stream]") {
  std::vector<StepRecord> records;
  for (int k = 0; k < 10; ++k) {
    auto sample = serving(k == 4 ? 0.95 : 0.3);
    if (k == 4) sample.zone = simsys::fluid::Zone::red;
    records.push_back(record_at(k, sample));
  }
  const auto frames = frames_of(records, 10);
  REQUIRE(frames.size() == 1);
  CHECK(frames[0].nodes[1].zone == 2);
  CHECK_THAT(*frames[0].nodes[1].red_s, WithinRel(0.1, 1e-12));
  // A bucket that never left green: zone 0, no red time.
  const auto calm = frames_of(std::span{records}.first(4), 4);
  CHECK(calm[0].nodes[1].zone == 0);
  CHECK(*calm[0].nodes[1].red_s == 0.0);
}

TEST_CASE("a bucket's worst zone is yellow when no step was red", "[stream]") {
  std::vector<StepRecord> records;
  for (int k = 0; k < 3; ++k) {
    auto sample = serving(0.8);
    sample.zone = k == 1 ? simsys::fluid::Zone::yellow : simsys::fluid::Zone::green;
    records.push_back(record_at(k, sample));
  }
  CHECK(frames_of(records, 3)[0].nodes[1].zone == 1);
}

TEST_CASE("a level is the bucket's last value: backlog and open connections", "[stream]") {
  std::vector<StepRecord> records;
  for (int k = 0; k < 5; ++k) {
    auto sample = serving(0.5);
    sample.backlog = k == 2 ? 80.0 : 10.0 * k;  // 0, 10, 80, 30, 40
    sample.connections = 1000.0 + k;
    records.push_back(record_at(k, sample));
  }
  const auto row = frames_of(records, 5)[0].nodes[1];
  CHECK(*row.backlog == 40.0);
  CHECK(*row.peak_backlog == 80.0);
  CHECK(*row.connections == 1004.0);
}

TEST_CASE("a bucket's latency is the drain-weighted mean of its steps' sojourns", "[stream]") {
  // Drains 1 and 3 with sojourns 2 ms and 4 ms: (1 x 2 + 3 x 4) / 4 = 3.5 ms.
  const std::vector<StepRecord> records{record_at(0, serving(0.1, 1.0, 0.002)),
                                        record_at(1, serving(0.1, 3.0, 0.004))};
  CHECK_THAT(*frames_of(records, 2)[0].nodes[1].latency_ms, WithinRel(3.5, 1e-12));
}

TEST_CASE("a bucket whose node never answered in some step has no latency", "[stream]") {
  const std::vector<StepRecord> records{
      record_at(0, serving(0.1, 1.0, 0.002)),
      record_at(1, serving(1.0, 0.0, std::numeric_limits<double>::infinity()))};
  CHECK_FALSE(frames_of(records, 2)[0].nodes[1].latency_ms.has_value());
}

TEST_CASE("a bucket that served nothing reports the time mean of its sojourns", "[stream]") {
  const std::vector<StepRecord> records{record_at(0, serving(0.0, 0.0, 0.001)),
                                        record_at(1, serving(0.0, 0.0, 0.003))};
  CHECK_THAT(*frames_of(records, 2)[0].nodes[1].latency_ms, WithinRel(2.0, 1e-12));
}

TEST_CASE("columns a node does not have are null in its row, and a client has no queue",
          "[stream]") {
  std::vector<StepRecord> records{record_at(0, serving(0.5))};
  const auto frame = frames_of(records, 1)[0];
  const auto& client = frame.nodes[0];
  CHECK_FALSE(client.demand_per_s.has_value());
  CHECK_FALSE(client.mean_rho.has_value());
  CHECK_FALSE(client.zone.has_value());
  CHECK_FALSE(client.latency_ms.has_value());
  const auto& service = frame.nodes[1];
  CHECK(service.demand_per_s.has_value());
  CHECK_FALSE(service.queue_lag_s.has_value());
  CHECK_FALSE(service.connections.has_value());
  CHECK_FALSE(service.dead_lettered_per_s.has_value());
  CHECK_FALSE(service.deduplicated_per_s.has_value());
}

TEST_CASE("600 steps in buckets of 7 make 85 full frames and a last one of 5 steps", "[stream]") {
  std::vector<StepRecord> records;
  for (int k = 0; k < 600; ++k) records.push_back(record_at(k, serving(0.5)));
  const auto frames = frames_of(records, 7);
  REQUIRE(frames.size() == 86);  // 600 = 85 x 7 + 5
  CHECK_THAT(frames[0].dt_s, WithinRel(0.7, 1e-12));
  CHECK_THAT(frames.back().dt_s, WithinRel(0.5, 1e-12));
  CHECK(simsys::seconds(frames[3].t) ==
        simsys::seconds(21 * 100ms));  // frame k starts at k x 0.7 s
  CHECK(frames.back().t == 595 * 100ms);
}

TEST_CASE("finish hands over nothing when the last bucket was full, or a second time", "[stream]") {
  std::vector<Frame> frames;
  FrameAggregator aggregator{probe_graph(), 2, [&](const Frame& f) { frames.push_back(f); }};
  for (int k = 0; k < 4; ++k) aggregator.observe(record_at(k, serving(0.5)));
  CHECK(frames.size() == 2);
  aggregator.finish();
  CHECK(frames.size() == 2);
  aggregator.observe(record_at(4, serving(0.5)));
  aggregator.finish();
  aggregator.finish();
  CHECK(frames.size() == 3);
}

TEST_CASE("a bucket of no step size is refused: a programmer error", "[stream]") {
  CHECK_THROWS_AS((FrameAggregator{probe_graph(), 0, [](const Frame&) {}}), std::invalid_argument);
}

TEST_CASE("explain's JSON gives a sharded database its shards beside its copies", "[stream]") {
  const auto doc = graph_json_of("book/url-shortener/scenario.yaml");
  const auto db = entry(doc["nodes"], "id", "url_db");
  CHECK(db["shards"].as<int>() == 3);
  CHECK(db["replicas"].as<int>() == 3);                // a primary and two read replicas per shard
  CHECK(db["capacity_per_s"].as<double>() == 45'000);  // 3 copies x 3 shards x 5k/s
}

TEST_CASE("explain's JSON gives an instant fault no duration", "[stream]") {
  const auto path = scratch_scenario("flush", R"(version: 1
name: flush
duration: 1m
nodes:
  users: {type: client, rate: {base: 100/s}, ops: {get: 1}, keys: {zipf: 0.8, space: 1M}}
  c: {type: cache, strategy: read_through, backend: origin, keys: 10k, hit_ratio: che}
  origin: {type: service, capacity: 100k/s, latency: 1ms}
links:
  - {from: users, to: c, dir: inout}
  - {from: c, to: origin, dir: inout}
faults: [{at: 20s, kind: cache_flush, node: c}]
)");
  const auto doc = YAML::Load(simsys::report::render_graph_json(graph_of(path)));
  REQUIRE(doc["faults"].size() == 1);
  CHECK(doc["faults"][0]["kind"].Scalar() == "cache_flush");
  CHECK(doc["faults"][0]["for_s"].IsNull());
}

TEST_CASE("explain's JSON lists a client's ops by name whatever order the scenario gave",
          "[stream]") {
  auto s = simsys::test_support::mm1(100);
  s.nodes[0].workload->op_shares = {{"write", 0.25}, {"read", 0.75}};
  const auto g = *simsys::graph::Graph::build(s);
  const auto doc = YAML::Load(simsys::report::render_graph_json(g));
  REQUIRE(doc["ops"].size() == 2);
  CHECK(doc["ops"][0]["op"].Scalar() == "read");
  CHECK(doc["ops"][1]["op"].Scalar() == "write");
}

// ---- the frames: op rows (Task 4)
// ----------------------------------------------------------------

namespace {

simsys::engine::ResponseSample answered_at(double answered_per_s, double mean_s, double variance_s2,
                                           double timed_out_per_s = 0.0) {
  simsys::engine::ResponseSample sample{};
  sample.client = 0;
  sample.op = "op";
  sample.offered = simsys::Rate{answered_per_s + timed_out_per_s};
  sample.attempts = sample.offered;
  sample.answered = simsys::Rate{answered_per_s};
  sample.timed_out = simsys::Rate{timed_out_per_s};
  sample.timed_out_attempts = simsys::Rate{timed_out_per_s};
  sample.response = {.mean_s = mean_s, .variance_s2 = variance_s2};
  sample.delivered_response = {.mean_s = mean_s, .variance_s2 = variance_s2};
  return sample;
}

StepRecord responding(int k, const simsys::engine::ResponseSample& sample) {
  auto record = record_at(k, serving(0.5));
  record.responses = {sample};
  return record;
}

}  // namespace

TEST_CASE("a bucket's op rates are the means of its steps'", "[stream]") {
  // 100/s then 300/s answered: a mean of 200/s.
  const std::vector<StepRecord> records{responding(0, answered_at(100, 0.002, 1e-6)),
                                        responding(1, answered_at(300, 0.002, 1e-6))};
  const auto frames = frames_of(records, 2);
  REQUIRE(frames[0].ops.size() == 1);
  const auto& op = frames[0].ops[0];
  CHECK_THAT(*op.offered_per_s, WithinRel(200.0, 1e-12));
  CHECK_THAT(*op.attempts_per_s, WithinRel(200.0, 1e-12));
  CHECK_THAT(*op.answered_per_s, WithinRel(200.0, 1e-12));
  CHECK(*op.timed_out_per_s == 0.0);
}

TEST_CASE("a bucket's op mean is weighted by the requests each step answered", "[stream]") {
  // 100/s at 2 ms and 300/s at 4 ms: (100 x 2 + 300 x 4) / 400 = 3.5 ms.
  const std::vector<StepRecord> records{responding(0, answered_at(100, 0.002, 1e-6)),
                                        responding(1, answered_at(300, 0.004, 1e-6))};
  CHECK_THAT(*frames_of(records, 2)[0].ops[0].mean_ms, WithinRel(3.5, 1e-12));
}

TEST_CASE("a bucket of identical steps has the p99 of one step's lognormal fit", "[stream]") {
  // Mean 2 ms, variance 4e-6 s² (a coefficient of variation of 1): the lognormal of those moments
  // has sigma² = ln(1 + 1) and mu = ln(0.002) − sigma²/2, so p99 = exp(mu + 2.3263 sigma)
  // = 0.002 / sqrt(2) × exp(2.3263 × sqrt(ln 2)) = 9.106 ms.
  std::vector<StepRecord> records;
  for (int k = 0; k < 10; ++k) records.push_back(responding(k, answered_at(100, 0.002, 4e-6)));
  const double sigma = std::sqrt(std::log(2.0));
  const double expected_ms = 2.0 / std::sqrt(2.0) * std::exp(2.326347874 * sigma);
  CHECK_THAT(*frames_of(records, 10)[0].ops[0].p99_ms, WithinRel(expected_ms, 1e-6));
  CHECK_THAT(expected_ms, WithinRel(9.106, 1e-3));
}

TEST_CASE("a bucket where more than 1 % timed out has the timeout as its p99", "[stream]") {
  // A client with a 5 ms timeout: 2 of 100 delivered requests per second came back after it, more
  // than the 1 % a p99 allows, so no answered request sits at the 99th percentile: the client saw
  // its timeout there (the run's rule, percentile_seen_by_client).
  auto s = simsys::test_support::mm1(100);
  s.nodes[0].workload->timeout = 5ms;
  const auto g = *simsys::graph::Graph::build(s);
  const std::vector<StepRecord> records{responding(0, answered_at(98, 0.002, 4e-6, 2))};
  const auto op = frames_of(records, 1, g)[0].ops[0];
  CHECK_THAT(*op.p99_ms, WithinRel(5.0, 1e-12));
  CHECK_THAT(*op.timed_out_per_s, WithinRel(2.0, 1e-12));
}

TEST_CASE("a bucket in which nothing was answered has no mean and no p99", "[stream]") {
  const std::vector<StepRecord> records{responding(0, answered_at(0, 0.002, 4e-6)),
                                        responding(1, answered_at(0, 0.002, 4e-6))};
  const auto op = frames_of(records, 2)[0].ops[0];
  CHECK(*op.answered_per_s == 0.0);
  CHECK_FALSE(op.mean_ms.has_value());
  CHECK_FALSE(op.p99_ms.has_value());
}
