// Module `simsys.config`, implementation unit: the `slo` list. Each objective's metric, node,
// op, maximum and window, checked against the declared nodes, the clients' ops and the run's
// header.
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

// ---- SLOs -----------------------------------------------------------------------------
//
// An SLO is a service-level objective, the "non-functional requirements" step of the
// book's framework (ch. 3): a bound a design must keep, checked against the simulated run.
// Latency percentiles are measured end to end for one client (`node`), optionally for one
// op; drop rate and utilization are ratios; queue lag (how far consumers trail producers)
// and staleness (the age of the data served) are times.

// Unnamed namespace: the helpers in these blocks are this unit's own (internal linkage).
// Outside it a function of a module implementation unit has module linkage, so every unit
// of simsys.config could call it and the linker sees its name; only what detail.hpp declares
// is shared between the units.
namespace {

constexpr std::string_view slo_keys = "metric node op max over";

using SloMaximum = decltype(model::SloSpec::max);

bool is_latency_percentile(model::SloMetric metric) {
  return metric == model::SloMetric::p50_latency || metric == model::SloMetric::p95_latency ||
         metric == model::SloMetric::p99_latency;
}

bool is_time_metric(model::SloMetric metric) {
  return is_latency_percentile(metric) || metric == model::SloMetric::queue_lag ||
         metric == model::SloMetric::staleness;
}

std::expected<model::SloMetric, ConfigError> read_slo_metric(const Site& site,
                                                             const YAML::Node& entry) {
  const auto value = entry["metric"];
  if (!value) return site.fail(entry, "entry needs a metric");
  if (auto checked = check_not_null(site, entry, "metric", "metric"); !checked) {
    return std::unexpected(checked.error());
  }
  const auto name = scalar_text(value);
  if (const auto metric = model::parse_slo_metric(name)) return *metric;
  return site.fail(value, std::format("unknown metric '{}'", name));
}

CheckResult read_slo_node(const Site& site, const YAML::Node& entry, const NodeIndex& ids,
                          std::optional<std::string>& node) {
  if (!entry["node"]) return {};
  auto id = scalar_value(site, entry, "node");
  if (!id) return std::unexpected(id.error());
  if (!is_declared(ids, *id))
    return site.fail(entry["node"], std::format("unknown node '{}'", *id));
  node = std::move(*id);
  return {};
}

CheckResult read_slo_op(const Site& site, const YAML::Node& entry, std::optional<std::string>& op) {
  if (!entry["op"]) return {};
  if (auto checked = check_not_null(site, entry, "op", "op name"); !checked) return checked;
  return read_op_name(site, entry["op"], "op name").transform(store_in(op));
}

// The variant's alternative follows the metric: a SimTime for the time metrics, a double
// for the ratios, so "0.01" can never be read as 0.01 seconds.
std::expected<SloMaximum, ConfigError> read_slo_max(const Site& site, const YAML::Node& entry,
                                                    model::SloMetric metric) {
  const auto name = model::to_string(metric);
  const auto value = entry["max"];
  if (!value) return site.fail(entry, std::format("{} needs a max", name));
  if (auto checked = check_not_null(site, entry, "max", "max"); !checked) {
    return std::unexpected(checked.error());
  }
  if (is_time_metric(metric)) {
    if (const auto time = parse_duration(scalar_text(value))) return SloMaximum{*time};
    return site.fail(value, std::format("{} max must be a duration", name));
  }
  const auto number = number_value(value);
  if (number && is_ratio(*number)) return SloMaximum{*number};
  return site.fail(value, std::format("{} max must be between 0 and 1", name));
}

bool offers_op(const model::NodeSpec& client, std::string_view op) {
  return client.workload && sends_op(*client.workload, op);
}

// A latency percentile is measured over requests, and requests start at a client: only a
// client's end-to-end response time is reported, so on any other node, or for an op the
// client never sends, the SLO could never fail. Without `op` it covers all the client's ops.
CheckResult check_latency_target(const Site& site, const YAML::Node& entry,
                                 const model::SloSpec& slo, const Declarations& declared) {
  if (!is_latency_percentile(slo.metric)) return {};
  const auto metric = model::to_string(slo.metric);
  if (!slo.node) return site.fail(entry, std::format("{} needs a node", metric));
  const auto& node = declared.nodes[declared.ids.find(*slo.node)->second];
  if (node.type != model::NodeType::client) {
    return site.fail(entry["node"], std::format("{} needs a client node, '{}' is a {}", metric,
                                                node.id, model::to_string(node.type)));
  }
  if (!slo.op || offers_op(node, *slo.op)) return {};
  return site.fail(entry["op"], std::format("client '{}' has no op '{}'", node.id, *slo.op));
}

// Utilization is a server's busy fraction: it needs a node to name, and a client, a pure
// source with no service rate, is never busy, so the SLO could never fail on one.
CheckResult check_utilization_target(const Site& site, const YAML::Node& entry,
                                     const model::SloSpec& slo, const Declarations& declared) {
  if (slo.metric != model::SloMetric::utilization) return {};
  if (!slo.node) return site.fail(entry, "utilization needs a node");
  const auto& node = declared.nodes[declared.ids.find(*slo.node)->second];
  if (node.type != model::NodeType::client) return {};
  return site.fail(entry["node"],
                   std::format("utilization needs a server node, '{}' is a client", node.id));
}

// A queue lag is a message queue's (spec §19 entry 14): how far its slowest consumer group trails
// it, read off the worker pools behind it, so on a service, a client or any other node the SLO
// could never be observed. A frontier is accepted too: it is the web crawler's queue (ch. 9),
// drained by worker pools as a message queue is, and its lag is read off them the same way (phase
// 3 Task 18). Without `node` the SLO bounds every queue of the run, frontiers included: the worst
// one decides.
CheckResult check_queue_lag_target(const Site& site, const YAML::Node& entry,
                                   const model::SloSpec& slo, const Declarations& declared) {
  if (slo.metric != model::SloMetric::queue_lag || !slo.node) return {};
  const auto& node = declared.nodes[declared.ids.find(*slo.node)->second];
  if (node.type == model::NodeType::message_queue || node.type == model::NodeType::frontier) {
    return {};
  }
  return site.fail(entry["node"], std::format("queue_lag needs a message_queue node ('{}' is a {})",
                                              node.id, model::to_string(node.type)));
}

// A dead-letter rate is a worker pool's (phase 3 spec §6.2): its dead letters over the jobs
// published to the queue it consumes (report::Summary::dead_letter_rate). Only a pool parks jobs
// that ran out of retries, so the SLO names one: on any other node it could never be observed, and
// unlike queue_lag it has no "worst of the run" form (each pool's retries are its own design).
CheckResult check_dead_letter_target(const Site& site, const YAML::Node& entry,
                                     const model::SloSpec& slo, const Declarations& declared) {
  if (slo.metric != model::SloMetric::dead_letter_rate) return {};
  if (!slo.node) return site.fail(entry, "dead_letter_rate needs a node");
  const auto& node = declared.nodes[declared.ids.find(*slo.node)->second];
  if (node.type == model::NodeType::worker_pool) return {};
  return site.fail(entry["node"],
                   std::format("dead_letter_rate needs a worker_pool node ('{}' is a {})", node.id,
                               model::to_string(node.type)));
}

// Only latency is measured per op (a client's response time for each op it sends); every
// other metric is node-wide, so an `op` on it would be silently ignored.
CheckResult check_op_is_for_latency(const Site& site, const YAML::Node& entry,
                                    const model::SloSpec& slo) {
  if (!slo.op || is_latency_percentile(slo.metric)) return {};
  return site.fail(entry["op"], "op is only valid for latency metrics");
}

// What each metric's `node` and `op` may be, checked once the whole entry is read.
CheckResult check_slo_shape(const Site& site, const YAML::Node& entry, const model::SloSpec& slo,
                            const Declarations& declared) {
  if (auto checked = check_latency_target(site, entry, slo, declared); !checked) return checked;
  if (auto checked = check_utilization_target(site, entry, slo, declared); !checked) {
    return checked;
  }
  if (auto checked = check_queue_lag_target(site, entry, slo, declared); !checked) return checked;
  if (auto checked = check_dead_letter_target(site, entry, slo, declared); !checked) {
    return checked;
  }
  return check_op_is_for_latency(site, entry, slo);
}

// `over: 10m`: the SLO is checked on every window of that length after warmup, and the worst window
// decides (report::Summary). A window must hold a whole number of steps, at least one: the run
// moves a step at a time and a step belongs to the window its start falls in, so a shorter window
// would observe nothing, and `over: 1500ms` at `step: 1s` would make windows of two steps and one
// step in turn (the rule a windowed rate limiter's `window` follows,
// graph::window_off_the_step_grid). And at most the observed run, duration − warmup: a longer one
// would hold the whole run as one window shorter than `over`, which the report would call a window
// of `over` it never filled. (A run whose observed part is not a whole number of windows still ends
// with a shorter window; that one is checked and labelled with its real length.)
CheckResult read_slo_over(const Site& site, const YAML::Node& entry, const model::Scenario& run,
                          std::optional<SimTime>& over) {
  if (!entry["over"]) return {};
  auto window = parse_key(site, entry, "over", "over (a duration: 10m, 1h)", parse_duration);
  if (!window) return std::unexpected(window.error());
  if (window->count() < run.step.count()) {
    return site.fail(entry["over"],
                     std::format("over must be at least one step ({} < step {})",
                                 describe_duration(*window), describe_duration(run.step)));
  }
  if (window->count() % run.step.count() != 0) {
    return site.fail(entry["over"],
                     std::format("over must be a whole number of steps ({}, step {})",
                                 describe_duration(*window), describe_duration(run.step)));
  }
  const SimTime observed{run.duration.count() - run.warmup.count()};
  if (window->count() > observed.count()) {
    return site.fail(entry["over"],
                     std::format("over must fit in the run after warmup ({} > duration {} - "
                                 "warmup {})",
                                 describe_duration(*window), describe_duration(run.duration),
                                 describe_duration(run.warmup)));
  }
  over = *window;
  return {};
}

std::expected<model::SloSpec, ConfigError> read_slo(const Site& site, const YAML::Node& entry,
                                                    const Declarations& declared,
                                                    const model::Scenario& run) {
  if (!entry.IsMap()) return site.fail(entry, "entry must be a map");
  if (auto checked = check_known_keys(site, entry, slo_keys, "key"); !checked) {
    return std::unexpected(checked.error());
  }
  auto metric = read_slo_metric(site, entry);
  if (!metric) return std::unexpected(metric.error());
  // C++20 designated initializers: each member named where it is set, so two members of the
  // same type cannot be swapped unnoticed as with positional aggregate initialization.
  model::SloSpec slo{.metric = *metric};
  if (auto read = read_slo_node(site, entry, declared.ids, slo.node); !read) {
    return std::unexpected(read.error());
  }
  if (auto read = read_slo_op(site, entry, slo.op); !read) return std::unexpected(read.error());
  if (auto read = read_slo_max(site, entry, slo.metric).transform(store_in(slo.max)); !read) {
    return std::unexpected(read.error());
  }
  if (auto read = read_slo_over(site, entry, run, slo.over); !read) {
    return std::unexpected(read.error());
  }
  return check_slo_shape(site, entry, slo, declared).transform([&] { return std::move(slo); });
}

}  // namespace

// `run`: the scenario's header (duration, step, warmup), which bounds an SLO's `over`.
std::expected<std::vector<model::SloSpec>, ConfigError> collect_slos(const Site& site,
                                                                     const YAML::Node& root,
                                                                     const Declarations& declared,
                                                                     const model::Scenario& run) {
  const auto list = root["slo"];
  if (!list) return std::vector<model::SloSpec>{};
  if (!list.IsSequence()) return site.fail(list, "slo must be a list");
  const auto entry_site = site.within("slo");
  return collect_each(
      list, [&](const YAML::Node& entry) { return read_slo(entry_site, entry, declared, run); });
}

}  // namespace simsys::config::detail
