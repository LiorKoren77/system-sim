// Module `simsys.config`, the implementation units' shared part: what every reader of the
// loader uses. The vocabulary (CheckResult, Site, NodeIndex, Declarations, StepScope), the
// schema's key tables and range rules, the common readers (scalars, numbers, durations, op
// names, node references, sizes) and the declarations of each concern's entry points, so the
// pipeline (config.cpp) and the units that read across concerns call one signature defined in
// one unit. detail.cpp defines the functions declared here; templates and constants are
// defined here.
//
// A header, not a module partition. Each implementation unit includes it after its imports, in
// the module purview, so its declarations are attached to simsys.config like the unit's own and
// can name ConfigError, which the primary interface declares. It includes nothing itself: a
// standard or yaml-cpp header included in a module purview would attach the library's
// declarations to simsys.config. Each including unit's global module fragment provides the
// headers (yaml-cpp, <expected>, <map>, ...) and its imports the modules (simsys.core,
// simsys.fluid, simsys.model) used here.
//
// Why no partition (`module simsys.config:shared;`, tried first; GCC 14.2, 2026-10-29):
// - a partition that imports the primary interface, for ConfigError, compiles, but every unit
//   of simsys.config that imports it then fails ("cannot import module in its own purview");
// - with ConfigError moved into an interface partition, a partition whose global module
//   fragment includes yaml-cpp and which imports simsys.core makes every importer fail with
//   the first defect of CLAUDE.md §3 ("Bad file data", "failed to load pendings for
//   std::formatter"): yaml-cpp includes <memory>, <sstream> and <ostream>, each of which alone
//   triggers it (in C++23 mode they bring in <format>). The shared templates read YAML::Node,
//   so the partition cannot do without yaml-cpp. (The first refusal is GCC's alone: the
//   standard allows a partition to import its primary interface, and Clang 18 accepts it.)
//
// What this costs, said plainly: a deliberate departure from the standard, forced by GCC 14.2.
// The structs (Site, Declarations, OwnKeys, ...), the `inline constexpr` tables and the
// templates below are definitions attached to simsys.config, and each of the ten units that
// includes this header defines them again. [basic.def.odr] allows such repeated definitions
// only for entities attached to the global module (the reason headers work); for an entity
// attached to a named module it requires one definition, in one unit, and repeating it is
// ill-formed, no diagnostic required. GCC 14.2 accepts it: it emits these definitions in vague
// linkage, as it would a header's, and the linker keeps one copy. The conforming design is a
// module implementation partition (`module simsys.config:shared;`, imported by each unit with
// `import :shared;`), as simsys.report does with shared.cppm; it replaces this header once GCC
// builds it. Until then, the header grows no definitions it does not need: a helper that only
// one unit uses stays in that unit, in an unnamed namespace.
#pragma once

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

inline constexpr std::array<std::string_view, 5> required_top_level_keys{
    "version", "name", "duration", "nodes", "links"};
// Key lists are space-separated words: one line per list reads like the schema's tables.
inline constexpr std::string_view top_level_keys =
    "version name seed duration step warmup datacenters nodes links slo faults";
// Node attributes whose value is the id of another node.
inline constexpr std::array<std::string_view, 3> reference_keys{"consumes", "backend", "origin"};

// Keys every node type accepts (docs/scenario-schema.md, "Node (common keys)").
inline constexpr std::string_view common_node_keys = "type dc replicas";
// Keys of a node that serves requests, i.e. every type but `client`: a client is a traffic
// source, with no service rate, service time or waiting room to describe (simsys.defaults
// refuses to resolve a capacity for one).
inline constexpr std::string_view server_node_keys = "capacity latency advanced";

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
inline constexpr std::array<OwnKeys, model::node_type_names.size()> own_keys_by_type{{
    {model::NodeType::client, "rate ops keys sizes timeout retries"},
    {model::NodeType::cdn, "ttl hit_ratio origin keys write_ops"},
    {model::NodeType::load_balancer, "policy vnodes"},
    {model::NodeType::rate_limiter, "algorithm limit burst window backend"},
    {model::NodeType::service, "ops cost"},
    {model::NodeType::cache, "eviction strategy keys hit_ratio backend ttl then cost write_ops"},
    {model::NodeType::sql_db, "replication_lag shards pool write_ops cost"},
    {model::NodeType::kv_cluster,
     "nodes vnodes replication write_quorum read_quorum mode write_ops"},
    {model::NodeType::message_queue, "partitions retention"},
    {model::NodeType::worker_pool, "consumes workers cost then"},
    {model::NodeType::blob_store, "then write_ops"},
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
inline constexpr fluid::Capacity engine_capacity_defaults{};
inline constexpr double default_yellow = engine_capacity_defaults.yellow;
inline constexpr double default_red = engine_capacity_defaults.red;

// Range rules for the numeric keys. NaN and infinity written as such never reach them
// (parse_yaml_double rejects them as no finite number); a decimal too large for a double
// (1e309) arrives rounded to infinity and fails every rule but is_zero_or_more, whose key
// has its own upper bound. An infinite variability or threshold has no meaning in the
// queueing formulas.
bool is_positive(double v);
bool is_non_negative(double v);
// Non-negative, infinity included: for a key whose upper bound is a separate rule with its
// own message, such as a link weight, so 1e309 is reported as too large, not as negative.
bool is_zero_or_more(double v);
// A utilization fraction: above 0 (a zero threshold would colour an idle node) and at most
// 1 (utilization cannot exceed 100%).
bool is_unit_fraction(double v);
// A ratio bound (drop rate, utilization): a fraction of all requests or of all time, so
// 0 (none allowed) up to 1 (everything); a bound above 1 could never be broken.
bool is_ratio(double v);

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
inline constexpr std::array<AdvancedNumber, 4> advanced_numbers{{
    {"service_variability", is_non_negative, "a finite non-negative number",
     &model::Advanced::service_variability},
    {"burstiness", is_non_negative, "a finite non-negative number", &model::Advanced::burstiness},
    {"yellow", is_unit_fraction, "in (0, 1]", &model::Advanced::yellow},
    {"red", is_unit_fraction, "in (0, 1]", &model::Advanced::red},
}};

// The advanced keys the schema types as int, read by the integer rule of `replicas` (a plain
// whole number: no "3.0", no "1e3"). `queue_limit` is a double in the model, as the queueing
// formulas take it, but a waiting room holds whole requests.
inline constexpr std::string_view advanced_integer_keys = "concurrency queue_limit";
// The other keys read as whole numbers (read_positive_int, parse_whole<int>, parse_count), by
// where they sit: at the top level, on a node, and in a client's `keys:` block. Together with
// advanced_integer_keys they decide counts_whole_things; a new integer key belongs in one of them.
inline constexpr std::string_view top_level_integer_keys = "version seed";
inline constexpr std::string_view node_integer_keys =
    "replicas vnodes burst shards pool partitions workers nodes replication write_quorum "
    "read_quorum keys";
inline constexpr std::string_view client_keys_integer_keys = "space uniform";

// How a scenario writes an empty list or map, for the messages about a key left without a
// value where an empty one is valid (`links:` -> "write [] for none").
inline constexpr std::string_view empty_list = "[]";
inline constexpr std::string_view empty_map = "{}";

// yaml-cpp counts lines and columns from 0; editors and compilers count from 1.
ConfigError error_at_mark(const std::string& file, const YAML::Mark& mark, std::string message);

ConfigError error_at(const std::string& file, const YAML::Node& node, std::string message);

// Pattern: Parameter Object — the context every reader needs travels as one value: the
// file, and the prefix naming the enclosing construct ("node 'a'", "link a -> b", "slo"),
// so every message says where to look. The document's own site has no prefix.
struct Site {
  const std::string& file;
  std::string where;

  [[nodiscard]] ConfigError error(const YAML::Node& at, std::string_view what) const;

  // The error as the failed outcome of any reader: std::unexpected<ConfigError> converts
  // to every std::expected<T, ConfigError>.
  [[nodiscard]] std::unexpected<ConfigError> fail(const YAML::Node& at,
                                                  std::string_view what) const;

  // A site one construct deeper, in the same file.
  [[nodiscard]] Site within(std::string inner) const;
};

bool is_declared(const NodeIndex& ids, std::string_view id);

// True when `word` is one of the space-separated words of `words`.
bool lists_word(std::string_view words, std::string_view word);

// A callable that stores its argument in `target`. Passed to std::expected::transform it
// turns "read a value" into "read it into this field": the result is expected<void, E>.
// C++20 abbreviated function template: an `auto&` parameter makes this a template without
// the `template <typename T>` line; it serves both plain fields and std::optional ones.
auto store_in(auto& target) {
  return [&target](auto value) { target = std::move(value); };
}

// IsDefined() first: asking a missing key's node for its type throws.
bool is_sequence(const YAML::Node& node);

// True for a key written with nothing after it; false for a missing key, too.
bool is_null(const YAML::Node& node);

// The text of a scalar, "" for a list, a map or null; used where the caller's own message
// ("must be always, miss or failure", ...) covers those shapes too.
std::string scalar_text(const YAML::Node& node);

// The text of a number, "" for anything that is no number. One rule for every numeric key:
// a number is a plain (unquoted, untagged) scalar, which yaml-cpp tags "?". A quoted "4" is
// a string in YAML ("!"), so `replicas: "4"` and `version: "1"` are rejected, as a typed
// language would reject the string "4" where an int is expected.
std::string number_text(const YAML::Node& node);

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
double rounded_out_of_range(std::string_view text);

// A number as YAML writes it, or nothing.
// - Not-a-number and infinity are no finite number in any spelling, so every key answers
//   them with its own "must be a finite ..." message: YAML's `.nan`, `.inf`, `-.inf` are no
//   text from_chars reads, and its own `nan`, `inf`, `infinity` are read as values and
//   rejected by the one isfinite test below.
// - A decimal beyond a double's range is a number all the same, rounded (see above), so the
//   key's range rule rejects 1e309 with its range message ("weight must be at most 1e9"),
//   not as no number, and 1e-400 is zero.
std::optional<double> parse_yaml_double(std::string_view text);

// The value of a numeric key, or nothing when it is no number (see number_text).
std::optional<double> number_value(const YAML::Node& node);

// Is `node` written as a number at all, finite or not, in range or not (1e309 and `.nan` are;
// a quoted "0.5", a word, "2k", a list or a map are not)? Lets a reader answer "must be a
// number" before its range rule, as the rate curve and the faults list do.
bool is_written_as_number(const YAML::Node& node);

// The key node of `parent[key]`, for messages about the key itself. yaml-cpp's lookup
// returns only the value.
YAML::Node key_node(const YAML::Node& parent, std::string_view key);

// The node an error about the value of `parent[key]` points at. A key written with nothing
// after it (`capacity:`, `ops:`) holds null, and yaml-cpp places a null value where the next
// token starts, usually the next line: for a null the key is where the reader must look.
// Any other value is its own best location.
YAML::Node shape_site(const YAML::Node& parent, std::string_view key);

// The same for one entry of a map being iterated, whose key node is at hand.
YAML::Node shape_site(const YamlElement& entry);

// "<label> must not be empty". Where an empty list or map is valid, `none` ("[]", "{}") says
// how to write one, so the message does not contradict `links: []` loading fine.
std::string empty_message(std::string_view label, std::string_view none);

// A key holding null is reported as empty, at the key (shape_site), whatever the value's
// type: a scalar, a list or a map. Every reader runs this before its own shape check, so no
// "must be a map" ever points at the line after a bare key. `label` names the key in the
// message ("capacity", "rate.base", "op share 'ping'").
CheckResult check_not_null(const Site& site, const YAML::Node& parent, std::string_view key,
                           std::string_view label, std::string_view none = {});

// The same for one entry of a map being iterated (an op share, an advanced key, a datacenter).
CheckResult check_entry_not_null(const Site& site, const YamlElement& entry, std::string_view label,
                                 std::string_view none = {});

// The text of `parent[key]`, which the caller has checked is present. yaml-cpp's Scalar()
// silently returns "" for a list, a map or null, so those shapes are rejected here, for
// every key.
std::expected<std::string, ConfigError> scalar_value(const Site& site, const YAML::Node& parent,
                                                     std::string_view key, std::string_view label);

std::expected<std::string, ConfigError> scalar_value(const Site& site, const YAML::Node& parent,
                                                     std::string_view key);

// A unit parser's error, attributed to the offending value and prefixed with its key.
ConfigError unit_error(const Site& site, const YAML::Node& value, std::string_view key,
                       const ParseError& error);

// The value a unit parser (parse_duration, parse_rate, parse_time_point, ...) produces.
template <typename Parse>
using Parsed = typename std::invoke_result_t<Parse&, std::string_view>::value_type;

// One scalar read by a unit parser; `label` names it in the messages ("rate.wave.period",
// "rate.points[2]"). A list or a map is no text to parse.
template <typename Parse>
std::expected<Parsed<Parse>, ConfigError> parse_scalar(const Site& site, const YAML::Node& value,
                                                       std::string_view label, Parse parse) {
  // C++20 std::format: type-checked formatting that returns a std::string, replacing
  // std::ostringstream chains and snprintf buffers.
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
                                                       std::string_view key);

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
                                                     std::string_view label);

// The name a map key spells. A list or map is valid YAML as a key (`[a, b]: 1`) but names
// nothing, and yaml-cpp's Scalar() reads it as "", which surfaced as "unknown key ''". `noun`
// names the key in the message: "top-level key", "node id", "advanced key", ... A null key
// (`~: 1`) is passed on as "", so the caller's own empty-name message applies.
std::expected<std::string, ConfigError> key_name(const Site& site, const YAML::Node& key,
                                                 std::string_view noun);

Site node_site(const Site& site, std::string_view id);

// "step refers to unknown node 'x'"; without `what` (when the site already names the
// referring construct, as "link 1" does) just "refers to unknown node 'x'".
std::string unknown_node_message(std::string_view what, std::string_view id);

// `parent[key]` (present) must name a declared node. `what` names the referring construct
// in the message: "step", "backend", ...
CheckResult check_reference(const Site& site, const YAML::Node& parent, std::string_view key,
                            std::string_view what, const NodeIndex& ids);

// A misspelt key would otherwise be ignored. `kind` names the key in the message:
// "unknown step key 'x'", "unknown key 'x'".
CheckResult check_known_keys(const Site& site, const YAML::Node& map, std::string_view allowed,
                             std::string_view kind);

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
                                 double& target);

// ---- Sizes (spec §19 entry 1) ------------------------------------------------------

// One request's size, as a client's `sizes` entry or a step's `size` writes it (parse_size:
// `50MB`). The message states the grammar, then the parser's reason ("missing size unit", "size
// must be at most 1e15B"), since the grammar alone does not say what is wrong with `2e15B`.
// `label` names the value: "sizes 'stream'", "step size". A request size must be positive:
// parse_size reads `0B` (a size), but a request of no bytes costs a byte-sized node 0 MB, so
// the node could serve infinitely many of them (its demand in requests divides by the size).
std::expected<Bytes, ConfigError> read_size(const Site& site, const YAML::Node& value,
                                            std::string_view label);

// Pattern: Parameter Object — what every step of one node is read against: the node's
// site (for messages), its id (a step may not target it) and the declared ids.
struct StepScope {
  const Site& site;
  std::string_view node_id;
  const NodeIndex& ids;
};

// ---- What each concern offers the others ------------------------------------------------
//
// Declared here, defined in the unit named above each group: the pipeline in config.cpp calls
// every concern, and a node body (nodes.cpp) reads its steps and a client's workload.

// header.cpp: the document's top level, its header keys and its datacenters.
CheckResult check_top_level(const Site& site, const YAML::Node& root);
CheckResult read_header(const Site& site, const YAML::Node& root, model::Scenario& scenario);
std::expected<Datacenters, ConfigError> read_datacenters(const Site& site, const YAML::Node& root);

// nodes.cpp: the two passes over `nodes`.
std::expected<Declarations, ConfigError> collect_node_declarations(const Site& site,
                                                                   const YAML::Node& nodes,
                                                                   const Datacenters& datacenters);
CheckResult read_node_bodies(const Site& site, const YAML::Node& nodes, Declarations& declared);

// steps.cpp: a node's `ops` step lists and its `then` steps.
CheckResult read_op_steps(const StepScope& scope, const YAML::Node& body, model::NodeSpec& node);
CheckResult read_then(const StepScope& scope, const YAML::Node& body, model::NodeSpec& node);

// workload.cpp: a client's traffic, and what other concerns reuse of it: a bounded rate (a rate
// limiter's `limit`), a whole count (a key-value cluster's counts) and whether a client sends an
// op (an SLO's `op`).
std::expected<Rate, ConfigError> read_bounded_rate(const Site& site, const YAML::Node& parent,
                                                   std::string_view key, std::string_view label);
std::optional<std::int64_t> parse_count(std::string_view text);
bool sends_op(const model::Workload& workload, std::string_view op);
std::expected<model::Workload, ConfigError> read_workload(const Site& site, const YAML::Node& body);

// links.cpp: the `links` list.
std::expected<std::vector<model::LinkSpec>, ConfigError> collect_links(
    const Site& site, const YAML::Node& links, const Declarations& declared);

// slos.cpp: the `slo` list.
std::expected<std::vector<model::SloSpec>, ConfigError> collect_slos(const Site& site,
                                                                     const YAML::Node& root,
                                                                     const Declarations& declared,
                                                                     const model::Scenario& run);

// faults.cpp: the `faults` list (spec §4.1), after the nodes and datacenters it names.
std::expected<std::vector<model::Fault>, ConfigError> read_faults(const Site& site,
                                                                  const YAML::Node& root,
                                                                  const Declarations& declared,
                                                                  const model::Scenario& run);

// overrides.cpp: `simsys sweep`'s replaced values.
std::expected<YAML::Node, ConfigError> find_override_target(const std::string& file,
                                                            const YAML::Node& root,
                                                            std::string_view path);
// C++20 std::span<const T>: a non-owning view of contiguous elements (a vector, an array or
// `{}`), passed without a copy.
std::expected<void, ConfigError> apply_overrides(const std::string& file, YAML::Node& root,
                                                 std::span<const Override> overrides);

}  // namespace simsys::config::detail
