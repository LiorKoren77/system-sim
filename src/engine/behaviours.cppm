// Module `simsys.behaviours`: what each node type does with flow (client: offer load
// along its routes; service: forward step demand, serve the rest; load balancer: pass
// everything on across its links; rate limiter: pass on what it admits, reject the rest;
// cache: answer, and pass to its store what its strategy sends there; blob store: answer, and
// notify along its `then` steps for each write; message queue: append, and copy every message to
// each consumer group; worker pool: drain its jobs and hand each one on along its `then` steps;
// external sink: answer, calling nobody, while the engine fails a share of what it served) and
// how it composes response latency from its own sojourn plus link legs and downstream responses.
//
// Latency composes on the reply path. A hop to a downstream node costs two link legs
// (request out, reply back) plus that node's own response; sequential hops add, parallel
// hops cost the slowest branch, and an op split over several links (by a client or a load
// balancer) is a share-weighted mixture of its routes (`mixture`). A hop that closes a cycle of
// CALLS (graph::Graph::is_async_hop: a.msg -> b.relay -> c.job -> a.msg, three services each
// calling the next) is asynchronous:
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
import simsys.defaults; // op_cost: a database write's default price
import simsys.flow;
import simsys.fluid; // ServiceMoments: what a byte-sized node hands its queue
import simsys.graph;
import simsys.model;
import simsys.queueing; // lognormal_order_statistic, probability_fewer_than: a quorum
import simsys.workload;

export namespace simsys::behaviours {

// Demand a node sends to one neighbour over one link during a step.
struct Outgoing {
  std::size_t target{};
  std::size_t link{};
  flow::Flow flow{};
};

// What a behaviour does with one step's flow: what it forwards (one Outgoing per (target, link),
// sorted by target then link so runs are deterministic), what it accepted (`served`: its input
// minus what it rejected; a service or a balancer accepts all of it, a rate limiter what it admits)
// and what it refuses (`rejected`: admission control, spec §17 entry 6; only a rate limiter fills
// it). The node's own load is not here: the engine runs the queue BEFORE route and reports it in
// NodeSample (engine::Simulation::serve). Rejected flow is answered at once ("429 Too Many
// Requests", ch. 4) and never forwarded; the engine counts it in the node's `rejected_per_s`, apart
// from the queue's drops: a rejection is the limiter's policy working, a drop a node failing
// (report::Summary's reject_rate and drop_rate). `served` over the node's inflow is the fraction of
// its requests the node passes on, the start of its delivered fraction (see `delivered`).
struct Routed {
  std::vector<Outgoing> outgoing{};
  flow::Flow served{};
  flow::Flow rejected{};
};

// C++20 concept: a named, compile-time predicate on a type. Replaces SFINAE tricks (std::enable_if)
// and "duck typing discovered in a 30-line template error"; a constrained parameter that does not
// fit fails at the call with the concept's name. Here: anything that answers "how long does node
// `n` take to respond to op `o`?" — the engine passes a lambda over its latest per-node estimates;
// tests pass stubs. The engine's lookup is made for one caller and answers for the HOP to `n`: when
// the link between them is a bandwidth pipe (spec §19 entry 3), its wait and the request's
// serialisation come with `n`'s response (engine::Simulation::hop_response), so every behaviour
// adds a pipe the way it adds the node behind it, and knows nothing of pipes; it still adds the
// link's fixed legs itself (round_trip). (std::invocable and std::convertible_to are C++20 standard
// concepts from <concepts>; convertible_to rather than same_as so a lookup returning `const
// LatencyEstimate&` fits too.) Like every concept here it checks signatures, not behaviour: a
// lookup that returns nonsense still satisfies it.
template <class F>
concept ResponseLookup = std::invocable<F, std::size_t, std::string_view> &&
                         std::convertible_to<std::invoke_result_t<F, std::size_t, std::string_view>,
                                             flow::LatencyEstimate>;

// The same shape for the other quantity the backward pass carries: "what fraction of the
// requests of op `o` sent to node `n` come back answered?" (see `delivered` below). The engine's
// lookup includes the share a pipe on the way did not drop (engine::Simulation::hop_delivered).
template <class F>
concept FractionLookup =
    std::invocable<F, std::size_t, std::string_view> &&
    std::convertible_to<std::invoke_result_t<F, std::size_t, std::string_view>, double>;

// The FractionLookup of a step before anything was reported lost: every call delivers all it is
// sent. The default `delivered_last_step` of a route that thins its later calls (see
// NodeBehaviour), so a unit test that routes one flow by hand need not build a lookup.
struct NothingLost {
  // C++23 static operator(): a call operator that reads no member may be declared static, so a
  // call passes no `this` (before C++23 it had to be a const member function). Right here: the
  // answer is the constant 1, whoever asks.
  [[nodiscard]] static double operator()(std::size_t /*node*/, std::string_view /*op*/) {
    return 1.0;
  }
};

// C++20 requires-expression: lists the operations a type must support, checked at compile time
// (spec §5). This is the engine's whole requirement on a node behaviour, written in code instead of
// in a comment on an abstract base class. The lambda is a stand-in ResponseLookup (C++20 allows
// lambdas in unevaluated operands such as this). It checks that the calls compile and return the
// right types; what they compute is the tests' job. `cost(op)` is the capacity units one request of
// `op` uses (spec §17 entry 4): the engine charges the node's queue in units, not requests.
// `route(in, now, dt)` takes the step's start and length and is called on a non-const behaviour
// (`node`): a stateless node (service, load balancer) ignores both and may declare route const, but
// a stateful one (the rate limiter's bucket or window counters) refills by dt, finds its window
// from `now`, and must change itself. Requiring it on `B&` admits both; `cost`, `response` and
// `delivered` stay const. `delivered(op, own, lookup)` is the fraction of the requests of `op` sent
// to this node that come back answered (spec §17 entry 8): `own` is the share the node itself does
// not lose on this step (the engine's 1 − (dropped + rejected) / inflow), times what its downstream
// calls deliver. `route`'s last argument is the same kind of lookup, `delivered_last_step` (spec
// §19 entry 11, "a step after a failed step does not run"): what each call delivered at the end of
// the LAST step, which a node whose calls follow one another (a service's steps, a cache's store
// hop and `then` steps, a worker pool's hand-offs: detail::HopBlocks::forward) uses to send a later
// call only the requests that survived the earlier ones; a rate limiter thins its admitted flow by
// its counter check's fraction (it fails closed). A node whose calls all start with the request (a
// balancer's routes, a leaf) or whose calls nobody waits for (a blob store's events) has nothing to
// thin and ignores it.
template <class B>
concept NodeBehaviour = requires(const B b, B& node, const flow::Flow& in, SimTime now, SimTime dt,
                                 flow::LatencyEstimate own) {
  {
    node.route(in, now, dt, [](std::size_t, std::string_view) { return 1.0; })
  } -> std::same_as<Routed>;
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

// Delivered fractions (spec §17 entry 8, see NodeBehaviour): a request is answered only if every
// part of its path answers it, so the fractions of SEQUENTIAL parts multiply (the limiter's own
// admission x the counter check x the api: 0.25 x 1 x 0.9), and so do a service's parallel calls
// (it needs every reply). Over a SPLIT (a client's routes, a load balancer's links) a request takes
// one branch, so the fraction is the share-weighted mean Σ s_i d_i. Assumes the parts fail
// independently of one another, as the fluid's per-node fractions do. Where the fraction comes from
// at each node: what it does not lose, 1 − (dropped + rejected) / inflow
// (engine::Simulation::account_rejections): a service or balancer loses what its full queue drops,
// a token-family limiter what it refuses, a leaky bucket what overflows; a request still waiting is
// not lost. engine::Simulation's backward pass composes them, one step stale over a feedback hop as
// latency is.

}  // namespace simsys::behaviours

// Not exported: an implementation piece of ServiceBehaviour, CacheBehaviour and
// BlobStoreBehaviour below (module linkage, for the reason given on detail::Forwarding further
// down).
namespace simsys::behaviours::detail {

// The outgoing flow towards (target, link), created on first use; the outgoing flows sorted by
// (target, link), so the engine adds them in the same order on every run; and what a node that
// rejects nothing accepts of a step's flow, all of it. Defined further down; declared here for
// the route templates below.
[[nodiscard]] flow::Flow& flow_towards(std::vector<Outgoing>& outgoing, std::size_t target,
                                       std::size_t link);
void sort_by_destination(std::vector<Outgoing>& outgoing);
[[nodiscard]] Routed serve_everything(const flow::Flow& in);

// One downstream call, resolved against the graph once at construction.
struct Hop {
  std::string op;      // the op sent downstream
  std::size_t target;  // node index
  std::size_t link;    // link index the demand travels on
  double factor;       // downstream requests per request that takes the hop (graph::ServiceHop)
  double link_leg_s;   // one-way link latency in seconds
  // Not waited for: the hop closes a call cycle, or is a write-back flush or a blob store's event
  // notification (graph::Graph::is_async_hop). It forwards flow but adds no latency and fails no
  // request.
  bool asynchronous;
};

// The hops a request takes together or skips together: one unconditional step (fraction 1);
// the miss steps after one lookup (graph::ServiceHop::lookup) or a cache's own `on: miss`
// `then` steps, which a miss takes all of in turn; a cache's store hop (fraction = its
// graph::Graph::backend_fraction); or one `on: failure` step (a failure block, `reports_on` set:
// see HopBlocks). `fraction` is the share of the requests that take the block, in (0, 1] (of the
// failed ones for a failure block: 1).
//
// The cache whose misses a block carries (a service's `on: miss` steps after a lookup of it, a
// read_through cache's or a CDN's store hop for a read, a cache's own `on: miss` then-steps), and
// that cache's steady hit ratio h: the block is taken by the miss fraction 1 − h of the requests,
// and by 1 − h × factor while the cache warms back after a cache_flush (HopBlocks::warm).
struct MissesOf {
  std::size_t cache;
  double hit_ratio;
};
struct Block {
  double fraction;
  model::StepMode mode;
  std::optional<std::size_t> lookup;  // the lookup a service's miss block follows; else empty
  std::vector<Hop> hops{};  // `{}`: -Wmissing-field-initializers (see model::Step's comment)
  std::optional<MissesOf> misses{};  // a miss block's cache; empty for a block every request takes
  // A failure block's anchor: the position, in the op's blocks, of the block whose failures it is
  // sent, or of the first member of the parallel group it reports on as a whole (HopBlocks); empty
  // for every other block.
  std::optional<std::size_t> reports_on{};
};

// The blocks of hops a node makes for each incoming op, and the three things the engine asks
// of them: the flow they forward, the latency they add and the share of requests they answer.
//
// Failure blocks (spec §5, `on: failure` steps; a service's op steps only). Models the
// dead-letter path of the book's ch. 10 (notification system: a notification that failed is kept
// in the notification log and put back on a queue to be retried), [{to: db, op: write}, {to: log,
// op: write, on: failure}]. A failure block reports on its ANCHOR, the nearest unconditional step
// before it (or the parallel group that step belongs to, as a whole), and carries the requests
// the anchor failed:
//   sent = rate × S_anchor × (1 − d_anchor) × factor
// with S_anchor the survival where the anchor starts (the steps BEFORE it thin the failure step
// too: a request lost earlier never reached the anchor, so it cannot fail there) and d_anchor the
// anchor's delivered fraction (Π d over a parallel group: a request any member lost failed), read
// one simulation step late like every thinning (`forward`). An anchor that is a cache lookup with
// `on: miss` steps after it is the lookup TOGETHER WITH its miss block (a read whose miss the
// database failed is a failed read): d_anchor = d_lookup × ((1 − f) + f × Π d_miss), f the miss
// share, the block's survival; the whole miss block counts, miss steps after the failure step
// included, since they are one block a miss takes. It is not thinned by its anchor (a request
// that failed there is exactly the one sent on), nor by another failure block (two in a row report
// on the same anchor, each the same share). What it does is not fed back: its requests had failed
// already, so its own losses fail no request (it adds nothing to `delivered` and does not thin the
// steps after it), and its latency is not in the op's response: `response` is the time of a
// request that goes through every step it waits for, and no such request takes a failure step (an
// `on: miss` block, by contrast, is taken by requests that are still answered, so its legs are in
// the response, weighted by its share). Ignored: the failed request's own latency (how long it
// waited before failing: a timeout's worth, say) and anything the failure step does to the
// request itself (it is not answered after all: no retry here). A retry is a client's `retries`,
// or a pool's (phase 3, Task 11): a failure step that calls back into its own op is refused at
// build (graph::Graph::check_failure_steps, an unbounded retry storm), and one that publishes to a
// message queue whose pool calls the op again builds (the queue ends the call cycle) but retries
// every failure for ever, with no limit and no backoff: the pool's `retries` is the modelled form.
// After a step that loses nothing it sends nothing: 1 − 1 = 0.
// Pattern: Delegation (composition over inheritance) — ServiceBehaviour (its `ops` steps),
// CacheBehaviour (its store hop and its `then` steps), BlobStoreBehaviour (its events) and
// WorkerPoolBehaviour (its hand-offs) each hold one HopBlocks, as the forwarding behaviours hold a
// Forwarding, instead of sharing the arithmetic through a base.
class HopBlocks {
 public:
  // The blocks of `op`, created empty on first use: the owner fills them at construction.
  [[nodiscard]] std::vector<Block>& blocks_for(const std::string& op) { return blocks_by_op_[op]; }

  // Every block that carries the misses of `cache` is taken from now on by 1 − h × `factor` of
  // the requests (MissesOf): the cache's hit ratio h scaled by its cache_flush warm-up factor
  // (engine::Condition::hit_ratio_factor), 0 just after the flush (every read misses), 1 once it is
  // full again (the steady 1 − h, bit for bit Graph's miss fraction). Forwarding, response and
  // delivered fraction all read the new share, so a cold cache sends more to its store, waits for
  // it more often and fails more of its reads when the store does.
  void warm(std::size_t cache, double factor);

  // std::span<const Block> rather than `const std::vector<Block>&`: a span can also say "no
  // blocks" (an empty view) without a static empty vector to point at.
  [[nodiscard]] std::span<const Block> blocks_of(std::string_view op) const;

  // Adds to `outgoing` what each hop of `op` is sent when `rate` requests a second arrive: rate ×
  // the share still alive when the hop is called × the block's fraction × factor, asynchronous hops
  // included (their flow goes on; only nobody waits for it). "Still alive" (spec §19 entry 11, a
  // step after a failed step does not run): a request is answered only if every call it waits for
  // is, so a request one call failed makes none of the calls after it. With d_i the delivered
  // fraction of call i (`delivered_last_step`: what the target answered at the end of the LAST
  // simulation step, of what it was sent of that op, everything it called in turn included) and S
  // the survival, the share of the requests alive:
  //   a sequential block       is sent S;  after it   S ← S × survival(block)
  //   survival(block)          = (1 − f) + f × Π d_i over its waited hops
  //                              (f the share that takes it: a miss block fails only its misses,
  //                              h + (1 − h) × d for a cache's hit ratio h; 1 for a plain step)
  //   inside a block           hop j is sent S × f × Π_{i<j} d_i: a miss takes its fetches in turn
  //   the parallel group       every member is sent the S of the point where the group sits
  //                            (they start together and do not thin one another; Graph::build
  //                            makes its members adjacent); after it S ← S × Π survival(member):
  //                            the group fails as a whole, a request lost by any member is lost
  // An asynchronous hop (nobody waits for it) survives as 1: it fails no request. The model
  // assumes the calls fail independently (as `delivered` does) and that a failed request costs
  // its later targets nothing (no retry, no timeout spent waiting: phase 3). Its blind spot is
  // the lag: d is last step's (the engine's forward pass runs before the backward pass that
  // measures this step's d), and a failing call's d moves only once its flow has crossed the
  // link to it, so a change of loss reaches the calls after it that link's delay plus one step
  // late (behind a 500 ms link at 100 ms steps the later calls are sent the full flow until
  // 0.6 s). Calls that load one another's losses can lock `run` into a sawtooth (the engine's
  // estimate() says when, and how settle mode avoids it).
  //
  // A failure block (see the class comment) is sent rate × S_anchor × (1 − d_anchor) and changes S
  // in no way: the steps after it are thinned as if it were not there.
  void forward(std::string_view op, Rate rate, std::vector<Outgoing>& outgoing,
               FractionLookup auto&& delivered_last_step) const {
    const auto blocks = blocks_of(op);
    const double group_survival = parallel_group_survival(blocks, delivered_last_step);
    double alive = 1.0;                // S: the share no earlier call has failed
    std::optional<double> group_sent;  // the S every member of the parallel group is sent
    for (const Block& block : blocks) {
      if (block.reports_on) {
        forward_failed(blocks, block, rate.per_second, group_survival, outgoing,
                       delivered_last_step);
        continue;
      }
      if (block.mode == model::StepMode::sequential) {
        forward_block(block, rate.per_second * alive, outgoing, delivered_last_step);
        alive *= survival(block, delivered_last_step);
        continue;
      }
      if (!group_sent) {  // the group's first member: the group starts here
        group_sent = alive;
        alive *= group_survival;
      }
      forward_block(block, rate.per_second * *group_sent, outgoing, delivered_last_step);
    }
  }

  // own + Σ sequential blocks + the slowest parallel block, where a block is the sum of its
  // waited legs (two link legs + the downstream response each), taken by a share `fraction`
  // of the requests only (conditional_hop). Asynchronous hops are skipped.
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
      if (block.reports_on) continue;  // taken by failed requests only (the class comment)
      const auto legs = waited_legs(block, downstream);
      if (!legs) continue;  // every hop is asynchronous: adds no latency to the caller
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

  // own × the delivered fraction of every block, sequential or parallel (the rules above
  // `mixture`); asynchronous hops are hand-offs that cannot fail the caller and are skipped.
  // A block taken by a share f of the requests only fails those: a request gets through it
  // with probability (1 − f) + f · Π d_i (a split between "not taken", which always passes,
  // and "taken", which passes only if every hop of the block answers). A failure block fails
  // nothing: its requests had failed already (the class comment).
  [[nodiscard]] double delivered(std::string_view op, double own,
                                 FractionLookup auto&& downstream) const {
    double fraction = own;
    for (const Block& block : blocks_of(op)) {
      if (!block.reports_on) fraction *= survival(block, downstream);
    }
    return fraction;
  }

 private:
  // The share of the requests that reach `block` and get through it, (1 − f) + f × Π d over its
  // waited hops (see `delivered`), an asynchronous hop counting 1. Written 1 − f × (1 − Π d), "all
  // but the share that takes the block and fails in it", which is exactly 1 when nothing is lost
  // (the sum form can round to 1 − 1e-16 and thin a lossless path by an ulp).
  [[nodiscard]] static double survival(const Block& block, FractionLookup auto&& downstream) {
    double every_hop_answers = 1.0;
    for (const Hop& hop : block.hops) {
      if (!hop.asynchronous) every_hop_answers *= downstream(hop.target, hop.op);
    }
    return 1.0 - block.fraction * (1.0 - every_hop_answers);
  }

  // Π survival over the parallel blocks of an op: the share its parallel group lets through
  // (1 when it has none).
  [[nodiscard]] static double parallel_group_survival(std::span<const Block> blocks,
                                                      FractionLookup auto&& downstream) {
    double through = 1.0;
    for (const Block& block : blocks) {
      if (block.mode == model::StepMode::parallel) through *= survival(block, downstream);
    }
    return through;
  }

  // Sends failure block `failed` the requests its anchor failed (the class comment):
  //   rate × S_anchor × (1 − d_anchor), d_anchor = anchor_survival.
  // Nothing at all when the anchor lost nothing, not even an empty flow entry: a failure step
  // behind a lossless step routes exactly as if it were not there.
  static void forward_failed(std::span<const Block> blocks, const Block& failed, double rate_per_s,
                             double group_survival, std::vector<Outgoing>& outgoing,
                             FractionLookup auto&& downstream) {
    const double got_through =
        anchor_survival(blocks, *failed.reports_on, group_survival, downstream);
    const double failed_share = 1.0 - got_through;
    if (failed_share <= 0.0) return;
    const double reached_anchor =
        alive_before(blocks, *failed.reports_on, group_survival, downstream);
    forward_block(failed, rate_per_s * reached_anchor * failed_share, outgoing, downstream);
  }

  // d_anchor, the share of the requests that reach block `anchor` and get through it: the
  // parallel group's Π d when the anchor is a group; else the anchor's survival, times its miss
  // block's (1 − f) + f × Π d_miss when it is a lookup that `on: miss` steps follow (the class
  // comment). Exactly 1 when nothing is lost (each survival is).
  [[nodiscard]] static double anchor_survival(std::span<const Block> blocks, std::size_t anchor,
                                              double group_survival,
                                              FractionLookup auto&& downstream) {
    if (blocks[anchor].mode == model::StepMode::parallel) return group_survival;
    double through = survival(blocks[anchor], downstream);
    if (const Block* misses = miss_block_after(blocks, anchor)) {
      through *= survival(*misses, downstream);
    }
    return through;
  }

  // The miss block of the lookup at block `anchor`, if `on: miss` steps follow it: the first block
  // after it that is not a failure block, when that one follows a lookup. A miss step's lookup is
  // the nearest unconditional step before it, as a failure step's anchor is, so a miss block right
  // after the anchor (failure blocks aside) is the anchor's own; ServiceBehaviour::add_hops keeps
  // the miss steps of one lookup in one block across failure steps.
  [[nodiscard]] static const Block* miss_block_after(std::span<const Block> blocks,
                                                     std::size_t anchor) {
    for (const Block& block : blocks.subspan(anchor + 1)) {
      if (block.reports_on) continue;
      return block.lookup ? &block : nullptr;
    }
    return nullptr;
  }

  // S where block `anchor` starts (a sequential block, or the first member of the parallel group):
  // the survivals of the blocks before it, the parallel group's once as a whole, failure blocks
  // none (they thin nothing). The same product, in the same order, as `forward`'s running S.
  [[nodiscard]] static double alive_before(std::span<const Block> blocks, std::size_t anchor,
                                           double group_survival,
                                           FractionLookup auto&& downstream) {
    double alive = 1.0;
    bool group_counted = false;
    for (const Block& block : blocks.first(anchor)) {
      if (block.reports_on) continue;
      if (block.mode == model::StepMode::sequential) {
        alive *= survival(block, downstream);
      } else if (!group_counted) {
        alive *= group_survival;
        group_counted = true;
      }
    }
    return alive;
  }

  // Sends each hop of `block` its part of `requests_per_s`, the requests alive when the block
  // starts: × the share f that takes the block, × the hop's factor, × the share the block's
  // earlier hops delivered (a miss takes its fetches one after another).
  static void forward_block(const Block& block, double requests_per_s,
                            std::vector<Outgoing>& outgoing, FractionLookup auto&& downstream) {
    double alive_in_block = 1.0;
    for (const Hop& hop : block.hops) {
      // rate × factor × fraction first, the survivals last: on a path with no losses every
      // survival is exactly 1, so the product is rate × factor × fraction to the last bit.
      flow_towards(outgoing, hop.target, hop.link)
          .add(hop.op, Rate{requests_per_s * hop.factor * block.fraction * alive_in_block});
      if (!hop.asynchronous) alive_in_block *= downstream(hop.target, hop.op);
    }
  }

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

  // std::less<> (C++14 transparent comparator) lets find() take a string_view without
  // building a std::string for every lookup.
  std::map<std::string, std::vector<Block>, std::less<>> blocks_by_op_;
};

// One hop of node `self`, handling `incoming_op`, to `target`, sending `op` × `factor` per
// request: asynchronous when the graph says so (graph::Graph::is_async_hop: a write-back flush,
// a blob store's event notification, a hop that closes a call cycle).
[[nodiscard]] Hop hop_to(const graph::Graph& g, std::size_t self, std::string_view incoming_op,
                         std::size_t target, std::string op, double factor);

// The same hop when the caller knows whether it is asynchronous without asking per incoming op
// (a worker pool's hand-offs, WorkerPoolBehaviour, which never are).
[[nodiscard]] Hop hop_with(const graph::Graph& g, std::size_t self, std::size_t target,
                           std::string op, double factor, bool asynchronous);

// Adds to `hops` the blocks of node `self`'s `then` steps for incoming `op`, each taken by the
// share graph::Graph::then_fraction gives (a cache's, CacheBehaviour; a blob store's events,
// BlobStoreBehaviour). A worker pool builds its own (WorkerPoolBehaviour): one set for every op.
void add_then_blocks(const graph::Graph& g, std::size_t self, const std::string& op,
                     HopBlocks& hops);

// The misses of cache (or CDN) `self` for a read `op` (MissesOf: the share 1 − h its store hop
// and its `on: miss` then-steps are taken by); none for a write, which does not miss.
[[nodiscard]] std::optional<MissesOf> own_misses(const graph::Graph& g, std::size_t self,
                                                 std::string_view op);

}  // namespace simsys::behaviours::detail

export namespace simsys::behaviours {

// Models: a `service` box of System Design Interview (Alex Xu, vol. 1) — the stateless web/API tier
// of ch. 1 ("Scale from zero to millions of users"), and every application server of the later
// designs (ch. 11 news feed fan-out service, ch. 12 chat servers). Captured: for each incoming op,
// the downstream calls it makes (`ops` steps), the fan-out of each call (`factor`: one post becomes
// 100 feed writes), and how the call latencies compose — sequential calls add, parallel calls wait
// for the slowest — and how much of the node's capacity each op uses (`cost`, in capacity units: a
// `write` of cost 3 is three `read`s of cost 1, the way an expensive endpoint is weighed when
// sizing a tier in ch. 2). Ignored: per-op service TIME (a costly op uses more capacity, but its
// own sojourn is the node's one service time: the queue is not a multi-class one), the burstiness a
// cost adds (a cost-c request arrives as a batch of c units at once, so Kingman's `burstiness`, the
// SCV of arrivals taken unit by unit, understates its arrival variability and the queueing wait
// with it), retries and timeouts. An `on: failure` step is sent the requests the step it reports
// on failed and fails none itself (spec §5; detail::HopBlocks, failure blocks: the dead-letter
// path of a request that failed, ch. 10's notification log). A service has no `then` steps either
// (the loader rejects them for this type): everything it sends is per op. Hops that close a call
// cycle are asynchronous hand-offs (see `response`). A step after a failed step does not run (spec
// §19 entry 11; ch. 8's shorten: a URL whose id was not issued is never written): each step is sent
// only the requests every step before it answered, and the parallel group fails as a whole (route,
// detail::HopBlocks::forward), with the fractions of the previous simulation step. Cache-aside
// (spec §17 entry 7; the pattern ch. 1 draws, which the book calls read-through: the application
// reads the cache and fetches a miss itself): a step `{to: db, on: miss}` after a step to a cache
// is taken by the cache's miss fraction 1 − h of the requests only (graph::Graph::service_hops
// resolves that `fraction` once): it forwards rate × factor × (1 − h) (the factor counting every
// missed lookup), costs the response (1 − h) × the hop on average plus the spread between hit and
// miss (conditional_hop), and fails a request only when it is taken (`delivered`). The miss steps
// after one lookup form one Block: a miss takes all of them, one after another, so the block is one
// conditional hop over the sum of their legs, and a miss is answered only if every fetch is.
class ServiceBehaviour {
 public:
  ServiceBehaviour(const graph::Graph& g, std::size_t self);

  // Serves every incoming op and forwards rate × factor along each of the op's steps,
  // asynchronous hops included (over a feedback link, the flow arrives a step later), each
  // step sent only the requests the steps before it delivered last step
  // (detail::HopBlocks::forward; `delivered_last_step` defaults to NothingLost, every step the
  // full flow). Returns a fresh Routed every call; if profiling shows the per-step allocations
  // matter, the engine can pass a scratch `Routed&` to be cleared and refilled instead. `now` and
  // `dt` are unused: a service holds no state between steps (see NodeBehaviour).
  // An explicit template head rather than `FractionLookup auto&&`: a default argument needs a
  // type to default to, and an abbreviated parameter has none.
  // C++20 type-constraint: `template <C T>` is shorthand for `template <class T> requires C<T>`.
  template <FractionLookup Lookup = NothingLost>
  [[nodiscard]] Routed route(const flow::Flow& in, SimTime /*now*/, SimTime /*dt*/,
                             Lookup&& delivered_last_step = {}) const {
    Routed routed;
    for (const auto& [op, rate] : in.entries()) {
      routed.served.add(op, rate);
      // A zero rate forwards nothing: an empty branch must not exist, or the engine would
      // weight its (possibly inf) latency by zero.
      if (rate.per_second <= 0.0) continue;
      hops_.forward(op, rate, routed.outgoing, delivered_last_step);
    }
    detail::sort_by_destination(routed.outgoing);
    return routed;
  }

  // Capacity units one request of `op` consumes: the spec's `cost` entry, 1 when the op
  // is not listed (every op of a phase-1 node costs one unit).
  [[nodiscard]] double cost(std::string_view op) const;

  // Response time of `op`: own sojourn + Σ sequential hops + slowest parallel hop, where
  // a hop is two link legs plus the downstream node's response. Asynchronous hops are
  // skipped: closing a call cycle, they are a fire-and-forget hand-off of work the caller
  // does not wait for (graph::Graph::is_async_hop). A hop over a node-level feedback link
  // that closes no call cycle is counted; `downstream` then returns the target's previous
  // step (see the module comment). Defined here because it is a template (the
  // constrained `auto` parameter makes it one; see detail::HopBlocks::response).
  [[nodiscard]] flow::LatencyEstimate response(std::string_view op, flow::LatencyEstimate own,
                                               ResponseLookup auto&& downstream) const {
    return hops_.response(op, own, downstream);
  }

  // own × the delivered fraction of every block the op waits for, sequential or parallel
  // (detail::HopBlocks::delivered): a miss block fails only the requests that take it.
  [[nodiscard]] double delivered(std::string_view op, double own,
                                 FractionLookup auto&& downstream) const {
    return hops_.delivered(op, own, downstream);
  }

  // The miss steps after a lookup of `cache` are taken by 1 − h × factor of the requests from now
  // on (detail::HopBlocks::warm): the engine's call while that cache warms back after a
  // cache_flush, the cache-aside case of the warm-up (the service, not the cache, fetches a miss).
  void set_hit_ratio_factor(std::size_t cache, double factor) { hops_.warm(cache, factor); }

 private:
  void add_hops(const graph::Graph& g, const std::string& op);
  [[nodiscard]] static detail::Block* open_miss_block(std::vector<detail::Block>& blocks,
                                                      std::size_t lookup);
  [[nodiscard]] static std::optional<std::size_t> anchor_block(
      std::span<const detail::Block> blocks, std::span<const std::size_t> block_of_hop,
      std::optional<std::size_t> reports_on);
  [[nodiscard]] detail::Hop hop_of(const graph::Graph& g, std::string_view op,
                                   const graph::ServiceHop& resolved) const;
  [[nodiscard]] std::optional<detail::MissesOf> misses_of_lookup(
      const graph::Graph& g, const std::string& op, std::optional<std::size_t> lookup) const;

  std::size_t self_;
  detail::HopBlocks hops_;  // per incoming op: its resolved steps, in blocks
  // std::less<> (C++14 transparent comparator) lets find() take a string_view without
  // building a std::string for every lookup.
  std::map<std::string, double, std::less<>> cost_by_op_;  // only the ops listed in `cost`
};

// Models: the "User" box at the top of every System Design Interview diagram — web and mobile
// clients as one aggregate source of load (ch. 1; the ch. 2 back-of-the-envelope estimates give its
// rate, e.g. 300M DAU -> QPS). Captured: a request rate that follows the scenario's rate curve over
// time (daily waves, ramps, spikes, replayed traces: workload::RateCurve) times a seeded random
// multiplier (workload::NoiseProcess), split into ops by fixed shares, each op split over the links
// that carry it by link weight (DNS round-robin, CDN vs origin), and a `timeout` after which the
// client gives up on a request (the engine counts the slow tail of each response as timed out
// instead of answered: engine::Simulation::give_up_after), and `retries` (below). Ignored: think
// time and closed-loop clients (offered load never reacts to latency: a slow site does not make
// these users send less or leave).
//
// Retries (phase 3 spec §6.1): the retry storm of ch. 1's and ch. 10's advice to retry a failed
// call with a backoff. A client with `retries: n` sends every request it lost (a node on the path
// dropped, refused or failed it) or timed out again, `retry_after` later, up to n extra attempts.
// In a fluid model that is FEEDBACK DEMAND: with b = retry_after / step rounded UP to whole steps
// (at least one: backoff_steps) and q_t the share of step t's attempts the path loses or times out,
//   attempts_t = curve_t + Σ_{k=1..n} r_k(t)      r_k(t) = r_{k−1}(t − b) × q_(t−b),  r_0 = curve
// r_k(t) being attempt k of the requests first sent at t − k·b: every attempt fails at the share
// the path fails in the step it is sent (RetryRing keeps the r_k in flight). On a path that
// fails a steady q, a request is sent 1 + q + q² + ... + q^n = (1 − q^(n+1)) / (1 − q) times (a
// geometric series that `retries` caps), is answered with probability 1 − q^(n+1) and is lost for
// good, its last attempt failed too, with probability q^(n+1). Why `offered` stays the curve: the
// retries are the clients' own doing, not more users, so a storm shows as `attempts/s` above
// `offered/s`. Why the servers do every attempt's work: nothing cancels a failed attempt (a
// dropped one cost its queue a place, a failed one its service, a timed-out one was served after
// the client left), and the retries pile on top, which is what turns an outage into a storm.
// Ignored: jitter (every retry waits exactly `retry_after`, rounded up to whole steps), exponential
// backoff (the wait does not grow from one attempt to the next), retry budgets and circuit
// breakers, idempotency (a retried write that had in fact succeeded is not counted twice), a user
// who gives up between attempts, and the time a retried request lost on its failed attempts: its
// latency is its LAST attempt's (engine::Simulation::client_responses composes one response per
// step, the path's), so the timeout τ each timed-out attempt waited (about nothing for a lost one)
// and the backoffs before each retry are not added. A proper model would make the response a
// mixture over the attempt that is answered: attempt k + 1, with probability q^k (1 − q), takes
// k × (τ or 0) + k × backoff more than one attempt does. That is phase 4 material.
class ClientBehaviour {
 public:
  // `seed` seeds this client's noise process; the engine passes workload::client_seed(scenario
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

  // The factor a scripted traffic_spike puts on the rate for the following `offer`s
  // (engine::Condition::rate_factor; 1 outside every fault). The engine sets it every step before
  // the client offers, as `tick` sets the noise.
  void set_rate_factor(double factor) { rate_factor_ = factor; }

  // The total rate offered at `now`: curve.rate_at(now) × the traffic_spike factor × the
  // multiplier of the last tick (1 before any tick, and always 1 without noise).
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

  // The client's `timeout` in seconds; empty when it waits for ever.
  [[nodiscard]] std::optional<double> timeout_s() const { return timeout_s_; }

  // The client's `retries` n: how many more times it sends a request that failed (0: never).
  [[nodiscard]] int retries() const { return retries_; }

  // The wait before a retry in whole steps: `retry_after` / `step` rounded UP, and at least one.
  // explain prints it beside a retry_after that is not a whole number of steps.
  [[nodiscard]] static int backoff_steps(SimTime retry_after, SimTime step);

  // What the client sends of `op` this step, retries included: `offered` (rate × op share, see
  // `offered`) plus the retries due now. `offered` itself without retries.
  [[nodiscard]] Rate attempts(std::string_view op, Rate offered) const;

  // The part of this step's attempts of `op` that are their request's last (attempt n's, the
  // first attempt's when n = 0): a request whose last attempt fails is lost for good.
  [[nodiscard]] Rate last_attempts(std::string_view op, Rate offered) const;

  // After the step: of every attempt of `op` sent this step but the last ones, `failed_share` (the
  // share the path lost or that timed out) is sent again `retry_after` later as the next attempt.
  // `offered` is the step's first attempts (the `offered` the step's attempts were made of). A
  // no-op for a client without retries.
  void retry_failed(std::string_view op, Rate offered, double failed_share);

  // Every retry in flight forgotten, and from now on each retry sent at the next evaluation: what
  // estimate()'s scratch copy needs, where every pass is the same instant and a backoff is no
  // more than a delay on the way to the fixed point (engine::Simulation::settling_copy).
  void settle_retries();

 private:
  // One op's retries in flight: the rate of each attempt k = 1..n that will be sent in each of the
  // next `backoff` steps. A ring of `backoff` slots, one per step until it is due, each holding
  // the n attempts' rates (attempt k at index k − 1). The current slot is what this step sends;
  // after the step it is refilled with the retries of this step's failures, which come round
  // again `backoff` steps later, and the ring moves on one slot.
  class RetryRing {
   public:
    RetryRing(int retries, int backoff_steps);
    [[nodiscard]] double due() const;  // Σ of the attempts due this step
    [[nodiscard]] double last_attempts(double first) const;
    void fail(double first, double failed_share);

   private:
    [[nodiscard]] std::span<double> current();
    [[nodiscard]] std::span<const double> current() const;
    std::size_t retries_;
    std::size_t slots_;
    std::size_t slot_{0};
    std::vector<double> rates_;  // slots_ × retries_, slot by slot
  };

  [[nodiscard]] std::optional<std::size_t> ring_index(std::string_view op) const;
  [[nodiscard]] double retries_due(std::string_view op) const;
  void start_retries(int backoff_steps);

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
  double rate_factor_{1.0};       // set_rate_factor: the active traffic_spike faults' product
  std::vector<std::pair<std::string, double>> op_shares_;  // sorted by op
  std::vector<RouteLeg> legs_;
  std::vector<std::string> ops_;
  std::optional<double> timeout_s_{};
  int retries_{0};
  std::vector<RetryRing> retry_rings_;  // like ops_; empty without retries
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

// Models: the "Load balancer" box of System Design Interview (Alex Xu, vol. 1) ch. 1 — the single
// public entry that spreads requests over a stateless web tier so that servers can be added or lose
// one without users noticing — and the hash ring of ch. 5 ("Design consistent hashing"), which
// sends each key to the same server. Captured: the balancer is a queue of its own (default 100k/s,
// 50 us: an L7 proxy hop, see simsys.defaults), so it adds its own sojourn; it forwards every op it
// receives over the links that carry it, split by link weight; and its policy (spec §17 entry 5)
// changes how the replicas BEHIND it queue, which graph::Graph::mark_balanced_targets resolves and
// the engine applies:
//   round_robin / weighted -> independent replicas, each an M/M/c at its share;
//   least_connections      -> one pooled M/M/(c·N) queue (join-the-shortest-queue ideal);
//   consistent_hash        -> the hottest of the M replicas carries 1 + z_M/sqrt(vnodes) × the
//                             average (z_M the expected maximum of M standard normals, the
//                             ring's machines: graph::ring_imbalance) and is queued apart from
//                             the M - 1 cooler ones.
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
  // The engine's call (see NodeBehaviour): a balancer's routes are a split, each request takes
  // one, so no call follows another and last step's losses thin nothing.
  [[nodiscard]] Routed route(const flow::Flow& in, SimTime now, SimTime dt,
                             FractionLookup auto&& /*delivered_last_step*/) const {
    return route(in, now, dt);
  }

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

// Models: the "Rate limiter" of System Design Interview (Alex Xu, vol. 1) ch. 4 — a middleware in
// front of the API servers that counts each client's requests in a fast store (Redis in the book)
// and answers "429 Too Many Requests" once a client is over its limit. Captured (spec §17 entry 6):
// the limiter is a queue of its own (default 100k/s, 50 us: an in-memory counter check, see
// simsys.defaults) that sees EVERY request, admitted or not; what it admits goes on over its links
// like a load balancer's flow; what it refuses is `rejected`, answered at once and never forwarded;
// with a `backend` (the counter store) every request it CHECKS, admitted or refused, first reads
// its counter there (one `counter` request each), which adds that hop to the response. A check the
// store lost fails the request closed: it is neither admitted onwards nor answered, so the admitted
// flow is forwarded × the share of the checks the store answered (last step's, as for any later
// step: detail::HopBlocks::forward), the same factor `delivered` multiplies by. (Failing open, an
// unanswered check letting the request through, is the other choice real limiters make; it would
// forward everything and answer what the check lost.) Each of the five algorithms of ch. 4 keeps
// its own state, stepped once per step of length dt that starts at `now`. With W = limit × window
// (requests a window may hold) and `in` the inflow (req/s):
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
// its own sojourn plus the counter check. Its CALLER sends its later steps only the requests
// the gate let through: a refused request is answered with a 429 and goes no further, so the
// gate's delivered fraction (its refusals included) thins the steps after it, one simulation
// step plus the link's delay late (detail::HopBlocks::forward). The refused share shows in the
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
  [[nodiscard]] Routed route(const flow::Flow& in, SimTime now, SimTime dt) {
    return route_checked(in, now, dt, 1.0);
  }
  // The engine's call (see NodeBehaviour): the counter check comes BEFORE the request is passed
  // on (as in `response`), so the limiter fails closed: a request whose check was lost is not
  // forwarded. The admitted flow is thinned by the check's delivered fraction of the last step
  // (detail::HopBlocks::forward's rule for a later step), matching `delivered`.
  [[nodiscard]] Routed route(const flow::Flow& in, SimTime now, SimTime dt,
                             FractionLookup auto&& delivered_last_step) {
    const double check = backend_ ? delivered_last_step(backend_->target, graph::counter_op) : 1.0;
    return route_checked(in, now, dt, check);
  }

  // One capacity unit per request, admitted or rejected: the check costs the same.
  [[nodiscard]] double cost(std::string_view /*op*/) const { return 1.0; }

  // own sojourn + the counter store hop (two link legs + the backend's `counter` response) + the
  // downstream mixture as for a load balancer. The counter check happens BEFORE the request is
  // passed on, so the two hops are sequential and add. This is the response of an ADMITTED request.
  // A rejected request also waited for the counter check (own + the counter hop, then the 429), but
  // it is not answered, so it never reaches the client's latency figures: spec §17 entry 8 weighs
  // them by the answered requests (`delivered`).
  [[nodiscard]] flow::LatencyEstimate response(std::string_view op, flow::LatencyEstimate own,
                                               ResponseLookup auto&& downstream) const {
    flow::LatencyEstimate total = own;
    if (backend_) {
      total += round_trip(backend_->link_leg_s) + downstream(backend_->target, graph::counter_op);
    }
    return total + forwarding_.response(op, downstream);
  }

  // own (the share not refused: 1 − refused / offered, or for a leaky bucket 1 − overflow /
  // demand) × the counter check's delivered fraction (a lost check answers nothing) × the
  // routes' mixture.
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
  // `route` with the share of the counter checks that answered, `check_answered`: the admitted
  // flow is forwarded × that share (fail closed), the counter checks are sent whole.
  [[nodiscard]] Routed route_checked(const flow::Flow& in, SimTime now, SimTime dt,
                                     double check_answered);

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

// Models: the "Cache" tier of System Design Interview (Alex Xu, vol. 1) ch. 1 ("Scale from zero to
// millions of users": a memory store in front of the database for data read often and changed
// rarely) and its uses in ch. 8 (URL shortener: <shortURL, longURL> in a cache), ch. 11 (news feed
// caches), ch. 12 (chat), ch. 13 (autocomplete) and ch. 14 (YouTube metadata). Spec §17 entry 7.
// Captured: the cache is a queue of its own (default 100k/s, 250 µs: an in-memory lookup, the
// latency table's 1 MB memory read, see simsys.defaults) that answers every request sent to it,
// hits and misses alike, at its `cost` per op; a FIXED hit ratio h, the share of reads it answers
// from memory; which ops are writes (`write_ops`, every other op is a read); and the strategy, i.e.
// who talks to the store. Names: ch. 1 draws one pattern, the application reading the cache and
// fetching a miss itself, and calls it "read-through"; the industry's name for it is cache-aside,
// which is ours. Our `read_through` is the cache-driven fetch, and the write strategies are general
// caching practice, not the book's:
//   cache_aside   -> the application. The cache serves everything and forwards nothing; the
//                    service's `{to: db, on: miss}` step carries the misses (ServiceBehaviour).
//   read_through  -> the cache. It forwards 1 − h of each read to its `backend` as the same op
//                    and answers a miss after the fetch: response = the mixture
//                    {h: own, 1 − h: own + two link legs + the backend's response}.
//   write_through -> every write goes on to the backend, synchronously: own + the hop.
//   write_around  -> the same flow as write_through: writes skip the cache and go to the
//                    store. What differs is what the cache holds afterwards (fewer entries
//                    nobody reads again), which shows in the hit ratio, and h is fixed here.
//   write_back    -> (spec §18 entry 5) the cache acknowledges a write from its own memory
//                    and flushes it to the backend later: every write goes on to the store as
//                    the same op (the store's load is unchanged: an upper bound, no coalescing
//                    of repeated writes to one key), but the flush is ASYNCHRONOUS BY DESIGN
//                    (graph::Graph marks it in its async hops), so the client's write response
//                    is the cache's own sojourn and a lost flush fails no client request.
// Under read_through a write is served by the cache alone (the application writes the store itself,
// as in cache_aside); under the write strategies a read miss is the application's to fetch (a
// service `on: miss` step), as in cache_aside. Which share of each op goes to the store is
// graph::Graph::backend_fraction, the one rule the paper load and the call-cycle search share with
// this behaviour. `then` steps (spec §18 entry 5): what happens after the cache has answered, when
// the cache sits IN FRONT of the next tier rather than beside the application: the inline
// placement, in which the cache itself forwards what it misses (read-through in spirit), NOT ch.
// 1's cache-aside, where the web server asks the cache and queries the database itself and the
// cache forwards nothing. The book's examples of a cache in front: ch. 1's CDN workflow (a CDN that
// does not have a file fetches it from the origin server, then serves it) and ch. 13's "Browser
// caching" (Google keeps autocomplete suggestions in the browser cache for an hour: the scenario's
// `ttl: 1h`): a hit answers in the browser, a miss goes on to the load balancer.
//   on: always -> every request the cache handles, hit or miss, sends rate × factor of
//                 `step.op` (an audit log, say), and waits for it: own + the hop. A sequential
//                 one adds its hop; the `mode: parallel` ones form ONE group, whose slowest hop
//                 the cache waits for AFTER its store hop and its sequential steps (own + store
//                 hop + sequential hops + the slowest parallel one): they overlap one another,
//                 never the store hop.
//   on: miss   -> the misses of the READS only (writes do not miss): (1 − h) × factor of them.
//                 A miss waits for the hop, a hit does not, so the response is the hit/miss
//                 mixture {h: own, 1 − h: own + hop} (conditional_hop). Several miss steps
//                 form one block, taken all together and in turn by a miss, like the miss
//                 steps after one lookup of a service; so each must be sequential (graph::Graph
//                 refuses a parallel one). A read_through cache cannot have one (its own backend
//                 fetch already fetches the miss), and a service cannot fetch the misses of a
//                 cache that has one again with an `on: miss` step of its own (graph::Graph
//                 refuses both double fetches).
//   Neither may send the store the op the store hop sends it (graph::Graph refuses a then step
//   that duplicates the backend hop).
// The share of each op a then-step carries is graph::Graph::then_fraction, as the paper load's.
// The cache strategy (caching practice's word, not the GoF Strategy pattern) is a closed enum,
// not a class per strategy: graph::Graph::backend_fraction tests it (through
// graph's sends_to_store) once per op at construction, and this behaviour only reads the
// resulting shares.
// A cache_flush (phase 3 spec §4.3, engine::FaultSchedule's WarmUp) empties the cache: the engine
// scales h by the warm-up factor every step (set_hit_ratio_factor), 0 at the flush, so every read
// misses, back to 1 once the cache is full; for a cache_aside cache it is the service's miss steps
// that it scales (ServiceBehaviour::set_hit_ratio_factor), as they carry the misses.
// Ignored: which keys are popular (h is a number here; graph::Graph turns a `che` hit ratio into
// one at build), entries expiring by TTL and the
// eviction order (`ttl`, `eviction` are read and printed only), a cold cache filling up
// after a start, the thundering herd of misses when a hot key expires (every
// miss is an independent request to the store), invalidation traffic, and write-back's
// dirty-entry queue: how far the store lags behind the cache, and the writes lost when the
// cache dies before a flush.
class CacheBehaviour {
 public:
  CacheBehaviour(const graph::Graph& g, std::size_t self);

  // Serves every incoming op (`served` is the whole input: a miss is answered too, after
  // the fetch), forwards rate × backend share of each op to the backend, and rate × factor ×
  // the step's share along each `then` step (the class comment), each block sent only the
  // requests the blocks before it delivered last step (detail::HopBlocks::forward: a `then` step
  // after a read_through fetch is sent the hits and the misses the store answered). `now` and
  // `dt` are unused: the only state the cache holds between steps is its hit ratio's warm-up
  // factor, which the engine sets (set_hit_ratio_factor). The template head as in
  // ServiceBehaviour::route.
  template <FractionLookup Lookup = NothingLost>
  [[nodiscard]] Routed route(const flow::Flow& in, SimTime /*now*/, SimTime /*dt*/,
                             Lookup&& delivered_last_step = {}) const {
    Routed routed;
    for (const auto& [op, rate] : in.entries()) {
      if (rate.per_second <= 0.0) continue;  // no empty branches (see ServiceBehaviour::route)
      routed.served.add(op, rate);
      hops_.forward(op, rate, routed.outgoing, delivered_last_step);
    }
    detail::sort_by_destination(routed.outgoing);
    return routed;
  }

  // Capacity units one request of `op` consumes: the spec's `cost` entry, else 1 (a hit and
  // a miss cost the cache the same lookup; the miss's fetch is charged to the backend).
  [[nodiscard]] double cost(std::string_view op) const;

  // own sojourn + each block the op waits for, taken by the share that takes it
  // (conditional_hop): nothing more under cache_aside, (1 − h) × the store hop for a
  // read_through read, the whole hop for a write_through or write_around write, nothing for a
  // write_back write (its flush is asynchronous), every `on: always` then-hop, and the block
  // of `on: miss` then-hops for the misses of a read.
  [[nodiscard]] flow::LatencyEstimate response(std::string_view op, flow::LatencyEstimate own,
                                               ResponseLookup auto&& downstream) const {
    return hops_.response(op, own, downstream);
  }

  // own × ((1 − f) + f × Π the block's delivered fractions) over the blocks, f the share that
  // takes each: a hit is answered by the cache alone, a forwarded request only if the store
  // answers it ("own × (h + (1 − h) · backend)" for a read_through read); an `on: always`
  // then-hop multiplies by its target's fraction; a write-back flush counts for nothing.
  [[nodiscard]] double delivered(std::string_view op, double own,
                                 FractionLookup auto&& downstream) const {
    return hops_.delivered(op, own, downstream);
  }

  // The cache's hit ratio is h × factor from now on (engine::Condition::hit_ratio_factor, a
  // cache_flush's warm-up): its read misses, to the store and along its `on: miss` then-steps,
  // are 1 − h × factor of the reads (detail::HopBlocks::warm). A `cache` other than this one
  // changes nothing: a cache's own blocks carry only its own misses.
  void set_hit_ratio_factor(std::size_t cache, double factor) { hops_.warm(cache, factor); }

 private:
  void add_backend_block(const graph::Graph& g, const std::string& op);

  std::size_t self_;
  // Per op that can reach the cache (graph::Graph::arriving_ops): the store hop as a block
  // taken by graph::Graph::backend_fraction of the requests (absent under cache_aside, which
  // has no backend edge), then the then-steps' blocks. A store hop is waited for unless it is
  // a write-back flush: no call cycle runs through it (graph::Graph refuses one that does,
  // check_backend_calls).
  detail::HopBlocks hops_;
  std::map<std::string, double, std::less<>> cost_by_op_;  // only the ops listed in `cost`
};

// Models: the relational database of System Design Interview (Alex Xu, vol. 1) ch. 1 ("Scale from
// zero to millions of users"), as its two scaling sections draw it, and every "DB" box of the later
// designs (ch. 8's URL table, ch. 7's ID tickets). Spec §18 entry 6.
//  - Database replication (ch. 1): `replicas: N` is one primary and N − 1 read replicas. Every
//    write (an op in `write_ops`, default insert, update, delete and write) goes to the
//    primary; reads go to the replicas, or to the primary when there are none (N = 1). Each
//    replica replays every write the primary COMMITTED (its log holds nothing the primary
//    dropped), so a replica's load is its share of the reads PLUS all the committed writes:
//    the reason the book says replication scales reads, not writes.
//  - A write's price: the spec's "SQL 5k reads / 1k writes" per second makes a write cost 5
//    capacity units (defaults::sql_write_cost) on the primary and on each replica that replays
//    it, unless the scenario's `cost` says otherwise; a read costs 1.
//  - Sharding (ch. 1, horizontal scaling): `shards: S` splits the data over S independent
//    sets of the same shape (a primary and N − 1 replicas each), each seeing 1/S of the
//    reads and of the writes: a perfect sharding key, no hot shard (the celebrity problem the
//    book warns about is not modelled).
//  - The connection pool (`pool`): how many queries one copy has in service at once. By
//    Little's law (concurrency in service = throughput × service time) a pool below the c the
//    capacity implies caps the copy at pool / service time with c = pool, in capacity units (a
//    write at cost 5 holds its connection five service times); a larger pool changes nothing
//    (graph::apply_database_keys).
//  - Replication lag (`replication_lag`, a constant): a replica receives a write that long after
//    the primary finished it and then applies it after its own queue's wait, so a read may
//    return data as old as the primary's write sojourn + the lag + the replica's sojourn: the
//    node's `staleness` (engine::NodeSample::staleness_s, the `staleness` SLO). A replica too
//    busy to replay every write falls behind without bound (the model drops the excess at
//    queue_limit; a real replica keeps it in its relay log/WAL until it is rebuilt): the
//    writes one replica misses are engine::NodeSample::replication_dropped_per_s, the staleness
//    stays the bounded queue's figure, and the `staleness` SLO fails whatever the mean age.
// Who does what: the ENGINE holds the two kinds of queue (primary and replica copies, see
// engine::Simulation::make_database) and splits the load between them with `is_write`; it
// hands `response` the sojourn of the queue the op uses (a write the primary's, a read a
// replica's). This behaviour serves everything, forwards nothing (a database is a leaf: it
// calls nobody) and composes nothing more.
// Ignored: replication conflicts and multi-primary set-ups, failover (a replica promoted when
// the primary dies, ch. 1: a database never fails in this model; faults come with phase 3),
// transactions and locks, queueing for
// a connection beyond the pool (a query waiting for a free connection waits in the same queue
// here), cross-shard queries and joins, resharding, hot shards, per-op service times (a write
// costs more capacity, 5 units by default, not a longer service time), and read-your-writes:
// a client that reads right after writing may see the stale replica, which only the staleness
// number tells.
class SqlDbBehaviour {
 public:
  SqlDbBehaviour(const graph::Graph& g, std::size_t self);

  // Serves every incoming op and forwards nothing: a database answers queries itself. `now`
  // and `dt` are unused (a database holds no state between steps).
  [[nodiscard]] Routed route(const flow::Flow& in, SimTime now, SimTime dt) const;
  // The engine's call (see NodeBehaviour): a leaf calls nobody, so there is nothing to thin.
  [[nodiscard]] Routed route(const flow::Flow& in, SimTime now, SimTime dt,
                             FractionLookup auto&& /*delivered_last_step*/) const {
    return route(in, now, dt);
  }

  // Capacity units one query of `op` uses (defaults::op_cost): the spec's `cost` entry, else 5
  // for a write (defaults::sql_write_cost), else 1; on the primary and on each replica that
  // replays it alike.
  [[nodiscard]] double cost(std::string_view op) const;

  // Whether `op` changes data and so goes to the primary (it is in `write_ops`); every other
  // op is a read a replica answers.
  [[nodiscard]] bool is_write(std::string_view op) const;

  // The sojourn the engine chose for the op (its own queue's): a database calls nobody.
  [[nodiscard]] flow::LatencyEstimate response(std::string_view /*op*/, flow::LatencyEstimate own,
                                               ResponseLookup auto&& /*downstream*/) const {
    return own;
  }

  // The share of the op's queries the database answers itself: no downstream to fail them.
  [[nodiscard]] double delivered(std::string_view /*op*/, double own,
                                 FractionLookup auto&& /*downstream*/) const {
    return own;
  }

 private:
  std::vector<std::string> write_ops_;
  // The ops listed in `cost` and every write op (at its default unless listed); the rest cost 1.
  std::map<std::string, double, std::less<>> cost_by_op_;
};

// How a byte-sized node (defaults::is_byte_sized: a blob store, a CDN sized in bytes) spends its
// time on one request (spec §19 entry 2; the model and its bounds are in BlobStoreBehaviour's
// header). Per op that reaches the node, resolved once at build from its size in megabytes MB_o
// (graph::op_cost) and the node's service time s_MB, the time of ONE megabyte:
//   object time  MB_o × s_MB                     how long one request holds one of the c slots:
//                                                its whole transfer
//   answer time  max(MB_o, 1) × s_MB for a write  when its client has the answer once it is in
//                s_MB for every other op          service: a write's last byte, a read's first
//                                                megabyte (never under one megabyte's time)
// The two differ for a small request: a 2 KB read holds its slot 0.002 × 40 ms = 80 µs (the
// capacity it uses: only its bytes) but is answered after 40 ms, since a store hands out whole
// megabytes, the unit of its service time, and no answer comes sooner than one of them.
// Each object's time is deterministic by default (cs² = 0, defaults::default_service_variability:
// a fixed size at a fixed rate), so the spread the queue sees is the size mix's.
// A CDN takes no writes (graph::Graph::build refuses them), so all its answers are first
// megabytes. A bandwidth link's pipe times its requests with this class too (serialising): a
// write at its last byte, any other op at its first megabyte.
// Pattern: Delegation — a blob store and a CDN sized in bytes each hold one MegabyteService (an
// optional member) and hand it their timing questions (megabyte_service(): object times for the
// queue, a request's answer time), as the engine's pipes do; none of them inherits it, and a
// node that counts requests simply holds none.
class MegabyteService {
 public:
  MegabyteService(const graph::Graph& g, std::size_t self);

  // A bandwidth link's pipe (spec §19 entry 3, engine::Simulation::pass_through_pipe): one megabyte
  // takes `megabyte_s` (1 MB / bandwidth) and a request of op o is `megabytes_by_op[o]` long. The
  // pipe's queue waits for whole requests (object time MB_o × s_MB: a request holds the one server
  // for its whole serialisation); the hop's answer follows spec §19 entry 2's rule for the node
  // behind it:
  //   a write (`write_ops`: graph::ResolvedLink::write_ops)               MB_o × s_MB
  //        the receiver acts after the LAST byte (a PUT is stored once all of it arrived; an op is
  //        a write on the link if the receiver or a node behind it that its body reaches, under
  //        its own name or a sized step's, lists it, so a balancer in front of the store changes
  //        nothing, nor does a service that sends the body on as a sized `put`);
  //   any other op                                                        min(MB_o, 1) × s_MB
  //        the receiver answers at its first megabyte while the rest streams in behind it.
  // For a streamed PUT into a blob store the wire's and the disk's whole-object times then add in
  // series (an upper bound: real pipelining overlaps them). No floor of one megabyte (a store's
  // unit of service; a link serialises bytes, so a 2 KB read is on the wire 16 µs at 1 Gbit/s)
  // and no per-object spread (cs² = 0: a fixed size at a fixed rate).
  // Pattern: Static factory (named constructor) — the same timing, built from a link's numbers
  // rather than a node's, under a name that says so.
  [[nodiscard]] static MegabyteService serialising(
      double megabyte_s, const std::map<std::string, double, std::less<>>& megabytes_by_op,
      std::span<const std::string> write_ops);

  // The moments of the object time over this step's arrivals `requests` (requests per second per
  // op), a mixture whose branch o is taken with probability p_o = rate_o / Σ rate and lasts
  // s_o = MB_o × s_MB with the node's per-object SCV cs² (0 by default):
  //   mean   E[S]   = Σ_o p_o s_o                        what Erlang C is fed as the service time
  //   second E[S²]  = Σ_o p_o (1 + cs²) s_o²             (E[X²] = Var X + mean² = (1 + cs²) mean²
  //                                                       on each branch)
  //   SCV    c_s²   = E[S²] / E[S]² − 1                  what the Allen–Cunneen factor
  //                                                       (c_a² + c_s²) / 2 is fed (queueing)
  // A mean alone would let many small requests shorten the large ones' wait (1000 reads/s of
  // 2 KB beside 8 writes/s of 50 MB: the mean object falls from 2 s to 16 ms and the M/M/c wait
  // from 122 ms to 1 ms, although rho rose); the SCV (124 there) puts the spread back, as M/G/c
  // queues do: a few long services hold up many short ones. Empty when nothing arrives: there is
  // no mix to weigh.
  [[nodiscard]] std::optional<fluid::ServiceMoments> object_time_moments(
      const flow::Flow& requests) const;

  // The node's own sojourn for `op`: the queue's `wait` plus the op's answer time, whose
  // variance is cs² × answer² (the node's per-object variability: 0 by default, so the answer
  // adds no spread and the sojourn's variance is the wait's), independent of the wait. An op
  // that never reaches the node is answered like a read.
  [[nodiscard]] flow::LatencyEstimate sojourn(std::string_view op,
                                              const flow::LatencyEstimate& wait) const;

  // A read's sojourn whatever its op: the wait plus one megabyte's time.
  [[nodiscard]] flow::LatencyEstimate first_megabyte(const flow::LatencyEstimate& wait) const;

 private:
  struct Times {
    double object_s{};
    double answer_s{};
  };
  MegabyteService() = default;  // only for serialising, which fills every member
  [[nodiscard]] Times times_of(std::string_view op) const;
  [[nodiscard]] flow::LatencyEstimate answered_after(const flow::LatencyEstimate& wait,
                                                     double answer_s) const;

  double megabyte_s_{};                                    // s_MB
  double service_variability_{};                           // cs² of one object's time
  std::map<std::string, Times, std::less<>> times_by_op_;  // every op that reaches the node
};

// Models: the CDN of System Design Interview (Alex Xu, vol. 1) ch. 1 ("Content delivery network":
// geographically dispersed servers that keep static files close to the user) and ch. 14 (the CDN
// that streams YouTube's videos); ch. 13's browser cache sits in the same place, one hop closer.
// Spec §18 entry 10. Captured: the CDN is a read-only cache in front of its `origin`. It is a queue
// of its own (default 100k/s, 10 ms: serving a ~1 MB object over the network, see simsys.defaults)
// that answers every read sent to it; the share h it hits (`hit_ratio`, fixed or che) it answers
// alone, the misses 1 − h it fetches from the origin as the SAME op and answers after the fetch
// (the book's ch. 1 CDN workflow, paraphrased: a CDN server that does not have the image in its
// cache requests the file from the origin, stores it and returns it). So it behaves exactly like a
// read_through cache whose backend is the origin:
//   flow to the origin = rate × (1 − h) of each read           (graph::Graph::backend_fraction)
//   response           = mixture {h: own, 1 − h: own + two link legs + the origin's response}
//   delivered          = own × (h + (1 − h) × the origin's delivered fraction)
// A `che` hit ratio is the Che approximation (simsys.caching) for the CDN's `keys` objects: it
// assumes LRU eviction and independent requests drawn from the clients' key popularity (the
// independent reference model), so a TTL-evicted edge and the bursts of a video going viral are
// outside it. A CDN without a hit ratio misses every read (h = 0): the safe side on paper, and what
// runs. A tiered CDN works as it should: a CDN whose origin is another CDN sends it its misses, and
// a che hit ratio there is computed over the first tier's misses (graph's key walk). "A read" is
// every op that is not one of the CDN's write ops (model::write_ops_of: `write`, or the scenario's
// `write_ops`), and it takes no writes: graph::Graph::build refuses a write op that reaches one,
// and explain lists the ops it serves. Bytes (spec §19 entry 2): a CDN sized in bytes (`capacity:
// 100GB/s`) counts megabytes, as a blob store does (BlobStoreBehaviour has the model and its
// bounds): 100k units/s of 1 MB, a 50 MB stream costs 50 of them, the 10 ms service time is one
// megabyte's, so c = 100k/s × 10 ms = 1000 megabyte slots and the CDN saturates at 100e9 / 50e6 =
// 2k streams/s. Its queue waits for whole streams (a 50 MB stream holds a slot 0.5 s:
// MegabyteService) and a stream, a read, is answered at its first megabyte. What it forwards to the
// origin is still the miss share of the REQUESTS: a cost prices work here, it does not multiply
// requests. A CDN sized in requests (the default 100k/s) charges 1 per request whatever its size.
// A cache_flush (a purge of the whole edge) warms back as a cache's does (CacheBehaviour). Ignored:
// the TTL and the stale content it serves (`ttl` is accepted and not modelled), a cold edge
// warming up after a start, invalidation, ch. 1's "CDN fallback" (the clients going to the origin
// while the CDN is down: phase 3's faults), the choice of the nearest PoP (a client's links say
// which CDN it uses), the CDN's price, and, sized in bytes, the transfer time of all but a stream's
// first megabyte (see BlobStoreBehaviour). Pattern: Delegation (composition over inheritance) — a
// CDN IS the read_through shape of a cache, so CdnBehaviour holds a CacheBehaviour and forwards
// route/response/delivered to it rather than restating them. graph::Graph resolves the origin as
// the cache's backend (ResolvedNode::backend) with the read_through share, which is all
// CacheBehaviour reads. Only cost() and megabyte_service() are its own: a cache counts requests, a
// CDN may count megabytes.
class CdnBehaviour {
 public:
  CdnBehaviour(const graph::Graph& g, std::size_t self);

  // Serves every read and forwards rate × (1 − h) of it to the origin (the class comment). The
  // origin fetch is a CDN's only call, so `delivered_last_step` thins nothing; it is passed on to
  // the cache that does the routing.
  template <FractionLookup Lookup = NothingLost>
  [[nodiscard]] Routed route(const flow::Flow& in, SimTime now, SimTime dt,
                             Lookup&& delivered_last_step = {}) const {
    return cache_.route(in, now, dt, delivered_last_step);
  }

  // Capacity units one request of `op` consumes (graph::op_cost): its megabytes when the CDN is
  // sized in bytes, else 1 (a CDN has no `cost` key).
  [[nodiscard]] double cost(std::string_view op) const;

  // own + (1 − h) × the origin hop, as a hit/miss mixture (the class comment).
  [[nodiscard]] flow::LatencyEstimate response(std::string_view op, flow::LatencyEstimate own,
                                               ResponseLookup auto&& downstream) const {
    return cache_.response(op, own, downstream);
  }

  // own × (h + (1 − h) × the origin's delivered fraction).
  [[nodiscard]] double delivered(std::string_view op, double own,
                                 FractionLookup auto&& downstream) const {
    return cache_.delivered(op, own, downstream);
  }

  // h × factor from now on, a cache_flush's warm-up (CacheBehaviour::set_hit_ratio_factor): the
  // origin is sent 1 − h × factor of every read.
  void set_hit_ratio_factor(std::size_t cache, double factor) {
    cache_.set_hit_ratio_factor(cache, factor);
  }

  // How the CDN times a stream when it counts megabytes; null when it counts requests.
  [[nodiscard]] const MegabyteService* megabyte_service() const {
    return megabyte_service_ ? &*megabyte_service_ : nullptr;
  }

 private:
  CacheBehaviour cache_;
  std::map<std::string, double, std::less<>> cost_by_op_;  // every op that reaches the CDN
  std::optional<MegabyteService> megabyte_service_;        // sized in bytes only
};

// Models: the blob storage of System Design Interview (Alex Xu, vol. 1) ch. 14 ("Design YouTube":
// the original storage that keeps the uploaded videos and the transcoded storage the CDN fetches
// from) and ch. 15 ("Design Google Drive": the cloud storage, S3 in the book, that keeps the file
// blocks, and its cold storage). Spec §19 entry 2. Captured: a store whose capacity is a byte rate
// counts MEGABYTES (defaults::byte_unit). Its capacity is bytes/s ÷ 1 MB units/s (spec §6's default
// "blob 500 MB/s" is 500 units/s), a request costs its size in megabytes (graph::op_cost:
// Graph::request_bytes / 1 MB, a 50 MB upload 50 units, a 2 KB read 0.002), and its service time
// s_MB is ONE megabyte's (default 40 ms: the book's "read 1 MB sequentially from disk", 30 ms, plus
// "... from the network", 10 ms), so Little's law gives c = 500/s × 40 ms = 20 megabyte slots.
// Nobody it calls is waited for: without `then` steps it is a leaf, as a database is, and its
// `then` steps are event notifications nobody waits for (below). The batch view: each request is a
// BATCH of megabytes that holds one slot for its whole transfer, MB_o × s_MB (a 50 MB object 2 s).
// That time is DETERMINISTIC (a fixed size at a fixed rate per slot: per-object SCV cs² = 0,
// defaults::default_service_variability), so the spread of service times is the size mix's alone,
// and the queue is M/G/c over objects, with c slots, the mixture's moments
// (MegabyteService::object_time_moments) and Kingman's correction to Erlang C (Allen–Cunneen):
//   demand   = Σ_op rate_op × MB_op           units (MB) per second; rho = demand / capacity
//   p_op     = rate_op / Σ rate,  s_op = MB_op × s_MB
//   s_obj    = E[S] = Σ_op p_op s_op                            the mean time one object holds
//   rho      = (Σ rate) × s_obj / c = demand / capacity         a slot (Little's law: the same
//                                                               rho as in megabytes)
//   c_s²     = Σ_op p_op (1 + cs²) s_op² / s_obj² − 1           the size mix's SCV
//   Wq       = (c_a² + c_s²) / 2 × Erlang C(c, rho) × s_obj / (c (1 − rho))
//                                                               M/M/c's mean wait (Erlang C)
//                                                               scaled by Allen–Cunneen
//   Wq      <= queue_limit / capacity                           the room-drain bound, the
//                                                               fluid queue's (fluid::wait):
//                                                               exact for a deterministic
//                                                               drain, conservative otherwise
// One size at Poisson arrivals (c_a² = 1, c_s² = 0) waits (1 + 0) / 2 = half the M/M/c wait: at
// rho 0.9, 50 MB objects at a default store wait 0.5 × C(20, 0.9) × 2 s / (20 × 0.1) = 0.28 s,
// where exponential transfers would wait 0.55 s and 1 MB units would say 0.5508 × 40 ms / 2 =
// 11 ms. Small requests among large ones raise c_s² (1000 reads/s of 2 KB beside 8 writes/s of
// 50 MB: 124), so they lengthen the large ones' wait, as they should, where a mean object alone
// would shrink it. Past saturation the wait is the waiting room's drain time: 1000 MB at 500 MB/s,
// 2 s, not the 4.74 s the engine's Erlang C term gives at its 0.99 clamp (half of plain Erlang C's
// 9.48 s, by the Allen–Cunneen factor of deterministic transfers).
// When a request is answered depends on its direction (MegabyteService::sojourn):
//   a write (an op in `write_ops`, default `write`)   response = Wq + MB_o × s_MB, its LAST
//                                                     byte: a PUT is acknowledged once stored (a
//                                                     300 MB upload at 40 ms per MB: 12 s + Wq)
//   a read (every other op)                           response = Wq + s_MB, its FIRST megabyte:
//                                                     the client reads the rest as it streams in,
//                                                     overlapping the transfer
// The answer adds no spread of its own (cs² = 0): the response's variance is the wait's. So every
// 300 MB upload takes at least 12 s at 25 MB/s per slot (40 ms per MB), and a client `timeout`
// under 12 s cuts all of them (youtube's clients wait 30 s, a margin for the book's resumable
// uploads; at `timeout: 10s` every upload times out). Both answers are at least one
// megabyte's time: a 2 KB request holds its slot only 0.002 × 40 ms = 80 µs (it uses the capacity
// of its bytes) but is answered in the whole 40 ms of one unit (an upper bound for small objects: a
// real store answers it in about a disk seek). The overlap argument holds for reads and streams
// only, and it understates a large read that the client needs whole before it can act (a 1 GB file
// downloaded to be opened takes 40 s, not 40 ms). The capacity counts every megabyte, so large
// objects queue everyone behind them. Ignored: durability (replication across regions, erasure
// coding: the store never loses an object and never fails), multipart and resumable uploads (an
// object is one request), lifecycle rules (moving old objects to cold storage), the jitter of real
// disks and networks (seeks, retransmissions: `advanced.service_variability` puts a per-object SCV
// back). Event notifications (spec §19 entries 6-7): its `then` steps are S3 event notifications,
// the way the book's event-driven pipelines start: ch. 14's "transcoding starts when the upload
// completes" (the original storage tells the transcoding queue), ch. 15's upload that notifies the
// metadata service. Every WRITE the store SERVED (an op in its `write_ops`, default `[write]`:
// youtube's original_store lists `upload`; not one it dropped: the engine routes what the queue
// drained) fires every step, rate × factor of the step's op. A read fires nothing: S3 notifies on
// ObjectCreated and ObjectRemoved, never on a GET, so 1000 views of a video start no transcode
// (graph::Graph::then_fraction is 0 for a read; notifying on reads is phase 3). A same-op step
// keeps the request's size, a renamed one carries its `size:` or none (spec §19 entry 1). Nobody
// waits for an event (graph::Graph::is_async_hop): the uploader's response and delivered share end
// at the store, as a write_back cache's flush is not waited for, and what the notified node drops
// counts in that node's drop rate, never in the uploader's answered rate. A step can be neither
// `on: miss` nor `on: failure` nor in a `mode` (graph::Graph::build and the loader refuse them: an
// event never misses, follows no step a failure step could report on, and is waited for by nobody).
// Ignored: event batching and the delivery delay (S3 sends an event within seconds, the model after
// the link's latency), failed or duplicated notifications (S3 delivers at least once and may
// retry), and filters by key prefix.
class BlobStoreBehaviour {
 public:
  BlobStoreBehaviour(const graph::Graph& g, std::size_t self);

  // Serves every incoming op and sends rate × factor of each `then` step's op for each write it
  // served (the class comment's events; a read sends nothing). `now` and `dt` are unused (a store
  // holds no state between steps).
  [[nodiscard]] Routed route(const flow::Flow& in, SimTime /*now*/, SimTime /*dt*/) const {
    Routed routed = detail::serve_everything(in);
    for (const auto& [op, rate] : routed.served.entries()) {
      events_.forward(op, rate, routed.outgoing, NothingLost{});
    }
    detail::sort_by_destination(routed.outgoing);
    return routed;
  }
  // The engine's call (see NodeBehaviour): every event is asynchronous, so none fails the request
  // and last step's losses thin none of the events after it.
  [[nodiscard]] Routed route(const flow::Flow& in, SimTime now, SimTime dt,
                             FractionLookup auto&& /*delivered_last_step*/) const {
    return route(in, now, dt);
  }

  // Capacity units one request of `op` uses (graph::op_cost): its megabytes, or 1 for a store
  // whose capacity the scenario states in requests (`capacity: 500/s`).
  [[nodiscard]] double cost(std::string_view op) const;

  // Its own sojourn: a blob store waits for nobody (its events are asynchronous).
  [[nodiscard]] flow::LatencyEstimate response(std::string_view /*op*/, flow::LatencyEstimate own,
                                               ResponseLookup auto&& /*downstream*/) const {
    return own;
  }

  // The share of the op's requests the store answers itself: an event the notified node loses
  // fails nobody's request.
  [[nodiscard]] double delivered(std::string_view /*op*/, double own,
                                 FractionLookup auto&& /*downstream*/) const {
    return own;
  }

  // How the store times an object (the class comment); null for a store whose capacity the
  // scenario states in requests, which counts and times requests like any server.
  [[nodiscard]] const MegabyteService* megabyte_service() const {
    return megabyte_service_ ? &*megabyte_service_ : nullptr;
  }

 private:
  std::map<std::string, double, std::less<>> cost_by_op_;  // every op that reaches the store
  std::optional<MegabyteService> megabyte_service_;        // sized in bytes only
  // Per write op that can reach the store, one block per `then` step, every hop asynchronous
  // (graph::Graph::is_async_hop): only HopBlocks::forward is ever asked of them. A read op has no
  // blocks, so it forwards nothing.
  detail::HopBlocks events_;
};

// Models: the message queue of System Design Interview (Alex Xu, vol. 1) ch. 1 ("Message queue": a
// durable buffer that lets a producer publish work and a consumer process it later, so the two
// scale and fail apart), ch. 10's notification system (one queue per channel, drained by its
// workers: the book's queue-plus-workers design) and the queues of ch. 14 (YouTube's transcoding
// queue) and ch. 15 (Google Drive's notification queue), run in practice as Kafka. Spec §19 entries
// 4 and 6. Captured: the node is the BROKER. Its own queue is the append (spec §6: 1M messages/s,
// 100 µs chosen, so c = 1M/s × 100 µs = 100 by Little's law), and a publisher is answered when its
// message is appended: the response is the append's sojourn alone and the delivered share the
// broker's own, whatever happens to the message later (the hops to the consumers are asynchronous
// by design, graph::Graph::is_async_hop). Each appended message then goes to EVERY consumer group,
// a worker pool that `consumes` the queue: Kafka's consumer groups each read the whole log, so the
// broker's outflow is the sum of its groups' (two groups on 1k messages/s receive 1k/s each, not
// 500), and a message keeps its op name. What a group has not read waits in that GROUP's pool
// queue, whose room is the queue's retention × the pool's capacity
// (graph::Graph::connect_consumer): the backlog of an append-only log, held per reader. Its queue
// lag, how far its slowest group is behind, is read off those pools by the engine
// (engine::NodeSample::queue_lag_s). Ignored: ordering (partitions are printed, not modelled),
// delivery guarantees and redelivery (at-least-once, a consumer that crashes before committing
// reads a message again), the consumer's commit interval, the broker's disk (an append costs the
// type's 100 µs whatever the message's size, and the log never fills a disk) and its replication.
class MessageQueueBehaviour {
 public:
  MessageQueueBehaviour(const graph::Graph& g, std::size_t self);

  // Appends every message it is sent (`served` is the input, nothing is refused) and sends each
  // op on, whole, to every consumer group whose link carries it (an op the graph never saw arrive
  // has no group: it is appended and read by nobody). `now` and `dt` are unused: the broker keeps
  // no state between steps (the backlog is the pools').
  [[nodiscard]] Routed route(const flow::Flow& in, SimTime now, SimTime dt) const;
  // The engine's call (see NodeBehaviour): every consumer group gets a copy of each message,
  // asynchronously, so no hand-off follows another and nothing is thinned.
  [[nodiscard]] Routed route(const flow::Flow& in, SimTime now, SimTime dt,
                             FractionLookup auto&& /*delivered_last_step*/) const {
    return route(in, now, dt);
  }

  // One capacity unit per message, whatever its op or size: an append.
  [[nodiscard]] double cost(std::string_view /*op*/) const { return 1.0; }

  // The append's sojourn: the publisher waits for nobody downstream.
  [[nodiscard]] flow::LatencyEstimate response(std::string_view /*op*/, flow::LatencyEstimate own,
                                               ResponseLookup auto&& /*downstream*/) const {
    return own;
  }

  // The share the broker appended: a message a consumer group later loses fails no publisher.
  [[nodiscard]] double delivered(std::string_view /*op*/, double own,
                                 FractionLookup auto&& /*downstream*/) const {
    return own;
  }

 private:
  // One consumer group that reads an op: the pool, and the link its messages travel on.
  struct Consumer {
    std::size_t target;
    std::size_t link;
  };

  // Per op that reaches the queue (graph::Graph::arriving_ops), the groups whose link carries it,
  // from graph::Graph::routes_for, the source the graph's asynchronous deliveries are built from
  // too (detail::consumer_deliveries): a drained link (weight 0, share 0) is no group.
  std::map<std::string, std::vector<Consumer>, std::less<>> consumers_by_op_;
};

// Models: the workers behind a queue in System Design Interview (Alex Xu, vol. 1): ch. 10's
// notification workers (the book's queue-plus-workers design: each channel's queue drained by its
// own workers, which call the third-party push service), ch. 14's transcoding servers, ch. 11's
// fan-out workers, ch. 15's notification and backup workers, the consumers of ch. 1's message
// queue. Spec §19 entry 5, spec §7: "capacity = workers / job cost". Captured: the pool is ONE
// M/M/c queue over consumption, its c = `workers` and its service time = `cost` (the time one
// worker spends on one job), so its rate is workers / cost (400 transcoders at 60 s a video: 6.667
// videos/s); one job is one capacity unit. Its waiting room is what its queue keeps for it,
// retention × that rate (graph::Graph::connect_consumer), so a backlog grows at inflow − rate while
// the pool is saturated, and what overflows the room is deleted unconsumed: the pool's drops, which
// no publisher sees (the hop from the queue is asynchronous). Its response and delivered share are
// its own: nothing upstream waits for it. Its `then` steps are the work a finished job hands on
// (spec §19 entry 5; ch. 14's transcoder writes its renditions to the transcoded store and
// publishes a completion message): every step fires for every job the pool DRAINED (the engine
// routes what the queue served, so a saturated pool hands on its capacity, not its arrivals, and
// the excess starts nothing until it is drained), `factor` requests of the step's op each, whatever
// op the queue brought the job under. A worker is busy for `cost` only: its hand-offs do not hold
// it, so a slow target leaves the pool's rho and sojourn as they are, and the target's own queue
// absorbs (or drops) the load. The hops are calls from the pool's side (graph::Graph::is_async_hop
// is false for them), but no one waits for the pool, so they add nothing to any client's response
// and fail no client's request (a sequential or parallel group alike); what a target drops counts
// in its own drop rate. Ignored: a worker blocked on a slow downstream call (an external sink's 150
// ms would hold a worker and cut the pool's real capacity: phase 3), autoscaling (`workers` is
// fixed), poison messages that fail and are retried forever, and the job-time spread of real work
// (the service SCV is the exponential's 1 unless `advanced.service_variability` says otherwise).
//
// Retries and dead letters (phase 3 spec §6.2; book ch. 10, "Design a notification system": a
// worker whose call to APNs, FCM or an SMS gateway fails puts the notification back on the queue
// with a retry count and tries again later, and one that has run out of retries goes to a
// dead-letter queue, which the on-call team watches). A pool with `retries: n` puts the jobs whose
// call to an external sink failed back on its OWN queue `retry_after` later (b = retry_after /
// step rounded to whole steps, at least one), up to n times. The re-published jobs go through the
// queue like new ones: the queue's demand, the pool's demand, rho and wait, the queue's lag and the
// sink's demand all include them. In a fluid model that is feedback demand, as a client's retries
// are (ClientBehaviour): with f the share of the jobs whose sink call fails (1 − d, d the sink's
// delivered fraction this step; 1 − Π d over the sink calls of a pool that calls several) and
// R_k(t) the jobs of attempt k (k = 1..n) re-published at step t,
//   R_1(t + b) = F(t) × f                 F(t): the jobs of their first attempt drained at t
//   R_k(t + b) = R_(k−1)(t) × f           k = 2..n: the share of attempt k − 1 that failed again
//   dead(t)    = R_n(t) × f               attempt n failed too: a dead letter (n = 0: none)
// so every job is attempted 1 + f + f² + ... + f^n = (1 − f^(n+1)) / (1 − f) times (a geometric
// series cut after n + 1 terms: attempt k happens only if the k earlier ones failed, probability
// f^k when every attempt fails independently with the same f) and is dead-lettered with
// probability f^(n+1). At 1000 jobs/s, f = 0.02 and n = 3 the sink is called 1020.408 times a
// second, fails 20.408 of them (its own drops, as before) and 1000 × 0.02⁴ = 1.6e-4 jobs a second
// are dead letters: answered + dead-lettered = the jobs published.
// Why a dead letter is not a drop: nothing overflowed and no node failed it for good; the job is
// parked where an operator can read it, replay it or give up on it (a dead-letter queue keeps it),
// which is a decision, not a loss the system made. So it is counted apart, in the pool's
// dead_lettered_per_s, and the sink's failures that a pool retries are not losses either
// (report::Summary leaves them out of the drop rate), while the sink's own books keep every failed
// call as a drop, so its conservation does not change.
// Ignored: exponential backoff (every retry waits the same `retry_after`), jitter, a retry when the
// pool itself is overloaded (only a failed sink call is retried; a job its queue's room drops is
// gone), the jobs a saturated pool leaves waiting (a re-published job not drained in the step it
// arrives is counted as a first attempt when it is, and gets its retries again), and a sink that
// overflows while the pool retries: f comes from the sink's delivered fraction, which mixes its
// quota's overflow with its failures, so a pool retries a call the sink shed for being too busy as
// it retries a 5xx (both make the provider say no), and its own retries load that quota further. A
// job lost at a `then` step before the sink's is that step's target's drop and is not retried (f
// is read off the sink calls alone). Re-published to the queue, a job is read again by every
// consumer group of it, as Kafka's groups each read the whole log (a design with a retry topic per
// group is not modelled).
class WorkerPoolBehaviour {
 public:
  WorkerPoolBehaviour(const graph::Graph& g, std::size_t self);

  // Serves every job it is sent (`in` is what the engine's queue drained this step) and sends
  // rate × factor of each `then` step's op for all of them, whatever their ops (the class
  // comment), each step only the jobs the steps before it delivered last step
  // (detail::HopBlocks::forward: a completion message is not published for a rendition the store
  // lost). `now` and `dt` are unused: routing keeps no state between steps (the backlog is its
  // queue's; the retries in flight are kept by note_jobs and retry_failed, after the route). The
  // template head as in ServiceBehaviour::route.
  template <FractionLookup Lookup = NothingLost>
  [[nodiscard]] Routed route(const flow::Flow& in, SimTime /*now*/, SimTime /*dt*/,
                             Lookup&& delivered_last_step = {}) const {
    Routed routed = detail::serve_everything(in);
    const Rate jobs = routed.served.total();
    // No empty branches (see ServiceBehaviour::route): an idle pool sends nothing on.
    if (jobs.per_second > 0.0) {
      hand_offs_.forward(every_job, jobs, routed.outgoing, delivered_last_step);
    }
    detail::sort_by_destination(routed.outgoing);
    return routed;
  }

  // One capacity unit per job: a job takes one worker for `cost`, whatever its op.
  [[nodiscard]] double cost(std::string_view /*op*/) const { return 1.0; }

  // Its own sojourn (wait for a worker + cost): nobody waits for its hand-offs, and nobody asks
  // (the queue's hop into the pool is asynchronous, so no caller looks this up through a call).
  [[nodiscard]] flow::LatencyEstimate response(std::string_view /*op*/, flow::LatencyEstimate own,
                                               ResponseLookup auto&& /*downstream*/) const {
    return own;
  }

  // The share of the jobs it does not drop: a hand-off its target loses fails no job.
  [[nodiscard]] double delivered(std::string_view /*op*/, double own,
                                 FractionLookup auto&& /*downstream*/) const {
    return own;
  }

  // The pool's `retries` n: how many more times it calls a sink for a job whose call failed (0:
  // never, phase 2's pool, which keeps no retry state at all).
  [[nodiscard]] int retries() const { return retries_; }

  // What the pool puts back on its queue this step (the class comment's Σ R_k(t)), with the ops the
  // queue gave the jobs: the engine adds it to the queue's arrivals. Empty without retries.
  [[nodiscard]] flow::Flow republished() const;

  // The jobs the pool drained this step (`routed.served`, per op) and the jobs that arrived at it
  // (its inflow): what retry_failed splits into attempts. A no-op without retries.
  void note_jobs(const flow::Flow& drained, Rate arrived);

  // The share f of this step's jobs whose call to an external sink failed: 1 − Π d over the pool's
  // sink calls (the class comment), each d read through `delivered` (the engine passes this step's
  // fractions, or estimate()'s damped ones). 0 for a pool that calls no sink.
  [[nodiscard]] double failed_share(FractionLookup auto&& delivered) const {
    double every_call_answers = 1.0;
    for (const SinkCall& call : sink_calls_) every_call_answers *= delivered(call.target, call.op);
    return 1.0 - every_call_answers;
  }

  // After the step: the jobs noted this step whose sink call failed (`failed_share` of each
  // attempt but the last) are re-published `retry_after` later as their next attempt, and the
  // last attempt's failures are this step's dead letters. A no-op without retries.
  void retry_failed(double failed_share);

  // The jobs whose last attempt failed in the latest retry_failed, per second: 0 without retries.
  [[nodiscard]] double dead_lettered_per_s() const { return dead_lettered_per_s_; }

  // Every retry in flight forgotten, each one due at the next evaluation and no delay to the pool:
  // what estimate()'s scratch copy needs, where every pass is the same instant (as
  // ClientBehaviour::settle_retries).
  void settle_retries();

 private:
  // The key the `then` blocks are filed under: one set of blocks for every job, since every step
  // fires for every job whatever its op (graph::Graph::then_fraction is 1 at a pool) and none of
  // its hops is asynchronous (see the constructor). A key, never an op: nothing is asked about it.
  static constexpr std::string_view every_job = "";

  // One call of a `then` step to an external sink: the calls whose failures the pool retries.
  struct SinkCall {
    std::size_t target;
    std::string op;
  };

  // The re-published jobs in flight. Attempt k's jobs (k = 1..n, index k − 1) re-published in each
  // step from `delivery` steps ago (still on their way from the queue to the pool, the link's
  // delay) to `backoff` steps ahead (due later): a ring of backoff + delivery + 1 slots, each the n
  // attempts' flows. The current slot is what the queue is sent this step; the one `delivery` steps
  // back is what reaches the pool this step; publish_later fills the one `backoff` ahead (the slot
  // the oldest step just left) and moves the ring on one step.
  // Pattern: Circular buffer, like engine::DelayLine: nothing is shifted as the steps go by.
  class RepublishRing {
   public:
    RepublishRing(int retries, int backoff_steps, int delivery_steps);
    [[nodiscard]] flow::Flow due() const;
    [[nodiscard]] double arriving(std::size_t attempt) const;  // per second, attempt index k − 1
    void publish_later(std::vector<flow::Flow> attempts);

   private:
    [[nodiscard]] std::size_t ahead(std::size_t steps) const;
    std::size_t backoff_;
    std::vector<std::vector<flow::Flow>> slots_;
    std::size_t now_{0};
  };

  void start_retries(int backoff_steps, int delivery_steps);

  // One block per `then` step (built in the constructor), only ever forwarded.
  detail::HopBlocks hand_offs_;
  std::vector<SinkCall> sink_calls_;
  int retries_{0};
  int backoff_steps_{1};               // retry_after / step, at least one (detail::backoff_steps)
  int delivery_steps_{0};              // the delay of the link from its queue, in steps
  std::optional<RepublishRing> ring_;  // empty without retries
  flow::Flow drained_{};               // note_jobs: this step's jobs
  double arrived_per_s_{};
  double dead_lettered_per_s_{};
};

// Models: a third party's API, which our system calls and does not run: the providers of System
// Design Interview (Alex Xu, vol. 1) ch. 10's notification system (Apple's APNs, Google's FCM, an
// SMS gateway, an e-mail service), which the book's workers call for every message they send, and
// chat-system's push gateway (ch. 12). Spec §19 entry 8, spec section 6 `third_party_api`.
// Captured: the node is a LEAF (it answers the request itself and calls nobody: the loader refuses
// `then` and `ops` on it) whose queue has the type's numbers (simsys.defaults: capacity 1k/s,
// chosen as the quota a provider grants one account; service time 100 ms and service SCV 2.0, spec
// section 6: slow and more erratic than the exponential), and a share `failure_rate` f of what that
// queue SERVES fails, a 5xx from the provider. The ENGINE counts the failures
// (engine::Simulation::serve moves f × served from the node's drain to its drops), so they show in
// the node's drop rate like a full queue's overflow, and the `own` share it hands `delivered`
// already lacks them: this behaviour forwards nothing, composes nothing and must not apply f a
// second time. A failure is the provider failing, not a policy working: nothing is ever counted as
// rejected. A client's `retries` send a failed request again (ClientBehaviour), and so do a worker
// pool's (WorkerPoolBehaviour: the job goes back on the pool's queue), which this queue sees as
// more demand. Ignored: the provider's latency rising with our load (its queue is our account's
// quota, not its fleet),
// failures that cost a different time than a success, and `politeness` (printed, not modelled: it
// needs per-host state, phase 3).
class ExternalSinkBehaviour {
 public:
  ExternalSinkBehaviour(const graph::Graph& g, std::size_t self);

  // Serves every incoming op and forwards nothing: a provider calls nobody back. `in` is what the
  // engine's queue drained, failures already taken out; `now` and `dt` are unused (a sink holds no
  // state between steps).
  [[nodiscard]] Routed route(const flow::Flow& in, SimTime now, SimTime dt) const;
  // The engine's call (see NodeBehaviour): a leaf calls nobody, so there is nothing to thin.
  [[nodiscard]] Routed route(const flow::Flow& in, SimTime now, SimTime dt,
                             FractionLookup auto&& /*delivered_last_step*/) const {
    return route(in, now, dt);
  }

  // The capacity units one request of `op` uses (graph::op_cost, as every behaviour and explain's
  // paper load price it): 1 for every op of a scenario file, whose sinks take no `cost` key.
  [[nodiscard]] double cost(std::string_view op) const;

  // The share f of what the sink serves that fails: the engine reads it to move those requests
  // from the node's drain to its drops (engine::Simulation::serve).
  [[nodiscard]] double failure_rate() const { return failure_rate_; }

  // Its own sojourn: a failed request takes as long as an answered one, and nothing is downstream.
  [[nodiscard]] flow::LatencyEstimate response(std::string_view /*op*/, flow::LatencyEstimate own,
                                               ResponseLookup auto&& /*downstream*/) const {
    return own;
  }

  // The engine's own share, 1 − (dropped + rejected) / inflow, which already counts the failures
  // as drops: returned unchanged, or each failure would be charged twice ((1 − f)² for 1 − f).
  [[nodiscard]] double delivered(std::string_view /*op*/, double own,
                                 FractionLookup auto&& /*downstream*/) const {
    return own;
  }

 private:
  std::map<std::string, double, std::less<>> cost_by_op_;
  double failure_rate_{};
};

// One kind of machine of a kv_cluster this step, as the engine served it (engine::Simulation::serve
// hands one per copy group): the sojourn ONE replica-request sees on one such machine, the share of
// the replica-requests sent to those machines that they did not drop, and how many of them are
// down (a node_down of some of the cluster's machines: engine::Condition::machines_down; always 0
// on the hot machine, see KvClusterBehaviour).
struct MachineGroup {
  flow::LatencyEstimate sojourn{};
  double kept{1.0};
  int down{0};
  // C++20 defaulted operator==: member-wise equality generated by the compiler (and `!=` by C++20's
  // rewriting), replacing two hand-written operators that must list every member. It is what lets
  // KvClusterBehaviour::observe see that this step's machines are last step's.
  bool operator==(const MachineGroup&) const = default;
};

// Models: the key-value store of System Design Interview (Alex Xu, vol. 1) ch. 6, Dynamo-style
// (Amazon's Dynamo, Cassandra), in the steady state (spec §19 entry 9; the quorum over the real
// replica set). `nodes` machines sit on a consistent-hash ring (ch. 5); every key lives on N =
// `replication` of them; the coordinator sends each read and each write to all N replicas and
// answers a write once W = `write_quorum` have acknowledged it and a read once R = `read_quorum`
// have replied (ch. 6, "quorum consensus": W + R > N makes a read meet the latest write; W = 1 or R
// = 1 trades that for speed). Captured, by four pieces:
//  - the machines: graph::apply_kv_ring makes the node `nodes` copies of `capacity` each, and,
//    when N < nodes, a hot machine carrying k times the average (graph::ring_imbalance, the
//    balancer's ring heuristic), which the engine runs as hot and cool copy groups;
//  - the fan-out: every request is N replica-requests, so cost() is N (defaults::op_cost) and the
//    node's demand, drain, drops and backlog count replica-requests (30k requests/s at N = 3 are
//    90k/s on the machines);
//  - the replica set: a key's N replicas are N DIFFERENT machines, so at most one of them is the
//    hot machine. It holds the keys whose sets include it, and is sent one replica-request per such
//    request: k/M of the N λ replica-requests, so a request includes it with probability
//      p_hot = N k / M        (at most 1: k <= M / N)
//    and otherwise reaches N cool machines;
//  - the quorum: a read is answered at the R-th fastest reply of its set, a write at the W-th (the
//    ops of model::write_ops_of, `write` by default), each machine's sojourn a lognormal fitted to
//    its own copy group's mean and variance (queueing::lognormal_order_statistic, which integrates
//    the survival function). A request is LOST when fewer than q of its N replica-requests come
//    back, each kept with its machine's kept share (queueing::probability_fewer_than, the
//    Poisson-binomial), so the loss mixes the two kinds of set by p_hot. The response is the
//    latency of the ANSWERED requests (what a client times), so it mixes them by the share each
//    kind of set answers, p_hot (1 − L_hot) and (1 − p_hot)(1 − L_cool), over the share answered
//    (law of total variance): a set that loses more weighs less. So
//    a quorum of 2 of 3 answers from the two cool replicas when the hot machine is slow or drops,
//    and a quorum of all 3 waits for it and shares its losses;
//  - quorum loss under machine failure (ch. 6, "handling failures"; a scripted node_down of r of
//    the M machines, MachineGroup::down): the r down machines are cool ones (the hot machine, the
//    one the utilization SLO reads, stays up), and a set's cool slots are drawn from the C cool
//    machines (C = M − 1 on an uneven ring, M on an even one), so the share of the sets of s cool
//    slots that hold j down machines is hypergeometric,
//      P(j) = C(r, j) C(C − r, s − j) / C(C, s)    (one down machine is in s / C of the sets, two
//                                                   in s (s − 1) / (C (C − 1)))
//    and such a set has N − j live replicas. A request on it is LOST when N − j is below its quorum
//    q (strict quorum: the coordinator asks no other machine), and otherwise answered at the q-th
//    fastest of its N − j live replies, which waits longer than the q-th of N (fewer replies to
//    pick the fastest from): the loss and the latency mix over j as over the hot and cool sets.
//    The engine sends nothing to a down machine and leaves the survivors their own load
//    (engine::Simulation::serving_copies).
// `mode` (ap or cp) is carried for explain only. Ignored: network partitions and what ap or cp does
// under them, sloppy quorum and hinted handoff (a dead machine's replicas are not taken over by the
// next machines on the ring, so the quorum is lost rather than met elsewhere), read repair and
// anti-entropy
// (Merkle tree) traffic, vector clocks and conflict resolution; the replicas' times taken as
// independent (one hot queue serves many keys) and lognormal (an M/M/c sojourn is not exactly one);
// the spread among the cool machines (they are one group); the latency of a request that lost a
// replica but still met its quorum (taken as if all N replied: its quorum would wait for a later
// reply; a Monte Carlo of the 8-machine lab puts the cost at +1.6 % on the mean and about +6 ms on
// the p99). The coordinator's own hop is a node of the scenario, if it is wanted (the kv-store
// lab's `coordinator` service).
class KvClusterBehaviour {
 public:
  KvClusterBehaviour(const graph::Graph& g, std::size_t self);

  // Serves every incoming op and forwards nothing: the replicas answer the coordinator, which
  // this node includes. `now` and `dt` are unused (the quorums come from observe()).
  [[nodiscard]] Routed route(const flow::Flow& in, SimTime now, SimTime dt) const;
  // The engine's call (see NodeBehaviour): a leaf calls nobody, so there is nothing to thin.
  [[nodiscard]] Routed route(const flow::Flow& in, SimTime now, SimTime dt,
                             FractionLookup auto&& /*delivered_last_step*/) const {
    return route(in, now, dt);
  }

  // N capacity units per request (graph::op_cost): one per replica the request is sent to.
  [[nodiscard]] double cost(std::string_view op) const;

  // This step's machines, in engine::Simulation::make_server's order: the hot machine first and
  // then the cool ones when the ring is uneven, else one group of identical machines. Recomputes
  // both quorums' answers, which response() and delivered() return until the next step, unless
  // the machines are exactly last step's (memoised: a quadrature per quorum is the cost of a step,
  // and a cluster under a steady load sees the same machines step after step).
  void observe(std::span<const MachineGroup> machines);

  // The op's quorum over its real replica set (class comment), from the last observe(). `own`,
  // the copies' request-weighted sojourn, is not used: a quorum is not one averaged replica.
  [[nodiscard]] flow::LatencyEstimate response(std::string_view op, flow::LatencyEstimate /*own*/,
                                               ResponseLookup auto&& /*downstream*/) const {
    return answer_of(op).latency;
  }

  // The share of the op's requests whose quorum was met (class comment). `own`, the share of the
  // replica-requests kept, is not used: a request needs q of its N, not all of them.
  [[nodiscard]] double delivered(std::string_view op, double /*own*/,
                                 FractionLookup auto&& /*downstream*/) const {
    return answer_of(op).delivered;
  }

 private:
  // A request's replicas: on the hot machine (0 or 1 of them) and on cool machines (the rest).
  struct ReplicaSet {
    int hot;
    int cool;
  };
  // What a quorum gives the clients this step.
  struct QuorumAnswer {
    flow::LatencyEstimate latency{};
    double delivered{1.0};
  };
  [[nodiscard]] const QuorumAnswer& answer_of(std::string_view op) const;
  [[nodiscard]] QuorumAnswer answer_quorum(int quorum) const;
  [[nodiscard]] double share_with_down(int cool_slots, int down_in_set) const;
  [[nodiscard]] flow::LatencyEstimate reply_time(ReplicaSet set, int quorum) const;
  [[nodiscard]] double loss(ReplicaSet set, int quorum) const;

  std::map<std::string, double, std::less<>> cost_by_op_;
  std::vector<std::string> write_ops_;
  int replication_{};
  int write_quorum_{};
  int read_quorum_{};
  double hot_share_{};   // p_hot: the share of the requests whose replicas include the hot machine
  int cool_machines_{};  // C: the machines a set's cool slots are drawn from (M − 1, or M if even)
  MachineGroup hot_{};
  MachineGroup cool_{};
  bool answered_{false};  // the answers below are hot_'s and cool_'s: observe() has run once
  QuorumAnswer read_answer_{};
  QuorumAnswer write_answer_{};
};

// Pattern: Strategy — the engine holds one Behaviour per node and dispatches with
// std::visit; a std::variant instead of an abstract base keeps every behaviour a plain
// value (no heap allocation, no virtual calls, copyable). Phase 2a appended the load
// balancer, the rate limiter and the cache, phase 2b the SQL database and the CDN, phase 2c the
// blob store, the message queue, the worker pool, the external sink and the kv cluster; later
// phases append the rest (ws_gateway, ...).
// C++17 std::variant: a closed sum type (a type-safe union that knows which alternative
// it holds) dispatched with std::visit. Closed: adding an alternative means editing this
// line, and every std::visit that does not handle it then fails to compile.
using Behaviour = std::variant<ClientBehaviour, ServiceBehaviour, LoadBalancerBehaviour,
                               RateLimiterBehaviour, CacheBehaviour, SqlDbBehaviour, CdnBehaviour,
                               BlobStoreBehaviour, MessageQueueBehaviour, WorkerPoolBehaviour,
                               ExternalSinkBehaviour, KvClusterBehaviour>;

}  // namespace simsys::behaviours

namespace simsys::behaviours {

// Checked here as well as in the tests: a behaviour that drifts from the engine's
// contract breaks this module's build, not only a test's.
static_assert(NodeBehaviour<ServiceBehaviour>);
static_assert(NodeBehaviour<LoadBalancerBehaviour>);
static_assert(NodeBehaviour<RateLimiterBehaviour>);
static_assert(NodeBehaviour<CacheBehaviour>);
static_assert(NodeBehaviour<SqlDbBehaviour>);
static_assert(NodeBehaviour<CdnBehaviour>);
static_assert(NodeBehaviour<BlobStoreBehaviour>);
static_assert(NodeBehaviour<MessageQueueBehaviour>);
static_assert(NodeBehaviour<WorkerPoolBehaviour>);
static_assert(NodeBehaviour<ExternalSinkBehaviour>);
static_assert(NodeBehaviour<KvClusterBehaviour>);
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

// What a leaf (a database, a blob store, an external sink) does with a step's flow: serves all of
// it, forwards nothing, rejects nothing.
[[nodiscard]] Routed serve_everything(const flow::Flow& in) {
  Routed routed;
  for (const auto& [op, rate] : in.entries()) {
    if (rate.per_second <= 0.0) continue;  // no empty entries (see ServiceBehaviour::route)
    routed.served.add(op, rate);
  }
  return routed;
}

// The price of one request of every op that reaches `self` (graph::op_cost), resolved once so
// cost() is one lookup per op and step.
[[nodiscard]] std::map<std::string, double, std::less<>> arriving_op_costs(const graph::Graph& g,
                                                                           std::size_t self) {
  std::map<std::string, double, std::less<>> costs;
  for (const auto& op : g.arriving_ops(self)) costs.emplace(op, graph::op_cost(g, self, op));
  return costs;
}

// The price of `op` from such a table: 1 for an op not in it (one that never reaches the node).
[[nodiscard]] double cost_in(const std::map<std::string, double, std::less<>>& costs,
                             std::string_view op) {
  const auto found = costs.find(op);
  return found == costs.end() ? 1.0 : found->second;
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

// ---- detail::HopBlocks ----

std::span<const detail::Block> detail::HopBlocks::blocks_of(std::string_view op) const {
  const auto found = blocks_by_op_.find(op);
  if (found == blocks_by_op_.end()) return {};
  return found->second;
}

// The miss fraction 1 − h × factor (MissesOf): written so that factor 1 gives 1 − h exactly, the
// share Graph::build resolved, and a warm cache routes bit for bit as before its flush.
void detail::HopBlocks::warm(std::size_t cache, double factor) {
  // C++20 std::views::values: a lazy view of each map entry's value (an op's blocks), replacing
  // a loop over the pairs that ignores `.first`.
  for (auto& blocks : blocks_by_op_ | std::views::values) {
    for (Block& block : blocks) {
      if (block.misses && block.misses->cache == cache) {
        block.fraction = 1.0 - block.misses->hit_ratio * factor;
      }
    }
  }
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
  return detail::cost_in(cost_by_op_, op);
}

// The graph's resolved hops (graph::Graph::service_hops), minus those no request takes: an
// `on: miss` step after a cache that always hits. Such a hop forwards nothing and adds nothing, so
// it is left out rather than carried as a zero branch. Each unconditional hop opens a block of its
// own, and so does each `on: failure` hop, anchored at the block of the step it reports on (the
// group's first member if that step is in the parallel group: detail::HopBlocks reports on the
// group as a whole); a miss hop joins the block of the miss hops of the same lookup before it
// (detail::Block), failure blocks between them notwithstanding.
void ServiceBehaviour::add_hops(const graph::Graph& g, const std::string& op) {
  using detail::Block;
  auto& blocks = hops_.blocks_for(op);
  const auto resolved_hops = g.service_hops(self_, op);
  // Which block each resolved hop went into: what a failure hop's `reports_on` (a hop position)
  // becomes. Unconditional hops, the only ones a failure hop reports on, are never left out.
  std::vector<std::size_t> block_of_hop(resolved_hops.size());
  for (std::size_t position = 0; position < resolved_hops.size(); ++position) {
    const graph::ServiceHop& resolved = resolved_hops[position];
    if (resolved.fraction <= 0.0) continue;
    auto* block = resolved.lookup ? open_miss_block(blocks, *resolved.lookup) : nullptr;
    if (block == nullptr) {
      blocks.push_back(
          Block{.fraction = resolved.fraction,
                .mode = resolved.mode,
                .lookup = resolved.lookup,
                .misses = misses_of_lookup(g, op, resolved.lookup),
                .reports_on = anchor_block(blocks, block_of_hop, resolved.reports_on)});
      block = &blocks.back();
    }
    block_of_hop[position] = static_cast<std::size_t>(block - blocks.data());
    block->hops.push_back(hop_of(g, op, resolved));
  }
}

// The block of the miss hops after lookup `lookup` if one is open (the last block that is not a
// failure block), else none: a new one starts.
detail::Block* ServiceBehaviour::open_miss_block(std::vector<detail::Block>& blocks,
                                                 std::size_t lookup) {
  // C++20 std::views::reverse: the blocks from the last one back, lazily, replacing a reverse
  // iterator loop (rbegin/rend) or an index counting down.
  for (detail::Block& block : blocks | std::views::reverse) {
    if (block.reports_on) continue;
    return block.lookup == lookup ? &block : nullptr;
  }
  return nullptr;
}

// The block a failure hop reports on (`reports_on`, the position of its step among the op's hops),
// moved back to the first member of the parallel group if that block is in it; empty for any other
// hop.
std::optional<std::size_t> ServiceBehaviour::anchor_block(std::span<const detail::Block> blocks,
                                                          std::span<const std::size_t> block_of_hop,
                                                          std::optional<std::size_t> reports_on) {
  if (!reports_on) return std::nullopt;
  std::size_t anchor = block_of_hop[*reports_on];
  while (anchor > 0 && blocks[anchor].mode == model::StepMode::parallel &&
         blocks[anchor - 1].mode == model::StepMode::parallel) {
    --anchor;
  }
  return anchor;
}

// The hop `resolved` makes, over the link Graph::build already proved carries its demand.
detail::Hop ServiceBehaviour::hop_of(const graph::Graph& g, std::string_view op,
                                     const graph::ServiceHop& resolved) const {
  const auto link = *g.demand_link(self_, resolved.target);
  return detail::Hop{.op = resolved.op,
                     .target = resolved.target,
                     .link = link,
                     .factor = resolved.factor,
                     .link_leg_s = seconds(g.links()[link].one_way),
                     .asynchronous = g.is_async_hop(self_, op, resolved.target, resolved.op)};
}

// The cache a miss block's lookup asks (graph::ServiceHop::lookup, a position among the op's hops)
// and its hit ratio, which the block's fraction 1 − h was resolved from; empty for any other block.
std::optional<detail::MissesOf> ServiceBehaviour::misses_of_lookup(
    const graph::Graph& g, const std::string& op, std::optional<std::size_t> lookup) const {
  if (!lookup) return std::nullopt;
  const std::size_t cache = g.service_hops(self_, op)[*lookup].target;
  return detail::MissesOf{.cache = cache, .hit_ratio = g.hit_ratio(cache).value_or(0.0)};
}

// ---- ClientBehaviour ----

// A client's backoff in whole steps (phase 3 spec §6.1): retry_after / step rounded UP, so that a
// retry never fires before the wait the scenario states (150 ms at 100 ms steps waits 2 steps,
// 200 ms; 250 ms waits 3), and at least one, because the engine can send a retry no sooner than
// the step after the failure it repeats. In integer nanoseconds, not doubles: 0.15 / 0.1 is
// 1.4999999999999998 in binary floating point, which rounding to the nearest step read as 1.
// ⌈a / b⌉ for positive integers is (a + b − 1) / b; written as the quotient plus one for a
// remainder, which is the same number and cannot overflow near the int64 limit.
int ClientBehaviour::backoff_steps(SimTime retry_after, SimTime step) {
  const SimTime::rep wait = retry_after.count();
  const SimTime::rep length = step.count();
  const SimTime::rep steps = wait / length + (wait % length == 0 ? 0 : 1);
  return static_cast<int>(std::max<SimTime::rep>(steps, 1));
}

ClientBehaviour::ClientBehaviour(const graph::Graph& g, std::size_t self, std::uint64_t seed)
    : seed_{seed} {
  const auto& workload = g.nodes()[self].spec.workload;
  if (!workload) return;  // a client without a workload offers nothing
  curve_ = workload->curve;
  noise_spec_ = workload->noise;
  timeout_s_ = graph::client_timeout_s(g.nodes()[self].spec);
  reset_noise();
  op_shares_ = workload->op_shares;
  std::ranges::sort(op_shares_, {}, &std::pair<std::string, double>::first);
  for (const auto& [op, share] : op_shares_) ops_.push_back(op);
  retries_ = workload->retries;
  start_retries(
      backoff_steps(workload->retry_after.value_or(defaults::retry_after), g.scenario().step));
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

// A traffic_spike fault (book ch. 4 and ch. 11: a flash crowd) multiplies the curve the way one
// more `spikes` entry would: a dimensionless factor on the deterministic rate, before the noise
// (workload::RateCurve: level × wave × Π spikes, × noise here). A fault of factor 3 on [at, at +
// for) and a spike {at, for, factor: 3} give the same rate. Outside every fault the factor is 1,
// and x × 1 is x exactly, so a run without faults is unchanged.
Rate ClientBehaviour::rate(SimTime now) const {
  return Rate{curve_.rate_at(now).per_second * rate_factor_ * noise_multiplier_};
}

std::vector<std::pair<std::string, Rate>> ClientBehaviour::offered(SimTime now) const {
  const double total = rate(now).per_second;
  std::vector<std::pair<std::string, Rate>> per_op;
  for (const auto& [op, share] : op_shares_) per_op.emplace_back(op, Rate{total * share});
  return per_op;  // sorted by op, as op_shares_ is
}

// Each route of an op carries its share of the op's retries as it does of its first attempts.
// Without retries due the sum adds 0.0, which leaves total × share_of_rate exactly as it was.
Routed ClientBehaviour::offer(SimTime now) const {
  Routed routed;
  const double total = rate(now).per_second;
  for (const RouteLeg& leg : legs_) {
    const double leg_rate = total * leg.share_of_rate + retries_due(leg.op) * leg.route_share;
    if (leg_rate <= 0.0) continue;  // no empty branches (see ServiceBehaviour::route)
    detail::flow_towards(routed.outgoing, leg.target, leg.link).add(leg.op, Rate{leg_rate});
  }
  detail::sort_by_destination(routed.outgoing);
  return routed;
}

Rate ClientBehaviour::attempts(std::string_view op, Rate offered) const {
  return Rate{offered.per_second + retries_due(op)};
}

Rate ClientBehaviour::last_attempts(std::string_view op, Rate offered) const {
  const auto ring = ring_index(op);
  return ring ? Rate{retry_rings_[*ring].last_attempts(offered.per_second)} : offered;
}

void ClientBehaviour::retry_failed(std::string_view op, Rate offered, double failed_share) {
  if (const auto ring = ring_index(op)) {
    retry_rings_[*ring].fail(offered.per_second, failed_share);
  }
}

void ClientBehaviour::settle_retries() {
  start_retries(1);
}

// One empty ring per op, or none at all for a client without retries (it never pays for them).
void ClientBehaviour::start_retries(int backoff_steps) {
  retry_rings_.clear();
  if (retries_ == 0) return;
  for (std::size_t i = 0; i < ops_.size(); ++i) retry_rings_.emplace_back(retries_, backoff_steps);
}

// Where `op`'s ring is in retry_rings_ (indexed like ops_, which is sorted); empty without retries.
std::optional<std::size_t> ClientBehaviour::ring_index(std::string_view op) const {
  if (retry_rings_.empty()) return std::nullopt;
  const auto found = std::ranges::lower_bound(ops_, op);
  if (found == ops_.end() || *found != op) return std::nullopt;
  return static_cast<std::size_t>(found - ops_.begin());
}

double ClientBehaviour::retries_due(std::string_view op) const {
  const auto ring = ring_index(op);
  return ring ? retry_rings_[*ring].due() : 0.0;
}

ClientBehaviour::RetryRing::RetryRing(int retries, int backoff_steps)
    : retries_{static_cast<std::size_t>(retries)},
      slots_{static_cast<std::size_t>(backoff_steps)},
      rates_(retries_ * slots_, 0.0) {}

std::span<double> ClientBehaviour::RetryRing::current() {
  return std::span{rates_}.subspan(slot_ * retries_, retries_);
}

std::span<const double> ClientBehaviour::RetryRing::current() const {
  return std::span{rates_}.subspan(slot_ * retries_, retries_);
}

double ClientBehaviour::RetryRing::due() const {
  // C++23 std::ranges::fold_left: a fold over the range itself, replacing std::accumulate's
  // iterator pair and its init-type pitfall (accumulate(b, e, 0) sums doubles as ints).
  return std::ranges::fold_left(current(), 0.0, std::plus<>{});
}

double ClientBehaviour::RetryRing::last_attempts(double first) const {
  return retries_ == 0 ? first : current().back();
}

// The retries of this step's failures replace what this step sent, attempt by attempt: attempt
// k + 1 of `backoff` steps from now is attempt k's failed share, from the last but one attempt
// down to the first (so each slot is read before it is overwritten); attempt n's failures are
// final and go nowhere. Then the ring moves on one step.
void ClientBehaviour::RetryRing::fail(double first, double failed_share) {
  const auto attempts = current();
  for (std::size_t k = attempts.size() - 1; k > 0; --k) {
    attempts[k] = attempts[k - 1] * failed_share;
  }
  attempts[0] = first * failed_share;
  slot_ = (slot_ + 1) % slots_;
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
Routed RateLimiterBehaviour::route_checked(const flow::Flow& in, SimTime now, SimTime dt,
                                           double check_answered) {
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
      forwarding_.forward(op, Rate{passed * check_answered}, routed.outgoing);
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
// store), the then-steps' targets and links, and checked the fixed hit ratio;
// Simulation::create refuses a cache without one.
CacheBehaviour::CacheBehaviour(const graph::Graph& g, std::size_t self)
    : self_{self}, cost_by_op_{g.nodes()[self].spec.cost.begin(), g.nodes()[self].spec.cost.end()} {
  for (const auto& op : g.arriving_ops(self)) {
    add_backend_block(g, op);
    detail::add_then_blocks(g, self_, op, hops_);
  }
}

double CacheBehaviour::cost(std::string_view op) const {
  return detail::cost_in(cost_by_op_, op);
}

detail::Hop detail::hop_to(const graph::Graph& g, std::size_t self, std::string_view incoming_op,
                           std::size_t target, std::string op, double factor) {
  const bool asynchronous = g.is_async_hop(self, incoming_op, target, op);
  return hop_with(g, self, target, std::move(op), factor, asynchronous);
}

detail::Hop detail::hop_with(const graph::Graph& g, std::size_t self, std::size_t target,
                             std::string op, double factor, bool asynchronous) {
  const auto link = *g.demand_link(self, target);  // Graph::build proved it exists
  return detail::Hop{.op = std::move(op),
                     .target = target,
                     .link = link,
                     .factor = factor,
                     .link_leg_s = seconds(g.links()[link].one_way),
                     .asynchronous = asynchronous};
}

// The store hop: the same op, for graph::Graph::backend_fraction of the requests. A share of 0
// (cache_aside, a read under a write strategy) adds no block: it forwards nothing. A read's hop
// carries the cache's misses (detail::own_misses); a write's is taken by every write.
void CacheBehaviour::add_backend_block(const graph::Graph& g, const std::string& op) {
  const auto& resolved = g.nodes()[self_];
  const double share = g.backend_fraction(self_, op);
  if (!resolved.backend || share <= 0.0) return;
  hops_.blocks_for(op).push_back(
      detail::Block{.fraction = share,
                    .mode = model::StepMode::sequential,
                    .lookup = std::nullopt,
                    .hops = {detail::hop_to(g, self_, op, *resolved.backend, op, 1.0)},
                    .misses = detail::own_misses(g, self_, op)});
}

// Each `on: always` then-step is a block of its own, in its declared mode; the `on: miss` ones
// share one sequential block, taken by the miss fraction graph::Graph::then_fraction gives
// (1 − h of a read; 0 of a write, which adds no block). A step no request takes (a miss step on a
// write) is left out, as in ServiceBehaviour; an `on: failure` step is refused in a `then` list
// (graph::Graph::then_fraction). A blob store's steps are all unconditional (graph::Graph refuses
// one on miss or on failure): a block each for a write, none for a read (then_fraction 0: an event
// fires for a write only).
std::optional<detail::MissesOf> detail::own_misses(const graph::Graph& g, std::size_t self,
                                                   std::string_view op) {
  if (graph::is_write(g.nodes()[self].spec, op)) return std::nullopt;
  return MissesOf{.cache = self, .hit_ratio = g.hit_ratio(self).value_or(0.0)};
}

void detail::add_then_blocks(const graph::Graph& g, std::size_t self, const std::string& op,
                             HopBlocks& hops) {
  auto& blocks = hops.blocks_for(op);
  std::optional<std::size_t> miss_block;  // position in `blocks` of the miss steps' block
  for (const model::Step& step : g.nodes()[self].spec.then) {
    const double fraction = g.then_fraction(self, step, op);
    if (fraction <= 0.0) continue;
    auto hop = hop_to(g, self, op, *g.index_of(step.to), step.op, step.factor);
    if (step.on != model::StepCondition::miss) {
      blocks.push_back(detail::Block{.fraction = fraction,
                                     .mode = step.mode,
                                     .lookup = std::nullopt,
                                     .hops = {std::move(hop)}});
      continue;
    }
    if (!miss_block) {
      miss_block = blocks.size();
      blocks.push_back(detail::Block{.fraction = fraction,
                                     .mode = model::StepMode::sequential,
                                     .lookup = std::nullopt,
                                     .misses = own_misses(g, self, op)});
    }
    blocks[*miss_block].hops.push_back(std::move(hop));
  }
}

// ---- SqlDbBehaviour ----

// The scenario's write ops as written, else the SQL default (model::write_ops_of), and their
// prices resolved once (defaults::op_cost) so cost() is one lookup per op and step.
SqlDbBehaviour::SqlDbBehaviour(const graph::Graph& g, std::size_t self)
    : write_ops_{model::write_ops_of(g.nodes()[self].spec)},
      cost_by_op_{g.nodes()[self].spec.cost.begin(), g.nodes()[self].spec.cost.end()} {
  const auto& spec = g.nodes()[self].spec;
  for (const auto& op : write_ops_) cost_by_op_.try_emplace(op, defaults::op_cost(spec, op));
}

double SqlDbBehaviour::cost(std::string_view op) const {
  return detail::cost_in(cost_by_op_, op);
}

bool SqlDbBehaviour::is_write(std::string_view op) const {
  // C++23 std::ranges::contains: "is this value in the range?" in one call, replacing
  // `std::find(begin, end, value) != end`.
  return std::ranges::contains(write_ops_, op);
}

Routed SqlDbBehaviour::route(const flow::Flow& in, SimTime /*now*/, SimTime /*dt*/) const {
  return detail::serve_everything(in);
}

// ---- MegabyteService ----

MegabyteService::MegabyteService(const graph::Graph& g, std::size_t self)
    : megabyte_s_{g.nodes()[self].capacity.service_time_s},
      service_variability_{g.nodes()[self].capacity.service_variability} {
  const auto write_ops = model::write_ops_of(g.nodes()[self].spec);
  for (const auto& op : g.arriving_ops(self)) {
    const double megabytes = graph::op_cost(g, self, op);
    const bool write = std::ranges::contains(write_ops, op);
    times_by_op_.emplace(
        op, Times{.object_s = megabytes * megabyte_s_,
                  .answer_s = write ? std::max(megabytes, 1.0) * megabyte_s_ : megabyte_s_});
  }
}

MegabyteService MegabyteService::serialising(
    double megabyte_s, const std::map<std::string, double, std::less<>>& megabytes_by_op,
    std::span<const std::string> write_ops) {
  MegabyteService pipe;
  pipe.megabyte_s_ = megabyte_s;
  pipe.service_variability_ = 0.0;
  for (const auto& [op, megabytes] : megabytes_by_op) {
    const double last_byte_s = megabytes * megabyte_s;
    const double first_megabyte_s = std::min(megabytes, 1.0) * megabyte_s;
    const bool write = std::ranges::contains(write_ops, op);
    pipe.times_by_op_.emplace(
        op, Times{.object_s = last_byte_s, .answer_s = write ? last_byte_s : first_megabyte_s});
  }
  return pipe;
}

// The sums are weighted by rate and divided by the total rate once at the end: Σ rate_o s_o / Σ
// rate = Σ p_o s_o. A mean of 0 (no op moves a byte) has no SCV and no time to feed a queue.
std::optional<fluid::ServiceMoments> MegabyteService::object_time_moments(
    const flow::Flow& requests) const {
  double rate_per_s = 0.0;
  double busy = 0.0;    // Σ rate × s: slots busy per second of arrivals
  double square = 0.0;  // Σ rate × (1 + cs²) × s²
  for (const auto& [op, rate] : requests.entries()) {
    const double object_s = times_of(op).object_s;
    rate_per_s += rate.per_second;
    busy += rate.per_second * object_s;
    square += rate.per_second * (1.0 + service_variability_) * object_s * object_s;
  }
  if (!(rate_per_s > 0.0) || !(busy > 0.0)) return std::nullopt;
  const double mean_s = busy / rate_per_s;
  const double second_s2 = square / rate_per_s;
  // max(.., 0): rounding can leave E[S²] / E[S]² a few ulps under 1 for a single size.
  return fluid::ServiceMoments{.mean_s = mean_s,
                               .scv = std::max(second_s2 / (mean_s * mean_s) - 1.0, 0.0)};
}

flow::LatencyEstimate MegabyteService::sojourn(std::string_view op,
                                               const flow::LatencyEstimate& wait) const {
  return answered_after(wait, times_of(op).answer_s);
}

flow::LatencyEstimate MegabyteService::first_megabyte(const flow::LatencyEstimate& wait) const {
  return answered_after(wait, megabyte_s_);
}

// The wait plus a service of mean `answer_s` and variance cs² × answer_s², independent of it.
flow::LatencyEstimate MegabyteService::answered_after(const flow::LatencyEstimate& wait,
                                                      double answer_s) const {
  return flow::LatencyEstimate{
      .mean_s = wait.mean_s + answer_s,
      .variance_s2 = wait.variance_s2 + service_variability_ * answer_s * answer_s};
}

MegabyteService::Times MegabyteService::times_of(std::string_view op) const {
  const auto found = times_by_op_.find(op);
  if (found == times_by_op_.end()) return Times{.object_s = megabyte_s_, .answer_s = megabyte_s_};
  return found->second;
}

namespace detail {
// The megabyte timing only when the node counts megabytes (defaults::is_byte_sized).
[[nodiscard]] std::optional<MegabyteService> megabyte_service_if_byte_sized(const graph::Graph& g,
                                                                            std::size_t self) {
  if (!defaults::is_byte_sized(g.nodes()[self].spec)) return std::nullopt;
  return MegabyteService{g, self};
}
}  // namespace detail

// ---- CdnBehaviour ----

CdnBehaviour::CdnBehaviour(const graph::Graph& g, std::size_t self)
    : cache_{g, self},
      cost_by_op_{detail::arriving_op_costs(g, self)},
      megabyte_service_{detail::megabyte_service_if_byte_sized(g, self)} {}

double CdnBehaviour::cost(std::string_view op) const {
  return detail::cost_in(cost_by_op_, op);
}

// ---- BlobStoreBehaviour ----

// Graph::build resolved every arriving op's size (a byte-sized node refuses an unsized one) and
// the `then` steps' targets and links (refusing a step on miss).
BlobStoreBehaviour::BlobStoreBehaviour(const graph::Graph& g, std::size_t self)
    : cost_by_op_{detail::arriving_op_costs(g, self)},
      megabyte_service_{detail::megabyte_service_if_byte_sized(g, self)} {
  for (const auto& op : g.arriving_ops(self)) detail::add_then_blocks(g, self, op, events_);
}

double BlobStoreBehaviour::cost(std::string_view op) const {
  return detail::cost_in(cost_by_op_, op);
}

// ---- MessageQueueBehaviour ----

// The queue's routes for each op that reaches it (graph::Graph::routes_for: one per consumer group
// whose link carries the op, each of share 1).
MessageQueueBehaviour::MessageQueueBehaviour(const graph::Graph& g, std::size_t self) {
  for (const auto& op : g.arriving_ops(self)) {
    auto& consumers = consumers_by_op_[op];
    for (const auto& route : g.routes_for(self, op)) {
      if (route.share <= 0.0) continue;  // a drained link: a group that reads nothing
      consumers.push_back(Consumer{.target = route.target, .link = route.link});
    }
  }
}

// Every group gets the whole rate of every op it reads: a copy, not a split.
Routed MessageQueueBehaviour::route(const flow::Flow& in, SimTime /*now*/, SimTime /*dt*/) const {
  Routed routed = detail::serve_everything(in);
  for (const auto& [op, rate] : routed.served.entries()) {
    const auto readers = consumers_by_op_.find(op);
    if (readers == consumers_by_op_.end()) continue;  // an op the graph never saw: no group
    for (const Consumer& consumer : readers->second) {
      detail::flow_towards(routed.outgoing, consumer.target, consumer.link).add(op, rate);
    }
  }
  detail::sort_by_destination(routed.outgoing);
  return routed;
}

// ---- WorkerPoolBehaviour ----

namespace detail {

// The delay, in steps, of the link a pool's queue delivers its jobs on (graph::ResolvedLink::
// delay_steps: the whole delay, a feedback link's included): how long a job re-published to the
// queue takes to reach the pool. 0 for a pool without `consumes` (built in code; it has no queue).
[[nodiscard]] int delivery_steps(const graph::Graph& g, std::size_t pool) {
  const auto& consumes = g.nodes()[pool].spec.consumes;
  if (!consumes) return 0;
  const auto queue = g.index_of(*consumes);
  for (const auto& link : g.links()) {
    if (queue && graph::demand_sender(link) == *queue && graph::demand_receiver(link) == pool) {
      return link.delay_steps;
    }
  }
  return 0;
}

}  // namespace detail

// One block per `then` step, each in its declared mode, under the one key `every_job`. Graph::build
// resolved the steps' targets and links, and refused one on miss or on failure, so every step is
// unconditional and taken by every job (graph::Graph::then_fraction is 1 at a pool, whatever the
// op). No hop is asynchronous: no call reaches a pool (Graph::check_pool_senders: only its queue's
// deliveries do, and they are asynchronous), so no call cycle passes through one
// (detail::find_async_hops leaves its steps out), and the other asynchronous hops start at a cache,
// a blob store or a queue. That is what lets ONE set of blocks serve the jobs of every op: were a
// pool's hop asynchronous for some op and not for another, the blocks would have to be per op.
//
// The retries (the class comment) are set up from the pool's `retries` and `retry_after` (absent:
// defaults::pool_retry_after), the calls of its steps to external sinks (whose failures it
// retries) and the delay of the link its queue delivers on (a job re-published at step t reaches
// the pool that many steps later).
WorkerPoolBehaviour::WorkerPoolBehaviour(const graph::Graph& g, std::size_t self) {
  const auto& spec = g.nodes()[self].spec;
  auto& blocks = hand_offs_.blocks_for(std::string{every_job});
  for (const model::Step& step : spec.then) {
    const std::size_t target = *g.index_of(step.to);
    blocks.push_back(
        detail::Block{.fraction = 1.0,
                      .mode = step.mode,
                      .lookup = std::nullopt,
                      .hops = {detail::hop_with(g, self, target, step.op, step.factor, false)}});
    if (g.nodes()[target].spec.type == model::NodeType::external_sink) {
      sink_calls_.push_back(SinkCall{.target = target, .op = step.op});
    }
  }
  retries_ = spec.retries.value_or(0);
  start_retries(detail::backoff_steps(spec.retry_after.value_or(defaults::pool_retry_after),
                                      g.scenario().step),
                detail::delivery_steps(g, self));
}

flow::Flow WorkerPoolBehaviour::republished() const {
  return ring_ ? ring_->due() : flow::Flow{};
}

void WorkerPoolBehaviour::note_jobs(const flow::Flow& drained, Rate arrived) {
  if (!ring_) return;
  drained_ = drained;
  arrived_per_s_ = arrived.per_second;
}

// The class comment's recurrences, on this step's jobs. Of the D jobs drained, the re-published
// ones of attempt k are those that reached the pool this step (`arriving`), times the share of its
// arrivals it drained (all of them when it drained a backlog too: min(1, D / arrived)); the rest
// are first attempts. Each attempt's failed share comes back as the next attempt, with the op mix
// of what was drained; attempt n's failed share is the dead letters.
void WorkerPoolBehaviour::retry_failed(double failed_share) {
  if (!ring_) return;
  const double jobs = drained_.total().per_second;
  const double drained_share = arrived_per_s_ > 0.0 ? std::min(1.0, jobs / arrived_per_s_) : 0.0;
  std::vector<double> retried(static_cast<std::size_t>(retries_));  // attempt k at k − 1
  for (std::size_t k = 0; k < retried.size(); ++k) {
    retried[k] = ring_->arriving(k) * drained_share;
  }
  const double first = std::max(0.0, jobs - std::ranges::fold_left(retried, 0.0, std::plus<>{}));
  dead_lettered_per_s_ = retried.back() * failed_share;
  std::vector<flow::Flow> next(retried.size());
  if (jobs > 0.0 && failed_share > 0.0) {
    next[0] = drained_.scaled(first / jobs * failed_share);
    for (std::size_t k = 1; k < next.size(); ++k) {
      next[k] = drained_.scaled(retried[k - 1] / jobs * failed_share);
    }
  }
  ring_->publish_later(std::move(next));
  drained_.clear();
  arrived_per_s_ = 0.0;
}

void WorkerPoolBehaviour::settle_retries() {
  start_retries(1, 0);
}

// An empty ring, or none at all for a pool without retries (it never pays for them).
void WorkerPoolBehaviour::start_retries(int backoff_steps, int delivery_steps) {
  backoff_steps_ = backoff_steps;
  delivery_steps_ = delivery_steps;
  ring_.reset();
  drained_.clear();
  arrived_per_s_ = 0.0;
  dead_lettered_per_s_ = 0.0;
  if (retries_ > 0) ring_.emplace(retries_, backoff_steps, delivery_steps);
}

WorkerPoolBehaviour::RepublishRing::RepublishRing(int retries, int backoff_steps,
                                                  int delivery_steps)
    : backoff_{static_cast<std::size_t>(backoff_steps)},
      slots_(static_cast<std::size_t>(backoff_steps + delivery_steps + 1),
             std::vector<flow::Flow>(static_cast<std::size_t>(retries))) {}

// The slot `steps` after the current one (the one `delivery` steps back is backoff + 1 ahead).
std::size_t WorkerPoolBehaviour::RepublishRing::ahead(std::size_t steps) const {
  return (now_ + steps) % slots_.size();
}

flow::Flow WorkerPoolBehaviour::RepublishRing::due() const {
  flow::Flow sum;
  for (const flow::Flow& attempt : slots_[now_]) sum += attempt;
  return sum;
}

// Re-published `delivery` steps ago: the slot after the one `backoff` ahead, round the ring.
double WorkerPoolBehaviour::RepublishRing::arriving(std::size_t attempt) const {
  return slots_[ahead(backoff_ + 1)][attempt].total().per_second;
}

// Replaces the slot `backoff` ahead (which last held the jobs that reached the pool a step ago,
// read already) and moves on one step.
void WorkerPoolBehaviour::RepublishRing::publish_later(std::vector<flow::Flow> attempts) {
  slots_[ahead(backoff_)] = std::move(attempts);
  now_ = ahead(1);
}

// ---- ExternalSinkBehaviour ----

ExternalSinkBehaviour::ExternalSinkBehaviour(const graph::Graph& g, std::size_t self)
    : cost_by_op_{detail::arriving_op_costs(g, self)},
      failure_rate_{g.nodes()[self].spec.failure_rate} {}

double ExternalSinkBehaviour::cost(std::string_view op) const {
  return detail::cost_in(cost_by_op_, op);
}

Routed ExternalSinkBehaviour::route(const flow::Flow& in, SimTime /*now*/, SimTime /*dt*/) const {
  return detail::serve_everything(in);
}

// ---- KvClusterBehaviour ----

namespace detail {
// p_hot = N k / M, the share of the requests whose N replicas include the hot machine (class
// comment): 0 when the ring is even (k = 1, N = nodes).
[[nodiscard]] double hot_replica_share(const graph::ResolvedNode& node) {
  if (node.imbalance <= 1.0) return 0.0;
  const double machines = *node.spec.kv_nodes;
  return std::min(1.0, model::kv_replication(node.spec) * node.imbalance / machines);
}

[[nodiscard]] queueing::TimeMoments moments_of(const flow::LatencyEstimate& latency) {
  return {.mean_s = latency.mean_s, .variance_s2 = latency.variance_s2};
}

// C(n, k), the number of ways to choose k of n things, as a double: the product of (n − k + i) / i
// for i = 1..k, each partial product itself a binomial coefficient, so it stays exact while those
// fit a double's 53 bits (n = 100, k = 5: 75 million). 0 outside 0 <= k <= n.
[[nodiscard]] double choose(int n, int k) {
  if (k < 0 || k > n) return 0.0;
  double ways = 1.0;
  for (int i = 1; i <= k; ++i) {
    ways = ways * static_cast<double>(n - k + i) / static_cast<double>(i);
  }
  return ways;
}

// The branches that answer some requests (share > 0), moved to the front in their order: the span
// returned holds them, and what follows it in `branches` is left unspecified.
[[nodiscard]] std::span<Branch> answered_only(std::span<Branch> branches) {
  std::size_t answering = 0;
  for (const Branch& branch : branches) {
    if (branch.share > 0.0) branches[answering++] = branch;
  }
  return branches.first(answering);
}
}  // namespace detail

// Graph::build checked the shape (1 <= W, R <= N <= nodes); the defaults (N = 3, W = R = 2) are
// model::kv_replication's and the quorum helpers'.
KvClusterBehaviour::KvClusterBehaviour(const graph::Graph& g, std::size_t self)
    : cost_by_op_{detail::arriving_op_costs(g, self)},
      write_ops_{model::write_ops_of(g.nodes()[self].spec)},
      replication_{model::kv_replication(g.nodes()[self].spec)},
      write_quorum_{model::kv_write_quorum(g.nodes()[self].spec)},
      read_quorum_{model::kv_read_quorum(g.nodes()[self].spec)},
      hot_share_{detail::hot_replica_share(g.nodes()[self])},
      cool_machines_{model::copies_of(g.nodes()[self].spec) - (hot_share_ > 0.0 ? 1 : 0)} {}

Routed KvClusterBehaviour::route(const flow::Flow& in, SimTime /*now*/, SimTime /*dt*/) const {
  return detail::serve_everything(in);
}

double KvClusterBehaviour::cost(std::string_view op) const {
  return detail::cost_in(cost_by_op_, op);
}

// The engine builds the hot and the cool group exactly when the graph gave the ring an imbalance
// (engine::Simulation::make_server), which is when hot_share_ > 0; anything else is a programmer
// error. With W = R the write's answer is the read's, computed once.
void KvClusterBehaviour::observe(std::span<const MachineGroup> machines) {
  const std::size_t expected = hot_share_ > 0.0 ? 2 : 1;
  if (machines.size() != expected) {
    throw std::logic_error{"KvClusterBehaviour::observe: expected " + std::to_string(expected) +
                           " machine groups, got " + std::to_string(machines.size())};
  }
  if (answered_ && machines.front() == hot_ && machines.back() == cool_) return;  // memoised
  hot_ = machines.front();
  cool_ = machines.back();
  answered_ = true;
  read_answer_ = answer_quorum(read_quorum_);
  write_answer_ = write_quorum_ == read_quorum_ ? read_answer_ : answer_quorum(write_quorum_);
}

// A write op (model::write_ops_of) waits for W replies, any other op for R.
const KvClusterBehaviour::QuorumAnswer& KvClusterBehaviour::answer_of(std::string_view op) const {
  return std::ranges::contains(write_ops_, op) ? write_answer_ : read_answer_;
}

// A quorum over the two kinds of replica set: {hot, N - 1 cool} with probability p_hot, {N cool}
// otherwise, each split by the j of its cool slots that hold a down machine (share_with_down; only
// j = 0, share 1, with every machine up): a branch of share p_set P(j) with N − j live replicas.
// A branch with fewer live replicas than the quorum loses every request; any other loses
// L_b = P(fewer than q of its live replica-requests come back). The loss is the share-weighted
// mixture of the branches' losses,
//   L = Σ_b p_b L_b,
// and the latency the mixture of the branches' order statistics over the requests ANSWERED
// (behaviours::mixture, the law of total variance), each branch weighted by the share of the
// requests it answers over all those answered:
//   w_b = p_b (1 − L_b) / (1 − L)
// (weighting by p_b alone timed the requests a branch lost too). With nothing lost the weights
// are the p_b exactly; with everything lost no request is timed, and the branches keep their
// offered shares (the time a request would take; a branch below quorum is timed at its whole set's
// reply). A branch that answers nothing is left out of the answered mix (answered_only): its reply
// time may be infinite (a dead hot machine at W = N), and its weight 0 would make 0 × inf = NaN.
KvClusterBehaviour::QuorumAnswer KvClusterBehaviour::answer_quorum(int quorum) const {
  std::vector<Branch> answered;
  std::vector<Branch> offered;
  double lost = 0.0;
  const auto add_set = [&](double share, ReplicaSet set) {
    if (share <= 0.0) return;  // a set no request meets: never weighted (0 x inf is NaN)
    for (int down = 0; down <= set.cool; ++down) {
      const double branch = share * share_with_down(set.cool, down);
      if (branch <= 0.0) continue;
      const ReplicaSet live{.hot = set.hot, .cool = set.cool - down};
      if (live.hot + live.cool < quorum) {  // the quorum is gone: every request on it is lost
        offered.push_back({.share = branch, .latency = reply_time(set, quorum)});
        lost += branch;
        continue;
      }
      const double branch_loss = loss(live, quorum);
      const flow::LatencyEstimate latency = reply_time(live, quorum);
      answered.push_back({.share = branch * (1.0 - branch_loss), .latency = latency});
      offered.push_back({.share = branch, .latency = latency});
      lost += branch * branch_loss;
    }
  };
  add_set(hot_share_, {.hot = 1, .cool = replication_ - 1});
  add_set(1.0 - hot_share_, {.hot = 0, .cool = replication_});
  const double delivered = 1.0 - lost;
  if (delivered <= 0.0) return {.latency = mixture(offered), .delivered = delivered};
  const auto kept = detail::answered_only(answered);
  for (Branch& branch : kept) branch.share /= delivered;
  return {.latency = mixture(kept), .delivered = delivered};
}

// The share of the sets with `cool_slots` cool replicas that hold exactly `down_in_set` of the
// cool group's down machines: the hypergeometric probability (class comment)
//   P(j) = C(r, j) C(C − r, s − j) / C(C, s)
// with r = cool_.down, C = cool_machines_, s = cool_slots, j = down_in_set: the ways to place j of
// the set's slots on down machines and the rest on live ones, over all ways to choose the set (each
// of its N distinct machines equally likely: the ring's arcs spread the keys evenly). Exactly 1 for
// j = 0 when no machine is down (x / x), so a cluster with every machine up answers as before.
double KvClusterBehaviour::share_with_down(int cool_slots, int down_in_set) const {
  const int down = std::min(cool_.down, cool_machines_);
  const double sets = detail::choose(cool_machines_, cool_slots);
  if (sets <= 0.0) return down_in_set == 0 ? 1.0 : 0.0;  // no such set is ever drawn (share 0)
  return detail::choose(down, down_in_set) *
         detail::choose(cool_machines_ - down, cool_slots - down_in_set) / sets;
}

// The quorum-th fastest reply of the set: queueing::lognormal_order_statistic over its machines.
flow::LatencyEstimate KvClusterBehaviour::reply_time(ReplicaSet set, int quorum) const {
  const std::array groups{
      queueing::ReplicaGroup{.replicas = set.hot, .sojourn = detail::moments_of(hot_.sojourn)},
      queueing::ReplicaGroup{.replicas = set.cool, .sojourn = detail::moments_of(cool_.sojourn)}};
  const auto reply = queueing::lognormal_order_statistic(groups, quorum);
  return {.mean_s = reply.mean_s, .variance_s2 = reply.variance_s2};
}

// P(fewer than `quorum` of the set's replica-requests come back), each kept with its machine's
// kept share: queueing::probability_fewer_than.
double KvClusterBehaviour::loss(ReplicaSet set, int quorum) const {
  const std::array kept{queueing::TrialGroup{.trials = set.hot, .success = hot_.kept},
                        queueing::TrialGroup{.trials = set.cool, .success = cool_.kept}};
  return queueing::probability_fewer_than(kept, quorum);
}

}  // namespace simsys::behaviours
