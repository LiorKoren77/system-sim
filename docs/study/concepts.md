# C++20 concepts and `requires`

## Used in

| Concept or constraint | File | Kind |
|---|---|---|
| `NodeBehaviour<B>` | `src/engine/behaviours.cppm` | requires-expression: `route` and `response` with exact result types |
| `RequestSource<S>` | `src/engine/behaviours.cppm` | requires-expression: `offer`, `response`, `ops` |
| `ResponseLookup<F>` | `src/engine/behaviours.cppm` | built from the standard `std::invocable` and `std::convertible_to` |
| `StepObserver<F>` | `src/engine/engine.cppm` | an alias of `std::invocable<F, const StepRecord&>` |
| `ResponseLookup auto&& downstream` | `ServiceBehaviour::response`, `ClientBehaviour::response` | constrained placeholder parameter |
| `StepObserver auto&& observer` | `engine::Simulation::run` | constrained placeholder parameter |
| `template <std::invocable<const YamlElement&> Reader>` | `src/config/config.cpp`, `collect_each` | type-constraint in the template head |
| `static_assert(NodeBehaviour<ServiceBehaviour>)` ... | bottom of `behaviours.cppm`, top of `tests/unit/test_behaviours.cpp` | the contract checked at build time |

## What

A **concept** is a named compile-time predicate on types. A **requires-expression** lists
operations that must compile; each `{ expr } -> C;` line (a *compound requirement*) also
checks the expression's type against another concept. From `behaviours.cppm`:

```cpp
template <class B>
concept NodeBehaviour = requires(const B b, const flow::Flow& in, flow::LatencyEstimate own) {
  { b.route(in) } -> std::same_as<Routed>;
  {
    b.response(std::string_view{}, own,
               [](std::size_t, std::string_view) { return flow::LatencyEstimate{}; })
  } -> std::same_as<flow::LatencyEstimate>;
};
```

The parameters `b`, `in`, `own` are never created; they only give the expressions something
to name. Concepts can also be composed from standard ones with `&&`:

```cpp
template <class F>
concept ResponseLookup = std::invocable<F, std::size_t, std::string_view> &&
    std::convertible_to<std::invoke_result_t<F, std::size_t, std::string_view>,
                        flow::LatencyEstimate>;
```

Four ways to apply one, all used or named in this repo:

| Form | Example |
|---|---|
| type-constraint in the template head | `template <std::invocable<const YamlElement&> Reader>` (`collect_each`) |
| `requires` clause | `template <class F> requires StepObserver<F> void run(RunOptions, F&&)` (the long form that `run`'s comment shows) |
| constrained placeholder (abbreviated template) | `void run(RunOptions options, StepObserver auto&& observer)` |
| boolean test | `static_assert(!NodeBehaviour<NoResponse>)` |

## The pre-C++20 way

- **SFINAE**: `template <class F, class = std::enable_if_t<std::is_invocable_v<F, const StepRecord&>>>`.
  It works, but the requirement hides in a defaulted template argument, and a failure
  reads "no matching function" followed by every candidate that did not fit.
- **Detection idiom** (`std::void_t<decltype(std::declval<B>().route(...))>`) to ask "does
  this type have a member": a dozen lines of traits per member.
- **An abstract base class** (`struct NodeBehaviour { virtual Routed route(const Flow&) const = 0; ... };`).
  That states the contract, but it forces virtual calls, heap ownership
  (`std::unique_ptr<NodeBehaviour>`) and inheritance on every behaviour.
- **Nothing**: an unconstrained `template <class F>`. A wrong argument fails deep inside
  the body, in a 30-line instantiation backtrace.

## Why it fits here

The engine needs exactly two things from a node behaviour: route a flow, and compose a
response time. `NodeBehaviour` says that in code, where the spec §5 sketch said it in a
comment. Dispatch is static (the behaviours live in a `std::variant`, see
[variant-visit.md](variant-visit.md)), so the concept is the contract and the variant is
the list of types that meet it. A behaviour that drifts from the contract breaks the
build of `behaviours.cppm` at its own `static_assert`, not an engine file three modules
away.

`ResponseLookup` and `StepObserver` constrain callables. The engine passes a lambda over
its response tables (`Simulation::backward_pass`); the tests pass stubs; `main.cpp`
passes an observer lambda that feeds a `report::Summary` and the CSV writer. A wrong
lambda signature is now reported as "constraints not satisfied" for the named concept.

## Pitfalls met while building

- **A concept checks signatures, not behaviour.** `NodeBehaviour` accepts a `route` that
  returns an empty `Routed` and a `response` that returns zero. The only thing the
  `NoResponse` test proves is the negative case:
  ```cpp
  struct NoResponse { simsys::behaviours::Routed route(const Flow&) const; };
  static_assert(!simsys::behaviours::NodeBehaviour<NoResponse>);
  ```
  It does not need a definition of `route`, because a concept never calls anything. What the
  behaviours *compute* is tested at run time by the `[behaviours]` test cases. The standard
  concepts are the same: `std::invocable` also has a semantic requirement (equality
  preserving) that no compiler checks.
- **A lambda inside a requires-expression.** `NodeBehaviour` needs *some* `ResponseLookup` to
  call `response` with, and a lambda is the shortest stand-in. C++20 allows lambdas in
  unevaluated operands (P0315R4), and GCC 14 accepts it in a module interface. Before
  that, a named stub type (`struct AnyLookup { LatencyEstimate operator()(std::size_t,
  std::string_view) const; };`) was the only option. It still is the portable fallback if
  another compiler rejects the lambda.
- **`same_as` versus `convertible_to` in a compound requirement.** `{ e } -> C<T>` checks
  `C<decltype((e)), T>`, the exact type of the expression including references. A lookup
  returning `const LatencyEstimate&` fails `std::same_as<LatencyEstimate>`, which is why
  `ResponseLookup` uses `std::convertible_to`. `NodeBehaviour` keeps `same_as` on purpose:
  a behaviour must return its own result by value.
- **`const B b` in the parameter list matters.** Because `b` is const, `route` and
  `response` must be `const` member functions. A behaviour that mutates itself while
  routing does not satisfy the concept, which is the intent: per-step state lives in the
  engine (`Simulation::Node`), not in the behaviour.
- **Templates in a module interface need their body there.** `ServiceBehaviour::response`
  is a template (its `auto&&` parameter makes it one), so it is defined in
  `behaviours.cppm`, not in an implementation unit; importers instantiate it.

## Further reading

- cppreference: [Constraints and concepts](https://en.cppreference.com/w/cpp/language/constraints),
  [requires expression](https://en.cppreference.com/w/cpp/language/requires),
  [Concepts library `<concepts>`](https://en.cppreference.com/w/cpp/concepts).
- P0734R0, *Wording Paper, C++ extensions for Concepts* (the language feature merged into C++20).
- P0898R3, *Standard Library Concepts* (`std::same_as`, `std::invocable`, ...).
- P1084R2, *Today's return-type-requirements are insufficient* (the `{ e } -> Concept` form).
- P1141R2, *Yet another approach for constrained declarations* (`Concept auto` parameters).
- P0315R4, *Wording for lambdas in unevaluated contexts*.
- P0557R1, B. Stroustrup, *Concepts: The Future of Generic Programming*.
