// `simsys` command-line entry point. Parsing is delegated to CLI11; commands are thin
// wrappers over library calls so they stay testable without a process.
//
// Exit codes: 0 the run worked and every SLO passed, 2 it worked but an SLO failed, 1 an
// error (bad file, bad option, a scenario the engine cannot run yet, or a run that observed
// no step: no data is an error, not a pass).
#include <unistd.h>  // isatty, STDOUT_FILENO: POSIX, which is fine for this project
#include <CLI/CLI.hpp>

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
import simsys.workload;

// Every engine call below lives in a named helper, never in main()'s body: GCC 14.2 at
// -O2 crashes ("in ipa_comdats") when Graph::build, Simulation::create and step are
// inlined into main (see the -fno-ipa-sra comment in CMakeLists.txt and the Lessons of
// the phase 1 plan).
//
// Numbers are formatted by simsys.report only. GCC 14 throws at run time on precision
// specs such as {:.3f} in a translation unit that imports simsys.core, so this file
// formats strings and integers, never doubles.

namespace {

namespace engine = simsys::engine;
namespace graph = simsys::graph;
namespace report = simsys::report;

// Compiler-style `file:line:column: error: message`, so editors can jump to it. Line 0
// means the location is unknown (e.g. an empty file), so there is no position to print.
// C++20 std::format: type-safe formatting into a std::string, with `{}` placeholders
// checked against the arguments at compile time. Replaces snprintf (buffer sizes, format
// and argument mismatches) and std::ostringstream chains. Strings and integers only here
// (see the note above on doubles).
std::string describe(const simsys::config::ConfigError& error) {
  if (error.line == 0) return std::format("{}: error: {}", error.file, error.message);
  return std::format("{}:{}:{}: error: {}", error.file, error.line, error.column, error.message);
}

void print_error(const std::string& message) {
  // C++23 std::println: type-safe formatted output (here to stderr), replacing
  // printf's format-string/argument mismatch bugs and iostream's verbosity. The
  // format string is checked at compile time.
  std::println(stderr, "{}", message);
}

// Scenario file -> resolved graph, or the message to print.
// C++23 std::expected<T, E>: a value-or-error return type; the error here is the finished
// message, so every command prints it the same way. C++23 monadic operations chain the
// two fallible stages without an `if` per stage: transform_error rewrites only an error
// (ConfigError or EngineError -> text), and_then runs the next stage only on a value and
// passes an error straight through. The pre-C++23 way is an early return after each step.
std::expected<graph::Graph, std::string> load_graph(const std::filesystem::path& file) {
  return simsys::config::load(file.string())
      .transform_error(describe)
      .and_then([&](simsys::model::Scenario scenario) {
        return graph::Graph::build(std::move(scenario))
            .transform_error([&](const graph::EngineError& error) {
              return std::format("{}: error: {}", file.string(), error.message);
            });
      });
}

// A graph the engine can run, or `file: error: why not` (e.g. a node type that phase 1
// validates and explains but does not simulate yet).
std::expected<engine::Simulation, std::string> create_simulation(
    const graph::Graph& g, const std::filesystem::path& file) {
  return engine::Simulation::create(g).transform_error([&](const graph::EngineError& error) {
    return std::format("{}: error: {}", file.string(), error.message);
  });
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

// `simsys estimate <file>`: one steady-state evaluation, summarised as a run of one step.
int run_estimate(const std::filesystem::path& file, const ReportOptions& options) {
  const auto g = load_graph(file);
  if (!g) {
    print_error(g.error());
    return 1;
  }
  const auto sim = create_simulation(*g, file);
  if (!sim) {
    print_error(sim.error());
    return 1;
  }
  const auto record = sim->estimate();
  if (!record.converged) {
    print_error(file.string() +
                ": warning: no steady state (a loop of gain >= 1); "
                "the numbers below are the last settle pass");
  }
  report::Summary summary{*g, simsys::SimTime{0}};
  summary.observe(record);
  return print_report(summary, options);
}

// A duration in the largest unit that divides it exactly, so it reads as a scenario writes
// it: 10s, 2m, 1500ms. Integer arithmetic on the tick count (nanoseconds) only: this unit
// never formats doubles (see the note at the top).
std::string describe_duration(simsys::SimTime t) {
  struct Unit {
    std::string_view name;
    long long ns;
  };
  constexpr std::array units{
      Unit{"h", 3'600'000'000'000}, Unit{"m", 60'000'000'000}, Unit{"s", 1'000'000'000},
      Unit{"ms", 1'000'000},        Unit{"us", 1'000},         Unit{"ns", 1}};
  const long long ticks = t.count();
  for (const auto& unit : units) {
    if (ticks != 0 && ticks % unit.ns == 0) return std::format("{}{}", ticks / unit.ns, unit.name);
  }
  return "0s";
}

// `--duration` overrides the scenario's; like the scenario's own, it must leave a non-empty
// window after the warmup (the loader enforces the same rule on the file). The text is
// the user's, so the message echoes what they typed.
std::expected<simsys::SimTime, std::string> run_duration(const std::filesystem::path& file,
                                                         const simsys::model::Scenario& scenario,
                                                         const std::optional<std::string>& text) {
  if (!text) return scenario.duration;
  const auto parsed = simsys::parse_duration(*text);
  if (!parsed) {
    return std::unexpected(std::format("{}: error: --duration '{}': {}", file.string(),
                                       parsed.error().input, parsed.error().message));
  }
  if (parsed->count() <= scenario.warmup.count()) {
    return std::unexpected(std::format("{}: error: --duration {} is not longer than the warmup {}",
                                       file.string(), *text, describe_duration(scenario.warmup)));
  }
  return *parsed;
}

// What `run` takes beyond the report options.
struct RunArgs {
  std::optional<std::string> duration{};  // overrides the scenario's
  std::optional<std::string> series{};    // CSV output path
};

// `simsys run <file>`: steps through the scenario, feeding every record to the summary
// and, when asked, to the CSV series (the contract a GUI plots from).
int run_scenario(const std::filesystem::path& file, const RunArgs& args,
                 const ReportOptions& options) {
  const auto g = load_graph(file);
  if (!g) {
    print_error(g.error());
    return 1;
  }
  const auto& scenario = g->scenario();
  const auto duration = run_duration(file, scenario, args.duration);
  if (!duration) {
    print_error(duration.error());
    return 1;
  }
  auto sim = create_simulation(*g, file);
  if (!sim) {
    print_error(sim.error());
    return 1;
  }

  std::ofstream series_file;
  std::optional<report::SeriesWriter> series;
  if (args.series) {
    series_file.open(*args.series);
    if (!series_file) {
      print_error(*args.series + ": error: cannot open for writing");
      return 1;
    }
    series.emplace(series_file, *g);
  }

  report::Summary summary{*g, scenario.warmup};
  // Pattern: Observer — the step loop hands each record to one callable; this lambda fans
  // it out to the two observers (summary, optional series) so neither knows of the other.
  const auto observe = [&](const engine::StepRecord& record) {
    summary.observe(record);
    if (series) series->observe(record);
  };
  // C++20 designated initializers: members named at the call site, in declaration order,
  // replacing a positional `{duration, step}` where a reader must remember which is which.
  sim->run({.duration = *duration, .step = scenario.step}, observe);
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
                                     file.string(), describe_duration(scenario.step)));
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

// The options `estimate` and `run` share.
void add_report_options(CLI::App& command, ReportOptions& options) {
  command.add_option("--report", options.format, "output format: table (default) or json")
      ->check(CLI::IsMember({"table", "json"}));
  command.add_flag("--no-color", options.no_color,
                   "never colour the table (default: colour when stdout is a terminal)");
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"simsys - a fluid-flow system design simulator"};
  app.set_version_flag("--version", std::string{simsys::version});
  app.require_subcommand(1);

  std::filesystem::path scenario_file;
  ReportOptions report_options;
  RunArgs run_args;
  int seed{};

  auto* validate = app.add_subcommand("validate", "check a scenario's types, references and units");
  auto* explain = app.add_subcommand(
      "explain", "print every resolved number, where it came from, and a static load check");
  auto* estimate = app.add_subcommand("estimate", "evaluate the steady state once and check SLOs");
  auto* run = app.add_subcommand("run", "step through the scenario and check SLOs");
  for (auto* command : {validate, explain, estimate, run}) {
    command->add_option("file", scenario_file, "scenario YAML file")
        ->required()
        ->check(CLI::ExistingFile);
  }
  CurveArgs curve_args;
  auto* curve = app.add_subcommand("curve", "print a client's rate curve as CSV (t_s,rate_per_s)");
  curve->add_option("file", scenario_file, "scenario YAML file")
      ->required()
      ->check(CLI::ExistingFile);
  curve->add_option("client", curve_args.client, "id of a client node")->required();
  curve->add_option("--step", curve_args.step, "time between samples (default 1m)");
  curve->add_option("--until", curve_args.until,
                    "end of the curve, exclusive (default: scenario's duration)");
  curve->add_option("--seed", curve_args.seed, "noise seed (default: scenario's seed)");
  curve->add_flag("--noise", curve_args.noise,
                  "multiply in the client's seeded noise, advanced every scenario step as in a run "
                  "(--step must then be a multiple of it)");
  add_report_options(*estimate, report_options);
  add_report_options(*run, report_options);
  run->add_option("--duration", run_args.duration,
                  "simulated time, e.g. 30s (default: scenario's)");
  run->add_option("--seed", seed, "random seed (reserved for phase 2 workload noise; unused)");
  run->add_option("--series", run_args.series, "write one CSV row per node per step to this file");

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
  return 0;
}
