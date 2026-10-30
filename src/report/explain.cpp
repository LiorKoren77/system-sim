// Implementation unit of `simsys.report`: `simsys explain` (render_explain). It prints every number
// the engine will use with where it came from (the scenario, the type's default, the datacenter's
// default), the links, the step order, and the back-of-the-envelope check of System Design
// Interview ch. 2: the peak offered load of every node and bandwidth link against its capacity, on
// paper, without queueing. A separate concern from the run's summaries and renderers, which stay in
// report.cpp (phase 3a split it out of that file's 3,249 lines).
//
// The helpers both units need (number formatting, the run's clock, zone names, the aligned text
// table) are shared through the module implementation partition simsys.report:shared
// (shared.cppm), not a header: see the top of that file for why.
module;
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// C++20 module implementation unit: `module simsys.report;` (no `export`) makes this file
// part of the named module. It sees everything the interface declared, exports nothing,
// and can include heavy headers in its own global module fragment, like a .cpp behind a
// header. A module may have any number of them; this is the second, beside report.cpp.
module simsys.report;

// C++20 `import :shared;`: imports a partition of this same module by its partition name alone
// (the `simsys.report` part is implied, and only units of simsys.report may import it). It brings
// in the helpers report.cpp uses too, with module linkage: shared, yet invisible outside.
import :shared;

// C++20 `import`: the compiled interfaces of the modules this unit reads from. The
// interface's own imports would be visible anyway; listing them says what is used here.
import simsys.behaviours; // ClientBehaviour::backoff_steps: the wait a retry really waits
import simsys.core;
import simsys.defaults;
import simsys.engine;
import simsys.fluid;
import simsys.graph;
import simsys.model;
import simsys.workload;

namespace simsys::report {

namespace {

// ---- explain ----

[[nodiscard]] std::string_view provenance(defaults::Source source) {
  // C++20 `using enum`: brings the enumerators into this function's scope, so the cases
  // read `scenario` instead of `defaults::Source::scenario`; nothing leaks outside the function.
  using enum defaults::Source;
  switch (source) {
    case scenario:
      return "(scenario)";
    case type_default:
      return "(default)";
    case dc_default:
      return "(datacenter default)";
  }
  // C++23 std::unreachable(): every enumerator returned above (-Wswitch, an error under
  // -Werror, proves it), so this point is unreachable. Replaces a dummy return value
  // that would hide a missing case, and silences -Wreturn-type.
  std::unreachable();
}

// The tag of a node's own number (its capacity, its service time): provenance's, except that an
// id generator's defaults are its algorithm's (defaults::defaults_of), not its type's:
// "(algorithm)", where "(default)" would read as the same number for every id generator.
[[nodiscard]] std::string_view node_provenance(defaults::Source source,
                                               const model::NodeSpec& spec) {
  const bool from_algorithm = source == defaults::Source::type_default &&
                              spec.type == model::NodeType::id_generator && spec.id_algorithm;
  return from_algorithm ? "(algorithm)" : provenance(source);
}

[[nodiscard]] std::string explain_header(const model::Scenario& s) {
  return "scenario " + s.name + "  version " + std::to_string(s.version) + "  duration " +
         format_duration(s.duration) + "  step " + format_duration(s.step) + "  warmup " +
         format_duration(s.warmup) + "  seed " + std::to_string(s.seed) + "\n" +
         "datacenters: " + join(s.datacenters, ", ") + "\n";
}

// ---- the scripted faults in one line ----

// What a fault acts on: its node, or for a dc_down its datacenter.
[[nodiscard]] const std::string& fault_target(const model::Fault& fault) {
  return fault.node.empty() ? fault.datacenter : fault.node;
}

// The key that sizes a fault, when its kind has one: " replicas=1", " primary" (a sql_db's
// primary, the scenario's `replicas: primary`), " factor=0.5", " extra=50ms"; "" for a whole
// node_down, a cache_flush or a dc_down.
[[nodiscard]] std::string fault_size(const model::Fault& fault) {
  if (fault.primary) return " primary";
  if (fault.replicas) return " replicas=" + std::to_string(*fault.replicas);
  if (fault.factor) return " factor=" + significant(*fault.factor);
  if (fault.extra) return " extra=" + format_duration(*fault.extra);
  return "";
}

// "node_down chat_servers 21:30 for 1m": kind, target, start on the run's clock and length (a
// cache_flush is an instant and has none: model::lasts), then its size.
[[nodiscard]] std::string fault_summary(const model::Fault& fault) {
  return std::string{model::to_string(fault.kind)} + " " + fault_target(fault) + " " +
         clock_or_duration(fault.at) +
         (model::lasts(fault.kind) ? " for " + format_duration(fault.duration) : "") +
         fault_size(fault);
}

// "faults: 2 scripted (node_down chat_servers 21:30 for 1m, cache_flush feed_cache 09:00)", so a
// reader sees what the run will break before running it. No line at all for a scenario without
// faults, so the explain of every such scenario is unchanged.
[[nodiscard]] std::string explain_faults(const model::Scenario& s) {
  if (s.faults.empty()) return "";
  std::vector<std::string> summaries;
  for (const auto& fault : s.faults) summaries.push_back(fault_summary(fault));
  return "faults: " + std::to_string(s.faults.size()) + " scripted (" + join(summaries, ", ") +
         ")\n";
}

// ---- the rate curve in one line ----

[[nodiscard]] std::string level_summary(const workload::RateCurve& curve) {
  if (!curve.points) return "base " + fmt_rate(curve.base.per_second);
  const auto& at = curve.points->at;
  // C++20 std::views::values: a view of each pair's second member (here the rate of each
  // (time, rate) point), without copying them out. C++20 std::ranges::minmax with a
  // projection: the lowest and highest rate of the trace in one pass, comparing by the rate
  // member instead of through a hand-written lambda.
  const auto [low, high] = std::ranges::minmax(at | std::views::values, {}, &Rate::per_second);
  return "points " + std::to_string(at.size()) + " (" + fmt_rate(low.per_second) + ".." +
         fmt_rate(high.per_second) + ")";
}

[[nodiscard]] std::string wave_summary(const workload::Wave& wave) {
  return "wave " + std::string{workload::to_string(wave.shape)} +
         " P=" + format_duration(wave.period) + " A=" + significant(wave.amplitude) + " peak " +
         clock_or_duration(wave.peak_at);
}

[[nodiscard]] std::string ramp_summary(const workload::Ramp& ramp) {
  return "ramp " + fmt_rate(ramp.from.per_second) + "->" + fmt_rate(ramp.to.per_second) + " over " +
         format_duration(ramp.over);
}

[[nodiscard]] std::string noise_summary(const workload::Noise& noise) {
  return "noise σ=" + significant(noise.sigma) + " τ=" + format_duration(noise.correlation);
}

// "base 800/s, wave sine P=24h A=0.6 peak 20:00, ramp 0/s->10k/s over 5m, spikes 3,
// noise σ=0.15 τ=5m": the level, then each term the client has, in composition order.
[[nodiscard]] std::string curve_summary(const model::Workload& workload) {
  const auto& curve = workload.curve;
  std::vector<std::string> terms{level_summary(curve)};
  if (curve.wave) terms.push_back(wave_summary(*curve.wave));
  if (curve.ramp) terms.push_back(ramp_summary(*curve.ramp));
  if (!curve.spikes.empty()) terms.push_back("spikes " + std::to_string(curve.spikes.size()));
  if (workload.noise) terms.push_back(noise_summary(*workload.noise));
  return join(terms, ", ");
}

// " (2 steps)" after a retry_after that is not a whole number of steps: the engine rounds the wait
// UP to whole steps (behaviours::ClientBehaviour::backoff_steps), so at 100 ms steps 150 ms waits
// 200 ms and 50 ms waits one step, 100 ms. Empty when the wait is whole: it is what is written.
[[nodiscard]] std::string effective_backoff(SimTime retry_after, SimTime step) {
  const int steps = behaviours::ClientBehaviour::backoff_steps(retry_after, step);
  if (step.count() * steps == retry_after.count()) return {};  // in nanoseconds: exact
  return " (" + std::to_string(steps) + (steps == 1 ? " step)" : " steps)");
}

// "retries=2 after 100ms": how often a client sends a failed request again and how long it waits
// first (the loader filled in defaults::retry_after when the scenario gave no wait), with the
// steps it really waits when they differ, "after 150ms (2 steps)". Empty without retries, so a
// client without them keeps the line it had.
[[nodiscard]] std::string retries_summary(const model::Workload& workload, SimTime step) {
  if (workload.retries == 0) return {};
  const SimTime retry_after = workload.retry_after.value_or(defaults::retry_after);
  return "retries=" + std::to_string(workload.retries) + " after " + format_duration(retry_after) +
         effective_backoff(retry_after, step);
}

// A client's datacenter, its rate curve, its op mix (fixed shares of the rate) and its retries
// (`step`: the scenario's, which the retries wait in).
[[nodiscard]] std::string client_details(const model::NodeSpec& spec, SimTime step) {
  std::vector<std::string> parts{"dc=" + spec.dc};
  if (!spec.workload) return join(parts, "  ");
  std::vector<std::string> shares;
  for (const auto& [op, share] : spec.workload->op_shares)
    shares.push_back(op + " " + percent(share));
  parts.push_back("rate: " + curve_summary(*spec.workload));
  parts.push_back("ops: " + join(shares, ", "));
  if (auto retries = retries_summary(*spec.workload, step); !retries.empty()) {
    parts.push_back(std::move(retries));
  }
  return join(parts, "  ");
}

// Where a database's default write cost comes from (defaults::sql_write_cost): "spec §6 5k
// reads/1k writes", the two rates of one SQL machine whose ratio is the cost.
[[nodiscard]] std::string sql_write_cost_source() {
  const double read_rate = defaults::for_type(model::NodeType::sql_db).service_rate.per_second;
  return "spec §6 " + with_si_prefix(read_rate) + " reads/" +
         with_si_prefix(defaults::sql_write_rate.per_second) + " writes";
}

// A database's write ops that reach it with no stated cost: they cost 5 units each, not 1
// (defaults::op_cost), "insert 5", "update 5", sorted by op (arriving_ops is sorted).
[[nodiscard]] std::vector<std::string> defaulted_write_costs(const graph::Graph& g,
                                                             std::size_t node) {
  const auto& spec = g.nodes()[node].spec;
  std::vector<std::string> parts;
  if (spec.type != model::NodeType::sql_db) return parts;
  for (const auto& op : g.arriving_ops(node)) {
    // C++20 std::map::contains: "is this key in the map?" in one call, replacing
    // `cost.find(op) != cost.end()` or `cost.count(op) != 0`; here: did the scenario state
    // this op's cost?
    if (!graph::is_write(spec, op) || spec.cost.contains(op)) continue;
    parts.push_back(op + " " + significant(defaults::op_cost(spec, op), 6));
  }
  return parts;
}

// "cost: read 1, write 3" (the stated costs, ops sorted by name), or nothing when every op costs
// the default one unit. A database's write op that reaches it with no stated cost costs 5 units,
// not 1: a fivefold load the scenario does not show, so it is printed too, after the stated ones
// and with its source once, "cost: read 2; insert 5, update 5 (defaults: spec §6 5k reads/1k
// writes)" (the semicolon ends the stated group, so the source covers only the defaulted one).
[[nodiscard]] std::string cost_summary(const graph::Graph& g, std::size_t node) {
  std::vector<std::string> stated;
  for (const auto& [op, units] : g.nodes()[node].spec.cost)
    stated.push_back(op + " " + significant(units, 6));
  const auto defaulted = defaulted_write_costs(g, node);
  std::vector<std::string> groups;
  if (!stated.empty()) groups.push_back(join(stated, ", "));
  if (!defaulted.empty()) {
    const std::string label = defaulted.size() > 1 ? "defaults: " : "default: ";
    groups.push_back(join(defaulted, ", ") + " (" + label + sql_write_cost_source() + ")");
  }
  if (groups.empty()) return {};
  return "  cost: " + join(groups, "; ");
}

// The vnodes of the consistent-hash balancer in front of `target` (the one whose ring
// gives the largest imbalance, as Graph::mark_balanced_targets keeps), for explain. A
// drained route (weight 0) sends nothing, so its ring does not count (as there). A kv_cluster's
// ring is its own (graph::apply_kv_ring): its `vnodes`, or the default.
[[nodiscard]] int ring_vnodes(const graph::Graph& g, std::size_t target) {
  const auto& target_spec = g.nodes()[target].spec;
  if (target_spec.type == model::NodeType::kv_cluster) {
    return target_spec.vnodes.value_or(model::default_vnodes);
  }
  int fewest = 0;  // fewest vnodes = largest imbalance 1 + z_M/sqrt(v), M the machines
  for (std::size_t node = 0; node < g.nodes().size(); ++node) {
    const auto& spec = g.nodes()[node].spec;
    if (spec.policy != model::LbPolicy::consistent_hash) continue;
    const int vnodes = spec.vnodes.value_or(model::default_vnodes);
    for (const auto& route : g.routes(node)) {
      if (route.share <= 0.0 || route.target != target) continue;
      if (fewest == 0 || vnodes < fewest) fewest = vnodes;
    }
  }
  return fewest;
}

// " (default)" after a number the scenario left out, nothing after one it stated (even
// when it states the default value: provenance is about presence, as in server_details).
template <class T>
[[nodiscard]] std::string if_defaulted(const std::optional<T>& stated) {
  return stated ? std::string{} : " (default)";
}

// What a load balancer's policy means for this node (spec §17 entry 5): a balancer's own policy,
// and on the node behind it `pooled` (least_connections) or the hottest replica's `imbalance`
// (consistent_hash).
[[nodiscard]] std::string balancing_summary(const graph::Graph& g, std::size_t index) {
  const auto& node = g.nodes()[index];
  std::string text;
  if (node.spec.type == model::NodeType::load_balancer) {
    const auto policy = node.spec.policy.value_or(model::LbPolicy::round_robin);
    text += "  policy=" + std::string{model::to_string(policy)} + if_defaulted(node.spec.policy);
    if (policy == model::LbPolicy::consistent_hash) {
      text += " vnodes=" + std::to_string(node.spec.vnodes.value_or(model::default_vnodes)) +
              if_defaulted(node.spec.vnodes);
    }
  }
  if (node.pooled) text += "  pooled";
  if (node.imbalance != 1.0) {
    text += "  imbalance=" + significant(node.imbalance, 3) + " (vnodes " +
            std::to_string(ring_vnodes(g, index)) + ")";
  }
  return text;
}

// A rate limiter's algorithm and its numbers with the defaults applied (spec §17 entry 6):
// `algorithm=token_bucket limit=1k/s burst=500 backend=counters`. A bucket shows its burst, a
// windowed algorithm its window (the one number that sizes each).
[[nodiscard]] std::string limiter_summary(const model::NodeSpec& spec) {
  if (spec.type != model::NodeType::rate_limiter || !spec.limit) return {};
  const auto algorithm = spec.algorithm.value_or(model::LimiterAlgorithm::token_bucket);
  std::string text = "  algorithm=" + std::string{model::to_string(algorithm)} +
                     " limit=" + fmt_rate(spec.limit->per_second);
  text +=
      model::is_windowed(algorithm)
          ? " window=" + format_duration(model::limiter_window(spec)) + if_defaulted(spec.window)
          : " burst=" + significant(model::limiter_burst(spec), 10) + if_defaulted(spec.burst);
  if (spec.backend) text += " backend=" + *spec.backend;
  return text;
}

// What an id generator's algorithm stands for (spec §18 entry 12; book ch. 7), with the numbers the
// algorithm gives one replica (defaults::for_id_algorithm), so a student sees WHY the node has the
// capacity it has: `algorithm=snowflake (4.096M/s per replica by design)`, `algorithm=ticket_server
// (one database: 1k/s, spec §6 writes)`, `algorithm=uuid (unbounded: 1G/s)`. The algorithm's number
// even when the scenario states its own `capacity`, which the node line then shows tagged
// `(scenario)`: the reader sees the design's figure and the override, and the figure is then called
// the default it is, so that it does not read as a second capacity: `algorithm=ticket_server (one
// database; default 1k/s, spec §6 writes)`.
[[nodiscard]] std::string id_algorithm_meaning(model::IdAlgorithm algorithm, bool capacity_stated) {
  const std::string rate = fmt_rate(defaults::for_id_algorithm(algorithm).service_rate.per_second);
  const std::string figure = capacity_stated ? "default " + rate : rate;
  const std::string_view before_figure = capacity_stated ? "; " : ": ";
  using enum model::IdAlgorithm;  // C++20, as in provenance
  switch (algorithm) {
    case snowflake:
      return figure + " per replica by design";
    case ticket_server:
      return "one database" + std::string{before_figure} + figure + ", spec §6 writes";
    case uuid:
      return "unbounded" + std::string{before_figure} + figure;
  }
  std::unreachable();
}

[[nodiscard]] std::string id_generator_summary(const model::NodeSpec& spec) {
  if (spec.type != model::NodeType::id_generator || !spec.id_algorithm) return {};
  return "  algorithm=" + std::string{model::to_string(*spec.id_algorithm)} + " (" +
         id_algorithm_meaning(*spec.id_algorithm, spec.capacity.has_value()) + ")";
}

// "write_ops=write", plus a note when no op that reaches the cache (or blob store) is one of
// them: a cache's write strategy then never acts, a blob store answers everything at its first
// megabyte. A note, not an error: a read-only cache or store is legitimate, but a write op named
// differently in the steps (`set`, `put`, `upload`) is a common slip.
[[nodiscard]] std::string write_ops_summary(const graph::Graph& g, std::size_t cache) {
  const auto write_ops = model::write_ops_of(g.nodes()[cache].spec);
  // C++20 std::ranges::none_of takes the range itself instead of a begin/end pair; C++23
  // std::ranges::contains asks "is this value in the range?" in one call, replacing
  // `std::find(begin, end, value) != end`.
  const bool none_arrive = std::ranges::none_of(g.arriving_ops(cache), [&](const std::string& op) {
    return std::ranges::contains(write_ops, op);
  });
  return " write_ops=" + join(write_ops, ",") + (none_arrive ? " (never arrives)" : "");
}

// A client key distribution as a reader says it: "zipf 1 over 100M keys", "uniform over 10k
// keys" (the scenario's `{zipf: 1.0, space: 100M}` and `{uniform: 10k}`).
[[nodiscard]] std::string key_distribution(const model::KeySpec& keys) {
  const std::string shape = keys.zipf ? "zipf " + significant(*keys.zipf) : "uniform";
  return shape + " over " + fmt_count(keys.space) + " keys";
}

// "u", "a, b": the clients a che hit ratio took its key distribution from.
// C++20 std::span<const T>: a non-owning read-only view of contiguous elements (pointer + size).
// Replaces a `const std::vector<T>&` (which forces the caller to own a vector) or a pointer and a
// count; here the graph's index list is passed without a copy, whatever container holds it.
[[nodiscard]] std::string client_ids(const graph::Graph& g, std::span<const std::size_t> clients) {
  std::vector<std::string> ids;
  for (const auto client : clients) ids.push_back(g.nodes()[client].spec.id);
  return join(ids, ", ");
}

// The hit ratio and where it comes from: `0.8 (fixed)`, stated by the scenario; `0.732 (che:
// zipf 1 over 100M keys from users)`, computed by the graph with the Che approximation from the
// cache's `keys` and the key distribution of the clients that reach it (named, so a reader can
// find the numbers it rests on; che_provenance); `che` alone for one the graph left without a
// number; `none`.
[[nodiscard]] std::string hit_ratio_summary(const graph::ResolvedNode& node) {
  const auto& stated = node.spec.hit_ratio;
  if (!stated) return "none";
  if (!node.hit_ratio) return stated->che ? "che" : "none";
  return significant(node.hit_ratio->value, stated->che ? 3 : 6);
}

// "; 10k keys x 4 partitioned, uneven arcs" for a che cache behind a consistent-hash ring: its
// replicas split the key space, so together they hold `keys × replicas` keys, each replica over
// its own arc of the ring, and the arcs are uneven (the ring's imbalance; graph's
// detail::ring_arcs). Nothing for any other cache.
[[nodiscard]] std::string partition_summary(const graph::ResolvedNode& node) {
  const int partitions = node.hit_ratio->partitions;
  if (partitions <= 1) return {};
  return "; " + fmt_count(static_cast<double>(*node.spec.keys)) + " keys x " +
         std::to_string(partitions) + " partitioned" +
         (node.imbalance > 1.0 ? ", uneven arcs" : "");
}

// "; fed through fixed-ratio cache 'browser_cache': treated as unfiltered" when the keys passed
// a cache that says how many requests miss but not which (graph's ResolvedHitRatio::
// unfiltered_through): the Che popularity is then the one in front of it, an approximation
// worth flagging. Nothing otherwise.
[[nodiscard]] std::string unfiltered_caveat(const graph::Graph& g,
                                            const graph::ResolvedHitRatio& resolved) {
  if (!resolved.unfiltered_through) return {};
  const auto& through = g.nodes()[*resolved.unfiltered_through];
  const std::string kind = through.hit_ratio ? "fixed-ratio " : "";
  const std::string unknown = through.hit_ratio ? "" : " with no hit ratio";
  return "; fed through " + kind + std::string{model::to_string(through.spec.type)} + " '" +
         through.spec.id + "'" + unknown + ": treated as unfiltered";
}

// What a che hit ratio rests on: ` (che: zipf 0.9 over 10M keys from u, filtered by l1; 2M keys
// x 4 partitioned, uneven arcs)`. "filtered by l1": the keys are cache l1's misses, whose
// popularity is the clients' with l1's hits taken out (graph's ResolvedHitRatio::filtered_by).
[[nodiscard]] std::string che_provenance(const graph::Graph& g, const graph::ResolvedNode& node) {
  const auto& resolved = *node.hit_ratio;
  std::string text =
      " (che: " + key_distribution(*resolved.keys) + " from " + client_ids(g, resolved.key_clients);
  if (resolved.filtered_by) text += ", filtered by " + g.nodes()[*resolved.filtered_by].spec.id;
  return text + partition_summary(node) + unfiltered_caveat(g, resolved) + ")";
}

// A cache's model (spec §17 entry 7): `hit_ratio=0.8 (fixed) strategy=cache_aside write_ops=write
// backend=db keys=20M eviction=lru ttl=5m`, the keys the scenario left out omitted (strategy and
// write_ops always have a value: their defaults). A che hit ratio shows its provenance instead of
// `(fixed)` (hit_ratio_summary). `hit_ratio=0.8 (fixed)`, `hit_ratio=0.732 (che: ...)`,
// `hit_ratio=che` (no number) or `hit_ratio=none`: what a cache and a CDN both open with.
[[nodiscard]] std::string hit_ratio_text(const graph::Graph& g, const graph::ResolvedNode& node) {
  std::string text = "hit_ratio=" + hit_ratio_summary(node);
  if (node.hit_ratio && node.hit_ratio->keys) {
    text += che_provenance(g, node);
  } else if (node.hit_ratio) {
    text += " (fixed)";
  }
  return text;
}

[[nodiscard]] std::string cache_summary(const graph::Graph& g, std::size_t cache) {
  const auto& node = g.nodes()[cache];
  const auto& spec = node.spec;
  if (spec.type != model::NodeType::cache) return {};
  std::string text = "  " + hit_ratio_text(g, node);
  text += " strategy=" + std::string{model::to_string(spec.strategy)} + write_ops_summary(g, cache);
  if (spec.backend) text += " backend=" + *spec.backend;
  if (spec.keys) text += " keys=" + fmt_count(static_cast<double>(*spec.keys));
  if (spec.eviction) text += " eviction=" + *spec.eviction;
  if (spec.ttl) text += " ttl=" + format_duration(*spec.ttl);
  return text;
}

// "serves: read, upload": the ops a CDN answers from its cache, every op that reaches it that is
// not one of its write ops (model::write_ops_of; a write op that reaches a CDN is refused at
// build). A scenario that sends an `upload` through a CDN without declaring it a write sees it
// here, cached like a read. "serves: none" when nothing reaches the CDN.
[[nodiscard]] std::string cdn_served_ops(const graph::Graph& g, std::size_t cdn) {
  const auto& spec = g.nodes()[cdn].spec;
  std::vector<std::string> served;
  for (const auto& op : g.arriving_ops(cdn)) {
    if (!graph::is_write(spec, op)) served.push_back(op);
  }
  return "  serves: " + (served.empty() ? std::string{"none"} : join(served, ", "));
}

// A CDN's model (spec §18 entry 10), shown as a cache's is: `hit_ratio=0.95 (fixed) misses →
// media_store keys=100k ttl=1h  serves: read_media`. "misses → media_store": the CDN fetches what
// it misses from its origin, so the reader sees where the 1 − h of the reads go (the check lines
// below count them there). A CDN without a hit ratio says what that means, `hit_ratio=none (every
// read misses: h = 0)`: the scenario's own statement, which runs as such. The keys the scenario
// left out are omitted.
[[nodiscard]] std::string cdn_summary(const graph::Graph& g, std::size_t cdn) {
  const auto& node = g.nodes()[cdn];
  const auto& spec = node.spec;
  if (spec.type != model::NodeType::cdn) return {};
  std::string text = "  " + hit_ratio_text(g, node);
  if (!spec.hit_ratio) text += " (every read misses: h = 0)";
  if (spec.origin) text += " misses → " + *spec.origin;
  if (spec.keys) text += " keys=" + fmt_count(static_cast<double>(*spec.keys));
  if (spec.ttl) text += " ttl=" + format_duration(*spec.ttl);
  return text + cdn_served_ops(g, cdn);
}

// "primary+1 replica", "primary+2 replicas", or "primary only" (replicas: 1).
[[nodiscard]] std::string replica_set(int copies_per_shard) {
  const int replicas = copies_per_shard - 1;
  if (replicas < 1) return "primary only";
  return "primary+" + std::to_string(replicas) + (replicas == 1 ? " replica" : " replicas");
}

// "  lag=50ms", "  lag=0s (default)", or "  lag=50ms (unused: no replicas)": with `replicas: 1`
// nothing replicates, so a stated lag describes nothing (it is accepted, so a scenario can keep
// it while trying one copy).
[[nodiscard]] std::string lag_summary(const model::NodeSpec& spec) {
  const std::string lag = "  lag=" + format_duration(spec.replication_lag.value_or(SimTime{0}));
  if (!spec.replication_lag) return lag + " (default)";
  return spec.replicas < 2 ? lag + " (unused: no replicas)" : lag;
}

// A SQL database's model (spec §18 entry 6): `primary+2 replicas  shards=4  lag=50ms pool=32
// write_ops=insert,update`, the defaults applied and marked: one shard, no lag; `pool` only when
// stated (the queue numbers say whether it binds: `capacity=400/s (pool 4 / 10ms) ... c=4 (pool)`),
// and the write ops always (the database default delete,insert,update,write, or the scenario's).
[[nodiscard]] std::string database_summary(const graph::ResolvedNode& node) {
  const auto& spec = node.spec;
  if (spec.type != model::NodeType::sql_db) return {};
  std::string text = "  " + replica_set(spec.replicas) +
                     "  shards=" + std::to_string(spec.shards.value_or(1)) +
                     if_defaulted(spec.shards) + lag_summary(spec);
  if (spec.pool) text += "  pool=" + std::to_string(*spec.pool);
  return text + "  write_ops=" + join(model::write_ops_of(spec), ",");
}

// Where each of a queue's four numbers came from, as explain prints it.
struct QueueProvenance {
  std::string capacity;
  std::string_view service_time;
  std::string_view concurrency;
  std::string_view queue_limit;
};

[[nodiscard]] bool is_leaky_bucket(const model::NodeSpec& spec) {
  return spec.type == model::NodeType::rate_limiter &&
         spec.algorithm == model::LimiterAlgorithm::leaky_bucket;
}

// A leaky bucket's queue is derived whole from the limiter (graph::leaky_bucket_capacity):
// it drains at `limit` through one deterministic leak slot and holds `burst`. Any other
// node's numbers are the scenario's, a default, or (c) implied by Little's law.
// A database whose pool binds (graph::pool_binds): Little's law makes its rate pool / service
// time and its c the pool, "capacity=400/s (pool 4 / 10ms) ... c=4 (pool)".
[[nodiscard]] QueueProvenance pool_provenance(const graph::ResolvedNode& node) {
  const auto& spec = node.spec;
  const auto queue_source =
      spec.advanced.queue_limit ? defaults::Source::scenario : defaults::Source::type_default;
  return {"(pool " + std::to_string(*spec.pool) + " / " +
              format_duration(node.capacity.service_time_s) + ")",
          provenance(node.service_time_source), "(pool)", provenance(queue_source)};
}

// A rate in a node's capacity units: "1.5k/s" (requests) for most nodes, "100k MB/s" for a node
// that counts megabytes (defaults::is_byte_sized, spec §19 entry 2), whose capacity, load and
// checks are all in MB.
[[nodiscard]] std::string fmt_units(const model::NodeSpec& spec, double per_second) {
  if (defaults::is_byte_sized(spec)) return with_si_prefix(per_second) + " MB/s";
  return fmt_rate(per_second);
}

// " per MB" after a byte-sized node's service time, the time of one megabyte (its unit); nothing
// after a time per request.
[[nodiscard]] std::string_view per_unit(const model::NodeSpec& spec) {
  return defaults::is_byte_sized(spec) ? " per MB" : "";
}

// " slots (1 object each)" after a byte-sized node's c: c = MB/s × the time of one MB (Little's
// law) counts transfers in flight, and each one is an object holding its slot for its whole
// transfer (behaviours::MegabyteService), whatever its size: 20 slots move 20 objects at once, not
// 20 MB. Nothing after a count of servers.
[[nodiscard]] std::string_view per_slot(const model::NodeSpec& spec) {
  return defaults::is_byte_sized(spec) ? " slots (1 object each)" : "";
}

// " MB" after a byte-sized node's queue_limit: its waiting room holds capacity units, here
// megabytes (the generic default of 1000 units is 1000 MB there, chosen). Nothing after a count
// of units that are requests (or cost-weighted requests).
[[nodiscard]] std::string_view queued_unit(const model::NodeSpec& spec) {
  return defaults::is_byte_sized(spec) ? " MB" : "";
}

[[nodiscard]] QueueProvenance queue_provenance(const graph::ResolvedNode& node) {
  if (is_leaky_bucket(node.spec)) return {"(limit)", "(limit)", "(leak slot)", "(burst)"};
  // A worker pool's four numbers all follow from `workers`, `cost` and its queue's retention
  // (defaults::worker_pool_rate, graph::Graph::connect_consumer).
  if (defaults::worker_pool_rate(node.spec)) {
    return {"(workers / cost)", "(cost)", "(workers)", "(retention × capacity)"};
  }
  if (graph::pool_binds(node)) return pool_provenance(node);
  const auto& spec = node.spec;
  // The graph keeps no provenance for queue_limit, so it is read off the spec: provenance
  // is about presence, not value. A scenario that writes the default (fluid::Capacity's
  // 1000) still wrote it, and explain says so.
  const auto queue_source =
      spec.advanced.queue_limit ? defaults::Source::scenario : defaults::Source::type_default;
  // c is derived by Little's law unless the scenario sets it (defaults::implied_concurrency);
  // a database's `pool` at or above that c binds nothing (graph::apply_database_keys).
  const std::string_view concurrency_source =
      spec.advanced.concurrency ? "(scenario)" : "(implied)";
  return {std::string{node_provenance(node.service_rate_source, spec)},
          node_provenance(node.service_time_source, spec), concurrency_source,
          provenance(queue_source)};
}

// "dc=main  replicas=4", or "dc=main" alone for a kv_cluster, whose copies are its machines,
// printed as `nodes=` with its ring and quorums (kv_cluster_summary): the graph stores them in
// `replicas` (graph::apply_kv_ring), a key the scenario may not state there.
[[nodiscard]] std::string placement(const model::NodeSpec& spec) {
  const std::string dc = "dc=" + spec.dc;
  if (spec.type == model::NodeType::kv_cluster) return dc;
  return dc + "  replicas=" + std::to_string(spec.replicas);
}

[[nodiscard]] std::string server_details(const graph::ResolvedNode& node) {
  const auto& spec = node.spec;
  const auto& cap = node.capacity;
  const auto source = queue_provenance(node);
  const std::array parts{
      placement(spec),
      "capacity=" + fmt_units(spec, cap.service_rate.per_second) + " " + source.capacity,
      "service_time=" + format_duration(cap.service_time_s) + std::string{per_unit(spec)} + " " +
          std::string{source.service_time},
      "c=" + std::to_string(cap.concurrency) + std::string{per_slot(spec)} + " " +
          std::string{source.concurrency},
      "queue_limit=" + fmt_room(cap.queue_limit) + std::string{queued_unit(spec)} + " " +
          std::string{source.queue_limit},
      "variability=" + significant(cap.service_variability, 3),
      "burstiness=" + significant(cap.burstiness, 3)};
  return join(parts, "  ");
}

// Whether a kv_cluster's quorums overlap: W + R > N is what makes every read quorum share a
// replica with every write quorum, so a read meets the latest write (book ch. 6, "quorum
// consensus"). Defaults applied.
[[nodiscard]] bool kv_quorums_overlap(const model::NodeSpec& spec) {
  return model::kv_write_quorum(spec) + model::kv_read_quorum(spec) > model::kv_replication(spec);
}

// "N=3 W=2 R=2", each count marked "(default)" when the scenario left it to Dynamo's (N = 3, W = R
// = 2: model::kv_replication and the quorum helpers), and a note when W + R <= N: the quorums do
// not overlap, so a read may miss the latest write. The model has no versions of a key, so the note
// is all explain can say.
[[nodiscard]] std::string kv_quorums(const model::NodeSpec& spec) {
  std::string text =
      "N=" + std::to_string(model::kv_replication(spec)) + if_defaulted(spec.replication) +
      " W=" + std::to_string(model::kv_write_quorum(spec)) + if_defaulted(spec.write_quorum) +
      " R=" + std::to_string(model::kv_read_quorum(spec)) + if_defaulted(spec.read_quorum);
  if (!kv_quorums_overlap(spec)) text += " (W + R ≤ N: a read may miss the latest write)";
  return text;
}

// "mode=ap (default; partitions: phase 3)": the mode only a partition would show (faults are
// phase 3), marked when defaulted; `cp` (consistency first) with quorums that do not overlap asks
// for what the quorums do not give, and the line says so.
[[nodiscard]] std::string kv_mode_summary(const model::NodeSpec& spec) {
  const auto mode = model::kv_mode_of(spec);
  std::string notes = spec.kv_mode ? "partitions: phase 3" : "default; partitions: phase 3";
  if (mode == model::KvMode::cp && !kv_quorums_overlap(spec)) {
    notes += "; cp needs W + R > N: these quorums contradict it";
  }
  return "mode=" + std::string{model::to_string(mode)} + " (" + notes + ")";
}

// A kv_cluster's shape (spec §19 entry 9; book ch. 6): "  nodes=6 N=3 W=2 R=2 mode=ap (partitions:
// phase 3)", the machines on its ring (`nodes`, which graph::apply_kv_ring also copies into
// `replicas`), the copies of each key and the two quorums (kv_quorums) and the mode
// (kv_mode_summary). Nothing on any other type.
[[nodiscard]] std::string kv_cluster_summary(const model::NodeSpec& spec) {
  if (spec.type != model::NodeType::kv_cluster) return {};
  return "  nodes=" + std::to_string(*spec.kv_nodes) + " " + kv_quorums(spec) + " " +
         kv_mode_summary(spec);
}

// "  sizes: stream 50MB, upload 300MB": on a byte-sized node (defaults::is_byte_sized), the size
// of one request of each op it serves (graph::Graph::request_bytes), ops in name order, in the
// scenario's own size grammar (format_size). It is the number behind the node's capacity: a
// node that counts megabytes serves capacity / size requests of the op a second. Nothing on a
// node that counts requests, where a size would be a number the node never uses.
[[nodiscard]] std::string size_summary(const graph::Graph& g, std::size_t node) {
  if (!defaults::is_byte_sized(g.nodes()[node].spec)) return {};
  std::vector<std::string> sizes;
  for (const auto& op : g.arriving_ops(node)) {
    if (const auto size = g.request_bytes(node, op)) sizes.push_back(op + " " + format_size(*size));
  }
  if (sizes.empty()) return {};
  return "  sizes: " + join(sizes, ", ");
}

// "  write_ops=upload" on a blob store: the ops it answers at their last byte, every other op at
// its first megabyte (behaviours::MegabyteService), with write_ops_summary's note when none of
// them reaches it. Nothing on any other type (a cache shows its own in cache_summary).
[[nodiscard]] std::string blob_store_summary(const graph::Graph& g, std::size_t node) {
  if (g.nodes()[node].spec.type != model::NodeType::blob_store) return {};
  return " " + write_ops_summary(g, node);
}

// "  retries=3 after 1s, then dead-letter": how often a worker pool puts a job whose call to an
// external sink failed back on its queue, how long after the failure, and where the job goes after
// its last retry (phase 3 spec §6.2, behaviours::WorkerPoolBehaviour). The loader filled in
// defaults::pool_retry_after when the scenario gave no wait. Empty for a pool that retries
// nothing, so its line stays what it was.
[[nodiscard]] std::string pool_retries_summary(const model::NodeSpec& spec) {
  if (spec.retries.value_or(0) == 0) return {};
  return "  retries=" + std::to_string(*spec.retries) + " after " +
         format_duration(spec.retry_after.value_or(defaults::pool_retry_after)) +
         ", then dead-letter";
}

// A worker pool's model (spec §19 entry 5): "  consumes=transcode_q  workers=400 cost=1m", the two
// numbers its capacity follows from, and its retries when it has some (pool_retries_summary). The
// capacity itself is stated once, in the queue numbers before it, `capacity=6.667/s (workers /
// cost)` (queue_provenance). Nothing for a pool built in code without both numbers (it runs on the
// type's, which server_details shows).
[[nodiscard]] std::string worker_pool_summary(const graph::ResolvedNode& node) {
  const auto& spec = node.spec;
  if (!defaults::worker_pool_rate(spec)) return {};
  const std::string consumes = spec.consumes ? "  consumes=" + *spec.consumes : "";
  return consumes + "  workers=" + std::to_string(*spec.workers) +
         " cost=" + format_duration(*spec.job_cost) + pool_retries_summary(spec);
}

// A retention as a queue's is quoted: in whole days when it is some ("7d", Kafka's default of
// 168 h), else as explain writes any duration ("10s", "36h"). Only here: every other duration
// keeps format_duration's hours ("ttl=24h"), so this rule changes no other line.
[[nodiscard]] std::string fmt_retention(SimTime retention) {
  constexpr SimTime::rep ns_per_day = 86'400'000'000'000;
  const auto ticks = retention.count();
  if (ticks > 0 && ticks % ns_per_day == 0) return std::to_string(ticks / ns_per_day) + "d";
  return format_duration(retention);
}

// A message queue's model (spec §19 entry 4): "  retention=7d (default: Kafka's 168 h)
// partitions=32 (not modelled)". The retention sets each consumer group's waiting room (retention ×
// the pool's capacity: see the pools' queue_limit); the book gives no figure, so an absent one is
// Kafka's `log.retention.hours`, and the line says whose default it is. Partitions are accepted and
// printed, not simulated (phase 3).
[[nodiscard]] std::string message_queue_summary(const model::NodeSpec& spec) {
  if (spec.type != model::NodeType::message_queue) return {};
  std::string text =
      "  retention=" + fmt_retention(spec.retention.value_or(defaults::default_retention));
  if (!spec.retention) text += " (default: Kafka's 168 h)";
  if (spec.partitions)
    text += " partitions=" + std::to_string(*spec.partitions) + " (not modelled)";
  return text;
}

// A search index's rebuild (spec §19 entry 10, book ch. 13): "  build_interval=24h (rebuild load
// not modelled)", only when the scenario states one. The index serves like a service; how often it
// is rebuilt from the aggregated logs is stored and printed, and the line says the rebuild's load
// (and the freshness of the answers between rebuilds) is not part of the model.
[[nodiscard]] std::string search_index_summary(const model::NodeSpec& spec) {
  if (spec.type != model::NodeType::search_index || !spec.build_interval) return {};
  return "  build_interval=" + format_duration(*spec.build_interval) +
         " (rebuild load not modelled)";
}

// A service discovery's failover (phase 3 spec §8.1, book ch. 12): "  failover=10s", always (the
// loader fills in the chosen 10 s when the scenario states none, so the line shows the window the
// engine uses): how long, after some replicas of a node its callers look up have died, the
// registry still hands their addresses out, and the requests sent to them are lost.
[[nodiscard]] std::string discovery_summary(const model::NodeSpec& spec) {
  if (spec.type != model::NodeType::service_discovery || !spec.failover) return {};
  return "  failover=" + format_duration(*spec.failover);
}

// An external sink's model (spec §19 entry 8): "  failure_rate=2% politeness=1/s (not modelled:
// phase 3)". The failure rate is always printed (0% is the plain queue): the share of what the sink
// serves that fails, which a run counts as drops. Politeness only when the scenario states it, and
// marked as what it is: stored, never simulated (it needs per-host state).
[[nodiscard]] std::string external_sink_summary(const model::NodeSpec& spec) {
  if (spec.type != model::NodeType::external_sink) return {};
  std::string text = "  failure_rate=" + percent(spec.failure_rate);
  if (spec.politeness) {
    text += " politeness=" + fmt_rate(spec.politeness->per_second) + " (not modelled: phase 3)";
  }
  return text;
}

// A ws_gateway's level, its heartbeat load and its cap (defined below, after the paper load whose
// connect rate it reads).
[[nodiscard]] std::string gateway_summary(const graph::Graph& g, std::size_t node);

// A notification fanout's model (phase 3 spec §8.3, book ch. 10): "  dedup=2% channels:
// android=android_q email=email_q ios=ios_q sms=sms_q", the share its dedup step drops (always
// printed: 0% is a fanout that drops none) and its channels, each name with the queue behind it,
// in name order. The shares each channel gets are its `then` steps' factors, which the paper load
// below applies (and the `then` targets' check lines show).
[[nodiscard]] std::string notification_fanout_summary(const model::NodeSpec& spec) {
  if (spec.type != model::NodeType::notification_fanout) return {};
  std::string text = "  dedup=" + percent(spec.dedup);
  if (spec.channels.empty()) return text;
  text += " channels:";
  for (const auto& [channel, queue] : spec.channels) text += " " + channel + "=" + queue;
  return text;
}

[[nodiscard]] std::string explain_nodes(const graph::Graph& g) {
  std::vector<Row> rows;
  for (std::size_t i = 0; i < g.nodes().size(); ++i) {
    const auto& node = g.nodes()[i];
    const bool client = node.spec.type == model::NodeType::client;
    rows.push_back(
        {{node.spec.id},
         {std::string{model::to_string(node.spec.type)}},
         {client ? client_details(node.spec, g.scenario().step)
                 : server_details(node) + kv_cluster_summary(node.spec) + cost_summary(g, i) +
                       size_summary(g, i) + blob_store_summary(g, i) + balancing_summary(g, i) +
                       limiter_summary(node.spec) + cache_summary(g, i) + cdn_summary(g, i) +
                       database_summary(node) + id_generator_summary(node.spec) +
                       message_queue_summary(node.spec) + worker_pool_summary(node) +
                       external_sink_summary(node.spec) + search_index_summary(node.spec) +
                       gateway_summary(g, i) + discovery_summary(node.spec) +
                       notification_fanout_summary(node.spec)}});
  }
  return "nodes:\n" + aligned(rows, Align::all_left);
}

// A defaulted link is half the datacenter round trip of the book's latency table.
[[nodiscard]] std::string link_latency_source(const graph::Graph& g,
                                              const graph::ResolvedLink& link) {
  if (link.latency_source != defaults::Source::dc_default) {
    return std::string{provenance(link.latency_source)};
  }
  const bool cross_dc = g.nodes()[link.from].spec.dc != g.nodes()[link.to].spec.dc;
  return cross_dc ? "(cross_dc_rtt/2)" : "(intra_dc_rtt/2)";
}

// The SI prefixes a network's bit rate is written with (core's byte-rate grammar reads the same).
struct BitPrefix {
  double size;
  std::string_view name;
};
constexpr std::array bit_prefixes{BitPrefix{1e9, "G"}, BitPrefix{1e6, "M"}, BitPrefix{1e3, "k"}};

// "1Gbit/s (125MB/s)": a bandwidth as a network is rated, in bits, then the bytes the pipe
// counts. Derived from the bytes (the model keeps no record of how the scenario wrote it): the bit
// form is printed with the largest prefix, G, M or k, of which bytes × 8 is a whole number:
// 125 MB/s is 1 Gbit/s, 5 GB/s is 40 Gbit/s, and 187.5 MB/s (written 1.5Gbit/s) is 1500Mbit/s.
// When no prefix gives one (1001 B/s) the bytes alone, rather than a bit rate with decimals
// nobody writes.
[[nodiscard]] std::string fmt_bandwidth(ByteRate bandwidth) {
  const std::string bytes = format_byte_rate(bandwidth);
  const double bits = bandwidth.bytes_per_second * 8.0;
  for (const auto& prefix : bit_prefixes) {
    const double count = bits / prefix.size;
    if (count < 1.0 || count != std::round(count)) continue;
    return to_chars_text(count, std::chars_format::fixed, 0) + std::string{prefix.name} +
           "bit/s (" + bytes + ")";
  }
  return bytes;
}

[[nodiscard]] std::string link_details(const graph::Graph& g, const graph::ResolvedLink& link,
                                       const model::LinkSpec& spec) {
  std::vector<std::string> parts{
      "one_way=" + format_duration(link.one_way) + " " + link_latency_source(g, link),
      "delay_steps=" + std::to_string(link.delay_steps)};
  if (spec.bandwidth) parts.push_back("bandwidth=" + fmt_bandwidth(*spec.bandwidth));
  if (!link.ops.empty()) parts.push_back("ops=" + join(link.ops, ","));
  if (link.weight != 1.0) parts.push_back("weight=" + significant(link.weight));
  if (link.feedback) parts.emplace_back("[feedback]");
  return join(parts, "  ");
}

[[nodiscard]] std::string link_name(const graph::Graph& g, const graph::ResolvedLink& link) {
  return g.nodes()[link.from].spec.id + " -> " + g.nodes()[link.to].spec.id;
}

// Graph::build resolves the scenario's links in order, one each, so link i is spec i.
[[nodiscard]] std::string explain_links(const graph::Graph& g) {
  std::vector<Row> rows;
  // C++23 std::views::zip: walks two ranges in lockstep and yields pairs of references (here
  // each resolved link and the scenario link it came from), replacing an index loop over both
  // (and it stops at the shorter one).
  for (const auto [link, spec] : std::views::zip(g.links(), g.scenario().links)) {
    rows.push_back({{link_name(g, link)},
                    {std::string{model::to_string(link.dir)}},
                    {link_details(g, link, spec)}});
  }
  return "links:\n" + aligned(rows, Align::all_left);
}

[[nodiscard]] std::string explain_order(const graph::Graph& g) {
  std::vector<std::string> ids;
  for (const auto index : g.topological_order()) ids.push_back(g.nodes()[index].spec.id);
  std::vector<std::string> feedback;
  for (const auto& link : g.links()) {
    if (link.feedback) feedback.push_back(link_name(g, link));
  }
  return "order: " + join(ids, ", ") + "\n" +
         "feedback edges: " + (feedback.empty() ? std::string{"none"} : join(feedback, ", ")) +
         "\n";
}

// Offered rate per op arriving at each node, requests per second.
using OpLoad = std::map<std::string, double, std::less<>>;

// ---- the peak a client offers ----
//
// The book's estimate is "peak QPS = 2 x average" (System Design Interview ch. 2): a design
// is sized for its busiest moment, not for its average or for midnight. So the checks use
// each client's PEAK rate over the run, found by sampling the deterministic curve (no
// noise: noise is a random excursion around the curve, not part of the plan on paper).

// One sample a minute: fine enough for any wave (a day has 1440 samples) and for ramps,
// cheap enough for runs of weeks. Chosen, not from the book.
constexpr SimTime peak_sample_step = 1_min;

// Moments where a curve's peak can fall between two whole minutes: a spike's start (a
// spike shorter than a minute could hide between samples) and a point of a trace (its
// rates are reached exactly at its times, and interpolation peaks there).
[[nodiscard]] std::vector<SimTime> breakpoints(const workload::RateCurve& curve) {
  std::vector<SimTime> times;
  for (const auto& spike : curve.spikes) times.push_back(spike.at);
  if (curve.points) {
    for (const auto& [time, rate] : curve.points->at) times.push_back(time);
  }
  return times;
}

// The highest rate the curve reaches in [0, duration): every minute, plus the breakpoints.
// The whole run, not one wave period: a spike, a ramp or a trace can peak after the
// wave's first cycle, and sampling a long run is still cheap.
[[nodiscard]] double peak_rate(const workload::RateCurve& curve, SimTime duration) {
  double peak = curve.rate_at(SimTime{0}).per_second;
  for (const auto& sample : workload::samples(curve, peak_sample_step, duration)) {
    peak = std::max(peak, sample.rate.per_second);
  }
  for (const auto t : breakpoints(curve)) {
    if (t.count() < duration.count()) peak = std::max(peak, curve.rate_at(t).per_second);
  }
  return peak;
}

// What the clients send at their peaks: each client's peak rate (peak_rate; a points-only
// client counts too) × op share × route share on each route. Each client's own peak, even
// when two clients peak at different times: an upper bound on the load at any one moment,
// which is the safe side for a capacity check on paper. (`estimate` evaluates t = 0
// instead; the two agree for a constant rate.)
// What a client whose peak rate is `peak` sends on one of its routes: peak × op share × route
// share; nothing for an op its workload does not list.
[[nodiscard]] std::optional<double> peak_on_route(const model::Workload& workload, double peak,
                                                  const graph::Route& route) {
  // C++20 std::ranges::find with a projection (&pair::first): "the share whose op is route.op",
  // comparing each pair's first member with the value, instead of std::find_if and a lambda.
  const auto share =
      std::ranges::find(workload.op_shares, route.op, &std::pair<std::string, double>::first);
  if (share == workload.op_shares.end()) return std::nullopt;
  return peak * share->second * route.share;
}

[[nodiscard]] std::vector<OpLoad> client_load(const graph::Graph& g) {
  std::vector<OpLoad> load(g.nodes().size());
  const auto duration = g.scenario().duration;
  for (std::size_t client = 0; client < g.nodes().size(); ++client) {
    const auto& workload = g.nodes()[client].spec.workload;
    if (!workload) continue;
    const double peak = peak_rate(workload->curve, duration);
    for (const auto& route : g.client_routes(client)) {
      if (const auto rate = peak_on_route(*workload, peak, route)) {
        load[route.target][route.op] += *rate;
      }
    }
  }
  return load;
}

// Each routing node's (load balancer's, rate limiter's, message queue's) routes per op that can
// reach it, as Graph::routes_for gives them, drained routes (share 0) left out: they carry
// nothing. routes_for builds a fresh vector per call, so the table is built once per
// paper_load rather than once per op, node and pass.
using RoutesByOp = std::map<std::string, std::vector<graph::Route>, std::less<>>;
using ForwardRoutes = std::vector<RoutesByOp>;  // per node index; empty for non-forwarders

[[nodiscard]] ForwardRoutes forward_routes(const graph::Graph& g) {
  ForwardRoutes table(g.nodes().size());
  for (std::size_t node = 0; node < g.nodes().size(); ++node) {
    const auto type = g.nodes()[node].spec.type;
    if (!graph::forwards_by_link(type) && !graph::copies_to_consumers(type)) continue;
    for (const auto& op : g.arriving_ops(node)) {
      auto& routes = table[node][op];
      for (const auto& route : g.routes_for(node, op)) {
        if (route.share > 0.0) routes.push_back(route);
      }
    }
  }
  return table;
}

// The routes `op` takes out of routing node `from`; none for an op that cannot reach it.
[[nodiscard]] std::span<const graph::Route> routes_of(const ForwardRoutes& table, std::size_t from,
                                                      std::string_view op) {
  const auto found = table[from].find(op);
  if (found == table[from].end()) return {};
  return found->second;
}

// Forwards what reached load balancer or message queue `from` over its routes, each op times the
// share Graph::routes_for gives: a balancer's split (as LoadBalancerBehaviour::route does), a
// queue's copy of every message to each consumer group, a share of 1 each (as
// MessageQueueBehaviour::route does). On paper nothing is dropped, so every message appended is
// delivered.
void forward_routed(const graph::Graph& g, const ForwardRoutes& table, std::size_t from,
                    std::vector<OpLoad>& load, std::vector<OpLoad>& fed_back) {
  const OpLoad arriving = load[from];  // a copy, as in forward_steps
  for (const auto& [op, rate] : arriving) {
    for (const auto& route : routes_of(table, from, op)) {
      auto& destination = g.links()[route.link].feedback ? fed_back : load;
      destination[route.target][op] += rate * route.share;
    }
  }
}

// Forwards what a rate limiter admits in the long run, min(arriving, limit), the same
// fraction of every op, over its routes as a balancer would, plus one `counter` request
// per CHECKED request (all that arrive) to its backend. On paper the burst is spent (a peak
// lasts longer than a bucket), a window admits limit x window per window, and a leaky
// bucket's queue leaks at `limit`, so one rule covers every algorithm (the sliding
// counter's shortfall under a flood is a step-level effect only `run` shows).
void forward_limited(const graph::Graph& g, const ForwardRoutes& table, std::size_t from,
                     std::vector<OpLoad>& load, std::vector<OpLoad>& fed_back) {
  const auto& node = g.nodes()[from];
  const OpLoad arriving = load[from];  // a copy, as in forward_steps
  // C++23 std::ranges::fold_left: a left fold over a range, without a hand-written loop
  // (and without std::accumulate's int-initial-value trap): here the sum of the op rates.
  const double offered = std::ranges::fold_left(arriving | std::views::values, 0.0, std::plus<>{});
  const double admitted = std::min(offered, node.spec.limit->per_second);
  const double fraction = offered > 0.0 ? admitted / offered : 0.0;
  for (const auto& [op, rate] : arriving) {
    for (const auto& route : routes_of(table, from, op)) {
      auto& destination = g.links()[route.link].feedback ? fed_back : load;
      destination[route.target][op] += rate * fraction * route.share;
    }
  }
  if (node.backend) {
    auto& destination = g.links()[*node.backend_link].feedback ? fed_back : load;
    destination[*node.backend][std::string{graph::counter_op}] += offered;
  }
}

// Forwards what a cache passes on to its store: rate × Graph::backend_fraction (1 − h of a
// read under read_through and at a CDN, whose store is its origin; every write under
// write_through, write_around and write_back: a write-back flush is asynchronous, but the store
// still absorbs it), as CacheBehaviour::route does. A cache_aside cache has no backend: the
// service's on-miss step carries its misses.
void forward_to_backend(const graph::Graph& g, std::size_t from, const OpLoad& arriving,
                        std::vector<OpLoad>& load, std::vector<OpLoad>& fed_back) {
  const auto& node = g.nodes()[from];
  if (!node.backend) return;
  auto& destination = g.links()[*node.backend_link].feedback ? fed_back : load;
  for (const auto& [op, rate] : arriving) {
    destination[*node.backend][op] += rate * g.backend_fraction(from, op);
  }
}

// Forwards along a cache's, a blob store's, a worker pool's or a notification fanout's `then` steps
// (what happens after it answers: ch. 13's browser cache sends its misses on to the servers; an
// upload's event starts transcoding; a transcoder writes its renditions; ch. 10's fanout publishes
// a notification to each of its channels): rate × factor × Graph::then_fraction, the whole flow for
// an unconditional step (at a blob store, every write and no read; at a pool, every job whatever
// its op; at a fanout, the 1 − dedup its dedup step lets through) and the misses (1 − h of a read)
// for an `on: miss` one. A share of 0 sends nothing, not a 0/s entry. The same multiplications as
// CacheBehaviour::route (spec §18 entry 5), BlobStoreBehaviour::route, WorkerPoolBehaviour::route
// and NotificationFanoutBehaviour::route.
void forward_then_steps(const graph::Graph& g, std::size_t from, const OpLoad& arriving,
                        std::vector<OpLoad>& load, std::vector<OpLoad>& fed_back) {
  for (const model::Step& step : g.nodes()[from].spec.then) {
    const auto target = g.index_of(step.to);
    const auto link = target ? g.demand_link(from, *target) : std::nullopt;
    if (!link) continue;  // Graph::build proved it exists; skip defensively
    auto& destination = g.links()[*link].feedback ? fed_back : load;
    for (const auto& [op, rate] : arriving) {
      const double share = g.then_fraction(from, step, op);
      if (share > 0.0) destination[*target][step.op] += rate * step.factor * share;
    }
  }
}

// How many times a worker pool with `retries: n` drains each job on paper: the capped geometric
// series of behaviours::WorkerPoolBehaviour,
//   A(f, n) = 1 + f + f² + ... + f^n
// (attempt k happens only if the k earlier ones failed, f^k when each fails independently with
// the same f), with f the share of the jobs its external sinks fail: each sink's `failure_rate`
// combined over its sink calls, 1 − Π (1 − failure_rate), a job any of them fails being retried
// (on paper no other step loses anything, so every call is reached). The quota's shedding, which a
// run counts as a failure too, is left out: a sink offered more than its capacity is red on its
// own line. 1 for a pool without retries.
[[nodiscard]] double paper_attempts_per_job(const graph::Graph& g, std::size_t pool) {
  const auto& spec = g.nodes()[pool].spec;
  const int retries = spec.retries.value_or(0);
  if (retries == 0) return 1.0;
  double every_sink_answers = 1.0;
  for (const model::Step& step : spec.then) {
    const auto target = g.index_of(step.to);
    if (!target || g.nodes()[*target].spec.type != model::NodeType::external_sink) continue;
    every_sink_answers *= 1.0 - g.nodes()[*target].spec.failure_rate;
  }
  const double failed = 1.0 - every_sink_answers;
  double attempts = 0.0;
  double chance_of_this_attempt = 1.0;  // f^k for the k-th retry
  for (int k = 0; k <= retries; ++k) {
    attempts += chance_of_this_attempt;
    chance_of_this_attempt *= failed;
  }
  return attempts;
}

// A worker pool's offered load times its attempts per job (paper_attempts_per_job): the jobs it
// re-publishes come back through its queue to it alone (engine::Simulation::
// keep_retries_in_their_group), and every attempt runs its `then` steps, so its sinks are offered
// the attempts too. In place: each pass rebuilds `load` from the clients (paper_pass), so the
// factor never compounds.
void add_pool_retries(const graph::Graph& g, std::size_t pool, std::vector<OpLoad>& load) {
  const double attempts = paper_attempts_per_job(g, pool);
  if (attempts == 1.0) return;
  for (auto& [op, rate] : load[pool]) rate *= attempts;
}

// What a cache or a CDN passes on: to its store (a CDN's origin) and, for a cache, along its
// `then` steps (a CDN has none). The hits stop here.
void forward_cached(const graph::Graph& g, std::size_t from, std::vector<OpLoad>& load,
                    std::vector<OpLoad>& fed_back) {
  const OpLoad arriving = load[from];  // a copy, as in forward_steps
  forward_to_backend(g, from, arriving, load, fed_back);
  forward_then_steps(g, from, arriving, load, fed_back);
}

// Forwards what reached node `from` along its resolved `ops` hops (rate × factor × the share
// of requests that take the hop: 1, or a cache's miss fraction 1 − h for an `on: miss`
// step, whose factor includes its lookup's, so every missed lookup is fetched;
// graph::ServiceHop), as ServiceBehaviour::route does, or over a load balancer's, a rate
// limiter's or a message queue's routes, or to a cache's store, or along a blob store's events or
// a worker pool's hand-offs. Flow over a feedback link goes to `fed_back`: its target came earlier
// in the order, so it counts in the next pass. On paper nothing is lost, so no step is thinned by
// an earlier one's losses as in a run (detail::HopBlocks::forward): a later step's peak offered
// load is the upper bound of a request that never fails before it.
void forward_steps(const graph::Graph& g, const ForwardRoutes& table, std::size_t from,
                   std::vector<OpLoad>& load, std::vector<OpLoad>& fed_back) {
  const auto& spec = g.nodes()[from].spec;
  if (spec.type == model::NodeType::client) return;  // client routes are in client_load
  if (spec.type == model::NodeType::load_balancer || graph::copies_to_consumers(spec.type)) {
    forward_routed(g, table, from, load, fed_back);
    return;
  }
  if (spec.type == model::NodeType::rate_limiter) {
    forward_limited(g, table, from, load, fed_back);
    return;
  }
  if (graph::hits_or_misses(spec.type)) {
    forward_cached(g, from, load, fed_back);
    return;
  }
  // What a blob store or a worker pool sends on along its `then` steps: a store one event per
  // write, rate × factor, and nothing for a read (spec §19 entries 6-7, as
  // BlobStoreBehaviour::route does; S3 has no event for a GET); a pool every step for every job it
  // is OFFERED. On paper nothing queues or is dropped: every write that arrives is served and
  // notifies, and a pool offered more than its workers / cost passes all of it on (its own line is
  // red; what the engine forwards is what the pool drains, at most its capacity). The paper load
  // answers "can this ever fit", not "what goes through". A pool with retries drains each job
  // A(f, n) times (add_pool_retries).
  // A notification fanout publishes each notification its dedup step lets through, rate × (1 −
  // dedup) × factor per step (phase 3 spec §8.3; NotificationFanoutBehaviour::route), and is
  // offered every notification, duplicates included (finding one is work too).
  if (spec.type == model::NodeType::blob_store || spec.type == model::NodeType::worker_pool ||
      spec.type == model::NodeType::notification_fanout) {
    if (spec.type == model::NodeType::worker_pool) add_pool_retries(g, from, load);
    const OpLoad arriving = load[from];  // a copy: a step may target `from` itself
    forward_then_steps(g, from, arriving, load, fed_back);
    return;
  }
  const OpLoad arriving = load[from];  // a copy: a step may target `from` itself
  for (const auto& [op, rate] : arriving) {
    for (const graph::ServiceHop& hop : g.service_hops(from, op)) {
      if (hop.fraction <= 0.0) continue;  // never taken (a cache that always hits)
      // An `on: failure` step is sent what the step it reports on failed (graph::ServiceHop), and
      // on paper nothing fails: it adds nothing to the load (its target's check line says so when
      // nothing else reaches it: failure_fed_note).
      if (hop.reports_on) continue;
      const auto link = g.demand_link(from, hop.target);
      if (!link) continue;  // Graph::build proved it exists; skip defensively
      auto& destination = g.links()[*link].feedback ? fed_back : load;
      destination[hop.target][hop.op] += rate * hop.factor * hop.fraction;
    }
  }
}

// ---- a websocket gateway on paper (phase 3 spec §8.2) ----

// The level of open connections a ws_gateway holds on paper, `connects` the connect requests a
// second that reach it: a fixed `connections` as stated; else the fixed point of the level,
//   C* = connects × session            (Little's law: L = λW, the connections open at once)
// capped at max_connections × replicas (behaviours::WsGatewayBehaviour, which `estimate` reports
// too); nothing for a gateway with neither (the engine refuses it to run, and explain says so).
[[nodiscard]] std::optional<double> paper_level(const model::NodeSpec& spec, double connects) {
  if (spec.connections) return static_cast<double>(*spec.connections);
  if (!(connects > 0.0)) return std::nullopt;
  const double level = connects * seconds(defaults::session_of(spec));
  const auto cap = model::connection_cap(spec);
  return cap ? std::min(level, *cap) : level;
}

// The heartbeats of a gateway's open connections, C / heartbeat a second (0 without a stated
// heartbeat or a level): the load its connections put on it and on what its `heartbeat` steps call.
[[nodiscard]] double paper_heartbeats(const model::NodeSpec& spec, std::optional<double> level) {
  if (!level || !spec.heartbeat) return 0.0;
  return *level / seconds(*spec.heartbeat);
}

// The connect requests a second in one node's load of a pass (its `connect` op).
[[nodiscard]] double connects_in(const OpLoad& load) {
  const auto found = load.find(model::connect_op);
  return found == load.end() ? 0.0 : found->second;
}

// Adds a ws_gateway's heartbeats to its own load before it forwards (paper_pass): they arrive as
// its `heartbeat` op beside what the clients send under that name, as engine::Simulation::receive
// adds them in a run, so the gateway's check counts them and its `heartbeat` steps carry them on (a
// presence service is checked against the 20k/s that 100k connections at 5 s send, not against
// nothing). In place: each pass rebuilds `load` from the clients, so nothing compounds.
void add_paper_heartbeats(const graph::Graph& g, std::size_t node, std::vector<OpLoad>& load) {
  const auto& spec = g.nodes()[node].spec;
  if (spec.type != model::NodeType::ws_gateway) return;
  const double beats = paper_heartbeats(spec, paper_level(spec, connects_in(load[node])));
  if (beats > 0.0) load[node][std::string{model::heartbeat_op}] += beats;
}

// The paper load's stopping rule, the same as engine::Simulation::estimate's (whose
// constants are internal to the engine, so the numbers are restated here): stop when no
// node's total moved by more than 1e-12 of its value (a few thousand ulps of a double, as
// close to "equal" as rounding allows), or give up after 100'000 passes.
constexpr double paper_tolerance = 1e-12;
constexpr int max_paper_passes = 100'000;

// The offered load at every node (in capacity units; per op in requests, `by_op`), and
// whether the passes reached a fixed point.
struct PaperLoad {
  std::vector<double> totals;
  std::vector<OpLoad> by_op;
  bool converged{};
  // Per node, unconverged only: its total was still growing when the passes stopped and its
  // pass-to-pass increments did not shrink (a loop of gain >= 1 feeds it), so `totals` holds a
  // pass's figure, not a load (paper_load).
  std::vector<bool> unbounded{};
};

// What one pass of the paper load produced: each node's total in capacity units and its load per
// op in requests.
struct PaperPass {
  std::vector<double> totals;
  std::vector<OpLoad> by_op;
};

// One pass: what the clients send plus what the feedback links carried in the previous pass
// (`fed_back`, replaced by this pass's), pushed through every node in topological order. Returns
// the total offered load per node in capacity units (Σ rate_op × cost_op, spec §17 entry 4), the
// quantity the node's capacity is a rate of. Forwarding stays in requests: a cost prices work at
// one node, it does not multiply requests.
[[nodiscard]] std::vector<double> paper_pass(const graph::Graph& g, const ForwardRoutes& table,
                                             const std::vector<OpLoad>& offered,
                                             std::vector<OpLoad>& fed_back,
                                             std::vector<OpLoad>& load) {
  load = offered;
  for (std::size_t i = 0; i < load.size(); ++i) {
    for (const auto& [op, rate] : fed_back[i]) load[i][op] += rate;
  }
  std::vector<OpLoad> next_fed_back(g.nodes().size());
  for (const auto index : g.topological_order()) {
    add_paper_heartbeats(g, index, load);
    forward_steps(g, table, index, load, next_fed_back);
  }
  fed_back = std::move(next_fed_back);
  std::vector<double> totals;
  for (std::size_t i = 0; i < load.size(); ++i) {
    // std::ranges::fold_left (see forward_limited), each op weighed by its cost.
    totals.push_back(std::ranges::fold_left(load[i], 0.0, [&](double sum, const auto& entry) {
      return sum + entry.second * graph::op_cost(g, i, entry.first);
    }));
  }
  return totals;
}

// A total that moved by no more than the tolerance (NaN never counts as unchanged).
[[nodiscard]] bool unchanged_total(double before, double after) {
  return after == before || std::abs(after - before) <= paper_tolerance * std::abs(after);
}

// No total moved by more than the tolerance.
[[nodiscard]] bool settled(std::span<const double> before, std::span<const double> after) {
  // C++20 std::ranges::all_of: the range itself instead of a begin/end pair (std::all_of), so it
  // also takes a view such as this zip directly.
  return std::ranges::all_of(std::views::zip(before, after), [](const auto& pair) {
    const auto [b, a] = pair;
    return unchanged_total(b, a);
  });
}

[[nodiscard]] bool all_finite(std::span<const double> totals) {
  return std::ranges::all_of(totals, [](double t) { return std::isfinite(t); });
}

// The nodes whose totals moved between the last two passes (`after` may have overflowed): a loop of
// gain >= 1 still feeds them. Their last figure is the pass cap's or the double's, not a load
// ("5.618e+307/s").
[[nodiscard]] std::vector<bool> still_growing(std::span<const double> before,
                                              std::span<const double> after) {
  std::vector<bool> growing;
  for (const auto [b, a] : std::views::zip(before, after)) {
    // An overflowed total (inf) is as close to inf as the tolerance allows: test it first.
    growing.push_back(!std::isfinite(a) || !unchanged_total(b, a));
  }
  return growing;
}

// The last finite pass of a paper load whose next pass overflowed, its growing nodes marked: a
// loop of gain well above 1 (2 doubles its flow every lap) reaches the double's limit long before
// the pass cap.
[[nodiscard]] PaperLoad overflowed(PaperPass last, std::span<const double> next) {
  auto unbounded = still_growing(last.totals, next);
  // C++20 designated initializers: name the fields being set, so the vectors cannot be passed in
  // the wrong order unnoticed; a field left out keeps its default member initializer. Replaces
  // positional braces, which say nothing about which value is which.
  return {.totals = std::move(last.totals),
          .by_op = std::move(last.by_op),
          .converged = false,
          .unbounded = std::move(unbounded)};
}

// How much smaller than the previous one a pass's increment must be for the loop to count as
// converging: a loop of gain 1 adds the same amount every pass, which rounding can make look a
// hair smaller (a part in 1e11 after 100,000 passes). Chosen: a gain within a millionth of 1 is
// called unbounded; its fixed point would be over a million times the flow it is fed.
constexpr double shrink_margin = 1e-6;

// The share of the last increment that the next pass adds again, r = d_next / d_last, for a node
// whose increments shrink geometrically (r < 1 - shrink_margin); empty for one whose increments do
// not shrink. A loop of gain g adds g times its previous lap each pass, so r tends to g.
[[nodiscard]] std::optional<double> shrinking_ratio(double before, double last, double next) {
  const double last_step = last - before;
  const double next_step = next - last;
  if (!(last_step > 0.0)) return std::nullopt;
  const double ratio = next_step / last_step;
  if (ratio >= 1.0 - shrink_margin) return std::nullopt;
  return std::max(ratio, 0.0);
}

// `next` + the rest of the geometric series its increments follow: each pass adds r times the
// previous increment, so after an increment d the passes still to come add d (r + r² + ...)
// = d r / (1 - r): the fixed point of the iteration, its error shrinking by r per pass (the
// geometric series' sum, the same that makes a loop of gain g settle at input / (1 - g)).
[[nodiscard]] double extrapolated(double last, double next, double ratio) {
  return next + (next - last) * ratio / (1.0 - ratio);
}

// The node's load per op extrapolated with its totals' ratio: every op on a loop grows by the
// loop's gain, so each op's increment shrinks like the total's.
[[nodiscard]] OpLoad extrapolated_ops(const OpLoad& last, const OpLoad& next, double ratio) {
  OpLoad ops = next;
  for (auto& [op, rate] : ops) {
    const auto found = last.find(op);
    rate = extrapolated(found == last.end() ? 0.0 : found->second, rate, ratio);
  }
  return ops;
}

// The pass cap reached with some totals still moving. Node by node, from the last three passes: a
// loop of gain just below 1 converges too slowly for the cap (0.9999 needs about 276,000 passes to
// reach the tolerance), but its increments shrink by the gain every pass, so its fixed point is the
// geometric series' sum (extrapolated); a loop of gain >= 1 has none, and its nodes' increments do
// not shrink: they are marked unbounded, their figure the last pass's. Converged when no node is
// unbounded.
[[nodiscard]] PaperLoad at_pass_cap(const PaperPass& before, const PaperPass& last,
                                    PaperPass next) {
  PaperLoad load{.totals = std::move(next.totals), .by_op = std::move(next.by_op)};
  load.unbounded.assign(load.totals.size(), false);
  for (std::size_t i = 0; i < load.totals.size(); ++i) {
    if (unchanged_total(last.totals[i], load.totals[i])) continue;
    const auto ratio = shrinking_ratio(before.totals[i], last.totals[i], load.totals[i]);
    if (!ratio) {
      load.unbounded[i] = true;
      continue;
    }
    load.totals[i] = extrapolated(last.totals[i], load.totals[i], *ratio);
    load.by_op[i] = extrapolated_ops(last.by_op[i], load.by_op[i], *ratio);
  }
  load.converged = !std::ranges::contains(load.unbounded, true);
  if (load.converged) load.unbounded.clear();
  return load;
}

// One pass of the paper load (paper_pass), its totals and its loads per op together.
[[nodiscard]] PaperPass next_pass(const graph::Graph& g, const ForwardRoutes& table,
                                  const std::vector<OpLoad>& offered,
                                  std::vector<OpLoad>& fed_back) {
  PaperPass pass;
  pass.totals = paper_pass(g, table, offered, fed_back, pass.by_op);
  return pass;
}

// The paper calculation (System Design Interview ch. 2, back-of-the-envelope QPS): the
// offered load at every node, propagated from the clients through route shares and step
// factors in topological order. No queueing, no drops, no delays: it is what a student
// would do by hand, and what the long-run estimate shows below saturation.
// A feedback loop is solved by fixed-point iteration: each pass carries the loop's flow
// one lap further, so a loop of gain g (the fraction of its flow it sends round again)
// approaches input / (1 - g), the geometric series' sum, after about
// log(tolerance) / log(g) passes (40 for g = 0.5). A loop of gain >= 1 has no fixed point:
// its totals grow until they overflow (the last finite pass is returned, unconverged) or until
// the pass cap, where a loop whose increments still shrink is extrapolated to its fixed point
// and the others are marked unbounded (at_pass_cap). Unlike estimate, nothing caps a node at its
// capacity here.
[[nodiscard]] PaperLoad paper_load(const graph::Graph& g) {
  const auto offered = client_load(g);
  const auto table = forward_routes(g);
  std::vector<OpLoad> fed_back(g.nodes().size());
  PaperPass before;
  PaperPass last = next_pass(g, table, offered, fed_back);
  for (int pass = 1; pass < max_paper_passes; ++pass) {
    PaperPass next = next_pass(g, table, offered, fed_back);
    if (!all_finite(next.totals)) return overflowed(std::move(last), next.totals);
    if (settled(last.totals, next.totals)) {
      return {.totals = std::move(next.totals), .by_op = std::move(next.by_op), .converged = true};
    }
    before = std::exchange(last, std::move(next));
  }
  // One pass more at the cap, to set its increment against the last one's.
  PaperPass next = next_pass(g, table, offered, fed_back);
  if (!all_finite(next.totals)) return overflowed(std::move(last), next.totals);
  return at_pass_cap(before, last, std::move(next));
}

// A ws_gateway's line ends with what its connections will be and cost (phase 3 spec §8.2;
// behaviours::WsGatewayBehaviour): "  connections: C*=100k at the peak connect rate (connect 100/s
// × session 16.7m), heartbeat 20k/s, cap 200k". C* is the fixed point connect rate × session
// (Little's law), the connect rate the clients' PEAK `connect` requests that reach the gateway (the
// paper load's, which the line names: off the peak of a curve the level is lower), the session
// marked "(default)" when the scenario left it to defaults::default_session; then the heartbeat
// load at C* (C* / heartbeat) and the cap, max_connections × replicas. A capped fixed point says
// what it would have been: "C*=50k at the peak connect rate (connect 100/s × session 16.7m = 100k,
// capped)". A run too short to reach C* says so (settling_note). A fixed level reads "connections:
// 2M (fixed)". A gateway with neither level, or both, is one run and estimate refuse
// (engine::Simulation::create), and its line says so. Nothing on any other type.
[[nodiscard]] std::string gateway_level_text(const model::NodeSpec& spec, double connects) {
  if (spec.connections) {
    const std::string fixed = with_si_prefix(static_cast<double>(*spec.connections)) + " (fixed)";
    if (connects > 0.0) return fixed + " and a connect op: run refuses both";
    return fixed;
  }
  if (!(connects > 0.0)) {
    return "none (no connect op reaches it, no fixed connections: run refuses it)";
  }
  const SimTime session = defaults::session_of(spec);
  const double unbounded = connects * seconds(session);
  const double level = *paper_level(spec, connects);
  const std::string capped =
      level < unbounded ? " = " + with_si_prefix(unbounded) + ", capped" : "";
  return "C*=" + with_si_prefix(level) + " at the peak connect rate (connect " +
         fmt_rate(connects) + " × session " + format_duration(session) +
         (spec.session ? "" : " (default)") + capped + ")";
}

// The level rises to C* as a first-order lag of time constant `session` (C(t) = C* (1 − e^{−t /
// session})): 1 − e^{−3} = 95 % of the way after 3 sessions, the usual "settled" of a lag.
constexpr int sessions_to_settle = 3;

// "; settles in about 3 sessions (1.5h); this run lasts 1m" when the scenario's duration is under
// 3 sessions: the run ends before the level gets near C*, so the reports' `connections` will be
// below the line's C*. Nothing for a fixed level (no lag), a gateway without a level, or a capped
// one: the cap stops the lag partway up, sooner than 3 sessions (at −session × ln(1 − cap / (λ
// session))), so the note would overstate the wait.
[[nodiscard]] std::string settling_note(const graph::Graph& g, const model::NodeSpec& spec,
                                        double connects) {
  if (spec.connections || !(connects > 0.0)) return {};
  const SimTime session = defaults::session_of(spec);
  if (*paper_level(spec, connects) < connects * seconds(session)) return {};  // capped
  // In ticks: this unit does not include <chrono>, whose operators SimTime arithmetic needs.
  const SimTime settles{sessions_to_settle * session.count()};
  const SimTime duration = g.scenario().duration;
  if (duration.count() >= settles.count()) return {};
  return "; settles in about " + std::to_string(sessions_to_settle) + " sessions (" +
         format_duration(settles) + "); this run lasts " + format_duration(duration);
}

std::string gateway_summary(const graph::Graph& g, std::size_t node) {
  const auto& spec = g.nodes()[node].spec;
  if (spec.type != model::NodeType::ws_gateway) return {};
  const double connects = connects_in(paper_load(g).by_op[node]);
  const auto level = paper_level(spec, connects);
  std::vector<std::string> parts{gateway_level_text(spec, connects)};
  if (level) {  // without a level there is nothing to beat: no heartbeat figure
    parts.push_back(spec.heartbeat ? "heartbeat " + fmt_rate(paper_heartbeats(spec, level))
                                   : "no heartbeat");
  }
  const auto cap = model::connection_cap(spec);
  parts.push_back(cap ? "cap " + with_si_prefix(*cap) : "no cap");
  return "  connections: " + join(parts, ", ") + settling_note(g, spec, connects);
}

// ---- loads that rest on an unknown hit ratio ----
//
// A cache or CDN with no number for h that the engine refuses to run (a `che` CDN without a
// `keys` size; a cache built in code without a hit ratio: Graph::build resolves every other
// `che` to a number) counts every read as a miss on paper (Graph::miss_fraction): the nodes
// behind its misses are checked against their worst case, an upper bound. Their check lines say
// so, or a student would read a red database as a flaw of the design. A CDN the scenario gave no
// hit ratio is not one of them: it misses every read, which is what the scenario says and what
// runs, so the load behind it is the load (its own line says h = 0, cdn_summary).

// What a check line resting on such a cache's misses ends with.
constexpr std::string_view unknown_hit_ratio_note =
    " (hit ratio unknown: counted as 0 — an upper bound)";

// Does `node` miss at a rate nobody knows yet?
[[nodiscard]] bool unknown_misses(const graph::Graph& g, std::size_t node) {
  const auto& spec = g.nodes()[node].spec;
  if (!graph::hits_or_misses(spec.type) || g.hit_ratio(node).has_value()) return false;
  if (spec.type == model::NodeType::cdn) return spec.hit_ratio && spec.hit_ratio->che;
  return true;
}

// The nodes that receive miss flow of such a cache directly: the targets of a service's
// `on: miss` steps after it (ServiceHop::lookup), its store hop (a read_through backend, a CDN's
// origin), and the targets of a cache's own `on: miss` then-steps.
[[nodiscard]] std::vector<std::size_t> unknown_miss_targets(const graph::Graph& g) {
  std::vector<std::size_t> targets;
  for (std::size_t node = 0; node < g.nodes().size(); ++node) {
    const auto& spec = g.nodes()[node].spec;
    // C++20 `std::views::keys`: a lazy view of each map entry's key (the op name), replacing a
    // loop over the pairs that ignores `.second`; nothing is copied.
    for (const auto& op : spec.ops | std::views::keys) {
      const auto hops = g.service_hops(node, op);
      for (const graph::ServiceHop& hop : hops) {
        if (hop.lookup && unknown_misses(g, hops[*hop.lookup].target))
          targets.push_back(hop.target);
      }
    }
    if (!unknown_misses(g, node)) continue;
    const auto& backend = g.nodes()[node].backend;
    if (backend && graph::fetches_read_misses(spec)) targets.push_back(*backend);
    for (const model::Step& step : spec.then) {
      if (step.on == model::StepCondition::miss) targets.push_back(*g.index_of(step.to));
    }
  }
  return targets;
}

// Every node whose paper load contains such miss flow: the direct targets and everything their
// demand edges reach (a node's load counts what flows into it, so the bound travels on). Over
// the edges, not per op: it can flag a node that only an unrelated op of a flagged node
// reaches, the safe side for a caveat.
[[nodiscard]] std::vector<bool> counts_unknown_hit_ratio(const graph::Graph& g) {
  std::vector<bool> flagged(g.nodes().size(), false);
  std::vector<std::size_t> work = unknown_miss_targets(g);
  while (!work.empty()) {
    const auto node = work.back();
    work.pop_back();
    if (flagged[node]) continue;
    flagged[node] = true;
    for (const auto& edge : g.edges()) {
      if (edge.from == node) work.push_back(edge.to);
    }
  }
  return flagged;
}

// ---- load the paper pass cannot follow yet ----
//
// The paper load (paper_pass, forward_steps) forwards from a client's routes, along a service's (or
// any node's) `ops` steps, through load balancers and rate limiters over their links
// (graph::forwards_by_link), from caches and CDNs on their misses (graph::hits_or_misses), along a
// blob store's `then` steps, its event notifications, from a message queue into each of its
// consumer groups and along a worker pool's and a notification fanout's `then` steps. It does not
// follow the `then` of a URL frontier (a type without a model yet). A node fed through one
// misses that part of its load: with nothing else it shows 0 offered load, a missing number,
// not a zero, and a verdict on it ("0%, green") would read as "this tier is idle"; with some load
// from elsewhere its percentage is a lower bound, and a green or yellow verdict on it would read as
// "this tier has room" (a node fed 1/s by a client and 10k/s through a blob store once printed
// "(0.05%, green)"). Its check line says so instead.

// Until when the paper load leaves unfollowed what a node of `type` sends on: "not modelled yet",
// or nothing for a type it follows (or that sends nothing on).
[[nodiscard]] std::optional<std::string_view> unfollowed_until(model::NodeType type) {
  using enum model::NodeType;  // C++20, as in provenance
  if (type == frontier) return "not modelled yet";
  return std::nullopt;
}

// Where `from` sends on what it receives. For a type the paper load does not follow: the targets
// of its `then` steps, the flow it would send; for one with none (a frontier that only feeds its
// consumers over its links), the nodes its links carry demand to. For every other node: its demand
// edges (steps, client routes, a balancer's links, a cache's backend, a blob store's events, a
// queue's consumer groups, a pool's hand-offs), what the paper load follows from it.
[[nodiscard]] std::vector<std::size_t> onward_targets(const graph::Graph& g, std::size_t from) {
  std::vector<std::size_t> targets;
  const auto& spec = g.nodes()[from].spec;
  if (!unfollowed_until(spec.type)) {
    for (const graph::DemandEdge& edge : g.edges()) {
      if (edge.from == from) targets.push_back(edge.to);
    }
    return targets;
  }
  for (const model::Step& step : spec.then) targets.push_back(*g.index_of(step.to));
  if (!targets.empty()) return targets;
  for (std::size_t to = 0; to < g.nodes().size(); ++to) {
    if (to != from && g.demand_link(from, to)) targets.push_back(to);
  }
  return targets;
}

// Adds each of `types` that `into` does not hold yet, in order; true when one was added.
bool add_missing(std::vector<model::NodeType>& into, std::span<const model::NodeType> types) {
  bool added = false;
  for (const model::NodeType type : types) {
    if (std::ranges::contains(into, type)) continue;
    into.push_back(type);
    added = true;
  }
  return added;
}

// Per node, the types on its way in whose onward flow the paper load does not follow, each once,
// nearest first: the web crawler's dns, behind the frontier and the downloader, has [frontier].
// (Two such types, as a notification fan-out feeding a frontier was until the fan-out got its
// model, give [frontier, notification_fanout]: the list holds as many as there are.)
// Empty for a node whose load the paper pass has in full. A node passes on its own type (if it is
// such a type) and everything on its way, so the mark crosses ordinary nodes too (the web
// crawler's parser, behind the downloader). A node in a loop through such a type has it on its way
// as well: the load that comes round is missing too. Breadth-first from every such node at once,
// which puts the nearest types first; a node is visited again only when its list grows, which
// happens at most once per type, so a loop ends.
[[nodiscard]] std::vector<std::vector<model::NodeType>> blocked_on_the_way(const graph::Graph& g) {
  const auto count = g.nodes().size();
  std::vector<std::vector<model::NodeType>> blocked(count);
  std::deque<std::size_t> work;
  for (std::size_t node = 0; node < count; ++node) {
    if (unfollowed_until(g.nodes()[node].spec.type)) work.push_back(node);
  }
  while (!work.empty()) {
    const auto from = work.front();
    work.pop_front();
    std::vector<model::NodeType> passed_on;
    const auto type = g.nodes()[from].spec.type;
    if (unfollowed_until(type)) passed_on.push_back(type);
    add_missing(passed_on, blocked[from]);
    for (const std::size_t to : onward_targets(g, from)) {
      if (add_missing(blocked[to], passed_on)) work.push_back(to);
    }
  }
  return blocked;
}

// "a", "a and b", "a, b and c".
[[nodiscard]] std::string spoken_list(std::span<const std::string> items) {
  if (items.size() <= 1) return items.empty() ? std::string{} : items.front();
  return join(items.first(items.size() - 1), ", ") + " and " + items.back();
}

// "load not propagated: frontier is not modelled yet)" or, with several types, grouped by how long
// each stays unfollowed, in the order they come ("... a and b are not modelled yet)"). (The
// frontier is the one type left unfollowed now; the grouping will hold the next phase's types
// apart.)
[[nodiscard]] std::string unpropagated_mark(std::span<const model::NodeType> blocked) {
  std::vector<std::pair<std::string_view, std::vector<std::string>>> groups;
  for (const model::NodeType type : blocked) {
    const auto until = *unfollowed_until(type);
    auto group = std::ranges::find(groups, until, [](const auto& g) { return g.first; });
    if (group == groups.end())
      group = groups.emplace(groups.end(), until, std::vector<std::string>{});
    group->second.emplace_back(model::to_string(type));
  }
  std::vector<std::string> reasons;
  for (const auto& [until, types] : groups) {
    reasons.push_back(spoken_list(types) + (types.size() == 1 ? " is " : " are ") +
                      std::string{until});
  }
  return "load not propagated: " + join(reasons, ", ") + ")";
}

// Behind a hash ring (imbalance k > 1) the group's average hides the copy that breaks
// first: the hottest of N copies is sent k / N of the offered load against ONE copy's
// capacity (engine::Simulation::make_server models it apart). The consistent-hashing lab's
// 40k/s on 4 copies of 15k/s, tried with vnodes 1 (k = 2): 67% on average, but 20k/s vs
// 15k/s, 133%, on the hottest.
[[nodiscard]] std::string hottest_copy_check(const graph::ResolvedNode& node, double offered) {
  if (node.imbalance <= 1.0) return {};
  const double hot = offered * node.imbalance / node.spec.replicas;
  const double copy_capacity = node.capacity.service_rate.per_second;
  const std::string head =
      "  hottest copy " + fmt_units(node.spec, hot) + " vs " + fmt_units(node.spec, copy_capacity);
  const std::string imbalance = "imbalance " + significant(node.imbalance, 3) + ")";
  if (copy_capacity <= 0.0) return head + " (no capacity, red; " + imbalance;
  const double rho = hot / copy_capacity;
  return head + " (" + percent(rho) + ", " +
         std::string{zone_name(fluid::zone_of(rho, node.capacity))} + "; " + imbalance;
}

// One kind of copy of a database at the paper load, in capacity units (a write costs 5 of them
// by default, so these are not requests): "primary 500 units/s vs 1k/s (50%, green)".
[[nodiscard]] std::string copy_check(std::string_view name, double load,
                                     const graph::ResolvedNode& node) {
  const double capacity = node.capacity.service_rate.per_second;
  const std::string head =
      std::string{name} + " " + with_si_prefix(load) + " units/s vs " + fmt_rate(capacity);
  if (capacity <= 0.0) return head + " (no capacity, red)";
  const double rho = load / capacity;
  return head + " (" + percent(rho) + ", " +
         std::string{zone_name(fluid::zone_of(rho, node.capacity))} + ")";
}

// A database's paper load per copy (graph::database_copy_loads, the split the engine uses), in
// capacity units: its arriving reads and writes, each op priced by its cost (a write 5 by
// default). Empty for any other node.
[[nodiscard]] std::optional<graph::DatabaseCopyLoads> database_loads(
    const graph::ResolvedNode& node, const OpLoad& arriving) {
  if (node.spec.type != model::NodeType::sql_db) return std::nullopt;
  double reads = 0.0;
  double writes = 0.0;
  for (const auto& [op, rate] : arriving) {
    (graph::is_write(node.spec, op) ? writes : reads) += rate * defaults::op_cost(node.spec, op);
  }
  return graph::database_copy_loads(node.spec, reads, writes);
}

// A SQL database's total capacity hides where its load goes (engine::Simulation::
// make_database): with N copies per shard and S shards, ONE primary is sent writes / S and ONE
// replica reads / ((N − 1) S) + writes / S (it replays every write of its shard), each against
// one copy's capacity. "  primary 100 units/s vs 1k/s (10%, green), each replica 600 units/s vs
// 1k/s (60%, green)"; without replicas, the primary alone with everything / S. In capacity
// units, and labelled so: a write costs 5 of them unless `cost` says otherwise.
[[nodiscard]] std::string database_check(const graph::ResolvedNode& node,
                                         const std::optional<graph::DatabaseCopyLoads>& loads) {
  if (!loads) return {};
  const std::string primary = "  " + copy_check("primary", loads->primary, node);
  if (!loads->replica) return primary;
  return primary + ", " + copy_check("each replica", *loads->replica, node);
}

// The zone the headline gives a node: its average's, except for a database, whose average counts
// primaries that serve no reads (a green average beside red replicas): there the busiest copy's,
// "(55%, red at the busiest copy)" when the two differ.
[[nodiscard]] std::string headline_zone(const graph::ResolvedNode& node, double rho,
                                        const std::optional<graph::DatabaseCopyLoads>& loads) {
  const fluid::Zone average = fluid::zone_of(rho, node.capacity);
  if (!loads) return std::string{zone_name(average)};
  const double busiest_rho = std::max(loads->primary, loads->replica.value_or(0.0)) /
                             node.capacity.service_rate.per_second;
  const fluid::Zone busiest = fluid::zone_of(busiest_rho, node.capacity);
  if (busiest == average) return std::string{zone_name(average)};
  return std::string{zone_name(busiest)} + " at the busiest copy";
}

// Utilization at the paper load: peak offered / (capacity per replica × replicas, × shards for a
// sharded database: every shard is a full set of copies), with the node's own zone thresholds (a
// database's busiest copy's, headline_zone), and the hottest copy's behind a hash ring. Both are in
// capacity units (requests when every cost is 1); a database's are labelled so, "peak offered 1.5k
// units/s", as its per-copy checks are (a write costs 5 of them), and a byte-sized node's are
// megabytes, "peak offered 14.08k MB/s (281.6/s)", the requests beside them (spec §19 entry 2: the
// request's own size in MB, Graph::request_bytes, so the percentage is the bytes' and youtube's
// cdn_us is at 14.1 %). Capacity per replica × replicas, × shards for a sharded database (every
// shard is a full set of copies), in capacity units per second.
[[nodiscard]] double total_capacity(const graph::ResolvedNode& node) {
  const double copies = node.spec.replicas * node.spec.shards.value_or(1);
  return node.capacity.service_rate.per_second * copies;
}

// The offered load as a check line prints it: "1.5k/s" (requests), "1.5k units/s" (a database,
// whose writes cost 5), or "64.86 MB/s (324.3/s)" (a byte-sized node: its megabytes, then the
// requests `arriving` that carry them).
[[nodiscard]] std::string offered_text(const graph::ResolvedNode& node, double offered,
                                       const OpLoad& arriving) {
  if (node.spec.type == model::NodeType::sql_db) return with_si_prefix(offered) + " units/s";
  if (!defaults::is_byte_sized(node.spec)) return fmt_rate(offered);
  const double requests = std::ranges::fold_left(arriving | std::views::values, 0.0, std::plus<>{});
  return fmt_units(node.spec, offered) + " (" + fmt_rate(requests) + ")";
}

// A kv_cluster's head (spec §19 entry 9; book ch. 6): every request goes to N replicas, so its
// paper load is N times the requests (defaults::op_cost) and its capacity `nodes` times one
// machine's (graph::apply_kv_ring), and the line says both: "peak offered 90k/s (30k/s × N 3) vs
// 80k/s (8 nodes)  ", the replica-requests with the requests that make them, the machines' total
// with how many there are.
[[nodiscard]] std::string kv_check_head(const graph::ResolvedNode& node, double offered,
                                        const OpLoad& arriving) {
  const double requests = std::ranges::fold_left(arriving | std::views::values, 0.0, std::plus<>{});
  return "peak offered " + fmt_rate(offered) + " (" + fmt_rate(requests) + " × N " +
         std::to_string(model::kv_replication(node.spec)) + ") vs " +
         fmt_rate(total_capacity(node)) + " (" + std::to_string(*node.spec.kv_nodes) + " nodes)  ";
}

// "peak offered 1.5k/s vs 2k/s total capacity  ": what every check line opens with (a
// kv_cluster's says what its numbers are made of: kv_check_head).
[[nodiscard]] std::string check_head(const graph::ResolvedNode& node, double offered,
                                     const OpLoad& arriving) {
  if (node.spec.type == model::NodeType::kv_cluster) return kv_check_head(node, offered, arriving);
  return "peak offered " + offered_text(node, offered, arriving) + " vs " +
         fmt_units(node.spec, total_capacity(node)) + " total capacity  ";
}

// A kv_cluster's verdict: behind its ring (N < nodes, imbalance k > 1) the average hides the
// machine that sheds first, so the verdict is the hottest machine's, k times the average:
// "(hottest 120%, red)" for 90k replica-requests/s over 8 machines of 10k/s (112.5 % on average,
// k = 1.0676 at 150 vnodes: graph::ring_imbalance); with N = nodes every machine holds every key
// and carries the average, "(10%, green)". The engine runs the same hot machine (its node rho is
// the hottest's).
[[nodiscard]] std::string kv_verdict(const graph::ResolvedNode& node, double offered) {
  const double capacity = total_capacity(node);
  if (capacity <= 0.0) return "(no capacity, red)";
  const double rho = offered * node.imbalance / capacity;
  const std::string hottest = node.imbalance > 1.0 ? "hottest " : "";
  return "(" + hottest + percent(rho) + ", " +
         std::string{zone_name(fluid::zone_of(rho, node.capacity))} + ")";
}

[[nodiscard]] std::string check_details(const graph::ResolvedNode& node, double offered,
                                        const OpLoad& arriving,
                                        const std::optional<graph::DatabaseCopyLoads>& loads) {
  const double capacity = total_capacity(node);
  const std::string head = check_head(node, offered, arriving);
  if (node.spec.type == model::NodeType::kv_cluster) return head + kv_verdict(node, offered);
  if (capacity <= 0.0) return head + "(no capacity, red)" + hottest_copy_check(node, offered);
  const double rho = offered / capacity;
  return head + "(" + percent(rho) + ", " + headline_zone(node, rho, loads) + ")" +
         hottest_copy_check(node, offered);
}

// The check of a node whose paper load misses what comes through `blocked` (blocked_on_the_way):
//  - nothing reached it: the head and the mark, no percentage and no zone,
//    "(load not propagated: frontier is not modelled yet)";
//  - some load reached it: that load's percentage as a lower bound, "(≥0.05%; load not
//    propagated: ...)". No zone below red: the real load is higher, so green or yellow might not
//    hold, but a red lower bound is red for certain, "(≥150%, red; ...)" (a database's at the
//    busiest copy, headline_zone);
// No per-copy checks: each would be a verdict on the same partial number.
[[nodiscard]] std::string partial_check(const graph::ResolvedNode& node, double offered,
                                        const OpLoad& arriving,
                                        std::span<const model::NodeType> blocked) {
  const std::string head = check_head(node, offered, arriving);
  const std::string mark = unpropagated_mark(blocked);
  const bool reached = offered > 0.0 || !std::isfinite(offered);
  if (!reached) return head + "(" + mark;
  const double capacity = total_capacity(node);
  if (capacity <= 0.0) return head + "(no capacity, red; " + mark;
  const double rho = offered / capacity;
  const std::string zone = headline_zone(node, rho, database_loads(node, arriving));
  // C++20 std::string::starts_with: a prefix test in one call, replacing
  // `compare(0, prefix.size(), prefix) == 0` or `rfind(prefix, 0) == 0`.
  const std::string red = zone.starts_with("red") ? ", " + zone : "";
  return head + "(≥" + percent(rho) + red + "; " + mark;
}

// A node a loop of gain >= 1 feeds (PaperLoad::unbounded): its load has no figure, and no
// capacity holds it. The footer of the checks names the loop.
[[nodiscard]] std::string unbounded_check(const graph::ResolvedNode& node) {
  return "peak offered unbounded vs " + fmt_units(node.spec, total_capacity(node)) +
         " total capacity  (no steady state, red)";
}

// A node only `on: failure` steps reach on paper: its paper load is 0 (nothing fails there, see
// forward_steps), yet it carries whatever the steps it reports on fail, which only `run` and
// `estimate` measure. A bare "(0%, green)" would read as an idle node, so its line says so.
constexpr std::string_view failure_fed_note =
    " (fed by on: failure steps only: sized by run/estimate)";

// Is `target` sent an `on: failure` step (graph::ServiceHop::reports_on) by any node's op?
[[nodiscard]] bool is_failure_step_target(const graph::Graph& g, std::size_t target) {
  for (std::size_t node = 0; node < g.nodes().size(); ++node) {
    for (const auto& op : g.nodes()[node].spec.ops | std::views::keys) {
      for (const graph::ServiceHop& hop : g.service_hops(node, op)) {
        if (hop.reports_on && hop.target == target) return true;
      }
    }
  }
  return false;
}

// What node `i`'s check line ends with: the unknown hit ratio's upper bound, and the failure-fed
// note for a node the paper load sends nothing that failure steps reach.
[[nodiscard]] std::string check_notes(const graph::Graph& g, std::size_t i, const PaperLoad& load,
                                      const std::vector<bool>& upper_bound) {
  std::string note{upper_bound[i] ? unknown_hit_ratio_note : std::string_view{}};
  if (load.totals[i] == 0.0 && is_failure_step_target(g, i)) note += failure_fed_note;
  return note;
}

// One node's check line: the head and its verdict, or, for a node part or all of whose load the
// paper load could not follow (blocked_on_the_way), partial_check's lower bound or mark.
[[nodiscard]] std::string check_line(const graph::Graph& g, std::size_t i, const PaperLoad& load,
                                     const std::vector<bool>& upper_bound,
                                     std::span<const model::NodeType> blocked) {
  const auto& node = g.nodes()[i];
  if (!load.converged && load.unbounded[i]) return unbounded_check(node);
  const std::string note = check_notes(g, i, load, upper_bound);
  if (!blocked.empty()) return partial_check(node, load.totals[i], load.by_op[i], blocked) + note;
  const auto loads = database_loads(node, load.by_op[i]);
  return check_details(node, load.totals[i], load.by_op[i], loads) + database_check(node, loads) +
         note;
}

// ---- a bandwidth link's pipe on paper ----

// What a client sends over link `link` at its peak, per op in requests: its routes over it.
[[nodiscard]] OpLoad client_link_load(const graph::Graph& g, std::size_t client, std::size_t link) {
  OpLoad sent;
  const auto& workload = g.nodes()[client].spec.workload;
  if (!workload) return sent;
  const double peak = peak_rate(workload->curve, g.scenario().duration);
  for (const auto& route : g.client_routes(client)) {
    if (route.link != link) continue;
    if (const auto rate = peak_on_route(*workload, peak, route)) sent[route.op] += *rate;
  }
  return sent;
}

// What any other node sends over link `link` at the paper load, per op in requests: its arrivals
// (`load.by_op`) forwarded once more, alone (forward_steps), and what lands at the link's
// receiver, whether it travels this pass or, over a feedback link, the next. One link per pair and
// direction, so everything the sender sends the receiver crosses this link.
[[nodiscard]] OpLoad forwarded_link_load(const graph::Graph& g, const ForwardRoutes& table,
                                         const PaperLoad& load, std::size_t link) {
  const auto& resolved = g.links()[link];
  const auto sender = graph::demand_sender(resolved);
  const auto receiver = graph::demand_receiver(resolved);
  std::vector<OpLoad> sent_now(g.nodes().size());
  std::vector<OpLoad> sent_later(g.nodes().size());
  sent_now[sender] = load.by_op[sender];
  forward_steps(g, table, sender, sent_now, sent_later);
  OpLoad sent = sent_later[receiver];
  for (const auto& [op, rate] : sent_now[receiver]) sent[op] += rate;
  if (receiver == sender) {  // a self-loop: what the sender held is no part of what it sent
    for (const auto& [op, rate] : load.by_op[sender]) sent[op] -= rate;
  }
  return sent;
}

// The megabytes per second a link's requests carry: each op's rate × its size at the receiver
// (Graph::request_bytes; Graph::build refused an unsized op on a bandwidth link).
[[nodiscard]] double link_megabytes(const graph::Graph& g, std::size_t receiver,
                                    const OpLoad& sent) {
  double megabytes = 0.0;
  for (const auto& [op, rate] : sent) {
    const auto size = g.request_bytes(receiver, op);
    if (size) megabytes += rate * defaults::byte_cost(*size);
  }
  return megabytes;
}

// A bandwidth link's check: the megabytes its sender puts on it at the paper load against the
// pipe's bandwidth in MB/s, "peak offered 100 MB/s vs 125 MB/s  (80%, yellow)", with the generic
// zones of a pipe (defaults::pipe_capacity). When the sender's own load is incomplete
// (blocked_on_the_way, or the sender is a type the paper load does not follow) it is marked as a
// node's is (partial_check): no percentage when nothing reached the link, a lower bound otherwise.
// When the sender's load rests on an unknown hit ratio (it is behind such a cache, or it is one,
// whose misses are what it sends on: counts_unknown_hit_ratio, unknown_misses) the line carries the
// node lines' upper-bound note: every read counted as a miss. When a loop of gain >= 1 feeds the
// sender (PaperLoad::unbounded), what it puts on the link has no figure either, and the line says
// so as a node's does (unbounded_check; a link behind a gain-2 loop printed "8.988e+307 MB/s").
[[nodiscard]] std::string link_check(const graph::Graph& g, const ForwardRoutes& table,
                                     const PaperLoad& load, std::size_t link,
                                     std::span<const model::NodeType> blocked_at_sender,
                                     bool upper_bound) {
  const auto& resolved = g.links()[link];
  const auto sender = graph::demand_sender(resolved);
  if (!load.converged && load.unbounded[sender]) {
    return "peak offered unbounded vs " + with_si_prefix(resolved.pipe->service_rate.per_second) +
           " MB/s  (no steady state, red)";
  }
  const bool from_client = g.nodes()[sender].spec.type == model::NodeType::client;
  const auto sent =
      from_client ? client_link_load(g, sender, link) : forwarded_link_load(g, table, load, link);
  const double offered = link_megabytes(g, graph::demand_receiver(resolved), sent);
  const double capacity = resolved.pipe->service_rate.per_second;
  const double rho = offered / capacity;
  const std::string zone{zone_name(fluid::zone_of(rho, *resolved.pipe))};
  const std::string head = "peak offered " + with_si_prefix(offered) + " MB/s vs " +
                           with_si_prefix(capacity) + " MB/s  ";
  std::vector<model::NodeType> blocked{blocked_at_sender.begin(), blocked_at_sender.end()};
  if (unfollowed_until(g.nodes()[sender].spec.type)) {
    add_missing(blocked, std::array{g.nodes()[sender].spec.type});
  }
  const std::string note{upper_bound ? unknown_hit_ratio_note : std::string_view{}};
  if (blocked.empty()) return head + "(" + percent(rho) + ", " + zone + ")" + note;
  if (!(offered > 0.0)) return head + "(" + unpropagated_mark(blocked) + note;
  const std::string red = zone == "red" ? ", red" : "";
  return head + "(≥" + percent(rho) + red + "; " + unpropagated_mark(blocked) + note;
}

[[nodiscard]] std::string explain_checks(const graph::Graph& g) {
  const auto load = paper_load(g);
  const auto upper_bound = counts_unknown_hit_ratio(g);
  const auto blocked = blocked_on_the_way(g);
  std::vector<Row> rows;
  for (std::size_t i = 0; i < g.nodes().size(); ++i) {
    const auto& node = g.nodes()[i];
    if (node.spec.type == model::NodeType::client) continue;  // a source: see its offered=
    rows.push_back({{node.spec.id}, {check_line(g, i, load, upper_bound, blocked[i])}});
  }
  const auto table = forward_routes(g);
  for (std::size_t link = 0; link < g.links().size(); ++link) {
    const auto& resolved = g.links()[link];
    if (!resolved.pipe) continue;
    const auto sender = graph::demand_sender(resolved);
    const bool upper = upper_bound[sender] || unknown_misses(g, sender);
    rows.push_back({{graph::link_label(g.nodes(), resolved)},
                    {link_check(g, table, load, link, blocked[sender], upper)}});
  }
  const std::string note = load.converged ? "" : "  (no steady state: a loop of gain >= 1)\n";
  const std::string header = "checks (peak offered load over the first " +
                             format_duration(g.scenario().duration) + ", no queueing):\n";
  return header + aligned(rows, Align::all_left) + note;
}

}  // namespace

std::string render_explain(const graph::Graph& g) {
  return explain_header(g.scenario()) + explain_faults(g.scenario()) + explain_nodes(g) +
         explain_links(g) + explain_order(g) + explain_checks(g);
}

}  // namespace simsys::report
