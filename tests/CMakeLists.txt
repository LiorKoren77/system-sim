# Tests are teaching material: name each TEST_CASE after the property it proves.
list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
include(Catch)

add_executable(unit_tests unit/test_units.cpp unit/test_config.cpp unit/test_queueing.cpp unit/test_flow.cpp)
target_link_libraries(unit_tests PRIVATE simsys Catch2::Catch2WithMain simsys_warnings)
catch_discover_tests(unit_tests)

add_executable(scenario_tests scenario/test_scenarios.cpp)
target_link_libraries(scenario_tests PRIVATE simsys Catch2::Catch2WithMain simsys_warnings)
target_compile_definitions(scenario_tests PRIVATE SIMSYS_SCENARIOS_DIR="${CMAKE_SOURCE_DIR}/scenarios")
catch_discover_tests(scenario_tests)
