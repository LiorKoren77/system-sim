// Properties of the scenario loader: what a valid file produces and how every kind of
// mistake in a scenario file is reported (message, file and line).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <span>
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
import simsys.workload;

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
    {type: client, rate: {base: 1/s}, ops: {x: 1}}
  api:
    type: service
    capacity: 1k/s
links:
  - {from: users, to: apii}
)");
  const auto s = simsys::config::load(path.string());
  REQUIRE_FALSE(s.has_value());
  REQUIRE(s.error().message == "link 1: refers to unknown node 'apii'");
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
                   "node 'thing': unknown node type 'quantum_db'");
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
                   "node 'api': step refers to unknown node 'cache'");
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
    workers: 1
    cost: 1ms
    then: [{to: nowhere, op: write}]
links: []
)",
                   "node 'workers': step refers to unknown node 'nowhere'");
}

TEST_CASE("consumes, backend and origin must name declared nodes") {
  SECTION("consumes") {
    require_rejected("simsys_bad_consumes.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  workers: {type: worker_pool, consumes: ghost, workers: 1, cost: 1ms}
links: []
)",
                     "node 'workers': consumes refers to unknown node 'ghost'");
  }
  SECTION("backend") {
    require_rejected("simsys_bad_backend.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  front: {type: cache, hit_ratio: {fixed: 0.5}, backend: ghost}
links: []
)",
                     "node 'front': backend refers to unknown node 'ghost'");
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
                     "node 'edge': origin refers to unknown node 'ghost'");
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
                   "node 'api': capacity: expected a number");
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
                     "links must not be empty (write [] for none)");
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
                     "node 'api': latency: expected a number");
  }
  SECTION("a link latency that is not a number") {
    require_rejected("simsys_bad_link_latency.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  users: {type: client, rate: {base: 1/s}, ops: {x: 1}}
  api: {type: service, capacity: 1k/s}
links:
  - {from: users, to: api, latency: never}
)",
                     "link users -> api: latency: expected a number");
  }
}

TEST_CASE("well-formed optional durations load") {
  const auto path = write_temp("simsys_good_durations.yaml", R"(
version: 1
name: t
duration: 1m
step: 100ms
warmup: 30s
nodes:
  users: {type: client, rate: {base: 1/s}, ops: {x: 1}}
  api: {type: service, capacity: 1k/s, latency: 0.5ms}
links:
  - {from: users, to: api, latency: 0.3ms}
)");
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
}

// A run reports only the steps after warmup. With nothing left to observe, every SLO would
// "pass" on no data, so the loader refuses such a window before anything runs.
TEST_CASE("the observation window is never empty: duration positive, warmup shorter") {
  SECTION("a zero duration") {
    require_rejected("simsys_zero_duration.yaml", R"(
version: 1
name: t
duration: 0s
nodes:
  api: {type: service, capacity: 1k/s}
links: []
)",
                     "duration must be positive");
  }
  SECTION("a warmup as long as the duration") {
    require_rejected("simsys_warmup_too_long.yaml", R"(
version: 1
name: t
duration: 1m
warmup: 60s
nodes:
  api: {type: service, capacity: 1k/s}
links: []
)",
                     "warmup must be shorter than duration");
  }
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
                     "link 1: from must be a scalar");
  }
  SECTION("consumes") {
    require_rejected("simsys_consumes_list.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  q: {type: message_queue}
  workers: {type: worker_pool, consumes: [q], workers: 1, cost: 1ms}
links: []
)",
                     "node 'workers': consumes must be a scalar");
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
                     "node 'thing': type must be a scalar");
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
  cache: {type: cache, hit_ratio: {fixed: 0.5}}
  api:
    type: service
    capacity: 1k/s
    ops: {read: [cache]}
links: []
)",
                     "node 'api': step must be a map");
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
                     "link 1: must be a map");
  }
  SECTION("then that is a scalar") {
    require_rejected("simsys_then_scalar.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  api: {type: service, capacity: 1k/s}
  queue: {type: message_queue}
  workers: {type: worker_pool, consumes: queue, workers: 1, cost: 1ms, then: api}
links: []
)",
                     "node 'workers': then must be a list");
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
                     "node 'api': step needs a 'to'");
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
                     "link 1: needs 'from'");
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
                     "link 1: needs 'to'");
  }
}

TEST_CASE("a client's numeric op shares load") {
  const auto path = write_temp("simsys_numeric_ops.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  users:
    type: client
    rate: {base: 1/s}
    ops: {ping: 1.0}
  api:
    type: service
    capacity: 1k/s
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

TEST_CASE("a non-client op value must be a step list") {
  const auto service_ops = [](std::string_view ops) {
    return "version: 1\nname: t\nduration: 1m\nnodes:\n  api:\n    type: service\n"
           "    capacity: 1k/s\n    ops: " +
           std::string{ops} + "\nlinks: []\n";
  };
  SECTION("a map value that is neither a list nor a number") {
    require_rejected("simsys_op_map.yaml", service_ops("{read: {to: ghost}}"),
                     "node 'api': op 'read' must be a step list");
  }
  SECTION("a text value that is not a number") {
    require_rejected("simsys_op_text.yaml", service_ops("{read: fast}"),
                     "node 'api': op 'read' must be a step list");
  }
  SECTION("ops given as a scalar") {
    require_rejected("simsys_ops_scalar.yaml", service_ops("hello"),
                     "node 'api': ops must be a map");
  }
  SECTION("ops given as a list") {
    require_rejected("simsys_ops_list.yaml", service_ops("[ghost]"),
                     "node 'api': ops must be a map");
  }
  SECTION("a service op that is a share is rejected: only a client's ops are shares") {
    require_rejected("simsys_op_share.yaml", service_ops("{ping: 0.5}"),
                     "node 'api': op 'ping' must be a step list");
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

// A blob store answers a write at its last byte and a read at its first megabyte (review of plan
// 2c Task 3, I2), so which ops are writes matters there too: `write_ops` is a blob store's key, as
// a cache's, a CDN's and a database's, stored as written (youtube's original_store lists `upload`).
TEST_CASE("a blob store states which ops are writes with write_ops") {
  const auto path = write_temp("simsys_blob_write_ops.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  b: {type: blob_store, capacity: 5GB/s, write_ops: [upload]}
links: []
)");
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  REQUIRE(s->nodes[0].write_ops == std::vector<std::string>{"upload"});
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
                     "node 'a': advanced.queue_limit must be a positive integer");
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
                     "node 'a': advanced.burstiness must be a finite non-negative number");
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
  CHECK(workload->curve.base == simsys::Rate{12800});
  REQUIRE(workload->op_shares.size() == 2);
  CHECK(workload->op_shares[0].first == "redirect");
  CHECK(workload->op_shares[0].second == Catch::Approx(0.91));
  CHECK(workload->op_shares[1].first == "shorten");
  CHECK(workload->timeout == 1s);
}

namespace {
// A scenario with one client `u` whose body after `ops` is `extra` (e.g. `keys: 5`).
std::string client_with(std::string_view extra) {
  return "version: 1\nname: t\nduration: 1m\nnodes:\n  u: {type: client, rate: {base: 10/s}, "
         "ops: {a: 1}, " +
         std::string{extra} + "}\nlinks: []\n";
}
}  // namespace

TEST_CASE("a client's keys are typed: zipf with a space, or uniform") {
  SECTION("zipf with a space") {
    const auto path =
        write_temp("simsys_keys_zipf.yaml", client_with("keys: {zipf: 1.0, space: 100M}"));
    const auto s = simsys::config::load(path.string());
    INFO((s ? "" : s.error().message));
    REQUIRE(s.has_value());
    const auto& workload = s->nodes[0].workload;
    REQUIRE(workload.has_value());
    REQUIRE(workload->keys.has_value());
    REQUIRE(workload->keys->zipf.has_value());
    CHECK(*workload->keys->zipf == 1.0);
    CHECK(workload->keys->space == 100e6);
  }
  SECTION("uniform over a space") {
    const auto path = write_temp("simsys_keys_uniform.yaml", client_with("keys: {uniform: 10M}"));
    const auto s = simsys::config::load(path.string());
    INFO((s ? "" : s.error().message));
    REQUIRE(s.has_value());
    const auto& workload = s->nodes[0].workload;
    REQUIRE(workload.has_value());
    REQUIRE(workload->keys.has_value());
    CHECK_FALSE(workload->keys->zipf.has_value());
    CHECK(workload->keys->space == 10e6);
  }
  SECTION("zipf 0 is uniform, and is stored as uniform") {
    // p_i ∝ i^0 = 1: one distribution, one canonical form, so two clients that write it the two
    // ways compare equal (graph's key_source compares KeySpecs with ==).
    const auto path =
        write_temp("simsys_keys_zipf0.yaml", client_with("keys: {zipf: 0, space: 10k}"));
    const auto s = simsys::config::load(path.string());
    INFO((s ? "" : s.error().message));
    REQUIRE(s.has_value());
    const auto& keys = s->nodes[0].workload->keys;
    REQUIRE(keys.has_value());
    // C++20 designated initializers: `KeySpec{.zipf = ..., .space = ...}` names each member,
    // replacing positional `KeySpec{std::nullopt, 10e3}`, where the reader must know the order
    // of the members; here the expected value reads as the canonical form it states.
    CHECK(*keys == simsys::model::KeySpec{.zipf = std::nullopt, .space = 10e3});
  }
  SECTION("a client without keys has none") {
    const auto path = write_temp("simsys_keys_absent.yaml", client_with("timeout: 1s"));
    const auto s = simsys::config::load(path.string());
    REQUIRE(s.has_value());
    CHECK_FALSE(s->nodes[0].workload->keys.has_value());
  }
}

TEST_CASE("a client's keys block is validated") {
  require_rejected("simsys_keys_no_space.yaml", client_with("keys: {zipf: 1.0}"),
                   "node 'u': keys needs zipf and space, or uniform");
  require_rejected("simsys_keys_negative.yaml", client_with("keys: {zipf: -1, space: 10}"),
                   "node 'u': keys.zipf must be a finite non-negative number");
  require_rejected("simsys_keys_infinite.yaml", client_with("keys: {zipf: .inf, space: 10}"),
                   "node 'u': keys.zipf must be a finite non-negative number");
  require_rejected("simsys_keys_word.yaml", client_with("keys: {zipf: hot, space: 10}"),
                   "node 'u': keys.zipf must be a finite non-negative number");
  // A count's message lists the forms it accepts, so a student who wrote `1.5M` or `-10` sees
  // what to write instead (review of c9d5f8c).
  require_rejected("simsys_keys_zero_space.yaml", client_with("keys: {zipf: 1, space: 0}"),
                   "node 'u': keys.space must be a positive count (a whole number, optionally "
                   "with k, M or G: 500, 10k, 100M)");
  require_rejected("simsys_keys_fraction_uniform.yaml", client_with("keys: {uniform: 1.5M}"),
                   "node 'u': keys.uniform must be a positive count (a whole number, optionally "
                   "with k, M or G: 500, 10k, 100M)");
  require_rejected("simsys_keys_both.yaml", client_with("keys: {zipf: 1, space: 10, uniform: 5}"),
                   "node 'u': keys takes zipf+space or uniform, not both");
  require_rejected("simsys_keys_scalar.yaml", client_with("keys: 5"),
                   "node 'u': keys must be a map");
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

TEST_CASE("a client with no rate at all needs rate.base or rate.points") {
  require_rejected("simsys_client_no_rate.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  u: {type: client, ops: {a: 1}}
links: []
)",
                   "node 'u': client needs rate.base or rate.points");
}

namespace {

// A one-client scenario whose rate is the flow-style mapping `rate_flow` (e.g.
// "{base: 1/s, wave: {...}}"), so each rejection case below states only the rate under test.
std::string client_with_rate(std::string_view rate_flow) {
  return "version: 1\nname: t\nduration: 1m\nnodes:\n  u: {type: client, rate: " +
         std::string{rate_flow} + ", ops: {a: 1}}\nlinks: []\n";
}

// A valid wave, reused where another part of the rate is the one under test.
constexpr std::string_view valid_wave = "{shape: sine, period: 1h, amplitude: 0.5, peak_at: 0s}";

// A (rate mapping, expected message) pair for the table-driven rejection cases.
struct RateRejection {
  std::string_view rate_flow;
  std::string_view message;
};

// C++20 std::span<const T>: a non-owning view of contiguous elements (here a std::array of
// cases), replacing a (pointer, length) pair or a template over the container type; the
// callers' arrays of any length convert to it implicitly.
void require_all_rejected(std::string_view file_name, std::span<const RateRejection> cases) {
  for (const auto& c : cases) {
    INFO("rate: " << c.rate_flow);
    require_rejected(file_name, client_with_rate(c.rate_flow), c.message);
  }
}

}  // namespace

TEST_CASE("a client's rate curve is typed: wave, ramp, spikes, points, noise") {
  const auto path = write_temp("simsys_typed_curve.yaml", R"(
version: 1
name: t
duration: 24h
nodes:
  u:
    type: client
    rate:
      base: 10k/s
      wave: {shape: sine, period: 24h, amplitude: 0.6, peak_at: "20:00"}
      ramp: {from: 0/s, to: 10k/s, over: 5m}
      spikes: [{at: 12:00, for: 10m, factor: 3}, {at: 5m, for: 30s, factor: 2}]
      noise: {sigma: 0.15, correlation: 5m}
    ops: {a: 1.0}
links: []
)");
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  REQUIRE(s->nodes.size() == 1);
  const auto& workload = s->nodes[0].workload;
  REQUIRE(workload.has_value());
  CHECK(workload->curve.base == simsys::Rate{10000});
  CHECK_FALSE(workload->curve.points.has_value());

  REQUIRE(workload->curve.wave.has_value());
  CHECK(workload->curve.wave->shape == simsys::workload::WaveShape::sine);
  CHECK(workload->curve.wave->period == 24h);
  CHECK(workload->curve.wave->amplitude == 0.6);
  CHECK(workload->curve.wave->peak_at == 20h);

  REQUIRE(workload->curve.ramp.has_value());
  CHECK(workload->curve.ramp->from == simsys::Rate{0});
  CHECK(workload->curve.ramp->to == simsys::Rate{10000});
  CHECK(workload->curve.ramp->over == 5min);

  REQUIRE(workload->curve.spikes.size() == 2);
  CHECK(workload->curve.spikes[0].at == 12h);
  CHECK(workload->curve.spikes[0].duration == 10min);
  CHECK(workload->curve.spikes[0].factor == 3.0);
  CHECK(workload->curve.spikes[1].at == 5min);

  REQUIRE(workload->noise.has_value());
  CHECK(workload->noise->sigma == 0.15);
  CHECK(workload->noise->correlation == 5min);
}

TEST_CASE("points replace base and must be sorted, non-empty, with non-negative rates") {
  SECTION("a sorted trace loads and leaves base at zero") {
    const auto path =
        write_temp("simsys_points.yaml", client_with_rate("{points: [[0s, 1k/s], [1h, 5k/s]]}"));
    const auto s = simsys::config::load(path.string());
    INFO((s ? "" : s.error().message));
    REQUIRE(s.has_value());
    const auto& workload = s->nodes[0].workload;
    REQUIRE(workload.has_value());
    REQUIRE(workload->curve.points.has_value());
    REQUIRE(workload->curve.points->at.size() == 2);
    CHECK(workload->curve.points->at[1].first == 1h);
    CHECK(workload->curve.points->at[1].second == simsys::Rate{5000});
    CHECK(workload->curve.base == simsys::Rate{0});
  }
  SECTION("malformed traces are rejected") {
    const RateRejection cases[] = {
        {"{points: [[1h, 1k/s], [0s, 2k/s]]}", "node 'u': rate.points must be sorted by time"},
        {"{points: []}", "node 'u': rate.points must have at least one entry"},
        {"{points: [[0s]]}", "node 'u': rate.points entries are [time, rate] pairs"},
    };
    require_all_rejected("simsys_bad_points.yaml", cases);
  }
}

TEST_CASE("a client needs rate.base or rate.points") {
  const std::string wave_only = "{wave: " + std::string{valid_wave} + "}";
  const RateRejection cases[] = {
      {wave_only, "node 'u': client needs rate.base or rate.points"},
      {"{base: 1/s, points: [[0s, 1/s]]}", "node 'u': rate.base and rate.points are alternatives"},
  };
  require_all_rejected("simsys_base_or_points.yaml", cases);
}

TEST_CASE("rate curve values are range-checked") {
  SECTION("wave") {
    const RateRejection cases[] = {
        {"{base: 1/s, wave: {shape: sine, period: 1h, amplitude: 1.5, peak_at: 0s}}",
         "node 'u': rate.wave.amplitude must be between 0 and 1"},
        {"{base: 1/s, wave: {shape: sine, period: 0s, amplitude: 0.5, peak_at: 0s}}",
         "node 'u': rate.wave.period must be positive"},
        {"{base: 1/s, wave: {shape: triangle, period: 1h, amplitude: 0.5, peak_at: 0s}}",
         "node 'u': rate.wave.shape must be sine, square or diurnal"},
        {"{base: 1/s, wave: {shape: sine}}",
         "node 'u': rate.wave needs shape, period, amplitude and peak_at"},
        {"{base: 1/s, wave: {shape: sine, period: 1h, amplitude: 0.5, peak_at: \"25:00\"}}",
         "node 'u': rate.wave.peak_at: hour must be 00-23"},
        {"{base: 1/s, wave: {shape: sine, period: 1h, amplitude: 0.5, peak_at: noon}}",
         "node 'u': rate.wave.peak_at: expected HH:MM or a duration"},
    };
    require_all_rejected("simsys_bad_wave.yaml", cases);
  }
  SECTION("spikes, with a 1-based index naming the offending spike") {
    const RateRejection cases[] = {
        {"{base: 1/s, spikes: [{at: 0s, for: 1s, factor: 2}, {at: 5s, for: 1s, factor: 0}]}",
         "node 'u': rate.spikes[2].factor must be positive"},
        {"{base: 1/s, spikes: [{at: 0s, for: 0s, factor: 2}]}",
         "node 'u': rate.spikes[1].for must be positive"},
    };
    require_all_rejected("simsys_bad_spikes.yaml", cases);
  }
  SECTION("ramp") {
    const RateRejection cases[] = {
        {"{base: 1/s, ramp: {from: 0/s}}", "node 'u': rate.ramp needs from, to and over"},
        {"{base: 1/s, ramp: {from: 0/s, to: 1/s, over: 0s}}",
         "node 'u': rate.ramp.over must be positive"},
        {"{points: [[0s, 1/s]], ramp: {from: 0/s, to: 1/s, over: 1m}}",
         "node 'u': rate.ramp needs rate.base"},
        // A base of 0/s is no base to be relative to: the ramp multiplier from/base would
        // divide by zero (workload's ramp term throws for it, a programmer error).
        {"{base: 0/s, ramp: {from: 0/s, to: 1/s, over: 1m}}",
         "node 'u': rate.ramp needs rate.base"},
    };
    require_all_rejected("simsys_bad_ramp.yaml", cases);
  }
  SECTION("noise") {
    const RateRejection cases[] = {
        {"{base: 1/s, noise: {sigma: -1, correlation: 5m}}",
         "node 'u': rate.noise.sigma must be non-negative"},
        {"{base: 1/s, noise: {sigma: 0.1, correlation: 0s}}",
         "node 'u': rate.noise.correlation must be positive"},
        {"{base: 1/s, noise: {sigma: 0.1}}", "node 'u': rate.noise needs sigma and correlation"},
    };
    require_all_rejected("simsys_bad_noise.yaml", cases);
  }
}

TEST_CASE("noise sigma is capped at ten") {
  SECTION("ten itself loads") {
    const auto path =
        write_temp("simsys_sigma_ten.yaml",
                   client_with_rate("{base: 1/s, noise: {sigma: 10, correlation: 5m}}"));
    const auto s = simsys::config::load(path.string());
    INFO((s ? "" : s.error().message));
    REQUIRE(s.has_value());
    CHECK(s->nodes[0].workload->noise->sigma == 10.0);
  }
  SECTION("anything above ten is rejected") {
    const RateRejection cases[] = {
        {"{base: 1/s, noise: {sigma: 10.5, correlation: 5m}}",
         "node 'u': rate.noise.sigma must be at most 10"},
        {"{base: 1/s, noise: {sigma: 1e300, correlation: 5m}}",
         "node 'u': rate.noise.sigma must be at most 10"},
    };
    require_all_rejected("simsys_sigma_above_ten.yaml", cases);
  }
}

TEST_CASE("unknown keys inside wave, ramp, a spike or noise are rejected") {
  const RateRejection cases[] = {
      {"{base: 1/s, wave: {shape: sine, period: 1h, amplitude: 0.5, peak_at: 0s, colour: red}}",
       "node 'u': unknown rate.wave key 'colour'"},
      {"{base: 1/s, spikes: [{at: 0s, for: 1s, factor: 2, why: x}]}",
       "node 'u': unknown rate.spikes[1] key 'why'"},
  };
  require_all_rejected("simsys_unknown_curve_key.yaml", cases);
}

TEST_CASE("every rate-curve bound names the offending entry") {
  const RateRejection cases[] = {
      {"{base: 1/s, ramp: {from: 2e15/s, to: 1/s, over: 1m}}",
       "node 'u': rate.ramp.from must be at most 1e15/s"},
      {"{base: 1/s, ramp: {from: 0/s, to: 2e15/s, over: 1m}}",
       "node 'u': rate.ramp.to must be at most 1e15/s"},
      {"{points: [[0s, 1/s], [1h, 2e15/s]]}", "node 'u': rate.points[2] must be at most 1e15/s"},
      {"{base: 1/s, spikes: [{at: 0s, for: 1s, factor: 2}, {at: 5s, for: 1s, factor: 2e6}]}",
       "node 'u': rate.spikes[2].factor must be at most 1e6"},
      {"{base: 1/s, noise: {sigma: 11, correlation: 5m}}",
       "node 'u': rate.noise.sigma must be at most 10"},
  };
  require_all_rejected("simsys_curve_bounds.yaml", cases);
}

TEST_CASE("a curve block of the wrong YAML shape says which shape it needs") {
  const RateRejection cases[] = {
      {"{base: 1/s, wave: [sine]}", "node 'u': rate.wave must be a map"},
      {"{base: 1/s, ramp: 5}", "node 'u': rate.ramp must be a map"},
      {"{base: 1/s, noise: [0.1]}", "node 'u': rate.noise must be a map"},
      {"{base: 1/s, spikes: {at: 0s}}", "node 'u': rate.spikes must be a list"},
      {"{base: 1/s, spikes: [5]}", "node 'u': rate.spikes[1] must be a map"},
      {"{points: 5}", "node 'u': rate.points must be a list"},
      {"{points: [{t: 0s, r: 1/s}]}", "node 'u': rate.points entries are [time, rate] pairs"},
      {"{base: 1/s, wave: {shape: sine, period: 1h, amplitude: 0.5, peak_at: [0s]}}",
       "node 'u': rate.wave.peak_at must be a scalar"},
  };
  require_all_rejected("simsys_curve_shapes.yaml", cases);
}

TEST_CASE("an unquoted clock time is read as hours and minutes after midnight") {
  // YAML 1.1 read 20:30 as the base-60 integer 1230; yaml-cpp keeps the text, and the loader
  // reads it as a clock time: 20 h 30 min after t = 0, which is midnight.
  const auto path = write_temp("simsys_unquoted_clock.yaml", R"(
version: 1
name: t
duration: 24h
nodes:
  u:
    type: client
    rate:
      base: 1k/s
      wave: {shape: sine, period: 24h, amplitude: 0.5, peak_at: 20:30}
      spikes: [{at: 12:00, for: 10m, factor: 2}]
    ops: {a: 1}
  v: {type: client, rate: {points: [[06:15, 1k/s]]}, ops: {a: 1}}
links: []
)");
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  const auto& u = s->nodes[0].workload->curve;
  CHECK(u.wave->peak_at == 20h + 30min);
  CHECK(u.spikes[0].at == 12h);
  CHECK(s->nodes[1].workload->curve.points->at[0].first == 6h + 15min);
}

TEST_CASE("overlapping spike factors may not multiply past a million") {
  SECTION("two spikes at the same moment compound past the bound") {
    const RateRejection cases[] = {
        {"{base: 1/s, spikes: [{at: 1m, for: 1m, factor: 1000}, {at: 1m, for: 1m, factor: 1001}]}",
         "node 'u': rate.spikes: overlapping factors multiply to more than 1e6"},
        // The second starts inside the first: their product counts from its start.
        {"{base: 1/s, spikes: [{at: 0s, for: 1h, factor: 1000}, {at: 30m, for: 1m, "
         "factor: 1001}]}",
         "node 'u': rate.spikes: overlapping factors multiply to more than 1e6"},
    };
    require_all_rejected("simsys_spikes_overlap.yaml", cases);
  }
  SECTION("exactly a million, or back-to-back spikes that never overlap, load") {
    for (const auto rate :
         {"{base: 1/s, spikes: [{at: 1m, for: 1m, factor: 1000}, {at: 1m, for: 1m, factor: 1000}]}",
          "{base: 1/s, spikes: [{at: 0s, for: 1m, factor: 1e6}, {at: 1m, for: 1m, factor: "
          "1e6}]}"}) {
      const auto path = write_temp("simsys_spikes_ok.yaml", client_with_rate(rate));
      const auto s = simsys::config::load(path.string());
      INFO((s ? "" : s.error().message));
      REQUIRE(s.has_value());
    }
  }
}

TEST_CASE("values that are not numbers say so") {
  const RateRejection cases[] = {
      {"{base: 1/s, wave: {shape: sine, period: 1h, amplitude: \"0.5\", peak_at: 0s}}",
       "node 'u': rate.wave.amplitude must be a number"},
      {"{base: 1/s, spikes: [{at: 0s, for: 1s, factor: lots}]}",
       "node 'u': rate.spikes[1].factor must be a number"},
      {"{base: 1/s, noise: {sigma: ~, correlation: 5m}}",
       "node 'u': rate.noise.sigma must not be empty"},
      // NaN is a number in YAML (.nan), just not one in range: the range message.
      {"{base: 1/s, wave: {shape: sine, period: 1h, amplitude: .nan, peak_at: 0s}}",
       "node 'u': rate.wave.amplitude must be between 0 and 1"},
  };
  require_all_rejected("simsys_curve_not_numbers.yaml", cases);
}

TEST_CASE("a scalar rate says rate must be a map") {
  const RateRejection cases[] = {
      {"100/s", "node 'u': rate must be a map (write rate: {base: 100/s})"},
  };
  require_all_rejected("simsys_scalar_rate.yaml", cases);
}

TEST_CASE("a client op share must be a number above zero and at most one") {
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
  cache: {type: cache, hit_ratio: {fixed: 0.5}}
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
  w: {type: worker_pool, consumes: q, workers: 1, cost: 1ms, then: [{to: c, op: write, factor: 100}]}
  q: {type: message_queue}
  c: {type: cache, hit_ratio: {fixed: 0.5}}
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

TEST_CASE("step factor must be finite and positive and on/mode must be known words") {
  const auto with_step = [](std::string_view step) {
    return "version: 1\nname: t\nduration: 1m\nnodes:\n  a: {type: service, ops: {x: [" +
           std::string{step} + "]}}\nlinks: []\n";
  };
  SECTION("a zero factor") {
    require_rejected("simsys_step_factor_zero.yaml", with_step("{to: a, factor: 0}"),
                     "node 'a': step factor must be a finite positive number");
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
                     "link a -> b: weight must be a finite non-negative number");
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

// ---- Review batch: the review's five proposed tests, verbatim -------------------------

TEST_CASE("a misspelt key inside a step, rate, link, slo or advanced block is rejected") {
  const auto with = [](std::string_view nodes, std::string_view tail = "links: []\n") {
    return "version: 1\nname: t\nduration: 1m\nnodes:\n" + std::string{nodes} + std::string{tail};
  };
  SECTION("step") {
    require_rejected("simsys_typo_step.yaml",
                     with("  a: {type: service, ops: {x: [{to: a, fator: 2}]}}\n"),
                     "node 'a': unknown step key 'fator'");
  }
  SECTION("rate") {
    require_rejected("simsys_typo_rate.yaml",
                     with("  u: {type: client, rate: {base: 1/s, wav: x}, ops: {a: 1}}\n"),
                     "node 'u': unknown rate key 'wav'");
  }
  SECTION("link") {
    require_rejected("simsys_typo_link.yaml",
                     with("  a: {type: service}\n  b: {type: service}\n",
                          "links: [{from: a, to: b, wieght: 2}]\n"),
                     "link a -> b: unknown key 'wieght'");
  }
  SECTION("slo") {
    require_rejected(
        "simsys_typo_slo.yaml",
        with("  a: {type: service}\n", "links: []\nslo: [{metric: drop_rate, max: 0.1, nod: a}]\n"),
        "slo: unknown key 'nod'");
  }
  SECTION("advanced") {
    require_rejected("simsys_typo_advanced.yaml",
                     with("  a: {type: service, advanced: {yelow: 0.5}}\n"),
                     "node 'a': unknown advanced key 'yelow'");
  }
}

TEST_CASE("a client needs ops and a then step needs an explicit op") {
  SECTION("client without ops") {
    require_rejected("simsys_client_no_ops.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  u: {type: client, rate: {base: 1/s}}
links: []
)",
                     "node 'u': client needs ops");
  }
  SECTION("then step without op: nothing to inherit") {
    require_rejected("simsys_then_no_op.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  w: {type: worker_pool, workers: 1, cost: 1ms, then: [{to: w}]}
links: []
)",
                     "node 'w': then step needs an 'op'");
  }
}

TEST_CASE("slo must be a list of entries that each name a metric and a max") {
  const auto with_slo = [](std::string_view slo) {
    return "version: 1\nname: t\nduration: 1m\nnodes:\n  a: {type: service}\nlinks: []\nslo: " +
           std::string{slo} + "\n";
  };
  SECTION("a map instead of a list") {
    require_rejected("simsys_slo_map.yaml", with_slo("{metric: drop_rate, max: 1}"),
                     "slo must be a list");
  }
  SECTION("no metric") {
    require_rejected("simsys_slo_no_metric.yaml", with_slo("[{max: 1}]"),
                     "slo: entry needs a metric");
  }
  SECTION("no max") {
    require_rejected("simsys_slo_no_max.yaml", with_slo("[{metric: drop_rate}]"),
                     "slo: drop_rate needs a max");
  }
}

TEST_CASE("a link's ops must be a list of op names") {
  const auto with_ops = [](std::string_view ops) {
    return "version: 1\nname: t\nduration: 1m\nnodes:\n  a: {type: service}\n  b: {type: service}\n"
           "links: [{from: a, to: b, ops: " +
           std::string{ops} + "}]\n";
  };
  SECTION("a scalar") {
    require_rejected("simsys_link_ops_scalar.yaml", with_ops("read"),
                     "link a -> b: ops must be a list of op names");
  }
  SECTION("a nested list") {
    require_rejected("simsys_link_ops_nested.yaml", with_ops("[[read]]"),
                     "link a -> b: ops must be a list of op names");
  }
}

TEST_CASE("a key written twice in one map is rejected at its second occurrence") {
  const auto path = write_temp("simsys_duplicate_key.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  a:
    type: service
    capacity: 1k/s
    capacity: 2k/s
links: []
)");
  const auto s = simsys::config::load(path.string());
  REQUIRE_FALSE(s.has_value());
  REQUIRE(s.error().message == "duplicate key 'capacity'");
  CHECK(s.error().line == 9);
  CHECK(s.error().column == 5);
}

// ---- Review batch: rules the review found missing -------------------------------------

namespace {

// The fixed header every scenario below shares, so each test shows only what it is about.
std::string scenario_with(std::string_view nodes, std::string_view tail = "links: []\n") {
  return "version: 1\nname: t\nduration: 1m\nnodes:\n" + std::string{nodes} + std::string{tail};
}

// Loads `yaml` and asserts it is accepted (the message of a rejection is shown otherwise).
void require_loaded(std::string_view file_name, std::string_view yaml) {
  CAPTURE(yaml);
  const auto path = write_temp(file_name, yaml);
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
}

}  // namespace

TEST_CASE("a link's ops list may not be empty") {
  // An empty list would load as "carry all ops" (the model's meaning of no filter): the
  // opposite of what was written.
  require_rejected("simsys_link_ops_empty.yaml",
                   scenario_with("  a: {type: service}\n  b: {type: service}\n",
                                 "links: [{from: a, to: b, ops: []}]\n"),
                   "link a -> b: ops must name at least one op (omit it to carry all)");
}

TEST_CASE("ops and then are accepted only on the types the schema gives them") {
  SECTION("a cache has no ops: it serves reads and forwards misses through then") {
    require_rejected(
        "simsys_cache_ops.yaml",
        scenario_with("  d: {type: sql_db}\n"
                      "  c: {type: cache, hit_ratio: {fixed: 0.5}, ops: {read: [{to: d}]}}\n"),
        "node 'c': key 'ops' is not valid for type 'cache'");
  }
  SECTION("a service has no then: its steps hang off each op") {
    require_rejected("simsys_service_then.yaml",
                     scenario_with("  d: {type: sql_db}\n"
                                   "  s: {type: service, then: [{to: d, op: write}]}\n"),
                     "node 's': key 'then' is not valid for type 'service'");
  }
  SECTION("a client is a traffic source and has no capacity") {
    require_rejected(
        "simsys_client_capacity.yaml",
        scenario_with("  u: {type: client, rate: {base: 1/s}, ops: {a: 1}, capacity: 1/s}\n"),
        "node 'u': key 'capacity' is not valid for type 'client'");
  }
}

// (No square brackets in a test name: Catch2's ctest discovery reads them as tags.)
TEST_CASE("latency SLOs must name a client and one of its ops; ratio maxima lie between 0 and 1") {
  const auto with_slo = [](std::string_view entry) {
    return scenario_with(
        "  u: {type: client, rate: {base: 1/s}, ops: {a: 1}}\n"
        "  s: {type: service}\n",
        "links: []\nslo: [" + std::string{entry} + "]\n");
  };
  SECTION("a percentile on a service, which originates no requests") {
    require_rejected("simsys_slo_latency_service.yaml",
                     with_slo("{metric: p99_latency, node: s, max: 1s}"),
                     "slo: p99_latency needs a client node, 's' is a service");
  }
  SECTION("an op the client does not offer") {
    require_rejected("simsys_slo_unknown_op.yaml",
                     with_slo("{metric: p99_latency, node: u, op: ghost, max: 1s}"),
                     "slo: client 'u' has no op 'ghost'");
  }
  SECTION("a utilization bound above 100%") {
    require_rejected("simsys_slo_utilization_range.yaml",
                     with_slo("{metric: utilization, max: 1.5}"),
                     "slo: utilization max must be between 0 and 1");
  }
  SECTION("a latency SLO without op covers all of the client's ops") {
    require_loaded("simsys_slo_all_ops.yaml", with_slo("{metric: p99_latency, node: u, max: 1s}"));
  }
}

// A queue lag is a message queue's (plan 2c Task 8, refinement 14): the lag is read off the
// queue's consumer groups, so on a service, a client or any other node the SLO could never be
// observed. `node` may be left out, which bounds the worst queue of the run. The message is the
// latency rule's, in the same words: "needs a <type> node, '<id>' is a <type>".
TEST_CASE("a queue_lag SLO must name a message queue, or no node at all") {
  const auto with_slo = [](std::string_view entry) {
    return scenario_with(
        "  u: {type: client, rate: {base: 1/s}, ops: {a: 1}}\n"
        "  svc: {type: service}\n"
        "  q: {type: message_queue}\n",
        "links: []\nslo: [" + std::string{entry} + "]\n");
  };
  SECTION("a service has no queue lag") {
    require_rejected("simsys_slo_lag_service.yaml",
                     with_slo("{metric: queue_lag, node: svc, max: 1m}"),
                     "slo: queue_lag needs a message_queue node ('svc' is a service)");
  }
  SECTION("a client has none either") {
    require_rejected("simsys_slo_lag_client.yaml",
                     with_slo("{metric: queue_lag, node: u, max: 1m}"),
                     "slo: queue_lag needs a message_queue node ('u' is a client)");
  }
  SECTION("a message queue is accepted") {
    require_loaded("simsys_slo_lag_queue.yaml", with_slo("{metric: queue_lag, node: q, max: 1m}"));
  }
  SECTION("no node is accepted: the worst queue is bounded") {
    require_loaded("simsys_slo_lag_worst.yaml", with_slo("{metric: queue_lag, max: 1m}"));
  }
}

TEST_CASE("a node may not send to itself and a link may not loop") {
  SECTION("a step to its own node") {
    require_rejected("simsys_step_self.yaml",
                     scenario_with("  s: {type: service, ops: {read: [{to: s}]}}\n"),
                     "node 's': a step may not target its own node");
  }
  SECTION("a link from a node to itself") {
    require_rejected("simsys_link_self.yaml",
                     scenario_with("  s: {type: service}\n", "links: [{from: s, to: s}]\n"),
                     "link s -> s: from and to must differ");
  }
}

TEST_CASE("two links between the same pair in the same direction are rejected") {
  const auto with_links = [](std::string_view links) {
    return scenario_with("  a: {type: service}\n  b: {type: service}\n",
                         "links: " + std::string{links} + "\n");
  };
  SECTION("the same from and to") {
    require_rejected("simsys_link_duplicate.yaml",
                     with_links("[{from: a, to: b}, {from: a, to: b, ops: [x]}]"),
                     "link a -> b: duplicate link (put several ops or a weight on one link)");
  }
  SECTION("an `in` link mirrors an `out` link: both carry demand from a to b") {
    require_rejected("simsys_link_duplicate_in.yaml",
                     with_links("[{from: a, to: b, dir: inout}, {from: b, to: a, dir: in}]"),
                     "link b -> a: duplicate link (put several ops or a weight on one link)");
  }
  SECTION("links in opposite directions are two different links") {
    require_loaded("simsys_link_opposite.yaml", with_links("[{from: a, to: b}, {from: b, to: a}]"));
  }
}

TEST_CASE("empty op names are rejected everywhere") {
  SECTION("a client op share") {
    require_rejected("simsys_empty_op_client.yaml",
                     scenario_with("  u: {type: client, rate: {base: 1/s}, ops: {\"\": 1}}\n"),
                     "node 'u': op name must not be empty");
  }
  SECTION("a service op") {
    require_rejected("simsys_empty_op_service.yaml",
                     scenario_with("  u: {type: service, ops: {'': []}}\n"),
                     "node 'u': op name must not be empty");
  }
  SECTION("a step's op") {
    require_rejected("simsys_empty_op_step.yaml",
                     scenario_with("  d: {type: sql_db}\n"
                                   "  s: {type: service, ops: {read: [{to: d, op: ''}]}}\n"),
                     "node 's': step op must not be empty");
  }
  SECTION("a link's op filter") {
    require_rejected("simsys_empty_op_link.yaml",
                     scenario_with("  a: {type: service}\n  b: {type: service}\n",
                                   "links: [{from: a, to: b, ops: [x, '']}]\n"),
                     "link a -> b: op name must not be empty");
  }
  SECTION("an slo's op") {
    require_rejected(
        "simsys_empty_op_slo.yaml",
        scenario_with("  u: {type: client, rate: {base: 1/s}, ops: {a: 1}}\n",
                      "links: []\nslo: [{metric: p99_latency, node: u, op: '', max: 1s}]\n"),
        "slo: op name must not be empty");
  }
}

TEST_CASE("a null value is reported at its own key") {
  // Line 1 is the empty line after R"( ; `capacity:` is on line 8. yaml-cpp places a null
  // value where the next token starts (line 9, `links`), which would send the reader to
  // the wrong line.
  const auto path = write_temp("simsys_null_capacity.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  a:
    type: service
    capacity:
links: []
)");
  const auto s = simsys::config::load(path.string());
  REQUIRE_FALSE(s.has_value());
  REQUIRE(s.error().message == "node 'a': capacity must not be empty");
  CHECK(s.error().line == 8);
}

TEST_CASE("a null number is reported at its own key, like a null text") {
  // Integer and word keys are not read as text, so they need the same rule: `replicas:` is on
  // line 8.
  const auto path = write_temp("simsys_null_replicas.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  a:
    type: service
    replicas:
links: []
)");
  const auto s = simsys::config::load(path.string());
  REQUIRE_FALSE(s.has_value());
  REQUIRE(s.error().message == "node 'a': replicas must not be empty");
  CHECK(s.error().line == 8);
}

TEST_CASE("the share-sum message shows the real sum") {
  // With 6 significant digits ({:g}) the sum 1.000002 printed as "1", contradicting
  // "expected 1".
  require_rejected(
      "simsys_share_sum_digits.yaml",
      scenario_with("  u: {type: client, rate: {base: 1/s}, ops: {a: 0.5, b: 0.500002}}\n"),
      "node 'u': op shares sum to 1.000002, expected 1");
}

TEST_CASE("version must be a whole number") {
  const auto with_version = [](std::string_view version) {
    return "version: " + std::string{version} +
           "\nname: t\nduration: 1m\nnodes:\n  a: {type: service}\nlinks: []\n";
  };
  // One rule for every integer key: a number is a plain (unquoted) scalar. A quoted "1" is
  // a string in YAML, so it is no version, as `replicas: "4"` is no replica count.
  SECTION("a quoted number is a string") {
    require_rejected("simsys_version_quoted.yaml", with_version("\"1\""),
                     "version must be a whole number");
  }
  SECTION("a decimal") {
    require_rejected("simsys_version_decimal.yaml", with_version("1.0"),
                     "version must be a whole number");
  }
  SECTION("a quoted replica count is a string too") {
    require_rejected("simsys_replicas_quoted.yaml",
                     scenario_with("  a: {type: service, replicas: \"4\"}\n"),
                     "node 'a': replicas must be a positive integer");
  }
}

TEST_CASE("faults must be a list") {
  require_rejected("simsys_faults_scalar.yaml",
                   scenario_with("  a: {type: service}\n", "links: []\nfaults: 5\n"),
                   "faults must be a list");
}

TEST_CASE("advanced.queue_limit must be a positive whole number") {
  // The schema types queue_limit as an int: a waiting room holds whole requests.
  require_rejected("simsys_queue_limit_fraction.yaml",
                   scenario_with("  a: {type: service, advanced: {queue_limit: 2.5}}\n"),
                   "node 'a': advanced.queue_limit must be a positive integer");
}

TEST_CASE("the threshold message names the defaulted side") {
  SECTION("red given, yellow defaulted") {
    require_rejected("simsys_threshold_default_yellow.yaml",
                     scenario_with("  a: {type: service, advanced: {red: 0.5}}\n"),
                     "node 'a': advanced.yellow (default 0.7) must be below advanced.red");
  }
  SECTION("yellow given, red defaulted") {
    require_rejected("simsys_threshold_default_red.yaml",
                     scenario_with("  a: {type: service, advanced: {yellow: 0.95}}\n"),
                     "node 'a': advanced.yellow must be below advanced.red (default 0.9)");
  }
}

TEST_CASE("datacenter entries are maps with non-empty names; latency is a list") {
  const auto with_datacenters = [](std::string_view datacenters) {
    return "version: 1\nname: t\nduration: 1m\ndatacenters: " + std::string{datacenters} +
           "\nnodes:\n  a: {type: service}\nlinks: []\n";
  };
  SECTION("a datacenter given as a number") {
    require_rejected("simsys_dc_scalar.yaml", with_datacenters("{eu: 5}"),
                     "datacenters: 'eu' must be a map");
  }
  SECTION("an empty datacenter name") {
    require_rejected("simsys_dc_empty_name.yaml", with_datacenters("{'': {}}"),
                     "datacenters: name must not be empty");
  }
  SECTION("the reserved latency key holds a list") {
    require_rejected("simsys_dc_latency.yaml", with_datacenters("{eu: {}, latency: garbage}"),
                     "datacenters: latency must be a list");
  }
}

TEST_CASE("a client with an empty op map is told it needs an op") {
  require_rejected("simsys_client_empty_ops.yaml",
                   scenario_with("  u: {type: client, rate: {base: 1/s}, ops: {}}\n"),
                   "node 'u': client needs at least one op");
}

// ---- Re-verification batch: aliases, nulls, repeated keys, link and datacenter rules -----

namespace {

// Like require_rejected, and the error must also point at `line` (1-based; line 1 of a raw
// string literal is the empty line right after `R"(`).
void require_rejected_at(std::string_view file_name, std::string_view yaml,
                         std::string_view message, int line) {
  CAPTURE(yaml);
  const auto path = write_temp(file_name, yaml);
  const auto s = simsys::config::load(path.string());
  REQUIRE_FALSE(s.has_value());
  REQUIRE(s.error().message == message);
  REQUIRE(s.error().line == line);
}

// The anchor of nesting level `level`: `base` is the fault map, a1, a2, ... the lists above it.
std::string anchor_name(int level) {
  return level == 0 ? "base" : "a" + std::to_string(level);
}

// `levels` lists nested inside each other, each holding `fan_out` copies of the level below:
// the first copy defines that level in place (`&a3 [...]`), the others are aliases to it
// (`*a3`). The text grows linearly with `levels`, but a walk that follows every alias meets
// the innermost fault map fan_out^levels times.
std::string nested_aliases(int levels, int fan_out) {
  std::string level = "&base {kind: node_down, node: a, at: 1s}";
  for (int i = 1; i <= levels; ++i) {
    std::string list = "&" + anchor_name(i) + " [" + level;
    for (int copy = 1; copy < fan_out; ++copy) list += ", *" + anchor_name(i - 1);
    level = list + "]";
  }
  return level;
}

}  // namespace

TEST_CASE("nested aliases are checked once and cheaply") {
  // yaml-cpp resolves an alias to the anchored node itself, so nested aliases make a small
  // file describe a huge tree: nested_aliases(8, 10) reaches the one `base` map 10^8 times. A
  // walk that followed every path took minutes here (about 4 in a Debug build); the loader's
  // walk meets each shared node once and remembers its expanded size, so it takes
  // milliseconds whether the expansion is accepted or over the cap. `faults` is only checked
  // to be a list until phase 3 types it, which is why nested lists are accepted inside it.
  const auto load_timed = [](std::string_view file_name, int levels) {
    const auto path = write_temp(
        file_name, scenario_with("  a: {type: service}\n",
                                 "links: []\nfaults: [" + nested_aliases(levels, 10) + "]\n"));
    const auto start = std::chrono::steady_clock::now();
    auto s = simsys::config::load(path.string());
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed < 200ms);
    return s;
  };
  SECTION("an expansion under the cap loads") {
    // An entry is a map, a list or a scalar value. The fault map counts 4 (itself and its
    // three values) and each list 1 plus its ten copies, so 5 levels expand to 411 111
    // entries; the rest of the scenario adds 9.
    const auto s = load_timed("simsys_nested_aliases_small.yaml", 5);
    INFO((s ? "" : s.error().message));
    REQUIRE(s.has_value());
  }
  SECTION("an expansion far over the cap is rejected without following every path") {
    const auto s = load_timed("simsys_nested_aliases.yaml", 8);
    REQUIRE_FALSE(s.has_value());
    CHECK(s.error().message == "aliases expand the scenario to 411111120 entries; at most 1000000");
  }
}

TEST_CASE("a null value is reported at its own key, for maps and lists too") {
  // A key with nothing after it holds null, and yaml-cpp places the null where the next token
  // starts (usually the next line). Every message below must point at the key's own line.
  // Where an empty list or map is valid, the message says how to write one, so it does not
  // contradict `links: []`.
  SECTION("advanced.queue_limit") {
    require_rejected_at("simsys_null_queue_limit.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  a:
    type: service
    advanced:
      queue_limit:
links: []
)",
                        "node 'a': advanced.queue_limit must not be empty", 9);
  }
  SECTION("a client op share") {
    require_rejected_at("simsys_null_share.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  u:
    type: client
    rate: {base: 1/s}
    ops:
      ping:
links: []
)",
                        "node 'u': op share 'ping' must not be empty", 10);
  }
  SECTION("a step's op") {
    require_rejected_at("simsys_null_step_op.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  d: {type: sql_db}
  s:
    type: service
    ops:
      read:
        - to: d
          op:
links: []
)",
                        "node 's': step op must not be empty", 12);
  }
  SECTION("a service's ops") {
    require_rejected_at("simsys_null_service_ops.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  s:
    type: service
    ops:
links: []
)",
                        "node 's': ops must not be empty (write {} for none)", 8);
  }
  SECTION("a client's ops: a client needs at least one, so no {} is suggested") {
    require_rejected_at("simsys_null_client_ops.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  u:
    type: client
    rate: {base: 1/s}
    ops:
links: []
)",
                        "node 'u': ops must not be empty", 9);
  }
  SECTION("a node's then") {
    require_rejected_at("simsys_null_then.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  w:
    type: worker_pool
    workers: 1
    cost: 1ms
    then:
links: []
)",
                        "node 'w': then must not be empty (write [] for none)", 10);
  }
  SECTION("a node's advanced") {
    require_rejected_at("simsys_null_advanced.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  a:
    type: service
    advanced:
links: []
)",
                        "node 'a': advanced must not be empty (write {} for none)", 8);
  }
  SECTION("a node's whole body") {
    require_rejected_at("simsys_null_node.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  a:
links: []
)",
                        "node 'a' must not be empty", 6);
  }
  SECTION("an slo's metric") {
    require_rejected_at("simsys_null_slo_metric.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  a: {type: service}
links: []
slo:
  - metric:
    max: 0.1
)",
                        "slo: metric must not be empty", 9);
  }
  SECTION("an slo's max") {
    require_rejected_at("simsys_null_slo_max.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  a: {type: service}
links: []
slo:
  - metric: drop_rate
    max:
)",
                        "slo: max must not be empty", 10);
  }
  SECTION("a datacenter") {
    require_rejected_at("simsys_null_datacenter.yaml", R"(
version: 1
name: t
duration: 1m
datacenters:
  east:
nodes:
  a: {type: service}
links: []
)",
                        "datacenters: 'east' must not be empty (write {} for none)", 6);
  }
  SECTION("a link's ops: an empty list is no valid filter, so no [] is suggested") {
    require_rejected_at("simsys_null_link_ops.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  a: {type: service}
  b: {type: service}
links:
  - from: a
    to: b
    ops:
)",
                        "link a -> b: ops must not be empty", 11);
  }
  SECTION("top-level nodes") {
    require_rejected_at("simsys_null_nodes.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
links: []
)",
                        "nodes must not be empty", 5);
  }
  SECTION("top-level links") {
    require_rejected_at("simsys_null_links.yaml", R"(
version: 1
name: t
duration: 1m
links:
nodes:
  a: {type: service}
)",
                        "links must not be empty (write [] for none)", 5);
  }
  SECTION("top-level slo") {
    require_rejected_at("simsys_null_slo.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  a: {type: service}
slo:
links: []
)",
                        "slo must not be empty (write [] for none)", 7);
  }
}

TEST_CASE("a key written twice is rejected at every level") {
  // The message carries no "node 'a':" prefix: the repeated-key walk runs over the whole
  // document before any reader, so it does not know whether a map is a node, a link or an
  // SLO. What it does know is the second key's position, and line and column are what an
  // editor jumps to, so each section pins the line of the second occurrence.
  SECTION("top level") {
    require_rejected_at("simsys_dup_top.yaml", R"(
version: 1
name: t
name: u
duration: 1m
nodes:
  a: {type: service}
links: []
)",
                        "duplicate key 'name'", 4);
  }
  SECTION("advanced") {
    require_rejected_at("simsys_dup_advanced.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  a:
    type: service
    advanced:
      yellow: 0.5
      yellow: 0.6
links: []
)",
                        "duplicate key 'yellow'", 10);
  }
  SECTION("a step") {
    require_rejected_at("simsys_dup_step.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  d: {type: sql_db}
  s:
    type: service
    ops:
      read:
        - to: d
          to: d
links: []
)",
                        "duplicate key 'to'", 12);
  }
  SECTION("client shares") {
    require_rejected_at("simsys_dup_shares.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  u:
    type: client
    rate: {base: 1/s}
    ops:
      a: 0.5
      a: 0.5
links: []
)",
                        "duplicate key 'a'", 11);
  }
  SECTION("datacenters") {
    require_rejected_at("simsys_dup_datacenters.yaml", R"(
version: 1
name: t
duration: 1m
datacenters:
  eu: {}
  eu: {}
nodes:
  a: {type: service}
links: []
)",
                        "duplicate key 'eu'", 7);
  }
  SECTION("a link") {
    require_rejected_at("simsys_dup_link.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  a: {type: service}
  b: {type: service}
links:
  - from: a
    to: b
    to: a
)",
                        "duplicate key 'to'", 11);
  }
  SECTION("an slo entry") {
    require_rejected_at("simsys_dup_slo.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  a: {type: service}
links: []
slo:
  - metric: drop_rate
    max: 0.1
    max: 0.2
)",
                        "duplicate key 'max'", 11);
  }
  SECTION("rate") {
    require_rejected_at("simsys_dup_rate.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  u:
    type: client
    rate:
      base: 1/s
      base: 2/s
    ops: {a: 1}
links: []
)",
                        "duplicate key 'base'", 10);
  }
}

TEST_CASE("the index prefix names a link whose ends are not yet known") {
  // Until both ends are read, "link a -> b" cannot be written, so these messages name the
  // link by its position in the list, counted from 1 like line numbers.
  const auto with_links = [](std::string_view links) {
    return scenario_with("  a: {type: service}\n  b: {type: service}\n",
                         "links: " + std::string{links} + "\n");
  };
  SECTION("a link that is not a map") {
    require_rejected("simsys_link_index_scalar.yaml", with_links("[5]"), "link 1: must be a map");
  }
  SECTION("a link without from") {
    require_rejected("simsys_link_index_no_from.yaml", with_links("[{to: a}]"),
                     "link 1: needs 'from'");
  }
  SECTION("a from that is a list") {
    require_rejected("simsys_link_index_from_list.yaml", with_links("[{from: [x], to: a}]"),
                     "link 1: from must be a scalar");
  }
  SECTION("a from that names no node") {
    require_rejected("simsys_link_index_ghost.yaml", with_links("[{from: ghost, to: a}]"),
                     "link 1: refers to unknown node 'ghost'");
  }
  SECTION("the index counts the links before the bad one") {
    require_rejected("simsys_link_index_second.yaml", with_links("[{from: a, to: b}, 5]"),
                     "link 2: must be a map");
  }
}

TEST_CASE("advanced.queue_limit follows the integer rule like replicas") {
  // A whole number written as a decimal or in exponent form is no integer, as `replicas: 3.0`
  // is no replica count: one rule for every integer key.
  SECTION("a decimal") {
    require_rejected("simsys_queue_limit_decimal.yaml",
                     scenario_with("  a: {type: service, advanced: {queue_limit: 3.0}}\n"),
                     "node 'a': advanced.queue_limit must be a positive integer");
  }
  SECTION("exponent form") {
    require_rejected("simsys_queue_limit_exponent.yaml",
                     scenario_with("  a: {type: service, advanced: {queue_limit: 1e3}}\n"),
                     "node 'a': advanced.queue_limit must be a positive integer");
  }
}

TEST_CASE("datacenter entries reject unknown keys and non-scalar names") {
  const auto with_datacenters = [](std::string_view datacenters) {
    return "version: 1\nname: t\nduration: 1m\ndatacenters: " + std::string{datacenters} +
           "\nnodes:\n  a: {type: service}\nlinks: []\n";
  };
  SECTION("a key inside a datacenter: none is defined yet") {
    require_rejected("simsys_dc_unknown_key.yaml", with_datacenters("{east: {foo: 1}}"),
                     "datacenters: 'east' has unknown key 'foo'");
  }
  SECTION("a list as a datacenter name") {
    require_rejected("simsys_dc_list_name.yaml", with_datacenters("{[a, b]: {}}"),
                     "datacenters: name must be a scalar");
  }
}

TEST_CASE("a link's ops may not repeat and its weight must be finite and at most 1e9") {
  const auto with_link = [](std::string_view attributes) {
    return scenario_with("  a: {type: service}\n  b: {type: service}\n",
                         "links: [{from: a, to: b, " + std::string{attributes} + "}]\n");
  };
  SECTION("an op listed twice") {
    require_rejected("simsys_link_ops_repeat.yaml", with_link("ops: [ping, ping]"),
                     "link a -> b: duplicate op 'ping'");
  }
  SECTION("a weight that would overflow a sum of weights") {
    require_rejected("simsys_link_weight_huge.yaml", with_link("weight: 1e308"),
                     "link a -> b: weight must be at most 1e9");
  }
  SECTION("an infinite weight, in YAML's own spelling") {
    require_rejected("simsys_link_weight_inf.yaml", with_link("weight: .inf"),
                     "link a -> b: weight must be a finite non-negative number");
  }
  SECTION("the bound itself is allowed") {
    require_loaded("simsys_link_weight_max.yaml", with_link("weight: 1e9"));
  }
}

// ---- Re-verification batch: alias expansion cap, number and key messages ----------------

namespace {

// `size` services that each declare `size` ops of `size` steps, written with two anchors:
// the step list is written once (`&steps`) and every op refers to it, the op map is written
// once (`&ops`) and every service refers to it. The text grows like size^2 (the alias
// references), the expanded scenario like size^3 (every service, op and step is real to the
// readers that build the Scenario).
std::string cubic_aliases(int size) {
  std::string steps = "[";
  for (int i = 0; i < size; ++i) steps += i == 0 ? "{to: db}" : ", {to: db}";
  steps += "]";
  std::string ops = "{";
  for (int i = 0; i < size; ++i) {
    ops += (i == 0 ? "" : ", ") + std::string{"op"} + std::to_string(i) + ": " +
           (i == 0 ? "&steps " + steps : std::string{"*steps"});
  }
  ops += "}";
  std::string nodes = "  db: {type: service}\n";
  for (int i = 0; i < size; ++i) {
    nodes += "  s" + std::to_string(i) +
             ": {type: service, ops: " + (i == 0 ? "&ops " + ops : std::string{"*ops"}) + "}\n";
  }
  return scenario_with(nodes);
}

}  // namespace

TEST_CASE("aliases that expand the scenario past a million entries are rejected") {
  // 100 services x 100 ops x 100 steps: about two million entries from a few kilobytes. The
  // readers that build the Scenario expand every alias again; before the cap, a 10 KB file
  // of this shape took about a minute and 800 MB. The cap is checked by the first walk over
  // the document, before any reader runs, so the rejection is immediate.
  const auto yaml = cubic_aliases(100);
  REQUIRE(yaml.size() < 50'000);
  const auto path = write_temp("simsys_cubic_aliases.yaml", yaml);
  const auto start = std::chrono::steady_clock::now();
  const auto s = simsys::config::load(path.string());
  const auto elapsed = std::chrono::steady_clock::now() - start;
  REQUIRE_FALSE(s.has_value());
  INFO(s.error().message);
  // C++20 std::string::starts_with: a prefix test in one call, replacing
  // `message.rfind(prefix, 0) == 0` or a compare(0, n, prefix) with its length arithmetic.
  CHECK(s.error().message.starts_with("aliases expand the scenario to"));
  CHECK(s.error().message.ends_with("entries; at most 1000000"));
  CHECK(elapsed < 1s);
}

TEST_CASE("an alias inside its own anchor is rejected, not followed forever") {
  // yaml-cpp registers an anchor when its node starts, so `&loop [*loop]` is a list that
  // contains itself: it expands to infinitely many entries.
  const auto path =
      write_temp("simsys_alias_cycle.yaml",
                 scenario_with("  a: {type: service}\n", "links: []\nfaults: &loop [1, *loop]\n"));
  const auto s = simsys::config::load(path.string());
  REQUIRE_FALSE(s.has_value());
  CHECK(s.error().message ==
        "aliases expand the scenario to more entries than can be counted; at most 1000000");
}

TEST_CASE("bad numbers get the message for what is wrong") {
  // A number too large for a double (1e309) is out of range, not "no number": it gets the
  // key's range message. Not-a-number and infinity, in any spelling, are no finite number.
  const auto with_link = [](std::string_view attributes) {
    return scenario_with("  a: {type: service}\n  b: {type: service}\n",
                         "links: [{from: a, to: b, " + std::string{attributes} + "}]\n");
  };
  SECTION("a NaN weight") {
    require_rejected("simsys_weight_nan.yaml", with_link("weight: .nan"),
                     "link a -> b: weight must be a finite non-negative number");
  }
  SECTION("infinity in the spelling of from_chars") {
    require_rejected("simsys_weight_inf_word.yaml", with_link("weight: inf"),
                     "link a -> b: weight must be a finite non-negative number");
  }
  SECTION("negative infinity") {
    require_rejected("simsys_weight_minus_inf.yaml", with_link("weight: -.inf"),
                     "link a -> b: weight must be a finite non-negative number");
  }
  SECTION("a weight too large for a double") {
    require_rejected("simsys_weight_1e309.yaml", with_link("weight: 1e309"),
                     "link a -> b: weight must be at most 1e9");
  }
  SECTION("a weight too negative for a double") {
    require_rejected("simsys_weight_minus_1e309.yaml", with_link("weight: -1e309"),
                     "link a -> b: weight must be a finite non-negative number");
  }
  SECTION("a weight too small for a double rounds to zero, a drained link") {
    require_loaded("simsys_weight_1e-400.yaml", with_link("weight: 1e-400"));
  }
  SECTION("a replica count too large for an int") {
    require_rejected("simsys_replicas_1e309.yaml",
                     scenario_with("  a: {type: service, replicas: 1e309}\n"),
                     "node 'a': replicas must be a positive integer");
  }
  SECTION("a replica count past the int range") {
    require_rejected("simsys_replicas_huge.yaml",
                     scenario_with("  a: {type: service, replicas: 99999999999}\n"),
                     "node 'a': replicas must be a positive integer");
  }
  SECTION("an advanced number too large for a double") {
    require_rejected("simsys_burstiness_1e309.yaml",
                     scenario_with("  a: {type: service, advanced: {burstiness: 1e309}}\n"),
                     "node 'a': advanced.burstiness must be a finite non-negative number");
  }
  SECTION("a step factor too large for a double") {
    require_rejected("simsys_factor_1e309.yaml",
                     scenario_with("  a: {type: service}\n"
                                   "  b: {type: service, ops: {x: [{to: a, factor: 1e309}]}}\n"),
                     "node 'b': step factor must be a finite positive number");
  }
  SECTION("a version past the int range is still not 1") {
    require_rejected("simsys_version_huge.yaml",
                     "version: 99999999999\nname: t\nduration: 1m\nnodes:\n  a: {type: "
                     "service}\nlinks: []\n",
                     "version must be 1");
  }
  SECTION("a seed past the int range") {
    require_rejected("simsys_seed_huge.yaml",
                     "version: 1\nname: t\nduration: 1m\nseed: 99999999999\nnodes:\n  a: "
                     "{type: service}\nlinks: []\n",
                     "seed must be between -2147483648 and 2147483647");
  }
}

TEST_CASE("a non-scalar key is reported as such, not as an empty name") {
  // A list or map is valid YAML as a map key, but names nothing; yaml-cpp's Scalar() reads
  // it as "", which made messages like "unknown top-level key ''".
  SECTION("at the top level") {
    require_rejected("simsys_key_list_top.yaml",
                     scenario_with("  a: {type: service}\n") + "[a, b]: 1\n",
                     "top-level key must be a scalar");
  }
  SECTION("a node id") {
    require_rejected("simsys_key_map_node_id.yaml", scenario_with("  {a: 1}: {type: service}\n"),
                     "node id must be a scalar");
  }
  SECTION("a key of a node body") {
    require_rejected("simsys_key_list_body.yaml", scenario_with("  a: {type: service, [x]: 1}\n"),
                     "node 'a': key must be a scalar");
  }
  SECTION("a key inside advanced") {
    require_rejected("simsys_key_list_advanced.yaml",
                     scenario_with("  a: {type: service, advanced: {[x]: 1}}\n"),
                     "node 'a': advanced key must be a scalar");
  }
  SECTION("a key of a link") {
    require_rejected("simsys_key_list_link.yaml",
                     scenario_with("  a: {type: service}\n  b: {type: service}\n",
                                   "links: [{from: a, to: b, [x]: 1}]\n"),
                     "link a -> b: key must be a scalar");
  }
  SECTION("a key of an slo entry") {
    require_rejected("simsys_key_list_slo.yaml",
                     scenario_with("  a: {type: service}\n",
                                   "links: []\nslo: [{metric: utilization, max: 0.5, [x]: 1}]\n"),
                     "slo: key must be a scalar");
  }
}

TEST_CASE("datacenters with only a latency list are reported at the datacenters key") {
  // `latency` is no datacenter, so this declares none. The message is about `datacenters` as
  // a whole and points at its key (line 5), not at the first entry inside it.
  require_rejected_at("simsys_dc_only_latency.yaml", R"(
version: 1
name: t
duration: 1m
datacenters:
  latency: []
nodes:
  a: {type: service}
links: []
)",
                      "datacenters must not be empty", 5);
}

TEST_CASE("utilization needs a server node, and op is only for latency metrics") {
  const auto with_slo = [](std::string_view entry) {
    return scenario_with(
        "  u: {type: client, rate: {base: 1/s}, ops: {a: 1}}\n"
        "  s: {type: service}\n",
        "links: []\nslo: [" + std::string{entry} + "]\n");
  };
  SECTION("utilization without a node: whose busy fraction?") {
    require_rejected("simsys_slo_utilization_no_node.yaml",
                     with_slo("{metric: utilization, max: 0.5}"), "slo: utilization needs a node");
  }
  SECTION("utilization of a client, a source that is never busy") {
    require_rejected("simsys_slo_utilization_client.yaml",
                     with_slo("{metric: utilization, node: u, max: 0.5}"),
                     "slo: utilization needs a server node, 'u' is a client");
  }
  SECTION("op on utilization") {
    require_rejected("simsys_slo_utilization_op.yaml",
                     with_slo("{metric: utilization, node: s, op: a, max: 0.5}"),
                     "slo: op is only valid for latency metrics");
  }
  SECTION("op on drop_rate") {
    require_rejected("simsys_slo_drop_rate_op.yaml",
                     with_slo("{metric: drop_rate, node: s, op: a, max: 0.1}"),
                     "slo: op is only valid for latency metrics");
  }
  SECTION("drop_rate of one node, utilization of a server") {
    require_loaded("simsys_slo_node_ratios.yaml",
                   with_slo("{metric: drop_rate, node: s, max: 0.1}, "
                            "{metric: utilization, node: s, max: 0.5}"));
  }
}

// Every bound keeps user input inside what the engine's arithmetic handles: past them a
// rate overflows to infinity, which the engine treats as a programmer error and aborts on.
TEST_CASE("concurrency, step factors, rates and capacities have upper bounds") {
  SECTION("concurrency above 100000: Erlang C costs O(c) per node per step") {
    require_rejected("simsys_concurrency_too_big.yaml",
                     scenario_with("  a: {type: service, advanced: {concurrency: 100001}}\n"),
                     "node 'a': advanced.concurrency must be at most 100000");
  }
  SECTION("concurrency of exactly 100000") {
    require_loaded("simsys_concurrency_max.yaml",
                   scenario_with("  a: {type: service, advanced: {concurrency: 100000}}\n"));
  }
  SECTION("a step factor above 1e6") {
    require_rejected("simsys_step_factor_too_big.yaml",
                     scenario_with("  a: {type: service, ops: {x: [{to: b, factor: 1e7}]}}\n"
                                   "  b: {type: service}\n"),
                     "node 'a': step factor must be at most 1e6");
  }
  SECTION("a step factor of 1e307, which would overflow the first rate it multiplies") {
    require_rejected("simsys_step_factor_huge.yaml",
                     scenario_with("  a: {type: service, ops: {x: [{to: b, factor: 1e307}]}}\n"
                                   "  b: {type: service}\n"),
                     "node 'a': step factor must be at most 1e6");
  }
  SECTION("a capacity above 1e15/s") {
    require_rejected("simsys_capacity_too_big.yaml",
                     scenario_with("  a: {type: service, capacity: 2e15/s}\n"),
                     "node 'a': capacity must be at most 1e15/s");
  }
  SECTION("a byte capacity above 1e15B/s") {
    require_rejected("simsys_byte_capacity_too_big.yaml",
                     scenario_with("  b: {type: blob_store, capacity: 2e15B/s}\n"),
                     "node 'b': capacity must be at most 1e15B/s");
  }
  SECTION("a client rate above 1e15/s") {
    require_rejected("simsys_rate_too_big.yaml",
                     scenario_with("  u: {type: client, rate: {base: 2e15/s}, ops: {a: 1}}\n"),
                     "node 'u': rate.base must be at most 1e15/s");
  }
  SECTION("the bounds themselves load") {
    require_loaded("simsys_bounds_max.yaml",
                   scenario_with("  u: {type: client, rate: {base: 1e15/s}, ops: {a: 1}}\n"
                                 "  a: {type: service, capacity: 1e15/s, "
                                 "ops: {x: [{to: b, factor: 1e6}]}}\n"
                                 "  b: {type: blob_store, capacity: 1e15B/s}\n"));
  }
}

TEST_CASE("a node's per-op cost is typed and must be a positive finite number") {
  SECTION("a service's cost map loads, keyed by op") {
    const auto path =
        write_temp("simsys_cost_loads.yaml",
                   scenario_with("  a: {type: service, cost: {write: 3, read: 1}}\n"));
    const auto s = simsys::config::load(path.string());
    INFO((s ? "" : s.error().message));
    REQUIRE(s.has_value());
    REQUIRE(s->nodes[0].cost == std::map<std::string, double>{{"read", 1.0}, {"write", 3.0}});
  }
  SECTION("a cost of zero") {
    require_rejected("simsys_cost_zero.yaml", scenario_with("  a: {type: service, cost: {x: 0}}\n"),
                     "node 'a': cost 'x' must be a positive finite number");
  }
  SECTION("a cost that overflows to infinity") {
    require_rejected("simsys_cost_inf.yaml",
                     scenario_with("  a: {type: service, cost: {x: 1e400}}\n"),
                     "node 'a': cost 'x' must be a positive finite number");
  }
  SECTION("a cost that is not a map") {
    require_rejected("simsys_cost_scalar.yaml", scenario_with("  a: {type: service, cost: 5}\n"),
                     "node 'a': cost must be a map of op to units");
  }
  SECTION("a client has no capacity to charge a cost against") {
    require_rejected(
        "simsys_cost_client.yaml",
        scenario_with("  u: {type: client, rate: {base: 1/s}, ops: {a: 1}, cost: {a: 2}}\n"),
        "node 'u': key 'cost' is not valid for type 'client'");
  }
  SECTION("a cache accepts a cost map") {
    require_loaded("simsys_cost_cache.yaml",
                   scenario_with("  c: {type: cache, hit_ratio: {fixed: 0.5}, cost: {read: 2}}\n"));
  }
}

TEST_CASE("cost is capped") {
  // The same bound as a step's fan-out factor (max_step_factor): a request costing more than
  // a million units is a typo, and rate x cost must stay far from overflowing a double.
  require_rejected("simsys_cost_too_big.yaml",
                   scenario_with("  a: {type: service, cost: {x: 1e7}}\n"),
                   "node 'a': cost 'x' must be at most 1e6");
  require_loaded("simsys_cost_max.yaml", scenario_with("  a: {type: service, cost: {x: 1e6}}\n"));
}

TEST_CASE("a load balancer's policy and vnodes are typed and checked") {
  // C++20 `using enum`: brings LbPolicy's enumerators into this scope so the checks below
  // read `least_connections` instead of `simsys::model::LbPolicy::least_connections`.
  using enum simsys::model::LbPolicy;
  SECTION("a named policy loads as the enum") {
    const auto path =
        write_temp("simsys_lb_policy.yaml",
                   scenario_with("  lb: {type: load_balancer, policy: least_connections}\n"));
    const auto s = simsys::config::load(path.string());
    INFO((s ? "" : s.error().message));
    REQUIRE(s.has_value());
    REQUIRE(s->nodes[0].policy == least_connections);
  }
  // The loader leaves an absent key absent, so explain can say "(default)"; every reader
  // applies the default (round_robin) itself.
  SECTION("no policy key: left absent, round_robin by default") {
    const auto path =
        write_temp("simsys_lb_default.yaml", scenario_with("  lb: {type: load_balancer}\n"));
    const auto s = simsys::config::load(path.string());
    INFO((s ? "" : s.error().message));
    REQUIRE(s.has_value());
    REQUIRE_FALSE(s->nodes[0].policy.has_value());
  }
  SECTION("consistent_hash takes a vnodes count") {
    const auto path = write_temp(
        "simsys_lb_vnodes.yaml",
        scenario_with("  lb: {type: load_balancer, policy: consistent_hash, vnodes: 100}\n"));
    const auto s = simsys::config::load(path.string());
    INFO((s ? "" : s.error().message));
    REQUIRE(s.has_value());
    REQUIRE(s->nodes[0].policy == consistent_hash);
    REQUIRE(s->nodes[0].vnodes == 100);
  }
  SECTION("an unknown policy") {
    require_rejected(
        "simsys_lb_sideways.yaml", scenario_with("  lb: {type: load_balancer, policy: sideways}\n"),
        "node 'lb': policy must be round_robin, least_connections, weighted or consistent_hash");
  }
  SECTION("vnodes of zero") {
    require_rejected(
        "simsys_lb_vnodes_zero.yaml",
        scenario_with("  lb: {type: load_balancer, policy: consistent_hash, vnodes: 0}\n"),
        "node 'lb': vnodes must be a positive integer");
  }
  SECTION("vnodes without consistent_hash") {
    require_rejected("simsys_lb_vnodes_rr.yaml",
                     scenario_with("  lb: {type: load_balancer, vnodes: 10}\n"),
                     "node 'lb': vnodes applies only to consistent_hash");
  }
  SECTION("a policy on a service") {
    require_rejected("simsys_policy_service.yaml",
                     scenario_with("  s: {type: service, policy: round_robin}\n"),
                     "node 's': key 'policy' is not valid for type 'service'");
  }
}

TEST_CASE("a rate limiter's algorithm, limit, burst, window and backend are typed and checked") {
  // C++20 `using enum`: the limiter algorithms' enumerators in scope, as for the LB policies.
  using enum simsys::model::LimiterAlgorithm;
  // A limiter in front of a counter store; `body` is the limiter's own YAML mapping. The
  // limiter -> counters link is declared, so a `backend: counters` has an edge to follow.
  const auto limiter_scenario = [](std::string_view body) {
    return scenario_with("  limiter: {type: rate_limiter, " + std::string{body} +
                             "}\n"
                             "  counters: {type: cache, hit_ratio: {fixed: 0.5}}\n",
                         "links: [{from: limiter, to: counters, dir: inout}]\n");
  };
  SECTION("every key loads as given") {
    const auto path = write_temp(
        "simsys_limiter_all.yaml",
        limiter_scenario("algorithm: token_bucket, limit: 1k/s, burst: 500, backend: counters"));
    const auto s = simsys::config::load(path.string());
    INFO((s ? "" : s.error().message));
    REQUIRE(s.has_value());
    const auto& limiter = s->nodes[0];
    REQUIRE(limiter.type == simsys::model::NodeType::rate_limiter);
    REQUIRE(limiter.algorithm == token_bucket);
    REQUIRE(limiter.limit.has_value());
    REQUIRE(limiter.limit->per_second == 1000.0);
    REQUIRE(limiter.burst == 500);
    REQUIRE(limiter.backend == "counters");
  }
  SECTION("each algorithm name loads as its enumerator") {
    const std::pair<std::string_view, simsys::model::LimiterAlgorithm> names[] = {
        {"token_bucket", token_bucket},
        {"leaky_bucket", leaky_bucket},
        {"fixed_window", fixed_window},
        {"sliding_log", sliding_log},
        {"sliding_counter", sliding_counter}};
    for (const auto& [name, algorithm] : names) {
      CAPTURE(name);
      const auto path =
          write_temp("simsys_limiter_algorithm.yaml",
                     limiter_scenario("algorithm: " + std::string{name} + ", limit: 100/s"));
      const auto s = simsys::config::load(path.string());
      INFO((s ? "" : s.error().message));
      REQUIRE(s.has_value());
      REQUIRE(s->nodes[0].algorithm == algorithm);
    }
  }
  SECTION("a window loads as a duration") {
    const auto path = write_temp("simsys_limiter_window.yaml",
                                 limiter_scenario("algorithm: fixed_window, limit: 100/s, "
                                                  "window: 500ms"));
    const auto s = simsys::config::load(path.string());
    INFO((s ? "" : s.error().message));
    REQUIRE(s.has_value());
    REQUIRE(s->nodes[0].window == 500ms);
  }
  // Window and burst stay absent when not stated, so explain can say "(default)";
  // model::limiter_window and model::limiter_burst apply the defaults for every reader.
  SECTION("defaults: token_bucket, a window of 1s, and a burst of limit times window") {
    const auto path = write_temp("simsys_limiter_defaults.yaml", limiter_scenario("limit: 1k/s"));
    const auto s = simsys::config::load(path.string());
    INFO((s ? "" : s.error().message));
    REQUIRE(s.has_value());
    const auto& limiter = s->nodes[0];
    REQUIRE(limiter.algorithm == token_bucket);
    REQUIRE_FALSE(limiter.window.has_value());
    REQUIRE_FALSE(limiter.burst.has_value());
    REQUIRE(simsys::model::limiter_window(limiter) == 1s);
    REQUIRE(simsys::model::limiter_burst(limiter) == 1000.0);  // limit 1000/s x window 1 s
    REQUIRE_FALSE(limiter.backend.has_value());
  }
  SECTION("a windowed algorithm gets no default burst: it counts limit x window per window") {
    const auto path = write_temp("simsys_limiter_window_default.yaml",
                                 limiter_scenario("algorithm: sliding_log, limit: 1k/s"));
    const auto s = simsys::config::load(path.string());
    INFO((s ? "" : s.error().message));
    REQUIRE(s.has_value());
    REQUIRE_FALSE(s->nodes[0].burst.has_value());
    REQUIRE(simsys::model::limiter_window(s->nodes[0]) == 1s);
  }
  SECTION("a burst on a windowed algorithm") {
    for (const std::string_view algorithm : {"fixed_window", "sliding_log", "sliding_counter"}) {
      CAPTURE(algorithm);
      require_rejected(
          "simsys_limiter_window_burst.yaml",
          limiter_scenario("algorithm: " + std::string{algorithm} + ", limit: 1k/s, burst: 50"),
          "node 'limiter': burst applies only to token_bucket and leaky_bucket");
    }
  }
  SECTION("a leaky bucket takes limit and burst, not a queue's own numbers") {
    for (const std::string_view extra :
         {"capacity: 5k/s", "latency: 1ms", "advanced: {queue_limit: 10}",
          "advanced: {concurrency: 4}"}) {
      CAPTURE(extra);
      require_rejected("simsys_limiter_leaky_extra.yaml",
                       limiter_scenario("algorithm: leaky_bucket, limit: 1k/s, burst: 500, " +
                                        std::string{extra}),
                       "node 'limiter': leaky_bucket takes limit and burst; capacity, latency, "
                       "concurrency, queue_limit and service_variability are not valid");
    }
  }
  SECTION("a leaky bucket refuses service_variability") {
    // The leak is regular by definition (SCV 0, graph::leaky_bucket_capacity): a scenario's
    // own number would be overridden in silence, so it is refused like the queue's numbers.
    require_rejected("simsys_limiter_leaky_variability.yaml",
                     limiter_scenario("algorithm: leaky_bucket, limit: 1k/s, burst: 500, "
                                      "advanced: {service_variability: 1.0}"),
                     "node 'limiter': leaky_bucket takes limit and burst; capacity, latency, "
                     "concurrency, queue_limit and service_variability are not valid");
  }
  SECTION("a limiter without a limit") {
    require_rejected("simsys_limiter_no_limit.yaml", limiter_scenario("algorithm: token_bucket"),
                     "node 'limiter': rate_limiter needs a limit");
  }
  SECTION("a burst of zero") {
    require_rejected("simsys_limiter_burst_zero.yaml", limiter_scenario("limit: 1k/s, burst: 0"),
                     "node 'limiter': burst must be a positive integer");
  }
  SECTION("a window of zero") {
    require_rejected("simsys_limiter_window_zero.yaml",
                     limiter_scenario("algorithm: fixed_window, limit: 1k/s, window: 0s"),
                     "node 'limiter': window must be positive");
  }
  SECTION("an unknown algorithm") {
    require_rejected("simsys_limiter_magic.yaml", limiter_scenario("algorithm: magic, limit: 1k/s"),
                     "node 'limiter': algorithm must be token_bucket, leaky_bucket, "
                     "fixed_window, sliding_log or sliding_counter");
  }
  SECTION("a backend that is not declared") {
    require_rejected("simsys_limiter_backend_nope.yaml",
                     scenario_with("  limiter: {type: rate_limiter, limit: 1k/s, backend: nope}\n"),
                     "node 'limiter': backend refers to unknown node 'nope'");
  }
  SECTION("a backend that is a service, not a cache") {
    require_rejected("simsys_limiter_backend_service.yaml",
                     scenario_with("  limiter: {type: rate_limiter, limit: 1k/s, backend: api}\n"
                                   "  api: {type: service}\n",
                                   "links: [{from: limiter, to: api}]\n"),
                     "node 'limiter': backend must be a cache");
  }
}

// Plan 2a, D5 of the rate-limiter review: a limiter's refusals are policy, not failures, so
// they have an SLO metric of their own, a ratio like drop_rate.
TEST_CASE("reject_rate loads as an SLO metric with a ratio max") {
  const auto path = write_temp(
      "simsys_slo_reject_rate.yaml",
      scenario_with("  limiter: {type: rate_limiter, limit: 1k/s}\n",
                    "links: []\nslo: [{metric: reject_rate, node: limiter, max: 0.5}]\n"));
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  REQUIRE(s->slos.size() == 1);
  REQUIRE(s->slos[0].metric == simsys::model::SloMetric::reject_rate);
  REQUIRE(s->slos[0].node == "limiter");
  REQUIRE(std::get<double>(s->slos[0].max) == 0.5);
}

// Plan 2a, Task 10 / refinement 7: a cache's hit ratio, strategy, write ops, key count,
// eviction policy and ttl are read into typed fields. Hit ratio `che` (the Che
// approximation of an LRU cache) is accepted here and computed in phase 2b.
TEST_CASE(
    "a cache's hit ratio, strategy, write ops, keys, eviction and ttl are typed and checked") {
  // C++20 `using enum`: the cache strategies' enumerators in scope, as for the LB policies.
  using enum simsys::model::CacheStrategy;
  // A cache `c` in front of a database `db`, linked c -> db so a `backend: db` has an edge to
  // follow; `body` is the cache's own YAML mapping.
  const auto cache_scenario = [](std::string_view body) {
    return scenario_with("  c: {type: cache, " + std::string{body} +
                             "}\n"
                             "  db: {type: sql_db}\n",
                         "links: [{from: c, to: db, dir: inout}]\n");
  };
  // Loads a scenario that must be accepted and returns it.
  const auto load_accepted = [](std::string_view file_name, const std::string& yaml) {
    const auto path = write_temp(file_name, yaml);
    auto s = simsys::config::load(path.string());
    INFO((s ? "" : s.error().message));
    REQUIRE(s.has_value());
    return *s;
  };
  SECTION("every key loads as given") {
    const auto s = load_accepted(
        "simsys_cache_all.yaml",
        cache_scenario("hit_ratio: {fixed: 0.8}, strategy: read_through, backend: db, "
                       "write_ops: [write, delete], keys: 20M, eviction: lru, ttl: 5m"));
    const auto& c = s.nodes[0];
    REQUIRE(c.type == simsys::model::NodeType::cache);
    REQUIRE(c.hit_ratio.has_value());
    REQUIRE(c.hit_ratio->fixed == 0.8);
    REQUIRE_FALSE(c.hit_ratio->che);
    REQUIRE(c.strategy == read_through);
    REQUIRE(c.backend == "db");
    REQUIRE(c.write_ops == std::vector<std::string>{"write", "delete"});
    REQUIRE(c.keys == 20'000'000);
    REQUIRE(c.eviction == "lru");
    REQUIRE(c.ttl == 5min);
  }
  // The loader leaves an absent write_ops absent; model::write_ops_of applies the type's default
  // (a cache's: write), so explain, the graph and the behaviours agree on one list.
  SECTION("a cache with only a hit ratio gets cache_aside and the default write op, write") {
    const auto s =
        load_accepted("simsys_cache_defaults.yaml", cache_scenario("hit_ratio: {fixed: 0.5}"));
    const auto& c = s.nodes[0];
    REQUIRE(c.strategy == cache_aside);
    REQUIRE_FALSE(c.write_ops.has_value());
    REQUIRE(simsys::model::write_ops_of(c) == std::vector<std::string>{"write"});
    REQUIRE_FALSE(c.backend.has_value());
    REQUIRE_FALSE(c.keys.has_value());
    REQUIRE_FALSE(c.eviction.has_value());
    REQUIRE_FALSE(c.ttl.has_value());
  }
  SECTION("each strategy name loads as its enumerator") {
    const std::pair<std::string_view, simsys::model::CacheStrategy> names[] = {
        {"cache_aside", cache_aside},
        {"read_through", read_through},
        {"write_through", write_through},
        {"write_around", write_around},
        {"write_back", write_back}};
    for (const auto& [name, expected] : names) {
      CAPTURE(name);
      const auto s =
          load_accepted("simsys_cache_strategy.yaml",
                        cache_scenario("hit_ratio: {fixed: 0.5}, strategy: " + std::string{name} +
                                       (expected == cache_aside ? "" : ", backend: db")));
      REQUIRE(s.nodes[0].strategy == expected);
    }
  }
  SECTION("keys take the count suffixes the shipped scenarios use") {
    const std::pair<std::string_view, long long> counts[] = {
        {"500", 500}, {"10k", 10'000}, {"1M", 1'000'000}, {"1G", 1'000'000'000}};
    for (const auto& [text, expected] : counts) {
      CAPTURE(text);
      const auto s =
          load_accepted("simsys_cache_keys.yaml",
                        cache_scenario("hit_ratio: {fixed: 0.5}, keys: " + std::string{text}));
      REQUIRE(s.nodes[0].keys == expected);
    }
  }
  SECTION("a hit ratio of 0 and of 1 are both valid: all misses and all hits") {
    for (const std::string_view fixed : {"0", "1.0"}) {
      CAPTURE(fixed);
      const auto s =
          load_accepted("simsys_cache_fixed_edge.yaml",
                        cache_scenario("hit_ratio: {fixed: " + std::string{fixed} + "}"));
      REQUIRE(s.nodes[0].hit_ratio.has_value());
      REQUIRE(s.nodes[0].hit_ratio->fixed == std::stod(std::string{fixed}));
    }
  }
  SECTION("hit_ratio che loads and leaves the number to phase 2b") {
    const auto s = load_accepted("simsys_cache_che.yaml",
                                 cache_scenario("hit_ratio: che, eviction: lru, keys: 20M"));
    REQUIRE(s.nodes[0].hit_ratio.has_value());
    REQUIRE(s.nodes[0].hit_ratio->che == true);
    REQUIRE_FALSE(s.nodes[0].hit_ratio->fixed.has_value());
  }
  SECTION("a cache_aside cache with a backend is refused: the application reads the store") {
    // Under cache_aside the service's `on: miss` step fetches the misses; a backend on the
    // cache would never be called, and a reader would take it for a read_through cache.
    for (const std::string_view strategy : {"strategy: cache_aside, ", ""}) {
      CAPTURE(strategy);
      require_rejected(
          "simsys_cache_aside_backend.yaml",
          cache_scenario("hit_ratio: {fixed: 0.5}, " + std::string{strategy} + "backend: db"),
          "node 'c': backend is unused with strategy cache_aside");
    }
  }
  SECTION("a cdn's hit ratio is read as a cache's is") {
    const auto s =
        load_accepted("simsys_cdn_hit_ratio.yaml",
                      scenario_with("  edge: {type: cdn, hit_ratio: {fixed: 0.95}, origin: db}\n"
                                    "  db: {type: sql_db}\n",
                                    "links: [{from: edge, to: db, dir: inout}]\n"));
    REQUIRE(s.nodes[0].hit_ratio.has_value());
    REQUIRE(s.nodes[0].hit_ratio->fixed == 0.95);
  }
  SECTION("a cache without a hit ratio") {
    require_rejected("simsys_cache_no_hit_ratio.yaml", cache_scenario("strategy: cache_aside"),
                     "node 'c': cache needs a hit_ratio");
  }
  SECTION("a fixed hit ratio outside 0..1") {
    for (const std::string_view fixed : {"1.5", "-0.1"}) {
      CAPTURE(fixed);
      require_rejected("simsys_cache_fixed_range.yaml",
                       cache_scenario("hit_ratio: {fixed: " + std::string{fixed} + "}"),
                       "node 'c': hit_ratio.fixed must be between 0 and 1");
    }
  }
  SECTION("a hit ratio that is neither che nor a fixed map") {
    require_rejected("simsys_cache_hit_magic.yaml", cache_scenario("hit_ratio: magic"),
                     "node 'c': hit_ratio must be che or {fixed: h}");
  }
  SECTION("a strategy that forwards to a backend needs one") {
    for (const std::string_view strategy : {"read_through", "write_through", "write_around"}) {
      CAPTURE(strategy);
      require_rejected(
          "simsys_cache_no_backend.yaml",
          cache_scenario("hit_ratio: {fixed: 0.5}, strategy: " + std::string{strategy}),
          "node 'c': strategy " + std::string{strategy} + " needs a backend");
    }
  }
  SECTION("an unknown strategy") {
    require_rejected("simsys_cache_sideways.yaml",
                     cache_scenario("hit_ratio: {fixed: 0.5}, strategy: sideways"),
                     "node 'c': strategy must be cache_aside, read_through, write_through, "
                     "write_around or write_back");
  }
  SECTION("an empty write_ops list") {
    require_rejected("simsys_cache_write_ops_empty.yaml",
                     cache_scenario("hit_ratio: {fixed: 0.5}, write_ops: []"),
                     "node 'c': write_ops must name at least one op");
  }
  SECTION("keys of zero") {
    require_rejected("simsys_cache_keys_zero.yaml",
                     cache_scenario("hit_ratio: {fixed: 0.5}, keys: 0"),
                     "node 'c': keys must be a positive integer");
  }
  SECTION("an unknown eviction policy") {
    require_rejected("simsys_cache_eviction_magic.yaml",
                     cache_scenario("hit_ratio: {fixed: 0.5}, eviction: magic"),
                     "node 'c': eviction must be lru, lfu, fifo or ttl");
  }
}

// Plan 2b, Task 4 / refinement 3: a SQL database is a primary plus read replicas; `shards`
// splits the data, `pool` is the connection pool (the concurrency of each queue) and
// `replication_lag` how far behind the replicas run. All four are typed at load time.
TEST_CASE("a sql database's shards, pool, replication lag and write ops are typed and checked") {
  const auto db_scenario = [](std::string_view body) {
    return scenario_with("  db: {type: sql_db, " + std::string{body} + "}\n");
  };
  const auto load_accepted = [](std::string_view file_name, const std::string& yaml) {
    const auto path = write_temp(file_name, yaml);
    auto s = simsys::config::load(path.string());
    INFO((s ? "" : s.error().message));
    REQUIRE(s.has_value());
    return *s;
  };
  SECTION("every key loads as given") {
    const auto s =
        load_accepted("simsys_sql_all.yaml",
                      db_scenario("replicas: 3, shards: 4, pool: 32, replication_lag: 50ms, "
                                  "write_ops: [write, insert]"));
    const auto& db = s.nodes[0];
    REQUIRE(db.type == simsys::model::NodeType::sql_db);
    REQUIRE(db.replicas == 3);
    REQUIRE(db.shards == 4);
    REQUIRE(db.pool == 32);
    REQUIRE(db.replication_lag == 50ms);
    REQUIRE(db.write_ops == std::vector<std::string>{"write", "insert"});
  }
  SECTION("a database with none of them leaves the optional keys absent") {
    const auto s = load_accepted("simsys_sql_defaults.yaml", db_scenario("replicas: 2"));
    const auto& db = s.nodes[0];
    REQUIRE_FALSE(db.shards.has_value());
    REQUIRE_FALSE(db.pool.has_value());
    REQUIRE_FALSE(db.replication_lag.has_value());
  }
  // A database writes by default through the four SQL statements that change data; every
  // other op (select, read, ...) is a read served by the replicas. Compared sorted: the
  // order of a default is not part of the contract.
  SECTION("the default write ops are write, insert, update and delete") {
    const auto s = load_accepted("simsys_sql_write_ops_default.yaml", db_scenario("replicas: 2"));
    REQUIRE_FALSE(s.nodes[0].write_ops.has_value());  // absent: the type's default applies
    auto write_ops = simsys::model::write_ops_of(s.nodes[0]);
    // C++20 std::ranges::sort: takes the container itself instead of a begin/end pair, and is
    // constrained by concepts (a short error for a range that cannot be sorted); replaces
    // std::sort(v.begin(), v.end()).
    std::ranges::sort(write_ops);
    REQUIRE(write_ops == std::vector<std::string>{"delete", "insert", "update", "write"});
  }
  // A stated list replaces the default, even when it is the cache's default `[write]`: the
  // loader stores exactly what the scenario wrote, so `insert` is then a read.
  SECTION("an explicit write_ops of just write on a sql database means exactly that") {
    const auto s = load_accepted("simsys_sql_write_ops_write.yaml",
                                 db_scenario("replicas: 2, write_ops: [write]"));
    REQUIRE(s.nodes[0].write_ops == std::vector<std::string>{"write"});
    REQUIRE(simsys::model::write_ops_of(s.nodes[0]) == std::vector<std::string>{"write"});
  }
  // A write costs a database 5 capacity units by default (spec §6: SQL 5k reads/s vs 1k
  // writes/s); `cost` states another price, as on a service or a cache.
  SECTION("cost is a database key") {
    const auto s =
        load_accepted("simsys_sql_cost.yaml", db_scenario("replicas: 2, cost: {insert: 2}"));
    REQUIRE(s.nodes[0].cost == std::map<std::string, double>{{"insert", 2.0}});
  }
  SECTION("shards of zero") {
    require_rejected("simsys_sql_shards_zero.yaml", db_scenario("shards: 0"),
                     "node 'db': shards must be a positive integer");
  }
  SECTION("a pool of zero") {
    require_rejected("simsys_sql_pool_zero.yaml", db_scenario("pool: 0"),
                     "node 'db': pool must be a positive integer");
  }
  SECTION("a pool above 100000: Erlang C costs O(c) per node per step") {
    require_rejected("simsys_sql_pool_too_big.yaml", db_scenario("pool: 200000"),
                     "node 'db': pool must be at most 100000");
  }
  SECTION("a negative replication lag is refused with the duration parser's message") {
    require_rejected("simsys_sql_lag_negative.yaml", db_scenario("replication_lag: -1ms"),
                     "node 'db': replication_lag: expected a non-negative finite number");
  }
  // `pool` is no longer sugar for `advanced.concurrency`: it sets c AND caps the copy's rate at
  // pool / service time (Little's law, graph::apply_database_keys), which a bare concurrency
  // never does. Two settings for one c would still contradict each other, so stating both is
  // refused, and the message says which one to keep and why (re-review of 96848f0).
  SECTION("pool together with advanced.concurrency") {
    require_rejected(
        "simsys_sql_pool_and_concurrency.yaml", db_scenario("pool: 8, advanced: {concurrency: 8}"),
        "node 'db': pool sets c and caps the rate by Little's law; drop advanced.concurrency");
  }
}

// A database answers every query itself and calls nobody (behaviours::SqlDbBehaviour), so a link
// that would carry demand out of it would carry nothing: the loader refuses it, whichever end
// writes it (review of 48d4d0b).
TEST_CASE("a link that carries demand out of a sql database is refused") {
  const std::string nodes =
      "  u: {type: client, rate: {base: 1000/s}, ops: {select: 1}}\n"
      "  db: {type: sql_db, replicas: 2}\n"
      "  audit: {type: service}\n";
  SECTION("written from the database") {
    require_rejected("simsys_sql_link_out.yaml",
                     scenario_with(nodes,
                                   "links:\n"
                                   "  - {from: u, to: db}\n"
                                   "  - {from: db, to: audit}\n"),
                     "node 'db': a database calls nobody");
  }
  SECTION("written from the other end with dir: in") {
    require_rejected("simsys_sql_link_in.yaml",
                     scenario_with(nodes,
                                   "links:\n"
                                   "  - {from: u, to: db}\n"
                                   "  - {from: audit, to: db, dir: in}\n"),
                     "node 'db': a database calls nobody");
  }
  SECTION("a link into the database is fine") {
    require_loaded("simsys_sql_link_into.yaml",
                   scenario_with(nodes,
                                 "links:\n"
                                 "  - {from: u, to: db}\n"
                                 "  - {from: db, to: audit, dir: in}\n"));
  }
}

// ---- Request-based CDN (plan 2b refinement 4, Task 5) ----
//
// A `cdn` (book ch. 1: a cache at the network's edge, serving static files close to the user)
// is a read-only cache in front of an `origin`, the node it fetches its misses from. The origin
// is now part of the model (NodeSpec::origin), not only a reference the loader checks, so a CDN
// without one has nowhere to send its misses and is refused here.
namespace {
// A client reading through the CDN `cdn` (whose own YAML mapping is `body`) to the service
// `origin`, linked u -> cdn -> origin.
std::string cdn_scenario(std::string_view body) {
  return scenario_with(
      "  u: {type: client, rate: {base: 100/s}, ops: {read: 1}}\n"
      "  cdn: {type: cdn, " +
          std::string{body} +
          "}\n"
          "  origin: {type: service}\n",
      "links:\n"
      "  - {from: u, to: cdn, dir: inout}\n"
      "  - {from: cdn, to: origin, dir: inout}\n");
}
simsys::model::Scenario load_cdn(std::string_view file_name, std::string_view body) {
  const auto path = write_temp(file_name, cdn_scenario(body));
  auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  return *s;
}
}  // namespace

// NodeSpec::origin is the name of the node the CDN fetches its misses from; the graph turns it
// into a demand edge cdn -> origin.
TEST_CASE("a cdn's origin is stored on the node") {
  const auto s = load_cdn("simsys_cdn_origin.yaml", "hit_ratio: {fixed: 0.95}, origin: origin");
  const auto& cdn = s.nodes[1];
  REQUIRE(cdn.type == simsys::model::NodeType::cdn);
  REQUIRE(cdn.origin == "origin");
  REQUIRE_FALSE(s.nodes[0].origin.has_value());  // only a cdn has one
}

TEST_CASE("a cdn without an origin is refused: its misses would have nowhere to go") {
  require_rejected("simsys_cdn_no_origin.yaml", cdn_scenario("hit_ratio: {fixed: 0.95}"),
                   "node 'cdn': a cdn needs an origin");
}

// The origin is a server the CDN calls. A client offers traffic and receives none, so a CDN
// whose "origin" is a client could never forward a miss to it. The words are the graph's for
// any call to a client ("clients only send"), so the loader and the graph say it the same way.
TEST_CASE("a cdn whose origin is a client is refused") {
  require_rejected("simsys_cdn_origin_client.yaml", cdn_scenario("origin: u"),
                   "node 'cdn': origin 'u' is a client; clients only send");
}

// A CDN that is its own origin would ask itself for every miss: nothing could ever answer it
// (review of 4d539b0, M6).
TEST_CASE("a cdn that is its own origin is refused") {
  require_rejected("simsys_cdn_origin_self.yaml", cdn_scenario("origin: cdn"),
                   "node 'cdn': a cdn cannot be its own origin");
}

// A CDN caches every op that reaches it unless the op is one of its write ops (`write` by
// default): a client's `upload` sent through the CDN would be "served at the edge" for the hit
// share of it. `write_ops` on a cdn names the ops that are writes, as on a cache, so the graph
// refuses them on their way in (a cdn does not take writes) instead of caching them (review of
// 4d539b0, I2).
TEST_CASE("a cdn reads write_ops, the ops it refuses rather than caches") {
  const auto s = load_cdn("simsys_cdn_write_ops.yaml",
                          "hit_ratio: {fixed: 0.9}, write_ops: [upload], origin: origin");
  REQUIRE(s.nodes[1].write_ops == std::vector<std::string>{"upload"});
  SECTION("an empty list is refused, as on a cache") {
    require_rejected("simsys_cdn_write_ops_empty.yaml",
                     cdn_scenario("write_ops: [], origin: origin"),
                     "node 'cdn': write_ops must name at least one op");
  }
}

TEST_CASE("a cdn's hit ratio is fixed or che, and its ttl is accepted") {
  SECTION("fixed") {
    const auto s =
        load_cdn("simsys_cdn_fixed.yaml", "ttl: 1h, hit_ratio: {fixed: 0.95}, origin: origin");
    REQUIRE(s.nodes[1].hit_ratio.has_value());
    REQUIRE(s.nodes[1].hit_ratio->fixed == 0.95);
    REQUIRE(s.nodes[1].ttl == std::chrono::hours{1});  // stored for explain, not modelled
  }
  SECTION("che") {
    const auto s = load_cdn("simsys_cdn_che.yaml", "ttl: 24h, hit_ratio: che, origin: origin");
    REQUIRE(s.nodes[1].hit_ratio.has_value());
    REQUIRE(s.nodes[1].hit_ratio->che);
    REQUIRE_FALSE(s.nodes[1].hit_ratio->fixed.has_value());
  }
  SECTION("none: a cdn without a hit ratio loads, and misses every time") {
    const auto s = load_cdn("simsys_cdn_no_ratio.yaml", "origin: origin");
    REQUIRE_FALSE(s.nodes[1].hit_ratio.has_value());
  }
}

// A che hit ratio needs the CDN's size, as a cache's does: `keys` on a cdn is how many objects
// its edge holds (not bytes: what it moves in bytes is a byte `capacity`, which makes it count
// megabytes), the Che approximation's cache size.
TEST_CASE("a cdn states its size in keys for a che hit ratio") {
  const auto s =
      load_cdn("simsys_cdn_keys.yaml", "hit_ratio: che, keys: 100k, ttl: 24h, origin: origin");
  REQUIRE(s.nodes[1].keys == 100'000);
  REQUIRE(s.nodes[1].ttl == std::chrono::hours{24});  // read as a cache's ttl, not modelled
}

// ---- ID generator (plan 2b refinement 6; book ch. 7 "Design a unique ID generator") ----
//
// `algorithm:` picks one of the three designs the chapter compares. Until now the loader
// accepted the key on an id_generator and dropped it; it is now typed, and required, because
// the algorithm decides the node's capacity (Graph::build) and a generator without one would
// silently run on a made-up default.

TEST_CASE("an id generator's algorithm loads as its enumerator") {
  using simsys::model::IdAlgorithm;
  const std::pair<std::string_view, IdAlgorithm> names[] = {
      {"snowflake", IdAlgorithm::snowflake},
      {"ticket_server", IdAlgorithm::ticket_server},
      {"uuid", IdAlgorithm::uuid}};
  for (const auto& [name, algorithm] : names) {
    CAPTURE(name);
    const auto path = write_temp(
        "simsys_id_algorithm.yaml",
        scenario_with("  ids: {type: id_generator, algorithm: " + std::string{name} + "}\n"));
    const auto s = simsys::config::load(path.string());
    INFO((s ? "" : s.error().message));
    REQUIRE(s.has_value());
    REQUIRE(s->nodes[0].type == simsys::model::NodeType::id_generator);
    REQUIRE(s->nodes[0].id_algorithm == algorithm);
  }
}

// The generator's own numbers stay optional and are read as on a service: only the algorithm is
// required, and a scenario that states a capacity keeps it (Graph::build lets it win).
TEST_CASE("an id generator keeps the capacity, latency and replicas the scenario states") {
  const auto path =
      write_temp("simsys_id_numbers.yaml",
                 scenario_with("  ids: {type: id_generator, algorithm: snowflake, replicas: 4, "
                               "capacity: 4M/s, latency: 10us}\n"));
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  REQUIRE(s->nodes[0].replicas == 4);
  REQUIRE(s->nodes[0].capacity == simsys::Rate{4e6});
  REQUIRE(s->nodes[0].latency == 10us);
}

// The message names the three words, as the limiter's does, so the fix is in the error.
TEST_CASE("an id generator without a known algorithm is refused") {
  const std::string message =
      "node 'ids': id_generator needs algorithm: snowflake, ticket_server or uuid";
  SECTION("no algorithm key") {
    require_rejected("simsys_id_no_algorithm.yaml", scenario_with("  ids: {type: id_generator}\n"),
                     message);
  }
  SECTION("an unknown algorithm") {
    require_rejected("simsys_id_magic.yaml",
                     scenario_with("  ids: {type: id_generator, algorithm: magic}\n"), message);
  }
  SECTION("a design of the chapter the model does not have") {
    require_rejected("simsys_id_multi_master.yaml",
                     scenario_with("  ids: {type: id_generator, algorithm: multi_master}\n"),
                     message);
  }
}

// Ch. 7's ticket server is ONE auto-increment database (two copies would hand out the same ids).
// Graph::build refuses more replicas; the loader now does too, so `simsys validate` catches it
// with a line number, with the graph's message (review of 3adc555, M3).
TEST_CASE("the loader refuses a ticket server with more than one replica") {
  require_rejected(
      "simsys_id_ticket_replicas.yaml",
      scenario_with("  ids: {type: id_generator, algorithm: ticket_server, replicas: 2}\n"),
      "node 'ids': a ticket server is one auto-increment database: replicas must be 1 (ch. 7; "
      "for more ids per second use snowflake)");
}

// `algorithm` on the other types is unchanged by this feature: a rate limiter has its own
// (the case "a rate limiter's algorithm, limit, burst, window and backend are typed and checked"
// above), and every other type still refuses the key. An id algorithm's names are not a
// limiter's: `snowflake` on a limiter is an unknown limiter algorithm.
TEST_CASE("the algorithm key stays the limiter's on a rate limiter and invalid elsewhere") {
  SECTION("a service") {
    require_rejected("simsys_algorithm_service.yaml",
                     scenario_with("  s: {type: service, algorithm: snowflake}\n"),
                     "node 's': key 'algorithm' is not valid for type 'service'");
  }
  SECTION("a rate limiter does not take an id algorithm") {
    require_rejected("simsys_limiter_snowflake.yaml",
                     scenario_with("  limiter: {type: rate_limiter, algorithm: snowflake, "
                                   "limit: 1k/s}\n"),
                     "node 'limiter': algorithm must be token_bucket, leaky_bucket, "
                     "fixed_window, sliding_log or sliding_counter");
  }
}

// ---- Request sizes, step sizes and link bandwidth (phase 2c, Task 1) ---------------------------
//
// Until now `sizes` (on a client) and `bandwidth` (on a link) were accepted and ignored; now they
// are typed into the model (Workload::sizes, LinkSpec::bandwidth) and a step may state `size`
// (Step::size). Sizes are decimal bytes (1 KB = 1000 B) and a bandwidth is a byte rate that may
// also be written in bits, as a network is (40Gbit/s = 5e9 B/s).

namespace {

// A scenario with one client `u` that sends the ops `stream` and `upload`, plus `sizes_value`
// as the text after `sizes: ` (e.g. `{stream: 50MB}`).
std::string client_with_sizes(std::string_view sizes_value) {
  return scenario_with(
      "  u: {type: client, rate: {base: 10/s}, ops: {stream: 0.9, upload: 0.1}, "
      "sizes: " +
      std::string{sizes_value} + "}\n");
}

// The size the workload states for `op`, in bytes, or nullopt when it states none.
std::optional<double> stated_size(const simsys::model::Workload& workload, std::string_view op) {
  const auto found =
      std::ranges::find_if(workload.sizes, [&](const auto& entry) { return entry.first == op; });
  if (found == workload.sizes.end()) return std::nullopt;
  return found->second.count;
}

// A scenario with two services `a` and `b` and one link whose attributes are `attributes`.
std::string link_with(std::string_view attributes) {
  return scenario_with("  a: {type: service}\n  b: {type: service}\n",
                       "links: [{from: a, to: b, " + std::string{attributes} + "}]\n");
}

// A service `a` whose op `x` has one step, to the service `b`, with the attributes
// `step_attributes` (a step may not target its own node).
std::string step_with(std::string_view step_attributes) {
  return scenario_with("  a: {type: service, ops: {x: [{to: b, " + std::string{step_attributes} +
                       "}]}}\n  b: {type: service}\n");
}

}  // namespace

TEST_CASE("a client's sizes are typed into its workload in bytes") {
  const auto path =
      write_temp("simsys_sizes_typed.yaml", client_with_sizes("{stream: 50MB, upload: 300MB}"));
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  const auto& workload = s->nodes[0].workload;
  REQUIRE(workload.has_value());
  REQUIRE(workload->sizes.size() == 2);
  // 50 MB = 50 x 1e6 B; 300 MB = 300 x 1e6 B.
  REQUIRE(stated_size(*workload, "stream").has_value());
  REQUIRE(stated_size(*workload, "upload").has_value());
  CHECK_THAT(*stated_size(*workload, "stream"), Catch::Matchers::WithinRel(5e7, 1e-12));
  CHECK_THAT(*stated_size(*workload, "upload"), Catch::Matchers::WithinRel(3e8, 1e-12));
}

TEST_CASE("a client may size only some of its ops and an unsized op has no size") {
  const auto path = write_temp("simsys_sizes_partial.yaml", client_with_sizes("{upload: 2KB}"));
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  const auto& workload = *s->nodes[0].workload;
  REQUIRE(workload.sizes.size() == 1);
  CHECK_FALSE(stated_size(workload, "stream").has_value());
  REQUIRE(stated_size(workload, "upload").has_value());
  CHECK_THAT(*stated_size(workload, "upload"), Catch::Matchers::WithinRel(2e3, 1e-12));  // 2 x 1e3
}

TEST_CASE("a client with no sizes states none") {
  const auto path = write_temp("simsys_sizes_absent.yaml", client_with("timeout: 1s"));
  const auto s = simsys::config::load(path.string());
  REQUIRE(s.has_value());
  CHECK(s->nodes[0].workload->sizes.empty());
}

TEST_CASE("sizes may not name an op the client does not send") {
  require_rejected("simsys_sizes_unsent_op.yaml", client_with_sizes("{x: 1MB}"),
                   "node 'u': sizes names op 'x', which the client does not send");
}

TEST_CASE("a size without a unit is refused and the message shows the grammar and the reason") {
  require_rejected("simsys_sizes_no_unit.yaml", client_with_sizes("{stream: 50}"),
                   "node 'u': sizes 'stream' must be a size (B, KB, MB or GB: 50MB): missing "
                   "size unit");
}

// The grammar alone would not say what is wrong with a size that has a unit: the parser's
// reason follows it.
TEST_CASE("a size above 1e15 bytes or below zero is refused with the parser's reason") {
  SECTION("above the ceiling") {
    require_rejected("simsys_sizes_too_big.yaml", client_with_sizes("{stream: 2e15B}"),
                     "node 'u': sizes 'stream' must be a size (B, KB, MB or GB: 50MB): size must "
                     "be at most 1e15B");
  }
  SECTION("negative") {
    require_rejected("simsys_sizes_negative.yaml", client_with_sizes("{stream: -1MB}"),
                     "node 'u': sizes 'stream' must be a size (B, KB, MB or GB: 50MB): expected "
                     "a non-negative finite number");
  }
}

// `0B` is a size (parse_size reads it), but no request: a byte-sized node charges a request its
// megabytes, and a 0-byte request would cost nothing, so it could be served infinitely fast.
TEST_CASE("a request size of zero bytes is refused on a client and on a step") {
  require_rejected("simsys_sizes_zero.yaml", client_with_sizes("{stream: 0B}"),
                   "node 'u': sizes 'stream' must be positive");
  require_rejected("simsys_step_size_zero.yaml", step_with("op: write, size: 0B"),
                   "node 'a': step size must be positive");
}

TEST_CASE("a step's size is typed into the step in bytes") {
  const auto path = write_temp("simsys_step_size.yaml", step_with("op: write, size: 50MB"));
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  const auto& steps = s->nodes[0].ops.at("x");
  REQUIRE(steps.size() == 1);
  REQUIRE(steps[0].size.has_value());
  CHECK_THAT(steps[0].size->count, Catch::Matchers::WithinRel(5e7, 1e-12));  // 50 x 1e6 B
}

TEST_CASE("a step that states no size has none") {
  const auto path = write_temp("simsys_step_no_size.yaml", step_with("op: write"));
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  CHECK_FALSE(s->nodes[0].ops.at("x")[0].size.has_value());
}

TEST_CASE("a step's size that is not a size is refused with the same grammar") {
  require_rejected("simsys_step_size_word.yaml", step_with("op: write, size: fast"),
                   "node 'a': step size must be a size (B, KB, MB or GB: 50MB): expected a "
                   "number");
}

TEST_CASE("a link's bandwidth in bits is typed into the link as bytes per second") {
  const auto path = write_temp("simsys_link_bandwidth.yaml", link_with("bandwidth: 40Gbit/s"));
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  REQUIRE(s->links[0].bandwidth.has_value());
  // 40 Gbit/s / 8 = 5e9 B/s.
  CHECK_THAT(s->links[0].bandwidth->bytes_per_second, Catch::Matchers::WithinRel(5e9, 1e-12));
}

TEST_CASE("a link's bandwidth in bytes is typed as stated and a link without one has none") {
  const auto with = write_temp("simsys_link_bandwidth_bytes.yaml", link_with("bandwidth: 500MB/s"));
  const auto loaded = simsys::config::load(with.string());
  INFO((loaded ? "" : loaded.error().message));
  REQUIRE(loaded.has_value());
  REQUIRE(loaded->links[0].bandwidth.has_value());
  // 500 x 1e6 = 5e8 B/s.
  CHECK_THAT(loaded->links[0].bandwidth->bytes_per_second, Catch::Matchers::WithinRel(5e8, 1e-12));

  const auto without = write_temp("simsys_link_no_bandwidth.yaml", link_with("latency: 1ms"));
  const auto bare = simsys::config::load(without.string());
  REQUIRE(bare.has_value());
  CHECK_FALSE(bare->links[0].bandwidth.has_value());
}

TEST_CASE("a link's bandwidth must be positive") {
  // A pipe of zero capacity can carry nothing: the message says what is wrong, not "invalid".
  require_rejected("simsys_link_bandwidth_zero.yaml", link_with("bandwidth: 0B/s"),
                   "link a -> b: bandwidth must be positive");
}

// The same ceiling as a byte capacity: 1e15 B/s, a million GB/s, far above any network.
TEST_CASE("a link's bandwidth above 1e15 bytes per second is refused") {
  require_rejected("simsys_link_bandwidth_too_big.yaml", link_with("bandwidth: 2e15B/s"),
                   "link a -> b: bandwidth must be at most 1e15B/s");
  require_rejected("simsys_link_bandwidth_bits_too_big.yaml", link_with("bandwidth: 1e16bit/s"),
                   "link a -> b: bandwidth must be at most 1e15B/s");  // 1e16 / 8 = 1.25e15 B/s
}

TEST_CASE("a link's bandwidth that is a request rate is refused and the message shows byte forms") {
  require_rejected("simsys_link_bandwidth_rate.yaml", link_with("bandwidth: 10k/s"),
                   "link a -> b: bandwidth is a byte rate (B/s, or bit/s for a network: "
                   "40Gbit/s)");
}

// A shipped-style scenario in miniature: ch. 14's YouTube sends four ops with sizes, a
// transcoder renames the uploaded video's op and says how big one rendition is, and the
// uploader's link is a 1 Gbit/s pipe. Everything arrives typed in the model.
TEST_CASE("sizes, a step's size and a link's bandwidth round-trip into the model together") {
  const auto path = write_temp("simsys_bytes_round_trip.yaml", R"(
version: 1
name: bytes
duration: 1m
nodes:
  viewers:
    type: client
    rate: {base: 100/s}
    ops: {stream: 0.88, upload: 0.02, upload_meta: 0.02, metadata: 0.08}
    sizes: {stream: 50MB, upload: 300MB, upload_meta: 2KB, metadata: 2KB}
  transcoders:
    type: service
    ops:
      upload: [{to: store, op: write, size: 50MB}, {to: store, op: log}]
  store:
    type: blob_store
links:
  - {from: viewers, to: transcoders, dir: inout, ops: [upload], bandwidth: 1Gbit/s}
  - {from: transcoders, to: store}
)");
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  const auto& viewers = *s->nodes[0].workload;
  REQUIRE(viewers.sizes.size() == 4);
  CHECK_THAT(*stated_size(viewers, "stream"), Catch::Matchers::WithinRel(5e7, 1e-12));
  CHECK_THAT(*stated_size(viewers, "upload"), Catch::Matchers::WithinRel(3e8, 1e-12));
  CHECK_THAT(*stated_size(viewers, "upload_meta"), Catch::Matchers::WithinRel(2e3, 1e-12));
  CHECK_THAT(*stated_size(viewers, "metadata"), Catch::Matchers::WithinRel(2e3, 1e-12));
  const auto& steps = s->nodes[1].ops.at("upload");
  REQUIRE(steps.size() == 2);
  REQUIRE(steps[0].size.has_value());
  CHECK_THAT(steps[0].size->count, Catch::Matchers::WithinRel(5e7, 1e-12));
  CHECK_FALSE(steps[1].size.has_value());
  REQUIRE(s->links[0].bandwidth.has_value());
  // 1 Gbit/s / 8 = 1.25e8 B/s.
  CHECK_THAT(s->links[0].bandwidth->bytes_per_second, Catch::Matchers::WithinRel(1.25e8, 1e-12));
  CHECK_FALSE(s->links[1].bandwidth.has_value());
}

// ---- Byte capacities: in bits too, and only where bytes are served (plan 2c fix round) ----

// A network is rated in bits, and a storage tier's throughput sometimes is too: `40Gbit/s` is a
// byte capacity of 40e9 / 8 = 5e9 B/s, read by the same parser as a link's bandwidth.
TEST_CASE("a byte capacity may be written in bits") {
  const auto path = write_temp("simsys_byte_capacity_bits.yaml", R"(
version: 1
name: t
duration: 1m
nodes:
  b: {type: blob_store, capacity: 40Gbit/s}
links: []
)");
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  CHECK_FALSE(s->nodes[0].capacity.has_value());
  REQUIRE(s->nodes[0].byte_capacity.has_value());
  CHECK_THAT(s->nodes[0].byte_capacity->bytes_per_second, Catch::Matchers::WithinRel(5e9, 1e-12));
}

// Only a CDN and a blob store serve bytes (plan 2c refinement 2): on any other type a byte rate
// would have to be read as some number of requests, which no author means, so it is refused.
TEST_CASE("a byte capacity is refused on a type that counts requests") {
  require_rejected("simsys_byte_capacity_service.yaml",
                   scenario_with("  svc: {type: service, capacity: 1GB/s}\n"),
                   "node 'svc': a service counts requests; a byte capacity belongs on a cdn or "
                   "blob_store");
  require_rejected("simsys_byte_capacity_bits_service.yaml",
                   scenario_with("  svc: {type: service, capacity: 8Gbit/s}\n"),
                   "node 'svc': a service counts requests; a byte capacity belongs on a cdn or "
                   "blob_store");
  require_rejected("simsys_byte_capacity_sink.yaml",
                   scenario_with("  api: {type: external_sink, capacity: 1GB/s}\n"),
                   "node 'api': an external_sink counts requests; a byte capacity belongs on a "
                   "cdn or blob_store");
}

// A blob store's `then` steps are event notifications nobody waits for (plan 2c Task 4), so a step
// `mode` (sequential latencies add, parallel ones overlap) means nothing there. The graph refuses
// `mode: parallel`; only the loader sees `mode: sequential` written (the model's default), so it
// refuses the key itself, at the key, whatever its value.
TEST_CASE("a blob store's then step may not state a mode") {
  for (const auto* mode : {"sequential", "parallel"}) {
    CAPTURE(mode);
    require_rejected_at("simsys_blob_event_mode.yaml", std::string{R"(
version: 1
name: t
duration: 1m
nodes:
  b:
    type: blob_store
    then:
      - {to: svc, op: notify, mode: )"} + mode + R"(}
  svc: {type: service}
links:
  - {from: b, to: svc}
)",
                        "node 'b': a blob store's then steps are event notifications nobody "
                        "waits for (mode orders the calls a request waits for)",
                        9);
  }
}

// ---- Message queues and worker pools are typed (plan 2c refinements 4-5, Task 6) ----
//
// A worker pool is M/M/c over consumption (spec section 7: "capacity = workers / job cost"): its
// `workers` is c and its `cost` the DURATION one worker spends on one job, so youtube's 400
// transcoders at 60 s a video do 400 / 60 s = 6.67 videos a second. A queue's `retention` is how
// long a message stays before it is deleted unconsumed, and its `partitions` is printed, not
// modelled. The loader reads them into typed fields and refuses, with a reason, every key that
// would state the pool's rate a second time.

namespace {
// A queue `q` and a pool `w` that consumes it, with `pool_keys` appended to the pool's flow map
// (after `consumes: q`) and `queue_keys` to the queue's.
std::string queue_and_pool(std::string_view queue_keys, std::string_view pool_keys) {
  return scenario_with("  q: {type: message_queue" +
                       (queue_keys.empty() ? std::string{} : ", " + std::string{queue_keys}) +
                       "}\n  w: {type: worker_pool, consumes: q" +
                       (pool_keys.empty() ? std::string{} : ", " + std::string{pool_keys}) + "}\n");
}
}  // namespace

TEST_CASE("a worker pool's workers and cost and a queue's retention and partitions are typed") {
  const auto path =
      write_temp("simsys_queue_pool_typed.yaml",
                 queue_and_pool("partitions: 32, retention: 168h", "workers: 400, cost: 60s"));
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  const auto& queue = s->nodes[0];
  CHECK(queue.partitions == 32);
  CHECK(queue.retention == 168h);
  const auto& pool = s->nodes[1];
  CHECK(pool.workers == 400);
  CHECK(pool.job_cost == 60s);  // a duration per job
  CHECK(pool.consumes == "q");
  CHECK(pool.cost.empty());  // the capacity-unit map is a service's; a pool has none
}

// The loader leaves an absent retention absent, so that explain can say "(default: ...)" and the
// graph applies defaults::default_retention in one place.
TEST_CASE("a queue that states no retention or partitions leaves them absent") {
  const auto path =
      write_temp("simsys_queue_defaults.yaml", queue_and_pool("", "workers: 4, cost: 5ms"));
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  CHECK_FALSE(s->nodes[0].retention.has_value());
  CHECK_FALSE(s->nodes[0].partitions.has_value());
}

// `cost` is spelled the same on a service and on a worker pool and means two things: a map of
// op to capacity units on a service, a duration per job on a pool. Each loads on its own type.
TEST_CASE("cost is a map of units on a service and a duration on a worker pool") {
  const auto path =
      write_temp("simsys_two_costs.yaml",
                 scenario_with("  svc: {type: service, cost: {write: 3, read: 1}}\n"
                               "  q: {type: message_queue}\n"
                               "  w: {type: worker_pool, consumes: q, workers: 2, cost: 250ms}\n"));
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  const auto& svc = s->nodes[0];
  CHECK(svc.cost == std::map<std::string, double>{{"read", 1.0}, {"write", 3.0}});
  CHECK_FALSE(svc.job_cost.has_value());
  const auto& pool = s->nodes[2];
  CHECK(pool.job_cost == 250ms);
  CHECK(pool.cost.empty());
}

TEST_CASE("a worker pool needs workers and a cost") {
  SECTION("no workers") {
    require_rejected("simsys_pool_no_workers.yaml", queue_and_pool("", "cost: 5ms"),
                     "node 'w': worker_pool needs workers");
  }
  SECTION("no cost") {
    require_rejected("simsys_pool_no_cost.yaml", queue_and_pool("", "workers: 4"),
                     "node 'w': worker_pool needs cost");
  }
}

// A pool's jobs come from the queue it drains and from nowhere else (review of plan 2c Task 6,
// I-1): without `consumes` it would be a server any node could call, so the key is required.
TEST_CASE("a worker pool needs the queue it consumes") {
  require_rejected("simsys_pool_no_consumes.yaml",
                   scenario_with("  q: {type: message_queue}\n"
                                 "  w: {type: worker_pool, workers: 4, cost: 5ms}\n"),
                   "node 'w': worker_pool needs consumes");
}

TEST_CASE("workers, cost, retention and partitions must be positive, and workers bounded") {
  SECTION("workers of zero") {
    require_rejected("simsys_pool_workers_zero.yaml", queue_and_pool("", "workers: 0, cost: 5ms"),
                     "node 'w': workers must be a positive integer");
  }
  SECTION("workers that is not a whole number") {
    require_rejected("simsys_pool_workers_half.yaml", queue_and_pool("", "workers: 2.5, cost: 5ms"),
                     "node 'w': workers must be a positive integer");
  }
  SECTION("workers above 100000: Erlang C costs O(c) per step, as advanced.concurrency") {
    require_rejected("simsys_pool_workers_huge.yaml",
                     queue_and_pool("", "workers: 100001, cost: 5ms"),
                     "node 'w': workers must be at most 100000");
  }
  SECTION("a cost of zero") {
    require_rejected("simsys_pool_cost_zero.yaml", queue_and_pool("", "workers: 4, cost: 0s"),
                     "node 'w': cost must be positive");
  }
  SECTION("a retention of zero") {
    require_rejected("simsys_queue_retention_zero.yaml",
                     queue_and_pool("retention: 0s", "workers: 4, cost: 5ms"),
                     "node 'q': retention must be positive");
  }
  SECTION("partitions of zero") {
    require_rejected("simsys_queue_partitions_zero.yaml",
                     queue_and_pool("partitions: 0", "workers: 4, cost: 5ms"),
                     "node 'q': partitions must be a positive integer");
  }
}

// Each of these four would state the pool's rate a second time: its c and service time are
// `workers` and `cost`, so its capacity is workers / cost and nothing else can say it.
TEST_CASE(
    "a worker pool's rate is workers / cost: capacity, latency, replicas and concurrency "
    "are refused") {
  const std::string reason = "node 'w': a worker pool's rate is workers / cost";
  const std::string own = "workers: 4, cost: 5ms, ";
  SECTION("capacity") {
    require_rejected("simsys_pool_capacity.yaml", queue_and_pool("", own + "capacity: 1k/s"),
                     reason);
  }
  SECTION("latency") {
    require_rejected("simsys_pool_latency.yaml", queue_and_pool("", own + "latency: 2ms"), reason);
  }
  SECTION("replicas") {
    require_rejected("simsys_pool_replicas.yaml", queue_and_pool("", own + "replicas: 2"), reason);
  }
  SECTION("advanced.concurrency") {
    require_rejected("simsys_pool_concurrency.yaml",
                     queue_and_pool("", own + "advanced: {concurrency: 8}"), reason);
  }
}

// A pool's waiting room is the part of the queue it has not read: messages wait in the log until
// `retention` deletes them, so the room is set on the queue, not by a limit on the pool.
TEST_CASE("a worker pool waits in its queue: advanced.queue_limit is refused for retention") {
  require_rejected("simsys_pool_queue_limit.yaml",
                   queue_and_pool("", "workers: 4, cost: 5ms, advanced: {queue_limit: 100}"),
                   "node 'w': a worker pool waits in its queue: set the queue's retention");
}

// ---- External sinks are typed (plan 2c refinement 8, Task 9) ----
//
// An external sink is a third party's API: the type's numbers (1k/s, 100 ms, service SCV 2.0) plus
// the share of what it serves that FAILS. `failure_rate` is a number in [0, 1] (0.02: one request
// in fifty gets a 5xx; the bounds themselves are meaningful, a provider that never fails and one
// that is down) and `politeness` a rate (how often one host may be called: the crawler's
// "1/s per host"). Both were accepted by name and ignored; now they are read into typed fields.
// A sink has no downstream calls, so `then` and `ops` stay refused as for every type that
// does not list them.

TEST_CASE("an external sink's failure_rate and politeness are typed") {
  const auto path = write_temp("simsys_sink_typed.yaml",
                               scenario_with("  apns: {type: external_sink, latency: 150ms, "
                                             "failure_rate: 0.02, politeness: 1/s}\n"));
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  const auto& sink = s->nodes[0];
  CHECK_THAT(sink.failure_rate, Catch::Matchers::WithinRel(0.02, 1e-12));
  REQUIRE(sink.politeness.has_value());
  CHECK_THAT(sink.politeness->per_second, Catch::Matchers::WithinRel(1.0, 1e-12));
  CHECK(*sink.latency == 150ms);  // the other keys of the node read as before
}

// An absent failure_rate is 0 (the plain queue), an absent politeness stays absent (explain
// prints the note only for a stated one).
TEST_CASE("an external sink that states neither key fails nothing and has no politeness") {
  const auto path =
      write_temp("simsys_sink_untyped.yaml", scenario_with("  api: {type: external_sink}\n"));
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  CHECK(s->nodes[0].failure_rate == 0.0);
  CHECK_FALSE(s->nodes[0].politeness.has_value());
}

// The closed interval: 0 and 1 are both valid (a provider that never fails; one that is down).
TEST_CASE("a failure_rate of exactly 0 or exactly 1 is accepted") {
  for (const auto* rate : {"0", "1"}) {
    CAPTURE(rate);
    const auto path = write_temp(
        "simsys_sink_rate_bounds.yaml",
        scenario_with("  s: {type: external_sink, failure_rate: " + std::string{rate} + "}\n"));
    const auto s = simsys::config::load(path.string());
    INFO((s ? "" : s.error().message));
    REQUIRE(s.has_value());
    CHECK(s->nodes[0].failure_rate == std::stod(rate));
  }
}

TEST_CASE("a failure_rate outside 0 to 1 is refused") {
  for (const auto* rate : {"1.5", "-0.1"}) {
    CAPTURE(rate);
    require_rejected(
        "simsys_sink_rate_range.yaml",
        scenario_with("  s: {type: external_sink, failure_rate: " + std::string{rate} + "}\n"),
        "node 's': failure_rate must be between 0 and 1");
  }
}

// A sink calls nobody: neither `then` nor `ops` is among its keys, so the loader's existing
// message for a key a type does not take applies (pinned here so that giving the sink behaviour
// does not quietly open these keys).
TEST_CASE("an external sink may not have then steps or ops") {
  require_rejected("simsys_sink_then.yaml",
                   scenario_with("  s: {type: external_sink, then: [{to: t, op: x}]}\n"
                                 "  t: {type: service}\n"),
                   "node 's': key 'then' is not valid for type 'external_sink'");
  require_rejected("simsys_sink_ops.yaml",
                   scenario_with("  s: {type: external_sink, ops: {send: [{to: t, op: x}]}}\n"
                                 "  t: {type: service}\n"),
                   "node 's': key 'ops' is not valid for type 'external_sink'");
}

// politeness is a rate one host may be called at: 0/s would be a provider nobody may call, which
// a missing link says more plainly (added by the implementer with the check it pins).
TEST_CASE("an external sink's politeness must be a positive rate") {
  require_rejected("simsys_sink_politeness_zero.yaml",
                   scenario_with("  s: {type: external_sink, politeness: 0/s}\n"),
                   "node 's': politeness must be positive");
}

// ---- kv_cluster is typed (plan 2c refinement 9, Task 10; book ch. 6, Dynamo) ----
//
// A key-value store of `nodes` machines on a consistent-hash ring (`vnodes` virtual nodes each),
// every key kept on `replication` N of them, a write acknowledged after W replicas and a read
// answered after R. `mode` (ap or cp) says what a partition costs (availability or consistency)
// and is read and printed only: it matters in phase 3. The loader reads them into typed fields
// and refuses the shapes that cannot exist: more replicas than machines, a quorum above N.
// Proposed field names (the plan's Files line): NodeSpec::kv_nodes, replication, write_quorum,
// read_quorum, kv_mode (model::KvMode::ap / cp). Tests below compare with `==` only, so the
// fields may be optionals (an absent key stays absent, as every other typed key) or plain ints.

namespace {
// One kv_cluster `store` whose own YAML mapping is `body`.
std::string kv_scenario(std::string_view body) {
  return scenario_with("  store: {type: kv_cluster, " + std::string{body} + "}\n");
}
}  // namespace

// The lab's own line, with `mode: cp` (the lab says "try cp"), as written in labs/kv-store.
TEST_CASE("a kv_cluster's nodes, vnodes, replication, quorums and mode are typed") {
  const auto path = write_temp(
      "simsys_kv_typed.yaml",
      kv_scenario("nodes: 8, vnodes: 150, replication: 3, write_quorum: 2, read_quorum: 2, "
                  "mode: cp, capacity: 10k/s"));
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  const auto& store = s->nodes[0];
  REQUIRE(store.type == simsys::model::NodeType::kv_cluster);
  REQUIRE(store.kv_nodes == 8);
  REQUIRE(store.vnodes == 150);  // the ring's virtual nodes, read for a kv_cluster too
  REQUIRE(store.replication == 3);
  REQUIRE(store.write_quorum == 2);
  REQUIRE(store.read_quorum == 2);
  REQUIRE(store.kv_mode == simsys::model::KvMode::cp);
  REQUIRE(store.replicas == 1);  // the machines are `nodes`; `replicas` stays at its default
}

// Dynamo's classic quorum (ch. 6: N = 3, W = 2, R = 2, so W + R > N) and the AP mode apply when
// the scenario says nothing; the defaults show in explain, marked "(default)", not here
// (test_report pins them). Here: a kv_cluster that states `nodes` alone loads, and what it did not
// state stays absent, the mode included, so explain can tell a default from a choice (review of
// plan 2c Tasks 9-10, minor 9).
TEST_CASE("a kv_cluster that states only its nodes loads and takes Dynamo's defaults") {
  const auto path = write_temp("simsys_kv_nodes_only.yaml", kv_scenario("nodes: 3"));
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  REQUIRE(s->nodes[0].kv_nodes == 3);
  REQUIRE_FALSE(s->nodes[0].replication.has_value());
  REQUIRE_FALSE(s->nodes[0].kv_mode.has_value());
  REQUIRE(simsys::model::kv_mode_of(s->nodes[0]) == simsys::model::KvMode::ap);
}

// Which ops are writes, and so wait for W replicas instead of R, is the node's `write_ops`, as on
// a cache, a CDN, a SQL database and a blob store (review of plan 2c Tasks 9-10, I-3). The graph
// and the engine always honoured it; the loader refused it, so a client sending `put` straight to
// a kv_cluster silently got the read quorum.
TEST_CASE("a kv_cluster lists its write ops") {
  const auto path =
      write_temp("simsys_kv_write_ops.yaml", kv_scenario("nodes: 3, write_ops: [put, delete]"));
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  REQUIRE(s->nodes[0].write_ops == std::vector<std::string>{"put", "delete"});
}

TEST_CASE("both modes are accepted, as ap and as cp") {
  for (const auto& [text, mode] :
       {std::pair{"ap", simsys::model::KvMode::ap}, std::pair{"cp", simsys::model::KvMode::cp}}) {
    CAPTURE(text);
    const auto path =
        write_temp("simsys_kv_mode.yaml", kv_scenario("nodes: 3, mode: " + std::string{text}));
    const auto s = simsys::config::load(path.string());
    INFO((s ? "" : s.error().message));
    REQUIRE(s.has_value());
    REQUIRE(s->nodes[0].kv_mode == mode);
  }
}

// W + R <= N is legal: no read is guaranteed to meet the latest write, which is exactly the
// eventually consistent end of the dial (ch. 6: W = 1 is a fast write, R = 1 a fast read). The
// loader does not insist on W + R > N.
TEST_CASE("weak quorums where W plus R does not exceed N are accepted") {
  const auto path =
      write_temp("simsys_kv_weak_quorum.yaml",
                 kv_scenario("nodes: 5, replication: 3, write_quorum: 1, read_quorum: 1"));
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
}

// A quorum may be as large as N itself (write to all, read from all: the strongest and the
// slowest setting) and N as large as the cluster (every node holds every key).
TEST_CASE("replication equal to nodes and quorums equal to replication are accepted") {
  const auto path =
      write_temp("simsys_kv_full_quorum.yaml",
                 kv_scenario("nodes: 3, replication: 3, write_quorum: 3, read_quorum: 3"));
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
}

TEST_CASE("a kv_cluster needs nodes") {
  require_rejected("simsys_kv_no_nodes.yaml", kv_scenario("replication: 3"),
                   "node 'store': kv_cluster needs nodes");
}

// The counts are whole numbers of at least one, in the words of every other positive count.
TEST_CASE("a kv_cluster's counts must be positive whole numbers") {
  SECTION("nodes of zero") {
    require_rejected("simsys_kv_nodes_zero.yaml", kv_scenario("nodes: 0"),
                     "node 'store': nodes must be a positive integer");
  }
  SECTION("replication of zero") {
    require_rejected("simsys_kv_replication_zero.yaml", kv_scenario("nodes: 3, replication: 0"),
                     "node 'store': replication must be a positive integer");
  }
  SECTION("write_quorum of zero") {
    require_rejected("simsys_kv_w_zero.yaml", kv_scenario("nodes: 3, write_quorum: 0"),
                     "node 'store': write_quorum must be a positive integer");
  }
  SECTION("read_quorum of zero") {
    require_rejected("simsys_kv_r_zero.yaml", kv_scenario("nodes: 3, read_quorum: 0"),
                     "node 'store': read_quorum must be a positive integer");
  }
  SECTION("vnodes of zero") {
    require_rejected("simsys_kv_vnodes_zero.yaml", kv_scenario("nodes: 3, vnodes: 0"),
                     "node 'store': vnodes must be a positive integer");
  }
}

// N copies need N machines to live on; a quorum is a number of those N replicas. The message
// carries both numbers, "(5 > 3)": the offending value, then the limit it broke.
TEST_CASE("replication above nodes and a quorum above replication are refused") {
  SECTION("replication above nodes") {
    require_rejected("simsys_kv_n_above_nodes.yaml", kv_scenario("nodes: 3, replication: 5"),
                     "node 'store': replication must be at most nodes (5 > 3)");
  }
  SECTION("the default replication of 3 above a two-node cluster") {
    // N defaults to 3 (Dynamo's), so a cluster of two that does not lower it is refused.
    require_rejected("simsys_kv_default_n_above_nodes.yaml", kv_scenario("nodes: 2"),
                     "node 'store': replication must be at most nodes (3 > 2)");
  }
  SECTION("write_quorum above replication") {
    require_rejected("simsys_kv_w_above_n.yaml",
                     kv_scenario("nodes: 5, replication: 3, write_quorum: 4"),
                     "node 'store': write_quorum must be at most replication (4 > 3)");
  }
  SECTION("read_quorum above replication") {
    require_rejected("simsys_kv_r_above_n.yaml",
                     kv_scenario("nodes: 5, replication: 3, read_quorum: 4"),
                     "node 'store': read_quorum must be at most replication (4 > 3)");
  }
}

TEST_CASE("a kv_cluster's mode must be ap or cp") {
  require_rejected("simsys_kv_mode_strong.yaml", kv_scenario("nodes: 3, mode: strong"),
                   "node 'store': mode must be ap or cp");
}

// `replicas` is the copies of ONE machine behind a balancer, a service's way to scale. A
// kv_cluster scales by `nodes` (machines on the ring) and keeps `replication` copies of every
// key: three things that `replicas` could mean at once, so it is refused here, like a worker
// pool's `replicas`, rather than guessed at.
TEST_CASE("a kv_cluster refuses replicas: its machines are nodes") {
  require_rejected("simsys_kv_replicas.yaml", kv_scenario("nodes: 3, replicas: 3"),
                   "node 'store': a kv_cluster's machines are nodes; replicas is not a key here");
}

// ---- Search indexes (plan 2c refinement 10, Task 11) ----
//
// A search index (ch. 13's trie database) serves like a service, and `build_interval` is the one
// key it owns: how often the index is rebuilt from the aggregated logs. Until now the loader
// accepted it and dropped it; it is now a typed, positive duration, stored for explain to print
// (the rebuild's load and the index's freshness are not modelled). `ops` stays refused: an index
// answers a query, it calls nobody, so it has no steps to list.

namespace {
std::string search_index_scenario(std::string_view keys) {
  return scenario_with("  trie_db: {type: search_index" +
                       (keys.empty() ? std::string{} : ", " + std::string{keys}) + "}\n");
}
}  // namespace

TEST_CASE("a search index's build_interval loads as a duration") {
  const auto path = write_temp("simsys_search_build_interval.yaml",
                               search_index_scenario("replicas: 4, build_interval: 24h"));
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  REQUIRE(s->nodes[0].type == simsys::model::NodeType::search_index);
  REQUIRE(s->nodes[0].build_interval == 24h);
  REQUIRE(s->nodes[0].replicas == 4);  // the service keys are read as on a service
}

// Absent stays absent: explain prints the line only for a stated interval, and the type has no
// default one to apply.
TEST_CASE("a search index that states no build_interval leaves it absent") {
  const auto path = write_temp("simsys_search_no_build_interval.yaml", search_index_scenario(""));
  const auto s = simsys::config::load(path.string());
  INFO((s ? "" : s.error().message));
  REQUIRE(s.has_value());
  REQUIRE_FALSE(s->nodes[0].build_interval.has_value());
}

TEST_CASE("a search index's build_interval must be positive") {
  require_rejected("simsys_search_build_interval_zero.yaml",
                   search_index_scenario("build_interval: 0s"),
                   "node 'trie_db': build_interval must be positive");
}

// The refusal that already stands, pinned so that serving like a service does not turn `ops` on:
// the key is not valid for the type.
TEST_CASE("a search index still refuses ops") {
  require_rejected("simsys_search_ops.yaml",
                   search_index_scenario("ops: {suggest: [{to: other, op: read}]}"),
                   "node 'trie_db': key 'ops' is not valid for type 'search_index'");
}
