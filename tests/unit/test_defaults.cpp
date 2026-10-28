// The back-of-the-envelope table (book ch. 2) and how a node's numbers are resolved:
// scenario values first, type defaults second, with provenance for `simsys explain`.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.core;
import simsys.defaults;
import simsys.fluid;
import simsys.model;

using namespace std::chrono_literals;
using Catch::Approx;
using simsys::Rate;
using simsys::defaults::Source;
using simsys::model::NodeSpec;
using simsys::model::NodeType;

TEST_CASE("the latency numbers table holds the book's values", "[defaults]") {
  constexpr auto& t = simsys::defaults::latency_numbers;
  static_assert(t.ssd_random_read == 150us);
  static_assert(t.disk_seek == 10ms);
  static_assert(t.intra_dc_rtt == 500us);
  static_assert(t.cross_dc_rtt == 150ms);
  REQUIRE(t.third_party_api == 100ms);
}

TEST_CASE("every non-client type has a positive default service rate and time", "[defaults]") {
  for (const auto name : simsys::model::node_type_names) {
    const auto type = *simsys::model::parse_node_type(name);
    if (type == NodeType::client) continue;
    const auto d = simsys::defaults::for_type(type);
    CAPTURE(name);
    REQUIRE(d.service_rate.per_second > 0);
    REQUIRE(d.service_time > 0ns);
  }
}

TEST_CASE("the web tier default is 2k/s with 1 ms service (spec table)", "[defaults]") {
  const auto d = simsys::defaults::for_type(NodeType::service);
  REQUIRE(d.service_rate == Rate{2000});
  REQUIRE(d.service_time == 1ms);
}

TEST_CASE("a scenario capacity wins over the type default and says so", "[defaults]") {
  const NodeSpec n{.id = "db", .type = NodeType::sql_db, .capacity = Rate{8000}};
  const auto r = simsys::defaults::resolve_service_rate(n);
  REQUIRE(r.value == Rate{8000});
  REQUIRE(r.source == Source::scenario);
  const auto t = simsys::defaults::resolve_service_time(n);
  REQUIRE(t.value == 150us);  // ssd_random_read
  REQUIRE(t.source == Source::type_default);
}

TEST_CASE("resolve_capacity folds advanced keys over defaults", "[defaults]") {
  NodeSpec n{.id = "web", .type = NodeType::service};
  n.advanced.queue_limit = 50;
  n.advanced.burstiness = 2.0;
  const auto c = simsys::defaults::resolve_capacity(n);
  REQUIRE(c.service_rate == Rate{2000});
  REQUIRE(c.service_time_s == Approx(0.001));
  REQUIRE(c.concurrency == 1);
  REQUIRE(c.queue_limit == 50);
  REQUIRE(c.burstiness == 2.0);
  REQUIRE(c.service_variability == 1.0);
  REQUIRE(c.yellow == 0.70);
  REQUIRE(c.red == 0.90);
}

TEST_CASE("external sinks default to the third-party API latency with variability 2", "[defaults]") {
  const auto c = simsys::defaults::resolve_capacity(NodeSpec{.id = "s", .type = NodeType::external_sink});
  REQUIRE(c.service_time_s == Approx(0.1));
  REQUIRE(c.service_variability == 2.0);
}

TEST_CASE("link latency defaults to half the datacenter round trip, one way", "[defaults]") {
  const simsys::model::LinkSpec inside{.from = "a", .to = "b"};
  const auto l = simsys::defaults::resolve_link_latency(inside, /*cross_dc=*/false);
  REQUIRE(l.value == 250us);
  REQUIRE(l.source == Source::dc_default);
  const auto x = simsys::defaults::resolve_link_latency(inside, /*cross_dc=*/true);
  REQUIRE(x.value == 75ms);
  const simsys::model::LinkSpec given{.from = "a", .to = "b", .latency = 3ms};
  REQUIRE(simsys::defaults::resolve_link_latency(given, true).value == 3ms);
  REQUIRE(simsys::defaults::resolve_link_latency(given, true).source == Source::scenario);
}
