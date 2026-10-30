// Module `simsys.graph`, implementation unit: the definitions behind graph.cppm — how
// Graph::build resolves the scenario (nodes, links, steps, routes, backends, consumer groups,
// hit ratios, request sizes, the topological order) and the searches it runs (call cycles by
// Tarjan's algorithm, the key walk and the size walk, the Che cache's keys). The interface keeps
// the types, the declarations and the inline one-liners; what a function does and why is
// documented here, with its definition. The split is a build-time decision: an edit to a body in
// this file recompiles this unit alone, while an edit to graph.cppm rebuilds every unit that
// imports simsys.graph (the engine, the reports, the session, the tests). (With CMake 3.31.
// CMake 3.28 rewrites every module map file after any edit and so recompiles the whole library
// regardless; the split still pays there, each importer reading a BMI half the size. Numbers:
// the phase 3 plan, Task 3.)
// C++20 global module fragment: `module;` opens the part of the unit before the module declaration,
// where the std headers are #included, so that their declarations stay in the global module
// instead of being attached to simsys.graph (src/core/core.cppm has the longer note).
module;
// No <chrono> although an implementation unit may include it (CLAUDE.md §3's first defect binds
// interfaces only): these definitions moved here from graph.cppm unchanged, and compare SimTime
// through count() as they did there.
#include <algorithm>
#include <cmath>
#include <compare>
#include <cstddef>
#include <expected>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// C++20 module implementation unit: `module simsys.graph;` (no `export`) makes this file part of
// the named module; it sees every declaration of graph.cppm, module-linkage ones (detail::)
// included, and exports nothing, like a .cpp behind a header but without re-parsing it.
module simsys.graph;

// C++20 `import`: the compiled interfaces of the modules the definitions use, instead of
// re-parsing their text as an #include would. graph.cppm imports the same ones but simsys.queueing,
// which only the definitions need.
import simsys.caching;
import simsys.core;
import simsys.defaults;
import simsys.fluid;
import simsys.model;
import simsys.queueing;

namespace simsys::graph {

namespace {

// A link carries demand from the node on its "sending" side: `from` for out/inout,
// `to` for a link written from the receiver's side with `dir: in`.
[[nodiscard]] bool carries_demand(const ResolvedLink& link, std::size_t from, std::size_t to) {
  // C++20 `using enum`: the enumerators of Direction in scope for this function only, so the
  // test below reads `in`, not `model::Direction::in` (replica_model_of in graph.cppm does the
  // same for LbPolicy); before C++20 each one had to be qualified or the enum left unscoped.
  using enum model::Direction;
  if (link.dir == in) return link.from == to && link.to == from;
  return link.from == from && link.to == to;
}

// The node a link delivers demand to when `sender` sends on it, if `sender` is its
// sending side.
[[nodiscard]] std::optional<std::size_t> demand_target(const ResolvedLink& link,
                                                       std::size_t sender) {
  if (link.dir == model::Direction::in) {
    if (link.to == sender) return link.from;
  } else if (link.from == sender) {
    return link.to;
  }
  return std::nullopt;
}

// An empty ops list means "all ops" (schema: links.ops).
[[nodiscard]] bool carries_op(const ResolvedLink& link, std::string_view op) {
  // C++23 std::ranges::contains: "is this value in the range?" in one call, replacing
  // `std::find(begin, end, value) != end`.
  return link.ops.empty() || std::ranges::contains(link.ops, op);
}

[[nodiscard]] std::string joined(const std::vector<std::string>& words, std::string_view sep) {
  std::string text;
  for (const auto& word : words) {
    if (!text.empty()) text += sep;
    text += word;
  }
  return text;
}

// How many whole steps a hop of `one_way` delays its flow (spec §3.4, refined): the
// nearest whole number of steps, halves rounded away from zero. A hop shorter than half a
// step only adds latency (the reply path accounts for it) and its flow passes through in
// the same step; a 75 ms cross-datacenter hop (half the book's 150 ms round trip) at
// 100 ms steps arrives one step late; 250 ms arrives three steps late. Rounding to the
// nearest step keeps the average arrival time honest: ceil alone would delay every 101 ms
// hop by two steps. Integer arithmetic on the nanosecond counts, so the count is exact
// for any duration (a double ratio loses digits past 2^53). Needs one_way >= 0, step > 0.
[[nodiscard]] SimTime::rep whole_delay_steps(SimTime one_way, SimTime step) {
  const auto whole = one_way.count() / step.count();
  const auto rest = one_way.count() % step.count();
  return rest >= step.count() - rest ? whole + 1 : whole;  // rest >= half a step: round up
}

// Graph::build also takes scenarios written in code (tests, generators), which the
// loader never checked, so it rejects the link numbers the loader would have rejected.
[[nodiscard]] std::optional<EngineError> invalid_link_numbers(const model::LinkSpec& spec) {
  const std::string name = "link " + spec.from + " -> " + spec.to + ": ";
  if (spec.latency && spec.latency->count() < 0) {
    return EngineError{name + "latency must not be negative"};
  }
  if (!std::isfinite(spec.weight) || spec.weight < 0) {
    return EngineError{name + "weight must be a finite non-negative number"};
  }
  return std::nullopt;
}

// Depth-first search roots for Graph::order_nodes: the clients first, then every other
// node, each group in index order. Starting from the request sources makes the back edge
// found in a loop the one that closes it as seen from the users (chat -> queue -> sync
// workers -> back to chat: sync -> chat is the feedback link). When ONE client enters the
// loop, that choice does not depend on the order the scenario file lists the nodes in.
// When several clients enter the same loop at different nodes, the first client in file
// order decides where the walk enters it, and so which link is cut; no current scenario
// has such a loop. The other nodes are roots too, so nodes no client reaches are still
// ordered.
// C++20 std::span<const T>: a non-owning view of contiguous elements (pointer + size), replacing a
// `const std::vector<T>&` parameter: the helpers here take what Graph's accessors hand out (spans)
// and a vector's elements alike, without naming the container.
[[nodiscard]] std::vector<std::size_t> dfs_roots(std::span<const ResolvedNode> nodes) {
  std::vector<std::size_t> roots;
  roots.reserve(nodes.size());
  for (const bool want_clients : {true, false}) {
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      if ((nodes[i].spec.type == model::NodeType::client) == want_clients) roots.push_back(i);
    }
  }
  return roots;
}

// A windowed algorithm moves its counters at window boundaries, and the fluid step sees a
// boundary only where a step starts: a 150 ms window stepped every 100 ms would reset at
// 100 ms and 200 ms, a different window with a different limit x window from the one the
// scenario states. So its window (stated, or the default 1 s) must hold a whole number of
// steps, at least one. The buckets use the window only to size their default burst.
[[nodiscard]] std::optional<EngineError> window_off_the_step_grid(const model::NodeSpec& spec,
                                                                  SimTime step) {
  const auto algorithm = spec.algorithm.value_or(model::LimiterAlgorithm::token_bucket);
  if (!model::is_windowed(algorithm)) return std::nullopt;
  const SimTime window = model::limiter_window(spec);
  // On the nanosecond counts: this interface does not include <chrono> (CLAUDE.md §3), so the
  // duration operators are out of reach, as in whole_delay_steps.
  if (window.count() >= step.count() && window.count() % step.count() == 0) return std::nullopt;
  return EngineError{"node '" + spec.id +
                     "': window must be a positive whole number of steps (window " +
                     describe_duration(window) + ", step " + describe_duration(step) + ")"};
}

// Graph::build also takes rate limiters written in code, which the loader never checked:
// the same rules as config's read_limiter_* and check_leaky_bucket_keys (a limit, positive;
// burst and window positive; a burst only on a bucket; a leaky bucket states nothing but
// limit and burst, not a queue's own numbers). The loader does not know the step's role, so
// the rule that a windowed limiter's window is a whole number of steps lives only here.
[[nodiscard]] std::optional<EngineError> invalid_limiter(const model::NodeSpec& spec,
                                                         SimTime step) {
  const std::string name = "node '" + spec.id + "': ";
  if (!spec.limit) return EngineError{name + "rate_limiter needs a limit"};
  if (!(spec.limit->per_second > 0) || !std::isfinite(spec.limit->per_second)) {
    return EngineError{name + "limit must be positive"};
  }
  if (spec.burst && *spec.burst <= 0) return EngineError{name + "burst must be a positive integer"};
  if (spec.window && spec.window->count() <= 0)
    return EngineError{name + "window must be positive"};
  const auto algorithm = spec.algorithm.value_or(model::LimiterAlgorithm::token_bucket);
  if (spec.burst && model::is_windowed(algorithm)) {
    return EngineError{name + "burst applies only to token_bucket and leaky_bucket"};
  }
  const bool restates_queue = spec.capacity || spec.latency || spec.advanced.queue_limit ||
                              spec.advanced.concurrency || spec.advanced.service_variability;
  if (algorithm == model::LimiterAlgorithm::leaky_bucket && restates_queue) {
    return EngineError{name +
                       "leaky_bucket takes limit and burst; capacity, latency, concurrency, "
                       "queue_limit and service_variability are not valid"};
  }
  return window_off_the_step_grid(spec, step);
}

[[nodiscard]] bool is_cache(const model::NodeSpec& spec) {
  return spec.type == model::NodeType::cache;
}

// The cache strategy (caching practice's word, not the GoF Strategy pattern: a closed
// enum tested here, no class per strategy) is read in this one place, which says which ops each
// strategy sends to the store (its backend hop, as the same op):
// a read under read_through (its misses), a write under write_through, write_around and
// write_back (every one; write_back's later and asynchronously), nothing else. Graph::
// backend_fraction (how much), the key walk (detail::add_backend_hop) and the then-step check
// below (invalid_then_steps) all ask it.
// A CDN behaves as a read_through cache here (fetches_read_misses), and no write reaches it
// (Graph::check_reads_only).
[[nodiscard]] bool sends_to_store(const model::NodeSpec& cache, std::string_view op) {
  // `using enum` (see replica_model_of): the strategies by their short names.
  using enum model::CacheStrategy;
  if (!is_write(cache, op)) return fetches_read_misses(cache);
  return cache.strategy == write_through || cache.strategy == write_around ||
         cache.strategy == write_back;
}

// A write reaches the store under the sender's op name (a cache's backend hop passes its op on
// unchanged, a limiter's counter is `counter`), and the store decides alone whether that op is a
// write (model::write_ops_of). A store that counted the sender's write as a read would serve it
// as one: a SQL database from any replica at cost 1, its primary never told (book ch. 1,
// "database replication": every write goes to the primary); a cache from its hit ratio. So a SQL
// database or a cache in the store's place must list the op among its writes; the message names
// the list to fix: "node 'cache_servers': sends write 'set' to 'db', which counts 'set' as a
// read; add it to db's write_ops". The other node types do not tell reads from writes, so they
// cannot disagree (and a CDN refuses every write: Graph::check_reads_only).
[[nodiscard]] std::optional<EngineError> write_counted_as_read(const model::NodeSpec& sender,
                                                               std::string_view op,
                                                               const model::NodeSpec& store) {
  const bool tells_writes = store.type == model::NodeType::sql_db || is_cache(store);
  if (!tells_writes || is_write(store, op)) return std::nullopt;
  const std::string quoted = "'" + std::string{op} + "'";
  return EngineError{"node '" + sender.id + "': sends write " + quoted + " to '" + store.id +
                     "', which counts " + quoted + " as a read; add it to " + store.id +
                     "'s write_ops"};
}

// Has the node a `then` step taken on `condition`?
[[nodiscard]] bool has_then_step_on(const model::NodeSpec& spec, model::StepCondition condition) {
  // C++20 ranges algorithm: std::ranges::any_of takes the container itself instead of a
  // begin/end pair (and a projection if wanted), replacing std::any_of(v.begin(), v.end(), p).
  return std::ranges::any_of(spec.then,
                             [&](const model::Step& step) { return step.on == condition; });
}

// Has the node a `then` step in parallel mode?
[[nodiscard]] bool has_parallel_then_step(const model::NodeSpec& spec) {
  return std::ranges::any_of(
      spec.then, [](const model::Step& step) { return step.mode == model::StepMode::parallel; });
}

// Does the cache forward its own misses (a `then` step on miss)? Then nothing after it may fetch
// them again (Graph::invalid_miss_step). (A CDN has no `then` steps: invalid_cdn.)
[[nodiscard]] bool forwards_own_misses(const model::NodeSpec& spec) {
  return has_then_step_on(spec, model::StepCondition::miss);
}

// Do the `then` steps of a node of `type` carry some ops that arrive and not others
// (Graph::then_fraction)? A cache's do (a miss step carries no write) and a blob store's do (an
// event fires for a write only); every other type's unconditional steps fire whatever arrives.
[[nodiscard]] bool then_steps_follow_the_op(model::NodeType type) {
  return hits_or_misses(type) || type == model::NodeType::blob_store;
}

// Would `step`, a `then` step of `cache`, send its store the very request its backend hop sends
// (the same node, an op sends_to_store says the hop carries)? Every such request would reach the
// store twice, and under write_back the step would be taken for the asynchronous flush.
[[nodiscard]] bool duplicates_backend_hop(const model::NodeSpec& cache, const model::Step& step) {
  return cache.backend && step.to == *cache.backend && sends_to_store(cache, step.op);
}

// A cache's `then` steps (spec §18 entry 5), checked against what the cache already does:
//  - a step on miss beside a read_through backend: both would fetch the miss;
//  - a step on miss in parallel: a miss waits for its then steps (the cache answers it only once
//    the next tier has), so the step cannot be timed beside the lookup, as a service's miss step
//    cannot (Graph::invalid_miss_step);
//  - a step that duplicates the backend hop (duplicates_backend_hop);
//  - a step on failure: an `on: failure` step reports on a step of an op's step list, the nearest
//    unconditional one before it (Graph::resolve_service_hop), and a cache's `then` list follows no
//    such step. Phase 2 kept it and never fired it; refused since failure steps fire (spec §5).
[[nodiscard]] std::optional<EngineError> invalid_then_steps(const model::NodeSpec& spec) {
  const std::string name = "node '" + spec.id + "': ";
  for (const model::Step& step : spec.then) {
    if (step.on == model::StepCondition::failure) {
      return EngineError{name +
                         "a then step on failure is not modelled at a cache (an on: failure step "
                         "reports on a step of an op's step list)"};
    }
    const bool on_miss = step.on == model::StepCondition::miss;
    if (on_miss && fetches_read_misses(spec)) {
      return EngineError{name +
                         "a then step on miss and a read_through backend both fetch the miss"};
    }
    if (on_miss && step.mode == model::StepMode::parallel) {
      return EngineError{name + "a then step on miss must be sequential (a miss waits for it)"};
    }
    if (duplicates_backend_hop(spec, step)) {
      return EngineError{name + "then step to '" + step.to + "' duplicates its backend hop"};
    }
  }
  return std::nullopt;
}

// Do the `mode: parallel` steps of one step list sit side by side? All of them form one group (they
// start together, the slowest is waited for), so the response is the same wherever they sit, but a
// request's later steps are thinned from the point where the group starts
// (behaviours::detail::HopBlocks::forward): [a ∥, s, b ∥] would send s only the requests b
// answered, though s runs before b. Listing the group where it runs removes the ambiguity. A plain
// loop: the list is short and the state is two flags.
[[nodiscard]] bool parallel_steps_adjacent(std::span<const model::Step> steps) {
  bool group_started = false;
  bool group_ended = false;
  for (const model::Step& step : steps) {
    const bool parallel = step.mode == model::StepMode::parallel;
    if (parallel && group_ended) return false;
    if (!parallel && group_started) group_ended = true;
    if (parallel) group_started = true;
  }
  return true;
}

// The first step list of `spec` whose parallel steps are not adjacent (parallel_steps_adjacent),
// an op's steps or its `then` steps, if there is one.
[[nodiscard]] std::optional<EngineError> nonadjacent_parallel_steps(const model::NodeSpec& spec) {
  const std::string name = "node '" + spec.id + "': ";
  for (const auto& [op, steps] : spec.ops) {
    if (!parallel_steps_adjacent(steps)) {
      return EngineError{name + "the parallel steps of op '" + op +
                         "' must be adjacent (they form one group)"};
    }
  }
  if (!parallel_steps_adjacent(spec.then)) {
    return EngineError{name + "the parallel then steps must be adjacent (they form one group)"};
  }
  return std::nullopt;
}

// A fixed hit ratio is a probability (config's read_hit_ratio), for a cache and a CDN alike.
[[nodiscard]] std::optional<EngineError> invalid_fixed_hit_ratio(const model::NodeSpec& spec) {
  if (!spec.hit_ratio || !spec.hit_ratio->fixed) return std::nullopt;
  const double h = *spec.hit_ratio->fixed;
  if (h >= 0.0 && h <= 1.0) return std::nullopt;
  return EngineError{"node '" + spec.id + "': hit_ratio.fixed must be between 0 and 1"};
}

// Graph::build also takes caches written in code, which the loader never checked: a fixed
// hit ratio is a probability, a strategy in which the cache calls the store needs to know
// which node that is, and one in which it does not (cache_aside) must not name one: it would
// never be called (config's read_hit_ratio and read_cache_strategy). A cache with no
// hit ratio at all builds (explain can show it); the engine refuses to run it. Its `then` steps
// must agree with its strategy and its backend (invalid_then_steps).
[[nodiscard]] std::optional<EngineError> invalid_cache(const model::NodeSpec& spec) {
  const std::string name = "node '" + spec.id + "': ";
  if (auto invalid = invalid_fixed_hit_ratio(spec)) return invalid;
  if (model::needs_backend(spec.strategy) && !spec.backend) {
    return EngineError{name + "strategy " + std::string{model::to_string(spec.strategy)} +
                       " needs a backend"};
  }
  if (!model::needs_backend(spec.strategy) && spec.backend) {
    return EngineError{name + "backend is unused with strategy " +
                       std::string{model::to_string(spec.strategy)}};
  }
  return invalid_then_steps(spec);
}

// Graph::build also takes CDNs written in code, which the loader never checked (config's
// read_cdn_keys): a fixed hit ratio is a probability, and a CDN needs an origin to send its
// misses to, other than itself (a miss it asked itself for could never be answered; whether the
// origin is a server is connect_store's check). The loader gives a CDN no `then` steps, no
// `strategy` and no `backend`, and none may be written in code: the CDN fetches its own misses,
// so a then step on miss would fetch them twice and an unconditional one has no place in an
// edge cache's model; it is read_through towards its origin by definition, so a strategy would
// contradict it (write_back would even excuse the origin hop from check_backend_calls),
// and its store is its origin, so a backend would be a second one.
[[nodiscard]] std::optional<EngineError> invalid_cdn(const model::NodeSpec& spec) {
  const std::string name = "node '" + spec.id + "': ";
  if (auto invalid = invalid_fixed_hit_ratio(spec)) return invalid;
  if (!spec.origin) return EngineError{name + "a cdn needs an origin"};
  if (*spec.origin == spec.id) return EngineError{name + "a cdn cannot be its own origin"};
  if (!spec.then.empty()) {
    return EngineError{name + "a cdn has no then steps (it fetches its misses from its origin)"};
  }
  if (spec.strategy != model::CacheStrategy::cache_aside) {  // NodeSpec's default: none stated
    return EngineError{name + "a cdn has no strategy (it reads through to its origin)"};
  }
  if (spec.backend) {
    return EngineError{name + "a cdn has no backend (its origin is the store behind it)"};
  }
  return std::nullopt;
}

// A database answers every query itself and calls nobody (behaviours::SqlDbBehaviour), so a link
// that carries demand out of one (from it, or written from the other end with `dir: in`) would
// carry nothing: refused, as the loader refuses it (config's check_link_sender), with its message.
[[nodiscard]] std::optional<EngineError> invalid_link_from_database(const model::LinkSpec& link,
                                                                    const model::NodeSpec& from,
                                                                    const model::NodeSpec& to) {
  const auto& sender = link.dir == model::Direction::in ? to : from;
  if (sender.type != model::NodeType::sql_db) return std::nullopt;
  return EngineError{"node '" + sender.id + "': a database calls nobody"};
}

// Graph::build also takes databases written in code: their `shards`, `pool` and
// `replication_lag` obey the loader's rules (config's read_sql_keys), with its messages.
[[nodiscard]] std::optional<EngineError> invalid_database(const model::NodeSpec& spec) {
  const std::string name = "node '" + spec.id + "': ";
  if (spec.shards && *spec.shards < 1)
    return EngineError{name + "shards must be a positive integer"};
  if (spec.pool && *spec.pool < 1) return EngineError{name + "pool must be a positive integer"};
  if (spec.pool && *spec.pool > defaults::max_concurrency) {
    return EngineError{name + "pool must be at most " + std::to_string(defaults::max_concurrency)};
  }
  if (spec.pool && spec.advanced.concurrency) {
    return EngineError{name +
                       "pool sets c and caps the rate by Little's law; drop advanced.concurrency"};
  }
  if (spec.replication_lag && spec.replication_lag->count() < 0) {
    return EngineError{name + "replication_lag must not be negative"};
  }
  return std::nullopt;
}

// What a database's own keys do to its resolved numbers (spec §18 entry 6): `pool` is the
// connection pool, how many queries one copy (primary or replica alike) has in service at once. The
// rates below are capacity units, like `capacity`: a query holds its connection for its cost × the
// service time, so a write at the default cost of 5 holds one five times as long as a read, and the
// pool caps units per second, not requests. A copy already has a concurrency, the c Little's law
// implies from its capacity × service time (defaults::implied_concurrency: 5k/s × 10 ms keeps 50
// queries in service). A pool below it leaves connections, not the disk, as the limit:
//   c            = min(pool, implied c)
//   service rate = min(capacity, pool / service time)
// so 4 connections of 10 ms queries serve at most 400/s however fast the disk. A pool at or above
// the implied c changes nothing: more connections never make the disk faster. The two lines
// keep the queue one consistent machine (fluid::Capacity: the drain uses the rate, Erlang C
// uses c and the service time, and rate = c / service time when the pool binds).
// (Its `write_ops` need nothing here: model::write_ops_of gives an absent list the database
// default, default_sql_write_ops, wherever it is read.)
void apply_database_keys(ResolvedNode& node) {
  if (!node.spec.pool) return;
  const int pool = *node.spec.pool;
  auto& capacity = node.capacity;
  capacity.concurrency = std::min(pool, capacity.concurrency);
  // Little's law: concurrency in service = throughput × service time, so `pool` queries in
  // service for service_time_s each leave at pool / service_time_s per second. (A zero service
  // time gives +inf, which the min turns back into the capacity.)
  const double pool_rate = static_cast<double>(pool) / capacity.service_time_s;
  capacity.service_rate = Rate{std::min(capacity.service_rate.per_second, pool_rate)};
}

// A leaky bucket (ch. 4) IS a queue: requests join a FIFO bucket that leaks at the fixed
// rate `limit`, and only a full bucket (`burst` waiting) turns requests away. So the
// limiter's own queue becomes the bucket, modelled as ONE leak slot:
//   service rate  = limit          (one request leaves every 1/limit seconds)
//   service time  = 1 / limit      (the slot is busy for exactly that interval)
//   concurrency   = 1, service_variability = 0   (one server, deterministic service)
//   queue_limit   = burst          (the bucket's room)
// so below the limit a request waits as in an M/D/1 queue (Poisson arrivals, deterministic
// leak): the Pollaczek-Khinchine wait, half the M/M/1 wait (fluid::FluidQueue::sojourn's
// Allen-Cunneen factor (1 + 0) / 2), plus the 1/limit leak interval itself. Above it the
// backlog fills and the overflow is refused (the engine reports it as `rejected`, the
// bucket's policy, not as a drop). `limit` and `burst` are the limiter's TOTALS, so with N
// replicas each copy is one slot of limit/N with room burst/N (the engine models one copy,
// see engine::Simulation::make_server). The loader and invalid_limiter refuse a `capacity`,
// `latency`, concurrency, queue limit or service variability on a leaky bucket: they would
// restate the bucket (the leak is regular by definition: variability 0). The variability of
// ARRIVALS (`advanced.burstiness`) is still the scenario's to set.
[[nodiscard]] fluid::Capacity leaky_bucket_capacity(const model::NodeSpec& spec,
                                                    fluid::Capacity capacity) {
  const double replicas = spec.replicas;
  const double leak_per_copy = spec.limit->per_second / replicas;
  capacity.service_rate = Rate{leak_per_copy};
  capacity.service_time_s = 1.0 / leak_per_copy;
  capacity.concurrency = 1;
  capacity.service_variability = 0.0;
  capacity.queue_limit = model::limiter_burst(spec) / replicas;
  return capacity;
}

[[nodiscard]] bool is_leaky_bucket(const model::NodeSpec& spec) {
  return spec.type == model::NodeType::rate_limiter &&
         spec.algorithm == model::LimiterAlgorithm::leaky_bucket;
}

// Graph::build also takes id generators written in code, which the loader never checked (config's
// read_id_generator_keys): an algorithm is required, and a ticket server takes one replica, with
// the loader's messages. Ch. 7's ticket server is ONE auto-increment database: a second copy
// would hand out the same ids, and splitting the sequence between copies (odd/even) is the
// chapter's multi-master design, which it rejects for not scaling and not surviving a server
// added or removed. One machine is also the design's flaw the chapter names, a single point of
// failure and a bottleneck at one database's write rate. For the single point of failure the
// chapter itself suggests several ticket servers, at the cost of keeping them in sync; the model
// has no such mode (no synchronisation cost to charge, and its failures are phase 3's), so a
// scenario that needs more ids per second than one database writes turns to snowflake.
[[nodiscard]] std::optional<EngineError> invalid_id_generator(const model::NodeSpec& spec) {
  const std::string name = "node '" + spec.id + "': ";
  if (!spec.id_algorithm) {
    return EngineError{name + "id_generator needs algorithm: snowflake, ticket_server or uuid"};
  }
  if (*spec.id_algorithm == model::IdAlgorithm::ticket_server && spec.replicas != 1) {
    return EngineError{name + std::string{model::ticket_server_replicas_rule}};
  }
  return std::nullopt;
}

// A blob store's `then` steps are event notifications (spec §19 entry 7; S3 event notifications,
// book ch. 14's "transcoding starts when the upload completes"): each fires for every write the
// store serves (Graph::then_fraction). Three step keys would ask for something an event never does,
// so the scenario is told what the steps do instead of being run without them:
//  - `on: miss`: a blob store never misses (only a cache can take a miss step);
//  - `on: failure`: a failure step reports on a step of an op's step list (the nearest
//    unconditional one before it, Graph::resolve_service_hop), and an event follows no such step
//    (it fires for every write stored), so the step would have nothing to report on;
//  - `mode: parallel`: a mode orders the calls a request waits for, and nobody waits for an
//    event. (`mode: sequential` is the model's default, so only the loader, which sees the key
//    written, can refuse it: config's read_blob_store_keys.)
[[nodiscard]] std::optional<EngineError> invalid_blob_store(const model::NodeSpec& spec) {
  const auto refused = [&](std::string_view why) {
    return EngineError{"node '" + spec.id + "': a blob store's then steps " + std::string{why}};
  };
  if (has_then_step_on(spec, model::StepCondition::miss)) {
    return refused("fire for every write it stores (on: miss needs a cache)");
  }
  if (has_then_step_on(spec, model::StepCondition::failure)) {
    return refused(
        "fire for every write it stores (on: failure: an event follows no step to report on; "
        "failure steps fire in an op's step list)");
  }
  if (has_parallel_then_step(spec)) {
    return refused(
        "are event notifications nobody waits for (mode orders the calls a request waits for)");
  }
  return std::nullopt;
}

// A worker pool's `then` steps are the work each job it DRAINS hands on (spec §19 entry 5; book ch.
// 14: a transcoder writes its renditions and publishes a completion message), every step for every
// job (Graph::then_fraction is 1, whatever op the queue brought). Two step keys ask for something a
// job never does, so they are refused in the blob store's words (invalid_blob_store):
//  - `on: miss`: a pool has no lookup to miss (only a cache can take a miss step);
//  - `on: failure`: a pool's `then` steps follow no step a failure step could report on (each
//    fires for every job drained; a failed hand-off is the target's drop), as a blob store's.
// A `mode: parallel` group is fine, unlike a blob store's: nobody waits for a pool (the hop from
// its queue is asynchronous), so the mode changes no one's latency, and a scenario may group a
// transcoder's renditions beside its completion message as the book draws them.
[[nodiscard]] std::optional<EngineError> invalid_worker_then_steps(const model::NodeSpec& spec) {
  const auto refused = [&](std::string_view why) {
    return EngineError{"node '" + spec.id +
                       "': a worker pool's then steps fire for every job it drains " +
                       std::string{why}};
  };
  if (has_then_step_on(spec, model::StepCondition::miss)) {
    return refused("(on: miss needs a cache)");
  }
  if (has_then_step_on(spec, model::StepCondition::failure)) {
    return refused(
        "(on: failure: a job's then steps follow no step to report on; failure steps fire in an "
        "op's step list)");
  }
  return std::nullopt;
}

// Graph::build also takes worker pools written in code, which the loader never checked (config's
// read_worker_pool_keys): `workers` positive and at most max_concurrency, `cost` positive, with the
// loader's messages. (A pool without them runs on the type's numbers: the loader requires them, a
// test may leave them out.) Its `then` steps are checked as well (invalid_worker_then_steps).
[[nodiscard]] std::optional<EngineError> invalid_worker_pool(const model::NodeSpec& spec) {
  const std::string name = "node '" + spec.id + "': ";
  if (spec.workers && *spec.workers < 1) {
    return EngineError{name + "workers must be a positive integer"};
  }
  if (spec.workers && *spec.workers > defaults::max_concurrency) {
    return EngineError{name + "workers must be at most " +
                       std::to_string(defaults::max_concurrency)};
  }
  if (spec.job_cost && spec.job_cost->count() <= 0) {
    return EngineError{name + "cost must be positive"};
  }
  return invalid_worker_then_steps(spec);
}

// "a service", "an external_sink": a node type with its article, as the loader's messages say it
// (config's type_with_article).
[[nodiscard]] std::string with_article(model::NodeType type) {
  const auto name = model::to_string(type);
  // C++23 std::string_view::contains: "is this character among those?" in one call, replacing
  // `find(c) != npos`.
  const bool vowel = std::string_view{"aeiou"}.contains(name.front());
  return (vowel ? "an " : "a ") + std::string{name};
}

// A notification fanout's `then` steps are the publishes each routed notification makes once the
// dedup step has let it through (phase 3 spec §8.3; book ch. 10), every step for 1 − dedup of what
// it serves (Graph::then_fraction). Graph::build also takes fanouts written in code, which the
// loader never checked (config's read_notification_fanout_keys): `dedup` in [0, 1), with the
// loader's message. Two step keys ask for something a publish never does, refused in the worker
// pool's words (invalid_worker_then_steps):
//  - `on: miss`: a fanout has no lookup to miss (only a cache can take a miss step);
//  - `on: failure`: its `then` steps follow no step a failure step could report on.
// A `mode: parallel` group is fine: the fanout publishes to its channels at once and waits for the
// slowest append. Whether `channels` and the steps name the same queues is checked once the nodes
// are resolved (Graph::check_fanout_channels).
[[nodiscard]] std::optional<EngineError> invalid_notification_fanout(const model::NodeSpec& spec) {
  const std::string name = "node '" + spec.id + "': ";
  if (!(spec.dedup >= 0.0 && spec.dedup < 1.0)) {
    return EngineError{name + "dedup must be between 0 and 1"};
  }
  const auto refused = [&](std::string_view why) {
    return EngineError{name + "a fanout's then steps fire for every notification it routes " +
                       std::string{why}};
  };
  if (has_then_step_on(spec, model::StepCondition::miss)) {
    return refused("(on: miss needs a cache)");
  }
  if (has_then_step_on(spec, model::StepCondition::failure)) {
    return refused(
        "(on: failure: a notification's then steps follow no step to report on; failure steps "
        "fire in an op's step list)");
  }
  return std::nullopt;
}

// Graph::build also takes message queues written in code: `retention` and `partitions` obey the
// loader's rules (config's read_message_queue_keys), with its messages.
[[nodiscard]] std::optional<EngineError> invalid_message_queue(const model::NodeSpec& spec) {
  const std::string name = "node '" + spec.id + "': ";
  if (spec.retention && spec.retention->count() <= 0) {
    return EngineError{name + "retention must be positive"};
  }
  if (spec.partitions && *spec.partitions < 1) {
    return EngineError{name + "partitions must be a positive integer"};
  }
  return std::nullopt;
}

// "write_quorum must be at most replication (4 > 3)", the loader's words (config's
// check_at_most).
[[nodiscard]] std::optional<EngineError> above(const std::string& name, std::string_view what,
                                               int value, std::string_view limit_name, int limit) {
  if (value <= limit) return std::nullopt;
  return EngineError{name + std::string{what} + " must be at most " + std::string{limit_name} +
                     " (" + std::to_string(value) + " > " + std::to_string(limit) + ")"};
}

// "nodes must be a positive integer" for a stated count below one, the loader's words.
[[nodiscard]] std::optional<EngineError> not_positive(const std::string& name,
                                                      std::string_view what,
                                                      const std::optional<int>& count) {
  if (!count || *count >= 1) return std::nullopt;
  return EngineError{name + std::string{what} + " must be a positive integer"};
}

// Graph::build also takes kv_clusters written in code, which the loader never checked (config's
// read_kv_cluster_keys): `nodes` present, every count positive, N <= nodes, W and R <= N, and no
// `replicas` (the graph sets it to `nodes`: apply_kv_ring), each in the loader's words.
[[nodiscard]] std::optional<EngineError> invalid_kv_cluster(const model::NodeSpec& spec) {
  const std::string name = "node '" + spec.id + "': ";
  if (!spec.kv_nodes) return EngineError{name + "kv_cluster needs nodes"};
  if (spec.replicas != 1) {
    return EngineError{name + "a kv_cluster's machines are nodes; replicas is not a key here"};
  }
  for (const auto& [what, count] :
       {std::pair{"nodes", spec.kv_nodes}, std::pair{"vnodes", spec.vnodes},
        std::pair{"replication", spec.replication}, std::pair{"write_quorum", spec.write_quorum},
        std::pair{"read_quorum", spec.read_quorum}}) {
    if (auto invalid = not_positive(name, what, count)) return invalid;
  }
  const int replication = model::kv_replication(spec);
  if (auto invalid = above(name, "replication", replication, "nodes", *spec.kv_nodes)) {
    return invalid;
  }
  if (auto invalid =
          above(name, "write_quorum", model::kv_write_quorum(spec), "replication", replication)) {
    return invalid;
  }
  return above(name, "read_quorum", model::kv_read_quorum(spec), "replication", replication);
}

// A consistent-hash ring's imbalance k: how many times the average share of the keys the HOTTEST of
// `machines` holds (book ch. 5), the one heuristic shared by a consistent_hash balancer's ring (one
// copy of each key: `copies_per_key` 1) and a kv_cluster's (N copies on N successive machines):
//   k = min(1 + z_M / sqrt(N v), M / N)
//   M  the machines (servers) on the ring, N the copies of each key, v the virtual nodes each
//   1 / sqrt(N v)  the relative spread of one machine's share: it is the sum of N v arc lengths
//                  (v positions, each owning the arc before it, once per copy), and a sum of n
//                  roughly independent arcs spreads like 1 / sqrt(n) (the Dynamo/Chord argument;
//                  the book's "about 10 % at 100 virtual nodes")
//   z_M  the expected maximum of M standard normals (queueing::expected_normal_maximum, Blom's
//        approximation): the hottest of M shares sits that many spreads above the average, and
//        that grows with M (0 for one machine, 1.05 for 4, 1.77 for 16, 2.58 for 128)
//   M / N  the cap: a machine holds a key at most once, so it is sent at most every request
// A heuristic, not a theorem (shares are not quite normal nor independent: they sum to N), checked
// against Monte Carlo rings (docs/study/numerics.md, section 8): N = 3, v = 150, the expected
// hottest over the average is 1.073, 1.091 and 1.133 for 8, 16 and 128 machines where k gives
// 1.068, 1.083 and 1.122; one copy over 4 servers at v = 150, 1.086 against 1.086. Its limit:
// within about 1 % at v >= 100, but at v = 1 a machine's share is a few whole arcs, far from
// normal, and k is off by up to ±16 % (one copy over 2 machines: exactly 1.5 against k = 1.589;
// three copies over 8 machines: 1.616 against 1.828; over 128: 2.943 against 2.492). One formula is
// kept for every v: v = 1 is an experiment that shows an uneven ring, and its verdicts hold either
// way. The old 1 + 1 / sqrt(v) ignored M (it is the k of one standard deviation) and understated
// the hot spot beyond about 16 machines.
[[nodiscard]] double ring_imbalance(int machines, int copies_per_key, int vnodes) {
  if (machines <= 1) return 1.0;  // one machine carries exactly the average
  const double spread = 1.0 / std::sqrt(static_cast<double>(copies_per_key) * vnodes);
  return std::min(1.0 + queueing::expected_normal_maximum(machines) * spread,
                  static_cast<double>(machines) / copies_per_key);
}

// A kv_cluster's machines on their ring (spec §19 entry 9; book ch. 5's ring, ch. 6's replication):
// its copies are its `nodes`, each of the stated (per-node) capacity, so `spec.replicas` becomes
// `nodes` (the scenario may not state `replicas` here: the loader refuses it) and every reader of a
// node's copies (explain's total capacity, the engine's copy groups) counts machines. With N <
// nodes each machine holds the keys of the arcs before its virtual nodes, N times over (it is one
// of the N successive replicas of each), so the ring is uneven: the hottest carries k =
// ring_imbalance(nodes, N, vnodes) times the average, as hot and cool copy groups in the engine
// (add_hot_and_cool_copies). k is at most nodes / N: a machine is at most one of a key's N
// replicas, so it is sent at most every request, N × rate / nodes × k <= rate. With N = nodes every
// machine holds every key: k = 1, identical independent copies.
void apply_kv_ring(ResolvedNode& node) {
  const auto& spec = node.spec;
  const int machines = *spec.kv_nodes;
  const int replication = model::kv_replication(spec);
  node.spec.replicas = machines;
  if (replication >= machines) return;  // every node holds every key: no hot node
  node.imbalance =
      ring_imbalance(machines, replication, spec.vnodes.value_or(model::default_vnodes));
}
// What a worker pool may consume (spec §19 entry 4): a message queue, or a URL frontier, which is a
// queue of URLs (book ch. 9: the web crawler's downloaders take URLs from it; phase 3 Task 18 runs
// it, behaviours::FrontierBehaviour).
[[nodiscard]] bool can_be_consumed(model::NodeType type) {
  return type == model::NodeType::message_queue || type == model::NodeType::frontier;
}

// Graph::build also takes clients written in code: their `keys` obey the loader's rules
// (config's read_client_keys), which simsys.caching's Popularity factories also assume (they
// throw on a negative exponent or an empty key space: programmer errors there).
[[nodiscard]] std::optional<EngineError> invalid_client_keys(const model::NodeSpec& spec) {
  if (!spec.workload || !spec.workload->keys) return std::nullopt;
  const auto& keys = *spec.workload->keys;
  const std::string name = "node '" + spec.id + "': ";
  if (keys.zipf && !(*keys.zipf >= 0.0 && std::isfinite(*keys.zipf))) {
    return EngineError{name + "keys.zipf must be a finite non-negative number"};
  }
  if (!(keys.space >= 1.0 && std::isfinite(keys.space))) {
    return EngineError{name + "keys.space must be a positive count"};
  }
  return std::nullopt;
}

// What the graph refuses in a node on its own, before any link is resolved: one check per type
// that has one (a client's keys, a limiter's algorithm against the step, a cache's, a CDN's, a
// database's, ... settings); nothing for the other types or a node that passes.
[[nodiscard]] std::optional<EngineError> invalid_node(const model::NodeSpec& spec, SimTime step) {
  using enum model::NodeType;  // `using enum` (see replica_model_of): the types by their names
  switch (spec.type) {
    case client:
      return invalid_client_keys(spec);
    case rate_limiter:
      return invalid_limiter(spec, step);
    case cache:
      return invalid_cache(spec);
    case cdn:
      return invalid_cdn(spec);
    case sql_db:
      return invalid_database(spec);
    case id_generator:
      return invalid_id_generator(spec);
    case blob_store:
      return invalid_blob_store(spec);
    case worker_pool:
      return invalid_worker_pool(spec);
    case message_queue:
      return invalid_message_queue(spec);
    case kv_cluster:
      return invalid_kv_cluster(spec);
    case notification_fanout:
      return invalid_notification_fanout(spec);
    default:  // a type with no check of its own
      return std::nullopt;
  }
}

// A node that serves, resolved: its capacity from the scenario or the type's defaults, where each
// number came from, a leaky bucket's queue made of its bucket (leaky_bucket_capacity, its rate the
// scenario's `limit`), a database's connection pool and a kv cluster's ring applied. An id
// generator needs nothing more: its algorithm's numbers are type defaults (defaults::defaults_of).
[[nodiscard]] ResolvedNode resolved_server(const model::NodeSpec& spec) {
  // C++20 designated initializers: `.capacity = ...` names the member it sets (in declaration
  // order; an omitted one keeps its default), replacing positional `ResolvedNode{spec, ...}`, where
  // a reader must count commas against the struct to know which value is which (database_shares
  // below sets six numbers of the same type this way).
  ResolvedNode node{
      .spec = spec,
      .capacity = defaults::resolve_capacity(spec),
      .service_rate_source = defaults::resolve_service_rate(spec).source,
      .service_time_source = defaults::resolve_service_time(spec).source,
  };
  if (is_leaky_bucket(spec)) {
    node.capacity = leaky_bucket_capacity(spec, node.capacity);
    // Both follow from the scenario's `limit`.
    node.service_rate_source = defaults::Source::scenario;
    node.service_time_source = defaults::Source::scenario;
  }
  if (spec.type == model::NodeType::sql_db) apply_database_keys(node);
  if (spec.type == model::NodeType::kv_cluster) apply_kv_ring(node);
  return node;
}

}  // namespace

bool is_write(const model::NodeSpec& spec, std::string_view op) {
  return std::ranges::contains(model::write_ops_of(spec), op);
}

bool fetches_read_misses(const model::NodeSpec& spec) {
  if (spec.type == model::NodeType::cdn) return true;
  return is_cache(spec) && spec.strategy == model::CacheStrategy::read_through;
}

// The table at DatabaseShares. Without replicas the S primaries share reads and writes alike.
DatabaseShares database_shares(const model::NodeSpec& spec) {
  const double copies_per_shard = spec.replicas;
  const double shards = spec.shards.value_or(1);
  if (copies_per_shard < 2.0) {
    return DatabaseShares{.primaries = shards,
                          .replicas = 0.0,
                          .primary_reads = 1.0 / shards,
                          .primary_writes = 1.0 / shards};
  }
  const double replicas = (copies_per_shard - 1.0) * shards;
  return DatabaseShares{.primaries = shards,
                        .replicas = replicas,
                        .primary_reads = 0.0,  // a primary serves no reads while replicas exist
                        .primary_writes = 1.0 / shards,
                        .replica_reads = 1.0 / replicas,
                        .replica_writes = 1.0 / shards};
}

// One copy's load is its share of each kind: with N = 3, 1000 read units/s and 100 write
// units/s, the primary carries 100/s and each replica 1000 / 2 + 100 = 600/s.
DatabaseCopyLoads database_copy_loads(const model::NodeSpec& spec, double reads, double writes) {
  const DatabaseShares shares = database_shares(spec);
  DatabaseCopyLoads loads{.primary = reads * shares.primary_reads + writes * shares.primary_writes};
  if (shares.replicas > 0.0) {
    loads.replica = reads * shares.replica_reads + writes * shares.replica_writes;
  }
  return loads;
}

// The pool lowered the rate below the stated or default capacity (apply_database_keys).
bool pool_binds(const ResolvedNode& node) {
  return node.spec.type == model::NodeType::sql_db && node.spec.pool &&
         node.capacity.service_rate.per_second <
             defaults::resolve_service_rate(node.spec).value.per_second;
}

// The call-graph search lives in a named, unexported namespace (module linkage) rather
// than the anonymous one: it was written in the interface unit, where GCC 14 rejects
// std::map/std::vector of an internal-linkage type ("references internal linkage entity"), and
// graph.cppm still declares two detail types (CallComponents, HitRatioInputs) for Graph's members.
namespace detail {

// A vertex of the call graph: `op` being handled at node `node`.
struct Call {
  std::size_t node{};
  std::string op{};
  // C++20 defaulted three-way comparison: `<=>` compares (node, op) in declaration order and the
  // compiler derives <, <=, >, >= and == from it, replacing hand-written operators or a std::tie
  // comparator; it is what makes a Call a std::map key (AsyncHop in graph.cppm does the same).
  auto operator<=>(const Call&) const = default;
};

// A load balancer or a rate limiter has no `ops` steps, yet each op it receives is a call
// to the nodes behind it: (lb, op) -> (target, op) for every route that carries the op with
// a positive share (a drained route, weight 0, carries no request, so it makes no call and
// can close no call cycle); a limiter with a backend also calls (backend, counter) first.
// A cache that passes a share of an op on to its backend calls (backend, op) (write_back's
// later flush excepted: nobody waits for it, see write_back_flushes), and (target, step.op)
// along each `then` step that carries a share of the op. A node with `ops` steps calls what its
// resolved hops (Graph::service_hops) say with a positive fraction: an `on: miss` step is a
// call for the requests that miss, one that never fires (fraction 0) is none.
// A normalisation into one common representation (no interface is wrapped, so no pattern label):
// the forwarders' routes, the caches' backend shares and the resolved hops are re-expressed as
// the steps the search below walks, keyed by the call they belong to, so the search needs no case
// per node type.
using ForwardedSteps = std::map<Call, std::vector<model::Step>>;

void add_hop_steps(const Graph& g, std::size_t node, ForwardedSteps& calls) {
  // C++20 range adaptors: `map | std::views::keys` is a lazy view of each entry's key (here
  // the op name), composed with `|`. Replaces iterating the pairs and writing `.first`, and
  // copies nothing.
  for (const auto& op : g.nodes()[node].spec.ops | std::views::keys) {
    auto& steps = calls[Call{node, op}];
    for (const ServiceHop& hop : g.service_hops(node, op)) {
      if (hop.fraction <= 0.0) continue;  // never fires: no call
      steps.push_back(model::Step{.to = g.nodes()[hop.target].spec.id, .op = hop.op});
    }
  }
}

void add_backend_steps(const Graph& g, std::size_t cache, ForwardedSteps& calls) {
  const auto& resolved = g.nodes()[cache];
  if (!resolved.backend || resolved.spec.strategy == model::CacheStrategy::write_back) return;
  for (const auto& op : g.arriving_ops(cache)) {
    if (g.backend_fraction(cache, op) <= 0.0) continue;
    calls[Call{cache, op}].push_back(
        model::Step{.to = g.nodes()[*resolved.backend].spec.id, .op = op});
  }
}

// A cache's `then` steps (spec §18 entry 5) fire per request the cache handles: every one for an
// `on: always` step, the read misses for an `on: miss` one (Graph::then_fraction). A notification
// fanout's fire for the notifications its dedup step lets through, and the fanout waits for them as
// a service waits for its steps, so they are calls too: one that closes a call cycle is an
// asynchronous hand-off like any other.
void add_then_steps(const Graph& g, std::size_t node, ForwardedSteps& calls) {
  for (const auto& op : g.arriving_ops(node)) {
    for (const model::Step& step : g.nodes()[node].spec.then) {
      if (g.then_fraction(node, step, op) <= 0.0) continue;  // never fires: no call
      calls[Call{node, op}].push_back(model::Step{.to = step.to, .op = step.op});
    }
  }
}

[[nodiscard]] ForwardedSteps forwarder_steps(const Graph& g) {
  ForwardedSteps forwarded;
  for (std::size_t node = 0; node < g.nodes().size(); ++node) {
    const auto& resolved = g.nodes()[node];
    add_hop_steps(g, node, forwarded);
    if (hits_or_misses(resolved.spec.type)) {
      add_backend_steps(g, node, forwarded);
      add_then_steps(g, node, forwarded);
    }
    if (resolved.spec.type == model::NodeType::notification_fanout) {
      add_then_steps(g, node, forwarded);
    }
    if (!forwards_by_link(resolved.spec.type)) continue;
    for (const auto& op : g.arriving_ops(node)) {
      auto& steps = forwarded[Call{node, op}];
      if (resolved.backend) {
        steps.push_back(
            model::Step{.to = g.nodes()[*resolved.backend].spec.id, .op = std::string{counter_op}});
      }
      for (const auto& route : g.routes_for(node, op)) {
        if (route.share <= 0.0) continue;  // drained: no call
        steps.push_back(model::Step{.to = g.nodes()[route.target].spec.id, .op = op});
      }
    }
  }
  return forwarded;
}

// The calls handling `call.op` at `call.node` makes, in declaration order (forwarder_steps);
// none for an op the node lists without steps (terminal) or does not list at all.
[[nodiscard]] std::span<const model::Step> steps_of(const ForwardedSteps& forwarded,
                                                    const Call& call) {
  if (const auto found = forwarded.find(call); found != forwarded.end()) return found->second;
  return {};
}

// Roots of the call-graph search: every client route's (entry node, op), clients in index
// order and each client's routes in order (a load balancer's routes carry no op of their
// own and are reached through the calls into it), so a call cycle is cut where it closes as seen
// from the users. Then every op of every node, so a cycle no client reaches is still cut.
[[nodiscard]] std::vector<Call> call_roots(const Graph& g) {
  std::vector<Call> roots;
  for (std::size_t node = 0; node < g.nodes().size(); ++node) {
    if (g.nodes()[node].spec.type != model::NodeType::client) continue;
    for (const auto& route : g.client_routes(node)) roots.push_back({route.target, route.op});
  }
  for (std::size_t node = 0; node < g.nodes().size(); ++node) {
    for (const auto& op : g.nodes()[node].spec.ops | std::views::keys) roots.push_back({node, op});
  }
  return roots;
}

// The hops that close a call cycle: the back edges of a depth-first search over (node, op)
// vertices, with an edge (node, op) -> (step.to, step.op) for every hop that fires (an
// unconditional step, or an `on: miss` step after a cache that misses; of the `then` steps
// only a cache's: a blob store's are no calls, nobody waits for them (blob_store_events); a worker
// pool's are calls, but no call reaches a pool (only its queue's asynchronous deliveries do,
// Graph::check_pool_senders), so no call cycle can pass through one; a frontier's are hand-offs
// to its downloaders that nobody waits for, as a queue's deliveries are, so a call that reaches a
// frontier ends there (behaviours::FrontierBehaviour); a notification fan-out's are calls it
// waits for, as a service's steps, forwarder_steps), an edge (lb, op) ->
// (target, op) for every route of a load balancer that carries the op, and (cache, op) ->
// (backend, op) for what a cache passes on synchronously (forwarder_steps). Same white/grey/black
// classification as Graph::order_nodes, one level finer: there the vertices are boxes, here they
// are the calls a request makes. A back edge is a call that, followed far enough, calls itself:
// a.msg -> b.relay -> c.job -> a.msg, three services each calling the next (a message queue in
// that loop would end the calls at it: its deliveries are asynchronous, consumer_deliveries). A
// request cannot wait for its own descendant, so that hop is a fire-and-forget hand-off (the
// latency model skips it). A node loop that ends in a terminal op (c.job -> a.push, where `push`
// makes no call) has no back edge here, so every one of its hops is waited for.
[[nodiscard]] std::vector<AsyncHop> find_async_hops(const Graph& g,
                                                    const ForwardedSteps& forwarded) {
  enum class Colour { grey, black };  // absent from the map = white (not yet visited)
  std::map<Call, Colour> colour;
  struct Frame {
    Call call;
    std::span<const model::Step> steps;
    std::size_t next_step;
  };
  std::vector<AsyncHop> hops;
  for (const Call& root : call_roots(g)) {
    // C++20 std::map::contains: "is this key present?" in one call, replacing
    // `find(key) != end()` or `count(key) != 0`.
    if (colour.contains(root)) continue;
    colour.emplace(root, Colour::grey);
    std::vector<Frame> stack{{root, steps_of(forwarded, root), 0}};
    while (!stack.empty()) {
      auto& frame = stack.back();
      if (frame.next_step == frame.steps.size()) {
        colour[frame.call] = Colour::black;
        stack.pop_back();
        continue;
      }
      const model::Step& step = frame.steps[frame.next_step++];
      Call callee{*g.index_of(step.to), step.op};  // build proved the target exists
      const auto found = colour.find(callee);
      if (found == colour.end()) {
        colour.emplace(callee, Colour::grey);
        const auto callee_steps = steps_of(forwarded, callee);
        stack.push_back({std::move(callee), callee_steps, 0});  // invalidates `frame`
      } else if (found->second == Colour::grey) {
        hops.push_back({frame.call.node, frame.call.op, callee.node, callee.op});
      }
    }
  }
  // Two identical steps of one op are one hop. C++20 ranges algorithms take the vector
  // itself instead of an iterator pair; std::ranges::unique returns the subrange of
  // leftovers to erase, the erase-unique idiom without begin/end bookkeeping.
  std::ranges::sort(hops);
  const auto duplicates = std::ranges::unique(hops);
  hops.erase(duplicates.begin(), duplicates.end());
  return hops;
}

// ---- Which calls share a call cycle: strongly connected components (Tarjan, 1972) ----
//
// find_async_hops answers "which hop does a depth-first search meet as a back edge?", and the
// answer depends on where the search starts: in u -> c.read -> db.read -> c.read the search from c
// cuts db's call back to c, the search from db (a client calling db) cuts c's fetch from db.
// Whether a hop lies on a call cycle at all must not depend on that, so it is asked of the call
// graph's strongly connected components instead.
//
// A strongly connected component (SCC) of a directed graph is a largest set of vertices in which
// every vertex reaches every other along the edges. Every vertex is in exactly one (a vertex on no
// cycle is a component on its own). Two distinct calls X and Y are in the same component exactly
// when there are paths X -> ... -> Y and Y -> ... -> X; put end to end, the two paths are a cycle
// through both. So "does the hop X -> Y lie on a call cycle?" is "are X and Y in one component?":
// Y reaches X back, and the hop closes the loop.
//
// Tarjan's algorithm finds every component in one depth-first search, in time linear in the calls
// plus the hops. Each call gets `index`, the order in which the search discovered it, and `low`,
// the smallest index it can reach through the calls below it in the search and one hop back to a
// call still being worked on (on the stack). Discovered calls are pushed on a stack. When a call
// finishes with low == index, nothing below it reaches higher up the search, so it is the first
// call discovered of its component, and the component is that call and every call above it on the
// stack: they are popped together and numbered. The recursion is as deep as the longest chain of
// calls, a few dozen in any scenario a person writes.
struct CallComponents {
  std::map<Call, std::size_t> component_of;  // every call the search reached -> its component

  // Do the distinct calls `a` and `b` lie on a common call cycle? A call that is in no hop
  // (absent from the map) is on no cycle.
  [[nodiscard]] bool on_common_cycle(const Call& a, const Call& b) const {
    const auto found_a = component_of.find(a);
    const auto found_b = component_of.find(b);
    if (found_a == component_of.end() || found_b == component_of.end()) return false;
    return found_a->second == found_b->second;
  }
};

class ComponentSearch {
 public:
  ComponentSearch(const Graph& g, const ForwardedSteps& forwarded) : g_{g}, forwarded_{forwarded} {}

  // Every call that makes a call is a root, in the map's order, so the numbering is reproducible
  // (a call that makes none is reached from its caller, or is in no hop at all).
  [[nodiscard]] CallComponents run() {
    for (const Call& call : forwarded_ | std::views::keys) {
      if (!marks_.contains(call)) visit(call);
    }
    return std::move(components_);
  }

 private:
  struct Mark {
    std::size_t index{};
    std::size_t low{};
    bool on_stack{};
  };

  void visit(const Call& call) {
    // A std::map's elements never move, so `mark` stays valid while visit() inserts others.
    Mark& mark = marks_[call] = Mark{.index = next_index_, .low = next_index_, .on_stack = true};
    ++next_index_;
    stack_.push_back(call);
    for (const model::Step& step : steps_of(forwarded_, call)) {
      const Call callee{*g_.index_of(step.to), step.op};  // build proved the target exists
      const auto found = marks_.find(callee);
      if (found == marks_.end()) {
        visit(callee);
        mark.low = std::min(mark.low, marks_.at(callee).low);
      } else if (found->second.on_stack) {
        mark.low = std::min(mark.low, found->second.index);  // a hop back up the search
      }
    }
    if (mark.low == mark.index) close_component(call);
  }

  // Pops the component whose first-discovered call is `root`: everything above it on the stack.
  void close_component(const Call& root) {
    bool reached_root = false;
    while (!reached_root) {
      Call member = std::move(stack_.back());
      stack_.pop_back();
      marks_.at(member).on_stack = false;
      reached_root = member == root;
      components_.component_of.emplace(std::move(member), next_component_);
    }
    ++next_component_;
  }

  const Graph& g_;
  const ForwardedSteps& forwarded_;
  std::map<Call, Mark> marks_;  // absent = not yet discovered
  std::vector<Call> stack_;
  std::size_t next_index_{};
  CallComponents components_;
  std::size_t next_component_{};
};

[[nodiscard]] CallComponents call_components(const Graph& g, const ForwardedSteps& forwarded) {
  return ComponentSearch{g, forwarded}.run();
}

// The op node `node` sends its backend for a request of `op` and waits for, if any: `counter` at a
// rate limiter (it asks its counter store before it admits, book ch. 4); the op itself at a cache
// or CDN that passes some of it on (Graph::backend_fraction), except a write_back cache's write,
// whose flush nobody waits for (write_back_flushes).
[[nodiscard]] std::optional<std::string> waited_backend_op(const Graph& g, std::size_t node,
                                                           const std::string& op) {
  const auto& spec = g.nodes()[node].spec;
  if (spec.type == model::NodeType::rate_limiter) return std::string{counter_op};
  if (is_cache(spec) && spec.strategy == model::CacheStrategy::write_back) return std::nullopt;
  if (g.backend_fraction(node, op) <= 0.0) return std::nullopt;
  return op;
}

// The flush hops of every write_back cache: (cache, write) -> (backend, write) for each write op
// that reaches it (spec §18 entry 5). Asynchronous BY DESIGN, not because they close a call cycle:
// the cache acknowledges the write from its own memory and flushes it later, so nobody waits for
// the flush. It still forwards every write (Graph::backend_fraction is 1), so the store's load is
// unchanged; the latency model skips it like any asynchronous hop.
[[nodiscard]] std::vector<AsyncHop> write_back_flushes(const Graph& g) {
  std::vector<AsyncHop> flushes;
  for (std::size_t cache = 0; cache < g.nodes().size(); ++cache) {
    const auto& resolved = g.nodes()[cache];
    if (!is_cache(resolved.spec) || !resolved.backend) continue;
    if (resolved.spec.strategy != model::CacheStrategy::write_back) continue;
    for (const auto& op : g.arriving_ops(cache)) {
      if (g.backend_fraction(cache, op) <= 0.0) continue;  // a read: write_back sends none
      flushes.push_back({cache, op, *resolved.backend, op});
    }
  }
  return flushes;
}

// The event notifications of every blob store (spec §19 entries 6-7): (store, op) -> (target,
// step.op) for each write op that reaches it and each `then` step (Graph::build refuses an `on:
// miss` or `on: failure` one, invalid_blob_store). Asynchronous BY DESIGN, as a write-back flush
// is: S3 sends the event once the object is stored (book ch. 14: transcoding starts when the upload
// completes), and the uploader, answered by then, never waits for whoever receives it. A read fires
// no step (Graph::then_fraction is 0: S3 has no event for a GET), so it has no hop.
[[nodiscard]] std::vector<AsyncHop> blob_store_events(const Graph& g) {
  std::vector<AsyncHop> events;
  for (std::size_t store = 0; store < g.nodes().size(); ++store) {
    const auto& spec = g.nodes()[store].spec;
    if (spec.type != model::NodeType::blob_store) continue;
    for (const auto& op : g.arriving_ops(store)) {
      for (const model::Step& step : spec.then) {
        if (g.then_fraction(store, step, op) <= 0.0) continue;         // a read: no event
        events.push_back({store, op, *g.index_of(step.to), step.op});  // connect_step resolved it
      }
    }
  }
  return events;
}

// The deliveries of every message queue to its consumer groups (spec §19 entry 6): (queue, op) ->
// (pool, op) for each op that reaches the queue and each route that carries it. Asynchronous BY
// DESIGN, as a blob store's events are: the broker answers its publisher once the message is
// appended (Kafka's acks), and the publisher never waits for the worker that reads it later, so a
// pool's backlog and drops reach neither its response nor its answered rate.
[[nodiscard]] std::vector<AsyncHop> consumer_deliveries(const Graph& g) {
  std::vector<AsyncHop> deliveries;
  for (std::size_t queue = 0; queue < g.nodes().size(); ++queue) {
    if (!copies_to_consumers(g.nodes()[queue].spec.type)) continue;
    for (const auto& op : g.arriving_ops(queue)) {
      for (const auto& route : g.routes_for(queue, op)) {
        if (route.share > 0.0) deliveries.push_back({queue, op, route.target, op});
      }
    }
  }
  return deliveries;
}

// The call-cycle hops, the write-back flushes, the blob stores' events and the queues' deliveries,
// sorted and without duplicates, as Graph::is_async_hop's binary search needs them.
[[nodiscard]] std::vector<AsyncHop> async_hops(const Graph& g, const ForwardedSteps& forwarded) {
  auto hops = find_async_hops(g, forwarded);
  // (C++23's std::vector::append_range would say this in one call; GCC 14's library lacks it.)
  const auto flushes = write_back_flushes(g);
  hops.insert(hops.end(), flushes.begin(), flushes.end());
  const auto events = blob_store_events(g);
  hops.insert(hops.end(), events.begin(), events.end());
  const auto deliveries = consumer_deliveries(g);
  hops.insert(hops.end(), deliveries.begin(), deliveries.end());
  std::ranges::sort(hops);
  const auto duplicates = std::ranges::unique(hops);
  hops.erase(duplicates.begin(), duplicates.end());
  return hops;
}

}  // namespace detail

// ---- Hit ratios (spec §18 entries 1-2) ----
//
// In `detail` (module linkage) rather than the anonymous namespace, for the reason given on
// the call-graph search above: these types hold std::vectors and std::sets.
namespace detail {

// Does `node` have a hit ratio the graph can resolve now? A cache or CDN with `{fixed: h}` or
// `che`; not a HitRatio{} built in code (neither set: no hit ratio at all), and not a `che`
// CDN without a `keys` size: it stays without a number and misses every time on paper (explain
// says the load behind it is an upper bound), and the engine refuses to run it.
[[nodiscard]] bool has_resolvable_hit_ratio(const model::NodeSpec& spec) {
  if (!hits_or_misses(spec.type) || !spec.hit_ratio) return false;
  if (!spec.hit_ratio->che) return spec.hit_ratio->fixed.has_value();
  return spec.type == model::NodeType::cache || spec.keys.has_value();
}

// A cache whose misses have a Che popularity to pass on: a resolvable `che` one. Any other
// cache (a fixed ratio, an unsized che CDN) says how MANY requests miss but not WHICH keys.
[[nodiscard]] bool filters_by_che(const model::NodeSpec& spec) {
  return has_resolvable_hit_ratio(spec) && spec.hit_ratio->che;
}

// ---- Which keys reach which node: the op-aware walk (spec §18 entry 1, revised) ----
//
// A che cache's popularity is the popularity of the keys that reach it, and keys travel with
// requests: a client's keys reach a cache only along the CALLS its ops make. Walking demand
// edges (box to box) instead would let a writer whose ops never touch the cache, but pass the
// same service, impose its key distribution on it. So the walk is over (node, op) vertices,
// like the call-cycle search (detail::find_async_hops), and runs per client.
//
// Its hops cannot come from forwarder_steps: those read the resolved hops' fractions, which need
// the very hit ratios being resolved here. call_hops below says, without any hit ratio, which
// (node, op) a call may lead to — every hop that carries flow for SOME hit ratio below 1 (an `on:
// miss` step is reachable whatever h turns out to be) — and whose misses it carries. The size walk
// further down (spec §19 entry 1) follows the same hops, and walk_calls is the depth-first search
// both run.
//
// Assumed: every hop between a client and a cache carries the client's key (a service asked
// for short link k asks its cache for k). One kind of hop does not, and is left out: a rate
// limiter's `counter` request to its store, which counts requests per USER (or per rule), not
// per key the request is about (book ch. 4: the Redis counter is keyed by user id or IP).

// A call (node, op) may lead to: `to` receives `op`. `misses_of` is set when the hop carries
// only the misses of a cache (that cache): a read_through cache's fetch from its backend, a
// cache's `then` step on miss, a service's `on: miss` step after its lookup at a cache. `size`
// is the payload the hop's step states (`size:`); a hop that is no step (a route, a backend
// fetch) states none. The key walk reads `misses_of`, the size walk `size`.
struct CallHop {
  std::size_t to{};
  std::string op{};
  std::optional<std::size_t> misses_of{};
  std::optional<Bytes> size{};
};

// A service's (or any node's) `ops` steps for `op`, every one of them. An `on: miss` step carries
// the misses of its lookup, the nearest unconditional step before it (as
// Graph::resolve_service_hops pairs them; a miss step after something that is no cache is refused
// there, and inherits here). An `on: failure` step carries the requests the step it reports on
// failed (spec §5): any of the requests, so the keys and the size travel on it as on an
// unconditional step, and it is no lookup for a miss step after it.
void add_op_step_hops(const Graph& g, const model::NodeSpec& spec, std::string_view op,
                      std::vector<CallHop>& hops) {
  const auto steps = spec.ops.find(std::string{op});
  if (steps == spec.ops.end()) return;
  std::optional<std::size_t> lookup;  // the cache the last unconditional step asked
  for (const model::Step& step : steps->second) {
    const auto target = *g.index_of(step.to);  // connect_step resolved every target
    switch (step.on) {
      case model::StepCondition::failure:
        hops.push_back({.to = target, .op = step.op, .size = step.size});
        break;
      case model::StepCondition::always:
        lookup = hits_or_misses(g.nodes()[target].spec.type) ? std::optional{target} : std::nullopt;
        hops.push_back({.to = target, .op = step.op, .size = step.size});
        break;
      case model::StepCondition::miss:
        hops.push_back({.to = target, .op = step.op, .misses_of = lookup, .size = step.size});
        break;
    }
  }
}

// A cache's fetch from its backend (Graph::backend_fraction, whatever h is): a read under
// read_through, or at a CDN (from its origin), carries the misses; a write under a write strategy
// carries every write.
void add_backend_hop(const ResolvedNode& cache, std::size_t node, std::string_view op,
                     std::vector<CallHop>& hops) {
  if (!hits_or_misses(cache.spec.type) || !cache.backend || !sends_to_store(cache.spec, op)) {
    return;
  }
  const auto misses_of = is_write(cache.spec, op) ? std::nullopt : std::optional{node};
  hops.push_back({.to = *cache.backend, .op = std::string{op}, .misses_of = misses_of});
}

// `then` steps (Graph::then_fraction): an unconditional one carries everything (at a blob store,
// every write and no read), an `on: miss` one the read misses of a cache (and nothing of a write,
// or of any other node; a CDN has no `then` steps).
void add_then_hops(const Graph& g, const model::NodeSpec& spec, std::size_t node,
                   std::string_view op, std::vector<CallHop>& hops) {
  for (const model::Step& step : spec.then) {
    const auto target = *g.index_of(step.to);
    if (step.on == model::StepCondition::always && g.then_fraction(node, step, op) > 0.0) {
      hops.push_back({.to = target, .op = step.op, .size = step.size});
    } else if (step.on == model::StepCondition::miss && hits_or_misses(spec.type) &&
               !is_write(spec, op)) {
      hops.push_back({.to = target, .op = step.op, .misses_of = node, .size = step.size});
    }
  }
}

// Every hop a request for `op` at `node` may take (see the section comment). A load balancer
// or a limiter passes the op on along its routes that carry it (not along a drained one); a
// limiter's backend (`counter`) is left out on purpose.
[[nodiscard]] std::vector<CallHop> call_hops(const Graph& g, std::size_t node,
                                             std::string_view op) {
  const auto& resolved = g.nodes()[node];
  std::vector<CallHop> hops;
  if (forwards_by_link(resolved.spec.type)) {
    for (const auto& route : g.routes_for(node, op)) {
      if (route.share > 0.0) hops.push_back({.to = route.target, .op = std::string{op}});
    }
  }
  add_op_step_hops(g, resolved.spec, op, hops);
  add_backend_hop(resolved, node, op, hops);
  add_then_hops(g, resolved.spec, node, op, hops);
  return hops;
}

// One way a client's keys arrive at a node: from `client`, sent on by `sender`, as `op`.
// `filtered_by` is the nearest che cache upstream whose MISSES these requests are (empty: no
// che cache filtered them); `unfiltered_through` the nearest cache since then that missed too
// but has no Che popularity to filter by (a fixed ratio), whose filtering is therefore ignored.
struct KeyArrival {
  std::size_t client{};
  std::size_t sender{};
  std::string op{};
  std::optional<std::size_t> filtered_by{};
  std::optional<std::size_t> unfiltered_through{};
  auto operator<=>(const KeyArrival&) const = default;  // a std::set key (see AsyncHop)
};

// Per node, every way keys arrive at it, sorted (by client first: error messages name the
// clients in scenario order) and without duplicates.
using KeyArrivals = std::vector<std::vector<KeyArrival>>;

// A vertex of the walk: `op` at `node`, with the filters its keys went through so far.
struct KeyStream {
  std::size_t node{};
  std::string op{};
  std::optional<std::size_t> filtered_by{};
  std::optional<std::size_t> unfiltered_through{};
  auto operator<=>(const KeyStream&) const = default;
};

// The filters of a hop's requests: those of the stream it leaves, unless it carries the misses
// of a cache — a che cache becomes the filter (and resets the caveat, which that cache's own
// provenance reports), any other cache becomes the caveat.
[[nodiscard]] KeyStream next_stream(const Graph& g, const KeyStream& from, const CallHop& hop) {
  KeyStream next{.node = hop.to,
                 .op = hop.op,
                 .filtered_by = from.filtered_by,
                 .unfiltered_through = from.unfiltered_through};
  if (!hop.misses_of) return next;
  if (filters_by_che(g.nodes()[*hop.misses_of].spec)) {
    next.filtered_by = hop.misses_of;
    next.unfiltered_through.reset();
  } else {
    next.unfiltered_through = hop.misses_of;
  }
  return next;
}

// Where a walk starts: `vertex`, sent by `sender` (a client, for both walks).
template <typename Vertex>
struct Seed {
  std::size_t sender{};
  Vertex vertex{};
};

// The depth-first search both walks run, over vertices that are a (node, op) plus what the walk
// carries (a KeyStream's filters, a SizeStream's size): from the seeds, along every vertex that
// `next(vertex)` lists, calling `record(sender, vertex)` for each arrival, the sender being the
// node of the vertex it came from. Each vertex is expanded once (Vertex's <=> says which are the
// same), so a call cycle ends the walk instead of looping.
template <typename Vertex, typename Next, typename Record>
void walk_calls(std::vector<Seed<Vertex>> seeds, Next next, Record record) {
  std::set<Vertex> seen;
  std::vector<Vertex> work;
  const auto arrive = [&](std::size_t sender, Vertex vertex) {
    record(sender, vertex);
    if (seen.insert(vertex).second) work.push_back(std::move(vertex));
  };
  for (auto& seed : seeds) arrive(seed.sender, std::move(seed.vertex));
  while (!work.empty()) {
    const Vertex vertex = std::move(work.back());
    work.pop_back();
    for (Vertex following : next(vertex)) arrive(vertex.node, std::move(following));
  }
}

// One client's walk over KeyStream vertices from its routes (a drained route, share 0, carries
// nothing), recording every arrival.
void walk_client_keys(const Graph& g, std::size_t client,
                      std::vector<std::set<KeyArrival>>& arrivals) {
  std::vector<Seed<KeyStream>> seeds;
  for (const auto& route : g.client_routes(client)) {
    if (route.share > 0.0) {
      seeds.push_back({.sender = client, .vertex = {.node = route.target, .op = route.op}});
    }
  }
  const auto next = [&](const KeyStream& stream) {
    std::vector<KeyStream> following;
    for (const CallHop& hop : call_hops(g, stream.node, stream.op)) {
      following.push_back(next_stream(g, stream, hop));
    }
    return following;
  };
  const auto record = [&](std::size_t sender, const KeyStream& stream) {
    arrivals[stream.node].insert(KeyArrival{.client = client,
                                            .sender = sender,
                                            .op = stream.op,
                                            .filtered_by = stream.filtered_by,
                                            .unfiltered_through = stream.unfiltered_through});
  };
  walk_calls(std::move(seeds), next, record);
}

// Every client's walk (walk_client_keys), as KeyArrivals. O(clients × (node, op, filter)
// vertices × hops), once per graph however many che caches read it.
[[nodiscard]] KeyArrivals key_arrivals(const Graph& g) {
  std::vector<std::set<KeyArrival>> arrivals(g.nodes().size());
  for (std::size_t node = 0; node < g.nodes().size(); ++node) {
    if (g.nodes()[node].spec.type == model::NodeType::client) walk_client_keys(g, node, arrivals);
  }
  KeyArrivals sorted(arrivals.size());
  for (std::size_t node = 0; node < arrivals.size(); ++node) {
    sorted[node].assign(arrivals[node].begin(), arrivals[node].end());
  }
  return sorted;
}

// ---- One request size per (node, op): the size walk (spec §19 entry 1) ----
//
// A byte-sized node (defaults::is_byte_sized) counts megabytes, so it must know how big each
// request it serves is. Sizes travel with requests, as keys do, so this is the key walk again
// (walk_calls over call_hops), carrying a size instead of filters. The rule: a request keeps its
// size while it keeps its op name (a balancer's or limiter's route, a cache's or CDN's miss or
// write forwarded to its store or origin, a step or `then` step whose op is the incoming op), a
// step that states `size:` sends that size, and a step that renames the op without one sends an
// unsized request. Seeded with each client's routes that carry flow, sized by its `sizes:`, and
// with every step that states its size: such a step sends that size wherever it fires, even at a
// node the walk never reaches from a client (a worker pool's `then` steps fire for whatever the
// pool receives, and collect_arriving_ops counts them as arriving whether or not a queue feeds
// the pool). Each request also carries its ORIGIN, the client or step that set its size (or, for
// an unsized one, the client that stated none or the step that renamed the op), so a message
// names where a size came from rather than the balancer every request passed through.
//
// Two hops the key walk leaves out are followed here, so that every op collect_arriving_ops
// brings to a node is walked: a rate limiter's `counter` request to its store (unsized: it is
// the limiter's own request, not the client's), and a message queue's links to the worker pools
// behind it (each message keeps its op name through the queue, and the pool's `then` steps carry
// it on). Like the key walk, it takes every hop that carries flow for SOME hit ratio, so a size
// is needed on an `on: miss` path even if the cache in front never misses. What the walk still
// cannot reach (a pool's unsized `then` step with no queue in front) Graph::resolve_request_sizes
// catches from arriving_ops.
//
// Assumed: one size per (node, op) (spec §19 entry 1: each op's size is its mean; a flow that
// carries bytes per op is phase 3).
//
// `on: failure` steps are walked like any step (spec §5: they fire since phase 3): call_hops
// follows them through add_op_step_hops and add_sized_step_seed seeds a sized one, so an unsized
// failure step over a bandwidth link is refused with every unsized step's message
// (unpriced_on_pipe), and a sized one is priced at its `size:`. (Phase 2 skipped them, since they
// never fired; an unsized one over a pipe would have made engine::megabytes_of throw once they
// did.)

// A vertex of the size walk: `op` at `node`, `size` bytes long (unsized when empty), its size
// set (or left unset) by `origin`, a client or the node of a step.
struct SizeStream {
  std::size_t node{};
  std::string op{};
  std::optional<Bytes> size{};
  std::size_t origin{};
  auto operator<=>(const SizeStream&) const = default;  // a std::set key (see AsyncHop)
};

// One way a request arrives at a node: as `op`, `size` bytes long, as `origin` set it. Sorted by
// op, then origin (messages name the origins in scenario order).
struct SizeArrival {
  std::string op{};
  std::size_t origin{};
  std::optional<Bytes> size{};
  auto operator<=>(const SizeArrival&) const = default;
};

// Do two arrivals carry the same op? (Graph::resolve_request_sizes groups by it.) A named
// function, not a lambda: with a lambda there, GCC 14.2 emitted the lambda's conversion to a
// function pointer in every unit importing this module, and the link failed with "multiple
// definition" (2026-10-28).
[[nodiscard]] bool same_op(const SizeArrival& a, const SizeArrival& b) {
  return a.op == b.op;
}

// Where a hop leads in the size walk: its step's own size (set at `from`'s node), else the
// incoming size and origin if the op keeps its name, else unsized, renamed at `from`'s node.
[[nodiscard]] SizeStream next_size(const SizeStream& from, const CallHop& hop) {
  if (hop.size) return {.node = hop.to, .op = hop.op, .size = hop.size, .origin = from.node};
  if (hop.op == from.op) {
    return {.node = hop.to, .op = hop.op, .size = from.size, .origin = from.origin};
  }
  return {.node = hop.to, .op = hop.op, .origin = from.node};
}

// The hops of the size walk that the key walk leaves out (see the section comment): a limiter's
// `counter` to its store, and a message queue's links to the worker pools behind it that carry
// the op.
void add_size_only_hops(const Graph& g, std::size_t node, std::string_view op,
                        std::vector<CallHop>& hops) {
  const auto& resolved = g.nodes()[node];
  if (resolved.spec.type == model::NodeType::rate_limiter && resolved.backend) {
    hops.push_back({.to = *resolved.backend, .op = std::string{counter_op}});
  }
  if (!copies_to_consumers(resolved.spec.type)) return;
  for (const auto& route : g.routes_for(node, op)) {
    if (route.share > 0.0) hops.push_back({.to = route.target, .op = std::string{op}});
  }
}

// Where a SizeStream leads: every call hop and size-only hop, each with its size (next_size).
[[nodiscard]] std::vector<SizeStream> next_sizes(const Graph& g, const SizeStream& stream) {
  auto hops = call_hops(g, stream.node, stream.op);
  add_size_only_hops(g, stream.node, stream.op, hops);
  std::vector<SizeStream> following;
  for (const CallHop& hop : hops) following.push_back(next_size(stream, hop));
  return following;
}

// Per node, every way a request arrives at it (SizeArrival), sorted and without duplicates.
using SizeArrivals = std::vector<std::vector<SizeArrival>>;

// The size a client states for `op` (`sizes:`), if it states one.
[[nodiscard]] std::optional<Bytes> stated_size(const model::NodeSpec& client,
                                               const std::string& op) {
  if (!client.workload) return std::nullopt;
  const auto stated = client.workload->sizes.find(op);
  if (stated == client.workload->sizes.end()) return std::nullopt;
  return stated->second;
}

// The walk's seeds from a client: its routes that carry flow (a drained one, share 0, carries
// nothing), each sized by the client's `sizes:`.
void add_client_seeds(const Graph& g, std::size_t client, std::vector<Seed<SizeStream>>& seeds) {
  const auto& spec = g.nodes()[client].spec;
  for (const auto& route : g.client_routes(client)) {
    if (route.share <= 0.0) continue;
    const SizeStream stream{.node = route.target,
                            .op = route.op,
                            .size = stated_size(spec, route.op),
                            .origin = client};
    seeds.push_back({.sender = client, .vertex = stream});
  }
}

// The walk's seed from a step of `node` that states its size, if it does (see the section
// comment): the size holds wherever the step fires.
void add_sized_step_seed(const Graph& g, std::size_t node, const model::Step& step,
                         std::vector<Seed<SizeStream>>& seeds) {
  if (!step.size) return;
  const SizeStream stream{
      .node = *g.index_of(step.to), .op = step.op, .size = step.size, .origin = node};
  seeds.push_back({.sender = node, .vertex = stream});
}

// Every seed of the size walk: each client's routes, and each step of an `ops` list or a `then`
// list that states its size.
[[nodiscard]] std::vector<Seed<SizeStream>> size_seeds(const Graph& g) {
  std::vector<Seed<SizeStream>> seeds;
  for (std::size_t node = 0; node < g.nodes().size(); ++node) {
    const auto& spec = g.nodes()[node].spec;
    if (spec.type == model::NodeType::client) add_client_seeds(g, node, seeds);
    // C++20 `views::values`: a lazy view of each map entry's value (here a node's vector of steps
    // per op), the twin of `views::keys`; replaces `for (const auto& [op, steps] : ...)` with an
    // unused name.
    for (const auto& steps : spec.ops | std::views::values) {
      for (const model::Step& step : steps) add_sized_step_seed(g, node, step, seeds);
    }
    for (const model::Step& step : spec.then) add_sized_step_seed(g, node, step, seeds);
  }
  return seeds;
}

// Sorted copies of per-place arrival sets: what the size walk hands its readers.
[[nodiscard]] SizeArrivals sorted_arrivals(const std::vector<std::set<SizeArrival>>& sets) {
  SizeArrivals sorted(sets.size());
  for (std::size_t place = 0; place < sets.size(); ++place) {
    sorted[place].assign(sets[place].begin(), sets[place].end());
  }
  return sorted;
}

// What the size walk found: per node, every way a request arrives at it, and per link (index of
// Graph::links()), every way a request crosses it, which a pipe (ResolvedLink::pipe) must price in
// megabytes. A request from `sender` to a node crosses the one link that carries demand between
// them (Graph::demand_link: one link per pair and direction).
struct SizeWalk {
  SizeArrivals at_nodes;
  SizeArrivals over_links;
};

// The size walk from every seed at once: a size depends on the path a request took, not on
// which client sent it.
[[nodiscard]] SizeWalk size_walk(const Graph& g) {
  std::vector<std::set<SizeArrival>> at_nodes(g.nodes().size());
  std::vector<std::set<SizeArrival>> over_links(g.links().size());
  const auto next = [&](const SizeStream& stream) { return next_sizes(g, stream); };
  const auto record = [&](std::size_t sender, const SizeStream& stream) {
    const SizeArrival arrival{.op = stream.op, .origin = stream.origin, .size = stream.size};
    at_nodes[stream.node].insert(arrival);
    if (const auto link = g.demand_link(sender, stream.node)) over_links[*link].insert(arrival);
  };
  auto seeds = size_seeds(g);
  walk_calls(std::move(seeds), next, record);
  return {.at_nodes = sorted_arrivals(at_nodes), .over_links = sorted_arrivals(over_links)};
}

// "op 'get' arrives with two sizes, 1MB from 'a' and 2MB from 'b'", if two of the arrivals of one
// op (sorted by origin) state different sizes: the first sized one and the first that disagrees
// with it, each named by its origin (the client or step that set it). At any node, byte-sized or
// not, so that one size per (node, op) holds wherever the walk goes (a service that forwards the
// op unchanged would otherwise pass both on).
[[nodiscard]] std::optional<EngineError> two_sizes(std::span<const ResolvedNode> nodes,
                                                   std::size_t node,
                                                   std::span<const SizeArrival> arrivals) {
  const SizeArrival* first = nullptr;
  for (const SizeArrival& arrival : arrivals) {
    if (!arrival.size) continue;
    if (!first) {
      first = &arrival;
    } else if (*arrival.size != *first->size) {
      return EngineError{"node '" + nodes[node].spec.id + "': op '" + first->op +
                         "' arrives with two sizes, " + format_size(*first->size) + " from '" +
                         nodes[first->origin].spec.id + "' and " + format_size(*arrival.size) +
                         " from '" + nodes[arrival.origin].spec.id + "'"};
    }
  }
  return std::nullopt;
}

// "capacity 500MB/s, default": the byte rate that makes a node byte-sized, and where it came from:
// the scenario's `capacity: 100GB/s`, else a blob store's spec §6 default
// (defaults::is_byte_sized).
[[nodiscard]] std::string byte_capacity_text(const model::NodeSpec& spec) {
  if (spec.byte_capacity) return "capacity " + format_byte_rate(*spec.byte_capacity) + ", scenario";
  return "capacity " + format_byte_rate(defaults::blob_store_byte_rate) + ", default";
}

// Why a worker pool's step can send an op unsized although its jobs are sized: every job fires
// every step, so a same-op step `{op: upload}` carries the size of the jobs that arrived as
// `upload` and renames the jobs of every other op, and a renamed request has no size (spec §19
// entry 1). Only when `op` is one of the ops the pool's jobs arrive as: a step that renames every
// job (youtube's `{op: write}` after `transcode` jobs) is a plain rename, which the message's own
// advice covers. Empty for any other sender, whose steps are per op.
[[nodiscard]] std::string pool_renaming_hint(const Graph& g, std::string_view op,
                                             std::size_t from) {
  if (g.nodes()[from].spec.type != model::NodeType::worker_pool) return {};
  // C++20 std::ranges::binary_search: the range itself instead of a begin/end pair; a yes/no
  // lookup in O(log n) on a sorted range (arriving_ops is sorted by name).
  if (!std::ranges::binary_search(g.arriving_ops(from), op)) return {};
  return " (a worker pool's then step keeps the size of the jobs that arrive as '" +
         std::string{op} + "' only: it renames every other job, and a renamed job has no size)";
}

// "node 'store' counts bytes (capacity 500MB/s, default), but op 'fetch' from 'svc' has no size:
// ...": a byte-sized node's error for an unsized op, naming where to add the size.
[[nodiscard]] EngineError no_size_error(const Graph& g, std::size_t node, std::string_view op,
                                        std::size_t from) {
  const auto nodes = g.nodes();
  const auto& spec = nodes[node].spec;
  return EngineError{"node '" + spec.id + "' counts bytes (" + byte_capacity_text(spec) +
                     "), but op '" + std::string{op} + "' from '" + nodes[from].spec.id +
                     "' has no size: add size: to the step or sizes: to the client" +
                     pool_renaming_hint(g, op, from)};
}

// Is the arrival a rate limiter's `counter` request to `node`, its backend (spec §17 entry 6)? Its
// origin is the limiter itself: the counter is the limiter's request, not a client's.
[[nodiscard]] bool is_limiter_counter(std::span<const ResolvedNode> nodes, std::size_t node,
                                      const SizeArrival& arrival) {
  const auto& origin = nodes[arrival.origin];
  return arrival.op == counter_op && origin.spec.type == model::NodeType::rate_limiter &&
         origin.backend == node;
}

// A byte-sized node's error for an op one of whose arrivals (sorted by origin) has no size,
// naming the first such origin and what to add; a limiter's counter, which no size can fix, gets
// its own message.
[[nodiscard]] std::optional<EngineError> unsized_bytes(const Graph& g, std::size_t node,
                                                       std::span<const SizeArrival> arrivals) {
  const auto nodes = g.nodes();
  const auto& spec = nodes[node].spec;
  if (!defaults::is_byte_sized(spec)) return std::nullopt;
  for (const SizeArrival& arrival : arrivals) {
    if (arrival.size) continue;
    if (is_limiter_counter(nodes, node, arrival)) {
      return EngineError{"node '" + spec.id + "' counts bytes (" + byte_capacity_text(spec) +
                         "), but '" + nodes[arrival.origin].spec.id +
                         "' sends it its counter: a limiter's counter carries no size; its "
                         "backend must count requests"};
    }
    return no_size_error(g, node, arrival.op, arrival.origin);
  }
  return std::nullopt;
}

// A byte-sized node's error for a request of 0 bytes: it would cost 0 MB, so the node could serve
// infinitely many (its demand in requests divides by the size). The loader refuses `0B` as a
// request size; this guards a scenario built in code.
[[nodiscard]] std::optional<EngineError> zero_bytes(std::span<const ResolvedNode> nodes,
                                                    std::size_t node,
                                                    std::span<const SizeArrival> arrivals) {
  const auto& spec = nodes[node].spec;
  if (!defaults::is_byte_sized(spec)) return std::nullopt;
  for (const SizeArrival& arrival : arrivals) {
    if (!arrival.size || arrival.size->count > 0.0) continue;
    return EngineError{"node '" + spec.id + "' counts bytes (" + byte_capacity_text(spec) +
                       "), but op '" + arrival.op + "' from '" + nodes[arrival.origin].spec.id +
                       "' has size " + format_size(*arrival.size) +
                       ": a request's size must be positive"};
  }
  return std::nullopt;
}

// The one size of an op at a node, from its arrivals (sorted by origin): two sizes are an error
// (two_sizes), and so are a zero size and an unsized arrival at a byte-sized node (zero_bytes,
// unsized_bytes). A request-counting node with sized and unsized arrivals resolves to unsized: it
// needs no size, and a byte-sized node further on then reports the unsized path rather than
// inherit a size that only some of the requests have.
// C++23 std::expected<T, E>: the size, or the reason the op has none, in the return value
// (graph.cppm's note at Graph::build has the longer story); replaces an exception for a bad
// scenario, which is user input, and a (bool, out-parameter) pair the caller could ignore.
[[nodiscard]] std::expected<std::optional<Bytes>, EngineError> resolved_size(
    const Graph& g, std::size_t node, std::span<const SizeArrival> arrivals) {
  // C++23 std::unexpected<E>: wraps an error so it converts to the error side of any
  // std::expected<T, E>, replacing a thrown exception or a (bool, out-parameter) pair; a plain
  // `return value;` is the value side. Every fallible step of the build returns its error this
  // way (see Graph::build).
  if (auto conflict = two_sizes(g.nodes(), node, arrivals)) return std::unexpected{*conflict};
  if (auto zero = zero_bytes(g.nodes(), node, arrivals)) return std::unexpected{*zero};
  if (auto unsized = unsized_bytes(g, node, arrivals)) return std::unexpected{*unsized};
  const auto has_no_size = [](const SizeArrival& arrival) { return !arrival.size; };
  if (std::ranges::any_of(arrivals, has_no_size)) return std::nullopt;
  return arrivals.front().size;
}

// A pipe's error for the first request (its arrivals sorted by op, then origin) it cannot price in
// megabytes (spec §19 entry 1: an unsized op on a bandwidth link is refused like one at a
// byte-sized node): an unsized one, naming the client or step that set or dropped its size and what
// to add, a limiter's counter (no size can fix it), or one of 0 bytes (it would cost nothing, and
// the engine divides by its megabytes). Nothing for a link without a bandwidth.
[[nodiscard]] std::optional<EngineError> unpriced_on_pipe(const Graph& g, const ResolvedLink& link,
                                                          std::span<const SizeArrival> arrivals) {
  if (!link.pipe) return std::nullopt;
  const auto nodes = g.nodes();
  const std::string name = "link " + link_label(nodes, link, "'") + " has a bandwidth, but ";
  for (const SizeArrival& arrival : arrivals) {
    const std::string& origin = nodes[arrival.origin].spec.id;
    if (!arrival.size && is_limiter_counter(nodes, demand_receiver(link), arrival)) {
      return EngineError{name + "'" + origin +
                         "' sends its counter over it: a limiter's counter carries no size, so its "
                         "backend link cannot have a bandwidth; remove it"};
    }
    if (!arrival.size) {
      return EngineError{name + "op '" + arrival.op + "' from '" + origin +
                         "' has no size: add size: to the step or sizes: to the client" +
                         pool_renaming_hint(g, arrival.op, arrival.origin)};
    }
    if (!(arrival.size->count > 0.0)) {
      return EngineError{name + "op '" + arrival.op + "' from '" + origin + "' has size " +
                         format_size(*arrival.size) + ": a request's size must be positive"};
    }
  }
  return std::nullopt;
}

// Does a hop for `op`, whose requests weigh `incoming` at the sender (Graph::request_bytes; empty
// when unsized), carry the request's payload on? While the op keeps its name it does. A renamed
// step does only when it states a `size:` equal to the incoming one: that is the body itself
// relabelled (a service that takes an `upload` of 10 MB and stores it as `{op: put, size: 10MB}`),
// which the node had to take in whole before sending it on, so the receiver far down writing it
// makes the first hop a write as well. A renamed step of another size, or of none, is a new request
// of its own (a lookup, a 1 KB metadata write, a notification), whose sender had read the incoming
// bytes already: a 10 MB upload whose service writes `{op: put_meta, size: 1KB}` stays a read on
// the client's wire. Sizes are compared in whole bytes (std::llround): `2.01MB` and `2010KB` parse
// to 2.01 × 1e6 = 2009999.9999999998 and 2010 × 1e3 = 2010000, one rounding apart as doubles but
// one size. A request is a whole number of bytes anyway.
[[nodiscard]] bool carries_payload(std::string_view op, const std::optional<Bytes>& incoming,
                                   const CallHop& hop) {
  if (hop.op == op) return true;
  return hop.size.has_value() && incoming.has_value() &&
         std::llround(hop.size->count) == std::llround(incoming->count);
}

// Is `op` a write at `node`, or at a node the request's payload reaches from there
// (ResolvedLink::write_ops)? The size walk's hops (call_hops and the size-only hops) that carry
// the payload on (carries_payload), under its name or under the name a step of the same size
// gives it. Runs after every (node, op)'s size is resolved (Graph::resolve_request_sizes), so
// the incoming size is Graph::request_bytes. `seen` holds the (node, op) pairs already asked, so
// a loop ends.
[[nodiscard]] bool written_downstream(const Graph& g, std::size_t node, const std::string& op,
                                      std::set<std::pair<std::size_t, std::string>>& seen) {
  if (!seen.emplace(node, op).second) return false;
  if (is_write(g.nodes()[node].spec, op)) return true;
  const auto incoming = g.request_bytes(node, op);
  auto hops = call_hops(g, node, op);
  add_size_only_hops(g, node, op, hops);
  for (const CallHop& hop : hops) {
    if (carries_payload(op, incoming, hop) && written_downstream(g, hop.to, hop.op, seen)) {
      return true;
    }
  }
  return false;
}

// The ops a pipe carries (its arrivals, sorted by op) that are writes for whoever receives them
// under that name (ResolvedLink::write_ops), each once and in order; none for a link without a
// bandwidth.
[[nodiscard]] std::vector<std::string> pipe_write_ops(const Graph& g, const ResolvedLink& link,
                                                      std::span<const SizeArrival> arrivals) {
  std::vector<std::string> writes;
  if (!link.pipe) return writes;
  for (const SizeArrival& arrival : arrivals) {
    if (std::ranges::contains(writes, arrival.op)) continue;
    std::set<std::pair<std::size_t, std::string>> seen;
    if (written_downstream(g, demand_receiver(link), arrival.op, seen)) {
      writes.push_back(arrival.op);
    }
  }
  return writes;
}

// ---- What a che cache's keys are ----

// The key distribution a che cache sees, the clients that send it, and the filters it went
// through (KeyArrival).
struct KeySource {
  model::KeySpec keys{};
  std::vector<std::size_t> clients{};
  std::optional<std::size_t> filtered_by{};
  std::optional<std::size_t> unfiltered_through{};
};

// "the misses of cache 'l1'" (or "of cdn 'edge'": the filter's own type), or "requests no cache
// filtered": one key stream, for messages.
[[nodiscard]] std::string stream_name(std::span<const ResolvedNode> nodes,
                                      std::optional<std::size_t> filtered_by) {
  if (!filtered_by) return "requests no cache filtered";
  const auto& filter = nodes[*filtered_by].spec;
  return "the misses of " + std::string{model::to_string(filter.type)} + " '" + filter.id + "'";
}

// A che cache that is a rate limiter's counter store, and that no client's keys reach otherwise:
// the counter requests carry none (the key walk leaves them out, see the section comment above),
// so there is no popularity to solve Che over. Named for what it is, rather than agreed_keys'
// "no client's demand reaches it", which the limiter's traffic contradicts.
[[nodiscard]] std::optional<EngineError> keyless_counter_store(
    std::span<const ResolvedNode> nodes, const std::string& store, const std::string& name,
    std::span<const KeyArrival> arrivals) {
  if (!arrivals.empty()) return std::nullopt;
  for (const auto& node : nodes) {
    if (node.spec.type != model::NodeType::rate_limiter || node.spec.backend != store) continue;
    return EngineError{name + ": the counter requests of rate limiter '" + node.spec.id +
                       "' carry no client keys (a counter is keyed by user or rule), and no "
                       "client's keys reach it otherwise; give it hit_ratio: {fixed: ...}"};
  }
  return std::nullopt;
}

// The clients' one key distribution (spec §18 entry 1): every client whose ops reach the cache must
// state its keys, and all must agree (KeySpec's defaulted ==, on the canonical form: zipf 0 is
// uniform). Mixing several distributions by their rates is in the Phase 3 backlog.
[[nodiscard]] std::expected<KeySource, EngineError> agreed_keys(
    std::span<const ResolvedNode> nodes, const std::string& name,
    std::span<const KeyArrival> arrivals) {
  if (arrivals.empty()) {
    return std::unexpected{EngineError{name + ": no client's demand reaches it"}};
  }
  std::optional<KeySource> source;
  for (const auto& arrival : arrivals) {
    const auto& client = nodes[arrival.client].spec;
    if (!client.workload || !client.workload->keys) {
      return std::unexpected{
          EngineError{name + ": client '" + client.id + "' reaches it without keys"}};
    }
    const auto keys = model::canonical(*client.workload->keys);
    if (!source) {
      source = KeySource{.keys = keys, .clients = {arrival.client}};
    } else if (keys != source->keys) {
      return std::unexpected{EngineError{name + ": clients '" +
                                         nodes[source->clients.front()].spec.id + "' and '" +
                                         client.id + "' send different key distributions"}};
    } else if (source->clients.back() != arrival.client) {  // sorted by client: one entry each
      source->clients.push_back(arrival.client);
    }
  }
  return *source;
}

// Which filter the cache's READS went through: one, for one popularity. A write is not a
// lookup (it neither hits nor misses), so the writes' path does not shape what Che sees: a
// write_through cache in front passes every write on but only its read misses. A cache
// passed since that filter whose misses could not be filtered (KeyArrival::unfiltered_through)
// is kept for explain's caveat.
[[nodiscard]] std::expected<void, EngineError> agree_on_filter(std::span<const ResolvedNode> nodes,
                                                               std::size_t cache,
                                                               const std::string& name,
                                                               std::span<const KeyArrival> arrivals,
                                                               KeySource& source) {
  // The reads among the arrivals, as pointers into `arrivals`, which both passes below read.
  std::vector<const KeyArrival*> reads;
  for (const auto& arrival : arrivals) {
    if (!is_write(nodes[cache].spec, arrival.op)) reads.push_back(&arrival);
  }
  std::set<std::optional<std::size_t>> filters;
  for (const KeyArrival* read : reads) filters.insert(read->filtered_by);
  if (filters.size() > 1) {
    // Name the filtered streams first: an empty optional (no filter) sorts first in the set.
    std::vector<std::optional<std::size_t>> named(filters.begin(), filters.end());
    if (!named.front()) std::ranges::rotate(named, std::next(named.begin()));
    return std::unexpected{EngineError{name + ": it receives both " + stream_name(nodes, named[0]) +
                                       " and " + stream_name(nodes, named[1])}};
  }
  if (filters.empty()) return {};  // only writes arrive: the clients' keys as they are
  source.filtered_by = *filters.begin();
  for (const KeyArrival* read : reads) {
    if (!read->unfiltered_through) continue;
    source.unfiltered_through = read->unfiltered_through;
    break;
  }
  return {};
}

// A ring partitions the cache's keys only if every request reaches it through a ring: a
// request sent to it any other way could land on any replica, which holds a different part
// of the keys. Mixed feeding has no single popularity per replica, so it is refused.
[[nodiscard]] std::expected<void, EngineError> check_ring_only(
    std::span<const ResolvedNode> nodes, std::size_t cache, std::span<const KeyArrival> arrivals) {
  if (!nodes[cache].keys_partitioned) return {};
  const auto is_ring = [&](std::size_t sender) {
    return nodes[sender].spec.type == model::NodeType::load_balancer &&
           nodes[sender].spec.policy == model::LbPolicy::consistent_hash;
  };
  // C++20 ranges algorithm with a projection: find_if over the arrivals, testing each one's
  // `sender` (the projection, a pointer to member) rather than the whole arrival.
  const auto ring = std::ranges::find_if(arrivals, is_ring, &KeyArrival::sender);
  const auto direct = std::ranges::find_if_not(arrivals, is_ring, &KeyArrival::sender);
  if (ring == arrivals.end() || direct == arrivals.end()) return {};
  return std::unexpected{
      EngineError{"node '" + nodes[cache].spec.id + "': keys are partitioned by ring '" +
                  nodes[ring->sender].spec.id + "' but '" + nodes[direct->sender].spec.id +
                  "' also sends to it directly"}};
}

// Pattern: Strategy/Policy — a cache's hit-ratio model is one of a closed set of policies
// (simsys.caching's HitRatioModel types), chosen once when the graph is built from the
// scenario's `hit_ratio` and dispatched with std::visit. Adding a model (an LFU one, say) is
// one more alternative and one more branch in Graph::resolve_hit_ratio that builds it.
// The policy is not kept after build: a Graph holding a caching::Popularity (in a policy on
// ResolvedNode, in a vector of policies or of CheHitRatio, or returned by an accessor) crashed
// GCC 14.2 (internal compiler error in stl_vector.h compiling simsys.behaviours, Release;
// verified 2026-10-28; see also Graph::resolve_hit_ratio). What rebuilds it is kept instead:
// ResolvedHitRatio's keys, filtered_by and the upstream shards (che_popularity), so phase 3's
// node_down can ask hit_ratio(the smaller capacity) of the same model again.
// C++17 std::variant: a type-safe union that holds exactly one of its alternatives; std::visit
// calls the overload for whichever it holds. Replaces a base class with a virtual hit_ratio
// and a heap-allocated derived object per cache.
using HitRatioPolicy = std::variant<caching::FixedHitRatio, caching::CheHitRatio>;

// The scenario's key distribution as simsys.caching's popularity: Zipf(s) over N keys, or
// uniform over N.
[[nodiscard]] caching::Popularity popularity_of(const model::KeySpec& keys) {
  if (keys.zipf) return caching::Popularity::zipf(*keys.zipf, keys.space);
  return caching::Popularity::uniform(keys.space);
}

// The inputs resolving a hit ratio may read: the graph's nodes (with the hit ratios of the
// che caches this one is behind already resolved, with their policies: Graph::resolve_hit_ratios'
// order), which one is the cache, and how keys arrive at it.
struct HitRatioInputs {
  std::span<const ResolvedNode> nodes;
  std::size_t cache;
  std::span<const KeyArrival> arrivals;
};

// The popularity the cache sees: the clients' keys, or, behind a che cache, that cache's
// popularity filtered by its shards' characteristic times (caching::Popularity::filtered_by;
// Martina, Garetto & Leonardi 2014). Rebuilt from what each resolved hit ratio keeps (the keys,
// the cache it is behind, that cache's shards), one filter per cache of the chain: the upstream
// cache's own popularity is already filtered by whatever is in front of IT. Needs every cache
// of the chain resolved first (Graph::resolve_hit_ratios' order).
[[nodiscard]] caching::Popularity che_popularity(std::span<const ResolvedNode> nodes,
                                                 const ResolvedHitRatio& resolved) {
  if (!resolved.filtered_by) return popularity_of(*resolved.keys);
  const auto& upstream = *nodes[*resolved.filtered_by].hit_ratio;
  return che_popularity(nodes, upstream).filtered_by(upstream.shards);
}

// What a `che` hit ratio rests on, checked: the cache size (`keys`), the key distribution of
// the clients that reach it and the filters it went through (agreed_keys, agree_on_filter),
// that a ring partitioning it is its only way in (check_ring_only), and over how many replicas
// the keys are partitioned. This is also its provenance in explain.
// A cache with N replicas holds `keys` entries per replica; what they hold together depends on
// how keys reach them:
//  - round robin, weighted, least connections: any request can reach any replica, so every
//    replica sees the whole key popularity and fills itself with the same hot keys. Each is
//    a full copy of `keys` entries, and the hit ratio is that of ONE cache of `keys`.
//  - consistent hash (ch. 5): each key hashes to exactly one replica, so the replicas hold
//    disjoint parts of the key space, `keys × N` distinct keys together, each over its own arc
//    of the ring (detail::ring_arcs).
// A replica lost to a fault is not subtracted: the hit ratio is resolved once, at build (phase
// 3 rebuilds the policy from the resolved inputs, see detail::HitRatioPolicy).
[[nodiscard]] std::expected<ResolvedHitRatio, EngineError> che_inputs(
    const HitRatioInputs& inputs) {
  const auto& cache = inputs.nodes[inputs.cache];
  const auto& spec = cache.spec;
  const std::string name = "node '" + spec.id + "': hit_ratio che";
  if (!spec.keys) {
    return std::unexpected{EngineError{name + " needs keys (the cache's capacity in keys)"}};
  }
  if (*spec.keys <= 0) {  // the loader's rule (config's read_cache_size), for code
    return std::unexpected{EngineError{"node '" + spec.id + "': keys must be a positive integer"}};
  }
  if (auto counter_only = keyless_counter_store(inputs.nodes, spec.id, name, inputs.arrivals)) {
    return std::unexpected{std::move(*counter_only)};
  }
  auto source = agreed_keys(inputs.nodes, name, inputs.arrivals);
  if (!source) return std::unexpected{std::move(source.error())};
  if (auto agreed = agree_on_filter(inputs.nodes, inputs.cache, name, inputs.arrivals, *source);
      !agreed) {
    return std::unexpected{std::move(agreed.error())};
  }
  if (auto ring_only = check_ring_only(inputs.nodes, inputs.cache, inputs.arrivals); !ring_only) {
    return std::unexpected{std::move(ring_only.error())};
  }
  const int partitions = cache.keys_partitioned ? spec.replicas : 1;
  return ResolvedHitRatio{.keys = source->keys,
                          .key_clients = std::move(source->clients),
                          .capacity = static_cast<double>(*spec.keys) * partitions,
                          .partitions = partitions,
                          .filtered_by = source->filtered_by,
                          .unfiltered_through = source->unfiltered_through};
}

// `replicas` replicas that each own a share `replica_share` of the key space (ring_arcs).
struct ArcGroup {
  double replica_share;
  double replicas;
};

// How a consistent-hash ring splits the key space over N replicas, from its imbalance k (the
// hottest replica's load over the average, Graph::mark_balanced_targets: ring_imbalance):
// the hottest replica owns a_hot = k/N of the keys, the N − 1 others share the rest evenly,
// a_cool = (1 − k/N)/(N − 1). Two groups, not N arcs: the cool replicas are interchangeable,
// and one Che solve per group keeps a 1000-replica cache as cheap as a 4-replica one. Like k
// itself a heuristic: "the hottest replica as hot as the expected maximum, the others even".
// One group of share 1 for an unpartitioned cache.
[[nodiscard]] std::vector<ArcGroup> ring_arcs(int partitions, double imbalance) {
  if (partitions <= 1) return {{.replica_share = 1.0, .replicas = 1.0}};
  const double n = partitions;
  const double hot = imbalance / n;
  return {{.replica_share = hot, .replicas = 1.0},
          {.replica_share = (1.0 - hot) / (n - 1.0), .replicas = n - 1.0}};
}

// The hit ratio of a cache whose replicas own the arcs `arcs`, `replica_keys` entries each:
//   h = Σ_j a_j · h_model(keys / a_j)
// A replica owning a share a_j of the keys (a random part of every popularity level) sees a_j
// of the requests, and its Che equation over its own part, Σ_{i in part} (1 − e^{−p_i t}) =
// keys, is a_j · Σ_i (1 − e^{−p_i t}) = keys: the equation of ONE cache of keys / a_j over the
// whole space, whose hit ratio is the replica's own. Weighting each replica by its share of
// the requests gives the total. Even arcs (a_j = 1/N) make every term Che(keys × N), the
// "one cache of keys × N" of the even split; uneven ones give less, because the hit ratio is
// concave in the cache size: what the hot replica loses by holding a big arc in the same
// `keys` is more than what the cool ones gain from holding small ones (book ch. 5's hotspot
// problem, seen from the cache: k = 2 over N = 4 gives 0.924 where the even split gives 0.963
// in the consistent-hashing lab). A share of 0 owns nothing and serves nothing.
[[nodiscard]] double hit_ratio_over_arcs(const HitRatioPolicy& policy, double replica_keys,
                                         std::span<const ArcGroup> arcs) {
  double hits = 0.0;
  for (const auto& arc : arcs) {
    if (arc.replica_share <= 0.0) continue;
    // C++20 constrained generic lambda (`const caching::HitRatioModel auto& m`): an `auto`
    // parameter with a concept in front makes the lambda a template whose argument must
    // satisfy the concept. std::visit instantiates it for each alternative of the variant. The
    // call `m.hit_ratio(...)` would fail to compile for a model without one anyway; the concept
    // says up front what every model must provide, and an alternative that is no
    // HitRatioModel fails here naming the concept rather than deep inside std::visit.
    const double replica_hit_ratio = std::visit(
        [&](const caching::HitRatioModel auto& m) {
          return m.hit_ratio(replica_keys / arc.replica_share);
        },
        policy);
    hits += arc.replicas * arc.replica_share * replica_hit_ratio;
  }
  return hits;
}

// Each arc group's key share and characteristic time: what a cache behind this one filters by
// (caching::Popularity::filtered_by). A group's share is all its replicas' together.
[[nodiscard]] std::vector<caching::CacheShard> che_shards(const caching::CheHitRatio& che,
                                                          double replica_keys,
                                                          std::span<const ArcGroup> arcs) {
  std::vector<caching::CacheShard> shards;
  for (const auto& arc : arcs) {
    if (arc.replica_share <= 0.0) continue;
    shards.push_back({.key_share = arc.replicas * arc.replica_share,
                      .characteristic_time = caching::che_characteristic_time(
                          replica_keys / arc.replica_share, che.popularity)});
  }
  return shards;
}

// The order the hit ratios are resolved in: every cache after the che caches whose misses it
// receives (it filters their popularity), found by depth-first search over "is behind" edges;
// a loop of caches each behind the next has no first one and is refused. Caches no other che
// cache feeds come in scenario order.
[[nodiscard]] std::expected<std::vector<std::size_t>, EngineError> hit_ratio_order(
    std::span<const ResolvedNode> nodes, const KeyArrivals& arrivals) {
  enum class Mark { unvisited, active, done };
  std::vector<Mark> marks(nodes.size(), Mark::unvisited);
  std::vector<std::size_t> order;
  // A function object rather than a lambda because it calls itself: one level of recursion
  // per cache in a chain, so a handful at most.
  struct Visit {
    std::span<const ResolvedNode> nodes;
    const KeyArrivals& arrivals;
    std::vector<Mark>& marks;
    std::vector<std::size_t>& order;
    std::expected<void, EngineError> operator()(std::size_t cache) const {
      if (marks[cache] == Mark::done) return {};
      if (marks[cache] == Mark::active) {
        return std::unexpected{EngineError{"node '" + nodes[cache].spec.id +
                                           "': hit_ratio che: its keys loop back through itself "
                                           "(each cache of the loop is behind the next)"}};
      }
      marks[cache] = Mark::active;
      for (const auto& arrival : arrivals[cache]) {
        if (!arrival.filtered_by) continue;
        if (auto visited = (*this)(*arrival.filtered_by); !visited) return visited;
      }
      marks[cache] = Mark::done;
      order.push_back(cache);
      return {};
    }
  };
  const Visit visit{nodes, arrivals, marks, order};
  for (std::size_t node = 0; node < nodes.size(); ++node) {
    if (!has_resolvable_hit_ratio(nodes[node].spec)) continue;
    if (auto visited = visit(node); !visited) return std::unexpected{std::move(visited.error())};
  }
  return order;
}

}  // namespace detail

// Pattern: Static factory (named constructor) with fallible staged construction — the
// static `build` runs a sequence of named construction steps, each of which may fail
// with an EngineError, and hands out a Graph only when all succeeded. The private
// constructor makes a half-built Graph unreachable. (Not a classic Builder: callers do
// not configure the steps.)
std::expected<Graph, EngineError> Graph::build(model::Scenario scenario) {
  Graph graph{std::move(scenario)};
  // C++23 std::expected::and_then: runs the next fallible step only if the previous one
  // succeeded and passes the first error through untouched, replacing an
  // `if (!r) return std::unexpected(r.error());` between every pair of steps.
  const auto built = graph.resolve_nodes()
                         .and_then([&] { return graph.resolve_links(); })
                         .and_then([&] { return graph.connect_consumers(); })
                         .and_then([&] { return graph.connect_steps(); })
                         .and_then([&] { return graph.check_fanout_channels(); })
                         .and_then([&] { return graph.connect_backends(); })
                         .and_then([&] { return graph.route_splitters(); })
                         .and_then([&] { return graph.check_pool_senders(); })
                         .and_then([&] { return graph.mark_balanced_targets(); })
                         .and_then([&] { return graph.resolve_hit_ratios(); })
                         .and_then([&] { return graph.resolve_hops(); })
                         .and_then([&] { return graph.check_routed_ops(); })
                         .and_then([&] { return graph.resolve_request_sizes(); });
  if (!built) return std::unexpected{built.error()};
  graph.order_nodes();
  // The call graph, built once: the asynchronous hops are its back edges, and no hop a request
  // waits for may lie on one of its cycles.
  const detail::ForwardedSteps calls = detail::forwarder_steps(graph);
  graph.async_hops_ = detail::async_hops(graph, calls);
  const auto components = detail::call_components(graph, calls);
  if (auto checked = graph.check_backend_calls(components); !checked) {
    return std::unexpected{checked.error()};
  }
  if (auto checked = graph.check_failure_steps(components); !checked) {
    return std::unexpected{checked.error()};
  }
  return graph;
}

std::optional<std::size_t> Graph::index_of(std::string_view id) const {
  // C++20 ranges algorithm with a projection: compare each node's `spec.id` to `id`
  // without writing a lambda; replaces std::find_if over an iterator pair.
  const auto found = std::ranges::find(
      nodes_, id, [](const ResolvedNode& n) -> std::string_view { return n.spec.id; });
  if (found == nodes_.end()) return std::nullopt;
  return static_cast<std::size_t>(found - nodes_.begin());
}

bool Graph::is_async_hop(std::size_t from, std::string_view op, std::size_t to,
                         std::string_view target_op) const {
  // Binary search: the vector is sorted by AsyncHop's defaulted <=>.
  return std::ranges::binary_search(async_hops_,
                                    AsyncHop{from, std::string{op}, to, std::string{target_op}});
}

std::string link_label(std::span<const ResolvedNode> nodes, const ResolvedLink& link,
                       std::string_view quote) {
  const std::string q{quote};
  return q + nodes[link.from].spec.id + q + "->" + q + nodes[link.to].spec.id + q;
}

std::optional<std::size_t> Graph::demand_link(std::size_t from, std::size_t to) const {
  for (std::size_t i = 0; i < links_.size(); ++i) {
    if (carries_demand(links_[i], from, to)) return i;
  }
  return std::nullopt;
}

// Node index = scenario order. Clients are sources, not queues, so they get no capacity.
// A rate limiter is checked first (it needs a limit), and a leaky bucket's queue is the
// bucket itself (leaky_bucket_capacity), its rate the scenario's `limit`.
Graph::Result Graph::resolve_nodes() {
  // First: the limiter checks divide by the step.
  if (scenario_.step.count() <= 0) return std::unexpected{EngineError{"step must be positive"}};
  nodes_.reserve(scenario_.nodes.size());
  for (const auto& spec : scenario_.nodes) {
    if (auto invalid = invalid_node(spec, scenario_.step)) {
      return std::unexpected{std::move(*invalid)};
    }
    // A client gets no capacity: the other members stay at their defaults.
    nodes_.push_back(spec.type == model::NodeType::client ? ResolvedNode{.spec = spec}
                                                          : resolved_server(spec));
  }
  routes_.resize(nodes_.size());
  arriving_ops_.resize(nodes_.size());
  first_sender_.resize(nodes_.size());
  service_hops_.resize(nodes_.size());
  request_bytes_.resize(nodes_.size());
  return {};
}

// Ids become indices; latency falls back to half the datacenter round trip
// (defaults::resolve_link_latency), cross-datacenter when the two ends sit in different DCs.
Graph::Result Graph::resolve_links() {
  for (const auto& spec : scenario_.links) {
    const auto from = index_of(spec.from);
    const auto to = index_of(spec.to);
    if (!from || !to) {
      return std::unexpected{EngineError{"link " + spec.from + " -> " + spec.to +
                                         " names a node that does not exist"}};
    }
    if (auto invalid = invalid_link_numbers(spec)) return std::unexpected{std::move(*invalid)};
    if (auto invalid = invalid_link_from_database(spec, nodes_[*from].spec, nodes_[*to].spec)) {
      return std::unexpected{std::move(*invalid)};
    }
    const bool cross_dc = nodes_[*from].spec.dc != nodes_[*to].spec.dc;
    const auto latency = defaults::resolve_link_latency(spec, cross_dc);
    const auto steps = whole_delay_steps(latency.value, scenario_.step);
    if (steps > max_delay_steps) {
      return std::unexpected{EngineError{
          "link " + spec.from + " -> " + spec.to + ": latency is " + std::to_string(steps) +
          " steps at this step size; at most " + std::to_string(max_delay_steps) + " steps"}};
    }
    links_.push_back(ResolvedLink{
        .from = *from,
        .to = *to,
        .dir = spec.dir,
        .one_way = latency.value,
        .latency_source = latency.source,
        .delay_steps = static_cast<int>(steps),  // fits: steps <= max_delay_steps
        .ops = spec.ops,
        .weight = spec.weight,
        // C++23 std::optional::transform: maps a present bandwidth through the function and
        // passes an absent one through as empty, replacing `bandwidth ? optional{f(*bandwidth)}
        // : nullopt`: a link has a pipe exactly when it has a bandwidth.
        .pipe = spec.bandwidth.transform(defaults::pipe_capacity),
    });
  }
  return {};
}

// Every step of every node becomes a demand edge, over the link that carries it. Steps come
// from `ops` (per incoming op: services, gateways) and `then` (after processing: workers,
// caches...); the loader never gives a service `then` steps. The steps become resolved hops
// later (resolve_hops), once the hit ratios their miss fractions need are known, and those
// need every demand edge (resolve_hit_ratios walks them upstream to the clients).
Graph::Result Graph::connect_steps() {
  for (std::size_t from = 0; from < nodes_.size(); ++from) {
    const auto& spec = nodes_[from].spec;
    if (auto split = nonadjacent_parallel_steps(spec)) return std::unexpected{std::move(*split)};
    // `views::values` (see size_seeds), then C++20 `views::join`, which flattens those vectors into
    // one lazy sequence of steps. Replaces two nested loops over the map and each vector; nothing
    // is copied.
    for (const model::Step& step : spec.ops | std::views::values | std::views::join) {
      if (auto connected = connect_step(from, step); !connected) return connected;
    }
    for (const model::Step& step : spec.then) {
      if (auto connected = connect_step(from, step); !connected) return connected;
    }
  }
  return {};
}

// Each node's op steps as resolved hops (resolve_service_hops), after resolve_hit_ratios.
Graph::Result Graph::resolve_hops() {
  for (std::size_t node = 0; node < nodes_.size(); ++node) {
    if (auto resolved = resolve_service_hops(node); !resolved) return resolved;
  }
  return {};
}

// The miss fraction 1 - h of a cache or CDN (ServiceHop's comment): 1 when there is no number
// for h. For a CDN the scenario gave no hit ratio that is the scenario's own statement (it misses
// every read, and runs so); for a `che` CDN without a `keys` size and a cache built in code
// without a hit ratio it is the safe side on paper (explain calls the load behind them an upper
// bound, and the engine refuses to run them).
double Graph::miss_fraction(std::size_t cache) const {
  return 1.0 - hit_ratio(cache).value_or(0.0);
}

// The share of requests that take `step` (ServiceHop's `fraction`): every request for an
// unconditional step, every request the step it reports on failed for an `on: failure` step (how
// many failed is the engine's to measure, behaviours::detail::HopBlocks), the miss fraction of
// `looked_up`, the cache the step's lookup asked, for an `on: miss` step (invalid_miss_step has
// made sure there is one).
double Graph::hop_fraction(const model::Step& step, std::optional<std::size_t> looked_up) const {
  // `using enum` (see replica_model_of): the conditions by their short names.
  using enum model::StepCondition;
  switch (step.on) {
    case always:
      return 1.0;
    case failure:
      return 1.0;
    case miss:
      return miss_fraction(*looked_up);
  }
  // C++23 std::unreachable(): tells the compiler control never gets here (every enumerator
  // returned above), replacing a dummy `return 0.0;` that would hide a missing case or
  // compiler-specific __builtin_unreachable(); reaching it is undefined behaviour.
  std::unreachable();
}

// Why an `on: miss` step cannot follow `lookup` (the nearest unconditional step before it, or
// nothing), if it cannot. A miss step needs a cache, asked a read (a write updates or invalidates
// an entry: it has no miss to report), which leaves the miss to the application: a CDN misses too
// but always fetches its misses from its origin, a read_through cache from its backend, and a
// cache with a `then` step on miss forwards them itself; after any of these a miss step would
// fetch each miss twice. And it must wait for the answer: neither it nor the lookup can be a
// parallel branch, or the response would time the fetch alongside the lookup instead of after it.
std::optional<EngineError> Graph::invalid_miss_step(std::size_t node, const model::Step& step,
                                                    const ServiceHop* lookup) const {
  const std::string name = "node '" + nodes_[node].spec.id + "': an 'on: miss' step ";
  if (!lookup || !hits_or_misses(nodes_[lookup->target].spec.type)) {
    return EngineError{name + "must follow a cache step"};
  }
  const auto& cache = nodes_[lookup->target].spec;
  if (is_write(cache, lookup->op)) {
    return EngineError{name + "cannot follow a write to " +
                       std::string{model::to_string(cache.type)} + " '" + cache.id +
                       "' (writes do not miss)"};
  }
  if (cache.type == model::NodeType::cdn) {
    return EngineError{name + "cannot follow cdn '" + cache.id +
                       "' (the cdn already fetched the miss from its origin)"};
  }
  if (fetches_read_misses(cache)) {
    return EngineError{name + "cannot follow read_through cache '" + cache.id +
                       "' (the cache already fetched the miss)"};
  }
  if (forwards_own_misses(cache)) {
    return EngineError{name + "cannot follow cache '" + cache.id +
                       "', whose own then step on miss already fetches the miss"};
  }
  if (step.mode == model::StepMode::parallel || lookup->mode == model::StepMode::parallel) {
    return EngineError{name +
                       "and the cache step it follows must be sequential (the miss step waits for "
                       "the cache's answer)"};
  }
  return std::nullopt;
}

// Why an `on: failure` step cannot report on `last_unconditional` (the nearest unconditional step
// before it, or nothing), if it cannot. It needs a step to report on, and it runs once that step
// has failed (spec §5: a sequential hop), so it cannot be timed beside it in a parallel group.
std::optional<EngineError> Graph::invalid_failure_step(
    std::size_t node, const model::Step& step,
    std::optional<std::size_t> last_unconditional) const {
  const std::string name = "node '" + nodes_[node].spec.id + "': an on: failure step ";
  if (!last_unconditional) return EngineError{name + "needs a step before it"};
  if (step.mode == model::StepMode::parallel) {
    return EngineError{name +
                       "must be sequential (it runs after the step it reports on has failed)"};
  }
  return std::nullopt;
}

// Each op's steps in order, with the share of requests that take each and the requests each
// sends (ServiceHop). A conditional step belongs to the nearest unconditional step before it: an
// `on: miss` step to its lookup, which must ask a cache for a read (invalid_miss_step), and an
// `on: failure` step to the step whose failures it is sent (invalid_failure_step):
// [cache.get, db.read on miss, cache.set on miss] is one lookup and what follows its miss, and in
// [db.write, log.write on failure, audit.write on failure] both failure steps report on db.write.
// connect_step has already resolved every target.
//
// A ws_gateway's message ops fan out (phase 3 spec §8.2; book ch. 12: a message to a group reaches
// each member) WHERE THE BOOK COPIES THEM: ch. 12's chat server stores a message once (one id from
// the id generator, one row in the message store) and puts one copy per recipient into the
// message sync queues, which the workers deliver. So a step of a message op (model::message_op:
// not connect, heartbeat or deliver) that targets a message_queue sends `fanout` times its own
// factor, fanout × factor copies per message, and every other step of it one request per message
// (the Task 14 review's correction: multiplying every step stored each message 1.4 times). The
// engine (the behaviours read these hops) and explain's paper load both see it. An `on: miss` step
// to a queue is multiplied too: its lookup asks a cache, which no fan-out multiplies, so the
// fan-out enters its factor once.
Graph::Result Graph::resolve_service_hops(std::size_t node) {
  const auto& spec = nodes_[node].spec;
  for (const auto& [op, steps] : spec.ops) {
    auto& hops = service_hops_[node][op];
    const bool fans_out = spec.type == model::NodeType::ws_gateway && model::message_op(op);
    std::optional<std::size_t> last_unconditional;  // its position in `hops`
    for (const model::Step& step : steps) {
      auto hop = resolve_service_hop(node, step, hops, last_unconditional);
      if (!hop) return std::unexpected{std::move(hop.error())};
      if (fans_out && copies_per_recipient(hop->target)) hop->factor *= model::fanout_of(spec);
      if (step.on == model::StepCondition::always) last_unconditional = hops.size();
      hops.push_back(std::move(*hop));
    }
  }
  return {};
}

// Whether a gateway's message step to `target` carries one copy per recipient
// (resolve_service_hops): it does when `target` is a message queue, ch. 12's message sync queue.
bool Graph::copies_per_recipient(std::size_t target) const {
  return nodes_[target].spec.type == model::NodeType::message_queue;
}

// One step of node `node`'s op, resolved against the op's hops before it (`hops`, the last
// unconditional one at `last_unconditional`), or why it cannot be.
std::expected<ServiceHop, EngineError> Graph::resolve_service_hop(
    std::size_t node, const model::Step& step, std::span<const ServiceHop> hops,
    std::optional<std::size_t> last_unconditional) const {
  ServiceHop hop{
      .target = *index_of(step.to), .op = step.op, .mode = step.mode, .factor = step.factor};
  std::optional<std::size_t> looked_up;  // the cache a miss step's lookup asked
  if (step.on == model::StepCondition::miss) {
    const ServiceHop* asked = last_unconditional ? &hops[*last_unconditional] : nullptr;
    if (auto invalid = invalid_miss_step(node, step, asked)) {
      return std::unexpected{std::move(*invalid)};
    }
    looked_up = asked->target;
    hop.factor *= asked->factor;  // every missed lookup is fetched (ServiceHop)
    hop.lookup = last_unconditional;
  }
  if (step.on == model::StepCondition::failure) {
    if (auto invalid = invalid_failure_step(node, step, last_unconditional)) {
      return std::unexpected{std::move(*invalid)};
    }
    hop.reports_on = last_unconditional;
  }
  hop.fraction = hop_fraction(step, looked_up);
  return hop;
}

// Every cache's and CDN's hit ratio, once, before anything reads one (resolve_hops' miss
// fractions, the backend shares, the arriving ops): the stated number, or the Che
// approximation over the popularity of the keys that reach the cache (resolve_hit_ratio).
// Runs after the last route exists, since the key walk follows them (detail::key_arrivals),
// and after mark_balanced_targets, since a ring in front partitions the cache's keys. The walk
// runs only when some che cache needs it, and the caches resolve in detail::hit_ratio_order:
// a cache behind another che cache filters that cache's popularity, so that one comes first.
Graph::Result Graph::resolve_hit_ratios() {
  const bool any_che = std::ranges::any_of(
      nodes_, [](const ResolvedNode& node) { return detail::filters_by_che(node.spec); });
  const auto arrivals = any_che ? detail::key_arrivals(*this) : detail::KeyArrivals(nodes_.size());
  const auto order = detail::hit_ratio_order(nodes_, arrivals);
  if (!order) return std::unexpected{order.error()};
  for (const auto cache : *order) {
    const detail::HitRatioInputs inputs{
        .nodes = nodes_, .cache = cache, .arrivals = arrivals[cache]};
    if (auto resolved = resolve_hit_ratio(cache, *nodes_[cache].spec.hit_ratio, inputs);
        !resolved) {
      return resolved;
    }
  }
  return {};
}

// One cache's hit ratio, with one branch on what the scenario states: the policy its
// `hit_ratio` names (FixedHitRatio{h}, or CheHitRatio over the popularity of its keys:
// detail::che_inputs and detail::che_popularity), evaluated over the ring's arcs
// (detail::hit_ratio_over_arcs; one arc of share 1 for a cache no ring partitions, and for
// every fixed one), plus the shards a che cache behind this one filters by. The numbers are
// stored on the node rather than returned in a std::expected, and the policy is dropped once
// evaluated: a std::expected of a struct holding the variant, like a Graph holding the
// policy, crashed GCC 14.2 (see detail::HitRatioPolicy).
Graph::Result Graph::resolve_hit_ratio(std::size_t cache, const model::HitRatio& stated,
                                       const detail::HitRatioInputs& inputs) {
  ResolvedHitRatio resolved;
  detail::HitRatioPolicy policy = caching::FixedHitRatio{.value = stated.fixed.value_or(0.0)};
  if (stated.che) {
    auto checked = detail::che_inputs(inputs);
    if (!checked) return std::unexpected{std::move(checked.error())};
    resolved = std::move(*checked);
    policy = caching::CheHitRatio{.popularity = detail::che_popularity(nodes_, resolved)};
  }
  const auto arcs = detail::ring_arcs(resolved.partitions, nodes_[cache].imbalance);
  const double replica_keys = static_cast<double>(nodes_[cache].spec.keys.value_or(0));
  resolved.value = detail::hit_ratio_over_arcs(policy, replica_keys, arcs);
  if (const auto* che = std::get_if<caching::CheHitRatio>(&policy)) {
    resolved.shards = detail::che_shards(*che, replica_keys, arcs);
  }
  nodes_[cache].hit_ratio = std::move(resolved);
  return {};
}

std::optional<double> Graph::hit_ratio(std::size_t node) const {
  const auto& resolved = nodes_.at(node).hit_ratio;
  if (!resolved) return std::nullopt;
  return resolved->value;
}

// Every (node, op)'s one size, from the size walk (detail::size_walk), after every route, step and
// backend exists and after collect_arriving_ops: nodes in scenario order, each node's ops in name
// order, so the first error is the same on every run. C++23 views::chunk_by splits a node's
// arrivals (sorted by op) into runs of one op each, replacing a hand-written loop that tracks where
// each run starts; each run is contiguous, so it is passed on as a span. Then every op that arrives
// at a byte-sized node must have a size, however it arrived (check_sized_arrivals), and every
// request a bandwidth link carries must have one, links in scenario order
// (detail::unpriced_on_pipe: a pipe prices megabytes, spec §19 entry 3). The same walk says which
// of a pipe's ops are writes downstream (detail::pipe_write_ops).
Graph::Result Graph::resolve_request_sizes() {
  const auto walk = detail::size_walk(*this);
  for (std::size_t node = 0; node < nodes_.size(); ++node) {
    for (const auto op_arrivals : walk.at_nodes[node] | std::views::chunk_by(detail::same_op)) {
      const std::span<const detail::SizeArrival> one_op{op_arrivals};
      auto size = detail::resolved_size(*this, node, one_op);
      if (!size) return std::unexpected{std::move(size.error())};
      if (*size) request_bytes_[node].emplace(one_op.front().op, **size);
    }
    if (auto checked = check_sized_arrivals(node); !checked) return checked;
  }
  for (std::size_t link = 0; link < links_.size(); ++link) {
    if (auto unpriced = detail::unpriced_on_pipe(*this, links_[link], walk.over_links[link])) {
      return std::unexpected{std::move(*unpriced)};
    }
    links_[link].write_ops = detail::pipe_write_ops(*this, links_[link], walk.over_links[link]);
  }
  return {};
}

// Every op that arrives at a byte-sized node has a size. The size walk refuses an unsized
// arrival it finds (detail::unsized_bytes); this catches one it cannot find, because
// collect_arriving_ops brings it by a path the walk does not take from a client (a worker
// pool's unsized `then` step with no queue feeding the pool: the step fires for whatever the
// pool receives). The message names the first node collect_arriving_ops saw send it.
Graph::Result Graph::check_sized_arrivals(std::size_t node) const {
  if (!defaults::is_byte_sized(nodes_[node].spec)) return {};
  for (const auto& op : arriving_ops_[node]) {
    if (request_bytes_[node].contains(op)) continue;
    return std::unexpected{detail::no_size_error(*this, node, op, first_sender_[node].at(op))};
  }
  return {};
}

std::optional<Bytes> Graph::request_bytes(std::size_t node, std::string_view op) const {
  const auto& by_op = request_bytes_.at(node);
  const auto found = by_op.find(op);
  if (found == by_op.end()) return std::nullopt;
  return found->second;
}

std::optional<double> client_timeout_s(const model::NodeSpec& client) {
  if (!client.workload || !client.workload->timeout) return std::nullopt;
  return seconds(*client.workload->timeout);
}

double op_cost(const Graph& g, std::size_t node, std::string_view op) {
  const auto size = g.request_bytes(node, op);  // first: it checks `node` (vector::at)
  const auto& spec = g.nodes()[node].spec;
  if (!defaults::is_byte_sized(spec)) return defaults::op_cost(spec, op);
  if (size) return defaults::byte_cost(*size);
  // Unreachable from a built Graph: check_sized_arrivals refused every unsized arriving op.
  if (std::ranges::binary_search(g.arriving_ops(node), op)) {
    throw std::logic_error{"op_cost: op '" + std::string{op} + "' arrives at byte-sized node '" +
                           spec.id + "' without a size"};
  }
  return defaults::op_cost(spec, op);  // never arrives: no flow of it is charged here
}

std::span<const ServiceHop> Graph::service_hops(std::size_t node, std::string_view op) const {
  const auto& by_op = service_hops_.at(node);
  const auto found = by_op.find(op);
  if (found == by_op.end()) return {};
  return found->second;
}

// Per step, as the engine's failover reads it (engine::FaultSchedule::add_failover_routes): any
// step of (node, op) to `target` with a discovery lookup before it is reached through the lookup,
// so `[svc, disc.lookup, svc]` reaches svc through it by its second call (the first is not held).
bool Graph::lookup_before(std::size_t node, std::string_view op, std::size_t target) const {
  const auto hops = service_hops(node, op);
  for (std::size_t position = 0; position < hops.size(); ++position) {
    if (hops[position].target == target && discovery_before_step(node, op, position)) return true;
  }
  return false;
}

// A step is a discovery lookup when it calls a service_discovery and asks `lookup`: a plain
// service with an op of that name is no registry. The nearest one before `position` wins.
std::optional<std::size_t> Graph::discovery_before_step(std::size_t node, std::string_view op,
                                                        std::size_t position) const {
  const auto hops = service_hops(node, op);
  std::optional<std::size_t> discovery;
  for (std::size_t before = 0; before < position && before < hops.size(); ++before) {
    const ServiceHop& hop = hops[before];
    const bool registry = nodes_[hop.target].spec.type == model::NodeType::service_discovery;
    if (registry && hop.op == "lookup") discovery = hop.target;
  }
  return discovery;
}

// Which ops go to the store is sends_to_store's answer (the strategy dispatch; a CDN is
// read_through towards its origin); this adds how many: every write that goes, the misses of a
// read that goes. CacheBehaviour (and so CdnBehaviour), the paper load and the call-cycle search
// all read this share.
double Graph::backend_fraction(std::size_t node, std::string_view op) const {
  const auto& resolved = nodes_.at(node);
  if (!hits_or_misses(resolved.spec.type) || !resolved.backend) return 0.0;
  if (!sends_to_store(resolved.spec, op)) return 0.0;
  return is_write(resolved.spec, op) ? 1.0 : miss_fraction(node);
}

double Graph::then_fraction(std::size_t node, const model::Step& step, std::string_view op) const {
  const auto& spec = nodes_.at(node).spec;
  using enum model::StepCondition;  // see hop_fraction
  switch (step.on) {
    case always:
      if (spec.type == model::NodeType::blob_store) return is_write(spec, op) ? 1.0 : 0.0;
      // The share the dedup step lets through: a duplicate is published to no channel.
      if (spec.type == model::NodeType::notification_fanout) return 1.0 - spec.dedup;
      return 1.0;
    case failure:
      return 0.0;
    case miss:
      if (!hits_or_misses(spec.type) || is_write(spec, op)) return 0.0;
      return miss_fraction(node);
  }
  std::unreachable();
}

Graph::Result Graph::connect_step(std::size_t from, const model::Step& step) {
  const auto& sender = nodes_[from].spec.id;
  const auto to = index_of(step.to);
  if (!to) {
    return std::unexpected{EngineError{"node '" + sender + "' sends '" + step.op + "' to '" +
                                       step.to + "' which does not exist"}};
  }
  // A client is a source: it has no queue and no ops to absorb demand, so flow sent to it
  // would vanish without being served or dropped.
  if (nodes_[*to].spec.type == model::NodeType::client) {
    return std::unexpected{EngineError{"node '" + sender + "' sends '" + step.op + "' to client '" +
                                       step.to + "'; clients only send"}};
  }
  const auto link = demand_link(from, *to);
  if (!link) {
    return std::unexpected{EngineError{"node '" + sender + "' sends '" + step.op + "' to '" +
                                       step.to + "' but no link connects them"}};
  }
  if (!carries_op(links_[*link], step.op)) {
    return std::unexpected{EngineError{"link " + sender + " -> " + step.to +
                                       " does not carry op '" + step.op +
                                       "' (ops: " + joined(links_[*link].ops, ", ") + ")"}};
  }
  add_edge(DemandEdge{.from = from, .to = *to, .link = *link});
  return {};
}

// A rate limiter's `backend` is the counter store it consults once per request it checks (spec §17
// entry 6): a demand edge limiter -> backend carrying op `counter`, on the link that carries demand
// between them. The loader checked that the backend exists and is a cache; a scenario built in code
// is checked for existence here. A cache whose strategy has it call the store
// (model::needs_backend) gets a demand edge to its backend in the same way, and a CDN one to its
// origin, which carries its read misses (connect_store, one rule for both); a cache_aside cache has
// none (the service's `on: miss` step does the store read, and invalid_cache refuses a backend on
// it).
Graph::Result Graph::connect_backends() {
  for (std::size_t node = 0; node < nodes_.size(); ++node) {
    const auto& spec = nodes_[node].spec;
    if (spec.type == model::NodeType::cdn) {
      if (auto connected = connect_store(node, "origin"); !connected) return connected;
      continue;
    }
    if (!spec.backend) continue;
    if (spec.type == model::NodeType::rate_limiter) {
      if (auto connected = connect_backend(node); !connected) return connected;
    } else if (is_cache(spec) && model::needs_backend(spec.strategy)) {
      if (auto connected = connect_store(node, "backend"); !connected) return connected;
    }
  }
  return {};
}

// The store behind a cache (its `backend`, `role` "backend") or a CDN (its `origin`, spec §18 entry
// 10: the read_through hop of a cache whose backend is the origin) becomes ResolvedNode::backend,
// with a demand edge over the link that carries demand between the two. One rule for both, each
// message in the node's own words: "backend 'db' needs a link from the cache", "origin 'x' needs a
// link from the cdn". invalid_cache and invalid_cdn proved the name is there; the loader checks the
// same (its reference check, config's read_cdn_origin). Which ops travel on the edge is known only
// once every node's arriving ops are (check_cache_backend_ops), and whether a request can wait for
// the hop only once the call cycles are (check_backend_calls).
Graph::Result Graph::connect_store(std::size_t node, std::string_view role) {
  const auto& spec = nodes_[node].spec;
  const std::string& store_id = role == "origin" ? *spec.origin : *spec.backend;
  const std::string name = "node '" + spec.id + "': " + std::string{role};
  const auto store = index_of(store_id);
  if (!store) {
    return std::unexpected{EngineError{name + " refers to unknown node '" + store_id + "'"}};
  }
  if (nodes_[*store].spec.type == model::NodeType::client) {  // see connect_step
    return std::unexpected{
        EngineError{name + " '" + store_id + "' is a client; clients only send"}};
  }
  const auto link = demand_link(node, *store);
  if (!link) {
    return std::unexpected{EngineError{name + " '" + store_id + "' needs a link from the " +
                                       std::string{model::to_string(spec.type)}}};
  }
  nodes_[node].backend = *store;
  nodes_[node].backend_link = *link;
  add_edge(DemandEdge{.from = node, .to = *store, .link = *link});
  return {};
}

// A CDN serves static content from the edge and has no write path (book ch. 1: the origin is
// where content changes), so a write op (model::write_ops_of: `write` unless the node lists its
// own) that reaches one is a mistake in the scenario. The message names the op and a node that
// sends it: "node 'edge': a cdn does not take writes ('upload' from 'users')".
Graph::Result Graph::check_reads_only(std::size_t cdn) const {
  const auto& spec = nodes_[cdn].spec;
  for (const auto& op : arriving_ops_[cdn]) {
    if (!is_write(spec, op)) continue;
    return std::unexpected{EngineError{"node '" + spec.id + "': a cdn does not take writes ('" +
                                       op + "' from '" + sender_of(cdn, op) + "')"}};
  }
  return {};
}

// Does node `from` send `op` to node `to`? Along a client route of that op, a resolved step that
// some request takes, a balancer's or limiter's route that carries an op it receives, a cache's
// or CDN's store hop that carries some of an op it receives, or a `then` step. The arriving ops
// say WHAT reaches a node; this says from WHOM, for messages (check_reads_only).
bool Graph::sends(std::size_t from, std::string_view op, std::size_t to) const {
  const auto& spec = nodes_[from].spec;
  const bool receives = std::ranges::contains(arriving_ops_[from], op);
  const bool is_client = spec.type == model::NodeType::client;
  for (const auto& route : routes_[from]) {
    if (route.target != to || route.share <= 0.0) continue;
    if (is_client ? route.op == op : receives && carries_op(links_[route.link], op)) return true;
  }
  for (const auto& hops : service_hops_[from] | std::views::values) {
    for (const ServiceHop& hop : hops) {
      if (hop.target == to && hop.op == op && hop.fraction > 0.0) return true;
    }
  }
  if (nodes_[from].backend == to && receives && backend_fraction(from, op) > 0.0) return true;
  const auto& to_id = nodes_[to].spec.id;
  return std::ranges::any_of(
      spec.then, [&](const model::Step& step) { return step.to == to_id && step.op == op; });
}

// The first node, in scenario order, that sends `op` to `node` (sends).
std::string Graph::sender_of(std::size_t node, std::string_view op) const {
  for (std::size_t from = 0; from < nodes_.size(); ++from) {
    if (sends(from, op, node)) return nodes_[from].spec.id;
  }
  return "?";  // not reached: an op arrives at a node only along one of the hops sends checks
}

// Every op a cache passes on to its backend must be one the link carries, or the cache would send
// flow the link refuses: the same rule and message as a step's (connect_step). And every write it
// passes on must be a write at the store too (write_counted_as_read): the consistent-hashing lab
// under write_through would otherwise show its database's primary idle while every `set` lands on
// it.
Graph::Result Graph::check_cache_backend_ops(std::size_t cache) {
  const auto& resolved = nodes_[cache];
  if (!resolved.backend) return {};
  const auto& link = links_[*resolved.backend_link];
  const auto& store = nodes_[*resolved.backend].spec;
  for (const auto& op : arriving_ops_[cache]) {
    if (backend_fraction(cache, op) <= 0.0) continue;
    if (!carries_op(link, op)) {
      return std::unexpected{EngineError{"link " + resolved.spec.id + " -> " + store.id +
                                         " does not carry op '" + op +
                                         "' (ops: " + joined(link.ops, ", ") + ")"}};
    }
    if (!is_write(resolved.spec, op)) continue;
    if (auto disagreement = write_counted_as_read(resolved.spec, op, store)) {
      return std::unexpected{std::move(*disagreement)};
    }
  }
  return {};
}

// A cache's fetch from its store is always waited for: a read_through miss is answered with what
// the store returns, a write_through write once the store has it. A CDN's fetch from its origin is
// a read_through miss, and a rate limiter asks its counter store before it admits a request. So
// none of these hops can be an asynchronous hand-off, and a store that calls back into the node in
// front of it (db.read -> cache.read -> db.read) asks a request to wait for itself: the scenario is
// refused. "On a call cycle" is asked of the call graph's strongly connected components
// (detail::call_components), not of the hop the cycle search happened to cut
// (detail::find_async_hops), so the answer does not depend on where the clients enter the loop.
// (write_back's flush is not a call: nobody waits.)
Graph::Result Graph::check_backend_calls(const detail::CallComponents& components) const {
  for (std::size_t node = 0; node < nodes_.size(); ++node) {
    const auto& resolved = nodes_[node];
    if (!resolved.backend) continue;
    for (const auto& op : arriving_ops_[node]) {
      const auto waited = detail::waited_backend_op(*this, node, op);
      if (!waited || !components.on_common_cycle({node, op}, {*resolved.backend, *waited})) {
        continue;
      }
      const std::string role = resolved.spec.type == model::NodeType::cdn ? "origin" : "backend";
      return std::unexpected{EngineError{"node '" + resolved.spec.id + "': " + role + " '" +
                                         nodes_[*resolved.backend].spec.id +
                                         "' closes a call cycle"}};
    }
  }
  return {};
}

// An `on: failure` step whose call lies on a call cycle with the op it reports from (web.write ->
// retrier.retry -> web.write) sends every failed request back into the op that failed it: a retry
// with no bound, each retry that fails again retried again, with no limit and no backoff. Real
// systems bound it (ch. 10's notification system retries a failed notification a few times, then
// gives up), and the model's bounded forms are a client's `retries` and a pool's (phase 3). The
// call-cycle search would make the hop asynchronous and the run would show a drop rate with no
// word on its cause, so the scenario is refused instead, the components asked as for the backend
// hops (check_backend_calls). A failure step that publishes to a message queue ends the call at the
// queue (its deliveries are asynchronous, detail::consumer_deliveries) and is no call cycle.
Graph::Result Graph::check_failure_steps(const detail::CallComponents& components) const {
  for (std::size_t node = 0; node < nodes_.size(); ++node) {
    for (const auto& [op, hops] : service_hops_[node]) {
      for (const ServiceHop& hop : hops) {
        if (!hop.reports_on || !components.on_common_cycle({node, op}, {hop.target, hop.op})) {
          continue;
        }
        return std::unexpected{EngineError{"node '" + nodes_[node].spec.id +
                                           "': an on: failure step to '" +
                                           nodes_[hop.target].spec.id +
                                           "' closes a call cycle (an unbounded retry: use the "
                                           "client's retries)"}};
      }
    }
  }
  return {};
}

Graph::Result Graph::connect_backend(std::size_t limiter) {
  const auto& spec = nodes_[limiter].spec;
  const auto backend = index_of(*spec.backend);
  if (!backend) {
    return std::unexpected{EngineError{"node '" + spec.id + "': backend refers to unknown node '" +
                                       *spec.backend + "'"}};
  }
  const auto link = demand_link(limiter, *backend);
  if (!link) {
    return std::unexpected{EngineError{"node '" + spec.id + "': backend '" + *spec.backend +
                                       "' needs a link from the limiter"}};
  }
  if (!carries_op(links_[*link], counter_op)) {
    return std::unexpected{
        EngineError{"link " + spec.id + " -> " + *spec.backend +
                    " does not carry op 'counter' (ops: " + joined(links_[*link].ops, ", ") + ")"}};
  }
  // `counter` is an INCR, a write: the store must count it as one (write_counted_as_read).
  if (auto disagreement = write_counted_as_read(spec, counter_op, nodes_[*backend].spec)) {
    return std::unexpected{std::move(*disagreement)};
  }
  nodes_[limiter].backend = *backend;
  nodes_[limiter].backend_link = *link;
  add_edge(DemandEdge{.from = limiter, .to = *backend, .link = *link});
  return {};
}

// The nodes that split flow over their outgoing links: their routes, and the demand edges
// those routes add (the last ones the graph has).
Graph::Result Graph::route_splitters() {
  if (auto routed = route_clients(); !routed) return routed;
  for (std::size_t node = 0; node < nodes_.size(); ++node) {
    if (copies_to_consumers(nodes_[node].spec.type)) {
      if (auto routed = route_queue(node); !routed) return routed;
    }
    if (!forwards_by_link(nodes_[node].spec.type)) continue;
    if (auto routed = route_forwarder(node); !routed) return routed;
  }
  return {};
}

// A message queue delivers to its consumer groups (spec §19 entry 4): every link it sends demand on
// must reach a worker pool that `consumes` it, since a broker hands a message only to the groups
// subscribed to it (anything else behind it would be a service the scenario believes is fed and is
// not); a link to anything else is refused, naming it. Each such link is one Route of share 1:
// every group reads EVERY message (a load balancer splits, shares summing to 1; a queue copies), as
// the same op. A drained link (weight 0) is a group that reads nothing (share 0), as everywhere a
// weight of 0 means a route nobody takes; any other weight changes nothing, a group cannot read
// part of the log. A queue with no consumer at all is refused: it would answer its publishers and
// keep their messages for nobody, a pipeline the scenario believes runs and that ends at the
// broker.
Graph::Result Graph::route_queue(std::size_t queue) {
  const auto& id = nodes_[queue].spec.id;
  for (std::size_t i = 0; i < links_.size(); ++i) {
    const auto target = demand_target(links_[i], queue);
    if (!target) continue;
    const auto& consumer = nodes_[*target].spec;
    if (consumer.type != model::NodeType::worker_pool || consumer.consumes != id) {
      return std::unexpected{EngineError{"node '" + id +
                                         "': a message queue delivers only to the worker pools "
                                         "that consume it ('" +
                                         consumer.id + "' does not)"}};
    }
    const double share = links_[i].weight > 0.0 ? 1.0 : 0.0;
    routes_[queue].push_back(Route{.target = *target, .link = i, .share = share});
    add_edge(DemandEdge{.from = queue, .to = *target, .link = i});
  }
  if (routes_[queue].empty()) {
    return std::unexpected{
        EngineError{"node '" + id +
                    "': no worker pool consumes this message queue (its messages would be "
                    "kept for nobody)"}};
  }
  return {};
}

// A notification fanout's `channels` and its `then` steps name the same message queues (phase 3
// spec §8.3), in both directions, so that the map is the student-facing list of the channels and no
// step publishes to a queue the list leaves out:
//   node 'fanout': channel 'email' names 'email_q', which no then step publishes to
//   node 'fanout': then step publishes to 'email_q', which no channel names
// A step to a node that is not a message queue (the notification log, a kv_cluster) is no channel
// and is not checked; a fanout with no channels is fine while no step publishes to a queue. A
// channel that names an undeclared node or one of another type is refused in the loader's words,
// for a fanout written in code. Run after connect_steps, which refuses a step to an unknown node.
Graph::Result Graph::check_fanout_channels() const {
  for (const auto& node : nodes_) {
    if (node.spec.type != model::NodeType::notification_fanout) continue;
    if (auto checked = check_channels_named(node.spec); !checked) return checked;
    if (auto checked = check_steps_named(node.spec); !checked) return checked;
  }
  return {};
}

// Each channel names a declared message queue that some `then` step publishes to.
Graph::Result Graph::check_channels_named(const model::NodeSpec& fanout) const {
  const std::string name = "node '" + fanout.id + "': ";
  for (const auto& [channel, queue] : fanout.channels) {
    const std::string named = "channel '" + channel + "' names ";
    const auto index = index_of(queue);
    if (!index) {
      return std::unexpected{EngineError{name + named + "'" + queue + "', which is not declared"}};
    }
    const auto type = nodes_[*index].spec.type;
    if (type != model::NodeType::message_queue) {
      return std::unexpected{EngineError{name + named + "'" + queue + "', which is " +
                                         with_article(type) + " (channels name message queues)"}};
    }
    // std::ranges::contains with a projection: each step is compared by its `to`, no lambda.
    if (!std::ranges::contains(fanout.then, queue, &model::Step::to)) {
      return std::unexpected{
          EngineError{name + named + "'" + queue + "', which no then step publishes to"}};
    }
  }
  return {};
}

// Each `then` step to a message queue publishes to a queue some channel names.
Graph::Result Graph::check_steps_named(const model::NodeSpec& fanout) const {
  for (const model::Step& step : fanout.then) {
    const auto target = index_of(step.to);
    if (!target || nodes_[*target].spec.type != model::NodeType::message_queue) continue;
    if (std::ranges::contains(fanout.channels | std::views::values, step.to)) continue;
    return std::unexpected{EngineError{"node '" + fanout.id + "': then step publishes to '" +
                                       step.to + "', which no channel names"}};
  }
  return {};
}

// A worker pool's jobs come from the queue it consumes and from nowhere else: a demand edge into a
// pool from any other node (a client's route, a service's step, a balancer's link, another pool's
// `then` step) is refused, naming the sender. Such a sender would make the pool a server it calls:
// the loader requires `consumes`, and the pool's room is that queue's retention (connect_consumer),
// a 7-day room in which a direct caller would wait days and never be dropped. A queue's own link is
// the only way in, and it is asynchronous (detail::consumer_deliveries): nobody waits for a pool.
// Run after route_splitters, the last step that adds demand edges.
Graph::Result Graph::check_pool_senders() const {
  for (const DemandEdge& edge : edges_) {
    const auto& pool = nodes_[edge.to].spec;
    if (pool.type != model::NodeType::worker_pool) continue;
    const auto& sender = nodes_[edge.from].spec;
    if (pool.consumes == sender.id) continue;
    return std::unexpected{EngineError{"node '" + pool.id +
                                       "': a worker pool takes jobs only from the queue it "
                                       "consumes ('" +
                                       sender.id + "' sends to it)"}};
  }
  return {};
}

// Each consumer link of a queue whose `ops` filter names ops must carry one the queue receives: a
// filter that carries none of them leaves the group with nothing to read, a consumer in name only.
// Needs the arriving ops (collect_arriving_ops). A drained link (weight 0) reads nothing by design
// and is not asked.
Graph::Result Graph::check_consumer_links(std::size_t queue) const {
  for (const Route& route : routes_[queue]) {
    const auto& link = links_[route.link];
    if (link.ops.empty() || route.share <= 0.0) continue;
    const auto carried = [&](const std::string& op) { return carries_op(link, op); };
    if (std::ranges::any_of(arriving_ops_[queue], carried)) continue;
    return std::unexpected{
        EngineError{"link " + link_label(nodes_, link, "'") + " carries none of the ops '" +
                    nodes_[queue].spec.id + "' receives (link ops: " + joined(link.ops, ", ") +
                    "; it receives: " + joined(arriving_ops_[queue], ", ") + ")"}};
  }
  return {};
}

// Each worker pool's queue (spec §19 entries 4-5), checked from the pool's side: the node it
// `consumes` exists, is a message queue or a URL frontier (can_be_consumed), and links to the pool,
// or the pool would sit idle while the scenario says it drains the queue. The pool's waiting room
// is then the messages its queue keeps for it (connect_consumer).
Graph::Result Graph::connect_consumers() {
  for (std::size_t pool = 0; pool < nodes_.size(); ++pool) {
    if (nodes_[pool].spec.type != model::NodeType::worker_pool) continue;
    if (auto connected = connect_consumer(pool); !connected) return connected;
  }
  return {};
}

// A pool's room (spec §19 entry 4): Kafka deletes a message older than the queue's `retention`, so
// at most the messages that `retention` seconds of draining would have read can wait for the group:
// queue_limit = retention × the pool's capacity (workers / cost),
//   10 s × 1200 jobs/s = 12,000 messages; the default 7 d × 1200/s = 725.76 million.
// A message older than that is deleted unconsumed: the fluid queue drops what overflows the room,
// and the drops are the pool's. Each group's room is its own (retention × ITS capacity). The
// retention is the queue's, or defaults::default_retention when it states none; a pool that reads a
// frontier (which has no retention) has a room without end (room_of). A pool built in code without
// `consumes` is refused in the loader's words: it could never receive a job, since
// check_pool_senders refuses every sender but its queue. Assumed: the backlog drains first in,
// first out, so a message published now waits the FIFO drain time of the backlog ahead of it
// (backlog over drain rate: its queue lag, engine::NodeSample:: queue_lag_s) and the room in
// messages is the room in seconds of draining.
Graph::Result Graph::connect_consumer(std::size_t pool) {
  auto& resolved = nodes_[pool];
  const auto& spec = resolved.spec;
  if (!spec.consumes) {
    return std::unexpected{EngineError{"node '" + spec.id + "': worker_pool needs consumes"}};
  }
  const std::string name = "node '" + spec.id + "': consumes ";
  const auto queue = index_of(*spec.consumes);
  if (!queue) {
    return std::unexpected{EngineError{name + "refers to unknown node '" + *spec.consumes + "'"}};
  }
  const auto& source = nodes_[*queue].spec;
  if (!can_be_consumed(source.type)) {
    return std::unexpected{EngineError{name + "'" + source.id + "' (type " +
                                       std::string{model::to_string(source.type)} +
                                       "); a worker pool consumes a message_queue or a frontier"}};
  }
  if (!demand_link(*queue, pool)) {
    return std::unexpected{EngineError{name + "'" + source.id + "', but no link from '" +
                                       source.id + "' delivers its messages to it"}};
  }
  resolved.capacity.queue_limit = room_of(source, resolved.capacity.service_rate);
  return {};
}

// The room of a pool that reads `source`: retention x the pool's rate behind a message queue (its
// retention, or defaults::default_retention), and no end at all behind a URL frontier, which has no
// retention and deletes nothing: a URL waits until a downloader takes it, however long the backlog
// (+infinity: the fluid queue drops nothing, and `estimate` reports a backlog that grows for ever
// as unbounded, fluid::FluidQueue::settle).
double Graph::room_of(const model::NodeSpec& source, Rate pool_rate) {
  if (source.type == model::NodeType::frontier) return std::numeric_limits<double>::infinity();
  const SimTime retention = source.retention.value_or(defaults::default_retention);
  return seconds(retention) * pool_rate.per_second;
}

// What follows from the splits and the resolved hops: which ops reach each node (through any
// chain of balancers and limiters), whether every op reaching one can leave it, that no write
// reaches a CDN, and that every consumer link of a queue carries something it receives. (What each
// balancer's policy does to the replicas behind it is marked earlier, right after the splits,
// because a che cache's capacity depends on it: mark_balanced_targets.)
Graph::Result Graph::check_routed_ops() {
  collect_arriving_ops();
  for (std::size_t node = 0; node < nodes_.size(); ++node) {
    if (hits_or_misses(nodes_[node].spec.type)) {
      if (auto checked = check_cache_backend_ops(node); !checked) return checked;
    }
    if (nodes_[node].spec.type == model::NodeType::cdn) {
      if (auto checked = check_reads_only(node); !checked) return checked;
    }
    if (copies_to_consumers(nodes_[node].spec.type)) {
      if (auto checked = check_consumer_links(node); !checked) return checked;
    }
    if (!forwards_by_link(nodes_[node].spec.type)) continue;
    if (auto checked = check_forwarded_ops(node); !checked) return checked;
  }
  return {};
}

Graph::Result Graph::route_clients() {
  for (std::size_t client = 0; client < nodes_.size(); ++client) {
    const auto& workload = nodes_[client].spec.workload;
    if (nodes_[client].spec.type != model::NodeType::client || !workload) continue;
    for (const auto& op : workload->op_shares | std::views::keys) {
      if (auto routed = route_client_op(client, op); !routed) return routed;
    }
  }
  return {};
}

// A client op goes out on every link from the client that carries it, split by the
// links' weights: share_i = weight_i / sum of weights. This is how a scenario says "90%
// of video bytes from the CDN, 10% from origin" or "3:1 between two load balancers" — a
// fixed split, as weighted DNS or round-robin would give. Weight 0 means a drained
// backend (as in the loader): it gets no share, and an op whose every link is drained
// has nowhere to go, which is an error rather than a silent equal split.
Graph::Result Graph::route_client_op(std::size_t client, const std::string& op) {
  struct Candidate {
    std::size_t link, target;
  };
  const auto& id = nodes_[client].spec.id;
  std::vector<Candidate> eligible;
  double total_weight = 0;
  for (std::size_t i = 0; i < links_.size(); ++i) {
    const auto target = demand_target(links_[i], client);
    if (!target || !carries_op(links_[i], op)) continue;
    if (nodes_[*target].spec.type == model::NodeType::client) {  // see connect_step
      return std::unexpected{EngineError{"client '" + id + "': link to client '" +
                                         nodes_[*target].spec.id + "' cannot carry op '" + op +
                                         "'; clients only send"}};
    }
    eligible.push_back({i, *target});
    total_weight += links_[i].weight;
  }
  if (eligible.empty()) {
    return std::unexpected{EngineError{"client '" + id + "': no link carries op '" + op + "'"}};
  }
  if (total_weight <= 0) {
    return std::unexpected{
        EngineError{"client '" + id + "': every link carrying op '" + op + "' has weight 0"}};
  }
  for (const auto& [link, target] : eligible) {
    const double share = links_[link].weight / total_weight;
    routes_[client].push_back(
        ClientRoute{.op = op, .target = target, .link = link, .share = share});
    add_edge(DemandEdge{.from = client, .to = target, .link = link});
  }
  return {};
}

// A load balancer forwards whatever reaches it, so it is routed per LINK, not per op: one
// Route on every link it sends demand on, its share the link's weight over the weights of
// all of them (Route's comment; routes_for narrows that to the links carrying one op). The
// split is the same for every policy: link weights spread traffic over DIFFERENT
// downstream nodes, while the policy (mark_balanced_targets) is about how the replicas
// INSIDE one downstream node share it. `weighted` is the policy that says the weights are
// the point; round robin over equal weights (the default 1) is an even split.
// A rate limiter forwards what it admits the same way, except over its backend link, which
// carries only its own `counter` requests (connect_backend). A limiter with no other link
// is a terminal gate: a service asks it "may I?" and it answers (notification-system's
// per-user limiter); a balancer with nowhere to send is an error.
Graph::Result Graph::route_forwarder(std::size_t node) {
  const auto& id = nodes_[node].spec.id;
  const auto backend_link = nodes_[node].backend_link;
  double total_weight = 0;
  for (std::size_t i = 0; i < links_.size(); ++i) {
    const auto target = demand_target(links_[i], node);
    if (!target || i == backend_link) continue;
    if (nodes_[*target].spec.type == model::NodeType::client) {  // see connect_step
      return std::unexpected{EngineError{"node '" + id + "': link to client '" +
                                         nodes_[*target].spec.id +
                                         "' cannot carry demand; clients only send"}};
    }
    routes_[node].push_back(Route{.target = *target, .link = i, .share = links_[i].weight});
    total_weight += links_[i].weight;
    add_edge(DemandEdge{.from = node, .to = *target, .link = i});
  }
  if (routes_[node].empty() && nodes_[node].spec.type == model::NodeType::load_balancer) {
    return std::unexpected{
        EngineError{"node '" + id + "': a load balancer needs an outgoing link"}};
  }
  // All weights 0 leaves every share 0; check_forwarded_ops reports it per op.
  for (auto& route : routes_[node]) route.share = total_weight > 0 ? route.share / total_weight : 0;
  return {};
}

std::vector<Route> Graph::routes_for(std::size_t node, std::string_view op) const {
  std::vector<Route> taken;
  if (nodes_.at(node).spec.type == model::NodeType::client) {
    for (const auto& route : routes_[node]) {
      if (route.op == op) taken.push_back(route);
    }
    return taken;
  }
  double total = 0;
  for (const auto& route : routes_[node]) {
    if (!carries_op(links_[route.link], op)) continue;
    taken.push_back(Route{
        .op = std::string{op}, .target = route.target, .link = route.link, .share = route.share});
    total += route.share;
  }
  if (copies_to_consumers(nodes_[node].spec.type)) return taken;  // copies: each share stays 1
  for (auto& route : taken) route.share = total > 0 ? route.share / total : 0;
  return taken;
}

// Which ops reach every node: a worklist over (node, op) pairs, seeded with the client
// routes that carry flow (not a drained one: the size walk skips it too), the steps that carry
// flow (the unconditional `then` steps of the node types that do not hit or miss, and every
// resolved hop of positive fraction: an `on: miss` step after a cache that misses) and the
// `counter` op of every limiter's backend, and grown through load balancers
// and rate limiters, which pass each op they receive on to every route whose link carries it and
// is not drained (weight 0: no flow takes it), through caches, which pass each op they receive
// on along every `then` step that carries some of THAT op (carries_flow: an `on: miss` step
// carries the read misses, so a cache that only ever receives writes sends nothing along it),
// through caches and CDNs, which pass an op on to their backend (a CDN's origin) when some
// of it goes there (backend_fraction), and through message queues, which pass every op on to
// every consumer group whose link carries it. Each pair enters the list once, so a loop ends; the
// first node seen sending it is kept for messages (first_sender_).
void Graph::collect_arriving_ops() {
  std::vector<std::map<std::string, std::size_t, std::less<>>> arriving(nodes_.size());
  std::vector<std::pair<std::size_t, std::string>> work;
  const auto arrive_from = [&](std::size_t sender, std::size_t node, const std::string& op) {
    if (arriving[node].try_emplace(op, sender).second) work.emplace_back(node, op);
  };
  for (std::size_t node = 0; node < nodes_.size(); ++node) {
    seed_arrivals(
        node, [&](std::size_t target, const std::string& op) { arrive_from(node, target, op); });
  }
  while (!work.empty()) {
    const auto [node, op] = std::move(work.back());
    work.pop_back();
    pass_on(node, op,
            [&](std::size_t target, const std::string& sent) { arrive_from(node, target, sent); });
  }
  for (std::size_t node = 0; node < nodes_.size(); ++node) {
    const auto ops = arriving[node] | std::views::keys;  // sorted: a map
    arriving_ops_[node].assign(ops.begin(), ops.end());
    first_sender_[node] = std::move(arriving[node]);
  }
}

// What `node` sends whatever reaches it: a client's routes that carry flow, its hops of positive
// fraction, its unconditional `then` steps unless they follow the op, a limiter's `counter`.
void Graph::seed_arrivals(std::size_t node, const Arrive& arrive) const {
  const auto& spec = nodes_[node].spec;
  if (spec.type == model::NodeType::client) {
    for (const auto& route : routes_[node]) {
      if (route.share > 0.0) arrive(route.target, route.op);
    }
  }
  for (const auto& hops : service_hops_[node] | std::views::values) {
    for (const ServiceHop& hop : hops) {
      if (hop.fraction > 0.0) arrive(hop.target, hop.op);
    }
  }
  // A cache's then steps depend on the op (a miss step carries no write), and so do a blob
  // store's (an event fires for a write only): they follow each op that arrives (pass_on). (A CDN
  // has none.) Every other node's unconditional ones fire whatever it receives (a worker pool's
  // too: they fire for every job it drains, whichever op its queue brought).
  if (!then_steps_follow_the_op(spec.type)) {
    for (const model::Step& step : spec.then) {
      if (step.on == model::StepCondition::always) arrive(*index_of(step.to), step.op);
    }
  }
  if (spec.type == model::NodeType::rate_limiter && nodes_[node].backend) {
    arrive(*nodes_[node].backend, std::string{counter_op});
  }
}

// Where `op`, once it reached `node`, goes next: along the `then` steps that follow the op, to a
// cache's or a CDN's backend, or over a forwarder's or a queue's routes that carry it.
void Graph::pass_on(std::size_t node, const std::string& op, const Arrive& arrive) const {
  const auto type = nodes_[node].spec.type;
  if (then_steps_follow_the_op(type)) {
    for (const model::Step& step : nodes_[node].spec.then) {
      if (carries_flow(node, step, op)) arrive(*index_of(step.to), step.op);
    }
  }
  if (hits_or_misses(type)) {
    if (backend_fraction(node, op) > 0.0) arrive(*nodes_[node].backend, op);
    return;
  }
  if (!forwards_by_link(type) && !copies_to_consumers(type)) return;
  for (const auto& route : routes_[node]) {
    if (route.share > 0.0 && carries_op(links_[route.link], op)) arrive(route.target, op);
  }
}

// Does a `then` step of cache or blob store `node` carry some of the requests of `op` that arrive
// at it? then_fraction says: an unconditional one all of them (a blob store's, all of a write and
// none of a read), an `on: miss` one the misses of a read (none if the cache never misses) and
// nothing of a write, an `on: failure` one nothing (refused in a then list: then_fraction).
bool Graph::carries_flow(std::size_t node, const model::Step& step, std::string_view op) const {
  return then_fraction(node, step, op) > 0.0;
}

// Every op that reaches a load balancer must be able to leave it, or the balancer would
// swallow it: the same two rules as a client op (route_client_op), with the node's wording.
// So must every op reaching a rate limiter that forwards at all; a limiter with no route
// (a terminal gate, see route_forwarder) answers every op itself.
Graph::Result Graph::check_forwarded_ops(std::size_t node) {
  const auto& id = nodes_[node].spec.id;
  if (routes_[node].empty()) return {};  // a terminal limiter (a balancer has a route)
  for (const auto& op : arriving_ops_[node]) {
    double total_weight = 0;
    bool carried = false;
    for (const auto& route : routes_[node]) {
      if (!carries_op(links_[route.link], op)) continue;
      carried = true;
      total_weight += links_[route.link].weight;
    }
    if (!carried) {
      return std::unexpected{EngineError{"node '" + id + "': no link carries op '" + op + "'"}};
    }
    if (total_weight <= 0) {
      return std::unexpected{
          EngineError{"node '" + id + "': every link carrying op '" + op + "' has weight 0"}};
    }
  }
  return {};
}

// A load balancer's policy changes how the replicas of the node BEHIND it queue (spec §17 entry 5),
// so its ReplicaModel is stored on that node, where the engine builds the queues
// (engine::Simulation::make_server):
//  - round_robin, weighted -> independent: requests are dealt out blindly, every replica is
//    its own queue (N independent M/M/c); nothing to mark.
//  - least_connections -> pooled: a request goes to the replica with the fewest in flight,
//    so no replica idles while another has a queue. Idealised as join-the-shortest-queue:
//    the replicas behave as ONE pool of N × c servers (M/M/(c·N)), `pooled`.
//  - consistent_hash with v virtual nodes per server -> imbalanced: keys land on ring arcs
//    of uneven length, so some of the M replicas holds more than 1/M of the keys (book ch. 5:
//    the standard deviation of the load is about 10% of the mean at 100 vnodes, 5% at 200).
//    The engine models the HOTTEST replica apart from the cooler ones, and needs its share:
//      imbalance k = 1 + z_M / sqrt(v)       (ring_imbalance with one copy per key; z_M the
//                                             expected maximum of M standard normals, M the
//                                             replicas behind the balancer: its ring's machines)
//    4 replicas: v = 100: 1.105; v = 150: 1.086; v = 1: 2.049. Capped at M (a single replica
//    is the average). ring_imbalance has the derivation, its limits and its Monte Carlo check;
//    a kv_cluster's own ring uses the same function, so the two agree.
// A drained route (weight 0) sends the target nothing and imposes no model. A node behind
// two balancers whose policies give different models would need two queueing models at
// once, so that is an error; the same model from different policies (round_robin and
// weighted) is fine. A SQL database takes no model from any balancer (its driver picks the
// copy), so any balancers may front one. Two consistent_hash rings with different vnodes keep the
// larger imbalance (the hotter ring decides the hottest replica). A node reached both through a
// balancer and directly (a client or a step sending to it) gets the balancer's model for
// ALL its traffic: the model belongs to the replica group, not to one path into it.
Graph::Result Graph::mark_balanced_targets() {
  struct Marked {
    ReplicaModel model;
    std::size_t balancer;
  };
  std::vector<std::optional<Marked>> marked(nodes_.size());
  for (std::size_t balancer = 0; balancer < nodes_.size(); ++balancer) {
    const auto& spec = nodes_[balancer].spec;
    if (spec.type != model::NodeType::load_balancer) continue;
    // An absent policy (the loader leaves it absent, as code may) is round_robin.
    const auto replica_model = replica_model_of(spec.policy.value_or(model::LbPolicy::round_robin));
    const int vnodes = spec.vnodes.value_or(model::default_vnodes);
    if (vnodes <= 0) {
      return std::unexpected{
          EngineError{"node '" + spec.id + "': vnodes must be a positive integer"}};
    }
    for (const auto& route : routes_[balancer]) {
      if (route.share <= 0.0) continue;  // drained: sends nothing, imposes nothing
      auto& target = nodes_[route.target];
      // A database's driver, not the balancer, picks its copy: writes go to the primary, reads
      // to a replica, and a sharding key spreads the shards evenly. So no balancer's model
      // applies to it (engine::Simulation::make_database): it imposes neither a pool nor a hot
      // copy, and two balancers of different models in front of one database do not conflict.
      if (target.spec.type == model::NodeType::sql_db) continue;
      // Nor to a kv_cluster's: its coordinator sends each request to the key's N replicas on its
      // own ring (apply_kv_ring), whatever a balancer in front of it does.
      if (target.spec.type == model::NodeType::kv_cluster) continue;
      auto& earlier = marked[route.target];
      if (earlier && earlier->model != replica_model) {
        return std::unexpected{EngineError{
            "node '" + target.spec.id +
            "' is behind load balancers with different replica models ('" +
            nodes_[earlier->balancer].spec.id + "' " + std::string{to_string(earlier->model)} +
            ", '" + spec.id + "' " + std::string{to_string(replica_model)} + ")"}};
      }
      if (!earlier) earlier = Marked{.model = replica_model, .balancer = balancer};
      if (replica_model == ReplicaModel::pooled) target.pooled = true;
      if (replica_model == ReplicaModel::imbalanced) {
        target.keys_partitioned = true;  // each key hashes to one replica
        // Imbalance factor 1 + z_M/sqrt(v) (see the function comment), at most M: the hottest
        // of M replicas cannot carry more than all of the demand, and one replica alone
        // carries exactly the average (factor 1). One copy of each key on a balancer's ring.
        const double factor = ring_imbalance(target.spec.replicas, 1, vnodes);
        target.imbalance = std::max(target.imbalance, factor);
      }
    }
  }
  return {};
}

// Two steps to the same target over the same link are one arrow of the diagram.
void Graph::add_edge(const DemandEdge& edge) {
  if (!std::ranges::contains(edges_, edge)) edges_.push_back(edge);
}

// A cycle-closing link always delays by at least one step, so the engine reads the
// previous step's flow on it: spec §3.1 "cycles use the previous step's value". That is
// all it decides. Whether the reply path waits for a hop over it is an op-level question
// (detail::find_async_hops): chat's `sync_workers -> chat_servers` closes a node loop,
// but if the op it sends there makes no further calls, the sync worker still waits for
// that reply, read one step stale by the backward pass.
void Graph::mark_feedback(std::size_t link) {
  links_[link].feedback = true;
  links_[link].delay_steps = std::max(links_[link].delay_steps, 1);
}

// Topological order by depth-first search, Tarjan-style edge classification: a node is
// white (unvisited), grey (on the DFS path) or black (finished). An edge to a grey node is
// a back edge, and a back edge is exactly an edge that closes a cycle; marking those as
// feedback leaves a DAG, whose reverse post-order is a topological order (every node after
// the nodes that send to it). An edge into a black node (a cross or forward edge into a
// finished subtree) closes no cycle and stays an ordinary edge. Roots come from
// dfs_roots: clients first, so in the chat loop of the tests the walk goes
// u -> chat -> q -> sync and the back edge is sync -> chat, whatever the file order (one
// client enters that loop; see dfs_roots for several).
// The feedback set is inclusion-minimal, not minimum: every back edge closes a cycle made
// of the DFS tree's edges, so none can be dropped, but a different walk might cut the
// same cycles with fewer edges (finding the minimum feedback arc set is NP-hard).
// Iterative (an explicit stack) rather than recursive, so a long chain cannot overflow
// the call stack.
void Graph::order_nodes() {
  std::vector<std::vector<const DemandEdge*>> outgoing(nodes_.size());
  for (const auto& edge : edges_) outgoing[edge.from].push_back(&edge);

  enum class Colour { white, grey, black };
  std::vector<Colour> colour(nodes_.size(), Colour::white);
  std::vector<std::size_t> post_order;
  post_order.reserve(nodes_.size());

  struct Frame {
    std::size_t node;
    std::size_t next_edge;
  };
  for (const std::size_t root : dfs_roots(nodes_)) {
    if (colour[root] != Colour::white) continue;
    std::vector<Frame> stack{{root, 0}};
    colour[root] = Colour::grey;
    while (!stack.empty()) {
      auto& frame = stack.back();
      if (frame.next_edge == outgoing[frame.node].size()) {
        colour[frame.node] = Colour::black;
        post_order.push_back(frame.node);
        stack.pop_back();
        continue;
      }
      const DemandEdge& edge = *outgoing[frame.node][frame.next_edge++];
      if (colour[edge.to] == Colour::grey) {
        mark_feedback(edge.link);
      } else if (colour[edge.to] == Colour::white) {
        colour[edge.to] = Colour::grey;
        stack.push_back({edge.to, 0});  // invalidates `frame`; the loop re-reads back()
      }
    }
  }
  // Reverse post-order: a node finishes only after everything it sends to, so the
  // last to finish is the first in topological order.
  order_.assign(post_order.rbegin(), post_order.rend());
}

// ---- politeness (phase 3 Task 18, spec §8.4; book ch. 9) ----

std::optional<Rate> politeness_cap(std::optional<int> hosts, std::optional<Rate> per_host) {
  if (!hosts || !per_host) return std::nullopt;
  return Rate{static_cast<double>(*hosts) * per_host->per_second};
}

std::vector<std::size_t> frontiers_of(const Graph& g) {
  std::vector<std::size_t> frontiers;
  for (std::size_t node = 0; node < g.nodes().size(); ++node) {
    if (g.nodes()[node].spec.type == model::NodeType::frontier) frontiers.push_back(node);
  }
  return frontiers;
}

std::optional<Rate> drain_cap_of(const Graph& g, std::size_t node) {
  const auto& spec = g.nodes()[node].spec;
  if (spec.type == model::NodeType::worker_pool && spec.consumes) {
    const auto& source = g.nodes()[*g.index_of(*spec.consumes)].spec;  // connect_consumer checked
    if (source.type != model::NodeType::frontier) return std::nullopt;
    return politeness_cap(source.hosts, source.politeness);
  }
  if (spec.type == model::NodeType::external_sink && spec.politeness) {
    const auto frontiers = frontiers_of(g);
    if (frontiers.size() != 1) return std::nullopt;
    return politeness_cap(g.nodes()[frontiers.front()].spec.hosts, spec.politeness);
  }
  return std::nullopt;
}

}  // namespace simsys::graph
