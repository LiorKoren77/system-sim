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
import simsys.fluid; // Zone: a link's worst zone
import simsys.graph;
import simsys.model;

export namespace simsys::report {

// One node over the observed part of a run (after warmup). Rates are summed over
// replicas, utilization is per copy (see engine::NodeSample).
struct NodeSummary {
  std::string id{};
  double mean_rho{};      // time-averaged utilization (busy fraction)
  double peak_rho{};      // highest utilization of any observed step
  double peak_backlog{};  // most requests waiting at the end of any observed step
  // Requests shed because the waiting room was full, and an external sink's failed requests (its
  // `failure_rate` of what it served, plan 2c refinement 8): failures.
  double total_dropped{};
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
  // A message queue's peak lag over the observed steps, in seconds (engine::NodeSample::
  // queue_lag_s: its slowest consumer group's wait, max(k × Wq, backlog / capacity)): the PEAK,
  // not the mean, since a lag bound is a promise about the worst moment (a notification an hour
  // late is late whatever the day's average was). What the `queue_lag` SLO observes (plan 2c
  // refinement 14). Empty for every node that is not a message queue.
  std::optional<double> peak_queue_lag_s{};
};

// One bandwidth link's pipe over the observed part of a run (plan 2c Task 5;
// engine::LinkSample), in MEGABYTES (per second for the rates). `name` is the link as written,
// `from->to` (graph::link_label), how every report names its row, also for a `dir: in` link,
// whose demand travels from `to` to `from`. The node table's columns (`queue`: its id is the name,
// and a pipe never rejects nor goes unavailable) and, for JSON, the time-averaged rates and
// backlog of the pipe, its mean latency (a request's wait plus its answer time on the wire, a
// write's whole serialisation and any other op's first megabyte, weighted by the megabytes offered
// in each step; by time when none were) and the worst zone of any observed step.
struct LinkSummary {
  NodeSummary queue{};
  double demand_per_s{};
  double drain_per_s{};
  double dropped_per_s{};
  double backlog{};
  double latency_s{};
  fluid::Zone zone{fluid::Zone::green};
};

// One (client, op) over the observed part of a run. Percentiles are a lognormal fit to
// the latency moments averaged over the ANSWERED requests (plan 2a refinement 8: a refused
// or dropped request saw no latency); `offered_per_s` still counts every request sent, and
// `answered_per_s` the part of them that came back answered (plan 2b refinement 7): offered
// minus what was refused or dropped on the way (a request still waiting in a queue is answered
// later, not lost), so answered / offered is the fraction the path delivered. A client with a
// `timeout` (plan 2c Task 13) also gives up on the delivered requests slower than it:
// `timed_out_per_s` counts them (0 without a timeout), `answered_per_s` leaves them out (offered
// − lost − timed out), and the mean describes the requests answered in time. A percentile is what
// the client sees at its quantile q (report.cpp's percentile_seen_by_client, review of plan 2c
// Task 13, C-1): when more than 1 − q of the delivered requests timed out
// (`timed_out_share` > 1 − q: more than 1 % for a p99), no request at that quantile was answered,
// and the percentile IS the timeout (the table prints `>1s`, a latency SLO on it
// fails whatever its max); otherwise it is the answered requests' fit read at q / (1 − s), s the
// timed-out share (the client's fastest q are the answered requests' fastest q / (1 − s): the
// timed-out ones are all slower), capped at the timeout (no answered request took longer). A
// timed-out request is not a drop: no node failed it, so it is in no node's drop rate. An infinite
// mean makes every latency infinite, which the renderers print as "unavailable" (table) or null
// (JSON); an op of which nothing was answered in time but something timed out has no latency of its
// own either (the JSON's null).
struct OpSummary {
  std::string client{};
  std::string op{};
  double offered_per_s{};
  double answered_per_s{};
  double timed_out_per_s{};
  double mean_s{};
  double p50_s{};
  double p95_s{};
  double p99_s{};
  // The client's timeout in seconds (graph::client_timeout_s), empty when it waits for ever, and
  // timed_out / (answered + timed_out) over the observed steps: 0 without a timeout or with
  // nothing delivered.
  std::optional<double> timeout_s{};
  double timed_out_share{};
};

enum class SloStatus { pass, fail, skipped };

// An SLO and its verdict. `observed` is in seconds for the time SLOs (latency, staleness, queue
// lag) and a plain ratio otherwise; it is empty when the run has nothing to measure it on (a
// queue lag without a message queue, a staleness on a node that is no database) or observed no
// step: skipped.
// `note` says why a verdict is not what `observed` against `max` alone would give: a staleness SLO
// fails on a database whose replicas fall behind without bound ("each replica misses 66.67/s of
// 400/s committed writes (16.7%)"), whatever its mean staleness; a latency SLO does not read an op
// whose requests were all lost ("nobody answered c.op: no latency to observe"), and is skipped when
// it reads only such ops. Empty otherwise.
// An SLO with `over` (plan 2c Task 14) is judged by its worst window (Summary::windowed_verdict):
// `observed`, `status` and `note` are that window's, `window_start` says where it starts,
// counted from the start of the run (warmup included, the clock a scenario's `at:` uses), and
// `window_length` how long it is: `over`, or less for the run's last window when the observed run
// is not a whole number of windows. Both are empty for an SLO without `over`, for a windowed
// latency SLO in none of whose windows its op was offered (then `skipped`), and under `estimate`.
// `estimate` (Summary::of_estimate) has one steady-state record, at t = 0, and no windows: it
// judges an SLO with `over` on that record and sets `at_one_instant`, so the reports say that run
// is what checks each window (`(at 00:00 only: run checks each 1h)`).
struct SloResult {
  model::SloSpec slo{};
  std::optional<double> observed{};
  SloStatus status{};
  std::string note{};
  std::optional<SimTime> window_start{};
  std::optional<SimTime> window_length{};
  bool at_one_instant{};
};

// Running aggregates over the StepRecords of a run, from `warmup` on (earlier records only
// fix the step length). Keeps a handful of sums per node and per op, never the records.
// Pattern: Observer — pass `[&](const auto& r) { summary.observe(r); }` to
// engine::Simulation::run. The Summary refers to `g`, which must outlive it.
// SLOs per window (plan 2c Task 14): for every SLO of the scenario with `over`, the Summary also
// keeps one more Summary of the same kind, fed only the steps of the window being accumulated
// (windows of length `over`, aligned from the end of warmup: [warmup + k·over, warmup +
// (k + 1)·over); a step belongs to the window its start falls in). When a step opens the next
// window the finished one is judged like a whole run and only its verdict is kept if it is the
// worst so far; so a window's figures come from exactly the code that makes the run's (its
// percentiles from percentile_seen_by_client, its drop rates from the same totals), and the
// memory is one Summary per windowed SLO, whatever the run's length.
class Summary {
 public:
  Summary(const graph::Graph& g, SimTime warmup);
  // `simsys estimate`'s Summary: the one steady-state record of engine::Simulation::estimate
  // observed as a run of one step from t = 0, which judges an SLO with `over` on that record
  // (SloResult::at_one_instant) instead of pretending it measured a window.
  [[nodiscard]] static Summary of_estimate(const graph::Graph& g, const engine::StepRecord& record);

  void observe(const engine::StepRecord& record);

  [[nodiscard]] std::vector<NodeSummary> nodes() const;  // indexed like Graph::nodes()
  [[nodiscard]] std::vector<OpSummary> ops() const;      // clients in node order, ops by name
  // One per link with a bandwidth, in Graph::links() order (engine::StepRecord::links).
  [[nodiscard]] std::vector<LinkSummary> links() const;
  // Zero means the window after warmup was empty: there is no data, which the SLO
  // verdicts and the renderers report as such (nothing is evaluated, nothing passes).
  [[nodiscard]] std::size_t observed_steps() const { return observed_steps_; }
  // One node's drop rate: the requests it FAILED (shed from a full queue) over the requests
  // sent to it, summed over the observed steps (Σ dropped / Σ demand), so always in [0, 1];
  // 0 when nothing reached it. An unknown id is a programmer error (the loader checks SLO
  // nodes): throws std::invalid_argument.
  [[nodiscard]] double drop_rate(std::string_view node) const;
  // The worst node's drop rate: every node must keep its own, so the worst one decides. A
  // bandwidth link's pipe counts as a node here (plan 2c Task 5): a request its full room drops is
  // lost as surely as one a node drops, so its drop rate (megabytes dropped over megabytes sent)
  // can be the worst. An SLO's `node:` still names nodes only.
  [[nodiscard]] double drop_rate() const;
  // The id of the node with the highest drop rate (a link's `from->to` when its pipe is the worst;
  // the first of equals, nodes before links); empty when nothing dropped anything.
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
  // The verdict of an SLO with `over`: its worst window's, the window still open at the end of the
  // run (a last window shorter than `over`) included. Every window is judged, except that a latency
  // SLO skips a window in which its op was not offered (no request, no latency), and one all of
  // whose windows are skipped is `skipped`. "Worst" is a fail before a
  // pass, then the higher observed value (every SLO is an upper bound). `slo` must be one of the
  // scenario's SLOs with `over` (evaluate_slos asks for exactly those); another is `skipped`.
  [[nodiscard]] SloResult windowed_verdict(const model::SloSpec& slo) const;

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
    std::optional<double> peak_queue_lag_s{};  // the largest queue_lag_s: message queues only
    void add(const engine::NodeSample& sample, double dt_s);
    [[nodiscard]] double drop_rate() const;
    [[nodiscard]] double reject_rate() const;
  };
  // A bandwidth link's pipe: a node's totals (in MB), and what its JSON adds.
  struct LinkTotals {
    std::string name{};
    NodeTotals queue{};
    double drained{};       // Σ drain_per_s·dt
    double backlog_time{};  // Σ backlog·dt
    Moments by_demand{};    // latency weighted by the megabytes offered
    Moments by_time{};      // ... and by time: the fallback when nothing was offered
    fluid::Zone worst_zone{fluid::Zone::green};
    void add(const engine::LinkSample& sample, double dt_s);
  };
  // Every member has an initializer, even `{}`: GCC's -Wmissing-field-initializers flags
  // a designated initializer that omits one without.
  struct OpTotals {
    std::size_t client{};
    std::string op{};
    double offered{};       // Σ offered·dt, requests
    double answered{};      // Σ answered·dt, requests: the part of `offered` that came back
    double timed_out{};     // Σ timed_out·dt, requests: delivered, but after the client gave up
    Moments by_answered{};  // weighted by requests answered: what an average answer sees
    Moments by_time{};      // weighted by time: the fallback when nothing was answered
    bool unavailable{};
    void add(const engine::ResponseSample& sample, double dt_s);
  };

  // An op's mean and percentiles from the moments its answered requests left (Summary::ops).
  static void fill_latencies(const Moments& m, OpSummary& op);
  [[nodiscard]] double step_length_s(SimTime t);
  [[nodiscard]] OpTotals& totals_for(const engine::ResponseSample& sample, std::size_t hint);
  // The index of the node with the highest `rate` (drop or reject), the first of equals.
  [[nodiscard]] std::size_t worst_index(double (NodeTotals::*rate)() const) const;
  [[nodiscard]] double node_rate(std::string_view node, double (NodeTotals::*rate)() const) const;
  // The node with the highest drop rate, the first of equals (0 without nodes), and the same for
  // the links' pipes (null without a bandwidth link).
  [[nodiscard]] double worst_node_drop_rate() const;
  [[nodiscard]] const LinkTotals* worst_dropping_link() const;

  // One SLO with `over` and its windows (see the class comment).
  struct WindowedSlo {
    model::SloSpec slo{};
    long long index{-1};  // the window being accumulated; -1 before the first step
    // That window's Summary: empty before the first step, else one. A vector used as an optional:
    // Summary is still incomplete here, which std::vector accepts (since C++17) and std::optional
    // does not.
    std::vector<Summary> current{};
    std::optional<SloResult> worst{};  // the worst verdict of the windows already finished
  };

  // A window's Summary, or estimate's: like the public constructor's, but tracking no windows of
  // its own.
  Summary(const graph::Graph& g, SimTime warmup, bool track_windows);
  void observe_windows(const engine::StepRecord& record);
  [[nodiscard]] SloResult judge_window(const WindowedSlo& windowed) const;

  // A pointer, not a reference, so a Summary stays copy- and move-assignable.
  const graph::Graph* graph_;
  SimTime warmup_;
  std::optional<SimTime> last_t_;
  std::size_t observed_steps_{};
  double observed_s_{};
  std::vector<NodeTotals> nodes_;
  std::vector<OpTotals> ops_;
  std::vector<LinkTotals> links_;     // one per link with a bandwidth, in Graph::links() order
  std::vector<WindowedSlo> windows_;  // one per scenario SLO with `over`, in the scenario's order
  bool estimate_{};                   // made by of_estimate: SLOs with `over` judged at t = 0
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
// A duration for people, as the tables print one: seconds in the largest unit that keeps the
// number >= 1, three significant digits ("1.61ms", "2.66h"); "unavailable" for infinity.
// `simsys sweep` prints its latencies with it.
[[nodiscard]] std::string format_duration(double seconds);
// A queue lag as the tables print one: format_duration, but "<1us" below a microsecond (an idle
// consumer group's lag is a vanishing Erlang C figure, not a measurement).
[[nodiscard]] std::string format_lag(double seconds);

// `simsys explain`: every resolved number with its provenance, the links, the step order
// and a static load check (offered load vs capacity, no queueing).
[[nodiscard]] std::string render_explain(const graph::Graph& g);

// For plotting: one CSV row per node per step,
// `t_s,node,demand_per_s,drain_per_s,rho,backlog,drops_per_s,rejected_per_s,latency_ms,zone,
// queue_lag_s` (the last empty except on a message queue's rows), then one per link with a
// bandwidth, its `from->to` in the node column, in megabytes.
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
  std::vector<bool> is_client_;          // indexed like ids_
  std::vector<std::string> link_names_;  // the bandwidth links', like engine::StepRecord::links
};

// For plotting what the clients see: one CSV row per (client, op) per step,
// `t_s,client,op,offered_per_s,answered_per_s,timed_out_per_s,mean_ms` (plan 2b refinement 7;
// `timed_out_per_s` since the review of plan 2c Task 13). The header is
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
