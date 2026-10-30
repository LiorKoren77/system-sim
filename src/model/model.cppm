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
#include <algorithm>
#include <array>
#include <cmath>
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

// One row of the node-type catalogue: the enumerator, the word the schema spells it with, and
// whether run/estimate can step a node of the type today (`runs`). The four types without a
// model yet (ws_gateway, service_discovery, notification_fanout, frontier: phase 3) have it
// false; explain and validate take every type. A row like core::EnumName with one more column,
// which core's lookups read through their NameRow concept.
struct NodeTypeRow {
  NodeType value;
  std::string_view name;
  bool runs;
};

// Pattern: Table-driven method — one constant table drives both directions of the
// enum <-> string mapping (core::name_in, core::parse_in) and says which types the engine
// runs (simulated), so adding a node type is one enumerator plus one row here (plus its
// defaults: -Wswitch in defaults::for_type and engine::Simulation::make_node is what catches a
// NEW enumerator, since nothing can count the enumerators; the static_assert on the last row at
// the end of this file catches an enumerator appended without a row).
// C++20 std::to_array: builds a std::array from a braced list and deduces its size, so
// the 18 is never written by hand. Replaces `std::array<NodeTypeRow, 18>{...}`,
// whose count must be kept in sync manually (CTAD alone cannot deduce from nested braces).
// `inline` is not needed for linkage: a const variable in a module interface's purview
// already has external linkage in C++20 ([basic.link]/3.2), unlike a header constant.
// It is kept so the declaration reads like the C++17 header-style constants students know.
inline constexpr auto node_type_table = std::to_array<NodeTypeRow>({
    {NodeType::client, "client", true},
    {NodeType::cdn, "cdn", true},
    {NodeType::load_balancer, "load_balancer", true},
    {NodeType::rate_limiter, "rate_limiter", true},
    {NodeType::service, "service", true},
    {NodeType::cache, "cache", true},
    {NodeType::sql_db, "sql_db", true},
    {NodeType::kv_cluster, "kv_cluster", true},
    {NodeType::message_queue, "message_queue", true},
    {NodeType::worker_pool, "worker_pool", true},
    {NodeType::blob_store, "blob_store", true},
    {NodeType::search_index, "search_index", true},
    {NodeType::ws_gateway, "ws_gateway", false},
    {NodeType::service_discovery, "service_discovery", false},
    {NodeType::id_generator, "id_generator", true},
    {NodeType::external_sink, "external_sink", true},
    {NodeType::notification_fanout, "notification_fanout", false},
    {NodeType::frontier, "frontier", false},
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

// Can run/estimate step a node of type `t` today? Its row's `runs` (engine::Simulation::create
// refuses a scenario with any other type, naming the phase that plans its model). An uncataloged
// value is a programmer error, as for to_string.
[[nodiscard]] constexpr bool simulated(NodeType t) {
  if (!is_cataloged(t)) throw std::invalid_argument{"unknown NodeType"};
  return node_type_table[std::to_underlying(t)].runs;
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

// How a load balancer spreads requests over the replicas behind it (System Design Interview ch. 1
// "Load balancer"; ch. 5 for the hash ring). In the fluid model the policy decides how the
// DOWNSTREAM node's replicas queue (spec §17 entry 5, see graph::Graph::mark_balanced_targets):
// round_robin keeps them independent queues, least_connections pools them into one, consistent_hash
// loads the hottest one more than the average, weighted splits by the links' `weight` (as every
// policy does across several outgoing links).
enum class LbPolicy { round_robin, least_connections, weighted, consistent_hash };

// Pattern: Table-driven method — the policy names, read by the loader (`policy:`) and
// printed by explain from this one table.
inline constexpr auto lb_policy_table = std::to_array<core::EnumName<LbPolicy>>({
    {LbPolicy::round_robin, "round_robin"},
    {LbPolicy::least_connections, "least_connections"},
    {LbPolicy::weighted, "weighted"},
    {LbPolicy::consistent_hash, "consistent_hash"},
});

[[nodiscard]] constexpr std::string_view to_string(LbPolicy p) {
  return core::name_in(lb_policy_table, p);
}
[[nodiscard]] constexpr std::optional<LbPolicy> parse_lb_policy(std::string_view name) {
  return core::parse_in(lb_policy_table, name);
}

// How a rate limiter decides which requests to turn away (System Design Interview ch. 4 "Design a
// rate limiter", the five algorithms of its "Algorithms for rate limiting" section). In the fluid
// model (spec §17 entry 6, see behaviours::RateLimiterBehaviour) each keeps the state ch. 4
// describes, stepped every dt:
//   token_bucket     -> a bucket of `burst` tokens refilled at `limit`;
//   leaky_bucket     -> a FIFO bucket of room `burst` leaking one request every 1/limit:
//                       excess requests wait, a full bucket refuses;
//   fixed_window     -> a counter per calendar window of length `window`, reset at each
//                       boundary: at most limit x window per window;
//   sliding_log      -> the admitted counts of the last `window` seconds, step by step:
//                       at most limit x window in ANY window;
//   sliding_counter  -> this window's count plus the previous one's, weighted by the
//                       overlap of the trailing window with it.
// `burst` sizes the two buckets only; the windowed three are sized by `window`.
enum class LimiterAlgorithm {
  token_bucket,
  leaky_bucket,
  fixed_window,
  sliding_log,
  sliding_counter
};

// Pattern: Table-driven method — the algorithm names, read by the loader (`algorithm:`) and
// printed by explain from this one table.
inline constexpr auto limiter_algorithm_table = std::to_array<core::EnumName<LimiterAlgorithm>>({
    {LimiterAlgorithm::token_bucket, "token_bucket"},
    {LimiterAlgorithm::leaky_bucket, "leaky_bucket"},
    {LimiterAlgorithm::fixed_window, "fixed_window"},
    {LimiterAlgorithm::sliding_log, "sliding_log"},
    {LimiterAlgorithm::sliding_counter, "sliding_counter"},
});

// The three algorithms that count requests per window rather than hold a bucket: they take
// `window`, and a `burst` on them is an error (config's read_limiter_burst, graph's
// invalid_limiter).
[[nodiscard]] constexpr bool is_windowed(LimiterAlgorithm a) {
  return a == LimiterAlgorithm::fixed_window || a == LimiterAlgorithm::sliding_log ||
         a == LimiterAlgorithm::sliding_counter;
}

[[nodiscard]] constexpr std::string_view to_string(LimiterAlgorithm a) {
  return core::name_in(limiter_algorithm_table, a);
}
[[nodiscard]] constexpr std::optional<LimiterAlgorithm> parse_limiter_algorithm(
    std::string_view name) {
  return core::parse_in(limiter_algorithm_table, name);
}

// How an id generator issues unique ids (System Design Interview ch. 7 "Design a unique ID
// generator in distributed systems"): three of the designs the chapter compares. The model has no
// behaviour of its own for any of them (spec §18 entry 12): Graph::build turns the algorithm into
// the capacity and service time of an ordinary service (defaults:: for_id_algorithm), so each
// design differs only in how many ids per second one replica can issue and how long one takes:
//   snowflake      -> Twitter's 64-bit layout: timestamp | datacenter | machine | 12 sequence
//                     bits. A machine issues at most 2^12 = 4096 ids per millisecond, with no
//                     coordination, so replicas add capacity.
//   ticket_server  -> Flickr's single auto-increment database: every id is a write on one SQL
//                     machine; a single point of failure, so exactly one replica.
//   uuid           -> a 128-bit version-4 UUID, 122 of its bits random (the other 6 name the
//                     version and the variant), generated locally: nothing to coordinate and
//                     nothing to wait for, unbounded in practice.
// Ignored: what the chapter weighs besides throughput (64 bits versus 128, sortable by time,
// clock skew and NTP); snowflake's 5-bit machine field, which caps a datacenter at 2^5 = 32
// machines (replicas beyond 32 add capacity here that the id layout could not number); the
// chapter's remedy for the ticket server's single point of failure, several ticket servers,
// which costs keeping them in sync (the model has no such mode: replicas must be 1); and
// multi-master replication, the chapter's fourth design.
enum class IdAlgorithm { snowflake, ticket_server, uuid };

// Why an id generator of algorithm ticket_server takes no second replica, for the loader (config's
// read_id_generator_keys) and Graph::build (scenarios built in code) alike: one text, two checks.
inline constexpr std::string_view ticket_server_replicas_rule =
    "a ticket server is one auto-increment database: replicas must be 1 (ch. 7; for more ids "
    "per second use snowflake)";

// Pattern: Table-driven method — the algorithm names, read by the loader (`algorithm:` on an
// id_generator) and printed by explain from this one table.
inline constexpr auto id_algorithm_table = std::to_array<core::EnumName<IdAlgorithm>>({
    {IdAlgorithm::snowflake, "snowflake"},
    {IdAlgorithm::ticket_server, "ticket_server"},
    {IdAlgorithm::uuid, "uuid"},
});

[[nodiscard]] constexpr std::string_view to_string(IdAlgorithm a) {
  return core::name_in(id_algorithm_table, a);
}
[[nodiscard]] constexpr std::optional<IdAlgorithm> parse_id_algorithm(std::string_view name) {
  return core::parse_in(id_algorithm_table, name);
}

// What a Dynamo-style key-value store gives up when a network partition splits it (System Design
// Interview ch. 6, "CAP theorem"): `ap` keeps answering on both sides and returns stale data
// (Dynamo, Cassandra), `cp` refuses what cannot reach a quorum (unavailable instead of stale).
// Read and printed only: without partitions (a phase 4 backlog item) the two behave alike.
enum class KvMode { ap, cp };

// Pattern: Table-driven method — the mode names, read by the loader (`mode:`) and printed by
// explain from this one table.
inline constexpr auto kv_mode_table = std::to_array<core::EnumName<KvMode>>({
    {KvMode::ap, "ap"},
    {KvMode::cp, "cp"},
});

[[nodiscard]] constexpr std::string_view to_string(KvMode m) {
  return core::name_in(kv_mode_table, m);
}
[[nodiscard]] constexpr std::optional<KvMode> parse_kv_mode(std::string_view name) {
  return core::parse_in(kv_mode_table, name);
}

// How a cache and the store behind it share reads and writes (System Design Interview ch. 1
// "Cache", and the caches of ch. 8 url shortener, ch. 11 news feed, ch. 12 chat, ch. 13
// autocomplete, ch. 14 YouTube). Names: ch. 1 draws one pattern, the application reading the cache
// and fetching a miss from the database itself, and calls it a "read-through cache"; the industry's
// name for that pattern is cache-aside, and that is ours. Our `read_through` is the cache-driven
// fetch (the cache itself loads a miss), and the write strategies are general caching practice; the
// book names none of them. In the fluid model (spec §17 entry 7, see behaviours::CacheBehaviour),
// with a fixed hit ratio h:
//   cache_aside   -> "lazy loading": the APPLICATION asks the cache, and on a miss reads the
//                    store itself (a service step `{to: db, on: miss}`, which carries the miss
//                    fraction 1 - h); the cache only answers. Writes go to the store through
//                    the service's own steps.
//   read_through  -> the CACHE fetches a missing value from its `backend` (1 - h of the reads)
//                    and the caller waits for that fetch on a miss.
//   write_through -> every write goes to the cache AND, synchronously, to the backend.
//   write_around  -> writes go to the backend and skip the cache; for the flow the same as
//                    write_through (the difference, what the cache then holds, shows in the
//                    hit ratio, which is fixed here).
//   write_back    -> writes are acknowledged by the cache and flushed to the backend later
//                    (a queue of dirty entries): every write still reaches the backend, at
//                    the same rate (an upper bound: no coalescing), but over an asynchronous
//                    hop, so the writer waits for the cache alone (spec §18 entry 5;
//                    graph's write_back_flushes).
enum class CacheStrategy { cache_aside, read_through, write_through, write_around, write_back };

// Pattern: Table-driven method — the strategy names, read by the loader (`strategy:`) and
// printed by explain from this one table.
inline constexpr auto cache_strategy_table = std::to_array<core::EnumName<CacheStrategy>>({
    {CacheStrategy::cache_aside, "cache_aside"},
    {CacheStrategy::read_through, "read_through"},
    {CacheStrategy::write_through, "write_through"},
    {CacheStrategy::write_around, "write_around"},
    {CacheStrategy::write_back, "write_back"},
});

[[nodiscard]] constexpr std::string_view to_string(CacheStrategy s) {
  return core::name_in(cache_strategy_table, s);
}
[[nodiscard]] constexpr std::optional<CacheStrategy> parse_cache_strategy(std::string_view name) {
  return core::parse_in(cache_strategy_table, name);
}

// The strategies in which the CACHE talks to the store, so it needs a `backend` (and a link
// to it): all but cache_aside, where the application does (and where a backend is refused:
// it would never be called).
[[nodiscard]] constexpr bool needs_backend(CacheStrategy s) {
  return s != CacheStrategy::cache_aside;
}

// A cache's `hit_ratio`: `{fixed: h}`, the share of reads the cache answers, given by the
// scenario (0 <= h <= 1), or `che`: computed from the cache size (`keys`) and the key
// popularity of the clients whose ops reach it (their KeySpec) by the Che approximation of an LRU
// cache (simsys.caching; the graph resolves it once, at build). The loader sets exactly one of the
// two; a HitRatio{} built in code sets neither, which counts as no hit ratio at all.
struct HitRatio {
  std::optional<double> fixed{};
  bool che{false};
};

// The ops a cache (and every other node type but a database) treats as writes when the scenario
// gives no `write_ops`: one op called `write`. Every other op is a read. Applied by write_ops_of.
inline constexpr std::string_view default_write_op = "write";

// The ops a SQL database sends to its primary when the scenario gives no `write_ops` (spec §18
// entry 6): the three SQL statements that change rows, plus the generic `write` the other node
// types use. Every other op (select, read, get, ...) is a read a replica may answer. Sorted.
// Applied by write_ops_of.
inline constexpr std::array<std::string_view, 4> default_sql_write_ops{"delete", "insert", "update",
                                                                       "write"};

// The window a windowed limiter counts over when the scenario gives none. Chosen: the
// book's ch. 4 examples are all "N requests per second", i.e. a one-second window.
inline constexpr SimTime default_limiter_window = 1_s;

// Virtual nodes per server on a consistent-hash ring when the scenario gives none.
// Chosen: the book's ch. 5 reports the standard deviation of a server's load at about 10%
// of the mean with 100 virtual nodes and 5% with 200; 150 sits between the two (the lab's
// "try 150"). graph::Graph::mark_balanced_targets turns it into the imbalance factor.
inline constexpr int default_vnodes = 150;

// A kv_cluster's replication factor N and its two quorums when the scenario gives none: book
// ch. 6's worked example, N = 3 with W = R = 2, Dynamo's classic configuration (W + R > N, so a
// read always meets the latest write). The quorums default to min(2, N) (kv_write_quorum,
// kv_read_quorum), so a cluster that states `replication: 1` keeps a valid W = R = 1.
inline constexpr int default_replication = 3;
inline constexpr int default_quorum = 2;

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
  // The payload this step's request carries (spec §19 entry 1), when the step states one: a worker
  // or service that sends bytes of its own, such as the youtube scenario's transcoders writing a 50
  // MB rendition (its choice; ch. 14 sizes no rendition) to the blob store. Positive (the loader
  // refuses 0B: a request of no bytes costs a byte-sized node nothing). Absent: the request keeps
  // its incoming size while it keeps its op name, and a renamed op is unsized
  // (graph::Graph::request_bytes resolves which size each (node, op) carries).
  std::optional<Bytes> size{};
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

// What a client offers: a rate that varies over time, split into ops by fixed shares. Phase 2a: the
// phase-1 `Rate base` member became `curve.base`. The whole `rate:` block is now typed (spec §8):
// the deterministic curve (base or points, times wave, ramp and spikes) in `curve`, and the seeded
// random multiplier in `noise`, kept apart because the curve is a pure function of time while noise
// is a process with state the engine steps. A client's request key distribution (spec §18 entry 1,
// `keys:` on a client): how popular each of the `space` keys it asks for is, under the independent
// reference model of simsys.caching (every request picks a key independently, with a fixed
// probability).
//  - `{zipf: s, space: N}`: Zipf over N keys with exponent s, `zipf` set — the key of rank i
//    is asked with probability ∝ i^-s (as in book ch. 8's URL shortener, where a few short
//    links get most of the clicks: our paraphrase; s ≈ 1 is the classic web skew);
//  - `{uniform: N}`: every key equally likely, `zipf` empty.
// `space` is a count of keys, a double because key spaces run to 1e9 and the Che sums are in
// doubles anyway. A che cache reads the KeySpec of the clients whose ops reach it (graph's
// agreed_keys); the clients must agree on one. Canonical form (canonical below): Zipf with
// exponent 0 IS the uniform distribution (i^0 = 1 for every rank), and is stored as uniform,
// `zipf` empty, so that == compares distributions rather than spellings.
struct KeySpec {
  std::optional<double> zipf{};
  double space{};
  // C++20 defaulted operator==: member-wise equality in one line, replacing a hand-written
  // comparison of both members; it is how the graph tells "the same distribution" from two
  // clients apart from "different ones" (on the canonical form).
  bool operator==(const KeySpec&) const = default;
};

// `keys` in canonical form: `{zipf: 0, space: N}` becomes `{uniform: N}`. The loader stores
// every client's keys this way; the graph applies it again for scenarios built in code.
[[nodiscard]] constexpr KeySpec canonical(KeySpec keys) {
  if (keys.zipf == 0.0) keys.zipf.reset();
  return keys;
}

struct Workload {
  workload::RateCurve curve{};
  std::optional<workload::Noise> noise{};                   // absent: no noise, the curve exactly
  std::vector<std::pair<std::string, double>> op_shares{};  // sorted by op, shares sum to 1
  // Declared right after op_shares so designated initializers can write
  // `{.curve = ..., .op_shares = ..., .keys = ...}`. Absent: the client says nothing about
  // which keys it asks for (fine unless a `che` cache downstream needs to know).
  std::optional<KeySpec> keys{};
  std::optional<SimTime> timeout{};
  // The size of one request of each op the client sends, for the ops that carry bytes (spec §19
  // entry 1, `sizes:` on a client): ch. 14 YouTube's 300 MB average upload, ch. 15 Google Drive's
  // 500 KB average file (the youtube scenario's 50 MB stream is its own choice: the book sizes no
  // stream). Each is positive (the loader refuses 0B). An op not listed is unsized, which only a
  // byte-sized node or a bandwidth link minds; a request-counting node never needs a size. Each
  // size is the op's mean: the model ignores a size distribution, headers and protocol overhead,
  // and compression. Sorted by op, like op_shares.
  std::map<std::string, Bytes> sizes{};
};

struct NodeSpec {
  std::string id{};
  // Value-initialized to the first enumerator, client. The loader always sets it from the
  // YAML `type:` key, and tests must set it too: a forgotten type is a silent client.
  NodeType type{};
  std::string dc{"main"};
  // The copies of the node, each of `capacity`. A kv_cluster may not state it (the loader refuses
  // the key): graph::apply_kv_ring overwrites it with the cluster's `kv_nodes`, so the readers of
  // a node's copies count its machines; read a cluster's machine count from `kv_nodes`.
  int replicas{1};
  std::optional<Rate> capacity{};           // per replica; absent -> type default
  std::optional<ByteRate> byte_capacity{};  // blob stores, CDNs: they count megabytes
  std::optional<SimTime> latency{};         // service time; absent -> type default
  Advanced advanced{};
  std::map<std::string, std::vector<Step>> ops{};  // incoming op -> steps (service, ws_gateway)
  std::vector<Step> then{};                        // steps after processing (workers, caches...)
  // Capacity units one request of an op consumes (spec §17 entry 4; service, cache): `capacity` is
  // then a rate of units, and a `write` of cost 3 uses three times the capacity of a `read` of cost
  // 1. An op not listed costs 1, so an empty map is the phase-1 node where every request costs the
  // same. Declared after ops/then so that designated initializers, which must follow declaration
  // order, can write `{..., .ops = ..., .cost = ...}`.
  std::map<std::string, double> cost{};
  // Load balancers only (declared after `cost`, for designated initializers): the policy (absent:
  // round_robin) and, for consistent_hash, the virtual nodes per server (absent: default_vnodes).
  // The loader leaves an absent key absent, so explain can tell a stated number from a default;
  // every reader applies the default. A kv_cluster's ring reads `vnodes` too (spec §19 entry 9: its
  // machines sit on a ring of their own, see kv_nodes below).
  std::optional<LbPolicy> policy{};
  std::optional<int> vnodes{};
  // Rate limiters only (spec §17 entry 6; declared after the LB keys, for designated initializers):
  // the algorithm (the loader fills in token_bucket when absent), the limit (required: a limiter
  // without one limits nothing), the burst of the two buckets (requests a token bucket lets through
  // above the limit at once, the room of a leaky bucket; absent: limit x window, see limiter_burst;
  // the windowed algorithms reject one), the window of the windowed algorithms (absent:
  // default_limiter_window, see limiter_window) and the node holding the counters (`backend`, a
  // cache), which the limiter consults once for every request it checks. The loader leaves an
  // absent burst or window absent, so explain can say "(default)"; the behaviour, the graph and the
  // report apply the defaults through limiter_burst and limiter_window. A scenario built in code
  // may leave the algorithm empty too.
  std::optional<LimiterAlgorithm> algorithm{};
  std::optional<Rate> limit{};
  std::optional<int> burst{};
  std::optional<SimTime> window{};
  std::optional<std::string> backend{};
  // Caches only (spec §17 entry 7; declared after `backend`, for designated initializers): the hit
  // ratio (required by the loader; a scenario built in code may leave it empty, which the engine
  // refuses), the strategy (default cache_aside, the pattern ch. 1 draws and calls read-through),
  // the ops that are writes (every other op is a read), and the sizing keys the Che approximation
  // reads: `keys` (how many entries fit), `eviction` (lru, lfu, fifo or ttl) and `ttl`. A `backend`
  // on a cache names the store the cache itself calls (read_through, write_through, write_around,
  // write_back); a CDN reads `hit_ratio`, `keys`, `ttl` and `write_ops` too (see `origin` below).
  // `write_ops` (caches, CDNs, SQL databases and blob stores, which answer a write at its last byte
  // and a read at its first megabyte): absent means the TYPE's default, present means exactly the
  // list the scenario wrote; the loader stores it as written and every reader asks write_ops_of. An
  // optional rather than a vector pre-filled with a default, because the two types' defaults differ
  // and a pre-filled `[write]` could not be told from a database's explicit `write_ops: [write]`.
  std::optional<HitRatio> hit_ratio{};
  CacheStrategy strategy{CacheStrategy::cache_aside};
  std::optional<std::vector<std::string>> write_ops{};
  std::optional<std::int64_t> keys{};
  std::optional<std::string> eviction{};
  std::optional<SimTime> ttl{};
  // CDNs only (spec §18 entry 10; declared after the cache keys, for designated initializers): the
  // node a CDN fetches its misses from, as the same op (book ch. 1: an edge that does not have a
  // file asks the origin server for it). Required: the loader and graph::Graph::build refuse a CDN
  // without one, and one that names a client.
  std::optional<std::string> origin{};
  // SQL databases only (spec §18 entry 6; declared after the cache keys, for designated
  // initializers). `replicas` is the primary plus `replicas - 1` read replicas (book ch. 1,
  // "database replication"); `shards` splits the data over that many such sets (ch. 1, "sharding";
  // absent: one); `pool` is the connection pool, the requests each copy serves at once: it sets the
  // copy's c and caps its rate at pool / service time by Little's law (graph::apply_database_keys),
  // so it is not advanced.concurrency renamed, and the loader refuses both together (absent: the
  // implied concurrency, no cap); `replication_lag` how far behind the primary a replica applies a
  // write (absent: 0, shipping takes no time). The loader leaves an absent one absent, so explain
  // can tell stated from default. `write_ops` (above) says which ops go to the primary; a
  // database's default is default_sql_write_ops (write_ops_of).
  std::optional<int> shards{};
  std::optional<int> pool{};
  std::optional<SimTime> replication_lag{};
  // Id generators only (spec §18 entry 12; declared after the database keys, for designated
  // initializers): which of ch. 7's designs the node is, and so its default capacity and service
  // time (defaults::for_id_algorithm). Required: the loader and Graph::build refuse an id_generator
  // without one. A separate member from the rate limiter's `algorithm`, although both are spelled
  // `algorithm:` in a scenario: the two name different things.
  std::optional<IdAlgorithm> id_algorithm{};
  // Worker pools only (spec §19 entry 5; declared after the id generator's key, for designated
  // initializers): the pool is M/M/c over consumption (spec §7: "capacity = workers / job cost"),
  // so `workers` is its c and `job_cost` the time ONE worker spends on ONE job, the scenario's
  // `cost:` key, a duration here (`cost: 60s`), while a service's `cost:` is the per-op map of
  // capacity units above (`cost`): one spelling, two meanings, read by type (config's read_cost and
  // read_worker_pool_keys). Both are required by the loader; a pool built in code without them
  // falls back to the type's numbers. `consumes` names the message queue (or URL frontier) whose
  // messages the pool drains; Graph::build checks that the queue links to it.
  std::optional<int> workers{};
  std::optional<SimTime> job_cost{};
  std::optional<std::string> consumes{};
  // Message queues only (spec §19 entry 4): how long a message stays in the log before it is
  // deleted unconsumed (absent: defaults::default_retention, Kafka's 7 days), which sets each
  // consumer group's waiting room (Graph::build: retention x the pool's capacity), and
  // `partitions`, accepted and printed, not modelled (phase 3: a partition is read by one worker of
  // a group at a time). The loader leaves an absent one absent, so explain can say "(default)".
  std::optional<SimTime> retention{};
  std::optional<int> partitions{};
  // External sinks only (spec §19 entry 8; declared after the message queue's keys, for designated
  // initializers): a third party's API, spec section 6's `third_party_api`, the providers of book
  // ch. 10's notification system (APNs, FCM, an SMS gateway, an e-mail service) that our workers
  // call and do not run. `failure_rate` is the share, in [0, 1], of the requests the sink SERVES
  // that fail (a 5xx from the provider), which the engine moves from the node's drain to its drops
  // (engine.cppm's fail_share). A plain double, not an optional: "fails nothing" is the real value
  // 0, and a sink that states none is the plain queue it was before. `politeness` is how often one
  // host may be called (the crawler's "1/s per host"), stored and printed, not modelled (phase 3:
  // it needs per-host state); absent when not stated. Ignored: retries (an `on: failure` step never
  // fires before phase 3), a provider's latency rising with OUR load (its queue is the type's 1k/s
  // quota, not the provider's real fleet), and per-host politeness.
  double failure_rate{0.0};
  std::optional<Rate> politeness{};
  // Key-value clusters only (spec §19 entry 9; declared after the external sink's keys, for
  // designated initializers): book ch. 6's Dynamo-style store. `kv_nodes` (the scenario's `nodes:`)
  // is how many machines sit on its consistent-hash ring, each of `capacity`, with `vnodes` virtual
  // nodes each (the balancer's member above, shared: the same ring); every key lives on
  // `replication` N of them, a write is acknowledged after `write_quorum` W replicas and a read
  // answered after `read_quorum` R. `kv_nodes` is required (the loader and Graph::build refuse a
  // cluster without one); the others default (default_replication, default_quorum: the book's N =
  // 3, W = R = 2) through kv_replication, kv_write_quorum and kv_read_quorum. The loader leaves an
  // absent one absent. `kv_mode` is ap or cp, what a partition would cost (KvMode): read and
  // printed, not modelled without partitions (a phase 4 backlog item); absent means ap
  // (kv_mode_of), kept absent so explain can mark it "(default)" as it marks N, W and R.
  std::optional<int> kv_nodes{};
  std::optional<int> replication{};
  std::optional<int> write_quorum{};
  std::optional<int> read_quorum{};
  std::optional<KvMode> kv_mode{};
  // Search indexes only (spec §19 entry 10; declared after the kv keys, for designated
  // initializers): book ch. 13 ("Design a search autocomplete system") keeps its trie database
  // fresh by rebuilding it periodically from the aggregated query logs, and `build_interval` says
  // how often (search-autocomplete: 24h). Serving is read-mostly, so a search index runs as a
  // service (engine::Simulation::make_node); the interval is stored and printed by explain, and
  // absent means "not stated", never zero (the loader refuses a zero). Ignored: the rebuild's own
  // load on the index and on the aggregators, and the freshness of the answers (a query typed after
  // the last rebuild still finds the old trie).
  std::optional<SimTime> build_interval{};
  std::optional<Workload> workload{};  // clients only
};

// A kv_cluster's replication factor N and its quorums W and R with the defaults applied: N = 3
// (default_replication), W = R = min(2, N) (default_quorum, lowered for a cluster of N = 1). The
// one place the defaults live, so the graph, the behaviour and explain agree.
[[nodiscard]] constexpr int kv_replication(const NodeSpec& spec) {
  return spec.replication.value_or(default_replication);
}
[[nodiscard]] constexpr int kv_write_quorum(const NodeSpec& spec) {
  return spec.write_quorum.value_or(std::min(default_quorum, kv_replication(spec)));
}
[[nodiscard]] constexpr int kv_read_quorum(const NodeSpec& spec) {
  return spec.read_quorum.value_or(std::min(default_quorum, kv_replication(spec)));
}
// The mode, ap (Dynamo's choice: available and possibly stale under a partition) by default.
[[nodiscard]] constexpr KvMode kv_mode_of(const NodeSpec& spec) {
  return spec.kv_mode.value_or(KvMode::ap);
}

// The window and the burst a rate limiter works with, defaults applied. The loader leaves
// an absent one empty (so explain can say "(default)"), as may a scenario built in code,
// and the behaviour, the graph and explain must agree on one number. Default burst (token and leaky
// bucket; the windowed algorithms have none): limit x window, rounded up to a whole request and at
// least one (see config's read_limiter_burst for why); 0 without a limit (Graph::build rejects such
// a limiter before anything asks).
[[nodiscard]] constexpr SimTime limiter_window(const NodeSpec& spec) {
  return spec.window.value_or(default_limiter_window);
}
[[nodiscard]] inline double limiter_burst(const NodeSpec& spec) {
  if (spec.burst) return *spec.burst;
  if (!spec.limit) return 0.0;
  return std::max(std::ceil(spec.limit->per_second * seconds(limiter_window(spec))), 1.0);
}

// The ops `spec` treats as writes: the scenario's `write_ops` exactly as written, else the
// type's default (default_sql_write_ops for a SQL database, default_write_op for every other
// node). The one place the default is applied, so the graph, the behaviours and explain agree.
[[nodiscard]] inline std::vector<std::string> write_ops_of(const NodeSpec& spec) {
  if (spec.write_ops) return *spec.write_ops;
  if (spec.type == NodeType::sql_db) {
    return {default_sql_write_ops.begin(), default_sql_write_ops.end()};
  }
  return {std::string{default_write_op}};
}

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
  // The bytes per second the network hop can carry (spec section 3.4: a link's bandwidth "caps
  // bytes/s and queues the excess like a node"), written in bytes or, as a network is rated,
  // in bits (40Gbit/s = 5e9 B/s). Absent: the link only adds its latency, the pipe is never the
  // constraint. The engine models the pipe (engine::Simulation::pass_through_pipe).
  std::optional<ByteRate> bandwidth{};
};

// drop_rate and reject_rate are two different promises: a drop is a node FAILING (its
// waiting room was full, or an external sink's provider answered with an error), a rejection is a
// rate limiter's POLICY working (ch. 4's 429). A design wants few drops anywhere and as many
// rejections as its limits call for, so each has its own target (report::Summary::drop_rate /
// reject_rate).
enum class SloMetric {
  p50_latency,
  p95_latency,
  p99_latency,
  drop_rate,
  reject_rate,
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
    {SloMetric::reject_rate, "reject_rate"},
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
  // `over: 1h` (book ch. 3: a non-functional requirement holds at every hour of the day, not on the
  // day's average): check the SLO on every window of this length, aligned from the end of warmup,
  // and let the worst window decide. Absent: the whole run after warmup, as before. The loader
  // keeps it between one step and duration − warmup.
  std::optional<SimTime> over{};
};

// ---- Scripted faults (spec §4.1) ----
//
// A fault is an event the scenario scripts under `faults:`: at a time on the run's clock something
// breaks, and (all but a cache flush) mends `for` later. Models the failure scenarios System Design
// Interview walks a design through: a web server or a database going offline (ch. 1, "if the
// master database goes offline, a slave database will be promoted"), a whole data center lost (ch.
// 1, "Data centers": GeoDNS sends its users to the healthy one), a cache restarted cold (ch. 1,
// "Considerations for using cache": a single cache server is a single point of failure), a slow or
// degraded dependency, and a flash crowd (ch. 4: the burst a rate limiter exists for). The model
// only carries the script; the loader fills it and the engine applies it. Captured: what breaks
// (one node, some of its replicas, or a datacenter), when, for how long, and by how much.
// Deliberately ignored: how the failure is detected (heartbeats, gossip: ch. 6), random failures
// (every fault is scripted, so a run stays deterministic for its seed), and network partitions
// (ch. 6's CAP trade-off, a phase 4 backlog item: `kind: partition` is refused).
enum class FaultKind : std::uint8_t {
  node_down,      // the node, or `replicas` of its replicas, serves nothing
  capacity,       // the node serves at `factor` (< 1) of its capacity: a degraded tier
  latency_spike,  // the node's service time grows by `extra`: a slow dependency
  cache_flush,    // a cache or CDN loses its contents at once and refills from its misses
  traffic_spike,  // a client sends `factor` times its rate: a flash crowd
  dc_down         // every node of `datacenter` is down
};

// Pattern: Table-driven method — the fault kind names, read by the loader (`kind:`) and printed by
// explain from this one table.
inline constexpr auto fault_kind_table = std::to_array<core::EnumName<FaultKind>>({
    {FaultKind::node_down, "node_down"},
    {FaultKind::capacity, "capacity"},
    {FaultKind::latency_spike, "latency_spike"},
    {FaultKind::cache_flush, "cache_flush"},
    {FaultKind::traffic_spike, "traffic_spike"},
    {FaultKind::dc_down, "dc_down"},
});

[[nodiscard]] constexpr std::string_view to_string(FaultKind k) {
  return core::name_in(fault_kind_table, k);
}
[[nodiscard]] constexpr std::optional<FaultKind> parse_fault_kind(std::string_view name) {
  return core::parse_in(fault_kind_table, name);
}

// One scripted fault. The members are the schema's keys, except `duration`, which is `for:` (a C++
// keyword), and `primary`, which is `replicas: primary`. Only the members of the fault's kind are
// set; the loader refuses a key another kind reads.
struct Fault {
  FaultKind kind{};
  SimTime at{};              // from the start of the run (t = 0 is midnight); inside the run
  SimTime duration{};        // the fault ends at `at + duration`; 0 for a cache_flush, an instant
  std::string node{};        // the node it acts on; empty for a dc_down
  std::string datacenter{};  // dc_down only
  std::optional<int> replicas{};  // node_down only: how many replicas go down; absent: all of them
  bool primary{false};  // node_down on a sql_db: the primary goes down (`replicas: primary`)
  std::optional<double> factor{};  // capacity: in (0, 1); traffic_spike: positive
  std::optional<SimTime> extra{};  // latency_spike: added to the node's service time
};

// How many copies of `node` a node_down counts (`Fault::replicas` is a number of them): its
// replicas, or for a kv_cluster its machines (`kv_nodes`; the loader refuses `replicas` on a
// cluster, and graph::apply_kv_ring makes the two equal only in the graph). The loader bounds
// `replicas` by it; engine::FaultSchedule divides by it.
[[nodiscard]] constexpr int copies_of(const NodeSpec& node) {
  if (node.type == NodeType::kv_cluster) return node.kv_nodes.value_or(node.replicas);
  return node.replicas;
}

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
  std::vector<Fault> faults{};  // in document order; empty: the run breaks nothing
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
static_assert(simsys::core::round_trips(simsys::model::lb_policy_table));
static_assert(simsys::model::node_type_table.back().value == simsys::model::NodeType::frontier);
static_assert(simsys::model::direction_table.back().value == simsys::model::Direction::inout);
static_assert(simsys::model::slo_metric_table.back().value == simsys::model::SloMetric::staleness);
static_assert(simsys::model::step_condition_table.back().value ==
              simsys::model::StepCondition::failure);
static_assert(simsys::model::step_mode_table.back().value == simsys::model::StepMode::parallel);
static_assert(simsys::model::lb_policy_table.back().value ==
              simsys::model::LbPolicy::consistent_hash);
static_assert(simsys::core::round_trips(simsys::model::limiter_algorithm_table));
static_assert(simsys::model::limiter_algorithm_table.back().value ==
              simsys::model::LimiterAlgorithm::sliding_counter);
static_assert(simsys::core::round_trips(simsys::model::cache_strategy_table));
static_assert(simsys::model::cache_strategy_table.back().value ==
              simsys::model::CacheStrategy::write_back);
static_assert(simsys::core::round_trips(simsys::model::id_algorithm_table));
static_assert(simsys::model::id_algorithm_table.back().value == simsys::model::IdAlgorithm::uuid);
static_assert(simsys::core::round_trips(simsys::model::kv_mode_table));
static_assert(simsys::model::kv_mode_table.back().value == simsys::model::KvMode::cp);
static_assert(simsys::core::round_trips(simsys::model::fault_kind_table));
static_assert(simsys::model::fault_kind_table.back().value == simsys::model::FaultKind::dc_down);
