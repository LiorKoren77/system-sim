// `simsys` command-line entry point. Parsing is delegated to CLI11; commands are thin
// wrappers over library calls so they stay testable without a process.
//
// Exit codes: 0 the run worked and every SLO passed, 2 it worked but an SLO failed, 1 an
// error (bad file, bad option, a scenario the engine cannot run yet).
#include <unistd.h>  // isatty, STDOUT_FILENO: POSIX, which is fine for this project
#include <CLI/CLI.hpp>

#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <utility>

// C++20 `import`: brings the exported names of a named module into this translation
// unit. Unlike #include, it does not re-parse text; the compiler reads a compiled
// module interface (BMI). Imports go after the #includes because GCC 14 reports
// redefinition errors when a std header is #included after importing a module that
// included the same header in its global module fragment.
import simsys.core;
import simsys.config;
import simsys.engine;
import simsys.graph;
import simsys.model;
import simsys.report;

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

// Prints the summary with its SLO verdicts and turns them into the exit code.
int print_report(const report::Summary& summary, const ReportOptions& options) {
  const auto& slos = summary.graph().scenario().slos;
  const auto results = report::evaluate_slos(summary, slos);
  if (options.format == "json") {
    std::println("{}", report::render_json(summary, results));
  } else {
    std::print("{}", report::render_table(summary, results, use_color(options)));
  }
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
  auto duration = scenario.duration;
  if (args.duration) {
    const auto parsed = simsys::parse_duration(*args.duration);
    if (!parsed) {
      print_error("--duration '" + parsed.error().input + "': " + parsed.error().message);
      return 1;
    }
    duration = *parsed;
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
  sim->run({.duration = duration, .step = scenario.step}, observe);
  return print_report(summary, options);
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
  return 0;
}
