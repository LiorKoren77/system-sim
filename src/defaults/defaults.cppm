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
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <utility>

export module simsys.defaults;

import simsys.core;
import simsys.fluid;
import simsys.model;

export namespace simsys::defaults {

// The book's latency table, as typed durations. One field per row we use.
// `disk_seek` (spec §6: SQL writes) and `compress_1kb` (spec §6: service CPU cost unit)
// are not used yet: they feed the per-operation cost table of phase 2, which is also
// when the spec's "SQL 1k writes" figure arrives (phase 1 has one rate per node).
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
    // "Latency numbers every programmer should know" (the Dean/Norvig list the book's
    // table descends from); the SSD row is in the later versions of that list.
    .ssd_random_read = 150_us,
    .disk_seek = 10_ms,         // book: "disk seek"
    .disk_read_1mb = 30_ms,     // book: "read 1 MB sequentially from disk"
    .network_read_1mb = 10_ms,  // book: "read 1 MB sequentially from the network"
    .compress_1kb = 10_us,      // book: "compress 1 KB with Zippy"
    .intra_dc_rtt = 500_us,     // book: "round trip within the same datacenter"
    .cross_dc_rtt = 150_ms,     // book: "send packet CA -> Netherlands -> CA"
    .third_party_api = 100_ms,  // spec §6: external APIs, with variability 2.0
};

// Chosen, not from the book or spec: a blob store counts objects of 1 MB, so the spec's
// "blob 500 MB/s" means 500 objects/s, and one object costs one per-MB disk read plus
// one per-MB network transfer. A scenario's byte_capacity is converted with it too.
inline constexpr double blob_object_bytes = 1e6;

// What a node of a given type does when the scenario is silent: how many requests per
// second one replica drains, how long one request occupies it, and how irregular that
// time is (squared coefficient of variation; 1 = exponential, the M/M/1 assumption).
struct TypeDefaults {
  Rate service_rate;  // per replica
  SimTime service_time;
  double service_variability{1.0};
};

// The per-type defaults. Every number says where it comes from: "spec" is the spec §6
// table (QPS line "web 2k, cache 100k, SQL 5k reads, KV 50k, queue 1M msg/s, blob
// 500 MB/s", plus the latency rows), "book" the latency table above, and "chosen" our
// own round number for a type the spec gives no figure for.
// A per-type switch rather than an array indexed by the enum, so -Wswitch (an error
// under -Werror) flags any enumerator added to model::NodeType without a case here.
// C++11 `constexpr` function holding a `switch`: it runs at run time and in constant
// expressions, which is what lets the static_asserts at the end of the file check the
// defaults during compilation. (A constexpr body may contain a switch since C++14.)
[[nodiscard]] constexpr TypeDefaults for_type(model::NodeType type) {
  // A value cast from an integer outside the enumerator list is a programmer error
  // (the loader only produces cataloged types). Throwing in a constexpr function is
  // fine: only a constant evaluation that reaches the throw fails to compile.
  if (!model::is_cataloged(type)) throw std::invalid_argument{"unknown NodeType"};
  // C++20 `using enum`: brings every enumerator into scope, so the cases read `cdn`
  // instead of `model::NodeType::cdn`. Replaces a using-declaration per enumerator or
  // a wall of qualified names; scoped to this function, so nothing leaks.
  using enum model::NodeType;
  const auto& t = latency_numbers;
  switch (type) {
    case client:
      return {Rate{0}, 0_s};  // a source, not a queue
    case cdn:
      // rate chosen (the spec has no CDN figure); time spec §6 "CDN / blob per-MB
      // transfer": an edge serves a cached ~1 MB object over the network. The memory
      // read (250 us) is only the lookup; the transfer dominates.
      return {Rate{100'000}, t.network_read_1mb};
    case load_balancer:
      return {Rate{100'000}, 50_us};  // chosen: an L7 proxy hop
    case rate_limiter:
      return {Rate{100'000}, 50_us};  // chosen: an in-memory counter check
    case service:
      return {Rate{2'000}, 1_ms};  // rate spec "web 2k"; time chosen: ~1 ms CPU per request
    case cache:
      // rate spec "cache 100k"; time spec §6 "cache service time floor" (memory read)
      return {Rate{100'000}, t.memory_read_1mb};
    case sql_db:
      // rate spec "SQL 5k reads"; time spec §6 "SQL/NoSQL read". Writes (1k/s, disk
      // seek) wait for the phase-2 per-operation cost table.
      return {Rate{5'000}, t.ssd_random_read};
    case kv_cluster:
      return {Rate{50'000}, t.ssd_random_read};  // rate spec "KV 50k"; time spec "NoSQL read"
    case message_queue:
      return {Rate{1'000'000}, 100_us};  // rate spec "queue 1M msg/s"; time chosen: an append
    case worker_pool:
      return {Rate{1'000}, 2_ms};  // chosen: a 2 ms background job
    case blob_store:
      // rate spec "blob 500 MB/s" / 1 MB objects = 500/s; time spec §6 "blob store
      // per-MB cost" (disk) + "per-MB transfer" (network) = 30 + 10 = 40 ms. Summed via
      // count(): see resolve_link_latency for why chrono's operator+ is not visible here.
      return {Rate{500e6 / blob_object_bytes},
              SimTime{t.disk_read_1mb.count() + t.network_read_1mb.count()}};
    case search_index:
      return {Rate{5'000}, 5_ms};  // chosen: an inverted-index query
    case ws_gateway:
      return {Rate{50'000}, 100_us};  // chosen: relaying one frame
    case service_discovery:
      // rate chosen; time book "round trip within the same datacenter" (one lookup)
      return {Rate{10'000}, t.intra_dc_rtt};
    case id_generator:
      return {Rate{10'000}, 100_us};  // chosen; snowflake scenarios override it
    case external_sink:
      // rate chosen; time and variability spec §6: third-party APIs are slow and erratic
      // (variability 2.0, i.e. more irregular than exponential).
      return {Rate{1'000}, t.third_party_api, 2.0};
    case notification_fanout:
      return {Rate{10'000}, 1_ms};  // chosen
    case frontier:
      return {Rate{10'000}, 1_ms};  // chosen
  }
  // C++23 std::unreachable(): tells the compiler this point cannot be reached, which
  // silences -Wreturn-type and lets it optimise. Replaces __builtin_unreachable() or a
  // dummy fallback value that would hide a missing case. It is truly unreachable here:
  // the guard above rejects every value outside the catalogue, the catalogue is proven
  // to list the enumerators in order (model::table_follows_enum_order), and -Wswitch
  // proves every enumerator has a case that returns.
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

// Pattern: Layered configuration (override over default, like a CSS cascade);
// Resolved<T> records which layer answered.
// A blob store may state its capacity in bytes; that is still the scenario's value,
// converted to objects per second with blob_object_bytes.
[[nodiscard]] Resolved<Rate> resolve_service_rate(const model::NodeSpec& n) {
  if (n.capacity) return {*n.capacity, Source::scenario};
  if (n.byte_capacity) {
    return {Rate{n.byte_capacity->bytes_per_second / blob_object_bytes}, Source::scenario};
  }
  return {for_type(n.type).service_rate, Source::type_default};
}

[[nodiscard]] Resolved<SimTime> resolve_service_time(const model::NodeSpec& n) {
  if (n.latency) return {*n.latency, Source::scenario};
  return {for_type(n.type).service_time, Source::type_default};
}

// The number of servers c that makes a node one consistent machine. Little's law
// (L = lambda * W) applied to the servers at full load: the requests in service equal
// throughput times time in service, so c = service_rate * service_time. Rounded up
// (a fractional server still needs a whole one) and at least 1. Assumes the rate and
// time describe the same requests; a scenario that disagrees sets advanced.concurrency.
// Without it the fluid drain (which uses the rate) and the Erlang C term (which uses
// c / time) would model two different machines; see fluid::Capacity.
[[nodiscard]] int implied_concurrency(Rate service_rate, SimTime service_time) {
  const double busy_servers = service_rate.per_second * seconds(service_time);
  return std::max(1, static_cast<int>(std::ceil(busy_servers)));
}

// Fold scenario values over defaults into the engine's Capacity. Every fallback is
// fluid::Capacity's own default member initializer (queue 1000 and c = 1 are chosen;
// yellow 0.70 / red 0.90 are the spec §3.3 zones, the ops rule of thumb that the M/M/1 wait,
// growing like 1/(1-rho), climbs steeply past ~70% and turns dangerous past ~90%), so
// the numbers live in exactly one place. Only concurrency is derived instead, and the
// variability comes from the type (external sinks are erratic).
// A client is a source, not a queue: asking for its capacity is a programmer error.
[[nodiscard]] fluid::Capacity resolve_capacity(const model::NodeSpec& n) {
  if (n.type == model::NodeType::client) {
    throw std::invalid_argument{"resolve_capacity: a client is a source, not a queue"};
  }
  constexpr fluid::Capacity base{};
  const auto& a = n.advanced;
  const Rate rate = resolve_service_rate(n).value;
  const SimTime time = resolve_service_time(n).value;
  return fluid::Capacity{
      .service_rate = rate,
      .service_time_s = seconds(time),
      .concurrency = a.concurrency.value_or(implied_concurrency(rate, time)),
      .queue_limit = a.queue_limit.value_or(base.queue_limit),
      .yellow = a.yellow.value_or(base.yellow),
      .red = a.red.value_or(base.red),
      .service_variability = a.service_variability.value_or(for_type(n.type).service_variability),
      .burstiness = a.burstiness.value_or(base.burstiness),
  };
}

// One-way link latency: the scenario value, else half the datacenter round trip (the
// book's table quotes round trips; a one-way hop is half of one, assuming a symmetric path).
[[nodiscard]] Resolved<SimTime> resolve_link_latency(const model::LinkSpec& l, bool cross_dc) {
  if (l.latency) return {*l.latency, Source::scenario};
  const auto rtt = cross_dc ? latency_numbers.cross_dc_rtt : latency_numbers.intra_dc_rtt;
  // Halve via count(): chrono's arithmetic operators are non-member templates declared in
  // <chrono>'s global-module-fragment include, which this interface must not repeat (see
  // top of file) and which an import never brings into scope; the member count() and the
  // SimTime constructor are visible. Integer division truncates an odd nanosecond count,
  // which is exact for both table values (500 us, 150 ms).
  return {SimTime{rtt.count() / 2}, Source::dc_default};
}

}  // namespace simsys::defaults

namespace {

// Teaching: a constexpr loop over the whole enum range. Because for_type is constexpr,
// the whole check runs in the compiler; a node type with a forgotten (zero) rate or
// service time fails the build instead of a test run.
constexpr bool every_non_client_type_has_positive_defaults() {
  for (std::size_t i = 0; i < simsys::model::node_type_names.size(); ++i) {
    const auto type = static_cast<simsys::model::NodeType>(i);
    if (type == simsys::model::NodeType::client) continue;
    const auto d = simsys::defaults::for_type(type);
    if (d.service_rate.per_second <= 0 || d.service_time.count() <= 0) return false;
  }
  return true;
}

}  // namespace

static_assert(
    simsys::defaults::for_type(simsys::model::NodeType::service).service_rate.per_second == 2000);
static_assert(every_non_client_type_has_positive_defaults());
