// Every scenario shipped under scenarios/ is an integration test of the whole pipeline:
// load -> graph -> explain -> (phase 1 only) run. The directory is injected by
// CMake as SIMSYS_SCENARIOS_DIR: a compile definition is the one macro we accept, because
// it carries a build-time path that no language feature can.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../support/explain_lines.hpp"

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion. It comes after the #includes because GCC 14 reports redefinitions
// when a std header is included after importing a module that included it in its
// global module fragment.
import simsys.config;
import simsys.core;
import simsys.defaults;
import simsys.engine;
import simsys.graph;
import simsys.model;
import simsys.queueing;
import simsys.report;

// explain's lines, shared with test_report and test_series (review of plan 2c Task 7, minor 7).
using simsys::test_support::check_line;
using simsys::test_support::node_line;

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

// The scenario at `path`, or the test fails with the loader's message (file:line: message), never
// a dereference of an empty std::expected.
simsys::model::Scenario load_or_fail(const std::string& path) {
  auto loaded = simsys::config::load(path);
  if (!loaded) {
    FAIL(loaded.error().file << ":" << loaded.error().line << ": " << loaded.error().message);
  }
  return std::move(*loaded);
}

// The graph of `s`, or the test fails with the graph's message.
simsys::graph::Graph build_or_fail(const simsys::model::Scenario& s) {
  auto built = simsys::graph::Graph::build(s);
  if (!built) FAIL(built.error().message);
  return std::move(*built);
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
    const auto g = build_or_fail(s);
    auto created = simsys::engine::Simulation::create(g);
    if (!created) {
      REQUIRE(created.error().message.find("phase 2") != std::string::npos);
      continue;  // a node type of a later phase: nothing to run yet
    }
    // Two runnable scenarios are checked by their own tests below instead, each for a stated
    // reason:
    //  - lab-unique-id ships with the ticket server carrying the load, to FAIL its drop_rate SLO
    //    (8k/s of capacity against 10k/s offered): the lesson. Its own test pins that.
    //  - url-shortener passes its SLOs, but 24 h at 1 s is 86.4k steps: 0.4 s in Release, about
    //    6 to 8 s in a Debug build (measured 2026-10-28), over the five seconds below. Its own test
    //    runs it end to end with no time limit and pins its SLO results.
    //  - youtube runs since plan 2c Task 7 (its pool's `then` steps), but is exempt from its SLOs
    //    until Task 16: its 300 MB uploads take 12 s at the store against the clients' 10 s
    //    timeout (Task 13 applies timeouts, Task 16 sizes the upload's per resumable chunk), its
    //    queue_lag SLO is evaluated since Task 8 and FAILS (the 400 transcoders fall 2.66 h
    //    behind, against its 30 m: the test below pins it) until Task 16 resizes them, and 24 h
    //    at 100 ms is 864k steps, far over the five seconds. The "book/youtube runs" test below
    //    runs its first steps instead.
    //  - news-feed runs since plan 2c Task 10 (its kv_cluster, graph_db, is simulated), but is
    //    exempt from its SLOs until Task 17: 24 h at 100 ms is 864k steps, far over the five
    //    seconds, and its drop_rate and queue_lag SLOs are Task 17's to meet. The "book/news-feed
    //    runs" test below runs its first steps instead.
    //  - search-autocomplete runs since plan 2c Task 11 (its search_index is a service) and MEETS
    //    its SLOs (measured 2026-10-29, Release, full day at 100 ms: p99 of suggest 6.86 ms
    //    against 100 ms, drop rate 0), so Task 18 has nothing to fix; it is exempt here only for
    //    time: 24 h at 100 ms is 862.8k steps, 8.3 s in Release (measured), over the five seconds
    //    below. Its own test below runs the day at a 1 s step (86.4k steps, as the url-shortener's
    //    does) and pins both SLOs passing.
    if (s.name == "lab-unique-id" || s.name == "url-shortener" || s.name == "youtube" ||
        s.name == "news-feed" || s.name == "search-autocomplete") {
      continue;  // not run here
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
  CHECK(ran > 0);  // the tutorials at least always run here (the three above are not counted)
}

// Phase 2a's goal: the rate-limiter lab, which uses a client with spikes, a rate limiter, a
// cache and a service, runs end to end. (It was red on purpose until the cache node landed, plan
// 2a Task 10; do not weaken it to a skip.)
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

// The consistent-hashing lab's caches are read_through: a `set` stays in the cache. Switched to
// write_through, each cache writes every `set` to db as `set`, an op the SQL database's default
// write_ops (insert, update, delete, write) does not name: db would count it as a read, serve it
// from a replica at cost 1 and report its primary idle, "0%, green", where 4k sets/s at cost 5
// are 400 % of the primary's 5k units/s (review of c9d5f8c, finding 1). The graph refuses the
// disagreement and names the list to fix; with `set` listed, explain shows the primary red.
TEST_CASE("the consistent-hashing lab under write_through needs set in db's write_ops",
          "[scenario]") {
  auto s = simsys::config::load(std::string{SIMSYS_SCENARIOS_DIR} +
                                "/labs/consistent-hashing/scenario.yaml");
  if (!s) FAIL(s.error().file << ":" << s.error().line << ": " << s.error().message);
  auto& caches = s->nodes[2];
  auto& db = s->nodes[3];
  REQUIRE(caches.id == "cache_servers");
  REQUIRE(db.id == "db");
  caches.strategy = simsys::model::CacheStrategy::write_through;
  const auto refused = simsys::graph::Graph::build(*s);
  REQUIRE_FALSE(refused.has_value());
  REQUIRE(refused.error().message ==
          "node 'cache_servers': sends write 'set' to 'db', which counts 'set' as a read; add it "
          "to db's write_ops");
  db.write_ops = std::vector<std::string>{"set"};
  const auto built = simsys::graph::Graph::build(*s);
  if (!built) FAIL(built.error().message);
  const auto text = simsys::report::render_explain(*built);
  CAPTURE(text);
  // C++23 std::string::contains: a substring test in one call, replacing
  // `find(...) != std::string::npos`, so the assertion reads as what it checks.
  REQUIRE(text.contains("primary 20k units/s vs 5k/s (400%, red)"));
}

TEST_CASE("book scenarios are refused by run until their node types exist", "[scenario]") {
  // Phase 1 runs only client and service nodes; the book scenarios use more. The message must
  // name the node and the phase so a student knows what is missing, not that the file is wrong.
  // chat-system is the one used here: it needs a websocket gateway, a queue and a worker pool,
  // none of which exist yet. (url-shortener used to stand here; with the id generator, the last
  // node type it lacked, it runs: see the pinned list below.)
  const auto s =
      load_or_fail(std::string{SIMSYS_SCENARIOS_DIR} + "/book/chat-system/scenario.yaml");
  const auto g = build_or_fail(s);
  const auto sim = simsys::engine::Simulation::create(g);
  REQUIRE_FALSE(sim.has_value());
  REQUIRE(sim.error().message.find("phase 2") != std::string::npos);
}

// Phase 2b runs the nodes up to the id generator: the two tutorials, the rate-limiter lab, the
// consistent-hashing lab (client, load balancer, che cache, sql_db), the unique-id lab (client,
// id_generator) and the book's URL shortener (client, rate limiter, caches, load balancer,
// service, id_generator, sql_db) use only those. Phase 2c adds the byte-sized nodes, the queue and
// the pool with its `then` steps (Task 7): youtube (clients, CDNs, blob stores, balancer, service,
// cache, SQL database, message queues, worker pools) is the first book scenario of 2c that runs;
// the kv_cluster (Task 10) adds the kv-store lab and the book's news feed (its graph_db is the one
// node type it lacked); the search index (Task 11, a service with no steps) adds the book's search
// autocomplete. Every other lab and book scenario still needs a later task's node types
// (google-drive's ws_gateway, chat-system's service_discovery, ...), so the set of runnable
// scenarios is pinned by name: a node type added later makes this fail and forces whoever adds it
// to update the list on purpose.
TEST_CASE(
    "scenarios that run today are: chain, hello, consistent-hashing, kv-store, news-feed, "
    "rate-limiter, search-autocomplete, unique-id, url-shortener, youtube",
    "[scenario]") {
  std::vector<std::string> runnable;
  for (const auto& file : scenario_files()) {
    CAPTURE(file);
    const auto loaded = simsys::config::load(file);
    if (!loaded) FAIL(loaded.error().message);  // a broken file must fail here, not crash below
    const auto& s = *loaded;
    const auto g = build_or_fail(s);
    if (simsys::engine::Simulation::create(g)) runnable.push_back(s.name);
  }
  std::ranges::sort(runnable);
  REQUIRE(runnable == std::vector<std::string>{"chain", "hello", "lab-consistent-hashing",
                                               "lab-kv-store", "lab-rate-limiter", "lab-unique-id",
                                               "news-feed", "search-autocomplete", "url-shortener",
                                               "youtube"});
}

TEST_CASE("a refused scenario's message names the first unsupported node and the phase",
          "[scenario]") {
  const auto s =
      load_or_fail(std::string{SIMSYS_SCENARIOS_DIR} + "/book/chat-system/scenario.yaml");
  const auto g = build_or_fail(s);
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

// The request rate in parentheses on the check line of a byte-sized node (plan 2c refinement 2):
// "  media_store  peak offered 64.86 MB/s (324.3/s) vs 1k MB/s ...", where the number before the
// parentheses is megabytes per second. Same k/M suffixes as peak_offered_per_s.
double peak_offered_requests_per_s(const std::string& explain_text, const std::string& id) {
  const auto checks = explain_text.find("checks (");
  REQUIRE(checks != std::string::npos);
  const auto line = explain_text.find("\n  " + id + " ", checks);
  REQUIRE(line != std::string::npos);
  const auto key = explain_text.find("MB/s (", explain_text.find("peak offered ", line));
  REQUIRE(key != std::string::npos);  // a byte-sized node's line: "<n> MB/s (<requests>/s)"
  const auto open = key + 5;
  std::size_t used = 0;
  double value = std::stod(explain_text.substr(open + 1), &used);
  const char suffix = explain_text[open + 1 + used];
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
  // media_store counts megabytes, so its line leads with MB/s and gives the request rate in
  // parentheses: the misses, 5 % of the cdn's reads.
  const double at_origin = peak_offered_requests_per_s(text, "media_store");
  REQUIRE(at_cdn > 0.0);
  // explain prints four significant digits ("6.486k/s"), so the ratio holds to 1e-3.
  REQUIRE_THAT(at_origin, Catch::Matchers::WithinRel(0.05 * at_cdn, 1e-3));
}

// Ch. 14's YouTube sizes its two CDNs in bytes (`capacity: 100GB/s`, `50GB/s`) and gives them a
// che hit ratio: valid scenario, explained, and the engine runs it (plan 2c Task 7: every type it
// uses is simulated, and a worker pool's `then` steps run). What the cdn changed is pinned through
// explain (review of 4d539b0, M12): the two edges' Che hit ratios (Zipf 1 over 10M videos, 100k and
// 50k of them at the edge: 0.642 and 0.594, checked against an independent Python computation by
// the reviewer) and the load they and the transcoders send transcoded_store, which an explain that
// ignored the origin edge or the pool would leave at zero.
TEST_CASE("book/youtube validates, explains and the engine accepts it", "[scenario]") {
  const auto s =
      simsys::config::load(std::string{SIMSYS_SCENARIOS_DIR} + "/book/youtube/scenario.yaml");
  if (!s) FAIL(s.error().file << ":" << s.error().line << ":" << s.error().message);
  // C++20 ranges algorithm with a projection: find the node whose `id` member equals "cdn_us",
  // replacing `std::find_if(begin, end, [](const auto& n) { return n.id == "cdn_us"; })`.
  const auto cdn_us = std::ranges::find(s->nodes, "cdn_us", &simsys::model::NodeSpec::id);
  REQUIRE(cdn_us != s->nodes.end());
  REQUIRE(cdn_us->origin == "transcoded_store");
  REQUIRE(cdn_us->byte_capacity == simsys::ByteRate{100e9});
  REQUIRE(cdn_us->hit_ratio.has_value());
  REQUIRE(cdn_us->hit_ratio->che);
  // original_store counts the uploads as writes (review of plan 2c Task 3, I2): a 300 MB upload is
  // acknowledged at its last byte, 300 MB x 40 ms = 12 s after its wait, not at its first MB.
  const auto original = std::ranges::find(s->nodes, "original_store", &simsys::model::NodeSpec::id);
  REQUIRE(original != s->nodes.end());
  REQUIRE(original->write_ops == std::vector<std::string>{"upload"});
  const auto g = simsys::graph::Graph::build(*s);
  if (!g) FAIL(g.error().message);
  REQUIRE_THAT(*g->hit_ratio(*g->index_of("cdn_us")), Catch::Matchers::WithinAbs(0.642, 5e-4));
  REQUIRE_THAT(*g->hit_ratio(*g->index_of("cdn_eu")), Catch::Matchers::WithinAbs(0.594, 5e-4));
  const auto text = simsys::report::render_explain(*g);
  CAPTURE(text);
  // At the evening peak transcoded_store is offered the edges' misses, (1 - h_us) x 281.6/s +
  // (1 - h_eu) x 140.8/s (each client's base x 1.6 x 88 % streams) = 157.82/s of 50 MB, and the
  // transcoders' renditions: 9.6 uploads/s x 4 writes = 38.4/s of 50 MB. In all 196.22/s, and
  // 196.22 x 50 MB = 9.811k MB/s. (The store counts megabytes, so its line leads with MB/s and
  // gives the requests in parentheses.)
  REQUIRE_THAT(peak_offered_requests_per_s(text, "transcoded_store"),
               Catch::Matchers::WithinRel(196.2, 1e-3));
  REQUIRE_THAT(peak_offered_per_s(text, "transcoded_store"),
               Catch::Matchers::WithinRel(9811.0, 1e-3));
  const auto sim = simsys::engine::Simulation::create(*g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
}

// The scenario runs: its first second at the start of the day (00:00, three hours past the 21:00
// peak: both clients at base x (1 + 0.6 cos 45 deg) = x 1.4243, 300/s x 1.4243 = 427.3/s, of which
// 2 % are uploads: 8.55/s) offers the 400 transcoders (60 s a video: 6.667 jobs/s) more than they
// drain. The pool drains its capacity, so completion_q and its handler are sent the DRAINED
// 400 / 60 = 6.667 messages/s, not the 8.55/s that arrived (the steps from the third one on: the
// European upload link delays its uploads by one step).
TEST_CASE("book/youtube runs: the transcoders hand on what they drained, 6.667 jobs a second",
          "[scenario]") {
  using namespace std::chrono_literals;
  const auto g = build_or_fail(
      load_or_fail(std::string{SIMSYS_SCENARIOS_DIR} + "/book/youtube/scenario.yaml"));
  auto sim = simsys::engine::Simulation::create(g);
  if (!sim) FAIL(sim.error().message);
  simsys::engine::StepRecord last;
  for (int i = 0; i < 10; ++i) last = sim->step(i * 100ms, 100ms);
  const auto& transcoders = last.nodes[*g.index_of("transcoders")];
  REQUIRE(transcoders.demand_per_s > 8.0);  // 8.55/s offered
  REQUIRE_THAT(transcoders.drain_per_s, Catch::Matchers::WithinRel(400.0 / 60.0, 1e-9));
  REQUIRE(transcoders.backlog > 0.0);  // the excess waits in the queue's room
  REQUIRE_THAT(last.nodes[*g.index_of("completion_q")].demand_per_s,
               Catch::Matchers::WithinRel(400.0 / 60.0, 1e-9));
  REQUIRE_THAT(last.nodes[*g.index_of("completion_handler")].demand_per_s,
               Catch::Matchers::WithinRel(400.0 / 60.0, 1e-9));
  // transcoded_store's other half (review of plan 2c Task 7, minor 9): beside the edges' misses,
  // (1 - h) of what each CDN served, it is sent four 50 MB renditions per DRAINED job, 6.667 x 4 x
  // 50 MB = 1333 MB/s, not the 8.55 x 4 x 50 = 1710 MB/s of the jobs that arrived. Its demand is in
  // megabytes, as the CDNs' is. (The curve barely moves in the 150 ms the European misses take to
  // cross: hence 0.1 %.)
  const auto misses_mb_per_s = [&](std::string_view cdn) {
    const auto index = *g.index_of(cdn);
    return (1.0 - *g.hit_ratio(index)) * last.nodes[index].drain_per_s;
  };
  const double renditions_mb_per_s = last.nodes[*g.index_of("transcoded_store")].demand_per_s -
                                     misses_mb_per_s("cdn_us") - misses_mb_per_s("cdn_eu");
  REQUIRE_THAT(renditions_mb_per_s, Catch::Matchers::WithinRel(400.0 / 60.0 * 4.0 * 50.0, 1e-3));
}

// Task 8 evaluates youtube's `queue_lag` SLO (`transcode_q`, max 30m), which was `skipped` while no
// queue reported a lag. The transcoders are 400 workers at 60 s a video = 400 / 60 s = 6.667
// videos/s, and from 15:43 on the uploads (2 % of the viewers' requests, 8.55/s at 00:00 and more
// at the 21:00 peak) exceed that, so the pool's backlog is still growing at 24:00: the run's peak
// backlog is its last step's, 63.76k videos (the README's number, measured at 100 ms steps:
// 63,759 here, 63,750 at the 10 s steps below). The queue's lag is the FIFO drain time of its one
// group, backlog / drain, the pool draining its whole capacity while saturated:
//   63,760 / 6.667/s = 9,564 s = 2.66 h  >  30 m = 1,800 s    -> FAIL
// (the Erlang wait of refinement 5 is the smaller term at every step: the saturated pool's, at rho
// clamped to 0.99, is Wq(400, 0.99, 60 s) = C(400, 0.99) x 60 s / (400 x 0.01) = 11.6 s). It FAILS
// on purpose until Task 16 resizes the transcoders to 600 workers (backlog 0, lag 0.57 s: the
// Erlang wait at rho 0.96).
// The run uses 10 s steps, not the scenario's 100 ms: the fluid backlog is the same to 0.02 %
// (checked at both) and 8,640 steps, not 864k, keep the test in seconds; hence the 1 % tolerance.
// (The 24 h run stops before the daily peak: the inflow still exceeds the capacity at midnight and
// the backlog peaks at about 71.9k videos, 3.0 h of lag, near 02:18 of the next day, the review of
// Task 7 at b4e263a, I-1. The SLO sees what the run saw, 2.66 h; Task 16 decides how to run it.)
TEST_CASE("book/youtube's queue_lag SLO is evaluated and fails: 2.66 h of lag against 30 m",
          "[scenario]") {
  using namespace std::chrono_literals;
  auto s = load_or_fail(std::string{SIMSYS_SCENARIOS_DIR} + "/book/youtube/scenario.yaml");
  s.step = 10s;
  const auto g = build_or_fail(s);
  auto sim = simsys::engine::Simulation::create(g);
  if (!sim) FAIL(sim.error().message);
  simsys::report::Summary summary{g, s.warmup};
  sim->run({.duration = s.duration, .step = s.step}, [&](const auto& r) { summary.observe(r); });
  const auto results = simsys::report::evaluate_slos(summary, s.slos);
  const auto lag = std::ranges::find(results, simsys::model::SloMetric::queue_lag,
                                     [](const auto& r) { return r.slo.metric; });
  REQUIRE(lag != results.end());
  REQUIRE(lag->slo.node == "transcode_q");
  REQUIRE(lag->status == simsys::report::SloStatus::fail);  // not skipped, not passing
  REQUIRE(lag->observed.has_value());
  REQUIRE_THAT(*lag->observed, Catch::Matchers::WithinRel(9564.0, 0.01));
  REQUIRE(*lag->observed > 1800.0);  // the SLO's 30 m
}

// Ch. 15's Google Drive built its graph with the new queue wiring (notify_q's two consumer groups,
// notification_svc and backup_workers, each consume it and are linked from it), and its queue and
// pools (with their `then` steps since Task 7) are simulated. The engine stops at long_poll, its
// ws_gateway: Simulation::create checks every node's TYPE over the whole file, so the first
// unsimulated type, long_poll (declared after the pools), is the one it names. Phase 3 brings
// ws_gateway.
TEST_CASE("book/google-drive is refused at long_poll, its ws_gateway", "[scenario]") {
  const auto g = build_or_fail(
      load_or_fail(std::string{SIMSYS_SCENARIOS_DIR} + "/book/google-drive/scenario.yaml"));
  const auto sim = simsys::engine::Simulation::create(g);
  REQUIRE_FALSE(sim.has_value());
  REQUIRE(sim.error().message ==
          "node 'long_poll' has type 'ws_gateway', which run/estimate do not support yet "
          "(phase 2)");
}

// Ch. 11's news feed: its fan-out queue and pool are simulated (Task 6, their `then` steps Task 7),
// and since plan 2c Task 10 so is graph_db, its kv_cluster, the last node type it lacked (the
// refusal moved from fanout_q to graph_db, and now there is none: the nodes are client, cdn,
// blob_store, load_balancer, service, cache, sql_db, message_queue, worker_pool and kv_cluster).
TEST_CASE("book/news-feed is accepted by the engine now that its kv_cluster is simulated",
          "[scenario]") {
  const auto g = build_or_fail(
      load_or_fail(std::string{SIMSYS_SCENARIOS_DIR} + "/book/news-feed/scenario.yaml"));
  const auto sim = simsys::engine::Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
}

// The news feed runs: in its first second (00:00, the curve at 1.3 x its 10k/s base: 13k/s, a
// tenth of it posts) every post the web tier takes is one job for the fan-out pool, which reads
// graph_db once per drained job. The pool's 32 workers at 5 ms drain up to 6.4k jobs/s, far above
// the ~1.3k/s offered, so every job is drained, and the 6-node, N = 3 cluster is sent three
// replica-requests per job: its demand is exactly 3 x what the pool drained.
TEST_CASE("book/news-feed runs: graph_db is sent three replica-requests per fan-out job",
          "[scenario]") {
  using namespace std::chrono_literals;
  const auto g = build_or_fail(
      load_or_fail(std::string{SIMSYS_SCENARIOS_DIR} + "/book/news-feed/scenario.yaml"));
  auto sim = simsys::engine::Simulation::create(g);
  if (!sim) FAIL(sim.error().message);
  simsys::engine::StepRecord last;
  for (int i = 0; i < 20; ++i) last = sim->step(i * 100ms, 100ms);
  const auto& fanout = last.nodes[*g.index_of("fanout")];
  const auto& graph_db = last.nodes[*g.index_of("graph_db")];
  REQUIRE(fanout.drain_per_s > 1000.0);  // about 1.3k jobs/s at the start of the day
  REQUIRE_THAT(graph_db.demand_per_s, Catch::Matchers::WithinRel(3.0 * fanout.drain_per_s, 1e-6));
  REQUIRE(graph_db.dropped_per_s == 0.0);  // 3.9k of 180k replica-requests/s
}

// Plan 2c refinement 2: byte-sized nodes count megabytes, so the capacities the 2b scenarios gave
// no verdict (converted from bytes at an assumed 1 MB per object: cdn_us 0.28 % where the bytes
// give about 14 %) are checked in MB/s, with the numbers the books' sizes give: youtube's 50 MB
// streams and 300 MB uploads at the 21:00 peak (the load the che hit ratios leave at each store),
// news-feed's 200 KB media. The lines read "peak offered <MB/s> (<requests/s>) vs <capacity>".
TEST_CASE("byte-sized capacities are checked in MB per second", "[scenario]") {
  const auto youtube = build_or_fail(
      load_or_fail(std::string{SIMSYS_SCENARIOS_DIR} + "/book/youtube/scenario.yaml"));
  const auto youtube_text = simsys::report::render_explain(youtube);
  CAPTURE(youtube_text);
  // cdn_us: 281.6 streams/s x 50 MB = 14.08k MB/s of 100GB/s = 100k MB/s: 14.08 %.
  const auto us = check_line(youtube_text, "cdn_us");
  REQUIRE(us.contains("peak offered 14.08k MB/s (281.6/s) vs 100k MB/s"));
  REQUIRE(us.contains("(14.1%, green)"));
  // cdn_eu: 140.8 streams/s x 50 MB = 7.04k MB/s of 50GB/s = 50k MB/s: 14.08 %.
  const auto eu = check_line(youtube_text, "cdn_eu");
  REQUIRE(eu.contains("peak offered 7.04k MB/s (140.8/s) vs 50k MB/s"));
  REQUIRE(eu.contains("(14.1%, green)"));
  // original_store: 9.6 uploads/s x 300 MB = 2.88k MB/s of 5GB/s = 5k MB/s: 57.6 %.
  const auto original = check_line(youtube_text, "original_store");
  REQUIRE(original.contains("peak offered 2.88k MB/s (9.6/s) vs 5k MB/s"));
  REQUIRE(original.contains("(57.6%, green)"));
  // transcoded_store: the cdns' misses, 157.82 streams/s x 50 MB = 7.891k MB/s, plus the
  // transcoders' renditions the paper load follows since Task 7: 9.6 uploads/s offered to the
  // pool (the paper load does not queue) x 4 renditions x 50 MB = 1.92k MB/s. In all 9.811k MB/s
  // (196.22 requests/s) of 10GB/s = 10k MB/s: 98.1 %, red, and a complete number.
  const auto transcoded = check_line(youtube_text, "transcoded_store");
  REQUIRE(
      transcoded.contains("peak offered 9.811k MB/s (196.2/s) vs 10k MB/s total capacity  "
                          "(98.1%, red)"));
  REQUIRE_FALSE(transcoded.contains("load not propagated"));

  const auto news = build_or_fail(
      load_or_fail(std::string{SIMSYS_SCENARIOS_DIR} + "/book/news-feed/scenario.yaml"));
  const auto news_text = simsys::report::render_explain(news);
  CAPTURE(news_text);
  // media_store: the cdn's 5 % misses, 324.3 reads/s x 200 KB = 64.86 MB/s of 1GB/s = 1k MB/s.
  REQUIRE(check_line(news_text, "media_store")
              .contains("peak offered 64.86 MB/s (324.3/s) vs 1k MB/s total capacity  "
                        "(6.49%, green)"));
}

// Since plan 2c Task 7 explain's paper load follows a message queue into its consumers and a worker
// pool's `then` steps, so no shipped scenario is marked "load not propagated" on account of a
// queue or a pool: whatever mark remains names a type with no phase yet (notification_fanout,
// frontier). The check reads each mark, the text from "load not propagated:" to the end of its
// line.
TEST_CASE("no shipped scenario's explain blames a message queue or a worker pool for missing load",
          "[scenario]") {
  const std::string key = "load not propagated:";
  for (const auto& file : scenario_files()) {
    CAPTURE(file);
    const auto text = simsys::report::render_explain(build_or_fail(load_or_fail(file)));
    for (auto at = text.find(key); at != std::string::npos; at = text.find(key, at + 1)) {
      const auto mark = text.substr(at, text.find('\n', at) - at);
      CAPTURE(mark);
      REQUIRE_FALSE(mark.contains("message_queue"));
      REQUIRE_FALSE(mark.contains("worker_pool"));
    }
  }
}

// The 2b "not checked" note for capacities converted from bytes is gone from every shipped
// scenario, and so is the object-size assumption it rested on. (A kv_cluster's own note went with
// the ring model of plan 2c Task 10: the test after the lab's, below, pins that.)
TEST_CASE("no shipped scenario's explain says a byte capacity is not checked", "[scenario]") {
  for (const auto& file : scenario_files()) {
    CAPTURE(file);
    const auto text = simsys::report::render_explain(build_or_fail(load_or_fail(file)));
    REQUIRE_FALSE(text.contains("not checked: capacity from"));
    REQUIRE_FALSE(text.contains("at 1MB/object"));
    REQUIRE_FALSE(text.contains("byte flows: phase 2c"));
  }
}

// ---- ID generator as service sugar (plan 2b refinement 6; book ch. 7) ----

namespace {
// Builds and runs the scenario `s` to its end and returns its SLO results, failing the test with
// the graph's or engine's message on any refusal.
std::vector<simsys::report::SloResult> run_slos(simsys::model::Scenario s) {
  const auto g = simsys::graph::Graph::build(s);
  if (!g) FAIL(g.error().message);
  auto sim = simsys::engine::Simulation::create(*g);
  if (!sim) FAIL(sim.error().message);
  simsys::report::Summary summary{*g, s.warmup};
  sim->run({.duration = s.duration, .step = s.step}, [&](const auto& r) { summary.observe(r); });
  return simsys::report::evaluate_slos(summary, s.slos);
}

// The scenario at `relative` under SIMSYS_SCENARIOS_DIR, failing the test with the loader's
// message (file, line) when it does not load.
simsys::model::Scenario load_scenario(const std::string& relative) {
  const auto s = simsys::config::load(std::string{SIMSYS_SCENARIOS_DIR} + relative);
  if (!s) FAIL(s.error().file << ":" << s.error().line << ": " << s.error().message);
  return *s;
}

const simsys::report::SloResult& slo_of(const std::vector<simsys::report::SloResult>& results,
                                        simsys::model::SloMetric metric) {
  const auto found = std::ranges::find(results, metric, [](const auto& r) { return r.slo.metric; });
  if (found == results.end()) FAIL("the scenario has no such SLO");
  return *found;
}
}  // namespace

// The lab ships with the ticket server carrying the load (link weight 1) and the snowflake idle
// (weight 0), so it must show the chapter's point: one database of 8k/s cannot issue the 10k ids/s
// the book requires.
//   offered: 10k/s, and 3 x = 30k/s for the 1 minute spike at 5 m; warmup 30 s, run 10 m, so the
//   observed span is 570 s with 60 s of spike: demand = 10k x 510 + 30k x 60 = 6.9M ids;
//   served at most 8k x 570 s = 4.56M; dropped >= 6.9M - 4.56M = 2.34M (the queue of 1000 holds
//   a few more at the end, 1.4e-4 of the demand): drop rate = 2.34M / 6.9M = 0.339.
TEST_CASE("labs/unique-id runs and, with the ticket server carrying the load, fails drop_rate",
          "[scenario]") {
  const auto results = run_slos(load_scenario("/labs/unique-id/scenario.yaml"));
  const auto& drops = slo_of(results, simsys::model::SloMetric::drop_rate);
  CAPTURE(drops.observed.value_or(-1.0));
  REQUIRE(drops.status == simsys::report::SloStatus::fail);
  REQUIRE(drops.observed.has_value());
  REQUIRE_THAT(*drops.observed, Catch::Matchers::WithinAbs(0.339, 2e-3));
}

// Switching the link weights, as the lab's comment says, routes everything to the four
// snowflake generators: 16M ids/s of capacity against 10k/s (30k/s in the spike), so nothing
// drops and every SLO passes. The lab's two links are written in this order: ticket_server,
// snowflake.
TEST_CASE("labs/unique-id passes its SLOs once the link weights route to the snowflake",
          "[scenario]") {
  auto s = load_scenario("/labs/unique-id/scenario.yaml");
  REQUIRE(s.links.size() == 2);
  REQUIRE(s.links[0].to == "ticket_server");
  REQUIRE(s.links[1].to == "snowflake");
  s.links[0].weight = 0.0;
  s.links[1].weight = 1.0;
  const auto results = run_slos(std::move(s));
  REQUIRE(results.size() == 2);
  for (const auto& r : results) {
    CAPTURE(static_cast<int>(r.slo.metric), r.observed.value_or(-1.0));
    REQUIRE(r.status == simsys::report::SloStatus::pass);
  }
  REQUIRE(slo_of(results, simsys::model::SloMetric::drop_rate).observed == 0.0);
}

// The lab's snowflake states neither a capacity nor a latency: the algorithm supplies them
// (review of 3adc555, I3), 2^12 ids per millisecond = 4.096M/s and 10 us per machine, so four
// machines give 4 x 4.096M = 16.384M/s, printed with four significant digits.
TEST_CASE("labs/unique-id's snowflake takes its capacity and latency from the algorithm",
          "[scenario]") {
  const auto s = load_scenario("/labs/unique-id/scenario.yaml");
  const auto snowflake = std::ranges::find(s.nodes, "snowflake", &simsys::model::NodeSpec::id);
  REQUIRE(snowflake != s.nodes.end());
  REQUIRE_FALSE(snowflake->capacity.has_value());
  REQUIRE_FALSE(snowflake->latency.has_value());
  const auto g = simsys::graph::Graph::build(s);
  if (!g) FAIL(g.error().message);
  const auto text = simsys::report::render_explain(*g);
  CAPTURE(text);
  REQUIRE(text.contains("capacity=4.096M/s (algorithm)  service_time=10us (algorithm)"));
  REQUIRE(check_line(text, "snowflake").contains("16.38M/s total capacity"));
}

// The two book scenarios that use `algorithm: snowflake` still load, build and explain, and now
// say what the algorithm stands for. url-shortener: `replicas: 2`; chat-system: `replicas: 3`.
// Their totals: 2 x 4.096M = 8.192M/s and 3 x 4.096M = 12.288M/s (printed with 4 significant
// digits).
TEST_CASE("book scenarios with a snowflake id generator validate and explain the algorithm",
          "[scenario]") {
  const std::vector<std::pair<std::string, std::string>> books{
      {"/book/url-shortener/scenario.yaml", "8.192M/s total capacity"},
      {"/book/chat-system/scenario.yaml", "12.29M/s total capacity"}};
  for (const auto& [file, total] : books) {
    CAPTURE(file);
    const auto s = load_scenario(file);
    const auto g = simsys::graph::Graph::build(s);
    if (!g) FAIL(g.error().message);
    const auto text = simsys::report::render_explain(*g);
    CAPTURE(text);
    REQUIRE(text.contains("algorithm=snowflake (4.096M/s per replica by design)"));
    const auto at = text.find("\n  id_gen ", text.find("checks ("));
    REQUIRE(at != std::string::npos);
    const auto line = text.substr(at + 1, text.find('\n', at + 1) - at - 1);
    REQUIRE(line.contains(total));
  }
}

// ---- The first book system end to end (plan 2b, Task 8; book ch. 8) ----

// book/url-shortener, sized with the book's figures (100M writes/day, 10:1 reads; a stateless web
// tier scaled out, a cache absorbing the reads, the database sharded), runs its 24 hours and
// meets both of its SLOs at the 14:00 peak. Two headline numbers are pinned:
//  - the url_cache hit ratio explain resolves by Che's approximation (LRU, 20M slots over the
//    clients' Zipf(1.0) space of 100M keys): 0.881, as explain prints it (an independent Python
//    computation of Che's fixed point gives 0.8806);
//  - the redirect p99, 7.49 ms in the run (simsys run, at step 1s and at 100ms alike). Its mean,
//    4.10 ms, is mostly wire: five round trips of 2 x 250 us (users -> limiter, limiter ->
//    rules_cache, limiter -> lb, lb -> web, web -> url_cache) = 2.5 ms, plus the service times
//    (50 + 250 + 50 + 300 + 250 us) and the web tier's queueing (rho 0.8 at the peak), plus the
//    11.9 % of misses that go on to a database replica. The p99 is the lognormal fitted to the
//    response's mean and variance; 2 % of tolerance, so a change to the model that moves it is
//    seen and the README's numbers are re-checked.
// And nothing is dropped: no node is red at the peak (web 80 %, the busiest database copy 65 %).
TEST_CASE("book/url-shortener runs its 24 hours and meets both SLOs with a 0.881 che hit ratio",
          "[scenario]") {
  const auto s = load_scenario("/book/url-shortener/scenario.yaml");
  const auto g = simsys::graph::Graph::build(s);
  if (!g) FAIL(g.error().message);
  REQUIRE_THAT(*g->hit_ratio(*g->index_of("url_cache")), Catch::Matchers::WithinAbs(0.881, 5e-4));
  const auto results = run_slos(s);
  REQUIRE(results.size() == 2);
  const auto& p99 = slo_of(results, simsys::model::SloMetric::p99_latency);
  const auto& drops = slo_of(results, simsys::model::SloMetric::drop_rate);
  CAPTURE(p99.observed.value_or(-1.0), drops.observed.value_or(-1.0));
  REQUIRE(p99.status == simsys::report::SloStatus::pass);
  REQUIRE(drops.status == simsys::report::SloStatus::pass);
  REQUIRE(p99.observed.has_value());
  REQUIRE_THAT(*p99.observed, Catch::Matchers::WithinRel(7.49e-3, 0.02));
  REQUIRE(drops.observed == 0.0);
}

// ---- Request sizes and link bandwidth in the shipped scenarios (phase 2c, Task 1) --------------
//
// `sizes:` and `bandwidth:` used to be read and ignored; the loader now types them, so a shipped
// scenario that names an op its client does not send (or writes a size the grammar refuses, or
// 0B) would stop loading: every scenario loading (above) proves those loader rules hold for all
// of them. The sizes below are each scenario's own, with the arithmetic of its unit.

namespace {

// The size `scenario` states for `op` at the client `client_id`, in bytes; the test fails when the
// client or the op's size is missing.
double stated_size_of(const simsys::model::Scenario& scenario, std::string_view client_id,
                      std::string_view op) {
  const auto client = std::ranges::find(scenario.nodes, client_id, &simsys::model::NodeSpec::id);
  REQUIRE(client != scenario.nodes.end());
  REQUIRE(client->workload.has_value());
  const auto found = std::ranges::find_if(client->workload->sizes,
                                          [&](const auto& entry) { return entry.first == op; });
  REQUIRE(found != client->workload->sizes.end());
  return found->second.count;
}

}  // namespace

TEST_CASE("book/youtube's clients state the sizes of a stream and an upload in bytes",
          "[scenario]") {
  const auto s = load_scenario("/book/youtube/scenario.yaml");
  for (const auto* client : {"viewers_us", "viewers_eu"}) {
    CAPTURE(client);
    // 50 MB = 5e7 B; 300 MB = 3e8 B; 2 KB = 2e3 B.
    CHECK_THAT(stated_size_of(s, client, "stream"), Catch::Matchers::WithinRel(5e7, 1e-12));
    CHECK_THAT(stated_size_of(s, client, "upload"), Catch::Matchers::WithinRel(3e8, 1e-12));
    CHECK_THAT(stated_size_of(s, client, "upload_meta"), Catch::Matchers::WithinRel(2e3, 1e-12));
    CHECK_THAT(stated_size_of(s, client, "metadata"), Catch::Matchers::WithinRel(2e3, 1e-12));
  }
}

TEST_CASE("book scenarios and the kv-store lab state their request sizes in bytes", "[scenario]") {
  // news-feed: 2 KB = 2e3 B, 200 KB = 2e5 B.
  const auto news = load_scenario("/book/news-feed/scenario.yaml");
  CHECK_THAT(stated_size_of(news, "users", "post"), Catch::Matchers::WithinRel(2e3, 1e-12));
  CHECK_THAT(stated_size_of(news, "users", "read_media"), Catch::Matchers::WithinRel(2e5, 1e-12));
  // google-drive: 500 KB = 5e5 B (the book's average file), 1 KB = 1e3 B.
  const auto drive = load_scenario("/book/google-drive/scenario.yaml");
  CHECK_THAT(stated_size_of(drive, "clients", "download"), Catch::Matchers::WithinRel(5e5, 1e-12));
  CHECK_THAT(stated_size_of(drive, "clients", "upload"), Catch::Matchers::WithinRel(5e5, 1e-12));
  CHECK_THAT(stated_size_of(drive, "clients", "metadata"), Catch::Matchers::WithinRel(1e3, 1e-12));
  // chat-system: 1 KB = 1e3 B.
  const auto chat = load_scenario("/book/chat-system/scenario.yaml");
  CHECK_THAT(stated_size_of(chat, "users", "send"), Catch::Matchers::WithinRel(1e3, 1e-12));
  // web-crawler: 200 B.
  const auto crawler = load_scenario("/book/web-crawler/scenario.yaml");
  CHECK_THAT(stated_size_of(crawler, "seeds", "crawl"), Catch::Matchers::WithinRel(200.0, 1e-12));
  // kv-store lab: 1 KB = 1e3 B.
  const auto kv = load_scenario("/labs/kv-store/scenario.yaml");
  CHECK_THAT(stated_size_of(kv, "clients", "put"), Catch::Matchers::WithinRel(1e3, 1e-12));
}

// ---- Request sizes (plan 2c refinement 1, Task 2) ----
//
// A byte-sized node counts megabytes, so the graph refuses a scenario in which an op reaches one
// without a size (Graph::build). Every shipped scenario builds, so every op that reaches one of
// its byte-sized nodes has a size: the scenarios say so with `sizes:` on a client or `size:` on
// a step that renames the op (youtube's transcoders, google-drive's block servers and backup
// workers, search-autocomplete's aggregators, web-crawler's parser).

TEST_CASE("every op that reaches a byte-sized node of a shipped scenario has a size",
          "[scenario]") {
  std::size_t byte_sized_nodes = 0;
  for (const auto& file : scenario_files()) {
    CAPTURE(file);
    const auto g = build_or_fail(load_or_fail(file));
    for (std::size_t node = 0; node < g.nodes().size(); ++node) {
      if (!simsys::defaults::is_byte_sized(g.nodes()[node].spec)) continue;
      ++byte_sized_nodes;
      for (const auto& op : g.arriving_ops(node)) {
        CAPTURE(g.nodes()[node].spec.id, op);
        CHECK(g.request_bytes(node, op).has_value());
      }
    }
  }
  // youtube alone has four (both CDNs and both stores): the loop is not vacuous.
  CHECK(byte_sized_nodes >= 4);
}

namespace {
// The size, in bytes, of `op` at node `id` of `g`; the test fails when there is none.
double request_bytes_of(const simsys::graph::Graph& g, std::string_view id, std::string_view op) {
  const auto size = g.request_bytes(*g.index_of(id), op);
  if (!size) FAIL("no size for op '" << op << "' at node '" << id << "'");
  return size->count;
}
}  // namespace

// Ch. 14's numbers as the scenario states them (`sizes:` on the viewers, `size:` on the
// transcoders' step):
//  - a stream is 50 MB = 5e7 B, and both CDNs fetch their misses from transcoded_store as the same
//    op, so the store sees one size (two CDNs agreeing, no conflict) of 5e7 B;
//  - an upload is 300 MB = 3e8 B and goes straight from the viewers to original_store;
//  - the transcoders write one rendition per output, which the scenario sizes like a stream,
//    50 MB = 5e7 B, since the write renames the op and would otherwise be unsized.
TEST_CASE("book/youtube sizes its streams, uploads and renditions at the byte-sized stores",
          "[scenario]") {
  const auto g = build_or_fail(load_scenario("/book/youtube/scenario.yaml"));
  REQUIRE_THAT(request_bytes_of(g, "cdn_us", "stream"), Catch::Matchers::WithinRel(5e7, 1e-12));
  REQUIRE_THAT(request_bytes_of(g, "cdn_eu", "stream"), Catch::Matchers::WithinRel(5e7, 1e-12));
  REQUIRE_THAT(request_bytes_of(g, "transcoded_store", "stream"),
               Catch::Matchers::WithinRel(5e7, 1e-12));
  REQUIRE_THAT(request_bytes_of(g, "original_store", "upload"),
               Catch::Matchers::WithinRel(3e8, 1e-12));
  REQUIRE_THAT(request_bytes_of(g, "transcoded_store", "write"),
               Catch::Matchers::WithinRel(5e7, 1e-12));
}

TEST_CASE("explain on book/youtube prints the stream size on the byte-sized cdn", "[scenario]") {
  const auto g = build_or_fail(load_scenario("/book/youtube/scenario.yaml"));
  const auto text = simsys::report::render_explain(g);
  CAPTURE(text);
  REQUIRE(node_line(text, "cdn_us").contains("sizes: stream 50MB"));
}

// ---- The two shipped scenarios with external sinks (plan 2c Task 9) ----
//
// External sinks run since Task 9, but no scenario that runs after phase 2c reaches one:
// notification-system (apns, fcm, sms_provider, email_provider: its workers' `then` steps) and
// chat-system (push) each also need a type of a later phase, so the engine still refuses them,
// and at the same node as before: Simulation::create checks every node's TYPE over the whole file
// and names the first unsupported one in file order. Giving the sinks a model changes neither
// refusal; their `failure_rate` shows in explain instead.

// notification-system's nodes in file order: services, lb, notification_svc, user_cache,
// user_db, user_limiter (all simulated), then `fanout`, a notification_fanout: the first node of
// a type with no model.
TEST_CASE("book/notification-system is still refused at fanout, its notification_fanout",
          "[scenario]") {
  const auto g = build_or_fail(load_scenario("/book/notification-system/scenario.yaml"));
  const auto sim = simsys::engine::Simulation::create(g);
  REQUIRE_FALSE(sim.has_value());
  REQUIRE(sim.error().message ==
          "node 'fanout' has type 'notification_fanout', which run/estimate do not support yet "
          "(phase 2)");
}

// chat-system's nodes in file order: users, lb, the chat API service, then `discovery`, a
// service_discovery, which comes before the `chat_servers` ws_gateway after it.
TEST_CASE("book/chat-system is still refused at discovery, its service_discovery", "[scenario]") {
  const auto g = build_or_fail(load_scenario("/book/chat-system/scenario.yaml"));
  const auto sim = simsys::engine::Simulation::create(g);
  REQUIRE_FALSE(sim.has_value());
  REQUIRE(sim.error().message ==
          "node 'discovery' has type 'service_discovery', which run/estimate do not support yet "
          "(phase 2)");
}

// The books' providers' failure rates are the numbers in the YAML: APNs and FCM 2 %, the SMS
// provider 5 %, the e-mail provider 1 % (ch. 10: third-party services are the unreliable part of
// the pipeline), and chat-system's push gateway 2 %.
TEST_CASE("explain on the shipped scenarios prints each external sink's failure rate",
          "[scenario]") {
  const auto notification = build_or_fail(load_scenario("/book/notification-system/scenario.yaml"));
  const auto notification_text = simsys::report::render_explain(notification);
  CAPTURE(notification_text);
  REQUIRE(node_line(notification_text, "apns").contains("failure_rate=2%"));
  REQUIRE(node_line(notification_text, "fcm").contains("failure_rate=2%"));
  REQUIRE(node_line(notification_text, "sms_provider").contains("failure_rate=5%"));
  REQUIRE(node_line(notification_text, "email_provider").contains("failure_rate=1%"));

  const auto chat = build_or_fail(load_scenario("/book/chat-system/scenario.yaml"));
  const auto chat_text = simsys::report::render_explain(chat);
  CAPTURE(chat_text);
  REQUIRE(node_line(chat_text, "push").contains("failure_rate=2%"));
}

// ---- The kv-store lab at 16 nodes (plan 2c refinement 9, Task 10; book ch. 6) ----
//
// The lab ships with `nodes: 16`: 30k requests/s from the clients, 3 x 30k = 90k replica-requests/s
// (N = 3: the coordinator sends each read and write to three replicas) over 16 nodes of 10k/s.
//   average node  90k / 16 = 5.625k/s   = 56.25 %
//   hottest node  k x 5.625k, k = 1 + z_16/sqrt(3 x 150) = 1.08338 (z_16 = 1.7688, the expected
//                 maximum of 16 standard normals; 150 vnodes), = 6.094k/s = 60.94 %
// The `utilization` SLO (max 0.8) is read at the hottest node, so it observes 0.6094 and passes;
// the `drop_rate` SLO (max 0.01) sees nothing dropped; the put's p99 (max 20 ms) is far below it:
// a put crosses four 0.25 ms link legs (1 ms), the coordinator (0.2 ms of service, utilization 0.5
// over 4 x 15k/s: 0.232 ms with its Erlang wait) and the W = 2 quorum over its key's real replicas
// (with probability p_hot = 3k/16 = 0.203 the hot machine and two cool ones, otherwise three cool
// ones; each machine's sojourn a lognormal of its own moments: 0.313 ms), so its mean is 1.544 ms
// and the lognormal fit's p99 2.31 ms (re-derived for the review of Tasks 9-10, C-1: 2.41 ms
// before, when every replica was an exponential of the machines' mean sojourn).
namespace {
// k = min(1 + z_M / sqrt(N v), M / N), z_M by Blom (queueing::expected_normal_maximum).
double ring_factor(int machines, int replication, int vnodes) {
  return std::min(1.0 + simsys::queueing::expected_normal_maximum(machines) /
                            std::sqrt(static_cast<double>(replication) * vnodes),
                  static_cast<double>(machines) / replication);
}

// The lab with its store at `nodes` machines of `vnodes` virtual nodes each, `minutes` long.
simsys::model::Scenario kv_lab(int nodes, int vnodes, std::chrono::minutes minutes) {
  auto s = load_scenario("/labs/kv-store/scenario.yaml");
  const auto store = std::ranges::find(s.nodes, "store", &simsys::model::NodeSpec::id);
  REQUIRE(store != s.nodes.end());
  store->kv_nodes = nodes;
  store->vnodes = vnodes;
  s.duration = minutes;
  return s;
}
}  // namespace

TEST_CASE(
    "labs/kv-store at sixteen nodes runs and passes its SLOs with the hottest node at 60.9 percent",
    "[scenario]") {
  const auto s = load_scenario("/labs/kv-store/scenario.yaml");
  const auto store = std::ranges::find(s.nodes, "store", &simsys::model::NodeSpec::id);
  REQUIRE(store != s.nodes.end());
  REQUIRE(store->kv_nodes == 16);
  const auto results = run_slos(s);
  REQUIRE(results.size() == s.slos.size());
  for (const auto& r : results) {
    CAPTURE(r.slo.node, static_cast<int>(r.slo.metric), r.observed.value_or(-1.0));
    REQUIRE(r.status == simsys::report::SloStatus::pass);
  }
  const auto& utilization = slo_of(results, simsys::model::SloMetric::utilization);
  REQUIRE(utilization.observed.has_value());
  REQUIRE_THAT(
      *utilization.observed,
      Catch::Matchers::WithinRel(90'000.0 / 16.0 / 10'000.0 * ring_factor(16, 3, 150), 1e-3));
  REQUIRE_THAT(*utilization.observed, Catch::Matchers::WithinAbs(0.6094, 5e-4));
  const auto& drops = slo_of(results, simsys::model::SloMetric::drop_rate);
  REQUIRE(drops.observed == 0.0);
  const auto& p99 = slo_of(results, simsys::model::SloMetric::p99_latency);
  REQUIRE(p99.observed.has_value());
  REQUIRE_THAT(*p99.observed, Catch::Matchers::WithinAbs(2.31e-3, 0.01e-3));
}

// The lesson the lab's comment states: the same lab at the book's eight nodes is over capacity.
// 90k replica-requests/s over 8 x 10k = 80k/s: 11.25k per node (112.5 %), the hottest
// k = 1.0676 x that = 12.01k (120.1 %). Every node's queue fills, so the cluster serves 80k of the
// 90k and drops one ninth: drop_rate 1/9 = 0.1111 against its 0.01, and the hottest node's rho
// saturates at 1.0 against the utilization SLO's 0.8. The put's p99 fails too: every replica now
// waits for its full room, 1000 requests drained at 10k/s = 100 ms, with the spread the fluid
// gives a full queue (the Erlang wait's at the 0.99 clamp, sd 10 ms: three servers per machine);
// the second of three such replies is 100.03 ms (sd 6.7 ms), and the client's p99 117.8 ms. (The
// 306 ms the lab showed before the review of Tasks 9-10 came from treating each replica as an
// exponential of mean 100 ms, sd 100 ms, not from the room.) Three SLOs fail. (5 minutes of the
// 30, 1 of them warmup: the queues fill in under a second.)
TEST_CASE(
    "labs/kv-store at eight nodes drops a ninth of the replica-requests and fails all three "
    "SLOs",
    "[scenario]") {
  const auto results = run_slos(kv_lab(8, 150, std::chrono::minutes{5}));
  const auto& drops = slo_of(results, simsys::model::SloMetric::drop_rate);
  REQUIRE(drops.status == simsys::report::SloStatus::fail);
  REQUIRE(drops.observed.has_value());
  REQUIRE_THAT(*drops.observed, Catch::Matchers::WithinRel(1.0 / 9.0, 1e-3));
  const auto& utilization = slo_of(results, simsys::model::SloMetric::utilization);
  REQUIRE(utilization.status == simsys::report::SloStatus::fail);
  REQUIRE(utilization.observed.has_value());
  REQUIRE_THAT(*utilization.observed, Catch::Matchers::WithinRel(1.0, 1e-3));
  const auto& p99 = slo_of(results, simsys::model::SloMetric::p99_latency);
  REQUIRE(p99.status == simsys::report::SloStatus::fail);
  REQUIRE(p99.observed.has_value());
  REQUIRE_THAT(*p99.observed, Catch::Matchers::WithinRel(117.8e-3, 0.01));
}

// The lab's "try vnodes: 1" at 16 machines (review of Tasks 9-10, C-1): the ring is at its most
// uneven, k = 1 + z_16/sqrt(3) = 2.021, so the hot machine is sent 11.37k/s against its 10k/s and
// drops 1.37k of the 90k replica-requests/s (drop rate 1.52 %, the SLO's 1 % fails) at rho 1 (the
// utilization SLO fails). The put's p99 PASSES: a key's three replicas hold the hot machine with
// probability 3k/16 = 0.379, and then the two cool replicas (rho 0.524) answer a W = R = 2 quorum
// first, so the store answers in 0.378 ms and nothing is lost; the client's p99 is 2.65 ms. (The
// old model put the quorum over the machines' mean sojourn, 10.7 ms, and failed it at 39.7 ms.)
TEST_CASE("labs/kv-store with one virtual node passes its p99: the quorum hides the hot machine",
          "[scenario]") {
  const auto results = run_slos(kv_lab(16, 1, std::chrono::minutes{5}));
  const auto& p99 = slo_of(results, simsys::model::SloMetric::p99_latency);
  REQUIRE(p99.status == simsys::report::SloStatus::pass);
  REQUIRE(p99.observed.has_value());
  REQUIRE_THAT(*p99.observed, Catch::Matchers::WithinRel(2.65e-3, 0.01));
  REQUIRE(slo_of(results, simsys::model::SloMetric::utilization).status ==
          simsys::report::SloStatus::fail);
  const auto& drops = slo_of(results, simsys::model::SloMetric::drop_rate);
  REQUIRE(drops.status == simsys::report::SloStatus::fail);
  REQUIRE(drops.observed.has_value());
  REQUIRE_THAT(*drops.observed,
               Catch::Matchers::WithinRel(
                   (90'000.0 * ring_factor(16, 3, 1) / 16.0 - 10'000.0) / 90'000.0, 1e-3));
}

// The consistent-hashing lab's balancer ring uses the same heuristic (review of Tasks 9-10, I-2),
// with N = 1 copy of each key over its M = 4 cache servers: k = 1 + z_4/sqrt(150) = 1.0857
// (z_4 = 1.0491). 40k/s over 4 x 15k/s is 66.7 % on average, so the hottest server, which the
// utilization SLO reads, runs at 2/3 x 1.0857 = 0.7238 (0.7211 with the old 1 + 1/sqrt(150)).
TEST_CASE("the consistent-hashing lab's hottest server runs at the ring factor over four servers",
          "[scenario]") {
  const auto results = run_slos(load_scenario("/labs/consistent-hashing/scenario.yaml"));
  const auto caches = std::ranges::find_if(results, [](const auto& r) {
    return r.slo.metric == simsys::model::SloMetric::utilization &&
           r.slo.node == std::optional<std::string>{"cache_servers"};
  });
  REQUIRE(caches != results.end());
  REQUIRE(caches->observed.has_value());
  REQUIRE_THAT(*caches->observed,
               Catch::Matchers::WithinRel(40'000.0 / 60'000.0 * ring_factor(4, 1, 150), 1e-6));
  REQUIRE_THAT(*caches->observed, Catch::Matchers::WithinAbs(0.7238, 1e-4));
}

// The "not checked: kv_cluster ..." note of phase 2c's first half is gone from every shipped
// scenario: each kv_cluster (the lab's store, news-feed's graph_db, chat-system's message_store
// and presence, notification-system's log_db) is modelled now, so explain checks it, or, where
// its load is behind a type with no model yet (notification-system's fanout), marks that.
TEST_CASE("no shipped scenario's explain says a kv_cluster is not checked", "[scenario]") {
  for (const auto& file : scenario_files()) {
    CAPTURE(file);
    const auto text = simsys::report::render_explain(build_or_fail(load_or_fail(file)));
    REQUIRE_FALSE(text.contains("not checked: kv_cluster"));
    REQUIRE_FALSE(text.contains("replication and quorums: phase 3)"));
  }
}

// The lab's own explain, at its shipped 16 nodes: the line the paper load gives the store.
TEST_CASE("explain on labs/kv-store checks the store at 16 nodes", "[scenario]") {
  const auto g = build_or_fail(load_scenario("/labs/kv-store/scenario.yaml"));
  const auto text = simsys::report::render_explain(g);
  CAPTURE(text);
  REQUIRE(check_line(text, "store")
              .contains("peak offered 90k/s (30k/s × N 3) vs 160k/s (16 nodes)  "
                        "(hottest 60.9%, green)"));
  REQUIRE(node_line(text, "store").contains("nodes=16 N=3 W=2 R=2 mode=ap (partitions: phase 3)"));
}

// ---- Search autocomplete runs: the search index serves like a service (plan 2c Task 11) --------
//
// Ch. 13's trie database is a search_index: a service with no steps (spec section 7), here 4
// replicas of 2k/s at 5 ms behind a read_through cache. Every node type the scenario uses is now
// simulated (the queue, the pool and the blob store since 2c Tasks 4 and 6-7), so the engine
// accepts it.

TEST_CASE(
    "book/search-autocomplete is accepted by the engine now that its search_index is simulated",
    "[scenario]") {
  const auto g = build_or_fail(load_scenario("/book/search-autocomplete/scenario.yaml"));
  const auto sim = simsys::engine::Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
}

// The peak is 20:00, where the client's wave is at its top: 24k/s x (1 + amplitude 0.5) = 36k/s.
// Following the paper load down the graph (explain's checks, no queueing):
//  - the client splits 95 % suggest and 5 % search. The browser cache sees the suggests, 36k x
//    0.95 = 34.2k/s, and its fixed hit ratio 0.4 lets 0.6 x 34.2k = 20.52k/s through to lb; the
//    searches go to lb directly, 36k x 0.05 = 1.8k/s; lb and api are offered 20.52k + 1.8k =
//    22.32k/s;
//  - the api sends each suggest to trie_cache (20.52k/s) and each search to query_log_q (1.8k/s);
//  - trie_cache is read_through with a che hit ratio h (explain prints 0.995), so the misses it
//    forwards to trie_db are (1 - h) x 20.52k = 103.1/s (the exact h is read from the graph below,
//    for h = 0.995 itself would say 102.6), against 4 replicas x 2k/s = 8k/s: 103.1 / 8000 =
//    1.29 %, green;
//  - query_log_q passes the 1.8k searches/s to the aggregators: 4 workers at 2 ms drain
//    4 / 2 ms = 2k/s, so 1.8k / 2k = 90 %, red (the red zone starts at the engine's 90 % default);
//  - the aggregators write one 1 KB row per search (the `size: 1KB` step): 1.8k/s x 1 KB = 1.8 MB/s
//    (decimal megabytes) against aggregated_store's 100 MB/s, 1.8 %, green.
TEST_CASE("explain on book/search-autocomplete checks the index, the aggregators and the store",
          "[scenario]") {
  const auto g = build_or_fail(load_scenario("/book/search-autocomplete/scenario.yaml"));
  const auto text = simsys::report::render_explain(g);
  CAPTURE(text);
  const double h = *g.hit_ratio(*g.index_of("trie_cache"));
  REQUIRE_THAT(peak_offered_per_s(text, "trie_db"),
               Catch::Matchers::WithinRel(20'520.0 * (1.0 - h), 1e-3));
  REQUIRE(check_line(text, "trie_db")
              .contains("peak offered 103.1/s vs 8k/s total capacity  (1.29%, green)"));
  REQUIRE(check_line(text, "aggregators")
              .contains("peak offered 1.8k/s vs 2k/s total capacity  (90%, red)"));
  REQUIRE(
      check_line(text, "aggregated_store")
          .contains("peak offered 1.8 MB/s (1.8k/s) vs 100 MB/s total capacity  (1.8%, green)"));
}

// The index's own line: its numbers are the scenario's (2k/s and 5 ms per replica, so Little's law
// gives c = 2000 x 5 ms = 10 per replica) and it says its `build_interval` is stored, not modelled.
TEST_CASE("explain on book/search-autocomplete prints the index's numbers and its build_interval",
          "[scenario]") {
  const auto g = build_or_fail(load_scenario("/book/search-autocomplete/scenario.yaml"));
  const auto text = simsys::report::render_explain(g);
  CAPTURE(text);
  const auto line = node_line(text, "trie_db");
  REQUIRE(line.contains("capacity=2k/s (scenario)  service_time=5ms (scenario)  c=10 (implied)"));
  REQUIRE(line.contains("build_interval=24h (rebuild load not modelled)"));
}

// Measured 2026-10-29 (Release, the shipped 100 ms step, the whole day): p99 of suggest 6.86 ms
// against its 100 ms and a drop rate of 0 against 0.0005; the busiest node is the aggregators at
// 90 % of peak and never above it (no red second, no drop). Both SLOs PASS on the shipped
// numbers. Here the day runs at a 1 s step, as the url-shortener's test does (86.4k steps, a few
// seconds in Debug, where 862.8k steps at 100 ms would take minutes): the fluid model's averages do
// not depend on the step, and a pass with a wide margin (6.86 ms of 100 ms) does not hinge on it.
TEST_CASE("book/search-autocomplete meets both SLOs over its day at a one second step",
          "[scenario]") {
  using namespace std::chrono_literals;
  auto s = load_scenario("/book/search-autocomplete/scenario.yaml");
  s.step = 1s;
  const auto results = run_slos(std::move(s));
  REQUIRE(results.size() == 2);
  const auto& p99 = slo_of(results, simsys::model::SloMetric::p99_latency);
  const auto& drops = slo_of(results, simsys::model::SloMetric::drop_rate);
  CAPTURE(p99.observed.value_or(-1.0), drops.observed.value_or(-1.0));
  REQUIRE(p99.status == simsys::report::SloStatus::pass);
  REQUIRE(drops.status == simsys::report::SloStatus::pass);
  REQUIRE(drops.observed == 0.0);
}
