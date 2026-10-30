// Module `simsys.session`, interface unit: what every command does with a scenario file, in one
// place. `simsys explain`, `estimate`, `run` and each point of `sweep` load the file into a
// resolved graph (the loader's checks, then the graph builder's), and `run` and `sweep` run it
// into a report::Summary. Before this module the sweep had its own copy of the CLI's loading and
// running; now both call the functions below, so a point of a sweep
// is exactly a `simsys run` of the scenario with one value replaced.
//
// Models: nothing from the book; this is plumbing between the layers.
//
// C++20 named module, interface unit (see src/core/core.cppm for the longer explanation). Lean
// on purpose, like every interface that imports another module: no <chrono>, <format>,
// <filesystem> or <print> here (the GCC 14.2 'Bad file data' defect, CLAUDE.md §3); the messages
// are formatted in session.cpp.
module;
#include <expected>
#include <span>
#include <string>

export module simsys.session;

// C++20 `import`: the compiled interfaces of the modules used here, instead of re-parsing their
// text as an #include would.
import simsys.config;
import simsys.engine;
import simsys.graph;
import simsys.model;
import simsys.report;

export namespace simsys::session {

// The scenario at `path` with each override applied before the loader checks it (so a replaced
// value meets every rule a value written in the file meets), resolved by the graph builder. The
// error is the finished message, compiler-style: the loader's `file:line:column: error: ...`, or
// `file: error: ...` for the graph builder's refusals.
// C++23 std::expected<T, E>: the graph or the message, replacing an exception or an out-parameter
// plus a bool; the caller must look before it uses the value.
// C++20 std::span<const T>: a non-owning view of contiguous elements, so the caller passes a
// vector, an array or `{}` without a copy.
[[nodiscard]] std::expected<graph::Graph, std::string> load_graph(
    const std::string& path, std::span<const config::Override> overrides = {});

// The second half of load_graph, for a scenario the caller has already loaded and edited
// (`run --seed` replaces the seed, which is not a value of the file an override could replace).
[[nodiscard]] std::expected<graph::Graph, std::string> build_graph(const std::string& path,
                                                                   model::Scenario scenario);

// A simulation of `g`, or `path: error: why not` (a node type the engine does not run yet).
[[nodiscard]] std::expected<engine::Simulation, std::string> create_simulation(
    const graph::Graph& g, const std::string& path);

// What else watches a run besides its Summary: the CSV series of `run --series` and
// `--series-ops`, each optional (a null pointer: not written).
struct RunObservers {
  report::SeriesWriter* series{};
  report::OpSeriesWriter* series_ops{};
};

// `sim` run over the scenario's duration at its step, every record observed by a Summary that
// starts at the scenario's warmup and by the given observers, and the faults' blocks set on it at
// the end (report::Summary::set_faults), judged against a second Summary of the fault-free twin's
// records (engine::Simulation::twin_record; none without faults). `sim` must be a simulation of
// `g`. `simsys sweep` reaches it too (each point); its rows show no fault block.
[[nodiscard]] report::Summary run_summary(const graph::Graph& g, engine::Simulation& sim,
                                          RunObservers observers = {});

// The same from the graph alone: create_simulation, then the run (a point of `simsys sweep`).
[[nodiscard]] std::expected<report::Summary, std::string> run_summary(const graph::Graph& g,
                                                                      const std::string& path);

}  // namespace simsys::session
