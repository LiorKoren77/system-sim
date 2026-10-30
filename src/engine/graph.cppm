// Module `simsys.graph`: the scenario resolved into something the engine can step —
// node capacities with provenance, link latencies and delay-line lengths, demand edges,
// a topological order, routes of the nodes that split flow (clients, load balancers),
// what each load balancer does to the replicas behind it, feedback edges for node cycles and
// asynchronous hops for call cycles, write-back flushes and blob store events. Cycles are found at
// two levels, for two different questions:
//  - Node level (Graph::order_nodes): which LINK closes a loop of boxes? Its flow goes
//    through a delay line of at least one step, so the forward pass can run in
//    topological order (spec §3.1: cycles use the previous step's value).
//  - Op level (detail::find_async_hops): which HOP closes a loop of CALLS, (node, op) ->
//    (node', op') -> ... -> (node, op)? A request cannot wait for itself, so that hop is
//    an asynchronous hand-off and adds no latency to its caller. A hop that is always waited
//    for (a cache's or a CDN's fetch from its store, a rate limiter's counter request) can be
//    no such hand-off, so a call cycle through one is refused, wherever the cycle is entered
//    (detail::call_components, Graph::check_backend_calls). So is an `on: failure` step that
//    closes a call cycle, a retry with no bound (Graph::check_failure_steps).
// A node cycle need not be a call cycle: with A.x -> B.y and B.z -> A.w the boxes loop
// but no call does. A hop over a node-level feedback link that closes no call cycle is
// counted in latency like any other, one step stale: the backward pass reads the
// target's response table before it is refreshed this step
// (engine::Simulation::backward_pass).
//
// Models: the arrows of a System Design Interview (Alex Xu, vol. 1) diagram. Each box of the book's
// designs (ch. 1 scaling, ch. 11 news feed, ch. 12 chat, ...) becomes a ResolvedNode; each arrow
// that carries requests becomes a DemandEdge over a ResolvedLink. Captured: who sends demand to
// whom and over which link, how long a hop takes (one way, from the book's datacenter round trips),
// how a client splits an op over several entry points (CDN vs origin, two load balancers), and
// which arrows close a loop (chat servers -> message queue -> sync workers -> chat servers), and,
// for a link that states a `bandwidth`, the pipe the engine queues its megabytes in
// (ResolvedLink::pipe, spec §19 entry 3; every request it carries must have a size). Ignored: link
// failures (phase 3), and any per-request routing decision — splits are fixed fractions, as
// round-robin or a weighted DNS record would give. A load balancer's policy is resolved here too,
// as a property of the node BEHIND it (spec §17 entry 5, see mark_balanced_targets): its
// ReplicaModel, whether its replicas queue apart, as one pool, or with a hot spot. A drained route
// (link weight 0) carries nothing, so it brings no op, closes no call cycle and imposes no replica
// model. A rate limiter forwards like a balancer (per link, by weight) whatever it admits, and its
// `backend` (the counter store) becomes a demand edge of its own, carrying op `counter` (spec §17
// entry 6, see connect_backends). A cache (spec §17 entry 7) contributes two things: its hit ratio
// h (Graph::hit_ratio; a CDN's too), stated (`{fixed: h}`) or computed once here by the Che
// approximation from the cache size and the key distribution of the clients whose ops reach it
// (spec §18 entries 1-2, see resolve_hit_ratios), which turns a service's `on: miss` step after it
// into a hop taken by the miss fraction 1 - h of the requests (Graph::service_hops), and, for the
// strategies in which the cache itself talks to the store, a demand edge to its `backend`
// (Graph::backend_fraction says which share of each op takes it; a write_back cache's flush over it
// is asynchronous by design, detail::write_back_flushes), and its `then` steps, demand edges like a
// service's steps whose share of each op is Graph::then_fraction (spec §18 entry 5: what goes on to
// the next tier after the cache). A CDN (spec §18 entry 10) is the read_through shape of a cache:
// its `origin` becomes its backend (Graph::connect_store), a demand edge carrying 1 - h of each
// read as the same op, no write may reach it (Graph::check_reads_only), and no service `on: miss`
// step may follow it. A blob store's `then` steps (spec §19 entries 6-7) are its event
// notifications (S3 event notifications, ch. 14 and 15): demand edges like a cache's unconditional
// steps, fired by every write it serves (an op in its `write_ops`; a read fires nothing,
// Graph::then_fraction), and asynchronous hops by design (detail::blob_store_events); a step on
// miss or on failure, or in parallel mode, is refused, since an event is none of those
// (invalid_blob_store). A message queue (spec §19 entries 4-6; book ch. 1's message queue, ch. 10's
// queue-plus-workers notification system, Kafka in ch. 14's transcoding and ch. 15's notification
// pipelines) COPIES: each link out of it goes to a worker pool that `consumes` it (a consumer
// group), and every group receives every message, so the queue's routes are one per group, each of
// share 1 (route_queue; a queue nobody consumes is refused); the hops along them are asynchronous
// by design (detail::consumer_deliveries: the publisher is answered at the append). A worker pool
// is M/M/c over consumption: c = workers, service time = cost, rate workers / cost
// (defaults::worker_pool_rate), and a waiting room of the messages its queue keeps, retention ×
// that rate (connect_consumers). It takes jobs from the queue it consumes and from nobody else
// (check_pool_senders). A notification fanout (phase 3 spec §8.3; book ch. 10) is a service whose
// `then` steps carry 1 − dedup of what it serves (Graph::then_fraction: its dedup step drops the
// rest as duplicates), waited for like a service's steps; its `channels` map and the steps that
// publish to a message queue must name the same queues (check_fanout_channels).
//
// C++20 named module, interface unit (see src/core/core.cppm for the longer
// explanation of modules versus headers). It declares; the definitions live in the
// implementation unit graph.cpp, so an edit to a function body leaves this BMI unchanged: with
// CMake 3.31 it recompiles graph.cpp alone, not every unit that imports simsys.graph (with CMake
// 3.28.3, the system CMake, every importer still recompiles: 61 units, measured in phase 3a;
// docs/study/modules.md). What stays here: the types, the declarations with their
// contracts, the constants and the inline one-liners whose body is their documentation.
module;
// NO <chrono>/<format>/<print>/<filesystem> here: this interface imports other modules
// (GCC 14.2 'Bad file data' bug, CLAUDE.md §3). Only the headers the declarations need.
#include <array>
#include <compare>
#include <cstddef>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module simsys.graph;

import simsys.caching;
import simsys.core;
import simsys.defaults;
import simsys.fluid;
import simsys.model;

// What resolving one cache's hit ratio reads (defined in graph.cpp with the hit-ratio code), and
// which calls lie on a common call cycle (defined there with the call-graph search); declared
// here for Graph::resolve_hit_ratio, Graph::check_backend_calls and Graph::check_failure_steps.
// Not exported: module linkage,
// like everything in detail.
namespace simsys::graph::detail {
struct HitRatioInputs;
struct CallComponents;
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

// A cache's or CDN's hit ratio h as the graph resolved it (spec §18 entries 1-2): `value`, and for
// `che` the inputs explain prints as its provenance: the key distribution the clients send
// (`keys`), those clients (`key_clients`, node indices in scenario order), and the cache size the
// approximation used (`capacity`, in keys): the cache's `keys`, times its replicas when a
// consistent-hash ring partitions the key space over them (`partitions`, the replica count then, 1
// otherwise; the arcs are uneven, see detail::ring_arcs). When the keys arrive as the misses of
// another che cache, `filtered_by` names it (the popularity is that cache's, filtered:
// caching::Popularity::filtered_by); when they pass a cache with no characteristic time to filter
// by (a fixed ratio), `unfiltered_through` names it and the popularity is used unfiltered. `shards`
// is what a cache behind THIS one filters by: each arc's key share and characteristic time (one
// shard of share 1 for an unpartitioned cache). A fixed ratio has none of these.
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
  // (model::needs_backend), or a CDN's origin (spec §18 entry 10: the CDN is the read_through shape
  // of a cache, its origin the backend), as a node index, and the link its requests travel on
  // (Graph::connect_backends); empty for every other node, a cache_aside cache included.
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
// does every CDN (from its `origin`, spec §18 entry 10). Then nothing after it may fetch them again
// (Graph::invalid_miss_step), and a read's miss share goes on to that node
// (Graph::backend_fraction). False for every other node.
[[nodiscard]] bool fetches_read_misses(const model::NodeSpec& spec);

// How a SQL database spreads its load over its copies (spec §18 entry 6; book ch. 1, "database
// replication" and "sharding"): N = replicas copies per shard, one primary and N − 1 read replicas,
// in S = shards shards. The share of the node's reads and of its writes that ONE copy of each kind
// is sent:
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

// The op a rate limiter sends its counter store once per request it checks (spec §17 entry 6):
// "read and increment this client's counter" (ch. 4's Redis INCR + EXPIRE).
inline constexpr std::string_view counter_op = "counter";

// Nodes that forward whatever reaches them over their links, split by link weight, rather
// than by per-op steps: load balancers and rate limiters (Graph::route_forwarder).
[[nodiscard]] constexpr bool forwards_by_link(model::NodeType type) {
  return type == model::NodeType::load_balancer || type == model::NodeType::rate_limiter;
}

// Does a node of `type` copy what it receives to every node behind it, rather than split it? A
// message queue does: each of its consumer groups (worker pools) reads every message (Kafka's
// consumer groups), so each route has share 1 (Graph::route_queue, Graph::routes_for).
[[nodiscard]] constexpr bool copies_to_consumers(model::NodeType type) {
  return type == model::NodeType::message_queue;
}

// The node types that hit or miss: a cache and a CDN. Their hit ratio h decides the share of
// what they receive that goes on (a service's `on: miss` step after a cache, a cache's backend
// hop or `then` step on miss, a CDN's fetch from its origin); explain's paper load asks the same.
[[nodiscard]] constexpr bool hits_or_misses(model::NodeType type) {
  return type == model::NodeType::cache || type == model::NodeType::cdn;
}

// How the replicas of a node behind a load balancer queue (spec §17 entry 5): what the balancer's
// policy does to them, which is all the fluid model can see of a policy.
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
//   - 1 for an `on: failure` step, of the requests the step it `reports_on` failed: every failed
//     request takes it (spec §5; the dead-letter path of ch. 10's notification system: a
//     notification that failed is kept in the notification log and queued again for a retry).
//     How many failed is known only while the engine runs (1 − d, d the delivered fraction of
//     that step, with its miss steps when it is a lookup, one simulation step late:
//     behaviours::detail::HopBlocks), so readers that size the graph on paper, where nothing is
//     lost, count such a hop as sending nothing (explain's paper load);
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
  // For an `on: failure` step, the position in the op's hops of the step whose failures it is sent:
  // the nearest unconditional step before it (Graph::resolve_service_hop), as a miss step's lookup
  // is; empty for every other step.
  std::optional<std::size_t> reports_on{};
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
  // A link that states a `bandwidth` is a pipe (spec §19 entry 3, defaults::pipe_capacity): one
  // server in megabytes, its service_rate the bandwidth in MB/s and its service_time_s the time of
  // ONE megabyte (the convention of a byte-sized node's ResolvedNode::capacity: a request of 10 MB
  // does not change it; the engine sets each step's object moments on the queue). Every op it
  // carries has a size (Graph::build refuses an unsized one). Empty without a bandwidth: the link
  // only adds its latency.
  std::optional<fluid::Capacity> pipe{};
  // The ops this pipe carries that are WRITES for whoever receives them: an op is a write on the
  // link if a node its payload reaches, the receiver or any node behind it, lists it in its
  // `write_ops` (model::write_ops_of), under its own name or under the name a step gives it that
  // states a `size:` equal to the incoming one (such a step forwards the body itself; a step of
  // another size is a new request). So a 10 MB `upload` through a load balancer into a blob store
  // that lists `upload` is a write on the client's link too: the balancer forwards the body once
  // all of it has arrived, as the store would; and so is one a service sends on as `{op: put, size:
  // 10MB}` to a store that writes `put`. The engine pays a write's whole serialisation on the wire,
  // any other op's first megabyte (behaviours::MegabyteService:: serialising). Sorted; empty
  // without a pipe (Graph::resolve_request_sizes, detail::written_downstream).
  std::vector<std::string> write_ops{};
};

// The node a link delivers demand to: its `to`, or its `from` for a link written from the
// receiver's side (`dir: in`); and the node that sends demand on it, the other end.
[[nodiscard]] constexpr std::size_t demand_receiver(const ResolvedLink& link) {
  return link.dir == model::Direction::in ? link.from : link.to;
}
[[nodiscard]] constexpr std::size_t demand_sender(const ResolvedLink& link) {
  return link.dir == model::Direction::in ? link.to : link.from;
}

// "a->b": a link as the scenario writes it, `from` then `to`, the name its pipe's rows carry in the
// reports; with `quote` "'", "'a'->'b'", the form its errors quote ("link 'a'->'b' has a bandwidth,
// ..."). Always the scenario's `from` and `to`, also for a link written from the receiver's side
// (`dir: in`): `{from: a, to: b, dir: in}` is named `a->b` although its demand, and so its pipe's
// megabytes, travel from b to a, so a reader finds the row under the link as the file writes it.
// C++20 std::span<const T>: a non-owning view of contiguous elements (pointer + size), replacing a
// `const std::vector<T>&` parameter, which would leak the container choice into the API.
[[nodiscard]] std::string link_label(std::span<const ResolvedNode> nodes, const ResolvedLink& link,
                                     std::string_view quote = "");

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
// (from, op) (a call cycle), or the hop is a write_back cache's flush to its store or a blob
// store's event notification along a `then` step.
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
  // Spans, not `const std::vector<T>&`: callers can iterate and index but neither resize nor
  // modify, and the container choice stays inside Graph.
  [[nodiscard]] std::span<const ResolvedNode> nodes() const { return nodes_; }
  [[nodiscard]] std::span<const ResolvedLink> links() const { return links_; }
  [[nodiscard]] std::span<const DemandEdge> edges() const { return edges_; }
  [[nodiscard]] std::span<const std::size_t> topological_order() const { return order_; }
  [[nodiscard]] std::optional<std::size_t> index_of(std::string_view id) const;
  // The link that carries demand from `from` to `to`: (from,to,out|inout) or (to,from,in).
  [[nodiscard]] std::optional<std::size_t> demand_link(std::size_t from, std::size_t to) const;
  // Empty for a node that does not split flow (anything but a client, a load balancer, a rate
  // limiter or a message queue; a limiter's backend link is not a route). A message queue's
  // routes copy: one per consumer group, each of share 1 (route_queue). An index past the last
  // node is a programmer error (std::out_of_range).
  [[nodiscard]] std::span<const Route> routes(std::size_t node) const { return routes_.at(node); }
  [[nodiscard]] std::span<const Route> client_routes(std::size_t client) const {
    return routes(client);
  }
  // The routes of `node` that `op` takes, each with `op` filled in and its share of THAT
  // op: a client's routes for the op as they are; a load balancer's routes whose link
  // carries the op, their shares renormalised to sum to 1 over those links; a message queue's
  // routes whose link carries the op, each of share 1 still (a copy, not a split).
  [[nodiscard]] std::vector<Route> routes_for(std::size_t node, std::string_view op) const;
  // Every op some flow can bring to `node`, sorted: client routes that carry flow (not a
  // drained one, weight 0) and the steps targeting it that carry flow (unconditional ones, and
  // `on: miss` ones after a cache that misses), whatever a load balancer or rate limiter in
  // front passes on, what a cache passes on to its backend, and `counter` on a limiter's
  // backend. Empty for a client.
  [[nodiscard]] std::span<const std::string> arriving_ops(std::size_t node) const {
    return arriving_ops_.at(node);
  }
  // A cache's or a CDN's hit ratio, stated or computed at build (ResolvedNode::hit_ratio);
  // nothing for any other node, and for one that has no number (see ResolvedNode::hit_ratio).
  [[nodiscard]] std::optional<double> hit_ratio(std::size_t node) const;
  // The size of one request of `op` at `node`, resolved once at build (spec §19 entry 1,
  // detail::size_walk): a client's `sizes:` entry or a step's `size:`, kept while the op keeps its
  // name. Empty for an unsized (node, op), which only a node that counts requests may receive, and
  // for an op that never reaches the node.
  [[nodiscard]] std::optional<Bytes> request_bytes(std::size_t node, std::string_view op) const;
  // The hops `node` makes for incoming `op`, one per step in declaration order, each with
  // the share of requests that take it (ServiceHop). Empty for an op the node does not list
  // or lists without steps.
  [[nodiscard]] std::span<const ServiceHop> service_hops(std::size_t node,
                                                         std::string_view op) const;
  // Service discovery (phase 3 spec §8.1; book ch. 12: the client asks the discovery, ZooKeeper,
  // for a server, then talks to the server it was handed). A DISCOVERY LOOKUP is a step to a
  // service_discovery node asking `lookup`; it is not ServiceHop::lookup, which is the position of
  // the CACHE lookup an `on: miss` step follows. Does node `node`, handling `op`, ask a discovery
  // for `target`'s address before it calls it: is there a discovery lookup anywhere before some
  // step to `target`? The rule is per step (discovery_before_step), as the engine's failover reads
  // it: a node_down of some of `target`'s replicas holds the steps after a lookup on the dead ones
  // (engine::FaultSchedule::dead_share), and `[svc, disc.lookup, svc]` reaches svc through it.
  [[nodiscard]] bool lookup_before(std::size_t node, std::string_view op, std::size_t target) const;
  // The discovery whose lookup comes last before step `position` of (node, op) (a position in
  // service_hops), the one that handed out the address that step calls; empty when no discovery
  // lookup comes before it.
  [[nodiscard]] std::optional<std::size_t> discovery_before_step(std::size_t node,
                                                                 std::string_view op,
                                                                 std::size_t position) const;
  // The share of the requests of `op` arriving at cache `node` that the cache passes on to its
  // backend (spec §17 entry 7), as the same op (for a CDN: 1 - h of a read, to its origin, spec §18
  // entry 10; it takes no writes):
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
  // step `step`: 1 for an unconditional step (a worker pool's: every job it drains, whatever op
  // its queue brought), except at a blob store, whose steps are event notifications that fire for
  // its writes only (1 of a write, 0 of a read: S3 notifies on ObjectCreated, never on a GET), and
  // at a notification fanout, whose steps fire for the 1 − dedup its dedup step lets through; for
  // an `on: miss` step after a cache, 1 - h of a read (an unknown h counts as 0, as in
  // service_hops) and 0 of a write (writes do not miss); 0 for an `on: failure` step (refused in
  // a then list: invalid_then_steps, invalid_blob_store, invalid_worker_then_steps; failure steps
  // fire in an op's step list, service_hops) and for a miss step anywhere else. (A CDN has no
  // `then` steps: it fetches its misses from its origin, backend_fraction.)
  [[nodiscard]] double then_fraction(std::size_t node, const model::Step& step,
                                     std::string_view op) const;
  // Does node `from`, handling `op`, hand `target_op` off to node `to` without waiting?
  // True exactly for the hops that close a call cycle (see detail::find_async_hops), for a
  // write_back cache's flushes to its store (detail::write_back_flushes), for a blob store's
  // `then` steps, its event notifications (detail::blob_store_events), and for a message queue's
  // deliveries to its consumer groups (detail::consumer_deliveries).
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
  [[nodiscard]] std::expected<ServiceHop, EngineError> resolve_service_hop(
      std::size_t node, const model::Step& step, std::span<const ServiceHop> hops,
      std::optional<std::size_t> last_unconditional) const;
  [[nodiscard]] std::optional<EngineError> invalid_miss_step(std::size_t node,
                                                             const model::Step& step,
                                                             const ServiceHop* lookup) const;
  [[nodiscard]] std::optional<EngineError> invalid_failure_step(
      std::size_t node, const model::Step& step,
      std::optional<std::size_t> last_unconditional) const;
  [[nodiscard]] double hop_fraction(const model::Step& step,
                                    std::optional<std::size_t> looked_up) const;
  [[nodiscard]] double miss_fraction(std::size_t cache) const;
  Result connect_backends();
  Result connect_backend(std::size_t limiter);
  Result connect_store(std::size_t node, std::string_view role);
  Result check_reads_only(std::size_t cdn) const;
  [[nodiscard]] bool sends(std::size_t from, std::string_view op, std::size_t to) const;
  [[nodiscard]] std::string sender_of(std::size_t node, std::string_view op) const;
  Result check_cache_backend_ops(std::size_t cache);
  Result check_backend_calls(const detail::CallComponents& components) const;
  Result check_failure_steps(const detail::CallComponents& components) const;
  Result route_splitters();
  Result route_queue(std::size_t queue);
  Result check_fanout_channels() const;
  Result check_channels_named(const model::NodeSpec& fanout) const;
  Result check_steps_named(const model::NodeSpec& fanout) const;
  Result check_pool_senders() const;
  Result check_consumer_links(std::size_t queue) const;
  Result connect_consumers();
  Result connect_consumer(std::size_t pool);
  Result resolve_hit_ratios();
  Result resolve_hit_ratio(std::size_t cache, const model::HitRatio& stated,
                           const detail::HitRatioInputs& inputs);
  Result resolve_hops();
  Result check_routed_ops();
  Result route_clients();
  Result route_client_op(std::size_t client, const std::string& op);
  Result route_forwarder(std::size_t node);
  void collect_arriving_ops();
  // collect_arriving_ops' two halves; `arrive(target, op)` records that `op` reaches `target`.
  // A std::function, not a template parameter: a member template of an exported class may not
  // reach the helpers of graph.cpp's anonymous namespace (internal linkage) that these call.
  using Arrive = std::function<void(std::size_t, const std::string&)>;
  void seed_arrivals(std::size_t node, const Arrive& arrive) const;
  void pass_on(std::size_t node, const std::string& op, const Arrive& arrive) const;
  Result resolve_request_sizes();
  Result check_sized_arrivals(std::size_t node) const;
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
  // Per node index: arriving op -> the first node collect_arriving_ops saw send it, for the
  // message about an op that reaches a byte-sized node by a path the size walk does not take.
  std::vector<std::map<std::string, std::size_t, std::less<>>> first_sender_;
  // Per node index: incoming op -> its resolved hops (ServiceHop).
  std::vector<std::map<std::string, std::vector<ServiceHop>, std::less<>>> service_hops_;
  std::vector<AsyncHop> async_hops_;  // sorted, no duplicates
  // Per node index: op -> the size of one request (request_bytes); unsized ops are absent.
  std::vector<std::map<std::string, Bytes, std::less<>>> request_bytes_;
};

// Capacity units one request of `op` uses at `node`, the one rule the behaviours' cost() and
// explain's paper load share: at a byte-sized node (defaults::is_byte_sized) the request's
// megabytes, Graph::request_bytes / 1 MB (spec §19 entry 2: a 50 MB stream costs 50); at any other
// node defaults::op_cost (its `cost` entry, a database write's 5, else 1). An op that never reaches
// a byte-sized node has no size and is priced as at a request-counting one: no flow of it is ever
// charged there. An op that does arrive there unsized cannot exist (Graph::build refuses it,
// Graph::resolve_request_sizes): asked for one, op_cost throws std::logic_error, a programmer
// error, rather than charge it one unit whatever its bytes.
[[nodiscard]] double op_cost(const Graph& g, std::size_t node, std::string_view op);

// A client's `timeout` in seconds, empty when it waits for ever (or the node has no workload): the
// one place the engine (behaviours::ClientBehaviour, which applies it) and the reports (which cap
// and judge percentiles by it) read it from.
[[nodiscard]] std::optional<double> client_timeout_s(const model::NodeSpec& client);

}  // namespace simsys::graph
