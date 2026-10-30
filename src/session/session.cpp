// Module `simsys.session`, implementation unit: the loading and running every command shares
// (see session.cppm). <format> is included here, not in the interface (CLAUDE.md §3); only `{}`
// placeholders, never a width or a precision (the second GCC 14.2 defect).
module;
#include <expected>
#include <format>
#include <span>
#include <string>
#include <utility>

// C++20 module implementation unit: `module simsys.session;` (no `export`) makes this file part
// of the named module; it sees every declaration of session.cppm and exports nothing.
module simsys.session;

// C++20 `import`: the compiled interfaces of the modules used here, instead of re-parsing their
// text as an #include would.
import simsys.config;
import simsys.engine;
import simsys.graph;
import simsys.model;
import simsys.report;

namespace simsys::session {

namespace {

// `path: error: message` for a refusal of the graph builder or the engine: they know no line of
// the file (the loader, which does, has already accepted it).
std::string engine_refusal(const std::string& path, const graph::EngineError& error) {
  // C++20 std::format: type-checked formatting into a std::string; `{}` placeholders only here.
  return std::format("{}: error: {}", path, error.message);
}

}  // namespace

std::expected<graph::Graph, std::string> load_graph(const std::string& path,
                                                    std::span<const config::Override> overrides) {
  // C++23 monadic std::expected: transform_error rewrites only an error (a ConfigError into its
  // message), and_then runs the next fallible stage only on a value and passes an error straight
  // through. The pre-C++23 way is an early return after each stage.
  return config::load(path, overrides)
      .transform_error(config::describe)
      .and_then([&](model::Scenario scenario) { return build_graph(path, std::move(scenario)); });
}

std::expected<graph::Graph, std::string> build_graph(const std::string& path,
                                                     model::Scenario scenario) {
  return graph::Graph::build(std::move(scenario))
      .transform_error(
          [&](const graph::EngineError& error) { return engine_refusal(path, error); });
}

std::expected<engine::Simulation, std::string> create_simulation(const graph::Graph& g,
                                                                 const std::string& path) {
  return engine::Simulation::create(g).transform_error(
      [&](const graph::EngineError& error) { return engine_refusal(path, error); });
}

report::Summary run_summary(const graph::Graph& g, engine::Simulation& sim,
                            RunObservers observers) {
  const auto& scenario = g.scenario();
  report::Summary summary{g, scenario.warmup};
  // Pattern: Observer — the step loop hands each record to one callable; this lambda fans it out
  // to the summary and the optional series writers, so none of them knows of another.
  const auto observe = [&](const engine::StepRecord& record) {
    summary.observe(record);
    if (observers.series != nullptr) observers.series->observe(record);
    if (observers.series_ops != nullptr) observers.series_ops->observe(record);
  };
  // C++20 designated initializers: the members named at the call site, in declaration order,
  // replacing a positional `{duration, step}` where a reader must remember which is which.
  sim.run({.duration = scenario.duration, .step = scenario.step}, observe);
  // What each scripted fault cost, for the reports' block per fault (none without faults).
  summary.set_faults(sim.fault_records());
  return summary;
}

std::expected<report::Summary, std::string> run_summary(const graph::Graph& g,
                                                        const std::string& path) {
  // C++23 std::expected::transform: maps the value (a simulation into its run's summary) and
  // passes an error through untouched; unlike and_then, the mapping itself cannot fail.
  return create_simulation(g, path).transform(
      [&](engine::Simulation sim) { return run_summary(g, sim); });
}

}  // namespace simsys::session
