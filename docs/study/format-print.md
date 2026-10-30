# C++20 `std::format`, C++23 `std::print` / `std::println`

## Used in

| Construct | Where (file: function) |
|---|---|
| `std::format` | `src/config/detail.cpp`: `Site::error` (first use, commented), `empty_message`; every loader message in the config units (`header.cpp`, `nodes.cpp`, ...); `src/config/config.cpp`: `describe` (a `ConfigError` as `file:line:column: error: message`); `src/session/session.cpp`: `engine_refusal` (the graph builder's and the engine's refusals as `file: error: message`); `src/cli/main.cpp`: the option refusals, from `unparsable_duration` on |
| `std::println(stderr, ...)` | `src/cli/main.cpp`: `print_error` (first use, commented) |
| `std::print` / `std::println` to stdout | `src/cli/main.cpp`: `print_report`, `run_validate`, `run_explain`, `run_estimate`, `print_samples` (`simsys curve`), `run_sweep_command` |
| `std::to_chars` instead of a format precision | `simsys::to_chars_text` (`src/core/core.cppm`), the one place a double becomes text, called by `with_significant_digits` (`src/config/workload.cpp`), by `significant` (`src/core/core.cppm`), by `format_number` (`src/report/report.cpp`), by `duration_text` (`format_duration`'s body), `fmt_count` and `percent` (`src/report/shared.cppm`) and so by every renderer of `report.cpp` and `explain.cpp`, and by the sweep's columns (`src/sweep/sweep.cpp`) |
| no `<format>` in interface units that import | `src/model/model.cppm`, `graph.cppm`, `behaviours.cppm`, `fluid.cppm`, `config.cppm`, `session.cppm`, ... (each says so in its global module fragment) |

## What

`std::format(fmt, args...)` returns a `std::string`. `{}` is replaced by the next
argument, and an optional spec after a colon controls fill, alignment, sign, width,
precision and presentation: `{:>8}`, `{:+}`, `{:.3f}`, `{:#x}`. The format string is
checked at **compile time**: a `{}` count that does not match the arguments, or a spec
the argument type does not support, is a compile error (P2216R3, applied to C++20 as a
defect report). Formatting does not depend on the locale unless you ask for it with `L`.

`std::print(fmt, args...)` (C++23) writes the result to `stdout` without building a
string first, and `std::println` adds the newline. Both take an optional `FILE*`:

```cpp
// config.cpp, config::describe
std::string describe(const ConfigError& error) {
  if (error.line == 0) return std::format("{}: error: {}", error.file, error.message);
  return std::format("{}:{}:{}: error: {}", error.file, error.line, error.column, error.message);
}
// main.cpp
void print_error(const std::string& message) { std::println(stderr, "{}", message); }
```

## The pre-C++20 way

- `printf("%s:%d:%d: error: %s\n", file.c_str(), line, column, msg.c_str())`: the
  format and the arguments are checked only by compiler warnings, `%d` against a `size_t`
  is undefined behaviour, and a `std::string` needs `.c_str()`.
- `std::ostringstream os; os << file << ':' << line << ...; os.str()`: type safe, but
  verbose, and manipulators (`std::setprecision`, `std::fixed`) stay set on the stream
  long after the line that set them.
- `snprintf` into a fixed buffer, then a check for truncation.

`std::format` keeps printf's compact format strings and adds iostream's type safety. The
library grew out of {fmt}.

## Why it fits here

The loader's job is messages: `file:line:column: error: node 'api': key 'capactiy' is not
valid for type 'service'`. Each message is one `std::format` call with the site prefix
(`Site::error`), and editors can jump to the location because it uses the compiler's
`file:line:column` layout. `main.cpp` prints them with `std::println(stderr, ...)`. Reports
are built as strings by `simsys.report` and written with one `std::print`.

## The two GCC 14.2 defects (and how this repo works around them)

**1. "Bad file data" when an importing interface includes `<format>`.** `CLAUDE.md` §3:

> Module interface units stay lean: no <chrono>/<filesystem>/<format>/<print> in the global
> module fragment of an interface that imports another module (GCC 14.2 'Bad file data'
> bug, verified 2026-10-28). Heavy headers go in the implementation unit.

The failure and a three-file reproduction are in [modules.md](modules.md). Its consequences
here: `<format>` is included only in implementation units (the config units, `session.cpp`) and
plain translation units (`main.cpp`); `config::load` takes `const std::string&`, not a
`std::filesystem::path`; interfaces compare `SimTime` through `.count()` because
`<chrono>`'s operators are not visible without the header.

**2. A width or precision in a format spec throws at run time.** In a translation unit that
imports a module whose global module fragment includes `<format>`, directly or through
`<chrono>` (as `simsys.core` does), `std::format("{:.15g}", x)` compiles but throws
`std::format_error: invalid width or precision in format-spec`. A probe with GCC 14.2.0
(a module that only includes `<chrono>`, one importer) gave:

| spec | importing TU | same TU without the import |
|---|---|---|
| `{}` (any type), `{:<}`, `{:+}`, `{:#x}` | works | works |
| `{:>6}`, `{:6}` (width) | **throws** | works |
| `{:.3f}`, `{:.15g}`, `{:.2}` (precision) | **throws** | works |

So any number inside a spec, width or precision, is unsafe; only `{}`, align without a
width, sign and `#` survive. Columns are padded by hand (`padded` in `shared.cppm`, and
`report::align_columns`, which `simsys sweep` reuses), and numbers go through `std::to_chars`
(C++17), which does not use `<format>`. One function in `simsys.core` wraps it for every
module:

```cpp
// core.cppm, to_chars_text (abridged): neither argument, the shortest text that reads back to
// the same double; a format alone, the shortest in that notation; both, rounded to `precision`
std::array<char, 512> buffer{};
...
written = std::to_chars(first, last, value, format.value_or(std::chars_format::general), *precision);
...
return std::string{first, written.ptr};

// workload.cpp, with_significant_digits: what "{:.15g}" was meant to do
return to_chars_text(value, std::chars_format::general, 15);
```

`simsys.report` formats every number this way (`significant`, `format_number`, `format_duration`,
`percent`, the JSON and CSV writers). `main.cpp` formats only strings and integers with plain
`{}`. Like `printf("%.15g")`, `chars_format::general` with a precision drops trailing zeros, so
a share sum of 1.000002 prints as `1.000002`, not `1.00000200000000`. (Five copies of such a
wrapper had grown before `to_chars_text` replaced them, its comment says.)

## Other pitfalls

- **The format string must be a constant expression.** `std::format(message)` with a
  run-time `std::string` does not compile. Use `std::format("{}", message)`, or
  `std::vformat` when the pattern itself is data.
- **A literal brace** in a format string must be doubled, `{{` and `}}`. `report.cpp`
  builds its JSON by concatenation (`json_object`, `json_field`), so it never needs to.
  Column alignment there is also by hand (`padded`, in `shared.cppm`), which avoids the width
  defect.
- **`std::print` and the C stdio buffer.** `std::print` writes through `FILE*`. Mixing it
  with `std::cout` on the same stream relies on the default `sync_with_stdio(true)`, so
  don't turn that off.

## Further reading

- cppreference: [`std::format`](https://en.cppreference.com/w/cpp/utility/format/format),
  [standard format specification](https://en.cppreference.com/w/cpp/utility/format/spec),
  [`std::print`](https://en.cppreference.com/w/cpp/io/print),
  [`std::println`](https://en.cppreference.com/w/cpp/io/println),
  [`std::to_chars`](https://en.cppreference.com/w/cpp/utility/to_chars).
- P0645R10, *Text Formatting* (C++20 `std::format`).
- P2216R3, *std::format improvements* (compile-time format string checks).
- P2093R14, *Formatted output* (C++23 `std::print`, `std::println`).
- P0067R5, *Elementary string conversions* (`std::to_chars`, `std::from_chars`).
- The {fmt} library by Victor Zverovich, where the design was proven.
