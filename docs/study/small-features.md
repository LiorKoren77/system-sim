# Small C++20/23 features

Some features are used all over the code but need only a few paragraphs each, not a note of
their own. This note has one section per feature. Each section says what the feature is, how
the same thing was written before it, and where the code uses it, by symbol (search for the
name; line numbers move). The first use of each feature in a file also carries a short
comment of its own (`CLAUDE.md` §1).

| Feature | Standard | Section |
|---|---|---|
| designated initializers | C++20 | [Designated initializers](#designated-initializers) |
| `std::span` | C++20 | [`std::span`](#stdspan) |
| `<=>` and defaulted `==` | C++20 | [Defaulted comparisons](#defaulted-comparisons) |
| `using enum` | C++20 | [`using enum`](#using-enum) |
| `consteval` | C++20 | [`consteval`](#consteval) |
| `std::to_array` | C++20 | [`std::to_array`](#stdto_array) |
| `std::to_underlying` | C++23 | [`std::to_underlying`](#stdto_underlying) |
| `std::unreachable` | C++23 | [`std::unreachable`](#stdunreachable) |
| `std::chrono` durations for `SimTime` | C++11 / C++17 | [`std::chrono` for `SimTime`](#stdchrono-for-simtime) |
| deducing `this` | C++23 | [Deducing `this`](#deducing-this) |
| `static operator()` | C++23 | [`static operator()`](#static-operator) |
| `std::erase_if` | C++20 | [`std::erase_if`](#stderase_if) |

## Designated initializers

**What (C++20).** An aggregate (a struct with public members and no user-written
constructors) can be initialized by naming its members: `{.mean_s = 2.0 * leg,
.variance_s2 = 0.0}`. A member left out gets its default member initializer, or is
value-initialized if it has none.

**Before.** Positional braces, `{2.0 * leg, 0.0}`. The reader has to remember the order of the
members, and two members of the same type can be swapped without any error: a mean passed as
a variance still compiles. The other option was a constructor with named parameters, which
makes the struct no longer an aggregate.

**Where.** Wherever a struct holds two numbers of the same type, which in a simulator is
almost everywhere:
- `behaviours::round_trip` and `queueing::mmc_wait` build `LatencyEstimate`/`TimeMoments` as
  `{.mean_s = ..., .variance_s2 = ...}`.
- `defaults::latency_numbers`, the book's latency table, is a `constexpr` constant written
  with designators (`.disk_seek = 10_ms`), so it reads like the book's table.
- The `Simulation` constructor adds each link as `Link{.line = ..., .pipe = ...}`;
  `session::run_summary` runs the simulation with `{.duration = ..., .step = ...}`;
  `config::detail::validate` starts from `Site{.file = file, .where = ""}`.
- The tests build every `NodeSpec` this way (`NodeSpec{.id = "web", .type = ...}`), naming only
  the members a test cares about.

**Pitfalls.** The designators must follow the order in which the members are declared (C
allows any order, C++ does not), and a wrong order is a compile error, which here is what you
want. C++ also does not allow C's nested (`.a.b = 1`) or array (`[2] = 1`) designators, or
mixing named and positional members. GCC's `-Wmissing-field-initializers` warns about a member
left out unless the member has an initializer of its own; that is why every member of
`report::Summary::OpTotals` has one, even if it is only `{}` (its comment says so).

## `std::span`

**What (C++20).** `std::span<T>` is a pointer and a length: a non-owning view of contiguous
elements (a `std::vector`, a `std::array`, a C array, or part of one). `std::span<const T>`
can only be read.

**Before.** A `(const T* data, std::size_t size)` pair, which loses the link between the two,
or `const std::vector<T>&`, which forces every caller to own a `vector` and exposes the
container type in the interface.

**Where.**
- Returning a view of a member: `Flow::entries()` and `Popularity::buckets()` return
  `std::span<const ...>` instead of `const std::vector<...>&`.
- Taking tables of different sizes: `core::detail::find_scale` takes the unit tables (six
  duration units, four rate prefixes) as `std::span<const Scale>`, without a template.
- Optional lists in an interface: `session::load_graph(path, std::span<const config::Override>
  overrides = {})` accepts a vector, an array or nothing.
- Filling in place: `Simulation::record_queue_lags(std::span<NodeSample>)` writes into the
  step's samples through a non-const span.
- `RequestSource::ops()` returns `std::span<const std::string>`, `report::align_columns` takes
  `std::span<const std::vector<std::string>>`, `queueing::fitted_answering` takes
  `std::span<const ReplicaGroup>`, and `views::chunk_by`'s chunks become spans in
  `Graph::resolve_request_sizes` ([ranges.md](ranges.md)).

**Pitfall: a span over a temporary dangles.** `Flow::entries() const&` would happily bind to
a temporary `Flow`, and the span would outlive it. So both `Flow::entries` and
`Popularity::buckets` add a deleted `const&&` overload, which is the better match for an
rvalue, and `for (auto& e : make_flow().entries())` does not compile.

## Defaulted comparisons

**What (C++20).** `auto operator<=>(const T&) const = default;` asks the compiler for a
three-way comparison that compares the members in declaration order. From it the compiler
derives `<`, `<=`, `>` and `>=`, and a defaulted `<=>` also declares a defaulted `==` (from
which `!=` is rewritten). `bool operator==(const T&) const = default;` alone gives equality
without an order.

**Before.** Six hand-written operators, or a comparator built with
`std::tie(a.x, a.y) < std::tie(b.x, b.y)`. Each one has to list every member, and a member
added later and forgotten in one of them is a silent bug.

**Where.**
- An order for sorting, deduplicating and binary search: `graph::AsyncHop` (sorted and
  searched by `Graph::is_async_hop`), the call graph's `detail::Call` (a `std::map` key),
  `detail::KeyArrival`, `detail::SizeArrival`.
- An order on a strong type: `Rate`, `ByteRate` and `Bytes` in `simsys.core`.
- Equality only: `flow::LatencyEstimate`, `behaviours::MachineGroup` (what lets
  `KvClusterBehaviour::observe` see that this step's machines are last step's),
  `model::KeySpec`, `graph::DemandEdge`.

**Pitfalls.**
- `<=>` over a `double` member deduces `std::partial_ordering`, because NaN is unordered:
  `a < b` and `a >= b` can both be false. The unit parsers reject NaN and infinity, so no `Rate`
  can hold one ([expected.md](expected.md)).
- Only default the order a type really has. `LatencyEstimate` defaults `==` but not `<=>`:
  is {1 s, 100 s²} less than {2 s, 0}? No answer is right, so the type offers none.
- A defaulted comparison needs every member's comparison to be usable where it is defaulted.
  `report.cpp`'s `same_slo` compares two `SloSpec`s by hand because `SimTime`'s `==` (from
  `<chrono>`) is not reachable through `simsys.model`'s interface (the first GCC defect,
  `CLAUDE.md` §3).

## `using enum`

**What (C++20).** `using enum model::NodeType;` brings every enumerator of a scoped enum into
the current scope, so a `switch` reads `case cdn:` instead of `case model::NodeType::cdn:`.

**Before.** A qualified name on every case, or a `using` declaration per enumerator, or giving
up the scoped `enum class` (and its protection against silent conversion to `int`) for an
unscoped `enum`.

**Where.** Always at function scope, so nothing leaks: `defaults::for_type`,
`fluid::zone_of`, `report`'s `zone_name`, `graph`'s
`replica_model_of`, `workload::wave_multiplier`, `RateLimiterBehaviour::admit`, and two tests.

## `consteval`

**What (C++20).** A `consteval` function is an *immediate function*: every call must be
evaluated at compile time, and a call that cannot be is a compile error. A `constexpr`
function, by contrast, may run at compile time and silently runs at run time otherwise.

**Before.** A `constexpr` function plus a hope that every caller used it in a constant
expression, or template metaprogramming.

**Where.**
- The time literals of `simsys.core` (`250_us`, `10_ms`, `1_h`, ...) are `consteval` and call
  `detail::checked_literal`, which throws when the nanosecond count overflows `int64_t`.
  In an immediate function a `throw` that is reached is a compile error, so
  `9223372036854775808_ns` does not compile instead of wrapping around (the time-literal tests
  in `tests/unit/test_units.cpp` show the cases).
- Build-time proofs used in `static_assert`: `core::follows_enum_order` and
  `core::round_trips` (a name table lists the enumerators in order and every name reads back
  to its enumerator), and `workload::mean_of`, which proves that the diurnal table averages 1.

## `std::to_array`

**What (C++20).** `std::to_array<T>({...})` builds a `std::array<T, N>` from a braced list and
deduces `N`.

**Before.** `std::array<EnumName<NodeType>, 18>{...}`, with the 18 written by hand and kept in
step with the list. Class template argument deduction (`std::array{...}`) cannot help when the
elements are themselves braced lists.

**Where.** The name tables: `model::node_type_table` (18 rows), `workload::wave_shape_table`,
and `graph`'s `replica_model_table`; short fixed lists such as the two branches in
`behaviours::conditional_hop` and `Simulation::request_weighted_sojourn`.

## `std::to_underlying`

**What (C++23).** `std::to_underlying(e)` returns an enum's value as exactly its underlying
type.

**Before.** `static_cast<std::size_t>(e)`, which compiles for any target type, so it can
narrow silently, and says nothing about why the cast is there.

**Where.** Where an enumerator is used as a row index: `model::is_cataloged`,
`core::follows_enum_order` (which proves that row `i` of a name table holds the enumerator
whose value is `i`), and the loader's check that its per-type key table follows the
`NodeType` order (`own_keys_follow_node_type_order` in `src/config/detail.hpp`).

## `std::unreachable`

**What (C++23).** `std::unreachable()` tells the compiler that control never gets there. If it
does, the behaviour is undefined. The compiler may drop the code path, and it stops warning
that a function can end without returning.

**Before.** A dummy `return` value after a `switch` that returns in every case (which hides a
missing case), or the compiler builtin `__builtin_unreachable()`.

**Where.** After a `switch` without a `default:` that returns from every case:
`defaults::for_type` and `report`'s `zone_name`. `-Wswitch` (an error here, under `-Werror`)
proves that every enumerator has a case, and the guards before the `switch` prove that the value
is one of them. `Simulation::make_node` (`src/engine/engine.cppm`) has a case for every node type
and no `default:`, and ends in `std::unreachable()` after its `switch`; until phase 3 gave the last
four types a model, those four cases ended in it themselves, behind a refusal in
`Simulation::create` that phase 3 deleted ([variant-visit.md](variant-visit.md) has the story).
The fault schedule's switch over `FaultKind` (`src/engine/faults.cppm`) ends the same way.

**Pitfall: it is a promise, not a check.** If the promise is wrong, the program has
undefined behaviour, with no message. So it goes only where a proof sits right before it.
`workload::wave_multiplier` shows the other choice: a `WaveShape` cast from a stray integer can
reach the end of its `switch`, so it throws `std::logic_error` there (its comment explains why
`std::unreachable` would have made that case undefined behaviour).

## `std::chrono` for `SimTime`

**What.** `using SimTime = std::chrono::duration<std::int64_t, std::nano>;` (`simsys.core`):
simulated time is a count of integer nanoseconds with its unit in the type. `std::chrono`
durations are C++11; `std::chrono::round` is C++17. The project uses no C++20 calendar type
(`year_month_day`, time zones): a clock time in a scenario ("20:00") is just an offset from
t = 0, which is midnight.

**Before.** A `double` of seconds. Seconds and milliseconds can then be mixed by accident, and
a sum of millions of 0.1 s steps drifts, because 0.1 has no exact binary value. Integer
nanoseconds add exactly.

**Where.** `seconds(SimTime)` and `from_seconds(double)` (with `std::chrono::round`, so a step
is rounded to the nearest nanosecond rather than truncated) convert at the edge of a formula.
`describe_duration` prints a duration the way a scenario writes it (`10s`, `1500ms`), and the
time literals build `SimTime` constants. `parse_clock_time`'s comment says why it does not use
`std::chrono::parse` (a stream, locale machinery, and no precise error message).

**Pitfall: GCC 14.2 and `<chrono>` in modules.** `<chrono>` pulls in `<format>`, so an
interface unit that imports another module must not include it (the first defect,
[modules.md](modules.md)). Such interfaces, `simsys.workload` among them, see `SimTime` but not
`<chrono>`'s operators, so they compute on `.count()` (exact integer nanoseconds) or on
`seconds(t)`.

## Deducing `this`

**What (C++23).** A member function can name its object as an explicit first parameter,
`this auto& self`. The object's type, including `const`, is then deduced like any template
parameter.

**Before.** Two overloads, one `const` and one not, with the same body, or one written in terms
of the other through `const_cast`, or CRTP.

**Where.** `Flow::position_of(this auto& self, std::string_view op)` binary-searches the
flow's entries by op name. Called from `Flow::rate_of`, a `const` member, `self` is const and
the result is a `const_iterator`. Called from `Flow::add`, it is an `iterator` that can be
written through. One function serves both.

## `static operator()`

**What (C++23).** A call operator that reads no member may be declared `static`, so a call
passes no `this` pointer.

**Before.** `double operator()(...) const`: the object's address is still passed, and the
optimiser has to see that it is unused.

**Where.** `behaviours::NothingLost`, the default `FractionLookup` of the `route` templates
([concepts.md](concepts.md)): "what fraction of what was sent arrived?", answered with 1
whoever asks. `BlobStoreBehaviour` passes `NothingLost{}` to its event notifications, which
nothing waits for.

## `std::erase_if`

**What (C++20).** `std::erase_if(container, predicate)` removes every element for which the
predicate holds and returns how many it removed. It is a free function overloaded for each
standard container (`<vector>`, `<string>`, `<deque>`, `<list>`, the maps and sets), so the same
call is right for each: on a `std::list` or a map it erases node by node, on a `std::vector` it
shifts the survivors down.

**Before.** The erase-remove idiom, `v.erase(std::remove_if(v.begin(), v.end(), p), v.end())`:
two algorithms and three iterators for one intent. `std::remove_if` alone erases nothing (it moves
the survivors to the front and returns where the leftovers start), and forgetting the `erase`
around it, or its second argument, compiles and leaves the vector the same size. For a map the
idiom does not even apply; one wrote the loop `it = m.erase(it)` by hand.

**Where.** `in_one_unit` (`src/engine/fault_log.cppm`): a `dc_down`'s block counts its
datacenter's nodes in one unit, so when they mix request-counting and byte-sized nodes it drops
the byte-sized ones from its list, `std::erase_if(nodes, [&](std::size_t node) { return
defaults::is_byte_sized(...); })`, and keeps the rest in order.

**Pitfall.** The lambda is handed to an algorithm, not to a range adaptor, so CLAUDE.md §3's fifth
GCC 14.2 defect (a lambda passed to `std::views::chunk_by` and friends in a module function,
defined twice at link time) does not apply. The predicate must not modify the elements it is
asked about.

## Further reading

- P0329R4, *Designated initialization wording*.
- P0122R7, *span: bounds-safe views for sequences of objects*.
- P0515R3, *Consistent comparison*; P1185R2, *`<=> != ==`*.
- P1099R5, *Using enum*.
- P1073R3, *Immediate functions*.
- P0325R4, *to_array from LFTS with updates*.
- P1682R3, *std::to_underlying for enumerations*.
- P0627R6, *Function to mark unreachable code*.
- P0092R1, *Polishing `<chrono>`* (`round`, `floor`, `ceil`).
- P0847R7, *Deducing this*.
- P1169R4, *static operator()*.
- P1209R0, *Adopt consistent container erasure from Library Fundamentals 2 for C++20*.
- cppreference: [aggregate initialization](https://en.cppreference.com/w/cpp/language/aggregate_initialization),
  [`std::span`](https://en.cppreference.com/w/cpp/container/span),
  [default comparisons](https://en.cppreference.com/w/cpp/language/default_comparisons),
  [`std::unreachable`](https://en.cppreference.com/w/cpp/utility/unreachable).
