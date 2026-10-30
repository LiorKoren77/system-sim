// Module `simsys.config`, implementation unit: the `nodes` map, read in two passes. The first
// declares every node: its id and type, the common keys (`dc`, `replicas`, `capacity`,
// `latency`, `advanced`), a per-op `cost` and a load balancer's keys. The second reads each
// body's references and traffic (its steps: steps.cpp; a client's workload: workload.cpp) and
// every other type's own keys: rate limiter, id generator, cache, SQL database, CDN, blob store,
// message queue, worker pool, search index, external sink, key-value cluster, websocket gateway,
// notification fanout.
module;
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

// C++20 module implementation unit: `module simsys.config;` (no `export`) makes this file
// part of the named module. It sees everything the interface declared, exports nothing,
// and can include heavy headers in its own global module fragment, like a .cpp behind a
// header.
module simsys.config;

import simsys.core;
// Only for defaults::max_concurrency (the cap the engine's defaults apply is the cap the loader
// enforces, one number in one place), defaults::pool_retry_after (the wait a pool with `retries`
// and no `retry_after` gets) and defaults::failover (a service discovery's, when it states none).
import simsys.defaults;
// Only for fluid::Capacity's default zone thresholds (detail.hpp): one source of truth for
// 0.7 / 0.9.
import simsys.fluid;
import simsys.model;

// The loader's shared vocabulary and common readers, included in the module purview after
// the imports: its declarations belong to simsys.config, like this unit's own, and can
// name ConfigError (see the header's comment for why it is no module partition).
#include "detail.hpp"

namespace simsys::config::detail {

// ---- Node declarations: the first pass ------------------------------------------------

// Unnamed namespace: the helpers in these blocks are this unit's own (internal linkage).
// Outside it a function of a module implementation unit has module linkage, so every unit
// of simsys.config could call it and the linker sees its name; only what detail.hpp declares
// is shared between the units.
namespace {

bool accepts_key(model::NodeType type, std::string_view key) {
  if (lists_word(common_node_keys, key)) return true;
  if (type != model::NodeType::client && lists_word(server_node_keys, key)) return true;
  return lists_word(own_keys_by_type[std::to_underlying(type)].keys, key);
}

// A misspelt key ("capactiy") would otherwise be ignored and the type default used.
CheckResult check_allowed_keys(const Site& site, const YAML::Node& body,
                               const model::NodeSpec& node) {
  for (const auto& entry : body) {
    auto key = key_name(site, entry.first, "key");
    if (!key) return std::unexpected(key.error());
    if (accepts_key(node.type, *key)) continue;
    return site.fail(entry.first, std::format("key '{}' is not valid for type '{}'", *key,
                                              model::to_string(node.type)));
  }
  return {};
}

// A node without `dc` lives in the first declared datacenter: `main` when the file
// declares none (the schema's default), the only one in a single-datacenter file.
CheckResult read_dc(const Site& site, const YAML::Node& body, model::NodeSpec& node,
                    const Datacenters& datacenters) {
  if (!body["dc"]) {
    node.dc = datacenters.front();
    return {};
  }
  auto name = scalar_value(site, body, "dc");
  if (!name) return std::unexpected(name.error());
  // C++23 std::ranges::contains: "is this value in the range?" in one call, replacing
  // `std::find(begin, end, value) != end`.
  if (!std::ranges::contains(datacenters, *name)) {
    return site.fail(body["dc"], std::format("unknown datacenter '{}'", *name));
  }
  node.dc = std::move(*name);
  return {};
}

// `what` names the key in the message: "replicas", "advanced.concurrency".
std::expected<int, ConfigError> read_positive_int(const Site& site, const YAML::Node& value,
                                                  std::string_view what) {
  const auto number = parse_whole<int>(number_text(value));
  if (number && *number > 0) return *number;
  return site.fail(value, std::format("{} must be a positive integer", what));
}

CheckResult read_replicas(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  if (!body["replicas"]) return {};
  if (auto checked = check_not_null(site, body, "replicas", "replicas"); !checked) return checked;
  return read_positive_int(site, body["replicas"], "replicas").transform(store_in(node.replicas));
}

// Capacity counts requests for most node types and bytes for storage ("500MB/s", or in bits as
// a network is rated: "40Gbit/s"). The "B/s" or "bit/s" suffix decides which parser reads it and
// which field receives it, so a text that neither parser accepts gets the message of the parser
// its suffix selects.
std::expected<void, ParseError> read_capacity_text(std::string_view text, model::NodeSpec& node) {
  // C++20 std::string_view::ends_with (and its twin starts_with): a suffix test in one call,
  // replacing compare(size() - n, n, suffix) and its length arithmetic.
  if (text.ends_with("B/s") || text.ends_with("bit/s")) {
    return parse_byte_rate(text).transform(store_in(node.byte_capacity));
  }
  return parse_rate(text).transform(store_in(node.capacity));
}

// "a service", "an external_sink": a node type with its article, for messages.
std::string type_with_article(model::NodeType type) {
  const auto name = model::to_string(type);
  // C++23 std::string_view::contains (the map's C++20 contains, for text): "is this character
  // among those?" in one call, replacing `find(c) != npos`.
  const bool vowel = std::string_view{"aeiou"}.contains(name.front());
  return std::format("{} {}", vowel ? "an" : "a", name);
}

// Only the types that serve bytes may count them (spec §19 entry 2, defaults::is_byte_sized): a CDN
// and a blob store. Every other type counts requests, and a byte rate there would have to be read
// as some number of requests, which no author means.
CheckResult check_byte_capacity_type(const Site& site, const YAML::Node& capacity,
                                     const model::NodeSpec& node) {
  if (!node.byte_capacity) return {};
  if (node.type == model::NodeType::cdn || node.type == model::NodeType::blob_store) return {};
  return site.fail(capacity, std::format("{} counts requests; a byte capacity belongs on a cdn or "
                                         "blob_store",
                                         type_with_article(node.type)));
}

// A byte capacity is bounded by the same number, in bytes: 1e15 B/s is 1e9 megabytes
// per second, far below the request-rate bound.
CheckResult check_capacity_bounded(const Site& site, const YAML::Node& capacity,
                                   const model::NodeSpec& node) {
  if (node.capacity && node.capacity->per_second > max_rate_per_second) {
    return site.fail(capacity, "capacity must be at most 1e15/s");
  }
  if (node.byte_capacity && node.byte_capacity->bytes_per_second > max_rate_per_second) {
    return site.fail(capacity, "capacity must be at most 1e15B/s");
  }
  return {};
}

CheckResult read_capacity(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  const auto capacity = body["capacity"];
  if (!capacity) return {};
  return scalar_value(site, body, "capacity")
      .and_then([&](const std::string& text) {
        return read_capacity_text(text, node).transform_error([&](const ParseError& error) {
          return unit_error(site, capacity, "capacity", error);
        });
      })
      .and_then([&] { return check_capacity_bounded(site, capacity, node); })
      .and_then([&] { return check_byte_capacity_type(site, capacity, node); });
}

const AdvancedNumber* find_advanced_number(std::string_view key) {
  // A ranges algorithm with a projection: `&AdvancedNumber::key` makes find compare each
  // row's key with `key`, replacing a hand-written lambda comparing one member.
  const auto row = std::ranges::find(advanced_numbers, key, &AdvancedNumber::key);
  return row == advanced_numbers.end() ? nullptr : &*row;
}

std::expected<double, ConfigError> read_advanced_number(const Site& site, const YAML::Node& value,
                                                        const AdvancedNumber& rule) {
  const auto number = number_value(value);
  if (number && rule.in_range(*number)) return *number;
  return site.fail(value, std::format("advanced.{} must be {}", rule.key, rule.requirement));
}

bool is_advanced_key(std::string_view name) {
  return lists_word(advanced_integer_keys, name) || find_advanced_number(name) != nullptr;
}

// The c of M/M/c, at most defaults::max_concurrency: the Erlang C term costs O(c) per node
// per step, so an unbounded c would let one scenario line stall every step of a run.
CheckResult read_concurrency(const Site& site, const YAML::Node& value, const std::string& label,
                             model::NodeSpec& node) {
  auto servers = read_positive_int(site, value, label);
  if (!servers) return std::unexpected(servers.error());
  if (*servers > defaults::max_concurrency) {
    return site.fail(value, std::format("{} must be at most {}", label, defaults::max_concurrency));
  }
  node.advanced.concurrency = *servers;
  return {};
}

// One known, non-null advanced value into its field. store_in also assigns the int that
// read_positive_int produces to queue_limit's std::optional<double>.
CheckResult read_advanced_value(const Site& site, const YamlElement& entry, std::string_view name,
                                model::NodeSpec& node) {
  const auto label = std::format("advanced.{}", name);
  if (name == "concurrency") return read_concurrency(site, entry.second, label, node);
  if (name == "queue_limit") {
    return read_positive_int(site, entry.second, label)
        .transform(store_in(node.advanced.queue_limit));
  }
  const auto& rule = *find_advanced_number(name);
  return read_advanced_number(site, entry.second, rule)
      .transform(store_in(node.advanced.*(rule.target)));
}

CheckResult read_advanced_entry(const Site& site, const YamlElement& entry, model::NodeSpec& node) {
  const auto name = key_name(site, entry.first, "advanced key");
  if (!name) return std::unexpected(name.error());
  if (!is_advanced_key(*name)) {
    return site.fail(entry.first, std::format("unknown advanced key '{}'", *name));
  }
  if (auto checked = check_entry_not_null(site, entry, std::format("advanced.{}", *name));
      !checked) {
    return checked;
  }
  return read_advanced_value(site, entry, *name, node);
}

// "advanced.yellow", or "advanced.yellow (default 0.7)" when the scenario left it out, so
// the message never names only a value the user did not write.
std::string threshold_label(std::string_view name, std::optional<double> given, double fallback) {
  if (given) return std::format("advanced.{}", name);
  return std::format("advanced.{} (default {})", name, fallback);
}

// The zones must be ordered: green below yellow, yellow below red. A threshold given alone
// is compared with the engine's default for the other one.
CheckResult check_thresholds_ordered(const Site& site, const YAML::Node& advanced,
                                     const model::NodeSpec& node) {
  const auto& a = node.advanced;
  if (!a.yellow && !a.red) return {};
  if (a.yellow.value_or(default_yellow) < a.red.value_or(default_red)) return {};
  return site.fail(advanced, std::format("{} must be below {}",
                                         threshold_label("yellow", a.yellow, default_yellow),
                                         threshold_label("red", a.red, default_red)));
}

CheckResult read_advanced(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  const auto advanced = body["advanced"];
  if (!advanced) return {};
  if (auto checked = check_not_null(site, body, "advanced", "advanced", empty_map); !checked) {
    return checked;
  }
  if (!advanced.IsMap()) return site.fail(advanced, "advanced must be a map");
  for (const auto& entry : advanced) {
    if (auto read = read_advanced_entry(site, entry, node); !read) return read;
  }
  return check_thresholds_ordered(site, advanced, node);
}

// ---- Per-op cost (spec §17 entry 4) ----

// The types whose `cost` is a map op -> capacity units (a SQL database's writes default to
// defaults::sql_write_cost; a stated cost replaces it). A worker_pool's `cost` is a
// different key with the same spelling (a duration per job, read_job_cost), so the
// reader is chosen by type, not by the key alone.
bool has_cost_map(model::NodeType type) {
  return type == model::NodeType::service || type == model::NodeType::cache ||
         type == model::NodeType::sql_db;
}

// The largest cost one request may have. Chosen, not from the book: the same bound as a
// step's fan-out factor (max_step_factor), for the same reason. A request a million times
// dearer than the cheapest is no realistic op mix (it is a typo), and rate x cost stays far
// from overflowing a double (1e15/s x 1e6 = 1e21).
inline constexpr double max_cost_units = max_step_factor;

// One `op: units` entry. Positive (a free op would be served without using capacity, and
// a negative one would give capacity back), finite (an op that never finishes is a
// capacity of zero, which the capacity key already says) and at most max_cost_units.
std::expected<std::pair<std::string, double>, ConfigError> read_cost_entry(
    const Site& site, const YamlElement& entry) {
  auto op = read_op_name(site, entry.first, "cost op");
  if (!op) return std::unexpected(op.error());
  const auto label = std::format("cost '{}'", *op);
  if (auto checked = check_entry_not_null(site, entry, label); !checked) {
    return std::unexpected(checked.error());
  }
  const auto units = number_value(entry.second);
  if (!units || !is_positive(*units)) {
    return site.fail(entry.second, std::format("{} must be a positive finite number", label));
  }
  if (*units > max_cost_units) {
    return site.fail(entry.second, std::format("{} must be at most 1e6", label));
  }
  return std::pair{std::move(*op), *units};
}

// `cost: {write: 3, read: 1}`: the capacity units one request of each op consumes; ops
// not listed cost 1 (model::NodeSpec::cost).
CheckResult read_cost(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  const auto cost = body["cost"];
  if (!cost || !has_cost_map(node.type)) return {};
  if (auto checked = check_not_null(site, body, "cost", "cost", empty_map); !checked) {
    return checked;
  }
  if (!cost.IsMap()) return site.fail(cost, "cost must be a map of op to units");
  auto entries =
      collect_each(cost, [&](const YamlElement& entry) { return read_cost_entry(site, entry); });
  if (!entries) return std::unexpected(entries.error());
  node.cost = {std::make_move_iterator(entries->begin()), std::make_move_iterator(entries->end())};
  return {};
}

// ---- Load balancer keys (spec §17 entry 5) ----

// `policy: least_connections`; absent means round_robin (the schema's default). An absent
// policy stays absent, so explain can say "(default)": every reader writes
// `policy.value_or(round_robin)`, as it must for scenarios built in code anyway.
CheckResult read_lb_policy(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  if (!body["policy"]) return {};
  auto word = scalar_value(site, body, "policy");
  if (!word) return std::unexpected(word.error());
  const auto policy = model::parse_lb_policy(*word);
  if (!policy) {
    return site.fail(body["policy"],
                     "policy must be round_robin, least_connections, weighted or consistent_hash");
  }
  node.policy = *policy;
  return {};
}

// `vnodes: 150`: virtual nodes per server on a hash ring (a consistent_hash balancer's, or a
// kv_cluster's own), a positive integer. Absent stays absent: every reader applies
// model::default_vnodes.
CheckResult read_ring_vnodes(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  const auto value = body["vnodes"];
  if (!value) return {};
  if (auto checked = check_not_null(site, body, "vnodes", "vnodes"); !checked) return checked;
  return read_positive_int(site, value, "vnodes").transform(store_in(node.vnodes));
}

// A balancer's ring is its consistent_hash policy's, so `vnodes` means something only to that
// policy; on another it is a leftover from an edit, not a no-op to accept.
CheckResult read_vnodes(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  if (auto read = read_ring_vnodes(site, body, node); !read) return read;
  if (node.vnodes && node.policy != model::LbPolicy::consistent_hash) {
    return site.fail(body["vnodes"], "vnodes applies only to consistent_hash");
  }
  return {};
}

// The policy first: whether `vnodes` is allowed depends on it.
CheckResult read_balancer_keys(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  if (node.type != model::NodeType::load_balancer) return {};
  return read_lb_policy(site, body, node).and_then([&] { return read_vnodes(site, body, node); });
}

// The keys every node type shares. `ops`, `then` and the node references (backend, ...)
// are read in the second pass, once every id is known.
CheckResult read_common_keys(const Site& site, const YAML::Node& body, model::NodeSpec& node,
                             const Datacenters& datacenters) {
  if (auto read = check_allowed_keys(site, body, node); !read) return read;
  if (auto read = read_dc(site, body, node, datacenters); !read) return read;
  if (auto read = read_replicas(site, body, node); !read) return read;
  if (auto read = read_capacity(site, body, node); !read) return read;
  if (auto read = read_optional_duration(site, body, "latency", node.latency); !read) return read;
  return read_advanced(site, body, node);
}

// `site` already names the node; the two shape messages name it themselves ("node 'a' must
// be a map"), as they are about the node as a whole.
std::expected<model::NodeType, ConfigError> read_node_type(const Site& site, const YAML::Node& id,
                                                           const YAML::Node& body) {
  if (body.IsNull()) {
    return std::unexpected(
        error_at(site.file, id, std::format("node '{}' must not be empty", id.Scalar())));
  }
  if (!body.IsMap()) {
    return std::unexpected(
        error_at(site.file, body, std::format("node '{}' must be a map", id.Scalar())));
  }
  if (!body["type"]) {
    return std::unexpected(
        error_at(site.file, id, std::format("node '{}' needs a type", id.Scalar())));
  }
  auto type_name = scalar_value(site, body, "type");
  if (!type_name) return std::unexpected(type_name.error());
  if (const auto type = model::parse_node_type(*type_name)) return *type;
  return site.fail(body["type"], std::format("unknown node type '{}'", *type_name));
}

// Repeated ids were rejected by check_keys_and_expansion, before any node was read.
std::expected<model::NodeSpec, ConfigError> read_node_declaration(const Site& site,
                                                                  const YamlElement& entry,
                                                                  const Datacenters& datacenters) {
  const auto& id = entry.first;
  const auto name = key_name(site, id, "node id");
  if (!name) return std::unexpected(name.error());
  if (name->empty()) return site.fail(id, "node id must not be empty");
  const auto this_node = node_site(site, *name);
  const auto type = read_node_type(this_node, id, entry.second);
  if (!type) return std::unexpected(type.error());
  // Members the common keys leave unset keep the schema defaults declared in simsys.model;
  // ops, then and the client workload are read in the second pass (read_node_bodies), the
  // other types' own keys in a later step.
  // C++20 designated initializers: each member named where it is set, so two members of the
  // same type cannot be swapped unnoticed as with positional aggregate initialization.
  model::NodeSpec node{.id = *name, .type = *type};
  if (auto read = read_common_keys(this_node, entry.second, node, datacenters); !read) {
    return std::unexpected(read.error());
  }
  if (auto read = read_cost(this_node, entry.second, node); !read) {
    return std::unexpected(read.error());
  }
  if (auto read = read_balancer_keys(this_node, entry.second, node); !read) {
    return std::unexpected(read.error());
  }
  return node;
}

NodeIndex index_by_id(const std::vector<model::NodeSpec>& nodes) {
  NodeIndex ids;
  for (std::size_t i = 0; i < nodes.size(); ++i) ids.emplace(nodes[i].id, i);
  return ids;
}

}  // namespace

// yaml-cpp iterates a map in document order, so the declarations keep that order.
std::expected<Declarations, ConfigError> collect_node_declarations(const Site& site,
                                                                   const YAML::Node& nodes,
                                                                   const Datacenters& datacenters) {
  if (!nodes.IsMap() || nodes.size() == 0) return site.fail(nodes, "nodes must be a non-empty map");
  auto declared = collect_each(nodes, [&](const YamlElement& entry) {
    return read_node_declaration(site, entry, datacenters);
  });
  if (!declared) return std::unexpected(declared.error());
  auto ids = index_by_id(*declared);
  return Declarations{.nodes = std::move(*declared), .ids = std::move(ids)};
}

// ---- Rate limiter keys (spec §17 entry 6) -------------------------------------------
//
// Read in the second pass, where every reader above is declared and every node's type is
// known (`backend` must name a cache). Order matters: the default burst is limit x window.

namespace {

// `algorithm: sliding_log`; absent means token_bucket (the book's first and most common
// algorithm, ch. 4), filled in here so every later stage reads an algorithm.
CheckResult read_limiter_algorithm(const Site& site, const YAML::Node& body,
                                   model::NodeSpec& node) {
  node.algorithm = model::LimiterAlgorithm::token_bucket;
  return read_optional_word(site, body, "algorithm", model::parse_limiter_algorithm,
                            "algorithm must be token_bucket, leaky_bucket, fixed_window, "
                            "sliding_log or sliding_counter",
                            *node.algorithm);
}

// `limit: 1k/s`, required: it is the one number that makes a limiter limit. Positive: a
// limit of 0/s is a wall, which a missing link says more plainly.
CheckResult read_limiter_limit(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  if (!body["limit"]) return site.fail(body, "rate_limiter needs a limit");
  auto limit = read_bounded_rate(site, body, "limit", "limit");
  if (!limit) return std::unexpected(limit.error());
  if (!is_positive(limit->per_second)) return site.fail(body["limit"], "limit must be positive");
  node.limit = *limit;
  return {};
}

// `window: 500ms`, default model::default_limiter_window (1 s), applied by
// model::limiter_window (an absent window stays absent, so explain can say "(default)").
// Zero would make a window hold nothing and the default burst zero.
CheckResult read_limiter_window(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  if (!body["window"]) return {};
  auto window = parse_duration_key(site, body, "window");
  if (!window) return std::unexpected(window.error());
  if (window->count() <= 0) return site.fail(body["window"], "window must be positive");
  node.window = *window;
  return {};
}

// `burst: 500`, a whole number of requests, for the two buckets only: a windowed algorithm
// counts limit x window per window and has no bucket for a burst to size, so a burst on one
// is an error (it would otherwise be silently ignored). Default for a bucket: limit x window
// rounded up, one window's worth (one second's, by default), the conventional sizing of a
// token bucket for want of a better number; the windowed algorithms get none. The default
// is applied by model::limiter_burst, not stored (explain then says "(default)"), but it is
// checked here: it must fit the int a stated burst is.
CheckResult read_limiter_burst(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  const bool windowed = model::is_windowed(*node.algorithm);
  if (body["burst"]) {
    if (windowed) {
      return site.fail(body["burst"], "burst applies only to token_bucket and leaky_bucket");
    }
    if (auto checked = check_not_null(site, body, "burst", "burst"); !checked) return checked;
    return read_positive_int(site, body["burst"], "burst").transform(store_in(node.burst));
  }
  if (windowed) return {};
  const double per_window = model::limiter_burst(node);  // limit x window, rounded up
  if (per_window > std::numeric_limits<int>::max()) {
    return site.fail(body, "limit x window is too large for a burst; give burst explicitly");
  }
  return {};
}

// A leaky bucket's queue IS the bucket: one slot leaking a request every 1/limit, with room
// for `burst` (graph::leaky_bucket_capacity). A `capacity`, `latency`, `advanced.concurrency`,
// `advanced.queue_limit` or `advanced.service_variability` would be a second statement of the
// limit, the burst or the regular leak (variability 0) that the bucket would have to override,
// so each is refused. Read in the first pass with the keys every node shares; checked here,
// where the algorithm is known.
CheckResult check_leaky_bucket_keys(const Site& site, const YAML::Node& body,
                                    const model::NodeSpec& node) {
  if (node.algorithm != model::LimiterAlgorithm::leaky_bucket) return {};
  const bool restated = node.capacity || node.latency || node.advanced.queue_limit ||
                        node.advanced.concurrency || node.advanced.service_variability;
  if (!restated) return {};
  return site.fail(body,
                   "leaky_bucket takes limit and burst; capacity, latency, concurrency, "
                   "queue_limit and service_variability are not valid");
}

// `backend: counters`: the counter store every checked request consults (ch. 4 keeps the
// counters in Redis). read_node_routing has already checked that the node exists; here it
// must also be a cache, the only type that models an in-memory key-value store in 2a.
CheckResult read_limiter_backend(const Site& site, const YAML::Node& body, model::NodeSpec& node,
                                 const Declarations& declared) {
  if (!body["backend"]) return {};
  const auto id = scalar_text(body["backend"]);
  const auto& target = declared.nodes[declared.ids.find(id)->second];
  if (target.type != model::NodeType::cache) {
    return site.fail(body["backend"], "backend must be a cache");
  }
  node.backend = id;
  return {};
}

CheckResult read_limiter_keys(const Site& site, const YAML::Node& body, model::NodeSpec& node,
                              const Declarations& declared) {
  if (node.type != model::NodeType::rate_limiter) return {};
  const auto this_node = node_site(site, node.id);
  return read_limiter_algorithm(this_node, body, node)
      .and_then([&] { return read_limiter_limit(this_node, body, node); })
      .and_then([&] { return read_limiter_window(this_node, body, node); })
      .and_then([&] { return read_limiter_burst(this_node, body, node); })
      .and_then([&] { return check_leaky_bucket_keys(this_node, body, node); })
      .and_then([&] { return read_limiter_backend(this_node, body, node, declared); });
}

// ---- Id generator keys (spec §18 entry 12; book ch. 7) ------------------------------

// `algorithm: snowflake`, required: the algorithm decides the generator's capacity and service
// time (defaults::for_id_algorithm), so a generator without one would run on a made-up number.
// One message for a missing and an unknown word, naming the three, so the fix is in the error.
// The same key on a rate limiter is the limiter's (read_limiter_algorithm), typed apart.
// A ticket server is one auto-increment database (book ch. 7): `replicas` other than 1 is
// refused here, at its line, with Graph::build's message (model::ticket_server_replicas_rule),
// so `validate` catches it too; `replicas` was read in the first pass.
CheckResult read_id_generator_keys(const Site& site, const YAML::Node& body,
                                   model::NodeSpec& node) {
  if (node.type != model::NodeType::id_generator) return {};
  const auto this_node = node_site(site, node.id);
  constexpr std::string_view requirement =
      "id_generator needs algorithm: snowflake, ticket_server or uuid";
  if (!body["algorithm"]) return this_node.fail(body, requirement);
  model::IdAlgorithm algorithm{};
  if (auto read = read_optional_word(this_node, body, "algorithm", model::parse_id_algorithm,
                                     requirement, algorithm);
      !read) {
    return read;
  }
  node.id_algorithm = algorithm;
  if (algorithm == model::IdAlgorithm::ticket_server && node.replicas != 1) {
    return this_node.fail(body["replicas"], model::ticket_server_replicas_rule);
  }
  return {};
}

// ---- Cache keys (spec §17 entry 7) -------------------------------------------------
//
// Read in the second pass, after read_node_routing has checked that a `backend` names a
// declared node. Order matters: whether a backend is required depends on the strategy.

// `hit_ratio: {fixed: 0.8}` or `hit_ratio: che`, required: a cache without one says nothing
// about how much of the load it absorbs, which is the one thing a cache is in a design.
// 0 (every read misses) and 1 (every read hits) are both valid.
CheckResult read_hit_ratio(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  const auto value = body["hit_ratio"];
  if (!value) return site.fail(body, "cache needs a hit_ratio");
  if (value.IsScalar() && value.Scalar() == "che") {
    node.hit_ratio = model::HitRatio{.fixed = std::nullopt, .che = true};
    return {};
  }
  if (!value.IsMap() || value.size() != 1 || !value["fixed"]) {
    return site.fail(value, "hit_ratio must be che or {fixed: h}");
  }
  const auto fixed = number_value(value["fixed"]);
  if (!fixed || *fixed < 0.0 || *fixed > 1.0) {
    return site.fail(value["fixed"], "hit_ratio.fixed must be between 0 and 1");
  }
  node.hit_ratio = model::HitRatio{.fixed = *fixed};
  return {};
}

// `strategy: read_through`; absent keeps cache_aside (model::NodeSpec's default). Every
// strategy but cache_aside has the cache call the store itself, so it needs a `backend`;
// under cache_aside the application reads the store (a service's `on: miss` step), so a
// `backend` would never be called and is refused rather than silently ignored.
CheckResult read_cache_strategy(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  if (auto read = read_optional_word(site, body, "strategy", model::parse_cache_strategy,
                                     "strategy must be cache_aside, read_through, write_through, "
                                     "write_around or write_back",
                                     node.strategy);
      !read) {
    return read;
  }
  if (body["backend"]) node.backend = scalar_text(body["backend"]);
  const auto strategy = model::to_string(node.strategy);
  if (!model::needs_backend(node.strategy) && node.backend) {
    return site.fail(body["backend"], std::format("backend is unused with strategy {}", strategy));
  }
  if (!model::needs_backend(node.strategy) || node.backend) return {};
  const auto at = body["strategy"] ? body["strategy"] : body;
  return site.fail(at, std::format("strategy {} needs a backend", strategy));
}

// `write_ops: [write, delete]`: the ops that change data; every other op is a read. Stored
// exactly as written; absent stays absent (model::write_ops_of applies the type's default, a
// cache's or a CDN's `write` or a database's four SQL writes). An empty list would make every op
// a read, which a cache or a database that is written to never is, so it is refused rather than
// read as "no writes". On a CDN the list names the ops it refuses (it has no write path, so the
// graph refuses a write op that reaches it) instead of caching them as reads; on a blob store, the
// ops it acknowledges at their last byte rather than at their first megabyte.
CheckResult read_write_ops(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  const auto list = body["write_ops"];
  if (!list) return {};
  if (!list.IsSequence() && !list.IsNull()) {
    return site.fail(list, "write_ops must be a list of op names");
  }
  if (list.IsNull() || list.size() == 0) {
    return site.fail(list, "write_ops must name at least one op");
  }
  return collect_each(list,
                      [&](const YAML::Node& op) { return read_op_name(site, op, "write_ops op"); })
      .transform(store_in(node.write_ops));
}

// `keys: 20M`: how many entries the cache holds (the Che approximation's cache size, 2b).
CheckResult read_cache_size(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  const auto value = body["keys"];
  if (!value) return {};
  const auto count = parse_count(number_text(value));
  if (!count) return site.fail(value, "keys must be a positive integer");
  node.keys = *count;
  return {};
}

// The eviction policies a cache may name (ch. 1 lists LRU, LFU and FIFO; `ttl` evicts by age
// only). Phase 2b's Che approximation reads it; in 2a it is checked and printed.
constexpr std::string_view eviction_policies = "lru lfu fifo ttl";

CheckResult read_eviction(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  const auto value = body["eviction"];
  if (!value) return {};
  const auto word = scalar_text(value);
  if (word.empty() || !lists_word(eviction_policies, word)) {
    return site.fail(value, "eviction must be lru, lfu, fifo or ttl");
  }
  node.eviction = word;
  return {};
}

// `ttl: 5m`: how long an entry lives. Zero would expire every entry at once.
CheckResult read_cache_ttl(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  if (!body["ttl"]) return {};
  auto ttl = parse_duration_key(site, body, "ttl");
  if (!ttl) return std::unexpected(ttl.error());
  if (ttl->count() <= 0) return site.fail(body["ttl"], "ttl must be positive");
  node.ttl = *ttl;
  return {};
}

CheckResult read_cache_keys(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  if (node.type != model::NodeType::cache) return {};
  const auto this_node = node_site(site, node.id);
  return read_hit_ratio(this_node, body, node)
      .and_then([&] { return read_cache_strategy(this_node, body, node); })
      .and_then([&] { return read_write_ops(this_node, body, node); })
      .and_then([&] { return read_cache_size(this_node, body, node); })
      .and_then([&] { return read_eviction(this_node, body, node); })
      .and_then([&] { return read_cache_ttl(this_node, body, node); });
}

// ---- SQL database keys (spec §18 entry 6) ----

// `shards: 4`: how many independent primary + replicas sets the data is split over.
CheckResult read_shards(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  if (!body["shards"]) return {};
  if (auto checked = check_not_null(site, body, "shards", "shards"); !checked) return checked;
  return read_positive_int(site, body["shards"], "shards").transform(store_in(node.shards));
}

// `pool: 32`: the connection pool, i.e. how many queries one copy (the primary or a replica)
// runs at once. It sets the c of the copy's M/M/c queue AND caps its rate at pool / service time
// by Little's law (graph::apply_database_keys), which advanced.concurrency alone never does; it
// has the same bound (defaults::max_concurrency: Erlang C costs O(c) per step). Stating both is
// refused: two numbers for one c would contradict each other, and the message says to keep the
// pool, the one that also models the connections' limit on throughput.
CheckResult read_pool(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  const auto value = body["pool"];
  if (!value) return {};
  if (auto checked = check_not_null(site, body, "pool", "pool"); !checked) return checked;
  auto connections = read_positive_int(site, value, "pool");
  if (!connections) return std::unexpected(connections.error());
  if (*connections > defaults::max_concurrency) {
    return site.fail(value, std::format("pool must be at most {}", defaults::max_concurrency));
  }
  if (node.advanced.concurrency) {
    return site.fail(value,
                     "pool sets c and caps the rate by Little's law; drop advanced.concurrency");
  }
  node.pool = *connections;
  return {};
}

// `replication_lag: 50ms`: a constant, any non-negative duration (the parser refuses a negative
// one). Default 0: shipping takes no time; the write still waits in both queues (the primary's
// and a replica's, engine::NodeSample::staleness_s), so zero lag is not zero staleness.
CheckResult read_sql_keys(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  if (node.type != model::NodeType::sql_db) return {};
  const auto this_node = node_site(site, node.id);
  return read_shards(this_node, body, node)
      .and_then([&] { return read_pool(this_node, body, node); })
      .and_then([&] {
        return read_optional_duration(this_node, body, "replication_lag", node.replication_lag);
      })
      .and_then([&] { return read_write_ops(this_node, body, node); });
}

// ---- CDN keys (spec §18 entry 10) ----
//
// A CDN (ch. 1: a cache at the network's edge) is a read-only cache in front of its `origin`:
// it answers the hits and fetches the misses from the origin as the same op.

// `origin: media_store`, required: the node the CDN fetches its misses from. read_node_routing
// has already checked that it exists (the lookup below still guards against a miss rather than
// dereference an end iterator); it must also be a server, since a client only sends (the graph's
// words for any call to a client), and not the CDN itself, which could never answer its own miss.
CheckResult read_cdn_origin(const Site& site, const YAML::Node& body, model::NodeSpec& node,
                            const Declarations& declared) {
  const auto value = body["origin"];
  if (!value) return site.fail(body, "a cdn needs an origin");
  const auto id = scalar_text(value);
  const auto found = declared.ids.find(id);
  if (found == declared.ids.end()) {
    return site.fail(value, std::format("origin refers to unknown node '{}'", id));
  }
  if (declared.nodes[found->second].type == model::NodeType::client) {
    return site.fail(value, std::format("origin '{}' is a client; clients only send", id));
  }
  if (id == node.id) return site.fail(value, "a cdn cannot be its own origin");
  node.origin = id;
  return {};
}

// The hit ratio in the cache's own grammar, `{fixed: h}` or `che`; optional: a CDN without
// one misses every read (h = 0, graph::Graph::backend_fraction). `keys: 100k` is its size in
// objects, which a `che` hit ratio needs (read_cache_size). `ttl` is read as a cache's is
// (positive), printed by explain and not modelled. `write_ops` names the ops that are writes
// (read_write_ops): the graph refuses them at a CDN, and caches every other op as a read.
CheckResult read_cdn_keys(const Site& site, const YAML::Node& body, model::NodeSpec& node,
                          const Declarations& declared) {
  if (node.type != model::NodeType::cdn) return {};
  const auto this_node = node_site(site, node.id);
  return read_cdn_origin(this_node, body, node, declared)
      .and_then([&]() -> CheckResult {
        if (!body["hit_ratio"]) return {};
        return read_hit_ratio(this_node, body, node);
      })
      .and_then([&] { return read_cache_size(this_node, body, node); })
      .and_then([&] { return read_cache_ttl(this_node, body, node); })
      .and_then([&] { return read_write_ops(this_node, body, node); });
}

// ---- Blob store keys (spec §19 entry 2) ----
//
// `write_ops: [upload]`: the ops a blob store acknowledges at their last byte (a PUT is answered
// once the object is stored), and the only ones that fire its `then` steps, its event
// notifications; every other op is a read, answered at its first megabyte
// (behaviours::MegabyteService). Read as a cache's are (read_write_ops); default `write`.

// A blob store's `then` steps are event notifications nobody waits for, so a step `mode`
// (sequential latencies add, parallel ones overlap) means nothing there. The graph refuses
// `mode: parallel` (graph's invalid_blob_store), but `mode: sequential` is the model's default:
// only here, where the key is written, can it be told apart, so the key is refused whatever its
// value. read_then has already checked the list and its steps.
CheckResult check_event_steps_have_no_mode(const Site& site, const YAML::Node& body) {
  const auto then = body["then"];
  if (!then || !then.IsSequence()) return {};
  for (const auto& step : then) {
    if (!step.IsMap() || !step["mode"]) continue;
    return site.fail(step["mode"],
                     "a blob store's then steps are event notifications nobody waits for "
                     "(mode orders the calls a request waits for)");
  }
  return {};
}

CheckResult read_blob_store_keys(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  if (node.type != model::NodeType::blob_store) return {};
  const auto this_node = node_site(site, node.id);
  return read_write_ops(this_node, body, node).and_then([&] {
    return check_event_steps_have_no_mode(this_node, body);
  });
}

// ---- Message queue and worker pool keys (spec §19 entries 4-5) ----
//
// A message queue is a broker whose own numbers are the type's (an append); what it adds is
// `retention`, how long a message waits for its consumer groups before it is deleted unread, and
// `partitions`, printed and not modelled. A worker pool is M/M/c over consumption: `workers` is
// its c and `cost` the time one worker spends on one job, so its rate is workers / cost and its
// waiting room is its queue's retention. Every key that would state one of those a second time is
// refused with the reason (refused_worker_pool_keys).

// `retention: 7d`: positive (a retention of 0 would delete every message before any group read
// it). Absent stays absent: the graph applies defaults::default_retention, explain says "default".
CheckResult read_retention(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  if (!body["retention"]) return {};
  auto retention = parse_duration_key(site, body, "retention");
  if (!retention) return std::unexpected(retention.error());
  if (retention->count() <= 0) return site.fail(body["retention"], "retention must be positive");
  node.retention = *retention;
  return {};
}

// `partitions: 32`: a positive integer, printed by explain and not modelled (phase 3: a partition
// is read by one worker of a group at a time, which would cap a group's c).
CheckResult read_partitions(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  if (!body["partitions"]) return {};
  if (auto checked = check_not_null(site, body, "partitions", "partitions"); !checked) {
    return checked;
  }
  return read_positive_int(site, body["partitions"], "partitions")
      .transform(store_in(node.partitions));
}

CheckResult read_message_queue_keys(const Site& site, const YAML::Node& body,
                                    model::NodeSpec& node) {
  if (node.type != model::NodeType::message_queue) return {};
  const auto this_node = node_site(site, node.id);
  return read_retention(this_node, body, node).and_then([&] {
    return read_partitions(this_node, body, node);
  });
}

// ---- Search index keys (spec §19 entry 10) ----
//
// A search index (ch. 13's trie database) serves like a service, so its capacity, latency and
// replicas are read as a service's; `build_interval` is the one key it owns.

// `build_interval: 24h`: how often the index is rebuilt from the aggregated query logs, a positive
// duration (an interval of 0 would be a rebuild that never stops). Stored and printed, not
// modelled: the rebuild's load and the freshness of the answers are ignored. Absent stays absent.
CheckResult read_search_index_keys(const Site& site, const YAML::Node& body,
                                   model::NodeSpec& node) {
  if (node.type != model::NodeType::search_index || !body["build_interval"]) return {};
  const auto this_node = node_site(site, node.id);
  auto interval = parse_duration_key(this_node, body, "build_interval");
  if (!interval) return std::unexpected(interval.error());
  if (interval->count() <= 0) {
    return this_node.fail(body["build_interval"], "build_interval must be positive");
  }
  node.build_interval = *interval;
  return {};
}

// ---- Service discovery keys (phase 3 spec §8.1; book ch. 12) ----
//
// A service discovery (ch. 12's ZooKeeper, which hands the chat client a server) answers `lookup`
// like a service, so its capacity, latency and replicas are read as a service's; `failover` is the
// one key it owns.

// `failover: 10s`: how long the registry still hands out a replica that has died (it learns of the
// death when the replica's health check or session times out), a positive duration (0 would be a
// registry that knows of a death before it happens: nothing to model). Absent, it is
// defaults::failover, filled in here so that explain prints the window the engine uses.
CheckResult read_discovery_keys(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  if (node.type != model::NodeType::service_discovery) return {};
  node.failover = defaults::failover;
  if (!body["failover"]) return {};
  const auto this_node = node_site(site, node.id);
  auto failover = parse_duration_key(this_node, body, "failover");
  if (!failover) return std::unexpected(failover.error());
  if (failover->count() <= 0) return this_node.fail(body["failover"], "failover must be positive");
  node.failover = *failover;
  return {};
}

// `workers: 400`, required: the pool's c, a positive integer with the bound of every c
// (defaults::max_concurrency: Erlang C costs O(c) per step, as advanced.concurrency).
CheckResult read_workers(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  if (!body["workers"]) return site.fail(body, "worker_pool needs workers");
  if (auto checked = check_not_null(site, body, "workers", "workers"); !checked) return checked;
  auto workers = read_positive_int(site, body["workers"], "workers");
  if (!workers) return std::unexpected(workers.error());
  if (*workers > defaults::max_concurrency) {
    return site.fail(body["workers"],
                     std::format("workers must be at most {}", defaults::max_concurrency));
  }
  node.workers = *workers;
  return {};
}

// `cost: 60s`, required: the time one worker spends on one job, a positive duration. The same
// key on a service is a map of op to capacity units (read_cost): has_cost_map leaves a pool's
// alone, and this reads it as what it is here.
CheckResult read_job_cost(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  if (!body["cost"]) return site.fail(body, "worker_pool needs cost");
  auto cost = parse_duration_key(site, body, "cost");
  if (!cost) return std::unexpected(cost.error());
  if (cost->count() <= 0) return site.fail(body["cost"], "cost must be positive");
  node.job_cost = *cost;
  return {};
}

// The keys that would state a pool's numbers a second time. Its rate is workers / cost, its c
// `workers` and its service time `cost`, so `capacity`, `latency`, `replicas` (more workers are
// more `workers`) and `advanced.concurrency` could only contradict them; and its waiting room is
// the part of its queue it has not read, which the queue's `retention` bounds, so
// `advanced.queue_limit` is refused in favour of that.
CheckResult refused_worker_pool_keys(const Site& site, const YAML::Node& body) {
  for (const auto* key : {"capacity", "latency", "replicas"}) {
    if (body[key]) return site.fail(body[key], "a worker pool's rate is workers / cost");
  }
  const auto advanced = body["advanced"];
  if (!advanced || !advanced.IsMap()) return {};
  if (advanced["concurrency"]) {
    return site.fail(advanced["concurrency"], "a worker pool's rate is workers / cost");
  }
  if (advanced["queue_limit"]) {
    return site.fail(advanced["queue_limit"],
                     "a worker pool waits in its queue: set the queue's retention");
  }
  return {};
}

// `consumes: transcode_q`, required: the queue whose messages the pool drains, its only source of
// jobs. A pool without one would be a server any node could call, waiting days in a 7-day room and
// never dropping, which is no worker pool of the book's designs (graph::Graph::check_pool_senders
// refuses every other sender). The reference itself was checked with the other node references
// (reference_keys); what it may name (a queue or a frontier that links to the pool) is
// Graph::build's to check, which refuses a missing `consumes` in these words too, since a scenario
// built in code skips the loader.
CheckResult read_consumes(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  if (!body["consumes"]) return site.fail(body, "worker_pool needs consumes");
  node.consumes = scalar_text(body["consumes"]);
  return {};
}

// Does one of the pool's `then` steps call an external sink? What a pool retries is a call to a
// third party that failed (behaviours::WorkerPoolBehaviour); the steps' targets were checked to
// exist when the `then` list was read (read_node_routing runs first).
bool calls_external_sink(const model::NodeSpec& node, const Declarations& declared) {
  // C++20 std::ranges::any_of: the algorithm takes the whole range, replacing std::any_of's
  // begin()/end() pair.
  return std::ranges::any_of(node.then, [&](const model::Step& step) {
    const auto found = declared.ids.find(step.to);
    return found != declared.ids.end() &&
           declared.nodes[found->second].type == model::NodeType::external_sink;
  });
}

// `retries: 3` and `retry_after: 1s` (phase 3 spec §6.2), both optional, read by the client's rules
// and refused in the client's words (read_retry_count, read_retry_after). A pool retries the jobs
// whose call to an external sink failed, so `retries` on a pool with no such `then` step has
// nothing to retry and is refused rather than silently ignored. A pool that states `retries` and
// no `retry_after` waits defaults::pool_retry_after, filled in here so explain prints the wait the
// engine uses.
CheckResult read_pool_retries(const Site& site, const YAML::Node& body, model::NodeSpec& node,
                              const Declarations& declared) {
  if (auto read = read_retry_after(site, body, node.retry_after); !read) return read;
  if (!body["retries"]) return {};
  if (auto checked = check_not_null(site, body, "retries", "retries"); !checked) return checked;
  auto count = read_retry_count(site, body["retries"]);
  if (!count) return std::unexpected(count.error());
  if (!calls_external_sink(node, declared)) {
    return site.fail(body["retries"], "retries need a then step to an external sink");
  }
  node.retries = *count;
  if (!node.retry_after) node.retry_after = defaults::pool_retry_after;
  return {};
}

CheckResult read_worker_pool_keys(const Site& site, const YAML::Node& body, model::NodeSpec& node,
                                  const Declarations& declared) {
  if (node.type != model::NodeType::worker_pool) return {};
  const auto this_node = node_site(site, node.id);
  return read_consumes(this_node, body, node)
      .and_then([&] { return read_workers(this_node, body, node); })
      .and_then([&] { return read_job_cost(this_node, body, node); })
      .and_then([&] { return refused_worker_pool_keys(this_node, body); })
      .and_then([&] { return read_pool_retries(this_node, body, node, declared); });
}

// ---- External sink keys (spec §19 entry 8) ----
//
// An external sink is a third party's API (book ch. 10's APNs, FCM, SMS and e-mail providers): a
// queue of the type's numbers whose served requests fail a share `failure_rate`, and a
// `politeness` that caps its drain over the hosts of the scenario's frontier. Neither key names
// another node; `then` and `ops` stay refused by own_keys_by_type (a provider calls nobody of
// ours).

// `failure_rate: 0.02`: the share of what the sink serves that fails (a 5xx), in [0, 1]. Both
// bounds are meaningful: 0 is a provider that never fails, 1 one that is down. Absent stays
// model::NodeSpec's 0, the plain queue.
CheckResult read_failure_rate(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  if (!body["failure_rate"]) return {};
  const auto rate = number_value(body["failure_rate"]);
  if (!rate || !is_ratio(*rate)) {
    return site.fail(body["failure_rate"], "failure_rate must be between 0 and 1");
  }
  node.failure_rate = *rate;
  return {};
}

// `politeness: 1/s`: how often one host may be called, a positive rate (0/s would be a provider
// nobody may call, which a missing link says more plainly), on an external sink and on a URL
// frontier alike: the engine caps the node's drain at hosts x politeness (spec §8.4,
// behaviours::FrontierBehaviour). Absent stays absent.
CheckResult read_politeness(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  if (!body["politeness"]) return {};
  auto politeness = read_bounded_rate(site, body, "politeness", "politeness");
  if (!politeness) return std::unexpected(politeness.error());
  if (!is_positive(politeness->per_second)) {
    return site.fail(body["politeness"], "politeness must be positive");
  }
  node.politeness = *politeness;
  return {};
}

CheckResult read_external_sink_keys(const Site& site, const YAML::Node& body,
                                    model::NodeSpec& node) {
  if (node.type != model::NodeType::external_sink) return {};
  const auto this_node = node_site(site, node.id);
  return read_failure_rate(this_node, body, node).and_then([&] {
    return read_politeness(this_node, body, node);
  });
}

// ---- URL frontier keys (phase 3 Task 18, spec §8.4; book ch. 9) ----
//
// A URL frontier is the web crawler's queue of URLs to fetch: `hosts` distinct hosts, each fetched
// at most `politeness` times a second, so it hands out at most hosts x politeness URLs a second.
// `then` (the hand-off to the downloaders) is read with the other steps; `capacity` is the
// prioritiser's rate, a common key.

// `hosts: 10M`: a count, as a cache's `keys` (parse_count: a positive integer with an optional SI
// suffix), that fits the int the model keeps. Absent stays absent: nothing is capped.
CheckResult read_hosts(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  const auto value = body["hosts"];
  if (!value) return {};
  const auto count = parse_count(number_text(value));
  if (!count || *count > std::numeric_limits<int>::max()) {
    return site.fail(value, "hosts must be a positive integer");
  }
  node.hosts = static_cast<int>(*count);
  return {};
}

CheckResult read_frontier_keys(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  if (node.type != model::NodeType::frontier) return {};
  const auto this_node = node_site(site, node.id);
  return read_hosts(this_node, body, node).and_then([&] {
    return read_politeness(this_node, body, node);
  });
}

// ---- Key-value cluster keys (spec §19 entry 9; book ch. 6) ----
//
// A kv_cluster is book ch. 6's Dynamo-style store: `nodes` machines on a consistent-hash ring of
// `vnodes` virtual nodes each, every key on `replication` N of them, a write acknowledged by
// `write_quorum` W replicas and a read answered by `read_quorum` R; `mode` (ap or cp) is printed
// until partitions exist (phase 3). Each count is a positive integer, `nodes` is required, and the
// shapes that cannot exist are refused: more copies of a key than machines to hold them, a quorum
// larger than the copies it counts. W + R <= N is allowed: it is the eventually consistent end of
// ch. 6's dial (fast writes and reads, a read may miss the latest write).

// An optional count of a kv_cluster ("replication: 3"): a positive integer, or absent.
CheckResult read_kv_count(const Site& site, const YAML::Node& body, std::string_view key,
                          std::optional<int>& target) {
  const auto value = body[std::string{key}];
  if (!value) return {};
  if (auto checked = check_not_null(site, body, key, key); !checked) return checked;
  return read_positive_int(site, value, key).transform(store_in(target));
}

// "replication must be at most nodes (5 > 3)": `value`, of key `what`, above `limit`, the value
// of key `limit_name`. Pointed at `what`'s line, or at `fallback` when the scenario left `what`
// to its default (the default N = 3 on a two-node cluster is reported at `nodes`).
CheckResult check_at_most(const Site& site, const YAML::Node& body, std::string_view what,
                          int value, std::string_view limit_name, int limit,
                          std::string_view fallback) {
  if (value <= limit) return {};
  const auto at = body[std::string{what}] ? body[std::string{what}] : body[std::string{fallback}];
  return site.fail(at,
                   std::format("{} must be at most {} ({} > {})", what, limit_name, value, limit));
}

// `nodes: 8`, required, and the counts that default: vnodes, replication, the two quorums.
CheckResult read_kv_counts(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  if (!body["nodes"]) return site.fail(body, "kv_cluster needs nodes");
  if (auto read = read_kv_count(site, body, "nodes", node.kv_nodes); !read) return read;
  if (auto read = read_ring_vnodes(site, body, node); !read) return read;
  if (auto read = read_kv_count(site, body, "replication", node.replication); !read) return read;
  if (auto read = read_kv_count(site, body, "write_quorum", node.write_quorum); !read) return read;
  return read_kv_count(site, body, "read_quorum", node.read_quorum);
}

// N copies need N machines; a quorum counts replies from those N (model::kv_replication and the
// quorum helpers apply the defaults, so a default is checked like a stated value).
CheckResult check_kv_shape(const Site& site, const YAML::Node& body, const model::NodeSpec& node) {
  const int replication = model::kv_replication(node);
  if (auto checked =
          check_at_most(site, body, "replication", replication, "nodes", *node.kv_nodes, "nodes");
      !checked) {
    return checked;
  }
  if (auto checked = check_at_most(site, body, "write_quorum", model::kv_write_quorum(node),
                                   "replication", replication, "replication");
      !checked) {
    return checked;
  }
  return check_at_most(site, body, "read_quorum", model::kv_read_quorum(node), "replication",
                       replication, "replication");
}

// `replicas` is how a service scales behind a balancer; a kv_cluster's machines are its `nodes`
// and the copies of a key its `replication`, so `replicas` could mean either and is refused rather
// than guessed at (as a worker pool's is).
CheckResult refused_kv_replicas(const Site& site, const YAML::Node& body) {
  if (!body["replicas"]) return {};
  return site.fail(body["replicas"],
                   "a kv_cluster's machines are nodes; replicas is not a key here");
}

// `mode: cp`, absent left absent (model::kv_mode_of applies ap), so explain can tell a stated
// mode from the default.
CheckResult read_kv_mode(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  if (!body["mode"]) return {};
  model::KvMode mode{};
  return read_optional_word(site, body, "mode", model::parse_kv_mode, "mode must be ap or cp", mode)
      .transform([&] { node.kv_mode = mode; });
}

CheckResult read_kv_cluster_keys(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  if (node.type != model::NodeType::kv_cluster) return {};
  const auto this_node = node_site(site, node.id);
  return refused_kv_replicas(this_node, body)
      .and_then([&] { return read_kv_counts(this_node, body, node); })
      .and_then([&] { return check_kv_shape(this_node, body, node); })
      .and_then([&] { return read_kv_mode(this_node, body, node); })
      .and_then([&] { return read_write_ops(this_node, body, node); });
}

// ---- Websocket gateway keys (phase 3 spec §8.2; book ch. 12) ----
//
// A ws_gateway holds its open connections as a level (behaviours::WsGatewayBehaviour): the counts
// that bound or fix it (`max_connections`, `connections`), the durations that drain and load it
// (`session`, `heartbeat`) or bring it back after a fall (`reconnect_within`), and the fan-out of
// its messages (`fanout`). Each is optional and an absent one stays absent (model::NodeSpec has
// the defaults). Whether a `connect` op reaches the gateway is a fact of the graph, not of the
// file: Simulation::create, not the loader, refuses a gateway with neither level or with both.

// A count of connections ("max_connections: 100k", "connections: 2M"): a positive integer with the
// k/M/G grammar of every count (parse_count, as a cache's `keys`).
CheckResult read_connection_count(const Site& site, const YAML::Node& body, std::string_view key,
                                  std::optional<std::int64_t>& target) {
  const auto value = body[std::string{key}];
  if (!value) return {};
  const auto count = parse_count(number_text(value));
  if (!count) return site.fail(value, std::string{key} + " must be a positive integer");
  target = *count;
  return {};
}

// A positive duration ("heartbeat: 5s", "session: 30m", "reconnect_within: 30s"): a zero heartbeat
// would be an infinite load, a zero session an instant disconnect, a zero reconnect_within an
// instant storm. In the words of `ttl` and `build_interval`.
CheckResult read_gateway_duration(const Site& site, const YAML::Node& body, std::string_view key,
                                  std::optional<SimTime>& target) {
  if (!body[std::string{key}]) return {};
  auto duration = parse_duration_key(site, body, key);
  if (!duration) return std::unexpected(duration.error());
  if (duration->count() <= 0) {
    return site.fail(body[std::string{key}], std::string{key} + " must be positive");
  }
  target = *duration;
  return {};
}

// `fanout: 1.4`, the average recipients of a message (ch. 12: 1:1 chats and small groups): at
// least 1. Below 1 a message would reach fewer than one recipient, a lost message, not a fan-out;
// exactly 1 (google-drive's long poll) is a message to its one device.
CheckResult read_fanout(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  if (!body["fanout"]) return {};
  const auto fanout = number_value(body["fanout"]);
  if (!fanout || !(*fanout >= 1.0)) return site.fail(body["fanout"], "fanout must be at least 1");
  // A step's own factor's bound (max_step_factor): the fan-out multiplies it.
  if (*fanout > max_step_factor) return site.fail(body["fanout"], "fanout must be at most 1e6");
  node.fanout = *fanout;
  return {};
}

// `deliver` is a message pushed to its recipient over the open connection (model::deliver_op): it
// ends at the gateway, so its step list is empty. Steps under it would be routed as a service's (a
// review found `deliver: [{to: d, op: deliverx}]` sending d 50/s), a hop the model gives no
// meaning, so they are refused here, with what to write instead. read_node_routing read the ops
// before this runs.
CheckResult check_deliver_terminal(const Site& site, const YAML::Node& body,
                                   const model::NodeSpec& node) {
  const std::string name{model::deliver_op};
  const auto deliver = node.ops.find(name);
  if (deliver == node.ops.end() || deliver->second.empty()) return {};
  return site.fail(body["ops"][name],
                   "op 'deliver' is terminal on a ws_gateway (write deliver: [])");
}

CheckResult read_gateway_keys(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  if (node.type != model::NodeType::ws_gateway) return {};
  const auto this_node = node_site(site, node.id);
  return read_connection_count(this_node, body, "max_connections", node.max_connections)
      .and_then(
          [&] { return read_connection_count(this_node, body, "connections", node.connections); })
      .and_then([&] { return read_gateway_duration(this_node, body, "heartbeat", node.heartbeat); })
      .and_then([&] { return read_gateway_duration(this_node, body, "session", node.session); })
      .and_then([&] {
        return read_gateway_duration(this_node, body, "reconnect_within", node.reconnect_within);
      })
      .and_then([&] { return read_fanout(this_node, body, node); })
      .and_then([&] { return check_deliver_terminal(this_node, body, node); });
}

// ---- Notification fanout keys (phase 3 spec §8.3; book ch. 10) ----
//
// A notification fanout is a service whose `then` steps fire for each routed notification that its
// dedup step lets through (behaviours::NotificationFanoutBehaviour). `dedup` is the share it drops
// as duplicates; `channels` names the message queue behind each channel. Which queues the `then`
// steps publish to must agree with `channels`, in both directions: that is Graph::build's check,
// since a scenario built in code skips the loader.

// `dedup: 0.02`, in [0, 1): 0 drops no duplicate (absent means the same), and 1 would drop every
// notification, a fanout that publishes nothing, which a missing link says more plainly, so the
// interval is half-open. The message gives the bounds as people say them.
CheckResult read_dedup(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  if (!body["dedup"]) return {};
  const auto share = number_value(body["dedup"]);
  if (!share || *share < 0.0 || *share >= 1.0) {
    return site.fail(body["dedup"], "dedup must be between 0 and 1");
  }
  node.dedup = *share;
  return {};
}

// One `channels` entry, `sms: sms_q`: a channel name and the declared message queue behind it. A
// value that names no node, or a node of another type, is refused with the type it has (a channel
// is a queue the channel's workers drain).
std::expected<std::pair<std::string, std::string>, ConfigError> read_channel(
    const Site& site, const YamlElement& entry, const Declarations& declared) {
  auto name = key_name(site, entry.first, "channel name");
  if (!name) return std::unexpected(name.error());
  if (name->empty()) return site.fail(entry.first, "a channel name must not be empty");
  const auto channel = std::format("channel '{}'", *name);
  if (auto checked = check_entry_not_null(site, entry, channel); !checked) {
    return std::unexpected(checked.error());
  }
  if (!entry.second.IsScalar()) {
    return site.fail(entry.second, std::format("{} must name a message queue", channel));
  }
  const auto& queue = entry.second.Scalar();
  const auto found = declared.ids.find(queue);
  if (found == declared.ids.end()) {
    return site.fail(entry.second,
                     std::format("{} names '{}', which is not declared", channel, queue));
  }
  const auto type = declared.nodes[found->second].type;
  if (type != model::NodeType::message_queue) {
    return site.fail(entry.second,
                     std::format("{} names '{}', which is {} (channels name message queues)",
                                 channel, queue, type_with_article(type)));
  }
  return std::pair{*name, queue};
}

// `channels: {ios: ios_q, sms: sms_q}`: a map, possibly empty (`{}`) or absent (a fanout whose
// steps publish to no queue; Graph::build refuses the file when one does).
CheckResult read_channels(const Site& site, const YAML::Node& body, model::NodeSpec& node,
                          const Declarations& declared) {
  if (!body["channels"]) return {};
  if (auto checked = check_not_null(site, body, "channels", "channels", empty_map); !checked) {
    return checked;
  }
  const auto channels = body["channels"];
  if (!channels.IsMap()) {
    return site.fail(channels, "channels must be a map of channel names to message queues");
  }
  auto read = collect_each(
      channels, [&](const YamlElement& entry) { return read_channel(site, entry, declared); });
  if (!read) return std::unexpected(read.error());
  // One queue per channel: two channels on one queue would be one channel under two names, and
  // which `then` step carries whose share could no longer be told.
  std::map<std::string, std::string> channel_of_queue;
  for (const auto& [name, queue] : *read) {
    const auto [first, added] = channel_of_queue.try_emplace(queue, name);
    if (!added) {
      return site.fail(channels[name], std::format("channel '{}' names '{}', which channel '{}' "
                                                   "names too",
                                                   name, queue, first->second));
    }
  }
  node.channels.insert(read->begin(), read->end());
  return {};
}

CheckResult read_notification_fanout_keys(const Site& site, const YAML::Node& body,
                                          model::NodeSpec& node, const Declarations& declared) {
  if (node.type != model::NodeType::notification_fanout) return {};
  const auto this_node = node_site(site, node.id);
  return read_dedup(this_node, body, node).and_then([&] {
    return read_channels(this_node, body, node, declared);
  });
}

// ---- Node bodies: the second pass -----------------------------------------------------

// A client offers traffic (its `ops` are shares of its rate); every other node forwards
// what it receives (its `ops` are step lists, on the types that accept the key).
CheckResult read_traffic(const StepScope& scope, const YAML::Node& body, model::NodeSpec& node) {
  if (node.type == model::NodeType::client) {
    return read_workload(scope.site, body).transform(store_in(node.workload));
  }
  return read_op_steps(scope, body, node);
}

CheckResult check_reference_key(const Site& site, const YAML::Node& body, std::string_view key,
                                const NodeIndex& ids) {
  if (!body[std::string{key}]) return {};
  return check_reference(site, body, key, key, ids);
}

// Everything in a node body that names other nodes or describes its traffic. The body is
// known to be a map: the declaration pass checked it.
CheckResult read_node_routing(const Site& site, const YAML::Node& body, model::NodeSpec& node,
                              const NodeIndex& ids) {
  const auto this_node = node_site(site, node.id);
  const StepScope scope{.site = this_node, .node_id = node.id, .ids = ids};
  if (auto read = read_traffic(scope, body, node); !read) return read;
  if (auto read = read_then(scope, body, node); !read) return read;
  for (const auto key : reference_keys) {
    if (auto checked = check_reference_key(this_node, body, key, ids); !checked) return checked;
  }
  return {};
}

}  // namespace

// Pattern: Two-pass resolution (declare, then resolve), as an assembler resolves labels —
// the first pass (collect_node_declarations) records every node id, this second pass reads
// the bodies' references, so a node may refer to one declared further down the file. The
// declarations are in document order, the same order yaml-cpp iterates the map, so the two
// walk side by side.
CheckResult read_node_bodies(const Site& site, const YAML::Node& nodes, Declarations& declared) {
  auto node = declared.nodes.begin();
  for (const auto& entry : nodes) {
    if (auto read = read_node_routing(site, entry.second, *node, declared.ids); !read) {
      return read;
    }
    if (auto read = read_limiter_keys(site, entry.second, *node, declared); !read) return read;
    if (auto read = read_cache_keys(site, entry.second, *node); !read) return read;
    if (auto read = read_sql_keys(site, entry.second, *node); !read) return read;
    if (auto read = read_cdn_keys(site, entry.second, *node, declared); !read) return read;
    if (auto read = read_blob_store_keys(site, entry.second, *node); !read) return read;
    if (auto read = read_id_generator_keys(site, entry.second, *node); !read) return read;
    if (auto read = read_message_queue_keys(site, entry.second, *node); !read) return read;
    if (auto read = read_worker_pool_keys(site, entry.second, *node, declared); !read) {
      return read;
    }
    if (auto read = read_external_sink_keys(site, entry.second, *node); !read) return read;
    if (auto read = read_frontier_keys(site, entry.second, *node); !read) return read;
    if (auto read = read_kv_cluster_keys(site, entry.second, *node); !read) return read;
    if (auto read = read_search_index_keys(site, entry.second, *node); !read) return read;
    if (auto read = read_gateway_keys(site, entry.second, *node); !read) return read;
    if (auto read = read_discovery_keys(site, entry.second, *node); !read) return read;
    if (auto read = read_notification_fanout_keys(site, entry.second, *node, declared); !read) {
      return read;
    }
    ++node;
  }
  return {};
}

}  // namespace simsys::config::detail
