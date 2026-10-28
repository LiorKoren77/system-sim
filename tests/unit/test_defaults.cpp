// The back-of-the-envelope table (book ch. 2) and how a node's numbers are resolved:
// scenario values first, type defaults second, with provenance for `simsys explain`.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

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
  // C++20 designated initializers: `.id = ...` names each member being set, in declaration
  // order, and leaves the rest at their defaults. Replaces positional braces, which break
  // silently when members are reordered, and setting field after field on a default object.
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
  // No advanced.concurrency: implied by Little's law, 2000/s x 1 ms = 2 busy servers.
  REQUIRE(c.concurrency == 2);
  REQUIRE(c.queue_limit == 50);
  REQUIRE(c.burstiness == 2.0);
  REQUIRE(c.service_variability == 1.0);
  REQUIRE(c.yellow == 0.70);
  REQUIRE(c.red == 0.90);
}

TEST_CASE("every advanced key overrides its fluid default", "[defaults]") {
  NodeSpec n{.id = "api", .type = NodeType::service};
  n.advanced = {.queue_limit = 50,
                .concurrency = 8,
                .service_variability = 1.5,
                .burstiness = 2.0,
                .yellow = 0.6,
                .red = 0.8};
  const auto c = simsys::defaults::resolve_capacity(n);
  REQUIRE(c.queue_limit == 50);
  REQUIRE(c.concurrency == 8);
  REQUIRE(c.service_variability == 1.5);
  REQUIRE(c.burstiness == 2.0);
  REQUIRE(c.yellow == 0.6);
  REQUIRE(c.red == 0.8);
}

TEST_CASE("a defaulted node is one consistent machine: concurrency = service rate x service time",
          "[defaults]") {
  for (const auto name : simsys::model::node_type_names) {
    const auto type = *simsys::model::parse_node_type(name);
    if (type == NodeType::client) continue;
    CAPTURE(name);
    const auto c = simsys::defaults::resolve_capacity(NodeSpec{.id = "n", .type = type});
    // Little's law at full load: busy servers = throughput x time in service.
    REQUIRE(c.concurrency ==
            std::max(1, static_cast<int>(std::ceil(c.service_rate.per_second * c.service_time_s))));
  }
}

// Erlang C costs O(c) per node per step, so c is capped: a node of 100M/s with 1 h service
// time would otherwise imply 3.6e11 servers, a number no int holds (casting it is UB).
TEST_CASE("implied concurrency is capped at max_concurrency before it becomes an int",
          "[defaults]") {
  STATIC_REQUIRE(simsys::defaults::max_concurrency == 100'000);
  REQUIRE(simsys::defaults::implied_concurrency(Rate{100e6}, 1h) == 100'000);
  const auto c = simsys::defaults::resolve_capacity(
      NodeSpec{.id = "n", .type = NodeType::service, .capacity = Rate{100e6}, .latency = 1h});
  REQUIRE(c.concurrency == simsys::defaults::max_concurrency);
  REQUIRE(simsys::defaults::implied_concurrency(Rate{1000}, 1ms) == 1);  // below the cap: unchanged
}

TEST_CASE("external sinks default to the third-party API latency with variability 2",
          "[defaults]") {
  const auto c =
      simsys::defaults::resolve_capacity(NodeSpec{.id = "s", .type = NodeType::external_sink});
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

TEST_CASE("a cross-datacenter link defaults to half the cross-DC round trip, from the DC default",
          "[defaults]") {
  const auto x =
      simsys::defaults::resolve_link_latency(simsys::model::LinkSpec{.from = "a", .to = "b"}, true);
  REQUIRE(x.value == 75ms);
  REQUIRE(x.source == Source::dc_default);
}

// Plan 2c refinement 2: a byte-sized node's capacity unit is one megabyte (defaults::byte_unit),
// so a stated byte rate is that many MB per second whatever the type, and it is still the
// scenario's own number: 200 MB/s = 200e6 B/s / 1e6 B per unit = 200 units/s.
TEST_CASE("a byte capacity resolves to megabytes per second and counts as a scenario value",
          "[defaults]") {
  NodeSpec n{.id = "blobs", .type = NodeType::blob_store};
  n.byte_capacity = simsys::ByteRate{200e6};
  const auto r = simsys::defaults::resolve_service_rate(n);
  REQUIRE(r.source == Source::scenario);
  REQUIRE(r.value.per_second == Approx(200.0));
}

// Spec section 6: "blob 500 MB/s" is 500 units/s of 1 MB, and its per-MB service time is the
// disk read (30 ms) plus the network transfer (10 ms) of the book's latency table: 40 ms per MB.
// Little's law (c = units/s x s) then gives c = 500 x 0.04 = 20 transfers in flight at once.
TEST_CASE("a blob store's default capacity is 500 MB per second at 40 ms per MB", "[defaults]") {
  const NodeSpec store{.id = "store", .type = NodeType::blob_store};
  REQUIRE(simsys::defaults::resolve_service_rate(store).value.per_second == Approx(500.0));
  REQUIRE(simsys::defaults::resolve_service_rate(store).source == Source::type_default);
  REQUIRE(simsys::defaults::resolve_service_time(store).value == 40ms);
  REQUIRE(simsys::defaults::resolve_capacity(store).concurrency == 20);
}

// A stated 5GB/s on a blob store (youtube's original_store) is 5000 MB/s at the same 40 ms per
// MB: c = 5000 x 0.04 = 200. A stated 100GB/s on a CDN is 100k MB/s at the CDN's 10 ms per MB
// (book: "read 1 MB sequentially from the network"): c = 100000 x 0.01 = 1000.
TEST_CASE("a stated byte capacity keeps the type's per-megabyte service time", "[defaults]") {
  NodeSpec store{.id = "original_store", .type = NodeType::blob_store};
  store.byte_capacity = simsys::ByteRate{5e9};
  const auto stored = simsys::defaults::resolve_capacity(store);
  REQUIRE(stored.service_rate.per_second == Approx(5000.0));
  REQUIRE(stored.service_time_s == Approx(0.040));
  REQUIRE(stored.concurrency == 200);

  NodeSpec cdn{.id = "cdn_us", .type = NodeType::cdn};
  cdn.byte_capacity = simsys::ByteRate{100e9};
  const auto edge = simsys::defaults::resolve_capacity(cdn);
  REQUIRE(edge.service_rate.per_second == Approx(100'000.0));
  REQUIRE(edge.service_time_s == Approx(0.010));
  REQUIRE(edge.concurrency == 1000);
}

// Plan 2c refinement 2: a node is byte-sized when its capacity is a byte rate, and then counts
// megabytes. Three ways to get there: a stated `capacity: 100GB/s` (byte_capacity, on a CDN or
// a blob store), or a blob store's type default, spec section 6's 500 MB/s. A capacity stated
// in requests (`capacity: 5k/s`) is a request count, whatever the type; so is a type whose
// default is a request rate (a CDN without a capacity, every service).
TEST_CASE("a blob store with no stated capacity is byte-sized by its type's default",
          "[defaults]") {
  REQUIRE(simsys::defaults::is_byte_sized(NodeSpec{.id = "blobs", .type = NodeType::blob_store}));
}

TEST_CASE("a cdn is byte-sized when its capacity is a byte rate", "[defaults]") {
  NodeSpec cdn{.id = "cdn", .type = NodeType::cdn};
  cdn.byte_capacity = simsys::ByteRate{100e9};
  REQUIRE(simsys::defaults::is_byte_sized(cdn));
}

TEST_CASE("a node whose capacity counts requests is not byte-sized", "[defaults]") {
  SECTION("a cdn with a request capacity") {
    REQUIRE_FALSE(simsys::defaults::is_byte_sized(
        NodeSpec{.id = "cdn", .type = NodeType::cdn, .capacity = Rate{100'000}}));
  }
  SECTION("a cdn with no capacity: its type default is a request rate") {
    REQUIRE_FALSE(simsys::defaults::is_byte_sized(NodeSpec{.id = "cdn", .type = NodeType::cdn}));
  }
  SECTION("a blob store whose capacity is stated in requests") {
    REQUIRE_FALSE(simsys::defaults::is_byte_sized(
        NodeSpec{.id = "blobs", .type = NodeType::blob_store, .capacity = Rate{500}}));
  }
  SECTION("a service") {
    REQUIRE_FALSE(
        simsys::defaults::is_byte_sized(NodeSpec{.id = "api", .type = NodeType::service}));
  }
}

// A byte-sized node moves a fixed number of megabytes at a fixed rate per slot, so one object's
// service time is deterministic: its squared coefficient of variation defaults to 0 (review of
// plan 2c Task 3's fix round, I2), and the spread of service times at the node is its size mix's
// alone. advanced.service_variability still overrides it. A blob store sized in requests keeps
// the type's exponential service (1).
TEST_CASE("a byte-sized node's transfers are deterministic: service variability 0 unless stated",
          "[defaults]") {
  REQUIRE(simsys::defaults::resolve_capacity(NodeSpec{.id = "blobs", .type = NodeType::blob_store})
              .service_variability == 0.0);
  NodeSpec cdn{.id = "cdn", .type = NodeType::cdn};
  cdn.byte_capacity = simsys::ByteRate{100e9};
  REQUIRE(simsys::defaults::resolve_capacity(cdn).service_variability == 0.0);
  REQUIRE(simsys::defaults::resolve_capacity(
              NodeSpec{.id = "blobs", .type = NodeType::blob_store, .capacity = Rate{500}})
              .service_variability == 1.0);
  REQUIRE(simsys::defaults::resolve_capacity(NodeSpec{.id = "blobs",
                                                      .type = NodeType::blob_store,
                                                      .advanced = {.service_variability = 1.0}})
              .service_variability == 1.0);
}

// The unit of a byte-sized node's capacity is one megabyte (decimal, as every size in the
// project): 500 MB/s is 500 units/s, and a 50 MB stream costs 50 units.
TEST_CASE("a byte-sized node counts megabytes: the byte unit is 1e6 bytes", "[defaults]") {
  STATIC_REQUIRE(simsys::defaults::byte_unit == 1e6);
}

TEST_CASE("an out-of-range node type is a programmer error", "[defaults]") {
  const auto bogus = static_cast<NodeType>(200);
  REQUIRE_THROWS_AS(simsys::defaults::for_type(bogus), std::invalid_argument);
  REQUIRE_THROWS_AS(simsys::model::to_string(bogus), std::invalid_argument);
}
