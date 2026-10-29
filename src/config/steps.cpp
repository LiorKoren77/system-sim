// Module `simsys.config`, implementation unit: steps, how a node forwards an op. A service's or
// a gateway's `ops` (op -> step list) and the `then` steps after processing: each step's
// target, op, trigger (`on`), mode, factor and size.
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

// ---- Steps: how a node forwards an op (docs/scenario-schema.md, "step") ----------------

constexpr std::string_view step_keys = "to op on mode factor size";

CheckResult check_step_target(const StepScope& scope, const YAML::Node& step) {
  if (!step.IsMap()) return scope.site.fail(step, "step must be a map");
  if (!step["to"]) return scope.site.fail(step, "step needs a 'to'");
  return check_reference(scope.site, step, "to", "step", scope.ids);
}

// The op a step forwards: its own `op`, else the op that came in (a service's `read` goes on
// as `read`). A `then` step follows processing, not one named op, so it has nothing to
// inherit: `incoming_op` is empty and `op` is required.
std::expected<std::string, ConfigError> read_step_op(const Site& site, const YAML::Node& step,
                                                     std::optional<std::string_view> incoming_op) {
  if (step["op"]) {
    return check_not_null(site, step, "op", "step op").and_then([&] {
      return read_op_name(site, step["op"], "step op");
    });
  }
  if (incoming_op) return std::string{*incoming_op};
  return site.fail(step, "then step needs an 'op'");
}

// `size: 50MB`, optional: the payload the step's request carries (model::Step::size).
CheckResult read_step_size(const Site& site, const YAML::Node& yaml, model::Step& step) {
  if (!yaml["size"]) return {};
  return check_not_null(site, yaml, "size", "step size").and_then([&] {
    return read_size(site, yaml["size"], "step size").transform(store_in(step.size));
  });
}

// `on` gates the flow (a cache's `miss` steps see only what it did not serve, ch. 1), `mode`
// decides whether latencies add (sequential) or overlap (parallel: the max counts), `size` is the
// payload the request carries (spec §19 entry 1), `factor` multiplies the flow (fan-out on write:
// one post -> 100 feed writes, ch. 11).
CheckResult read_step_attributes(const Site& site, const YAML::Node& yaml, model::Step& step) {
  if (auto read = read_optional_word(site, yaml, "on", model::parse_step_condition,
                                     "step condition must be always, miss or failure", step.on);
      !read) {
    return read;
  }
  if (auto read = read_optional_word(site, yaml, "mode", model::parse_step_mode,
                                     "step mode must be sequential or parallel", step.mode);
      !read) {
    return read;
  }
  if (auto read = read_step_size(site, yaml, step); !read) return read;
  if (auto read = read_optional_number(site, yaml, "factor", is_positive,
                                       "step factor must be a finite positive number", step.factor);
      !read) {
    return read;
  }
  if (step.factor <= max_step_factor) return {};
  return site.fail(yaml["factor"], "step factor must be at most 1e6");
}

// A step back to its own node would re-inject its whole flow every time step: the fluid
// engine sees a feedback edge with factor >= 1, so the rate grows without bound. A retry,
// the one real reason to send to yourself, is `client.retries`. Checked last, so a step's
// other mistakes are reported first.
CheckResult check_not_own_node(const StepScope& scope, const YAML::Node& yaml,
                               const model::Step& step) {
  if (step.to != scope.node_id) return {};
  return scope.site.fail(yaml["to"], "a step may not target its own node");
}

std::expected<model::Step, ConfigError> read_step(const StepScope& scope, const YAML::Node& yaml,
                                                  std::optional<std::string_view> incoming_op) {
  if (auto checked = check_step_target(scope, yaml); !checked) {
    return std::unexpected(checked.error());
  }
  if (auto checked = check_known_keys(scope.site, yaml, step_keys, "step key"); !checked) {
    return std::unexpected(checked.error());
  }
  auto op = read_step_op(scope.site, yaml, incoming_op);
  if (!op) return std::unexpected(op.error());
  // C++20 designated initializers: each member named where it is set, so two members of the
  // same type cannot be swapped unnoticed as with positional aggregate initialization.
  model::Step step{.to = yaml["to"].Scalar(), .op = std::move(*op)};
  if (auto read = read_step_attributes(scope.site, yaml, step); !read) {
    return std::unexpected(read.error());
  }
  return check_not_own_node(scope, yaml, step).transform([&] { return std::move(step); });
}

std::expected<std::vector<model::Step>, ConfigError> read_step_list(
    const StepScope& scope, const YAML::Node& list, std::optional<std::string_view> incoming_op) {
  return collect_each(list,
                      [&](const YAML::Node& yaml) { return read_step(scope, yaml, incoming_op); });
}

// One entry of a forwarding node's `ops`: an op name and the step list it triggers. An
// empty list (`deliver: []`) is a terminal op: the node handles it and forwards nothing.
CheckResult read_op_entry(const StepScope& scope, const YamlElement& entry, model::NodeSpec& node) {
  auto name = read_op_name(scope.site, entry.first, "op name");
  if (!name) return std::unexpected(name.error());
  const auto label = std::format("op '{}'", *name);
  if (auto checked = check_entry_not_null(scope.site, entry, label, empty_list); !checked) {
    return checked;
  }
  if (!is_sequence(entry.second)) {
    return scope.site.fail(entry.second, std::format("op '{}' must be a step list", *name));
  }
  return read_step_list(scope, entry.second, *name).transform([&](std::vector<model::Step> steps) {
    node.ops.emplace(std::move(*name), std::move(steps));
  });
}

// A forwarding node's `ops`: incoming op name -> the steps it triggers.
CheckResult read_op_steps(const StepScope& scope, const YAML::Node& body, model::NodeSpec& node) {
  const auto ops = body["ops"];
  if (!ops) return {};
  if (auto checked = check_not_null(scope.site, body, "ops", "ops", empty_map); !checked) {
    return checked;
  }
  if (!ops.IsMap()) return scope.site.fail(ops, "ops must be a map");
  for (const auto& entry : ops) {
    if (auto read = read_op_entry(scope, entry, node); !read) return read;
  }
  return {};
}

// `then`: the steps after a node has processed its work (a worker's follow-up writes, a
// cache's misses going on to the database).
CheckResult read_then(const StepScope& scope, const YAML::Node& body, model::NodeSpec& node) {
  const auto then = body["then"];
  if (!then) return {};
  if (auto checked = check_not_null(scope.site, body, "then", "then", empty_list); !checked) {
    return checked;
  }
  if (!then.IsSequence()) return scope.site.fail(then, "then must be a list");
  return read_step_list(scope, then, std::nullopt).transform(store_in(node.then));
}

}  // namespace simsys::config::detail
