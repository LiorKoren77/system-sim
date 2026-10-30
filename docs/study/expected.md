# C++23 `std::expected<T, E>`

## Used in

- `src/core/core.cppm`: `parse_duration`, `parse_rate`, `parse_byte_rate`, `parse_size` return
  `std::expected<SimTime | Rate | ByteRate | Bytes, ParseError>`; the `simsys::detail` helpers
  (`split_number`, `require_non_negative_finite`, `split_non_negative_number`,
  `nanoseconds_per`, `to_sim_time`, `parse_scaled_number`) chain the same type.
- `src/config/config.cpp`: `simsys::config::load` returns `std::expected<Scenario, ConfigError>`;
  every reader and check of the config units (`header.cpp`, `nodes.cpp`, `workload.cpp`, ...) returns `std::expected<T, ConfigError>`, with
  `CheckResult = std::expected<void, ConfigError>` for a check that produces no value
  (`check_top_level`, `read_version`, `read_capacity`, ...), and `validate` chains them.
- `src/engine/graph.cpp` and `src/engine/engine.cppm`: `Graph::build` returns
  `std::expected<Graph, EngineError>` (the refusals the loader cannot see, such as a step with
  no link) and `Simulation::create` returns `std::expected<Simulation, EngineError>` (a node
  type the engine does not run yet).
- `src/session/session.cpp`: `load_graph`, `build_graph`, `create_simulation` and
  `run_summary(graph, path)` turn those errors into the finished message
  (`std::expected<T, std::string>`) for the CLI and the sweep; `src/sweep/sweep.cpp` adds the
  point's label in front of it.
- `src/cli/main.cpp`: the consumer (`run_validate`: `if (!scenario) { print_error(describe(
  scenario.error())); ... }`).

## What

A value-or-error sum type: it holds either a `T` or an `E`, never both, never neither.

| Operation | Meaning |
|---|---|
| `has_value()`, `explicit operator bool` | true when it holds a value |
| `*e`, `e->` | the value; **undefined behaviour** if it holds an error |
| `e.value()` | the value; throws `std::bad_expected_access<E>` if it holds an error |
| `e.error()` | the error; undefined behaviour if it holds a value |
| `e.value_or(x)` | the value, or `x` |
| `std::unexpected(err)` | wraps an error so it converts to any `expected<T, E>` |

Monadic operations (C++23) act on the value or the error without an `if`:

| Operation | Function must return | Effect |
|---|---|---|
| `transform(f)` | a plain value | `expected<T,E>` -> `expected<U,E>`; errors pass through |
| `and_then(f)` | an `expected<U,E>` | chain a step that can itself fail |
| `transform_error(f)` | a plain error | value passes through; error `E` -> `G` |
| `or_else(f)` | an `expected<T,G>` | recover from or replace an error |

Three uses in this code base, and why they were the right ones:

```cpp
// core.cppm, parse_rate: the last step, wrapping the double in a Rate, cannot fail
return detail::parse_scaled_number(number_text, text, detail::rate_prefixes)
    .transform([](double v) { return Rate{v}; });

// src/config/detail.hpp, parse_scalar: give the unit parser's error a location and the key's name
return parse(value.Scalar()).transform_error([&](const ParseError& error) {
  return unit_error(site, value, label, error);
});

// session.cpp, load_graph: two fallible stages, one error type at the end
return config::load(path, overrides)
    .transform_error(config::describe)   // ConfigError -> its message
    .and_then([&](model::Scenario scenario) { return build_graph(path, std::move(scenario)); });
```

`transform` fits `parse_rate` because the final step is infallible, so there is no second error
to merge (`and_then` would only add noise). `transform_error` fits `parse_scalar` because the
value needs no change and only the error type must change, `ParseError` -> `ConfigError` (with
file, line, column and the key's name in front). `load_graph` uses both: the loader's error
becomes a string first, so that the graph builder's stage, which fails with a string too, can be
chained with `and_then`, which requires the same error type on both sides.

`transform` also turns "read a value" into "read it into this field". `store_in(target)`
(`src/config/detail.hpp`) returns a lambda that assigns its argument and returns nothing, so
`parse_rate(text).transform(store_in(node.capacity))` is an `expected<void, ParseError>`:
stored, or why not. `read_capacity` chains such steps with `and_then`. Steps whose value the
next line needs still use early return (below).

## Why

- **Versus exceptions**: a `throw` is invisible in the signature, and the error path is
  far more expensive than a normal return. Bad input in a scenario file is expected, not exceptional.
  `CLAUDE.md` reserves exceptions for programmer errors.
- **Versus error codes / out-parameters**: a bare `int` or `bool` loses the detail, and callers can
  ignore it. An `expected` has no value to use until you look at it.
- **Structured errors**: `ParseError{input, message}` and `ConfigError{file, line, column,
  message}` carry data that `main.cpp` formats as `file:line:column: error: message`.
- **`[[nodiscard]]` is added by hand**: libstdc++ 14's `std::expected` is not marked nodiscard, so
  every function here that returns one carries the attribute (see the comment on
  `detail::fail` in `core.cppm`). Without it, `parse_duration(s);` would compile silently.

## Patterns used here

Early-return propagation, for steps that can fail (`core.cppm`, `parse_duration`):

```cpp
auto split = detail::split_non_negative_number(text, text);
if (!split) return std::unexpected(split.error());
auto [value, unit] = *split;
```

`std::expected<void, E>` for checks that produce no value (`CheckResult` in `detail.hpp`;
`read_version` in `header.cpp`):

```cpp
using CheckResult = std::expected<void, ConfigError>;
CheckResult read_version(const Site& site, const YAML::Node& root, int& version) {
  ...
  if (!number || *number != 1) return site.fail(value, "version must be 1");
  version = *number;
  return {};   // {} = "fine"
}
```

`// Pattern: Pipeline with early exit (railway-oriented error handling)` (`config.cpp`,
`validate`): each stage returns an `expected`; they run in order and the first failure is
returned. Chains of `if (auto checked = check_x(...); !checked) return checked;` forward a
`CheckResult` unchanged (`check_top_level`), while `validate` converts to
`expected<Scenario, ConfigError>` with `std::unexpected(checked.error())`.

The **single exception boundary** is `config::load`: yaml-cpp throws `YAML::Exception` on syntax
and shape errors, so `load` wraps `YAML::LoadFile(file)`, the overrides and `validate` in one
`try`/`catch` and returns `std::unexpected(detail::error_at_mark(file, e.mark, e.msg))`. Callers
never need `try`.

## Pitfalls

- **`*e` on an error is UB**, with no diagnostic. Always test (`if (!e) ...`) first; note that
  the code above dereferences only after the check (`*split`, `*number`).
- **`value()` throws** `std::bad_expected_access<E>` on an error. That is a hidden exception
  path; prefer an explicit check, or `value_or` when a default is meaningful.
- **`E` must be copyable or movable**: `std::unexpected(split.error())` copies the error. Fine
  for two strings; take it by move if an error ever gets heavy.
- **`auto operator<=>` on `double` members deduces `std::partial_ordering`** (`Rate`, `ByteRate`
  in `core.cppm`): NaN is unordered, so `a < b` and `a >= b` can both be false. Instead of
  guarding every comparison, the parsers reject NaN and infinity up front
  (`require_non_negative_finite`, and a second finite check after scaling in
  `parse_scaled_number`), so no `Rate` holding NaN can exist.
