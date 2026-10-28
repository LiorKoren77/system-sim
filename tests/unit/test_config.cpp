// Properties of the scenario loader: what a valid file produces and how every kind of
// mistake in a scenario file is reported (message, file and line).
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <unistd.h>  // getpid

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.config;

using namespace std::chrono_literals;

namespace {

// Writes `yaml` to a file in the system temp directory and returns its path, so the
// loader is exercised through its real entry point (a file path), not a string shortcut.
// The process id is part of the file name so two test processes running at the same time
// (e.g. ctest -j) never overwrite each other's scenario files.
std::filesystem::path write_temp(std::string_view name, std::string_view yaml) {
  const auto unique_name = std::to_string(::getpid()) + "_" + std::string{name};
  const auto path = std::filesystem::temp_directory_path() / unique_name;
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
  // `  - {from: users, to: apii}`: 2 spaces, "- " (cols 3-4), "{from: users, " (cols 5-18),
  // "to: " (cols 19-22), so `apii` starts at 1-based column 23.
  CHECK(s.error().column == 23);
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
  api: {type: service, capacity: fast/s}
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
  CHECK(s.error().line >= 1);  // yaml-cpp's syntax-error position is reported, not dropped
}

TEST_CASE("nodes must be a non-empty map") {
  SECTION("an empty map") {
    require_rejected("simsys_nodes_empty.yaml", R"(
version: 1
name: t
duration: 1m
nodes: {}
links: []
)",
                     "nodes must be a non-empty map");
  }
  SECTION("a list") {
    require_rejected("simsys_nodes_list.yaml", R"(
version: 1
name: t
duration: 1m
nodes: [a, b]
links: []
)",
                     "nodes must be a non-empty map");
  }
}

TEST_CASE("links must be a list") {
  SECTION("a map") {
    require_rejected("simsys_links_map.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  api: {type: service, capacity: 1k/s}
links: {a: b}
)",
                     "links must be a list");
  }
  SECTION("null") {
    require_rejected("simsys_links_null.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  api: {type: service, capacity: 1k/s}
links:
)",
                     "links must be a list");
  }
}

TEST_CASE("the optional step and warmup durations are validated") {
  SECTION("step with an unknown unit") {
    require_rejected("simsys_bad_step_unit.yaml", R"(
version: 1
name: t
duration: 1m
step: 5 bananas
nodes:
  api: {type: service, capacity: 1k/s}
links: []
)",
                     "step: unknown duration unit ' bananas'");
  }
  SECTION("warmup with an unknown unit") {
    require_rejected("simsys_bad_warmup_unit.yaml", R"(
version: 1
name: t
duration: 1m
warmup: 1 fortnight
nodes:
  api: {type: service, capacity: 1k/s}
links: []
)",
                     "warmup: unknown duration unit ' fortnight'");
  }
  SECTION("a node latency that is not a number") {
    require_rejected("simsys_bad_node_latency.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  api: {type: service, capacity: 1k/s, latency: soon}
links: []
)",
                     "latency: expected a number");
  }
  SECTION("a link latency that is not a number") {
    require_rejected("simsys_bad_link_latency.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  users: {type: client}
  api: {type: service, capacity: 1k/s}
links:
  - {from: users, to: api, latency: never}
)",
                     "latency: expected a number");
  }
}

TEST_CASE("well-formed optional durations load") {
  const auto path = write_temp("simsys_good_durations.yaml", R"(
version: 1
name: t
duration: 1m
step: 100ms
warmup: 2m
nodes:
  users: {type: client}
  api: {type: service, capacity: 1k/s, latency: 0.5ms}
links:
  - {from: users, to: api, latency: 0.3ms}
)");
  const auto s = simsys::config::load(path);
  REQUIRE(s.has_value());
}

TEST_CASE("a node id declared twice is rejected") {
  require_rejected("simsys_duplicate_node.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  api: {type: service, capacity: 1k/s}
  api: {type: service, capacity: 2k/s}
links: []
)",
                   "duplicate node id 'api'");
}

TEST_CASE("a scalar attribute given as a list or map is rejected") {
  SECTION("link from") {
    require_rejected("simsys_from_list.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  api: {type: service, capacity: 1k/s}
links:
  - from: [api]
    to: api
)",
                     "from must be a scalar");
  }
  SECTION("consumes") {
    require_rejected("simsys_consumes_list.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  q: {type: message_queue}
  workers: {type: worker_pool, consumes: [q]}
links: []
)",
                     "consumes must be a scalar");
  }
  SECTION("type") {
    require_rejected("simsys_type_map.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  thing: {type: {x: 1}}
links: []
)",
                     "type must be a scalar");
  }
}

TEST_CASE("a construct of the wrong shape gets the loader's own message") {
  SECTION("a node body that is a scalar") {
    require_rejected("simsys_node_scalar.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  api: hello
links: []
)",
                     "node 'api' must be a map");
  }
  SECTION("a step that is a scalar") {
    require_rejected("simsys_step_scalar.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  cache: {type: cache}
  api:
    type: service
    capacity: 1k/s
    ops: {read: [cache]}
links: []
)",
                     "step must be a map");
  }
  SECTION("a link that is a scalar") {
    require_rejected("simsys_link_scalar.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  api: {type: service, capacity: 1k/s}
links:
  - api
)",
                     "link must be a map");
  }
  SECTION("then that is a scalar") {
    require_rejected("simsys_then_scalar.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  api: {type: service, capacity: 1k/s}
  queue: {type: message_queue}
  workers: {type: worker_pool, consumes: queue, then: api}
links: []
)",
                     "then must be a list");
  }
}

TEST_CASE("a step may refer to a node declared further down the file") {
  const auto path = write_temp("simsys_forward_reference.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  web:
    type: service
    capacity: 1k/s
    ops: {read: [{to: db, op: read}]}
  db: {type: service, capacity: 1k/s}
links: []
)");
  const auto s = simsys::config::load(path);
  REQUIRE(s.has_value());
  REQUIRE(s->nodes.size() == 2);
}

TEST_CASE("a step or link missing a required key is rejected") {
  SECTION("a step without 'to'") {
    require_rejected("simsys_step_no_to.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  api:
    type: service
    capacity: 1k/s
    ops: {read: [{op: read}]}
links: []
)",
                     "step needs a 'to'");
  }
  SECTION("a link without 'from'") {
    require_rejected("simsys_link_no_from.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  api: {type: service, capacity: 1k/s}
links:
  - {to: api}
)",
                     "link needs 'from'");
  }
  SECTION("a link without 'to'") {
    require_rejected("simsys_link_no_to.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  api: {type: service, capacity: 1k/s}
links:
  - {from: api}
)",
                     "link needs 'to'");
  }
}

TEST_CASE("ops values that are not step lists are skipped, for clients and services alike") {
  const auto path = write_temp("simsys_numeric_ops.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  users:
    type: client
    ops: {ping: 1.0}
  api:
    type: service
    capacity: 1k/s
    ops: {ping: 0.5}
links: []
)");
  const auto s = simsys::config::load(path);
  REQUIRE(s.has_value());
  REQUIRE(s->nodes.size() == 2);
}

TEST_CASE("a document whose root is not a map is rejected with the loader's own message") {
  SECTION("a bare scalar") {
    require_rejected("simsys_root_scalar.yaml", "hello", "scenario must be a map");
  }
  SECTION("a list") {
    require_rejected("simsys_root_list.yaml", "- a\n- b", "scenario must be a map");
  }
  SECTION("an empty file still reports the first missing key") {
    require_rejected("simsys_root_empty.yaml", "", "missing top-level key 'version'");
  }
}

TEST_CASE("name must be a scalar") {
  SECTION("a list") {
    require_rejected("simsys_name_list.yaml", R"(
version: 1
name: [x]
duration: 1m
nodes:
  api: {type: service, capacity: 1k/s}
links: []
)",
                     "name must be a scalar");
  }
  SECTION("a map") {
    require_rejected("simsys_name_map.yaml", R"(
version: 1
name: {a: 1}
duration: 1m
nodes:
  api: {type: service, capacity: 1k/s}
links: []
)",
                     "name must be a scalar");
  }
}
