# `std::variant` and `std::visit` (C++17, with C++20 CTAD for aggregates)

## Used in

| Construct | Where (file: function) |
|---|---|
| `std::variant<SimTime, double>` | `src/model/model.cppm`: `SloSpec::max`, an SLO limit that is a duration or a ratio |
| building the right alternative | `src/config/config.cpp`: `read_slo_max` (`SloMaximum{*time}` or `SloMaximum{*number}`, `using SloMaximum = decltype(model::SloSpec::max)`) |
| `std::visit` + overload set | `src/report/report.cpp`: `limit_value`, `fmt_limit` |
| `using Behaviour = std::variant<...>`, twelve alternatives | `src/engine/behaviours.cppm`, end of the exported namespace: `ClientBehaviour`, `ServiceBehaviour`, `LoadBalancerBehaviour`, `RateLimiterBehaviour`, `CacheBehaviour`, `SqlDbBehaviour`, `CdnBehaviour`, `BlobStoreBehaviour`, `MessageQueueBehaviour`, `WorkerPoolBehaviour`, `ExternalSinkBehaviour`, `KvClusterBehaviour` |
| `std::visit` + overload set (a client lambda and one generic lambda for every server) | `src/engine/engine.cppm`: `Simulation::forward_pass`, `Simulation::backward_pass` |
| `std::visit` with one generic lambda and `if constexpr (requires ...)` | `engine.cppm`: `megabyte_service_of` |
| `std::get_if`, `std::holds_alternative` | `engine.cppm`: `Simulation::client_responses`, `is_write_at`, `failure_rate_of`, `report_machines`, `split_writes` |
| `using HitRatioPolicy = std::variant<FixedHitRatio, CheHitRatio>` + `std::visit` | `src/engine/graph.cppm`: `detail::hit_ratio_over_arcs` |
| the `Overloaded` helper | `simsys.core` (`src/core/core.cppm`), exported for `engine.cppm` and `report.cpp` |

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
is all `simsys.core` defines, once, for every module that visits a variant. The struct itself
is still needed, because the standard library offers no ready-made `overloaded`.

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

| | `std::variant<Client, Service, ...>` (closed) | `std::unique_ptr<Behaviour>` + virtuals (open) |
|---|---|---|
| storage | inline in `Simulation::Node`, copied with it | one heap object per node |
| dispatch | `std::visit`, resolvable at compile time | indirect call through a vtable |
| contract | the `NodeBehaviour` concept ([concepts.md](concepts.md)) | the pure virtual functions |
| adding a type | edit the alias; every `visit` without a lambda for it stops compiling | add a subclass anywhere; no existing code changes |
| fits when | the set of types is known and owned here | plugins or third parties add types |

The node catalogue is fixed by the schema, so a closed sum type is the right trade. Phase 1
had two alternatives; phases 2a to 2c appended ten, from `LoadBalancerBehaviour` to
`KvClusterBehaviour`, and each time the compiler listed the places the new type had to be
handled. (Spec §5 planned "a small type-erased wrapper"; phase 1 found the variant enough.)

**How the engine keeps twelve alternatives exhaustive.** Twelve lambdas per `visit` would be
noise: eleven of the twelve are servers that route, cost, respond and deliver the same way at
the call site. So `forward_pass` and `backward_pass` pass two lambdas, one for
`ClientBehaviour` and one generic `[&](auto& server)` for the rest. Overload resolution prefers
the non-template client lambda for a client, so the generic one sees only servers. That generic
lambda is a catch-all, and a catch-all hides a missing case (see the pitfalls below). It is safe
here because of the `NodeBehaviour` concept ([concepts.md](concepts.md)): every server
alternative is checked by a `static_assert(NodeBehaviour<...>)` at the bottom of
`behaviours.cppm`, so a new behaviour that lacks `route`, `cost`, `response` or `delivered`
fails there, by name, and the generic lambda can call them on any server. Exhaustiveness moves
from the lambdas to the concept.

**And the `NodeType` switch beside it.** The variant is not the only per-type list. The engine
picks each node's behaviour in `Simulation::make_node`, a `switch` over the scenario's
`model::NodeType` enum. Its last case once read `default:` and made a `ServiceBehaviour`, and a
separate hand-kept function, `simulated()`, listed the types the engine runs. Both bypassed the
compiler's check: with a `default:`, `-Wswitch` (an error here, under `-Werror`) no longer
warns about an enumerator without a case, so a new node type would silently have run as a
service. The whole-of-phase-2 review changed both. `make_node` names every enumerator, with
`case service:` explicit, and the four phase-3 types end in `std::unreachable()`, with a comment
that `Simulation::create` refused them before `make_node` runs
([small-features.md](small-features.md#stdunreachable)). `simulated()` now reads a column of
`model::node_type_table`, the one table that names every type, so adding a type is one row
that says whether it runs. The lesson is the same as for `visit`: a fallback branch turns a
compile error you want into a run-time behaviour you did not choose.

## Pitfalls met while building

- **`get_if` opts out of exhaustiveness.** In phase 1 `backward_pass` asked
  `std::get_if<ServiceBehaviour>(&node.behaviour)` and skipped everything else, so a cache
  behaviour added later would have been skipped silently, where `forward_pass`'s `visit`
  failed to compile until it handled it. `backward_pass` is a `visit` now. Use
  `visit` where every alternative must be considered, and `get_if` only where "the others do
  nothing" is really meant: `is_write_at` (only a database has writes), `failure_rate_of` (only
  an external sink fails), `report_machines` (only a kv cluster has machines). The comment on
  `make_node` calls these type tests "the opposite of Strategy", kept where the alternative would
  be a member every other behaviour implements to do nothing.
- **A generic catch-all hides missing cases.** Adding `[](const auto&) { ... }` to an
  `Overloaded` makes every `visit` compile again, which defeats the check above.
- **All lambdas must return the same type.** `forward_pass` returns `behaviours::Routed`
  from both, `limit_value` returns `double`, and `megabyte_service_of` states its
  `-> const behaviours::MegabyteService*` so that both branches of its `if constexpr` agree. If one returned `long`, `visit` would not
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
