# C++20/23 study notes

One short note per language or library feature used in the simulator: what it is, the
pre-C++20 way, where it is used in the code (file and function), a pitfall met while
building it, and further reading. See §10 of the design spec for the planned list.

| Note | Feature | Standard | Main uses in the code |
|---|---|---|---|
| [modules.md](modules.md) | named modules, interface vs implementation units, the global module fragment | C++20 | every `src/**/*.cppm`; `config.cppm` / `config.cpp` |
| [expected.md](expected.md) | `std::expected`, monadic `and_then` / `transform` / `transform_error` | C++23 | `core.cppm` unit parsers, `config::load`, `Graph::build`, `main.cpp` |
| [concepts.md](concepts.md) | concepts, requires-expressions, constrained `auto` parameters | C++20 | `NodeBehaviour`, `RequestSource`, `ResponseLookup` (`behaviours.cppm`), `StepObserver` (`engine.cppm`) |
| [ranges.md](ranges.md) | range algorithms with projections, views; `contains`, `fold_left`, `to`, `zip`, `join_with` | C++20 / C++23 | `Flow::position_of`, `Graph::connect_steps`, the loader's table lookups, report renderers |
| [variant-visit.md](variant-visit.md) | `std::variant`, `std::visit`, the `Overloaded` set, CTAD for aggregates | C++17 / C++20 | `behaviours::Behaviour`, `SloSpec::max`, `Simulation::forward_pass`, `limit_value` |
| [format-print.md](format-print.md) | `std::format`, `std::print` / `std::println`, `std::to_chars`; two GCC 14.2 defects | C++20 / C++23 | loader messages, `main.cpp`, every number in `report.cpp` |
| [generator.md](generator.md) | coroutines, `co_yield`, `std::generator` (lazy single-pass ranges) | C++20 / C++23 | `workload::samples`, `curve_samples` and `print_samples` in `main.cpp` (`simsys curve`) |
| [random.md](random.md) | `<random>` engines vs distributions, `std::mt19937_64`, seeding; the Ornstein–Uhlenbeck noise | C++11 | `workload::NoiseProcess`, `behaviours::client_seed`, `ClientBehaviour::tick` |
| [numerics.md](numerics.md) | `expm1` / `log1p`, bisection vs Newton, sums kept in [0, 1], latency moments, fluid steps (floating point, not a language feature) | C++11 `<cmath>` | `caching.cppm` (`power_integral`, `characteristic_time`), `queueing.cppm` (`erlang_c`, `lognormal_quantile`), `flow.cppm`, `fluid.cppm` |
