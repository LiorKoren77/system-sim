// The rate curve a client follows: a base rate shaped by multiplicative terms (spec §8).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
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
  REQUIRE_THAT(sum / (24 * 60), WithinRel(1000.0, 1e-3));  // the table is normalised to mean 1
}

TEST_CASE("a ramp moves linearly from `from` to `to` over `over`, then holds", "[workload]") {
  RateCurve c{.base = Rate{1000}};
  c.ramp = Ramp{.from = Rate{0}, .to = Rate{2000}, .over = 10min};
  REQUIRE(c.rate_at(0s).per_second == 0);
  REQUIRE_THAT(c.rate_at(5min).per_second, WithinRel(1000.0, 1e-12));
  REQUIRE(c.rate_at(10min).per_second == 2000);
  REQUIRE(c.rate_at(3h).per_second == 2000);
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
