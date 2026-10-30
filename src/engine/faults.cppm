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
//   cache_flush    hit ratio × h(n) / h     the cache emptied at `at`, warming back on the n reads
//                                           it gets from then on (WarmUp below)
//   dc_down        node_down on every node of the datacenter, and capacity × 0 on every link that
//                  crosses its edge (link_condition_at)
// Captured: when a fault starts and ends, how much capacity it takes, that faults on one node add
// up, that a request waiting at a node that is down waits until it is back (Condition::back_in_s),
// how fast a flushed cache refills, and the failover of a node its callers find through a service
// discovery (below). Ignored: how a load balancer detects the failure (it routes around a dead copy
// from `at` exactly), restarts and warm-ups (a node is back at full capacity at `at + for`: a
// sql_db's primary comes back at `at + for` rather than a replica being promoted), which copies go
// down (the survivors share the load evenly: a copy's capacity is
// scaled, see fluid::Derating; a kv_cluster's down machines are cool ones, see
// behaviours::KvClusterBehaviour), what clients do about it (a client's `retries` are its own
// model, behaviours::ClientBehaviour: the schedule only takes the capacity away), ch. 1's geoDNS
// failover: a dc_down cuts the datacenter off, and only the scenario's own links (a second route
// the client splits its requests over) send anything elsewhere, and ch. 1's data synchronisation
// between datacenters: a dc_down has no replication lag and loses no writes, the other
// datacenter's stores are assumed current (and its caches come back warm at `at + for`: a cold
// restart is a cache_flush the scenario scripts at that time).
//
// Failover behind a service discovery (phase 3 spec §8.1; book ch. 12). In ch. 12's chat system
// the client does not reach a chat server through a load balancer: it asks the service discovery
// (ZooKeeper) for one and then talks to the server it was handed. A registry learns that a server
// died only when the server's session or health check times out; until then it keeps handing out
// the dead address, and every request sent there is lost. The model: when an op reaches a node
// THROUGH a lookup (its steps ask a service_discovery `lookup` before a step to the node, per step:
// graph::Graph::discovery_before_step; a FailoverRoute below) and a node_down takes r of the node's
// R replicas, then for min(failover, the fault's `for`) from the fault's start the share r / R of
// that op's requests still goes to the dead replicas (dead_share; `failover` is the discovery's,
// NodeSpec::failover, 10 s by default). Those requests count at the node as demand and as drops,
// never as drain, and the caller's op loses them (behaviours::detail::HopBlocks::fail_over); the
// other (R − r) / R reach the survivors, which have their own (R − r) / R of the capacity, so
// they see the load per copy they had before the fault. Once the window closes the registry knows,
// every request goes to the survivors and they queue (the node_down above: R / (R − r) times the
// load per copy). r = R (every replica down) makes the share 1: for the window each looked-up
// request is lost at once instead of waiting in the room for the node to come back. A node_down
// on a node reached WITHOUT a lookup has no failover: the load balancer in front (ch. 1) spreads
// the load over the survivors from the first step. Only a node_down fails over: a capacity
// fault degrades a tier whose copies all stay alive, so nothing is sent to a dead one, and a
// dc_down is cut off by its links. A sql_db's or a kv_cluster's partial node_down has no failover
// either: their clients find the live copies by the database's own replication and quorums, not
// through a registry. Ignored: the registry's own availability (a node_down of the discovery takes
// its capacity like any service's, and the addresses its callers hold stay good), partial
// knowledge (every caller learns of the death at the same moment, `failover` after it), the client
// sending a failed request to another server (that is a client's `retries`, which see the lost
// share like any loss), the link delay of the dead share (a request handed a dead address at
// `t` is lost at the node when it arrives there, but whether it was is decided at `t`), a registry
// with no live replica to hand out (with every replica down, the requests after the window wait in
// the node's room for it to come back, where a real registry would fail them at once), and a
// lookup in a `then` list (add_failover_routes walks the steps of each op's own list only).
//
// Servers that die with what they hold (down_faults): a node_down and a dc_down both kill the
// servers they take, a dc_down every server of its datacenter, so a ws_gateway loses the open
// connections those servers held and its users storm back when the fault clears (the reconnect
// storm, behaviours::WsGatewayBehaviour, phase 3 Task 15; a dc_down since the Task 15 review).
// A capacity fault does not: its servers are slow, not gone, and their sockets stay open.
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
#include <algorithm>  // std::ranges::contains, std::max, std::min
#include <cstddef>
#include <limits>
#include <optional>
#include <ranges>  // std::views::keys: a caller's op names (add_failover_routes)
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
import simsys.defaults; // failover: a service discovery's, when a scenario built in code has none
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
  double hit_ratio_factor = 1;  // × a cache's hit ratio: cache_flush's warm-up, h(n) / h
};

// One fault that has killed some of a node's servers (FaultSchedule::down_faults): which one, as
// its place in the schedule (the same for every step it lasts, so a step can tell a fault that
// has just struck from one that was already on), and the share r / R of the servers it took.
struct DownFault {
  std::size_t fault{};
  double share{};
};

// One way a caller reaches a node through a service discovery (FaultSchedule's failover, the
// header comment): node `caller` has an op whose steps ask `discovery` for `target` before calling
// it (graph::Graph::discovery_before_step), and the discovery hands out a dead replica for
// `failover` (its NodeSpec::failover).
struct FailoverRoute {
  std::size_t caller{};
  std::size_t discovery{};
  std::size_t target{};
  SimTime failover{};
};

// The scenario's faults, resolved once against its graph: which node or link each acts on, when,
// and the Condition it puts it in while it lasts. Built by engine::Simulation, which also hands it,
// after every step, the one thing the graph cannot tell: the reads each flushed cache was sent in
// that step (count_reads), which its warm-up counts.
class FaultSchedule {
 public:
  FaultSchedule() = default;  // no faults: the identity for every node and link, at every time

  // `g.scenario().faults` resolved against `g`, every flushed cache still empty (it has been sent
  // no reads yet: count_reads). Throws std::invalid_argument for a cache_flush on a fixed-ratio
  // cache without `keys` (Simulation::create refuses that scenario first): a programmer error.
  explicit FaultSchedule(const graph::Graph& g);

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
  // misses (Simulation::warm_flushed_caches), and whose reads it counts (count_reads). Empty
  // without a flush.
  // C++20 std::span<const std::size_t>: a non-owning read-only view of contiguous elements (here
  // the schedule's own vector), so the caller iterates them without a copy and without being tied
  // to the container; replaces returning a `const std::vector<std::size_t>&`.
  [[nodiscard]] std::span<const std::size_t> flushed_caches() const { return flushed_; }

  // The reads cache `cache` (all its copies together, its write ops left out) was sent in the step
  // [now, now + dt), at `reads_per_s`: what its warm-up counts (WarmUp). Each flush of the cache
  // counts the part of the step from its `at` to the next flush of the cache, if any, and nothing
  // once the cache is full again. The engine calls it after every step's forward pass
  // (Simulation::count_flushed_reads), so the hit ratio of the next step is h(n) at the reads
  // seen so far. Nothing to count on a node never flushed.
  void count_reads(std::size_t cache, SimTime now, SimTime dt, double reads_per_s);

  // The seconds the flush of cache `cache` at `at` took to warm back: from `at` to the moment
  // inside the step during which the reads counted since the flush reached the full point
  // (WarmUp), once count_reads has got there. Empty before that, for ever when the cache never
  // fills (it is sent too few reads, or the next flush empties it first), and when no such flush
  // changes anything (a cache without a hit ratio).
  [[nodiscard]] std::optional<double> warm_up_s(std::size_t cache, SimTime at) const;

  // Every way a caller reaches, through a discovery lookup, a node that some node_down of the
  // scenario takes replicas from (FailoverRoute), each once, in (caller, op, step) order: the
  // routes whose dead share the engine hands the callers every step. Empty without such a fault.
  [[nodiscard]] std::span<const FailoverRoute> failover_routes() const { return routes_; }

  // The share of the requests a caller that looked `node` up sends to its dead replicas at `now`
  // (the header comment), with a discovery of failover `failover`: 1 − Π (R − r_i) / R over the
  // node_downs i on the node whose window [at, at + min(failover, for)) holds `now`: r / R for one,
  // 1 when every replica is down; 0 outside every window.
  [[nodiscard]] double dead_share(std::size_t node, SimTime now, SimTime failover) const;

  // The faults that have killed servers of `node` at `now` (the header's "servers that die with
  // what they hold": every node_down and dc_down on it active at `now`), each with the share of
  // its servers it took, r / R (1 for every replica, and for a dc_down), in the scenario's order:
  // what a ws_gateway's connections follow (behaviours::WsGatewayBehaviour::follow_outages, the
  // reconnect storm). Empty outside them, which is every step of a scenario without one.
  [[nodiscard]] std::vector<DownFault> down_faults(std::size_t node, SimTime now) const;

  // The share r / R that `fault` sends to dead replicas of `node` during its failover: its
  // node_down effect's lost capacity, 1 − (R − r) / R; 0 for every other kind, and for a sql_db's
  // or a kv_cluster's partial node_down, which leave the capacity factor alone (the header
  // comment).
  [[nodiscard]] static double dead_share_of(const model::Fault& fault, const model::NodeSpec& node);

 private:
  // One fault that changes something on one node (a dc_down is one per node of the datacenter):
  // the node, when, and its effect alone.
  struct Scheduled {
    std::size_t node{};
    SimTime at{};
    SimTime duration{};
    Condition effect{};
    bool kills_replicas{};  // a node_down: callers that looked the node up fail over (dead_share)
    bool kills_servers{};   // a node_down or a dc_down: what its servers held dies (down_faults)
  };

  // A link a dc_down cuts, and for how long.
  struct Cut {
    std::size_t link{};
    SimTime at{};
    SimTime duration{};
  };

  // A flushed cache refilling (spec §4.3; docs/study/numerics.md, "warm-up", has the derivation).
  // ONE copy of the cache has seen n requests since the flush: the sum of the reads the engine
  // counted, step by step (count_reads), each step's reads × `share_per_copy`. Each of its R copies
  // sees 1/R of the reads and fills its own entries (a ring's copies hold different keys, so a
  // partitioned che cache counts as one cache of keys × R seeing every read: partitions / R). n is
  // the integral of the reads the cache actually gets, so a cache flushed while nothing reads it
  // stays empty until the reads come, and one flushed on a rising curve fills faster as it rises
  // (at a constant rate n = rate × t). Its hit ratio for a step is then, at the n of the steps
  // before it and relative to the steady h it had before the flush:
  //   che       h(n) / h = refill_hit_ratio(C, popularity, n) / che_hit_ratio(C, popularity)
  //             (caching::refill_hit_ratio: Σ p_i (1 − e^{−p_i n}), the hit ratio of an LRU cache
  //             whose characteristic time is n, while its D(n) = Σ (1 − e^{−p_i n}) distinct keys
  //             are fewer than C), full at n = t_c, Che's characteristic time of C, where
  //             D(t_c) = C by definition
  //   fixed     n / keys: linear in the reads, full after `keys` of them. The simple case, and an
  //             optimistic one: it has every read add an entry, but once h(n) = h · n / keys only
  //             the misses do. The consistent model, dE/dn = 1 − h · E / K for K = keys entries,
  //             gives E(n) = (K / h)(1 − e^{−h n / K}), full at n = −(K / h) ln(1 − h): 2.56 K at
  //             h = 0.9, not K. A fixed ratio has no popularity to say which, so the simple case
  //             is kept (and pinned: 0.45 at half the fill of a 0.9 cache)
  // and exactly 1 from the moment it is full (`full_at`, in requests per copy). 0 at the flush.
  // Ignored: that an LRU cache refills in recency order (the recent keys are a little hotter than
  // "distinct"), the thundering herd of misses on the store, and the keys a write would put back.
  struct WarmUp {
    std::size_t cache{};
    SimTime at{};
    // The next flush of the same cache, which empties it again: this warm-up counts no reads from
    // then on (warmth_at reads the later one). Empty when there is none.
    std::optional<SimTime> until{};
    double share_per_copy{};  // × the cache's reads: one copy's share (partitions / R, 1 / R)
    double full_at{};         // requests per copy: keys (fixed) or t_c (che)
    double seen{};            // n: the requests one copy has seen since the flush (count_reads)
    std::optional<double> warm_in_s{};  // warm_up_s, once `seen` reaches `full_at`
    // che only: the popularity the cache's reads draw from, its capacity C in keys and its steady
    // Che hit ratio. Empty for a fixed ratio.
    std::optional<caching::Popularity> popularity{};
    double capacity{};
    double steady{1.0};
  };

  void add_node_fault(const graph::Graph& g, const model::Fault& fault);
  void add_dc_down(const graph::Graph& g, const model::Fault& fault);
  void add_flush(const graph::Graph& g, const model::Fault& fault);
  void end_at_next_flushes();
  void add_failover_routes(const graph::Graph& g);
  void add_route(FailoverRoute route);
  [[nodiscard]] static WarmUp warm_up_of(const graph::Graph& g, std::size_t cache, SimTime at);
  [[nodiscard]] static double counted_s(const WarmUp& warm_up, SimTime now, SimTime dt);
  static void add_reads(WarmUp& warm_up, SimTime now, double reads_per_s, double counted_s);
  [[nodiscard]] static Condition effect_of(const model::Fault& fault, const model::NodeSpec& node);
  [[nodiscard]] static Condition down_effect(const model::Fault& fault,
                                             const model::NodeSpec& node);
  [[nodiscard]] static Condition database_down(const model::Fault& fault, int copies);
  [[nodiscard]] static double surviving_share(int down, int copies);
  [[nodiscard]] static bool changes_nothing(const Condition& c);
  [[nodiscard]] static bool is_outage(const Condition& effect);
  [[nodiscard]] static bool active(SimTime at, SimTime duration, SimTime now);
  [[nodiscard]] double killed_share(std::size_t node, SimTime now, SimTime window) const;
  static void compose(Condition& into, const Condition& effect);
  void whole_cluster_down(std::size_t node, Condition& condition) const;
  [[nodiscard]] double back_in_s(std::size_t node, SimTime now) const;
  [[nodiscard]] double link_back_in_s(std::size_t link, SimTime now) const;
  [[nodiscard]] double warmth_at(std::size_t node, SimTime now) const;
  [[nodiscard]] static double warmth(const WarmUp& warm_up);

  std::vector<Scheduled> faults_;      // in the scenario's order
  std::vector<Cut> cuts_;              // dc_down's links, in the scenario's order
  std::vector<WarmUp> warm_ups_;       // in the scenario's order
  std::vector<std::size_t> flushed_;   // flushed_caches()
  std::vector<int> kv_machines_;       // per node: a kv_cluster's machines, 0 for any other node
  std::vector<FailoverRoute> routes_;  // failover_routes()
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
//             fault), what the fault cost (`lost`, a flush's `extra_misses`), the backlog it built;
//             the report adds what it did to the SLOs (report::FaultBlock)
//   after     the follow-on, the work a fault leaves behind once it has cleared: the backlog to
//             drain (the requests that waited for the node to come back), a flushed cache's
//             warm-up (ch. 1: every read misses an empty cache and goes to the database until it
//             is full again), the attempts the clients' retries added and the jobs the worker
//             pools re-published (below), and a gateway's reconnect storm (ch. 12: every
//             connection a dead chat server held comes back at once; below)
//   recovery  the time to recover, the number an incident report and a scenario's README quote
//
// The baseline is a counterfactual: a TWIN run, the same scenario with every fault stripped,
// stepped in lockstep with the run (engine::Simulation owns it when the scenario has faults). An
// incident report compares the day of the incident with the day that would have been, not with
// the minute before: on a diurnal curve the minute before is a different load, at the run's first
// step there is none, and inside another fault it is itself broken. The twin's same step is the
// system as it would be at that moment without the faults, so:
//   lost         the faulted part's drops minus the twin's, summed over the steps from `at` until
//                the recovery step (to the end of the run when it never recovers): the drops of the
//                drain after the fault included, the drops of an overload the twin has too excluded
//   recovery     the first step that starts once the fault has cleared (at + for; a flush's `at`)
//                at which every client op's p99 and every node's utilisation (rho) are within 1 %
//                of the twin's same step (relative, with 1e-9 of absolute slack for float residue)
// Why "within 1 % of the twin" and not "the SLOs pass again": an SLO with room to spare passes
// while a backlog is still draining and the system is far from its normal state, and one that
// fails without the fault never passes again; neither tells when the fault is over. The twin is
// the normal state, at every moment. 1 % is tight enough that a queue still draining (rho 1 while
// it works off the backlog) does not count, and loose enough that a fluid model's last digits do
// not delay it for ever. The cost: a run with faults steps the scenario twice (a run without
// faults has no twin, and is the run it was).
// Overlapping faults on one target split nothing: each record counts from its own `at` to its own
// recovery, so two outages of one node that overlap both count the steps they share (the report
// says `(overlaps fault 00:00:10)` on the later block, so that nobody adds them). The failover's
// after lines overlap the same way: each counts the dead-replica share over the other's window too.
// Units: the faulted part counts in its own unit (engine::NodeSample: capacity units, which are
// requests when every cost is 1, and MEGABYTES on a byte-sized node, defaults::is_byte_sized):
// `lost_unit` says which, and the rates, the backlog and the room are in it too. A dc_down's links
// across the datacenter's edge lose requests, counted apart (`lost_on_cut_links`), never added to
// its nodes' figure. A datacenter whose nodes mix the two units counts its request-counting nodes
// only, in requests (the megabytes of its byte-sized nodes are in the node table): one unit per
// record. A traffic_spike's faulted part is the client, in requests.
// Retries (a client's `retries`, Task 10; a worker pool's, Task 11): ch. 1's retry storm, where a
// failing server is sent every request several times. A node's drops then count ATTEMPTS, not
// requests: a request sent three times and dropped three times is three drops, and a request
// dropped twice and answered on its third attempt is two drops and no loss to its user. Both
// sides are true and an incident report needs both: the attempts are the servers' work (what the
// faulted node turned away, what the backlog, the room and the rates beside them count), the
// requests lost for good are what the users saw. So when a retrying client's paths reach the
// faulted part, or a retrying pool calls into it, `lost_unit` is "attempts" (MB stays MB) and
// `lost_requests` holds the requests lost for good: Σ over the clients whose paths reach the part
// of their final losses (the last attempts lost on the way or timed out, ResponseSample::lost +
// timed_out), minus the twin's, over the same span as `lost`. A pool's failed calls are not a
// user's loss (its client's publish was answered by the queue): they come back as re-published
// jobs, `after`'s `pool_republished_jobs`, and the last attempt's failures are dead letters (the
// node table's). A traffic_spike's `lost` is already the client's requests lost for good (not
// offered − answered: in a step with retries the client answers attempts of earlier requests and
// leaves unanswered requests it will send again). `after` adds what the retries cost beyond the
// twin's, from `at` to the recovery: the clients' attempts minus their offered load
// (`retries_added_attempts`) and the pools' re-published jobs. Without any retrying client or
// pool nothing of this appears (`lost_requests` empty, both figures 0): the block is the one
// written before retries existed.
// Failover (FaultSchedule's header): a node_down on a node that some op reaches through a service
// discovery keeps that op's callers on the dead replicas for the discovery's `failover`; what they
// sent there is part of `lost` (the node's drops), and `after` says how much of it (failover_lost:
// `failover 10s: 2.5k requests sent to the dead replica`, in `attempts` when retries reach the
// node, as `lost` is). Ignored: anything the run does not show (how a load balancer detects the
// failure: FaultSchedule's header).
// The reconnect storm (behaviours::WsGatewayBehaviour has the model): a node_down of a ws_gateway,
// or a dc_down of its datacenter, drops the connections its dead servers held, and they reconnect
// over `reconnect_within` from the fault's end. `after` names the storm's size (the connections
// THIS fault dropped, at its first step: a fault that strikes while another is on drops its share
// of what the survivors hold, and two that strike in the same step share that step's drop as the
// gateway took it, in the scenario's order), how long it lasts, and the gateway's PEAK OFFERED
// UTILISATION over the storm, demand / total capacity (capacity per replica × replicas), the
// number explain's checks print as "peak offered ... (148%)", uncapped: the node table's rho is
// the busy fraction and stops at 100 %, which would hide by how much a storm overloads the tier.
// With the moment it is first reached: where the storm and the heartbeats of the refilling level
// meet. The storm's window is [at + for, at + for + reconnect_within + one step]: not inside the
// fault, where the capacity is 0 and every arrival is "over 100 %", and not after the storm, where
// a later fault or a flash crowd is not this storm's (the Task 15 review read it to the end of the
// run). The "one step" holds the step during which the storm's last connects land when
// reconnect_within is not a whole number of steps, and the first without them.
struct FaultAfter {
  // Whose backlog the fault left above the twin's: the faulted node, a dc_down's datacenter, a
  // traffic_spike's downstream node with the most work left (backlog / capacity, in seconds, over
  // the twin's). Empty when the fault left none, and for a cache_flush (whose follow-on is its
  // warm-up).
  std::string backlog_node{};
  // Seconds from the end of the fault to the end of the first step at which that backlog is back
  // at the twin's (0 when it was as the fault ended); empty when there is none, or it was not
  // back by the end of the run.
  std::optional<double> backlog_drained_in_s{};
  // A cache_flush only: seconds from the flush until the cache is full again (its hit ratio
  // factor back at 1: FaultSchedule::warm_up_s), once the run got that far; empty otherwise, and
  // when the next flush of the cache emptied it first (below).
  std::optional<double> cache_warm_in_s{};
  // A cache_flush only: the `at` of the next flush of the same cache when it struck before this
  // one's cache was full again. That flush empties the cache and starts a warm-up of its own, so
  // this one's window ends there: it never got warm, and the misses from then on are the later
  // flush's. Empty otherwise.
  std::optional<SimTime> interrupted_by_flush_at{};
  // The retries the fault set off (ch. 1's retry storm), each over the twin's from `at` to the
  // recovery, 0 when there were none. `retries_added_attempts`: Σ (attempts − offered) of every
  // client whose paths reach the faulted part (a traffic_spike: its client), the requests sent
  // again. `pool_republished_jobs`: Σ of the jobs the retrying worker pools that call into it
  // re-published to their queues (NodeSample::republished_jobs_per_s), those pools' ids joined by
  // ", " in `republishing_pools`.
  double retries_added_attempts{};
  std::string republishing_pools{};
  double pool_republished_jobs{};
  // A node_down on a node some op reaches through a service discovery (FaultSchedule's failover):
  // the requests the registry sent to the dead replicas before it noticed, Σ over the faulted
  // node's dead-replica losses (NodeSample::failover_lost_per_s) × dt from `at` to the recovery
  // (the twin has none). Empty when the fault has no failover (another kind, or no lookup before
  // the node). `failover` is the window that applied: the discovery's (the first route's, when
  // several discoveries hand the node out) cut to the fault's `for`, min(failover, for); and
  // `dead_replicas` the replicas the fault took (r, or all R).
  std::optional<double> failover_lost{};
  SimTime failover{};
  int dead_replicas{};
  // A node_down of a ws_gateway, or a dc_down of a datacenter that holds one, only (the reconnect
  // storm, FaultRecord's header): the gateways' ids (", " between several), the connections the
  // fault dropped (its share of NodeSample::connections_dropped at its first step),
  // `reconnect_within` (they come back evenly over it from the fault's end), and the gateways'
  // peak OFFERED utilisation over the storm, demand / total capacity (a fraction that passes 1
  // when the storm overloads them; not a node's `peak_rho`, the busy fraction), with the start of
  // the step that first reached it. The storm's size is empty for every other fault; the peak,
  // until a step after the fault has been observed.
  std::string reconnect_storm_gateways{};
  std::optional<double> reconnect_storm_connections{};
  SimTime reconnect_storm_over{};
  std::optional<double> peak_offered_rho{};
  std::optional<SimTime> peak_offered_rho_at{};
};

// One fault's record. The faulted part is the node the fault names; for a dc_down it is the
// datacenter: its nodes (but its clients) summed, and the links the cut crosses apart; for a
// traffic_spike it is the client, whose numbers are its requests (engine::ResponseSample): offered
// (the spiked rate), served = answered, lost = the requests lost for good (their last attempt lost
// on the way or timed out: offered − answered without retries), and no backlog or room of its own
// (the backlog the spike builds is downstream: `after`). The window "during" is [at, at + for); for
// a cache_flush, an instant, it is the warm-up [at, at + flush_window_s): until the cache is full
// again, or until the next flush of the same cache empties it first (FaultAfter's
// interrupted_by_flush_at), to the end of the run if neither happens: the time the flush costs
// anything. The run alone finds out when that is (the warm-up counts the reads the cache gets:
// FaultSchedule::count_reads). What a flush costs is not a loss (its cache drops nothing) but the
// reads its empty cache sends on: `extra_misses`, the origin's demand minus the twin's, summed
// over the window.
struct FaultRecord {
  model::FaultKind kind{};
  std::string node{};  // as the fault names it (empty for a dc_down: see `datacenter`)
  SimTime at{};
  SimTime duration{};      // `for`; 0 for a cache_flush (model::lasts)
  double offered_per_s{};  // the faulted part's demand, the mean over the steps "during"
  double served_per_s{};   // ... and its drain
  double lost{};           // Σ (dropped − the twin's dropped) × dt from `at` to the recovery
  std::string lost_unit{"requests"};  // the faulted part's unit: "requests", "attempts" or "MB"
  // With retries involved (lost_unit "attempts"; FaultRecord's header): the requests lost for good,
  // Σ over the clients whose paths reach the faulted part of their final losses
  // (ResponseSample::lost + timed_out) minus the twin's, from `at` to the recovery. Empty when no
  // retrying client or pool reaches the faulted part.
  std::optional<double> lost_requests{};
  double lost_on_cut_links{};  // a dc_down: the same over the links it cuts, in requests
  double backlog_peak{};       // the most that waited at the end of any step "during"
  double room{};               // queue_limit × copies: the room the backlog fills
  // A cache_flush: Σ (the origin's demand − the twin's) × dt over the warm-up, in the origin's
  // unit; the origin is the node the cache's misses go to (its backend, a CDN's origin, or the
  // `on: miss` steps after a lookup of it), its id (several joined by ", ").
  double extra_misses{};
  std::string origin{};
  std::string origin_unit{"requests"};
  // A cache_flush: the length of its window "during" (above), in seconds, set at the step at which
  // the run finds it: the warm-up FaultSchedule::warm_up_s gives, or the time to the next flush
  // of the cache when that one strikes first. Empty while the cache is still warming, and for
  // every other kind. A cache without a hit ratio loses nothing to a flush: 0 from the start.
  std::optional<double> flush_window_s{};
  FaultAfter after{};
  // The start of the first step at or after at + for with every client op's p99 and every node's
  // rho within 1 % of the twin's same step; empty when the run ended first.
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
FaultSchedule::FaultSchedule(const graph::Graph& g) {
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
        add_flush(g, faults[i]);
        break;
      case model::FaultKind::node_down:
      case model::FaultKind::capacity:
      case model::FaultKind::latency_spike:
      case model::FaultKind::traffic_spike:
        add_node_fault(g, faults[i]);
        break;
    }
  }
  end_at_next_flushes();
  add_failover_routes(g);
}

void FaultSchedule::add_node_fault(const graph::Graph& g, const model::Fault& fault) {
  const std::size_t node = node_named(g, fault);
  const Condition effect = effect_of(fault, g.nodes()[node].spec);
  if (changes_nothing(effect)) return;
  // C++20 designated initializers: `Scheduled{.node = ..., .at = ...}` names each member it sets,
  // in declaration order. Replaces positional braces, where the two SimTimes (at, for) could be
  // swapped without a compiler error.
  faults_.push_back(Scheduled{.node = node,
                              .at = fault.at,
                              .duration = fault.duration,
                              .effect = effect,
                              .kills_replicas = fault.kind == model::FaultKind::node_down,
                              .kills_servers = fault.kind == model::FaultKind::node_down});
}

// Every (caller, discovery, target) whose target a node_down of the schedule takes replicas from
// (dead_share_of > 0): each step of each op, with the discovery whose lookup comes last before it.
void FaultSchedule::add_failover_routes(const graph::Graph& g) {
  const auto nodes = g.nodes();
  for (std::size_t caller = 0; caller < nodes.size(); ++caller) {
    // C++20 std::views::keys: a lazy view of each map entry's key (an op name), replacing a loop
    // over the pairs that ignores `.second`; nothing is copied. No lambda goes into the adaptor
    // (CLAUDE.md §3's fifth GCC 14.2 defect).
    for (const auto& op : nodes[caller].spec.ops | std::views::keys) {
      const auto hops = g.service_hops(caller, op);
      for (std::size_t position = 0; position < hops.size(); ++position) {
        const auto discovery = g.discovery_before_step(caller, op, position);
        if (!discovery) continue;
        add_route(FailoverRoute{
            .caller = caller,
            .discovery = *discovery,
            .target = hops[position].target,
            .failover = nodes[*discovery].spec.failover.value_or(defaults::failover)});
      }
    }
  }
}

// A route once, and only when a fault on its target kills replicas: the engine hands the caller
// nothing for any other, every step.
void FaultSchedule::add_route(FailoverRoute route) {
  const auto same = [&](const FailoverRoute& r) {
    return r.caller == route.caller && r.discovery == route.discovery && r.target == route.target;
  };
  const auto kills = [&](const Scheduled& f) {
    return f.node == route.target && f.kills_replicas && f.effect.capacity_factor < 1.0;
  };
  // C++20 std::ranges::any_of: the range itself rather than a begin/end pair (std::any_of), so
  // the pair cannot be mismatched; a lambda handed to an ALGORITHM is safe under GCC 14.2.
  if (std::ranges::any_of(routes_, same) || !std::ranges::any_of(faults_, kills)) return;
  routes_.push_back(route);
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
                                .effect = Condition{.capacity_factor = 0.0},
                                .kills_servers = true});
  }
  const auto links = g.links();
  for (std::size_t link = 0; link < links.size(); ++link) {
    if (inside(links[link].from) == inside(links[link].to)) continue;
    cuts_.push_back(Cut{.link = link, .at = fault.at, .duration = fault.duration});
  }
}

// A flush of a cache with no hit ratio (a CDN that misses every read) changes nothing: h × 0 = h.
void FaultSchedule::add_flush(const graph::Graph& g, const model::Fault& fault) {
  const std::size_t cache = node_named(g, fault);
  if (!g.nodes()[cache].hit_ratio) return;
  warm_ups_.push_back(warm_up_of(g, cache, fault.at));
  // C++23 std::ranges::contains: "is this value in the range?" in one call, replacing
  // `std::find(begin, end, value) != end`. may_cut uses its projection form (`&Cut::link`: compare
  // each element's member, not the element).
  if (!std::ranges::contains(flushed_, cache)) flushed_.push_back(cache);
}

// Each flush's warm-up ends where the next flush of the same cache (the earliest later `at`)
// empties it again (WarmUp::until).
void FaultSchedule::end_at_next_flushes() {
  for (auto& warm_up : warm_ups_) {
    for (const auto& later : warm_ups_) {
      if (later.cache != warm_up.cache || later.at.count() <= warm_up.at.count()) continue;
      if (!warm_up.until || later.at.count() < warm_up.until->count()) warm_up.until = later.at;
    }
  }
}

// The warm-up's numbers (WarmUp's comment has the model), resolved once; it has seen no reads yet.
FaultSchedule::WarmUp FaultSchedule::warm_up_of(const graph::Graph& g, std::size_t cache,
                                                SimTime at) {
  const auto& node = g.nodes()[cache];
  const auto& resolved = *node.hit_ratio;
  const double copies = static_cast<double>(model::copies_of(node.spec));
  if (!resolved.keys) {  // a fixed ratio: linear in n / keys
    if (!node.spec.keys) {
      throw std::invalid_argument{"FaultSchedule: cache_flush on '" + node.spec.id +
                                  "', a fixed ratio without keys (Simulation::create refuses it)"};
    }
    return WarmUp{.cache = cache,
                  .at = at,
                  .share_per_copy = 1.0 / copies,
                  .full_at = static_cast<double>(*node.spec.keys)};
  }
  // che: a ring's partitions hold different keys, one cache of `capacity` seeing every read; any
  // other replicas are copies of one cache each seeing its share.
  auto popularity = popularity_seen_by(g.nodes(), resolved);
  const double full_at = caching::che_characteristic_time(resolved.capacity, popularity);
  const double steady = caching::che_hit_ratio(resolved.capacity, popularity);
  return WarmUp{.cache = cache,
                .at = at,
                .share_per_copy = static_cast<double>(resolved.partitions) / copies,
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

// The failover window of each node_down is min(failover, for) from its start.
double FaultSchedule::dead_share(std::size_t node, SimTime now, SimTime failover) const {
  return killed_share(node, now, failover);
}

// The whole of each node_down and dc_down, [at, at + for); each one's share is what its capacity
// factor took, 1 − (R − r) / R (0.25 for 1 of 4, 1 for every replica and for a dc_down).
std::vector<DownFault> FaultSchedule::down_faults(std::size_t node, SimTime now) const {
  std::vector<DownFault> down;
  for (std::size_t i = 0; i < faults_.size(); ++i) {
    const auto& fault = faults_[i];
    if (fault.node != node || !fault.kills_servers || !active(fault.at, fault.duration, now)) {
      continue;
    }
    down.push_back(DownFault{.fault = i, .share = 1.0 - fault.effect.capacity_factor});
  }
  return down;
}

// The surviving share Π (R − r_i) / R of the node_downs on `node` active at `now`, each over its
// first `window`, is each one's capacity factor, so the share killed is exactly what those faults
// took (0.25 for 1 of 4).
double FaultSchedule::killed_share(std::size_t node, SimTime now, SimTime window) const {
  double survivors = 1.0;
  for (const auto& fault : faults_) {
    if (fault.node != node || !fault.kills_replicas) continue;
    const SimTime span{std::min(window.count(), fault.duration.count())};
    if (active(fault.at, span, now)) survivors *= fault.effect.capacity_factor;
  }
  return 1.0 - survivors;
}

double FaultSchedule::dead_share_of(const model::Fault& fault, const model::NodeSpec& node) {
  if (fault.kind != model::FaultKind::node_down) return 0.0;
  return 1.0 - down_effect(fault, node).capacity_factor;
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
  return latest == nullptr ? 1.0 : warmth(*latest);
}

// Every warm-up of the cache still filling counts its part of the step (counted_s).
void FaultSchedule::count_reads(std::size_t cache, SimTime now, SimTime dt, double reads_per_s) {
  for (auto& warm_up : warm_ups_) {
    if (warm_up.cache != cache || warm_up.warm_in_s) continue;
    const double counted = counted_s(warm_up, now, dt);
    if (counted > 0.0) add_reads(warm_up, now, reads_per_s * warm_up.share_per_copy, counted);
  }
}

// The seconds of the step [now, now + dt) inside the warm-up's span [at, until): all of a step
// after the flush, the part after `at` of the step the flush falls in, none before it or once the
// next flush has emptied the cache again. Tick arithmetic, exact.
double FaultSchedule::counted_s(const WarmUp& warm_up, SimTime now, SimTime dt) {
  const auto start = std::max(now.count(), warm_up.at.count());
  auto end = now.count() + dt.count();
  if (warm_up.until) end = std::min(end, warm_up.until->count());
  return end > start ? seconds(SimTime{end - start}) : 0.0;
}

// n += the copy's reads over the counted seconds. The step in which n reaches full_at holds the
// moment the cache is full: at the share of those seconds its last reads took, counted from the
// start of the counted part, so a constant rate gives full_at / rate exactly, whatever the step.
void FaultSchedule::add_reads(WarmUp& warm_up, SimTime now, double reads_per_s, double counted_s) {
  const double reads = reads_per_s * counted_s;
  if (reads > 0.0 && warm_up.seen + reads >= warm_up.full_at) {
    const double from_at_s =
        seconds(SimTime{std::max(now.count(), warm_up.at.count()) - warm_up.at.count()});
    warm_up.warm_in_s = from_at_s + (warm_up.full_at - warm_up.seen) / reads_per_s;
  }
  warm_up.seen += reads;
}

std::optional<double> FaultSchedule::warm_up_s(std::size_t cache, SimTime at) const {
  for (const auto& warm_up : warm_ups_) {
    if (warm_up.cache == cache && warm_up.at.count() == at.count()) return warm_up.warm_in_s;
  }
  return std::nullopt;
}

// h(n) / h at the n requests per copy counted since the flush (WarmUp's comment): exactly 1 once n
// reaches full_at, so a warm cache is bit for bit the cache before the flush.
double FaultSchedule::warmth(const WarmUp& warm_up) {
  const double requests = warm_up.seen;
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
