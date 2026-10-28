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

export module simsys.fluid;

import simsys.core;
import simsys.flow;
import simsys.queueing;

export namespace simsys::fluid {

// What one replica of a node can do. The book gives throughput (QPS) and latency as
// separate back-of-the-envelope numbers, so we keep both: `service_rate` bounds the
// fluid drain, `service_time_s` is how long one request occupies one of the
// `concurrency` servers and feeds the Erlang C term.
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
  double rho{};                   // utilization, capped at 1
  flow::LatencyEstimate latency;  // sojourn = queueing wait + service time
  Zone zone{Zone::green};
};

// The fluid model of one node. State is a single number, the backlog (requests waiting);
// everything else is recomputed each step from demand and capacity.
class FluidQueue {
 public:
  explicit FluidQueue(Capacity capacity) : capacity_{capacity} {}

  // Advances the node by `dt` (must be > 0) under a constant `demand`.
  // Not [[nodiscard]]: callers that only care about the new backlog may drop the report.
  FluidStep advance(Rate demand, SimTime dt);
  // Jumps to the long-run state: empty below capacity, full (queue_limit) at/above.
  void settle(Rate demand);

  [[nodiscard]] double backlog() const { return backlog_; }
  [[nodiscard]] const Capacity& capacity() const { return capacity_; }

 private:
  // One short method per line of the §3.3 formula; see advance().
  [[nodiscard]] double drain_for(double demand_per_s, double dt_s) const;
  void grow_backlog(double demand_per_s, double drain_per_s, double dt_s);
  [[nodiscard]] double spill_overflow(double dt_s);
  [[nodiscard]] double utilization(double demand_per_s) const;
  [[nodiscard]] double backlog_wait_s() const;
  [[nodiscard]] flow::LatencyEstimate sojourn(double rho) const;

  Capacity capacity_;
  double backlog_{};
};

}  // namespace simsys::fluid

namespace simsys::fluid {

// Highest utilization fed to the Erlang C term. The M/M/c wait diverges as rho -> 1;
// the backlog already carries the delay of a saturated node, so counting an infinite
// stochastic wait on top would count the same overload twice.
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
//   drain  = min(cap, demand + backlog_/dt_s)              serve what we can: new demand
//                                                          plus everything that waited
//   backlog_ += (demand - drain) * dt_s                    grows past capacity, shrinks below
//   overflow = max(0, backlog_ - queue_limit) / dt_s;  backlog_ = min(backlog_, queue_limit)
//                                                          the waiting room is finite: the
//                                                          excess is dropped (load shedding)
//   rho    = cap > 0 ? min(1, demand/cap) : 1              utilization, a saturated node is 1
//   latency.mean = backlog_/cap                            Little's law on the deterministic
//                                                          backlog (it carries no variance)
//                + k * mmc_wait(c, min(rho, 0.99), s).mean stochastic bunching of arrivals;
//                                                          capped at 0.99, see max_stochastic_rho
//                + s                                       the service itself
//   latency.var  = k^2 * mmc_wait(...).variance + service_variability * s^2
//   k      = variability_factor(burstiness, service_variability)
//                  Allen–Cunneen scales the MEAN wait by k. Applying k^2 to the variance is
//                  OUR OWN HEURISTIC (a scaled random variable has k^2 times the variance);
//                  no published result is claimed for it.
//   zone   = zone_of(rho, capacity_)
//
// Conservation: demand*dt = (drain + dropped)*dt + (backlog_after - backlog_before).
FluidStep FluidQueue::advance(Rate demand, SimTime dt) {
  const double dt_s = simsys::seconds(dt);
  const double demand_per_s = demand.per_second;
  const double drain_per_s = drain_for(demand_per_s, dt_s);
  grow_backlog(demand_per_s, drain_per_s, dt_s);
  const double dropped_per_s = spill_overflow(dt_s);
  const double rho = utilization(demand_per_s);
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

// A node with no capacity (cap <= 0) is treated as saturated: it drains nothing, so
// all demand piles up and (below) rho is 1.
double FluidQueue::drain_for(double demand_per_s, double dt_s) const {
  const double cap = capacity_.service_rate.per_second;
  if (cap <= 0.0) return 0.0;
  return std::min(cap, demand_per_s + backlog_ / dt_s);
}

// The max(.., 0.0) only removes floating-point residue when a backlog drains exactly to
// empty (e.g. -1e-17). The value being clamped comes first so a NaN would propagate
// instead of being replaced by 0 (std::max/std::min return their first argument when the
// comparison is false). std::fmin/fmax are avoided for the same reason: they hide NaNs.
void FluidQueue::grow_backlog(double demand_per_s, double drain_per_s, double dt_s) {
  backlog_ = std::max(backlog_ + (demand_per_s - drain_per_s) * dt_s, 0.0);
}

// Returns the dropped rate in requests per second and trims the backlog to queue_limit.
double FluidQueue::spill_overflow(double dt_s) {
  const double overflow = std::max(backlog_ - capacity_.queue_limit, 0.0) / dt_s;
  backlog_ = std::min(backlog_, capacity_.queue_limit);
  return overflow;
}

// Utilization rho = lambda / mu: offered load over capacity, capped at 1 (beyond that the
// excess is backlog and drops, not "more than 100% busy"). Assumes nothing about arrival
// randomness. The ratio comes first in std::min so a NaN demand propagates.
double FluidQueue::utilization(double demand_per_s) const {
  const double cap = capacity_.service_rate.per_second;
  if (cap <= 0.0) return 1.0;
  return std::min(demand_per_s / cap, 1.0);
}

// Little's law, L = lambda * W, applied to the deterministic backlog: draining `backlog_`
// requests at `cap` per second takes backlog_/cap seconds. Assumes the backlog drains
// at full capacity. With no capacity the wait never ends.
double FluidQueue::backlog_wait_s() const {
  const double cap = capacity_.service_rate.per_second;
  if (cap <= 0.0) return std::numeric_limits<double>::infinity();
  return backlog_ / cap;
}

// Sojourn time (what the book calls latency) = deterministic backlog wait
//                                            + stochastic queueing wait
//                                            + service time.
// The stochastic part is the M/M/c wait scaled by the Allen–Cunneen factor k (see the
// formula block above advance()). Assumes the node is in steady state at the capped rho.
flow::LatencyEstimate FluidQueue::sojourn(double rho) const {
  const double s = capacity_.service_time_s;
  const double k =
      queueing::variability_factor(capacity_.burstiness, capacity_.service_variability);
  const auto wait = queueing::mmc_wait(capacity_.concurrency, std::min(rho, max_stochastic_rho), s);
  return flow::LatencyEstimate{
      .mean_s = backlog_wait_s() + k * wait.mean_s + s,
      // Var(Wq) scaled by k^2 (our heuristic) plus the service variance cs^2 * s^2; for
      // exponential service (cs^2 = 1) that is s^2. Independence of wait and service is
      // assumed, so the variances add.
      .variance_s2 = k * k * wait.variance_s2 + capacity_.service_variability * s * s};
}

}  // namespace simsys::fluid
