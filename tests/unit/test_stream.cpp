// The output contract a frontend reads (phase 5a, spec 2026-11-10-phase5a-streaming-design.md):
// the format_version every JSON document carries, the report on one line, the resolved graph as
// JSON, the frames of `run --stream` and the stream's lines. Lines of JSON are parsed with yaml-cpp:
// JSON is YAML 1.2's flow syntax, so a JSON object loads as a YAML map and a test compares trees,
// not text.
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
  const auto dir = std::filesystem::temp_directory_path() /
                   ("simsys_stream_" + std::to_string(::getpid()));
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
  for (const auto& path : {shipped("tutorial/chain/scenario.yaml"),
                           shipped("labs/rate-limiter/scenario.yaml"),
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
