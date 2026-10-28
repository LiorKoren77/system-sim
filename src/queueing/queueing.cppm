// Module `simsys.queueing`: closed-form results from queueing theory that the fluid
// engine uses as the stochastic correction on top of its deterministic backlog.
// Written for a reader who has read the book but not studied queueing theory:
// every function names its formula and the assumption it rests on.
//
// C++20 named module, interface unit: `module;` opens the global module fragment, where
// the std headers are included so they stay shareable with other translation units;
// `export module` starts the module's purview. Replaces a header plus include guards:
// only what is marked `export` is visible to importers, and no macros leak out.
// (See src/core/core.cppm for the longer explanation.)
module;
#include <cmath>
#include <limits>
#include <stdexcept>

export module simsys.queueing;

export namespace simsys::queueing {

// Erlang C: the probability that an arriving request finds all `servers` busy and has
// to wait, in an M/M/c queue (Poisson arrivals, exponential service, c servers).
// `rho` is the utilization lambda / (c * mu) in [0, 1).
// At or beyond rho = 1 every arrival waits, so we return 1 (the fluid backlog carries
// the actual delay then, see fluid.cppm). A rho <= 0, including a negative one, is
// treated as no load and returns 0.
//
// Preconditions (programmer errors, not user input): `servers >= 1`, enforced by an
// exception because CLAUDE.md allows exceptions for programmer errors and scenario
// validation keeps user input from ever reaching this function. Not guarded, only
// documented: a NaN `rho` propagates to a NaN result (both comparisons below are false),
// which is the desired loud failure. The engine must therefore not cap rho at 0.99 with
// `std::fmin`, which returns the non-NaN argument and would hide the NaN.
[[nodiscard]] double erlang_c(int servers, double rho) {
  if (servers < 1) throw std::invalid_argument{"servers must be >= 1"};
  if (rho <= 0.0) return 0.0;
  if (rho >= 1.0) return 1.0;
  const double offered_load = servers * rho;  // "a", in Erlangs
  // Erlang B is the *loss* formula: the probability that a request is lost (turned away)
  // in a system with c servers and no waiting room, i.e. that all c servers are busy.
  // Its recurrence is B(0) = 1, B(k) = a B(k-1) / (k + a B(k-1)). Every intermediate
  // stays in [0, 1], unlike the textbook a^k/k!, which overflows a double once a > ~712
  // (a thousand pooled connections at rho 0.99 is a realistic input).
  double blocking = 1.0;
  for (int k = 1; k <= servers; ++k)
    blocking = offered_load * blocking / (k + offered_load * blocking);
  // Erlang C from Erlang B: P_wait = B / (1 - rho (1 - B)). It is larger than B because
  // a queue lets blocked requests wait instead of leaving.
  return blocking / (1.0 - rho * (1.0 - blocking));
}

struct WaitMoments {
  double mean_s;
  double variance_s2;
};

// Mean and variance of the time spent waiting in queue (not being served) in M/M/c.
// The wait is 0 with probability 1 - P_wait. Otherwise the arrival finds all c servers
// busy and N requests already queued ahead of it, where N is geometric:
//   P(N = n | wait) = (1 - rho) rho^n,   n = 0, 1, 2, ...
// It must wait for N + 1 completions (one per request ahead, plus one to free a server
// for itself). While all c servers are busy, completions form a Poisson process of rate
// c*mu (exponential service is memoryless), so each gap is Exp(c mu). A geometric
// number of Exp(c mu) gaps sums to an exponential again:
//   Wq | wait ~ Exp(c mu (1 - rho)) = Exp(c mu - lambda),
//   theta = E[Wq | wait] = s / (c (1 - rho)),             (s = 1/mu, the service time)
// so, conditioned on waiting, Wq is exponential with mean theta, and:
//   E[Wq]   = P_wait * theta                               (law of total expectation)
//   Var[Wq] = E[Wq^2] - E[Wq]^2 = P*2 theta^2 - P^2 theta^2 = P (2 - P) theta^2
//                                                          (Exp's second moment is 2 theta^2)
// Infinite at saturation: a queue with rho >= 1 has no steady state.
// Same preconditions as erlang_c: `servers >= 1` (throws), NaN rho propagates, and a
// negative service time simply yields negative waits (not guarded).
[[nodiscard]] WaitMoments mmc_wait(int servers, double rho, double service_time_s) {
  if (servers < 1) throw std::invalid_argument{"servers must be >= 1"};
  if (rho >= 1.0) {
    constexpr double inf = std::numeric_limits<double>::infinity();
    // C++20 designated initializers: name the fields at the construction site. Replaces
    // positional `{a, b}`, which silently swaps two doubles of the same type when the
    // struct is reordered; here mean and variance cannot be confused.
    return {.mean_s = inf, .variance_s2 = inf};
  }
  const double p_wait = erlang_c(servers, rho);
  const double theta = service_time_s / (servers * (1.0 - rho));
  return {.mean_s = p_wait * theta, .variance_s2 = p_wait * (2.0 - p_wait) * theta * theta};
}

// Kingman's formula is the G/G/1 approximation
//   E[Wq] ~ (rho / (1 - rho)) * ((c_a^2 + c_s^2) / 2) * s;
// its multi-server extension is Allen-Cunneen, which is what this factor implements: a
// G/G/c queue waits about (c_a^2 + c_s^2) / 2 times as long as the M/M/c queue with the
// same rho. Here c_a^2 is the squared coefficient of variation (variance / mean^2) of
// inter-arrival times ("burstiness") and c_s^2 that of service times ("service_variability").
// Assumptions and limits:
//  - the factor scales the *mean* wait only; using it on the variance is a separate
//    heuristic and must be flagged where it is done;
//  - inter-arrival times are assumed renewal (independent of each other);
//  - exact for M/M/c (1, 1 -> 1) and for M/G/1 (Pollaczek-Khinchine);
//  - most accurate as rho -> 1, a heuristic at light load.
[[nodiscard]] double variability_factor(double arrival_scv, double service_scv) {
  return (arrival_scv + service_scv) / 2.0;
}

// Standard-normal quantiles for the three percentiles reports show. Constants rather
// than an inverse-error-function implementation: three values is all we need.
inline constexpr double z_p50 = 0.0;
inline constexpr double z_p95 = 1.6448536269514722;
inline constexpr double z_p99 = 2.3263478740408408;

// Lognormal fit by moment matching, as in Fenton-Wilkinson. Latency along a path is a
// sum of positive, right-skewed terms; a lognormal with the same mean m and variance v
// is the usual engineering fit:
//   sigma^2 = ln(1 + v/m^2),  mu = ln m - sigma^2/2,  quantile(z) = exp(mu + sigma z)
// Reports label the result "estimated": it is a fit, not a measurement. The fit is poor
// for a distribution with an atom at 0, such as Wq alone (zero with probability
// 1 - P_wait); the engine applies it to the full sojourn time, whose mean is >= s.
// `variance <= 0` returns the mean on purpose: it also clamps the small negative
// variances that rounding can produce when moments are composed by subtraction.
// Overflow: the result is NaN only when v/m^2 itself exceeds the double range
// (~1.8e308): sigma^2 = log1p(inf) = inf, and mu + sigma z becomes -inf + inf (or
// inf * 0 at z = 0). No real latency has that spread, so it is documented, not guarded.
[[nodiscard]] double lognormal_quantile(double mean, double variance, double z_score) {
  if (mean <= 0.0) return 0.0;
  if (variance <= 0.0) return mean;
  // (variance / mean) / mean instead of variance / (mean * mean): mean * mean underflows
  // to 0 for means below ~1e-162, which would make sigma^2 infinite.
  const double sigma_sq = std::log1p((variance / mean) / mean);
  const double mu = std::log(mean) - sigma_sq / 2.0;
  return std::exp(mu + std::sqrt(sigma_sq) * z_score);
}

}  // namespace simsys::queueing
