// Module `simsys.config`, implementation unit: the checks behind `load`. yaml-cpp and
// <format> are included here, not in config.cppm (see the GCC 14.2 note there).
module;
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// C++20 module implementation unit: `module simsys.config;` (no `export`) makes this file
// part of the named module. It sees everything the interface declared, exports nothing,
// and can include heavy headers in its own global module fragment, like a .cpp behind a
// header.
module simsys.config;

import simsys.core;
// Only for fluid::Capacity's default zone thresholds: one source of truth for 0.7 / 0.9.
import simsys.fluid;
import simsys.model;

namespace simsys::config::detail {

// C++23 std::expected<void, E>: "either fine or this error". It replaces a bool plus an
// out-parameter for the error; an empty `{}` means the check passed.
using CheckResult = std::expected<void, ConfigError>;

// The declared node ids, each with its index in Declarations::nodes, so a reader that
// holds an id can reach the node (an SLO needs its target's type and ops). A map, not a
// scan of the declarations: every reference is looked up, and a linear scan per lookup
// made validation quadratic (60k nodes took 147 s). std::less<> (C++14 transparent
// comparator) lets it be searched with a std::string_view directly, without building a
// temporary std::string per lookup.
using NodeIndex = std::map<std::string, std::size_t, std::less<>>;

// What the first pass over `nodes` produces: the declarations in document order (for the
// Scenario) and the id -> index map (for the reference checks).
struct Declarations {
  std::vector<model::NodeSpec> nodes;
  NodeIndex ids;
};

using Datacenters = std::vector<std::string>;

// What one element of a YAML map or list is when iterated: a Node (list element) that is
// also a std::pair<Node, Node> (map entry, `.first` the key and `.second` the value).
using YamlElement = YAML::const_iterator::value_type;

constexpr std::array<std::string_view, 5> required_top_level_keys{"version", "name", "duration",
                                                                  "nodes", "links"};
// Key lists are space-separated words: one line per list reads like the schema's tables.
constexpr std::string_view top_level_keys =
    "version name seed duration step warmup datacenters nodes links slo faults";
// Node attributes whose value is the id of another node.
constexpr std::array<std::string_view, 3> reference_keys{"consumes", "backend", "origin"};

// Keys every node type accepts (docs/scenario-schema.md, "Node (common keys)").
constexpr std::string_view common_node_keys = "type dc replicas";
// Keys of a node that serves requests, i.e. every type but `client`: a client is a traffic
// source, with no service rate, service time or waiting room to describe (simsys.defaults
// refuses to resolve a capacity for one).
constexpr std::string_view server_node_keys = "capacity latency advanced";

// The keys a node type adds to the common ones.
struct OwnKeys {
  model::NodeType type;
  std::string_view keys;
};

// Pattern: Table-driven method — which keys a node type accepts is data, one row per type
// copied from the "Node types" table of docs/scenario-schema.md, not a switch of if-chains.
// Supporting a new key is one word in one row. The rows follow NodeType order so a type
// finds its row by index; the static_assert below proves that order at compile time.
// `ops` (what a node does with each incoming op) belongs to client, service and ws_gateway;
// `then` (steps after processing) to the five types the schema lists for it.
constexpr std::array<OwnKeys, model::node_type_names.size()> own_keys_by_type{{
    {model::NodeType::client, "rate ops keys sizes timeout retries"},
    {model::NodeType::cdn, "ttl hit_ratio origin"},
    {model::NodeType::load_balancer, "policy vnodes"},
    {model::NodeType::rate_limiter, "algorithm limit burst backend"},
    {model::NodeType::service, "ops"},
    {model::NodeType::cache, "eviction strategy keys hit_ratio backend ttl then"},
    {model::NodeType::sql_db, "replication_lag shards pool"},
    {model::NodeType::kv_cluster, "nodes vnodes replication write_quorum read_quorum mode"},
    {model::NodeType::message_queue, "partitions retention"},
    {model::NodeType::worker_pool, "consumes workers cost then"},
    {model::NodeType::blob_store, "then"},
    {model::NodeType::search_index, "build_interval"},
    {model::NodeType::ws_gateway, "max_connections heartbeat fanout ops"},
    {model::NodeType::service_discovery, "failover"},
    {model::NodeType::id_generator, "algorithm"},
    {model::NodeType::external_sink, "failure_rate politeness"},
    {model::NodeType::notification_fanout, "channels dedup then"},
    {model::NodeType::frontier, "politeness hosts then"},
}};

// C++23 std::to_underlying: an enum's integer value, always of exactly its underlying
// type. Replaces static_cast<std::size_t>(e), which compiles for any target type and so
// can silently narrow; here it turns a NodeType into the row index it must match.
constexpr bool own_keys_follow_node_type_order() {
  for (std::size_t i = 0; i < own_keys_by_type.size(); ++i) {
    if (std::to_underlying(own_keys_by_type[i].type) != i) return false;
  }
  return true;
}
static_assert(own_keys_follow_node_type_order(), "own_keys_by_type rows must follow NodeType");

// The engine's zone thresholds when a scenario gives none, read from fluid::Capacity's
// default member initializers (~70% / ~90% utilization: the operations rule of thumb for
// where the M/M/1 wait, growing like 1/(1-rho), climbs steeply and then turns dangerous).
// Read, not restated: the loader must order a lone threshold against the value the engine
// will really use, and a second copy of the numbers could drift from it.
constexpr fluid::Capacity engine_capacity_defaults{};
constexpr double default_yellow = engine_capacity_defaults.yellow;
constexpr double default_red = engine_capacity_defaults.red;

// Range rules for the numeric keys. Non-finite values (inf, nan) fail all of them: an
// infinite variability or threshold has no meaning in the queueing formulas.
bool is_positive(double v) {
  return std::isfinite(v) && v > 0;
}
bool is_non_negative(double v) {
  return std::isfinite(v) && v >= 0;
}
// A count held in a double (the model stores queue_limit as one): positive and whole.
bool is_positive_whole(double v) {
  return is_positive(v) && std::trunc(v) == v;
}
// A utilization fraction: above 0 (a zero threshold would colour an idle node) and at most
// 1 (utilization cannot exceed 100%).
bool is_unit_fraction(double v) {
  return v > 0 && v <= 1;
}
// A ratio bound (drop rate, utilization): a fraction of all requests or of all time, so
// 0 (none allowed) up to 1 (everything); a bound above 1 could never be broken.
bool is_ratio(double v) {
  return v >= 0 && v <= 1;
}

// One numeric `advanced` key: its range rule, the message completing "advanced.<key> must
// be ...", and the Advanced member that receives it. `std::optional<double>
// model::Advanced::*` is a pointer to member (pre-C++11): it names a field of any Advanced,
// and `advanced.*target` selects that field of one particular object.
struct AdvancedNumber {
  std::string_view key;
  bool (*in_range)(double);
  std::string_view requirement;
  std::optional<double> model::Advanced::*target;
};

// Pattern: Table-driven method — each numeric advanced key is one row, so the reading and
// range-checking code is written once. `concurrency` is an int in the model and is read
// separately; `queue_limit` is an int in the schema but a double in the model, so it is
// read as a number and must be whole.
constexpr std::array<AdvancedNumber, 5> advanced_numbers{{
    {"queue_limit", is_positive_whole, "a positive integer", &model::Advanced::queue_limit},
    {"service_variability", is_non_negative, "non-negative", &model::Advanced::service_variability},
    {"burstiness", is_non_negative, "non-negative", &model::Advanced::burstiness},
    {"yellow", is_unit_fraction, "in (0, 1]", &model::Advanced::yellow},
    {"red", is_unit_fraction, "in (0, 1]", &model::Advanced::red},
}};

// yaml-cpp counts lines and columns from 0; editors and compilers count from 1.
ConfigError error_at_mark(const std::string& file, const YAML::Mark& mark, std::string message) {
  // C++20 designated initializers: each member is named at the point of construction,
  // so swapping `line` and `column` (both int) cannot go unnoticed as it could with
  // positional aggregate initialization.
  return ConfigError{.file = file,
                     .line = mark.line + 1,
                     .column = mark.column + 1,
                     .message = std::move(message)};
}

ConfigError error_at(const std::string& file, const YAML::Node& node, std::string message) {
  return error_at_mark(file, node.Mark(), std::move(message));
}

// Pattern: Parameter Object — the context every reader needs travels as one value: the
// file, and the prefix naming the enclosing construct ("node 'a'", "link a -> b", "slo"),
// so every message says where to look. The document's own site has no prefix.
struct Site {
  const std::string& file;
  std::string where;

  [[nodiscard]] ConfigError error(const YAML::Node& at, std::string_view what) const {
    // C++20 std::format: type-checked (at compile time) formatting that returns a
    // std::string, replacing std::ostringstream chains and snprintf buffers.
    if (where.empty()) return error_at(file, at, std::string{what});
    return error_at(file, at, std::format("{}: {}", where, what));
  }

  // The error as the failed outcome of any reader: std::unexpected<ConfigError> converts
  // to every std::expected<T, ConfigError>.
  [[nodiscard]] std::unexpected<ConfigError> fail(const YAML::Node& at,
                                                  std::string_view what) const {
    return std::unexpected(error(at, what));
  }

  // A site one construct deeper, in the same file.
  [[nodiscard]] Site within(std::string inner) const {
    return Site{.file = file, .where = std::move(inner)};
  }
};

bool is_declared(const NodeIndex& ids, std::string_view id) {
  // C++20 contains(): "is this key in the map?" in one call, replacing the
  // `find(key) != end()` comparison.
  return ids.contains(id);
}

// True when `word` is one of the space-separated words of `words`.
bool lists_word(std::string_view words, std::string_view word) {
  // C++20 ranges algorithm: std::ranges::any_of takes the range itself instead of a
  // begin/end iterator pair, so it composes with views. C++20 std::views::split: a lazy
  // view of the pieces between separators, replacing a hand-written find/substr loop; each
  // piece is a contiguous range of chars, so the C++20 iterator-pair constructor of
  // std::string_view can view it without copying.
  return std::ranges::any_of(words | std::views::split(' '), [word](auto piece) {
    return std::string_view{piece.begin(), piece.end()} == word;
  });
}

// A callable that stores its argument in `target`. Passed to std::expected::transform it
// turns "read a value" into "read it into this field": the result is expected<void, E>.
// C++20 abbreviated function template: an `auto&` parameter makes this a template without
// the `template <typename T>` line; it serves both plain fields and std::optional ones.
auto store_in(auto& target) {
  return [&target](auto value) { target = std::move(value); };
}

// IsDefined() first: asking a missing key's node for its type throws.
bool is_sequence(const YAML::Node& node) {
  return node.IsDefined() && node.IsSequence();
}

// The text of a scalar, "" for a list, a map or null; used where the caller's own message
// ("must be always, miss or failure", ...) covers those shapes too.
std::string scalar_text(const YAML::Node& node) {
  return node.IsScalar() ? node.Scalar() : std::string{};
}

// The text of a number, "" for anything that is no number. One rule for every numeric key:
// a number is a plain (unquoted, untagged) scalar, which yaml-cpp tags "?". A quoted "4" is
// a string in YAML ("!"), so `replicas: "4"` and `version: "1"` are rejected, as a typed
// language would reject the string "4" where an int is expected.
std::string number_text(const YAML::Node& node) {
  return node.IsScalar() && node.Tag() == "?" ? node.Scalar() : std::string{};
}

// The whole text as a T, or nothing: from_chars never throws and reports how far it got;
// the whole text must be consumed, so "0.5x" is no number and "4.0" is no int. For an int
// it rejects "abc", "+1" and "0x1" and accepts "-1" (callers check the sign).
template <typename T>
std::optional<T> parse_whole(std::string_view text) {
  T value{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
  return value;
}

// The key node of `parent[key]`, for messages about the key itself. yaml-cpp's lookup
// returns only the value.
YAML::Node key_node(const YAML::Node& parent, std::string_view key) {
  for (const auto& entry : parent) {
    if (entry.first.IsScalar() && entry.first.Scalar() == key) return entry.first;
  }
  return parent;
}

// A key written with nothing after it (`capacity:`) holds null. yaml-cpp places a null
// value where the next token starts, usually the next line, so the error points at the key.
// `label` names the key in the message ("capacity", "rate.base").
CheckResult check_not_null(const Site& site, const YAML::Node& parent, std::string_view key,
                           std::string_view label) {
  if (!parent[std::string{key}].IsNull()) return {};
  return site.fail(key_node(parent, key), std::format("{} must not be empty", label));
}

// The text of `parent[key]`, which the caller has checked is present. yaml-cpp's Scalar()
// silently returns "" for a list, a map or null, so those shapes are rejected here, for
// every key.
std::expected<std::string, ConfigError> scalar_value(const Site& site, const YAML::Node& parent,
                                                     std::string_view key, std::string_view label) {
  if (auto checked = check_not_null(site, parent, key, label); !checked) {
    return std::unexpected(checked.error());
  }
  const auto value = parent[std::string{key}];
  if (!value.IsScalar()) return site.fail(value, std::format("{} must be a scalar", label));
  return value.Scalar();
}

std::expected<std::string, ConfigError> scalar_value(const Site& site, const YAML::Node& parent,
                                                     std::string_view key) {
  return scalar_value(site, parent, key, key);
}

// A unit parser's error, attributed to the offending value and prefixed with its key.
ConfigError unit_error(const Site& site, const YAML::Node& value, std::string_view key,
                       const ParseError& error) {
  return site.error(value, std::format("{}: {}", key, error.message));
}

std::expected<SimTime, ConfigError> parse_duration_key(const Site& site, const YAML::Node& parent,
                                                       std::string_view key) {
  const auto value = parent[std::string{key}];
  // C++23 std::expected::and_then: runs the next fallible step only if the previous one
  // produced a value, passing an error through untouched; it chains steps without an
  // `if (!x) return std::unexpected(x.error());` between them.
  // C++23 std::expected::transform_error: converts the error (ParseError -> ConfigError)
  // and passes a value through untouched.
  return scalar_value(site, parent, key).and_then([&](const std::string& text) {
    return parse_duration(text).transform_error(
        [&](const ParseError& error) { return unit_error(site, value, key, error); });
  });
}

// For durations a scenario may omit (step, warmup, latency): absent keeps `target`'s
// default, present must parse.
CheckResult read_optional_duration(const Site& site, const YAML::Node& parent, std::string_view key,
                                   auto& target) {
  if (!parent[std::string{key}]) return {};
  // C++23 std::expected::transform: applies store_in's lambda to the value, if there is
  // one; the lambda returns nothing, so the result is std::expected<void, ConfigError>.
  return parse_duration_key(site, parent, key).transform(store_in(target));
}

// Pattern: Higher-order function — "apply a fallible reader to each element of a YAML list
// or map, stop at the first error, collect the values" is written once; the reader, the
// part that varies, is a parameter.
// C++20 concepts: `std::invocable<const YamlElement&> Reader` constrains the template
// parameter to callables taking one element. It replaces an unconstrained `typename`
// (or C++11 enable_if tricks): a reader of the wrong shape is rejected at the call with
// "constraints not satisfied" instead of an error deep inside the body.
template <std::invocable<const YamlElement&> Reader>
auto collect_each(const YAML::Node& container, Reader read)
    -> std::expected<
        std::vector<typename std::invoke_result_t<Reader&, const YamlElement&>::value_type>,
        ConfigError> {
  std::vector<typename std::invoke_result_t<Reader&, const YamlElement&>::value_type> collected;
  for (const auto& element : container) {
    auto value = read(element);
    if (!value) return std::unexpected(value.error());
    collected.push_back(std::move(*value));
  }
  return collected;
}

// An op name: a non-empty scalar, wherever one is written (a client's share, a service's
// op, a step's op, a link's op filter, an SLO's op). `label` names it in the message.
std::expected<std::string, ConfigError> read_op_name(const Site& site, const YAML::Node& name,
                                                     std::string_view label) {
  if (name.IsScalar() && !name.Scalar().empty()) return name.Scalar();
  if (name.IsMap() || name.IsSequence()) {
    return site.fail(name, std::format("{} must be a scalar", label));
  }
  return site.fail(name, std::format("{} must not be empty", label));
}

// ---- Repeated keys --------------------------------------------------------------------
//
// yaml-cpp keeps both entries of a key written twice in one map: a lookup by name sees the
// first, an iteration sees both. Readers of both kinds exist, so a repeated key would mean
// different things to different checks; it is rejected before any of them runs.

// `noun` names the key in the message: "key", or "node id" for the `nodes` map.
CheckResult check_map_keys_unique(const Site& site, const YAML::Node& map, std::string_view noun) {
  // string_views into the document's own key text, which outlives this check.
  std::set<std::string_view> seen;
  for (const auto& entry : map) {
    if (!entry.first.IsScalar()) continue;  // a list or map as a key: not a name to repeat
    if (seen.insert(entry.first.Scalar()).second) continue;
    return site.fail(entry.first, std::format("duplicate {} '{}'", noun, entry.first.Scalar()));
  }
  return {};
}

// Every map of the document, at any depth: a depth-first walk through maps and lists.
CheckResult check_unique_keys(const Site& site, const YAML::Node& node) {
  if (node.IsMap()) {
    if (auto checked = check_map_keys_unique(site, node, "key"); !checked) return checked;
  }
  if (!node.IsMap() && !node.IsSequence()) return {};
  for (const auto& element : node) {
    // A map entry's child is its value; a list element is itself the child (a YamlElement
    // is a Node too, hence the cast to pick that view of it).
    const auto& child = node.IsMap() ? element.second : static_cast<const YAML::Node&>(element);
    if (auto checked = check_unique_keys(site, child); !checked) return checked;
  }
  return {};
}

// The top-level keys, then the node ids (with the more specific message), then the rest.
CheckResult check_no_repeated_keys(const Site& site, const YAML::Node& root) {
  if (!root.IsMap()) return {};
  if (auto checked = check_map_keys_unique(site, root, "key"); !checked) return checked;
  if (const auto nodes = root["nodes"]; nodes.IsMap()) {
    if (auto checked = check_map_keys_unique(site, nodes, "node id"); !checked) return checked;
  }
  return check_unique_keys(site, root);
}

// ---- Top level ------------------------------------------------------------------------

CheckResult check_required_top_level_keys(const Site& site, const YAML::Node& root) {
  for (const auto key : required_top_level_keys) {
    if (!root[std::string{key}]) {
      return site.fail(root, std::format("missing top-level key '{}'", key));
    }
  }
  return {};
}

// A misspelt optional key ("wramup") would otherwise be ignored and its default used.
CheckResult check_known_top_level_keys(const Site& site, const YAML::Node& root) {
  for (const auto& entry : root) {
    const auto& key = entry.first;
    if (lists_word(top_level_keys, key.Scalar())) continue;
    return site.fail(key, std::format("unknown top-level key '{}'", key.Scalar()));
  }
  return {};
}

// An empty document is null, not a map: it falls through to the required-keys check, whose
// "missing top-level key" message is the more useful one. Any other non-map root (a bare
// scalar, a list) would make the key lookups below throw yaml-cpp's own error.
CheckResult check_root_is_map(const Site& site, const YAML::Node& root) {
  if (root.IsDefined() && !root.IsNull() && !root.IsMap()) {
    return site.fail(root, "scenario must be a map");
  }
  return {};
}

// `faults` is typed in phase 3; its shape is checked now, so a scalar is not silently kept.
CheckResult check_faults_is_list(const Site& site, const YAML::Node& root) {
  const auto faults = root["faults"];
  if (!faults || faults.IsSequence()) return {};
  return site.fail(faults, "faults must be a list");
}

CheckResult check_top_level(const Site& site, const YAML::Node& root) {
  if (auto checked = check_root_is_map(site, root); !checked) return checked;
  if (auto checked = check_no_repeated_keys(site, root); !checked) return checked;
  if (auto checked = check_required_top_level_keys(site, root); !checked) return checked;
  if (auto checked = check_known_top_level_keys(site, root); !checked) return checked;
  return check_faults_is_list(site, root);
}

// ---- Header ---------------------------------------------------------------------------

// Read like every other integer key (a plain scalar through parse_whole), so "+1", "0x1",
// "1.0" and a quoted "1" are no version.
CheckResult read_version(const Site& site, const YAML::Node& root, int& version) {
  if (auto checked = check_not_null(site, root, "version", "version"); !checked) return checked;
  const auto value = root["version"];
  const auto number = parse_whole<int>(number_text(value));
  if (!number) return site.fail(value, "version must be a whole number");
  if (*number != 1) return site.fail(value, "version must be 1");
  version = *number;
  return {};
}

CheckResult read_name(const Site& site, const YAML::Node& root, std::string& name) {
  auto text = scalar_value(site, root, "name");
  if (!text) return std::unexpected(text.error());
  if (text->empty()) return site.fail(root["name"], "name must not be empty");
  name = std::move(*text);
  return {};
}

CheckResult read_seed(const Site& site, const YAML::Node& root, int& seed) {
  const auto value = root["seed"];
  if (!value) return {};
  if (auto checked = check_not_null(site, root, "seed", "seed"); !checked) return checked;
  const auto number = parse_whole<int>(number_text(value));
  if (!number) return site.fail(value, "seed must be an integer");
  seed = *number;
  return {};
}

// The simulation's time step (`step:`), not a routing step. A zero step would never
// advance the clock; parse_duration already rejects negatives.
CheckResult read_time_step(const Site& site, const YAML::Node& root, SimTime& step) {
  if (auto read = read_optional_duration(site, root, "step", step); !read) return read;
  if (step.count() > 0) return {};
  return site.fail(root["step"], "step must be positive");
}

// The scalar header fields; an absent optional one keeps the Scenario's default.
CheckResult read_header(const Site& site, const YAML::Node& root, model::Scenario& scenario) {
  if (auto read = read_version(site, root, scenario.version); !read) return read;
  if (auto read = read_name(site, root, scenario.name); !read) return read;
  if (auto read = read_seed(site, root, scenario.seed); !read) return read;
  auto duration = parse_duration_key(site, root, "duration");
  if (!duration) return std::unexpected(duration.error());
  scenario.duration = *duration;
  if (auto read = read_time_step(site, root, scenario.step); !read) return read;
  return read_optional_duration(site, root, "warmup", scenario.warmup);
}

// ---- Datacenters ----------------------------------------------------------------------

// One datacenter: a non-empty name whose value is its settings map (`{}` today).
CheckResult read_datacenter(const Site& site, const YamlElement& entry, Datacenters& names) {
  const auto& name = entry.first.Scalar();
  if (name.empty()) return site.fail(entry.first, "name must not be empty");
  if (!entry.second.IsMap())
    return site.fail(entry.second, std::format("'{}' must be a map", name));
  names.push_back(name);
  return {};
}

// `latency` is not a datacenter: the schema reserves it for the list of cross-DC
// round-trip overrides, which a later step reads; only its shape is checked now.
CheckResult read_datacenter_entry(const Site& site, const YamlElement& entry, Datacenters& names) {
  if (entry.first.Scalar() != "latency") return read_datacenter(site, entry, names);
  if (entry.second.IsSequence()) return {};
  return site.fail(entry.second, "latency must be a list");
}

// `datacenters` maps each name to its settings; only the names, in file order, matter yet.
std::expected<Datacenters, ConfigError> read_datacenters(const Site& site, const YAML::Node& root) {
  const auto map = root["datacenters"];
  if (!map) return Datacenters{"main"};
  if (!map.IsMap()) return site.fail(map, "datacenters must be a map");
  const auto entry_site = site.within("datacenters");
  Datacenters names;
  for (const auto& entry : map) {
    if (auto read = read_datacenter_entry(entry_site, entry, names); !read) {
      return std::unexpected(read.error());
    }
  }
  if (names.empty()) return site.fail(map, "datacenters must not be empty");
  return names;
}

// ---- Node declarations: the first pass ------------------------------------------------

bool accepts_key(model::NodeType type, std::string_view key) {
  if (lists_word(common_node_keys, key)) return true;
  if (type != model::NodeType::client && lists_word(server_node_keys, key)) return true;
  return lists_word(own_keys_by_type[std::to_underlying(type)].keys, key);
}

// A misspelt key ("capactiy") would otherwise be ignored and the type default used.
CheckResult check_allowed_keys(const Site& site, const YAML::Node& body,
                               const model::NodeSpec& node) {
  for (const auto& entry : body) {
    const auto key = entry.first.Scalar();
    if (accepts_key(node.type, key)) continue;
    return site.fail(entry.first, std::format("key '{}' is not valid for type '{}'", key,
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

// Capacity counts requests for most node types and bytes for storage ("500MB/s"). The
// "B/s" suffix decides which parser reads it and which field receives it, so a text that
// neither parser accepts gets the message of the parser its suffix selects.
std::expected<void, ParseError> read_capacity_text(std::string_view text, model::NodeSpec& node) {
  // C++20 std::string_view::ends_with: a suffix test without size arithmetic.
  if (text.ends_with("B/s")) return parse_byte_rate(text).transform(store_in(node.byte_capacity));
  return parse_rate(text).transform(store_in(node.capacity));
}

CheckResult read_capacity(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  const auto capacity = body["capacity"];
  if (!capacity) return {};
  return scalar_value(site, body, "capacity").and_then([&](const std::string& text) {
    return read_capacity_text(text, node).transform_error([&](const ParseError& error) {
      return unit_error(site, capacity, "capacity", error);
    });
  });
}

const AdvancedNumber* find_advanced_number(std::string_view key) {
  // A ranges algorithm with a projection: `&AdvancedNumber::key` makes find compare each
  // row's key with `key`, replacing a hand-written lambda comparing one member.
  const auto row = std::ranges::find(advanced_numbers, key, &AdvancedNumber::key);
  return row == advanced_numbers.end() ? nullptr : &*row;
}

std::expected<double, ConfigError> read_advanced_number(const Site& site, const YAML::Node& value,
                                                        const AdvancedNumber& rule) {
  const auto number = parse_whole<double>(number_text(value));
  if (number && rule.in_range(*number)) return *number;
  return site.fail(value, std::format("advanced.{} must be {}", rule.key, rule.requirement));
}

CheckResult read_advanced_entry(const Site& site, const YamlElement& entry, model::NodeSpec& node) {
  const auto name = entry.first.Scalar();
  if (name == "concurrency") {
    return read_positive_int(site, entry.second, "advanced.concurrency")
        .transform(store_in(node.advanced.concurrency));
  }
  const auto* rule = find_advanced_number(name);
  if (!rule) return site.fail(entry.first, std::format("unknown advanced key '{}'", name));
  return read_advanced_number(site, entry.second, *rule)
      .transform(store_in(node.advanced.*(rule->target)));
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
  if (!advanced.IsMap()) return site.fail(advanced, "advanced must be a map");
  for (const auto& entry : advanced) {
    if (auto read = read_advanced_entry(site, entry, node); !read) return read;
  }
  return check_thresholds_ordered(site, advanced, node);
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

Site node_site(const Site& site, std::string_view id) {
  return site.within(std::format("node '{}'", id));
}

// Repeated ids were rejected by check_no_repeated_keys, before any node was read.
std::expected<model::NodeSpec, ConfigError> read_node_declaration(const Site& site,
                                                                  const YamlElement& entry,
                                                                  const Datacenters& datacenters) {
  const auto& id = entry.first;
  if (id.Scalar().empty()) return site.fail(id, "node id must not be empty");
  const auto this_node = node_site(site, id.Scalar());
  const auto type = read_node_type(this_node, id, entry.second);
  if (!type) return std::unexpected(type.error());
  // Members the common keys leave unset keep the schema defaults declared in simsys.model;
  // ops, then and the client workload are read in the second pass (read_node_bodies), the
  // other types' own keys in a later step.
  model::NodeSpec node{.id = id.Scalar(), .type = *type};
  if (auto read = read_common_keys(this_node, entry.second, node, datacenters); !read) {
    return std::unexpected(read.error());
  }
  return node;
}

NodeIndex index_by_id(const std::vector<model::NodeSpec>& nodes) {
  NodeIndex ids;
  for (std::size_t i = 0; i < nodes.size(); ++i) ids.emplace(nodes[i].id, i);
  return ids;
}

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

// `parent[key]` (present) must name a declared node. `what` names the referring construct
// in the message: "step", "link", "backend", ...
CheckResult check_reference(const Site& site, const YAML::Node& parent, std::string_view key,
                            std::string_view what, const NodeIndex& ids) {
  const auto reference = parent[std::string{key}];
  return scalar_value(site, parent, key).and_then([&](const std::string& id) -> CheckResult {
    if (is_declared(ids, id)) return {};
    return site.fail(reference, std::format("{} refers to unknown node '{}'", what, id));
  });
}

// A misspelt key would otherwise be ignored. `kind` names the key in the message:
// "unknown step key 'x'", "unknown key 'x'".
CheckResult check_known_keys(const Site& site, const YAML::Node& map, std::string_view allowed,
                             std::string_view kind) {
  for (const auto& entry : map) {
    const auto key = entry.first.Scalar();
    if (lists_word(allowed, key)) continue;
    return site.fail(entry.first, std::format("unknown {} '{}'", kind, key));
  }
  return {};
}

// One word of a word-valued key (`on: miss`, `dir: inout`) and the enumerator it means.
template <typename E>
struct NamedValue {
  std::string_view name;
  E value;
};

// Pattern: Table-driven method — each enum the schema spells as a word is a table of
// {word, enumerator}; one lookup in each direction serves them all.
constexpr std::array<NamedValue<model::StepCondition>, 3> step_conditions{{
    {"always", model::StepCondition::always},
    {"miss", model::StepCondition::miss},
    {"failure", model::StepCondition::failure},
}};
constexpr std::array<NamedValue<model::StepMode>, 2> step_modes{{
    {"sequential", model::StepMode::sequential},
    {"parallel", model::StepMode::parallel},
}};
constexpr std::array<NamedValue<model::Direction>, 3> directions{{
    {"in", model::Direction::in},
    {"out", model::Direction::out},
    {"inout", model::Direction::inout},
}};
constexpr std::array<NamedValue<model::SloMetric>, 7> slo_metrics{{
    {"p50_latency", model::SloMetric::p50_latency},
    {"p95_latency", model::SloMetric::p95_latency},
    {"p99_latency", model::SloMetric::p99_latency},
    {"drop_rate", model::SloMetric::drop_rate},
    {"utilization", model::SloMetric::utilization},
    {"queue_lag", model::SloMetric::queue_lag},
    {"staleness", model::SloMetric::staleness},
}};

template <typename E, std::size_t N>
std::optional<E> find_named(const std::array<NamedValue<E>, N>& table, std::string_view name) {
  const auto row = std::ranges::find(table, name, &NamedValue<E>::name);
  if (row == table.end()) return std::nullopt;
  return row->value;
}

// The inverse lookup; every enumerator has a row, so the result is always a table word.
template <typename E, std::size_t N>
std::string_view name_of(const std::array<NamedValue<E>, N>& table, E value) {
  return std::ranges::find(table, value, &NamedValue<E>::value)->name;
}

// An optional word-valued key: absent keeps `target`'s default; present must be a word of
// `table`, otherwise `requirement` ("dir must be in, out or inout") is the message.
template <typename E, std::size_t N>
CheckResult read_optional_word(const Site& site, const YAML::Node& parent, std::string_view key,
                               const std::array<NamedValue<E>, N>& table,
                               std::string_view requirement, E& target) {
  const auto value = parent[std::string{key}];
  if (!value) return {};
  if (auto checked = check_not_null(site, parent, key, key); !checked) return checked;
  const auto word = find_named(table, scalar_text(value));
  if (!word) return site.fail(value, requirement);
  target = *word;
  return {};
}

// An optional numeric key: absent keeps `target`'s default; present must be a number that
// passes `in_range`, otherwise `requirement` ("step factor must be positive") is the message.
CheckResult read_optional_number(const Site& site, const YAML::Node& parent, std::string_view key,
                                 bool (*in_range)(double), std::string_view requirement,
                                 double& target) {
  const auto value = parent[std::string{key}];
  if (!value) return {};
  if (auto checked = check_not_null(site, parent, key, key); !checked) return checked;
  const auto number = parse_whole<double>(number_text(value));
  if (!number || !in_range(*number)) return site.fail(value, requirement);
  target = *number;
  return {};
}

// ---- Steps: how a node forwards an op (docs/scenario-schema.md, "step") ----------------

constexpr std::string_view step_keys = "to op on mode factor";

// Pattern: Parameter Object — what every step of one node is read against: the node's
// site (for messages), its id (a step may not target it) and the declared ids.
struct StepScope {
  const Site& site;
  std::string_view node_id;
  const NodeIndex& ids;
};

CheckResult check_step_target(const StepScope& scope, const YAML::Node& step) {
  if (!step.IsMap()) return scope.site.fail(step, "step must be a map");
  if (!step["to"]) return scope.site.fail(step, "step needs a 'to'");
  return check_reference(scope.site, step, "to", "step", scope.ids);
}

// The op a step forwards: its own `op`, else the op that came in (a service's `read` goes on
// as `read`). A `then` step follows processing, not one named op, so it has nothing to
// inherit: `incoming_op` is empty and `op` is required.
std::expected<std::string, ConfigError> read_step_op(const Site& site, const YAML::Node& step,
                                                     std::optional<std::string_view> incoming_op) {
  if (step["op"]) return read_op_name(site, step["op"], "step op");
  if (incoming_op) return std::string{*incoming_op};
  return site.fail(step, "then step needs an 'op'");
}

// `on` gates the flow (a cache's `miss` steps see only what it did not serve, ch. 1),
// `mode` decides whether latencies add (sequential) or overlap (parallel: the max counts),
// `factor` multiplies the flow (fan-out on write: one post -> 100 feed writes, ch. 11).
CheckResult read_step_attributes(const Site& site, const YAML::Node& yaml, model::Step& step) {
  if (auto read = read_optional_word(site, yaml, "on", step_conditions,
                                     "step condition must be always, miss or failure", step.on);
      !read) {
    return read;
  }
  if (auto read = read_optional_word(site, yaml, "mode", step_modes,
                                     "step mode must be sequential or parallel", step.mode);
      !read) {
    return read;
  }
  return read_optional_number(site, yaml, "factor", is_positive, "step factor must be positive",
                              step.factor);
}

// A step back to its own node would re-inject its whole flow every time step: the fluid
// engine sees a feedback edge with factor >= 1, so the rate grows without bound. A retry,
// the one real reason to send to yourself, is `client.retries`. Checked last, so a step's
// other mistakes are reported first.
CheckResult check_not_own_node(const StepScope& scope, const YAML::Node& yaml,
                               const model::Step& step) {
  if (step.to != scope.node_id) return {};
  return scope.site.fail(yaml["to"], "a step may not target its own node");
}

std::expected<model::Step, ConfigError> read_step(const StepScope& scope, const YAML::Node& yaml,
                                                  std::optional<std::string_view> incoming_op) {
  if (auto checked = check_step_target(scope, yaml); !checked) {
    return std::unexpected(checked.error());
  }
  if (auto checked = check_known_keys(scope.site, yaml, step_keys, "step key"); !checked) {
    return std::unexpected(checked.error());
  }
  auto op = read_step_op(scope.site, yaml, incoming_op);
  if (!op) return std::unexpected(op.error());
  model::Step step{.to = yaml["to"].Scalar(), .op = std::move(*op)};
  if (auto read = read_step_attributes(scope.site, yaml, step); !read) {
    return std::unexpected(read.error());
  }
  return check_not_own_node(scope, yaml, step).transform([&] { return std::move(step); });
}

std::expected<std::vector<model::Step>, ConfigError> read_step_list(
    const StepScope& scope, const YAML::Node& list, std::optional<std::string_view> incoming_op) {
  return collect_each(list,
                      [&](const YAML::Node& yaml) { return read_step(scope, yaml, incoming_op); });
}

// One entry of a forwarding node's `ops`: an op name and the step list it triggers. An
// empty list (`deliver: []`) is a terminal op: the node handles it and forwards nothing.
CheckResult read_op_entry(const StepScope& scope, const YamlElement& entry, model::NodeSpec& node) {
  auto name = read_op_name(scope.site, entry.first, "op name");
  if (!name) return std::unexpected(name.error());
  if (!is_sequence(entry.second)) {
    return scope.site.fail(entry.second, std::format("op '{}' must be a step list", *name));
  }
  return read_step_list(scope, entry.second, *name).transform([&](std::vector<model::Step> steps) {
    node.ops.emplace(std::move(*name), std::move(steps));
  });
}

// A forwarding node's `ops`: incoming op name -> the steps it triggers.
CheckResult read_op_steps(const StepScope& scope, const YAML::Node& ops, model::NodeSpec& node) {
  if (!ops) return {};
  if (!ops.IsMap()) return scope.site.fail(ops, "ops must be a map");
  for (const auto& entry : ops) {
    if (auto read = read_op_entry(scope, entry, node); !read) return read;
  }
  return {};
}

// `then`: the steps after a node has processed its work (a worker's follow-up writes, a
// cache's misses going on to the database).
CheckResult read_then(const StepScope& scope, const YAML::Node& body, model::NodeSpec& node) {
  const auto then = body["then"];
  if (!then) return {};
  if (!then.IsSequence()) return scope.site.fail(then, "then must be a list");
  return read_step_list(scope, then, std::nullopt).transform(store_in(node.then));
}

// ---- Client workload: the traffic a client offers -------------------------------------
//
// Models the user population of the book's back-of-the-envelope estimates (ch. 2): a
// request rate (DAU x requests per user / 86 400 s) and the mix of operations those
// requests are (the read/write ratio, e.g. 10:1 for the URL shortener, ch. 8). Phase 1
// keeps only the constant `rate.base`; the curve terms (wave, spikes, ramp, points, noise)
// are accepted and ignored until phase 2 models time-varying load.

constexpr std::string_view rate_keys = "base wave spikes ramp points noise";

// The tolerance of "the shares sum to 1": decimal shares are not exact in binary floating
// point (0.1 + 0.2 != 0.3), so the sum is compared within 1e-6.
constexpr double share_sum_tolerance = 1e-6;

using OpShare = std::pair<std::string, double>;
using OpShares = std::vector<OpShare>;

std::expected<Rate, ConfigError> read_rate_base(const Site& site, const YAML::Node& body) {
  const auto rate = body["rate"];
  if (!rate || !rate.IsMap() || !rate["base"]) {
    return site.fail(rate ? rate : body, "client needs rate.base");
  }
  if (auto checked = check_known_keys(site, rate, rate_keys, "rate key"); !checked) {
    return std::unexpected(checked.error());
  }
  return scalar_value(site, rate, "base", "rate.base").and_then([&](const std::string& text) {
    return parse_rate(text).transform_error([&](const ParseError& error) {
      return unit_error(site, rate["base"], "rate.base", error);
    });
  });
}

// A share is the probability that a request is this op: above 0 (a zero share is an op
// that never happens; leave it out) and at most 1.
std::expected<OpShare, ConfigError> read_op_share(const Site& site, const YamlElement& entry) {
  auto name = read_op_name(site, entry.first, "op name");
  if (!name) return std::unexpected(name.error());
  const auto share = parse_whole<double>(number_text(entry.second));
  if (share && is_unit_fraction(*share)) return OpShare{std::move(*name), *share};
  return site.fail(entry.second, std::format("op share '{}' must be in (0, 1]", *name));
}

// `value` with 15 significant digits: every share sum outside the 1e-6 tolerance differs
// from 1 within the first 7, while the binary rounding noise of the addition (0.5 +
// 0.500002 is 1.0000019999999998) sits in the 16th and 17th and is not shown.
// std::to_chars, not std::format("{:.15g}"): with GCC 14.2, a precision in a format spec
// throws std::format_error at run time in a module unit that imports a module whose global
// module fragment includes <format>, as simsys.core's does (verified 2026-10-28 with a
// three-file probe). Plain "{}" is unaffected.
std::string with_significant_digits(double value) {
  std::array<char, 32> buffer{};  // 15 digits, sign, point and exponent fit with room to spare
  const auto written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                     std::chars_format::general, 15);
  return std::string{buffer.data(), written.ptr};
}

// Every request is exactly one op, so the shares are a probability distribution.
CheckResult check_shares_sum_to_one(const Site& site, const YAML::Node& ops,
                                    const OpShares& shares) {
  // C++23 std::ranges::fold_left: a left fold over a range, replacing std::accumulate's
  // iterator pair (and its trap of an int initial value truncating doubles: here 0.0).
  // C++20 std::views::values: a view of each pair's second member, without a copy.
  const double sum = std::ranges::fold_left(shares | std::views::values, 0.0, std::plus<>{});
  if (std::abs(sum - 1.0) <= share_sum_tolerance) return {};
  return site.fail(ops,
                   std::format("op shares sum to {}, expected 1", with_significant_digits(sum)));
}

// Sorted by op name, so the same mix listed in another order loads as the same Workload.
std::expected<OpShares, ConfigError> read_op_shares(const Site& site, const YAML::Node& body) {
  const auto ops = body["ops"];
  if (!ops) return site.fail(body, "client needs ops");
  if (!ops.IsMap()) return site.fail(ops, "ops must be a map");
  if (ops.size() == 0) return site.fail(ops, "client needs at least one op");
  auto shares =
      collect_each(ops, [&](const YamlElement& entry) { return read_op_share(site, entry); });
  if (!shares) return shares;
  std::ranges::sort(*shares);  // pairs compare by their first member (the op) first
  return check_shares_sum_to_one(site, ops, *shares).transform([&] { return std::move(*shares); });
}

std::expected<model::Workload, ConfigError> read_workload(const Site& site,
                                                          const YAML::Node& body) {
  model::Workload workload;
  if (auto read = read_rate_base(site, body).transform(store_in(workload.base)); !read) {
    return std::unexpected(read.error());
  }
  if (auto read = read_op_shares(site, body).transform(store_in(workload.op_shares)); !read) {
    return std::unexpected(read.error());
  }
  if (auto read = read_optional_duration(site, body, "timeout", workload.timeout); !read) {
    return std::unexpected(read.error());
  }
  return workload;
}

// ---- Node bodies: the second pass -----------------------------------------------------

// A client offers traffic (its `ops` are shares of its rate); every other node forwards
// what it receives (its `ops` are step lists, on the types that accept the key).
CheckResult read_traffic(const StepScope& scope, const YAML::Node& body, model::NodeSpec& node) {
  if (node.type == model::NodeType::client) {
    return read_workload(scope.site, body).transform(store_in(node.workload));
  }
  return read_op_steps(scope, body["ops"], node);
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

// Pattern: Two-pass resolution (declare, then resolve), as an assembler resolves labels —
// the first pass (collect_node_declarations) records every node id, this second pass reads
// the bodies' references, so a node may refer to one declared further down the file. The
// declarations are in document order, the same order yaml-cpp iterates the map, so the two
// walk side by side.
CheckResult read_node_bodies(const Site& site, const YAML::Node& nodes, Declarations& declared) {
  auto node = declared.nodes.begin();
  for (const auto& entry : nodes) {
    if (auto read = read_node_routing(site, entry.second, *node++, declared.ids); !read) {
      return read;
    }
  }
  return {};
}

// ---- Links ----------------------------------------------------------------------------

// `bandwidth` † is accepted and ignored until phase 2 models byte flows.
constexpr std::string_view link_keys = "from to dir ops latency bandwidth weight";

// A link is named by its ends before they are checked: "link a -> b" in every message.
Site link_site(const Site& site, std::string_view from, std::string_view to) {
  return site.within(std::format("link {} -> {}", from, to));
}

CheckResult check_link_end(const Site& site, const YAML::Node& link, std::string_view end,
                           const NodeIndex& ids) {
  if (!link[std::string{end}]) return site.fail(link, std::format("link needs '{}'", end));
  return check_reference(site, link, end, "link", ids);
}

// A link joins two nodes over a network hop; a node does not call itself over the network.
CheckResult check_link_ends_differ(const Site& site, const YAML::Node& link,
                                   const model::LinkSpec& spec) {
  if (spec.from != spec.to) return {};
  return site.fail(link, "from and to must differ");
}

std::expected<std::string, ConfigError> read_link_op(const Site& site, const YAML::Node& op) {
  if (!op.IsScalar()) return site.fail(op, "ops must be a list of op names");
  return read_op_name(site, op, "op name");
}

// The ops a link carries; absent means all of them. An empty list is rejected: it would
// load as "all", the opposite of what it says.
CheckResult read_link_ops(const Site& site, const YAML::Node& link, std::vector<std::string>& ops) {
  const auto list = link["ops"];
  if (!list) return {};
  if (!list.IsSequence()) return site.fail(list, "ops must be a list of op names");
  if (list.size() == 0) {
    return site.fail(list, "ops must name at least one op (omit it to carry all)");
  }
  return collect_each(list, [&](const YAML::Node& op) { return read_link_op(site, op); })
      .transform(store_in(ops));
}

// `weight` is the link's share when several links carry the same op (a weighted load
// balancer, ch. 1); 0 is allowed: a drained backend that receives nothing.
CheckResult read_link_attributes(const Site& site, const YAML::Node& link, model::LinkSpec& spec) {
  if (auto checked = check_known_keys(site, link, link_keys, "key"); !checked) return checked;
  if (auto read = read_optional_word(site, link, "dir", directions, "dir must be in, out or inout",
                                     spec.dir);
      !read) {
    return read;
  }
  if (auto read = read_link_ops(site, link, spec.ops); !read) return read;
  if (auto read = read_optional_duration(site, link, "latency", spec.latency); !read) return read;
  return read_optional_number(site, link, "weight", is_non_negative, "weight must be non-negative",
                              spec.weight);
}

std::expected<model::LinkSpec, ConfigError> read_link(const Site& site, const YAML::Node& link,
                                                      const NodeIndex& ids) {
  if (!link.IsMap()) return site.fail(link, "link must be a map");
  if (auto from = check_link_end(site, link, "from", ids); !from) {
    return std::unexpected(from.error());
  }
  if (auto to = check_link_end(site, link, "to", ids); !to) return std::unexpected(to.error());
  model::LinkSpec spec{.from = link["from"].Scalar(), .to = link["to"].Scalar()};
  const auto this_link = link_site(site, spec.from, spec.to);
  if (auto checked = check_link_ends_differ(this_link, link, spec); !checked) {
    return std::unexpected(checked.error());
  }
  if (auto read = read_link_attributes(this_link, link, spec); !read) {
    return std::unexpected(read.error());
  }
  return spec;
}

// The direction demand travels over a link: (sender, receiver). `dir` is written from the
// `from` side, so an `in` link carries demand from `to` to `from`; `out` and `inout` carry
// it from `from` to `to` (inout's replies flow back on the same link).
using DemandPair = std::pair<std::string, std::string>;

DemandPair demand_pair(const model::LinkSpec& link) {
  if (link.dir == model::Direction::in) return {link.to, link.from};
  return {link.from, link.to};
}

// One link per pair and demand direction: the graph routes an op over THE link between two
// nodes, so a second one (for other ops, or a second weight) would be ignored or ambiguous.
// Several ops go in one link's `ops`, a split goes in weights across different targets.
std::expected<model::LinkSpec, ConfigError> read_unique_link(const Site& site,
                                                             const YAML::Node& link,
                                                             const NodeIndex& ids,
                                                             std::set<DemandPair>& seen) {
  auto spec = read_link(site, link, ids);
  if (!spec || seen.insert(demand_pair(*spec)).second) return spec;
  return link_site(site, spec->from, spec->to)
      .fail(link, "duplicate link (put several ops or a weight on one link)");
}

std::expected<std::vector<model::LinkSpec>, ConfigError> collect_links(const Site& site,
                                                                       const YAML::Node& links,
                                                                       const NodeIndex& ids) {
  // A bare `links:` parses as null, which is not a list either.
  if (!links.IsSequence()) return site.fail(links, "links must be a list");
  std::set<DemandPair> seen;
  return collect_each(
      links, [&](const YAML::Node& link) { return read_unique_link(site, link, ids, seen); });
}

// ---- SLOs -----------------------------------------------------------------------------
//
// An SLO is a service-level objective, the "non-functional requirements" step of the
// book's framework (ch. 3): a bound a design must keep, checked against the simulated run.
// Latency percentiles are measured end to end for one client (`node`), optionally for one
// op; drop rate and utilization are ratios; queue lag (how far consumers trail producers)
// and staleness (the age of the data served) are times.

constexpr std::string_view slo_keys = "metric node op max";

using SloMaximum = decltype(model::SloSpec::max);

bool is_latency_percentile(model::SloMetric metric) {
  return metric == model::SloMetric::p50_latency || metric == model::SloMetric::p95_latency ||
         metric == model::SloMetric::p99_latency;
}

bool is_time_metric(model::SloMetric metric) {
  return is_latency_percentile(metric) || metric == model::SloMetric::queue_lag ||
         metric == model::SloMetric::staleness;
}

std::expected<model::SloMetric, ConfigError> read_slo_metric(const Site& site,
                                                             const YAML::Node& entry) {
  const auto value = entry["metric"];
  if (!value) return site.fail(entry, "entry needs a metric");
  const auto name = scalar_text(value);
  if (const auto metric = find_named(slo_metrics, name)) return *metric;
  return site.fail(value, std::format("unknown metric '{}'", name));
}

CheckResult read_slo_node(const Site& site, const YAML::Node& entry, const NodeIndex& ids,
                          std::optional<std::string>& node) {
  if (!entry["node"]) return {};
  auto id = scalar_value(site, entry, "node");
  if (!id) return std::unexpected(id.error());
  if (!is_declared(ids, *id))
    return site.fail(entry["node"], std::format("unknown node '{}'", *id));
  node = std::move(*id);
  return {};
}

CheckResult read_slo_op(const Site& site, const YAML::Node& entry, std::optional<std::string>& op) {
  if (!entry["op"]) return {};
  return read_op_name(site, entry["op"], "op name").transform(store_in(op));
}

// The variant's alternative follows the metric: a SimTime for the time metrics, a double
// for the ratios, so "0.01" can never be read as 0.01 seconds.
std::expected<SloMaximum, ConfigError> read_slo_max(const Site& site, const YAML::Node& entry,
                                                    model::SloMetric metric) {
  const auto name = name_of(slo_metrics, metric);
  const auto value = entry["max"];
  if (!value) return site.fail(entry, std::format("{} needs a max", name));
  if (is_time_metric(metric)) {
    if (const auto time = parse_duration(scalar_text(value))) return SloMaximum{*time};
    return site.fail(value, std::format("{} max must be a duration", name));
  }
  const auto number = parse_whole<double>(number_text(value));
  if (number && is_ratio(*number)) return SloMaximum{*number};
  return site.fail(value, std::format("{} max must be between 0 and 1", name));
}

bool offers_op(const model::NodeSpec& client, std::string_view op) {
  // C++20 std::views::keys: a view of each pair's first member (the op name).
  return client.workload &&
         std::ranges::contains(client.workload->op_shares | std::views::keys, op);
}

// A latency percentile is measured over requests, and requests start at a client: only a
// client's end-to-end response time is reported, so on any other node, or for an op the
// client never sends, the SLO could never fail. Without `op` it covers all the client's ops.
CheckResult check_latency_target(const Site& site, const YAML::Node& entry,
                                 const model::SloSpec& slo, const Declarations& declared) {
  if (!is_latency_percentile(slo.metric)) return {};
  const auto metric = name_of(slo_metrics, slo.metric);
  if (!slo.node) return site.fail(entry, std::format("{} needs a node", metric));
  const auto& node = declared.nodes[declared.ids.find(*slo.node)->second];
  if (node.type != model::NodeType::client) {
    return site.fail(entry["node"], std::format("{} needs a client node, '{}' is a {}", metric,
                                                node.id, model::to_string(node.type)));
  }
  if (!slo.op || offers_op(node, *slo.op)) return {};
  return site.fail(entry["op"], std::format("client '{}' has no op '{}'", node.id, *slo.op));
}

std::expected<model::SloSpec, ConfigError> read_slo(const Site& site, const YAML::Node& entry,
                                                    const Declarations& declared) {
  if (!entry.IsMap()) return site.fail(entry, "entry must be a map");
  if (auto checked = check_known_keys(site, entry, slo_keys, "key"); !checked) {
    return std::unexpected(checked.error());
  }
  auto metric = read_slo_metric(site, entry);
  if (!metric) return std::unexpected(metric.error());
  model::SloSpec slo{.metric = *metric};
  if (auto read = read_slo_node(site, entry, declared.ids, slo.node); !read) {
    return std::unexpected(read.error());
  }
  if (auto read = read_slo_op(site, entry, slo.op); !read) return std::unexpected(read.error());
  if (auto read = read_slo_max(site, entry, slo.metric).transform(store_in(slo.max)); !read) {
    return std::unexpected(read.error());
  }
  return check_latency_target(site, entry, slo, declared).transform([&] { return std::move(slo); });
}

std::expected<std::vector<model::SloSpec>, ConfigError> collect_slos(const Site& site,
                                                                     const YAML::Node& root,
                                                                     const Declarations& declared) {
  const auto list = root["slo"];
  if (!list) return std::vector<model::SloSpec>{};
  if (!list.IsSequence()) return site.fail(list, "slo must be a list");
  const auto entry_site = site.within("slo");
  return collect_each(
      list, [&](const YAML::Node& entry) { return read_slo(entry_site, entry, declared); });
}

// Pattern: Pipeline with early exit (railway-oriented error handling) — each stage returns
// std::expected<T, ConfigError> (T is void for a pure check, the parsed value otherwise),
// and the first error leaves the track and is returned, so the user sees one precise
// message. The stages are not independent: node bodies, links and SLOs need the ids the
// declaration pass collects, and SLOs need the clients' ops, so their order is fixed.
std::expected<model::Scenario, ConfigError> validate(const std::string& file,
                                                     const YAML::Node& root) {
  const Site site{.file = file, .where = ""};
  if (auto checked = check_top_level(site, root); !checked) return std::unexpected(checked.error());
  model::Scenario scenario;
  if (auto read = read_header(site, root, scenario); !read) return std::unexpected(read.error());
  if (auto read = read_datacenters(site, root).transform(store_in(scenario.datacenters)); !read) {
    return std::unexpected(read.error());
  }
  auto declared = collect_node_declarations(site, root["nodes"], scenario.datacenters);
  if (!declared) return std::unexpected(declared.error());
  if (auto read = read_node_bodies(site, root["nodes"], *declared); !read) {
    return std::unexpected(read.error());
  }
  auto links = collect_links(site, root["links"], declared->ids);
  if (!links) return std::unexpected(links.error());
  auto slos = collect_slos(site, root, *declared);
  if (!slos) return std::unexpected(slos.error());
  scenario.nodes = std::move(declared->nodes);
  scenario.links = std::move(*links);
  scenario.slos = std::move(*slos);
  return scenario;
}

}  // namespace simsys::config::detail

namespace simsys::config {

std::expected<model::Scenario, ConfigError> load(const std::string& file) {
  // The only exception boundary: yaml-cpp reports syntax errors (LoadFile) by throwing,
  // and so does any shape mistake the checks above do not pre-empt (subscripting a
  // scalar, reading a missing node); we return std::expected, so callers never need a
  // try/catch.
  try {
    return detail::validate(file, YAML::LoadFile(file));
  } catch (const YAML::Exception& e) {
    return std::unexpected(detail::error_at_mark(file, e.mark, e.msg));
  }
}

}  // namespace simsys::config
