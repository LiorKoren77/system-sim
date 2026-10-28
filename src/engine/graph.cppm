// Module `simsys.graph`: the scenario resolved into something the engine can step —
// node capacities with provenance, link latencies and delay-line lengths, demand edges,
// a topological order, routes of the nodes that split flow (clients, load balancers),
// what each load balancer does to the replicas behind it, feedback edges for node cycles and
// asynchronous hops for call cycles. Cycles are found at two levels, for two different questions:
//  - Node level (Graph::order_nodes): which LINK closes a loop of boxes? Its flow goes
//    through a delay line of at least one step, so the forward pass can run in
//    topological order (spec §3.1: cycles use the previous step's value).
//  - Op level (detail::find_async_hops): which HOP closes a loop of CALLS, (node, op) ->
//    (node', op') -> ... -> (node, op)? A request cannot wait for itself, so that hop is
//    an asynchronous hand-off and adds no latency to its caller.
// A node cycle need not be a call cycle: with A.x -> B.y and B.z -> A.w the boxes loop
// but no call does. A hop over a node-level feedback link that closes no call cycle is
// counted in latency like any other, one step stale: the backward pass reads the
// target's response table before it is refreshed this step
// (engine::Simulation::backward_pass).
//
// Models: the arrows of a System Design Interview (Alex Xu, vol. 1) diagram. Each box of
// the book's designs (ch. 1 scaling, ch. 11 news feed, ch. 12 chat, ...) becomes a
// ResolvedNode; each arrow that carries requests becomes a DemandEdge over a
// ResolvedLink. Captured: who sends demand to whom and over which link, how long a hop
// takes (one way, from the book's datacenter round trips), how a client splits an op
// over several entry points (CDN vs origin, two load balancers), and which arrows close
// a loop (chat servers -> message queue -> sync workers -> chat servers). Ignored:
// bandwidth limits and link failures (phase 2), and any per-request routing decision —
// splits are fixed fractions, as round-robin or a weighted DNS record would give. A load
// balancer's policy is resolved here too, as a property of the node BEHIND it (plan 2a
// refinement 5, see mark_balanced_targets): whether its replicas queue apart, as one
// pool, or with a hot spot.
//
// C++20 named module, interface unit (see src/core/core.cppm for the longer
// explanation of modules versus headers).
module;
// NO <chrono>/<format>/<print>/<filesystem> here: this interface imports other modules
// (GCC 14.2 'Bad file data' bug, CLAUDE.md §3). SimTime is compared through count().
#include <algorithm>
#include <cmath>
#include <compare>
#include <cstddef>
#include <expected>
#include <initializer_list>
#include <map>
#include <optional>
#include <ranges>
#include <set>
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

// The longest delay line a link may need, in steps. A delay line holds one Flow per step
// in flight, so this bounds its memory (a million steps of a few ops is tens of MB per
// link) and keeps the step count well inside an int. A scenario that hits it almost
// always pairs a tiny step with a long hop (1 us steps over a 1 h link); a coarser step
// is the fix.
inline constexpr int max_delay_steps = 1'000'000;

// Why a scenario that loaded cleanly still cannot be simulated (a step with no link to
// travel on, a client or load-balancer op that no link carries, demand sent into a client,
// a load balancer with nowhere to send, a node behind load balancers of different policies,
// a link longer than max_delay_steps, and, for scenarios built in code, a negative latency,
// a bad weight, a non-positive vnodes or a non-positive step).
struct EngineError {
  std::string message;
};

// A scenario node plus the numbers the engine needs, each with the layer it came from.
struct ResolvedNode {
  model::NodeSpec spec{};
  fluid::Capacity capacity{};  // meaningless for clients (sources); the engine never reads it
  defaults::Source service_rate_source{defaults::Source::type_default};
  defaults::Source service_time_source{defaults::Source::type_default};
  // Set on a node that sits behind a load balancer (Graph::mark_balanced_targets): its
  // replicas form one pooled queue (least_connections), and the hottest replica carries
  // `imbalance` times the average share (consistent_hash; 1 = an even split).
  bool pooled{false};
  double imbalance{1.0};
};

// A scenario link with node indices instead of ids, and its resolved timing.
struct ResolvedLink {
  std::size_t from{};
  std::size_t to{};
  model::Direction dir{model::Direction::out};
  SimTime one_way{};
  defaults::Source latency_source{defaults::Source::dc_default};
  int delay_steps{};  // round(one_way / step), halves away from zero; >= 1 if feedback
  // Closes a node cycle: its flow arrives a step late (see Graph::mark_feedback). Says
  // nothing about latency; that is Graph::is_async_hop's question.
  bool feedback{false};
  std::vector<std::string> ops{};  // empty = all ops
  double weight{1.0};
};

// Where a splitter (a client or a load balancer) sends flow and which share goes there.
// A client's route is per op: `op` names it and `share` is that op's fraction on this
// link. A load balancer forwards whatever arrives, so its routes are per LINK: `op` is
// empty ("any op the link's filter allows") and `share` is the link's weight over all its
// outgoing links; Graph::routes_for renormalises it over the links that carry a given op.
struct Route {
  std::string op{};
  std::size_t target{};
  std::size_t link{};
  double share{};
};
// The phase-1 name, kept for the code and tests written when only clients split flow.
using ClientRoute = Route;

// A demand edge: `from` forwards flow to `to` over `link` (because of a step or a client route).
struct DemandEdge {
  std::size_t from{};
  std::size_t to{};
  std::size_t link{};
  // C++20 defaulted operator==: member-wise equality in one line, replacing a
  // hand-written comparison; used to keep one DemandEdge per (from, to, link).
  bool operator==(const DemandEdge&) const = default;
};

// A hop that closes a cycle of calls: node `from`, handling `op`, sends `target_op` to
// node `to`, and following the calls from there leads back to (from, op). The caller
// does not wait for it (see Graph::is_async_hop).
struct AsyncHop {
  std::size_t from{};
  std::string op{};
  std::size_t to{};
  std::string target_op{};
  // C++20 defaulted three-way comparison: `<=>` compares the members in declaration order
  // and the compiler derives <, <=, >, >= from it (and, since it is defaulted, == as
  // well). Replaces six hand-written operators or a std::tie comparator; it is what lets
  // the hops be sorted, deduplicated and binary-searched.
  auto operator<=>(const AsyncHop&) const = default;
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
  // Empty for a node that does not split flow (anything but a client or a load
  // balancer). An index past the last node is a programmer error (std::out_of_range).
  [[nodiscard]] std::span<const Route> routes(std::size_t node) const { return routes_.at(node); }
  [[nodiscard]] std::span<const Route> client_routes(std::size_t client) const {
    return routes(client);
  }
  // The routes of `node` that `op` takes, each with `op` filled in and its share of THAT
  // op: a client's routes for the op as they are; a load balancer's routes whose link
  // carries the op, their shares renormalised to sum to 1 over those links.
  [[nodiscard]] std::vector<Route> routes_for(std::size_t node, std::string_view op) const;
  // Every op some flow can bring to `node`, sorted: client routes and unconditional steps
  // targeting it, and whatever a load balancer in front passes on. Empty for a client.
  [[nodiscard]] std::span<const std::string> arriving_ops(std::size_t node) const {
    return arriving_ops_.at(node);
  }
  // Does node `from`, handling `op`, hand `target_op` off to node `to` without waiting?
  // True exactly for the hops that close a call cycle (see detail::find_async_hops).
  [[nodiscard]] bool is_async_hop(std::size_t from, std::string_view op, std::size_t to,
                                  std::string_view target_op) const;
  // Every asynchronous hop, sorted by (from, op, to, target_op).
  [[nodiscard]] std::span<const AsyncHop> async_hops() const { return async_hops_; }

 private:
  using Result = std::expected<void, EngineError>;

  explicit Graph(model::Scenario scenario) : scenario_{std::move(scenario)} {}

  void resolve_nodes();
  Result resolve_links();
  Result connect_steps();
  Result connect_step(std::size_t from, const model::Step& step);
  Result route_splitters();
  Result route_clients();
  Result route_client_op(std::size_t client, const std::string& op);
  Result route_balancer(std::size_t balancer);
  void collect_arriving_ops();
  Result check_balancer_ops(std::size_t balancer);
  Result mark_balanced_targets();
  void order_nodes();
  void add_edge(const DemandEdge& edge);
  void mark_feedback(std::size_t link);

  model::Scenario scenario_;
  std::vector<ResolvedNode> nodes_;
  std::vector<ResolvedLink> links_;
  std::vector<DemandEdge> edges_;
  std::vector<std::size_t> order_;
  std::vector<std::vector<Route>> routes_;              // per node index; empty for non-splitters
  std::vector<std::vector<std::string>> arriving_ops_;  // per node index, sorted
  std::vector<AsyncHop> async_hops_;                    // sorted, no duplicates
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

// How many whole steps a hop of `one_way` delays its flow (spec §3.4, refined): the
// nearest whole number of steps, halves rounded away from zero. A hop shorter than half a
// step only adds latency (the reply path accounts for it) and its flow passes through in
// the same step; a 75 ms cross-datacenter hop (half the book's 150 ms round trip) at
// 100 ms steps arrives one step late; 250 ms arrives three steps late. Rounding to the
// nearest step keeps the average arrival time honest: ceil alone would delay every 101 ms
// hop by two steps. Integer arithmetic on the nanosecond counts, so the count is exact
// for any duration (a double ratio loses digits past 2^53). Needs one_way >= 0, step > 0.
[[nodiscard]] SimTime::rep whole_delay_steps(SimTime one_way, SimTime step) {
  const auto whole = one_way.count() / step.count();
  const auto rest = one_way.count() % step.count();
  return rest >= step.count() - rest ? whole + 1 : whole;  // rest >= half a step: round up
}

// Graph::build also takes scenarios written in code (tests, generators), which the
// loader never checked, so it rejects the link numbers the loader would have rejected.
[[nodiscard]] std::optional<EngineError> invalid_link_numbers(const model::LinkSpec& spec) {
  const std::string name = "link " + spec.from + " -> " + spec.to + ": ";
  if (spec.latency && spec.latency->count() < 0) {
    return EngineError{name + "latency must not be negative"};
  }
  if (!std::isfinite(spec.weight) || spec.weight < 0) {
    return EngineError{name + "weight must be a finite non-negative number"};
  }
  return std::nullopt;
}

// Depth-first search roots for Graph::order_nodes: the clients first, then every other
// node, each group in index order. Starting from the request sources makes the back edge
// found in a loop the one that closes it as seen from the users (chat -> queue -> sync
// workers -> back to chat: sync -> chat is the feedback link). When ONE client enters the
// loop, that choice does not depend on the order the scenario file lists the nodes in.
// When several clients enter the same loop at different nodes, the first client in file
// order decides where the walk enters it, and so which link is cut; no current scenario
// has such a loop. The other nodes are roots too, so nodes no client reaches are still
// ordered.
[[nodiscard]] std::vector<std::size_t> dfs_roots(std::span<const ResolvedNode> nodes) {
  std::vector<std::size_t> roots;
  roots.reserve(nodes.size());
  for (const bool want_clients : {true, false}) {
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      if ((nodes[i].spec.type == model::NodeType::client) == want_clients) roots.push_back(i);
    }
  }
  return roots;
}

}  // namespace

// The call-graph search lives in a named, unexported namespace (module linkage) rather
// than the anonymous one: GCC 14 rejects std::map/std::vector of an internal-linkage type
// in a module interface unit ("references internal linkage entity").
namespace detail {

// A vertex of the call graph: `op` being handled at node `node`.
struct Call {
  std::size_t node{};
  std::string op{};
  auto operator<=>(const Call&) const = default;  // a std::map key (see AsyncHop)
};

// A load balancer has no `ops` steps, yet each op it receives is a call to the nodes
// behind it: (lb, op) -> (target, op) for every route that carries the op.
// Pattern: Adapter — the balancer's routes are re-expressed as the steps the search below
// already walks, keyed by the call they belong to, so the search needs no second case.
using ForwardedSteps = std::map<Call, std::vector<model::Step>>;

[[nodiscard]] ForwardedSteps balancer_steps(const Graph& g) {
  ForwardedSteps forwarded;
  for (std::size_t node = 0; node < g.nodes().size(); ++node) {
    if (g.nodes()[node].spec.type != model::NodeType::load_balancer) continue;
    for (const auto& op : g.arriving_ops(node)) {
      auto& steps = forwarded[Call{node, op}];
      for (const auto& route : g.routes_for(node, op)) {
        steps.push_back(model::Step{.to = g.nodes()[route.target].spec.id, .op = op});
      }
    }
  }
  return forwarded;
}

// The steps handling `call.op` at `call.node` runs, in declaration order; none for an op
// the node lists without steps (terminal) or does not list at all. A load balancer's are
// its forwarded steps.
[[nodiscard]] std::span<const model::Step> steps_of(const Graph& g, const ForwardedSteps& forwarded,
                                                    const Call& call) {
  if (const auto found = forwarded.find(call); found != forwarded.end()) return found->second;
  const auto& ops = g.nodes()[call.node].spec.ops;
  const auto found = ops.find(call.op);
  if (found == ops.end()) return {};
  return found->second;
}

// Roots of the call-graph search: every client route's (entry node, op), clients in index
// order and each client's routes in order (a load balancer's routes carry no op of their
// own and are reached through the calls into it), so a call cycle is cut where it closes as seen
// from the users. Then every op of every node, so a cycle no client reaches is still cut.
[[nodiscard]] std::vector<Call> call_roots(const Graph& g) {
  std::vector<Call> roots;
  for (std::size_t node = 0; node < g.nodes().size(); ++node) {
    if (g.nodes()[node].spec.type != model::NodeType::client) continue;
    for (const auto& route : g.client_routes(node)) roots.push_back({route.target, route.op});
  }
  for (std::size_t node = 0; node < g.nodes().size(); ++node) {
    // C++20 range adaptors: `map | std::views::keys` is a lazy view of each entry's key
    // (here the op name), composed with `|`. Replaces iterating the pairs and writing
    // `.first`, and copies nothing.
    for (const auto& op : g.nodes()[node].spec.ops | std::views::keys) roots.push_back({node, op});
  }
  return roots;
}

// The hops that close a call cycle: the back edges of a depth-first search over (node, op)
// vertices, with an edge (node, op) -> (step.to, step.op) for every unconditional step
// (conditional steps never fire in phase 1, see behaviours::ServiceBehaviour; `then`
// steps belong to node types phase 1 does not run), and an edge (lb, op) -> (target, op)
// for every route of a load balancer that carries the op. Same white/grey/black classification
// as Graph::order_nodes, one level finer: there the vertices are boxes, here they are
// the calls a request makes. A back edge is a call that, followed far enough, calls
// itself: chat.msg -> queue.publish -> sync.job -> chat.msg. A request cannot wait for
// its own descendant, so that hop is a fire-and-forget hand-off (the latency model
// skips it). A node loop that ends in a terminal op (sync.job -> chat.push) has no back
// edge here, so every one of its hops is waited for.
[[nodiscard]] std::vector<AsyncHop> find_async_hops(const Graph& g) {
  enum class Colour { grey, black };  // absent from the map = white (not yet visited)
  std::map<Call, Colour> colour;
  struct Frame {
    Call call;
    std::span<const model::Step> steps;
    std::size_t next_step;
  };
  const ForwardedSteps forwarded = balancer_steps(g);
  std::vector<AsyncHop> hops;
  for (const Call& root : call_roots(g)) {
    // C++20 std::map::contains: "is this key present?" in one call, replacing
    // `find(key) != end()` or `count(key) != 0`.
    if (colour.contains(root)) continue;
    colour.emplace(root, Colour::grey);
    std::vector<Frame> stack{{root, steps_of(g, forwarded, root), 0}};
    while (!stack.empty()) {
      auto& frame = stack.back();
      if (frame.next_step == frame.steps.size()) {
        colour[frame.call] = Colour::black;
        stack.pop_back();
        continue;
      }
      const model::Step& step = frame.steps[frame.next_step++];
      if (step.on != model::StepCondition::always) continue;
      Call callee{*g.index_of(step.to), step.op};  // build proved the target exists
      const auto found = colour.find(callee);
      if (found == colour.end()) {
        colour.emplace(callee, Colour::grey);
        const auto callee_steps = steps_of(g, forwarded, callee);
        stack.push_back({std::move(callee), callee_steps, 0});  // invalidates `frame`
      } else if (found->second == Colour::grey) {
        hops.push_back({frame.call.node, frame.call.op, callee.node, callee.op});
      }
    }
  }
  // Two identical steps of one op are one hop. C++20 ranges algorithms take the vector
  // itself instead of an iterator pair; std::ranges::unique returns the subrange of
  // leftovers to erase, the erase-unique idiom without begin/end bookkeeping.
  std::ranges::sort(hops);
  const auto duplicates = std::ranges::unique(hops);
  hops.erase(duplicates.begin(), duplicates.end());
  return hops;
}

}  // namespace detail

// Pattern: Static factory (named constructor) with fallible staged construction — the
// static `build` runs a sequence of named construction steps, each of which may fail
// with an EngineError, and hands out a Graph only when all succeeded. The private
// constructor makes a half-built Graph unreachable. (Not a classic Builder: callers do
// not configure the steps.)
std::expected<Graph, EngineError> Graph::build(model::Scenario scenario) {
  Graph graph{std::move(scenario)};
  graph.resolve_nodes();
  // C++23 std::expected::and_then: runs the next fallible step only if the previous one
  // succeeded and passes the first error through untouched, replacing an
  // `if (!r) return std::unexpected(r.error());` between every pair of steps.
  const auto built =
      graph.resolve_links().and_then([&] { return graph.connect_steps(); }).and_then([&] {
        return graph.route_splitters();
      });
  if (!built) return std::unexpected{built.error()};
  graph.order_nodes();
  graph.async_hops_ = detail::find_async_hops(graph);
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

bool Graph::is_async_hop(std::size_t from, std::string_view op, std::size_t to,
                         std::string_view target_op) const {
  // Binary search: the vector is sorted by AsyncHop's defaulted <=>.
  return std::ranges::binary_search(async_hops_,
                                    AsyncHop{from, std::string{op}, to, std::string{target_op}});
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
      // C++20 designated initializers: `.spec = spec` names the member it sets and leaves
      // the others at their defaults, replacing positional `ResolvedNode{spec, {}, ...}`
      // where a reader must count commas against the struct definition.
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
  arriving_ops_.resize(nodes_.size());
}

// Ids become indices; latency falls back to half the datacenter round trip
// (defaults::resolve_link_latency), cross-datacenter when the two ends sit in different DCs.
Graph::Result Graph::resolve_links() {
  if (scenario_.step.count() <= 0) return std::unexpected{EngineError{"step must be positive"}};
  for (const auto& spec : scenario_.links) {
    const auto from = index_of(spec.from);
    const auto to = index_of(spec.to);
    if (!from || !to) {
      return std::unexpected{EngineError{"link " + spec.from + " -> " + spec.to +
                                         " names a node that does not exist"}};
    }
    if (auto invalid = invalid_link_numbers(spec)) return std::unexpected{std::move(*invalid)};
    const bool cross_dc = nodes_[*from].spec.dc != nodes_[*to].spec.dc;
    const auto latency = defaults::resolve_link_latency(spec, cross_dc);
    const auto steps = whole_delay_steps(latency.value, scenario_.step);
    if (steps > max_delay_steps) {
      return std::unexpected{EngineError{
          "link " + spec.from + " -> " + spec.to + ": latency is " + std::to_string(steps) +
          " steps at this step size; at most " + std::to_string(max_delay_steps) + " steps"}};
    }
    links_.push_back(ResolvedLink{
        .from = *from,
        .to = *to,
        .dir = spec.dir,
        .one_way = latency.value,
        .latency_source = latency.source,
        .delay_steps = static_cast<int>(steps),  // fits: steps <= max_delay_steps
        .ops = spec.ops,
        .weight = spec.weight,
    });
  }
  return {};
}

// Every step of every node becomes a demand edge, over the link that carries it. Steps
// come from `ops` (per incoming op: services, gateways) and `then` (after processing:
// workers, caches...); the loader never gives a service `then` steps.
Graph::Result Graph::connect_steps() {
  for (std::size_t from = 0; from < nodes_.size(); ++from) {
    const auto& spec = nodes_[from].spec;
    // `views::values` (like `views::keys` in call_roots, but each entry's vector of steps),
    // then C++20 `views::join`, which flattens those vectors into one lazy sequence of
    // steps. Replaces two nested loops over the map and each vector; nothing is copied.
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
  // A client is a source: it has no queue and no ops to absorb demand, so flow sent to it
  // would vanish without being served or dropped.
  if (nodes_[*to].spec.type == model::NodeType::client) {
    return std::unexpected{EngineError{"node '" + sender + "' sends '" + step.op + "' to client '" +
                                       step.to + "'; clients only send"}};
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

// The nodes that split flow over their outgoing links, then what follows from those
// splits: which ops reach each node (through any chain of load balancers), whether every
// op reaching a load balancer can leave it, and what each balancer's policy does to the
// replicas behind it.
Graph::Result Graph::route_splitters() {
  if (auto routed = route_clients(); !routed) return routed;
  for (std::size_t node = 0; node < nodes_.size(); ++node) {
    if (nodes_[node].spec.type != model::NodeType::load_balancer) continue;
    if (auto routed = route_balancer(node); !routed) return routed;
  }
  collect_arriving_ops();
  for (std::size_t node = 0; node < nodes_.size(); ++node) {
    if (nodes_[node].spec.type != model::NodeType::load_balancer) continue;
    if (auto checked = check_balancer_ops(node); !checked) return checked;
  }
  return mark_balanced_targets();
}

Graph::Result Graph::route_clients() {
  for (std::size_t client = 0; client < nodes_.size(); ++client) {
    const auto& workload = nodes_[client].spec.workload;
    if (nodes_[client].spec.type != model::NodeType::client || !workload) continue;
    for (const auto& op : workload->op_shares | std::views::keys) {
      if (auto routed = route_client_op(client, op); !routed) return routed;
    }
  }
  return {};
}

// A client op goes out on every link from the client that carries it, split by the
// links' weights: share_i = weight_i / sum of weights. This is how a scenario says "90%
// of video bytes from the CDN, 10% from origin" or "3:1 between two load balancers" — a
// fixed split, as weighted DNS or round-robin would give. Weight 0 means a drained
// backend (as in the loader): it gets no share, and an op whose every link is drained
// has nowhere to go, which is an error rather than a silent equal split.
Graph::Result Graph::route_client_op(std::size_t client, const std::string& op) {
  struct Candidate {
    std::size_t link, target;
  };
  const auto& id = nodes_[client].spec.id;
  std::vector<Candidate> eligible;
  double total_weight = 0;
  for (std::size_t i = 0; i < links_.size(); ++i) {
    const auto target = demand_target(links_[i], client);
    if (!target || !carries_op(links_[i], op)) continue;
    if (nodes_[*target].spec.type == model::NodeType::client) {  // see connect_step
      return std::unexpected{EngineError{"client '" + id + "': link to client '" +
                                         nodes_[*target].spec.id + "' cannot carry op '" + op +
                                         "'; clients only send"}};
    }
    eligible.push_back({i, *target});
    total_weight += links_[i].weight;
  }
  if (eligible.empty()) {
    return std::unexpected{EngineError{"client '" + id + "': no link carries op '" + op + "'"}};
  }
  if (total_weight <= 0) {
    return std::unexpected{
        EngineError{"client '" + id + "': every link carrying op '" + op + "' has weight 0"}};
  }
  for (const auto& [link, target] : eligible) {
    const double share = links_[link].weight / total_weight;
    routes_[client].push_back(
        ClientRoute{.op = op, .target = target, .link = link, .share = share});
    add_edge(DemandEdge{.from = client, .to = target, .link = link});
  }
  return {};
}

// A load balancer forwards whatever reaches it, so it is routed per LINK, not per op: one
// Route on every link it sends demand on, its share the link's weight over the weights of
// all of them (Route's comment; routes_for narrows that to the links carrying one op). The
// split is the same for every policy: link weights spread traffic over DIFFERENT
// downstream nodes, while the policy (mark_balanced_targets) is about how the replicas
// INSIDE one downstream node share it. `weighted` is the policy that says the weights are
// the point; round robin over equal weights (the default 1) is an even split.
Graph::Result Graph::route_balancer(std::size_t balancer) {
  const auto& id = nodes_[balancer].spec.id;
  double total_weight = 0;
  for (std::size_t i = 0; i < links_.size(); ++i) {
    const auto target = demand_target(links_[i], balancer);
    if (!target) continue;
    if (nodes_[*target].spec.type == model::NodeType::client) {  // see connect_step
      return std::unexpected{EngineError{"node '" + id + "': link to client '" +
                                         nodes_[*target].spec.id +
                                         "' cannot carry demand; clients only send"}};
    }
    routes_[balancer].push_back(Route{.target = *target, .link = i, .share = links_[i].weight});
    total_weight += links_[i].weight;
    add_edge(DemandEdge{.from = balancer, .to = *target, .link = i});
  }
  if (routes_[balancer].empty()) {
    return std::unexpected{
        EngineError{"node '" + id + "': a load balancer needs an outgoing link"}};
  }
  // All weights 0 leaves every share 0; check_balancer_ops reports it per op.
  for (auto& route : routes_[balancer])
    route.share = total_weight > 0 ? route.share / total_weight : 0;
  return {};
}

std::vector<Route> Graph::routes_for(std::size_t node, std::string_view op) const {
  std::vector<Route> taken;
  if (nodes_.at(node).spec.type == model::NodeType::client) {
    for (const auto& route : routes_[node]) {
      if (route.op == op) taken.push_back(route);
    }
    return taken;
  }
  double total = 0;
  for (const auto& route : routes_[node]) {
    if (!carries_op(links_[route.link], op)) continue;
    taken.push_back(Route{
        .op = std::string{op}, .target = route.target, .link = route.link, .share = route.share});
    total += route.share;
  }
  for (auto& route : taken) route.share = total > 0 ? route.share / total : 0;
  return taken;
}

// Which ops reach every node: a worklist over (node, op) pairs, seeded with the client
// routes and the unconditional steps (the only ones that forward flow in phase 2a), and
// grown through load balancers, which pass each op they receive on to every route whose
// link carries it. Each pair enters the list once, so a loop of balancers ends.
void Graph::collect_arriving_ops() {
  std::vector<std::set<std::string>> arriving(nodes_.size());
  std::vector<std::pair<std::size_t, std::string>> work;
  const auto arrive = [&](std::size_t node, const std::string& op) {
    if (arriving[node].insert(op).second) work.emplace_back(node, op);
  };
  for (std::size_t node = 0; node < nodes_.size(); ++node) {
    const auto& spec = nodes_[node].spec;
    if (spec.type == model::NodeType::client) {
      for (const auto& route : routes_[node]) arrive(route.target, route.op);
    }
    for (const model::Step& step : spec.ops | std::views::values | std::views::join) {
      if (step.on == model::StepCondition::always) arrive(*index_of(step.to), step.op);
    }
    for (const model::Step& step : spec.then) {
      if (step.on == model::StepCondition::always) arrive(*index_of(step.to), step.op);
    }
  }
  while (!work.empty()) {
    const auto [node, op] = std::move(work.back());
    work.pop_back();
    if (nodes_[node].spec.type != model::NodeType::load_balancer) continue;
    for (const auto& route : routes_[node]) {
      if (carries_op(links_[route.link], op)) arrive(route.target, op);
    }
  }
  for (std::size_t node = 0; node < nodes_.size(); ++node) {
    arriving_ops_[node].assign(arriving[node].begin(), arriving[node].end());  // sorted: a set
  }
}

// Every op that reaches a load balancer must be able to leave it, or the balancer would
// swallow it: the same two rules as a client op (route_client_op), with the node's wording.
Graph::Result Graph::check_balancer_ops(std::size_t balancer) {
  const auto& id = nodes_[balancer].spec.id;
  for (const auto& op : arriving_ops_[balancer]) {
    double total_weight = 0;
    bool carried = false;
    for (const auto& route : routes_[balancer]) {
      if (!carries_op(links_[route.link], op)) continue;
      carried = true;
      total_weight += links_[route.link].weight;
    }
    if (!carried) {
      return std::unexpected{EngineError{"node '" + id + "': no link carries op '" + op + "'"}};
    }
    if (total_weight <= 0) {
      return std::unexpected{
          EngineError{"node '" + id + "': every link carrying op '" + op + "' has weight 0"}};
    }
  }
  return {};
}

// A load balancer's policy changes how the replicas of the node BEHIND it queue (plan 2a
// refinement 5), so it is stored on that node, where the engine builds its queue:
//  - round_robin, weighted: requests are dealt out blindly, every replica is its own
//    queue (N independent M/M/c); nothing to mark.
//  - least_connections: a request goes to the replica with the fewest in flight, so no
//    replica idles while another has a queue. Idealised as join-the-shortest-queue: the
//    replicas behave as ONE pool of N × c servers (M/M/(c·N)), `pooled`.
//  - consistent_hash with v virtual nodes per server: keys land on ring arcs of uneven
//    length, so the hottest replica holds more than 1/N of the keys. Heuristic (chosen,
//    not derived exactly): the arcs' relative spread shrinks like 1/sqrt(v) (each server's
//    share is a sum of v arcs, the Dynamo/Chord argument), so the hottest carries
//    imbalance = 1 + 1/sqrt(v) times the average (v = 100: 1.1; v = 1: 2), capped at N
//    (a single replica is the average).
// A node behind two balancers of different policies would need two queueing models at
// once, so that is an error. Two consistent_hash rings with different vnodes keep the
// larger imbalance (the hotter ring decides the hottest replica).
Graph::Result Graph::mark_balanced_targets() {
  std::vector<std::optional<model::LbPolicy>> policy_of(nodes_.size());
  for (std::size_t balancer = 0; balancer < nodes_.size(); ++balancer) {
    const auto& spec = nodes_[balancer].spec;
    if (spec.type != model::NodeType::load_balancer) continue;
    // A scenario built in code may leave the policy out (the loader always fills it in).
    const auto policy = spec.policy.value_or(model::LbPolicy::round_robin);
    const int vnodes = spec.vnodes.value_or(model::default_vnodes);
    if (vnodes <= 0) {
      return std::unexpected{
          EngineError{"node '" + spec.id + "': vnodes must be a positive integer"}};
    }
    for (const auto& route : routes_[balancer]) {
      auto& target = nodes_[route.target];
      if (policy_of[route.target] && *policy_of[route.target] != policy) {
        return std::unexpected{EngineError{"node '" + target.spec.id +
                                           "' is behind load balancers with different policies"}};
      }
      policy_of[route.target] = policy;
      if (policy == model::LbPolicy::least_connections) target.pooled = true;
      if (policy == model::LbPolicy::consistent_hash) {
        // Imbalance factor 1 + 1/sqrt(v) (see the function comment), at most N: the hottest
        // of N replicas cannot carry more than all of the demand, and one replica alone
        // carries exactly the average (factor 1).
        const double replicas = target.spec.replicas;
        const double factor =
            std::min(1.0 + 1.0 / std::sqrt(static_cast<double>(vnodes)), replicas);
        target.imbalance = std::max(target.imbalance, factor);
      }
    }
  }
  return {};
}

// Two steps to the same target over the same link are one arrow of the diagram.
void Graph::add_edge(const DemandEdge& edge) {
  if (!std::ranges::contains(edges_, edge)) edges_.push_back(edge);
}

// A cycle-closing link always delays by at least one step, so the engine reads the
// previous step's flow on it: spec §3.1 "cycles use the previous step's value". That is
// all it decides. Whether the reply path waits for a hop over it is an op-level question
// (detail::find_async_hops): chat's `sync_workers -> chat_servers` closes a node loop,
// but if the op it sends there makes no further calls, the sync worker still waits for
// that reply, read one step stale by the backward pass.
void Graph::mark_feedback(std::size_t link) {
  links_[link].feedback = true;
  links_[link].delay_steps = std::max(links_[link].delay_steps, 1);
}

// Topological order by depth-first search, Tarjan-style edge classification: a node is
// white (unvisited), grey (on the DFS path) or black (finished). An edge to a grey node is
// a back edge, and a back edge is exactly an edge that closes a cycle; marking those as
// feedback leaves a DAG, whose reverse post-order is a topological order (every node after
// the nodes that send to it). An edge into a black node (a cross or forward edge into a
// finished subtree) closes no cycle and stays an ordinary edge. Roots come from
// dfs_roots: clients first, so in the chat loop of the tests the walk goes
// u -> chat -> q -> sync and the back edge is sync -> chat, whatever the file order (one
// client enters that loop; see dfs_roots for several).
// The feedback set is inclusion-minimal, not minimum: every back edge closes a cycle made
// of the DFS tree's edges, so none can be dropped, but a different walk might cut the
// same cycles with fewer edges (finding the minimum feedback arc set is NP-hard).
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
  for (const std::size_t root : dfs_roots(nodes_)) {
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
