// Module `simsys.config`, implementation unit: the checks behind `load`. yaml-cpp and
// <format> are included here, not in config.cppm (see the GCC 14.2 note there).
module;
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// C++20 module implementation unit: `module simsys.config;` (no `export`) makes this file
// part of the named module. It sees everything the interface declared, exports nothing,
// and can include heavy headers in its own global module fragment, like a .cpp behind a
// header.
module simsys.config;

import simsys.core;

namespace simsys::config::detail {

// C++23 std::expected<void, E>: "either fine or this error". It replaces a bool plus an
// out-parameter for the error; an empty `{}` means the check passed.
using CheckResult = std::expected<void, ConfigError>;

constexpr std::array<std::string_view, 5> required_top_level_keys{"version", "name", "duration",
                                                                  "nodes", "links"};
// Node attributes whose value is the id of another node.
constexpr std::array<std::string_view, 3> reference_keys{"consumes", "backend", "origin"};

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

bool is_known_type(std::string_view type) {
  // C++20 std::ranges::find: takes the container itself instead of a begin/end pair,
  // so the two iterators can never come from different containers.
  return std::ranges::find(known_types, type) != known_types.end();
}

// C++20 std::span<const T>: a non-owning view of contiguous elements; callers can pass a
// std::vector (or an array) without copying it and without a pointer+size pair.
bool is_declared(std::span<const NodeDecl> nodes, std::string_view id) {
  return std::ranges::any_of(nodes, [id](const NodeDecl& node) { return node.id == id; });
}

// IsDefined() first: asking a missing key's node for its type throws.
bool is_sequence(const YAML::Node& node) {
  return node.IsDefined() && node.IsSequence();
}

// A unit parser's error, attributed to the offending value and prefixed with its key.
ConfigError unit_error(const std::string& file, const YAML::Node& value, std::string_view key,
                       const ParseError& error) {
  // C++20 std::format: type-checked (at compile time) formatting that returns a
  // std::string, replacing std::ostringstream chains and snprintf buffers.
  return error_at(file, value, std::format("{}: {}", key, error.message));
}

CheckResult check_required_top_level_keys(const std::string& file, const YAML::Node& root) {
  for (const auto key : required_top_level_keys) {
    if (!root[std::string{key}]) {
      return std::unexpected(error_at(file, root, std::format("missing top-level key '{}'", key)));
    }
  }
  return {};
}

CheckResult check_version(const std::string& file, const YAML::Node& root) {
  const auto version = root["version"];
  // as<int>(fallback) yields the fallback instead of throwing when the value is no int.
  if (version.as<int>(0) != 1) return std::unexpected(error_at(file, version, "version must be 1"));
  return {};
}

std::expected<SimTime, ConfigError> parse_duration_key(const std::string& file,
                                                       const YAML::Node& parent,
                                                       std::string_view key) {
  const auto value = parent[std::string{key}];
  // C++23 std::expected::transform_error: converts the error (ParseError -> ConfigError)
  // and passes a value through untouched, replacing an `if (!parsed) return ...` block.
  return parse_duration(value.Scalar()).transform_error([&](const ParseError& error) {
    return unit_error(file, value, key, error);
  });
}

// Capacity counts requests for most node types and bytes for storage ("500MB/s").
std::expected<void, ParseError> parse_capacity(std::string_view text) {
  // C++20 std::string_view::ends_with: a suffix test without size arithmetic.
  // C++23 std::expected::transform with a lambda that drops the value: only "valid or
  // not" matters here, so both parsers' results become std::expected<void, ParseError>.
  if (text.ends_with("B/s")) return parse_byte_rate(text).transform([](ByteRate) {});
  return parse_rate(text).transform([](Rate) {});
}

CheckResult check_capacity(const std::string& file, const YAML::Node& node) {
  const auto capacity = node["capacity"];
  if (!capacity) return {};
  return parse_capacity(capacity.Scalar()).transform_error([&](const ParseError& error) {
    return unit_error(file, capacity, "capacity", error);
  });
}

std::expected<NodeDecl, ConfigError> read_node_declaration(const std::string& file,
                                                           const YAML::Node& id,
                                                           const YAML::Node& body) {
  const auto type = body["type"];
  if (!type) {
    return std::unexpected(error_at(file, id, std::format("node '{}' needs a type", id.Scalar())));
  }
  if (!is_known_type(type.Scalar())) {
    return std::unexpected(
        error_at(file, type, std::format("unknown node type '{}'", type.Scalar())));
  }
  return NodeDecl{id.Scalar(), type.Scalar()};
}

// yaml-cpp iterates a map in document order, so the declarations keep that order.
std::expected<std::vector<NodeDecl>, ConfigError> collect_node_declarations(
    const std::string& file, const YAML::Node& nodes) {
  std::vector<NodeDecl> declarations;
  for (const auto& entry : nodes) {
    auto declaration = read_node_declaration(file, entry.first, entry.second);
    if (!declaration) return std::unexpected(declaration.error());
    declarations.push_back(std::move(*declaration));
  }
  return declarations;
}

// `what` names the referring construct in the message: "step", "link", "backend", ...
CheckResult check_reference(const std::string& file, const YAML::Node& reference,
                            std::string_view what, std::span<const NodeDecl> nodes) {
  if (is_declared(nodes, reference.Scalar())) return {};
  return std::unexpected(error_at(
      file, reference, std::format("{} refers to unknown node '{}'", what, reference.Scalar())));
}

CheckResult check_step(const std::string& file, const YAML::Node& step,
                       std::span<const NodeDecl> nodes) {
  const auto to = step["to"];
  if (!to) return std::unexpected(error_at(file, step, "step needs a 'to'"));
  return check_reference(file, to, "step", nodes);
}

CheckResult check_step_list(const std::string& file, const YAML::Node& steps,
                            std::span<const NodeDecl> nodes) {
  for (const auto& step : steps) {
    if (auto checked = check_step(file, step, nodes); !checked) return checked;
  }
  return {};
}

// A service's ops map each op to a step list; a client's ops map each op to a traffic
// share (a number), which names no node and is skipped.
CheckResult check_op_steps(const std::string& file, const YAML::Node& ops,
                           std::span<const NodeDecl> nodes) {
  for (const auto& op : ops) {
    if (!is_sequence(op.second)) continue;
    if (auto checked = check_step_list(file, op.second, nodes); !checked) return checked;
  }
  return {};
}

CheckResult check_reference_key(const std::string& file, const YAML::Node& body,
                                std::string_view key, std::span<const NodeDecl> nodes) {
  const auto reference = body[std::string{key}];
  if (!reference) return {};
  return check_reference(file, reference, key, nodes);
}

CheckResult check_node_references(const std::string& file, const YAML::Node& body,
                                  std::span<const NodeDecl> nodes) {
  if (auto checked = check_op_steps(file, body["ops"], nodes); !checked) return checked;
  if (auto checked = check_step_list(file, body["then"], nodes); !checked) return checked;
  for (const auto key : reference_keys) {
    if (auto checked = check_reference_key(file, body, key, nodes); !checked) return checked;
  }
  return {};
}

// A second pass over the node bodies, after all ids are known, so a node may refer to
// one declared further down the file.
CheckResult check_node_bodies(const std::string& file, const YAML::Node& nodes_yaml,
                              std::span<const NodeDecl> nodes) {
  for (const auto& entry : nodes_yaml) {
    if (auto checked = check_capacity(file, entry.second); !checked) return checked;
    if (auto checked = check_node_references(file, entry.second, nodes); !checked) return checked;
  }
  return {};
}

CheckResult check_link_end(const std::string& file, const YAML::Node& link, std::string_view end,
                           std::span<const NodeDecl> nodes) {
  const auto reference = link[std::string{end}];
  if (!reference) {
    return std::unexpected(error_at(file, link, std::format("link needs a '{}'", end)));
  }
  return check_reference(file, reference, "link", nodes);
}

std::expected<LinkDecl, ConfigError> read_link(const std::string& file, const YAML::Node& link,
                                               std::span<const NodeDecl> nodes) {
  if (auto from = check_link_end(file, link, "from", nodes); !from) {
    return std::unexpected(from.error());
  }
  if (auto to = check_link_end(file, link, "to", nodes); !to) return std::unexpected(to.error());
  return LinkDecl{link["from"].Scalar(), link["to"].Scalar()};
}

std::expected<std::vector<LinkDecl>, ConfigError> collect_links(const std::string& file,
                                                                const YAML::Node& links,
                                                                std::span<const NodeDecl> nodes) {
  std::vector<LinkDecl> collected;
  for (const auto& link : links) {
    auto read = read_link(file, link, nodes);
    if (!read) return std::unexpected(read.error());
    collected.push_back(std::move(*read));
  }
  return collected;
}

// Pattern: Validator pipeline — each check is a pure function (file, node) ->
// expected<..., ConfigError>, run in sequence; the first failure wins and is returned.
std::expected<Scenario, ConfigError> validate(const std::string& file, const YAML::Node& root) {
  if (auto checked = check_required_top_level_keys(file, root); !checked) {
    return std::unexpected(checked.error());
  }
  if (auto checked = check_version(file, root); !checked) return std::unexpected(checked.error());
  auto duration = parse_duration_key(file, root, "duration");
  if (!duration) return std::unexpected(duration.error());
  auto nodes = collect_node_declarations(file, root["nodes"]);
  if (!nodes) return std::unexpected(nodes.error());
  if (auto checked = check_node_bodies(file, root["nodes"], *nodes); !checked) {
    return std::unexpected(checked.error());
  }
  auto links = collect_links(file, root["links"], *nodes);
  if (!links) return std::unexpected(links.error());
  return Scenario{root["name"].Scalar(), root["version"].as<int>(), *duration, std::move(*nodes),
                  std::move(*links)};
}

}  // namespace simsys::config::detail

namespace simsys::config {

std::expected<Scenario, ConfigError> load(const std::string& file) {
  // The only exception boundary: yaml-cpp reports syntax errors (LoadFile) and shape
  // errors (subscripting a scalar, reading a missing node) by throwing; we return
  // std::expected, so callers never need a try/catch.
  try {
    return detail::validate(file, YAML::LoadFile(file));
  } catch (const YAML::Exception& e) {
    return std::unexpected(detail::error_at_mark(file, e.mark, e.msg));
  }
}

}  // namespace simsys::config
