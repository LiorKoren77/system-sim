// Module `simsys.caching`: how much of the read traffic a cache absorbs (spec §5, §7 cache row;
// spec §18 entry 2).
//
// Models: the hit ratio of the cache tier of System Design Interview ch. 1 (and of the URL
// shortener's cache in ch. 8), computed from the clients' request popularity with the Che
// approximation for an LRU cache instead of guessed by hand. The usual guess is the 80/20
// rule, "20 % of the keys serve 80 % of the reads": an industry rule of thumb, not a figure
// from the book. "The hottest 20 %" describes an ideal cache that always holds exactly the
// most popular keys (perfect LFU); an LRU cache keeps whatever was requested recently and
// does a little worse. For Zipf(s) over 1e8 keys and a cache of 20 % of them:
//      s              0.6    0.7    0.8    0.9    1.0    1.2
//      ideal (LFU)    0.53   0.62   0.72   0.83   0.92   0.99   share of the hottest 20 %
//      LRU (Che)      0.39   0.49   0.62   0.76   0.88   0.99   this module
// so "80/20" holds at about s ≈ 0.88 for the ideal cache and s ≈ 0.93 for LRU.
// Captured: how the hit ratio grows with the cache size and with the skew of the key
// popularity, and what a cache passes on: its misses are the cold tail of the popularity,
// which is all a second cache behind it ever sees (Popularity::filtered_by). Ignored: temporal
// locality (a key requested now is no likelier to be requested again soon — the independent
// reference model below), TTL expiry, the cold start of a new cache (the estimate is the steady
// state; a cache refilling after a flush is refill_hit_ratio), and eviction
// policies other than LRU. FIFO (and random eviction) has a Che-like closed form too, Martina,
// Garetto & Leonardi, "A unified approach to the performance analysis of caching systems", IEEE
// INFOCOM 2014: a key is cached with probability p·t / (1 + p·t) instead of 1 − e^{−p·t}. Not
// implemented; in the table's setting it gives 0.36 / 0.73 / 0.86 at s = 0.6 / 0.9 / 1.0, two to
// four points below LRU. LFU does better than LRU, up to the ideal row above.
//
// C++20 named module, interface unit (see src/core/core.cppm for the longer explanation).
// This interface imports no other module, and needs no heavy header anyway.
module;
#include <algorithm>
#include <cmath>
// C++20 <concepts>: the standard concept library (std::same_as, ...) used below.
#include <concepts>
#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

export module simsys.caching;

export namespace simsys::caching {

// A run of keys that share one (mean) request probability: `count` keys, each of which a
// request asks for with probability `key_probability` (the probability of ONE key, not of
// the run). Popularity is a list of these, so Σ count · key_probability = 1.
struct Bucket {
  double count;
  double key_probability;
};

// One cache of several that split a key space between them (the replicas of a cache behind
// a consistent-hash ring): the share of the keys it owns — under a random partition also its
// share of the requests — and its characteristic time (che_characteristic_time).
struct CacheShard {
  double key_share;
  double characteristic_time;
};

// Request popularity over a key space, under the *independent reference model* (IRM):
// every request picks key i independently of all earlier requests, with a fixed
// probability p_i. Two shapes:
//  - uniform: p_i = 1/N — every key equally hot (the worst case for a cache);
//  - Zipf:    p_i ∝ i^-s for ranks i = 1..N — a few keys get most of the requests
//    (as in book ch. 8's URL shortener, where a few short links get most of the clicks;
//    our paraphrase, not the book's words). s = 0 is uniform; s ≈ 1 is the classic
//    web-object skew; larger s is hotter.
//
// A Zipf key space can hold 1e9 keys, too many to store one probability each. The ranks
// are therefore grouped into at most 2000 log-spaced buckets of keys with nearly the same
// probability (see zipf_rank_buckets below); the Che sums then cost 2000 terms instead of 1e9.
class Popularity {
 public:
  // Pattern: Static factory (named constructor) — `zipf(s, N)` and `uniform(N)` say which
  // distribution is meant, where a constructor taking (double, double) would not.
  // `space` is the number of keys N; both factories round it down to whole keys.
  // Throws std::invalid_argument for an exponent that is negative, NaN or infinite, and for
  // a space below 1, NaN or infinite: programmer errors, as the scenario loader (and
  // Graph::build, for scenarios written in code) rejects such `keys` before they get here.
  [[nodiscard]] static Popularity zipf(double exponent, double space);
  [[nodiscard]] static Popularity uniform(double space);

  // C++20 std::span<const T>: a non-owning read-only view of contiguous elements. Replaces
  // returning `const std::vector<Bucket>&` (which exposes the container type) or a
  // pointer + length pair.
  [[nodiscard]] std::span<const Bucket> buckets() const& { return buckets_; }
  // The `const&` qualifier above does NOT keep a temporary out: an rvalue binds to `const&`
  // just as it binds to a `const T&` parameter, so `Popularity::zipf(1, 1e3).buckets()` would
  // compile and return a span over buckets destroyed at the end of the statement. This
  // deleted `const&&` overload is the better match for an rvalue, so that call is an error.
  std::span<const Bucket> buckets() const&& = delete;

  // N, the number of keys (Σ count over the buckets).
  [[nodiscard]] double key_count() const { return key_count_; }

  // The popularity of the MISSES of an LRU cache with characteristic time `t` that sees this
  // popularity: what a second cache behind it (its backend, or the target of its `then` step
  // on miss) is asked for. V. Martina, M. Garetto & E. Leonardi, "A unified approach to the
  // performance analysis of caching systems", IEEE INFOCOM 2014, §IV (networks of caches):
  // under the Che approximation key i misses with probability e^{−p_i t} (no request for it
  // within the last t), so the miss stream asks for key i with probability
  //   p'_i = p_i e^{−p_i t} / Σ_j p_j e^{−p_j t}       (renormalised over the misses).
  // The hot keys (p_i t ≫ 1) all but vanish from it: what is left is the cold tail, which is
  // why a second cache behind a first one hits far less than its size suggests. The same keys,
  // the same buckets, different probabilities. Assumes the misses are again IRM (each miss
  // independent of the others); real miss streams are slightly burstier.
  // `t` = +infinity (a cache that holds every key: it never misses) gives the limit of the
  // formula, all the weight on the coldest bucket, so the result is always a distribution.
  [[nodiscard]] Popularity filtered_by(double characteristic_time) const;

  // The same for a key space split between several LRU caches, each owning a share of the keys
  // (the arcs of a consistent-hash ring): key i misses in the one cache that owns it, so
  //   p'_i ∝ p_i Σ_j a_j e^{−p_i t_j}     (a_j the cache's key share, t_j its characteristic
  // time; a random partition puts a share a_j of every popularity level in cache j).
  [[nodiscard]] Popularity filtered_by(std::span<const CacheShard> shards) const;

 private:
  explicit Popularity(std::vector<Bucket> buckets);

  std::vector<Bucket> buckets_;
  double key_count_ = 0.0;
};

// The Che approximation for an LRU cache: H. Che, Y. Tung & Z. Wang, "Hierarchical Web
// caching systems: modeling, design and experimental results", IEEE JSAC 20(7), 2002.
// C. Fricker, P. Robert & J. Roberts, "A versatile and accurate approximation for LRU cache
// performance", ITC 24, 2012, explain why it is so accurate: for Zipf-like popularities it
// becomes exact as the cache size C grows to infinity. (It is the cache size that must be
// large, not the key space: a huge key space with a tiny cache is not covered.)
//
// Idea: time is counted in requests. An LRU cache of C keys keeps a key while it has been
// requested within the last t_c requests — the *characteristic time*, approximated as the
// same for every key. Under IRM key i is asked for by a fraction p_i of the requests, so
// the number of requests for i among t_c is about Poisson with mean p_i t_c, and i is in
// the cache with probability 1 − e^{−p_i t_c} (at least one request for it in t_c).
//   characteristic time t_c solves   Σ_i (1 − e^{−p_i t_c}) = C   (the cache is full)
//   hit ratio                     h = Σ_i p_i (1 − e^{−p_i t_c})   (a request for i hits
//                                                                   iff i is cached)
// Assumptions: IRM (no temporal locality), LRU eviction, a large cache, C counted in keys
// (not bytes), steady state. `capacity >= N` returns 1 (every key fits; in the steady state
// there are no misses), `capacity == 0` returns 0, and a fractional capacity is allowed (the
// equation is continuous in C). Throws std::invalid_argument for a capacity that is
// negative or NaN (a programmer error, as above); an infinite one holds every key.
//
// Cost: a bisection of 80 steps over at most 2000 buckets — at most 160 000 exponentials,
// about half a millisecond (Release), once per cache when the graph is built.
[[nodiscard]] double che_hit_ratio(double capacity, const Popularity& popularity);

// The characteristic time t_c of the same cache: how many requests a key survives in it
// unrequested (see above), the number a cache behind it needs to know which keys this one
// misses (Popularity::filtered_by). +infinity when `capacity >= N` (nothing is ever evicted),
// 0 when `capacity == 0`; the same errors as che_hit_ratio.
[[nodiscard]] double che_characteristic_time(double capacity, const Popularity& popularity);

// D(n) = Σ_i (1 − e^{−p_i n}): the expected number of DISTINCT keys among `requests` independent
// requests drawn from `popularity` (the coupon-collector sum: each draw is a coupon, a key is
// collected once some request asked for it). It is the left-hand side of Che's equation above,
// with the characteristic time read as a count of requests: D(t_c) = C. A cache emptied by a flush
// that has since seen n requests and evicted nothing yet holds exactly D(n) keys (phase 3's
// cache_flush, engine::FaultSchedule). Throws std::invalid_argument for a negative or NaN count.
[[nodiscard]] double keys_requested(const Popularity& popularity, double requests);

// The hit ratio of an LRU cache of `capacity` keys that was emptied and has since seen `requests`
// requests (phase 3 spec §4.3, the warm-up after a cache_flush): the Che hit ratio of a cache of
//   min(capacity, D(requests))                                     entries.
// While D(n) < capacity nothing has been evicted, so the cache holds every key asked for in the
// last n requests: it IS an LRU cache whose characteristic time is n, and Che's hit ratio is read
// at t = n with no equation to solve, h = Σ_i p_i (1 − e^{−p_i n}). Once D(n) reaches the capacity
// the cache is full and evicts as usual: the steady che_hit_ratio(capacity). 0 for 0 requests.
// Assumes what che_hit_ratio assumes (IRM, LRU), and that the cache refills only from the requests
// it is asked (a miss fetches its key and keeps it). Same errors as keys_requested and
// che_hit_ratio.
[[nodiscard]] double refill_hit_ratio(double capacity, const Popularity& popularity,
                                      double requests);

// C++20 concepts and `requires` expressions: a concept is a named compile-time predicate
// on a type. Replaces SFINAE and enable_if tricks (and "read the docs and hope" duck
// typing), so a type that lacks hit_ratio fails with a short "constraint not satisfied"
// error at the use site instead of pages of template instantiation errors.
// HitRatioModel: anything that answers hit_ratio(capacity) -> double, the share of reads a
// cache of `capacity` keys serves. Each model owns whatever else it reads (the Che model its
// key popularity), so a model that needs no popularity is not handed one. (Spec §5 wrote
// hit_ratio(w, keys), a workload and a key count; spec §18 records why it became
// hit_ratio(capacity). See also docs/scenario-schema.md, `che` hit ratios.)
template <class M>
concept HitRatioModel = requires(const M& model, double capacity) {
  { model.hit_ratio(capacity) } -> std::same_as<double>;
};

// Pattern: Strategy/Policy — each cache's hit-ratio model is one of these, chosen when the
// graph is built from the scenario's `hit_ratio: {fixed: 0.8}` or `hit_ratio: che` (spec §5).

// `hit_ratio: {fixed: h}`: the scenario author's own estimate, used as given (phase 2a),
// whatever the capacity: the argument is ignored on purpose. A stated ratio describes the
// cache as the author sized it, and nothing says how it would scale, so a cache that loses a
// replica (phase 3's node_down, which asks hit_ratio again with the smaller capacity) keeps it.
struct FixedHitRatio {
  double value;
  [[nodiscard]] double hit_ratio(double /*capacity*/) const { return value; }
};

// `hit_ratio: che`: computed from the cache size and the key popularity of the clients
// whose requests reach the cache.
struct CheHitRatio {
  Popularity popularity;
  [[nodiscard]] double hit_ratio(double capacity) const {
    return che_hit_ratio(capacity, popularity);
  }
};

}  // namespace simsys::caching

// ---- implementation --------------------------------------------------------------------

namespace simsys::caching {
namespace {

// Number of log-spaced buckets a Zipf key space is grouped into (spec §18 entry 2). Error: measured
// against dense sums (N ≤ 1e5) and against 40 000 buckets (N = 1e9), the relative hit-ratio error
// is at most 2e-6 for s ≤ 3 and N ≤ 1e9 (the dense-sum test pins 1e-5). It grows with the spread of
// p inside a bucket, a factor of up to N^{s/B}, so with N^{1/B}: a larger key space needs more
// buckets for the same accuracy. Cost: building the buckets of a 1e8-key space and solving Che
// takes about half a millisecond.
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
// whose relative error is about s(s+1)/(24 i²). Buckets hold more than one key only from
// rank ~600 on (N = 1e9; later for a smaller N), where that is ~2e-7 at s = 1 and ~1e-6
// at s = 3.
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
                       .key_probability = mean_rank_weight(exponent, edge + 1.0, next_edge)});
    edge = next_edge;
  }
  return buckets;
}

// Divides every weight by the generalised harmonic number H_{N,s} = Σ count·weight, the
// Zipf normalising constant, so the probabilities sum to 1.
void normalise(std::vector<Bucket>& buckets) {
  double total = 0.0;
  for (const auto& bucket : buckets) total += bucket.count * bucket.key_probability;
  for (auto& bucket : buckets) bucket.key_probability /= total;
}

// 1 − e^{−x} for x ≥ 0 via expm1: correct to the last bit even when x is ~1e-12 (a cold key over
// a short characteristic time). 1 − exp(−x) subtracts two numbers next to 1, whose doubles are
// 1.1e-16 apart, so it keeps only the difference's leading digits: at x = 1e-12 a relative error
// of 2.2e-5, and below x ≈ 1e-16 exactly 0.
[[nodiscard]] double one_minus_exp_neg(double x) {
  return -std::expm1(-x);
}

// Σ_i (1 − e^{−p_i t}): the expected number of distinct keys requested within t, i.e. the
// number of keys an LRU cache with characteristic time t holds. Increasing in t.
[[nodiscard]] double keys_in_cache(std::span<const Bucket> buckets, double t) {
  double keys = 0.0;
  for (const auto& bucket : buckets)
    keys += bucket.count * one_minus_exp_neg(bucket.key_probability * t);
  return keys;
}

// Solves keys_in_cache(t) = capacity for the characteristic time t_c, by bisection on
// log t: t_c spans many orders of magnitude (about C for a small cache, up to ~1/p_N for a
// cache almost as large as a skewed space: s = 3 over 1e9 keys has p_N ~ 1e-27), and
// halving the interval in log space takes the same number of steps for every one of them.
// Bracket: t_c ≥ C, because 1 − e^{−x} ≤ x (the curve lies below its tangent at 0), so
//   keys_in_cache(t) = Σ_i (1 − e^{−p_i t}) ≤ Σ_i p_i t = t,
// and a cache whose characteristic time is below C would hold fewer than C keys. Above,
// 1e300 is as long a time as a double expresses. 80 halvings shrink the bracket's at most
// ~1400 natural-log units (C down to 1e-300) below double precision.
[[nodiscard]] double characteristic_time(std::span<const Bucket> buckets, double capacity) {
  double log_low = std::log(capacity);
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
    hits += bucket.count * bucket.key_probability * one_minus_exp_neg(bucket.key_probability * t);
  return hits;
}

// The weight of each bucket's keys in a miss stream, as a logarithm: log Σ_j a_j p e^{−p t_j}
// (Popularity::filtered_by), unnormalised. In log space because p·t can reach hundreds or
// more for the hot keys of a big cache: e^{−p t} would underflow to 0 for every bucket, and
// 0/0 is no distribution. For each bucket the largest term is factored out (log-sum-exp):
//   log Σ_j a_j e^{−p t_j} = m + log Σ_j a_j e^{−p t_j − m},  m = max_j (−p t_j over a_j > 0).
// t_j = +infinity (a shard that never misses) contributes nothing; if every shard is one,
// the limit of the formula as t → ∞ is taken: the weight −p·(the largest double), next to
// which log p no longer counts, keeps only the least popular bucket after normalising, which
// is where the last misses would be.
[[nodiscard]] std::vector<double> log_miss_weights(std::span<const Bucket> buckets,
                                                   std::span<const CacheShard> shards) {
  // C++20 ranges algorithms (std::ranges::all_of here, std::ranges::max in filtered_by) take
  // the range itself instead of a begin/end iterator pair, so the two ends cannot come from
  // different containers; ranges::max also returns the largest VALUE of a whole range, where
  // std::max compares two arguments and std::max_element returns an iterator.
  const bool never_misses = std::ranges::all_of(shards, [](const CacheShard& shard) {
    return shard.key_share <= 0.0 || std::isinf(shard.characteristic_time);
  });
  std::vector<double> weights;
  weights.reserve(buckets.size());
  for (const auto& bucket : buckets) {
    const double p = bucket.key_probability;
    if (p <= 0.0) {  // a key nobody asks for is never missed either
      weights.push_back(-std::numeric_limits<double>::infinity());
      continue;
    }
    if (never_misses) {  // the limit: only the coldest keys keep any weight
      weights.push_back(-p * std::numeric_limits<double>::max());
      continue;
    }
    double exponent_max = -std::numeric_limits<double>::infinity();
    for (const auto& shard : shards) {
      if (shard.key_share <= 0.0 || std::isinf(shard.characteristic_time)) continue;
      exponent_max = std::max(exponent_max, -p * shard.characteristic_time);
    }
    double sum = 0.0;
    for (const auto& shard : shards) {
      if (shard.key_share <= 0.0 || std::isinf(shard.characteristic_time)) continue;
      sum += shard.key_share * std::exp(-p * shard.characteristic_time - exponent_max);
    }
    weights.push_back(std::log(p) + exponent_max + std::log(sum));
  }
  return weights;
}

// N, the number of whole keys in a key space of `space`: rounded down, after rejecting a
// space that is NaN, infinite or below one key. This check and the others below are written
// `!(x >= bound)` rather than `x < bound`: every comparison with a NaN is false, so
// `NaN < 1` would let a NaN through, while `!(NaN >= 1)` rejects it.
[[nodiscard]] double whole_keys(double space) {
  if (!(space >= 1.0) || !std::isfinite(space)) {
    throw std::invalid_argument{"key space must be a finite count of at least one key"};
  }
  return std::floor(space);
}

}  // namespace

Popularity::Popularity(std::vector<Bucket> buckets) : buckets_{std::move(buckets)} {
  for (const auto& bucket : buckets_) key_count_ += bucket.count;
}

Popularity Popularity::zipf(double exponent, double space) {
  if (!(exponent >= 0.0) || !std::isfinite(exponent)) {
    throw std::invalid_argument{"zipf exponent must be finite and >= 0"};
  }
  auto buckets = zipf_rank_buckets(exponent, whole_keys(space));
  normalise(buckets);
  return Popularity{std::move(buckets)};
}

Popularity Popularity::uniform(double space) {
  const double keys = whole_keys(space);
  return Popularity{{{.count = keys, .key_probability = 1.0 / keys}}};
}

double che_characteristic_time(double capacity, const Popularity& popularity) {
  if (!(capacity >= 0.0)) throw std::invalid_argument{"cache capacity must be >= 0"};
  // Every key fits: nothing is ever evicted, a key stays however long it goes unrequested.
  if (capacity >= popularity.key_count()) return std::numeric_limits<double>::infinity();
  if (capacity == 0.0) return 0.0;
  return characteristic_time(popularity.buckets(), capacity);
}

double che_hit_ratio(double capacity, const Popularity& popularity) {
  const double t = che_characteristic_time(capacity, popularity);
  if (std::isinf(t)) return 1.0;  // every key fits: no steady-state miss
  if (t == 0.0) return 0.0;
  return hit_ratio_at(popularity.buckets(), t);
}

double keys_requested(const Popularity& popularity, double requests) {
  if (!(requests >= 0.0)) throw std::invalid_argument{"a count of requests must be >= 0"};
  return keys_in_cache(popularity.buckets(), requests);
}

double refill_hit_ratio(double capacity, const Popularity& popularity, double requests) {
  if (keys_requested(popularity, requests) >= capacity) return che_hit_ratio(capacity, popularity);
  return hit_ratio_at(popularity.buckets(), requests);  // t_c of a cache of D(n) keys is n itself
}

Popularity Popularity::filtered_by(double characteristic_time) const {
  const CacheShard whole{.key_share = 1.0, .characteristic_time = characteristic_time};
  return filtered_by(std::span{&whole, 1});
}

Popularity Popularity::filtered_by(std::span<const CacheShard> shards) const {
  std::vector<Bucket> misses = buckets_;
  const auto weights = log_miss_weights(buckets_, shards);
  // Divide by the largest weight before leaving log space: e^{log w − max} is at most 1 and at
  // least one bucket gets exactly 1, so neither overflow nor an all-zero underflow can happen.
  const double largest = std::ranges::max(weights);
  for (std::size_t i = 0; i < misses.size(); ++i) {
    misses[i].key_probability = std::exp(weights[i] - largest);
  }
  normalise(misses);
  return Popularity{std::move(misses)};
}

}  // namespace simsys::caching
