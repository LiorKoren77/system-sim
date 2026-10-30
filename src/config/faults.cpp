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

// A cache flush is an instant: the cache is empty from `at` on and refills from its own misses,
// so there is nothing to mend and no length.
bool lasts(model::FaultKind kind) {
  return kind != model::FaultKind::cache_flush;
}

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
  if (auto checked = check_not_null(site, entry, "kind", "kind"); !checked) {
    return std::unexpected(checked.error());
  }
  const auto name = scalar_text(value);
  if (const auto kind = model::parse_fault_kind(name)) return *kind;
  if (name == partition_kind) {
    return site.fail(value, "partition is not modelled in phase 3 (see the backlog)");
  }
  return site.fail(value, std::format("unknown kind '{}'", name));
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
// would never be seen, so it is refused rather than silently dropped.
CheckResult read_fault_at(const Site& site, const YAML::Node& entry, const model::Scenario& run,
                          model::Fault& fault) {
  if (auto required = require_key(site, entry, fault.kind, "at"); !required) return required;
  auto at = parse_key(site, entry, "at", "at", parse_time_point);
  if (!at) return std::unexpected(at.error());
  if (at->count() >= run.duration.count()) {
    return site.fail(entry["at"], std::format("at must fall inside the run ({})",
                                              describe_duration(run.duration)));
  }
  fault.at = *at;
  return {};
}

// `for`: how long the fault lasts, positive; a cache flush takes none (lasts). The fault may run
// past the end of the run: the run simply ends while it is still on.
CheckResult read_fault_length(const Site& site, const YAML::Node& entry, model::Fault& fault) {
  const auto kind = model::to_string(fault.kind);
  if (!lasts(fault.kind)) {
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

// `replicas: primary` (a sql_db's primary, ch. 1: writes stop until a replica is promoted) or a
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
  const auto count = parse_whole<int>(number_text(value));
  if (!count) return site.fail(value, "replicas must be a whole number or primary");
  if (*count < 1) return site.fail(value, "replicas must be at least 1");
  if (*count > model::copies_of(node)) {
    return site.fail(value, std::format("replicas {} is more than node '{}' has ({})", *count,
                                        node.id, model::copies_of(node)));
  }
  fault.replicas = *count;
  return {};
}

// A capacity fault scales the node's service rate by `factor` in (0, 1): 0 would be a node_down,
// and 1 or more is no fault (a node that is faster is a different scenario's `capacity`).
CheckResult read_capacity_factor(const Site& site, const YAML::Node& entry, model::Fault& fault) {
  if (auto required = require_key(site, entry, fault.kind, "factor"); !required) return required;
  if (auto checked = check_not_null(site, entry, "factor", "factor"); !checked) return checked;
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
CheckResult read_spike_factor(const Site& site, const YAML::Node& entry, model::Fault& fault) {
  if (auto required = require_key(site, entry, fault.kind, "factor"); !required) return required;
  if (auto checked = check_not_null(site, entry, "factor", "factor"); !checked) return checked;
  const auto factor = number_value(entry["factor"]);
  if (!factor || !is_positive(*factor)) {
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
  return faults;
}

}  // namespace simsys::config::detail
