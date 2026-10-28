# C++23 `std::expected<T, E>`

## Used in

- `src/core/core.cppm`: `parse_duration`, `parse_rate`, `parse_byte_rate` return
  `std::expected<SimTime | Rate | ByteRate, ParseError>`; the `simsys::detail` helpers
  (`split_number`, `require_non_negative_finite`, `split_non_negative_number`,
  `nanoseconds_per`, `to_sim_time`, `parse_scaled_number`) chain the same type.
- `src/config/config.cpp`: `simsys::config::load` returns `std::expected<Scenario, ConfigError>`;
  the validator pipeline is a set of `check_*` functions returning
  `CheckResult = std::expected<void, ConfigError>`.
- `src/cli/main.cpp`: `run_validate` is the consumer (`if (!scenario) { scenario.error() ... }`).

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

Two uses in this code base, and why they were the right ones:

```cpp
// core.cppm, parse_rate: the last step, wrapping the double in a Rate, cannot fail
return detail::parse_scaled_number(number_text, text, detail::rate_prefixes)
    .transform([](double v) { return Rate{v}; });

// config.cpp, parse_duration_key: re-prefix the error with the key name and add a location
return parse_duration(value.Scalar()).transform_error([&](const ParseError& error) {
  return unit_error(file, value, key, error);
});
```

`transform` fits `parse_rate` because the final step is infallible, so there is no second error
to merge (`and_then` would only add noise). `transform_error` fits `parse_duration_key` because
the value needs no change and only the error type must change, `ParseError` -> `ConfigError`
(with file, line, column and a `duration: ` prefix). `parse_capacity` combines both:
`parse_byte_rate(text).transform([](ByteRate) {})` discards the value to get an
`expected<void, ParseError>`. Steps that can fail still use early return (below).

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

`std::expected<void, E>` for checks that produce no value (`config.cpp`):

```cpp
using CheckResult = std::expected<void, ConfigError>;
CheckResult check_version(const std::string& file, const YAML::Node& root) {
  ...
  if (version.as<int>(0) != 1) return std::unexpected(error_at(file, version, "version must be 1"));
  return {};   // {} = "fine"
}
```

`// Pattern: Validator pipeline` (`config.cpp`, `validate`): each check is a function
returning an `expected`; they run in order and the first failure is returned. Chains of
`if (auto checked = check_x(...); !checked) return checked;` forward a `CheckResult` unchanged
(`check_step_list`), while `validate` converts to `expected<Scenario, ConfigError>` with
`std::unexpected(checked.error())`.

The **single exception boundary** is `config::load`: yaml-cpp throws `YAML::Exception` on syntax
and shape errors, so `load` wraps `validate(file, YAML::LoadFile(file))` in one `try`/`catch` and
returns `std::unexpected(detail::error_at_mark(file, e.mark, e.msg))`. Callers never need `try`.

## Pitfalls

- **`*e` on an error is UB**, with no diagnostic. Always test (`if (!e) ...`) first; note that
  the code above dereferences only after the check (`*split`, `*nodes`).
- **`value()` throws** `std::bad_expected_access<E>` on an error. That is a hidden exception
  path; prefer an explicit check, or `value_or` when a default is meaningful.
- **`E` must be copyable or movable**: `std::unexpected(split.error())` copies the error. Fine
  for two strings; take it by move if an error ever gets heavy.
- **`auto operator<=>` on `double` members deduces `std::partial_ordering`** (`Rate`, `ByteRate`
  in `core.cppm`): NaN is unordered, so `a < b` and `a >= b` can both be false. Instead of
  guarding every comparison, the parsers reject NaN and infinity up front
  (`require_non_negative_finite`, and a second finite check after scaling in
  `parse_scaled_number`), so no `Rate` holding NaN can exist.
