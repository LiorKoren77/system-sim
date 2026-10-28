// The link delay line: a ring buffer of flows, one slot per step of latency (spec §3.4).
#include <catch2/catch_test_macros.hpp>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.core;
import simsys.engine;
import simsys.flow;

using simsys::Rate;
using simsys::engine::DelayLine;
using simsys::flow::Flow;

TEST_CASE("a zero-step delay line passes flow through in the same step", "[engine]") {
  DelayLine line{0};
  Flow f;
  f.add("x", Rate{5});
  line.push(f);
  REQUIRE(line.pop().rate_of("x").per_second == 5);
  line.push(Flow{});
  REQUIRE(line.pop().empty());
}

TEST_CASE("an n-step delay line returns a flow n pops later", "[engine]") {
  DelayLine line{2};
  Flow f;
  f.add("x", Rate{5});
  line.push(f);
  REQUIRE(line.pop().empty());  // step 0
  line.push(Flow{});
  REQUIRE(line.pop().empty());  // step 1
  line.push(Flow{});
  REQUIRE(line.pop().rate_of("x").per_second == 5);  // step 2
}

TEST_CASE("a delay line that is popped before any push yields nothing", "[engine]") {
  DelayLine line{1};
  REQUIRE(line.pop().empty());
}
