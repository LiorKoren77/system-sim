// Module `simsys.config`, implementation unit: the `links` list. Each link's ends, direction,
// op filter, latency, bandwidth and weight; one link per pair and demand direction; no link
// carrying demand out of a database.
module;
#include <yaml-cpp/yaml.h>

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

// ---- Links ----------------------------------------------------------------------------

// Unnamed namespace: the helpers in these blocks are this unit's own (internal linkage).
// Outside it a function of a module implementation unit has module linkage, so every unit
// of simsys.config could call it and the linker sees its name; only what detail.hpp declares
// is shared between the units.
namespace {

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

// `bandwidth: 40Gbit/s` (model::LinkSpec::bandwidth), optional: a byte rate, in bytes or, as a
// network is rated, in bits (parse_byte_rate). The message replaces the parser's: a request
// rate (`10k/s`) is the likely mistake, and the author needs the byte forms. Positive: a pipe
// of zero capacity carries nothing. At most 1e15 B/s, a byte capacity's ceiling
// (max_rate_per_second read in bytes: a million GB/s, far above any network).
CheckResult read_link_bandwidth(const Site& site, const YAML::Node& link,
                                std::optional<ByteRate>& bandwidth) {
  const auto value = link["bandwidth"];
  if (!value) return {};
  if (auto checked = check_not_null(site, link, "bandwidth", "bandwidth"); !checked) {
    return checked;
  }
  const auto rate = parse_byte_rate(scalar_text(value));
  if (!rate) {
    return site.fail(value, "bandwidth is a byte rate (B/s, or bit/s for a network: 40Gbit/s)");
  }
  if (!is_positive(rate->bytes_per_second)) return site.fail(value, "bandwidth must be positive");
  if (rate->bytes_per_second > max_rate_per_second) {
    return site.fail(value, "bandwidth must be at most 1e15B/s");
  }
  bandwidth = *rate;
  return {};
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
  if (auto read = read_link_bandwidth(site, link, spec.bandwidth); !read) return read;
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
  // C++20 designated initializers: each member named where it is set, so two members of the
  // same type cannot be swapped unnoticed as with positional aggregate initialization.
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

}  // namespace

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

}  // namespace simsys::config::detail
