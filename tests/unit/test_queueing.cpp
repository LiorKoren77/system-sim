// Closed-form queueing results the fluid engine must reproduce. Values are from the
// textbook formulas (Kleinrock vol. 1, ch. 3) worked by hand in the comments.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <type_traits>
#include <utility>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.queueing;

using Catch::Approx;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using namespace simsys::queueing;

// A note for test authors: `Approx(x).margin(m)` passes when the value is within the
// absolute margin m OR within Approx's default relative epsilon (about 1.19e-5, i.e. 100
// float epsilons). So `.margin(1e-12)` does not tighten anything; a value 1e-6 off still
// passes. For a tight check use a matcher that applies only the tolerance you give it:
//   REQUIRE_THAT(v, WithinRel(expected, 1e-12));   // relative error <= 1e-12
//   REQUIRE_THAT(v, WithinAbs(expected, 1e-15));   // absolute error <= 1e-15

TEST_CASE("erlang_c for one server equals the utilization", "[queueing]") {
  // M/M/1: an arrival waits iff the server is busy, P(busy) = rho.
  REQUIRE(erlang_c(1, 0.5) == Approx(0.5));
  REQUIRE(erlang_c(1, 0.9) == Approx(0.9));
}

TEST_CASE("erlang_c matches hand-computed multi-server values", "[queueing]") {
  // c = 2, rho = 0.5: offered load a = 1 Erlang.
  // terms a^k/k! for k < c: 1, 1; last term a^c/c!/(1-rho) = 0.5/0.5 = 1 -> 1/(1+1+1).
  REQUIRE(erlang_c(2, 0.5) == Approx(1.0 / 3.0));
  // c = 3, rho = 0.8: a = 2.4; sum = 1 + 2.4 + 2.88 = 6.28; last = 2.304/0.2 = 11.52.
  REQUIRE(erlang_c(3, 0.8) == Approx(11.52 / (6.28 + 11.52)).margin(1e-6));
}

TEST_CASE("fewer than one server is a programmer error", "[queueing]") {
  REQUIRE_THROWS_AS(erlang_c(0, 0.5), std::invalid_argument);
  REQUIRE_THROWS_AS(mmc_wait(0, 0.5, 0.001), std::invalid_argument);
}

TEST_CASE("erlang_c is 0 with no load and 1 when saturated", "[queueing]") {
  REQUIRE(erlang_c(4, 0.0) == 0.0);
  REQUIRE(erlang_c(4, 1.0) == 1.0);
  REQUIRE(erlang_c(4, 1.7) == 1.0);
}

TEST_CASE("mmc_wait reproduces the M/M/1 mean wait rho/(mu - lambda)", "[queueing]") {
  // service time 1 ms (mu = 1000/s), rho = 0.5 (lambda = 500/s): Wq = 0.5/500 = 1 ms.
  const auto w = mmc_wait(1, 0.5, 0.001);
  REQUIRE(w.mean_s == Approx(0.001));
  // Wq is 0 with prob 1-Pw, else exponential with mean s/(c(1-rho)) = 2 ms:
  // Var = Pw(2-Pw) * (2 ms)^2 = 0.75 * 4e-6.
  REQUIRE(w.variance_s2 == Approx(0.75 * 4e-6));
}

TEST_CASE("mmc_wait for two servers scales by Erlang C over c(1 - rho)", "[queueing]") {
  const auto w = mmc_wait(2, 0.5, 0.001);
  REQUIRE(w.mean_s == Approx((1.0 / 3.0) * 0.001 / (2 * 0.5)));
}

TEST_CASE("mmc_wait is infinite at or beyond saturation", "[queueing]") {
  const auto w = mmc_wait(1, 1.0, 0.001);
  REQUIRE(w.mean_s == std::numeric_limits<double>::infinity());
  REQUIRE(w.variance_s2 == std::numeric_limits<double>::infinity());
  const auto beyond = mmc_wait(1, 1.7, 0.001);
  REQUIRE(beyond.mean_s == std::numeric_limits<double>::infinity());
  REQUIRE(beyond.variance_s2 == std::numeric_limits<double>::infinity());
}

TEST_CASE("variability_factor is 1 for Poisson arrivals and exponential service", "[queueing]") {
  REQUIRE(variability_factor(1.0, 1.0) == Approx(1.0));
}

TEST_CASE("variability_factor averages the two squared coefficients of variation", "[queueing]") {
  REQUIRE(variability_factor(2.0, 1.0) == Approx(1.5));  // bursty arrivals
  REQUIRE(variability_factor(0.0, 0.0) == Approx(0.0));  // D/D/c: nobody waits
}

TEST_CASE("lognormal_quantile with zero variance is the mean at every z", "[queueing]") {
  REQUIRE(lognormal_quantile(0.010, 0.0, z_p50) == Approx(0.010));
  REQUIRE(lognormal_quantile(0.010, 0.0, z_p99) == Approx(0.010));
}

TEST_CASE("lognormal_quantile median lies below the mean (right skew)", "[queueing]") {
  // mean 10 ms, sd 5 ms: median = m / sqrt(1 + v/m^2) = 10 / sqrt(1.25) = 8.944 ms
  REQUIRE(lognormal_quantile(0.010, 25e-6, z_p50) == Approx(0.008944).margin(1e-6));
}

TEST_CASE("lognormal_quantile p99 matches a hand-computed value", "[queueing]") {
  // sigma^2 = ln(1.25) = 0.22314, mu = ln(0.01) - sigma^2/2 = -4.71674,
  // q99 = exp(mu + 0.47238 * 2.32635) = exp(-3.6178) = 0.02684
  // Full precision (z_p99 = 2.3263478740408408): 0.0268411247647154.
  REQUIRE_THAT(lognormal_quantile(0.010, 25e-6, z_p99), WithinRel(0.0268411247647154, 1e-12));
}

TEST_CASE("lognormal_quantile p95 matches a hand-computed value", "[queueing]") {
  // Same fit as the p99 case (mean 10 ms, variance 25e-6 -> v/m^2 = 0.25):
  //   sigma^2 = ln(1.25)                      = 0.22314355131420976
  //   sigma   = sqrt(sigma^2)                 = 0.47238072707743883
  //   mu      = ln(0.01) - sigma^2/2          = -4.605170185988092 - 0.11157177565710488
  //                                           = -4.716741961645196
  //   q95     = exp(mu + sigma * 1.6448536269514722)
  //           = exp(-4.716741961645196 + 0.47238072707743883 * 1.6448536269514722)
  //           = exp(-4.716741961645196 + 0.77699715223) = exp(-3.93974480941)
  //           = 0.019453178380092202
  REQUIRE_THAT(lognormal_quantile(0.010, 25e-6, z_p95), WithinRel(0.019453178380092202, 1e-12));
}

TEST_CASE("lognormal_quantile of a zero mean is zero", "[queueing]") {
  REQUIRE(lognormal_quantile(0.0, 0.0, z_p95) == 0.0);
}

TEST_CASE("erlang_c stays finite for a thousand servers near saturation", "[queueing]") {
  // a = 990 Erlangs: a^c/c! is ~e^986 (ln = 1000 ln 990 - ln 1000!), beyond a double;
  // the Erlang B recurrence never leaves [0, 1].
  REQUIRE_THAT(erlang_c(1000, 0.99), WithinRel(0.6590804218808545, 1e-12));
}

TEST_CASE("mmc_wait variance uses Erlang C, not utilization, for several servers", "[queueing]") {
  // c = 2, rho = 0.5: P_wait = 1/3, theta = 1 ms / (2 * 0.5) = 1 ms; Var = (1/3)(5/3) theta^2.
  const auto w = mmc_wait(2, 0.5, 0.001);
  REQUIRE(w.variance_s2 == Approx(5.0 / 9.0 * 1e-6).margin(1e-15));
}

TEST_CASE("z constants are the standard-normal 50th, 95th and 99th percentiles", "[queueing]") {
  const auto phi = [](double z) { return 0.5 * std::erfc(-z / std::sqrt(2.0)); };
  // Within 1e-15, about ten ulps of a double near 1. A 1e-8 margin would not catch
  // 8-digit constants: 1.6448536 and 2.3263479 are off by 2.8e-9 and 6.9e-10 in
  // probability.
  REQUIRE_THAT(phi(z_p50), WithinAbs(0.50, 1e-15));
  REQUIRE_THAT(phi(z_p95), WithinAbs(0.95, 1e-15));
  REQUIRE_THAT(phi(z_p99), WithinAbs(0.99, 1e-15));
}

TEST_CASE("pooling servers lowers the chance of waiting at equal utilization", "[queueing]") {
  // The book's "add replicas behind a load balancer" lesson in one line: at rho 0.9,
  // sixteen pooled servers make an arrival wait far less often than one.
  REQUIRE(erlang_c(16, 0.9) < erlang_c(4, 0.9));
  REQUIRE(erlang_c(4, 0.9) < erlang_c(1, 0.9));
}

TEST_CASE("lognormal_quantile survives means whose square underflows a double", "[queueing]") {
  const auto q = lognormal_quantile(1.38e-162, 1.20e-166, z_p95);
  REQUIRE(std::isfinite(q));
  REQUIRE(q > 0.0);
}

// ---- Order statistics of exponentials (plan 2c Task 10; book ch. 6 quorum consensus) ----
//
// A kv_cluster's coordinator sends a read to all N replicas and answers after the R-th reply
// (the R-th FASTEST of N), a write after the W-th. If each replica's sojourn is exponential with
// mean m and the replicas are independent, the k-th smallest of n of them has a closed form
// (Renyi's representation): it is a sum of k independent exponentials, the i-th (i = 0 .. k-1)
// being the wait for the first of the n - i replicas still running to finish, with mean
// m / (n - i). So
//   mean     = m * sum_{i=0}^{k-1} 1 / (n - i)
//   variance = m^2 * sum_{i=0}^{k-1} 1 / (n - i)^2        (independent terms: variances add)
// Worked for n = 3 and m = 1 ms (the harmonic-like sums are 1/3, 1/3 + 1/2, 1/3 + 1/2 + 1):
//   k = 1  mean 1/3 = 0.3333 ms   variance 1/9              = 0.1111 ms^2
//   k = 2  mean 5/6 = 0.8333 ms   variance 1/9 + 1/4        = 0.3611 ms^2 (= 13/36)
//   k = 3  mean 11/6 = 1.8333 ms  variance 1/9 + 1/4 + 1    = 1.3611 ms^2 (= 49/36)
// The signature proposed here: exponential_order_statistic(n, k, mean_s) returns the same
// {mean_s, variance_s2} shape as mmc_wait (the project's moment pair), the mean in seconds.
TEST_CASE("the k-th fastest of three exponential replicas has Renyi's mean and variance",
          "[queueing]") {
  constexpr double m = 0.001;  // 1 ms
  SECTION("k = 1: the fastest of three, mean 1/3 ms, variance 1/9 ms^2") {
    const auto fastest = exponential_order_statistic(3, 1, m);
    REQUIRE_THAT(fastest.mean_s, WithinRel(1.0 / 3.0 * m, 1e-12));
    REQUIRE_THAT(fastest.variance_s2, WithinRel(1.0 / 9.0 * m * m, 1e-12));
  }
  SECTION("k = 2: the quorum of two, mean 5/6 ms, variance 13/36 ms^2") {
    const auto second = exponential_order_statistic(3, 2, m);
    REQUIRE_THAT(second.mean_s, WithinRel(5.0 / 6.0 * m, 1e-12));
    REQUIRE_THAT(second.mean_s, WithinRel(0.8333e-3, 1e-4));  // the book-sized number
    REQUIRE_THAT(second.variance_s2, WithinRel(13.0 / 36.0 * m * m, 1e-12));
    REQUIRE_THAT(second.variance_s2, WithinRel(0.3611e-6, 1e-4));
  }
  SECTION("k = 3: the slowest of three, mean 11/6 ms, variance 49/36 ms^2") {
    const auto slowest = exponential_order_statistic(3, 3, m);
    REQUIRE_THAT(slowest.mean_s, WithinRel(11.0 / 6.0 * m, 1e-12));
    REQUIRE_THAT(slowest.mean_s, WithinRel(1.8333e-3, 1e-4));
    REQUIRE_THAT(slowest.variance_s2, WithinRel(49.0 / 36.0 * m * m, 1e-12));
    REQUIRE_THAT(slowest.variance_s2, WithinRel(1.3611e-6, 1e-4));
  }
}

// n = 1, k = 1 is the exponential itself (mean m, variance m^2: an exponential's standard
// deviation equals its mean); k = n = 2 is the maximum of two, whose mean is the harmonic number
// H_2 = 1 + 1/2 = 1.5 times m (the slower of two coin-flip waits) and variance m^2 (1/4 + 1).
TEST_CASE("one replica is the exponential itself and the slowest of two is 1.5 means",
          "[queueing]") {
  const auto only = exponential_order_statistic(1, 1, 0.002);
  REQUIRE_THAT(only.mean_s, WithinRel(0.002, 1e-12));
  REQUIRE_THAT(only.variance_s2, WithinRel(0.002 * 0.002, 1e-12));
  const auto slower_of_two = exponential_order_statistic(2, 2, 0.002);
  REQUIRE_THAT(slower_of_two.mean_s, WithinRel(1.5 * 0.002, 1e-12));
  REQUIRE_THAT(slower_of_two.variance_s2, WithinRel(1.25 * 0.002 * 0.002, 1e-12));
}

// The mean scales with m and the variance with m^2 (a change of time unit): ten times the
// replica sojourn is ten times the quorum's mean and a hundred times its variance.
TEST_CASE(
    "an order statistic scales its mean with the replica mean and its variance with the square",
    "[queueing]") {
  const auto base = exponential_order_statistic(5, 3, 0.001);
  const auto tenfold = exponential_order_statistic(5, 3, 0.010);
  REQUIRE_THAT(tenfold.mean_s, WithinRel(10.0 * base.mean_s, 1e-12));
  REQUIRE_THAT(tenfold.variance_s2, WithinRel(100.0 * base.variance_s2, 1e-12));
}

// Waiting for one more replica never makes the answer sooner: the k-th smallest grows with k.
TEST_CASE("waiting for more replicas lengthens the mean and the variance", "[queueing]") {
  for (int k = 1; k < 5; ++k) {
    CAPTURE(k);
    const auto fewer = exponential_order_statistic(5, k, 0.001);
    const auto more = exponential_order_statistic(5, k + 1, 0.001);
    REQUIRE(more.mean_s > fewer.mean_s);
    REQUIRE(more.variance_s2 > fewer.variance_s2);
  }
}

// A quorum larger than the replica count, or smaller than one, is not a question this function
// can answer: callers (the loader and Graph::build) have already refused it, so it is a
// programmer error and throws, like erlang_c with no servers.
TEST_CASE("a quorum beyond the replicas or below one is a programmer error", "[queueing]") {
  REQUIRE_THROWS_AS(exponential_order_statistic(3, 4, 0.001), std::invalid_argument);
  REQUIRE_THROWS_AS(exponential_order_statistic(3, 0, 0.001), std::invalid_argument);
  REQUIRE_THROWS_AS(exponential_order_statistic(0, 1, 0.001), std::invalid_argument);
}

// The moment pair the order statistics return is the project's TimeMoments, the same type
// mmc_wait returns for a wait: a sojourn is a time like a wait, and the old name WaitMoments
// read wrong at a quorum's reply (review of plan 2c Tasks 9-10, minor 10).
TEST_CASE("an order statistic and a wait share one moment pair, TimeMoments", "[queueing]") {
  STATIC_REQUIRE(std::is_same_v<decltype(exponential_order_statistic(3, 2, 0.001)), TimeMoments>);
  STATIC_REQUIRE(std::is_same_v<decltype(mmc_wait(1, 0.5, 0.001)), TimeMoments>);
}

// ---- The standard normal (review of plan 2c Tasks 9-10, I-2 and C-1) ----
//
// Phi(z) = P(Z <= z) for a standard normal Z, and its inverse. Phi is erfc in disguise:
// Phi(z) = erfc(-z / sqrt 2) / 2. The inverse has no closed form; the reference values below are
// the textbook quantiles to 16 digits (Phi^-1(0.975) = 1.959963984540054, the "1.96" of every
// 95 % confidence interval).
TEST_CASE("normal_cdf is one half at zero and the tables' values elsewhere", "[queueing]") {
  REQUIRE(normal_cdf(0.0) == 0.5);
  REQUIRE_THAT(normal_cdf(1.959963984540054), WithinAbs(0.975, 1e-15));
  REQUIRE_THAT(normal_cdf(-z_p99), WithinAbs(0.01, 1e-15));
}

TEST_CASE("normal_quantile inverts normal_cdf to full double precision", "[queueing]") {
  REQUIRE(normal_quantile(0.5) == 0.0);
  REQUIRE_THAT(normal_quantile(0.975), WithinRel(1.959963984540054, 1e-13));
  REQUIRE_THAT(normal_quantile(0.95), WithinRel(z_p95, 1e-13));
  REQUIRE_THAT(normal_quantile(0.99), WithinRel(z_p99, 1e-13));
  REQUIRE_THAT(normal_quantile(0.01), WithinRel(-z_p99, 1e-13));
  // Deep in the tails, where a rational approximation alone loses digits: 1e-10 below.
  REQUIRE_THAT(normal_quantile(1e-10), WithinRel(-6.361340902404056, 1e-12));
}

TEST_CASE("normal_quantile outside the open interval from 0 to 1 is a programmer error",
          "[queueing]") {
  REQUIRE_THROWS_AS(normal_quantile(0.0), std::invalid_argument);
  REQUIRE_THROWS_AS(normal_quantile(1.0), std::invalid_argument);
  REQUIRE_THROWS_AS(normal_quantile(std::numeric_limits<double>::quiet_NaN()),
                    std::invalid_argument);
}

// ---- The expected maximum of M standard normals: the ring's hottest machine (I-2) ----
//
// A consistent-hash ring gives each of M machines a share of the keys that varies around the
// average; the hottest machine is the MAXIMUM of M such shares, and the maximum of M standard
// normals grows with M (about sqrt(2 ln M) for large M). Its mean has no closed form beyond M = 5;
// Blom's approximation (1958) puts it at the plotting position
//   z_M = Phi^-1((M - 0.375) / (M + 0.25))
// M = 1 gives Phi^-1(0.5) = 0 (one machine IS the average). Exact values for comparison: M = 2,
// 1/sqrt(pi) = 0.5642 (Blom 0.5895, 4.5 % high); M = 4, 1.0294 (Blom 1.0491, 1.9 % high). Blom's
// error shrinks as M grows, and the ring heuristic built on it is checked against Monte Carlo in
// docs/study/numerics.md.
TEST_CASE("the expected maximum of M normals follows Blom's plotting position", "[queueing]") {
  REQUIRE(expected_normal_maximum(1) == 0.0);
  REQUIRE_THAT(expected_normal_maximum(2), WithinRel(0.5894557978497785, 1e-12));
  REQUIRE_THAT(expected_normal_maximum(4), WithinRel(1.0491313979639711, 1e-12));
  REQUIRE_THAT(expected_normal_maximum(15), WithinRel(1.7393841569195554, 1e-12));
  REQUIRE_THAT(expected_normal_maximum(16), WithinRel(1.7688250385187059, 1e-12));
  REQUIRE_THAT(expected_normal_maximum(128), WithinRel(2.5846924742431137, 1e-12));
  // Close to the exact expected maxima: within 5 % at M = 2, 2 % at M = 4.
  // C++20 std::numbers::pi (<numbers>): the constant as a typed double, replacing the
  // non-standard M_PI macro or a hand-typed literal.
  REQUIRE_THAT(expected_normal_maximum(2), WithinRel(1.0 / std::sqrt(std::numbers::pi), 0.05));
  REQUIRE_THAT(expected_normal_maximum(4), WithinRel(1.0293753730039641, 0.02));
}

TEST_CASE("the expected maximum of no machines is a programmer error", "[queueing]") {
  REQUIRE_THROWS_AS(expected_normal_maximum(0), std::invalid_argument);
}

// ---- The Poisson-binomial: how many of a quorum's replicas succeed (I-1, C-1) ----
//
// N independent trials, the ones of a group sharing one success probability: how likely are FEWER
// than q successes? With one group it is the binomial, P(Bin(N, p) < q). A kv_cluster whose
// machines each drop a ninth of what they are sent (p = 8/9 kept) loses a request whose quorum q
// of N = 3 replica-requests does not come back:
//   q = 1  (1/9)^3                          = 0.001372   (0.14 %: every replica must fail)
//   q = 2  (1/9)^3 + 3 (8/9)(1/9)^2         = 0.034294   (3.4 %)
//   q = 3  1 - (8/9)^3                      = 0.297668   (29.8 %: any one failure loses it)
// against the one ninth (11.1 %) that "the share of its replica-requests dropped" gives at every q.
TEST_CASE("fewer than q of N replicas succeed with the binomial's probability", "[queueing]") {
  // C++20 designated initializers: `.trials = 3` names the member it sets, replacing positional
  // `{3, 8.0 / 9.0}`, where a count and a probability could be swapped unnoticed.
  const std::array kept{TrialGroup{.trials = 3, .success = 8.0 / 9.0}};
  REQUIRE_THAT(probability_fewer_than(kept, 1), WithinRel(1.0 / 729.0, 1e-12));
  REQUIRE_THAT(probability_fewer_than(kept, 2), WithinRel(25.0 / 729.0, 1e-12));
  REQUIRE_THAT(probability_fewer_than(kept, 3), WithinRel(1.0 - 512.0 / 729.0, 1e-12));
  REQUIRE_THAT(probability_fewer_than(kept, 2), WithinAbs(0.034294, 1e-6));
}

// Two groups of different probabilities: one hot replica that succeeds half the time and two
// that always do. Three successes need the hot one (1/2); two always happen (0); one always does.
TEST_CASE("a Poisson-binomial over two groups counts their successes together", "[queueing]") {
  const std::array groups{TrialGroup{.trials = 1, .success = 0.5},
                          TrialGroup{.trials = 2, .success = 1.0}};
  REQUIRE(probability_fewer_than(groups, 3) == 0.5);
  REQUIRE(probability_fewer_than(groups, 2) == 0.0);
  REQUIRE(probability_fewer_than(groups, 1) == 0.0);
  // At least zero successes always happen; more successes than trials never do.
  REQUIRE(probability_fewer_than(groups, 0) == 0.0);
  REQUIRE(probability_fewer_than(groups, 4) == 1.0);
}

// ---- A quorum's reply time by quadrature (C-1) ----
//
// The k-th fastest of N replies, X, is later than t exactly when fewer than k replicas have
// answered by t: P(X > t) = S_k(t), the Poisson-binomial above with each replica's success
// probability its F(t) = P(reply by t). Its moments follow from the survival function:
//   E[X] = integral of S_k(t) dt,    E[X^2] = integral of 2 t S_k(t) dt     (t from 0 to infinity)
// moments_from_survival integrates them on a grid even in ln t. The cross-check: N exponential
// replicas of mean m must give Renyi's closed form (exponential_order_statistic), to 1e-9.
TEST_CASE("the survival-function quadrature reproduces Renyi for exponential replicas",
          "[queueing]") {
  constexpr double m = 0.001;
  // From m e^-30 (no replica has answered: S = 1 below) to m e^4 (e^-e^4 = 2e-24: all have).
  const LogGrid grid{.first_log_s = std::log(m) - 30.0, .step = 0.25, .points = 137};
  for (const auto& [n, k] :
       {std::pair{1, 1}, std::pair{3, 1}, std::pair{3, 2}, std::pair{3, 3}, std::pair{5, 3}}) {
    CAPTURE(n, k);
    const auto quadrature = moments_from_survival(grid, [&](double log_t) {
      const std::array replicas{
          TrialGroup{.trials = n, .success = -std::expm1(-std::exp(log_t) / m)}};
      return probability_fewer_than(replicas, k);
    });
    const auto renyi = exponential_order_statistic(n, k, m);
    REQUIRE_THAT(quadrature.mean_s, WithinRel(renyi.mean_s, 1e-9));
    REQUIRE_THAT(quadrature.variance_s2, WithinRel(renyi.variance_s2, 1e-9));
  }
}

// One replica is its own order statistic: the lognormal fitted to its moments, integrated back,
// returns them. This is the quadrature's documented error bound (docs/study/numerics.md section
// 7): below 1e-8 relative at every sojourn SCV from 1e-4 to 100 (measured: a few 1e-16, the
// variance at SCV 1e-4 1.2e-11). Before the Poisson-binomial carried each replica's failure
// Phi(-z) (review of the kv/search fix round, M-3) it held to SCV 10 only: 1 - Phi(z) lost the
// slow tail's digits, 1.1e-7 on the variance at SCV 20 and 1.7e-6 at 50.
TEST_CASE("one lognormal replica's order statistic returns its own moments", "[queueing]") {
  for (const double scv : {1e-4, 0.01, 0.5, 1.0, 2.0, 10.0, 20.0, 50.0, 100.0}) {
    CAPTURE(scv);
    const double m = 0.37e-3;
    const std::array one{
        ReplicaGroup{.replicas = 1, .sojourn = {.mean_s = m, .variance_s2 = scv * m * m}}};
    const auto answer = lognormal_order_statistic(one, 1);
    REQUIRE_THAT(answer.mean_s, WithinRel(m, 1e-8));
    REQUIRE_THAT(answer.variance_s2, WithinRel(scv * m * m, 1e-8));
  }
}

// Three replicas of mean 1 ms and SCV 1 (an M/M/c sojourn at light load), each a lognormal with
// sigma^2 = ln 2: the k-th fastest, computed independently (a Python quadrature of the same
// integrals). Next to Renyi's exponentials of the same mean (1/3, 5/6, 11/6 ms) the lognormal's
// lighter left tail makes the fastest slower (0.4210 ms) and its lighter right tail the slowest
// faster (1.7528 ms); the quorum of two barely moves (0.8262 against 0.8333 ms).
TEST_CASE("the k-th fastest of three lognormal replicas of SCV one", "[queueing]") {
  const std::array three{
      ReplicaGroup{.replicas = 3, .sojourn = {.mean_s = 1e-3, .variance_s2 = 1e-6}}};
  const auto first = lognormal_order_statistic(three, 1);
  REQUIRE_THAT(first.mean_s, WithinRel(4.2099631630e-4, 1e-8));
  REQUIRE_THAT(first.variance_s2, WithinRel(7.3174194295e-8, 1e-7));
  const auto second = lognormal_order_statistic(three, 2);
  REQUIRE_THAT(second.mean_s, WithinRel(8.2618489437e-4, 1e-8));
  REQUIRE_THAT(second.variance_s2, WithinRel(2.5078568374e-7, 1e-7));
  const auto third = lognormal_order_statistic(three, 3);
  REQUIRE_THAT(third.mean_s, WithinRel(1.7528187893e-3, 1e-8));
  REQUIRE_THAT(third.variance_s2, WithinRel(1.7438470357e-6, 1e-7));
}

// A request whose replicas are one hot machine (100 ms, sd 10 ms: a full queue) and two cool ones
// (1 ms, SCV 1): the quorum of two is the slower of the two cool replies (1.4439 ms), the hot one
// almost never among the first two; the fastest is a cool one, a little sooner than either
// (0.5561 ms); the third reply is the hot machine's own 100 ms. The hot replica is hidden unless
// the quorum needs every replica.
TEST_CASE("a quorum of two hides a hot replica behind two cool ones", "[queueing]") {
  const std::array replicas{
      ReplicaGroup{.replicas = 1, .sojourn = {.mean_s = 0.1, .variance_s2 = 1e-4}},
      ReplicaGroup{.replicas = 2, .sojourn = {.mean_s = 1e-3, .variance_s2 = 1e-6}}};
  REQUIRE_THAT(lognormal_order_statistic(replicas, 1).mean_s, WithinRel(5.5605917566e-4, 1e-7));
  REQUIRE_THAT(lognormal_order_statistic(replicas, 2).mean_s, WithinRel(1.4439407717e-3, 1e-7));
  REQUIRE_THAT(lognormal_order_statistic(replicas, 3).mean_s, WithinRel(0.1, 1e-7));
}

// A machine with no capacity never answers (an infinite sojourn): a quorum the others can fill
// still answers, one that needs it never does. A NaN anywhere is an upstream bug and stays NaN.
TEST_CASE("a replica that never answers delays only a quorum that needs it", "[queueing]") {
  constexpr double inf = std::numeric_limits<double>::infinity();
  const std::array replicas{
      ReplicaGroup{.replicas = 1, .sojourn = {.mean_s = inf, .variance_s2 = inf}},
      ReplicaGroup{.replicas = 2, .sojourn = {.mean_s = 1e-3, .variance_s2 = 1e-6}}};
  REQUIRE_THAT(lognormal_order_statistic(replicas, 2).mean_s, WithinRel(1.4439408243e-3, 1e-7));
  REQUIRE(lognormal_order_statistic(replicas, 3).mean_s == inf);
  const std::array poisoned{ReplicaGroup{
      .replicas = 3,
      .sojourn = {.mean_s = std::numeric_limits<double>::quiet_NaN(), .variance_s2 = 1e-6}}};
  REQUIRE(std::isnan(lognormal_order_statistic(poisoned, 2).mean_s));
}

// A quorum larger than the replicas is a programmer error here too (Graph::build refuses it).
TEST_CASE("a lognormal quorum beyond its replicas is a programmer error", "[queueing]") {
  const std::array two{
      ReplicaGroup{.replicas = 2, .sojourn = {.mean_s = 1e-3, .variance_s2 = 1e-6}}};
  REQUIRE_THROWS_AS(lognormal_order_statistic(two, 3), std::invalid_argument);
  REQUIRE_THROWS_AS(lognormal_order_statistic(two, 0), std::invalid_argument);
}

// ---- lognormal_below: a client's timeout cuts the slow tail (plan 2c Task 13) ----
//
// A client that gives up after `limit` turns every response slower than that into an unanswered
// request. With the response time fitted as a lognormal (the same fit the reports use), the share
// answered in time and the moments of that share follow in closed form. Derivation, for a
// lognormal X with mean m and variance v:
//   sigma^2 = ln(1 + v/m^2),  mu = ln m - sigma^2/2                 (lognormal_fit)
//   P(X < L) = Phi(z),  z = (ln L - mu) / sigma                      (Phi = normal_cdf)
//   E[X^k 1{X<L}] = exp(k mu + k^2 sigma^2 / 2) Phi(z - k sigma)    (k = 1, 2: the partial moments)
//   E[X | X<L] = E[X 1{X<L}] / Phi(z),   Var[X | X<L] = E[X^2 | X<L] - E[X | X<L]^2
// Worked for m = 100 ms, v = 0.01 s^2 (an exponential: v/m^2 = 1), so sigma^2 = ln 2 = 0.693147,
// sigma = 0.832555, mu = ln 0.1 - 0.346574 = -2.649159:
//   L = 200 ms: z = (ln 0.2 + 2.649159) / 0.832555 = 1.24883, Phi = 0.894137,
//     E[X 1{X<L}] = 0.1 Phi(z - sigma) = 0.1 Phi(0.416277) = 0.066140  ->  mean 0.073970 s,
//     E[X^2 1{X<L}] = exp(2 mu + 2 sigma^2) Phi(z - 2 sigma) = 0.0200 Phi(-0.416277) = 0.0067721
//     -> E[X^2 | X<L] = 0.0067721 / 0.894137 = 0.0075739, variance 0.0075739 - 0.073970^2 =
//     0.0021022 s^2.
//   L = 1 s: z = 3.18196, Phi = 0.99927;   L = 50 ms: z = -0.41628, Phi = 0.33860.
// All three recomputed independently with Python's math.erfc and agree with the plan to the digits
// the plan prints (checked 2026-10-29: fraction 0.8941367, mean 0.07397039, variance 0.002102246).
// Assumption: the response is lognormal, which it only approximates; the engine applies the same
// fit to the percentiles it reports.
TEST_CASE("a lognormal with mean 100 ms and variance 0.01 s2 answers 89.4 percent below 200 ms",
          "[queueing]") {
  const auto below = lognormal_below(0.1, 0.01, 0.2);
  REQUIRE_THAT(below.fraction, WithinAbs(0.89414, 5e-6));
  REQUIRE_THAT(below.mean_s, WithinAbs(0.073970, 5e-7));
  REQUIRE_THAT(below.variance_s2, WithinAbs(0.0021022, 5e-8));
  // Truncating can only shorten the mean, and the part that is left spreads less.
  REQUIRE(below.mean_s < 0.1);
  REQUIRE(below.variance_s2 < 0.01);
}

TEST_CASE(
    "the share below a limit grows with the limit: 33.86 percent at 50 ms, 99.927 percent at 1 s",
    "[queueing]") {
  REQUIRE_THAT(lognormal_below(0.1, 0.01, 0.05).fraction, WithinAbs(0.33860, 5e-6));
  REQUIRE_THAT(lognormal_below(0.1, 0.01, 1.0).fraction, WithinAbs(0.99927, 5e-6));
  // A limit far above the mean cuts almost nothing: the truncated moments approach the input's.
  const auto loose = lognormal_below(0.1, 0.01, 1.0);
  REQUIRE_THAT(loose.mean_s, WithinAbs(0.099132, 5e-7));
  REQUIRE_THAT(loose.variance_s2, WithinAbs(0.0088934, 5e-8));
}

TEST_CASE("an infinite limit answers everything and leaves the moments as they were",
          "[queueing]") {
  const auto below = lognormal_below(0.1, 0.01, std::numeric_limits<double>::infinity());
  REQUIRE(below.fraction == 1.0);
  REQUIRE_THAT(below.mean_s, WithinRel(0.1, 1e-12));
  REQUIRE_THAT(below.variance_s2, WithinRel(0.01, 1e-12));
}

// A response with no spread (a deterministic 100 ms: sigma = 0) is either wholly below the limit or
// wholly above it: the step function the lognormal tends to as v goes to 0.
TEST_CASE("a response with no variance is answered whole below the limit and not at all above it",
          "[queueing]") {
  const auto fast = lognormal_below(0.1, 0.0, 0.2);
  REQUIRE(fast.fraction == 1.0);
  REQUIRE_THAT(fast.mean_s, WithinRel(0.1, 1e-12));
  REQUIRE(fast.variance_s2 == 0.0);
  REQUIRE(lognormal_below(0.1, 0.0, 0.05).fraction == 0.0);
}

// youtube's 300 MB uploads take 12 s with almost no spread (sigma of ln X = 0.0137 at variance
// 0.027 s^2, 2.9e-4 at 1e-6 s^2) against a 10 s timeout: z = (ln 10 - mu) / sigma = -13.3, or
// -630, and Phi(-630) underflows to exactly 0. The truncated moments are then 0 / 0; they must stay
// finite numbers, because the reports weigh every moment by the requests answered (0 x NaN is NaN
// and would poison the sums of the steps that did answer something).
TEST_CASE("a limit far below a path that never beats it answers nothing and keeps finite moments",
          "[queueing]") {
  const auto near = lognormal_below(12.0, 0.027, 10.0);
  REQUIRE(near.fraction < 1e-30);
  REQUIRE(near.fraction >= 0.0);
  REQUIRE(std::isfinite(near.mean_s));
  const auto far = lognormal_below(12.0, 1e-6, 10.0);
  REQUIRE(far.fraction == 0.0);
  REQUIRE(std::isfinite(far.mean_s));
  REQUIRE(std::isfinite(far.variance_s2));
  REQUIRE(far.variance_s2 >= 0.0);
}
