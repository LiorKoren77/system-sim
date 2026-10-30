// Module `simsys.fluid`: the capacity/backlog mechanics every queue-like node shares.
// This is spec §3.3 — the heart of the fluid engine — kept free of node semantics so it
// can be tested against closed forms on its own.
//
// Models: one replica group of a server, cache, queue or database as the book draws it
// in System Design Interview ch. 1 (back-of-the-envelope estimation): a box that can
// serve `service_rate` requests per second, with a bounded waiting room. Requests are a
// continuous fluid, not individuals. Captured: saturation (backlog grows past capacity),
// load shedding (drops once the waiting room is full), and the stochastic delay a real
// node shows below saturation (bunching of random arrivals). Ignored: request sizes (the
// engine prices them as capacity units and, at a byte-sized node, sets the moments of its
// objects' service time: set_service), priorities, retries, and any per-request identity.
//
// C++20 named module, interface unit (see src/core/core.cppm for the longer
// explanation). Note CLAUDE.md §3: no <chrono> here; time helpers come from simsys.core.
module;
#include <algorithm>
#include <limits>
#include <stdexcept>

export module simsys.fluid;

import simsys.core;
import simsys.flow;
import simsys.queueing;

export namespace simsys::fluid {

// What one replica of a node can do. The book gives throughput (QPS) and latency as
// separate back-of-the-envelope numbers, so we keep both: `service_rate` bounds the
// fluid drain, `service_time_s` is how long one request occupies one of the
// `concurrency` servers and feeds the Erlang C term.
//
// Invariant: `service_rate` should equal `concurrency / service_time_s`. The defaults keep it, up
// to rounding c up to a whole server, by deriving c (defaults::implied_concurrency, Little's law);
// a scenario that sets advanced.concurrency owns it. The fluid drain uses `service_rate`, the
// Erlang C term `concurrency` and `service_time_s`, so a mismatch makes them describe two different
// machines. Example: rate 2000/s, s = 1 ms, c = 1 at 1500/s gives an Erlang wait computed at rho
// 0.75 (3 ms) while a consistent 2000/s server (s = 0.5 ms) would wait 1.5 ms. `service_rate` must
// be >= 0. One planned exception, a byte-sized node (spec §19 entry 2): `service_rate` counts
// megabytes (MB/s) and c = MB/s × the time of one MB, but `service_time_s` is the mean time one
// OBJECT holds a server and `service_variability` the squared coefficient of variation of those
// times over the node's size mix (FluidQueue::set_service, from the engine each step), so the rate
// is MB/s and c / service_time_s is objects/s. The invariant then holds where it matters, in rho:
// objects/s × s_obj / c = MB/s × s_MB / c = MB/s / service_rate (Little's law), so the fluid drain
// and Erlang C see the same utilization and the wait is a whole object's.
struct Capacity {
  Rate service_rate;        // requests per second one replica can drain
  double service_time_s{};  // seconds one request occupies a server
  int concurrency{1};       // servers per replica: the c in M/M/c
  // Units of work that may wait per replica (requests when every cost is 1: the engine
  // feeds the queue capacity units, see engine::NodeSample); beyond this the fluid is dropped.
  double queue_limit{1000};
  // The yellow/red thresholds follow the common operations rule of thumb that latency
  // starts to climb steeply above ~70% utilization and a node is at risk above ~90%
  // (queueing theory: the M/M/1 wait grows like 1/(1-rho)).
  double yellow{0.70};  // utilization where the node turns yellow
  double red{0.90};     // ... and red
  double service_variability{
      1};                // squared coefficient of variation of service time (1 = exponential)
  double burstiness{1};  // squared coefficient of variation of inter-arrival times (1 = Poisson)
};

// The first two moments of the time one request holds a server, as Kingman's formula reads them:
// the mean and the squared coefficient of variation (SCV, variance / mean²; 0 = every request
// takes the same time, 1 = exponential). What a node whose service time moves with its op mix
// hands its queue each step (FluidQueue::set_service).
struct ServiceMoments {
  double mean_s{};
  double scv{};
};

// What a scripted fault does to one replica's queue for one step (engine::Condition, phase 3 spec
// §4.2): the replica serves `capacity_factor` of its service rate, and every request holds a server
// `extra_service_s` longer. The identity (1, 0) changes nothing. The room and the backlog are not
// touched: requests that were waiting keep waiting while the node is down, and drain when it is
// back. See FluidQueue::advance(Rate, SimTime, const Derating&) for the arithmetic.
struct Derating {
  double capacity_factor{1};
  double extra_service_s{0};
};

enum class Zone { green, yellow, red };

// The thresholds live in Capacity, as data, so each node can have its own alerting levels
// without a subclass. >= red -> red, >= yellow -> yellow.
[[nodiscard]] Zone zone_of(double rho, const Capacity& c);

// Everything one step produced, for metrics and for routing the drained flow onward.
struct FluidStep {
  double demand_per_s{};
  double drain_per_s{};
  double dropped_per_s{};
  double backlog{};               // requests waiting after this step
  double rho{};                   // utilization = busy fraction drain/capacity, in [0, 1]
  flow::LatencyEstimate latency;  // sojourn = queueing wait + service time
  // The queueing wait alone (the latency above without its service time): what a byte-sized
  // node adds each op's own service to, a write's whole transfer or a read's first megabyte
  // (engine::Simulation::own_sojourn).
  flow::LatencyEstimate wait;
  Zone zone{Zone::green};
};

// The fluid model of one node. State is a single number, the backlog (requests waiting);
// everything else is recomputed each step from demand and capacity.
class FluidQueue {
 public:
  explicit FluidQueue(Capacity capacity) : capacity_{capacity} {}

  // Advances the node by `dt` under a constant `demand`. Throws std::invalid_argument
  // if dt <= 0 (a programmer error: the engine never takes an empty or backward step).
  // C++20 [[nodiscard("reason")]]: the attribute (C++17) now carries a message that the
  // compiler prints in the warning, so a caller learns *why* dropping the value is a bug.
  [[nodiscard("the drained flow must be routed downstream")]] FluidStep advance(Rate demand,
                                                                                SimTime dt);
  // The same step with the replica derated for it (a scripted fault, see Derating): the capacity
  // the step runs on is the derated one, the capacity kept for the next step is not. With the
  // identity it is advance(demand, dt), bit for bit.
  [[nodiscard("the drained flow must be routed downstream")]] FluidStep advance(
      Rate demand, SimTime dt, const Derating& derating);
  // Jumps to the long-run state: empty at or below capacity, full (queue_limit) above it.
  // At exactly rho = 1 it keeps the room empty, as advance() does from an empty backlog
  // (drain = demand = capacity), so `estimate` and `run` agree there (the strict > in the body).
  void settle(Rate demand);

  // Replaces the moments of the time one request occupies a server: the mean feeds the Erlang C
  // wait and the sojourn's service term, the SCV the Allen–Cunneen factor (and the sojourn's
  // service variance); the rate, c, the room and the backlog stay. For a node whose service time
  // moves with its op mix: a byte-sized node's objects are as long as their sizes (the engine
  // sets their mixture's moments every step; see Capacity's invariant). One call for both, so the
  // queue never sees a new mean with an old spread. Throws std::invalid_argument unless the mean
  // is > 0 and the SCV >= 0 (a programmer error).
  void set_service(ServiceMoments service);

  [[nodiscard]] double backlog() const { return backlog_; }
  [[nodiscard]] const Capacity& capacity() const { return capacity_; }

 private:
  // One short method per line of the §3.3 formula; see advance().
  [[nodiscard]] double drain_for(double demand_per_s, double dt_s) const;
  void grow_backlog(double demand_per_s, double drain_per_s, double dt_s);
  [[nodiscard]] double spill_overflow(double dt_s);
  [[nodiscard]] double utilization(double drain_per_s) const;
  [[nodiscard]] double backlog_wait_s() const;
  [[nodiscard]] double room_drain_s() const;
  [[nodiscard]] flow::LatencyEstimate wait(double rho) const;
  [[nodiscard]] flow::LatencyEstimate sojourn(const flow::LatencyEstimate& waited) const;
  static void require_positive_step(SimTime dt);
  [[nodiscard]] static double slowed_rate_per_s(const Capacity& capacity, double extra_s);

  Capacity capacity_;
  double backlog_{};
};

}  // namespace simsys::fluid

namespace simsys::fluid {

// Highest utilization fed to the Erlang C term. The M/M/c wait diverges as rho -> 1;
// a saturated node's delay is carried by the backlog instead (see sojourn()).
constexpr double max_stochastic_rho = 0.99;

Zone zone_of(double rho, const Capacity& c) {
  // C++20 `using enum`: brings the enumerators into scope so the body reads `red`
  // instead of `Zone::red`. Replaces a repeated qualifier (or an unscoped enum, which
  // would leak the names everywhere); limited to this function, so nothing leaks.
  using enum Zone;
  if (rho >= c.red) return red;
  if (rho >= c.yellow) return yellow;
  return green;
}

// The §3.3 step, one named helper per line:
//
//   dt_s   = seconds(dt);  cap = service_rate.per_second;  demand = demand.per_second
//   drain  = min(demand + backlog_/dt_s, cap)              serve what we can: new demand
//                                                          plus everything that waited
//   backlog_ += (demand - drain) * dt_s                    grows past capacity, shrinks below
//   overflow = max(0, backlog_ - queue_limit) / dt_s;  backlog_ = min(backlog_, queue_limit)
//                                                          the waiting room is finite: the
//                                                          excess is dropped (load shedding)
//   rho    = cap > 0 ? drain/cap : 1                       busy fraction; a node with no
//                                                          capacity counts as saturated
//   k      = variability_factor(burstiness, service_variability)
//   fluid  = backlog_/cap                                  deterministic drain time
//   room   = queue_limit/cap                               the full waiting room's drain time
//   rho_st = min(demand, drain)/cap                        the load the NEW arrivals bring
//   stoch  = min(k * mmc_wait(c, min(rho_st, 0.99), s).mean, room)
//                                                          random bunching of arrivals, never
//                                                          longer than the room (the room-drain
//                                                          bound, see wait())
//   latency.mean = max(fluid, stoch) + s                   OUR HEURISTIC blend, see sojourn()
//   latency.var  = min(k^2 * mmc_wait(...).variance, room^2/4) + service_variability * s^2
//                  (always). Allen–Cunneen scales the MEAN wait by k. Applying k^2 to the
//                  variance is OUR OWN HEURISTIC (a scaled random variable has k^2 times the
//                  variance); no published result is claimed for it. The room^2/4 cap is our
//                  heuristic too (the room's scale), see wait().
//   zone   = zone_of(rho, capacity_)
//
// Conservation: demand*dt = (drain + dropped)*dt + (backlog_after - backlog_before).
FluidStep FluidQueue::advance(Rate demand, SimTime dt) {
  require_positive_step(dt);
  const double dt_s = simsys::seconds(dt);
  const double demand_per_s = demand.per_second;
  const double drain_per_s = drain_for(demand_per_s, dt_s);
  grow_backlog(demand_per_s, drain_per_s, dt_s);
  const double dropped_per_s = spill_overflow(dt_s);
  const double rho = utilization(drain_per_s);
  // The stochastic wait reads the utilization of what ARRIVES, min(demand, drain) / cap, not the
  // busy fraction rho. They differ only while a backlog drains after the load fell (a ramp down:
  // demand < drain = cap): every server is busy, but the new arrivals no longer bunch as at rho 1,
  // and the queue they find is the backlog, which the fluid term carries. Read at rho 1 the wait
  // held at the clamp (or the room's drain time) until the backlog was gone; read at the arrivals'
  // load it falls with the backlog. Filling or saturated (demand >= drain) and with an empty
  // backlog (drain = demand) it is rho itself. Demand first: a NaN propagates.
  const flow::LatencyEstimate waited = wait(utilization(std::min(demand_per_s, drain_per_s)));
  // C++20 designated initializers: name the fields at the construction site. Replaces
  // positional aggregate init, which silently swaps same-typed doubles when the struct
  // is reordered; FluidStep has five doubles in a row, so the names earn their keep.
  return FluidStep{.demand_per_s = demand_per_s,
                   .drain_per_s = drain_per_s,
                   .dropped_per_s = dropped_per_s,
                   .backlog = backlog_,
                   .rho = rho,
                   .latency = sojourn(waited),
                   .wait = waited,
                   .zone = zone_of(rho, capacity_)};
}

// A derated step (Derating) runs advance() on the capacity
//   service_rate'   = service_rate × capacity_factor × s / (s + extra)
//   service_time_s' = s + extra
// and puts the nominal capacity back afterwards (the backlog it left stays). The two factors:
//   capacity_factor  the share of the replica still serving (a node_down of r of its R replicas:
//                    (R − r) / R; a degraded tier: the fault's factor). The servers c and the
//                    service time stay, so the survivors are the same machines at a higher rho:
//                    for independent copies (one queue per replica) that is exact, each copy
//                    sees demand / R at a capacity × (R − r) / R, the rho of R − r copies seeing
//                    demand / (R − r). For a pooled group it keeps c·R servers where R − r are
//                    left: the Erlang C wait at that rho is then a little optimistic.
//   extra            a slower request: c servers each holding a request s + extra drain
//                    c / (s + extra) per second, which is the rate × s / (s + extra) (Little's law,
//                    the Capacity invariant rate = c / s). The service time keeps its shape (its
//                    SCV), so an exponential service stays exponential with the longer mean: the
//                    M/M/1 closed form W = s' / (1 − rho') holds during a latency spike. A constant
//                    added delay would have a smaller SCV and a slightly shorter wait (ignored).
// capacity_factor 0 is a replica that serves nothing: drain 0, the room fills and the rest is
// dropped, the wait is infinite (backlog_wait_s), rho 1. With the identity the nominal step is
// taken directly, so a run without faults is bit-identical to one before faults existed.
FluidStep FluidQueue::advance(Rate demand, SimTime dt, const Derating& derating) {
  if (derating.capacity_factor == 1.0 && derating.extra_service_s == 0.0) {
    return advance(demand, dt);
  }
  const Capacity nominal = capacity_;
  capacity_.service_rate =
      Rate{slowed_rate_per_s(nominal, derating.extra_service_s) * derating.capacity_factor};
  capacity_.service_time_s = nominal.service_time_s + derating.extra_service_s;
  const FluidStep step = advance(demand, dt);
  capacity_ = nominal;
  return step;
}

// The rate of servers that hold every request `extra_s` longer: rate × s / (s + extra) (see
// above). A capacity that states no service time (s = 0: its rate is all it says) gets the rate of
// c servers each busy `extra_s` per request, c / extra, if that is the lower.
double FluidQueue::slowed_rate_per_s(const Capacity& capacity, double extra_s) {
  const double rate = capacity.service_rate.per_second;
  if (!(extra_s > 0.0)) return rate;
  const double s = capacity.service_time_s;
  if (s > 0.0) return rate * (s / (s + extra_s));
  return std::min(rate, static_cast<double>(capacity.concurrency) / extra_s);
}

// Strictly above capacity: at demand = capacity exactly the fluid neither grows nor drains, so the
// long-run backlog is whatever it started at, and both commands start empty. The stochastic wait at
// rho 1 is the clamp's (max_stochastic_rho), not a full room's. A real queue at rho 1 is
// null-recurrent and holds about half its room on average: size off rho 1.
void FluidQueue::settle(Rate demand) {
  backlog_ = demand.per_second > capacity_.service_rate.per_second ? capacity_.queue_limit : 0.0;
}

// `!(x > 0)` and `!(x >= 0)` rather than `x <= 0` and `x < 0` so that a NaN is refused too.
void FluidQueue::set_service(ServiceMoments service) {
  if (!(service.mean_s > 0.0)) {
    throw std::invalid_argument{"FluidQueue::set_service: the mean time must be > 0"};
  }
  if (!(service.scv >= 0.0)) {
    throw std::invalid_argument{"FluidQueue::set_service: the SCV must be >= 0"};
  }
  capacity_.service_time_s = service.mean_s;
  capacity_.service_variability = service.scv;
}

// `dt.count() <= 0`, not `dt <= SimTime::zero()`: SimTime::zero() is a static member and
// visible here, but duration's comparison operators are non-member templates declared
// in <chrono>, which this interface must not include (CLAUDE.md §3, GCC 14 BMI bug);
// importing simsys.core does not bring them into scope, because simsys.core includes
// <chrono> in its global module fragment and never exports those operators (and
// argument-dependent lookup does not reach them either, [basic.lookup.argdep]/4.3).
// So we compare the raw tick count; SimTime is integer nanoseconds, so this is exact.
void FluidQueue::require_positive_step(SimTime dt) {
  if (dt.count() <= 0) throw std::invalid_argument{"FluidQueue::advance: dt must be > 0"};
}

// The demand term comes first in std::min so a NaN demand propagates (std::min returns
// its first argument when the comparison is false) instead of being "served" at full
// capacity. With cap = 0 the node drains nothing and all demand piles up.
double FluidQueue::drain_for(double demand_per_s, double dt_s) const {
  return std::min(demand_per_s + backlog_ / dt_s, capacity_.service_rate.per_second);
}

// The max(.., 0.0) only removes floating-point residue when a backlog drains exactly to
// empty: rounding of order ulp(demand*dt), up to ~1e-4 requests at 1e9/s. The value
// being clamped comes first so a NaN would propagate instead of being replaced by 0
// (std::max/std::min return their first argument when the comparison is false).
// std::fmin/fmax are avoided for the same reason: they hide NaNs.
void FluidQueue::grow_backlog(double demand_per_s, double drain_per_s, double dt_s) {
  backlog_ = std::max(backlog_ + (demand_per_s - drain_per_s) * dt_s, 0.0);
}

// Returns the dropped rate in requests per second and trims the backlog to queue_limit.
double FluidQueue::spill_overflow(double dt_s) {
  const double overflow = std::max(backlog_ - capacity_.queue_limit, 0.0) / dt_s;
  backlog_ = std::min(backlog_, capacity_.queue_limit);
  return overflow;
}

// Utilization rho = drain / cap: the fraction of time the servers are busy. Whenever the
// backlog is empty this equals the textbook lambda/mu capped at 1 (drain = min(demand,
// cap)); the difference is a node working off a backlog: demand may be 50% of capacity
// but every server is busy, so it is red, not green. Never exceeds 1 because drain <= cap.
// A NaN drain propagates (division keeps NaN).
double FluidQueue::utilization(double drain_per_s) const {
  const double cap = capacity_.service_rate.per_second;
  if (cap <= 0.0) return 1.0;
  return drain_per_s / cap;
}

// Fluid (virtual) waiting time: a request joining behind `backlog_` requests at a server
// draining `cap`/s waits backlog_/cap. Agrees with Little's law W = L/lambda only when
// saturated (lambda = cap); it is a deterministic drain time, not a long-run average.
// Uses the end-of-step backlog, so it is pessimistic by up to half a step's growth.
//
// With no capacity the wait never ends: +inf. Engine contract: never multiply a latency
// by a zero weight (inf * 0 = NaN), skip branches that carry no flow, and the report
// layer prints inf as "unavailable".
double FluidQueue::backlog_wait_s() const {
  const double cap = capacity_.service_rate.per_second;
  if (cap <= 0.0) return std::numeric_limits<double>::infinity();
  return backlog_ / cap;
}

// The longest anyone admitted can wait: a FULL waiting room's drain time, queue_limit / cap. A
// request that finds the room full is dropped (spill_overflow), so one that gets in finds at most
// queue_limit units ahead of it, drained at cap per second. +inf with no capacity, as above.
double FluidQueue::room_drain_s() const {
  const double cap = capacity_.service_rate.per_second;
  if (cap <= 0.0) return std::numeric_limits<double>::infinity();
  return capacity_.queue_limit / cap;
}

// Sojourn time (what the book calls latency) = queueing wait (wait()) + service time
// (sojourn()), where the queueing wait is the LARGER of two estimates (our heuristic, not a
// published result):
//
//   fluid  backlog_/cap: the deterministic drain time of the backlog (backlog_wait_s()).
//   stoch  k * M/M/c Wq at min(rho_st, 0.99): the random bunching of arrivals that a node
//          shows below saturation; k is the Allen–Cunneen factor (formula block above
//          advance()), rho_st the arrivals' utilization min(demand, drain)/cap (advance()).
//          Assumes steady state at that rho. Bounded by the room (below).
//
// The room-drain bound: a queue with a waiting room of K units turns away whoever finds it full, so
// a request it admits has at most K units ahead of it and waits at most T = K / cap, the full
// room's drain time; stoch is min(k * Wq, T). For a deterministic drain (the fluid backlog, a leaky
// bucket's regular leak) T is exact: the last one admitted to a full room waits exactly that. For
// random arrivals and service it is conservative: T bounds the WORST admitted wait, so it is above
// M/M/c/K's MEAN wait, about twice it near rho = 1 (M/M/1 with a room of 10 at rho 0.98 waits 4.8
// ms on average where T says 10 ms). Erlang C at the 0.99 clamp knows no room: with 2 s objects at
// a 20-slot blob store the engine's stochastic term would be (1 + 0) / 2 x C(20, 0.99) x 2 s / (20
// x 0.01) = 4.74 s (9.48 s with exponential service), where the 1000 MB room drains in 2 s. At a
// node that counts requests with the default room it binds only for very bursty arrivals: the room
// holds 1000 requests, ten times the ~100 Erlang C queues at the clamp (Lq = rho^2 / (1 - rho) = 98
// at one server; T / clamp >= (1000 s/c) / (100 s/c) = 10), so the mean binds only when the
// arrivals' variability factor k exceeds 10, and the variance cap below when k exceeds 5; otherwise
// only a small `queue_limit` or objects of many units meet it. Its variance is capped at T^2 / 4,
// OUR HEURISTIC: the room's scale, which keeps a bounded wait from carrying the clamp's spread
// (99.7 s^2 for that blob store, against 1 s^2). It is not a theorem: a wait in [0, T] whose mean
// sits at T has no spread at all (Bhatia–Davis), and the tight bound for a given mean would make
// the spread, and so the p99, fall as the load rises. What it does not do is bound the percentiles:
// the reports fit a lognormal to the mean and variance (queueing::lognormal_quantile), which knows
// no bound, so an estimated p99 can exceed T + s (a blob store fed 12 writes/s of 50 MB: p99 6.88 s
// against T + s = 4 s).
//
// Why max and not the sum: by Little's law (L = lambda * W) the M/M/1 wait at rho 0.99
// corresponds to a stochastic queue of ~99 requests (Lq = rho^2/(1 - rho) = 98). Once the fluid
// backlog exceeds that, the stochastic queue IS the backlog, so adding both counts it twice
// (measured +50-98% overstatement in early overload with 100 ms steps). The max equals the old
// formula below saturation (backlog 0), is continuous in the mean, and becomes fluid-only
// deep in overload.
//
// Variance: min(k^2 * Var(Wq at min(rho_st, 0.99)), T^2 / 4) + cs^2 * s^2, ALWAYS, whichever
// term wins the mean; for exponential service (cs^2 = 1) the second term is s^2. Wait and service
// are assumed independent, so their variances add. The stochastic variance is kept even
// when the fluid drain carries the mean, so percentiles stay monotone as overload
// deepens: dropping it (a "deterministic drain" has no spread) made the estimated p99
// FALL the moment the backlog wait overtook the Erlang wait, because the lognormal fit
// lost its spread faster than the mean grew. Also our heuristic, not a published result.
flow::LatencyEstimate FluidQueue::wait(double rho) const {
  const double k =
      queueing::variability_factor(capacity_.burstiness, capacity_.service_variability);
  const auto erlang = queueing::mmc_wait(capacity_.concurrency, std::min(rho, max_stochastic_rho),
                                         capacity_.service_time_s);
  const double room_s = room_drain_s();
  const double fluid_wait_s = backlog_wait_s();
  // The scaled Erlang terms first in std::min and std::max, so a NaN in them propagates.
  const double stochastic_wait_s = std::min(k * erlang.mean_s, room_s);
  const double stochastic_variance_s2 = std::min(k * k * erlang.variance_s2, room_s * room_s / 4.0);
  return flow::LatencyEstimate{.mean_s = std::max(stochastic_wait_s, fluid_wait_s),
                               .variance_s2 = stochastic_variance_s2};
}

// The wait plus one service time s, its variance cs^2 × s^2 (independent of the wait).
flow::LatencyEstimate FluidQueue::sojourn(const flow::LatencyEstimate& waited) const {
  const double s = capacity_.service_time_s;
  return flow::LatencyEstimate{
      .mean_s = waited.mean_s + s,
      .variance_s2 = waited.variance_s2 + capacity_.service_variability * s * s};
}

}  // namespace simsys::fluid
