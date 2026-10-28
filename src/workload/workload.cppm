// Module `simsys.workload`: the rate curve a client follows (spec §8) — a base rate
// shaped by multiplicative terms (wave, ramp, spikes), or replaced by scripted points.
//
// Models: the traffic estimate System Design Interview ch. 2 (back-of-the-envelope
// estimation) starts from — "average QPS, and peak QPS = 2 x average" — turned into a
// function of time, so the peak is a *moment* a design must survive rather than a
// constant. Captured: daily cycles (wave), launches and migrations (ramp), flash crowds
// and retry storms (spikes), and replayed production traces (points). Ignored: anything
// random — noise is a separate, seeded process (Task 3) the client multiplies in — and
// any feedback from the system (a slow site does not lower this curve; real users would
// give up, which the model does not capture).
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
#include <iterator>
#include <numbers>
#include <optional>
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
// and the trough 0.5 x the level. The loader keeps it in [0, 1] (Task 5), so no
// multiplier, and therefore no rate, is ever negative.
//
// Every member of the structs below has a default member initializer, even `{}`, so
// callers can use C++20 designated initializers (`Wave{.amplitude = 0.3}`) without
// tripping -Wmissing-field-initializers (an error under -Werror; see model.cppm).
struct Wave {
  WaveShape shape{WaveShape::sine};
  SimTime period{24_h};  // one day: the cycle every user-facing service shows
  double amplitude{0.5};
  SimTime peak_at{};  // where the wave is highest; a clock time, e.g. 20:00
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

// A recorded or hand-drawn rate trace: (time, rate) pairs, sorted by time and non-empty
// (the loader guarantees both). Replaces `base`; the other terms still multiply it.
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
  [[nodiscard]] Rate rate_at(SimTime t) const;
};

// The wave term alone, exposed so `simsys curve` can print a shape on its own.
// Throws std::invalid_argument if `w.period` is not positive (a programmer error: the
// loader rejects such a scenario).
[[nodiscard]] double wave_multiplier(const Wave& w, SimTime t);

}  // namespace simsys::workload

// Not exported: implementation details of the terms.
namespace simsys::workload::detail {

// A "textbook consumer-web day", one value per hour from 00:00 to 23:00: night trough
// 0.40 at 04:00, a morning rise as people wake and commute, a midday plateau, a small
// afternoon lull, and the evening peak 1.60 at 20:00 when people are home on their phones,
// then a decline toward midnight. CHOSEN, not measured: it has the shape the book's
// "peak QPS = 2 x average" rule of thumb (ch. 2) assumes and that public traffic graphs
// of consumer sites show, with peak/trough = 4. It is a teaching default, not data.
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
// at run time; here it exists only to feed the static_assert. C++20 also made
// std::array's accessors and range-for over it usable in constant expressions this way.
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
// C++20 constexpr algorithms: std::ranges::max_element runs at compile time, so the peak
// hour is derived from the table instead of being a second, hand-kept constant that
// could drift from it.
inline constexpr auto diurnal_peak_hour =
    static_cast<double>(std::ranges::max_element(diurnal_day) - diurnal_day.begin());

// Phase of `t` within the wave, as a fraction of the period: wrap(t - peak_at, P) / P in
// [0, 1), where 0 is the peak. Done on integer nanoseconds: `%` is exact (fmod on
// seconds would round), and C++ `%` keeps the dividend's sign, so a time before the peak
// (negative difference) gets P added to land in [0, P).
[[nodiscard]] std::int64_t phase_ns(const Wave& w, SimTime t) {
  const std::int64_t period = w.period.count();
  if (period <= 0) throw std::invalid_argument{"wave period must be positive"};
  std::int64_t phase = (t.count() - w.peak_at.count()) % period;
  if (phase < 0) phase += period;
  return phase;
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
// "Centred" means the distance to the nearest peak, min(phi, P - phi), is at most P/4;
// both edges belong to the high half. Compared as 4 d <= P on integers: exact, no
// rounding can move an edge. Mean over a period: 1.
[[nodiscard]] double square(const Wave& w, std::int64_t phase) {
  const std::int64_t period = w.period.count();
  const std::int64_t distance_to_peak = std::min(phase, period - phase);
  return 4 * distance_to_peak <= period ? 1.0 + w.amplitude : 1.0 - w.amplitude;
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

// Ramp: (from + (to - from) x min(t / over, 1)) / base — the ramp's rate as a multiplier
// of `base`, so the spec's "ramp from 0 to 2000/s" with base 1000/s doubles the base at
// the end. Before t = 0 the fraction is clamped to 0; `over == 0` jumps straight to `to`.
// A ramp needs a base to be relative to: with base 0 (typically because `points` set the
// level) the term is 1. The loader rejects a ramp without a base (Task 5).
[[nodiscard]] double ramp_multiplier(const Ramp& r, Rate base, SimTime t) {
  if (base.per_second == 0.0) return 1.0;
  const double over = seconds(r.over);
  const double fraction = over <= 0.0 ? 1.0 : std::clamp(seconds(t) / over, 0.0, 1.0);
  const double level = r.from.per_second + (r.to.per_second - r.from.per_second) * fraction;
  return level / base.per_second;
}

// Spikes: the product of `factor` over every spike active at t, i.e. at <= t < at +
// duration (end exclusive, so back-to-back spikes never overlap). Overlapping spikes
// compound: a 5 x flash crowd plus a 2 x retry storm is 10 x.
[[nodiscard]] double spikes_multiplier(const std::vector<Spike>& spikes, SimTime t) {
  double product = 1.0;
  for (const auto& s : spikes) {
    const bool active = s.at.count() <= t.count() && t.count() < s.at.count() + s.duration.count();
    if (active) product *= s.factor;
  }
  return product;
}

// Points: piecewise-linear interpolation, held flat outside the trace —
// t <= first time: first rate; t >= last time: last rate; otherwise
// r0 + (r1 - r0) (t - t0) / (t1 - t0) between the neighbours t0 <= t < t1.
// Throws std::invalid_argument on an empty trace (a programmer error: the loader rejects it).
[[nodiscard]] double interpolate(const Points& points, SimTime t) {
  const auto& at = points.at;
  if (at.empty()) throw std::invalid_argument{"points must not be empty"};
  if (t.count() <= at.front().first.count()) return at.front().second.per_second;
  if (t.count() >= at.back().first.count()) return at.back().second.per_second;
  // C++20 ranges algorithm with a projection: std::ranges::upper_bound takes the whole
  // vector (no begin/end pair) and a projection that says *which part* of each element
  // to compare, here the time in nanoseconds — replacing a hand-written comparator lambda
  // over pairs. It returns the first point strictly after t; the one before it is <= t.
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
  // C++23 std::unreachable(): tells the compiler (and the reader) that control cannot get
  // here — the switch covers every enumerator — replacing a dummy `return 1.0;` that would
  // hide a missing case. Reaching it would be undefined behaviour, so -Wswitch (which warns
  // when an enumerator is added without a case) is the guard.
  std::unreachable();
}

Rate RateCurve::rate_at(SimTime t) const {
  const double level = points ? detail::interpolate(*points, t) : base.per_second;
  const double wave_term = wave ? wave_multiplier(*wave, t) : 1.0;
  const double ramp_term = ramp ? detail::ramp_multiplier(*ramp, base, t) : 1.0;
  return Rate{level * wave_term * ramp_term * detail::spikes_multiplier(spikes, t)};
}

}  // namespace simsys::workload
