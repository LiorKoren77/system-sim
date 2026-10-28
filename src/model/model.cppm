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

// One row of the catalogue: an enumerator and its YAML spelling, side by side, so a row
// can never pair a name with the wrong enumerator.
struct NodeTypeName {
  NodeType type;
  std::string_view name;
};

// Pattern: Table-driven method — one constant table drives both directions of the
// enum <-> string mapping, so adding a node type is one enumerator plus one row here
// (plus its defaults: -Wswitch in defaults::for_type is what catches a NEW enumerator,
// since nothing here can count the enumerators; is_cataloged() then rejects an
// enumerator that got a case but no row).
// C++20 std::to_array: builds a std::array from a braced list and deduces its size, so
// the 18 is never written by hand. Replaces `std::array<NodeTypeName, 18>{...}`, whose
// count must be kept in sync manually (CTAD alone cannot deduce from nested braces).
// `inline` is not needed for linkage: a const variable in a module interface's purview
// already has external linkage in C++20 ([basic.link]/3.2), unlike a header constant.
// It is kept so the declaration reads like the C++17 header-style constants students know.
inline constexpr auto node_type_table = std::to_array<NodeTypeName>({
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

// C++20 `consteval`: an immediate function, which may ONLY run at compile time (calling
// it at run time is a compile error). Stronger than constexpr, which merely allows it;
// right here because this is a build-time proof, used in static_assert below and in the
// tests. Row i must hold the enumerator whose value is i, so indexing the table by
// std::to_underlying(type) is correct.
[[nodiscard]] consteval bool table_follows_enum_order() {
  for (std::size_t i = 0; i < node_type_table.size(); ++i) {
    if (std::to_underlying(node_type_table[i].type) != i) return false;
  }
  return true;
}

// The names alone, derived from the table at compile time (an immediately invoked
// constexpr lambda), for callers that iterate the catalogue's spellings.
inline constexpr auto node_type_names = [] {
  std::array<std::string_view, node_type_table.size()> names{};
  for (std::size_t i = 0; i < names.size(); ++i) names[i] = node_type_table[i].name;
  return names;
}();

// C++23 std::to_underlying: the enum's integer value without a static_cast that could
// silently accept the wrong type (it always yields exactly the underlying type).
// False for a value cast from an integer that names no enumerator.
[[nodiscard]] constexpr bool is_cataloged(NodeType t) {
  return std::to_underlying(t) < node_type_table.size();
}

// constexpr function: callable at run time *and* in constant expressions, which is what
// lets the static_asserts below (and in the tests) check the table during compilation.
// An uncataloged value is a programmer error (the loader only produces valid types).
[[nodiscard]] constexpr std::string_view to_string(NodeType t) {
  if (!is_cataloged(t)) throw std::invalid_argument{"unknown NodeType"};
  return node_type_table[std::to_underlying(t)].name;
}

// The inverse of to_string; std::nullopt for a name not in the catalogue.
[[nodiscard]] constexpr std::optional<NodeType> parse_node_type(std::string_view name) {
  for (const auto& row : node_type_table) {
    if (row.name == name) return row.type;
  }
  return std::nullopt;
}

enum class StepCondition { always, miss, failure };
enum class StepMode { sequential, parallel };

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

// What a client offers. Phase 1: a constant base rate; curves come in phase 2.
struct Workload {
  Rate base{};
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
  std::optional<Workload> workload{};              // clients only
};

enum class Direction { in, out, inout };

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

// Compile-time table check: a misordered row in node_type_table breaks the build here,
// not a scenario at run time.
static_assert(simsys::model::table_follows_enum_order());
static_assert(simsys::model::parse_node_type("frontier") == simsys::model::NodeType::frontier);
