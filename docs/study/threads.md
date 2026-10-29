# Threads: C++20 `std::jthread`, `std::stop_token`, and `std::atomic` as a work queue

## Used in

| Construct | File | Function |
|---|---|---|
| `std::jthread` workers, joined when the vector goes out of scope | `src/sweep/sweep.cpp` | `run_in_parallel` |
| `std::atomic<std::size_t>` handing out point indices (`fetch_add`) | `src/sweep/sweep.cpp` | `run_in_parallel`, `pull_indices` |
| results stored by index in a preallocated vector, no lock | `src/sweep/sweep.cpp` | `PerPoint`, `for_each_point` |
| the lowest failing point wins, whatever the threads did | `src/sweep/sweep.cpp` | `PerPoint::first_error` |
| `std::thread::hardware_concurrency` as the default `--jobs` | `src/sweep/sweep.cpp` | `default_jobs` |
| the property, from outside: same bytes with `--jobs 1` and `--jobs 3` | `tests/cli/sweep_jobs.cmake`, `tests/unit/test_sweep.cpp` | `same_bytes`; "the rows do not depend on how many workers ran the points" |

`simsys sweep` runs one scenario once per value of one key (`--param
nodes.users.rate.base=100/s..900/s:3`), and the runs are independent, so they run side by side:
three 24-hour youtube simulations take 28 s with `--jobs 3` instead of 62 s with `--jobs 1`.

## What

**A thread** is a second (third, ...) flow of control in the same process: same memory, its own
stack. `std::thread` (C++11) starts one from any callable; `join()` waits for it to finish.

**`std::jthread`** (C++20, `<thread>`) is a `std::thread` with two additions:

1. **It joins in its destructor.** A `std::thread` that is still joinable when it is destroyed
   calls `std::terminate`: every path out of the function (an early return, an exception) had to
   join by hand, or the program died. A `std::jthread` is RAII: leaving the scope is the join.
2. **It carries a `std::stop_source`.** If the callable takes a `std::stop_token` as its first
   parameter, the jthread passes one in; `request_stop()` (called by the destructor before it
   joins) sets it, and the thread's loop can check `token.stop_requested()` and return early.
   It is a polite request, not a kill: a thread that never looks never stops early.

```cpp
std::atomic<std::size_t> next{0};
std::vector<std::jthread> workers;
workers.reserve(threads);
for (std::size_t t = 0; t < threads; ++t) {
  workers.emplace_back(pull_indices, std::ref(next), count, std::cref(work));
}
}  // the jthreads join here
```

`std::ref` / `std::cref`: a thread copies its arguments (they must outlive the caller's frame in
general); the reference wrappers pass `next` and `work` by reference on purpose, which is safe
here because the jthreads join before `next` and `work` go out of scope.

**`std::atomic<T>`** (C++11, `<atomic>`) is a value that several threads may read and modify at
once. `next.fetch_add(1)` reads the counter, adds one and stores it as **one indivisible step**,
and returns the old value:

```cpp
void pull_indices(std::atomic<std::size_t>& next, std::size_t count,
                  const std::function<void(std::size_t)>& work) {
  for (auto index = next.fetch_add(1); index < count; index = next.fetch_add(1)) work(index);
}
```

## The atomic counter is the work queue

A thread pool usually has a queue of tasks guarded by a mutex and a condition variable. A sweep
needs none of that: all the tasks (points 0 .. n-1) are known before the first thread starts, so
the "queue" is the next unclaimed index, and taking a task is one `fetch_add`. Each index is
handed out exactly once (no two workers get the same one, none is skipped), with no lock.

Why pull, not split? Giving worker 1 points 0-2 and worker 2 points 3-5 up front is simpler but
balances badly: the points of a sweep differ in cost (a saturated point may take longer), and a
worker done with its share sits idle while another still has three to go. Pulling one index at a
time keeps every worker busy until the last point is claimed.

`fetch_add` with the default `std::memory_order_seq_cst` is used for readability; the counter
only has to hand out distinct numbers, so `std::memory_order_relaxed` would be enough: the
results are published to the main thread by the joins (below), not by the counter.

## What a data race is, and why there is none here

A **data race** is two threads touching the same memory location, at least one of them writing,
with nothing ordering the two accesses (no lock, no atomic, no join between them). In C++ it is
undefined behaviour: not "you might read a stale value", but anything, since the compiler and the
CPU are allowed to assume it never happens (keep a value in a register, reorder stores, tear a
64-bit write in two).

What the sweep's threads share, and why each is safe:

| Shared | Who writes | Why it is not a race |
|---|---|---|
| `next`, the counter | every worker | `std::atomic`: every access is an atomic operation |
| `results.values[i]`, `results.errors[i]` | only the worker that claimed index `i` | distinct elements of a `std::vector` are distinct memory locations; one writer each, and the main thread reads them only after the joins |
| the scenario path, the `SweepParam`, the metrics | nobody (read only) | concurrent reads never race |
| the yaml-cpp document, the `Graph`, the `Simulation`, the `Summary` | the worker of that point | each point has its own; nothing is shared between points |

The **join** is what makes the main thread's reads safe: everything a thread did *happens-before*
its `join()` returns. So the vectors need no lock, and the comment at `PerPoint` says so.

One trap the design avoids: `std::vector<bool>` is not a vector of `bool`s but of bits packed
into bytes, so two threads writing "different elements" write the same byte: a data race.
`PerPoint` stores `std::optional<T>` and `std::string`, never `bool`.

## Why each point is its own Simulation, and why `--jobs` cannot show in the output

Every point loads the file with its value in place (`config::load(path, overrides)`), builds its
own `Graph`, creates its own `Simulation` and feeds its own `report::Summary`. A simulation is
deterministic: the same scenario and seed give the same numbers, bit for bit (the clients' noise
is seeded from the scenario, `behaviours::client_seed`). Nothing in one point depends on another
or on which thread ran it, and row `i` is stored at index `i`, not in the order the points
finish. So the rows, and the bytes printed from them, are the same for any `--jobs`: the CLI test
compares `--jobs 1` with `--jobs 3` byte for byte, and the unit test compares the doubles exactly.

Sharing one `Simulation` between points (resetting it per point) would save a little memory and
cost exactly this property: one forgotten member would carry state from one point into the next,
and only for some thread schedules.

Errors follow the same rule. Which failing point a worker meets first depends on the threads;
`PerPoint::first_error` scans the slots in index order after the joins, so the report always
names the lowest-numbered failing point.

## Why `std::stop_token` is not used here

A sweep wants every point: there is no "first answer wins" to stop the others for. The one case
where stopping early would pay is a failure, but every point is loaded and checked before any is
simulated (the first phase of `run_sweep`), so a bad value never wastes a 24-hour run. The
jthreads still get a `stop_token` (it is part of the type) and `pull_indices` simply does not take
it.

An exception thrown inside a worker would call `std::terminate` (a thread's exception does not
cross into the thread that joins it). That is acceptable here because the project throws only
for programmer errors (CLAUDE.md §3); user mistakes come back as `std::expected` values and are
stored per point.

## Why `<thread>` lives in `sweep.cpp`

`sweep.cppm`, the interface, declares what a sweep *is* (`SweepParam`, `run_sweep`, ...); how the
points are spread over threads is an implementation detail no importer needs to see, so
`<thread>` and `<atomic>` are included only in the implementation unit, `sweep.cpp`, and an
importer (the CLI, the tests) does not pay to parse them. This is a design choice, a lean
interface, not a compiler workaround: the GCC 14.2 "Bad file data" defect (CLAUDE.md §3) needs
`<chrono>`, `<format>`, `<filesystem>` or `<print>` in the global module fragment of an interface
that imports another module, and `sweep.cppm` imports none, so it would not bite here. Even
`default_jobs()` (`std::thread::hardware_concurrency()`) is a function of the module, so
`main.cpp` never includes `<thread>`.

## The pre-C++20 way

- `std::thread` plus a hand-written RAII guard (`struct joiner { ~joiner() { t.join(); } }`), or
  a `try`/`catch` that joins on every path.
- A stop flag: a `std::atomic<bool>` shared by hand, which `std::stop_token` now packages (and
  `std::condition_variable_any` can wait on).
- Before C++11, platform APIs (`pthread_create`, `CreateThread`) and no portable memory model at
  all: what a data race even meant was up to the compiler.

## Further reading

- cppreference: `std::jthread`, `std::stop_token`, `std::atomic`, "Memory model" (the data race
  definition) and `std::memory_order`.
- Anthony Williams, *C++ Concurrency in Action*, 2nd ed., ch. 2 (managing threads), ch. 5 (the
  memory model and atomics), ch. 8 (dividing work between threads).
- Nicolai Josuttis, P0660 (the proposal for `std::jthread` and the stop token).
