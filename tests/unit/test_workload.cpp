// The rate curve a client follows: a base rate shaped by multiplicative terms (spec §8).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.core;
import simsys.workload;

using namespace std::chrono_literals;
// Catch2's Approx never tightens below ~1e-5 relative; closed forms are pinned with matchers.
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using simsys::Rate;
using simsys::workload::Points;
using simsys::workload::Ramp;
using simsys::workload::RateCurve;
using simsys::workload::Spike;
using simsys::workload::Wave;
using simsys::workload::wave_multiplier;
using simsys::workload::WaveShape;

TEST_CASE("a bare base rate is constant", "[workload]") {
  // C++20 designated initializers: name the field you set; the rest keep their defaults.
  const RateCurve c{.base = Rate{100}};
  REQUIRE(c.rate_at(0s).per_second == 100);
  REQUIRE(c.rate_at(23h).per_second == 100);
}

TEST_CASE("a sine wave peaks at peak_at and troughs half a period later", "[workload]") {
  // 1 + A cos(2 pi (t - peak) / P): the peak is base x (1 + A), the trough base x (1 - A).
  RateCurve c{.base = Rate{1000}};
  c.wave = Wave{.shape = WaveShape::sine, .period = 24h, .amplitude = 0.5, .peak_at = 20h};
  REQUIRE_THAT(c.rate_at(20h).per_second, WithinRel(1500.0, 1e-12));
  REQUIRE_THAT(c.rate_at(8h).per_second, WithinRel(500.0, 1e-12));
  REQUIRE_THAT(c.rate_at(14h).per_second, WithinRel(1000.0, 1e-9));  // quarter period: the mean
}

TEST_CASE("a square wave is high for the half period centred on the peak", "[workload]") {
  RateCurve c{.base = Rate{1000}};
  c.wave = Wave{.shape = WaveShape::square, .period = 24h, .amplitude = 0.5, .peak_at = 12h};
  REQUIRE(c.rate_at(12h).per_second == 1500);
  REQUIRE(c.rate_at(6h).per_second == 1500);  // 12h - 6h: the edge belongs to the high half
  REQUIRE(c.rate_at(5h + 59min).per_second == 500);
  REQUIRE(c.rate_at(0s).per_second == 500);
}

TEST_CASE("the diurnal shape has its minimum at night and averages one over a day", "[workload]") {
  RateCurve c{.base = Rate{1000}};
  c.wave = Wave{.shape = WaveShape::diurnal, .period = 24h, .amplitude = 1.0, .peak_at = 20h};
  REQUIRE(c.rate_at(4h).per_second < c.rate_at(20h).per_second);
  double sum = 0;
  for (int m = 0; m < 24 * 60; ++m) sum += c.rate_at(std::chrono::minutes{m}).per_second;
  // Exact, not approximate: the curve is periodic and piecewise linear with its breakpoints
  // on whole hours, which are sample points. On each hour the 60 left-endpoint samples sum
  // to 60 a + 29.5 (b - a) for endpoints a, b; over the day the (b - a) terms telescope to 0,
  // leaving 60 x (the table's sum, 24). So the left Riemann sum equals the true mean, 1.
  REQUIRE_THAT(sum / (24 * 60), WithinRel(1000.0, 1e-12));  // the table is normalised to mean 1
}

TEST_CASE("the diurnal wave reproduces the table at whole hours and is continuous across midnight",
          "[workload]") {
  const Wave d{.shape = WaveShape::diurnal, .period = 24h, .amplitude = 1.0, .peak_at = 20h};
  REQUIRE_THAT(wave_multiplier(d, 4h), WithinAbs(0.40, 1e-12));
  REQUIRE_THAT(wave_multiplier(d, 20h), WithinAbs(1.60, 1e-12));
  REQUIRE_THAT(wave_multiplier(d, 23h + 30min), WithinAbs(0.875, 1e-12));  // halfway 1.00 -> 0.75
  REQUIRE_THAT(wave_multiplier(d, 24h - 1ns), WithinAbs(wave_multiplier(d, 0s), 1e-9));
}

TEST_CASE("peak_at off the hour shifts the whole diurnal table rigidly", "[workload]") {
  const Wave d{
      .shape = WaveShape::diurnal, .period = 24h, .amplitude = 1.0, .peak_at = 20h + 30min};
  REQUIRE_THAT(wave_multiplier(d, 20h + 30min), WithinAbs(1.60, 1e-12));
  REQUIRE_THAT(wave_multiplier(d, 4h + 30min), WithinAbs(0.40, 1e-12));
  REQUIRE_THAT(wave_multiplier(d, 20h), WithinAbs(1.575, 1e-12));  // halfway table(19)=1.55 -> 1.60
}

TEST_CASE("a ramp moves linearly from `from` to `to` over `over`, then holds", "[workload]") {
  RateCurve c{.base = Rate{1000}};
  c.ramp = Ramp{.from = Rate{0}, .to = Rate{2000}, .over = 10min};
  REQUIRE(c.rate_at(0s).per_second == 0);
  REQUIRE_THAT(c.rate_at(5min).per_second, WithinRel(1000.0, 1e-12));
  REQUIRE(c.rate_at(10min).per_second == 2000);
  REQUIRE(c.rate_at(3h).per_second == 2000);
}

TEST_CASE("a ramp over zero time is a step at t = 0 from `from` to `to`", "[workload]") {
  RateCurve c{.base = Rate{1000}};
  c.ramp = Ramp{.from = Rate{500}, .to = Rate{2000}, .over = 0s};
  REQUIRE(c.rate_at(-1ns).per_second == 500);
  REQUIRE(c.rate_at(0s).per_second == 2000);
}

TEST_CASE("a ramp sets the level itself, so the base does not scale it", "[workload]") {
  // The ramp's rate is absolute: from + (to - from) x fraction. The base is not a divisor
  // (the loader still asks for one, to keep the file's intent explicit), so even base 0
  // gives the ramp's own rate.
  RateCurve zero_base{.base = Rate{0}};
  zero_base.ramp = Ramp{.from = Rate{0}, .to = Rate{2000}, .over = 10min};
  REQUIRE_THAT(zero_base.rate_at(5min).per_second, WithinRel(1000.0, 1e-12));
}

TEST_CASE("a ramp next to points is a programmer error", "[workload]") {
  // Points replace the level and so does a ramp: two levels at once have no meaning. The
  // config loader rejects the scenario ("rate.ramp needs rate.base").
  RateCurve with_points{.base = Rate{1000}};
  with_points.points = Points{.at = {{0s, Rate{1000}}}};
  with_points.ramp = Ramp{.from = Rate{0}, .to = Rate{2000}, .over = 10min};
  REQUIRE_THROWS_AS(with_points.rate_at(5min), std::invalid_argument);
}

TEST_CASE("a ramp on a tiny base yields a finite rate, even at a zero-wave trough", "[workload]") {
  // A ramp used to be the multiplier ramp/base: 1e15 / 1e-300 overflows to infinity, and
  // infinity x a wave trough of 0 is NaN. As an absolute level it is just 1e15 x the wave.
  const RateCurve curve{
      .base = Rate{1e-300},
      .wave = Wave{.shape = WaveShape::sine, .period = 2min, .amplitude = 1.0, .peak_at = 0s},
      .ramp = Ramp{.from = Rate{1e15}, .to = Rate{1e15}, .over = 5min}};
  for (const auto t : {0s, 30s, 60s, 90s}) REQUIRE(std::isfinite(curve.rate_at(t).per_second));
}

TEST_CASE(
    "scripted spikes multiply from at until at + duration, end exclusive, and overlapping spikes "
    "compound",
    "[workload]") {
  RateCurve c{.base = Rate{100}};
  c.spikes = {Spike{.at = 5min, .duration = 30s, .factor = 5},
              Spike{.at = 5min + 10s, .duration = 10s, .factor = 2}};
  REQUIRE(c.rate_at(4min + 59s).per_second == 100);
  REQUIRE(c.rate_at(5min).per_second == 500);
  REQUIRE(c.rate_at(5min + 15s).per_second == 1000);
  REQUIRE(c.rate_at(5min + 30s).per_second == 100);  // the end is exclusive
}

TEST_CASE("points replace the base with a piecewise-linear rate and hold the last value",
          "[workload]") {
  RateCurve c{.base = Rate{999}};
  c.points = Points{.at = {{0s, Rate{1000}}, {1h, Rate{5000}}}};
  REQUIRE(c.rate_at(0s).per_second == 1000);
  REQUIRE_THAT(c.rate_at(30min).per_second, WithinRel(3000.0, 1e-12));
  REQUIRE(c.rate_at(2h).per_second == 5000);
}

TEST_CASE("before the first point the rate is the first point's", "[workload]") {
  RateCurve c{.base = Rate{0}};
  c.points = Points{.at = {{1h, Rate{1000}}, {2h, Rate{2000}}}};
  REQUIRE(c.rate_at(0s).per_second == 1000);
}

TEST_CASE("two points at the same time make a step that takes the later value from that instant",
          "[workload]") {
  RateCurve c{.base = Rate{0}};
  c.points = Points{.at = {{1h, Rate{100}}, {1h, Rate{200}}, {2h, Rate{300}}}};
  REQUIRE(c.rate_at(1h - 1ns).per_second == 100);
  REQUIRE(c.rate_at(1h).per_second == 200);
  REQUIRE_THAT(c.rate_at(1h + 30min).per_second, WithinRel(250.0, 1e-12));
}

TEST_CASE("terms multiply: a wave on top of a spike on top of points", "[workload]") {
  RateCurve c{.base = Rate{0}};
  c.points = Points{.at = {{0s, Rate{1000}}, {1h, Rate{1000}}}};
  c.wave = Wave{.shape = WaveShape::sine, .period = 24h, .amplitude = 0.5, .peak_at = 0s};
  c.spikes = {Spike{.at = 0s, .duration = 1min, .factor = 2}};
  REQUIRE_THAT(c.rate_at(0s).per_second, WithinRel(1000.0 * 1.5 * 2, 1e-12));
}

TEST_CASE("time past 24 h wraps the wave by its period", "[workload]") {
  RateCurve c{.base = Rate{1000}};
  c.wave = Wave{.shape = WaveShape::sine, .period = 24h, .amplitude = 0.5, .peak_at = 20h};
  REQUIRE_THAT(c.rate_at(44h).per_second, WithinRel(1500.0, 1e-12));
}

TEST_CASE("wave_multiplier is exposed so the curve command can print the shape alone",
          "[workload]") {
  const Wave w{.shape = WaveShape::square, .period = 1h, .amplitude = 1.0, .peak_at = 0s};
  REQUIRE(simsys::workload::wave_multiplier(w, 0s) == 2.0);
  REQUIRE(simsys::workload::wave_multiplier(w, 30min) == 0.0);
}

TEST_CASE("the square wave's far edge is high at exactly a quarter period after the peak",
          "[workload]") {
  const Wave w{.shape = WaveShape::square, .period = 24h, .amplitude = 1.0, .peak_at = 12h};
  REQUIRE(wave_multiplier(w, 18h) == 2.0);
  REQUIRE(wave_multiplier(w, 18h + 1ns) == 0.0);
}

TEST_CASE("a wave amplitude outside zero to one is a programmer error", "[workload]") {
  const Wave w{.shape = WaveShape::sine, .period = 1h, .amplitude = 1.5, .peak_at = 0s};
  REQUIRE_THROWS_AS(wave_multiplier(w, 0s), std::invalid_argument);
}

TEST_CASE("a spike whose end would overflow the clock is still active after its start",
          "[workload]") {
  RateCurve c{.base = Rate{1}};
  c.spikes = {Spike{.at = 2h,
                    .duration = simsys::SimTime{std::numeric_limits<std::int64_t>::max()},
                    .factor = 3}};
  REQUIRE(c.rate_at(3h).per_second == 3);
}

using simsys::workload::Noise;
using simsys::workload::NoiseProcess;

TEST_CASE("noise with sigma zero is exactly one", "[workload]") {
  NoiseProcess n{Noise{.sigma = 0.0, .correlation = 5min}, /*seed=*/7};
  for (int i = 0; i < 10; ++i) REQUIRE(n.next(100ms) == 1.0);
}

TEST_CASE("the same seed gives the same noise path and a different seed differs", "[workload]") {
  const Noise spec{.sigma = 0.2, .correlation = 5min};
  NoiseProcess a{spec, 7}, b{spec, 7}, c{spec, 8};
  bool any_difference = false;
  for (int i = 0; i < 100; ++i) {
    const double x = a.next(100ms);
    REQUIRE(x == b.next(100ms));
    if (x != c.next(100ms)) any_difference = true;
  }
  REQUIRE(any_difference);
}

TEST_CASE("the noise multiplier is never negative", "[workload]") {
  NoiseProcess n{Noise{.sigma = 2.0, .correlation = 1s}, 1};
  for (int i = 0; i < 10'000; ++i) REQUIRE(n.next(100ms) >= 0.0);
}

TEST_CASE("over a long run the multiplier averages one with standard deviation sigma",
          "[workload]") {
  // Ornstein-Uhlenbeck with stationary standard deviation sigma; sampled once per correlation
  // time the values are nearly independent, so 20k samples pin the moments to a few percent.
  NoiseProcess n{Noise{.sigma = 0.1, .correlation = 1s}, 42};
  double sum = 0, sum2 = 0;
  const int samples = 20'000;
  for (int i = 0; i < samples; ++i) {
    const double x = n.next(1s);
    sum += x;
    sum2 += x * x;
  }
  const double mean = sum / samples, var = sum2 / samples - mean * mean;
  REQUIRE_THAT(mean, WithinAbs(1.0, 0.01));
  REQUIRE_THAT(std::sqrt(var), WithinAbs(0.1, 0.01));
}

// The multiplier is max(0, 1 + x) with x ~ N(0, sigma^2), so its mean is not 1 but
//   E[max(0, 1 + sigma Z)] = Phi(1/sigma) + sigma phi(1/sigma)
// (Phi, phi: the standard normal CDF and density): cutting off the negative tail adds back
// what it would have subtracted. 1.004 at sigma 0.5, 1.083 at sigma 1, 1.40 at 2, 4.5 at 10.
TEST_CASE("the noise floor lifts the mean multiplier: about 1.08 at sigma one", "[workload]") {
  NoiseProcess n{Noise{.sigma = 1.0, .correlation = 1s}, 7};
  double sum = 0;
  const int samples = 200'000;  // at dt = tau consecutive samples correlate by e^-1 only
  for (int i = 0; i < samples; ++i) sum += n.next(1s);
  const double floor_mean = 0.8413447460685429 + 0.24197072451914337;  // Phi(1) + 1 phi(1)
  REQUIRE_THAT(sum / samples, WithinAbs(floor_mean, 0.02));
}

TEST_CASE("consecutive samples are correlated over the correlation time", "[workload]") {
  NoiseProcess n{Noise{.sigma = 0.3, .correlation = 10s}, 3};
  double prev = n.next(100ms), cov = 0, var = 0;
  const int samples = 50'000;
  for (int i = 0; i < samples; ++i) {
    const double x = n.next(100ms);
    cov += (x - 1) * (prev - 1);
    var += (x - 1) * (x - 1);
    prev = x;
  }
  REQUIRE(cov / var > 0.95);  // e^{-0.1/10} = 0.990
}

TEST_CASE("a non-positive correlation time or a negative sigma is a programmer error",
          "[workload]") {
  REQUIRE_THROWS_AS((NoiseProcess{Noise{.sigma = 0.1, .correlation = 0s}, 1}),
                    std::invalid_argument);
  REQUIRE_THROWS_AS((NoiseProcess{Noise{.sigma = -0.1, .correlation = 1s}, 1}),
                    std::invalid_argument);
}

TEST_CASE("samples yields one rate per step from zero up to but excluding until", "[workload]") {
  RateCurve c{.base = Rate{100}};
  c.spikes = {Spike{.at = 300ms, .duration = 100ms, .factor = 3}};
  std::vector<double> got;
  // C++23 std::generator: a lazy coroutine range; the curve is sampled only as far as consumed.
  // It replaces building a whole std::vector up front or hand-writing an iterator class.
  for (const auto& s : simsys::workload::samples(c, 100ms, 500ms)) got.push_back(s.rate.per_second);
  REQUIRE(got == std::vector<double>{100, 100, 100, 300, 100});
}

TEST_CASE("samples carries the time of each sample", "[workload]") {
  const RateCurve c{.base = Rate{1}};
  auto gen = simsys::workload::samples(c, 1s, 3s);
  // std::generator's iterator is input-only: it has operator* but no operator->, so (*it).t.
  auto it = gen.begin();
  REQUIRE((*it).t == 0s);
  ++it;
  REQUIRE((*it).t == 1s);
  ++it;
  REQUIRE((*it).t == 2s);
  ++it;
  REQUIRE(it == gen.end());
}

TEST_CASE("samples with noise applies the seeded multiplier and stays deterministic",
          "[workload]") {
  const RateCurve c{.base = Rate{1000}};
  const Noise noise{.sigma = 0.2, .correlation = 1s};
  std::vector<double> a, b;
  for (const auto& s : simsys::workload::samples(c, 100ms, 1s, noise, /*seed=*/5))
    a.push_back(s.rate.per_second);
  for (const auto& s : simsys::workload::samples(c, 100ms, 1s, noise, /*seed=*/5))
    b.push_back(s.rate.per_second);
  REQUIRE(a == b);
  REQUIRE(a.size() == 10);
  REQUIRE(a != std::vector<double>(10, 1000.0));  // the noise did something
}

TEST_CASE("samples with a non-positive step is a programmer error", "[workload]") {
  const RateCurve c{.base = Rate{1}};
  REQUIRE_THROWS_AS(simsys::workload::samples(c, 0s, 1s).begin(), std::invalid_argument);
}

TEST_CASE("samples stops cleanly when the next step would overflow the clock", "[workload]") {
  const RateCurve c{.base = Rate{1}};
  const simsys::SimTime huge{std::numeric_limits<std::int64_t>::max() - 10};
  int n = 0;
  for (const auto& s : simsys::workload::samples(
           c, huge, simsys::SimTime{std::numeric_limits<std::int64_t>::max()})) {
    ++n;
    REQUIRE(s.t.count() >= 0);
  }
  REQUIRE(n == 2);  // t = 0 and t = huge; a third step would overflow
}

TEST_CASE(
    "the first noisy sample already includes one noise step, as the engine's first offer does",
    "[workload]") {
  // The engine ticks a client's noise by dt and then offers (engine forward pass), so the
  // rate offered at t = 0 is already curve x one noise step; samples does the same.
  const RateCurve c{.base = Rate{1000}};
  const Noise noise{.sigma = 0.2, .correlation = 1s};
  NoiseProcess process{noise, 5};
  const double first_multiplier = process.next(100ms);
  auto gen = simsys::workload::samples(c, 100ms, 1s, noise, 5);
  REQUIRE((*gen.begin()).rate.per_second == 1000 * first_multiplier);
}

TEST_CASE("noisy samples advance the noise at the noise step and keep every k-th sample",
          "[workload]") {
  // `simsys curve --noise --step 1s` on a scenario stepping every 100 ms must print the noise
  // a run sees: the process advances every 100 ms and every tenth sample is printed.
  const RateCurve c{.base = Rate{1000}};
  const Noise noise{.sigma = 0.2, .correlation = 1s};
  std::vector<double> fine, coarse;
  for (const auto& s : simsys::workload::samples(c, 100ms, 3s, noise, 5))
    fine.push_back(s.rate.per_second);
  for (const auto& s : simsys::workload::samples(c, 1s, 3s, noise, 5, /*noise_step=*/100ms))
    coarse.push_back(s.rate.per_second);
  REQUIRE(fine.size() == 30);
  REQUIRE(coarse == std::vector<double>{fine[0], fine[10], fine[20]});
}

TEST_CASE("a sample step that is not a multiple of the noise step is a programmer error",
          "[workload]") {
  const RateCurve c{.base = Rate{1}};
  const Noise noise{.sigma = 0.2, .correlation = 1s};
  REQUIRE_THROWS_AS(simsys::workload::samples(c, 150ms, 1s, noise, 5, 100ms).begin(),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(simsys::workload::samples(c, 1s, 1s, noise, 5, 0s).begin(),
                    std::invalid_argument);
}

TEST_CASE("wave shape names round-trip through one table", "[workload]") {
  for (const auto& row : simsys::workload::wave_shape_table) {
    REQUIRE(simsys::workload::parse_wave_shape(row.name) == row.value);
    REQUIRE(simsys::workload::to_string(row.value) == row.name);
  }
  REQUIRE_FALSE(simsys::workload::parse_wave_shape("triangle").has_value());
}
