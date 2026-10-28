// What client and service nodes do with flow and how they compose response latency.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <map>
#include <span>
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

using namespace std::chrono_literals;
using Catch::Approx;
using simsys::Rate;
using simsys::behaviours::ClientBehaviour;
using simsys::behaviours::Outgoing;
using simsys::behaviours::ServiceBehaviour;
using simsys::flow::Flow;
using simsys::flow::LatencyEstimate;
using simsys::model::LinkSpec;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
using simsys::model::Step;
using simsys::model::StepMode;
using simsys::model::Workload;

namespace {
// users -> web -> {cache, db}; web.get = [cache.read, db.select x2]; put is terminal at web.
Scenario two_hop(StepMode mode) {
  Scenario s{.name = "t", .version = 1, .duration = 1min};
  s.nodes = {NodeSpec{.id = "users", .type = NodeType::client,
                      .workload = Workload{.base = Rate{100}, .op_shares = {{"get", 0.8}, {"put", 0.2}}}},
             NodeSpec{.id = "web", .type = NodeType::service,
                      .ops = {{"get", {Step{.to = "cache", .op = "read", .mode = mode},
                                       Step{.to = "db", .op = "select", .mode = mode, .factor = 2}}}}},
             NodeSpec{.id = "cache", .type = NodeType::service},
             NodeSpec{.id = "db", .type = NodeType::service}};
  s.links = {LinkSpec{.from = "users", .to = "web", .latency = 1ms},
             LinkSpec{.from = "web", .to = "cache", .latency = 0s},
             LinkSpec{.from = "web", .to = "db", .latency = 10ms}};
  return s;
}
}  // namespace

// C++20 concepts checked at compile time: the behaviours must satisfy the engine's
// requirements, or this file does not compile.
static_assert(simsys::behaviours::NodeBehaviour<ServiceBehaviour>);
static_assert(simsys::behaviours::RequestSource<ClientBehaviour>);

TEST_CASE("a client offers base rate times op share along its routes", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(two_hop(StepMode::sequential));
  const ClientBehaviour c{g, *g.index_of("users")};
  const auto routed = c.offer(0s);
  REQUIRE(routed.outgoing.size() == 1);
  REQUIRE(routed.outgoing[0].target == *g.index_of("web"));
  REQUIRE(routed.outgoing[0].flow.rate_of("get").per_second == Approx(80));
  REQUIRE(routed.outgoing[0].flow.rate_of("put").per_second == Approx(20));
}

TEST_CASE("a service forwards each step's op scaled by its factor and serves every incoming op", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(two_hop(StepMode::sequential));
  const ServiceBehaviour web{g, *g.index_of("web")};
  Flow in;
  in.add("get", Rate{50});
  in.add("put", Rate{5});  // no steps: terminal op, served locally
  const auto routed = web.route(in);
  REQUIRE(routed.outgoing.size() == 2);
  const auto to_db = std::ranges::find(routed.outgoing, *g.index_of("db"), &Outgoing::target);
  REQUIRE(to_db != routed.outgoing.end());
  REQUIRE(to_db->flow.rate_of("select").per_second == Approx(100));  // factor 2
  const auto to_cache = std::ranges::find(routed.outgoing, *g.index_of("cache"), &Outgoing::target);
  REQUIRE(to_cache->flow.rate_of("read").per_second == Approx(50));
  REQUIRE(routed.served.total().per_second == Approx(55));
}

TEST_CASE("a service with no incoming flow forwards nothing", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(two_hop(StepMode::sequential));
  const ServiceBehaviour web{g, *g.index_of("web")};
  const auto routed = web.route(Flow{});
  REQUIRE(routed.outgoing.empty());
  REQUIRE(routed.served.empty());
}

TEST_CASE("sequential steps add hop latencies: two link legs plus the downstream response", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(two_hop(StepMode::sequential));
  const ServiceBehaviour web{g, *g.index_of("web")};
  const LatencyEstimate own{0.001, 0.0};
  const auto downstream = [&](std::size_t node, std::string_view) {
    return node == *g.index_of("db") ? LatencyEstimate{0.005, 1e-6} : LatencyEstimate{0.0002, 0.0};
  };
  const auto r = web.response("get", own, downstream);
  // own 1 ms + cache (0 + 0.2 ms + 0) + db (10 ms + 5 ms + 10 ms) = 26.2 ms
  REQUIRE(r.mean_s == Approx(0.0262));
  REQUIRE(r.variance_s2 == Approx(1e-6));
}

TEST_CASE("parallel steps add the slowest branch instead of the sum", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(two_hop(StepMode::parallel));
  const ServiceBehaviour web{g, *g.index_of("web")};
  const auto downstream = [&](std::size_t node, std::string_view) {
    return node == *g.index_of("db") ? LatencyEstimate{0.005, 1e-6} : LatencyEstimate{0.0002, 4e-6};
  };
  const auto r = web.response("get", LatencyEstimate{0.001, 0.0}, downstream);
  REQUIRE(r.mean_s == Approx(0.001 + 0.025));
  REQUIRE(r.variance_s2 == Approx(4e-6));  // largest variance among branches
}

TEST_CASE("an op without steps responds with the node's own sojourn", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(two_hop(StepMode::sequential));
  const ServiceBehaviour web{g, *g.index_of("web")};
  const auto r = web.response("put", LatencyEstimate{0.003, 2e-6}, [](std::size_t, std::string_view) {
    return LatencyEstimate{99.0, 99.0};
  });
  REQUIRE(r == LatencyEstimate{0.003, 2e-6});
}

TEST_CASE("a client's response adds its link's two legs to the first hop's response", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(two_hop(StepMode::sequential));
  const ClientBehaviour c{g, *g.index_of("users")};
  const auto r = c.response("get", [](std::size_t, std::string_view) { return LatencyEstimate{0.004, 1e-6}; });
  REQUIRE(r.mean_s == Approx(0.001 + 0.004 + 0.001));
  REQUIRE(r.variance_s2 == Approx(1e-6));
}

TEST_CASE("a client op split across two links mixes the responses by share", "[behaviours]") {
  Scenario s{.name = "split", .version = 1, .duration = 1min};
  s.nodes = {NodeSpec{.id = "u", .type = NodeType::client,
                      .workload = Workload{.base = Rate{100}, .op_shares = {{"id", 1.0}}}},
             NodeSpec{.id = "fast", .type = NodeType::service}, NodeSpec{.id = "slow", .type = NodeType::service}};
  s.links = {LinkSpec{.from = "u", .to = "fast", .latency = 0s, .weight = 3},
             LinkSpec{.from = "u", .to = "slow", .latency = 0s, .weight = 1}};
  const auto g = *simsys::graph::Graph::build(s);
  const ClientBehaviour c{g, *g.index_of("u")};
  const auto r = c.response("id", [&](std::size_t node, std::string_view) {
    return node == *g.index_of("fast") ? LatencyEstimate{0.001, 0.0} : LatencyEstimate{0.005, 0.0};
  });
  // Mixture: mean = 0.75 * 1 ms + 0.25 * 5 ms = 2 ms;
  // variance = E[X^2] - mean^2 = (0.75 * 1e-6 + 0.25 * 25e-6) - 4e-6 = 3e-6 (law of total variance).
  REQUIRE(r.mean_s == Approx(0.002));
  REQUIRE(r.variance_s2 == Approx(3e-6));
}

TEST_CASE("client ops are listed in sorted order for deterministic reports", "[behaviours]") {
  const auto g = *simsys::graph::Graph::build(two_hop(StepMode::sequential));
  const ClientBehaviour c{g, *g.index_of("users")};
  const auto ops = c.ops();
  REQUIRE(ops.size() == 2);
  REQUIRE(ops[0] == "get");
  REQUIRE(ops[1] == "put");
}
