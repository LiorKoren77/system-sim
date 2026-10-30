// Module `simsys.session`, interface unit: what every command does with a scenario file, in one
// place. `simsys explain`, `estimate`, `run` and each point of `sweep` load the file into a
// resolved graph (the loader's checks, then the graph builder's), and `run` and `sweep` run it
// into a report::Summary. Before this module the sweep had its own copy of the CLI's loading and
// running; now both call the functions below, so a point of a sweep
// is exactly a `simsys run` of the scenario with one value replaced. The worker pool that runs
// the points of a sweep side by side lives here too (run_in_parallel), so that `run
// --replications` runs its seeds on the same pool (run_replications).
//
// Models: nothing from the book; this is plumbing between the layers. Replications are the one
// piece of method: see run_replications.
//
// C++20 named module, interface unit (see src/core/core.cppm for the longer explanation). Lean
// on purpose, like every interface that imports another module: no <chrono>, <format>,
// <filesystem> or <print> here (the GCC 14.2 'Bad file data' defect, CLAUDE.md §3); the messages
// are formatted in session.cpp.
module;
#include <cstddef>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <vector>

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

// Calls work(0) ... work(count - 1), each exactly once, on `jobs` worker threads (at least 1, at
// most one per index), and returns when every call has returned. `work` must be safe to call from
// several threads at once: each call should write only what belongs to its own index. The pool of
// `simsys sweep`'s points and of `run --replications`' seeds (docs/study/threads.md).
void run_in_parallel(std::size_t count, std::size_t jobs,
                     const std::function<void(std::size_t)>& work);

// Does some client's rate carry noise (`rate: {noise: ...}` with a sigma above 0)? The noise is the
// only random input of a run, so without it, or with `sigma: 0` (a multiplier of exactly 1 at every
// step), every seed gives the same run, and `run --replications` runs once.
[[nodiscard]] bool has_rate_noise(const graph::Graph& g);

// The seeds of `count` replications from `seed`: seed, seed + 1, ..., wrapping around past INT_MAX
// to INT_MIN instead of overflowing (see session.cpp).
[[nodiscard]] std::vector<int> replication_seeds(int seed, std::size_t count);

// `count` runs of `g`, replication k (from 0) with the clients' noise seeded from `seed + k`
// (engine::Simulation::create(g, seed)), on `jobs` workers (run_in_parallel), returned in seed
// order: the summaries do not depend on `jobs`. Every summary names `g`, which must outlive them.
// `g` must be runnable (create_simulation accepted it): a graph the engine refuses is a programmer
// error here, std::invalid_argument. Replication k's seed is replication_seeds(seed, count)[k].
[[nodiscard]] std::vector<report::Summary> run_replications(const graph::Graph& g, int seed,
                                                            std::size_t count, std::size_t jobs);

}  // namespace simsys::session
