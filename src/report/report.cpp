// Implementation unit of `simsys.report`: the running aggregates, SLO verdicts and the
// three renderers of a run (table, JSON, CSV series). `simsys explain` lives in explain.cpp, and
// the helpers both units need (number formatting, zone names, the aligned text table) in the
// module partition simsys.report:shared (shared.cppm). Heavy headers (<ostream>, <ranges>,
// ...) belong here, not in the interface unit (CLAUDE.md §3, GCC 14.2 'Bad file data' bug).
//
// Every number is formatted with std::to_chars (simsys::to_chars_text), never with a std::format
// precision such as "{:.3f}": with GCC 14.2, a precision in a format spec throws std::format_error
// at run time in a unit that imports simsys.core (CLAUDE.md §3, second <format> defect).
module;
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <functional>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <ostream>
#include <ranges>
#include <span>  // GCC 14: this unit iterates Graph's spans itself
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

// C++20 module implementation unit: `module simsys.report;` (no `export`) makes this file
// part of the named module. It sees everything the interface declared, exports nothing,
// and can include heavy headers in its own global module fragment, like a .cpp behind a
// header.
module simsys.report;

// C++20 `import :shared;`: imports a partition of this same module by its partition name alone
// (the `simsys.report` part is implied, and only units of simsys.report may import it): the
// helpers explain.cpp uses too, with module linkage, so shared yet invisible outside the module.
import :shared;

// C++20 `import`: the compiled interfaces of the modules this unit reads from. The
// interface's own imports would be visible anyway; listing them says what is used here.
import simsys.core;
import simsys.defaults;
import simsys.engine;
import simsys.flow;
import simsys.fluid;
import simsys.graph;
import simsys.model;
import simsys.queueing;
import simsys.workload;

namespace simsys::report {

namespace {

constexpr double infinity = std::numeric_limits<double>::infinity();

}  // namespace

// ---- number formatting ----

// The units, the SI prefixes and the formatters both units use (with_si_prefix, fmt_rate, percent,
// ...) are in shared.cppm. The two exported formatters of the tables (report.cppm) are defined
// here, outside the anonymous namespace, so the renderers below call them by their one name
// (format_duration, format_lag).

// Seconds in the largest unit that keeps the number >= 1, three significant digits:
// 0.0005 -> "500us", 120 -> "2m". Infinity means the node never answers: "unavailable". The body
// is :shared's duration_text, which the SimTime overload there calls too.
std::string format_duration(double seconds_value) {
  return duration_text(seconds_value);
}

std::string format_rate(double per_second) {
  return fmt_rate(per_second);
}

// A queue lag as the node table and the SLO lines print it: a duration, or "<1us" for a lag that
// would print below one microsecond. An idle consumer group's stochastic wait is Erlang C's tiny
// chance of waiting times a job (youtube's completion_q: 6.3e-11 s, "0.0632ns"), a figure below
// anything a queue's lag is ever measured in; the JSON and the CSV keep the number.
std::string format_lag(double seconds_value) {
  const double below = rounds_to_one * 1e-6;  // what format_duration would print under "1us"
  if (seconds_value > 0.0 && seconds_value < below) return "<1us";
  return format_duration(seconds_value);
}

namespace {

// ---- enum names ----

// The SLO metric and link direction names come from their tables in simsys.model
// (model::to_string), the same words the loader reads.

[[nodiscard]] std::string_view status_name(SloStatus status) {
  // C++20 `using enum`: brings the enumerators into this function's scope, so the cases
  // read `pass` instead of `SloStatus::pass`; nothing leaks outside the function.
  using enum SloStatus;
  switch (status) {
    case pass:
      return "pass";
    case fail:
      return "fail";
    case skipped:
      return "skipped";
  }
  // C++23 std::unreachable(): every enumerator returned above (-Wswitch, an error under
  // -Werror, proves it), so this point is unreachable. Replaces a dummy return value
  // that would hide a missing case, and silences -Wreturn-type.
  std::unreachable();
}

[[nodiscard]] bool is_latency(model::SloMetric metric) {
  using enum model::SloMetric;
  return metric == p50_latency || metric == p95_latency || metric == p99_latency;
}

// A zone's ANSI colour, for the coloured table (render_table's `color`).
[[nodiscard]] std::string_view ansi_for(fluid::Zone zone) {
  using enum fluid::Zone;
  switch (zone) {
    case green:
      return "\x1b[32m";
    case yellow:
      return "\x1b[33m";
    case red:
      return "\x1b[31m";
  }
  std::unreachable();
}

// ---- what a client with a timeout sees ----

// A percentile the reports print: its level q and its place on the normal scale, the z-score the
// lognormal fit reads it at (queueing::lognormal_quantile).
struct Quantile {
  double level;
  double z;
};
// C++20 designated initializers: name the fields being set, so two doubles (here `level` and `z`)
// cannot be swapped unnoticed; a field left out keeps its default member initializer. Replaces
// positional braces, which say nothing about which value is which.
constexpr Quantile median{.level = 0.50, .z = queueing::z_p50};
constexpr Quantile ninety_fifth{.level = 0.95, .z = queueing::z_p95};
constexpr Quantile ninety_ninth{.level = 0.99, .z = queueing::z_p99};

// timed_out / (answered + timed_out): the share of the attempts the path delivered that came back
// after the client had given up; 0 when nothing was delivered. `timed_out` counts every attempt
// that timed out (OpTotals::timed_out_attempts), the retried ones too: the client waited past its
// timeout on each, and that is the share a percentile asks about.
[[nodiscard]] double timed_out_share(double answered, double timed_out) {
  const double delivered = answered + timed_out;
  return delivered > 0.0 ? timed_out / delivered : 0.0;
}

// Did more than 1 − q of the delivered requests time out (more than 1 % for a p99)? Then no
// request at quantile q was answered: the client saw the timeout there.
[[nodiscard]] bool quantile_timed_out(double share_timed_out, Quantile q) {
  return share_timed_out > 1.0 - q.level;
}

// The percentile at q of the response a client sees: the one rule for the whole run's op table and
// for each window of an SLO's `over`. `mean_s` and `variance_s2` are the DELIVERED requests'
// moments, before the timeout cut them (Summary::OpTotals::by_delivered), s the timed-out share of
// them.
//  - Without a timeout: the lognormal fitted to those moments, read at q.
//  - With one, when more than 1 − q of the delivered requests timed out (s > 1 − q): the
//    timeout, flagged `beyond_timeout`. No request at quantile q was answered; a latency SLO at q
//    then fails whatever its max: past a max below the timeout the requests are certainly slower,
//    and a max at or above it cannot be checked (the client stopped measuring at the timeout).
//  - With one, otherwise (s <= 1 − q): the same delivered fit read at q, capped at the timeout.
//    Why the delivered fit: the engine decides who times out on that fit (give_up_after), so a
//    request is timed out exactly when the fit puts it past the timeout, and the fastest q of the
//    delivered requests are the fastest q of the fit: answered ones, all faster than the timeout.
//    The client's q-quantile IS the fit's, which the timeout does not move. The cap only matters
//    when the run mixes steps: one steady step has s <= 1 − q exactly when the fit's q-quantile is
//    at or below the timeout.
// So a percentile never falls as the timeout grows (a longer timeout times out fewer requests and
// caps higher), and a client whose timeout cut nothing reads exactly what it would without one.
// The rule it replaced refitted a lognormal to the answered requests' truncated moments and read it
// at q / (1 − s); that refit has a tail past the cut the truncated distribution does not, and was
// neither exact nor monotonic: M/M/1 at 990/s and 1 ms (model p99 490.5 ms) printed a p99 of
// 508.9 ms behind a 600 ms timeout and 475.5 ms behind a 1 s one.
struct SeenPercentile {
  double seconds;
  bool beyond_timeout;
};

[[nodiscard]] SeenPercentile percentile_seen_by_client(double mean_s, double variance_s2,
                                                       double share_timed_out,
                                                       std::optional<double> timeout_s,
                                                       Quantile q) {
  const double fit_s = queueing::lognormal_quantile(mean_s, variance_s2, q.z);
  if (!timeout_s) return {.seconds = fit_s, .beyond_timeout = false};
  if (quantile_timed_out(share_timed_out, q)) {
    return {.seconds = *timeout_s, .beyond_timeout = true};
  }
  // The fit first in std::min: a NaN propagates.
  return {.seconds = std::min(fit_s, *timeout_s), .beyond_timeout = false};
}

// The same question of a summarised op.
[[nodiscard]] bool beyond_timeout(const OpSummary& op, Quantile q) {
  return op.timeout_s && quantile_timed_out(op.timed_out_share, q);
}

// An op of which nothing came back answered in time: its requests all timed out, all failed on the
// path (a sink failing everything it serves), or the path never answers. No answered request has a
// latency: the JSON's null, the table's `-` (the engine's stand-in moments are the timeout or the
// failed requests' sojourn, never a measurement).
[[nodiscard]] bool nothing_answered(const OpSummary& op) {
  return op.answered_per_s <= 0.0;
}

// An op nobody sent (a client of rate 0, `rate: {base: 0/s}`, or an SLO window in which the op was
// not offered): no request, so no latency either. A latency SLO leaves it out without a note:
// nothing went wrong.
[[nodiscard]] bool nothing_offered(const OpSummary& op) {
  return op.offered_per_s <= 0.0;
}

// The case of nothing_answered that no other rule judges: requests were sent and every one was lost
// on a path that does answer (nothing timed out, a finite latency), so the moments describe
// requests that all failed. A latency SLO does not read such an op, and says so (unanswered_note).
// One that timed out fails at the timeout (fail_if_quantile_timed_out), an unavailable one at
// infinity.
[[nodiscard]] bool every_request_lost(const OpSummary& op) {
  return !nothing_offered(op) && nothing_answered(op) && op.timed_out_attempts_per_s <= 0.0 &&
         std::isfinite(op.mean_s);
}

// ---- SLO evaluation ----

// An SLO's limit as a plain number: seconds for a latency, the ratio itself otherwise.
// Pattern: Visitor over a closed sum type — the variant lists every kind of limit, and
// std::visit fails to compile if a lambda for one of them is missing.
[[nodiscard]] double limit_value(const std::variant<SimTime, double>& max) {
  // core::Overloaded: the overloaded-visitor idiom; C++20 class template argument deduction for
  // aggregates deduces its lambdas' types from the braces (see simsys.core).
  return std::visit(core::Overloaded{[](SimTime limit) { return seconds(limit); },
                                     [](double limit) { return limit; }},
                    max);
}

// Whether a latency SLO reads `op`: the SLO's client's op, or any of its ops when it names none.
[[nodiscard]] bool slo_reads_op(const model::SloSpec& slo, const OpSummary& op) {
  return op.client == slo.node.value_or("") && (!slo.op || op.op == *slo.op);
}

// The latency quantile `quantile` of the client's op, or the worst one over all of the
// client's ops when the SLO names none (every op must meet it, so the worst decides); an op nobody
// sent (nothing_offered) or whose requests were all lost (every_request_lost) is left out, and
// with only such ops there is nothing to observe (skipped).
// `double OpSummary::*` is a pointer to a data member (C++98): it selects p50/p95/p99
// without three copies of the loop.
[[nodiscard]] std::optional<double> worst_latency(std::span<const OpSummary> ops,
                                                  const model::SloSpec& slo,
                                                  double OpSummary::*quantile) {
  std::optional<double> worst;
  for (const auto& op : ops) {
    if (!slo_reads_op(slo, op) || nothing_offered(op) || every_request_lost(op)) continue;
    const double value = op.*quantile;
    worst = worst ? std::max(*worst, value) : value;
  }
  return worst;
}

// Why a latency SLO observed less than it reads: "nobody answered c.op" for each op it would read
// whose requests were all lost (empty when there is none, or for any other metric).
[[nodiscard]] std::string unanswered_note(std::span<const OpSummary> ops,
                                          const model::SloSpec& slo) {
  if (!is_latency(slo.metric)) return {};
  std::vector<std::string> silent;
  for (const auto& op : ops) {
    if (slo_reads_op(slo, op) && every_request_lost(op)) silent.push_back(op.client + "." + op.op);
  }
  if (silent.empty()) return {};
  return "nobody answered " + join(silent, ", ") + ": no latency to observe";
}

[[nodiscard]] std::optional<double> mean_utilization(std::span<const NodeSummary> nodes,
                                                     const model::SloSpec& slo) {
  // C++20 std::ranges::find with a projection (&NodeSummary::id): "the node whose id is
  // X" without a hand-written comparator lambda.
  const auto found = std::ranges::find(nodes, slo.node.value_or(""), &NodeSummary::id);
  if (found == nodes.end()) return std::nullopt;
  return found->mean_rho;
}

// The named node's mean staleness (spec §18 entry 6: a SQL database's write sojourn at the primary
// + replication lag + sojourn at a replica, how old the data a read may return is), in seconds;
// empty for a node with no lagging copies, whose staleness SLO is then skipped (nothing to
// observe).
[[nodiscard]] std::optional<double> mean_staleness(std::span<const NodeSummary> nodes,
                                                   const model::SloSpec& slo) {
  const auto found = std::ranges::find(nodes, slo.node.value_or(""), &NodeSummary::id);
  if (found == nodes.end()) return std::nullopt;
  return found->mean_staleness_s;
}

// The peak lag after warmup (NodeSummary::peak_queue_lag_s) of the named message queue, or of the
// worst queue when the SLO names none (every queue must keep the bound, so the worst decides; spec
// §19 entry 14); empty when no message queue ran (a frontier's lag is phase 3's), and the SLO is
// skipped.
[[nodiscard]] std::optional<double> peak_queue_lag(std::span<const NodeSummary> nodes,
                                                   const model::SloSpec& slo) {
  if (slo.node) {
    const auto found = std::ranges::find(nodes, *slo.node, &NodeSummary::id);
    return found == nodes.end() ? std::nullopt : found->peak_queue_lag_s;
  }
  std::optional<double> worst;
  for (const auto& node : nodes) {
    // flow::nan_propagating_max: a NaN lag at any queue stays in the fold, where std::max would
    // let the next queue's finite lag replace it.
    if (node.peak_queue_lag_s) {
      worst = flow::nan_propagating_max(*node.peak_queue_lag_s, worst.value_or(0.0));
    }
  }
  return worst;
}

// The named worker pool's dead-letter rate (Summary::dead_letter_rate); empty for a node that is no
// worker pool (the loader refuses one; built in code, the SLO is skipped).
[[nodiscard]] std::optional<double> pool_dead_letter_rate(const Summary& summary,
                                                          const model::SloSpec& slo) {
  const auto index = slo.node ? summary.graph().index_of(*slo.node) : std::nullopt;
  if (!index || summary.graph().nodes()[*index].spec.type != model::NodeType::worker_pool) {
    return std::nullopt;
  }
  return summary.dead_letter_rate(*slo.node);
}

// What the run observed for the SLO's metric; empty when the model does not observe it.
[[nodiscard]] std::optional<double> observe_metric(const Summary& summary,
                                                   std::span<const NodeSummary> nodes,
                                                   std::span<const OpSummary> ops,
                                                   const model::SloSpec& slo) {
  using enum model::SloMetric;
  switch (slo.metric) {
    case p50_latency:
      return worst_latency(ops, slo, &OpSummary::p50_s);
    case p95_latency:
      return worst_latency(ops, slo, &OpSummary::p95_s);
    case p99_latency:
      return worst_latency(ops, slo, &OpSummary::p99_s);
    case drop_rate:  // the named node's, else the worst node's
      return slo.node ? summary.drop_rate(*slo.node) : summary.drop_rate();
    case reject_rate:  // the same for a rate limiter's refusals
      return slo.node ? summary.reject_rate(*slo.node) : summary.reject_rate();
    case utilization:
      return mean_utilization(nodes, slo);
    case staleness:
      return mean_staleness(nodes, slo);
    case queue_lag:  // the peak, not the mean: a lag bound is a bound on the worst moment
      return peak_queue_lag(nodes, slo);
    case dead_letter_rate:  // a worker pool's (the loader refuses any other node)
      return pool_dead_letter_rate(summary, slo);
  }
  std::unreachable();
}

// A staleness bound promises that a replica catches up within it. A replica that cannot replay
// every committed write falls behind without bound (the model drops the excess at queue_limit; a
// real replica keeps it in its relay log/WAL until it is rebuilt), so its lag only grows: the SLO
// fails whatever the mean staleness the model's bounded queue reports, and says why, in one
// replica's terms: "each replica misses 426.3/s of 900/s committed writes (47.4%)".
void fail_if_replica_falls_behind(SloResult& result, std::span<const NodeSummary> nodes) {
  const auto found = std::ranges::find(nodes, result.slo.node.value_or(""), &NodeSummary::id);
  if (found == nodes.end()) return;
  const double missed_per_s = found->mean_replication_dropped_per_s.value_or(0.0);
  if (missed_per_s <= 0.0) return;
  const double committed_per_s = found->mean_committed_writes_per_s.value_or(0.0);
  result.status = SloStatus::fail;
  result.note = "each replica misses " + fmt_rate(missed_per_s) + " of " +
                fmt_rate(committed_per_s) + " committed writes";
  if (committed_per_s > 0.0) result.note += " (" + percent(missed_per_s / committed_per_s) + ")";
}

// The quantile a latency SLO bounds.
[[nodiscard]] Quantile quantile_of(model::SloMetric metric) {
  if (metric == model::SloMetric::p50_latency) return median;
  if (metric == model::SloMetric::p95_latency) return ninety_fifth;
  return ninety_ninth;
}

// A latency SLO at quantile q on an op of which more than 1 − q of the delivered requests timed
// out fails whatever its max (percentile_seen_by_client): it observes the timeout and says why,
// "57.3% of delivered requests timed out (more than 1%)", naming the op when the SLO names none
// (the op with the largest timed-out share speaks for the client).
void fail_if_quantile_timed_out(SloResult& result, std::span<const OpSummary> ops) {
  const Quantile q = quantile_of(result.slo.metric);
  const OpSummary* worst = nullptr;
  for (const auto& op : ops) {
    if (!slo_reads_op(result.slo, op) || !beyond_timeout(op, q)) continue;
    if (worst == nullptr || op.timed_out_share > worst->timed_out_share) worst = &op;
  }
  if (worst == nullptr) return;
  result.status = SloStatus::fail;
  result.observed = *worst->timeout_s;
  const std::string requests = result.slo.op ? "requests" : worst->op + " requests";
  result.note = percent(worst->timed_out_share) + " of delivered " + requests +
                " timed out (more than " + percent(1.0 - q.level) + ")";
}

// An SLO with nothing observed: the run has nothing the metric is measured on (a staleness on a
// node that is no database, a queue lag with no message queue, a frontier's before phase 3), or
// no step was observed.
[[nodiscard]] SloResult skipped(const model::SloSpec& slo) {
  return SloResult{.slo = slo, .observed = std::nullopt, .status = SloStatus::skipped};
}

// ---- SLOs per window ----

// Are `a` and `b` the same SLO, every key alike? Written out rather than a defaulted
// SloSpec::operator==: SimTime's == (std::chrono's) is not reachable through the model module's
// interface (CLAUDE.md §3), so the tick counts are compared.
[[nodiscard]] bool same_slo(const model::SloSpec& a, const model::SloSpec& b) {
  // C++23 std::optional::transform: maps a present value and passes an empty optional through,
  // replacing `t ? std::optional{t->count()} : std::nullopt`.
  const auto ticks = [](const std::optional<SimTime>& t) {
    return t.transform([](SimTime d) { return d.count(); });
  };
  return a.metric == b.metric && a.node == b.node && a.op == b.op &&
         a.max.index() == b.max.index() && limit_value(a.max) == limit_value(b.max) &&
         ticks(a.over) == ticks(b.over);
}

// The order of verdicts by badness: a fail before a pass, then the higher observed value (every
// SLO bounds its metric from above), the earlier window on a tie. A skipped window never counts.
[[nodiscard]] int badness(SloStatus status) {
  return status == SloStatus::fail ? 2 : status == SloStatus::pass ? 1 : 0;
}

[[nodiscard]] bool worse(const SloResult& candidate, const SloResult& than) {
  if (badness(candidate.status) != badness(than.status)) {
    return badness(candidate.status) > badness(than.status);
  }
  return candidate.observed.value_or(-infinity) > than.observed.value_or(-infinity);
}

void keep_if_worse(std::optional<SloResult>& worst, SloResult candidate) {
  if (candidate.status == SloStatus::skipped) return;
  if (!worst || worse(candidate, *worst)) worst = std::move(candidate);
}

// A metric passes when it is at most its limit. An infinite latency (a node that never
// answers) and NaN both fail. No data is an error, not a pass: with no observed step every
// average would be a 0 that passes any limit, so nothing is evaluated (the CLI exits 1).
[[nodiscard]] SloResult evaluate_slo(const Summary& summary, std::span<const NodeSummary> nodes,
                                     std::span<const OpSummary> ops, const model::SloSpec& slo) {
  if (summary.observed_steps() == 0) return skipped(slo);
  const auto observed = observe_metric(summary, nodes, ops, slo);
  if (!observed) {
    SloResult result = skipped(slo);
    result.note = unanswered_note(ops, slo);
    return result;
  }
  const bool within = *observed <= limit_value(slo.max);
  SloResult result{.slo = slo,
                   .observed = observed,
                   .status = within ? SloStatus::pass : SloStatus::fail,
                   .note = unanswered_note(ops, slo)};
  if (slo.metric == model::SloMetric::staleness) fail_if_replica_falls_behind(result, nodes);
  if (is_latency(slo.metric)) fail_if_quantile_timed_out(result, ops);
  return result;
}

// ---- table ----

// A latency, a staleness or a queue lag is a time, printed with its unit; every other metric a
// ratio.
[[nodiscard]] std::string fmt_slo_value(model::SloMetric metric, double value) {
  if (metric == model::SloMetric::queue_lag) return format_lag(value);
  const bool is_time = is_latency(metric) || metric == model::SloMetric::staleness;
  return is_time ? format_duration(value) : significant(value);
}

// A limit in its own kind: a duration with a unit (queue_lag's 1s too), a ratio as is.
[[nodiscard]] std::string fmt_limit(const std::variant<SimTime, double>& max) {
  return std::visit(core::Overloaded{[](SimTime limit) { return format_duration(limit); },
                                     [](double limit) { return significant(limit); }},
                    max);
}

// The drop rate shown is the worst node's, named when some node dropped anything; the
// reject rate (the worst node's too) only when some rate limiter refused anything.
[[nodiscard]] std::string table_header(const Summary& summary) {
  const std::string head = "scenario " + std::string{summary.scenario_name()} + "  ";
  if (summary.observed_steps() == 0) return head + "0 steps observed: nothing to evaluate\n";
  const auto worst = summary.worst_drop_node();
  const auto worst_rejecting = summary.worst_reject_node();
  const std::string rejections = worst_rejecting.empty()
                                     ? std::string{}
                                     : "  reject rate " + percent(summary.reject_rate()) +
                                           " (worst: " + std::string{worst_rejecting} + ")";
  return head + std::to_string(summary.observed_steps()) + " steps observed  drop rate " +
         percent(summary.drop_rate()) +
         (worst.empty() ? std::string{} : " (worst: " + std::string{worst} + ")") + rejections +
         "\n";
}

// Under `estimate`, a scenario with scripted faults gets a line saying they are left out (phase 3
// spec §4.2): estimate is the steady state of every node in the identity condition
// (engine::Simulation::estimate), and only `run` steps through the faults. Nothing otherwise.
[[nodiscard]] std::string faults_note(const Summary& summary) {
  const auto scripted = summary.graph().scenario().faults.size();
  if (!summary.is_estimate() || scripted == 0) return {};
  return "faults: " + std::to_string(scripted) +
         " scripted (estimate shows the steady state without them; run applies them)\n";
}

// A table before it is aligned: its header, its body rows and, for every body cell, the number
// that cell prints (`text_cell` for a cell that prints text: a name, a client's `-`, the
// `unavailable` mark). A run's report prints the rows as they are (table_text); `run
// --replications` builds one per run and prints each cell as the band of its numbers across the
// runs (banded_parts). Row builders shared by both keep the two reports' cells alike.
struct TableParts {
  Row header{};
  std::vector<Row> rows{};
  std::vector<std::vector<double>> numbers{};

  void add(Row row, std::vector<double> row_numbers) {
    rows.push_back(std::move(row));
    numbers.push_back(std::move(row_numbers));
  }
};

// The number of a cell that prints text: NaN, which no number equals.
constexpr double text_cell = std::numeric_limits<double>::quiet_NaN();

// `title`, then the header and the rows aligned under it.
[[nodiscard]] std::string table_text(std::string_view title, const TableParts& parts) {
  std::vector<Row> rows{parts.header};
  rows.insert(rows.end(), parts.rows.begin(), parts.rows.end());
  return std::string{title} + "\n" + aligned(rows);
}

// The same, or nothing for a table without rows (no bandwidth link, no database).
[[nodiscard]] std::string table_text_if_any(std::string_view title, const TableParts& parts) {
  return parts.rows.empty() ? std::string{} : table_text(title, parts);
}

// A queue's cells (a node's, or a bandwidth link's pipe's), its rho cells coloured by the queue's
// own zone thresholds; the tables add what follows them.
[[nodiscard]] Row queue_cells(const NodeSummary& node, const fluid::Capacity& capacity,
                              bool color) {
  const auto colour_of = [&](double rho) {
    return color ? ansi_for(fluid::zone_of(rho, capacity)) : std::string_view{};
  };
  return {{node.id},
          {significant(node.mean_rho), colour_of(node.mean_rho)},
          {significant(node.peak_rho), colour_of(node.peak_rho)},
          {with_si_prefix(node.peak_backlog)},
          {with_si_prefix(node.total_dropped)},
          {with_si_prefix(node.total_rejected)},
          {significant(node.red_time_s)}};
}

// The numbers of queue_cells, cell by cell.
[[nodiscard]] std::vector<double> queue_numbers(const NodeSummary& node) {
  return {text_cell,          node.mean_rho,       node.peak_rho,  node.peak_backlog,
          node.total_dropped, node.total_rejected, node.red_time_s};
}

// The last, unlabelled cell of a queue's row: why its figures are not to be trusted, if they are
// not.
[[nodiscard]] Cell unavailable_cell(const NodeSummary& node) {
  return {node.unavailable ? "unavailable" : ""};
}

// Does some worker pool of the run retry (`retries` >= 1)? Only then has the node table its
// `dead_lettered/s` column (phase 3 spec §6.2), so a run of phase 2's pools prints the table it
// always did.
[[nodiscard]] bool some_pool_retries(const graph::Graph& g) {
  // C++20 std::ranges::any_of: the range itself instead of a begin/end pair (std::any_of).
  return std::ranges::any_of(g.nodes(), [](const graph::ResolvedNode& node) {
    return node.spec.type == model::NodeType::worker_pool && node.spec.retries.value_or(0) > 0;
  });
}

// Where the `dead_lettered/s` column sits: right after `dropped` (node, mean_rho, peak_rho,
// peak_backlog, dropped), since a dead letter is the pool's count beside the drops it is not.
constexpr std::size_t dead_letter_column = 5;

// Clients are sources with no queue: their queue columns show "-". A message queue's `peak_lag`
// is its peak lag after warmup (NodeSummary::peak_queue_lag_s), "-" on every other row. With
// `dead_letters` the row has the `dead_lettered/s` cell too: a worker pool's mean rate of dead
// letters (NodeSummary::mean_dead_lettered_per_s, 0 without retries), "-" on every other row.
[[nodiscard]] Row node_row(const NodeSummary& node, const graph::ResolvedNode& resolved, bool color,
                           bool dead_letters) {
  Row row = resolved.spec.type == model::NodeType::client
                ? Row{{node.id}, {"-"}, {"-"}, {"-"}, {"-"}, {"-"}, {"-"}, {"-"}}
                : queue_cells(node, resolved.capacity, color);
  if (resolved.spec.type != model::NodeType::client) {
    row.push_back({node.peak_queue_lag_s ? format_lag(*node.peak_queue_lag_s) : "-"});
    row.push_back(unavailable_cell(node));
  }
  if (dead_letters) {
    const auto rate = node.mean_dead_lettered_per_s;
    // std::next over a difference_type: the column index is a size_t, the iterator's offset signed.
    row.insert(std::next(row.begin(), static_cast<std::ptrdiff_t>(dead_letter_column)),
               Cell{rate ? with_si_prefix(*rate) : "-"});
  }
  return row;
}

// The numbers of node_row, cell by cell: none on a client's row. With `dead_letters`, the pool's
// dead-letter rate at dead_letter_column, as node_row puts its cell there (text on other rows).
[[nodiscard]] std::vector<double> node_numbers(const NodeSummary& node,
                                               const graph::ResolvedNode& resolved,
                                               bool dead_letters) {
  if (resolved.spec.type == model::NodeType::client) {
    return std::vector<double>(dead_letters ? 9 : 8, text_cell);
  }
  auto numbers = queue_numbers(node);
  numbers.push_back(node.peak_queue_lag_s.value_or(text_cell));
  numbers.push_back(text_cell);
  if (dead_letters) {
    numbers.insert(std::next(numbers.begin(), static_cast<std::ptrdiff_t>(dead_letter_column)),
                   node.mean_dead_lettered_per_s.value_or(text_cell));
  }
  return numbers;
}

[[nodiscard]] TableParts node_parts(const Summary& summary, bool color) {
  TableParts parts{.header = {{"node"},
                              {"mean_rho"},
                              {"peak_rho"},
                              {"peak_backlog"},
                              {"dropped"},
                              {"rejected"},
                              {"red_s"},
                              {"peak_lag"}}};
  const bool dead_letters = some_pool_retries(summary.graph());
  if (dead_letters) {
    parts.header.insert(
        std::next(parts.header.begin(), static_cast<std::ptrdiff_t>(dead_letter_column)),
        Cell{"dead_lettered/s"});
  }
  const auto nodes = summary.nodes();
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const auto& resolved = summary.graph().nodes()[i];
    parts.add(node_row(nodes[i], resolved, color, dead_letters),
              node_numbers(nodes[i], resolved, dead_letters));
  }
  return parts;
}

constexpr std::string_view node_title = "nodes";

[[nodiscard]] std::string node_table(const Summary& summary, bool color) {
  return table_text(node_title, node_parts(summary, color));
}

// The bandwidth links' pipes, in the node table's columns but a table of their own, since their
// numbers are megabytes: `a->b`, its utilization, the most megabytes it held and the megabytes it
// dropped (a pipe refuses nothing, so `rejected` is 0). Nothing without a bandwidth link.
[[nodiscard]] TableParts link_parts(const Summary& summary, bool color) {
  TableParts parts{.header = {{"link"},
                              {"mean_rho"},
                              {"peak_rho"},
                              {"peak_backlog"},
                              {"dropped"},
                              {"rejected"},
                              {"red_s"}}};
  const auto links = summary.links();
  std::size_t next = 0;  // links() follows Graph::links(), pipes only
  for (const auto& link : summary.graph().links()) {
    if (!link.pipe) continue;
    const auto& queue = links[next++].queue;
    Row row = queue_cells(queue, *link.pipe, color);
    row.push_back(unavailable_cell(queue));
    auto numbers = queue_numbers(queue);
    numbers.push_back(text_cell);
    parts.add(std::move(row), std::move(numbers));
  }
  return parts;
}

constexpr std::string_view link_title =
    "links (bandwidth pipes, in megabytes: peak_backlog and dropped are MB)";

[[nodiscard]] std::string link_table(const Summary& summary, bool color) {
  return table_text_if_any(link_title, link_parts(summary, color));
}

// A database's replication, which the node table has no columns for: the mean staleness, the
// writes one shard's primary committed per second (what each of its replicas must replay) and
// the share of them one replica missed (a replica that misses any falls behind without bound).
// Only when the run has a database.
[[nodiscard]] TableParts replication_parts(const Summary& summary) {
  TableParts parts{.header = {{"database"},
                              {"staleness"},
                              {"committed writes/s per shard"},
                              {"missed writes/s per replica"}}};
  for (const auto& node : summary.nodes()) {
    if (!node.mean_replication_dropped_per_s) continue;
    const double staleness = node.mean_staleness_s.value_or(0.0);
    const double committed = node.mean_committed_writes_per_s.value_or(0.0);
    const double missed = *node.mean_replication_dropped_per_s;
    parts.add({{node.id},
               {format_duration(staleness)},
               {with_si_prefix(committed)},
               {with_si_prefix(missed)}},
              {text_cell, staleness, committed, missed});
  }
  return parts;
}

constexpr std::string_view replication_title = "replication";

[[nodiscard]] std::string replication_table(const Summary& summary) {
  return table_text_if_any(replication_title, replication_parts(summary));
}

// An op's latency cell: `-` when nobody was answered (a sink that fails everything it serves has no
// latency to show, only the sojourn of requests that all failed), else the duration.
[[nodiscard]] std::string op_latency_cell(const OpSummary& op, double seconds_value) {
  return nothing_answered(op) ? "-" : format_duration(seconds_value);
}

// A percentile's cell: `>1s` when more than 1 − q of the delivered requests timed out (the client
// never saw that quantile answered: percentile_seen_by_client), else as any latency cell.
[[nodiscard]] std::string op_percentile_cell(const OpSummary& op, double seconds_value,
                                             Quantile q) {
  if (beyond_timeout(op, q)) return ">" + format_duration(*op.timeout_s);
  return op_latency_cell(op, seconds_value);
}

// The `attempts/s` column, right after `offered/s`, is always there: what the clients sent,
// retries included (equal to offered/s for a client without retries, so a storm stands out as the
// row where it is higher). The `timed out/s` column is there when some client has a timeout:
// without one nothing can time out, and the table keeps its old shape.
[[nodiscard]] TableParts op_parts(const Summary& summary) {
  const auto ops = summary.ops();
  // C++20 std::ranges::any_of: the range itself instead of a begin/end pair (std::any_of), so it
  // cannot be paired with another container's end by mistake.
  const bool any_timeout =
      std::ranges::any_of(ops, [](const OpSummary& op) { return op.timeout_s.has_value(); });
  TableParts parts{.header = {{"client.op"}, {"offered/s"}, {"attempts/s"}}};
  if (any_timeout) parts.header.push_back({"timed out/s"});
  parts.header.insert(parts.header.end(), {{"mean"}, {"p50"}, {"p95"}, {"p99"}, {"(estimated)"}});
  for (const auto& op : ops) {
    Row row{{op.client + "." + op.op},
            {with_si_prefix(op.offered_per_s)},
            {with_si_prefix(op.attempts_per_s)}};
    std::vector<double> numbers{text_cell, op.offered_per_s, op.attempts_per_s};
    if (any_timeout) {
      row.push_back({with_si_prefix(op.timed_out_per_s)});
      numbers.push_back(op.timed_out_per_s);
    }
    row.insert(row.end(), {{op_latency_cell(op, op.mean_s)},
                           {op_percentile_cell(op, op.p50_s, median)},
                           {op_percentile_cell(op, op.p95_s, ninety_fifth)},
                           {op_percentile_cell(op, op.p99_s, ninety_ninth)}});
    numbers.insert(numbers.end(), {op.mean_s, op.p50_s, op.p95_s, op.p99_s});
    parts.add(std::move(row), std::move(numbers));
  }
  return parts;
}

constexpr std::string_view op_title =
    "ops (percentiles estimated: lognormal fit to the response moments)";

[[nodiscard]] std::string op_table(const Summary& summary) {
  return table_text(op_title, op_parts(summary));
}

[[nodiscard]] Cell status_cell(SloStatus status, bool color) {
  using enum SloStatus;
  switch (status) {
    case pass:
      return {"PASS", color ? ansi_for(fluid::Zone::green) : std::string_view{}};
    case fail:
      return {"FAIL", color ? ansi_for(fluid::Zone::red) : std::string_view{}};
    case skipped:
      return {"SKIP", {}};
  }
  std::unreachable();
}

// Which window decided an SLO with `over`: `(worst 10m from 00:20)`, its length as `over` reads
// and its start on the run's clock (window_clock). A last window cut short by the end of the run
// is labelled with the length it had: `(worst 28m from 14:02, the run's short last window)`.
// Under `estimate`, which measured no window: `(at 00:00 only: run checks each 1h)`. Empty
// without `over`, or when every window was skipped.
[[nodiscard]] std::string window_text(const SloResult& r) {
  if (!r.slo.over) return {};
  if (r.at_one_instant) {
    return "(at 00:00 only: run checks each " + describe_duration(*r.slo.over) + ")";
  }
  if (!r.window_start || !r.window_length) return {};
  const bool cut_short = r.window_length->count() < r.slo.over->count();
  return "(worst " + describe_duration(*r.window_length) + " from " +
         window_clock(*r.window_start) + (cut_short ? ", the run's short last window)" : ")");
}

// The trailing, unlabelled cell of an SLO's row: the window that decided it and why the verdict
// is not what `observed` against `max` alone would give, each when there is one.
[[nodiscard]] std::string slo_remark(const SloResult& r) {
  const std::string window = window_text(r);
  if (window.empty() || r.note.empty()) return window + r.note;
  return window + "  " + r.note;
}

// One SLO's row under the status cell given.
[[nodiscard]] Row slo_row(const SloResult& r, Cell status) {
  const auto observed =
      r.observed.transform([&](double v) { return fmt_slo_value(r.slo.metric, v); });
  Row row{std::move(status),          {std::string{model::to_string(r.slo.metric)}},
          {r.slo.node.value_or("-")}, {r.slo.op.value_or("-")},
          {observed.value_or("-")},   {fmt_limit(r.slo.max)}};
  // A seventh, unlabelled column: which window, and why.
  if (const auto remark = slo_remark(r); !remark.empty()) row.push_back({remark});
  return row;
}

[[nodiscard]] Row slo_header() {
  return {{"status"}, {"metric"}, {"node"}, {"op"}, {"observed"}, {"max"}};
}

[[nodiscard]] std::string slo_table(std::span<const SloResult> slos, bool color) {
  if (slos.empty()) return {};
  std::vector<Row> rows{slo_header()};
  for (const auto& r : slos) rows.push_back(slo_row(r, status_cell(r.status, color)));
  return "slo\n" + aligned(rows, Align::all_left);
}

// ---- the fault block (spec §4.4; engine::FaultRecord says what each line models) ----

// Float residue of the fluid arithmetic printed as the 0 it is (a served rate of 1e-14/s).
[[nodiscard]] double without_residue(double value) {
  constexpr double residue = 1e-9;
  return std::abs(value) < residue ? 0.0 : value;
}

// The faulted part as the block names it: the node, or a dc_down's datacenter.
[[nodiscard]] std::string fault_target(const engine::FaultRecord& r) {
  return r.kind == model::FaultKind::dc_down ? r.datacenter : r.node;
}

// An SLO in the words of the SLO table's columns, each only when the SLO has it:
// `p99_latency c op`, `drop_rate`.
[[nodiscard]] std::string slo_words(const model::SloSpec& slo) {
  std::string words{model::to_string(slo.metric)};
  if (slo.node) words += " " + *slo.node;
  if (slo.op) words += " " + *slo.op;
  return words;
}

// An amount of the faulted part's unit (engine::FaultRecord::lost_unit) as the node table prints
// one, with its unit: `3.9k requests`, `24k MB`.
[[nodiscard]] std::string amount_text(double value, std::string_view unit) {
  return with_si_prefix(without_residue(value)) + " " + std::string{unit};
}

// A backlog or a room: a count of requests as the node table's peak_backlog prints it (`100`), a
// megabyte figure with its unit (`1k MB`).
[[nodiscard]] std::string waiting_text(double value, std::string_view unit) {
  return with_si_prefix(without_residue(value)) + (unit == "MB" ? " MB" : "");
}

// A rate of the faulted part: `400/s`, or on a byte-sized node `5k MB/s`, its capacity's unit.
[[nodiscard]] std::string rate_text(double per_second, std::string_view unit) {
  if (unit == "MB") return with_si_prefix(without_residue(per_second)) + " MB/s";
  return fmt_rate(without_residue(per_second));
}

// `fault 00:00:10 node_down s for 10s`: when (fault_clock), what, where, and how long, which a
// cache_flush, an instant, does not have (model::lasts); `for` as the scenario writes it. A fault
// that overlaps earlier ones on its target names them: `(overlaps fault 00:00:10)`.
[[nodiscard]] std::string fault_header(const FaultBlock& block) {
  const auto& r = block.record;
  std::string header = "fault " + fault_clock(r.at) + " " + std::string{model::to_string(r.kind)} +
                       " " + fault_target(r);
  if (model::lasts(r.kind)) header += " for " + describe_duration(r.duration);
  std::vector<std::string> earlier;
  for (const SimTime at : block.overlaps) earlier.push_back(fault_clock(at));
  if (!earlier.empty()) header += " (overlaps fault " + join(earlier, ", ") + ")";
  return header + "\n";
}

// One body line: two spaces, the label padded so that the three values start in one column.
[[nodiscard]] std::string fault_line(std::string_view label, const std::string& text) {
  constexpr std::size_t label_width = std::string_view{"recovery: "}.size();
  return "  " + std::string{label} + std::string(label_width - label.size(), ' ') + text + "\n";
}

// `lost 3.9k requests`, `lost 24k MB`; a dc_down keeps its cut links' requests apart, never added
// to its nodes' figure: `lost 24k MB at its nodes and 500 requests on cut links`.
[[nodiscard]] std::string lost_text(const engine::FaultRecord& r) {
  const std::string at_nodes = "lost " + amount_text(r.lost, r.lost_unit);
  if (r.kind != model::FaultKind::dc_down) return at_nodes;
  return at_nodes + " at its nodes and " + amount_text(r.lost_on_cut_links, "requests") +
         " on cut links";
}

// Whether the room filled: the backlog reached it (to float residue) and the node itself dropped.
[[nodiscard]] bool room_filled(const engine::FaultRecord& r) {
  constexpr double residue = 1e-9;
  return r.room > 0.0 && r.backlog_peak >= r.room - residue && without_residue(r.lost) > 0.0;
}

// `backlog 100 (room 100 dropped the rest)`: the parenthesis only when the room filled and dropped
// (room_filled; a dc_down that lost only on its cut links, or a traffic_spike's client, which has
// no room, prints none). Both numbers as the node table's peak_backlog, so that `backlog 9k` and
// its `room 9k` read alike.
[[nodiscard]] std::string backlog_text(const engine::FaultRecord& r) {
  const std::string backlog = "backlog " + waiting_text(r.backlog_peak, r.lost_unit);
  if (!room_filled(r)) return backlog;
  return backlog + " (room " + waiting_text(r.room, r.lost_unit) + " dropped the rest)";
}

// A flush's cost (engine::FaultRecord): the reads its empty cache sent its origin beyond the
// twin's, `extra misses 454.5 to origin`.
[[nodiscard]] std::string extra_misses_text(const engine::FaultRecord& r) {
  std::string text = "extra misses " + waiting_text(r.extra_misses, r.origin_unit);
  return r.origin.empty() ? text : text + " to " + r.origin;
}

// What the fault did to one SLO (FaultSlo): `p99_latency c op over its max for 300ms`, `drop_rate
// flipped to FAIL`, or both, `..., flipped to FAIL`.
[[nodiscard]] std::string fault_slo_text(const FaultSlo& effect,
                                         std::span<const model::SloSpec> slos) {
  std::vector<std::string> parts;
  if (effect.over_max_s > 0.0) {
    parts.push_back("over its max for " + format_duration(effect.over_max_s));
  }
  if (effect.flipped_to) {
    parts.push_back("flipped to " + std::string{status_cell(*effect.flipped_to, false).text});
  }
  return slo_words(slos[effect.slo]) + " " + join(parts, ", ");
}

// What the fault cost while it lasted: the faulted node's demand and drain, what it lost, its
// backlog (a flush: its extra misses instead, having no backlog of its own), then what it did to
// each SLO.
[[nodiscard]] std::string during_line(const FaultBlock& block,
                                      std::span<const model::SloSpec> slos) {
  const auto& r = block.record;
  std::vector<std::string> parts{"offered " + rate_text(r.offered_per_s, r.lost_unit),
                                 "served " + rate_text(r.served_per_s, r.lost_unit), lost_text(r)};
  parts.push_back(r.kind == model::FaultKind::cache_flush ? extra_misses_text(r) : backlog_text(r));
  for (const auto& effect : block.slos) parts.push_back(fault_slo_text(effect, slos));
  return fault_line("during:", join(parts, ", "));
}

// What followed: a flush's warm-up, any other fault's backlog to drain (a traffic_spike's is
// downstream, and names the node: `s backlog drained in 0.2s`).
[[nodiscard]] std::string after_text(const engine::FaultRecord& r) {
  if (r.kind == model::FaultKind::cache_flush) {
    return r.after.cache_warm_in_s
               ? "cache warm again in " + tenths_of_seconds(*r.after.cache_warm_in_s)
               : "cache not warm again by the end of the run";
  }
  if (r.after.backlog_node.empty()) return "no backlog to drain";
  const std::string backlog = r.kind == model::FaultKind::traffic_spike
                                  ? r.after.backlog_node + " backlog"
                                  : std::string{"backlog"};
  return r.after.backlog_drained_in_s
             ? backlog + " drained in " + tenths_of_seconds(*r.after.backlog_drained_in_s)
             : backlog + " not drained by the end of the run";
}

// When the system was itself again (engine::FaultRecord's recovery rule, in its words), on the
// clock and after the fault cleared (at + for), or after the flush, an instant that clears nothing.
[[nodiscard]] std::string recovery_text(const engine::FaultRecord& r) {
  if (!r.recovered_at) return "not recovered by the end of the run";
  const SimTime since_cleared{r.recovered_at->count() - r.at.count() - r.duration.count()};
  const std::string_view since = model::lasts(r.kind) ? "the fault cleared" : "the flush";
  return "back within 1% (every op's p99, every node's rho) at " + fault_clock(*r.recovered_at) +
         " (" + tenths_of_seconds(seconds(since_cleared)) + " after " + std::string{since} + ")";
}

[[nodiscard]] std::string fault_block(const FaultBlock& block,
                                      std::span<const model::SloSpec> slos) {
  return fault_header(block) + during_line(block, slos) +
         fault_line("after:", after_text(block.record)) +
         fault_line("recovery:", recovery_text(block.record));
}

// One block per fault, in `at` order; nothing without faults.
[[nodiscard]] std::string fault_blocks(const Summary& summary) {
  std::string blocks;
  for (const auto& block : summary.faults()) {
    blocks += fault_block(block, summary.graph().scenario().slos);
  }
  return blocks;
}

// Whether `earlier` was still counting when `later` struck (FaultBlock::overlaps): the same target
// (the same node, or the same datacenter), and `later`'s `at` inside `earlier`'s span from its
// `at` to its recovery (to the end of the run when it never recovered).
[[nodiscard]] bool overlaps(const engine::FaultRecord& later, const engine::FaultRecord& earlier) {
  const bool dc_down = later.kind == model::FaultKind::dc_down;
  if (dc_down != (earlier.kind == model::FaultKind::dc_down)) return false;
  if (fault_target(later) != fault_target(earlier)) return false;
  if (later.at.count() < earlier.at.count()) return false;
  return !earlier.recovered_at || later.at.count() < earlier.recovered_at->count();
}

// ---- JSON ----

// Quotes and backslashes are escaped, control characters become \u00XX; everything
// else (UTF-8 included) passes through, as JSON allows.
[[nodiscard]] std::string json_string(std::string_view text) {
  constexpr std::string_view hex = "0123456789abcdef";
  std::string out = "\"";
  for (const char c : text) {
    const unsigned code = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (code < 0x20) {
      out += "\\u00";
      out += hex[code >> 4];
      out += hex[code & 0xFu];
    } else {
      out += c;
    }
  }
  return out + '"';
}

// JSON has no infinity: a latency that never arrives is null.
[[nodiscard]] std::string json_number(double value) {
  return std::isfinite(value) ? format_number(value) : "null";
}

[[nodiscard]] std::string json_optional_number(const std::optional<double>& value) {
  return value ? json_number(*value) : "null";
}

[[nodiscard]] std::string json_optional_string(const std::optional<std::string>& text) {
  return text ? json_string(*text) : "null";
}

[[nodiscard]] std::string json_field(std::string_view name, const std::string& value) {
  return json_string(name) + ": " + value;
}

[[nodiscard]] std::string json_object(std::span<const std::string> fields) {
  return "{" + join(fields, ", ") + "}";
}

// `"name": [ ... ]` at the top level, one object per line.
[[nodiscard]] std::string json_array(std::string_view name, std::span<const std::string> objects) {
  if (objects.empty()) return "  " + json_field(name, "[]");
  return "  " + json_field(name, "[\n    " + join(objects, ",\n    ") + "\n  ]");
}

[[nodiscard]] std::string json_node(const NodeSummary& n) {
  const std::array fields{
      json_field("id", json_string(n.id)), json_field("mean_rho", json_number(n.mean_rho)),
      json_field("peak_rho", json_number(n.peak_rho)),
      json_field("peak_backlog", json_number(n.peak_backlog)),
      json_field("dropped", json_number(n.total_dropped)),
      // a worker pool's (0 without retries); null for every other node
      json_field("dead_lettered", json_optional_number(n.total_dead_lettered)),
      json_field("dead_lettered_per_s", json_optional_number(n.mean_dead_lettered_per_s)),
      json_field("rejected", json_number(n.total_rejected)),
      json_field("red_s", json_number(n.red_time_s)),
      json_field("unavailable", n.unavailable ? "true" : "false"),
      // null for a node with no lagging copies, and for an infinite one
      json_field("staleness_s", n.mean_staleness_s ? json_number(*n.mean_staleness_s) : "null"),
      // per replica and per shard; null for every node that is not a database
      json_field("replication_dropped_per_s",
                 json_optional_number(n.mean_replication_dropped_per_s)),
      json_field("committed_writes_per_s", json_optional_number(n.mean_committed_writes_per_s)),
      // seconds; null for every node that is not a message queue
      json_field("peak_queue_lag_s", json_optional_number(n.peak_queue_lag_s))};
  return json_object(fields);
}

// A bandwidth link's pipe (LinkSummary), in megabytes: its time-averaged rates and backlog, the
// fields a step's engine::LinkSample has, then its peaks and red time as a node's.
[[nodiscard]] std::string json_link(const LinkSummary& link) {
  const std::array fields{json_field("name", json_string(link.queue.id)),
                          json_field("demand_per_s", json_number(link.demand_per_s)),
                          json_field("drain_per_s", json_number(link.drain_per_s)),
                          json_field("dropped_per_s", json_number(link.dropped_per_s)),
                          json_field("backlog", json_number(link.backlog)),
                          json_field("rho", json_number(link.queue.mean_rho)),
                          json_field("latency_s", json_number(link.latency_s)),
                          json_field("zone", json_string(zone_name(link.zone))),
                          json_field("peak_rho", json_number(link.queue.peak_rho)),
                          json_field("peak_backlog", json_number(link.queue.peak_backlog)),
                          json_field("red_s", json_number(link.queue.red_time_s))};
  return json_object(fields);
}

// An op of which nothing was answered (nothing_answered) has no latency to report: null, as for a
// path that never answers (the numbers it holds are the client's waits or the failed requests'
// sojourn, not answers).
[[nodiscard]] std::string json_op(const OpSummary& op) {
  const bool no_latency = nothing_answered(op);
  const auto latency = [&](double value) { return no_latency ? "null" : json_number(value); };
  const std::array fields{
      json_field("client", json_string(op.client)),
      json_field("op", json_string(op.op)),
      json_field("offered_per_s", json_number(op.offered_per_s)),
      json_field("attempts_per_s", json_number(op.attempts_per_s)),
      json_field("answered_per_s", json_number(op.answered_per_s)),
      json_field("timed_out_per_s", json_number(op.timed_out_per_s)),
      json_field("timed_out_attempts_per_s", json_number(op.timed_out_attempts_per_s)),
      json_field("mean_s", latency(op.mean_s)),
      json_field("p50_s", latency(op.p50_s)),
      json_field("p95_s", latency(op.p95_s)),
      json_field("p99_s", latency(op.p99_s))};
  return json_object(fields);
}

// `observed` and `max` are in seconds for the time SLOs (latency, staleness, queue lag), plain
// ratios otherwise.
[[nodiscard]] std::string json_slo(const SloResult& r) {
  const std::array fields{json_field("metric", json_string(model::to_string(r.slo.metric))),
                          json_field("node", json_optional_string(r.slo.node)),
                          json_field("op", json_optional_string(r.slo.op)),
                          json_field("observed", r.observed ? json_number(*r.observed) : "null"),
                          json_field("max", json_number(limit_value(r.slo.max))),
                          json_field("status", json_string(status_name(r.status))),
                          json_field("note", r.note.empty() ? "null" : json_string(r.note))};
  if (!r.slo.over || r.at_one_instant) return json_object(fields);
  // Only an SLO with `over` that `run` judged has the key (estimate measured no window): its worst
  // window's start, in seconds from the start of the run; null when every window was skipped.
  std::vector<std::string> windowed{fields.begin(), fields.end()};
  windowed.push_back(json_field("window_start_s",
                                r.window_start ? json_number(seconds(*r.window_start)) : "null"));
  return json_object(windowed);
}

// What a fault did to the SLOs, as JSON: `flipped`, the words of each SLO whose verdict differs
// from the twin's (the run's verdict is the `slo` array's), and `over_max`, each SLO broken during
// the fault with the seconds it was.
[[nodiscard]] std::array<std::string, 2> json_fault_slos(const FaultBlock& block,
                                                         std::span<const model::SloSpec> slos) {
  std::vector<std::string> flipped;
  std::vector<std::string> over_max;
  for (const auto& effect : block.slos) {
    const std::string words = json_string(slo_words(slos[effect.slo]));
    if (effect.flipped_to) flipped.push_back(words);
    if (effect.over_max_s <= 0.0) continue;
    const std::array fields{json_field("slo", words),
                            json_field("for_s", json_number(effect.over_max_s))};
    over_max.push_back(json_object(fields));
  }
  return {json_field("flipped", "[" + join(flipped, ", ") + "]"),
          json_field("over_max", "[" + join(over_max, ", ") + "]")};
}

// The during object: the text's numbers, unrounded, in the faulted part's unit (`lost_unit`); null
// where the kind has none (`lost_on_cut_links` but for a dc_down; `room` for a flush, which builds
// no backlog of its own; `extra_misses` and `origin` but for a flush).
[[nodiscard]] std::string json_during(const FaultBlock& block,
                                      std::span<const model::SloSpec> slos) {
  const auto& r = block.record;
  const bool dc_down = r.kind == model::FaultKind::dc_down;
  const bool flush = r.kind == model::FaultKind::cache_flush;
  const auto [flipped, over_max] = json_fault_slos(block, slos);
  const std::array during{
      json_field("offered_per_s", json_number(without_residue(r.offered_per_s))),
      json_field("served_per_s", json_number(without_residue(r.served_per_s))),
      json_field("lost", json_number(without_residue(r.lost))),
      json_field("lost_unit", json_string(r.lost_unit)),
      json_field("lost_on_cut_links",
                 dc_down ? json_number(without_residue(r.lost_on_cut_links)) : "null"),
      json_field("backlog_peak", json_number(without_residue(r.backlog_peak))),
      json_field("room", flush ? "null" : json_number(r.room)),
      json_field("extra_misses", flush ? json_number(without_residue(r.extra_misses)) : "null"),
      json_field("origin", flush ? json_string(r.origin) : "null"),
      flipped,
      over_max};
  return json_object(during);
}

// A fault's block as JSON (the text's numbers, unrounded, in seconds): `for_s` null for a flush,
// `overlaps_at_s` the earlier faults it overlaps, each `after` entry null where it does not apply
// or did not happen, `recovered_at_s` null when the run ended first; a dc_down names its
// datacenter (`node` null).
[[nodiscard]] std::string json_fault(const FaultBlock& block,
                                     std::span<const model::SloSpec> slos) {
  const auto& r = block.record;
  std::vector<std::string> overlaps;
  for (const SimTime at : block.overlaps) overlaps.push_back(json_number(seconds(at)));
  const std::array after{
      json_field("backlog_node",
                 r.after.backlog_node.empty() ? "null" : json_string(r.after.backlog_node)),
      json_field("backlog_drained_in_s", json_optional_number(r.after.backlog_drained_in_s)),
      json_field("cache_warm_in_s", json_optional_number(r.after.cache_warm_in_s))};
  const bool dc_down = r.kind == model::FaultKind::dc_down;
  std::vector<std::string> fields{json_field("at_s", json_number(seconds(r.at))),
                                  json_field("kind", json_string(model::to_string(r.kind))),
                                  json_field("node", dc_down ? "null" : json_string(r.node))};
  if (dc_down) fields.push_back(json_field("datacenter", json_string(r.datacenter)));
  fields.push_back(
      json_field("for_s", model::lasts(r.kind) ? json_number(seconds(r.duration)) : "null"));
  fields.push_back(json_field("overlaps_at_s", "[" + join(overlaps, ", ") + "]"));
  fields.push_back(json_field("during", json_during(block, slos)));
  fields.push_back(json_field("after", json_object(after)));
  fields.push_back(json_field("recovered_at_s", json_optional_number(r.recovered_at.transform(
                                                    [](SimTime t) { return seconds(t); }))));
  return json_object(fields);
}

// C++20 std::views::transform + C++23 std::ranges::to: map every element through `f` and
// collect the results into a vector, replacing a reserve + push_back loop.
template <class T, class F>
[[nodiscard]] std::vector<std::string> map_to_strings(std::span<const T> items, F f) {
  return items | std::views::transform(f) | std::ranges::to<std::vector<std::string>>();
}

// The top-level lines of a run's JSON report, one key each. With no observed step the drop and
// reject rates are null: there is no data, not a rate of 0.
[[nodiscard]] std::vector<std::string> json_report_lines(const Summary& summary,
                                                         std::span<const SloResult> slos) {
  const auto nodes = summary.nodes();
  const auto ops = summary.ops();
  const auto links = summary.links();
  const bool observed = summary.observed_steps() > 0;
  const auto worst = summary.worst_drop_node();
  const auto worst_rejecting = summary.worst_reject_node();
  std::vector<std::string> lines{
      "  " + json_field("scenario", json_string(summary.scenario_name())),
      "  " + json_field("steps", std::to_string(summary.observed_steps())),
      "  " + json_field("drop_rate", observed ? json_number(summary.drop_rate()) : "null"),
      "  " + json_field("worst_drop_node", worst.empty() ? "null" : json_string(worst)),
      "  " + json_field("reject_rate", observed ? json_number(summary.reject_rate()) : "null"),
      "  " + json_field("worst_reject_node",
                        worst_rejecting.empty() ? "null" : json_string(worst_rejecting)),
      json_array("nodes", map_to_strings(std::span<const NodeSummary>{nodes}, json_node)),
      json_array("links", map_to_strings(std::span<const LinkSummary>{links}, json_link)),
      json_array("ops", map_to_strings(std::span<const OpSummary>{ops}, json_op)),
      json_array("slo", map_to_strings(slos, json_slo))};
  if (summary.faults().empty()) return lines;
  // The faults array, last and only with faults: a report without them is what it was before.
  std::vector<std::string> faults;
  for (const auto& block : summary.faults()) {
    faults.push_back(json_fault(block, summary.graph().scenario().slos));
  }
  lines.push_back(json_array("faults", faults));
  return lines;
}

// The lines as one JSON object, a key per line.
[[nodiscard]] std::string json_document(std::span<const std::string> lines) {
  return "{\n" + join(lines, ",\n") + "\n}";
}

// ---- replications (`run --replications`: report.cppm says what the cells and the verdicts are) --

// The band's dash, U+2013 EN DASH in UTF-8: the typographic sign of a range (`1.61–1.63`). Three
// bytes and one column, which the aligner counts as one (display_width).
constexpr std::string_view en_dash = "–";

// A cell's number split from its unit: `1.62ms` is 1.62 and `ms`, `0.105` is 0.105 and nothing,
// `1.5k` is 1.5 and `k`. Empty for a text that does not start with a number (`unavailable`, `-`,
// `>1s`, `<1us`).
struct NumberAndUnit {
  std::string_view number;
  std::string_view unit;
};

[[nodiscard]] std::optional<NumberAndUnit> split_unit(std::string_view text) {
  double ignored{};
  // std::from_chars (C++17): reads the longest number at the start of the text, locale-free, and
  // says where it stopped: what follows is the unit the formatters appended.
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), ignored);
  if (error != std::errc{} || end == text.data()) return std::nullopt;
  const auto length = static_cast<std::size_t>(end - text.data());
  return NumberAndUnit{.number = text.substr(0, length), .unit = text.substr(length)};
}

// 1-based rank of the nearest-rank `percent` quantile of n values: ceil(percent·n / 100), at least
// 1. Integer arithmetic, so no float rounding can move a rank (0.1 × 10 is not exactly 1 in
// binary floating point).
[[nodiscard]] std::size_t nearest_rank(std::size_t percent, std::size_t n) {
  return std::max<std::size_t>((percent * n + 99) / 100, 1);
}

// Does a run's value `a` rank before another's `b`? Ascending, and NaN (a run that observed
// nothing, or a cell of text) after every number.
[[nodiscard]] bool ranks_before(double a, double b) {
  return !std::isnan(a) && (std::isnan(b) || a < b);
}

// The runs' indices in ascending order of their values, equal values in run order.
[[nodiscard]] std::vector<std::size_t> rank_order(std::span<const double> values) {
  std::vector<std::size_t> order(values.size());
  // C++23 std::ranges::iota: fills the range with 0, 1, 2, ..., the range itself instead of
  // std::iota's begin/end pair.
  std::ranges::iota(order, std::size_t{0});
  // std::ranges::stable_sort: stable, so runs with equal values keep their order and a tie always
  // names the lower seed, whatever the threads did.
  std::ranges::stable_sort(
      order, [&](std::size_t a, std::size_t b) { return ranks_before(values[a], values[b]); });
  return order;
}

// The median and the 10-90 % band of `values` (one per run), nearest rank (report.cppm).
[[nodiscard]] Band band_of(std::span<const double> values) {
  const auto order = rank_order(values);
  const auto run_at = [&](std::size_t percent) {
    return order[nearest_rank(percent, order.size()) - 1];
  };
  const std::size_t low = run_at(10);
  const std::size_t middle = run_at(50);
  const std::size_t high = run_at(90);
  return Band{.median = values[middle],
              .low = values[low],
              .high = values[high],
              .median_run = middle,
              .low_run = low,
              .high_run = high};
}

// What a run's report is made of, computed once per run: its ops, its nodes and its SLO verdicts.
struct RunFigures {
  std::vector<std::vector<OpSummary>> ops{};
  std::vector<std::vector<NodeSummary>> nodes{};
  std::vector<std::vector<SloResult>> slos{};
};

// Throws std::invalid_argument unless every run has the first run's ops, nodes and SLOs: runs of
// different scenarios cannot be ranked against each other (a programmer error).
void require_one_scenario(const RunFigures& figures) {
  for (std::size_t run = 1; run < figures.ops.size(); ++run) {
    if (figures.ops[run].size() != figures.ops[0].size() ||
        figures.nodes[run].size() != figures.nodes[0].size() ||
        figures.slos[run].size() != figures.slos[0].size()) {
      throw std::invalid_argument{"report: replications of different scenarios"};
    }
  }
}

[[nodiscard]] RunFigures figures_of(std::span<const Summary> runs) {
  RunFigures figures;
  for (const auto& run : runs) {
    figures.ops.push_back(run.ops());
    figures.nodes.push_back(run.nodes());
    figures.slos.push_back(evaluate_slos(run, run.graph().scenario().slos));
  }
  require_one_scenario(figures);
  return figures;
}

// The band of one field of item `index` across the runs (`per_run[r][index].*field`).
// A pointer to a data member (`double OpSummary::*`, C++98) names a field of a type, not of an
// object: `item.*field` reads that field of `item`, so one function serves every column.
template <class Item>
[[nodiscard]] Band band_across(std::span<const std::vector<Item>> per_run, std::size_t index,
                               double Item::*field) {
  std::vector<double> values;
  for (const auto& run : per_run) values.push_back(run[index].*field);
  return band_of(values);
}

[[nodiscard]] OpBands op_bands(std::span<const std::vector<OpSummary>> ops, std::size_t i) {
  const auto band = [&](double OpSummary::*field) { return band_across(ops, i, field); };
  return OpBands{.client = ops.front()[i].client,
                 .op = ops.front()[i].op,
                 .offered_per_s = band(&OpSummary::offered_per_s),
                 .attempts_per_s = band(&OpSummary::attempts_per_s),
                 .answered_per_s = band(&OpSummary::answered_per_s),
                 .timed_out_per_s = band(&OpSummary::timed_out_per_s),
                 .timed_out_attempts_per_s = band(&OpSummary::timed_out_attempts_per_s),
                 .mean_s = band(&OpSummary::mean_s),
                 .p50_s = band(&OpSummary::p50_s),
                 .p95_s = band(&OpSummary::p95_s),
                 .p99_s = band(&OpSummary::p99_s)};
}

// The band of a field only some node types have (a message queue's peak lag, a worker pool's dead
// letters) across the runs; none for another node (no run has one).
[[nodiscard]] std::optional<Band> optional_band(std::span<const std::vector<NodeSummary>> nodes,
                                                std::size_t i,
                                                std::optional<double> NodeSummary::*field) {
  if (!(nodes.front()[i].*field)) return std::nullopt;
  std::vector<double> values;
  for (const auto& run : nodes) values.push_back((run[i].*field).value_or(text_cell));
  return band_of(values);
}

[[nodiscard]] NodeBands node_bands(std::span<const std::vector<NodeSummary>> nodes, std::size_t i) {
  const auto band = [&](double NodeSummary::*field) { return band_across(nodes, i, field); };
  // C++20 std::ranges::count_if: counts over the whole range, replacing a begin/end pair.
  const auto unavailable = std::ranges::count_if(
      nodes, [&](const std::vector<NodeSummary>& run) { return run[i].unavailable; });
  return NodeBands{
      .id = nodes.front()[i].id,
      .mean_rho = band(&NodeSummary::mean_rho),
      .peak_rho = band(&NodeSummary::peak_rho),
      .peak_backlog = band(&NodeSummary::peak_backlog),
      .dropped = band(&NodeSummary::total_dropped),
      .rejected = band(&NodeSummary::total_rejected),
      .red_s = band(&NodeSummary::red_time_s),
      .dead_lettered_per_s = optional_band(nodes, i, &NodeSummary::mean_dead_lettered_per_s),
      .peak_queue_lag_s = optional_band(nodes, i, &NodeSummary::peak_queue_lag_s),
      .unavailable_in = static_cast<std::size_t>(unavailable)};
}

// SLO `j` across the runs: its median run is the run of the median observed value (a run that
// observed nothing ranks last), and the replicated verdict is that run's.
[[nodiscard]] SloReplication slo_replication(std::span<const std::vector<SloResult>> slos,
                                             std::size_t j) {
  std::vector<double> observed;
  std::size_t passes = 0;
  for (const auto& run : slos) {
    observed.push_back(run[j].observed.value_or(text_cell));
    if (run[j].status == SloStatus::pass) ++passes;
  }
  const std::size_t median_run = band_of(observed).median_run;
  return SloReplication{
      .result = slos[median_run][j], .passes_in = passes, .median_run = median_run};
}

// The run that stands for all (ReplicationSummary::median_run): the first SLO's median run, else
// the first op's mean latency's, else the middle seed.
[[nodiscard]] std::size_t standing_run(const ReplicationSummary& replicated) {
  if (!replicated.slos.empty()) return replicated.slos.front().median_run;
  if (!replicated.ops.empty()) return replicated.ops.front().mean_s.median_run;
  return nearest_rank(50, replicated.seeds.size()) - 1;
}

// The median run's Summary; `runs` must be the runs `replicated` summarises.
[[nodiscard]] const Summary& median_summary(const ReplicationSummary& replicated,
                                            std::span<const Summary> runs) {
  if (runs.size() != replicated.seeds.size()) {
    throw std::invalid_argument{"report: the runs are not the replications summarised"};
  }
  return runs[replicated.median_run];
}

// ---- replications: the table ----

// The runs' numbers in one cell of one table.
[[nodiscard]] std::vector<double> cell_numbers(std::span<const TableParts> runs, std::size_t row,
                                               std::size_t cell) {
  std::vector<double> numbers;
  for (const auto& run : runs) numbers.push_back(run.numbers[row][cell]);
  return numbers;
}

[[nodiscard]] bool is_text(double number) {
  return std::isnan(number);
}

// A cell that prints text in every run: that text when every run printed the same (a name, a
// client's `-`); else the first mark that is not blank and how many runs printed it (`unavailable
// in 2 of 5`), the one text cell that varies.
[[nodiscard]] Cell text_band(std::span<const TableParts> runs, std::size_t row, std::size_t cell) {
  const std::string* mark = nullptr;
  for (const auto& run : runs) {
    const auto& text = run.rows[row][cell].text;
    if (!text.empty() && mark == nullptr) mark = &text;
  }
  const auto& first = runs.front().rows[row][cell];
  if (mark == nullptr) return first;
  std::size_t shown = 0;
  for (const auto& run : runs) {
    if (run.rows[row][cell].text == *mark) ++shown;
  }
  if (shown == runs.size()) return first;
  return {*mark + " in " + std::to_string(shown) + " of " + std::to_string(runs.size())};
}

// A cell across the runs: format_band of the texts the median, the 10 % and the 90 % runs printed
// in it (each run's own text, so a `>1s` or a `-` shows as that run printed it), coloured as the
// median run's.
[[nodiscard]] Cell banded_cell(std::span<const TableParts> runs, std::size_t row,
                               std::size_t cell) {
  const auto numbers = cell_numbers(runs, row, cell);
  if (std::ranges::all_of(numbers, is_text)) return text_band(runs, row, cell);
  const Band band = band_of(numbers);
  const Cell& median = runs[band.median_run].rows[row][cell];
  return {format_band(median.text, runs[band.low_run].rows[row][cell].text,
                      runs[band.high_run].rows[row][cell].text),
          median.colour};
}

// Throws std::invalid_argument unless every run's table has the first one's rows and cells.
void require_same_shape(std::span<const TableParts> runs) {
  for (const auto& run : runs) {
    bool same = run.rows.size() == runs.front().rows.size();
    for (std::size_t row = 0; same && row < run.rows.size(); ++row) {
      same = run.rows[row].size() == runs.front().rows[row].size();
    }
    if (!same) throw std::invalid_argument{"report: replications of different scenarios"};
  }
}

// One table of every run made one: the same header, each cell banded across the runs.
[[nodiscard]] TableParts banded_parts(std::span<const TableParts> runs) {
  require_same_shape(runs);
  TableParts banded{.header = runs.front().header};
  for (std::size_t row = 0; row < runs.front().rows.size(); ++row) {
    Row cells;
    for (std::size_t cell = 0; cell < runs.front().rows[row].size(); ++cell) {
      cells.push_back(banded_cell(runs, row, cell));
    }
    banded.add(std::move(cells), runs.front().numbers[row]);
  }
  return banded;
}

// `build(run)` for every run, banded.
template <class Build>
[[nodiscard]] TableParts banded_over(std::span<const Summary> runs, const Build& build) {
  std::vector<TableParts> per_run;
  for (const auto& run : runs) per_run.push_back(build(run));
  return banded_parts(per_run);
}

// The node, link, database and op tables of a run, every number a band.
[[nodiscard]] std::string banded_tables(std::span<const Summary> runs, bool color) {
  const auto nodes = [&](const Summary& run) { return node_parts(run, color); };
  const auto links = [&](const Summary& run) { return link_parts(run, color); };
  return table_text(node_title, banded_over(runs, nodes)) +
         table_text_if_any(link_title, banded_over(runs, links)) +
         table_text_if_any(replication_title, banded_over(runs, replication_parts)) +
         table_text(op_title, banded_over(runs, op_parts));
}

// What the cells are, and which seed the median run is.
[[nodiscard]] std::string replications_intro(const ReplicationSummary& replicated) {
  const auto& seeds = replicated.seeds;
  return "replications: " + std::to_string(seeds.size()) + " runs, seeds " +
         std::to_string(seeds.front()) + ".." + std::to_string(seeds.back()) +
         "; a cell is the median across the runs [its 10" + std::string{en_dash} +
         "90 % band], nearest rank\n" + "median run: seed " +
         std::to_string(seeds[replicated.median_run]) +
         " (the line below and the fault blocks are its own)\n";
}

// `PASS in 3 of 5`: the median run passed, as did 3 of the 5 runs. `FAIL (passed in 2 of 5)`: the
// median run failed, 2 runs passed. The count is always of the runs that passed.
[[nodiscard]] Cell replicated_status_cell(const SloReplication& slo, std::size_t runs, bool color) {
  Cell cell = status_cell(slo.result.status, color);
  const std::string count = std::to_string(slo.passes_in) + " of " + std::to_string(runs);
  cell.text += slo.result.status == SloStatus::pass ? " in " + count : " (passed in " + count + ")";
  return cell;
}

[[nodiscard]] std::string replicated_slo_table(const ReplicationSummary& replicated, bool color) {
  if (replicated.slos.empty()) return {};
  std::vector<Row> rows{slo_header()};
  for (const auto& slo : replicated.slos) {
    rows.push_back(
        slo_row(slo.result, replicated_status_cell(slo, replicated.seeds.size(), color)));
  }
  return "slo (each: its median run's verdict and observed value, and the runs that passed it)\n" +
         aligned(rows, Align::all_left);
}

// ---- replications: the JSON ----

// `"name": [v0, v1, ...]`, one entry per seed.
[[nodiscard]] std::string json_per_seed(std::string_view name,
                                        std::span<const std::string> values) {
  return json_field(name, "[" + join(values, ", ") + "]");
}

// `"name": [ ... ]` nested at `indent`, one object per line.
[[nodiscard]] std::string json_nested_array(std::string_view name,
                                            std::span<const std::string> objects,
                                            const std::string& indent) {
  if (objects.empty()) return indent + json_field(name, "[]");
  const std::string inner = indent + "  ";
  return indent +
         json_field(name, "[\n" + inner + join(objects, ",\n" + inner) + "\n" + indent + "]");
}

// The numbers of json_op, in its order, and whether each is a latency (null when nothing was
// answered, as json_op writes it).
struct OpNumber {
  std::string_view name;
  double OpSummary::*field;
  bool latency;
};
constexpr std::array op_numbers{
    OpNumber{"offered_per_s", &OpSummary::offered_per_s, false},
    OpNumber{"attempts_per_s", &OpSummary::attempts_per_s, false},
    OpNumber{"answered_per_s", &OpSummary::answered_per_s, false},
    OpNumber{"timed_out_per_s", &OpSummary::timed_out_per_s, false},
    OpNumber{"timed_out_attempts_per_s", &OpSummary::timed_out_attempts_per_s, false},
    OpNumber{"mean_s", &OpSummary::mean_s, true},
    OpNumber{"p50_s", &OpSummary::p50_s, true},
    OpNumber{"p95_s", &OpSummary::p95_s, true},
    OpNumber{"p99_s", &OpSummary::p99_s, true}};

[[nodiscard]] std::string json_op_per_seed(std::span<const std::vector<OpSummary>> ops,
                                           std::size_t i) {
  std::vector<std::string> fields{json_field("client", json_string(ops.front()[i].client)),
                                  json_field("op", json_string(ops.front()[i].op))};
  for (const auto& number : op_numbers) {
    std::vector<std::string> values;
    for (const auto& run : ops) {
      const auto& op = run[i];
      values.push_back(number.latency && nothing_answered(op) ? "null"
                                                              : json_number(op.*number.field));
    }
    fields.push_back(json_per_seed(number.name, values));
  }
  return json_object(fields);
}

// One node's numbers per seed, in json_node's order: each a list with an entry per seed, written as
// json_node writes it (a number; null where an optional field has none, as it is for the node types
// that do not have it).
[[nodiscard]] std::string json_node_per_seed(std::span<const std::vector<NodeSummary>> nodes,
                                             std::size_t i) {
  const auto plain = [&](std::string_view name, double NodeSummary::*field) {
    std::vector<std::string> values;
    for (const auto& run : nodes) values.push_back(json_number(run[i].*field));
    return json_per_seed(name, values);
  };
  const auto optional = [&](std::string_view name, std::optional<double> NodeSummary::*field) {
    std::vector<std::string> values;
    for (const auto& run : nodes) values.push_back(json_optional_number(run[i].*field));
    return json_per_seed(name, values);
  };
  std::vector<std::string> unavailable;
  for (const auto& run : nodes) unavailable.emplace_back(run[i].unavailable ? "true" : "false");
  const std::array fields{
      json_field("id", json_string(nodes.front()[i].id)),
      plain("mean_rho", &NodeSummary::mean_rho),
      plain("peak_rho", &NodeSummary::peak_rho),
      plain("peak_backlog", &NodeSummary::peak_backlog),
      plain("dropped", &NodeSummary::total_dropped),
      optional("dead_lettered", &NodeSummary::total_dead_lettered),
      optional("dead_lettered_per_s", &NodeSummary::mean_dead_lettered_per_s),
      plain("rejected", &NodeSummary::total_rejected),
      plain("red_s", &NodeSummary::red_time_s),
      json_per_seed("unavailable", unavailable),
      optional("staleness_s", &NodeSummary::mean_staleness_s),
      optional("replication_dropped_per_s", &NodeSummary::mean_replication_dropped_per_s),
      optional("committed_writes_per_s", &NodeSummary::mean_committed_writes_per_s),
      optional("peak_queue_lag_s", &NodeSummary::peak_queue_lag_s)};
  return json_object(fields);
}

[[nodiscard]] std::string json_slo_per_seed(std::span<const std::vector<SloResult>> slos,
                                            std::size_t j) {
  const auto& slo = slos.front()[j].slo;
  std::vector<std::string> observed;
  std::vector<std::string> status;
  for (const auto& run : slos) {
    observed.push_back(json_optional_number(run[j].observed));
    status.push_back(json_string(status_name(run[j].status)));
  }
  const std::array fields{json_field("metric", json_string(model::to_string(slo.metric))),
                          json_field("node", json_optional_string(slo.node)),
                          json_field("op", json_optional_string(slo.op)),
                          json_per_seed("observed", observed), json_per_seed("status", status)};
  return json_object(fields);
}

// `per_seed`'s three lists, one object per op, node and SLO.
[[nodiscard]] std::string json_per_seed_lists(const RunFigures& figures) {
  std::vector<std::string> ops;
  for (std::size_t i = 0; i < figures.ops.front().size(); ++i) {
    ops.push_back(json_op_per_seed(figures.ops, i));
  }
  std::vector<std::string> nodes;
  for (std::size_t i = 0; i < figures.nodes.front().size(); ++i) {
    nodes.push_back(json_node_per_seed(figures.nodes, i));
  }
  std::vector<std::string> slos;
  for (std::size_t j = 0; j < figures.slos.front().size(); ++j) {
    slos.push_back(json_slo_per_seed(figures.slos, j));
  }
  const std::string indent = "      ";
  return "{\n" + json_nested_array("ops", ops, indent) + ",\n" +
         json_nested_array("nodes", nodes, indent) + ",\n" +
         json_nested_array("slo", slos, indent) + "\n    }";
}

// The `replications` key: the seeds, and every op's, node's and SLO's numbers per seed.
[[nodiscard]] std::string json_replications(const ReplicationSummary& replicated,
                                            const RunFigures& figures) {
  std::vector<std::string> seeds;
  for (const int seed : replicated.seeds) seeds.push_back(std::to_string(seed));
  return "  " + json_field("replications",
                           "{\n    " + json_per_seed("seeds", seeds) + ",\n    " +
                               json_field("per_seed", json_per_seed_lists(figures)) + "\n  }");
}

// ---- CSV ----

[[nodiscard]] std::string csv_server_row(const std::string& t_s, const std::string& id,
                                         const engine::NodeSample& n) {
  const std::array fields{
      t_s,
      id,
      significant(n.demand_per_s, 6),
      significant(n.drain_per_s, 6),
      significant(n.rho, 6),
      significant(n.backlog, 6),
      significant(n.dropped_per_s, 6),
      n.dead_lettered_per_s ? significant(*n.dead_lettered_per_s, 6) : std::string{},
      significant(n.rejected_per_s, 6),
      significant(n.latency.mean_s * 1000.0, 6),
      std::string{zone_name(n.zone)},
      n.queue_lag_s ? significant(*n.queue_lag_s, 6) : std::string{}};
  return join(fields, ",") + "\n";
}

// A bandwidth link's pipe, in the same twelve columns as a node's, in megabytes;
// `dead_lettered_per_s` empty (a pipe is no worker pool), `rejected_per_s` 0 (a pipe refuses
// nothing by policy) and `queue_lag_s` empty (a pipe is no message queue).
[[nodiscard]] std::string csv_link_row(const std::string& t_s, const std::string& name,
                                       const engine::LinkSample& l) {
  const std::array fields{t_s,
                          name,
                          significant(l.demand_per_s, 6),
                          significant(l.drain_per_s, 6),
                          significant(l.rho, 6),
                          significant(l.backlog, 6),
                          significant(l.dropped_per_s, 6),
                          std::string{},
                          std::string{"0"},
                          significant(l.latency.mean_s * 1000.0, 6),
                          std::string{zone_name(l.zone)},
                          std::string{}};
  return join(fields, ",") + "\n";
}

// A client is a source: no queue, so zeros where a server has numbers, zone `none`, and no dead
// letters (a pool's) and no lag (a queue's).
[[nodiscard]] std::string csv_client_row(const std::string& t_s, const std::string& id) {
  return t_s + "," + id + ",0,0,0,0,0,,0,0,none,\n";
}

// One (client, op) of a step: what it offered, what it sent (its retries included), what came back
// answered, what came back after the client's timeout (0 without one), and the mean response of an
// answered request in milliseconds (`inf` when the path never answers).
[[nodiscard]] std::string csv_op_row(const std::string& t_s, const std::string& client,
                                     const engine::ResponseSample& r) {
  const std::array fields{t_s,
                          client,
                          r.op,
                          significant(r.offered.per_second, 6),
                          significant(r.attempts.per_second, 6),
                          significant(r.answered.per_second, 6),
                          significant(r.timed_out.per_second, 6),
                          significant(r.response.mean_s * 1000.0, 6)};
  return join(fields, ",") + "\n";
}

// Float residue is no quantity. A queue that fills and drains back to empty can be left with
// |x| ~ 1e-13 where the exact figure is 0 (the subtractions of its conservation law do not cancel
// to the last bit): a pool behind a saturated service printed `peak_backlog 1.819e-13`. A backlog,
// drop or rejection total below 1e-9 of what the queue serves in one second (its capacity per
// replica; an absolute 1e-9 for a queue with none) is reported as 0, so nobody goes looking for a
// backlog that is not there. The totals the summary keeps are not touched, only the figures it
// hands the reports (Summary::nodes, Summary::links).
constexpr double residue_per_capacity = 1e-9;

[[nodiscard]] double without_residue(double value, double capacity_per_s) {
  const double threshold =
      capacity_per_s > 0.0 ? residue_per_capacity * capacity_per_s : residue_per_capacity;
  return std::abs(value) < threshold ? 0.0 : value;
}

}  // namespace

// ---- Summary ----

// Weighted moments of a mixture: each step contributes its mean m and variance v with
// weight w (requests answered in that step), so E[X] = Σ w·m / Σ w and
// E[X²] = Σ w·(v + m²) / Σ w (E[X²] of one component is its variance plus its squared mean).
void Summary::Moments::add(double w, double mean_s, double variance_s2) {
  weight += w;
  weighted_mean += w * mean_s;
  weighted_square += w * (variance_s2 + mean_s * mean_s);
}

double Summary::Moments::mean_s() const {
  return weighted_mean / weight;
}

// Law of total variance over the steps: Var X = E[X²] − (E[X])², where E[X²] already holds each
// step's own spread (v) and the spread between step means (m²). Assumes a request's step is chosen
// in proportion to the weight (requests) the step added.
double Summary::Moments::variance_s2() const {
  const double mean = mean_s();
  return weighted_square / weight - mean * mean;
}

// Demand, drops and backlog arrive in capacity units (engine::NodeSample): a request of
// an op with `cost` 3 counts three times. The tables keep their column names; with every
// cost at its default of 1, units are requests. A node's drop rate is therefore the share of the
// work it was sent that it shed, the same ratio when costs are uniform.
void Summary::NodeTotals::add(const engine::NodeSample& sample, double dt_s) {
  rho_time += sample.rho * dt_s;
  demand += sample.demand_per_s * dt_s;
  peak_rho = std::max(peak_rho, sample.rho);
  peak_backlog = std::max(peak_backlog, sample.backlog);
  dropped += sample.dropped_per_s * dt_s;
  rejected += sample.rejected_per_s * dt_s;
  if (sample.zone == fluid::Zone::red) red_time_s += dt_s;
  // Infinite only at a node with no capacity of its own (nominal 0): one that a fault took down
  // waits finitely for it to come back (fluid::Derating), so an outage never flags it.
  if (!std::isfinite(sample.latency.mean_s)) unavailable = true;
  if (sample.staleness_s) {
    staleness_time += *sample.staleness_s * dt_s;  // an infinite staleness stays infinite
    staleness_seen_s += dt_s;
  }
  if (sample.replication_dropped_per_s) {  // a database: it reports both, always together
    replication_dropped += *sample.replication_dropped_per_s * dt_s;
    committed_writes += sample.committed_writes_per_s.value_or(0.0) * dt_s;
    replication_seen_s += dt_s;
  }
  if (sample.queue_lag_s) {  // a message queue: its worst moment is what a lag bound bounds
    // flow::nan_propagating_max: a NaN step stays NaN for the run, where std::max would let the
    // next step's finite lag replace it.
    peak_queue_lag_s =
        flow::nan_propagating_max(*sample.queue_lag_s, peak_queue_lag_s.value_or(0.0));
  }
  if (sample.dead_lettered_per_s) {  // a worker pool: 0 without retries
    dead_lettered = dead_lettered.value_or(0.0) + *sample.dead_lettered_per_s * dt_s;
  }
  if (sample.republished_per_s) {  // a message queue: 0 when its groups retry nothing
    republished = republished.value_or(0.0) + *sample.republished_per_s * dt_s;
  }
  // A sink's drops fall on every caller alike (it fails a share of all it serves), so the
  // retrying callers' part of them is their part of its demand, this step.
  if (sample.retried_demand_per_s && sample.demand_per_s > 0.0) {
    retried_dropped +=
        sample.dropped_per_s * (*sample.retried_demand_per_s / sample.demand_per_s) * dt_s;
  }
}

// A step whose response is infinite (a node on the path never answers) poisons the whole op: no
// finite average describes it. It is flagged instead of accumulated, so no 0 × inf = NaN can reach
// the sums. A step that delivered nothing (answered + timed-out attempts = 0: every request it sent
// was dropped or refused, or it sent none) adds its counts and nothing else: no request saw its
// latency, so it weighs no moment and flags nothing, whatever the latency a request WOULD have seen
// (phase 3 Task 6's review: a node that is down drops what its full room cannot hold, and those
// steps must not decide the op's latency for the whole run).
void Summary::OpTotals::add(const engine::ResponseSample& sample, double dt_s) {
  offered += sample.offered.per_second * dt_s;
  attempts += sample.attempts.per_second * dt_s;
  answered += sample.answered.per_second * dt_s;
  timed_out += sample.timed_out.per_second * dt_s;
  timed_out_attempts += sample.timed_out_attempts.per_second * dt_s;
  const double delivered_per_s = sample.answered.per_second + sample.timed_out_attempts.per_second;
  if (!(delivered_per_s > 0.0)) return;
  const auto& response = sample.response;
  if (!std::isfinite(response.mean_s) || !std::isfinite(response.variance_s2)) {
    unavailable = true;
    return;
  }
  // Weighted by the requests ANSWERED (spec §17 entry 8): a request a limiter refused or a full
  // queue dropped never saw this latency, so it must not weigh it. In a step where a limiter
  // refuses 3 of 4 requests, only the admitted quarter counts.
  by_answered.add(sample.answered.per_second * dt_s, response.mean_s, response.variance_s2);
  by_time.add(dt_s, response.mean_s, response.variance_s2);
  // Weighted by the requests DELIVERED: the client's quantiles range over every request that came
  // back, the ones it gave up on included.
  const auto& delivered = sample.delivered_response;
  by_delivered.add(delivered_per_s * dt_s, delivered.mean_s, delivered.variance_s2);
}

// A bandwidth link's pipe is a queue like a node's (engine::LinkSample has NodeSample's fields of
// the same names, in megabytes), so its node-table columns accumulate exactly as a node's do
// (NodeTotals::add); a pipe never rejects. What only the link's JSON shows comes beside them.
void Summary::LinkTotals::add(const engine::LinkSample& sample, double dt_s) {
  queue.add(engine::NodeSample{.demand_per_s = sample.demand_per_s,
                               .drain_per_s = sample.drain_per_s,
                               .dropped_per_s = sample.dropped_per_s,
                               .backlog = sample.backlog,
                               .rho = sample.rho,
                               .latency = sample.latency,
                               .zone = sample.zone},
            dt_s);
  drained += sample.drain_per_s * dt_s;
  backlog_time += sample.backlog * dt_s;
  by_demand.add(sample.demand_per_s * dt_s, sample.latency.mean_s, sample.latency.variance_s2);
  by_time.add(dt_s, sample.latency.mean_s, sample.latency.variance_s2);
  worst_zone = std::max(worst_zone, sample.zone);  // green < yellow < red, as declared
}

Summary::Summary(const graph::Graph& g, SimTime warmup) : Summary{g, warmup, true} {}

Summary Summary::of_estimate(const graph::Graph& g, const engine::StepRecord& record) {
  Summary summary{g, SimTime{0}, false};
  summary.estimate_ = true;
  summary.observe(record);
  return summary;
}

Summary::Summary(const graph::Graph& g, SimTime warmup, bool track_windows)
    : graph_{&g}, warmup_{warmup}, nodes_(g.nodes().size()) {
  for (const auto& link : g.links()) {
    if (link.pipe) links_.push_back(LinkTotals{.name = graph::link_label(g.nodes(), link)});
  }
  // One entry per (client, op), in the engine's order: clients by node index, ops by name.
  for (std::size_t client = 0; client < g.nodes().size(); ++client) {
    const auto& workload = g.nodes()[client].spec.workload;
    if (!workload) continue;
    std::vector<std::string> ops;
    for (const auto& [op, share] : workload->op_shares) ops.push_back(op);
    std::ranges::sort(ops);
    for (auto& op : ops) ops_.push_back(OpTotals{.client = client, .op = std::move(op)});
  }
  if (!track_windows) return;
  for (const auto& slo : g.scenario().slos) {
    if (slo.over) windows_.push_back(WindowedSlo{.slo = slo});
  }
}

// Step records carry only their start time, so a step's length is the gap to the
// previous record; the first record (or one that does not follow the last, such as
// Simulation::estimate's) uses the scenario's step. `.count()` arithmetic: both are
// SimTime, so the tick counts are in the same unit (nanoseconds).
double Summary::step_length_s(SimTime t) {
  const bool follows = last_t_ && t.count() > last_t_->count();
  const SimTime dt = follows ? SimTime{t.count() - last_t_->count()} : graph_->scenario().step;
  last_t_ = t;
  return seconds(dt);
}

Summary::OpTotals& Summary::totals_for(const engine::ResponseSample& sample, std::size_t hint) {
  const auto matches = [&](const OpTotals& t) {
    return t.client == sample.client && t.op == sample.op;
  };
  if (hint < ops_.size() && matches(ops_[hint])) return ops_[hint];  // the usual case
  const auto found = std::ranges::find_if(ops_, matches);
  if (found != ops_.end()) return *found;
  return ops_.emplace_back(OpTotals{.client = sample.client, .op = sample.op});
}

// A fault's window "during" (engine::FaultRecord): [at, at + for), a flush's warm-up [at, at +
// warm_up_s), or from `at` to the end of the run when its cache never warms.
Summary::FaultWindow Summary::window_of(const engine::FaultRecord& record) {
  if (model::lasts(record.kind)) {
    return {.start = record.at, .end = SimTime{record.at.count() + record.duration.count()}};
  }
  if (!record.warm_up_s) return {.start = record.at, .end = std::nullopt};
  return {.start = record.at,
          .end = SimTime{record.at.count() + from_seconds(*record.warm_up_s).count()}};
}

void Summary::watch_faults(std::span<const engine::FaultRecord> records) {
  watched_.clear();
  for (const auto& record : records) watched_.push_back(window_of(record));
}

// `.count()` comparisons: SimTime's own operators are std::chrono's (the tick counts are in the
// same unit, nanoseconds).
bool Summary::in_watched_window(SimTime t) const {
  return std::ranges::any_of(watched_, [&](const FaultWindow& window) {
    return t.count() >= window.start.count() && (!window.end || t.count() < window.end->count());
  });
}

// One step judged alone, by the very rules that judge a run (evaluate_slo on a Summary of that
// step only): an SLO with `over` as if it had none, since one step is no window. The one
// definition of "the SLO holds", applied to a step.
void Summary::judge_step(const engine::StepRecord& record) {
  Summary step{*graph_, SimTime{0}, false};
  step.observe(record);
  const auto nodes = step.nodes();
  const auto ops = step.ops();
  JudgedStep judged{.t = record.t, .dt = graph_->scenario().step};
  for (const auto& slo : graph_->scenario().slos) {
    model::SloSpec alone = slo;
    alone.over.reset();
    judged.verdicts.push_back(evaluate_slo(step, nodes, ops, alone).status);
  }
  judged_.push_back(std::move(judged));
}

namespace {

// A flip belongs to the run (FaultSlo): it is named on every block that broke the SLO during its
// window, or on every block when none did. `flipped[k]` is SLO k's run verdict when it differs from
// the twin's.
void attribute_flips(std::span<FaultBlock> blocks,
                     std::span<const std::optional<SloStatus>> flipped) {
  for (std::size_t k = 0; k < flipped.size(); ++k) {
    if (!flipped[k]) continue;
    const auto broke = [&](const FaultBlock& block) {
      return std::ranges::any_of(block.slos, [&](const FaultSlo& e) { return e.slo == k; });
    };
    const bool any_broke = std::ranges::any_of(blocks, broke);
    for (auto& block : blocks) {
      if (any_broke && !broke(block)) continue;
      auto found = std::ranges::find(block.slos, k, &FaultSlo::slo);
      if (found == block.slos.end()) {
        // The list's order: before the first entry of a later SLO.
        found = block.slos.insert(
            std::ranges::find_if(block.slos, [&](const FaultSlo& e) { return e.slo > k; }),
            FaultSlo{.slo = k});
      }
      found->flipped_to = flipped[k];
    }
  }
}

}  // namespace

void Summary::set_faults(std::span<const engine::FaultRecord> records, const Summary& twin) {
  const auto& slos = graph_->scenario().slos;
  const auto verdicts = evaluate_slos(*this, slos);
  const auto twin_verdicts = evaluate_slos(twin, slos);
  std::vector<std::optional<SloStatus>> flipped;
  for (std::size_t k = 0; k < verdicts.size(); ++k) {
    const bool differs = verdicts[k].status != twin_verdicts[k].status;
    flipped.push_back(differs ? std::optional{verdicts[k].status} : std::nullopt);
  }
  std::vector<FaultBlock> blocks;
  for (std::size_t i = 0; i < records.size(); ++i) {
    blocks.push_back(block_of(records[i], twin));
    for (std::size_t j = 0; j < i; ++j) {
      if (overlaps(records[i], records[j])) blocks.back().overlaps.push_back(records[j].at);
    }
  }
  attribute_flips(blocks, flipped);
  faults_ = std::move(blocks);
}

// The record and every SLO it broke during its window where the twin did not (FaultSlo).
FaultBlock Summary::block_of(const engine::FaultRecord& record, const Summary& twin) const {
  FaultBlock block{.record = record};
  const FaultWindow window = window_of(record);
  for (std::size_t k = 0; k < graph_->scenario().slos.size(); ++k) {
    const SimTime over = over_max(k, window, twin);
    if (over.count() > 0) block.slos.push_back(FaultSlo{.slo = k, .over_max_s = seconds(over)});
  }
  return block;
}

// Σ dt over the judged steps in `window` at which SLO `slo` fails here and not in the twin.
SimTime Summary::over_max(std::size_t slo, const FaultWindow& window, const Summary& twin) const {
  SimTime total{0};
  for (const auto& step : judged_) {
    const bool inside = step.t.count() >= window.start.count() &&
                        (!window.end || step.t.count() < window.end->count());
    if (!inside || step.verdicts[slo] != SloStatus::fail || twin.twin_breaks(slo, step.t)) continue;
    total += step.dt;
  }
  return total;
}

// Whether this Summary (a twin's) judged SLO `slo` broken at the step that starts at `t`; a step
// it did not judge breaks nothing.
bool Summary::twin_breaks(std::size_t slo, SimTime t) const {
  // C++20 std::ranges::lower_bound with a projection: the binary search over the judged steps by
  // the tick count each projects to (they are in time order), without a comparator on both sides.
  const auto found = std::ranges::lower_bound(judged_, t.count(), {},
                                              [](const JudgedStep& s) { return s.t.count(); });
  if (found == judged_.end() || found->t.count() != t.count()) return false;
  return found->verdicts[slo] == SloStatus::fail;
}

void Summary::observe(const engine::StepRecord& record) {
  const double dt_s = step_length_s(record.t);
  // A step of a fault's window is judged alone (watch_faults), in the warmup too: it is the step's
  // own verdict, not the run's.
  if (in_watched_window(record.t)) judge_step(record);
  if (record.t.count() < warmup_.count()) return;  // warmup: the queues are still filling
  ++observed_steps_;
  observed_s_ += dt_s;
  // C++23 std::views::zip: walks two ranges in lockstep and yields pairs of references (here each
  // node's running totals and its sample of this step), replacing an index loop over both.
  for (auto [totals, sample] : std::views::zip(nodes_, record.nodes)) totals.add(sample, dt_s);
  for (auto [totals, sample] : std::views::zip(links_, record.links)) totals.add(sample, dt_s);
  for (std::size_t k = 0; k < record.responses.size(); ++k) {
    const auto& sample = record.responses[k];
    totals_for(sample, k).add(sample, dt_s);
  }
  observe_windows(record);
}

// Feeds each windowed SLO's current window, after judging and closing the one before when this
// step starts a new window (see the class comment). Window k is [warmup + k·over, warmup +
// (k + 1)·over); `.count()` arithmetic on the tick counts (nanoseconds), exact.
void Summary::observe_windows(const engine::StepRecord& record) {
  for (auto& windowed : windows_) {
    const long long index = (record.t.count() - warmup_.count()) / windowed.slo.over->count();
    if (index != windowed.index) {
      if (!windowed.current.empty()) keep_if_worse(windowed.worst, judge_window(windowed));
      windowed.current.clear();
      windowed.current.push_back(Summary{*graph_, warmup_, false});
      windowed.index = index;
    }
    windowed.current.front().observe(record);
  }
}

// The current window of `windowed` judged like a whole run (evaluate_slo), with where it starts.
// Every window is judged: a queue still draining after its clients stopped is busy, and its
// utilization, drops or lag in that window count. Only a latency SLO skips a window, one in which
// its op was not offered: the same evaluate_slo leaves out an op nobody sent (worst_latency,
// nothing_offered), as an idle service's p99 would be its service time's, a bound no request met.
SloResult Summary::judge_window(const WindowedSlo& windowed) const {
  const Summary& window = windowed.current.front();
  SloResult result = evaluate_slo(window, window.nodes(), window.ops(), windowed.slo);
  if (result.status == SloStatus::skipped) return result;
  // Where it starts, and how long it is: `over`, or up to the end of its last step when the run
  // ends first (the still-open last window, windowed_verdict). `.count()` arithmetic, as above.
  const auto over = windowed.slo.over->count();
  const auto start = warmup_.count() + static_cast<SimTime::rep>(windowed.index) * over;
  const auto end = window.last_t_->count() + graph_->scenario().step.count();
  result.window_start = SimTime{start};
  result.window_length = SimTime{std::min(over, end - start)};
  return result;
}

SloResult Summary::windowed_verdict(const model::SloSpec& slo) const {
  if (estimate_) {
    SloResult result = evaluate_slo(*this, nodes(), ops(), slo);
    result.at_one_instant = true;
    return result;
  }
  const auto found = std::ranges::find_if(
      windows_, [&](const WindowedSlo& windowed) { return same_slo(windowed.slo, slo); });
  if (found == windows_.end() || found->current.empty()) return skipped(slo);
  std::optional<SloResult> worst = found->worst;
  keep_if_worse(worst, judge_window(*found));  // the last window, however short
  return worst.value_or(skipped(slo));
}

std::vector<NodeSummary> Summary::nodes() const {
  std::vector<NodeSummary> out;
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    const auto& t = nodes_[i];
    const double capacity = graph_->nodes()[i].capacity.service_rate.per_second;
    out.push_back(NodeSummary{.id = graph_->nodes()[i].spec.id,
                              .mean_rho = observed_s_ > 0.0 ? t.rho_time / observed_s_ : 0.0,
                              .peak_rho = t.peak_rho,
                              .peak_backlog = without_residue(t.peak_backlog, capacity),
                              .total_dropped = without_residue(t.dropped, capacity),
                              .total_rejected = without_residue(t.rejected, capacity),
                              .red_time_s = t.red_time_s,
                              .unavailable = t.unavailable,
                              .peak_queue_lag_s = t.peak_queue_lag_s});
    if (t.dead_lettered) {
      out.back().total_dead_lettered = *t.dead_lettered;
      out.back().mean_dead_lettered_per_s =
          observed_s_ > 0.0 ? *t.dead_lettered / observed_s_ : 0.0;
    }
    if (t.staleness_seen_s > 0.0)
      out.back().mean_staleness_s = t.staleness_time / t.staleness_seen_s;
    if (t.replication_seen_s > 0.0) {
      out.back().mean_replication_dropped_per_s = t.replication_dropped / t.replication_seen_s;
      out.back().mean_committed_writes_per_s = t.committed_writes / t.replication_seen_s;
    }
  }
  return out;
}

std::vector<LinkSummary> Summary::links() const {
  std::vector<LinkSummary> out;
  const auto per_s = [&](double total) { return observed_s_ > 0.0 ? total / observed_s_ : 0.0; };
  std::size_t next = 0;  // links_ follows Graph::links(), pipes only
  for (const auto& link : graph_->links()) {
    if (!link.pipe) continue;
    const auto& t = links_[next++];
    const double capacity = link.pipe->service_rate.per_second;
    const Moments& latency = t.by_demand.weight > 0.0 ? t.by_demand : t.by_time;
    out.push_back(LinkSummary{
        .queue = NodeSummary{.id = t.name,
                             .mean_rho = per_s(t.queue.rho_time),
                             .peak_rho = t.queue.peak_rho,
                             .peak_backlog = without_residue(t.queue.peak_backlog, capacity),
                             .total_dropped = without_residue(t.queue.dropped, capacity),
                             .total_rejected = 0.0,
                             .red_time_s = t.queue.red_time_s,
                             .unavailable = t.queue.unavailable},
        .demand_per_s = per_s(t.queue.demand),
        .drain_per_s = per_s(t.drained),
        .dropped_per_s = per_s(t.queue.dropped),
        .backlog = per_s(t.backlog_time),
        .latency_s = latency.weight > 0.0 ? latency.weighted_mean / latency.weight : 0.0,
        .zone = t.worst_zone});
  }
  return out;
}

std::vector<OpSummary> Summary::ops() const {
  std::vector<OpSummary> out;
  for (const auto& t : ops_) {
    const auto& client = graph_->nodes()[t.client].spec;
    OpSummary op{
        .client = client.id,
        .op = t.op,
        .offered_per_s = observed_s_ > 0.0 ? t.offered / observed_s_ : 0.0,
        .attempts_per_s = observed_s_ > 0.0 ? t.attempts / observed_s_ : 0.0,
        .answered_per_s = observed_s_ > 0.0 ? t.answered / observed_s_ : 0.0,
        .timed_out_per_s = observed_s_ > 0.0 ? t.timed_out / observed_s_ : 0.0,
        .timed_out_attempts_per_s = observed_s_ > 0.0 ? t.timed_out_attempts / observed_s_ : 0.0,
        .timeout_s = graph::client_timeout_s(client),
        .timed_out_share = timed_out_share(t.answered, t.timed_out_attempts)};
    if (t.unavailable) {
      op.mean_s = op.p50_s = op.p95_s = op.p99_s = infinity;
    } else {
      fill_latencies(t, op);
    }
    out.push_back(std::move(op));
  }
  return out;
}

// The mean is the answered requests' (the time an answer took on average). The percentiles are a
// lognormal moment fit (Fenton-Wilkinson, see queueing::lognormal_quantile): an estimate, which is
// why every renderer labels them "estimated". percentile_seen_by_client reads them off the
// delivered requests' moments, before any timeout cut them (identical to the answered ones without
// a timeout), and applies the client's timeout. Whichever moments are empty fall back to the
// time-weighted ones: an op nothing reached, which no renderer prints a latency for.
void Summary::fill_latencies(const OpTotals& totals, OpSummary& op) {
  const Moments& answered = totals.by_answered.weight > 0.0 ? totals.by_answered : totals.by_time;
  if (answered.weight <= 0.0) return;  // no step observed
  op.mean_s = answered.mean_s();
  const bool timed = op.timeout_s.has_value() && totals.by_delivered.weight > 0.0;
  const Moments& delivered = timed ? totals.by_delivered : answered;
  const auto seen = [&](Quantile q) {
    return percentile_seen_by_client(delivered.mean_s(), delivered.variance_s2(),
                                     op.timed_out_share, op.timeout_s, q)
        .seconds;
  };
  op.p50_s = seen(median);
  op.p95_s = seen(ninety_fifth);
  op.p99_s = seen(ninety_ninth);
}

// Per node, not per scenario: a node's drops are a fraction of what it was sent, so the
// ratio stays in [0, 1] even when fan-out (ch. 11) multiplies the client's requests 100
// times before they reach it. Dividing by what the clients offered would not be a ratio.
// Drops only: a rate limiter's refusals are its policy, not a failure (reject_rate), and the failed
// calls a pool retries are attempts, not losses (retried_dropped, Summary::drop_rate). The max
// keeps a rounding error from making a sink called only by retrying pools lose −1e-17.
double Summary::NodeTotals::drop_rate() const {
  return demand > 0.0 ? std::max(0.0, dropped - retried_dropped) / demand : 0.0;
}

// The same ratio for refusals. Rejections are part of the demand (a token-family limiter
// rejects what its queue drained, a leaky bucket what overflowed), so it stays <= 1 too.
double Summary::NodeTotals::reject_rate() const {
  return demand > 0.0 ? rejected / demand : 0.0;
}

// The first of equal maxima (std::ranges::max_element keeps it), so ties name the
// earliest node. A projection through a pointer to member function: std::invoke calls
// (totals.*rate)().
std::size_t Summary::worst_index(double (NodeTotals::*rate)() const) const {
  const auto worst = std::ranges::max_element(nodes_, std::less<>{}, rate);
  return static_cast<std::size_t>(worst - nodes_.begin());
}

double Summary::node_rate(std::string_view node, double (NodeTotals::*rate)() const) const {
  const auto index = graph_->index_of(node);
  if (!index) {
    throw std::invalid_argument{"Summary: unknown node '" + std::string{node} + "'"};
  }
  return (nodes_[*index].*rate)();
}

// Σ dead letters of the pool over Σ (demand − re-published) of the queue it consumes: what the
// producers published, the retries left out (they are the same jobs again).
double Summary::dead_letter_rate(std::string_view pool) const {
  const auto index = graph_->index_of(pool);
  if (!index) {
    throw std::invalid_argument{"Summary: unknown node '" + std::string{pool} + "'"};
  }
  const auto& consumes = graph_->nodes()[*index].spec.consumes;
  const auto queue = consumes ? graph_->index_of(*consumes) : std::nullopt;
  if (!queue) return 0.0;
  const auto& published = nodes_[*queue];
  const double jobs = published.demand - published.republished.value_or(0.0);
  return jobs > 0.0 ? nodes_[*index].dead_lettered.value_or(0.0) / jobs : 0.0;
}

double Summary::drop_rate(std::string_view node) const {
  return node_rate(node, &NodeTotals::drop_rate);
}

double Summary::worst_node_drop_rate() const {
  return nodes_.empty() ? 0.0 : nodes_[worst_index(&NodeTotals::drop_rate)].drop_rate();
}

// The first of equal maxima, as worst_index.
const Summary::LinkTotals* Summary::worst_dropping_link() const {
  const auto worst = std::ranges::max_element(
      links_, std::less<>{}, [](const LinkTotals& link) { return link.queue.drop_rate(); });
  return worst == links_.end() ? nullptr : &*worst;
}

// The worst node's, or the worst pipe's when it is higher.
double Summary::drop_rate() const {
  const auto* link = worst_dropping_link();
  const double node = worst_node_drop_rate();
  return link != nullptr ? std::max(node, link->queue.drop_rate()) : node;
}

// A tie between a node and a pipe names the node.
std::string_view Summary::worst_drop_node() const {
  if (drop_rate() <= 0.0) return {};
  const auto* link = worst_dropping_link();
  if (link != nullptr && link->queue.drop_rate() > worst_node_drop_rate()) return link->name;
  return graph_->nodes()[worst_index(&NodeTotals::drop_rate)].spec.id;
}

double Summary::reject_rate(std::string_view node) const {
  return node_rate(node, &NodeTotals::reject_rate);
}

double Summary::reject_rate() const {
  return nodes_.empty() ? 0.0 : nodes_[worst_index(&NodeTotals::reject_rate)].reject_rate();
}

std::string_view Summary::worst_reject_node() const {
  if (reject_rate() <= 0.0) return {};
  return graph_->nodes()[worst_index(&NodeTotals::reject_rate)].spec.id;
}

std::string_view Summary::scenario_name() const {
  return graph_->scenario().name;
}

// ---- SLOs ----

std::vector<SloResult> evaluate_slos(const Summary& summary, std::span<const model::SloSpec> slos) {
  const auto nodes = summary.nodes();
  const auto ops = summary.ops();
  std::vector<SloResult> results;
  for (const auto& slo : slos) {
    results.push_back(slo.over ? summary.windowed_verdict(slo)
                               : evaluate_slo(summary, nodes, ops, slo));
  }
  return results;
}

bool all_pass(std::span<const SloResult> results) {
  // C++20 std::ranges::none_of: the range itself instead of a begin/end pair (std::none_of).
  return std::ranges::none_of(results,
                              [](const SloResult& r) { return r.status == SloStatus::fail; });
}

// ---- renderers ----

// With no observed step there are no node or op figures to show, only the skipped SLOs.
std::string render_table(const Summary& summary, std::span<const SloResult> slos, bool color) {
  if (summary.observed_steps() == 0) {
    return table_header(summary) + slo_table(slos, color) + fault_blocks(summary);
  }
  return table_header(summary) + faults_note(summary) + node_table(summary, color) +
         link_table(summary, color) + replication_table(summary) + op_table(summary) +
         slo_table(slos, color) + fault_blocks(summary);
}

std::string render_json(const Summary& summary, std::span<const SloResult> slos) {
  return json_document(json_report_lines(summary, slos));
}

// ---- replications ----

ReplicationSummary summarize_replications(std::span<const Summary> runs,
                                          std::span<const int> seeds) {
  if (runs.empty() || seeds.size() != runs.size()) {
    throw std::invalid_argument{"report::summarize_replications: one seed per run, at least one"};
  }
  const RunFigures figures = figures_of(runs);
  ReplicationSummary replicated{.seeds = {seeds.begin(), seeds.end()}};
  for (std::size_t i = 0; i < figures.ops.front().size(); ++i) {
    replicated.ops.push_back(op_bands(figures.ops, i));
  }
  for (std::size_t i = 0; i < figures.nodes.front().size(); ++i) {
    replicated.nodes.push_back(node_bands(figures.nodes, i));
  }
  for (std::size_t j = 0; j < figures.slos.front().size(); ++j) {
    replicated.slos.push_back(slo_replication(figures.slos, j));
  }
  replicated.median_run = standing_run(replicated);
  return replicated;
}

std::vector<SloResult> replicated_verdicts(const ReplicationSummary& replicated) {
  std::vector<SloResult> verdicts;
  for (const auto& slo : replicated.slos) verdicts.push_back(slo.result);
  return verdicts;
}

std::string format_band(std::string_view median, std::string_view low, std::string_view high) {
  if (low == median && high == median) return std::string{median};
  const auto m = split_unit(median);
  const auto l = split_unit(low);
  const auto h = split_unit(high);
  const bool one_unit = m && l && h && l->unit == m->unit && h->unit == m->unit;
  const auto low_text = one_unit ? l->number : low;
  const auto high_text = one_unit ? h->number : high;
  return std::string{median} + " [" + std::string{low_text} + std::string{en_dash} +
         std::string{high_text} + "]";
}

// A run with no observed step has no figures to band: the summary line, the replicated SLO
// verdicts (all skipped) and the faults, as render_table prints such a run.
std::string render_replications_table(const ReplicationSummary& replicated,
                                      std::span<const Summary> runs, bool color) {
  const Summary& median = median_summary(replicated, runs);
  const std::string head = replications_intro(replicated) + table_header(median);
  const std::string verdicts = replicated_slo_table(replicated, color) + fault_blocks(median);
  if (median.observed_steps() == 0) return head + verdicts;
  return head + banded_tables(runs, color) + verdicts;
}

std::string render_replications_json(const ReplicationSummary& replicated,
                                     std::span<const Summary> runs) {
  const Summary& median = median_summary(replicated, runs);
  auto lines = json_report_lines(median, replicated_verdicts(replicated));
  lines.push_back(json_replications(replicated, figures_of(runs)));
  return json_document(lines);
}

double op_latency_s(const OpSummary& op, OpLatency column) {
  switch (column) {
    case OpLatency::mean:
      return op.mean_s;
    case OpLatency::p50:
      return op.p50_s;
    case OpLatency::p95:
      return op.p95_s;
    case OpLatency::p99:
      return op.p99_s;
  }
  std::unreachable();
}

bool latency_beyond_timeout(const OpSummary& op, OpLatency column) {
  switch (column) {
    case OpLatency::mean:
      return false;
    case OpLatency::p50:
      return beyond_timeout(op, median);
    case OpLatency::p95:
      return beyond_timeout(op, ninety_fifth);
    case OpLatency::p99:
      return beyond_timeout(op, ninety_ninth);
  }
  std::unreachable();
}

std::string format_number(double value) {
  return to_chars_text(value);
}

std::string align_columns(std::span<const std::vector<std::string>> rows, std::string_view indent) {
  std::vector<Row> cells;
  for (const auto& row : rows) {
    auto& line = cells.emplace_back();
    for (const auto& text : row) line.push_back(Cell{.text = text});
  }
  return aligned(cells, Align::first_left_rest_right, indent);
}

// ---- SeriesWriter ----

SeriesWriter::SeriesWriter(std::ostream& out, const graph::Graph& g) : out_{&out} {
  for (const auto& node : g.nodes()) {
    ids_.push_back(node.spec.id);
    is_client_.push_back(node.spec.type == model::NodeType::client);
  }
  for (const auto& link : g.links()) {
    if (link.pipe) link_names_.push_back(graph::link_label(g.nodes(), link));
  }
  *out_ << "t_s,node,demand_per_s,drain_per_s,rho,backlog,drops_per_s,dead_lettered_per_s,"
           "rejected_per_s,latency_ms,zone,queue_lag_s\n";
}

// CSV is for machines, so every numeric column holds a number on every row the quantity exists
// for, and is empty where it does not (only `dead_lettered_per_s` and `queue_lag_s`, below):
// - `t_s` is the shortest form that reads back to the same double (0.1, 2, 0.35), not a
//   fixed number of decimals that would round a fine step away; the other numbers have 6
//   significant digits;
// - `demand_per_s` is what reached the node, `drain_per_s` what its queue served, both in
//   capacity units (engine::NodeSample). With `drops_per_s` and `backlog` they balance, step
//   by step and so over a run that starts empty (the fluid queue's conservation law):
//       Σ demand·dt = Σ (drain + drops)·dt + backlog at the end
//   whatever came in was served, shed or is still waiting. A limiter's rejections are not a
//   term of their own (engine::NodeSample): a token bucket's or a window algorithm's were
//   drained by its queue before it refused them, so they are inside `drain` (and
//   `rejected_per_s` says how much of it); a leaky bucket's are its queue's overflow, written
//   in `rejected_per_s` instead of `drops_per_s`, so for one the identity takes them in the
//   drops' place;
// - a client row writes 0 in every numeric column but `dead_lettered_per_s` and `queue_lag_s` (a
//   source has no queue), which it leaves empty, and `none` as its zone, so a plot can filter
//   clients out by zone;
// - `dead_lettered_per_s` (engine::NodeSample::dead_lettered_per_s) is a worker pool's jobs whose
//   last retry failed, right after `drops_per_s` because they are not drops (the identity above
//   does not include them: the pool drained those jobs); 0 on a pool without retries and EMPTY on
//   every other row, clients' and links' included. A sink's rows keep the failures a pool retries
//   in `drops_per_s`: its books balance whatever its callers do with them;
// - an infinite latency is written as `inf` (which spreadsheet and plotting tools parse),
//   unlike the table ("unavailable") and JSON (null), read by people or by parsers
//   without an infinity;
// - `queue_lag_s` (engine::NodeSample::queue_lag_s) is a message queue's lag in
//   SECONDS (a lag is minutes or hours, plotted against the backlog), and EMPTY on every other
//   row, clients' and links' included: a 0 would claim a lag the node cannot have;
// - after the nodes, a row per link with a bandwidth, `from->to` in the node
//   column, in MEGABYTES (engine::LinkSample): the pipe is a queue, so the same identity holds on
//   its rows, `rejected_per_s` is 0 and `latency_ms` is a request's wait plus its answer time on
//   the wire (a write's whole serialisation, any other op's first megabyte).
void SeriesWriter::observe(const engine::StepRecord& record) {
  const auto t_s = format_number(seconds(record.t));
  for (std::size_t i = 0; i < ids_.size() && i < record.nodes.size(); ++i) {
    *out_ << (is_client_[i] ? csv_client_row(t_s, ids_[i])
                            : csv_server_row(t_s, ids_[i], record.nodes[i]));
  }
  for (std::size_t i = 0; i < link_names_.size() && i < record.links.size(); ++i) {
    *out_ << csv_link_row(t_s, link_names_[i], record.links[i]);
  }
}

// ---- OpSeriesWriter ----

OpSeriesWriter::OpSeriesWriter(std::ostream& out, const graph::Graph& g) : out_{&out} {
  for (const auto& node : g.nodes()) ids_.push_back(node.spec.id);
  *out_ << "t_s,client,op,offered_per_s,attempts_per_s,answered_per_s,timed_out_per_s,mean_ms\n";
}

// One row per response sample, in the record's order (clients by node index, ops by name), with
// the node series' conventions (SeriesWriter::observe): `t_s` in its shortest form, 6
// significant digits, `inf` for a mean of a path that never answers. answered = offered × the
// fraction the path delivered (engine::ResponseSample): offered minus what a limiter refused or
// a queue dropped on the way this step, never above offered for a client without retries; a
// request still waiting in a growing queue is not lost, so a backlog alone leaves answered =
// offered. A client with retries answers attempts × that fraction, which a wave of retries can
// lift above offered for a step (engine::ResponseSample). A sample whose client is
// not a node of the writer's graph is a programmer error (a record of another simulation): `at`
// throws.
void OpSeriesWriter::observe(const engine::StepRecord& record) {
  const auto t_s = format_number(seconds(record.t));
  for (const auto& sample : record.responses) {
    *out_ << csv_op_row(t_s, ids_.at(sample.client), sample);
  }
}

}  // namespace simsys::report
