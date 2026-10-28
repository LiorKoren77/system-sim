// Properties of the scenario loader: what a valid file produces and how every kind of
// mistake in a scenario file is reported (message, file and line).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.config;

using namespace std::chrono_literals;

namespace {

// Writes `yaml` to a file in the system temp directory and returns its path, so the
// loader is exercised through its real entry point (a file path), not a string shortcut.
std::filesystem::path write_temp(std::string_view name, std::string_view yaml) {
  const auto path = std::filesystem::temp_directory_path() / name;
  std::ofstream out(path);
  out << yaml;
  return path;
}

// Loads `yaml` and asserts it is rejected with exactly `message`, attributed to the file.
// C++23 std::expected<T, E> (the loader's return type) is a value-or-error result:
// `has_value()` tells which one it holds and `error()` gives the structured ConfigError.
void require_rejected(std::string_view file_name, std::string_view yaml, std::string_view message) {
  CAPTURE(yaml);  // a failing assertion below then shows the offending scenario
  const auto path = write_temp(file_name, yaml);
  const auto s = simsys::config::load(path);
  REQUIRE_FALSE(s.has_value());
  REQUIRE(s.error().message == message);
  REQUIRE(s.error().file == path.string());
}

}  // namespace

TEST_CASE("a minimal scenario loads with its nodes and links in document order") {
  const auto path = write_temp("simsys_minimal.yaml", R"(
version: 1
name: minimal
duration: 1m
nodes:
  users:
    type: client
    rate: {base: 100/s}
    ops: {ping: 1.0}
  api:
    type: service
    capacity: 1k/s
    latency: 1ms
links:
  - {from: users, to: api, dir: inout}
)");
  const auto s = simsys::config::load(path);
  REQUIRE(s.has_value());
  REQUIRE(s->name == "minimal");
  REQUIRE(s->duration == 1min);
  REQUIRE(s->nodes.size() == 2);
  REQUIRE(s->nodes[0].id == "users");
  REQUIRE(s->nodes[1].type == "service");
  REQUIRE(s->links.size() == 1);
  REQUIRE(s->links[0].from == "users");
}

TEST_CASE("a byte-rate capacity is accepted") {
  const auto path = write_temp("simsys_byte_rate.yaml", R"(
version: 1
name: bytes
duration: 1m
nodes:
  store:
    type: blob_store
    capacity: 500MB/s
links: []
)");
  REQUIRE(simsys::config::load(path).has_value());
}

TEST_CASE("a link to an undeclared node is reported with its line") {
  // Line 1 is the empty line right after R"( ; `- {from: users, to: apii}` is on line 12.
  const auto path = write_temp("simsys_bad_link.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  users:
    type: client
  api:
    type: service
    capacity: 1k/s
links:
  - {from: users, to: apii}
)");
  const auto s = simsys::config::load(path);
  REQUIRE_FALSE(s.has_value());
  REQUIRE(s.error().message == "link refers to unknown node 'apii'");
  REQUIRE(s.error().file == path.string());
  REQUIRE(s.error().line == 12);
}

TEST_CASE("an unknown node type is rejected") {
  require_rejected("simsys_unknown_type.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  thing: {type: quantum_db}
links: []
)",
                   "unknown node type 'quantum_db'");
}

TEST_CASE("a node without a type is rejected") {
  require_rejected("simsys_no_type.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  thing: {capacity: 1k/s}
links: []
)",
                   "node 'thing' needs a type");
}

TEST_CASE("a step pointing at an undeclared node is rejected") {
  require_rejected("simsys_bad_step.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  api:
    type: service
    capacity: 1k/s
    ops: {read: [{to: cache, op: read}]}
links: []
)",
                   "step refers to unknown node 'cache'");
}

TEST_CASE("a then-step pointing at an undeclared node is rejected") {
  require_rejected("simsys_bad_then.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  queue: {type: message_queue}
  workers:
    type: worker_pool
    consumes: queue
    then: [{to: nowhere, op: write}]
links: []
)",
                   "step refers to unknown node 'nowhere'");
}

TEST_CASE("consumes, backend and origin must name declared nodes") {
  SECTION("consumes") {
    require_rejected("simsys_bad_consumes.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  workers: {type: worker_pool, consumes: ghost}
links: []
)",
                     "consumes refers to unknown node 'ghost'");
  }
  SECTION("backend") {
    require_rejected("simsys_bad_backend.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  front: {type: cache, backend: ghost}
links: []
)",
                     "backend refers to unknown node 'ghost'");
  }
  SECTION("origin") {
    require_rejected("simsys_bad_origin.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  edge: {type: cdn, origin: ghost}
links: []
)",
                     "origin refers to unknown node 'ghost'");
  }
}

TEST_CASE("a malformed duration is rejected with the unit parser's message") {
  require_rejected("simsys_bad_duration.yaml", R"(
version: 1
name: t
duration: 1 fortnight
nodes:
  api: {type: service, capacity: 1k/s}
links: []
)",
                   "duration: unknown duration unit ' fortnight'");
}

TEST_CASE("a malformed capacity is rejected with the unit parser's message") {
  require_rejected("simsys_bad_capacity.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  api: {type: service, capacity: fast}
links: []
)",
                   "capacity: expected a number");
}

TEST_CASE("a missing top-level key is reported") {
  require_rejected("simsys_no_links.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  api: {type: service, capacity: 1k/s}
)",
                   "missing top-level key 'links'");
}

TEST_CASE("version other than 1 is rejected") {
  require_rejected("simsys_version2.yaml", R"(
version: 2
name: t
duration: 1m
nodes:
  api: {type: service, capacity: 1k/s}
links: []
)",
                   "version must be 1");
}

TEST_CASE("invalid YAML syntax is reported as a config error, not an exception") {
  const auto path = write_temp("simsys_broken.yaml", "nodes: [unclosed");
  const auto s = simsys::config::load(path);
  REQUIRE_FALSE(s.has_value());
  REQUIRE(s.error().file == path.string());
  // C++20 std::string::ends_with: replaces the compare(size - n, n, suffix) idiom.
  REQUIRE(s.error().file.ends_with("simsys_broken.yaml"));
}
