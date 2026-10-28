// What client and service nodes do with flow and how they compose response latency.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.behaviours;
import simsys.core;
import simsys.flow;
import simsys.graph;
import simsys.model;
import simsys.workload;

using namespace std::chrono_literals;
using Catch::Matchers::WithinRel;
using simsys::Rate;
using simsys::behaviours::ClientBehaviour;
using simsys::behaviours::LoadBalancerBehaviour;
using simsys::behaviours::Outgoing;
using simsys::behaviours::ServiceBehaviour;
using simsys::flow::Flow;
using simsys::flow::LatencyEstimate;
using simsys::model::LbPolicy;
using simsys::model::LinkSpec;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
using simsys::model::Step;
using simsys::model::StepMode;
using simsys::model::Workload;

namespace {
// C++20 designated initializers: `NodeSpec{.id = "web", .type = ...}` names each member it
// sets and leaves the rest at their defaults, replacing positional aggregate
// initialization where a reader must count commas against the struct definition.
// users -> web -> {cache, db}; web.get = [cache.read, db.select x2]; put is terminal at web.
Scenario two_hop(StepMode mode) {
  Scenario s{.name = "t", .version = 1, .duration = 1min};
  s.nodes = {NodeSpec{.id = "users",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{100}},
                                           .op_shares = {{"get", 0.8}, {"put", 0.2}}}},
             NodeSpec{.id = "web",
                      .type = NodeType::service,
                      .ops = {{"get",
                               {Step{.to = "cache", .op = "read", .mode = mode},
                                Step{.to = "db", .op = "select", .mode = mode, .factor = 2}}}}},
             NodeSpec{.id = "cache", .type = NodeType::service},
             NodeSpec{.id = "db", .type = NodeType::service}};
  s.links = {LinkSpec{.from = "users", .to = "web", .latency = 1ms},
             LinkSpec{.from = "web", .to = "cache", .latency = 0s},
             LinkSpec{.from = "web", .to = "db", .latency = 10ms}};
  return s;
}
// u -> lb -> {a (weight 3), b (weight 1)}, zero-latency links.
Scenario weighted_lb() {
  Scenario s{.name = "lb", .version = 1, .duration = 1min};
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{100}}, .op_shares = {{"get", 1.0}}}},
      NodeSpec{.id = "lb", .type = NodeType::load_balancer, .policy = LbPolicy::weighted},
      NodeSpec{.id = "a", .type = NodeType::service},
      NodeSpec{.id = "b", .type = NodeType::service}};
  s.links = {LinkSpec{.from = "u", .to = "lb", .latency = 0s},
             LinkSpec{.from = "lb", .to = "a", .latency = 0s, .weight = 3},
             LinkSpec{.from = "lb", .to = "b", .latency = 0s, .weight = 1}};
  return s;
}
}  // namespace

// C++20 concepts checked at compile time: the behaviours must satisfy the engine's
// requirements, or this file does not compile.
static_assert(simsys::behaviours::NodeBehaviour<ServiceBehaviour>);
static_assert(simsys::behaviours::RequestSource<ClientBehaviour>);
static_assert(simsys::behaviours::NodeBehaviour<LoadBalancerBehaviour>);

// And the negative: a type that routes flow but cannot answer for its latency is not a
// NodeBehaviour, so the engine refuses it at compile time instead of misbehaving at run time.
struct NoResponse {
  simsys::behaviours::Routed route(const Flow&, simsys::SimTime) const;
};
static_assert(!simsys::behaviours::NodeBehaviour<NoResponse>);

TEST_CASE("a client offers base rate times op share along its routes", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(two_hop(StepMode::sequential));
  const ClientBehaviour c{g, *g.index_of("users")};
  const auto routed = c.offer(0s);
  REQUIRE(routed.outgoing.size() == 1);
  REQUIRE(routed.outgoing[0].target == *g.index_of("web"));
  REQUIRE_THAT(routed.outgoing[0].flow.rate_of("get").per_second, WithinRel(80.0, 1e-12));
  REQUIRE_THAT(routed.outgoing[0].flow.rate_of("put").per_second, WithinRel(20.0, 1e-12));
}

TEST_CASE("a service forwards each step's op scaled by its factor and serves every incoming op",
          "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(two_hop(StepMode::sequential));
  const ServiceBehaviour web{g, *g.index_of("web")};
  Flow in;
  in.add("get", Rate{50});
  in.add("put", Rate{5});  // no steps: terminal op, served locally
  const auto routed = web.route(in, 100ms);
  REQUIRE(routed.outgoing.size() == 2);
  // C++20 ranges algorithm with a projection: `&Outgoing::target` is applied to each
  // element before comparing with the db's index, replacing a lambda passed to std::find_if.
  const auto to_db = std::ranges::find(routed.outgoing, *g.index_of("db"), &Outgoing::target);
  REQUIRE(to_db != routed.outgoing.end());
  REQUIRE_THAT(to_db->flow.rate_of("select").per_second, WithinRel(100.0, 1e-12));  // factor 2
  const auto to_cache = std::ranges::find(routed.outgoing, *g.index_of("cache"), &Outgoing::target);
  REQUIRE_THAT(to_cache->flow.rate_of("read").per_second, WithinRel(50.0, 1e-12));
  REQUIRE_THAT(routed.served.total().per_second, WithinRel(55.0, 1e-12));
}

TEST_CASE("a service with no incoming flow forwards nothing", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(two_hop(StepMode::sequential));
  const ServiceBehaviour web{g, *g.index_of("web")};
  const auto routed = web.route(Flow{}, 100ms);
  REQUIRE(routed.outgoing.empty());
  REQUIRE(routed.served.empty());
}

TEST_CASE("sequential steps add hop latencies: two link legs plus the downstream response",
          "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(two_hop(StepMode::sequential));
  const ServiceBehaviour web{g, *g.index_of("web")};
  const LatencyEstimate own{0.001, 0.0};
  const auto downstream = [&](std::size_t node, std::string_view) {
    return node == *g.index_of("db") ? LatencyEstimate{0.005, 1e-6} : LatencyEstimate{0.0002, 0.0};
  };
  const auto r = web.response("get", own, downstream);
  // own 1 ms + cache (0 + 0.2 ms + 0) + db (10 ms + 5 ms + 10 ms) = 26.2 ms
  REQUIRE_THAT(r.mean_s, WithinRel(0.0262, 1e-12));
  REQUIRE_THAT(r.variance_s2, WithinRel(1e-6, 1e-12));
}

TEST_CASE("parallel steps add the slowest branch instead of the sum", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(two_hop(StepMode::parallel));
  const ServiceBehaviour web{g, *g.index_of("web")};
  const auto downstream = [&](std::size_t node, std::string_view) {
    return node == *g.index_of("db") ? LatencyEstimate{0.005, 1e-6} : LatencyEstimate{0.0002, 4e-6};
  };
  const auto r = web.response("get", LatencyEstimate{0.001, 0.0}, downstream);
  REQUIRE_THAT(r.mean_s, WithinRel(0.001 + 0.025, 1e-12));
  REQUIRE_THAT(r.variance_s2, WithinRel(4e-6, 1e-12));  // largest variance among branches
}

TEST_CASE("an op without steps responds with the node's own sojourn", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(two_hop(StepMode::sequential));
  const ServiceBehaviour web{g, *g.index_of("web")};
  const auto r =
      web.response("put", LatencyEstimate{0.003, 2e-6},
                   [](std::size_t, std::string_view) { return LatencyEstimate{99.0, 99.0}; });
  REQUIRE(r == LatencyEstimate{0.003, 2e-6});
}

TEST_CASE("a client's response adds its link's two legs to the first hop's response",
          "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(two_hop(StepMode::sequential));
  const ClientBehaviour c{g, *g.index_of("users")};
  const auto r =
      c.response("get", [](std::size_t, std::string_view) { return LatencyEstimate{0.004, 1e-6}; });
  REQUIRE_THAT(r.mean_s, WithinRel(0.001 + 0.004 + 0.001, 1e-12));
  REQUIRE_THAT(r.variance_s2, WithinRel(1e-6, 1e-12));
}

TEST_CASE("a client op split across two links mixes the responses by share", "[behaviours]") {
  Scenario s{.name = "split", .version = 1, .duration = 1min};
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{100}}, .op_shares = {{"id", 1.0}}}},
      NodeSpec{.id = "fast", .type = NodeType::service},
      NodeSpec{.id = "slow", .type = NodeType::service}};
  s.links = {LinkSpec{.from = "u", .to = "fast", .latency = 0s, .weight = 3},
             LinkSpec{.from = "u", .to = "slow", .latency = 0s, .weight = 1}};
  const auto g = *simsys::graph::Graph::build(s);
  const ClientBehaviour c{g, *g.index_of("u")};
  const auto r = c.response("id", [&](std::size_t node, std::string_view) {
    return node == *g.index_of("fast") ? LatencyEstimate{0.001, 0.0} : LatencyEstimate{0.005, 0.0};
  });
  // Mixture: mean = 0.75 * 1 ms + 0.25 * 5 ms = 2 ms;
  // variance = Σ s_i v_i + Σ s_i (m_i − mean)²
  //          = 0 + 0.75 * (1 ms − 2 ms)² + 0.25 * (5 ms − 2 ms)² = 0.75e-6 + 2.25e-6 = 3e-6
  // (law of total variance: the expected conditional variance plus the variance of the
  // conditional means; both routes are deterministic here, so all of it is the spread
  // between routes).
  REQUIRE_THAT(r.mean_s, WithinRel(0.002, 1e-12));
  REQUIRE_THAT(r.variance_s2, WithinRel(3e-6, 1e-12));
}

TEST_CASE("client ops are listed in sorted order for deterministic reports", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(two_hop(StepMode::sequential));
  const ClientBehaviour c{g, *g.index_of("users")};
  // C++20 std::span<const std::string>: ops() returns a non-owning view of the client's
  // vector (pointer + size) instead of `const std::vector<std::string>&`, so the caller
  // can read and index the names but cannot change them or learn the container type.
  const auto ops = c.ops();
  REQUIRE(ops.size() == 2);
  REQUIRE(ops[0] == "get");
  REQUIRE(ops[1] == "put");
}

TEST_CASE("two steps to the same target merge into one outgoing flow carrying both ops",
          "[behaviours]") {
  Scenario s = two_hop(StepMode::sequential);
  s.nodes[1].ops["get"] = {Step{.to = "db", .op = "select"},
                           Step{.to = "db", .op = "count", .factor = 0.5}};
  const auto g = *simsys::graph::Graph::build(s);
  Flow in;
  in.add("get", Rate{10});
  const auto routed = ServiceBehaviour{g, *g.index_of("web")}.route(in, 100ms);
  REQUIRE(routed.outgoing.size() == 1);
  REQUIRE_THAT(routed.outgoing[0].flow.rate_of("select").per_second, WithinRel(10.0, 1e-12));
  REQUIRE_THAT(routed.outgoing[0].flow.rate_of("count").per_second, WithinRel(5.0, 1e-12));
}

TEST_CASE("an unavailable entry node makes the client's latency infinite, never zero-variance",
          "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(two_hop(StepMode::sequential));
  const ClientBehaviour c{g, *g.index_of("users")};
  const double inf = std::numeric_limits<double>::infinity();
  const auto r =
      c.response("get", [&](std::size_t, std::string_view) { return LatencyEstimate{inf, inf}; });
  REQUIRE(std::isinf(r.mean_s));
  REQUIRE(std::isinf(r.variance_s2));
}

TEST_CASE("the hop that closes a call cycle forwards flow but adds no latency to the caller",
          "[behaviours]") {
  // chat.msg -> q.publish -> sync.job -> chat.msg: a true call cycle; sync -> chat closes it.
  Scenario s{.name = "loop2", .version = 1, .duration = 1min};
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{1}}, .op_shares = {{"msg", 1.0}}}},
      NodeSpec{.id = "chat",
               .type = NodeType::service,
               .ops = {{"msg", {Step{.to = "q", .op = "publish"}}}}},
      NodeSpec{.id = "q",
               .type = NodeType::service,
               .ops = {{"publish", {Step{.to = "sync", .op = "job"}}}}},
      NodeSpec{.id = "sync",
               .type = NodeType::service,
               .ops = {{"job", {Step{.to = "chat", .op = "msg", .factor = 0.5}}}}}};
  s.links = {LinkSpec{.from = "u", .to = "chat", .latency = 0s},
             LinkSpec{.from = "chat", .to = "q", .latency = 0s},
             LinkSpec{.from = "q", .to = "sync", .latency = 0s},
             LinkSpec{.from = "sync", .to = "chat", .latency = 0s}};
  const auto g = *simsys::graph::Graph::build(s);
  const ServiceBehaviour sync{g, *g.index_of("sync")};
  Flow in;
  in.add("job", Rate{3});
  REQUIRE(sync.route(in, 100ms).outgoing.size() == 1);  // flow still goes round
  const auto r =
      sync.response("job", LatencyEstimate{0.002, 0.0},
                    [](std::size_t, std::string_view) { return LatencyEstimate{99.0, 99.0}; });
  REQUIRE(r == LatencyEstimate{0.002, 0.0});  // the hop back to chat is asynchronous
}

TEST_CASE("a node-level back edge that is not a call cycle still adds its latency",
          "[behaviours]") {
  // A.x -> B.y and B.z -> A.w: B -> A closes the node cycle, but B.z really waits for A.w.
  Scenario s{.name = "xcycle", .version = 1, .duration = 1min};
  s.nodes = {
      NodeSpec{
          .id = "u",
          .type = NodeType::client,
          .workload = Workload{.curve = {.base = Rate{10}}, .op_shares = {{"x", 0.5}, {"z", 0.5}}}},
      NodeSpec{.id = "A", .type = NodeType::service, .ops = {{"x", {Step{.to = "B", .op = "y"}}}}},
      NodeSpec{.id = "B", .type = NodeType::service, .ops = {{"z", {Step{.to = "A", .op = "w"}}}}}};
  s.links = {LinkSpec{.from = "u", .to = "A", .ops = {"x"}, .latency = 0s},
             LinkSpec{.from = "u", .to = "B", .ops = {"z"}, .latency = 0s},
             LinkSpec{.from = "A", .to = "B", .latency = 0s},
             LinkSpec{.from = "B", .to = "A", .latency = 0s}};
  const auto g = *simsys::graph::Graph::build(s);
  const ServiceBehaviour b{g, *g.index_of("B")};
  const auto r = b.response("z", LatencyEstimate{0.001, 0.0}, [](std::size_t, std::string_view) {
    return LatencyEstimate{0.5, 0.0};
  });
  REQUIRE_THAT(r.mean_s, WithinRel(0.001 + 0.0 + 0.5 + 0.0, 1e-12));  // own + two zero legs + A.w
}

TEST_CASE("a client with a zero base rate offers nothing", "[behaviours]") {
  Scenario s = two_hop(StepMode::sequential);
  s.nodes[0].workload->curve.base = Rate{0};
  const auto g = *simsys::graph::Graph::build(s);
  REQUIRE(ClientBehaviour{g, *g.index_of("users"), 0}.offer(0s).outgoing.empty());
}

TEST_CASE("asking a client for an op it does not send is a programmer error", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(two_hop(StepMode::sequential));
  const ClientBehaviour c{g, *g.index_of("users")};
  REQUIRE_THROWS_AS(
      c.response("nope", [](std::size_t, std::string_view) { return LatencyEstimate{}; }),
      std::invalid_argument);
}

TEST_CASE("a client offers the curve's rate at the step's time", "[behaviours]") {
  Scenario s = two_hop(StepMode::sequential);
  s.nodes[0].workload->curve.spikes = {
      simsys::workload::Spike{.at = 1s, .duration = 1s, .factor = 3}};
  const auto g = *simsys::graph::Graph::build(s);
  ClientBehaviour c{g, *g.index_of("users"), simsys::behaviours::client_seed(1, 0)};
  REQUIRE_THAT(c.offer(0s).outgoing[0].flow.rate_of("get").per_second, WithinRel(80.0, 1e-12));
  REQUIRE_THAT(c.offer(1s).outgoing[0].flow.rate_of("get").per_second, WithinRel(240.0, 1e-12));
}

TEST_CASE("noise applies only after a tick and is deterministic per seed", "[behaviours]") {
  Scenario s = two_hop(StepMode::sequential);
  s.nodes[0].workload->noise = simsys::workload::Noise{.sigma = 0.3, .correlation = 5s};
  const auto g = *simsys::graph::Graph::build(s);
  ClientBehaviour a{g, 0, 9}, b{g, 0, 9}, c{g, 0, 10};
  REQUIRE_THAT(a.offer(0s).outgoing[0].flow.total().per_second,
               WithinRel(100.0, 1e-12));  // no tick yet
  a.tick(0s, 100ms);
  b.tick(0s, 100ms);
  c.tick(0s, 100ms);
  REQUIRE(a.offer(0s).outgoing[0].flow.total() == b.offer(0s).outgoing[0].flow.total());
  REQUIRE(a.offer(0s).outgoing[0].flow.total() != c.offer(0s).outgoing[0].flow.total());
}

TEST_CASE("client_seed separates seeds and clients", "[behaviours]") {
  using simsys::behaviours::client_seed;
  REQUIRE(client_seed(1, 0) != client_seed(2, 0));
  REQUIRE(client_seed(1, 0) != client_seed(1, 1));
  REQUIRE(client_seed(1, 0) == client_seed(1, 0));
}

TEST_CASE("client_seed never gives two seed and client pairs the same engine seed",
          "[behaviours]") {
  using simsys::behaviours::client_seed;
  REQUIRE(client_seed(1, 0) != client_seed(0, 1));  // plain seed ^ index would collide here
  std::vector<std::uint64_t> seeds;
  for (int seed = -100; seed <= 100; ++seed)
    for (std::size_t i = 0; i < 64; ++i) seeds.push_back(client_seed(seed, i));
  std::ranges::sort(seeds);
  REQUIRE(std::ranges::adjacent_find(seeds) == seeds.end());
}

TEST_CASE("a service's cost per op comes from its spec and defaults to one", "[behaviours]") {
  Scenario s = two_hop(StepMode::sequential);
  s.nodes[1].cost = {{"get", 2.5}};
  const auto g = *simsys::graph::Graph::build(s);
  const ServiceBehaviour web{g, *g.index_of("web")};
  REQUIRE(web.cost("get") == 2.5);
  REQUIRE(web.cost("put") == 1.0);
}

TEST_CASE("a load balancer forwards every incoming op across its routes and serves nothing itself",
          "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(weighted_lb());
  LoadBalancerBehaviour lb{g, *g.index_of("lb")};
  Flow in;
  in.add("get", Rate{100});
  const auto routed = lb.route(in, 100ms);
  REQUIRE(routed.outgoing.size() == 2);
  const auto to_a = std::ranges::find(routed.outgoing, *g.index_of("a"), &Outgoing::target);
  const auto to_b = std::ranges::find(routed.outgoing, *g.index_of("b"), &Outgoing::target);
  REQUIRE(to_a != routed.outgoing.end());
  REQUIRE(to_b != routed.outgoing.end());
  REQUIRE_THAT(to_a->flow.rate_of("get").per_second, WithinRel(75.0, 1e-12));
  REQUIRE_THAT(to_b->flow.rate_of("get").per_second, WithinRel(25.0, 1e-12));
  // It handled every request (its own tiny service time), though it answers none itself.
  REQUIRE_THAT(routed.served.total().per_second, WithinRel(100.0, 1e-12));
}

TEST_CASE(
    "a load balancer's response is its own sojourn plus the share-weighted downstream mixture",
    "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(weighted_lb());
  const LoadBalancerBehaviour lb{g, *g.index_of("lb")};
  const auto r =
      lb.response("get", LatencyEstimate{0.0001, 0.0}, [&](std::size_t node, std::string_view) {
        return node == *g.index_of("a") ? LatencyEstimate{0.001, 0.0} : LatencyEstimate{0.005, 0.0};
      });
  REQUIRE_THAT(r.mean_s, WithinRel(0.0001 + 0.75 * 0.001 + 0.25 * 0.005, 1e-12));
  // Between-branch variance of a two-point mixture: s_a s_b (m_a - m_b)^2; each branch is
  // deterministic and the links add nothing, so all of the variance is the spread between them.
  REQUIRE_THAT(r.variance_s2, WithinRel(0.75 * 0.25 * (0.004 * 0.004), 1e-9));
}
