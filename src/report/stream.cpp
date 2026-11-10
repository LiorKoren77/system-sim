// Implementation unit of `simsys.report`: the frames of `simsys run --stream` (FrameAggregator, the
// rules in report.cppm's "frames" section) and, later in this file, the stream's lines. A unit of
// simsys.report rather than a module of its own: a bucket's p99 is read by the same rule as a run's
// (one rule, in the :shared partition), and the lines use the report's JSON primitives.
//
// Models: nothing new. A frame is a coarser view of the engine's steps, and each aggregate is
// chosen so that the fluid queue's conservation law (spec §3.3) still balances over a bucket.
module;
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

module simsys.report;

// C++20 `import :shared;`: the partition of this module that holds what its units share (the JSON
// primitives, the number formatting).
import :shared;

// C++20 `import`: the compiled interfaces of the modules this unit reads from.
import simsys.core;
import simsys.engine;
import simsys.fluid;
import simsys.graph;
import simsys.model;

namespace simsys::report {

// Defaulted out of line, in this unit (report.cppm's Frame says why).
Frame::Frame() = default;
Frame::Frame(const Frame& other) = default;
Frame::Frame(Frame&& other) noexcept = default;
Frame& Frame::operator=(const Frame& other) = default;
Frame& Frame::operator=(Frame&& other) noexcept = default;
Frame::~Frame() = default;

FrameAggregator::FrameAggregator(const graph::Graph& g, std::size_t steps_per_bucket,
                                 std::function<void(const Frame&)> sink)
    : graph_{&g},
      steps_per_bucket_{steps_per_bucket},
      sink_{std::move(sink)},
      step_s_{seconds(g.scenario().step)} {
  if (steps_per_bucket_ == 0) {
    throw std::invalid_argument{"report::FrameAggregator: a bucket needs at least one step"};
  }
}

FrameAggregator::~FrameAggregator() = default;

void FrameAggregator::observe(const engine::StepRecord& record) {
  if (steps_ == 0) start_bucket(record);
  for (std::size_t i = 0; i < nodes_.size() && i < record.nodes.size(); ++i) {
    add_node(nodes_[i], record.nodes[i]);
  }
  for (std::size_t i = 0; i < links_.size() && i < record.links.size(); ++i) {
    add_link(links_[i], record.links[i]);
  }
  for (std::size_t j = 0; j < ops_.size() && j < record.responses.size(); ++j) {
    add_op(ops_[j], record.responses[j], step_s_);
  }
  if (++steps_ == steps_per_bucket_) hand_over();
}

void FrameAggregator::finish() {
  if (steps_ > 0) hand_over();
}

// A fresh bucket starts at this record's time, with one empty sum per node and per pipe the record
// carries.
void FrameAggregator::start_bucket(const engine::StepRecord& record) {
  start_ = record.t;
  nodes_.assign(record.nodes.size(), QueueSums{});
  links_.assign(record.links.size(), QueueSums{});
  ops_.clear();
  for (const auto& response : record.responses) ops_.push_back(OpSums{.client = response.client});
}

namespace {

// C++23 std::to_underlying: an enumerator's value in the enum's underlying type, replacing
// `static_cast<std::underlying_type_t<E>>(e)`, which repeats the type and silently changes meaning
// if the enum's underlying type does. A zone's number is its order: green 0, yellow 1, red 2, so
// the worst zone is the largest number (docs/study/small-features.md#stdto_underlying).
[[nodiscard]] int zone_number(fluid::Zone zone) {
  return static_cast<int>(std::to_underlying(zone));
}

// x·dt added to an optional sum: the sum appears with the first step that reports the column.
void add_optional(std::optional<double>& sum, const std::optional<double>& value, double dt_s) {
  if (value) sum = sum.value_or(0.0) + *value * dt_s;
}

}  // namespace

// The op's rates as Summary sums them, and its response into the bucket's own mixtures
// (ResponseMoments::add: weighted by the requests answered and delivered).
void FrameAggregator::add_op(OpSums& sums, const engine::ResponseSample& sample, double dt_s) {
  sums.offered += sample.offered.per_second * dt_s;
  sums.attempts += sample.attempts.per_second * dt_s;
  sums.answered += sample.answered.per_second * dt_s;
  sums.timed_out += sample.timed_out.per_second * dt_s;
  sums.timed_out_attempts += sample.timed_out_attempts.per_second * dt_s;
  sums.response.add(sample, dt_s);
}

void FrameAggregator::add_node(QueueSums& sums, const engine::NodeSample& sample) const {
  const double dt = step_s_;
  sums.demand += sample.demand_per_s * dt;
  sums.drain += sample.drain_per_s * dt;
  sums.dropped += sample.dropped_per_s * dt;
  sums.rejected += sample.rejected_per_s * dt;
  sums.rho += sample.rho * dt;
  sums.backlog = sample.backlog;
  sums.peak_backlog = std::max(sums.peak_backlog, sample.backlog);
  sums.peak_rho = std::max(sums.peak_rho, sample.rho);
  sums.zone = std::max(sums.zone, zone_number(sample.zone));
  if (sample.zone == fluid::Zone::red) sums.red_s += dt;
  if (std::isfinite(sample.latency.mean_s)) {
    sums.latency_by_drain += sample.latency.mean_s * sample.drain_per_s * dt;
    sums.latency_by_time += sample.latency.mean_s * dt;
  } else {
    sums.unanswered = true;
  }
  if (sample.queue_lag_s) {
    sums.queue_lag_s = std::max(sums.queue_lag_s.value_or(0.0), *sample.queue_lag_s);
  }
  if (sample.connections) sums.connections = sample.connections;
  add_optional(sums.dead_lettered, sample.dead_lettered_per_s, dt);
  add_optional(sums.deduplicated, sample.deduplicated_per_s, dt);
}

// A pipe has a node's queue numbers and none of its type-specific ones (engine::LinkSample).
void FrameAggregator::add_link(QueueSums& sums, const engine::LinkSample& sample) const {
  add_node(sums, engine::NodeSample{.demand_per_s = sample.demand_per_s,
                                    .drain_per_s = sample.drain_per_s,
                                    .dropped_per_s = sample.dropped_per_s,
                                    .backlog = sample.backlog,
                                    .rho = sample.rho,
                                    .latency = sample.latency,
                                    .zone = sample.zone});
}

// The row of a bucket of `steps_` steps: each sum over its length, each peak and level as kept. The
// latency is weighted by the requests each step served (the sojourn an average request of the
// bucket saw), or by time when the bucket served nothing.
NodeRow FrameAggregator::node_row(const QueueSums& sums, bool client) const {
  if (client) return {};  // a source has no queue: its rates are its op rows
  const double length = step_s_ * static_cast<double>(steps_);
  const auto mean = [&](double sum) { return sum / length; };
  const auto mean_of = [&](const std::optional<double>& sum) -> std::optional<double> {
    if (!sum) return std::nullopt;
    return *sum / length;
  };
  std::optional<double> latency_ms{};
  if (!sums.unanswered) {
    const double latency_s =
        sums.drain > 0.0 ? sums.latency_by_drain / sums.drain : sums.latency_by_time / length;
    latency_ms = latency_s * 1000.0;
  }
  return NodeRow{.demand_per_s = mean(sums.demand),
                 .drain_per_s = mean(sums.drain),
                 .dropped_per_s = mean(sums.dropped),
                 .rejected_per_s = mean(sums.rejected),
                 .backlog = sums.backlog,
                 .peak_backlog = sums.peak_backlog,
                 .mean_rho = mean(sums.rho),
                 .peak_rho = sums.peak_rho,
                 .zone = sums.zone,
                 .red_s = sums.red_s,
                 .latency_ms = latency_ms,
                 .queue_lag_s = sums.queue_lag_s,
                 .connections = sums.connections,
                 .dead_lettered_per_s = mean_of(sums.dead_lettered),
                 .deduplicated_per_s = mean_of(sums.deduplicated)};
}

// A pipe's row: a node's, without the refusals (a pipe refuses nothing by policy).
NodeRow FrameAggregator::link_row(const QueueSums& sums) const {
  auto row = node_row(sums, false);
  row.rejected_per_s.reset();
  return row;
}

// The op's rates as means; its mean response and p99 by the run's rule (seen_latencies, capped at
// the client's timeout), or empty when the bucket answered nothing or its path never answered.
OpRow FrameAggregator::op_row(const OpSums& sums) const {
  const double length = step_s_ * static_cast<double>(steps_);
  OpRow row{.offered_per_s = sums.offered / length,
            .attempts_per_s = sums.attempts / length,
            .answered_per_s = sums.answered / length,
            .timed_out_per_s = sums.timed_out / length};
  if (!(sums.answered > 0.0) || sums.response.unavailable) return row;
  const auto timeout_s = graph::client_timeout_s(graph_->nodes()[sums.client].spec);
  const auto seen = seen_latencies(sums.response, timeout_s,
                                   timed_out_share(sums.answered, sums.timed_out_attempts));
  if (!seen) return row;
  const auto in_ms = [](double s) -> std::optional<double> {
    if (!std::isfinite(s)) return std::nullopt;
    return s * 1000.0;
  };
  row.mean_ms = in_ms(seen->mean_s);
  row.p99_ms = in_ms(seen->p99_s);
  return row;
}

void FrameAggregator::hand_over() {
  Frame frame;
  frame.t = start_;
  frame.dt_s = step_s_ * static_cast<double>(steps_);
  const auto nodes = graph_->nodes();
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    const bool client = i < nodes.size() && nodes[i].spec.type == model::NodeType::client;
    frame.nodes.push_back(node_row(nodes_[i], client));
  }
  for (const auto& sums : links_) frame.links.push_back(link_row(sums));
  for (const auto& sums : ops_) frame.ops.push_back(op_row(sums));
  steps_ = 0;
  sink_(frame);
}

}  // namespace simsys::report
