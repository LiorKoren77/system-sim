// Module `simsys.sweep`, implementation unit: the `--param` and `--metric` grammars, the two
// phases of a sweep (load every point, then simulate every point) on worker threads, and the two
// renderings. <thread> and <atomic> are included here and nowhere else (docs/study/threads.md).
//
// Numbers are written with std::to_chars, never with a std::format width or precision: this unit
// imports simsys.core, whose <chrono> brings <format>, and GCC 14.2 then throws std::format_error
// at run time on `{:.3}` (CLAUDE.md §3). Columns are padded by hand.
module;
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <expected>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

// C++20 module implementation unit: `module simsys.sweep;` (no `export`) makes this file part of
// the named module. It sees every declaration of sweep.cppm, exports nothing, and may include
// heavy headers (here <thread>) without making every importer parse them.
module simsys.sweep;

import simsys.config;
import simsys.core;
import simsys.engine;
import simsys.graph;
import simsys.model;
import simsys.report;

namespace simsys::sweep {

namespace {

// ---- numbers as text ------------------------------------------------------------------------

// `value` with at most `digits` significant digits, trailing zeros dropped (0.1, 500, 1.61).
// std::to_chars (C++17): locale-independent, allocation-free, and free of the GCC 14.2 format
// defect above.
std::string significant(double value, int digits) {
  std::array<char, 64> buffer{};
  const auto written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                     std::chars_format::general, digits);
  return std::string{buffer.data(), written.ptr};
}

// `value` with exactly two decimals (466.67): how far a point is from a whole number.
std::string two_decimals(double value) {
  std::array<char, 400> buffer{};  // fixed notation of any finite double fits in 400 chars
  const auto written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                     std::chars_format::fixed, 2);
  return std::string{buffer.data(), written.ptr};
}

// ---- --param: <dot.path>=<from>..<to>:<n> ------------------------------------------------------

constexpr std::string_view param_grammar = "<dot.path>=<from>..<to>:<n>";

std::unexpected<std::string> param_error(std::string_view what) {
  // C++23 std::unexpected<E>: wraps an error so it converts to any std::expected<T, E>.
  return std::unexpected("--param: " + std::string{what});
}

// The last segment of a dot path: the key itself (`workers` of `nodes.w.workers`).
std::string_view last_key(std::string_view path) {
  const auto dot = path.rfind('.');
  return dot == std::string_view::npos ? path : path.substr(dot + 1);
}

bool is_all_digits(std::string_view text) {
  // C++20 ranges algorithms (std::ranges::all_of, find, any_of): take the range itself instead of
  // a begin/end pair, and accept a projection (`&NodeSpec::id`) instead of a comparison lambda.
  return !text.empty() && std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; });
}

// "a.b.c" -> {"a", "b", "c"}; empty pieces are kept ("a..b" has an empty middle one).
std::vector<std::string_view> split_dots(std::string_view text) {
  std::vector<std::string_view> pieces;
  std::size_t start = 0;
  for (auto dot = text.find('.'); dot != std::string_view::npos; dot = text.find('.', start)) {
    pieces.push_back(text.substr(start, dot - start));
    start = dot + 1;
  }
  pieces.push_back(text.substr(start));
  return pieces;
}

// The path's own mistakes, before any file is read: an empty key, or a list index. Whether the
// keys exist is the loader's question (config::load with an Override).
std::optional<std::string> path_problem(std::string_view path) {
  for (const auto key : split_dots(path)) {
    if (key.empty()) return std::string{path} + ": a dot path has no empty keys";
    if (is_all_digits(key)) {
      return std::string{path} + ": '" + std::string{key} +
             "' would index a list; a dot path goes through maps only (a list entry has no name)";
    }
  }
  return std::nullopt;
}

// One end of a range: a number and the unit text after it ("100/s" -> 100, "/s"; "400" -> 400,
// "").
struct RangeEnd {
  double number{};
  std::string unit{};
};

// An end starts with a digit or a minus sign: from_chars would also read `.5ms` (0.5 ms), which is
// what the upper end of `1ms...5ms` would be, a different sweep from the one typed.
bool starts_like_a_number(std::string_view text) {
  return !text.empty() && (text.front() == '-' || (text.front() >= '0' && text.front() <= '9'));
}

std::expected<RangeEnd, std::string> parse_end(std::string_view text) {
  double number{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
  if (!starts_like_a_number(text) || error != std::errc{} || !std::isfinite(number)) {
    return param_error("'" + std::string{text} + "' is not a number followed by a unit");
  }
  // C++20 designated initializers: each member named where the value is built, so the reader
  // need not remember the order of the struct's members.
  return RangeEnd{.number = number, .unit = std::string{end, text.data() + text.size()}};
}

// The spec split at its separators: `path` = `from` .. `to` : `count`.
struct SpecParts {
  std::string_view path;
  std::string_view range;  // everything after '=', echoed in messages as typed
  std::string_view from;
  std::string_view to;
  std::string_view count;
};

std::expected<SpecParts, std::string> split_spec(std::string_view spec) {
  const auto bad_shape =
      param_error("'" + std::string{spec} + "' is not " + std::string{param_grammar});
  const auto equals = spec.find('=');
  if (equals == std::string_view::npos || equals == 0) return bad_shape;
  const auto range = spec.substr(equals + 1);
  const auto colon = range.rfind(':');
  if (colon == std::string_view::npos) return bad_shape;
  const auto ends = range.substr(0, colon);
  const auto dots = ends.find("..");
  if (dots == std::string_view::npos) return bad_shape;
  return SpecParts{.path = spec.substr(0, equals),
                   .range = range,
                   .from = ends.substr(0, dots),
                   .to = ends.substr(dots + 2),
                   .count = range.substr(colon + 1)};
}

// The most points a sweep runs. Chosen, not from the book: each point is a whole simulation, so
// 10,000 is far beyond any sweep a person reads, yet small enough that a typo of a few zeros
// (`:1000000000000`) is refused before its values are expanded into memory.
constexpr std::size_t max_points = 10'000;

// The number of points: a whole number, at least 2 (one point is a run, not a sweep), at most
// max_points.
std::expected<std::size_t, std::string> parse_count(const SpecParts& parts) {
  std::size_t count{};
  const auto* end = parts.count.data() + parts.count.size();
  const auto [stop, error] = std::from_chars(parts.count.data(), end, count);
  if (error != std::errc{} || stop != end) {
    return param_error(std::string{parts.range} + ": the count after ':' must be a whole number");
  }
  if (count < 2) {
    return param_error(std::string{parts.range} + ": a range needs at least 2 points");
  }
  if (count > max_points) {
    return param_error(std::string{parts.range} + ": at most " + std::to_string(max_points) +
                       " points");
  }
  return count;
}

// Point k of n evenly spaced points from `from` to `to`, both included. The multiplication comes
// before the division so whole ranges give whole points exactly (400 + 200 x 1 / 2 = 500).
double point_at(double from, double to, std::size_t k, std::size_t n) {
  return from + (to - from) * static_cast<double>(k) / static_cast<double>(n - 1);
}

bool is_whole(double value) {
  return std::abs(value - std::round(value)) <= 1e-9 * std::max(1.0, std::abs(value));
}

// A point's text in the scenario's syntax: a whole key's point as an integer, any other with 12
// significant digits, which hides the last-bit noise of the arithmetic (0.30000000000000004).
std::string point_text(double value, bool whole) {
  return significant(whole ? std::round(value) : value, whole ? 17 : 12);
}

std::expected<std::vector<std::string>, std::string> expand(const SpecParts& parts,
                                                            const RangeEnd& from,
                                                            const RangeEnd& to, std::size_t n) {
  // The loader knows which keys it reads as whole numbers (466.67 workers is no pool).
  const bool whole = config::counts_whole_things(parts.path);
  std::vector<std::string> values;
  for (std::size_t k = 0; k < n; ++k) {
    const double value = point_at(from.number, to.number, k, n);
    if (whole && !is_whole(value)) {
      return param_error(std::string{last_key(parts.path)} + " must be whole (" +
                         std::string{parts.range} + " gives " + two_decimals(value) + from.unit +
                         ")");
    }
    values.push_back(point_text(value, whole) + from.unit);
  }
  return values;
}

// ---- --metric --------------------------------------------------------------------------------

constexpr std::string_view metric_grammar = "<client>.<op>.mean, <node>.mean_rho or <queue>.lag";

// C++20 std::span<const T>: a non-owning view of contiguous elements (pointer + size), so a
// function reads a vector or an array without a copy and without being a template.
std::optional<SweepMetric> metric_of(std::string_view name,
                                     std::span<const std::string_view> pieces) {
  // No `&std::string_view::empty` as the predicate: taking the address of a standard library
  // member function is not allowed (it may be overloaded or change between versions).
  if (std::ranges::any_of(pieces, [](std::string_view piece) { return piece.empty(); })) {
    return std::nullopt;
  }
  const std::string text{name};
  if (pieces.size() == 3 && pieces[2] == "mean") {
    return SweepMetric{.name = text,
                       .kind = MetricKind::mean_latency,
                       .node = std::string{pieces[0]},
                       .op = std::string{pieces[1]}};
  }
  if (pieces.size() != 2) return std::nullopt;
  if (pieces[1] == "mean_rho") {
    return SweepMetric{.name = text, .kind = MetricKind::mean_rho, .node = std::string{pieces[0]}};
  }
  if (pieces[1] == "lag") {
    return SweepMetric{
        .name = text, .kind = MetricKind::peak_queue_lag, .node = std::string{pieces[0]}};
  }
  return std::nullopt;
}

// Why `metric` cannot be read off `node`, one check per kind of metric; nothing when it can.
std::optional<std::string> latency_problem(const model::NodeSpec& node, const SweepMetric& metric) {
  if (node.type != model::NodeType::client || !node.workload) {
    return "'" + node.id + "' has type " + std::string{model::to_string(node.type)} +
           ", not client";
  }
  // C++23 std::ranges::contains with a projection: is `op` the first member of some pair?
  if (std::ranges::contains(node.workload->op_shares, metric.op,
                            &std::pair<std::string, double>::first)) {
    return std::nullopt;
  }
  return "client '" + node.id + "' sends no op '" + metric.op + "'";
}

std::optional<std::string> utilization_problem(const model::NodeSpec& node) {
  if (node.type != model::NodeType::client) return std::nullopt;
  return "'" + node.id + "' is a client, which serves nothing, so it has no utilization";
}

std::optional<std::string> lag_problem(const model::NodeSpec& node) {
  if (node.type == model::NodeType::message_queue) return std::nullopt;
  return "'" + node.id + "' has type " + std::string{model::to_string(node.type)} +
         ", not message_queue: only a message queue has a lag";
}

std::optional<std::string> problem_with_node(const model::NodeSpec& node,
                                             const SweepMetric& metric) {
  switch (metric.kind) {
    case MetricKind::mean_latency:
      return latency_problem(node, metric);
    case MetricKind::mean_rho:
      return utilization_problem(node);
    case MetricKind::peak_queue_lag:
      return lag_problem(node);
  }
  return std::nullopt;
}

// Why `metric` cannot be read off a run of `scenario`, or nothing: a node it does not have, an op
// its client does not send, or a node of the wrong type for the metric.
std::optional<std::string> metric_problem(const model::Scenario& scenario,
                                          const SweepMetric& metric) {
  const auto node = std::ranges::find(scenario.nodes, metric.node, &model::NodeSpec::id);
  auto problem = node == scenario.nodes.end()
                     ? std::optional{"the scenario has no node '" + metric.node + "'"}
                     : problem_with_node(*node, metric);
  if (!problem) return std::nullopt;
  return "--metric " + metric.name + ": " + *problem;
}

std::optional<std::string> first_metric_problem(const model::Scenario& scenario,
                                                std::span<const SweepMetric> metrics) {
  for (const auto& metric : metrics) {
    if (auto problem = metric_problem(scenario, metric)) return problem;
  }
  return std::nullopt;
}

// What a metric reads off one run: the report's own Summary (the numbers `simsys run --report
// json` prints), so a sweep computes nothing the report does not. A NaN for a value the run did
// not produce: the mean latency of an op of which nothing was answered (the report prints `-`
// there: a mean over no answers is no latency), a queue lag of a run that observed no step.
constexpr double no_value = std::numeric_limits<double>::quiet_NaN();

double read_metric(std::span<const report::NodeSummary> nodes,
                   std::span<const report::OpSummary> ops, const SweepMetric& metric) {
  if (metric.kind == MetricKind::mean_latency) {
    for (const auto& op : ops) {
      if (op.client != metric.node || op.op != metric.op) continue;
      return op.answered_per_s > 0.0 ? op.mean_s : no_value;
    }
    return no_value;
  }
  const auto node = std::ranges::find(nodes, metric.node, &report::NodeSummary::id);
  if (node == nodes.end()) return no_value;
  if (metric.kind == MetricKind::mean_rho) return node->mean_rho;
  return node->peak_queue_lag_s.value_or(no_value);
}

// ---- one point ---------------------------------------------------------------------------------

// "point 2 (nodes.users.rate.base=-1/s): ": points count from 1, as a person counts them.
std::string point_label(const SweepParam& param, std::size_t index) {
  return "point " + std::to_string(index + 1) + " (" + param.path + "=" + param.values[index] +
         "): ";
}

// Phase 1 for one point: the scenario with the point's value in place, checked by the loader
// and resolved by the graph builder. No simulation yet.
std::expected<graph::Graph, std::string> prepare_point(const std::string& path,
                                                       const SweepParam& param, std::size_t index) {
  const std::array overrides{config::Override{.path = param.path, .value = param.values[index]}};
  // C++23 monadic std::expected: transform_error rewrites only an error, and_then runs the next
  // fallible stage only on a value; no `if (!x) return ...` after each stage.
  return config::load(path, overrides)
      .transform_error(config::describe)
      .and_then([&](model::Scenario scenario) {
        return graph::Graph::build(std::move(scenario))
            .transform_error([&](const graph::EngineError& error) {
              return path + ": error: " + error.message;
            });
      })
      .transform_error(
          [&](const std::string& message) { return point_label(param, index) + message; });
}

// Phase 2 for one point: a run of the whole scenario, as `simsys run` does it (its duration, its
// step, its warmup), and each metric read off the summary. The Simulation, the Summary and the
// graph belong to this point alone: nothing here is shared with another thread.
std::expected<std::vector<double>, std::string> measure_point(
    const std::string& path, const graph::Graph& g, std::span<const SweepMetric> metrics) {
  auto sim = engine::Simulation::create(g);
  if (!sim) return std::unexpected(path + ": error: " + sim.error().message);
  const auto& scenario = g.scenario();
  report::Summary summary{g, scenario.warmup};
  sim->run({.duration = scenario.duration, .step = scenario.step},
           [&](const engine::StepRecord& record) { summary.observe(record); });
  const auto nodes = summary.nodes();
  const auto ops = summary.ops();
  std::vector<double> numbers;
  for (const auto& metric : metrics) numbers.push_back(read_metric(nodes, ops, metric));
  return numbers;
}

// ---- the points in parallel --------------------------------------------------------------------

// One worker: takes the next unclaimed index until none is left. fetch_add returns the old value
// and increments in one indivisible step, so no two workers ever get the same index and none is
// skipped, without a lock.
void pull_indices(std::atomic<std::size_t>& next, std::size_t count,
                  const std::function<void(std::size_t)>& work) {
  for (auto index = next.fetch_add(1); index < count; index = next.fetch_add(1)) work(index);
}

// Calls work(0) ... work(count - 1), each exactly once, on `jobs` threads (at least 1, at most
// one per index), and returns when all are done.
// Pattern: Thread pool / work queue — a fixed set of workers pulls tasks from one shared queue;
// the queue here is just an atomic counter over the indices 0..count-1, since the tasks are known
// up front and need no other state. A worker that finishes a short point takes the next one, so a
// slow point does not hold back a whole share of them (as a static split into `jobs` slices
// would).
void run_in_parallel(std::size_t count, std::size_t jobs,
                     const std::function<void(std::size_t)>& work) {
  // C++11 std::atomic<T>: a value that threads read and modify without a data race; fetch_add is
  // one indivisible read-modify-write. A plain std::size_t incremented by several threads would
  // be a data race: undefined behaviour, in practice two workers running one point.
  std::atomic<std::size_t> next{0};
  // C++20 std::jthread: a std::thread that joins in its destructor (and carries a stop_token).
  // A std::thread still joinable at destruction calls std::terminate, so every exit path had to
  // join by hand; here leaving the scope is the join. The joins also publish each worker's
  // writes to this thread: everything a worker did happens-before its join returns.
  std::vector<std::jthread> workers;
  const auto threads = std::clamp<std::size_t>(jobs, 1, std::max<std::size_t>(count, 1));
  workers.reserve(threads);
  for (std::size_t t = 0; t < threads; ++t) {
    workers.emplace_back(pull_indices, std::ref(next), count, std::cref(work));
  }
}  // the jthreads join here

// What a phase produced for every point: a value, or the message of its failure. Slot i is
// written only by the worker that claimed index i, and only read after the joins: distinct
// elements of a std::vector are distinct memory locations, so the writes cannot race (this would
// NOT hold for std::vector<bool>, which packs elements into shared bytes), and no lock is needed.
template <typename T>
struct PerPoint {
  std::vector<std::optional<T>> values;
  std::vector<std::string> errors;  // "" where the point succeeded

  explicit PerPoint(std::size_t count) : values(count), errors(count) {}

  void store(std::size_t index, std::expected<T, std::string> result) {
    if (result) {
      values[index].emplace(std::move(*result));
    } else {
      errors[index] = std::move(result.error());
    }
  }

  // The failure of the lowest-numbered failing point: which point a worker meets first depends
  // on the threads, this does not.
  [[nodiscard]] std::optional<std::string> first_error() const {
    const auto failed =
        std::ranges::find_if(errors, [](const std::string& error) { return !error.empty(); });
    if (failed == errors.end()) return std::nullopt;
    return *failed;
  }
};

// Runs `stage(i)` for every point on `jobs` workers and keeps every result by index.
template <typename T, typename Stage>
PerPoint<T> for_each_point(std::size_t count, std::size_t jobs, const Stage& stage) {
  PerPoint<T> results{count};
  run_in_parallel(count, jobs, [&](std::size_t index) { results.store(index, stage(index)); });
  return results;
}

// ---- rendering ---------------------------------------------------------------------------------

// A cell for people, as the report's tables print the same number (report::format_duration and
// format_lag: "1.61ms", "<1us"); "-" for a number the run did not produce.
std::string human_cell(const SweepMetric& metric, double value) {
  if (std::isnan(value)) return "-";
  switch (metric.kind) {
    case MetricKind::mean_latency:
      return report::format_duration(value);
    case MetricKind::mean_rho:
      return significant(value, 3);
    case MetricKind::peak_queue_lag:
      return report::format_lag(value);
  }
  return "-";
}

// A cell for programs: the shortest text that reads back to the same double, empty for a number
// the run did not produce (a CSV reader's missing value).
std::string csv_cell(double value) {
  return std::isnan(value) ? std::string{} : report::format_number(value);
}

// The table's cells, the header first.
using Cells = std::vector<std::vector<std::string>>;

Cells table_cells(const SweepParam& param, std::span<const SweepMetric> metrics,
                  std::span<const SweepRow> rows) {
  Cells cells;
  auto& header = cells.emplace_back(std::vector<std::string>{param.path});
  for (const auto& metric : metrics) header.push_back(metric.name);
  for (const auto& row : rows) {
    auto& line = cells.emplace_back(std::vector<std::string>{row.value});
    for (std::size_t m = 0; m < metrics.size() && m < row.metrics.size(); ++m) {
      line.push_back(human_cell(metrics[m], row.metrics[m]));
    }
  }
  return cells;
}

std::vector<std::size_t> column_widths(const Cells& cells) {
  std::vector<std::size_t> widths;
  for (const auto& line : cells) {
    if (widths.size() < line.size()) widths.resize(line.size());
    for (std::size_t c = 0; c < line.size(); ++c) widths[c] = std::max(widths[c], line[c].size());
  }
  return widths;
}

// The swept value left-aligned, the numbers right-aligned (their units line up), two spaces
// between columns. Padded by hand: no std::format width (CLAUDE.md §3).
std::string table_line(const std::vector<std::string>& line, std::span<const std::size_t> widths) {
  std::string text;
  for (std::size_t c = 0; c < line.size(); ++c) {
    const std::string padding(widths[c] - line[c].size(), ' ');
    if (c == 0) {
      text += line[c] + padding;
    } else {
      text += "  " + padding + line[c];
    }
  }
  return text + "\n";
}

std::string render_table(const Cells& cells) {
  const auto widths = column_widths(cells);
  std::size_t total = 0;
  for (const auto width : widths) total += width;
  total += 2 * (widths.empty() ? 0 : widths.size() - 1);
  std::string text = table_line(cells.front(), widths) + std::string(total, '-') + "\n";
  for (std::size_t r = 1; r < cells.size(); ++r) text += table_line(cells[r], widths);
  return text;
}

std::string render_csv(const SweepParam& param, std::span<const SweepMetric> metrics,
                       std::span<const SweepRow> rows) {
  std::string text = param.path;
  for (const auto& metric : metrics) text += "," + metric.name;
  text += "\n";
  for (const auto& row : rows) {
    text += row.value;
    for (const double number : row.metrics) text += "," + csv_cell(number);
    text += "\n";
  }
  return text;
}

}  // namespace

std::expected<SweepParam, std::string> parse_param(std::string_view spec) {
  const auto parts = split_spec(spec);
  if (!parts) return std::unexpected(parts.error());
  if (auto problem = path_problem(parts->path)) return param_error(*problem);
  const auto count = parse_count(*parts);
  if (!count) return std::unexpected(count.error());
  const auto from = parse_end(parts->from);
  if (!from) return std::unexpected(from.error());
  const auto to = parse_end(parts->to);
  if (!to) return std::unexpected(to.error());
  if (from->unit != to->unit) {
    return param_error(std::string{parts->range} +
                       ": both ends must be the same kind of value in the same unit (" +
                       std::string{parts->from} + " and " + std::string{parts->to} + ")");
  }
  auto values = expand(*parts, *from, *to, *count);
  if (!values) return std::unexpected(values.error());
  return SweepParam{.path = std::string{parts->path}, .values = std::move(*values)};
}

std::expected<SweepMetric, std::string> parse_metric(std::string_view name) {
  const auto pieces = split_dots(name);
  if (auto metric = metric_of(name, pieces)) return *metric;
  return std::unexpected("--metric: '" + std::string{name} + "' is not a metric (write " +
                         std::string{metric_grammar} + ")");
}

// Pattern: Pipeline with early exit — the file, then the metrics against it, then every point's
// scenario, then every point's run; the first stage that fails ends the sweep with one message.
// Every point is loaded before any is simulated, so a bad value at point 9 costs no simulation of
// points 1 to 8.
std::expected<std::vector<SweepRow>, std::string> run_sweep(const std::string& scenario_path,
                                                            const SweepParam& param,
                                                            std::span<const SweepMetric> metrics,
                                                            std::size_t jobs) {
  const auto base = config::load(scenario_path);
  if (!base) return std::unexpected(config::describe(base.error()));
  if (auto problem = first_metric_problem(*base, metrics)) return std::unexpected(*problem);
  // The path's mistakes (a missing key, a list on the way) are the same at every point: reported
  // once, with no point to blame.
  if (const auto path = config::check_override_path(scenario_path, param.path); !path) {
    return std::unexpected(config::describe(path.error()));
  }

  const auto count = param.values.size();
  const auto graphs = for_each_point<graph::Graph>(
      count, jobs, [&](std::size_t i) { return prepare_point(scenario_path, param, i); });
  if (auto error = graphs.first_error()) return std::unexpected(*error);

  const auto numbers = for_each_point<std::vector<double>>(count, jobs, [&](std::size_t i) {
    return measure_point(scenario_path, *graphs.values[i], metrics)
        .transform_error([&](const std::string& e) { return point_label(param, i) + e; });
  });
  if (auto error = numbers.first_error()) return std::unexpected(*error);

  std::vector<SweepRow> rows;
  for (std::size_t i = 0; i < count; ++i) {
    rows.push_back(SweepRow{.value = param.values[i], .metrics = *numbers.values[i]});
  }
  return rows;
}

std::size_t default_jobs() {
  // 0 means "unknown" (the standard allows it): then one worker.
  return std::max(1U, std::thread::hardware_concurrency());
}

std::string render_sweep(const SweepParam& param, std::span<const SweepMetric> metrics,
                         std::span<const SweepRow> rows, SweepFormat format) {
  if (format == SweepFormat::csv) return render_csv(param, metrics, rows);
  return render_table(table_cells(param, metrics, rows));
}

}  // namespace simsys::sweep
