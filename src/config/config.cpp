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
#include <cstdlib>
#include <expected>
#include <format>
#include <functional>
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
  if (auto checked = check_not_null(site, body, "rate", "rate"); !checked) {
    return std::unexpected(checked.error());
  }
  if (!rate || !rate.IsMap() || !rate["base"]) {
    return site.fail(rate ? rate : body, "client needs rate.base");
  }
  if (auto checked = check_known_keys(site, rate, rate_keys, "rate key"); !checked) {
    return std::unexpected(checked.error());
  }
  return scalar_value(site, rate, "base", "rate.base")
      .and_then([&](const std::string& text) {
        return parse_rate(text).transform_error([&](const ParseError& error) {
          return unit_error(site, rate["base"], "rate.base", error);
        });
      })
      .and_then([&](Rate base) -> std::expected<Rate, ConfigError> {
        if (base.per_second <= max_rate_per_second) return base;
        return site.fail(rate["base"], "rate.base must be at most 1e15/s");
      });
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
  if (auto read = read_optional_word(site, link, "dir", directions, "dir must be in, out or inout",
                                     spec.dir);
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
  // A bare `links:` (null) was reported at its key by check_sections_not_null.
  if (!links.IsSequence()) return site.fail(links, "links must be a list");
  std::set<DemandPair> seen;
  std::size_t index = 0;
  return collect_each(links, [&](const YAML::Node& link) {
    return read_unique_link(link_index_site(site, ++index), link, ids, seen);
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
  if (auto checked = check_not_null(site, entry, "op", "op name"); !checked) return checked;
  return read_op_name(site, entry["op"], "op name").transform(store_in(op));
}

// The variant's alternative follows the metric: a SimTime for the time metrics, a double
// for the ratios, so "0.01" can never be read as 0.01 seconds.
std::expected<SloMaximum, ConfigError> read_slo_max(const Site& site, const YAML::Node& entry,
                                                    model::SloMetric metric) {
  const auto name = name_of(slo_metrics, metric);
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
