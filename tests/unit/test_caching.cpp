// The Che approximation for an LRU cache under a Zipf or uniform request distribution.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.caching;

// Catch2's Approx never tightens below ~1e-5 relative; closed forms are pinned with matchers.
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using simsys::caching::che_hit_ratio;
using simsys::caching::Popularity;

TEST_CASE("under a uniform distribution the hit ratio is the cache's share of the key space",
          "[caching]") {
  // p_i = 1/N: the Che equation gives 1 - e^{-t/N} = C/N exactly, so h = C/N.
  REQUIRE_THAT(che_hit_ratio(500, Popularity::uniform(1000)), WithinRel(0.5, 1e-9));
  REQUIRE_THAT(che_hit_ratio(1000, Popularity::uniform(1000)), WithinRel(1.0, 1e-9));
  REQUIRE_THAT(che_hit_ratio(0, Popularity::uniform(1000)), WithinAbs(0.0, 1e-12));
}

TEST_CASE("a cache larger than the key space hits everything", "[caching]") {
  REQUIRE_THAT(che_hit_ratio(5000, Popularity::zipf(1.0, 1000)), WithinRel(1.0, 1e-12));
}

TEST_CASE("the bucketed Zipf sum matches a dense sum to a tenth of a percent", "[caching]") {
  // Dense reference: p_i = i^-s / H, 10k keys, C = 100, solved by bisection here in the test.
  const double s = 1.0;
  const int n = 10'000;
  const double cap = 100;
  std::vector<double> p(n);
  double h_n = 0;
  for (int i = 1; i <= n; ++i) {
    p[static_cast<std::size_t>(i - 1)] = std::pow(i, -s);
    h_n += p[static_cast<std::size_t>(i - 1)];
  }
  for (auto& x : p) x /= h_n;
  auto in_cache = [&](double t) {
    double sum = 0;
    for (double x : p) sum += 1 - std::exp(-x * t);
    return sum;
  };
  double lo = 0, hi = 1e12;
  for (int k = 0; k < 200; ++k) {
    const double mid = (lo + hi) / 2;
    (in_cache(mid) < cap ? lo : hi) = mid;
  }
  double h_ref = 0;
  for (double x : p) h_ref += x * (1 - std::exp(-x * lo));
  REQUIRE_THAT(che_hit_ratio(cap, Popularity::zipf(s, n)), WithinRel(h_ref, 1e-3));
}

TEST_CASE("a hotter key space (larger zipf exponent) hits more", "[caching]") {
  const auto cold = che_hit_ratio(1000, Popularity::zipf(0.6, 1'000'000));
  const auto hot = che_hit_ratio(1000, Popularity::zipf(1.2, 1'000'000));
  REQUIRE(hot > cold);
  REQUIRE(cold > che_hit_ratio(1000, Popularity::uniform(1'000'000)));
}

TEST_CASE("a hundred-million-key zipf space evaluates in well under a second", "[caching]") {
  const auto started = std::chrono::steady_clock::now();
  const auto h =
      che_hit_ratio(20e6, Popularity::zipf(1.0, 100'000'000));  // url-shortener's url_cache
  const auto elapsed = std::chrono::steady_clock::now() - started;
  REQUIRE(h > 0.5);
  REQUIRE(h < 1.0);
  REQUIRE(elapsed < std::chrono::milliseconds{500});
}

TEST_CASE("invalid popularities are programmer errors", "[caching]") {
  REQUIRE_THROWS_AS(Popularity::zipf(-0.1, 10), std::invalid_argument);
  REQUIRE_THROWS_AS(Popularity::uniform(0), std::invalid_argument);
  REQUIRE_THROWS_AS(che_hit_ratio(-1, Popularity::uniform(10)), std::invalid_argument);
}

TEST_CASE("the HitRatioModel concept is satisfied by the fixed and che policies", "[caching]") {
  // C++20 concepts: spec §5's HitRatioModel — anything with hit_ratio(capacity, popularity) ->
  // double.
  static_assert(simsys::caching::HitRatioModel<simsys::caching::FixedHitRatio>);
  static_assert(simsys::caching::HitRatioModel<simsys::caching::CheHitRatio>);
  REQUIRE_THAT(simsys::caching::FixedHitRatio{0.8}.hit_ratio(10, Popularity::uniform(100)),
               WithinRel(0.8, 1e-12));
  REQUIRE_THAT(simsys::caching::CheHitRatio{}.hit_ratio(50, Popularity::uniform(100)),
               WithinRel(0.5, 1e-9));
}
