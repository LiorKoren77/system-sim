// Module `simsys.sweep`, implementation unit: the `--param` and `--metric` grammars, the two
// phases of a sweep (load every point, then simulate every point) on worker threads, and the two
// renderings. The worker threads are session::run_in_parallel's (session.cpp,
// docs/study/threads.md); <thread> is included here only for default_jobs.
//
// Numbers are written with std::to_chars (simsys::to_chars_text), never with a std::format width or
// precision: this unit imports simsys.core, whose <chrono> brings <format>, and GCC 14.2 then
// throws std::format_error at run time on `{:.3}` (CLAUDE.md §3). Columns are padded by
// report::align_columns, the run tables' own aligner.
module;
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <expected>
#include <functional>  // std::function: session::run_in_parallel's parameter
#include <iterator>    // std::back_inserter: a row's verdict cells after its numbers
#include <limits>
#include <optional>
#include <ranges>  // std::views::cartesian_product: the grid's pairs (grid_points)
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

// C++20 `import`: the compiled interfaces of the modules this unit reads from, instead of
// re-parsing their text as an #include would.
import simsys.config;
import simsys.core;
import simsys.graph;
import simsys.model;
import simsys.report;
import simsys.session;

namespace simsys::sweep {

namespace {

// ---- --param: <dot.path>=<from>..<to>:<n> ------------------------------------------------------

constexpr std::string_view param_grammar = "<dot.path>=<from>..<to>:<n>[:log]";

std::unexpected<std::string> param_error(std::string_view what) {
  // C++23 std::unexpected<E>: wraps an error so it converts to any std::expected<T, E>.
  return std::unexpected("--param: " + std::string{what});
}

// The last segment of a dot path: the key itself (`workers` of `nodes.w.workers`).
std::string_view last_key(std::string_view path) {
  const auto dot = path.rfind('.');
  return dot == std::string_view::npos ? path : path.substr(dot + 1);
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

// C++23 std::expected<T, E>: the parsed end or the message, replacing an exception or an
// out-parameter plus a bool; the caller must look before it uses the value.
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
  std::string_view spacing;  // the word after a second ':' (`log`), empty without one
};

// Whether the text after the range's last ':' is a spacing word (`log`) rather than the count:
// a word starts with a letter, and only counts as one when what precedes it, back to the colon
// before, is a count of digits (`:5:log`). A mistyped count (`:three`) is then still the count,
// and gets the count's own message.
bool is_spacing_word(std::string_view range, std::size_t last_colon) {
  const auto word = range.substr(last_colon + 1);
  const bool starts_with_letter = !word.empty() && ((word.front() >= 'a' && word.front() <= 'z') ||
                                                    (word.front() >= 'A' && word.front() <= 'Z'));
  if (!starts_with_letter || last_colon == 0) return false;
  const auto count_colon = range.rfind(':', last_colon - 1);
  if (count_colon == std::string_view::npos) return false;
  const auto count = range.substr(count_colon + 1, last_colon - count_colon - 1);
  return !count.empty() && std::ranges::all_of(count, [](char c) { return c >= '0' && c <= '9'; });
}

std::expected<SpecParts, std::string> split_spec(std::string_view spec) {
  const auto bad_shape =
      param_error("'" + std::string{spec} + "' is not " + std::string{param_grammar});
  const auto equals = spec.find('=');
  if (equals == std::string_view::npos || equals == 0) return bad_shape;
  const auto range = spec.substr(equals + 1);
  // The colons are found from the right: an end may hold one itself (a clock time, `10:00`).
  const auto last_colon = range.rfind(':');
  if (last_colon == std::string_view::npos) return bad_shape;
  const auto after_last = range.substr(last_colon + 1);
  const bool has_spacing = is_spacing_word(range, last_colon);
  // With a spacing word the count sits between the last two colons, else after the last one.
  const auto count_colon = !has_spacing     ? last_colon
                           : last_colon > 0 ? range.rfind(':', last_colon - 1)
                                            : std::string_view::npos;
  if (count_colon == std::string_view::npos) return bad_shape;
  const auto ends = range.substr(0, count_colon);
  const auto dots = ends.find("..");
  if (dots == std::string_view::npos) return bad_shape;
  return SpecParts{.path = spec.substr(0, equals),
                   .range = range,
                   .from = ends.substr(0, dots),
                   .to = ends.substr(dots + 2),
                   .count = has_spacing
                                ? range.substr(count_colon + 1, last_colon - count_colon - 1)
                                : after_last,
                   .spacing = has_spacing ? after_last : std::string_view{}};
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

// "workers must be whole (400..600:4 gives 466.67)": a whole key's point that is not whole.
std::unexpected<std::string> not_whole(const SpecParts& parts, double value,
                                       std::string_view unit) {
  return param_error(std::string{last_key(parts.path)} + " must be whole (" +
                     std::string{parts.range} + " gives " +
                     to_chars_text(value, std::chars_format::fixed, 2) + std::string{unit} + ")");
}

std::expected<std::vector<std::string>, std::string> expand(const SpecParts& parts,
                                                            const RangeEnd& from,
                                                            const RangeEnd& to, std::size_t n) {
  // The loader knows which keys it reads as whole numbers (466.67 workers is no pool).
  const bool whole = config::counts_whole_things(parts.path);
  std::vector<std::string> values;
  for (std::size_t k = 0; k < n; ++k) {
    const double value = point_at(from.number, to.number, k, n);
    if (whole && !is_whole(value)) return not_whole(parts, value, from.unit);
    values.push_back(point_text(value, whole) + from.unit);
  }
  return values;
}

// Point k of n geometric points from `from` to `to` (both positive), both included: a constant
// ratio r = (to / from)^(1 / (n − 1)) between neighbours, from × r^k, computed as
// exp(ln from + k/(n − 1) × (ln to − ln from)) so that a range as wide as 1e-300..1e300 does not
// overflow `to / from`. The last point is `to` itself, so the floating-point arithmetic cannot
// leave it a bit short (1000^(1/3) = 9.999999999999998).
double geometric_point_at(double from, double to, std::size_t k, std::size_t n) {
  if (k + 1 == n) return to;
  const double share = static_cast<double>(k) / static_cast<double>(n - 1);
  return std::exp(std::log(from) + share * (std::log(to) - std::log(from)));
}

// The `:log` points. A whole key's ends must be whole, as on a linear range (the ends are what
// the user typed); the points between are rounded, since a geometric point is almost never whole
// (a linear one is, if n is chosen for it). Two points that come out alike (rounded, or from equal
// ends) would run the same scenario twice and are refused, naming the first repeat.
std::expected<std::vector<std::string>, std::string> expand_geometric(const SpecParts& parts,
                                                                      const RangeEnd& from,
                                                                      const RangeEnd& to,
                                                                      std::size_t n) {
  const bool whole = config::counts_whole_things(parts.path);
  if (whole && !is_whole(from.number)) return not_whole(parts, from.number, from.unit);
  if (whole && !is_whole(to.number)) return not_whole(parts, to.number, to.unit);
  std::vector<std::string> values;
  for (std::size_t k = 0; k < n; ++k) {
    double value = geometric_point_at(from.number, to.number, k, n);
    if (whole) value = std::round(value);
    auto text = point_text(value, whole) + from.unit;
    if (!values.empty() && values.back() == text) {
      const auto key = std::string{last_key(parts.path)};
      return param_error(key + "=" + std::string{parts.range} + " gives " + text + " twice" +
                         (whole ? " (rounded to whole " + key + ")" : std::string{}) +
                         "; use fewer points");
    }
    values.push_back(std::move(text));
  }
  return values;
}

// ---- --metric --------------------------------------------------------------------------------

constexpr std::string_view metric_grammar =
    "<client>.<op>.mean|p50|p95|p99|attempts|answered, "
    "<node>.mean_rho|peak_rho|drop_rate|reject_rate, "
    "<queue>.lag, <pool>.dead_lettered, <fanout>.deduplicated or <gateway>.connections";

// A metric's last word and what it reads (sweep.cppm's MetricKind); one row each.
struct MetricWord {
  std::string_view word;
  MetricKind kind;
};

// Pattern: Table-driven method — a new metric is one row here (and one case in read_metric), not
// another `if`.
constexpr std::array op_metric_words{MetricWord{"mean", MetricKind::mean_latency},
                                     MetricWord{"p50", MetricKind::p50_latency},
                                     MetricWord{"p95", MetricKind::p95_latency},
                                     MetricWord{"p99", MetricKind::p99_latency},
                                     MetricWord{"attempts", MetricKind::attempts_rate},
                                     MetricWord{"answered", MetricKind::answered_rate}};
constexpr std::array node_metric_words{MetricWord{"mean_rho", MetricKind::mean_rho},
                                       MetricWord{"peak_rho", MetricKind::peak_rho},
                                       MetricWord{"drop_rate", MetricKind::drop_rate},
                                       MetricWord{"reject_rate", MetricKind::reject_rate},
                                       MetricWord{"lag", MetricKind::peak_queue_lag},
                                       MetricWord{"dead_lettered", MetricKind::dead_lettered_rate},
                                       MetricWord{"deduplicated", MetricKind::deduplicated_rate},
                                       MetricWord{"connections", MetricKind::connections}};

// C++20 std::span<const T>: a non-owning view of contiguous elements (pointer + size), so a
// function reads a vector or an array without a copy and without being a template.
std::optional<MetricKind> kind_named(std::span<const MetricWord> words, std::string_view word) {
  // C++20 ranges algorithms (std::ranges::find, find_if, any_of): take the range itself instead
  // of a begin/end pair, and accept a projection (`&MetricWord::word`) instead of a
  // comparison lambda.
  const auto found = std::ranges::find(words, word, &MetricWord::word);
  if (found == words.end()) return std::nullopt;
  return found->kind;
}

std::optional<SweepMetric> metric_of(std::string_view name, std::span<const std::string> pieces) {
  // No `&std::string::empty` as the predicate: taking the address of a standard library member
  // function is not allowed (it may be overloaded or change between versions).
  if (std::ranges::any_of(pieces, [](const std::string& piece) { return piece.empty(); })) {
    return std::nullopt;
  }
  const std::string text{name};
  if (pieces.size() == 3) {
    const auto kind = kind_named(op_metric_words, pieces[2]);
    if (!kind) return std::nullopt;
    return SweepMetric{.name = text, .kind = *kind, .node = pieces[0], .op = pieces[1]};
  }
  if (pieces.size() != 2) return std::nullopt;
  const auto kind = kind_named(node_metric_words, pieces[1]);
  if (!kind) return std::nullopt;
  return SweepMetric{.name = text, .kind = *kind, .node = pieces[0]};
}

// Which of the op table's latency columns a latency metric reads; empty for the others.
std::optional<report::OpLatency> latency_column(MetricKind kind) {
  switch (kind) {
    case MetricKind::mean_latency:
      return report::OpLatency::mean;
    case MetricKind::p50_latency:
      return report::OpLatency::p50;
    case MetricKind::p95_latency:
      return report::OpLatency::p95;
    case MetricKind::p99_latency:
      return report::OpLatency::p99;
    case MetricKind::mean_rho:
    case MetricKind::peak_rho:
    case MetricKind::drop_rate:
    case MetricKind::reject_rate:
    case MetricKind::peak_queue_lag:
    case MetricKind::attempts_rate:
    case MetricKind::answered_rate:
    case MetricKind::dead_lettered_rate:
    case MetricKind::deduplicated_rate:
    case MetricKind::connections:
      return std::nullopt;
  }
  return std::nullopt;
}

// Which of a client op's rates a rate metric reads (OpSummary's attempts_per_s or
// answered_per_s); empty for the others. A pointer to data member (`double OpSummary::*`, C++98)
// names a field without an object: `op->*column` reads that field of one op, so the two rates
// share one reader instead of a switch per kind.
std::optional<double report::OpSummary::*> rate_column(MetricKind kind) {
  if (kind == MetricKind::attempts_rate) return &report::OpSummary::attempts_per_s;
  if (kind == MetricKind::answered_rate) return &report::OpSummary::answered_per_s;
  return std::nullopt;
}

// Why `metric` cannot be read off `node`, one check per kind of metric; nothing when it can.
std::optional<std::string> client_op_problem(const model::NodeSpec& node,
                                             const SweepMetric& metric) {
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

// A client serves nothing: it has no utilization and drops nothing (`what` names the metric).
std::optional<std::string> serving_problem(const model::NodeSpec& node, std::string_view what) {
  if (node.type != model::NodeType::client) return std::nullopt;
  return "'" + node.id + "' is a client, which serves nothing, so it has no " + std::string{what};
}

// A URL frontier has a lag too (phase 3 Task 18): its consumer pool's, as a message queue's.
std::optional<std::string> lag_problem(const model::NodeSpec& node) {
  if (node.type == model::NodeType::message_queue || node.type == model::NodeType::frontier) {
    return std::nullopt;
  }
  return "'" + node.id + "' has type " + std::string{model::to_string(node.type)} +
         ", not message_queue or frontier: only a message queue or a frontier has a lag";
}

std::optional<std::string> dead_letter_problem(const model::NodeSpec& node) {
  if (node.type == model::NodeType::worker_pool) return std::nullopt;
  return "'" + node.id + "' has type " + std::string{model::to_string(node.type)} +
         ", not worker_pool: only a worker pool has dead letters";
}

std::optional<std::string> deduplicated_problem(const model::NodeSpec& node) {
  if (node.type == model::NodeType::notification_fanout) return std::nullopt;
  return "'" + node.id + "' has type " + std::string{model::to_string(node.type)} +
         ", not notification_fanout: only a notification fanout has duplicates";
}

std::optional<std::string> connections_problem(const model::NodeSpec& node) {
  if (node.type == model::NodeType::ws_gateway) return std::nullopt;
  return "'" + node.id + "' has type " + std::string{model::to_string(node.type)} +
         ", not ws_gateway: only a ws_gateway has connections";
}

std::optional<std::string> problem_with_node(const model::NodeSpec& node,
                                             const SweepMetric& metric) {
  switch (metric.kind) {
    case MetricKind::mean_latency:
    case MetricKind::p50_latency:
    case MetricKind::p95_latency:
    case MetricKind::p99_latency:
    case MetricKind::attempts_rate:
    case MetricKind::answered_rate:
      return client_op_problem(node, metric);
    case MetricKind::dead_lettered_rate:
      return dead_letter_problem(node);
    case MetricKind::mean_rho:
    case MetricKind::peak_rho:
      return serving_problem(node, "utilization");
    case MetricKind::drop_rate:
      return serving_problem(node, "drop rate");
    case MetricKind::reject_rate:
      return serving_problem(node, "reject rate");
    case MetricKind::peak_queue_lag:
      return lag_problem(node);
    case MetricKind::connections:
      return connections_problem(node);
    case MetricKind::deduplicated_rate:
      return deduplicated_problem(node);
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
  return "--metric: " + metric.name + ": " + *problem;
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
// not produce: a latency of an op of which nothing was answered (the report prints `-` there: no
// answer, no latency), a queue lag of a run that observed no step.
constexpr double no_value = std::numeric_limits<double>::quiet_NaN();

// One point's numbers and, by index, the percentiles that are the client's timeout rather than a
// latency it saw (SweepRow::beyond_timeout).
struct Measured {
  std::vector<double> numbers;
  std::vector<std::size_t> beyond_timeout;
  std::vector<SloCell> slos{};     // with --slos only
  std::optional<int> exit_code{};  // with --slos only
};

// A client op metric's op, or nothing when the run has no row for it.
const report::OpSummary* op_of(std::span<const report::OpSummary> ops, const SweepMetric& metric) {
  const auto found = std::ranges::find_if(ops, [&](const report::OpSummary& op) {
    return op.client == metric.node && op.op == metric.op;
  });
  return found == ops.end() ? nullptr : &*found;
}

double node_metric(const report::Summary& summary, std::span<const report::NodeSummary> nodes,
                   const SweepMetric& metric) {
  const auto node = std::ranges::find(nodes, metric.node, &report::NodeSummary::id);
  if (node == nodes.end()) return no_value;
  switch (metric.kind) {
    case MetricKind::mean_rho:
      return node->mean_rho;
    case MetricKind::peak_rho:
      return node->peak_rho;
    case MetricKind::drop_rate:
      return summary.drop_rate(metric.node);
    case MetricKind::reject_rate:  // what the behaviour refused, a share of what it was sent
      return summary.reject_rate(metric.node);
    case MetricKind::peak_queue_lag:
      return node->peak_queue_lag_s.value_or(no_value);
    case MetricKind::dead_lettered_rate:  // JSON's dead_lettered_per_s, 0 without retries
      return node->mean_dead_lettered_per_s.value_or(no_value);
    case MetricKind::connections:  // the level at the end of the run, the table's `connections`
      return node->connections.value_or(no_value);
    case MetricKind::deduplicated_rate:  // JSON's deduplicated_per_s, 0 with `dedup: 0`
      return node->mean_deduplicated_per_s.value_or(no_value);
    case MetricKind::mean_latency:  // an op's, read by read_metrics
    case MetricKind::p50_latency:
    case MetricKind::p95_latency:
    case MetricKind::p99_latency:
    case MetricKind::attempts_rate:  // ... and an op's rate
    case MetricKind::answered_rate:
      return no_value;
  }
  return no_value;
}

// A client op's rate, read as the JSON prints it: what the client sent (`attempts_per_s`, its
// retries included) or what came back answered (`answered_per_s`). A NaN when the run has no row.
double op_rate(std::span<const report::OpSummary> ops, const SweepMetric& metric,
               double report::OpSummary::*column) {
  const auto* op = op_of(ops, metric);
  return op != nullptr ? op->*column : no_value;
}

// Every metric of one point's summary, in the order asked.
Measured read_metrics(const report::Summary& summary, std::span<const SweepMetric> metrics) {
  const auto nodes = summary.nodes();
  const auto ops = summary.ops();
  Measured measured;
  for (const auto& metric : metrics) {
    if (const auto rate = rate_column(metric.kind)) {
      measured.numbers.push_back(op_rate(ops, metric, *rate));
      continue;
    }
    const auto column = latency_column(metric.kind);
    if (!column) {
      measured.numbers.push_back(node_metric(summary, nodes, metric));
      continue;
    }
    const auto* op = op_of(ops, metric);
    const bool answered = op != nullptr && op->answered_per_s > 0.0;
    measured.numbers.push_back(answered ? report::op_latency_s(*op, *column) : no_value);
    if (answered && report::latency_beyond_timeout(*op, *column)) {
      measured.beyond_timeout.push_back(measured.numbers.size() - 1);
    }
  }
  return measured;
}

// ---- one point ---------------------------------------------------------------------------------

// Every point's values, one per param, in run order. One param: its values. Two: every pair,
// the first param the outer loop, so (a1, b1) (a1, b2) ... (a2, b1) ...
// Two nested loops, the outer over the first param. C++23 std::views::cartesian_product gives the
// same pairs in the same order as a view of tuples, which this used until 2026-11-10: LLVM's
// standard library (libc++ 20, the one Emscripten uses) has no cartesian_product yet, and one
// version of the code must build with every toolchain (docs/study/ranges.md keeps the example).
// One or two params: grid_problem has refused any other count before this runs.
std::vector<std::vector<std::string>> grid_points(std::span<const SweepParam> params) {
  std::vector<std::vector<std::string>> points;
  if (params.size() == 1) {
    for (const auto& value : params[0].values) points.push_back({value});
    return points;
  }
  for (const auto& outer : params[0].values) {
    for (const auto& inner : params[1].values) points.push_back({outer, inner});
  }
  return points;
}

// "point 2 (nodes.users.rate.base=-1/s): ", or with two keys "point 3 (a=300/s, b=1k/s): ":
// points count from 1, as a person counts them.
std::string point_label(std::span<const SweepParam> params, std::span<const std::string> values,
                        std::size_t index) {
  std::string pairs;
  for (std::size_t k = 0; k < params.size(); ++k) {
    pairs += (k == 0 ? "" : ", ") + params[k].path + "=" + values[k];
  }
  return "point " + std::to_string(index + 1) + " (" + pairs + "): ";
}

// Phase 1 for one point: the scenario with the point's value in place, checked by the loader
// and resolved by the graph builder, exactly as `simsys explain` and `run` load a file
// (session::load_graph). No simulation yet.
std::expected<graph::Graph, std::string> prepare_point(const std::string& path,
                                                       std::span<const SweepParam> params,
                                                       std::span<const std::string> values,
                                                       std::size_t index) {
  std::vector<config::Override> overrides;
  for (std::size_t k = 0; k < params.size(); ++k) {
    overrides.push_back(config::Override{.path = params[k].path, .value = values[k]});
  }
  // C++23 std::expected::transform_error: rewrites only an error (here: the point's label in front
  // of the message), passing a value through.
  return session::load_graph(path, overrides).transform_error([&](const std::string& message) {
    return point_label(params, values, index) + message;
  });
}

// Each SLO's column heading: its metric, then `:node` and `.op` where it names them; a heading that
// an earlier SLO already took gets `#2`, `#3`, ... in file order, so every column has its own name.
std::vector<std::string> slo_headings(std::span<const model::SloSpec> slos) {
  std::vector<std::string> plain;
  std::vector<std::string> headings;
  for (const auto& slo : slos) {
    std::string heading{model::to_string(slo.metric)};
    if (slo.node) heading += ":" + *slo.node;
    if (slo.node && slo.op) heading += "." + *slo.op;
    const auto earlier = std::ranges::count(plain, heading);
    plain.push_back(heading);
    headings.push_back(earlier == 0 ? heading : heading + "#" + std::to_string(earlier + 1));
  }
  return headings;
}

SloVerdict verdict_of(report::SloStatus status) {
  switch (status) {
    case report::SloStatus::pass:
      return SloVerdict::pass;
    case report::SloStatus::fail:
      return SloVerdict::fail;
    case report::SloStatus::skipped:
      return SloVerdict::skipped;
  }
  // C++23 std::unreachable: tells the compiler (and the reader) that the switch above covers every
  // value, replacing a made-up fallback return that would hide a value added to the enum later;
  // reaching it is undefined behaviour, and -Wswitch already refuses a missing case at compile
  // time.
  std::unreachable();
}

// The point judged as `simsys run` judges its file (main.cpp's print_report): every SLO by
// report::evaluate_slos, and the exit code 0 when all pass (skipped counts as a pass), 2 when one
// fails, 1 when the run observed no step.
void judge(const report::Summary& summary, const graph::Graph& g,
           std::span<const std::string> headings, Measured& measured) {
  const auto results = report::evaluate_slos(summary, g.scenario().slos);
  for (std::size_t k = 0; k < results.size(); ++k) {
    measured.slos.push_back(
        SloCell{.heading = headings[k], .verdict = verdict_of(results[k].status)});
  }
  if (summary.observed_steps() == 0) {
    measured.exit_code = 1;
  } else {
    measured.exit_code = report::all_pass(results) ? 0 : 2;
  }
}

// Phase 2 for one point: a run of the whole scenario, exactly as `simsys run` does it (its
// duration, its step, its warmup: session::run_summary), and each metric read off the summary;
// with --slos, the point judged (judge). The Simulation, the Summary and the graph belong to this
// point alone: nothing here is shared with another thread. `headings` are the SLOs' column names,
// the same at every point (an override changes a value, never the SLO list), worked out once.
std::expected<Measured, std::string> measure_point(const std::string& path, const graph::Graph& g,
                                                   std::span<const SweepMetric> metrics,
                                                   SloColumns slos,
                                                   std::span<const std::string> headings) {
  // C++23 std::expected::transform: maps the value (a summary into its metrics) and passes an
  // error through untouched.
  return session::run_summary(g, path).transform([&](const report::Summary& summary) {
    auto measured = read_metrics(summary, metrics);
    if (slos == SloColumns::on) judge(summary, g, headings, measured);
    return measured;
  });
}

std::unexpected<std::string> too_many_keys() {
  return param_error("at most two keys (a grid of two); script a third");
}

// Why `params` cannot be swept as one grid, or nothing: one or two keys, no key twice, and at
// most max_points points in all (every point's graph is built before any runs). parse_params and
// run_sweep both ask, so a caller of the exported run_sweep cannot step past it.
std::optional<std::string> grid_problem(std::span<const SweepParam> params) {
  if (params.empty()) return "--param: a sweep needs a key";
  if (params.size() > max_params) return too_many_keys().error();
  if (params.size() == 1) return std::nullopt;
  if (params[0].path == params[1].path) return "--param: " + params[1].path + " is given twice";
  const auto rows = params[0].values.size();
  const auto columns = params[1].values.size();
  if (rows * columns <= max_points) return std::nullopt;
  return "--param: " + std::to_string(rows) + " x " + std::to_string(columns) + " = " +
         std::to_string(rows * columns) + " points: at most " + std::to_string(max_points);
}

// The params' path mistakes (a missing key, a list on the way) are the same at every point:
// reported once, with no point to blame. Two paths that reach one value of the file (an anchor
// and its alias) would write one key's points over the other's, so the grid is refused.
std::optional<std::string> first_path_problem(const std::string& scenario_path,
                                              std::span<const SweepParam> params) {
  for (const auto& param : params) {
    if (const auto path = config::check_override_path(scenario_path, param.path); !path) {
      return config::describe(path.error());
    }
  }
  if (params.size() < 2) return std::nullopt;
  const auto same = config::same_override_target(scenario_path, params[0].path, params[1].path);
  if (!same) return config::describe(same.error());
  if (!*same) return std::nullopt;
  return scenario_path + ": error: --param: " + params[1].path + " reaches the same value as " +
         params[0].path + " (a YAML anchor and its alias)";
}

// ---- the points in parallel --------------------------------------------------------------------

// The worker pool is session::run_in_parallel (simsys.session), shared with `run --replications`:
// each phase of a sweep (load every point, then run every point) hands it the point indices.

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
  session::run_in_parallel(count, jobs,
                           [&](std::size_t index) { results.store(index, stage(index)); });
  return results;
}

// ---- rendering ---------------------------------------------------------------------------------

// A cell for people, as the report's tables print the same number (report::format_duration,
// format_lag, format_rate and format_count: "1.61ms", "<1us", "1.75k/s", "2k"; `>1s` for a
// percentile that is the client's timeout, as the op table prints it); "-" for a number the run
// did not produce.
std::string human_cell(const SweepMetric& metric, double value, bool beyond_timeout) {
  if (std::isnan(value)) return "-";
  if (beyond_timeout) return ">" + report::format_duration(value);
  if (latency_column(metric.kind)) return report::format_duration(value);
  if (rate_column(metric.kind) || metric.kind == MetricKind::dead_lettered_rate ||
      metric.kind == MetricKind::deduplicated_rate) {
    return report::format_rate(value);
  }
  if (metric.kind == MetricKind::peak_queue_lag) return report::format_lag(value);
  if (metric.kind == MetricKind::connections) return report::format_count(value);
  return significant(value, 3);  // a utilization or a drop rate: a ratio
}

// A cell for programs: the shortest text that reads back to the same double, empty for a number
// the run did not produce (a CSV reader's missing value).
std::string csv_cell(double value) {
  return std::isnan(value) ? std::string{} : report::format_number(value);
}

// The table's cells, the header first.
using Cells = std::vector<std::vector<std::string>>;

// A verdict as each format prints it: for people in capitals, a skipped SLO a dash as a missing
// number is; for programs in the SLO table's JSON words.
std::string verdict_cell(SloVerdict verdict, SweepFormat format) {
  const bool table = format == SweepFormat::table;
  switch (verdict) {
    case SloVerdict::pass:
      return table ? "PASS" : "pass";
    case SloVerdict::fail:
      return table ? "FAIL" : "fail";
    case SloVerdict::skipped:
      return table ? "-" : "skipped";
  }
  std::unreachable();
}

// The verdict columns' headings, `exit` last; none when the rows carry no verdicts (no --slos).
// Every row of one sweep has the same SLOs, so the first row's headings are every row's.
std::vector<std::string> verdict_headings(std::span<const SweepRow> rows) {
  if (rows.empty() || !rows.front().exit_code) return {};
  std::vector<std::string> headings;
  for (const auto& cell : rows.front().slos) headings.push_back(cell.heading);
  headings.emplace_back("exit");
  return headings;
}

// A row's verdict cells, its exit code last; none without --slos.
std::vector<std::string> verdict_cells(const SweepRow& row, SweepFormat format) {
  if (!row.exit_code) return {};
  std::vector<std::string> cells;
  for (const auto& cell : row.slos) cells.push_back(verdict_cell(cell.verdict, format));
  cells.push_back(std::to_string(*row.exit_code));
  return cells;
}

std::vector<std::string> table_row(std::span<const SweepMetric> metrics, const SweepRow& row) {
  std::vector<std::string> line{row.values};
  for (std::size_t m = 0; m < metrics.size() && m < row.metrics.size(); ++m) {
    const bool beyond = std::ranges::contains(row.beyond_timeout, m);
    line.push_back(human_cell(metrics[m], row.metrics[m], beyond));
  }
  std::ranges::move(verdict_cells(row, SweepFormat::table), std::back_inserter(line));
  return line;
}

Cells table_cells(std::span<const SweepParam> params, std::span<const SweepMetric> metrics,
                  std::span<const SweepRow> rows) {
  Cells cells;
  auto& header = cells.emplace_back();
  for (const auto& param : params) header.push_back(param.path);
  for (const auto& metric : metrics) header.push_back(metric.name);
  std::ranges::move(verdict_headings(rows), std::back_inserter(header));
  for (const auto& row : rows) cells.push_back(table_row(metrics, row));
  return cells;
}

// The report's aligner (report::align_columns: the swept value left, the numbers right, two spaces
// between columns), with a rule of dashes under the header as wide as the header line.
std::string render_table(const Cells& cells) {
  const std::string aligned = report::align_columns(cells, "");
  const auto header_end = aligned.find('\n');
  return aligned.substr(0, header_end + 1) + std::string(header_end, '-') + "\n" +
         aligned.substr(header_end + 1);
}

std::string render_csv(std::span<const SweepParam> params, std::span<const SweepMetric> metrics,
                       std::span<const SweepRow> rows) {
  std::string text;
  for (const auto& param : params) text += (text.empty() ? "" : ",") + param.path;
  for (const auto& metric : metrics) text += "," + metric.name;
  for (const auto& heading : verdict_headings(rows)) text += "," + heading;
  text += "\n";
  for (const auto& row : rows) {
    for (std::size_t k = 0; k < row.values.size(); ++k) text += (k == 0 ? "" : ",") + row.values[k];
    for (const double number : row.metrics) text += "," + csv_cell(number);
    for (const auto& cell : verdict_cells(row, SweepFormat::csv)) text += "," + cell;
    text += "\n";
  }
  return text;
}

}  // namespace

std::expected<SweepParam, std::string> parse_param(std::string_view spec) {
  const auto parts = split_spec(spec);
  if (!parts) return std::unexpected(parts.error());
  if (auto problem = config::dot_path_problem(parts->path)) return param_error(*problem);
  const bool geometric = parts->spacing == "log";
  if (!parts->spacing.empty() && !geometric) {
    return param_error(std::string{parts->range} + ": the only spacing suffix is :log");
  }
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
  if (geometric && (from->number <= 0 || to->number <= 0)) {
    return param_error(std::string{parts->range} + ": geometric points need two positive ends");
  }
  auto values =
      geometric ? expand_geometric(*parts, *from, *to, *count) : expand(*parts, *from, *to, *count);
  if (!values) return std::unexpected(values.error());
  return SweepParam{.path = std::string{parts->path},
                    .values = std::move(*values),
                    .spacing = geometric ? Spacing::geometric : Spacing::linear};
}

std::expected<std::vector<SweepParam>, std::string> parse_params(
    std::span<const std::string> specs) {
  if (specs.size() > max_params) return too_many_keys();
  std::vector<SweepParam> params;
  for (const auto& spec : specs) {
    auto param = parse_param(spec);
    if (!param) return std::unexpected(param.error());
    params.push_back(std::move(*param));
  }
  if (auto problem = grid_problem(params)) return std::unexpected(*problem);
  return params;
}

std::expected<SweepMetric, std::string> parse_metric(std::string_view name) {
  if (auto metric = metric_of(name, config::dot_path_keys(name))) return *metric;
  return std::unexpected("--metric: " + std::string{name} + ": not a metric (write " +
                         std::string{metric_grammar} + ")");
}

// Pattern: Pipeline with early exit — the file, then the metrics against it, then every point's
// scenario, then every point's run; the first stage that fails ends the sweep with one message.
// Every point is loaded before any is simulated, so a bad value at point 9 costs no simulation of
// points 1 to 8.
std::expected<std::vector<SweepRow>, std::string> run_sweep(const std::string& scenario_path,
                                                            std::span<const SweepParam> params,
                                                            std::span<const SweepMetric> metrics,
                                                            std::size_t jobs, SloColumns slos) {
  const auto base = config::load(scenario_path);
  if (!base) return std::unexpected(config::describe(base.error()));
  // A metric the file cannot answer is the file's mistake as much as the option's: worded as the
  // loader words one (`<file>: error: `), as the CLI words a --param or --metric it refuses.
  if (auto problem = first_metric_problem(*base, metrics)) {
    return std::unexpected(scenario_path + ": error: " + *problem);
  }
  if (auto problem = grid_problem(params)) {
    return std::unexpected(scenario_path + ": error: " + *problem);
  }
  if (auto problem = first_path_problem(scenario_path, params)) return std::unexpected(*problem);

  auto points = grid_points(params);
  const auto headings = slo_headings(base->slos);
  const auto count = points.size();
  const auto graphs = for_each_point<graph::Graph>(count, jobs, [&](std::size_t i) {
    return prepare_point(scenario_path, params, points[i], i);
  });
  if (auto error = graphs.first_error()) return std::unexpected(*error);

  const auto measured = for_each_point<Measured>(count, jobs, [&](std::size_t i) {
    return measure_point(scenario_path, *graphs.values[i], metrics, slos, headings)
        .transform_error(
            [&](const std::string& e) { return point_label(params, points[i], i) + e; });
  });
  if (auto error = measured.first_error()) return std::unexpected(*error);

  std::vector<SweepRow> rows;
  for (std::size_t i = 0; i < count; ++i) {
    const auto& point = *measured.values[i];
    rows.push_back(SweepRow{.values = std::move(points[i]),
                            .metrics = point.numbers,
                            .beyond_timeout = point.beyond_timeout,
                            .slos = point.slos,
                            .exit_code = point.exit_code});
  }
  return rows;
}

std::expected<std::vector<SweepRow>, std::string> run_sweep(const std::string& scenario_path,
                                                            const SweepParam& param,
                                                            std::span<const SweepMetric> metrics,
                                                            std::size_t jobs, SloColumns slos) {
  return run_sweep(scenario_path, std::span{&param, 1}, metrics, jobs, slos);
}

std::size_t default_jobs() {
  // 0 means "unknown" (the standard allows it): then one worker.
  return std::max(1U, std::thread::hardware_concurrency());
}

std::string render_sweep(std::span<const SweepParam> params, std::span<const SweepMetric> metrics,
                         std::span<const SweepRow> rows, SweepFormat format) {
  if (format == SweepFormat::csv) return render_csv(params, metrics, rows);
  return render_table(table_cells(params, metrics, rows));
}

std::string render_sweep(const SweepParam& param, std::span<const SweepMetric> metrics,
                         std::span<const SweepRow> rows, SweepFormat format) {
  return render_sweep(std::span{&param, 1}, metrics, rows, format);
}

}  // namespace simsys::sweep
