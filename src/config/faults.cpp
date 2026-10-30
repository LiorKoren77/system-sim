// Module `simsys.config`, implementation unit: the `faults` list (spec §4.1). Each entry is typed
// into a model::Fault and checked against the run's header (`at` inside the run), the declared
// nodes (a node of a type the kind can act on, at most its replicas down) and the declared
// datacenters. A message names the entry by its position: "faults[2]: cache_flush takes no for".
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
// Only for fluid::Capacity's default zone thresholds (detail.hpp): one source of truth for
// 0.7 / 0.9.
import simsys.fluid;
import simsys.model;

// The loader's shared vocabulary and common readers, included in the module purview after
// the imports: its declarations belong to simsys.config, like this unit's own, and can
// name ConfigError (see the header's comment for why it is no module partition).
#include "detail.hpp"

namespace simsys::config::detail {

// ---- Faults ------------------------------------------------------------------------------
//
// A fault is a scripted failure (model::Fault documents what each kind models and from which
// chapter): at `at` something breaks, and all but a cache flush mend `for` later. The loader
// refuses every key a kind does not read, so a scenario never carries a key that silently does
// nothing (a `factor` on a node_down, a `for` on a flush).

// Unnamed namespace: the helpers in these blocks are this unit's own (internal linkage).
// Outside it a function of a module implementation unit has module linkage, so every unit
// of simsys.config could call it and the linker sees its name; only what detail.hpp declares
// is shared between the units.
namespace {

constexpr std::string_view fault_keys = "at kind for node replicas factor extra datacenter";

// The kind the schema names but phase 3 does not model: a network partition (book ch. 6, the
// CAP theorem's P) needs per-replica reachability, which the fluid model does not track. It is
// refused with its own message rather than as an unknown kind, so the author learns it is planned.
constexpr std::string_view partition_kind = "partition";

// A key that only some kinds read, and those kinds as the message names them. `kinds` is read
// with lists_word too: "and" names no kind, so it never matches one.
struct KindKey {
  std::string_view key;
  std::string_view kinds;
};

// Pattern: Table-driven method — which kinds read `replicas`, `factor`, `extra` and `datacenter`
// is data, one row per key, so one loop refuses each of them on every other kind. `node` and
// `for` are read by all kinds but one each, and have their own messages (read_fault_node,
// read_fault_length).
constexpr std::array<KindKey, 4> kind_keys{{
    {"replicas", "node_down"},
    {"factor", "capacity and traffic_spike"},
    {"extra", "latency_spike"},
    {"datacenter", "dc_down"},
}};

// A dc_down names a datacenter; every other kind acts on one node.
bool names_node(model::FaultKind kind) {
  return kind != model::FaultKind::dc_down;
}

// "a for", "an extra": the article the "<kind> needs ..." messages put before a key.
std::string with_article(std::string_view key) {
  // C++23 std::string_view::contains: "is this character in it?" in one call, replacing
  // `find(c) != npos`, which reads as index arithmetic when the question is yes or no.
  const bool vowel = std::string_view{"aeiou"}.contains(key.front());
  return std::format("{} {}", vowel ? "an" : "a", key);
}

// A key every fault of `kind` must state.
CheckResult require_key(const Site& site, const YAML::Node& entry, model::FaultKind kind,
                        std::string_view key) {
  if (entry[std::string{key}]) return {};
  return site.fail(entry, std::format("{} needs {}", model::to_string(kind), with_article(key)));
}

std::expected<model::FaultKind, ConfigError> read_fault_kind(const Site& site,
                                                             const YAML::Node& entry) {
  const auto value = entry["kind"];
  if (!value) return site.fail(entry, "needs a kind");
  // scalar_value refuses a bare `kind:` and a list or map (`kind: [node_down]`: "kind must be a
  // scalar"), which would otherwise read as the empty name, an unknown kind ''.
  auto name = scalar_value(site, entry, "kind");
  if (!name) return std::unexpected(name.error());
  if (const auto kind = model::parse_fault_kind(*name)) return *kind;
  if (*name == partition_kind) {
    return site.fail(value, "partition is not modelled in phase 3 (see the backlog)");
  }
  return site.fail(value, std::format("unknown kind '{}'", *name));
}

// `replicas` on a node_down, `factor` on a capacity fault, ...: a key another kind reads.
CheckResult check_keys_of_kind(const Site& site, const YAML::Node& entry, model::FaultKind kind) {
  for (const auto& [key, kinds] : kind_keys) {
    if (!entry[std::string{key}] || lists_word(kinds, model::to_string(kind))) continue;
    return site.fail(key_node(entry, key), std::format("{} applies to {} only", key, kinds));
  }
  return {};
}

// `at` is a point on the run's clock, written as the rate curve writes one (parse_time_point:
// "21:30" or "90m", both offsets from t = 0, midnight). A fault that starts at or after the end
// would never be seen, so it is refused rather than silently dropped. Every kind needs one, so the
// message names no kind ("needs an at", like "needs a kind").
CheckResult read_fault_at(const Site& site, const YAML::Node& entry, const model::Scenario& run,
                          model::Fault& fault) {
  if (!entry["at"]) return site.fail(entry, "needs an at");
  auto at = parse_key(site, entry, "at", "at", parse_time_point);
  if (!at) return std::unexpected(at.error());
  if (at->count() >= run.duration.count()) {
    return site.fail(entry["at"], std::format("at must fall inside the run ({})",
                                              describe_duration(run.duration)));
  }
  fault.at = *at;
  return {};
}

// `for`: how long the fault lasts, positive; a cache flush takes none (model::lasts). The fault may
// run past the end of the run: the run simply ends while it is still on.
CheckResult read_fault_length(const Site& site, const YAML::Node& entry, model::Fault& fault) {
  const auto kind = model::to_string(fault.kind);
  if (!model::lasts(fault.kind)) {
    if (!entry["for"]) return {};
    return site.fail(key_node(entry, "for"), std::format("{} takes no for", kind));
  }
  if (auto required = require_key(site, entry, fault.kind, "for"); !required) return required;
  auto length = parse_key(site, entry, "for", "for", parse_duration);
  if (!length) return std::unexpected(length.error());
  if (length->count() <= 0) return site.fail(entry["for"], "for must be positive");
  fault.duration = *length;
  return {};
}

const model::NodeSpec& declared_node(const Declarations& declared, std::string_view id) {
  return declared.nodes[declared.ids.find(id)->second];
}

// What each kind can act on. A traffic spike multiplies what a client sends, so it needs a client;
// a flush empties a cache, so it needs a cache or a CDN (an edge cache, ch. 1); the other kinds
// stop or slow a server, and a client serves nothing (it is a traffic source with no capacity).
CheckResult check_node_fits_kind(const Site& site, const YAML::Node& value, model::FaultKind kind,
                                 const model::NodeSpec& node) {
  const auto kind_name = model::to_string(kind);
  const auto type = node.type;
  const auto type_name = model::to_string(type);
  if (kind == model::FaultKind::traffic_spike) {
    if (type == model::NodeType::client) return {};
    return site.fail(value,
                     std::format("traffic_spike needs a client, '{}' is a {}", node.id, type_name));
  }
  if (kind == model::FaultKind::cache_flush) {
    if (type == model::NodeType::cache || type == model::NodeType::cdn) return {};
    return site.fail(
        value, std::format("cache_flush needs a cache or cdn, '{}' is a {}", node.id, type_name));
  }
  if (type != model::NodeType::client) return {};
  return site.fail(
      value, std::format("{} cannot name a client (clients emit, they do not serve)", kind_name));
}

CheckResult read_fault_node(const Site& site, const YAML::Node& entry, const Declarations& declared,
                            model::Fault& fault) {
  if (!names_node(fault.kind)) {
    if (!entry["node"]) return {};
    return site.fail(key_node(entry, "node"), "dc_down takes no node (it names a datacenter)");
  }
  if (auto required = require_key(site, entry, fault.kind, "node"); !required) return required;
  auto id = scalar_value(site, entry, "node");
  if (!id) return std::unexpected(id.error());
  if (!is_declared(declared.ids, *id)) {
    return site.fail(entry["node"], std::format("node '{}' is not declared", *id));
  }
  if (auto checked =
          check_node_fits_kind(site, entry["node"], fault.kind, declared_node(declared, *id));
      !checked) {
    return checked;
  }
  fault.node = std::move(*id);
  return {};
}

// A count beyond an int (99999999999) is still a count, too many or too few: the message names
// the bound it breaks, not "must be a whole number".
CheckResult refuse_replicas_beyond_int(const Site& site, const YAML::Node& value,
                                       std::string_view text, const model::NodeSpec& node) {
  // C++20 std::string_view::starts_with: a prefix test in one call, replacing
  // `!text.empty() && text.front() == '-'` or `text.compare(0, 1, "-") == 0`.
  if (text.starts_with('-')) return site.fail(value, "replicas must be at least 1");
  return site.fail(value, std::format("replicas must be at most {}", model::copies_of(node)));
}

// `replicas: primary` (a sql_db's primary, ch. 1: its writes stop, model::Fault) or a
// count, 1 up to the node's copies; absent, the whole node goes down.
CheckResult read_fault_replicas(const Site& site, const YAML::Node& entry,
                                const Declarations& declared, model::Fault& fault) {
  const auto value = entry["replicas"];
  if (!value) return {};
  if (auto checked = check_not_null(site, entry, "replicas", "replicas"); !checked) return checked;
  const auto& node = declared_node(declared, fault.node);
  if (scalar_text(value) == "primary") {
    if (node.type != model::NodeType::sql_db) {
      return site.fail(value, "primary applies to a sql_db only");
    }
    fault.primary = true;
    return {};
  }
  const auto text = number_text(value);
  const auto count = parse_whole<int>(text);
  if (!count && count.error() == NumberFault::out_of_range) {
    return refuse_replicas_beyond_int(site, value, text, node);
  }
  if (!count) return site.fail(value, "replicas must be a whole number or primary");
  if (*count < 1) return site.fail(value, "replicas must be at least 1");
  if (*count > model::copies_of(node)) {
    return site.fail(value, std::format("replicas {} is more than node '{}' has ({})", *count,
                                        node.id, model::copies_of(node)));
  }
  fault.replicas = *count;
  return {};
}

// `factor` is stated, not bare, and written as a number: text, a quoted number, "2k", "50%" or a
// list is "factor must be a number", as in the rate curve (read_curve_number), not a range
// message that would call "three" not positive. 1e400 and `.nan` are numbers, out of range.
CheckResult check_factor_is_number(const Site& site, const YAML::Node& entry,
                                   model::FaultKind kind) {
  if (auto required = require_key(site, entry, kind, "factor"); !required) return required;
  if (auto checked = check_not_null(site, entry, "factor", "factor"); !checked) return checked;
  if (is_written_as_number(entry["factor"])) return {};
  return site.fail(entry["factor"], "factor must be a number");
}

// A capacity fault scales the node's service rate by `factor` in (0, 1): 0 would be a node_down,
// and 1 or more is no fault (a node that is faster is a different scenario's `capacity`).
CheckResult read_capacity_factor(const Site& site, const YAML::Node& entry, model::Fault& fault) {
  if (auto checked = check_factor_is_number(site, entry, fault.kind); !checked) return checked;
  const auto factor = number_value(entry["factor"]);
  if (!factor || !(*factor > 0.0 && *factor < 1.0)) {
    return site.fail(entry["factor"],
                     "capacity factor must be between 0 and 1 (a gain is the scenario's capacity)");
  }
  fault.factor = *factor;
  return {};
}

// A traffic spike multiplies a client's rate by `factor`, positive and at most max_step_factor,
// the bound of a curve spike's factor (workload.cpp's read_spike), for the same reason: rate x
// factor must stay far from overflowing a double. A factor below 1 is a lull, which is allowed.
// `> 0.0`, not is_positive (which refuses infinity): 1e400, read as infinity, breaks the upper
// bound, and its message says so.
CheckResult read_spike_factor(const Site& site, const YAML::Node& entry, model::Fault& fault) {
  if (auto checked = check_factor_is_number(site, entry, fault.kind); !checked) return checked;
  const auto factor = number_value(entry["factor"]);
  if (!factor || !(*factor > 0.0)) {
    return site.fail(entry["factor"], "traffic_spike factor must be positive");
  }
  if (*factor > max_step_factor) {
    return site.fail(entry["factor"], "traffic_spike factor must be at most 1e6");
  }
  fault.factor = *factor;
  return {};
}

CheckResult read_fault_factor(const Site& site, const YAML::Node& entry, model::Fault& fault) {
  if (fault.kind == model::FaultKind::capacity) return read_capacity_factor(site, entry, fault);
  if (fault.kind == model::FaultKind::traffic_spike) return read_spike_factor(site, entry, fault);
  return {};
}

// `extra`: the service time a latency spike adds to every request the node serves (a slow disk,
// a garbage-collection pause, a degraded dependency), a positive duration.
CheckResult read_fault_extra(const Site& site, const YAML::Node& entry, model::Fault& fault) {
  if (fault.kind != model::FaultKind::latency_spike) return {};
  if (auto required = require_key(site, entry, fault.kind, "extra"); !required) return required;
  auto extra = parse_key(site, entry, "extra", "extra", parse_duration);
  if (!extra) return std::unexpected(extra.error());
  if (extra->count() <= 0) return site.fail(entry["extra"], "extra must be positive");
  fault.extra = *extra;
  return {};
}

CheckResult read_fault_datacenter(const Site& site, const YAML::Node& entry,
                                  const model::Scenario& run, model::Fault& fault) {
  if (fault.kind != model::FaultKind::dc_down) return {};
  if (auto required = require_key(site, entry, fault.kind, "datacenter"); !required) {
    return required;
  }
  auto name = scalar_value(site, entry, "datacenter");
  if (!name) return std::unexpected(name.error());
  // C++23 std::ranges::contains: "is this value in the range?" in one call, replacing
  // `std::ranges::find(r, v) != r.end()`, which spells the question as an iterator comparison.
  if (!std::ranges::contains(run.datacenters, *name)) {
    return site.fail(entry["datacenter"], std::format("datacenter '{}' is not declared", *name));
  }
  fault.datacenter = std::move(*name);
  return {};
}

// The keys of one entry, once its kind is known and every key it states is one that kind reads.
// `node` comes before `replicas`, which is checked against that node.
CheckResult read_fault_keys(const Site& site, const YAML::Node& entry, const Declarations& declared,
                            const model::Scenario& run, model::Fault& fault) {
  if (auto read = read_fault_at(site, entry, run, fault); !read) return read;
  if (auto read = read_fault_node(site, entry, declared, fault); !read) return read;
  if (auto read = read_fault_length(site, entry, fault); !read) return read;
  if (auto read = read_fault_replicas(site, entry, declared, fault); !read) return read;
  if (auto read = read_fault_factor(site, entry, fault); !read) return read;
  if (auto read = read_fault_extra(site, entry, fault); !read) return read;
  return read_fault_datacenter(site, entry, run, fault);
}

// The kind first: it decides which keys are valid, and a `partition` entry (with keys of its own,
// such as `fraction`) gets its own message, not "unknown key 'fraction'".
std::expected<model::Fault, ConfigError> read_fault(const Site& site, const YAML::Node& entry,
                                                    const Declarations& declared,
                                                    const model::Scenario& run) {
  if (!entry.IsMap()) return site.fail(entry, "must be a map");
  auto kind = read_fault_kind(site, entry);
  if (!kind) return std::unexpected(kind.error());
  if (auto checked = check_known_keys(site, entry, fault_keys, "key"); !checked) {
    return std::unexpected(checked.error());
  }
  if (auto checked = check_keys_of_kind(site, entry, *kind); !checked) {
    return std::unexpected(checked.error());
  }
  // C++20 designated initializers: each member named where it is set, so two members of the
  // same type cannot be swapped unnoticed as with positional aggregate initialization.
  model::Fault fault{.kind = *kind};
  return read_fault_keys(site, entry, declared, run, fault).transform([&] {
    return std::move(fault);
  });
}

// ---- Overlapping traffic spikes ----------------------------------------------------------------
//
// Spikes on one client compound: two spikes of 1000 at once send a million times the rate. So,
// as for the rate curve's own spikes (workload.cpp's check_spike_product), bounding each factor
// by max_step_factor is not enough, and the bound applies to the product at every moment. The two
// caps are checked separately: the faults of a client are not multiplied with its curve's spikes
// (each list stays far from overflow on its own: 1e6 x 1e6 x 1e15 requests/s is 1e27, still far
// from a double's ~1.8e308).

bool is_spike_on(const model::Fault& fault, std::string_view client) {
  return fault.kind == model::FaultKind::traffic_spike && fault.node == client;
}

// Is `fault` on at `t`, i.e. at <= t < at + duration? Written as a difference so it cannot
// overflow: both times are offsets from t = 0, never negative.
bool is_on_at(const model::Fault& fault, SimTime t) {
  return fault.at.count() <= t.count() && t.count() - fault.at.count() < fault.duration.count();
}

// The product of the factors of `client`'s spikes on at `t`. C++20 std::span: a non-owning view
// of contiguous elements (pointer + size), replacing the `const T*, std::size_t` pair or a
// `const std::vector<T>&` that would tie the function to one container; here it also lets the
// caller pass the first i + 1 faults (span::first) without copying them.
double spike_product_at(std::span<const model::Fault> faults, std::string_view client, SimTime t) {
  double product = 1.0;
  for (const auto& fault : faults) {
    if (is_spike_on(fault, client) && is_on_at(fault, t)) product *= *fault.factor;
  }
  return product;
}

// Do `client`'s spikes among `faults` multiply past max_step_factor at some moment? The product
// is a step function that rises only where a spike starts, so its maximum is at some spike's
// `at`, and checking every start finds it (n starts x n faults, fine for a handful of faults).
bool spikes_exceed_cap(std::span<const model::Fault> faults, std::string_view client) {
  // C++20 std::ranges::any_of: takes the range itself, not a begin/end iterator pair.
  return std::ranges::any_of(faults, [&](const model::Fault& start) {
    return is_spike_on(start, client) &&
           spike_product_at(faults, client, start.at) > max_step_factor;
  });
}

// Each spike in document order is checked against the ones before it: the first prefix whose
// product passes the cap ends at the entry that tipped it over, and the message names that one,
// the later of the overlapping entries.
CheckResult check_spike_products(const Site& site, const YAML::Node& list,
                                 std::span<const model::Fault> faults) {
  for (std::size_t i = 0; i < faults.size(); ++i) {
    const auto& fault = faults[i];
    if (fault.kind != model::FaultKind::traffic_spike) continue;
    if (!spikes_exceed_cap(faults.first(i + 1), fault.node)) continue;
    return site.within(std::format("faults[{}]", i))
        .fail(list[i]["factor"],
              std::format("overlapping traffic_spike factors on '{}' multiply to more than 1e6",
                          fault.node));
  }
  return {};
}

}  // namespace

// `run`: the scenario's header (duration, for `at`) and datacenters (for a dc_down).
std::expected<std::vector<model::Fault>, ConfigError> read_faults(const Site& site,
                                                                  const YAML::Node& root,
                                                                  const Declarations& declared,
                                                                  const model::Scenario& run) {
  const auto list = root["faults"];
  if (!list) return std::vector<model::Fault>{};
  if (!list.IsSequence()) return site.fail(list, "faults must be a list");
  std::vector<model::Fault> faults;
  for (std::size_t i = 0; i < list.size(); ++i) {
    auto fault = read_fault(site.within(std::format("faults[{}]", i)), list[i], declared, run);
    if (!fault) return std::unexpected(fault.error());
    faults.push_back(std::move(*fault));
  }
  return check_spike_products(site, list, faults).transform([&] { return std::move(faults); });
}

}  // namespace simsys::config::detail
