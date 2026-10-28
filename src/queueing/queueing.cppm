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

export module simsys.queueing;

export namespace simsys::queueing {

// Erlang C: the probability that an arriving request finds all `servers` busy and has
// to wait, in an M/M/c queue (Poisson arrivals, exponential service, c servers).
// `rho` is the utilization lambda / (c * mu) in [0, 1).
//   P_wait = [a^c/c! * 1/(1-rho)] / [sum_{k<c} a^k/k! + a^c/c! * 1/(1-rho)],  a = c*rho
// At or beyond rho = 1 every arrival waits, so we return 1 (the fluid backlog carries
// the actual delay then, see fluid.cppm).
[[nodiscard]] double erlang_c(int servers, double rho) {
  if (rho <= 0.0) return 0.0;
  if (rho >= 1.0) return 1.0;
  const double offered_load = servers * rho;  // "a", in Erlangs
  double term = 1.0;                          // a^k / k!, built iteratively (no factorials)
  double sum_below_c = 0.0;
  for (int k = 0; k < servers; ++k) {
    sum_below_c += term;
    term *= offered_load / (k + 1);
  }
  const double last = term / (1.0 - rho);  // a^c / c! * 1/(1-rho)
  return last / (sum_below_c + last);
}

struct WaitMoments {
  double mean_s;
  double variance_s2;
};

// Mean and variance of the time spent waiting in queue (not being served) in M/M/c.
// The wait is 0 with probability 1 - P_wait, otherwise exponential with mean
// s / (c * (1 - rho)) where s is the service time, so
//   E[Wq]   = P_wait * s / (c (1 - rho))               (Little's law applied to the queue)
//   Var[Wq] = P_wait (2 - P_wait) * (s / (c (1 - rho)))^2
// Infinite at saturation: a queue with rho >= 1 has no steady state.
[[nodiscard]] WaitMoments mmc_wait(int servers, double rho, double service_time_s) {
  if (rho >= 1.0) {
    constexpr double inf = std::numeric_limits<double>::infinity();
    return {inf, inf};
  }
  const double p_wait = erlang_c(servers, rho);
  const double scale = service_time_s / (servers * (1.0 - rho));
  // C++20 designated initializers: name the fields at the construction site. Replaces
  // positional `{a, b}`, which silently swaps two doubles of the same type when the
  // struct is reordered; here mean and variance cannot be confused.
  return {.mean_s = p_wait * scale, .variance_s2 = p_wait * (2.0 - p_wait) * scale * scale};
}

// Kingman / Allen–Cunneen approximation: a G/G/c queue waits about
//   (c_a^2 + c_s^2) / 2  times as long as the M/M/c queue with the same rho,
// where c_a^2 is the squared coefficient of variation (variance / mean^2) of
// inter-arrival times ("burstiness") and c_s^2 that of service times
// ("service_variability"). Exact for M/M/c (1, 1 -> 1); a heuristic otherwise.
[[nodiscard]] double variability_factor(double arrival_scv, double service_scv) {
  return (arrival_scv + service_scv) / 2.0;
}

// Standard-normal quantiles for the three percentiles reports show. Constants rather
// than an inverse-error-function implementation: three values is all we need.
inline constexpr double z_p50 = 0.0;
inline constexpr double z_p95 = 1.6448536;
inline constexpr double z_p99 = 2.3263479;

// Lognormal fit by moments. Latency along a path is a sum of positive, right-skewed
// terms; a lognormal with the same mean m and variance v is the usual engineering fit:
//   sigma^2 = ln(1 + v/m^2),  mu = ln m - sigma^2/2,  quantile(z) = exp(mu + sigma z)
// Reports label the result "estimated": it is a fit, not a measurement.
[[nodiscard]] double lognormal_quantile(double mean, double variance, double z_score) {
  if (mean <= 0.0) return 0.0;
  if (variance <= 0.0) return mean;
  const double sigma_sq = std::log1p(variance / (mean * mean));
  const double mu = std::log(mean) - sigma_sq / 2.0;
  return std::exp(mu + std::sqrt(sigma_sq) * z_score);
}

}  // namespace simsys::queueing
