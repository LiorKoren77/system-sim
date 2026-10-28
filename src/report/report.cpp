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

// A count with an SI prefix and four significant digits: 500, 1.5k, 2M.
[[nodiscard]] std::string with_si_prefix(double value) {
  if (!std::isfinite(value)) return "unavailable";
  for (const auto& prefix : si_prefixes) {
    if (std::abs(value) >= prefix.size) return num(value / prefix.size) + std::string{prefix.name};
  }
  return num(value);
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

// What the run observed for the SLO's metric; empty when phase 1 does not model it.
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
    case utilization:
      return mean_utilization(nodes, slo);
    case queue_lag:  // consumer lag of a message queue: phase 2
    case staleness:  // replica/cache staleness: phase 2
      return std::nullopt;
  }
  std::unreachable();
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
  return SloResult{
      .slo = slo, .observed = observed, .status = within ? SloStatus::pass : SloStatus::fail};
}

// ---- table ----

[[nodiscard]] std::string fmt_slo_value(model::SloMetric metric, double value) {
  return is_latency(metric) ? fmt_duration(value) : num(value);
}

// A limit in its own kind: a duration with a unit (queue_lag's 1s too), a ratio as is.
[[nodiscard]] std::string fmt_limit(const std::variant<SimTime, double>& max) {
  return std::visit(Overloaded{[](SimTime limit) { return fmt_duration(limit); },
                               [](double limit) { return num(limit); }},
                    max);
}

// The drop rate shown is the worst node's, named when some node dropped anything.
[[nodiscard]] std::string table_header(const Summary& summary) {
  const std::string head = "scenario " + std::string{summary.scenario_name()} + "  ";
  if (summary.observed_steps() == 0) return head + "0 steps observed: nothing to evaluate\n";
  const auto worst = summary.worst_drop_node();
  return head + std::to_string(summary.observed_steps()) + " steps observed  drop rate " +
         percent(summary.drop_rate()) +
         (worst.empty() ? std::string{} : " (worst: " + std::string{worst} + ")") + "\n";
}

// Clients are sources with no queue: their queue columns show "-". The rho cells are
// coloured by the node's own zone thresholds.
[[nodiscard]] Row node_row(const NodeSummary& node, const graph::ResolvedNode& resolved,
                           bool color) {
  if (resolved.spec.type == model::NodeType::client)
    return {{node.id}, {"-"}, {"-"}, {"-"}, {"-"}, {"-"}};
  const auto colour_of = [&](double rho) {
    return color ? ansi_for(fluid::zone_of(rho, resolved.capacity)) : std::string_view{};
  };
  return {{node.id},
          {num(node.mean_rho), colour_of(node.mean_rho)},
          {num(node.peak_rho), colour_of(node.peak_rho)},
          {with_si_prefix(node.peak_backlog)},
          {with_si_prefix(node.total_dropped)},
          {num(node.red_time_s)},
          {node.unavailable ? "unavailable" : ""}};
}

[[nodiscard]] std::string node_table(const Summary& summary, bool color) {
  std::vector<Row> rows{
      {{"node"}, {"mean_rho"}, {"peak_rho"}, {"peak_backlog"}, {"dropped"}, {"red_s"}}};
  const auto nodes = summary.nodes();
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    rows.push_back(node_row(nodes[i], summary.graph().nodes()[i], color));
  }
  return "nodes\n" + aligned(rows);
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
    rows.push_back({status_cell(r.status, color),
                    {std::string{model::to_string(r.slo.metric)}},
                    {r.slo.node.value_or("-")},
                    {r.slo.op.value_or("-")},
                    {observed.value_or("-")},
                    {fmt_limit(r.slo.max)}});
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
  const std::array fields{json_field("id", json_string(n.id)),
                          json_field("mean_rho", json_number(n.mean_rho)),
                          json_field("peak_rho", json_number(n.peak_rho)),
                          json_field("peak_backlog", json_number(n.peak_backlog)),
                          json_field("dropped", json_number(n.total_dropped)),
                          json_field("red_s", json_number(n.red_time_s)),
                          json_field("unavailable", n.unavailable ? "true" : "false")};
  return json_object(fields);
}

[[nodiscard]] std::string json_op(const OpSummary& op) {
  const std::array fields{json_field("client", json_string(op.client)),
                          json_field("op", json_string(op.op)),
                          json_field("offered_per_s", json_number(op.offered_per_s)),
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
                          json_field("status", json_string(status_name(r.status)))};
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

[[nodiscard]] std::string explain_header(const model::Scenario& s) {
  return "scenario " + s.name + "  version " + std::to_string(s.version) + "  duration " +
         fmt_duration(s.duration) + "  step " + fmt_duration(s.step) + "  warmup " +
         fmt_duration(s.warmup) + "  seed " + std::to_string(s.seed) + "\n" +
         "datacenters: " + join(s.datacenters, ", ") + "\n";
}

// ---- the rate curve in one line ----

[[nodiscard]] std::string_view wave_shape_name(workload::WaveShape shape) {
  using enum workload::WaveShape;
  switch (shape) {
    case sine:
      return "sine";
    case square:
      return "square";
    case diurnal:
      return "diurnal";
  }
  std::unreachable();
}

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
  // C++20 std::ranges::minmax with a projection: the lowest and highest rate of the trace
  // in one pass, comparing by the rate member instead of through a hand-written lambda.
  const auto [low, high] = std::ranges::minmax(at | std::views::values, {}, &Rate::per_second);
  return "points " + std::to_string(at.size()) + " (" + fmt_rate(low.per_second) + ".." +
         fmt_rate(high.per_second) + ")";
}

[[nodiscard]] std::string wave_summary(const workload::Wave& wave) {
  return "wave " + std::string{wave_shape_name(wave.shape)} + " P=" + fmt_duration(wave.period) +
         " A=" + num(wave.amplitude) + " peak " + clock_or_duration(wave.peak_at);
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

[[nodiscard]] std::string server_details(const graph::ResolvedNode& node) {
  const auto& spec = node.spec;
  const auto& cap = node.capacity;
  // The graph keeps no provenance for queue_limit, so it is read off the spec: provenance
  // is about presence, not value. A scenario that writes the default (fluid::Capacity's
  // 1000) still wrote it, and explain says so.
  const auto queue_source =
      spec.advanced.queue_limit ? defaults::Source::scenario : defaults::Source::type_default;
  // c is derived by Little's law unless the scenario sets it (defaults::implied_concurrency).
  const std::string_view concurrency_source =
      spec.advanced.concurrency ? "(scenario)" : "(implied)";
  const std::array parts{
      "dc=" + spec.dc,
      "replicas=" + std::to_string(spec.replicas),
      "capacity=" + fmt_rate(cap.service_rate.per_second) + " " +
          std::string{provenance(node.service_rate_source)},
      "service_time=" + fmt_duration(cap.service_time_s) + " " +
          std::string{provenance(node.service_time_source)},
      "c=" + std::to_string(cap.concurrency) + " " + std::string{concurrency_source},
      "queue_limit=" + num(cap.queue_limit, 6) + " " + std::string{provenance(queue_source)},
      "variability=" + num(cap.service_variability, 3),
      "burstiness=" + num(cap.burstiness, 3)};
  return join(parts, "  ");
}

[[nodiscard]] std::string explain_nodes(const graph::Graph& g) {
  std::vector<Row> rows;
  for (const auto& node : g.nodes()) {
    const bool client = node.spec.type == model::NodeType::client;
    rows.push_back({{node.spec.id},
                    {std::string{model::to_string(node.spec.type)}},
                    {client ? client_details(node.spec) : server_details(node)}});
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

// What the clients send at t = 0, the time `estimate` evaluates: the curve's rate there
// (rate_at, so a points-only client counts too; no noise) × op share × route share on
// each route.
[[nodiscard]] std::vector<OpLoad> client_load(const graph::Graph& g) {
  std::vector<OpLoad> load(g.nodes().size());
  for (std::size_t client = 0; client < g.nodes().size(); ++client) {
    const auto& workload = g.nodes()[client].spec.workload;
    if (!workload) continue;
    const double rate_at_zero = workload->curve.rate_at(SimTime{0}).per_second;
    for (const auto& route : g.client_routes(client)) {
      const auto share =
          std::ranges::find(workload->op_shares, route.op, &std::pair<std::string, double>::first);
      if (share == workload->op_shares.end()) continue;
      load[route.target][route.op] += rate_at_zero * share->second * route.share;
    }
  }
  return load;
}

// Forwards what reached node `from` along its unconditional `ops` steps (rate × factor),
// as ServiceBehaviour::route does. Flow over a feedback link goes to `fed_back`: its
// target came earlier in the order, so it counts in the next pass.
void forward_steps(const graph::Graph& g, std::size_t from, std::vector<OpLoad>& load,
                   std::vector<OpLoad>& fed_back) {
  const auto& spec = g.nodes()[from].spec;
  if (spec.type == model::NodeType::client) return;  // client routes are in client_load
  const OpLoad arriving = load[from];                // a copy: a step may target `from` itself
  for (const auto& [op, rate] : arriving) {
    const auto steps = spec.ops.find(op);
    if (steps == spec.ops.end()) continue;
    for (const auto& step : steps->second) {
      if (step.on != model::StepCondition::always) continue;  // phase 1: never fire
      const auto target = g.index_of(step.to);
      const auto link = target ? g.demand_link(from, *target) : std::nullopt;
      if (!link) continue;  // Graph::build proved both exist; skip defensively
      auto& destination = g.links()[*link].feedback ? fed_back : load;
      destination[*target][step.op] += rate * step.factor;
    }
  }
}

// The paper load's stopping rule, the same as engine::Simulation::estimate's (whose
// constants are internal to the engine, so the numbers are restated here): stop when no
// node's total moved by more than 1e-12 of its value (a few thousand ulps of a double, as
// close to "equal" as rounding allows), or give up after 100'000 passes.
constexpr double paper_tolerance = 1e-12;
constexpr int max_paper_passes = 100'000;

// The offered load at every node, and whether the passes reached a fixed point.
struct PaperLoad {
  std::vector<double> totals;
  bool converged{};
};

// One pass: what the clients send plus what the feedback links carried in the previous
// pass (`fed_back`, replaced by this pass's), pushed through every node in topological
// order. Returns the total offered rate per node.
[[nodiscard]] std::vector<double> paper_pass(const graph::Graph& g,
                                             const std::vector<OpLoad>& offered,
                                             std::vector<OpLoad>& fed_back) {
  auto load = offered;
  for (std::size_t i = 0; i < load.size(); ++i) {
    for (const auto& [op, rate] : fed_back[i]) load[i][op] += rate;
  }
  std::vector<OpLoad> next_fed_back(g.nodes().size());
  for (const auto index : g.topological_order()) forward_steps(g, index, load, next_fed_back);
  fed_back = std::move(next_fed_back);
  std::vector<double> totals;
  for (const auto& ops : load) {
    // C++23 std::ranges::fold_left over C++20 std::views::values: sum the rates of a map
    // without a hand-written loop (and without std::accumulate's int-initial-value trap).
    totals.push_back(std::ranges::fold_left(ops | std::views::values, 0.0, std::plus<>{}));
  }
  return totals;
}

// No total moved by more than the tolerance (NaN never counts as unchanged).
[[nodiscard]] bool settled(std::span<const double> before, std::span<const double> after) {
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
  std::vector<OpLoad> fed_back(g.nodes().size());
  auto totals = paper_pass(g, offered, fed_back);
  for (int pass = 1; pass < max_paper_passes; ++pass) {
    auto next = paper_pass(g, offered, fed_back);
    if (!all_finite(next)) return {.totals = std::move(totals), .converged = false};
    if (settled(totals, next)) return {.totals = std::move(next), .converged = true};
    totals = std::move(next);
  }
  return {.totals = std::move(totals), .converged = false};
}

// Utilization at the paper load: offered / (capacity per replica × replicas), with the
// node's own zone thresholds.
[[nodiscard]] std::string check_details(const graph::ResolvedNode& node, double offered) {
  const double total_capacity = node.capacity.service_rate.per_second * node.spec.replicas;
  const std::string head =
      "offered " + fmt_rate(offered) + " vs " + fmt_rate(total_capacity) + " total capacity  ";
  if (total_capacity <= 0.0) return head + "(no capacity, red)";
  const double rho = offered / total_capacity;
  return head + "(" + percent(rho) + ", " +
         std::string{zone_name(fluid::zone_of(rho, node.capacity))} + ")";
}

[[nodiscard]] std::string explain_checks(const graph::Graph& g) {
  const auto load = paper_load(g);
  std::vector<Row> rows;
  for (std::size_t i = 0; i < g.nodes().size(); ++i) {
    const auto& node = g.nodes()[i];
    if (node.spec.type == model::NodeType::client) continue;  // a source: see its offered=
    rows.push_back({{node.spec.id}, {check_details(node, load.totals[i])}});
  }
  const std::string note = load.converged ? "" : "  (no steady state: a loop of gain >= 1)\n";
  return "checks:\n" + aligned(rows, Align::all_left) + note;
}

// ---- CSV ----

[[nodiscard]] std::string csv_server_row(const std::string& t_s, const std::string& id,
                                         const engine::NodeSample& n) {
  const std::array fields{t_s,
                          id,
                          num(n.rho, 6),
                          num(n.backlog, 6),
                          num(n.dropped_per_s, 6),
                          num(n.latency.mean_s * 1000.0, 6),
                          std::string{zone_name(n.zone)}};
  return join(fields, ",") + "\n";
}

// A client is a source: no queue, so zeros where a server has numbers and zone `none`.
[[nodiscard]] std::string csv_client_row(const std::string& t_s, const std::string& id) {
  return t_s + "," + id + ",0,0,0,0,none\n";
}

}  // namespace

// ---- Summary ----

// Weighted moments of a mixture: each step contributes its mean m and variance v with
// weight w (requests offered in that step), so E[X] = Σ w·m / Σ w and
// E[X²] = Σ w·(v + m²) / Σ w (E[X²] of one component is its variance plus its squared mean).
void Summary::Moments::add(double w, double mean_s, double variance_s2) {
  weight += w;
  weighted_mean += w * mean_s;
  weighted_square += w * (variance_s2 + mean_s * mean_s);
}

void Summary::NodeTotals::add(const engine::NodeSample& sample, double dt_s) {
  rho_time += sample.rho * dt_s;
  demand += sample.demand_per_s * dt_s;
  peak_rho = std::max(peak_rho, sample.rho);
  peak_backlog = std::max(peak_backlog, sample.backlog);
  dropped += sample.dropped_per_s * dt_s;
  if (sample.zone == fluid::Zone::red) red_time_s += dt_s;
  if (!std::isfinite(sample.latency.mean_s)) unavailable = true;
}

// A step whose response is infinite (a node on the path never answers) poisons the whole
// op: no finite average describes it. It is flagged instead of accumulated, so no
// 0 × inf = NaN can reach the sums.
void Summary::OpTotals::add(const engine::ResponseSample& sample, double dt_s) {
  offered += sample.offered.per_second * dt_s;
  const auto& response = sample.response;
  if (!std::isfinite(response.mean_s) || !std::isfinite(response.variance_s2)) {
    unavailable = true;
    return;
  }
  by_offered.add(sample.offered.per_second * dt_s, response.mean_s, response.variance_s2);
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
  // C++23 std::views::zip: walks two ranges in lockstep and yields pairs of references,
  // replacing an index loop over both vectors (and stops at the shorter one).
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
                              .red_time_s = t.red_time_s,
                              .unavailable = t.unavailable});
  }
  return out;
}

std::vector<OpSummary> Summary::ops() const {
  std::vector<OpSummary> out;
  for (const auto& t : ops_) {
    OpSummary op{.client = graph_->nodes()[t.client].spec.id,
                 .op = t.op,
                 .offered_per_s = observed_s_ > 0.0 ? t.offered / observed_s_ : 0.0};
    const Moments& m = t.by_offered.weight > 0.0 ? t.by_offered : t.by_time;
    if (t.unavailable) {
      op.mean_s = op.p50_s = op.p95_s = op.p99_s = infinity;
    } else if (m.weight > 0.0) {
      // Law of total variance over the steps: Var X = E[X²] − (E[X])², where E[X²] already
      // holds each step's own spread (v) and the spread between step means (m²). Assumes a
      // request's step is chosen in proportion to the traffic offered in it.
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
double Summary::NodeTotals::drop_rate() const {
  return demand > 0.0 ? dropped / demand : 0.0;
}

// The first of equal maxima (std::ranges::max_element keeps it), so ties name the
// earliest node. A projection through a member function: std::invoke calls drop_rate().
std::size_t Summary::worst_drop_index() const {
  const auto worst = std::ranges::max_element(nodes_, std::less<>{}, &NodeTotals::drop_rate);
  return static_cast<std::size_t>(worst - nodes_.begin());
}

double Summary::drop_rate(std::string_view node) const {
  const auto index = graph_->index_of(node);
  if (!index) {
    throw std::invalid_argument{"Summary::drop_rate: unknown node '" + std::string{node} + "'"};
  }
  return nodes_[*index].drop_rate();
}

double Summary::drop_rate() const {
  return nodes_.empty() ? 0.0 : nodes_[worst_drop_index()].drop_rate();
}

std::string_view Summary::worst_drop_node() const {
  if (drop_rate() <= 0.0) return {};
  return graph_->nodes()[worst_drop_index()].spec.id;
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
  // C++20 std::ranges::none_of: takes the range itself instead of a begin/end pair.
  return std::ranges::none_of(results,
                              [](const SloResult& r) { return r.status == SloStatus::fail; });
}

// ---- renderers ----

// With no observed step there are no node or op figures to show, only the skipped SLOs.
std::string render_table(const Summary& summary, std::span<const SloResult> slos, bool color) {
  if (summary.observed_steps() == 0) return table_header(summary) + slo_table(slos, color);
  return table_header(summary) + node_table(summary, color) + op_table(summary) +
         slo_table(slos, color);
}

// With no observed step the drop rate is null: there is no data, not a rate of 0.
std::string render_json(const Summary& summary, std::span<const SloResult> slos) {
  const auto nodes = summary.nodes();
  const auto ops = summary.ops();
  const bool observed = summary.observed_steps() > 0;
  const auto worst = summary.worst_drop_node();
  const std::array lines{
      "  " + json_field("scenario", json_string(summary.scenario_name())),
      "  " + json_field("steps", std::to_string(summary.observed_steps())),
      "  " + json_field("drop_rate", observed ? json_number(summary.drop_rate()) : "null"),
      "  " + json_field("worst_drop_node", worst.empty() ? "null" : json_string(worst)),
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
  *out_ << "t_s,node,rho,backlog,drops_per_s,latency_ms,zone\n";
}

// CSV is for machines, so every numeric column always holds a number:
// - `t_s` is the shortest form that reads back to the same double (0.1, 2, 0.35), not a
//   fixed number of decimals that would round a fine step away;
// - a client row writes 0 in rho, backlog, drops_per_s and latency_ms (a source has no
//   queue) and `none` as its zone, so a plot can filter clients out by zone;
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

}  // namespace simsys::report
