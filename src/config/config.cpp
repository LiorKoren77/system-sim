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
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
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
// Only for defaults::max_concurrency: the cap the engine's defaults apply is the cap the
// loader enforces, one number in one place.
import simsys.defaults;
// Only for fluid::Capacity's default zone thresholds: one source of truth for 0.7 / 0.9.
import simsys.fluid;
import simsys.model;
// The rate curve types a client's `rate:` block is read into.
import simsys.workload;

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
    {model::NodeType::cdn, "ttl hit_ratio origin keys write_ops"},
    {model::NodeType::load_balancer, "policy vnodes"},
    {model::NodeType::rate_limiter, "algorithm limit burst window backend"},
    {model::NodeType::service, "ops cost"},
    {model::NodeType::cache, "eviction strategy keys hit_ratio backend ttl then cost write_ops"},
    {model::NodeType::sql_db, "replication_lag shards pool write_ops cost"},
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

// Range rules for the numeric keys. NaN and infinity written as such never reach them
// (parse_yaml_double rejects them as no finite number); a decimal too large for a double
// (1e309) arrives rounded to infinity and fails every rule but is_zero_or_more, whose key
// has its own upper bound. An infinite variability or threshold has no meaning in the
// queueing formulas.
bool is_positive(double v) {
  return std::isfinite(v) && v > 0;
}
bool is_non_negative(double v) {
  return std::isfinite(v) && v >= 0;
}
// Non-negative, infinity included: for a key whose upper bound is a separate rule with its
// own message, such as a link weight, so 1e309 is reported as too large, not as negative.
bool is_zero_or_more(double v) {
  return v >= 0;
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

// Upper bounds that keep user input inside the engine's arithmetic. Past them a rate
// overflows to infinity somewhere downstream, and the engine treats a non-finite rate as a
// programmer error (it throws), so the loader must reject the input first. Chosen, not
// from the book: 1e15 requests/s is ten orders of magnitude above the book's estimates
// (search autocomplete's ~24k QPS, ch. 13, for instance), and a factor of 1e6 is fan-out on
// write to a million followers (ch. 11's celebrity problem), so no design comes near either; 1e15 x
// 1e6 = 1e21 is still far from the double's limit (~1.8e308), leaving room for long chains of
// factors.
inline constexpr double max_rate_per_second = 1e15;
inline constexpr double max_step_factor = 1e6;

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
// range-checking code is written once. The integer keys are not rows: they are read like
// `replicas` (see advanced_integer_keys).
constexpr std::array<AdvancedNumber, 4> advanced_numbers{{
    {"service_variability", is_non_negative, "a finite non-negative number",
     &model::Advanced::service_variability},
    {"burstiness", is_non_negative, "a finite non-negative number", &model::Advanced::burstiness},
    {"yellow", is_unit_fraction, "in (0, 1]", &model::Advanced::yellow},
    {"red", is_unit_fraction, "in (0, 1]", &model::Advanced::red},
}};

// The advanced keys the schema types as int, read by the integer rule of `replicas` (a plain
// whole number: no "3.0", no "1e3"). `queue_limit` is a double in the model, as the queueing
// formulas take it, but a waiting room holds whole requests.
constexpr std::string_view advanced_integer_keys = "concurrency queue_limit";

// How a scenario writes an empty list or map, for the messages about a key left without a
// value where an empty one is valid (`links:` -> "write [] for none").
constexpr std::string_view empty_list = "[]";
constexpr std::string_view empty_map = "{}";

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

// True for a key written with nothing after it; false for a missing key, too.
bool is_null(const YAML::Node& node) {
  return node.IsDefined() && node.IsNull();
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

// Why a numeric text is no value of the wanted type. `out_of_range` is a well-formed number
// beyond the type's range (99999999999 for an int, 1e309 for a double), so a reader can
// answer with the key's range message; anything else is `not_a_number`.
enum class NumberFault { not_a_number, out_of_range };

// The whole text as a T: from_chars never throws and reports how far it got; the whole text
// must be consumed, so "0.5x" is no number and "4.0" is no int. For an int it rejects "abc",
// "+1" and "0x1" and accepts "-1" (callers check the sign).
template <typename T>
std::expected<T, NumberFault> parse_whole(std::string_view text) {
  T value{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (end != text.data() + text.size()) return std::unexpected(NumberFault::not_a_number);
  if (error == std::errc::result_out_of_range) return std::unexpected(NumberFault::out_of_range);
  if (error != std::errc{}) return std::unexpected(NumberFault::not_a_number);
  return value;
}

// A decimal beyond the range of a double, rounded as IEEE 754 rounds it: an overflow to
// infinity (1e309, -1e309), an underflow to zero or a subnormal (1e-400). from_chars says
// only "out of range", not which way; std::strtod (C89) returns the rounded value
// (+-HUGE_VAL, infinity under IEEE 754, for an overflow). It only sees text that from_chars
// read whole as a decimal, which it reads the same way: the program never calls setlocale,
// so the "C" locale's '.' is the decimal point for both.
double rounded_out_of_range(std::string_view text) {
  return std::strtod(std::string{text}.c_str(), nullptr);
}

// A number as YAML writes it, or nothing.
// - Not-a-number and infinity are no finite number in any spelling, so every key answers
//   them with its own "must be a finite ..." message: YAML's `.nan`, `.inf`, `-.inf` are no
//   text from_chars reads, and its own `nan`, `inf`, `infinity` are read as values and
//   rejected by the one isfinite test below.
// - A decimal beyond a double's range is a number all the same, rounded (see above), so the
//   key's range rule rejects 1e309 with its range message ("weight must be at most 1e9"),
//   not as no number, and 1e-400 is zero.
std::optional<double> parse_yaml_double(std::string_view text) {
  const auto value = parse_whole<double>(text);
  if (value) return std::isfinite(*value) ? std::optional{*value} : std::nullopt;
  if (value.error() == NumberFault::out_of_range) return rounded_out_of_range(text);
  return std::nullopt;
}

// The value of a numeric key, or nothing when it is no number (see number_text).
std::optional<double> number_value(const YAML::Node& node) {
  return parse_yaml_double(number_text(node));
}

// The key node of `parent[key]`, for messages about the key itself. yaml-cpp's lookup
// returns only the value.
YAML::Node key_node(const YAML::Node& parent, std::string_view key) {
  for (const auto& entry : parent) {
    if (entry.first.IsScalar() && entry.first.Scalar() == key) return entry.first;
  }
  return parent;
}

// The node an error about the value of `parent[key]` points at. A key written with nothing
// after it (`capacity:`, `ops:`) holds null, and yaml-cpp places a null value where the next
// token starts, usually the next line: for a null the key is where the reader must look.
// Any other value is its own best location.
YAML::Node shape_site(const YAML::Node& parent, std::string_view key) {
  const auto value = parent[std::string{key}];
  return is_null(value) ? key_node(parent, key) : value;
}

// The same for one entry of a map being iterated, whose key node is at hand.
YAML::Node shape_site(const YamlElement& entry) {
  return entry.second.IsNull() ? entry.first : entry.second;
}

// "<label> must not be empty". Where an empty list or map is valid, `none` ("[]", "{}") says
// how to write one, so the message does not contradict `links: []` loading fine.
std::string empty_message(std::string_view label, std::string_view none) {
  if (none.empty()) return std::format("{} must not be empty", label);
  return std::format("{} must not be empty (write {} for none)", label, none);
}

// A key holding null is reported as empty, at the key (shape_site), whatever the value's
// type: a scalar, a list or a map. Every reader runs this before its own shape check, so no
// "must be a map" ever points at the line after a bare key. `label` names the key in the
// message ("capacity", "rate.base", "op share 'ping'").
CheckResult check_not_null(const Site& site, const YAML::Node& parent, std::string_view key,
                           std::string_view label, std::string_view none = {}) {
  if (!is_null(parent[std::string{key}])) return {};
  return site.fail(shape_site(parent, key), empty_message(label, none));
}

// The same for one entry of a map being iterated (an op share, an advanced key, a datacenter).
CheckResult check_entry_not_null(const Site& site, const YamlElement& entry, std::string_view label,
                                 std::string_view none = {}) {
  if (!entry.second.IsNull()) return {};
  return site.fail(shape_site(entry), empty_message(label, none));
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

// The value a unit parser (parse_duration, parse_rate, parse_time_point, ...) produces.
template <typename Parse>
using Parsed = typename std::invoke_result_t<Parse&, std::string_view>::value_type;

// One scalar read by a unit parser; `label` names it in the messages ("rate.wave.period",
// "rate.points[2]"). A list or a map is no text to parse.
template <typename Parse>
std::expected<Parsed<Parse>, ConfigError> parse_scalar(const Site& site, const YAML::Node& value,
                                                       std::string_view label, Parse parse) {
  if (!value.IsScalar()) return site.fail(value, std::format("{} must be a scalar", label));
  // C++23 std::expected::transform_error: converts the error (ParseError -> ConfigError)
  // and passes a value through untouched.
  return parse(value.Scalar()).transform_error([&](const ParseError& error) {
    return unit_error(site, value, label, error);
  });
}

// `parent[key]`, which the caller has checked is present, read by a unit parser. A key
// with nothing after it is reported as empty, at the key (check_not_null).
template <typename Parse>
std::expected<Parsed<Parse>, ConfigError> parse_key(const Site& site, const YAML::Node& parent,
                                                    std::string_view key, std::string_view label,
                                                    Parse parse) {
  // C++23 std::expected::and_then: runs the next fallible step only if the previous one
  // produced a value, passing an error through untouched; it chains steps without an
  // `if (!x) return std::unexpected(x.error());` between them.
  return check_not_null(site, parent, key, label).and_then([&] {
    return parse_scalar(site, parent[std::string{key}], label, parse);
  });
}

std::expected<SimTime, ConfigError> parse_duration_key(const Site& site, const YAML::Node& parent,
                                                       std::string_view key) {
  return parse_key(site, parent, key, key, parse_duration);
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

// The name a map key spells. A list or map is valid YAML as a key (`[a, b]: 1`) but names
// nothing, and yaml-cpp's Scalar() reads it as "", which surfaced as "unknown key ''". `noun`
// names the key in the message: "top-level key", "node id", "advanced key", ... A null key
// (`~: 1`) is passed on as "", so the caller's own empty-name message applies.
std::expected<std::string, ConfigError> key_name(const Site& site, const YAML::Node& key,
                                                 std::string_view noun) {
  if (key.IsMap() || key.IsSequence()) {
    return site.fail(key, std::format("{} must be a scalar", noun));
  }
  return scalar_text(key);
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

// ---- Alias expansion -----------------------------------------------------------------
//
// YAML aliases: `&name` anchors a node and `*name` refers back to it. yaml-cpp resolves an
// alias to the anchored node itself, shared, not to a copy. A walk that follows every
// reference therefore meets a shared node once per path to it, and nested aliases multiply
// the paths: 8 nested lists that each repeat the level below 10 times reach the innermost
// map 10^8 times from a few hundred bytes of text (the "billion laughs" pattern). The walk
// below descends into each map and list once, so the repeated-key check stays cheap, and a
// shared node is checked once, which is also the right meaning: its keys are the same
// wherever it is referenced.
//
// The typed readers that build the Scenario do expand aliases: to them a shared step list is
// there once per op that refers to it. Three nested anchors (one step list in every op, one
// op map in every service) make a few kilobytes of text into millions of steps; a 10 KB
// file of that shape took about a minute and 800 MB to load. So the same walk also measures
// the expanded document and the loader refuses it past a cap, before any reader runs.
//
// yaml-cpp 0.8 does not implement YAML merge keys (`<<: *base`): `<<` is an ordinary key
// here, checked like any other, and adds no expansion of its own.

// The most entries (maps, lists and scalar values, each counted once per place the readers
// meet it) a scenario may expand to. The largest shipped scenario has 247; a million is far
// beyond any hand-written design and loads in seconds (about 4 s for 986 000 entries in a
// Debug build, measured 2026-10-28). A file of a million entries written out without
// aliases (tens of megabytes) gets the same message.
inline constexpr std::size_t max_expanded_entries = 1'000'000;

// The count of an expansion past what std::size_t holds, or of a node that contains itself:
// yaml-cpp registers an anchor when its node starts, so `&loop [*loop]` is a list holding
// itself, and its expansion has no end.
inline constexpr std::size_t uncountable = std::numeric_limits<std::size_t>::max();

std::size_t add_saturating(std::size_t a, std::size_t b) {
  return b > uncountable - a ? uncountable : a + b;
}

// "This memo entry is `node`": is() is yaml-cpp's only identity test.
auto holds_node(const YAML::Node& node) {
  return [&node](const auto& entry) { return entry.node.is(node); };
}

// Pattern: Memoization — the expanded size of each map and list, computed once and looked up
// at every later reference; it doubles as the visited set (the guard of a graph traversal)
// that makes the depth-first walk visit each vertex of the alias DAG once.
//
// yaml-cpp gives a node no address or id; its only identity test is `a.is(b)`. An alias
// reports its anchor's Mark (line, column), so a shared node is filed under one position.
// The memo files nodes by Mark and compares with is() only within one position. The maps
// and lists the walk visits start at distinct positions; a map used as a key can share its
// position with its parent (a block map's Mark is that of its first key), but the walk
// never descends into keys, and is() keeps a lookup correct whenever two nodes do share a
// position. A bucket so holds one node in practice, a lookup costs O(log n), and the walk
// O(n log n).
class ExpandedSizes {
 public:
  // The size recorded for `node`, or nothing the first time the walk meets it.
  [[nodiscard]] std::optional<std::size_t> find(const YAML::Node& node) const {
    const auto bucket = by_position_.find(position(node));
    if (bucket == by_position_.end()) return std::nullopt;
    const auto entry = std::ranges::find_if(bucket->second, holds_node(node));
    if (entry == bucket->second.end()) return std::nullopt;
    return entry->size;
  }

  // Records `node`'s size, replacing the one recorded before.
  void record(const YAML::Node& node, std::size_t size) {
    auto& bucket = by_position_[position(node)];
    const auto entry = std::ranges::find_if(bucket, holds_node(node));
    if (entry != bucket.end()) {
      entry->size = size;
    } else {
      bucket.push_back({.node = node, .size = size});
    }
  }

 private:
  struct Entry {
    YAML::Node node;
    std::size_t size;
  };

  static std::pair<int, int> position(const YAML::Node& node) {
    return {node.Mark().line, node.Mark().column};
  }

  std::map<std::pair<int, int>, std::vector<Entry>> by_position_;
};

// One depth-first walk over every map and list of the document: checks each map's keys
// (once per map) and returns how many entries `node` expands to. A map or list is 1 plus the
// expanded sizes of its values or elements, each counted once per occurrence; a scalar is 1;
// keys are names, not entries, and are not descended into.
std::expected<std::size_t, ConfigError> walk_expanded(const Site& site, const YAML::Node& node,
                                                      ExpandedSizes& sizes) {
  if (!node.IsMap() && !node.IsSequence()) return 1;
  if (const auto known = sizes.find(node)) return *known;
  // Uncountable while its children are walked: met again before then, it contains itself.
  sizes.record(node, uncountable);
  if (node.IsMap()) {
    if (auto checked = check_map_keys_unique(site, node, "key"); !checked) {
      return std::unexpected(checked.error());
    }
  }
  std::size_t size = 1;
  for (const auto& element : node) {
    // A map entry's child is its value; a list element is itself the child (a YamlElement
    // is a Node too, hence the cast to pick that view of it).
    const auto& child = node.IsMap() ? element.second : static_cast<const YAML::Node&>(element);
    const auto child_size = walk_expanded(site, child, sizes);
    if (!child_size) return child_size;
    size = add_saturating(size, *child_size);
  }
  sizes.record(node, size);
  return size;
}

std::string entry_count(std::size_t entries) {
  if (entries == uncountable) return "more entries than can be counted";
  return std::format("{} entries", entries);
}

CheckResult check_expansion_capped(const Site& site, const YAML::Node& root, std::size_t entries) {
  if (entries <= max_expanded_entries) return {};
  return site.fail(root, std::format("aliases expand the scenario to {}; at most {}",
                                     entry_count(entries), max_expanded_entries));
}

// The top-level keys, then the node ids (with the more specific message), then one walk over
// the whole document: every map's keys, and the size the aliases expand it to.
CheckResult check_keys_and_expansion(const Site& site, const YAML::Node& root) {
  if (!root.IsMap()) return {};
  if (auto checked = check_map_keys_unique(site, root, "key"); !checked) return checked;
  if (const auto nodes = root["nodes"]; nodes.IsMap()) {
    if (auto checked = check_map_keys_unique(site, nodes, "node id"); !checked) return checked;
  }
  ExpandedSizes sizes;
  return walk_expanded(site, root, sizes).and_then([&](std::size_t entries) {
    return check_expansion_capped(site, root, entries);
  });
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
    auto key = key_name(site, entry.first, "top-level key");
    if (!key) return std::unexpected(key.error());
    if (lists_word(top_level_keys, *key)) continue;
    return site.fail(entry.first, std::format("unknown top-level key '{}'", *key));
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

// A top-level key whose value is a list or map, and how it is written empty when that is
// valid ("" when it is not: a scenario needs nodes, and `datacenters` declares at least one).
struct Section {
  std::string_view key;
  std::string_view none;
};

constexpr std::array<Section, 5> sections{{
    {"datacenters", ""},
    {"nodes", ""},
    {"links", empty_list},
    {"slo", empty_list},
    {"faults", empty_list},
}};

// A bare `links:` is reported here, at its key, before any reader asks for a list.
CheckResult check_sections_not_null(const Site& site, const YAML::Node& root) {
  for (const auto& section : sections) {
    if (auto checked = check_not_null(site, root, section.key, section.key, section.none);
        !checked) {
      return checked;
    }
  }
  return {};
}

CheckResult check_top_level(const Site& site, const YAML::Node& root) {
  if (auto checked = check_root_is_map(site, root); !checked) return checked;
  if (auto checked = check_keys_and_expansion(site, root); !checked) return checked;
  if (auto checked = check_required_top_level_keys(site, root); !checked) return checked;
  if (auto checked = check_known_top_level_keys(site, root); !checked) return checked;
  if (auto checked = check_sections_not_null(site, root); !checked) return checked;
  return check_faults_is_list(site, root);
}

// ---- Header ---------------------------------------------------------------------------

// Read like every other integer key (a plain scalar through parse_whole), so "+1", "0x1",
// "1.0" and a quoted "1" are no version.
CheckResult read_version(const Site& site, const YAML::Node& root, int& version) {
  if (auto checked = check_not_null(site, root, "version", "version"); !checked) return checked;
  const auto value = root["version"];
  const auto number = parse_whole<int>(number_text(value));
  if (!number && number.error() == NumberFault::not_a_number) {
    return site.fail(value, "version must be a whole number");
  }
  // A whole number past the int range is no 1 either.
  if (!number || *number != 1) return site.fail(value, "version must be 1");
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
  if (!number && number.error() == NumberFault::out_of_range) {
    return site.fail(value,
                     std::format("seed must be between {} and {}", std::numeric_limits<int>::min(),
                                 std::numeric_limits<int>::max()));
  }
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

// A run of zero length has no step to report.
CheckResult read_run_duration(const Site& site, const YAML::Node& root, SimTime& duration) {
  auto read = parse_duration_key(site, root, "duration");
  if (!read) return std::unexpected(read.error());
  if (read->count() <= 0) return site.fail(root["duration"], "duration must be positive");
  duration = *read;
  return {};
}

// The report covers the steps from `warmup` to `duration`. A warmup that is not shorter
// leaves that window empty, and an SLO checked against no data would pass on nothing.
// Compared by tick count (nanoseconds), as read_time_step does.
CheckResult read_warmup(const Site& site, const YAML::Node& root, model::Scenario& scenario) {
  if (auto read = read_optional_duration(site, root, "warmup", scenario.warmup); !read) {
    return read;
  }
  if (scenario.warmup.count() < scenario.duration.count()) return {};
  return site.fail(root["warmup"], "warmup must be shorter than duration");
}

// The scalar header fields; an absent optional one keeps the Scenario's default.
CheckResult read_header(const Site& site, const YAML::Node& root, model::Scenario& scenario) {
  if (auto read = read_version(site, root, scenario.version); !read) return read;
  if (auto read = read_name(site, root, scenario.name); !read) return read;
  if (auto read = read_seed(site, root, scenario.seed); !read) return read;
  if (auto read = read_run_duration(site, root, scenario.duration); !read) return read;
  if (auto read = read_time_step(site, root, scenario.step); !read) return read;
  return read_warmup(site, root, scenario);
}

// ---- Datacenters ----------------------------------------------------------------------

// A key under `datacenters` is a name: a non-empty scalar. A list or map is valid YAML as a
// key but names nothing.
CheckResult check_datacenter_name(const Site& site, const YAML::Node& name) {
  if (name.IsMap() || name.IsSequence()) return site.fail(name, "name must be a scalar");
  if (scalar_text(name).empty()) return site.fail(name, "name must not be empty");
  return {};
}

// A datacenter's value is its settings map. The schema reserves the map for later keys and
// defines none yet, so it must be `{}`: any key in it is unknown.
CheckResult check_datacenter_settings(const Site& site, const YamlElement& entry) {
  const auto label = std::format("'{}'", entry.first.Scalar());
  if (auto checked = check_entry_not_null(site, entry, label, empty_map); !checked) return checked;
  if (!entry.second.IsMap()) return site.fail(entry.second, std::format("{} must be a map", label));
  if (entry.second.size() == 0) return {};
  const auto key = (*entry.second.begin()).first;
  return key_name(site, key, std::format("{} key", label))
      .and_then([&](const std::string& name) -> CheckResult {
        return site.fail(key, std::format("{} has unknown key '{}'", label, name));
      });
}

CheckResult read_datacenter(const Site& site, const YamlElement& entry, Datacenters& names) {
  if (auto checked = check_datacenter_settings(site, entry); !checked) return checked;
  names.push_back(entry.first.Scalar());
  return {};
}

// `latency` is not a datacenter: the schema reserves it, the only such key under
// `datacenters`, for the list of cross-DC round-trip overrides, which a later step reads;
// only its shape is checked now.
CheckResult check_latency_overrides(const Site& site, const YamlElement& entry) {
  if (auto checked = check_entry_not_null(site, entry, "latency", empty_list); !checked) {
    return checked;
  }
  if (entry.second.IsSequence()) return {};
  return site.fail(entry.second, "latency must be a list");
}

CheckResult read_datacenter_entry(const Site& site, const YamlElement& entry, Datacenters& names) {
  if (auto checked = check_datacenter_name(site, entry.first); !checked) return checked;
  if (entry.first.Scalar() == "latency") return check_latency_overrides(site, entry);
  return read_datacenter(site, entry, names);
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
  // About `datacenters` as a whole, so at its key: the map's own position is its first entry
  // (`latency`, the only key that declares no datacenter).
  if (names.empty()) {
    return site.fail(key_node(root, "datacenters"), "datacenters must not be empty");
  }
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

// Capacity counts requests for most node types and bytes for storage ("500MB/s"). The
// "B/s" suffix decides which parser reads it and which field receives it, so a text that
// neither parser accepts gets the message of the parser its suffix selects.
std::expected<void, ParseError> read_capacity_text(std::string_view text, model::NodeSpec& node) {
  // C++20 std::string_view::ends_with (and its twin starts_with): a suffix test in one call,
  // replacing compare(size() - n, n, suffix) and its length arithmetic.
  if (text.ends_with("B/s")) return parse_byte_rate(text).transform(store_in(node.byte_capacity));
  return parse_rate(text).transform(store_in(node.capacity));
}

// A byte capacity is bounded by the same number, in bytes: 1e15 B/s is 1e9 objects of 1 MB
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
      .and_then([&] { return check_capacity_bounded(site, capacity, node); });
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

// ---- Per-op cost (plan 2a refinement 4) ----

// The types whose `cost` is a map op -> capacity units (a SQL database's writes default to
// defaults::sql_write_cost; a stated cost replaces it). A worker_pool's `cost` is a
// different key with the same spelling (a duration per job, read in phase 2c), so the
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

// ---- Load balancer keys (plan 2a refinement 5) ----

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

// `vnodes: 150`: virtual nodes per server on the hash ring, so it means something only to
// consistent_hash; on another policy it is a leftover from an edit, not a no-op to accept.
// Absent, the graph uses model::default_vnodes.
CheckResult read_vnodes(const Site& site, const YAML::Node& body, model::NodeSpec& node) {
  const auto value = body["vnodes"];
  if (!value) return {};
  if (auto checked = check_not_null(site, body, "vnodes", "vnodes"); !checked) return checked;
  auto count = read_positive_int(site, value, "vnodes");
  if (!count) return std::unexpected(count.error());
  if (node.policy != model::LbPolicy::consistent_hash) {
    return site.fail(value, "vnodes applies only to consistent_hash");
  }
  node.vnodes = *count;
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

Site node_site(const Site& site, std::string_view id) {
  return site.within(std::format("node '{}'", id));
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

// "step refers to unknown node 'x'"; without `what` (when the site already names the
// referring construct, as "link 1" does) just "refers to unknown node 'x'".
std::string unknown_node_message(std::string_view what, std::string_view id) {
  if (what.empty()) return std::format("refers to unknown node '{}'", id);
  return std::format("{} refers to unknown node '{}'", what, id);
}

// `parent[key]` (present) must name a declared node. `what` names the referring construct
// in the message: "step", "backend", ...
CheckResult check_reference(const Site& site, const YAML::Node& parent, std::string_view key,
                            std::string_view what, const NodeIndex& ids) {
  const auto reference = parent[std::string{key}];
  return scalar_value(site, parent, key).and_then([&](const std::string& id) -> CheckResult {
    if (is_declared(ids, id)) return {};
    return site.fail(reference, unknown_node_message(what, id));
  });
}

// A misspelt key would otherwise be ignored. `kind` names the key in the message:
// "unknown step key 'x'", "unknown key 'x'".
CheckResult check_known_keys(const Site& site, const YAML::Node& map, std::string_view allowed,
                             std::string_view kind) {
  for (const auto& entry : map) {
    auto key = key_name(site, entry.first, kind);
    if (!key) return std::unexpected(key.error());
    if (lists_word(allowed, *key)) continue;
    return site.fail(entry.first, std::format("unknown {} '{}'", kind, *key));
  }
  return {};
}

// An optional word-valued key: absent keeps `target`'s default; present must be a word
// `parse` recognises (model::parse_direction, model::parse_step_mode, ...: each a lookup in
// the enum's name table, core::parse_in), otherwise `requirement` ("dir must be in, out or
// inout") is the message.
// C++20 requires-clause: `parse` must turn a word into a std::optional<E>, the shape of
// every lookup here; a lookup of another shape fails at the call, not inside the body.
template <typename E, typename Parse>
  requires std::same_as<std::invoke_result_t<Parse&, std::string_view>, std::optional<E>>
CheckResult read_optional_word(const Site& site, const YAML::Node& parent, std::string_view key,
                               Parse parse, std::string_view requirement, E& target) {
  const auto value = parent[std::string{key}];
  if (!value) return {};
  if (auto checked = check_not_null(site, parent, key, key); !checked) return checked;
  const auto word = parse(scalar_text(value));
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
  const auto number = number_value(value);
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
  if (step["op"]) {
    return check_not_null(site, step, "op", "step op").and_then([&] {
      return read_op_name(site, step["op"], "step op");
    });
  }
  if (incoming_op) return std::string{*incoming_op};
  return site.fail(step, "then step needs an 'op'");
}

// `on` gates the flow (a cache's `miss` steps see only what it did not serve, ch. 1),
// `mode` decides whether latencies add (sequential) or overlap (parallel: the max counts),
// `factor` multiplies the flow (fan-out on write: one post -> 100 feed writes, ch. 11).
CheckResult read_step_attributes(const Site& site, const YAML::Node& yaml, model::Step& step) {
  if (auto read = read_optional_word(site, yaml, "on", model::parse_step_condition,
                                     "step condition must be always, miss or failure", step.on);
      !read) {
    return read;
  }
  if (auto read = read_optional_word(site, yaml, "mode", model::parse_step_mode,
                                     "step mode must be sequential or parallel", step.mode);
      !read) {
    return read;
  }
  if (auto read = read_optional_number(site, yaml, "factor", is_positive,
                                       "step factor must be a finite positive number", step.factor);
      !read) {
    return read;
  }
  if (step.factor <= max_step_factor) return {};
  return site.fail(yaml["factor"], "step factor must be at most 1e6");
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
  const auto label = std::format("op '{}'", *name);
  if (auto checked = check_entry_not_null(scope.site, entry, label, empty_list); !checked) {
    return checked;
  }
  if (!is_sequence(entry.second)) {
    return scope.site.fail(entry.second, std::format("op '{}' must be a step list", *name));
  }
  return read_step_list(scope, entry.second, *name).transform([&](std::vector<model::Step> steps) {
    node.ops.emplace(std::move(*name), std::move(steps));
  });
}

// A forwarding node's `ops`: incoming op name -> the steps it triggers.
CheckResult read_op_steps(const StepScope& scope, const YAML::Node& body, model::NodeSpec& node) {
  const auto ops = body["ops"];
  if (!ops) return {};
  if (auto checked = check_not_null(scope.site, body, "ops", "ops", empty_map); !checked) {
    return checked;
  }
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
  if (auto checked = check_not_null(scope.site, body, "then", "then", empty_list); !checked) {
    return checked;
  }
  if (!then.IsSequence()) return scope.site.fail(then, "then must be a list");
  return read_step_list(scope, then, std::nullopt).transform(store_in(node.then));
}

// ---- Client workload: the traffic a client offers -------------------------------------
//
// Models the user population of the book's back-of-the-envelope estimates (ch. 2): a
// request rate (DAU x requests per user / 86 400 s) and the mix of operations those
// requests are (the read/write ratio, e.g. 10:1 for the URL shortener, ch. 8). Phase 2a
// types the whole `rate:` block (spec §8): the level (`base`, or a `points` trace), the
// multiplicative terms `wave`, `ramp` and `spikes` (simsys.workload has the formulas), and
// the seeded `noise` process. The range rules below keep every term's multiplier finite
// and non-negative, so no rate the engine computes from them can be negative or infinite.

constexpr std::string_view rate_keys = "base wave spikes ramp points noise";
// The keys of each curve block; every one of them is required.
constexpr std::string_view wave_keys = "shape period amplitude peak_at";
constexpr std::string_view ramp_keys = "from to over";
constexpr std::string_view spike_keys = "at for factor";
constexpr std::string_view noise_keys = "sigma correlation";

// The tolerance of "the shares sum to 1": decimal shares are not exact in binary floating
// point (0.1 + 0.2 != 0.3), so the sum is compared within 1e-6.
constexpr double share_sum_tolerance = 1e-6;

using OpShare = std::pair<std::string, double>;
using OpShares = std::vector<OpShare>;

// A rate no larger than max_rate_per_second (see there); `label` names it ("rate.base",
// "rate.ramp.to", "rate.points[3]").
std::expected<Rate, ConfigError> check_rate_bounded(const Site& site, const YAML::Node& at,
                                                    std::string_view label, Rate rate) {
  if (rate.per_second <= max_rate_per_second) return rate;
  return site.fail(at, std::format("{} must be at most 1e15/s", label));
}

std::expected<Rate, ConfigError> read_bounded_rate(const Site& site, const YAML::Node& parent,
                                                   std::string_view key, std::string_view label) {
  return parse_key(site, parent, key, label, parse_rate).and_then([&](Rate rate) {
    return check_rate_bounded(site, parent[std::string{key}], label, rate);
  });
}

// The words of a space-separated list, in order.
std::vector<std::string> words_of(std::string_view words) {
  std::vector<std::string> parts;
  for (const auto piece : words | std::views::split(' '))
    parts.emplace_back(piece.begin(), piece.end());
  return parts;
}

// "a, b and c" from the words "a b c": the required keys of a curve block, as its
// "needs ..." message lists them.
std::string spoken_list(std::string_view words) {
  const auto parts = words_of(words);
  std::string text = parts.front();
  for (std::size_t i = 1; i < parts.size(); ++i) {
    text += i + 1 == parts.size() ? " and " : ", ";
    text += parts[i];
  }
  return text;
}

// A curve block (`rate.wave`, `rate.spikes[2]`, ...) is a map that has exactly `keys`: none
// unknown ("unknown rate.wave key 'colour'"), none missing ("rate.wave needs shape, period,
// amplitude and peak_at"). Every key is required because no default would be a safe guess
// (how long is the period of a wave written without one?).
CheckResult check_curve_block(const Site& site, const YAML::Node& block, std::string_view label,
                              std::string_view keys) {
  if (!block.IsMap()) return site.fail(block, std::format("{} must be a map", label));
  if (auto checked = check_known_keys(site, block, keys, std::format("{} key", label)); !checked) {
    return checked;
  }
  const auto required = words_of(keys);
  const bool complete =
      std::ranges::all_of(required, [&](const std::string& key) { return block[key].IsDefined(); });
  if (complete) return {};
  return site.fail(block, std::format("{} needs {}", label, spoken_list(keys)));
}

// YAML's spellings of not-a-number and infinity (YAML 1.2 core schema). They are numbers in
// YAML, just not finite ones, so a key answers them with its range message, not "must be a
// number".
constexpr std::array<std::string_view, 12> yaml_special_floats{".nan",  ".NaN",  ".NAN",  ".inf",
                                                               ".Inf",  ".INF",  "+.inf", "+.Inf",
                                                               "+.INF", "-.inf", "-.Inf", "-.INF"};

// Is `node` written as a number at all, finite or not, in range or not? A plain scalar
// that reads whole as a decimal (1e309 included, which is out of range, not "no number"),
// or one of YAML's NaN/infinity spellings. A quoted "0.5", a word, a list or a map is not.
bool is_written_as_number(const YAML::Node& node) {
  const auto text = number_text(node);
  if (text.empty()) return false;
  const auto parsed = parse_whole<double>(text);
  return parsed || parsed.error() == NumberFault::out_of_range ||
         std::ranges::contains(yaml_special_floats, std::string_view{text});
}

// `block[key]` as a number that passes `in_range`. Otherwise the message says what is
// wrong: "<label>.<key> must not be empty" for a bare key, "must be a number" for text or
// a quoted number (a typed language rejects "0.5" where a double is expected), and
// "must be <requirement>" for a number out of range (NaN and infinity included).
std::expected<double, ConfigError> read_curve_number(const Site& site, const YAML::Node& block,
                                                     std::string_view label, std::string_view key,
                                                     bool (*in_range)(double),
                                                     std::string_view requirement) {
  const auto full = std::format("{}.{}", label, key);
  if (auto checked = check_not_null(site, block, key, full); !checked) {
    return std::unexpected(checked.error());
  }
  const auto value = block[std::string{key}];
  if (!is_written_as_number(value))
    return site.fail(value, std::format("{} must be a number", full));
  const auto number = number_value(value);
  if (number && in_range(*number)) return *number;
  return site.fail(value, std::format("{} must be {}", full, requirement));
}

// `block[key]` as a duration above zero: a period, a spike's length, a ramp's length, a
// correlation time. Zero would divide by zero in the term's formula or make it vanish.
std::expected<SimTime, ConfigError> read_positive_duration(const Site& site,
                                                           const YAML::Node& block,
                                                           std::string_view label,
                                                           std::string_view key) {
  const auto full = std::format("{}.{}", label, key);
  return parse_key(site, block, key, full, parse_duration)
      .and_then([&](SimTime t) -> std::expected<SimTime, ConfigError> {
        if (t.count() > 0) return t;
        return site.fail(block[std::string{key}], std::format("{} must be positive", full));
      });
}

// `block[key]` as a point in simulated time: a clock time ("20:00") or a duration from the
// start ("5m"), both offsets from t = 0, which is midnight (phase 2a refinement 1). An
// unquoted 20:00 is fine: yaml-cpp keeps every scalar's text, and YAML 1.2 has no base-60
// numbers (YAML 1.1 read 20:00 as the integer 1200).
std::expected<SimTime, ConfigError> read_time_point(const Site& site, const YAML::Node& block,
                                                    std::string_view label, std::string_view key) {
  return parse_key(site, block, key, std::format("{}.{}", label, key), parse_time_point);
}

std::expected<workload::WaveShape, ConfigError> read_wave_shape(const Site& site,
                                                                const YAML::Node& wave) {
  if (const auto shape = workload::parse_wave_shape(scalar_text(wave["shape"]))) return *shape;
  return site.fail(shape_site(wave, "shape"), "rate.wave.shape must be sine, square or diurnal");
}

// The amplitude stays in [0, 1] so the wave multiplier 1 + A x (shape - 1) never drops
// below zero (see workload::Wave).
std::expected<workload::Wave, ConfigError> read_wave(const Site& site, const YAML::Node& wave) {
  constexpr std::string_view label = "rate.wave";
  if (auto checked = check_curve_block(site, wave, label, wave_keys); !checked) {
    return std::unexpected(checked.error());
  }
  workload::Wave read;
  if (auto shape = read_wave_shape(site, wave).transform(store_in(read.shape)); !shape) {
    return std::unexpected(shape.error());
  }
  if (auto period =
          read_positive_duration(site, wave, label, "period").transform(store_in(read.period));
      !period) {
    return std::unexpected(period.error());
  }
  if (auto amplitude =
          read_curve_number(site, wave, label, "amplitude", is_ratio, "between 0 and 1")
              .transform(store_in(read.amplitude));
      !amplitude) {
    return std::unexpected(amplitude.error());
  }
  return read_time_point(site, wave, label, "peak_at").transform([&](SimTime peak_at) {
    read.peak_at = peak_at;
    return read;
  });
}

std::expected<workload::Ramp, ConfigError> read_ramp(const Site& site, const YAML::Node& ramp) {
  constexpr std::string_view label = "rate.ramp";
  if (auto checked = check_curve_block(site, ramp, label, ramp_keys); !checked) {
    return std::unexpected(checked.error());
  }
  workload::Ramp read;
  if (auto from =
          read_bounded_rate(site, ramp, "from", "rate.ramp.from").transform(store_in(read.from));
      !from) {
    return std::unexpected(from.error());
  }
  if (auto to = read_bounded_rate(site, ramp, "to", "rate.ramp.to").transform(store_in(read.to));
      !to) {
    return std::unexpected(to.error());
  }
  return read_positive_duration(site, ramp, label, "over").transform([&](SimTime over) {
    read.over = over;
    return read;
  });
}

// A spike's factor is above zero (a zero factor silences the client; leave the spike out)
// and at most max_step_factor, the bound of a step's fan-out factor, for the same reason:
// rate x factor must stay far from overflowing a double. Overlapping spikes multiply, so
// read_spikes bounds their product too (check_spike_product).
std::expected<workload::Spike, ConfigError> read_spike(const Site& site, const YAML::Node& spike,
                                                       std::string_view label) {
  if (auto checked = check_curve_block(site, spike, label, spike_keys); !checked) {
    return std::unexpected(checked.error());
  }
  workload::Spike read;
  if (auto at = read_time_point(site, spike, label, "at").transform(store_in(read.at)); !at) {
    return std::unexpected(at.error());
  }
  if (auto length =
          read_positive_duration(site, spike, label, "for").transform(store_in(read.duration));
      !length) {
    return std::unexpected(length.error());
  }
  if (auto factor = read_curve_number(site, spike, label, "factor", is_positive, "positive")
                        .transform(store_in(read.factor));
      !factor) {
    return std::unexpected(factor.error());
  }
  if (read.factor <= max_step_factor) return read;
  return site.fail(spike["factor"], std::format("{}.factor must be at most 1e6", label));
}

// Is `spike` active at `t`, i.e. at <= t < at + duration? The same rule as the curve's
// (workload::RateCurve::rate_at), written as a difference so it cannot overflow: both
// times are offsets from t = 0, never negative.
bool spike_active_at(const workload::Spike& spike, SimTime t) {
  return spike.at.count() <= t.count() && t.count() - spike.at.count() < spike.duration.count();
}

// The product of the factors of every spike active at `t`: overlapping spikes compound.
double spike_product_at(const std::vector<workload::Spike>& spikes, SimTime t) {
  double product = 1.0;
  for (const auto& spike : spikes) {
    if (spike_active_at(spike, t)) product *= spike.factor;
  }
  return product;
}

// Overlapping spikes compound, so bounding each factor by max_step_factor is not enough:
// a thousand spikes of 1e6 at the same moment multiply to infinity. The bound applies to
// the product at every moment. The product is a step function that rises only where a
// spike starts (it falls where one ends), so its maximum is reached at some spike's `at`,
// and checking every start finds it: n starts x n spikes, O(n^2), fine for the handful of
// spikes a scenario has.
CheckResult check_spike_product(const Site& site, const YAML::Node& list,
                                const std::vector<workload::Spike>& spikes) {
  const bool bounded = std::ranges::all_of(spikes, [&](const workload::Spike& spike) {
    return spike_product_at(spikes, spike.at) <= max_step_factor;
  });
  if (bounded) return {};
  return site.fail(list, "rate.spikes: overlapping factors multiply to more than 1e6");
}

// Spikes are numbered from 1 in messages, as an author counts them ("rate.spikes[2]").
std::expected<std::vector<workload::Spike>, ConfigError> read_spikes(const Site& site,
                                                                     const YAML::Node& rate) {
  std::vector<workload::Spike> spikes;
  const auto list = rate["spikes"];
  if (!list) return spikes;
  if (auto checked = check_not_null(site, rate, "spikes", "rate.spikes", empty_list); !checked) {
    return std::unexpected(checked.error());
  }
  if (!list.IsSequence()) return site.fail(list, "rate.spikes must be a list");
  for (std::size_t i = 0; i < list.size(); ++i) {
    auto spike = read_spike(site, list[i], std::format("rate.spikes[{}]", i + 1));
    if (!spike) return std::unexpected(spike.error());
    spikes.push_back(*spike);
  }
  return check_spike_product(site, list, spikes).transform([&] { return std::move(spikes); });
}

// One `[time, rate]` entry of a trace; `label` ("rate.points[2]") prefixes a unit error.
std::expected<std::pair<SimTime, Rate>, ConfigError> read_point(const Site& site,
                                                                const YAML::Node& entry,
                                                                std::string_view label) {
  if (!entry.IsSequence() || entry.size() != 2) {
    return site.fail(entry, "rate.points entries are [time, rate] pairs");
  }
  auto time = parse_scalar(site, entry[0], label, parse_time_point);
  if (!time) return std::unexpected(time.error());
  return parse_scalar(site, entry[1], label, parse_rate)
      .and_then([&](Rate rate) { return check_rate_bounded(site, entry[1], label, rate); })
      .transform([&](Rate rate) { return std::pair{*time, rate}; });
}

// Sorted by time, not necessarily strictly: two entries at the same time are a step change
// (the interpolation takes the later one from that instant on).
CheckResult check_points_sorted(const Site& site, const YAML::Node& list,
                                const workload::Points& points) {
  // C++20 std::ranges::is_sorted with a projection: compares the entries by their time
  // (the nanosecond count) without a hand-written comparator over pairs.
  const auto time_ns = [](const std::pair<SimTime, Rate>& point) { return point.first.count(); };
  if (std::ranges::is_sorted(points.at, {}, time_ns)) return {};
  return site.fail(list, "rate.points must be sorted by time");
}

// A recorded or drawn trace that replaces `base`. Its rates are non-negative because
// parse_rate accepts no sign.
std::expected<workload::Points, ConfigError> read_points(const Site& site, const YAML::Node& rate) {
  if (auto checked = check_not_null(site, rate, "points", "rate.points"); !checked) {
    return std::unexpected(checked.error());
  }
  const auto list = rate["points"];
  if (!list.IsSequence()) return site.fail(list, "rate.points must be a list");
  if (list.size() == 0) return site.fail(list, "rate.points must have at least one entry");
  workload::Points points;
  for (std::size_t i = 0; i < list.size(); ++i) {
    auto point = read_point(site, list[i], std::format("rate.points[{}]", i + 1));
    if (!point) return std::unexpected(point.error());
    points.at.push_back(*point);
  }
  return check_points_sorted(site, list, points).transform([&] { return std::move(points); });
}

// The level the terms multiply: exactly one of `base` and `points`.
CheckResult read_level(const Site& site, const YAML::Node& rate, workload::RateCurve& curve) {
  const bool has_base = rate["base"].IsDefined();
  const bool has_points = rate["points"].IsDefined();
  if (has_base && has_points) return site.fail(rate, "rate.base and rate.points are alternatives");
  if (has_points) return read_points(site, rate).transform(store_in(curve.points));
  if (!has_base) return site.fail(rate, "client needs rate.base or rate.points");
  return read_bounded_rate(site, rate, "base", "rate.base").transform(store_in(curve.base));
}

CheckResult read_wave_term(const Site& site, const YAML::Node& rate, workload::RateCurve& curve) {
  if (!rate["wave"]) return {};
  if (auto checked = check_not_null(site, rate, "wave", "rate.wave"); !checked) return checked;
  return read_wave(site, rate["wave"]).transform(store_in(curve.wave));
}

// A ramp's rate is the client's level while it is in force (workload::RateCurve: the spec's
// multiplier ramp/base times base, applied without the division). It needs no base for the
// arithmetic, but the rule "a ramp needs a positive rate.base" is kept on purpose: the file
// then always states the client's nominal rate (explain prints it; deleting the ramp leaves
// a complete curve), and next to `points`, which are a level too, a ramp has no meaning.
CheckResult read_ramp_term(const Site& site, const YAML::Node& rate, workload::RateCurve& curve) {
  if (!rate["ramp"]) return {};
  if (auto checked = check_not_null(site, rate, "ramp", "rate.ramp"); !checked) return checked;
  // One message for both. workload's rate_at throws for a ramp next to points (a
  // programmer error), so no scenario may reach it.
  if (curve.points || curve.base.per_second <= 0.0) {
    return site.fail(rate["ramp"], "rate.ramp needs rate.base");
  }
  return read_ramp(site, rate["ramp"]).transform(store_in(curve.ramp));
}

// The deterministic curve: the level, then the multiplicative terms.
std::expected<workload::RateCurve, ConfigError> read_rate_curve(const Site& site,
                                                                const YAML::Node& rate) {
  workload::RateCurve curve;
  if (auto read = read_level(site, rate, curve); !read) return std::unexpected(read.error());
  if (auto read = read_wave_term(site, rate, curve); !read) return std::unexpected(read.error());
  if (auto read = read_ramp_term(site, rate, curve); !read) return std::unexpected(read.error());
  return read_spikes(site, rate).transform([&](std::vector<workload::Spike> spikes) {
    curve.spikes = std::move(spikes);
    return std::move(curve);
  });
}

// The largest noise `sigma` a scenario may ask for. Chosen, not from the book: the noise
// multiplier is max(0, 1 + x) with x ~ N(0, sigma^2), so at sigma = 10 the client is already
// silent almost half the time (P(x < -1) = 46 %) and bursts to around 10x its curve, at
// times 20x, the rest: no longer "noise around a curve", and no real workload needs more. The cap
// also keeps a noisy rate inside the engine's arithmetic (max_rate_per_second x a multiplier of a
// few tens is still finite) and turns a typo such as `sigma: 15` for 15 % into an error instead of
// a strange run.
inline constexpr double max_noise_sigma = 10.0;

// `sigma` as read: non-negative (0 means no noise) and at most max_noise_sigma.
CheckResult check_sigma_capped(const Site& site, const YAML::Node& noise, std::string_view label,
                               double sigma) {
  if (sigma <= max_noise_sigma) return {};
  return site.fail(shape_site(noise, "sigma"),
                   std::format("{}.sigma must be at most {}", label, max_noise_sigma));
}

// The Ornstein-Uhlenbeck noise of spec §9.2: `sigma` is its stationary standard deviation
// (0 is allowed: no noise), `correlation` (tau) roughly how long a burst lasts.
std::expected<workload::Noise, ConfigError> read_noise(const Site& site, const YAML::Node& noise) {
  constexpr std::string_view label = "rate.noise";
  if (auto checked = check_curve_block(site, noise, label, noise_keys); !checked) {
    return std::unexpected(checked.error());
  }
  workload::Noise read;
  if (auto sigma = read_curve_number(site, noise, label, "sigma", is_non_negative, "non-negative")
                       .transform(store_in(read.sigma));
      !sigma) {
    return std::unexpected(sigma.error());
  }
  if (auto capped = check_sigma_capped(site, noise, label, read.sigma); !capped) {
    return std::unexpected(capped.error());
  }
  return read_positive_duration(site, noise, label, "correlation").transform([&](SimTime tau) {
    read.correlation = tau;
    return read;
  });
}

CheckResult read_noise_term(const Site& site, const YAML::Node& rate,
                            std::optional<workload::Noise>& noise) {
  if (!rate["noise"]) return {};
  if (auto checked = check_not_null(site, rate, "noise", "rate.noise"); !checked) return checked;
  return read_noise(site, rate["noise"]).transform(store_in(noise));
}

// The `rate:` block: a map of known keys, read into the curve and the noise.
CheckResult read_rate(const Site& site, const YAML::Node& body, model::Workload& workload) {
  if (auto checked = check_not_null(site, body, "rate", "rate"); !checked) return checked;
  const auto rate = body["rate"];
  if (!rate) return site.fail(body, "client needs rate.base or rate.points");
  // `rate: 100/s` was the phase-1 spelling of a constant rate; say how to write it now.
  if (rate.IsScalar()) {
    return site.fail(rate,
                     std::format("rate must be a map (write rate: {{base: {}}})", rate.Scalar()));
  }
  if (!rate.IsMap()) return site.fail(rate, "rate must be a map");
  if (auto checked = check_known_keys(site, rate, rate_keys, "rate key"); !checked) return checked;
  if (auto read = read_rate_curve(site, rate).transform(store_in(workload.curve)); !read) {
    return read;
  }
  return read_noise_term(site, rate, workload.noise);
}

// A share is the probability that a request is this op: above 0 (a zero share is an op
// that never happens; leave it out) and at most 1.
std::expected<OpShare, ConfigError> read_op_share(const Site& site, const YamlElement& entry) {
  auto name = read_op_name(site, entry.first, "op name");
  if (!name) return std::unexpected(name.error());
  const auto label = std::format("op share '{}'", *name);
  if (auto checked = check_entry_not_null(site, entry, label); !checked) {
    return std::unexpected(checked.error());
  }
  const auto share = number_value(entry.second);
  if (share && is_unit_fraction(*share)) return OpShare{std::move(*name), *share};
  return site.fail(entry.second, std::format("op share '{}' must be in (0, 1]", *name));
}

// `value` with 15 significant digits: every share sum outside the 1e-6 tolerance differs
// from 1 within the first 7, while the binary rounding noise of the addition (0.5 +
// 0.500002 is 1.0000019999999998) sits in the 16th and 17th and is not shown.
// std::to_chars, not std::format("{:.15g}"): with GCC 14.2, a precision in a format spec
// throws std::format_error at run time in a module unit that imports a module whose global
// module fragment includes <chrono>, as simsys.core's does: <chrono> pulls in <format>
// (verified 2026-10-28 with a three-file probe; the same GCC 14.2 module defect behind the
// lean-interface rule of CLAUDE.md section 3). Plain "{}" is unaffected.
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
  // No `{}` hint: a client needs at least one op.
  if (auto checked = check_not_null(site, body, "ops", "ops"); !checked) {
    return std::unexpected(checked.error());
  }
  if (!ops.IsMap()) return site.fail(ops, "ops must be a map");
  if (ops.size() == 0) return site.fail(ops, "client needs at least one op");
  auto shares =
      collect_each(ops, [&](const YamlElement& entry) { return read_op_share(site, entry); });
  if (!shares) return shares;
  std::ranges::sort(*shares);  // pairs compare by their first member (the op) first
  return check_shares_sum_to_one(site, ops, *shares).transform([&] { return std::move(*shares); });
}

// A count as the scenarios write cache sizes and key spaces: a positive integer with an optional SI
// suffix k, M or G (x 1e3, 1e6, 1e9; the same letters a rate uses, without "/s"): 500, 10k, 20M,
// 10G. Nothing for anything else (a fraction, a sign, an overflow of 64 bits). A local
// helper rather than parse_rate(text + "/s"): a rate may be fractional ("1.5k/s"), a count
// of entries may not.
std::optional<std::int64_t> parse_count(std::string_view text) {
  constexpr std::array suffixes{std::pair{'k', std::int64_t{1'000}},
                                std::pair{'M', std::int64_t{1'000'000}},
                                std::pair{'G', std::int64_t{1'000'000'000}}};
  std::int64_t scale = 1;
  for (const auto& [letter, size] : suffixes) {
    if (!text.empty() && text.back() == letter) {
      scale = size;
      text.remove_suffix(1);
      break;
    }
  }
  const auto count = parse_whole<std::int64_t>(text);
  if (!count || *count <= 0 || *count > std::numeric_limits<std::int64_t>::max() / scale) {
    return std::nullopt;
  }
  return *count * scale;
}

// ---- Client keys (plan 2b refinement 1) ----

constexpr std::string_view client_keys_keys = "zipf space uniform";

// `keys.space: 100M`, `keys.uniform: 10M`: a key-space size, a count like a cache's `keys`.
std::expected<double, ConfigError> read_key_space(const Site& site, const YAML::Node& value,
                                                  std::string_view key) {
  const auto count = parse_count(number_text(value));
  if (!count) return site.fail(value, std::format("keys.{} must be a positive count", key));
  return static_cast<double>(*count);
}

// `keys: {zipf: 1.0, space: 100M}`: Zipf with exponent s >= 0 over N keys (s = 0 is uniform,
// and stored as such).
std::expected<model::KeySpec, ConfigError> read_zipf_keys(const Site& site,
                                                          const YAML::Node& keys) {
  if (!keys["zipf"] || !keys["space"]) {
    return site.fail(keys, "keys needs zipf and space, or uniform");
  }
  // number_value already refuses .inf/.nan and words (parse_yaml_double): one message for all.
  const auto exponent = number_value(keys["zipf"]);
  if (!exponent || *exponent < 0.0)
    return site.fail(keys["zipf"], "keys.zipf must be a finite non-negative number");
  // Canonical form (model::canonical): `zipf: 0` is stored as uniform.
  return read_key_space(site, keys["space"], "space").transform([&](double space) {
    return model::canonical(model::KeySpec{.zipf = *exponent, .space = space});
  });
}

// A client's `keys`: which keys it asks for, and how popular each is (model::KeySpec). Read
// only for the `che` caches downstream (graph's agreed_keys), but checked on every client: a
// misspelt block would otherwise surface only when a che cache is added.
CheckResult read_client_keys(const Site& site, const YAML::Node& body, model::Workload& workload) {
  const auto keys = body["keys"];
  if (!keys) return {};
  if (!keys.IsMap()) return site.fail(keys, "keys must be a map");
  if (auto checked = check_known_keys(site, keys, client_keys_keys, "keys entry"); !checked) {
    return checked;
  }
  const bool zipf_form = keys["zipf"] || keys["space"];
  if (zipf_form && keys["uniform"]) {
    return site.fail(keys, "keys takes zipf+space or uniform, not both");
  }
  if (!keys["uniform"]) return read_zipf_keys(site, keys).transform(store_in(workload.keys));
  return read_key_space(site, keys["uniform"], "uniform").transform([&](double space) {
    workload.keys = model::KeySpec{.zipf = std::nullopt, .space = space};
  });
}

std::expected<model::Workload, ConfigError> read_workload(const Site& site,
                                                          const YAML::Node& body) {
  model::Workload workload;
  if (auto read = read_rate(site, body, workload); !read) return std::unexpected(read.error());
  if (auto read = read_op_shares(site, body).transform(store_in(workload.op_shares)); !read) {
    return std::unexpected(read.error());
  }
  if (auto read = read_client_keys(site, body, workload); !read) {
    return std::unexpected(read.error());
  }
  if (auto read = read_optional_duration(site, body, "timeout", workload.timeout); !read) {
    return std::unexpected(read.error());
  }
  return workload;
}

// ---- Rate limiter keys (plan 2a refinement 6) -------------------------------------------
//
// Read in the second pass, where every reader above is declared and every node's type is
// known (`backend` must name a cache). Order matters: the default burst is limit x window.

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

// ---- Id generator keys (plan 2b refinement 6; book ch. 7) ------------------------------

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

// ---- Cache keys (plan 2a refinement 7) -------------------------------------------------
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
// graph refuses a write op that reaches it) instead of caching them as reads.
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

// ---- SQL database keys (plan 2b refinement 3) ----

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

// ---- CDN keys (plan 2b refinement 4) ----
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
    if (auto read = read_id_generator_keys(site, entry.second, *node); !read) return read;
    ++node;
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

// Until both ends are read the link has no name, so its site names it by its position in
// the list ("link 1"), counted from 1 like line numbers.
Site link_index_site(const Site& site, std::size_t index) {
  return site.within(std::format("link {}", index));
}

CheckResult check_link_end(const Site& site, const YAML::Node& link, std::string_view end,
                           const NodeIndex& ids) {
  if (!link[std::string{end}]) return site.fail(link, std::format("needs '{}'", end));
  return check_reference(site, link, end, "", ids);
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

// The filter is a set of ops: one listed twice is a typo or a leftover edit. Runs after
// read_link_op has checked that every element is a scalar.
CheckResult check_link_ops_unique(const Site& site, const YAML::Node& list) {
  std::set<std::string_view> seen;  // views into the document's own text, as in the key check
  for (const auto& op : list) {
    if (seen.insert(op.Scalar()).second) continue;
    return site.fail(op, std::format("duplicate op '{}'", op.Scalar()));
  }
  return {};
}

// The ops a link carries; absent means all of them. An empty list is rejected: it would
// load as "all", the opposite of what it says.
CheckResult read_link_ops(const Site& site, const YAML::Node& link, std::vector<std::string>& ops) {
  const auto list = link["ops"];
  if (!list) return {};
  if (auto checked = check_not_null(site, link, "ops", "ops"); !checked) return checked;
  if (!list.IsSequence()) return site.fail(list, "ops must be a list of op names");
  if (list.size() == 0) {
    return site.fail(list, "ops must name at least one op (omit it to carry all)");
  }
  return collect_each(list, [&](const YAML::Node& op) { return read_link_op(site, op); })
      .and_then([&](std::vector<std::string> names) {
        return check_link_ops_unique(site, list).transform([&] { ops = std::move(names); });
      });
}

// The largest link weight. Weights matter only relative to each other: the graph splits an
// op over its links as share_i = weight_i / (sum of the weights), so 1e9 leaves any real
// ratio expressible (a billion to one) while every sum of weights stays finite. Without a
// bound, 1e308 + 1e308 overflows to inf and inf / inf is NaN, a share that poisons every
// rate downstream; 1e309, rounded to infinity, does the same directly.
inline constexpr double max_link_weight = 1e9;

// `weight` is the link's share when several links carry the same op (a weighted load
// balancer, ch. 1); 0 is allowed: a drained backend that receives nothing.
CheckResult read_link_weight(const Site& site, const YAML::Node& link, double& weight) {
  if (auto read = read_optional_number(site, link, "weight", is_zero_or_more,
                                       "weight must be a finite non-negative number", weight);
      !read) {
    return read;
  }
  if (weight <= max_link_weight) return {};
  return site.fail(link["weight"], "weight must be at most 1e9");
}

CheckResult read_link_attributes(const Site& site, const YAML::Node& link, model::LinkSpec& spec) {
  if (auto checked = check_known_keys(site, link, link_keys, "key"); !checked) return checked;
  if (auto read = read_optional_word(site, link, "dir", model::parse_direction,
                                     "dir must be in, out or inout", spec.dir);
      !read) {
    return read;
  }
  if (auto read = read_link_ops(site, link, spec.ops); !read) return read;
  if (auto read = read_optional_duration(site, link, "latency", spec.latency); !read) return read;
  return read_link_weight(site, link, spec.weight);
}

// `site` names the link by its index (link_index_site) until its ends are known.
std::expected<model::LinkSpec, ConfigError> read_link(const Site& site, const YAML::Node& link,
                                                      const NodeIndex& ids) {
  if (!link.IsMap()) return site.fail(link, "must be a map");
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

// A database answers every query itself and calls nobody (behaviours::SqlDbBehaviour), so a
// link that carries demand out of one (written from it, or from the other end with `dir: in`)
// would carry nothing: refused at the database's name.
CheckResult check_link_sender(const Site& site, const YAML::Node& link, const model::LinkSpec& spec,
                              const Declarations& declared) {
  const auto& sender = declared.nodes[declared.ids.find(demand_pair(spec).first)->second];
  if (sender.type != model::NodeType::sql_db) return {};
  return node_site(site, sender.id).fail(link, "a database calls nobody");
}

// One link per pair and demand direction: the graph routes an op over THE link between two
// nodes, so a second one (for other ops, or a second weight) would be ignored or ambiguous.
// Several ops go in one link's `ops`, a split goes in weights across different targets.
std::expected<model::LinkSpec, ConfigError> read_unique_link(const Site& site,
                                                             const YAML::Node& link,
                                                             const Declarations& declared,
                                                             std::set<DemandPair>& seen) {
  auto spec = read_link(site, link, declared.ids);
  if (!spec) return spec;
  if (auto checked = check_link_sender(site, link, *spec, declared); !checked) {
    return std::unexpected(checked.error());
  }
  if (seen.insert(demand_pair(*spec)).second) return spec;
  return link_site(site, spec->from, spec->to)
      .fail(link, "duplicate link (put several ops or a weight on one link)");
}

std::expected<std::vector<model::LinkSpec>, ConfigError> collect_links(
    const Site& site, const YAML::Node& links, const Declarations& declared) {
  // A bare `links:` (null) was reported at its key by check_sections_not_null.
  if (!links.IsSequence()) return site.fail(links, "links must be a list");
  std::set<DemandPair> seen;
  std::size_t index = 0;
  return collect_each(links, [&](const YAML::Node& link) {
    return read_unique_link(link_index_site(site, ++index), link, declared, seen);
  });
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
  if (auto checked = check_not_null(site, entry, "metric", "metric"); !checked) {
    return std::unexpected(checked.error());
  }
  const auto name = scalar_text(value);
  if (const auto metric = model::parse_slo_metric(name)) return *metric;
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
  if (auto checked = check_not_null(site, entry, "op", "op name"); !checked) return checked;
  return read_op_name(site, entry["op"], "op name").transform(store_in(op));
}

// The variant's alternative follows the metric: a SimTime for the time metrics, a double
// for the ratios, so "0.01" can never be read as 0.01 seconds.
std::expected<SloMaximum, ConfigError> read_slo_max(const Site& site, const YAML::Node& entry,
                                                    model::SloMetric metric) {
  const auto name = model::to_string(metric);
  const auto value = entry["max"];
  if (!value) return site.fail(entry, std::format("{} needs a max", name));
  if (auto checked = check_not_null(site, entry, "max", "max"); !checked) {
    return std::unexpected(checked.error());
  }
  if (is_time_metric(metric)) {
    if (const auto time = parse_duration(scalar_text(value))) return SloMaximum{*time};
    return site.fail(value, std::format("{} max must be a duration", name));
  }
  const auto number = number_value(value);
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
  const auto metric = model::to_string(slo.metric);
  if (!slo.node) return site.fail(entry, std::format("{} needs a node", metric));
  const auto& node = declared.nodes[declared.ids.find(*slo.node)->second];
  if (node.type != model::NodeType::client) {
    return site.fail(entry["node"], std::format("{} needs a client node, '{}' is a {}", metric,
                                                node.id, model::to_string(node.type)));
  }
  if (!slo.op || offers_op(node, *slo.op)) return {};
  return site.fail(entry["op"], std::format("client '{}' has no op '{}'", node.id, *slo.op));
}

// Utilization is a server's busy fraction: it needs a node to name, and a client, a pure
// source with no service rate, is never busy, so the SLO could never fail on one.
CheckResult check_utilization_target(const Site& site, const YAML::Node& entry,
                                     const model::SloSpec& slo, const Declarations& declared) {
  if (slo.metric != model::SloMetric::utilization) return {};
  if (!slo.node) return site.fail(entry, "utilization needs a node");
  const auto& node = declared.nodes[declared.ids.find(*slo.node)->second];
  if (node.type != model::NodeType::client) return {};
  return site.fail(entry["node"],
                   std::format("utilization needs a server node, '{}' is a client", node.id));
}

// Only latency is measured per op (a client's response time for each op it sends); every
// other metric is node-wide, so an `op` on it would be silently ignored.
CheckResult check_op_is_for_latency(const Site& site, const YAML::Node& entry,
                                    const model::SloSpec& slo) {
  if (!slo.op || is_latency_percentile(slo.metric)) return {};
  return site.fail(entry["op"], "op is only valid for latency metrics");
}

// What each metric's `node` and `op` may be, checked once the whole entry is read.
CheckResult check_slo_shape(const Site& site, const YAML::Node& entry, const model::SloSpec& slo,
                            const Declarations& declared) {
  if (auto checked = check_latency_target(site, entry, slo, declared); !checked) return checked;
  if (auto checked = check_utilization_target(site, entry, slo, declared); !checked) {
    return checked;
  }
  return check_op_is_for_latency(site, entry, slo);
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
  return check_slo_shape(site, entry, slo, declared).transform([&] { return std::move(slo); });
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
  auto links = collect_links(site, root["links"], *declared);
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
