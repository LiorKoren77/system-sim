// The typed scenario vocabulary: node types are an enum with a two-way string mapping,
// and specs carry the schema's defaults.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.core;
import simsys.model;

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
  static_assert(simsys::model::node_type_names.size() == 18);
  static_assert(simsys::model::parse_node_type("service") == NodeType::service);
  static_assert(simsys::model::to_string(NodeType::frontier) == "frontier");
  SUCCEED();
}

TEST_CASE("a default NodeSpec has one replica in datacenter main and no overrides", "[model]") {
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

TEST_CASE("a default LinkSpec is outbound, carries all ops, weight one, no latency override", "[model]") {
  const simsys::model::LinkSpec l{.from = "a", .to = "b"};
  REQUIRE(l.dir == simsys::model::Direction::out);
  REQUIRE(l.ops.empty());
  REQUIRE(l.weight == 1.0);
  REQUIRE_FALSE(l.latency.has_value());
}
