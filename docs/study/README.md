# C++20/23 study notes

One short note per language or library feature used in the simulator: what it is, the
pre-C++20 way, where it is used in the code (file and function, never a line number: lines
move), a pitfall met while building it, and further reading. Features small enough to need
no note of their own share [small-features.md](small-features.md). See §10 of the design spec
for the planned list.

| Note | Feature | Standard | Main uses in the code |
|---|---|---|---|
| [modules.md](modules.md) | named modules, interface vs implementation units, the global module fragment, module linkage, partitions (interface and implementation kinds, `export import`); the ten GCC 14.2 defects | C++20 | the 15 modules of `src/` and the tests' `simsys.test_fixtures` (a table of all of them); the partitions `simsys.engine:records`, `:faults`, `:fault_log` and `simsys.report:shared` |
| [expected.md](expected.md) | `std::expected`, monadic `and_then` / `transform` / `transform_error` | C++23 | `core.cppm` unit parsers, `config::load`, `Graph::build`, `Simulation::create`, `session::load_graph`, `main.cpp` |
| [concepts.md](concepts.md) | concepts, requires-expressions, constrained `auto` parameters, constrained template parameters (`template <C T>`) | C++20 | `NodeBehaviour`, `RequestSource`, `ResponseLookup`, `FractionLookup` (`behaviours.cppm`), `HitRatioModel` (`caching.cppm`), `StepObserver` (`engine.cppm`) |
| [ranges.md](ranges.md) | range algorithms with projections, views; `contains`, `fold_left`, `to`, `zip`, `join_with`, `chunk_by` (and the GCC defect it met), `iota`, `stable_sort` | C++20 / C++23 | `Flow::position_of`, `Graph::connect_steps`, `Graph::resolve_request_sizes`, the loader's table lookups, report renderers, `rank_order` (`run --replications`) |
| [variant-visit.md](variant-visit.md) | `std::variant`, `std::visit`, the `Overloaded` set, CTAD for aggregates; keeping a 15-alternative variant and the `NodeType` switch exhaustive | C++17 / C++20 | `behaviours::Behaviour`, `SloSpec::max`, `Simulation::forward_pass`, `Simulation::make_node`, `limit_value` |
| [format-print.md](format-print.md) | `std::format`, `std::print` / `std::println`, `std::to_chars`; two GCC 14.2 defects | C++20 / C++23 | loader messages, `session.cpp`, `main.cpp`, `simsys::to_chars_text` and every number of `simsys.report` (`shared.cppm`, `report.cpp`, `explain.cpp`) |
| [generator.md](generator.md) | coroutines, `co_yield`, `std::generator` (lazy single-pass ranges) | C++20 / C++23 | `workload::samples`, `curve_samples` and `print_samples` in `main.cpp` (`simsys curve`) |
| [random.md](random.md) | `<random>` engines vs distributions, `std::mt19937_64`, seeding; the Ornstein–Uhlenbeck noise | C++11 | `workload::NoiseProcess`, `workload::client_seed`, `ClientBehaviour::tick` |
| [numerics.md](numerics.md) | `expm1` / `log1p`, bisection vs Newton, sums kept in [0, 1], latency moments, fluid steps and choosing the step, a quorum's order statistics by quadrature, the ring's hottest machine, the truncated lognormal of a client's timeout, a flushed cache's warm-up, retries as a geometric series and their fixed point, a gateway's connection level (Euler vs the exponential) and its reconnect storm (floating point and models, not a language feature) | C++11 `<cmath>` | `caching.cppm` (`power_integral`, `characteristic_time`, `refill_hit_ratio`), `queueing.cppm` (`erlang_c`, `lognormal_quantile`, `lognormal_below`, `lognormal_order_statistic`), `flow.cppm`, `fluid.cppm`, `faults.cppm` (`count_reads`), `behaviours.cppm` (`ClientBehaviour`'s retries, `WsGatewayBehaviour`) |
| [threads.md](threads.md) | `std::jthread`, `std::stop_token`, `std::atomic` as a work queue, data races | C++20 / C++11 | `run_in_parallel` (`src/session/session.cpp`, `simsys sweep` and `run --replications`), `PerPoint` (`src/sweep/sweep.cpp`) |
| [small-features.md](small-features.md) | designated initializers, `std::span`, `<=>` and defaulted `==`, `using enum`, `consteval`, `std::to_array`, `std::to_underlying`, `std::unreachable`, `std::chrono` durations, deducing `this`, `static operator()`, `std::erase_if` | C++20 / C++23 | everywhere; each section names its main uses |

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
  (`src/report/report.cppm`), fanned out by one lambda in `session::run_summary`
  (`src/session/session.cpp`), which `simsys run` and every point of `simsys sweep` call: each
  observer sees every step record and none knows of another.

Not every reuse is a named pattern, and none is claimed where none applies: `defaults_of`
(`src/defaults/defaults.cppm`) makes an id generator a `ServiceBehaviour` whose two numbers its
`algorithm` supplies, with no new class; and a cache's `strategy` (`sends_to_store` in
`src/engine/graph.cpp`) is caching practice's word for a closed enum tested in one place, not
the GoF Strategy pattern.

Phase 2c added these:

- `Pattern: Strategy` on the `Behaviour` variant (`src/engine/behaviours.cppm`) and on the
  engine's `make_node` (`src/engine/engine.cppm`): phase 2c appended five alternatives
  (`BlobStoreBehaviour`, `MessageQueueBehaviour`, `WorkerPoolBehaviour`, `ExternalSinkBehaviour`,
  `KvClusterBehaviour`), and every `std::visit` that missed one stopped compiling until it was
  handled ([variant-visit.md](variant-visit.md) says how twelve alternatives stay exhaustive
  with two lambdas per `visit`). A search index is no new alternative: like the id generator it is a `ServiceBehaviour`
  with the type's numbers.
- `Pattern: Delegation` on `detail::HopBlocks` (`src/engine/behaviours.cppm`): the blob store's
  event notifications and the worker pool's `then` steps hold the same blocks of hops a service and
  a cache hold, built by one shared `detail::add_then_blocks`, instead of a base class.
- `Pattern: Thread pool / work queue` on `run_in_parallel` (`src/session/session.cpp`): `simsys
  sweep`'s points and `run --replications`' seeds on `std::jthread` workers that pull indices from one `std::atomic<std::size_t>`
  ([threads.md](threads.md)); and `Pattern: Pipeline with early exit` on the sweep's stages (the
  file, the metrics, every point loaded, then every point run).
- `Pattern: Static factory (named constructor)` on `MegabyteService::serialising`
  (`src/engine/behaviours.cppm`): a link's pipe is timed by the byte-sized node's arithmetic, built
  from a bandwidth rather than a node.
- A workaround, not a pattern: `detail::same_op` (`src/engine/graph.cpp`) is a named function
  where a lambda would read better, because GCC 14.2 emits a lambda passed to a range adaptor in a
  module function in every importing object file ("multiple definition" at link time: the fifth
  defect in `CLAUDE.md`). A lambda inside a function body that is not handed to a range adaptor is
  fine.
- Two C++20/23 constructs met for the first time in 2c: the C++23 `static operator()` of
  `NothingLost` (`src/engine/behaviours.cppm`: a call operator that reads no member, so a call
  passes no `this`; [small-features.md](small-features.md#static-operator)), the default
  lookup of the `template <FractionLookup Lookup = NothingLost>` route templates, whose
  constrained template parameter (`template <C T>`, C++20) is spelled out rather than written
  `FractionLookup auto&&` because a default argument needs a type to default to
  ([concepts.md](concepts.md)); and C++20 `std::jthread`, a thread that joins in its destructor
  ([threads.md](threads.md)).

Phase 3 added these:

- `Pattern: Decorator (static, over the NodeBehaviour concept)` on `WsGatewayBehaviour`
  (`src/engine/behaviours.cppm`): the gateway wraps a `ServiceBehaviour`, has the same interface
  (`route`, `cost`, `response`, `delivered`) and adds the connection level around it, with no base
  class and no virtual call: the concept checks the interface at compile time.
- `Pattern: Circular buffer` on the worker pool's `RepublishRing` (`src/engine/behaviours.cppm`),
  "like `engine::DelayLine`": the jobs a pool re-publishes wait in slots a head index moves over,
  as a link's delay line does, so nothing is shifted as the steps go by.
- `Pattern: Table-driven method` on `kind_keys` (`src/config/faults.cpp`): which fault kinds read
  `replicas`, `factor`, `extra` and `datacenter` is one row per key, so one loop refuses each on
  every other kind; and on `fault_kind_table` (`src/model/model.cppm`), the kind names read by the
  loader and printed by `explain`.
- Not a pattern but a design choice worth reading: the fault-free **twin** (`Simulation::twin_`,
  `src/engine/engine.cppm`), a second `Simulation` of the same scenario without its faults,
  stepped in lockstep, so that every fault block compares a run with the run that would have been
  (spec §20 entry 5). Held in a `std::vector<Simulation>` used as an optional because of the ninth
  GCC 14.2 defect ([modules.md](modules.md)).
- Two C++20/23 constructs met for the first time in phase 3: C++20 `std::erase_if`
  (`in_one_unit`, `src/engine/fault_log.cppm`; [small-features.md](small-features.md#stderase_if))
  and C++23 `std::ranges::iota` beside C++20 `std::ranges::stable_sort` with a projection
  (`rank_order`, `src/report/report.cpp`; [ranges.md](ranges.md)); and C++20 module partitions of
  the interface kind, `export module simsys.engine:faults;` re-exported by `export import
  :faults;` ([modules.md](modules.md)).
