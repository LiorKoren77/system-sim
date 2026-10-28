// The resolved graph: who sends demand to whom, in what order, through which link.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.core;
import simsys.defaults;
import simsys.graph;
import simsys.model;

using namespace std::chrono_literals;
using Catch::Matchers::WithinRel;
using simsys::Rate;
using simsys::graph::Graph;
using simsys::model::Direction;
using simsys::model::LbPolicy;
using simsys::model::LinkSpec;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
using simsys::model::Step;
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

TEST_CASE("a load balancer needs at least one outgoing link", "[graph]") {
  Scenario s{.name = "lb", .version = 1, .duration = 1min};
  s.nodes = {client("u", Rate{100}, {{"get", 1.0}}), load_balancer("lb", LbPolicy::round_robin)};
  s.links = {LinkSpec{.from = "u", .to = "lb"}};
  const auto g = Graph::build(s);
  REQUIRE_FALSE(g.has_value());
  REQUIRE(g.error().message == "node 'lb': a load balancer needs an outgoing link");
}
