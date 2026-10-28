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
#include <optional>
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
using simsys::behaviours::RateLimiterBehaviour;
using simsys::behaviours::ServiceBehaviour;
using simsys::flow::Flow;
using simsys::flow::LatencyEstimate;
using simsys::model::LbPolicy;
using simsys::model::LimiterAlgorithm;
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
// A rate limiter of the given algorithm and limit; burst, backend and window only when given.
// Designated initializers must follow declaration order: algorithm, limit, burst, window, backend.
NodeSpec limiter_spec(LimiterAlgorithm algorithm, double limit_per_s,
                      std::optional<int> burst = std::nullopt,
                      std::optional<std::string> backend = std::nullopt,
                      std::optional<simsys::SimTime> window = std::nullopt) {
  return NodeSpec{.id = "limiter",
                  .type = NodeType::rate_limiter,
                  .algorithm = algorithm,
                  .limit = Rate{limit_per_s},
                  .burst = burst,
                  .window = window,
                  .backend = std::move(backend)};
}
// u -> limiter -> api, and, when the limiter names a backend, limiter -> counters (a cache).
// Node indices: u 0, limiter 1, api 2, counters 3. The links to the limiter and the api
// have zero latency unless a leg is given; plain links (no `ops` filter), as in the lab.
Scenario limiter_front(NodeSpec limiter,
                       std::vector<std::pair<std::string, double>> op_shares = {{"call", 1.0}},
                       simsys::SimTime backend_leg = 0s, simsys::SimTime api_leg = 0s) {
  const bool with_backend = limiter.backend.has_value();
  Scenario s{.name = "limiter", .version = 1, .duration = 1min};
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{100}},
                                           .op_shares = std::move(op_shares)}},
             std::move(limiter), NodeSpec{.id = "api", .type = NodeType::service}};
  s.links = {LinkSpec{.from = "u", .to = "limiter", .latency = 0s},
             LinkSpec{.from = "limiter", .to = "api", .latency = api_leg}};
  if (with_backend) {
    s.nodes.push_back(NodeSpec{.id = "counters", .type = NodeType::cache});
    s.links.push_back(LinkSpec{.from = "limiter", .to = "counters", .latency = backend_leg});
  }
  return s;
}
// A flow of one op at `per_second`.
Flow flow_of(std::string_view op, double per_second) {
  Flow f;
  f.add(op, Rate{per_second});
  return f;
}
}  // namespace

// C++20 concepts checked at compile time: the behaviours must satisfy the engine's
// requirements, or this file does not compile.
static_assert(simsys::behaviours::NodeBehaviour<ServiceBehaviour>);
static_assert(simsys::behaviours::RequestSource<ClientBehaviour>);
static_assert(simsys::behaviours::NodeBehaviour<LoadBalancerBehaviour>);
static_assert(simsys::behaviours::NodeBehaviour<RateLimiterBehaviour>);

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

TEST_CASE(
    "a load balancer splits each op by the renormalised link weights and accepts every request",
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
  // `served` is what the behaviour accepted: a balancer rejects nothing, so all of its input.
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

// ---- RateLimiterBehaviour ----

// Token bucket (ch. 4): a bucket of `burst` tokens refilled at `limit` per second; a request
// takes a token or is rejected. In the fluid model one step of length dt admits
// min(in, limit + L/dt) with L the tokens in the bucket, then L += (limit - admitted) dt,
// kept in [0, burst]. Steady 800/s under a 1000/s limit leaves 200/s of refill: +20 tokens per
// 100 ms step until the bucket is full at 500, so 100 steps fill it whatever its start.
TEST_CASE("a token bucket admits the limit plus what the bucket holds and rejects the rest",
          "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(
      limiter_front(limiter_spec(LimiterAlgorithm::token_bucket, 1000, 500)));
  RateLimiterBehaviour lim{g, *g.index_of("limiter")};
  const Flow in = flow_of("call", 800);
  for (int i = 0; i < 100; ++i) {
    const auto steady = lim.route(in, 100ms);
    REQUIRE_THAT(steady.served.total().per_second, WithinRel(800.0, 1e-12));  // all admitted
    REQUIRE(steady.rejected.total().per_second == 0.0);
  }
  // Bucket full (500). A 4000/s burst: step 1 admits min(4000, 1000 + 500/0.1) = 4000 and the
  // bucket drops by (4000 - 1000) x 0.1 = 300 to 200; step 2 admits 1000 + 200/0.1 = 3000 and
  // empties it; from step 3 on only the limit, 1000/s, gets through.
  const Flow burst = flow_of("call", 4000);
  const auto r1 = lim.route(burst, 100ms);
  REQUIRE_THAT(r1.served.total().per_second, WithinRel(4000.0, 1e-12));
  REQUIRE(r1.rejected.empty());
  const auto r2 = lim.route(burst, 100ms);
  REQUIRE_THAT(r2.served.total().per_second, WithinRel(3000.0, 1e-12));
  REQUIRE_THAT(r2.rejected.total().per_second, WithinRel(1000.0, 1e-12));
  const auto r3 = lim.route(burst, 100ms);
  REQUIRE_THAT(r3.served.total().per_second, WithinRel(1000.0, 1e-12));
  REQUIRE_THAT(r3.rejected.total().per_second, WithinRel(3000.0, 1e-12));
  const auto r4 = lim.route(burst, 100ms);
  REQUIRE_THAT(r4.served.total().per_second, WithinRel(1000.0, 1e-12));  // and it stays there
  REQUIRE_THAT(r4.rejected.total().per_second, WithinRel(3000.0, 1e-12));
}

TEST_CASE("a token bucket refills while idle: a quiet step restores what a burst took",
          "[behaviours]") {
  // Drained by two 4000/s steps (burst 500 -> 200 -> 0). Idle at 0/s for 3 steps refills
  // 1000 x 0.1 = 100 tokens each, 300 in all; a 4000/s step then admits
  // 1000 + 300/0.1 = 4000 again, which an unrefilled bucket would cap at 1000.
  const auto g = *simsys::graph::Graph::build(
      limiter_front(limiter_spec(LimiterAlgorithm::token_bucket, 1000, 500)));
  RateLimiterBehaviour lim{g, *g.index_of("limiter")};
  const Flow burst = flow_of("call", 4000);
  for (int i = 0; i < 102; ++i) (void)lim.route(i < 100 ? flow_of("call", 800) : burst, 100ms);
  for (int i = 0; i < 3; ++i) (void)lim.route(Flow{}, 100ms);
  const auto after_idle = lim.route(burst, 100ms);
  REQUIRE_THAT(after_idle.served.total().per_second, WithinRel(4000.0, 1e-9));
  REQUIRE(after_idle.rejected.total().per_second < 1e-6);
}

TEST_CASE("a limiter with no incoming flow admits and rejects nothing", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(
      limiter_front(limiter_spec(LimiterAlgorithm::token_bucket, 1000, 500)));
  RateLimiterBehaviour lim{g, *g.index_of("limiter")};
  const auto routed = lim.route(Flow{}, 100ms);
  REQUIRE(routed.outgoing.empty());
  REQUIRE(routed.served.empty());
  REQUIRE(routed.rejected.empty());
}

TEST_CASE("admitted flow is forwarded downstream op by op; rejected flow is not", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(limiter_front(
      limiter_spec(LimiterAlgorithm::token_bucket, 1000, 500), {{"a", 0.5}, {"b", 0.5}}));
  RateLimiterBehaviour lim{g, *g.index_of("limiter")};
  Flow idle;
  idle.add("a", Rate{400});
  idle.add("b", Rate{400});
  for (int i = 0; i < 100; ++i) (void)lim.route(idle, 100ms);  // bucket full
  Flow burst;
  burst.add("a", Rate{2000});
  burst.add("b", Rate{2000});                   // 4000/s in all, split 50/50
  (void)lim.route(burst, 100ms);                // admits 4000, bucket 500 -> 200
  (void)lim.route(burst, 100ms);                // admits 3000, bucket -> 0
  const auto routed = lim.route(burst, 100ms);  // admits the limit only: 1000 of 4000
  // The admitted 1000/s keeps the op mix: 500 of each op goes on to the api, in one flow.
  REQUIRE(routed.outgoing.size() == 1);
  const auto to_api = std::ranges::find(routed.outgoing, *g.index_of("api"), &Outgoing::target);
  REQUIRE(to_api != routed.outgoing.end());
  REQUIRE_THAT(to_api->flow.rate_of("a").per_second, WithinRel(500.0, 1e-12));
  REQUIRE_THAT(to_api->flow.rate_of("b").per_second, WithinRel(500.0, 1e-12));
  // The rest is shed per op too, and nothing of it is forwarded: what reaches the api plus
  // what is rejected is exactly what came in.
  REQUIRE_THAT(routed.rejected.rate_of("a").per_second, WithinRel(1500.0, 1e-12));
  REQUIRE_THAT(routed.rejected.rate_of("b").per_second, WithinRel(1500.0, 1e-12));
  REQUIRE_THAT(routed.served.total().per_second, WithinRel(1000.0, 1e-12));
  REQUIRE_THAT(to_api->flow.total().per_second + routed.rejected.total().per_second,
               WithinRel(4000.0, 1e-12));
}

// Fixed window, sliding log and sliding counter (ch. 4) differ in how exactly they count the
// last `window` seconds, not in how much they let through in a burst: at most limit x window
// requests per window. A fluid model keeps that one number: a token bucket of burst
// limit x window (plan 2a refinement 6). Here limit 1000/s and window 1 s: burst 1000, twice
// the 500 of the token-bucket test above, so the same 4000/s burst lasts a step longer.
TEST_CASE("a windowed algorithm is a token bucket whose burst is limit times window",
          "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(limiter_front(
      limiter_spec(LimiterAlgorithm::fixed_window, 1000, std::nullopt, std::nullopt, 1s)));
  RateLimiterBehaviour lim{g, *g.index_of("limiter")};
  const Flow idle = flow_of("call", 800);
  for (int i = 0; i < 100; ++i) (void)lim.route(idle, 100ms);  // window budget full: 1000
  const Flow burst = flow_of("call", 4000);
  // Budget (tokens) before each 4000/s step: 1000, 700, 400, 100, 0. A step admits
  // min(4000, 1000 + budget/0.1) and spends (admitted - 1000) x 0.1: steps 1-3 admit all
  // 4000 (1000 + 1000/0.1 = 11000, 8000, 5000 all exceed 4000), step 4 admits
  // 1000 + 100/0.1 = 2000, step 5 only the limit. A 500-token bucket would stop at step 3.
  for (int i = 0; i < 2; ++i) {
    const auto r = lim.route(burst, 100ms);
    REQUIRE_THAT(r.served.total().per_second, WithinRel(4000.0, 1e-9));
  }
  const auto r3 = lim.route(burst, 100ms);  // 400 left: 1000 + 400/0.1 = 5000 >= 4000
  REQUIRE_THAT(r3.served.total().per_second, WithinRel(4000.0, 1e-9));
  REQUIRE(r3.rejected.total().per_second < 1e-6);
  const auto r4 = lim.route(burst, 100ms);  // 100 left: 1000 + 100/0.1 = 2000
  REQUIRE_THAT(r4.served.total().per_second, WithinRel(2000.0, 1e-9));
  REQUIRE_THAT(r4.rejected.total().per_second, WithinRel(2000.0, 1e-9));
  const auto r5 = lim.route(burst, 100ms);  // empty: the limit only
  REQUIRE_THAT(r5.served.total().per_second, WithinRel(1000.0, 1e-9));
  REQUIRE_THAT(r5.rejected.total().per_second, WithinRel(3000.0, 1e-9));
}

TEST_CASE("every windowed algorithm gives the same fluid behaviour at the same window",
          "[behaviours]") {
  // Window 500 ms: burst = 1000 x 0.5 = 500, the token bucket of the first test. From full,
  // two 4000/s steps admit 4000 then 3000, the third only the limit.
  for (const auto algorithm : {LimiterAlgorithm::fixed_window, LimiterAlgorithm::sliding_log,
                               LimiterAlgorithm::sliding_counter}) {
    const auto g = *simsys::graph::Graph::build(
        limiter_front(limiter_spec(algorithm, 1000, std::nullopt, std::nullopt, 500ms)));
    RateLimiterBehaviour lim{g, *g.index_of("limiter")};
    for (int i = 0; i < 100; ++i) (void)lim.route(flow_of("call", 800), 100ms);
    const Flow burst = flow_of("call", 4000);
    REQUIRE_THAT(lim.route(burst, 100ms).served.total().per_second, WithinRel(4000.0, 1e-9));
    REQUIRE_THAT(lim.route(burst, 100ms).served.total().per_second, WithinRel(3000.0, 1e-9));
    REQUIRE_THAT(lim.route(burst, 100ms).served.total().per_second, WithinRel(1000.0, 1e-9));
  }
}

// Leaky bucket (ch. 4): requests join a FIFO queue that drains at a fixed rate; only a full
// queue rejects. That is a queue with capacity `limit` and room `burst`, which the engine's
// FluidQueue already is, so the behaviour itself rejects nothing and forwards what it is
// given; the engine's queue shapes the output (see the leaky-bucket case in test_theory.cpp).
TEST_CASE("a leaky bucket queues the excess instead of rejecting it", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(
      limiter_front(limiter_spec(LimiterAlgorithm::leaky_bucket, 1000, 500)));
  RateLimiterBehaviour lim{g, *g.index_of("limiter")};
  const Flow in = flow_of("call", 1500);
  for (int i = 0; i < 20; ++i) {  // past the point where a 500-request queue would be full
    const auto routed = lim.route(in, 100ms);
    REQUIRE(routed.rejected.empty());
    REQUIRE_THAT(routed.served.total().per_second, WithinRel(1500.0, 1e-12));
    REQUIRE(routed.outgoing.size() == 1);
    REQUIRE_THAT(routed.outgoing[0].flow.total().per_second, WithinRel(1500.0, 1e-12));
  }
}

TEST_CASE("a limiter with a backend sends one counter op per admitted request", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(
      limiter_front(limiter_spec(LimiterAlgorithm::token_bucket, 1000, 500, "counters")));
  RateLimiterBehaviour lim{g, *g.index_of("limiter")};
  const auto counters = *g.index_of("counters");
  const auto api = *g.index_of("api");
  for (int i = 0; i < 100; ++i) (void)lim.route(flow_of("call", 800), 100ms);
  const auto steady = lim.route(flow_of("call", 800), 100ms);
  REQUIRE(steady.outgoing.size() == 2);
  const auto to_counters = std::ranges::find(steady.outgoing, counters, &Outgoing::target);
  const auto to_api = std::ranges::find(steady.outgoing, api, &Outgoing::target);
  REQUIRE(to_counters != steady.outgoing.end());
  REQUIRE(to_api != steady.outgoing.end());
  REQUIRE_THAT(to_counters->flow.rate_of("counter").per_second, WithinRel(800.0, 1e-12));
  REQUIRE(to_counters->flow.rate_of("call").per_second == 0.0);  // the call itself goes to the api
  REQUIRE_THAT(to_api->flow.rate_of("call").per_second, WithinRel(800.0, 1e-12));
  REQUIRE(to_api->flow.rate_of("counter").per_second == 0.0);

  // Drain the bucket with 4000/s bursts; once only the limit gets through, the counter store
  // is asked 1000 times a second, for the admitted requests alone, not for the 4000 offered
  // and not for the 3000 rejected.
  const Flow burst = flow_of("call", 4000);
  (void)lim.route(burst, 100ms);
  (void)lim.route(burst, 100ms);
  const auto limited = lim.route(burst, 100ms);
  const auto limited_counters = std::ranges::find(limited.outgoing, counters, &Outgoing::target);
  const auto limited_api = std::ranges::find(limited.outgoing, api, &Outgoing::target);
  REQUIRE(limited_counters != limited.outgoing.end());
  REQUIRE(limited_api != limited.outgoing.end());
  REQUIRE_THAT(limited_counters->flow.rate_of("counter").per_second, WithinRel(1000.0, 1e-12));
  REQUIRE_THAT(limited_api->flow.rate_of("call").per_second, WithinRel(1000.0, 1e-12));
  REQUIRE_THAT(limited.rejected.total().per_second, WithinRel(3000.0, 1e-12));
}

TEST_CASE("a limiter's response adds the counter store hop and the api hop to its own sojourn",
          "[behaviours]") {
  // Links: limiter -> counters 1 ms one way, limiter -> api 3 ms one way. The counter check
  // happens before the request is passed on, so the two hops add (sequentially).
  const auto g = *simsys::graph::Graph::build(
      limiter_front(limiter_spec(LimiterAlgorithm::token_bucket, 1000, 500, "counters"),
                    {{"call", 1.0}}, 1ms, 3ms));
  const RateLimiterBehaviour lim{g, *g.index_of("limiter")};
  const auto counters = *g.index_of("counters");
  std::vector<std::pair<std::size_t, std::string>> asked;
  const auto r = lim.response(
      "call", LatencyEstimate{0.001, 2e-7}, [&](std::size_t node, std::string_view op) {
        asked.emplace_back(node, std::string{op});
        return node == counters ? LatencyEstimate{0.0002, 1e-8} : LatencyEstimate{0.004, 1e-6};
      });
  // own 1 ms + counters (2 x 1 ms + 0.2 ms) + api (2 x 3 ms + 4 ms) = 13.2 ms; the variances
  // of sequential parts add: 2e-7 + 1e-8 + 1e-6.
  REQUIRE_THAT(r.mean_s, WithinRel(0.0132, 1e-12));
  REQUIRE_THAT(r.variance_s2, WithinRel(1.21e-6, 1e-9));
  // The counter store is asked about the `counter` op, the api about the call itself.
  REQUIRE(std::ranges::find(asked, std::pair{counters, std::string{"counter"}}) != asked.end());
  REQUIRE(std::ranges::find(asked, std::pair{*g.index_of("api"), std::string{"call"}}) !=
          asked.end());
  REQUIRE(asked.size() == 2);
}

TEST_CASE("a limiter without a backend adds only the api hop to its own sojourn", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(limiter_front(
      limiter_spec(LimiterAlgorithm::token_bucket, 1000, 500), {{"call", 1.0}}, 0s, 3ms));
  const RateLimiterBehaviour lim{g, *g.index_of("limiter")};
  const auto r =
      lim.response("call", LatencyEstimate{0.001, 0.0},
                   [](std::size_t, std::string_view) { return LatencyEstimate{0.004, 1e-6}; });
  REQUIRE_THAT(r.mean_s, WithinRel(0.001 + 0.006 + 0.004, 1e-12));
  REQUIRE_THAT(r.variance_s2, WithinRel(1e-6, 1e-9));
}
