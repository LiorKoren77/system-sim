// simsys::Generator<T>, the project's coroutine generator (src/core/generator.cppm): the
// properties workload::samples relies on, checked on coroutines written here.
#include <catch2/catch_test_macros.hpp>

#include <coroutine>  // a unit that defines a coroutine needs std::coroutine_traits
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <utility>
#include <vector>

// C++20 `import`: after every #include (the eighth GCC 14.2 defect, CLAUDE.md §3).
import simsys.generator;

using simsys::Generator;

namespace {

// C++20 coroutine: `co_yield` makes this function one; each value is handed to the consumer and
// the body waits, suspended, until the consumer asks for the next.
Generator<int> count_to(int n, int& started) {
  ++started;
  for (int i = 1; i <= n; ++i) co_yield i;
}

Generator<int> fails_before_yielding() {
  throw std::invalid_argument{"bad argument"};
  co_yield 0;  // never reached; makes the function a coroutine
}

Generator<int> fails_after(int n) {
  for (int i = 0; i < n; ++i) co_yield i;
  throw std::runtime_error{"stopped"};
}

std::vector<int> collect(Generator<int> g) {
  std::vector<int> out;
  for (const int v : g) out.push_back(v);
  return out;
}

}  // namespace

TEST_CASE("a generator yields its values in order, then ends", "[generator]") {
  int started = 0;
  CHECK(collect(count_to(3, started)) == std::vector{1, 2, 3});
  CHECK(collect(count_to(0, started)).empty());
}

TEST_CASE("a generator's body does not run until the first begin", "[generator]") {
  int started = 0;
  auto g = count_to(3, started);
  CHECK(started == 0);  // lazy: creating it ran nothing
  auto it = g.begin();
  CHECK(started == 1);
  CHECK(*it == 1);
}

TEST_CASE("a generator is consumed only as far as it is read", "[generator]") {
  int started = 0;
  auto g = count_to(1'000'000, started);
  std::vector<int> first;
  for (const int v : g) {
    first.push_back(v);
    if (first.size() == 3) break;  // the other 999,997 values are never computed
  }
  CHECK(first == std::vector{1, 2, 3});
}

TEST_CASE("what a generator's body throws reaches the consumer at begin or at ++", "[generator]") {
  auto bad = fails_before_yielding();
  CHECK_THROWS_AS(bad.begin(), std::invalid_argument);
  auto late = fails_after(2);
  auto it = late.begin();
  CHECK(*it == 0);
  ++it;
  CHECK(*it == 1);
  CHECK_THROWS_AS(++it, std::runtime_error);
}

TEST_CASE("a generator moves and is an input range", "[generator]") {
  int started = 0;
  auto g = count_to(2, started);
  auto moved = std::move(g);
  CHECK(collect(std::move(moved)) == std::vector{1, 2});
  // C++20 concepts as compile-time checks: what a range-for and the ranges algorithms need.
  static_assert(std::input_iterator<Generator<int>::iterator>);
  static_assert(std::ranges::input_range<Generator<int>>);
}
