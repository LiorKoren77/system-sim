// Whole-simulation checks against closed-form queueing theory (spec §12).
// Spec entries pinned here: §19 entry 9 (kv_cluster in the steady state).
// This file: kv_cluster in the steady state (book ch. 6, Dynamo). One of the nine files the theory
// tests are split into by topic; the scenario builders more than one of them needs live in
// tests/support/fixtures.cppm (module simsys.test_fixtures).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
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
import simsys.fluid;
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
using simsys::graph::Graph;
using simsys::model::LinkSpec;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
using simsys::model::Workload;
// Shared builders (tests/support/fixtures.cppm).
using simsys::test_support::ring_factor;
using simsys::test_support::simulation_of;

// ---- kv_cluster in the steady state (spec §19 entry 9; book ch. 6, Dynamo) ----
//
// `nodes` machines of `capacity` each on a consistent-hash ring; the coordinator sends every read
// and write to the key's N replicas (ch. 6 "quorum consensus"), so the cluster is sent N times the
// requests. The sample counts that load in REPLICA-REQUESTS: `demand_per_s`, `drain_per_s` and
// `dropped_per_s` are N x the client-level requests (the unit the nodes' queues work in, so the
// balance demand = drain + dropped + d(backlog) holds as everywhere), while the clients' offered
// and answered rates stay in requests. With N < nodes the hottest machine carries k = 1 +
// z_M/sqrt(N vnodes) x the average (ring_factor), as hot and cool copy groups. A request's answer
// is taken over its REAL replica set: with probability p_hot = N k / M its N replicas are the hot
// machine and N - 1 cool ones, otherwise N cool ones; a read is answered at the R-th fastest reply,
// a write at the W-th, each machine's sojourn a lognormal fitted to its own mean and variance
// (queueing::lognormal_order_statistic), and the request is lost when fewer than q of its N
// replica-requests come back (queueing::probability_fewer_than).

namespace {
// u(reads + writes per second) -> store, a kv_cluster of `nodes` machines, `replication` N copies
// per key and the quorums W and R. Every node has `capacity` (default 10k/s) and a service time
// `latency` (default 1 ms, so c = 10 servers per node by Little's law), the link has zero
// latency: a response is the cluster's own. Node indices: u 0, store 1. Ops are `read` and
// `write` (the default write op), left out of the client's mix when their rate is 0.
struct KvFront {
  int nodes{};
  int replication{};
  int write_quorum{};
  int read_quorum{};
  double reads{};
  double writes{};
  double capacity{10'000};
  simsys::SimTime latency{1ms};
  std::optional<int> vnodes{};
};

Scenario kv_front(const KvFront& shape) {
  // C++20 designated initializers: `.name = ...` names each member being set, in
  // declaration order, and leaves the rest at their defaults. Replaces positional braces
  // (which break silently when members are reordered) or a default object assigned field
  // by field; here a scenario reads like the YAML it stands for.
  Scenario s{.name = "kv-front", .version = 1, .duration = 10s};
  std::vector<std::pair<std::string, double>> shares;
  const double total = shape.reads + shape.writes;
  if (shape.reads > 0) shares.emplace_back("read", shape.reads / total);
  if (shape.writes > 0) shares.emplace_back("write", shape.writes / total);
  NodeSpec store{.id = "store",
                 .type = NodeType::kv_cluster,
                 .capacity = Rate{shape.capacity},
                 .latency = shape.latency};
  store.kv_nodes = shape.nodes;
  store.replication = shape.replication;
  store.write_quorum = shape.write_quorum;
  store.read_quorum = shape.read_quorum;
  store.vnodes = shape.vnodes;
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload =
                          Workload{.curve = {.base = Rate{total}}, .op_shares = std::move(shares)}},
             std::move(store)};
  s.links = {LinkSpec{.from = "u", .to = "store", .latency = 0s}};
  return s;
}

// What the client saw for `op` in record `r`.
const simsys::engine::ResponseSample& response_of(const simsys::engine::StepRecord& r,
                                                  std::string_view op) {
  // C++20 std::ranges::find with a projection: compares `op` with each sample's `op` member
  // (the projection, a pointer to member), replacing std::find_if over begin/end with a lambda
  // that reads the member.
  const auto it = std::ranges::find(r.responses, op, &simsys::engine::ResponseSample::op);
  REQUIRE(it != r.responses.end());
  return *it;
}

// The kv ring's imbalance at the default 150 virtual nodes: M = 8, N = 3 gives
// 1 + 1.4342/sqrt(450) = 1.06761; M = 16, N = 3 gives 1.08338; M = 4, N = 2 gives 1.06057.
double kv_ring_k(int machines, int replication) {
  return ring_factor(machines, replication, simsys::model::default_vnodes);
}

// A machine's sojourn in the M/M/c queue that one copy of the cluster is: c = 10 servers of
// 1 ms (the kv_front defaults) at utilization rho, its wait from Erlang C plus one exponential
// service (variance s^2).
simsys::queueing::TimeMoments machine_sojourn(double rho) {
  const auto wait = simsys::queueing::mmc_wait(10, rho, 0.001);
  return {.mean_s = wait.mean_s + 0.001, .variance_s2 = wait.variance_s2 + 1e-6};
}
}  // namespace

// nodes = 3, N = 3: every node holds every key, so every node is sent every request: 1k reads/s
// make 1k replica-requests/s at each of the three, 3k in all. Each node: capacity 10k/s x 1 ms
// service = c = 10 servers, rho = 1000 / 10000 = 0.1 (utilization per server), and the chance a
// request waits is Erlang C(c = 10, rho = 0.1) = 1.126e-7, a mean wait of C x s / (c (1 - rho)) =
// 1.25e-11 s: nothing next to the 1 ms service (relative 1.3e-8), so each replica's sojourn has
// mean m = 1 ms and variance s^2 = 1 ms^2 (SCV 1) to a part in 1e8. The read's response is the
// R-th fastest of three lognormals of those moments (queueing::lognormal_order_statistic, whose
// unit tests pin these figures): 0.4210, 0.8262 and 1.7528 ms for R = 1, 2, 3. Renyi's
// exponentials of the same mean would give 1/3, 5/6 and 11/6 ms; the fit, not the queue, differs.
TEST_CASE("a three-node cluster at replication three answers a read in the R-th fastest replica",
          "[theory]") {
  REQUIRE(simsys::queueing::erlang_c(10, 0.1) < 2e-7);  // the wait is negligible, as derived
  const auto read_with = [](int read_quorum) {
    const auto g = *Graph::build(kv_front({.nodes = 3,
                                           .replication = 3,
                                           .write_quorum = 2,
                                           .read_quorum = read_quorum,
                                           .reads = 1000,
                                           .writes = 0}));
    return simulation_of(g).step(0s, 100ms);
  };
  SECTION("R = 1: the fastest of three, 0.4210 ms") {
    const auto r = read_with(1);
    REQUIRE_THAT(response_of(r, "read").response.mean_s, WithinRel(4.2099631630e-4, 1e-6));
    REQUIRE_THAT(response_of(r, "read").response.variance_s2, WithinRel(7.3174194295e-8, 1e-6));
  }
  SECTION("R = 2: the quorum of two, 0.8262 ms") {
    const auto r = read_with(2);
    REQUIRE_THAT(response_of(r, "read").response.mean_s, WithinRel(8.2618489437e-4, 1e-6));
    REQUIRE_THAT(response_of(r, "read").response.variance_s2, WithinRel(2.5078568374e-7, 1e-6));
  }
  SECTION("R = 3: the slowest of three, 1.7528 ms") {
    const auto r = read_with(3);
    REQUIRE_THAT(response_of(r, "read").response.mean_s, WithinRel(1.7528187893e-3, 1e-6));
    REQUIRE_THAT(response_of(r, "read").response.variance_s2, WithinRel(1.7438470357e-6, 1e-6));
  }
}

// The same cluster at 1k reads/s: each node is sent 1k x N / nodes = 1000/s, so the node is at
// rho = 0.1 (the average, and the hottest too: N = nodes has no imbalance) and the sample counts
// the cluster's 3 x 1000 = 3000 replica-requests/s, which it all serves.
TEST_CASE("a kv_cluster's sample counts N times the requests in replica-requests", "[theory]") {
  const auto g = *Graph::build(kv_front({.nodes = 3,
                                         .replication = 3,
                                         .write_quorum = 2,
                                         .read_quorum = 2,
                                         .reads = 1000,
                                         .writes = 0}));
  const auto r = simulation_of(g).step(0s, 100ms);
  REQUIRE_THAT(r.nodes[1].demand_per_s, WithinRel(3000.0, 1e-12));
  REQUIRE_THAT(r.nodes[1].drain_per_s, WithinRel(3000.0, 1e-9));
  REQUIRE(r.nodes[1].dropped_per_s == 0.0);
  REQUIRE_THAT(r.nodes[1].rho, WithinRel(0.1, 1e-12));
  // The clients' side stays in requests: 1000 offered, all answered.
  REQUIRE_THAT(response_of(r, "read").offered.per_second, WithinRel(1000.0, 1e-12));
  REQUIRE_THAT(response_of(r, "read").answered.per_second, WithinRel(1000.0, 1e-9));
}

// A different N over a bigger ring: 4 nodes, N = 2, 4k reads/s: the cluster is sent 2 x 4000 =
// 8000 replica-requests/s, 2000 per node on average (rho 0.2), and the hottest node
// k = 1 + z_4/sqrt(2 x 150) = 1.0606 times that: rho_hot = 0.2121, the node's reported rho.
TEST_CASE("each request is sent to N replicas whatever the number of nodes", "[theory]") {
  const auto g = *Graph::build(kv_front({.nodes = 4,
                                         .replication = 2,
                                         .write_quorum = 1,
                                         .read_quorum = 1,
                                         .reads = 4000,
                                         .writes = 0}));
  const auto r = simulation_of(g).step(0s, 100ms);
  REQUIRE_THAT(r.nodes[1].demand_per_s, WithinRel(8000.0, 1e-12));
  REQUIRE_THAT(r.nodes[1].rho, WithinRel(kv_ring_k(4, 2) * 0.2, 1e-9));
  REQUIRE_THAT(r.nodes[1].rho, WithinAbs(0.2121, 1e-4));
}

// Writes wait for the W-th replica, reads for the R-th, from the same replicas: reads and writes
// 500/s each at nodes = 3, N = 3 are 1000 replica-requests/s per node again (m = 1 ms, SCV 1),
// so with R = 1 and W = 3 the read answers in 0.4210 ms and the write in 1.7528 ms, four times
// slower: the price of acknowledging at every replica.
TEST_CASE("a write is answered at the W-th fastest replica and a read at the R-th", "[theory]") {
  const auto g = *Graph::build(kv_front({.nodes = 3,
                                         .replication = 3,
                                         .write_quorum = 3,
                                         .read_quorum = 1,
                                         .reads = 500,
                                         .writes = 500}));
  const auto r = simulation_of(g).step(0s, 100ms);
  REQUIRE_THAT(response_of(r, "read").response.mean_s, WithinRel(4.2099631630e-4, 1e-6));
  REQUIRE_THAT(response_of(r, "write").response.mean_s, WithinRel(1.7528187893e-3, 1e-6));
}

TEST_CASE("a write quorum of two answers a write in 0.8262 ms", "[theory]") {
  const auto g = *Graph::build(kv_front({.nodes = 3,
                                         .replication = 3,
                                         .write_quorum = 2,
                                         .read_quorum = 3,
                                         .reads = 0,
                                         .writes = 1000}));
  const auto r = simulation_of(g).step(0s, 100ms);
  REQUIRE_THAT(response_of(r, "write").response.mean_s, WithinRel(8.2618489437e-4, 1e-6));
}

// Which ops are writes is the node's `write_ops` (model::write_ops_of; absent: just `write`). With
// write_ops = [put], a `put` waits for W = 3 and a `get` for R = 1, the same replicas as before.
TEST_CASE("the ops a kv_cluster lists in write_ops use the write quorum", "[theory]") {
  Scenario s = kv_front({.nodes = 3,
                         .replication = 3,
                         .write_quorum = 3,
                         .read_quorum = 1,
                         .reads = 500,
                         .writes = 500});
  s.nodes[0].workload->op_shares = {{"get", 0.5}, {"put", 0.5}};
  s.nodes[1].write_ops = std::vector<std::string>{"put"};
  const auto r = simulation_of(*Graph::build(s)).step(0s, 100ms);
  REQUIRE_THAT(response_of(r, "get").response.mean_s, WithinRel(4.2099631630e-4, 1e-6));
  REQUIRE_THAT(response_of(r, "put").response.mean_s, WithinRel(1.7528187893e-3, 1e-6));
}

// With one copy per key (N = 1) a request goes to one machine and waits for it: the quorum of one
// is that machine's own sojourn, and over the ring a request reaches the hot machine with
// probability p_hot = N k / M = k / M, the share the node's own latency already gives it. So the
// cluster answers exactly like the plain queue whose latency the node reports: on one machine,
// and on 8 machines behind the ring (k = 1.1171 at N = 1, hot rho 0.5585, cool 0.4933).
TEST_CASE("a cluster with one copy per key answers like the plain queue", "[theory]") {
  for (const int machines : {1, 8}) {
    CAPTURE(machines);
    const auto g = *Graph::build(kv_front({.nodes = machines,
                                           .replication = 1,
                                           .write_quorum = 1,
                                           .read_quorum = 1,
                                           .reads = 5'000.0 * machines,
                                           .writes = 0}));
    const auto r = simulation_of(g).step(0s, 100ms);
    const auto& plain = r.nodes[1].latency;
    REQUIRE_THAT(response_of(r, "read").response.mean_s, WithinRel(plain.mean_s, 1e-9));
    REQUIRE_THAT(response_of(r, "read").response.variance_s2, WithinRel(plain.variance_s2, 1e-8));
  }
}

// A request meets the hot machine only when its key's replica set holds it. nodes = 8, N = 3, 20k
// reads/s at 10k/s per node, 1 ms (c = 10): 60k replica-requests/s, 7.5k per node on average,
// rho = 0.75. The ring (k = kv_ring_k(8, 3) = 1.06761): the hot node carries k x 7.5k = 8.007k
// (rho_hot = 0.8007) and each of the 7 others (8 - k)/(8 x 7) x 60k = 7.428k (rho_cool = 0.7428).
// The hot node is sent k/8 of the 60k replica-requests, one from each request whose key it holds,
// so p_hot = 3 k / 8 = 0.40035 of the requests have it among their three replicas:
//   read = p_hot x 2nd of {hot, cool, cool}  +  (1 - p_hot) x 2nd of {cool, cool, cool}
// mixed by the law of total variance (behaviours::mixture). 0.94958 ms, against the 0.9402 ms of
// the old model (5/6 of the copies' request-weighted mean sojourn, as if every replica were an
// exponential of that mean). The node reports the hot copy's rho.
TEST_CASE("a read sees the hot machine only when its key's replicas include it", "[theory]") {
  const auto g = *Graph::build(kv_front({.nodes = 8,
                                         .replication = 3,
                                         .write_quorum = 2,
                                         .read_quorum = 2,
                                         .reads = 20'000,
                                         .writes = 0}));
  const auto r = simulation_of(g).step(0s, 100ms);
  constexpr double nodes = 8;
  const double k = kv_ring_k(8, 3);
  const double rho_hot = k * 0.75;
  const double rho_cool = (nodes - k) / (nodes - 1) * 0.75;
  using simsys::queueing::ReplicaGroup;
  const std::array with_hot{ReplicaGroup{.replicas = 1, .sojourn = machine_sojourn(rho_hot)},
                            ReplicaGroup{.replicas = 2, .sojourn = machine_sojourn(rho_cool)}};
  const std::array all_cool{ReplicaGroup{.replicas = 3, .sojourn = machine_sojourn(rho_cool)}};
  const auto hot_read = simsys::queueing::lognormal_order_statistic(with_hot, 2);
  const auto cool_read = simsys::queueing::lognormal_order_statistic(all_cool, 2);
  const double p_hot = 3.0 * k / nodes;
  const double mean = p_hot * hot_read.mean_s + (1 - p_hot) * cool_read.mean_s;
  const double variance =
      p_hot * (hot_read.variance_s2 + (hot_read.mean_s - mean) * (hot_read.mean_s - mean)) +
      (1 - p_hot) * (cool_read.variance_s2 + (cool_read.mean_s - mean) * (cool_read.mean_s - mean));
  REQUIRE_THAT(response_of(r, "read").response.mean_s, WithinRel(mean, 1e-9));
  REQUIRE_THAT(response_of(r, "read").response.variance_s2, WithinRel(variance, 1e-8));
  REQUIRE_THAT(response_of(r, "read").response.mean_s, WithinRel(9.4958356507e-4, 1e-6));
  REQUIRE_THAT(r.nodes[1].rho, WithinRel(rho_hot, 1e-12));
  REQUIRE_THAT(r.nodes[1].demand_per_s, WithinRel(60'000.0, 1e-12));
}

// The kv-store lab with `vnodes: 1` at 16 machines (0.3 ms each, c = 3; 21k reads and 9k writes,
// W = R = 2): the ring is as uneven as it gets, k = 1 + z_16/sqrt(3) = 2.0212, so the hot machine
// is sent 2.0212 x 5.625k = 11.37k/s against its 10k/s, fills its room and answers in about 100 ms,
// dropping 12 % of what it is sent. Yet a key's three replicas hold it with probability
// p_hot = 3 k / 16 = 0.379, and then the two cool replicas (rho 0.524) answer first: the quorum of
// two never waits for the hot one and never misses its drops. The store answers in 0.3775 ms
// (lognormal order statistics), nothing lost. A Monte Carlo of the same replicas with their exact
// M/M/3 sojourns (the review's, and ours: 0.385-0.389 ms) lands within 3 %: the residue is the
// lognormal's shape, not the quorum. The old model took the order statistic over the copies'
// request-weighted mean (10.7 ms) and failed the lab's p99.
TEST_CASE("with one virtual node the quorum of two hides the hot machine", "[theory]") {
  const auto g = *Graph::build(kv_front({.nodes = 16,
                                         .replication = 3,
                                         .write_quorum = 2,
                                         .read_quorum = 2,
                                         .reads = 21'000,
                                         .writes = 9'000,
                                         .capacity = 10'000,
                                         .latency = 300us,
                                         .vnodes = 1}));
  const auto settled = simulation_of(g).estimate();
  REQUIRE(settled.nodes[1].rho == 1.0);  // the hot machine is saturated
  REQUIRE(settled.nodes[1].dropped_per_s > 1'000.0);
  for (const auto* op : {"read", "write"}) {
    CAPTURE(op);
    const auto& answer = response_of(settled, op);
    REQUIRE_THAT(answer.response.mean_s, WithinRel(3.7751979967e-4, 1e-5));
    REQUIRE_THAT(answer.response.mean_s, WithinRel(0.385e-3, 0.03));  // the Monte Carlo
    REQUIRE_THAT(answer.answered.per_second, WithinRel(answer.offered.per_second, 1e-12));
  }
}

// A request is lost when fewer than q of its N replica-requests come back. Three machines, N = 3,
// each sent 11.25k/s against its 10k/s: each keeps 8/9 of what it is sent once its room is full. A
// read needing R replies is lost with P(Bin(3, 8/9) < R) (queueing::probability_fewer_than):
//   R = 1  1/729 = 0.14 %,   R = 2  25/729 = 3.4 %,   R = 3  1 - 512/729 = 29.8 %
// where the old rule lost the replica-requests' share, 1/9 = 11.1 %, at every R: it over-counted
// a forgiving quorum and under-counted a strict one.
TEST_CASE("a quorum of q of three saturated replicas is lost with the binomial's probability",
          "[theory]") {
  const auto delivered_with = [](int read_quorum) {
    const auto g = *Graph::build(kv_front({.nodes = 3,
                                           .replication = 3,
                                           .write_quorum = read_quorum,
                                           .read_quorum = read_quorum,
                                           .reads = 11'250,
                                           .writes = 0}));
    const auto settled = simulation_of(g).estimate();
    REQUIRE_THAT(settled.nodes[1].dropped_per_s, WithinRel(3'750.0, 1e-9));  // 1/9 of 33.75k
    const auto& read = response_of(settled, "read");
    return read.answered.per_second / read.offered.per_second;
  };
  REQUIRE_THAT(delivered_with(1), WithinRel(1.0 - 1.0 / 729.0, 1e-9));
  REQUIRE_THAT(delivered_with(2), WithinRel(1.0 - 25.0 / 729.0, 1e-9));
  REQUIRE_THAT(delivered_with(3), WithinRel(512.0 / 729.0, 1e-9));
}

// The book's lab as the plan first shipped it: nodes = 8, N = 3, vnodes 150, 30k requests/s of
// capacity-10k/s nodes. The cluster is sent 3 x 30k = 90k replica-requests/s against 8 x 10k = 80k:
// 11.25k per node on average (112.5 %), the hottest k = 1.0676 x that = 12.01k (120.1 %, red) and
// each of the others (8 - k)/56 x 90k = 11.14k (111.4 %): every node is over capacity and every
// queue ends full. The cluster serves 80k and drops 10k replica-requests/s, one ninth: the hot
// machine keeps 10k/12.01k = 0.8326 of what it is sent, a cool one 10k/11.14k = 0.8976. A request
// is lost when fewer than two of its three replicas answer, over its real replica set: with p_hot =
// 3k/8 = 0.4004, P(< 2 of {hot, cool, cool}), otherwise P(< 2 of three cool): 3.41 % lost, 96.59 %
// answered, not the 8/9 of the old "share of replica-requests dropped".
TEST_CASE("eight nodes at replication three overload at 30k requests a second", "[theory]") {
  const auto g = *Graph::build(kv_front({.nodes = 8,
                                         .replication = 3,
                                         .write_quorum = 2,
                                         .read_quorum = 2,
                                         .reads = 21'000,
                                         .writes = 9'000,
                                         .vnodes = 150}));
  const auto settled = simulation_of(g).estimate();
  REQUIRE(settled.converged);
  const auto& store = settled.nodes[1];
  REQUIRE_THAT(store.demand_per_s, WithinRel(90'000.0, 1e-9));
  REQUIRE_THAT(store.drain_per_s, WithinRel(80'000.0, 1e-9));
  REQUIRE_THAT(store.dropped_per_s, WithinRel(10'000.0, 1e-9));
  REQUIRE_THAT(store.rho, WithinRel(1.0, 1e-9));
  REQUIRE(store.zone == simsys::fluid::Zone::red);
  const double k = kv_ring_k(8, 3);
  const double kept_hot = 10'000.0 / (90'000.0 * k / 8.0);
  const double kept_cool = 10'000.0 / (90'000.0 * (8.0 - k) / 56.0);
  using simsys::queueing::TrialGroup;
  const std::array with_hot{TrialGroup{.trials = 1, .success = kept_hot},
                            TrialGroup{.trials = 2, .success = kept_cool}};
  const std::array all_cool{TrialGroup{.trials = 3, .success = kept_cool}};
  const double p_hot = 3.0 * k / 8.0;
  const double lost = p_hot * simsys::queueing::probability_fewer_than(with_hot, 2) +
                      (1.0 - p_hot) * simsys::queueing::probability_fewer_than(all_cool, 2);
  for (const auto* op : {"read", "write"}) {
    CAPTURE(op);
    const auto& answered = response_of(settled, op);
    const double delivered = answered.answered.per_second / answered.offered.per_second;
    REQUIRE_THAT(delivered, WithinRel(1.0 - lost, 1e-9));
    REQUIRE_THAT(delivered, WithinRel(0.9658844059, 1e-8));
  }
}

// The average load can be under capacity while the hottest node already sheds: nodes = 8, N = 3,
// 26k requests/s make 78k replica-requests/s, 9.75k per node on average (97.5 %: under 10k/s).
// The hottest node, sent k/8 = 13.35 % of them (10.41k/s, 104.1 %), drops 78k x k/8 - 10k =
// 409.2 replica-requests/s once its queue is full; each of the others is sent (8 - k)/56 x 78k =
// 9.656k/s and serves all of it. What the clients lose depends on the quorum: a read (R = 2) is
// answered by the two cool replicas whenever the hot one drops it, so nothing is lost; a write that
// needs all three (W = 3) is lost whenever the hot one drops it, p_hot x (1 - kept_hot) = 1.57 %.
TEST_CASE("the hottest node of a ring drops while the average node is under capacity", "[theory]") {
  const auto g = *Graph::build(kv_front({.nodes = 8,
                                         .replication = 3,
                                         .write_quorum = 3,
                                         .read_quorum = 2,
                                         .reads = 20'000,
                                         .writes = 6'000}));
  const auto settled = simulation_of(g).estimate();
  const double k = kv_ring_k(8, 3);
  const double dropped = 78'000.0 * k / 8.0 - 10'000.0;
  REQUIRE_THAT(dropped, WithinRel(409.19, 1e-4));  // the derivation's number
  const auto& store = settled.nodes[1];
  REQUIRE_THAT(store.dropped_per_s, WithinRel(dropped, 1e-6));
  REQUIRE_THAT(store.drain_per_s, WithinRel(78'000.0 - dropped, 1e-9));
  REQUIRE_THAT(store.rho, WithinRel(1.0, 1e-9));  // the hottest node's
  const auto& read = response_of(settled, "read");
  REQUIRE_THAT(read.answered.per_second, WithinRel(read.offered.per_second, 1e-12));
  const double kept_hot = 10'000.0 / (78'000.0 * k / 8.0);
  const auto& write = response_of(settled, "write");
  REQUIRE_THAT(write.answered.per_second / write.offered.per_second,
               WithinRel(1.0 - 3.0 * k / 8.0 * (1.0 - kept_hot), 1e-9));
  REQUIRE_THAT(write.answered.per_second / write.offered.per_second, WithinRel(0.9842620682, 1e-8));
}

// The fix: double the nodes. 16 nodes of 10k/s against the same 90k replica-requests/s: 5.625k per
// node on average (56.25 %), the hottest k = 1.08338 x 5.625k = 6.094k (60.9 %, green), nothing
// dropped, every request answered.
TEST_CASE("sixteen nodes carry the same load with the hottest node at 60.9 percent", "[theory]") {
  const auto g = *Graph::build(kv_front({.nodes = 16,
                                         .replication = 3,
                                         .write_quorum = 2,
                                         .read_quorum = 2,
                                         .reads = 21'000,
                                         .writes = 9'000,
                                         .vnodes = 150}));
  const auto settled = simulation_of(g).estimate();
  const auto& store = settled.nodes[1];
  REQUIRE_THAT(store.demand_per_s, WithinRel(90'000.0, 1e-9));
  REQUIRE_THAT(store.drain_per_s, WithinRel(90'000.0, 1e-9));
  REQUIRE(store.dropped_per_s == 0.0);
  REQUIRE_THAT(store.rho, WithinRel(kv_ring_k(16, 3) * 0.5625, 1e-9));
  REQUIRE_THAT(store.rho, WithinRel(0.6094, 1e-4));
  REQUIRE(store.zone == simsys::fluid::Zone::green);
  for (const auto* op : {"read", "write"}) {
    CAPTURE(op);
    REQUIRE_THAT(response_of(settled, op).answered.per_second,
                 WithinRel(response_of(settled, op).offered.per_second, 1e-9));
  }
}

// estimate is the long-run state of run: below capacity the last step of a run equals it, for
// the even cluster (N = nodes) and for the ring, in the node's numbers and in each op's response.
TEST_CASE("estimate and run agree on a kv_cluster below capacity", "[theory]") {
  const auto agree = [](const KvFront& shape) {
    const auto g = *Graph::build(kv_front(shape));
    auto running = simulation_of(g);
    simsys::engine::StepRecord last;
    running.run({.duration = 2s, .step = 100ms},
                [&](const simsys::engine::StepRecord& r) { last = r; });
    const auto e = simulation_of(g).estimate();
    REQUIRE_THAT(last.nodes[1].rho, WithinRel(e.nodes[1].rho, 1e-9));
    REQUIRE_THAT(last.nodes[1].demand_per_s, WithinRel(e.nodes[1].demand_per_s, 1e-9));
    REQUIRE_THAT(last.nodes[1].drain_per_s, WithinRel(e.nodes[1].drain_per_s, 1e-9));
    for (const auto* op : {"read", "write"}) {
      CAPTURE(op);
      REQUIRE_THAT(response_of(last, op).response.mean_s,
                   WithinRel(response_of(e, op).response.mean_s, 1e-9));
      REQUIRE_THAT(response_of(last, op).answered.per_second,
                   WithinRel(response_of(e, op).answered.per_second, 1e-9));
    }
  };
  SECTION("three nodes, N = 3: every node alike") {
    agree({.nodes = 3,
           .replication = 3,
           .write_quorum = 3,
           .read_quorum = 1,
           .reads = 500,
           .writes = 500});
  }
  SECTION("sixteen nodes, N = 3: a ring") {
    agree({.nodes = 16,
           .replication = 3,
           .write_quorum = 2,
           .read_quorum = 2,
           .reads = 21'000,
           .writes = 9'000});
  }
}

// Conservation in replica-requests, every step of the overloaded cluster: what the nodes were
// sent (N x the requests) is what they served plus what they dropped plus what they added to
// their queues. Nothing is created or lost by the replication, and the saturated run's last step
// is the settled state: the queues fill within 5 to 9 steps (1000 of room per node, the excess
// 112 to 217 per step).
TEST_CASE("a kv_cluster conserves its replica-requests at every step of an overload", "[theory]") {
  const auto g = *Graph::build(kv_front({.nodes = 8,
                                         .replication = 3,
                                         .write_quorum = 2,
                                         .read_quorum = 2,
                                         .reads = 21'000,
                                         .writes = 9'000}));
  auto sim = simulation_of(g);
  double backlog = 0.0;
  simsys::engine::StepRecord last;
  sim.run({.duration = 3s, .step = 100ms}, [&](const simsys::engine::StepRecord& r) {
    const auto& n = r.nodes[1];
    REQUIRE_THAT(backlog + n.demand_per_s * 0.1,
                 WithinRel((n.drain_per_s + n.dropped_per_s) * 0.1 + n.backlog, 1e-9));
    REQUIRE_THAT(n.demand_per_s, WithinRel(90'000.0, 1e-9));  // 3 x 30k, every step
    backlog = n.backlog;
    last = r;
  });
  const auto settled = sim.estimate();
  REQUIRE_THAT(last.nodes[1].drain_per_s, WithinRel(settled.nodes[1].drain_per_s, 1e-9));
  REQUIRE_THAT(last.nodes[1].dropped_per_s, WithinRel(settled.nodes[1].dropped_per_s, 1e-9));
  REQUIRE_THAT(backlog, WithinRel(8.0 * 1000.0, 1e-9));  // 8 full queues of 1000
}
