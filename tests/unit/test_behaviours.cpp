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
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using simsys::Rate;
using simsys::behaviours::BlobStoreBehaviour;
using simsys::behaviours::CacheBehaviour;
using simsys::behaviours::CdnBehaviour;
using simsys::behaviours::ClientBehaviour;
using simsys::behaviours::LoadBalancerBehaviour;
using simsys::behaviours::Outgoing;
using simsys::behaviours::RateLimiterBehaviour;
using simsys::behaviours::ServiceBehaviour;
using simsys::behaviours::SqlDbBehaviour;
using simsys::flow::Flow;
using simsys::flow::LatencyEstimate;
using simsys::model::CacheStrategy;
using simsys::model::HitRatio;
using simsys::model::LbPolicy;
using simsys::model::LimiterAlgorithm;
using simsys::model::LinkSpec;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
using simsys::model::Step;
using simsys::model::StepCondition;
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
// u -> limiter -> api, and, when the limiter names a backend, limiter -> counters (a cache, whose
// write_ops name `counter`: an INCR is a write, and Graph::build refuses a store that counts it
// as a read).
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
    s.nodes.push_back(NodeSpec{.id = "counters",
                               .type = NodeType::cache,
                               .write_ops = std::vector<std::string>{"counter"}});
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
static_assert(simsys::behaviours::NodeBehaviour<CacheBehaviour>);
static_assert(simsys::behaviours::NodeBehaviour<CdnBehaviour>);
static_assert(simsys::behaviours::NodeBehaviour<BlobStoreBehaviour>);

// And the negative: a type that routes flow but cannot answer for its latency is not a
// NodeBehaviour, so the engine refuses it at compile time instead of misbehaving at run time.
struct NoResponse {
  simsys::behaviours::Routed route(const Flow&, simsys::SimTime, simsys::SimTime) const;
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
  const auto routed = web.route(in, 0s, 100ms);
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
  const auto routed = web.route(Flow{}, 0s, 100ms);
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
  const auto routed = ServiceBehaviour{g, *g.index_of("web")}.route(in, 0s, 100ms);
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
  REQUIRE(sync.route(in, 0s, 100ms).outgoing.size() == 1);  // flow still goes round
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
  const auto routed = lb.route(in, 0s, 100ms);
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
    const auto steady = lim.route(in, 0s, 100ms);
    REQUIRE_THAT(steady.served.total().per_second, WithinRel(800.0, 1e-12));  // all admitted
    REQUIRE(steady.rejected.total().per_second == 0.0);
  }
  // Bucket full (500). A 4000/s burst: step 1 admits min(4000, 1000 + 500/0.1) = 4000 and the
  // bucket drops by (4000 - 1000) x 0.1 = 300 to 200; step 2 admits 1000 + 200/0.1 = 3000 and
  // empties it; from step 3 on only the limit, 1000/s, gets through.
  const Flow burst = flow_of("call", 4000);
  const auto r1 = lim.route(burst, 0s, 100ms);
  REQUIRE_THAT(r1.served.total().per_second, WithinRel(4000.0, 1e-12));
  REQUIRE(r1.rejected.empty());
  const auto r2 = lim.route(burst, 0s, 100ms);
  REQUIRE_THAT(r2.served.total().per_second, WithinRel(3000.0, 1e-12));
  REQUIRE_THAT(r2.rejected.total().per_second, WithinRel(1000.0, 1e-12));
  const auto r3 = lim.route(burst, 0s, 100ms);
  REQUIRE_THAT(r3.served.total().per_second, WithinRel(1000.0, 1e-12));
  REQUIRE_THAT(r3.rejected.total().per_second, WithinRel(3000.0, 1e-12));
  const auto r4 = lim.route(burst, 0s, 100ms);
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
  for (int i = 0; i < 102; ++i) (void)lim.route(i < 100 ? flow_of("call", 800) : burst, 0s, 100ms);
  for (int i = 0; i < 3; ++i) (void)lim.route(Flow{}, 0s, 100ms);
  const auto after_idle = lim.route(burst, 0s, 100ms);
  REQUIRE_THAT(after_idle.served.total().per_second, WithinRel(4000.0, 1e-9));
  REQUIRE(after_idle.rejected.total().per_second < 1e-6);
}

TEST_CASE("a limiter with no incoming flow admits and rejects nothing", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(
      limiter_front(limiter_spec(LimiterAlgorithm::token_bucket, 1000, 500)));
  RateLimiterBehaviour lim{g, *g.index_of("limiter")};
  const auto routed = lim.route(Flow{}, 0s, 100ms);
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
  for (int i = 0; i < 100; ++i) (void)lim.route(idle, 0s, 100ms);  // bucket full
  Flow burst;
  burst.add("a", Rate{2000});
  burst.add("b", Rate{2000});                       // 4000/s in all, split 50/50
  (void)lim.route(burst, 0s, 100ms);                // admits 4000, bucket 500 -> 200
  (void)lim.route(burst, 0s, 100ms);                // admits 3000, bucket -> 0
  const auto routed = lim.route(burst, 0s, 100ms);  // admits the limit only: 1000 of 4000
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

// The three windowed algorithms of ch. 4 allow at most limit x window requests per window;
// they differ in WHICH window they count, and in a fluid stepped every dt that difference is
// visible at the step level (plan 2a refinement 6). The tests below flood a 1000/s limit with
// 16k/s (the lab's 20x spike on 800/s) at 100 ms steps, window 1 s: W = limit x window = 1000
// requests per window, and one step at 16k/s brings 1600, more than a whole window's worth.
// The step's start `now` is passed in, so window boundaries fall where the clock puts them.
namespace {
// Admitted rate of each of `steps` steps of length dt from t = 0, inflow `demand(k)` at step k.
template <class Demand>
std::vector<double> admitted_per_step(RateLimiterBehaviour& lim, std::size_t steps, Demand demand) {
  std::vector<double> admitted;
  for (std::size_t k = 0; k < steps; ++k) {
    const auto now = static_cast<long>(k) * 100ms;
    const auto routed = lim.route(flow_of("call", demand(k)), now, 100ms);
    admitted.push_back(routed.served.total().per_second);
  }
  return admitted;
}
RateLimiterBehaviour windowed(const simsys::graph::Graph& g) {
  return RateLimiterBehaviour{g, *g.index_of("limiter")};
}
}  // namespace

// Fixed window: a counter per calendar window [k·window, (k+1)·window) that resets at each
// boundary. Under a flood the first step of a window takes the whole budget (1000 requests
// in 100 ms = 10000/s) and the other nine get nothing: the fluid version of ch. 4's "the
// window's quota is gone in its first moments".
TEST_CASE(
    "a fixed window admits a whole window at its start under a flood and nothing until the next",
    "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(limiter_front(
      limiter_spec(LimiterAlgorithm::fixed_window, 1000, std::nullopt, std::nullopt, 1s)));
  auto lim = windowed(g);
  const auto admitted = admitted_per_step(lim, 30, [](std::size_t) { return 16'000.0; });
  for (std::size_t k = 0; k < 30; ++k) {
    CAPTURE(k);
    if (k % 10 == 0) {
      REQUIRE_THAT(admitted[k], WithinRel(10'000.0, 1e-12));
    } else {
      REQUIRE(admitted[k] == 0.0);
    }
  }
}

// Ch. 4's flaw of the fixed window: a burst that straddles a boundary gets the end of one
// window AND the start of the next. Idle until t = 0.5 s, then a flood: step 5 takes window
// 0's whole budget, step 10 (t = 1 s) window 1's, so the second [0.5 s, 1.5 s) lets 2000
// requests through, twice limit x window.
TEST_CASE("a fixed window lets twice the limit through across a window boundary", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(limiter_front(
      limiter_spec(LimiterAlgorithm::fixed_window, 1000, std::nullopt, std::nullopt, 1s)));
  auto lim = windowed(g);
  const auto admitted =
      admitted_per_step(lim, 15, [](std::size_t k) { return k < 5 ? 0.0 : 16'000.0; });
  double in_straddling_second = 0.0;
  for (std::size_t k = 5; k < 15; ++k) in_straddling_second += admitted[k] * 0.1;
  REQUIRE_THAT(in_straddling_second, WithinRel(2000.0, 1e-12));
}

// Sliding log: every admitted request's timestamp is kept and the log of the last `window`
// seconds is counted. In the fluid the log is a ring of the last ceil(window/dt) steps'
// admitted counts; a step admits what fits under W beside the previous ten-minus-one steps.
// So NO window of ten consecutive steps ever holds more than W, wherever the flood starts.
TEST_CASE("a sliding log never admits more than limit x window in any window", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(limiter_front(
      limiter_spec(LimiterAlgorithm::sliding_log, 1000, std::nullopt, std::nullopt, 1s)));
  for (const std::size_t flood_from : {std::size_t{0}, std::size_t{5}}) {
    CAPTURE(flood_from);
    auto lim = windowed(g);
    const auto admitted =
        admitted_per_step(lim, 40, [&](std::size_t k) { return k < flood_from ? 0.0 : 16'000.0; });
    REQUIRE_THAT(admitted[flood_from], WithinRel(10'000.0, 1e-12));  // the whole budget at once
    for (std::size_t first = 0; first + 10 <= 40; ++first) {
      double in_window = 0.0;
      for (std::size_t k = first; k < first + 10; ++k) in_window += admitted[k] * 0.1;
      CAPTURE(first);
      REQUIRE(in_window <= 1000.0 + 1e-9);
    }
  }
}

// Sliding counter: the current window's count plus the previous window's, weighted by how
// much of the previous window the trailing `window` still overlaps:
//   estimate = prev · (1 − f) + cur,   f = (now mod window) / window
// A request is admitted while estimate < W. Under a flood the previous window's full count
// keeps the estimate high early in each window, so the long-run admitted rate settles below
// the limit: 10/11 of it with ten steps per window (windows.py of the review reproduces it).
TEST_CASE("a sliding counter smooths to about 0.9 of the limit under a flood", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(limiter_front(
      limiter_spec(LimiterAlgorithm::sliding_counter, 1000, std::nullopt, std::nullopt, 1s)));
  auto lim = windowed(g);
  const auto admitted = admitted_per_step(lim, 2000, [](std::size_t) { return 16'000.0; });
  double sum = 0.0;
  for (std::size_t k = 1000; k < 2000; ++k) sum += admitted[k];
  REQUIRE_THAT(sum / 1000.0, WithinRel(1000.0 * 10.0 / 11.0, 1e-3));
}

TEST_CASE("below its limit every windowed algorithm admits everything", "[behaviours]") {
  for (const auto algorithm : {LimiterAlgorithm::fixed_window, LimiterAlgorithm::sliding_log,
                               LimiterAlgorithm::sliding_counter}) {
    CAPTURE(algorithm);
    const auto g = *simsys::graph::Graph::build(
        limiter_front(limiter_spec(algorithm, 1000, std::nullopt, std::nullopt, 1s)));
    auto lim = windowed(g);
    for (const double a : admitted_per_step(lim, 50, [](std::size_t) { return 800.0; })) {
      REQUIRE_THAT(a, WithinRel(800.0, 1e-12));
    }
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
    const auto routed = lim.route(in, 0s, 100ms);
    REQUIRE(routed.rejected.empty());
    REQUIRE_THAT(routed.served.total().per_second, WithinRel(1500.0, 1e-12));
    REQUIRE(routed.outgoing.size() == 1);
    REQUIRE_THAT(routed.outgoing[0].flow.total().per_second, WithinRel(1500.0, 1e-12));
  }
}

// Ch. 4: the limiter reads (and bumps) the client's counter for EVERY request, because that
// read is how it decides; a request it then refuses was checked as surely as one it admits.
// So the counter store sees the limiter's whole inflow, not just the admitted part.
TEST_CASE("a limiter with a backend asks the counter store about every request it checks",
          "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(
      limiter_front(limiter_spec(LimiterAlgorithm::token_bucket, 1000, 500, "counters")));
  RateLimiterBehaviour lim{g, *g.index_of("limiter")};
  const auto counters = *g.index_of("counters");
  const auto api = *g.index_of("api");
  for (int i = 0; i < 100; ++i) (void)lim.route(flow_of("call", 800), 0s, 100ms);
  const auto steady = lim.route(flow_of("call", 800), 0s, 100ms);
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
  // is still asked 4000 times a second: 1000 checks that admitted and 3000 that refused.
  const Flow burst = flow_of("call", 4000);
  (void)lim.route(burst, 0s, 100ms);
  (void)lim.route(burst, 0s, 100ms);
  const auto limited = lim.route(burst, 0s, 100ms);
  const auto limited_counters = std::ranges::find(limited.outgoing, counters, &Outgoing::target);
  const auto limited_api = std::ranges::find(limited.outgoing, api, &Outgoing::target);
  REQUIRE(limited_counters != limited.outgoing.end());
  REQUIRE(limited_api != limited.outgoing.end());
  REQUIRE_THAT(limited_counters->flow.rate_of("counter").per_second, WithinRel(4000.0, 1e-12));
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

// A step of zero length would divide the bucket's tokens by zero (limit + tokens / dt): the
// engine never takes one (FluidQueue::advance refuses it too), so it is a programmer error.
TEST_CASE("a limiter refuses a step of zero length", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(
      limiter_front(limiter_spec(LimiterAlgorithm::token_bucket, 1000, 500)));
  RateLimiterBehaviour lim{g, *g.index_of("limiter")};
  REQUIRE_THROWS_AS(lim.route(flow_of("call", 5000), 0s, 0s), std::invalid_argument);
}

// Plan 2a refinement 8: only ANSWERED requests reach the client's latency, so the backward
// pass carries, next to each (node, op)'s latency, the fraction of its requests that come
// back answered. Sequential parts multiply (a request must pass the limiter AND the counter
// check AND the api); a split is the share-weighted mixture of its routes.
TEST_CASE("a limiter delivers its own fraction times the counter check's times its routes'",
          "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(
      limiter_front(limiter_spec(LimiterAlgorithm::token_bucket, 1000, 500, "counters")));
  const RateLimiterBehaviour lim{g, *g.index_of("limiter")};
  const auto counters = *g.index_of("counters");
  const double delivered = lim.delivered("call", 0.25, [&](std::size_t node, std::string_view) {
    return node == counters ? 0.9 : 0.8;
  });
  REQUIRE_THAT(delivered, WithinRel(0.25 * 0.9 * 0.8, 1e-12));
}

TEST_CASE("a service delivers its own fraction times every hop it waits for", "[behaviours]") {
  // web.get calls cache.read and db.select; web.put calls nothing.
  const auto g = *simsys::graph::Graph::build(two_hop(StepMode::parallel));
  const ServiceBehaviour web{g, *g.index_of("web")};
  const auto cache = *g.index_of("cache");
  const auto lookup = [&](std::size_t node, std::string_view) { return node == cache ? 0.5 : 0.9; };
  REQUIRE_THAT(web.delivered("get", 0.8, lookup), WithinRel(0.8 * 0.5 * 0.9, 1e-12));
  REQUIRE_THAT(web.delivered("put", 0.8, lookup), WithinRel(0.8, 1e-12));
}

TEST_CASE("a load balancer's and a client's delivered fraction mix their routes by share",
          "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(weighted_lb());
  const LoadBalancerBehaviour lb{g, *g.index_of("lb")};
  const auto a = *g.index_of("a");
  const auto lookup = [&](std::size_t node, std::string_view) { return node == a ? 1.0 : 0.2; };
  REQUIRE_THAT(lb.delivered("get", 0.5, lookup), WithinRel(0.5 * (0.75 * 1.0 + 0.25 * 0.2), 1e-12));
  const ClientBehaviour users{g, *g.index_of("u")};
  REQUIRE_THAT(users.delivered("get", [](std::size_t, std::string_view) { return 0.4; }),
               WithinRel(0.4, 1e-12));
}

// ---- CacheBehaviour and the on-miss steps of a service (plan 2a refinement 7, Task 10) ----

namespace {
// u -> web -> cache (-> db). Node indices: u 0, web 1, cache 2, db 3 (only with a backend).
// web forwards each of the ops read, write and set to the cache unchanged; the cache has the
// given strategy and fixed hit ratio, and, with a backend, forwards to `db` over a link of
// 1 ms one way. Links to and from web have zero latency.
Scenario cached_front(CacheStrategy strategy, double hit, bool with_backend) {
  Scenario s{.name = "cached", .version = 1, .duration = 1min};
  NodeSpec cache{.id = "cache",
                 .type = NodeType::cache,
                 .hit_ratio = HitRatio{.fixed = hit},
                 .strategy = strategy};
  if (with_backend) cache.backend = "db";
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{100}},
                                    .op_shares = {{"read", 0.6}, {"set", 0.2}, {"write", 0.2}}}},
      NodeSpec{.id = "web",
               .type = NodeType::service,
               .ops = {{"read", {Step{.to = "cache", .op = "read"}}},
                       {"set", {Step{.to = "cache", .op = "set"}}},
                       {"write", {Step{.to = "cache", .op = "write"}}}}},
      std::move(cache)};
  s.links = {LinkSpec{.from = "u", .to = "web", .latency = 0s},
             LinkSpec{.from = "web", .to = "cache", .latency = 0s}};
  if (with_backend) {
    s.nodes.push_back(NodeSpec{.id = "db", .type = NodeType::service});
    s.links.push_back(LinkSpec{.from = "cache", .to = "db", .latency = 1ms});
  }
  return s;
}
// u -> web -> {cache, db} with web.get = [cache.read, db.read on miss]: the cache-aside read
// path. The cache has the given fixed hit ratio; web -> cache is 0.5 ms one way, web -> db
// 10 ms one way. Node indices: u 0, web 1, cache 2, db 3.
Scenario aside_front(double hit) {
  Scenario s{.name = "aside", .version = 1, .duration = 1min};
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{100}}, .op_shares = {{"get", 1.0}}}},
      NodeSpec{.id = "web",
               .type = NodeType::service,
               .ops = {{"get",
                        {Step{.to = "cache", .op = "read"},
                         Step{.to = "db", .op = "read", .on = StepCondition::miss}}}}},
      NodeSpec{.id = "cache",
               .type = NodeType::cache,
               .hit_ratio = HitRatio{.fixed = hit},
               .strategy = CacheStrategy::cache_aside},
      NodeSpec{.id = "db", .type = NodeType::service}};
  s.links = {LinkSpec{.from = "u", .to = "web", .latency = 0s},
             LinkSpec{.from = "web", .to = "cache", .latency = 500us},
             LinkSpec{.from = "web", .to = "db", .latency = 10ms}};
  return s;
}
// u -> web -> {cache, db, db2} with web.get = [cache.read, db.read on miss, db2.read on miss]:
// one lookup and the two fetches its miss needs. Every link has zero latency and the cache the
// given fixed hit ratio. Node indices: u 0, web 1, cache 2, db 3, db2 4.
Scenario aside_two_misses(double hit) {
  Scenario s = aside_front(hit);
  s.nodes[1].ops["get"].push_back(Step{.to = "db2", .op = "read", .on = StepCondition::miss});
  s.nodes.push_back(NodeSpec{.id = "db2", .type = NodeType::service});
  s.links.push_back(LinkSpec{.from = "web", .to = "db2", .latency = 0s});
  for (auto& link : s.links) link.latency = 0s;
  return s;
}
// The flow a routed result sends to `target`, empty when it sends none.
Flow sent_to(const simsys::behaviours::Routed& routed, std::size_t target) {
  const auto it = std::ranges::find(routed.outgoing, target, &Outgoing::target);
  return it == routed.outgoing.end() ? Flow{} : it->flow;
}
}  // namespace

// Cache-aside ("lazy loading"; the pattern ch. 1 draws and calls read-through): the
// application, not the cache, goes to the database on a miss. So the cache node itself only
// ever answers: it serves every request it is sent and forwards nothing; the misses are the
// service's business (the on-miss tests below).
TEST_CASE("a cache_aside cache serves every read itself and forwards nothing", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(cached_front(CacheStrategy::cache_aside, 0.8, false));
  CacheBehaviour cache{g, *g.index_of("cache")};
  const auto routed = cache.route(flow_of("read", 100), 0s, 100ms);
  REQUIRE(routed.outgoing.empty());
  REQUIRE(routed.rejected.empty());
  REQUIRE_THAT(routed.served.rate_of("read").per_second, WithinRel(100.0, 1e-12));
  REQUIRE_THAT(routed.served.total().per_second, WithinRel(100.0, 1e-12));
  // Its response is its own sojourn: whatever the lookup says about other nodes is ignored.
  const LatencyEstimate own{0.0002, 3e-9};
  const auto r = cache.response(
      "read", own, [](std::size_t, std::string_view) { return LatencyEstimate{99.0, 99.0}; });
  REQUIRE(r == own);
  const double delivered =
      cache.delivered("read", 0.9, [](std::size_t, std::string_view) { return 0.1; });
  REQUIRE_THAT(delivered, WithinRel(0.9, 1e-12));
}

// Read-through as this model names it (the cache-driven fetch of general practice, not ch. 1's
// application-driven "read-through"): the cache itself fetches a missing value from the store,
// so the caller sees one call. With a fixed hit ratio h the fluid model sends the miss
// fraction 1 - h of the reads on to the backend, as the same op, and the cache answers every
// request itself (`served` is all of the input, hits and misses alike).
TEST_CASE(
    "a read_through cache forwards its miss fraction to the backend and charges it to the "
    "response",
    "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(cached_front(CacheStrategy::read_through, 0.8, true));
  CacheBehaviour cache{g, *g.index_of("cache")};
  const auto db = *g.index_of("db");
  const auto routed = cache.route(flow_of("read", 100), 0s, 100ms);
  REQUIRE(routed.outgoing.size() == 1);
  REQUIRE(routed.outgoing[0].target == db);
  REQUIRE_THAT(routed.outgoing[0].flow.rate_of("read").per_second, WithinRel(20.0, 1e-12));
  REQUIRE_THAT(routed.outgoing[0].flow.total().per_second, WithinRel(20.0, 1e-12));
  REQUIRE_THAT(routed.served.rate_of("read").per_second, WithinRel(100.0, 1e-12));
  REQUIRE(routed.rejected.empty());

  // Hit (80 %): own sojourn only, mean 1 ms, variance 4e-8. Miss (20 %): own + the hop to the
  // db, 2 legs of 1 ms + the db's response (5 ms, variance 1e-6): mean 8 ms, variance 1.04e-6.
  // A mixture (law of total variance, as for a client's split routes):
  //   mean     = 0.8 x 1 ms + 0.2 x 8 ms                          = 2.4 ms
  //   variance = Σ s_i v_i + Σ s_i (m_i - mean)²
  //            = (0.8 x 4e-8 + 0.2 x 1.04e-6) + (0.8 x (1.4 ms)² + 0.2 x (5.6 ms)²)
  //            = 2.4e-7 + (1.568e-6 + 6.272e-6) = 8.08e-6
  std::vector<std::pair<std::size_t, std::string>> asked;
  const auto r = cache.response("read", LatencyEstimate{0.001, 4e-8},
                                [&](std::size_t node, std::string_view op) {
                                  asked.emplace_back(node, std::string{op});
                                  return LatencyEstimate{0.005, 1e-6};
                                });
  REQUIRE_THAT(r.mean_s, WithinRel(0.0024, 1e-12));
  REQUIRE_THAT(r.variance_s2, WithinRel(8.08e-6, 1e-9));
  REQUIRE(asked.size() == 1);
  REQUIRE(asked[0] == std::pair{db, std::string{"read"}});
}

// A miss that the backend fails to answer is a request the client never gets back: of the
// requests sent to the cache, the hits (80 %) are answered by the cache alone, the misses
// (20 %) only as often as the backend answers them. Sequential parts multiply, a split is the
// share-weighted mean (the rules above ServiceBehaviour).
TEST_CASE("a read_through cache delivers its hits plus its misses times the backend's fraction",
          "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(cached_front(CacheStrategy::read_through, 0.8, true));
  const CacheBehaviour cache{g, *g.index_of("cache")};
  const double delivered =
      cache.delivered("read", 0.9, [](std::size_t, std::string_view) { return 0.5; });
  REQUIRE_THAT(delivered, WithinRel(0.9 * (0.8 + 0.2 * 0.5), 1e-12));
}

// Write-through (general caching practice, not from ch. 1): every write goes to the cache AND
// the store, and the caller waits for both. Write-around: writes go straight to the store,
// skipping the cache. A cache with a fixed hit ratio does the same thing to the flow in both
// cases (every write reaches the backend, synchronously); they differ in what the cache then
// holds, which a fixed ratio does not model.
TEST_CASE(
    "write_through forwards every write to the backend synchronously; write_around the same flow",
    "[behaviours]") {
  for (const auto strategy : {CacheStrategy::write_through, CacheStrategy::write_around}) {
    CAPTURE(static_cast<int>(strategy));
    const auto g = *simsys::graph::Graph::build(cached_front(strategy, 0.8, true));
    CacheBehaviour cache{g, *g.index_of("cache")};
    const auto db = *g.index_of("db");
    const auto routed = cache.route(flow_of("write", 50), 0s, 100ms);
    REQUIRE(routed.outgoing.size() == 1);
    REQUIRE(routed.outgoing[0].target == db);
    REQUIRE_THAT(routed.outgoing[0].flow.rate_of("write").per_second, WithinRel(50.0, 1e-12));
    REQUIRE_THAT(routed.outgoing[0].flow.total().per_second, WithinRel(50.0, 1e-12));
    REQUIRE_THAT(routed.served.rate_of("write").per_second, WithinRel(50.0, 1e-12));
    REQUIRE(routed.rejected.empty());

    // own 1 ms + the hop (2 legs of 1 ms + the db's 5 ms) = 8 ms; the variances of sequential
    // parts add: 4e-8 + 1e-6. No mixture: every write takes the hop.
    std::vector<std::pair<std::size_t, std::string>> asked;
    const auto r = cache.response("write", LatencyEstimate{0.001, 4e-8},
                                  [&](std::size_t node, std::string_view op) {
                                    asked.emplace_back(node, std::string{op});
                                    return LatencyEstimate{0.005, 1e-6};
                                  });
    REQUIRE_THAT(r.mean_s, WithinRel(0.008, 1e-12));
    REQUIRE_THAT(r.variance_s2, WithinRel(1.04e-6, 1e-9));
    REQUIRE(asked.size() == 1);
    REQUIRE(asked[0] == std::pair{db, std::string{"write"}});
  }
}

// `write_ops` says which ops change data; every other op is a read. A cache whose write op
// is called `set` forwards `set` under write_through, and treats an op called `write` like
// any read (here, under read_through, a read's misses are what get forwarded).
TEST_CASE("the cache's write ops come from write_ops", "[behaviours]") {
  SECTION("a listed op is a write: write_through forwards all of it; an unlisted one is a read") {
    auto s = cached_front(CacheStrategy::write_through, 1.0, true);  // all reads hit
    s.nodes[2].write_ops = {"set"};
    const auto g = *simsys::graph::Graph::build(s);
    CacheBehaviour cache{g, *g.index_of("cache")};
    const auto db = *g.index_of("db");
    Flow in;
    in.add("set", Rate{10});
    in.add("write", Rate{10});
    const auto routed = cache.route(in, 0s, 100ms);
    const auto to_db = sent_to(routed, db);
    REQUIRE_THAT(to_db.rate_of("set").per_second, WithinRel(10.0, 1e-12));
    // "write" is a read now, and every read hits: nothing of it goes to the backend.
    REQUIRE(to_db.rate_of("write").per_second == 0.0);
    REQUIRE_THAT(routed.served.total().per_second, WithinRel(20.0, 1e-12));
  }
  SECTION("an op that is no longer listed is a read, so a read_through cache forwards its misses") {
    auto s = cached_front(CacheStrategy::read_through, 0.8, true);
    s.nodes[2].write_ops = {"set"};
    const auto g = *simsys::graph::Graph::build(s);
    CacheBehaviour cache{g, *g.index_of("cache")};
    const auto routed = cache.route(flow_of("write", 10), 0s, 100ms);
    REQUIRE_THAT(sent_to(routed, *g.index_of("db")).rate_of("write").per_second,
                 WithinRel(2.0, 1e-12));  // 20 % of 10
  }
}

TEST_CASE("a cache's cost per op comes from its spec and defaults to one", "[behaviours]") {
  auto s = cached_front(CacheStrategy::cache_aside, 0.8, false);
  s.nodes[2].cost = {{"read", 2.5}};
  const auto g = *simsys::graph::Graph::build(s);
  const CacheBehaviour cache{g, *g.index_of("cache")};
  REQUIRE(cache.cost("read") == 2.5);
  REQUIRE(cache.cost("write") == 1.0);
}

// Cache-aside from the service's side (ch. 1's pattern, which the book calls read-through):
// web asks the cache, and only for the share the cache missed does it go on to the database. The
// step `{to: db, on: miss}` therefore carries the miss fraction 1 - h of web's requests, while the
// cache step carries all of them.
TEST_CASE("a service forwards a miss step's op at the preceding cache's miss fraction",
          "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(aside_front(0.8));
  const ServiceBehaviour web{g, *g.index_of("web")};
  const auto routed = web.route(flow_of("get", 100), 0s, 100ms);
  REQUIRE(routed.outgoing.size() == 2);
  REQUIRE_THAT(sent_to(routed, *g.index_of("cache")).rate_of("read").per_second,
               WithinRel(100.0, 1e-12));
  REQUIRE_THAT(sent_to(routed, *g.index_of("db")).rate_of("read").per_second,
               WithinRel(20.0, 1e-12));
  REQUIRE_THAT(routed.served.total().per_second, WithinRel(100.0, 1e-12));
}

// The response of a cache-aside read: own + the cache hop (always) + the db hop for the miss
// share only. The db hop is a two-point variable: 0 with probability 0.8, the hop's time with
// probability 0.2, so its mean is 0.2 x hop and its variance, by the law of total variance,
// 0.2 v + 0.2 x 0.8 x hop² (the second term is the spread between hit and miss).
TEST_CASE("a cache_aside read's response is own + cache hop + the miss share of the db hop",
          "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(aside_front(0.8));
  const ServiceBehaviour web{g, *g.index_of("web")};
  const auto cache = *g.index_of("cache");
  std::vector<std::pair<std::size_t, std::string>> asked;
  const auto r =
      web.response("get", LatencyEstimate{0.001, 0.0}, [&](std::size_t node, std::string_view op) {
        asked.emplace_back(node, std::string{op});
        return node == cache ? LatencyEstimate{0.0002, 4e-8} : LatencyEstimate{0.005, 1e-6};
      });
  // cache hop: 2 x 0.5 ms + 0.2 ms = 1.2 ms (variance 4e-8)
  // db hop:    2 x 10 ms + 5 ms    = 25 ms  (variance 1e-6), taken by 20 % of requests:
  //            mean 5 ms, variance 0.2 x 1e-6 + 0.2 x 0.8 x (25 ms)² = 1.002e-4
  // total: mean 1 ms + 1.2 ms + 5 ms = 7.2 ms; variance 4e-8 + 1.002e-4 = 1.0024e-4
  REQUIRE_THAT(r.mean_s, WithinRel(0.0072, 1e-12));
  REQUIRE_THAT(r.variance_s2, WithinRel(1.0024e-4, 1e-9));
  REQUIRE(asked.size() == 2);
  REQUIRE(std::ranges::find(asked, std::pair{cache, std::string{"read"}}) != asked.end());
  REQUIRE(std::ranges::find(asked, std::pair{*g.index_of("db"), std::string{"read"}}) !=
          asked.end());
}

TEST_CASE("a service's delivered fraction counts the db hop only for the share that misses",
          "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(aside_front(0.8));
  const ServiceBehaviour web{g, *g.index_of("web")};
  const auto cache = *g.index_of("cache");
  // cache answers 90 %, db 50 %: 1.0 x 0.9 x (0.8 + 0.2 x 0.5) = 0.81
  const double delivered = web.delivered(
      "get", 1.0, [&](std::size_t node, std::string_view) { return node == cache ? 0.9 : 0.5; });
  REQUIRE_THAT(delivered, WithinRel(0.81, 1e-12));
}

// A miss needs every fetch after the lookup: [cache.read, db.read on miss, db2.read on miss] is
// one block that 20 % of the requests take whole and 80 % skip whole. So the block is timed as
// ONE conditional hop over the sum of its legs (here 20 ms + 30 ms = 50 ms):
//   mean     = 0.2 x 50 ms                      = 10 ms
//   variance = 0.2 x 0.8 x (50 ms)^2            = 4e-4   (the hops' own variances are 0)
// and not as two independent coin flips (0.16 x (20 ms)^2 + 0.16 x (30 ms)^2 = 2.08e-4). A
// miss is answered only if both fetches are, so the delivered fraction is
//   (1 - 0.2) + 0.2 x 0.5 x 0.5 = 0.85            (not (0.8 + 0.2 x 0.5)^2 = 0.81).
TEST_CASE(
    "miss steps after one lookup fire together: a miss pays both hops and fails if either does",
    "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(aside_two_misses(0.8));
  const ServiceBehaviour web{g, *g.index_of("web")};
  const auto cache = *g.index_of("cache");
  const auto db = *g.index_of("db");
  const auto r = web.response("get", LatencyEstimate{}, [&](std::size_t node, std::string_view) {
    if (node == cache) return LatencyEstimate{};
    return node == db ? LatencyEstimate{0.020, 0.0} : LatencyEstimate{0.030, 0.0};
  });
  REQUIRE_THAT(r.mean_s, WithinRel(0.010, 1e-12));
  REQUIRE_THAT(r.variance_s2, WithinRel(0.2 * 0.8 * 0.05 * 0.05, 1e-9));
  const double delivered = web.delivered(
      "get", 1.0, [&](std::size_t node, std::string_view) { return node == cache ? 1.0 : 0.5; });
  REQUIRE_THAT(delivered, WithinRel(0.85, 1e-12));
  // Each fetch still carries the miss share of the flow.
  const auto routed = web.route(flow_of("get", 100), 0s, 100ms);
  REQUIRE_THAT(sent_to(routed, db).rate_of("read").per_second, WithinRel(20.0, 1e-12));
  REQUIRE_THAT(sent_to(routed, *g.index_of("db2")).rate_of("read").per_second,
               WithinRel(20.0, 1e-12));
}

// ---- Cache then-steps and write_back (plan 2b, Task 3) ----

namespace {
// u -> cache -> `target` -> svc. The cache has a fixed hit ratio and the given `then` steps;
// the link from the cache to the target takes 1 ms one way. `target` is a load balancer when
// it is called `lb` and a plain service otherwise. Node indices: u 0, cache 1, target 2.
Scenario then_front(double hit, std::string target, std::vector<Step> then) {
  Scenario s{.name = "then", .version = 1, .duration = 1min};
  NodeSpec cache{.id = "cache",
                 .type = NodeType::cache,
                 .hit_ratio = HitRatio{.fixed = hit},
                 .strategy = CacheStrategy::cache_aside};
  cache.then = std::move(then);
  NodeSpec target_node{.id = target, .type = NodeType::service};
  if (target == "lb") {
    target_node =
        NodeSpec{.id = "lb", .type = NodeType::load_balancer, .policy = LbPolicy::round_robin};
  }
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{100}}, .op_shares = {{"suggest", 1.0}}}},
      std::move(cache), std::move(target_node)};
  s.links = {LinkSpec{.from = "u", .to = "cache", .latency = 0s},
             LinkSpec{.from = "cache", .to = target, .latency = 1ms}};
  if (target == "lb") {
    s.nodes.push_back(NodeSpec{.id = "svc", .type = NodeType::service});
    s.links.push_back(LinkSpec{.from = "lb", .to = "svc", .latency = 0s});
  }
  return s;
}
}  // namespace

// Search autocomplete's browser cache (ch. 13): a hit is answered from the cache; a miss goes on
// to the load balancer. The `then` step on miss carries the miss fraction 1 - h of the reads, and
// the caller pays the hop only on a miss, so the response is a two-point mixture (as for a
// cache-aside service's on-miss step):
//   hit  (0.4): own
//   miss (0.6): own + hop, hop = 2 legs of 1 ms + the lb's response (10 ms) = 12 ms
//   mean     = own + 0.6 x 12 ms
//   variance = 0.4 x 0.6 x (12 ms)²           (own and the lb's variances are 0)
TEST_CASE(
    "a cache's then step on miss forwards the miss fraction of reads and charges the hop to "
    "misses",
    "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(
      then_front(0.4, "lb", {Step{.to = "lb", .op = "suggest", .on = StepCondition::miss}}));
  CacheBehaviour cache{g, *g.index_of("cache")};
  const auto lb = *g.index_of("lb");
  const auto routed = cache.route(flow_of("suggest", 100), 0s, 100ms);
  REQUIRE(routed.outgoing.size() == 1);
  REQUIRE(routed.outgoing[0].target == lb);
  REQUIRE_THAT(routed.outgoing[0].flow.rate_of("suggest").per_second, WithinRel(60.0, 1e-12));
  REQUIRE_THAT(routed.outgoing[0].flow.total().per_second, WithinRel(60.0, 1e-12));
  REQUIRE_THAT(routed.served.rate_of("suggest").per_second, WithinRel(100.0, 1e-12));
  REQUIRE(routed.rejected.empty());

  const double own = 0.001;
  std::vector<std::pair<std::size_t, std::string>> asked;
  const auto r = cache.response("suggest", LatencyEstimate{own, 0.0},
                                [&](std::size_t node, std::string_view op) {
                                  asked.emplace_back(node, std::string{op});
                                  return LatencyEstimate{0.010, 0.0};
                                });
  REQUIRE_THAT(r.mean_s, WithinRel(own + 0.6 * 0.012, 1e-12));
  REQUIRE_THAT(r.variance_s2, WithinRel(0.4 * 0.6 * 0.012 * 0.012, 1e-9));
  REQUIRE(asked.size() == 1);
  REQUIRE(asked[0] == std::pair{lb, std::string{"suggest"}});

  // A hit is answered by the cache alone; a miss only as often as the lb answers it.
  const double delivered =
      cache.delivered("suggest", 1.0, [](std::size_t, std::string_view) { return 0.5; });
  REQUIRE_THAT(delivered, WithinRel(0.4 + 0.6 * 0.5, 1e-12));
}

// `on: always` is the default condition: every request the cache handles, hit or miss, triggers
// the step (an audit log, say). The flow is the whole input and the hop is paid by every request.
TEST_CASE("a cache's then step on always forwards everything", "[behaviours]") {
  const auto g =
      *simsys::graph::Graph::build(then_front(0.4, "audit", {Step{.to = "audit", .op = "log"}}));
  CacheBehaviour cache{g, *g.index_of("cache")};
  const auto audit = *g.index_of("audit");
  const auto routed = cache.route(flow_of("suggest", 100), 0s, 100ms);
  REQUIRE(routed.outgoing.size() == 1);
  REQUIRE(routed.outgoing[0].target == audit);
  REQUIRE_THAT(routed.outgoing[0].flow.rate_of("log").per_second, WithinRel(100.0, 1e-12));
  REQUIRE_THAT(routed.outgoing[0].flow.total().per_second, WithinRel(100.0, 1e-12));

  // own 1 ms + the hop (2 legs of 1 ms + the audit's 5 ms): sequential, so the means and the
  // variances add; no mixture because every request takes the hop.
  const auto r = cache.response("suggest", LatencyEstimate{0.001, 4e-8},
                                [&](std::size_t node, std::string_view op) {
                                  REQUIRE(node == audit);
                                  REQUIRE(op == "log");
                                  return LatencyEstimate{0.005, 1e-6};
                                });
  REQUIRE_THAT(r.mean_s, WithinRel(0.008, 1e-12));
  REQUIRE_THAT(r.variance_s2, WithinRel(1.04e-6, 1e-9));
  const double delivered =
      cache.delivered("suggest", 1.0, [](std::size_t, std::string_view) { return 0.5; });
  REQUIRE_THAT(delivered, WithinRel(0.5, 1e-12));
}

// Write-back (general caching practice): the cache acknowledges a write itself and flushes it to
// the store later. The flush is a hop nobody waits for: the backend still sees every write (the
// forwarded flow), but the caller's latency and delivered fraction are the cache's alone.
TEST_CASE("a write_back cache serves writes itself and flushes them asynchronously",
          "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(cached_front(CacheStrategy::write_back, 0.8, true));
  CacheBehaviour cache{g, *g.index_of("cache")};
  const auto db = *g.index_of("db");
  const auto routed = cache.route(flow_of("write", 50), 0s, 100ms);
  REQUIRE_THAT(routed.served.rate_of("write").per_second, WithinRel(50.0, 1e-12));
  REQUIRE(routed.rejected.empty());
  const auto to_db = sent_to(routed, db);
  REQUIRE_THAT(to_db.rate_of("write").per_second, WithinRel(50.0, 1e-12));
  REQUIRE_THAT(to_db.total().per_second, WithinRel(50.0, 1e-12));

  const LatencyEstimate own{0.001, 4e-8};
  const auto r = cache.response(
      "write", own, [](std::size_t, std::string_view) { return LatencyEstimate{1.0, 0.0}; });
  REQUIRE(r == own);
  const double delivered =
      cache.delivered("write", 1.0, [](std::size_t, std::string_view) { return 0.0; });
  REQUIRE_THAT(delivered, WithinRel(1.0, 1e-12));
}

// Both kinds of then step on one cache: the miss step's `factor` multiplies the read misses
// (100 reads, h = 0.75: 25 misses x 2 = 50 queries), and an always step fires for every
// request the cache handles, writes included (writes do not miss, so the miss step sends none).
TEST_CASE(
    "a cache's miss then step multiplies its factor into the read misses and an always step "
    "fires for writes too",
    "[behaviours]") {
  auto s = cached_front(CacheStrategy::cache_aside, 0.75, false);  // ops read, set, write
  s.nodes[2].then = {Step{.to = "svc", .op = "q", .on = StepCondition::miss, .factor = 2.0},
                     Step{.to = "audit", .op = "log"}};
  s.nodes.push_back(NodeSpec{.id = "svc", .type = NodeType::service});
  s.nodes.push_back(NodeSpec{.id = "audit", .type = NodeType::service});
  s.links.push_back(LinkSpec{.from = "cache", .to = "svc", .latency = 0s});
  s.links.push_back(LinkSpec{.from = "cache", .to = "audit", .latency = 0s});
  const auto g = *simsys::graph::Graph::build(s);
  const CacheBehaviour cache{g, *g.index_of("cache")};
  const auto svc = *g.index_of("svc");
  const auto audit = *g.index_of("audit");
  const auto reads = cache.route(flow_of("read", 100), 0s, 100ms);
  REQUIRE_THAT(sent_to(reads, svc).rate_of("q").per_second, WithinRel(50.0, 1e-12));
  REQUIRE_THAT(sent_to(reads, audit).rate_of("log").per_second, WithinRel(100.0, 1e-12));
  const auto writes = cache.route(flow_of("write", 40), 0s, 100ms);
  REQUIRE(sent_to(writes, svc).total().per_second == 0.0);
  REQUIRE_THAT(sent_to(writes, audit).rate_of("log").per_second, WithinRel(40.0, 1e-12));
}

// Write-back with an audit step: the flush to the store is asynchronous (nobody waits for it),
// the audit step is an ordinary call (the cache answers once the audit has). So a write's
// response is own 1 ms + the audit's 5 ms (zero-latency link), whatever the store takes, and it
// is answered as often as the audit answers it, whatever the store does with the flush.
TEST_CASE("a write_back cache waits for its always then step but not for its flush",
          "[behaviours]") {
  auto s = cached_front(CacheStrategy::write_back, 0.8, true);
  s.nodes[2].then = {Step{.to = "audit", .op = "log"}};
  s.nodes.push_back(NodeSpec{.id = "audit", .type = NodeType::service});
  s.links.push_back(LinkSpec{.from = "cache", .to = "audit", .latency = 0s});
  const auto g = *simsys::graph::Graph::build(s);
  const CacheBehaviour cache{g, *g.index_of("cache")};
  const auto db = *g.index_of("db");
  const auto audit = *g.index_of("audit");
  const auto r =
      cache.response("write", LatencyEstimate{0.001, 0.0}, [&](std::size_t node, std::string_view) {
        return LatencyEstimate{node == audit ? 0.005 : 1.0, 0.0};
      });
  REQUIRE_THAT(r.mean_s, WithinRel(0.006, 1e-12));  // own + audit, no db
  const double delivered = cache.delivered(
      "write", 1.0, [&](std::size_t node, std::string_view) { return node == db ? 0.0 : 0.5; });
  REQUIRE_THAT(delivered, WithinRel(0.5, 1e-12));
  const auto routed = cache.route(flow_of("write", 50), 0s, 100ms);
  REQUIRE_THAT(sent_to(routed, db).rate_of("write").per_second, WithinRel(50.0, 1e-12));
  REQUIRE_THAT(sent_to(routed, audit).rate_of("log").per_second, WithinRel(50.0, 1e-12));
}

// A pin across the HopBlocks refactor: one op with every kind of block a service has.
// api.go = [a parallel, b parallel, c.read (a cache, h = 0.75), d on miss, e on miss], zero-latency
// links, own 1 ms; a 2 ms, b 3 ms, c 0.25 ms, d 4 ms, e 5 ms. The sequential parts add, the two
// parallel calls cost their slowest, and the miss block (d then e, 9 ms) is taken by 25 %:
//   1 + 0.25 + 0.25 x (4 + 5) + max(2, 3) = 6.5 ms.
TEST_CASE("a service's parallel, sequential and miss blocks compose to 6.5 ms", "[behaviours]") {
  Scenario s{.name = "mix", .version = 1, .duration = 1min};
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{100}}, .op_shares = {{"go", 1.0}}}},
      NodeSpec{.id = "api",
               .type = NodeType::service,
               .ops = {{"go",
                        {Step{.to = "a", .op = "x", .mode = StepMode::parallel},
                         Step{.to = "b", .op = "x", .mode = StepMode::parallel},
                         Step{.to = "c", .op = "read"},
                         Step{.to = "d", .op = "x", .on = StepCondition::miss},
                         Step{.to = "e", .op = "x", .on = StepCondition::miss}}}}},
      NodeSpec{.id = "a", .type = NodeType::service},
      NodeSpec{.id = "b", .type = NodeType::service},
      NodeSpec{.id = "c", .type = NodeType::cache, .hit_ratio = HitRatio{.fixed = 0.75}},
      NodeSpec{.id = "d", .type = NodeType::service},
      NodeSpec{.id = "e", .type = NodeType::service}};
  s.links = {LinkSpec{.from = "u", .to = "api", .latency = 0s}};
  for (const auto* target : {"a", "b", "c", "d", "e"}) {
    s.links.push_back(LinkSpec{.from = "api", .to = target, .latency = 0s});
  }
  const auto g = *simsys::graph::Graph::build(s);
  const ServiceBehaviour api{g, *g.index_of("api")};
  const std::map<std::string, double, std::less<>> response_s{
      {"a", 0.002}, {"b", 0.003}, {"c", 0.00025}, {"d", 0.004}, {"e", 0.005}};
  const auto r =
      api.response("go", LatencyEstimate{0.001, 0.0}, [&](std::size_t node, std::string_view) {
        return LatencyEstimate{response_s.at(g.nodes()[node].spec.id), 0.0};
      });
  REQUIRE_THAT(r.mean_s, WithinRel(0.0065, 1e-12));
}

// ---- SqlDbBehaviour (plan 2b, Task 4) ----

namespace {
// u -> db, zero-latency link; the client sends reads (90 %) and writes (10 %) to the database
// op for op. Node indices: u 0, db 1. The engine, not the behaviour, splits the load between
// primary and replicas (it holds the two queues), so the behaviour sees one flow.
Scenario sql_front(int replicas = 3) {
  Scenario s{.name = "sql", .version = 1, .duration = 1min};
  NodeSpec db{.id = "db", .type = NodeType::sql_db};
  db.replicas = replicas;
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{100}},
                                           .op_shares = {{"read", 0.9}, {"write", 0.1}}}},
             std::move(db)};
  s.links = {LinkSpec{.from = "u", .to = "db", .latency = 0s}};
  return s;
}
}  // namespace

// C++20 concepts checked at compile time, as for the other behaviours above.
static_assert(simsys::behaviours::NodeBehaviour<SqlDbBehaviour>);

// A database is a leaf (book ch. 1): it answers every query itself and calls nobody. How the
// primary and the replicas share the work is the engine's business (two queues), not a
// forwarding decision.
TEST_CASE("a sql database serves reads and writes and forwards nothing", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(sql_front());
  SqlDbBehaviour db{g, *g.index_of("db")};
  Flow in;
  in.add("read", Rate{100});
  in.add("write", Rate{10});
  const auto routed = db.route(in, 0s, 100ms);
  REQUIRE(routed.outgoing.empty());
  REQUIRE(routed.rejected.empty());
  REQUIRE_THAT(routed.served.rate_of("read").per_second, WithinRel(100.0, 1e-12));
  REQUIRE_THAT(routed.served.rate_of("write").per_second, WithinRel(10.0, 1e-12));
  REQUIRE_THAT(routed.served.total().per_second, WithinRel(110.0, 1e-12));
}

// `write_ops` (default write, insert, update, delete) says which ops go to the primary; every
// other op is a read the replicas can answer.
TEST_CASE("reads and writes are classified by write_ops", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(sql_front());
  const SqlDbBehaviour db{g, *g.index_of("db")};
  REQUIRE(db.is_write("insert"));
  REQUIRE(db.is_write("write"));
  REQUIRE_FALSE(db.is_write("select"));
  REQUIRE_FALSE(db.is_write("read"));
  SECTION("a spec that lists only insert makes write a read") {
    auto s = sql_front();
    s.nodes[1].write_ops = {"insert"};
    const auto custom = *simsys::graph::Graph::build(s);
    const SqlDbBehaviour narrow{custom, *custom.index_of("db")};
    REQUIRE(narrow.is_write("insert"));
    REQUIRE_FALSE(narrow.is_write("write"));
  }
}

// A write costs a database 5 capacity units unless `cost` says otherwise: the spec's §6 table
// gives one SQL machine "5k reads" or "1k writes" per second, 5000 / 1000 = 5 (a write's disk
// seek and log flush). A read costs 1, and a stated cost wins over the default.
TEST_CASE("a sql database's writes cost five units by default", "[behaviours]") {
  auto s = sql_front();
  s.nodes[1].cost = {{"update", 2.0}};
  const auto g = *simsys::graph::Graph::build(s);
  const SqlDbBehaviour db{g, *g.index_of("db")};
  REQUIRE(db.cost("write") == 5.0);
  REQUIRE(db.cost("insert") == 5.0);
  REQUIRE(db.cost("delete") == 5.0);
  REQUIRE(db.cost("update") == 2.0);
  REQUIRE(db.cost("select") == 1.0);
}

// `write_ops` stated on a database replaces the database default; it does not add to it. A
// scenario that says only `write` is a write means exactly that: an `insert` is then a read.
TEST_CASE("an explicit write_ops of just write on a sql database means exactly that",
          "[behaviours]") {
  auto s = sql_front();
  s.nodes[1].write_ops = std::vector<std::string>{"write"};
  const auto g = *simsys::graph::Graph::build(s);
  const SqlDbBehaviour db{g, *g.index_of("db")};
  REQUIRE(db.is_write("write"));
  REQUIRE_FALSE(db.is_write("insert"));
  REQUIRE_FALSE(db.is_write("update"));
  REQUIRE_FALSE(db.is_write("delete"));
}

// The engine holds the primary's and the replicas' queues and hands the behaviour the sojourn
// of the one an op uses (writes: primary; reads: a replica copy); the database composes
// nothing else, so its response is that `own`, and it asks nobody downstream.
TEST_CASE("a sql database's response is the sojourn the engine chose, with no downstream steps",
          "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(sql_front());
  const SqlDbBehaviour db{g, *g.index_of("db")};
  const LatencyEstimate own{0.0042, 1.5e-6};
  bool asked = false;
  const auto lookup = [&](std::size_t, std::string_view) {
    asked = true;
    return LatencyEstimate{99.0, 99.0};
  };
  REQUIRE(db.response("read", own, lookup) == own);
  REQUIRE(db.response("write", own, lookup) == own);
  REQUIRE_FALSE(asked);
  REQUIRE_THAT(db.delivered("read", 0.9, [](std::size_t, std::string_view) { return 0.1; }),
               WithinRel(0.9, 1e-12));
}

// ---- CdnBehaviour (plan 2b refinement 4, Task 5) ----
//
// A CDN (book ch. 1: a cache at the network's edge) answers the share h of the reads it is sent
// from its own store and fetches the rest, 1 - h, from its origin as the SAME op: the shape of a
// read_through cache, without a write path (the graph refuses a write that reaches a CDN).

namespace {
// u -> cdn -> origin. Node indices: u 0, cdn 1, origin 2. The cdn has the given fixed hit ratio
// and the origin is a plain service 1 ms away one way; u -> cdn has no latency.
Scenario cdn_in_front(double hit) {
  Scenario s{.name = "cdn", .version = 1, .duration = 1min};
  NodeSpec cdn{.id = "cdn", .type = NodeType::cdn, .hit_ratio = HitRatio{.fixed = hit}};
  cdn.origin = "origin";
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{100}}, .op_shares = {{"read", 1.0}}}},
      std::move(cdn), NodeSpec{.id = "origin", .type = NodeType::service}};
  s.links = {LinkSpec{.from = "u", .to = "cdn", .latency = 0s},
             LinkSpec{.from = "cdn", .to = "origin", .latency = 1ms}};
  return s;
}
}  // namespace

// With hit ratio 0.95 the fluid model sends the miss fraction 0.05 of the reads on, unchanged,
// and the CDN itself answers every request (`served` is all of the input, hits and misses
// alike, as for a read_through cache): 100 reads/s in, 5 reads/s to the origin.
TEST_CASE("a cdn forwards its miss fraction of a read to the origin unchanged", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(cdn_in_front(0.95));
  CdnBehaviour cdn{g, *g.index_of("cdn")};
  const auto origin = *g.index_of("origin");
  const auto routed = cdn.route(flow_of("read", 100), 0s, 100ms);
  REQUIRE(routed.outgoing.size() == 1);
  REQUIRE(routed.outgoing[0].target == origin);
  REQUIRE_THAT(routed.outgoing[0].flow.rate_of("read").per_second, WithinRel(5.0, 1e-12));
  REQUIRE_THAT(routed.outgoing[0].flow.total().per_second, WithinRel(5.0, 1e-12));  // only `read`
  REQUIRE_THAT(routed.served.rate_of("read").per_second, WithinRel(100.0, 1e-12));
  REQUIRE(routed.rejected.empty());
}

TEST_CASE("a cdn that always hits sends nothing to its origin", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(cdn_in_front(1.0));
  CdnBehaviour cdn{g, *g.index_of("cdn")};
  const auto routed = cdn.route(flow_of("read", 100), 0s, 100ms);
  REQUIRE_THAT(sent_to(routed, *g.index_of("origin")).total().per_second, WithinAbs(0.0, 1e-12));
  REQUIRE_THAT(routed.served.rate_of("read").per_second, WithinRel(100.0, 1e-12));
}

// Same mixture as the read_through cache's response, with h = 0.95. Hit (95 %): own sojourn only,
// mean 1 ms, variance 4e-8. Miss (5 %): own + the hop to the origin, 2 legs of 1 ms + the
// origin's response (5 ms, variance 1e-6): mean 8 ms, variance 4e-8 + 1e-6 = 1.04e-6.
// A mixture (law of total variance, as for a client's split routes):
//   mean     = 0.95 x 1 ms + 0.05 x 8 ms = 1.35 ms   (= own + 0.05 x the 7 ms hop)
//   variance = Σ s_i v_i + Σ s_i (m_i - mean)²
//            = (0.95 x 4e-8 + 0.05 x 1.04e-6) + (0.95 x (0.35 ms)² + 0.05 x (6.65 ms)²)
//            = 9.0e-8 + (1.16375e-7 + 2.211125e-6) = 2.4175e-6
TEST_CASE("a cdn's response is its own sojourn plus the miss share of the origin hop",
          "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(cdn_in_front(0.95));
  CdnBehaviour cdn{g, *g.index_of("cdn")};
  const auto origin = *g.index_of("origin");
  std::vector<std::pair<std::size_t, std::string>> asked;
  const auto r = cdn.response("read", LatencyEstimate{0.001, 4e-8},
                              [&](std::size_t node, std::string_view op) {
                                asked.emplace_back(node, std::string{op});
                                return LatencyEstimate{0.005, 1e-6};
                              });
  REQUIRE_THAT(r.mean_s, WithinRel(0.00135, 1e-12));
  REQUIRE_THAT(r.variance_s2, WithinRel(2.4175e-6, 1e-9));
  REQUIRE(asked.size() == 1);
  REQUIRE(asked[0] == std::pair{origin, std::string{"read"}});  // the same op, not a renamed one
}

// Of the requests sent to the cdn, the hits (95 %) are answered by the cdn alone; the misses
// (5 %) only as often as the origin answers them. Sequential parts multiply, a split is the
// share-weighted mean: arriving fraction 0.9 x (0.95 + 0.05 x 0.5) = 0.9 x 0.975 = 0.8775.
TEST_CASE("a cdn delivers its hits plus its misses times the origin's delivered fraction",
          "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(cdn_in_front(0.95));
  const CdnBehaviour cdn{g, *g.index_of("cdn")};
  const double delivered =
      cdn.delivered("read", 0.9, [](std::size_t, std::string_view) { return 0.5; });
  REQUIRE_THAT(delivered, WithinRel(0.8775, 1e-12));
}

// ---- Byte-sized nodes count megabytes (plan 2c refinement 2, Task 3) ----
//
// A node whose capacity is a byte rate (a blob store's default, spec section 6's 500 MB/s, or a
// CDN's stated `capacity: 100GB/s`) counts one MB as one unit of capacity, so a request of op o
// costs request_bytes(o) / 1e6 units: a 50 MB upload costs 50, a 2 KB read 2e3 / 1e6 = 0.002.
// The sizes are the clients' (`sizes:`), resolved once by the graph (Graph::request_bytes).

namespace {
// u (sizes: upload 50 MB, read 2 KB) -> store (a blob store with its type's default capacity,
// byte-sized). Node indices: u 0, store 1; zero-latency link.
Scenario sized_blob_front() {
  Scenario s{.name = "blob", .version = 1, .duration = 1min};
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{100}},
                                           .op_shares = {{"upload", 0.5}, {"read", 0.5}},
                                           .sizes = {{"upload", simsys::Bytes{50e6}},
                                                     {"read", simsys::Bytes{2e3}}}}},
             NodeSpec{.id = "store", .type = NodeType::blob_store}};
  s.links = {LinkSpec{.from = "u", .to = "store", .latency = 0s}};
  return s;
}

// u (sizes: stream 50 MB) -> cdn (fixed hit ratio 0.95, origin `origin`) -> origin (a service,
// which counts requests). The cdn is byte-sized (100 GB/s) when `bytes` is given, else it counts
// requests (the default 100k/s). Node indices: u 0, cdn 1, origin 2.
Scenario stream_cdn_front(std::optional<simsys::ByteRate> bytes) {
  Scenario s{.name = "stream-cdn", .version = 1, .duration = 1min};
  NodeSpec cdn{.id = "cdn",
               .type = NodeType::cdn,
               .byte_capacity = bytes,
               .hit_ratio = HitRatio{.fixed = 0.95}};
  cdn.origin = "origin";
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{100}},
                                           .op_shares = {{"stream", 1.0}},
                                           .sizes = {{"stream", simsys::Bytes{50e6}}}}},
             std::move(cdn), NodeSpec{.id = "origin", .type = NodeType::service}};
  s.links = {LinkSpec{.from = "u", .to = "cdn", .latency = 0s},
             LinkSpec{.from = "cdn", .to = "origin", .latency = 1ms}};
  return s;
}
}  // namespace

TEST_CASE("a byte-sized blob store charges a request its size in megabytes", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(sized_blob_front());
  const BlobStoreBehaviour store{g, *g.index_of("store")};
  REQUIRE_THAT(store.cost("upload"), WithinRel(50.0, 1e-12));  // 50e6 B / 1e6 B per unit
  REQUIRE_THAT(store.cost("read"), WithinRel(0.002, 1e-12));   // 2e3 B / 1e6 B per unit
}

// A blob store without `then` steps is a leaf (book ch. 14's S3-like store): it answers every
// request itself and calls nobody, as a database does.
TEST_CASE("a blob store serves every op and forwards nothing", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(sized_blob_front());
  const BlobStoreBehaviour store{g, *g.index_of("store")};
  Flow in;
  in.add("upload", Rate{5});
  in.add("read", Rate{1000});
  const auto routed = store.route(in, 0s, 100ms);
  REQUIRE(routed.outgoing.empty());
  REQUIRE(routed.rejected.empty());
  REQUIRE_THAT(routed.served.rate_of("upload").per_second, WithinRel(5.0, 1e-12));
  REQUIRE_THAT(routed.served.rate_of("read").per_second, WithinRel(1000.0, 1e-12));
}

// The response of a leaf is its own sojourn, nobody downstream is asked, and the delivered
// fraction is the node's own (the same contract as SqlDbBehaviour above).
TEST_CASE("a blob store's response is its own sojourn and asks nobody downstream", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(sized_blob_front());
  const BlobStoreBehaviour store{g, *g.index_of("store")};
  const LatencyEstimate own{0.040015, 2e-7};
  bool asked = false;
  const auto lookup = [&](std::size_t, std::string_view) {
    asked = true;
    return LatencyEstimate{99.0, 99.0};
  };
  REQUIRE(store.response("upload", own, lookup) == own);
  REQUIRE_FALSE(asked);
  REQUIRE_THAT(store.delivered("upload", 0.9, [](std::size_t, std::string_view) { return 0.1; }),
               WithinRel(0.9, 1e-12));
}

TEST_CASE("a byte-sized cdn charges a request its size in megabytes", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(stream_cdn_front(simsys::ByteRate{100e9}));
  const CdnBehaviour cdn{g, *g.index_of("cdn")};
  REQUIRE_THAT(cdn.cost("stream"), WithinRel(50.0, 1e-12));  // 50e6 B / 1e6 B per unit
}

TEST_CASE("a cdn that counts requests charges one unit per request whatever the size",
          "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(stream_cdn_front(std::nullopt));
  const CdnBehaviour cdn{g, *g.index_of("cdn")};
  REQUIRE(cdn.cost("stream") == 1.0);
}

// Charging megabytes changes what an op costs, not what it forwards: the origin is asked for the
// miss share (5 %) of the REQUESTS, not of the megabytes.
TEST_CASE("a byte-sized cdn still forwards its miss share of the requests", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(stream_cdn_front(simsys::ByteRate{100e9}));
  CdnBehaviour cdn{g, *g.index_of("cdn")};
  const auto routed = cdn.route(flow_of("stream", 100), 0s, 100ms);
  REQUIRE(routed.outgoing.size() == 1);
  REQUIRE_THAT(routed.outgoing[0].flow.rate_of("stream").per_second, WithinRel(5.0, 1e-12));
}
