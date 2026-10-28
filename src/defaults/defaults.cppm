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
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

export module simsys.defaults;

import simsys.core;
import simsys.fluid;
import simsys.model;

export namespace simsys::defaults {

// The book's latency table, as typed durations. One field per row we use.
// `disk_seek` (spec §6: SQL writes) and `compress_1kb` (spec §6: service CPU cost unit)
// are not used yet: a cost is a share of a node's capacity, not a longer service time (see
// sql_write_cost, which prices a SQL write from the spec's "1k writes" figure instead).
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

// The capacity unit of a byte-sized node (is_byte_sized below), plan 2c refinement 2: one
// megabyte, decimal like every size in the project, so 500 MB/s is 500 units/s and a 50 MB
// stream costs 50 units (byte_cost). Chosen because the book's latency table prices transfers
// per MB ("read 1 MB sequentially from the network": 10 ms), which makes one unit's service
// time a row of that table: a byte-sized node's service time is the time of ONE megabyte, and
// Little's law gives its concurrency as for any node, c = units/s × s (a 100 GB/s CDN at 10 ms
// per MB: 100k/s × 10 ms = 1000 transfers at once). The size is each op's own, resolved by the
// graph (graph::Graph::request_bytes), never an assumed object.
inline constexpr double byte_unit = 1e6;

// Capacity units one request of `size` bytes uses at a byte-sized node: its megabytes, a 50 MB
// stream 50, a 2 KB read 0.002. A cost is capacity, not duration (as a SQL write's 5 units are,
// sql_write_cost): the request still waits and is served like one unit, so its sojourn is its
// wait plus ONE megabyte's service time, its time to first byte.
[[nodiscard]] constexpr double byte_cost(Bytes size) {
  return size.count / byte_unit;
}

// Spec §6's QPS line: "blob 500 MB/s", one blob store machine's throughput.
inline constexpr ByteRate blob_store_byte_rate{500e6};

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
      // read (250 us) is only the lookup; the transfer dominates. A CDN sized in bytes
      // (`capacity: 100GB/s`, is_byte_sized) keeps the time as the time of one MB, its unit.
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
      // rate spec "SQL 5k reads"; time spec §6 "SQL/NoSQL read". Writes (spec "1k
      // writes") cost more of this rate: sql_write_cost.
      return {Rate{5'000}, t.ssd_random_read};
    case kv_cluster:
      return {Rate{50'000}, t.ssd_random_read};  // rate spec "KV 50k"; time spec "NoSQL read"
    case message_queue:
      return {Rate{1'000'000}, 100_us};  // rate spec "queue 1M msg/s"; time chosen: an append
    case worker_pool:
      return {Rate{1'000}, 2_ms};  // chosen: a 2 ms background job
    case blob_store:
      // rate spec "blob 500 MB/s" in units of 1 MB (byte_unit) = 500 units/s; time spec §6
      // "blob store per-MB cost" (disk) + "per-MB transfer" (network) = 30 + 10 = 40 ms per
      // MB, so c = 500/s × 40 ms = 20 transfers at once (Little's law). Summed via count():
      // see resolve_link_latency for why chrono's operator+ is not visible here.
      return {Rate{blob_store_byte_rate.bytes_per_second / byte_unit},
              SimTime{t.disk_read_1mb.count() + t.network_read_1mb.count()}};
    case search_index:
      return {Rate{5'000}, 5_ms};  // chosen: an inverted-index query
    case ws_gateway:
      return {Rate{50'000}, 100_us};  // chosen: relaying one frame
    case service_discovery:
      // rate chosen; time book "round trip within the same datacenter" (one lookup)
      return {Rate{10'000}, t.intra_dc_rtt};
    case id_generator:
      // chosen, and never what a generator runs on: its `algorithm` (required by the loader
      // and Graph::build) supplies the numbers instead, for_id_algorithm below.
      return {Rate{10'000}, 100_us};
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
  // to list the enumerators in order (core::follows_enum_order), and -Wswitch
  // proves every enumerator has a case that returns.
  std::unreachable();
}

// The rate one SQL machine sustains when every query is a write: spec §6's QPS line, "SQL 5k
// reads" and "1k writes" per second (a write pays a disk seek and a log flush a read does not).
inline constexpr Rate sql_write_rate{1'000};

// What a write costs a SQL database, in capacity units (plan 2a refinement 4: `capacity` is a
// rate of units, a read costs 1): the machine that does 5000 reads/s does 1000 writes/s, so one
// write takes the capacity of 5000 / 1000 = 5 reads. Charged by op_cost to every write op
// (model::write_ops_of) the scenario gives no `cost` for, on the primary and on each replica
// that replays it. Ignored: a write's longer service time (a cost uses more capacity, not a
// longer sojourn) and the gap between an append-only log write and a random update.
inline constexpr double sql_write_cost =
    for_type(model::NodeType::sql_db).service_rate.per_second / sql_write_rate.per_second;

// Capacity units one request of `op` uses at the node `spec` describes: its `cost` entry, else
// sql_write_cost for a database's write op, else 1. The one place the rule lives, so the engine
// (through the behaviours) and explain's paper load price every op alike.
[[nodiscard]] double op_cost(const model::NodeSpec& spec, std::string_view op) {
  const auto stated = spec.cost.find(std::string{op});
  if (stated != spec.cost.end()) return stated->second;
  // C++23 std::ranges::contains: "is this value in the range?" in one call, replacing
  // `std::find(begin, end, value) != end`.
  const bool database_write =
      spec.type == model::NodeType::sql_db && std::ranges::contains(model::write_ops_of(spec), op);
  return database_write ? sql_write_cost : 1.0;
}

// ---- Id generators (plan 2b refinement 6; book ch. 7) ----

// Snowflake's sequence field: 12 bits, the ids one machine may issue within one millisecond
// (book ch. 7, "Twitter snowflake approach": "it can support a maximum of 4096 new IDs per
// millisecond"). The next millisecond's timestamp resets it.
inline constexpr int snowflake_sequence_bits = 12;

// What one replica of an id generator does when the scenario states no capacity or latency,
// per algorithm (model::IdAlgorithm says what each design is):
//  - snowflake: 2^12 ids per millisecond × 1000 ms = 4.096M ids/s per machine, the ceiling the
//    id layout sets by design; 10 us chosen: a clock read, a counter increment and a few
//    shifts, the same order as the book's "compress 1 KB" row. With both numbers, Little's law
//    gives the implied concurrency c = ceil(4.096M/s × 10 us) = ceil(40.96) = 41 ids in service
//    at once, where one sequence counter issues them one after another: one id per 244 ns
//    (1 / 4.096M/s) would give c = 1. Either way the service time is negligible beside the
//    round trip to the generator (hundreds of us), so the model keeps the round 10 us.
//  - ticket_server: one auto-increment database, and handing out an id IS a write (REPLACE
//    INTO + LAST_INSERT_ID in Flickr's design): spec §6's "SQL 1k writes" per second,
//    sql_write_rate; 1 ms chosen: a single-row commit on one machine.
//  - uuid: generated locally from random bits, with nothing shared to wait for, so nothing
//    bounds it in practice: 1e9/s chosen as "unbounded" (any finite number the fluid queue can
//    divide by; a run never comes near it), and 1 us chosen: drawing 122 random bits.
// Variability stays 1 (exponential, as for every type but external sinks): the sugar is a
// service with these two numbers, nothing more.
[[nodiscard]] constexpr TypeDefaults for_id_algorithm(model::IdAlgorithm algorithm) {
  using enum model::IdAlgorithm;  // C++20, see for_type
  switch (algorithm) {
    case snowflake:
      return {Rate{(1 << snowflake_sequence_bits) * 1000.0}, 10_us};
    case ticket_server:
      return {sql_write_rate, 1_ms};  // spec §6 "SQL 1k writes"
    case uuid:
      return {Rate{1e9}, 1_us};
  }
  std::unreachable();  // C++23, see for_type: -Wswitch proves every enumerator returns
}

// The defaults a node falls back to: its type's (for_type), or, for an id generator, its
// algorithm's.
// An id generator is a ServiceBehaviour with chapter-7 defaults, no new behaviour class: the only
// thing its algorithm changes is the two numbers returned here, which resolve_service_rate and
// resolve_service_time (and so resolve_capacity) read like any type default. A scenario's
// `capacity` or `latency` still wins over them.
[[nodiscard]] constexpr TypeDefaults defaults_of(const model::NodeSpec& n) {
  if (n.type == model::NodeType::id_generator && n.id_algorithm) {
    return for_id_algorithm(*n.id_algorithm);
  }
  return for_type(n.type);
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
// A capacity stated in bytes is still the scenario's value, read in megabytes per second
// (byte_unit): `capacity: 100GB/s` is 100k units/s.
[[nodiscard]] Resolved<Rate> resolve_service_rate(const model::NodeSpec& n) {
  if (n.capacity) return {*n.capacity, Source::scenario};
  if (n.byte_capacity) {
    return {Rate{n.byte_capacity->bytes_per_second / byte_unit}, Source::scenario};
  }
  return {defaults_of(n).service_rate, Source::type_default};
}

// Does the node count bytes rather than requests (plan 2c refinement 2)? Exactly when its
// capacity is a byte rate on a type that serves bytes: a stated `capacity: 100GB/s` on a CDN or
// a blob store, or a blob store's type default, spec §6's 500 MB/s. A capacity stated in
// requests is a request count on any type, and so is every type whose default is a request
// rate. Such a node counts megabytes (byte_unit) and needs the size of every request it serves
// (graph::Graph::request_bytes).
[[nodiscard]] bool is_byte_sized(const model::NodeSpec& n) {
  if (n.capacity) return false;
  if (n.type == model::NodeType::blob_store) return true;
  return n.type == model::NodeType::cdn && n.byte_capacity.has_value();
}

[[nodiscard]] Resolved<SimTime> resolve_service_time(const model::NodeSpec& n) {
  if (n.latency) return {*n.latency, Source::scenario};
  return {defaults_of(n).service_time, Source::type_default};
}

// The most servers c one replica may have, implied or written as advanced.concurrency
// (the loader rejects more). Chosen, not from the book: the Erlang C term sums c terms, so
// it costs O(c) per node per step, and 100k servers per replica already models any single
// tier of the book's designs; more is a sign the rate and latency describe different things.
inline constexpr int max_concurrency = 100'000;

// The number of servers c that makes a node one consistent machine. Little's law
// (L = lambda * W) applied to the servers at full load: the requests in service equal
// throughput times time in service, so c = service_rate * service_time. Rounded up
// (a fractional server still needs a whole one) and at least 1. Assumes the rate and
// time describe the same requests; a scenario that disagrees sets advanced.concurrency.
// Without it the fluid drain (which uses the rate) and the Erlang C term (which uses
// c / time) would model two different machines; see fluid::Capacity.
//
// Capped at max_concurrency, and the cap is applied to the double BEFORE the int cast:
// casting a double that no int can hold (100M/s x 1 h = 3.6e11 servers) is undefined
// behaviour, not a large number.
[[nodiscard]] int implied_concurrency(Rate service_rate, SimTime service_time) {
  const double busy_servers = service_rate.per_second * seconds(service_time);
  const double servers = std::clamp(std::ceil(busy_servers), 1.0, double{max_concurrency});
  return static_cast<int>(servers);
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
      .service_variability = a.service_variability.value_or(defaults_of(n).service_variability),
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
static_assert(simsys::defaults::sql_write_cost == 5.0);  // spec §6: 5k reads/s vs 1k writes/s
// spec §6: "blob 500 MB/s" is 500 units of 1 MB per second; a 50 MB stream costs 50 of them.
static_assert(
    simsys::defaults::for_type(simsys::model::NodeType::blob_store).service_rate.per_second == 500);
static_assert(simsys::defaults::byte_cost(simsys::Bytes{50e6}) == 50.0);
// ch. 7: 12 sequence bits per millisecond per machine.
static_assert(simsys::defaults::for_id_algorithm(simsys::model::IdAlgorithm::snowflake)
                  .service_rate.per_second == 4'096'000);
