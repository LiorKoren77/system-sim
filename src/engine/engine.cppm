// Module `simsys.engine`: the step loop (spec §3). One step = a forward pass in
// topological order moving demand through delay lines and fluid queues, then a backward
// pass composing response latency per (node, op). `estimate` repeats the same evaluation
// in settle mode until it stops changing: the long-run state. The only randomness is the
// clients' workload noise, and each client draws it from its own engine seeded from the
// scenario's `seed` (workload::client_seed), so runs with the same seed are
// byte-identical by construction.
//
// Models: a whole System Design Interview (Alex Xu, vol. 1) diagram set in motion. The
// users box offers load (ch. 2 back-of-the-envelope QPS), every arrow carries it to the
// next box after the hop's latency (an arrow with a bandwidth through a pipe that queues and
// drops megabytes first, Simulation::pass_through_pipe), every box serves what its capacity
// allows and queues or sheds the rest (ch. 1 scaling), and the response time a user sees is
// built up on the way back: each box's own wait and service time plus the hops it made (a
// pipe's wait and serialisation with them, Simulation::hop_response). Captured:
// saturation spreading downstream as reduced throughput (dropped flow never reaches the
// next box), propagation delay, fan-out and the feedback loops of ch. 12 (chat). Ignored:
// individual requests (flow is a fluid), retries, and anything that changes within a step
// (demand is constant across one step). A scripted fault (the scenario's `faults:`, FaultSchedule)
// changes a node's numbers for the steps it covers: forward_pass asks the schedule for the node's
// Condition and the node steps under it (serve, ClientBehaviour::set_rate_factor). A client's
// timeout is applied to the fitted response time it sees (Simulation::give_up_after), not request
// by request.
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

// C++20 `export import` of a partition: brings the interface partition simsys.engine:faults (the
// fault schedule, src/engine/faults.cppm) into this unit and re-exports it, so every importer of
// simsys.engine sees engine::Condition and engine::FaultSchedule with no import of its own. The
// partition's name is relative (`:faults`): only units of simsys.engine can name it.
export import :faults;

import simsys.core;
import simsys.behaviours;
import simsys.defaults;
import simsys.flow;
import simsys.fluid;
import simsys.graph;
import simsys.model;
import simsys.queueing; // lognormal_below: what a client's timeout leaves answered
import simsys.workload; // client_seed: each client's noise seed

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

// One node's numbers for one step. Rates and backlog are summed over replicas (spec §16 entry 4);
// rho and zone are per copy, which all copies share — or, behind a consistent-hash balancer, those
// of the hottest copy, the one that breaks an SLO first, and for a SQL database those of the busier
// of its primary and its replicas — and latency is the sojourn a request sees: one copy's, or
// behind a hash ring the share-weighted mixture of the hot and the cool copies' (see
// Simulation::make_server), or for a database the mixture of its writes' (primary) and its reads'
// (replica) sojourns, weighted by their request rates (Simulation::make_database), and for a
// byte-sized node the mixture of its ops' sojourns, each answered at its own time, by their request
// rates (Simulation::request_weighted_megabyte_sojourn). Demand, drain, drops and backlog are in
// CAPACITY UNITS (per second for the rates), the unit `capacity` is a rate of (spec §17 entry 4): a
// request of an op with `cost` 3 counts three times. With every cost at its default of 1, units are
// requests, as in phase 1. The drop ratio dropped / demand is therefore the share of the node's
// work it shed. A kv_cluster's every request costs N units (spec §19 entry 9, defaults::op_cost):
// its demand, drain, drops and backlog are REPLICA-REQUESTS, N times the requests its clients sent
// (30k requests/s at N = 3 are 90k/s), the work its machines do; the clients' offered and answered
// rates (ResponseSample) stay in requests. `rejected_per_s` is a second way to shed (spec §17 entry
// 6): requests a rate limiter refused by POLICY, never forwarded. For the token bucket and the
// windowed algorithms they are refused after the limiter's queue drained them, so they are part of
// `drain_per_s` too; for a leaky bucket they are its queue's overflow ("bucket full"), which is
// then reported here and not in `dropped_per_s`. Drops are failures, rejections are the policy
// working: report::Summary keeps a drop rate and a reject rate apart. A limiter's every op costs 1,
// so these units are requests. Conservation, per node and per step (the fluid queue's balance,
// summed over copies):
//   demand·dt = (drain + dropped)·dt + Δbacklog
// whatever comes in is served, shed or left waiting. Rejections do not appear as a term of
// their own: a token-family limiter's (token bucket, windowed) are inside `drain` (its
// queue looked at them), and a leaky bucket's are the `dropped` term, moved to `rejected`. An
// external sink's failed requests are moved from `drain` to `dropped` (fail_share): the sum, and
// with it the balance, does not change.
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
  // How old the data a read may return is, in seconds (spec §18 entry 6): for a SQL database, the
  // write's sojourn at the primary (wait + service, until it is committed) + the replication lag
  // (shipping it) + its sojourn at a replica (waiting behind the reads and the other replayed
  // writes, then applied): a replica shows a write that long after the client sent it. 0 for a
  // database without replicas (every read is served by the primary, which has the latest write);
  // empty for every other node, which keeps no copies of data that could lag. Infinite when the
  // primary or the replica never answers. Finite while a replica drops replayed writes (below): the
  // model's queue is bounded, so this is the bounded-queue figure, while a real replica's lag grows
  // without bound (see replication_dropped_per_s).
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
  // A message queue's lag, in seconds (spec §19 entries 4 and 14; the queue between producers and
  // workers of ch. 10 and ch. 14): how long a message published at this step waits until its
  // slowest consumer group reads it. Kafka reports a group's lag as the records it has not read;
  // divided by the rate the group consumes them, that is this time. The broker holds nothing back
  // (it appends at 1M/s); the unread messages wait in each group's worker pool, so the lag is read
  // off the pools (Simulation::record_queue_lags):
  //   lag(group) = the pool's queueing wait this step (fluid::FluidQueue::wait)
  //              = max(min(k × Wq, T), backlog / capacity)
  //     backlog / capacity  the FIFO drain time: a message published now is read after everything
  //                         ahead of it, which the pool works off at its capacity (workers / cost)
  //                         while it is behind: 60,000 messages at 1200/s are 50 s;
  //     k × Wq              the stochastic part: Allen–Cunneen's factor k times the Erlang C wait
  //                         of an M/M/c queue (c = workers, service time = cost, rho clamped at
  //                         0.99): what a message waits with no backlog because every worker is
  //                         busy, 3.289 ms for 1k/s into 4 workers at 3.333 ms;
  //     T                   the room's drain time, = the queue's retention: a message older than
  //                         that is deleted unconsumed, so no lag passes it.
  //   The larger of the two terms is our heuristic (FluidQueue::wait), not a published result.
  //   lag(queue) = the largest over its groups: a queue is as far behind as its slowest reader.
  // It is not the age of the oldest unread message, which a dashboard may show instead: under
  // FIFO the message published now becomes the oldest one lag later, so the age trails this lag
  // by one lag and reaches the same peak that much later. The `queue_lag` SLO bounds the peak
  // after warmup, so it reads "every message published after warmup was read within `max`".
  // The wait only: the job's `cost` is not lag (a message is read once a worker takes it), so the
  // lag is the pool's latency minus its cost. Assumes the backlog is read first in, first out (a
  // Kafka partition is). Empty for every node that is not a message queue (a 0 would be a claim).
  std::optional<double> queue_lag_s{};
};

// What a client sees for one op: the rate it offers, the part of it that comes back answered, and
// the response time an answered request gets. `answered` = offered × the delivered fraction the
// backward pass composes along the op's path (spec §17 entry 8,
// behaviours::NodeBehaviour::delivered): a request a limiter refused or a full queue dropped is not
// answered (one still waiting in a queue is: later, see Simulation::account_rejections), and
// reports weigh latency by `answered` so that those requests, which never saw the path's latency,
// do not dilute it. A client with a `timeout` (Simulation::give_up_after) also gives up on the
// delivered requests slower than it: those are `timed_out`, not answered, and `response` is the
// latency of the ones answered in time. So answered = offered − lost − timed_out, and timed_out is
// 0 for a client that waits for ever. `delivered_response` is the latency of every delivered
// request, answered or timed out, before the client gave up (the untruncated fit the timeout cut):
// the reports read a timed-out client's percentiles off it. Without a timeout the two are equal.
struct ResponseSample {
  std::size_t client{};
  std::string op{};
  Rate offered{};
  Rate answered{};
  Rate timed_out{};
  flow::LatencyEstimate response{};
  flow::LatencyEstimate delivered_response{};
};

// One bandwidth link's pipe for one step (spec §19 entry 3; Simulation::pass_through_pipe):
// NodeSample's fields of the same names, in MEGABYTES (per second for the rates), the pipe being a
// queue of megabytes like a byte-sized node. `demand_per_s` is what the sender put on the link,
// `drain_per_s` what the pipe passed on to the receiver, `dropped_per_s` what overflowed its 1000
// MB room; they balance as a node's do,
//   demand·dt = (drain + dropped)·dt + Δbacklog.
// `rho` and `zone` are the one server's; `latency` is what a hop over the pipe adds, its wait plus
// the request's answer time on the wire (a write's whole serialisation, size × 1 MB / bandwidth;
// any other op's first megabyte), mixed over the ops that arrived by their requests, over the ops
// it carries alike in a step when none arrived (the link's fixed one-way delay is not in it). A
// pipe refuses nothing by policy, so it has no `rejected_per_s`.
struct LinkSample {
  double demand_per_s{};
  double drain_per_s{};
  double dropped_per_s{};
  double backlog{};
  double rho{};
  flow::LatencyEstimate latency{};
  fluid::Zone zone{fluid::Zone::green};
};

// Everything one step produced: one NodeSample per node (indexed like Graph::nodes()), one
// ResponseSample per (client, op), clients in node order, ops sorted by name, and one LinkSample
// per link that has a bandwidth, in Graph::links() order (none without a bandwidth: such a link
// only delays its flow).
struct StepRecord {
  SimTime t{};
  std::vector<NodeSample> nodes{};
  std::vector<ResponseSample> responses{};
  std::vector<LinkSample> links{};
  // Only Simulation::estimate clears it: false means the settle passes hit their cap
  // before the numbers stopped changing, so the record is the last pass, not a steady
  // state. A loop that returns as much as it receives (gain >= 1) and that no capacity
  // stops within reach climbs for ever: it has no steady state. So do delivered fractions
  // that never settle (a step after a failed step, see estimate). Every step of `run` leaves it
  // true.
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
  // (estimating at another time, `--at`, is in the Phase 3 backlog). Record time is 0; `converged`
  // says whether a steady state was found.
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

  // Settle mode only: a delivered fraction as estimate()'s passes thin later steps by, the damped
  // running average d̄ of what the passes measured (see estimate). Each pass moves it a share
  // `weight` of the way to the new measurement, d̄ ← (1 − w) d̄ + w d, and halves w whenever the
  // gap d − d̄ changes sign (the average overshot the fixed point), down to min_settling_weight.
  // 1 with w = 1/2 before any pass: nothing lost yet, and at first the old halfway average.
  struct DampedFraction {
    double value{1.0};
    double weight{0.5};
    double last_gap{0.0};  // d − d̄ at the previous pass: 0 before the first
    void move_towards(double measured);
  };

  // What the backward pass knows about one (node, op): the response time of an answered request and
  // the fraction of the requests that are answered (spec §17 entry 8).
  struct Answer {
    flow::LatencyEstimate latency{};
    double delivered{1.0};  // 1 until the first backward pass: nothing lost yet
    // Settle mode only: `delivered` damped over estimate()'s passes, which thin later steps by it.
    DampedFraction settling_delivered{};
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

  // What one copy group's step produced (advance_group): what one copy's queue held during the
  // step (its waiting content plus the step's arrivals, in units), one copy's fluid step, and what
  // all the group's copies drained and dropped, per op, in capacity units per second: the group's
  // totals, the clients' work and a replica's replayed writes alike.
  struct GroupStep {
    flow::Flow content{};
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
    // (receiver, link) of every bandwidth link this node sends demand on (connect_pipes): where
    // its hop lookups find a pipe (hop_response, hop_delivered). Empty for most nodes.
    std::vector<std::pair<std::size_t, std::size_t>> pipes_out{};
    // A message queue's consumer groups (connect_consumer_groups): the worker pools whose waits
    // are its lag (record_queue_lags). Empty for every other node; a queue has at least one
    // (Graph::build refuses a queue nobody consumes).
    std::vector<std::size_t> consumer_groups{};
  };

  // A bandwidth link's pipe (spec §19 entry 3; defaults::pipe_capacity has its numbers and what it
  // models): one queue of one server in MEGABYTES, fed what the sender puts on the link, each
  // request priced at its size (megabytes_by_op) and timed by `timing` (a request holds the pipe
  // for its whole serialisation, size × 1 MB / bandwidth; the receiver acts on a write at its last
  // byte and on any other op at its first megabyte). Its step's wait and the share it passed (not
  // dropped) are what the backward pass adds to every hop over the link (hop_response,
  // hop_delivered).
  struct Pipe {
    CopyGroup group;  // one copy (CopyGroup's defaults): the queue and what it holds, in MB
    behaviours::MegabyteService timing;
    std::map<std::string, double, std::less<>> megabytes_by_op{};  // every op it may carry
    std::size_t slot{};                // its sample's index in StepRecord::links
    flow::LatencyEstimate wait{};      // this step's queueing wait
    double passed{1.0};                // the share of this step's arriving megabytes not dropped
    DampedFraction settling_passed{};  // settle mode only: `passed` damped as Answer's d̄ is
  };

  struct Link {
    DelayLine line;
    // What leaves the sender's end of the link in the latest evaluation: what the sender put on
    // it, or, over a pipe, the part the pipe passed on (pass_through_pipe). The delay line
    // carries it to the receiver.
    flow::Flow sent{};
    std::optional<Pipe> pipe{};  // a link with a bandwidth only
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
  [[nodiscard]] static std::optional<Pipe> make_pipe(const graph::Graph& g, std::size_t link,
                                                     std::size_t slot);
  void connect(const graph::Graph& g);
  void connect_pipes(const graph::Graph& g);
  void connect_consumer_groups(const graph::Graph& g);
  void register_reachable_ops(const graph::Graph& g);
  [[nodiscard]] Simulation settling_copy() const;

  [[nodiscard]] StepRecord evaluate(SimTime now, SimTime dt, Mode mode);
  void forward_pass(SimTime now, SimTime dt, Mode mode, StepRecord& record);
  // C++20 std::span<T>: a non-owning view of contiguous elements (a whole array or vector, or
  // the front of one), replacing a (pointer, count) pair or a `std::vector&` that would force the
  // caller to own a vector. Here the step's node samples, which this fills in place.
  void record_queue_lags(std::span<NodeSample> samples) const;
  void receive(std::size_t index, Mode mode);
  // Any queueing node (service, load balancer, rate limiter): runs its queues and returns
  // what they drained, in requests per second, for the behaviour to route.
  [[nodiscard]] flow::Flow serve(std::size_t index, SimTime dt, Mode mode,
                                 const Condition& condition, NodeSample& sample);
  [[nodiscard]] static GroupStep advance_group(CopyGroup& group, const flow::Flow& arrivals,
                                               SimTime dt, Mode mode,
                                               const fluid::Derating& derating);
  [[nodiscard]] static GroupStep serve_copies(CopyGroup& group, const flow::Flow& arrivals,
                                              SimTime dt, Mode mode,
                                              const fluid::Derating& derating,
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
  // The sojourns live in a fixed-size array of which only the groups in use are passed as a span
  // (see the caller).
  static void record_sojourns(Node& node, std::span<const flow::LatencyEstimate> sojourns,
                              std::span<const flow::LatencyEstimate> waits, NodeSample& sample);
  [[nodiscard]] static flow::LatencyEstimate request_weighted_sojourn(const Node& node);
  static void time_whole_objects(Node& node);
  [[nodiscard]] static flow::LatencyEstimate request_weighted_megabyte_sojourn(
      const flow::Flow& requests, const flow::LatencyEstimate& wait,
      const behaviours::MegabyteService& megabyte_service);
  static void record_replication(const Node& node, std::span<const flow::LatencyEstimate> sojourns,
                                 const ReplicationTally& tally, NodeSample& sample);
  [[nodiscard]] static flow::Flow take_drained(CopyGroup& group, const flow::Flow& content,
                                               const fluid::FluidStep& copy_step);
  void send(std::size_t index, const behaviours::Routed& routed, SimTime dt, Mode mode,
            std::span<LinkSample> link_samples);
  static void pass_through_pipe(Link& link, SimTime dt, Mode mode,
                                std::span<LinkSample> link_samples);
  void account_rejections(std::size_t index, const behaviours::Routed& routed, NodeSample& sample);
  void backward_pass();
  [[nodiscard]] std::vector<ResponseSample> client_responses(SimTime now) const;
  // Applies a client's `timeout` to one of its samples (see the definition).
  static void give_up_after(double timeout_s, ResponseSample& sample);
  [[nodiscard]] flow::LatencyEstimate response_of(std::size_t node, std::string_view op) const;
  [[nodiscard]] static flow::LatencyEstimate own_sojourn(const Node& node, std::string_view op);
  [[nodiscard]] static double own_passed(const Node& node, std::string_view op);
  [[nodiscard]] double delivered_of(std::size_t node, std::string_view op) const;
  [[nodiscard]] const Pipe* pipe_between(std::size_t caller, std::size_t target) const;
  [[nodiscard]] flow::LatencyEstimate hop_response(std::size_t caller, std::size_t target,
                                                   std::string_view op) const;
  [[nodiscard]] double hop_delivered(std::size_t caller, std::size_t target,
                                     std::string_view op) const;
  [[nodiscard]] double hop_delivered_for_thinning(std::size_t caller, std::size_t target,
                                                  std::string_view op, Mode mode) const;
  void average_settling_fractions();

  std::vector<Node> nodes_;
  std::vector<Link> links_;
  std::size_t pipe_count_{};  // the links with a bandwidth: StepRecord::links has one each
  std::vector<std::size_t> order_;
  SimTime step_{};
  // The scenario's faults (empty without any). estimate()'s scratch copy (settling_copy) leaves it
  // empty: the steady state is the one without faults.
  FaultSchedule faults_{};
};

}  // namespace simsys::engine

namespace simsys::engine {

namespace {

// Why a cache cannot run, if it cannot: a scenario built in code left it without a number or `che`
// for its hit ratio (the loader requires one: the same message). Everything else a cache can say
// runs: a `che` hit ratio (Graph::build has already turned it into a number,
// Graph::resolve_hit_ratios), every strategy including write_back (its flush is an asynchronous
// hop, graph::Graph::is_async_hop) and `then` steps (spec §18 entry 5, behaviours::CacheBehaviour).
[[nodiscard]] std::optional<std::string> cache_not_runnable(const model::NodeSpec& spec) {
  const bool has_number = spec.hit_ratio && spec.hit_ratio->fixed;
  const bool is_che = spec.hit_ratio && spec.hit_ratio->che;
  if (!has_number && !is_che) return "node '" + spec.id + "': cache needs a hit_ratio";
  return std::nullopt;
}

// Why a CDN cannot run, if it cannot (spec §18 entry 10): a `che` hit ratio without a `keys` size.
// Graph::build leaves it without a number, and running it as h = 0 would silently replace the
// scenario's Che approximation by "every read misses" (explain shows that bound and says so; a run
// could not). A CDN with no hit ratio at all runs: it misses every read, as the scenario says. A
// CDN sized in bytes runs too, counting megabytes (spec §19 entry 2, behaviours::CdnBehaviour).
[[nodiscard]] std::optional<std::string> cdn_not_runnable(const model::NodeSpec& spec) {
  if (spec.hit_ratio && spec.hit_ratio->che && !spec.keys) {
    return "node '" + spec.id + "': hit_ratio che needs keys (the cdn's size in objects)";
  }
  return std::nullopt;
}

// Why a node of a simulated type still cannot run, if it cannot (the cases above). A blob store
// always runs, `then` steps included: they are event notifications (spec §19 entries 6-7),
// asynchronous hops the backward pass does not wait for (behaviours::BlobStoreBehaviour). So do a
// message queue (behaviours::MessageQueueBehaviour) and a worker pool, `then` steps included:
// the work each drained job hands on (behaviours::WorkerPoolBehaviour), and an
// external sink, whatever its failure_rate (behaviours::ExternalSinkBehaviour, fail_share).
[[nodiscard]] std::optional<std::string> not_runnable_yet(const model::NodeSpec& spec) {
  if (spec.type == model::NodeType::cache) return cache_not_runnable(spec);
  if (spec.type == model::NodeType::cdn) return cdn_not_runnable(spec);
  return std::nullopt;
}

// Why a scripted fault cannot run yet, if one cannot: a dc_down, whose datacenter's nodes and links
// Task 7 of phase 3 takes down (a temporary refusal, replaced there). Every other kind runs
// (FaultSchedule; a cache_flush changes nothing until Task 7 either, but refusing it would refuse
// the scenarios that script one).
[[nodiscard]] std::optional<std::string> fault_not_runnable_yet(const model::Scenario& scenario) {
  for (const auto& fault : scenario.faults) {
    if (fault.kind == model::FaultKind::dc_down) return "dc_down lands in Task 7";
  }
  return std::nullopt;
}

// The fluid queue's view of a node's condition (fluid::Derating): its capacity factor and its
// extra latency in seconds. The identity condition gives the identity derating (seconds(0) is 0).
[[nodiscard]] fluid::Derating derating_of(const Condition& condition) {
  return fluid::Derating{.capacity_factor = condition.capacity_factor,
                         .extra_service_s = seconds(condition.extra_latency)};
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
  // core::Overloaded: the overloaded-visitor idiom; C++20 class template argument deduction for
  // aggregates deduces its lambdas' types from the braces (see simsys.core).
  return std::visit(core::Overloaded{[](const behaviours::ClientBehaviour&) { return 1.0; },
                                     [&](const auto& server) { return server.cost(op); }},
                    behaviour);
}

// Requests per second -> capacity units per second: each op's rate × its cost (spec §17 entry 4).
// Demand in units is Σ rate_op × cost_op, the load the node's capacity (a rate of units) is
// compared with: 300 req/s of an op of cost 3 is 900 units/s, so a 1000 units/s node runs at rho =
// 0.9.
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

// The share of what a node serves that fails (spec §19 entry 8): an external sink's `failure_rate`
// (behaviours::ExternalSinkBehaviour); every other node fails nothing it served.
[[nodiscard]] double failure_rate_of(const behaviours::Behaviour& behaviour) {
  const auto* sink = std::get_if<behaviours::ExternalSinkBehaviour>(&behaviour);
  return sink != nullptr ? sink->failure_rate() : 0.0;
}

// A kv_cluster's quorum needs its machines one kind at a time: what one replica-request saw on the
// hot machine and on a cool one this step, and the share each kind kept, in make_server's group
// order (behaviours::KvClusterBehaviour::observe). Every other node mixes its groups into one
// sojourn (record_sojourns) and is told nothing.
void report_machines(behaviours::Behaviour& behaviour,
                     std::span<const behaviours::MachineGroup> machines) {
  if (auto* cluster = std::get_if<behaviours::KvClusterBehaviour>(&behaviour)) {
    cluster->observe(machines);
  }
}

// An external sink's failures (spec §19 entry 8; book ch. 10: the third-party providers are the
// unreliable part of the notification pipeline). The provider works on a request that fails as long
// as on one that succeeds, so the queue serves both alike and its rho counts every request it
// served; what changes is where a failed request goes. With f the failure rate and `served` what
// the node's queues drained this step (per op, and in total in the sample):
//   failed   = f × served         a 5xx: counted as a DROP (the node failing), never a rejection
//   dropped' = dropped + failed   the queue's overflow plus the failures
//   drain'   = (1 − f) × served   what is answered, and what the behaviour is handed
// Conservation is untouched: drain' + dropped' = drain + dropped, so demand·dt = (drain +
// dropped)·dt + Δbacklog still holds, and the node's own delivered share 1 − (dropped + rejected)
// / inflow (account_rejections) loses the failures once: 500/s at f = 0.02 is answered 490/s.
// Assumes every op fails alike and that a failure is final (no retries before phase 3).
void fail_share(double failure_rate, flow::Flow& drained, flow::Flow& dropped, NodeSample& sample) {
  if (failure_rate <= 0.0) return;  // fails nothing: the plain queue, bit for bit
  dropped += drained.scaled(failure_rate);
  drained = drained.scaled(1.0 - failure_rate);
  const double failed_per_s = failure_rate * sample.drain_per_s;
  sample.dropped_per_s += failed_per_s;
  sample.drain_per_s -= failed_per_s;
}

// How a node times its requests in megabytes (behaviours::MegabyteService) when it counts them:
// a blob store or a CDN sized in bytes (defaults::is_byte_sized). Null for every other node.
[[nodiscard]] const behaviours::MegabyteService* megabyte_service_of(
    const behaviours::Behaviour& behaviour) {
  return std::visit(
      [](const auto& node) -> const behaviours::MegabyteService* {
        // C++20 requires-expression as a compile-time condition:
        // `requires { node.megabyte_service(); }` is true exactly for the alternatives that have
        // such a member. Replaces the C++17 detection idiom (a std::void_t trait written for the
        // purpose) with one line, so only the behaviours that may count megabytes need to know
        // about them.
        if constexpr (requires { node.megabyte_service(); }) {
          return node.megabyte_service();
        } else {
          return nullptr;
        }
      },
      behaviour);
}

// A latency spike at a byte-sized node (a blob store, a CDN counting megabytes): its queue's wait
// already reflects the slower service (fluid::Derating), but each op is answered at its own time
// after that wait (own_sojourn, behaviours::MegabyteService::sojourn), which knows nothing of the
// extra. The extra holds every request before its bytes move, so it is added to the wait every op
// pays. Every other node needs nothing: its sojourn is its queue's, whose service time is already
// s + extra; and its wait must stay the wait (a worker pool's is its queue's lag,
// record_queue_lags).
[[nodiscard]] flow::LatencyEstimate with_extra_before_bytes(flow::LatencyEstimate wait,
                                                            const behaviours::Behaviour& behaviour,
                                                            const fluid::Derating& derating) {
  if (derating.extra_service_s > 0.0 && megabyte_service_of(behaviour) != nullptr) {
    wait.mean_s += derating.extra_service_s;
  }
  return wait;
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

// Adds `copies` identical copies, each of which took `copy_step`, to the group's totals (spec §16
// entry 4): N copies behind an equal split each saw demand / N, so the group's rates and backlog
// are the copy's times N. A pooled group is one copy (its queue IS the group); a hash ring adds its
// hot copy (1) and its cool ones (N - 1); a database its S primaries and its (N - 1) S replicas.
// Only the clients' part of the work counts (NodeSample): `arrived_share` of the copy's demand and
// `content_share` of what it drained, dropped and holds, both 1 except on a database replica
// (Simulation::serve_copies). `rejected_per_s` stays 0: a queue never rejects; the forward pass
// fills it (account_rejections).
void add_copies(NodeSample& sample, const fluid::FluidStep& copy_step, double copies,
                double arrived_share, double content_share) {
  sample.demand_per_s += copy_step.demand_per_s * copies * arrived_share;
  sample.drain_per_s += copy_step.drain_per_s * copies * content_share;
  sample.dropped_per_s += copy_step.dropped_per_s * copies * content_share;
  sample.backlog += copy_step.backlog * copies * content_share;
}

// The share of what arrived that was not lost, 1 − lost / arrived, kept in [0, 1] (see
// account_rejections); 1 when nothing arrived: nothing was lost.
[[nodiscard]] double kept_fraction(double lost_per_s, double arrived_per_s) {
  if (arrived_per_s <= 0.0) return 1.0;
  return std::clamp(1.0 - lost_per_s / arrived_per_s, 0.0, 1.0);
}

// kept_fraction of two flows' totals: the requests (or units) lost and those that arrived.
[[nodiscard]] double kept_share(const flow::Flow& lost, const flow::Flow& arrived) {
  const double arrived_per_s = arrived.total().per_second;
  return kept_fraction(lost.total().per_second, arrived_per_s);
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
// ...and the smallest weight a damped fraction's average moves by (Simulation::DampedFraction):
// small enough to settle a self-thinning slope down to f' = 1 − 2 × 1024 = −2047 (the damped map's
// slope 1 − w + w f' stays above −1), large enough that the slowest fraction, its slope near 0,
// still settles in about 1024 × ln(1e12) ≈ 28k passes, inside max_settle_passes.
inline constexpr double min_settling_weight = 1.0 / 1024.0;

// Written so that a NaN never counts as unchanged (every comparison with NaN is false)
// and an `inf` latency (a zero-capacity node, "unavailable") that stays inf does.
[[nodiscard]] bool unchanged(double before, double after) {
  return before == after || std::abs(after - before) <= settle_tolerance * std::abs(after);
}

// Records of two consecutive settle passes over the same simulation have the same nodes
// and the same (client, op) responses in the same order. A bandwidth link's pipe needs no test of
// its own: its demand is what its sender sent, which moves only when the sender's demand does (and
// that is tested), and its wait and losses reach the clients through the responses and answered
// rates tested here.
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

// The megabytes one request of `op` is on a pipe (Simulation::Pipe::megabytes_by_op). An op it
// was not built with cannot cross it: Graph::build refuses a bandwidth link that carries an
// unsized op (graph::Graph::resolve_request_sizes), so this is a programmer error.
[[nodiscard]] double megabytes_of(const std::map<std::string, double, std::less<>>& by_op,
                                  std::string_view op) {
  const auto found = by_op.find(op);
  if (found == by_op.end()) {
    throw std::logic_error{"a pipe carries op '" + std::string{op} + "', which has no size"};
  }
  return found->second;
}

// Requests per second -> megabytes per second on a pipe, op by op (rate × MB), as in_units prices
// a byte-sized node's requests.
[[nodiscard]] flow::Flow to_megabytes(const flow::Flow& requests,
                                      const std::map<std::string, double, std::less<>>& by_op) {
  flow::Flow megabytes;
  for (const auto& [op, rate] : requests.entries()) {
    megabytes.add(op, Rate{rate.per_second * megabytes_of(by_op, op)});
  }
  return megabytes;
}

// One request per second of every op a pipe carries: the mix an idle pipe reports its latency
// for (pass_through_pipe), each op weighted alike for want of arrivals to weigh them by.
[[nodiscard]] flow::Flow one_of_each(const std::map<std::string, double, std::less<>>& by_op) {
  flow::Flow mix;
  // C++20 std::views::keys: a lazy view of each map entry's key (the op name), replacing a loop
  // over the pairs that ignores `.second`; nothing is copied.
  for (const auto& op : by_op | std::views::keys) mix.add(op, Rate{1.0});
  return mix;
}

// The inverse (MB / MB per request): what a pipe passes on is requests again.
[[nodiscard]] flow::Flow to_requests(const flow::Flow& megabytes,
                                     const std::map<std::string, double, std::less<>>& by_op) {
  flow::Flow requests;
  for (const auto& [op, rate] : megabytes.entries()) {
    requests.add(op, Rate{rate.per_second / megabytes_of(by_op, op)});
  }
  return requests;
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

// Two passes over the nodes, in file order: first every node's TYPE, then what a node of a
// simulated type still lacks (not_runnable_yet: a cache without a hit ratio, a che CDN without a
// `keys` size). So a type the engine has no model for is named before a node that only lacks a
// number: google-drive is refused at its ws_gateway, long_poll, the first type it has no model of.
// The message names the phase that plans a model for the type: phase 3 for each of the four left
// (ws_gateway, service_discovery, notification_fanout, frontier), the types whose row in
// model::node_type_table does not say `runs` (model::simulated).
std::expected<Simulation, graph::EngineError> Simulation::create(const graph::Graph& g) {
  for (const auto& node : g.nodes()) {
    if (model::simulated(node.spec.type)) continue;
    // C++23 std::unexpected<E>: wraps an error so it converts to the error side of a
    // std::expected (a bare EngineError would be taken as a value, and fail to convert).
    // Replaces throwing or returning a sentinel; the caller must look at the result.
    return std::unexpected{graph::EngineError{
        "node '" + node.spec.id + "' has type '" + std::string{model::to_string(node.spec.type)} +
        "', which run/estimate do not support yet (phase 3)"}};
  }
  for (const auto& node : g.nodes()) {
    if (auto refused = not_runnable_yet(node.spec)) {
      return std::unexpected{graph::EngineError{std::move(*refused)}};
    }
  }
  if (auto refused = fault_not_runnable_yet(g.scenario())) {
    return std::unexpected{graph::EngineError{std::move(*refused)}};
  }
  return Simulation{g};
}

// Copies what a step needs out of the graph, so the Simulation does not outlive a
// reference to it.
Simulation::Simulation(const graph::Graph& g)
    : order_{g.topological_order().begin(), g.topological_order().end()},
      step_{g.scenario().step},
      faults_{g.scenario()} {
  for (std::size_t i = 0; i < g.nodes().size(); ++i) nodes_.push_back(make_node(g, i));
  // C++20 designated initializers: `Link{.line = ...}` names the member it sets and leaves
  // the others at their defaults, replacing positional braces where a reader must count
  // members against the struct (and same-typed doubles, as in CopyGroup, could swap silently).
  for (std::size_t i = 0; i < g.links().size(); ++i) {
    links_.push_back(
        Link{.line = DelayLine{line_steps(g.links()[i])}, .pipe = make_pipe(g, i, pipe_count_)});
    if (links_.back().pipe) ++pipe_count_;
  }
  connect(g);
  connect_pipes(g);
  connect_consumer_groups(g);
  register_reachable_ops(g);
}

// Pattern: Strategy — each node gets the behaviour of its type, held by value in the
// behaviours::Behaviour variant and dispatched with std::visit in the forward pass, where every
// behaviour routes and composes its own way. Not every question goes through the strategy: a few
// helpers ask the variant which kind it holds (std::get_if, std::holds_alternative: is_write_at,
// failure_rate_of, report_machines, split_writes) for what only one or two node types have, a
// database's writes, a sink's failures, a kv cluster's machines. That is a type test, the opposite
// of Strategy; it is kept where the alternative would be a member every other behaviour implements
// to do nothing. A client's noise seed is client_seed(scenario seed, node index): one random engine
// per client, so appending a node at the end of the file never changes another client's noise (the
// seed is the node index; inserting a node earlier shifts the clients after it; spec §17 entry 3),
// and `simsys curve --noise` derives the same seed.
Simulation::Node Simulation::make_node(const graph::Graph& g, std::size_t index) {
  const auto& resolved = g.nodes()[index];
  switch (resolved.spec.type) {
    case model::NodeType::client:
      return Node{.behaviour = behaviours::ClientBehaviour{
                      g, index, workload::client_seed(g.scenario().seed, index)}};
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
    // A message queue's queue is the broker's append (spec §6: 1M msg/s at 100 µs); what its
    // consumer groups have not read waits in THEIR queues, behind it.
    case model::NodeType::message_queue:
      return make_server(behaviours::MessageQueueBehaviour{g, index}, resolved);
    // A worker pool's queue is one M/M/c of `workers` servers at `cost` a job, its room the
    // retention of the queue it reads (graph::Graph::connect_consumer): a backlog of unread
    // messages, and what overflows it deleted unconsumed.
    case model::NodeType::worker_pool:
      return make_server(behaviours::WorkerPoolBehaviour{g, index}, resolved);
    // An external sink's queue is the provider answering (the type's 1k/s at 100 ms, service SCV
    // 2.0); serve fails `failure_rate` of what it served (fail_share).
    case model::NodeType::external_sink:
      return make_server(behaviours::ExternalSinkBehaviour{g, index}, resolved);
    // A kv_cluster's queues are its machines (graph::apply_kv_ring: `nodes` copies of `capacity`,
    // a hot and a cool group behind its own ring when N < nodes); each request is N
    // replica-requests (its cost) and is answered at the quorum's reply
    // (behaviours::KvClusterBehaviour).
    case model::NodeType::kv_cluster:
      return make_server(behaviours::KvClusterBehaviour{g, index}, resolved);
    // A database's queues are its primaries and its replicas (make_database), whatever a
    // balancer in front of it does.
    case model::NodeType::sql_db:
      return make_database(behaviours::SqlDbBehaviour{g, index}, resolved);
    // An id generator (spec §18 entry 12) is a service whose capacity and service time its
    // algorithm supplied at build (defaults::defaults_of): no behaviour of its own. A search index
    // (spec §19 entry 10; book ch. 13's trie database) is service sugar in the same way: a service
    // with no steps and the type's numbers (5k/s, 5 ms) unless the scenario states its own. Serving
    // is read-mostly, so nothing else is modelled; its `build_interval` is stored and printed, and
    // the rebuild's load is ignored.
    case model::NodeType::service:
    case model::NodeType::search_index:
    case model::NodeType::id_generator:
      return make_server(behaviours::ServiceBehaviour{g, index}, resolved);
    // The four types with no model yet (phase 3): create() refused every scenario that has one
    // (model::simulated), so no Simulation is ever constructed over them.
    case model::NodeType::ws_gateway:
    case model::NodeType::service_discovery:
    case model::NodeType::notification_fanout:
    case model::NodeType::frontier:
      break;
  }
  // C++23 std::unreachable(): tells the compiler control never gets here, replacing the
  // compiler-specific __builtin_unreachable(); reaching it is undefined behaviour. The switch
  // above has no `default:`, so -Wswitch (an error under -Werror) names any enumerator a new
  // node type adds without a case, where a default would have turned it silently into a service.
  std::unreachable();
}

// The queues of a node that serves (spec §17 entry 5: what the balancer in front, if any, does to
// the replica group; graph::Graph::mark_balanced_targets sets the flags and documents the models).
// A node reached both through a balancer and directly gets the balancer's model for all its
// traffic: the model belongs to the group, not to one path. Pattern: Strategy (data-driven) — the
// three replica models differ only in the copy groups built here; serve() runs any list of groups
// the same way.
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

// A SQL database (spec §18 entry 6, behaviours::SqlDbBehaviour; book ch. 1 "database replication"
// and "sharding") with N = replicas copies per shard and S = shards is TWO kinds of copy, each copy
// its own queue of the node's capacity (concurrency and rate bounded by `pool`,
// graph::apply_database_keys). graph::database_shares says what one copy of each kind is sent; in
// short:
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

// A bandwidth link's pipe (Pipe), or nothing for a link without a bandwidth: its queue of
// megabytes (graph::ResolvedLink::pipe, defaults::pipe_capacity), the size of every op its
// receiver can be sent with one (Graph::build refused a link that carries an unsized op, so these
// cover every op that crosses it) and the ops that are writes on it (graph::ResolvedLink::
// write_ops: for the receiver or a node behind it that the request's body reaches, under its own
// name or a sized step's), which a hop pays whole while any other op pays its first megabyte
// (MegabyteService::serialising). `slot` is its sample's place in StepRecord::links.
std::optional<Simulation::Pipe> Simulation::make_pipe(const graph::Graph& g, std::size_t link,
                                                      std::size_t slot) {
  const auto& resolved = g.links()[link];
  if (!resolved.pipe) return std::nullopt;
  const auto receiver = graph::demand_receiver(resolved);
  std::map<std::string, double, std::less<>> megabytes_by_op;
  for (const auto& op : g.arriving_ops(receiver)) {
    if (const auto size = g.request_bytes(receiver, op)) {
      megabytes_by_op.emplace(op, defaults::byte_cost(*size));
    }
  }
  auto timing = behaviours::MegabyteService::serialising(resolved.pipe->service_time_s,
                                                         megabytes_by_op, resolved.write_ops);
  return Pipe{.group = CopyGroup{.queue = fluid::FluidQueue{*resolved.pipe}},
              .timing = std::move(timing),
              .megabytes_by_op = std::move(megabytes_by_op),
              .slot = slot};
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

// Tells each sender which of its links are pipes, and to whom (Node::pipes_out): a link carries
// demand one way, from its sending side to its receiving side (graph::demand_sender and
// demand_receiver), and there is one link per pair and direction, so (sender, receiver) finds the
// pipe a hop crosses.
void Simulation::connect_pipes(const graph::Graph& g) {
  for (std::size_t link = 0; link < g.links().size(); ++link) {
    const auto& resolved = g.links()[link];
    if (!resolved.pipe) continue;
    nodes_[graph::demand_sender(resolved)].pipes_out.emplace_back(graph::demand_receiver(resolved),
                                                                  link);
  }
}

// Tells each message queue which worker pools consume it (Node::consumer_groups): every pool whose
// `consumes` names the queue (Graph::build checked that it links to it). A pool reading a frontier
// is no message queue's group: a frontier is not simulated yet.
void Simulation::connect_consumer_groups(const graph::Graph& g) {
  for (std::size_t pool = 0; pool < g.nodes().size(); ++pool) {
    const auto& spec = g.nodes()[pool].spec;
    if (spec.type != model::NodeType::worker_pool || !spec.consumes) continue;
    const auto queue = *g.index_of(*spec.consumes);  // Graph::connect_consumer resolved it
    if (g.nodes()[queue].spec.type != model::NodeType::message_queue) continue;
    nodes_[queue].consumer_groups.push_back(pool);
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
// g = 0.9). An acyclic graph takes two (the second confirms the first) when nothing is lost;
// with losses, a few more (below).
//
// Why it converges whenever a steady state exists: starting from no flow at all, demand can only
// rise from pass to pass (every node forwards a non-decreasing function of what it receives), and
// what a loop sends round is bounded by the drain of its nodes, which capacity caps. A rising,
// bounded sequence converges, here to the smallest steady state. One mechanism breaks the "only
// rise": a step after a failed step does not run (spec §19 entry 11,
// behaviours::detail::HopBlocks::forward), and a pass can thin a later step only by delivered
// fractions measured in an earlier pass (the backward pass runs after the forward one). So the
// first pass sends every step the full flow, and the passes after it carry each loss one step
// further down an op's steps: a chain of k lossy steps settles in about k + 1 passes more. A loss
// that feeds back on itself (a node called several times by one op, [a, a, a, a, a], whose later
// calls are thinned by its own loss, or two ops whose losses load each other's nodes) is a
// fixed-point iteration of its own, d = f(d): the more a delivers, the more its later calls are
// sent, the less it delivers, so f is decreasing. Fed back raw (d ← f(d)), a steep f (|f'| > 1)
// never settles: the fractions flip between a high and a low value every pass. So settle mode thins
// by a damped running average instead (average_settling_fractions, DampedFraction):
//   d̄ ← (1 − w) d̄ + w f(d̄)                   (damped fixed-point iteration, Mann's "averaging")
// whose fixed point is the same, d̄ = f(d̄); f decreasing and continuous has exactly one. Near it
// the error is multiplied each pass by the damped map's slope 1 − w + w f', which shrinks it when
// it lies in (−1, 1), i.e. when f' > 1 − 2 / w. A fixed w = 1/2 (the plain halfway average) thus
// settles only for f' > −3: five calls to one node (f' = −1.79) do, ten (f' = −3.65) and twenty
// (f' = −6.2) did not, flipping round the fixed point until the pass cap. So w starts at 1/2 and
// is halved whenever d − d̄ changes sign, the sign that d̄ overshot the fixed point: each halving
// doubles the slope the iteration can absorb, so after k overshoots it settles any f' > 1 − 2^(k+2)
// and stops overshooting (the slope turns positive) once w < 1 / (1 − f'). A fraction that never
// overshoots keeps w = 1/2 and settles exactly as before. w stops halving at min_settling_weight
// (1/1024): the one limit left, f' > −2047, far steeper than any op calls one node (twenty calls
// are −6.2). Five calls to `a` (3686/s, a room of one) at 1k/s settle at d = 0.9: load 4095, 10 %
// dropped; ten and twenty calls at the capacity that makes d = 0.9 their fixed point settle there
// too (tests/theory/test_failed_steps.cpp). `run` cannot settle
// there in that case: its steps thin by the raw last step's fraction (the one-step lag), so it
// shows a sawtooth, a step at 5000 offered and red alternating with one at 2988 and yellow, for the
// whole run; its averages over many steps are not the fixed point either. The estimate of every
// shipped scenario that runs converges (news-feed's, the one with losses before later steps,
// included); passes that still oscillate end at the cap with `converged` false.
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
// one-slot line (a million-step link would otherwise copy a million Flows), with a copy of its
// pipe, if it has one, that holds nothing and has lost nothing yet. The queues need no reset
// either: settle mode sets each backlog from the demand (FluidQueue::settle). What the nodes
// were built with comes along unchanged, a queue's consumer groups included (so estimate reports
// its lag, record_queue_lags).
Simulation Simulation::settling_copy() const {
  Simulation scratch;
  scratch.order_ = order_;
  scratch.step_ = step_;
  scratch.pipe_count_ = pipe_count_;
  // faults_ stays empty: estimate is the steady state without the scripted faults (every node in
  // the identity condition), which `simsys estimate` says above its table.
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
  for (std::size_t i = 0; i < links_.size(); ++i) {
    if (!links_[i].pipe) continue;
    auto& pipe = scratch.links_[i].pipe.emplace(*links_[i].pipe);
    pipe.group.waiting.clear();
    pipe.wait = flow::LatencyEstimate{};
    pipe.passed = 1.0;
    pipe.settling_passed = DampedFraction{};
  }
  return scratch;
}

StepRecord Simulation::evaluate(SimTime now, SimTime dt, Mode mode) {
  StepRecord record{.t = now,
                    .nodes = std::vector<NodeSample>(nodes_.size()),
                    .links = std::vector<LinkSample>(pipe_count_)};
  forward_pass(now, dt, mode, record);
  record_queue_lags(record.nodes);
  backward_pass();
  if (mode == Mode::settle) average_settling_fractions();
  record.responses = client_responses(now);
  return record;
}

// The damping of estimate()'s thinning (see estimate): after each settle pass every delivered
// fraction later steps are thinned by, a node's per op and a pipe's kept share, moves towards what
// the pass measured (DampedFraction::move_towards). Exactly 1 stays 1 (a lossless path is
// untouched), and at the fixed point d̄ = d.
void Simulation::average_settling_fractions() {
  for (auto& node : nodes_) {
    for (auto& [op, answer] : node.responses) {
      answer.settling_delivered.move_towards(answer.delivered);
    }
  }
  for (auto& link : links_) {
    if (link.pipe) link.pipe->settling_passed.move_towards(link.pipe->passed);
  }
}

// Damped fixed-point iteration with an adaptive weight (see estimate for why it converges):
//   d̄ ← (1 − w) d̄ + w d,   w halved when (d − d̄) and the previous pass's gap differ in sign.
// At w = 1/2, (1 − w) d̄ + w d is (d̄ + d) / 2 to the last bit (halving is exact in binary), so a
// fraction that never overshoots settles exactly as the plain halfway average did.
void Simulation::DampedFraction::move_towards(double measured) {
  const double gap = measured - value;
  if (gap * last_gap < 0.0) weight = std::max(weight / 2.0, min_settling_weight);
  last_gap = gap;
  value = (1.0 - weight) * value + weight * measured;
}

// Each message queue's lag (NodeSample::queue_lag_s has the formula): the largest of its consumer
// groups' waits this step. After the forward pass, since a queue comes before its pools in the
// order and their waits are this step's only once they have served.
void Simulation::record_queue_lags(std::span<NodeSample> samples) const {
  for (std::size_t queue = 0; queue < nodes_.size(); ++queue) {
    const auto& groups = nodes_[queue].consumer_groups;
    if (groups.empty()) continue;  // not a message queue
    double lag = 0.0;
    // flow::nan_propagating_max, not std::max: a NaN wait met at any group stays in the fold
    // (std::max would let the next finite wait replace it).
    for (const std::size_t pool : groups) {
      lag = flow::nan_propagating_max(nodes_[pool].wait.mean_s, lag);
    }
    samples[queue].queue_lag_s = lag;
  }
}

// Forward pass (spec §3.2-3.4), nodes in topological order. For each node:
//   1. receive: pop every incoming link into the node's inflow;
//   2. the node's condition at `now` (FaultSchedule::condition_at: the identity outside every
//      scripted fault, and always in estimate, whose copy has no faults) is handed to it: a
//      client offers its load times the condition's rate factor (in advance mode it first ticks
//      its noise by dt; settle mode never ticks, so estimate sees the curve without noise at the
//      time it passes, t = 0); any other node (service, load balancer, rate limiter) runs its
//      fluid queue, derated by the condition (serve), on the inflow and routes the part it
//      drained (backlog included, with the queue's op mix: see serve) for the step
//      [now, now + dt), each call after another sent only what
//      the calls before it delivered at the end of the previous step (delivered_last_step);
//      what the route rejects is recorded and goes nowhere (account_rejections);
//   3. send: accumulate the routed flow per link, pass it through the link's pipe if the link
//      has a bandwidth (pass_through_pipe), and push it into each link's delay line.
// Why this order is right: a forward link's target comes later in the order, so it pops
// after this step's push (a zero-step link delivers in the same step). A feedback link's
// target came earlier, so it already popped and gets this push next step. Every link is
// pushed exactly once per step (an empty flow if the source routed nothing on it) and
// popped exactly once, which keeps each delay line's ring in step with the clock.
void Simulation::forward_pass(SimTime now, SimTime dt, Mode mode, StepRecord& record) {
  auto& samples = record.nodes;
  for (const std::size_t index : order_) {
    receive(index, mode);
    // What each of this node's calls delivered at the end of the LAST evaluation: the backward
    // pass that fills the response tables runs after this one, and a pipe on the way has not
    // passed this step's flow yet (send, below), so every target reads one step stale. Before the
    // first backward pass every table says 1 (Answer), so the first step thins nothing.
    const auto delivered_last_step = [this, index, mode](std::size_t target, std::string_view op) {
      return hop_delivered_for_thinning(index, target, op, mode);
    };
    const Condition condition = faults_.condition_at(index, now);
    // A client's sample stays all zeros, green: a source has no queue.
    const behaviours::Routed routed = std::visit(
        core::Overloaded{[&](behaviours::ClientBehaviour& client) {
                           if (mode == Mode::advance) client.tick(now, dt);
                           client.set_rate_factor(condition.rate_factor);
                           return client.offer(now);
                         },
                         // A generic lambda: every other alternative is a queueing node.
                         // Overload resolution prefers the non-template ClientBehaviour&
                         // lambda for a client, so this one sees only servers.
                         [&](auto& server) {
                           return server.route(serve(index, dt, mode, condition, samples[index]),
                                               now, dt, delivered_last_step);
                         }},
        nodes_[index].behaviour);
    account_rejections(index, routed, samples[index]);
    send(index, routed, dt, mode, record.links);
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
//    and served = drain − rejected. Out of one, served / inflow counted
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
// Capacity units (spec §17 entry 4): the queue holds WORK, not requests. An op of cost c_op
// arriving at r_op req/s brings r_op × c_op units/s (in_units), the FluidQueue serves `capacity`
// units/s, and everything below is in units. Only on the way out does a drained op become requests
// again: forwarded units / c_op (in_requests).
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
// megabytes they move: its queue waits for whole objects, fed their size mix's moments
// (time_whole_objects), and each op is answered at its own time after that wait (own_sojourn).
//
// A scripted fault (FaultSchedule) enters here and nowhere else for a queueing node: every copy
// group steps on its capacity derated by the node's `condition` (fluid::Derating: capacity ×
// capacity_factor, service time + extra_latency), which is the identity outside every fault. Every
// queueing type (a service, a balancer, a limiter's gate or leaky bucket, a cache's or a CDN's
// lookup, a database's primaries and replicas, a kv_cluster's machines, a worker pool, a broker, a
// sink) is derated alike, because its queues are built here; its behaviour sees only what drained.
// A node_down of every copy is capacity 0: nothing drains, the room fills to queue_limit and the
// rest is dropped, and when the fault ends the backlog drains at the restored capacity.
flow::Flow Simulation::serve(std::size_t index, SimTime dt, Mode mode, const Condition& condition,
                             NodeSample& sample) {
  auto& node = nodes_[index];
  time_whole_objects(node);
  const fluid::Derating derating = derating_of(condition);
  // Not const: `writes` becomes the primaries' committed writes below.
  auto [reads, writes] = split_writes(in_units(node.inflow, node.behaviour), node.behaviour);
  flow::Flow drained;
  flow::Flow dropped;            // units, the clients' part (account_rejections reads it)
  ReplicationTally replication;  // a database's committed and missed writes (replay_committed)
  sample = NodeSample{};
  // At most two groups (max_copy_groups): fixed arrays, no allocation per step.
  std::array<flow::LatencyEstimate, max_copy_groups> sojourns{};
  std::array<flow::LatencyEstimate, max_copy_groups> waits{};
  std::array<behaviours::MachineGroup, max_copy_groups> machines{};
  for (std::size_t i = 0; i < node.copy_groups.size(); ++i) {
    auto& group = node.copy_groups[i];
    const flow::Flow arrivals = group_arrivals(group, reads, writes);
    GroupStep step = serve_copies(group, arrivals, dt, mode, derating, node.behaviour, sample);
    // The busiest kind of copy speaks for the node's load: the hottest copy behind a ring (it
    // takes the largest share), the busier of a database's primary and replicas.
    if (i == 0 || step.copy.rho > sample.rho) {
      sample.rho = step.copy.rho;
      sample.zone = step.copy.zone;
    }
    sojourns[i] = step.copy.latency;
    waits[i] = with_extra_before_bytes(step.copy.wait, node.behaviour, derating);
    // One machine's sojourn, and the share the group's copies kept of what they were sent (the
    // arrivals are one copy's; step.dropped is the group's): a kv_cluster's quorum reads both.
    machines[i] = {.sojourn = step.copy.latency,
                   .kept = kept_fraction(step.dropped.total().per_second,
                                         arrivals.total().per_second * group.copies)};
    replay_committed(group, step, node.behaviour, writes, replication);
    drained += clients_part(group, std::move(step.drained), node.behaviour);
    dropped += clients_part(group, std::move(step.dropped), node.behaviour);
  }
  fail_share(failure_rate_of(node.behaviour), drained, dropped, sample);
  node.dropped = in_requests(dropped, node.behaviour);
  // std::span with C++17 class template argument deduction: `std::span{sojourns}` deduces
  // std::span<LatencyEstimate, max_copy_groups> from the array without spelling the type, and
  // first(n) views only the n groups in use.
  const auto in_use = std::span{sojourns}.first(node.copy_groups.size());
  record_sojourns(node, in_use, std::span{waits}.first(node.copy_groups.size()), sample);
  report_machines(node.behaviour, std::span{machines}.first(node.copy_groups.size()));
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
  if (const auto* megabyte_service = megabyte_service_of(node.behaviour)) {
    node.write_sojourn = node.read_sojourn;
    sample.latency = request_weighted_megabyte_sojourn(node.inflow, node.wait, *megabyte_service);
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

// A byte-sized node's queue waits for whole objects (spec §19 entry 2): each request holds one of
// its c megabyte slots for its whole transfer, so the queue is fed the moments of this step's
// arrivals' object times (behaviours::MegabyteService::object_time_moments):
//   s_obj = Σ_op p_op × MB_op × s_MB          the mean, what Erlang C is fed (p_op = rate share)
//   c_s²  = Σ_op p_op (1 + cs²) s_op² / s_obj² − 1     the size mix's SCV, what the Allen–Cunneen
//                                                      factor (c_a² + c_s²) / 2 is fed
// recomputed every step as the op mix moves, like the per-op costs, in one call
// (fluid::FluidQueue::set_service) so mean and spread always belong together. c and the rate stay
// those of the megabytes (c = MB/s × s_MB), so rho is unchanged (Little's law: objects/s × s_obj /
// c = MB/s / capacity) and only the wait becomes a whole object's: 5 uploads/s of 50 MB at a
// default store wait (1 + 0) / 2 × C(20, 0.5) × 2 s / (20 × 0.5) = 0.373 ms (one size: c_s² = 0),
// not the 0.0149 ms of 1 MB units. A step in which nothing arrives keeps the last mix's moments
// (it was one megabyte's time, at the node's per-object SCV, before any).
void Simulation::time_whole_objects(Node& node) {
  const auto* megabyte_service = megabyte_service_of(node.behaviour);
  if (megabyte_service == nullptr) return;
  const auto moments = megabyte_service->object_time_moments(node.inflow);
  if (!moments) return;
  for (auto& group : node.copy_groups) group.queue.set_service(*moments);
}

// A byte-sized node's latency: each op's own sojourn (own_sojourn) mixed by the requests of it
// that arrived this step (`requests`), what an average request saw, as a database mixes its reads
// and writes (request_weighted_sojourn); a pipe's likewise, each op's wait plus its answer time on
// the wire (pass_through_pipe). Nothing arrived: what a read arriving now would see, the wait plus
// one megabyte (an idle pipe passes the ops it carries instead, so it never gets here empty-handed
// unless it carries none). The mixture of behaviours::mixture, accumulated in place over the
// arrivals (two passes, no allocation per step), with p_op = rate_op / Σ rate and op o's sojourn of
// mean m_o and variance v_o:
//   mean     = Σ_o p_o m_o
//   variance = Σ_o p_o (v_o + (m_o − mean)²)       (law of total variance: within + between)
// equal to Σ p (v + m²) − mean² but written as a sum of non-negative terms, so it cannot come out
// negative when the two nearly cancel (one op: a 12 s upload with a variance of 0.03 s²).
flow::LatencyEstimate Simulation::request_weighted_megabyte_sojourn(
    const flow::Flow& requests, const flow::LatencyEstimate& wait,
    const behaviours::MegabyteService& megabyte_service) {
  const double total_per_s = requests.total().per_second;
  if (total_per_s <= 0.0) return megabyte_service.first_megabyte(wait);
  double mean_s = 0.0;
  for (const auto& [op, rate] : requests.entries()) {
    if (rate.per_second <= 0.0) continue;
    mean_s += rate.per_second / total_per_s * megabyte_service.sojourn(op, wait).mean_s;
  }
  double variance_s2 = 0.0;
  for (const auto& [op, rate] : requests.entries()) {
    if (rate.per_second <= 0.0) continue;
    const auto own = megabyte_service.sojourn(op, wait);
    const double apart_s = own.mean_s - mean_s;
    variance_s2 += rate.per_second / total_per_s * (own.variance_s2 + apart_s * apart_s);
  }
  return flow::LatencyEstimate{.mean_s = mean_s, .variance_s2 = variance_s2};
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

// A database's replication numbers (spec §18 entry 6; NodeSample::staleness_s,
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

// One copy group's step (replicas, spec §16 entry 4): `waiting` and the queue describe one copy,
// which is sent `arrivals` (group_arrivals). Returns one copy's fluid step and what the group's
// copies drained (and, at a database replica, dropped), and adds the clients' part (clients_share)
// of their totals to `sample` (add_copies).
Simulation::GroupStep Simulation::serve_copies(CopyGroup& group, const flow::Flow& arrivals,
                                               SimTime dt, Mode mode,
                                               const fluid::Derating& derating,
                                               const behaviours::Behaviour& behaviour,
                                               NodeSample& sample) {
  GroupStep step = advance_group(group, arrivals, dt, mode, derating);
  // The queue is well-mixed (see serve): what it drains, drops and holds has the content's op
  // mix, so the clients' share of the content is their share of each of those.
  add_copies(sample, step.copy, group.copies, clients_share(group, arrivals, behaviour),
             clients_share(group, step.content, behaviour));
  return step;
}

// The fluid step of one copy of `group`, sent `arrivals` (per op, in capacity units per second),
// derated by the node's condition (fluid::Derating: every copy of the group alike, the survivors of
// a node_down sharing its load evenly; the identity outside every fault and when estimating),
// settled first when estimating, and what the group's copies drained and dropped of it, per op,
// in the content's mix (the queue is well-mixed, see serve). Every copy group of a node runs
// through here (serve_copies), and so does a bandwidth link's pipe (pass_through_pipe).
Simulation::GroupStep Simulation::advance_group(CopyGroup& group, const flow::Flow& arrivals,
                                                SimTime dt, Mode mode,
                                                const fluid::Derating& derating) {
  const Rate per_copy = arrivals.total();
  if (mode == Mode::settle) {
    group.queue.settle(per_copy);
    group.waiting = settled_waiting(arrivals, group.queue.backlog());
  }
  GroupStep step{.content = queue_content(group.waiting, arrivals, dt)};
  step.copy = group.queue.advance(per_copy, dt, derating);
  step.drained = take_drained(group, step.content, step.copy);
  // Every group's drops, per op: a replica's replayed writes it missed (replay_committed) and,
  // on every node, the requests lost (account_rejections).
  step.dropped = dropped_part(step.content, step.copy, group.copies);
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

// What node `index` routed goes onto its links: summed per link (one push per link and step,
// see DelayLine), through the link's pipe first when it has a bandwidth (pass_through_pipe, whose
// sample lands in `link_samples`), then into the delay line.
void Simulation::send(std::size_t index, const behaviours::Routed& routed, SimTime dt, Mode mode,
                      std::span<LinkSample> link_samples) {
  const auto& outgoing_links = nodes_[index].outgoing_links;
  for (const std::size_t link : outgoing_links) links_[link].sent.clear();
  for (const auto& out : routed.outgoing) links_[out.link].sent += out.flow;
  for (const std::size_t link : outgoing_links) {
    if (links_[link].pipe) pass_through_pipe(links_[link], dt, mode, link_samples);
  }
  if (mode == Mode::settle) return;  // estimate reads `sent` directly; lines stay untouched
  for (const std::size_t link : outgoing_links) links_[link].line.push(links_[link].sent);
}

// A bandwidth link's pipe for one step (spec §19 entry 3), between the sender and the delay line: a
// fluid queue like a byte-sized node's, in megabytes.
//   demand   = Σ_op rate_op × MB_op                           MB/s put on the link
//   s_obj    = Σ_op p_op MB_op × s_MB,  c_s² = the size mix's SCV    (MegabyteService::
//              object_time_moments: one request holds the pipe for its whole serialisation,
//              s_MB = 1 MB / bandwidth; p_op = rate_op / Σ rate)
//   Wq       = (c_a² + c_s²) / 2 × Erlang C(1, rho) × s_obj / (1 − rho)     (Allen–Cunneen with
//              one server: for Poisson arrivals Pollaczek–Khinchine's M/G/1 wait, exact), at most
//              the room's drain time, 1000 MB / bandwidth, and at least the backlog's
//              (fluid::FluidQueue::advance)
// What it drains goes on to the receiver as requests; what overflows the room is lost, and the
// share it kept, 1 − dropped / arrived (in MB: the queue is well-mixed, so every op loses the same
// share), multiplies the delivered fraction of every hop over the link (hop_delivered). Its wait
// is kept for the backward pass, which adds the wait plus the op's answer time to every hop over
// the link (hop_response: a write's whole serialisation, MB_op × s_MB, any other op's first
// megabyte, min(MB_op, 1) × s_MB). Its sample's latency is that hop term mixed by the requests
// that arrived (request_weighted_megabyte_sojourn); in a step with none, by the ops the link
// carries, one each (an idle pipe reports what its own size mix would see, not one megabyte).
// Direction: the pipe is fed by the DEMAND that crosses the link, each request charged its whole
// stated size in the demand direction, whichever way its bytes really travel (a 50 MB `stream`
// read loads the link from the viewer towards the CDN, though the video comes back the other way).
// Replies add no bytes, so an `inout` link is one pipe, fed by its requests only; two links
// `x->y` and `y->x` that both state a bandwidth are two pipes, each fed by its own direction's
// demand.
// Ignored: TCP's slow start and windows, packet loss and retransmission, the propagation delay
// changing with load (the link's one-way latency stays fixed), and the true direction of each
// byte (above).
void Simulation::pass_through_pipe(Link& link, SimTime dt, Mode mode,
                                   std::span<LinkSample> link_samples) {
  Pipe& pipe = *link.pipe;
  const flow::Flow& requests = link.sent;
  if (const auto moments = pipe.timing.object_time_moments(requests)) {
    pipe.group.queue.set_service(*moments);
  }
  const flow::Flow megabytes = to_megabytes(requests, pipe.megabytes_by_op);
  const bool idle = requests.total().per_second <= 0.0;
  // A link has no condition yet (dc_down's links are Task 7's): the identity derating.
  const GroupStep step = advance_group(pipe.group, megabytes, dt, mode, fluid::Derating{});
  pipe.wait = step.copy.wait;
  pipe.passed = kept_share(step.dropped, megabytes);
  link_samples[pipe.slot] =
      LinkSample{.demand_per_s = step.copy.demand_per_s,
                 .drain_per_s = step.copy.drain_per_s,
                 .dropped_per_s = step.copy.dropped_per_s,
                 .backlog = step.copy.backlog,
                 .rho = step.copy.rho,
                 .latency = request_weighted_megabyte_sojourn(
                     idle ? one_of_each(pipe.megabytes_by_op) : requests, pipe.wait, pipe.timing),
                 .zone = step.copy.zone};
  link.sent = to_requests(step.drained, pipe.megabytes_by_op);
}

// Backward pass (spec §16 entry 1), nodes in reverse topological order: a server's response to op =
// own sojourn this step (own_sojourn: a database's write the primary's, its read a replica's) + its
// hops (ServiceBehaviour::response) or its share-weighted routes (LoadBalancerBehaviour::response).
// Reverse order means every target later in the order was already refreshed this step. Asynchronous
// hops (those closing an op-level call cycle, a write_back cache's flushes and a blob store's event
// notifications: graph::Graph::is_async_hop) add no latency and fail no request, so they are never
// looked up. A hop over a node-level back edge that closes no call cycle targets a node earlier in
// the order, not yet refreshed: it reads that node's previous-step entry, one step stale (the same
// "previous step's value" rule as the forward pass). The tables hold every op that can reach the
// node from construction on (register_reachable_ops) and refresh all of them every step, so an op
// whose flow has not arrived yet, or has paused, is estimated at the node's current load.
//
// Next to each latency it composes the DELIVERED fraction (spec §17 entry 8): the share of the
// node's requests of that op that come back answered, the node's own `passed` share times what its
// downstream calls deliver (behaviours::NodeBehaviour::delivered: sequential parts multiply, splits
// mix by share). A feedback target is read one step stale, as for latency.
void Simulation::backward_pass() {
  // C++20 std::views::reverse: walks the order back to front without copying it,
  // replacing a reverse-iterator loop (rbegin/rend) or a reversed copy.
  for (const std::size_t index : order_ | std::views::reverse) {
    auto& node = nodes_[index];
    // This node's hops: a pipe on the link to a target comes with the target's answer.
    const auto lookup = [this, index](std::size_t target,
                                      std::string_view op) -> flow::LatencyEstimate {
      return hop_response(index, target, op);
    };
    const auto fraction = [this, index](std::size_t target, std::string_view op) {
      return hop_delivered(index, target, op);
    };
    // Only servers compose a response here; clients are read in client_responses().
    std::visit(core::Overloaded{[](const behaviours::ClientBehaviour&) {},
                                [&](const auto& server) {
                                  for (auto& [op, answer] : node.responses) {
                                    answer.latency =
                                        server.response(op, own_sojourn(node, op), lookup);
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
  std::vector<ResponseSample> responses;
  for (std::size_t index = 0; index < nodes_.size(); ++index) {
    const auto* client = std::get_if<behaviours::ClientBehaviour>(&nodes_[index].behaviour);
    if (client == nullptr) continue;
    const auto lookup = [this, index](std::size_t target,
                                      std::string_view op) -> flow::LatencyEstimate {
      return hop_response(index, target, op);
    };
    const auto fraction = [this, index](std::size_t target, std::string_view op) {
      return hop_delivered(index, target, op);
    };
    for (const auto& [op, offered] : client->offered(now)) {
      ResponseSample sample{.client = index,
                            .op = op,
                            .offered = offered,
                            .answered = Rate{offered.per_second * client->delivered(op, fraction)},
                            .response = client->response(op, lookup)};
      sample.delivered_response = sample.response;  // before give_up_after cuts `response`
      if (const auto timeout_s = client->timeout_s()) give_up_after(*timeout_s, sample);
      responses.push_back(std::move(sample));
    }
  }
  return responses;
}

// A client's `timeout` (book ch. 1 and ch. 8: a client does not wait for ever). Of the requests the
// path delivered, the share whose response time is below the timeout is answered, with the moments
// of that share alone; the rest are timed out. The response time is the lognormal fitted to the
// response's mean and variance (queueing::lognormal_below), the same fit the reports read
// percentiles off. A write at a byte-sized node is answered at its last byte
// (behaviours::MegabyteService::sojourn), and that is already in `response`, so an upload is timed
// out on its whole transfer. Only the client gives up: nothing cancels the request at the servers,
// which still queue and serve it, so their demand and utilisation do not change (the work of a
// timed-out request is wasted, not saved). Ignored: retries (phase 3) and a request answered after
// the client left (it counts as timed out, never as answered late).
void Simulation::give_up_after(double timeout_s, ResponseSample& sample) {
  const auto below =
      queueing::lognormal_below(sample.response.mean_s, sample.response.variance_s2, timeout_s);
  // The timed-out part first and the answered part as what is left, so answered + timed_out is
  // the delivered rate exactly, and a share answered too small for a double to tell 1 − share
  // from 1 (under 1.1e-16: unique-id's ids, 126 ms against 50 ms, answer about 5e-21) answers
  // nothing at all rather than a few requests a year that the table would print latencies for.
  const double delivered_per_s = sample.answered.per_second;
  sample.timed_out = Rate{delivered_per_s * (1.0 - below.fraction)};
  sample.answered = Rate{delivered_per_s - sample.timed_out.per_second};
  sample.response = {.mean_s = below.mean_s, .variance_s2 = below.variance_s2};
}

// The node's own sojourn for `op` this step (record_sojourns): a database's write waits at
// its primary, its read at a replica; a byte-sized node answers a write at its last byte and a
// read at its first megabyte after the same wait (behaviours::MegabyteService::sojourn); any
// other node has one sojourn for every op.
flow::LatencyEstimate Simulation::own_sojourn(const Node& node, std::string_view op) {
  if (const auto* megabyte_service = megabyte_service_of(node.behaviour)) {
    return megabyte_service->sojourn(op, node.wait);
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

// The pipe a hop from `caller` to `target` crosses (connect_pipes), or null when their link has
// no bandwidth: the usual case, an empty list.
const Simulation::Pipe* Simulation::pipe_between(std::size_t caller, std::size_t target) const {
  for (const auto& [receiver, link] : nodes_[caller].pipes_out) {
    if (receiver == target) return &*links_[link].pipe;
  }
  return nullptr;
}

// What the backward pass hands a behaviour as the answer of a hop (behaviours::ResponseLookup):
// the target's response to `op` and, when the link to it is a pipe, the pipe's term before it,
// this step's wait plus the request's answer time on the wire: a write's whole serialisation,
// MB_op × 1 MB / bandwidth, any other op's first megabyte (MegabyteService::serialising). One
// mechanism for every hop of every behaviour: each adds the link's fixed legs (round_trip) and what
// this returns, so a pipe is counted once per hop, after the two legs, wherever the hop is (a
// client's route, a balancer's link, a service's step, a cache's store hop), and never for an
// asynchronous hop, which nobody looks up. Per step and per (link, op): the wait is the pipe's this
// step, the answer time the op's own. A hop without a pipe returns the target's response
// untouched, so a scenario without a bandwidth runs exactly as before.
flow::LatencyEstimate Simulation::hop_response(std::size_t caller, std::size_t target,
                                               std::string_view op) const {
  const Pipe* pipe = pipe_between(caller, target);
  if (pipe == nullptr) return response_of(target, op);
  return pipe->timing.sojourn(op, pipe->wait) + response_of(target, op);
}

// The same for the delivered fraction (behaviours::FractionLookup): the target's, times the share
// of this step's megabytes the pipe did not drop when the hop crosses one. A pipe's losses fail
// requests as a node's drops do, and multiply with what is lost further on.
double Simulation::hop_delivered(std::size_t caller, std::size_t target,
                                 std::string_view op) const {
  const Pipe* pipe = pipe_between(caller, target);
  if (pipe == nullptr) return delivered_of(target, op);
  return pipe->passed * delivered_of(target, op);
}

// What the forward pass thins `caller`'s later calls by (behaviours::detail::HopBlocks::forward):
// in advance mode the hop's delivered fraction of the last step (hop_delivered); in settle mode
// its running average d̄, the target's settling_delivered times a pipe's settling_passed (the
// damping estimate() needs to settle, see there).
double Simulation::hop_delivered_for_thinning(std::size_t caller, std::size_t target,
                                              std::string_view op, Mode mode) const {
  if (mode == Mode::advance) return hop_delivered(caller, target, op);
  const auto& table = nodes_[target].responses;
  const auto found = table.find(op);
  const double settled = found == table.end() ? 1.0 : found->second.settling_delivered.value;
  const Pipe* pipe = pipe_between(caller, target);
  return pipe == nullptr ? settled : pipe->settling_passed.value * settled;
}

}  // namespace simsys::engine
