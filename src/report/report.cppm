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
// explanation of modules versus headers). Only declarations live here; the renderers are in
// report.cpp and explain.cpp, the number formatting they share in the partition shared.cppm.
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
  double mean_rho{};  // time-averaged utilization (busy fraction)
  double peak_rho{};  // highest utilization of any observed step
  // Most requests waiting at the end of any observed step. This and the two totals below are
  // reported as 0 when they are float residue, under 1e-9 of the queue's capacity per second.
  double peak_backlog{};
  // Requests shed because the waiting room was full, and an external sink's failed requests (its
  // `failure_rate` of what it served, spec §19 entry 8): failures. A sink's failed calls stay here
  // when a pool retries them (its books, engine::NodeSample); only its drop rate leaves them out
  // (Summary::drop_rate).
  double total_dropped{};
  // Requests refused by policy (a rate limiter's 429s, spec §17 entry 6): answered at once, never
  // forwarded, and counted apart from the drops (reject_rate, not drop_rate).
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
  // queue_lag_s: its slowest consumer group's wait, max(k × Wq, backlog / capacity)): the PEAK, not
  // the mean, since a lag bound is a promise about the worst moment (a notification an hour late is
  // late whatever the day's average was). What the `queue_lag` SLO observes (spec §19 entry 14).
  // Empty for every node that is not a message queue.
  std::optional<double> peak_queue_lag_s{};
  // A worker pool's dead letters (engine::NodeSample::dead_lettered_per_s: the jobs whose last
  // retry at an external sink failed, phase 3 spec §6.2): their total over the observed steps
  // (Σ rate·dt) and that total over the observed time, a mean rate. 0 on a pool without retries,
  // empty on every other node. Never in total_dropped: a dead letter is parked, not lost.
  std::optional<double> total_dead_lettered{};
  std::optional<double> mean_dead_lettered_per_s{};
  // A ws_gateway's open connections (engine::NodeSample::connections, a LEVEL, phase 3 spec §8.2):
  // `connections` the level of the last observed step, where the run ended, and `connections_peak`
  // the largest of any observed step (a gateway at its cap, or a level still rising, tells the two
  // apart). Empty for every node that is not a gateway.
  std::optional<double> connections{};
  std::optional<double> connections_peak{};
};

// One bandwidth link's pipe over the observed part of a run (engine::LinkSample), in MEGABYTES (per
// second for the rates). `name` is the link as written, `from->to` (graph::link_label), how every
// report names its row, also for a `dir: in` link, whose demand travels from `to` to `from`. The
// node table's columns (`queue`: its id is the name, and a pipe never rejects nor goes unavailable)
// and, for JSON, the time-averaged rates and backlog of the pipe, its mean latency (a request's
// wait plus its answer time on the wire, a write's whole serialisation and any other op's first
// megabyte, weighted by the megabytes offered in each step; by time when none were) and the worst
// zone of any observed step.
struct LinkSummary {
  NodeSummary queue{};
  double demand_per_s{};
  double drain_per_s{};
  double dropped_per_s{};
  double backlog{};
  double latency_s{};
  fluid::Zone zone{fluid::Zone::green};
};

// One (client, op) over the observed part of a run. Percentiles are a lognormal fit to the latency
// moments averaged over the ANSWERED requests (spec §17 entry 8: a refused or dropped request saw
// no latency); `offered_per_s` still counts every request sent, and `answered_per_s` the part of
// them that came back answered (spec §18 entry 13): offered minus what was refused or dropped on
// the way (a request still waiting in a queue is answered later, not lost), so answered / offered
// is the fraction the path delivered. A client with a `timeout` also gives up on the delivered
// requests slower than it: `timed_out_per_s` counts them (0 without a timeout), `answered_per_s`
// leaves them out (offered − lost − timed out), and the mean describes the requests answered in
// time. `attempts_per_s` is what the client SENT, its offered load plus its retries (a client with
// `retries`, behaviours::ClientBehaviour): equal to `offered_per_s` without retries or with nothing
// to retry, above it by the retry storm when its path fails. With retries `timed_out_per_s` counts
// the requests given up on for good (their last attempt timed out) and `timed_out_attempts_per_s`
// every attempt that timed out (engine::ResponseSample::timed_out_attempts); without retries they
// are equal. A retried request's latency is its last attempt's: the time its failed attempts took
// (the timeout τ of each one that timed out, about nothing for a lost one) and the backoffs before
// each retry are not added (ignored; a proper model would read the response as a mixture over the
// attempt that is answered, attempt k + 1 adding k × (τ or 0) + k × backoff: phase 4 material). A
// percentile is what the client sees at its quantile q (report.cpp's percentile_seen_by_client):
// when more than 1 − q of the delivered attempts timed out (`timed_out_share` > 1 − q: more than
// 1 % for a p99), no request at that quantile was answered, and the percentile IS the timeout (the
// table prints `>1s`, a latency SLO on it fails whatever its max); otherwise it is the fit to every
// DELIVERED request's latency, before the timeout cut it, read at q and capped at the timeout (the
// engine times out exactly the requests that fit puts past the timeout, so its fastest q are all
// answered). A timed-out request is not a drop: no node failed it, so it is in no node's drop rate.
// An infinite mean makes every latency infinite, which the renderers print as "unavailable" (table)
// or null (JSON); an op of which nothing was answered in time but something timed out has no
// latency of its own either (the JSON's null).
struct OpSummary {
  std::string client{};
  std::string op{};
  double offered_per_s{};
  double attempts_per_s{};
  double answered_per_s{};
  double timed_out_per_s{};
  double timed_out_attempts_per_s{};
  double mean_s{};
  double p50_s{};
  double p95_s{};
  double p99_s{};
  // The client's timeout in seconds (graph::client_timeout_s), empty when it waits for ever, and
  // timed_out_attempts / (answered + timed_out_attempts) over the observed steps, the share of the
  // delivered attempts that came back after the timeout: 0 without a timeout or with nothing
  // delivered.
  std::optional<double> timeout_s{};
  double timed_out_share{};
};

// The op table's latency columns: OpSummary's mean_s, p50_s, p95_s and p99_s.
enum class OpLatency { mean, p50, p95, p99 };

// The number in that column, in seconds.
[[nodiscard]] double op_latency_s(const OpSummary& op, OpLatency column);

// Whether that column is the client's timeout rather than a latency it saw: a percentile at q of
// an op more than 1 − q of whose delivered requests timed out (the op table's `>1s`). Never the
// mean, which describes the requests answered in time.
[[nodiscard]] bool latency_beyond_timeout(const OpSummary& op, OpLatency column);

enum class SloStatus { pass, fail, skipped };

// An SLO and its verdict. `observed` is in seconds for the time SLOs (latency, staleness, queue
// lag) and a plain ratio otherwise; it is empty when the run has nothing to measure it on (a queue
// lag without a message queue, a staleness on a node that is no database) or observed no step:
// skipped. `note` says why a verdict is not what `observed` against `max` alone would give: a
// staleness SLO fails on a database whose replicas fall behind without bound ("each replica misses
// 66.67/s of 400/s committed writes (16.7%)"), whatever its mean staleness; a latency SLO does not
// read an op whose requests were all lost ("nobody answered c.op: no latency to observe"), and is
// skipped when it reads only such ops. Empty otherwise. An SLO with `over` is judged by its worst
// window (Summary::windowed_verdict): `observed`, `status` and `note` are that window's,
// `window_start` says where it starts, counted from the start of the run (warmup included, the
// clock a scenario's `at:` uses), and `window_length` how long it is: `over`, or less for the run's
// last window when the observed run is not a whole number of windows. Both are empty for an SLO
// without `over`, for a windowed latency SLO in none of whose windows its op was offered (then
// `skipped`), and under `estimate`. `estimate` (Summary::of_estimate) has one steady-state record,
// at t = 0, and no windows: it judges an SLO with `over` on that record and sets `at_one_instant`,
// so the reports say that run is what checks each window (`(at 00:00 only: run checks each 1h)`).
struct SloResult {
  model::SloSpec slo{};
  std::optional<double> observed{};
  SloStatus status{};
  std::string note{};
  std::optional<SimTime> window_start{};
  std::optional<SimTime> window_length{};
  bool at_one_instant{};
};

// What a scripted fault did to one of the scenario's SLOs, judged against the fault-free twin run
// (engine::FaultRecord's header has the twin; Summary::set_faults fills it):
//   over_max_s  the seconds, inside the fault's window "during", of the steps at which this run
//               breaks the SLO and the twin's same step does not, each step judged alone by the
//               same rules as a whole run (evaluate_slos on a Summary of that one step; an SLO with
//               `over` judged on the step as if it had none). A step the twin breaks too is the
//               SLO's own trouble, not the fault's. A step with nothing to observe (a latency SLO
//               on an op nothing answered at that step) breaks nothing.
//   flipped_to  the run's verdict (the SLO table's: the run, or for an SLO with `over` its worst
//               window) when it differs from the twin's; empty when the two agree. A flip belongs
//               to the run, not to one fault: it is named on each fault that broke the SLO during
//               its window, or, when none did (the drops of a drain after the fault tipped a run
//               total), on every fault.
struct FaultSlo {
  std::size_t slo{};  // index into Scenario::slos
  double over_max_s{};
  std::optional<SloStatus> flipped_to{};
};

// One fault's block (spec §4.4): the engine's record, what the fault did to the SLOs (only those
// with something to say, in the scenario's order), and the `at` of every earlier fault on the same
// target (the same node, the same datacenter, or a dc_down and a fault on a node of its
// datacenter, whichever came first) whose counting span, from its `at` to its recovery (the end of
// the run when it never recovered), contains this fault's `at`: both blocks count the steps they
// share, and the later one says so.
struct FaultBlock {
  engine::FaultRecord record{};
  std::vector<FaultSlo> slos{};
  std::vector<SimTime> overlaps{};
};

// Running aggregates over the StepRecords of a run, from `warmup` on (earlier records only fix the
// step length). Keeps a handful of sums per node and per op, never the records. Pattern: Observer —
// pass `[&](const auto& r) { summary.observe(r); }` to engine::Simulation::run. The Summary refers
// to `g`, which must outlive it. SLOs per window: for every SLO of the scenario with `over`, the
// Summary also keeps one more Summary of the same kind, fed only the steps of the window being
// accumulated (windows of length `over`, aligned from the end of warmup: [warmup + k·over, warmup +
// (k + 1)·over); a step belongs to the window its start falls in). When a step opens the next
// window the finished one is judged like a whole run and only its verdict is kept if it is the
// worst so far; so a window's figures come from exactly the code that makes the run's (its
// percentiles from percentile_seen_by_client, its drop rates from the same totals), and the memory
// is one Summary per windowed SLO, whatever the run's length.
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
  // 0 when nothing reached it. Not a loss (phase 3 spec §6.2): a failed call that a worker pool
  // retries, which is attempted again or ends as the pool's dead letter (counted apart, never as a
  // drop; dead_letter_rate judges those). So an external sink counts, step by step, its drops
  // times the share of its demand that came from callers that do NOT retry
  // (engine::NodeSample::retried_demand_per_s): 0 when only retrying pools call it, whatever its
  // own books say (NodeSummary::total_dropped keeps them), and 0.02 × 10 / 1030 when a client
  // sends it 10/s beside a pool's 1020/s. An unknown id is a programmer error (the loader checks
  // SLO nodes): throws std::invalid_argument.
  [[nodiscard]] double drop_rate(std::string_view node) const;
  // The worst node's drop rate: every node must keep its own, so the worst one decides. A bandwidth
  // link's pipe counts as a node here: a request its full room drops is lost as surely as one a
  // node drops, so its drop rate (megabytes dropped over megabytes sent) can be the worst. An SLO's
  // `node:` still names nodes only.
  [[nodiscard]] double drop_rate() const;
  // The id of the node with the highest drop rate (a link's `from->to` when its pipe is the worst;
  // the first of equals, nodes before links); empty when nothing dropped anything.
  [[nodiscard]] std::string_view worst_drop_node() const;
  // A worker pool's dead-letter rate (phase 3 spec §6.2; book ch. 10's dead-letter queue): the jobs
  // whose last attempt failed over the jobs PUBLISHED to its queue, both summed over the observed
  // steps (Σ dead_lettered / Σ (queue demand − re-published)), so in [0, 1]: the share of the work
  // the system gave up on, which the drop rate no longer sees once a pool retries. 0 for a pool
  // that retries nothing or reads no message queue. An unknown id is a programmer error (the
  // loader checks SLO nodes): throws std::invalid_argument.
  [[nodiscard]] double dead_letter_rate(std::string_view pool) const;
  // The same three for REJECTIONS, a rate limiter's refusals by policy (Σ rejected / Σ
  // demand; a leaky bucket's overflow included): a limiter that sheds what its limit says
  // is working, not failing, so its refusals are kept out of the drop rate (D5 of the
  // rate-limiter review) and get an SLO metric of their own.
  [[nodiscard]] double reject_rate(std::string_view node) const;
  [[nodiscard]] double reject_rate() const;
  [[nodiscard]] std::string_view worst_reject_node() const;
  [[nodiscard]] std::string_view scenario_name() const;
  // Made by of_estimate: one steady-state record, which leaves the scenario's faults out.
  [[nodiscard]] bool is_estimate() const { return estimate_; }
  [[nodiscard]] const graph::Graph& graph() const { return *graph_; }
  // The verdict of an SLO with `over`: its worst window's, the window still open at the end of the
  // run (a last window shorter than `over`) included. Every window is judged, except that a latency
  // SLO skips a window in which its op was not offered (no request, no latency), and one all of
  // whose windows are skipped is `skipped`. "Worst" is a fail before a
  // pass, then the higher observed value (every SLO is an upper bound). `slo` must be one of the
  // scenario's SLOs with `over` (evaluate_slos asks for exactly those); another is `skipped`.
  [[nodiscard]] SloResult windowed_verdict(const model::SloSpec& slo) const;

  // During a run with faults, before each step is observed: the windows "during" each fault of
  // `records` (engine::FaultRecord: [at, at + for), a cache_flush's [at, at + flush_window_s), to
  // the end of the run while that end is unknown). `observe` judges every SLO of the scenario on
  // each step that starts in one of them, that step alone (FaultSlo::over_max_s), and keeps the
  // verdicts: a few per step of the windows, nothing outside them. A flush's window ends when its
  // cache is full again, which only the run finds out, so session::run_summary hands the records
  // again at every step; the run's Summary and its twin's both watch the same records, so that
  // set_faults can compare them step by step.
  void watch_faults(std::span<const engine::FaultRecord> records);
  // After the run: one block per record (engine::Simulation::fault_records, in `at` order),
  // replacing any held, each with what the fault did to the SLOs against `twin`, the Summary of the
  // fault-free twin run over the same steps (engine::Simulation::twin_record), and the faults it
  // overlaps. The renderers print one block per fault after the SLO table, and a `faults` array in
  // the JSON (none without faults).
  void set_faults(std::span<const engine::FaultRecord> records, const Summary& twin);
  [[nodiscard]] std::span<const FaultBlock> faults() const { return faults_; }

 private:
  // Weighted first and second moments of a latency, accumulated across steps.
  struct Moments {
    double weight{};
    double weighted_mean{};    // Σ w·m
    double weighted_square{};  // Σ w·(v + m²) = Σ w·E[X²]
    void add(double w, double mean_s, double variance_s2);
    // The mixture's mean and variance; precondition: weight > 0.
    [[nodiscard]] double mean_s() const;
    [[nodiscard]] double variance_s2() const;
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
    std::optional<double> dead_lettered{};     // Σ dead_lettered_per_s·dt: worker pools only
    std::optional<double> republished{};       // Σ republished_per_s·dt: message queues only
    std::optional<double> last_connections{};  // the latest step's level: ws_gateways only
    std::optional<double> peak_connections{};  // the largest level of any step: ws_gateways only
    // Σ dropped_per_s × (retried_demand_per_s / demand_per_s) · dt: an external sink's drops of
    // the calls from pools with retries, which are attempted again, not lost (drop_rate).
    double retried_dropped{};
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
    double offered{};    // Σ offered·dt, requests
    double attempts{};   // Σ attempts·dt, requests sent: `offered` and the retries
    double answered{};   // Σ answered·dt, requests: the part of `offered` that came back
    double timed_out{};  // Σ timed_out·dt, requests: delivered, but after the client gave up
    double timed_out_attempts{};  // Σ timed_out_attempts·dt: every attempt that timed out
    Moments by_answered{};        // weighted by requests answered: what an average answer sees
    Moments by_time{};            // weighted by time: the fallback when nothing was answered
    // The delivered requests' latency before the client gave up (ResponseSample::
    // delivered_response), weighted by the attempts delivered, answered + timed out (every
    // attempt): what a timed-out client's percentiles are read off (percentile_seen_by_client).
    Moments by_delivered{};
    bool unavailable{};
    void add(const engine::ResponseSample& sample, double dt_s);
  };

  // An op's mean from the moments its answered requests left, its percentiles from the delivered
  // ones (Summary::ops).
  static void fill_latencies(const OpTotals& totals, OpSummary& op);
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
  // One step judged alone inside a watched window (watch_faults): its start, its length, and each
  // scenario SLO's verdict on it, in the scenario's order.
  struct JudgedStep {
    SimTime t{};
    SimTime dt{};
    std::vector<SloStatus> verdicts{};
  };
  // A watched window: [start, end), to the end of the run when `end` is empty.
  struct FaultWindow {
    SimTime start{};
    std::optional<SimTime> end{};
  };
  [[nodiscard]] static FaultWindow window_of(const engine::FaultRecord& record);
  [[nodiscard]] bool in_watched_window(SimTime t) const;
  void judge_step(const engine::StepRecord& record);
  [[nodiscard]] FaultBlock block_of(const engine::FaultRecord& record, const Summary& twin) const;
  [[nodiscard]] SimTime over_max(std::size_t slo, const FaultWindow& window,
                                 const Summary& twin) const;
  [[nodiscard]] bool twin_breaks(std::size_t slo, SimTime t) const;

  std::vector<FaultWindow> watched_{};  // watch_faults
  std::vector<JudgedStep> judged_{};    // the steps observed inside a watched window
  std::vector<FaultBlock> faults_{};    // set_faults
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
// A rate as the tables print one, with an SI prefix and four significant digits and the unit:
// "1.75k/s", "500/s". `simsys sweep` prints its rates with it.
[[nodiscard]] std::string format_rate(double per_second);
// A queue lag as the tables print one: format_duration, but "<1us" below a microsecond (an idle
// consumer group's lag is a vanishing Erlang C figure, not a measurement).
[[nodiscard]] std::string format_lag(double seconds);
// The tables' aligner, for a table of plain text cells (`simsys sweep` prints its own with it):
// every column padded to its widest cell, the first left-aligned and the rest right-aligned (so
// numbers line up on their units), two spaces between columns; each line starts with `indent`
// and ends with a newline, trailing blanks trimmed.
[[nodiscard]] std::string align_columns(std::span<const std::vector<std::string>> rows,
                                        std::string_view indent);

// ---- replications: one scenario run over several seeds of its rate noise (`run --replications`)
//
// Why: session::run_replications (session.cpp) says what replications are for; here is how N runs
// become one report. Every number a run's tables print becomes, across the runs, its median and
// its 10-90 % band, and every SLO the verdict of its median run with the number of runs it passed
// in.
//
// The quantile rule is NEAREST-RANK: the p-quantile of n sorted values is the value of rank
// ceil(p·n), counted from 1. For n = 5 the 10 % quantile is the 1st value, the median the 3rd and
// the 90 % quantile the 5th: the band is the smallest and the largest run. For even n the median is
// the lower of the two middle values (n = 10: the 5th). Why nearest rank and not an interpolation
// between two runs: with the handful of runs a replication has (5, 10, 20), an interpolated median
// is a number no run printed, and its digits would claim a precision five samples cannot give. A
// value of rank k is always some run's own number, so the median run can be named and its report
// shown (the JSON around `replications`, the fault blocks, each SLO's observed value).
// What this deliberately is not: a confidence interval. The band says how far the runs made spread,
// not where the true mean lies with 90 % confidence (that needs a t-distribution or batch means
// over many more runs).

// One number across the runs: its median and its 10-90 % band by the nearest-rank rule, and
// which run holds each of the three (indexes into the runs). Runs with equal values keep their
// order, so the ranks, and the runs named, do not depend on anything but the values.
struct Band {
  double median{};
  double low{};   // the 10 % quantile
  double high{};  // the 90 % quantile
  std::size_t median_run{};
  std::size_t low_run{};
  std::size_t high_run{};
};

// One (client, op): the op table's numbers across the runs (OpSummary's fields of the same names).
struct OpBands {
  std::string client{};
  std::string op{};
  Band offered_per_s{};
  Band attempts_per_s{};
  Band answered_per_s{};
  Band timed_out_per_s{};
  Band timed_out_attempts_per_s{};
  Band mean_s{};
  Band p50_s{};
  Band p95_s{};
  Band p99_s{};
};

// One node: the node table's numbers across the runs (NodeSummary's fields), `dead_lettered_per_s`
// only for a worker pool, `peak_queue_lag_s` only for a message queue, and in how many runs the
// node was unavailable at some step.
struct NodeBands {
  std::string id{};
  Band mean_rho{};
  Band peak_rho{};
  Band peak_backlog{};
  Band dropped{};
  Band rejected{};
  Band red_s{};
  std::optional<Band> dead_lettered_per_s{};
  std::optional<Band> peak_queue_lag_s{};
  std::size_t unavailable_in{};
};

// One SLO across the runs: the verdict of its MEDIAN RUN, the run whose observed value for this
// SLO is the median of the runs' observed values (a run that observed nothing ranks last), and the
// number of runs that passed it. A replicated SLO passes when its median run passes: "half the
// afternoons or more meet it", whatever the few that do not.
struct SloReplication {
  SloResult result{};
  std::size_t passes_in{};
  std::size_t median_run{};
};

// What each run's report is made of, `ops[k]`, `nodes[k]` and `slos[k]` being run k's ops, nodes
// and SLO verdicts (evaluate_slos), computed once per run: the bands are taken across them and the
// JSON's per-seed lists are written from them.
struct RunFigures {
  std::vector<std::vector<OpSummary>> ops{};
  std::vector<std::vector<NodeSummary>> nodes{};
  std::vector<std::vector<SloResult>> slos{};
};

// N runs of one scenario summarised. `seeds[k]` is run k's seed. Each SLO is judged by its own
// median run (SloReplication::median_run), and two SLOs can have different ones. `median_run` is
// the run whose report stands for all of them where one run's figures are needed (the JSON around
// `replications`, the table's summary line and its fault blocks): the first SLO's median run, so
// that its verdict, its observed value and the report around them come from one run; without SLOs
// the median run of the first op's mean latency; without either, the middle seed's.
struct ReplicationSummary {
  std::vector<int> seeds{};
  RunFigures per_run{};
  std::vector<OpBands> ops{};
  std::vector<NodeBands> nodes{};
  std::vector<SloReplication> slos{};
  std::size_t median_run{};
};

// `runs[k]` is the run of `seeds[k]`; every run must be of the same scenario (the same nodes, ops
// and SLOs): a mismatch, no run, or a seed count other than the run count is a programmer error,
// std::invalid_argument. The SLOs are each run's evaluate_slos of its scenario's SLOs.
[[nodiscard]] ReplicationSummary summarize_replications(std::span<const Summary> runs,
                                                        std::span<const int> seeds);

// Each SLO's replicated verdict (SloReplication::result), in the scenario's order: what all_pass
// turns into `run --replications`' exit code.
[[nodiscard]] std::vector<SloResult> replicated_verdicts(const ReplicationSummary& replicated);

// A table cell across the runs: the median's text, then the band's ends in brackets with an en
// dash, `1.62ms [1.61–1.63]`. The ends leave out their unit when both have the median's
// (`5.69ms [5.64–5.75]`, `0.105 [0.0938–0.112]`); when either is in another unit, or is no number
// at all, both ends are printed whole (`1.2ms [998us–1.4ms]`, `1.2ms [1.1ms–unavailable]`), so a
// bare number in the brackets always has the median's unit. When the three texts are the same the
// cell is that text alone (`0`, `-`): a band that prints as one value says nothing more.
[[nodiscard]] std::string format_band(std::string_view median, std::string_view low,
                                      std::string_view high);

// For people: two lines saying what the cells are and whose the summary line and the fault blocks
// are (ReplicationSummary::median_run's seed), that run's summary line, the node, link, database
// and op tables of a run with every number a format_band cell, the SLO table with `PASS in 3 of 5`
// or `FAIL (passed in 2 of 5)` and each SLO's median run's observed value and seed (`median run:
// seed 5`), and the fault blocks of the run that stands for all (scripted faults are the same in
// every run). `runs` are the runs summarize_replications summarised.
[[nodiscard]] std::string render_replications_table(const ReplicationSummary& replicated,
                                                    std::span<const Summary> runs, bool color);
// For programs: the median run's JSON report with the replicated SLO verdicts, each with the
// `median_seed` of its own median run, and one more key after them all, `replications`: `seeds`,
// `median_seed` (the run whose report surrounds it) and `per_seed`, where each op's, node's and
// SLO's numbers are lists with one entry per seed, each written exactly as that seed's own `run
// --report json` writes it.
[[nodiscard]] std::string render_replications_json(const ReplicationSummary& replicated,
                                                   std::span<const Summary> runs);

// `simsys explain`: every resolved number with its provenance, the links, the step order
// and a static load check (offered load vs capacity, no queueing).
[[nodiscard]] std::string render_explain(const graph::Graph& g);

// For plotting: one CSV row per node per step,
// `t_s,node,demand_per_s,drain_per_s,rho,backlog,drops_per_s,dead_lettered_per_s,rejected_per_s,
// latency_ms,zone,queue_lag_s,connections` (`dead_lettered_per_s` empty except on a worker pool's
// rows, `queue_lag_s` except on a message queue's, `connections` except on a ws_gateway's), then
// one per link with a bandwidth, its `from->to` in the node column, in megabytes.
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
// `t_s,client,op,offered_per_s,attempts_per_s,answered_per_s,timed_out_per_s,mean_ms` (spec §18
// entry 13; `timed_out_per_s` spec §19 entry 12; `attempts_per_s` phase 3 spec §6.1). The header is
// written on construction; the column rules are at OpSeriesWriter::observe in report.cpp. Pattern:
// Observer, like SeriesWriter: session::run_summary hands each step record to both, and neither
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
