// Properties of the scenario loader: what a valid file produces and how every kind of
// mistake in a scenario file is reported (message, file and line).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <unistd.h>  // getpid

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.config;
import simsys.core;
import simsys.model;

using namespace std::chrono_literals;

namespace {

// A scenario file in the system temp directory that removes itself when the test ends
// (RAII: the destructor runs even when a REQUIRE fails and unwinds the test), so test runs
// leave nothing behind. `string()` gives the form `load` takes: passing a std::filesystem::path
// directly only compiles on POSIX, where its native string type is std::string.
struct TempFile {
  std::filesystem::path path;
  TempFile(std::filesystem::path p) : path(std::move(p)) {}
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  ~TempFile() {
    std::error_code ignored;  // the non-throwing overload: a destructor must not throw
    std::filesystem::remove(path, ignored);
  }
  std::string string() const { return path.string(); }
};

// Writes `yaml` to a file in the system temp directory, so the loader is exercised through
// its real entry point (a file path), not a string shortcut. The process id is part of the
// file name so two test processes running at the same time (e.g. ctest -j) never overwrite
// each other's scenario files.
TempFile write_temp(std::string_view name, std::string_view yaml) {
  const auto unique_name = std::to_string(::getpid()) + "_" + std::string{name};
  const auto path = std::filesystem::temp_directory_path() / unique_name;
  std::ofstream(path) << yaml;
  return TempFile{path};  // a prvalue: guaranteed copy elision (C++17), no copy or move needed
}

// Loads `yaml` and asserts it is rejected with exactly `message`, attributed to the file.
// C++23 std::expected<T, E> (the loader's return type) is a value-or-error result:
// `has_value()` tells which one it holds and `error()` gives the structured ConfigError.
void require_rejected(std::string_view file_name, std::string_view yaml, std::string_view message) {
  CAPTURE(yaml);  // a failing assertion below then shows the offending scenario
  const auto path = write_temp(file_name, yaml);
  const auto s = simsys::config::load(path.string());
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
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  REQUIRE(s->name == "minimal");
  REQUIRE(s->duration == 1min);
  REQUIRE(s->nodes.size() == 2);
  REQUIRE(s->nodes[0].id == "users");
  REQUIRE(s->nodes[1].type == simsys::model::NodeType::service);
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
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
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
  const auto s = simsys::config::load(path.string());
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
  const auto s = simsys::config::load(path.string());
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
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
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
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
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

TEST_CASE("a client's and a service's numeric op shares load") {
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
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
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

TEST_CASE("an op value must be a step list or a share") {
  const auto service_ops = [](std::string_view ops) {
    return "version: 1\nname: t\nduration: 1m\nnodes:\n  api:\n    type: service\n"
           "    capacity: 1k/s\n    ops: " +
           std::string{ops} + "\nlinks: []\n";
  };
  SECTION("a map value that is neither a list nor a number") {
    require_rejected("simsys_op_map.yaml", service_ops("{read: {to: ghost}}"),
                     "op 'read' must be a step list or a share");
  }
  SECTION("a text value that is not a number") {
    require_rejected("simsys_op_text.yaml", service_ops("{read: fast}"),
                     "op 'read' must be a step list or a share");
  }
  SECTION("ops given as a scalar") {
    require_rejected("simsys_ops_scalar.yaml", service_ops("hello"), "ops must be a map");
  }
  SECTION("ops given as a list") {
    require_rejected("simsys_ops_list.yaml", service_ops("[ghost]"), "ops must be a map");
  }
  SECTION("a service op that is a share still loads") {
    const auto path = write_temp("simsys_op_share.yaml", service_ops("{ping: 0.5}"));
    const auto s = simsys::config::load(path.string());
    INFO((s ? "" : s.error().message));
    REQUIRE(s.has_value());
  }
  SECTION("an empty step list is a valid terminal op") {
    const auto path = write_temp("simsys_op_empty_list.yaml", service_ops("{read: []}"));
    const auto s = simsys::config::load(path.string());
    INFO((s ? "" : s.error().message));
    REQUIRE(s.has_value());
  }
}

TEST_CASE("name and node ids must not be empty") {
  SECTION("an empty quoted name") {
    require_rejected("simsys_name_empty.yaml", R"(
version: 1
name: ''
duration: 1m
nodes:
  api: {type: service, capacity: 1k/s}
links: []
)",
                     "name must not be empty");
  }
  SECTION("a null name") {
    require_rejected("simsys_name_null.yaml", R"(
version: 1
name:
duration: 1m
nodes:
  api: {type: service, capacity: 1k/s}
links: []
)",
                     "name must not be empty");
  }
  SECTION("an empty node id") {
    require_rejected("simsys_node_id_empty.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  '': {type: service}
links: []
)",
                     "node id must not be empty");
  }
}

TEST_CASE("common node keys are typed: dc, replicas, capacity, latency, advanced") {
  const auto path = write_temp("simsys_typed_node_keys.yaml", R"(
version: 1
name: typed
duration: 1m
datacenters: {eu: {}, us: {}}
nodes:
  users: {type: client, dc: eu, rate: {base: 10/s}, ops: {ping: 1.0}}
  api:
    type: service
    dc: us
    replicas: 4
    capacity: 3k/s
    latency: 0.3ms
    advanced: {queue_limit: 50, concurrency: 8, service_variability: 1.5, burstiness: 2, yellow: 0.6, red: 0.8}
links:
  - {from: users, to: api, dir: inout}
)");
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  REQUIRE(s->datacenters == std::vector<std::string>{"eu", "us"});
  REQUIRE(s->nodes.size() == 2);
  const auto& api = s->nodes[1];
  REQUIRE(api.id == "api");
  CHECK(api.dc == "us");
  CHECK(api.replicas == 4);
  CHECK(api.capacity == simsys::Rate{3000});
  CHECK(api.latency == 300us);
  CHECK(api.advanced.queue_limit == 50.0);
  CHECK(api.advanced.concurrency == 8);
  CHECK(api.advanced.service_variability == 1.5);
  CHECK(api.advanced.burstiness == 2.0);
  CHECK(api.advanced.yellow == 0.6);
  CHECK(api.advanced.red == 0.8);
}

TEST_CASE("top-level seed, step and warmup are typed with defaults") {
  SECTION("explicit values") {
    const auto path = write_temp("simsys_header_options.yaml", R"(
version: 1
name: t
duration: 2h
seed: 7
step: 50ms
warmup: 1m
nodes: {a: {type: service}}
links: []
)");
    const auto s = simsys::config::load(path.string());
    INFO((s ? "" : s.error().message));
    REQUIRE(s.has_value());
    CHECK(s->seed == 7);
    CHECK(s->step == 50ms);
    CHECK(s->warmup == 1min);
  }
  SECTION("absent keys take the schema defaults") {
    const auto path = write_temp("simsys_header_defaults.yaml", R"(
version: 1
name: t
duration: 2h
nodes: {a: {type: service}}
links: []
)");
    const auto s = simsys::config::load(path.string());
    INFO((s ? "" : s.error().message));
    REQUIRE(s.has_value());
    CHECK(s->seed == 1);
    CHECK(s->step == 100ms);
    CHECK(s->warmup == 0s);
    CHECK(s->datacenters == std::vector<std::string>{"main"});
  }
}

TEST_CASE("a node's dc must be a declared datacenter") {
  require_rejected("simsys_unknown_dc.yaml", R"(
version: 1
name: t
duration: 1m
datacenters: {eu: {}}
nodes:
  a: {type: service, dc: mars}
links: []
)",
                   "node 'a': unknown datacenter 'mars'");
}

TEST_CASE("replicas must be a positive integer") {
  require_rejected("simsys_bad_replicas.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  a: {type: service, replicas: 0}
links: []
)",
                   "node 'a': replicas must be a positive integer");
}

TEST_CASE("advanced thresholds must satisfy 0 < yellow < red <= 1") {
  require_rejected("simsys_bad_thresholds.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  a:
    type: service
    advanced: {yellow: 0.9, red: 0.8}
links: []
)",
                   "node 'a': advanced.yellow must be below advanced.red");
}

TEST_CASE("unknown keys are rejected at the top level and inside a node") {
  SECTION("top level") {
    require_rejected("simsys_unknown_top_key.yaml", R"(
version: 1
name: t
duration: 1m
colour: blue
nodes:
  a: {type: service}
links: []
)",
                     "unknown top-level key 'colour'");
  }
  SECTION("inside a node") {
    require_rejected("simsys_unknown_node_key.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  a: {type: service, ttl: 5s}
links: []
)",
                     "node 'a': key 'ttl' is not valid for type 'service'");
  }
}

TEST_CASE("a byte-rate capacity is stored separately from a request-rate capacity") {
  const auto path = write_temp("simsys_byte_capacity_field.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  b: {type: blob_store, capacity: 500MB/s}
links: []
)");
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  REQUIRE(s->nodes.size() == 1);
  CHECK_FALSE(s->nodes[0].capacity.has_value());
  CHECK(s->nodes[0].byte_capacity == simsys::ByteRate{500e6});
}

TEST_CASE("advanced numeric keys are range-checked") {
  SECTION("concurrency of zero") {
    require_rejected("simsys_bad_concurrency.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  a:
    type: service
    advanced: {concurrency: 0}
links: []
)",
                     "node 'a': advanced.concurrency must be a positive integer");
  }
  SECTION("negative queue_limit") {
    require_rejected("simsys_bad_queue_limit.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  a:
    type: service
    advanced: {queue_limit: -5}
links: []
)",
                     "node 'a': advanced.queue_limit must be positive");
  }
  SECTION("negative burstiness") {
    require_rejected("simsys_bad_burstiness.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  a:
    type: service
    advanced: {burstiness: -1}
links: []
)",
                     "node 'a': advanced.burstiness must be non-negative");
  }
}

TEST_CASE("a client's rate.base, op shares and timeout are typed") {
  const auto path = write_temp("simsys_client_workload.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  u: {type: client, rate: {base: 12.8k/s}, ops: {redirect: 0.91, shorten: 0.09}, timeout: 1s}
links: []
)");
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  REQUIRE(s->nodes.size() == 1);
  const auto& workload = s->nodes[0].workload;
  REQUIRE(workload.has_value());
  CHECK(workload->base == simsys::Rate{12800});
  REQUIRE(workload->op_shares.size() == 2);
  CHECK(workload->op_shares[0].first == "redirect");
  CHECK(workload->op_shares[0].second == Catch::Approx(0.91));
  CHECK(workload->op_shares[1].first == "shorten");
  CHECK(workload->timeout == 1s);
}

TEST_CASE("client op shares must sum to one") {
  require_rejected("simsys_shares_sum.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  u: {type: client, rate: {base: 10/s}, ops: {a: 0.5, b: 0.4}}
links: []
)",
                   "node 'u': op shares sum to 0.9, expected 1");
}

TEST_CASE("a client needs rate.base") {
  require_rejected("simsys_client_no_rate.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  u: {type: client, ops: {a: 1}}
links: []
)",
                   "node 'u': client needs rate.base");
}

TEST_CASE("a client op share must be a number in (0, 1]") {
  require_rejected("simsys_share_zero.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  u: {type: client, rate: {base: 10/s}, ops: {a: 0, b: 1}}
links: []
)",
                   "node 'u': op share 'a' must be in (0, 1]");
}

TEST_CASE("service steps are typed with their defaults") {
  const auto path = write_temp("simsys_typed_steps.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  web:
    type: service
    ops:
      read:
        - {to: cache}
        - {to: db, op: select, on: miss, mode: parallel, factor: 3}
  cache: {type: cache}
  db: {type: sql_db}
links:
  - {from: web, to: cache}
  - {from: web, to: db}
)");
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  REQUIRE(s->nodes.size() == 3);
  const auto& steps = s->nodes[0].ops.at("read");
  REQUIRE(steps.size() == 2);
  CHECK(steps[0].to == "cache");
  CHECK(steps[0].op == "read");  // defaults to the incoming op
  CHECK(steps[0].on == simsys::model::StepCondition::always);
  CHECK(steps[0].mode == simsys::model::StepMode::sequential);
  CHECK(steps[0].factor == 1.0);
  CHECK(steps[1].to == "db");
  CHECK(steps[1].op == "select");
  CHECK(steps[1].on == simsys::model::StepCondition::miss);
  CHECK(steps[1].mode == simsys::model::StepMode::parallel);
  CHECK(steps[1].factor == 3.0);
}

TEST_CASE("then steps are typed on nodes that have them") {
  const auto path = write_temp("simsys_typed_then.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  w: {type: worker_pool, consumes: q, then: [{to: c, op: write, factor: 100}]}
  q: {type: message_queue}
  c: {type: cache}
links:
  - {from: q, to: w}
  - {from: w, to: c}
)");
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  REQUIRE(s->nodes.size() == 3);
  const auto& then = s->nodes[0].then;
  REQUIRE(then.size() == 1);
  CHECK(then[0].op == "write");
  CHECK(then[0].factor == 100.0);
}

TEST_CASE("step factor must be positive and on/mode must be known words") {
  const auto with_step = [](std::string_view step) {
    return "version: 1\nname: t\nduration: 1m\nnodes:\n  a: {type: service, ops: {x: [" +
           std::string{step} + "]}}\nlinks: []\n";
  };
  SECTION("a zero factor") {
    require_rejected("simsys_step_factor_zero.yaml", with_step("{to: a, factor: 0}"),
                     "node 'a': step factor must be positive");
  }
  SECTION("an unknown mode") {
    require_rejected("simsys_step_bad_mode.yaml", with_step("{to: a, mode: sideways}"),
                     "node 'a': step mode must be sequential or parallel");
  }
  SECTION("an unknown condition") {
    require_rejected("simsys_step_bad_on.yaml", with_step("{to: a, on: sometimes}"),
                     "node 'a': step condition must be always, miss or failure");
  }
}

TEST_CASE("links are typed: direction, ops filter, latency, weight") {
  const auto path = write_temp("simsys_typed_links.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  a: {type: service}
  b: {type: service}
links:
  - {from: a, to: b, dir: inout, ops: [read, write], latency: 2ms, weight: 0.5}
  - {from: b, to: a}
)");
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  REQUIRE(s->links.size() == 2);
  CHECK(s->links[0].dir == simsys::model::Direction::inout);
  CHECK(s->links[0].ops == std::vector<std::string>{"read", "write"});
  CHECK(s->links[0].latency == 2ms);
  CHECK(s->links[0].weight == 0.5);
  CHECK(s->links[1].dir == simsys::model::Direction::out);
  CHECK(s->links[1].ops.empty());
  CHECK_FALSE(s->links[1].latency.has_value());
}

TEST_CASE("a link's direction must be in, out or inout and its weight non-negative") {
  const auto with_link = [](std::string_view link) {
    return "version: 1\nname: t\nduration: 1m\nnodes:\n  a: {type: service}\n"
           "  b: {type: service}\nlinks:\n  - " +
           std::string{link} + "\n";
  };
  SECTION("an unknown direction") {
    require_rejected("simsys_link_bad_dir.yaml", with_link("{from: a, to: b, dir: sideways}"),
                     "link a -> b: dir must be in, out or inout");
  }
  SECTION("a negative weight") {
    require_rejected("simsys_link_bad_weight.yaml", with_link("{from: a, to: b, weight: -1}"),
                     "link a -> b: weight must be non-negative");
  }
}

TEST_CASE("slo entries are typed; latency maxima are durations, ratios are numbers") {
  const auto path = write_temp("simsys_typed_slo.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  u: {type: client, rate: {base: 10/s}, ops: {a: 1}}
links: []
slo:
  - {metric: p99_latency, node: u, op: a, max: 50ms}
  - {metric: drop_rate, max: 0.001}
)");
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  REQUIRE(s->slos.size() == 2);
  CHECK(s->slos[0].metric == simsys::model::SloMetric::p99_latency);
  CHECK(s->slos[0].node == "u");
  CHECK(s->slos[0].op == "a");
  // C++17 std::get<T>: reads the variant alternative T, throwing std::bad_variant_access
  // when it holds another one; here it proves a latency maximum is held as a duration.
  CHECK(std::get<simsys::SimTime>(s->slos[0].max) == 50ms);
  CHECK(s->slos[1].metric == simsys::model::SloMetric::drop_rate);
  CHECK_FALSE(s->slos[1].node.has_value());
  CHECK(std::get<double>(s->slos[1].max) == 0.001);
}

TEST_CASE("an unknown slo metric or a bad max is rejected") {
  const auto with_slo = [](std::string_view entry) {
    return "version: 1\nname: t\nduration: 1m\nnodes:\n  api: {type: service}\nlinks: []\n"
           "slo: [" +
           std::string{entry} + "]\n";
  };
  SECTION("an unknown metric") {
    require_rejected("simsys_slo_bad_metric.yaml", with_slo("{metric: happiness, max: 1}"),
                     "slo: unknown metric 'happiness'");
  }
  SECTION("a latency maximum that is not a duration") {
    require_rejected("simsys_slo_bad_max.yaml", with_slo("{metric: p99_latency, max: 0.5}"),
                     "slo: p99_latency max must be a duration");
  }
  SECTION("a node that does not exist") {
    require_rejected("simsys_slo_bad_node.yaml",
                     with_slo("{metric: drop_rate, node: nobody, max: 0}"),
                     "slo: unknown node 'nobody'");
  }
  SECTION("a latency percentile without a node") {
    require_rejected("simsys_slo_no_node.yaml", with_slo("{metric: p50_latency, max: 1s}"),
                     "slo: p50_latency needs a node");
  }
}
