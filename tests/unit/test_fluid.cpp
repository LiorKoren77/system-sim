// The fluid step of a single node (spec §3.3), checked against the formulas and
// against the conservation law flow_in = flow_out + drops + delta_backlog.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.core;
import simsys.fluid;

using Catch::Approx;
using namespace std::chrono_literals;
using simsys::Rate;
using simsys::fluid::Capacity;
using simsys::fluid::FluidQueue;
using simsys::fluid::Zone;

namespace {
// 1000 req/s, 1 ms service, one server, room for 1000 queued requests.
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
  // Fluid wait = backlog / capacity (Little's law on the backlog).
  REQUIRE(s2.latency.mean_s >= 0.1);
}

TEST_CASE("a full backlog overflows into drops at demand minus capacity", "[fluid]") {
  FluidQueue q{web()};
  for (int i = 0; i < 20; ++i) q.advance(Rate{1500}, 100ms);  // backlog reaches 1000
  const auto s = q.advance(Rate{1500}, 100ms);
  REQUIRE(s.backlog == Approx(1000));
  REQUIRE(s.dropped_per_s == Approx(500));
}

TEST_CASE("a backlog drains at capacity once demand stops", "[fluid]") {
  FluidQueue q{web()};
  q.advance(Rate{1500}, 100ms);  // backlog 50
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
    REQUIRE(demand * dt == Approx((s.drain_per_s + s.dropped_per_s) * dt + delta).margin(1e-9));
    previous_backlog = s.backlog;
  }
}

TEST_CASE("zones follow the yellow and red thresholds", "[fluid]") {
  const auto c = web();  // defaults yellow 0.70, red 0.90
  REQUIRE(simsys::fluid::zone_of(0.69, c) == Zone::green);
  REQUIRE(simsys::fluid::zone_of(0.70, c) == Zone::yellow);
  REQUIRE(simsys::fluid::zone_of(0.90, c) == Zone::red);
}

TEST_CASE("the stochastic wait is capped at rho 0.99 so saturation is not double counted", "[fluid]") {
  FluidQueue q{web()};
  const auto s = q.advance(Rate{1000}, 100ms);  // rho exactly 1, backlog still 0
  REQUIRE(s.latency.mean_s < 1.0);              // finite: Erlang term evaluated at 0.99
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
  bursty.burstiness = 3.0;  // (3 + 1)/2 = 2x the M/M/1 wait
  FluidQueue q{bursty};
  const auto s = q.advance(Rate{500}, 100ms);
  REQUIRE(s.latency.mean_s == Approx(0.002 + 0.001));  // 2 ms wait + 1 ms service
}
