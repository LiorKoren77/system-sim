# C++20/23 study notes

One short note per language or library feature used in the simulator: what it is, the
pre-C++20 way, where it is used in the code (file and function), a pitfall met while
building it, and further reading. See §10 of the design spec for the planned list.

| Note | Feature | Standard | Main uses in the code |
|---|---|---|---|
| [modules.md](modules.md) | named modules, interface vs implementation units, the global module fragment | C++20 | every `src/**/*.cppm`; `config.cppm` / `config.cpp` |
| [expected.md](expected.md) | `std::expected`, monadic `and_then` / `transform` / `transform_error` | C++23 | `core.cppm` unit parsers, `config::load`, `Graph::build`, `main.cpp` |
| [concepts.md](concepts.md) | concepts, requires-expressions, constrained `auto` parameters, constrained template parameters (`template <C T>`) | C++20 | `NodeBehaviour`, `RequestSource`, `ResponseLookup`, `FractionLookup` (`behaviours.cppm`), `StepObserver` (`engine.cppm`) |
| [ranges.md](ranges.md) | range algorithms with projections, views; `contains`, `fold_left`, `to`, `zip`, `join_with` | C++20 / C++23 | `Flow::position_of`, `Graph::connect_steps`, the loader's table lookups, report renderers |
| [variant-visit.md](variant-visit.md) | `std::variant`, `std::visit`, the `Overloaded` set, CTAD for aggregates | C++17 / C++20 | `behaviours::Behaviour`, `SloSpec::max`, `Simulation::forward_pass`, `limit_value` |
| [format-print.md](format-print.md) | `std::format`, `std::print` / `std::println`, `std::to_chars`; two GCC 14.2 defects | C++20 / C++23 | loader messages, `main.cpp`, every number in `report.cpp` |
| [generator.md](generator.md) | coroutines, `co_yield`, `std::generator` (lazy single-pass ranges) | C++20 / C++23 | `workload::samples`, `curve_samples` and `print_samples` in `main.cpp` (`simsys curve`) |
| [random.md](random.md) | `<random>` engines vs distributions, `std::mt19937_64`, seeding; the Ornstein–Uhlenbeck noise | C++11 | `workload::NoiseProcess`, `behaviours::client_seed`, `ClientBehaviour::tick` |
| [numerics.md](numerics.md) | `expm1` / `log1p`, bisection vs Newton, sums kept in [0, 1], latency moments, fluid steps and choosing the step, a quorum's order statistics by quadrature, the ring's hottest machine, the truncated lognormal of a client's timeout (floating point, not a language feature) | C++11 `<cmath>` | `caching.cppm` (`power_integral`, `characteristic_time`), `queueing.cppm` (`erlang_c`, `lognormal_quantile`, `lognormal_below`, `exponential_order_statistic`), `flow.cppm`, `fluid.cppm` |
| [threads.md](threads.md) | `std::jthread`, `std::stop_token`, `std::atomic` as a work queue, data races | C++20 / C++11 | `run_in_parallel`, `PerPoint` (`src/sweep/sweep.cpp`, `simsys sweep`) |

## Where to look: design patterns

Every design pattern is named where it is used, in a `Pattern:` comment, so
`grep -rn 'Pattern:' src` lists them all. Phase 2b added three worth reading side by side:

- `Pattern: Delegation` on `CdnBehaviour` (`src/engine/behaviours.cppm`): a CDN is the
  read-through shape of a cache, so it holds a `CacheBehaviour` and forwards every call to it
  instead of inheriting from it or copying it.
- The `CopyRole` enum (`server`, `primary`, `replica`) on `CopyGroup` in
  `src/engine/engine.cppm`, used by `make_database` under `Pattern: Strategy (data-driven)`: a
  closed enum states what a database copy is instead of inferring it from its shares.
- `Pattern: Observer` on `Summary`, `SeriesWriter` and `OpSeriesWriter`
  (`src/report/report.cppm`), fanned out by one lambda in `run` (`src/cli/main.cpp`): each
  observer sees every step record and none knows of another.

Not every reuse is a named pattern, and none is claimed where none applies: `defaults_of`
(`src/defaults/defaults.cppm`) makes an id generator a `ServiceBehaviour` whose two numbers its
`algorithm` supplies, with no new class; and a cache's `strategy` (`sends_to_store` in
`src/engine/graph.cppm`) is caching practice's word for a closed enum tested in one place, not
the GoF Strategy pattern.

Phase 2c added these:

- `Pattern: Strategy` on the `Behaviour` variant (`src/engine/behaviours.cppm`) and on the
  engine's `make_node` (`src/engine/engine.cppm`): phase 2c appended five alternatives
  (`BlobStoreBehaviour`, `MessageQueueBehaviour`, `WorkerPoolBehaviour`, `ExternalSinkBehaviour`,
  `KvClusterBehaviour`), and every `std::visit` that missed one stopped compiling until it was
  handled. A search index is no new alternative: like the id generator it is a `ServiceBehaviour`
  with the type's numbers.
- `Pattern: Delegation` on `detail::HopBlocks` (`src/engine/behaviours.cppm`): the blob store's
  event notifications and the worker pool's `then` steps hold the same blocks of hops a service and
  a cache hold, built by one shared `detail::add_then_blocks`, instead of a base class.
- `Pattern: Thread pool / work queue` on `run_in_parallel` (`src/sweep/sweep.cpp`): `simsys sweep`'s
  points on `std::jthread` workers that pull indices from one `std::atomic<std::size_t>`
  ([threads.md](threads.md)); and `Pattern: Pipeline with early exit` on the sweep's stages (the
  file, the metrics, every point loaded, then every point run).
- `Pattern: Static factory (named constructor)` on `MegabyteService::serialising`
  (`src/engine/behaviours.cppm`): a link's pipe is timed by the byte-sized node's arithmetic, built
  from a bandwidth rather than a node.
- A workaround, not a pattern: `detail::same_op` (`src/engine/graph.cppm`) is a named function
  where a lambda would read better, because GCC 14.2 emits a lambda passed to a range adaptor in a
  module function in every importing object file ("multiple definition" at link time: the fifth
  defect in `CLAUDE.md`). A lambda inside a function body that is not handed to a range adaptor is
  fine.
- Two C++20/23 constructs met for the first time in 2c: the C++23 `static operator()` of
  `NothingLost` (`src/engine/behaviours.cppm`: a call operator that reads no member, so a call
  passes no `this`), the default lookup of the `template <FractionLookup Lookup = NothingLost>`
  route templates, whose constrained template parameter (`template <C T>`, C++20) is spelled out
  rather than written `FractionLookup auto&&` because a default argument needs a type to default
  to ([concepts.md](concepts.md)); and C++20 `std::jthread`, a thread that joins in its destructor
  ([threads.md](threads.md)).
