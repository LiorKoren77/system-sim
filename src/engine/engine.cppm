// Module `simsys.engine`: the step loop (spec §3). One step = a forward pass in
// topological order moving demand through delay lines and fluid queues, then a backward
// pass composing response latency per (node, op). `estimate` is the same evaluation at
// the long-run state. No randomness in phase 1, so runs are byte-identical by construction.
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
// (GCC 14.2 'Bad file data' bug, CLAUDE.md §3). This unit imports the whole module
// chain; it compiles only with -flarge-source-files (top-level CMakeLists.txt).
#include <algorithm>
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
class DelayLine {
 public:
  explicit DelayLine(int steps);   // steps >= 0; a negative count is a programmer error
  void push(flow::Flow arriving);  // what enters the link this step (exactly once per step)
  [[nodiscard]] flow::Flow pop();  // what leaves the link this step; empty if nothing

 private:
  std::vector<flow::Flow> slots_;
  std::size_t head_{0};
};

// One node's numbers for one step. Rates and backlog are summed over replicas (plan
// refinement 4); rho, latency and zone are per copy, which all copies share.
struct NodeSample {
  double demand_per_s{};
  double drain_per_s{};
  double dropped_per_s{};
  double backlog{};
  double rho{};
  flow::LatencyEstimate latency;  // own sojourn; zero for a client (a source has no queue)
  fluid::Zone zone{fluid::Zone::green};
};

// What a client sees for one op: the rate it offers and the response time it gets.
struct ResponseSample {
  std::size_t client{};
  std::string op{};
  Rate offered{};
  flow::LatencyEstimate response{};
};

// Everything one step produced: one NodeSample per node (indexed like Graph::nodes()) and
// one ResponseSample per (client, op), clients in node order, ops sorted by name.
struct StepRecord {
  SimTime t{};
  std::vector<NodeSample> nodes{};
  std::vector<ResponseSample> responses{};
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
  // Pattern: Factory method — `create` validates first, then calls the private
  // constructor, so a Simulation over an unsupported graph cannot exist.
  [[nodiscard]] static std::expected<Simulation, graph::EngineError> create(const graph::Graph& g);

  // Advances every node by `dt` from simulated time `now`. Throws std::invalid_argument
  // for dt <= 0 (FluidQueue::advance: a programmer error).
  [[nodiscard]] StepRecord step(SimTime now, SimTime dt);

  // The long-run state at the scenario's step, without disturbing this simulation: it
  // evaluates a copy with every queue settled and every link passing flow straight
  // through. Record time is 0.
  [[nodiscard]] StepRecord estimate() const;

  // Steps from t = 0 while t < duration (so duration / step records, the last at
  // duration - step) and hands every record to `observer`.
  // Pattern: Observer — the loop keeps no record itself, so a 24 h run at 100 ms steps
  // (864 000 records) uses no more memory than one step; reports, SLO checks and the CSV
  // series writer are observers that keep only what they need.
  // C++20 abbreviated function template with a constrained placeholder:
  // `StepObserver auto&&` reads "anything satisfying StepObserver", replacing
  // `template <class F> requires StepObserver<F> void run(RunOptions, F&&)`. Defined here
  // because a template's body must be visible to the importer that instantiates it.
  // Throws std::invalid_argument for step <= 0 or duration < 0 (programmer errors: the
  // loader validates the scenario's values).
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

  // Per-op response times of one node, kept across steps: a feedback target reads the
  // previous step's entry. std::less<> (transparent) lets find() take a string_view.
  using ResponseTable = std::map<std::string, flow::LatencyEstimate, std::less<>>;

  struct Node {
    behaviours::Behaviour behaviour;
    std::optional<fluid::FluidQueue> queue{};  // services only; a client is a source
    double replicas{1};
    std::vector<std::pair<std::string, Rate>> offered{};  // clients: base × share per op
    std::vector<std::size_t> incoming_links{};            // links delivering demand here
    std::vector<std::size_t> outgoing_links{};            // links this node sends demand on
    flow::Flow inflow{};                                  // this step's arrivals
    // Services: what one copy's queue holds, per op, in requests (not per second; Flow
    // is reused as "a number per op"). Its total agrees with the queue's backlog to
    // rounding. See serve().
    flow::Flow waiting{};
    ResponseTable responses{};
  };

  struct Link {
    DelayLine line;
    flow::Flow sent{};  // what the source put on the link in the latest evaluation
  };

  explicit Simulation(const graph::Graph& g);
  static void require_valid(const RunOptions& options);
  [[nodiscard]] static Node make_node(const graph::Graph& g, std::size_t index);
  [[nodiscard]] static int line_steps(const graph::ResolvedLink& link);
  void connect(const graph::Graph& g);

  [[nodiscard]] StepRecord evaluate(SimTime now, SimTime dt, Mode mode);
  void forward_pass(SimTime now, SimTime dt, Mode mode, std::vector<NodeSample>& samples);
  void receive(std::size_t index, Mode mode);
  [[nodiscard]] behaviours::Routed serve(std::size_t index,
                                         const behaviours::ServiceBehaviour& service, SimTime dt,
                                         Mode mode, NodeSample& sample);
  [[nodiscard]] static flow::Flow take_drained(Node& node, const flow::Flow& content,
                                               const fluid::FluidStep& copy_step);
  void send(std::size_t index, const behaviours::Routed& routed, Mode mode);
  void backward_pass(const std::vector<NodeSample>& samples);
  [[nodiscard]] std::vector<ResponseSample> client_responses() const;
  [[nodiscard]] flow::LatencyEstimate response_of(std::size_t node, std::string_view op) const;

  std::vector<Node> nodes_;
  std::vector<Link> links_;
  std::vector<std::size_t> order_;
  SimTime step_{};
  int settle_passes_{1};
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

[[nodiscard]] bool supported_in_phase_1(model::NodeType type) {
  return type == model::NodeType::client || type == model::NodeType::service;
}

// Offered rate per op of a client: base × op share, sorted by op (reports list ops in
// name order).
[[nodiscard]] std::vector<std::pair<std::string, Rate>> offered_rates(const model::NodeSpec& spec) {
  std::vector<std::pair<std::string, Rate>> offered;
  if (!spec.workload) return offered;
  for (const auto& [op, share] : spec.workload->op_shares) {
    offered.emplace_back(op, Rate{spec.workload->base.per_second * share});
  }
  std::ranges::sort(offered, {}, &std::pair<std::string, Rate>::first);
  return offered;
}

void add_once(std::vector<std::size_t>& links, std::size_t link) {
  // C++23 std::ranges::contains: "is this value in the range?" in one call, replacing
  // `std::find(begin, end, value) != end`.
  if (!std::ranges::contains(links, link)) links.push_back(link);
}

// What one copy's queue holds during a step, per op, in requests: what was waiting plus
// this step's arrivals (rate × dt).
[[nodiscard]] flow::Flow queue_content(const flow::Flow& waiting, const flow::Flow& arrivals,
                                       SimTime dt) {
  flow::Flow content = waiting;
  content += arrivals.scaled(seconds(dt));
  return content;
}

// The long-run queue of estimate(): FluidQueue::settle sets the total backlog, and in
// the long run the queue holds the arrival mix, so each op waits in proportion to its
// arrival rate: arrivals × backlog / demand (rate × seconds = requests).
[[nodiscard]] flow::Flow settled_waiting(const flow::Flow& arrivals, double backlog) {
  const double demand_per_s = arrivals.total().per_second;
  return demand_per_s > 0.0 ? arrivals.scaled(backlog / demand_per_s) : flow::Flow{};
}

// A replica group of `replicas` identical copies behind an equal split (plan refinement
// 4): each copy saw demand / replicas, so the group's rates and backlog are the copy's
// times `replicas`; utilization, latency and zone are the same for every copy.
[[nodiscard]] NodeSample group_sample(const fluid::FluidStep& step, double replicas) {
  // C++20 designated initializers: name each field at the construction site, so the
  // five doubles in a row cannot be swapped silently.
  return NodeSample{.demand_per_s = step.demand_per_s * replicas,
                    .drain_per_s = step.drain_per_s * replicas,
                    .dropped_per_s = step.dropped_per_s * replicas,
                    .backlog = step.backlog * replicas,
                    .rho = step.rho,
                    .latency = step.latency,
                    .zone = step.zone};
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
    if (supported_in_phase_1(node.spec.type)) continue;
    return std::unexpected{graph::EngineError{
        "node '" + node.spec.id + "' has type '" + std::string{model::to_string(node.spec.type)} +
        "', which run/estimate do not support yet (phase 2)"}};
  }
  return Simulation{g};
}

// Copies what a step needs out of the graph, so the Simulation does not outlive a
// reference to it. Settle passes: see estimate().
Simulation::Simulation(const graph::Graph& g)
    : order_{g.topological_order().begin(), g.topological_order().end()},
      step_{g.scenario().step},
      settle_passes_{1 + static_cast<int>(std::ranges::count_if(
                             g.links(), [](const graph::ResolvedLink& l) { return l.feedback; }))} {
  for (std::size_t i = 0; i < g.nodes().size(); ++i) nodes_.push_back(make_node(g, i));
  for (const auto& link : g.links()) links_.push_back(Link{.line = DelayLine{line_steps(link)}});
  connect(g);
}

// Pattern: Strategy — each node gets the behaviour of its type, held by value in the
// behaviours::Behaviour variant and dispatched with std::visit in the forward pass.
Simulation::Node Simulation::make_node(const graph::Graph& g, std::size_t index) {
  const auto& resolved = g.nodes()[index];
  if (resolved.spec.type == model::NodeType::client) {
    return Node{.behaviour = behaviours::ClientBehaviour{g, index},
                .offered = offered_rates(resolved.spec)};
  }
  return Node{.behaviour = behaviours::ServiceBehaviour{g, index},
              .queue = fluid::FluidQueue{resolved.capacity},
              .replicas = static_cast<double>(resolved.spec.replicas)};
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

// ---- Simulation: one evaluation ----

StepRecord Simulation::step(SimTime now, SimTime dt) {
  return evaluate(now, dt, Mode::advance);
}

void Simulation::require_valid(const RunOptions& options) {
  if (options.step.count() <= 0) throw std::invalid_argument{"Simulation::run: step must be > 0"};
  if (options.duration.count() < 0) {
    throw std::invalid_argument{"Simulation::run: duration must be >= 0"};
  }
}

// The long run of an acyclic graph is reached in one settle pass: every node settles to
// its demand, and the links pass flow straight through in topological order. A feedback
// link is read before its source has run, so each pass carries the loop's flow one lap
// further: 1 + (number of feedback links) passes resolve loops that do not re-amplify
// themselves (chat: delivered messages do not re-enter the queue). A loop that feeds
// itself indefinitely (a crawler's frontier) has no finite steady state to jump to;
// `run` is the tool for it.
StepRecord Simulation::estimate() const {
  Simulation scratch = *this;  // a copy: estimate must not move a running simulation
  StepRecord record;
  for (int pass = 0; pass < settle_passes_; ++pass) {
    record = scratch.evaluate(SimTime{0}, step_, Mode::settle);
  }
  return record;
}

StepRecord Simulation::evaluate(SimTime now, SimTime dt, Mode mode) {
  StepRecord record{.t = now, .nodes = std::vector<NodeSample>(nodes_.size())};
  forward_pass(now, dt, mode, record.nodes);
  backward_pass(record.nodes);
  record.responses = client_responses();
  return record;
}

// Forward pass (spec §3.2-3.4), nodes in topological order. For each node:
//   1. receive: pop every incoming link into the node's inflow;
//   2. a client offers its load; a service runs its fluid queue on the inflow and routes
//      the part it drained (backlog included, with the queue's op mix: see serve);
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
        Overloaded{[&](const behaviours::ClientBehaviour& client) { return client.offer(now); },
                   [&](const behaviours::ServiceBehaviour& service) {
                     return serve(index, service, dt, mode, samples[index]);
                   }},
        nodes_[index].behaviour);
    send(index, routed, mode);
  }
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

// One service step: equal split over replicas, the §3.3 fluid step per copy (settled
// first when estimating), then route what the queue drained downstream.
//
// The FluidQueue knows only totals (one backlog number); `node.waiting` adds which ops
// are queued. Model: the queue is well-mixed FIFO fluid. Every request in it, whether it
// waited from an earlier step or arrived in this one, is equally likely to be among the
// drained, the dropped or the still waiting, so each part has the content's op mix:
//   content   = waiting + arrivals × dt                     requests per op
//   forwarded = content × drained / content_total           drained = drain × dt
//   dropped   = content × dropped / content_total           discarded: never propagates
//   waiting'  = content × backlog / content_total           = content − forwarded − dropped
// The totals come from FluidQueue::advance, which stays the single source of truth;
// waiting' is computed from the backlog directly rather than by subtraction, so it
// cannot go negative by cancellation, and its total matches the backlog to rounding.
// This is what makes "what a node drains is what the next node receives" hold even with
// no arrivals at all: a node working off its backlog forwards the queued ops, not this
// step's (empty) mix. (Phase 2's rate curves will exercise that zero-arrival case.)
// Ignored: priorities, per-op service times and any reordering inside the queue.
behaviours::Routed Simulation::serve(std::size_t index, const behaviours::ServiceBehaviour& service,
                                     SimTime dt, Mode mode, NodeSample& sample) {
  auto& node = nodes_[index];
  auto& queue = *node.queue;  // every service has a queue (make_node)
  // Replicas (plan refinement 4): `waiting` and the queue describe one copy, which sees
  // 1/replicas of the arrivals; take_drained scales the forwarded flow back to the group.
  const flow::Flow arrivals = node.inflow.scaled(1.0 / node.replicas);
  const Rate per_replica = arrivals.total();
  if (mode == Mode::settle) {
    queue.settle(per_replica);
    node.waiting = settled_waiting(arrivals, queue.backlog());
  }
  const flow::Flow content = queue_content(node.waiting, arrivals, dt);
  const fluid::FluidStep copy_step = queue.advance(per_replica, dt);
  sample = group_sample(copy_step, node.replicas);
  return service.route(take_drained(node, content, copy_step));
}

// Splits one copy's content (see serve) by the totals of its fluid step: keeps the
// still-waiting part in `node.waiting` and returns the drained part as the group's
// forwarded rate per op, content × (drain_per_s × replicas) / content_total: the
// drained requests per copy (drain × dt) over dt, times the copies. An empty queue with
// no arrivals has nothing to split.
flow::Flow Simulation::take_drained(Node& node, const flow::Flow& content,
                                    const fluid::FluidStep& copy_step) {
  const double content_total = content.total().per_second;  // requests, see Node::waiting
  if (content_total <= 0.0) {
    node.waiting.clear();
    return flow::Flow{};
  }
  node.waiting = content.scaled(copy_step.backlog / content_total);
  return content.scaled(copy_step.drain_per_s * node.replicas / content_total);
}

void Simulation::send(std::size_t index, const behaviours::Routed& routed, Mode mode) {
  const auto& outgoing_links = nodes_[index].outgoing_links;
  for (const std::size_t link : outgoing_links) links_[link].sent.clear();
  for (const auto& out : routed.outgoing) links_[out.link].sent += out.flow;
  if (mode == Mode::settle) return;  // estimate reads `sent` directly; lines stay untouched
  for (const std::size_t link : outgoing_links) links_[link].line.push(links_[link].sent);
}

// Backward pass (plan refinement 1), nodes in reverse topological order: a service's
// response to op = own sojourn this step + its hops (ServiceBehaviour::response). Reverse
// order means every forward target was already computed this step; a feedback target
// was not, so it contributes its previous step's entry — the same "previous step's
// value" rule as the forward pass. A target that has not yet received an op (its flow is
// still in a delay line) contributes zero latency for it.
// The table keeps every op the node has ever received and refreshes all of them, so an
// op that pauses keeps an estimate at the node's current load.
void Simulation::backward_pass(const std::vector<NodeSample>& samples) {
  const auto lookup = [this](std::size_t target, std::string_view op) -> flow::LatencyEstimate {
    return response_of(target, op);
  };
  // C++20 std::views::reverse: walks the order back to front without copying it,
  // replacing a reverse-iterator loop (rbegin/rend) or a reversed copy.
  for (const std::size_t index : order_ | std::views::reverse) {
    auto& node = nodes_[index];
    // Only services compose a response here; clients are read in client_responses().
    const auto* service = std::get_if<behaviours::ServiceBehaviour>(&node.behaviour);
    if (service == nullptr) continue;
    for (const auto& entry : node.inflow.entries()) node.responses.try_emplace(entry.op);
    for (auto& [op, response] : node.responses) {
      response = service->response(op, samples[index].latency, lookup);
    }
  }
}

// Every client op in node order then op order, after the backward pass filled every
// service's table: a client's response is the share-weighted mixture over its routes.
std::vector<ResponseSample> Simulation::client_responses() const {
  const auto lookup = [this](std::size_t target, std::string_view op) -> flow::LatencyEstimate {
    return response_of(target, op);
  };
  std::vector<ResponseSample> responses;
  for (std::size_t index = 0; index < nodes_.size(); ++index) {
    const auto* client = std::get_if<behaviours::ClientBehaviour>(&nodes_[index].behaviour);
    if (client == nullptr) continue;
    for (const auto& [op, offered] : nodes_[index].offered) {
      responses.push_back(ResponseSample{
          .client = index, .op = op, .offered = offered, .response = client->response(op, lookup)});
    }
  }
  return responses;
}

// Zero for an op the node has never received (and for a client, which has no table).
flow::LatencyEstimate Simulation::response_of(std::size_t node, std::string_view op) const {
  const auto& table = nodes_[node].responses;
  const auto found = table.find(op);
  return found == table.end() ? flow::LatencyEstimate{} : found->second;
}

}  // namespace simsys::engine
