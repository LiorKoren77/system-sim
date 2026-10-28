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
// clients whose ops reach it (plan 2b refinements 1-2, see resolve_hit_ratios), which turns a
// service's `on: miss` step after it into a hop taken by the miss fraction 1 - h of the requests
// (Graph::service_hops), and, for the strategies in which the cache itself talks to the store,
// a demand edge to its `backend` (Graph::backend_fraction says which share of each op takes
// it; a write_back cache's flush over it is asynchronous by design, detail::write_back_flushes),
// and its `then` steps, demand edges like a service's steps whose share of each op is
// Graph::then_fraction (plan 2b refinement 5: what goes on to the next tier after the cache).
// A CDN (plan 2b refinement 4) is the read_through shape of a cache: its `origin` becomes its
// backend (Graph::connect_origin), a demand edge carrying 1 - h of each read as the same op, no
// write may reach it (Graph::check_reads_only), and no service `on: miss` step may follow it.
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

// What resolving one cache's hit ratio reads (defined with the hit-ratio code below); declared
// here for Graph::resolve_hit_ratio. Not exported: module linkage, like everything in detail.
namespace simsys::graph::detail {
struct HitRatioInputs;
}  // namespace simsys::graph::detail

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
// the clients send (`keys`), those clients (`key_clients`, node indices in scenario order),
// and the cache size the approximation used (`capacity`, in keys): the cache's `keys`, times
// its replicas when a consistent-hash ring partitions the key space over them (`partitions`,
// the replica count then, 1 otherwise; the arcs are uneven, see detail::ring_arcs). When the
// keys arrive as the misses of another che cache, `filtered_by` names it (the popularity is
// that cache's, filtered: caching::Popularity::filtered_by); when they pass a cache with no
// characteristic time to filter by (a fixed ratio), `unfiltered_through` names it and the
// popularity is used unfiltered. `shards` is what a cache behind THIS one filters by: each
// arc's key share and characteristic time (one shard of share 1 for an unpartitioned cache).
// A fixed ratio has none of these.
struct ResolvedHitRatio {
  double value{};
  std::optional<model::KeySpec> keys{};
  std::vector<std::size_t> key_clients{};
  double capacity{};
  int partitions{1};
  std::optional<std::size_t> filtered_by{};
  std::optional<std::size_t> unfiltered_through{};
  std::vector<caching::CacheShard> shards{};
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
  // key space between them (a cache's replicas then hold different keys over uneven arcs,
  // see detail::ring_arcs).
  bool pooled{false};
  double imbalance{1.0};
  bool keys_partitioned{false};
  // A rate limiter's counter store, the store behind a cache whose strategy calls it
  // (model::needs_backend), or a CDN's origin (plan 2b refinement 4: the CDN is the read_through
  // shape of a cache, its origin the backend), as a node index, and the link its requests travel
  // on (Graph::connect_backends); empty for every other node, a cache_aside cache included.
  std::optional<std::size_t> backend{};
  std::optional<std::size_t> backend_link{};
  // A cache's or CDN's hit ratio, resolved once at build (Graph::resolve_hit_ratios), so every
  // later reader (miss steps, backend shares, behaviours, explain) looks it up. Empty for every
  // other node, for one the scenario left without a hit ratio, and for a `che` CDN without a
  // `keys` size (explain shows it as unknown; the engine refuses to run it).
  std::optional<ResolvedHitRatio> hit_ratio{};
};

// Whether `op` changes data at the node `spec` describes (it is one of its write ops,
// model::write_ops_of: a cache's `[write]`, a database's SQL statements, or the scenario's list);
// every other op is a read. One rule for the graph, the engine's database split and explain.
[[nodiscard]] bool is_write(const model::NodeSpec& spec, std::string_view op);

// Does the cache or CDN `spec` fetch its own read misses from the node behind it
// (ResolvedNode::backend), as the same op? A read_through cache does (from its `backend`), and so
// does every CDN (from its `origin`, plan 2b refinement 4). Then nothing after it may fetch them
// again (Graph::invalid_miss_step), and a read's miss share goes on to that node
// (Graph::backend_fraction). False for every other node.
[[nodiscard]] bool fetches_read_misses(const model::NodeSpec& spec);

// How a SQL database spreads its load over its copies (plan 2b refinement 3; book ch. 1,
// "database replication" and "sharding"): N = replicas copies per shard, one primary and N − 1
// read replicas, in S = shards shards. The share of the node's reads and of its writes that ONE
// copy of each kind is sent:
//                 copies        reads share        writes share
//   primary       S             0 (1/S if N = 1)   1/S
//   replica       (N − 1) S     1/((N − 1) S)      1/S   (it replays every write of its shard)
// The writes a replica replays are the ones its primary COMMITTED: the engine applies the
// replica's writes share to what the primaries drained (engine::Simulation::serve), explain to
// the paper load (where nothing is dropped, so the two agree).
struct DatabaseShares {
  double primaries{1};
  double replicas{0};  // (N − 1) S; 0 for a database without replicas
  double primary_reads{1};
  double primary_writes{1};
  double replica_reads{0};
  double replica_writes{0};
};
[[nodiscard]] DatabaseShares database_shares(const model::NodeSpec& spec);

// What ONE primary and ONE replica carry when the database is sent `reads` and `writes` (any
// unit: explain passes capacity units per second). Empty `replica` without replicas.
struct DatabaseCopyLoads {
  double primary{};
  std::optional<double> replica{};
};
[[nodiscard]] DatabaseCopyLoads database_copy_loads(const model::NodeSpec& spec, double reads,
                                                    double writes);

// Whether a database's connection pool, not its disk, bounds its throughput: Little's law caps
// a copy with `pool` connections at pool / service time, and this is below the `capacity` the
// scenario (or the type) states. Graph::build then resolves the copy to that rate and c = pool
// (apply_database_keys); explain says where the number came from.
[[nodiscard]] bool pool_binds(const ResolvedNode& node);

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
//   - 1 - h for an `on: miss` step, h the hit ratio of the cache the nearest preceding
//     unconditional step, its `lookup`, asked (ch. 1's cache, which the book calls
//     read-through and the industry cache-aside: only the misses go on to the database). A
//     cache with no number (one built in code without a hit ratio) counts as missing every
//     time, the safe side for a capacity check on paper. (A CDN fetches its own misses from its
//     origin, so no miss step may follow one: Graph::invalid_miss_step.)
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
  // its backend (plan 2a refinement 7), as the same op (for a CDN: 1 - h of a read, to its
  // origin, plan 2b refinement 4; it takes no writes):
  //   reads (ops not in write_ops): 1 - h under read_through, else 0 (cache_aside leaves the
  //     misses to the service's `on: miss` step; under the write strategies a read miss is
  //     the application's business too);
  //   writes: 1 under write_through and write_around (synchronous), and under write_back
  //     (every write is flushed eventually: an upper bound, no coalescing; the flush is an
  //     asynchronous hop, detail::write_back_flushes), else 0 (cache_aside and read_through:
  //     the service writes the store itself).
  // 0 for any node that is not a cache with a backend edge or a CDN. An unknown hit ratio counts
  // as h = 0 (every read misses), as in service_hops.
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
  Result connect_origin(std::size_t cdn);
  void attach_store(std::size_t node, std::size_t store, std::size_t link);
  Result check_reads_only(std::size_t cdn) const;
  Result check_cache_backend_ops(std::size_t cache);
  Result check_cache_backend_calls() const;
  Result route_splitters();
  Result resolve_hit_ratios();
  Result resolve_hit_ratio(std::size_t cache, const model::HitRatio& stated,
                           const detail::HitRatioInputs& inputs);
  Result resolve_hops();
  Result check_routed_ops();
  Result route_clients();
  Result route_client_op(std::size_t client, const std::string& op);
  Result route_forwarder(std::size_t node);
  void collect_arriving_ops();
  [[nodiscard]] bool carries_flow(std::size_t node, const model::Step& step,
                                  std::string_view op) const;
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

[[nodiscard]] bool is_cache(const model::NodeSpec& spec) {
  return spec.type == model::NodeType::cache;
}

// The node types whose answer an `on: miss` step can follow: they hit or miss.
[[nodiscard]] bool hits_or_misses(model::NodeType type) {
  return type == model::NodeType::cache || type == model::NodeType::cdn;
}

// Pattern: Strategy (enum-dispatched) — the cache strategy is a closed enum and this is the one
// place that says which ops each strategy sends to the store (its backend hop, as the same op):
// a read under read_through (its misses), a write under write_through, write_around and
// write_back (every one; write_back's later and asynchronously), nothing else. Graph::
// backend_fraction (how much), the key walk (detail::add_backend_hop) and the then-step check
// below (invalid_then_steps) all ask it.
// A CDN behaves as a read_through cache here (fetches_read_misses), and no write reaches it
// (Graph::check_reads_only).
[[nodiscard]] bool sends_to_store(const model::NodeSpec& cache, std::string_view op) {
  // `using enum` (see replica_model_of): the strategies by their short names.
  using enum model::CacheStrategy;
  if (!is_write(cache, op)) return fetches_read_misses(cache);
  return cache.strategy == write_through || cache.strategy == write_around ||
         cache.strategy == write_back;
}

// Does the cache or CDN forward its own misses (a `then` step on miss)? Then nothing after it
// may fetch them again (Graph::invalid_miss_step).
[[nodiscard]] bool forwards_own_misses(const model::NodeSpec& spec) {
  // C++20 ranges algorithm: std::ranges::any_of takes the container itself instead of a
  // begin/end pair (and a projection if wanted), replacing std::any_of(v.begin(), v.end(), p).
  return std::ranges::any_of(
      spec.then, [](const model::Step& step) { return step.on == model::StepCondition::miss; });
}

// Would `step`, a `then` step of `cache`, send its store the very request its backend hop sends
// (the same node, an op sends_to_store says the hop carries)? Every such request would reach the
// store twice, and under write_back the step would be taken for the asynchronous flush.
[[nodiscard]] bool duplicates_backend_hop(const model::NodeSpec& cache, const model::Step& step) {
  return cache.backend && step.to == *cache.backend && sends_to_store(cache, step.op);
}

// A cache's `then` steps (plan 2b refinement 5), checked against what the cache already does:
//  - a step on miss beside a read_through backend: both would fetch the miss;
//  - a step on miss in parallel: a miss waits for its then steps (the cache answers it only once
//    the next tier has), so the step cannot be timed beside the lookup, as a service's miss step
//    cannot (Graph::invalid_miss_step);
//  - a step that duplicates the backend hop (duplicates_backend_hop).
[[nodiscard]] std::optional<EngineError> invalid_then_steps(const model::NodeSpec& spec) {
  const std::string name = "node '" + spec.id + "': ";
  for (const model::Step& step : spec.then) {
    const bool on_miss = step.on == model::StepCondition::miss;
    if (on_miss && fetches_read_misses(spec)) {
      return EngineError{name +
                         "a then step on miss and a read_through backend both fetch the miss"};
    }
    if (on_miss && step.mode == model::StepMode::parallel) {
      return EngineError{name + "a then step on miss must be sequential (a miss waits for it)"};
    }
    if (duplicates_backend_hop(spec, step)) {
      return EngineError{name + "then step to '" + step.to + "' duplicates its backend hop"};
    }
  }
  return std::nullopt;
}

// A fixed hit ratio is a probability (config's read_hit_ratio), for a cache and a CDN alike.
[[nodiscard]] std::optional<EngineError> invalid_fixed_hit_ratio(const model::NodeSpec& spec) {
  if (!spec.hit_ratio || !spec.hit_ratio->fixed) return std::nullopt;
  const double h = *spec.hit_ratio->fixed;
  if (h >= 0.0 && h <= 1.0) return std::nullopt;
  return EngineError{"node '" + spec.id + "': hit_ratio.fixed must be between 0 and 1"};
}

// Graph::build also takes caches written in code, which the loader never checked: a fixed
// hit ratio is a probability, a strategy in which the cache calls the store needs to know
// which node that is, and one in which it does not (cache_aside) must not name one: it would
// never be called (config's read_hit_ratio and read_cache_strategy). A cache with no
// hit ratio at all builds (explain can show it); the engine refuses to run it. Its `then` steps
// must agree with its strategy and its backend (invalid_then_steps).
[[nodiscard]] std::optional<EngineError> invalid_cache(const model::NodeSpec& spec) {
  const std::string name = "node '" + spec.id + "': ";
  if (auto invalid = invalid_fixed_hit_ratio(spec)) return invalid;
  if (model::needs_backend(spec.strategy) && !spec.backend) {
    return EngineError{name + "strategy " + std::string{model::to_string(spec.strategy)} +
                       " needs a backend"};
  }
  if (!model::needs_backend(spec.strategy) && spec.backend) {
    return EngineError{name + "backend is unused with strategy " +
                       std::string{model::to_string(spec.strategy)}};
  }
  return invalid_then_steps(spec);
}

// Graph::build also takes CDNs written in code, which the loader never checked (config's
// read_cdn_keys): a fixed hit ratio is a probability, and a CDN needs an origin to send its
// misses to (whether that is a server is connect_origin's check). The loader gives a CDN no
// `then` steps, and none may be written in code: the CDN fetches its own misses, so a then step
// on miss would fetch them twice, and an unconditional one has no place in an edge cache's model.
[[nodiscard]] std::optional<EngineError> invalid_cdn(const model::NodeSpec& spec) {
  const std::string name = "node '" + spec.id + "': ";
  if (auto invalid = invalid_fixed_hit_ratio(spec)) return invalid;
  if (!spec.origin) return EngineError{name + "a cdn needs an origin"};
  if (!spec.then.empty()) {
    return EngineError{name + "a cdn has no then steps (it fetches its misses from its origin)"};
  }
  return std::nullopt;
}

// A database answers every query itself and calls nobody (behaviours::SqlDbBehaviour), so a link
// that carries demand out of one (from it, or written from the other end with `dir: in`) would
// carry nothing: refused, as the loader refuses it (config's check_link_sender), with its message.
[[nodiscard]] std::optional<EngineError> invalid_link_from_database(const model::LinkSpec& link,
                                                                    const model::NodeSpec& from,
                                                                    const model::NodeSpec& to) {
  const auto& sender = link.dir == model::Direction::in ? to : from;
  if (sender.type != model::NodeType::sql_db) return std::nullopt;
  return EngineError{"node '" + sender.id + "': a database calls nobody"};
}

// Graph::build also takes databases written in code: their `shards`, `pool` and
// `replication_lag` obey the loader's rules (config's read_sql_keys), with its messages.
[[nodiscard]] std::optional<EngineError> invalid_database(const model::NodeSpec& spec) {
  const std::string name = "node '" + spec.id + "': ";
  if (spec.shards && *spec.shards < 1)
    return EngineError{name + "shards must be a positive integer"};
  if (spec.pool && *spec.pool < 1) return EngineError{name + "pool must be a positive integer"};
  if (spec.pool && *spec.pool > defaults::max_concurrency) {
    return EngineError{name + "pool must be at most " + std::to_string(defaults::max_concurrency)};
  }
  if (spec.pool && spec.advanced.concurrency) {
    return EngineError{name +
                       "pool sets c and caps the rate by Little's law; drop advanced.concurrency"};
  }
  if (spec.replication_lag && spec.replication_lag->count() < 0) {
    return EngineError{name + "replication_lag must not be negative"};
  }
  return std::nullopt;
}

// What a database's own keys do to its resolved numbers (plan 2b refinement 3): `pool` is the
// connection pool, how many queries one copy (primary or replica alike) has in service at once.
// The rates below are capacity units, like `capacity`: a query holds its connection for its
// cost × the service time, so a write at the default cost of 5 holds one five times as long as a
// read, and the pool caps units per second, not requests.
// A copy already has a concurrency, the c Little's law implies from its capacity × service time
// (defaults::implied_concurrency: 5k/s × 10 ms keeps 50 queries in service). A pool below it
// leaves connections, not the disk, as the limit:
//   c            = min(pool, implied c)
//   service rate = min(capacity, pool / service time)
// so 4 connections of 10 ms queries serve at most 400/s however fast the disk. A pool at or above
// the implied c changes nothing: more connections never make the disk faster. The two lines
// keep the queue one consistent machine (fluid::Capacity: the drain uses the rate, Erlang C
// uses c and the service time, and rate = c / service time when the pool binds).
// (Its `write_ops` need nothing here: model::write_ops_of gives an absent list the database
// default, default_sql_write_ops, wherever it is read.)
void apply_database_keys(ResolvedNode& node) {
  if (!node.spec.pool) return;
  const int pool = *node.spec.pool;
  auto& capacity = node.capacity;
  capacity.concurrency = std::min(pool, capacity.concurrency);
  // Little's law: concurrency in service = throughput × service time, so `pool` queries in
  // service for service_time_s each leave at pool / service_time_s per second. (A zero service
  // time gives +inf, which the min turns back into the capacity.)
  const double pool_rate = static_cast<double>(pool) / capacity.service_time_s;
  capacity.service_rate = Rate{std::min(capacity.service_rate.per_second, pool_rate)};
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
    return EngineError{name + "keys.zipf must be a finite non-negative number"};
  }
  if (!(keys.space >= 1.0 && std::isfinite(keys.space))) {
    return EngineError{name + "keys.space must be a positive count"};
  }
  return std::nullopt;
}

}  // namespace

bool is_write(const model::NodeSpec& spec, std::string_view op) {
  return std::ranges::contains(model::write_ops_of(spec), op);
}

bool fetches_read_misses(const model::NodeSpec& spec) {
  if (spec.type == model::NodeType::cdn) return true;
  return is_cache(spec) && spec.strategy == model::CacheStrategy::read_through;
}

// The table at DatabaseShares. Without replicas the S primaries share reads and writes alike.
DatabaseShares database_shares(const model::NodeSpec& spec) {
  const double copies_per_shard = spec.replicas;
  const double shards = spec.shards.value_or(1);
  if (copies_per_shard < 2.0) {
    return DatabaseShares{.primaries = shards,
                          .replicas = 0.0,
                          .primary_reads = 1.0 / shards,
                          .primary_writes = 1.0 / shards};
  }
  const double replicas = (copies_per_shard - 1.0) * shards;
  return DatabaseShares{.primaries = shards,
                        .replicas = replicas,
                        .primary_reads = 0.0,  // a primary serves no reads while replicas exist
                        .primary_writes = 1.0 / shards,
                        .replica_reads = 1.0 / replicas,
                        .replica_writes = 1.0 / shards};
}

// One copy's load is its share of each kind: with N = 3, 1000 read units/s and 100 write
// units/s, the primary carries 100/s and each replica 1000 / 2 + 100 = 600/s.
DatabaseCopyLoads database_copy_loads(const model::NodeSpec& spec, double reads, double writes) {
  const DatabaseShares shares = database_shares(spec);
  DatabaseCopyLoads loads{.primary = reads * shares.primary_reads + writes * shares.primary_writes};
  if (shares.replicas > 0.0) {
    loads.replica = reads * shares.replica_reads + writes * shares.replica_writes;
  }
  return loads;
}

// The pool lowered the rate below the stated or default capacity (apply_database_keys).
bool pool_binds(const ResolvedNode& node) {
  return node.spec.type == model::NodeType::sql_db && node.spec.pool &&
         node.capacity.service_rate.per_second <
             defaults::resolve_service_rate(node.spec).value.per_second;
}

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
    if (hits_or_misses(resolved.spec.type)) {
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
// the call-graph search above: these types hold std::vectors and std::sets.
namespace detail {

// Does `node` have a hit ratio the graph can resolve now? A cache or CDN with `{fixed: h}` or
// `che`; not a HitRatio{} built in code (neither set: no hit ratio at all), and not a `che`
// CDN without a `keys` size: it stays without a number and misses every time on paper (explain
// says the load behind it is an upper bound), and the engine refuses to run it.
[[nodiscard]] bool has_resolvable_hit_ratio(const model::NodeSpec& spec) {
  if (!hits_or_misses(spec.type) || !spec.hit_ratio) return false;
  if (!spec.hit_ratio->che) return spec.hit_ratio->fixed.has_value();
  return spec.type == model::NodeType::cache || spec.keys.has_value();
}

// A cache whose misses have a Che popularity to pass on: a resolvable `che` one. Any other
// cache (a fixed ratio, an unsized che CDN) says how MANY requests miss but not WHICH keys.
[[nodiscard]] bool filters_by_che(const model::NodeSpec& spec) {
  return has_resolvable_hit_ratio(spec) && spec.hit_ratio->che;
}

// ---- Which keys reach which node: the op-aware walk (plan 2b refinement 1, revised) ----
//
// A che cache's popularity is the popularity of the keys that reach it, and keys travel with
// requests: a client's keys reach a cache only along the CALLS its ops make. Walking demand
// edges (box to box) instead would let a writer whose ops never touch the cache, but pass the
// same service, impose its key distribution on it. So the walk is over (node, op) vertices,
// like the call-cycle search (detail::find_async_hops), and runs per client.
//
// Its hops cannot come from forwarder_steps: those read the resolved hops' fractions, which
// need the very hit ratios being resolved here. key_hops below says, without any hit ratio,
// which (node, op) a call may lead to — every hop that carries flow for SOME hit ratio below 1
// (an `on: miss` step is reachable whatever h turns out to be) — and whose misses it carries.
//
// Assumed: every hop between a client and a cache carries the client's key (a service asked
// for short link k asks its cache for k). One kind of hop does not, and is left out: a rate
// limiter's `counter` request to its store, which counts requests per USER (or per rule), not
// per key the request is about (book ch. 4: the Redis counter is keyed by user id or IP).

// A call (node, op) may lead to: `to` receives `op`. `misses_of` is set when the hop carries
// only the misses of a cache (that cache): a read_through cache's fetch from its backend, a
// cache's `then` step on miss, a service's `on: miss` step after its lookup at a cache.
struct KeyHop {
  std::size_t to{};
  std::string op{};
  std::optional<std::size_t> misses_of{};
};

// A service's (or any node's) `ops` steps for `op`: every step but `on: failure` (nothing fails
// before phase 2c). An `on: miss` step carries the misses of its lookup, the nearest
// unconditional step before it (as Graph::resolve_service_hops pairs them; a miss step after
// something that is no cache is refused there, and inherits here).
void add_op_step_hops(const Graph& g, const model::NodeSpec& spec, std::string_view op,
                      std::vector<KeyHop>& hops) {
  const auto steps = spec.ops.find(std::string{op});
  if (steps == spec.ops.end()) return;
  std::optional<std::size_t> lookup;  // the cache the last unconditional step asked
  for (const model::Step& step : steps->second) {
    const auto target = *g.index_of(step.to);  // connect_step resolved every target
    switch (step.on) {
      case model::StepCondition::failure:
        continue;
      case model::StepCondition::always:
        lookup = hits_or_misses(g.nodes()[target].spec.type) ? std::optional{target} : std::nullopt;
        hops.push_back({.to = target, .op = step.op});
        break;
      case model::StepCondition::miss:
        hops.push_back({.to = target, .op = step.op, .misses_of = lookup});
        break;
    }
  }
}

// A cache's fetch from its backend (Graph::backend_fraction, whatever h is): a read under
// read_through, or at a CDN (from its origin), carries the misses; a write under a write strategy
// carries every write.
void add_backend_hop(const ResolvedNode& cache, std::size_t node, std::string_view op,
                     std::vector<KeyHop>& hops) {
  if (!hits_or_misses(cache.spec.type) || !cache.backend || !sends_to_store(cache.spec, op)) {
    return;
  }
  const auto misses_of = is_write(cache.spec, op) ? std::nullopt : std::optional{node};
  hops.push_back({.to = *cache.backend, .op = std::string{op}, .misses_of = misses_of});
}

// `then` steps (Graph::then_fraction): an unconditional one carries everything, an `on: miss`
// one the read misses of a cache or CDN (and nothing of a write, or of any other node).
void add_then_hops(const Graph& g, const model::NodeSpec& spec, std::size_t node,
                   std::string_view op, std::vector<KeyHop>& hops) {
  for (const model::Step& step : spec.then) {
    const auto target = *g.index_of(step.to);
    if (step.on == model::StepCondition::always) {
      hops.push_back({.to = target, .op = step.op});
    } else if (step.on == model::StepCondition::miss && hits_or_misses(spec.type) &&
               !is_write(spec, op)) {
      hops.push_back({.to = target, .op = step.op, .misses_of = node});
    }
  }
}

// Every hop a request for `op` at `node` may take (see the section comment). A load balancer
// or a limiter passes the op on along its routes that carry it (not along a drained one); a
// limiter's backend (`counter`) is left out on purpose.
[[nodiscard]] std::vector<KeyHop> key_hops(const Graph& g, std::size_t node, std::string_view op) {
  const auto& resolved = g.nodes()[node];
  std::vector<KeyHop> hops;
  if (forwards_by_link(resolved.spec.type)) {
    for (const auto& route : g.routes_for(node, op)) {
      if (route.share > 0.0) hops.push_back({.to = route.target, .op = std::string{op}});
    }
  }
  add_op_step_hops(g, resolved.spec, op, hops);
  add_backend_hop(resolved, node, op, hops);
  add_then_hops(g, resolved.spec, node, op, hops);
  return hops;
}

// One way a client's keys arrive at a node: from `client`, sent on by `sender`, as `op`.
// `filtered_by` is the nearest che cache upstream whose MISSES these requests are (empty: no
// che cache filtered them); `unfiltered_through` the nearest cache since then that missed too
// but has no Che popularity to filter by (a fixed ratio), whose filtering is therefore ignored.
struct KeyArrival {
  std::size_t client{};
  std::size_t sender{};
  std::string op{};
  std::optional<std::size_t> filtered_by{};
  std::optional<std::size_t> unfiltered_through{};
  auto operator<=>(const KeyArrival&) const = default;  // a std::set key (see AsyncHop)
};

// Per node, every way keys arrive at it, sorted (by client first: error messages name the
// clients in scenario order) and without duplicates.
using KeyArrivals = std::vector<std::vector<KeyArrival>>;

// A vertex of the walk: `op` at `node`, with the filters its keys went through so far.
struct KeyStream {
  std::size_t node{};
  std::string op{};
  std::optional<std::size_t> filtered_by{};
  std::optional<std::size_t> unfiltered_through{};
  auto operator<=>(const KeyStream&) const = default;
};

// The filters of a hop's requests: those of the stream it leaves, unless it carries the misses
// of a cache — a che cache becomes the filter (and resets the caveat, which that cache's own
// provenance reports), any other cache becomes the caveat.
[[nodiscard]] KeyStream next_stream(const Graph& g, const KeyStream& from, const KeyHop& hop) {
  KeyStream next{.node = hop.to,
                 .op = hop.op,
                 .filtered_by = from.filtered_by,
                 .unfiltered_through = from.unfiltered_through};
  if (!hop.misses_of) return next;
  if (filters_by_che(g.nodes()[*hop.misses_of].spec)) {
    next.filtered_by = hop.misses_of;
    next.unfiltered_through.reset();
  } else {
    next.unfiltered_through = hop.misses_of;
  }
  return next;
}

// One client's walk: depth-first over KeyStream vertices from its routes (a drained route,
// share 0, carries nothing), recording every arrival. Each vertex is expanded once, so a call
// cycle ends the walk instead of looping.
void walk_client_keys(const Graph& g, std::size_t client,
                      std::vector<std::set<KeyArrival>>& arrivals) {
  std::set<KeyStream> seen;
  std::vector<KeyStream> work;
  const auto arrive = [&](std::size_t sender, KeyStream stream) {
    arrivals[stream.node].insert(KeyArrival{.client = client,
                                            .sender = sender,
                                            .op = stream.op,
                                            .filtered_by = stream.filtered_by,
                                            .unfiltered_through = stream.unfiltered_through});
    if (seen.insert(stream).second) work.push_back(std::move(stream));
  };
  for (const auto& route : g.client_routes(client)) {
    if (route.share > 0.0) arrive(client, KeyStream{.node = route.target, .op = route.op});
  }
  while (!work.empty()) {
    const KeyStream stream = std::move(work.back());
    work.pop_back();
    for (const KeyHop& hop : key_hops(g, stream.node, stream.op)) {
      arrive(stream.node, next_stream(g, stream, hop));
    }
  }
}

// Every client's walk (walk_client_keys), as KeyArrivals. O(clients × (node, op, filter)
// vertices × hops), once per graph however many che caches read it.
[[nodiscard]] KeyArrivals key_arrivals(const Graph& g) {
  std::vector<std::set<KeyArrival>> arrivals(g.nodes().size());
  for (std::size_t node = 0; node < g.nodes().size(); ++node) {
    if (g.nodes()[node].spec.type == model::NodeType::client) walk_client_keys(g, node, arrivals);
  }
  KeyArrivals sorted(arrivals.size());
  for (std::size_t node = 0; node < arrivals.size(); ++node) {
    sorted[node].assign(arrivals[node].begin(), arrivals[node].end());
  }
  return sorted;
}

// ---- What a che cache's keys are ----

// The key distribution a che cache sees, the clients that send it, and the filters it went
// through (KeyArrival).
struct KeySource {
  model::KeySpec keys{};
  std::vector<std::size_t> clients{};
  std::optional<std::size_t> filtered_by{};
  std::optional<std::size_t> unfiltered_through{};
};

// "the misses of cache 'l1'", or "requests no cache filtered": one key stream, for messages.
[[nodiscard]] std::string stream_name(std::span<const ResolvedNode> nodes,
                                      std::optional<std::size_t> filtered_by) {
  if (!filtered_by) return "requests no cache filtered";
  return "the misses of cache '" + nodes[*filtered_by].spec.id + "'";
}

// The clients' one key distribution (plan 2b refinement 1): every client whose ops reach the
// cache must state its keys, and all must agree (KeySpec's defaulted ==, on the canonical form:
// zipf 0 is uniform). Phase 2c may mix several distributions by their rates.
[[nodiscard]] std::expected<KeySource, EngineError> agreed_keys(
    std::span<const ResolvedNode> nodes, const std::string& name,
    std::span<const KeyArrival> arrivals) {
  if (arrivals.empty()) {
    return std::unexpected{EngineError{name + ": no client's demand reaches it"}};
  }
  std::optional<KeySource> source;
  for (const auto& arrival : arrivals) {
    const auto& client = nodes[arrival.client].spec;
    if (!client.workload || !client.workload->keys) {
      return std::unexpected{
          EngineError{name + ": client '" + client.id + "' reaches it without keys"}};
    }
    const auto keys = model::canonical(*client.workload->keys);
    if (!source) {
      source = KeySource{.keys = keys, .clients = {arrival.client}};
    } else if (keys != source->keys) {
      return std::unexpected{EngineError{name + ": clients '" +
                                         nodes[source->clients.front()].spec.id + "' and '" +
                                         client.id + "' send different key distributions"}};
    } else if (source->clients.back() != arrival.client) {  // sorted by client: one entry each
      source->clients.push_back(arrival.client);
    }
  }
  return *source;
}

// Which filter the cache's READS went through: one, for one popularity. A write is not a
// lookup (it neither hits nor misses), so the writes' path does not shape what Che sees: a
// write_through cache in front passes every write on but only its read misses. A cache
// passed since that filter whose misses could not be filtered (KeyArrival::unfiltered_through)
// is kept for explain's caveat.
[[nodiscard]] std::expected<void, EngineError> agree_on_filter(std::span<const ResolvedNode> nodes,
                                                               std::size_t cache,
                                                               const std::string& name,
                                                               std::span<const KeyArrival> arrivals,
                                                               KeySource& source) {
  // The reads among the arrivals. (A plain loop, not a std::views::filter: a lambda calling
  // is_write, which has internal linkage, cannot appear in this module-linkage function.)
  std::vector<const KeyArrival*> reads;
  for (const auto& arrival : arrivals) {
    if (!is_write(nodes[cache].spec, arrival.op)) reads.push_back(&arrival);
  }
  std::set<std::optional<std::size_t>> filters;
  for (const KeyArrival* read : reads) filters.insert(read->filtered_by);
  if (filters.size() > 1) {
    // Name the filtered streams first: an empty optional (no filter) sorts first in the set.
    std::vector<std::optional<std::size_t>> named(filters.begin(), filters.end());
    if (!named.front()) std::ranges::rotate(named, std::next(named.begin()));
    return std::unexpected{EngineError{name + ": it receives both " + stream_name(nodes, named[0]) +
                                       " and " + stream_name(nodes, named[1])}};
  }
  if (filters.empty()) return {};  // only writes arrive: the clients' keys as they are
  source.filtered_by = *filters.begin();
  for (const KeyArrival* read : reads) {
    if (!read->unfiltered_through) continue;
    source.unfiltered_through = read->unfiltered_through;
    break;
  }
  return {};
}

// A ring partitions the cache's keys only if every request reaches it through a ring: a
// request sent to it any other way could land on any replica, which holds a different part
// of the keys. Mixed feeding has no single popularity per replica, so it is refused.
[[nodiscard]] std::expected<void, EngineError> check_ring_only(
    std::span<const ResolvedNode> nodes, std::size_t cache, std::span<const KeyArrival> arrivals) {
  if (!nodes[cache].keys_partitioned) return {};
  const auto is_ring = [&](std::size_t sender) {
    return nodes[sender].spec.type == model::NodeType::load_balancer &&
           nodes[sender].spec.policy == model::LbPolicy::consistent_hash;
  };
  // C++20 ranges algorithm with a projection: find_if over the arrivals, testing each one's
  // `sender` (the projection, a pointer to member) rather than the whole arrival.
  const auto ring = std::ranges::find_if(arrivals, is_ring, &KeyArrival::sender);
  const auto direct = std::ranges::find_if_not(arrivals, is_ring, &KeyArrival::sender);
  if (ring == arrivals.end() || direct == arrivals.end()) return {};
  return std::unexpected{
      EngineError{"node '" + nodes[cache].spec.id + "': keys are partitioned by ring '" +
                  nodes[ring->sender].spec.id + "' but '" + nodes[direct->sender].spec.id +
                  "' also sends to it directly"}};
}

// Pattern: Strategy/Policy — a cache's hit-ratio model is one of a closed set of policies
// (simsys.caching's HitRatioModel types), chosen once when the graph is built from the
// scenario's `hit_ratio` and dispatched with std::visit. Adding a model (an LFU one, say) is
// one more alternative and one more branch in Graph::resolve_hit_ratio that builds it.
// The policy is not kept after build: a Graph holding a caching::Popularity (in a policy on
// ResolvedNode, in a vector of policies or of CheHitRatio, or returned by an accessor) crashed
// GCC 14.2 (internal compiler error in stl_vector.h compiling simsys.behaviours, Release;
// verified 2026-10-28; see also Graph::resolve_hit_ratio). What rebuilds it is kept instead:
// ResolvedHitRatio's keys, filtered_by and the upstream shards (che_popularity), so phase 3's
// node_down can ask hit_ratio(the smaller capacity) of the same model again.
// C++17 std::variant: a type-safe union that holds exactly one of its alternatives; std::visit
// calls the overload for whichever it holds. Replaces a base class with a virtual hit_ratio
// and a heap-allocated derived object per cache.
using HitRatioPolicy = std::variant<caching::FixedHitRatio, caching::CheHitRatio>;

// The scenario's key distribution as simsys.caching's popularity: Zipf(s) over N keys, or
// uniform over N.
[[nodiscard]] caching::Popularity popularity_of(const model::KeySpec& keys) {
  if (keys.zipf) return caching::Popularity::zipf(*keys.zipf, keys.space);
  return caching::Popularity::uniform(keys.space);
}

// The inputs resolving a hit ratio may read: the graph's nodes (with the hit ratios of the
// che caches this one is behind already resolved, with their policies: Graph::resolve_hit_ratios'
// order), which one is the cache, and how keys arrive at it.
struct HitRatioInputs {
  std::span<const ResolvedNode> nodes;
  std::size_t cache;
  std::span<const KeyArrival> arrivals;
};

// The popularity the cache sees: the clients' keys, or, behind a che cache, that cache's
// popularity filtered by its shards' characteristic times (caching::Popularity::filtered_by;
// Martina, Garetto & Leonardi 2014). Rebuilt from what each resolved hit ratio keeps (the keys,
// the cache it is behind, that cache's shards), one filter per cache of the chain: the upstream
// cache's own popularity is already filtered by whatever is in front of IT. Needs every cache
// of the chain resolved first (Graph::resolve_hit_ratios' order).
[[nodiscard]] caching::Popularity che_popularity(std::span<const ResolvedNode> nodes,
                                                 const ResolvedHitRatio& resolved) {
  if (!resolved.filtered_by) return popularity_of(*resolved.keys);
  const auto& upstream = *nodes[*resolved.filtered_by].hit_ratio;
  return che_popularity(nodes, upstream).filtered_by(upstream.shards);
}

// What a `che` hit ratio rests on, checked: the cache size (`keys`), the key distribution of
// the clients that reach it and the filters it went through (agreed_keys, agree_on_filter),
// that a ring partitioning it is its only way in (check_ring_only), and over how many replicas
// the keys are partitioned. This is also its provenance in explain.
// A cache with N replicas holds `keys` entries per replica; what they hold together depends on
// how keys reach them:
//  - round robin, weighted, least connections: any request can reach any replica, so every
//    replica sees the whole key popularity and fills itself with the same hot keys. Each is
//    a full copy of `keys` entries, and the hit ratio is that of ONE cache of `keys`.
//  - consistent hash (ch. 5): each key hashes to exactly one replica, so the replicas hold
//    disjoint parts of the key space, `keys × N` distinct keys together, each over its own arc
//    of the ring (detail::ring_arcs).
// A replica lost to a fault is not subtracted: the hit ratio is resolved once, at build (phase
// 3 rebuilds the policy from the resolved inputs, see detail::HitRatioPolicy).
[[nodiscard]] std::expected<ResolvedHitRatio, EngineError> che_inputs(
    const HitRatioInputs& inputs) {
  const auto& cache = inputs.nodes[inputs.cache];
  const auto& spec = cache.spec;
  const std::string name = "node '" + spec.id + "': hit_ratio che";
  if (!spec.keys) {
    return std::unexpected{EngineError{name + " needs keys (the cache's capacity in keys)"}};
  }
  if (*spec.keys <= 0) {  // the loader's rule (config's read_cache_size), for code
    return std::unexpected{EngineError{"node '" + spec.id + "': keys must be a positive integer"}};
  }
  auto source = agreed_keys(inputs.nodes, name, inputs.arrivals);
  if (!source) return std::unexpected{std::move(source.error())};
  if (auto agreed = agree_on_filter(inputs.nodes, inputs.cache, name, inputs.arrivals, *source);
      !agreed) {
    return std::unexpected{std::move(agreed.error())};
  }
  if (auto ring_only = check_ring_only(inputs.nodes, inputs.cache, inputs.arrivals); !ring_only) {
    return std::unexpected{std::move(ring_only.error())};
  }
  const int partitions = cache.keys_partitioned ? spec.replicas : 1;
  return ResolvedHitRatio{.keys = source->keys,
                          .key_clients = std::move(source->clients),
                          .capacity = static_cast<double>(*spec.keys) * partitions,
                          .partitions = partitions,
                          .filtered_by = source->filtered_by,
                          .unfiltered_through = source->unfiltered_through};
}

// `replicas` replicas that each own a share `replica_share` of the key space (ring_arcs).
struct ArcGroup {
  double replica_share;
  double replicas;
};

// How a consistent-hash ring splits the key space over N replicas, from its imbalance k (the
// hottest replica's load over the average, Graph::mark_balanced_targets: 1 + 1/sqrt(vnodes)):
// the hottest replica owns a_hot = k/N of the keys, the N − 1 others share the rest evenly,
// a_cool = (1 − k/N)/(N − 1). Two groups, not N arcs: the cool replicas are interchangeable,
// and one Che solve per group keeps a 1000-replica cache as cheap as a 4-replica one. Like k
// itself a heuristic: "one replica one standard deviation hot, the others even". One group of
// share 1 for an unpartitioned cache.
[[nodiscard]] std::vector<ArcGroup> ring_arcs(int partitions, double imbalance) {
  if (partitions <= 1) return {{.replica_share = 1.0, .replicas = 1.0}};
  const double n = partitions;
  const double hot = imbalance / n;
  return {{.replica_share = hot, .replicas = 1.0},
          {.replica_share = (1.0 - hot) / (n - 1.0), .replicas = n - 1.0}};
}

// The hit ratio of a cache whose replicas own the arcs `arcs`, `replica_keys` entries each:
//   h = Σ_j a_j · h_model(keys / a_j)
// A replica owning a share a_j of the keys (a random part of every popularity level) sees a_j
// of the requests, and its Che equation over its own part, Σ_{i in part} (1 − e^{−p_i t}) =
// keys, is a_j · Σ_i (1 − e^{−p_i t}) = keys: the equation of ONE cache of keys / a_j over the
// whole space, whose hit ratio is the replica's own. Weighting each replica by its share of
// the requests gives the total. Even arcs (a_j = 1/N) make every term Che(keys × N), the
// "one cache of keys × N" of the even split; uneven ones give less, because the hit ratio is
// concave in the cache size: what the hot replica loses by holding a big arc in the same
// `keys` is more than what the cool ones gain from holding small ones (book ch. 5's hotspot
// problem, seen from the cache: k = 2 over N = 4 gives 0.924 where the even split gives 0.963
// in the consistent-hashing lab). A share of 0 owns nothing and serves nothing.
[[nodiscard]] double hit_ratio_over_arcs(const HitRatioPolicy& policy, double replica_keys,
                                         std::span<const ArcGroup> arcs) {
  double hits = 0.0;
  for (const auto& arc : arcs) {
    if (arc.replica_share <= 0.0) continue;
    // C++20 constrained generic lambda (`const caching::HitRatioModel auto& m`): an `auto`
    // parameter with a concept in front makes the lambda a template whose argument must
    // satisfy the concept. std::visit instantiates it for each alternative of the variant. The
    // call `m.hit_ratio(...)` would fail to compile for a model without one anyway; the concept
    // says up front what every model must provide, and an alternative that is no
    // HitRatioModel fails here naming the concept rather than deep inside std::visit.
    const double replica_hit_ratio = std::visit(
        [&](const caching::HitRatioModel auto& m) {
          return m.hit_ratio(replica_keys / arc.replica_share);
        },
        policy);
    hits += arc.replicas * arc.replica_share * replica_hit_ratio;
  }
  return hits;
}

// Each arc group's key share and characteristic time: what a cache behind this one filters by
// (caching::Popularity::filtered_by). A group's share is all its replicas' together.
[[nodiscard]] std::vector<caching::CacheShard> che_shards(const caching::CheHitRatio& che,
                                                          double replica_keys,
                                                          std::span<const ArcGroup> arcs) {
  std::vector<caching::CacheShard> shards;
  for (const auto& arc : arcs) {
    if (arc.replica_share <= 0.0) continue;
    shards.push_back({.key_share = arc.replicas * arc.replica_share,
                      .characteristic_time = caching::che_characteristic_time(
                          replica_keys / arc.replica_share, che.popularity)});
  }
  return shards;
}

// The order the hit ratios are resolved in: every cache after the che caches whose misses it
// receives (it filters their popularity), found by depth-first search over "is behind" edges;
// a loop of caches each behind the next has no first one and is refused. Caches no other che
// cache feeds come in scenario order.
[[nodiscard]] std::expected<std::vector<std::size_t>, EngineError> hit_ratio_order(
    std::span<const ResolvedNode> nodes, const KeyArrivals& arrivals) {
  enum class Mark { unvisited, active, done };
  std::vector<Mark> marks(nodes.size(), Mark::unvisited);
  std::vector<std::size_t> order;
  // A function object rather than a lambda because it calls itself: one level of recursion
  // per cache in a chain, so a handful at most.
  struct Visit {
    std::span<const ResolvedNode> nodes;
    const KeyArrivals& arrivals;
    std::vector<Mark>& marks;
    std::vector<std::size_t>& order;
    std::expected<void, EngineError> operator()(std::size_t cache) const {
      if (marks[cache] == Mark::done) return {};
      if (marks[cache] == Mark::active) {
        return std::unexpected{EngineError{"node '" + nodes[cache].spec.id +
                                           "': hit_ratio che: its keys loop back through itself "
                                           "(each cache of the loop is behind the next)"}};
      }
      marks[cache] = Mark::active;
      for (const auto& arrival : arrivals[cache]) {
        if (!arrival.filtered_by) continue;
        if (auto visited = (*this)(*arrival.filtered_by); !visited) return visited;
      }
      marks[cache] = Mark::done;
      order.push_back(cache);
      return {};
    }
  };
  const Visit visit{nodes, arrivals, marks, order};
  for (std::size_t node = 0; node < nodes.size(); ++node) {
    if (!has_resolvable_hit_ratio(nodes[node].spec)) continue;
    if (auto visited = visit(node); !visited) return std::unexpected{std::move(visited.error())};
  }
  return order;
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
    if (spec.type == model::NodeType::cdn) {
      if (auto invalid = invalid_cdn(spec)) return std::unexpected{std::move(*invalid)};
    }
    if (spec.type == model::NodeType::sql_db) {
      if (auto invalid = invalid_database(spec)) return std::unexpected{std::move(*invalid)};
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
    if (spec.type == model::NodeType::sql_db) apply_database_keys(node);
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
    if (auto invalid = invalid_link_from_database(spec, nodes_[*from].spec, nodes_[*to].spec)) {
      return std::unexpected{std::move(*invalid)};
    }
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
// miss to the application (a read_through cache has already fetched it from its backend, a CDN
// from its origin, and a cache with a `then` step on miss forwards it itself: either way a miss
// step would fetch it twice), and it must wait for the answer: neither it nor the lookup can be a
// parallel branch, or the response would time the fetch alongside the lookup instead of after it.
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
  if (cache.type == model::NodeType::cdn) {
    return EngineError{name + "cannot follow cdn '" + cache.id +
                       "' (the cdn already fetched the miss from its origin)"};
  }
  if (fetches_read_misses(cache)) {
    return EngineError{name + "cannot follow read_through cache '" + cache.id +
                       "' (the cache already fetched the miss)"};
  }
  if (forwards_own_misses(cache)) {
    return EngineError{name + "cannot follow " + std::string{model::to_string(cache.type)} + " '" +
                       cache.id + "', whose own then step on miss already fetches the miss"};
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
// approximation over the popularity of the keys that reach the cache (resolve_hit_ratio).
// Runs after the last route exists, since the key walk follows them (detail::key_arrivals),
// and after mark_balanced_targets, since a ring in front partitions the cache's keys. The walk
// runs only when some che cache needs it, and the caches resolve in detail::hit_ratio_order:
// a cache behind another che cache filters that cache's popularity, so that one comes first.
Graph::Result Graph::resolve_hit_ratios() {
  const bool any_che = std::ranges::any_of(
      nodes_, [](const ResolvedNode& node) { return detail::filters_by_che(node.spec); });
  const auto arrivals = any_che ? detail::key_arrivals(*this) : detail::KeyArrivals(nodes_.size());
  const auto order = detail::hit_ratio_order(nodes_, arrivals);
  if (!order) return std::unexpected{order.error()};
  for (const auto cache : *order) {
    const detail::HitRatioInputs inputs{
        .nodes = nodes_, .cache = cache, .arrivals = arrivals[cache]};
    if (auto resolved = resolve_hit_ratio(cache, *nodes_[cache].spec.hit_ratio, inputs);
        !resolved) {
      return resolved;
    }
  }
  return {};
}

// One cache's hit ratio, with one branch on what the scenario states: the policy its
// `hit_ratio` names (FixedHitRatio{h}, or CheHitRatio over the popularity of its keys:
// detail::che_inputs and detail::che_popularity), evaluated over the ring's arcs
// (detail::hit_ratio_over_arcs; one arc of share 1 for a cache no ring partitions, and for
// every fixed one), plus the shards a che cache behind this one filters by. The numbers are
// stored on the node rather than returned in a std::expected, and the policy is dropped once
// evaluated: a std::expected of a struct holding the variant, like a Graph holding the
// policy, crashed GCC 14.2 (see detail::HitRatioPolicy).
Graph::Result Graph::resolve_hit_ratio(std::size_t cache, const model::HitRatio& stated,
                                       const detail::HitRatioInputs& inputs) {
  ResolvedHitRatio resolved;
  detail::HitRatioPolicy policy = caching::FixedHitRatio{.value = stated.fixed.value_or(0.0)};
  if (stated.che) {
    auto checked = detail::che_inputs(inputs);
    if (!checked) return std::unexpected{std::move(checked.error())};
    resolved = std::move(*checked);
    policy = caching::CheHitRatio{.popularity = detail::che_popularity(nodes_, resolved)};
  }
  const auto arcs = detail::ring_arcs(resolved.partitions, nodes_[cache].imbalance);
  const double replica_keys = static_cast<double>(nodes_[cache].spec.keys.value_or(0));
  resolved.value = detail::hit_ratio_over_arcs(policy, replica_keys, arcs);
  if (const auto* che = std::get_if<caching::CheHitRatio>(&policy)) {
    resolved.shards = detail::che_shards(*che, replica_keys, arcs);
  }
  nodes_[cache].hit_ratio = std::move(resolved);
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

// Which ops go to the store is sends_to_store's answer (the strategy dispatch; a CDN is
// read_through towards its origin); this adds how many: every write that goes, the misses of a
// read that goes. CacheBehaviour (and so CdnBehaviour), the paper load and the call-cycle search
// all read this share.
double Graph::backend_fraction(std::size_t node, std::string_view op) const {
  const auto& resolved = nodes_.at(node);
  if (!hits_or_misses(resolved.spec.type) || !resolved.backend) return 0.0;
  if (!sends_to_store(resolved.spec, op)) return 0.0;
  return is_write(resolved.spec, op) ? 1.0 : miss_fraction(node);
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
// A CDN gets one to its origin (connect_origin), which carries its read misses.
Graph::Result Graph::connect_backends() {
  for (std::size_t node = 0; node < nodes_.size(); ++node) {
    const auto& spec = nodes_[node].spec;
    if (spec.type == model::NodeType::cdn) {
      if (auto connected = connect_origin(node); !connected) return connected;
      continue;
    }
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
  attach_store(cache, *backend, *link);
  return {};
}

// A CDN's origin (plan 2b refinement 4) becomes its backend: the read_through hop of a cache,
// with the same checks and the CDN's own wording (invalid_cdn proved there is an origin; the
// loader checks the same, config's read_cdn_origin).
Graph::Result Graph::connect_origin(std::size_t cdn) {
  const auto& spec = nodes_[cdn].spec;
  const std::string name = "node '" + spec.id + "': ";
  const auto origin = index_of(*spec.origin);
  if (!origin) {
    return std::unexpected{
        EngineError{name + "origin refers to unknown node '" + *spec.origin + "'"}};
  }
  if (nodes_[*origin].spec.type == model::NodeType::client) {  // see connect_step
    return std::unexpected{EngineError{name + "origin must not be a client"}};
  }
  const auto link = demand_link(cdn, *origin);
  if (!link) {
    return std::unexpected{
        EngineError{name + "origin '" + *spec.origin + "' needs a link from the cdn"}};
  }
  attach_store(cdn, *origin, *link);
  return {};
}

// The node behind a cache or a CDN, and the demand edge to it.
void Graph::attach_store(std::size_t node, std::size_t store, std::size_t link) {
  nodes_[node].backend = store;
  nodes_[node].backend_link = link;
  add_edge(DemandEdge{.from = node, .to = store, .link = link});
}

// A CDN serves static content from the edge and has no write path (book ch. 1: the origin is
// where content changes), so a write op (model::write_ops_of: `write` unless the node lists its
// own) that reaches one is a mistake in the scenario, whoever sends it.
Graph::Result Graph::check_reads_only(std::size_t cdn) const {
  const auto& spec = nodes_[cdn].spec;
  for (const auto& op : arriving_ops_[cdn]) {
    if (!is_write(spec, op)) continue;
    return std::unexpected{EngineError{"node '" + spec.id + "': a cdn does not take writes"}};
  }
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
    if (!hits_or_misses(resolved.spec.type) || !resolved.backend) continue;
    if (resolved.spec.strategy == model::CacheStrategy::write_back) continue;
    const std::string role = resolved.spec.type == model::NodeType::cdn ? "origin" : "backend";
    for (const auto& op : arriving_ops_[cache]) {
      if (backend_fraction(cache, op) <= 0.0) continue;
      if (!is_async_hop(cache, op, *resolved.backend, op)) continue;
      return std::unexpected{EngineError{"node '" + resolved.spec.id + "': " + role + " '" +
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
// chain of balancers and limiters), whether every op reaching one can leave it, and that no
// write reaches a CDN. (What each balancer's policy does to the replicas behind it is marked
// earlier, right after the splits, because a che cache's capacity depends on it:
// mark_balanced_targets.)
Graph::Result Graph::check_routed_ops() {
  collect_arriving_ops();
  for (std::size_t node = 0; node < nodes_.size(); ++node) {
    if (hits_or_misses(nodes_[node].spec.type)) {
      if (auto checked = check_cache_backend_ops(node); !checked) return checked;
    }
    if (nodes_[node].spec.type == model::NodeType::cdn) {
      if (auto checked = check_reads_only(node); !checked) return checked;
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
// routes, the steps that carry flow (the unconditional `then` steps of the node types that do
// not hit or miss, and every resolved hop of positive fraction: an `on: miss` step after a cache
// that misses) and the `counter` op of every limiter's backend, and grown through load balancers
// and rate limiters, which pass each op they receive on to every route whose link carries it and
// is not drained (weight 0: no flow takes it), through caches and CDNs, which pass each op they
// receive on along every `then` step that carries some of THAT op (carries_flow: an `on: miss`
// step carries the read misses, so a cache that only ever receives writes sends nothing along
// it), and through caches and CDNs, which pass an op on to their backend (a CDN's origin) when
// some of it goes there (backend_fraction). Each pair enters the list once, so a loop ends.
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
    // A cache's or CDN's then steps depend on the op (a miss step carries no write): they
    // follow each op that arrives, below. Every other node's unconditional ones fire whatever
    // it receives (a worker pool's work comes from the queue it consumes, not along an edge).
    if (!hits_or_misses(spec.type)) {
      for (const model::Step& step : spec.then) {
        if (step.on == model::StepCondition::always) arrive(*index_of(step.to), step.op);
      }
    }
    if (spec.type == model::NodeType::rate_limiter && nodes_[node].backend) {
      arrive(*nodes_[node].backend, std::string{counter_op});
    }
  }
  while (!work.empty()) {
    const auto [node, op] = std::move(work.back());
    work.pop_back();
    if (hits_or_misses(nodes_[node].spec.type)) {
      for (const model::Step& step : nodes_[node].spec.then) {
        if (carries_flow(node, step, op)) arrive(*index_of(step.to), step.op);
      }
    }
    if (hits_or_misses(nodes_[node].spec.type)) {
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

// Does a `then` step of cache or CDN `node` carry some of the requests of `op` that arrive at
// it? then_fraction says: an unconditional one all of them, an `on: miss` one the misses of a
// read (none if the node never misses) and nothing of a write, an `on: failure` one nothing
// before phase 2c.
bool Graph::carries_flow(std::size_t node, const model::Step& step, std::string_view op) const {
  return then_fraction(node, step, op) > 0.0;
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
// weighted) is fine. A SQL database takes no model from any balancer (its driver picks the
// copy), so any balancers may front one. Two consistent_hash rings with different vnodes keep the
// larger imbalance (the hotter ring decides the hottest replica). A node reached both through a
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
      // A database's driver, not the balancer, picks its copy: writes go to the primary, reads
      // to a replica, and a sharding key spreads the shards evenly. So no balancer's model
      // applies to it (engine::Simulation::make_database): it imposes neither a pool nor a hot
      // copy, and two balancers of different models in front of one database do not conflict.
      if (target.spec.type == model::NodeType::sql_db) continue;
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
