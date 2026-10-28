// Module `simsys.model`: the typed scenario — what the YAML means once parsed. No
// yaml-cpp here; `simsys.config` fills these structs and the engine reads them.
//
// Models: the boxes and arrows of a System Design Interview (Alex Xu, vol. 1) diagram as
// plain data: node types (ch. 1's building blocks: load balancer, cache, CDN, message
// queue, ...), how a node forwards each operation, the links between datacenters and the
// SLOs a design must meet. Captured: topology, per-node capacity and latency overrides,
// client workloads. Ignored: any simulation semantics; the engine attaches those.
//
// Every default the schema (docs/scenario-schema.md) promises lives as a default member
// initializer here, so loader, engine and tests agree on it by construction.
//
// C++20 named module, interface unit (see src/core/core.cppm for the longer
// explanation): `module;` opens the global module fragment for std headers, `export
// module` starts the purview, and only `export`ed declarations reach importers.
module;
// NO <chrono>/<format>/<print>/<filesystem> here: this interface imports simsys.core
// (GCC 14.2 'Bad file data' bug, CLAUDE.md §3). Durations use simsys::literals (100_ms).
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

export module simsys.model;

import simsys.core;
// The rate curve and noise types of a client's workload. Dependency direction: model ->
// workload -> core; simsys.workload never imports simsys.model, so the curve maths stays
// usable (and testable) without the scenario types.
import simsys.workload;

export namespace simsys::model {

// The node-type catalogue of docs/scenario-schema.md. A scoped enum (`enum class`) so a
// NodeType never converts silently to int; std::uint8_t because 18 values fit in a byte.
// The order matches `node_type_table` below (checked at compile time at the end of file).
enum class NodeType : std::uint8_t {
  client,
  cdn,
  load_balancer,
  rate_limiter,
  service,
  cache,
  sql_db,
  kv_cluster,
  message_queue,
  worker_pool,
  blob_store,
  search_index,
  ws_gateway,
  service_discovery,
  id_generator,
  external_sink,
  notification_fanout,
  frontier
};

// Pattern: Table-driven method — one constant table drives both directions of the
// enum <-> string mapping (core::EnumName, core::name_in, core::parse_in), so adding a
// node type is one enumerator plus one row here (plus its defaults: -Wswitch in
// defaults::for_type is what catches a NEW enumerator, since nothing can count the
// enumerators; the static_assert on the last row at the end of this file catches an
// enumerator appended without a row).
// C++20 std::to_array: builds a std::array from a braced list and deduces its size, so
// the 18 is never written by hand. Replaces `std::array<EnumName<NodeType>, 18>{...}`,
// whose count must be kept in sync manually (CTAD alone cannot deduce from nested braces).
// `inline` is not needed for linkage: a const variable in a module interface's purview
// already has external linkage in C++20 ([basic.link]/3.2), unlike a header constant.
// It is kept so the declaration reads like the C++17 header-style constants students know.
inline constexpr auto node_type_table = std::to_array<core::EnumName<NodeType>>({
    {NodeType::client, "client"},
    {NodeType::cdn, "cdn"},
    {NodeType::load_balancer, "load_balancer"},
    {NodeType::rate_limiter, "rate_limiter"},
    {NodeType::service, "service"},
    {NodeType::cache, "cache"},
    {NodeType::sql_db, "sql_db"},
    {NodeType::kv_cluster, "kv_cluster"},
    {NodeType::message_queue, "message_queue"},
    {NodeType::worker_pool, "worker_pool"},
    {NodeType::blob_store, "blob_store"},
    {NodeType::search_index, "search_index"},
    {NodeType::ws_gateway, "ws_gateway"},
    {NodeType::service_discovery, "service_discovery"},
    {NodeType::id_generator, "id_generator"},
    {NodeType::external_sink, "external_sink"},
    {NodeType::notification_fanout, "notification_fanout"},
    {NodeType::frontier, "frontier"},
});

// The names alone, derived from the table at compile time (an immediately invoked
// constexpr lambda), for callers that iterate the catalogue's spellings.
inline constexpr auto node_type_names = [] {
  std::array<std::string_view, node_type_table.size()> names{};
  for (std::size_t i = 0; i < names.size(); ++i) names[i] = node_type_table[i].name;
  return names;
}();

// False for a value cast from an integer that names no enumerator.
// C++23 std::to_underlying: the enum's integer value without a static_cast that could
// silently accept the wrong type (it always yields exactly the underlying type).
[[nodiscard]] constexpr bool is_cataloged(NodeType t) {
  return std::to_underlying(t) < node_type_table.size();
}

// constexpr function: callable at run time *and* in constant expressions, which is what
// lets the static_asserts below (and in the tests) check the table during compilation.
// An uncataloged value is a programmer error (the loader only produces valid types).
[[nodiscard]] constexpr std::string_view to_string(NodeType t) {
  if (!is_cataloged(t)) throw std::invalid_argument{"unknown NodeType"};
  return core::name_in(node_type_table, t);
}

// The inverse of to_string; std::nullopt for a name not in the catalogue.
[[nodiscard]] constexpr std::optional<NodeType> parse_node_type(std::string_view name) {
  return core::parse_in(node_type_table, name);
}

enum class StepCondition { always, miss, failure };
enum class StepMode { sequential, parallel };

// Pattern: Table-driven method — the words of a step's `on:` and `mode:`, read by the
// loader from these tables (core::parse_in) like every other enum the schema spells.
inline constexpr auto step_condition_table = std::to_array<core::EnumName<StepCondition>>({
    {StepCondition::always, "always"},
    {StepCondition::miss, "miss"},
    {StepCondition::failure, "failure"},
});
inline constexpr auto step_mode_table = std::to_array<core::EnumName<StepMode>>({
    {StepMode::sequential, "sequential"},
    {StepMode::parallel, "parallel"},
});

[[nodiscard]] constexpr std::optional<StepCondition> parse_step_condition(std::string_view name) {
  return core::parse_in(step_condition_table, name);
}
[[nodiscard]] constexpr std::optional<StepMode> parse_step_mode(std::string_view name) {
  return core::parse_in(step_mode_table, name);
}

// One thing a node does with an incoming op: forward (a multiple of) it to `to` as `op`.
// Every struct below is an aggregate (public members, no constructors) with default
// member initializers, so callers can use C++20 designated initializers and name only
// what differs from the default: `Step{.to = "db"}`, `LinkSpec{.from = "a", .to = "b"}`.
// Unnamed members keep their defaults; the names must follow declaration order.
// Every member has a default member initializer, even an empty `{}`: GCC's
// -Wmissing-field-initializers (on with -Wextra, an error under -Werror) flags a
// designated initializer that omits a member without one.
struct Step {
  std::string to{};
  std::string op{};  // defaults to the incoming op name (the loader fills it)
  StepCondition on{StepCondition::always};  // `miss`: only cache misses go on (ch. 1 cache)
  StepMode mode{StepMode::sequential};
  double factor{1.0};  // fan-out: one post -> 100 feed writes (ch. 11 news feed)
};

// The `advanced:` block. Optionals: absent means "use the type default" (simsys.defaults).
struct Advanced {
  std::optional<double> queue_limit{};
  std::optional<int> concurrency{};
  std::optional<double> service_variability{};  // squared coefficient of variation of service
  std::optional<double> burstiness{};  // squared coefficient of variation of inter-arrivals
  std::optional<double> yellow{};      // utilization thresholds of the node's zones
  std::optional<double> red{};
};

// What a client offers: a rate that varies over time, split into ops by fixed shares.
// Phase 2a: the phase-1 `Rate base` member became `curve.base`. The whole `rate:` block is
// now typed (spec §8): the deterministic curve (base or points, times wave, ramp and
// spikes) in `curve`, and the seeded random multiplier in `noise`, kept apart because the
// curve is a pure function of time while noise is a process with state the engine steps.
struct Workload {
  workload::RateCurve curve{};
  std::optional<workload::Noise> noise{};                   // absent: no noise, the curve exactly
  std::vector<std::pair<std::string, double>> op_shares{};  // sorted by op, shares sum to 1
  std::optional<SimTime> timeout{};
};

struct NodeSpec {
  std::string id{};
  // Value-initialized to the first enumerator, client. The loader always sets it from the
  // YAML `type:` key, and tests must set it too: a forgotten type is a silent client.
  NodeType type{};
  std::string dc{"main"};
  int replicas{1};
  std::optional<Rate> capacity{};           // per replica; absent -> type default
  std::optional<ByteRate> byte_capacity{};  // blob stores (phase 2 uses it)
  std::optional<SimTime> latency{};         // service time; absent -> type default
  Advanced advanced{};
  std::map<std::string, std::vector<Step>> ops{};  // incoming op -> steps (service, ws_gateway)
  std::vector<Step> then{};                        // steps after processing (workers, caches...)
  // Capacity units one request of an op consumes (plan 2a refinement 4; service, cache):
  // `capacity` is then a rate of units, and a `write` of cost 3 uses three times the
  // capacity of a `read` of cost 1. An op not listed costs 1, so an empty map is the
  // phase-1 node where every request costs the same. Declared after ops/then so that
  // designated initializers, which must follow declaration order, can write
  // `{..., .ops = ..., .cost = ...}`.
  std::map<std::string, double> cost{};
  std::optional<Workload> workload{};  // clients only
};

enum class Direction { in, out, inout };

// Pattern: Table-driven method — as node_type_table: one table, read in both directions by
// to_string/parse_direction, shared by the loader (reads `dir:`) and the report (prints it).
inline constexpr auto direction_table = std::to_array<core::EnumName<Direction>>({
    {Direction::in, "in"},
    {Direction::out, "out"},
    {Direction::inout, "inout"},
});

[[nodiscard]] constexpr std::string_view to_string(Direction d) {
  return core::name_in(direction_table, d);
}
[[nodiscard]] constexpr std::optional<Direction> parse_direction(std::string_view name) {
  return core::parse_in(direction_table, name);
}

struct LinkSpec {
  std::string from{};
  std::string to{};
  Direction dir{Direction::out};
  std::vector<std::string> ops{};    // empty = all ops
  std::optional<SimTime> latency{};  // one-way; absent -> datacenter default
  double weight{1.0};                // share when several links carry the same op
};

enum class SloMetric {
  p50_latency,
  p95_latency,
  p99_latency,
  drop_rate,
  utilization,
  queue_lag,
  staleness
};

// Pattern: Table-driven method — the SLO metric names, read by the loader (`metric:`) and
// printed by the report from this one table.
inline constexpr auto slo_metric_table = std::to_array<core::EnumName<SloMetric>>({
    {SloMetric::p50_latency, "p50_latency"},
    {SloMetric::p95_latency, "p95_latency"},
    {SloMetric::p99_latency, "p99_latency"},
    {SloMetric::drop_rate, "drop_rate"},
    {SloMetric::utilization, "utilization"},
    {SloMetric::queue_lag, "queue_lag"},
    {SloMetric::staleness, "staleness"},
});

[[nodiscard]] constexpr std::string_view to_string(SloMetric m) {
  return core::name_in(slo_metric_table, m);
}
[[nodiscard]] constexpr std::optional<SloMetric> parse_slo_metric(std::string_view name) {
  return core::parse_in(slo_metric_table, name);
}

struct SloSpec {
  SloMetric metric{};
  std::optional<std::string> node{};
  std::optional<std::string> op{};
  // C++17 std::variant: a type-safe union that knows which alternative it holds. Latency
  // SLOs carry a duration, ratio SLOs a plain number; the variant keeps "0.01" from being
  // read as 0.01 seconds, which a single double field could not prevent.
  std::variant<SimTime, double> max{};
};

struct Scenario {
  std::string name{};
  int version{};
  int seed{1};
  SimTime duration{};
  // 100_ms: simsys::literals is an inline namespace of simsys, so its literals are found
  // from simsys::model without a using-directive. 100 ms resolves sub-second dynamics
  // (queue build-up) while a one-hour run stays at 36k steps.
  SimTime step{100_ms};
  SimTime warmup{};
  std::vector<std::string> datacenters{"main"};
  std::vector<NodeSpec> nodes{};
  std::vector<LinkSpec> links{};
  std::vector<SloSpec> slos{};
};

}  // namespace simsys::model

// Compile-time table checks: a misordered or duplicated row, or an enumerator appended
// without a row, breaks the build here, not a scenario at run time. core::round_trips
// proves each table's contract; the last-row asserts stand in for "every enumerator has a
// row", which nothing in the language can count.
static_assert(simsys::core::round_trips(simsys::model::node_type_table));
static_assert(simsys::core::round_trips(simsys::model::direction_table));
static_assert(simsys::core::round_trips(simsys::model::slo_metric_table));
static_assert(simsys::core::round_trips(simsys::model::step_condition_table));
static_assert(simsys::core::round_trips(simsys::model::step_mode_table));
static_assert(simsys::model::node_type_table.back().value == simsys::model::NodeType::frontier);
static_assert(simsys::model::direction_table.back().value == simsys::model::Direction::inout);
static_assert(simsys::model::slo_metric_table.back().value == simsys::model::SloMetric::staleness);
static_assert(simsys::model::step_condition_table.back().value ==
              simsys::model::StepCondition::failure);
static_assert(simsys::model::step_mode_table.back().value == simsys::model::StepMode::parallel);
