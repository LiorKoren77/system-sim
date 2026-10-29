// Implementation unit of `simsys.report`: the running aggregates, SLO verdicts and the
// four renderers (table, JSON, CSV series, explain). Heavy headers (<ostream>, <ranges>,
// ...) belong here, not in the interface unit (CLAUDE.md §3, GCC 14.2 'Bad file data' bug).
//
// Every number is formatted with std::to_chars, never with a std::format precision such as
// "{:.3f}": with GCC 14.2, a precision in a format spec throws std::format_error at run
// time in a unit that imports simsys.core (CLAUDE.md §3, second <format> defect).
module;
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
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

// ---- number formatting ----

// std::to_chars (C++17): locale-independent, allocation-free number formatting. Used for
// every number here instead of std::format precisions (see the top of the file).
// A value too long for the buffer (only possible for huge fixed-point numbers) falls back
// to the shortest round-trip form, which always fits.
[[nodiscard]] std::string shortest(double value) {
  std::array<char, 32> buffer{};  // the shortest form of any double is at most 24 chars
  const auto written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  return std::string{buffer.data(), written.ptr};
}

[[nodiscard]] std::string to_text(double value, std::chars_format format, int precision) {
  std::array<char, 128> buffer{};
  const auto written =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, format, precision);
  if (written.ec != std::errc{}) return shortest(value);
  return std::string{buffer.data(), written.ptr};
}

// `value` with `digits` significant digits, trailing zeros dropped: 0.5, 1.5, 1000.
[[nodiscard]] std::string num(double value, int digits = 4) {
  return to_text(value, std::chars_format::general, digits);
}

// A unit and its size in the base unit (seconds, or one request).
struct Unit {
  std::string_view name;
  double size;
};

// Pattern: Table-driven method — the units are data searched by one function each, the
// same grammar the scenario files use ("250us", "1.5k/s"), largest first.
constexpr std::array duration_units{Unit{"h", 3600.0}, Unit{"m", 60.0},  Unit{"s", 1.0},
                                    Unit{"ms", 1e-3},  Unit{"us", 1e-6}, Unit{"ns", 1e-9}};
constexpr std::array si_prefixes{Unit{"G", 1e9}, Unit{"M", 1e6}, Unit{"k", 1e3}};

// 0.9995 of a unit already prints as "1" at three significant digits, so it is shown in
// that unit: 999.7 us reads "1ms", not "1e+03us".
constexpr double rounds_to_one = 0.9995;

// Seconds in the largest unit that keeps the number >= 1, three significant digits:
// 0.0005 -> "500us", 120 -> "2m". Infinity means the node never answers: "unavailable".
[[nodiscard]] std::string fmt_duration(double seconds_value) {
  if (!std::isfinite(seconds_value)) return "unavailable";
  if (seconds_value == 0.0) return "0s";
  for (const auto& unit : duration_units) {
    if (std::abs(seconds_value) / unit.size >= rounds_to_one) {
      return num(seconds_value / unit.size, 3) + std::string{unit.name};
    }
  }
  return num(seconds_value / 1e-9, 3) + "ns";  // below one nanosecond
}

[[nodiscard]] std::string fmt_duration(SimTime t) {
  return fmt_duration(seconds(t));
}

// From here on the largest prefix, G, would need an exponent itself ("1e+04G").
constexpr double beyond_prefixes = 1e4 * si_prefixes.front().size;

// A count with an SI prefix and four significant digits: 500, 1.5k, 2M. Past 9999G it is
// written in plain scientific notation (1e+15), which parse_rate reads back too; G stays
// the largest prefix, as in the scenario grammar (core's rate_prefixes stop at G).
[[nodiscard]] std::string with_si_prefix(double value) {
  if (!std::isfinite(value)) return "unavailable";
  if (std::abs(value) >= beyond_prefixes) return num(value);
  for (const auto& prefix : si_prefixes) {
    if (std::abs(value) >= prefix.size) return num(value / prefix.size) + std::string{prefix.name};
  }
  return num(value);
}

// A count of things (keys) with an SI prefix, as with_si_prefix — 500, 1.5k, 2M, 9999G — but
// past the largest prefix in plain digits (20000000000000) rather than scientific notation: a
// count, unlike a rate, is a whole number a reader compares digit by digit, and the scenario
// grammar writes counts with k/M/G or digits only (config's parse_count).
[[nodiscard]] std::string fmt_count(double value) {
  if (!std::isfinite(value) || std::abs(value) < beyond_prefixes) return with_si_prefix(value);
  return to_text(value, std::chars_format::fixed, 0);  // no std::format precision: see the top
}

[[nodiscard]] std::string fmt_rate(double per_second) {
  return with_si_prefix(per_second) + "/s";
}

[[nodiscard]] std::string percent(double ratio) {
  return num(ratio * 100.0, 3) + "%";
}

// ---- enum names ----

[[nodiscard]] std::string_view zone_name(fluid::Zone zone) {
  // C++20 `using enum`: brings the enumerators into this function's scope, so the cases
  // read `green` instead of `fluid::Zone::green`; nothing leaks outside the function.
  using enum fluid::Zone;
  switch (zone) {
    case green:
      return "green";
    case yellow:
      return "yellow";
    case red:
      return "red";
  }
  // C++23 std::unreachable(): every enumerator returned above (-Wswitch, an error under
  // -Werror, proves it), so this point is unreachable. Replaces a dummy return value
  // that would hide a missing case, and silences -Wreturn-type.
  std::unreachable();
}

// The SLO metric and link direction names come from their tables in simsys.model
// (model::to_string), the same words the loader reads.

[[nodiscard]] std::string_view status_name(SloStatus status) {
  using enum SloStatus;
  switch (status) {
    case pass:
      return "pass";
    case fail:
      return "fail";
    case skipped:
      return "skipped";
  }
  std::unreachable();
}

[[nodiscard]] bool is_latency(model::SloMetric metric) {
  using enum model::SloMetric;
  return metric == p50_latency || metric == p95_latency || metric == p99_latency;
}

// ---- small text helpers ----

// C++23 std::views::join_with + std::ranges::to: joins the parts with a separator and
// collects the characters into a std::string in one pipeline. Replaces a hand-written
// loop that appends a separator before every part but the first.
[[nodiscard]] std::string join(std::span<const std::string> parts, std::string_view separator) {
  return parts | std::views::join_with(separator) | std::ranges::to<std::string>();
}

// One cell of an aligned text table; `colour` is an ANSI escape, or empty.
struct Cell {
  std::string text{};
  std::string_view colour{};
};
using Row = std::vector<Cell>;

enum class Align { first_left_rest_right, all_left };

constexpr std::string_view ansi_reset = "\x1b[0m";

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

[[nodiscard]] std::vector<std::size_t> column_widths(std::span<const Row> rows) {
  std::vector<std::size_t> widths;
  for (const auto& row : rows) {
    if (widths.size() < row.size()) widths.resize(row.size());
    for (std::size_t i = 0; i < row.size(); ++i) {
      widths[i] = std::max(widths[i], row[i].text.size());
    }
  }
  return widths;
}

[[nodiscard]] std::string padded(const std::string& text, std::size_t width, bool right) {
  const std::string fill(width - text.size(), ' ');
  return right ? fill + text : text + fill;
}

// Every column padded to its widest cell (names left, numbers right by default), two
// spaces between columns, colour codes wrapped around the padded text so they do not
// count towards the width. Trailing blanks are trimmed.
[[nodiscard]] std::string aligned(std::span<const Row> rows,
                                  Align align = Align::first_left_rest_right) {
  const auto widths = column_widths(rows);
  std::string out;
  for (const auto& row : rows) {
    std::string line = "  ";
    for (std::size_t i = 0; i < row.size(); ++i) {
      if (i > 0) line += "  ";
      const bool right = align == Align::first_left_rest_right && i > 0;
      const auto cell = padded(row[i].text, widths[i], right);
      line += row[i].colour.empty() ? cell
                                    : std::string{row[i].colour} + cell + std::string{ansi_reset};
    }
    line.erase(line.find_last_not_of(' ') + 1);
    out += line + '\n';
  }
  return out;
}

// ---- SLO evaluation ----

// The "overloaded" idiom: one object whose operator() is overloaded by every lambda it is
// built from, so std::visit picks the lambda matching the alternative held.
// C++17 pack expansion in a using-declaration (`using Fs::operator()...`) merges the
// lambdas' call operators. C++20 class template argument deduction for aggregates deduces
// `Overloaded<L1, L2>` from the braces, so C++17's deduction guide
// (`Overloaded(Fs...) -> Overloaded<Fs...>`) is no longer needed; the struct itself is
// still the idiom, as the standard library offers no ready-made overload set.
template <class... Fs>
struct Overloaded : Fs... {
  using Fs::operator()...;
};

// An SLO's limit as a plain number: seconds for a latency, the ratio itself otherwise.
// Pattern: Visitor over a closed sum type — the variant lists every kind of limit, and
// std::visit fails to compile if a lambda for one of them is missing.
[[nodiscard]] double limit_value(const std::variant<SimTime, double>& max) {
  return std::visit(
      Overloaded{[](SimTime limit) { return seconds(limit); }, [](double limit) { return limit; }},
      max);
}

// The latency quantile `quantile` of the client's op, or the worst one over all of the
// client's ops when the SLO names none (every op must meet it, so the worst decides).
// `double OpSummary::*` is a pointer to a data member (C++98): it selects p50/p95/p99
// without three copies of the loop.
[[nodiscard]] std::optional<double> worst_latency(std::span<const OpSummary> ops,
                                                  const model::SloSpec& slo,
                                                  double OpSummary::*quantile) {
  std::optional<double> worst;
  for (const auto& op : ops) {
    if (op.client != slo.node.value_or("")) continue;
    if (slo.op && op.op != *slo.op) continue;
    const double value = op.*quantile;
    worst = worst ? std::max(*worst, value) : value;
  }
  return worst;
}

[[nodiscard]] std::optional<double> mean_utilization(std::span<const NodeSummary> nodes,
                                                     const model::SloSpec& slo) {
  // C++20 std::ranges::find with a projection (&NodeSummary::id): "the node whose id is
  // X" without a hand-written comparator lambda.
  const auto found = std::ranges::find(nodes, slo.node.value_or(""), &NodeSummary::id);
  if (found == nodes.end()) return std::nullopt;
  return found->mean_rho;
}

// The named node's mean staleness (plan 2b refinement 3: a SQL database's write sojourn at the
// primary + replication lag + sojourn at a replica, how old the data a read may return is), in
// seconds; empty for a node with no lagging copies, whose staleness SLO is then skipped (nothing
// to observe).
[[nodiscard]] std::optional<double> mean_staleness(std::span<const NodeSummary> nodes,
                                                   const model::SloSpec& slo) {
  const auto found = std::ranges::find(nodes, slo.node.value_or(""), &NodeSummary::id);
  if (found == nodes.end()) return std::nullopt;
  return found->mean_staleness_s;
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
    case queue_lag:  // consumer lag of a message queue: phase 2c
      return std::nullopt;
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

// An SLO with nothing observed: phase 1 does not model the metric, or no step was observed.
[[nodiscard]] SloResult skipped(const model::SloSpec& slo) {
  // C++20 designated initializers: name the fields being set, so `observed` and `status`
  // cannot be swapped unnoticed; a field left out keeps its default member initializer.
  // Replaces positional braces, which say nothing about which value is which.
  return SloResult{.slo = slo, .observed = std::nullopt, .status = SloStatus::skipped};
}

// A metric passes when it is at most its limit. An infinite latency (a node that never
// answers) and NaN both fail. No data is an error, not a pass: with no observed step every
// average would be a 0 that passes any limit, so nothing is evaluated (the CLI exits 1).
[[nodiscard]] SloResult evaluate_slo(const Summary& summary, std::span<const NodeSummary> nodes,
                                     std::span<const OpSummary> ops, const model::SloSpec& slo) {
  if (summary.observed_steps() == 0) return skipped(slo);
  const auto observed = observe_metric(summary, nodes, ops, slo);
  if (!observed) return skipped(slo);
  const bool within = *observed <= limit_value(slo.max);
  SloResult result{
      .slo = slo, .observed = observed, .status = within ? SloStatus::pass : SloStatus::fail};
  if (slo.metric == model::SloMetric::staleness) fail_if_replica_falls_behind(result, nodes);
  return result;
}

// ---- table ----

// A latency or a staleness is a time, printed with its unit; every other metric a ratio.
[[nodiscard]] std::string fmt_slo_value(model::SloMetric metric, double value) {
  const bool is_time = is_latency(metric) || metric == model::SloMetric::staleness;
  return is_time ? fmt_duration(value) : num(value);
}

// A limit in its own kind: a duration with a unit (queue_lag's 1s too), a ratio as is.
[[nodiscard]] std::string fmt_limit(const std::variant<SimTime, double>& max) {
  return std::visit(Overloaded{[](SimTime limit) { return fmt_duration(limit); },
                               [](double limit) { return num(limit); }},
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

// Clients are sources with no queue: their queue columns show "-". The rho cells are
// coloured by the node's own zone thresholds.
[[nodiscard]] Row node_row(const NodeSummary& node, const graph::ResolvedNode& resolved,
                           bool color) {
  if (resolved.spec.type == model::NodeType::client)
    return {{node.id}, {"-"}, {"-"}, {"-"}, {"-"}, {"-"}, {"-"}};
  const auto colour_of = [&](double rho) {
    return color ? ansi_for(fluid::zone_of(rho, resolved.capacity)) : std::string_view{};
  };
  return {{node.id},
          {num(node.mean_rho), colour_of(node.mean_rho)},
          {num(node.peak_rho), colour_of(node.peak_rho)},
          {with_si_prefix(node.peak_backlog)},
          {with_si_prefix(node.total_dropped)},
          {with_si_prefix(node.total_rejected)},
          {num(node.red_time_s)},
          {node.unavailable ? "unavailable" : ""}};
}

[[nodiscard]] std::string node_table(const Summary& summary, bool color) {
  std::vector<Row> rows{{{"node"},
                         {"mean_rho"},
                         {"peak_rho"},
                         {"peak_backlog"},
                         {"dropped"},
                         {"rejected"},
                         {"red_s"}}};
  const auto nodes = summary.nodes();
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    rows.push_back(node_row(nodes[i], summary.graph().nodes()[i], color));
  }
  return "nodes\n" + aligned(rows);
}

// A database's replication, which the node table has no columns for: the mean staleness, the
// writes one shard's primary committed per second (what each of its replicas must replay) and
// the share of them one replica missed (a replica that misses any falls behind without bound).
// Only when the run has a database.
[[nodiscard]] std::string replication_table(const Summary& summary) {
  std::vector<Row> rows{{{"database"},
                         {"staleness"},
                         {"committed writes/s per shard"},
                         {"missed writes/s per replica"}}};
  for (const auto& node : summary.nodes()) {
    if (!node.mean_replication_dropped_per_s) continue;
    rows.push_back({{node.id},
                    {fmt_duration(node.mean_staleness_s.value_or(0.0))},
                    {with_si_prefix(node.mean_committed_writes_per_s.value_or(0.0))},
                    {with_si_prefix(*node.mean_replication_dropped_per_s)}});
  }
  if (rows.size() == 1) return {};
  return "replication\n" + aligned(rows);
}

[[nodiscard]] std::string op_table(const Summary& summary) {
  std::vector<Row> rows{
      {{"client.op"}, {"offered/s"}, {"mean"}, {"p50"}, {"p95"}, {"p99"}, {"(estimated)"}}};
  for (const auto& op : summary.ops()) {
    rows.push_back({{op.client + "." + op.op},
                    {with_si_prefix(op.offered_per_s)},
                    {fmt_duration(op.mean_s)},
                    {fmt_duration(op.p50_s)},
                    {fmt_duration(op.p95_s)},
                    {fmt_duration(op.p99_s)}});
  }
  return "ops (percentiles estimated: lognormal fit to the response moments)\n" + aligned(rows);
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

[[nodiscard]] std::string slo_table(std::span<const SloResult> slos, bool color) {
  if (slos.empty()) return {};
  std::vector<Row> rows{{{"status"}, {"metric"}, {"node"}, {"op"}, {"observed"}, {"max"}}};
  for (const auto& r : slos) {
    // C++23 std::optional::transform: applies the function to a present value and passes
    // an empty optional through, replacing `observed ? f(*observed) : nullopt`.
    const auto observed =
        r.observed.transform([&](double v) { return fmt_slo_value(r.slo.metric, v); });
    Row row{status_cell(r.status, color), {std::string{model::to_string(r.slo.metric)}},
            {r.slo.node.value_or("-")},   {r.slo.op.value_or("-")},
            {observed.value_or("-")},     {fmt_limit(r.slo.max)}};
    if (!r.note.empty()) row.push_back({r.note});  // a seventh, unlabelled column: why
    rows.push_back(std::move(row));
  }
  return "slo\n" + aligned(rows, Align::all_left);
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
  return std::isfinite(value) ? shortest(value) : "null";
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
      json_field("rejected", json_number(n.total_rejected)),
      json_field("red_s", json_number(n.red_time_s)),
      json_field("unavailable", n.unavailable ? "true" : "false"),
      // null for a node with no lagging copies, and for an infinite one
      json_field("staleness_s", n.mean_staleness_s ? json_number(*n.mean_staleness_s) : "null"),
      // per replica and per shard; null for every node that is not a database
      json_field("replication_dropped_per_s",
                 json_optional_number(n.mean_replication_dropped_per_s)),
      json_field("committed_writes_per_s", json_optional_number(n.mean_committed_writes_per_s))};
  return json_object(fields);
}

[[nodiscard]] std::string json_op(const OpSummary& op) {
  const std::array fields{json_field("client", json_string(op.client)),
                          json_field("op", json_string(op.op)),
                          json_field("offered_per_s", json_number(op.offered_per_s)),
                          json_field("answered_per_s", json_number(op.answered_per_s)),
                          json_field("mean_s", json_number(op.mean_s)),
                          json_field("p50_s", json_number(op.p50_s)),
                          json_field("p95_s", json_number(op.p95_s)),
                          json_field("p99_s", json_number(op.p99_s))};
  return json_object(fields);
}

// `observed` and `max` are in seconds for latency SLOs, plain ratios otherwise.
[[nodiscard]] std::string json_slo(const SloResult& r) {
  const std::array fields{json_field("metric", json_string(model::to_string(r.slo.metric))),
                          json_field("node", json_optional_string(r.slo.node)),
                          json_field("op", json_optional_string(r.slo.op)),
                          json_field("observed", r.observed ? json_number(*r.observed) : "null"),
                          json_field("max", json_number(limit_value(r.slo.max))),
                          json_field("status", json_string(status_name(r.status))),
                          json_field("note", r.note.empty() ? "null" : json_string(r.note))};
  return json_object(fields);
}

// C++20 std::views::transform + C++23 std::ranges::to: map every element through `f` and
// collect the results into a vector, replacing a reserve + push_back loop.
template <class T, class F>
[[nodiscard]] std::vector<std::string> map_to_strings(std::span<const T> items, F f) {
  return items | std::views::transform(f) | std::ranges::to<std::vector<std::string>>();
}

// ---- explain ----

[[nodiscard]] std::string_view provenance(defaults::Source source) {
  using enum defaults::Source;
  switch (source) {
    case scenario:
      return "(scenario)";
    case type_default:
      return "(default)";
    case dc_default:
      return "(datacenter default)";
  }
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
         fmt_duration(s.duration) + "  step " + fmt_duration(s.step) + "  warmup " +
         fmt_duration(s.warmup) + "  seed " + std::to_string(s.seed) + "\n" +
         "datacenters: " + join(s.datacenters, ", ") + "\n";
}

// ---- the rate curve in one line ----

[[nodiscard]] std::string two_digits(std::int64_t value) {
  return (value < 10 ? "0" : "") + std::to_string(value);
}

// A point in simulated time as a scenario writes it: "20:00" for a whole minute of the
// first day (t = 0 is midnight, phase 2a refinement 1), otherwise a duration ("36h", "90s").
// Integer nanoseconds, so 19:59:59.9 is never rounded to "20:00".
[[nodiscard]] std::string clock_or_duration(SimTime t) {
  constexpr auto minute_ns = (1_min).count();
  constexpr auto day_ns = (24_h).count();
  const auto ns = t.count();
  if (ns < 0 || ns >= day_ns || ns % minute_ns != 0) return fmt_duration(t);
  const auto minutes = ns / minute_ns;
  return two_digits(minutes / 60) + ":" + two_digits(minutes % 60);
}

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
         " P=" + fmt_duration(wave.period) + " A=" + num(wave.amplitude) + " peak " +
         clock_or_duration(wave.peak_at);
}

[[nodiscard]] std::string ramp_summary(const workload::Ramp& ramp) {
  return "ramp " + fmt_rate(ramp.from.per_second) + "->" + fmt_rate(ramp.to.per_second) + " over " +
         fmt_duration(ramp.over);
}

[[nodiscard]] std::string noise_summary(const workload::Noise& noise) {
  return "noise σ=" + num(noise.sigma) + " τ=" + fmt_duration(noise.correlation);
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

// A client's datacenter, its rate curve and its op mix (fixed shares of the rate).
[[nodiscard]] std::string client_details(const model::NodeSpec& spec) {
  std::vector<std::string> parts{"dc=" + spec.dc};
  if (!spec.workload) return join(parts, "  ");
  std::vector<std::string> shares;
  for (const auto& [op, share] : spec.workload->op_shares)
    shares.push_back(op + " " + percent(share));
  parts.push_back("rate: " + curve_summary(*spec.workload));
  parts.push_back("ops: " + join(shares, ", "));
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
    parts.push_back(op + " " + num(defaults::op_cost(spec, op), 6));
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
    stated.push_back(op + " " + num(units, 6));
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
// drained route (weight 0) sends nothing, so its ring does not count (as there).
[[nodiscard]] int ring_vnodes(const graph::Graph& g, std::size_t target) {
  int fewest = 0;  // fewest vnodes = largest imbalance 1 + 1/sqrt(v)
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

// What a load balancer's policy means for this node (plan 2a refinement 5): a balancer's
// own policy, and on the node behind it `pooled` (least_connections) or the hottest
// replica's `imbalance` (consistent_hash).
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
    text += "  imbalance=" + num(node.imbalance, 3) + " (vnodes " +
            std::to_string(ring_vnodes(g, index)) + ")";
  }
  return text;
}

// A rate limiter's algorithm and its numbers with the defaults applied (plan 2a refinement
// 6): `algorithm=token_bucket limit=1k/s burst=500 backend=counters`. A bucket shows its
// burst, a windowed algorithm its window (the one number that sizes each).
[[nodiscard]] std::string limiter_summary(const model::NodeSpec& spec) {
  if (spec.type != model::NodeType::rate_limiter || !spec.limit) return {};
  const auto algorithm = spec.algorithm.value_or(model::LimiterAlgorithm::token_bucket);
  std::string text = "  algorithm=" + std::string{model::to_string(algorithm)} +
                     " limit=" + fmt_rate(spec.limit->per_second);
  text += model::is_windowed(algorithm)
              ? " window=" + fmt_duration(model::limiter_window(spec)) + if_defaulted(spec.window)
              : " burst=" + num(model::limiter_burst(spec), 10) + if_defaulted(spec.burst);
  if (spec.backend) text += " backend=" + *spec.backend;
  return text;
}

// What an id generator's algorithm stands for (plan 2b refinement 6; book ch. 7), with the
// numbers the algorithm gives one replica (defaults::for_id_algorithm), so a student sees WHY
// the node has the capacity it has: `algorithm=snowflake (4.096M/s per replica by design)`,
// `algorithm=ticket_server (one database: 1k/s, spec §6 writes)`, `algorithm=uuid (unbounded:
// 1G/s)`. The algorithm's number even when the scenario states its own `capacity`, which the
// node line then shows tagged `(scenario)`: the reader sees the design's figure and the
// override, and the figure is then called the default it is, so that it does not read as a
// second capacity: `algorithm=ticket_server (one database; default 1k/s, spec §6 writes)`.
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
  const std::string shape = keys.zipf ? "zipf " + num(*keys.zipf) : "uniform";
  return shape + " over " + fmt_count(keys.space) + " keys";
}

// "u", "a, b": the clients a che hit ratio took its key distribution from.
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
  return num(node.hit_ratio->value, stated->che ? 3 : 6);
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

// A cache's model (plan 2a refinement 7): `hit_ratio=0.8 (fixed) strategy=cache_aside
// write_ops=write backend=db keys=20M eviction=lru ttl=5m`, the keys the scenario left out
// omitted (strategy and write_ops always have a value: their defaults). A che hit ratio shows
// its provenance instead of `(fixed)` (hit_ratio_summary).
// `hit_ratio=0.8 (fixed)`, `hit_ratio=0.732 (che: ...)`, `hit_ratio=che` (no number) or
// `hit_ratio=none`: what a cache and a CDN both open with.
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
  if (spec.ttl) text += " ttl=" + fmt_duration(*spec.ttl);
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

// A CDN's model (plan 2b refinement 4), shown as a cache's is: `hit_ratio=0.95 (fixed) misses →
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
  if (spec.ttl) text += " ttl=" + fmt_duration(*spec.ttl);
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
  const std::string lag = "  lag=" + fmt_duration(spec.replication_lag.value_or(SimTime{0}));
  if (!spec.replication_lag) return lag + " (default)";
  return spec.replicas < 2 ? lag + " (unused: no replicas)" : lag;
}

// A SQL database's model (plan 2b refinement 3): `primary+2 replicas  shards=4  lag=50ms
// pool=32  write_ops=insert,update`, the defaults applied and marked: one shard, no lag; `pool`
// only when stated (the queue numbers say whether it binds: `capacity=400/s (pool 4 / 10ms)
// ... c=4 (pool)`), and the write ops always (the database default delete,insert,update,write,
// or the scenario's).
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
              fmt_duration(node.capacity.service_time_s) + ")",
          provenance(node.service_time_source), "(pool)", provenance(queue_source)};
}

// A rate in a node's capacity units: "1.5k/s" (requests) for most nodes, "100k MB/s" for a node
// that counts megabytes (defaults::is_byte_sized, plan 2c refinement 2), whose capacity, load
// and checks are all in MB.
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

[[nodiscard]] std::string server_details(const graph::ResolvedNode& node) {
  const auto& spec = node.spec;
  const auto& cap = node.capacity;
  const auto source = queue_provenance(node);
  const std::array parts{
      "dc=" + spec.dc,
      "replicas=" + std::to_string(spec.replicas),
      "capacity=" + fmt_units(spec, cap.service_rate.per_second) + " " + source.capacity,
      "service_time=" + fmt_duration(cap.service_time_s) + std::string{per_unit(spec)} + " " +
          std::string{source.service_time},
      "c=" + std::to_string(cap.concurrency) + std::string{per_slot(spec)} + " " +
          std::string{source.concurrency},
      "queue_limit=" + num(cap.queue_limit, 6) + std::string{queued_unit(spec)} + " " +
          std::string{source.queue_limit},
      "variability=" + num(cap.service_variability, 3),
      "burstiness=" + num(cap.burstiness, 3)};
  return join(parts, "  ");
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

[[nodiscard]] std::string explain_nodes(const graph::Graph& g) {
  std::vector<Row> rows;
  for (std::size_t i = 0; i < g.nodes().size(); ++i) {
    const auto& node = g.nodes()[i];
    const bool client = node.spec.type == model::NodeType::client;
    rows.push_back(
        {{node.spec.id},
         {std::string{model::to_string(node.spec.type)}},
         {client ? client_details(node.spec)
                 : server_details(node) + cost_summary(g, i) + size_summary(g, i) +
                       blob_store_summary(g, i) + balancing_summary(g, i) +
                       limiter_summary(node.spec) + cache_summary(g, i) + cdn_summary(g, i) +
                       database_summary(node) + id_generator_summary(node.spec)}});
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

[[nodiscard]] std::string link_details(const graph::Graph& g, const graph::ResolvedLink& link) {
  std::vector<std::string> parts{
      "one_way=" + fmt_duration(link.one_way) + " " + link_latency_source(g, link),
      "delay_steps=" + std::to_string(link.delay_steps)};
  if (!link.ops.empty()) parts.push_back("ops=" + join(link.ops, ","));
  if (link.weight != 1.0) parts.push_back("weight=" + num(link.weight));
  if (link.feedback) parts.emplace_back("[feedback]");
  return join(parts, "  ");
}

[[nodiscard]] std::string link_name(const graph::Graph& g, const graph::ResolvedLink& link) {
  return g.nodes()[link.from].spec.id + " -> " + g.nodes()[link.to].spec.id;
}

[[nodiscard]] std::string explain_links(const graph::Graph& g) {
  std::vector<Row> rows;
  for (const auto& link : g.links()) {
    rows.push_back(
        {{link_name(g, link)}, {std::string{model::to_string(link.dir)}}, {link_details(g, link)}});
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
[[nodiscard]] std::vector<OpLoad> client_load(const graph::Graph& g) {
  std::vector<OpLoad> load(g.nodes().size());
  const auto duration = g.scenario().duration;
  for (std::size_t client = 0; client < g.nodes().size(); ++client) {
    const auto& workload = g.nodes()[client].spec.workload;
    if (!workload) continue;
    const double peak = peak_rate(workload->curve, duration);
    for (const auto& route : g.client_routes(client)) {
      const auto share =
          std::ranges::find(workload->op_shares, route.op, &std::pair<std::string, double>::first);
      if (share == workload->op_shares.end()) continue;
      load[route.target][route.op] += peak * share->second * route.share;
    }
  }
  return load;
}

// Capacity units one request of `op` uses at node `i`: its megabytes at a byte-sized node, else
// its `cost` entry, else 5 for a database's write, else 1 (graph::op_cost, the rule the
// behaviours' cost() follow), so explain prices every op as a run does, on any graph, including
// types the engine does not simulate yet.
[[nodiscard]] double cost_of(const graph::Graph& g, std::size_t i, const std::string& op) {
  return graph::op_cost(g, i, op);
}

// Each forwarding node's (load balancer's, rate limiter's) routes per op that can reach
// it, as Graph::routes_for gives them, drained routes (share 0) left out: they carry
// nothing. routes_for builds a fresh vector per call, so the table is built once per
// paper_load rather than once per op, node and pass.
using RoutesByOp = std::map<std::string, std::vector<graph::Route>, std::less<>>;
using ForwardRoutes = std::vector<RoutesByOp>;  // per node index; empty for non-forwarders

[[nodiscard]] ForwardRoutes forward_routes(const graph::Graph& g) {
  ForwardRoutes table(g.nodes().size());
  for (std::size_t node = 0; node < g.nodes().size(); ++node) {
    if (!graph::forwards_by_link(g.nodes()[node].spec.type)) continue;
    for (const auto& op : g.arriving_ops(node)) {
      auto& routes = table[node][op];
      for (const auto& route : g.routes_for(node, op)) {
        if (route.share > 0.0) routes.push_back(route);
      }
    }
  }
  return table;
}

// The routes `op` takes out of forwarding node `from`; none for an op that cannot reach it.
[[nodiscard]] std::span<const graph::Route> routes_of(const ForwardRoutes& table, std::size_t from,
                                                      std::string_view op) {
  const auto found = table[from].find(op);
  if (found == table[from].end()) return {};
  return found->second;
}

// Forwards what reached load balancer `from` over its routes, each op split by the
// shares Graph::routes_for gives (as LoadBalancerBehaviour::route does).
void forward_balanced(const graph::Graph& g, const ForwardRoutes& table, std::size_t from,
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

// Forwards along a cache's or a blob store's `then` steps (what happens after it answers: ch.
// 13's browser cache sends its misses on to the servers; an upload's event starts transcoding):
// rate × factor × Graph::then_fraction, the whole flow for an unconditional step (at a blob store,
// every write and no read) and the misses (1 − h of a read) for an `on: miss` one. A share of 0
// sends nothing, not a 0/s entry. The same multiplications as CacheBehaviour::route (plan 2b
// refinement 5) and BlobStoreBehaviour::route.
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

// What a cache or a CDN passes on: to its store (a CDN's origin) and, for a cache, along its
// `then` steps (a CDN has none). The hits stop here.
void forward_cached(const graph::Graph& g, std::size_t from, std::vector<OpLoad>& load,
                    std::vector<OpLoad>& fed_back) {
  const OpLoad arriving = load[from];  // a copy, as in forward_steps
  forward_to_backend(g, from, arriving, load, fed_back);
  forward_then_steps(g, from, arriving, load, fed_back);
}

// What a blob store sends on: one event per write along each `then` step, rate × factor, and
// nothing for a read (plan 2c refinements 6-7, as BlobStoreBehaviour::route does; S3 has no event
// for a GET). On paper nothing is dropped, so every write that arrives is served and notifies.
void forward_events(const graph::Graph& g, std::size_t from, std::vector<OpLoad>& load,
                    std::vector<OpLoad>& fed_back) {
  const OpLoad arriving = load[from];  // a copy, as in forward_steps
  forward_then_steps(g, from, arriving, load, fed_back);
}

// Forwards what reached node `from` along its resolved `ops` hops (rate × factor × the share
// of requests that take the hop: 1, or a cache's miss fraction 1 − h for an `on: miss`
// step, whose factor includes its lookup's, so every missed lookup is fetched;
// graph::ServiceHop), as ServiceBehaviour::route does, or over a load balancer's or a
// rate limiter's routes, or to a cache's store, or along a blob store's events. Flow over a
// feedback link goes to `fed_back`: its target came earlier in the order, so it counts in the
// next pass.
void forward_steps(const graph::Graph& g, const ForwardRoutes& table, std::size_t from,
                   std::vector<OpLoad>& load, std::vector<OpLoad>& fed_back) {
  const auto& spec = g.nodes()[from].spec;
  if (spec.type == model::NodeType::client) return;  // client routes are in client_load
  if (spec.type == model::NodeType::load_balancer) {
    forward_balanced(g, table, from, load, fed_back);
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
  if (spec.type == model::NodeType::blob_store) {
    forward_events(g, from, load, fed_back);
    return;
  }
  const OpLoad arriving = load[from];  // a copy: a step may target `from` itself
  for (const auto& [op, rate] : arriving) {
    for (const graph::ServiceHop& hop : g.service_hops(from, op)) {
      if (hop.fraction <= 0.0) continue;  // never taken (on: failure, or a cache that always hits)
      const auto link = g.demand_link(from, hop.target);
      if (!link) continue;  // Graph::build proved it exists; skip defensively
      auto& destination = g.links()[*link].feedback ? fed_back : load;
      destination[hop.target][hop.op] += rate * hop.factor * hop.fraction;
    }
  }
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
};

// One pass: what the clients send plus what the feedback links carried in the previous
// pass (`fed_back`, replaced by this pass's), pushed through every node in topological
// order. Returns the total offered load per node in capacity units (Σ rate_op × cost_op,
// plan 2a refinement 4), the quantity the node's capacity is a rate of. Forwarding stays
// in requests: a cost prices work at one node, it does not multiply requests.
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
    forward_steps(g, table, index, load, next_fed_back);
  }
  fed_back = std::move(next_fed_back);
  std::vector<double> totals;
  for (std::size_t i = 0; i < load.size(); ++i) {
    // std::ranges::fold_left (see forward_limited), each op weighed by its cost.
    totals.push_back(std::ranges::fold_left(load[i], 0.0, [&](double sum, const auto& entry) {
      return sum + entry.second * cost_of(g, i, entry.first);
    }));
  }
  return totals;
}

// No total moved by more than the tolerance (NaN never counts as unchanged).
[[nodiscard]] bool settled(std::span<const double> before, std::span<const double> after) {
  // C++23 std::views::zip: walks two ranges in lockstep and yields pairs of references,
  // replacing an index loop over both spans (and stops at the shorter one).
  return std::ranges::all_of(std::views::zip(before, after), [](const auto& pair) {
    const auto [b, a] = pair;
    return a == b || std::abs(a - b) <= paper_tolerance * std::abs(a);
  });
}

[[nodiscard]] bool all_finite(std::span<const double> totals) {
  return std::ranges::all_of(totals, [](double t) { return std::isfinite(t); });
}

// The paper calculation (System Design Interview ch. 2, back-of-the-envelope QPS): the
// offered load at every node, propagated from the clients through route shares and step
// factors in topological order. No queueing, no drops, no delays: it is what a student
// would do by hand, and what the long-run estimate shows below saturation.
// A feedback loop is solved by fixed-point iteration: each pass carries the loop's flow
// one lap further, so a loop of gain g (the fraction of its flow it sends round again)
// approaches input / (1 - g), the geometric series' sum, after about
// log(tolerance) / log(g) passes (40 for g = 0.5). A loop of gain >= 1 has no fixed point:
// its totals grow until the pass cap or until they overflow; either way the last finite
// pass is returned, unconverged. Unlike estimate, nothing caps a node at its capacity here.
[[nodiscard]] PaperLoad paper_load(const graph::Graph& g) {
  const auto offered = client_load(g);
  const auto table = forward_routes(g);
  std::vector<OpLoad> fed_back(g.nodes().size());
  std::vector<OpLoad> by_op;
  auto totals = paper_pass(g, table, offered, fed_back, by_op);
  for (int pass = 1; pass < max_paper_passes; ++pass) {
    std::vector<OpLoad> next_by_op;
    auto next = paper_pass(g, table, offered, fed_back, next_by_op);
    if (!all_finite(next)) {
      return {.totals = std::move(totals), .by_op = std::move(by_op), .converged = false};
    }
    if (settled(totals, next)) {
      return {.totals = std::move(next), .by_op = std::move(next_by_op), .converged = true};
    }
    totals = std::move(next);
    by_op = std::move(next_by_op);
  }
  return {.totals = std::move(totals), .by_op = std::move(by_op), .converged = false};
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
// The paper load (paper_pass, forward_steps) forwards from a client's routes, along a service's
// (or any node's) `ops` steps, through load balancers and rate limiters over their links
// (graph::forwards_by_link), from caches and CDNs on their misses (graph::hits_or_misses) and
// along a blob store's `then` steps, its event notifications (plan 2c Task 4).
// It does not follow a worker pool's `then`, nor a message queue's consumers (phase 2c; a
// worker pool's work comes from the queue it consumes, over a link, not along a step), nor the
// `then` of a notification fan-out or a URL frontier (types without a phase yet). A node fed
// through one of those misses that part of its load: with nothing else it shows 0 offered load, a
// missing number, not a zero, and a verdict on it ("0%, green") would read as "this tier is idle";
// with some load from elsewhere its percentage is a lower bound, and a green or yellow verdict on
// it would read as "this tier has room" (review of 3e23477, I1: a node fed 1/s by a client and
// 10k/s through a blob store printed "(0.05%, green)"). Its check line says so instead.

// Until when the paper load leaves unfollowed what a node of `type` sends on: "phase 2c", "not
// modelled yet", or nothing for a type it follows (or that sends nothing on).
[[nodiscard]] std::optional<std::string_view> unfollowed_until(model::NodeType type) {
  using enum model::NodeType;  // C++20, as in provenance
  if (type == message_queue || type == worker_pool) return "phase 2c";
  if (type == notification_fanout || type == frontier) return "not modelled yet";
  return std::nullopt;
}

// Where `from` sends on what it receives. For a type the paper load does not follow: the targets
// of its `then` steps, the flow it would send; for one with none (a message queue, whose
// consumers are only linked to it: `consumes` is checked by the loader but not kept), the nodes
// its links carry demand to. For every other node: its demand edges (steps, client routes, a
// balancer's links, a cache's backend, a blob store's events), what the paper load follows from
// it.
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
// nearest first: youtube's transcoders, behind transcode_q, have [message_queue]; metadata_db,
// also written by the completion handler, [worker_pool, message_queue]. Empty for a node
// whose load the paper pass has in full. A node passes on its own type (if it is such a type)
// and everything on its way, so the mark crosses ordinary nodes too (the web crawler's parser,
// behind the downloader). A node in a loop through such a type has it on its way as well: the
// load that comes round is missing too. Breadth-first from every such node at once, which puts
// the nearest types first; a node is visited again only when its list grows, which happens at
// most once per type, so a loop ends.
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

// "load not propagated: message_queue is phase 2c)" or, with several types, grouped by how long
// each stays unfollowed, in the order they come: "load not propagated: worker_pool is phase 2c,
// frontier is not modelled yet)", "... worker_pool and message_queue are phase 2c)".
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
  const std::string imbalance = "imbalance " + num(node.imbalance, 3) + ")";
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

// Utilization at the paper load: peak offered / (capacity per replica × replicas, × shards
// for a sharded database: every shard is a full set of copies), with the node's own zone
// thresholds (a database's busiest copy's, headline_zone), and the hottest copy's behind a hash
// ring. Both are in capacity units (requests when every cost is 1); a database's are labelled
// so, "peak offered 1.5k units/s", as its per-copy checks are (a write costs 5 of them), and a
// byte-sized node's are megabytes, "peak offered 14.08k MB/s (281.6/s)", the requests beside
// them (plan 2c refinement 2: the request's own size in MB, Graph::request_bytes, so the
// percentage is the bytes' and youtube's cdn_us is at 14.1 %).
// Capacity per replica × replicas, × shards for a sharded database (every shard is a full set
// of copies), in capacity units per second.
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

// "peak offered 1.5k/s vs 2k/s total capacity  ": what every check line opens with.
[[nodiscard]] std::string check_head(const graph::ResolvedNode& node, double offered,
                                     const OpLoad& arriving) {
  return "peak offered " + offered_text(node, offered, arriving) + " vs " +
         fmt_units(node.spec, total_capacity(node)) + " total capacity  ";
}

// Why a node's capacity gets no verdict, or nothing when it gets one: a kv_cluster's `nodes`,
// `replication` and quorums are read and ignored until plan 2c Task 10 (schema), so its capacity
// is ONE box's (`capacity: 10k/s` is per node in labs/kv-store, of eight, each write landing on
// N = 3 of them). A percentage against it would be a verdict on a model the simulator does not
// have: labs/kv-store printed 300 %, red.
[[nodiscard]] std::optional<std::string> unchecked_capacity(const graph::ResolvedNode& node) {
  if (node.spec.type == model::NodeType::kv_cluster) {
    return std::string{"kv_cluster nodes, replication and quorums: phase 3"};
  }
  return std::nullopt;
}

[[nodiscard]] std::string check_details(const graph::ResolvedNode& node, double offered,
                                        const OpLoad& arriving,
                                        const std::optional<graph::DatabaseCopyLoads>& loads) {
  const double capacity = total_capacity(node);
  const std::string head = check_head(node, offered, arriving);
  if (const auto unchecked = unchecked_capacity(node)) {
    return head + "(not checked: " + *unchecked + ")";
  }
  if (capacity <= 0.0) return head + "(no capacity, red)" + hottest_copy_check(node, offered);
  const double rho = offered / capacity;
  return head + "(" + percent(rho) + ", " + headline_zone(node, rho, loads) + ")" +
         hottest_copy_check(node, offered);
}

// The check of a node whose paper load misses what comes through `blocked` (blocked_on_the_way):
//  - nothing reached it: the head and the mark, no percentage and no zone,
//    "(load not propagated: message_queue is phase 2c)";
//  - some load reached it: that load's percentage as a lower bound, "(≥0.05%; load not
//    propagated: ...)". No zone below red: the real load is higher, so green or yellow might not
//    hold, but a red lower bound is red for certain, "(≥150%, red; ...)" (a database's at the
//    busiest copy, headline_zone);
//  - a capacity with no verdict (unchecked_capacity): its own "not checked" note
//    (check_details), then the mark.
// No per-copy checks: each would be a verdict on the same partial number.
[[nodiscard]] std::string partial_check(const graph::ResolvedNode& node, double offered,
                                        const OpLoad& arriving,
                                        std::span<const model::NodeType> blocked) {
  const std::string head = check_head(node, offered, arriving);
  const std::string mark = unpropagated_mark(blocked);
  const bool reached = offered > 0.0 || !std::isfinite(offered);
  if (!reached) return head + "(" + mark;
  if (unchecked_capacity(node)) {
    return check_details(node, offered, arriving, std::nullopt) + "  (lower bound; " + mark;
  }
  const double capacity = total_capacity(node);
  if (capacity <= 0.0) return head + "(no capacity, red; " + mark;
  const double rho = offered / capacity;
  const std::string zone = headline_zone(node, rho, database_loads(node, arriving));
  // C++20 std::string::starts_with: a prefix test in one call, replacing
  // `compare(0, prefix.size(), prefix) == 0` or `rfind(prefix, 0) == 0`.
  const std::string red = zone.starts_with("red") ? ", " + zone : "";
  return head + "(≥" + percent(rho) + red + "; " + mark;
}

// One node's check line: the head and its verdict, or, for a node part or all of whose load the
// paper load could not follow (blocked_on_the_way), partial_check's lower bound or mark.
[[nodiscard]] std::string check_line(const graph::Graph& g, std::size_t i, const PaperLoad& load,
                                     const std::vector<bool>& upper_bound,
                                     std::span<const model::NodeType> blocked) {
  const auto& node = g.nodes()[i];
  const std::string note =
      upper_bound[i] ? " (hit ratio unknown: counted as 0 — an upper bound)" : "";
  if (!blocked.empty()) return partial_check(node, load.totals[i], load.by_op[i], blocked) + note;
  // A capacity with no verdict (unchecked_capacity) gets none per copy either.
  const auto loads = unchecked_capacity(node) ? std::nullopt : database_loads(node, load.by_op[i]);
  return check_details(node, load.totals[i], load.by_op[i], loads) + database_check(node, loads) +
         note;
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
  const std::string note = load.converged ? "" : "  (no steady state: a loop of gain >= 1)\n";
  const std::string header = "checks (peak offered load over the first " +
                             fmt_duration(g.scenario().duration) + ", no queueing):\n";
  return header + aligned(rows, Align::all_left) + note;
}

// ---- CSV ----

[[nodiscard]] std::string csv_server_row(const std::string& t_s, const std::string& id,
                                         const engine::NodeSample& n) {
  const std::array fields{t_s,
                          id,
                          num(n.demand_per_s, 6),
                          num(n.drain_per_s, 6),
                          num(n.rho, 6),
                          num(n.backlog, 6),
                          num(n.dropped_per_s, 6),
                          num(n.rejected_per_s, 6),
                          num(n.latency.mean_s * 1000.0, 6),
                          std::string{zone_name(n.zone)}};
  return join(fields, ",") + "\n";
}

// A client is a source: no queue, so zeros where a server has numbers and zone `none`.
[[nodiscard]] std::string csv_client_row(const std::string& t_s, const std::string& id) {
  return t_s + "," + id + ",0,0,0,0,0,0,0,none\n";
}

// One (client, op) of a step: what it offered, what came back answered, and the mean
// response of an answered request in milliseconds (`inf` when the path never answers).
[[nodiscard]] std::string csv_op_row(const std::string& t_s, const std::string& client,
                                     const engine::ResponseSample& r) {
  const std::array fields{t_s,
                          client,
                          r.op,
                          num(r.offered.per_second, 6),
                          num(r.answered.per_second, 6),
                          num(r.response.mean_s * 1000.0, 6)};
  return join(fields, ",") + "\n";
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
}

// A step whose response is infinite (a node on the path never answers) poisons the whole
// op: no finite average describes it. It is flagged instead of accumulated, so no
// 0 × inf = NaN can reach the sums.
void Summary::OpTotals::add(const engine::ResponseSample& sample, double dt_s) {
  offered += sample.offered.per_second * dt_s;
  answered += sample.answered.per_second * dt_s;
  const auto& response = sample.response;
  if (!std::isfinite(response.mean_s) || !std::isfinite(response.variance_s2)) {
    unavailable = true;
    return;
  }
  // Weighted by the requests ANSWERED (plan 2a refinement 8): a request a limiter refused
  // or a full queue dropped never saw this latency, so it must not weigh it. In a step where
  // a limiter refuses 3 of 4 requests, only the admitted quarter counts.
  by_answered.add(sample.answered.per_second * dt_s, response.mean_s, response.variance_s2);
  by_time.add(dt_s, response.mean_s, response.variance_s2);
}

Summary::Summary(const graph::Graph& g, SimTime warmup)
    : graph_{&g}, warmup_{warmup}, nodes_(g.nodes().size()) {
  // One entry per (client, op), in the engine's order: clients by node index, ops by name.
  for (std::size_t client = 0; client < g.nodes().size(); ++client) {
    const auto& workload = g.nodes()[client].spec.workload;
    if (!workload) continue;
    std::vector<std::string> ops;
    for (const auto& [op, share] : workload->op_shares) ops.push_back(op);
    std::ranges::sort(ops);
    for (auto& op : ops) ops_.push_back(OpTotals{.client = client, .op = std::move(op)});
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

void Summary::observe(const engine::StepRecord& record) {
  const double dt_s = step_length_s(record.t);
  if (record.t.count() < warmup_.count()) return;  // warmup: the queues are still filling
  ++observed_steps_;
  observed_s_ += dt_s;
  for (auto [totals, sample] : std::views::zip(nodes_, record.nodes)) totals.add(sample, dt_s);
  for (std::size_t k = 0; k < record.responses.size(); ++k) {
    const auto& sample = record.responses[k];
    totals_for(sample, k).add(sample, dt_s);
  }
}

std::vector<NodeSummary> Summary::nodes() const {
  std::vector<NodeSummary> out;
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    const auto& t = nodes_[i];
    out.push_back(NodeSummary{.id = graph_->nodes()[i].spec.id,
                              .mean_rho = observed_s_ > 0.0 ? t.rho_time / observed_s_ : 0.0,
                              .peak_rho = t.peak_rho,
                              .peak_backlog = t.peak_backlog,
                              .total_dropped = t.dropped,
                              .total_rejected = t.rejected,
                              .red_time_s = t.red_time_s,
                              .unavailable = t.unavailable});
    if (t.staleness_seen_s > 0.0)
      out.back().mean_staleness_s = t.staleness_time / t.staleness_seen_s;
    if (t.replication_seen_s > 0.0) {
      out.back().mean_replication_dropped_per_s = t.replication_dropped / t.replication_seen_s;
      out.back().mean_committed_writes_per_s = t.committed_writes / t.replication_seen_s;
    }
  }
  return out;
}

std::vector<OpSummary> Summary::ops() const {
  std::vector<OpSummary> out;
  for (const auto& t : ops_) {
    OpSummary op{.client = graph_->nodes()[t.client].spec.id,
                 .op = t.op,
                 .offered_per_s = observed_s_ > 0.0 ? t.offered / observed_s_ : 0.0,
                 .answered_per_s = observed_s_ > 0.0 ? t.answered / observed_s_ : 0.0};
    const Moments& m = t.by_answered.weight > 0.0 ? t.by_answered : t.by_time;
    if (t.unavailable) {
      op.mean_s = op.p50_s = op.p95_s = op.p99_s = infinity;
    } else if (m.weight > 0.0) {
      // Law of total variance over the steps: Var X = E[X²] − (E[X])², where E[X²] already
      // holds each step's own spread (v) and the spread between step means (m²). Assumes a
      // request's step is chosen in proportion to the requests answered in it.
      op.mean_s = m.weighted_mean / m.weight;
      const double variance = m.weighted_square / m.weight - op.mean_s * op.mean_s;
      // Lognormal moment fit (Fenton-Wilkinson, see queueing::lognormal_quantile): an
      // estimate, which is why every renderer labels the percentiles "estimated".
      op.p50_s = queueing::lognormal_quantile(op.mean_s, variance, queueing::z_p50);
      op.p95_s = queueing::lognormal_quantile(op.mean_s, variance, queueing::z_p95);
      op.p99_s = queueing::lognormal_quantile(op.mean_s, variance, queueing::z_p99);
    }
    out.push_back(std::move(op));
  }
  return out;
}

// Per node, not per scenario: a node's drops are a fraction of what it was sent, so the
// ratio stays in [0, 1] even when fan-out (ch. 11) multiplies the client's requests 100
// times before they reach it. Dividing by what the clients offered would not be a ratio.
// Drops only: a rate limiter's refusals are its policy, not a failure (reject_rate).
double Summary::NodeTotals::drop_rate() const {
  return demand > 0.0 ? dropped / demand : 0.0;
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

double Summary::drop_rate(std::string_view node) const {
  return node_rate(node, &NodeTotals::drop_rate);
}

double Summary::drop_rate() const {
  return nodes_.empty() ? 0.0 : nodes_[worst_index(&NodeTotals::drop_rate)].drop_rate();
}

std::string_view Summary::worst_drop_node() const {
  if (drop_rate() <= 0.0) return {};
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
  for (const auto& slo : slos) results.push_back(evaluate_slo(summary, nodes, ops, slo));
  return results;
}

bool all_pass(std::span<const SloResult> results) {
  return std::ranges::none_of(results,
                              [](const SloResult& r) { return r.status == SloStatus::fail; });
}

// ---- renderers ----

// With no observed step there are no node or op figures to show, only the skipped SLOs.
std::string render_table(const Summary& summary, std::span<const SloResult> slos, bool color) {
  if (summary.observed_steps() == 0) return table_header(summary) + slo_table(slos, color);
  return table_header(summary) + node_table(summary, color) + replication_table(summary) +
         op_table(summary) + slo_table(slos, color);
}

// With no observed step the drop and reject rates are null: there is no data, not a rate of 0.
std::string render_json(const Summary& summary, std::span<const SloResult> slos) {
  const auto nodes = summary.nodes();
  const auto ops = summary.ops();
  const bool observed = summary.observed_steps() > 0;
  const auto worst = summary.worst_drop_node();
  const auto worst_rejecting = summary.worst_reject_node();
  const std::array lines{
      "  " + json_field("scenario", json_string(summary.scenario_name())),
      "  " + json_field("steps", std::to_string(summary.observed_steps())),
      "  " + json_field("drop_rate", observed ? json_number(summary.drop_rate()) : "null"),
      "  " + json_field("worst_drop_node", worst.empty() ? "null" : json_string(worst)),
      "  " + json_field("reject_rate", observed ? json_number(summary.reject_rate()) : "null"),
      "  " + json_field("worst_reject_node",
                        worst_rejecting.empty() ? "null" : json_string(worst_rejecting)),
      json_array("nodes", map_to_strings(std::span<const NodeSummary>{nodes}, json_node)),
      json_array("ops", map_to_strings(std::span<const OpSummary>{ops}, json_op)),
      json_array("slo", map_to_strings(slos, json_slo))};
  return "{\n" + join(lines, ",\n") + "\n}";
}

std::string format_number(double value) {
  return shortest(value);
}

std::string render_explain(const graph::Graph& g) {
  return explain_header(g.scenario()) + explain_nodes(g) + explain_links(g) + explain_order(g) +
         explain_checks(g);
}

// ---- SeriesWriter ----

SeriesWriter::SeriesWriter(std::ostream& out, const graph::Graph& g) : out_{&out} {
  for (const auto& node : g.nodes()) {
    ids_.push_back(node.spec.id);
    is_client_.push_back(node.spec.type == model::NodeType::client);
  }
  *out_ << "t_s,node,demand_per_s,drain_per_s,rho,backlog,drops_per_s,rejected_per_s,"
           "latency_ms,zone\n";
}

// CSV is for machines, so every numeric column always holds a number:
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
// - a client row writes 0 in every numeric column (a source has no queue) and `none` as its
//   zone, so a plot can filter clients out by zone;
// - an infinite latency is written as `inf` (which spreadsheet and plotting tools parse),
//   unlike the table ("unavailable") and JSON (null), read by people or by parsers
//   without an infinity.
void SeriesWriter::observe(const engine::StepRecord& record) {
  const auto t_s = shortest(seconds(record.t));
  for (std::size_t i = 0; i < ids_.size() && i < record.nodes.size(); ++i) {
    *out_ << (is_client_[i] ? csv_client_row(t_s, ids_[i])
                            : csv_server_row(t_s, ids_[i], record.nodes[i]));
  }
}

// ---- OpSeriesWriter ----

OpSeriesWriter::OpSeriesWriter(std::ostream& out, const graph::Graph& g) : out_{&out} {
  for (const auto& node : g.nodes()) ids_.push_back(node.spec.id);
  *out_ << "t_s,client,op,offered_per_s,answered_per_s,mean_ms\n";
}

// One row per response sample, in the record's order (clients by node index, ops by name), with
// the node series' conventions (SeriesWriter::observe): `t_s` in its shortest form, 6
// significant digits, `inf` for a mean of a path that never answers. answered = offered × the
// fraction the path delivered (engine::ResponseSample): offered minus what a limiter refused or
// a queue dropped on the way this step, never above offered; a request still waiting in a
// growing queue is not lost, so a backlog alone leaves answered = offered. A sample whose client is
// not a node of the writer's graph is a programmer error (a record of another simulation): `at`
// throws.
void OpSeriesWriter::observe(const engine::StepRecord& record) {
  const auto t_s = shortest(seconds(record.t));
  for (const auto& sample : record.responses) {
    *out_ << csv_op_row(t_s, ids_.at(sample.client), sample);
  }
}

}  // namespace simsys::report
