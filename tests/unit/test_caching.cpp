// The Che approximation for an LRU cache under a Zipf or uniform request distribution.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.caching;

// Catch2's Approx never tightens below ~1e-5 relative; closed forms are pinned with matchers.
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using simsys::caching::che_hit_ratio;
using simsys::caching::CheHitRatio;
using simsys::caching::FixedHitRatio;
using simsys::caching::Popularity;

namespace {
// The Che approximation summed densely, one probability per rank (p_i = i^-s / H_{N,s}), as
// the reference the bucketed sum must match. t_c by bisection on log t, from t = C (a lower
// bound, since each key contributes 1 - e^{-p t} <= p t, so the keys held at t are <= t).
double dense_che_hit_ratio(double s, std::size_t n, double capacity) {
  std::vector<double> p(n);
  double norm = 0;
  for (std::size_t i = n; i >= 1; --i) norm += p[i - 1] = std::pow(static_cast<double>(i), -s);
  for (auto& x : p) x /= norm;
  auto keys_in_cache = [&](double t) {
    double keys = 0;
    for (const double x : p) keys += -std::expm1(-x * t);
    return keys;
  };
  double lo = std::log(capacity);
  double hi = std::log(1e300);
  for (int k = 0; k < 100; ++k) {
    const double mid = (lo + hi) / 2;
    (keys_in_cache(std::exp(mid)) < capacity ? lo : hi) = mid;
  }
  const double t = std::exp(lo);
  double h = 0;
  for (const double x : p) h += x * -std::expm1(-x * t);
  return h;
}

constexpr double not_a_number = std::numeric_limits<double>::quiet_NaN();
constexpr double infinity = std::numeric_limits<double>::infinity();
}  // namespace

TEST_CASE("under a uniform distribution the hit ratio is the cache's share of the key space",
          "[caching]") {
  // p_i = 1/N: the Che equation gives 1 - e^{-t/N} = C/N exactly, so h = C/N.
  REQUIRE_THAT(che_hit_ratio(500, Popularity::uniform(1000)), WithinRel(0.5, 1e-9));
  REQUIRE_THAT(che_hit_ratio(1000, Popularity::uniform(1000)), WithinRel(1.0, 1e-9));
  REQUIRE_THAT(che_hit_ratio(0, Popularity::uniform(1000)), WithinAbs(0.0, 1e-12));
}

TEST_CASE("a tiny cache holds its share of a uniform key space however small", "[caching]") {
  // h = C/N holds for C far below one key too: the characteristic time is then about C,
  // well below any fixed lower bound a solver might assume.
  REQUIRE_THAT(che_hit_ratio(1e-15, Popularity::uniform(10)), WithinRel(1e-16, 1e-6));
}

TEST_CASE("a cache larger than the key space hits everything", "[caching]") {
  REQUIRE_THAT(che_hit_ratio(5000, Popularity::zipf(1.0, 1000)), WithinRel(1.0, 1e-12));
}

TEST_CASE("the bucketed Zipf sum matches a dense sum to within 1e-5 across skews and cache sizes",
          "[caching]") {
  // 20k keys: ranks above ~1500 share buckets, so the midpoint integral is exercised.
  for (const double s : {0.5, 1.0, 2.0}) {
    for (const double cap : {10.0, 1'000.0, 10'000.0}) {
      CAPTURE(s, cap);
      REQUIRE_THAT(che_hit_ratio(cap, Popularity::zipf(s, 20'000)),
                   WithinRel(dense_che_hit_ratio(s, 20'000, cap), 1e-5));
    }
  }
}

TEST_CASE("a bigger cache never hits less, up to the whole key space", "[caching]") {
  const auto popularity = Popularity::zipf(1.0, 100'000);
  double previous = 0.0;
  for (const double cap : {0.5, 1.0, 10.0, 1e3, 5e4, 99'999.0, 99'999.99, 100'000.0}) {
    CAPTURE(cap);
    const double h = che_hit_ratio(cap, popularity);
    REQUIRE(h >= previous);
    previous = h;
  }
  // Continuous at the `capacity >= N` shortcut: one key short of everything misses ~p_N.
  REQUIRE_THAT(che_hit_ratio(99'999.0, popularity), WithinAbs(1.0, 1e-5));
}

TEST_CASE("a one-key space hits in proportion to the fraction of the key the cache holds",
          "[caching]") {
  // N = 1: 1 - e^{-t} = C gives h = C exactly, for a fractional capacity too.
  REQUIRE_THAT(che_hit_ratio(0.25, Popularity::zipf(1.0, 1)), WithinRel(0.25, 1e-12));
  REQUIRE_THAT(che_hit_ratio(0.25, Popularity::uniform(1)), WithinRel(0.25, 1e-12));
  REQUIRE_THAT(che_hit_ratio(1.0, Popularity::zipf(1.0, 1)), WithinRel(1.0, 1e-12));
}

TEST_CASE("an LRU cache holding 20 percent of a zipf-1 key space serves about 88 percent of reads",
          "[caching]") {
  // The 80/20 rule of thumb: with s = 1 Che gives ~0.88 (LRU), a little below the ~0.915 a
  // cache holding exactly the hottest 20 % would serve (the LFU ideal); s ~ 0.9 gives ~0.76.
  REQUIRE_THAT(che_hit_ratio(20e6, Popularity::zipf(1.0, 100e6)), WithinAbs(0.88, 0.01));
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

TEST_CASE("NaN and infinity are rejected as programmer errors", "[caching]") {
  // A NaN fails every comparison, so a check written `x < 0` lets it through; these must not.
  REQUIRE_THROWS_AS(Popularity::zipf(not_a_number, 10), std::invalid_argument);
  REQUIRE_THROWS_AS(Popularity::zipf(infinity, 10), std::invalid_argument);
  REQUIRE_THROWS_AS(Popularity::zipf(1, not_a_number), std::invalid_argument);
  REQUIRE_THROWS_AS(Popularity::zipf(1, infinity), std::invalid_argument);
  REQUIRE_THROWS_AS(Popularity::uniform(not_a_number), std::invalid_argument);
  REQUIRE_THROWS_AS(Popularity::uniform(infinity), std::invalid_argument);
  REQUIRE_THROWS_AS(che_hit_ratio(not_a_number, Popularity::uniform(10)), std::invalid_argument);
}

TEST_CASE("uniform and zipf round a fractional space the same way", "[caching]") {
  // A key space is a count of whole keys: both factories floor 1000.5 to 1000.
  REQUIRE(Popularity::uniform(1000.5).key_count() == 1000.0);
  REQUIRE(Popularity::zipf(1.0, 1000.5).key_count() == 1000.0);
  REQUIRE_THAT(che_hit_ratio(500, Popularity::uniform(1000.5)), WithinRel(0.5, 1e-9));
}

TEST_CASE("the HitRatioModel concept is satisfied by the fixed and che policies", "[caching]") {
  // C++20 concepts: spec §5's HitRatioModel — anything with hit_ratio(capacity) -> double. A
  // model owns what else it needs: the che policy carries its key popularity.
  static_assert(simsys::caching::HitRatioModel<FixedHitRatio>);
  static_assert(simsys::caching::HitRatioModel<CheHitRatio>);
  REQUIRE_THAT(FixedHitRatio{0.8}.hit_ratio(10), WithinRel(0.8, 1e-12));
  REQUIRE_THAT(CheHitRatio{Popularity::uniform(100)}.hit_ratio(50), WithinRel(0.5, 1e-9));
}

namespace {
// True when buckets() can be called on a temporary Popularity (an rvalue, std::declval<P>()):
// the span it returns would view buckets destroyed at the end of the full expression.
template <class P>
concept views_buckets_of_temporary = requires { std::declval<P>().buckets(); };
}  // namespace

TEST_CASE("buckets cannot be viewed on a temporary popularity", "[caching]") {
  STATIC_CHECK_FALSE(views_buckets_of_temporary<Popularity>);
}
