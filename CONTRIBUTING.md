# system-sim — project conventions

This is an **educational project**: its owner is learning C++20/23 through it and will
teach both modern C++ and system design with it. Future students will read the code.
Every contribution, human or AI, follows these rules.

## 1. Explain every new C++20/23 construct on first use

The first time a C++20 or C++23 language or library feature appears in a translation
unit (module, source or test file), add an inline comment that says:

- what the construct is and which standard introduced it (`C++20` / `C++23`),
- what it replaces or improves over the pre-C++20 way,
- why it is the right tool here.

Keep it to 2–5 lines. Later uses in the same file need no comment. Example:

```cpp
// C++23 std::expected<T, E>: a value-or-error return type. Replaces the old choice
// between exceptions and out-parameters/error codes; the caller must inspect it,
// and errors carry structured data (here: file, line, column, message).
auto load(std::string_view path) -> std::expected<Scenario, ConfigError>;
```

Features to watch for: modules (`export module`, `import`), concepts and `requires`,
ranges and views, `ranges::to`, coroutines (`co_yield`, `std::generator`), `std::expected`,
`std::format` / `std::print`, deducing `this`, `<=>` and defaulted comparisons,
designated initializers, `consteval` / `constinit` / `constexpr` containers, `std::span`,
`std::string_view` improvements, `using enum`, `[[likely]]`, `std::jthread` /
`std::stop_token`, `std::latch` / `std::barrier`, `std::chrono` calendar types,
`std::mdspan`, `if consteval`, `auto(x)` decay copy, multidimensional `operator[]`,
`static operator()`, `std::unreachable`, `std::to_underlying`, `std::flat_map`.

A longer note per feature lives in `docs/study/<feature>.md`; the inline comment may
link to it.

## 2. Explain the system-design model in the code

Every node type, policy and formula that models something from *System Design
Interview* (Alex Xu, vol. 1) or from queueing theory carries explanatory documentation:

- a header comment on the behaviour/policy: what real component it models, which book
  chapter, what the model captures and what it deliberately ignores,
- an inline comment on each formula: its name (Little's law, Erlang C, Kingman,
  Che approximation, ...), the variables, and the assumption it rests on,
- an inline comment on each default taken from the back-of-the-envelope table saying
  where the number comes from.

Write for a reader who has read the book but has not studied queueing theory.

## 3. General

- C++23. GCC 14 is the only compiler that builds the project (Clang 18 lacks
  std::expected/std::generator with both standard libraries); Clang 18 is used for
  clang-format only. Named modules. No raw `new`, no macros,
  exceptions only for programmer errors.
- Tests are part of the teaching material: name them after the property they prove.
  Test names must not contain `[` or `]`: Catch2's ctest discovery reads those as tags and
  merges every later case into one failing entry (hit twice on 2026-10-28).
- Scenario YAML files get a README explaining the design and the mechanisms the model
  abstracts away.
- Commit messages explain *why*; the diff shows *what*.
- First GCC 14.2 defect: module interface units stay lean: no
  <chrono>/<filesystem>/<format>/<print> in the global module fragment of an interface
  that imports another module ('Bad file data' bug, verified 2026-10-28). Heavy headers go
  in the implementation unit.
- Second GCC 14.2 defect (2026-10-28): in a TU that imports a module whose global module
  fragment includes <format> (simsys.core does, through <chrono>), a std::format spec with
  any width or precision, such as `{:>6}` or `{:.15g}`, throws std::format_error at run
  time. Only `{}`, align without a width (`{:<}`), sign (`{:+}`) and `#` are safe. Use
  std::to_chars for numbers and pad by hand (docs/study/format-print.md has the table).
- Third GCC 14.2 defect (2026-10-28), -O2/-O3 only: an internal compiler error in
  `ipa_comdats` when `Graph::build` + `Simulation::create` + `step` are inlined into
  `main()`. `simsys_warnings` adds `-fno-ipa-sra` to every target; do not remove it.
- Fourth GCC 14.2 defect (2026-10-28), release builds only: a TU importing simsys.model
  calls compiler-generated special members of std templates over model types (e.g.
  `std::optional<Workload>`'s move constructor, the `NodeSpec::ops` map's destructor) out
  of line without emitting them, and at -O2/-O3 no module object file defines them either:
  undefined references at link time. Workaround: `src/model/model.cpp`, compiled with
  `-fno-inline`, calls each such member so libsimsys.a defines it; a new undefined
  reference of this kind gets one more `emit_special_members` line there.
- Fifth GCC 14.2 defect (2026-10-28): a lambda passed to a range adaptor such as
  `std::views::chunk_by` in a function defined in a module (a member function or not) is
  emitted in more than one object file ("multiple definition" of the lambda's conversion to a
  function pointer at link time). Workaround: a named function in `detail::` instead of the lambda
  (`graph.cpp`'s `detail::same_op`).
- Sixth GCC 14.2 defect (2026-10-28): each import maps the imported module's whole line table
  into the importer's source-location space, and the module units at the end of the import chain
  (simsys.behaviours on) run out of it: "unable to represent further imported source locations",
  then a crash in `write_location` while writing the module. Workaround: the `simsys` library
  target compiles with `-flarge-source-files` (CMakeLists.txt; a little compile time and memory);
  the tests and the CLI import modules but export none and do not need it. Do not remove it.
- Seventh GCC 14.2 defect (2026-10-30), Debug and Release: a default argument in a module
  interface that names an exported `inline` variable (`std::optional<ByteRate> bw = one_gbit`)
  is evaluated in the caller's TU, which references the variable without emitting it; the
  interface's object file defines an inline variable only if it uses it itself, so the link
  fails with an undefined reference. A direct use in the importer is fine. Workaround: an
  exported namespace-scope constant in a module interface is `constexpr` without `inline`: it
  keeps external linkage (it is exported) and the interface's object file defines it once
  (`tests/support/fixtures.cppm`).
- Eighth GCC 14.2 defect (2026-10-30): a standard header `#include`d AFTER an `import` is compiled
  a second time against the library internals the imported module's global module fragment
  already brought in, and clashes with them, even when the header itself is new to the importer.
  `#include <set>` after `import simsys.graph;` (whose fragment has `<map>`, `<functional>`, ...)
  fails with `bits/node_handle.h:67:56: error: template definition of non-template 'using
  std::_Node_handle_common<_Val, _NodeAlloc>::_AllocTraits = ...'`; after `import simsys.core;`
  with `ext/aligned_buffer.h:96:67: error: redeclaration of '... __aligned_buffer<_Tp>::_M_storage'`
  (the test-writer saw a `_Hashtable` conflict: same cause, another shared internal). Workaround:
  every `#include` before the first `import` of the file, the order every unit already follows;
  `<set>` there compiles and works (verified with tests/unit/test_session.cpp).

## 4. Clean code, named patterns, TDD

- **Clean code**: self-explanatory names; short functions that do one thing and delegate
  the next thing to another well-named function; no clever tricks without a comment.
- **Name every design pattern** where it is used (Factory, Registry, Strategy/Policy,
  Adapter, Decorator, Visitor, Builder, Type erasure, ...) in a comment at the point of use:
  `// Pattern: Strategy — the eviction policy is a compile-time parameter ...`. Students
  should be able to grep for `Pattern:`.
- **Test-driven**: tests are written before the implementation, in small batches; the
  implementation is the minimum that makes them pass; then refactor. Integration-level
  tests are added once the units they combine exist.
