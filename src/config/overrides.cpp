// Module `simsys.config`, implementation unit: overrides, `simsys sweep`'s replaced values. A
// dot path (`nodes.users.rate.base`) leads through the parsed YAML's maps to one scalar, which
// is replaced before validation; the questions the CLI asks of a path before any file is read
// (dot_path_problem, counts_whole_things) are answered here too.
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

// ---- Overrides (`simsys sweep`) ----
// An override replaces one scalar of the parsed document before `validate` sees it, so the
// replaced text meets the same checks as text written in the file. It addresses the scalar by
// map keys only: a list entry has no name a path could say (`links.0` would depend on the order
// of the file's lines), so a path through a list is refused, not guessed.

// A mistake in an override's path: on the command line, so it has no line in the file.
std::unexpected<ConfigError> override_error(const std::string& file, std::string_view path,
                                            std::string_view what) {
  // C++20 designated initializers: each member named where it is set, so two members of the
  // same type cannot be swapped unnoticed as with positional aggregate initialization.
  return std::unexpected(ConfigError{.file = file,
                                     .line = 0,
                                     .column = 0,
                                     .message = std::format("--param: {}: {}", path, what),
                                     .rule = std::string{what}});
}

// Why a dot path cannot enter a list, in every message that says so (here and dot_path_problem).
constexpr std::string_view maps_only = "a dot path goes through maps only";

// Why `key` cannot be looked up in `parent`, the node at `walked` ("" for the document), or
// nothing when `parent` is a map.
std::optional<std::string> not_a_map(const YAML::Node& parent, std::string_view walked) {
  if (parent.IsMap()) return std::nullopt;
  const auto name = walked.empty() ? std::string{"the document"} : std::string{walked};
  if (parent.IsSequence()) return name + " is a list; " + std::string{maps_only};
  return name + " is a single value, it has no keys";
}

bool is_all_digits(std::string_view text) {
  // C++20 ranges algorithm: std::ranges::all_of takes the range itself, not a begin/end
  // iterator pair.
  return !text.empty() && std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; });
}

// The node at `path` in `root`, or why there is none. Only const lookups: yaml-cpp's
// non-const `node[key]` inserts a missing key, which would turn a typo into a new key.
std::expected<YAML::Node, ConfigError> find_override_target(const std::string& file,
                                                            const YAML::Node& root,
                                                            std::string_view path) {
  YAML::Node current = root;
  std::string walked;
  for (const auto& key : dot_path_keys(path)) {
    if (const auto refused = not_a_map(current, walked)) {
      return override_error(file, path, *refused);
    }
    const YAML::Node& parent = current;
    const YAML::Node next = parent[key];
    if (!next) return override_error(file, path, "no such key in the scenario");
    // yaml-cpp's `current = next` would not move the handle: it rebinds the node `current`
    // refers to, rewriting the document. reset() moves the handle and leaves the document alone.
    current.reset(next);
    walked += walked.empty() ? key : "." + key;
  }
  if (!current.IsScalar()) {
    return override_error(file, path, "not a single value (a map or a list)");
  }
  return current;
}

// Replaces each override's scalar in `root`, in order; the first bad path stops.
// C++20 std::span<const T>: a non-owning view of contiguous elements, so a vector, an array or
// `{}` of overrides is read without a copy.
std::expected<void, ConfigError> apply_overrides(const std::string& file, YAML::Node& root,
                                                 std::span<const Override> overrides) {
  for (const auto& replacement : overrides) {
    auto target = find_override_target(file, root, replacement.path);
    if (!target) return std::unexpected(target.error());
    // Assigning text to a scalar keeps the node's tag: a plain `400` stays a plain scalar, which
    // the number readers require (number_text), and a quoted one stays quoted.
    *target = replacement.value;
  }
  return {};
}

}  // namespace simsys::config::detail

namespace simsys::config {

// A loop, not views::split | views::transform: a lambda passed to a range adaptor in a module is
// emitted twice by GCC 14.2 (CLAUDE.md §3).
std::vector<std::string> dot_path_keys(std::string_view path) {
  std::vector<std::string> keys{std::string{}};
  for (const char c : path) {
    if (c == '.') {
      keys.emplace_back();
    } else {
      keys.back() += c;
    }
  }
  return keys;
}

std::optional<std::string> dot_path_problem(std::string_view path) {
  for (const auto& key : dot_path_keys(path)) {
    if (key.empty()) return std::string{path} + ": a dot path has no empty keys";
    if (detail::is_all_digits(key)) {
      return std::string{path} + ": '" + key + "' would index a list; " +
             std::string{detail::maps_only} + " (a list entry has no name)";
    }
  }
  return std::nullopt;
}

bool counts_whole_things(std::string_view override_path) {
  const auto keys = dot_path_keys(override_path);
  const auto& key = keys.back();
  if (keys.size() == 1) return detail::lists_word(detail::top_level_integer_keys, key);
  if (keys.size() < 3 || keys[0] != "nodes") return false;
  if (keys.size() == 3) return detail::lists_word(detail::node_integer_keys, key);
  if (keys.size() != 4) return false;
  if (keys[2] == "advanced") return detail::lists_word(detail::advanced_integer_keys, key);
  return keys[2] == "keys" && detail::lists_word(detail::client_keys_integer_keys, key);
}

}  // namespace simsys::config
