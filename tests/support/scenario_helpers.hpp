// Scenarios, numbers and texts more than one test file needs: the ring imbalance formula, a piped
// client-to-service scenario, and `explain` of a scenario built in code. A plain header, like
// explain_lines.hpp, included after the test's std headers and before its own imports: the std
// headers it needs come first (a no-op when the test included them already), then the modules.
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>

// C++20 `import` in a header: the #include copies these lines into the test, which then imports
// the modules as if it had written them (importing one twice is harmless).
import simsys.core;
import simsys.graph;
import simsys.model;
import simsys.queueing;
import simsys.report;

namespace simsys::test_support {

// A consistent-hash ring's imbalance k, the hottest machine's load over the average: each of M
// machines holds the keys of its v virtual nodes' arcs, N copies of each key (N = 1 behind a
// balancer), so its share is a sum of N v arc lengths whose relative spread is about 1/sqrt(N v)
// (book ch. 5: about 10 % at 100 virtual nodes). The hottest of M such shares sits z_M standard
// deviations above the mean, z_M the expected maximum of M standard normals (Blom:
// queueing::expected_normal_maximum):
//   k = min(1 + z_M / sqrt(N v), M / N)        (M / N: a machine holds at most every key once)
// v = 100, M = 4: 1 + 1.0491 / 10 = 1.1049;  v = 150, M = 4: 1.0857;  v = 1, M = 4: 2.0491.
inline double ring_factor(int machines, int copies_per_key, int vnodes) {
  const double spread = 1.0 / std::sqrt(static_cast<double>(copies_per_key) * vnodes);
  return std::min(1.0 + simsys::queueing::expected_normal_maximum(machines) * spread,
                  static_cast<double>(machines) / copies_per_key);
}

inline constexpr simsys::ByteRate one_gbit_per_s{125e6};  // piped's default link

// a(`rate`/s of `put`, 1 MB each) -> b (a service of 1M/s) over a 250 us link of the given
// bandwidth (1 Gbit/s = 125 MB/s unless given; none: a plain link). 3 s at 100 ms steps, the first
// 500 ms warmup.
// C++20 designated initializers below (`.name = "piped"`): each member named, in declaration order.
inline simsys::model::Scenario piped(double rate,
                                     std::optional<simsys::ByteRate> bandwidth = one_gbit_per_s) {
  using namespace std::chrono_literals;
  using simsys::model::NodeSpec;
  using simsys::model::NodeType;
  simsys::model::Scenario s{
      .name = "piped", .version = 1, .duration = 3s, .step = 100ms, .warmup = 500ms};
  s.nodes = {
      NodeSpec{.id = "a",
               .type = NodeType::client,
               .workload = simsys::model::Workload{.curve = {.base = simsys::Rate{rate}},
                                                   .op_shares = {{"put", 1.0}},
                                                   .sizes = {{"put", simsys::Bytes{1e6}}}}},
      NodeSpec{
          .id = "b", .type = NodeType::service, .capacity = simsys::Rate{1e6}, .latency = 1ms}};
  s.links = {
      simsys::model::LinkSpec{.from = "a", .to = "b", .latency = 250us, .bandwidth = bandwidth}};
  return s;
}

// `simsys explain`'s text for a scenario built in code; the test fails here, with the graph's
// message, when the scenario does not build.
inline std::string explain_of(const simsys::model::Scenario& s) {
  const auto built = simsys::graph::Graph::build(s);
  INFO((built ? "" : built.error().message));
  REQUIRE(built.has_value());
  return simsys::report::render_explain(*built);
}

}  // namespace simsys::test_support
