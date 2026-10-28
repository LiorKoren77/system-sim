// Module `simsys.behaviours`: what each node type does with flow (client: offer load
// along its routes; service: forward step demand, serve the rest) and how it composes
// response latency from its own sojourn plus link legs and downstream responses.
//
// Latency composes on the reply path. A hop to a downstream node costs two link legs
// (request out, reply back) plus that node's own response; sequential hops add, parallel
// hops cost the slowest branch, and a client op split over several links is a
// share-weighted mixture of its routes.
//
// C++20 named module, interface unit (see src/core/core.cppm for the longer
// explanation of modules versus headers).
module;
// NO <chrono>/<format>/<print>/<filesystem> here: this interface imports other modules
// (GCC 14.2 'Bad file data' bug, CLAUDE.md §3). Link legs go through simsys::seconds().
#include <algorithm>
#include <concepts>
#include <cstddef>
#include <map>
#include <span>
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
// (std::invocable and std::same_as are C++20 standard concepts from <concepts>.)
template <class F>
concept ResponseLookup =
    std::invocable<F, std::size_t, std::string_view> &&
    std::same_as<std::invoke_result_t<F, std::size_t, std::string_view>, flow::LatencyEstimate>;

// C++20 requires-expression: lists the operations a type must support, checked at
// compile time (spec §5). This is the engine's whole requirement on a node behaviour,
// written in code instead of in a comment on an abstract base class. The lambda is a
// stand-in ResponseLookup (C++20 allows lambdas in unevaluated operands such as this).
template <class B>
concept NodeBehaviour = requires(const B b, const flow::Flow& in, flow::LatencyEstimate own) {
  { b.route(in) } -> std::same_as<Routed>;
  {
    b.response(std::string_view{}, own,
               [](std::size_t, std::string_view) { return flow::LatencyEstimate{}; })
  } -> std::same_as<flow::LatencyEstimate>;
};

// A source of requests: it offers load at simulated time `now` instead of routing an
// incoming flow.
template <class S>
concept RequestSource = requires(const S s, SimTime now) {
  { s.offer(now) } -> std::same_as<Routed>;
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
// fire here.
class ServiceBehaviour {
 public:
  ServiceBehaviour(const graph::Graph& g, std::size_t self);

  // Serves every incoming op and forwards rate × factor along each of the op's steps.
  [[nodiscard]] Routed route(const flow::Flow& in) const;

  // Response time of `op`: own sojourn + Σ sequential hops + slowest parallel hop, where
  // a hop is two link legs plus the downstream node's response. Defined here because it
  // is a template (the constrained `auto` parameter makes it one).
  //
  // C++20 abbreviated function template with a constrained placeholder:
  // `ResponseLookup auto&&` means "any type satisfying ResponseLookup", replacing
  // `template <class F> requires ResponseLookup<F>` — and `auto&&` accepts lambdas,
  // function objects and references alike without copying them.
  [[nodiscard]] flow::LatencyEstimate response(std::string_view op, flow::LatencyEstimate own,
                                               ResponseLookup auto&& downstream) const {
    flow::LatencyEstimate total = own;
    std::vector<flow::LatencyEstimate> parallel;
    for (const Hop& hop : hops_of(op)) {
      const auto leg = round_trip(hop.link_leg_s) + downstream(hop.target, hop.op);
      if (hop.mode == model::StepMode::sequential) {
        total += leg;
      } else {
        parallel.push_back(leg);
      }
    }
    return total + flow::parallel_max(parallel);
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
  };

  // Fixed propagation delay out and back: it has a mean but no variance.
  [[nodiscard]] static flow::LatencyEstimate round_trip(double link_leg_s) {
    return {.mean_s = 2.0 * link_leg_s, .variance_s2 = 0.0};
  }

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
  // source, so `served` stays empty.
  [[nodiscard]] Routed offer(SimTime now) const;

  // Response time the client sees for `op`: over the routes that carry it, a
  // share-weighted mixture of (two link legs + the entry node's response).
  //
  // Mixture of distributions (a request takes route i with probability s_i):
  //   mean      = Σ s_i m_i
  //   variance  = Σ s_i (v_i + m_i²) − mean²      (law of total variance:
  //               Var X = E[Var(X | route)] + Var(E[X | route]), rearranged as E[X²] − E[X]²)
  // Assumes the route choice is independent of the latency on that route (fixed
  // fractions, as weighted DNS would give). Zero-share routes are skipped: their
  // latency may be inf (a zero-capacity node) and inf × 0 is NaN.
  [[nodiscard]] flow::LatencyEstimate response(std::string_view op,
                                               ResponseLookup auto&& downstream) const {
    double mean = 0.0;
    double second_moment = 0.0;  // E[X²]
    for (const RouteLeg& leg : legs_) {
      if (leg.op != op || leg.route_share <= 0.0) continue;
      const auto latency =
          flow::LatencyEstimate{.mean_s = 2.0 * leg.link_leg_s, .variance_s2 = 0.0} +
          downstream(leg.target, leg.op);
      mean += leg.route_share * latency.mean_s;
      second_moment += leg.route_share * (latency.variance_s2 + latency.mean_s * latency.mean_s);
    }
    // E[X²] − E[X]² can dip a few ulps below zero when every route has the same latency.
    return {.mean_s = mean, .variance_s2 = std::max(0.0, second_moment - mean * mean)};
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
// C++17 std::variant (used here with C++20 concepts guarding each alternative): a
// type-safe union that knows which alternative it holds.
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
void sort_by_destination(std::vector<Outgoing>& outgoing) {
  std::ranges::sort(outgoing, {}, [](const Outgoing& o) { return std::pair{o.target, o.link}; });
}

}  // namespace detail

// ---- ServiceBehaviour ----

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
                       .link_leg_s = seconds(g.links()[link].one_way)});
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
