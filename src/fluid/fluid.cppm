// Module `simsys.fluid`: the capacity/backlog mechanics every queue-like node shares.
// This is spec §3.3 — the heart of the fluid engine — kept free of node semantics so it
// can be tested against closed forms on its own.
//
// Models: one replica group of a server, cache, queue or database as the book draws it
// in System Design Interview ch. 1 (back-of-the-envelope estimation): a box that can
// serve `service_rate` requests per second, with a bounded waiting room. Requests are a
// continuous fluid, not individuals. Captured: saturation (backlog grows past capacity),
// load shedding (drops once the waiting room is full), and the stochastic delay a real
// node shows below saturation (bunching of random arrivals). Ignored: request sizes,
// priorities, retries, and any per-request identity.
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
// Invariant the defaults do not enforce (scenarios own it): `service_rate` should equal
// `concurrency / service_time_s`. The fluid drain uses the former, the Erlang C term the
// latter, so a mismatch makes them describe two different machines. Example: rate 2000/s,
// s = 1 ms, c = 1 at 1500/s gives an Erlang wait computed at rho 0.75 (3 ms) while a
// consistent 2000/s server (s = 0.5 ms) would wait 1.5 ms. `service_rate` must be >= 0.
struct Capacity {
  Rate service_rate;         // requests per second one replica can drain
  double service_time_s{};   // seconds one request occupies a server
  int concurrency{1};        // servers per replica: the c in M/M/c
  double queue_limit{1000};  // requests that may wait; beyond this the fluid is dropped
  // The yellow/red thresholds follow the common operations rule of thumb that latency
  // starts to climb steeply above ~70% utilization and a node is at risk above ~90%
  // (queueing theory: the M/M/1 wait grows like 1/(1-rho)).
  double yellow{0.70};  // utilization where the node turns yellow
  double red{0.90};     // ... and red
  double service_variability{
      1};                // squared coefficient of variation of service time (1 = exponential)
  double burstiness{1};  // squared coefficient of variation of inter-arrival times (1 = Poisson)
};

enum class Zone { green, yellow, red };

// Pattern: Strategy-by-data — the thresholds live in Capacity, so each node can have its
// own alerting levels without a subclass. >= red -> red, >= yellow -> yellow.
[[nodiscard]] Zone zone_of(double rho, const Capacity& c);

// Everything one step produced, for metrics and for routing the drained flow onward.
struct FluidStep {
  double demand_per_s{};
  double drain_per_s{};
  double dropped_per_s{};
  double backlog{};               // requests waiting after this step
  double rho{};                   // utilization = busy fraction drain/capacity, in [0, 1]
  flow::LatencyEstimate latency;  // sojourn = queueing wait + service time
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
  // Jumps to the long-run state: empty below capacity, full (queue_limit) at/above.
  // At exactly rho = 1 the two disagree, per spec: advance() keeps an empty backlog
  // empty (drain = demand = capacity), settle() fills it (the >= below).
  void settle(Rate demand);

  [[nodiscard]] double backlog() const { return backlog_; }
  [[nodiscard]] const Capacity& capacity() const { return capacity_; }

 private:
  // One short method per line of the §3.3 formula; see advance().
  [[nodiscard]] double drain_for(double demand_per_s, double dt_s) const;
  void grow_backlog(double demand_per_s, double drain_per_s, double dt_s);
  [[nodiscard]] double spill_overflow(double dt_s);
  [[nodiscard]] double utilization(double drain_per_s) const;
  [[nodiscard]] double backlog_wait_s() const;
  [[nodiscard]] flow::LatencyEstimate sojourn(double rho) const;
  static void require_positive_step(SimTime dt);

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
//   stoch  = k * mmc_wait(c, min(rho, 0.99), s).mean       random bunching of arrivals
//   latency.mean = max(fluid, stoch) + s                   OUR HEURISTIC blend, see sojourn()
//   latency.var  = (stoch wins ? k^2 * mmc_wait(...).variance : 0)
//                + service_variability * s^2
//                  Allen–Cunneen scales the MEAN wait by k. Applying k^2 to the variance is
//                  OUR OWN HEURISTIC (a scaled random variable has k^2 times the variance);
//                  no published result is claimed for it.
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
  // C++20 designated initializers: name the fields at the construction site. Replaces
  // positional aggregate init, which silently swaps same-typed doubles when the struct
  // is reordered; FluidStep has five doubles in a row, so the names earn their keep.
  return FluidStep{.demand_per_s = demand_per_s,
                   .drain_per_s = drain_per_s,
                   .dropped_per_s = dropped_per_s,
                   .backlog = backlog_,
                   .rho = rho,
                   .latency = sojourn(rho),
                   .zone = zone_of(rho, capacity_)};
}

void FluidQueue::settle(Rate demand) {
  backlog_ = demand.per_second >= capacity_.service_rate.per_second ? capacity_.queue_limit : 0.0;
}

// `dt.count() <= 0`, not `dt <= SimTime::zero()`: duration's comparison operators and
// zero() are declared in <chrono>, which this interface must not include (CLAUDE.md §3,
// GCC 14 BMI bug), so we compare the raw tick count. SimTime is integer nanoseconds,
// so this is exact.
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

// Sojourn time (what the book calls latency) = queueing wait + service time, where the
// queueing wait is the LARGER of two estimates (our heuristic, not a published result):
//
//   fluid  backlog_/cap: the deterministic drain time of the backlog (backlog_wait_s()).
//   stoch  k * M/M/c Wq at min(rho, 0.99): the random bunching of arrivals that a node
//          shows below saturation; k is the Allen–Cunneen factor (formula block above
//          advance()). Assumes steady state at that rho.
//
// Why max and not the sum: by Little's law (L = lambda * W) the M/M/1 wait at rho 0.99
// corresponds to a stochastic queue of ~99 requests (Lq = rho^2/(1 - rho) = 98). Once the fluid
// backlog exceeds that, the stochastic queue IS the backlog, so adding both counts it twice
// (measured +50-98% overstatement in early overload with 100 ms steps). The max equals the old
// formula below saturation (backlog 0), is continuous, and becomes fluid-only deep in overload.
//
// Variance: k^2 * Var(Wq) when the stochastic term wins, 0 when the fluid term wins (a
// deterministic drain), plus the service variance cs^2 * s^2 in both cases; for
// exponential service (cs^2 = 1) that is s^2. Wait and service are assumed independent,
// so their variances add.
flow::LatencyEstimate FluidQueue::sojourn(double rho) const {
  const double s = capacity_.service_time_s;
  const double k =
      queueing::variability_factor(capacity_.burstiness, capacity_.service_variability);
  const auto wait = queueing::mmc_wait(capacity_.concurrency, std::min(rho, max_stochastic_rho), s);
  const double fluid_wait_s = backlog_wait_s();
  const double stochastic_wait_s = k * wait.mean_s;
  const double service_variance_s2 = capacity_.service_variability * s * s;
  if (fluid_wait_s > stochastic_wait_s) {
    return flow::LatencyEstimate{.mean_s = fluid_wait_s + s, .variance_s2 = service_variance_s2};
  }
  return flow::LatencyEstimate{.mean_s = stochastic_wait_s + s,
                               .variance_s2 = k * k * wait.variance_s2 + service_variance_s2};
}

}  // namespace simsys::fluid
