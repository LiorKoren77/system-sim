// Module interface partition `simsys.engine:faults`: a scenario's scripted faults (model::Fault,
// the `faults:` list) as the engine reads them, one Condition per node and step (phase 3 spec
// §4.2).
//
// Models: the failures System Design Interview (Alex Xu, vol. 1) designs against. Ch. 1 takes a web
// server or a database offline ("if one server goes offline, the load balancer routes the traffic
// to the others"; "if the master database goes offline, a slave database will be promoted"), ch. 6
// loses a node of a key-value store, and ch. 4 and ch. 11 meet a flash crowd. A fault changes, for
// the steps it covers, the numbers a node's queue is built from, and nothing else:
//   node_down      capacity × (R − r) / R   r of the node's R copies down (all of them: × 0)
//   capacity       capacity × factor        a degraded tier; a request still takes its service time
//   latency_spike  service time + extra     a slow dependency (its servers drain c / (s + extra))
//   traffic_spike  a client's rate × factor a flash crowd, as a `spikes` entry on its rate curve
// Captured: when a fault starts and ends, how much capacity it takes, and that faults on one node
// add up. Ignored: how the failure is detected and how long failover takes (a node is down from
// `at` exactly and back at full capacity at `at + for`, with no restart or warm-up), which copies
// go down (the survivors share the load evenly: a copy's capacity is scaled, see fluid::Derating),
// and what clients do about it (retries are phase 3's Task 10). cache_flush (a cold cache warming
// back) and dc_down (a datacenter's nodes and links) are Task 7's; here they change nothing, and
// engine::Simulation::create refuses dc_down until then.
//
// C++20 module partition, interface kind: `export module simsys.engine:faults;` names a piece of
// the module simsys.engine whose exports become part of the module's interface once the primary
// interface re-exports it (`export import :faults;` in engine.cppm). An importer of simsys.engine
// sees engine::FaultSchedule as if engine.cppm declared it; nobody outside the module can import
// the partition by name. It replaces one more section in a 2000-line interface (or a header that
// belongs to no module): the fault schedule is a part of the engine with its own file.
module;
// NO <chrono>/<format>/<print>/<filesystem> here: this unit imports other modules (GCC 14.2 'Bad
// file data' bug, CLAUDE.md §3). SimTime's member operators (+=) work without <chrono>; its
// comparisons go through .count().
#include <cstddef>
#include <string>  // the == of the node ids: a std header's operators are not exported by an import
#include <utility>  // std::unreachable
#include <vector>

export module simsys.engine:faults;

import simsys.core;
import simsys.model;

export namespace simsys::engine {

// What a node is in at one time: the factors and the extra latency its active faults put on the
// numbers its queueing model is built from. The default is the identity, which changes nothing:
// what a node gets outside every fault, so a run without faults is the run before faults existed.
struct Condition {
  double capacity_factor = 1;  // × capacity: node_down (R − r) / R, all copies 0; capacity: factor
  SimTime extra_latency{};     // + service time: latency_spike's extra
  double rate_factor = 1;      // × a client's rate: traffic_spike's factor
  double hit_ratio_factor = 1;  // × a cache's hit ratio: cache_flush (Task 7; always 1 here)
};

// The scenario's faults, resolved once against its nodes: which node each acts on, when, and the
// Condition it puts the node in while it lasts. Built by engine::Simulation from the typed list.
class FaultSchedule {
 public:
  FaultSchedule() = default;  // no faults: the identity for every node, at every time
  explicit FaultSchedule(const model::Scenario& scenario);

  // The condition of node `node` (its index in Scenario::nodes, which is Graph::index_of's) at
  // `now`: every fault on it active at `now` composed, the identity when none is. A fault is
  // active on [at, at + for).
  [[nodiscard]] Condition condition_at(std::size_t node, SimTime now) const;

 private:
  // One fault that changes something: the node it acts on, when, and its effect alone.
  struct Scheduled {
    std::size_t node{};
    SimTime at{};
    SimTime duration{};
    Condition effect{};
  };

  [[nodiscard]] static Condition effect_of(const model::Fault& fault, const model::NodeSpec& node);
  [[nodiscard]] static double surviving_share(const model::Fault& fault,
                                              const model::NodeSpec& node);
  [[nodiscard]] static bool changes_nothing(const Condition& c);
  [[nodiscard]] static bool active(const Scheduled& fault, SimTime now);
  static void compose(Condition& into, const Condition& effect);

  std::vector<Scheduled> faults_;  // in the scenario's order
};

}  // namespace simsys::engine

namespace simsys::engine {

// Each fault's effect is worked out here, once; a fault that changes nothing in this task
// (cache_flush, dc_down) is left out, so condition_at never meets it. `node` is the fault's node
// looked up by id (the loader checked it is declared).
FaultSchedule::FaultSchedule(const model::Scenario& scenario) {
  for (const auto& fault : scenario.faults) {
    if (fault.node.empty()) continue;  // dc_down names a datacenter: Task 7
    for (std::size_t node = 0; node < scenario.nodes.size(); ++node) {
      if (scenario.nodes[node].id != fault.node) continue;
      const Condition effect = effect_of(fault, scenario.nodes[node]);
      if (changes_nothing(effect)) break;
      // C++20 designated initializers: `Scheduled{.node = ..., .at = ...}` names each member it
      // sets, in declaration order. Replaces positional braces, where the two SimTimes (at, for)
      // could be swapped without a compiler error.
      faults_.push_back(
          Scheduled{.node = node, .at = fault.at, .duration = fault.duration, .effect = effect});
      break;
    }
  }
}

// Every active fault on the node, composed into the identity (see compose).
Condition FaultSchedule::condition_at(std::size_t node, SimTime now) const {
  Condition condition;
  for (const auto& fault : faults_) {
    if (fault.node == node && active(fault, now)) compose(condition, fault.effect);
  }
  return condition;
}

// What one fault does to its node while it lasts (the table in the header comment).
Condition FaultSchedule::effect_of(const model::Fault& fault, const model::NodeSpec& node) {
  switch (fault.kind) {
    case model::FaultKind::node_down:
      return Condition{.capacity_factor = surviving_share(fault, node)};
    case model::FaultKind::capacity:
      return Condition{.capacity_factor = fault.factor.value_or(1.0)};
    case model::FaultKind::latency_spike:
      return Condition{.extra_latency = fault.extra.value_or(SimTime{0})};
    case model::FaultKind::traffic_spike:
      return Condition{.rate_factor = fault.factor.value_or(1.0)};
    case model::FaultKind::cache_flush:  // Task 7: the hit ratio warming back
    case model::FaultKind::dc_down:      // Task 7: refused by Simulation::create until then
      return Condition{};
  }
  // C++23 std::unreachable(): control never gets here (the switch names every kind, and -Wswitch
  // under -Werror names a new one that has no case). Replaces __builtin_unreachable(), which is
  // GCC's own spelling; reaching it is undefined behaviour.
  std::unreachable();
}

// The share of a node's capacity left when `fault.replicas` of its R copies (model::copies_of: a
// kv_cluster's machines) are down: (R − r) / R. Capacity is per copy, and the copies split the load
// evenly, so the survivors serve (R − r) / R of it: 1 of 4 leaves 0.75. No `replicas`: every copy,
// 0. `replicas: primary` (a sql_db) counts as one copy of R: the capacity is all this task models;
// that writes stop until a replica is promoted (ch. 1) is not modelled yet.
double FaultSchedule::surviving_share(const model::Fault& fault, const model::NodeSpec& node) {
  const auto copies = static_cast<double>(model::copies_of(node));
  if (fault.primary) return (copies - 1.0) / copies;
  if (!fault.replicas) return 0.0;
  return (copies - static_cast<double>(*fault.replicas)) / copies;
}

bool FaultSchedule::changes_nothing(const Condition& c) {
  return c.capacity_factor == 1.0 && c.extra_latency.count() == 0 && c.rate_factor == 1.0 &&
         c.hit_ratio_factor == 1.0;
}

// Active at `now` when at <= now < at + for, written as now − at < for: the loader does not require
// a fault to end inside the run, and at + for overflows int64 for a huge `for` (the fault would
// switch off at once), while now − at, with at <= now checked first, lies in [0, now]. The same
// test as a rate curve's spike (workload's `active`).
bool FaultSchedule::active(const Scheduled& fault, SimTime now) {
  return fault.at.count() <= now.count() && now.count() - fault.at.count() < fault.duration.count();
}

// Faults on one node compose: factors multiply (two capacity faults of 0.5 leave 0.25 of the
// capacity; a node_down of 1 of 4 copies under a capacity fault of 0.5 leaves 0.375) and extra
// latencies add (1 ms and 2 ms spikes make a service time 3 ms longer). Overlapping traffic_spike
// faults multiply without a cap: a curve's `spikes` cap their product at 1e6 (the loader's bound,
// workload::RateCurve), the faults list has no such bound, as a flash crowd on a flash crowd is the
// scenario's to state.
void FaultSchedule::compose(Condition& into, const Condition& effect) {
  into.capacity_factor *= effect.capacity_factor;
  into.extra_latency += effect.extra_latency;
  into.rate_factor *= effect.rate_factor;
  into.hit_ratio_factor *= effect.hit_ratio_factor;
}

}  // namespace simsys::engine
