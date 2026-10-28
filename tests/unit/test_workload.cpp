// The rate curve a client follows: a base rate shaped by multiplicative terms (spec §8).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
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
