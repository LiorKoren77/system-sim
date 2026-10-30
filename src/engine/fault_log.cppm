// Module interface partition `simsys.engine:fault_log`: the scripted faults' records (FaultRecord;
// src/engine/faults.cppm has the model, the twin run included) as the engine fills them, step by
// step, from each step of the run and the fault-free twin's same step.
//
// Models: the bookkeeping of an incident report, nothing of its own. It keeps a few sums per fault
// and never a step record, so a 24 h run keeps as little as a 1 s one. It judges no SLO: what a
// fault did to the SLOs is the report's (report::FaultBlock), which already holds their one
// definition; the engine cannot import the report (the report imports the engine).
//
// C++20 module partition, interface kind (faults.cppm has the longer note). engine.cppm writes
// `export import :fault_log;` (every interface partition must be re-exported by the primary
// interface), but nothing here is exported: FaultLog is a member of the exported Simulation, so
// importers must be able to REACH its definition (to know Simulation's size), not NAME it. The
// partition imports the two partitions it reads, `:records` (StepRecord) and `:faults`
// (FaultRecord, FaultSchedule), never the primary interface, which re-exports it: that would be an
// import cycle.
module;
// NO <chrono>/<format>/<print>/<filesystem> here: this unit imports other modules (GCC 14.2 'Bad
// file data' bug, CLAUDE.md §3). Like every unit from simsys.behaviours on, it is compiled with
// -flarge-source-files (the sixth defect; the simsys library target sets it).
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

export module simsys.engine:fault_log;

// C++20 `import` of a partition of the same module: `:records` and `:faults` name partitions of
// simsys.engine (the module's name is implied), whose declarations this unit sees as if they were
// its own; only units of simsys.engine can import them by these names.
import :records;
import :faults;
import simsys.core;
import simsys.defaults; // is_byte_sized: a node that counts megabytes
import simsys.graph;
import simsys.model;
import simsys.queueing; // lognormal_quantile: an op's p99 off a step's moments

namespace simsys::engine {

// The scripted faults' records, filled from every step the Simulation advances, each measured
// against the fault-free twin's same step (FaultRecord's header). Simulation::step hands it both
// records; Simulation::fault_records reads the result.
class FaultLog {
 public:
  // What one link did in one step (Simulation::send), in requests per second: what the sender put
  // on it, and what it lost there (all of it on a cut plain link; a pipe's overflow).
  struct LinkTally {
    double sent_per_s{};
    double lost_per_s{};
  };
  // One step of the run and the twin's same step, each with its links' tallies (one per link of
  // the graph). A view: it holds references to what Simulation::step has just produced.
  // C++20 std::span<const T>: a non-owning view of contiguous elements (here a vector the
  // Simulation owns), replacing a `const std::vector<T>&` that would tie the view to one container.
  struct StepPair {
    const StepRecord& run;
    const StepRecord& twin;
    std::span<const LinkTally> run_links;
    std::span<const LinkTally> twin_links;
  };

  FaultLog() = default;  // no faults: no records, and observe does nothing
  // Declared here and defaulted out of the class (after it), so that they are not inline: this
  // partition's object file defines them once, and an importer calls them instead of instantiating
  // the members' std::vector moves itself, which made GCC 14.2 crash in a Release build of a test
  // ("internal compiler error: in finish_function", stl_vector.h, verified 2026-10-30; the family
  // of CLAUDE.md §3's fourth defect).
  FaultLog(const FaultLog& other);
  FaultLog(FaultLog&& other) noexcept;
  FaultLog& operator=(const FaultLog& other);
  FaultLog& operator=(FaultLog&& other) noexcept;
  ~FaultLog();
  // One record per fault of `g`'s scenario, in `at` order (the list's order among equal `at`s).
  // `rooms[i]` is node i's room, queue_limit × copies over its copy groups (Simulation's).
  FaultLog(const graph::Graph& g, const FaultSchedule& schedule, std::span<const double> rooms);

  // The step that starts at `step.run.t` and lasts `dt`.
  void observe(const StepPair& step, SimTime dt);

  [[nodiscard]] std::span<const FaultRecord> records() const { return records_; }

 private:
  // The faulted part's numbers in one step of one of the two runs (FaultRecord's comment).
  struct Part {
    double offered_per_s{};
    double served_per_s{};
    double lost_per_s{};      // at its nodes (a traffic_spike: the client's unanswered requests)
    double cut_lost_per_s{};  // on a dc_down's cut links, in requests
    double backlog{};
  };
  // A part whose backlog may outlast the fault: its nodes, its name in the block (`s backlog
  // drained`), its capacity (to compare parts in seconds of work), and the backlog above the
  // twin's at the end of the latest step and at its worst during the fault.
  struct Backlogged {
    std::vector<std::size_t> nodes{};
    std::string name{};
    double capacity_per_s{};
    double excess{};
    double previous_excess{};  // ... at the end of the step before
    double peak_excess_s{};
  };
  // What a record is built from, beside it.
  struct Tracker {
    std::vector<std::size_t> nodes{};      // the faulted nodes (a dc_down: its datacenter's)
    std::vector<std::size_t> cut_links{};  // a dc_down's cut links
    std::optional<std::size_t> client{};   // a traffic_spike's client
    std::vector<std::size_t> origins{};    // a cache_flush's: where its misses go
    // Whose backlog may need draining: the faulted part, or for a traffic_spike every node
    // downstream (any node but a client), the worst of which is named once the spike is over.
    std::vector<Backlogged> backlogged{};
    std::optional<std::size_t> drained{};  // the one `after` follows
    double observed_s{};                   // Σ dt over the steps "during"
    double offered{};                      // Σ offered × dt over them
    double served{};                       // Σ served × dt
    bool past_window{};                    // a step after "during" was observed
  };
  // What the recovery rule compares (FaultRecord's header): every node's rho and every client op's
  // p99 (StepRecord::responses' order) of one step.
  struct Snapshot {
    std::vector<double> rho{};
    std::vector<double> p99_s{};
  };

  [[nodiscard]] Tracker tracker_of(const graph::Graph& g, const model::Fault& fault) const;
  [[nodiscard]] static FaultRecord record_of(const graph::Graph& g, const FaultSchedule& schedule,
                                             const model::Fault& fault, const Tracker& tracker,
                                             std::span<const double> rooms);
  void add_backlogged(Tracker& tracker, std::vector<std::size_t> nodes, std::string name) const;
  [[nodiscard]] static Part part_of(const Tracker& tracker, const StepRecord& record,
                                    std::span<const LinkTally> links);
  [[nodiscard]] bool needs_snapshot(const FaultRecord& r, SimTime t) const;
  void observe_one(FaultRecord& r, Tracker& tracker, const StepPair& step, SimTime dt,
                   const std::optional<bool>& back_to_twin) const;
  static void observe_during(FaultRecord& r, Tracker& tracker, const Part& run,
                             const StepPair& step, double dt_s);
  static void observe_backlogs(Tracker& tracker, const StepPair& step, bool during);
  static void observe_drain(FaultRecord& r, Tracker& tracker, SimTime t, SimTime dt);
  static void choose_drained(FaultRecord& r, Tracker& tracker);
  static void count_losses(FaultRecord& r, const Part& run, const Part& twin, double dt_s);
  static void observe_warm_up(FaultRecord& r, SimTime t, SimTime dt);
  [[nodiscard]] static bool in_window(const FaultRecord& r, SimTime t);
  [[nodiscard]] static bool cleared(const FaultRecord& r, SimTime t);
  [[nodiscard]] static std::optional<double> window_length_s(const FaultRecord& r);
  [[nodiscard]] static Snapshot snapshot_of(const StepRecord& record);
  [[nodiscard]] static bool back_to(const Snapshot& run, const Snapshot& twin);

  std::vector<FaultRecord> records_{};
  std::vector<Tracker> trackers_{};   // one per record, in the same order
  std::vector<double> capacities_{};  // per node: its capacity, units per second
  std::vector<std::string> ids_{};    // per node: its id
  std::vector<bool> clients_{};       // per node: a client, which has no queue
};

namespace {

// Below this a backlog, a loss or a rate is float residue of the fluid arithmetic, not requests (a
// drained queue can end at 1e-13 rather than 0).
constexpr double fault_residue = 1e-9;

// The recovery rule's tolerance (FaultRecord's header): within 1 % of the twin's same step.
constexpr double recovery_tolerance = 0.01;

// `run` within 1 % of `twin`, relative to it. Equal values always are (an infinite p99 that is
// infinite in the twin too included), and so are two NaNs (an op neither run observed at the
// step: the same non-observation); the residue's slack lets a twin's 0 accept float residue.
[[nodiscard]] bool within_tolerance(double run, double twin) {
  if (std::isnan(run) && std::isnan(twin)) return true;
  return run == twin || std::abs(run - twin) <= recovery_tolerance * std::abs(twin) + fault_residue;
}

// A backlog above the twin's by more than float residue and the 1 % the recovery rule allows: work
// the fault left (FaultAfter::backlog_node).
[[nodiscard]] bool backlog_left(double excess, double twin_backlog) {
  return excess > recovery_tolerance * std::abs(twin_backlog) + fault_residue;
}

// The faults in `at` order, the list's order among equal `at`s.
[[nodiscard]] std::vector<std::size_t> in_at_order(std::span<const model::Fault> faults) {
  std::vector<std::size_t> order(faults.size());
  std::iota(order.begin(), order.end(), std::size_t{0});  // 0, 1, 2, ...: the list's order
  // C++20 std::ranges::stable_sort with a projection: sorts the indexes by the `at` each one
  // projects to (a lambda handed to an ALGORITHM, which is safe under GCC 14.2; only a lambda
  // handed to a range ADAPTOR in a module trips the fifth defect), keeping the list's order among
  // equal keys. Replaces a hand-written insertion sort and a comparator that names both sides.
  std::ranges::stable_sort(order, {}, [&](std::size_t i) { return faults[i].at.count(); });
  return order;
}

// Every node the misses of `cache` go to (FaultRecord::origin): its own fetch (a read_through
// cache's backend, a CDN's origin: graph::fetches_read_misses), its `then` steps on miss, and every
// `on: miss` step of another node whose nearest unconditional step before it looks `cache` up
// (cache-aside, graph.cpp's add_op_step_hops: the same rule).
[[nodiscard]] std::vector<std::size_t> origins_of(const graph::Graph& g, std::size_t cache) {
  const auto nodes = g.nodes();
  std::vector<std::size_t> origins;
  const auto add = [&](std::size_t node) {
    // C++23 std::ranges::contains: "is this value in the range" in one call, replacing
    // `std::find(v.begin(), v.end(), x) != v.end()`.
    if (!std::ranges::contains(origins, node)) origins.push_back(node);
  };
  if (graph::fetches_read_misses(nodes[cache].spec) && nodes[cache].backend) {
    add(*nodes[cache].backend);
  }
  for (const auto& step : nodes[cache].spec.then) {
    if (step.on == model::StepCondition::miss) add(*g.index_of(step.to));
  }
  for (const auto& node : nodes) {
    for (const auto& [op, steps] : node.spec.ops) {
      bool after_lookup = false;
      for (const auto& step : steps) {
        if (step.on == model::StepCondition::always) after_lookup = step.to == nodes[cache].spec.id;
        if (step.on == model::StepCondition::miss && after_lookup) add(*g.index_of(step.to));
      }
    }
  }
  return origins;
}

// The unit of the nodes' numbers (engine::NodeSample): megabytes when every one of them is
// byte-sized, requests (capacity units) otherwise.
[[nodiscard]] std::string unit_of(const graph::Graph& g, std::span<const std::size_t> nodes) {
  // C++20 std::ranges::all_of: the range itself rather than a begin/end pair (std::all_of), so the
  // pair cannot be mismatched.
  const bool megabytes = !nodes.empty() && std::ranges::all_of(nodes, [&](std::size_t node) {
    return defaults::is_byte_sized(g.nodes()[node].spec);
  });
  return megabytes ? "MB" : "requests";
}

// A datacenter's nodes in one unit (FaultRecord's header): all of them when they share one, its
// request-counting nodes when they mix.
[[nodiscard]] std::vector<std::size_t> in_one_unit(const graph::Graph& g,
                                                   std::vector<std::size_t> nodes) {
  if (unit_of(g, nodes) == "MB") return nodes;
  // C++20 std::erase_if: removes every element the predicate holds for, in one call, replacing the
  // erase-remove idiom `v.erase(std::remove_if(v.begin(), v.end(), p), v.end())`.
  std::erase_if(nodes,
                [&](std::size_t node) { return defaults::is_byte_sized(g.nodes()[node].spec); });
  return nodes;
}

// The ids of `nodes`, joined by ", ".
[[nodiscard]] std::string ids_of(const graph::Graph& g, std::span<const std::size_t> nodes) {
  std::string ids;
  for (const std::size_t node : nodes) ids += (ids.empty() ? "" : ", ") + g.nodes()[node].spec.id;
  return ids;
}

// The sum over `nodes` of a node sample's field, picked by a pointer to data member (C++98).
[[nodiscard]] double sum_of(const StepRecord& record, std::span<const std::size_t> nodes,
                            double NodeSample::*field) {
  double sum = 0.0;
  for (const std::size_t node : nodes) sum += record.nodes[node].*field;
  return sum;
}

}  // namespace

FaultLog::FaultLog(const FaultLog& other) = default;
FaultLog::FaultLog(FaultLog&& other) noexcept = default;
FaultLog& FaultLog::operator=(const FaultLog& other) = default;
FaultLog& FaultLog::operator=(FaultLog&& other) noexcept = default;
FaultLog::~FaultLog() = default;

FaultLog::FaultLog(const graph::Graph& g, const FaultSchedule& schedule,
                   std::span<const double> rooms) {
  for (const auto& node : g.nodes()) {
    capacities_.push_back(node.capacity.service_rate.per_second);
    ids_.push_back(node.spec.id);
    clients_.push_back(node.spec.type == model::NodeType::client);
  }
  const auto& faults = g.scenario().faults;
  for (const std::size_t i : in_at_order(faults)) {
    trackers_.push_back(tracker_of(g, faults[i]));
    records_.push_back(record_of(g, schedule, faults[i], trackers_.back(), rooms));
  }
}

// The faulted part (FaultRecord's comment): the named node; a dc_down's datacenter, its nodes but
// its clients, in one unit, and the links that cross its edge (FaultSchedule::add_dc_down's
// choice); a traffic_spike's client, with every node but a client as a candidate for the backlog
// it leaves (one the spike does not reach is never above the twin's); a flush's cache and the
// origins its misses go to.
FaultLog::Tracker FaultLog::tracker_of(const graph::Graph& g, const model::Fault& fault) const {
  Tracker tracker;
  const auto nodes = g.nodes();
  if (fault.kind == model::FaultKind::dc_down) {
    const auto inside = [&](std::size_t node) { return nodes[node].spec.dc == fault.datacenter; };
    std::vector<std::size_t> in_dc;
    for (std::size_t node = 0; node < nodes.size(); ++node) {
      if (inside(node) && !clients_[node]) in_dc.push_back(node);
    }
    tracker.nodes = in_one_unit(g, std::move(in_dc));
    const auto links = g.links();
    for (std::size_t link = 0; link < links.size(); ++link) {
      if (inside(links[link].from) != inside(links[link].to)) tracker.cut_links.push_back(link);
    }
    add_backlogged(tracker, tracker.nodes, fault.datacenter);
    return tracker;
  }
  const std::size_t node = *g.index_of(fault.node);
  if (fault.kind == model::FaultKind::traffic_spike) {
    tracker.client = node;
    for (std::size_t other = 0; other < nodes.size(); ++other) {
      if (!clients_[other]) add_backlogged(tracker, {other}, ids_[other]);
    }
    return tracker;
  }
  tracker.nodes.push_back(node);
  if (fault.kind == model::FaultKind::cache_flush) {
    tracker.origins = origins_of(g, node);
  } else {
    add_backlogged(tracker, tracker.nodes, fault.node);
  }
  return tracker;
}

void FaultLog::add_backlogged(Tracker& tracker, std::vector<std::size_t> nodes,
                              std::string name) const {
  double capacity = 0.0;
  for (const std::size_t node : nodes) capacity += capacities_[node];
  // C++20 designated initializers: each member named at the call site, in declaration order,
  // replacing positional braces in which two doubles could swap silently.
  tracker.backlogged.push_back(
      Backlogged{.nodes = std::move(nodes), .name = std::move(name), .capacity_per_s = capacity});
}

// A record of zeros until the fault starts: what it is, its unit, the room its backlog can fill,
// and a flush's window and origin.
FaultRecord FaultLog::record_of(const graph::Graph& g, const FaultSchedule& schedule,
                                const model::Fault& fault, const Tracker& tracker,
                                std::span<const double> rooms) {
  double room = 0.0;
  for (const std::size_t node : tracker.nodes) room += rooms[node];
  FaultRecord r{.kind = fault.kind,
                .node = fault.node,
                .at = fault.at,
                .duration = fault.duration,
                .lost_unit = tracker.client ? "requests" : unit_of(g, tracker.nodes),
                .room = tracker.client ? 0.0 : room,
                .origin = ids_of(g, tracker.origins),
                .origin_unit = unit_of(g, tracker.origins),
                .datacenter = fault.datacenter};
  if (fault.kind == model::FaultKind::cache_flush) {
    // A cache without a hit ratio loses nothing to a flush: its window is empty.
    const std::size_t cache = tracker.nodes.front();
    r.warm_up_s = g.nodes()[cache].hit_ratio ? schedule.warm_up_s(cache, fault.at) : 0.0;
  }
  return r;
}

// The recovery rule's snapshots (the run's step and the twin's) only when some record reads them:
// a step at or after a fault has cleared that has not recovered yet. A 24 h run with one 30 s fault
// takes a few hundred, not 864,000.
void FaultLog::observe(const StepPair& step, SimTime dt) {
  if (records_.empty()) return;
  std::optional<bool> back_to_twin;
  for (std::size_t i = 0; i < records_.size() && !back_to_twin; ++i) {
    if (needs_snapshot(records_[i], step.run.t)) {
      back_to_twin = back_to(snapshot_of(step.run), snapshot_of(step.twin));
    }
  }
  for (std::size_t i = 0; i < records_.size(); ++i) {
    observe_one(records_[i], trackers_[i], step, dt, back_to_twin);
  }
}

bool FaultLog::needs_snapshot(const FaultRecord& r, SimTime t) const {
  return cleared(r, t) && !r.recovered_at;
}

// One record's share of the step, from `at` on: a step during the fault adds to the means, a step
// after it may end the drain, and every step until the recovery adds its losses over the twin's,
// unless it is the recovery step itself.
void FaultLog::observe_one(FaultRecord& r, Tracker& tracker, const StepPair& step, SimTime dt,
                           const std::optional<bool>& back_to_twin) const {
  const SimTime t = step.run.t;
  if (t.count() < r.at.count()) return;
  const Part run = part_of(tracker, step.run, step.run_links);
  const Part twin = part_of(tracker, step.twin, step.twin_links);
  const bool during = in_window(r, t);
  observe_backlogs(tracker, step, during);
  if (during) {
    observe_during(r, tracker, run, step, seconds(dt));
  } else {
    observe_drain(r, tracker, t, dt);
  }
  if (!r.recovered_at) {
    if (cleared(r, t) && back_to_twin.value_or(false)) {
      r.recovered_at = t;
    } else {
      count_losses(r, run, twin, seconds(dt));
    }
  }
  observe_warm_up(r, t, dt);
}

// The faulted part's numbers in one step of one run (FaultRecord's comment: a dc_down's cut links
// lose requests, apart; a traffic_spike's client counts its requests, offered and answered, the
// rest lost or timed out: answered = offered − lost − timed out, ResponseSample).
FaultLog::Part FaultLog::part_of(const Tracker& tracker, const StepRecord& record,
                                 std::span<const LinkTally> links) {
  Part part{.offered_per_s = sum_of(record, tracker.nodes, &NodeSample::demand_per_s),
            .served_per_s = sum_of(record, tracker.nodes, &NodeSample::drain_per_s),
            .lost_per_s = sum_of(record, tracker.nodes, &NodeSample::dropped_per_s),
            .backlog = sum_of(record, tracker.nodes, &NodeSample::backlog)};
  for (const std::size_t link : tracker.cut_links) {
    if (link < links.size()) part.cut_lost_per_s += links[link].lost_per_s;
  }
  if (!tracker.client) return part;
  for (const auto& sample : record.responses) {
    if (sample.client != *tracker.client) continue;
    part.offered_per_s += sample.offered.per_second;
    part.served_per_s += sample.answered.per_second;
    part.lost_per_s += std::max(0.0, sample.offered.per_second - sample.answered.per_second);
  }
  return part;
}

// During the fault: the run's means of offered and served so far, its backlog's peak, and for a
// flush the reads its origins were sent beyond the twin's.
void FaultLog::observe_during(FaultRecord& r, Tracker& tracker, const Part& run,
                              const StepPair& step, double dt_s) {
  tracker.observed_s += dt_s;
  tracker.offered += run.offered_per_s * dt_s;
  tracker.served += run.served_per_s * dt_s;
  r.offered_per_s = tracker.offered / tracker.observed_s;
  r.served_per_s = tracker.served / tracker.observed_s;
  r.backlog_peak = std::max(r.backlog_peak, run.backlog);
  const double extra_per_s = sum_of(step.run, tracker.origins, &NodeSample::demand_per_s) -
                             sum_of(step.twin, tracker.origins, &NodeSample::demand_per_s);
  r.extra_misses += extra_per_s * dt_s;
}

// Every candidate's backlog above the twin's at this step, and during the fault its worst, in
// seconds of its own work (excess / capacity), so that a traffic_spike's candidates compare.
void FaultLog::observe_backlogs(Tracker& tracker, const StepPair& step, bool during) {
  for (auto& part : tracker.backlogged) {
    const double twin_backlog = sum_of(step.twin, part.nodes, &NodeSample::backlog);
    const double excess = sum_of(step.run, part.nodes, &NodeSample::backlog) - twin_backlog;
    part.previous_excess = part.excess;
    part.excess = backlog_left(excess, twin_backlog) ? excess : 0.0;
    if (during && part.capacity_per_s > 0.0) {
      part.peak_excess_s = std::max(part.peak_excess_s, part.excess / part.capacity_per_s);
    }
  }
}

// After the fault: the backlog it left above the twin's is drained at the end of the first step
// at which it is back at the twin's, timed from the end of the fault (0 when it was back as the
// fault ended). Exact tick arithmetic for a fault that lasts (0.2 s, not 0.19999999999999929); a
// flush's window is seconds. The first step after the fault chooses whose backlog to follow.
void FaultLog::observe_drain(FaultRecord& r, Tracker& tracker, SimTime t, SimTime dt) {
  const bool first_after = !std::exchange(tracker.past_window, true);
  if (first_after) choose_drained(r, tracker);
  if (!tracker.drained || r.after.backlog_drained_in_s) return;
  const Backlogged& part = tracker.backlogged[*tracker.drained];
  if (first_after && part.previous_excess <= 0.0) {
    r.after.backlog_drained_in_s = 0.0;
    return;
  }
  if (part.excess > 0.0) return;
  const SimTime since_at{t.count() + dt.count() - r.at.count()};
  if (model::lasts(r.kind)) {
    r.after.backlog_drained_in_s = seconds(SimTime{since_at.count() - r.duration.count()});
  } else {
    r.after.backlog_drained_in_s = seconds(since_at) - window_length_s(r).value_or(0.0);
  }
}

// The part whose backlog `after` follows: the one with the most work left over the twin's at its
// worst during the fault (the only one, but for a traffic_spike), if it had any.
void FaultLog::choose_drained(FaultRecord& r, Tracker& tracker) {
  // C++20 std::ranges::max_element with a projection (&Backlogged::peak_excess_s): the element
  // whose member is largest, without a comparator that names the member on both sides.
  const auto worst = std::ranges::max_element(tracker.backlogged, {}, &Backlogged::peak_excess_s);
  if (worst == tracker.backlogged.end() || worst->peak_excess_s <= 0.0) return;
  tracker.drained = static_cast<std::size_t>(worst - tracker.backlogged.begin());
  r.after.backlog_node = worst->name;
}

// What the step cost beyond the twin's: the faulted part's drops (a traffic_spike's client: its
// unanswered requests), and a dc_down's cut links' apart.
void FaultLog::count_losses(FaultRecord& r, const Part& run, const Part& twin, double dt_s) {
  r.lost += (run.lost_per_s - twin.lost_per_s) * dt_s;
  r.lost_on_cut_links += (run.cut_lost_per_s - twin.cut_lost_per_s) * dt_s;
}

// A flushed cache is warm again from the step during which its warm-up ends, once the run gets
// there: the warm-up time itself, not the step's end (Task 7's curve, FaultSchedule::warm_up_s).
void FaultLog::observe_warm_up(FaultRecord& r, SimTime t, SimTime dt) {
  if (r.kind != model::FaultKind::cache_flush || !r.warm_up_s || r.after.cache_warm_in_s) return;
  if (seconds(SimTime{t.count() + dt.count() - r.at.count()}) >= *r.warm_up_s) {
    r.after.cache_warm_in_s = *r.warm_up_s;
  }
}

// Inside the fault's window: [at, at + for) (model::active_at), or a flush's warm-up from `at` on
// (for ever when the cache never warms).
bool FaultLog::in_window(const FaultRecord& r, SimTime t) {
  if (model::lasts(r.kind)) return model::active_at(r.at, r.duration, t);
  if (t.count() < r.at.count()) return false;
  return !r.warm_up_s || seconds(SimTime{t.count() - r.at.count()}) < *r.warm_up_s;
}

// The fault has cleared by `t`: at + for (a flush, an instant, at `at`).
bool FaultLog::cleared(const FaultRecord& r, SimTime t) {
  return t.count() - r.at.count() >= r.duration.count();
}

// The window's length in seconds: `for`, or a flush's warm-up (empty: it never warms).
std::optional<double> FaultLog::window_length_s(const FaultRecord& r) {
  if (model::lasts(r.kind)) return seconds(r.duration);
  return r.warm_up_s;
}

// The step as the recovery rule reads it: the p99 of each client op is read off the lognormal fit
// to the moments of its delivered requests (the reports' percentiles, on one step).
FaultLog::Snapshot FaultLog::snapshot_of(const StepRecord& record) {
  Snapshot snapshot;
  for (const auto& node : record.nodes) snapshot.rho.push_back(node.rho);
  for (const auto& sample : record.responses) {
    const auto& fit = sample.delivered_response;
    snapshot.p99_s.push_back(
        queueing::lognormal_quantile(fit.mean_s, fit.variance_s2, queueing::z_p99));
  }
  return snapshot;
}

// Every node's rho and every client op's p99 within 1 % of the twin's same step.
bool FaultLog::back_to(const Snapshot& run, const Snapshot& twin) {
  if (run.rho.size() != twin.rho.size() || run.p99_s.size() != twin.p99_s.size()) return false;
  for (std::size_t i = 0; i < run.rho.size(); ++i) {
    if (!within_tolerance(run.rho[i], twin.rho[i])) return false;
  }
  for (std::size_t i = 0; i < run.p99_s.size(); ++i) {
    if (!within_tolerance(run.p99_s[i], twin.p99_s[i])) return false;
  }
  return true;
}

}  // namespace simsys::engine
