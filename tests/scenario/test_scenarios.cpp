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
#include <string_view>
#include <utility>
#include <vector>

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
    if (s.name == "lab-unique-id" || s.name == "url-shortener") continue;  // not run here
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
  CHECK(ran > 0);  // the tutorials at least always run here (the two above are not counted)
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
// service, id_generator, sql_db) use only those. Every other lab and book scenario still needs a
// later task's node types, so the set of runnable scenarios is pinned by name: a node type
// added later makes this fail and forces whoever adds it to update the list on purpose.
TEST_CASE(
    "scenarios that run under phase 2b are: chain, hello, consistent-hashing, rate-limiter, "
    "unique-id, url-shortener",
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
                                               "lab-rate-limiter", "lab-unique-id",
                                               "url-shortener"});
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
// che hit ratio: valid scenario, explained, and its byte-sized nodes run (plan 2c Task 3), but
// the engine still refuses it at its first queue, transcode_q: the queues and workers are
// Tasks 6 and 7. The first refusal is the first node in the YAML whose type the engine does not
// simulate (original_store, a blob store whose `then` publishes each upload to transcode_q, is
// simulated since Task 4: its steps are asynchronous event notifications). What
// the cdn changed is pinned through explain (review of 4d539b0, M12): the two edges'
// Che hit ratios (Zipf 1 over 10M videos, 100k and 50k of them at the edge: 0.642 and 0.594,
// checked against an independent Python computation by the reviewer) and the miss flow they
// send transcoded_store, which an explain that ignored the origin edge would leave at zero.
TEST_CASE("book/youtube validates and explains, and the engine refuses it at transcode_q",
          "[scenario]") {
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
  // The edges' misses at the evening peak, (1 − h_us) × 281.6/s + (1 − h_eu) × 140.8/s (each
  // client's base × 1.6 × 88 % streams) = 157.8/s; the transcoders' writes add nothing on paper
  // yet (a worker pool's load is phase 2c's). The store counts megabytes, so its line leads with
  // MB/s (157.82/s × 50 MB = 7.891k MB/s) and gives the requests in parentheses.
  REQUIRE_THAT(peak_offered_requests_per_s(text, "transcoded_store"),
               Catch::Matchers::WithinRel(157.8, 1e-3));
  REQUIRE_THAT(peak_offered_per_s(text, "transcoded_store"),
               Catch::Matchers::WithinRel(7890.0, 1e-3));
  const auto sim = simsys::engine::Simulation::create(*g);
  REQUIRE_FALSE(sim.has_value());
  // viewers_us, viewers_eu, cdn_us, cdn_eu, lb, api, metadata_cache, metadata_db, original_store
  // are all simulated; transcode_q, the next node in the file, is a message queue.
  REQUIRE(sim.error().message ==
          "node 'transcode_q' has type 'message_queue', which run/estimate do not support yet "
          "(phase 2)");
}

// Ch. 15's Google Drive is refused where its first message queue comes, notify_q (the order of the
// file: lb, api, block_servers, the two stores, the metadata tier, then notify_q). Its blob stores
// have no `then` steps, so they were never what stopped it; the queue and its two consumer groups
// are Tasks 6 and 7, and the pin stays until then. A characterisation test: it passed before
// plan 2c Task 4 too, and pins that the events did not move the refusal.
TEST_CASE("book/google-drive is refused at notify_q", "[scenario]") {
  const auto g = build_or_fail(
      load_or_fail(std::string{SIMSYS_SCENARIOS_DIR} + "/book/google-drive/scenario.yaml"));
  const auto sim = simsys::engine::Simulation::create(g);
  REQUIRE_FALSE(sim.has_value());
  REQUIRE(sim.error().message ==
          "node 'notify_q' has type 'message_queue', which run/estimate do not support yet "
          "(phase 2)");
}

// Ch. 11's news feed gets as far as its fan-out queue: the cdn and the media store (a blob store
// counting megabytes) are simulated now, so the first node the engine cannot run is fanout_q, a
// message queue (the second node of the media path used to be the one refused).
TEST_CASE("book/news-feed is refused at fanout_q, no longer at media_store", "[scenario]") {
  const auto g = build_or_fail(
      load_or_fail(std::string{SIMSYS_SCENARIOS_DIR} + "/book/news-feed/scenario.yaml"));
  const auto sim = simsys::engine::Simulation::create(g);
  REQUIRE_FALSE(sim.has_value());
  REQUIRE(sim.error().message ==
          "node 'fanout_q' has type 'message_queue', which run/estimate do not support yet "
          "(phase 2)");
}

namespace {
// The check line of node `id` in explain's checks block.
std::string check_line_of(const std::string& explain_text, const std::string& id) {
  const auto at = explain_text.find("\n  " + id + " ", explain_text.find("checks ("));
  REQUIRE(at != std::string::npos);
  return explain_text.substr(at + 1, explain_text.find('\n', at + 1) - at - 1);
}
}  // namespace

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
  const auto us = check_line_of(youtube_text, "cdn_us");
  REQUIRE(us.contains("peak offered 14.08k MB/s (281.6/s) vs 100k MB/s"));
  REQUIRE(us.contains("(14.1%, green)"));
  // cdn_eu: 140.8 streams/s x 50 MB = 7.04k MB/s of 50GB/s = 50k MB/s: 14.08 %.
  const auto eu = check_line_of(youtube_text, "cdn_eu");
  REQUIRE(eu.contains("peak offered 7.04k MB/s (140.8/s) vs 50k MB/s"));
  REQUIRE(eu.contains("(14.1%, green)"));
  // original_store: 9.6 uploads/s x 300 MB = 2.88k MB/s of 5GB/s = 5k MB/s: 57.6 %.
  const auto original = check_line_of(youtube_text, "original_store");
  REQUIRE(original.contains("peak offered 2.88k MB/s (9.6/s) vs 5k MB/s"));
  REQUIRE(original.contains("(57.6%, green)"));
  // transcoded_store: only the cdns' misses reach it on paper, 157.82 streams/s (printed to four
  // digits, 157.8) x 50 MB = 7.891k MB/s of 10GB/s = 10k MB/s: at least 78.9 %, a lower bound
  // (the transcoders' writes come through queues the paper load does not follow yet).
  const auto transcoded = check_line_of(youtube_text, "transcoded_store");
  REQUIRE(transcoded.contains("peak offered 7.891k MB/s (157.8/s) vs 10k MB/s"));
  REQUIRE(transcoded.contains("(≥78.9%; load not propagated:"));

  const auto news = build_or_fail(
      load_or_fail(std::string{SIMSYS_SCENARIOS_DIR} + "/book/news-feed/scenario.yaml"));
  const auto news_text = simsys::report::render_explain(news);
  CAPTURE(news_text);
  // media_store: the cdn's 5 % misses, 324.3 reads/s x 200 KB = 64.86 MB/s of 1GB/s = 1k MB/s.
  REQUIRE(check_line_of(news_text, "media_store")
              .contains("peak offered 64.86 MB/s (324.3/s) vs 1k MB/s total capacity  "
                        "(6.49%, green)"));
}

// The 2b "not checked" note for capacities converted from bytes is gone from every shipped
// scenario, and so is the object-size assumption it rested on. (A kv_cluster's capacity still has
// its own "not checked" note until the ring is modelled, plan 2c Task 10: that one is a different
// reason and stays.)
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
  REQUIRE(check_line_of(text, "snowflake").contains("16.38M/s total capacity"));
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

namespace {
// The line that describes node `id` in explain's `nodes:` block: the first line that starts with
// the id (the nodes come before the links and the checks).
std::string node_line_of(const std::string& explain_text, const std::string& id) {
  const auto at = explain_text.find("\n  " + id + " ");
  REQUIRE(at != std::string::npos);
  return explain_text.substr(at + 1, explain_text.find('\n', at + 1) - at - 1);
}
}  // namespace

TEST_CASE("explain on book/youtube prints the stream size on the byte-sized cdn", "[scenario]") {
  const auto g = build_or_fail(load_scenario("/book/youtube/scenario.yaml"));
  const auto text = simsys::report::render_explain(g);
  CAPTURE(text);
  REQUIRE(node_line_of(text, "cdn_us").contains("sizes: stream 50MB"));
}
