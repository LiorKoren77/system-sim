// Whole-simulation checks against closed-form queueing theory (spec §12).
// Spec entries pinned here: §18 entries 6-10 (the SQL database, its write cost, connection pool and
// staleness, the CDN).
// This file: a SQL database (a primary and read replicas) and the request-based CDN. One of the
// nine files the theory tests are split into by topic; the scenario builders more than one of them
// needs live in tests/support/fixtures.cppm (module simsys.test_fixtures).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.core;
import simsys.engine;
import simsys.graph;
import simsys.model;
import simsys.queueing;
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
using simsys::graph::Graph;
using simsys::model::HitRatio;
using simsys::model::LinkSpec;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
using simsys::model::Workload;
// Shared builders (tests/support/fixtures.cppm).
using simsys::test_support::mm1;

// ---- SQL database: a primary and read replicas (spec §18 entry 6, book ch. 1) ----

namespace {
// u(reads + writes per second) -> db, a SQL database of `replicas` copies (one primary plus
// replicas - 1 read replicas) in `shards` shards. Every copy has capacity 1000/s and a 1 ms
// service time, the link has zero latency, so a response is the database's own sojourn.
// Node indices: u 0, db 1. An op with no load is left out of the client's mix.
// `pool` and `replication_lag` are set only when given.
// A `write` costs 1 capacity unit here (stated in `cost`), not the database default of 5 (spec
// §6: "SQL 5k reads" vs "1k writes" per second): every derivation below then has one unit per
// query, so its rates are requests. The default has a test of its own (it clears the `cost`).
Scenario sql_front(int replicas, std::optional<int> shards, double reads, double writes,
                   std::optional<int> pool = std::nullopt,
                   std::optional<simsys::SimTime> replication_lag = std::nullopt) {
  // C++20 designated initializers: `.name = ...` names each member being set, in
  // declaration order, and leaves the rest at their defaults. Replaces positional braces
  // (which break silently when members are reordered) or a default object assigned field
  // by field; here a scenario reads like the YAML it stands for.
  Scenario s{.name = "sql-front", .version = 1, .duration = 10s};
  std::vector<std::pair<std::string, double>> shares;
  if (reads > 0) shares.emplace_back("read", reads / (reads + writes));
  if (writes > 0) shares.emplace_back("write", writes / (reads + writes));
  NodeSpec db{.id = "db",
              .type = NodeType::sql_db,
              .replicas = replicas,
              .capacity = Rate{1000},
              .latency = 1ms};
  db.shards = shards;
  db.pool = pool;
  db.replication_lag = replication_lag;
  db.cost = {{"write", 1.0}};
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{reads + writes}},
                                           .op_shares = std::move(shares)}},
             std::move(db)};
  s.links = {LinkSpec{.from = "u", .to = "db", .latency = 0s}};
  return s;
}
// The mean response the client sees for `op` in a one-step run of `s`.
double client_mean_s(const simsys::engine::StepRecord& r, std::string_view op) {
  // C++20 std::ranges::find with a projection: compares `op` with each sample's `op` member
  // (the projection, a pointer to member), replacing std::find_if over begin/end with a lambda
  // that reads the member.
  const auto it = std::ranges::find(r.responses, op, &simsys::engine::ResponseSample::op);
  REQUIRE(it != r.responses.end());
  return it->response.mean_s;
}
}  // namespace

// Book ch. 1 (database replication): writes go to the primary, reads to the replicas, and each
// replica also applies every write it is sent by the primary. With 3 copies that is 1 primary
// (100 writes/s: rho 0.1) and 2 replicas, each with 1000 / 2 reads + 100 replicated writes =
// 600 units/s (rho 0.6); both are independent M/M/1 queues (M/M/c with c = 1, `mmc_wait`).
// The node reports the busier queue's rho, and each op sees the sojourn of its own queue.
TEST_CASE(
    "writes go to the primary and reads to the replicas, each replica also applying every write",
    "[theory]") {
  const auto r =
      (*Simulation::create(*Graph::build(sql_front(3, std::nullopt, 1000, 100)))).step(0s, 100ms);
  const auto& db = r.nodes[1];
  REQUIRE_THAT(db.demand_per_s, WithinRel(1100.0, 1e-9));
  REQUIRE_THAT(db.rho, WithinRel(0.6, 1e-9));  // max(primary 0.1, replica 0.6)
  const auto replica = simsys::queueing::mmc_wait(1, 0.6, 0.001);
  const auto primary = simsys::queueing::mmc_wait(1, 0.1, 0.001);
  REQUIRE_THAT(client_mean_s(r, "read"), WithinRel(replica.mean_s + 0.001, 1e-9));
  REQUIRE_THAT(client_mean_s(r, "write"), WithinRel(primary.mean_s + 0.001, 1e-9));
}

// With one copy there are no replicas: the primary serves reads and writes alike, one M/M/1
// queue at (500 + 100) / 1000 = 0.6, and both ops see its sojourn.
TEST_CASE("with one replica everything is on the primary", "[theory]") {
  const auto r =
      (*Simulation::create(*Graph::build(sql_front(1, std::nullopt, 500, 100)))).step(0s, 100ms);
  REQUIRE_THAT(r.nodes[1].rho, WithinRel(0.6, 1e-9));
  const auto shared = simsys::queueing::mmc_wait(1, 0.6, 0.001);
  REQUIRE_THAT(client_mean_s(r, "read"), WithinRel(shared.mean_s + 0.001, 1e-9));
  REQUIRE_THAT(client_mean_s(r, "write"), WithinRel(shared.mean_s + 0.001, 1e-9));
}

// Sharding (book ch. 1, horizontal scaling): the data is split over S independent sets of the
// same shape, so each shard's primary and replicas see 1/S of the load. Here each replica sees
// (1000 / 2 + 100) / 2 = 300 units/s per shard: rho 0.3 (the primary: 100 / 2 / 1000 = 0.05).
TEST_CASE("shards divide both loads", "[theory]") {
  const auto r = (*Simulation::create(*Graph::build(sql_front(3, 2, 1000, 100)))).step(0s, 100ms);
  REQUIRE_THAT(r.nodes[1].rho, WithinRel(0.3, 1e-9));
  const auto replica = simsys::queueing::mmc_wait(1, 0.3, 0.001);
  const auto primary = simsys::queueing::mmc_wait(1, 0.05, 0.001);
  REQUIRE_THAT(client_mean_s(r, "read"), WithinRel(replica.mean_s + 0.001, 1e-9));
  REQUIRE_THAT(client_mean_s(r, "write"), WithinRel(primary.mean_s + 0.001, 1e-9));
}

// A replica applies a write `replication_lag` after the primary has finished it, and then waits
// its turn in the replica's own queue behind the reads and the other replayed writes, so data a
// replica serves can be as old as: the primary's sojourn (wait + service) + the lag + the
// replica's sojourn. With 3 copies, 1000 reads/s and 100 writes/s the primary runs at rho 0.1
// and each replica at (500 + 100) / 1000 = 0.6 (M/M/1 waits, `mmc_wait` with c = 1). With no
// replica nothing is stale.
TEST_CASE("staleness is the replication lag plus the write's sojourn at the primary and a replica",
          "[theory]") {
  SECTION("three copies: 50 ms lag, the primary at rho 0.1, the replicas at 0.6") {
    const auto r = (*Simulation::create(
                        *Graph::build(sql_front(3, std::nullopt, 1000, 100, std::nullopt, 50ms))))
                       .step(0s, 100ms);
    REQUIRE(r.nodes[1].staleness_s.has_value());
    const auto primary = simsys::queueing::mmc_wait(1, 0.1, 0.001);
    const auto replica = simsys::queueing::mmc_wait(1, 0.6, 0.001);
    REQUIRE_THAT(*r.nodes[1].staleness_s,
                 WithinRel(0.050 + primary.mean_s + 0.001 + replica.mean_s + 0.001, 1e-9));
  }
  SECTION("one copy: nothing is stale") {
    const auto r = (*Simulation::create(
                        *Graph::build(sql_front(1, std::nullopt, 500, 100, std::nullopt, 50ms))))
                       .step(0s, 100ms);
    REQUIRE(r.nodes[1].staleness_s.value_or(0.0) == 0.0);
  }
  SECTION("a node that is not a database has no staleness") {
    const auto r = (*Simulation::create(*Graph::build(mm1(500)))).step(0s, 100ms);
    REQUIRE_FALSE(r.nodes[1].staleness_s.has_value());
  }
}

// The two queues a write crosses on its way to a reader both count, with their waits. 3 copies, 200
// reads/s and 800 writes/s: the primary runs at 800 / 1000 = 0.8, so a write waits rho / (1 − rho)
// × s = 4 ms there (M/M/1), four times its 1 ms service; each replica applies the 800 writes plus
// its 100 reads, rho 0.9, and waits 9 ms. Staleness = 50 ms lag + (4 + 1) ms at the primary + (9 +
// 1) ms at the replica = 65 ms.
TEST_CASE("staleness counts the wait in the primary's write queue and the replica's sojourn",
          "[theory]") {
  const auto r =
      (*Simulation::create(*Graph::build(sql_front(3, std::nullopt, 200, 800, std::nullopt, 50ms))))
          .step(0s, 100ms);
  const auto primary = simsys::queueing::mmc_wait(1, 0.8, 0.001);
  const auto replica = simsys::queueing::mmc_wait(1, 0.9, 0.001);
  REQUIRE(r.nodes[1].staleness_s.has_value());
  REQUIRE_THAT(*r.nodes[1].staleness_s,
               WithinRel(0.050 + primary.mean_s + 0.001 + replica.mean_s + 0.001, 1e-9));
  REQUIRE_THAT(*r.nodes[1].staleness_s, WithinRel(0.065, 1e-9));
}

// A replica replays the primary's log, and the log holds only the writes the primary COMMITTED:
// what the primary dropped never reaches a replica. 2 copies, 100 reads/s + 1500 writes/s, 1000/s
// per copy, settled (estimate: both queues full). The primary drains 1000 writes/s and drops 500.
// The replica is sent its 100 reads + the 1000 committed writes = 1100/s against 1000/s: it drops
// 100/s in its queue's mix, 100 × 100/1100 reads (the clients' part, in the node's drops) and 100 ×
// 1000/1100 replicated writes (reported apart, `replication_dropped_per_s`). Every write costs 1
// here (the fixture), so units are requests.
TEST_CASE("replicas replay only the writes the primary committed", "[theory]") {
  const auto r =
      (*Simulation::create(*Graph::build(sql_front(2, std::nullopt, 100, 1500)))).estimate();
  REQUIRE(r.converged);
  REQUIRE_THAT(r.nodes[1].dropped_per_s, WithinRel(500.0 + 100.0 * 100.0 / 1100.0, 1e-6));
  REQUIRE(r.nodes[1].replication_dropped_per_s.has_value());
  REQUIRE_THAT(*r.nodes[1].replication_dropped_per_s, WithinRel(100.0 * 1000.0 / 1100.0, 1e-6));
  // What the replica must replay is what the primary committed, 1000/s, not the 1500/s offered.
  REQUIRE_THAT(r.nodes[1].committed_writes_per_s.value_or(-1.0), WithinRel(1000.0, 1e-6));
}

// The balance demand·dt = (drain + dropped)·dt + Δbacklog holds for the CLIENTS' queries even while
// a replica is overloaded by replication. 2 copies, 800 reads/s + 400 writes/s: the primary commits
// all 400 writes (rho 0.4); the replica is sent 800 + 400 = 1200/s against 1000/s, its queue grows
// by 20 a step and is full (1000) from step 50 on. Its content keeps the arrival mix, 800 : 400, so
// the backlog the node shows is the clients' two thirds of the full queue, 1000 × 800/1200 = 666.7,
// below queue_limit while it already drops; the replicated third of the drops is reported apart.
TEST_CASE("a database's client totals balance while a replica drops replicated writes",
          "[theory]") {
  auto sim = *Simulation::create(*Graph::build(sql_front(2, std::nullopt, 800, 400)));
  double in = 0.0;
  double out = 0.0;
  double backlog = 0.0;
  for (int k = 0; k < 200; ++k) {
    // Kept in a variable of its own: `db` refers into it. Binding `db` to
    // `sim.step(...).nodes[1]` directly would dangle (the temporary StepRecord dies at the end
    // of that full-expression): GCC's -Wdangling-reference warns about it there, and rightly so,
    // the reference is real, not a false positive.
    const auto record = sim.step(k * 100ms, 100ms);
    const auto& db = record.nodes[1];
    in += db.demand_per_s * 0.1;
    out += (db.drain_per_s + db.dropped_per_s) * 0.1;
    backlog = db.backlog;
  }
  REQUIRE_THAT(in, WithinRel(out + backlog, 1e-9));
  REQUIRE_THAT(backlog, WithinRel(1000.0 * 800.0 / 1200.0, 1e-6));
  // 200/s dropped at the replica, a third of it replicated writes.
  REQUIRE_THAT(sim.estimate().nodes[1].replication_dropped_per_s.value(),
               WithinRel(200.0 * 400.0 / 1200.0, 1e-6));
}

// Replicated writes a replica dropped are a database's number: 0 while every replica keeps up,
// empty for a node that keeps no replicated data; the same for the committed writes a replica
// must replay (all 100/s here: the primary keeps up).
TEST_CASE("only a database reports replication drops", "[theory]") {
  SECTION("a database whose replicas keep up drops none") {
    const auto r =
        (*Simulation::create(*Graph::build(sql_front(3, std::nullopt, 1000, 100)))).estimate();
    REQUIRE(r.nodes[1].replication_dropped_per_s.value_or(-1.0) == 0.0);
    REQUIRE_THAT(r.nodes[1].committed_writes_per_s.value_or(-1.0), WithinRel(100.0, 1e-9));
  }
  SECTION("a service has none to report") {
    const auto r = (*Simulation::create(*Graph::build(mm1(500)))).estimate();
    REQUIRE_FALSE(r.nodes[1].replication_dropped_per_s.has_value());
    REQUIRE_FALSE(r.nodes[1].committed_writes_per_s.has_value());
  }
}

// The replication drops are ONE replica's, beside the writes ONE replica must replay: summed over
// the replicas they could exceed the writes committed, which reads as nonsense ("2131/s dropped of
// 900/s"). 3 copies × 2 shards, 3200 reads/s + 600 writes/s, 1000/s per copy, settled. Each shard's
// primary is sent 600 / 2 = 300 writes/s (rho 0.3) and commits them all: committed per shard =
// 300/s. Each of the (3 − 1) × 2 = 4 replicas is sent 3200 / 4 = 800 reads + its shard's 300
// committed writes = 1100/s against 1000/s, so it drops 100/s in its queue's mix, 100 × 300/1100 =
// 27.27/s of them replicated writes. That is the per-replica figure; the four replicas together
// drop 4 × 27.27 = 109.1/s, which is not reported.
TEST_CASE("replication drops are one replica's, beside the committed writes per shard",
          "[theory]") {
  const auto r = (*Simulation::create(*Graph::build(sql_front(3, 2, 3200, 600)))).estimate();
  REQUIRE(r.converged);
  REQUIRE_THAT(r.nodes[1].committed_writes_per_s.value_or(-1.0), WithinRel(300.0, 1e-6));
  REQUIRE_THAT(r.nodes[1].replication_dropped_per_s.value_or(-1.0),
               WithinRel(100.0 * 300.0 / 1100.0, 1e-6));
}

// `pool` is the connection pool: how many queries one copy has in service at once, the c of its
// M/M/c queue. Little's law (concurrency in service = throughput × service time) ties it to the
// throughput: 5k/s × 10 ms keeps 50 queries in service, and `pool: 25` lets only 25 run, so a
// copy serves at most 25 / 10 ms = 2500/s with 25 servers. Reads only, 3 copies: each of the 2
// replicas is sent 500/s, rho 500 / 2500 = 0.2, and a read waits Erlang C's wait for c = 25
// plus its 10 ms.
TEST_CASE("a pool below Little's law's concurrency is each queue's c and bounds its rate",
          "[theory]") {
  auto s = sql_front(3, std::nullopt, 1000, 0, 25);
  s.nodes[1].capacity = Rate{5000};
  s.nodes[1].latency = 10ms;
  const auto r = (*Simulation::create(*Graph::build(s))).step(0s, 100ms);
  REQUIRE_THAT(r.nodes[1].rho, WithinRel(0.2, 1e-9));
  const auto pooled = simsys::queueing::mmc_wait(25, 0.2, 0.010);
  REQUIRE_THAT(client_mean_s(r, "read"), WithinRel(pooled.mean_s + 0.010, 1e-9));
}

// A pool at or above the concurrency Little's law implies changes nothing: the disk, not the
// connections, is the limit. 1000/s × 1 ms implies one query in service; `pool: 4` leaves the
// replicas at M/M/1, rho 0.5, as with no pool at all.
TEST_CASE("a pool above the concurrency Little's law implies changes nothing", "[theory]") {
  const auto r =
      (*Simulation::create(*Graph::build(sql_front(3, std::nullopt, 1000, 0, 4)))).step(0s, 100ms);
  REQUIRE_THAT(r.nodes[1].rho, WithinRel(0.5, 1e-9));
  const auto single = simsys::queueing::mmc_wait(1, 0.5, 0.001);
  REQUIRE_THAT(client_mean_s(r, "read"), WithinRel(single.mean_s + 0.001, 1e-9));
}

// A small pool caps the throughput, it never speeds the disk: 4 connections of a 10 ms query
// carry 4 / 0.010 = 400 queries/s (Little's law), whatever the disk's 5k/s. One copy, 3000
// reads/s: it drains 400/s at rho 1 and, settled with a full queue, drops the other 2600/s.
TEST_CASE("a small pool bounds a database's throughput", "[theory]") {
  auto s = sql_front(1, std::nullopt, 3000, 0, 4);
  s.nodes[1].capacity = Rate{5000};
  s.nodes[1].latency = 10ms;
  const auto r = (*Simulation::create(*Graph::build(s))).estimate();
  REQUIRE_THAT(r.nodes[1].rho, WithinRel(1.0, 1e-12));
  REQUIRE_THAT(r.nodes[1].drain_per_s, WithinRel(400.0, 1e-9));
  REQUIRE_THAT(r.nodes[1].dropped_per_s, WithinRel(2600.0, 1e-9));
}

// A write costs a database 5 capacity units unless the scenario says otherwise: the spec's §6
// table gives one SQL machine "5k reads" or "1k writes" per second (a write's disk seek and
// log flush), 5000 / 1000 = 5 reads' worth. One copy of 1000 units/s, 100 writes/s: 500
// units/s, rho 0.5. A stated `cost` replaces the default.
TEST_CASE("a write costs five units on a database by default", "[theory]") {
  auto s = sql_front(1, std::nullopt, 0, 100);
  s.nodes[1].cost.clear();
  SECTION("the default") {
    const auto r = (*Simulation::create(*Graph::build(s))).step(0s, 100ms);
    REQUIRE_THAT(r.nodes[1].demand_per_s, WithinRel(500.0, 1e-9));
    REQUIRE_THAT(r.nodes[1].rho, WithinRel(0.5, 1e-9));
  }
  SECTION("a stated cost") {
    s.nodes[1].cost = {{"write", 2.0}};
    const auto r = (*Simulation::create(*Graph::build(s))).step(0s, 100ms);
    REQUIRE_THAT(r.nodes[1].rho, WithinRel(0.2, 1e-9));
  }
}

// ---- Request-based CDN (spec §18 entry 10) ----
//
// Book ch. 1: a CDN is a cache at the network's edge. Here it is a read-only cache in front of
// an origin: it answers the share h of the reads itself and sends the misses, as the same op, to
// the origin. Everything but the hit ratio is chosen so the numbers come from it alone.

namespace {
// u (`reads`/s of `read`) -> cdn -> origin. The cdn (10k/s, 1 ms service: c = 10 servers) has the
// given hit ratio (none: it misses every read); the origin (1000/s, 10 ms service: c = 10) is far
// from saturated at the loads used. The u -> cdn link has no latency, the cdn -> origin link
// 10 ms one way (a round trip of 20 ms): the "origin hop" a miss adds is that plus the origin's
// response. Node indices: u 0, cdn 1, origin 2.
Scenario cdn_origin_chain(double reads, std::optional<double> hit) {
  Scenario s{.name = "cdn-origin", .version = 1, .duration = 10s};
  NodeSpec cdn{.id = "cdn", .type = NodeType::cdn, .capacity = Rate{10'000}, .latency = 1ms};
  if (hit) cdn.hit_ratio = HitRatio{.fixed = *hit};
  cdn.origin = "origin";
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{reads}}, .op_shares = {{"read", 1.0}}}},
      std::move(cdn),
      NodeSpec{.id = "origin", .type = NodeType::service, .capacity = Rate{1000}, .latency = 10ms}};
  s.links = {LinkSpec{.from = "u", .to = "cdn", .latency = 0s},
             LinkSpec{.from = "cdn", .to = "origin", .latency = 10ms}};
  return s;
}
}  // namespace

// 1000 reads/s at a CDN that hits 95 % of them:
//   cdn:    demand 1000/s, capacity 10k/s over c = 10 servers (10k/s x 1 ms), rho = 0.1;
//   origin: demand 1000 x (1 - 0.95) = 50/s of the same op, capacity 1000/s, c = 10 (1000/s x
//           10 ms), rho = 50 / 1000 = 0.05.
// Each node's own sojourn is its M/M/c wait plus its service time (Erlang C: mmc_wait):
//   cdn    mmc_wait(10, 0.1, 1 ms)   = 1.25e-11 s  (P(wait) = 1.13e-7, theta = 1 ms / 9)
//   origin mmc_wait(10, 0.05, 10 ms) = 1.8e-13 s   (P(wait) = 1.72e-10, theta = 10 ms / 9.5)
// so almost nothing queues and the sojourns are the service times, 1 ms and 10 ms. A read the cdn
// hits costs its own sojourn; one it misses adds the origin hop (2 x 10 ms of link + the origin's
// sojourn), taken by 5 % of the reads:
//   mean = cdn + 0.05 x (2 x 10 ms + origin) = 1 ms + 0.05 x 30 ms = 2.5 ms (to within 1e-10 s).
TEST_CASE("a cdn answers its hits alone and sends the origin the miss share of the reads",
          "[theory]") {
  const auto g = *Graph::build(cdn_origin_chain(1000, 0.95));
  const auto sim = Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
  const auto r = (*Simulation::create(g)).step(0s, 100ms);
  REQUIRE_THAT(r.nodes[*g.index_of("cdn")].demand_per_s, WithinRel(1000.0, 1e-9));
  REQUIRE_THAT(r.nodes[*g.index_of("origin")].demand_per_s, WithinRel(50.0, 1e-9));
  const auto cdn = simsys::queueing::mmc_wait(10, 0.1, 0.001).mean_s + 0.001;
  const auto origin = simsys::queueing::mmc_wait(10, 0.05, 0.010).mean_s + 0.010;
  REQUIRE_THAT(r.nodes[*g.index_of("cdn")].latency.mean_s, WithinRel(cdn, 1e-9));
  REQUIRE_THAT(r.nodes[*g.index_of("origin")].latency.mean_s, WithinRel(origin, 1e-9));
  REQUIRE(r.responses.size() == 1);
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(cdn + 0.05 * (2 * 0.010 + origin), 1e-9));
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinAbs(0.0025, 1e-7));  // the napkin number
  REQUIRE_THAT(r.responses[0].answered.per_second, WithinRel(1000.0, 1e-9));
  // The long-run state agrees: nothing here depends on the order of the steps.
  const auto settled = (*Simulation::create(g)).estimate();
  REQUIRE_THAT(settled.nodes[*g.index_of("origin")].demand_per_s, WithinRel(50.0, 1e-9));
}

// A CDN the scenario gave no hit ratio misses every read (h = 0) but still runs and still adds
// its own hop: 400 reads/s all reach the origin (rho = 400 / 1000 = 0.4, c = 10, a queue worth
// mmc_wait(10, 0.4, 10 ms) = 1.47e-5 s of waiting: P(wait) = 0.0088, theta = 10 ms / 6).
TEST_CASE("a cdn without a hit ratio runs and passes every read on to its origin", "[theory]") {
  const auto g = *Graph::build(cdn_origin_chain(400, std::nullopt));
  const auto sim = Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
  const auto r = (*Simulation::create(g)).step(0s, 100ms);
  REQUIRE_THAT(r.nodes[*g.index_of("origin")].demand_per_s, WithinRel(400.0, 1e-9));
  const auto cdn = simsys::queueing::mmc_wait(10, 0.04, 0.001).mean_s + 0.001;  // rho 400/10k
  const auto origin = simsys::queueing::mmc_wait(10, 0.4, 0.010).mean_s + 0.010;
  REQUIRE_THAT(r.responses[0].response.mean_s, WithinRel(cdn + (2 * 0.010 + origin), 1e-9));
}

// A `che` CDN without a `keys` size has no number for h: explain shows the paper load as an upper
// bound (every read a miss), but a run would silently replace the Che approximation the scenario
// asked for by h = 0, so the engine refuses it and says what is missing.
TEST_CASE("a che cdn without a keys size builds but the engine refuses to run it", "[theory]") {
  auto s = cdn_origin_chain(1000, std::nullopt);
  s.nodes[1].hit_ratio = HitRatio{.che = true};
  const auto g = Graph::build(std::move(s));
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  const auto sim = Simulation::create(*g);
  REQUIRE_FALSE(sim.has_value());
  REQUIRE(sim.error().message ==
          "node 'cdn': hit_ratio che needs keys (the cdn's size in objects)");
}
