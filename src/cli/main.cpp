// `simsys` command-line entry point. Parsing is delegated to CLI11; commands are thin
// wrappers over library calls so they stay testable without a process.
//
// Exit codes: 0 the run worked and every SLO passed, 2 it worked but an SLO failed, 1 an
// error (bad file, bad option, a scenario the engine cannot run yet, or a run that observed
// no step: no data is an error, not a pass). `sweep` reports numbers and judges none: 0 when
// every point ran, whatever its SLOs, 1 on an error.
#include <unistd.h>  // isatty, STDOUT_FILENO: POSIX, which is fine for this project
#include <CLI/CLI.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <generator>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// C++20 `import`: brings the exported names of a named module into this translation
// unit. Unlike #include, it does not re-parse text; the compiler reads a compiled
// module interface (BMI). Imports go after the #includes because GCC 14 reports
// redefinition errors when a std header is #included after importing a module that
// included the same header in its global module fragment.
import simsys.core;
import simsys.behaviours; // client_seed: the noise seed the engine gives each client
import simsys.config;
import simsys.engine;
import simsys.graph;
import simsys.model;
import simsys.report;
import simsys.session;
import simsys.sweep;
import simsys.workload;

// Every engine call below lives in a named helper (most in simsys.session), never in main()'s
// body: GCC 14.2 at -O2 crashes ("in ipa_comdats") when Graph::build, Simulation::create and
// step are inlined into main (see the -fno-ipa-sra comment in CMakeLists.txt and the Lessons of
// the phase 1 plan).
//
// Numbers are formatted by simsys.report only. GCC 14 throws at run time on precision
// specs such as {:.3f} in a translation unit that imports simsys.core, so this file
// formats strings and integers, never doubles.

namespace {

namespace graph = simsys::graph;
namespace report = simsys::report;

// Compiler-style `file:line:column: error: message`, so editors can jump to it (simsys.config
// writes it, one rule for every command and for `sweep`).
using simsys::config::describe;

void print_error(const std::string& message) {
  // C++23 std::println: type-safe formatted output (here to stderr), replacing
  // printf's format-string/argument mismatch bugs and iostream's verbosity. The
  // format string is checked at compile time.
  std::println(stderr, "{}", message);
}

// Scenario file -> resolved graph, or the message to print (simsys.session: `sweep` loads each
// of its points the same way).
// C++23 std::expected<T, E>: the graph or the finished message, so every command prints an error
// the same way.
std::expected<graph::Graph, std::string> load_graph(const std::filesystem::path& file) {
  return simsys::session::load_graph(file.string());
}

// How `estimate` and `run` print their result.
struct ReportOptions {
  std::string format{"table"};  // "table" or "json"
  bool no_color{};
};

// Colour only for a person: on when stdout is a terminal, off when it is piped or
// redirected, off when asked, and never inside JSON.
bool use_color(const ReportOptions& options) {
  return options.format == "table" && !options.no_color && isatty(STDOUT_FILENO) != 0;
}

// Prints the summary with its SLO verdicts and turns them into the exit code. A run that
// observed no step evaluated nothing: all_pass of all-skipped SLOs is true, but no data is
// an error, not a pass, so it exits 1. (An exit code needs a process to test; the library
// half of this contract is pinned in tests/unit/test_report.cpp, "an empty observation
// window evaluates nothing".)
int print_report(const report::Summary& summary, const ReportOptions& options) {
  const auto& slos = summary.graph().scenario().slos;
  const auto results = report::evaluate_slos(summary, slos);
  if (options.format == "json") {
    std::println("{}", report::render_json(summary, results));
  } else {
    std::print("{}", report::render_table(summary, results, use_color(options)));
  }
  if (summary.observed_steps() == 0) return 1;
  return report::all_pass(results) ? 0 : 2;
}

// `simsys validate <file>`: structural check of one scenario.
int run_validate(const std::filesystem::path& file) {
  const auto scenario = simsys::config::load(file.string());
  if (!scenario) {
    print_error(describe(scenario.error()));
    return 1;
  }
  std::println("OK: {} ({} nodes, {} links)", scenario->name, scenario->nodes.size(),
               scenario->links.size());
  return 0;
}

// `simsys explain <file>`: every resolved number and where it came from.
int run_explain(const std::filesystem::path& file) {
  const auto g = load_graph(file);
  if (!g) {
    print_error(g.error());
    return 1;
  }
  std::print("{}", report::render_explain(*g));
  return 0;
}

// What `estimate` evaluates, stated above its table: the steady state of the curve's value
// at t = 0 (00:00), noise left out (Simulation::estimate). A daily wave's trough or peak
// is not that value, so the line also says where the peak IS checked: explain's static
// load check. Table only: a line of prose before the JSON would break its parsers.
constexpr std::string_view estimate_header =
    "estimate at t = 0 (00:00): the curve without noise; explain's checks use the peak";

// estimate admits min(in, limit) at a rate limiter: the long-run rate of a bucket, but not
// of the sliding counter (about 10/11 of the limit under a flood), and blind to the fixed
// window's sawtooth inside each window. When a windowed limiter is present, the header says so.
constexpr std::string_view windowed_limiter_caveat =
    "; windowed limiters are evaluated at min(in, limit) — run them to see the sawtooth";

// C++20 std::ranges::any_of: takes the range itself instead of a begin/end iterator pair
// (std::any_of), so the span cannot be paired with another container's end by mistake.
[[nodiscard]] bool has_windowed_limiter(const graph::Graph& g) {
  return std::ranges::any_of(g.nodes(), [](const graph::ResolvedNode& node) {
    const auto& spec = node.spec;
    return spec.type == simsys::model::NodeType::rate_limiter &&
           simsys::model::is_windowed(
               spec.algorithm.value_or(simsys::model::LimiterAlgorithm::token_bucket));
  });
}

// `simsys estimate <file>`: one steady-state evaluation, summarised as a run of one step.
int run_estimate(const std::filesystem::path& file, const ReportOptions& options) {
  const auto g = load_graph(file);
  if (!g) {
    print_error(g.error());
    return 1;
  }
  const auto sim = simsys::session::create_simulation(*g, file.string());
  if (!sim) {
    print_error(sim.error());
    return 1;
  }
  const auto record = sim->estimate();
  if (!record.converged) {
    print_error(file.string() +
                ": warning: no steady state (a loop of gain >= 1, or delivered fractions "
                "that did not settle: a step after a failed step); "
                "the numbers below are the last settle pass");
  }
  if (options.format == "table") {
    std::println("{}{}", estimate_header,
                 has_windowed_limiter(*g) ? windowed_limiter_caveat : std::string_view{});
  }
  return print_report(report::Summary::of_estimate(*g, record), options);
}

// What `run` takes beyond the report options.
struct RunArgs {
  std::optional<std::string> duration{};    // overrides the scenario's
  std::optional<std::string> series{};      // CSV output path, one row per node per step
  std::optional<std::string> series_ops{};  // CSV output path, one row per client op per step
  std::optional<int> seed{};                // overrides the scenario's (see load_for_run)
};

// `--duration`'s text, checked as a duration before anything is loaded, so its message echoes
// what the user typed.
std::optional<std::string> unparsable_duration(const std::filesystem::path& file,
                                               const std::string& text) {
  const auto parsed = simsys::parse_duration(text);
  if (parsed) return std::nullopt;
  // C++20 std::format: type-safe formatting into a std::string, with `{}` placeholders checked
  // against the arguments at compile time. Strings and integers only here (see the top).
  return std::format("{}: error: --duration '{}': {}", file.string(), parsed.error().input,
                     parsed.error().message);
}

// Why the file loads but not with `--duration`, worded after the option: a duration not longer
// than the warmup (nothing would be observed), or the loader's rule as it judged the new duration
// (an SLO's `over` that no longer fits after the warmup: review of plan 2c Task 19, where
// `--duration 10m` on youtube kept its `over: 1h`), at the line it judged. The file's own mistake
// comes first, as the loader words it.
std::string duration_refusal(const std::filesystem::path& file, const std::string& text,
                             const simsys::config::ConfigError& refused) {
  const auto file_alone = simsys::config::load(file.string());
  if (!file_alone) return describe(file_alone.error());
  const auto duration = simsys::parse_duration(text);  // parsed already: unparsable_duration
  if (duration && duration->count() <= file_alone->warmup.count()) {
    return std::format("{}: error: --duration {} is not longer than the warmup {}", file.string(),
                       text, simsys::describe_duration(file_alone->warmup));
  }
  auto blamed = refused;
  blamed.message = std::format("--duration {}: {}", text, refused.rule);
  return describe(blamed);
}

// The graph `run` simulates. `--duration` is applied as a config::Override, so the loader holds it
// to every rule it holds the file's own duration to (comprehensive 2c review, I-4: the CLI kept a
// copy of the `over` rule); `--seed` replaces the scenario's before the graph is built: the engine
// seeds every client's noise from Graph::scenario().seed (behaviours::client_seed), so replacing
// it any later would not reach the clients.
std::expected<graph::Graph, std::string> load_for_run(const std::filesystem::path& file,
                                                      const RunArgs& args) {
  std::vector<simsys::config::Override> overrides;
  if (args.duration) {
    if (auto refused = unparsable_duration(file, *args.duration)) {
      // C++23 std::unexpected<E>: wraps the error so it converts to any std::expected<T, E>.
      return std::unexpected(std::move(*refused));
    }
    // C++20 designated initializers: the members named where the value is built, in declaration
    // order, replacing positional braces where a reader must remember which string is which.
    overrides.push_back({.path = "duration", .value = *args.duration});
  }
  auto scenario = simsys::config::load(file.string(), overrides);
  if (!scenario) {
    return std::unexpected(args.duration ? duration_refusal(file, *args.duration, scenario.error())
                                         : describe(scenario.error()));
  }
  if (args.seed) scenario->seed = *args.seed;
  return simsys::session::build_graph(file.string(), std::move(*scenario));
}

// Opens `path` for a series file (`--series`, `--series-ops`), or says why it cannot.
bool open_series(const std::string& path, std::ofstream& file) {
  file.open(path);
  if (!file) print_error(path + ": error: cannot open for writing");
  return static_cast<bool>(file);
}

// Can `path` be opened for writing? Probed in append mode, which never truncates, and a file the
// probe created is removed again: a run refused for the OTHER series file leaves this one as it
// found it (review of 3e23477, M1: `--series` was truncated before `--series-ops` failed).
bool writable(const std::string& path) {
  std::error_code ignored;  // the non-throwing overloads: a probe answers, it does not throw
  const bool existed = std::filesystem::exists(path, ignored);
  const bool opened = std::ofstream{path, std::ios::app}.is_open();
  if (opened && !existed) std::filesystem::remove(path, ignored);
  return opened;
}

// `path` as the file system resolves it: absolute, `.` and `..` and the symbolic links of the
// parts that exist resolved (std::filesystem::weakly_canonical), so `a.csv`, `./a.csv` and a link
// to it compare equal; its plain normal form if even that fails.
std::filesystem::path resolved(const std::string& path) {
  std::error_code error;
  auto canonical = std::filesystem::weakly_canonical(path, error);
  return error ? std::filesystem::path{path}.lexically_normal() : canonical;
}

// Why the series files cannot be written, before either is opened (opening truncates), or
// nothing: two names for one file (both writers would truncate it and interleave their rows,
// M2), or a path that cannot be opened for writing.
std::optional<std::string> series_paths_refused(const std::optional<std::string>& series,
                                                const std::optional<std::string>& series_ops) {
  if (series && series_ops && resolved(*series) == resolved(*series_ops)) {
    return *series_ops + ": error: --series and --series-ops name the same file";
  }
  for (const auto& path : {series, series_ops}) {
    if (path && !writable(*path)) return *path + ": error: cannot open for writing";
  }
  return std::nullopt;
}

// `simsys run <file>`: steps through the scenario, feeding every record to the summary
// and, when asked, to the CSV series (the contract a GUI plots from).
// `--seed` reaches the engine through the scenario (load_for_run); a process-level test of
// that would need to spawn simsys, so it is checked by hand (two seeds, two outputs) and
// the engine half is pinned in tests/theory ("two runs with the same seed are identical
// and a different seed is not").
int run_scenario(const std::filesystem::path& file, const RunArgs& args,
                 const ReportOptions& options) {
  const auto g = load_for_run(file, args);
  if (!g) {
    print_error(g.error());
    return 1;
  }
  auto sim = simsys::session::create_simulation(*g, file.string());
  if (!sim) {
    print_error(sim.error());
    return 1;
  }

  // Both paths are checked, then both files open, before the run: a bad path stops it before
  // any step and before either file is truncated.
  if (const auto refused = series_paths_refused(args.series, args.series_ops)) {
    print_error(*refused);
    return 1;
  }
  std::ofstream series_file;
  std::ofstream series_ops_file;
  if (args.series && !open_series(*args.series, series_file)) return 1;
  if (args.series_ops && !open_series(*args.series_ops, series_ops_file)) return 1;
  std::optional<report::SeriesWriter> series;
  std::optional<report::OpSeriesWriter> series_ops;
  if (args.series) series.emplace(series_file, *g);
  if (args.series_ops) series_ops.emplace(series_ops_file, *g);

  const auto summary = simsys::session::run_summary(
      *g, *sim,
      {.series = series ? &*series : nullptr, .series_ops = series_ops ? &*series_ops : nullptr});
  return print_report(summary, options);
}

// What `curve` takes beyond the file.
struct CurveArgs {
  std::string client{};
  std::string step{"1m"};
  std::optional<std::string> until{};  // default: the scenario's duration
  std::optional<int> seed{};           // default: the scenario's seed
  bool noise{};
};

// A client node and its index among the scenario's nodes: the index the graph and the
// engine use (Graph keeps the scenario's node order), from which its noise seed derives.
struct ClientRef {
  const simsys::model::NodeSpec* spec;
  std::size_t index;
};

// "a service", "an id_generator".
std::string_view article_for(std::string_view noun) {
  // C++23 std::string_view::contains: a yes/no membership test, replacing the
  // `find(c) != npos` comparison that reads as index arithmetic.
  return std::string_view{"aeiou"}.contains(noun.front()) ? "an" : "a";
}

// The client `id` names, or the message to print.
std::expected<ClientRef, std::string> find_client(const std::filesystem::path& file,
                                                  const simsys::model::Scenario& scenario,
                                                  std::string_view id) {
  for (std::size_t i = 0; i < scenario.nodes.size(); ++i) {
    const auto& node = scenario.nodes[i];
    if (node.id != id) continue;
    if (node.type == simsys::model::NodeType::client && node.workload) return ClientRef{&node, i};
    const auto type = simsys::model::to_string(node.type);
    return std::unexpected(std::format("{}: error: '{}' is {} {}, not a client", file.string(), id,
                                       article_for(type), type));
  }
  return std::unexpected(std::format("{}: error: no client '{}'", file.string(), id));
}

// A duration option of `curve` (`--step`, `--until`), echoed in its message as typed.
std::expected<simsys::SimTime, std::string> curve_duration(const std::filesystem::path& file,
                                                           std::string_view option,
                                                           const std::string& text) {
  const auto parsed = simsys::parse_duration(text);
  if (!parsed) {
    return std::unexpected(std::format("{}: error: {} '{}': {}", file.string(), option,
                                       parsed.error().input, parsed.error().message));
  }
  return *parsed;
}

// The sampling grid of `curve`: a positive step, and an end (exclusive) that defaults to
// the scenario's duration.
struct CurveGrid {
  simsys::SimTime step;
  simsys::SimTime until;
};

// With --noise, the noise advances at the scenario's step, as in a run, and every k-th
// value is printed; so --step must be a whole number of scenario steps.
std::expected<simsys::SimTime, std::string> check_noise_step(
    const std::filesystem::path& file, const simsys::model::Scenario& scenario,
    const CurveArgs& args, simsys::SimTime step) {
  if (!args.noise || step.count() % scenario.step.count() == 0) return step;
  return std::unexpected(std::format("{}: error: --step must be a multiple of the scenario step {}",
                                     file.string(), simsys::describe_duration(scenario.step)));
}

std::expected<CurveGrid, std::string> curve_grid(const std::filesystem::path& file,
                                                 const simsys::model::Scenario& scenario,
                                                 const CurveArgs& args) {
  auto step = curve_duration(file, "--step", args.step);
  if (!step) return std::unexpected(step.error());
  if (step->count() <= 0) {
    return std::unexpected(std::format("{}: error: --step must be positive", file.string()));
  }
  step = check_noise_step(file, scenario, args, *step);
  if (!step) return std::unexpected(step.error());
  if (!args.until) return CurveGrid{.step = *step, .until = scenario.duration};
  // C++23 std::expected::transform: maps the value (here a duration to a CurveGrid) and
  // passes an error through untouched, replacing `if (!r) return unexpected(r.error());`
  // followed by building the result by hand; unlike and_then, the lambda cannot fail.
  return curve_duration(file, "--until", *args.until).transform([&](simsys::SimTime until) {
    return CurveGrid{.step = *step, .until = until};
  });
}

// The client's curve as a lazy stream of samples: the deterministic curve, or with
// `--noise` the curve times the client's noise process, so the printed noise is the noise
// a run will see: seeded exactly as the engine seeds it (behaviours::client_seed), advanced
// every scenario step (the engine's dt) whatever --step prints, and ticked before each
// value is read, as the engine ticks a client before it offers. The t = 0 row therefore
// already includes one noise step, like the first step of a run.
// C++23 std::generator<T> (from <generator>): a coroutine-backed, lazy, single-pass range.
// simsys.workload produces samples one co_yield at a time; this file only iterates the
// range, so a day at 1 s steps (86 400 rows) is printed without ever being stored. The
// pre-C++23 way was a std::vector filled up front or a callback parameter.
std::expected<std::generator<simsys::workload::Sample>, std::string> curve_samples(
    const std::filesystem::path& file, const simsys::model::Scenario& scenario,
    const ClientRef& client, const CurveArgs& args, const CurveGrid& grid) {
  const auto& workload = *client.spec->workload;
  if (!args.noise) return simsys::workload::samples(workload.curve, grid.step, grid.until);
  if (!workload.noise) {
    return std::unexpected(
        std::format("{}: error: client '{}' has no noise block", file.string(), client.spec->id));
  }
  const auto seed =
      simsys::behaviours::client_seed(args.seed.value_or(scenario.seed), client.index);
  return simsys::workload::samples(workload.curve, grid.step, grid.until, *workload.noise, seed,
                                   scenario.step);
}

// One CSV row per sample, numbers in their shortest round-trip form (simsys.report: this
// file never formats doubles itself, see the note at the top).
void print_samples(std::generator<simsys::workload::Sample>& samples) {
  std::println("t_s,rate_per_s");
  for (const auto& sample : samples) {
    std::println("{},{}", report::format_number(simsys::seconds(sample.t)),
                 report::format_number(sample.rate.per_second));
  }
}

// `simsys curve <file> <client>`: the client's offered rate over time as CSV, for a plot.
int run_curve(const std::filesystem::path& file, const CurveArgs& args) {
  const auto scenario = simsys::config::load(file.string());
  if (!scenario) {
    print_error(describe(scenario.error()));
    return 1;
  }
  const auto client = find_client(file, *scenario, args.client);
  if (!client) {
    print_error(client.error());
    return 1;
  }
  const auto grid = curve_grid(file, *scenario, args);
  if (!grid) {
    print_error(grid.error());
    return 1;
  }
  auto samples = curve_samples(file, *scenario, *client, args, *grid);
  if (!samples) {
    print_error(samples.error());
    return 1;
  }
  print_samples(*samples);
  return 0;
}

// What `sweep` takes beyond the file.
struct SweepArgs {
  std::string param{};
  std::vector<std::string> metrics{};
  std::size_t jobs{simsys::sweep::default_jobs()};
  std::string format{"table"};
};

// The --metric texts, parsed; the first bad one is the error.
std::expected<std::vector<simsys::sweep::SweepMetric>, std::string> parse_metrics(
    const std::vector<std::string>& texts) {
  std::vector<simsys::sweep::SweepMetric> metrics;
  for (const auto& text : texts) {
    auto metric = simsys::sweep::parse_metric(text);
    if (!metric) return std::unexpected(metric.error());
    metrics.push_back(std::move(*metric));
  }
  return metrics;
}

// `simsys sweep <file> --param <spec> --metric <m>...`: one run per point, one row per point.
// Nothing reaches stdout unless every point ran, so a script never reads half a table.
int run_sweep_command(const std::filesystem::path& file, const SweepArgs& args) {
  const auto param = simsys::sweep::parse_param(args.param);
  if (!param) {
    print_error(param.error());
    return 1;
  }
  const auto metrics = parse_metrics(args.metrics);
  if (!metrics) {
    print_error(metrics.error());
    return 1;
  }
  const auto rows = simsys::sweep::run_sweep(file.string(), *param, *metrics, args.jobs);
  if (!rows) {
    print_error(rows.error());
    return 1;
  }
  const auto format =
      args.format == "csv" ? simsys::sweep::SweepFormat::csv : simsys::sweep::SweepFormat::table;
  std::print("{}", simsys::sweep::render_sweep(*param, *metrics, *rows, format));
  return 0;
}

// The options `estimate` and `run` share.
void add_report_options(CLI::App& command, ReportOptions& options) {
  command.add_option("--report", options.format, "output format: table (default) or json")
      ->check(CLI::IsMember({"table", "json"}));
  command.add_flag("--no-color", options.no_color,
                   "never colour the table (default: colour when stdout is a terminal)");
}

// Every command's first argument: the scenario file, which must exist.
void add_file_argument(CLI::App& command, std::filesystem::path& file) {
  command.add_option("file", file, "scenario YAML file")->required()->check(CLI::ExistingFile);
}

// `run`'s own options, after the report options it shares with `estimate`.
void add_run_options(CLI::App& run, RunArgs& args) {
  run.add_option("--duration", args.duration, "simulated time, e.g. 30s (default: scenario's)");
  run.add_option("--seed", args.seed, "seed of the clients' rate noise (default: scenario's)");
  run.add_option("--series", args.series, "write one CSV row per node per step to this file");
  run.add_option("--series-ops", args.series_ops,
                 "write one CSV row per client op per step to this file");
}

CLI::App* add_curve_command(CLI::App& app, CurveArgs& args, std::filesystem::path& file) {
  auto* curve = app.add_subcommand("curve", "print a client's rate curve as CSV (t_s,rate_per_s)");
  add_file_argument(*curve, file);
  curve->add_option("client", args.client, "id of a client node")->required();
  curve->add_option("--step", args.step, "time between samples (default 1m)");
  curve->add_option("--until", args.until,
                    "end of the curve, exclusive (default: scenario's duration)");
  curve->add_option("--seed", args.seed, "noise seed (default: scenario's seed)");
  curve->add_flag("--noise", args.noise,
                  "multiply in the client's seeded noise, advanced every scenario step as in a run "
                  "(--step must then be a multiple of it)");
  return curve;
}

CLI::App* add_sweep_command(CLI::App& app, SweepArgs& args, std::filesystem::path& file) {
  auto* sweep = app.add_subcommand(
      "sweep", "run the scenario once per value of one key, the runs in parallel; one row each");
  add_file_argument(*sweep, file);
  sweep
      ->add_option("--param", args.param,
                   "the swept key and its range, <dot.path>=<from>..<to>:<n>, "
                   "e.g. nodes.users.rate.base=100/s..900/s:3")
      ->required();
  sweep
      ->add_option("--metric", args.metrics,
                   "a column: <client>.<op>.mean|p50|p95|p99, <node>.mean_rho|peak_rho|drop_rate "
                   "or <queue>.lag (repeatable)")
      ->required();
  sweep->add_option("--jobs", args.jobs, "worker threads (default: the machine's hardware threads)")
      ->check(CLI::Range(std::size_t{1}, simsys::sweep::max_jobs));
  sweep->add_option("--format", args.format, "output format: table (default) or csv")
      ->check(CLI::IsMember({"table", "csv"}));
  return sweep;
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"simsys - a fluid-flow system design simulator"};
  app.set_version_flag("--version", std::string{simsys::version});
  app.require_subcommand(1);

  std::filesystem::path scenario_file;
  ReportOptions report_options;
  RunArgs run_args;
  CurveArgs curve_args;
  SweepArgs sweep_args;

  auto* validate = app.add_subcommand("validate", "check a scenario's types, references and units");
  auto* explain = app.add_subcommand(
      "explain", "print every resolved number, where it came from, and a static load check");
  auto* estimate = app.add_subcommand("estimate", "evaluate the steady state once and check SLOs");
  auto* run = app.add_subcommand("run", "step through the scenario and check SLOs");
  for (auto* command : {validate, explain, estimate, run})
    add_file_argument(*command, scenario_file);
  auto* curve = add_curve_command(app, curve_args, scenario_file);
  auto* sweep = add_sweep_command(app, sweep_args, scenario_file);
  add_report_options(*estimate, report_options);
  add_report_options(*run, report_options);
  add_run_options(*run, run_args);

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    // app.exit prints help / version / the error exactly as CLI11 intends; its own error
    // codes (105, 106, ...) are folded into our 1 so scripts see one documented contract.
    return app.exit(e) == 0 ? 0 : 1;
  }

  if (validate->parsed()) return run_validate(scenario_file);
  if (explain->parsed()) return run_explain(scenario_file);
  if (estimate->parsed()) return run_estimate(scenario_file, report_options);
  if (run->parsed()) return run_scenario(scenario_file, run_args, report_options);
  if (curve->parsed()) return run_curve(scenario_file, curve_args);
  if (sweep->parsed()) return run_sweep_command(scenario_file, sweep_args);
  return 0;
}
