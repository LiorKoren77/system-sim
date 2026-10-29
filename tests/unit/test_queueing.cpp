// Closed-form queueing results the fluid engine must reproduce. Values are from the
// textbook formulas (Kleinrock vol. 1, ch. 3) worked by hand in the comments.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>

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
