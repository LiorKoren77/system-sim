// Module `simsys.engine`: the step loop (spec §3). One step = a forward pass in
// topological order moving demand through delay lines and fluid queues, then a backward
// pass composing response latency per (node, op). `estimate` repeats the same evaluation
// in settle mode until it stops changing: the long-run state. The only randomness is the
// clients' workload noise, and each client draws it from its own engine seeded from the
// scenario's `seed` (behaviours::client_seed), so runs with the same seed are
// byte-identical by construction.
//
// Models: a whole System Design Interview (Alex Xu, vol. 1) diagram set in motion. The
// users box offers load (ch. 2 back-of-the-envelope QPS), every arrow carries it to the
// next box after the hop's latency, every box serves what its capacity allows and queues
// or sheds the rest (ch. 1 scaling), and the response time a user sees is built up on
// the way back: each box's own wait and service time plus the hops it made. Captured:
// saturation spreading downstream as reduced throughput (dropped flow never reaches the
// next box), propagation delay, fan-out and the feedback loops of ch. 12 (chat). Ignored:
// individual requests (flow is a fluid), retries and timeouts, and anything that changes
// within a step (demand is constant across one step).
//
// C++20 named module, interface unit (see src/core/core.cppm for the longer
// explanation of modules versus headers).
module;
// NO <chrono>/<format>/<print>/<filesystem> here: this interface imports other modules
// (GCC 14.2 'Bad file data' bug, CLAUDE.md §3). Like every unit from simsys.behaviours
// on, this one imports a deep module chain and needs -flarge-source-files (set for the
// simsys library in the top-level CMakeLists.txt, which explains why).
#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <span>  // GCC 14: this unit iterates Flow::entries() and Graph's spans itself
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

export module simsys.engine;

import simsys.core;
import simsys.behaviours;
import simsys.defaults;
import simsys.flow;
import simsys.fluid;
import simsys.graph;
import simsys.model;

export namespace simsys::engine {

// The flow in transit on one link (spec §3.4): what enters now leaves `steps` steps
// later, so a 75 ms cross-datacenter hop at 100 ms steps delivers its demand one step
// late. Steps of zero pass the flow straight through.
//
// Pattern: Circular buffer — `steps + 1` slots and a head index; nothing is shifted or
// reallocated as flow moves along the link.
// Invariant: slot `head_` is the flow that leaves at the next pop; slot
// `(head_ + steps) % size` is where this step's arrival goes. With one push and then one
// pop per step, a flow pushed in step k is popped in step k + steps. (Pop before push,
// as a feedback link does, adds one step: see Simulation::line_steps.)
// Contract: push at most once per step. A second push in the same step writes the same
// slot and replaces the first flow (it is not added); the engine sums everything a link
// carries into one Flow first (Simulation::send).
class DelayLine {
 public:
  explicit DelayLine(int steps);   // steps >= 0; a negative count is a programmer error
  void push(flow::Flow arriving);  // what enters the link this step (once per step, see above)
  [[nodiscard]] flow::Flow pop();  // what leaves the link this step; empty if nothing

 private:
  std::vector<flow::Flow> slots_;
  std::size_t head_{0};
};

// One node's numbers for one step. Rates and backlog are summed over replicas (plan
// refinement 4); rho and zone are per copy, which all copies share — or, behind a
// consistent-hash balancer, those of the hottest copy, the one that breaks an SLO first, and
// for a SQL database those of the busier of its primary and its replicas —
// and latency is the sojourn a request sees: one copy's, or behind a hash ring the
// share-weighted mixture of the hot and the cool copies' (see Simulation::make_server), or
// for a database the mixture of its writes' (primary) and its reads' (replica) sojourns,
// weighted by their request rates (Simulation::make_database), and for a byte-sized node the
// mixture of its ops' sojourns, each answered at its own time, by their request rates
// (Simulation::request_weighted_megabyte_sojourn).
// Demand, drain, drops and backlog are in CAPACITY UNITS (per second for the rates), the
// unit `capacity` is a rate of (phase 2a refinement 4): a request of an op with `cost` 3
// counts three times. With every cost at its default of 1, units are requests, as in
// phase 1. The drop ratio dropped / demand is therefore the share of the node's work it shed.
// `rejected_per_s` is a second way to shed (plan 2a refinement 6): requests a rate limiter
// refused by POLICY, never forwarded. For the token bucket and the windowed algorithms they
// are refused after the limiter's queue drained them, so they are part of `drain_per_s`
// too; for a leaky bucket they are its queue's overflow ("bucket full"), which is then
// reported here and not in `dropped_per_s`. Drops are failures, rejections are the policy
// working: report::Summary keeps a drop rate and a reject rate apart. A limiter's every op
// costs 1, so these units are requests.
// Conservation, per node and per step (the fluid queue's balance, summed over copies):
//   demand·dt = (drain + dropped)·dt + Δbacklog
// whatever comes in is served, shed or left waiting. Rejections do not appear as a term of
// their own: a token-family limiter's (token bucket, windowed) are inside `drain` (its
// queue looked at them), and a leaky bucket's are the `dropped` term, moved to `rejected`.
// A database's replicas also replay every write the primary committed (replication); that
// work answers no client, so demand, drain, drops and backlog count the CLIENTS' queries only
// (the balance above holds for them: a replica full of replayed writes therefore drops while
// the backlog shown is below queue_limit × copies), and the replication load shows where it
// bites: in the replicas' rho, zone and the reads' sojourn (Simulation::clients_part), and in
// the replayed writes a replica drops (`replication_dropped_per_s`, one replica's, beside
// `committed_writes_per_s`, what one replica must replay).
struct NodeSample {
  double demand_per_s{};
  double drain_per_s{};
  double dropped_per_s{};
  double rejected_per_s{};
  double backlog{};
  double rho{};
  flow::LatencyEstimate latency;  // own sojourn; zero for a client (a source has no queue)
  fluid::Zone zone{fluid::Zone::green};
  // How old the data a read may return is, in seconds (plan 2b refinement 3): for a SQL
  // database, the write's sojourn at the primary (wait + service, until it is committed) + the
  // replication lag (shipping it) + its sojourn at a replica (waiting behind the reads and the
  // other replayed writes, then applied): a replica shows a write that long after the client
  // sent it. 0 for a database without replicas (every read is served by the primary, which has
  // the latest write); empty for every other node, which keeps no copies of data that could
  // lag. Infinite when the primary or the replica never answers. Finite while a replica drops
  // replayed writes (below): the model's queue is bounded, so this is the bounded-queue figure,
  // while a real replica's lag grows without bound (see replication_dropped_per_s).
  std::optional<double> staleness_s{};
  // The committed writes ONE replica failed to replay, per second, in REQUESTS (writes), not
  // capacity units: the replicas' total / ((N − 1) · S), every replica being alike. A replica
  // whose queue is full drops replayed writes with the rest: it falls behind without bound (the
  // model drops the excess at queue_limit; a real replica keeps it in its relay log/WAL until it
  // is rebuilt), and the `staleness` SLO fails on any. Per replica, so that it compares with
  // `committed_writes_per_s` (a sum over replicas could exceed every write committed). Counted
  // apart from `dropped_per_s`, which is the clients' queries. Same convention as
  // `staleness_s`: 0 for a database whose replicas keep up (or that has none), empty for every
  // other node.
  std::optional<double> replication_dropped_per_s{};
  // The writes ONE shard's primary committed, per second, in requests: the primaries' total / S,
  // and so the writes each replica of that shard must replay. Every database has it (0 when no
  // write arrives); empty for every other node.
  std::optional<double> committed_writes_per_s{};
};

// What a client sees for one op: the rate it offers, the part of it that comes back
// answered, and the response time an answered request gets. `answered` = offered × the
// delivered fraction the backward pass composes along the op's path (plan 2a refinement 8,
// behaviours::NodeBehaviour::delivered): a request a limiter refused or a full queue dropped
// is not answered (one still waiting in a queue is: later, see Simulation::account_rejections),
// and reports weigh latency by `answered` so that those requests, which never saw the path's
// latency, do not dilute it.
struct ResponseSample {
  std::size_t client{};
  std::string op{};
  Rate offered{};
  Rate answered{};
  flow::LatencyEstimate response{};
};

// Everything one step produced: one NodeSample per node (indexed like Graph::nodes()) and
// one ResponseSample per (client, op), clients in node order, ops sorted by name.
struct StepRecord {
  SimTime t{};
  std::vector<NodeSample> nodes{};
  std::vector<ResponseSample> responses{};
  // Only Simulation::estimate clears it: false means the settle passes hit their cap
  // before the numbers stopped changing, so the record is the last pass, not a steady
  // state. A loop that returns as much as it receives (gain >= 1) and that no capacity
  // stops within reach climbs for ever: it has no steady state. Every step of `run`
  // leaves it true.
  bool converged{true};
};

// How long to run and how far to advance per step.
struct RunOptions {
  SimTime duration{};
  SimTime step{};
};

// What `run` hands each step's record to.
// C++20 concept from a standard one: std::invocable<F, Args...> is "F can be called with
// Args". Naming the requirement turns a bad observer into one clear error at the call
// ("constraint StepObserver not satisfied") instead of a page of template instantiation.
template <class F>
concept StepObserver = std::invocable<F, const StepRecord&>;

class Simulation {
 public:
  // C++23 std::expected<T, E>: a runnable Simulation or the reason the graph cannot be
  // run yet. Replaces a throwing constructor: an unsupported node type is user input
  // (a valid scenario for `explain`), not a programmer error.
  // Each client's noise is seeded with client_seed(scenario.seed, node index) (see
  // make_node), so the same scenario and seed give the same run, bit for bit.
  // Pattern: Static factory (named constructor) — `create` validates first, then calls
  // the private constructor, so a Simulation over an unsupported graph cannot exist.
  [[nodiscard]] static std::expected<Simulation, graph::EngineError> create(const graph::Graph& g);

  // Advances every node by `dt` from simulated time `now`. `dt` must be the scenario's
  // step: Graph sized every delay line in steps of it, so another dt would silently
  // rescale every link (a 150 ms link is 2 steps; at dt = 10 ms it would deliver after
  // 20 ms). Throws std::invalid_argument otherwise (a programmer error).
  [[nodiscard]] StepRecord step(SimTime now, SimTime dt);

  // The long-run state at the scenario's step, without disturbing this simulation and
  // independent of what it ran before: settle passes from a clean state until nothing
  // changes (see the definition). Clients offer their rate curve at t = 0 without noise
  // (estimating at another time, `--at`, is phase 2c). Record time is 0; `converged` says
  // whether a steady state was found.
  [[nodiscard]] StepRecord estimate() const;

  // Steps from t = 0 while t < duration and hands every record to `observer`: that is
  // ceil(duration / step) records, the last at the largest multiple of step below
  // duration (1 s at 300 ms steps gives 0, 300, 600, 900 ms, so the last step simulates
  // up to 1.2 s).
  // Pattern: Observer, in its simplest form — a single observer passed per call instead
  // of a list of subscribers. The loop keeps no record itself, so a 24 h run at 100 ms
  // steps (864 000 records) uses no more memory than one step; reports, SLO checks and
  // the CSV series writer are observers that keep only what they need.
  // C++20 abbreviated function template with a constrained placeholder:
  // `StepObserver auto&&` reads "anything satisfying StepObserver", replacing
  // `template <class F> requires StepObserver<F> void run(RunOptions, F&&)`. Defined here
  // because a template's body must be visible to the importer that instantiates it.
  // Throws std::invalid_argument for step <= 0, a step other than the scenario's (see
  // `step`) or duration < 0 (programmer errors: the loader validates the scenario).
  void run(RunOptions options, StepObserver auto&& observer) {
    require_valid(options);
    // `.count()` comparisons: chrono's non-member `<` is invisible in this interface
    // (it does not include <chrono>, CLAUDE.md §3); `+=` is a member and works. Both
    // operands are SimTime, so their counts are in the same unit (nanoseconds).
    for (SimTime now{0}; now.count() < options.duration.count(); now += options.step) {
      observer(step(now, options.step));
    }
  }

 private:
  // advance: one real step. settle: jump to the long-run state (FluidQueue::settle) and
  // read links without their delay (in the long run a link delivers what it is sent).
  enum class Mode { advance, settle };

  // What the backward pass knows about one (node, op): the response time of an answered
  // request and the fraction of the requests that are answered (plan 2a refinement 8).
  struct Answer {
    flow::LatencyEstimate latency{};
    double delivered{1.0};  // 1 until the first backward pass: nothing lost yet
  };
  // Per-op answers of one node, kept across steps: a feedback target reads the previous
  // step's entry. std::less<> (transparent) lets find() take a string_view.
  using ResponseTable = std::map<std::string, Answer, std::less<>>;

  // Identical replica copies modelled by ONE queue (make_server): each copy is sent
  // `demand_share` of the group's arrivals, there are `copies` of them, and a request of the
  // group reaches one of them with probability `request_share` (= demand_share × copies,
  // stored rather than recomputed so that the independent case is exactly 1). Group totals
  // are the queue's totals × copies.
  // A database's writes reach its copy groups differently from its reads
  // (make_database): `demand_share` of the node's writes reach one copy, and the group answers
  // `request_share` of them (0 on the replicas: they apply the writes, the primary answers).
  struct WriteShares {
    double demand_share{};
    double request_share{};
  };
  // What a copy group is to the data it holds, stated rather than read off its shares:
  //   server   every node but a database: a copy like any other, serving every op
  //   primary  a database's primaries: they commit the writes (and, without replicas, serve the
  //            reads too); what they drained is the log the replicas replay
  //   replica  a database's read replicas: they serve the reads and replay the committed
  //            writes, which answer no client
  enum class CopyRole { server, primary, replica };
  struct CopyGroup {
    fluid::FluidQueue queue;
    CopyRole role{CopyRole::server};
    double demand_share{1};
    double copies{1};
    double request_share{1};
    // Only on a database's groups: the shares for its write ops (above). Empty: a write is
    // like every other op, which is all a node other than a database knows.
    std::optional<WriteShares> writes{};
    // What one copy's queue holds, per op, in capacity units (not per second; Flow is
    // reused as "a number per op"). Its total agrees with the queue's backlog to rounding.
    // See serve().
    flow::Flow waiting{};
  };

  // What one copy group's step produced (serve_copies): one copy's fluid step, and what all the
  // group's copies drained and dropped, per op, in capacity units per second: the group's
  // totals, the clients' work and a replica's replayed writes alike.
  struct GroupStep {
    fluid::FluidStep copy{};
    flow::Flow drained{};
    flow::Flow dropped{};
  };

  // A database's replication this step, in requests (writes) per second (replay_committed,
  // NodeSample::committed_writes_per_s and replication_dropped_per_s): what one shard's primary
  // committed and what one replica of it failed to replay.
  struct ReplicationTally {
    double committed_per_shard{};
    double missed_per_replica{};
  };

  // A database's two inputs to its staleness (make_database, NodeSample::staleness_s).
  struct Replication {
    double lag_s{};
    bool has_replicas{};
  };

  struct Node {
    behaviours::Behaviour behaviour;
    // Every node but a client (a source) has one group (independent or pooled replicas) or
    // two (behind a hash ring: the hottest copy first, then the cool ones; a database with
    // replicas: its primaries, then its replicas). See make_server and make_database.
    std::vector<CopyGroup> copy_groups{};
    std::vector<std::size_t> incoming_links{};  // links delivering demand here
    std::vector<std::size_t> outgoing_links{};  // links this node sends demand on
    flow::Flow inflow{};                        // this step's arrivals
    // What the node's queues dropped this step, per op, in requests: the clients' part only (a
    // replica's replayed writes answer no client). Filled by serve, read by account_rejections.
    flow::Flow dropped{};
    // The share of this step's arrivals the node does not lose, 1 − (dropped + rejected) /
    // inflow: the node's own factor in its delivered fraction (see account_rejections). A
    // database keeps its writes' share apart (`passed_writes`): its primary and its replicas
    // fill up separately.
    double passed{1.0};
    double passed_writes{1.0};
    // This step's own sojourn of a read and of a write (serve): a database's writes wait at its
    // primary and its reads at a replica; every other node has one sojourn for all its ops,
    // held in both. The backward pass hands each op its own (own_sojourn).
    flow::LatencyEstimate read_sojourn{};
    flow::LatencyEstimate write_sojourn{};
    // This step's queueing wait alone, mixed over the groups as the sojourns are (serve). A
    // byte-sized node answers each op at its own time after it (own_sojourn: a write's last
    // byte, a read's first megabyte), so there the two sojourns above are not used.
    flow::LatencyEstimate wait{};
    std::optional<Replication> replication{};  // databases only
    ResponseTable responses{};
  };

  struct Link {
    DelayLine line;
    flow::Flow sent{};  // what the source put on the link in the latest evaluation
  };

  Simulation() = default;  // only for settling_copy(), which fills every member
  // Copying a Simulation works only inside this module under GCC 14: an importer that
  // copies one gets undefined references to its implicit special members. settling_copy,
  // a member compiled in this module, is where estimate() copies; callers only move.
  explicit Simulation(const graph::Graph& g);
  void require_valid(const RunOptions& options) const;
  void require_scenario_step(SimTime dt) const;
  [[nodiscard]] static Node make_node(const graph::Graph& g, std::size_t index);
  [[nodiscard]] static Node make_server(behaviours::Behaviour behaviour,
                                        const graph::ResolvedNode& resolved);
  [[nodiscard]] static CopyGroup independent_copies(const graph::ResolvedNode& resolved);
  [[nodiscard]] static CopyGroup pooled_copies(const graph::ResolvedNode& resolved);
  static void add_hot_and_cool_copies(const graph::ResolvedNode& resolved,
                                      std::vector<CopyGroup>& groups);
  [[nodiscard]] static Node make_database(behaviours::Behaviour behaviour,
                                          const graph::ResolvedNode& resolved);
  [[nodiscard]] static int line_steps(const graph::ResolvedLink& link);
  void connect(const graph::Graph& g);
  void register_reachable_ops(const graph::Graph& g);
  [[nodiscard]] Simulation settling_copy() const;

  [[nodiscard]] StepRecord evaluate(SimTime now, SimTime dt, Mode mode);
  void forward_pass(SimTime now, SimTime dt, Mode mode, std::vector<NodeSample>& samples);
  void receive(std::size_t index, Mode mode);
  // Any queueing node (service, load balancer, rate limiter): runs its queues and returns
  // what they drained, in requests per second, for the behaviour to route.
  [[nodiscard]] flow::Flow serve(std::size_t index, SimTime dt, Mode mode, NodeSample& sample);
  [[nodiscard]] static GroupStep serve_copies(CopyGroup& group, const flow::Flow& arrivals,
                                              SimTime dt, Mode mode,
                                              const behaviours::Behaviour& behaviour,
                                              NodeSample& sample);
  [[nodiscard]] static flow::Flow group_arrivals(const CopyGroup& group, const flow::Flow& reads,
                                                 const flow::Flow& writes);
  static void replay_committed(const CopyGroup& group, const GroupStep& step,
                               const behaviours::Behaviour& behaviour, flow::Flow& writes,
                               ReplicationTally& tally);
  [[nodiscard]] static flow::Flow clients_part(const CopyGroup& group, flow::Flow units,
                                               const behaviours::Behaviour& behaviour);
  [[nodiscard]] static double clients_share(const CopyGroup& group, const flow::Flow& units,
                                            const behaviours::Behaviour& behaviour);
  [[nodiscard]] static flow::Flow dropped_part(const flow::Flow& content,
                                               const fluid::FluidStep& copy_step, double copies);
  // C++20 std::span<const T>: a non-owning view of contiguous elements (a whole array or
  // vector, or the front of one), replacing a (pointer, count) pair or a `const std::vector&`
  // that would force the caller to own a vector. The sojourns live in a fixed-size array of
  // which only the groups in use are passed (see the caller).
  static void record_sojourns(Node& node, std::span<const flow::LatencyEstimate> sojourns,
                              std::span<const flow::LatencyEstimate> waits, NodeSample& sample);
  [[nodiscard]] static flow::LatencyEstimate request_weighted_sojourn(const Node& node);
  static void time_whole_objects(Node& node);
  [[nodiscard]] static flow::LatencyEstimate request_weighted_megabyte_sojourn(
      const Node& node, const behaviours::MegabyteService& megabytes);
  static void record_replication(const Node& node, std::span<const flow::LatencyEstimate> sojourns,
                                 const ReplicationTally& tally, NodeSample& sample);
  [[nodiscard]] static flow::Flow take_drained(CopyGroup& group, const flow::Flow& content,
                                               const fluid::FluidStep& copy_step);
  void send(std::size_t index, const behaviours::Routed& routed, Mode mode);
  void account_rejections(std::size_t index, const behaviours::Routed& routed, NodeSample& sample);
  void backward_pass();
  [[nodiscard]] std::vector<ResponseSample> client_responses(SimTime now) const;
  [[nodiscard]] flow::LatencyEstimate response_of(std::size_t node, std::string_view op) const;
  [[nodiscard]] static flow::LatencyEstimate own_sojourn(const Node& node, std::string_view op);
  [[nodiscard]] static double own_passed(const Node& node, std::string_view op);
  [[nodiscard]] double delivered_of(std::size_t node, std::string_view op) const;

  std::vector<Node> nodes_;
  std::vector<Link> links_;
  std::vector<std::size_t> order_;
  SimTime step_{};
};

}  // namespace simsys::engine

namespace simsys::engine {

namespace {

// The "overloaded" visitor idiom: one object whose operator() is overloaded by every
// lambda it is built from, so std::visit picks the lambda matching the alternative held.
// C++17 pack expansion in a using-declaration (`using Fs::operator()...`) merges the
// lambdas' call operators into one overload set. C++20 class template argument deduction
// for aggregates deduces `Overloaded<L1, L2>` from the braces, so the deduction guide
// C++17 needed (`Overloaded(Fs...) -> Overloaded<Fs...>`) is gone.
template <class... Fs>
struct Overloaded : Fs... {
  using Fs::operator()...;
};

// The node types the engine can step today; the others arrive with their phase.
[[nodiscard]] bool simulated(model::NodeType type) {
  // C++20 `using enum`: the enumerators of NodeType in scope for this function only, so
  // the test reads `client` instead of `model::NodeType::client`.
  using enum model::NodeType;
  return type == client || type == service || type == load_balancer || type == rate_limiter ||
         type == cache || type == sql_db || type == cdn || type == id_generator ||
         type == blob_store;
}

// Why a cache cannot run, if it cannot: a scenario built in code left it without a number or
// `che` for its hit ratio (the loader requires one: the same message). Everything else a cache
// can say runs: a `che` hit ratio (Graph::build has already turned it into a number,
// Graph::resolve_hit_ratios), every strategy including write_back (its flush is an asynchronous
// hop, graph::Graph::is_async_hop) and `then` steps (plan 2b refinement 5,
// behaviours::CacheBehaviour).
[[nodiscard]] std::optional<std::string> cache_not_runnable(const model::NodeSpec& spec) {
  const bool has_number = spec.hit_ratio && spec.hit_ratio->fixed;
  const bool is_che = spec.hit_ratio && spec.hit_ratio->che;
  if (!has_number && !is_che) return "node '" + spec.id + "': cache needs a hit_ratio";
  return std::nullopt;
}

// Why a CDN cannot run, if it cannot (plan 2b refinement 4): a `che` hit ratio without a `keys`
// size. Graph::build leaves it without a number, and running it as h = 0 would silently replace
// the scenario's Che approximation by "every read misses" (explain shows that bound and says so;
// a run could not). A CDN with no hit ratio at all runs: it misses every read, as the scenario
// says. A CDN sized in bytes runs too, counting megabytes (plan 2c refinement 2,
// behaviours::CdnBehaviour).
[[nodiscard]] std::optional<std::string> cdn_not_runnable(const model::NodeSpec& spec) {
  if (spec.hit_ratio && spec.hit_ratio->che && !spec.keys) {
    return "node '" + spec.id + "': hit_ratio che needs keys (the cdn's size in objects)";
  }
  return std::nullopt;
}

// Why a node of a simulated type still cannot run, if it cannot (the cases above). A blob store
// always runs, `then` steps included: they are event notifications (plan 2c refinements 6-7),
// asynchronous hops the backward pass does not wait for (behaviours::BlobStoreBehaviour).
[[nodiscard]] std::optional<std::string> not_runnable_yet(const model::NodeSpec& spec) {
  if (spec.type == model::NodeType::cache) return cache_not_runnable(spec);
  if (spec.type == model::NodeType::cdn) return cdn_not_runnable(spec);
  return std::nullopt;
}

void add_once(std::vector<std::size_t>& links, std::size_t link) {
  // C++23 std::ranges::contains: "is this value in the range?" in one call, replacing
  // `std::find(begin, end, value) != end`.
  if (!std::ranges::contains(links, link)) links.push_back(link);
}

// What one copy's queue holds during a step, per op, in capacity units: what was waiting
// plus this step's arrivals (rate × dt).
[[nodiscard]] flow::Flow queue_content(const flow::Flow& waiting, const flow::Flow& arrivals,
                                       SimTime dt) {
  flow::Flow content = waiting;
  content += arrivals.scaled(seconds(dt));
  return content;
}

// The long-run queue of estimate(): FluidQueue::settle sets the total backlog, and in
// the long run the queue holds the arrival mix, so each op waits in proportion to its
// arrival rate: arrivals × backlog / demand (units per second × seconds = units).
[[nodiscard]] flow::Flow settled_waiting(const flow::Flow& arrivals, double backlog) {
  const double demand_per_s = arrivals.total().per_second;
  return demand_per_s > 0.0 ? arrivals.scaled(backlog / demand_per_s) : flow::Flow{};
}

// Capacity units one request of `op` uses at a node: its behaviour's cost(op). A client
// has no queue to charge, so it never gets here; 1 keeps the visitor total.
[[nodiscard]] double cost_of(const behaviours::Behaviour& behaviour, std::string_view op) {
  return std::visit(Overloaded{[](const behaviours::ClientBehaviour&) { return 1.0; },
                               [&](const auto& server) { return server.cost(op); }},
                    behaviour);
}

// Requests per second -> capacity units per second: each op's rate × its cost (plan 2a
// refinement 4). Demand in units is Σ rate_op × cost_op, the load the node's capacity
// (a rate of units) is compared with: 300 req/s of an op of cost 3 is 900 units/s, so a
// 1000 units/s node runs at rho = 0.9.
[[nodiscard]] flow::Flow in_units(const flow::Flow& requests, const behaviours::Behaviour& server) {
  flow::Flow units;
  for (const auto& [op, rate] : requests.entries()) {
    units.add(op, Rate{rate.per_second * cost_of(server, op)});
  }
  return units;
}

// The inverse: units per second of each op -> requests per second (units / cost). What a
// node forwards downstream is requests, whatever they cost here: the next node prices
// them with its own costs.
[[nodiscard]] flow::Flow in_requests(const flow::Flow& units, const behaviours::Behaviour& server) {
  flow::Flow requests;
  for (const auto& [op, rate] : units.entries()) {
    requests.add(op, Rate{rate.per_second / cost_of(server, op)});
  }
  return requests;
}

// The most copy groups a node has: the hot and the cool copies behind a hash ring (see
// Simulation::make_server), a database's primary and replicas (Simulation::make_database);
// every other node has one.
inline constexpr std::size_t max_copy_groups = 2;

// Whether `op` is a write the node queues apart from its reads: only a database has such ops
// (behaviours::SqlDbBehaviour::is_write, its `write_ops`); every other node treats all its ops
// alike, so for it nothing is.
[[nodiscard]] bool is_write_at(const behaviours::Behaviour& behaviour, std::string_view op) {
  const auto* database = std::get_if<behaviours::SqlDbBehaviour>(&behaviour);
  return database != nullptr && database->is_write(op);
}

// How a node times its requests in megabytes (behaviours::MegabyteService) when it counts them:
// a blob store or a CDN sized in bytes (defaults::is_byte_sized). Null for every other node.
[[nodiscard]] const behaviours::MegabyteService* megabytes_of(
    const behaviours::Behaviour& behaviour) {
  return std::visit(
      [](const auto& node) -> const behaviours::MegabyteService* {
        // C++20 requires-expression as a compile-time condition: `requires { node.megabytes(); }`
        // is true exactly for the alternatives that have such a member. Replaces the C++17
        // detection idiom (a std::void_t trait written for the purpose) with one line, so only
        // the behaviours that may count megabytes need to know about them.
        if constexpr (requires { node.megabytes(); }) {
          return node.megabytes();
        } else {
          return nullptr;
        }
      },
      behaviour);
}

// `units` split into its reads and its writes (is_write_at): every op of a node that is not a
// database is a read, and the flow is returned whole (moved, not copied: taken by value, so a
// caller that hands over a temporary pays nothing).
[[nodiscard]] std::pair<flow::Flow, flow::Flow> split_writes(
    flow::Flow units, const behaviours::Behaviour& behaviour) {
  if (!std::holds_alternative<behaviours::SqlDbBehaviour>(behaviour)) {
    return {std::move(units), flow::Flow{}};
  }
  std::pair<flow::Flow, flow::Flow> split;
  for (const auto& [op, rate] : units.entries()) {
    (is_write_at(behaviour, op) ? split.second : split.first).add(op, rate);
  }
  return split;
}

// The share of `part` in `whole` (totals), 1 when the whole is empty: nothing to split.
[[nodiscard]] double share_of(const flow::Flow& part, const flow::Flow& whole) {
  const double total = whole.total().per_second;
  return total > 0.0 ? part.total().per_second / total : 1.0;
}

// Adds `copies` identical copies, each of which took `copy_step`, to the group's totals
// (plan refinement 4): N copies behind an equal split each saw demand / N, so the group's
// rates and backlog are the copy's times N. A pooled group is one copy (its queue IS the
// group); a hash ring adds its hot copy (1) and its cool ones (N - 1); a database its S
// primaries and its (N - 1) S replicas. Only the clients' part of the work counts (NodeSample):
// `arrived_share` of the copy's demand and `content_share` of what it drained, dropped and
// holds, both 1 except on a database replica (Simulation::serve_copies). `rejected_per_s`
// stays 0: a queue never rejects; the forward pass fills it (account_rejections).
void add_copies(NodeSample& sample, const fluid::FluidStep& copy_step, double copies,
                double arrived_share, double content_share) {
  sample.demand_per_s += copy_step.demand_per_s * copies * arrived_share;
  sample.drain_per_s += copy_step.drain_per_s * copies * content_share;
  sample.dropped_per_s += copy_step.dropped_per_s * copies * content_share;
  sample.backlog += copy_step.backlog * copies * content_share;
}

// The share of the requests that arrived and were not lost, 1 − lost / arrived, kept in [0, 1]
// (see account_rejections); 1 when nothing arrived: nothing was lost.
[[nodiscard]] double kept_share(const flow::Flow& lost, const flow::Flow& arrived) {
  const double arrived_per_s = arrived.total().per_second;
  if (arrived_per_s <= 0.0) return 1.0;
  return std::clamp(1.0 - lost.total().per_second / arrived_per_s, 0.0, 1.0);
}

// estimate()'s stopping rule: a pass changed nothing that matters when every node's
// demand and every response (and answered rate) a client sees moved by at most this
// fraction of its value.
// 1e-12 is a few thousand ulps of a double: as close to "exactly equal" as rounding allows.
inline constexpr double settle_tolerance = 1e-12;
// ...and the most passes it tries. Each pass costs microseconds per node, so the cap
// bounds estimate() well under a second for phase 1 scenarios; see estimate() for what
// needs more passes than this.
inline constexpr int max_settle_passes = 100'000;

// Written so that a NaN never counts as unchanged (every comparison with NaN is false)
// and an `inf` latency (a zero-capacity node, "unavailable") that stays inf does.
[[nodiscard]] bool unchanged(double before, double after) {
  return before == after || std::abs(after - before) <= settle_tolerance * std::abs(after);
}

// Records of two consecutive settle passes over the same simulation have the same nodes
// and the same (client, op) responses in the same order.
[[nodiscard]] bool settled(const StepRecord& before, const StepRecord& after) {
  for (std::size_t i = 0; i < after.nodes.size(); ++i) {
    if (!unchanged(before.nodes[i].demand_per_s, after.nodes[i].demand_per_s)) return false;
  }
  for (std::size_t i = 0; i < after.responses.size(); ++i) {
    if (!unchanged(before.responses[i].response.mean_s, after.responses[i].response.mean_s) ||
        !unchanged(before.responses[i].answered.per_second,
                   after.responses[i].answered.per_second)) {
      return false;
    }
  }
  return true;
}

}  // namespace

// ---- DelayLine ----

DelayLine::DelayLine(int steps) {
  if (steps < 0) throw std::invalid_argument{"DelayLine: steps must be >= 0"};
  slots_.resize(static_cast<std::size_t>(steps) + 1);
}

// Writes `steps` slots ahead of the head (the last slot, cyclically). With steps == 0
// that is the head itself, so the pop that follows in the same step returns it.
void DelayLine::push(flow::Flow arriving) {
  slots_[(head_ + slots_.size() - 1) % slots_.size()] = std::move(arriving);
}

// Takes the head slot, leaves it empty for a later push, and moves the head on.
flow::Flow DelayLine::pop() {
  flow::Flow leaving = std::exchange(slots_[head_], flow::Flow{});
  head_ = (head_ + 1) % slots_.size();
  return leaving;
}

// ---- Simulation: construction ----

std::expected<Simulation, graph::EngineError> Simulation::create(const graph::Graph& g) {
  for (const auto& node : g.nodes()) {
    if (simulated(node.spec.type)) continue;
    // C++23 std::unexpected<E>: wraps an error so it converts to the error side of a
    // std::expected (a bare EngineError would be taken as a value, and fail to convert).
    // Replaces throwing or returning a sentinel; the caller must look at the result.
    return std::unexpected{graph::EngineError{
        "node '" + node.spec.id + "' has type '" + std::string{model::to_string(node.spec.type)} +
        "', which run/estimate do not support yet (phase 2)"}};
  }
  for (const auto& node : g.nodes()) {
    if (auto refused = not_runnable_yet(node.spec)) {
      return std::unexpected{graph::EngineError{std::move(*refused)}};
    }
  }
  return Simulation{g};
}

// Copies what a step needs out of the graph, so the Simulation does not outlive a
// reference to it.
Simulation::Simulation(const graph::Graph& g)
    : order_{g.topological_order().begin(), g.topological_order().end()}, step_{g.scenario().step} {
  for (std::size_t i = 0; i < g.nodes().size(); ++i) nodes_.push_back(make_node(g, i));
  // C++20 designated initializers: `Link{.line = ...}` names the member it sets and leaves
  // the others at their defaults, replacing positional braces where a reader must count
  // members against the struct (and same-typed doubles, as in CopyGroup, could swap silently).
  for (const auto& link : g.links()) links_.push_back(Link{.line = DelayLine{line_steps(link)}});
  connect(g);
  register_reachable_ops(g);
}

// Pattern: Strategy — each node gets the behaviour of its type, held by value in the
// behaviours::Behaviour variant and dispatched with std::visit in the forward pass.
// A client's noise seed is client_seed(scenario seed, node index): one random engine per
// client, so appending a node at the end of the file never changes another client's noise
// (the seed is the node index; inserting a node earlier shifts the clients after it; spec
// phase 2a refinement 3), and `simsys curve --noise` derives the same seed.
Simulation::Node Simulation::make_node(const graph::Graph& g, std::size_t index) {
  const auto& resolved = g.nodes()[index];
  switch (resolved.spec.type) {
    case model::NodeType::client:
      return Node{.behaviour = behaviours::ClientBehaviour{
                      g, index, behaviours::client_seed(g.scenario().seed, index)}};
    case model::NodeType::load_balancer:
      return make_server(behaviours::LoadBalancerBehaviour{g, index}, resolved);
    // A rate limiter's queue is built like any server's from the resolved capacity: for the
    // token bucket and the windowed algorithms a plain gate (the type's 100k/s, 50 us check,
    // or the scenario's `capacity`), for a leaky bucket the bucket itself
    // (graph::leaky_bucket_capacity: one deterministic slot leaking at `limit`, room
    // `burst`), so the queue does the leaking.
    case model::NodeType::rate_limiter:
      return make_server(behaviours::RateLimiterBehaviour{g, index}, resolved);
    // A cache's queue is the lookup itself (the type's 100k/s, 250 us, or the scenario's
    // numbers); the store behind it, if its strategy calls one, is a node of its own.
    case model::NodeType::cache:
      return make_server(behaviours::CacheBehaviour{g, index}, resolved);
    // A CDN's queue is the edge serving an object (the type's 100k/s, 10 ms, or the scenario's
    // numbers); its origin is a node of its own, reached on a miss.
    case model::NodeType::cdn:
      return make_server(behaviours::CdnBehaviour{g, index}, resolved);
    // A blob store's queue is the store moving megabytes (spec §6's 500 MB/s at 40 ms per MB, or
    // the scenario's numbers); a request is charged its size (behaviours::BlobStoreBehaviour).
    case model::NodeType::blob_store:
      return make_server(behaviours::BlobStoreBehaviour{g, index}, resolved);
    // A database's queues are its primaries and its replicas (make_database), whatever a
    // balancer in front of it does.
    case model::NodeType::sql_db:
      return make_database(behaviours::SqlDbBehaviour{g, index}, resolved);
    // An id generator (plan 2b refinement 6) is a service whose capacity and service time its
    // algorithm supplied at build (defaults::defaults_of): no behaviour of its own.
    case model::NodeType::id_generator:
    default:  // create() admitted only simulated types: the rest are services
      return make_server(behaviours::ServiceBehaviour{g, index}, resolved);
  }
}

// The queues of a node that serves (plan 2a refinement 5: what the balancer in front, if
// any, does to the replica group; graph::Graph::mark_balanced_targets sets the flags and
// documents the models). A node reached both through a balancer and directly gets the
// balancer's model for all its traffic: the model belongs to the group, not to one path.
// Pattern: Strategy (data-driven) — the three replica models differ only in the copy
// groups built here; serve() runs any list of groups the same way.
//  - Independent copies (no balancer, round_robin, weighted): independent_copies.
//  - Pooled (least_connections): pooled_copies.
//  - Imbalanced (consistent_hash, imbalance k > 1): add_hot_and_cool_copies.
Simulation::Node Simulation::make_server(behaviours::Behaviour behaviour,
                                         const graph::ResolvedNode& resolved) {
  Node node{.behaviour = std::move(behaviour)};
  if (resolved.pooled) {
    node.copy_groups.push_back(pooled_copies(resolved));
  } else if (resolved.imbalance > 1.0) {
    add_hot_and_cool_copies(resolved, node.copy_groups);
  } else {
    node.copy_groups.push_back(independent_copies(resolved));
  }
  return node;
}

// One queue models one copy, which sees demand / N; the group is N of it.
Simulation::CopyGroup Simulation::independent_copies(const graph::ResolvedNode& resolved) {
  const double replicas = resolved.spec.replicas;
  return CopyGroup{.queue = fluid::FluidQueue{resolved.capacity},
                   .demand_share = 1.0 / replicas,
                   .copies = replicas,
                   .request_share = 1.0};
}

// ONE queue for the whole group with N × the service rate, N × the servers and N × the
// waiting room, i.e. M/M/(c·N). It sees the whole demand and its numbers are the group's.
// Assumes join-the-shortest-queue is perfect: no request ever waits while some replica is
// idle. The servers are capped like any concurrency (defaults::max_concurrency: Erlang C
// costs O(c) per step) but the service rate is not: the rate is what the group DRAINS, and
// capping it would shed flow the real pool serves. The cap reaches only the Erlang C wait,
// and errs on the pessimistic side (fewer servers wait longer); at 100k servers that wait is
// negligible below the rho 0.99 the stochastic term is evaluated at anyway.
Simulation::CopyGroup Simulation::pooled_copies(const graph::ResolvedNode& resolved) {
  const double replicas = resolved.spec.replicas;
  fluid::Capacity pool = resolved.capacity;
  pool.service_rate = Rate{pool.service_rate.per_second * replicas};
  // Computed in double so a large N × c cannot overflow an int.
  pool.concurrency = static_cast<int>(std::min(static_cast<double>(pool.concurrency) * replicas,
                                               static_cast<double>(defaults::max_concurrency)));
  pool.queue_limit *= replicas;
  return CopyGroup{.queue = fluid::FluidQueue{pool}};
}

// Behind a hash ring the hottest of N copies holds k times the average share (k = the
// node's imbalance, graph::Graph::mark_balanced_targets) and the N - 1 others split the rest
// evenly, so the group is TWO kinds of copy:
//                 copies   demand share of ONE copy    request share (all copies)
//   hot copy        1        k / N                       k / N
//   cool copies     N - 1    (N - k) / (N (N - 1))       (N - k) / N
// The shares add up to the whole demand: k/N + (N - 1)(N - k)/(N (N - 1)) = 1. Each kind is
// its own queue, so the group drains, drops and holds hot + (N - 1) × cool: a saturated hot
// copy sheds only its own excess, while the cool copies serve their share. The node reports
// the hot copy's rho and zone (the copy that breaks an SLO first) and, as its latency, the
// mixture a request sees: the hot copy's sojourn with probability k / N, a cool copy's
// otherwise (serve). With k = N (2 copies on a 1-vnode ring) the cool copies get nothing
// and are left out. Ignored: WHICH keys are hot (the hottest arc is a fixed share, not a
// popular key: a Zipf hot key is the cache's business), and the spread among the cool
// copies (they are taken as equal, so only the hottest copy is singled out).
void Simulation::add_hot_and_cool_copies(const graph::ResolvedNode& resolved,
                                         std::vector<CopyGroup>& groups) {
  const double replicas = resolved.spec.replicas;
  const double k = resolved.imbalance;  // in (1, N]: mark_balanced_targets caps it at N
  groups.push_back(CopyGroup{.queue = fluid::FluidQueue{resolved.capacity},
                             .demand_share = k / replicas,
                             .copies = 1,
                             .request_share = k / replicas});
  if (k >= replicas) return;  // the hot copy takes everything
  groups.push_back(CopyGroup{.queue = fluid::FluidQueue{resolved.capacity},
                             .demand_share = (replicas - k) / (replicas * (replicas - 1)),
                             .copies = replicas - 1,
                             .request_share = (replicas - k) / replicas});
}

// A SQL database (plan 2b refinement 3, behaviours::SqlDbBehaviour; book ch. 1 "database
// replication" and "sharding") with N = replicas copies per shard and S = shards is TWO kinds
// of copy, each copy its own queue of the node's capacity (concurrency and rate bounded by
// `pool`, graph::apply_database_keys). graph::database_shares says what one copy of each kind is
// sent; in short:
//                 copies        one copy is sent                       answers
//   primaries     S             writes / S                             the writes
//   replicas      (N − 1) S     reads / ((N − 1) S) + committed / S    the reads
// Every replica replays every write its shard's primary COMMITTED (its log holds nothing the
// primary dropped), so its load is its share of the reads plus all of its shard's committed
// writes: with N = 3, 1000 reads/s and 60 writes/s at the default write cost of 5 (300
// units/s) on copies of 1k/s, each of the 2 replicas carries 500 + 300 = 800 units/s (rho 0.8)
// and the primary 300 (rho 0.3). The primaries come first so that serve() knows what they
// committed before it serves the replicas. The replicas' writes answer no client (WriteShares
// request share 0): a write's response is the primary's sojourn, a read's a replica's
// (own_sojourn). With N = 1 there are no replicas: S primaries take reads and writes alike,
// (reads + writes) / S each. Each group's CopyRole says which it is.
// Pattern: Strategy (data-driven), as in make_server — a fourth replica model is only another
// set of copy groups (with WriteShares for the writes); serve() runs it like the others.
// A balancer in front changes nothing here (graph::Graph::mark_balanced_targets marks no pool
// or hot copy on a database): the database's driver, not a load balancer, decides that writes
// go to the primary and reads to a replica, and a sharding key spreads the shards evenly.
Simulation::Node Simulation::make_database(behaviours::Behaviour behaviour,
                                           const graph::ResolvedNode& resolved) {
  const graph::DatabaseShares shares = graph::database_shares(resolved.spec);
  const bool has_replicas = shares.replicas > 0.0;
  Node node{.behaviour = std::move(behaviour),
            .replication =
                Replication{.lag_s = seconds(resolved.spec.replication_lag.value_or(SimTime{0})),
                            .has_replicas = has_replicas}};
  if (!has_replicas) {
    node.copy_groups.push_back(CopyGroup{.queue = fluid::FluidQueue{resolved.capacity},
                                         .role = CopyRole::primary,
                                         .demand_share = shares.primary_reads,
                                         .copies = shares.primaries,
                                         .request_share = 1.0});
    return node;
  }
  node.copy_groups.push_back(CopyGroup{
      .queue = fluid::FluidQueue{resolved.capacity},
      .role = CopyRole::primary,
      .demand_share = shares.primary_reads,  // 0: a primary serves no reads while replicas exist
      .copies = shares.primaries,
      .request_share = 0.0,
      .writes = WriteShares{.demand_share = shares.primary_writes, .request_share = 1.0}});
  node.copy_groups.push_back(CopyGroup{
      .queue = fluid::FluidQueue{resolved.capacity},
      .role = CopyRole::replica,
      .demand_share = shares.replica_reads,
      .copies = shares.replicas,
      .request_share = 1.0,
      .writes = WriteShares{.demand_share = shares.replica_writes, .request_share = 0.0}});
  return node;
}

// How many steps the DelayLine itself must hold. A forward link's source runs before
// its target in the same step (push, then pop), so the line delays by exactly
// `delay_steps`. A feedback link's target runs BEFORE its source (that is what makes it
// feedback): it pops before this step's push, which already costs one step. So the line
// holds one step fewer, and the total is still `delay_steps` (>= 1 by Graph's
// construction): a zero-latency loop is read one step later, "cycles use the previous
// step's value" (spec §3.1).
int Simulation::line_steps(const graph::ResolvedLink& link) {
  return link.feedback ? link.delay_steps - 1 : link.delay_steps;
}

// Every demand edge makes its link an output of the sender and an input of the receiver.
// One link carries demand one way only, so each link is pushed by one node and popped by
// one node, once per step.
void Simulation::connect(const graph::Graph& g) {
  for (const auto& edge : g.edges()) {
    add_once(nodes_[edge.from].outgoing_links, edge.link);
    add_once(nodes_[edge.to].incoming_links, edge.link);
  }
}

// Gives every (node, op) that some flow can bring to the node a response-table entry up
// front: the ops of the client routes entering it, of the unconditional steps targeting it
// (the only steps that forward flow in phase 1) and those a load balancer in front passes
// on (graph::Graph::arriving_ops collects all three). The backward pass refreshes
// every entry from step 0, so while a hop's flow is still in a delay line the caller
// counts the target's empty-queue response (service time plus its own hops), which is
// what a first request would see, instead of zero.
void Simulation::register_reachable_ops(const graph::Graph& g) {
  for (std::size_t node = 0; node < g.nodes().size(); ++node) {
    for (const auto& op : g.arriving_ops(node)) nodes_[node].responses.try_emplace(op);
  }
}

// ---- Simulation: one evaluation ----

StepRecord Simulation::step(SimTime now, SimTime dt) {
  require_scenario_step(dt);
  return evaluate(now, dt, Mode::advance);
}

void Simulation::require_valid(const RunOptions& options) const {
  if (options.step.count() <= 0) throw std::invalid_argument{"Simulation::run: step must be > 0"};
  require_scenario_step(options.step);
  if (options.duration.count() < 0) {
    throw std::invalid_argument{"Simulation::run: duration must be >= 0"};
  }
}

// `.count()`: chrono's non-member `!=` is invisible in this interface (see `run`).
void Simulation::require_scenario_step(SimTime dt) const {
  if (dt.count() != step_.count()) {
    throw std::invalid_argument{
        "Simulation: the step must be the scenario's (delay lines are sized for it)"};
  }
}

// The fixed point of the settle passes: run the same pass again and again until its
// output stops changing (fixed-point iteration). Each pass settles every queue at its
// demand and lets links pass flow straight through, in topological order. A feedback
// link is read before its source has run, so each pass carries a loop's flow one lap
// further: a loop of gain g (the fraction of its flow it sends round again) adds g^k of
// the input on lap k, and its demand tends to the geometric series' sum, input / (1 - g).
// The passes stop when nothing changes by more than settle_tolerance, which a loop of
// gain g reaches after about log(tolerance) / log(g) passes (40 for g = 0.5, 260 for
// g = 0.9). An acyclic graph takes two (the second confirms the first).
//
// Why it converges whenever a steady state exists: starting from no flow at all, demand
// can only rise from pass to pass (every node forwards a non-decreasing function of what
// it receives), and what a loop sends round is bounded by the drain of its nodes, which
// capacity caps. A rising, bounded sequence converges, here to the smallest steady state.
//
// A loop of gain >= 1 climbs until a node on it saturates; that state is steady (the node
// drops the excess) and is reported. If saturation is further away than the cap allows
// (each pass adds the loop's input once, so it takes about capacity / input passes), or
// the gain is so close to one that log(tolerance) / log(g) exceeds the cap, the result is
// the last pass with `converged` false: no steady state in reach.
StepRecord Simulation::estimate() const {
  Simulation scratch = settling_copy();  // estimate must not move a running simulation
  StepRecord previous = scratch.evaluate(SimTime{0}, step_, Mode::settle);
  for (int pass = 2; pass <= max_settle_passes; ++pass) {
    StepRecord current = scratch.evaluate(SimTime{0}, step_, Mode::settle);
    if (settled(previous, current)) return current;
    previous = std::move(current);
  }
  previous.converged = false;
  return previous;
}

// A copy that remembers nothing of the steps this simulation ran, so estimate() gives
// the same answer before and after a run: no flow on any link, no queue content, no
// response yet (the response tables keep their ops, see register_reachable_ops), and
// every client's noise back at multiplier 1 (settle mode never ticks it again). Delay
// lines are not copied: settle mode reads `sent` directly, so each link gets an empty
// one-slot line (a million-step link would otherwise copy a million Flows). The queues
// need no reset either: settle mode sets each backlog from the demand (FluidQueue::settle).
Simulation Simulation::settling_copy() const {
  Simulation scratch;
  scratch.order_ = order_;
  scratch.step_ = step_;
  scratch.nodes_ = nodes_;
  for (auto& node : scratch.nodes_) {
    node.inflow.clear();
    for (auto& group : node.copy_groups) group.waiting.clear();
    for (auto& [op, answer] : node.responses) answer = Answer{};
    node.dropped.clear();
    node.passed = 1.0;
    node.passed_writes = 1.0;
    if (auto* client = std::get_if<behaviours::ClientBehaviour>(&node.behaviour)) {
      client->reset_noise();
    }
    // Every settle pass starts at t = 0, so a window would never close and a full bucket
    // would take burst / ((in - limit) x dt) passes to drain: the scratch limiter admits
    // min(in, limit), each algorithm's long-run rate (RateLimiterBehaviour::use_long_run_rate).
    if (auto* limiter = std::get_if<behaviours::RateLimiterBehaviour>(&node.behaviour)) {
      limiter->use_long_run_rate();
    }
  }
  scratch.links_.assign(links_.size(), Link{.line = DelayLine{0}});
  return scratch;
}

StepRecord Simulation::evaluate(SimTime now, SimTime dt, Mode mode) {
  StepRecord record{.t = now, .nodes = std::vector<NodeSample>(nodes_.size())};
  forward_pass(now, dt, mode, record.nodes);
  backward_pass();
  record.responses = client_responses(now);
  return record;
}

// Forward pass (spec §3.2-3.4), nodes in topological order. For each node:
//   1. receive: pop every incoming link into the node's inflow;
//   2. a client offers its load (in advance mode it first ticks its noise by dt; settle
//      mode never ticks, so estimate sees the curve without noise at the time it passes,
//      t = 0); any other node (service, load balancer, rate limiter) runs its fluid queue
//      on the inflow and routes the part it drained (backlog included, with the queue's op
//      mix: see serve) for the step [now, now + dt); what the route rejects is recorded
//      and goes nowhere (account_rejections);
//   3. send: accumulate the routed flow per link and push it into each link's delay line.
// Why this order is right: a forward link's target comes later in the order, so it pops
// after this step's push (a zero-step link delivers in the same step). A feedback link's
// target came earlier, so it already popped and gets this push next step. Every link is
// pushed exactly once per step (an empty flow if the source routed nothing on it) and
// popped exactly once, which keeps each delay line's ring in step with the clock.
void Simulation::forward_pass(SimTime now, SimTime dt, Mode mode,
                              std::vector<NodeSample>& samples) {
  for (const std::size_t index : order_) {
    receive(index, mode);
    // A client's sample stays all zeros, green: a source has no queue.
    const behaviours::Routed routed = std::visit(
        Overloaded{[&](behaviours::ClientBehaviour& client) {
                     if (mode == Mode::advance) client.tick(now, dt);
                     return client.offer(now);
                   },
                   // A generic lambda: every other alternative is a queueing node.
                   // Overload resolution prefers the non-template ClientBehaviour&
                   // lambda for a client, so this one sees only servers.
                   [&](auto& server) {
                     return server.route(serve(index, dt, mode, samples[index]), now, dt);
                   }},
        nodes_[index].behaviour);
    account_rejections(index, routed, samples[index]);
    send(index, routed, mode);
  }
}

// What a node refused and what it passed on this step:
//  - `rejected_per_s` is what the behaviour refused; for a leaky bucket, whose behaviour
//    refuses nothing, it is the bucket's overflow, moved out of `dropped_per_s` (the queue
//    reports it as a drop, but "bucket full, 429" is the policy, not a failure);
//  - `passed` = 1 − (dropped + rejected) / inflow, in requests: the node's own factor in its
//    delivered fraction. Only a loss makes a request unanswered: one that waits in a growing
//    queue is answered later, so a backlog is neutral (answered = offered − lost, every step).
//    The pairing is per step: this step's drops are set against this step's arrivals, though
//    the well-mixed queue (see serve) drops older requests too. In a steady state it equals
//    served / inflow, the share used before: with the backlog still, drain + drops = inflow,
//    and served = drain − rejected. Out of one, served / inflow (review of 3e23477, I2) counted
//    every request still waiting at the end of its arrival step as lost and never credited it
//    later: a 3x spike into a deep queue, nothing dropped, reported 583/s answered of 667/s.
//    No arrivals: 1 (nothing was lost). A database's reads and writes queue at different
//    copies (make_database), so it keeps one factor for each.
void Simulation::account_rejections(std::size_t index, const behaviours::Routed& routed,
                                    NodeSample& sample) {
  auto& node = nodes_[index];
  sample.rejected_per_s = routed.rejected.total().per_second;
  const auto* limiter = std::get_if<behaviours::RateLimiterBehaviour>(&node.behaviour);
  if (limiter != nullptr && limiter->refuses_on_overflow()) {
    sample.rejected_per_s += std::exchange(sample.dropped_per_s, 0.0);
  }
  // A leaky bucket's overflow is in `dropped` (its queue's) and not in `rejected` (its
  // behaviour refuses nothing): the sum counts it once.
  flow::Flow lost = node.dropped;
  lost += routed.rejected;
  if (!node.replication) {
    node.passed = kept_share(lost, node.inflow);
    return;
  }
  const auto [lost_reads, lost_writes] = split_writes(lost, node.behaviour);
  const auto [arrived_reads, arrived_writes] = split_writes(node.inflow, node.behaviour);
  node.passed = kept_share(lost_reads, arrived_reads);
  node.passed_writes = kept_share(lost_writes, arrived_writes);
}

// In settle mode the links have no delay: in the long run a link delivers exactly what
// it is sent, so the target reads its source's latest output directly.
void Simulation::receive(std::size_t index, Mode mode) {
  auto& node = nodes_[index];
  node.inflow.clear();
  for (const std::size_t link : node.incoming_links) {
    if (mode == Mode::advance) {
      node.inflow += links_[link].line.pop();
    } else {
      node.inflow += links_[link].sent;
    }
  }
}

// One step of a queueing node (service, load balancer, rate limiter): the group's arrivals
// priced in capacity units, and for each copy group (one copy of N, the pool, or the hot and
// the cool copies, see make_server) its share of them through the §3.3 fluid step (settled
// first when estimating). What the groups drained goes back, summed and converted to
// requests, for the node's behaviour to route downstream (forward_pass). The sample adds
// the groups' totals; its rho and zone are the busiest group's (the hottest copy behind a
// ring, the busier of a database's primary and replicas); its latency is the
// request-share-weighted mixture of the groups' sojourns (record_sojourns), which with one
// group is that group's sojourn.
//
// Capacity units (plan 2a refinement 4): the queue holds WORK, not requests. An op of
// cost c_op arriving at r_op req/s brings r_op × c_op units/s (in_units), the
// FluidQueue serves `capacity` units/s, and everything below is in units. Only on the
// way out does a drained op become requests again: forwarded units / c_op (in_requests).
//
// The FluidQueue knows only totals (one backlog number); each copy group's `waiting` adds
// which ops are queued. Model: the queue is well-mixed (random-order) fluid, not FIFO, and
// drops are taken proportionally, not from the tail. Every unit of work in it, whether it
// waited from an earlier step or arrived in this one, is equally likely to be among the
// drained, the dropped or the still waiting, so each part has the content's op mix in
// units:
//   content   = waiting + arrivals × dt                     units per op
//   forwarded = content × drained / content_total           drained = drain × dt
//   dropped   = content × dropped / content_total           discarded: never propagates
//   waiting'  = content × backlog / content_total           = content − forwarded − dropped
// Mixing in units, not requests, means a cost-4 op takes four times the drain of a cost-1
// op per request: ops x (cost 1) and y (cost 4) at 500 req/s each bring 500 + 2000 units/s;
// a 1000 units/s node drains them 1:4 in units, 200 + 800 units/s, which is 200 x and 200 y
// requests per second.
// The totals come from FluidQueue::advance, which stays the single source of truth;
// waiting' is computed from the backlog directly rather than by subtraction, so it
// cannot go negative by cancellation, and its total matches the backlog to rounding.
// This is what makes "what a node drains is what the next node receives" hold even with
// no arrivals at all: a node working off its backlog forwards the queued ops, not this
// step's (empty) mix. (A rate curve that falls to zero, e.g. points ending at 0/s,
// reaches that zero-arrival case.)
// With a constant op mix the order makes no difference. A rate curve scales a client's
// total, not its op shares, so one client's mix is constant; the mix at a node varies when
// clients with different curves, or paths of different delay, feed it. Then a FIFO queue
// would forward the old mix first and this one forwards a blend.
// Ignored: FIFO order, tail drop (a full real queue rejects the newest arrivals),
// priorities and per-op service times (a costly op uses more capacity, not a longer
// sojourn: see ServiceBehaviour). The one exception is a byte-sized node, whose ops are the
// megabytes they move: its queue waits for the mean whole object (time_whole_objects) and each
// op is answered at its own time after that wait (own_sojourn).
flow::Flow Simulation::serve(std::size_t index, SimTime dt, Mode mode, NodeSample& sample) {
  auto& node = nodes_[index];
  time_whole_objects(node);
  // Not const: `writes` becomes the primaries' committed writes below.
  auto [reads, writes] = split_writes(in_units(node.inflow, node.behaviour), node.behaviour);
  flow::Flow drained;
  flow::Flow dropped;            // units, the clients' part (account_rejections reads it)
  ReplicationTally replication;  // a database's committed and missed writes (replay_committed)
  sample = NodeSample{};
  // At most two groups (max_copy_groups): fixed arrays, no allocation per step.
  std::array<flow::LatencyEstimate, max_copy_groups> sojourns{};
  std::array<flow::LatencyEstimate, max_copy_groups> waits{};
  for (std::size_t i = 0; i < node.copy_groups.size(); ++i) {
    auto& group = node.copy_groups[i];
    GroupStep step =
        serve_copies(group, group_arrivals(group, reads, writes), dt, mode, node.behaviour, sample);
    // The busiest kind of copy speaks for the node's load: the hottest copy behind a ring (it
    // takes the largest share), the busier of a database's primary and replicas.
    if (i == 0 || step.copy.rho > sample.rho) {
      sample.rho = step.copy.rho;
      sample.zone = step.copy.zone;
    }
    sojourns[i] = step.copy.latency;
    waits[i] = step.copy.wait;
    replay_committed(group, step, node.behaviour, writes, replication);
    drained += clients_part(group, std::move(step.drained), node.behaviour);
    dropped += clients_part(group, std::move(step.dropped), node.behaviour);
  }
  node.dropped = in_requests(dropped, node.behaviour);
  // std::span with C++17 class template argument deduction: `std::span{sojourns}` deduces
  // std::span<LatencyEstimate, max_copy_groups> from the array without spelling the type, and
  // first(n) views only the n groups in use.
  const auto in_use = std::span{sojourns}.first(node.copy_groups.size());
  record_sojourns(node, in_use, std::span{waits}.first(node.copy_groups.size()), sample);
  if (node.replication) record_replication(node, in_use, replication, sample);
  return in_requests(drained, node.behaviour);
}

// What one copy of `group` is sent: `demand_share` of the node's reads (of every op, for a node
// other than a database) and, on a database, its WriteShares' `demand_share` of the writes
// (the clients' at a primary, the primaries' committed ones at a replica: see serve).
flow::Flow Simulation::group_arrivals(const CopyGroup& group, const flow::Flow& reads,
                                      const flow::Flow& writes) {
  flow::Flow arrivals = reads.scaled(group.demand_share);
  if (writes.empty()) return arrivals;  // every node but a database: no writes kept apart
  arrivals += writes.scaled(group.writes ? group.writes->demand_share : group.demand_share);
  return arrivals;
}

// A database's replication through one copy group's step (book ch. 1, "database replication"):
//  - at the primaries: a replica replays the primaries' log, which holds what they COMMITTED,
//    not what the clients offered, so from here on `writes` are the writes the primaries drained
//    (make_database serves them first). One shard's committed writes, in requests, are the
//    group's total / S (S = the primaries' copies): what each replica of the shard must replay.
//  - at the replicas: the replayed writes the group dropped, in requests, / ((N − 1) S) (the
//    replicas' copies): what ONE replica missed, comparable with the committed per shard.
// A server group (every node but a database) leaves both untouched.
void Simulation::replay_committed(const CopyGroup& group, const GroupStep& step,
                                  const behaviours::Behaviour& behaviour, flow::Flow& writes,
                                  ReplicationTally& tally) {
  const auto requests_per_copy = [&](const flow::Flow& write_units) {
    return in_requests(write_units, behaviour).total().per_second / group.copies;
  };
  if (group.role == CopyRole::primary) {
    writes = split_writes(step.drained, behaviour).second;
    tally.committed_per_shard = requests_per_copy(writes);
  } else if (group.role == CopyRole::replica) {
    tally.missed_per_replica = requests_per_copy(split_writes(step.dropped, behaviour).second);
  }
}

// The part of `units` (per op) that is the clients' work at this group: all of it, except the
// writes a database replica replays, which answer no client (WriteShares::request_share 0).
// Taken by value: every group but a replica hands `units` straight back, moved.
flow::Flow Simulation::clients_part(const CopyGroup& group, flow::Flow units,
                                    const behaviours::Behaviour& behaviour) {
  if (group.role != CopyRole::replica) return units;
  flow::Flow part;
  for (const auto& [op, rate] : units.entries()) {
    if (!is_write_at(behaviour, op)) part.add(op, rate);
  }
  return part;
}

// The clients' share of `units` at this group (clients_part over the whole): 1 except at a
// replica, without copying anything for the groups where it is 1.
double Simulation::clients_share(const CopyGroup& group, const flow::Flow& units,
                                 const behaviours::Behaviour& behaviour) {
  if (group.role != CopyRole::replica) return 1.0;
  return share_of(clients_part(group, units, behaviour), units);
}

// The sojourn an answered request sees at the node (the backward pass reads them through
// own_sojourn): the mixture over the groups that answer it, by the share each answers
// (behaviours::mixture; with one group, that group's sojourn exactly). Reads and writes are
// mixed apart, by `request_share` and by WriteShares::request_share: on a database a write sees
// its primary, a read a replica; on every other node the two are the same. The node's latency
// is the two mixed by the requests of each (request_weighted_sojourn).
// A group that answers none of a kind is left out of that kind's mixture, so a saturated
// primary (an infinite sojourn) never reaches the reads as 0 × inf.
// The groups' waits alone (`waits`) are mixed the same way, into the node's `wait`: a byte-sized
// node answers each op at its own time after it (own_sojourn), and its latency is those ops'
// sojourns mixed by their requests (request_weighted_megabyte_sojourn). Mixing the waits first
// and adding an op's service after is the same mixture: the service does not depend on the group.
void Simulation::record_sojourns(Node& node, std::span<const flow::LatencyEstimate> sojourns,
                                 std::span<const flow::LatencyEstimate> waits, NodeSample& sample) {
  const std::span<const CopyGroup> groups = node.copy_groups;
  const auto mixed = [&](std::span<const flow::LatencyEstimate> estimates, auto request_share) {
    std::array<behaviours::Branch, max_copy_groups> branches{};
    std::size_t used = 0;
    for (std::size_t i = 0; i < groups.size(); ++i) {
      const double share = request_share(groups[i]);
      if (share > 0.0) branches[used++] = {.share = share, .latency = estimates[i]};
    }
    if (used == 1) return branches[0].latency;
    return behaviours::mixture(std::span{branches}.first(used));
  };
  const auto read_share = [](const CopyGroup& g) { return g.request_share; };
  node.read_sojourn = mixed(sojourns, read_share);
  node.wait = mixed(waits, read_share);
  if (const auto* megabytes = megabytes_of(node.behaviour)) {
    node.write_sojourn = node.read_sojourn;
    sample.latency = request_weighted_megabyte_sojourn(node, *megabytes);
    return;
  }
  if (!node.replication) {
    node.write_sojourn = node.read_sojourn;
    sample.latency = node.read_sojourn;
    return;
  }
  node.write_sojourn = mixed(sojourns, [](const CopyGroup& g) {
    return g.writes ? g.writes->request_share : g.request_share;
  });
  sample.latency = request_weighted_sojourn(node);
}

// A byte-sized node's queue waits for whole objects (plan 2c refinement 2; review of Task 3 at
// a8b6cb4, I1): each request holds one of its c megabyte slots for its whole transfer, so the
// time Erlang C is fed is the rate-weighted mean OBJECT time of this step's arrivals,
//   s_obj = Σ_op rate_op × MB_op × s_MB / Σ_op rate_op    (behaviours::MegabyteService)
// recomputed every step as the op mix moves, like the per-op costs. c and the rate stay those of
// the megabytes (c = MB/s × s_MB), so rho is unchanged (Little's law: objects/s × s_obj / c =
// MB/s / capacity) and only the wait becomes a whole object's: 5 uploads/s of 50 MB at a default
// store wait C(20, 0.5) × 2 s / (20 × 0.5) = 0.746 ms, not the 0.0149 ms of 1 MB units. A step
// in which nothing arrives keeps the last mix's time (it was one megabyte's before any).
void Simulation::time_whole_objects(Node& node) {
  const auto* megabytes = megabytes_of(node.behaviour);
  if (megabytes == nullptr) return;
  const auto object_time_s = megabytes->mean_object_time_s(node.inflow);
  if (!object_time_s) return;
  for (auto& group : node.copy_groups) group.queue.set_service_time(*object_time_s);
}

// A byte-sized node's latency: each op's own sojourn (own_sojourn) mixed by the requests of it
// that arrived this step, what an average request saw, as a database mixes its reads and writes
// (request_weighted_sojourn). Nothing arrived: what a read arriving now would see, the wait plus
// one megabyte.
flow::LatencyEstimate Simulation::request_weighted_megabyte_sojourn(
    const Node& node, const behaviours::MegabyteService& megabytes) {
  const double total_per_s = node.inflow.total().per_second;
  if (total_per_s <= 0.0) return megabytes.first_megabyte(node.wait);
  std::vector<behaviours::Branch> branches;
  for (const auto& [op, rate] : node.inflow.entries()) {
    if (rate.per_second <= 0.0) continue;
    branches.push_back(
        {.share = rate.per_second / total_per_s, .latency = megabytes.sojourn(op, node.wait)});
  }
  return behaviours::mixture(branches);
}

// A database's latency: its read and its write sojourns mixed by the requests of each that
// arrived this step, what an average request saw. Only one kind arrived: that kind's sojourn.
flow::LatencyEstimate Simulation::request_weighted_sojourn(const Node& node) {
  const auto [reads, writes] = split_writes(node.inflow, node.behaviour);
  const double read_rate = reads.total().per_second;
  const double write_rate = writes.total().per_second;
  if (write_rate <= 0.0) return node.read_sojourn;
  if (read_rate <= 0.0) return node.write_sojourn;
  const double read_share = read_rate / (read_rate + write_rate);
  // C++20 std::to_array: a std::array from a braced list, its size deduced (two branches),
  // replacing `std::array<Branch, 2>{...}` whose count is restated by hand.
  return behaviours::mixture(std::to_array<behaviours::Branch>(
      {{.share = read_share, .latency = node.read_sojourn},
       {.share = 1.0 - read_share, .latency = node.write_sojourn}}));
}

// A database's replication numbers (plan 2b refinement 3; NodeSample::staleness_s,
// replication_dropped_per_s and committed_writes_per_s, the last two from replay_committed).
// Staleness: a write a client sent shows at a replica after
//   primary sojourn (wait + service: committed) + replication lag (shipped)
//   + replica sojourn (waiting behind the reads and the other replayed writes, then applied)
// the primaries' and the replicas' sojourns being the first and the second group's
// (make_database). Assumes a replayed write waits like any query in the replica's queue (one
// well-mixed queue, see serve); ignored: the replica applying the log on a thread of its own.
// A replica that drops replayed writes falls behind without bound (the model drops the excess
// at queue_limit; a real replica keeps it in its relay log/WAL until it is rebuilt): the
// staleness here is then the bounded queue's, finite, and the `staleness` SLO fails through
// the missed writes instead (report's note).
void Simulation::record_replication(const Node& node,
                                    std::span<const flow::LatencyEstimate> sojourns,
                                    const ReplicationTally& tally, NodeSample& sample) {
  sample.replication_dropped_per_s = tally.missed_per_replica;
  sample.committed_writes_per_s = tally.committed_per_shard;
  if (!node.replication->has_replicas) {
    sample.staleness_s = 0.0;  // every read is the primary's: it has the latest write
    return;
  }
  sample.staleness_s = sojourns[0].mean_s + node.replication->lag_s + sojourns[1].mean_s;
}

// One copy group's step (replicas, plan refinement 4): `waiting` and the queue describe one
// copy, which is sent `arrivals` (group_arrivals). Returns one copy's fluid step and what the
// group's copies drained (and, at a database replica, dropped), and adds the clients' part
// (clients_share) of their totals to `sample` (add_copies).
Simulation::GroupStep Simulation::serve_copies(CopyGroup& group, const flow::Flow& arrivals,
                                               SimTime dt, Mode mode,
                                               const behaviours::Behaviour& behaviour,
                                               NodeSample& sample) {
  const Rate per_copy = arrivals.total();
  if (mode == Mode::settle) {
    group.queue.settle(per_copy);
    group.waiting = settled_waiting(arrivals, group.queue.backlog());
  }
  const flow::Flow content = queue_content(group.waiting, arrivals, dt);
  GroupStep step{.copy = group.queue.advance(per_copy, dt)};
  step.drained = take_drained(group, content, step.copy);
  // Every group's drops, per op: a replica's replayed writes it missed (replay_committed) and,
  // on every node, the requests lost (account_rejections).
  step.dropped = dropped_part(content, step.copy, group.copies);
  // The queue is well-mixed (see serve): what it drains, drops and holds has the content's op
  // mix, so the clients' share of the content is their share of each of those.
  add_copies(sample, step.copy, group.copies, clients_share(group, arrivals, behaviour),
             clients_share(group, content, behaviour));
  return step;
}

// What the group's copies dropped, per op: the content's mix (the queue is well-mixed, see
// serve) scaled to the dropped total, content × (dropped_per_s × copies) / content_total.
flow::Flow Simulation::dropped_part(const flow::Flow& content, const fluid::FluidStep& copy_step,
                                    double copies) {
  const double content_total = content.total().per_second;  // units, see CopyGroup::waiting
  if (content_total <= 0.0) return flow::Flow{};
  return content.scaled(copy_step.dropped_per_s * copies / content_total);
}

// Splits one copy's content (see serve) by the totals of its fluid step: keeps the
// still-waiting part in `group.waiting` and returns the drained part as the group's
// forwarded rate per op in units, content × (drain_per_s × copies) / content_total: the
// drained units per copy (drain × dt) over dt, times the copies. An empty queue with no
// arrivals has nothing to split.
flow::Flow Simulation::take_drained(CopyGroup& group, const flow::Flow& content,
                                    const fluid::FluidStep& copy_step) {
  const double content_total = content.total().per_second;  // units, see CopyGroup::waiting
  if (content_total <= 0.0) {
    group.waiting.clear();
    return flow::Flow{};
  }
  group.waiting = content.scaled(copy_step.backlog / content_total);
  return content.scaled(copy_step.drain_per_s * group.copies / content_total);
}

void Simulation::send(std::size_t index, const behaviours::Routed& routed, Mode mode) {
  const auto& outgoing_links = nodes_[index].outgoing_links;
  for (const std::size_t link : outgoing_links) links_[link].sent.clear();
  for (const auto& out : routed.outgoing) links_[out.link].sent += out.flow;
  if (mode == Mode::settle) return;  // estimate reads `sent` directly; lines stay untouched
  for (const std::size_t link : outgoing_links) links_[link].line.push(links_[link].sent);
}

// Backward pass (plan refinement 1), nodes in reverse topological order: a server's
// response to op = own sojourn this step (own_sojourn: a database's write the primary's, its
// read a replica's) + its hops (ServiceBehaviour::response) or its
// share-weighted routes (LoadBalancerBehaviour::response). Reverse
// order means every target later in the order was already refreshed this step.
// Asynchronous hops (those closing an op-level call cycle, a write_back cache's flushes and a
// blob store's event notifications: graph::Graph::is_async_hop) add no latency and fail no
// request, so they are never looked up. A hop over a node-level back edge that
// closes no call cycle targets a node earlier in the order, not yet refreshed: it reads
// that node's previous-step entry, one step stale (the same "previous step's value" rule
// as the forward pass).
// The tables hold every op that can reach the node from construction on
// (register_reachable_ops) and refresh all of them every step, so an op whose flow has
// not arrived yet, or has paused, is estimated at the node's current load.
//
// Next to each latency it composes the DELIVERED fraction (plan 2a refinement 8): the share
// of the node's requests of that op that come back answered, the node's own `passed` share
// times what its downstream calls deliver (behaviours::NodeBehaviour::delivered: sequential
// parts multiply, splits mix by share). A feedback target is read one step stale, as for
// latency.
void Simulation::backward_pass() {
  const auto lookup = [this](std::size_t target, std::string_view op) -> flow::LatencyEstimate {
    return response_of(target, op);
  };
  const auto fraction = [this](std::size_t target, std::string_view op) {
    return delivered_of(target, op);
  };
  // C++20 std::views::reverse: walks the order back to front without copying it,
  // replacing a reverse-iterator loop (rbegin/rend) or a reversed copy.
  for (const std::size_t index : order_ | std::views::reverse) {
    auto& node = nodes_[index];
    // Only servers compose a response here; clients are read in client_responses().
    std::visit(Overloaded{[](const behaviours::ClientBehaviour&) {},
                          [&](const auto& server) {
                            for (auto& [op, answer] : node.responses) {
                              answer.latency = server.response(op, own_sojourn(node, op), lookup);
                              answer.delivered =
                                  server.delivered(op, own_passed(node, op), fraction);
                            }
                          }},
               node.behaviour);
  }
}

// Every client op in node order then op order, after the backward pass filled every
// service's table: a client's response is the share-weighted mixture over its routes.
// `offered` is what the client sent this step (curve × noise × op share), so reports
// weight each step's latency by the load that saw it. That pairing assumes the curve
// changes slowly relative to the end-to-end delay: this step's response is composed from
// queues fed by flow sent up to d steps ago (d = the path's delay in steps), so the latency
// a spike causes is attributed to the requests offered d steps later.
std::vector<ResponseSample> Simulation::client_responses(SimTime now) const {
  const auto lookup = [this](std::size_t target, std::string_view op) -> flow::LatencyEstimate {
    return response_of(target, op);
  };
  const auto fraction = [this](std::size_t target, std::string_view op) {
    return delivered_of(target, op);
  };
  std::vector<ResponseSample> responses;
  for (std::size_t index = 0; index < nodes_.size(); ++index) {
    const auto* client = std::get_if<behaviours::ClientBehaviour>(&nodes_[index].behaviour);
    if (client == nullptr) continue;
    for (const auto& [op, offered] : client->offered(now)) {
      responses.push_back(
          ResponseSample{.client = index,
                         .op = op,
                         .offered = offered,
                         .answered = Rate{offered.per_second * client->delivered(op, fraction)},
                         .response = client->response(op, lookup)});
    }
  }
  return responses;
}

// The node's own sojourn for `op` this step (record_sojourns): a database's write waits at
// its primary, its read at a replica; a byte-sized node answers a write at its last byte and a
// read at its first megabyte after the same wait (behaviours::MegabyteService::sojourn); any
// other node has one sojourn for every op.
flow::LatencyEstimate Simulation::own_sojourn(const Node& node, std::string_view op) {
  if (const auto* megabytes = megabytes_of(node.behaviour)) {
    return megabytes->sojourn(op, node.wait);
  }
  return is_write_at(node.behaviour, op) ? node.write_sojourn : node.read_sojourn;
}

// The node's own factor in the delivered fraction of `op` (account_rejections), split the
// same way.
double Simulation::own_passed(const Node& node, std::string_view op) {
  return is_write_at(node.behaviour, op) ? node.passed_writes : node.passed;
}

// Zero for an op no flow can bring to the node (and for a client, which has no table).
flow::LatencyEstimate Simulation::response_of(std::size_t node, std::string_view op) const {
  const auto& table = nodes_[node].responses;
  const auto found = table.find(op);
  return found == table.end() ? flow::LatencyEstimate{} : found->second.latency;
}

// One for an op no flow can bring to the node: nothing of it is lost there.
double Simulation::delivered_of(std::size_t node, std::string_view op) const {
  const auto& table = nodes_[node].responses;
  const auto found = table.find(op);
  return found == table.end() ? 1.0 : found->second.delivered;
}

}  // namespace simsys::engine
