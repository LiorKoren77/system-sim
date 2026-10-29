// Properties of the unit parsers that scenario files rely on.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cmath>
#include <string_view>
#include <type_traits>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.core;

using namespace std::chrono_literals;

namespace {

// Asserts that `parse` failed with exactly `message`, and that the error carries the
// whole original text so a scenario author can find the offending value.
template <typename Result>
void require_rejected(const Result& result, std::string_view input, std::string_view message) {
  CAPTURE(input);  // a failing assertion below then names the offending input
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message == message);
  REQUIRE(result.error().input == input);
}

}  // namespace

TEST_CASE("parse_duration understands every unit used in scenario files") {
  REQUIRE(simsys::parse_duration("500ns").value() == 500ns);
  REQUIRE(simsys::parse_duration("150us").value() == 150us);
  REQUIRE(simsys::parse_duration("0.5ms").value() == 500us);
  REQUIRE(simsys::parse_duration("2s").value() == 2s);
  REQUIRE(simsys::parse_duration("5m").value() == 5min);
  REQUIRE(simsys::parse_duration("24h").value() == 24h);
}

// A day, for a message queue's retention: Kafka's default of 168 hours is written `7d`, and a
// fraction of a day is hours: 1.5d = 36 h.
TEST_CASE("parse_duration reads days: 7d is Kafka's 168 hours") {
  REQUIRE(simsys::parse_duration("7d").value() == 168h);
  REQUIRE(simsys::parse_duration("1d").value() == 24h);
  REQUIRE(simsys::parse_duration("1.5d").value() == 36h);
}

TEST_CASE("parse_duration rejects unknown units and non-numbers with a structured error") {
  require_rejected(simsys::parse_duration("5 parsecs"), "5 parsecs",
                   "unknown duration unit ' parsecs'");
  require_rejected(simsys::parse_duration("ms"), "ms", "expected a number");
}

TEST_CASE("parse_duration rejects negative, NaN and infinite values") {
  require_rejected(simsys::parse_duration("-5ms"), "-5ms", "expected a non-negative finite number");
  require_rejected(simsys::parse_duration("nanms"), "nanms",
                   "expected a non-negative finite number");
  require_rejected(simsys::parse_duration("infms"), "infms",
                   "expected a non-negative finite number");
}

TEST_CASE("parse_duration rejects values that overflow the nanosecond representation") {
  require_rejected(simsys::parse_duration("999999999999h"), "999999999999h",
                   "duration out of range");
  require_rejected(simsys::parse_duration("9223372036854775807ns"), "9223372036854775807ns",
                   "duration out of range");
}

TEST_CASE("parse_duration rejects non-zero values below the 1ns resolution but accepts zero") {
  require_rejected(simsys::parse_duration("0.4ns"), "0.4ns", "duration below 1ns resolution");
  REQUIRE(simsys::parse_duration("0ns").value() == 0ns);
  REQUIRE(simsys::parse_duration("0ms").value() == 0ns);
}

// Error messages quote durations back to the scenario author, so they must read the way a
// scenario writes them and parse back to the same value.
TEST_CASE("a duration is described in the largest unit that divides it exactly") {
  REQUIRE(simsys::describe_duration(100ms) == "100ms");
  REQUIRE(simsys::describe_duration(1500ms) == "1500ms");
  REQUIRE(simsys::describe_duration(1s) == "1s");
  REQUIRE(simsys::describe_duration(2min) == "2m");
  REQUIRE(simsys::describe_duration(250us) == "250us");
  REQUIRE(simsys::describe_duration(0ns) == "0s");
  REQUIRE(simsys::parse_duration(simsys::describe_duration(1500ms)).value() == 1500ms);
}

TEST_CASE("parse_duration requires a unit") {
  require_rejected(simsys::parse_duration("5"), "5", "missing duration unit");
}

TEST_CASE("parse_duration rejects numbers too large for a double") {
  require_rejected(simsys::parse_duration("1e400ms"), "1e400ms", "number out of range");
}

TEST_CASE("parse_rate understands plain, k, M and G suffixes with a /s denominator") {
  REQUIRE(simsys::parse_rate("300/s").value().per_second == 300.0);
  REQUIRE(simsys::parse_rate("10k/s").value().per_second == 10'000.0);
  REQUIRE(simsys::parse_rate("1.5M/s").value().per_second == 1'500'000.0);
  REQUIRE(simsys::parse_rate("2G/s").value().per_second == 2'000'000'000.0);
}

TEST_CASE("parse_rate rejects negative, NaN and infinite values") {
  require_rejected(simsys::parse_rate("-5/s"), "-5/s", "expected a non-negative finite number");
  require_rejected(simsys::parse_rate("nan/s"), "nan/s", "expected a non-negative finite number");
  require_rejected(simsys::parse_rate("inf/s"), "inf/s", "expected a non-negative finite number");
}

TEST_CASE("parse_rate requires the /s denominator and a known suffix") {
  require_rejected(simsys::parse_rate("10k"), "10k", "rate must end with '/s'");
  require_rejected(simsys::parse_rate("5x/s"), "5x/s", "unknown prefix 'x' (expected k, M, G)");
}

TEST_CASE("parse_rate does not accept byte units; those belong to parse_byte_rate") {
  require_rejected(simsys::parse_rate("500MB/s"), "500MB/s",
                   "unknown prefix 'MB' (expected k, M, G)");
}

TEST_CASE("parse_rate reports a missing number against the full original text") {
  require_rejected(simsys::parse_rate("B/s"), "B/s", "expected a number");
  require_rejected(simsys::parse_rate("/s"), "/s", "expected a number");
}

TEST_CASE("Rate is comparable so capacities can be ordered in reports") {
  REQUIRE(simsys::Rate{1.0} < simsys::Rate{2.0});
  REQUIRE(simsys::Rate{3.0} == simsys::Rate{3.0});
}

TEST_CASE("parse_byte_rate understands B, KB, MB and GB suffixes with a /s denominator") {
  REQUIRE(simsys::parse_byte_rate("200B/s").value().bytes_per_second == 200.0);
  REQUIRE(simsys::parse_byte_rate("500KB/s").value().bytes_per_second == 500e3);
  REQUIRE(simsys::parse_byte_rate("500MB/s").value().bytes_per_second == 500e6);
  REQUIRE(simsys::parse_byte_rate("1GB/s").value().bytes_per_second == 1e9);
  REQUIRE(simsys::parse_byte_rate("1.5MB/s").value().bytes_per_second == 1.5e6);
}

TEST_CASE("parse_byte_rate requires the B/s ending so request rates cannot be mistaken for it") {
  require_rejected(simsys::parse_byte_rate("10k/s"), "10k/s", "byte rate must end with 'B/s'");
  require_rejected(simsys::parse_byte_rate("10kB"), "10kB", "byte rate must end with 'B/s'");
}

TEST_CASE("parse_byte_rate rejects unknown prefixes and negative values") {
  require_rejected(simsys::parse_byte_rate("5xB/s"), "5xB/s",
                   "unknown prefix 'x' (expected K, M, G)");
  require_rejected(simsys::parse_byte_rate("-1MB/s"), "-1MB/s",
                   "expected a non-negative finite number");
}

TEST_CASE("ByteRate is comparable so link capacities can be ordered in reports") {
  REQUIRE(simsys::ByteRate{1.0} < simsys::ByteRate{2.0});
}

TEST_CASE("parse_duration rounds half away from zero at the nanosecond boundary") {
  REQUIRE(simsys::parse_duration("0.5ns").value() == 1ns);
}

TEST_CASE("parse_duration accepts a value just inside the int64 nanosecond range") {
  REQUIRE(simsys::parse_duration("1e18ns").value() == simsys::SimTime{1'000'000'000'000'000'000});
}

TEST_CASE("parse_rate and parse_byte_rate reject values that overflow to infinity after scaling") {
  require_rejected(simsys::parse_rate("1e308G/s"), "1e308G/s", "rate out of range");
  require_rejected(simsys::parse_byte_rate("1e308GB/s"), "1e308GB/s", "rate out of range");
}

TEST_CASE("parse_rate normalises negative zero so a zero rate never carries a sign bit") {
  const auto rate = simsys::parse_rate("-0/s").value().per_second;
  REQUIRE(rate == 0.0);
  REQUIRE_FALSE(std::signbit(rate));
}

TEST_CASE("parse_rate and parse_byte_rate accept zero") {
  REQUIRE(simsys::parse_rate("0/s").value().per_second == 0.0);
  REQUIRE(simsys::parse_byte_rate("0B/s").value().bytes_per_second == 0.0);
}

TEST_CASE("prefix errors list the prefixes valid for that unit, so k versus K is discoverable") {
  require_rejected(simsys::parse_byte_rate("1kB/s"), "1kB/s",
                   "unknown prefix 'k' (expected K, M, G)");
  require_rejected(simsys::parse_rate("10K/s"), "10K/s", "unknown prefix 'K' (expected k, M, G)");
  require_rejected(simsys::parse_byte_rate("B/s"), "B/s", "expected a number");
}

TEST_CASE("seconds and from_seconds round-trip SimTime through double") {
  REQUIRE(simsys::seconds(1500ms) == Catch::Approx(1.5));
  REQUIRE(simsys::from_seconds(0.0001) == 100us);
  REQUIRE(simsys::seconds(simsys::from_seconds(2.5)) == Catch::Approx(2.5));
}

TEST_CASE("from_seconds rounds half to even, unlike parse_duration's half away from zero") {
  REQUIRE(simsys::from_seconds(2.5e-9) == 2ns);
  REQUIRE(simsys::parse_duration("2.5ns").value() == 3ns);
}

// The literals are consteval, so an out-of-range one (`9223372036854775808_ns`, or
// `9223372037_s`, whose nanoseconds exceed int64) is a compile error, not a runtime
// failure: it cannot be a TEST_CASE. The same holds for `153722868_min` and `2562048_h`
// (int64 nanoseconds end at about 153722867 minutes, or 2562047 hours, ~292 years).
// Uncomment one of those to see the diagnostic.
TEST_CASE("simsys time literals equal their std::chrono counterparts") {
  using namespace simsys::literals;
  static_assert(250_us == 250us);
  static_assert(150_ms == 150ms);
  static_assert(2_s == 2s);
  REQUIRE(7_ns == 7ns);
}

TEST_CASE("parse_clock_time reads HH:MM as an offset from midnight") {
  // Simulated time starts at 00:00 (spec §17 entry 1), so a clock time is a duration.
  REQUIRE(simsys::parse_clock_time("00:00").value() == 0s);
  REQUIRE(simsys::parse_clock_time("20:00").value() == 20h);
  REQUIRE(simsys::parse_clock_time("09:30").value() == 9h + 30min);
  REQUIRE(simsys::parse_clock_time("23:59").value() == 23h + 59min);
}

TEST_CASE("parse_clock_time rejects malformed and out-of-range times with a structured error") {
  require_rejected(simsys::parse_clock_time("24:00"), "24:00", "hour must be 00-23");
  require_rejected(simsys::parse_clock_time("12:60"), "12:60", "minute must be 00-59");
  require_rejected(simsys::parse_clock_time("1200"), "1200", "expected HH:MM");
  require_rejected(simsys::parse_clock_time("12:0"), "12:0", "expected HH:MM");
  require_rejected(simsys::parse_clock_time("ab:cd"), "ab:cd", "expected HH:MM");
}

TEST_CASE("parse_time_point accepts a clock time or a duration") {
  // `at:` and `peak_at:` take either form; both yield an offset from midnight (t = 0).
  REQUIRE(simsys::parse_time_point("12:00").value() == 12h);
  REQUIRE(simsys::parse_time_point("5m").value() == 5min);
  require_rejected(simsys::parse_time_point("noon"), "noon", "expected HH:MM or a duration");
}

TEST_CASE("parse_time_point keeps the clock-time range message for HH:MM-shaped text") {
  require_rejected(simsys::parse_time_point("25:00"), "25:00", "hour must be 00-23");
  require_rejected(simsys::parse_time_point("12:60"), "12:60", "minute must be 00-59");
}

TEST_CASE("simsys minute and hour literals equal their std::chrono counterparts") {
  using namespace simsys::literals;
  static_assert(5_min == 5min);
  static_assert(24_h == 24h);
  SUCCEED();
}

// ---- Request sizes (phase 2c) -------------------------------------------------------
//
// `Bytes` is a size in bytes, a strong type beside Rate and ByteRate (spec section 4): a size
// cannot be passed where a rate is expected. `parse_size` reads the sizes a client's `sizes:`
// and a step's `size:` state. Decimal units, as the byte-rate grammar: 1 KB = 1000 B.

namespace {

// Asserts that `parse_size(input)` failed, that the error carries the whole original text and
// that its message states the grammar, so a scenario author who wrote `10` or `1KiB` learns
// what to write. The wording around the grammar is the implementer's; the grammar is pinned.
void require_size_grammar_error(std::string_view input) {
  CAPTURE(input);
  const auto parsed = simsys::parse_size(input);
  REQUIRE_FALSE(parsed.has_value());
  REQUIRE(parsed.error().input == input);
  REQUIRE_THAT(parsed.error().message, Catch::Matchers::ContainsSubstring("B, KB, MB or GB"));
}

}  // namespace

TEST_CASE("parse_size reads bytes, kilobytes, megabytes and gigabytes as decimal units") {
  // 1 KB = 1000 B (decimal, as parse_byte_rate), so each value is the number times its power of
  // ten: 200 B; 2 x 1e3; 50 x 1e6; 1.5 x 1e9.
  REQUIRE_THAT(simsys::parse_size("200B").value().count, Catch::Matchers::WithinRel(200.0, 1e-12));
  REQUIRE_THAT(simsys::parse_size("2KB").value().count, Catch::Matchers::WithinRel(2e3, 1e-12));
  REQUIRE_THAT(simsys::parse_size("50MB").value().count, Catch::Matchers::WithinRel(5e7, 1e-12));
  REQUIRE_THAT(simsys::parse_size("1.5GB").value().count, Catch::Matchers::WithinRel(1.5e9, 1e-12));
}

TEST_CASE("parse_size accepts a size up to 1e15 bytes, the same ceiling as a byte rate") {
  REQUIRE_THAT(simsys::parse_size("1e15B").value().count, Catch::Matchers::WithinRel(1e15, 1e-12));
}

TEST_CASE("parse_size refuses a number with no unit and says which units exist") {
  require_size_grammar_error("10");
}

TEST_CASE("parse_size refuses a wrong-case or binary unit and says which units exist") {
  require_size_grammar_error("2kB");   // the kilo prefix is K, as in 500KB/s
  require_size_grammar_error("1KiB");  // binary units are not part of the grammar
}

TEST_CASE("parse_size refuses a negative size and says which units exist") {
  require_size_grammar_error("-1MB");
}

TEST_CASE("parse_size refuses a size above 1e15 bytes and names the limit") {
  require_size_grammar_error("1e16B");  // 1e16 B is ten times the ceiling of 1e15 B
  REQUIRE_THAT(simsys::parse_size("1e16B").error().message,
               Catch::Matchers::ContainsSubstring("1e15"));
}

TEST_CASE("Bytes is a strong type: ordered by its size and not convertible from a rate") {
  REQUIRE(simsys::Bytes{1.0} < simsys::Bytes{2.0});
  REQUIRE(simsys::Bytes{3.0} == simsys::Bytes{3.0});
  // A size is not a rate and not a byte rate: neither converts to Bytes, and a bare double
  // does not either, so `sizes:` can never be fed a throughput by mistake.
  static_assert(!std::is_convertible_v<simsys::Rate, simsys::Bytes>);
  static_assert(!std::is_convertible_v<simsys::ByteRate, simsys::Bytes>);
  static_assert(!std::is_convertible_v<double, simsys::Bytes>);
  SUCCEED();
}

TEST_CASE("parse_byte_rate reads network bit rates and converts them to bytes per second") {
  // A network is rated in bits: bytes per second = bits per second / 8.
  //   1 Gbit/s   = 1e9   / 8 = 1.25e8 B/s
  //   40 Gbit/s  = 4e10  / 8 = 5e9    B/s
  //   100 Mbit/s = 1e8   / 8 = 1.25e7 B/s
  REQUIRE_THAT(simsys::parse_byte_rate("1Gbit/s").value().bytes_per_second,
               Catch::Matchers::WithinRel(1.25e8, 1e-12));
  REQUIRE_THAT(simsys::parse_byte_rate("40Gbit/s").value().bytes_per_second,
               Catch::Matchers::WithinRel(5e9, 1e-12));
  REQUIRE_THAT(simsys::parse_byte_rate("100Mbit/s").value().bytes_per_second,
               Catch::Matchers::WithinRel(1.25e7, 1e-12));
}

TEST_CASE("parse_byte_rate still reads 500MB/s as 5e8 bytes per second") {
  // 500 x 1e6 = 5e8: the byte spelling is unchanged by the bit spelling.
  REQUIRE_THAT(simsys::parse_byte_rate("500MB/s").value().bytes_per_second,
               Catch::Matchers::WithinRel(5e8, 1e-12));
}

TEST_CASE("parse_byte_rate refuses a bit rate without its per-second denominator") {
  const auto parsed = simsys::parse_byte_rate("1Gbit");
  REQUIRE_FALSE(parsed.has_value());
  REQUIRE(parsed.error().input == "1Gbit");
  REQUIRE_THAT(parsed.error().message, Catch::Matchers::ContainsSubstring("must end with"));
}

// ---- format_size and format_byte_rate: sizes read back in the scenario's grammar ----

TEST_CASE("format_size picks the largest unit that keeps the number at least 1") {
  REQUIRE(simsys::format_size(simsys::Bytes{0.0}) == "0B");
  REQUIRE(simsys::format_size(simsys::Bytes{0.5}) == "0.5B");  // below a byte: still bytes
  REQUIRE(simsys::format_size(simsys::Bytes{125e6}) == "125MB");
  // 1e15 B, the ceiling, is a million gigabytes: GB is the largest unit, in fixed notation.
  REQUIRE(simsys::format_size(simsys::Bytes{1e15}) == "1000000GB");
}

TEST_CASE("format_size round-trips through parse_size") {
  for (const double bytes : {0.0, 0.5, 200.0, 2e3, 1.5e9, 125e6, 5e7, 3e8, 123456789.0, 1e15}) {
    CAPTURE(bytes);
    const auto text = simsys::format_size(simsys::Bytes{bytes});
    const auto parsed = simsys::parse_size(text);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->count == bytes);  // shortest round-trip digits: exact
  }
}

TEST_CASE("parse_size accepts 0B: a size, not a request, so it is no error here") {
  // The loader refuses a zero request size (`sizes:`, a step's `size:`); the grammar does not.
  REQUIRE(simsys::parse_size("0B").value().count == 0.0);
}

TEST_CASE("format_byte_rate never prints an exponent, however large or small the rate") {
  // An author never writes 1e+291GB/s; explain and messages print what the grammar reads.
  for (const double bytes_per_second : {1e15, 1e300, 1e-100, 5e-324}) {
    CAPTURE(bytes_per_second);
    const auto text = simsys::format_byte_rate(simsys::ByteRate{bytes_per_second});
    CAPTURE(text);
    // C++23 std::string::contains and C++20 ends_with: yes/no questions about the text,
    // replacing `find('e') != npos` and a compare() at size() - 3, which read as index
    // arithmetic when all the test asks is whether the character or suffix is there.
    REQUIRE_FALSE(text.contains('e'));
    REQUIRE(text.ends_with("B/s"));
  }
  REQUIRE(simsys::format_byte_rate(simsys::ByteRate{5e9}) == "5GB/s");  // 40 Gbit/s
}
