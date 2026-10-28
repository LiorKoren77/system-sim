// Module `simsys.caching`: how much of the read traffic a cache absorbs (spec §5, §7 cache
// row; phase 2b refinement 2).
//
// Models: the cache-hit-ratio estimate behind System Design Interview ch. 1 and ch. 8
// ("a cache holding the hottest 20 % of keys serves ~80 % of reads"), computed from the
// clients' request popularity with the Che approximation for an LRU cache instead of
// guessed by hand. Captured: how the hit ratio grows with the cache size and with the skew
// of the key popularity. Ignored: temporal locality (a key requested now is no likelier to
// be requested again soon — the independent reference model below), TTL expiry, cold
// start (the estimate is the steady state), and eviction policies other than LRU (FIFO is
// within a few percent of LRU under this model; LFU does better than Che predicts).
//
// C++20 named module, interface unit (see src/core/core.cppm for the longer explanation).
// This interface imports no other module, and needs no heavy header anyway.
module;
#include <algorithm>
#include <cmath>
// C++20 <concepts>: the standard concept library (std::same_as, ...) used below.
#include <concepts>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

export module simsys.caching;

export namespace simsys::caching {

// A run of keys that share one (mean) request probability: `count` keys, each requested
// with probability `probability`. Popularity is a list of these, so Σ count·probability = 1.
struct Bucket {
  double count;
  double probability;
};

// Request popularity over a key space, under the *independent reference model* (IRM):
// every request picks key i independently of all earlier requests, with a fixed
// probability p_i. Two shapes:
//  - uniform: p_i = 1/N — every key equally hot (the worst case for a cache);
//  - Zipf:    p_i ∝ i^-s for ranks i = 1..N — a few keys get most of the requests
//    (book ch. 8, URL shortener: "a few short links get most of the clicks"). s = 0 is
//    uniform; s ≈ 1 is the classic web-object skew; larger s is hotter.
//
// A Zipf key space can hold 1e9 keys, too many to store one probability each. The ranks
// are therefore grouped into at most 2000 log-spaced buckets of keys with nearly the same
// probability (see zipf_buckets below); the Che sums then cost 2000 terms instead of 1e9.
class Popularity {
 public:
  // Pattern: Factory Method (named constructors) — `zipf(s, N)` and `uniform(N)` say
  // which distribution is meant, where a constructor taking (double, double) would not.
  // `space` is the number of keys N; a Zipf space is truncated to whole ranks.
  // Throws std::invalid_argument for exponent < 0 or space < 1: programmer errors, as
  // the scenario loader rejects such `keys` before they reach this module.
  [[nodiscard]] static Popularity zipf(double exponent, double space);
  [[nodiscard]] static Popularity uniform(double space);

  // C++20 std::span<const T>: a non-owning read-only view of contiguous elements. Replaces
  // returning `const std::vector<Bucket>&` (which exposes the container type) or a
  // pointer + length pair. The `&` qualifier forbids calling it on a temporary Popularity,
  // whose buckets would die before the span is used.
  [[nodiscard]] std::span<const Bucket> buckets() const& { return buckets_; }

  // N, the number of keys (Σ count over the buckets).
  [[nodiscard]] double key_count() const { return key_count_; }

 private:
  explicit Popularity(std::vector<Bucket> buckets);

  std::vector<Bucket> buckets_;
  double key_count_ = 0.0;
};

// The Che approximation for an LRU cache (Che, Tung & Wang 2002; Fricker, Robert &
// Roberts 2012 show it becomes exact for LRU as the key space grows).
//
// Idea: an LRU cache of C keys keeps a key while it has been requested within the last
// t_c seconds of "request time" — the *characteristic time*, the same for every key.
// Under IRM key i is requested at rate p_i (requests counted as time units), so it is in
// the cache with probability 1 − e^{−p_i t_c} (Poisson: at least one request in t_c).
//   characteristic time t_c solves   Σ_i (1 − e^{−p_i t_c}) = C   (the cache is full)
//   hit ratio                     h = Σ_i p_i (1 − e^{−p_i t_c})   (a request for i hits
//                                                                   iff i is cached)
// Assumptions: IRM (no temporal locality), LRU eviction, a large key space, C counted in
// keys (not bytes), steady state. `capacity >= N` returns 1 and `capacity == 0` returns 0.
// Throws std::invalid_argument for capacity < 0 (a programmer error, as above).
//
// Cost: a bisection of 80 steps over at most 2000 buckets — at most 160 000 exponentials,
// about half a millisecond (Release), once per cache when the graph is built.
[[nodiscard]] double che_hit_ratio(double capacity, const Popularity& popularity);

// C++20 concepts and `requires` expressions: a concept is a named compile-time predicate
// on a type. Replaces SFINAE and enable_if tricks (and "read the docs and hope" duck
// typing), so a type that lacks hit_ratio fails with a short "constraint not satisfied"
// error at the use site instead of pages of template instantiation errors.
// Spec §5's HitRatioModel: anything that answers hit_ratio(capacity, popularity) -> double.
template <class M>
concept HitRatioModel = requires(const M model, double capacity, const Popularity& popularity) {
  { model.hit_ratio(capacity, popularity) } -> std::same_as<double>;
};

// Pattern: Strategy/Policy — each cache's hit-ratio model is one of these, chosen at
// build time from the scenario's `hit_ratio: 0.8` (fixed) or `hit_ratio: che` (spec §5).

// `hit_ratio: <h>`: the scenario author's own estimate, used as given (phase 2a).
struct FixedHitRatio {
  double value;
  [[nodiscard]] double hit_ratio(double /*capacity*/, const Popularity& /*popularity*/) const {
    return value;
  }
};

// `hit_ratio: che`: computed from the cache size and the clients' key popularity.
struct CheHitRatio {
  [[nodiscard]] double hit_ratio(double capacity, const Popularity& popularity) const {
    return che_hit_ratio(capacity, popularity);
  }
};

}  // namespace simsys::caching

// ---- implementation --------------------------------------------------------------------

namespace simsys::caching {
namespace {

// Number of log-spaced buckets a Zipf key space is grouped into (phase 2b refinement 2).
// Error bound: the plan promises a relative hit-ratio error <= 1e-3, which the dense-sum
// test checks at 1e4 keys; measured against dense sums it is ~1e-7 for s in [0.6, 1.5]
// (inside a bucket p varies by under 1 %, and 1 − e^{−pt} is smooth). Cost: building
// the buckets of a 1e8-key space and solving Che takes about half a millisecond.
constexpr int zipf_bucket_count = 2000;

// ∫_a^b x^{−s} dx, in a form that stays accurate as s → 1:
//   (b^{1−s} − a^{1−s}) / (1−s) = a^{1−s} · expm1((1−s) ln(b/a)) / (1−s),
// and ln(b/a) at s = 1 exactly. std::expm1(u) = e^u − 1 without the cancellation of
// computing e^u and subtracting 1 when u is tiny.
[[nodiscard]] double power_integral(double exponent, double a, double b) {
  const double log_ratio = std::log(b / a);
  if (exponent == 1.0) return log_ratio;
  const double one_minus_s = 1.0 - exponent;
  return std::pow(a, one_minus_s) * std::expm1(one_minus_s * log_ratio) / one_minus_s;
}

// Mean of i^{−s} over the ranks first..last. A single rank is exact; a run of ranks uses
// the midpoint rule for sums, Σ_{i=first}^{last} f(i) ≈ ∫_{first−½}^{last+½} f(x) dx,
// whose relative error ~ s(s+1)/(24 i²) is below 1e-6 for the ranks (≥ ~500) where
// buckets hold more than one key.
[[nodiscard]] double mean_rank_weight(double exponent, double first, double last) {
  if (first == last) return std::pow(first, -exponent);
  return power_integral(exponent, first - 0.5, last + 0.5) / (last - first + 1.0);
}

// Upper rank edge of bucket k: ⌊N^{k/B}⌋, so the edges are evenly spaced in log(rank) and
// a bucket's probabilities differ by a factor of at most N^{s/B} (+1 % for s = 1 over 1e9
// keys) — but at least one more than the previous edge (so the first few hundred ranks
// get one key per bucket and no bucket is empty), and never beyond N (pow may land a hair
// off N at k = B).
[[nodiscard]] double next_rank_edge(double previous_edge, int k, double ranks) {
  const double log_spaced = std::floor(std::pow(ranks, static_cast<double>(k) / zipf_bucket_count));
  return k == zipf_bucket_count ? ranks
                                : std::min(ranks, std::max(previous_edge + 1.0, log_spaced));
}

// Zipf ranks 1..N grouped into log-spaced buckets of {count, unnormalised mean weight}.
[[nodiscard]] std::vector<Bucket> zipf_rank_buckets(double exponent, double ranks) {
  std::vector<Bucket> buckets;
  buckets.reserve(zipf_bucket_count);
  double edge = 0.0;
  for (int k = 1; k <= zipf_bucket_count && edge < ranks; ++k) {
    const double next_edge = next_rank_edge(edge, k, ranks);
    // C++20 designated initializers: name the fields at the construction site, so the
    // two doubles cannot be swapped silently (positional `{a, b}` would compile either way).
    buckets.push_back({.count = next_edge - edge,
                       .probability = mean_rank_weight(exponent, edge + 1.0, next_edge)});
    edge = next_edge;
  }
  return buckets;
}

// Divides every weight by the generalised harmonic number H_{N,s} = Σ count·weight, the
// Zipf normalising constant, so the probabilities sum to 1.
void normalise(std::vector<Bucket>& buckets) {
  double total = 0.0;
  for (const auto& bucket : buckets) total += bucket.count * bucket.probability;
  for (auto& bucket : buckets) bucket.probability /= total;
}

// 1 − e^{−x} for x ≥ 0 via expm1: exact to the last bit even when x is ~1e-12 (a cold key
// over a short characteristic time), where 1 − exp(−x) would round to 0.
[[nodiscard]] double one_minus_exp_neg(double x) {
  return -std::expm1(-x);
}

// Σ_i (1 − e^{−p_i t}): the expected number of distinct keys requested within t, i.e. the
// number of keys an LRU cache with characteristic time t holds. Increasing in t.
[[nodiscard]] double keys_in_cache(std::span<const Bucket> buckets, double t) {
  double keys = 0.0;
  for (const auto& bucket : buckets)
    keys += bucket.count * one_minus_exp_neg(bucket.probability * t);
  return keys;
}

// Solves keys_in_cache(t) = capacity for the characteristic time t_c, by bisection on
// log t: t_c spans many orders of magnitude (about C for a uniform space, up to ~1/p_N
// for a cache almost as large as a skewed space), and halving the interval in log space
// takes the same number of steps for every one of them. The bracket [1e-12, 1e300] holds
// every t_c a double can express; 80 halvings shrink its ~718 natural-log units below
// double precision. (The plan's [1e-9, 1e18] would cut t_c short for steep Zipf spaces —
// s = 3 over 1e9 keys has p_N ~ 1e-27.)
[[nodiscard]] double characteristic_time(std::span<const Bucket> buckets, double capacity) {
  double log_low = std::log(1e-12);
  double log_high = std::log(1e300);
  for (int step = 0; step < 80; ++step) {
    const double log_mid = (log_low + log_high) / 2.0;
    (keys_in_cache(buckets, std::exp(log_mid)) < capacity ? log_low : log_high) = log_mid;
  }
  return std::exp((log_low + log_high) / 2.0);
}

// h = Σ_i p_i (1 − e^{−p_i t}): the share of requests whose key is in the cache.
[[nodiscard]] double hit_ratio_at(std::span<const Bucket> buckets, double t) {
  double hits = 0.0;
  for (const auto& bucket : buckets)
    hits += bucket.count * bucket.probability * one_minus_exp_neg(bucket.probability * t);
  return hits;
}

}  // namespace

Popularity::Popularity(std::vector<Bucket> buckets) : buckets_{std::move(buckets)} {
  for (const auto& bucket : buckets_) key_count_ += bucket.count;
}

Popularity Popularity::zipf(double exponent, double space) {
  if (exponent < 0.0) throw std::invalid_argument{"zipf exponent must be >= 0"};
  if (space < 1.0) throw std::invalid_argument{"key space must hold at least one key"};
  auto buckets = zipf_rank_buckets(exponent, std::floor(space));
  normalise(buckets);
  return Popularity{std::move(buckets)};
}

Popularity Popularity::uniform(double space) {
  if (space < 1.0) throw std::invalid_argument{"key space must hold at least one key"};
  return Popularity{{{.count = space, .probability = 1.0 / space}}};
}

double che_hit_ratio(double capacity, const Popularity& popularity) {
  if (capacity < 0.0) throw std::invalid_argument{"cache capacity must be >= 0"};
  if (capacity >= popularity.key_count()) return 1.0;  // every key fits: only cold misses
  if (capacity == 0.0) return 0.0;
  const auto buckets = popularity.buckets();
  return hit_ratio_at(buckets, characteristic_time(buckets, capacity));
}

}  // namespace simsys::caching
