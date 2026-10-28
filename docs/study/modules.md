# C++20 named modules

## Used in

| Unit | File | Kind |
|---|---|---|
| `simsys.core` | `src/core/core.cppm` | interface only |
| `simsys.config` | `src/config/config.cppm` | interface (`export module simsys.config;`) |
| `simsys.config` | `src/config/config.cpp` | implementation unit (`module simsys.config;`) |
| importers | `src/cli/main.cpp`, `tests/unit/test_units.cpp`, `tests/unit/test_config.cpp` | `import simsys.core;` / `import simsys.config;` |

`config.cppm` itself imports `simsys.core` (line 31) for `SimTime`; `config.cpp` imports it again
because an implementation unit implicitly imports its own interface, but the names the
interface imported are not thereby visible to it.

## What

```cpp
module;                       // global module fragment starts
#include <expected>
export module simsys.config;  // module purview starts; this file is the interface unit
import simsys.core;           // use another module's exported names
export namespace simsys::config { struct ConfigError { /* ... */ }; }
```

- `export module X;` names the module and begins its **purview** (everything after it).
- `export` on a declaration (or on a whole `namespace` block) makes it visible to importers.
- `import X;` brings in X's exported names. There is no text to re-parse: the compiler reads a
  compiled **BMI** (binary module interface; GCC calls it a `.gcm` file in `gcm.cache/`).
- Names in the purview that are not exported still exist, but importers cannot name them.
  `core.cppm` uses this on purpose: `namespace simsys::detail { ... }` (line 66) sits *between*
  two `export namespace simsys { ... }` blocks, so `Scale`, `split_number`, `fail` ... are
  invisible to importers. It cannot be nested inside an export block because everything
  inside `export namespace` is exported.

## Why it replaces headers

- **Textual inclusion** pastes the header into every includer and re-parses it each time.
  A module interface is compiled once into a BMI that importers load.
- **No include guards**: a module is imported once by definition.
- **No macro leakage**: macros defined inside a module do not escape, and macros in the importer
  do not change the module's meaning.
- **Real encapsulation**: non-exported names (like `simsys::detail`) cannot be reached, so they
  cannot become accidental API. Headers cannot hide anything.
- **Faster rebuilds**: with the interface/implementation split below, editing function bodies
  does not recompile importers.

## The global module fragment

Everything between `module;` and `export module X;` is the *global module fragment*: only
preprocessor directives are allowed, and the declarations that `#include`s bring in belong to the
**global module**, not to X. `core.cppm` lines 8-26 explain why std headers go there: an
`#include <vector>` inside the purview is legal but would attach `std::vector` to
`simsys.core` (a distinct entity that no longer belongs to the shared global module), which is
wrong for library types that every other translation unit also uses.

## Interface vs implementation units

| | `export module X;` | `module X;` |
|---|---|---|
| role | the interface: what importers see | definitions behind it |
| may `export` | yes | no |
| importers depend on it | yes (BMI) | no |
| sees the interface's own declarations | n/a | yes (implicit import of the interface) |

`config.cppm` declares `load(const std::string&)`; `config.cpp` defines it. The comment
`// Pattern: Interface / implementation unit split` in `config.cppm` names this. Because
importers read only the BMI built from the interface, editing `config.cpp` (e.g. adding a
validator) rebuilds that one file; `main.cpp` and the tests are untouched. `config.cpp` also owns
the heavy includes (`<yaml-cpp/yaml.h>`, `<format>`), which the next section makes mandatory.

## GCC 14.2 limitation (verified 2026-10-28)

An interface unit that `import`s another module must not pull `<format>` into its global module
fragment, directly or via `<chrono>`, `<filesystem>` or `<print>`. Otherwise a consumer that
reads the BMI fails with `failed to read compiled module cluster ...: Bad file data` and
`failed to load pendings for 'std::formatter'` (a GCC 14.2 bug). Rule: keep interface units
lean; heavy headers go in the implementation unit. That is why `simsys::config::load` takes
`const std::string&` and not `std::filesystem::path` (`main.cpp` still passes a `path`; it
converts implicitly).

Minimal reproduction (`g++-14 -std=c++23 -fmodules-ts -x c++ -c` for each file in order):

```cpp
// a.cppm                          // b.cppm                          // use.cpp
module;                            module;                            #include <format>
#include <chrono>                  #include <filesystem>              #include <string>
export module a;                   export module b;                   import b;
export using Tick =                import a;                          int main() {
  std::chrono::nanoseconds;        export Tick f(const std::string&);   return (int)f("x").count(); }
```

Compiling `use.cpp` fails with the errors above. The fix: `b.cppm` keeps only
`#include <string>`; `#include <filesystem>` and the definition of `f` move to `b.cpp`
(`module;  #include <filesystem>  module b;`). Then everything compiles. (In this reproduction
the failure appears when the *importer* includes `<format>`; the project hit it the same way,
in the test and CLI translation units.)

## Rules of thumb used here

- **One module per layer** (`simsys.core`, `simsys.config`), named `simsys.<layer>`.
- In a **non-module file**, `#include` first and `import` last. GCC 14 reports redefinition
  errors in the other order (see the comments above the imports in `main.cpp` and the tests).
- **CMake 3.28** (`CMakeLists.txt`): interface units go in `FILE_SET CXX_MODULES`,
  implementation units are plain `PRIVATE` sources, and `CMAKE_CXX_SCAN_FOR_MODULES ON` lets
  the build discover `export module` / `import` edges and order compilation (Ninja required).
- Std headers always in the global module fragment, never in the purview.

## Toolchain status (2026-10)

- **GCC 14 + CMake 3.28 + Ninja** works; it is the only supported combination.
- **Clang 18 cannot build the project**: with libstdc++, `__cpp_concepts` is 201907L, which
  hides `std::expected` and `std::generator`; libc++ 18 has no `<generator>`. Clang is used
  only for clang-format.
- **`import std;`** (the whole standard library as a module) needs GCC 15 and CMake 3.30, so
  the code still `#include`s std headers.
