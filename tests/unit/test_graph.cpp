// The resolved graph: who sends demand to whom, in what order, through which link.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
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
                  .workload = Workload{.base = base, .op_shares = std::move(ops)}};
}
NodeSpec service(std::string id, std::map<std::string, std::vector<Step>> ops = {}) {
  return NodeSpec{.id = std::move(id), .type = NodeType::service, .ops = std::move(ops)};
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
  Scenario s{.name = "loop", .version = 1, .duration = 1min};
  s.nodes = {service("q", {{"publish", {Step{.to = "sync", .op = "job"}}}}),
             service("sync", {{"job", {Step{.to = "chat", .op = "push"}}}}),
             service("chat", {{"msg", {Step{.to = "q", .op = "publish"}}}}),
             client("u", Rate{1}, {{"msg", 1.0}})};  // client last
  s.links = {LinkSpec{.from = "u", .to = "chat"}, LinkSpec{.from = "chat", .to = "q"},
             LinkSpec{.from = "q", .to = "sync"}, LinkSpec{.from = "sync", .to = "chat"}};
  const auto g = Graph::build(s);
  REQUIRE(g.has_value());
  const auto feedback = [](const simsys::graph::ResolvedLink& l) { return l.feedback; };
  REQUIRE(std::ranges::count_if(g->links(), feedback) == 1);
  REQUIRE(g->nodes()[std::ranges::find_if(g->links(), feedback)->from].spec.id == "sync");
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
