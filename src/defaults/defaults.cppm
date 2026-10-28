// Module `simsys.defaults`: the back-of-the-envelope numbers (System Design Interview,
// Alex Xu, vol. 1, ch. 2, "latency numbers every programmer should know") and the
// per-node-type defaults the spec (§6) derives from them. The book's table is what lets
// you estimate a design on a napkin; here it is also what a node falls back to when the
// scenario does not say. Every resolved number carries its Source so `simsys explain`
// can print where it came from. Ignored on purpose: the numbers are round, 2010-era
// orders of magnitude, not a benchmark of any real machine.
//
// C++20 module interface unit (`export module`): see src/core/core.cppm for the longer
// explanation of modules versus headers.
module;
// NO <chrono>/<format>/<print>/<filesystem> here: this interface imports other modules
// (GCC 14.2 'Bad file data' bug, CLAUDE.md §3). Durations use simsys::literals (250_us).
#include <optional>
#include <utility>

export module simsys.defaults;

import simsys.core;
import simsys.fluid;
import simsys.model;

export namespace simsys::defaults {

// The book's latency table, as typed durations. One field per row we use.
struct LatencyNumbers {
  SimTime memory_read_1mb, ssd_random_read, disk_seek, disk_read_1mb, network_read_1mb,
      compress_1kb, intra_dc_rtt, cross_dc_rtt, third_party_api;
};

// C++20 designated initializers on a constexpr constant: the table reads like the book's
// table (each value is named) and a misordered field is a compile error, not a silently
// wrong latency. Replaces positional brace initialization, where swapping two values
// of the same type compiles fine.
inline constexpr LatencyNumbers latency_numbers{
    .memory_read_1mb = 250_us,  // book: "read 1 MB sequentially from memory"
    .ssd_random_read = 150_us,  // book: "SSD random read"
    .disk_seek = 10_ms,         // book: "disk seek"
    .disk_read_1mb = 30_ms,     // book: "read 1 MB sequentially from disk"
    .network_read_1mb = 10_ms,  // book: "read 1 MB over 1 Gbps network"
    .compress_1kb = 10_us,      // book: "compress 1 KB with Zippy"
    .intra_dc_rtt = 500_us,     // book: "round trip within the same datacenter"
    .cross_dc_rtt = 150_ms,     // book: "send packet CA -> Netherlands -> CA"
    .third_party_api = 100_ms,  // spec §6: external APIs, with variability 2.0
};

// What a node of a given type does when the scenario is silent: how many requests per
// second one replica drains, how long one request occupies it, and how irregular that
// time is (squared coefficient of variation; 1 = exponential, the M/M/1 assumption).
struct TypeDefaults {
  Rate service_rate;  // per replica
  SimTime service_time;
  double service_variability{1.0};
};

// Pattern: Table-driven method. The QPS figures are the spec §6 line "web 2k, cache 100k,
// SQL 5k, KV 50k, queue 1M" (the orders of magnitude the book's ch. 1-2 estimates use);
// the rest are our own round numbers, marked "chosen".
// C++11 `constexpr` function holding a `switch`: it runs at run time and in constant
// expressions, which is what lets the static_asserts at the end of the file check the
// table during compilation. (A constexpr body may contain a switch since C++14.)
[[nodiscard]] constexpr TypeDefaults for_type(model::NodeType type) {
  // C++20 `using enum`: brings every enumerator into scope, so the cases read `cdn`
  // instead of `model::NodeType::cdn`. Replaces a using-declaration per enumerator or
  // a wall of qualified names; scoped to this function, so nothing leaks.
  using enum model::NodeType;
  switch (type) {
    case client:
      return {Rate{0}, 0_s};  // a source, not a queue
    case cdn:
      return {Rate{100'000}, latency_numbers.memory_read_1mb};  // edge serves from memory
    case load_balancer:
      return {Rate{100'000}, 50_us};  // chosen: L7 proxy hop
    case rate_limiter:
      return {Rate{100'000}, 50_us};  // chosen: in-memory counter
    case service:
      return {Rate{2'000}, 1_ms};  // spec: web 2k; chosen: ~1 ms CPU per request
    case cache:
      return {Rate{100'000}, latency_numbers.memory_read_1mb};  // spec: cache 100k
    case sql_db:
      return {Rate{5'000}, latency_numbers.ssd_random_read};  // spec: SQL 5k reads; SSD read
    case kv_cluster:
      return {Rate{50'000}, latency_numbers.ssd_random_read};  // spec: KV 50k
    case message_queue:
      return {Rate{1'000'000}, 100_us};  // spec: queue 1M; chosen: append
    case worker_pool:
      return {Rate{1'000}, 2_ms};  // chosen: 2 ms job
    case blob_store:
      // spec: 500 MB/s; phase 1 counts ~0.5 MB objects
      return {Rate{1'000}, latency_numbers.disk_read_1mb};
    case search_index:
      return {Rate{5'000}, 5_ms};  // chosen
    case ws_gateway:
      return {Rate{50'000}, 100_us};  // chosen
    case service_discovery:
      return {Rate{10'000}, latency_numbers.intra_dc_rtt};  // one lookup round trip
    case id_generator:
      return {Rate{10'000}, 100_us};  // chosen; snowflake overrides in scenarios
    case external_sink:
      // Third-party APIs are slow and erratic: spec §6 gives variability 2.0 (> exponential).
      return {Rate{1'000}, latency_numbers.third_party_api, 2.0};
    case notification_fanout:
      return {Rate{10'000}, 1_ms};  // chosen
    case frontier:
      return {Rate{10'000}, 1_ms};  // chosen
  }
  // C++23 std::unreachable(): tells the compiler this point cannot be reached, which
  // silences -Wreturn-type and lets it optimise. Replaces __builtin_unreachable() or a
  // dummy fallback value that would hide a missing case; -Wswitch flags a new enumerator.
  std::unreachable();
}

// Where a resolved number came from, for `simsys explain`.
enum class Source { scenario, type_default, dc_default };

// A resolved number and its provenance.
template <class T>
struct Resolved {
  T value;
  Source source;
};

// Pattern: Chain of responsibility (two links): the scenario's value if present, else
// the type default. The first link that has an answer wins and says so.
[[nodiscard]] Resolved<Rate> resolve_service_rate(const model::NodeSpec& n) {
  if (n.capacity) return {*n.capacity, Source::scenario};
  return {for_type(n.type).service_rate, Source::type_default};
}

[[nodiscard]] Resolved<SimTime> resolve_service_time(const model::NodeSpec& n) {
  if (n.latency) return {*n.latency, Source::scenario};
  return {for_type(n.type).service_time, Source::type_default};
}

// Fold scenario values over type defaults into the engine's Capacity. The fluid
// defaults (queue 1000, yellow 0.70, red 0.90, c = 1) are spec §3.3/§4: the ~70% / ~90%
// utilization marks are the operations rule of thumb that M/M/1 waiting time (which
// grows like 1/(1-rho)) starts to climb steeply and then turns dangerous.
[[nodiscard]] fluid::Capacity resolve_capacity(const model::NodeSpec& n) {
  const auto d = for_type(n.type);
  const auto& a = n.advanced;
  return fluid::Capacity{
      .service_rate = resolve_service_rate(n).value,
      .service_time_s = seconds(resolve_service_time(n).value),
      .concurrency = a.concurrency.value_or(1),
      .queue_limit = a.queue_limit.value_or(1000.0),
      .yellow = a.yellow.value_or(0.70),
      .red = a.red.value_or(0.90),
      .service_variability = a.service_variability.value_or(d.service_variability),
      .burstiness = a.burstiness.value_or(1.0),
  };
}

// One-way link latency: the scenario value, else half the datacenter round trip (the
// book's table quotes round trips; a one-way hop is half of one, assuming a symmetric path).
[[nodiscard]] Resolved<SimTime> resolve_link_latency(const model::LinkSpec& l, bool cross_dc) {
  if (l.latency) return {*l.latency, Source::scenario};
  const auto rtt = cross_dc ? latency_numbers.cross_dc_rtt : latency_numbers.intra_dc_rtt;
  // Halve via count(): chrono's arithmetic operators are non-member templates from
  // <chrono>, which this interface must not include (see top of file), so they are not
  // visible here; the member count() and the SimTime constructor are.
  return {SimTime{rtt.count() / 2}, Source::dc_default};
}

}  // namespace simsys::defaults

namespace {

// Teaching: a constexpr loop over the enum's name table. Because for_type is constexpr,
// the whole check runs in the compiler; a new node type with a forgotten (zero) default
// fails the build instead of a test run.
constexpr bool every_non_client_type_has_positive_service_time() {
  for (const auto name : simsys::model::node_type_names) {
    const auto type = *simsys::model::parse_node_type(name);
    if (type == simsys::model::NodeType::client) continue;
    if (simsys::defaults::for_type(type).service_time.count() <= 0) return false;
  }
  return true;
}

}  // namespace

static_assert(
    simsys::defaults::for_type(simsys::model::NodeType::service).service_rate.per_second == 2000);
static_assert(every_non_client_type_has_positive_service_time());
