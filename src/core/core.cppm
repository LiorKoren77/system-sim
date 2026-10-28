// Module `simsys.core`: the vocabulary types every other layer uses — simulated time,
// rates, and the parsers that turn scenario strings like "0.5ms" or "10k/s" into them.
//
// C++20 named modules. This file is a *module interface unit*. Everything after
// `export module simsys.core;` is the module's purview; only declarations marked
// `export` are visible to importers. Compared with headers: no include guards, no
// macro leakage, one compilation of the interface instead of one per includer.
// The `module;` line opens the *global module fragment*. An #include inside the purview
// would be legal but would attach the header's declarations to this module, which is
// wrong for std headers (they belong to the global module and must stay shareable with
// other translation units), so std headers are included here instead.
module;
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

export module simsys.core;

export namespace simsys {

inline constexpr std::string_view version = "0.0.1";

// Simulated time. A std::chrono duration rather than a double so that seconds and
// milliseconds cannot be mixed by accident; integer nanoseconds give exact arithmetic
// (a double would drift after millions of additions).
using SimTime = std::chrono::duration<std::int64_t, std::nano>;

// Conversions to/from seconds as double, for arithmetic in the engine. All other code
// stores time as SimTime; doubles appear only inside formulas.
[[nodiscard]] constexpr double seconds(SimTime t) {
  return std::chrono::duration<double>(t).count();
}
// Rounds to the nearest nanosecond (std::chrono::round, C++17, constexpr), the same
// resolution parse_duration uses; a plain duration_cast would truncate and bias every
// step low. Ties go to EVEN (2.5ns -> 2ns), unlike parse_duration's half away from zero
// (std::llround: "2.5ns" -> 3ns). NaN, infinite or out-of-int64-range input is undefined
// behaviour (the double -> int64 conversion overflows); callers pass engine-computed,
// finite step lengths.
[[nodiscard]] constexpr SimTime from_seconds(double s) {
  return std::chrono::round<SimTime>(std::chrono::duration<double>(s));
}

// A duration in the largest unit that divides it exactly, so it reads as a scenario writes
// it: 10s, 2m, 1500ms. Integer arithmetic on the tick count (nanoseconds) only, so the text
// is exact for any duration; std::to_string rather than std::format, because importers of
// this interface must not pull <format> into their own (CLAUDE.md §3).
[[nodiscard]] inline std::string describe_duration(SimTime t) {
  struct Unit {
    std::string_view name;
    std::int64_t ns;
  };
  constexpr std::array units{
      Unit{"h", 3'600'000'000'000}, Unit{"m", 60'000'000'000}, Unit{"s", 1'000'000'000},
      Unit{"ms", 1'000'000},        Unit{"us", 1'000},         Unit{"ns", 1}};
  const std::int64_t ticks = t.count();
  for (const auto& unit : units) {
    if (ticks != 0 && ticks % unit.ns == 0) {
      return std::to_string(ticks / unit.ns) + std::string{unit.name};
    }
  }
  return "0s";
}

}  // namespace simsys

// Not exported (same reason as the detail block below): the literals call it, but
// importers cannot name it.
namespace simsys::detail {
// Converts `v` units of `ns_per_unit` nanoseconds to SimTime. Rejects any value whose
// nanosecond count exceeds int64 max (for _ns: v itself; otherwise the product), so
// `9223372036854775808_ns` or `9223372037_s` fails to compile instead of wrapping.
consteval SimTime checked_literal(unsigned long long v, std::int64_t ns_per_unit) {
  constexpr auto max_ns = static_cast<unsigned long long>(std::numeric_limits<std::int64_t>::max());
  if (v > max_ns / static_cast<unsigned long long>(ns_per_unit)) {
    throw std::overflow_error{"time literal out of range"};
  }
  return SimTime{static_cast<std::int64_t>(v) * ns_per_unit};
}
}  // namespace simsys::detail

export namespace simsys {

// Time literals for other modules. `simsys.core` is the only interface unit allowed to
// include <chrono> (CLAUDE.md §3: GCC 14.2 corrupts the BMI of an interface that imports
// a module *and* includes <chrono>/<format>; verified 2026-10-28 with a three-file probe).
// Downstream interfaces write `250_us` instead of `250us`.
// C++20 consteval: an immediate function. It can only run at compile time, so a literal
// never costs anything at run time, and a `throw` reached inside it is a compile error.
// Replaces constexpr, which allows but does not force compile-time evaluation (a
// constexpr literal evaluated at run time would overflow int64 without a diagnostic).
// (C++11 user-defined literals; the inline namespace lets users opt in with one
// `using namespace simsys::literals;`, like std::chrono_literals.)
inline namespace literals {
consteval SimTime operator""_ns(unsigned long long v) {
  return detail::checked_literal(v, 1);
}
consteval SimTime operator""_us(unsigned long long v) {
  return detail::checked_literal(v, 1'000);
}
consteval SimTime operator""_ms(unsigned long long v) {
  return detail::checked_literal(v, 1'000'000);
}
consteval SimTime operator""_s(unsigned long long v) {
  return detail::checked_literal(v, 1'000'000'000);
}
consteval SimTime operator""_min(unsigned long long v) {
  return detail::checked_literal(v, 60'000'000'000);
}
consteval SimTime operator""_h(unsigned long long v) {
  return detail::checked_literal(v, 3'600'000'000'000);
}
}  // namespace literals

// Requests (or messages, operations...) per second. A struct, not a bare double, so a
// rate cannot be passed where a latency is expected. Byte throughput has its own type,
// ByteRate, so "10k requests/s" can never be wired into a link's bandwidth.
struct Rate {
  double per_second;
  // C++20 defaulted three-way comparison: one line gives ==, !=, <, <=, >, >= that
  // compare members in declaration order, replacing six hand-written operators. With
  // a `double` member, `auto` deduces std::partial_ordering (NaN is unordered with
  // everything), which is why the parsers reject NaN rather than let it into a Rate.
  auto operator<=>(const Rate&) const = default;
};

// Bytes per second: link bandwidth, disk throughput. Separate from Rate on purpose.
struct ByteRate {
  double bytes_per_second;
  auto operator<=>(const ByteRate&) const = default;
};

struct ParseError {
  std::string input;
  std::string message;
};

}  // namespace simsys

// Not exported: importers cannot name anything in simsys::detail. It sits between the
// two export blocks because it needs ParseError and the parsers below need it. It has to
// live outside the export blocks because everything inside an `export namespace` block is
// exported.
namespace simsys::detail {

// One row of a unit table: "ms" -> 1e6, "k" -> 1e3, ...
struct Scale {
  std::string_view suffix;
  double factor;
};

// Pattern: Table-driven method (Code Complete) — each grammar (duration units,
// request-rate prefixes, byte-rate prefixes) is a constant table searched by one
// function, so adding a unit is a one-line data change instead of another `if` branch.
constexpr std::array duration_units{Scale{"ns", 1.0}, Scale{"us", 1e3}, Scale{"ms", 1e6},
                                    Scale{"s", 1e9},  Scale{"m", 60e9}, Scale{"h", 3600e9}};
constexpr std::array rate_prefixes{Scale{"", 1.0}, Scale{"k", 1e3}, Scale{"M", 1e6},
                                   Scale{"G", 1e9}};
// Decimal SI prefixes, not binary: K = 1e3, not 1024. Capital K, matching how "KB" is
// written in scenario files; lowercase `k` is deliberately rejected for bytes (and
// capital `K` for request rates), and the error message says which spelling to use.
constexpr std::array byte_prefixes{Scale{"", 1.0}, Scale{"K", 1e3}, Scale{"M", 1e6},
                                   Scale{"G", 1e9}};

// C++23 std::unexpected<E>: wraps an error so it can be returned through a
// std::expected<T, E>; the caller gets `.error()` instead of an exception.
// [[nodiscard]]: libstdc++ 14 does not mark std::expected nodiscard, so without the
// attribute a caller that drops a parse result (and its error) would compile silently.
[[nodiscard]] std::unexpected<ParseError> fail(std::string_view input, std::string message) {
  return std::unexpected(ParseError{std::string{input}, std::move(message)});
}

// C++20 std::span<const T>: a non-owning view of contiguous elements. It lets one
// function accept tables of different lengths (std::array<Scale, 6> and <Scale, 4>)
// without a template or a pointer+size pair.
[[nodiscard]] std::optional<double> find_scale(std::span<const Scale> table,
                                               std::string_view suffix) {
  for (const auto& row : table) {
    if (row.suffix == suffix) return row.factor;
  }
  return std::nullopt;
}

// C++23 std::expected<T, E>: a value-or-error return type. Replaces the old choice
// between exceptions and out-parameters/error codes. The caller must inspect it, and
// errors carry structured data (here: the offending input and a message) that the
// config loader can decorate with file, line and column.
//
// Splits "12.5ms" into (12.5, "ms"). std::from_chars (C++17) is locale-independent and
// never allocates. `input` is the full original text, kept for the error.
[[nodiscard]] std::expected<std::pair<double, std::string_view>, ParseError> split_number(
    std::string_view text, std::string_view input) {
  double value{};
  const auto* begin = text.data();
  const auto* end = begin + text.size();
  // C++17 structured binding: unpacks the from_chars result (end pointer, error code)
  // into two named variables instead of `result.ptr` / `result.ec`.
  auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec == std::errc::result_out_of_range) return fail(input, "number out of range");
  if (ec != std::errc{} || ptr == begin) return fail(input, "expected a number");
  return std::pair{value, text.substr(static_cast<std::size_t>(ptr - begin))};
}

// from_chars happily accepts "-5", "nan" and "inf"; none is a valid time or rate.
[[nodiscard]] std::expected<double, ParseError> require_non_negative_finite(
    double value, std::string_view input) {
  if (!std::isfinite(value) || value < 0.0) {
    return fail(input, "expected a non-negative finite number");
  }
  return value;
}

// split_number + require_non_negative_finite: "12.5ms" -> (12.5, "ms"), or an error.
[[nodiscard]] std::expected<std::pair<double, std::string_view>, ParseError>
split_non_negative_number(std::string_view text, std::string_view input) {
  auto split = split_number(text, input);
  if (!split) return std::unexpected(split.error());
  auto checked = require_non_negative_finite(split->first, input);
  if (!checked) return std::unexpected(checked.error());
  return split;
}

// How many nanoseconds one `unit` lasts.
[[nodiscard]] std::expected<double, ParseError> nanoseconds_per(std::string_view unit,
                                                                std::string_view input) {
  if (unit.empty()) return fail(input, "missing duration unit");
  if (auto factor = find_scale(duration_units, unit)) return *factor;
  return fail(input, "unknown duration unit '" + std::string{unit} + "'");
}

// Rounds to whole nanoseconds, half away from zero (std::llround), so "0.5ns" is 1ns.
// Rejects what SimTime cannot hold: values at or beyond int64 max (llround would
// overflow). Of the values that round to 0ns, only exact zero is accepted; any
// positive value that rounds to 0 (below 0.5ns) is rejected.
[[nodiscard]] std::expected<SimTime, ParseError> to_sim_time(double nanoseconds,
                                                             std::string_view input) {
  // int64 -> double is lossy in general (2^63-1 becomes 2^63); the cast is explicit
  // and the >= keeps the comparison safe at exactly that boundary.
  constexpr auto limit = static_cast<double>(SimTime::max().count());
  if (!std::isfinite(nanoseconds) || nanoseconds >= limit) {
    return fail(input, "duration out of range");
  }
  const auto count = std::llround(nanoseconds);
  if (count == 0 && nanoseconds > 0.0) return fail(input, "duration below 1ns resolution");
  return SimTime{count};
}

// The prefixes a table accepts, for error messages: {"", k, M, G} -> "k, M, G". Derived
// from the table so the message cannot go stale when a prefix is added.
[[nodiscard]] std::string list_prefixes(std::span<const Scale> table) {
  std::string listed;
  for (const auto& row : table) {
    if (row.suffix.empty()) continue;
    if (!listed.empty()) listed += ", ";
    listed += row.suffix;
  }
  return listed;
}

// Shared by both rate parsers: parse `number_text` (the input with its denominator
// already stripped, e.g. "10k") and scale it by its prefix from `table`. `input` is the
// full original text, kept for errors.
[[nodiscard]] std::expected<double, ParseError> parse_scaled_number(std::string_view number_text,
                                                                    std::string_view input,
                                                                    std::span<const Scale> table) {
  auto split = split_non_negative_number(number_text, input);
  if (!split) return std::unexpected(split.error());
  auto [value, prefix] = *split;
  const auto factor = find_scale(table, prefix);
  if (!factor) {
    return fail(input, "unknown prefix '" + std::string{prefix} + "' (expected " +
                           list_prefixes(table) + ")");
  }
  // The finite check must come after scaling: 1e308 is finite, 1e308 * 1e9 is not.
  const double scaled = value * *factor;
  if (!std::isfinite(scaled)) return fail(input, "rate out of range");
  // `-0` passes the non-negative check (-0.0 < 0.0 is false); `+ 0.0` turns -0.0 into +0.0.
  return scaled + 0.0;
}

// "09" -> 9. Exactly two ASCII digits, nothing else. std::from_chars alone would also
// accept "-5" (a sign), so the digit check comes first; from_chars then does the
// conversion without locale lookups or allocation.
[[nodiscard]] std::optional<int> parse_two_digits(std::string_view pair) {
  const auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
  if (pair.size() != 2 || !is_digit(pair[0]) || !is_digit(pair[1])) return std::nullopt;
  int value{};
  const auto [ptr, ec] = std::from_chars(pair.data(), pair.data() + pair.size(), value);
  if (ec != std::errc{}) return std::nullopt;
  return value;
}

}  // namespace simsys::detail

export namespace simsys {

// Three steps: split off a non-negative number, look the unit up, convert to SimTime.
// Parsing goes through double: above 2^53 ns (~104 days) values lose exactness, which
// is fine because scenarios run for at most days.
[[nodiscard]] std::expected<SimTime, ParseError> parse_duration(std::string_view text) {
  auto split = detail::split_non_negative_number(text, text);
  if (!split) return std::unexpected(split.error());
  auto [value, unit] = *split;
  auto scale = detail::nanoseconds_per(unit, text);
  if (!scale) return std::unexpected(scale.error());
  return detail::to_sim_time(value * *scale, text);
}

// "20:00" -> 20h. Exactly HH:MM, two digits each side of a colon. Simulated time starts at
// midnight (phase 2a refinement 1: t = 0 is 00:00), so a wall-clock time is simply an
// offset from the start of the run; scenario keys such as `at:` and `peak_at:` use it.
// std::chrono::hh_mm_ss exists for the opposite direction (splitting a duration into
// hours, minutes and seconds for display). std::chrono::parse("%H:%M") does exist in
// GCC 14's libstdc++ (verified with g++-14 -std=c++23), but it reads from a std::istream,
// which pulls in <sstream> and locale machinery, and its failure is a bare stream state
// bit, whereas the scenario author needs the exact messages below.
[[nodiscard]] std::expected<SimTime, ParseError> parse_clock_time(std::string_view text) {
  if (text.size() != 5 || text[2] != ':') return detail::fail(text, "expected HH:MM");
  const auto hours = detail::parse_two_digits(text.substr(0, 2));
  const auto minutes = detail::parse_two_digits(text.substr(3, 2));
  if (!hours || !minutes) return detail::fail(text, "expected HH:MM");
  if (*hours > 23) return detail::fail(text, "hour must be 00-23");
  if (*minutes > 59) return detail::fail(text, "minute must be 00-59");
  return *hours * 1_h + *minutes * 1_min;
}

// A point on the simulated timeline, in either of the two spellings scenario authors use:
// a clock time ("20:00") or a duration from the start ("90m"). Both are offsets from
// t = 0. Text with a colon was meant as a clock time, so it gets parse_clock_time's own,
// precise message ("hour must be 00-23"); any other text that is not a duration gets one
// combined message, so an author who wrote neither form learns about both.
[[nodiscard]] std::expected<SimTime, ParseError> parse_time_point(std::string_view text) {
  // C++23 std::string_view::contains: replaces `find(c) != npos`, which reads as
  // arithmetic on an index when all we want is a yes/no question.
  if (text.contains(':')) return parse_clock_time(text);
  auto parsed = parse_duration(text);
  if (!parsed) return detail::fail(text, "expected HH:MM or a duration");
  return parsed;
}

// "10k/s" -> Rate{10000}. Accepts only the SI prefixes "", k, M, G.
[[nodiscard]] std::expected<Rate, ParseError> parse_rate(std::string_view text) {
  // C++20 std::string_view::ends_with: no more size arithmetic to check a suffix.
  if (!text.ends_with("/s")) return detail::fail(text, "rate must end with '/s'");
  // C++23 std::expected::transform: applies a function to the value if there is one and
  // passes an error through untouched. It fits here because the last step (wrapping the
  // double in a Rate) cannot fail; steps that can fail still use the early-return pattern.
  const auto number_text = text.substr(0, text.size() - 2);  // drop "/s"
  return detail::parse_scaled_number(number_text, text, detail::rate_prefixes)
      .transform([](double v) { return Rate{v}; });
}

// "500MB/s" -> ByteRate{5e8}. Accepts the prefixes "", K, M, G before "B/s".
[[nodiscard]] std::expected<ByteRate, ParseError> parse_byte_rate(std::string_view text) {
  if (!text.ends_with("B/s")) return detail::fail(text, "byte rate must end with 'B/s'");
  const auto number_text = text.substr(0, text.size() - 3);  // drop "B/s"
  return detail::parse_scaled_number(number_text, text, detail::byte_prefixes)
      .transform([](double v) { return ByteRate{v}; });
}

}  // namespace simsys

// ---- Enum <-> word tables -------------------------------------------------------------
//
// Every enum the scenario schema spells as a word (node types, link directions, SLO
// metrics, step conditions and modes, wave shapes) is one constant table of rows
// {enumerator, word}, read in both directions by the two lookups below. The tables live
// next to their enums (simsys.model, simsys.workload); the machinery lives here, once.
// Pattern: Table-driven method — adding a word is one row of data, not another `if`.
export namespace simsys::core {

// One row of a name table: an enumerator and the word the scenario schema spells it with,
// side by side, so a row can never pair a word with the wrong enumerator.
template <typename E>
struct EnumName {
  E value;
  std::string_view name;
};

// Row i of `table` must hold the enumerator whose value is i, so a lookup by
// std::to_underlying is correct. consteval: a build-time proof, used in static_asserts.
template <typename E, std::size_t N>
[[nodiscard]] consteval bool follows_enum_order(const std::array<EnumName<E>, N>& table) {
  for (std::size_t i = 0; i < N; ++i) {
    // C++23 std::to_underlying: the enum's integer value without a static_cast that could
    // silently accept the wrong type (it always yields exactly the underlying type).
    // C++20 std::cmp_not_equal: compares integers of different signedness by value. An
    // int enum's value against a std::size_t index with plain != would convert the int to
    // unsigned first (-1 == SIZE_MAX) and warn under -Wsign-compare; this cannot.
    if (std::cmp_not_equal(std::to_underlying(table[i].value), i)) return false;
  }
  return true;
}

// The word for `value`, by index (see follows_enum_order). An enumerator without a row (a
// value cast from an integer) is a programmer error: the loader only produces cataloged
// values.
template <typename E, std::size_t N>
[[nodiscard]] constexpr std::string_view name_in(const std::array<EnumName<E>, N>& table, E value) {
  const auto index = std::to_underlying(value);
  // C++20 std::in_range<T>(x): "does x fit in T?", correct for any mix of signed and
  // unsigned types (a negative int does not fit in std::size_t). Replaces a hand-written
  // `x >= 0` test, which warns as always true for an unsigned underlying type.
  if (!std::in_range<std::size_t>(index) || static_cast<std::size_t>(index) >= N) {
    throw std::invalid_argument{"enumerator without a name"};
  }
  return table[static_cast<std::size_t>(index)].name;
}

// The inverse of name_in; std::nullopt for a word not in the table.
template <typename E, std::size_t N>
[[nodiscard]] constexpr std::optional<E> parse_in(const std::array<EnumName<E>, N>& table,
                                                  std::string_view name) {
  for (const auto& row : table) {
    if (row.name == name) return row.value;
  }
  return std::nullopt;
}

// The whole contract of a table, proven at compile time: every word is unique (else
// parse_in would find only the first), and every enumerator survives the round trip
// parse_in(name_in(v)) == v (which fails for a misordered table, since name_in indexes by
// value). C++20 consteval: an immediate function, evaluated only during compilation, so
// the proof costs nothing at run time and a broken table is a build error at its
// static_assert, next to the table (model.cppm, workload.cppm).
template <typename E, std::size_t N>
[[nodiscard]] consteval bool round_trips(const std::array<EnumName<E>, N>& table) {
  for (std::size_t i = 0; i < N; ++i) {
    for (std::size_t j = i + 1; j < N; ++j) {
      if (table[i].name == table[j].name) return false;
    }
  }
  if (!follows_enum_order(table)) return false;
  for (const auto& row : table) {
    if (parse_in(table, name_in(table, row.value)) != row.value) return false;
  }
  return true;
}

}  // namespace simsys::core
