// Module `simsys.config`, implementation unit: the checks behind `load`. yaml-cpp and
// <format> are included here, not in config.cppm (see the GCC 14.2 note there).
module;
#include <yaml-cpp/yaml.h>

#include <array>
#include <charconv>
#include <expected>
#include <format>
#include <functional>
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

constexpr std::array<std::string_view, 5> required_top_level_keys{"version", "name", "duration",
                                                                  "nodes", "links"};
constexpr std::array<std::string_view, 2> optional_top_level_durations{"step", "warmup"};
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

bool is_declared(const NodeIds& ids, std::string_view id) {
  // C++20 contains(): "is this key in the set?" in one call, replacing the
  // `find(key) != end()` comparison.
  return ids.contains(id);
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

// For durations a scenario may omit (step, warmup, latency): absent is fine, present must
// parse.
CheckResult check_optional_duration(const std::string& file, const YAML::Node& parent,
                                    std::string_view key) {
  if (!parent[std::string{key}]) return {};
  // C++23 std::expected::transform with a lambda that drops the value: only "valid or
  // not" matters here, so the result becomes std::expected<void, ConfigError>.
  return parse_duration_key(file, parent, key).transform([](SimTime) {});
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
  if (auto checked = check_version(file, root); !checked) return checked;
  for (const auto key : optional_top_level_durations) {
    if (auto checked = check_optional_duration(file, root, key); !checked) return checked;
  }
  return {};
}

// Capacity counts requests for most node types and bytes for storage ("500MB/s"). Only
// "valid or not" matters, so both parsers' values are dropped.
std::expected<void, ParseError> check_capacity_text(std::string_view text) {
  // C++20 std::string_view::ends_with: a suffix test without size arithmetic.
  if (text.ends_with("B/s")) return parse_byte_rate(text).transform([](ByteRate) {});
  return parse_rate(text).transform([](Rate) {});
}

CheckResult check_capacity(const std::string& file, const YAML::Node& body) {
  const auto capacity = body["capacity"];
  if (!capacity) return {};
  return scalar_value(file, body, "capacity").and_then([&](const std::string& text) {
    return check_capacity_text(text).transform_error(
        [&](const ParseError& error) { return unit_error(file, capacity, "capacity", error); });
  });
}

std::expected<model::NodeSpec, ConfigError> read_node_declaration(const std::string& file,
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
  if (!type) {
    return std::unexpected(
        error_at(file, body["type"], std::format("unknown node type '{}'", *type_name)));
  }
  // The other NodeSpec members keep the schema defaults declared in simsys.model; the
  // typed keys (replicas, capacity, ops, ...) are read in later steps.
  return model::NodeSpec{.id = id.Scalar(), .type = *type};
}

// The id goes into the set first, so a repeated id is reported before its body is read.
CheckResult add_node_declaration(const std::string& file, const YAML::Node& id,
                                 const YAML::Node& body, Declarations& declared) {
  if (id.Scalar().empty()) {
    return std::unexpected(error_at(file, id, "node id must not be empty"));
  }
  // set::insert returns {iterator, inserted}; `inserted` is false when the id was there.
  if (!declared.ids.insert(id.Scalar()).second) {
    return std::unexpected(error_at(file, id, std::format("duplicate node id '{}'", id.Scalar())));
  }
  auto declaration = read_node_declaration(file, id, body);
  if (!declaration) return std::unexpected(declaration.error());
  declared.nodes.push_back(std::move(*declaration));
  return {};
}

// yaml-cpp iterates a map in document order, so the declarations keep that order. It also
// keeps both entries of a repeated key, which is what lets duplicates be detected here.
std::expected<Declarations, ConfigError> collect_node_declarations(const std::string& file,
                                                                   const YAML::Node& nodes) {
  if (!nodes.IsMap() || nodes.size() == 0) {
    return std::unexpected(error_at(file, nodes, "nodes must be a non-empty map"));
  }
  Declarations declared;
  for (const auto& entry : nodes) {
    if (auto added = add_node_declaration(file, entry.first, entry.second, declared); !added) {
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

// True when the whole text is a number ("1", "0.5"): a traffic share. from_chars never
// throws and reports how far it got; the whole text must be consumed, so "0.5x" is no number.
bool is_number(std::string_view text) {
  double value{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
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

CheckResult check_node_references(const std::string& file, const YAML::Node& body,
                                  const NodeIds& ids) {
  if (auto checked = check_op_steps(file, body["ops"], ids); !checked) return checked;
  if (auto checked = check_then(file, body, ids); !checked) return checked;
  for (const auto key : reference_keys) {
    if (auto checked = check_reference_key(file, body, key, ids); !checked) return checked;
  }
  return {};
}

// The body is known to be a map: the declaration pass checked it.
CheckResult check_node_body(const std::string& file, const YAML::Node& body, const NodeIds& ids) {
  if (auto checked = check_capacity(file, body); !checked) return checked;
  if (auto checked = check_optional_duration(file, body, "latency"); !checked) return checked;
  return check_node_references(file, body, ids);
}

// A second pass over the node bodies, after all ids are known, so a node may refer to
// one declared further down the file.
CheckResult check_node_bodies(const std::string& file, const YAML::Node& nodes,
                              const NodeIds& ids) {
  for (const auto& entry : nodes) {
    if (auto checked = check_node_body(file, entry.second, ids); !checked) return checked;
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
  if (auto latency = check_optional_duration(file, link, "latency"); !latency) {
    return std::unexpected(latency.error());
  }
  return model::LinkSpec{.from = link["from"].Scalar(), .to = link["to"].Scalar()};
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
  auto name = scalar_value(file, root, "name");
  if (!name) return std::unexpected(name.error());
  // A null `name:` has the empty scalar too.
  if (name->empty()) {
    return std::unexpected(error_at(file, root["name"], "name must not be empty"));
  }
  auto duration = parse_duration_key(file, root, "duration");
  if (!duration) return std::unexpected(duration.error());
  auto declared = collect_node_declarations(file, root["nodes"]);
  if (!declared) return std::unexpected(declared.error());
  if (auto checked = check_node_bodies(file, root["nodes"], declared->ids); !checked) {
    return std::unexpected(checked.error());
  }
  auto links = collect_links(file, root["links"], declared->ids);
  if (!links) return std::unexpected(links.error());
  return model::Scenario{.name = std::move(*name),
                         .version = root["version"].as<int>(),
                         .duration = *duration,
                         .nodes = std::move(declared->nodes),
                         .links = std::move(*links)};
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
