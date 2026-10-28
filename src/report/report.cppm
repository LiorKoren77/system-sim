// Module `simsys.report`: turns a run into something a person or a GUI can read.
//
// Models: nothing from the book; this is the measurement layer. It summarises the step
// records of a run (time-averaged moments, percentiles fitted to them and labelled
// "estimated", SLO verdicts as in ch. 12-style availability/latency targets), renders a
// text table, a JSON report and a CSV series, and explains a resolved scenario: every
// number the engine will use, where it came from, and the back-of-the-envelope load
// check (System Design Interview ch. 2) a student would do on paper before running.
//
// C++20 named module, interface unit (see src/core/core.cppm for the longer
// explanation of modules versus headers). Only declarations live here; the renderers and
// their number formatting are in report.cpp.
module;
// NO <chrono>/<format>/<print>/<filesystem> here: this interface imports other modules
// (GCC 14.2 'Bad file data' bug, CLAUDE.md §3). Heavy headers go in report.cpp.
#include <cstddef>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module simsys.report;

// C++20 `import`: brings a named module's exported names into this unit without
// re-parsing any text (the compiler reads the imported module's compiled interface).
import simsys.core;
import simsys.engine;
import simsys.graph;
import simsys.model;

export namespace simsys::report {

// One node over the observed part of a run (after warmup). Rates are summed over
// replicas, utilization is per copy (see engine::NodeSample).
struct NodeSummary {
  std::string id{};
  double mean_rho{};       // time-averaged utilization (busy fraction)
  double peak_rho{};       // highest utilization of any observed step
  double peak_backlog{};   // most requests waiting at the end of any observed step
  double total_dropped{};  // requests shed because the waiting room was full: failures
  // Requests refused by policy (a rate limiter's 429s, plan 2a refinement 6): answered at
  // once, never forwarded, and counted apart from the drops (reject_rate, not drop_rate).
  double total_rejected{};
  double red_time_s{};  // seconds spent in the red zone
  bool unavailable{};   // the node's latency was infinite in some observed step
  // A SQL database's staleness (engine::NodeSample::staleness_s: the write's sojourn at the
  // primary + the replication lag + its sojourn at a replica), time-averaged over the observed
  // steps; infinite when a queue never answered in one of them; empty for every node that keeps
  // no lagging copies. The bounded-queue figure: finite while a replica misses writes, whose
  // real lag grows without bound (below).
  std::optional<double> mean_staleness_s{};
  // The committed writes ONE replica failed to replay, per second (writes, not capacity units;
  // engine::NodeSample::replication_dropped_per_s), time-averaged over the observed steps: any
  // at all means a replica falls behind without bound (the model drops the excess at
  // queue_limit; a real replica keeps it in its relay log/WAL until it is rebuilt). Empty for
  // every node that is not a database.
  std::optional<double> mean_replication_dropped_per_s{};
  // The writes ONE shard's primary committed per second, which each of its replicas must replay
  // (engine::NodeSample::committed_writes_per_s), time-averaged the same way: what the missed
  // writes above are a share of. Empty for every node that is not a database.
  std::optional<double> mean_committed_writes_per_s{};
};

// One (client, op) over the observed part of a run. Percentiles are a lognormal fit to
// the latency moments averaged over the ANSWERED requests (plan 2a refinement 8: a refused
// or dropped request saw no latency); `offered_per_s` still counts every request sent, and
// `answered_per_s` the part of them that came back answered (plan 2b refinement 7): offered
// minus what was refused or dropped on the way (a request still waiting in a queue is answered
// later, not lost), so answered / offered is the fraction the path delivered. An infinite mean
// makes every latency
// infinite, which the renderers print as "unavailable" (table) or null (JSON).
struct OpSummary {
  std::string client{};
  std::string op{};
  double offered_per_s{};
  double answered_per_s{};
  double mean_s{};
  double p50_s{};
  double p95_s{};
  double p99_s{};
};

enum class SloStatus { pass, fail, skipped };

// An SLO and its verdict. `observed` is in seconds for latency SLOs and a plain ratio
// otherwise; it is empty when the metric is not modelled yet (skipped). `note` says why a
// verdict is not what `observed` against `max` alone would give: a staleness SLO fails on a
// database whose replicas fall behind without bound ("each replica misses 66.67/s of 400/s
// committed writes (16.7%)"), whatever its mean staleness. Empty otherwise.
struct SloResult {
  model::SloSpec slo{};
  std::optional<double> observed{};
  SloStatus status{};
  std::string note{};
};

// Running aggregates over the StepRecords of a run, from `warmup` on (earlier records only
// fix the step length). Keeps a handful of sums per node and per op, never the records.
// Pattern: Observer — pass `[&](const auto& r) { summary.observe(r); }` to
// engine::Simulation::run. The Summary refers to `g`, which must outlive it.
class Summary {
 public:
  Summary(const graph::Graph& g, SimTime warmup);

  void observe(const engine::StepRecord& record);

  [[nodiscard]] std::vector<NodeSummary> nodes() const;  // indexed like Graph::nodes()
  [[nodiscard]] std::vector<OpSummary> ops() const;      // clients in node order, ops by name
  // Zero means the window after warmup was empty: there is no data, which the SLO
  // verdicts and the renderers report as such (nothing is evaluated, nothing passes).
  [[nodiscard]] std::size_t observed_steps() const { return observed_steps_; }
  // One node's drop rate: the requests it FAILED (shed from a full queue) over the requests
  // sent to it, summed over the observed steps (Σ dropped / Σ demand), so always in [0, 1];
  // 0 when nothing reached it. An unknown id is a programmer error (the loader checks SLO
  // nodes): throws std::invalid_argument.
  [[nodiscard]] double drop_rate(std::string_view node) const;
  // The worst node's drop rate: every node must keep its own, so the worst one decides.
  [[nodiscard]] double drop_rate() const;
  // The id of the node with the highest drop rate; empty when no node dropped anything.
  [[nodiscard]] std::string_view worst_drop_node() const;
  // The same three for REJECTIONS, a rate limiter's refusals by policy (Σ rejected / Σ
  // demand; a leaky bucket's overflow included): a limiter that sheds what its limit says
  // is working, not failing, so its refusals are kept out of the drop rate (D5 of the
  // rate-limiter review) and get an SLO metric of their own.
  [[nodiscard]] double reject_rate(std::string_view node) const;
  [[nodiscard]] double reject_rate() const;
  [[nodiscard]] std::string_view worst_reject_node() const;
  [[nodiscard]] std::string_view scenario_name() const;
  [[nodiscard]] const graph::Graph& graph() const { return *graph_; }

 private:
  // Weighted first and second moments of a latency, accumulated across steps.
  struct Moments {
    double weight{};
    double weighted_mean{};    // Σ w·m
    double weighted_square{};  // Σ w·(v + m²) = Σ w·E[X²]
    void add(double w, double mean_s, double variance_s2);
  };
  struct NodeTotals {
    double rho_time{};  // Σ rho·dt
    double peak_rho{};
    double peak_backlog{};
    double demand{};    // Σ demand_per_s·dt: requests sent to the node
    double dropped{};   // Σ dropped_per_s·dt
    double rejected{};  // Σ rejected_per_s·dt
    double red_time_s{};
    bool unavailable{};
    double staleness_time{};       // Σ staleness_s·dt over the steps that report one
    double staleness_seen_s{};     // Σ dt over those steps: 0 for a node with no staleness
    double replication_dropped{};  // Σ replication_dropped_per_s·dt (writes, per replica)
    double committed_writes{};     // Σ committed_writes_per_s·dt (writes, per shard)
    double replication_seen_s{};   // Σ dt over the steps that report the two above
    void add(const engine::NodeSample& sample, double dt_s);
    [[nodiscard]] double drop_rate() const;
    [[nodiscard]] double reject_rate() const;
  };
  // Every member has an initializer, even `{}`: GCC's -Wmissing-field-initializers flags
  // a designated initializer that omits one without.
  struct OpTotals {
    std::size_t client{};
    std::string op{};
    double offered{};       // Σ offered·dt, requests
    double answered{};      // Σ answered·dt, requests: the part of `offered` that came back
    Moments by_answered{};  // weighted by requests answered: what an average answer sees
    Moments by_time{};      // weighted by time: the fallback when nothing was answered
    bool unavailable{};
    void add(const engine::ResponseSample& sample, double dt_s);
  };

  [[nodiscard]] double step_length_s(SimTime t);
  [[nodiscard]] OpTotals& totals_for(const engine::ResponseSample& sample, std::size_t hint);
  // The index of the node with the highest `rate` (drop or reject), the first of equals.
  [[nodiscard]] std::size_t worst_index(double (NodeTotals::*rate)() const) const;
  [[nodiscard]] double node_rate(std::string_view node, double (NodeTotals::*rate)() const) const;

  // A pointer, not a reference, so a Summary stays copy- and move-assignable.
  const graph::Graph* graph_;
  SimTime warmup_;
  std::optional<SimTime> last_t_;
  std::size_t observed_steps_{};
  double observed_s_{};
  std::vector<NodeTotals> nodes_;
  std::vector<OpTotals> ops_;
};

// C++20 std::span<const T>: a non-owning view of contiguous elements, so callers can pass
// a vector, an array or `{}` (no SLOs) without a copy and without a template.
[[nodiscard]] std::vector<SloResult> evaluate_slos(const Summary& summary,
                                                   std::span<const model::SloSpec> slos);
[[nodiscard]] bool all_pass(std::span<const SloResult> results);  // skipped counts as pass

// For people: a header line, a node table (rho cells coloured by zone when `color`), an
// op table with estimated percentiles, and the SLO verdicts.
[[nodiscard]] std::string render_table(const Summary& summary, std::span<const SloResult> slos,
                                       bool color);
// For programs: the same content as JSON (two-space indent, seconds, null for infinity).
[[nodiscard]] std::string render_json(const Summary& summary, std::span<const SloResult> slos);
// A number for machines (CSV): the shortest text that reads back to the same double (0.1,
// 2, 4000), locale-independent (std::to_chars). `simsys curve` prints its columns with it.
[[nodiscard]] std::string format_number(double value);

// `simsys explain`: every resolved number with its provenance, the links, the step order
// and a static load check (offered load vs capacity, no queueing).
[[nodiscard]] std::string render_explain(const graph::Graph& g);

// For plotting: one CSV row per node per step,
// `t_s,node,demand_per_s,drain_per_s,rho,backlog,drops_per_s,rejected_per_s,latency_ms,zone`.
// The header is written on construction; the column rules are at SeriesWriter::observe in
// report.cpp.
// Pattern: Observer, like Summary. `out` and `g` must outlive the writer.
class SeriesWriter {
 public:
  SeriesWriter(std::ostream& out, const graph::Graph& g);
  void observe(const engine::StepRecord& record);

 private:
  std::ostream* out_;
  std::vector<std::string> ids_;
  std::vector<bool> is_client_;  // indexed like ids_
};

// For plotting what the clients see: one CSV row per (client, op) per step,
// `t_s,client,op,offered_per_s,answered_per_s,mean_ms` (plan 2b refinement 7). The header is
// written on construction; the column rules are at OpSeriesWriter::observe in report.cpp.
// Pattern: Observer, like SeriesWriter: the CLI hands each step record to both, and neither
// knows of the other. `out` and `g` must outlive the writer.
class OpSeriesWriter {
 public:
  OpSeriesWriter(std::ostream& out, const graph::Graph& g);
  void observe(const engine::StepRecord& record);

 private:
  std::ostream* out_;
  std::vector<std::string> ids_;  // node ids, indexed like Graph::nodes(): a sample's client
};

}  // namespace simsys::report
