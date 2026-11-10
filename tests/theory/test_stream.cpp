// The frames of `run --stream` against the engine's own steps (phase 5a spec §6.2): a bucket of one
// step is that step, and over any bucket the fluid queue's conservation law still balances when the
// rates are the bucket's means and the backlog its last level:
//   mean_demand · Δt = (mean_drain + mean_dropped) · Δt + backlog(end) − backlog(start)
// which is the per-step law (engine::NodeSample) summed over the bucket's steps.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// C++20 `import`: after every #include (the eighth GCC 14.2 defect, CLAUDE.md §3).
import simsys.core;
import simsys.engine;
import simsys.fluid;
import simsys.graph;
import simsys.model;
import simsys.report;
import simsys.session;
import simsys.test_fixtures;

using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using simsys::engine::StepRecord;
using simsys::report::Frame;
using simsys::report::FrameAggregator;

namespace {

// Every record of a whole run of `g` at its scenario's step, and the frames of the same run in
// buckets of `steps_per_bucket` (one record stream, two readers).
struct RunAndFrames {
  std::vector<StepRecord> records;
  std::vector<Frame> frames;
};

RunAndFrames run_and_frames(const simsys::graph::Graph& g, std::size_t steps_per_bucket) {
  RunAndFrames out;
  FrameAggregator aggregator{g, steps_per_bucket, [&](const Frame& f) { out.frames.push_back(f); }};
  auto sim = simsys::test_support::simulation_of(g);
  sim.run({.duration = g.scenario().duration, .step = g.scenario().step}, [&](const StepRecord& r) {
    out.records.push_back(r);
    aggregator.observe(r);
  });
  aggregator.finish();
  return out;
}

void check_equal(const std::optional<double>& row, double sample) {
  REQUIRE(row.has_value());
  if (sample == 0.0) {
    CHECK_THAT(*row, WithinAbs(0.0, 1e-12));
  } else {
    CHECK_THAT(*row, WithinRel(sample, 1e-12));
  }
}

void check_optional_equal(const std::optional<double>& row, const std::optional<double>& sample) {
  CHECK(row.has_value() == sample.has_value());
  if (row && sample) check_equal(row, *sample);
}

// A one-step bucket's row is the step's sample, in the frame's units (latency in ms).
void check_row_is_sample(const simsys::report::NodeRow& row, const simsys::engine::NodeSample& n,
                         double dt_s) {
  check_equal(row.demand_per_s, n.demand_per_s);
  check_equal(row.drain_per_s, n.drain_per_s);
  check_equal(row.dropped_per_s, n.dropped_per_s);
  check_equal(row.rejected_per_s, n.rejected_per_s);
  check_equal(row.backlog, n.backlog);
  check_equal(row.peak_backlog, n.backlog);
  check_equal(row.mean_rho, n.rho);
  check_equal(row.peak_rho, n.rho);
  CHECK(row.zone == static_cast<int>(n.zone));
  check_equal(row.red_s, n.zone == simsys::fluid::Zone::red ? dt_s : 0.0);
  if (std::isfinite(n.latency.mean_s)) {
    check_equal(row.latency_ms, n.latency.mean_s * 1000.0);
  } else {
    CHECK_FALSE(row.latency_ms.has_value());
  }
  check_optional_equal(row.queue_lag_s, n.queue_lag_s);
  check_optional_equal(row.connections, n.connections);
  check_optional_equal(row.dead_lettered_per_s, n.dead_lettered_per_s);
  check_optional_equal(row.deduplicated_per_s, n.deduplicated_per_s);
}

std::string shipped(std::string_view relative) {
  return std::string{SIMSYS_SCENARIOS_DIR} + "/" + std::string{relative};
}

}  // namespace

TEST_CASE("a bucket of one step is that step, on every node of the shipped tutorials and labs",
          "[stream]") {
  for (const auto* relative : {"tutorial/hello/scenario.yaml", "tutorial/chain/scenario.yaml",
                               "labs/rate-limiter/scenario.yaml"}) {
    INFO(relative);
    const auto g = simsys::session::load_graph(shipped(relative));
    REQUIRE(g);
    const auto run = run_and_frames(*g, 1);
    REQUIRE(run.frames.size() == run.records.size());
    const double dt_s = simsys::seconds(g->scenario().step);
    for (std::size_t k = 0; k < run.records.size(); k += 97) {  // every 97th step: a spread sample
      CHECK(run.frames[k].t == run.records[k].t);
      for (std::size_t i = 0; i < g->nodes().size(); ++i) {
        if (g->nodes()[i].spec.type == simsys::model::NodeType::client) continue;
        check_row_is_sample(run.frames[k].nodes[i], run.records[k].nodes[i], dt_s);
      }
    }
  }
}

TEST_CASE("a bucket of one step is that step on a bandwidth link's pipe", "[stream]") {
  // 400 MB/s of puts over a 1 Gbit/s (125 MB/s) pipe: the pipe queues and drops megabytes.
  const auto g = *simsys::graph::Graph::build(simsys::test_support::piped(400));
  const auto run = run_and_frames(g, 1);
  for (std::size_t k = 0; k < run.records.size(); ++k) {
    REQUIRE(run.frames[k].links.size() == 1);
    const auto& row = run.frames[k].links[0];
    const auto& pipe = run.records[k].links[0];
    check_equal(row.demand_per_s, pipe.demand_per_s);
    check_equal(row.drain_per_s, pipe.drain_per_s);
    check_equal(row.dropped_per_s, pipe.dropped_per_s);
    check_equal(row.backlog, pipe.backlog);
    check_equal(row.peak_rho, pipe.rho);
    CHECK(row.zone == static_cast<int>(pipe.zone));
    CHECK_FALSE(row.rejected_per_s.has_value());  // not a link column
  }
}

TEST_CASE("over every bucket the fluid queue conserves flow: in = served + shed + backlog change",
          "[stream]") {
  // A 1k/s service sent 1.5k/s with a room of 100: the backlog grows 50 a step and fills the room
  // at the second step; from the third on, 1.5k/s − 1k/s = 500/s is shed. Buckets of 7 steps over
  // the probe's 100 steps: 14 full buckets and one of 2.
  auto s = simsys::test_support::mm1(1500);
  s.nodes[1].advanced.queue_limit = 100;
  const auto g = *simsys::graph::Graph::build(s);
  const auto run = run_and_frames(g, 7);
  REQUIRE(run.frames.size() == 15);
  CHECK_THAT(run.frames.back().dt_s, WithinRel(0.2, 1e-12));
  double previous_backlog = 0.0;
  for (const auto& frame : run.frames) {
    const auto& row = frame.nodes[1];
    const double in = *row.demand_per_s * frame.dt_s;
    const double out = (*row.drain_per_s + *row.dropped_per_s) * frame.dt_s;
    CHECK_THAT(in, WithinRel(out + *row.backlog - previous_backlog, 1e-9));
    previous_backlog = *row.backlog;
  }
  for (std::size_t k = 1; k < run.frames.size(); ++k) {
    CHECK_THAT(*run.frames[k].nodes[1].dropped_per_s, WithinRel(500.0, 1e-9));
    CHECK_THAT(*run.frames[k].nodes[1].backlog, WithinRel(100.0, 1e-9));
  }
  // The first bucket: 5 of its 7 steps shed 500/s.
  CHECK_THAT(*run.frames[0].nodes[1].dropped_per_s, WithinRel(500.0 * 5 / 7, 1e-9));
}
