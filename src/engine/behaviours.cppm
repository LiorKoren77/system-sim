// Module `simsys.behaviours`: what each node type does with flow (client: offer load
// along its routes; service: forward step demand, serve the rest; load balancer: pass
// everything on across its links) and how it composes response latency from its own
// sojourn plus link legs and downstream responses.
//
// Latency composes on the reply path. A hop to a downstream node costs two link legs
// (request out, reply back) plus that node's own response; sequential hops add, parallel
// hops cost the slowest branch, and an op split over several links (by a client or a load
// balancer) is a share-weighted mixture of its routes (`mixture`). A hop that closes a cycle of
// CALLS (graph::Graph::is_async_hop: chat.msg -> queue -> sync.job -> chat.msg) is asynchronous:
// its flow goes round, but the sender does not wait for it, so it adds nothing to the
// response. A hop over a node-level feedback link that closes no call cycle (A.x -> B.y,
// B.z -> A.w: the boxes loop, the calls do not) is waited for like any other, one step
// stale: the backward pass reads the target's response from the previous step's table,
// because in topological order the target has not been refreshed yet.
//
// C++20 named module, interface unit (see src/core/core.cppm for the longer
// explanation of modules versus headers).
module;
// NO <chrono>/<format>/<print>/<filesystem> here: this interface imports other modules
// (GCC 14.2 'Bad file data' bug, CLAUDE.md §3). Link legs go through simsys::seconds().
#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

export module simsys.behaviours;

import simsys.core;
import simsys.flow;
import simsys.graph;
import simsys.model;
import simsys.workload;

export namespace simsys::behaviours {

// Demand a node sends to one neighbour over one link during a step.
struct Outgoing {
  std::size_t target{};
  std::size_t link{};
  flow::Flow flow{};
};

// What a behaviour does with one step's flow: what it forwards (one Outgoing per
// (target, link), sorted by target then link so runs are deterministic) and what it
// serves itself (the load its own queue sees).
struct Routed {
  std::vector<Outgoing> outgoing{};
  flow::Flow served{};
};

// C++20 concept: a named, compile-time predicate on a type. Replaces SFINAE tricks
// (std::enable_if) and "duck typing discovered in a 30-line template error"; a
// constrained parameter that does not fit fails at the call with the concept's name.
// Here: anything that answers "how long does node `n` take to respond to op `o`?" —
// the engine passes a lambda over its latest per-node estimates; tests pass stubs.
// (std::invocable and std::convertible_to are C++20 standard concepts from <concepts>;
// convertible_to rather than same_as so a lookup returning `const LatencyEstimate&`
// fits too.) Like every concept here it checks signatures, not behaviour: a lookup
// that returns nonsense still satisfies it.
template <class F>
concept ResponseLookup = std::invocable<F, std::size_t, std::string_view> &&
                         std::convertible_to<std::invoke_result_t<F, std::size_t, std::string_view>,
                                             flow::LatencyEstimate>;

// C++20 requires-expression: lists the operations a type must support, checked at
// compile time (spec §5). This is the engine's whole requirement on a node behaviour,
// written in code instead of in a comment on an abstract base class. The lambda is a
// stand-in ResponseLookup (C++20 allows lambdas in unevaluated operands such as this).
// It checks that the calls compile and return the right types; what they compute is
// the tests' job. `cost(op)` is the capacity units one request of `op` uses (plan 2a
// refinement 4): the engine charges the node's queue in units, not requests.
// `route(in, dt)` takes the step length and is called on a non-const behaviour (`node`):
// a stateless node (service, load balancer) ignores dt and may declare route const, but
// a stateful one (the rate limiter's token bucket, next) refills by dt and must change
// itself. Requiring it on `B&` admits both; `cost` and `response` stay const.
template <class B>
concept NodeBehaviour =
    requires(const B b, B& node, const flow::Flow& in, SimTime dt, flow::LatencyEstimate own) {
      { node.route(in, dt) } -> std::same_as<Routed>;
      { b.cost(std::string_view{}) } -> std::same_as<double>;
      {
        b.response(std::string_view{}, own,
                   [](std::size_t, std::string_view) { return flow::LatencyEstimate{}; })
      } -> std::same_as<flow::LatencyEstimate>;
    };

// A source of requests: it offers load at simulated time `now` instead of routing an
// incoming flow, lists the ops it sends (one latency report each), and composes the
// response time it sees for each of them. Signatures only, as for NodeBehaviour.
// C++20 std::span<const std::string>: a non-owning view (pointer + size) of the op names,
// replacing `const std::vector<std::string>&`; a source may store its ops in any
// contiguous container and callers can read but not modify them.
//
// Who owns what: the engine owns the clock, the source owns its random state. `tick(now,
// dt)` is the one call that changes a source: the engine makes it once per real step,
// before `offer`, to advance the source's noise by dt. `offer(now)` is const, a pure
// function of `now` and the last tick, so asking twice gives the same flow, and an engine
// that never ticks (estimate, which evaluates the curve at t = 0) sees the curve exactly,
// without noise, with no special case. `tick` needs a non-const S, hence the second
// parameter `S& source` next to the const `s`.
template <class S>
concept RequestSource = requires(const S s, S& source, SimTime now, SimTime dt) {
  { source.tick(now, dt) };
  { s.offer(now) } -> std::same_as<Routed>;
  {
    s.response(std::string_view{},
               [](std::size_t, std::string_view) { return flow::LatencyEstimate{}; })
  } -> std::same_as<flow::LatencyEstimate>;
  { s.ops() } -> std::convertible_to<std::span<const std::string>>;
};

// Fixed propagation delay out and back over a link of one-way latency `link_leg_s`: it has
// a mean but no variance.
// C++20 designated initializers: `{.mean_s = ..., .variance_s2 = ...}` names each member
// it sets, replacing positional `{2.0 * leg, 0.0}` where a reader must remember which
// number is the mean; names must follow declaration order.
[[nodiscard]] constexpr flow::LatencyEstimate round_trip(double link_leg_s) {
  return {.mean_s = 2.0 * link_leg_s, .variance_s2 = 0.0};
}

// One branch of a split: a request takes it with probability `share` and then sees
// `latency`.
struct Branch {
  double share{};
  flow::LatencyEstimate latency{};
};

// The response time of a request that takes branch i with probability s_i, where branch
// i has latency mean m_i and variance v_i — a mixture of distributions:
//   mean      = Σ s_i m_i
//   variance  = Σ s_i v_i  +  Σ s_i (m_i − mean)²
//               (law of total variance: Var X = E[Var(X | branch)] + Var(E[X | branch]),
//               "the spread within branches plus the spread between them")
// Assumes the shares sum to 1 and the branch choice is independent of the latency on it
// (fixed fractions, as weighted DNS or a load balancer's split give). Written as a sum of
// non-negative terms, the variance cannot come out negative, unlike the textbook
// E[X²] − mean², which loses precision when the two terms nearly cancel. Used by every
// node that splits an op: ClientBehaviour and LoadBalancerBehaviour.
[[nodiscard]] flow::LatencyEstimate mixture(std::span<const Branch> branches);

// Models: a `service` box of System Design Interview (Alex Xu, vol. 1) — the stateless
// web/API tier of ch. 1 ("Scale from zero to millions of users"), and every application
// server of the later designs (ch. 11 news feed fan-out service, ch. 12 chat servers).
// Captured: for each incoming op, the downstream calls it makes (`ops` steps), the
// fan-out of each call (`factor`: one post becomes 100 feed writes), and how the call
// latencies compose — sequential calls add, parallel calls wait for the slowest — and
// how much of the node's capacity each op uses (`cost`, in capacity units: a `write` of
// cost 3 is three `read`s of cost 1, the way an expensive endpoint is weighed when sizing
// a tier in ch. 2). Ignored: per-op service TIME (a costly op uses more capacity, but its
// own sojourn is the node's one service time: the queue is not a multi-class one), the
// burstiness a cost adds (a cost-c request arrives as a batch of c units at once, so
// Kingman's `burstiness`, the SCV of arrivals taken unit by unit, understates its arrival
// variability and the queueing wait with it), retries and timeouts, and conditional steps (`on:
// miss` / `on: failure`): a service has no cache to miss and phase 1 models no failures, so those
// steps never fire here. A service has no `then` steps either (the loader rejects them for this
// type): everything it sends is per op. Hops that close a call cycle are asynchronous
// hand-offs (see `response`).
class ServiceBehaviour {
 public:
  ServiceBehaviour(const graph::Graph& g, std::size_t self);

  // Serves every incoming op and forwards rate × factor along each of the op's steps,
  // asynchronous hops included (over a feedback link, the flow arrives a step later).
  // Returns a fresh Routed every call; if profiling shows the per-step allocations
  // matter, the engine can pass a scratch `Routed&` to be cleared and refilled instead.
  // `dt` is unused: a service holds no state between steps (see NodeBehaviour).
  [[nodiscard]] Routed route(const flow::Flow& in, SimTime dt) const;

  // Capacity units one request of `op` consumes: the spec's `cost` entry, 1 when the op
  // is not listed (every op of a phase-1 node costs one unit).
  [[nodiscard]] double cost(std::string_view op) const;

  // Response time of `op`: own sojourn + Σ sequential hops + slowest parallel hop, where
  // a hop is two link legs plus the downstream node's response. Asynchronous hops are
  // skipped: closing a call cycle, they are a fire-and-forget hand-off of work the caller
  // does not wait for (graph::Graph::is_async_hop). A hop over a node-level feedback link
  // that closes no call cycle is counted; `downstream` then returns the target's previous
  // step (see the module comment). Defined here because it is a template (the
  // constrained `auto` parameter makes it one).
  //
  // C++20 abbreviated function template with a constrained placeholder:
  // `ResponseLookup auto&&` means "any type satisfying ResponseLookup", replacing
  // `template <class F> requires ResponseLookup<F>` — and `auto&&` accepts lambdas,
  // function objects and references alike without copying them.
  [[nodiscard]] flow::LatencyEstimate response(std::string_view op, flow::LatencyEstimate own,
                                               ResponseLookup auto&& downstream) const {
    flow::LatencyEstimate total = own;
    flow::LatencyEstimate slowest_parallel{};  // running max: no buffer of branches
    for (const Hop& hop : hops_of(op)) {
      if (hop.asynchronous) continue;  // closes a call cycle: adds no latency to the caller
      const auto leg = round_trip(hop.link_leg_s) + downstream(hop.target, hop.op);
      if (hop.mode == model::StepMode::sequential) {
        total += leg;
      } else {
        // flow::parallel_max over (max so far, this branch) is the max over all branches,
        // and keeps the approximation's definition in one place.
        slowest_parallel = flow::parallel_max(std::array{slowest_parallel, leg});
      }
    }
    return total + slowest_parallel;
  }

 private:
  // One step of an op, resolved against the graph once at construction.
  struct Hop {
    std::string op;      // the op sent downstream
    std::size_t target;  // node index
    std::size_t link;    // link index the demand travels on
    double factor;       // downstream requests per incoming request
    model::StepMode mode;
    double link_leg_s;  // one-way link latency in seconds
    bool asynchronous;  // closes a call cycle: not waited for, see `response`
  };

  // std::span<const Hop> rather than `const std::vector<Hop>&`: a span can also say "no
  // hops" (an empty view) without a static empty vector to point at.
  [[nodiscard]] std::span<const Hop> hops_of(std::string_view op) const;
  void add_hops(const graph::Graph& g, const std::string& op,
                const std::vector<model::Step>& steps);

  std::size_t self_;
  // std::less<> (C++14 transparent comparator) lets find() take a string_view without
  // building a std::string for every lookup.
  std::map<std::string, std::vector<Hop>, std::less<>> hops_by_op_;
  std::map<std::string, double, std::less<>> cost_by_op_;  // only the ops listed in `cost`
};

// The seed of client `index`'s noise process (spec phase 2a refinement 3): one
// std::mt19937_64 per client, so adding a client never changes another client's noise.
// Formula: (seed × 0x9E3779B97F4A7C15) XOR index, all mod 2^64. The constant is 2^64
// divided by the golden ratio (splitmix64's increment); what matters is that it is odd and
// large. The multiply is there to keep (seed, index) pairs apart, not to decorrelate
// neighbouring seeds (std::mt19937_64's own seeding already does that): a plain
// seed XOR index collides, 1^0 == 0^1, while seed × K moves each seed's block of indices
// far into the high bits, so for the seeds and client counts a scenario uses no two pairs
// share an engine seed (tests/unit/test_behaviours.cpp checks seeds -100..100 × 64
// clients). A negative seed converts to std::uint64_t modulo 2^64 (well defined since
// C++20's two's-complement rule), so it is as deterministic as a positive one. The engine
// (Simulation::make_node) and `simsys curve
// --noise` both call this, so the curve command prints exactly the noise a run will use.
inline constexpr std::uint64_t golden_ratio_increment = 0x9E3779B97F4A7C15;

[[nodiscard]] constexpr std::uint64_t client_seed(int seed, std::size_t index) {
  return (static_cast<std::uint64_t>(seed) * golden_ratio_increment) ^ std::uint64_t{index};
}

// Models: the "User" box at the top of every System Design Interview diagram — web and
// mobile clients as one aggregate source of load (ch. 1; the ch. 2 back-of-the-envelope
// estimates give its rate, e.g. 300M DAU -> QPS). Captured: a request rate that follows
// the scenario's rate curve over time (daily waves, ramps, spikes, replayed traces:
// workload::RateCurve) times a seeded random multiplier (workload::NoiseProcess), split
// into ops by fixed shares, each op split over the links that carry it by link weight
// (DNS round-robin, CDN vs origin). Ignored: think time and closed-loop clients (offered
// load never reacts to latency: a slow site does not make these users wait or leave),
// and client-side timeouts.
class ClientBehaviour {
 public:
  // `seed` seeds this client's noise process; the engine passes client_seed(scenario
  // seed, node index). It defaults to 0 for clients built without noise (tests): a client
  // whose workload has no noise never draws a random number, so its seed is unused.
  ClientBehaviour(const graph::Graph& g, std::size_t self, std::uint64_t seed = 0);

  // Advances the noise by one step of length `dt` (Ornstein-Uhlenbeck, see
  // workload::NoiseProcess) and keeps the multiplier for the following `offer`s. A no-op
  // for a client without noise. `now` is unused: the noise depends on dt only, but the
  // engine hands every source the step's time (see RequestSource).
  void tick(SimTime now, SimTime dt);

  // Multiplier back to 1, and the noise process re-created from its seed as a fresh client
  // holds it. Simulation::estimate calls it on its scratch copy, where only the multiplier
  // matters: settle mode never ticks, so the estimate sees the curve without noise however
  // far a run has got. The running simulation's client is a separate object, untouched.
  void reset_noise();

  // The total rate offered at `now`: curve.rate_at(now) × the multiplier of the last tick
  // (1 before any tick, and always 1 without noise).
  [[nodiscard]] Rate rate(SimTime now) const;

  // The rate offered per op at `now`, rate(now) × op share, sorted by op name: what the
  // client sends this step, which reports weight latencies by.
  [[nodiscard]] std::vector<std::pair<std::string, Rate>> offered(SimTime now) const;

  // Load offered at `now`: rate(now) × op share × route share on each route. A client is a
  // source, so `served` stays empty. Like ServiceBehaviour::route, a fresh Routed per
  // call; the engine may later pass a scratch `Routed&` instead.
  [[nodiscard]] Routed offer(SimTime now) const;

  // Response time the client sees for `op`: over the routes that carry it, a
  // share-weighted mixture (see `mixture`) of (two link legs + the entry node's response).
  // An op the client does not send is a programmer error (std::invalid_argument): the
  // graph guarantees a route for every op of the workload.
  [[nodiscard]] flow::LatencyEstimate response(std::string_view op,
                                               ResponseLookup auto&& downstream) const {
    // C++20 ranges algorithm: std::ranges::binary_search takes the sorted vector itself
    // instead of an iterator pair, so begin and end cannot come from different containers
    // (find_if, find and sort below work the same way, some with a projection).
    if (!std::ranges::binary_search(ops_, op)) {
      throw std::invalid_argument{"ClientBehaviour::response: the client does not send op '" +
                                  std::string{op} + "'"};
    }
    std::vector<Branch> branches;
    for (const RouteLeg& leg : legs_) {
      // Zero-share routes are skipped before their latency is looked up: it may be inf (a
      // zero-capacity node) and inf × 0 is NaN.
      if (leg.op != op || leg.route_share <= 0.0) continue;
      branches.push_back({.share = leg.route_share,
                          .latency = round_trip(leg.link_leg_s) + downstream(leg.target, leg.op)});
    }
    return mixture(branches);
  }

  // The ops this client sends, sorted by name, so reports list them deterministically.
  [[nodiscard]] std::span<const std::string> ops() const { return ops_; }

 private:
  // One graph::ClientRoute plus what the behaviour needs per step.
  struct RouteLeg {
    std::string op;
    std::size_t target;
    std::size_t link;
    double route_share;    // fraction of this op's requests taking this link
    double share_of_rate;  // op share × route share: fraction of the client's rate
    double link_leg_s;     // one-way link latency in seconds
  };

  // The curve is kept by value: a behaviour must not hold a reference into the Graph,
  // which the Simulation does not outlive (see Simulation's constructor).
  workload::RateCurve curve_{};
  std::optional<workload::Noise> noise_spec_{};  // absent: no noise
  std::uint64_t seed_{};
  // No noise means no process at all rather than a sigma-0 one: an absent noise costs
  // nothing per step and owns no random engine.
  std::optional<workload::NoiseProcess> noise_{};
  double noise_multiplier_{1.0};  // max(0, 1 + x) from the last tick; 1 until the first
  std::vector<std::pair<std::string, double>> op_shares_;  // sorted by op
  std::vector<RouteLeg> legs_;
  std::vector<std::string> ops_;
};

// Models: the "Load balancer" box of System Design Interview (Alex Xu, vol. 1) ch. 1 —
// the single public entry that spreads requests over a stateless web tier so that servers
// can be added or lose one without users noticing — and the hash ring of ch. 5
// ("Design consistent hashing"), which sends each key to the same server.
// Captured: the balancer is a queue of its own (default 100k/s, 50 us: an L7 proxy hop,
// see simsys.defaults), so it adds its own sojourn; it forwards every op it receives over
// the links that carry it, split by link weight; and its policy (plan 2a refinement 5)
// changes how the replicas BEHIND it queue, which graph::Graph::mark_balanced_targets
// resolves and the engine applies:
//   round_robin / weighted -> independent replicas, each an M/M/c at its share;
//   least_connections      -> one pooled M/M/(c·N) queue (join-the-shortest-queue ideal);
//   consistent_hash        -> the hottest replica carries 1 + 1/sqrt(vnodes) × the average.
// Ignored: health checks and failover (no failures yet), sticky sessions, the L4 vs L7
// difference (both are one hop with a service time here), connection setup (TLS), and
// per-key routing: a consistent-hash balancer here splits by link weight like the others;
// only its imbalance factor reaches the downstream.
class LoadBalancerBehaviour {
 public:
  LoadBalancerBehaviour(const graph::Graph& g, std::size_t self);

  // Every incoming op is split over the routes whose link carries it, at the shares
  // graph::Graph::routes_for gives (link weights renormalised over those links). The
  // balancer itself handles every request, so `served` is the whole inflow (its own queue
  // sees all of it), though it answers none: the response comes from downstream. `dt` is
  // unused: the balancer holds no state between steps (see NodeBehaviour).
  [[nodiscard]] Routed route(const flow::Flow& in, SimTime dt) const;

  // One capacity unit per request, whatever the op: forwarding costs the same for all.
  [[nodiscard]] double cost(std::string_view /*op*/) const { return 1.0; }

  // own sojourn + the share-weighted mixture over the routes carrying `op` of (two link
  // legs + the downstream node's response to `op`). A route whose hop closes a call cycle
  // (graph::Graph::is_async_hop) is a hand-off the caller does not wait for: its branch
  // costs nothing beyond the balancer's own sojourn.
  [[nodiscard]] flow::LatencyEstimate response(std::string_view op, flow::LatencyEstimate own,
                                               ResponseLookup auto&& downstream) const {
    std::vector<Branch> branches;
    for (const Leg& leg : legs_of(op)) {
      if (leg.share <= 0.0) continue;  // see ClientBehaviour::response
      branches.push_back(
          {.share = leg.share,
           .latency = leg.asynchronous ? flow::LatencyEstimate{}
                                       : round_trip(leg.link_leg_s) + downstream(leg.target, op)});
    }
    return own + mixture(branches);
  }

 private:
  // One route of one op, resolved against the graph once at construction.
  struct Leg {
    std::size_t target;
    std::size_t link;
    double share;       // fraction of this op's requests taking this link
    double link_leg_s;  // one-way link latency in seconds
    bool asynchronous;  // closes a call cycle: not waited for, see `response`
  };

  [[nodiscard]] std::span<const Leg> legs_of(std::string_view op) const;

  // Per op that can reach the balancer (graph::Graph::arriving_ops). std::less<> as in
  // ServiceBehaviour: find() takes a string_view.
  std::map<std::string, std::vector<Leg>, std::less<>> legs_by_op_;
};

// Pattern: Strategy — the engine holds one Behaviour per node and dispatches with
// std::visit; a std::variant instead of an abstract base keeps every behaviour a plain
// value (no heap allocation, no virtual calls, copyable). Phase 2 appends alternatives
// (cache, queue, load balancer, ...).
// C++17 std::variant: a closed sum type (a type-safe union that knows which alternative
// it holds) dispatched with std::visit. Closed: adding an alternative means editing this
// line, and every std::visit that does not handle it then fails to compile.
using Behaviour = std::variant<ClientBehaviour, ServiceBehaviour, LoadBalancerBehaviour>;

}  // namespace simsys::behaviours

namespace simsys::behaviours {

// Checked here as well as in the tests: a behaviour that drifts from the engine's
// contract breaks this module's build, not only a test's.
static_assert(NodeBehaviour<ServiceBehaviour>);
static_assert(NodeBehaviour<LoadBalancerBehaviour>);
static_assert(RequestSource<ClientBehaviour>);

namespace detail {

// The outgoing flow towards (target, link), created on first use. std::ranges::find_if:
// the predicate form of the ranges algorithms introduced in ClientBehaviour::response.
[[nodiscard]] flow::Flow& flow_towards(std::vector<Outgoing>& outgoing, std::size_t target,
                                       std::size_t link) {
  const auto found = std::ranges::find_if(
      outgoing, [&](const Outgoing& o) { return o.target == target && o.link == link; });
  if (found != outgoing.end()) return found->flow;
  return outgoing.emplace_back(Outgoing{.target = target, .link = link}).flow;
}

// Sorted by (target, link) so the engine adds flows in the same order on every run.
// C++20 ranges algorithm with a projection: the third argument maps each Outgoing to the
// (target, link) key it is compared by; `{}` is the default comparison (std::ranges::less).
// Replaces a hand-written comparator lambda that repeats the key on both sides.
void sort_by_destination(std::vector<Outgoing>& outgoing) {
  std::ranges::sort(outgoing, {}, [](const Outgoing& o) { return std::pair{o.target, o.link}; });
}

}  // namespace detail

// ---- mixture ----

flow::LatencyEstimate mixture(std::span<const Branch> branches) {
  double mean = 0.0;
  double within = 0.0;  // Σ s_i v_i
  for (const Branch& branch : branches) {
    mean += branch.share * branch.latency.mean_s;
    within += branch.share * branch.latency.variance_s2;
  }
  // A branch that never answers (inf mean) makes the mixture's spread infinite too, but
  // the between term would compute inf − inf = NaN, so state the limit directly. A NaN
  // anywhere in the inputs still comes out as NaN.
  if (std::isinf(mean)) {
    return {.mean_s = mean, .variance_s2 = within + std::numeric_limits<double>::infinity()};
  }
  double between = 0.0;  // Σ s_i (m_i − mean)²
  for (const Branch& branch : branches) {
    const double deviation = branch.latency.mean_s - mean;
    between += branch.share * deviation * deviation;
  }
  return {.mean_s = mean, .variance_s2 = within + between};
}

// ---- ServiceBehaviour ----

// Only `ops` steps: a service has no `then` (loader-enforced, see the class comment).
ServiceBehaviour::ServiceBehaviour(const graph::Graph& g, std::size_t self)
    : self_{self}, cost_by_op_{g.nodes()[self].spec.cost.begin(), g.nodes()[self].spec.cost.end()} {
  for (const auto& [op, steps] : g.nodes()[self].spec.ops) add_hops(g, op, steps);
}

double ServiceBehaviour::cost(std::string_view op) const {
  const auto found = cost_by_op_.find(op);
  return found == cost_by_op_.end() ? 1.0 : found->second;
}

void ServiceBehaviour::add_hops(const graph::Graph& g, const std::string& op,
                                const std::vector<model::Step>& steps) {
  auto& hops = hops_by_op_[op];
  for (const auto& step : steps) {
    // Only unconditional steps: see the class comment for why miss/failure never fire.
    if (step.on != model::StepCondition::always) continue;
    // Graph::build already proved the target exists and a link carries the demand.
    const auto target = *g.index_of(step.to);
    const auto link = *g.demand_link(self_, target);
    hops.push_back(Hop{.op = step.op,
                       .target = target,
                       .link = link,
                       .factor = step.factor,
                       .mode = step.mode,
                       .link_leg_s = seconds(g.links()[link].one_way),
                       .asynchronous = g.is_async_hop(self_, op, target, step.op)});
  }
}

std::span<const ServiceBehaviour::Hop> ServiceBehaviour::hops_of(std::string_view op) const {
  const auto found = hops_by_op_.find(op);
  if (found == hops_by_op_.end()) return {};
  return found->second;
}

Routed ServiceBehaviour::route(const flow::Flow& in, SimTime /*dt*/) const {
  Routed routed;
  for (const auto& [op, rate] : in.entries()) {
    routed.served.add(op, rate);
    // A zero rate forwards nothing: an empty branch must not exist, or the engine would
    // weight its (possibly inf) latency by zero.
    if (rate.per_second <= 0.0) continue;
    for (const Hop& hop : hops_of(op)) {
      detail::flow_towards(routed.outgoing, hop.target, hop.link)
          .add(hop.op, Rate{rate.per_second * hop.factor});
    }
  }
  detail::sort_by_destination(routed.outgoing);
  return routed;
}

// ---- ClientBehaviour ----

ClientBehaviour::ClientBehaviour(const graph::Graph& g, std::size_t self, std::uint64_t seed)
    : seed_{seed} {
  const auto& workload = g.nodes()[self].spec.workload;
  if (!workload) return;  // a client without a workload offers nothing
  curve_ = workload->curve;
  noise_spec_ = workload->noise;
  reset_noise();
  op_shares_ = workload->op_shares;
  std::ranges::sort(op_shares_, {}, &std::pair<std::string, double>::first);
  for (const auto& [op, share] : op_shares_) ops_.push_back(op);
  for (const auto& route : g.client_routes(self)) {
    const auto op_share =
        std::ranges::find(workload->op_shares, route.op, &std::pair<std::string, double>::first);
    legs_.push_back(RouteLeg{.op = route.op,
                             .target = route.target,
                             .link = route.link,
                             .route_share = route.share,
                             .share_of_rate = op_share->second * route.share,
                             .link_leg_s = seconds(g.links()[route.link].one_way)});
  }
}

void ClientBehaviour::tick(SimTime /*now*/, SimTime dt) {
  if (noise_) noise_multiplier_ = noise_->next(dt);
}

void ClientBehaviour::reset_noise() {
  noise_multiplier_ = 1.0;
  if (noise_spec_) noise_.emplace(*noise_spec_, seed_);
}

Rate ClientBehaviour::rate(SimTime now) const {
  return Rate{curve_.rate_at(now).per_second * noise_multiplier_};
}

std::vector<std::pair<std::string, Rate>> ClientBehaviour::offered(SimTime now) const {
  const double total = rate(now).per_second;
  std::vector<std::pair<std::string, Rate>> per_op;
  for (const auto& [op, share] : op_shares_) per_op.emplace_back(op, Rate{total * share});
  return per_op;  // sorted by op, as op_shares_ is
}

Routed ClientBehaviour::offer(SimTime now) const {
  Routed routed;
  const double total = rate(now).per_second;
  for (const RouteLeg& leg : legs_) {
    const double leg_rate = total * leg.share_of_rate;
    if (leg_rate <= 0.0) continue;  // no empty branches (see ServiceBehaviour::route)
    detail::flow_towards(routed.outgoing, leg.target, leg.link).add(leg.op, Rate{leg_rate});
  }
  detail::sort_by_destination(routed.outgoing);
  return routed;
}

// ---- LoadBalancerBehaviour ----

LoadBalancerBehaviour::LoadBalancerBehaviour(const graph::Graph& g, std::size_t self) {
  for (const auto& op : g.arriving_ops(self)) {
    auto& legs = legs_by_op_[op];
    for (const auto& route : g.routes_for(self, op)) {
      legs.push_back(Leg{.target = route.target,
                         .link = route.link,
                         .share = route.share,
                         .link_leg_s = seconds(g.links()[route.link].one_way),
                         .asynchronous = g.is_async_hop(self, op, route.target, op)});
    }
  }
}

// An op the graph did not list as arriving has no legs: none can reach the balancer in a
// graph that Graph::build accepted.
std::span<const LoadBalancerBehaviour::Leg> LoadBalancerBehaviour::legs_of(
    std::string_view op) const {
  const auto found = legs_by_op_.find(op);
  if (found == legs_by_op_.end()) return {};
  return found->second;
}

Routed LoadBalancerBehaviour::route(const flow::Flow& in, SimTime /*dt*/) const {
  Routed routed;
  for (const auto& [op, rate] : in.entries()) {
    routed.served.add(op, rate);
    if (rate.per_second <= 0.0) continue;  // no empty branches (see ServiceBehaviour::route)
    for (const Leg& leg : legs_of(op)) {
      if (leg.share <= 0.0) continue;
      detail::flow_towards(routed.outgoing, leg.target, leg.link)
          .add(op, Rate{rate.per_second * leg.share});
    }
  }
  detail::sort_by_destination(routed.outgoing);
  return routed;
}

}  // namespace simsys::behaviours
