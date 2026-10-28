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
- Scenario YAML files get a README explaining the design and the mechanisms the model
  abstracts away.
- Commit messages explain *why*; the diff shows *what*.
- Module interface units stay lean: no <chrono>/<filesystem>/<format>/<print> in the global
  module fragment of an interface that imports another module (GCC 14.2 'Bad file data'
  bug, verified 2026-10-28). Heavy headers go in the implementation unit.

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
