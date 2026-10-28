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
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
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
// two export blocks because it needs ParseError and the parsers below need it.
namespace simsys::detail {

// One row of a unit table: "ms" -> 1e6, "k" -> 1e3, ...
struct Scale {
  std::string_view suffix;
  double factor;
};

// Pattern: table-driven lookup — each grammar (duration units, request-rate prefixes,
// byte-rate prefixes) is a constant table searched by one function, so adding a unit
// is a one-line data change instead of another `if` branch.
constexpr std::array duration_units{Scale{"ns", 1.0}, Scale{"us", 1e3}, Scale{"ms", 1e6},
                                    Scale{"s", 1e9},  Scale{"m", 60e9}, Scale{"h", 3600e9}};
constexpr std::array rate_prefixes{Scale{"", 1.0}, Scale{"k", 1e3}, Scale{"M", 1e6},
                                   Scale{"G", 1e9}};
// Capital K, matching how "KB" is written in scenario files.
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

// Rounds to whole nanoseconds, rejecting what SimTime cannot hold: values at or beyond
// int64 max (llround would overflow) and non-zero values that would round to 0ns.
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

// Shared by both rate parsers: strip `denominator` ("/s" or "B/s"), parse the number,
// scale it by its prefix from `table`. The caller has already checked the denominator.
[[nodiscard]] std::expected<double, ParseError> parse_scaled_number(std::string_view text,
                                                                    std::string_view denominator,
                                                                    std::span<const Scale> table) {
  auto split = split_non_negative_number(text.substr(0, text.size() - denominator.size()), text);
  if (!split) return std::unexpected(split.error());
  auto [value, prefix] = *split;
  if (auto factor = find_scale(table, prefix)) return value * *factor;
  return fail(text, "unknown rate suffix '" + std::string{prefix} + "'");
}

}  // namespace simsys::detail

export namespace simsys {

// Three steps: split off a non-negative number, look the unit up, convert to SimTime.
// Parsing goes through double: above 2^53 ns (~104 days) values lose exactness, which
// is fine because scenarios run for at most days.
[[nodiscard]] std::expected<SimTime, ParseError> parse_duration(std::string_view text) {
  auto split = detail::split_non_negative_number(text, text);
  if (!split) return std::unexpected(split.error());
  // C++17 structured binding; still the clearest way to unpack a pair.
  auto [value, unit] = *split;
  auto scale = detail::nanoseconds_per(unit, text);
  if (!scale) return std::unexpected(scale.error());
  return detail::to_sim_time(value * *scale, text);
}

// "10k/s" -> Rate{10000}. Accepts only the SI prefixes "", k, M, G.
[[nodiscard]] std::expected<Rate, ParseError> parse_rate(std::string_view text) {
  // C++20 std::string_view::ends_with: no more size arithmetic to check a suffix.
  if (!text.ends_with("/s")) return detail::fail(text, "rate must end with '/s'");
  // C++23 std::expected::transform: applies a function to the value if there is one and
  // passes an error through untouched — the `if (!x) return unexpected` dance in one call.
  return detail::parse_scaled_number(text, "/s", detail::rate_prefixes).transform([](double v) {
    return Rate{v};
  });
}

// "500MB/s" -> ByteRate{5e8}. Accepts the prefixes "", K, M, G before "B/s".
[[nodiscard]] std::expected<ByteRate, ParseError> parse_byte_rate(std::string_view text) {
  if (!text.ends_with("B/s")) return detail::fail(text, "byte rate must end with 'B/s'");
  return detail::parse_scaled_number(text, "B/s", detail::byte_prefixes).transform([](double v) {
    return ByteRate{v};
  });
}

}  // namespace simsys
