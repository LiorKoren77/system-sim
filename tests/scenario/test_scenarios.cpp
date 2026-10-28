// Every scenario shipped under scenarios/ is an integration test of the whole pipeline:
// load -> graph -> explain -> (phase 1 only) run. The directory is injected by
// CMake as SIMSYS_SCENARIOS_DIR: a compile definition is the one macro we accept, because
// it carries a build-time path that no language feature can.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion. It comes after the #includes because GCC 14 reports redefinitions
// when a std header is included after importing a module that included it in its
// global module fragment.
import simsys.config;
import simsys.core;
import simsys.engine;
import simsys.graph;
import simsys.model;
import simsys.report;

namespace {

// Every scenario.yaml under SIMSYS_SCENARIOS_DIR, sorted so failures are reproducible
// and reports list the files in the same order on every machine.
std::vector<std::string> scenario_files() {
  namespace fs = std::filesystem;
  std::vector<std::string> files;
  for (const auto& entry : fs::recursive_directory_iterator(SIMSYS_SCENARIOS_DIR)) {
    if (entry.path().filename() == "scenario.yaml") files.push_back(entry.path().string());
  }
  // C++20 std::ranges::sort: takes the container itself instead of a begin/end pair,
  // and is constrained by concepts, so a non-sortable range fails with a short error.
  std::ranges::sort(files);
  return files;
}

}  // namespace

TEST_CASE("every shipped scenario validates", "[scenario]") {
  const auto files = scenario_files();
  for (const auto& file : files) {
    DYNAMIC_SECTION(std::filesystem::path{file}.lexically_relative(SIMSYS_SCENARIOS_DIR).string()) {
      auto scenario = simsys::config::load(file);
      if (!scenario) {
        const auto& e = scenario.error();
        FAIL(e.file << ":" << e.line << ":" << e.column << ": " << e.message);
      }
      CHECK(scenario->nodes.size() >= 2);
      CHECK(!scenario->links.empty());
    }
  }
  CHECK(!files.empty());
}

TEST_CASE("every shipped scenario resolves into a graph that explain can print", "[scenario]") {
  for (const auto& file : scenario_files()) {
    CAPTURE(file);
    const auto s = simsys::config::load(file);
    REQUIRE(s.has_value());
    const auto g = simsys::graph::Graph::build(*s);
    if (!g) FAIL(g.error().message);
    REQUIRE_FALSE(simsys::report::render_explain(*g).empty());
  }
}

// Which scenarios run grows with each phase, so the test does not name them: a scenario whose
// node types the engine does not simulate yet is refused by Simulation::create with a message
// naming the phase ("... do not support yet (phase 2)") and is skipped; every other one is run
// to its end and must meet its own SLOs. A scenario the engine refuses for any other reason is
// a failure, not a skip.
TEST_CASE("scenarios whose node types all exist run in under five seconds and pass their SLOs",
          "[scenario]") {
  using namespace std::chrono_literals;
  int ran = 0;
  for (const auto& file : scenario_files()) {
    CAPTURE(file);
    const auto loaded = simsys::config::load(file);
    if (!loaded) FAIL(loaded.error().message);  // a broken file must fail here, not crash below
    const auto& s = *loaded;
    const auto g = *simsys::graph::Graph::build(s);
    auto created = simsys::engine::Simulation::create(g);
    if (!created) {
      REQUIRE(created.error().message.find("phase 2") != std::string::npos);
      continue;  // a node type of a later phase: nothing to run yet
    }
    ++ran;
    auto& sim = *created;
    simsys::report::Summary summary{g, s.warmup};
    const auto started = std::chrono::steady_clock::now();
    // C++20 designated initializers: the run options name the fields they set, so the
    // call says which duration is which; replaces a positional {s.duration, s.step}.
    sim.run({.duration = s.duration, .step = s.step}, [&](const auto& r) { summary.observe(r); });
    REQUIRE(std::chrono::steady_clock::now() - started < 5s);
    const auto results = simsys::report::evaluate_slos(summary, s.slos);
    for (const auto& r : results) {
      // No to_string(SloMetric) exists yet, so the metric is captured as its enum value
      // (0..2 = p50/p95/p99 latency, 3 = drop_rate); latencies are in seconds.
      CAPTURE(static_cast<int>(r.slo.metric), r.observed.value_or(-1.0));
      REQUIRE(r.status != simsys::report::SloStatus::fail);
    }
  }
  CHECK(ran > 0);  // the tutorials at least always run
}

// Phase 2a's goal: the rate-limiter lab, which uses a client with spikes, a rate limiter, a
// cache and a service, runs end to end. Its `counters` cache (hit_ratio: {fixed: 1.0}) lands
// in plan Task 10; until then Simulation::create refuses the cache and THIS CASE STAYS RED on
// purpose. It goes green when the cache node exists: do not weaken it to a skip.
TEST_CASE("labs/rate-limiter runs in phase 2a", "[scenario]") {
  using namespace std::chrono_literals;
  const auto s =
      simsys::config::load(std::string{SIMSYS_SCENARIOS_DIR} + "/labs/rate-limiter/scenario.yaml");
  if (!s) FAIL(s.error().file << ":" << s.error().line << ": " << s.error().message);
  const auto g = simsys::graph::Graph::build(*s);
  if (!g) FAIL(g.error().message);
  auto sim = simsys::engine::Simulation::create(*g);
  if (!sim) FAIL(sim.error().message);
  // A short run is enough to prove the pipeline end to end: the first steps of 30 minutes.
  int steps = 0;
  sim->run({.duration = 1s, .step = s->step}, [&](const auto&) { ++steps; });
  REQUIRE(steps == 10);
}

// Phase 2b, Task 4: the consistent-hashing lab (clients with Zipf keys -> a hash ring -> four che
// caches -> a SQL database with a primary and a read replica) runs end to end and meets its own
// SLOs (cache utilization <= 0.85, database utilization <= 0.7). If an SLO fails the CAPTUREd
// numbers say by how much, so the implementer can judge whether the model or the lab's numbers
// are off; the test must not be weakened to hide it.
TEST_CASE("labs/consistent-hashing runs and passes its SLOs", "[scenario]") {
  const auto s = simsys::config::load(std::string{SIMSYS_SCENARIOS_DIR} +
                                      "/labs/consistent-hashing/scenario.yaml");
  if (!s) FAIL(s.error().file << ":" << s.error().line << ": " << s.error().message);
  const auto g = simsys::graph::Graph::build(*s);
  if (!g) FAIL(g.error().message);
  auto sim = simsys::engine::Simulation::create(*g);
  if (!sim) FAIL(sim.error().message);
  simsys::report::Summary summary{*g, s->warmup};
  sim->run({.duration = s->duration, .step = s->step}, [&](const auto& r) { summary.observe(r); });
  const auto results = simsys::report::evaluate_slos(summary, s->slos);
  REQUIRE(results.size() == s->slos.size());
  for (const auto& r : results) {
    CAPTURE(r.slo.node, static_cast<int>(r.slo.metric), r.observed.value_or(-1.0));
    REQUIRE(r.status != simsys::report::SloStatus::fail);
  }
}

TEST_CASE("book scenarios are refused by run until their node types exist", "[scenario]") {
  // Phase 1 runs only client and service nodes; every book scenario uses more. The message
  // must name the node and the phase so a student knows what is missing, not that the file is
  // wrong.
  const auto s = *simsys::config::load(std::string{SIMSYS_SCENARIOS_DIR} +
                                       "/book/url-shortener/scenario.yaml");
  const auto g = *simsys::graph::Graph::build(s);
  const auto sim = simsys::engine::Simulation::create(g);
  REQUIRE_FALSE(sim.has_value());
  REQUIRE(sim.error().message.find("phase 2") != std::string::npos);
}

// Phase 2b runs the nodes up to the SQL database: the two tutorials, the rate-limiter lab and
// the consistent-hashing lab (client, load balancer, che cache, sql_db) use only those. Every
// other lab and every book scenario still needs a later task's node types, so the set of
// runnable scenarios is pinned by name: a node type added later makes this fail and forces
// whoever adds it to update the list on purpose.
TEST_CASE(
    "labs and tutorials that run under phase 2b are: chain, hello, consistent-hashing, "
    "rate-limiter",
    "[scenario]") {
  std::vector<std::string> runnable;
  for (const auto& file : scenario_files()) {
    CAPTURE(file);
    const auto loaded = simsys::config::load(file);
    if (!loaded) FAIL(loaded.error().message);  // a broken file must fail here, not crash below
    const auto& s = *loaded;
    const auto g = *simsys::graph::Graph::build(s);
    if (simsys::engine::Simulation::create(g)) runnable.push_back(s.name);
  }
  std::ranges::sort(runnable);
  REQUIRE(runnable ==
          std::vector<std::string>{"chain", "hello", "lab-consistent-hashing", "lab-rate-limiter"});
}

TEST_CASE("a refused scenario's message names the first unsupported node and the phase",
          "[scenario]") {
  const auto s = *simsys::config::load(std::string{SIMSYS_SCENARIOS_DIR} +
                                       "/book/url-shortener/scenario.yaml");
  const auto g = *simsys::graph::Graph::build(s);
  const auto sim = simsys::engine::Simulation::create(g);
  REQUIRE_FALSE(sim.has_value());
  const auto& message = sim.error().message;
  CAPTURE(message);
  REQUIRE(message.find("phase 2") != std::string::npos);
  // Name some node of the scenario: the engine reports the first unsupported one it meets.
  const auto names_a_node = std::ranges::any_of(
      s.nodes, [&](const auto& node) { return message.find(node.id) != std::string::npos; });
  REQUIRE(names_a_node);
}

// ---- Request-based CDN (plan 2b refinement 4, Task 5) ----

namespace {
// The peak offered load explain's check line gives node `id`, in requests per second: the number
// after "peak offered " in "  media_store  peak offered 324.3/s vs ...", with its k/M suffix
// applied. explain prints a few significant digits, so a caller compares with a tolerance.
double peak_offered_per_s(const std::string& explain_text, const std::string& id) {
  const auto checks = explain_text.find("checks (");
  REQUIRE(checks != std::string::npos);
  const auto line = explain_text.find("\n  " + id + " ", checks);
  REQUIRE(line != std::string::npos);
  const auto key = std::string{"peak offered "};
  const auto number_at = explain_text.find(key, line) + key.size();
  std::size_t used = 0;
  double value = std::stod(explain_text.substr(number_at), &used);
  const char suffix = explain_text[number_at + used];
  if (suffix == 'k') value *= 1e3;
  if (suffix == 'M') value *= 1e6;
  return value;
}
}  // namespace

// Ch. 11's news feed serves media through a CDN with a fixed hit ratio of 0.95 in front of the
// media store. The paper load must follow the miss edge cdn -> media_store: 1 - 0.95 = 5 % of the
// reads that reach the CDN, where it used to be nothing (the edge did not exist).
TEST_CASE("book/news-feed's media store is checked against the cdn's 5 percent miss share",
          "[scenario]") {
  const auto s =
      simsys::config::load(std::string{SIMSYS_SCENARIOS_DIR} + "/book/news-feed/scenario.yaml");
  if (!s) FAIL(s.error().file << ":" << s.error().line << ": " << s.error().message);
  const auto g = simsys::graph::Graph::build(*s);
  if (!g) FAIL(g.error().message);
  const auto text = simsys::report::render_explain(*g);
  CAPTURE(text);
  const double at_cdn = peak_offered_per_s(text, "cdn");
  const double at_origin = peak_offered_per_s(text, "media_store");
  REQUIRE(at_cdn > 0.0);
  // explain prints four significant digits ("6.486k/s"), so the ratio holds to 1e-3.
  REQUIRE_THAT(at_origin, Catch::Matchers::WithinRel(0.05 * at_cdn, 1e-3));
}

// Ch. 14's YouTube sizes its two CDNs in bytes (`capacity: 100GB/s`, `50GB/s`) and gives them a
// che hit ratio: valid scenario, explained, but not runnable before phase 2c's byte flows.
// The engine's FIRST refusal is the node types it does not simulate at all (blob_store, queues
// and workers: "... (phase 2)"), which comes before the cdn's own byte-capacity refusal
// (see the theory test for that message), so only "phase 2" is asserted here.
TEST_CASE("book/youtube validates and explains, and the engine does not run it yet", "[scenario]") {
  const auto s =
      simsys::config::load(std::string{SIMSYS_SCENARIOS_DIR} + "/book/youtube/scenario.yaml");
  if (!s) FAIL(s.error().file << ":" << s.error().line << ": " << s.error().message);
  // C++20 ranges algorithm with a projection: find the node whose `id` member equals "cdn_us",
  // replacing `std::find_if(begin, end, [](const auto& n) { return n.id == "cdn_us"; })`.
  const auto cdn_us = std::ranges::find(s->nodes, "cdn_us", &simsys::model::NodeSpec::id);
  REQUIRE(cdn_us != s->nodes.end());
  REQUIRE(cdn_us->origin == "transcoded_store");
  REQUIRE(cdn_us->byte_capacity == simsys::ByteRate{100e9});
  REQUIRE(cdn_us->hit_ratio.has_value());
  REQUIRE(cdn_us->hit_ratio->che);
  const auto g = simsys::graph::Graph::build(*s);
  if (!g) FAIL(g.error().message);
  REQUIRE_FALSE(simsys::report::render_explain(*g).empty());
  const auto sim = simsys::engine::Simulation::create(*g);
  REQUIRE_FALSE(sim.has_value());
  REQUIRE(sim.error().message.find("phase 2") != std::string::npos);
}
