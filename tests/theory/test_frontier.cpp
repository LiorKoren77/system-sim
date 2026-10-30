// Whole-simulation checks against closed-form arithmetic (spec §12).
// Spec entries pinned here: spec §8.4 (the URL frontier and per-host politeness; book ch. 9's web
// crawler), §19 entry 8's `politeness` on an external sink, phase 3 Task 18.
// This file: the frontier's drain cap, its queue lag, the crawl loop's fixed point and the sink's
// politeness cap. One of the files the theory tests are split into by topic; the scenario
// builders more than one of them needs live in tests/support/fixtures.cppm (module
// simsys.test_fixtures).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>

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
import simsys.test_fixtures;

// A note for test authors: `Approx(x).margin(m)` passes when the value is within the
// absolute margin m OR within Approx's default relative epsilon (about 1.19e-5), so a
// margin can only loosen a check, never tighten it. For a tight check use WithinAbs /
// WithinRel, which apply only the tolerance you give them.
using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using simsys::Rate;
using simsys::engine::Simulation;
using simsys::engine::StepRecord;
using simsys::graph::Graph;
using simsys::model::HitRatio;
using simsys::model::LinkSpec;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
using simsys::model::Step;
using simsys::model::StepCondition;
using simsys::model::Workload;
// Shared builders (tests/support/fixtures.cppm).
using simsys::test_support::explain_of;
using simsys::test_support::first_step;
using simsys::test_support::group_of;
using simsys::test_support::published;
using simsys::test_support::records_of;
using simsys::test_support::simulation_of;
// explain's lines (tests/support/explain_lines.hpp).
using simsys::test_support::check_line;
using simsys::test_support::node_line;

// ---- The URL frontier: a queue whose drain is capped by politeness (spec §8.4; book ch. 9) ----
//
// A crawler must not hammer one site: book ch. 9's "politeness" lets each host be fetched by one
// worker at a time, with a pause between two fetches. Seen from the whole crawler, that is a RATE
// CAP: if the crawl touches `hosts` different hosts and each may be called `politeness` times a
// second, the frontier can hand out at most
//   cap = hosts x politeness              (URLs/s; the model spreads the URLs evenly over the
//   hosts)
// URLs a second however many are waiting and however many workers are free. Its drain is
//   drain = min(pool capacity, hosts x politeness)         (pool capacity = workers / cost, Little)
// The frontier is a message queue in front of one consumer pool (the downloader), with a
// prioritiser (the book's front queues) at its door: `capacity` is the prioritiser's own rate, a
// service in front of the queue, so its room and its drops are a service's.
//
// Numbers, from one picture: seeds -> frontier -> downloader, every link zero-latency so what a
// step sends arrives in the same step; the downloader is 2000 workers at 500 ms a fetch
//   pool capacity = 2000 / 0.5 s = 4000 URLs/s        (Little's law: c / service time)
// and the frontier is 1000 hosts at 1/s each (cap 1000/s) with a prioritiser of 5k/s. Fed 1500/s
// the downloader is offered what the prioritiser passes, 1500/s, and drains the cap:
//   drain   = min(4000, 1000 x 1) = 1000/s
//   backlog = (1500 - 1000) x t = 500 t           (fluid: what arrives and is not drained waits)
//   lag     = backlog / drain = 500 t / 1000 = t / 2          (how long a URL queued NOW waits
//             until the pool reads it: the FIFO drain time, see test_queues_pools.cpp's "Queue
//             lag"); 30 s after a minute, 15 s after half of it. The stochastic part of the lag,
//             the pool's Erlang wait, is at most 25 ms here and the fluid part larger from the
//             first seconds on.
// (Node indices: seeds 0, frontier 1, downloader 2, then the extra nodes of each picture.)

namespace {
// seeds (`rate`/s of op `crawl`) -> frontier -> downloader, all links zero-latency, 60 s of 100 ms
// steps (the Scenario defaults).
Scenario crawl(double rate, NodeSpec frontier) {
  // C++20 designated initializers: `.name = ...` names each member being set, in declaration
  // order, and leaves the rest at their defaults. Replaces positional braces (which break silently
  // when members are reordered); a scenario built in code reads like the YAML it stands for.
  Scenario s{.name = "crawl", .version = 1, .duration = 60s};
  auto downloader = group_of("downloader", 2000, 500ms);
  downloader.consumes = "frontier";  // group_of drains a queue called "q"
  s.nodes = {
      NodeSpec{.id = "seeds",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{rate}}, .op_shares = {{"crawl", 1.0}}}},
      std::move(frontier), std::move(downloader)};
  s.links = {LinkSpec{.from = "seeds", .to = "frontier", .latency = 0s},
             LinkSpec{.from = "frontier", .to = "downloader", .latency = 0s}};
  return s;
}

// The shipped crawler's frontier: `hosts` hosts each fetched `per_host_per_s` times a second, a
// prioritiser of `prioritiser_per_s`, and a `then` hand-off to the downloader. The fields are set
// by assignment, not in a designated initializer, so the test does not depend on where
// NodeSpec declares `hosts`.
NodeSpec frontier_of(int hosts, double per_host_per_s, double prioritiser_per_s) {
  NodeSpec frontier{.id = "frontier",
                    .type = NodeType::frontier,
                    .capacity = Rate{prioritiser_per_s},
                    .then = {Step{.to = "downloader", .op = "fetch"}}};
  frontier.hosts = hosts;
  frontier.politeness = Rate{per_host_per_s};
  return frontier;
}

// The frontier's queue lag in `record`; the test fails when it reports none.
double frontier_lag_of(const StepRecord& record, const Graph& g) {
  const auto& lag = record.nodes[*g.index_of("frontier")].queue_lag_s;
  REQUIRE(lag.has_value());
  return *lag;
}
}  // namespace

TEST_CASE("a frontier over 1000 hosts at 1 request a host a second drains 1000 URLs a second",
          "[theory]") {
  const auto g = *Graph::build(crawl(1500, frontier_of(1000, 1.0, 5000)));
  const auto records = records_of(g, 600);  // 60 s
  const auto& last = records.back();
  const auto& downloader = last.nodes[*g.index_of("downloader")];
  REQUIRE_THAT(last.nodes[*g.index_of("frontier")].demand_per_s, WithinRel(1500.0, 1e-9));
  REQUIRE_THAT(downloader.demand_per_s, WithinRel(1500.0, 1e-9));  // the prioritiser passes it all
  REQUIRE_THAT(downloader.drain_per_s, WithinRel(1000.0, 1e-9));   // min(4000, 1000 x 1/s)
  REQUIRE(downloader.dropped_per_s == 0.0);  // the room is 7 days of the pool's rate: not full
  // backlog = (1500 - 1000) x t: 5000 after 10 s, 15000 after 30 s, 30000 after 60 s
  REQUIRE_THAT(records[99].nodes[*g.index_of("downloader")].backlog, WithinRel(5000.0, 1e-6));
  REQUIRE_THAT(records[299].nodes[*g.index_of("downloader")].backlog, WithinRel(15000.0, 1e-6));
  REQUIRE_THAT(downloader.backlog, WithinRel(30000.0, 1e-6));
}

TEST_CASE("the frontier's queue lag is its backlog over its drain: 30 s after a minute",
          "[theory]") {
  const auto g = *Graph::build(crawl(1500, frontier_of(1000, 1.0, 5000)));
  const auto records = records_of(g, 600);
  // 30,000 URLs waiting, drained 1000 a second: a URL queued at t = 60 s is read 30 s later.
  REQUIRE_THAT(frontier_lag_of(records[599], g), WithinRel(30000.0 / 1000.0, 1e-6));
  REQUIRE_THAT(frontier_lag_of(records[299], g), WithinRel(15000.0 / 1000.0, 1e-6));  // t = 30 s
  double previous = 0.0;
  for (const auto& record : records) {
    const double lag = frontier_lag_of(record, g);
    REQUIRE(lag >= previous);  // the inflow exceeds the cap for the whole run: it never falls
    previous = lag;
  }
}

TEST_CASE("a frontier fed less than hosts times politeness drains everything and queues nothing",
          "[theory]") {
  const auto g = *Graph::build(crawl(800, frontier_of(1000, 1.0, 5000)));
  const auto records = records_of(g, 100);
  const auto& downloader = records.back().nodes[*g.index_of("downloader")];
  REQUIRE_THAT(downloader.demand_per_s, WithinRel(800.0, 1e-9));
  REQUIRE_THAT(downloader.drain_per_s, WithinRel(800.0, 1e-9));  // 800 < the cap of 1000
  REQUIRE(downloader.backlog == 0.0);
  REQUIRE(frontier_lag_of(records.back(), g) < 0.1);  // only the pool's Erlang wait is left
}

// With 10M hosts the politeness cap is 10M x 1/s = 10M/s, far above the pool's 4000/s, so the
// pool binds: min(4000, 10M). Fed 5000/s, exactly the prioritiser's 5k/s (rho 1: it neither grows
// a room nor drops), the pool drains 4000 and the backlog grows by 1000 a second.
TEST_CASE("with 10 million hosts the pool's 4000 a second binds, not the politeness cap",
          "[theory]") {
  const auto g = *Graph::build(crawl(5000, frontier_of(10'000'000, 1.0, 5000)));
  const auto records = records_of(g, 100);  // 10 s
  const auto& last = records.back();
  const auto& downloader = last.nodes[*g.index_of("downloader")];
  REQUIRE_THAT(last.nodes[*g.index_of("frontier")].demand_per_s, WithinRel(5000.0, 1e-9));
  REQUIRE_THAT(downloader.demand_per_s, WithinRel(5000.0, 1e-9));
  REQUIRE_THAT(downloader.drain_per_s, WithinRel(4000.0, 1e-9));  // workers / cost
  REQUIRE_THAT(downloader.backlog, WithinRel((5000.0 - 4000.0) * 10.0, 1e-6));
  REQUIRE(last.nodes[*g.index_of("frontier")].dropped_per_s == 0.0);  // 5000 <= its 5k/s
}

// A prioritiser of 3k/s fed 5000/s is the first bottleneck: it passes 3000 a second (its drain)
// and drops the other 2000 once its room is full, as any service does; the pool, offered 3000 of
// its 4000, drains all of it and queues nothing. The settled state is estimate()'s.
TEST_CASE("a prioritiser of 3000 a second binds before the pool: it drains 3000 and drops 2000",
          "[theory]") {
  const auto g = *Graph::build(crawl(5000, frontier_of(10'000'000, 1.0, 3000)));
  const auto e = simulation_of(g).estimate();
  REQUIRE(e.converged);
  const auto& frontier = e.nodes[*g.index_of("frontier")];
  REQUIRE_THAT(frontier.demand_per_s, WithinRel(5000.0, 1e-9));
  REQUIRE_THAT(frontier.drain_per_s, WithinRel(3000.0, 1e-6));
  REQUIRE_THAT(frontier.dropped_per_s, WithinRel(2000.0, 1e-6));  // 5000 - 3000
  const auto& downloader = e.nodes[*g.index_of("downloader")];
  REQUIRE_THAT(downloader.demand_per_s, WithinRel(3000.0, 1e-6));  // what the prioritiser passed
  REQUIRE_THAT(downloader.drain_per_s, WithinRel(3000.0, 1e-6));
  REQUIRE(downloader.backlog == 0.0);
}

// Only a message queue has a lag, and a frontier is one: in a run and in estimate() its lag is
// evaluated (not empty); the seeds and the downloader report none.
TEST_CASE("queue_lag_s is present on a frontier and absent on the nodes around it", "[theory]") {
  const auto g = *Graph::build(crawl(1500, frontier_of(1000, 1.0, 5000)));
  const auto r = first_step(g);
  REQUIRE(r.nodes[*g.index_of("frontier")].queue_lag_s.has_value());
  REQUIRE_FALSE(r.nodes[*g.index_of("seeds")].queue_lag_s.has_value());
  REQUIRE_FALSE(r.nodes[*g.index_of("downloader")].queue_lag_s.has_value());
  const auto e = simulation_of(g).estimate();
  REQUIRE(e.nodes[*g.index_of("frontier")].queue_lag_s.has_value());
  REQUIRE_FALSE(e.nodes[*g.index_of("seeds")].queue_lag_s.has_value());
  REQUIRE_FALSE(e.nodes[*g.index_of("downloader")].queue_lag_s.has_value());
}

TEST_CASE("estimate settles a politeness-capped frontier at its cap of 1000 URLs a second",
          "[theory]") {
  const auto g = *Graph::build(crawl(1500, frontier_of(1000, 1.0, 5000)));
  const auto e = simulation_of(g).estimate();
  REQUIRE(e.converged);
  REQUIRE_THAT(e.nodes[*g.index_of("frontier")].demand_per_s, WithinRel(1500.0, 1e-9));
  REQUIRE_THAT(e.nodes[*g.index_of("downloader")].drain_per_s, WithinRel(1000.0, 1e-9));
}

// ---- The crawl loop: url_seen feeds the frontier back (book ch. 9, "URL seen?") ----
//
// Each page the downloader fetches is parsed, and its F links are looked up in url_seen, a cache
// that answers "seen already" with hit ratio h = 0.9; the misses (1 - h = 10 % of the links, the
// new URLs) go back to the frontier. A loop, so the frontier's inflow x solves
//   x = seeds + F x (1 - h) d              (d: the share of the loop's flow its chain delivers)
//   x = seeds / (1 - F (1 - h) d)          (geometric series: each lap returns F (1 - h) d of the
//                                           one before; valid while the loop gain g = F (1 - h) d
//                                           is below 1)
// With a lossless chain d = 1 (every capacity here is far above its load) and seeds 50/s:
//   F = 2:  g = 0.2, x = 50 / 0.8 = 62.5/s        F = 8:  g = 0.8, x = 50 / 0.2 = 250/s
//   F = 4:  g = 0.4, x = 50 / 0.6 = 83.33/s
// and the downloader drains the same x (4000/s of capacity, and the frontier's cap is 10M x 1/s).
// At F = 50, the book's links per page, g = 5 > 1: the loop feeds itself faster than anything
// drains it, so the flow climbs until a node saturates (below).
namespace {
// seeds (50/s) -> frontier (10M hosts at 1/s, prioritiser 5k/s) -> downloader (4000/s) -> extractor
// (a service: each page's `links_per_page` links) -> url_seen (a cache, hit ratio 0.9) -> back to
// the frontier on a miss. Every capacity but the pool's and the prioritiser's is far above the
// load. Node indices: seeds 0, frontier 1, downloader 2, extractor 3, url_seen 4.
Scenario crawl_loop(double links_per_page) {
  auto s = crawl(50, frontier_of(10'000'000, 1.0, 5000));
  s.nodes[2].then = {Step{.to = "extractor", .op = "parse"}};
  s.nodes.push_back(NodeSpec{
      .id = "extractor",
      .type = NodeType::service,
      .capacity = Rate{1e6},
      .latency = 1ms,
      .ops = {{"parse", {Step{.to = "url_seen", .op = "check", .factor = links_per_page}}}}});
  s.nodes.push_back(
      NodeSpec{.id = "url_seen",
               .type = NodeType::cache,
               .capacity = Rate{1e7},
               .latency = 1ms,
               .then = {Step{.to = "frontier", .op = "crawl", .on = StepCondition::miss}},
               .hit_ratio = HitRatio{.fixed = 0.9}});
  s.links.push_back(LinkSpec{.from = "downloader", .to = "extractor", .latency = 0s});
  s.links.push_back(LinkSpec{.from = "extractor", .to = "url_seen", .latency = 0s});
  s.links.push_back(LinkSpec{.from = "url_seen", .to = "frontier", .latency = 0s});
  return s;
}

// x = seeds / (1 - F (1 - h) d), for a lossless chain (d = 1): the frontier's inflow at the fixed
// point of the loop above.
double loop_fixed_point(double seeds, double links_per_page, double hit_ratio) {
  return seeds / (1.0 - links_per_page * (1.0 - hit_ratio));
}
}  // namespace

TEST_CASE("the crawl loop settles at seeds over one minus the loop gain: F = 8 gives 250 a second",
          "[theory]") {
  for (const double links : {2.0, 4.0, 8.0}) {
    CAPTURE(links);
    const auto g = *Graph::build(crawl_loop(links));
    const auto e = simulation_of(g).estimate();
    REQUIRE(e.converged);
    const double x = loop_fixed_point(50.0, links, 0.9);
    REQUIRE_THAT(e.nodes[*g.index_of("frontier")].demand_per_s, WithinRel(x, 1e-6));
    REQUIRE_THAT(e.nodes[*g.index_of("downloader")].drain_per_s, WithinRel(x, 1e-6));
    // url_seen is asked F links per page and passes the misses on: 10 % of F x
    REQUIRE_THAT(e.nodes[*g.index_of("url_seen")].demand_per_s, WithinRel(links * x, 1e-6));
  }
  REQUIRE_THAT(loop_fixed_point(50.0, 8.0, 0.9), WithinRel(250.0, 1e-12));
}

TEST_CASE("a run of the crawl loop at F = 8 reaches the same 250 a second as estimate",
          "[theory]") {
  const auto g = *Graph::build(crawl_loop(8));
  const auto records = records_of(g, 300);  // each lap of the loop is a step: 0.8^300 ~ 1e-29
  REQUIRE_THAT(records.back().nodes[*g.index_of("frontier")].demand_per_s, WithinRel(250.0, 1e-6));
  REQUIRE_THAT(records.back().nodes[*g.index_of("downloader")].drain_per_s, WithinRel(250.0, 1e-6));
  REQUIRE(records.back().nodes[*g.index_of("downloader")].backlog == 0.0);
}

// At F = 50 the gain is 50 x 0.1 = 5 > 1. A loop of gain above 1 grows each lap by that factor
// until a node on it saturates, and then it is steady: here the downloader drains only its 4000/s,
// the links its misses bring back are 4000 x 50 x 0.1 = 20,000/s (+ the seeds' 50), the frontier is
// offered 20,050/s, its prioritiser passes 5000 and drops 15,050, and the pool, offered 5000,
// drains its 4000 and falls behind by 1000 a second for ever. (A loop of gain EXACTLY 1 or a retry
// storm no capacity stops is what estimate() reports as "no steady state"; a loop of gain 5
// reaches a capacity within a handful of laps, so it settles. What `explain`, which has no
// capacities in its paper load, says about it is pinned below.)
TEST_CASE("at 50 links a page the loop saturates the pool: it drains 4000 and the frontier drops",
          "[theory]") {
  const auto g = *Graph::build(crawl_loop(50));
  const auto e = simulation_of(g).estimate();
  REQUIRE(e.converged);
  const auto& frontier = e.nodes[*g.index_of("frontier")];
  const auto& downloader = e.nodes[*g.index_of("downloader")];
  REQUIRE_THAT(downloader.drain_per_s, WithinRel(4000.0, 1e-9));  // the pool's cap binds
  REQUIRE_THAT(frontier.demand_per_s, WithinRel(50.0 + 4000.0 * 50.0 * 0.1, 1e-6));  // 20,050
  REQUIRE_THAT(frontier.drain_per_s, WithinRel(5000.0, 1e-6));      // the prioritiser's rate
  REQUIRE_THAT(frontier.dropped_per_s, WithinRel(15'050.0, 1e-6));  // 20,050 - 5000
}

TEST_CASE("a run of the crawl loop at F = 50 drains the pool's 4000 while its backlog grows",
          "[theory]") {
  const auto g = *Graph::build(crawl_loop(50));
  const auto records = records_of(g, 100);  // 10 s: the loop reaches the caps within ~6 steps
  const auto& last = records[99];
  const auto& downloader = last.nodes[*g.index_of("downloader")];
  REQUIRE_THAT(downloader.drain_per_s, WithinRel(4000.0, 1e-9));
  REQUIRE_THAT(last.nodes[*g.index_of("frontier")].demand_per_s, WithinRel(20'050.0, 1e-6));
  // (5000 offered - 4000 drained) a second x 0.1 s = 100 URLs more every step, for ever
  const double growth = downloader.backlog - records[98].nodes[*g.index_of("downloader")].backlog;
  REQUIRE_THAT(growth, WithinRel(100.0, 1e-6));
  REQUIRE(downloader.backlog > records[50].nodes[*g.index_of("downloader")].backlog);
}

// explain's paper load has no capacities to saturate: it sums the loop's geometric series, and a
// loop of gain >= 1 has no sum. At F = 8 (gain 0.8) the frontier is offered 250/s of its
// prioritiser's 5k/s: 5 %, and nothing is "not propagated" any more; at F = 50 the loop has no
// steady state and explain says so.
TEST_CASE("explain follows the frontier's then and sums the loop at F = 8: 250 a second, 5 percent",
          "[theory]") {
  const auto text = explain_of(crawl_loop(8));
  CAPTURE(text);
  REQUIRE(check_line(text, "frontier")
              .contains("peak offered 250/s vs 5k/s total capacity  (5%, green)"));
  // C++23 std::string::contains: a substring test in one call, replacing
  // `find(...) != std::string::npos`, so the assertion reads as what it checks.
  REQUIRE_FALSE(text.contains("load not propagated"));
  REQUIRE_FALSE(text.contains("no steady state"));
}

TEST_CASE("explain says a crawl loop of 50 links a page has no steady state", "[theory]") {
  const auto text = explain_of(crawl_loop(50));
  CAPTURE(text);
  REQUIRE(text.contains("no steady state"));
  REQUIRE(check_line(text, "frontier").contains("unbounded"));
  REQUIRE_FALSE(text.contains("load not propagated"));
}

// ---- An external sink's politeness: the same cap on the sink's drain (spec §8.4) ----
//
// The crawler's `internet` sink states `politeness: 1/s` too. The sink is the web, the hosts are
// the frontier's: its drain is capped at hosts x politeness, `hosts` taken from the ONE frontier of
// the scenario (a sink with no frontier, or with two, has no single host count to multiply: the
// scenario is refused). Here the frontier's own cap is out of the way (1000 hosts x 100/s =
// 100k/s), the downloader (4000/s) is fed 1500/s and calls the sink (5k/s of capacity, 300 ms) for
// each page; the sink's politeness 1/s over the frontier's 1000 hosts caps it at 1000/s:
//   sink drain = min(5000, 1000 x 1/s) = 1000/s        (the other 500/s are the sink's to shed)
// and the downloader is not slowed by it: a worker is busy for `cost` only, and a target that
// cannot keep up drops its own load without telling anyone upstream.
namespace {
// seeds (1500/s) -> frontier (1000 hosts, 100/s each, prioritiser 5k/s) -> downloader -> internet
// (an external sink of 5k/s with the given politeness per host, none when empty).
Scenario crawl_into_internet(std::optional<double> sink_politeness_per_s) {
  auto s = crawl(1500, frontier_of(1000, 100.0, 5000));
  s.nodes[2].then = {Step{.to = "internet", .op = "get"}};
  NodeSpec internet{.id = "internet", .type = NodeType::external_sink, .capacity = Rate{5000}};
  if (sink_politeness_per_s) internet.politeness = Rate{*sink_politeness_per_s};
  s.nodes.push_back(std::move(internet));
  s.links.push_back(LinkSpec{.from = "downloader", .to = "internet", .latency = 0s});
  return s;
}

// The message of whichever stage refuses `s`: Graph::build, or else Simulation::create; the test
// fails when neither does.
std::string refusal_of(const Scenario& s) {
  // C++23 std::expected<T, E>: a value-or-error return type; has_value() tells which one it
  // holds and error() gives the structured error. Replaces exceptions or out-parameters: the
  // caller must look at the result.
  const auto g = Graph::build(s);
  if (!g) return g.error().message;
  const auto sim = Simulation::create(*g);
  REQUIRE_FALSE(sim.has_value());
  return sim.error().message;
}
}  // namespace

TEST_CASE("an internet sink with politeness 1 a second over a frontier of 1000 hosts drains 1000",
          "[theory]") {
  const auto g = *Graph::build(crawl_into_internet(1.0));
  const auto e = simulation_of(g).estimate();
  REQUIRE(e.converged);
  const auto& internet = e.nodes[*g.index_of("internet")];
  REQUIRE_THAT(internet.demand_per_s, WithinRel(1500.0, 1e-9));  // every page is fetched
  REQUIRE_THAT(internet.drain_per_s, WithinRel(1000.0, 1e-9));   // min(5000, 1000 hosts x 1/s)
  // the pool is not slowed by the sink it calls
  REQUIRE_THAT(e.nodes[*g.index_of("downloader")].drain_per_s, WithinRel(1500.0, 1e-9));
  // and a run drains the same cap from its first step on
  const auto records = records_of(g, 50);
  REQUIRE_THAT(records.back().nodes[*g.index_of("internet")].drain_per_s, WithinRel(1000.0, 1e-9));
}

TEST_CASE("the same sink without politeness drains what it is offered: 1500 a second", "[theory]") {
  const auto g = *Graph::build(crawl_into_internet(std::nullopt));
  const auto e = simulation_of(g).estimate();
  REQUIRE_THAT(e.nodes[*g.index_of("internet")].drain_per_s, WithinRel(1500.0, 1e-9));
}

TEST_CASE("a sink with politeness and no frontier in the scenario is refused", "[theory]") {
  // u (100/s) -> q (a message queue) -> w (a pool) -> internet (a sink with politeness 1/s)
  auto pool = group_of("w", 10, 10ms);
  pool.then = {Step{.to = "internet", .op = "get"}};
  auto s = published(100, {std::move(pool)});
  NodeSpec internet{.id = "internet", .type = NodeType::external_sink};
  internet.politeness = Rate{1.0};
  s.nodes.push_back(std::move(internet));
  s.links.push_back(LinkSpec{.from = "w", .to = "internet", .latency = 0s});
  REQUIRE(refusal_of(s) ==
          "node 'internet': politeness needs exactly one frontier in the scenario (found 0)");
}

TEST_CASE("a sink with politeness and two frontiers in the scenario is refused", "[theory]") {
  auto s = crawl_into_internet(1.0);
  // a second lane: seeds -> frontier2 -> downloader2, beside the first
  auto second = frontier_of(1000, 100.0, 5000);
  second.id = "frontier2";
  second.then = {Step{.to = "downloader2", .op = "fetch"}};
  auto pool = group_of("downloader2", 2000, 500ms);
  pool.consumes = "frontier2";
  s.nodes.push_back(std::move(second));
  s.nodes.push_back(std::move(pool));
  s.links.push_back(LinkSpec{.from = "seeds", .to = "frontier2", .latency = 0s});
  s.links.push_back(LinkSpec{.from = "frontier2", .to = "downloader2", .latency = 0s});
  REQUIRE(refusal_of(s) ==
          "node 'internet': politeness needs exactly one frontier in the scenario (found 2)");
}

// ---- The shipped crawler states its frontier in typed fields ----

TEST_CASE("the shipped web crawler's frontier is 10M hosts at 1 request a host a second",
          "[theory]") {
  const auto loaded =
      simsys::config::load(std::string{SIMSYS_SCENARIOS_DIR} + "/book/web-crawler/scenario.yaml");
  INFO((loaded ? "" : loaded.error().message));
  REQUIRE(loaded.has_value());
  const auto frontier = loaded->nodes[1];
  REQUIRE(frontier.id == "frontier");
  REQUIRE(frontier.hosts.has_value());
  REQUIRE(*frontier.hosts == 10'000'000);  // "10M": the loader reads the SI suffix
  REQUIRE(frontier.politeness.has_value());
  REQUIRE_THAT(frontier.politeness->per_second, WithinRel(1.0, 1e-12));
}

// ---- The cap bounds the drain in front of the pool; the pool keeps its workers (Task 18 review)
// --
//
// Politeness limits how fast URLs are HANDED to the downloaders, not how many downloaders there
// are. The model is a token bucket in front of the pool, refilled at hosts x politeness tokens a
// second, one token per URL handed over; the pool behind it keeps its 2000 workers at 500 ms. So
//   drain = min(pool capacity, cap) = min(2000 / 0.5 s, 1000 x 1/s) = 1000 URLs/s
// and the pool's utilization is the share of its workers busy, by Little's law (busy workers =
// throughput x the time each job holds one):
//   rho = drain x cost / workers = 1000 x 0.5 / 2000 = 0.25           (at the cap)
//   rho = 900 x 0.5 / 2000 = 0.225                                     (at 900/s, below it)
// More workers lower rho and drain nothing more: the cap is what binds, and the utilization SLO
// sees the workers idle. (Until the review the engine shrank the pool to the cap, rate AND
// servers: it read rho 1 at the cap and 0.9 at 900/s while 500 of the 2000 workers were busy, so
// a utilization SLO failed for a reason that was not true.)
TEST_CASE("at the politeness cap the pool keeps its 2000 workers: rho 0.25, drain 1000, lag 30 s",
          "[theory]") {
  const auto g = *Graph::build(crawl(1500, frontier_of(1000, 1.0, 5000)));
  const auto records = records_of(g, 600);  // 60 s
  const auto& downloader = records.back().nodes[*g.index_of("downloader")];
  REQUIRE_THAT(downloader.drain_per_s, WithinRel(1000.0, 1e-9));
  REQUIRE_THAT(downloader.rho, WithinRel(0.25, 1e-9));          // 1000 x 0.5 s / 2000 workers
  REQUIRE_THAT(downloader.backlog, WithinRel(30'000.0, 1e-6));  // (1500 - 1000) x 60 s
  REQUIRE_THAT(frontier_lag_of(records.back(), g), WithinRel(30.0, 1e-6));  // 30k / 1000 a second
}

TEST_CASE("below the politeness cap at 900 a second the pool reads rho 0.225 and queues nothing",
          "[theory]") {
  const auto g = *Graph::build(crawl(900, frontier_of(1000, 1.0, 5000)));
  const auto records = records_of(g, 100);  // 10 s
  const auto& downloader = records.back().nodes[*g.index_of("downloader")];
  REQUIRE_THAT(downloader.drain_per_s, WithinRel(900.0, 1e-9));
  REQUIRE_THAT(downloader.rho, WithinRel(0.225, 1e-9));  // 900 x 0.5 s / 2000 workers
  REQUIRE(downloader.backlog == 0.0);
  const auto e = simulation_of(g).estimate();  // the long run agrees
  REQUIRE_THAT(e.nodes[*g.index_of("downloader")].rho, WithinRel(0.225, 1e-9));
}

TEST_CASE("twice the workers behind the same cap halve the pool's rho and drain no more",
          "[theory]") {
  auto s = crawl(1500, frontier_of(1000, 1.0, 5000));
  s.nodes[2] = group_of("downloader", 4000, 500ms);
  s.nodes[2].consumes = "frontier";
  const auto g = *Graph::build(s);
  const auto& downloader = records_of(g, 100).back().nodes[*g.index_of("downloader")];
  REQUIRE_THAT(downloader.drain_per_s, WithinRel(1000.0, 1e-9));  // still the cap
  REQUIRE_THAT(downloader.rho, WithinRel(0.125, 1e-9));           // 1000 x 0.5 s / 4000 workers
}

// With 10M hosts the cap (10M/s) is far above the pool's own 4000/s, so the pool binds with its
// real 2000 workers: every one of them busy, rho 1.
TEST_CASE("with 10 million hosts the pool's own 4000 a second binds and every worker is busy",
          "[theory]") {
  const auto g = *Graph::build(crawl(5000, frontier_of(10'000'000, 1.0, 5000)));
  const auto& downloader = records_of(g, 100).back().nodes[*g.index_of("downloader")];
  REQUIRE_THAT(downloader.drain_per_s, WithinRel(4000.0, 1e-9));
  REQUIRE_THAT(downloader.rho, WithinRel(1.0, 1e-9));  // 4000 x 0.5 s / 2000 workers
}

// ---- A frontier deletes nothing: its pool's room is unbounded (Task 18 review) ----
//
// A message queue deletes a message older than its retention, so its pool's room is retention x
// the pool's rate and what overflows it is lost. A frontier has no retention: a URL waits until a
// downloader takes it. So its pool's room is unbounded, and `estimate`, which jumps to the long
// run, cannot fill it: offered 1500/s against a drain of 1000/s, the backlog grows by 500 URLs
// every second for ever, and the long-run lag has no figure. estimate says so (an infinite lag and
// backlog, printed "unbounded") and drops nothing, where a full room of 7 days of draining used to
// drop the 500/s it could not hold and report a lag of 168 h.
TEST_CASE(
    "estimate of a frontier offered more than its drain reports an unbounded lag and no drops",
    "[theory]") {
  const auto g = *Graph::build(crawl(1500, frontier_of(1000, 1.0, 5000)));
  const auto e = simulation_of(g).estimate();
  REQUIRE(e.converged);
  const auto& downloader = e.nodes[*g.index_of("downloader")];
  REQUIRE_THAT(downloader.drain_per_s, WithinRel(1000.0, 1e-9));
  REQUIRE(downloader.dropped_per_s == 0.0);  // nothing is deleted unconsumed
  REQUIRE(std::isinf(downloader.backlog));
  const auto& lag = e.nodes[*g.index_of("frontier")].queue_lag_s;
  REQUIRE(lag.has_value());
  REQUIRE(std::isinf(*lag));
}

TEST_CASE("estimate of a frontier fed below its drain reports a finite lag and no backlog",
          "[theory]") {
  const auto g = *Graph::build(crawl(900, frontier_of(1000, 1.0, 5000)));
  const auto e = simulation_of(g).estimate();
  REQUIRE(e.nodes[*g.index_of("downloader")].backlog == 0.0);
  const auto& lag = e.nodes[*g.index_of("frontier")].queue_lag_s;
  REQUIRE(lag.has_value());
  REQUIRE(*lag < 0.1);  // the pool's Erlang wait alone
}

TEST_CASE("estimate's node table prints the frontier's lag and its pool's backlog as unbounded",
          "[theory]") {
  const auto g = *Graph::build(crawl(1500, frontier_of(1000, 1.0, 5000)));
  const auto summary = simsys::report::Summary::of_estimate(g, simulation_of(g).estimate());
  const auto table = simsys::report::render_table(summary, {}, false);
  CAPTURE(table);
  REQUIRE(table.contains("unbounded"));
  REQUIRE_FALSE(table.contains("unavailable"));
  REQUIRE(simsys::report::format_lag(std::numeric_limits<double>::infinity()) == "unbounded");
}

TEST_CASE("explain prints the room of a frontier's pool as unbounded", "[theory]") {
  const auto text = explain_of(crawl(1500, frontier_of(1000, 1.0, 5000)));
  CAPTURE(text);
  REQUIRE(
      node_line(text, "downloader").contains("queue_limit=unbounded (a frontier deletes nothing)"));
}

// ---- explain checks a frontier against what it can hand on: min(prioritiser, cap) ----
//
// The frontier's backlog grows when it is offered more than it can hand to its pool, and that is
// the smaller of its prioritiser's capacity and the politeness cap:
//   1500/s offered vs min(5000, 1000 hosts x 1/s) = 1000/s: 150 %, red (the backlog grows by 500/s)
//   800/s offered vs 1000/s: 80 %, yellow
// The check used to compare against the prioritiser alone (1500 of 5000: 30 %, green) while the
// backlog grew without bound. With the cap above the prioritiser (10M hosts) the line is the plain
// "total capacity" one (the crawl loop's test above pins it).
TEST_CASE("explain checks a frontier against its drain cap when the cap is below its prioritiser",
          "[theory]") {
  const auto over = explain_of(crawl(1500, frontier_of(1000, 1.0, 5000)));
  CAPTURE(over);
  REQUIRE(check_line(over, "frontier")
              .contains("peak offered 1.5k/s vs 1k/s drain cap (1k hosts x 1/s, below the "
                        "prioritiser's 5k/s)  (150%, red)"));
  const auto under = explain_of(crawl(800, frontier_of(1000, 1.0, 5000)));
  CAPTURE(under);
  REQUIRE(check_line(under, "frontier").contains("(80%, yellow)"));
}

// ---- What a frontier refuses (Task 18 review) ----
//
// Two consumer pools on one frontier would each be handed every URL (a frontier copies to its
// `then` steps, as a message queue copies to its groups) and each would get the whole politeness
// budget: twice the fetches per host the cap allows. A crawl has one bank of downloaders; a second
// pool is refused, naming both.
TEST_CASE("a frontier consumed by two pools is refused, naming both pools", "[theory]") {
  auto s = crawl(1500, frontier_of(1000, 1.0, 5000));
  auto second = group_of("downloader2", 2000, 500ms);
  second.consumes = "frontier";
  s.nodes[1].then.push_back(Step{.to = "downloader2", .op = "fetch"});
  s.nodes.push_back(std::move(second));
  s.links.push_back(LinkSpec{.from = "frontier", .to = "downloader2", .latency = 0s});
  REQUIRE(refusal_of(s) ==
          "node 'frontier': a frontier hands its URLs to one consumer pool "
          "('downloader' and 'downloader2' consume it)");
}

// A sink's politeness multiplies the frontier's `hosts`: over a frontier that states none there is
// nothing to multiply, and the sink would silently cap nothing. Refused like a scenario without a
// frontier, and explain's sink line says why.
TEST_CASE("a sink with politeness over a frontier without hosts is refused", "[theory]") {
  auto s = crawl_into_internet(1.0);
  s.nodes[1].hosts.reset();
  REQUIRE(refusal_of(s) ==
          "node 'internet': politeness needs the frontier's hosts ('frontier' states none)");
  const auto text = explain_of(s);
  CAPTURE(text);
  REQUIRE(node_line(text, "internet")
              .contains("politeness=1/s (the frontier states no hosts: run refuses it)"));
}

// The sink's cap is a bound in front of it too: its queue keeps the provider's 5k/s, so draining
// the 1000/s the cap lets through it reads rho 1000 / 5000 = 0.2, not 1.
TEST_CASE("an internet sink capped at 1000 a second by politeness reads rho 0.2 of its 5k/s",
          "[theory]") {
  const auto g = *Graph::build(crawl_into_internet(1.0));
  const auto e = simulation_of(g).estimate();
  const auto& internet = e.nodes[*g.index_of("internet")];
  REQUIRE_THAT(internet.drain_per_s, WithinRel(1000.0, 1e-9));
  REQUIRE_THAT(internet.rho, WithinRel(0.2, 1e-9));
}
