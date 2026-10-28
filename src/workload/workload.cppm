// Module `simsys.workload`: the rate curve a client follows (spec §8) — a base rate
// shaped by multiplicative terms (wave, ramp, spikes), or replaced by scripted points.
//
// Models: the traffic estimate System Design Interview ch. 2 (back-of-the-envelope
// estimation) starts from — "average QPS, and peak QPS = 2 x average" — turned into a
// function of time, so the peak is a *moment* a design must survive rather than a
// constant. Captured: daily cycles (wave), launches and migrations (ramp), flash crowds
// and retry storms (spikes), and replayed production traces (points). Ignored: anything
// random — noise is a separate, seeded process (NoiseProcess below) the client multiplies
// in — and any feedback from the system (a slow site does not lower this curve; real users
// would give up, which the model does not capture).
//
// Composition (spec §8, phase 2a refinement 2):
//
//     rate(t) = level(t) x wave(t) x ramp(t) x PRODUCT over active spikes of factor
//     level(t) = points ? interpolate(points, t) : base
//
// Every term is a dimensionless multiplier around 1, so terms combine freely and each
// can be read (and printed by `simsys curve`) on its own. Times are offsets from t = 0,
// which is midnight (phase 2a refinement 1), so `peak_at: 20:00` is 20 h after start.
//
// C++20 named module, interface unit (see src/core/core.cppm for the longer
// explanation). CLAUDE.md §3: this interface imports simsys.core, so no <chrono> in the
// global module fragment. std::chrono's non-member operators (t - peak_at, t < at, ...)
// are therefore not usable here; the code computes on the integer nanosecond `.count()`
// (exact) or on `seconds(t)` doubles (inside formulas only).
module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <generator>
#include <iterator>
#include <numbers>
#include <optional>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

export module simsys.workload;

import simsys.core;

export namespace simsys::workload {

// The three wave shapes. `sine` is the smooth textbook cycle, `square` an on/off
// schedule (office hours, a batch window), `diurnal` a chosen consumer-web day.
enum class WaveShape { sine, square, diurnal };

// A periodic multiplier. `amplitude` is the relative swing: 0.5 means the peak is 1.5 x
// and the trough 0.5 x the level. It must lie in [0, 1], so no multiplier, and therefore
// no rate, is ever negative: the config loader rejects a scenario outside that range, and
// wave_multiplier throws for one (a programmer error).
//
// Every member of the structs below has a default member initializer, even `{}`, so
// callers can use C++20 designated initializers (`Wave{.amplitude = 0.3}`) without
// tripping -Wmissing-field-initializers (an error under -Werror; see model.cppm).
struct Wave {
  WaveShape shape{WaveShape::sine};
  SimTime period{24_h};   // one day: the cycle every user-facing service shows
  double amplitude{0.5};  // chosen: peak 1.5 x, trough 0.5 x the mean; not from the book
  SimTime peak_at{};      // where the wave is highest; a clock time, e.g. 20:00
};

// A linear change of the rate from `from` to `to` over the first `over` of the run,
// then `to` forever: a launch, a gradual traffic migration, a load-test ramp-up.
struct Ramp {
  Rate from{};
  Rate to{};
  SimTime over{};
};

// A scripted burst: the rate is multiplied by `factor` during [at, at + duration).
// Models a flash crowd (a celebrity post, ch. 11), a marketing push, or a retry storm.
struct Spike {
  SimTime at{};
  SimTime duration{};
  double factor{1.0};
};

// A recorded or hand-drawn rate trace: (time, rate) pairs, non-empty and with
// non-decreasing times (the config loader rejects an empty or unsorted trace). Two points
// at the same time make a step: the later one takes effect from that instant. Replaces
// `base`; the wave and spikes still multiply it (a ramp cannot: see RateCurve::rate_at).
struct Points {
  std::vector<std::pair<SimTime, Rate>> at{};
};

// Spec §8: base x wave x ramp x spikes (x noise, applied by the client); points replace base.
struct RateCurve {
  Rate base{};
  std::optional<Points> points{};
  std::optional<Wave> wave{};
  std::optional<Ramp> ramp{};
  std::vector<Spike> spikes{};

  // The deterministic rate at time `t`: everything except noise.
  // Throws std::invalid_argument for a wave with a non-positive period or an amplitude
  // outside [0, 1], for empty points, and for a ramp with base 0 or with points (it has
  // nothing to be relative to). All are programmer errors: the config loader rejects
  // such a scenario with a message naming the offending key.
  [[nodiscard]] Rate rate_at(SimTime t) const;
};

// The wave term alone, exposed so `simsys curve` can print a shape on its own.
// Throws std::invalid_argument if `w.period` is not positive or `w.amplitude` is outside
// [0, 1] (programmer errors: the config loader rejects such a scenario).
[[nodiscard]] double wave_multiplier(const Wave& w, SimTime t);

// Spec §9.2 workload noise: a multiplier 1 + x on the rate, where x is an Ornstein-Uhlenbeck
// process — Gaussian noise that reverts to zero over `correlation` (tau) and has stationary
// standard deviation `sigma`. Exact discretisation for a step dt:
//   x_{k+1} = x_k e^{-dt/tau} + sigma sqrt(1 - e^{-2 dt/tau}) xi,   xi ~ N(0, 1)
// Models bursts that last about tau (a trending topic, a push notification wave) rather than
// per-request randomness, which the fluid model does not represent (spec §9, last paragraph).
struct Noise {
  double sigma{};         // stationary standard deviation of x; 0.1 means "typically within 10 %"
  SimTime correlation{};  // tau: how long a burst lasts, roughly; must be positive
};

// The seeded noise state of ONE client: call next(dt) once per simulation step.
//
// Determinism contract: the same Noise and the same seed give the same path, bit for bit.
// Use one process per client (they must not share an engine, or adding a client would
// change every other client's noise); the simulation engine is to derive each client's
// seed from the scenario's one `seed` (spec §9.2) and the client's index, so runs are
// reproducible and clients are independent.
class NoiseProcess {
 public:
  // Throws std::invalid_argument for sigma < 0 or correlation <= 0 (programmer errors: the
  // loader rejects such a scenario).
  NoiseProcess(Noise spec, std::uint64_t seed);

  // Advances the process by `dt` and returns the multiplier max(0, 1 + x). The floor keeps
  // the rate non-negative when sigma is large. Throws std::invalid_argument if `dt` is not
  // positive. With sigma == 0 returns exactly 1.0 without touching the random engine.
  [[nodiscard]] double next(SimTime dt);

 private:
  Noise spec_;
  double x_{};  // the Ornstein-Uhlenbeck state; starts at 0, the long-run mean
  // The engine and the distribution travel together on purpose: libstdc++'s
  // normal_distribution (Box-Muller/polar method) produces values in pairs and caches the
  // second one, so the distribution carries hidden state that belongs to this path.
  std::mt19937_64 engine_;
  std::normal_distribution<double> normal_{0.0, 1.0};  // xi ~ N(0, 1)
};

// One point of a sampled curve: the time (offset from t = 0) and the rate at that time.
struct Sample {
  SimTime t{};
  Rate rate{};
};

// C++20 coroutines + C++23 std::generator<T>: a function that `co_yield`s values becomes a
// lazy range. It replaces building a whole std::vector up front or hand-writing an iterator
// class; the curve is sampled only as far as the consumer reads (a day at 1 s steps is
// 86400 samples nobody has to store). Each co_yield suspends the coroutine and hands one
// Sample to the caller; resuming continues the loop. The body does not run until the first
// begin(), which is why a bad `step` throws there (a programmer error; the CLI validates
// its --step first). No design pattern: just a lazy range.
//
// `curve` is taken BY VALUE on purpose. A coroutine's parameters are copied into its heap
// frame, and a reference parameter is copied as a *reference*: the generator outlives the
// call expression (it is returned, then iterated later), so a `const RateCurve&` bound to
// a caller's temporary would dangle by the time the body first runs. A by-value parameter
// is moved or copied into the frame and lives as long as the generator does.
//
// Samples are taken at t = 0, step, 2 step, ... while t < until (end exclusive). Throws
// std::invalid_argument if `step` is not positive.
[[nodiscard]] std::generator<Sample> samples(RateCurve curve, SimTime step, SimTime until);

// The same stream with workload noise (spec §9.2): the generator owns a NoiseProcess seeded
// with `seed` and multiplies rate_at(t) by noise.next(step) for each sample, so the noise
// advances exactly once per sample and the same arguments give the same stream, bit for bit.
// Throws std::invalid_argument for a non-positive `step`, or the NoiseProcess's own errors
// (negative sigma, non-positive correlation), on the first begin().
[[nodiscard]] std::generator<Sample> samples(RateCurve curve, SimTime step, SimTime until,
                                             Noise noise, std::uint64_t seed);

}  // namespace simsys::workload

// Not exported: implementation details of the terms.
namespace simsys::workload::detail {

// A "textbook consumer-web day", one value per hour from 00:00 to 23:00: night trough
// 0.40 at 04:00, a morning rise as people wake and commute, a midday plateau, a small
// afternoon lull, and the evening peak 1.60 at 20:00 when people are home on their phones,
// then a decline toward midnight. CHOSEN, not measured: it has the shape public traffic
// graphs of consumer sites show, with peak/trough = 4. Its peak is 1.6 x the daily mean,
// gentler than the book's "peak QPS = 2 x average" rule of thumb (ch. 2); for the book's
// 2 x use `sine` with amplitude 1.0. It is a teaching default, not data.
//
// Pattern: Table-driven method — the shape is data (24 numbers), read by one
// interpolation function, so a different day is a different table, not different code.
// The numbers are normalised by hand to mean 1 (they sum to 24), so the diurnal wave
// shapes *when* traffic comes without changing the daily total; the static_assert below
// proves it at compile time.
inline constexpr std::array<double, 24> diurnal_day{
    0.75, 0.60, 0.50, 0.45, 0.40, 0.45, 0.60, 0.80,   // 00:00 - 07:00: night, waking up
    0.95, 1.05, 1.05, 1.15, 1.15, 1.15, 1.05, 1.05,   // 08:00 - 15:00: work day, lunch
    1.15, 1.30, 1.45, 1.55, 1.60, 1.50, 1.30, 1.00};  // 16:00 - 23:00: evening peak

// C++20 consteval: an immediate function, evaluated only at compile time (a call that
// cannot be is a compile error). Unlike a constexpr function it can never silently run
// at run time; here it exists only to feed the static_assert.
consteval double mean_of(const std::array<double, 24>& table) {
  double sum = 0;
  for (double v : table) sum += v;
  return sum / static_cast<double>(table.size());
}
static_assert(mean_of(diurnal_day) - 1.0 < 1e-12 && 1.0 - mean_of(diurnal_day) < 1e-12,
              "the diurnal table must average exactly 1 over the day");

// The table's peak hour (20 for the table above). The wave places this hour at the
// user's `peak_at`, so `peak_at: 20:00` reproduces the table and `peak_at: 13:00` shifts
// the whole day by -7 h (a different time zone, a lunchtime-heavy service).
// C++20 ranges algorithms (namespace std::ranges): each takes the whole container instead
// of a begin/end iterator pair, and accepts a projection (see interpolate below). What
// C++20 adds here is that overload; like its C++17 counterpart std::max_element it is
// constexpr, so the peak hour is derived from the table at compile time instead of being
// a second, hand-kept constant that could drift from it.
inline constexpr auto diurnal_peak_hour =
    static_cast<double>(std::ranges::max_element(diurnal_day) - diurnal_day.begin());

// `x` wrapped into [0, period). C++ `%` keeps the dividend's sign, so a negative x gives
// a remainder in (-period, 0], and adding period lands it in [0, period). Neither step can
// overflow: |x % period| < period.
[[nodiscard]] std::int64_t wrap(std::int64_t x, std::int64_t period) {
  const std::int64_t remainder = x % period;
  return remainder < 0 ? remainder + period : remainder;
}

// Phase of `t` within the wave: wrap(t - peak_at, P), in [0, P) nanoseconds, where 0 is
// the peak. Done on integer nanoseconds, because `%` is exact (fmod on seconds would
// round). Both times are wrapped BEFORE subtracting: t - peak_at itself could overflow
// int64 for extreme values, while the difference of two wrapped values lies in (-P, P),
// and one more wrap gives the same phase.
[[nodiscard]] std::int64_t phase_ns(const Wave& w, SimTime t) {
  const std::int64_t period = w.period.count();
  if (period <= 0) throw std::invalid_argument{"wave period must be positive"};
  return wrap(wrap(t.count(), period) - wrap(w.peak_at.count(), period), period);
}

// Sine: 1 + A cos(2 pi phi / P). A cosine so the peak sits at phi = 0 (t = peak_at);
// the mean over one period is exactly 1, so the wave moves load around without adding any.
[[nodiscard]] double sine(const Wave& w, std::int64_t phase) {
  const double cycles = static_cast<double>(phase) / static_cast<double>(w.period.count());
  // C++20 std::numbers::pi: the constant as a typed `inline constexpr double`, replacing
  // the non-standard M_PI macro (POSIX, not ISO C++) or a hand-typed literal.
  return 1.0 + w.amplitude * std::cos(2.0 * std::numbers::pi * cycles);
}

// Square: 1 + A for the half period centred on the peak, 1 - A for the other half.
// "Centred" means the distance to the nearest peak, d = min(phi, P - phi), is at most
// P/4; both edges belong to the high half. Compared as d <= P / 4 in INTEGER division:
// for a whole number d, d <= P/4 holds exactly when d <= floor(P/4), so the truncation
// loses nothing (P = 6: d = 1 is high, d = 2 is low, as 2 > 1.5). This is exact, so no
// rounding can move an edge, and unlike 4 d <= P it cannot overflow. Mean over a period: 1.
[[nodiscard]] double square(const Wave& w, std::int64_t phase) {
  const std::int64_t period = w.period.count();
  const std::int64_t distance_to_peak = std::min(phase, period - phase);
  return distance_to_peak <= period / 4 ? 1.0 + w.amplitude : 1.0 - w.amplitude;
}

// Diurnal: 1 + A (table(h) - 1), where h is the table hour the phase maps to (the peak
// hour plus phi scaled to 24 table hours, wrapped) and table(h) linearly interpolates
// between the two neighbouring hours. A = 1 gives the table itself, A = 0 a flat line.
// Linear interpolation of a table with mean 1 also has mean 1 over the period (each
// hour-long segment averages its two endpoints, and every entry is an endpoint twice).
[[nodiscard]] double diurnal(const Wave& w, std::int64_t phase) {
  constexpr auto hours = diurnal_day.size();
  const double cycles = static_cast<double>(phase) / static_cast<double>(w.period.count());
  double hour = diurnal_peak_hour + cycles * static_cast<double>(hours);
  if (hour >= static_cast<double>(hours)) hour -= static_cast<double>(hours);
  const auto lower = static_cast<std::size_t>(hour) % hours;
  const auto upper = (lower + 1) % hours;
  const double fraction = hour - std::floor(hour);
  const double table = diurnal_day[lower] + (diurnal_day[upper] - diurnal_day[lower]) * fraction;
  return 1.0 + w.amplitude * (table - 1.0);
}

// How far along the ramp is at `t`: 0 up to t = 0, t / over during the ramp, 1 from
// t = over on. Decided on integer nanoseconds first, so the ends are exact and a zero
// (or negative) `over` is a clean step at t = 0: `from` before it, `to` from it on.
[[nodiscard]] double ramp_fraction(SimTime t, SimTime over) {
  if (t.count() < 0) return 0.0;
  if (t.count() >= over.count()) return 1.0;
  return seconds(t) / seconds(over);  // 0 <= t < over here, so over > 0
}

// Ramp: (from + (to - from) x fraction(t)) / base — the ramp's rate as a multiplier of
// `base`, so the spec's "ramp from 0 to 2000/s" with base 1000/s doubles the base at the
// end. A ramp needs a base to be relative to, so base 0 throws std::invalid_argument (a
// programmer error: the config loader rejects a ramp without rate.base).
[[nodiscard]] double ramp_multiplier(const Ramp& r, Rate base, SimTime t) {
  if (base.per_second == 0.0) throw std::invalid_argument{"a ramp needs a nonzero base rate"};
  const double fraction = ramp_fraction(t, r.over);
  const double level = r.from.per_second + (r.to.per_second - r.from.per_second) * fraction;
  return level / base.per_second;
}

// Is `s` active at `t`, i.e. at <= t < at + duration? End exclusive, so back-to-back
// spikes never overlap. Written as t - at < duration rather than t < at + duration: the
// sum overflows int64 for a huge duration (the spike would wrongly switch off), while the
// difference cannot: at <= t is checked first, and `at` is never negative (the config
// loader reads it with parse_time_point, which yields offsets from t = 0), so the
// difference lies in [0, t].
[[nodiscard]] bool active(const Spike& s, SimTime t) {
  return s.at.count() <= t.count() && t.count() - s.at.count() < s.duration.count();
}

// Spikes: the product of `factor` over every spike active at t. Overlapping spikes
// compound: a 5 x flash crowd plus a 2 x retry storm is 10 x.
[[nodiscard]] double spikes_multiplier(const std::vector<Spike>& spikes, SimTime t) {
  double product = 1.0;
  for (const auto& s : spikes) {
    if (active(s, t)) product *= s.factor;
  }
  return product;
}

// Points: piecewise-linear interpolation, held flat outside the trace —
// t < first time: first rate; t >= last time: last rate; otherwise
// r0 + (r1 - r0) (t - t0) / (t1 - t0) between the neighbours t0 <= t < t1, where t0 is
// the LAST point at or before t. Precondition: times non-decreasing (unsorted input is a
// config loader error). Equal times are allowed and make a step: the search for "the
// first point strictly after t" skips every point at t, so the later of two equal-time
// points wins from that instant on, and t1 > t0 always holds (no division by zero).
// Throws std::invalid_argument on an empty trace (a programmer error: the loader rejects it).
[[nodiscard]] double interpolate(const Points& points, SimTime t) {
  const auto& at = points.at;
  if (at.empty()) throw std::invalid_argument{"points must not be empty"};
  if (t.count() < at.front().first.count()) return at.front().second.per_second;
  if (t.count() >= at.back().first.count()) return at.back().second.per_second;
  // A projection says *which part* of each element to compare, here the time in
  // nanoseconds — replacing a hand-written comparator lambda over pairs.
  // std::ranges::upper_bound returns the first point strictly after t; since
  // t >= the first time, it is not the first point, and the one before it is <= t.
  const auto next = std::ranges::upper_bound(at, t.count(), {},
                                             [](const auto& point) { return point.first.count(); });
  const auto& [t1, r1] = *next;
  const auto& [t0, r0] = *std::prev(next);
  const double fraction =
      static_cast<double>(t.count() - t0.count()) / static_cast<double>(t1.count() - t0.count());
  return r0.per_second + (r1.per_second - r0.per_second) * fraction;
}

}  // namespace simsys::workload::detail

namespace simsys::workload {

double wave_multiplier(const Wave& w, SimTime t) {
  // Written as !(in range) so a NaN amplitude is rejected too (every comparison with NaN
  // is false).
  if (!(w.amplitude >= 0.0 && w.amplitude <= 1.0)) {
    throw std::invalid_argument{"wave amplitude must be in [0, 1]"};
  }
  const auto phase = detail::phase_ns(w, t);
  // C++20 `using enum`: brings the enumerators into this scope so the cases read `sine`
  // instead of `WaveShape::sine`, without making the enum unscoped everywhere.
  using enum WaveShape;
  switch (w.shape) {
    case sine:
      return detail::sine(w, phase);
    case square:
      return detail::square(w, phase);
    case diurnal:
      return detail::diurnal(w, phase);
  }
  // No `default:` on purpose: -Wswitch (an error under -Werror) then flags an enumerator
  // added without a case. That guards the CODE, not the VALUE: a WaveShape can still hold
  // a number that names no enumerator (static_cast<WaveShape>(7)), and control reaches
  // here. That is a programmer error, so it throws rather than returning a made-up
  // multiplier (or invoking std::unreachable, which would make it undefined behaviour).
  throw std::logic_error{"unknown wave shape"};
}

// C++11 <random>, explained here because this is the module's first use. std::mt19937_64
// is the Mersenne Twister engine: the standard fixes its algorithm, so a given seed yields
// the same sequence on every platform and compiler (unlike std::rand or
// std::default_random_engine, whose algorithm is implementation-defined). That is what
// makes "same scenario + same seed = byte-identical run" possible.
NoiseProcess::NoiseProcess(Noise spec, std::uint64_t seed) : spec_{spec}, engine_{seed} {
  if (spec.sigma < 0.0) throw std::invalid_argument{"noise sigma must not be negative"};
  if (spec.correlation.count() <= 0) {
    throw std::invalid_argument{"noise correlation time must be positive"};
  }
}

double NoiseProcess::next(SimTime dt) {
  if (dt.count() <= 0) throw std::invalid_argument{"noise step dt must be positive"};
  // sigma == 0: no noise. Return before drawing so the engine stays untouched and a
  // sigma-0 client is byte-identical to a client without noise.
  if (spec_.sigma == 0.0) return 1.0;
  // Exact Ornstein-Uhlenbeck step (valid for any dt, not just small ones):
  //   decay = e^{-dt/tau}   the fraction of the old deviation x that survives the step
  //   x'    = x decay + sigma sqrt(1 - decay^2) xi
  // The second term is the fresh randomness; sqrt(1 - decay^2) scales it so the variance
  // stays at sigma^2 whatever dt is. Assumes Gaussian, stationary noise, with a single
  // correlation time.
  const double decay = std::exp(-seconds(dt) / seconds(spec_.correlation));
  x_ = x_ * decay + spec_.sigma * std::sqrt(1.0 - decay * decay) * normal_(engine_);
  return std::max(0.0, 1.0 + x_);
}

Rate RateCurve::rate_at(SimTime t) const {
  // A ramp is a multiplier relative to `base`; points replace the base, so the
  // combination has no meaning (the config loader says "rate.ramp needs rate.base").
  if (ramp && points) throw std::invalid_argument{"a ramp needs a base rate, not points"};
  const double level = points ? detail::interpolate(*points, t) : base.per_second;
  const double wave_term = wave ? wave_multiplier(*wave, t) : 1.0;
  const double ramp_term = ramp ? detail::ramp_multiplier(*ramp, base, t) : 1.0;
  return Rate{level * wave_term * ramp_term * detail::spikes_multiplier(spikes, t)};
}

std::generator<Sample> samples(RateCurve curve, SimTime step, SimTime until) {
  if (step.count() <= 0) throw std::invalid_argument{"sample step must be positive"};
  // Loop on integer nanoseconds (exact; no floating-point drift accumulates in t).
  for (std::int64_t t = 0; t < until.count(); t += step.count()) {
    const SimTime at{t};
    co_yield Sample{.t = at, .rate = curve.rate_at(at)};
  }
}

std::generator<Sample> samples(RateCurve curve, SimTime step, SimTime until, Noise noise,
                               std::uint64_t seed) {
  if (step.count() <= 0) throw std::invalid_argument{"sample step must be positive"};
  // The process lives in the coroutine frame, so its state survives every suspension.
  NoiseProcess process{noise, seed};
  for (std::int64_t t = 0; t < until.count(); t += step.count()) {
    const SimTime at{t};
    co_yield Sample{.t = at, .rate = Rate{curve.rate_at(at).per_second * process.next(step)}};
  }
}

}  // namespace simsys::workload
