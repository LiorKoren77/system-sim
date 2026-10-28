// Module `simsys.graph`: the scenario resolved into something the engine can step —
// node capacities with provenance, link latencies and delay-line lengths, demand edges,
// a topological order, client routes, and feedback edges for cycles (spec §3.1: cycles
// use the previous step's value, which a one-step delay line gives us for free).
//
// Models: the arrows of a System Design Interview (Alex Xu, vol. 1) diagram. Each box of
// the book's designs (ch. 1 scaling, ch. 11 news feed, ch. 12 chat, ...) becomes a
// ResolvedNode; each arrow that carries requests becomes a DemandEdge over a
// ResolvedLink. Captured: who sends demand to whom and over which link, how long a hop
// takes (one way, from the book's datacenter round trips), how a client splits an op
// over several entry points (CDN vs origin, two load balancers), and which arrows close
// a loop (chat servers -> message queue -> sync workers -> chat servers). Ignored:
// bandwidth limits and link failures (phase 2), and any per-request routing decision —
// splits are fixed fractions, as round-robin or a weighted DNS record would give.
//
// C++20 named module, interface unit (see src/core/core.cppm for the longer
// explanation of modules versus headers).
module;
// NO <chrono>/<format>/<print>/<filesystem> here: this interface imports other modules
// (GCC 14.2 'Bad file data' bug, CLAUDE.md §3). SimTime is compared through count().
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module simsys.graph;

import simsys.core;
import simsys.defaults;
import simsys.fluid;
import simsys.model;

export namespace simsys::graph {

// Why a scenario that loaded cleanly still cannot be simulated (a step with no link to
// travel on, a client op that no link carries).
struct EngineError {
  std::string message;
};

// A scenario node plus the numbers the engine needs, each with the layer it came from.
struct ResolvedNode {
  model::NodeSpec spec{};
  fluid::Capacity capacity{};  // meaningless for clients (sources); the engine never reads it
  defaults::Source service_rate_source{defaults::Source::type_default};
  defaults::Source service_time_source{defaults::Source::type_default};
};

// A scenario link with node indices instead of ids, and its resolved timing.
struct ResolvedLink {
  std::size_t from{};
  std::size_t to{};
  model::Direction dir{model::Direction::out};
  SimTime one_way{};
  defaults::Source latency_source{defaults::Source::dc_default};
  int delay_steps{};  // round(one_way / step), halves away from zero; >= 1 if feedback
  bool feedback{false};
  std::vector<std::string> ops{};  // empty = all ops
  double weight{1.0};
};

// Where a client sends one op and which share of that op's rate goes there.
struct ClientRoute {
  std::string op{};
  std::size_t target{};
  std::size_t link{};
  double share{};
};

// A demand edge: `from` forwards flow to `to` over `link` (because of a step or a client route).
struct DemandEdge {
  std::size_t from{};
  std::size_t to{};
  std::size_t link{};
  // C++20 defaulted operator==: member-wise equality in one line, replacing a
  // hand-written comparison; used to keep one DemandEdge per (from, to, link).
  bool operator==(const DemandEdge&) const = default;
};

class Graph {
 public:
  // C++23 std::expected<T, E>: a Graph or the reason it cannot be built. Replaces a
  // throwing constructor (a bad scenario is user input, not a programmer error) and the
  // out-parameter-plus-bool idiom; the caller must look before using the value.
  [[nodiscard]] static std::expected<Graph, EngineError> build(model::Scenario scenario);

  [[nodiscard]] const model::Scenario& scenario() const { return scenario_; }
  // C++20 std::span<const T>: a non-owning view of contiguous elements (pointer + size).
  // Replaces returning `const std::vector<T>&`, which leaks the container choice into
  // the API; callers can iterate and index but neither resize nor modify.
  [[nodiscard]] std::span<const ResolvedNode> nodes() const { return nodes_; }
  [[nodiscard]] std::span<const ResolvedLink> links() const { return links_; }
  [[nodiscard]] std::span<const DemandEdge> edges() const { return edges_; }
  [[nodiscard]] std::span<const std::size_t> topological_order() const { return order_; }
  [[nodiscard]] std::optional<std::size_t> index_of(std::string_view id) const;
  // The link that carries demand from `from` to `to`: (from,to,out|inout) or (to,from,in).
  [[nodiscard]] std::optional<std::size_t> demand_link(std::size_t from, std::size_t to) const;
  // Empty for a node that is not a client. An index past the last node is a programmer
  // error (std::out_of_range).
  [[nodiscard]] std::span<const ClientRoute> client_routes(std::size_t client) const {
    return routes_.at(client);
  }

 private:
  using Result = std::expected<void, EngineError>;

  explicit Graph(model::Scenario scenario) : scenario_{std::move(scenario)} {}

  void resolve_nodes();
  Result resolve_links();
  Result connect_steps();
  Result connect_step(std::size_t from, const model::Step& step);
  Result route_clients();
  Result route_client_op(std::size_t client, const std::string& op);
  void order_nodes();
  void add_edge(const DemandEdge& edge);
  void mark_feedback(std::size_t link);

  model::Scenario scenario_;
  std::vector<ResolvedNode> nodes_;
  std::vector<ResolvedLink> links_;
  std::vector<DemandEdge> edges_;
  std::vector<std::size_t> order_;
  std::vector<std::vector<ClientRoute>> routes_;  // per node index; empty for non-clients
};

}  // namespace simsys::graph

namespace simsys::graph {

namespace {

// A link carries demand from the node on its "sending" side: `from` for out/inout,
// `to` for a link written from the receiver's side with `dir: in`.
[[nodiscard]] bool carries_demand(const ResolvedLink& link, std::size_t from, std::size_t to) {
  // C++20 `using enum`: the enumerators of Direction in scope, so the test below reads
  // `in` rather than `model::Direction::in`.
  using enum model::Direction;
  if (link.dir == in) return link.from == to && link.to == from;
  return link.from == from && link.to == to;
}

// The node a link delivers demand to when `sender` sends on it, if `sender` is its
// sending side.
[[nodiscard]] std::optional<std::size_t> demand_target(const ResolvedLink& link,
                                                       std::size_t sender) {
  if (link.dir == model::Direction::in) {
    if (link.to == sender) return link.from;
  } else if (link.from == sender) {
    return link.to;
  }
  return std::nullopt;
}

// An empty ops list means "all ops" (schema: links.ops).
[[nodiscard]] bool carries_op(const ResolvedLink& link, std::string_view op) {
  // C++23 std::ranges::contains: "is this value in the range?" in one call, replacing
  // `std::find(begin, end, value) != end`.
  return link.ops.empty() || std::ranges::contains(link.ops, op);
}

[[nodiscard]] std::string joined(const std::vector<std::string>& words, std::string_view sep) {
  std::string text;
  for (const auto& word : words) {
    if (!text.empty()) text += sep;
    text += word;
  }
  return text;
}

// How many whole steps a hop of `one_way` delays the flow (spec §3.4, refined): the
// nearest whole number of steps, halves rounded away from zero. A hop shorter than half
// a step only adds latency (the reply path accounts for it) and its flow passes through
// in the same step; a 75 ms cross-datacenter hop (half the book's 150 ms round trip) at
// 100 ms steps arrives one step late; 250 ms arrives three steps late. Rounding to the
// nearest step keeps the average arrival time honest: ceil alone would delay every
// 101 ms hop by two steps. count(): chrono's non-member operators are not visible here.
[[nodiscard]] int delay_steps_for(SimTime one_way, SimTime step) {
  const auto ratio = static_cast<double>(one_way.count()) / static_cast<double>(step.count());
  return static_cast<int>(std::llround(ratio));
}

}  // namespace

// Pattern: Factory method with staged construction — the static `build` runs a sequence
// of named construction steps, each of which may fail with an EngineError, and hands out
// a Graph only when all succeeded. The private constructor makes a half-built Graph
// unreachable. (Not a classic Builder: callers do not configure the steps.)
std::expected<Graph, EngineError> Graph::build(model::Scenario scenario) {
  Graph graph{std::move(scenario)};
  graph.resolve_nodes();
  // C++23 std::expected::and_then: runs the next fallible step only if the previous one
  // succeeded and passes the first error through untouched, replacing an
  // `if (!r) return std::unexpected(r.error());` between every pair of steps.
  const auto built =
      graph.resolve_links().and_then([&] { return graph.connect_steps(); }).and_then([&] {
        return graph.route_clients();
      });
  if (!built) return std::unexpected{built.error()};
  graph.order_nodes();
  return graph;
}

std::optional<std::size_t> Graph::index_of(std::string_view id) const {
  // C++20 ranges algorithm with a projection: compare each node's `spec.id` to `id`
  // without writing a lambda; replaces std::find_if over an iterator pair.
  const auto found = std::ranges::find(
      nodes_, id, [](const ResolvedNode& n) -> std::string_view { return n.spec.id; });
  if (found == nodes_.end()) return std::nullopt;
  return static_cast<std::size_t>(found - nodes_.begin());
}

std::optional<std::size_t> Graph::demand_link(std::size_t from, std::size_t to) const {
  for (std::size_t i = 0; i < links_.size(); ++i) {
    if (carries_demand(links_[i], from, to)) return i;
  }
  return std::nullopt;
}

// Node index = scenario order. Clients are sources, not queues, so they get no capacity.
void Graph::resolve_nodes() {
  nodes_.reserve(scenario_.nodes.size());
  for (const auto& spec : scenario_.nodes) {
    if (spec.type == model::NodeType::client) {
      nodes_.push_back(ResolvedNode{.spec = spec});
      continue;
    }
    nodes_.push_back(ResolvedNode{
        .spec = spec,
        .capacity = defaults::resolve_capacity(spec),
        .service_rate_source = defaults::resolve_service_rate(spec).source,
        .service_time_source = defaults::resolve_service_time(spec).source,
    });
  }
  routes_.resize(nodes_.size());
}

// Ids become indices; latency falls back to half the datacenter round trip
// (defaults::resolve_link_latency), cross-datacenter when the two ends sit in different DCs.
Graph::Result Graph::resolve_links() {
  if (scenario_.step.count() <= 0) return std::unexpected{EngineError{"the step must be positive"}};
  for (const auto& spec : scenario_.links) {
    const auto from = index_of(spec.from);
    const auto to = index_of(spec.to);
    if (!from || !to) {
      return std::unexpected{EngineError{"link " + spec.from + " -> " + spec.to +
                                         " names a node that does not exist"}};
    }
    const bool cross_dc = nodes_[*from].spec.dc != nodes_[*to].spec.dc;
    const auto latency = defaults::resolve_link_latency(spec, cross_dc);
    links_.push_back(ResolvedLink{
        .from = *from,
        .to = *to,
        .dir = spec.dir,
        .one_way = latency.value,
        .latency_source = latency.source,
        .delay_steps = delay_steps_for(latency.value, scenario_.step),
        .ops = spec.ops,
        .weight = spec.weight,
    });
  }
  return {};
}

// Every step of every node becomes a demand edge, over the link that carries it.
Graph::Result Graph::connect_steps() {
  for (std::size_t from = 0; from < nodes_.size(); ++from) {
    const auto& spec = nodes_[from].spec;
    // C++20 views: `views::values` sees each map entry's vector of steps, `views::join`
    // flattens those vectors into one lazy sequence of steps. Replaces two nested loops
    // over the map and each vector; nothing is copied.
    for (const model::Step& step : spec.ops | std::views::values | std::views::join) {
      if (auto connected = connect_step(from, step); !connected) return connected;
    }
    for (const model::Step& step : spec.then) {
      if (auto connected = connect_step(from, step); !connected) return connected;
    }
  }
  return {};
}

Graph::Result Graph::connect_step(std::size_t from, const model::Step& step) {
  const auto& sender = nodes_[from].spec.id;
  const auto to = index_of(step.to);
  if (!to) {
    return std::unexpected{EngineError{"node '" + sender + "' sends '" + step.op + "' to '" +
                                       step.to + "' which does not exist"}};
  }
  const auto link = demand_link(from, *to);
  if (!link) {
    return std::unexpected{EngineError{"node '" + sender + "' sends '" + step.op + "' to '" +
                                       step.to + "' but no link connects them"}};
  }
  if (!carries_op(links_[*link], step.op)) {
    return std::unexpected{EngineError{"link " + sender + " -> " + step.to +
                                       " does not carry op '" + step.op +
                                       "' (ops: " + joined(links_[*link].ops, ", ") + ")"}};
  }
  add_edge(DemandEdge{.from = from, .to = *to, .link = *link});
  return {};
}

Graph::Result Graph::route_clients() {
  for (std::size_t client = 0; client < nodes_.size(); ++client) {
    const auto& workload = nodes_[client].spec.workload;
    if (nodes_[client].spec.type != model::NodeType::client || !workload) continue;
    // std::views::keys: like views::values above, but each pair's first member (the op).
    for (const auto& op : workload->op_shares | std::views::keys) {
      if (auto routed = route_client_op(client, op); !routed) return routed;
    }
  }
  return {};
}

// A client op goes out on every link from the client that carries it, split by the
// links' weights: share_i = weight_i / sum of weights (all zero: equal shares). This is
// how a scenario says "90% of video bytes from the CDN, 10% from origin" or "3:1 between
// two load balancers" — a fixed split, as weighted DNS or round-robin would give.
Graph::Result Graph::route_client_op(std::size_t client, const std::string& op) {
  struct Candidate {
    std::size_t link, target;
  };
  std::vector<Candidate> eligible;
  double total_weight = 0;
  for (std::size_t i = 0; i < links_.size(); ++i) {
    const auto target = demand_target(links_[i], client);
    if (!target || !carries_op(links_[i], op)) continue;
    eligible.push_back({i, *target});
    total_weight += links_[i].weight;
  }
  if (eligible.empty()) {
    return std::unexpected{
        EngineError{"client '" + nodes_[client].spec.id + "': no link carries op '" + op + "'"}};
  }
  for (const auto& [link, target] : eligible) {
    const double share = total_weight > 0 ? links_[link].weight / total_weight
                                          : 1.0 / static_cast<double>(eligible.size());
    routes_[client].push_back(
        ClientRoute{.op = op, .target = target, .link = link, .share = share});
    add_edge(DemandEdge{.from = client, .to = target, .link = link});
  }
  return {};
}

// Two steps to the same target over the same link are one arrow of the diagram.
void Graph::add_edge(const DemandEdge& edge) {
  if (!std::ranges::contains(edges_, edge)) edges_.push_back(edge);
}

// A cycle-closing link always delays by at least one step, so the engine reads the
// previous step's flow on it: spec §3.1 "cycles use the previous step's value".
void Graph::mark_feedback(std::size_t link) {
  links_[link].feedback = true;
  links_[link].delay_steps = std::max(links_[link].delay_steps, 1);
}

// Topological order by depth-first search, Tarjan-style edge classification: a node is
// white (unvisited), grey (on the DFS path) or black (finished). An edge to a grey node is
// a back edge, and a back edge is exactly an edge that closes a cycle; marking those as
// feedback leaves a DAG, whose reverse post-order is a topological order (every node after
// the nodes that send to it). Starting from every node in index order makes the result
// deterministic and covers isolated nodes. In the chat loop of the tests the walk goes
// u -> chat -> q -> sync and the back edge is sync -> chat.
// Iterative (an explicit stack) rather than recursive, so a long chain cannot overflow
// the call stack.
void Graph::order_nodes() {
  std::vector<std::vector<const DemandEdge*>> outgoing(nodes_.size());
  for (const auto& edge : edges_) outgoing[edge.from].push_back(&edge);

  enum class Colour { white, grey, black };
  std::vector<Colour> colour(nodes_.size(), Colour::white);
  std::vector<std::size_t> post_order;
  post_order.reserve(nodes_.size());

  struct Frame {
    std::size_t node;
    std::size_t next_edge;
  };
  for (std::size_t root = 0; root < nodes_.size(); ++root) {
    if (colour[root] != Colour::white) continue;
    std::vector<Frame> stack{{root, 0}};
    colour[root] = Colour::grey;
    while (!stack.empty()) {
      auto& frame = stack.back();
      if (frame.next_edge == outgoing[frame.node].size()) {
        colour[frame.node] = Colour::black;
        post_order.push_back(frame.node);
        stack.pop_back();
        continue;
      }
      const DemandEdge& edge = *outgoing[frame.node][frame.next_edge++];
      if (colour[edge.to] == Colour::grey) {
        mark_feedback(edge.link);
      } else if (colour[edge.to] == Colour::white) {
        colour[edge.to] = Colour::grey;
        stack.push_back({edge.to, 0});  // invalidates `frame`; the loop re-reads back()
      }
    }
  }
  // Reverse post-order: a node finishes only after everything it sends to, so the
  // last to finish is the first in topological order.
  order_.assign(post_order.rbegin(), post_order.rend());
}

}  // namespace simsys::graph
