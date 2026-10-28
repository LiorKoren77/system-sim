// Properties of the per-op flow map and of latency-moment composition.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <stdexcept>
#include <string>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.core;
import simsys.flow;

using Catch::Approx;
using simsys::Rate;
using simsys::flow::Flow;
using simsys::flow::LatencyEstimate;

TEST_CASE("Flow starts empty and accumulates rates per op", "[flow]") {
  Flow f;
  REQUIRE(f.empty());
  f.add("read", Rate{100});
  f.add("read", Rate{50});
  f.add("write", Rate{10});
  REQUIRE_FALSE(f.empty());
  REQUIRE(f.rate_of("read").per_second == Approx(150));
  REQUIRE(f.rate_of("write").per_second == Approx(10));
  REQUIRE(f.rate_of("missing").per_second == 0.0);
  REQUIRE(f.total().per_second == Approx(160));
}

TEST_CASE("Flow keeps its ops sorted so iteration order is deterministic", "[flow]") {
  Flow f;
  f.add("zeta", Rate{1});
  f.add("alpha", Rate{2});
  f.add("mid", Rate{3});
  const auto entries = f.entries();
  REQUIRE(entries.size() == 3);
  REQUIRE(entries[0].op == "alpha");
  REQUIRE(entries[1].op == "mid");
  REQUIRE(entries[2].op == "zeta");
}

TEST_CASE("Flow addition merges ops and scaling multiplies every rate", "[flow]") {
  Flow a;
  a.add("read", Rate{100});
  Flow b;
  b.add("read", Rate{20});
  b.add("write", Rate{5});
  a += b;
  REQUIRE(a.rate_of("read").per_second == Approx(120));
  REQUIRE(a.rate_of("write").per_second == Approx(5));
  const Flow half = a.scaled(0.5);
  REQUIRE(half.rate_of("read").per_second == Approx(60));
  REQUIRE(half.rate_of("write").per_second == Approx(2.5));
  REQUIRE(a.rate_of("read").per_second == Approx(120));  // scaled() does not mutate
}

TEST_CASE("Flow::clear empties the map", "[flow]") {
  Flow f;
  f.add("x", Rate{1});
  f.clear();
  REQUIRE(f.empty());
}

TEST_CASE("Sequential hops add latency means and variances", "[flow]") {
  // Independence assumption: Var(X+Y) = Var X + Var Y.
  // C++20 designated initializers: `{.mean_s = ..., .variance_s2 = ...}` names each field
  // at the construction site. Positional `{0.001, 1e-6}` would compile just as happily
  // with the two doubles swapped; here the reader sees which number is which.
  const LatencyEstimate a{.mean_s = 0.001, .variance_s2 = 1e-6};
  const LatencyEstimate b{.mean_s = 0.002, .variance_s2 = 4e-6};
  const auto sum = a + b;
  REQUIRE(sum.mean_s == Approx(0.003));
  REQUIRE(sum.variance_s2 == Approx(5e-6));
}

TEST_CASE("Parallel branches take the largest mean and the largest variance", "[flow]") {
  const std::array branches{LatencyEstimate{0.001, 9e-6}, LatencyEstimate{0.005, 1e-6}};
  const auto m = simsys::flow::parallel_max(branches);
  REQUIRE(m.mean_s == Approx(0.005));
  REQUIRE(m.variance_s2 == Approx(9e-6));
  REQUIRE(simsys::flow::parallel_max({}) == LatencyEstimate{});
}

TEST_CASE("Adding a flow to itself doubles every rate", "[flow]") {
  Flow f;
  f.add("write", Rate{1});
  f.add("read", Rate{3});
  f += f;
  REQUIRE(f.entries().size() == 2);
  REQUIRE(f.rate_of("read").per_second == 6.0);
  REQUIRE(f.rate_of("write").per_second == 2.0);
}

TEST_CASE("Ops match by whole name, never by prefix", "[flow]") {
  Flow f;
  f.add("read", Rate{5});
  REQUIRE(f.rate_of("rea").per_second == 0.0);       // lower_bound lands on "read"
  REQUIRE(f.rate_of("read_all").per_second == 0.0);  // lower_bound lands at end
  f.add("rea", Rate{1});                             // inserted before "read", shifting it
  f.add("read", Rate{1});                            // must still accumulate into "read"
  REQUIRE(f.entries().size() == 2);
  REQUIRE(f.rate_of("read").per_second == 6.0);
  REQUIRE(f.rate_of("rea").per_second == 1.0);
}

TEST_CASE("Negative or NaN rates and factors are programmer errors", "[flow]") {
  Flow f;
  f.add("read", Rate{1});
  REQUIRE_THROWS_AS(f.add("read", Rate{-1}), std::invalid_argument);
  REQUIRE_THROWS_AS(f.scaled(-0.5), std::invalid_argument);
  REQUIRE_THROWS_AS(f.scaled(std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
  REQUIRE(f.rate_of("read").per_second == 1.0);  // a rejected call leaves the flow unchanged
}

TEST_CASE("scale multiplies in place without allocating a new flow", "[flow]") {
  Flow f;
  f.add("read", Rate{4});
  f.scale(0.25);
  REQUIRE(f.rate_of("read").per_second == 1.0);
}

TEST_CASE("LatencyEstimate equality is member-wise and has no ordering", "[flow]") {
  // C++20 defaulted operator==: only equality is defaulted; `<` on an estimate would be
  // meaningless (is {1 s, 100 s^2} smaller than {2 s, 0}?), so it is not provided.
  REQUIRE(LatencyEstimate{1.0, 2.0} == LatencyEstimate{1.0, 2.0});
  REQUIRE(LatencyEstimate{1.0, 2.0} != LatencyEstimate{1.0, 3.0});
}
