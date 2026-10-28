// Module `simsys.config`, implementation unit: the checks behind `load`. yaml-cpp and
// <format> are included here, not in config.cppm (see the GCC 14.2 note there).
module;
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// C++20 module implementation unit: `module simsys.config;` (no `export`) makes this file
// part of the named module. It sees everything the interface declared, exports nothing,
// and can include heavy headers in its own global module fragment, like a .cpp behind a
// header.
module simsys.config;

import simsys.core;
import simsys.model;

namespace simsys::config::detail {

// C++23 std::expected<void, E>: "either fine or this error". It replaces a bool plus an
// out-parameter for the error; an empty `{}` means the check passed.
using CheckResult = std::expected<void, ConfigError>;

// The declared node ids. A set, not a scan of the declarations: every reference is looked
// up, and a linear scan per lookup made validation quadratic (60k nodes took 147 s).
// std::less<> (C++14 transparent comparator) lets the set be searched with a
// std::string_view directly, without building a temporary std::string per lookup.
using NodeIds = std::set<std::string, std::less<>>;

// What the first pass over `nodes` produces: the declarations in document order (for the
// Scenario) and their ids as a set (for the duplicate and reference checks).
struct Declarations {
  std::vector<model::NodeSpec> nodes;
  NodeIds ids;
};

using Datacenters = std::vector<std::string>;

constexpr std::array<std::string_view, 5> required_top_level_keys{"version", "name", "duration",
                                                                  "nodes", "links"};
// Key lists are space-separated words: one line per list reads like the schema's tables.
constexpr std::string_view top_level_keys =
    "version name seed duration step warmup datacenters nodes links slo faults";
// Node attributes whose value is the id of another node.
constexpr std::array<std::string_view, 3> reference_keys{"consumes", "backend", "origin"};

// Keys every node type accepts (docs/scenario-schema.md, "Node (common keys)").
constexpr std::string_view common_node_keys = "type dc replicas capacity latency ops then advanced";

// The keys a node type adds to the common ones.
struct OwnKeys {
  model::NodeType type;
  std::string_view keys;
};

// Pattern: Table-driven method — which keys a node type accepts is data, one row per type
// copied from the "Node types" table of docs/scenario-schema.md, not a switch of if-chains.
// Supporting a new key is one word in one row. The rows follow NodeType order so a type
// finds its row by index; the static_assert below proves that order at compile time.
constexpr std::array<OwnKeys, model::node_type_names.size()> own_keys_by_type{{
    {model::NodeType::client, "rate ops keys sizes timeout retries"},
    {model::NodeType::cdn, "ttl hit_ratio origin"},
    {model::NodeType::load_balancer, "policy vnodes"},
    {model::NodeType::rate_limiter, "algorithm limit burst backend"},
    {model::NodeType::service, ""},
    {model::NodeType::cache, "eviction strategy keys hit_ratio backend ttl then"},
    {model::NodeType::sql_db, "replication_lag shards pool"},
    {model::NodeType::kv_cluster, "nodes vnodes replication write_quorum read_quorum mode"},
    {model::NodeType::message_queue, "partitions retention"},
    {model::NodeType::worker_pool, "consumes workers cost then"},
    {model::NodeType::blob_store, "then"},
    {model::NodeType::search_index, "build_interval"},
    {model::NodeType::ws_gateway, "max_connections heartbeat fanout"},
    {model::NodeType::service_discovery, "failover"},
    {model::NodeType::id_generator, "algorithm"},
    {model::NodeType::external_sink, "failure_rate politeness"},
    {model::NodeType::notification_fanout, "channels dedup"},
    {model::NodeType::frontier, "politeness hosts then"},
}};

constexpr bool own_keys_follow_node_type_order() {
  for (std::size_t i = 0; i < own_keys_by_type.size(); ++i) {
    if (std::to_underlying(own_keys_by_type[i].type) != i) return false;
  }
  return true;
}
static_assert(own_keys_follow_node_type_order(), "own_keys_by_type rows must follow NodeType");

// The engine's zone thresholds when a scenario gives none (fluid::Capacity, resolved in
// simsys.defaults): ~70% / ~90% utilization, the operations rule of thumb for where M/M/1
// waiting time, growing like 1/(1-rho), starts to climb steeply and then turns dangerous.
// Restated rather than imported: the loader should not depend on the engine's modules.
constexpr double default_yellow = 0.70;
constexpr double default_red = 0.90;

// Range rules for the numeric `advanced` keys. Non-finite values (inf, nan) fail all of
// them: an infinite variability or threshold has no meaning in the queueing formulas.
bool is_positive(double v) {
  return std::isfinite(v) && v > 0;
}
bool is_non_negative(double v) {
  return std::isfinite(v) && v >= 0;
}
// A utilization fraction: above 0 (a zero threshold would colour an idle node) and at most
// 1 (utilization cannot exceed 100%).
bool is_unit_fraction(double v) {
  return v > 0 && v <= 1;
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
// range-checking code is written once. `concurrency` is an integer and is read separately.
constexpr std::array<AdvancedNumber, 5> advanced_numbers{{
    {"queue_limit", is_positive, "positive", &model::Advanced::queue_limit},
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

bool is_declared(const NodeIds& ids, std::string_view id) {
  // C++20 contains(): "is this key in the set?" in one call, replacing the
  // `find(key) != end()` comparison.
  return ids.contains(id);
}

// True when `word` is one of the space-separated words of `words`.
bool lists_word(std::string_view words, std::string_view word) {
  // C++20 std::views::split: a lazy view of the pieces between separators, replacing a
  // hand-written find/substr loop; each piece is a contiguous range of chars, so the C++20
  // iterator-pair constructor of std::string_view can view it without copying.
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
// ("must be a positive integer", ...) covers those shapes too.
std::string scalar_text(const YAML::Node& node) {
  return node.IsScalar() ? node.Scalar() : std::string{};
}

// The whole text as a T, or nothing: from_chars never throws and reports how far it got;
// the whole text must be consumed, so "0.5x" is no number and "4.0" is no int. For an int
// it rejects "abc" and accepts "-1" (callers check the sign).
template <typename T>
std::optional<T> parse_whole(std::string_view text) {
  T value{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
  return value;
}

// True when the whole text is a number ("1", "0.5"): a traffic share.
bool is_number(std::string_view text) {
  return parse_whole<double>(text).has_value();
}

// A unit parser's error, attributed to the offending value and prefixed with its key.
ConfigError unit_error(const std::string& file, const YAML::Node& value, std::string_view key,
                       const ParseError& error) {
  // C++20 std::format: type-checked (at compile time) formatting that returns a
  // std::string, replacing std::ostringstream chains and snprintf buffers.
  return error_at(file, value, std::format("{}: {}", key, error.message));
}

// The text of `parent[key]`, which the caller has checked is present. yaml-cpp's Scalar()
// silently returns "" for a list or a map, so that shape is rejected here, for every key.
std::expected<std::string, ConfigError> scalar_value(const std::string& file,
                                                     const YAML::Node& parent,
                                                     std::string_view key) {
  const auto value = parent[std::string{key}];
  if (value.IsSequence() || value.IsMap()) {
    return std::unexpected(error_at(file, value, std::format("{} must be a scalar", key)));
  }
  return value.Scalar();
}

std::expected<SimTime, ConfigError> parse_duration_key(const std::string& file,
                                                       const YAML::Node& parent,
                                                       std::string_view key) {
  const auto value = parent[std::string{key}];
  // C++23 std::expected::and_then: runs the next fallible step only if the previous one
  // produced a value, passing an error through untouched; it chains steps without an
  // `if (!x) return std::unexpected(x.error());` between them.
  // C++23 std::expected::transform_error: converts the error (ParseError -> ConfigError)
  // and passes a value through untouched.
  return scalar_value(file, parent, key).and_then([&](const std::string& text) {
    return parse_duration(text).transform_error(
        [&](const ParseError& error) { return unit_error(file, value, key, error); });
  });
}

// For durations a scenario may omit (step, warmup, latency): absent keeps `target`'s
// default, present must parse.
CheckResult read_optional_duration(const std::string& file, const YAML::Node& parent,
                                   std::string_view key, auto& target) {
  if (!parent[std::string{key}]) return {};
  // C++23 std::expected::transform: applies store_in's lambda to the value, if there is
  // one; the lambda returns nothing, so the result is std::expected<void, ConfigError>.
  return parse_duration_key(file, parent, key).transform(store_in(target));
}

CheckResult check_required_top_level_keys(const std::string& file, const YAML::Node& root) {
  for (const auto key : required_top_level_keys) {
    if (!root[std::string{key}]) {
      return std::unexpected(error_at(file, root, std::format("missing top-level key '{}'", key)));
    }
  }
  return {};
}

// A misspelt optional key ("wramup") would otherwise be ignored and its default used.
CheckResult check_known_top_level_keys(const std::string& file, const YAML::Node& root) {
  for (const auto& entry : root) {
    const auto& key = entry.first;
    if (lists_word(top_level_keys, key.Scalar())) continue;
    return std::unexpected(
        error_at(file, key, std::format("unknown top-level key '{}'", key.Scalar())));
  }
  return {};
}

CheckResult check_version(const std::string& file, const YAML::Node& root) {
  const auto version = root["version"];
  // as<int>(fallback) yields the fallback instead of throwing when the value is no int.
  if (version.as<int>(0) != 1) return std::unexpected(error_at(file, version, "version must be 1"));
  return {};
}

// An empty document is null, not a map: it falls through to the required-keys check, whose
// "missing top-level key" message is the more useful one. Any other non-map root (a bare
// scalar, a list) would make the key lookups below throw yaml-cpp's own error.
CheckResult check_root_is_map(const std::string& file, const YAML::Node& root) {
  if (root.IsDefined() && !root.IsNull() && !root.IsMap()) {
    return std::unexpected(error_at(file, root, "scenario must be a map"));
  }
  return {};
}

CheckResult check_top_level(const std::string& file, const YAML::Node& root) {
  if (auto checked = check_root_is_map(file, root); !checked) return checked;
  if (auto checked = check_required_top_level_keys(file, root); !checked) return checked;
  if (auto checked = check_known_top_level_keys(file, root); !checked) return checked;
  return check_version(file, root);
}

CheckResult read_name(const std::string& file, const YAML::Node& root, std::string& name) {
  auto text = scalar_value(file, root, "name");
  if (!text) return std::unexpected(text.error());
  // A null `name:` has the empty scalar too.
  if (text->empty()) return std::unexpected(error_at(file, root["name"], "name must not be empty"));
  name = std::move(*text);
  return {};
}

CheckResult read_seed(const std::string& file, const YAML::Node& root, int& seed) {
  const auto value = root["seed"];
  if (!value) return {};
  const auto number = parse_whole<int>(scalar_text(value));
  if (!number) return std::unexpected(error_at(file, value, "seed must be an integer"));
  seed = *number;
  return {};
}

// A zero step would never advance the clock; parse_duration already rejects negatives.
CheckResult read_step(const std::string& file, const YAML::Node& root, SimTime& step) {
  if (auto read = read_optional_duration(file, root, "step", step); !read) return read;
  if (step.count() > 0) return {};
  return std::unexpected(error_at(file, root["step"], "step must be positive"));
}

// The scalar header fields; an absent optional one keeps the Scenario's default.
CheckResult read_header(const std::string& file, const YAML::Node& root,
                        model::Scenario& scenario) {
  if (auto read = read_name(file, root, scenario.name); !read) return read;
  if (auto read = read_seed(file, root, scenario.seed); !read) return read;
  auto duration = parse_duration_key(file, root, "duration");
  if (!duration) return std::unexpected(duration.error());
  scenario.duration = *duration;
  if (auto read = read_step(file, root, scenario.step); !read) return read;
  return read_optional_duration(file, root, "warmup", scenario.warmup);
}

// `datacenters` maps each name to its settings (`{}` today); only the names, in file
// order, matter yet. `latency` is not a datacenter: the schema reserves it for cross-DC
// round-trip overrides, which a later step reads.
std::expected<Datacenters, ConfigError> read_datacenters(const std::string& file,
                                                         const YAML::Node& root) {
  const auto map = root["datacenters"];
  if (!map) return Datacenters{"main"};
  if (!map.IsMap()) return std::unexpected(error_at(file, map, "datacenters must be a map"));
  Datacenters names;
  for (const auto& entry : map) {
    if (entry.first.Scalar() != "latency") names.push_back(entry.first.Scalar());
  }
  if (names.empty()) return std::unexpected(error_at(file, map, "datacenters must not be empty"));
  return names;
}

// A misspelt key ("capactiy") would otherwise be ignored and the type default used.
CheckResult check_allowed_keys(const std::string& file, const YAML::Node& body,
                               const model::NodeSpec& node) {
  const auto own_keys = own_keys_by_type[std::to_underlying(node.type)].keys;
  for (const auto& entry : body) {
    const auto key = entry.first.Scalar();
    if (lists_word(common_node_keys, key) || lists_word(own_keys, key)) continue;
    return std::unexpected(error_at(file, entry.first,
                                    std::format("node '{}': key '{}' is not valid for type '{}'",
                                                node.id, key, model::to_string(node.type))));
  }
  return {};
}

// A node without `dc` lives in the first declared datacenter: `main` when the file
// declares none (the schema's default), the only one in a single-datacenter file.
CheckResult read_dc(const std::string& file, const YAML::Node& body, model::NodeSpec& node,
                    const Datacenters& datacenters) {
  if (!body["dc"]) {
    node.dc = datacenters.front();
    return {};
  }
  auto name = scalar_value(file, body, "dc");
  if (!name) return std::unexpected(name.error());
  // C++23 std::ranges::contains: "is this value in the range?" in one call, replacing
  // `std::find(begin, end, value) != end`.
  if (!std::ranges::contains(datacenters, *name)) {
    return std::unexpected(error_at(
        file, body["dc"], std::format("node '{}': unknown datacenter '{}'", node.id, *name)));
  }
  node.dc = std::move(*name);
  return {};
}

// `what` names the key in the message: "replicas", "advanced.concurrency".
std::expected<int, ConfigError> read_positive_int(const std::string& file, const YAML::Node& value,
                                                  std::string_view id, std::string_view what) {
  const auto number = parse_whole<int>(scalar_text(value));
  if (number && *number > 0) return *number;
  return std::unexpected(
      error_at(file, value, std::format("node '{}': {} must be a positive integer", id, what)));
}

CheckResult read_replicas(const std::string& file, const YAML::Node& body, model::NodeSpec& node) {
  if (!body["replicas"]) return {};
  return read_positive_int(file, body["replicas"], node.id, "replicas")
      .transform(store_in(node.replicas));
}

// Capacity counts requests for most node types and bytes for storage ("500MB/s"). The
// "B/s" suffix decides which parser reads it and which field receives it, so a text that
// neither parser accepts gets the message of the parser its suffix selects.
std::expected<void, ParseError> read_capacity_text(std::string_view text, model::NodeSpec& node) {
  // C++20 std::string_view::ends_with: a suffix test without size arithmetic.
  if (text.ends_with("B/s")) return parse_byte_rate(text).transform(store_in(node.byte_capacity));
  return parse_rate(text).transform(store_in(node.capacity));
}

CheckResult read_capacity(const std::string& file, const YAML::Node& body, model::NodeSpec& node) {
  const auto capacity = body["capacity"];
  if (!capacity) return {};
  return scalar_value(file, body, "capacity").and_then([&](const std::string& text) {
    return read_capacity_text(text, node).transform_error([&](const ParseError& error) {
      return unit_error(file, capacity, "capacity", error);
    });
  });
}

const AdvancedNumber* find_advanced_number(std::string_view key) {
  // C++20 ranges algorithm with a projection: `&AdvancedNumber::key` makes find compare
  // each row's key with `key`, replacing a hand-written lambda comparing one member.
  const auto row = std::ranges::find(advanced_numbers, key, &AdvancedNumber::key);
  return row == advanced_numbers.end() ? nullptr : &*row;
}

std::expected<double, ConfigError> read_advanced_number(const std::string& file,
                                                        const YAML::Node& value,
                                                        std::string_view id,
                                                        const AdvancedNumber& rule) {
  const auto number = parse_whole<double>(scalar_text(value));
  if (number && rule.in_range(*number)) return *number;
  return std::unexpected(
      error_at(file, value,
               std::format("node '{}': advanced.{} must be {}", id, rule.key, rule.requirement)));
}

CheckResult read_advanced_entry(const std::string& file, const YAML::Node& key,
                                const YAML::Node& value, model::NodeSpec& node) {
  const auto name = key.Scalar();
  if (name == "concurrency") {
    return read_positive_int(file, value, node.id, "advanced.concurrency")
        .transform(store_in(node.advanced.concurrency));
  }
  const auto* rule = find_advanced_number(name);
  if (!rule) {
    return std::unexpected(
        error_at(file, key, std::format("node '{}': unknown advanced key '{}'", node.id, name)));
  }
  return read_advanced_number(file, value, node.id, *rule)
      .transform(store_in(node.advanced.*(rule->target)));
}

// The zones must be ordered: green below yellow, yellow below red. A threshold given alone
// is compared with the engine's default for the other one.
CheckResult check_thresholds_ordered(const std::string& file, const YAML::Node& advanced,
                                     const model::NodeSpec& node) {
  const auto& a = node.advanced;
  if (!a.yellow && !a.red) return {};
  if (a.yellow.value_or(default_yellow) < a.red.value_or(default_red)) return {};
  return std::unexpected(
      error_at(file, advanced,
               std::format("node '{}': advanced.yellow must be below advanced.red", node.id)));
}

CheckResult read_advanced(const std::string& file, const YAML::Node& body, model::NodeSpec& node) {
  const auto advanced = body["advanced"];
  if (!advanced) return {};
  if (!advanced.IsMap()) {
    return std::unexpected(
        error_at(file, advanced, std::format("node '{}': advanced must be a map", node.id)));
  }
  for (const auto& entry : advanced) {
    if (auto read = read_advanced_entry(file, entry.first, entry.second, node); !read) return read;
  }
  return check_thresholds_ordered(file, advanced, node);
}

// The keys every node type shares. Node references (ops, then, backend, ...) are checked
// in the second pass, once every id is known.
CheckResult read_common_keys(const std::string& file, const YAML::Node& body, model::NodeSpec& node,
                             const Datacenters& datacenters) {
  if (auto read = check_allowed_keys(file, body, node); !read) return read;
  if (auto read = read_dc(file, body, node, datacenters); !read) return read;
  if (auto read = read_replicas(file, body, node); !read) return read;
  if (auto read = read_capacity(file, body, node); !read) return read;
  if (auto read = read_optional_duration(file, body, "latency", node.latency); !read) return read;
  return read_advanced(file, body, node);
}

std::expected<model::NodeType, ConfigError> read_node_type(const std::string& file,
                                                           const YAML::Node& id,
                                                           const YAML::Node& body) {
  if (!body.IsMap()) {
    return std::unexpected(
        error_at(file, body, std::format("node '{}' must be a map", id.Scalar())));
  }
  if (!body["type"]) {
    return std::unexpected(error_at(file, id, std::format("node '{}' needs a type", id.Scalar())));
  }
  auto type_name = scalar_value(file, body, "type");
  if (!type_name) return std::unexpected(type_name.error());
  const auto type = model::parse_node_type(*type_name);
  if (type) return *type;
  return std::unexpected(
      error_at(file, body["type"], std::format("unknown node type '{}'", *type_name)));
}

std::expected<model::NodeSpec, ConfigError> read_node_declaration(const std::string& file,
                                                                  const YAML::Node& id,
                                                                  const YAML::Node& body,
                                                                  const Datacenters& datacenters) {
  const auto type = read_node_type(file, id, body);
  if (!type) return std::unexpected(type.error());
  // Members the common keys leave unset keep the schema defaults declared in simsys.model;
  // ops, then and the type's own keys are typed in later steps.
  model::NodeSpec node{.id = id.Scalar(), .type = *type};
  if (auto read = read_common_keys(file, body, node, datacenters); !read) {
    return std::unexpected(read.error());
  }
  return node;
}

// The id goes into the set first, so a repeated id is reported before its body is read.
CheckResult add_node_declaration(const std::string& file, const YAML::Node& id,
                                 const YAML::Node& body, const Datacenters& datacenters,
                                 Declarations& declared) {
  if (id.Scalar().empty()) {
    return std::unexpected(error_at(file, id, "node id must not be empty"));
  }
  // set::insert returns {iterator, inserted}; `inserted` is false when the id was there.
  if (!declared.ids.insert(id.Scalar()).second) {
    return std::unexpected(error_at(file, id, std::format("duplicate node id '{}'", id.Scalar())));
  }
  auto declaration = read_node_declaration(file, id, body, datacenters);
  if (!declaration) return std::unexpected(declaration.error());
  declared.nodes.push_back(std::move(*declaration));
  return {};
}

// yaml-cpp iterates a map in document order, so the declarations keep that order. It also
// keeps both entries of a repeated key, which is what lets duplicates be detected here.
std::expected<Declarations, ConfigError> collect_node_declarations(const std::string& file,
                                                                   const YAML::Node& nodes,
                                                                   const Datacenters& datacenters) {
  if (!nodes.IsMap() || nodes.size() == 0) {
    return std::unexpected(error_at(file, nodes, "nodes must be a non-empty map"));
  }
  Declarations declared;
  for (const auto& entry : nodes) {
    if (auto added = add_node_declaration(file, entry.first, entry.second, datacenters, declared);
        !added) {
      return std::unexpected(added.error());
    }
  }
  return declared;
}

// `parent[key]` (present) must name a declared node. `what` names the referring construct
// in the message: "step", "link", "backend", ...
CheckResult check_reference(const std::string& file, const YAML::Node& parent, std::string_view key,
                            std::string_view what, const NodeIds& ids) {
  const auto reference = parent[std::string{key}];
  return scalar_value(file, parent, key).and_then([&](const std::string& id) -> CheckResult {
    if (is_declared(ids, id)) return {};
    return std::unexpected(
        error_at(file, reference, std::format("{} refers to unknown node '{}'", what, id)));
  });
}

CheckResult check_step(const std::string& file, const YAML::Node& step, const NodeIds& ids) {
  if (!step.IsMap()) return std::unexpected(error_at(file, step, "step must be a map"));
  if (!step["to"]) return std::unexpected(error_at(file, step, "step needs a 'to'"));
  return check_reference(file, step, "to", "step", ids);
}

CheckResult check_step_list(const std::string& file, const YAML::Node& steps, const NodeIds& ids) {
  for (const auto& step : steps) {
    if (auto checked = check_step(file, step, ids); !checked) return checked;
  }
  return {};
}

// An op is a step list (a service's op) or a share (a client's op, a number); a share names
// no node. Anything else, such as a map or a word, is neither and would silently do nothing.
CheckResult check_op(const std::string& file, const YAML::Node& name, const YAML::Node& value,
                     const NodeIds& ids) {
  if (is_sequence(value)) return check_step_list(file, value, ids);
  if (value.IsScalar() && is_number(value.Scalar())) return {};
  return std::unexpected(
      error_at(file, value, std::format("op '{}' must be a step list or a share", name.Scalar())));
}

// `ops` is optional; when present it is a map from op name to step list or share.
CheckResult check_op_steps(const std::string& file, const YAML::Node& ops, const NodeIds& ids) {
  if (!ops) return {};
  if (!ops.IsMap()) return std::unexpected(error_at(file, ops, "ops must be a map"));
  for (const auto& op : ops) {
    if (auto checked = check_op(file, op.first, op.second, ids); !checked) return checked;
  }
  return {};
}

// `then` (a worker pool's follow-up steps) is optional, but when present it is a step list.
CheckResult check_then(const std::string& file, const YAML::Node& body, const NodeIds& ids) {
  const auto then = body["then"];
  if (!then) return {};
  if (!then.IsSequence()) return std::unexpected(error_at(file, then, "then must be a list"));
  return check_step_list(file, then, ids);
}

CheckResult check_reference_key(const std::string& file, const YAML::Node& body,
                                std::string_view key, const NodeIds& ids) {
  if (!body[std::string{key}]) return {};
  return check_reference(file, body, key, key, ids);
}

// The body is known to be a map: the declaration pass checked it.
CheckResult check_node_references(const std::string& file, const YAML::Node& body,
                                  const NodeIds& ids) {
  if (auto checked = check_op_steps(file, body["ops"], ids); !checked) return checked;
  if (auto checked = check_then(file, body, ids); !checked) return checked;
  for (const auto key : reference_keys) {
    if (auto checked = check_reference_key(file, body, key, ids); !checked) return checked;
  }
  return {};
}

// A second pass over the node bodies, after all ids are known, so a node may refer to
// one declared further down the file.
CheckResult check_node_bodies(const std::string& file, const YAML::Node& nodes,
                              const NodeIds& ids) {
  for (const auto& entry : nodes) {
    if (auto checked = check_node_references(file, entry.second, ids); !checked) return checked;
  }
  return {};
}

CheckResult check_link_end(const std::string& file, const YAML::Node& link, std::string_view end,
                           const NodeIds& ids) {
  if (!link[std::string{end}]) {
    return std::unexpected(error_at(file, link, std::format("link needs '{}'", end)));
  }
  return check_reference(file, link, end, "link", ids);
}

std::expected<model::LinkSpec, ConfigError> read_link(const std::string& file,
                                                      const YAML::Node& link, const NodeIds& ids) {
  if (!link.IsMap()) return std::unexpected(error_at(file, link, "link must be a map"));
  if (auto from = check_link_end(file, link, "from", ids); !from) {
    return std::unexpected(from.error());
  }
  if (auto to = check_link_end(file, link, "to", ids); !to) return std::unexpected(to.error());
  model::LinkSpec spec{.from = link["from"].Scalar(), .to = link["to"].Scalar()};
  if (auto latency = read_optional_duration(file, link, "latency", spec.latency); !latency) {
    return std::unexpected(latency.error());
  }
  return spec;
}

std::expected<std::vector<model::LinkSpec>, ConfigError> collect_links(const std::string& file,
                                                                       const YAML::Node& links,
                                                                       const NodeIds& ids) {
  // A bare `links:` parses as null, which is not a list either.
  if (!links.IsSequence()) return std::unexpected(error_at(file, links, "links must be a list"));
  std::vector<model::LinkSpec> collected;
  for (const auto& link : links) {
    auto read = read_link(file, link, ids);
    if (!read) return std::unexpected(read.error());
    collected.push_back(std::move(*read));
  }
  return collected;
}

// Pattern: Validation pipeline — a sequence of independent checks run in order; the first
// error stops the run so the user sees one precise message. Each check returns
// std::expected<T, ConfigError> (T is void for a pure check, the parsed value otherwise).
std::expected<model::Scenario, ConfigError> validate(const std::string& file,
                                                     const YAML::Node& root) {
  if (auto checked = check_top_level(file, root); !checked) return std::unexpected(checked.error());
  model::Scenario scenario{.version = root["version"].as<int>()};
  if (auto read = read_header(file, root, scenario); !read) return std::unexpected(read.error());
  if (auto read = read_datacenters(file, root).transform(store_in(scenario.datacenters)); !read) {
    return std::unexpected(read.error());
  }
  auto declared = collect_node_declarations(file, root["nodes"], scenario.datacenters);
  if (!declared) return std::unexpected(declared.error());
  if (auto checked = check_node_bodies(file, root["nodes"], declared->ids); !checked) {
    return std::unexpected(checked.error());
  }
  auto links = collect_links(file, root["links"], declared->ids);
  if (!links) return std::unexpected(links.error());
  scenario.nodes = std::move(declared->nodes);
  scenario.links = std::move(*links);
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
