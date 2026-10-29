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
#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <functional>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

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

// The mean and variance of a time, in seconds and seconds squared: a queueing wait (mmc_wait) or a
// whole reply time, a quorum's (the order statistics below). Was WaitMoments, a name that read
// wrong at a sojourn (review of plan 2c Tasks 9-10, minor 10).
struct TimeMoments {
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
[[nodiscard]] TimeMoments mmc_wait(int servers, double rho, double service_time_s) {
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

// The k-th smallest of n independent exponential times of mean `mean_s`: how long a coordinator
// that sends one request to n replicas waits for the k-th reply (book ch. 6, "quorum consensus":
// a Dynamo-style store answers a read after R of its N replicas reply, a write after W).
// Rényi's representation: while n − i replicas are still running, the next one finishes after
// the minimum of n − i exponentials, itself exponential with mean m / (n − i) (the first of
// several "coin-flip" clocks rings sooner the more of them run), and by memorylessness each such
// gap is independent of the ones before. The k-th reply is the sum of the first k gaps:
//   mean     = m × Σ_{i=0}^{k−1} 1 / (n − i)
//   variance = m² × Σ_{i=0}^{k−1} 1 / (n − i)²        (independent terms: their variances add)
// n = 3, m = 1 ms: k = 1 → 1/3 ms (the fastest of three), k = 2 → 5/6 ms (Dynamo's quorum of 2),
// k = 3 → 11/6 ms (the slowest, the harmonic number H_3). Assumes the replicas' times are
// independent and exponential: true of an M/M/1 sojourn, an approximation for a node of c > 1
// servers or a service SCV other than 1 (whose sojourn is not exponential). The engine does not
// use it since the review of plan 2c Tasks 9-10 (C-1): a kv_cluster's replicas are not alike (one
// may be the ring's hot machine) and not exponential, so lognormal_order_statistic takes each
// machine's own moments. It stays as the closed form a student can check by hand, and as the
// cross-check of that quadrature (fed exponentials, it must reproduce these sums).
// Preconditions (programmer errors: the loader and graph::Graph::build refuse such a quorum):
// 1 <= k <= n, else std::invalid_argument. An infinite mean gives infinite moments.
[[nodiscard]] TimeMoments exponential_order_statistic(int n, int k, double mean_s) {
  if (n < 1) throw std::invalid_argument{"exponential_order_statistic: n must be >= 1"};
  if (k < 1 || k > n) {
    throw std::invalid_argument{"exponential_order_statistic: k must be in [1, n]"};
  }
  double mean_factor = 0.0;      // Σ 1 / (n − i)
  double variance_factor = 0.0;  // Σ 1 / (n − i)²
  for (int i = 0; i < k; ++i) {
    const double running = n - i;  // replicas still to reply
    mean_factor += 1.0 / running;
    variance_factor += 1.0 / (running * running);
  }
  return {.mean_s = mean_s * mean_factor, .variance_s2 = mean_s * mean_s * variance_factor};
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
// is the usual engineering fit: ln X is normal with
//   sigma^2 = ln(1 + v/m^2),  mu = ln m - sigma^2/2
// (lognormal_quantile reads its percentiles off it, lognormal_order_statistic its whole
// distribution). Precondition: m > 0; v = 0 gives sigma = 0, a time with no spread.
struct LognormalFit {
  double mu;     // the mean of ln X
  double sigma;  // the standard deviation of ln X
};
[[nodiscard]] LognormalFit lognormal_fit(double mean, double variance) {
  // (variance / mean) / mean instead of variance / (mean * mean): mean * mean underflows
  // to 0 for means below ~1e-162, which would make sigma^2 infinite.
  const double sigma_sq = std::log1p((variance / mean) / mean);
  return {.mu = std::log(mean) - sigma_sq / 2.0, .sigma = std::sqrt(sigma_sq)};
}

// A percentile of the lognormal fit: quantile(z) = exp(mu + sigma z), z the standard-normal
// quantile of the percentile (z_p99 for the p99).
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
  const LognormalFit fit = lognormal_fit(mean, variance);
  return std::exp(fit.mu + fit.sigma * z_score);
}

// Phi(z) = P(Z <= z) for a standard normal Z. The error function's complement does it in one
// call: Phi(z) = erfc(-z / sqrt 2) / 2, accurate to the last digit in both tails (1 - erf would
// lose every digit below Phi = 1e-16).
// C++20 std::numbers::sqrt2 (<numbers>): the constant as a typed double, replacing the
// non-standard M_SQRT2 macro or a hand-typed 1.41421356...
[[nodiscard]] double normal_cdf(double z) {
  return 0.5 * std::erfc(-z / std::numbers::sqrt2);
}

// `trials` independent trials that each succeed with probability `success`: one kind of replica
// in a quorum (its share of answered, or of kept, requests). See probability_fewer_than.
struct TrialGroup {
  int trials;
  double success;
};

// A grid even in ln t: points first_log_s, first_log_s + step, ... (`points` of them). Even in
// ln t because a reply time spans orders of magnitude (a cool machine's 0.3 ms, a full queue's
// 100 ms), and a lognormal is smooth on that scale. See moments_from_survival.
struct LogGrid {
  double first_log_s;
  double step;
  int points;
};

// The mean and variance of a positive time X from its survival function S(t) = P(X > t):
//   E[X]   = integral of S(t) dt          over t from 0 to infinity
//   E[X^2] = integral of 2 t S(t) dt      (both: integrate P(X > t) instead of t f(t), by parts)
// `survival(ln t)` returns S(t). With u = ln t (dt = t du) the integrands are t S and 2 t^2 S,
// summed by the trapezoid rule on `grid`. Below the grid S is taken as 1 (nothing has answered
// yet), and the rule's sum over that left tail is a geometric series, added in closed form:
//   h Σ_{j >= 1} e^(u0 - j h)       = h e^u0 / (e^h - 1)
//   h Σ_{j >= 1} 2 e^(2(u0 - j h))  = 2 h e^(2 u0) / (e^(2h) - 1)
// so the rule runs over the whole line in u. Above the grid S is taken as 0. For an integrand
// that is smooth and dies off at both ends, as these do, the trapezoid rule over the whole line
// converges faster than any power of h (the error falls like e^(-c / h)): docs/study/numerics.md
// has the bound and a "try it". Assumes the grid ends where S has reached 0 to double precision.
// C++20 requires-clause on a template: `survival` must be callable with a double and return a
// number, checked at the call with the concept's name instead of deep in the body (replaces an
// unconstrained template whose misuse fails inside the loop).
template <class Survival>
  requires std::invocable<Survival&, double> &&
           std::convertible_to<std::invoke_result_t<Survival&, double>, double>
[[nodiscard]] TimeMoments moments_from_survival(const LogGrid& grid, Survival&& survival) {
  const double h = grid.step;
  const double first_t = std::exp(grid.first_log_s);
  double first = h * first_t / std::expm1(h);                         // ∫ S dt, left tail
  double second = 2.0 * h * first_t * first_t / std::expm1(2.0 * h);  // ∫ 2t S dt, left tail
  for (int j = 0; j < grid.points; ++j) {
    const double log_t = grid.first_log_s + j * h;
    const double t = std::exp(log_t);
    const double unanswered = survival(log_t);
    first += h * t * unanswered;
    second += 2.0 * h * t * t * unanswered;
  }
  // Var X = E[X^2] - E[X]^2; the possibly-NaN difference first, so std::max passes a NaN on.
  return {.mean_s = first, .variance_s2 = std::max(second - first * first, 0.0)};
}

// `replicas` replicas whose reply times are independent draws of one distribution of mean and
// variance `sojourn`: one kind of machine a quorum's request reaches (the ring's hot machine,
// or one of the cool ones). See lognormal_order_statistic.
struct ReplicaGroup {
  int replicas;
  TimeMoments sojourn;
};

}  // namespace simsys::queueing

// Not exported: the pieces of the functions below.
namespace simsys::queueing::detail {

// Acklam's rational approximation of Phi^-1 (P. J. Acklam, 2003; relative error below 1.15e-9),
// in three regions: the central one in r = (p - 1/2)^2, the two tails in q = sqrt(-2 ln p). The
// coefficients are his; normal_quantile then polishes the result with one Halley step.
inline constexpr std::array acklam_a{-3.969683028665376e+01, 2.209460984245205e+02,
                                     -2.759285104469687e+02, 1.383577518672690e+02,
                                     -3.066479806614716e+01, 2.506628277459239e+00};
inline constexpr std::array acklam_b{-5.447609879822406e+01, 1.615858368580409e+02,
                                     -1.556989798598866e+02, 6.680131188771972e+01,
                                     -1.328068155288572e+01};
inline constexpr std::array acklam_c{-7.784894002430293e-03, -3.223964580411365e-01,
                                     -2.400758277161838e+00, -2.549732539343734e+00,
                                     4.374664141464968e+00,  2.938163982698783e+00};
inline constexpr std::array acklam_d{7.784695709041462e-03, 3.224671290700398e-01,
                                     2.445134137142996e+00, 3.754408661907416e+00};
inline constexpr double acklam_tail = 0.02425;  // below it (and above 1 - it): the tail formula

// Horner's rule: c[0] x^(n-1) + c[1] x^(n-2) + ... + c[n-1].
template <std::size_t N>
[[nodiscard]] double horner(const std::array<double, N>& coefficients, double x) {
  double value = 0.0;
  for (const double c : coefficients) value = value * x + c;
  return value;
}

// Acklam's lower tail, p < 0.02425: x = C(q) / (D(q) q + 1), q = sqrt(-2 ln p).
[[nodiscard]] double acklam_lower_tail(double p) {
  const double q = std::sqrt(-2.0 * std::log(p));
  return horner(acklam_c, q) / (horner(acklam_d, q) * q + 1.0);
}

// Acklam's central region: x = A(r) q / (B(r) r + 1), q = p - 1/2, r = q^2.
[[nodiscard]] double acklam_central(double p) {
  const double q = p - 0.5;
  const double r = q * q;
  return horner(acklam_a, r) * q / (horner(acklam_b, r) * r + 1.0);
}

// One Halley step on Phi(x) = p, which roughly triples the correct digits (1e-9 -> full double):
//   e = Phi(x) - p,  u = e / phi(x) = e sqrt(2 pi) e^(x^2/2),  x' = x - u / (1 + x u / 2)
[[nodiscard]] double halley_refined(double x, double p) {
  const double error = normal_cdf(x) - p;
  const double u = error * std::sqrt(2.0 * std::numbers::pi) * std::exp(x * x / 2.0);
  return x - u / (1.0 + x * u / 2.0);
}

// P(fewer than `successes` of the groups' trials succeed), with `below` (size `successes`) as the
// working distribution: below[j] = P(exactly j successes so far), j < successes. Each trial of
// success probability p moves mass up one (below[j] p to j + 1) or keeps it (below[j] (1 - p));
// mass that reaches `successes` leaves the array, since only "fewer than" is asked.
// C++20 std::span: a non-owning view of contiguous elements, here of the caller's groups and of a
// scratch array it owns, so the quadrature calls this once per grid point without allocating;
// replaces a (pointer, size) pair or a `std::vector&` that would tie the caller to a vector.
[[nodiscard]] double fewer_than(std::span<const TrialGroup> groups, std::span<double> below) {
  // C++20 std::ranges::fill: the whole range in one argument, replacing fill(begin, end, value).
  std::ranges::fill(below, 0.0);
  below.front() = 1.0;
  for (const TrialGroup& group : groups) {
    for (int trial = 0; trial < group.trials; ++trial) {
      for (std::size_t j = below.size() - 1; j > 0; --j) {
        below[j] = below[j] * (1.0 - group.success) + below[j - 1] * group.success;
      }
      below.front() *= 1.0 - group.success;
    }
  }
  // C++23 std::ranges::fold_left: a left fold over a range with an explicit initial value whose
  // type (double) is the result's, replacing std::accumulate(begin, end, 0.0), where a `0` would
  // silently sum in int.
  return std::ranges::fold_left(below, 0.0, std::plus<>{});
}

// The smallest spread a fitted replica is given: sigma = 0.001, a sojourn whose standard deviation
// is 0.1 % of its mean. A sojourn with none at all (deterministic service, no wait) would be a step
// in S(t) that no grid even in ln t resolves; at 0.001 the order statistic's mean moves by less
// than a part in 1e3 and the grid stays finite.
inline constexpr double min_sigma = 1e-3;
// The grid stops this many standard deviations of ln X beyond every group: Phi(-8) = 6e-16, so S is
// 1 below the grid and 0 above it to double precision.
inline constexpr double grid_reach = 8.0;
// Steps per standard deviation of the narrowest group, and the most points a grid may have (a very
// narrow group far from a wide one widens the step instead: then the bound of numerics.md loosens).
inline constexpr double steps_per_sigma = 2.0;
inline constexpr int max_grid_points = 4000;

// A group of replicas with its lognormal fit (sigma at least min_sigma).
struct FittedReplicas {
  int replicas;
  LognormalFit fit;
};

// A replica that answers at all: a finite, positive mean and a finite variance. (One of mean 0
// answers at once; one without a finite moment never does.)
[[nodiscard]] bool answers_in_time(const ReplicaGroup& group) {
  return group.replicas > 0 && group.sojourn.mean_s > 0.0 && std::isfinite(group.sojourn.mean_s) &&
         std::isfinite(group.sojourn.variance_s2);
}

[[nodiscard]] bool answers_at_once(const ReplicaGroup& group) {
  return group.sojourn.mean_s <= 0.0;
}

[[nodiscard]] bool has_nan(const ReplicaGroup& group) {
  return std::isnan(group.sojourn.mean_s) || std::isnan(group.sojourn.variance_s2);
}

[[nodiscard]] int replicas_in(std::span<const ReplicaGroup> groups) {
  int replicas = 0;
  for (const ReplicaGroup& group : groups) replicas += group.replicas;
  return replicas;
}

// The groups that answer in a finite, positive time, each with its lognormal fit.
[[nodiscard]] std::vector<FittedReplicas> fitted_answering(std::span<const ReplicaGroup> groups) {
  std::vector<FittedReplicas> fitted;
  for (const ReplicaGroup& group : groups) {
    if (!answers_in_time(group)) continue;
    LognormalFit fit = lognormal_fit(group.sojourn.mean_s, group.sojourn.variance_s2);
    fit.sigma = std::max(fit.sigma, min_sigma);
    fitted.push_back({.replicas = group.replicas, .fit = fit});
  }
  return fitted;
}

// The grid for fitted groups: from the lowest group's mu - 8 sigma (nobody has answered) to the
// highest's mu + 2 sigma^2 + 8 sigma (everybody has, even for E[X^2], whose integrand 2 t^2 S peaks
// at mu + 2 sigma^2 in ln t), sigma / 2 apart for the narrowest group.
[[nodiscard]] LogGrid lognormal_grid(std::span<const FittedReplicas> fitted) {
  double lowest = std::numeric_limits<double>::infinity();
  double highest = -std::numeric_limits<double>::infinity();
  double narrowest = std::numeric_limits<double>::infinity();
  for (const auto& [replicas, fit] : fitted) {
    lowest = std::min(lowest, fit.mu - grid_reach * fit.sigma);
    highest = std::max(highest, fit.mu + 2.0 * fit.sigma * fit.sigma + grid_reach * fit.sigma);
    narrowest = std::min(narrowest, fit.sigma);
  }
  const double step = std::max(narrowest / steps_per_sigma, (highest - lowest) / max_grid_points);
  return {.first_log_s = lowest,
          .step = step,
          .points = static_cast<int>(std::ceil((highest - lowest) / step)) + 1};
}

// The k-th reply's moments over fitted groups: S_k(t) = P(fewer than k have answered by t), each
// replica of a group answering by t with its fit's P(ln X <= ln t) = Phi((ln t - mu) / sigma).
[[nodiscard]] TimeMoments integrate_quorum(std::span<const FittedReplicas> fitted, int k) {
  std::vector<TrialGroup> answered(fitted.size());
  std::vector<double> below(static_cast<std::size_t>(k));
  return moments_from_survival(lognormal_grid(fitted), [&](double log_t) {
    for (std::size_t i = 0; i < fitted.size(); ++i) {
      const auto& [replicas, fit] = fitted[i];
      answered[i] = {.trials = replicas, .success = normal_cdf((log_t - fit.mu) / fit.sigma)};
    }
    return fewer_than(answered, below);
  });
}

}  // namespace simsys::queueing::detail

export namespace simsys::queueing {

// Phi^-1(p): the z with P(Z <= z) = p, for p strictly between 0 and 1. Acklam's rational
// approximation polished by one Halley step: full double precision across the range (the tests
// pin the 97.5th percentile, 1.959963984540054, and the 1e-10 tail, -6.3613409024). Used for the
// expected maximum of M normals (expected_normal_maximum) and wherever a percentile's z is needed
// beyond the three constants above.
// Precondition (programmer error, the callers compute p): 0 < p < 1, else std::invalid_argument;
// a NaN p is refused too.
[[nodiscard]] double normal_quantile(double p) {
  if (!(p > 0.0 && p < 1.0)) {
    throw std::invalid_argument{"normal_quantile: p must be strictly between 0 and 1"};
  }
  double x = 0.0;
  if (p < detail::acklam_tail) {
    x = detail::acklam_lower_tail(p);
  } else if (p > 1.0 - detail::acklam_tail) {
    x = -detail::acklam_lower_tail(1.0 - p);  // the normal is symmetric: Phi^-1(p) = -Phi^-1(1 - p)
  } else {
    x = detail::acklam_central(p);
  }
  return detail::halley_refined(x, p);
}

// The expected maximum of `count` independent standard normals, by Blom's approximation (1958):
//   z_M = Phi^-1((M - 0.375) / (M + 0.25))
// the plotting position of the largest of M ordered normal draws. Exact for M = 1 (0) and close
// beyond: 0.5895 for M = 2 against the exact 1/sqrt(pi) = 0.5642, 1.0491 for M = 4 against
// 1.0294; for large M it grows like sqrt(2 ln M). It is how far above the average share the
// hottest of M machines on a hash ring sits, in standard deviations (graph::ring_imbalance).
// Precondition: count >= 1 (programmer error otherwise).
[[nodiscard]] double expected_normal_maximum(int count) {
  if (count < 1) throw std::invalid_argument{"expected_normal_maximum: count must be >= 1"};
  const double m = count;
  return normal_quantile((m - 0.375) / (m + 0.25));
}

// The Poisson-binomial distribution: independent trials whose success probabilities differ (here
// by group). The probability that FEWER than `successes` of all the groups' trials succeed, by
// dynamic programming over the trials (detail::fewer_than; O(trials x successes)). With one group
// it is the binomial P(Bin(n, p) < k). A kv_cluster asks it twice: how likely a quorum of q of a
// request's N replica-requests is NOT met (each kept with its machine's share: the request is
// lost), and, at each time t of the quadrature below, how likely fewer than q replicas have
// answered by t. 0 for successes <= 0 (that many always succeed), 1 beyond the trials.
// Assumes the trials are independent: one replica's fate says nothing of another's.
[[nodiscard]] double probability_fewer_than(std::span<const TrialGroup> groups, int successes) {
  if (successes <= 0) return 0.0;
  std::vector<double> below(static_cast<std::size_t>(successes));
  return detail::fewer_than(groups, below);
}

// How long a coordinator waits for the k-th fastest of a request's replicas when each group's
// replies are lognormal, fitted to that group's own sojourn moments (review of plan 2c Tasks 9-10,
// C-1). The k-th reply X is later than t exactly when fewer than k replicas have answered by t:
//   S_k(t) = P(X > t) = P(fewer than k successes),  replica i succeeding with F_i(t) = P(X_i <= t)
// the Poisson-binomial above over the groups, and the moments are E[X] = ∫ S_k dt and E[X^2] =
// ∫ 2 t S_k dt (moments_from_survival, on a grid even in ln t that spans every group's fit to
// 8 sigma, sigma / 2 apart: under 1e-8 relative error for sojourn SCVs up to 10, see
// docs/study/numerics.md). Where exponential_order_statistic needs every replica alike and
// exponential, this takes the replicas as they are: a hot machine beside cool ones, each with its
// own spread. Assumes the replicas' times are independent (one hot queue serves many keys, so
// they are not quite) and that a lognormal of the right mean and variance has the right shape (an
// M/M/c sojourn is not exactly one: the review's Monte Carlo of the kv-store lab's quorum lands
// within 3 %).
// Edge cases: a group of mean 0 answers at once; one without a finite mean and variance (a machine
// with no capacity) never answers, so a quorum it is needed for takes forever ({inf, inf}); a NaN
// anywhere is an upstream bug and comes out as NaN.
// Precondition: 1 <= k <= the replicas of all groups (programmer error: Graph::build refuses
// such a quorum), else std::invalid_argument.
[[nodiscard]] TimeMoments lognormal_order_statistic(std::span<const ReplicaGroup> groups, int k) {
  if (k < 1 || k > detail::replicas_in(groups)) {
    throw std::invalid_argument{"lognormal_order_statistic: k must be in [1, replicas]"};
  }
  constexpr double nan = std::numeric_limits<double>::quiet_NaN();
  constexpr double inf = std::numeric_limits<double>::infinity();
  // C++20 std::ranges::any_of: the predicate over a whole range, replacing any_of(begin, end, f).
  if (std::ranges::any_of(groups, detail::has_nan)) return {.mean_s = nan, .variance_s2 = nan};
  int quorum = k;
  for (const ReplicaGroup& group : groups) {
    if (detail::answers_at_once(group)) quorum -= group.replicas;
  }
  if (quorum <= 0) return {.mean_s = 0.0, .variance_s2 = 0.0};
  const auto fitted = detail::fitted_answering(groups);
  int answering = 0;
  for (const auto& group : fitted) answering += group.replicas;
  if (quorum > answering) return {.mean_s = inf, .variance_s2 = inf};
  return detail::integrate_quorum(fitted, quorum);
}

}  // namespace simsys::queueing
