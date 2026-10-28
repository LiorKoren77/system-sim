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
module;
#include <algorithm>
#include <compare>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module simsys.flow;

import simsys.core;

export namespace simsys::flow {

using OpName = std::string;

// Mean and variance (seconds, seconds^2) of a latency contribution. Moments rather
// than a full distribution: along a path they add (assuming the hops are independent,
// Var(X + Y) = Var X + Var Y), and the client fits a lognormal at the end
// (queueing.cppm). Spec §3.5.
struct LatencyEstimate {
  double mean_s{};
  double variance_s2{};
  // C++20 defaulted three-way comparison: one line gives ==, !=, <, <=, >, >= comparing
  // members in declaration order, replacing six hand-written operators. Here it is
  // mostly used for `==` (e.g. "empty parallel_max equals a zero estimate" in tests).
  auto operator<=>(const LatencyEstimate&) const = default;
  LatencyEstimate& operator+=(const LatencyEstimate& other);
  // Hidden friend: found only through argument-dependent lookup, so it does not pollute
  // the namespace; defined in terms of += so the two cannot drift apart.
  friend LatencyEstimate operator+(LatencyEstimate a, const LatencyEstimate& b) { return a += b; }
};

// Fan-out in parallel: the caller waits for the slowest branch. The max of lognormals
// has no closed form; spec §3.5 approximates it by the largest mean and the largest
// variance among the branches (slightly pessimistic on variance). No branches -> zero.
[[nodiscard]] LatencyEstimate parallel_max(std::span<const LatencyEstimate> branches);

// A flow: rate per op name, additive. Pattern: flat map (sorted vector) — a few ops per
// flow, iterated every step, so a contiguous sorted vector beats std::map (GCC 14 has
// no std::flat_map yet; C++23 adds it, and this class is what it would replace). Sorted
// order also makes every run byte-identical (§3.6).
class Flow {
 public:
  struct Entry {
    OpName op;
    Rate rate;
  };
  void add(std::string_view op, Rate rate);               // accumulates
  [[nodiscard]] Rate rate_of(std::string_view op) const;  // Rate{0} when absent
  [[nodiscard]] Rate total() const;
  [[nodiscard]] Flow scaled(double factor) const;  // does not mutate
  [[nodiscard]] bool empty() const { return entries_.empty(); }
  // C++20 std::span<const T>: a non-owning read-only view of contiguous elements. Callers
  // iterate the entries without copying them and without seeing the vector type.
  [[nodiscard]] std::span<const Entry> entries() const { return entries_; }
  void clear() { entries_.clear(); }
  Flow& operator+=(const Flow& other);

 private:
  // C++20 std::ranges::lower_bound: takes the container itself instead of an iterator
  // pair, and a *projection* (`&Entry::op`) that says which member to compare, so we can
  // binary-search Entries by op name without writing a comparator lambda.
  [[nodiscard]] auto position_of(std::string_view op) const {
    return std::ranges::lower_bound(entries_, op, {}, &Entry::op);
  }
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
  const auto position = position_of(op);
  // position_of is const and yields a const_iterator; mutation needs a mutable one.
  const auto offset = position - entries_.cbegin();
  if (is_at(position, op)) {
    entries_[static_cast<std::size_t>(offset)].rate.per_second += rate.per_second;
  } else {
    entries_.insert(entries_.begin() + offset, Entry{std::string{op}, rate});
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

Flow Flow::scaled(double factor) const {
  Flow result = *this;
  for (auto& entry : result.entries_) entry.rate.per_second *= factor;
  return result;
}

Flow& Flow::operator+=(const Flow& other) {
  for (const auto& entry : other.entries_) add(entry.op, entry.rate);
  return *this;
}

}  // namespace simsys::flow
