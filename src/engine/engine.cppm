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
// consistent-hash balancer, those of the hottest copy, the one that breaks an SLO first —
// and latency is the sojourn a request sees: one copy's, or behind a hash ring the
// share-weighted mixture of the hot and the cool copies' (see Simulation::make_server).
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
struct NodeSample {
  double demand_per_s{};
  double drain_per_s{};
  double dropped_per_s{};
  double rejected_per_s{};
  double backlog{};
  double rho{};
  flow::LatencyEstimate latency;  // own sojourn; zero for a client (a source has no queue)
  fluid::Zone zone{fluid::Zone::green};
};

// What a client sees for one op: the rate it offers, the part of it that comes back
// answered, and the response time an answered request gets. `answered` = offered × the
// delivered fraction the backward pass composes along the op's path (plan 2a refinement 8,
// behaviours::NodeBehaviour::delivered): a request a limiter refused or a full queue dropped
// is not answered, and reports weigh latency by `answered` so that those requests, which
// never saw the path's latency, do not dilute it.
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
  struct CopyGroup {
    fluid::FluidQueue queue;
    double demand_share{1};
    double copies{1};
    double request_share{1};
    // What one copy's queue holds, per op, in capacity units (not per second; Flow is
    // reused as "a number per op"). Its total agrees with the queue's backlog to rounding.
    // See serve().
    flow::Flow waiting{};
  };

  struct Node {
    behaviours::Behaviour behaviour;
    // Every node but a client (a source) has one group (independent or pooled replicas) or
    // two (behind a hash ring: the hottest copy first, then the cool ones). See make_server.
    std::vector<CopyGroup> copy_groups{};
    std::vector<std::size_t> incoming_links{};  // links delivering demand here
    std::vector<std::size_t> outgoing_links{};  // links this node sends demand on
    flow::Flow inflow{};                        // this step's arrivals
    // The share of this step's arrivals the node passes on (served / inflow, at most 1): the
    // node's own factor in its delivered fraction (see forward_pass).
    double passed{1.0};
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
  [[nodiscard]] static fluid::FluidStep serve_copies(CopyGroup& group, const flow::Flow& units,
                                                     SimTime dt, Mode mode, flow::Flow& drained);
  [[nodiscard]] static flow::Flow take_drained(CopyGroup& group, const flow::Flow& content,
                                               const fluid::FluidStep& copy_step);
  void send(std::size_t index, const behaviours::Routed& routed, Mode mode);
  void account_rejections(std::size_t index, const behaviours::Routed& routed, NodeSample& sample);
  void backward_pass(const std::vector<NodeSample>& samples);
  [[nodiscard]] std::vector<ResponseSample> client_responses(SimTime now) const;
  [[nodiscard]] flow::LatencyEstimate response_of(std::size_t node, std::string_view op) const;
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
         type == cache;
}

// Why a node of a simulated type still cannot run, if it cannot: a cache whose model needs
// phase 2b (a `che` hit ratio, computed by the Che approximation; the write_back strategy, a
// queue of dirty entries flushed later; `then` steps, which fire per hit or miss) or that a
// scenario built in code left without a hit ratio (the loader requires one).
[[nodiscard]] std::optional<std::string> not_runnable_yet(const model::NodeSpec& spec) {
  if (spec.type != model::NodeType::cache) return std::nullopt;
  const std::string node = "node '" + spec.id + "': ";
  if (!spec.hit_ratio) return node + "cache needs a hit_ratio";
  if (spec.hit_ratio->che || !spec.hit_ratio->fixed) {
    return node + "hit_ratio che (the Che approximation) is computed from phase 2b on; " +
           "run/estimate need {fixed: h} until then";
  }
  if (spec.strategy == model::CacheStrategy::write_back) {
    return node + "strategy write_back (writes flushed later) runs from phase 2b on";
  }
  if (!spec.then.empty()) return node + "a cache's then steps run from phase 2b on";
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
// Simulation::make_server); every other node has one.
inline constexpr std::size_t max_copy_groups = 2;

// Adds `copies` identical copies, each of which took `copy_step`, to the group's totals
// (plan refinement 4): N copies behind an equal split each saw demand / N, so the group's
// rates and backlog are the copy's times N. A pooled group is one copy (its queue IS the
// group); a hash ring adds its hot copy (1) and its cool ones (N - 1). `rejected_per_s`
// stays 0: a queue never rejects; the forward pass fills it (account_rejections).
void add_copies(NodeSample& sample, const fluid::FluidStep& copy_step, double copies) {
  sample.demand_per_s += copy_step.demand_per_s * copies;
  sample.drain_per_s += copy_step.drain_per_s * copies;
  sample.dropped_per_s += copy_step.dropped_per_s * copies;
  sample.backlog += copy_step.backlog * copies;
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
// client, so adding a client never changes another client's noise (spec phase 2a
// refinement 3), and `simsys curve --noise` derives the same seed.
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
    // A cache's queue is the lookup itself (the type's 100k/s, 0.1 ms, or the scenario's
    // numbers); the store behind it, if its strategy calls one, is a node of its own.
    case model::NodeType::cache:
      return make_server(behaviours::CacheBehaviour{g, index}, resolved);
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
    node.passed = 1.0;
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
  backward_pass(record.nodes);
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
//  - `passed` = served / inflow (requests), the node's own factor in its delivered
//    fraction: a token-family limiter's admitted / offered, a queue's drain / demand. A
//    step that works off a backlog serves more than it receives; the excess answers
//    requests of earlier steps, which the per-step pairing cannot credit to them, so the
//    factor is capped at 1. No arrivals: 1 (nothing was lost).
void Simulation::account_rejections(std::size_t index, const behaviours::Routed& routed,
                                    NodeSample& sample) {
  auto& node = nodes_[index];
  sample.rejected_per_s = routed.rejected.total().per_second;
  const auto* limiter = std::get_if<behaviours::RateLimiterBehaviour>(&node.behaviour);
  if (limiter != nullptr && limiter->refuses_on_overflow()) {
    sample.rejected_per_s += std::exchange(sample.dropped_per_s, 0.0);
  }
  const double arrived = node.inflow.total().per_second;
  node.passed = arrived > 0.0 ? std::min(routed.served.total().per_second / arrived, 1.0) : 1.0;
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
// the groups' totals; its rho and zone are the first group's (the hottest copy behind a
// ring); its latency is the request-share-weighted mixture of the groups' sojourns
// (behaviours::mixture), which with one group is that group's sojourn.
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
// sojourn: see ServiceBehaviour).
flow::Flow Simulation::serve(std::size_t index, SimTime dt, Mode mode, NodeSample& sample) {
  auto& node = nodes_[index];
  const flow::Flow units = in_units(node.inflow, node.behaviour);
  flow::Flow drained;
  sample = NodeSample{};
  // At most two groups (max_copy_groups): a fixed array, no allocation per step.
  std::array<behaviours::Branch, max_copy_groups> sojourns{};
  for (std::size_t i = 0; i < node.copy_groups.size(); ++i) {
    auto& group = node.copy_groups[i];
    const fluid::FluidStep copy_step = serve_copies(group, units, dt, mode, drained);
    add_copies(sample, copy_step, group.copies);
    if (i == 0) {  // the hottest copy (or the only kind) speaks for the node's load
      sample.rho = copy_step.rho;
      sample.zone = copy_step.zone;
    }
    sojourns[i] = {.share = group.request_share, .latency = copy_step.latency};
  }
  sample.latency = node.copy_groups.size() == 1
                       ? sojourns[0].latency
                       : behaviours::mixture(std::span{sojourns}.first(node.copy_groups.size()));
  return in_requests(drained, node.behaviour);
}

// One copy group's step (replicas, plan refinement 4): `waiting` and the queue describe one
// copy, which sees demand_share of the group's arrivals; take_drained adds what all the
// group's copies forward to `drained`. Returns one copy's fluid step.
fluid::FluidStep Simulation::serve_copies(CopyGroup& group, const flow::Flow& units, SimTime dt,
                                          Mode mode, flow::Flow& drained) {
  const flow::Flow arrivals = units.scaled(group.demand_share);
  const Rate per_copy = arrivals.total();
  if (mode == Mode::settle) {
    group.queue.settle(per_copy);
    group.waiting = settled_waiting(arrivals, group.queue.backlog());
  }
  const flow::Flow content = queue_content(group.waiting, arrivals, dt);
  const fluid::FluidStep copy_step = group.queue.advance(per_copy, dt);
  drained += take_drained(group, content, copy_step);
  return copy_step;
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
// response to op = own sojourn this step + its hops (ServiceBehaviour::response) or its
// share-weighted routes (LoadBalancerBehaviour::response). Reverse
// order means every target later in the order was already refreshed this step.
// Asynchronous hops (those closing an op-level call cycle, graph::Graph::is_async_hop)
// add no latency, so they are never looked up. A hop over a node-level back edge that
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
void Simulation::backward_pass(const std::vector<NodeSample>& samples) {
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
                              answer.latency = server.response(op, samples[index].latency, lookup);
                              answer.delivered = server.delivered(op, node.passed, fraction);
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
