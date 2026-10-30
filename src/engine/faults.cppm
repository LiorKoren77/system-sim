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
//   node_down      capacity × (R − r) / R   r of the node's R copies down (all of them: × 0); on a
//                                           sql_db the primaries × 0 (`primary`) or the read
//                                           replicas × (N − 1 − r) / (N − 1); on a kv_cluster r
//                                           machines down, their keys' quorums met among fewer
//                                           replicas (Condition::machines_down)
//   capacity       capacity × factor        a degraded tier; a request still takes its service time
//   latency_spike  service time + extra     a slow dependency (its servers drain c / (s + extra))
//   traffic_spike  a client's rate × factor a flash crowd, as a `spikes` entry on its rate curve
//   cache_flush    hit ratio × h(t) / h     the cache emptied at `at`, warming back (WarmUp below)
//   dc_down        node_down on every node of the datacenter, and capacity × 0 on every link that
//                  crosses its edge (link_condition_at)
// Captured: when a fault starts and ends, how much capacity it takes, that faults on one node add
// up, that a request waiting at a node that is down waits until it is back (Condition::back_in_s),
// and how fast a flushed cache refills. Ignored: how the failure is detected and how long failover
// takes (a node is down from `at` exactly and back at full capacity at `at + for`, with no restart
// or warm-up of its own: a sql_db's primary comes back at `at + for` rather than a replica being
// promoted), which copies go down (the survivors share the load evenly: a copy's capacity is
// scaled, see fluid::Derating; a kv_cluster's down machines are cool ones, see
// behaviours::KvClusterBehaviour), what clients do about it (retries are phase 3's Task 10), and
// ch. 1's geoDNS failover: a dc_down cuts the datacenter off, and only the scenario's own links (a
// second route the client splits its requests over) send anything elsewhere.
//
// C++20 module partition, interface kind: `export module simsys.engine:faults;` names a piece of
// the module simsys.engine whose exports become part of the module's interface once the primary
// interface re-exports it (`export import :faults;` in engine.cppm). An importer of simsys.engine
// sees engine::FaultSchedule as if engine.cppm declared it; nobody outside the module can import
// the partition by name. It replaces one more section in a 2000-line interface (or a header that
// belongs to no module): the fault schedule is a part of the engine with its own file.
//
// C++20 global module fragment: `module;` opens the part of the unit before the module declaration,
// where the std headers are #included. Their declarations stay in the global module, shared with
// every other unit that includes them, instead of being attached to simsys.engine as an #include
// after `export module` would do (src/core/core.cppm has the longer note).
module;
// NO <chrono>/<format>/<print>/<filesystem> here: this unit imports other modules (GCC 14.2 'Bad
// file data' bug, CLAUDE.md §3). SimTime's member operators (+=) work without <chrono>; its
// comparisons and differences go through .count().
#include <algorithm>  // std::ranges::contains, std::max
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>  // the == of the node ids: a std header's operators are not exported by an import
#include <utility>  // std::unreachable
#include <vector>

export module simsys.engine:faults;

// C++20 `import`: the compiled interfaces of the modules this partition reads (the Che warm-up
// from caching, SimTime from core, the resolved graph, the typed faults from model), replacing
// #includes of their headers; their names are visible here, their macros and includes are not.
// The partition does not import simsys.engine itself: the primary interface re-exports it, so
// that import would be a cycle, which the standard forbids.
import simsys.caching;
import simsys.core;
import simsys.graph;
import simsys.model;

// C++20 `export namespace`: every declaration in the block is exported (part of the module's
// interface), replacing an `export` keyword on each one; the block below, with no `export`, holds
// what only this unit sees.
export namespace simsys::engine {

// What a node (or a link) is in at one time: the factors and the extra latency its active faults
// put on the numbers its queueing model is built from. The default is the identity, which changes
// nothing: what a node gets outside every fault, so a run without faults is the run before faults
// existed. A link reads only capacity_factor (a dc_down's 0) and back_in_s.
struct Condition {
  double capacity_factor = 1;  // × capacity: node_down (R − r) / R, all copies 0; capacity: factor
  // A sql_db's two kinds of copy (book ch. 1, "database replication"), apart from the factor above:
  double primary_factor = 1;  // × its primaries' capacity: `replicas: primary`, 0
  double replica_factor = 1;  // × its read replicas': `replicas: r`, (N − 1 − r) / (N − 1)
  int machines_down = 0;      // a kv_cluster's machines down: `replicas: r` (behaviours::
                              // KvClusterBehaviour meets each quorum among the live replicas)
  // The seconds until the node serves again when a fault took all its capacity, or all of a
  // sql_db's primaries or read replicas: the latest end of the zero-capacity faults on it, chained
  // when one starts before another ends; 0 outside such a fault. What a request admitted to its
  // room waits before the backlog ahead of it starts to drain (fluid::Derating).
  double back_in_s = 0;
  SimTime extra_latency{};      // + service time: latency_spike's extra
  double rate_factor = 1;       // × a client's rate: traffic_spike's factor
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

  // The seconds the flush of cache `cache` at `at` takes to warm back: until its hit ratio factor
  // is 1 again, full_at / reads_per_copy (WarmUp: t_c / rate for a che cache, keys / rate for a
  // fixed ratio). Empty when no such flush changes anything (a cache without a hit ratio) or the
  // cache is handed no reads, so that it never fills.
  [[nodiscard]] std::optional<double> warm_up_s(std::size_t cache, SimTime at) const;

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
  [[nodiscard]] static Condition down_effect(const model::Fault& fault,
                                             const model::NodeSpec& node);
  [[nodiscard]] static Condition database_down(const model::Fault& fault, int copies);
  [[nodiscard]] static double surviving_share(int down, int copies);
  [[nodiscard]] static bool changes_nothing(const Condition& c);
  [[nodiscard]] static bool is_outage(const Condition& effect);
  [[nodiscard]] static bool active(SimTime at, SimTime duration, SimTime now);
  static void compose(Condition& into, const Condition& effect);
  void whole_cluster_down(std::size_t node, Condition& condition) const;
  [[nodiscard]] double back_in_s(std::size_t node, SimTime now) const;
  [[nodiscard]] double link_back_in_s(std::size_t link, SimTime now) const;
  [[nodiscard]] double warmth_at(std::size_t node, SimTime now) const;
  [[nodiscard]] static double warmth(const WarmUp& warm_up, SimTime now);

  std::vector<Scheduled> faults_;     // in the scenario's order
  std::vector<Cut> cuts_;             // dc_down's links, in the scenario's order
  std::vector<WarmUp> warm_ups_;      // in the scenario's order
  std::vector<std::size_t> flushed_;  // flushed_caches()
  std::vector<int> kv_machines_;      // per node: a kv_cluster's machines, 0 for any other node
};

// ---- What a fault cost: the record behind the report's block per fault (spec §4.4) ----
//
// Models: the incident report (a postmortem) a team writes after an outage, in the three questions
// System Design Interview (Alex Xu, vol. 1) asks of every failure in ch. 1 ("if one server goes
// offline...", "if the master database goes offline...", "if any damage happens to one data
// center...") and again in ch. 12: what did it cost while it lasted, what came after it, and when
// was the system itself again. One record per fault, filled by the engine step by step
// (engine::FaultLog), printed by the report as three lines:
//   during    the blast radius: what the faulted node was sent and what it served (means over the
//             fault), the requests its full room dropped, the backlog it built, and the SLOs the
//             fault broke that held before it (`flipped`)
//   after     the follow-on, the work a fault leaves behind once it has cleared: the backlog to
//             drain (the requests that waited for the node to come back), a flushed cache's
//             warm-up (ch. 1: every read misses an empty cache and goes to the database until it
//             is full again); later a gateway's reconnect storm (Task 15) and a client's extra
//             attempts (Task 10)
//   recovery  the time to recover, the number an incident report and a scenario's README quote:
//             the first step after the fault clears at which every client op's p99 and every
//             node's utilisation (rho) are within 1 % of the step before the fault
// Why "within 1 % of before" and not "the SLOs pass again": an SLO with room to spare passes
// while a backlog is still draining and the system is far from its normal state, and one that
// already failed before the fault never passes again; neither tells when the fault is over. Every
// system has a state it was in before the fault, and being back in it is the question. 1 % is
// tight enough that a queue still draining (rho 1 while it works off the backlog) does not count,
// and loose enough that a fluid model's last digits do not delay it for ever.
// Ignored: anything the run does not show (the time to detect the failure, the failover itself:
// FaultSchedule's header), a rate curve's own drift (a diurnal curve moves rho too; over seconds it
// is far under 1 %, over hours it is not, and a fault whose system never returns to the numbers
// of the step before it is reported "not recovered"), and a fault at the run's first step, which
// has no step before it to return to: no SLO can flip and no recovery is judged.
struct FaultAfter {
  // Seconds from the end of the fault to the end of the first step whose backlog at the faulted
  // node is empty (0 when it was empty as the fault ended); empty when the fault built no backlog
  // or the backlog was not drained by the end of the run.
  std::optional<double> backlog_drained_in_s{};
  // A cache_flush only: seconds from the flush until the cache is full again (its hit ratio
  // factor back at 1: FaultSchedule::warm_up_s), once the run got that far; empty otherwise.
  std::optional<double> cache_warm_in_s{};
};

// One fault's record. The faulted part is the node the fault names; for a dc_down it is the
// datacenter: its nodes (but its clients) summed, plus the links the cut crosses (their requests
// sent and lost, in requests, beside the nodes' capacity units); for a traffic_spike it is the
// client, whose numbers are its requests (engine::ResponseSample): offered (the spiked rate),
// served = answered, lost = offered − answered (lost on the way or timed out), and no backlog or
// room of its own (the backlog the spike builds downstream is in the node table). Rates and
// amounts are in the node's capacity units (engine::NodeSample), requests when every cost is 1. The
// window "during" is [at, at + for); for a cache_flush, an instant, it is the warm-up [at, at +
// cache_warm_in_s) (to the end of the run if the cache never warms), the time the flush costs
// anything.
struct FaultRecord {
  model::FaultKind kind{};
  std::string node{};  // as the fault names it (empty for a dc_down: see `datacenter`)
  SimTime at{};
  SimTime duration{};      // `for`; 0 for a cache_flush (model::lasts)
  double offered_per_s{};  // the faulted node's demand, the mean over the steps "during"
  double served_per_s{};   // ... and its drain
  double lost{};           // Σ dropped × dt over them: what its full room dropped
  double backlog_peak{};   // the most that waited at the end of any of them
  double room{};           // queue_limit × copies: the room the backlog fills
  std::vector<std::size_t> flipped{};  // indexes into Scenario::slos: held at the step before
                                       // `at`, broken at some step "during" (FaultLog's rule)
  FaultAfter after{};
  // The start of the first step at or after at + for, inside no fault, with every client op's p99
  // and every node's rho within 1 % of the step before `at`; empty when the run ended first.
  std::optional<SimTime> recovered_at{};
  std::string datacenter{};  // a dc_down's datacenter; empty for every other kind
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

// a + b for two non-negative tick counts, at most the largest count: a fault's `for` may be huge
// (the loader does not require it to end inside the run), and the sum must not overflow.
[[nodiscard]] SimTime::rep saturating_sum(SimTime::rep a, SimTime::rep b) {
  const auto most = std::numeric_limits<SimTime::rep>::max();
  return b > most - a ? most : a + b;
}

// The ticks from `now` until the outage made of `windows` ends, 0 when none is on at `now`. A
// window is an outage fault's {at, duration} (FaultSchedule's Scheduled or Cut), and `in_outage`
// says which of `windows` count. The outage is the latest end of those on at `now`, then of every
// later one that starts before that end (it begins while the target is still down, so it stretches
// the outage), until none does: two node_downs over [10 s, 20 s) and [15 s, 30 s) are one outage
// from 10 s to 30 s. Remaining times, not ends (at + for), so that a huge `for` cannot overflow
// (FaultSchedule::active has the same reason).
template <class Window, class Predicate>
[[nodiscard]] SimTime::rep outage_left(const std::vector<Window>& windows, Predicate in_outage,
                                       SimTime now) {
  SimTime::rep left = 0;
  for (const auto& w : windows) {
    const auto since = now.count() - w.at.count();
    if (in_outage(w) && since >= 0 && since < w.duration.count()) {
      left = std::max(left, w.duration.count() - since);
    }
  }
  for (bool stretched = left > 0; stretched;) {
    stretched = false;
    for (const auto& w : windows) {
      const auto starts_in = w.at.count() - now.count();
      if (!in_outage(w) || starts_in <= 0 || starts_in > left) continue;
      const auto ends_in = saturating_sum(starts_in, w.duration.count());
      if (ends_in > left) {
        left = ends_in;
        stretched = true;
      }
    }
  }
  return left;
}

}  // namespace

// Each fault's effect is worked out here, once; a fault that changes nothing is left out, so
// condition_at never meets it.
FaultSchedule::FaultSchedule(const graph::Graph& g, std::span<const double> flush_reads) {
  for (const auto& node : g.nodes()) {
    kv_machines_.push_back(
        node.spec.type == model::NodeType::kv_cluster ? model::copies_of(node.spec) : 0);
  }
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

// Every active fault on the node composed (see compose), the time until it is back from an outage
// (back_in_s), and the warm-up of its latest flush.
Condition FaultSchedule::condition_at(std::size_t node, SimTime now) const {
  Condition condition;
  for (const auto& fault : faults_) {
    if (fault.node == node && active(fault.at, fault.duration, now)) {
      compose(condition, fault.effect);
    }
  }
  whole_cluster_down(node, condition);
  condition.back_in_s = back_in_s(node, now);
  condition.hit_ratio_factor = warmth_at(node, now);
  return condition;
}

// Overlapping node_downs that together take every machine of a kv_cluster leave it nothing: the
// whole node at capacity 0, as one node_down without `replicas` would. No single fault takes it
// all, so back_in_s stays 0: its room drains from the moment the first of them ends.
void FaultSchedule::whole_cluster_down(std::size_t node, Condition& condition) const {
  if (condition.machines_down == 0) return;  // also the empty schedule, which knows no nodes
  if (condition.machines_down < kv_machines_[node]) return;
  condition.capacity_factor = 0.0;
  condition.machines_down = 0;
}

// Capacity 0 while a dc_down cuts the link, and when the cut ends (a pipe's room drains then).
Condition FaultSchedule::link_condition_at(std::size_t link, SimTime now) const {
  for (const auto& cut : cuts_) {
    if (cut.link == link && active(cut.at, cut.duration, now)) {
      return Condition{.capacity_factor = 0.0, .back_in_s = link_back_in_s(link, now)};
    }
  }
  return Condition{};
}

// The seconds until node `node` is back from the outage it is in (outage_left over its faults that
// take all of its capacity or all of a sql_db's primaries or read replicas, is_outage), 0 outside
// one.
double FaultSchedule::back_in_s(std::size_t node, SimTime now) const {
  const auto left = outage_left(
      faults_, [&](const Scheduled& f) { return f.node == node && is_outage(f.effect); }, now);
  return seconds(SimTime{left});
}

// The same for a link a dc_down cuts.
double FaultSchedule::link_back_in_s(std::size_t link, SimTime now) const {
  const auto left = outage_left(cuts_, [&](const Cut& cut) { return cut.link == link; }, now);
  return seconds(SimTime{left});
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

// The step at which warmth reaches 1, as a time from the flush: n = reads_per_copy × t reaches
// full_at at t = full_at / reads_per_copy.
std::optional<double> FaultSchedule::warm_up_s(std::size_t cache, SimTime at) const {
  for (const auto& warm_up : warm_ups_) {
    if (warm_up.cache != cache || warm_up.at.count() != at.count()) continue;
    if (warm_up.reads_per_copy <= 0.0) return std::nullopt;
    return warm_up.full_at / warm_up.reads_per_copy;
  }
  return std::nullopt;
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
      return down_effect(fault, node);
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

// A node_down by the kind of node it takes copies from (R = model::copies_of, its copies; the
// loader keeps `replicas` in 1..R):
//   every copy (no `replicas`, r = R, or a node of one copy)   capacity × 0
//   a sql_db                                                   database_down
//   a kv_cluster, r of its M machines                          machines_down r: the survivors keep
//                       their own load and the requests on the down machines' keys meet their
//                       quorum among fewer replicas (behaviours::KvClusterBehaviour), ch. 6
//   any other node, r of R                                     capacity × (R − r) / R
Condition FaultSchedule::down_effect(const model::Fault& fault, const model::NodeSpec& node) {
  const int copies = model::copies_of(node);
  const bool every_copy = !fault.primary && (!fault.replicas || *fault.replicas >= copies);
  if (every_copy || copies == 1) return Condition{.capacity_factor = 0.0};
  if (node.type == model::NodeType::sql_db) return database_down(fault, copies);
  if (node.type == model::NodeType::kv_cluster) return Condition{.machines_down = *fault.replicas};
  return Condition{.capacity_factor = surviving_share(*fault.replicas, copies)};
}

// A sql_db of N copies per shard, a primary and N − 1 read replicas (book ch. 1, "database
// replication"), with some of them down in every shard:
//   `primary`   the primaries × 0: the writes wait in the primary's room and are dropped once it
//               is full, until it is back (the book promotes a replica instead; the model's
//               primary simply returns at `at + for`); the reads go on at the replicas
//   r of N      the read replicas × (N − 1 − r) / (N − 1), the primary untouched: 1 of 3 copies
//               leaves each replica copy half its capacity (the survivors share the reads evenly,
//               fluid::Derating), and r = N − 1 leaves no replica (reads wait, writes go on)
// Before Task 6's review a `primary` was one copy of N down, every copy at (N − 1) / N, and the
// writes were all still answered.
Condition FaultSchedule::database_down(const model::Fault& fault, int copies) {
  if (fault.primary) return Condition{.primary_factor = 0.0};
  const auto replicas = static_cast<double>(copies - 1);
  return Condition{.replica_factor = (replicas - static_cast<double>(*fault.replicas)) / replicas};
}

// The share of a node's capacity left when `down` of its R copies are down: (R − r) / R. Capacity
// is per copy, and the copies split the load evenly, so the survivors serve (R − r) / R of it: 1 of
// 4 leaves 0.75.
double FaultSchedule::surviving_share(int down, int copies) {
  const auto r = static_cast<double>(copies);
  return (r - static_cast<double>(down)) / r;
}

bool FaultSchedule::changes_nothing(const Condition& c) {
  return c.capacity_factor == 1.0 && c.primary_factor == 1.0 && c.replica_factor == 1.0 &&
         c.machines_down == 0 && c.extra_latency.count() == 0 && c.rate_factor == 1.0 &&
         c.hit_ratio_factor == 1.0;
}

// A fault that takes all of a node's capacity, or all of a sql_db's primaries or read replicas: an
// outage, whose end a request in the room waits for (back_in_s).
bool FaultSchedule::is_outage(const Condition& effect) {
  return effect.capacity_factor == 0.0 || effect.primary_factor == 0.0 ||
         effect.replica_factor == 0.0;
}

// Active at `now` when at <= now < at + for (model::active_at, which `simsys curve` reads a
// traffic_spike's window with too).
bool FaultSchedule::active(SimTime at, SimTime duration, SimTime now) {
  return model::active_at(at, duration, now);
}

// Faults on one node compose: factors multiply (two capacity faults of 0.5 leave 0.25 of the
// capacity; a node_down of 1 of 4 copies under a capacity fault of 0.5 leaves 0.375), a
// kv_cluster's down machines add (two faults of 1 machine: 2 down; all of them: whole_cluster_down)
// and extra latencies add (1 ms and 2 ms spikes make a service time 3 ms longer). Overlapping
// traffic_spike faults multiply without a cap: a curve's `spikes` cap their product at 1e6 (the
// loader's bound, workload::RateCurve), the faults list has no such bound, as a flash crowd on a
// flash crowd is the scenario's to state. A dc_down composes like the node_down it is. The hit
// ratio factor is not composed: the latest flush sets it (warmth_at); nor is back_in_s, which
// condition_at reads off the outages (back_in_s).
void FaultSchedule::compose(Condition& into, const Condition& effect) {
  into.capacity_factor *= effect.capacity_factor;
  into.primary_factor *= effect.primary_factor;
  into.replica_factor *= effect.replica_factor;
  into.machines_down += effect.machines_down;
  into.extra_latency += effect.extra_latency;
  into.rate_factor *= effect.rate_factor;
}

}  // namespace simsys::engine
