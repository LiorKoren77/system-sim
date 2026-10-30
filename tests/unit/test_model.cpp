// The typed scenario vocabulary: node types are an enum with a two-way string mapping,
// and specs carry the schema's defaults.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.core;
import simsys.model;
import simsys.workload;

using namespace std::chrono_literals;
using simsys::model::NodeType;

TEST_CASE("every node type name round-trips through parse_node_type and to_string", "[model]") {
  for (const auto name : simsys::model::node_type_names) {
    CAPTURE(name);
    const auto type = simsys::model::parse_node_type(name);
    REQUIRE(type.has_value());
    REQUIRE(simsys::model::to_string(*type) == name);
  }
}

TEST_CASE("parse_node_type rejects unknown names", "[model]") {
  REQUIRE_FALSE(simsys::model::parse_node_type("mainframe").has_value());
}

TEST_CASE("the node type table is complete at compile time", "[model]") {
  static_assert(simsys::core::follows_enum_order(simsys::model::node_type_table));
  static_assert(simsys::model::parse_node_type("service") == NodeType::service);
  static_assert(simsys::model::to_string(NodeType::frontier) == "frontier");
  SUCCEED();
}

// The node-type table's third column says which types run/estimate can step today: every type but
// the two whose models phase 3 still has to add (a service_discovery runs since Task 13: a
// service that answers `lookup`; a ws_gateway since Task 14). Simulation::create reads it
// (model::simulated), and the scenario tests pin the shipped scenarios it lets run.
TEST_CASE("every node type runs but the two that phase 3 has still to model", "[model]") {
  for (const auto& row : simsys::model::node_type_table) {
    CAPTURE(row.name);
    const bool still_to_model =
        row.value == NodeType::notification_fanout || row.value == NodeType::frontier;
    REQUIRE(simsys::model::simulated(row.value) == !still_to_model);
  }
  static_assert(simsys::model::simulated(NodeType::service));
  static_assert(!simsys::model::simulated(NodeType::frontier));
}

TEST_CASE("a service discovery is a node type the engine runs", "[model]") {
  REQUIRE(simsys::model::simulated(NodeType::service_discovery));
}

namespace {
// Two broken tables, to show that round_trips catches both ways a table can be wrong.
enum class Colour { red, green };
// C++20 std::to_array: builds a std::array from a braced list, deducing its length, so
// the table can grow without a hand-counted size; replaces `std::array<T, N>{...}` where N
// had to be kept in step with the rows.
constexpr auto duplicated_name = std::to_array<simsys::core::EnumName<Colour>>({
    {Colour::red, "red"},
    {Colour::green, "red"},
});
constexpr auto out_of_order = std::to_array<simsys::core::EnumName<Colour>>({
    {Colour::green, "green"},
    {Colour::red, "red"},
});
}  // namespace

// static_assert: the proof runs while compiling, so a broken table never builds; the test
// case only gives the proof a name students can find.
TEST_CASE("every enum name table round-trips and ends at its last enumerator", "[model]") {
  using simsys::core::round_trips;
  static_assert(round_trips(simsys::model::node_type_table));
  static_assert(round_trips(simsys::model::direction_table));
  static_assert(round_trips(simsys::model::slo_metric_table));
  static_assert(round_trips(simsys::model::step_condition_table));
  static_assert(round_trips(simsys::model::step_mode_table));
  static_assert(round_trips(simsys::workload::wave_shape_table));
  // Nothing can count an enum's enumerators, so "the table is complete" is pinned by its
  // last row: a new enumerator appended without a row breaks one of these.
  static_assert(simsys::model::node_type_table.back().value == NodeType::frontier);
  static_assert(simsys::model::direction_table.back().value == simsys::model::Direction::inout);
  static_assert(simsys::model::slo_metric_table.back().value ==
                simsys::model::SloMetric::dead_letter_rate);
  static_assert(simsys::model::step_condition_table.back().value ==
                simsys::model::StepCondition::failure);
  static_assert(simsys::model::step_mode_table.back().value == simsys::model::StepMode::parallel);
  static_assert(simsys::workload::wave_shape_table.back().value ==
                simsys::workload::WaveShape::diurnal);
  static_assert(!round_trips(duplicated_name));
  static_assert(!round_trips(out_of_order));
  SUCCEED();
}

TEST_CASE("a default NodeSpec has one replica in datacenter main and no overrides", "[model]") {
  // C++20 designated initializers: `.field = value` names each member it sets; every
  // other member keeps its default, which is exactly what this test inspects. Replaces
  // positional aggregate initialization, where the reader counts fields to see which
  // value lands where.
  const simsys::model::NodeSpec n{.id = "x", .type = NodeType::service};
  REQUIRE(n.replicas == 1);
  REQUIRE(n.dc == "main");
  REQUIRE_FALSE(n.capacity.has_value());
  REQUIRE_FALSE(n.latency.has_value());
  REQUIRE_FALSE(n.workload.has_value());
  REQUIRE(n.ops.empty());
}

TEST_CASE("a default Scenario steps every 100 ms with seed 1 in one datacenter", "[model]") {
  const simsys::model::Scenario s;
  REQUIRE(s.step == 100ms);
  REQUIRE(s.seed == 1);
  REQUIRE(s.warmup == 0s);
  REQUIRE(s.datacenters == std::vector<std::string>{"main"});
}

TEST_CASE("a default Step forwards unconditionally, sequentially, with factor one", "[model]") {
  const simsys::model::Step st{.to = "db"};
  REQUIRE(st.on == simsys::model::StepCondition::always);
  REQUIRE(st.mode == simsys::model::StepMode::sequential);
  REQUIRE(st.factor == 1.0);
}

TEST_CASE("a default LinkSpec is outbound, carries all ops, weight one, no latency override",
          "[model]") {
  const simsys::model::LinkSpec l{.from = "a", .to = "b"};
  REQUIRE(l.dir == simsys::model::Direction::out);
  REQUIRE(l.ops.empty());
  REQUIRE(l.weight == 1.0);
  REQUIRE_FALSE(l.latency.has_value());
}

// `write_ops` absent means "the type's default", present means exactly the list written. One
// helper, model::write_ops_of, applies the default, so the loader, the graph, the behaviours and
// explain cannot disagree: a cache writes with `write`, a database with the four SQL statements
// that change rows; a database told `[write]` has exactly that one write op.
TEST_CASE("write_ops absent is the type's default and present is exactly what was written",
          "[model]") {
  using simsys::model::NodeSpec;
  using simsys::model::write_ops_of;
  REQUIRE(write_ops_of(NodeSpec{.type = NodeType::cache}) == std::vector<std::string>{"write"});
  REQUIRE(write_ops_of(NodeSpec{.type = NodeType::sql_db}) ==
          std::vector<std::string>{"delete", "insert", "update", "write"});
  NodeSpec db{.type = NodeType::sql_db};
  db.write_ops = std::vector<std::string>{"write"};
  REQUIRE(write_ops_of(db) == std::vector<std::string>{"write"});
  NodeSpec cache{.type = NodeType::cache};
  cache.write_ops = std::vector<std::string>{"set", "delete"};
  REQUIRE(write_ops_of(cache) == std::vector<std::string>{"set", "delete"});
}

// ---- ID generator (spec §18 entry 12; book ch. 7 "Design a unique ID generator") ----
//
// The three designs the chapter compares are a closed set, spelled in scenario files as
// `algorithm: snowflake | ticket_server | uuid`; one name table drives both directions, exactly
// as for the rate limiter's algorithms.

// static_assert: proven while compiling, so a table with a missing, duplicated or misordered row
// never builds; the last row pins "the table is complete", since nothing can count an enum's
// enumerators.
TEST_CASE("the id algorithm table round-trips and ends at its last enumerator", "[model]") {
  static_assert(simsys::core::round_trips(simsys::model::id_algorithm_table));
  static_assert(simsys::model::id_algorithm_table.back().value == simsys::model::IdAlgorithm::uuid);
  SUCCEED();
}

TEST_CASE("every id algorithm name parses to its enumerator and prints back unchanged", "[model]") {
  using simsys::model::IdAlgorithm;
  using simsys::model::parse_id_algorithm;
  using simsys::model::to_string;
  REQUIRE(parse_id_algorithm("snowflake") == IdAlgorithm::snowflake);
  REQUIRE(parse_id_algorithm("ticket_server") == IdAlgorithm::ticket_server);
  REQUIRE(parse_id_algorithm("uuid") == IdAlgorithm::uuid);
  REQUIRE(to_string(IdAlgorithm::snowflake) == "snowflake");
  REQUIRE(to_string(IdAlgorithm::ticket_server) == "ticket_server");
  REQUIRE(to_string(IdAlgorithm::uuid) == "uuid");
}

TEST_CASE("parse_id_algorithm rejects names the chapter does not compare", "[model]") {
  // "multi_master" is a real ch. 7 design, but the model has no behaviour for it; "UUID" is the
  // right idea spelled in a case the scenario grammar (all lower case) does not accept.
  REQUIRE_FALSE(simsys::model::parse_id_algorithm("multi_master").has_value());
  REQUIRE_FALSE(simsys::model::parse_id_algorithm("UUID").has_value());
  REQUIRE_FALSE(simsys::model::parse_id_algorithm("").has_value());
}

TEST_CASE("a default NodeSpec has no id algorithm", "[model]") {
  const simsys::model::NodeSpec n{.id = "x", .type = NodeType::id_generator};
  REQUIRE_FALSE(n.id_algorithm.has_value());
}

// Request sizes and link bandwidth are optional in the scenario (phase 2c): a client states
// `sizes:` only for ops that carry bytes, a step `size:` only when it carries its own payload, a
// link `bandwidth:` only when the pipe is a constraint. Absent means "not stated", never zero, so
// the graph can tell an unsized op (an error at a byte-sized node) from a zero-byte one.
TEST_CASE("a default Workload states no request sizes", "[model]") {
  const simsys::model::Workload w{};
  REQUIRE(w.sizes.empty());
}

TEST_CASE("a default Step states no size of its own", "[model]") {
  const simsys::model::Step st{.to = "db"};
  REQUIRE_FALSE(st.size.has_value());
}

TEST_CASE("a default LinkSpec states no bandwidth", "[model]") {
  const simsys::model::LinkSpec l{.from = "a", .to = "b"};
  REQUIRE_FALSE(l.bandwidth.has_value());
}

// ---- Message queues and worker pools (spec §19 entries 4-5) ----
//
// The five keys the loader types for the two asynchronous-work node types. All are optionals:
// "absent" means "the type's default" (the retention) or "not stated" (the rest), so that explain
// can say `(default: ...)` and the graph can apply one default through `defaults`.
//   worker_pool:    workers   the pool's c, a positive int (M/M/c's c);
//                   job_cost  the time one worker spends on one job, a positive DURATION (the
//                             pool's `cost`: not the per-op capacity-unit map of a service, which
//                             stays `NodeSpec::cost`);
//                   consumes  the id of the queue the pool drains;
//   message_queue:  retention how long a message stays before it is deleted unconsumed;
//                   partitions printed, not modelled.
TEST_CASE("a default NodeSpec states no workers, job cost, queue, retention or partitions",
          "[model]") {
  const simsys::model::NodeSpec n{.id = "x", .type = NodeType::worker_pool};
  REQUIRE_FALSE(n.workers.has_value());
  REQUIRE_FALSE(n.job_cost.has_value());
  REQUIRE_FALSE(n.consumes.has_value());
  REQUIRE_FALSE(n.retention.has_value());
  REQUIRE_FALSE(n.partitions.has_value());
}

TEST_CASE(
    "a worker pool's workers, job cost and queue, and a queue's retention and partitions "
    "are typed fields",
    "[model]") {
  simsys::model::NodeSpec pool{.id = "transcoders", .type = NodeType::worker_pool};
  pool.workers = 400;
  pool.job_cost = 60s;  // a duration per job; youtube's transcoders take a minute per video
  pool.consumes = "transcode_q";
  REQUIRE(*pool.workers == 400);
  REQUIRE(*pool.job_cost == 60s);
  REQUIRE(*pool.consumes == "transcode_q");
  simsys::model::NodeSpec queue{.id = "transcode_q", .type = NodeType::message_queue};
  queue.retention = 168h;
  queue.partitions = 32;
  REQUIRE(*queue.retention == 168h);
  REQUIRE(*queue.partitions == 32);
  // The pool's `cost` is a duration here and a map of capacity units on a service: two members.
  REQUIRE(pool.cost.empty());
}

// ---- External sinks (spec §19 entry 8) ----
//
// An external sink is a third party's API (APNs, an SMS provider): a queue of the type's numbers
// that fails a share of what it serves. Two typed keys describe it:
//   failure_rate  the share, in [0, 1], of the requests it serves that fail (a 5xx); a plain
//                 double because "no failures" is a real value, 0, not an absent one: a sink that
//                 states none behaves as the queue it always was;
//   politeness    a rate (the crawler's "one request per host per second"), stored and printed,
//                 not modelled (phase 3, it needs per-host state); absent when not stated.
TEST_CASE("a default NodeSpec fails nothing and states no politeness", "[model]") {
  const simsys::model::NodeSpec n{.id = "apns", .type = NodeType::external_sink};
  REQUIRE(n.failure_rate == 0.0);
  REQUIRE_FALSE(n.politeness.has_value());
}

TEST_CASE("an external sink's failure rate is a number and its politeness a rate", "[model]") {
  simsys::model::NodeSpec sink{.id = "sms_provider", .type = NodeType::external_sink};
  sink.failure_rate = 0.05;  // the notification system's SMS provider: 5 % of sends fail
  sink.politeness = simsys::Rate{1.0};
  REQUIRE(sink.failure_rate == 0.05);
  REQUIRE(sink.politeness->per_second == 1.0);
}

// ---- Search indexes (spec §19 entry 10) ----
//
// A search index serves like a service (read-mostly, spec section 7), and the one key it owns,
// `build_interval`, says how often the index is rebuilt from the logs (ch. 13: the trie is rebuilt
// weekly from aggregated query logs; search-autocomplete's file says 168h). It is stored and
// printed, not modelled, and absent means "not stated", never zero.
TEST_CASE("a default NodeSpec states no build interval", "[model]") {
  const simsys::model::NodeSpec n{.id = "trie_db", .type = NodeType::search_index};
  REQUIRE_FALSE(n.build_interval.has_value());
}

TEST_CASE("a search index's build interval is a typed duration", "[model]") {
  simsys::model::NodeSpec index{.id = "trie_db", .type = NodeType::search_index};
  index.build_interval = 168h;  // search-autocomplete's file: rebuilt every week
  REQUIRE(*index.build_interval == 168h);
}

// ---- Scripted faults (phase 3b, design spec 4.1) ----
//
// A fault is a scripted event the scenario lists under `faults:`: at a time, a node goes down,
// slows, loses capacity, a cache is flushed, a client's traffic spikes, or a whole datacenter is
// lost. The model only carries it; the loader fills it and the engine applies it.
TEST_CASE("a default Scenario scripts no faults", "[model]") {
  const simsys::model::Scenario s{};
  REQUIRE(s.faults.empty());
}

TEST_CASE("the fault kind table round-trips and ends at its last enumerator", "[model]") {
  using simsys::model::FaultKind;
  // The same compile-time proof as every other name table (see the test above): a duplicated or
  // misordered row, or an enumerator appended without a row, breaks the build.
  static_assert(simsys::core::round_trips(simsys::model::fault_kind_table));
  static_assert(simsys::model::fault_kind_table.back().value == FaultKind::dc_down);
  SUCCEED();
}

TEST_CASE("every fault kind name round-trips through parse_fault_kind and to_string", "[model]") {
  using simsys::model::FaultKind;
  const std::pair<FaultKind, std::string_view> kinds[] = {
      {FaultKind::node_down, "node_down"},         {FaultKind::capacity, "capacity"},
      {FaultKind::latency_spike, "latency_spike"}, {FaultKind::cache_flush, "cache_flush"},
      {FaultKind::traffic_spike, "traffic_spike"}, {FaultKind::dc_down, "dc_down"},
  };
  for (const auto& [kind, name] : kinds) {
    CAPTURE(name);
    REQUIRE(simsys::model::to_string(kind) == name);
    REQUIRE(simsys::model::parse_fault_kind(name) == kind);
  }
}

TEST_CASE("parse_fault_kind rejects a kind the schema does not have", "[model]") {
  // `partition` is on the backlog (kv_cluster partitions), not a kind of phase 3.
  REQUIRE_FALSE(simsys::model::parse_fault_kind("partition").has_value());
  REQUIRE_FALSE(simsys::model::parse_fault_kind("").has_value());
}

TEST_CASE("a Fault carries what breaks, where, when and for how long", "[model]") {
  using simsys::model::FaultKind;
  // The field names are the schema's keys (`for` is a C++ keyword, so `duration`).
  const simsys::model::Fault f{
      .kind = FaultKind::node_down, .at = 21h + 30min, .duration = 1min, .node = "chat_servers"};
  REQUIRE(f.kind == FaultKind::node_down);
  REQUIRE(f.at == 21h + 30min);
  REQUIRE(f.duration == 1min);
  REQUIRE(f.node == "chat_servers");
}
