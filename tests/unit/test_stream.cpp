// The output contract a frontend reads (phase 5a, spec 2026-11-10-phase5a-streaming-design.md):
// the format_version every JSON document carries, the report on one line, the resolved graph as
// JSON, the frames of `run --stream` and the stream's lines. Lines of JSON are parsed with
// yaml-cpp: JSON is YAML 1.2's flow syntax, so a JSON object loads as a YAML map and a test
// compares trees, not text.
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>  // getpid

#include <yaml-cpp/yaml.h>

// C++20 `import`: the compiled interfaces of the modules under test, after every #include (the
// eighth GCC 14.2 defect: a std header included after an import clashes with the module's).
import simsys.config;
import simsys.graph;
import simsys.report;
import simsys.session;

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
  // The fault block's texts and the SLO notes carry spaces; only the layout's whitespace goes.
  const auto reports = reports_of(scratch_scenario("probe", probe_yaml));
  const auto pretty = YAML::Load(reports.pretty);
  const auto line = YAML::Load(reports.line);
  CHECK(line["faults"][0]["kind"].Scalar() == pretty["faults"][0]["kind"].Scalar());
  CHECK(same_tree(line["slo"], pretty["slo"]));
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
  CHECK(scalars(doc["datacenters"]) == std::vector<std::string>{"main"});
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
