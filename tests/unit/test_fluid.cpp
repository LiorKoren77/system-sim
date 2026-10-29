// The fluid step of a single node (spec §3.3), checked against the formulas and
// against the conservation law flow_in = flow_out + drops + delta_backlog.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.core;
import simsys.fluid;
import simsys.queueing;

using Catch::Approx;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using namespace std::chrono_literals;
using simsys::Rate;
using simsys::fluid::Capacity;
using simsys::fluid::FluidQueue;
using simsys::fluid::ServiceMoments;
using simsys::fluid::Zone;

namespace {
// 1000 req/s, 1 ms service, one server, room for 1000 queued requests.
// C++20 designated initializers: `.field = value` names each member it sets, in
// declaration order, and the rest keep their defaults. Replaces positional aggregate
// initialization, which could not skip `concurrency` here and where swapped numbers
// compile silently.
Capacity web() {
  return Capacity{.service_rate = Rate{1000}, .service_time_s = 0.001, .queue_limit = 1000};
}
}  // namespace

TEST_CASE("an underloaded queue drains everything with no backlog", "[fluid]") {
  FluidQueue q{web()};
  const auto s = q.advance(Rate{500}, 100ms);
  REQUIRE(s.drain_per_s == Approx(500));
  REQUIRE(s.dropped_per_s == 0.0);
  REQUIRE(s.backlog == 0.0);
  REQUIRE(s.rho == Approx(0.5));
  REQUIRE(s.zone == Zone::green);
}

TEST_CASE("sojourn under load is M/M/1 wait plus service time", "[fluid]") {
  FluidQueue q{web()};
  const auto s = q.advance(Rate{500}, 100ms);
  // Wq = rho/(mu-lambda) = 1 ms, plus S = 1 ms.
  REQUIRE(s.latency.mean_s == Approx(0.002));
  // Var(Wq) = 0.75 * (2 ms)^2 = 3e-6; Var(S) = cs^2 * S^2 = 1e-6 (exponential).
  REQUIRE(s.latency.variance_s2 == Approx(4e-6));
}

TEST_CASE("overload grows the backlog by (demand - capacity) * dt each step", "[fluid]") {
  FluidQueue q{web()};
  const auto s1 = q.advance(Rate{1500}, 100ms);
  REQUIRE(s1.drain_per_s == Approx(1000));
  REQUIRE(s1.backlog == Approx(50));
  REQUIRE(s1.rho == 1.0);
  REQUIRE(s1.zone == Zone::red);
  const auto s2 = q.advance(Rate{1500}, 100ms);
  REQUIRE(s2.backlog == Approx(100));
  REQUIRE(s2.rho == 1.0);
  // Fluid wait = backlog / capacity = 100 ms. It wins over the 99 ms Erlang term (rho
  // capped at 0.99) instead of being added to it; plus 1 ms service.
  REQUIRE_THAT(s2.latency.mean_s, WithinRel(0.100 + 0.001, 1e-12));
}

TEST_CASE("a full backlog overflows into drops at demand minus capacity", "[fluid]") {
  FluidQueue q{web()};
  for (int i = 0; i < 20; ++i) (void)q.advance(Rate{1500}, 100ms);  // backlog reaches 1000
  const auto s = q.advance(Rate{1500}, 100ms);
  REQUIRE(s.backlog == Approx(1000));
  REQUIRE(s.dropped_per_s == Approx(500));
}

TEST_CASE("a backlog drains at capacity once demand stops", "[fluid]") {
  FluidQueue q{web()};
  (void)q.advance(Rate{1500}, 100ms);  // backlog 50
  const auto s = q.advance(Rate{0}, 100ms);
  REQUIRE(s.drain_per_s == Approx(500));  // 50 requests in 0.1 s
  REQUIRE(s.backlog == Approx(0).margin(1e-9));
}

TEST_CASE("flow is conserved every step: in = out + drops + delta backlog", "[fluid]") {
  FluidQueue q{web()};
  double previous_backlog = 0.0;
  const double dt = 0.1;
  for (const double demand : {200.0, 1500.0, 1500.0, 3000.0, 3000.0, 0.0, 0.0}) {
    const auto s = q.advance(Rate{demand}, 100ms);
    const double delta = s.backlog - previous_backlog;
    // Catch2 matchers instead of Approx: `Approx(x).margin(m)` passes when the value is
    // within m OR within Approx's default relative epsilon (~1.19e-5), so a margin can
    // only loosen a check, never tighten it. WithinAbs / WithinRel check exactly one
    // tolerance, which is what a conservation law needs.
    REQUIRE_THAT(demand * dt, WithinAbs((s.drain_per_s + s.dropped_per_s) * dt + delta, 1e-9));
    previous_backlog = s.backlog;
  }
}

TEST_CASE("zones follow the yellow and red thresholds", "[fluid]") {
  const auto c = web();  // defaults yellow 0.70, red 0.90
  REQUIRE(simsys::fluid::zone_of(0.69, c) == Zone::green);
  REQUIRE(simsys::fluid::zone_of(0.70, c) == Zone::yellow);
  REQUIRE(simsys::fluid::zone_of(0.90, c) == Zone::red);
}

TEST_CASE("the Erlang term is evaluated at rho 0.99 when the backlog is still empty", "[fluid]") {
  FluidQueue q{web()};
  const auto s = q.advance(Rate{1000}, 100ms);  // rho exactly 1, backlog still 0
  // M/M/1 Wq at rho 0.99 = rho/(mu - lambda) = 0.99/(1000 - 990) = 99 ms, plus 1 ms service.
  REQUIRE_THAT(s.latency.mean_s, WithinRel(0.100, 1e-12));
}

TEST_CASE("settle jumps to the long-run state: empty below capacity, full above", "[fluid]") {
  FluidQueue under{web()};
  under.settle(Rate{500});
  REQUIRE(under.backlog() == 0.0);
  FluidQueue over{web()};
  over.settle(Rate{1500});
  REQUIRE(over.backlog() == Approx(1000));
}

TEST_CASE("burstiness and service variability scale the stochastic wait (Kingman)", "[fluid]") {
  auto bursty = web();
  bursty.burstiness = 3.0;           // ca^2
  bursty.service_variability = 3.0;  // cs^2; k = (3 + 3)/2 = 3x the M/M/1 wait
  FluidQueue q{bursty};
  const auto s = q.advance(Rate{500}, 100ms);
  REQUIRE_THAT(s.latency.mean_s, WithinRel(0.003 + 0.001, 1e-12));  // 3 ms wait + 1 ms service
  // k^2 * Var(Wq) = 9 * 3e-6, plus the service variance cs^2 * S^2 = 3 * (1 ms)^2.
  REQUIRE_THAT(s.latency.variance_s2, WithinRel(9 * 3e-6 + 3 * 0.001 * 0.001, 1e-12));
}

TEST_CASE("flow is conserved to rounding under adversarial demand, dt and queue limits",
          "[fluid]") {
  for (const double limit : {0.0, 0.3, 1000.0}) {
    auto c = web();
    c.queue_limit = limit;
    FluidQueue q{c};
    const simsys::SimTime dts[] = {1us, 100ms, 1h};
    for (const double demand : {0.0, 1e9, 1000.0, 0.0, 1500.0, 1e9, 200.0})
      for (const auto dt : dts) {
        CAPTURE(limit, demand, dt.count());
        const double before = q.backlog(), dt_s = simsys::seconds(dt);
        const auto s = q.advance(Rate{demand}, dt);
        const double residual =
            demand * dt_s - (s.drain_per_s + s.dropped_per_s) * dt_s - (s.backlog - before);
        REQUIRE(std::abs(residual) <= 1e-12 * std::max({demand * dt_s, before, 1.0}));
      }
  }
}

TEST_CASE("a backlog larger than one step's spare capacity drains at full capacity", "[fluid]") {
  FluidQueue q{web()};
  q.settle(Rate{1500});                        // backlog 1000
  const auto s = q.advance(Rate{500}, 100ms);  // spare (1000-500)/s * 0.1 s = 50
  REQUIRE(s.drain_per_s == Approx(1000));
  REQUIRE(s.backlog == Approx(950));
  REQUIRE(s.rho == Approx(1.0));  // busy draining, not "50% utilized"
  REQUIRE(s.zone == Zone::red);
  // Fluid wait 950/1000 s wins over the 99 ms Erlang term, plus 1 ms service.
  REQUIRE_THAT(s.latency.mean_s, WithinRel(0.950 + 0.001, 1e-12));
  // The variance keeps the stochastic term even though the fluid mean wins. M/M/1 at the
  // capped rho 0.99 (c = 1, so P_wait = rho = 0.99; k = 1 for Poisson/exponential):
  //   theta   = s / (c (1 - rho)) = 0.001 / 0.01 = 0.1 s
  //   Var(Wq) = P (2 - P) theta^2 = 0.99 * 1.01 * 0.01 = 0.9999 * 0.01 = 0.009999 s^2
  //   Var(S)  = cs^2 * s^2 = 1 * (0.001)^2 = 1e-6 s^2
  REQUIRE_THAT(s.latency.variance_s2, WithinRel(0.009999 + 1e-6, 1e-9));
}

TEST_CASE("estimated p99 never decreases while overload deepens", "[fluid]") {
  // The variance must not jump when the fluid wait overtakes the stochastic one,
  // otherwise p99 would fall as the backlog grows (review finding).
  FluidQueue q{web()};
  double previous_p99 = 0.0;
  for (int step = 0; step < 10; ++step) {
    const auto s = q.advance(Rate{1500}, 100ms);
    const double p99 = simsys::queueing::lognormal_quantile(s.latency.mean_s, s.latency.variance_s2,
                                                            simsys::queueing::z_p99);
    CAPTURE(step, s.latency.mean_s, s.latency.variance_s2);
    REQUIRE(p99 >= previous_p99);
    previous_p99 = p99;
  }
}

TEST_CASE("a node with no capacity drains nothing and reports an unbounded wait", "[fluid]") {
  auto c = web();
  c.service_rate = Rate{0};
  FluidQueue q{c};
  const auto s = q.advance(Rate{100}, 100ms);
  REQUIRE(s.drain_per_s == 0.0);
  REQUIRE(s.backlog == Approx(10));
  REQUIRE(s.zone == Zone::red);
  REQUIRE(std::isinf(s.latency.mean_s));
}

TEST_CASE("a non-positive step is a programmer error", "[fluid]") {
  FluidQueue q{web()};
  REQUIRE_THROWS_AS(q.advance(Rate{1}, 0ns), std::invalid_argument);
  REQUIRE_THROWS_AS(q.advance(Rate{1}, -100ms), std::invalid_argument);
}

TEST_CASE("a NaN demand propagates instead of being served", "[fluid]") {
  FluidQueue q{web()};
  const auto s = q.advance(Rate{std::numeric_limits<double>::quiet_NaN()}, 100ms);
  REQUIRE(std::isnan(s.drain_per_s));
}

// The step reports its queueing wait apart from the sojourn: the M/M/1 node above at rho 0.5
// waits Wq = 1 ms (variance 3e-6 s²), and its sojourn adds one service time of 1 ms (variance
// cs² × S² = 1e-6 s²). A byte-sized node adds each op's own service to the wait instead
// (engine::Simulation::own_sojourn).
TEST_CASE("a step reports the queueing wait apart from the service time", "[fluid]") {
  FluidQueue q{web()};
  const auto s = q.advance(Rate{500}, 100ms);
  REQUIRE_THAT(s.wait.mean_s, WithinRel(0.001, 1e-12));
  REQUIRE_THAT(s.wait.variance_s2, WithinRel(3e-6, 1e-12));
  REQUIRE_THAT(s.latency.mean_s, WithinRel(s.wait.mean_s + 0.001, 1e-12));
  REQUIRE_THAT(s.latency.variance_s2, WithinRel(s.wait.variance_s2 + 1e-6, 1e-12));
}

// A byte-sized node's queue is fed the moments of its objects' service time each step (review of
// plan 2c Task 3, I1, and of its fix round, I1) while its rate and c stay those of its megabytes,
// so rho does not move and the wait scales with the mean time: a 20-slot store of 500 MB/s at
// 250 MB/s is at rho 0.5 whether one slot is held 40 ms (one MB) or 2 s (a 50 MB object), and with
// exponential service (SCV 1) Erlang C's wait is C(20, 0.5) x s / (20 x 0.5): 0.0149 ms against
// 0.746 ms.
TEST_CASE("setting the service moments changes the Erlang wait and leaves rho alone", "[fluid]") {
  FluidQueue q{Capacity{.service_rate = Rate{500}, .service_time_s = 0.040, .concurrency = 20}};
  const auto per_megabyte = q.advance(Rate{250}, 100ms);
  q.set_service(ServiceMoments{.mean_s = 2.0, .scv = 1.0});
  const auto per_object = q.advance(Rate{250}, 100ms);
  REQUIRE_THAT(per_megabyte.rho, WithinRel(0.5, 1e-12));
  REQUIRE_THAT(per_object.rho, WithinRel(0.5, 1e-12));
  REQUIRE_THAT(per_megabyte.wait.mean_s, WithinAbs(1.4925e-5, 1e-9));
  REQUIRE_THAT(per_object.wait.mean_s, WithinAbs(7.4623e-4, 1e-8));
  REQUIRE_THAT(per_object.latency.mean_s, WithinRel(per_object.wait.mean_s + 2.0, 1e-12));
}

// The SCV the service moments carry is the c_s^2 of the Allen-Cunneen factor (c_a^2 + c_s^2) / 2:
// a deterministic service (SCV 0, M/D/c) with Poisson arrivals waits (1 + 0) / 2 = half the M/M/c
// wait, 0.373 ms where exponential service waits 0.746 ms; an SCV of 3 (a spread-out size mix)
// waits (1 + 3) / 2 = twice as long, 1.49 ms.
TEST_CASE("the service's squared coefficient of variation scales the stochastic wait", "[fluid]") {
  const auto wait_with = [](double scv) {
    FluidQueue q{Capacity{.service_rate = Rate{500}, .service_time_s = 0.040, .concurrency = 20}};
    q.set_service(ServiceMoments{.mean_s = 2.0, .scv = scv});
    return q.advance(Rate{250}, 100ms).wait.mean_s;
  };
  const double exponential = simsys::queueing::mmc_wait(20, 0.5, 2.0).mean_s;
  REQUIRE_THAT(wait_with(1.0), WithinRel(exponential, 1e-12));
  REQUIRE_THAT(wait_with(0.0), WithinRel(exponential / 2.0, 1e-12));
  REQUIRE_THAT(wait_with(0.0), WithinAbs(3.7311e-4, 1e-8));
  REQUIRE_THAT(wait_with(3.0), WithinRel(exponential * 2.0, 1e-12));
}

TEST_CASE(
    "service moments with a mean that is not positive or a negative scv are a programmer error",
    "[fluid]") {
  FluidQueue q{web()};
  constexpr double nan = std::numeric_limits<double>::quiet_NaN();
  REQUIRE_THROWS_AS(q.set_service(ServiceMoments{.mean_s = 0.0, .scv = 1.0}),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(q.set_service(ServiceMoments{.mean_s = -1.0, .scv = 1.0}),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(q.set_service(ServiceMoments{.mean_s = nan, .scv = 1.0}),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(q.set_service(ServiceMoments{.mean_s = 1.0, .scv = -0.5}),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(q.set_service(ServiceMoments{.mean_s = 1.0, .scv = nan}),
                    std::invalid_argument);
}

// The room-drain bound (review of plan 2c Task 3's fix round, C1): a request admitted to a waiting
// room of K units cannot wait longer than the room takes to drain, K / service_rate, so the
// stochastic term is min(Kingman wait, K / rate) (exact for a deterministic drain, conservative
// for random service: M/M/c/K's mean wait is below it). A 500 MB/s store with a 1000 MB room fed
// 600 MB/s of 2 s objects is past saturation: Erlang C at the 0.99 clamp would say C(20, 0.99) x
// 2 s / (20 x 0.01) = 9.48 s, but the room drains in 1000 / 500 = 2 s, and that is the wait. Its
// spread is capped at the room's scale, T^2 / 4 (our heuristic, not a theorem), here 1 s^2, where
// the clamp's variance is 99.7 s^2: the cap is what the variance is.
TEST_CASE("a queue's stochastic wait is bounded by the time its waiting room takes to drain",
          "[fluid]") {
  FluidQueue q{Capacity{
      .service_rate = Rate{500}, .service_time_s = 0.040, .concurrency = 20, .queue_limit = 1000}};
  q.set_service(ServiceMoments{.mean_s = 2.0, .scv = 1.0});
  const auto s = q.advance(Rate{600}, 100ms);  // backlog 10 MB: a fluid wait of 20 ms only
  REQUIRE_THAT(simsys::queueing::mmc_wait(20, 0.99, 2.0).mean_s, WithinAbs(9.4787, 1e-4));
  REQUIRE_THAT(s.wait.mean_s, WithinRel(2.0, 1e-12));
  REQUIRE_THAT(s.wait.variance_s2, WithinRel(1.0, 1e-12));  // T^2 / 4 at T = 2 s
  REQUIRE_THAT(s.latency.mean_s, WithinRel(4.0, 1e-12));
}

// With the default room the bound does not bind at a node that counts requests unless its arrivals
// are very bursty (a variability factor above 10 for the mean, 5 for the variance cap): the room
// holds 1000 requests, ten times the ~100 that Erlang C's queue holds at the 0.99 clamp (Lq =
// rho^2 / (1 - rho) = 98 at one server). The web node above still waits its 99 ms there, against a
// room that drains in 1000 / 1000/s = 1 s.
TEST_CASE(
    "the room-drain bound does not bind at a request-sized node with the default room and Poisson "
    "arrivals",
    "[fluid]") {
  FluidQueue q{web()};
  const auto s = q.advance(Rate{2000}, 1ms);  // backlog 1: a fluid wait of 1 ms
  REQUIRE_THAT(s.wait.mean_s, WithinRel(0.099, 1e-9));
  REQUIRE_THAT(s.wait.variance_s2, WithinRel(0.009999, 1e-9));
}
