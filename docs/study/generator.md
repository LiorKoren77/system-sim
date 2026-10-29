# C++20 coroutines and C++23 `std::generator`

## Used in

| Construct | File | Function |
|---|---|---|
| `std::generator<Sample>` returned by a coroutine | `src/workload/workload.cppm` | `workload::samples(curve, step, until)`: the deterministic curve |
| the same, with state kept in the coroutine frame | `src/workload/workload.cppm` | `workload::samples(curve, step, until, noise, seed, noise_step)`: a `NoiseProcess` lives in the frame and is ticked between yields |
| a plain function returning a generator | `src/workload/workload.cppm` | the five-argument `samples` overload forwards to the six-argument one; it has no `co_yield`, so it is not a coroutine |
| consuming a generator | `src/cli/main.cpp` | `curve_samples` (returns `std::expected<std::generator<Sample>, std::string>`), `print_samples` (a range-`for` that prints one CSV row per sample) |
| the iterator by hand | `tests/unit/test_workload.cpp` | "samples carries the time of each sample" |

## What

A **coroutine** (C++20) is a function that can suspend and be resumed later. A function
becomes one by containing `co_await`, `co_yield` or `co_return`; its parameters and
locals then live in a heap-allocated *coroutine frame* instead of on the stack, so they
survive every suspension. C++20 shipped only the language machinery (promise types,
awaitables, `std::coroutine_handle`); C++23 added the first ready-made library type,
`std::generator<T>` in `<generator>`.

A `std::generator<T>` is a lazy, single-pass `input_range`. Each `co_yield v` hands one
value to the consumer and suspends; the consumer's `++it` resumes the body until the next
`co_yield`; falling off the end of the body ends the range. From `workload.cppm`:

```cpp
std::generator<Sample> samples(RateCurve curve, SimTime step, SimTime until) {
  if (step.count() <= 0) throw std::invalid_argument{"sample step must be positive"};
  for (std::int64_t t = 0; t < until.count();) {
    const SimTime at{t};
    co_yield Sample{.t = at, .rate = curve.rate_at(at)};
    if (!detail::advance_within(t, step.count(), until.count())) break;
  }
}
```

The consumer is an ordinary range-`for` (`main.cpp`, `print_samples`):

```cpp
for (const auto& sample : samples) std::println("{},{}", ...);
```

## The pre-C++23 way

- **Fill a container up front**: `std::vector<Sample> samples(...)`. Simple, but a day at
  1 s steps is 86 400 samples stored before the first is printed, and the caller cannot
  stop early without the producer having done all the work.
- **A callback**: `void for_each_sample(..., std::function<void(Sample)>)`. Lazy, but it
  inverts control: the caller cannot `break`, compose with ranges, or hold the stream.
- **A hand-written iterator class**: a struct with `operator*`, `operator++`, `operator==`
  and the loop's state (`t`, the noise process) spread over member variables. That is the
  state machine the compiler now writes from the loop above.
- **C++20 without C++23**: coroutines but no library type, so every project wrote its own
  `generator` (promise type, iterator, sentinel), or used cppcoro or range-v3.

## Why it fits here

A rate curve is naturally "a loop that produces values". The noisy overload shows the
gain best: the `NoiseProcess` is a local of the loop, ticked every scenario step and
yielded only every k-th tick, and the frame keeps it alive between yields. As an iterator
class that would be four members and a hand-kept invariant; as a coroutine it is the loop
the engine itself runs. `simsys curve` prints any length of curve in constant memory.

No design pattern: just a lazy range (the comment at the declaration says so, so a
`Pattern:` grep does not suggest one).

## Pitfalls met while building

- **Reference parameters dangle; take them by value.** A coroutine copies its parameters
  into the frame, but a reference parameter is copied *as a reference*. The generator is
  returned and iterated later, so with `const RateCurve& curve` a call such as
  `samples(RateCurve{.base = Rate{1}}, 1s, 3s)` binds the reference to a temporary that
  dies at the end of the full expression, before the body has run once. `samples` takes
  `RateCurve curve` by value: the frame holds its own copy for as long as the generator
  lives. The same holds for a lambda coroutine's captures, which live in the closure
  object, not the frame: a lambda that `co_yield`s and captures by value still dangles
  once the closure is gone. Write the coroutine as a function with by-value parameters.
- **Nothing runs until the first `begin()`.** A generator starts suspended, so the
  `invalid_argument` for a bad step is thrown by `begin()`, not by the call. The test says
  it plainly: `REQUIRE_THROWS_AS(samples(c, 0s, 1s).begin(), std::invalid_argument)`.
  `main.cpp` therefore validates `--step` and `--until` itself (`curve_grid`) before it
  creates the generator, so a user error becomes a message, not an exception mid-print.
  The five-argument overload is not a coroutine, so its forwarding is immediate, and the
  checks still happen at the first `begin()` of the generator it returns.
- **The iterator is input-only and has no `operator->`.** `*it` returns `Sample&&` (the
  generator's reference type for a non-reference `T`), and there is no `operator->`. The
  first version of the test wrote `it->t` and did not compile; it now writes `(*it).t`.
- **Single pass, move-only.** `begin()` may be called once (its precondition is a
  coroutine still at its first suspension; a second call is undefined behaviour), so the
  noisy-stream test builds two generators to compare two runs. A generator can be moved (returned in a
  `std::expected`) but not copied.
- **Int64 overflow is the loop's job.** `t += step` near the end of the clock would wrap,
  so the loop advances with `advance_within`, which compares `step` with the room left;
  the test "samples stops cleanly when the next step would overflow the clock" pins it.
- **GCC 14.2 and modules.** `<generator>` is included in the global module fragment of an
  interface that imports another module, which CLAUDE.md §3 forbids for `<chrono>` and
  `<format>`. A probe build showed `<generator>` is not affected, so `samples()`
  lives in the interface.

## Further reading

- cppreference: [Coroutines](https://en.cppreference.com/w/cpp/language/coroutines),
  [`std::generator`](https://en.cppreference.com/w/cpp/coroutine/generator).
- P0912R5, *Merge Coroutines TS into C++20 working draft*.
- P2502R2, *`std::generator`: Synchronous Coroutine Generator for Ranges* (C++23).
- Lewis Baker, *Asymmetric Transfer* blog: "Coroutine Theory", "Understanding the promise
  type".
- CppCoreGuidelines CP.51 "Do not use capturing lambdas that are coroutines" and CP.53
  "Parameters to coroutines should not be passed by reference".
