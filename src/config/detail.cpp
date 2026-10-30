// Module `simsys.config`, implementation unit: the definitions of the functions detail.hpp
// declares and documents (Site's members, the range rules, the common readers), compiled once
// here instead of once per unit that includes the header.
module;
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
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

bool is_positive(double v) {
  return std::isfinite(v) && v > 0;
}

bool is_non_negative(double v) {
  return std::isfinite(v) && v >= 0;
}

bool is_zero_or_more(double v) {
  return v >= 0;
}

bool is_unit_fraction(double v) {
  return v > 0 && v <= 1;
}

bool is_ratio(double v) {
  return v >= 0 && v <= 1;
}

ConfigError error_at_mark(const std::string& file, const YAML::Mark& mark, std::string message) {
  // C++20 designated initializers: each member is named at the point of construction,
  // so swapping `line` and `column` (both int) cannot go unnoticed as it could with
  // positional aggregate initialization.
  return ConfigError{.file = file,
                     .line = mark.line + 1,
                     .column = mark.column + 1,
                     .message = message,
                     .rule = message};
}

ConfigError error_at(const std::string& file, const YAML::Node& node, std::string message) {
  return error_at_mark(file, node.Mark(), std::move(message));
}

ConfigError Site::error(const YAML::Node& at, std::string_view what) const {
  // C++20 std::format: type-checked (at compile time) formatting that returns a
  // std::string, replacing std::ostringstream chains and snprintf buffers.
  if (where.empty()) return error_at(file, at, std::string{what});
  auto error = error_at(file, at, std::format("{}: {}", where, what));
  error.rule = what;
  return error;
}

std::unexpected<ConfigError> Site::fail(const YAML::Node& at, std::string_view what) const {
  return std::unexpected(error(at, what));
}

Site Site::within(std::string inner) const {
  return Site{.file = file, .where = std::move(inner)};
}

bool is_declared(const NodeIndex& ids, std::string_view id) {
  // C++20 contains(): "is this key in the map?" in one call, replacing the
  // `find(key) != end()` comparison.
  return ids.contains(id);
}

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

bool is_sequence(const YAML::Node& node) {
  return node.IsDefined() && node.IsSequence();
}

bool is_null(const YAML::Node& node) {
  return node.IsDefined() && node.IsNull();
}

std::string scalar_text(const YAML::Node& node) {
  return node.IsScalar() ? node.Scalar() : std::string{};
}

std::string number_text(const YAML::Node& node) {
  return node.IsScalar() && node.Tag() == "?" ? node.Scalar() : std::string{};
}

double rounded_out_of_range(std::string_view text) {
  return std::strtod(std::string{text}.c_str(), nullptr);
}

std::optional<double> parse_yaml_double(std::string_view text) {
  const auto value = parse_whole<double>(text);
  if (value) return std::isfinite(*value) ? std::optional{*value} : std::nullopt;
  if (value.error() == NumberFault::out_of_range) return rounded_out_of_range(text);
  return std::nullopt;
}

std::optional<double> number_value(const YAML::Node& node) {
  return parse_yaml_double(number_text(node));
}

// YAML's spellings of not-a-number and infinity (YAML 1.2 core schema). They are numbers in
// YAML, just not finite ones, so a key answers them with its range message, not "must be a
// number".
constexpr std::array<std::string_view, 12> yaml_special_floats{".nan",  ".NaN",  ".NAN",  ".inf",
                                                               ".Inf",  ".INF",  "+.inf", "+.Inf",
                                                               "+.INF", "-.inf", "-.Inf", "-.INF"};

// Is `node` written as a number at all, finite or not, in range or not? A plain scalar
// that reads whole as a decimal (1e309 included, which is out of range, not "no number"),
// or one of YAML's NaN/infinity spellings. A quoted "0.5", a word, a list or a map is not.
bool is_written_as_number(const YAML::Node& node) {
  const auto text = number_text(node);
  if (text.empty()) return false;
  const auto parsed = parse_whole<double>(text);
  // C++23 std::ranges::contains: "is this value in the range?" in one call, replacing
  // `std::ranges::find(r, v) != r.end()`, which spells the question as an iterator comparison.
  return parsed || parsed.error() == NumberFault::out_of_range ||
         std::ranges::contains(yaml_special_floats, std::string_view{text});
}

YAML::Node key_node(const YAML::Node& parent, std::string_view key) {
  for (const auto& entry : parent) {
    if (entry.first.IsScalar() && entry.first.Scalar() == key) return entry.first;
  }
  return parent;
}

YAML::Node shape_site(const YAML::Node& parent, std::string_view key) {
  const auto value = parent[std::string{key}];
  return is_null(value) ? key_node(parent, key) : value;
}

YAML::Node shape_site(const YamlElement& entry) {
  return entry.second.IsNull() ? entry.first : entry.second;
}

std::string empty_message(std::string_view label, std::string_view none) {
  if (none.empty()) return std::format("{} must not be empty", label);
  return std::format("{} must not be empty (write {} for none)", label, none);
}

CheckResult check_not_null(const Site& site, const YAML::Node& parent, std::string_view key,
                           std::string_view label, std::string_view none) {
  if (!is_null(parent[std::string{key}])) return {};
  return site.fail(shape_site(parent, key), empty_message(label, none));
}

CheckResult check_entry_not_null(const Site& site, const YamlElement& entry, std::string_view label,
                                 std::string_view none) {
  if (!entry.second.IsNull()) return {};
  return site.fail(shape_site(entry), empty_message(label, none));
}

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

ConfigError unit_error(const Site& site, const YAML::Node& value, std::string_view key,
                       const ParseError& error) {
  return site.error(value, std::format("{}: {}", key, error.message));
}

std::expected<SimTime, ConfigError> parse_duration_key(const Site& site, const YAML::Node& parent,
                                                       std::string_view key) {
  return parse_key(site, parent, key, key, parse_duration);
}

std::expected<std::string, ConfigError> read_op_name(const Site& site, const YAML::Node& name,
                                                     std::string_view label) {
  if (name.IsScalar() && !name.Scalar().empty()) return name.Scalar();
  if (name.IsMap() || name.IsSequence()) {
    return site.fail(name, std::format("{} must be a scalar", label));
  }
  return site.fail(name, std::format("{} must not be empty", label));
}

std::expected<std::string, ConfigError> key_name(const Site& site, const YAML::Node& key,
                                                 std::string_view noun) {
  if (key.IsMap() || key.IsSequence()) {
    return site.fail(key, std::format("{} must be a scalar", noun));
  }
  return scalar_text(key);
}

Site node_site(const Site& site, std::string_view id) {
  return site.within(std::format("node '{}'", id));
}

std::string unknown_node_message(std::string_view what, std::string_view id) {
  if (what.empty()) return std::format("refers to unknown node '{}'", id);
  return std::format("{} refers to unknown node '{}'", what, id);
}

CheckResult check_reference(const Site& site, const YAML::Node& parent, std::string_view key,
                            std::string_view what, const NodeIndex& ids) {
  const auto reference = parent[std::string{key}];
  return scalar_value(site, parent, key).and_then([&](const std::string& id) -> CheckResult {
    if (is_declared(ids, id)) return {};
    return site.fail(reference, unknown_node_message(what, id));
  });
}

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

std::expected<Bytes, ConfigError> read_size(const Site& site, const YAML::Node& value,
                                            std::string_view label) {
  const auto size = parse_size(scalar_text(value));
  if (!size) {
    return site.fail(value, std::format("{} must be a size (B, KB, MB or GB: 50MB): {}", label,
                                        size_error_reason(size.error())));
  }
  if (!is_positive(size->count)) return site.fail(value, std::format("{} must be positive", label));
  return *size;
}

// The most retries a client or a worker pool may state. Chosen, not from the book: ten extra
// attempts already let a failing path multiply the load by 11 (1 + 10 attempts per request), the
// engine keeps one bucket per retry, and a larger number is a typo rather than a client or a
// worker anyone ships.
inline constexpr int max_retries = 10;

// `retries: 2` (model::Workload::retries, model::NodeSpec::retries): a whole number from 0 to
// max_retries. A count beyond an int is too many as well; a negative number, a fraction, a word or
// a quoted "2" is no count (number_text: a quoted scalar is a string).
std::expected<int, ConfigError> read_retry_count(const Site& site, const YAML::Node& value) {
  const auto text = number_text(value);
  const auto count = parse_whole<int>(text);
  const bool beyond_an_int = !count && count.error() == NumberFault::out_of_range && text[0] != '-';
  if (beyond_an_int || (count && *count > max_retries)) {
    return site.fail(value, std::format("retries must be at most {}", max_retries));
  }
  if (!count || *count < 0) return site.fail(value, "retries must be a non-negative integer");
  return *count;
}

// `retry_after: 250ms`: a positive wait, and only beside `retries`: a backoff with nothing to retry
// is a mistake the author should hear about, as a `worker_pool` without `workers` is. The sign is
// read first, so that `-1s` gets the same message as `0s` (the duration reader would say "expected
// a non-negative finite number", which does not tell that 0 is refused too).
CheckResult read_retry_after(const Site& site, const YAML::Node& body,
                             std::optional<SimTime>& retry_after) {
  const auto value = body["retry_after"];
  // C++20 std::string::starts_with: a prefix test in one call, replacing `s.rfind("-", 0) == 0`.
  if (value && value.IsScalar() && value.Scalar().starts_with('-')) {
    return site.fail(value, "retry_after must be positive");
  }
  if (auto read = read_optional_duration(site, body, "retry_after", retry_after); !read) {
    return read;
  }
  if (!retry_after) return {};
  if (retry_after->count() <= 0) return site.fail(value, "retry_after must be positive");
  if (!body["retries"]) return site.fail(body["retry_after"], "retry_after needs retries");
  return {};
}

}  // namespace simsys::config::detail
