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

// Popularity groups the keys into buckets, so Che's sums run over buckets, not over 1e8 keys:
// measured 3 ms in a Debug build and 1 ms in Release (2026-10-28). A pass over every key, once
// per bisection step, would take minutes. The bound is far from both: generous for a loaded CI
// machine running a Debug build, and still a failure for a per-key evaluation.
TEST_CASE("a hundred-million-key zipf space evaluates without a pass over every key", "[caching]") {
  const auto started = std::chrono::steady_clock::now();
  const auto h =
      che_hit_ratio(20e6, Popularity::zipf(1.0, 100'000'000));  // url-shortener's url_cache
  const auto elapsed = std::chrono::steady_clock::now() - started;
  REQUIRE(h > 0.5);
  REQUIRE(h < 1.0);
  REQUIRE(elapsed < std::chrono::seconds{2});
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
  // model owns what else it needs: the che policy carries its key popularity. A concept names
  // the requirement and checks it at compile time (static_assert below), replacing an abstract
  // base class with a virtual hit_ratio (no vtable, no common base) or SFINAE tricks.
  static_assert(simsys::caching::HitRatioModel<FixedHitRatio>);
  static_assert(simsys::caching::HitRatioModel<CheHitRatio>);
  REQUIRE_THAT(FixedHitRatio{0.8}.hit_ratio(10), WithinRel(0.8, 1e-12));
  REQUIRE_THAT(CheHitRatio{Popularity::uniform(100)}.hit_ratio(50), WithinRel(0.5, 1e-9));
}

namespace {
// True when buckets() can be called on a temporary Popularity (an rvalue, std::declval<P>()):
// the span it returns would view buckets destroyed at the end of the full expression.
// C++20 requires-expression: `requires { expr; }` is true when expr compiles, false otherwise,
// with no error; wrapped in a concept it becomes a compile-time test of an API's shape, replacing
// the C++17 detection idiom (std::void_t and a partial specialization). Here it proves that
// buckets() cannot be called on an rvalue (its const&& overload is deleted).
template <class P>
concept views_buckets_of_temporary = requires { std::declval<P>().buckets(); };
}  // namespace

TEST_CASE("buckets cannot be viewed on a temporary popularity", "[caching]") {
  STATIC_CHECK_FALSE(views_buckets_of_temporary<Popularity>);
}

// ---- Caches behind caches: the miss stream (Martina, Garetto & Leonardi 2014) ----

namespace {
// Dense reference for a second LRU cache fed by the misses of a first: solve Che for the first
// cache (capacity c1) over p_i = i^-s / H, filter q_i ∝ p_i e^{-p_i t1} (key i misses with
// probability e^{-p_i t1}), and solve Che again for the second cache (capacity c2) over q.
// The same bisection as dense_che_hit_ratio, written for any probability vector.
struct DenseChe {
  double hit_ratio;
  double characteristic_time;
};
DenseChe dense_che(const std::vector<double>& p, double capacity) {
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
  // C++20 designated initializers: each member named, replacing positional `{h, t}` where two
  // doubles could be swapped unnoticed.
  return {.hit_ratio = h, .characteristic_time = t};
}
std::vector<double> dense_zipf(double s, std::size_t n) {
  std::vector<double> p(n);
  double norm = 0;
  for (std::size_t i = n; i >= 1; --i) norm += p[i - 1] = std::pow(static_cast<double>(i), -s);
  for (auto& x : p) x /= norm;
  return p;
}
double dense_filtered_hit_ratio(double s, std::size_t n, double first, double second) {
  const auto p = dense_zipf(s, n);
  const double t1 = dense_che(p, first).characteristic_time;
  std::vector<double> q(n);
  double norm = 0;
  for (std::size_t i = 0; i < n; ++i) norm += q[i] = p[i] * std::exp(-p[i] * t1);
  for (auto& x : q) x /= norm;
  return dense_che(q, second).hit_ratio;
}
}  // namespace

TEST_CASE("the characteristic time of a uniform cache solves N(1 - e^(-t/N)) = C", "[caching]") {
  // p_i = 1/N: the Che equation N(1 − e^{−t/N}) = C gives t = −N ln(1 − C/N); half of 1000
  // keys is t = 1000 ln 2.
  REQUIRE_THAT(simsys::caching::che_characteristic_time(500, Popularity::uniform(1000)),
               WithinRel(1000 * std::log(2.0), 1e-9));
  // An empty cache forgets at once; a cache that holds every key never does.
  REQUIRE(simsys::caching::che_characteristic_time(0, Popularity::uniform(1000)) == 0.0);
  REQUIRE(simsys::caching::che_characteristic_time(1000, Popularity::uniform(1000)) == infinity);
}

TEST_CASE("the characteristic time matches the dense reference", "[caching]") {
  const auto dense = dense_che(dense_zipf(0.9, 100'000), 20'000);
  REQUIRE_THAT(simsys::caching::che_characteristic_time(20'000, Popularity::zipf(0.9, 100'000)),
               WithinRel(dense.characteristic_time, 1e-4));
}

// The misses of an LRU cache are not a sample of the clients' keys: the hot keys hit, so what
// goes on to the next tier is the cold tail. Filtering by e^{-p t} is that tail's popularity.
TEST_CASE("a popularity filtered by a cache's characteristic time matches the dense filtered sum",
          "[caching]") {
  const auto clients = Popularity::zipf(0.9, 100'000);
  const double t1 = simsys::caching::che_characteristic_time(20'000, clients);
  const auto misses = clients.filtered_by(t1);
  REQUIRE(misses.key_count() == clients.key_count());  // the same keys, different popularity
  for (const double second : {5'000.0, 20'000.0, 80'000.0}) {
    CAPTURE(second);
    REQUIRE_THAT(che_hit_ratio(second, misses),
                 WithinAbs(dense_filtered_hit_ratio(0.9, 100'000, 20'000, second), 1e-3));
  }
}

TEST_CASE("the second cache of two hits far less than its own size suggests", "[caching]") {
  // The review's chain at scale: zipf 0.9 over 10M keys, a 2M-key first cache. A second cache
  // of 2M keys over the RAW popularity would hit 0.747; over the first cache's misses it hits
  // 0.257, and 8M keys give 0.870 instead of 0.963. Reference values from the dense sum (the
  // helper above, run over all 10M keys once: too slow to repeat here in a Debug build).
  const auto clients = Popularity::zipf(0.9, 10e6);
  const auto misses = clients.filtered_by(simsys::caching::che_characteristic_time(2e6, clients));
  REQUIRE_THAT(che_hit_ratio(2e6, misses), WithinAbs(0.256607, 1e-3));
  REQUIRE_THAT(che_hit_ratio(8e6, misses), WithinAbs(0.870337, 1e-3));
}

TEST_CASE("filtering a uniform popularity leaves it uniform", "[caching]") {
  // Every key misses with the same probability e^{-t/N}, so the misses are uniform too.
  const auto misses = Popularity::uniform(1000).filtered_by(300.0);
  REQUIRE_THAT(che_hit_ratio(250, misses), WithinRel(0.25, 1e-9));
}

TEST_CASE("equal shards filter like one cache with their characteristic time", "[caching]") {
  // A key space split over caches (the arcs of a ring): key i misses in the cache that owns it.
  // Two halves with the same t filter exactly as one cache with that t.
  const auto clients = Popularity::zipf(1.0, 10'000);
  const std::vector<simsys::caching::CacheShard> halves{
      {.key_share = 0.5, .characteristic_time = 2000.0},
      {.key_share = 0.5, .characteristic_time = 2000.0}};
  REQUIRE_THAT(che_hit_ratio(1000, clients.filtered_by(halves)),
               WithinRel(che_hit_ratio(1000, clients.filtered_by(2000.0)), 1e-12));
}

TEST_CASE("the misses of a cache holding every key still form a popularity", "[caching]") {
  // t = infinity: no key ever misses. The limit of p e^{-p t}, renormalised, puts all the weight
  // on the coldest keys; it must stay a probability distribution, not 0/0.
  const auto misses = Popularity::zipf(1.0, 1000).filtered_by(infinity);
  double total = 0;
  for (const auto& bucket : misses.buckets()) {
    REQUIRE(std::isfinite(bucket.key_probability));
    total += bucket.count * bucket.key_probability;
  }
  REQUIRE_THAT(total, WithinRel(1.0, 1e-12));
  const double h = che_hit_ratio(10, misses);
  REQUIRE(h >= 0.0);
  REQUIRE(h <= 1.0);
}

TEST_CASE("the fixed policy ignores the capacity it is asked about", "[caching]") {
  // A stated ratio is the author's estimate for THIS cache; resizing it (a replica lost to a
  // fault, phase 3) cannot change what the author wrote.
  REQUIRE_THAT(FixedHitRatio{0.8}.hit_ratio(1), WithinRel(0.8, 1e-12));
  REQUIRE_THAT(FixedHitRatio{0.8}.hit_ratio(1e9), WithinRel(0.8, 1e-12));
}
