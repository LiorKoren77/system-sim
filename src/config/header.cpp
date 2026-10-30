// Module `simsys.config`, implementation unit: the document's top level. The checks that run
// before any section is read (the root is a map; the required and known top-level keys; no key
// written twice in one map; aliases that expand to at most a million entries), the header keys
// (`version`, `name`, `seed`, `duration`, `step`, `warmup`) and the `datacenters` list.
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
#include <set>
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

// ---- Repeated keys --------------------------------------------------------------------
//
// yaml-cpp keeps both entries of a key written twice in one map: a lookup by name sees the
// first, an iteration sees both. Readers of both kinds exist, so a repeated key would mean
// different things to different checks; it is rejected before any of them runs.

// Unnamed namespace: the helpers in these blocks are this unit's own (internal linkage).
// Outside it a function of a module implementation unit has module linkage, so every unit
// of simsys.config could call it and the linker sees its name; only what detail.hpp declares
// is shared between the units.
namespace {

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
    // C++20 ranges algorithm: std::ranges::find_if takes the range itself, not a begin/end
    // iterator pair.
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
      // C++20 designated initializers: each member named where it is set, so two members of the
      // same type cannot be swapped unnoticed as with positional aggregate initialization.
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

}  // namespace

CheckResult check_top_level(const Site& site, const YAML::Node& root) {
  if (auto checked = check_root_is_map(site, root); !checked) return checked;
  if (auto checked = check_keys_and_expansion(site, root); !checked) return checked;
  if (auto checked = check_required_top_level_keys(site, root); !checked) return checked;
  if (auto checked = check_known_top_level_keys(site, root); !checked) return checked;
  return check_sections_not_null(site, root);
}

// ---- Header ---------------------------------------------------------------------------

namespace {

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

}  // namespace

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

namespace {

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

}  // namespace

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

}  // namespace simsys::config::detail
