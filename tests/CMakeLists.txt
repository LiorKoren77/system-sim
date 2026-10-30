# Tests are teaching material: name each TEST_CASE after the property it proves.
list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
include(Catch)

# The builders more than one test file needs (an M/M/1 probe, the pipe and message-queue pictures,
# a fresh simulation, explain of a scenario built in code), as the named module
# simsys.test_fixtures: compiled once here instead of once per test file that would include them.
add_library(simsys_test_support)
target_sources(simsys_test_support
  PUBLIC FILE_SET CXX_MODULES FILES support/fixtures.cppm
  PRIVATE support/fixtures.cpp)
# Catch2::Catch2 (not ...WithMain): the fixtures call REQUIRE and FAIL; main() is the tests'.
target_link_libraries(simsys_test_support PUBLIC simsys Catch2::Catch2 PRIVATE simsys_warnings)
# GCC 14.2's fourth defect (CLAUDE.md §3), release link: fixtures.cpp emits the special members of
# std templates over model types that a test executable calls but no object defines; -fno-inline
# keeps its calls to them out of line, so its object file defines them (as for src/model/model.cpp).
set_source_files_properties(support/fixtures.cpp PROPERTIES COMPILE_OPTIONS -fno-inline)

add_executable(unit_tests unit/test_units.cpp unit/test_config.cpp unit/test_queueing.cpp unit/test_flow.cpp unit/test_fluid.cpp unit/test_workload.cpp unit/test_caching.cpp unit/test_model.cpp unit/test_defaults.cpp unit/test_graph.cpp unit/test_behaviours.cpp unit/test_engine.cpp unit/test_report.cpp unit/test_series.cpp unit/test_sweep.cpp unit/test_session.cpp)
target_link_libraries(unit_tests PRIVATE simsys simsys_test_support Catch2::Catch2WithMain simsys_warnings)
# test_series.cpp explains shipped scenarios and test_sweep.cpp sweeps tutorial/hello, so they need
# the directory, like scenario_tests.
target_compile_definitions(unit_tests PRIVATE SIMSYS_SCENARIOS_DIR="${CMAKE_SOURCE_DIR}/scenarios")
catch_discover_tests(unit_tests)

add_executable(scenario_tests scenario/test_scenarios.cpp)
target_link_libraries(scenario_tests PRIVATE simsys simsys_test_support Catch2::Catch2WithMain simsys_warnings)
target_compile_definitions(scenario_tests PRIVATE SIMSYS_SCENARIOS_DIR="${CMAKE_SOURCE_DIR}/scenarios")
catch_discover_tests(scenario_tests)

# The theory tests, one file per topic (closed forms checked against whole simulations).
add_executable(theory_tests
  theory/test_core.cpp
  theory/test_database_cdn.cpp
  theory/test_bytes_pipes.cpp
  theory/test_service_sugar.cpp
  theory/test_queues_pools.cpp
  theory/test_sinks.cpp
  theory/test_kv.cpp
  theory/test_failed_steps.cpp
  theory/test_retries.cpp
  theory/test_rho_one.cpp
  theory/test_timeout.cpp
  theory/test_faults.cpp
  theory/test_windows.cpp)
target_link_libraries(theory_tests PRIVATE simsys simsys_test_support Catch2::Catch2WithMain simsys_warnings)
# test_timeout.cpp pins the shipped youtube scenario's uploads and test_windows.cpp the shipped
# url-shortener's hours, so they need the directory too.
target_compile_definitions(theory_tests PRIVATE SIMSYS_SCENARIOS_DIR="${CMAKE_SOURCE_DIR}/scenarios")
catch_discover_tests(theory_tests)

# simsys itself, run as a process: what `run` does to the series files on disk. One scratch
# directory per check, so `ctest -j` runs them side by side.
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

# simsys sweep, run as a process: the output does not depend on --jobs, a bad --param is an error
# (exit 1), a failing SLO at some point is not (exit 0). One scratch directory per check, so `ctest
# -j` runs them side by side.
foreach(check same_bytes refuses_bad_param error_prefixes slo_failure_exit_0)
  if(check STREQUAL "same_bytes")
    set(name "simsys sweep gives the same bytes with --jobs 1 and 3")
  elseif(check STREQUAL "refuses_bad_param")
    set(name "simsys sweep exits 1 on a bad --param")
  elseif(check STREQUAL "error_prefixes")
    set(name "simsys sweep words every option refusal as file, error, then the option")
  else()
    set(name "simsys sweep exits 0 when an SLO fails at some point")
  endif()
  add_test(NAME "${name}"
           COMMAND ${CMAKE_COMMAND} -DSIMSYS=$<TARGET_FILE:simsys_cli>
                   -DSCENARIO=${CMAKE_SOURCE_DIR}/scenarios/tutorial/hello/scenario.yaml
                   -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli-sweep-${check} -DCHECK=${check}
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/sweep_jobs.cmake)
endforeach()

# simsys run --duration, run as a process: the override is held to the file's own rules, the warmup
# and every SLO's `over` window.
foreach(check warmup over)
  if(check STREQUAL "warmup")
    set(name "simsys run refuses a duration not longer than the warmup")
  else()
    set(name "simsys run refuses a duration that leaves an SLO's over window no room")
  endif()
  add_test(NAME "${name}"
           COMMAND ${CMAKE_COMMAND} -DSIMSYS=$<TARGET_FILE:simsys_cli>
                   -DSCENARIO=${CMAKE_SOURCE_DIR}/scenarios/book/url-shortener/scenario.yaml
                   -DCHECK=${check} -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/run_duration.cmake)
endforeach()

# simsys curve, run as a process: a traffic_spike fault shows on the client's curve exactly as the
# same `spikes` entry does (phase 3 Task 6's review).
add_test(NAME "simsys curve applies a traffic_spike fault exactly as a spikes entry"
         COMMAND ${CMAKE_COMMAND} -DSIMSYS=$<TARGET_FILE:simsys_cli>
                 -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli-curve-faults
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/curve_faults.cmake)

# simsys run --replications, run as a process: the median and the 10-90 % band across seeds, the
# refusals, the exit code of the median run, and that --jobs cannot show in the output. One scratch
# directory per check, so `ctest -j` runs them side by side.
foreach(check bands same_bytes json exit_codes noiseless one_is_plain refusals series_refused
              fault_once slo_median_seeds noiseless_json_note sigma_zero jobs_help)
  if(check STREQUAL "bands")
    set(name "simsys run --replications prints each metric's median and band and PASS in k of N")
  elseif(check STREQUAL "same_bytes")
    set(name "simsys run --replications gives the same bytes with --jobs 1, 3 and 4")
  elseif(check STREQUAL "json")
    set(name "simsys run --replications adds the seeds and the per-seed values to the JSON")
  elseif(check STREQUAL "exit_codes")
    set(name "simsys run --replications exits 2 only when an SLO's median run fails")
  elseif(check STREQUAL "noiseless")
    set(name "simsys run --replications runs a scenario without noise once and says so first")
  elseif(check STREQUAL "one_is_plain")
    set(name "simsys run --replications 1 is the plain run byte for byte")
  elseif(check STREQUAL "refusals")
    set(name "simsys run refuses a replication count outside 1 to 1000")
  elseif(check STREQUAL "series_refused")
    set(name "simsys run refuses a series file with more than one replication")
  elseif(check STREQUAL "fault_once")
    set(name "simsys run --replications prints a scripted fault's block once")
  elseif(check STREQUAL "slo_median_seeds")
    set(name "simsys run --replications names each SLO's own median seed and exits 2 when one fails")
  elseif(check STREQUAL "noiseless_json_note")
    set(name "simsys run --replications --report json without noise puts the note on stderr")
  elseif(check STREQUAL "sigma_zero")
    set(name "simsys run --replications treats noise of sigma 0 as no noise and runs once")
  else()
    set(name "simsys run --help says --jobs is ignored without --replications")
  endif()
  add_test(NAME "${name}"
           COMMAND ${CMAKE_COMMAND} -DSIMSYS=$<TARGET_FILE:simsys_cli>
                   -DSCENARIO=${CMAKE_SOURCE_DIR}/scenarios/tutorial/hello/scenario.yaml
                   -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli-replications-${check} -DCHECK=${check}
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/cli/replications.cmake)
endforeach()
