// Properties of the unit parsers that scenario files rely on.
#include <catch2/catch_test_macros.hpp>

#include <chrono>

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
  require_rejected(simsys::parse_rate("5x/s"), "5x/s", "unknown rate suffix 'x'");
}

TEST_CASE("parse_rate does not accept byte units; those belong to parse_byte_rate") {
  require_rejected(simsys::parse_rate("500MB/s"), "500MB/s", "unknown rate suffix 'MB'");
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
  require_rejected(simsys::parse_byte_rate("5xB/s"), "5xB/s", "unknown rate suffix 'x'");
  require_rejected(simsys::parse_byte_rate("-1MB/s"), "-1MB/s",
                   "expected a non-negative finite number");
}

TEST_CASE("ByteRate is comparable so link capacities can be ordered in reports") {
  REQUIRE(simsys::ByteRate{1.0} < simsys::ByteRate{2.0});
}
