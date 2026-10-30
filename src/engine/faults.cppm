// Module interface partition `simsys.engine:faults`: a scenario's scripted faults (model::Fault,
// the `faults:` list) as the engine reads them, one Condition per node, per link and per step
// (phase 3 spec §4.2-4.3).
//
// Models: the failures System Design Interview (Alex Xu, vol. 1) designs against. Ch. 1 takes a web
// server or a database offline ("if one server goes offline, the load balancer routes the traffic
// to the others"; "if the master database goes offline, a slave database will be promoted"), loses
// a whole data center ("if any damage happens to one data center, ... all traffic is routed to the
// healthy data center"), and puts a cache tier in front of the database whose loss sends every read
// to the database; ch. 6 loses a node of a key-value store, and ch. 4 and ch. 11 meet a flash
// crowd. A fault changes, for the steps it covers, the numbers a node's queue is built from, and
// nothing else:
//   node_down      capacity × (R − r) / R   r of the node's R copies down (all of them: × 0)
//   capacity       capacity × factor        a degraded tier; a request still takes its service time
//   latency_spike  service time + extra     a slow dependency (its servers drain c / (s + extra))
//   traffic_spike  a client's rate × factor a flash crowd, as a `spikes` entry on its rate curve
//   cache_flush    hit ratio × h(t) / h     the cache emptied at `at`, warming back (WarmUp below)
//   dc_down        node_down on every node of the datacenter, and capacity × 0 on every link that
//                  crosses its edge (link_condition_at)
// Captured: when a fault starts and ends, how much capacity it takes, that faults on one node add
// up, and how fast a flushed cache refills. Ignored: how the failure is detected and how long
// failover takes (a node is down from `at` exactly and back at full capacity at `at + for`, with no
// restart or warm-up of its own), which copies go down (the survivors share the load evenly: a
// copy's capacity is scaled, see fluid::Derating), what clients do about it (retries are phase 3's
// Task 10), and ch. 1's geoDNS failover: a dc_down cuts the datacenter off, and only the scenario's
// own links (a second route the client splits its requests over) send anything elsewhere.
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
// comparisons and differences go through .count().
#include <algorithm>  // std::ranges::contains
#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>  // the == of the node ids: a std header's operators are not exported by an import
#include <utility>  // std::unreachable
#include <vector>

export module simsys.engine:faults;

import simsys.caching;
import simsys.core;
import simsys.graph;
import simsys.model;

export namespace simsys::engine {

// What a node (or a link) is in at one time: the factors and the extra latency its active faults
// put on the numbers its queueing model is built from. The default is the identity, which changes
// nothing: what a node gets outside every fault, so a run without faults is the run before faults
// existed. A link reads only capacity_factor (a dc_down's 0).
struct Condition {
  double capacity_factor = 1;  // × capacity: node_down (R − r) / R, all copies 0; capacity: factor
  SimTime extra_latency{};     // + service time: latency_spike's extra
  double rate_factor = 1;      // × a client's rate: traffic_spike's factor
  double hit_ratio_factor = 1;  // × a cache's hit ratio: cache_flush's warm-up, h(t) / h
};

// The scenario's faults, resolved once against its graph: which node or link each acts on, when,
// and the Condition it puts it in while it lasts. Built by engine::Simulation, which also supplies
// the one number the graph cannot: how many reads a flushed cache is handed at the flush.
class FaultSchedule {
 public:
  FaultSchedule() = default;  // no faults: the identity for every node and link, at every time

  // `g.scenario().faults` resolved against `g`. `flush_reads[i]` is, for a cache_flush at position
  // i of the faults list, the reads per second the flushed cache (all its copies together) is
  // handed at the flush, which the warm-up turns into "requests seen" (WarmUp); entries for the
  // other kinds are not read, and a list without a cache_flush may pass none. The engine computes
  // them (Simulation::fault_schedule_of). Throws std::invalid_argument for a cache_flush without
  // its entry, or on a fixed-ratio cache without `keys` (Simulation::create refuses that scenario
  // first): programmer errors.
  // C++20 std::span<const double>: a non-owning read-only view of contiguous doubles, so the caller
  // may pass a vector, an array or `{}` (no flush) without this class owning or copying them;
  // replaces a `const std::vector<double>&`, which would force the caller to build a vector.
  FaultSchedule(const graph::Graph& g, std::span<const double> flush_reads);

  // The condition of node `node` (its index in Scenario::nodes, which is Graph::index_of's) at
  // `now`: every fault on it active at `now` composed, the identity when none is. A fault is
  // active on [at, at + for); a flush has no `for`: its warm-up runs from `at` until the cache is
  // full again.
  [[nodiscard]] Condition condition_at(std::size_t node, SimTime now) const;

  // The condition of link `link` (its index in Graph::links) at `now`: capacity factor 0 while a
  // dc_down cuts it (one end in the datacenter, the other outside), the identity otherwise.
  [[nodiscard]] Condition link_condition_at(std::size_t link, SimTime now) const;

  // Whether some dc_down cuts link `link` at some time: the links the engine checks for a cut
  // (Simulation::connect_cuttable).
  [[nodiscard]] bool may_cut(std::size_t link) const;

  // The nodes a cache_flush acts on, each once, in the order of their first flush: the caches
  // whose hit ratio factor the engine hands, every step, to the behaviours that route their
  // misses (Simulation::warm_flushed_caches). Empty without a flush.
  [[nodiscard]] std::span<const std::size_t> flushed_caches() const { return flushed_; }

 private:
  // One fault that changes something on one node (a dc_down is one per node of the datacenter):
  // the node, when, and its effect alone.
  struct Scheduled {
    std::size_t node{};
    SimTime at{};
    SimTime duration{};
    Condition effect{};
  };

  // A link a dc_down cuts, and for how long.
  struct Cut {
    std::size_t link{};
    SimTime at{};
    SimTime duration{};
  };

  // A flushed cache refilling (spec §4.3). At t after the flush ONE copy of the cache has seen
  // n = reads_per_copy × t requests, at the reads per second it was handed at the flush (kept for
  // the whole warm-up: a rate curve that rises or falls meanwhile is not followed). Each of its R
  // copies sees 1/R of the reads and fills its own entries (a ring's copies hold different keys,
  // so a partitioned che cache counts as one cache of keys × R seeing every read). Its hit ratio
  // is then, relative to the steady h it had before the flush:
  //   che       h(n) / h = refill_hit_ratio(C, popularity, n) / che_hit_ratio(C, popularity)
  //             (caching::refill_hit_ratio: the Che hit ratio of a cache of min(C, D(n)) entries,
  //             D(n) the distinct keys among n requests), full at n = t_c, Che's characteristic
  //             time of C, where D(t_c) = C by definition
  //   fixed     n / keys: one new entry per request (every request misses an empty cache), so the
  //             cache is full after keys requests, keys / rate seconds, and a ratio that is a
  //             number, not a function of the size, warms linearly: the simple case
  // and exactly 1 from the moment it is full (`full_at`, in requests per copy). 0 at the flush.
  // Ignored: that an LRU cache refills in recency order (the recent keys are a little hotter than
  // "distinct"), the thundering herd of misses on the store, and the keys a write would put back.
  struct WarmUp {
    std::size_t cache{};
    SimTime at{};
    double reads_per_copy{};  // per second
    double full_at{};         // requests per copy: keys (fixed) or t_c (che)
    // che only: the popularity the cache's reads draw from, its capacity C in keys and its steady
    // Che hit ratio. Empty for a fixed ratio.
    std::optional<caching::Popularity> popularity{};
    double capacity{};
    double steady{1.0};
  };

  void add_node_fault(const graph::Graph& g, const model::Fault& fault);
  void add_dc_down(const graph::Graph& g, const model::Fault& fault);
  void add_flush(const graph::Graph& g, const model::Fault& fault, double reads);
  [[nodiscard]] static WarmUp warm_up_of(const graph::Graph& g, std::size_t cache, SimTime at,
                                         double reads);
  [[nodiscard]] static Condition effect_of(const model::Fault& fault, const model::NodeSpec& node);
  [[nodiscard]] static double surviving_share(const model::Fault& fault,
                                              const model::NodeSpec& node);
  [[nodiscard]] static bool changes_nothing(const Condition& c);
  [[nodiscard]] static bool active(SimTime at, SimTime duration, SimTime now);
  static void compose(Condition& into, const Condition& effect);
  [[nodiscard]] double warmth_at(std::size_t node, SimTime now) const;
  [[nodiscard]] static double warmth(const WarmUp& warm_up, SimTime now);

  std::vector<Scheduled> faults_;     // in the scenario's order
  std::vector<Cut> cuts_;             // dc_down's links, in the scenario's order
  std::vector<WarmUp> warm_ups_;      // in the scenario's order
  std::vector<std::size_t> flushed_;  // flushed_caches()
};

}  // namespace simsys::engine

namespace simsys::engine {

namespace {

// The node `fault` names, as its graph index (the loader checked it is declared).
[[nodiscard]] std::size_t node_named(const graph::Graph& g, const model::Fault& fault) {
  return *g.index_of(fault.node);
}

// The key popularity a che cache's reads draw from: its clients' keys, or, behind another che
// cache, that cache's popularity filtered by its shards (the misses it passes on). The same chain
// Graph::build resolved the hit ratio from (graph.cpp's che_popularity), rebuilt here from what
// each ResolvedHitRatio keeps, because a Graph cannot hold a caching::Popularity (a GCC 14.2 crash,
// see graph.cpp's HitRatioPolicy).
[[nodiscard]] caching::Popularity popularity_seen_by(std::span<const graph::ResolvedNode> nodes,
                                                     const graph::ResolvedHitRatio& resolved) {
  if (!resolved.filtered_by) {
    const auto& keys = *resolved.keys;
    return keys.zipf ? caching::Popularity::zipf(*keys.zipf, keys.space)
                     : caching::Popularity::uniform(keys.space);
  }
  const auto& upstream = *nodes[*resolved.filtered_by].hit_ratio;
  return popularity_seen_by(nodes, upstream).filtered_by(upstream.shards);
}

}  // namespace

// Each fault's effect is worked out here, once; a fault that changes nothing is left out, so
// condition_at never meets it.
FaultSchedule::FaultSchedule(const graph::Graph& g, std::span<const double> flush_reads) {
  const auto& faults = g.scenario().faults;
  for (std::size_t i = 0; i < faults.size(); ++i) {
    switch (faults[i].kind) {
      case model::FaultKind::dc_down:
        add_dc_down(g, faults[i]);
        break;
      case model::FaultKind::cache_flush:
        if (i >= flush_reads.size()) {
          throw std::invalid_argument{"FaultSchedule: no read rate for the cache_flush of faults[" +
                                      std::to_string(i) + "]"};
        }
        add_flush(g, faults[i], flush_reads[i]);
        break;
      case model::FaultKind::node_down:
      case model::FaultKind::capacity:
      case model::FaultKind::latency_spike:
      case model::FaultKind::traffic_spike:
        add_node_fault(g, faults[i]);
        break;
    }
  }
}

void FaultSchedule::add_node_fault(const graph::Graph& g, const model::Fault& fault) {
  const std::size_t node = node_named(g, fault);
  const Condition effect = effect_of(fault, g.nodes()[node].spec);
  if (changes_nothing(effect)) return;
  // C++20 designated initializers: `Scheduled{.node = ..., .at = ...}` names each member it sets,
  // in declaration order. Replaces positional braces, where the two SimTimes (at, for) could be
  // swapped without a compiler error.
  faults_.push_back(
      Scheduled{.node = node, .at = fault.at, .duration = fault.duration, .effect = effect});
}

// dc_down (ch. 1's data center outage): node_down on every node whose `dc` is the datacenter (a
// client excepted: it has no capacity to take, and its users keep sending), and a cut on every
// link with one end inside and the other outside, whichever way its demand flows: a request
// crossing into the datacenter and one leaving it are both lost. A link with both ends inside is
// left alone (its nodes are down anyway), so a client in the datacenter still fills its service's
// room, as at a node_down.
void FaultSchedule::add_dc_down(const graph::Graph& g, const model::Fault& fault) {
  const auto nodes = g.nodes();
  const auto inside = [&](std::size_t node) { return nodes[node].spec.dc == fault.datacenter; };
  for (std::size_t node = 0; node < nodes.size(); ++node) {
    if (!inside(node) || nodes[node].spec.type == model::NodeType::client) continue;
    faults_.push_back(Scheduled{.node = node,
                                .at = fault.at,
                                .duration = fault.duration,
                                .effect = Condition{.capacity_factor = 0.0}});
  }
  const auto links = g.links();
  for (std::size_t link = 0; link < links.size(); ++link) {
    if (inside(links[link].from) == inside(links[link].to)) continue;
    cuts_.push_back(Cut{.link = link, .at = fault.at, .duration = fault.duration});
  }
}

// A flush of a cache with no hit ratio (a CDN that misses every read) changes nothing: h × 0 = h.
void FaultSchedule::add_flush(const graph::Graph& g, const model::Fault& fault, double reads) {
  const std::size_t cache = node_named(g, fault);
  if (!g.nodes()[cache].hit_ratio) return;
  warm_ups_.push_back(warm_up_of(g, cache, fault.at, reads));
  // C++23 std::ranges::contains: "is this value in the range?" in one call, replacing
  // `std::find(begin, end, value) != end`. may_cut uses its projection form (`&Cut::link`: compare
  // each element's member, not the element).
  if (!std::ranges::contains(flushed_, cache)) flushed_.push_back(cache);
}

// The warm-up's numbers (WarmUp's comment has the model), resolved once.
FaultSchedule::WarmUp FaultSchedule::warm_up_of(const graph::Graph& g, std::size_t cache,
                                                SimTime at, double reads) {
  const auto& node = g.nodes()[cache];
  const auto& resolved = *node.hit_ratio;
  const double copies = static_cast<double>(model::copies_of(node.spec));
  if (!resolved.keys) {  // a fixed ratio: linear over keys / rate
    if (!node.spec.keys) {
      throw std::invalid_argument{"FaultSchedule: cache_flush on '" + node.spec.id +
                                  "', a fixed ratio without keys (Simulation::create refuses it)"};
    }
    return WarmUp{.cache = cache,
                  .at = at,
                  .reads_per_copy = reads / copies,
                  .full_at = static_cast<double>(*node.spec.keys)};
  }
  // che: a ring's partitions hold different keys, one cache of `capacity` seeing every read; any
  // other replicas are copies of one cache each seeing its share.
  auto popularity = popularity_seen_by(g.nodes(), resolved);
  const double full_at = caching::che_characteristic_time(resolved.capacity, popularity);
  const double steady = caching::che_hit_ratio(resolved.capacity, popularity);
  return WarmUp{.cache = cache,
                .at = at,
                .reads_per_copy = reads * static_cast<double>(resolved.partitions) / copies,
                .full_at = full_at,
                .popularity = std::move(popularity),
                .capacity = resolved.capacity,
                .steady = steady};
}

// Every active fault on the node composed (see compose), and the warm-up of its latest flush.
Condition FaultSchedule::condition_at(std::size_t node, SimTime now) const {
  Condition condition;
  for (const auto& fault : faults_) {
    if (fault.node == node && active(fault.at, fault.duration, now)) {
      compose(condition, fault.effect);
    }
  }
  condition.hit_ratio_factor = warmth_at(node, now);
  return condition;
}

Condition FaultSchedule::link_condition_at(std::size_t link, SimTime now) const {
  for (const auto& cut : cuts_) {
    if (cut.link == link && active(cut.at, cut.duration, now)) {
      return Condition{.capacity_factor = 0.0};
    }
  }
  return Condition{};
}

bool FaultSchedule::may_cut(std::size_t link) const {
  return std::ranges::contains(cuts_, link, &Cut::link);
}

// The hit ratio factor of the node's latest flush at or before `now` (a later flush empties the
// cache again and restarts the warm-up); 1 before any flush and on a node never flushed.
double FaultSchedule::warmth_at(std::size_t node, SimTime now) const {
  const WarmUp* latest = nullptr;
  for (const auto& warm_up : warm_ups_) {
    if (warm_up.cache != node || warm_up.at.count() > now.count()) continue;
    if (latest == nullptr || warm_up.at.count() >= latest->at.count()) latest = &warm_up;
  }
  return latest == nullptr ? 1.0 : warmth(*latest, now);
}

// h(n) / h at n requests per copy since the flush (WarmUp's comment): exactly 1 once n reaches
// full_at, so a warm cache is bit for bit the cache before the flush.
double FaultSchedule::warmth(const WarmUp& warm_up, SimTime now) {
  const double requests =
      warm_up.reads_per_copy * seconds(SimTime{now.count() - warm_up.at.count()});
  if (requests >= warm_up.full_at) return 1.0;
  if (!warm_up.popularity) return requests / warm_up.full_at;
  return caching::refill_hit_ratio(warm_up.capacity, *warm_up.popularity, requests) /
         warm_up.steady;
}

// What one fault does to its node while it lasts (the table in the header comment). cache_flush
// and dc_down never get here: they are resolved apart (add_flush, add_dc_down).
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
    case model::FaultKind::cache_flush:
    case model::FaultKind::dc_down:
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
bool FaultSchedule::active(SimTime at, SimTime duration, SimTime now) {
  return at.count() <= now.count() && now.count() - at.count() < duration.count();
}

// Faults on one node compose: factors multiply (two capacity faults of 0.5 leave 0.25 of the
// capacity; a node_down of 1 of 4 copies under a capacity fault of 0.5 leaves 0.375) and extra
// latencies add (1 ms and 2 ms spikes make a service time 3 ms longer). Overlapping traffic_spike
// faults multiply without a cap: a curve's `spikes` cap their product at 1e6 (the loader's bound,
// workload::RateCurve), the faults list has no such bound, as a flash crowd on a flash crowd is the
// scenario's to state. A dc_down composes like the node_down it is. The hit ratio factor is not
// composed: the latest flush sets it (warmth_at).
void FaultSchedule::compose(Condition& into, const Condition& effect) {
  into.capacity_factor *= effect.capacity_factor;
  into.extra_latency += effect.extra_latency;
  into.rate_factor *= effect.rate_factor;
}

}  // namespace simsys::engine
