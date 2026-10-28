# C++20 ranges and views (and the C++23 additions)

## Used in

| Construct | Where (file: function) |
|---|---|
| `ranges::lower_bound` + member projection | `src/flow/flow.cppm`: `Flow::position_of` |
| `ranges::find` + member projection | `src/config/config.cpp`: `find_advanced_number`, `name_of`; `src/report/report.cpp`: `mean_utilization`; `src/engine/behaviours.cppm`: `ClientBehaviour` constructor |
| `ranges::find` + lambda projection | `src/engine/graph.cppm`: `Graph::index_of` |
| `ranges::sort` (+ projection) | `graph.cppm`: `detail::find_async_hops`; `behaviours.cppm`: `detail::sort_by_destination`; `engine.cppm`: `offered_rates`; `config.cpp`: `read_op_shares` |
| `ranges::unique`, `ranges::binary_search` | `graph.cppm`: `find_async_hops`, `Graph::is_async_hop`; `behaviours.cppm`: `ClientBehaviour::response` |
| `ranges::count_if` with a member pointer | `report.cpp`: `paper_load` (`&graph::ResolvedLink::feedback`) |
| `views::values`, `views::keys`, `views::join` | `graph.cppm`: `Graph::connect_steps`, `Graph::route_clients`, `detail::call_roots`; `config.cpp`: `offers_op` |
| `views::split` + `ranges::any_of` | `config.cpp`: `lists_word` |
| `views::reverse` | `engine.cppm`: `Simulation::backward_pass` |
| C++23 `ranges::contains` | `graph.cppm`: `carries_op`, `Graph::add_edge`; `config.cpp`: `read_dc`, `offers_op`; `engine.cppm`: `add_once` |
| C++23 `ranges::fold_left` | `config.cpp`: `check_shares_sum_to_one`; `report.cpp`: `paper_load` |
| C++23 `ranges::to`, `views::join_with`, `views::zip` | `report.cpp`: `join`, `map_to_strings`, `Summary::observe` |

## What

Two halves, both in `<ranges>` / `<algorithm>` under `std::ranges`:

- **Range algorithms** take the range itself (`std::ranges::sort(hops)`) instead of an
  iterator pair, so `begin` and `end` cannot come from two different containers. Most take
  a **projection**: a callable applied to each element before it is compared. A member
  pointer is a projection, because the algorithms call it through `std::invoke`.
- **Views** are lazy, cheap-to-copy ranges built by adaptors and chained with `|`. Nothing
  is computed or copied until the loop pulls an element.

```cpp
// graph.cppm, Graph::connect_steps: every step of every op, in one loop
for (const model::Step& step : spec.ops | std::views::values | std::views::join) { ... }

// flow.cppm, Flow::position_of: binary search by op name, no comparator lambda
return std::ranges::lower_bound(self.entries_, op, {}, &Entry::op);
```

`{}` is the default comparison, `std::ranges::less`. C++23 added `ranges::to<C>()` to
collect a view into a container, `contains`, `fold_left`, `zip` and `join_with`.

## The pre-C++20 way

```cpp
auto it = std::lower_bound(v.begin(), v.end(), op,
                           [](const Entry& e, std::string_view key) { return e.op < key; });
bool found = std::find(links.begin(), links.end(), link) != links.end();
double sum = std::accumulate(shares.begin(), shares.end(), 0.0,
                             [](double s, const auto& p) { return s + p.second; });
for (const auto& [op, steps] : spec.ops) for (const auto& step : steps) { ... }
```

Every call repeats `begin()`/`end()`, comparators repeat the member on both sides, and
"collect into a container" is a `reserve` + `push_back` loop.

## Why it fits here

The engine is mostly small searches and walks over tables: a `Flow` is a sorted vector of
`{op, rate}` searched by op name, a graph is vectors of nodes and links searched by id, a
report folds and zips per-step samples. Projections let each search say *which member*
it compares (`&Entry::op`, `&NodeSummary::id`, `&AdvancedNumber::key`) without a lambda.
Views let `connect_steps` walk a `map<string, vector<Step>>` as one flat sequence without
copying a single step.

## Pitfalls met while building

- **Projection plus `string_view` against `std::string`.** In `Flow::position_of` the
  value searched for is a `std::string_view` and the projected member `Entry::op` is a
  `std::string`. `ranges::less` compares them only because the two types satisfy
  `std::totally_ordered_with` (checked with GCC 14). A type pair without a common reference
  fails with a constraint error, not a silent conversion. The member pointer projection
  returns a reference, so nothing is copied.
- **A lambda projection that copies.** `Graph::index_of` projects a node to its id with
  `[](const ResolvedNode& n) -> std::string_view { return n.spec.id; }`. Without the
  `-> std::string_view` the lambda returns `std::string` *by value*, a copy of every id it
  passes.
- **`views::join` over map values flattens in key order.** `spec.ops` is a `std::map`, so the
  joined steps come in sorted op-name order, not YAML order. That is deterministic, which
  §3.6 needs, but don't expect file order. The join works because each inner range is an
  lvalue `vector` living in the map. Joining ranges produced on the fly (a `transform` that
  returns vectors by value) is a C++23 fix (P2328R1) and single-pass.
- **`ranges::contains` is C++23** (P2302R4). In C++20 you write
  `std::ranges::find(r, x) != std::ranges::end(r)`. libstdc++ has `contains` from GCC 13.
  The repo comments its first use in each file.
- **`fold_left` takes its result type from the operation, not the initial value.**
  `std::accumulate(v.begin(), v.end(), 0)` over doubles sums into an `int` (0.5 + 0.25
  gives 0). `std::ranges::fold_left(v, 0, std::plus<>{})` returns `double` (0.75),
  because its type is that of `plus(init, element)`. Both were checked with GCC 14. The
  code still writes `0.0`, which makes the intent clear.
- **Views over temporaries dangle.** `Flow::entries()` returns a `std::span`, and the
  `&&`-qualified overload is deleted: `for (auto& e : make_flow().entries())` would iterate
  a dead vector. GCC 14 does not implement C++23's range-for lifetime extension (P2718R0).
- **`views::split` yields subranges, not strings.** `lists_word` builds a `std::string_view`
  from each piece with the C++20 iterator-pair constructor, which needs contiguous pieces.

## Further reading

- cppreference: [Ranges library](https://en.cppreference.com/w/cpp/ranges),
  [Constrained algorithms](https://en.cppreference.com/w/cpp/algorithm/ranges),
  [`ranges::contains`](https://en.cppreference.com/w/cpp/algorithm/ranges/contains),
  [`ranges::fold_left`](https://en.cppreference.com/w/cpp/algorithm/ranges/fold_left),
  [`ranges::to`](https://en.cppreference.com/w/cpp/ranges/to).
- P0896R4, *The One Ranges Proposal* (C++20 ranges, projections, views).
- P2302R4, *std::ranges::contains*; P2322R6, *ranges::fold*; P1206R7, *ranges::to*;
  P2321R2, *zip*; P2441R2, *views::join_with* (all C++23).
- P2328R1, *join_view should join all views of ranges*; P2718R0, *Wording for P2644R1 Fix
  for Range-based for Loop* (temporaries' lifetime).
- Eric Niebler's range-v3, the library the standard design came from.
