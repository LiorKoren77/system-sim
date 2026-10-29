// Module `simsys.config`, implementation unit: `load` and the pipeline it runs (`validate`).
// The readers live in one implementation unit per concern: header.cpp (top level, header,
// datacenters), nodes.cpp (both passes over `nodes`, every type's own keys), steps.cpp,
// workload.cpp (a client's traffic), links.cpp, slos.cpp, overrides.cpp; what they share is
// declared in detail.hpp and defined in detail.cpp. yaml-cpp and <format> are included in the
// implementation units, not in config.cppm (see the GCC 14.2 note there).
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

// Pattern: Pipeline with early exit (railway-oriented error handling) — each stage returns
// std::expected<T, ConfigError> (T is void for a pure check, the parsed value otherwise),
// and the first error leaves the track and is returned, so the user sees one precise
// message. The stages are not independent: node bodies, links and SLOs need the ids the
// declaration pass collects, and SLOs need the clients' ops, so their order is fixed.
std::expected<model::Scenario, ConfigError> validate(const std::string& file,
                                                     const YAML::Node& root) {
  // C++20 designated initializers: each member named where it is set, so two members of the
  // same type cannot be swapped unnoticed as with positional aggregate initialization.
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
  auto slos = collect_slos(site, root, *declared, scenario);
  if (!slos) return std::unexpected(slos.error());
  scenario.nodes = std::move(declared->nodes);
  scenario.links = std::move(*links);
  scenario.slos = std::move(*slos);
  return scenario;
}

}  // namespace simsys::config::detail

namespace simsys::config {

std::expected<model::Scenario, ConfigError> load(const std::string& file) {
  return load(file, {});
}

std::expected<model::Scenario, ConfigError> load(const std::string& file,
                                                 std::span<const Override> overrides) {
  // The only exception boundary: yaml-cpp reports syntax errors (LoadFile) by throwing,
  // and so does any shape mistake the checks above do not pre-empt (subscripting a
  // scalar, reading a missing node); we return std::expected, so callers never need a
  // try/catch.
  try {
    auto root = YAML::LoadFile(file);
    if (auto applied = detail::apply_overrides(file, root, overrides); !applied) {
      return std::unexpected(applied.error());
    }
    return detail::validate(file, root);
  } catch (const YAML::Exception& e) {
    return std::unexpected(detail::error_at_mark(file, e.mark, e.msg));
  }
}

std::expected<void, ConfigError> check_override_path(const std::string& file,
                                                     std::string_view override_path) {
  try {
    return detail::find_override_target(file, YAML::LoadFile(file), override_path)
        .transform([](const YAML::Node&) {});
  } catch (const YAML::Exception& e) {
    return std::unexpected(detail::error_at_mark(file, e.mark, e.msg));
  }
}

std::string describe(const ConfigError& error) {
  if (error.line == 0) return std::format("{}: error: {}", error.file, error.message);
  return std::format("{}:{}:{}: error: {}", error.file, error.line, error.column, error.message);
}

}  // namespace simsys::config
