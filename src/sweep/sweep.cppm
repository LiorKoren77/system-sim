// Module `simsys.sweep`, interface unit: `simsys sweep` runs one scenario over a range of one key,
// one simulation per point, the points in parallel, and prints one row per point with the metrics
// asked for.
//
// Models: nothing new from the book. A sweep is the capacity-planning question of System Design
// Interview ch. 1 and 2 ("how does the system behave as the load grows?") asked of the simulator
// many times: each point is an ordinary run of the scenario with one value replaced, measured by
// the report's own Summary. It deliberately does no search (no "find the rate where p99 crosses
// 100 ms"), no interpolation between points and no statistics across points: n points are n
// independent runs, and the table shows them side by side.
//
// Lean by design: <thread> and <atomic> live in sweep.cpp, not here. The threads are how the
// points run, not part of what a sweep is, so no importer should parse those headers. (The GCC
// 14.2 'Bad file data' defect of CLAUDE.md §3 would not bite here: it needs <chrono>, <format>,
// <filesystem> or <print> in an interface that imports another module, and this one imports
// none.)
//
// C++20 named module, interface unit (see src/core/core.cppm for the longer explanation of
// modules versus headers): `module;` opens the global module fragment for the #includes, and
// `export module simsys.sweep;` starts the module's purview, where only `export`ed names are
// visible to importers.
module;
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module simsys.sweep;

export namespace simsys::sweep {

// The swept key and the text of each point, in the scenario's own unit syntax (`100/s`, `5ms`,
// `400`), so each value can replace the file's text as it stands (config::Override).
struct SweepParam {
  std::string path;
  std::vector<std::string> values;
};

// `<dot.path>=<from>..<to>:<n>`: n >= 2 evenly spaced points from `from` to `to`, both ends
// included. Both ends are a number followed by the same unit text (`100/s..900/s`, `1ms..5ms`,
// `400..600`), which every point repeats; a key that counts whole things (`workers`, `replicas`,
// ...) must get a whole number at every point. Errors start with "--param: ".
// C++23 std::expected<T, E>: the parsed value or the message, replacing an exception or an
// out-parameter plus bool; the caller must look before it uses the value.
[[nodiscard]] std::expected<SweepParam, std::string> parse_param(std::string_view spec);

// What a metric reads off a point's run summary: the numbers `simsys run --report json` prints.
enum class MetricKind {
  mean_latency,    // `<client>.<op>.mean`: the op's mean latency as the client sees it, seconds
  p50_latency,     // `<client>.<op>.p50`: its estimated median, seconds, as the op table's p50
  p95_latency,     // `<client>.<op>.p95`: ... its p95
  p99_latency,     // `<client>.<op>.p99`: ... its p99
  mean_rho,        // `<node>.mean_rho`: the node's time-averaged utilization, a ratio
  peak_rho,        // `<node>.peak_rho`: its highest utilization of any observed step, a ratio
  drop_rate,       // `<node>.drop_rate`: the share of the work sent to it that it dropped, a ratio
  peak_queue_lag,  // `<queue>.lag`: a message queue's peak lag after warmup, seconds
  attempts_rate,   // `<client>.<op>.attempts`: what the client sent, retries included, per second
  answered_rate,   // `<client>.<op>.answered`: the part of its requests answered, per second
  dead_lettered_rate  // `<pool>.dead_lettered`: a worker pool's dead letters, per second
};

// One `--metric`. `name` is the text as typed: it heads the metric's column. `op` is empty except
// for a client op's metric (a latency or a rate).
struct SweepMetric {
  std::string name;
  MetricKind kind{};
  std::string node{};
  std::string op{};
};

// The grammar of a metric only: whether `node` and `op` exist is known once a scenario is loaded
// (run_sweep checks). Errors start with "--metric: " and name the metric.
[[nodiscard]] std::expected<SweepMetric, std::string> parse_metric(std::string_view name);

// One point: its value as written in the param, and one number per metric, in the order asked,
// in SI base units (seconds for a latency or a lag, a plain ratio for a utilization or a drop
// rate, per second for a rate), like report::SloResult::observed. `beyond_timeout` lists (by index
// into `metrics`) the percentiles that are the client's timeout, not a latency it saw: more than
// 1 − q of the delivered requests timed out, and the table prints them `>1s`, as the op table does.
struct SweepRow {
  std::string value;
  std::vector<double> metrics;
  std::vector<std::size_t> beyond_timeout{};
};

enum class SweepFormat { table, csv };

// C++20 std::span<const T>: a non-owning view of contiguous elements, so the caller passes a
// vector or an array of metrics without a copy and without `run_sweep` being a template.
// Loads `scenario_path` once per point with the param's value in place (config::Override), checks
// every point and every metric before any simulation runs, then runs the points' simulations
// with `jobs` worker threads (at least 1, at most one per point). Each point is its own
// Simulation, deterministic, so the rows do not depend on `jobs`. An error names the file, or
// the lowest-numbered failing point (from 1) and its override: "point 2 (key=value): ...".
[[nodiscard]] std::expected<std::vector<SweepRow>, std::string> run_sweep(
    const std::string& scenario_path, const SweepParam& param, std::span<const SweepMetric> metrics,
    std::size_t jobs);

// The CLI's default --jobs: the machine's hardware threads, or 1 when it cannot tell.
[[nodiscard]] std::size_t default_jobs();

// The largest --jobs the CLI accepts. Chosen, not from the book: far above any machine's hardware
// threads, and run_sweep never starts more workers than there are points anyway.
inline constexpr std::size_t max_jobs = 1024;

// `table`: for people, a header naming the swept key and the metrics, then one line per point,
// latencies in readable units (1.61 ms). `csv`: for programs, the same header with commas and
// each number as the shortest text that reads back to the same double (report::format_number).
// Both end with a newline.
[[nodiscard]] std::string render_sweep(const SweepParam& param,
                                       std::span<const SweepMetric> metrics,
                                       std::span<const SweepRow> rows, SweepFormat format);

}  // namespace simsys::sweep
