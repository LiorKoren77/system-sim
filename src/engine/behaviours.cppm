// Module `simsys.behaviours`: what each node type does with flow (client: offer load
// along its routes; service: forward step demand, serve the rest) and how it composes
// response latency from its own sojourn plus link legs and downstream responses.
//
// Latency composes on the reply path. A hop to a downstream node costs two link legs
// (request out, reply back) plus that node's own response; sequential hops add, parallel
// hops cost the slowest branch, and a client op split over several links is a
// share-weighted mixture of its routes. A hop over a feedback link (one that closes a
// cycle, graph::Graph::mark_feedback) is asynchronous: its flow goes round, a step later,
// but the sender does not wait for it, so it adds nothing to the response.
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
#include <limits>
#include <map>
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
// the tests' job.
template <class B>
concept NodeBehaviour = requires(const B b, const flow::Flow& in, flow::LatencyEstimate own) {
  { b.route(in) } -> std::same_as<Routed>;
  {
    b.response(std::string_view{}, own,
               [](std::size_t, std::string_view) { return flow::LatencyEstimate{}; })
  } -> std::same_as<flow::LatencyEstimate>;
};

// A source of requests: it offers load at simulated time `now` instead of routing an
// incoming flow, lists the ops it sends (one latency report each), and composes the
// response time it sees for each of them. Signatures only, as for NodeBehaviour.
template <class S>
concept RequestSource = requires(const S s, SimTime now) {
  { s.offer(now) } -> std::same_as<Routed>;
  {
    s.response(std::string_view{},
               [](std::size_t, std::string_view) { return flow::LatencyEstimate{}; })
  } -> std::same_as<flow::LatencyEstimate>;
  { s.ops() } -> std::convertible_to<std::span<const std::string>>;
};

// Models: a `service` box of System Design Interview (Alex Xu, vol. 1) — the stateless
// web/API tier of ch. 1 ("Scale from zero to millions of users"), and every application
// server of the later designs (ch. 11 news feed fan-out service, ch. 12 chat servers).
// Captured: for each incoming op, the downstream calls it makes (`ops` steps), the
// fan-out of each call (`factor`: one post becomes 100 feed writes), and how the call
// latencies compose — sequential calls add, parallel calls wait for the slowest.
// Ignored: per-op CPU cost (every op costs the node's one service time; phase 2 adds a
// cost table), retries and timeouts, and conditional steps (`on: miss` / `on: failure`):
// a service has no cache to miss and phase 1 models no failures, so those steps never
// fire here. A service has no `then` steps either (the loader rejects them for this
// type): everything it sends is per op. Hops over feedback links are asynchronous
// hand-offs (see `response`).
class ServiceBehaviour {
 public:
  ServiceBehaviour(const graph::Graph& g, std::size_t self);

  // Serves every incoming op and forwards rate × factor along each of the op's steps,
  // feedback hops included (their flow arrives at the target a step later).
  // Returns a fresh Routed every call; if profiling shows the per-step allocations
  // matter, the engine can pass a scratch `Routed&` to be cleared and refilled instead.
  [[nodiscard]] Routed route(const flow::Flow& in) const;

  // Response time of `op`: own sojourn + Σ sequential hops + slowest parallel hop, where
  // a hop is two link legs plus the downstream node's response. Feedback hops are
  // skipped: closing a cycle, they are a fire-and-forget hand-off of work the caller
  // does not wait for (graph::Graph::mark_feedback). Defined here because it is a
  // template (the constrained `auto` parameter makes it one).
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
      if (hop.feedback) continue;  // asynchronous: adds no latency to the caller
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
    bool feedback;      // over a cycle-closing link: asynchronous, see `response`
  };

  // Fixed propagation delay out and back: it has a mean but no variance.
  // C++20 designated initializers: `{.mean_s = ..., .variance_s2 = ...}` names each member
  // it sets, replacing positional `{2.0 * leg, 0.0}` where a reader must remember which
  // number is the mean; names must follow declaration order.
  [[nodiscard]] static flow::LatencyEstimate round_trip(double link_leg_s) {
    return {.mean_s = 2.0 * link_leg_s, .variance_s2 = 0.0};
  }

  // C++20 std::span<const Hop>: a non-owning view (pointer + size) of the hops stored in
  // the map, replacing `const std::vector<Hop>&` (which could not express "no hops"
  // without a static empty vector) or a pointer/length pair.
  [[nodiscard]] std::span<const Hop> hops_of(std::string_view op) const;
  void add_hops(const graph::Graph& g, const std::string& op,
                const std::vector<model::Step>& steps);

  std::size_t self_;
  // std::less<> (C++14 transparent comparator) lets find() take a string_view without
  // building a std::string for every lookup.
  std::map<std::string, std::vector<Hop>, std::less<>> hops_by_op_;
};

// Models: the "User" box at the top of every System Design Interview diagram — web and
// mobile clients as one aggregate source of load (ch. 1; the ch. 2 back-of-the-envelope
// estimates give its rate, e.g. 300M DAU -> QPS). Captured: a base request rate split
// into ops by fixed shares, each op split over the links that carry it by link weight
// (DNS round-robin, CDN vs origin). Ignored in phase 1: time-varying load (`now` is
// unused; phase 2 plugs rate curves and spikes in here), think time and closed-loop
// clients (offered load never reacts to latency), and client-side timeouts.
class ClientBehaviour {
 public:
  ClientBehaviour(const graph::Graph& g, std::size_t self);

  // Load offered at `now`: base × op share × route share on each route. A client is a
  // source, so `served` stays empty. Like ServiceBehaviour::route, a fresh Routed per
  // call; the engine may later pass a scratch `Routed&` instead.
  [[nodiscard]] Routed offer(SimTime now) const;

  // Response time the client sees for `op`: over the routes that carry it, a
  // share-weighted mixture of (two link legs + the entry node's response).
  //
  // Mixture of distributions (a request takes route i with probability s_i, and route i
  // has latency mean m_i and variance v_i):
  //   mean      = Σ s_i m_i
  //   variance  = Σ s_i v_i  +  Σ s_i (m_i − mean)²
  //               (law of total variance: Var X = E[Var(X | route)] + Var(E[X | route]),
  //               "the spread within routes plus the spread between them")
  // Assumes the route choice is independent of the latency on that route (fixed
  // fractions, as weighted DNS would give). Written as a sum of non-negative terms, the
  // variance cannot come out negative, unlike the textbook E[X²] − mean², which loses
  // precision when the two terms nearly cancel and so needs a clamp at zero.
  // An op the client does not send is a programmer error (std::invalid_argument): the
  // graph guarantees a route for every op of the workload.
  [[nodiscard]] flow::LatencyEstimate response(std::string_view op,
                                               ResponseLookup auto&& downstream) const {
    if (!std::ranges::binary_search(ops_, op)) {
      throw std::invalid_argument{"ClientBehaviour::response: the client does not send op '" +
                                  std::string{op} + "'"};
    }
    const auto latency_of = [&](const RouteLeg& leg) -> flow::LatencyEstimate {
      return flow::LatencyEstimate{.mean_s = 2.0 * leg.link_leg_s, .variance_s2 = 0.0} +
             downstream(leg.target, leg.op);
    };
    // Zero-share routes are skipped before their latency is looked up: it may be inf (a
    // zero-capacity node) and inf × 0 is NaN.
    const auto carries = [&](const RouteLeg& leg) { return leg.op == op && leg.route_share > 0.0; };
    double mean = 0.0;
    double within = 0.0;  // Σ s_i v_i
    for (const RouteLeg& leg : legs_) {
      if (!carries(leg)) continue;
      const auto latency = latency_of(leg);
      mean += leg.route_share * latency.mean_s;
      within += leg.route_share * latency.variance_s2;
    }
    // A route that never answers (inf mean) makes the mixture's spread infinite too, but
    // the between term would compute inf − inf = NaN, so state the limit directly. A NaN
    // anywhere in the inputs still comes out as NaN.
    if (std::isinf(mean)) {
      return {.mean_s = mean, .variance_s2 = within + std::numeric_limits<double>::infinity()};
    }
    double between = 0.0;  // Σ s_i (m_i − mean)²; second pass, the lookup is a cheap read
    for (const RouteLeg& leg : legs_) {
      if (!carries(leg)) continue;
      const double deviation = latency_of(leg).mean_s - mean;
      between += leg.route_share * deviation * deviation;
    }
    return {.mean_s = mean, .variance_s2 = within + between};
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
    double share_of_base;  // op share × route share: fraction of the base rate
    double link_leg_s;     // one-way link latency in seconds
  };

  Rate base_{};
  std::vector<RouteLeg> legs_;
  std::vector<std::string> ops_;
};

// Pattern: Strategy — the engine holds one Behaviour per node and dispatches with
// std::visit; a std::variant instead of an abstract base keeps every behaviour a plain
// value (no heap allocation, no virtual calls, copyable). Phase 2 appends alternatives
// (cache, queue, load balancer, ...).
// C++17 std::variant: a closed sum type (a type-safe union that knows which alternative
// it holds) dispatched with std::visit. Closed: adding an alternative means editing this
// line, and every std::visit that does not handle it then fails to compile.
using Behaviour = std::variant<ClientBehaviour, ServiceBehaviour>;

}  // namespace simsys::behaviours

namespace simsys::behaviours {

// Checked here as well as in the tests: a behaviour that drifts from the engine's
// contract breaks this module's build, not only a test's.
static_assert(NodeBehaviour<ServiceBehaviour>);
static_assert(RequestSource<ClientBehaviour>);

namespace detail {

// The outgoing flow towards (target, link), created on first use.
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

// ---- ServiceBehaviour ----

// Only `ops` steps: a service has no `then` (loader-enforced, see the class comment).
ServiceBehaviour::ServiceBehaviour(const graph::Graph& g, std::size_t self) : self_{self} {
  for (const auto& [op, steps] : g.nodes()[self].spec.ops) add_hops(g, op, steps);
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
                       .feedback = g.links()[link].feedback});
  }
}

std::span<const ServiceBehaviour::Hop> ServiceBehaviour::hops_of(std::string_view op) const {
  const auto found = hops_by_op_.find(op);
  if (found == hops_by_op_.end()) return {};
  return found->second;
}

Routed ServiceBehaviour::route(const flow::Flow& in) const {
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

ClientBehaviour::ClientBehaviour(const graph::Graph& g, std::size_t self) {
  const auto& workload = g.nodes()[self].spec.workload;
  if (!workload) return;  // a client without a workload offers nothing
  base_ = workload->base;
  for (const auto& [op, share] : workload->op_shares) ops_.push_back(op);
  std::ranges::sort(ops_);
  for (const auto& route : g.client_routes(self)) {
    const auto op_share =
        std::ranges::find(workload->op_shares, route.op, &std::pair<std::string, double>::first);
    legs_.push_back(RouteLeg{.op = route.op,
                             .target = route.target,
                             .link = route.link,
                             .route_share = route.share,
                             .share_of_base = op_share->second * route.share,
                             .link_leg_s = seconds(g.links()[route.link].one_way)});
  }
}

Routed ClientBehaviour::offer(SimTime /*now*/) const {
  Routed routed;
  for (const RouteLeg& leg : legs_) {
    const double rate = base_.per_second * leg.share_of_base;
    if (rate <= 0.0) continue;  // no empty branches (see ServiceBehaviour::route)
    detail::flow_towards(routed.outgoing, leg.target, leg.link).add(leg.op, Rate{rate});
  }
  detail::sort_by_destination(routed.outgoing);
  return routed;
}

}  // namespace simsys::behaviours
