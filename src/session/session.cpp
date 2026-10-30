// Module `simsys.session`, implementation unit: the loading and running every command shares
// (see session.cppm), the worker pool, and the replications of `run --replications`. <format>,
// <thread> and <atomic> are included here, not in the interface (CLAUDE.md §3: <format> must not
// be in it; the threads are how runs are spread, not part of what a session is, so no importer
// parses those headers). Only `{}` placeholders, never a width or a precision (the second GCC
// 14.2 defect).
module;
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
import simsys.workload; // workload::Workload: whether a client's rate has noise

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
  // The fault-free twin's Summary (engine::FaultRecord's header: the counterfactual every fault
  // block is measured against). It observes nothing without faults, whose run has no twin.
  report::Summary twin{g, scenario.warmup};
  summary.watch_faults(sim.fault_records());
  twin.watch_faults(sim.fault_records());
  // Pattern: Observer — the step loop hands each record to one callable; this lambda fans it out
  // to the summaries and the optional series writers, so none of them knows of another.
  const auto observe = [&](const engine::StepRecord& record) {
    summary.observe(record);
    if (const auto* twin_record = sim.twin_record()) twin.observe(*twin_record);
    if (observers.series != nullptr) observers.series->observe(record);
    if (observers.series_ops != nullptr) observers.series_ops->observe(record);
  };
  // C++20 designated initializers: the members named at the call site, in declaration order,
  // replacing a positional `{duration, step}` where a reader must remember which is which.
  sim.run({.duration = scenario.duration, .step = scenario.step}, observe);
  // What each scripted fault cost, against the twin, for the reports' block per fault (none
  // without faults).
  summary.set_faults(sim.fault_records(), twin);
  return summary;
}

std::expected<report::Summary, std::string> run_summary(const graph::Graph& g,
                                                        const std::string& path) {
  // C++23 std::expected::transform: maps the value (a simulation into its run's summary) and
  // passes an error through untouched; unlike and_then, the mapping itself cannot fail.
  return create_simulation(g, path).transform(
      [&](engine::Simulation sim) { return run_summary(g, sim); });
}

// ---- the worker pool ----------------------------------------------------------------------------

namespace {

// One worker: takes the next unclaimed index until none is left. fetch_add returns the old value
// and increments in one indivisible step, so no two workers ever get the same index and none is
// skipped, without a lock.
void pull_indices(std::atomic<std::size_t>& next, std::size_t count,
                  const std::function<void(std::size_t)>& work) {
  for (auto index = next.fetch_add(1); index < count; index = next.fetch_add(1)) work(index);
}

}  // namespace

// Pattern: Thread pool / work queue — a fixed set of workers pulls tasks from one shared queue;
// the queue here is just an atomic counter over the indices 0..count-1, since the tasks are known
// up front and need no other state. A worker that finishes a short task takes the next one, so a
// slow one does not hold back a whole share of them (as a static split into `jobs` slices
// would). The pool lives for one call only: its workers start here and are joined before this
// function returns, so each phase of a sweep (load every point, then run every point) and each
// `run --replications` gets a fresh pool; nothing is submitted to it later, unlike a long-lived
// pool of a server.
void run_in_parallel(std::size_t count, std::size_t jobs,
                     const std::function<void(std::size_t)>& work) {
  // C++11 std::atomic<T>: a value that threads read and modify without a data race; fetch_add is
  // one indivisible read-modify-write. A plain std::size_t incremented by several threads would
  // be a data race: undefined behaviour, in practice two workers running one task.
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

// ---- replications -------------------------------------------------------------------------------
//
// What replications are for. A run is deterministic for its seed, and the clients' rate noise
// (spec §17: an Ornstein-Uhlenbeck multiplier on each client's curve, workload::NoiseProcess) is
// its one random input. One run is therefore one sample of the noise: its p99 of 5.69 ms says what
// THAT afternoon of traffic did, not what the system does. N runs over seeds seed .. seed + N - 1
// are N afternoons; the spread of their numbers (the report's median and 10-90 % band,
// report::summarize_replications) is the "how sure are we" of a capacity plan, which System Design
// Interview's back-of-the-envelope estimates (ch. 2) leave to the reader's judgement.
//
// What is deliberately ignored: the band is a spread of the runs, not a confidence interval of a
// mean (no t-distribution, no batch means, no claim about runs not made); the scripted faults are
// the same in every replication (no random or correlated failures across runs: a fault is part of
// the scenario, not of the noise). Consecutive seeds are fine: each seeds its own std::mt19937_64
// per client (workload::client_seed), whose seeding already makes neighbouring seeds' streams
// unrelated.
//
// Why seeds and not one long run: the noise is correlated over `correlation` (seconds to minutes),
// and a scenario's curve (a daily wave, spikes) is not stationary, so one run twice as long is not
// two independent samples of the same afternoon; N runs of the same scenario are.

namespace {

// Replication k's seed: `seed + k`, wrapping around past INT_MAX instead of overflowing (signed
// overflow is undefined behaviour). The sum is done in unsigned arithmetic, which wraps by
// definition, and converted back: C++20 defines that conversion of an out-of-range value to a
// signed type as modular (two's complement), where before it was implementation-defined.
int seed_of(int seed, std::size_t k) {
  return static_cast<int>(static_cast<unsigned>(seed) + static_cast<unsigned>(k));
}

// One replication: a simulation of `g` with its own seed, run like any `simsys run`. create cannot
// fail: run_replications checked `g` before the workers started.
report::Summary run_seed(const graph::Graph& g, int seed) {
  auto sim = engine::Simulation::create(g, seed);
  return run_summary(g, *sim);
}

// Throws std::invalid_argument when the engine refuses `g` (run_replications' precondition), on the
// calling thread: an exception thrown in a worker would end the process (std::terminate).
void require_runnable(const graph::Graph& g) {
  const auto sim = engine::Simulation::create(g);
  if (!sim) throw std::invalid_argument{"session::run_replications: " + sim.error().message};
}

}  // namespace

std::vector<int> replication_seeds(int seed, std::size_t count) {
  std::vector<int> seeds;
  for (std::size_t k = 0; k < count; ++k) seeds.push_back(seed_of(seed, k));
  return seeds;
}

bool has_rate_noise(const graph::Graph& g) {
  // C++20 std::ranges::any_of: the range itself instead of a begin/end pair (std::any_of).
  return std::ranges::any_of(g.nodes(), [](const graph::ResolvedNode& node) {
    return node.spec.workload && node.spec.workload->noise.has_value();
  });
}

// Each replication is its own Simulation and writes only its own slot, so the workers share
// nothing but `g` (read only) and the counter of run_in_parallel; the slots are read after the
// joins. A slot is a std::optional because a Summary has no empty state to preallocate.
std::vector<report::Summary> run_replications(const graph::Graph& g, int seed, std::size_t count,
                                              std::size_t jobs) {
  require_runnable(g);
  const auto seeds = replication_seeds(seed, count);
  std::vector<std::optional<report::Summary>> slots(count);
  run_in_parallel(count, jobs, [&](std::size_t k) { slots[k].emplace(run_seed(g, seeds[k])); });
  std::vector<report::Summary> summaries;
  summaries.reserve(count);
  for (auto& slot : slots) summaries.push_back(std::move(*slot));
  return summaries;
}

}  // namespace simsys::session
