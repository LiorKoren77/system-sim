// Module `simsys.core`: the vocabulary types every other layer uses — simulated time,
// rates, and the parsers that turn scenario strings like "0.5ms" or "10k/s" into them.
//
// C++20 named modules. This file is a *module interface unit*. Everything after
// `export module simsys.core;` is the module's purview; only declarations marked
// `export` are visible to importers. Compared with headers: no include guards, no
// macro leakage, one compilation of the interface instead of one per includer.
// The `module;` line opens the *global module fragment*, the only place a module
// unit may #include traditional headers (the standard library is still header-based
// in GCC 14).
module;
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <expected>
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

// Requests (or bytes, messages...) per second. A struct, not a bare double, so a rate
// cannot be passed where a latency is expected. It deliberately carries no unit: the
// node type decides whether it counts requests or bytes.
struct Rate {
  double per_second;
  // C++20 defaulted three-way comparison: one line gives ==, !=, <, <=, >, >= that
  // compare members in declaration order. Before C++20 this was six hand-written
  // operators that silently went stale when a member was added.
  auto operator<=>(const Rate&) const = default;
};

struct ParseError {
  std::string input;
  std::string message;
};

namespace detail {

// C++23 std::expected<T, E>: a value-or-error return type. Replaces the old choice
// between exceptions and out-parameters/error codes. The caller must inspect it, and
// errors carry structured data (here: the offending input and a message) that the
// config loader can decorate with file, line and column.
//
// Splits "12.5ms" into (12.5, "ms"). std::from_chars (C++17) is locale-independent and
// never allocates, which is what a parser called thousands of times wants.
inline std::expected<std::pair<double, std::string_view>, ParseError> split_number(
    std::string_view text) {
  double value{};
  const auto* begin = text.data();
  const auto* end = begin + text.size();
  auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr == begin) {
    // C++23 std::unexpected<E>: wraps an error so it can be returned through a
    // std::expected<T, E>; the caller gets `.error()` instead of an exception.
    return std::unexpected(ParseError{std::string{text}, "expected a number"});
  }
  return std::pair{value, text.substr(static_cast<std::size_t>(ptr - begin))};
}

// How many nanoseconds one `unit` lasts. `input` is the whole string, kept for the error.
inline std::expected<double, ParseError> nanoseconds_per(std::string_view unit,
                                                         std::string_view input) {
  if (unit == "ns") return 1.0;
  if (unit == "us") return 1e3;
  if (unit == "ms") return 1e6;
  if (unit == "s") return 1e9;
  if (unit == "m") return 60e9;
  if (unit == "h") return 3600e9;
  return std::unexpected(
      ParseError{std::string{input}, "unknown duration unit '" + std::string{unit} + "'"});
}

// What an SI-style rate suffix multiplies by; an empty suffix means "as is".
inline std::expected<double, ParseError> multiplier_for(std::string_view suffix,
                                                        std::string_view input) {
  if (suffix.empty()) return 1.0;
  if (suffix == "k") return 1e3;
  if (suffix == "M") return 1e6;
  if (suffix == "G") return 1e9;
  return std::unexpected(
      ParseError{std::string{input}, "unknown rate suffix '" + std::string{suffix} + "'"});
}

}  // namespace detail

// Three steps: split the number from its unit, look the unit up, combine.
std::expected<SimTime, ParseError> parse_duration(std::string_view text) {
  auto split = detail::split_number(text);
  if (!split) return std::unexpected(split.error());
  // C++17 structured binding; still the clearest way to unpack a pair.
  auto [value, unit] = *split;

  auto scale = detail::nanoseconds_per(unit, text);
  if (!scale) return std::unexpected(scale.error());

  return SimTime{std::llround(value * *scale)};
}

// Same three steps as parse_duration, after checking the "/s" denominator.
std::expected<Rate, ParseError> parse_rate(std::string_view text) {
  constexpr std::string_view denominator = "/s";
  // C++20 std::string_view::ends_with: no more size arithmetic to check a suffix.
  if (!text.ends_with(denominator)) {
    return std::unexpected(ParseError{std::string{text}, "rate must end with '/s'"});
  }
  auto split = detail::split_number(text.substr(0, text.size() - denominator.size()));
  if (!split) return std::unexpected(split.error());
  auto [value, suffix] = *split;
  // Byte rates ("500MB/s") share the grammar: strip a trailing 'B'. A Rate carries no
  // unit on purpose; the node type decides whether it counts requests or bytes.
  if (suffix.ends_with('B')) suffix.remove_suffix(1);

  auto multiplier = detail::multiplier_for(suffix, text);
  if (!multiplier) return std::unexpected(multiplier.error());

  return Rate{value * *multiplier};
}

}  // namespace simsys
