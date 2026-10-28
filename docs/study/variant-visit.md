# `std::variant` and `std::visit` (C++17, with C++20 CTAD for aggregates)

## Used in

| Construct | Where (file: function) |
|---|---|
| `std::variant<SimTime, double>` | `src/model/model.cppm`: `SloSpec::max`, an SLO limit that is a duration or a ratio |
| building the right alternative | `src/config/config.cpp`: `read_slo_max` (`SloMaximum{*time}` or `SloMaximum{*number}`, `using SloMaximum = decltype(model::SloSpec::max)`) |
| `std::visit` + overload set | `src/report/report.cpp`: `limit_value`, `fmt_limit` |
| `using Behaviour = std::variant<ClientBehaviour, ServiceBehaviour>` | `src/engine/behaviours.cppm`, end of the exported namespace |
| `std::visit` + overload set | `src/engine/engine.cppm`: `Simulation::forward_pass` |
| `std::get_if` | `engine.cppm`: `Simulation::backward_pass`, `Simulation::client_responses` |
| the `Overloaded` helper | anonymous namespaces of `engine.cppm` and `report.cpp` |

## What

`std::variant<A, B>` holds exactly one value of type `A` or `B` and remembers which one
(`index()`, `holds_alternative<T>`). It lives inline (no heap allocation) and is
copyable when its alternatives are. `std::visit(f, v)` calls `f` with the alternative
currently held. With an **overload set** as `f`, each alternative gets its own lambda:

```cpp
template <class... Fs>
struct Overloaded : Fs... {        // inherits every lambda's call operator...
  using Fs::operator()...;         // ...and merges them into one overload set (C++17)
};

// report.cpp, limit_value
return std::visit(
    Overloaded{[](SimTime limit) { return seconds(limit); }, [](double limit) { return limit; }},
    max);
```

`Overloaded{l1, l2}` is aggregate initialization of a struct whose bases are the two
closure types. To name the type, the compiler must *deduce* `Overloaded<L1, L2>` from the
braces. That is where C++20 helps (next section).

## Why C++20 removed the deduction guide

C++17 class template argument deduction only looked at constructors, and an aggregate has
none, so the idiom needed a hand-written guide:

```cpp
template <class... Fs> Overloaded(Fs...) -> Overloaded<Fs...>;   // C++17 only
```

C++20 adds *aggregate deduction candidates* (P1816R0, P2082R1): the compiler deduces the
template arguments from the aggregate's elements in the braces. The two-line struct above
is all `engine.cppm` and `report.cpp` contain. The struct itself is still needed, because
the standard library offers no ready-made `overloaded`.

## The pre-C++17 way

- A **tagged union**: `enum Kind { duration, ratio }; union { SimTime t; double d; };`, with
  a `switch` on the tag. Nothing stops reading `d` when `t` is active, and non-trivial
  members need hand-written construction and destruction.
- Two fields and a flag (`double max; bool is_time;`). The repo's SLO comment states the
  bug this invites: "0.01" read as 0.01 seconds.
- An **abstract base class** with one subclass per case (next section).

## Variant versus abstract base: the Strategy discussion

`behaviours.cppm` says it at the point of use:

> Pattern: Strategy — the engine holds one Behaviour per node and dispatches with
> std::visit; a std::variant instead of an abstract base keeps every behaviour a plain
> value (no heap allocation, no virtual calls, copyable).

| | `std::variant<Client, Service>` (closed) | `std::unique_ptr<Behaviour>` + virtuals (open) |
|---|---|---|
| storage | inline in `Simulation::Node`, copied with it | one heap object per node |
| dispatch | `std::visit`, resolvable at compile time | indirect call through a vtable |
| contract | the `NodeBehaviour` concept ([concepts.md](concepts.md)) | the pure virtual functions |
| adding a type | edit the alias; every `visit` without a lambda for it stops compiling | add a subclass anywhere; no existing code changes |
| fits when | the set of types is known and owned here | plugins or third parties add types |

The node catalogue is fixed by the schema, and phase 2 appends `cache`, `message_queue`,
... to the alias, so a closed sum type is the right trade. The compiler error on a
missing case is a feature: it lists every place the new node type must be handled.
(Spec §5 planned "a small type-erased wrapper"; phase 1 found the variant enough.)

## Pitfalls met while building

- **`get_if` opts out of exhaustiveness.** `backward_pass` asks
  `std::get_if<ServiceBehaviour>(&node.behaviour)` and skips everything else. When phase 2
  adds a cache behaviour, `forward_pass`'s `visit` will fail to compile until it is
  handled, but `backward_pass` will silently skip caches. Use `visit` where every
  alternative must be considered, and `get_if` only where "the others do nothing" is
  really meant.
- **A generic catch-all hides missing cases.** Adding `[](const auto&) { ... }` to an
  `Overloaded` makes every `visit` compile again, which defeats the check above.
- **All lambdas must return the same type.** `forward_pass` returns `behaviours::Routed`
  from both, `limit_value` returns `double`. If one returned `long`, `visit` would not
  compile. C++20's `std::visit<R>` (P0655R1) fixes the result type explicitly.
- **Construct the alternative you mean.** C++20 tightened the converting constructor
  (P0608R3): narrowing conversions no longer count, so
  `std::variant<SimTime, double>{5}` (an `int`) does not compile. `int` to `double`
  narrows, and `SimTime`'s constructor from a count is explicit (checked with GCC 14).
  `read_slo_max` builds the alternative from a value of exactly that type.
- **Default construction picks the first alternative.** `SloSpec::max{}` is `SimTime{0}`,
  a zero duration. The loader always sets it; a test that builds an `SloSpec` in code and
  forgets `max` gets "at most 0 s", not an error.
- **valueless_by_exception.** If constructing a new alternative throws during assignment, the
  variant can hold nothing. The types here do not throw on copy or move, so it never happens,
  but `visit` would throw `std::bad_variant_access` if it did.

## Further reading

- cppreference: [`std::variant`](https://en.cppreference.com/w/cpp/utility/variant),
  [`std::visit`](https://en.cppreference.com/w/cpp/utility/variant/visit) (its example is
  the `overloaded` idiom), [class template argument deduction](https://en.cppreference.com/w/cpp/language/class_template_argument_deduction).
- P0088R3, *Variant: a type-safe union for C++17*.
- P0195R2, *Pack expansions in using-declarations* (`using Fs::operator()...`).
- P1816R0 and P2082R1, *Class template argument deduction for aggregates* (C++20).
- P0608R3, *A sane variant converting constructor*; P0655R1, *visit<R>*.
- *Design Patterns* (Gamma et al.), Strategy; and Sean Parent, *Inheritance Is the Base
  Class of Evil* (value semantics over class hierarchies).
