# Tests are teaching material: name each TEST_CASE after the property it proves.
list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
include(Catch)

add_executable(unit_tests unit/test_units.cpp unit/test_config.cpp unit/test_queueing.cpp unit/test_flow.cpp unit/test_fluid.cpp unit/test_workload.cpp unit/test_caching.cpp unit/test_model.cpp unit/test_defaults.cpp unit/test_graph.cpp unit/test_behaviours.cpp unit/test_engine.cpp unit/test_report.cpp unit/test_series.cpp)
target_link_libraries(unit_tests PRIVATE simsys Catch2::Catch2WithMain simsys_warnings)
# test_series.cpp explains shipped scenarios, so it needs the directory, like scenario_tests.
target_compile_definitions(unit_tests PRIVATE SIMSYS_SCENARIOS_DIR="${CMAKE_SOURCE_DIR}/scenarios")
catch_discover_tests(unit_tests)

add_executable(scenario_tests scenario/test_scenarios.cpp)
target_link_libraries(scenario_tests PRIVATE simsys Catch2::Catch2WithMain simsys_warnings)
target_compile_definitions(scenario_tests PRIVATE SIMSYS_SCENARIOS_DIR="${CMAKE_SOURCE_DIR}/scenarios")
catch_discover_tests(scenario_tests)

add_executable(theory_tests theory/test_theory.cpp)
target_link_libraries(theory_tests PRIVATE simsys Catch2::Catch2WithMain simsys_warnings)
catch_discover_tests(theory_tests)

# simsys itself, run as a process: what `run` does to the series files on disk (review of
# 3e23477, M1 and M2). One scratch directory per check, so `ctest -j` runs them side by side.
foreach(check same_file keeps_file)
  if(check STREQUAL "same_file")
    set(name "simsys run refuses one file for both series")
  else()
    set(name "simsys run leaves the series file untouched when the op series cannot be opened")
  endif()
  add_test(NAME "${name}"
           COMMAND ${CMAKE_COMMAND} -DSIMSYS=$<TARGET_FILE:simsys_cli>
                   -DSCENARIO=${CMAKE_SOURCE_DIR}/scenarios/tutorial/hello/scenario.yaml
                   -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli-${check} -DCHECK=${check}
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/series_paths.cmake)
endforeach()
