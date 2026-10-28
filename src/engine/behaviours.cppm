// Module `simsys.behaviours`: what each node type does with flow (client: offer load
// along its routes; service: forward step demand, serve the rest; load balancer: pass
// everything on across its links; rate limiter: pass on what it admits, reject the rest;
// cache: answer, and pass to its store what its strategy sends there)
// and how it composes response latency from its own sojourn plus link legs and downstream
// responses.
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
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
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

// What a behaviour does with one step's flow: what it forwards (one Outgoing per (target,
// link), sorted by target then link so runs are deterministic), what it accepted (`served`:
// its input minus what it rejected; a service or a balancer accepts all of it, a rate
// limiter what it admits) and what it refuses (`rejected`: admission control, plan 2a
// refinement 6; only a rate limiter fills it). The node's own load is not here: the engine
// runs the queue BEFORE route and reports it in NodeSample (engine::Simulation::serve).
// Rejected flow is answered at once ("429 Too Many Requests", ch. 4) and never forwarded;
// the engine counts it in the node's `rejected_per_s`, apart from the queue's drops: a
// rejection is the limiter's policy working, a drop a node failing (report::Summary's
// reject_rate and drop_rate). `served` over the node's inflow is the fraction of its
// requests the node passes on, the start of its delivered fraction (see `delivered`).
struct Routed {
  std::vector<Outgoing> outgoing{};
  flow::Flow served{};
  flow::Flow rejected{};
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

// The same shape for the other quantity the backward pass carries: "what fraction of the
// requests of op `o` sent to node `n` come back answered?" (see `delivered` below).
template <class F>
concept FractionLookup =
    std::invocable<F, std::size_t, std::string_view> &&
    std::convertible_to<std::invoke_result_t<F, std::size_t, std::string_view>, double>;

// C++20 requires-expression: lists the operations a type must support, checked at
// compile time (spec §5). This is the engine's whole requirement on a node behaviour,
// written in code instead of in a comment on an abstract base class. The lambda is a
// stand-in ResponseLookup (C++20 allows lambdas in unevaluated operands such as this).
// It checks that the calls compile and return the right types; what they compute is
// the tests' job. `cost(op)` is the capacity units one request of `op` uses (plan 2a
// refinement 4): the engine charges the node's queue in units, not requests.
// `route(in, now, dt)` takes the step's start and length and is called on a non-const
// behaviour (`node`): a stateless node (service, load balancer) ignores both and may declare
// route const, but a stateful one (the rate limiter's bucket or window counters) refills by
// dt, finds its window from `now`, and must change itself. Requiring it on `B&` admits both;
// `cost`, `response` and `delivered` stay const.
// `delivered(op, own, lookup)` is the fraction of the requests of `op` sent to this node
// that come back answered (plan 2a refinement 8): `own` is the share the node itself passes
// on this step (the engine's served / inflow), times what its downstream calls deliver.
template <class B>
concept NodeBehaviour = requires(const B b, B& node, const flow::Flow& in, SimTime now, SimTime dt,
                                 flow::LatencyEstimate own) {
  { node.route(in, now, dt) } -> std::same_as<Routed>;
  { b.cost(std::string_view{}) } -> std::same_as<double>;
  {
    b.response(std::string_view{}, own,
               [](std::size_t, std::string_view) { return flow::LatencyEstimate{}; })
  } -> std::same_as<flow::LatencyEstimate>;
  {
    b.delivered(std::string_view{}, 1.0, [](std::size_t, std::string_view) { return 1.0; })
  } -> std::same_as<double>;
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
  {
    s.delivered(std::string_view{}, [](std::size_t, std::string_view) { return 1.0; })
  } -> std::same_as<double>;
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

// A hop that only a share `fraction` of the requests take (an `on: miss` step after a cache,
// a read_through cache's fetch from its store): with probability f the request pays `hop`,
// otherwise nothing. The two-point mixture {f: hop, 1 − f: 0} (see `mixture`):
//   mean     = f · m
//   variance = f · v + f (1 − f) · m²     (law of total variance: within + between)
// Added to the sequential parts before it, it gives the hit/miss mixture of the whole
// response: own + mixture{h: 0, 1 − h: hop} = mixture{h: own, 1 − h: own + hop}.
// f >= 1 is the hop itself; f <= 0 is nothing (and never asks for a possibly infinite hop).
[[nodiscard]] flow::LatencyEstimate conditional_hop(double fraction, flow::LatencyEstimate hop);

// Delivered fractions (plan 2a refinement 8, see NodeBehaviour): a request is answered only
// if every part of its path answers it, so the fractions of SEQUENTIAL parts multiply (the
// limiter's own admission x the counter check x the api: 0.25 x 1 x 0.9), and so do a
// service's parallel calls (it needs every reply). Over a SPLIT (a client's routes, a load
// balancer's links) a request takes one branch, so the fraction is the share-weighted mean
// Σ s_i d_i. Assumes the parts fail independently of one another, as the fluid's per-node
// fractions do. Where the fraction comes from at each node: a service or balancer passes on
// what its queue drains (drain / demand), a token-family limiter what it admits
// (admitted / offered), a leaky bucket what leaks (drain / demand); engine::Simulation's
// backward pass composes them, one step stale over a feedback hop as latency is.

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
// variability and the queueing wait with it), retries and timeouts, and `on: failure` steps:
// phase 2a models no failures, so those never fire here. A service has no `then` steps either
// (the loader rejects them for this type): everything it sends is per op. Hops that close a
// call cycle are asynchronous hand-offs (see `response`).
// Cache-aside (plan 2a refinement 7; the pattern ch. 1 draws, which the book calls
// read-through: the application reads the cache and fetches a miss itself): a step
// `{to: db, on: miss}` after a step to a cache is taken by the cache's miss fraction 1 − h of
// the requests only (graph::Graph::service_hops resolves that `fraction` once): it forwards
// rate × factor × (1 − h) (the factor counting every missed lookup), costs the response
// (1 − h) × the hop on average plus the spread between hit and miss (conditional_hop), and
// fails a request only when it is taken (`delivered`). The miss steps after one lookup form
// one Block: a miss takes all of them, one after another, so the block is one conditional hop
// over the sum of their legs, and a miss is answered only if every fetch is.
class ServiceBehaviour {
 public:
  ServiceBehaviour(const graph::Graph& g, std::size_t self);

  // Serves every incoming op and forwards rate × factor along each of the op's steps,
  // asynchronous hops included (over a feedback link, the flow arrives a step later).
  // Returns a fresh Routed every call; if profiling shows the per-step allocations
  // matter, the engine can pass a scratch `Routed&` to be cleared and refilled instead.
  // `now` and `dt` are unused: a service holds no state between steps (see NodeBehaviour).
  [[nodiscard]] Routed route(const flow::Flow& in, SimTime now, SimTime dt) const;

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
    for (const Block& block : blocks_of(op)) {
      const auto legs = waited_legs(block, downstream);
      if (!legs) continue;  // every hop closes a call cycle: adds no latency to the caller
      // A block taken by a share of the requests only (the miss steps of a lookup) is the
      // mixture of taking it and not (conditional_hop); the slowest-branch max below treats
      // that mixture as one branch, the same approximation as for any parallel branch.
      const auto leg = conditional_hop(block.fraction, *legs);
      if (block.mode == model::StepMode::sequential) {
        total += leg;
      } else {
        // flow::parallel_max over (max so far, this branch) is the max over all branches,
        // and keeps the approximation's definition in one place.
        slowest_parallel = flow::parallel_max(std::array{slowest_parallel, leg});
      }
    }
    return total + slowest_parallel;
  }

  // own × the delivered fraction of every block the op waits for, sequential or parallel
  // (the rules above `mixture`); asynchronous hops are hand-offs that cannot fail the caller
  // and are skipped. A block taken by a share f of the requests only fails those: a request
  // gets through it with probability (1 − f) + f · Π d_i (a split between "not taken", which
  // always passes, and "taken", which passes only if every hop of the block answers).
  [[nodiscard]] double delivered(std::string_view op, double own,
                                 FractionLookup auto&& downstream) const {
    double fraction = own;
    for (const Block& block : blocks_of(op)) {
      double every_hop_answers = 1.0;
      for (const Hop& hop : block.hops) {
        if (!hop.asynchronous) every_hop_answers *= downstream(hop.target, hop.op);
      }
      fraction *= (1.0 - block.fraction) + block.fraction * every_hop_answers;
    }
    return fraction;
  }

 private:
  // One step of an op, resolved against the graph once at construction.
  struct Hop {
    std::string op;      // the op sent downstream
    std::size_t target;  // node index
    std::size_t link;    // link index the demand travels on
    double factor;       // downstream requests per request that takes the hop (graph::ServiceHop)
    double link_leg_s;   // one-way link latency in seconds
    bool asynchronous;   // closes a call cycle: not waited for, see `response`
  };

  // The hops a request takes together or skips together: one unconditional step (fraction 1),
  // or the miss steps after one lookup (graph::ServiceHop::lookup), which a miss takes all of
  // in turn (graph::Graph refuses a parallel miss step). `fraction` is the share of the
  // requests that take the block, in (0, 1].
  struct Block {
    double fraction;
    model::StepMode mode;
    std::optional<std::size_t> lookup;  // the lookup a miss block follows; empty otherwise
    std::vector<Hop> hops{};  // `{}`: -Wmissing-field-initializers (see model::Step's comment)
  };

  // The block's legs a request waits for, added up (two link legs plus the downstream's
  // response each, one after another); nothing when every hop is an asynchronous hand-off.
  [[nodiscard]] static std::optional<flow::LatencyEstimate> waited_legs(
      const Block& block, ResponseLookup auto&& downstream) {
    std::optional<flow::LatencyEstimate> legs;
    for (const Hop& hop : block.hops) {
      if (hop.asynchronous) continue;
      legs = legs.value_or(flow::LatencyEstimate{}) + round_trip(hop.link_leg_s) +
             downstream(hop.target, hop.op);
    }
    return legs;
  }

  // std::span<const Block> rather than `const std::vector<Block>&`: a span can also say "no
  // blocks" (an empty view) without a static empty vector to point at.
  [[nodiscard]] std::span<const Block> blocks_of(std::string_view op) const;
  void add_hops(const graph::Graph& g, const std::string& op);

  std::size_t self_;
  // std::less<> (C++14 transparent comparator) lets find() take a string_view without
  // building a std::string for every lookup.
  std::map<std::string, std::vector<Block>, std::less<>> blocks_by_op_;
  std::map<std::string, double, std::less<>> cost_by_op_;  // only the ops listed in `cost`
};

// The seed of client `index`'s noise process (spec phase 2a refinement 3): one
// std::mt19937_64 per client, so appending a node (client or not) at the end of the file
// never changes another client's noise. The seed is the node index, so a node inserted
// before a client moves that client's index, and so its noise.
// Formula: (seed × 0x9E3779B97F4A7C15) XOR index, all mod 2^64. The constant is 2^64
// divided by the golden ratio (splitmix64's increment); what matters is that it is odd and
// large. The multiply is there to keep (seed, index) pairs apart, not to decorrelate
// neighbouring seeds (std::mt19937_64's own seeding already does that): a plain
// seed XOR index collides, 1^0 == 0^1, while seed × K moves each seed's block of indices
// far into the high bits, so for the seeds and client counts a scenario uses no two pairs
// share an engine seed (tests/unit/test_behaviours.cpp checks seeds -100..100 × 64
// clients). A negative seed converts to std::uint64_t modulo 2^64, so it is as deterministic
// as a positive one: signed-to-unsigned conversion has always been defined that way; what
// C++20's two's-complement rule fixed is the other direction (unsigned to signed). The engine
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

  // The fraction of the client's requests of `op` that come back answered: over the routes
  // that carry it, Σ route share × the entry node's delivered fraction (a split, see the
  // rules above `mixture`). Same contract as `response` for an op the client does not send.
  [[nodiscard]] double delivered(std::string_view op, FractionLookup auto&& downstream) const {
    if (!std::ranges::binary_search(ops_, op)) {
      throw std::invalid_argument{"ClientBehaviour::delivered: the client does not send op '" +
                                  std::string{op} + "'"};
    }
    double fraction = 0.0;
    for (const RouteLeg& leg : legs_) {
      if (leg.op != op || leg.route_share <= 0.0) continue;
      fraction += leg.route_share * downstream(leg.target, leg.op);
    }
    return fraction;
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

}  // namespace simsys::behaviours

// Not exported: an implementation piece of the two forwarding behaviours below. It has
// module linkage (a named namespace, not an anonymous one: GCC 14 rejects a member of
// internal-linkage type in an exported class of a module interface).
namespace simsys::behaviours::detail {

// How a node that forwards by LINK (a load balancer, a rate limiter) passes each op on:
// over every route whose link carries it, at the shares graph::Graph::routes_for gives
// (link weights renormalised over those links), and how the response of such a node mixes
// its routes. Pattern: Delegation (composition over inheritance) — LoadBalancerBehaviour and
// RateLimiterBehaviour each hold one Forwarding instead of repeating the per-op legs, or sharing
// them through a base class.
class Forwarding {
 public:
  Forwarding(const graph::Graph& g, std::size_t self);

  // Adds `rate` of `op` to `outgoing`, split over the op's routes.
  void forward(std::string_view op, Rate rate, std::vector<Outgoing>& outgoing) const;

  // The share-weighted mixture (see `mixture`) over the routes carrying `op` of (two link
  // legs + the downstream node's response to `op`). A route whose hop closes a call cycle
  // (graph::Graph::is_async_hop) is a hand-off the caller does not wait for: its branch
  // costs nothing. No routes (a terminal limiter): zero, the node answers itself.
  [[nodiscard]] flow::LatencyEstimate response(std::string_view op,
                                               ResponseLookup auto&& downstream) const {
    std::vector<Branch> branches;
    for (const Leg& leg : legs_of(op)) {
      if (leg.share <= 0.0) continue;  // see ClientBehaviour::response
      branches.push_back(
          {.share = leg.share,
           .latency = leg.asynchronous ? flow::LatencyEstimate{}
                                       : round_trip(leg.link_leg_s) + downstream(leg.target, op)});
    }
    return mixture(branches);
  }

  // Σ share × the downstream node's delivered fraction over the routes carrying `op` (a
  // split, see the rules above `mixture`); an asynchronous route is a hand-off that cannot
  // fail the caller (1). No route with a share (a terminal limiter): 1, the node answers
  // every request it passes itself.
  [[nodiscard]] double delivered(std::string_view op, FractionLookup auto&& downstream) const {
    double fraction = 0.0;
    double shares = 0.0;
    for (const Leg& leg : legs_of(op)) {
      if (leg.share <= 0.0) continue;
      shares += leg.share;
      fraction += leg.share * (leg.asynchronous ? 1.0 : downstream(leg.target, op));
    }
    return shares > 0.0 ? fraction : 1.0;
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

  // Per op that can reach the node (graph::Graph::arriving_ops). std::less<> as in
  // ServiceBehaviour: find() takes a string_view.
  std::map<std::string, std::vector<Leg>, std::less<>> legs_by_op_;
};

}  // namespace simsys::behaviours::detail

export namespace simsys::behaviours {

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
//   consistent_hash        -> the hottest replica carries 1 + 1/sqrt(vnodes) × the average
//                             and is queued apart from the N - 1 cooler ones.
// Ignored: health checks and failover (no failures yet), sticky sessions, the L4 vs L7
// difference (both are one hop with a service time here), connection setup (TLS), and
// per-key routing: a consistent-hash balancer here splits by link weight like the others;
// only its imbalance factor reaches the downstream (graph::Graph::mark_balanced_targets
// explains the factor, engine::Simulation::make_server the hot and cool copies).
class LoadBalancerBehaviour {
 public:
  LoadBalancerBehaviour(const graph::Graph& g, std::size_t self);

  // Every incoming op is split over the routes whose link carries it, at the shares
  // graph::Graph::routes_for gives (link weights renormalised over those links). A balancer
  // rejects nothing, so `served`, the flow it accepted, is its whole input. `now` and `dt`
  // are unused: the balancer holds no state between steps (see NodeBehaviour).
  [[nodiscard]] Routed route(const flow::Flow& in, SimTime now, SimTime dt) const;

  // One capacity unit per request, whatever the op: forwarding costs the same for all.
  [[nodiscard]] double cost(std::string_view /*op*/) const { return 1.0; }

  // own sojourn + the share-weighted mixture over the routes carrying `op` of (two link
  // legs + the downstream node's response to `op`). A route whose hop closes a call cycle
  // (graph::Graph::is_async_hop) is a hand-off the caller does not wait for: its branch
  // costs nothing beyond the balancer's own sojourn.
  [[nodiscard]] flow::LatencyEstimate response(std::string_view op, flow::LatencyEstimate own,
                                               ResponseLookup auto&& downstream) const {
    return own + forwarding_.response(op, downstream);
  }

  // own × the share-weighted mixture of the routes' delivered fractions.
  [[nodiscard]] double delivered(std::string_view op, double own,
                                 FractionLookup auto&& downstream) const {
    return own * forwarding_.delivered(op, downstream);
  }

 private:
  detail::Forwarding forwarding_;
};

// Models: the "Rate limiter" of System Design Interview (Alex Xu, vol. 1) ch. 4 — a
// middleware in front of the API servers that counts each client's requests in a fast
// store (Redis in the book) and answers "429 Too Many Requests" once a client is over its
// limit. Captured (plan 2a refinement 6): the limiter is a queue of its own (default
// 100k/s, 50 us: an in-memory counter check, see simsys.defaults) that sees EVERY request,
// admitted or not; what it admits goes on over its links like a load balancer's flow; what
// it refuses is `rejected`, answered at once and never forwarded; with a `backend` (the
// counter store) every request it CHECKS, admitted or refused, first reads its counter
// there (one `counter` request each), which adds that hop to the response. Each of the five
// algorithms of ch. 4 keeps its own state, stepped once per step of length dt that starts at
// `now`. With W = limit × window (requests a window may hold) and `in` the inflow (req/s):
//  - token_bucket: a bucket of at most `burst` tokens, refilled at `limit` per second, one
//    token per request. Over a step the bucket holds L tokens and receives limit·dt more:
//      admitted = min(in, limit + L/dt)              (requests per second)
//      L       <- clamp(L + (limit − admitted)·dt, 0, burst)
//    Below the limit everything passes and the bucket fills; a burst above it passes until
//    the bucket is empty; then exactly `limit` passes.
//  - fixed_window: a counter `used` per calendar window [k·window, (k+1)·window), reset when
//    floor(now / window) changes. admitted = min(in·dt, W − used) / dt. Under a flood the
//    first step of each window takes all of W and the rest get nothing; a flood that starts
//    late in one window gets W at its end AND W at the next one's start: ch. 4's "twice the
//    limit across a boundary".
//  - sliding_log: ch. 4 keeps a timestamp per admitted request and counts the last `window`
//    seconds of them. The fluid keeps a ring of the admitted counts of the last
//    n = ceil(window / dt) steps (this one included): admitted = min(in·dt, W − Σ previous
//    n − 1 steps) / dt, so NO n consecutive steps ever hold more than W. Memory: n numbers,
//    where the book's log holds one timestamp per request.
//  - sliding_counter: the current window's count `cur` plus the previous window's `prev`,
//    weighted by how much of it the trailing window still overlaps:
//      estimate = prev · (1 − f) + cur,   f = (now mod window) / window
//    and admitted = min(in·dt, W − estimate) / dt. The book's approximation of the log in
//    two numbers: it assumes the previous window's requests were spread evenly. Under a
//    flood it admits less than the limit (10/11 of it at ten steps per window), because the
//    previous window's full count keeps the estimate high early in each window.
//  - leaky_bucket: a FIFO bucket that leaks one request every 1/limit with room for `burst`;
//    excess waits instead of being rejected, and a full bucket refuses. That is exactly the
//    node's own FluidQueue as one deterministic leak slot (graph::leaky_bucket_capacity), so
//    this behaviour admits everything the queue lets through, and the queue's overflow is
//    the bucket's refusal, which the engine reports as `rejected`, not dropped
//    (refuses_on_overflow).
// What the fluid step cannot see: anything inside a step. A window boundary inside a step
// would be seen only at the next step's start, so the graph refuses a windowed limiter whose
// window is not a whole number of steps (graph::window_off_the_step_grid); the requests of
// one step arrive as one lump; and the bursts the book draws (100 requests in one
// millisecond) are spread over the step.
// Ignored: per-client keys (one aggregate limit for all clients: the book's per-user,
// per-IP or per-API rules would need flows per key), the race conditions and
// synchronisation of distributed counters (ch. 4's Lua scripts and sorted sets), the
// rate-limit headers a client could back off on (rejected requests are not retried), and
// soft limits. Several replicas share ONE limit (the counters are central), so the bucket
// or window state is the group's, not one per copy.
// A limiter with no outgoing link but its backend is a terminal gate (notification-system's
// per-user `user_limiter`): it answers every request it checks itself, so its response is
// its own sojourn plus the counter check. Its CALLER keeps calling its own later steps at
// the full rate, whatever the gate refused: a service's steps fire per request it serves,
// and a fluid has no per-request outcome to branch on. The refused share shows in the
// gate's reject rate and, through `delivered`, in the answered rate the client sees.
class RateLimiterBehaviour {
 public:
  RateLimiterBehaviour(const graph::Graph& g, std::size_t self);

  // Admits per the algorithm (the class comment; all of it for a leaky bucket), the same
  // fraction of every op; forwards the admitted flow over the routes that carry each op
  // (not the backend link); sends one `counter` request per CHECKED request (its whole
  // inflow) to the backend; rejects the rest. Changes the limiter's state by the step that
  // starts at `now` and lasts `dt`, so it is not const (see NodeBehaviour). Throws
  // std::invalid_argument for dt <= 0 (the bucket would divide by it; a programmer error,
  // as in fluid::FluidQueue::advance).
  [[nodiscard]] Routed route(const flow::Flow& in, SimTime now, SimTime dt);

  // One capacity unit per request, admitted or rejected: the check costs the same.
  [[nodiscard]] double cost(std::string_view /*op*/) const { return 1.0; }

  // own sojourn + the counter store hop (two link legs + the backend's `counter` response)
  // + the downstream mixture as for a load balancer. The counter check happens BEFORE the
  // request is passed on, so the two hops are sequential and add. This is the response of
  // an ADMITTED request. A rejected request also waited for the counter check (own + the
  // counter hop, then the 429), but it is not answered, so it never reaches the client's
  // latency figures: plan 2a refinement 8 weighs them by the answered requests (`delivered`).
  [[nodiscard]] flow::LatencyEstimate response(std::string_view op, flow::LatencyEstimate own,
                                               ResponseLookup auto&& downstream) const {
    flow::LatencyEstimate total = own;
    if (backend_) {
      total += round_trip(backend_->link_leg_s) + downstream(backend_->target, graph::counter_op);
    }
    return total + forwarding_.response(op, downstream);
  }

  // own (the admitted share: admitted / offered, or for a leaky bucket drain / demand) × the
  // counter check's delivered fraction (a lost check answers nothing) × the routes' mixture.
  [[nodiscard]] double delivered(std::string_view op, double own,
                                 FractionLookup auto&& downstream) const {
    const double check = backend_ ? downstream(backend_->target, graph::counter_op) : 1.0;
    return own * check * forwarding_.delivered(op, downstream);
  }

  // True for a leaky bucket: its queue's overflow is the policy refusing ("bucket full"),
  // which the engine reports as rejected rather than dropped.
  [[nodiscard]] bool refuses_on_overflow() const {
    return algorithm_ == model::LimiterAlgorithm::leaky_bucket;
  }

  // Switches to the long-run admission rule for engine::Simulation::estimate, whose settle
  // passes all start at t = 0 and would otherwise never leave the first window (or would
  // keep spending a full bucket's burst): every non-leaky algorithm then admits
  // min(in, limit), the rate each lets through on average under a steady inflow (a token
  // bucket's refill; a window's W per window). Ignored by it: the sliding counter's
  // shortfall under a flood (10/11 of the limit at ten steps per window), a step-level
  // effect only `run` shows.
  void use_long_run_rate() { long_run_ = true; }

 private:
  // The counter store hop, resolved once. Always waited for: the backend is a cache, whose
  // `counter` op makes no further calls, so the hop can close no call cycle.
  struct BackendHop {
    std::size_t target;
    std::size_t link;
    double link_leg_s;
  };

  // The admitted rate of `offered` requests per second over the step [now, now + dt), and
  // the algorithm's state after it (the formula blocks in the class comment).
  // Pattern: Strategy (a switch over a closed enum) — one admit_* per algorithm.
  [[nodiscard]] double admit(double offered_per_s, SimTime now, SimTime dt);
  [[nodiscard]] double admit_token_bucket(double offered_per_s, double dt_s);
  [[nodiscard]] double admit_fixed_window(double offered_per_s, SimTime now, double dt_s);
  [[nodiscard]] double admit_sliding_log(double offered_per_s, SimTime dt);
  [[nodiscard]] double admit_sliding_counter(double offered_per_s, SimTime now, double dt_s);
  // Moves the window counters to the window containing `now`.
  void enter_window_of(SimTime now);

  detail::Forwarding forwarding_;
  std::optional<BackendHop> backend_{};
  model::LimiterAlgorithm algorithm_;
  double limit_per_s_;
  bool long_run_{false};
  // Token bucket. Starts FULL (= burst): a limiter that has been idle holds a full bucket,
  // which is how a run's first seconds would meet a real one. The cost is a first burst of
  // up to `burst` extra requests at t = 0; a scenario's `warmup` keeps it out of the summary.
  double burst_;
  double tokens_;
  // Windowed algorithms: W and the window, and the counters of the window `now` is in.
  double per_window_;
  SimTime window_;
  long long window_index_{0};  // floor(now / window) of the current window; t = 0 is window 0
  double used_{};      // admitted in the current window (fixed window: `used`; counter: `cur`)
  double previous_{};  // admitted in the previous window (sliding counter: `prev`)
  // Sliding log: admitted per step over the last n steps, a ring whose head is the oldest.
  std::vector<double> log_{};
  std::size_t log_head_{0};
  double log_sum_{0.0};
};

// Models: the "Cache" tier of System Design Interview (Alex Xu, vol. 1) ch. 1 ("Scale from
// zero to millions of users": a memory store in front of the database for data read often
// and changed rarely) and its uses in ch. 8 (URL shortener: <shortURL, longURL> in a
// cache), ch. 11 (news feed caches), ch. 12 (chat), ch. 13 (autocomplete) and ch. 14
// (YouTube metadata). Plan 2a refinement 7. Captured: the cache is a queue of its own
// (default 100k/s, 250 µs: an in-memory lookup, the latency table's 1 MB memory read, see
// simsys.defaults) that answers every
// request sent to it, hits and misses alike, at its `cost` per op; a FIXED hit ratio h, the
// share of reads it answers from memory; which ops are writes (`write_ops`, every other op
// is a read); and the strategy, i.e. who talks to the store. Names: ch. 1 draws one pattern,
// the application reading the cache and fetching a miss itself, and calls it "read-through";
// the industry's name for it is cache-aside, which is ours. Our `read_through` is the
// cache-driven fetch, and the write strategies are general caching practice, not the book's:
//   cache_aside   -> the application. The cache serves everything and forwards nothing; the
//                    service's `{to: db, on: miss}` step carries the misses (ServiceBehaviour).
//   read_through  -> the cache. It forwards 1 − h of each read to its `backend` as the same op
//                    and answers a miss after the fetch: response = the mixture
//                    {h: own, 1 − h: own + two link legs + the backend's response}.
//   write_through -> every write goes on to the backend, synchronously: own + the hop.
//   write_around  -> the same flow as write_through: writes skip the cache and go to the
//                    store. What differs is what the cache holds afterwards (fewer entries
//                    nobody reads again), which shows in the hit ratio, and h is fixed here.
//   write_back    -> phase 2b (the engine refuses it): writes acknowledged by the cache and
//                    flushed later need a queue of dirty entries.
// Under read_through a write is served by the cache alone (the application writes the store
// itself, as in cache_aside); under the write strategies a read miss is the application's to
// fetch (a service `on: miss` step), as in cache_aside. Which share of each op goes to the
// store is graph::Graph::backend_fraction, the one rule the paper load and the call-cycle
// search share with this behaviour.
// Pattern: Strategy (enum-dispatched) — the strategy is a closed enum, not a class per
// strategy: graph::Graph::backend_fraction switches on it once per op at construction, and
// this behaviour only reads the resulting shares.
// Ignored: which keys are popular (h is a number, not a function of the cache size and the
// key distribution: the Che approximation arrives in 2b), entries expiring by TTL and the
// eviction order (`ttl`, `eviction` are read and printed only), a cold cache filling up
// after a start or a flush, the thundering herd of misses when a hot key expires (every
// miss is an independent request to the store), and invalidation traffic.
class CacheBehaviour {
 public:
  CacheBehaviour(const graph::Graph& g, std::size_t self);

  // Serves every incoming op (`served` is the whole input: a miss is answered too, after
  // the fetch) and forwards rate × backend share of each op to the backend. `now` and `dt`
  // are unused: with a fixed hit ratio the cache holds no state between steps.
  [[nodiscard]] Routed route(const flow::Flow& in, SimTime now, SimTime dt) const;

  // Capacity units one request of `op` consumes: the spec's `cost` entry, else 1 (a hit and
  // a miss cost the cache the same lookup; the miss's fetch is charged to the backend).
  [[nodiscard]] double cost(std::string_view op) const;

  // own sojourn + the backend hop for the share that takes it (conditional_hop): nothing
  // under cache_aside, (1 − h) × the hop for a read_through read, the whole hop for a
  // write_through or write_around write.
  [[nodiscard]] flow::LatencyEstimate response(std::string_view op, flow::LatencyEstimate own,
                                               ResponseLookup auto&& downstream) const {
    const double share = backend_share(op);
    if (share <= 0.0) return own;
    return own + conditional_hop(
                     share, round_trip(backend_->link_leg_s) + downstream(backend_->target, op));
  }

  // own × ((1 − f) + f × the backend's delivered fraction), f the share that goes to the
  // backend: a hit is answered by the cache alone, a forwarded request only if the store
  // answers it ("own × (h + (1 − h) · backend)" for a read_through read).
  [[nodiscard]] double delivered(std::string_view op, double own,
                                 FractionLookup auto&& downstream) const {
    const double share = backend_share(op);
    if (share <= 0.0) return own;
    return own * ((1.0 - share) + share * downstream(backend_->target, op));
  }

 private:
  // The store hop, resolved once; absent for cache_aside (no backend edge). Always waited
  // for, like a limiter's counter check: a store makes no call back into its cache, so the
  // hop closes no call cycle in any design the book draws.
  struct BackendHop {
    std::size_t target;
    std::size_t link;
    double link_leg_s;
  };

  // graph::Graph::backend_fraction of `op`, taken once per op that can reach the cache
  // (graph::Graph::arriving_ops); 0 for any other op, which a built graph never sends here.
  [[nodiscard]] double backend_share(std::string_view op) const;

  std::optional<BackendHop> backend_{};
  std::map<std::string, double, std::less<>> share_by_op_;  // std::less<>: find by string_view
  std::map<std::string, double, std::less<>> cost_by_op_;   // only the ops listed in `cost`
};

// Pattern: Strategy — the engine holds one Behaviour per node and dispatches with
// std::visit; a std::variant instead of an abstract base keeps every behaviour a plain
// value (no heap allocation, no virtual calls, copyable). Phase 2a appended the load
// balancer, the rate limiter and the cache; later phases append the rest (queue, ...).
// C++17 std::variant: a closed sum type (a type-safe union that knows which alternative
// it holds) dispatched with std::visit. Closed: adding an alternative means editing this
// line, and every std::visit that does not handle it then fails to compile.
using Behaviour = std::variant<ClientBehaviour, ServiceBehaviour, LoadBalancerBehaviour,
                               RateLimiterBehaviour, CacheBehaviour>;

}  // namespace simsys::behaviours

namespace simsys::behaviours {

// Checked here as well as in the tests: a behaviour that drifts from the engine's
// contract breaks this module's build, not only a test's.
static_assert(NodeBehaviour<ServiceBehaviour>);
static_assert(NodeBehaviour<LoadBalancerBehaviour>);
static_assert(NodeBehaviour<RateLimiterBehaviour>);
static_assert(NodeBehaviour<CacheBehaviour>);
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

flow::LatencyEstimate conditional_hop(double fraction, flow::LatencyEstimate hop) {
  if (fraction >= 1.0) return hop;
  if (fraction <= 0.0) return {};
  // C++20 std::to_array: the two branches as a std::array, its size deduced.
  return mixture(std::to_array<Branch>(
      {{.share = fraction, .latency = hop}, {.share = 1.0 - fraction, .latency = {}}}));
}

// ---- ServiceBehaviour ----

// Only `ops` steps: a service has no `then` (loader-enforced, see the class comment).
ServiceBehaviour::ServiceBehaviour(const graph::Graph& g, std::size_t self)
    : self_{self}, cost_by_op_{g.nodes()[self].spec.cost.begin(), g.nodes()[self].spec.cost.end()} {
  // C++20 std::views::keys: a lazy view of each map entry's key (the op name), replacing a
  // loop over the pairs that ignores `.second`; nothing is copied.
  for (const auto& op : g.nodes()[self].spec.ops | std::views::keys) add_hops(g, op);
}

double ServiceBehaviour::cost(std::string_view op) const {
  const auto found = cost_by_op_.find(op);
  return found == cost_by_op_.end() ? 1.0 : found->second;
}

// The graph's resolved hops (graph::Graph::service_hops), minus those no request takes: an
// `on: failure` step (no failures before phase 2c) and an `on: miss` step after a cache
// that always hits. Such a hop forwards nothing and adds nothing, so it is left out rather
// than carried as a zero branch. Each unconditional hop opens a block of its own; a miss hop
// joins the block of the miss hops of the same lookup before it (Block).
void ServiceBehaviour::add_hops(const graph::Graph& g, const std::string& op) {
  auto& blocks = blocks_by_op_[op];
  for (const graph::ServiceHop& resolved : g.service_hops(self_, op)) {
    if (resolved.fraction <= 0.0) continue;
    const bool same_miss_block =
        resolved.lookup && !blocks.empty() && blocks.back().lookup == resolved.lookup;
    if (!same_miss_block) {
      blocks.push_back(
          Block{.fraction = resolved.fraction, .mode = resolved.mode, .lookup = resolved.lookup});
    }
    // Graph::build already proved a link carries the demand.
    const auto link = *g.demand_link(self_, resolved.target);
    blocks.back().hops.push_back(
        Hop{.op = resolved.op,
            .target = resolved.target,
            .link = link,
            .factor = resolved.factor,
            .link_leg_s = seconds(g.links()[link].one_way),
            .asynchronous = g.is_async_hop(self_, op, resolved.target, resolved.op)});
  }
}

std::span<const ServiceBehaviour::Block> ServiceBehaviour::blocks_of(std::string_view op) const {
  const auto found = blocks_by_op_.find(op);
  if (found == blocks_by_op_.end()) return {};
  return found->second;
}

// Each hop forwards rate × factor × the share of requests that take its block.
Routed ServiceBehaviour::route(const flow::Flow& in, SimTime /*now*/, SimTime /*dt*/) const {
  Routed routed;
  for (const auto& [op, rate] : in.entries()) {
    routed.served.add(op, rate);
    // A zero rate forwards nothing: an empty branch must not exist, or the engine would
    // weight its (possibly inf) latency by zero.
    if (rate.per_second <= 0.0) continue;
    for (const Block& block : blocks_of(op)) {
      for (const Hop& hop : block.hops) {
        detail::flow_towards(routed.outgoing, hop.target, hop.link)
            .add(hop.op, Rate{rate.per_second * hop.factor * block.fraction});
      }
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

// ---- detail::Forwarding ----

detail::Forwarding::Forwarding(const graph::Graph& g, std::size_t self) {
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

// An op the graph did not list as arriving has no legs: none can reach the node in a
// graph that Graph::build accepted.
std::span<const detail::Forwarding::Leg> detail::Forwarding::legs_of(std::string_view op) const {
  const auto found = legs_by_op_.find(op);
  if (found == legs_by_op_.end()) return {};
  return found->second;
}

void detail::Forwarding::forward(std::string_view op, Rate rate,
                                 std::vector<Outgoing>& outgoing) const {
  if (rate.per_second <= 0.0) return;  // no empty branches (see ServiceBehaviour::route)
  for (const Leg& leg : legs_of(op)) {
    if (leg.share <= 0.0) continue;
    detail::flow_towards(outgoing, leg.target, leg.link).add(op, Rate{rate.per_second * leg.share});
  }
}

// ---- LoadBalancerBehaviour ----

LoadBalancerBehaviour::LoadBalancerBehaviour(const graph::Graph& g, std::size_t self)
    : forwarding_{g, self} {}

Routed LoadBalancerBehaviour::route(const flow::Flow& in, SimTime /*now*/, SimTime /*dt*/) const {
  Routed routed;
  for (const auto& [op, rate] : in.entries()) {
    routed.served.add(op, rate);
    forwarding_.forward(op, rate, routed.outgoing);
  }
  detail::sort_by_destination(routed.outgoing);
  return routed;
}

// ---- RateLimiterBehaviour ----

// Graph::build proved the limit is there and positive, and resolved the backend's link.
RateLimiterBehaviour::RateLimiterBehaviour(const graph::Graph& g, std::size_t self)
    : forwarding_{g, self},
      algorithm_{g.nodes()[self].spec.algorithm.value_or(model::LimiterAlgorithm::token_bucket)},
      limit_per_s_{g.nodes()[self].spec.limit->per_second},
      burst_{model::limiter_burst(g.nodes()[self].spec)},
      tokens_{burst_},
      window_{model::limiter_window(g.nodes()[self].spec)} {
  per_window_ = limit_per_s_ * seconds(window_);  // W = limit × window, in requests
  const auto& resolved = g.nodes()[self];
  if (resolved.backend) {
    backend_ = BackendHop{.target = *resolved.backend,
                          .link = *resolved.backend_link,
                          .link_leg_s = seconds(g.links()[*resolved.backend_link].one_way)};
  }
}

// A leaky bucket admits all: its queue (the node's FluidQueue) already held back what
// exceeds the leak rate, and refused what overflowed. In the long-run mode of estimate
// every other algorithm admits min(in, limit) (see use_long_run_rate).
double RateLimiterBehaviour::admit(double offered_per_s, SimTime now, SimTime dt) {
  // C++20 `using enum`: the algorithm names in scope for this function only, so the switch
  // reads `case token_bucket:` instead of `case model::LimiterAlgorithm::token_bucket:`.
  using enum model::LimiterAlgorithm;
  if (algorithm_ == leaky_bucket) return offered_per_s;
  if (long_run_) return std::min(offered_per_s, limit_per_s_);
  const double dt_s = seconds(dt);
  switch (algorithm_) {
    case token_bucket:
      return admit_token_bucket(offered_per_s, dt_s);
    case fixed_window:
      return admit_fixed_window(offered_per_s, now, dt_s);
    case sliding_log:
      return admit_sliding_log(offered_per_s, dt);
    case sliding_counter:
      return admit_sliding_counter(offered_per_s, now, dt_s);
    case leaky_bucket:
      break;  // handled above
  }
  return offered_per_s;
}

// The tokens are requests: limit_per_s_ x dt_s new ones arrive this step, admitted x dt_s
// leave (the formula block in the class comment).
double RateLimiterBehaviour::admit_token_bucket(double offered_per_s, double dt_s) {
  const double admitted = std::min(offered_per_s, limit_per_s_ + tokens_ / dt_s);
  tokens_ = std::clamp(tokens_ + (limit_per_s_ - admitted) * dt_s, 0.0, burst_);
  return admitted;
}

// Window index = floor(now / window), in whole nanoseconds, so a boundary is exact. Moving on
// by one window makes the current count the previous one; skipping a whole window (an idle
// window admits nothing) leaves a previous count of zero.
void RateLimiterBehaviour::enter_window_of(SimTime now) {
  const long long index = now.count() / window_.count();
  if (index == window_index_) return;
  previous_ = index == window_index_ + 1 ? used_ : 0.0;
  used_ = 0.0;
  window_index_ = index;
}

// What is left of W in the current window, spread over this step.
double RateLimiterBehaviour::admit_fixed_window(double offered_per_s, SimTime now, double dt_s) {
  enter_window_of(now);
  const double admitted = std::min(offered_per_s, std::max(per_window_ - used_, 0.0) / dt_s);
  used_ += admitted * dt_s;
  return admitted;
}

// The ring holds the admitted counts of the last n steps, oldest at the head. The head's
// step leaves the window as this one enters it, so `recent` is the n − 1 steps before this
// one. The running sum is recomputed exactly whenever the head wraps round, so rounding
// cannot creep in over a long run (amortised O(1) per step). A dt that changes n (never in
// a run: the step is the scenario's) starts a fresh, empty log.
double RateLimiterBehaviour::admit_sliding_log(double offered_per_s, SimTime dt) {
  // n = ceil(window / dt) in whole nanoseconds: the steps one window spans.
  const auto steps = static_cast<std::size_t>((window_.count() + dt.count() - 1) / dt.count());
  if (log_.size() != steps) {
    log_.assign(steps, 0.0);
    log_head_ = 0;
    log_sum_ = 0.0;
  }
  const double dt_s = seconds(dt);
  const double recent = std::max(log_sum_ - log_[log_head_], 0.0);
  const double admitted = std::min(offered_per_s, std::max(per_window_ - recent, 0.0) / dt_s);
  log_[log_head_] = admitted * dt_s;
  log_sum_ = recent + log_[log_head_];
  log_head_ = (log_head_ + 1) % log_.size();
  // C++23 std::ranges::fold_left: a left fold over the whole ring, replacing std::accumulate
  // (iterator pair, and an initial value whose type silently becomes the sum's: 0 for 0.0).
  if (log_head_ == 0) log_sum_ = std::ranges::fold_left(log_, 0.0, std::plus<>{});
  return admitted;
}

// The estimate of ch. 4's sliding window counter, at the step's start (class comment).
double RateLimiterBehaviour::admit_sliding_counter(double offered_per_s, SimTime now, double dt_s) {
  enter_window_of(now);
  const double elapsed = static_cast<double>(now.count() % window_.count()) /
                         static_cast<double>(window_.count());  // f in [0, 1)
  const double estimate = previous_ * (1.0 - elapsed) + used_;
  const double admitted = std::min(offered_per_s, std::max(per_window_ - estimate, 0.0) / dt_s);
  used_ += admitted * dt_s;
  return admitted;
}

// Every op keeps the same admitted fraction: the limiter counts requests, not ops, so
// under a limit the op mix that gets through is the mix that arrived (and so is the
// rejected part). Zero rates are skipped, so an empty or idle inflow yields empty flows.
Routed RateLimiterBehaviour::route(const flow::Flow& in, SimTime now, SimTime dt) {
  if (dt.count() <= 0) {
    throw std::invalid_argument{"RateLimiterBehaviour::route: dt must be > 0"};
  }
  Routed routed;
  const double offered = in.total().per_second;
  const double admitted = admit(offered, now, dt);
  // Exactly 1 when everything passes, so nothing is "rejected" by rounding.
  const double fraction = admitted >= offered ? 1.0 : admitted / offered;
  for (const auto& [op, rate] : in.entries()) {
    if (rate.per_second <= 0.0) continue;
    const double passed = rate.per_second * fraction;
    const double refused = rate.per_second - passed;
    if (passed > 0.0) {
      routed.served.add(op, Rate{passed});
      forwarding_.forward(op, Rate{passed}, routed.outgoing);
    }
    if (refused > 0.0) routed.rejected.add(op, Rate{refused});
  }
  if (backend_ && offered > 0.0) {
    // One counter check per request the limiter looked at, admitted or refused: reading the
    // counter is how it decides (ch. 4).
    detail::flow_towards(routed.outgoing, backend_->target, backend_->link)
        .add(graph::counter_op, Rate{offered});
  }
  detail::sort_by_destination(routed.outgoing);
  return routed;
}

// ---- CacheBehaviour ----

// Graph::build resolved the backend edge (only for a strategy in which the cache calls the
// store) and checked the fixed hit ratio; Simulation::create refuses a cache without one.
CacheBehaviour::CacheBehaviour(const graph::Graph& g, std::size_t self)
    : cost_by_op_{g.nodes()[self].spec.cost.begin(), g.nodes()[self].spec.cost.end()} {
  const auto& resolved = g.nodes()[self];
  if (!resolved.backend) return;
  backend_ = BackendHop{.target = *resolved.backend,
                        .link = *resolved.backend_link,
                        .link_leg_s = seconds(g.links()[*resolved.backend_link].one_way)};
  for (const auto& op : g.arriving_ops(self))
    share_by_op_.emplace(op, g.backend_fraction(self, op));
}

double CacheBehaviour::cost(std::string_view op) const {
  const auto found = cost_by_op_.find(op);
  return found == cost_by_op_.end() ? 1.0 : found->second;
}

double CacheBehaviour::backend_share(std::string_view op) const {
  const auto found = share_by_op_.find(op);
  return found == share_by_op_.end() ? 0.0 : found->second;
}

Routed CacheBehaviour::route(const flow::Flow& in, SimTime /*now*/, SimTime /*dt*/) const {
  Routed routed;
  for (const auto& [op, rate] : in.entries()) {
    if (rate.per_second <= 0.0) continue;  // no empty branches (see ServiceBehaviour::route)
    routed.served.add(op, rate);
    const double share = backend_share(op);
    if (share <= 0.0) continue;
    detail::flow_towards(routed.outgoing, backend_->target, backend_->link)
        .add(op, Rate{rate.per_second * share});
  }
  detail::sort_by_destination(routed.outgoing);
  return routed;
}

}  // namespace simsys::behaviours
