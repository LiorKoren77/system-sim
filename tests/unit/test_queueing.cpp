// Closed-form queueing results the fluid engine must reproduce. Values are from the
// textbook formulas (Kleinrock vol. 1, ch. 3) worked by hand in the comments.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.queueing;

using Catch::Approx;
using namespace simsys::queueing;

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
  REQUIRE(lognormal_quantile(0.010, 25e-6, z_p99) == Approx(0.02684).margin(2e-5));
}

TEST_CASE("lognormal_quantile of a zero mean is zero", "[queueing]") {
  REQUIRE(lognormal_quantile(0.0, 0.0, z_p95) == 0.0);
}
