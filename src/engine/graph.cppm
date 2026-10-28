// Module `simsys.graph`: the scenario resolved into something the engine can step —
// node capacities with provenance, link latencies and delay-line lengths, demand edges,
// a topological order, routes of the nodes that split flow (clients, load balancers),
// what each load balancer does to the replicas behind it, feedback edges for node cycles and
// asynchronous hops for call cycles and write-back flushes. Cycles are found at two levels, for two
// different questions:
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
// refinement 5, see mark_balanced_targets): its ReplicaModel, whether its replicas queue
// apart, as one pool, or with a hot spot. A drained route (link weight 0) carries nothing,
// so it brings no op, closes no call cycle and imposes no replica model. A rate limiter
// forwards like a balancer (per link, by weight) whatever it admits, and its `backend` (the
// counter store) becomes a demand edge of its own, carrying op `counter` (plan 2a
// refinement 6, see connect_backends). A cache (plan 2a refinement 7) contributes two
// things: its hit ratio h (Graph::hit_ratio; a CDN's too), stated (`{fixed: h}`) or computed
// once here by the Che approximation from the cache size and the key distribution of the
// clients upstream (plan 2b refinements 1-2, see resolve_hit_ratios), which turns a service's
// `on: miss` step after it into a hop taken by the miss fraction 1 - h of the requests
// (Graph::service_hops), and, for the strategies in which the cache itself talks to the store,
// a demand edge to its `backend` (Graph::backend_fraction says which share of each op takes
// it; a write_back cache's flush over it is asynchronous by design, detail::write_back_flushes),
// and its `then` steps, demand edges like a service's steps whose share of each op is
// Graph::then_fraction (plan 2b refinement 5: what goes on to the next tier after the cache).
//
// C++20 named module, interface unit (see src/core/core.cppm for the longer
// explanation of modules versus headers).
module;
// NO <chrono>/<format>/<print>/<filesystem> here: this interface imports other modules
// (GCC 14.2 'Bad file data' bug, CLAUDE.md §3). SimTime is compared through count().
#include <algorithm>
#include <array>
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
#include <variant>
#include <vector>

export module simsys.graph;

import simsys.caching;
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
// a load balancer with nowhere to send, a node behind load balancers whose policies give
// its replicas different queueing models (ReplicaModel), a link longer than
// max_delay_steps, and, for scenarios built in code, a negative latency, a bad weight, a
// non-positive vnodes or a non-positive step).
struct EngineError {
  std::string message;
};

// A cache's or CDN's hit ratio h as the graph resolved it (plan 2b refinements 1-2):
// `value`, and for `che` the inputs explain prints as its provenance: the key distribution
// the clients upstream send (`keys`), those clients (`key_clients`, node indices in
// scenario order), and the cache size the approximation used (`capacity`, in keys): the
// cache's `keys`, times its replicas when a consistent-hash ring partitions the key space
// over them (`partitions`, the replica count then, 1 otherwise; see
// detail::effective_capacity). A fixed ratio has none of these.
struct ResolvedHitRatio {
  double value{};
  std::optional<model::KeySpec> keys{};
  std::vector<std::size_t> key_clients{};
  double capacity{};
  int partitions{1};
};

// A scenario node plus the numbers the engine needs, each with the layer it came from.
struct ResolvedNode {
  model::NodeSpec spec{};
  fluid::Capacity capacity{};  // meaningless for clients (sources); the engine never reads it
  defaults::Source service_rate_source{defaults::Source::type_default};
  defaults::Source service_time_source{defaults::Source::type_default};
  // Set on a node that sits behind a load balancer (Graph::mark_balanced_targets), the
  // ReplicaModel in numbers: its replicas form one pooled queue (`pooled`,
  // least_connections), or the hottest replica carries `imbalance` times the average share
  // (consistent_hash; 1 = an even split, the independent model). `keys_partitioned` is set
  // with the imbalance: a ring sends each key to one replica only, so the replicas split the
  // key space between them (a cache's replicas then hold different keys, see
  // detail::effective_capacity).
  bool pooled{false};
  double imbalance{1.0};
  bool keys_partitioned{false};
  // A rate limiter's counter store, or the store behind a cache whose strategy calls it
  // (model::needs_backend), as a node index, and the link its requests travel on
  // (Graph::connect_backends); empty for every other node, a cache_aside cache included.
  std::optional<std::size_t> backend{};
  std::optional<std::size_t> backend_link{};
  // A cache's or CDN's hit ratio, resolved once at build (Graph::resolve_hit_ratios), so every
  // later reader (miss steps, backend shares, behaviours, explain) looks it up. Empty for every
  // other node, for one the scenario left without a hit ratio, and for a `che` CDN without a
  // `keys` size (phase 2b Task 5 gives the CDN one).
  std::optional<ResolvedHitRatio> hit_ratio{};
};

// The op a rate limiter sends its counter store once per request it checks (plan 2a
// refinement 6): "read and increment this client's counter" (ch. 4's Redis INCR + EXPIRE).
inline constexpr std::string_view counter_op = "counter";

// Nodes that forward whatever reaches them over their links, split by link weight, rather
// than by per-op steps: load balancers and rate limiters (Graph::route_forwarder).
[[nodiscard]] constexpr bool forwards_by_link(model::NodeType type) {
  return type == model::NodeType::load_balancer || type == model::NodeType::rate_limiter;
}

// How the replicas of a node behind a load balancer queue (plan 2a refinement 5): what the
// balancer's policy does to them, which is all the fluid model can see of a policy.
//  - independent: each replica is its own queue at an equal share (round_robin, weighted:
//    requests are dealt out blindly);
//  - pooled: the replicas are one queue of N × the servers (least_connections);
//  - imbalanced: the hottest replica holds more than its share (consistent_hash).
// Two balancers in front of one node must agree on THIS, not on the policy's name:
// round_robin and weighted give the same independent copies.
enum class ReplicaModel { independent, pooled, imbalanced };

// Pattern: Table-driven method — the model names, printed in Graph::build's error from this
// one table (as model::lb_policy_table does for the policies).
// C++20 std::to_array: builds a std::array from a braced list and deduces its size, replacing
// `std::array<EnumName<ReplicaModel>, 3>` whose count must be kept in sync by hand.
inline constexpr auto replica_model_table = std::to_array<core::EnumName<ReplicaModel>>({
    {ReplicaModel::independent, "independent"},
    {ReplicaModel::pooled, "pooled"},
    {ReplicaModel::imbalanced, "imbalanced"},
});

[[nodiscard]] constexpr std::string_view to_string(ReplicaModel m) {
  return core::name_in(replica_model_table, m);
}

// The replica model a balancing policy gives the node behind it (see ReplicaModel).
[[nodiscard]] constexpr ReplicaModel replica_model_of(model::LbPolicy policy) {
  // C++20 `using enum`: the enumerators of LbPolicy in scope for this function only, so the
  // cases read `weighted` rather than `model::LbPolicy::weighted`; nothing leaks outside.
  using enum model::LbPolicy;
  switch (policy) {
    case least_connections:
      return ReplicaModel::pooled;
    case consistent_hash:
      return ReplicaModel::imbalanced;
    case round_robin:
    case weighted:
      break;
  }
  return ReplicaModel::independent;
}

// One step of a node's op, resolved (Graph::service_hops): the node it calls, the op it
// sends, how it combines with the other hops (`mode`), and two numbers whose product is the
// hop's flow per request (flow = rate × factor × fraction):
//   `fraction`, the share of the node's requests that take the hop at all, a probability:
//   - 1 for an unconditional step;
//   - 1 - h for an `on: miss` step, h the hit ratio of the cache (or CDN) the nearest
//     preceding unconditional step, its `lookup`, asked (ch. 1's cache, which the book calls
//     read-through and the industry cache-aside: only the misses go on to the database). A
//     cache or CDN with no number (a CDN without a hit ratio, or with `che` but no `keys`
//     size yet) counts as missing every time, the safe side for a capacity check on paper;
//   - 0 for an `on: failure` step until external sinks fail (phase 2c);
//   `factor`, the requests the hop sends per request that takes it: the step's own fan-out
//   (one post -> 100 feed writes), times the lookup's for an `on: miss` step. A request
//   that looks up k keys misses k(1 - h) of them on average (linearity of expectation, true
//   however its hits are correlated) and fetches each, so the miss step's flow is
//   rate × k × (1 - h).
// A hop of fraction f costs its caller f × the hop on average: with probability f the
// request takes it, otherwise not (behaviours::conditional_hop). The miss steps after one
// lookup (same `lookup`) are taken together: a miss pays every one of them in turn
// (behaviours::ServiceBehaviour's blocks). Assumed for latency: a request's k lookups hit or
// miss together, so a request waits for its fetches with probability 1 - h, not 1 - h^k.
struct ServiceHop {
  std::size_t target{};
  std::string op{};
  double fraction{1.0};
  model::StepMode mode{model::StepMode::sequential};
  double factor{1.0};
  // For an `on: miss` step, the position in the op's hops of the lookup it follows; empty for
  // every other step.
  std::optional<std::size_t> lookup{};
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

// Where a splitter (a client, a load balancer or a rate limiter) sends flow and which share
// goes there. A client's route is per op: `op` names it and `share` is that op's fraction on
// this link. A load balancer (or limiter) forwards whatever arrives, so its routes are per
// LINK: `op` is
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

// A hop the caller does not wait for (see Graph::is_async_hop): node `from`, handling `op`,
// sends `target_op` to node `to`, and either following the calls from there leads back to
// (from, op) (a call cycle), or the hop is a write_back cache's flush to its store.
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
  // Empty for a node that does not split flow (anything but a client, a load balancer or a
  // rate limiter; a limiter's backend link is not a route). An index past the last node is
  // a programmer error (std::out_of_range).
  [[nodiscard]] std::span<const Route> routes(std::size_t node) const { return routes_.at(node); }
  [[nodiscard]] std::span<const Route> client_routes(std::size_t client) const {
    return routes(client);
  }
  // The routes of `node` that `op` takes, each with `op` filled in and its share of THAT
  // op: a client's routes for the op as they are; a load balancer's routes whose link
  // carries the op, their shares renormalised to sum to 1 over those links.
  [[nodiscard]] std::vector<Route> routes_for(std::size_t node, std::string_view op) const;
  // Every op some flow can bring to `node`, sorted: client routes and the steps targeting
  // it that carry flow (unconditional ones, and `on: miss` ones after a cache that misses),
  // whatever a load balancer or rate limiter in front passes on, what a cache passes on to
  // its backend, and `counter` on a limiter's backend. Empty for a client.
  [[nodiscard]] std::span<const std::string> arriving_ops(std::size_t node) const {
    return arriving_ops_.at(node);
  }
  // A cache's or a CDN's hit ratio, stated or computed at build (ResolvedNode::hit_ratio);
  // nothing for any other node, and for one that has no number (see ResolvedNode::hit_ratio).
  [[nodiscard]] std::optional<double> hit_ratio(std::size_t node) const;
  // The hops `node` makes for incoming `op`, one per step in declaration order, each with
  // the share of requests that take it (ServiceHop). Empty for an op the node does not list
  // or lists without steps.
  [[nodiscard]] std::span<const ServiceHop> service_hops(std::size_t node,
                                                         std::string_view op) const;
  // The share of the requests of `op` arriving at cache `node` that the cache passes on to
  // its backend (plan 2a refinement 7), as the same op:
  //   reads (ops not in write_ops): 1 - h under read_through, else 0 (cache_aside leaves the
  //     misses to the service's `on: miss` step; under the write strategies a read miss is
  //     the application's business too);
  //   writes: 1 under write_through and write_around (synchronous), and under write_back
  //     (every write is flushed eventually: an upper bound, no coalescing; the flush is an
  //     asynchronous hop, detail::write_back_flushes), else 0 (cache_aside and read_through:
  //     the service writes the store itself).
  // 0 for any node that is not a cache with a backend edge. An unknown hit ratio counts as
  // h = 0 (every read misses), as in service_hops.
  [[nodiscard]] double backend_fraction(std::size_t node, std::string_view op) const;
  // The share of the requests of `op` that node `node` has handled which go on to its `then`
  // step `step`: 1 for an unconditional step; for an `on: miss` step after a cache or CDN,
  // 1 - h of a read (an unknown h counts as 0, as in service_hops) and 0 of a write (writes do
  // not miss); 0 for an `on: failure` step (phase 2c) and for a miss step anywhere else.
  [[nodiscard]] double then_fraction(std::size_t node, const model::Step& step,
                                     std::string_view op) const;
  // Does node `from`, handling `op`, hand `target_op` off to node `to` without waiting?
  // True exactly for the hops that close a call cycle (see detail::find_async_hops) and for a
  // write_back cache's flushes to its store (detail::write_back_flushes).
  [[nodiscard]] bool is_async_hop(std::size_t from, std::string_view op, std::size_t to,
                                  std::string_view target_op) const;
  // Every asynchronous hop, sorted by (from, op, to, target_op).
  [[nodiscard]] std::span<const AsyncHop> async_hops() const { return async_hops_; }

 private:
  using Result = std::expected<void, EngineError>;

  explicit Graph(model::Scenario scenario) : scenario_{std::move(scenario)} {}

  Result resolve_nodes();
  Result resolve_links();
  Result connect_steps();
  Result connect_step(std::size_t from, const model::Step& step);
  Result resolve_service_hops(std::size_t node);
  [[nodiscard]] std::optional<EngineError> invalid_miss_step(std::size_t node,
                                                             const model::Step& step,
                                                             const ServiceHop* lookup) const;
  [[nodiscard]] double hop_fraction(const model::Step& step,
                                    std::optional<std::size_t> looked_up) const;
  [[nodiscard]] double miss_fraction(std::size_t cache) const;
  Result connect_backends();
  Result connect_backend(std::size_t limiter);
  Result connect_cache_backend(std::size_t cache);
  Result check_cache_backend_ops(std::size_t cache);
  Result check_cache_backend_calls() const;
  Result route_splitters();
  Result resolve_hit_ratios();
  Result resolve_hops();
  Result check_routed_ops();
  Result route_clients();
  Result route_client_op(std::size_t client, const std::string& op);
  Result route_forwarder(std::size_t node);
  void collect_arriving_ops();
  [[nodiscard]] bool carries_flow(std::size_t node, const model::Step& step) const;
  Result check_forwarded_ops(std::size_t node);
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
  // Per node index: incoming op -> its resolved hops (ServiceHop).
  std::vector<std::map<std::string, std::vector<ServiceHop>, std::less<>>> service_hops_;
  std::vector<AsyncHop> async_hops_;  // sorted, no duplicates
};

}  // namespace simsys::graph

namespace simsys::graph {

namespace {

// A link carries demand from the node on its "sending" side: `from` for out/inout,
// `to` for a link written from the receiver's side with `dir: in`.
[[nodiscard]] bool carries_demand(const ResolvedLink& link, std::size_t from, std::size_t to) {
  // `using enum` (see replica_model_of): the test below reads `in`, not `model::Direction::in`.
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

// A windowed algorithm moves its counters at window boundaries, and the fluid step sees a
// boundary only where a step starts: a 150 ms window stepped every 100 ms would reset at
// 100 ms and 200 ms, a different window with a different limit x window from the one the
// scenario states. So its window (stated, or the default 1 s) must hold a whole number of
// steps, at least one. The buckets use the window only to size their default burst.
[[nodiscard]] std::optional<EngineError> window_off_the_step_grid(const model::NodeSpec& spec,
                                                                  SimTime step) {
  const auto algorithm = spec.algorithm.value_or(model::LimiterAlgorithm::token_bucket);
  if (!model::is_windowed(algorithm)) return std::nullopt;
  const SimTime window = model::limiter_window(spec);
  // On the nanosecond counts: this interface does not include <chrono> (CLAUDE.md §3), so the
  // duration operators are out of reach, as in whole_delay_steps.
  if (window.count() >= step.count() && window.count() % step.count() == 0) return std::nullopt;
  return EngineError{"node '" + spec.id +
                     "': window must be a positive whole number of steps (window " +
                     describe_duration(window) + ", step " + describe_duration(step) + ")"};
}

// Graph::build also takes rate limiters written in code, which the loader never checked:
// the same rules as config's read_limiter_* and check_leaky_bucket_keys (a limit, positive;
// burst and window positive; a burst only on a bucket; a leaky bucket states nothing but
// limit and burst, not a queue's own numbers). The loader does not know the step's role, so
// the rule that a windowed limiter's window is a whole number of steps lives only here.
[[nodiscard]] std::optional<EngineError> invalid_limiter(const model::NodeSpec& spec,
                                                         SimTime step) {
  const std::string name = "node '" + spec.id + "': ";
  if (!spec.limit) return EngineError{name + "rate_limiter needs a limit"};
  if (!(spec.limit->per_second > 0) || !std::isfinite(spec.limit->per_second)) {
    return EngineError{name + "limit must be positive"};
  }
  if (spec.burst && *spec.burst <= 0) return EngineError{name + "burst must be a positive integer"};
  if (spec.window && spec.window->count() <= 0)
    return EngineError{name + "window must be positive"};
  const auto algorithm = spec.algorithm.value_or(model::LimiterAlgorithm::token_bucket);
  if (spec.burst && model::is_windowed(algorithm)) {
    return EngineError{name + "burst applies only to token_bucket and leaky_bucket"};
  }
  const bool restates_queue = spec.capacity || spec.latency || spec.advanced.queue_limit ||
                              spec.advanced.concurrency || spec.advanced.service_variability;
  if (algorithm == model::LimiterAlgorithm::leaky_bucket && restates_queue) {
    return EngineError{name +
                       "leaky_bucket takes limit and burst; capacity, latency, concurrency, "
                       "queue_limit and service_variability are not valid"};
  }
  return window_off_the_step_grid(spec, step);
}

// Graph::build also takes caches written in code, which the loader never checked: a fixed
// hit ratio is a probability, a strategy in which the cache calls the store needs to know
// which node that is, and one in which it does not (cache_aside) must not name one: it would
// never be called (config's read_hit_ratio and read_cache_strategy). A cache with no
// hit ratio at all builds (explain can show it); the engine refuses to run it. A read_through
// cache fetches its misses from its backend itself, so a `then` step on miss would fetch each
// miss a second time (plan 2b refinement 5): the two together are refused.
[[nodiscard]] std::optional<EngineError> invalid_cache(const model::NodeSpec& spec) {
  const std::string name = "node '" + spec.id + "': ";
  if (spec.hit_ratio && spec.hit_ratio->fixed) {
    const double h = *spec.hit_ratio->fixed;
    if (!(h >= 0.0 && h <= 1.0))
      return EngineError{name + "hit_ratio.fixed must be between 0 and 1"};
  }
  if (model::needs_backend(spec.strategy) && !spec.backend) {
    return EngineError{name + "strategy " + std::string{model::to_string(spec.strategy)} +
                       " needs a backend"};
  }
  if (!model::needs_backend(spec.strategy) && spec.backend) {
    return EngineError{name + "backend is unused with strategy " +
                       std::string{model::to_string(spec.strategy)}};
  }
  const bool then_on_miss = std::ranges::any_of(
      spec.then, [](const model::Step& step) { return step.on == model::StepCondition::miss; });
  if (spec.strategy == model::CacheStrategy::read_through && then_on_miss) {
    return EngineError{name + "a then step on miss and a read_through backend both fetch the miss"};
  }
  return std::nullopt;
}

[[nodiscard]] bool is_cache(const model::NodeSpec& spec) {
  return spec.type == model::NodeType::cache;
}

// The node types whose answer an `on: miss` step can follow: they hit or miss.
[[nodiscard]] bool hits_or_misses(model::NodeType type) {
  return type == model::NodeType::cache || type == model::NodeType::cdn;
}

[[nodiscard]] bool is_write(const model::NodeSpec& cache, std::string_view op) {
  return std::ranges::contains(cache.write_ops, op);
}

// A leaky bucket (ch. 4) IS a queue: requests join a FIFO bucket that leaks at the fixed
// rate `limit`, and only a full bucket (`burst` waiting) turns requests away. So the
// limiter's own queue becomes the bucket, modelled as ONE leak slot:
//   service rate  = limit          (one request leaves every 1/limit seconds)
//   service time  = 1 / limit      (the slot is busy for exactly that interval)
//   concurrency   = 1, service_variability = 0   (one server, deterministic service)
//   queue_limit   = burst          (the bucket's room)
// so below the limit a request waits as in an M/D/1 queue (Poisson arrivals, deterministic
// leak): the Pollaczek-Khinchine wait, half the M/M/1 wait (fluid::FluidQueue::sojourn's
// Allen-Cunneen factor (1 + 0) / 2), plus the 1/limit leak interval itself. Above it the
// backlog fills and the overflow is refused (the engine reports it as `rejected`, the
// bucket's policy, not as a drop). `limit` and `burst` are the limiter's TOTALS, so with N
// replicas each copy is one slot of limit/N with room burst/N (the engine models one copy,
// see engine::Simulation::make_server). The loader and invalid_limiter refuse a `capacity`,
// `latency`, concurrency, queue limit or service variability on a leaky bucket: they would
// restate the bucket (the leak is regular by definition: variability 0). The variability of
// ARRIVALS (`advanced.burstiness`) is still the scenario's to set.
[[nodiscard]] fluid::Capacity leaky_bucket_capacity(const model::NodeSpec& spec,
                                                    fluid::Capacity capacity) {
  const double replicas = spec.replicas;
  const double leak_per_copy = spec.limit->per_second / replicas;
  capacity.service_rate = Rate{leak_per_copy};
  capacity.service_time_s = 1.0 / leak_per_copy;
  capacity.concurrency = 1;
  capacity.service_variability = 0.0;
  capacity.queue_limit = model::limiter_burst(spec) / replicas;
  return capacity;
}

[[nodiscard]] bool is_leaky_bucket(const model::NodeSpec& spec) {
  return spec.type == model::NodeType::rate_limiter &&
         spec.algorithm == model::LimiterAlgorithm::leaky_bucket;
}

// Graph::build also takes clients written in code: their `keys` obey the loader's rules
// (config's read_client_keys), which simsys.caching's Popularity factories also assume (they
// throw on a negative exponent or an empty key space: programmer errors there).
[[nodiscard]] std::optional<EngineError> invalid_client_keys(const model::NodeSpec& spec) {
  if (!spec.workload || !spec.workload->keys) return std::nullopt;
  const auto& keys = *spec.workload->keys;
  const std::string name = "node '" + spec.id + "': ";
  if (keys.zipf && !(*keys.zipf >= 0.0 && std::isfinite(*keys.zipf))) {
    return EngineError{name + "keys.zipf must be non-negative"};
  }
  if (!(keys.space >= 1.0 && std::isfinite(keys.space))) {
    return EngineError{name + "keys.space must be a positive count"};
  }
  return std::nullopt;
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

// A load balancer or a rate limiter has no `ops` steps, yet each op it receives is a call
// to the nodes behind it: (lb, op) -> (target, op) for every route that carries the op with
// a positive share (a drained route, weight 0, carries no request, so it makes no call and
// can close no call cycle); a limiter with a backend also calls (backend, counter) first.
// A cache that passes a share of an op on to its backend calls (backend, op) (write_back's
// later flush excepted: nobody waits for it, see write_back_flushes), and (target, step.op)
// along each `then` step that carries a share of the op. A node with `ops` steps calls what its
// resolved hops (Graph::service_hops) say with a positive fraction: an `on: miss` step is a
// call for the requests that miss, one that never fires (fraction 0) is none.
// Pattern: Adapter — the forwarders' routes, the caches' backend shares and the resolved
// hops are re-expressed as the steps the search below walks, keyed by the call they belong
// to, so the search needs no case per node type.
using ForwardedSteps = std::map<Call, std::vector<model::Step>>;

void add_hop_steps(const Graph& g, std::size_t node, ForwardedSteps& calls) {
  // C++20 range adaptors: `map | std::views::keys` is a lazy view of each entry's key (here
  // the op name), composed with `|`. Replaces iterating the pairs and writing `.first`, and
  // copies nothing.
  for (const auto& op : g.nodes()[node].spec.ops | std::views::keys) {
    auto& steps = calls[Call{node, op}];
    for (const ServiceHop& hop : g.service_hops(node, op)) {
      if (hop.fraction <= 0.0) continue;  // never fires: no call
      steps.push_back(model::Step{.to = g.nodes()[hop.target].spec.id, .op = hop.op});
    }
  }
}

void add_backend_steps(const Graph& g, std::size_t cache, ForwardedSteps& calls) {
  const auto& resolved = g.nodes()[cache];
  if (!resolved.backend || resolved.spec.strategy == model::CacheStrategy::write_back) return;
  for (const auto& op : g.arriving_ops(cache)) {
    if (g.backend_fraction(cache, op) <= 0.0) continue;
    calls[Call{cache, op}].push_back(
        model::Step{.to = g.nodes()[*resolved.backend].spec.id, .op = op});
  }
}

// A cache's `then` steps (plan 2b refinement 5) fire per request the cache handles: every one
// for an `on: always` step, the read misses for an `on: miss` one (Graph::then_fraction).
void add_then_steps(const Graph& g, std::size_t cache, ForwardedSteps& calls) {
  for (const auto& op : g.arriving_ops(cache)) {
    for (const model::Step& step : g.nodes()[cache].spec.then) {
      if (g.then_fraction(cache, step, op) <= 0.0) continue;  // never fires: no call
      calls[Call{cache, op}].push_back(model::Step{.to = step.to, .op = step.op});
    }
  }
}

[[nodiscard]] ForwardedSteps forwarder_steps(const Graph& g) {
  ForwardedSteps forwarded;
  for (std::size_t node = 0; node < g.nodes().size(); ++node) {
    const auto& resolved = g.nodes()[node];
    add_hop_steps(g, node, forwarded);
    if (is_cache(resolved.spec)) {
      add_backend_steps(g, node, forwarded);
      add_then_steps(g, node, forwarded);
    }
    if (!forwards_by_link(resolved.spec.type)) continue;
    for (const auto& op : g.arriving_ops(node)) {
      auto& steps = forwarded[Call{node, op}];
      if (resolved.backend) {
        steps.push_back(
            model::Step{.to = g.nodes()[*resolved.backend].spec.id, .op = std::string{counter_op}});
      }
      for (const auto& route : g.routes_for(node, op)) {
        if (route.share <= 0.0) continue;  // drained: no call
        steps.push_back(model::Step{.to = g.nodes()[route.target].spec.id, .op = op});
      }
    }
  }
  return forwarded;
}

// The calls handling `call.op` at `call.node` makes, in declaration order (forwarder_steps);
// none for an op the node lists without steps (terminal) or does not list at all.
[[nodiscard]] std::span<const model::Step> steps_of(const ForwardedSteps& forwarded,
                                                    const Call& call) {
  if (const auto found = forwarded.find(call); found != forwarded.end()) return found->second;
  return {};
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
    for (const auto& op : g.nodes()[node].spec.ops | std::views::keys) roots.push_back({node, op});
  }
  return roots;
}

// The hops that close a call cycle: the back edges of a depth-first search over (node, op)
// vertices, with an edge (node, op) -> (step.to, step.op) for every hop that fires (an
// unconditional step, or an `on: miss` step after a cache that misses; of the `then` steps
// only a cache's, the other node types that have them do not run yet), an edge (lb, op) ->
// (target, op) for every route of a load balancer that carries the op, and (cache, op) ->
// (backend, op) for what a cache passes on synchronously (forwarder_steps). Same white/grey/black
// classification as Graph::order_nodes, one level finer: there the vertices are boxes, here they
// are the calls a request makes. A back edge is a call that, followed far enough, calls itself:
// chat.msg -> queue.publish -> sync.job -> chat.msg. A request cannot wait for its own descendant,
// so that hop is a fire-and-forget hand-off (the latency model skips it). A node loop that ends in
// a terminal op (sync.job -> chat.push) has no back edge here, so every one of its hops is waited
// for.
[[nodiscard]] std::vector<AsyncHop> find_async_hops(const Graph& g) {
  enum class Colour { grey, black };  // absent from the map = white (not yet visited)
  std::map<Call, Colour> colour;
  struct Frame {
    Call call;
    std::span<const model::Step> steps;
    std::size_t next_step;
  };
  const ForwardedSteps forwarded = forwarder_steps(g);
  std::vector<AsyncHop> hops;
  for (const Call& root : call_roots(g)) {
    // C++20 std::map::contains: "is this key present?" in one call, replacing
    // `find(key) != end()` or `count(key) != 0`.
    if (colour.contains(root)) continue;
    colour.emplace(root, Colour::grey);
    std::vector<Frame> stack{{root, steps_of(forwarded, root), 0}};
    while (!stack.empty()) {
      auto& frame = stack.back();
      if (frame.next_step == frame.steps.size()) {
        colour[frame.call] = Colour::black;
        stack.pop_back();
        continue;
      }
      const model::Step& step = frame.steps[frame.next_step++];
      Call callee{*g.index_of(step.to), step.op};  // build proved the target exists
      const auto found = colour.find(callee);
      if (found == colour.end()) {
        colour.emplace(callee, Colour::grey);
        const auto callee_steps = steps_of(forwarded, callee);
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

// The flush hops of every write_back cache: (cache, write) -> (backend, write) for each write op
// that reaches it (plan 2b refinement 5). Asynchronous BY DESIGN, not because they close a call
// cycle: the cache acknowledges the write from its own memory and flushes it later, so nobody
// waits for the flush. It still forwards every write (Graph::backend_fraction is 1), so the
// store's load is unchanged; the latency model skips it like any asynchronous hop.
[[nodiscard]] std::vector<AsyncHop> write_back_flushes(const Graph& g) {
  std::vector<AsyncHop> flushes;
  for (std::size_t cache = 0; cache < g.nodes().size(); ++cache) {
    const auto& resolved = g.nodes()[cache];
    if (!is_cache(resolved.spec) || !resolved.backend) continue;
    if (resolved.spec.strategy != model::CacheStrategy::write_back) continue;
    for (const auto& op : g.arriving_ops(cache)) {
      if (g.backend_fraction(cache, op) <= 0.0) continue;  // a read: write_back sends none
      flushes.push_back({cache, op, *resolved.backend, op});
    }
  }
  return flushes;
}

// The call-cycle hops and the write-back flushes, sorted and without duplicates, as
// Graph::is_async_hop's binary search needs them.
[[nodiscard]] std::vector<AsyncHop> async_hops(const Graph& g) {
  auto hops = find_async_hops(g);
  // (C++23's std::vector::append_range would say this in one call; GCC 14's library lacks it.)
  const auto flushes = write_back_flushes(g);
  hops.insert(hops.end(), flushes.begin(), flushes.end());
  std::ranges::sort(hops);
  const auto duplicates = std::ranges::unique(hops);
  hops.erase(duplicates.begin(), duplicates.end());
  return hops;
}

}  // namespace detail

// ---- Hit ratios (plan 2b refinements 1-2) ----
//
// In `detail` (module linkage) rather than the anonymous namespace, for the reason given on
// the call-graph search above: KeySource holds a std::vector.
namespace detail {

// For every node, the clients whose demand reaches it over some chain of demand edges
// (services, balancers, limiters, caches passing on to their store, ...), in index order.
// One walk downstream from each client, so the map costs O(clients × edges) and is built once
// per graph, however many che caches ask it (Graph::resolve_hit_ratios).
[[nodiscard]] std::vector<std::vector<std::size_t>> clients_upstream(
    std::span<const ResolvedNode> nodes, std::span<const DemandEdge> edges) {
  std::vector<std::vector<std::size_t>> downstream(nodes.size());
  for (const auto& edge : edges) downstream[edge.from].push_back(edge.to);
  std::vector<std::vector<std::size_t>> upstream(nodes.size());
  for (std::size_t client = 0; client < nodes.size(); ++client) {
    if (nodes[client].spec.type != model::NodeType::client) continue;
    std::vector<bool> reached(nodes.size(), false);
    std::vector<std::size_t> work{client};
    reached[client] = true;
    while (!work.empty()) {
      const auto node = work.back();
      work.pop_back();
      for (const auto next : downstream[node]) {
        if (reached[next]) continue;
        reached[next] = true;
        upstream[next].push_back(client);
        work.push_back(next);
      }
    }
  }
  return upstream;
}

// The key distribution a che cache sees, and the clients that send it.
struct KeySource {
  model::KeySpec keys;
  std::vector<std::size_t> clients;
};

// The one key distribution the keyed clients upstream of `cache` agree on (plan 2b refinement
// 1). Clients without `keys` say nothing and are skipped; none at all, or two that disagree,
// leave the Che approximation without its popularity, an error naming the first two clients
// that disagree. (Phase 2c may mix several distributions by their rates.)
[[nodiscard]] std::expected<KeySource, EngineError> key_source(
    std::span<const ResolvedNode> nodes, std::size_t cache, std::span<const std::size_t> clients) {
  const std::string name = "node '" + nodes[cache].spec.id + "': hit_ratio che";
  std::optional<KeySource> source;
  for (const auto client : clients) {
    const auto& workload = nodes[client].spec.workload;
    if (!workload || !workload->keys) continue;
    if (!source) {
      source = KeySource{.keys = *workload->keys, .clients = {client}};
    } else if (*workload->keys != source->keys) {  // KeySpec's defaulted ==
      return std::unexpected{
          EngineError{name + ": clients '" + nodes[source->clients.front()].spec.id + "' and '" +
                      nodes[client].spec.id + "' send different key distributions"}};
    } else {
      source->clients.push_back(client);
    }
  }
  if (!source) return std::unexpected{EngineError{name + " needs a client with keys upstream"}};
  return *source;
}

// The scenario's key distribution as simsys.caching's popularity: Zipf(s) over N keys, or
// uniform over N.
[[nodiscard]] caching::Popularity popularity_of(const model::KeySpec& keys) {
  if (keys.zipf) return caching::Popularity::zipf(*keys.zipf, keys.space);
  return caching::Popularity::uniform(keys.space);
}

// Pattern: Strategy/Policy — the hit-ratio model is a compile-time policy selected at build:
// each cache's `hit_ratio` picks one of simsys.caching's HitRatioModel types, held in a
// std::variant and visited once (Graph::resolve_hit_ratios); adding a model (an LFU one, say)
// is one more alternative and one more function that builds it from the scenario.
using HitRatioPolicy = std::variant<caching::FixedHitRatio, caching::CheHitRatio>;

// The inputs resolving a hit ratio may read: the graph's nodes, which one is the cache, and
// who is upstream of it.
struct HitRatioInputs {
  std::span<const ResolvedNode> nodes;
  std::size_t cache;
  std::span<const std::size_t> clients_upstream;
};

// The cache size the Che approximation sees, in keys, and over how many replicas it is
// partitioned. A cache with N replicas holds `keys` entries per replica; what they hold
// together depends on how keys reach them:
//  - round robin, weighted, least connections: any request can reach any replica, so every
//    replica sees the whole key popularity and fills itself with the same hot keys. Each is
//    a full copy of `keys` entries, and the hit ratio is that of ONE cache of `keys`.
//  - consistent hash (ch. 5): each key hashes to exactly one replica, so the replicas hold
//    disjoint parts of the key space, `keys × N` distinct keys together. Each replica is an
//    LRU cache over a random 1/N of the keys, seeing 1/N of the requests. Counted in the
//    requests of the whole system, its Che equation is Σ_{i in its part} (1 − e^{−p_i t}) =
//    keys; each part holds about 1/N of every popularity level, so one t solves all N of
//    them, and adding the N equations gives Σ_i (1 − e^{−p_i t}) = keys × N: the equation of
//    one cache of `keys × N` over the whole space, whose hit ratio is the replicas' total.
// Assumes a random partition: the ring's uneven arcs (ResolvedNode::imbalance) change how
// many keys each replica owns, which this capacity ignores. A replica lost to a fault is not
// subtracted either: the hit ratio is resolved once, at build.
struct EffectiveCapacity {
  double keys;
  int partitions;
};
[[nodiscard]] EffectiveCapacity effective_capacity(const ResolvedNode& cache) {
  const int partitions = cache.keys_partitioned ? cache.spec.replicas : 1;
  return {.keys = static_cast<double>(*cache.spec.keys) * partitions, .partitions = partitions};
}

// What a `che` hit ratio rests on, checked: the cache size C (effective_capacity) and the key
// distribution of the clients upstream (key_source). This is also its provenance in explain.
[[nodiscard]] std::expected<ResolvedHitRatio, EngineError> che_inputs(
    const HitRatioInputs& inputs) {
  const auto& cache = inputs.nodes[inputs.cache];
  const auto& spec = cache.spec;
  if (!spec.keys) {
    return std::unexpected{EngineError{
        "node '" + spec.id + "': hit_ratio che needs keys (the cache's capacity in keys)"}};
  }
  if (*spec.keys <= 0) {  // the loader's rule (config's read_cache_size), for code
    return std::unexpected{EngineError{"node '" + spec.id + "': keys must be a positive integer"}};
  }
  const auto source = key_source(inputs.nodes, inputs.cache, inputs.clients_upstream);
  if (!source) return std::unexpected{source.error()};
  const auto capacity = effective_capacity(cache);
  return ResolvedHitRatio{.keys = source->keys,
                          .key_clients = source->clients,
                          .capacity = capacity.keys,
                          .partitions = capacity.partitions};
}

// What a cache's hit ratio rests on: for `che` its checked inputs (che_inputs); for
// `{fixed: h}` nothing, as it is the scenario author's own estimate (and a fixed cache need
// not have keyed clients upstream). Only called with `fixed` or `che` set.
[[nodiscard]] std::expected<ResolvedHitRatio, EngineError> hit_ratio_inputs(
    const model::HitRatio& stated, const HitRatioInputs& inputs) {
  if (stated.che) return che_inputs(inputs);
  return ResolvedHitRatio{};
}

// The model a cache's `hit_ratio` names, built from what it rests on: FixedHitRatio{h}, or
// CheHitRatio over the popularity of the keys the clients upstream ask for.
[[nodiscard]] HitRatioPolicy hit_ratio_model(const model::HitRatio& stated,
                                             const ResolvedHitRatio& inputs) {
  if (stated.che) return caching::CheHitRatio{.popularity = popularity_of(*inputs.keys)};
  return caching::FixedHitRatio{.value = *stated.fixed};
}

// Does `node` have a hit ratio the graph can resolve now? A cache or CDN with `{fixed: h}` or
// `che`; not a HitRatio{} built in code (neither set: no hit ratio at all), and not a `che`
// CDN without a `keys` size, which the scenario schema cannot give a CDN before phase 2b's
// Task 5 (it stays without a number and misses every time on paper, as before).
[[nodiscard]] bool has_resolvable_hit_ratio(const model::NodeSpec& spec) {
  if (!hits_or_misses(spec.type) || !spec.hit_ratio) return false;
  if (!spec.hit_ratio->che) return spec.hit_ratio->fixed.has_value();
  return spec.type == model::NodeType::cache || spec.keys.has_value();
}

}  // namespace detail

// Pattern: Static factory (named constructor) with fallible staged construction — the
// static `build` runs a sequence of named construction steps, each of which may fail
// with an EngineError, and hands out a Graph only when all succeeded. The private
// constructor makes a half-built Graph unreachable. (Not a classic Builder: callers do
// not configure the steps.)
std::expected<Graph, EngineError> Graph::build(model::Scenario scenario) {
  Graph graph{std::move(scenario)};
  // C++23 std::expected::and_then: runs the next fallible step only if the previous one
  // succeeded and passes the first error through untouched, replacing an
  // `if (!r) return std::unexpected(r.error());` between every pair of steps.
  const auto built = graph.resolve_nodes()
                         .and_then([&] { return graph.resolve_links(); })
                         .and_then([&] { return graph.connect_steps(); })
                         .and_then([&] { return graph.connect_backends(); })
                         .and_then([&] { return graph.route_splitters(); })
                         .and_then([&] { return graph.mark_balanced_targets(); })
                         .and_then([&] { return graph.resolve_hit_ratios(); })
                         .and_then([&] { return graph.resolve_hops(); })
                         .and_then([&] { return graph.check_routed_ops(); });
  if (!built) return std::unexpected{built.error()};
  graph.order_nodes();
  graph.async_hops_ = detail::async_hops(graph);
  if (auto checked = graph.check_cache_backend_calls(); !checked) {
    return std::unexpected{checked.error()};
  }
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
// A rate limiter is checked first (it needs a limit), and a leaky bucket's queue is the
// bucket itself (leaky_bucket_capacity), its rate the scenario's `limit`.
Graph::Result Graph::resolve_nodes() {
  // First: the limiter checks below divide by the step.
  if (scenario_.step.count() <= 0) return std::unexpected{EngineError{"step must be positive"}};
  nodes_.reserve(scenario_.nodes.size());
  for (const auto& spec : scenario_.nodes) {
    if (spec.type == model::NodeType::client) {
      if (auto invalid = invalid_client_keys(spec)) return std::unexpected{std::move(*invalid)};
      // C++20 designated initializers: `.spec = spec` names the member it sets and leaves
      // the others at their defaults, replacing positional `ResolvedNode{spec, {}, ...}`
      // where a reader must count commas against the struct definition.
      nodes_.push_back(ResolvedNode{.spec = spec});
      continue;
    }
    if (spec.type == model::NodeType::rate_limiter) {
      if (auto invalid = invalid_limiter(spec, scenario_.step))
        return std::unexpected{std::move(*invalid)};
    }
    if (is_cache(spec)) {
      if (auto invalid = invalid_cache(spec)) return std::unexpected{std::move(*invalid)};
    }
    ResolvedNode node{
        .spec = spec,
        .capacity = defaults::resolve_capacity(spec),
        .service_rate_source = defaults::resolve_service_rate(spec).source,
        .service_time_source = defaults::resolve_service_time(spec).source,
    };
    if (is_leaky_bucket(spec)) {
      node.capacity = leaky_bucket_capacity(spec, node.capacity);
      // Both follow from the scenario's `limit`.
      node.service_rate_source = defaults::Source::scenario;
      node.service_time_source = defaults::Source::scenario;
    }
    nodes_.push_back(std::move(node));
  }
  routes_.resize(nodes_.size());
  arriving_ops_.resize(nodes_.size());
  service_hops_.resize(nodes_.size());
  return {};
}

// Ids become indices; latency falls back to half the datacenter round trip
// (defaults::resolve_link_latency), cross-datacenter when the two ends sit in different DCs.
Graph::Result Graph::resolve_links() {
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

// Every step of every node becomes a demand edge, over the link that carries it. Steps come
// from `ops` (per incoming op: services, gateways) and `then` (after processing: workers,
// caches...); the loader never gives a service `then` steps. The steps become resolved hops
// later (resolve_hops), once the hit ratios their miss fractions need are known, and those
// need every demand edge (resolve_hit_ratios walks them upstream to the clients).
Graph::Result Graph::connect_steps() {
  for (std::size_t from = 0; from < nodes_.size(); ++from) {
    const auto& spec = nodes_[from].spec;
    // `views::values` (like `views::keys` in detail::add_hop_steps, but each entry's vector of
    // steps), then C++20 `views::join`, which flattens those vectors into one lazy sequence of
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

// Each node's op steps as resolved hops (resolve_service_hops), after resolve_hit_ratios.
Graph::Result Graph::resolve_hops() {
  for (std::size_t node = 0; node < nodes_.size(); ++node) {
    if (auto resolved = resolve_service_hops(node); !resolved) return resolved;
  }
  return {};
}

// The miss fraction 1 - h of a cache or CDN (ServiceHop's comment): 1 when there is no number
// for h (a CDN without a hit ratio or with an unsized `che`, a cache built in code without
// one), the safe side on paper.
double Graph::miss_fraction(std::size_t cache) const {
  return 1.0 - hit_ratio(cache).value_or(0.0);
}

// The share of requests that take `step` (ServiceHop's `fraction`): every request for an
// unconditional step, none for an `on: failure` step (nothing fails before phase 2c's external
// sinks), the miss fraction of `looked_up`, the cache the step's lookup asked, for an `on:
// miss` step (invalid_miss_step has made sure there is one).
double Graph::hop_fraction(const model::Step& step, std::optional<std::size_t> looked_up) const {
  // `using enum` (see replica_model_of): the conditions by their short names.
  using enum model::StepCondition;
  switch (step.on) {
    case always:
      return 1.0;
    case failure:
      return 0.0;
    case miss:
      return miss_fraction(*looked_up);
  }
  // C++23 std::unreachable(): tells the compiler control never gets here (every enumerator
  // returned above), replacing a dummy `return 0.0;` that would hide a missing case or
  // compiler-specific __builtin_unreachable(); reaching it is undefined behaviour.
  std::unreachable();
}

// Why an `on: miss` step cannot follow `lookup` (the nearest unconditional step before it, or
// nothing), if it cannot. A miss step needs something that can miss (a cache or a CDN), asked
// a read (a write updates or invalidates an entry: it has no miss to report), which leaves the
// miss to the application (a read_through cache has already fetched it from its backend, so a
// miss step would fetch it twice), and it must wait for the answer: neither it nor the lookup
// can be a parallel branch, or the response would time the fetch alongside the lookup instead
// of after it.
std::optional<EngineError> Graph::invalid_miss_step(std::size_t node, const model::Step& step,
                                                    const ServiceHop* lookup) const {
  const std::string name = "node '" + nodes_[node].spec.id + "': an 'on: miss' step ";
  if (!lookup || !hits_or_misses(nodes_[lookup->target].spec.type)) {
    return EngineError{name + "must follow a cache or cdn step"};
  }
  const auto& cache = nodes_[lookup->target].spec;
  if (is_write(cache, lookup->op)) {
    return EngineError{name + "cannot follow a write to " +
                       std::string{model::to_string(cache.type)} + " '" + cache.id +
                       "' (writes do not miss)"};
  }
  if (is_cache(cache) && cache.strategy == model::CacheStrategy::read_through) {
    return EngineError{name + "cannot follow read_through cache '" + cache.id +
                       "' (the cache already fetched the miss)"};
  }
  if (step.mode == model::StepMode::parallel || lookup->mode == model::StepMode::parallel) {
    return EngineError{name +
                       "and the cache step it follows must be sequential (the miss step waits for "
                       "the cache's answer)"};
  }
  return std::nullopt;
}

// Each op's steps in order, with the share of requests that take each and the requests each
// sends (ServiceHop). An `on: miss` step belongs to the nearest unconditional step before it,
// its lookup, which must ask a cache or a CDN for a read (invalid_miss_step):
// [cache.get, db.read on miss, cache.set on miss] is one lookup and what follows its miss.
// connect_step has already resolved every target.
Graph::Result Graph::resolve_service_hops(std::size_t node) {
  for (const auto& [op, steps] : nodes_[node].spec.ops) {
    auto& hops = service_hops_[node][op];
    std::optional<std::size_t> lookup;  // position in `hops` of the last unconditional step
    for (const model::Step& step : steps) {
      ServiceHop hop{
          .target = *index_of(step.to), .op = step.op, .mode = step.mode, .factor = step.factor};
      std::optional<std::size_t> looked_up;  // the cache a miss step's lookup asked
      if (step.on == model::StepCondition::miss) {
        const ServiceHop* asked = lookup ? &hops[*lookup] : nullptr;
        if (auto invalid = invalid_miss_step(node, step, asked)) {
          return std::unexpected{std::move(*invalid)};
        }
        looked_up = asked->target;
        hop.factor *= asked->factor;  // every missed lookup is fetched (ServiceHop)
        hop.lookup = lookup;
      }
      hop.fraction = hop_fraction(step, looked_up);
      if (step.on == model::StepCondition::always) lookup = hops.size();
      hops.push_back(std::move(hop));
    }
  }
  return {};
}

// Every cache's and CDN's hit ratio, once, before anything reads one (resolve_hops' miss
// fractions, the backend shares, the arriving ops): the stated number, or the Che
// approximation over the clients' key distribution (detail::hit_ratio_inputs and
// detail::hit_ratio_model). Runs after the last demand edge exists, since a che cache walks
// them upstream to its clients, and after mark_balanced_targets, since a ring in front
// partitions the cache's keys; the upstream map is built on the first che cache that needs
// it and shared by the rest. The model is built apart from its inputs rather than returned
// with them in one std::expected: a std::expected of a struct holding the HitRatioPolicy
// variant crashed GCC 14.2 (internal compiler error compiling simsys.behaviours, Release;
// verified 2026-10-28).
Graph::Result Graph::resolve_hit_ratios() {
  std::optional<std::vector<std::vector<std::size_t>>> upstream;
  for (std::size_t node = 0; node < nodes_.size(); ++node) {
    const auto& spec = nodes_[node].spec;
    if (!detail::has_resolvable_hit_ratio(spec)) continue;
    if (spec.hit_ratio->che && !upstream) upstream = detail::clients_upstream(nodes_, edges_);
    const detail::HitRatioInputs inputs{
        .nodes = nodes_,
        .cache = node,
        .clients_upstream = upstream ? std::span<const std::size_t>{(*upstream)[node]}
                                     : std::span<const std::size_t>{}};
    auto resolved = detail::hit_ratio_inputs(*spec.hit_ratio, inputs);
    if (!resolved) return std::unexpected{std::move(resolved.error())};
    const auto model = detail::hit_ratio_model(*spec.hit_ratio, *resolved);
    // C++20 constrained generic lambda (`const caching::HitRatioModel auto& m`): a generic
    // `auto` parameter with a concept in front, which makes the lambda a template whose
    // argument must satisfy the concept. std::visit instantiates it for every alternative of
    // the variant, so the concept does real work: it is what guarantees `m.hit_ratio(C)`
    // exists and returns a double for each model, and an alternative that is no
    // HitRatioModel fails to compile here with the concept's name in the message, instead of
    // an error deep inside std::visit.
    resolved->value = std::visit(
        [&](const caching::HitRatioModel auto& m) { return m.hit_ratio(resolved->capacity); },
        model);
    nodes_[node].hit_ratio = std::move(*resolved);
  }
  return {};
}

std::optional<double> Graph::hit_ratio(std::size_t node) const {
  const auto& resolved = nodes_.at(node).hit_ratio;
  if (!resolved) return std::nullopt;
  return resolved->value;
}

std::span<const ServiceHop> Graph::service_hops(std::size_t node, std::string_view op) const {
  const auto& by_op = service_hops_.at(node);
  const auto found = by_op.find(op);
  if (found == by_op.end()) return {};
  return found->second;
}

// Pattern: Strategy (enum-dispatched) — the cache strategy is a closed enum and this switch
// over it is the one place that says what each strategy sends to the store; CacheBehaviour,
// the paper load and the call-cycle search all read this answer.
double Graph::backend_fraction(std::size_t node, std::string_view op) const {
  const auto& resolved = nodes_.at(node);
  if (!is_cache(resolved.spec) || !resolved.backend) return 0.0;
  // `using enum` (see replica_model_of): the strategies by their short names.
  using enum model::CacheStrategy;
  const auto strategy = resolved.spec.strategy;
  if (!is_write(resolved.spec, op)) return strategy == read_through ? miss_fraction(node) : 0.0;
  const bool writes_to_store =
      strategy == write_through || strategy == write_around || strategy == write_back;
  return writes_to_store ? 1.0 : 0.0;
}

double Graph::then_fraction(std::size_t node, const model::Step& step, std::string_view op) const {
  const auto& spec = nodes_.at(node).spec;
  using enum model::StepCondition;  // see hop_fraction
  switch (step.on) {
    case always:
      return 1.0;
    case failure:
      return 0.0;
    case miss:
      if (!hits_or_misses(spec.type) || is_write(spec, op)) return 0.0;
      return miss_fraction(node);
  }
  std::unreachable();
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

// A rate limiter's `backend` is the counter store it consults once per request it checks
// (plan 2a refinement 6): a demand edge limiter -> backend carrying op `counter`, on the
// link that carries demand between them. The loader checked that the backend exists and
// is a cache; a scenario built in code is checked for existence here.
// A cache whose strategy has it call the store (model::needs_backend) gets a demand edge to
// its backend in the same way (connect_cache_backend); a cache_aside cache has none (the
// service's `on: miss` step does the store read, and invalid_cache refuses a backend on it).
Graph::Result Graph::connect_backends() {
  for (std::size_t node = 0; node < nodes_.size(); ++node) {
    const auto& spec = nodes_[node].spec;
    if (!spec.backend) continue;
    if (spec.type == model::NodeType::rate_limiter) {
      if (auto connected = connect_backend(node); !connected) return connected;
    } else if (is_cache(spec) && model::needs_backend(spec.strategy)) {
      if (auto connected = connect_cache_backend(node); !connected) return connected;
    }
  }
  return {};
}

// Which ops travel on the edge is known only once every node's arriving ops are
// (check_cache_backend_ops), and whether a request can wait for the hop only once the call
// cycles are (check_cache_backend_calls).
Graph::Result Graph::connect_cache_backend(std::size_t cache) {
  const auto& spec = nodes_[cache].spec;
  const auto backend = index_of(*spec.backend);
  if (!backend) {
    return std::unexpected{EngineError{"node '" + spec.id + "': backend refers to unknown node '" +
                                       *spec.backend + "'"}};
  }
  if (nodes_[*backend].spec.type == model::NodeType::client) {  // see connect_step
    return std::unexpected{EngineError{"node '" + spec.id + "': backend '" + *spec.backend +
                                       "' is a client; clients only send"}};
  }
  const auto link = demand_link(cache, *backend);
  if (!link) {
    return std::unexpected{EngineError{"node '" + spec.id + "': backend '" + *spec.backend +
                                       "' needs a link from the cache"}};
  }
  nodes_[cache].backend = *backend;
  nodes_[cache].backend_link = *link;
  add_edge(DemandEdge{.from = cache, .to = *backend, .link = *link});
  return {};
}

// Every op a cache passes on to its backend must be one the link carries, or the cache
// would send flow the link refuses: the same rule and message as a step's (connect_step).
Graph::Result Graph::check_cache_backend_ops(std::size_t cache) {
  const auto& resolved = nodes_[cache];
  if (!resolved.backend) return {};
  const auto& link = links_[*resolved.backend_link];
  for (const auto& op : arriving_ops_[cache]) {
    if (backend_fraction(cache, op) <= 0.0 || carries_op(link, op)) continue;
    return std::unexpected{EngineError{"link " + resolved.spec.id + " -> " +
                                       nodes_[*resolved.backend].spec.id + " does not carry op '" +
                                       op + "' (ops: " + joined(link.ops, ", ") + ")"}};
  }
  return {};
}

// A cache's fetch from its store is always waited for: a read_through miss is answered with
// what the store returns, a write_through write once the store has it. So the hop cannot be
// the one the call-cycle search cuts (detail::find_async_hops): a store that calls back into
// the cache in front of it (db.read -> cache.read -> db.read) asks a request to wait for
// itself, and the scenario is refused. (write_back's flush is not a call: nobody waits.)
Graph::Result Graph::check_cache_backend_calls() const {
  for (std::size_t cache = 0; cache < nodes_.size(); ++cache) {
    const auto& resolved = nodes_[cache];
    if (!is_cache(resolved.spec) || !resolved.backend) continue;
    if (resolved.spec.strategy == model::CacheStrategy::write_back) continue;
    for (const auto& op : arriving_ops_[cache]) {
      if (backend_fraction(cache, op) <= 0.0) continue;
      if (!is_async_hop(cache, op, *resolved.backend, op)) continue;
      return std::unexpected{EngineError{"node '" + resolved.spec.id + "': backend '" +
                                         nodes_[*resolved.backend].spec.id +
                                         "' closes a call cycle"}};
    }
  }
  return {};
}

Graph::Result Graph::connect_backend(std::size_t limiter) {
  const auto& spec = nodes_[limiter].spec;
  const auto backend = index_of(*spec.backend);
  if (!backend) {
    return std::unexpected{EngineError{"node '" + spec.id + "': backend refers to unknown node '" +
                                       *spec.backend + "'"}};
  }
  const auto link = demand_link(limiter, *backend);
  if (!link) {
    return std::unexpected{EngineError{"node '" + spec.id + "': backend '" + *spec.backend +
                                       "' needs a link from the limiter"}};
  }
  if (!carries_op(links_[*link], counter_op)) {
    return std::unexpected{
        EngineError{"link " + spec.id + " -> " + *spec.backend +
                    " does not carry op 'counter' (ops: " + joined(links_[*link].ops, ", ") + ")"}};
  }
  nodes_[limiter].backend = *backend;
  nodes_[limiter].backend_link = *link;
  add_edge(DemandEdge{.from = limiter, .to = *backend, .link = *link});
  return {};
}

// The nodes that split flow over their outgoing links: their routes, and the demand edges
// those routes add (the last ones the graph has).
Graph::Result Graph::route_splitters() {
  if (auto routed = route_clients(); !routed) return routed;
  for (std::size_t node = 0; node < nodes_.size(); ++node) {
    if (!forwards_by_link(nodes_[node].spec.type)) continue;
    if (auto routed = route_forwarder(node); !routed) return routed;
  }
  return {};
}

// What follows from the splits and the resolved hops: which ops reach each node (through any
// chain of balancers and limiters), and whether every op reaching one can leave it. (What
// each balancer's policy does to the replicas behind it is marked earlier, right after the
// splits, because a che cache's capacity depends on it: mark_balanced_targets.)
Graph::Result Graph::check_routed_ops() {
  collect_arriving_ops();
  for (std::size_t node = 0; node < nodes_.size(); ++node) {
    if (is_cache(nodes_[node].spec)) {
      if (auto checked = check_cache_backend_ops(node); !checked) return checked;
    }
    if (!forwards_by_link(nodes_[node].spec.type)) continue;
    if (auto checked = check_forwarded_ops(node); !checked) return checked;
  }
  return {};
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
// A rate limiter forwards what it admits the same way, except over its backend link, which
// carries only its own `counter` requests (connect_backend). A limiter with no other link
// is a terminal gate: a service asks it "may I?" and it answers (notification-system's
// per-user limiter); a balancer with nowhere to send is an error.
Graph::Result Graph::route_forwarder(std::size_t node) {
  const auto& id = nodes_[node].spec.id;
  const auto backend_link = nodes_[node].backend_link;
  double total_weight = 0;
  for (std::size_t i = 0; i < links_.size(); ++i) {
    const auto target = demand_target(links_[i], node);
    if (!target || i == backend_link) continue;
    if (nodes_[*target].spec.type == model::NodeType::client) {  // see connect_step
      return std::unexpected{EngineError{"node '" + id + "': link to client '" +
                                         nodes_[*target].spec.id +
                                         "' cannot carry demand; clients only send"}};
    }
    routes_[node].push_back(Route{.target = *target, .link = i, .share = links_[i].weight});
    total_weight += links_[i].weight;
    add_edge(DemandEdge{.from = node, .to = *target, .link = i});
  }
  if (routes_[node].empty() && nodes_[node].spec.type == model::NodeType::load_balancer) {
    return std::unexpected{
        EngineError{"node '" + id + "': a load balancer needs an outgoing link"}};
  }
  // All weights 0 leaves every share 0; check_forwarded_ops reports it per op.
  for (auto& route : routes_[node]) route.share = total_weight > 0 ? route.share / total_weight : 0;
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
// routes, the steps that carry flow (unconditional `then` steps, `on: miss` `then` steps of a
// cache or CDN that misses, and every resolved hop of positive fraction: an `on: miss` step
// after a cache that misses) and the `counter` op of
// every limiter's backend, and grown through load balancers and rate limiters, which pass
// each op they receive on to every route whose link carries it and is not drained (weight 0:
// no flow takes it), and through caches, which pass an op on to their backend when some of
// it goes there (backend_fraction). Each pair enters the list once, so a loop ends.
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
    for (const auto& hops : service_hops_[node] | std::views::values) {
      for (const ServiceHop& hop : hops) {
        if (hop.fraction > 0.0) arrive(hop.target, hop.op);
      }
    }
    for (const model::Step& step : spec.then) {
      if (carries_flow(node, step)) arrive(*index_of(step.to), step.op);
    }
    if (spec.type == model::NodeType::rate_limiter && nodes_[node].backend) {
      arrive(*nodes_[node].backend, std::string{counter_op});
    }
  }
  while (!work.empty()) {
    const auto [node, op] = std::move(work.back());
    work.pop_back();
    if (is_cache(nodes_[node].spec)) {
      if (backend_fraction(node, op) > 0.0) arrive(*nodes_[node].backend, op);
      continue;
    }
    if (!forwards_by_link(nodes_[node].spec.type)) continue;
    for (const auto& route : routes_[node]) {
      if (route.share > 0.0 && carries_op(links_[route.link], op)) arrive(route.target, op);
    }
  }
  for (std::size_t node = 0; node < nodes_.size(); ++node) {
    arriving_ops_[node].assign(arriving[node].begin(), arriving[node].end());  // sorted: a set
  }
}

// Can a `then` step of `node` carry flow at all? An unconditional one always; an `on: miss` one
// when the node can miss (a cache or CDN with a hit ratio below 1, or none known yet); an
// `on: failure` one not before phase 2c. Which ops it carries is then_fraction's business.
bool Graph::carries_flow(std::size_t node, const model::Step& step) const {
  if (step.on == model::StepCondition::always) return true;
  return step.on == model::StepCondition::miss && hits_or_misses(nodes_[node].spec.type) &&
         miss_fraction(node) > 0.0;
}

// Every op that reaches a load balancer must be able to leave it, or the balancer would
// swallow it: the same two rules as a client op (route_client_op), with the node's wording.
// So must every op reaching a rate limiter that forwards at all; a limiter with no route
// (a terminal gate, see route_forwarder) answers every op itself.
Graph::Result Graph::check_forwarded_ops(std::size_t node) {
  const auto& id = nodes_[node].spec.id;
  if (routes_[node].empty()) return {};  // a terminal limiter (a balancer has a route)
  for (const auto& op : arriving_ops_[node]) {
    double total_weight = 0;
    bool carried = false;
    for (const auto& route : routes_[node]) {
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
// refinement 5), so its ReplicaModel is stored on that node, where the engine builds the
// queues (engine::Simulation::make_server):
//  - round_robin, weighted -> independent: requests are dealt out blindly, every replica is
//    its own queue (N independent M/M/c); nothing to mark.
//  - least_connections -> pooled: a request goes to the replica with the fewest in flight,
//    so no replica idles while another has a queue. Idealised as join-the-shortest-queue:
//    the replicas behave as ONE pool of N × c servers (M/M/(c·N)), `pooled`.
//  - consistent_hash with v virtual nodes per server -> imbalanced: keys land on ring arcs
//    of uneven length, so some replica holds more than 1/N of the keys (book ch. 5: the
//    standard deviation of the load is about 10% of the mean at 100 vnodes, 5% at 200). The
//    engine models the HOTTEST replica apart from the cooler ones, and needs its share:
//      imbalance k = 1 + 1/sqrt(v)                 (v = 100: 1.1; v = 150: 1.082; v = 1: 2)
//    capped at N (a single replica is the average). A heuristic, chosen, not derived: each
//    server's share is a sum of v arcs, so its relative spread shrinks like 1/sqrt(v) (the
//    Dynamo/Chord argument, and the book's 10% at v = 100), and k is "the mean plus one
//    standard deviation". That is the hottest of a FEW servers; the hottest of N is an
//    extreme value, which grows with N: for roughly normal shares it sits near
//    sqrt(2 ln N) standard deviations above the mean, so for large N this k understates
//    the hot spot. A possible refinement is the N-aware k = 1 + sqrt(2 ln N)/sqrt(v).
// A drained route (weight 0) sends the target nothing and imposes no model. A node behind
// two balancers whose policies give different models would need two queueing models at
// once, so that is an error; the same model from different policies (round_robin and
// weighted) is fine. Two consistent_hash rings with different vnodes keep the larger
// imbalance (the hotter ring decides the hottest replica). A node reached both through a
// balancer and directly (a client or a step sending to it) gets the balancer's model for
// ALL its traffic: the model belongs to the replica group, not to one path into it.
Graph::Result Graph::mark_balanced_targets() {
  struct Marked {
    ReplicaModel model;
    std::size_t balancer;
  };
  std::vector<std::optional<Marked>> marked(nodes_.size());
  for (std::size_t balancer = 0; balancer < nodes_.size(); ++balancer) {
    const auto& spec = nodes_[balancer].spec;
    if (spec.type != model::NodeType::load_balancer) continue;
    // An absent policy (the loader leaves it absent, as code may) is round_robin.
    const auto replica_model = replica_model_of(spec.policy.value_or(model::LbPolicy::round_robin));
    const int vnodes = spec.vnodes.value_or(model::default_vnodes);
    if (vnodes <= 0) {
      return std::unexpected{
          EngineError{"node '" + spec.id + "': vnodes must be a positive integer"}};
    }
    for (const auto& route : routes_[balancer]) {
      if (route.share <= 0.0) continue;  // drained: sends nothing, imposes nothing
      auto& target = nodes_[route.target];
      auto& earlier = marked[route.target];
      if (earlier && earlier->model != replica_model) {
        return std::unexpected{EngineError{
            "node '" + target.spec.id +
            "' is behind load balancers with different replica models ('" +
            nodes_[earlier->balancer].spec.id + "' " + std::string{to_string(earlier->model)} +
            ", '" + spec.id + "' " + std::string{to_string(replica_model)} + ")"}};
      }
      if (!earlier) earlier = Marked{.model = replica_model, .balancer = balancer};
      if (replica_model == ReplicaModel::pooled) target.pooled = true;
      if (replica_model == ReplicaModel::imbalanced) {
        target.keys_partitioned = true;  // each key hashes to one replica
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
