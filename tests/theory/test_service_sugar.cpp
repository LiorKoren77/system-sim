// Whole-simulation checks against closed-form queueing theory (spec §12).
// Spec entries pinned here: §18 entry 12 and §19 entry 10 (the id generator and the search index as
// service sugar).
// This file: the id generator and the search index, two node types that are a service with their
// own defaults (service sugar). One of the nine files the theory tests are split into by topic; the
// scenario builders more than one of them needs live in tests/support/fixtures.cppm (module
// simsys.test_fixtures).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <utility>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.core;
import simsys.engine;
import simsys.graph;
import simsys.model;
import simsys.queueing;
import simsys.report;

// A note for test authors: `Approx(x).margin(m)` passes when the value is within the
// absolute margin m OR within Approx's default relative epsilon (about 1.19e-5), so a
// margin can only loosen a check, never tighten it. For a tight check use WithinAbs /
// WithinRel, which apply only the tolerance you give them.
using namespace std::chrono_literals;
using Catch::Matchers::WithinRel;
using simsys::Rate;
using simsys::engine::Simulation;
using simsys::graph::Graph;
using simsys::model::CacheStrategy;
using simsys::model::Direction;
using simsys::model::HitRatio;
using simsys::model::IdAlgorithm;
using simsys::model::LinkSpec;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
using simsys::model::Workload;

// ---- ID generator as service sugar (spec §18 entry 12; book ch. 7) ----
//
// An id_generator runs exactly as a service whose capacity and service time the algorithm
// supplies (Graph::build). So the numbers below are a service's: the fluid drain, and the M/M/c
// wait of Erlang C (queueing::mmc_wait: the second argument is the per-server utilization rho).

namespace {
// u (`demand`/s of `next_id`) -> ids (an id_generator): zero-latency link, so the client's
// response time is the generator's own sojourn. Capacity, latency and queue limit only when given.
// Node indices: u 0, ids 1.
Scenario id_chain(simsys::model::IdAlgorithm algorithm, double demand, int replicas = 1,
                  std::optional<Rate> capacity = {}, std::optional<simsys::SimTime> latency = {},
                  std::optional<double> queue_limit = {}) {
  // C++20 designated initializers: `.name = ...` names each member being set, in
  // declaration order, and leaves the rest at their defaults. Replaces positional braces
  // (which break silently when members are reordered) or a default object assigned field
  // by field; here a scenario reads like the YAML it stands for.
  Scenario s{.name = "id-chain", .version = 1, .duration = 10s, .warmup = 2s};
  NodeSpec ids{.id = "ids",
               .type = NodeType::id_generator,
               .replicas = replicas,
               .capacity = capacity,
               .latency = latency,
               .advanced = {.queue_limit = queue_limit}};
  ids.id_algorithm = algorithm;
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{demand}},
                                           .op_shares = {{"next_id", 1.0}}}},
             std::move(ids)};
  s.links = {LinkSpec{.from = "u", .to = "ids", .dir = Direction::inout, .latency = 0s}};
  return s;
}
}  // namespace

// 10k ids/s on one snowflake machine, no numbers stated:
//   capacity 4096 ids/ms x 1000 = 4.096M/s, service time 10 us, so rho = 10k / 4.096M = 0.00244
//   and c = ceil(4.096M/s x 10 us) = ceil(40.96) = 41 servers (Little's law, defaults).
// Erlang C at 41 servers and rho = 0.00244 is P(wait) of the order (c rho)^c / c! = 0.1^41 / 41!
// ~ 3e-91: nobody
// ever waits. The sojourn is the service time, 10 us; the wait is nothing (NOT 10 us as the plan
// text puts it: that would take a rho near 0.5).
TEST_CASE("a snowflake at 10k ids per second is almost idle and answers in its 10 us", "[theory]") {
  const auto g = *Graph::build(id_chain(IdAlgorithm::snowflake, 10'000));
  const auto sim = Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
  const auto r = (*Simulation::create(g)).step(0s, 100ms);
  const auto& ids = r.nodes[*g.index_of("ids")];
  const double rho = 10'000.0 / 4'096'000.0;
  REQUIRE_THAT(ids.rho, WithinRel(rho, 1e-9));
  const double sojourn = simsys::queueing::mmc_wait(41, rho, 10e-6).mean_s + 10e-6;
  REQUIRE_THAT(ids.latency.mean_s, WithinRel(sojourn, 1e-9));
  REQUIRE_THAT(ids.latency.mean_s, WithinRel(10e-6, 1e-6));  // the napkin number: no queue
  REQUIRE(ids.dropped_per_s == 0.0);
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(sojourn, 1e-9));  // link latency is 0
}

// "Sugar": the generator is a service with the algorithm's numbers, nothing more. The same
// scenario written with a `service` of 4.096M/s and 10 us gives identical samples.
TEST_CASE("an id generator steps exactly as the service its algorithm stands for", "[theory]") {
  const auto as_generator = *Graph::build(id_chain(IdAlgorithm::snowflake, 10'000));
  auto as_service_scenario = id_chain(IdAlgorithm::snowflake, 10'000);
  as_service_scenario.nodes[1] = NodeSpec{
      .id = "ids", .type = NodeType::service, .capacity = Rate{4'096'000}, .latency = 10us};
  const auto as_service = *Graph::build(std::move(as_service_scenario));
  const auto a = (*Simulation::create(as_generator)).step(0s, 100ms);
  const auto b = (*Simulation::create(as_service)).step(0s, 100ms);
  REQUIRE(a.nodes[1].rho == b.nodes[1].rho);
  REQUIRE(a.nodes[1].latency.mean_s == b.nodes[1].latency.mean_s);
  REQUIRE(a.nodes[1].drain_per_s == b.nodes[1].drain_per_s);
  REQUIRE(a.responses[0].response.mean_s == b.responses[0].response.mean_s);
}

// More replicas share the ids: 3 snowflake machines at 12.288M/s total take 10k/s at
// rho = 10k / (3 x 4.096M) = 8.14e-4 each (the capacity is per replica).
TEST_CASE("a snowflake's replicas split the load: rho is demand over replicas times capacity",
          "[theory]") {
  const auto g = *Graph::build(id_chain(IdAlgorithm::snowflake, 10'000, 3));
  const auto r = (*Simulation::create(g)).step(0s, 100ms);
  REQUIRE_THAT(r.nodes[*g.index_of("ids")].rho, WithinRel(10'000.0 / (3 * 4'096'000.0), 1e-9));
}

// The lab's ticket server: one database of 8k/s (queue limit 1000) offered 10k ids/s.
//   offered 10k/s, capacity 8k/s: the queue fills (1000 / (10k - 8k) = 0.5 s), then in steady
//   state the node serves 8k/s and drops the rest:
//     dropped = 10k - 8k = 2k/s, drop rate = 2k / 10k = 0.2, rho = 1 (saturated).
// The client is answered only what was served: 8k/s.
TEST_CASE("a ticket server offered more than its capacity drops the excess: 2k of 10k ids a second",
          "[theory]") {
  const auto s = id_chain(IdAlgorithm::ticket_server, 10'000, 1, Rate{8000}, 1ms, 1000.0);
  const auto g = *Graph::build(s);
  const auto sim = Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
  const auto e = (*Simulation::create(g)).estimate();
  const auto& ids = e.nodes[*g.index_of("ids")];
  REQUIRE_THAT(ids.drain_per_s, WithinRel(8000.0, 1e-6));
  REQUIRE_THAT(ids.dropped_per_s, WithinRel(2000.0, 1e-6));
  REQUIRE_THAT(ids.rho, WithinRel(1.0, 1e-9));
  REQUIRE_THAT(e.responses[0].answered.per_second, WithinRel(8000.0, 1e-6));
  // The run agrees once the queue has filled (0.5 s, inside the 2 s warmup): the node's drop
  // rate, Σ dropped / Σ demand, is 0.2.
  simsys::report::Summary summary{g, s.warmup};
  (*Simulation::create(g)).run({.duration = s.duration, .step = s.step}, [&](const auto& r) {
    summary.observe(r);
  });
  REQUIRE_THAT(summary.drop_rate("ids"), WithinRel(0.2, 1e-6));
}

// A ticket server with no numbers stated is one SQL box: 1k writes/s (spec §6). 1500 ids/s
// overflow it by 500/s, a third of the load.
TEST_CASE("a ticket server with the algorithm's default capacity saturates at 1k ids a second",
          "[theory]") {
  const auto g = *Graph::build(id_chain(IdAlgorithm::ticket_server, 1500));
  const auto e = (*Simulation::create(g)).estimate();
  REQUIRE_THAT(e.nodes[*g.index_of("ids")].drain_per_s, WithinRel(1000.0, 1e-6));
  REQUIRE_THAT(e.nodes[*g.index_of("ids")].dropped_per_s, WithinRel(500.0, 1e-6));
}

// ---- Search index as service sugar (spec §19 entry 10; book ch. 13) ----
//
// A search_index runs exactly as a service of the same keys: no steps, the fluid drain and the
// M/M/c wait of Erlang C (queueing::mmc_wait). The scenario is search-autocomplete's trie, cut
// to its core: u -> trie_cache (read_through) -> trie_db.

namespace {
// u (60k/s of `suggest`) -> trie_cache (fixed hit ratio 0.9, read_through, capacity 100k/s) ->
// the trie: `trie_type` (a search_index, or the service it must equal) with 4 replicas, 2k/s each
// and 5 ms, all links with zero latency so the client's response is the nodes' own sojourns.
// Node indices: u 0, trie_cache 1, trie_db 2.
Scenario search_chain(NodeType trie_type) {
  Scenario s{.name = "search-chain", .version = 1, .duration = 10s, .warmup = 2s};
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{60'000}},
                                           .op_shares = {{"suggest", 1.0}}}},
             NodeSpec{.id = "trie_cache",
                      .type = NodeType::cache,
                      .capacity = Rate{100'000},
                      .latency = 250us,
                      .backend = "trie_db",
                      .hit_ratio = HitRatio{.fixed = 0.9},
                      .strategy = CacheStrategy::read_through},
             NodeSpec{.id = "trie_db",
                      .type = trie_type,
                      .replicas = 4,
                      .capacity = Rate{2000},
                      .latency = 5ms}};
  s.links = {
      LinkSpec{.from = "u", .to = "trie_cache", .dir = Direction::inout, .latency = 0s},
      LinkSpec{.from = "trie_cache", .to = "trie_db", .dir = Direction::inout, .latency = 0s}};
  return s;
}
}  // namespace

// The index behind the cache is offered the misses: (1 - 0.9) x 60k = 6k/s over 4 copies of 2k/s,
// per-copy rho = 6000 / 8000 = 0.75, with c = 2000/s x 5 ms = 10 servers per copy (Little's law).
// Erlang C at c = 10 and rho = 0.75 gives its wait; the sojourn is that plus the 5 ms service.
TEST_CASE(
    "a search index behind a read_through cache answers with the Erlang C sojourn of its copies",
    "[theory]") {
  const auto g = *Graph::build(search_chain(NodeType::search_index));
  const auto sim = Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
  const auto r = (*Simulation::create(g)).step(0s, 100ms);
  const auto& index = r.nodes[*g.index_of("trie_db")];
  REQUIRE_THAT(index.demand_per_s, WithinRel(6000.0, 1e-9));
  REQUIRE_THAT(index.rho, WithinRel(0.75, 1e-9));
  const double sojourn = simsys::queueing::mmc_wait(10, 0.75, 5e-3).mean_s + 5e-3;
  REQUIRE_THAT(index.latency.mean_s, WithinRel(sojourn, 1e-9));
  REQUIRE(index.dropped_per_s == 0.0);
}

// "Sugar": the same scenario written with a `service` of the same keys gives identical samples,
// bit for bit (== on doubles, not a tolerance): the engine runs one behaviour for both.
TEST_CASE("a search index steps exactly as the service of the same keys", "[theory]") {
  const auto as_index = *Graph::build(search_chain(NodeType::search_index));
  const auto as_service = *Graph::build(search_chain(NodeType::service));
  auto index_created = Simulation::create(as_index);
  INFO((index_created ? "" : index_created.error().message));
  REQUIRE(index_created.has_value());  // checked before the dereference below, so a refusal fails
  auto service_created = Simulation::create(as_service);
  REQUIRE(service_created.has_value());
  auto& index_sim = *index_created;
  auto& service_sim = *service_created;
  for (int i = 0; i < 5; ++i) {
    const auto a = index_sim.step(i * 100ms, 100ms);
    const auto b = service_sim.step(i * 100ms, 100ms);
    for (std::size_t node = 0; node < a.nodes.size(); ++node) {
      CAPTURE(i, node);
      REQUIRE(a.nodes[node].demand_per_s == b.nodes[node].demand_per_s);
      REQUIRE(a.nodes[node].rho == b.nodes[node].rho);
      REQUIRE(a.nodes[node].drain_per_s == b.nodes[node].drain_per_s);
      REQUIRE(a.nodes[node].latency.mean_s == b.nodes[node].latency.mean_s);
      REQUIRE(a.nodes[node].latency.variance_s2 == b.nodes[node].latency.variance_s2);
    }
    REQUIRE(a.responses[0].response.mean_s == b.responses[0].response.mean_s);
    REQUIRE(a.responses[0].answered.per_second == b.responses[0].answered.per_second);
  }
}
