// The resolved graph: who sends demand to whom, in what order, through which link.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.caching;
import simsys.core;
import simsys.defaults;
import simsys.graph;
import simsys.model;
import simsys.report;
import simsys.test_fixtures;

using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using simsys::Rate;
using simsys::graph::DemandEdge;
using simsys::graph::Graph;
using simsys::graph::ServiceHop;
using simsys::model::CacheStrategy;
using simsys::model::Direction;
using simsys::model::HitRatio;
using simsys::model::KeySpec;
using simsys::model::LbPolicy;
using simsys::model::LinkSpec;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
using simsys::model::Step;
using simsys::model::StepCondition;
using simsys::model::StepMode;
using simsys::model::Workload;

namespace {
// C++20 designated initializers: `NodeSpec{.id = ..., .type = ...}` names each member it
// sets and leaves the rest at their defaults. Replaces positional aggregate initialization,
// where a reader must count commas against the struct definition; names must follow
// declaration order, so a reordered struct fails to compile instead of silently swapping values.
NodeSpec client(std::string id, Rate base, std::vector<std::pair<std::string, double>> ops) {
  return NodeSpec{.id = std::move(id),
                  .type = NodeType::client,
                  .workload = Workload{.curve = {.base = base}, .op_shares = std::move(ops)}};
}
NodeSpec service(std::string id, std::map<std::string, std::vector<Step>> ops = {}) {
  return NodeSpec{.id = std::move(id), .type = NodeType::service, .ops = std::move(ops)};
}
NodeSpec load_balancer(std::string id, LbPolicy policy) {
  return NodeSpec{.id = std::move(id), .type = NodeType::load_balancer, .policy = policy};
}
Scenario chain() {  // users -> api -> db
  Scenario s{.name = "chain", .version = 1, .duration = 1min};
  s.nodes = {client("users", Rate{100}, {{"get", 1.0}}),
             service("api", {{"get", {Step{.to = "db", .op = "select"}}}}), service("db")};
  s.links = {LinkSpec{.from = "users", .to = "api", .dir = Direction::inout},
             LinkSpec{.from = "api", .to = "db", .dir = Direction::inout}};
  return s;
}
}  // namespace

TEST_CASE("nodes keep scenario order and get resolved capacities with provenance", "[graph]") {
  const auto g = Graph::build(chain());
  REQUIRE(g.has_value());
  REQUIRE(g->nodes().size() == 3);
  REQUIRE(g->nodes()[1].spec.id == "api");
  REQUIRE(g->nodes()[1].capacity.service_rate == Rate{2000});  // service default
  REQUIRE(g->nodes()[1].service_rate_source == simsys::defaults::Source::type_default);
  REQUIRE(g->index_of("db") == 2);
  REQUIRE_FALSE(g->index_of("nope").has_value());
}

TEST_CASE("topological order puts every node after the nodes that send to it", "[graph]") {
  Scenario s = chain();
  std::swap(s.nodes[0], s.nodes[2]);  // db, api, users in the file
  const auto g = Graph::build(s);
  REQUIRE(g.has_value());
  // C++20 std::span<const std::size_t>: topological_order() hands out a non-owning view
  // of the graph's vector (pointer + size), replacing `const std::vector<T>&`; the test
  // can search it like any range but cannot modify it.
  const auto order = g->topological_order();
  // C++20 ranges algorithms: std::ranges::find takes the range itself instead of an
  // iterator pair (`std::find(order.begin(), order.end(), x)`), so begin and end cannot be
  // mixed up between two containers; later uses here add projections and predicates.
  const auto pos = [&](std::string_view id) {
    return std::ranges::find(order, *g->index_of(id)) - order.begin();
  };
  REQUIRE(pos("users") < pos("api"));
  REQUIRE(pos("api") < pos("db"));
}

TEST_CASE("links resolve one-way latency from the datacenters and compute delay steps", "[graph]") {
  Scenario s = chain();
  s.datacenters = {"us", "eu"};
  s.nodes[0].dc = "us";
  s.nodes[1].dc = "us";
  s.nodes[2].dc = "eu";
  const auto g = Graph::build(s);
  REQUIRE(g.has_value());
  REQUIRE(g->links()[0].one_way == 250us);
  REQUIRE(g->links()[0].delay_steps == 0);  // 250 us < 100 ms step
  REQUIRE(g->links()[1].one_way == 75ms);
  REQUIRE(g->links()[1].latency_source == simsys::defaults::Source::dc_default);
  REQUIRE(g->links()[1].delay_steps == 1);  // round(75 ms / 100 ms) = round(0.75)
  s.links[1].latency = 250ms;
  REQUIRE(Graph::build(s)->links()[1].delay_steps == 3);
}

TEST_CASE("a step without a connecting link is an error", "[graph]") {
  Scenario s = chain();
  s.links.pop_back();  // api -> db missing
  const auto g = Graph::build(s);
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message == "node 'api' sends 'select' to 'db' but no link connects them");
}

TEST_CASE("a step's op must be allowed by the link's ops filter", "[graph]") {
  Scenario s = chain();
  s.links[1].ops = {"insert"};
  const auto g = Graph::build(s);
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message == "link api -> db does not carry op 'select' (ops: insert)");
}

TEST_CASE("client ops are routed to the links that carry them, split by weight", "[graph]") {
  Scenario s{.name = "fanout", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{100}, {{"meta", 0.5}, {"stream", 0.5}}), service("cdn"),
             service("lb"), service("lb2")};
  s.links = {LinkSpec{.from = "u", .to = "cdn", .ops = {"stream"}},
             LinkSpec{.from = "u", .to = "lb", .ops = {"meta"}, .weight = 3},
             LinkSpec{.from = "u", .to = "lb2", .ops = {"meta"}, .weight = 1}};
  const auto g = Graph::build(s);
  REQUIRE(g.has_value());
  const auto routes = g->client_routes(*g->index_of("u"));
  REQUIRE(routes.size() == 3);
  // C++20 ranges algorithm with a projection: find the route whose `target` member equals
  // the lb's index. The member pointer `&ClientRoute::target` is applied to each element
  // before comparing, replacing a hand-written lambda passed to std::find_if.
  const auto to_lb =
      std::ranges::find(routes, *g->index_of("lb"), &simsys::graph::ClientRoute::target);
  REQUIRE(to_lb != routes.end());
  REQUIRE(to_lb->op == "meta");
  REQUIRE_THAT(to_lb->share, WithinRel(0.75, 1e-12));
  const auto to_cdn =
      std::ranges::find(routes, *g->index_of("cdn"), &simsys::graph::ClientRoute::target);
  REQUIRE_THAT(to_cdn->share, WithinRel(1.0, 1e-12));
}

TEST_CASE("a client op with no link to carry it is an error", "[graph]") {
  Scenario s = chain();
  s.nodes[0].workload->op_shares = {{"get", 0.5}, {"put", 0.5}};
  s.links[0].ops = {"get"};
  const auto g = Graph::build(s);
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message == "client 'users': no link carries op 'put'");
}

TEST_CASE("a cycle becomes a feedback edge with at least one step of delay", "[graph]") {
  Scenario s{.name = "loop", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{1}, {{"msg", 1.0}}),
             service("chat", {{"msg", {Step{.to = "q", .op = "publish"}}}}),
             service("q", {{"publish", {Step{.to = "sync", .op = "job"}}}}),
             service("sync", {{"job", {Step{.to = "chat", .op = "push"}}}})};
  s.links = {LinkSpec{.from = "u", .to = "chat"}, LinkSpec{.from = "chat", .to = "q"},
             LinkSpec{.from = "q", .to = "sync"}, LinkSpec{.from = "sync", .to = "chat"}};
  const auto g = Graph::build(s);
  REQUIRE(g.has_value());
  REQUIRE(g->topological_order().size() == 4);
  const auto feedback = [](const simsys::graph::ResolvedLink& l) { return l.feedback; };
  REQUIRE(std::ranges::count_if(g->links(), feedback) == 1);
  const auto back = std::ranges::find_if(g->links(), feedback);
  REQUIRE(back->delay_steps >= 1);
  REQUIRE(g->nodes()[back->from].spec.id == "sync");  // the DFS back edge closes the loop
}

TEST_CASE("demand_link finds the link in either spelling of the direction", "[graph]") {
  Scenario s = chain();
  s.links[1] = LinkSpec{
      .from = "db", .to = "api", .dir = Direction::in};  // db <- api, written from db's side
  const auto g = Graph::build(s);
  REQUIRE(g.has_value());
  REQUIRE(g->demand_link(*g->index_of("api"), *g->index_of("db")) == 1);
  REQUIRE_FALSE(g->demand_link(*g->index_of("db"), *g->index_of("api")).has_value());
}

TEST_CASE("the feedback edge closes the loop seen from the clients, whatever the node order",
          "[graph]") {
  const std::array nodes{client("u", Rate{1}, {{"msg", 1.0}}),
                         service("chat", {{"msg", {Step{.to = "q", .op = "publish"}}}}),
                         service("q", {{"publish", {Step{.to = "sync", .op = "job"}}}}),
                         service("sync", {{"job", {Step{.to = "chat", .op = "push"}}}})};
  std::array<std::size_t, nodes.size()> file_order{0, 1, 2, 3};
  int orders_tried = 0;
  // C++20 std::ranges::next_permutation: rearranges the range into the next permutation in
  // lexicographic order and returns {end, found}; `found` turns false after the last one.
  // Starting from the sorted order, the loop visits all 4! = 24 file orders of the nodes.
  do {
    Scenario s{.name = "loop", .version = 1, .duration = 1min};
    for (const std::size_t i : file_order) s.nodes.push_back(nodes[i]);
    // Links name nodes by id, so they are the same whatever the file order.
    s.links = {LinkSpec{.from = "u", .to = "chat"}, LinkSpec{.from = "chat", .to = "q"},
               LinkSpec{.from = "q", .to = "sync"}, LinkSpec{.from = "sync", .to = "chat"}};
    const auto g = Graph::build(s);
    REQUIRE(g.has_value());
    const auto feedback = [](const simsys::graph::ResolvedLink& l) { return l.feedback; };
    REQUIRE(std::ranges::count_if(g->links(), feedback) == 1);
    const auto& back = *std::ranges::find_if(g->links(), feedback);
    REQUIRE(g->nodes()[back.from].spec.id == "sync");
    REQUIRE(g->nodes()[back.to].spec.id == "chat");
    ++orders_tried;
  } while (std::ranges::next_permutation(file_order).found);
  REQUIRE(orders_tried == 24);
}

TEST_CASE("a cross edge into a finished subtree is not feedback", "[graph]") {
  Scenario s{.name = "x", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{1}, {{"x", 1.0}}),
             service("a", {{"x", {Step{.to = "c", .op = "x"}}}}),
             service("b", {{"x", {Step{.to = "c", .op = "x"}}}}), service("c")};
  s.links = {LinkSpec{.from = "u", .to = "a"}, LinkSpec{.from = "u", .to = "b"},
             LinkSpec{.from = "a", .to = "c"}, LinkSpec{.from = "b", .to = "c"}};
  const auto g = Graph::build(s);
  REQUIRE(g.has_value());
  // The member pointer is the predicate: ranges algorithms call it through std::invoke,
  // so `&ResolvedLink::feedback` reads each link's flag without a lambda.
  REQUIRE(std::ranges::none_of(g->links(), &simsys::graph::ResolvedLink::feedback));
}

TEST_CASE("demand may not be sent into a client", "[graph]") {
  Scenario s = chain();
  s.nodes[2] = service("db", {{"select", {Step{.to = "users", .op = "notify"}}}});
  s.links.push_back(LinkSpec{.from = "db", .to = "users"});
  const auto g = Graph::build(s);
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message == "node 'db' sends 'notify' to client 'users'; clients only send");
}

TEST_CASE("a client may not route its ops into another client", "[graph]") {
  Scenario s = chain();
  s.nodes.push_back(client("bots", Rate{1}, {{"get", 1.0}}));
  s.links.push_back(LinkSpec{.from = "users", .to = "bots"});
  const auto g = Graph::build(s);
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message ==
          "client 'users': link to client 'bots' cannot carry op 'get'; clients only send");
}

TEST_CASE("every link of an op having weight zero is an error", "[graph]") {
  Scenario s = chain();
  s.links[0].weight = 0;
  const auto g = Graph::build(s);
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message == "client 'users': every link carrying op 'get' has weight 0");
}

TEST_CASE("a link longer than a million steps is rejected", "[graph]") {
  Scenario s = chain();
  s.step = 1us;
  s.links[1].latency = 1h;  // 3.6e9 steps
  const auto g = Graph::build(s);
  REQUIRE_FALSE(g.has_value());
  // C++20 std::string::starts_with: a prefix test in one call, replacing
  // `s.compare(0, prefix.size(), prefix) == 0` or `s.rfind(prefix, 0) == 0`.
  REQUIRE(g.error().message.starts_with("link api -> db: latency is 3600000000 steps"));
}

TEST_CASE("a node cycle that is not a call cycle keeps every synchronous hop", "[graph]") {
  // A.x -> B.y and B.z -> A.w: the nodes form a cycle, the calls do not.
  Scenario s{.name = "xcycle", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{10}, {{"x", 0.5}, {"z", 0.5}}),
             service("A", {{"x", {Step{.to = "B", .op = "y"}}}}),
             service("B", {{"z", {Step{.to = "A", .op = "w"}}}})};
  s.links = {LinkSpec{.from = "u", .to = "A", .ops = {"x"}},
             LinkSpec{.from = "u", .to = "B", .ops = {"z"}}, LinkSpec{.from = "A", .to = "B"},
             LinkSpec{.from = "B", .to = "A"}};
  const auto g = Graph::build(s);
  REQUIRE(g.has_value());
  REQUIRE(std::ranges::count_if(g->links(), [](const auto& l) { return l.feedback; }) ==
          1);  // node level
  const auto a = *g->index_of("A"), b = *g->index_of("B");
  REQUIRE_FALSE(g->is_async_hop(a, "x", b, "y"));
  REQUIRE_FALSE(g->is_async_hop(b, "z", a, "w"));  // synchronous even though B -> A is the
                                                   // node-level back edge
}

TEST_CASE("a node loop that ends in a terminal op has no asynchronous hop", "[graph]") {
  // chat.msg -> q.publish -> sync.job -> chat.push: the nodes loop, but push makes no call.
  Scenario s{.name = "loop", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{1}, {{"msg", 1.0}}),
             service("chat", {{"msg", {Step{.to = "q", .op = "publish"}}}, {"push", {}}}),
             service("q", {{"publish", {Step{.to = "sync", .op = "job"}}}}),
             service("sync", {{"job", {Step{.to = "chat", .op = "push"}}}})};
  s.links = {LinkSpec{.from = "u", .to = "chat"}, LinkSpec{.from = "chat", .to = "q"},
             LinkSpec{.from = "q", .to = "sync"}, LinkSpec{.from = "sync", .to = "chat"}};
  const auto g = *Graph::build(s);
  const auto chat = *g.index_of("chat"), q = *g.index_of("q"), sync = *g.index_of("sync");
  REQUIRE_FALSE(g.is_async_hop(chat, "msg", q, "publish"));
  REQUIRE_FALSE(g.is_async_hop(q, "publish", sync, "job"));
  REQUIRE_FALSE(g.is_async_hop(sync, "job", chat, "push"));  // push is terminal: no call cycle
}

TEST_CASE("a true call cycle (chat.msg -> q -> sync -> chat.msg) closes asynchronously",
          "[graph]") {
  Scenario s{.name = "loop2", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{1}, {{"msg", 1.0}}),
             service("chat", {{"msg", {Step{.to = "q", .op = "publish"}}}}),
             service("q", {{"publish", {Step{.to = "sync", .op = "job"}}}}),
             service("sync", {{"job", {Step{.to = "chat", .op = "msg", .factor = 0.5}}}})};
  s.links = {LinkSpec{.from = "u", .to = "chat"}, LinkSpec{.from = "chat", .to = "q"},
             LinkSpec{.from = "q", .to = "sync"}, LinkSpec{.from = "sync", .to = "chat"}};
  const auto g = *Graph::build(s);
  const auto chat = *g.index_of("chat"), q = *g.index_of("q"), sync = *g.index_of("sync");
  REQUIRE(g.is_async_hop(sync, "job", chat, "msg"));  // the hop that closes the cycle
  REQUIRE_FALSE(g.is_async_hop(chat, "msg", q, "publish"));
  REQUIRE_FALSE(g.is_async_hop(q, "publish", sync, "job"));
}

TEST_CASE("a drained link keeps its route with share zero and the others split the rest",
          "[graph]") {
  Scenario s = chain();
  s.nodes.push_back(service("api2"));
  s.links.push_back(LinkSpec{.from = "users", .to = "api2", .weight = 0});
  const auto g = *Graph::build(s);
  const auto routes = g.client_routes(0);
  REQUIRE(routes.size() == 2);
  REQUIRE(
      std::ranges::find(routes, *g.index_of("api"), &simsys::graph::ClientRoute::target)->share ==
      1.0);
  REQUIRE(
      std::ranges::find(routes, *g.index_of("api2"), &simsys::graph::ClientRoute::target)->share ==
      0.0);
}

TEST_CASE("programmatic scenarios with negative latency or a bad weight are rejected", "[graph]") {
  Scenario s = chain();
  s.links[0].latency = simsys::SimTime{-1};
  auto g = Graph::build(s);
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message == "link users -> api: latency must not be negative");
  for (const double weight :
       {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -1.0}) {
    s = chain();
    s.links[0].weight = weight;
    g = Graph::build(s);
    REQUIRE_FALSE(g.has_value());
    REQUIRE(g.error().message == "link users -> api: weight must be a finite non-negative number");
  }
}

TEST_CASE("a programmatic scenario with a non-positive step is rejected", "[graph]") {
  Scenario s = chain();
  s.step = simsys::SimTime{0};
  const auto g = Graph::build(s);
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message == "step must be positive");
}

TEST_CASE("a load balancer splits each op across its outgoing links by weight", "[graph]") {
  Scenario s{.name = "lb", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{100}, {{"get", 1.0}}), load_balancer("lb", LbPolicy::weighted),
             service("a"), service("b")};
  s.links = {LinkSpec{.from = "u", .to = "lb"}, LinkSpec{.from = "lb", .to = "a", .weight = 3},
             LinkSpec{.from = "lb", .to = "b", .weight = 1}};
  const auto g = *Graph::build(s);
  // routes(i) is the generalised client_routes(i): any node that only splits flow
  // (a client or a load balancer) has routes; a service has steps instead.
  const auto routes = g.routes(*g.index_of("lb"));
  REQUIRE(routes.size() == 2);
  REQUIRE_THAT(std::ranges::find(routes, *g.index_of("a"), &simsys::graph::Route::target)->share,
               WithinRel(0.75, 1e-12));
  REQUIRE_THAT(std::ranges::find(routes, *g.index_of("b"), &simsys::graph::Route::target)->share,
               WithinRel(0.25, 1e-12));
}

TEST_CASE("a load balancer's routes honour link op filters and reject an op no link carries",
          "[graph]") {
  Scenario s{.name = "lb", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{100}, {{"get", 0.5}, {"put", 0.5}}),
             load_balancer("lb", LbPolicy::round_robin), service("a"), service("b")};
  s.links = {LinkSpec{.from = "u", .to = "lb"}, LinkSpec{.from = "lb", .to = "a", .ops = {"get"}},
             LinkSpec{.from = "lb", .to = "b", .ops = {"get"}}};
  const auto g = Graph::build(s);
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message == "node 'lb': no link carries op 'put'");
}

TEST_CASE("least_connections marks its downstream service as pooled", "[graph]") {
  Scenario s{.name = "lb", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{100}, {{"get", 1.0}}),
             load_balancer("lb", LbPolicy::least_connections), service("a")};
  s.nodes[2].replicas = 4;
  s.links = {LinkSpec{.from = "u", .to = "lb"}, LinkSpec{.from = "lb", .to = "a"}};
  const auto g = *Graph::build(s);
  REQUIRE(g.nodes()[*g.index_of("a")].pooled);
  REQUIRE(g.nodes()[*g.index_of("a")].imbalance == 1.0);
}

// The ring heuristic (numerics §8): over M servers holding N copies of each key, the hottest
// carries k = 1 + z_M / sqrt(N vnodes) times the average, z_M the expected maximum of M standard
// normals by Blom's approximation, Phi^-1((M - 0.375) / (M + 0.25)) (pinned in test_queueing.cpp):
// z_4 = 1.0491313979639711, z_8 = 1.4342001596863794. A balancer's ring holds one copy of a key (N
// = 1).
namespace {
constexpr double blom_z4 = 1.0491313979639711;
constexpr double blom_z8 = 1.4342001596863794;
}  // namespace

TEST_CASE("consistent_hash gives its downstream the expected hottest of its replicas' shares",
          "[graph]") {
  Scenario s{.name = "lb", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{100}, {{"get", 1.0}}), load_balancer("lb", LbPolicy::consistent_hash),
             service("a")};
  s.nodes[1].vnodes = 100;
  s.nodes[2].replicas = 4;
  s.links = {LinkSpec{.from = "u", .to = "lb"}, LinkSpec{.from = "lb", .to = "a"}};
  const auto g = *Graph::build(s);
  // 1 + z_4/sqrt(100) = 1.1049: a server's share spreads about 1/sqrt(vnodes) around the
  // average (book ch. 5: 10 % at 100 virtual nodes), and the hottest of 4 sits z_4 = 1.049
  // standard deviations above it.
  REQUIRE_THAT(g.nodes()[*g.index_of("a")].imbalance, WithinRel(1.0 + blom_z4 / 10.0, 1e-12));
  REQUIRE_THAT(g.nodes()[*g.index_of("a")].imbalance, WithinRel(1.1049, 1e-4));
  REQUIRE_FALSE(g.nodes()[*g.index_of("a")].pooled);
}

// A weight-0 link is a drained backend: nothing travels on it, so it cannot close a loop of
// calls either. Here x reaches A through lb (B's link is drained) and y reaches A directly;
// A.y calls lb.x. Only through the drained link would lb.x -> B.x -> A.y -> lb.x be a cycle,
// and a hop marked asynchronous for it would wrongly drop A.y's wait for lb.x from latency.
TEST_CASE("a drained (weight 0) balancer route closes no call cycle", "[graph]") {
  Scenario s{.name = "drained", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{100}, {{"x", 0.5}, {"y", 0.5}}),
             load_balancer("lb", LbPolicy::weighted),
             service("A", {{"x", {}}, {"y", {Step{.to = "lb", .op = "x"}}}}),
             service("B", {{"x", {Step{.to = "A", .op = "y"}}}})};
  s.links = {LinkSpec{.from = "u", .to = "lb", .ops = {"x"}},
             LinkSpec{.from = "u", .to = "A", .ops = {"y"}},
             LinkSpec{.from = "lb", .to = "A", .weight = 1},
             LinkSpec{.from = "lb", .to = "B", .weight = 0},
             LinkSpec{.from = "B", .to = "A"},
             LinkSpec{.from = "A", .to = "lb"}};
  const auto g = *Graph::build(s);
  REQUIRE(g.async_hops().empty());
  REQUIRE_FALSE(g.is_async_hop(*g.index_of("A"), "y", *g.index_of("lb"), "x"));
  REQUIRE(g.arriving_ops(*g.index_of("B")).empty());  // no flow can bring x to B
}

// A drained route sends the node behind it nothing, so the balancer's policy says nothing
// about how that node's replicas queue: l2 (least_connections) drains its link to `a`, and
// `a` keeps l1's independent copies.
TEST_CASE("a drained (weight 0) balancer route imposes no replica model on its target", "[graph]") {
  Scenario s{.name = "drained", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{100}, {{"op", 1.0}}), load_balancer("l1", LbPolicy::round_robin),
             load_balancer("l2", LbPolicy::least_connections), service("a"), service("b")};
  s.nodes[3].replicas = 2;
  s.links = {LinkSpec{.from = "u", .to = "l1"}, LinkSpec{.from = "u", .to = "l2"},
             LinkSpec{.from = "l1", .to = "a"}, LinkSpec{.from = "l2", .to = "a", .weight = 0},
             LinkSpec{.from = "l2", .to = "b", .weight = 1}};
  const auto g = Graph::build(s);
  REQUIRE(g.has_value());
  REQUIRE_FALSE(g->nodes()[*g->index_of("a")].pooled);
  REQUIRE(g->nodes()[*g->index_of("b")].pooled);
}

// What must agree between two balancers in front of one node is the queueing model they give
// its replicas, not the policy's name: round_robin and weighted both deal requests out blindly
// (independent copies), so they may share a node; round_robin and least_connections (pooled)
// may not, and the error names both balancers and their models.
TEST_CASE("balancers whose policies give the same replica model may front one node", "[graph]") {
  const auto two = [](LbPolicy first, LbPolicy second) {
    Scenario s{.name = "two", .version = 1, .duration = 1min};
    s.nodes = {client("u", Rate{100}, {{"op", 1.0}}), load_balancer("l1", first),
               load_balancer("l2", second), service("a")};
    s.nodes[3].replicas = 2;
    s.links = {LinkSpec{.from = "u", .to = "l1"}, LinkSpec{.from = "u", .to = "l2"},
               LinkSpec{.from = "l1", .to = "a"}, LinkSpec{.from = "l2", .to = "a"}};
    return Graph::build(s);
  };
  REQUIRE(two(LbPolicy::round_robin, LbPolicy::weighted).has_value());
  REQUIRE(two(LbPolicy::weighted, LbPolicy::round_robin).has_value());
  REQUIRE(two(LbPolicy::consistent_hash, LbPolicy::consistent_hash).has_value());
  const auto mixed = two(LbPolicy::round_robin, LbPolicy::least_connections);
  REQUIRE_FALSE(mixed.has_value());
  REQUIRE(mixed.error().message ==
          "node 'a' is behind load balancers with different replica models ('l1' independent, "
          "'l2' pooled)");
  const auto hashed = two(LbPolicy::least_connections, LbPolicy::consistent_hash);
  REQUIRE_FALSE(hashed.has_value());
  REQUIRE(hashed.error().message ==
          "node 'a' is behind load balancers with different replica models ('l1' pooled, "
          "'l2' imbalanced)");
}

TEST_CASE("a load balancer needs at least one outgoing link", "[graph]") {
  Scenario s{.name = "lb", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{100}, {{"get", 1.0}}), load_balancer("lb", LbPolicy::round_robin)};
  s.links = {LinkSpec{.from = "u", .to = "lb"}};
  const auto g = Graph::build(s);
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message == "node 'lb': a load balancer needs an outgoing link");
}

namespace {
// u -> limiter -> api; the limiter as given.
Scenario behind_limiter(NodeSpec limiter) {
  Scenario s{.name = "limiter", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{100}, {{"call", 1.0}}), std::move(limiter), service("api")};
  s.links = {LinkSpec{.from = "u", .to = "limiter"}, LinkSpec{.from = "limiter", .to = "api"}};
  return s;
}
NodeSpec limiter_of(simsys::model::LimiterAlgorithm algorithm, std::optional<int> burst) {
  return NodeSpec{.id = "limiter",
                  .type = NodeType::rate_limiter,
                  .algorithm = algorithm,
                  .limit = Rate{1000},
                  .burst = burst};
}
}  // namespace

// A windowed algorithm counts limit x window per window; it has no bucket for `burst` to
// size, so a burst on one is a mistake the graph names, as the loader does for YAML.
TEST_CASE("a burst on a windowed limiter written in code is rejected", "[graph]") {
  // C++20 `using enum`: brings the enumerators of a scoped enum into this scope, so the
  // list below reads `fixed_window` instead of `LimiterAlgorithm::fixed_window`. Replaces
  // falling back to an unscoped enum (which leaks its names everywhere) to get short names.
  using enum simsys::model::LimiterAlgorithm;
  for (const auto algorithm : {fixed_window, sliding_log, sliding_counter}) {
    const auto g = Graph::build(behind_limiter(limiter_of(algorithm, 50)));
    REQUIRE_FALSE(g.has_value());
    REQUIRE(g.error().message ==
            "node 'limiter': burst applies only to token_bucket and leaky_bucket");
  }
  REQUIRE(Graph::build(behind_limiter(limiter_of(token_bucket, 50))).has_value());
  REQUIRE(Graph::build(behind_limiter(limiter_of(leaky_bucket, 50))).has_value());
}

// A windowed algorithm moves its counters at window boundaries, and the fluid engine can
// only see a boundary at a step boundary: a 150 ms window stepped every 100 ms would reset
// at 100 ms and 200 ms, so neither its length nor its limit x window would be the one the
// scenario states. The window must hold a whole number of steps, at least one. A bucket
// uses its window only to size the default burst, so any positive window will do there.
TEST_CASE("a windowed limiter's window must be a whole number of steps", "[graph]") {
  using enum simsys::model::LimiterAlgorithm;
  const auto with_window = [](simsys::model::LimiterAlgorithm algorithm,
                              std::optional<simsys::SimTime> window) {
    auto limiter = limiter_of(algorithm, std::nullopt);
    limiter.window = window;
    return behind_limiter(std::move(limiter));
  };
  for (const auto algorithm : {fixed_window, sliding_log, sliding_counter}) {
    const auto off_grid = Graph::build(with_window(algorithm, 150ms));
    REQUIRE_FALSE(off_grid.has_value());
    REQUIRE(off_grid.error().message ==
            "node 'limiter': window must be a positive whole number of steps "
            "(window 150ms, step 100ms)");
    const auto under_a_step = Graph::build(with_window(algorithm, 50ms));
    REQUIRE_FALSE(under_a_step.has_value());
    REQUIRE(under_a_step.error().message ==
            "node 'limiter': window must be a positive whole number of steps "
            "(window 50ms, step 100ms)");
    REQUIRE(Graph::build(with_window(algorithm, 100ms)).has_value());
    REQUIRE(Graph::build(with_window(algorithm, 1s)).has_value());
    // No window stated: the default one second is checked against the step too.
    auto odd_step = with_window(algorithm, std::nullopt);
    odd_step.step = 30ms;
    const auto default_window = Graph::build(odd_step);
    REQUIRE_FALSE(default_window.has_value());
    REQUIRE(default_window.error().message ==
            "node 'limiter': window must be a positive whole number of steps "
            "(window 1s, step 30ms)");
  }
  REQUIRE(Graph::build(with_window(token_bucket, 150ms)).has_value());
  REQUIRE(Graph::build(with_window(leaky_bucket, 150ms)).has_value());
}

// A leaky bucket's queue IS the bucket (D/1: one slot leaking a request every 1/limit), so a
// capacity, latency, concurrency, queue limit or service variability would be a second,
// contradicting statement of `limit`, `burst` and the regular leak.
TEST_CASE("a leaky bucket written in code takes only limit and burst", "[graph]") {
  const auto rejected = [](NodeSpec limiter) {
    const auto g = Graph::build(behind_limiter(std::move(limiter)));
    REQUIRE_FALSE(g.has_value());
    REQUIRE(g.error().message ==
            "node 'limiter': leaky_bucket takes limit and burst; capacity, latency, "
            "concurrency, queue_limit and service_variability are not valid");
  };
  auto with_capacity = limiter_of(simsys::model::LimiterAlgorithm::leaky_bucket, 500);
  with_capacity.capacity = Rate{5000};
  rejected(with_capacity);
  auto with_latency = limiter_of(simsys::model::LimiterAlgorithm::leaky_bucket, 500);
  with_latency.latency = 1ms;
  rejected(with_latency);
  auto with_queue = limiter_of(simsys::model::LimiterAlgorithm::leaky_bucket, 500);
  with_queue.advanced.queue_limit = 10;
  rejected(with_queue);
  auto with_servers = limiter_of(simsys::model::LimiterAlgorithm::leaky_bucket, 500);
  with_servers.advanced.concurrency = 4;
  rejected(with_servers);
  // The leak is regular by definition (service variability 0): a scenario's own number would be
  // silently overridden, so it is refused like the others.
  auto with_variability = limiter_of(simsys::model::LimiterAlgorithm::leaky_bucket, 500);
  with_variability.advanced.service_variability = 1.0;
  rejected(with_variability);
}

// Ch. 4's leaky bucket leaks requests at a FIXED rate: one slot that takes exactly 1/limit
// per request, so the bucket queues like M/D/1 (deterministic service, SCV 0). With N
// replicas each copy is one slot of limit/N and room burst/N.
TEST_CASE("a leaky bucket's queue is one deterministic slot of rate limit with room burst",
          "[graph]") {
  auto spec = limiter_of(simsys::model::LimiterAlgorithm::leaky_bucket, 500);
  const auto one = Graph::build(behind_limiter(spec));
  REQUIRE(one.has_value());
  const auto& cap = one->nodes()[1].capacity;
  REQUIRE(cap.service_rate == Rate{1000});
  REQUIRE_THAT(cap.service_time_s, WithinRel(0.001, 1e-12));
  REQUIRE(cap.concurrency == 1);
  REQUIRE(cap.service_variability == 0.0);
  REQUIRE(cap.queue_limit == 500.0);
  spec.replicas = 2;
  const auto two = Graph::build(behind_limiter(spec));
  REQUIRE(two.has_value());
  const auto& per_copy = two->nodes()[1].capacity;
  REQUIRE(per_copy.service_rate == Rate{500});
  REQUIRE_THAT(per_copy.service_time_s, WithinRel(0.002, 1e-12));
  REQUIRE(per_copy.concurrency == 1);
  REQUIRE(per_copy.queue_limit == 250.0);
}

// ---- Caches (spec §17 entry 7) ----

// The shape of one resolved service hop, as Graph::service_hops exports it: the node it calls,
// the op it sends, `fraction`, the share of the service's requests that take the hop at all (a
// probability: 1, or for an `on: miss` step the preceding cache's miss fraction 1 - h),
// `factor`, the requests it sends per request that takes it (the step's own factor, times the
// lookup's for an `on: miss` step), and how it combines with the other hops.
static_assert(std::is_same_v<decltype(ServiceHop::target), std::size_t>);
static_assert(std::is_same_v<decltype(ServiceHop::op), std::string>);
static_assert(std::is_same_v<decltype(ServiceHop::fraction), double>);
static_assert(std::is_same_v<decltype(ServiceHop::factor), double>);
static_assert(std::is_same_v<decltype(ServiceHop::mode), StepMode>);

namespace {
// A cache node with a fixed hit ratio; `backend` (the node it forwards to, for read_through,
// write_through and write_around) only when given. Designated initializers follow declaration
// order: backend, then hit_ratio, then strategy.
NodeSpec cache(std::string id, double hit, CacheStrategy strategy, std::string backend = "") {
  NodeSpec node{.id = std::move(id),
                .type = NodeType::cache,
                .hit_ratio = HitRatio{.fixed = hit},
                .strategy = strategy};
  if (!backend.empty()) node.backend = std::move(backend);
  return node;
}
// u -> web -> `cache_node` and web -> db, with `steps` as web's steps for the op `get`; when the
// cache names a backend, cache_node -> that node's link too (the backend is always `db`).
Scenario cached_web(NodeSpec cache_node, std::vector<Step> steps) {
  const bool with_backend = cache_node.backend.has_value();
  Scenario s{.name = "cached", .version = 1, .duration = 1min};
  const std::string cache_id = cache_node.id;
  s.nodes = {client("u", Rate{100}, {{"get", 1.0}}), service("web", {{"get", std::move(steps)}}),
             std::move(cache_node), service("db")};
  s.links = {LinkSpec{.from = "u", .to = "web", .dir = Direction::inout},
             LinkSpec{.from = "web", .to = cache_id, .dir = Direction::inout},
             LinkSpec{.from = "web", .to = "db", .dir = Direction::inout}};
  if (with_backend) {
    s.links.push_back(LinkSpec{.from = cache_id, .to = "db", .dir = Direction::inout});
  }
  return s;
}
// web.get = [cache.read, db.read on miss]: the read path of ch. 1's cache, which the book calls
// read-through and the industry cache-aside (the application fetches the miss).
std::vector<Step> read_then_db_on_miss() {
  return {Step{.to = "cache", .op = "read"},
          Step{.to = "db", .op = "read", .on = StepCondition::miss}};
}
}  // namespace

// Ch. 1's cache (the book's "read-through", the industry's cache-aside): the web tier asks the
// cache first and goes to the database only for what the cache did not have. In the fluid
// model the share of requests that miss is 1 - hit ratio, so a conditional `on: miss` step
// carries that fraction of the flow.
TEST_CASE("a service step after a cache step fires on miss for the cache's miss fraction",
          "[graph]") {
  const auto s =
      cached_web(cache("cache", 0.8, CacheStrategy::cache_aside), read_then_db_on_miss());
  const auto built = Graph::build(s);
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  const auto& g = *built;
  REQUIRE_THAT(*g.hit_ratio(*g.index_of("cache")), WithinRel(0.8, 1e-12));
  const auto hops = g.service_hops(*g.index_of("web"), "get");
  REQUIRE(hops.size() == 2);
  REQUIRE(hops[0].target == *g.index_of("cache"));
  REQUIRE(hops[0].op == "read");
  REQUIRE_THAT(hops[0].fraction, WithinRel(1.0, 1e-12));  // the cache is asked about every read
  REQUIRE(hops[1].target == *g.index_of("db"));
  REQUIRE(hops[1].op == "read");
  REQUIRE_THAT(hops[1].fraction, WithinRel(0.2, 1e-12));  // 1 - 0.8
  REQUIRE(hops[1].mode == StepMode::sequential);
}

TEST_CASE("a node that is not a cache has no hit ratio", "[graph]") {
  const auto g = *Graph::build(
      cached_web(cache("cache", 0.8, CacheStrategy::cache_aside), read_then_db_on_miss()));
  REQUIRE_FALSE(g.hit_ratio(*g.index_of("web")).has_value());
  REQUIRE_FALSE(g.hit_ratio(*g.index_of("u")).has_value());
}

TEST_CASE("a cache that always hits sends nothing on to the miss step", "[graph]") {
  const auto g = *Graph::build(
      cached_web(cache("cache", 1.0, CacheStrategy::cache_aside), read_then_db_on_miss()));
  const auto hops = g.service_hops(*g.index_of("web"), "get");
  REQUIRE(hops.size() == 2);
  REQUIRE_THAT(hops[1].fraction, WithinRel(0.0, 1e-12));
}

// A miss step needs a cache in front of it. A CDN can miss too, but it fetches its own misses from
// its origin, so a miss step after one is refused with a message of its own (below): the message
// here names the one kind of node a miss step may follow.
TEST_CASE("an on-miss step must follow a cache step", "[graph]") {
  const std::string expected = "node 'web': an 'on: miss' step must follow a cache step";
  SECTION("as the first step") {
    const auto g =
        Graph::build(cached_web(cache("cache", 0.8, CacheStrategy::cache_aside),
                                {Step{.to = "db", .op = "read", .on = StepCondition::miss}}));
    REQUIRE_FALSE(g.has_value());
    REQUIRE(g.error().message == expected);
  }
  SECTION("after a step to a service, which has no miss to report") {
    const auto g =
        Graph::build(cached_web(cache("cache", 0.8, CacheStrategy::cache_aside),
                                {Step{.to = "db", .op = "read"},
                                 Step{.to = "db", .op = "read", .on = StepCondition::miss}}));
    REQUIRE_FALSE(g.has_value());
    REQUIRE(g.error().message == expected);
  }
}

// An `on: failure` step reports the failure of the step before it (spec §5: it belongs to the
// nearest unconditional step before it), so one with nothing before it reports nothing: refused at
// build, naming the node. Phase 2 parsed such a step and ignored it, so the mistake went unseen.
TEST_CASE("an on-failure step needs a step before it", "[graph]") {
  const std::string expected = "node 'web': an on: failure step needs a step before it";
  SECTION("as the first step") {
    const auto g = Graph::build(cached_web(
        service("log"), {Step{.to = "log", .op = "write", .on = StepCondition::failure}}));
    REQUIRE_FALSE(g.has_value());
    REQUIRE(g.error().message == expected);
  }
  SECTION("as the first step of several failure steps, none of which has a step to report on") {
    const auto g = Graph::build(cached_web(
        service("log"), {Step{.to = "log", .op = "write", .on = StepCondition::failure},
                         Step{.to = "db", .op = "write", .on = StepCondition::failure}}));
    REQUIRE_FALSE(g.has_value());
    REQUIRE(g.error().message == expected);
  }
  SECTION("after an unconditional step it builds") {
    const auto g = Graph::build(cached_web(
        service("log"), {Step{.to = "db", .op = "write"},
                         Step{.to = "log", .op = "write", .on = StepCondition::failure}}));
    INFO((g ? "" : g.error().message));
    REQUIRE(g.has_value());
  }
}

// An `on: failure` step runs once the step it reports on has failed (spec §5: a sequential hop),
// so it cannot be timed beside that step in a parallel group: refused, as a parallel miss step is.
TEST_CASE("a parallel on-failure step is refused", "[graph]") {
  const auto g = Graph::build(cached_web(
      service("log"),
      {Step{.to = "db", .op = "write"},
       Step{
           .to = "log", .op = "write", .on = StepCondition::failure, .mode = StepMode::parallel}}));
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message ==
          "node 'web': an on: failure step must be sequential (it runs after the step it reports "
          "on has failed)");
}

// An `on: failure` step reports on a step of an op's step list (the nearest unconditional one
// before it); a cache's `then` list follows no such step. Phase 2 kept such a step and never fired
// it; now that failure steps fire, the scenario is told instead of running without it.
TEST_CASE("a cache's then step on failure is refused", "[graph]") {
  auto c = cache("c", 0.5, CacheStrategy::write_through, "db");
  c.then = {Step{.to = "db", .op = "audit", .on = StepCondition::failure}};
  const auto built = Graph::build(cached_web(std::move(c), {Step{.to = "c", .op = "write"}}));
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "node 'c': a then step on failure is not modelled at a cache (an on: failure step "
          "reports on a step of an op's step list)");
}

// Read-through as this model names it (the cache-driven fetch): on a miss the CACHE fetches the
// value from its backend and answers with it, so the application never sees a miss to act on.
// A miss step after it would fetch every miss a second time.
TEST_CASE(
    "an on-miss step after a read_through cache is refused: the cache already fetched the miss",
    "[graph]") {
  const auto g = Graph::build(
      cached_web(cache("cache", 0.8, CacheStrategy::read_through, "db"), read_then_db_on_miss()));
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message ==
          "node 'web': an 'on: miss' step cannot follow read_through cache 'cache' (the cache "
          "already fetched the miss)");
}

// A miss step runs only once the cache has answered "miss", so it cannot be sent alongside the
// lookup; and a lookup sent in parallel with other calls would let the miss step start before
// the answer arrives. Either way the step would be timed as a branch of a parallel max, not
// after the lookup, so both are refused.
TEST_CASE("an on-miss step cannot be parallel: it waits for the cache's answer", "[graph]") {
  const std::string expected =
      "node 'web': an 'on: miss' step and the cache step it follows must be sequential (the "
      "miss step waits for the cache's answer)";
  SECTION("the miss step is parallel") {
    const auto g = Graph::build(cached_web(
        cache("cache", 0.8, CacheStrategy::cache_aside),
        {Step{.to = "cache", .op = "read"},
         Step{.to = "db", .op = "read", .on = StepCondition::miss, .mode = StepMode::parallel}}));
    REQUIRE_FALSE(g.has_value());
    REQUIRE(g.error().message == expected);
  }
  SECTION("the lookup it follows is parallel") {
    const auto g =
        Graph::build(cached_web(cache("cache", 0.8, CacheStrategy::cache_aside),
                                {Step{.to = "cache", .op = "read", .mode = StepMode::parallel},
                                 Step{.to = "db", .op = "read", .on = StepCondition::miss}}));
    REQUIRE_FALSE(g.has_value());
    REQUIRE(g.error().message == expected);
  }
}

// A request that looks up k keys (factor k on the cache step) misses k(1 - h) of them on
// average, and the application fetches every key it missed, so the miss step sends
// rate x k x (1 - h) requests (linearity of expectation: true however the lookups' hits are
// correlated). The probability that the request takes the miss step stays 1 - h: the latency
// model still treats a request's lookups as hitting or missing together.
TEST_CASE(
    "a miss step after a lookup of factor k fetches every missed lookup: k(1 - h) per request",
    "[graph]") {
  const auto built =
      Graph::build(cached_web(cache("cache", 0.8, CacheStrategy::cache_aside),
                              {Step{.to = "cache", .op = "read", .factor = 20},
                               Step{.to = "db", .op = "read", .on = StepCondition::miss}}));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  const auto hops = built->service_hops(*built->index_of("web"), "get");
  REQUIRE(hops.size() == 2);
  REQUIRE_THAT(hops[1].fraction * hops[1].factor, WithinRel(20 * 0.2, 1e-12));
  REQUIRE_THAT(hops[1].fraction, WithinRel(0.2, 1e-12));  // still a probability
}

// Only reads hit or miss: a write to a cache (an op in its write_ops) updates or invalidates
// the entry and has no "miss" to report, so a miss step after it would never mean anything.
TEST_CASE("a miss step after a write lookup is refused", "[graph]") {
  const auto g =
      Graph::build(cached_web(cache("cache", 0.8, CacheStrategy::cache_aside),
                              {Step{.to = "cache", .op = "write"},
                               Step{.to = "db", .op = "write", .on = StepCondition::miss}}));
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message ==
          "node 'web': an 'on: miss' step cannot follow a write to cache 'cache' (writes do not "
          "miss)");
}

// A web tier in front of a CDN: u -> web -> cdn -> db, with `steps` as web's steps for `get` and
// db the CDN's origin (cached_web's scenario plus the link the origin needs).
Scenario web_behind_cdn(std::optional<HitRatio> hit, std::vector<Step> steps) {
  NodeSpec cdn{.id = "cdn", .type = NodeType::cdn, .hit_ratio = hit};
  cdn.origin = "db";
  auto s = cached_web(std::move(cdn), std::move(steps));
  s.links.push_back(LinkSpec{.from = "cdn", .to = "db", .dir = Direction::inout});
  return s;
}

// A CDN is a cache at the network's edge (ch. 1): a fixed hit ratio on it means what it means on a
// cache. But a CDN fetches its own misses from its origin (spec §18 entry 10), so the miss share 1
// - h travels on the cdn -> origin edge, and a service's `on: miss` step after a CDN lookup, which
// would fetch every miss a second time, is refused, as after a read_through cache.
TEST_CASE("a cdn's fixed hit ratio feeds its own origin edge, not a service's miss step",
          "[graph]") {
  SECTION("the misses go to the origin") {
    const auto built =
        Graph::build(web_behind_cdn(HitRatio{.fixed = 0.95}, {Step{.to = "cdn", .op = "read"}}));
    INFO((built ? "" : built.error().message));
    REQUIRE(built.has_value());
    const auto cdn = *built->index_of("cdn");
    REQUIRE_THAT(*built->hit_ratio(cdn), WithinRel(0.95, 1e-12));
    REQUIRE_THAT(built->backend_fraction(cdn, "read"), WithinRel(0.05, 1e-12));
  }
  SECTION("a service's miss step after the cdn is refused") {
    const auto built = Graph::build(web_behind_cdn(
        HitRatio{.fixed = 0.95}, {Step{.to = "cdn", .op = "read"},
                                  Step{.to = "db", .op = "read", .on = StepCondition::miss}}));
    REQUIRE_FALSE(built.has_value());
    REQUIRE(built.error().message ==
            "node 'web': an 'on: miss' step cannot follow cdn 'cdn' (the cdn already fetched the "
            "miss from its origin)");
  }
}

// A `che` CDN needs a size in keys for the Che approximation. Without one the graph still builds
// (so explain can print the scenario) but has no number for it, and its misses count as every
// read: the whole read load goes on to the origin, the safe side on paper.
TEST_CASE(
    "a che cdn without a keys size builds with no hit ratio and sends every read to its "
    "origin",
    "[graph]") {
  const auto built =
      Graph::build(web_behind_cdn(HitRatio{.che = true}, {Step{.to = "cdn", .op = "read"}}));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  const auto cdn = *built->index_of("cdn");
  REQUIRE_FALSE(built->hit_ratio(cdn).has_value());
  REQUIRE_THAT(built->backend_fraction(cdn, "read"), WithinRel(1.0, 1e-12));
}

// read_through: on a miss the cache itself fetches from its backend; write_through: it writes
// to the backend before answering. Both put a call from the cache to the backend on the graph,
// so the backend's demand and the cycle checks see it. cache_aside never calls its backend.
TEST_CASE("read_through and write_through caches get a demand edge to their backend", "[graph]") {
  const std::vector<Step> to_cache{Step{.to = "cache", .op = "read"}};
  for (const auto strategy : {CacheStrategy::read_through, CacheStrategy::write_through}) {
    CAPTURE(static_cast<int>(strategy));
    const auto built = Graph::build(cached_web(cache("cache", 0.8, strategy, "db"), to_cache));
    INFO((built ? "" : built.error().message));
    REQUIRE(built.has_value());
    const auto cache_index = *built->index_of("cache");
    const auto db_index = *built->index_of("db");
    const auto link = built->demand_link(cache_index, db_index);
    REQUIRE(link.has_value());
    // C++23 std::ranges::contains: "is this value in the range?" in one call, replacing
    // `std::find(begin, end, value) != end`; DemandEdge's defaulted == makes it comparable.
    REQUIRE(std::ranges::contains(built->edges(),
                                  DemandEdge{.from = cache_index, .to = db_index, .link = *link}));
  }
}

TEST_CASE("a backend on a cache_aside cache is refused: the application reads the store itself",
          "[graph]") {
  const auto built = Graph::build(
      cached_web(cache("cache", 0.8, CacheStrategy::cache_aside, "db"), read_then_db_on_miss()));
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message == "node 'cache': backend is unused with strategy cache_aside");
}

// A store that calls back into the cache in front of it (db.read -> cache.read -> db.read)
// makes the cache's fetch a call that waits for itself. The call-cycle search would make that
// hop asynchronous, but a read_through miss is answered only after the fetch: the cache cannot
// hand it off, so the scenario is refused.
TEST_CASE("a cache whose backend hop closes a call cycle is refused", "[graph]") {
  Scenario s{.name = "loop", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{100}, {{"read", 1.0}}),
             service("db", {{"read", {Step{.to = "cache", .op = "read"}}}}),
             cache("cache", 0.8, CacheStrategy::read_through, "db")};
  s.links = {LinkSpec{.from = "u", .to = "db", .dir = Direction::inout},
             LinkSpec{.from = "db", .to = "cache", .dir = Direction::inout},
             LinkSpec{.from = "cache", .to = "db", .dir = Direction::inout}};
  const auto g = Graph::build(s);
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message == "node 'cache': backend 'db' closes a call cycle");
}

// The same loop entered at the cache (the client calls it, directly or through a front service):
// u -> cache.read -> db.read -> cache.read. The depth-first search that picks the asynchronous hops
// (detail::find_async_hops) now starts at the cache and cuts db's call back to it, so the cache's
// fetch is no cut hop; yet it still waits for itself. The refusal asks whether the fetch lies on
// ANY call cycle (its two ends in one strongly connected component of the call graph), so where the
// client enters no longer matters.
namespace {
Scenario loop_entered_at_cache(bool through_front) {
  Scenario s{.name = "loop-at-cache", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{100}, {{"read", 1.0}}),
             cache("cache", 0.5, CacheStrategy::read_through, "db"),
             service("db", {{"read", {Step{.to = "cache", .op = "read"}}}})};
  s.links = {LinkSpec{.from = "cache", .to = "db", .dir = Direction::inout},
             LinkSpec{.from = "db", .to = "cache", .dir = Direction::inout}};
  if (!through_front) {
    s.links.push_back(LinkSpec{.from = "u", .to = "cache", .dir = Direction::inout});
    return s;
  }
  s.nodes.push_back(service("front", {{"read", {Step{.to = "cache", .op = "read"}}}}));
  s.links.push_back(LinkSpec{.from = "u", .to = "front", .dir = Direction::inout});
  s.links.push_back(LinkSpec{.from = "front", .to = "cache", .dir = Direction::inout});
  return s;
}
}  // namespace

TEST_CASE("a cache whose backend hop closes a call cycle is refused wherever the client enters",
          "[graph]") {
  SECTION("the client calls the cache") {
    const auto g = Graph::build(loop_entered_at_cache(false));
    REQUIRE_FALSE(g.has_value());
    REQUIRE(g.error().message == "node 'cache': backend 'db' closes a call cycle");
  }
  SECTION("the client calls a front service that calls the cache") {
    const auto g = Graph::build(loop_entered_at_cache(true));
    REQUIRE_FALSE(g.has_value());
    REQUIRE(g.error().message == "node 'cache': backend 'db' closes a call cycle");
  }
}

TEST_CASE("a forwarding cache's backend needs a link from the cache", "[graph]") {
  Scenario s = cached_web(cache("cache", 0.8, CacheStrategy::read_through, "db"),
                          {Step{.to = "cache", .op = "read"}});
  s.links.pop_back();  // cache -> db missing
  const auto g = Graph::build(s);
  REQUIRE_FALSE(g.has_value());
  // C++23 std::string::contains: a substring test in one call, replacing
  // `find(...) != std::string::npos`; used where only part of the message is the point.
  REQUIRE(g.error().message.contains("node 'cache': backend 'db' needs a link"));
}

// ---- Che hit ratios resolved from the clients' key distributions (spec §18 entries 1-2) ----

// The Che approximation needs the cache size (the cache's `keys`) and how popular each key is,
// and popularity is a property of the clients, not of the cache: the graph reads it off the
// clients that reach the cache, walking back through the forwarders (load balancers, limiters,
// services) between them.
namespace {
NodeSpec che_cache(std::optional<std::int64_t> keys = 1000) {
  NodeSpec node{.id = "cache",
                .type = NodeType::cache,
                .hit_ratio = HitRatio{.che = true},
                .strategy = CacheStrategy::cache_aside};
  node.keys = keys;
  return node;
}
NodeSpec keyed_client(std::string id, KeySpec keys) {
  auto node = client(std::move(id), Rate{100}, {{"get", 1.0}});
  node.workload->keys = keys;
  return node;
}
// u (the given keys) -> web -> cache (che) and web -> db.
Scenario che_web(NodeSpec cache_node, std::optional<KeySpec> keys) {
  auto s = cached_web(std::move(cache_node), read_then_db_on_miss());
  s.nodes[0].workload->keys = keys;
  return s;
}
}  // namespace

TEST_CASE("a che cache fed by one client resolves its hit ratio from that client's keys",
          "[graph]") {
  const auto s = che_web(che_cache(1000), KeySpec{.zipf = 1.0, .space = 10'000});
  const auto built = Graph::build(s);
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  const auto& g = *built;
  const auto cache_index = *g.index_of("cache");
  REQUIRE(g.hit_ratio(cache_index).has_value());
  REQUIRE_THAT(*g.hit_ratio(cache_index),
               WithinRel(simsys::caching::che_hit_ratio(
                             1000, simsys::caching::Popularity::zipf(1.0, 10'000)),
                         1e-12));
}

TEST_CASE("a che cache behind a load balancer and a limiter still finds the client's keys",
          "[graph]") {
  Scenario s{.name = "che-chain", .version = 1, .duration = 1min};
  s.nodes = {keyed_client("u", KeySpec{.zipf = 1.0, .space = 10'000}),
             load_balancer("lb", LbPolicy::round_robin),
             limiter_of(simsys::model::LimiterAlgorithm::token_bucket, std::nullopt),
             service("web", {{"get", read_then_db_on_miss()}}),
             che_cache(1000),
             service("db")};
  s.links = {LinkSpec{.from = "u", .to = "lb"}, LinkSpec{.from = "lb", .to = "limiter"},
             LinkSpec{.from = "limiter", .to = "web"}, LinkSpec{.from = "web", .to = "cache"},
             LinkSpec{.from = "web", .to = "db"}};
  const auto built = Graph::build(s);
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  const auto& g = *built;
  REQUIRE_THAT(*g.hit_ratio(*g.index_of("cache")),
               WithinRel(simsys::caching::che_hit_ratio(
                             1000, simsys::caching::Popularity::zipf(1.0, 10'000)),
                         1e-12));
}

TEST_CASE("a che cache reached by a client without keys is an error", "[graph]") {
  const auto g = Graph::build(che_web(che_cache(1000), std::nullopt));
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message == "node 'cache': hit_ratio che: client 'u' reaches it without keys");
}

namespace {
// a and b -> web -> cache (che) and web -> db.
Scenario two_clients_one_cache(KeySpec a, KeySpec b) {
  Scenario s{.name = "two-clients", .version = 1, .duration = 1min};
  s.nodes = {keyed_client("a", a), keyed_client("b", b),
             service("web", {{"get", read_then_db_on_miss()}}), che_cache(1000), service("db")};
  s.links = {LinkSpec{.from = "a", .to = "web"}, LinkSpec{.from = "b", .to = "web"},
             LinkSpec{.from = "web", .to = "cache"}, LinkSpec{.from = "web", .to = "db"}};
  return s;
}
}  // namespace

TEST_CASE("two clients with different key distributions reaching one che cache is an error",
          "[graph]") {
  const auto g = Graph::build(two_clients_one_cache(KeySpec{.zipf = 1.0, .space = 10'000},
                                                    KeySpec{.zipf = 0.5, .space = 10'000}));
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message ==
          "node 'cache': hit_ratio che: clients 'a' and 'b' send different key distributions");
}

TEST_CASE("two clients with the same key distribution reaching one che cache agree", "[graph]") {
  const auto built = Graph::build(two_clients_one_cache(KeySpec{.zipf = 1.0, .space = 10'000},
                                                        KeySpec{.zipf = 1.0, .space = 10'000}));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  const auto& g = *built;
  REQUIRE_THAT(*g.hit_ratio(*g.index_of("cache")),
               WithinRel(simsys::caching::che_hit_ratio(
                             1000, simsys::caching::Popularity::zipf(1.0, 10'000)),
                         1e-12));
}

TEST_CASE("a che cache needs a keys capacity", "[graph]") {
  const auto g =
      Graph::build(che_web(che_cache(std::nullopt), KeySpec{.zipf = 1.0, .space = 10'000}));
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message ==
          "node 'cache': hit_ratio che needs keys (the cache's capacity in keys)");
}

// Uniform popularity is the cache's worst case: every key equally likely, so a cache holding a
// quarter of the keys answers a quarter of the reads, whatever the eviction policy.
TEST_CASE("a uniform key space gives the cache's share of the keys", "[graph]") {
  const auto built =
      Graph::build(che_web(che_cache(250), KeySpec{.zipf = std::nullopt, .space = 1000}));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  REQUIRE_THAT(*built->hit_ratio(*built->index_of("cache")), WithinRel(0.25, 1e-6));
}

// Several replicas of one cache: how much they hold together depends on how keys reach them.
// A round-robin balancer sends every key to every replica, so each replica's cache is a full
// copy competing for the same hot keys, with `keys` capacity: the replicas together hit as
// often as one of them. A consistent-hash ring (book ch. 5) sends each key to one replica
// only, so the replicas partition the key space and together hold `keys × replicas` distinct
// keys: for the Che approximation they are one cache of that size.
namespace {
// u (zipf 1 over 100k keys) -> lb (the given policy) -> cache (che, 10k keys, 4 replicas).
Scenario replicated_che_cache(LbPolicy policy) {
  Scenario s{.name = "replicated-cache", .version = 1, .duration = 1min};
  auto cache_node = che_cache(10'000);
  cache_node.replicas = 4;
  s.nodes = {keyed_client("u", KeySpec{.zipf = 1.0, .space = 100'000}), load_balancer("lb", policy),
             std::move(cache_node)};
  s.links = {LinkSpec{.from = "u", .to = "lb"}, LinkSpec{.from = "lb", .to = "cache"}};
  return s;
}
double che_over_100k_zipf1_keys(double capacity) {
  return simsys::caching::che_hit_ratio(capacity, simsys::caching::Popularity::zipf(1.0, 100'000));
}
}  // namespace

// The ring's arcs are uneven (ResolvedNode::imbalance k, 1 + z_M/sqrt(vnodes), M the replicas): the
// hottest replica owns k/M of the keys and the others share the rest. Each replica is an LRU cache
// of `keys` over its own arc, which for Che is one cache of keys / a over the whole space (a the
// arc's share), and it serves a of the requests: h = Σ_j a_j · Che(keys / a_j).
TEST_CASE("a cache partitioned by a consistent-hash ring sums Che over its uneven arcs",
          "[graph]") {
  const auto ring = Graph::build(replicated_che_cache(LbPolicy::consistent_hash));
  INFO((ring ? "" : ring.error().message));
  REQUIRE(ring.has_value());
  const double k = 1.0 + blom_z4 / std::sqrt(150.0);  // 4 replicas, default vnodes: 1.0857
  const double hot = k / 4;
  const double cool = (1 - hot) / 3;
  REQUIRE_THAT(*ring->hit_ratio(*ring->index_of("cache")),
               WithinRel(hot * che_over_100k_zipf1_keys(10'000 / hot) +
                             3 * cool * che_over_100k_zipf1_keys(10'000 / cool),
                         1e-12));

  const auto round_robin = Graph::build(replicated_che_cache(LbPolicy::round_robin));
  INFO((round_robin ? "" : round_robin.error().message));
  REQUIRE(round_robin.has_value());
  REQUIRE_THAT(*round_robin->hit_ratio(*round_robin->index_of("cache")),
               WithinRel(che_over_100k_zipf1_keys(10'000), 1e-12));
}

// ---- Cache then-steps and write_back (spec §18 entry 5) ----

namespace {
// u -> browser_cache -> lb -> svc: the search-autocomplete front. The cache has a fixed hit
// ratio and the given `then` steps; it forwards to lb only through them. Plain links, as in
// the scenario file.
Scenario browser_front(NodeSpec browser_cache) {
  Scenario s{.name = "browser", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{100}, {{"suggest", 1.0}}), std::move(browser_cache),
             load_balancer("lb", LbPolicy::round_robin), service("svc")};
  s.links = {LinkSpec{.from = "u", .to = "browser_cache", .dir = Direction::inout},
             LinkSpec{.from = "browser_cache", .to = "lb", .dir = Direction::inout},
             LinkSpec{.from = "lb", .to = "svc", .dir = Direction::inout}};
  return s;
}
}  // namespace

// Search autocomplete (ch. 13): the browser caches suggestions, and only what it misses goes on
// to the load balancer. The cache's `then: [{to: lb, op: suggest, on: miss}]` is a demand edge
// like a service's step, and the share of reads it carries is the miss fraction 1 - h.
TEST_CASE("a cache's then step on miss gets a demand edge carrying the miss fraction", "[graph]") {
  auto browser_cache = cache("browser_cache", 0.4, CacheStrategy::cache_aside);
  browser_cache.then = {Step{.to = "lb", .op = "suggest", .on = StepCondition::miss}};
  const auto built = Graph::build(browser_front(std::move(browser_cache)));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  const auto& g = *built;
  const auto from = *g.index_of("browser_cache");
  const auto to = *g.index_of("lb");
  const auto link = g.demand_link(from, to);
  REQUIRE(link.has_value());
  REQUIRE(std::ranges::contains(g.edges(), DemandEdge{.from = from, .to = to, .link = *link}));
  const auto& step = g.nodes()[from].spec.then.front();
  REQUIRE_THAT(g.then_fraction(from, step, "suggest"), WithinRel(0.6, 1e-12));
}

// A read_through cache already fetches the miss itself, from its backend. A then step on miss
// would fetch the same miss a second time, so the two together are refused.
TEST_CASE("a then step on miss and a read_through backend both fetching the miss is an error",
          "[graph]") {
  auto c = cache("c", 0.5, CacheStrategy::read_through, "db");
  c.then = {Step{.to = "lb", .op = "read", .on = StepCondition::miss}};
  Scenario s{.name = "double-fetch", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{100}, {{"read", 1.0}}), std::move(c), service("db"),
             load_balancer("lb", LbPolicy::round_robin), service("svc")};
  s.links = {LinkSpec{.from = "u", .to = "c", .dir = Direction::inout},
             LinkSpec{.from = "c", .to = "db", .dir = Direction::inout},
             LinkSpec{.from = "c", .to = "lb", .dir = Direction::inout},
             LinkSpec{.from = "lb", .to = "svc", .dir = Direction::inout}};
  const auto built = Graph::build(s);
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "node 'c': a then step on miss and a read_through backend both fetch the miss");
}

// The same double fetch with the roles swapped: the cache forwards its own misses (`then` on
// miss), so a service that also fetches each miss after asking it (`on: miss` after the cache
// step) would send every miss to the store twice. Whatever the strategy: under the write
// strategies a read miss is still the application's to fetch, as under cache_aside.
TEST_CASE("a service miss step after a cache whose own then step fetches the miss is an error",
          "[graph]") {
  for (const auto strategy : {CacheStrategy::cache_aside, CacheStrategy::write_through,
                              CacheStrategy::write_around, CacheStrategy::write_back}) {
    CAPTURE(static_cast<int>(strategy));
    auto c = cache("c", 0.75, strategy, strategy == CacheStrategy::cache_aside ? "" : "db");
    c.then = {Step{.to = "db", .op = "read", .on = StepCondition::miss}};
    auto s = cached_web(std::move(c), {Step{.to = "c", .op = "read"},
                                       Step{.to = "db", .op = "read", .on = StepCondition::miss}});
    if (strategy == CacheStrategy::cache_aside) {
      s.links.push_back(LinkSpec{.from = "c", .to = "db", .dir = Direction::inout});
    }
    const auto built = Graph::build(s);
    REQUIRE_FALSE(built.has_value());
    REQUIRE(built.error().message ==
            "node 'web': an 'on: miss' step cannot follow cache 'c', whose own then step on miss "
            "already fetches the miss");
  }
}

// All the parallel steps of a step list form one group, and the steps after it are sent only the
// requests the whole group answered. [a ∥, s, b ∥] leaves it unclear whether s runs before b or
// after the group, so the group must be listed where it runs, its members side by side. The same
// rule holds for `then` steps.
TEST_CASE("parallel steps that are not adjacent are refused, in an op and in then steps",
          "[graph]") {
  Scenario s{.name = "group", .version = 1, .duration = 1min};
  s.nodes = {
      NodeSpec{.id = "users",
               .type = NodeType::client,
               .workload = Workload{.curve = {.base = Rate{100}}, .op_shares = {{"get", 1.0}}}},
      NodeSpec{.id = "web",
               .type = NodeType::service,
               .ops = {{"get",
                        {Step{.to = "a", .op = "get", .mode = StepMode::parallel},
                         Step{.to = "s", .op = "get"},
                         Step{.to = "b", .op = "get", .mode = StepMode::parallel}}}}},
      NodeSpec{.id = "a", .type = NodeType::service},
      NodeSpec{.id = "s", .type = NodeType::service},
      NodeSpec{.id = "b", .type = NodeType::service}};
  s.links = {LinkSpec{.from = "users", .to = "web"}};
  for (const char* target : {"a", "s", "b"})
    s.links.push_back(LinkSpec{.from = "web", .to = target});
  const auto built = Graph::build(s);
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "node 'web': the parallel steps of op 'get' must be adjacent (they form one group)");

  auto c = cache("c", 0.4, CacheStrategy::cache_aside);
  c.then = {Step{.to = "a", .op = "get", .mode = StepMode::parallel}, Step{.to = "s", .op = "get"},
            Step{.to = "b", .op = "get", .mode = StepMode::parallel}};
  s.nodes[1].ops = {{"get", {Step{.to = "c", .op = "get"}}}};
  s.nodes.push_back(std::move(c));
  s.links = {LinkSpec{.from = "users", .to = "web"}, LinkSpec{.from = "web", .to = "c"}};
  for (const char* target : {"a", "s", "b"}) s.links.push_back(LinkSpec{.from = "c", .to = target});
  const auto then_built = Graph::build(s);
  REQUIRE_FALSE(then_built.has_value());
  REQUIRE(then_built.error().message ==
          "node 'c': the parallel then steps must be adjacent (they form one group)");

  // Side by side, before or after the sequential steps, the same steps build.
  s.nodes.back().then = {Step{.to = "s", .op = "get"},
                         Step{.to = "a", .op = "get", .mode = StepMode::parallel},
                         Step{.to = "b", .op = "get", .mode = StepMode::parallel}};
  REQUIRE(Graph::build(s).has_value());
}

// A miss waits for what its miss steps fetch: the cache answers a miss only after the next
// tier has. A parallel miss step would be timed beside the lookup instead of after it, the
// reason a service's miss step must be sequential too, so it is refused.
TEST_CASE("a parallel miss then step is refused", "[graph]") {
  auto browser_cache = cache("c", 0.4, CacheStrategy::cache_aside);
  browser_cache.id = "browser_cache";
  browser_cache.then = {
      Step{.to = "lb", .op = "suggest", .on = StepCondition::miss, .mode = StepMode::parallel}};
  const auto built = Graph::build(browser_front(std::move(browser_cache)));
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "node 'browser_cache': a then step on miss must be sequential (a miss waits for it)");
}

// A cache whose strategy writes to its store already sends every write there (its backend hop).
// A `then` step that sends the same op to the same node would send each write a second time, and
// under write_back it would also be mistaken for the asynchronous flush. Refused; a then step to
// the store with another op (an audit record, say) is a different call and builds.
TEST_CASE("a then step that duplicates the cache's backend hop is refused", "[graph]") {
  const std::vector<Step> to_cache{Step{.to = "c", .op = "write"}};
  const auto writing_strategies = {CacheStrategy::write_through, CacheStrategy::write_back};
  SECTION("the same op to the backend") {
    for (const auto strategy : writing_strategies) {
      CAPTURE(static_cast<int>(strategy));
      auto c = cache("c", 0.5, strategy, "db");
      c.then = {Step{.to = "db", .op = "write"}};
      const auto built = Graph::build(cached_web(std::move(c), to_cache));
      REQUIRE_FALSE(built.has_value());
      REQUIRE(built.error().message == "node 'c': then step to 'db' duplicates its backend hop");
    }
  }
  SECTION("another op to the backend is a call of its own, waited for") {
    for (const auto strategy : writing_strategies) {
      CAPTURE(static_cast<int>(strategy));
      auto c = cache("c", 0.5, strategy, "db");
      c.then = {Step{.to = "db", .op = "audit"}};
      const auto built = Graph::build(cached_web(std::move(c), to_cache));
      INFO((built ? "" : built.error().message));
      REQUIRE(built.has_value());
      REQUIRE_FALSE(
          built->is_async_hop(*built->index_of("c"), "write", *built->index_of("db"), "audit"));
    }
  }
}

// A then step on miss carries the misses of the READS only (Graph::then_fraction). A cache that
// only ever receives writes therefore sends nothing along it, and the op it would send arrives
// nowhere: a load balancer behind it whose links do not carry that op is no error.
TEST_CASE("a miss then step on a cache that only receives writes is not a false refusal",
          "[graph]") {
  auto c = cache("c", 0.5, CacheStrategy::cache_aside);
  c.then = {Step{.to = "lb", .op = "other", .on = StepCondition::miss}};
  Scenario s{.name = "writes-only", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{100}, {{"write", 1.0}}), std::move(c),
             load_balancer("lb", LbPolicy::round_robin), service("s")};
  s.links = {LinkSpec{.from = "u", .to = "c", .dir = Direction::inout},
             LinkSpec{.from = "c", .to = "lb", .dir = Direction::inout},
             LinkSpec{.from = "lb", .to = "s", .dir = Direction::inout, .ops = {"write"}}};
  const auto built = Graph::build(s);
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  REQUIRE(built->arriving_ops(*built->index_of("lb")).empty());
}

// Write-back (general caching practice): the cache acknowledges a write itself and flushes it
// to the store later. Nobody waits for the flush, so the hop from the cache to the backend is
// asynchronous: it forwards flow but adds no latency to the caller.
TEST_CASE("a write_back cache needs a backend and a link, and its flush hop is asynchronous",
          "[graph]") {
  const std::vector<Step> to_cache{Step{.to = "cache", .op = "write"}};
  SECTION("with a backend and a link it builds and the flush hop is asynchronous") {
    const auto built =
        Graph::build(cached_web(cache("cache", 0.8, CacheStrategy::write_back, "db"), to_cache));
    INFO((built ? "" : built.error().message));
    REQUIRE(built.has_value());
    const auto c = *built->index_of("cache");
    const auto db = *built->index_of("db");
    REQUIRE(built->is_async_hop(c, "write", db, "write"));
    REQUIRE(std::ranges::contains(
        built->async_hops(),
        simsys::graph::AsyncHop{.from = c, .op = "write", .to = db, .target_op = "write"}));
  }
  SECTION("without a backend it is refused") {
    const auto built =
        Graph::build(cached_web(cache("cache", 0.8, CacheStrategy::write_back), to_cache));
    REQUIRE_FALSE(built.has_value());
    REQUIRE(built.error().message == "node 'cache': strategy write_back needs a backend");
  }
  SECTION("with a backend but no link from the cache it is refused") {
    auto s = cached_web(cache("cache", 0.8, CacheStrategy::write_back, "db"), to_cache);
    s.links.pop_back();  // cache -> db missing
    const auto built = Graph::build(s);
    REQUIRE_FALSE(built.has_value());
    REQUIRE(built.error().message.contains("node 'cache': backend 'db' needs a link"));
  }
}

// ---- Which keys reach a che cache: the op-aware walk (spec §18 entry 1, revised) ----
//
// A client's keys reach a cache only along the calls its ops make: an op that never calls the
// cache brings none of its keys there, even when the same service sits in front of both.

namespace {
// readers (read, zipf 1 over 1M) and writers (write, the given keys) -> web;
// web.read = [cache.read, db.read on miss], web.write = [db.write]; cache: che, 100k keys.
Scenario readers_and_writers(std::optional<KeySpec> reader_keys,
                             std::optional<KeySpec> writer_keys) {
  Scenario s{.name = "ops", .version = 1, .duration = 1min};
  auto readers = client("readers", Rate{10'000}, {{"read", 1.0}});
  readers.workload->keys = reader_keys;
  auto writers = client("writers", Rate{1000}, {{"write", 1.0}});
  writers.workload->keys = writer_keys;
  s.nodes = {std::move(readers), std::move(writers),
             service("web", {{"read",
                              {Step{.to = "cache", .op = "read"},
                               Step{.to = "db", .op = "read", .on = StepCondition::miss}}},
                             {"write", {Step{.to = "db", .op = "write"}}}}),
             che_cache(100'000), service("db")};
  s.links = {LinkSpec{.from = "readers", .to = "web"}, LinkSpec{.from = "writers", .to = "web"},
             LinkSpec{.from = "web", .to = "cache"}, LinkSpec{.from = "web", .to = "db"}};
  return s;
}
}  // namespace

TEST_CASE("a client whose ops never call the cache does not bring its keys to it", "[graph]") {
  const auto built = Graph::build(readers_and_writers(
      KeySpec{.zipf = 1.0, .space = 1e6}, KeySpec{.zipf = std::nullopt, .space = 50e6}));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  const auto cache_index = *built->index_of("cache");
  REQUIRE_THAT(*built->hit_ratio(cache_index),
               WithinRel(simsys::caching::che_hit_ratio(
                             100'000, simsys::caching::Popularity::zipf(1.0, 1e6)),
                         1e-12));
  const auto& resolved = *built->nodes()[cache_index].hit_ratio;
  REQUIRE(resolved.key_clients == std::vector<std::size_t>{*built->index_of("readers")});
}

TEST_CASE("a client without keys whose ops never call the cache is no error", "[graph]") {
  const auto built =
      Graph::build(readers_and_writers(KeySpec{.zipf = 1.0, .space = 1e6}, std::nullopt));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
}

TEST_CASE("a client without keys whose ops call the cache is an error naming it", "[graph]") {
  const auto built =
      Graph::build(readers_and_writers(std::nullopt, KeySpec{.zipf = std::nullopt, .space = 50e6}));
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "node 'cache': hit_ratio che: client 'readers' reaches it without keys");
}

TEST_CASE("a che cache no client's demand reaches is an error", "[graph]") {
  Scenario s{.name = "orphan", .version = 1, .duration = 1min};
  s.nodes = {keyed_client("u", KeySpec{.zipf = std::nullopt, .space = 10}), service("s"),
             che_cache(3)};
  s.links = {LinkSpec{.from = "u", .to = "s"}};
  const auto built = Graph::build(s);
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message == "node 'cache': hit_ratio che: no client's demand reaches it");
}

// A rate limiter asks its counter store "how many requests has THIS user made?": the store is keyed
// per user (or per rule), not by the keys the client's requests are about, so the counter op brings
// no client keys to it. A che store that nothing else reaches has no key distribution to solve
// over, and the message says why, rather than "no client's demand reaches it": the clients' demand
// does, through the limiter. (`counter` is in the store's write_ops: an INCR is a write, see "a
// limiter's counter that its store counts as a read is refused" below.)
TEST_CASE("a limiter's counter requests bring no client keys to its store", "[graph]") {
  Scenario s{.name = "lim", .version = 1, .duration = 1min};
  auto limiter = limiter_of(simsys::model::LimiterAlgorithm::token_bucket, std::nullopt);
  limiter.backend = "cache";
  auto store = che_cache(10'000);
  store.write_ops = std::vector<std::string>{"counter"};
  s.nodes = {keyed_client("u", KeySpec{.zipf = 1.0, .space = 100e6}), std::move(limiter),
             std::move(store), service("web")};
  s.links = {LinkSpec{.from = "u", .to = "limiter"}, LinkSpec{.from = "limiter", .to = "cache"},
             LinkSpec{.from = "limiter", .to = "web"}};
  const auto built = Graph::build(s);
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "node 'cache': hit_ratio che: the counter requests of rate limiter 'limiter' carry no "
          "client keys (a counter is keyed by user or rule), and no client's keys reach it "
          "otherwise; give it hit_ratio: {fixed: ...}");
}

// `zipf: 0` IS the uniform distribution (p_i ∝ i^0 = 1): two clients writing it the two ways
// send the same keys and must agree.
TEST_CASE("zipf 0 and uniform over the same space are the same key distribution", "[graph]") {
  const auto built = Graph::build(two_clients_one_cache(
      KeySpec{.zipf = 0.0, .space = 10'000}, KeySpec{.zipf = std::nullopt, .space = 10'000}));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  REQUIRE_THAT(*built->hit_ratio(*built->index_of("cache")), WithinRel(0.1, 1e-6));
}

// ---- Caches behind caches: filtered popularity (Martina, Garetto & Leonardi 2014) ----
//
// The second cache of a chain sees only the FIRST cache's misses: the hot keys stopped at the
// first cache, so the second one gets the cold tail, where caching works much worse. Its key
// popularity is the clients' filtered by the first cache: p_i e^{-p_i t1}, renormalised.

namespace {
// u (zipf 0.9 over 10M, get) -> l1 -> l2 -> db; both caches read_through, l1 the given hit
// ratio (2M keys), l2 che with `l2_keys`.
Scenario two_caches(HitRatio l1_ratio, std::int64_t l2_keys) {
  Scenario s{.name = "c2c", .version = 1, .duration = 1min};
  NodeSpec l1{.id = "l1",
              .type = NodeType::cache,
              .hit_ratio = l1_ratio,
              .strategy = CacheStrategy::read_through};
  l1.backend = "l2";
  l1.keys = 2'000'000;
  NodeSpec l2{.id = "l2",
              .type = NodeType::cache,
              .hit_ratio = HitRatio{.che = true},
              .strategy = CacheStrategy::read_through};
  l2.backend = "db";
  l2.keys = l2_keys;
  s.nodes = {keyed_client("u", KeySpec{.zipf = 0.9, .space = 10e6}), std::move(l1), std::move(l2),
             service("db")};
  s.links = {LinkSpec{.from = "u", .to = "l1", .dir = Direction::inout},
             LinkSpec{.from = "l1", .to = "l2", .dir = Direction::inout},
             LinkSpec{.from = "l2", .to = "db", .dir = Direction::inout}};
  return s;
}
}  // namespace

TEST_CASE("a che cache behind a che cache sees the first cache's misses", "[graph]") {
  // Dense reference (zipf 0.9 over all 10M keys, see test_caching): l2 at 2M keys hits 0.257,
  // at 8M 0.870; over the raw popularity it would claim 0.747 and 0.963.
  for (const auto& [l2_keys, expected] : {std::pair{std::int64_t{2'000'000}, 0.256607},
                                          std::pair{std::int64_t{8'000'000}, 0.870337}}) {
    const auto built = Graph::build(two_caches(HitRatio{.che = true}, l2_keys));
    INFO((built ? "" : built.error().message));
    REQUIRE(built.has_value());
    REQUIRE_THAT(*built->hit_ratio(*built->index_of("l1")), WithinAbs(0.747110, 1e-4));
    REQUIRE_THAT(*built->hit_ratio(*built->index_of("l2")), WithinAbs(expected, 1e-3));
    const auto& resolved = *built->nodes()[*built->index_of("l2")].hit_ratio;
    REQUIRE(resolved.filtered_by == built->index_of("l1"));
  }
}

// A fixed ratio says nothing about WHICH keys hit, so there is no characteristic time to filter
// by: the downstream che cache uses the clients' popularity as it is, and says so in explain.
TEST_CASE("a che cache behind a fixed-ratio cache uses the unfiltered popularity", "[graph]") {
  const auto built = Graph::build(two_caches(HitRatio{.fixed = 0.5}, 2'000'000));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  REQUIRE_THAT(
      *built->hit_ratio(*built->index_of("l2")),
      WithinRel(simsys::caching::che_hit_ratio(2e6, simsys::caching::Popularity::zipf(0.9, 10e6)),
                1e-12));
  const auto& resolved = *built->nodes()[*built->index_of("l2")].hit_ratio;
  REQUIRE_FALSE(resolved.filtered_by.has_value());
  REQUIRE(resolved.unfiltered_through == built->index_of("l1"));
}

// A cache's `then` step on miss carries its misses too (search autocomplete's browser cache).
TEST_CASE("a cache's then step on miss filters the keys it passes on", "[graph]") {
  Scenario s{.name = "then", .version = 1, .duration = 1min};
  NodeSpec front{.id = "front",
                 .type = NodeType::cache,
                 .hit_ratio = HitRatio{.che = true},
                 .strategy = CacheStrategy::cache_aside};
  front.keys = 1000;
  front.then = {Step{.to = "cache", .op = "get", .on = StepCondition::miss}};
  s.nodes = {keyed_client("u", KeySpec{.zipf = 1.0, .space = 100'000}), std::move(front),
             che_cache(1000)};
  s.links = {LinkSpec{.from = "u", .to = "front"}, LinkSpec{.from = "front", .to = "cache"}};
  const auto built = Graph::build(s);
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  const auto clients = simsys::caching::Popularity::zipf(1.0, 100'000);
  const auto misses = clients.filtered_by(simsys::caching::che_characteristic_time(1000, clients));
  REQUIRE_THAT(*built->hit_ratio(*built->index_of("cache")),
               WithinRel(simsys::caching::che_hit_ratio(1000, misses), 1e-12));
}

// One popularity per cache: reads that arrive as l1's misses and reads that skipped l1 have
// different popularities, and mixing them by rate is in the Phase 3 backlog.
TEST_CASE("a che cache receiving both filtered and unfiltered reads is an error", "[graph]") {
  auto s = two_caches(HitRatio{.che = true}, 2'000'000);
  s.nodes[0].workload->op_shares = {{"get", 0.5}, {"peek", 0.5}};
  s.links[0].ops = {"get"};
  s.links.push_back(LinkSpec{.from = "u", .to = "l2", .dir = Direction::inout, .ops = {"peek"}});
  const auto built = Graph::build(s);
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "node 'l2': hit_ratio che: it receives both the misses of cache 'l1' and requests no "
          "cache filtered");
}

// ---- Ring-partitioned caches: uneven arcs (spec §18 entry 2, revised) ----

namespace {
// u (zipf 0.9 over 10M) -> ring (consistent_hash, `vnodes`) -> c (4 replicas, 2M keys each,
// che, read_through) -> db: the consistent-hashing lab.
Scenario ring_lab(int vnodes) {
  Scenario s{.name = "ring", .version = 1, .duration = 1min};
  auto ring = load_balancer("ring", LbPolicy::consistent_hash);
  ring.vnodes = vnodes;
  NodeSpec c{.id = "c",
             .type = NodeType::cache,
             .replicas = 4,
             .hit_ratio = HitRatio{.che = true},
             .strategy = CacheStrategy::read_through};
  c.backend = "db";
  c.keys = 2'000'000;
  s.nodes = {keyed_client("u", KeySpec{.zipf = 0.9, .space = 10e6}), std::move(ring), std::move(c),
             service("db")};
  s.links = {LinkSpec{.from = "u", .to = "ring", .dir = Direction::inout},
             LinkSpec{.from = "ring", .to = "c", .dir = Direction::inout},
             LinkSpec{.from = "c", .to = "db", .dir = Direction::inout}};
  return s;
}
}  // namespace

TEST_CASE("one vnode per server over four servers: the hot arc halves the ring's hit ratio gain",
          "[graph]") {
  // vnodes 1: k = 1 + z_4 = 2.0491, so the hot replica owns k/4 = 0.5123 of the keys (Che of
  // 2M / 0.5123 = 3.904M keys: 0.8445) and each cool one (1 - 0.5123)/3 = 0.1626 (12.3M > 10M
  // keys: every key fits, 1). h = 0.5123 × 0.8445 + 0.4877 × 1 = 0.9203, below the 0.963 an even
  // split (one 8M-key cache) would give.
  const auto built = Graph::build(ring_lab(1));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  const auto zipf = simsys::caching::Popularity::zipf(0.9, 10e6);
  const double hot = (1.0 + blom_z4) / 4.0;
  REQUIRE_THAT(
      *built->hit_ratio(*built->index_of("c")),
      WithinRel(hot * simsys::caching::che_hit_ratio(2e6 / hot, zipf) + (1.0 - hot), 1e-12));
  REQUIRE_THAT(*built->hit_ratio(*built->index_of("c")), WithinAbs(0.920318, 1e-4));
}

TEST_CASE("a ring-partitioned cache that is also sent to directly is an error", "[graph]") {
  auto s = ring_lab(150);
  s.nodes[0].workload->op_shares = {{"get", 0.5}, {"peek", 0.5}};
  s.nodes.push_back(service("web", {{"peek", {Step{.to = "c", .op = "get"}}}}));
  s.links[0].ops = {"get"};
  s.links.push_back(LinkSpec{.from = "u", .to = "web", .dir = Direction::inout, .ops = {"peek"}});
  s.links.push_back(LinkSpec{.from = "web", .to = "c", .dir = Direction::inout});
  const auto built = Graph::build(s);
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "node 'c': keys are partitioned by ring 'ring' but 'web' also sends to it directly");
}

// ---- SQL database (spec §18 entry 6) ----

namespace {
// u -> db, or u -> ring -> db through a consistent-hash balancer of one vnode per server: a SQL
// database of 3 copies (one primary, two read replicas) of 5k/s and 10 ms each.
Scenario database_behind_ring(bool ring) {
  Scenario s{.name = "db", .version = 1, .duration = 1min};
  NodeSpec db{
      .id = "db", .type = NodeType::sql_db, .replicas = 3, .capacity = Rate{5000}, .latency = 10ms};
  s.nodes = {NodeSpec{.id = "u",
                      .type = NodeType::client,
                      .workload = Workload{.curve = {.base = Rate{1000}},
                                           .op_shares = {{"select", 0.9}, {"insert", 0.1}}}},
             std::move(db)};
  if (!ring) {
    s.links = {LinkSpec{.from = "u", .to = "db"}};
    return s;
  }
  s.nodes.push_back(NodeSpec{.id = "ring",
                             .type = NodeType::load_balancer,
                             .policy = LbPolicy::consistent_hash,
                             .vnodes = 1});
  s.links = {LinkSpec{.from = "u", .to = "ring"}, LinkSpec{.from = "ring", .to = "db"}};
  return s;
}
}  // namespace

// The database driver, not a balancer, sends writes to the primary and reads to a replica, and a
// sharding key spreads the shards evenly: a hash ring in front leaves the copies even (imbalance
// 1), so explain and the engine agree that no copy is hot.
TEST_CASE("a hash ring imposes no hot copy on a database", "[graph]") {
  const auto g = Graph::build(database_behind_ring(true));
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  REQUIRE(g->nodes()[*g->index_of("db")].imbalance == 1.0);
}

// Two balancers whose policies give different replica models may both front a database: neither
// model applies to it (its driver picks the copy), so there is no conflict to refuse. A hash ring
// (imbalanced) and a least_connections balancer (pooled) both linked to `db` build, and `db` keeps
// even, unpooled copies.
TEST_CASE("balancers of different replica models may both front a database", "[graph]") {
  auto s = database_behind_ring(true);
  s.nodes.push_back(NodeSpec{
      .id = "least", .type = NodeType::load_balancer, .policy = LbPolicy::least_connections});
  s.links.push_back(LinkSpec{.from = "u", .to = "least"});
  s.links.push_back(LinkSpec{.from = "least", .to = "db"});
  const auto g = Graph::build(s);
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  const auto& db = g->nodes()[*g->index_of("db")];
  REQUIRE(db.imbalance == 1.0);
  REQUIRE_FALSE(db.pooled);
}

// A database is a leaf: it answers every query itself (behaviours::SqlDbBehaviour), so a link
// that would carry demand out of it would carry nothing. Graph::build refuses one in a scenario
// built in code with the loader's message, whichever end writes it.
TEST_CASE("a link that carries demand out of a database is refused", "[graph]") {
  auto s = database_behind_ring(false);
  s.nodes.push_back(NodeSpec{.id = "audit", .type = NodeType::service});
  SECTION("written from the database") {
    s.links.push_back(LinkSpec{.from = "db", .to = "audit"});
  }
  SECTION("written from the other end with dir: in") {
    s.links.push_back(LinkSpec{.from = "audit", .to = "db", .dir = Direction::in});
  }
  const auto g = Graph::build(s);
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message == "node 'db': a database calls nobody");
}

// Little's law, concurrency in service = throughput × service time, read backwards: a copy with
// `pool` connections has at most `pool` queries in service, so it serves at most pool / service
// time. 5k/s × 10 ms implies 50 in service; `pool: 4` allows 4, so 4 / 10 ms = 400/s with c = 4. A
// pool of 100 binds nothing: 5k/s and the implied c = 50 stay.
TEST_CASE("a database's pool bounds its throughput by Little's law and never raises it",
          "[graph]") {
  auto s = database_behind_ring(false);
  SECTION("pool 4 of an implied 50") {
    s.nodes[1].pool = 4;
    const auto g = *Graph::build(s);
    const auto& capacity = g.nodes()[1].capacity;
    REQUIRE_THAT(capacity.service_rate.per_second, WithinRel(400.0, 1e-12));
    REQUIRE(capacity.concurrency == 4);
    REQUIRE_THAT(capacity.service_time_s, WithinRel(0.010, 1e-12));
  }
  SECTION("pool 100 of an implied 50") {
    s.nodes[1].pool = 100;
    const auto g = *Graph::build(s);
    const auto& capacity = g.nodes()[1].capacity;
    REQUIRE_THAT(capacity.service_rate.per_second, WithinRel(5000.0, 1e-12));
    REQUIRE(capacity.concurrency == 50);
  }
}

// A database built in code obeys the loader's rule on `pool` and `advanced.concurrency` with its
// message: both set c, and only the pool also caps the rate.
TEST_CASE("a database's pool and advanced.concurrency are refused together in code", "[graph]") {
  auto s = database_behind_ring(false);
  s.nodes[1].pool = 8;
  s.nodes[1].advanced.concurrency = 8;
  const auto g = Graph::build(s);
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message ==
          "node 'db': pool sets c and caps the rate by Little's law; drop advanced.concurrency");
}

// ---- Request-based CDN (spec §18 entry 10) ----
//
// A CDN (book ch. 1: a cache at the network's edge; ch. 13's browser cache is the same idea one
// hop closer to the user) is a read-only cache in front of its `origin`. It answers the hit
// fraction h itself and sends the misses, as the SAME op, to the origin: a demand edge
// cdn -> origin carrying 1 - h of each read op, exactly as a read_through cache's backend hop.
namespace {
// A CDN with the given hit ratio (a fixed one or che) and origin; `origin` is assigned after the
// designated initializer, so these tests do not depend on where the field is declared.
NodeSpec cdn_node(std::optional<HitRatio> hit, std::optional<std::string> origin = "origin") {
  NodeSpec node{.id = "cdn", .type = NodeType::cdn, .hit_ratio = hit};
  node.origin = std::move(origin);
  return node;
}
// u (100/s, the given ops) -> cdn -> origin (a service), both links of default latency.
// Node indices: u 0, cdn 1, origin 2.
Scenario cdn_front(NodeSpec cdn,
                   std::vector<std::pair<std::string, double>> ops = {{"read", 1.0}}) {
  Scenario s{.name = "cdn-front", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{100}, std::move(ops)), std::move(cdn), service("origin")};
  s.links = {LinkSpec{.from = "u", .to = "cdn", .dir = Direction::inout},
             LinkSpec{.from = "cdn", .to = "origin", .dir = Direction::inout}};
  return s;
}
}  // namespace

TEST_CASE("a cdn with a fixed hit ratio sends the misses to its origin as the same op", "[graph]") {
  const auto built = Graph::build(cdn_front(cdn_node(HitRatio{.fixed = 0.95})));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  const auto& g = *built;
  const auto cdn = *g.index_of("cdn");
  const auto origin = *g.index_of("origin");
  REQUIRE_THAT(*g.hit_ratio(cdn), WithinRel(0.95, 1e-12));
  // The demand edge cdn -> origin exists, over the link the scenario draws between them.
  const auto link = g.demand_link(cdn, origin);
  REQUIRE(link.has_value());
  REQUIRE(std::ranges::contains(g.edges(), DemandEdge{.from = cdn, .to = origin, .link = *link}));
  // The edge carries the miss share of the read op: 1 - 0.95 = 0.05 of every `read` the cdn is
  // asked, the share Graph::backend_fraction gives a read_through cache's backend hop too.
  REQUIRE_THAT(g.backend_fraction(cdn, "read"), WithinRel(0.05, 1e-12));
  // Same op name on both sides: the origin is asked for `read`, not for a renamed op.
  // C++20 std::ranges::equal: element-wise equality of two ranges (here a span and a vector),
  // replacing `std::equal(a.begin(), a.end(), b.begin(), b.end())`; the test reads as "the ops
  // that arrive are exactly {read}".
  REQUIRE(std::ranges::equal(g.arriving_ops(cdn), std::vector<std::string>{"read"}));
  REQUIRE(std::ranges::equal(g.arriving_ops(origin), std::vector<std::string>{"read"}));
}

// "Missing every time" is the safe side for a capacity check on paper: a CDN the scenario gave
// no hit ratio still runs, and its origin is planned for the whole read load (h = 0).
TEST_CASE("a cdn without a hit ratio misses every read and still builds", "[graph]") {
  const auto built = Graph::build(cdn_front(cdn_node(std::nullopt)));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  const auto cdn = *built->index_of("cdn");
  REQUIRE_FALSE(built->hit_ratio(cdn).has_value());
  REQUIRE_THAT(built->backend_fraction(cdn, "read"), WithinRel(1.0, 1e-12));
}

// A CDN serves static files from the edge and has no write path: there is nothing in its model that
// a write could update, so a write that reaches it is a mistake in the scenario. "A write" is
// whatever model::write_ops_of says for the node: a CDN's default is the op `write`, and a
// scenario's `write_ops` replaces it, exactly as for a cache. The message names the op and a node
// that sends it, so a student finds the arrow to remove.
TEST_CASE("a write op reaching a cdn is refused", "[graph]") {
  SECTION("the default write op, sent by a client") {
    const auto g =
        Graph::build(cdn_front(cdn_node(HitRatio{.fixed = 0.95}), {{"read", 0.9}, {"write", 0.1}}));
    REQUIRE_FALSE(g.has_value());
    REQUIRE(g.error().message == "node 'cdn': a cdn does not take writes ('write' from 'u')");
  }
  SECTION("an op the scenario lists in write_ops, sent by a client") {
    auto cdn = cdn_node(HitRatio{.fixed = 0.95});
    cdn.write_ops = std::vector<std::string>{"purge"};
    const auto g = Graph::build(cdn_front(std::move(cdn), {{"read", 0.9}, {"purge", 0.1}}));
    REQUIRE_FALSE(g.has_value());
    REQUIRE(g.error().message == "node 'cdn': a cdn does not take writes ('purge' from 'u')");
  }
  SECTION("an op that is not in write_ops is a read, even if it is called write") {
    auto cdn = cdn_node(HitRatio{.fixed = 0.95});
    cdn.write_ops = std::vector<std::string>{"purge"};
    const auto g = Graph::build(cdn_front(std::move(cdn), {{"read", 0.9}, {"write", 0.1}}));
    INFO((g ? "" : g.error().message));
    REQUIRE(g.has_value());
  }
  SECTION("sent by a service step") {
    Scenario s{.name = "cdn-behind-web", .version = 1, .duration = 1min};
    s.nodes = {client("u", Rate{100}, {{"put", 1.0}}),
               service("web", {{"put", {Step{.to = "cdn", .op = "write"}}}}),
               cdn_node(HitRatio{.fixed = 0.95}), service("origin")};
    s.links = {LinkSpec{.from = "u", .to = "web", .dir = Direction::inout},
               LinkSpec{.from = "web", .to = "cdn", .dir = Direction::inout},
               LinkSpec{.from = "cdn", .to = "origin", .dir = Direction::inout}};
    const auto g = Graph::build(std::move(s));
    REQUIRE_FALSE(g.has_value());
    REQUIRE(g.error().message == "node 'cdn': a cdn does not take writes ('write' from 'web')");
  }
}

// The loader refuses a cdn without an origin (config tests); a scenario built in code skips the
// loader, so the graph repeats the rule, as it does for a limiter without a limit.
TEST_CASE("a cdn built in code without an origin is refused", "[graph]") {
  const auto g = Graph::build(cdn_front(cdn_node(HitRatio{.fixed = 0.95}, std::nullopt)));
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message == "node 'cdn': a cdn needs an origin");
}

// Che for a cdn resolves as for a cache: the size in keys (`keys` on the node, in objects) and the
// key distribution of the clients that reach it, found by walking the calls back upstream.
TEST_CASE("a che cdn resolves its hit ratio from the keys of the clients upstream", "[graph]") {
  auto cdn = cdn_node(HitRatio{.che = true});
  cdn.keys = 1000;
  auto s = cdn_front(std::move(cdn));
  s.nodes[0].workload->keys = KeySpec{.zipf = 1.0, .space = 10'000};
  const auto built = Graph::build(std::move(s));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  const auto h = built->hit_ratio(*built->index_of("cdn"));
  REQUIRE(h.has_value());
  // The same number a che cache of 1000 keys gets for Zipf(1) over 10k keys (see the che cache
  // tests above): the Che approximation, not a value stated by the scenario.
  REQUIRE_THAT(*h, WithinRel(simsys::caching::che_hit_ratio(
                                 1000, simsys::caching::Popularity::zipf(1.0, 10'000)),
                             1e-12));
  // And the miss share it sends the origin is the complement.
  REQUIRE_THAT(built->backend_fraction(*built->index_of("cdn"), "read"),
               WithinRel(1.0 - *h, 1e-12));
}

// The origin is a server the CDN calls on a miss, reached over a link from the CDN: a scenario
// built in code gets the loader's rules (config tests) from the graph, with the same words.
TEST_CASE("a cdn built in code needs an origin it can call", "[graph]") {
  SECTION("not a client") {
    const auto g = Graph::build(cdn_front(cdn_node(HitRatio{.fixed = 0.95}, "u")));
    REQUIRE_FALSE(g.has_value());
    REQUIRE(g.error().message == "node 'cdn': origin 'u' is a client; clients only send");
  }
  SECTION("a node that exists") {
    const auto g = Graph::build(cdn_front(cdn_node(HitRatio{.fixed = 0.95}, "ghost")));
    REQUIRE_FALSE(g.has_value());
    REQUIRE(g.error().message == "node 'cdn': origin refers to unknown node 'ghost'");
  }
  SECTION("over a link from the cdn") {
    auto s = cdn_front(cdn_node(HitRatio{.fixed = 0.95}));
    s.links.pop_back();  // cdn -> origin
    const auto g = Graph::build(std::move(s));
    REQUIRE_FALSE(g.has_value());
    REQUIRE(g.error().message == "node 'cdn': origin 'origin' needs a link from the cdn");
  }
}

// A CDN fetches its own misses, so a `then` step (which the loader never gives a CDN) would fetch
// them twice or add a hop the edge-cache model has no place for: refused in code too.
TEST_CASE("a cdn built in code with then steps is refused", "[graph]") {
  auto cdn = cdn_node(HitRatio{.fixed = 0.95});
  cdn.then = {Step{.to = "origin", .op = "read", .on = StepCondition::miss}};
  const auto g = Graph::build(cdn_front(std::move(cdn)));
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message ==
          "node 'cdn': a cdn has no then steps (it fetches its misses from its origin)");
}

// A CDN that names itself as its origin would fetch its own misses from itself: a miss that can
// never be answered. The loader refuses it (config tests), and so does the graph for a scenario
// built in code, before it looks for the link.
TEST_CASE("a cdn built in code that is its own origin is refused", "[graph]") {
  const auto g = Graph::build(cdn_front(cdn_node(HitRatio{.fixed = 0.95}, "cdn")));
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message == "node 'cdn': a cdn cannot be its own origin");
}

// A CDN is read_through towards its origin by definition; the cache's `strategy` and `backend` keys
// are not its keys (the loader has no such key on a cdn). Written in code they would contradict the
// model: a write_back strategy, for one, would excuse the origin hop from the call-cycle check,
// although a miss always waits for the origin.
TEST_CASE("a cdn built in code with a cache strategy or a backend is refused", "[graph]") {
  SECTION("a strategy other than the default") {
    auto cdn = cdn_node(HitRatio{.fixed = 0.95});
    cdn.strategy = CacheStrategy::write_back;
    const auto g = Graph::build(cdn_front(std::move(cdn)));
    REQUIRE_FALSE(g.has_value());
    REQUIRE(g.error().message ==
            "node 'cdn': a cdn has no strategy (it reads through to its origin)");
  }
  SECTION("a backend") {
    auto cdn = cdn_node(HitRatio{.fixed = 0.95});
    cdn.backend = "origin";
    const auto g = Graph::build(cdn_front(std::move(cdn)));
    REQUIRE_FALSE(g.has_value());
    REQUIRE(g.error().message ==
            "node 'cdn': a cdn has no backend (its origin is the store behind it)");
  }
}

// An origin that calls back into the CDN in front of it (origin.read -> cdn.read -> origin.read)
// makes the CDN's miss fetch a call that waits for itself, as for a read_through cache's backend
// (above): refused, and the message calls the node the CDN's origin.
TEST_CASE("a cdn whose origin hop closes a call cycle is refused", "[graph]") {
  Scenario s{.name = "cdn-loop", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{100}, {{"read", 1.0}}),
             service("origin", {{"read", {Step{.to = "cdn", .op = "read"}}}}),
             cdn_node(HitRatio{.fixed = 0.95})};
  s.links = {LinkSpec{.from = "u", .to = "origin", .dir = Direction::inout},
             LinkSpec{.from = "origin", .to = "cdn", .dir = Direction::inout},
             LinkSpec{.from = "cdn", .to = "origin", .dir = Direction::inout}};
  const auto g = Graph::build(s);
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message == "node 'cdn': origin 'origin' closes a call cycle");
}

// The loop entered at the CDN: u -> cdn.get -> origin.get -> cdn.get. With h = 0 every request
// the origin sends back misses again, a loop of gain 1 that a run would let saturate the origin.
// The search for asynchronous hops starts at the CDN and cuts the origin's call, not the miss
// fetch, but the fetch is on the cycle all the same: refused.
TEST_CASE("a cdn whose origin hop closes a call cycle is refused when the client calls the cdn",
          "[graph]") {
  Scenario s{.name = "cdn-loop-at-edge", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{100}, {{"get", 1.0}}), cdn_node(HitRatio{.fixed = 0.0}),
             service("origin", {{"get", {Step{.to = "cdn", .op = "get"}}}})};
  s.links = {LinkSpec{.from = "u", .to = "cdn", .dir = Direction::inout},
             LinkSpec{.from = "cdn", .to = "origin", .dir = Direction::inout},
             LinkSpec{.from = "origin", .to = "cdn", .dir = Direction::inout}};
  const auto g = Graph::build(s);
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message == "node 'cdn': origin 'origin' closes a call cycle");
}

// Che's "filtered" popularity names the node whose misses a che cache receives, by its type: a
// CDN's misses are a CDN's, not a cache's.
TEST_CASE("a che cache receiving a cdn's misses and unfiltered reads names the cdn", "[graph]") {
  auto s = two_caches(HitRatio{.che = true}, 2'000'000);
  NodeSpec l1 = cdn_node(HitRatio{.che = true}, "l2");
  l1.id = "l1";
  l1.keys = 2'000'000;
  s.nodes[1] = std::move(l1);
  s.nodes[0].workload->op_shares = {{"get", 0.5}, {"peek", 0.5}};
  s.links[0].ops = {"get"};
  s.links.push_back(LinkSpec{.from = "u", .to = "l2", .dir = Direction::inout, .ops = {"peek"}});
  const auto built = Graph::build(s);
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "node 'l2': hit_ratio che: it receives both the misses of cdn 'l1' and requests no "
          "cache filtered");
}

// ---- ID generator as service sugar (spec §18 entry 12; book ch. 7) ----
//
// An id_generator is not a new kind of behaviour: Graph::build turns its `algorithm` into the
// capacity and service time a service of that design would have, and the node then runs as a
// service. The numbers come from the chapter: a snowflake id has 12 sequence bits, so one machine
// issues 4096 ids per millisecond (4096 x 1000 = 4.096M/s); a ticket server is one auto-increment
// database, a write on a SQL box that does 1k writes/s (spec §6); a UUID needs no coordination at
// all, so nothing bounds it in practice.

namespace {
using simsys::model::IdAlgorithm;

// u (10/s of `next_id`) -> ids: an id_generator of the given algorithm and replicas, with the
// capacity and latency only when the scenario states them. Node indices: u 0, ids 1.
Scenario id_front(IdAlgorithm algorithm, int replicas = 1, std::optional<Rate> capacity = {},
                  std::optional<simsys::SimTime> latency = {}) {
  Scenario s{.name = "id-front", .version = 1, .duration = 1min};
  NodeSpec ids{.id = "ids",
               .type = NodeType::id_generator,
               .replicas = replicas,
               .capacity = capacity,
               .latency = latency};
  ids.id_algorithm = algorithm;
  s.nodes = {client("u", Rate{10}, {{"next_id", 1.0}}), std::move(ids)};
  s.links = {LinkSpec{.from = "u", .to = "ids", .dir = Direction::inout}};
  return s;
}
}  // namespace

TEST_CASE("a snowflake gets 4096 ids per millisecond per replica and 10 us from its algorithm",
          "[graph]") {
  const auto g = Graph::build(id_front(IdAlgorithm::snowflake));
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  const auto& ids = g->nodes()[1];
  // 12 sequence bits = 4096 ids per ms per machine; x 1000 ms = 4'096'000 ids/s.
  REQUIRE(ids.capacity.service_rate == Rate{4'096'000});
  REQUIRE_THAT(ids.capacity.service_time_s, WithinRel(10e-6, 1e-12));
  // Not the scenario's numbers: explain must not tag them "(scenario)".
  REQUIRE(ids.service_rate_source != simsys::defaults::Source::scenario);
  REQUIRE(ids.service_time_source != simsys::defaults::Source::scenario);
}

TEST_CASE("a ticket server gets one database's 1k writes per second and 1 ms", "[graph]") {
  const auto g = Graph::build(id_front(IdAlgorithm::ticket_server));
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  const auto& ids = g->nodes()[1];
  // Spec §6: one SQL machine does 1k writes/s, and an auto-increment is a write.
  REQUIRE(ids.capacity.service_rate == Rate{1000});
  REQUIRE_THAT(ids.capacity.service_time_s, WithinRel(1e-3, 1e-12));
  REQUIRE(ids.service_rate_source != simsys::defaults::Source::scenario);
}

TEST_CASE("a uuid generator is unbounded in practice: 1e9 ids per second and 1 us", "[graph]") {
  const auto g = Graph::build(id_front(IdAlgorithm::uuid));
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  const auto& ids = g->nodes()[1];
  REQUIRE(ids.capacity.service_rate == Rate{1e9});
  REQUIRE_THAT(ids.capacity.service_time_s, WithinRel(1e-6, 1e-12));
}

// `capacity` on a resolved node is per replica (a service's 2000/s is each copy's); the node's
// total is that times its replicas. Three snowflake machines issue 3 x 4.096M = 12.288M ids/s.
TEST_CASE("a snowflake's capacity is per replica and the node's total is replicas times it",
          "[graph]") {
  const auto g = Graph::build(id_front(IdAlgorithm::snowflake, 3));
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  const auto& ids = g->nodes()[1];
  REQUIRE(ids.capacity.service_rate == Rate{4'096'000});
  REQUIRE_THAT(ids.capacity.service_rate.per_second * ids.spec.replicas,
               WithinRel(12'288'000.0, 1e-12));
}

TEST_CASE("a capacity or latency the scenario states wins over the algorithm's", "[graph]") {
  SECTION("both stated: a snowflake given 4M/s and 20 us") {
    const auto g =
        Graph::build(id_front(IdAlgorithm::snowflake, 1, Rate{4'000'000}, simsys::SimTime{20us}));
    REQUIRE(g.has_value());
    const auto& ids = g->nodes()[1];
    REQUIRE(ids.capacity.service_rate == Rate{4'000'000});
    REQUIRE_THAT(ids.capacity.service_time_s, WithinRel(20e-6, 1e-12));
    REQUIRE(ids.service_rate_source == simsys::defaults::Source::scenario);
    REQUIRE(ids.service_time_source == simsys::defaults::Source::scenario);
  }
  SECTION("only the capacity stated: the algorithm still supplies the service time") {
    const auto g = Graph::build(id_front(IdAlgorithm::ticket_server, 1, Rate{8000}));
    REQUIRE(g.has_value());
    REQUIRE(g->nodes()[1].capacity.service_rate == Rate{8000});
    REQUIRE_THAT(g->nodes()[1].capacity.service_time_s, WithinRel(1e-3, 1e-12));
    REQUIRE(g->nodes()[1].service_rate_source == simsys::defaults::Source::scenario);
    REQUIRE(g->nodes()[1].service_time_source != simsys::defaults::Source::scenario);
  }
  SECTION("only the latency stated: the algorithm still supplies the capacity") {
    const auto g =
        Graph::build(id_front(IdAlgorithm::snowflake, 1, std::nullopt, simsys::SimTime{50us}));
    REQUIRE(g.has_value());
    REQUIRE(g->nodes()[1].capacity.service_rate == Rate{4'096'000});
    REQUIRE_THAT(g->nodes()[1].capacity.service_time_s, WithinRel(50e-6, 1e-12));
    REQUIRE(g->nodes()[1].service_time_source == simsys::defaults::Source::scenario);
  }
}

// Ch. 7's ticket server is ONE auto-increment database: a second copy would hand out the same
// ids unless the two were split (odd/even), and the chapter dismisses that for not scaling and
// not surviving a server added or removed. So the model refuses replicas, and the message says
// what to do instead.
TEST_CASE("a ticket server with more than one replica is refused", "[graph]") {
  const auto g = Graph::build(id_front(IdAlgorithm::ticket_server, 2));
  REQUIRE_FALSE(g.has_value());
  CAPTURE(g.error().message);
  REQUIRE(g.error().message.starts_with("node 'ids': "));
  REQUIRE(g.error().message.contains("replicas must be 1"));
}

TEST_CASE("a ticket server with one replica and snowflake or uuid with many are accepted",
          "[graph]") {
  REQUIRE(Graph::build(id_front(IdAlgorithm::ticket_server, 1)).has_value());
  REQUIRE(Graph::build(id_front(IdAlgorithm::snowflake, 4)).has_value());
  REQUIRE(Graph::build(id_front(IdAlgorithm::uuid, 4)).has_value());
}

// The loader refuses an id_generator without an algorithm (config tests); a scenario built in code
// skips the loader, so the graph repeats the rule, as it does for a limiter without a limit and a
// cdn without an origin.
TEST_CASE("an id generator built in code without an algorithm is refused", "[graph]") {
  auto s = id_front(IdAlgorithm::snowflake);
  s.nodes[1].id_algorithm = std::nullopt;
  const auto g = Graph::build(std::move(s));
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message ==
          "node 'ids': id_generator needs algorithm: snowflake, ticket_server or uuid");
}

// ---- A write stays a write at its store ----
//
// Each node decides alone which of its ops are writes (model::write_ops_of): a cache's
// `write_ops` (default `write`), a SQL database's (default the SQL statements insert, update,
// delete and `write`). A cache that passes a write on to its store passes the op unchanged, so a
// cache whose write is called `set` or `put` sends the database an op the database counts as a
// READ: served by any replica at cost 1, the primary idle. Ch. 1's replication says the opposite
// (every write goes to the primary). The graph refuses the disagreement instead of guessing, and
// says which list to fix. The same holds for a rate limiter's `counter`, an INCR (ch. 4).

namespace {
// u -> wb -> db: a client sending `get` and `put` straight to a cache with fixed h = 0.9 whose
// write op is `put` and whose strategy has it send that write to `db`, a SQL database.
Scenario cache_writing_to_database(CacheStrategy strategy) {
  Scenario s{.name = "cache-writes", .version = 1, .duration = 1min};
  auto wb = cache("wb", 0.9, strategy, "db");
  wb.write_ops = std::vector<std::string>{"put"};
  s.nodes = {client("u", Rate{1000}, {{"get", 0.5}, {"put", 0.5}}), std::move(wb),
             NodeSpec{.id = "db", .type = NodeType::sql_db, .replicas = 3}};
  s.links = {LinkSpec{.from = "u", .to = "wb", .dir = Direction::inout},
             LinkSpec{.from = "wb", .to = "db", .dir = Direction::inout}};
  return s;
}
// u -> limiter -> api, and limiter -> `store`, the limiter's counter store.
Scenario limiter_counting_in(NodeSpec store) {
  auto limiter = limiter_of(simsys::model::LimiterAlgorithm::token_bucket, 100);
  limiter.backend = store.id;
  auto s = behind_limiter(std::move(limiter));
  s.links.push_back(LinkSpec{.from = "limiter", .to = store.id});
  s.nodes.push_back(std::move(store));
  return s;
}
}  // namespace

TEST_CASE("a write_back cache's put that its sql_db counts as a read is refused", "[graph]") {
  const auto g = Graph::build(cache_writing_to_database(CacheStrategy::write_back));
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message ==
          "node 'wb': sends write 'put' to 'db', which counts 'put' as a read; add it to db's "
          "write_ops");
}

TEST_CASE("a cache's write that its sql_db lists in write_ops builds under every write strategy",
          "[graph]") {
  for (const auto strategy :
       {CacheStrategy::write_through, CacheStrategy::write_back, CacheStrategy::write_around}) {
    CAPTURE(static_cast<int>(strategy));
    auto s = cache_writing_to_database(strategy);
    s.nodes[2].write_ops = std::vector<std::string>{"put"};
    const auto g = Graph::build(std::move(s));
    INFO((g ? "" : g.error().message));
    REQUIRE(g.has_value());
    const auto db = *g->index_of("db");
    REQUIRE(simsys::graph::is_write(g->nodes()[db].spec, "put"));
  }
}

// Under read_through the cache sends its store only the misses of its reads; its writes stay in
// the cache, so the database never sees `put` and its write_ops need not name it.
TEST_CASE("a read_through cache's writes never reach the store, so its store's list is free",
          "[graph]") {
  const auto g = Graph::build(cache_writing_to_database(CacheStrategy::read_through));
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
}

// A cache behind a cache counts its writes too: a cache store with the default `write_ops`
// (`write`) would answer a front cache's `put` from its hit ratio, as a read.
TEST_CASE("a write that a store cache counts as a read is refused like a database's", "[graph]") {
  auto s = cache_writing_to_database(CacheStrategy::write_through);
  s.nodes[2] = cache("db", 0.5, CacheStrategy::cache_aside);
  const auto refused = Graph::build(s);
  REQUIRE_FALSE(refused.has_value());
  REQUIRE(refused.error().message ==
          "node 'wb': sends write 'put' to 'db', which counts 'put' as a read; add it to db's "
          "write_ops");
  s.nodes[2].write_ops = std::vector<std::string>{"put"};
  const auto built = Graph::build(std::move(s));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
}

// A limiter's `counter` is "read and increment this client's counter" (ch. 4's Redis INCR): a
// write. A sql_db counter store with the default write_ops would serve it from a replica.
TEST_CASE("a limiter's counter that its store counts as a read is refused", "[graph]") {
  SECTION("a sql_db store") {
    const auto g = Graph::build(
        limiter_counting_in(NodeSpec{.id = "store", .type = NodeType::sql_db, .replicas = 2}));
    REQUIRE_FALSE(g.has_value());
    REQUIRE(g.error().message ==
            "node 'limiter': sends write 'counter' to 'store', which counts 'counter' as a read; "
            "add it to store's write_ops");
  }
  SECTION("a cache store") {
    const auto g =
        Graph::build(limiter_counting_in(cache("store", 1.0, CacheStrategy::cache_aside)));
    REQUIRE_FALSE(g.has_value());
    REQUIRE(g.error().message ==
            "node 'limiter': sends write 'counter' to 'store', which counts 'counter' as a read; "
            "add it to store's write_ops");
  }
  SECTION("a store that lists counter in its write_ops builds") {
    auto store = NodeSpec{.id = "store", .type = NodeType::sql_db, .replicas = 2};
    store.write_ops = std::vector<std::string>{"counter"};
    const auto g = Graph::build(limiter_counting_in(std::move(store)));
    INFO((g ? "" : g.error().message));
    REQUIRE(g.has_value());
  }
}

// A limiter asks its counter store before it admits a request (ch. 4: read and increment the
// counter, then forward or refuse), so the counter hop is always waited for, like a cache's fetch.
// Here the store is a write_through cache whose database calls the limiter again: limiter.call ->
// counters.counter -> db.counter -> limiter.call, a request that waits for itself. Refused, naming
// the limiter and its backend.
TEST_CASE("a limiter whose counter store calls back into it is refused", "[graph]") {
  auto counters = cache("counters", 0.5, CacheStrategy::write_through, "db");
  counters.write_ops = std::vector<std::string>{"counter"};
  auto s = limiter_counting_in(std::move(counters));
  s.nodes.push_back(service("db", {{"counter", {Step{.to = "limiter", .op = "call"}}}}));
  s.links.push_back(LinkSpec{.from = "counters", .to = "db", .dir = Direction::inout});
  s.links.push_back(LinkSpec{.from = "db", .to = "limiter", .dir = Direction::inout});
  const auto g = Graph::build(s);
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message == "node 'limiter': backend 'counters' closes a call cycle");
}

// ---- One request size per (node, op), resolved at build (spec §19 entry 1) ----
//
// A byte-sized node counts megabytes, so it must know how big each request it serves is. The
// graph resolves one size per (node, op) with a walk over the client routes and the steps that
// mirrors the che key walk above: a request keeps its size while it keeps its op name, and a
// step that renames the op gives an unsized request unless the step states `size:`.
//
// The accessor these tests propose: `Graph::request_bytes(node_index, op)` returns
// `std::optional<simsys::Bytes>`: the size, or an empty optional for an unsized (node, op) and
// for an op that never reaches the node, as `hit_ratio(index)` is empty for a node without one.

namespace {
// The size, in bytes, the graph resolved for `op` at the node `id`; empty when it has none.
std::optional<double> request_bytes_at(const Graph& g, std::string_view id, std::string_view op) {
  const auto size = g.request_bytes(*g.index_of(id), op);
  if (!size) return std::nullopt;
  return size->count;
}

// The same, for a size the test needs: it fails here, instead of dereferencing an empty optional.
double bytes_required(const Graph& g, std::string_view id, std::string_view op) {
  const auto size = request_bytes_at(g, id, op);
  REQUIRE(size.has_value());
  return *size;
}

// A client sending `op` at 100/s, each request `size` long (no size when empty).
NodeSpec sized_client(std::string id, std::string op, std::optional<simsys::Bytes> size) {
  auto node = client(std::move(id), Rate{100}, {{op, 1.0}});
  if (size) node.workload->sizes = {{op, *size}};
  return node;
}

// A blob store with no stated capacity: byte-sized by its type's default, spec section 6's
// 500 MB/s.
NodeSpec default_blob_store(std::string id = "store") {
  return NodeSpec{.id = std::move(id), .type = NodeType::blob_store};
}

// u (sending `get` at the given size) -> lb -> svc -> store, with svc.get = [{store, step_op}] and
// `store` the given node.
Scenario sized_chain(std::optional<simsys::Bytes> size, Step step, NodeSpec store) {
  Scenario s{.name = "sized", .version = 1, .duration = 1min};
  s.nodes = {sized_client("u", "get", size), load_balancer("lb", LbPolicy::round_robin),
             service("svc", {{"get", {std::move(step)}}}), std::move(store)};
  const auto store_id = s.nodes.back().id;
  s.links = {LinkSpec{.from = "u", .to = "lb"}, LinkSpec{.from = "lb", .to = "svc"},
             LinkSpec{.from = "svc", .to = store_id}};
  return s;
}
}  // namespace

// 1 MB is the number a client states for `get`; the balancer routes it on and the service's step
// keeps the op name, so every node on the way sees a request of exactly 1 MB = 1e6 B (decimal:
// 1 MB = 1000 KB, as core::parse_size reads it).
TEST_CASE("a request keeps its size through a balancer and a step with the same op", "[graph]") {
  const auto built = Graph::build(
      sized_chain(simsys::Bytes{1e6}, Step{.to = "store", .op = "get"}, default_blob_store()));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  for (const auto* node : {"lb", "svc", "store"}) {
    CAPTURE(node);
    const auto size = request_bytes_at(*built, node, "get");
    REQUIRE(size.has_value());
    REQUIRE_THAT(*size, WithinRel(1e6, 1e-12));
  }
}

// A step that renames the op (get -> fetch) starts a new request whose size the client never
// stated, so it is unsized. A request-counting node (a plain service) never needs a size, so the
// scenario builds; the empty optional says so.
TEST_CASE("a step that renames the op leaves the request unsized unless the step states a size",
          "[graph]") {
  SECTION("unsized") {
    const auto built = Graph::build(
        sized_chain(simsys::Bytes{1e6}, Step{.to = "store", .op = "fetch"}, service("store")));
    INFO((built ? "" : built.error().message));
    REQUIRE(built.has_value());
    REQUIRE_FALSE(request_bytes_at(*built, "store", "fetch").has_value());
    // The incoming op `get` still has its 1 MB at the service that renamed it.
    REQUIRE_THAT(bytes_required(*built, "svc", "get"), WithinRel(1e6, 1e-12));
  }
  SECTION("sized by the step") {
    const auto built = Graph::build(sized_chain(
        simsys::Bytes{1e6}, Step{.to = "store", .op = "fetch", .size = simsys::Bytes{2e6}},
        service("store")));
    INFO((built ? "" : built.error().message));
    REQUIRE(built.has_value());
    const auto size = request_bytes_at(*built, "store", "fetch");  // 2 MB = 2e6 B
    REQUIRE(size.has_value());
    REQUIRE_THAT(*size, WithinRel(2e6, 1e-12));
  }
}

// The size is a property of the request, so a client that states no size for an op leaves it
// unsized all the way, and a request-counting node downstream is no error.
TEST_CASE("an unsized op reaching a request-counting service builds and has no size", "[graph]") {
  const auto built =
      Graph::build(sized_chain(std::nullopt, Step{.to = "store", .op = "get"}, service("store")));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  REQUIRE_FALSE(request_bytes_at(*built, "store", "get").has_value());
}

// A limiter forwards by link, like a balancer: the op, and so the size, passes through it.
TEST_CASE("a request keeps its size through a rate limiter's route", "[graph]") {
  auto s = behind_limiter(limiter_of(simsys::model::LimiterAlgorithm::token_bucket, 50));
  s.nodes[0].workload->sizes = {{"call", simsys::Bytes{2e3}}};  // 2 KB = 2000 B
  const auto built = Graph::build(s);
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  const auto size = request_bytes_at(*built, "api", "call");
  REQUIRE(size.has_value());
  REQUIRE_THAT(*size, WithinRel(2e3, 1e-12));
}

// A cache forwards what it does not hold to its backend under the same op: the read miss under
// read_through, the write under write_through (ch. 1's cache strategies). Same op, same bytes.
TEST_CASE("a cache forwards a request to its backend with the size it arrived with", "[graph]") {
  const auto forwarding = [](CacheStrategy strategy, std::string op, simsys::Bytes size) {
    Scenario s{.name = "forwarding", .version = 1, .duration = 1min};
    s.nodes = {sized_client("u", op, size), cache("cache", 0.8, strategy, "db"), service("db")};
    s.links = {LinkSpec{.from = "u", .to = "cache"}, LinkSpec{.from = "cache", .to = "db"}};
    return s;
  };
  SECTION("a read_through cache's miss") {
    const auto built = Graph::build(forwarding(CacheStrategy::read_through, "read", {1e6}));
    INFO((built ? "" : built.error().message));
    REQUIRE(built.has_value());
    REQUIRE_THAT(bytes_required(*built, "db", "read"), WithinRel(1e6, 1e-12));
  }
  SECTION("a write_through cache's write") {
    const auto built = Graph::build(forwarding(CacheStrategy::write_through, "write", {2e3}));
    INFO((built ? "" : built.error().message));
    REQUIRE(built.has_value());
    REQUIRE_THAT(bytes_required(*built, "db", "write"), WithinRel(2e3, 1e-12));
  }
}

// A CDN is the read_through shape of a cache (spec §18 entry 10): on a miss it fetches the object
// from its origin. The origin is asked for the same stream, so it serves 50 MB = 5e7 B per miss,
// and both the byte-sized CDN (capacity 100GB/s) and its blob-store origin know it.
TEST_CASE("a cdn's origin receives its reads' size", "[graph]") {
  NodeSpec cdn{.id = "cdn",
               .type = NodeType::cdn,
               .byte_capacity = simsys::ByteRate{100e9},
               .hit_ratio = HitRatio{.fixed = 0.95}};
  cdn.origin = "store";
  Scenario s{.name = "cdn-sizes", .version = 1, .duration = 1min};
  s.nodes = {sized_client("u", "stream", simsys::Bytes{50e6}), std::move(cdn),
             default_blob_store()};
  s.links = {LinkSpec{.from = "u", .to = "cdn"}, LinkSpec{.from = "cdn", .to = "store"}};
  const auto built = Graph::build(s);
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  REQUIRE_THAT(bytes_required(*built, "cdn", "stream"), WithinRel(5e7, 1e-12));
  REQUIRE_THAT(bytes_required(*built, "store", "stream"), WithinRel(5e7, 1e-12));
}

namespace {
// a and b (each sending `get` at its own size, none when empty) -> store (the given node).
Scenario two_clients_one_store(std::optional<simsys::Bytes> a, std::optional<simsys::Bytes> b,
                               NodeSpec store = default_blob_store()) {
  Scenario s{.name = "two-senders", .version = 1, .duration = 1min};
  s.nodes = {sized_client("a", "get", a), sized_client("b", "get", b), std::move(store)};
  s.links = {LinkSpec{.from = "a", .to = "store"}, LinkSpec{.from = "b", .to = "store"}};
  return s;
}
}  // namespace

// Two clients may share a (node, op) only if they agree on its size: the byte-sized node counts
// one number of megabytes per request, and a second size would make that number a guess.
TEST_CASE("two clients sending one op at different sizes into a byte-sized store is an error",
          "[graph]") {
  const auto built = Graph::build(two_clients_one_store(simsys::Bytes{1e6}, simsys::Bytes{2e6}));
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "node 'store': op 'get' arrives with two sizes, 1MB from 'a' and 2MB from 'b'");
}

TEST_CASE("two clients sending one op at the same size agree", "[graph]") {
  const auto built = Graph::build(two_clients_one_store(simsys::Bytes{1e6}, simsys::Bytes{1e6}));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  REQUIRE_THAT(bytes_required(*built, "store", "get"), WithinRel(1e6, 1e-12));
}

// The senders in the message are whoever sent the request to the node, not always a client:
// here two services each write to the store, each stating its own size on the step.
TEST_CASE("two services whose steps state different sizes for one op name both senders",
          "[graph]") {
  Scenario s{.name = "two-steps", .version = 1, .duration = 1min};
  s.nodes = {
      sized_client("u", "go", std::nullopt),
      service("a", {{"go", {Step{.to = "store", .op = "write", .size = simsys::Bytes{1e6}}}}}),
      service("b", {{"go", {Step{.to = "store", .op = "write", .size = simsys::Bytes{2e6}}}}}),
      default_blob_store()};
  s.links = {LinkSpec{.from = "u", .to = "a"}, LinkSpec{.from = "u", .to = "b"},
             LinkSpec{.from = "a", .to = "store"}, LinkSpec{.from = "b", .to = "store"}};
  const auto built = Graph::build(s);
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "node 'store': op 'write' arrives with two sizes, 1MB from 'a' and 2MB from 'b'");
}

// The blob store's capacity is a byte rate (spec section 6: 500 MB/s), so it counts megabytes
// and every request it serves needs one. The message says where the capacity came from, and what
// to add: a size on the step that sent the request, or on the client.
TEST_CASE("an unsized op reaching a byte-sized store is an error that says where to add the size",
          "[graph]") {
  const std::string advice = "has no size: add size: to the step or sizes: to the client";
  SECTION("sent by a service's renaming step") {
    const auto built = Graph::build(
        sized_chain(simsys::Bytes{1e6}, Step{.to = "store", .op = "fetch"}, default_blob_store()));
    REQUIRE_FALSE(built.has_value());
    REQUIRE(built.error().message ==
            "node 'store' counts bytes (capacity 500MB/s, default), but op 'fetch' from 'svc' " +
                advice);
  }
  SECTION("sent by a client that states no size") {
    const auto built = Graph::build(two_clients_one_store(std::nullopt, std::nullopt));
    REQUIRE_FALSE(built.has_value());
    // Two senders lack the size; the first in scenario order is named.
    REQUIRE(built.error().message ==
            "node 'store' counts bytes (capacity 500MB/s, default), but op 'get' from 'a' " +
                advice);
  }
}

// The same node, with the step stating the size: the scenario builds, and the step's 2 MB is the
// size of `fetch` at the store (the incoming `get` size is not inherited by a renamed op).
TEST_CASE("a step's size makes a renamed op acceptable to a byte-sized store", "[graph]") {
  const auto built = Graph::build(sized_chain(
      simsys::Bytes{1e6}, Step{.to = "store", .op = "fetch", .size = simsys::Bytes{2e6}},
      default_blob_store()));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  REQUIRE_THAT(bytes_required(*built, "store", "fetch"), WithinRel(2e6, 1e-12));
}

// ---- Every op at a byte-sized node has a size, and the size walk's
// decisions pinned ----
//
// Graph::collect_arriving_ops (which ops reach a node) and the size walk (which size each one
// has) start from different places: a worker pool's unconditional `then` steps fire in the first
// whatever reaches the pool, while the size walk reaches a pool only through a queue. Every op
// that arrives at a byte-sized node must still have a size, or the node would charge it one unit
// whatever its bytes, so the graph refuses such an op whichever walk found it.

namespace {
// A worker pool with the given `then` steps, one worker of 1 ms a job (and the loader requires
// both), draining `queue` when one is named (a queue linked to a pool must be consumed by it).
NodeSpec worker_pool(std::string id, std::vector<Step> then,
                     std::optional<std::string> queue = std::nullopt) {
  NodeSpec pool{.id = std::move(id), .type = NodeType::worker_pool};
  pool.workers = 1;
  pool.job_cost = 1ms;
  pool.consumes = std::move(queue);
  pool.then = std::move(then);
  return pool;
}

NodeSpec message_queue(std::string id) {
  return NodeSpec{.id = std::move(id), .type = NodeType::message_queue};
}

// q -> pool -> store, the pool's one `then` step being `step`; nothing publishes to q, so no job
// ever reaches the pool.
Scenario unfed_pool(Step step) {
  Scenario s{.name = "unfed-pool", .version = 1, .duration = 1min};
  s.nodes = {message_queue("q"), worker_pool("pool", {std::move(step)}, "q"), default_blob_store()};
  s.links = {LinkSpec{.from = "q", .to = "pool"}, LinkSpec{.from = "pool", .to = "store"}};
  return s;
}

const std::string no_size_advice = "has no size: add size: to the step or sizes: to the client";
}  // namespace

// The control: a loop of BOXES that is no loop of CALLS. The store publishes each read to a queue
// whose worker pool reads the cache again, but a queue answers its publisher at the append
// (detail::consumer_deliveries), so no request waits for the pool: the cache's fetch lies on no
// call cycle, and the scenario builds.
TEST_CASE("a cache whose store hands work to a queue that reaches the cache again builds",
          "[graph]") {
  Scenario s{.name = "async-loop", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{100}, {{"read", 1.0}}),
             cache("cache", 0.5, CacheStrategy::read_through, "db"),
             service("db", {{"read", {Step{.to = "q", .op = "read"}}}}), message_queue("q"),
             worker_pool("pool", {Step{.to = "cache", .op = "read"}}, "q")};
  s.links = {LinkSpec{.from = "u", .to = "cache", .dir = Direction::inout},
             LinkSpec{.from = "cache", .to = "db", .dir = Direction::inout},
             LinkSpec{.from = "db", .to = "q", .dir = Direction::inout},
             LinkSpec{.from = "q", .to = "pool"}, LinkSpec{.from = "pool", .to = "cache"}};
  const auto g = Graph::build(s);
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
}

// An `on: failure` step that calls back into its own op (web.write fails at db, the failure step
// asks `retrier`, which writes to web again) is a retry with no bound: every retried write that
// fails again is retried again, and nothing in the model stops it (the review's probe answered
// 21/s of 1000, `estimate` dropping 99.5 % with no word on why). The failure step's call and the
// op it reports from share a call cycle (one strongly connected component of the call graph, the
// test the backend hops use), so the scenario is refused and pointed at the bounded form.
TEST_CASE("an on-failure step that closes a call cycle is refused: an unbounded retry", "[graph]") {
  Scenario s{.name = "retry-loop", .version = 1, .duration = 1min};
  s.nodes = {
      client("u", Rate{1000}, {{"write", 1.0}}),
      service("web", {{"write",
                       {Step{.to = "db", .op = "write"},
                        Step{.to = "retrier", .op = "retry", .on = StepCondition::failure}}}}),
      service("db"), service("retrier", {{"retry", {Step{.to = "web", .op = "write"}}}})};
  s.links = {LinkSpec{.from = "u", .to = "web", .dir = Direction::inout},
             LinkSpec{.from = "web", .to = "db", .dir = Direction::inout},
             LinkSpec{.from = "web", .to = "retrier", .dir = Direction::inout},
             LinkSpec{.from = "retrier", .to = "web", .dir = Direction::inout}};
  const auto g = Graph::build(s);
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message ==
          "node 'web': an on: failure step to 'retrier' closes a call cycle (an unbounded retry: "
          "use the client's retries)");
}

// The same retry through a message queue: the failure step publishes the failed write to
// `retry_q`, whose pool writes to web again. A queue answers its publisher at the append
// (detail::consumer_deliveries), so the call cycle ends at the queue and the scenario builds. It
// still retries for ever (a job that fails again is published again: the dead-letter queue of
// ch. 10's notification system, without its retry limit); a pool's `retries` is the bounded form.
TEST_CASE(
    "an on-failure step that retries through a message queue builds: the queue ends the calls",
    "[graph]") {
  Scenario s{.name = "queued-retry", .version = 1, .duration = 1min};
  s.nodes = {
      client("u", Rate{1000}, {{"write", 1.0}}),
      service("web", {{"write",
                       {Step{.to = "db", .op = "write"},
                        Step{.to = "retry_q", .op = "write", .on = StepCondition::failure}}}}),
      service("db"), message_queue("retry_q"),
      worker_pool("retrier", {Step{.to = "web", .op = "write"}}, "retry_q")};
  s.links = {LinkSpec{.from = "u", .to = "web", .dir = Direction::inout},
             LinkSpec{.from = "web", .to = "db", .dir = Direction::inout},
             LinkSpec{.from = "web", .to = "retry_q", .dir = Direction::inout},
             LinkSpec{.from = "retry_q", .to = "retrier"},
             LinkSpec{.from = "retrier", .to = "web", .dir = Direction::inout}};
  const auto g = Graph::build(s);
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
}

// Nothing feeds the pool, yet its `then` step counts as arriving at the store (an unconditional
// `then` fires for whatever the node receives). Before this fix the store took `write` with no size
// and charged it one unit; now the step must say how big a write is. No job arrives at the pool as
// `write`, so the step renames every job: the message gives the plain advice, without the note on a
// pool's same-op steps (that note is for a step whose op is one of the pool's arriving ops, the
// case of the test below with `upload` and `reupload`).
TEST_CASE("a worker pool's then step that no queue feeds still needs a size at a byte-sized store",
          "[graph]") {
  const auto built = Graph::build(unfed_pool(Step{.to = "store", .op = "write"}));
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "node 'store' counts bytes (capacity 500MB/s, default), but op 'write' from 'pool' " +
              no_size_advice);
}

// The step's own size holds wherever the step fires, fed or not: 50 MB = 5e7 B.
TEST_CASE("a worker pool's then step that no queue feeds is sized by its own size", "[graph]") {
  const auto built =
      Graph::build(unfed_pool(Step{.to = "store", .op = "write", .size = simsys::Bytes{50e6}}));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  REQUIRE_THAT(bytes_required(*built, "store", "write"), WithinRel(5e7, 1e-12));
}

// A pool built in code that names no queue (no `consumes`): the loader refuses such a file
// (`worker_pool needs consumes`), and so does Graph::build, in the same words. Before, it built and
// could never receive a job (every sender but its queue is refused, and it had none), so its room
// fell back to the default retention for a queue that did not exist.
TEST_CASE("a worker pool built in code without consumes is refused as the loader refuses it",
          "[graph]") {
  Scenario s{.name = "unlinked-pool", .version = 1, .duration = 1min};
  s.nodes = {sized_client("u", "upload", simsys::Bytes{300e6}), message_queue("q"),
             worker_pool("other", {}, "q"),
             worker_pool("pool", {Step{.to = "store", .op = "upload"}}), default_blob_store()};
  s.links = {LinkSpec{.from = "u", .to = "q"}, LinkSpec{.from = "q", .to = "other"},
             LinkSpec{.from = "pool", .to = "store"}};
  const auto built = Graph::build(s);
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message == "node 'pool': worker_pool needs consumes");
}

// A client link of weight 0 is drained: no request takes it (Graph::client_routes gives it share
// 0). So `get` does not arrive at the store behind it, and the store needs no size for it.
TEST_CASE("a client's drained route brings no op to the node behind it", "[graph]") {
  Scenario s{.name = "drained", .version = 1, .duration = 1min};
  s.nodes = {sized_client("u", "get", std::nullopt), service("svc"), default_blob_store()};
  s.links = {LinkSpec{.from = "u", .to = "svc"},
             LinkSpec{.from = "u", .to = "store", .weight = 0.0}};
  const auto built = Graph::build(s);
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  CHECK(built->arriving_ops(*built->index_of("store")).empty());
  REQUIRE_FALSE(request_bytes_at(*built, "store", "get").has_value());
}

// A rate limiter asks its backend `counter` once per request it checks (spec §17 entry 6): the
// limiter's own request, which carries no client bytes. A byte-sized backend would have to price it
// in megabytes, so it is refused with a message of its own.
TEST_CASE("a limiter's counter into a byte-sized store is refused: the counter carries no size",
          "[graph]") {
  auto store = default_blob_store();
  store.write_ops = std::vector<std::string>{"counter"};  // `counter` is an INCR, a write
  const auto built = Graph::build(limiter_counting_in(std::move(store)));
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "node 'store' counts bytes (capacity 500MB/s, default), but 'limiter' sends it its "
          "counter: a limiter's counter carries no size; its backend must count requests");
}

// One size per (node, op) holds at every node, not only at a byte-sized one: a plain service that
// forwards the op unchanged would otherwise pass two sizes on.
TEST_CASE("two sizes for one op are refused at a request-counting service too", "[graph]") {
  const auto built =
      Graph::build(two_clients_one_store(simsys::Bytes{1e6}, simsys::Bytes{2e6}, service("store")));
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "node 'store': op 'get' arrives with two sizes, 1MB from 'a' and 2MB from 'b'");
}

// Sized and unsized requests of one op meet at a service: the service needs no size, so it
// builds and resolves the op to unsized, rather than give every request the size only some have.
// A byte-sized store behind it then names the client whose requests lack the size.
TEST_CASE("sized and unsized requests of one op resolve to unsized", "[graph]") {
  SECTION("at a request-counting service") {
    const auto built =
        Graph::build(two_clients_one_store(simsys::Bytes{1e6}, std::nullopt, service("store")));
    INFO((built ? "" : built.error().message));
    REQUIRE(built.has_value());
    REQUIRE_FALSE(request_bytes_at(*built, "store", "get").has_value());
  }
  SECTION("and a byte-sized store behind it names the client that sent no size") {
    Scenario s{.name = "mixed", .version = 1, .duration = 1min};
    s.nodes = {sized_client("a", "get", simsys::Bytes{1e6}), sized_client("b", "get", std::nullopt),
               service("svc", {{"get", {Step{.to = "store", .op = "get"}}}}), default_blob_store()};
    s.links = {LinkSpec{.from = "a", .to = "svc"}, LinkSpec{.from = "b", .to = "svc"},
               LinkSpec{.from = "svc", .to = "store"}};
    const auto built = Graph::build(s);
    REQUIRE_FALSE(built.has_value());
    REQUIRE(built.error().message ==
            "node 'store' counts bytes (capacity 500MB/s, default), but op 'get' from 'b' " +
                no_size_advice);
  }
}

// Two clients' sizes meet behind a balancer: the store hears both from 'lb', so the message
// names where each size came from (the clients), not the balancer twice. The store is declared
// before the balancer, so it is the first node whose sizes are resolved.
TEST_CASE("a size conflict names the clients that stated the sizes, not the balancer between",
          "[graph]") {
  Scenario s{.name = "conflict-behind-lb", .version = 1, .duration = 1min};
  s.nodes = {sized_client("a", "get", simsys::Bytes{1e6}),
             sized_client("b", "get", simsys::Bytes{2e6}), default_blob_store(),
             load_balancer("lb", LbPolicy::round_robin)};
  s.links = {LinkSpec{.from = "a", .to = "lb"}, LinkSpec{.from = "b", .to = "lb"},
             LinkSpec{.from = "lb", .to = "store"}};
  const auto built = Graph::build(s);
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "node 'store': op 'get' arrives with two sizes, 1MB from 'a' and 2MB from 'b'");
}

// A message keeps its op name through the queue and into the pool (spec §19 entry 4), so a pool's
// `then` step with the same op carries the client's 300 MB on; a step that renames the op carries
// its own `size:`, 50 MB (youtube's transcoders write renditions so).
TEST_CASE("a queue's pool keeps a same-op size on its then step and uses a renamed step's size",
          "[graph]") {
  Scenario s{.name = "queue-pool", .version = 1, .duration = 1min};
  s.nodes = {sized_client("u", "upload", simsys::Bytes{300e6}), message_queue("q"),
             worker_pool("pool",
                         {Step{.to = "store", .op = "upload"},
                          Step{.to = "renditions", .op = "write", .size = simsys::Bytes{50e6}}},
                         "q"),
             default_blob_store(), default_blob_store("renditions")};
  s.links = {LinkSpec{.from = "u", .to = "q"}, LinkSpec{.from = "q", .to = "pool"},
             LinkSpec{.from = "pool", .to = "store"}, LinkSpec{.from = "pool", .to = "renditions"}};
  const auto built = Graph::build(s);
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  REQUIRE_THAT(bytes_required(*built, "pool", "upload"), WithinRel(3e8, 1e-12));
  REQUIRE_THAT(bytes_required(*built, "store", "upload"), WithinRel(3e8, 1e-12));
  REQUIRE_THAT(bytes_required(*built, "renditions", "write"), WithinRel(5e7, 1e-12));
}

// ---- A worker pool's then steps (spec §19 entry 5) ----
//
// A pool's `then` steps are the work each DRAINED job hands on (book ch. 14: a transcoding worker
// writes its renditions and publishes a completion message), resolved like a cache's unconditional
// steps: demand edges to their targets, a share of 1 whatever op the pool's queue brought (every
// drained job fires every step), the step's own `size:` or, for a same-op step, the job's size.
// `on: miss` and `on: failure` are refused as a blob store's are: a pool has no lookup to miss, and
// a pool's `then` steps follow no step a failure step could report on.

namespace {
// u (publishes `publish`, 8/s) -> q -> w (consumes q; `then`) -> {store (a default blob store),
// done_q} and done_q -> w2. Node indices: u 0, q 1, w 2, store 3, done_q 4, w2 5.
Scenario pool_handing_on(std::vector<Step> then,
                         NodeSpec u = client("u", Rate{8}, {{"publish", 1.0}})) {
  Scenario s{.name = "pool-then", .version = 1, .duration = 1min};
  s.nodes = {std::move(u),         message_queue("q"),      worker_pool("w", std::move(then), "q"),
             default_blob_store(), message_queue("done_q"), worker_pool("w2", {}, "done_q")};
  s.links = {LinkSpec{.from = "u", .to = "q"}, LinkSpec{.from = "q", .to = "w"},
             LinkSpec{.from = "w", .to = "store"}, LinkSpec{.from = "w", .to = "done_q"},
             LinkSpec{.from = "done_q", .to = "w2"}};
  return s;
}

const std::vector<Step> renditions_and_completion{
    Step{.to = "store", .op = "write", .factor = 4.0, .size = simsys::Bytes{50e6}},
    Step{.to = "done_q", .op = "publish"}};
}  // namespace

TEST_CASE("a worker pool's then steps are demand edges to their targets", "[graph]") {
  const auto built = Graph::build(pool_handing_on(renditions_and_completion));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  const auto pool = *built->index_of("w");
  for (const auto* target : {"store", "done_q"}) {
    CAPTURE(target);
    const auto to = *built->index_of(target);
    const auto link = built->demand_link(pool, to);
    REQUIRE(link.has_value());
    REQUIRE(
        std::ranges::contains(built->edges(), DemandEdge{.from = pool, .to = to, .link = *link}));
  }
  // The ops that arrive are the steps' own, once each: `write` at the store, `publish` at done_q.
  const std::vector<std::string> writes{"write"};
  REQUIRE(std::ranges::equal(built->arriving_ops(*built->index_of("store")), writes));
  const std::vector<std::string> publishes{"publish"};
  REQUIRE(std::ranges::equal(built->arriving_ops(*built->index_of("done_q")), publishes));
}

// Every drained job fires every unconditional step, whatever op the queue brought it under: the
// share is 1 for `publish`, the op this scenario publishes, and for any other op too (unlike a blob
// store's steps, which fire for its writes only). A characterisation test of the graph: the share
// was already 1 before the steps ran.
TEST_CASE("a worker pool's then step takes a share of 1 of every job it drains", "[graph]") {
  const auto built = Graph::build(pool_handing_on(renditions_and_completion));
  REQUIRE(built.has_value());
  const auto pool = *built->index_of("w");
  for (const auto& step : built->nodes()[pool].spec.then) {
    CAPTURE(step.to);
    REQUIRE(built->then_fraction(pool, step, "publish") == 1.0);
    REQUIRE(built->then_fraction(pool, step, "audit") == 1.0);
  }
}

// The step's `size:` is the write's 50 MB = 5e7 B at the byte-sized store, where the renamed op
// would otherwise be unsized and refused; the completion message states none and the client's
// `publish` is unsized, so done_q's `publish` has no size (it counts requests, not bytes).
TEST_CASE("a worker pool's then step is sized by its own size at the store", "[graph]") {
  const auto built = Graph::build(pool_handing_on(renditions_and_completion));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  REQUIRE_THAT(bytes_required(*built, "store", "write"), WithinRel(5e7, 1e-12));
  REQUIRE_FALSE(request_bytes_at(*built, "done_q", "publish").has_value());
}

// A same-op step carries the size the job came with: the client's 2 KB `publish` goes through the
// queue and into the pool, and the pool's own `publish` step to done_q still weighs 2 KB = 2e3 B.
TEST_CASE("a worker pool's same-op then step inherits the size of the job it drains", "[graph]") {
  const auto built = Graph::build(pool_handing_on(
      {Step{.to = "done_q", .op = "publish"}}, sized_client("u", "publish", simsys::Bytes{2e3})));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  REQUIRE_THAT(bytes_required(*built, "w", "publish"), WithinRel(2e3, 1e-12));
  REQUIRE_THAT(bytes_required(*built, "done_q", "publish"), WithinRel(2e3, 1e-12));
}

// A same-op step passes on the size of the jobs that arrive under its op only: u publishes `upload`
// and `reupload`, both 300 MB; the pool's step `{to: store, op: upload}` carries the uploads' 300
// MB on, but renames every `reupload` job to `upload`, and a renamed request without `size:` has
// none. The store counts bytes, so the build fails, and the message says why a size the client
// states did not reach it.
TEST_CASE(
    "a pool's same-op then step renames the jobs of another op unsized, and the error says so",
    "[graph]") {
  NodeSpec u = client("u", Rate{8}, {{"upload", 0.5}, {"reupload", 0.5}});
  u.workload->sizes = {{"upload", simsys::Bytes{300e6}}, {"reupload", simsys::Bytes{300e6}}};
  const auto built =
      Graph::build(pool_handing_on({Step{.to = "store", .op = "upload"}}, std::move(u)));
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "node 'store' counts bytes (capacity 500MB/s, default), but op 'upload' from 'w' " +
              no_size_advice +
              " (a worker pool's then step keeps the size of the jobs that arrive as 'upload' "
              "only: it renames every other job, and a renamed job has no size)");
}

// Nobody waits on a pool: the hop into it from its queue is asynchronous. Its own then-hops are
// ordinary hops from its point of view, not asynchronous hops: what a pool hands on is not an event
// nobody waits for but a call the pool makes (and the engine's backward pass never asks, because no
// one waits for the pool: its response stays its own).
TEST_CASE("the queue's hop into a pool is asynchronous and the pool's own then hops are not",
          "[graph]") {
  const auto built = Graph::build(pool_handing_on(renditions_and_completion));
  REQUIRE(built.has_value());
  const auto queue = *built->index_of("q");
  const auto pool = *built->index_of("w");
  REQUIRE(built->is_async_hop(queue, "publish", pool, "publish"));
  REQUIRE_FALSE(built->is_async_hop(pool, "publish", *built->index_of("store"), "write"));
  REQUIRE_FALSE(built->is_async_hop(pool, "publish", *built->index_of("done_q"), "publish"));
}

// `on: miss` names a cache's miss; a pool has no lookup. Refused at build, with the reason, in
// the blob store's words (a pool's steps fire for every job it drains).
TEST_CASE("an on-miss then step on a worker pool is refused", "[graph]") {
  const auto built = Graph::build(
      pool_handing_on({Step{.to = "done_q", .op = "publish", .on = StepCondition::miss}}));
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "node 'w': a worker pool's then steps fire for every job it drains "
          "(on: miss needs a cache)");
}

TEST_CASE("an on-miss step after an always step on a worker pool is refused too", "[graph]") {
  const auto built = Graph::build(
      pool_handing_on({Step{.to = "done_q", .op = "publish"},
                       Step{.to = "done_q", .op = "audit", .on = StepCondition::miss}}));
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "node 'w': a worker pool's then steps fire for every job it drains "
          "(on: miss needs a cache)");
}

// `on: failure` reports on a step of an op's step list (the nearest unconditional one before it,
// spec §5), and a pool's `then` steps follow none: every one fires for every job drained, and a
// failed hand-off is the target's drop. Refused at build, as a cache's then step on failure is,
// with a message that says where failure steps do fire.
TEST_CASE("an on-failure then step on a worker pool is refused", "[graph]") {
  const auto built = Graph::build(
      pool_handing_on({Step{.to = "done_q", .op = "publish", .on = StepCondition::failure}}));
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "node 'w': a worker pool's then steps fire for every job it drains "
          "(on: failure: a job's then steps follow no step to report on; failure steps fire in an "
          "op's step list)");
}

// Parallel is fine on a pool (unlike a blob store's event): nobody waits for either, but a
// scenario may group the hand-offs as it likes (a transcoder writes its renditions beside its
// completion message), and both steps fire.
TEST_CASE("a parallel then group on a worker pool builds and fires both steps", "[graph]") {
  const auto built = Graph::build(pool_handing_on(
      {Step{.to = "store", .op = "write", .mode = StepMode::parallel, .size = simsys::Bytes{50e6}},
       Step{.to = "done_q", .op = "publish", .mode = StepMode::parallel}}));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  const auto pool = *built->index_of("w");
  REQUIRE(built->demand_link(pool, *built->index_of("store")).has_value());
  REQUIRE(built->demand_link(pool, *built->index_of("done_q")).has_value());
  REQUIRE_FALSE(built->arriving_ops(*built->index_of("store")).empty());
  REQUIRE_FALSE(built->arriving_ops(*built->index_of("done_q")).empty());
}

// A request of 0 bytes costs 0 megabytes: a byte-sized node would serve infinitely many of them
// per second and divide by zero to say so. The loader refuses `0B` in `sizes:` and `size:`; the
// graph refuses it too, for a scenario built in code.
TEST_CASE("a zero-byte request at a byte-sized store is refused", "[graph]") {
  const auto built = Graph::build(two_clients_one_store(simsys::Bytes{0.0}, simsys::Bytes{0.0}));
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "node 'store' counts bytes (capacity 500MB/s, default), but op 'get' from 'a' has size "
          "0B: a request's size must be positive");
}

// ---- A blob store's then steps are asynchronous event notifications (spec §19 entries 6-7)
// ----
//
// S3 event notifications (book ch. 14: "transcoding starts when the upload completes"): for every
// write the store stored (an op in its `write_ops`; S3's ObjectCreated), each `then` step sends its
// op to its target, and nobody waits for it. A read fires nothing: S3 has no event for a GET.
// The graph says so the way it does for a write_back cache's flush: Graph::is_async_hop, which the
// engine's backward pass reads to leave the hop out of the sender's response and delivered share.

namespace {
// u (uploads of 100 MB and reads of 1 MB, 10/s each) -> store (a default blob store whose
// `write_ops` is `[upload]`, as youtube's original_store, and whose `then` is `then`) -> svc, a
// service, over zero-latency links. Node indices: u 0, store 1, svc 2.
Scenario store_with_notifications(std::vector<Step> then) {
  Scenario s{.name = "blob-notify", .version = 1, .duration = 1min};
  auto u = client("u", Rate{20}, {{"read", 0.5}, {"upload", 0.5}});
  u.workload->sizes = {{"upload", simsys::Bytes{100e6}}, {"read", simsys::Bytes{1e6}}};
  auto store = default_blob_store();
  store.write_ops = std::vector<std::string>{"upload"};
  store.then = std::move(then);
  s.nodes = {std::move(u), std::move(store), service("svc")};
  s.links = {LinkSpec{.from = "u", .to = "store", .latency = 0s},
             LinkSpec{.from = "store", .to = "svc", .latency = 0s}};
  return s;
}
}  // namespace

// Every write the store stores fires the step, so the write op has its own asynchronous hop to the
// target, carrying the op the step names. A read fires nothing (S3 has no event for a GET), so it
// has no hop at all. The client's call into the store stays synchronous, and so does a hop the
// step does not make.
TEST_CASE("a blob store's then step is an asynchronous hop for its writes and none for its reads",
          "[graph]") {
  const auto built = Graph::build(store_with_notifications({Step{.to = "svc", .op = "notify"}}));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  const auto u = *built->index_of("u");
  const auto store = *built->index_of("store");
  const auto svc = *built->index_of("svc");
  REQUIRE(built->is_async_hop(store, "upload", svc, "notify"));
  REQUIRE_FALSE(built->is_async_hop(store, "read", svc, "notify"));  // a read notifies nobody
  REQUIRE_FALSE(std::ranges::contains(
      built->async_hops(),
      simsys::graph::AsyncHop{.from = store, .op = "read", .to = svc, .target_op = "notify"}));
  REQUIRE_FALSE(built->is_async_hop(store, "upload", svc, "other"));  // a hop no step makes
  REQUIRE_FALSE(built->is_async_hop(u, "upload", store, "upload"));   // the upload itself waits
  REQUIRE(std::ranges::contains(
      built->async_hops(),
      simsys::graph::AsyncHop{.from = store, .op = "upload", .to = svc, .target_op = "notify"}));
}

// A same-op step is the same hop with the incoming op name (it also keeps the size, which the
// theory test pins): the target_op is the op the step carries, here `upload`.
TEST_CASE("a blob store's same-op then step is an asynchronous hop carrying that op", "[graph]") {
  const auto built = Graph::build(store_with_notifications({Step{.to = "svc", .op = "upload"}}));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  const auto store = *built->index_of("store");
  const auto svc = *built->index_of("svc");
  REQUIRE(built->is_async_hop(store, "upload", svc, "upload"));
  REQUIRE_FALSE(built->is_async_hop(store, "upload", svc, "notify"));
}

// The paper load (explain) and the engine's forward pass follow a blob store's then steps along the
// graph's demand edges, as a service's steps: store -> svc is an edge over the link between them,
// the op that arrives at svc is `notify`, and nothing else reaches it. A characterisation test: it
// passed before a blob store's events were modelled too (a `then` step was already a demand edge),
// and pins that the events kept it so.
TEST_CASE("a blob store's then step is a demand edge that delivers its op to the target",
          "[graph]") {
  const auto built = Graph::build(store_with_notifications({Step{.to = "svc", .op = "notify"}}));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  const auto store = *built->index_of("store");
  const auto svc = *built->index_of("svc");
  const auto link = built->demand_link(store, svc);
  REQUIRE(link.has_value());
  REQUIRE(
      std::ranges::contains(built->edges(), DemandEdge{.from = store, .to = svc, .link = *link}));
  // arriving_ops is a span: compare it as a range with the one op expected.
  const std::vector<std::string> expected{"notify"};
  REQUIRE(std::ranges::equal(built->arriving_ops(svc), expected));
}

// A cache's miss is the only thing `on: miss` can name. A blob store never misses: its steps fire
// for every write it stores, so a step on miss is refused at build with the reason.
TEST_CASE("an on-miss then step on a blob store is refused", "[graph]") {
  const auto built = Graph::build(
      store_with_notifications({Step{.to = "svc", .op = "notify", .on = StepCondition::miss}}));
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "node 'store': a blob store's then steps fire for every write it stores "
          "(on: miss needs a cache)");
}

TEST_CASE("an on-miss step after an always step on a blob store is refused too", "[graph]") {
  const auto built = Graph::build(
      store_with_notifications({Step{.to = "svc", .op = "notify"},
                                Step{.to = "svc", .op = "audit", .on = StepCondition::miss}}));
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "node 'store': a blob store's then steps fire for every write it stores "
          "(on: miss needs a cache)");
}

// A store that only ever receives reads (its `write_ops` default to `[write]`, and `get` is no
// write) fires no event: the step sends nothing, so `notify` never arrives at svc and the store
// has no asynchronous hop. 1000 GETs/s of a video do not start 1000 transcodes/s.
TEST_CASE("a blob store that only serves reads sends its then target nothing", "[graph]") {
  Scenario s{.name = "blob-reads", .version = 1, .duration = 1min};
  auto u = client("u", Rate{1000}, {{"get", 1.0}});
  u.workload->sizes = {{"get", simsys::Bytes{1e6}}};
  auto store = default_blob_store();
  store.then = {Step{.to = "svc", .op = "notify"}};
  s.nodes = {std::move(u), std::move(store), service("svc")};
  s.links = {LinkSpec{.from = "u", .to = "store", .latency = 0s},
             LinkSpec{.from = "store", .to = "svc", .latency = 0s}};
  const auto built = Graph::build(s);
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  REQUIRE(built->arriving_ops(*built->index_of("svc")).empty());
  REQUIRE(built->async_hops().empty());
  const auto& step = built->nodes()[*built->index_of("store")].spec.then[0];
  REQUIRE(built->then_fraction(*built->index_of("store"), step, "get") == 0.0);
}

// The share of a store's requests a step carries (Graph::then_fraction): all of a write, none of
// a read.
TEST_CASE("a blob store's then step carries every write and no read", "[graph]") {
  const auto built = Graph::build(store_with_notifications({Step{.to = "svc", .op = "notify"}}));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  const auto store = *built->index_of("store");
  const auto& step = built->nodes()[store].spec.then[0];
  REQUIRE(built->then_fraction(store, step, "upload") == 1.0);
  REQUIRE(built->then_fraction(store, step, "read") == 0.0);
}

// `on: failure` reports on a step of an op's step list (the nearest unconditional one before it,
// spec §5), and a blob store's events follow none: each fires for every write stored (S3 delivers
// at least once). Refused at build, as a cache's then step on failure is, with a message that says
// where failure steps do fire.
TEST_CASE("an on-failure then step on a blob store is refused", "[graph]") {
  const auto built = Graph::build(
      store_with_notifications({Step{.to = "svc", .op = "notify", .on = StepCondition::failure}}));
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "node 'store': a blob store's then steps fire for every write it stores "
          "(on: failure: an event follows no step to report on; failure steps fire in an op's "
          "step list)");
}

// `mode` orders the calls a request waits for (sequential latencies add, parallel ones overlap).
// Nobody waits for an event, so a parallel one means nothing: refused at build. (`mode:
// sequential`, the model's default, is refused by the loader, which sees the key written.)
TEST_CASE("a parallel then step on a blob store is refused", "[graph]") {
  const auto built = Graph::build(
      store_with_notifications({Step{.to = "svc", .op = "notify", .mode = StepMode::parallel}}));
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "node 'store': a blob store's then steps are event notifications nobody waits for "
          "(mode orders the calls a request waits for)");
}

// ---- A link's bandwidth is a pipe (spec §19 entry 3) ----
//
// A link that states `bandwidth` becomes a pipe the engine queues megabytes through: one server
// (c = 1) whose unit is 1 MB, served in 1 MB / bandwidth, deterministic, with a waiting room of
// 1000 MB (the generic queue_limit default read in megabytes). The graph resolves it once, at
// build, as it resolves a byte-sized node's capacity.
//
// The accessor these tests propose: `ResolvedLink::pipe`, a `std::optional<fluid::Capacity>`: the
// pipe's capacity in megabytes (service_rate in MB/s, service_time_s the time of ONE megabyte,
// the same convention as a byte-sized node's ResolvedNode::capacity, so a request of 10 MB does
// not change it; the engine sets each step's object moments on the queue), empty for a link
// without a bandwidth.
//
// And a pipe needs the size of every op it carries (it prices megabytes, not requests), as a
// byte-sized node does: an op that reaches the link unsized is an error at build.

namespace {
// u -> lb -> svc -> store (see sized_chain), with `bandwidth` on the link into `store`
// (links()[2]); `store` counts requests, so only the pipe minds a missing size.
Scenario pipe_chain(std::optional<simsys::Bytes> size, Step step, simsys::ByteRate bandwidth) {
  auto s = sized_chain(size, std::move(step), service("store"));
  s.links[2].bandwidth = bandwidth;
  return s;
}

// 1 Gbit/s = 1e9 / 8 bytes per second, the fixtures module's constant (shared with the theory
// tests of the same pipe).
using simsys::test_support::one_gbit;
}  // namespace

// 1 Gbit/s = 125e6 B/s = 125 units of 1 MB per second; one unit takes 1 / 125 s = 8 ms; c = rate
// x time = 125 x 8 ms = 1 (the invariant fluid::Capacity states, with c = 1, as a pipe is a
// single server); the room is the generic 1000 units read as megabytes; each object's own service
// is deterministic (SCV 0: a fixed number of megabytes at a fixed rate), and arrivals are Poisson
// until the scenario says otherwise (burstiness 1).
TEST_CASE("a link with a bandwidth resolves a one-server pipe in megabytes", "[graph]") {
  const auto built =
      Graph::build(pipe_chain(simsys::Bytes{1e6}, Step{.to = "store", .op = "get"}, one_gbit));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  REQUIRE(built->links().size() == 3);
  REQUIRE(built->links()[2].pipe.has_value());
  const auto& pipe = *built->links()[2].pipe;
  REQUIRE_THAT(pipe.service_rate.per_second, WithinRel(125.0, 1e-12));
  REQUIRE_THAT(pipe.service_time_s, WithinRel(0.008, 1e-12));
  REQUIRE(pipe.concurrency == 1);
  REQUIRE_THAT(pipe.queue_limit, WithinRel(1000.0, 1e-12));
  REQUIRE(pipe.service_variability == 0.0);
  REQUIRE(pipe.burstiness == 1.0);
}

TEST_CASE("the pipe's numbers follow the bandwidth in bits or bytes, not the request size",
          "[graph]") {
  SECTION("40 Gbit/s is 5 GB/s: 5000 MB per second, 0.2 ms per megabyte") {
    const auto built = Graph::build(
        pipe_chain(simsys::Bytes{1e6}, Step{.to = "store", .op = "get"}, simsys::ByteRate{5e9}));
    REQUIRE(built.has_value());
    const auto& pipe = *built->links()[2].pipe;
    REQUIRE_THAT(pipe.service_rate.per_second, WithinRel(5000.0, 1e-12));
    REQUIRE_THAT(pipe.service_time_s, WithinRel(0.0002, 1e-12));
    REQUIRE(pipe.concurrency == 1);
  }
  SECTION("1 GB/s stated in bytes: 1000 MB per second, 1 ms per megabyte") {
    const auto built = Graph::build(
        pipe_chain(simsys::Bytes{1e6}, Step{.to = "store", .op = "get"}, simsys::ByteRate{1e9}));
    REQUIRE(built.has_value());
    const auto& pipe = *built->links()[2].pipe;
    REQUIRE_THAT(pipe.service_rate.per_second, WithinRel(1000.0, 1e-12));
    REQUIRE_THAT(pipe.service_time_s, WithinRel(0.001, 1e-12));
  }
  SECTION("a 10 MB request leaves the per-megabyte numbers alone") {
    const auto built =
        Graph::build(pipe_chain(simsys::Bytes{10e6}, Step{.to = "store", .op = "get"}, one_gbit));
    REQUIRE(built.has_value());
    const auto& pipe = *built->links()[2].pipe;
    REQUIRE_THAT(pipe.service_rate.per_second, WithinRel(125.0, 1e-12));
    REQUIRE_THAT(pipe.service_time_s, WithinRel(0.008, 1e-12));
    REQUIRE_THAT(bytes_required(*built, "store", "get"), WithinRel(10e6, 1e-12));
  }
}

TEST_CASE("only the link that states a bandwidth has a pipe", "[graph]") {
  const auto built =
      Graph::build(pipe_chain(simsys::Bytes{1e6}, Step{.to = "store", .op = "get"}, one_gbit));
  REQUIRE(built.has_value());
  REQUIRE_FALSE(built->links()[0].pipe.has_value());  // u -> lb
  REQUIRE_FALSE(built->links()[1].pipe.has_value());  // lb -> svc
  REQUIRE(built->links()[2].pipe.has_value());        // svc -> store
  // A scenario with no bandwidth anywhere has no pipe, sized requests or not.
  const auto plain = Graph::build(
      sized_chain(simsys::Bytes{1e6}, Step{.to = "store", .op = "get"}, service("store")));
  REQUIRE(plain.has_value());
  for (const auto& link : plain->links()) REQUIRE_FALSE(link.pipe.has_value());
}

// The message names the link, the op, the node that sent it and what to add, as a byte-sized
// node's does.
TEST_CASE("an unsized op on a bandwidth link is an error that says where to add the size",
          "[graph]") {
  const std::string advice = "has no size: add size: to the step or sizes: to the client";
  SECTION("sent by a service's renaming step") {
    const auto built =
        Graph::build(pipe_chain(simsys::Bytes{1e6}, Step{.to = "store", .op = "fetch"}, one_gbit));
    REQUIRE_FALSE(built.has_value());
    REQUIRE(built.error().message ==
            "link 'svc'->'store' has a bandwidth, but op 'fetch' from 'svc' " + advice);
  }
  SECTION("sent by a client that states no size") {
    auto s = sized_chain(std::nullopt, Step{.to = "store", .op = "get"}, service("store"));
    s.links[0].bandwidth = one_gbit;  // u -> lb
    const auto built = Graph::build(s);
    REQUIRE_FALSE(built.has_value());
    REQUIRE(built.error().message ==
            "link 'u'->'lb' has a bandwidth, but op 'get' from 'u' " + advice);
  }
}

namespace {
// u (100/s of `put`, 1 MB each) -> web, whose `put` runs `steps`; web -> db and web -> log, the
// link to log with a 1 Gbit/s bandwidth: its pipe prices every request web sends to log in
// megabytes, so each needs a size (db and log count requests and never mind one).
Scenario failure_web(std::vector<Step> steps) {
  Scenario s{.name = "failure-web", .version = 1, .duration = 1min};
  s.nodes = {sized_client("u", "put", simsys::Bytes{1e6}),
             service("web", {{"put", std::move(steps)}}), service("db"), service("log")};
  s.links = {LinkSpec{.from = "u", .to = "web"}, LinkSpec{.from = "web", .to = "db"},
             LinkSpec{.from = "web", .to = "log", .bandwidth = one_gbit}};
  return s;
}
}  // namespace

// An `on: failure` step is priced like any step (spec §5): the size walk follows it, so an unsized
// one over a bandwidth link is refused with the refusal every unsized step gets (the same words as
// the test above: the link, the op, the sender and what to add). Phase 2's walk skipped failure
// steps, so this scenario used to build and the engine would have thrown at the first request.
TEST_CASE("an unsized on-failure step over a bandwidth link is refused like any unsized step",
          "[graph]") {
  const std::string unsized =
      "link 'web'->'log' has a bandwidth, but op 'write' from 'web' has no "
      "size: add size: to the step or sizes: to the client";
  SECTION("on: failure") {
    const auto built =
        Graph::build(failure_web({Step{.to = "db", .op = "write"},
                                  Step{.to = "log", .op = "write", .on = StepCondition::failure}}));
    REQUIRE_FALSE(built.has_value());
    REQUIRE(built.error().message == unsized);
  }
  SECTION("on: always, the same step, is refused in the same words") {
    const auto built = Graph::build(
        failure_web({Step{.to = "db", .op = "write"}, Step{.to = "log", .op = "write"}}));
    REQUIRE_FALSE(built.has_value());
    REQUIRE(built.error().message == unsized);
  }
}

// The same step with a `size:` builds, and the graph knows the size of what log is sent: 2 MB
// (a failure step states its own size like any step: it is a new request, not the client's put).
TEST_CASE("a sized on-failure step over a bandwidth link builds and prices its 2 MB", "[graph]") {
  const auto built = Graph::build(failure_web(
      {Step{.to = "db", .op = "write"},
       Step{
           .to = "log", .op = "write", .on = StepCondition::failure, .size = simsys::Bytes{2e6}}}));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  REQUIRE(built->links()[2].pipe.has_value());  // web -> log
  const auto size = request_bytes_at(*built, "log", "write");
  REQUIRE(size.has_value());
  REQUIRE_THAT(*size, WithinRel(2e6, 1e-12));
}

TEST_CASE("a step's size makes a renamed op acceptable on a bandwidth link", "[graph]") {
  const auto built = Graph::build(
      pipe_chain(simsys::Bytes{1e6}, Step{.to = "store", .op = "fetch", .size = simsys::Bytes{2e6}},
                 one_gbit));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  REQUIRE(built->links()[2].pipe.has_value());
  REQUIRE_THAT(bytes_required(*built, "store", "fetch"), WithinRel(2e6, 1e-12));
}

// A pipe prices the ops its link CARRIES: a client's `put`s go over the bandwidth link (its ops
// filter) and its unsized `get`s over another, so nothing needs the `get` a size; give the
// bandwidth link the `get` as well and it does.
TEST_CASE("only the ops a bandwidth link carries need a size", "[graph]") {
  Scenario s{.name = "split", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{100}, {{"get", 0.5}, {"put", 0.5}}), service("w"), service("r")};
  s.nodes[0].workload->sizes = {{"put", simsys::Bytes{1e6}}};  // `get` has no size
  s.links = {LinkSpec{.from = "u", .to = "w", .ops = {"put"}, .bandwidth = one_gbit},
             LinkSpec{.from = "u", .to = "r", .ops = {"get"}}};
  SECTION("the unsized op takes the link without a bandwidth") {
    const auto built = Graph::build(s);
    INFO((built ? "" : built.error().message));
    REQUIRE(built.has_value());
    REQUIRE(built->links()[0].pipe.has_value());
    REQUIRE_FALSE(built->links()[1].pipe.has_value());
  }
  SECTION("the same op on the bandwidth link is refused") {
    s.links[0].ops = {"put", "get"};
    s.links[1].ops = {"get"};
    const auto built = Graph::build(s);
    REQUIRE_FALSE(built.has_value());
    REQUIRE(built.error().message ==
            "link 'u'->'w' has a bandwidth, but op 'get' from 'u' has no size: add size: to the "
            "step or sizes: to the client");
  }
}

// Two more requests a pipe cannot price in megabytes: one of 0 bytes would cost nothing (the engine
// divides by its megabytes), and a rate limiter's `counter` is the limiter's own request, which no
// `size:` can describe, so the link to its backend cannot be a pipe at all; each refusal says why.
TEST_CASE("a pipe refuses a 0-byte request and a limiter's counter, each with its reason",
          "[graph]") {
  SECTION("a request of 0 bytes") {
    const auto built =
        Graph::build(pipe_chain(simsys::Bytes{0}, Step{.to = "store", .op = "get"}, one_gbit));
    REQUIRE_FALSE(built.has_value());
    REQUIRE(built.error().message ==
            "link 'svc'->'store' has a bandwidth, but op 'get' from 'u' has size 0B: a request's "
            "size must be positive");
  }
  SECTION("a limiter's counter over its backend link") {
    auto s = limiter_counting_in(service("store"));
    s.links.back().bandwidth = one_gbit;  // limiter -> store
    const auto built = Graph::build(s);
    REQUIRE_FALSE(built.has_value());
    REQUIRE(built.error().message ==
            "link 'limiter'->'store' has a bandwidth, but 'limiter' sends its counter over it: a "
            "limiter's counter carries no size, so its backend link cannot have a bandwidth; "
            "remove it");
  }
}

// ---- Message queues and worker pools (spec §19 entries 4-6) ----
//
// A message queue is a broker (Kafka, ch. 14's transcode_q): a publisher's request is answered when
// the message is appended, and every consumer group (a worker pool that `consumes` the queue)
// receives EVERY message, as Kafka's consumer groups do; google-drive's two groups on notify_q
// read it so. The graph checks the wiring both ways, gives the queue one route of share 1 per
// consumer, makes each queue -> pool hop asynchronous (nobody upstream waits for a worker), and
// resolves a pool as M/M/c over consumption: c = workers, service time = cost, capacity
// workers / cost, with a waiting room of retention x capacity messages.

namespace {
// A pool of `workers` workers at `cost` a job that drains `queue`.
NodeSpec consuming_pool(std::string id, std::string queue, int workers, simsys::SimTime cost) {
  NodeSpec pool{.id = std::move(id), .type = NodeType::worker_pool};
  pool.consumes = std::move(queue);
  pool.workers = workers;
  pool.job_cost = cost;
  return pool;
}

// u (100 `publish`/s) -> q -> every pool in `pools`, each of which consumes q.
// Node indices: u 0, q 1, then the pools in the order given.
Scenario queue_feeding(std::vector<NodeSpec> pools) {
  Scenario s{.name = "queue-feeding", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{100}, {{"publish", 1.0}}), message_queue("q")};
  s.links = {LinkSpec{.from = "u", .to = "q"}};
  for (auto& pool : pools) {
    s.links.push_back(LinkSpec{.from = "q", .to = pool.id});
    s.nodes.push_back(std::move(pool));
  }
  return s;
}

// The pool `w`: 4 workers of 3.333333 ms (a capacity of 4 / 3.333333 ms = 1200.00012/s).
NodeSpec four_workers_at_3_333_ms() {
  return consuming_pool("w", "q", 4, simsys::SimTime{3'333'333});
}
}  // namespace

// Kafka delivers a message to the consumers of a group, not to a service that happens to be
// linked: a link out of a queue to anything but a pool that consumes the queue is a design error,
// and the message names the queue and the node that does not consume it. Both shapes of it: a
// service behind the queue, and a pool that drains another queue.
TEST_CASE("a message queue delivers only to the worker pools that consume it", "[graph]") {
  SECTION("a service behind the queue") {
    auto s = queue_feeding({});
    s.nodes.push_back(service("svc"));
    s.links.push_back(LinkSpec{.from = "q", .to = "svc"});
    const auto built = Graph::build(s);
    REQUIRE_FALSE(built.has_value());
    REQUIRE(built.error().message ==
            "node 'q': a message queue delivers only to the worker pools that consume it ('svc' "
            "does not)");
  }
  SECTION("a pool that consumes another queue") {
    auto s = queue_feeding({consuming_pool("w", "other_q", 4, 5ms)});
    s.nodes.push_back(message_queue("other_q"));
    s.links.push_back(LinkSpec{.from = "other_q", .to = "w"});  // other_q's wiring is right
    const auto built = Graph::build(s);
    REQUIRE_FALSE(built.has_value());
    REQUIRE(built.error().message ==
            "node 'q': a message queue delivers only to the worker pools that consume it ('w' "
            "does not)");
  }
}

// The other direction of the wiring: a pool that says it drains `q` is fed by a link from `q`.
// Without it the pool would sit idle while the scenario claims otherwise; the message names both
// the pool and the queue.
TEST_CASE("a worker pool's consumes queue must link to it, and the error names both", "[graph]") {
  auto s = queue_feeding({});
  s.nodes.push_back(four_workers_at_3_333_ms());  // consumes q, but nothing links q -> w
  const auto built = Graph::build(s);
  REQUIRE_FALSE(built.has_value());
  const auto& message = built.error().message;
  INFO(message);
  REQUIRE(message.starts_with("node 'w'"));
  REQUIRE(message.contains("'q'"));
  REQUIRE(message.contains("link"));
}

// What a pool may drain is a queue: a message queue, or a URL frontier (web-crawler's downloader
// consumes `frontier`; the frontier itself runs in phase 3). Naming any other node is refused with
// its type, so the scenario's author sees what was named.
TEST_CASE("a worker pool consumes only a message queue or a frontier", "[graph]") {
  SECTION("a service is no queue") {
    auto s = queue_feeding({});
    s.nodes.push_back(service("svc"));
    s.nodes.push_back(consuming_pool("w", "svc", 4, 5ms));
    s.links.push_back(LinkSpec{.from = "svc", .to = "w"});
    const auto built = Graph::build(s);
    REQUIRE_FALSE(built.has_value());
    REQUIRE(built.error().message ==
            "node 'w': consumes 'svc' (type service); a worker pool consumes a message_queue or a "
            "frontier");
  }
  SECTION("a frontier is one") {
    auto s = queue_feeding({consuming_pool("w0", "q", 4, 5ms)});  // q needs a consumer of its own
    s.nodes.push_back(NodeSpec{.id = "frontier", .type = NodeType::frontier});
    s.nodes.push_back(consuming_pool("w", "frontier", 4, 5ms));
    s.links.push_back(LinkSpec{.from = "frontier", .to = "w"});
    const auto built = Graph::build(s);
    INFO((built ? "" : built.error().message));
    REQUIRE(built.has_value());
  }
}

// Every consumer group receives every message: the queue's routes are one per consumer, each of
// share 1 (a load balancer's shares sum to 1 because it SPLITS; a queue COPIES). With two groups
// each gets the whole flow, not half. The op keeps its name, so each pool is reached by `publish`.
TEST_CASE("a message queue has one route of share one per consumer group", "[graph]") {
  const auto g = *Graph::build(
      queue_feeding({consuming_pool("w1", "q", 4, 5ms), consuming_pool("w2", "q", 2, 10ms)}));
  const auto q = *g.index_of("q");
  const auto w1 = *g.index_of("w1");
  const auto w2 = *g.index_of("w2");
  const auto routes = g.routes(q);
  REQUIRE(routes.size() == 2);
  for (const auto& route : routes) {
    CAPTURE(route.target);
    REQUIRE_THAT(route.share, WithinRel(1.0, 1e-12));
  }
  // C++20 std::ranges::count with a projection: how many routes have `target` == w1, replacing
  // std::count_if with a lambda that compares the member by hand.
  REQUIRE(std::ranges::count(routes, w1, &simsys::graph::Route::target) == 1);
  REQUIRE(std::ranges::count(routes, w2, &simsys::graph::Route::target) == 1);
  // Per op the shares are still 1 each: nothing renormalises a copy into a split.
  const auto publish = g.routes_for(q, "publish");
  REQUIRE(publish.size() == 2);
  for (const auto& route : publish) REQUIRE_THAT(route.share, WithinRel(1.0, 1e-12));
  REQUIRE(std::ranges::equal(g.arriving_ops(w1), std::vector<std::string>{"publish"}));
  REQUIRE(std::ranges::equal(g.arriving_ops(w2), std::vector<std::string>{"publish"}));
}

// Spec §19 entry 6: the publisher waits for the append alone. The queue -> pool hop is the hand-off
// nobody upstream waits for (as a write_back cache's flush and a blob store's event are), so the
// backward pass leaves the pool's sojourn and losses out of the publisher's response and delivered
// fraction. The publisher's own call into the queue stays synchronous.
TEST_CASE("the hop from a queue to its consumer is asynchronous and the publisher's is not",
          "[graph]") {
  const auto g = *Graph::build(
      queue_feeding({consuming_pool("w1", "q", 4, 5ms), consuming_pool("w2", "q", 2, 10ms)}));
  const auto u = *g.index_of("u");
  const auto q = *g.index_of("q");
  REQUIRE(g.is_async_hop(q, "publish", *g.index_of("w1"), "publish"));
  REQUIRE(g.is_async_hop(q, "publish", *g.index_of("w2"), "publish"));
  REQUIRE_FALSE(g.is_async_hop(u, "publish", q, "publish"));
}

// The pool is M/M/c over consumption (spec section 7: "capacity = workers / job cost"):
//   c            = workers                         = 4
//   service time = cost                            = 3.333333 ms
//   capacity     = workers / cost = 4 / 3.333333 ms = 1200.00012 jobs/s (1.2k/s)
//   room         = retention x capacity: Kafka deletes a message older than `retention`, so at
//                  most the messages `retention` seconds of draining could have read can wait:
//                  10 s x 1200.00012/s = 12,000 messages.
TEST_CASE("a worker pool resolves to c workers, service time cost and capacity workers over cost",
          "[graph]") {
  const auto g = *Graph::build(queue_feeding({four_workers_at_3_333_ms()}));
  const auto& pool = g.nodes()[*g.index_of("w")].capacity;
  REQUIRE(pool.concurrency == 4);
  REQUIRE_THAT(pool.service_time_s, WithinRel(3.333333e-3, 1e-9));
  REQUIRE_THAT(pool.service_rate.per_second, WithinRel(1200.0, 1e-6));
}

TEST_CASE("a pool's waiting room is the queue's retention times its capacity", "[graph]") {
  SECTION("retention stated: 10 s of draining is 12,000 messages") {
    auto s = queue_feeding({four_workers_at_3_333_ms()});
    s.nodes[1].retention = 10s;
    const auto g = *Graph::build(s);
    REQUIRE_THAT(g.nodes()[*g.index_of("w")].capacity.queue_limit, WithinRel(12'000.0, 1e-6));
  }
  SECTION("retention absent: Kafka's default of 7 days, 604,800 s x 1200/s = 725.76 million") {
    const auto g = *Graph::build(queue_feeding({four_workers_at_3_333_ms()}));
    REQUIRE_THAT(g.nodes()[*g.index_of("w")].capacity.queue_limit,
                 WithinRel(604'800.0 * 1200.0, 1e-6));
  }
  SECTION("each group's room is its own: retention x ITS capacity") {
    auto s = queue_feeding({four_workers_at_3_333_ms(), consuming_pool("slow", "q", 1, 100ms)});
    s.nodes[1].retention = 10s;
    const auto g = *Graph::build(s);
    // slow: 1 worker at 100 ms = 10 jobs/s, so 10 s x 10/s = 100 messages.
    REQUIRE_THAT(g.nodes()[*g.index_of("slow")].capacity.queue_limit, WithinRel(100.0, 1e-9));
    REQUIRE_THAT(g.nodes()[*g.index_of("w")].capacity.queue_limit, WithinRel(12'000.0, 1e-6));
  }
}

// The broker's own queue is the append (spec section 6: 1M msg/s at 100 us chosen), c = 1M/s x
// 100 us = 100 by Little's law: it keeps the type's numbers whatever consumes it.
TEST_CASE("the broker itself keeps the type's append numbers: 1M messages a second, c = 100",
          "[graph]") {
  const auto g = *Graph::build(queue_feeding({four_workers_at_3_333_ms()}));
  const auto& broker = g.nodes()[*g.index_of("q")].capacity;
  REQUIRE_THAT(broker.service_rate.per_second, WithinRel(1e6, 1e-12));
  REQUIRE_THAT(broker.service_time_s, WithinRel(100e-6, 1e-12));
  REQUIRE(broker.concurrency == 100);
}

// ---- A pool's only sender is its queue, and a queue's wiring carries flow ----
//
// A worker pool drains the queue it consumes and nothing else: any other demand edge into it (a
// client's route, a service's step) would make it a server that a caller waits for, with a 7-day
// room in which nobody is ever dropped. A queue nobody consumes, and a consumer link whose `ops`
// filter lets none of the queue's ops through, are pipelines the scenario believes run and that
// carry nothing: all refused at build, each naming what to fix.

// A client sending straight to a pool: refused, naming the sender (the pool consumes q, which
// nothing publishes to: the client's route is its only way in).
TEST_CASE("a client sending to a worker pool is refused: a pool takes jobs from its queue only",
          "[graph]") {
  Scenario s{.name = "direct-pool", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{100}, {{"job", 1.0}}), message_queue("q"), worker_pool("w", {}, "q")};
  s.links = {LinkSpec{.from = "u", .to = "w"}, LinkSpec{.from = "q", .to = "w"}};
  const auto built = Graph::build(s);
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "node 'w': a worker pool takes jobs only from the queue it consumes ('u' sends to it)");
}

// The pool consumes q, but a service also calls it directly: the second way in is refused, though
// the first is fine (the review measured rho 1.67 and a client's response holding the pool's
// sojourn before this check).
TEST_CASE("a service step into a pool that consumes a queue is refused, naming the service",
          "[graph]") {
  auto s = queue_feeding({consuming_pool("w", "q", 4, 5ms)});
  s.nodes[0].workload->op_shares = {{"publish", 0.5}, {"call", 0.5}};
  s.nodes.push_back(service("svc", {{"call", {Step{.to = "w", .op = "publish"}}}}));
  s.links.push_back(LinkSpec{.from = "u", .to = "svc"});
  s.links.push_back(LinkSpec{.from = "svc", .to = "w"});
  const auto built = Graph::build(s);
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "node 'w': a worker pool takes jobs only from the queue it consumes ('svc' sends to it)");
}

// A queue with no link to any pool would answer its publishers and keep their messages for nobody.
TEST_CASE("a message queue that no worker pool consumes is refused", "[graph]") {
  const auto built = Graph::build(queue_feeding({}));
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "node 'q': no worker pool consumes this message queue (its messages would be kept for "
          "nobody)");
}

// A consumer link's `ops` filter selects what its group reads: here w1 reads only `publish` and
// w2 everything, so the 10 % `audit` messages reach w2 alone (each group still reads ALL of what
// it reads: share 1, a copy).
TEST_CASE("a consumer link's ops filter decides which ops its group reads", "[graph]") {
  auto s = queue_feeding({consuming_pool("w1", "q", 4, 5ms), consuming_pool("w2", "q", 4, 5ms)});
  s.nodes[0].workload->op_shares = {{"publish", 0.9}, {"audit", 0.1}};
  s.links[1].ops = {"publish"};  // q -> w1
  const auto g = *Graph::build(s);
  const auto q = *g.index_of("q");
  const auto w1 = *g.index_of("w1");
  const auto w2 = *g.index_of("w2");
  REQUIRE(std::ranges::equal(g.arriving_ops(w1), std::vector<std::string>{"publish"}));
  REQUIRE(std::ranges::equal(g.arriving_ops(w2), std::vector<std::string>{"audit", "publish"}));
  const auto audit = g.routes_for(q, "audit");
  REQUIRE(audit.size() == 1);
  REQUIRE(audit.front().target == w2);
  REQUIRE_THAT(audit.front().share, WithinRel(1.0, 1e-12));
  REQUIRE(g.is_async_hop(q, "audit", w2, "audit"));
  REQUIRE_FALSE(g.is_async_hop(q, "audit", w1, "audit"));
}

// A filter that lets none of the queue's ops through leaves its group nothing to read.
TEST_CASE("a consumer link whose ops filter carries nothing the queue receives is refused",
          "[graph]") {
  auto s = queue_feeding({consuming_pool("w", "q", 4, 5ms)});
  s.links[1].ops = {"audit"};  // q -> w; the queue only ever receives `publish`
  const auto built = Graph::build(s);
  REQUIRE_FALSE(built.has_value());
  REQUIRE(built.error().message ==
          "link 'q'->'w' carries none of the ops 'q' receives (link ops: audit; it receives: "
          "publish)");
}

// A consumer link of weight 0 is a drained route, as everywhere: its group reads nothing (share
// 0), so no op arrives at it and no delivery to it is an asynchronous hop; the other group still
// reads every message.
TEST_CASE("a weight-0 consumer link is a group that reads nothing: share 0", "[graph]") {
  auto s = queue_feeding({consuming_pool("w1", "q", 4, 5ms), consuming_pool("idle", "q", 4, 5ms)});
  s.links[2].weight = 0.0;  // q -> idle
  const auto g = *Graph::build(s);
  const auto q = *g.index_of("q");
  const auto idle = *g.index_of("idle");
  const auto routes = g.routes(q);
  REQUIRE(routes.size() == 2);
  for (const auto& route : routes) {
    CAPTURE(route.target);
    REQUIRE(route.share == (route.target == idle ? 0.0 : 1.0));
  }
  REQUIRE(g.arriving_ops(idle).empty());
  REQUIRE_FALSE(g.is_async_hop(q, "publish", idle, "publish"));
  REQUIRE(
      std::ranges::equal(g.arriving_ops(*g.index_of("w1")), std::vector<std::string>{"publish"}));
}

// A URL frontier has no `retention`, so a pool draining one gets the default room: Kafka's 7 days
// of its own rate, 604,800 s x (4 workers / 5 ms = 800/s) = 483.84 million URLs.
TEST_CASE("a pool consuming a frontier gets the default room: 7 days of its rate", "[graph]") {
  Scenario s{.name = "frontier-pool", .version = 1, .duration = 1min};
  s.nodes = {
      client("u", Rate{100}, {{"url", 1.0}}),
      NodeSpec{
          .id = "frontier", .type = NodeType::frontier, .then = {Step{.to = "w", .op = "url"}}},
      consuming_pool("w", "frontier", 4, 5ms)};
  s.links = {LinkSpec{.from = "u", .to = "frontier"}, LinkSpec{.from = "frontier", .to = "w"}};
  const auto g = Graph::build(s);
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  REQUIRE_THAT(g->nodes()[*g->index_of("w")].capacity.queue_limit,
               WithinRel(604'800.0 * 800.0, 1e-9));
}

// ---- kv_cluster: a ring of nodes, N replicas per key (spec §19 entry 9) ----
//
// Book ch. 6 (Dynamo): `nodes` machines on a consistent-hash ring, every key kept on the N
// machines that follow its position. The graph resolves the cluster like any node of copies: its
// copies are its `nodes`, each of `capacity` (a kv_cluster's `capacity` is PER NODE, as the lab
// says), and the ring gives it an imbalance exactly as a consistent_hash balancer gives the
// nodes behind it (mark_balanced_targets: the hottest copy carries k = 1 + z_M/sqrt(N vnodes)
// times the average, M the machines, N the copies of a key). With N = nodes every node holds every
// key, so no node is hotter than another. The demand a cluster is sent is N times its requests
// (every request goes to N replicas); that is a number the paper load and the engine compute,
// pinned in test_report.cpp and test_kv.cpp, not a field of the graph.
//
// Proposed shape: Graph::build resolves the copies into ResolvedNode::spec.replicas (the count
// that explain's capacity, the engine's copy groups and the balancer's ring already read) and
// the ring into ResolvedNode::imbalance; the loader refuses `replicas` on a kv_cluster, so the
// resolved value is never one the scenario stated. `copies_of` is the one place a different
// shape (a new ResolvedNode::copies, say) would change these tests.

namespace {
// A kv_cluster `store` of `nodes` machines keeping `replication` copies of each key, behind a
// client `u` sending 1000 `get`/s over one link. `vnodes` only when given (absent: the default).
NodeSpec kv_store(int nodes, int replication, std::optional<int> vnodes = std::nullopt) {
  NodeSpec store{.id = "store", .type = NodeType::kv_cluster, .capacity = Rate{10'000}};
  store.kv_nodes = nodes;
  store.replication = replication;
  store.write_quorum = 1;
  store.read_quorum = 1;
  store.vnodes = vnodes;
  return store;
}

Scenario kv_behind_client(NodeSpec store) {
  Scenario s{.name = "kv", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{1000}, {{"get", 1.0}}), std::move(store)};
  s.links = {LinkSpec{.from = "u", .to = "store"}};
  return s;
}

// How many machines the resolved cluster is made of.
int copies_of(const simsys::graph::ResolvedNode& node) {
  return node.spec.replicas;
}

const simsys::graph::ResolvedNode& store_of(const Graph& g) {
  return g.nodes()[*g.index_of("store")];
}
}  // namespace

TEST_CASE("a kv_cluster's copies are its nodes, each of the stated per-node capacity", "[graph]") {
  const auto g = Graph::build(kv_behind_client(kv_store(8, 3)));
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  REQUIRE(copies_of(store_of(*g)) == 8);
  // `capacity` is one node's, not the cluster's: the graph does not multiply it by `nodes`
  // (explain's total capacity does: 8 x 10k/s).
  REQUIRE(store_of(*g).capacity.service_rate == Rate{10'000});
}

// One virtual-node count of 100 over a cluster of 8 whose keys live on 3 each: a machine holds
// the arcs of its 100 virtual nodes for each of the 3 copies, 300 arcs, so its share spreads
// 1/sqrt(300) around the average, and the hottest of 8 sits z_8 = 1.434 deviations above it:
// 1 + 1.434/sqrt(300) = 1.0828 times the average.
TEST_CASE(
    "a kv_cluster keeping fewer copies than it has nodes is imbalanced by its hottest machine",
    "[graph]") {
  const auto g = Graph::build(kv_behind_client(kv_store(8, 3, 100)));
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  REQUIRE_THAT(store_of(*g).imbalance, WithinRel(1.0 + blom_z8 / std::sqrt(300.0), 1e-12));
  REQUIRE_THAT(store_of(*g).imbalance, WithinRel(1.0828, 1e-4));
  REQUIRE_FALSE(store_of(*g).pooled);
}

// Without `vnodes` the ring takes model::default_vnodes, the balancer's 150: 1 + z_8/sqrt(450) =
// 1.06761 (the lab's "try 150" at 8 nodes).
TEST_CASE("a kv_cluster without vnodes takes the balancer's default of 150", "[graph]") {
  const auto g = Graph::build(kv_behind_client(kv_store(8, 3)));
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  REQUIRE_THAT(store_of(*g).imbalance, WithinRel(1.0 + blom_z8 / std::sqrt(450.0), 1e-12));
  REQUIRE_THAT(store_of(*g).imbalance, WithinRel(1.0676, 1e-4));
}

// One virtual node per machine is the worst ring: each machine holds 3 arcs, so the hottest of 8
// carries 1 + z_8/sqrt(3) = 1.828 times the average (as the consistent-hashing lab's "try
// vnodes: 1", where one copy per key and 4 servers give 2.049).
TEST_CASE("one virtual node per machine nearly doubles the hottest node's share", "[graph]") {
  const auto g = Graph::build(kv_behind_client(kv_store(8, 3, 1)));
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  REQUIRE_THAT(store_of(*g).imbalance, WithinRel(1.0 + blom_z8 / std::sqrt(3.0), 1e-12));
  REQUIRE_THAT(store_of(*g).imbalance, WithinRel(1.828, 1e-3));
}

// The cap: a machine holds a key at most once, so it is sent at most every request, k N / M <= 1.
// 3 machines, N = 2, one virtual node: 1 + z_3/sqrt(2) = 1.615 is beyond M / N = 1.5, so k = 1.5:
// the hottest machine is one of every key's two replicas.
TEST_CASE("a kv ring's hottest machine carries at most every request", "[graph]") {
  const auto g = Graph::build(kv_behind_client(kv_store(3, 2, 1)));
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  REQUIRE(store_of(*g).imbalance == 1.5);
}

// With N = nodes every node holds a replica of every key, so the load splits evenly whatever
// the ring looks like: no hot node, imbalance exactly 1 (the engine then models independent,
// identical copies), even with a single virtual node.
TEST_CASE("a kv_cluster whose replication equals its nodes has no imbalance", "[graph]") {
  const auto g = Graph::build(kv_behind_client(kv_store(3, 3, 1)));
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  REQUIRE(copies_of(store_of(*g)) == 3);
  REQUIRE(store_of(*g).imbalance == 1.0);
}

// A scenario built in code skips the loader, so Graph::build says the same words (as it does
// for a worker pool without `consumes`).
TEST_CASE("Graph::build refuses a kv_cluster built in code with an impossible shape", "[graph]") {
  const auto refused = [](NodeSpec store) {
    const auto g = Graph::build(kv_behind_client(std::move(store)));
    REQUIRE_FALSE(g.has_value());
    return g.error().message;
  };
  SECTION("no nodes") {
    NodeSpec store{.id = "store", .type = NodeType::kv_cluster};
    REQUIRE(refused(store) == "node 'store': kv_cluster needs nodes");
  }
  SECTION("replication above nodes") {
    REQUIRE(refused(kv_store(3, 5)) == "node 'store': replication must be at most nodes (5 > 3)");
  }
  SECTION("a write quorum above replication") {
    auto store = kv_store(5, 3);
    store.write_quorum = 4;
    REQUIRE(refused(store) == "node 'store': write_quorum must be at most replication (4 > 3)");
  }
  SECTION("a read quorum above replication") {
    auto store = kv_store(5, 3);
    store.read_quorum = 4;
    REQUIRE(refused(store) == "node 'store': read_quorum must be at most replication (4 > 3)");
  }
  SECTION("vnodes of zero, in the balancer's words") {
    REQUIRE(refused(kv_store(8, 3, 0)) == "node 'store': vnodes must be a positive integer");
  }
}

// ---- Search index as service sugar (spec §19 entry 10; book ch. 13) ----
//
// A search_index is not a new kind of behaviour: it is a service with no steps and the type's
// numbers unless the scenario states its own (spec section 7: "read-mostly capacity"). Graph::build
// resolves its capacity, service time and servers per copy exactly as for a service, through
// Little's law: concurrency c = capacity x service time.

namespace {
// u (10/s of `suggest`) -> trie_cache (read_through, fixed hit ratio 0.9, backend trie_db) ->
// trie_db: search-autocomplete's trie, with the numbers only when the scenario states them.
// Node indices: u 0, trie_cache 1, trie_db 2.
Scenario search_front(int replicas, std::optional<Rate> capacity = {},
                      std::optional<simsys::SimTime> latency = {}) {
  Scenario s{.name = "search-front", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{10}, {{"suggest", 1.0}}),
             cache("trie_cache", 0.9, CacheStrategy::read_through, "trie_db"),
             NodeSpec{.id = "trie_db",
                      .type = NodeType::search_index,
                      .replicas = replicas,
                      .capacity = capacity,
                      .latency = latency}};
  s.links = {LinkSpec{.from = "u", .to = "trie_cache", .dir = Direction::inout},
             LinkSpec{.from = "trie_cache", .to = "trie_db", .dir = Direction::inout}};
  return s;
}
}  // namespace

// replicas 4, capacity 2k/s, latency 5ms (search-autocomplete's trie_db): each copy keeps its
// 2k/s and 5 ms (a service's capacity is per replica), the node's total is 4 x 2k = 8k/s, and
// Little's law gives each copy c = 2000/s x 5 ms = 10 servers.
TEST_CASE(
    "a search index with stated numbers resolves as a service: 2k/s and 5 ms per replica, c 10",
    "[graph]") {
  const auto g = Graph::build(search_front(4, Rate{2000}, simsys::SimTime{5ms}));
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  const auto& index = g->nodes()[2];
  REQUIRE(index.capacity.service_rate == Rate{2000});
  REQUIRE_THAT(index.capacity.service_time_s, WithinRel(5e-3, 1e-12));
  REQUIRE(index.capacity.concurrency == 10);
  REQUIRE_THAT(index.capacity.service_rate.per_second * index.spec.replicas,
               WithinRel(8000.0, 1e-12));
  REQUIRE(index.service_rate_source == simsys::defaults::Source::scenario);
  REQUIRE(index.service_time_source == simsys::defaults::Source::scenario);
}

// With nothing stated the type's row of the defaults table stands: 5k/s and 5 ms (rate and time
// "chosen: a prefix lookup in a trie, top k read", defaults.cppm), so Little's law gives
// c = 5000 x 5 ms = 25.
TEST_CASE("a search index without capacity or latency takes the type's 5k/s and 5 ms, c 25",
          "[graph]") {
  const auto g = Graph::build(search_front(1));
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  const auto& index = g->nodes()[2];
  REQUIRE(index.capacity.service_rate == Rate{5000});
  REQUIRE_THAT(index.capacity.service_time_s, WithinRel(5e-3, 1e-12));
  REQUIRE(index.capacity.concurrency == 25);
  REQUIRE(index.service_rate_source == simsys::defaults::Source::type_default);
  REQUIRE(index.service_time_source == simsys::defaults::Source::type_default);
}

// One number stated, the other the type's: a latency of 10 ms with the default 5k/s is
// c = 5000 x 10 ms = 50, and the capacity is still tagged as the type's.
TEST_CASE("a search index states one number and takes the other from the type", "[graph]") {
  const auto g = Graph::build(search_front(1, std::nullopt, simsys::SimTime{10ms}));
  REQUIRE(g.has_value());
  const auto& index = g->nodes()[2];
  REQUIRE(index.capacity.service_rate == Rate{5000});
  REQUIRE_THAT(index.capacity.service_time_s, WithinRel(10e-3, 1e-12));
  REQUIRE(index.capacity.concurrency == 50);
  REQUIRE(index.service_rate_source == simsys::defaults::Source::type_default);
  REQUIRE(index.service_time_source == simsys::defaults::Source::scenario);
}

// ---- A ws_gateway in the graph (phase 3 Task 14; spec §8.2; book ch. 12 and ch. 15)
// ---------------
//
// A gateway holds its connections as a level that either a `connect` op reaching it fills or a
// fixed `connections` states. The graph's part is to say which ops reach a node (arriving_ops),
// and that is all the engine's rule reads: Simulation::create refuses a gateway that no `connect`
// reaches and that states no `connections`, and one that has both (tests/theory/test_gateways.cpp).
// Graph::build itself accepts such a gateway on purpose: the shipped chat-system and google-drive
// have neither until their own tasks give them one, and explain, the series and the scenario
// tests still build and describe them.

namespace {
// u (client: `connect`, `send`) -> gw (ws_gateway: send -> q, heartbeat -> presence, deliver
// terminal) -> q, presence. `connect_link_ops` filters what u -> gw carries (empty: everything).
// Node indices: u 0, gw 1, q 2, presence 3.
Scenario gateway_front(std::vector<std::string> connect_link_ops = {}) {
  Scenario s{.name = "gateway-front", .version = 1, .duration = 1min};
  NodeSpec gateway{.id = "gw",
                   .type = NodeType::ws_gateway,
                   .ops = {{"deliver", {}},
                           {"heartbeat", {Step{.to = "presence", .op = "beat"}}},
                           {"send", {Step{.to = "q", .op = "publish"}}}}};
  gateway.max_connections = 100'000;
  gateway.heartbeat = 5s;
  s.nodes = {client("u", Rate{200}, {{"connect", 0.5}, {"send", 0.5}}), std::move(gateway),
             service("q"), service("presence")};
  s.links = {LinkSpec{.from = "u", .to = "gw", .ops = std::move(connect_link_ops)},
             LinkSpec{.from = "gw", .to = "q"}, LinkSpec{.from = "gw", .to = "presence"}};
  return s;
}

bool arrives(const Graph& g, std::size_t node, std::string_view op) {
  // Is `op` among the ops that reach the node?
  return std::ranges::find(g.arriving_ops(node), op) != g.arriving_ops(node).end();
}
}  // namespace

TEST_CASE("a connect op sent straight to a gateway arrives at it, with the sends", "[graph]") {
  const auto g = Graph::build(gateway_front());
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  REQUIRE(arrives(*g, 1, "connect"));
  REQUIRE(arrives(*g, 1, "send"));
  REQUIRE(arrives(*g, 3, "beat"));  // the gateway's own heartbeat step, whoever sends heartbeats
}

TEST_CASE("a connect op sent only to another node does not arrive at the gateway", "[graph]") {
  auto s = gateway_front({"send"});  // u -> gw carries the sends only
  s.nodes.push_back(service("edge"));
  s.links.push_back(LinkSpec{.from = "u", .to = "edge", .ops = {"connect"}});
  const auto g = Graph::build(s);
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  REQUIRE_FALSE(arrives(*g, 1, "connect"));
  REQUIRE(arrives(*g, 4, "connect"));
  REQUIRE(arrives(*g, 1, "send"));
}

TEST_CASE("a connect op reaches a gateway through a load balancer", "[graph]") {
  auto s = gateway_front();
  s.nodes.push_back(load_balancer("lb", LbPolicy::round_robin));
  s.links[0] = LinkSpec{.from = "u", .to = "lb"};
  s.links.push_back(LinkSpec{.from = "lb", .to = "gw"});
  const auto g = Graph::build(s);
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  REQUIRE(arrives(*g, 1, "connect"));
}

TEST_CASE("Graph::build accepts a gateway with neither a connect op nor connections", "[graph]") {
  auto s = gateway_front();
  s.nodes[0] = client("u", Rate{200}, {{"send", 1.0}});
  const auto g = Graph::build(s);
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
}

// `deliver: []` is terminal: the op is served and sends nothing on; `send` and `heartbeat` have
// their steps, one hop each.
TEST_CASE("a gateway's deliver has no hops, its send and heartbeat one each", "[graph]") {
  const auto g = Graph::build(gateway_front());
  REQUIRE(g.has_value());
  REQUIRE(g->service_hops(1, "deliver").empty());
  REQUIRE(g->service_hops(1, "send").size() == 1);
  REQUIRE(g->service_hops(1, "send")[0].target == 2);
  REQUIRE(g->service_hops(1, "heartbeat").size() == 1);
  REQUIRE(g->service_hops(1, "heartbeat")[0].target == 3);
}
