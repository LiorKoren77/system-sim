// Module `simsys.flow`: the per-op flow map (how much traffic of each operation type
// crosses a node, in requests per second) and the composition rules for latency moments
// (means and variances add along a path, the slowest branch dominates in parallel).
//
// Models: the "what travels along an edge" part of the fluid engine (spec §3.5-3.6). A
// flow is a rate per operation name ("read", "write", ...), like the request mix a load
// balancer sees in System Design Interview ch. 1; latency travels the other way, as
// moments, and is composed by the backward pass. Deliberately ignored: request sizes,
// per-request identity and any correlation between hops (see LatencyEstimate).
//
// C++20 named module, interface unit: `module;` opens the global module fragment, where
// the std headers are included so they stay shareable with other translation units;
// `export module` starts the module's purview. Replaces a header plus include guards:
// only what is marked `export` is visible to importers, and no macros leak out.
// (See src/core/core.cppm for the longer explanation.)
//
// GCC 14 note: a translation unit that iterates `Flow::entries()` should `#include <span>`
// itself rather than rely on this module's global-module-fragment include reaching it,
// because GCC 14's reachability of GMF declarations has known bugs. (Checked 2026-10-28
// on GCC 14.2: the range-for in tests/unit/test_flow.cpp compiled without it, so the
// rule is defensive.)
module;
#include <algorithm>
#include <cmath>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

export module simsys.flow;

import simsys.core;

// Not exported: module linkage, visible only inside simsys.flow.
namespace simsys::flow {
void require_finite_non_negative(double value, const char* message) {
  // C++20 [[unlikely]]: tells the compiler this branch is cold, so the hot path is laid
  // out first. Replaces the compiler-specific `__builtin_expect`, usually hidden in a
  // LIKELY/UNLIKELY macro. Here the branch is only ever taken by a programmer error.
  if (!std::isfinite(value) || value < 0.0) [[unlikely]]
    throw std::invalid_argument{message};
}
}  // namespace simsys::flow

export namespace simsys::flow {

using OpName = std::string;

// Mean and variance (seconds, seconds^2) of a latency contribution. Moments rather
// than a full distribution: along a path they add (assuming the hops are independent,
// Var(X + Y) = Var X + Var Y), and the client fits a lognormal at the end
// (queueing.cppm). Spec §3.5.
struct LatencyEstimate {
  double mean_s{};
  double variance_s2{};
  // C++20 defaulted operator==: member-wise equality (and, by C++20 rewriting, `!=`) in
  // one line instead of two hand-written operators. Only equality is defaulted, not
  // `<=>`: an estimate has no natural order (is {1 s, 100 s^2} less than {2 s, 0}?).
  // Being IEEE comparisons, an estimate holding a NaN compares unequal even to itself.
  bool operator==(const LatencyEstimate&) const = default;
  LatencyEstimate& operator+=(const LatencyEstimate& other);
  // Hidden friend: found only through argument-dependent lookup, so it does not pollute
  // the namespace; defined in terms of += so the two cannot drift apart.
  friend LatencyEstimate operator+(LatencyEstimate a, const LatencyEstimate& b) { return a += b; }
};

// Fan-out in parallel: the caller waits for the slowest branch. The max of lognormals
// has no closed form; spec §3.5 approximates it by the largest mean and the largest
// variance among the branches. No branches -> zero.
//
// This approximation is OPTIMISTIC, not pessimistic: the slowest of several random
// branches is slower on average than the slowest average, E[max Xi] >= max E[Xi].
// Example: two independent Exp(1) branches have E[max] = 1 + 1/2 = 1.5 and
// Var[max] = 1 + 1/4 = 1.25, but this returns {1, 1}. It also treats a step with
// `factor: 100` as one branch, ignoring tail amplification: a request fanned out to 100
// servers waits for the slowest of the 100, so a rare per-server p99 becomes the common
// case (Dean & Barroso, "The Tail at Scale", CACM 2013).
// Recorded improvement for a later phase (the spec owns this decision): for n iid
// branches P(max <= x) = F(x)^n, so the exact quantile is
//   quantile_max(p) = quantile_single(p^(1/n)).
[[nodiscard]] LatencyEstimate parallel_max(std::span<const LatencyEstimate> branches);

// A flow: rate per op name, additive.
// Pattern: flat map (a data-structure idiom) — a sorted vector of entries searched by
// binary search. A flow holds a few ops and is iterated every step, so one contiguous
// vector beats the node-per-entry std::map on cache locality and allocations (GCC 14 has
// no std::flat_map yet; C++23 adds it, and this class is what it would replace).
// Determinism: unlike std::unordered_map, whose iteration order depends on hashing and
// bucket count, sorted order makes every run byte-identical (§3.6).
//
// Rates and scale factors must be finite and non-negative. The engine never subtracts
// flows and scenario validation rejects bad user input, so a negative or NaN value here
// is a programmer error, reported by std::invalid_argument (CLAUDE.md allows exceptions
// for those); a rejected call leaves the flow unchanged.
class Flow {
 public:
  struct Entry {
    OpName op;
    Rate rate;
  };
  void add(std::string_view op, Rate rate);               // accumulates
  [[nodiscard]] Rate rate_of(std::string_view op) const;  // Rate{0} when absent
  [[nodiscard]] Rate total() const;
  void scale(double factor);                       // multiplies every rate in place
  [[nodiscard]] Flow scaled(double factor) const;  // a scaled copy; does not mutate
  // "No op ever seen" since construction or clear(), not "zero traffic": an op added
  // with Rate{0} still makes the flow non-empty. Ask total() for the traffic.
  [[nodiscard]] bool empty() const { return entries_.empty(); }
  // C++20 std::span<const T>: a non-owning read-only view of contiguous elements. Callers
  // iterate the entries without copying them and without seeing the vector type.
  // The view is valid until the next non-const call on this Flow (add, scale, +=, clear
  // may reallocate or reorder the vector).
  [[nodiscard]] std::span<const Entry> entries() const& { return entries_; }
  // Deleted on rvalues: a view into a dying temporary is a bug, e.g.
  // `for (auto& e : make_flow().entries())` dangles (GCC 14 lacks the C++23 P2718
  // range-for lifetime extension that would keep the temporary alive).
  std::span<const Entry> entries() const&& = delete;
  void clear() { entries_.clear(); }
  Flow& operator+=(const Flow& other);

 private:
  // C++23 deducing `this` (explicit object parameter): `this auto& self` turns the object
  // into an ordinary deduced template parameter, so one function serves both callers. On
  // a const Flow (rate_of) `self.entries_` is const and the result is a const_iterator; on
  // a mutable one (add) it is an iterator that can be written through. Replaces the
  // pre-C++23 duplicated const/non-const overload pair (or const_cast and CRTP tricks).
  // C++20 std::ranges::lower_bound: takes the container itself instead of an iterator
  // pair, and a *projection* (`&Entry::op`) that says which member to compare, so we can
  // binary-search Entries by op name without writing a comparator lambda.
  [[nodiscard]] auto position_of(this auto& self, std::string_view op) {
    return std::ranges::lower_bound(self.entries_, op, {}, &Entry::op);
  }
  // True when `position` (as returned by position_of) holds exactly `op`: lower_bound
  // only finds where `op` would go, which may be a different, longer name or the end.
  [[nodiscard]] bool is_at(std::vector<Entry>::const_iterator position, std::string_view op) const;
  std::vector<Entry> entries_;
};

LatencyEstimate& LatencyEstimate::operator+=(const LatencyEstimate& other) {
  mean_s += other.mean_s;
  variance_s2 += other.variance_s2;
  return *this;
}

LatencyEstimate parallel_max(std::span<const LatencyEstimate> branches) {
  LatencyEstimate slowest{};
  for (const auto& branch : branches) {
    slowest.mean_s = std::max(slowest.mean_s, branch.mean_s);
    slowest.variance_s2 = std::max(slowest.variance_s2, branch.variance_s2);
  }
  return slowest;
}

bool Flow::is_at(std::vector<Entry>::const_iterator position, std::string_view op) const {
  return position != entries_.cend() && position->op == op;
}

void Flow::add(std::string_view op, Rate rate) {
  require_finite_non_negative(rate.per_second, "Flow::add: rate must be finite and >= 0");
  const auto position = position_of(op);  // mutable iterator: *this is non-const here
  if (is_at(position, op)) {
    position->rate.per_second += rate.per_second;
  } else {
    entries_.insert(position, Entry{std::string{op}, rate});
  }
}

Rate Flow::rate_of(std::string_view op) const {
  const auto position = position_of(op);
  return is_at(position, op) ? position->rate : Rate{0};
}

Rate Flow::total() const {
  double sum = 0.0;
  for (const auto& entry : entries_) sum += entry.rate.per_second;
  return Rate{sum};
}

void Flow::scale(double factor) {
  require_finite_non_negative(factor, "Flow::scale: factor must be finite and >= 0");
  for (auto& entry : entries_) entry.rate.per_second *= factor;
}

Flow Flow::scaled(double factor) const {
  Flow result = *this;
  result.scale(factor);
  return result;
}

Flow& Flow::operator+=(const Flow& other) {
  // Self-add guard: `f += f` would otherwise loop over `other.entries_` while add()
  // mutates that very vector. Doubling in place says what is meant and never iterates a
  // container it is changing.
  if (this == &other) {
    scale(2.0);
    return *this;
  }
  for (const auto& entry : other.entries_) add(entry.op, entry.rate);
  return *this;
}

}  // namespace simsys::flow
