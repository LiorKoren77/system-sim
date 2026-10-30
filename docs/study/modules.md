# C++20 named modules

## Used in

The library is 15 named modules, one per layer, each named `simsys.<layer>`. CMake lists the
interface units and partitions in `FILE_SET CXX_MODULES` (`CMakeLists.txt`). The CLI
(`src/cli/main.cpp`) and the test programs (`tests/**/*.cpp`) are plain translation units that
`import` them; the tests add one module of their own, `simsys.test_fixtures` (the last row).

| Module | Files | Kind | Imports |
|---|---|---|---|
| `simsys.core` | `src/core/core.cppm` | interface only | none |
| `simsys.workload` | `src/workload/workload.cppm` | interface only | core |
| `simsys.model` | `src/model/model.cppm`, `model.cpp` | interface + implementation unit (the fourth defect's workaround, below) | core, workload |
| `simsys.config` | `src/config/config.cppm`; `config.cpp`, `header.cpp`, `nodes.cpp`, `steps.cpp`, `workload.cpp`, `links.cpp`, `slos.cpp`, `overrides.cpp`, `detail.cpp`; `detail.hpp` | interface + nine implementation units (the loader, one unit per concern) sharing a header included in their purview (below) | core, model; the implementation units add defaults, fluid, workload |
| `simsys.queueing` | `src/queueing/queueing.cppm` | interface only | none |
| `simsys.flow` | `src/flow/flow.cppm` | interface only | core |
| `simsys.fluid` | `src/fluid/fluid.cppm` | interface only | core, flow, queueing |
| `simsys.caching` | `src/caching/caching.cppm` | interface only | none |
| `simsys.defaults` | `src/defaults/defaults.cppm` | interface only | core, fluid, model |
| `simsys.graph` | `src/engine/graph.cppm`, `graph.cpp` | interface + implementation unit (declarations in the interface, definitions in `graph.cpp`) | caching, core, defaults, fluid, model, queueing |
| `simsys.behaviours` | `src/engine/behaviours.cppm` | interface only | core, defaults, flow, fluid, graph, model, queueing, workload |
| `simsys.engine` | `src/engine/engine.cppm` | interface only | core, behaviours, defaults, flow, fluid, graph, model, queueing |
| `simsys.report` | `src/report/report.cppm`, `shared.cppm`, `report.cpp`, `explain.cpp` | interface + implementation partition `:shared` + two implementation units (the run's renderers; `simsys explain`) | core, engine, fluid, graph, model; the partition imports core, fluid; the implementation units add defaults, flow, queueing, workload |
| `simsys.session` | `src/session/session.cppm`, `session.cpp` | interface + implementation unit | config, engine, graph, model, report |
| `simsys.sweep` | `src/sweep/sweep.cppm`, `sweep.cpp` | interface + implementation unit | none in the interface; the implementation unit imports config, core, graph, model, report, session |
| `simsys.test_fixtures` (tests only) | `tests/support/fixtures.cppm`, `fixtures.cpp` | interface + implementation unit (library `simsys_test_support`: the builders several test files share) | core, engine, graph, model; the implementation unit adds queueing, report |

A module's name says nothing about its directory: `src/engine/` holds three modules (graph,
behaviours, engine). The import graph has no cycle (a module cannot import itself through
another; the build would not find an order), and no module reaches into another's `detail::`
(below).

An implementation unit implicitly imports its own interface, and with it every module the
interface imports ([module.import]/7). So `config.cpp` could use `SimTime` (from
`simsys.core`, which `config.cppm` imports) without an `import` of its own. It repeats the
imports anyway, as `report.cpp`, `session.cpp` and `sweep.cpp` do, to say what the file uses.
(Verified with GCC 14.2: an implementation unit `module b;` calls a function of module `a` that
only its interface `b.cppm` imports, and it compiles, links and runs.) The reverse also holds:
an implementation unit may import more than its interface, which keeps importers from depending
on what only the definitions need (`sweep.cpp` imports `simsys.session`; importers of
`simsys.sweep` do not).

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
  `core.cppm` uses this on purpose: its two `namespace simsys::detail { ... }` blocks (one
  holding `checked_literal`, which the time literals call, the other the parsers' helpers
  `Scale`, `split_number`, `fail`, ...) each sit *between* `export namespace simsys { ... }`
  blocks, so they are invisible to importers. A detail block cannot be nested inside an export
  block because everything inside `export namespace` is exported.

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
**global module**, not to X. The header comment of `core.cppm`, just above its `module;`
line, explains why std headers go there: an `#include <vector>` inside the purview is legal but
would attach `std::vector` to `simsys.core` (a distinct entity that no longer belongs to the
shared global module), which is wrong for library types that every other translation unit also
uses.

## Interface vs implementation units

| | `export module X;` | `module X;` |
|---|---|---|
| role | the interface: what importers see | definitions behind it |
| may `export` | yes | no |
| importers depend on it | yes (BMI) | no |
| sees the interface's own declarations | n/a | yes (implicit import of the interface) |
| sees the modules the interface imports | n/a | yes (they come with that implicit import) |

`config.cppm` declares `load(path)` and `load(path, overrides)`; `config.cpp` defines them.
The header comment of `config.cppm` calls this split a *compilation firewall* ("interface
declares, implementation unit defines") and says why it carries no `Pattern:` tag: it is a
build technique, not a GoF design pattern. Because importers read only the BMI built from the
interface, editing `config.cpp` (e.g. adding a validator) leaves the BMI unchanged, and with
CMake 3.31 it rebuilds that one file; `main.cpp` and the tests are untouched. With CMake 3.28.3
(the system CMake here; 3.28 is the minimum) the build is not that selective: its collation step
rewrites every unit's `.modmap` file after any edit, the compiles record that file as an input, so
every importer recompiles anyway (a one-line edit in a `Graph` member body recompiled 61 units,
measured in phase 3a; 3.31 recompiled `graph.cpp` alone). The split still pays there, since each
importer reads a smaller BMI (`simsys.graph`'s went from 16.9 MB to 8.0 MB). The implementation
units also own the heavy includes (`<yaml-cpp/yaml.h>`, `<format>`), which the next section makes
mandatory.

## GCC 14.2 limitation (verified 2026-10-28)

An interface unit that `import`s another module must not pull `<format>` into its global module
fragment, directly or via `<chrono>`, `<filesystem>` or `<print>`. Otherwise a consumer that
reads the BMI fails with `failed to read compiled module cluster ...: Bad file data` and
`failed to load pendings for 'std::formatter'` (a GCC 14.2 bug). Rule: keep interface units
lean; heavy headers go in the implementation unit. That is why `simsys::config::load` and
`simsys::session::load_graph` take `const std::string&` and not `std::filesystem::path`
(`main.cpp` holds a `path` and passes `file.string()`; an implicit conversion would work only
on POSIX, where a path's native string is `std::string`).

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

## Module linkage: a named `detail` namespace instead of an anonymous one

In a header, helpers that no other file should see go in an anonymous namespace (internal
linkage). In a module interface that is often impossible. The call-graph search of `simsys.graph`
keeps its vertex type `detail::Call` and the functions over it (`detail::find_async_hops`, ...)
in a plain, unexported `namespace detail`, and its comment in `graph.cpp` says why (the code was
written in the interface unit and moved unchanged): GCC 14 rejects a
`std::map` or `std::vector` of an internal-linkage type in a module interface unit
("references internal linkage entity"). The standard calls this an *exposure*: an entity that
importers may use (an exported function's inline body, a template they instantiate) must not
name a translation-unit-local one, because an importer has no way to reach it.

A name in an unexported namespace of the purview has **module linkage**, a linkage C++20 added
between internal and external. Every unit of the same module can use it; no importer can name
it. That is the encapsulation an anonymous namespace gave a `.cpp` file, widened to the module.
`behaviours.cppm`, `graph.cppm`, `graph.cpp`, `queueing.cppm`, `workload.cppm`, `model.cpp` and
the config units (through `detail.hpp`) all have such `detail` blocks. In an implementation unit
module linkage is wider than a helper needs: a function there that is not in an anonymous
namespace has module linkage, so every other unit of the module could call it and the linker
sees its name. So the implementation units (`report.cpp`, `explain.cpp`, `session.cpp`, and the
seven config units for every helper `detail.hpp` does not declare) put their own helpers in an
anonymous namespace (internal linkage): what the units share is declared in one place, and
nothing else leaks between them. (`nm -C` on the Release objects of `src/config/` counted 190
external `detail` functions in the seven units before, 15 after: the entry points `detail.hpp`
declares.)

## Partitions, and how the large modules are laid out

A module may be cut into **partitions**, units named `M:part`. Two kinds:

```cpp
export module simsys.report:types;  // interface partition: part of M's interface; the primary
                                    // interface must `export import :types;` it
module simsys.report:shared;        // implementation partition: no `export`; a piece of the
                                    // module that only its own units can import
import :shared;                     // in any unit of simsys.report: the module name is implied
```

An implementation partition is the module's private header, done right. It is compiled once into
a BMI (CMake lists it in `FILE_SET CXX_MODULES` with the interfaces, because others import it),
nothing outside the module can import it, and its declarations are attached to the module with
**module linkage**: every unit of the module that imports it sees them, no importer of the module
does, and no `export` is needed (an implementation partition cannot export anything). A header
included by two `.cpp` files gives none of that: it is parsed twice, and what it declares belongs
to the global module, callable by anything that includes it.

The three large modules show three layouts.

- **`simsys.graph`: interface + one implementation unit.** `graph.cppm` keeps the types, the
  declarations with their contracts and the inline one-liners; `graph.cpp` (`module
  simsys.graph;`) holds every definition. Nothing is shared between implementation units, so
  nothing more is needed.
- **`simsys.report`: interface + an implementation partition + two implementation units.**
  `report.cpp` (summaries, SLO verdicts, the table, JSON and CSV) and `explain.cpp` (`simsys
  explain`) both `import :shared;`. `shared.cppm` defines what both use: the number formatters
  (`duration_text`, the body of the exported `format_duration(double)`, which `report.cpp`'s
  definition returns), the run's clock, the zone names and the aligned text table. It imports
  `simsys.core` and `simsys.fluid` but not the module's own interface (next paragraph), so it
  cannot see `format_duration(double)`; that is why the body lives in the partition and the
  exported function forwards to it, not the other way round.
- **`simsys.config`: interface + nine implementation units + a header in the purview.** The
  loader is cut by concern: `config.cpp` (`load`, the pipeline), `header.cpp`, `nodes.cpp`,
  `steps.cpp`, `workload.cpp`, `links.cpp`, `slos.cpp`, `overrides.cpp`, and `detail.cpp`, which
  defines the shared readers. What they share (the vocabulary types, the key tables, the
  templates and the cross-unit declarations) is in `detail.hpp`, which each unit `#include`s
  *after* its imports, inside the module purview, so the declarations attach to
  `simsys.config` and can name `ConfigError`. Each unit's own helpers sit in an anonymous
  namespace.

**The purview header is an exception, and a departure from the standard.** A definition
attached to a named module (a struct, an `inline constexpr` table, a template) must be defined
in one unit only ([basic.def.odr]); the leave to repeat a definition in every unit that includes
it is for the global module, which is what makes ordinary headers legal. `detail.hpp` repeats its
structs, tables and templates in the nine units that include it: ill-formed, no diagnostic
required. GCC 14.2 accepts it (it emits them in vague linkage, as it would a header's, and the
linker keeps one), and it is in the tree only because GCC 14.2 refused both partition designs:

1. A partition that imports the primary interface (for `ConfigError`), `module simsys.config:shared;
   import simsys.config;`, compiles, but every unit that then imports `:shared` fails with
   "cannot import module in its own purview". The standard allows a partition to
   import its primary interface (it forbids only a non-partition implementation unit of `M`
   from importing `M`, which it does implicitly anyway); Clang 18 accepts the same three files.
   This is also why `simsys.report:shared` imports no part of `simsys.report`.
2. With `ConfigError` moved into an interface partition, a partition whose global module fragment
   includes yaml-cpp and which imports `simsys.core` makes every importer fail with the first
   defect ("Bad file data", "failed to load pendings for 'std::formatter'"): yaml-cpp includes
   `<memory>`, `<sstream>` and `<ostream>`, each of which alone triggers it in C++23 mode.

The conforming design is an implementation partition `simsys.config:shared`, as in
`simsys.report`; it should replace the header once GCC builds it. Until then the header takes no
definition it does not need: a helper one unit uses stays in that unit.

## The other GCC 14.2 module defects met while building

`CLAUDE.md` §3 lists seven GCC 14.2 defects, each verified (the first six on 2026-10-28, the
seventh on 2026-10-30) and each with its workaround in the tree. The first is the "Bad file data" one above. The second, a format width
or precision that throws at run time, is in [format-print.md](format-print.md). The rest:

- **Third: an internal compiler error at -O2/-O3** (`ipa_comdats`) when `Graph::build`,
  `Simulation::create` and `step` are all inlined into `main()`. `simsys_warnings` adds
  `-fno-ipa-sra` (interprocedural scalar replacement of aggregates) to every target. The
  comment in `CMakeLists.txt` records the passes turned off one at a time to find it.
- **Fourth: undefined references in Release links.** A unit that imports `simsys.model` calls
  compiler-generated special members of standard templates over model types (the move
  constructor of `std::optional<Workload>`, the destructor of the `NodeSpec::ops` map) out of
  line, without emitting them. At -O2/-O3 the module's own objects inline every call, so no
  object file defines them, and the link fails. The workaround is `src/model/model.cpp`, an
  implementation unit with no model logic. It is compiled with `-fno-inline` and calls each
  such member once (`detail::emit_special_members`), so `libsimsys.a` defines them. A new
  "undefined reference" of this kind gets one more explicit instantiation line there.
- **Fifth: a lambda handed to a range adaptor is defined twice.** A lambda passed to an
  adaptor such as `std::views::chunk_by` in a function defined in a module is emitted in more
  than one object file ("multiple definition" of the lambda's conversion to a function pointer
  at link time). The workaround is a named function instead of the lambda, `detail::same_op` in
  `graph.cpp` ([ranges.md](ranges.md) shows the call).
- **Sixth: running out of source locations.** Each import maps the imported module's whole
  line table into the importer's source-location space. The module units at the end of the
  import chain (from `simsys.behaviours` on) run out of it ("unable to represent further
  imported source locations") and the compiler then crashes while writing the module. The
  `simsys` library target compiles with `-flarge-source-files` (`CMakeLists.txt`; a little
  compile time and memory). The CLI and the tests import modules but export none, so they do
  not need it.
- **Seventh: an exported `inline` variable named by a default argument is never defined.**
  `piped(double rate, std::optional<ByteRate> bandwidth = one_gbit)` in a module interface: the
  default argument is evaluated in the caller's unit, which references `one_gbit` without
  emitting it (an inline variable is emitted where it is used, in every unit that uses it, and
  the linker keeps one). The interface's object file defines an inline variable only if it uses
  it itself, so nothing defines it and the link fails with an undefined reference, in Debug too.
  A direct use of the variable in the importer is fine. The workaround is to drop `inline`:
  `export constexpr ByteRate one_gbit{125e6};` keeps external linkage (it is exported; a
  namespace-scope `const` that is not exported would be internal) and the interface's object
  file defines it once (`tests/support/fixtures.cppm`).

## Rules of thumb used here

- **One module per layer**, named `simsys.<layer>` (the table at the top).
- In a **non-module file**, `#include` first and `import` last. GCC 14 reports redefinition
  errors in the other order (see the comments above the imports in `main.cpp` and the tests).
- **CMake 3.28** (`CMakeLists.txt`): interface units go in `FILE_SET CXX_MODULES`,
  implementation units are plain `PRIVATE` sources, and `CMAKE_CXX_SCAN_FOR_MODULES ON` lets
  the build discover `export module` / `import` edges and order compilation (Ninja required).
- Std headers always in the global module fragment, never in the purview.
- **Interface-only modules trade rebuild time for simplicity.** Most modules here define
  everything in their interface, so editing a function body rebuilds every importer. The large
  ones are split: `config`, `graph`, `report`, `session` and `sweep` declare in the interface
  and define in implementation units (phase 3a moved `graph.cppm`'s definitions, about 3,200
  lines, into `graph.cpp`). With CMake 3.31 a body edit then recompiles one unit; with 3.28 every
  importer still recompiles, each reading a smaller BMI (above). Each such move is checked
  against the fourth and fifth defects, which depend on where code is instantiated.

## Toolchain status (2026-10)

- **GCC 14 + CMake 3.28 + Ninja** works; it is the only supported combination. CMake 3.31
  builds the same tree and rebuilds less after an edit (above).
- **Clang 18 cannot build the project**: with libstdc++, `__cpp_concepts` is 201907L, which
  hides `std::expected` and `std::generator`; libc++ 18 has no `<generator>`. Clang is used
  only for clang-format.
- **`import std;`** (the whole standard library as a module) needs GCC 15 and CMake 3.30, so
  the code still `#include`s std headers.
