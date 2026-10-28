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

TEST_CASE("consistent_hash gives its downstream an imbalance of one plus one over root vnodes",
          "[graph]") {
  Scenario s{.name = "lb", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{100}, {{"get", 1.0}}), load_balancer("lb", LbPolicy::consistent_hash),
             service("a")};
  s.nodes[1].vnodes = 100;
  s.nodes[2].replicas = 4;
  s.links = {LinkSpec{.from = "u", .to = "lb"}, LinkSpec{.from = "lb", .to = "a"}};
  const auto g = *Graph::build(s);
  // 1 + 1/sqrt(100) = 1.1: the hottest of the ring's arcs carries about that much more than
  // the average (a heuristic: the spread of a hash ring shrinks as 1/sqrt(vnodes)).
  REQUIRE_THAT(g.nodes()[*g.index_of("a")].imbalance, WithinRel(1.1, 1e-12));
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

// ---- Caches (plan 2a refinement 7, Task 10) ----

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

// A miss step needs a cache in front of it. A CDN can miss too, but it fetches its own misses
// from its origin, so a miss step after one is refused with a message of its own (below): the
// message here names the one kind of node a miss step may follow (review of 4d539b0, M3).
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

// A CDN is a cache at the network's edge (ch. 1): a fixed hit ratio on it means what it means on
// a cache. But a CDN fetches its own misses from its origin (plan 2b refinement 4), so the miss
// share 1 - h travels on the cdn -> origin edge, and a service's `on: miss` step after a CDN
// lookup, which would fetch every miss a second time, is refused, as after a read_through cache.
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

// ---- Che hit ratios resolved from the clients' key distributions (plan 2b, Task 2) ----

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

// The ring's arcs are uneven (ResolvedNode::imbalance k, 1 + 1/sqrt(vnodes)): the hottest
// replica owns k/N of the keys and the others share the rest. Each replica is an LRU cache of
// `keys` over its own arc, which for Che is one cache of keys / a over the whole space (a the
// arc's share), and it serves a of the requests: h = Σ_j a_j · Che(keys / a_j).
TEST_CASE("a cache partitioned by a consistent-hash ring sums Che over its uneven arcs",
          "[graph]") {
  const auto ring = Graph::build(replicated_che_cache(LbPolicy::consistent_hash));
  INFO((ring ? "" : ring.error().message));
  REQUIRE(ring.has_value());
  const double k = 1.0 + 1.0 / std::sqrt(150.0);  // default vnodes
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

// ---- Cache then-steps and write_back (plan 2b, Task 3) ----

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

// ---- Which keys reach a che cache: the op-aware walk (plan 2b refinement 1, revised) ----
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

// A rate limiter asks its counter store "how many requests has THIS user made?": the store is
// keyed per user (or per rule), not by the keys the client's requests are about, so the
// counter op brings no client keys to it. A che store that nothing else reaches has no key
// distribution to solve over, and the message says why, rather than "no client's demand reaches
// it": the clients' demand does, through the limiter (review of c9d5f8c). (`counter` is in the
// store's write_ops: an INCR is a write, see "a limiter's counter that its store counts as a
// read is refused" below.)
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
// different popularities, and mixing them by rate is phase 2c's.
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

// ---- Ring-partitioned caches: uneven arcs (plan 2b refinement 2, revised) ----

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
  // vnodes 1: k = 2, so the hot replica owns 2/4 of the keys (Che of 2M / 0.5 = 4M keys: 0.848)
  // and each cool one 1/6 (12M > 10M keys: every key fits, 1). h = 0.5 × 0.848 + 0.5 × 1 = 0.924,
  // below the 0.963 an even split (one 8M-key cache) would give.
  const auto built = Graph::build(ring_lab(1));
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  const auto zipf = simsys::caching::Popularity::zipf(0.9, 10e6);
  REQUIRE_THAT(*built->hit_ratio(*built->index_of("c")),
               WithinRel(0.5 * simsys::caching::che_hit_ratio(4e6, zipf) + 0.5, 1e-12));
  REQUIRE_THAT(*built->hit_ratio(*built->index_of("c")), WithinAbs(0.924118, 1e-4));
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

// ---- SQL database (plan 2b, Task 4; review of 48d4d0b) ----

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

// The database driver, not a balancer, sends writes to the primary and reads to a replica, and
// a sharding key spreads the shards evenly: a hash ring in front leaves the copies even
// (imbalance 1), so explain and the engine agree that no copy is hot (review of 48d4d0b, I1).
TEST_CASE("a hash ring imposes no hot copy on a database", "[graph]") {
  const auto g = Graph::build(database_behind_ring(true));
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  REQUIRE(g->nodes()[*g->index_of("db")].imbalance == 1.0);
}

// Two balancers whose policies give different replica models may both front a database: neither
// model applies to it (its driver picks the copy), so there is no conflict to refuse. A hash ring
// (imbalanced) and a least_connections balancer (pooled) both linked to `db` build, and `db`
// keeps even, unpooled copies (re-review of 96848f0).
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
// time (review of 48d4d0b, C3). 5k/s × 10 ms implies 50 in service; `pool: 4` allows 4, so
// 4 / 10 ms = 400/s with c = 4. A pool of 100 binds nothing: 5k/s and the implied c = 50 stay.
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
// message: both set c, and only the pool also caps the rate (re-review of 96848f0).
TEST_CASE("a database's pool and advanced.concurrency are refused together in code", "[graph]") {
  auto s = database_behind_ring(false);
  s.nodes[1].pool = 8;
  s.nodes[1].advanced.concurrency = 8;
  const auto g = Graph::build(s);
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message ==
          "node 'db': pool sets c and caps the rate by Little's law; drop advanced.concurrency");
}

// ---- Request-based CDN (plan 2b refinement 4, Task 5) ----
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

// A CDN serves static files from the edge and has no write path: there is nothing in its model
// that a write could update, so a write that reaches it is a mistake in the scenario. "A write"
// is whatever model::write_ops_of says for the node: a CDN's default is the op `write`, and a
// scenario's `write_ops` replaces it, exactly as for a cache. The message names the op and a
// node that sends it, so a student finds the arrow to remove (review of 4d539b0, M7).
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
// built in code, before it looks for the link (review of 4d539b0, M6).
TEST_CASE("a cdn built in code that is its own origin is refused", "[graph]") {
  const auto g = Graph::build(cdn_front(cdn_node(HitRatio{.fixed = 0.95}, "cdn")));
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message == "node 'cdn': a cdn cannot be its own origin");
}

// A CDN is read_through towards its origin by definition; the cache's `strategy` and `backend`
// keys are not its keys (the loader has no such key on a cdn). Written in code they would
// contradict the model: a write_back strategy, for one, would excuse the origin hop from the
// call-cycle check, although a miss always waits for the origin (review of 4d539b0, M9).
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

// Che's "filtered" popularity names the node whose misses a che cache receives, by its type: a
// CDN's misses are a CDN's, not a cache's (review of 4d539b0, M7).
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

// ---- ID generator as service sugar (plan 2b refinement 6; book ch. 7) ----
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

// ---- A write stays a write at its store (review of c9d5f8c, finding 1) ----
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
