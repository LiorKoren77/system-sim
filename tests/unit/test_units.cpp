// Properties of the unit parsers that scenario files rely on.
#include <catch2/catch_test_macros.hpp>

#include <chrono>

import simsys.core;

using namespace std::chrono_literals;

TEST_CASE("parse_duration understands every unit used in scenario files") {
  REQUIRE(simsys::parse_duration("500ns").value() == 500ns);
  REQUIRE(simsys::parse_duration("150us").value() == 150us);
  REQUIRE(simsys::parse_duration("0.5ms").value() == 500us);
  REQUIRE(simsys::parse_duration("2s").value() == 2s);
  REQUIRE(simsys::parse_duration("5m").value() == 5min);
  REQUIRE(simsys::parse_duration("24h").value() == 24h);
}

TEST_CASE("parse_duration rejects unknown units and non-numbers with a structured error") {
  auto bad_unit = simsys::parse_duration("5 parsecs");
  REQUIRE_FALSE(bad_unit.has_value());
  REQUIRE(bad_unit.error().message == "unknown duration unit ' parsecs'");

  auto not_a_number = simsys::parse_duration("ms");
  REQUIRE_FALSE(not_a_number.has_value());
  REQUIRE(not_a_number.error().message == "expected a number");
}

TEST_CASE("parse_rate understands plain, k, M and G suffixes with a /s denominator") {
  REQUIRE(simsys::parse_rate("300/s").value().per_second == 300.0);
  REQUIRE(simsys::parse_rate("10k/s").value().per_second == 10'000.0);
  REQUIRE(simsys::parse_rate("1.5M/s").value().per_second == 1'500'000.0);
  REQUIRE(simsys::parse_rate("2G/s").value().per_second == 2'000'000'000.0);
}

TEST_CASE("parse_rate accepts byte rates; a Rate carries no unit, the node type decides") {
  REQUIRE(simsys::parse_rate("500MB/s").value().per_second == 500e6);
  REQUIRE(simsys::parse_rate("1GB/s").value().per_second == 1e9);
  REQUIRE(simsys::parse_rate("200B/s").value().per_second == 200.0);
}

TEST_CASE("parse_rate requires the /s denominator and a known suffix") {
  auto no_denominator = simsys::parse_rate("10k");
  REQUIRE_FALSE(no_denominator.has_value());
  REQUIRE(no_denominator.error().message == "rate must end with '/s'");

  auto bad_suffix = simsys::parse_rate("5x/s");
  REQUIRE_FALSE(bad_suffix.has_value());
  REQUIRE(bad_suffix.error().message == "unknown rate suffix 'x'");
}

TEST_CASE("Rate is comparable so capacities can be ordered in reports") {
  REQUIRE(simsys::Rate{1.0} < simsys::Rate{2.0});
  REQUIRE(simsys::Rate{3.0} == simsys::Rate{3.0});
}
