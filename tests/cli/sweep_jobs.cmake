# Process-level checks of `simsys sweep`: that the number of workers cannot show in the output, and
# what the exit code says, can only be seen from outside the process. Registered in
# tests/CMakeLists.txt, one ctest entry per CHECK.
#
# Run by ctest as a CMake script:
#   cmake -DSIMSYS=<simsys> -DSCENARIO=<hello's scenario.yaml> -DWORK=<scratch dir> -DCHECK=<name>
#         -P sweep_jobs.cmake
# CHECK picks one property:
#   same_bytes        - `sweep hello --param nodes.users.rate.base=100/s..900/s:3 --metric
#                       users.ping.mean --metric api.mean_rho` prints the same bytes with
#                       --jobs 1 and --jobs 3, as a table and as CSV; the CSV is a header and
#                       three rows (4 lines) with the right values (1.61 ms, 2.5 ms, 10.5 ms,
#                       in seconds; rho 0.1, 0.5, 0.9);
#   refuses_bad_param - a spec that breaks the grammar, a count that is not whole, a key the
#                       file does not have and a value the loader refuses each exit 1, with the
#                       reason on stderr and nothing on stdout;
#   slo_failure_exit_0 - a sweep where an SLO fails at every point still exits 0 (`run` on the
#                       same file exits 2): a sweep reports numbers, it does not judge them;
#   error_prefixes    - every refusal of an option starts as the CLI's other errors do, with the
#                       file and `error:`, then names its option the same way, `--param: ` or
#                       `--metric: `, whether the text, the file or the loader found the mistake.
cmake_minimum_required(VERSION 3.28)

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

set(PARAM nodes.users.rate.base=100/s..900/s:3)

# Runs `simsys sweep <scenario> ARGN` and sets `sweep_code`, `sweep_out` and `sweep_err` in the
# caller's scope.
function(run_sweep scenario)
  execute_process(COMMAND "${SIMSYS}" sweep "${scenario}" ${ARGN}
                  RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
  set(sweep_code "${code}" PARENT_SCOPE)
  set(sweep_out "${out}" PARENT_SCOPE)
  set(sweep_err "${err}" PARENT_SCOPE)
endfunction()

# Fails the test unless `sweep` exited 0 with something on stdout.
function(require_output what)
  if(NOT sweep_code EQUAL 0)
    message(FATAL_ERROR "${what}: expected exit 0, got ${sweep_code}\nstdout: ${sweep_out}\nstderr: ${sweep_err}")
  endif()
  if(sweep_out STREQUAL "")
    message(FATAL_ERROR "${what}: nothing on stdout")
  endif()
endfunction()

# Runs the hello sweep with the given --jobs and --format, requires output, and returns it in
# `sweep_out` of the caller (set by run_sweep, so it is passed up here once more).
function(hello_sweep jobs format)
  run_sweep("${SCENARIO}" --param "${PARAM}" --metric users.ping.mean --metric api.mean_rho
            --jobs ${jobs} --format ${format})
  require_output("--jobs ${jobs} --format ${format}")
  set(sweep_out "${sweep_out}" PARENT_SCOPE)
endfunction()

# A sweep that must be refused: exit 1, the reason on stderr, nothing on stdout.
function(expect_refusal expected)
  run_sweep("${SCENARIO}" --metric users.ping.mean ${ARGN})
  if(NOT sweep_code EQUAL 1)
    message(FATAL_ERROR "expected exit 1, got ${sweep_code} for ${ARGN}\nstdout: ${sweep_out}\nstderr: ${sweep_err}")
  endif()
  string(FIND "${sweep_err}" "${expected}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "stderr lacks '${expected}' for ${ARGN}:\n${sweep_err}")
  endif()
  if(NOT sweep_out STREQUAL "")
    message(FATAL_ERROR "a refused sweep still printed on stdout for ${ARGN}:\n${sweep_out}")
  endif()
endfunction()

if(CHECK STREQUAL "same_bytes")
  foreach(format table csv)
    hello_sweep(1 ${format})
    set(with_one_worker "${sweep_out}")
    hello_sweep(3 ${format})
    set(with_three_workers "${sweep_out}")
    if(NOT with_one_worker STREQUAL with_three_workers)
      message(FATAL_ERROR "--format ${format}: --jobs 1 and --jobs 3 print different bytes\n"
                          "--jobs 1:\n${with_one_worker}\n--jobs 3:\n${with_three_workers}")
    endif()
    set(output_of_${format} "${with_one_worker}")
  endforeach()

  # The table carries each point's value; the exact widths are the renderer's business.
  foreach(value 100/s 500/s 900/s)
    string(FIND "${output_of_table}" "${value}" at)
    if(at EQUAL -1)
      message(FATAL_ERROR "the table lacks the point ${value}:\n${output_of_table}")
    endif()
  endforeach()

  # CSV: header + 3 rows. REGEX REPLACE strips the trailing newline so the list has no empty
  # last element; CMake lists split on `;`, so newlines are turned into them first.
  string(REGEX REPLACE "\n$" "" csv_text "${output_of_csv}")
  string(REPLACE "\n" ";" csv_lines "${csv_text}")
  list(LENGTH csv_lines csv_line_count)
  if(NOT csv_line_count EQUAL 4)
    message(FATAL_ERROR "--format csv: expected 4 lines (header + 3 rows), got ${csv_line_count}:\n${output_of_csv}")
  endif()
  list(GET csv_lines 0 header)
  if(NOT header STREQUAL "nodes.users.rate.base,users.ping.mean,api.mean_rho")
    message(FATAL_ERROR "unexpected CSV header: '${header}'")
  endif()
  # M/M/1 at 1 ms service + 0.5 ms round trip (see tests/unit/test_sweep.cpp): the cells are
  # the shortest round-trip text of the engine's doubles, so only the leading digits are
  # checked here (the unit test holds them to a relative 1e-9).
  list(GET csv_lines 1 row)
  if(NOT row MATCHES "^100/s,0\\.00161[0-9]*,0\\.(1|0999[0-9]*)$")
    message(FATAL_ERROR "unexpected row for 100/s: '${row}'")
  endif()
  list(GET csv_lines 2 row)
  if(NOT row MATCHES "^500/s,0\\.0025[0-9]*,0\\.(5|4999[0-9]*)$")
    message(FATAL_ERROR "unexpected row for 500/s: '${row}'")
  endif()
  list(GET csv_lines 3 row)
  if(NOT row MATCHES "^900/s,0\\.0(105|1049999[0-9]*),0\\.(9[0-9]*|8999[0-9]*)$")
    message(FATAL_ERROR "unexpected row for 900/s: '${row}'")
  endif()
elseif(CHECK STREQUAL "refuses_bad_param")
  expect_refusal("--param" --param "nodes.users.rate.base")
  expect_refusal("workers must be whole (400..600:4 gives 466.67)"
                 --param "nodes.api.workers=400..600:4")
  expect_refusal("nodes.users.rate.bse: no such key in the scenario"
                 --param "nodes.users.rate.bse=100/s..900/s:3")
  expect_refusal("-1/s"
                 --param "nodes.users.rate.base=-1/s..-1/s:2")
  # An unknown metric is the same kind of mistake in the other option.
  run_sweep("${SCENARIO}" --param "${PARAM}" --metric api.nonsense)
  if(NOT sweep_code EQUAL 1)
    message(FATAL_ERROR "an unknown metric: expected exit 1, got ${sweep_code}")
  endif()
  string(FIND "${sweep_err}" "api.nonsense" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "stderr does not name the unknown metric:\n${sweep_err}")
  endif()
elseif(CHECK STREQUAL "error_prefixes")
  set(param_prefix "${SCENARIO}: error: --param: ")
  set(metric_prefix "${SCENARIO}: error: --metric: ")
  foreach(case
          "${param_prefix}|--param|nodes.users.rate.base|--metric|api.mean_rho"
          "${param_prefix}|--param|nodes.api.workers=400..600:4|--metric|api.mean_rho"
          "${param_prefix}|--param|nodes.users.rate.bse=100/s..900/s:3|--metric|api.mean_rho"
          "${param_prefix}|--param|links.0.latency=1ms..2ms:2|--metric|api.mean_rho"
          "${metric_prefix}|--param|${PARAM}|--metric|api.nonsense"
          "${metric_prefix}|--param|${PARAM}|--metric|api.lag"
          "${metric_prefix}|--param|${PARAM}|--metric|nope.mean_rho")
    string(REPLACE "|" ";" words "${case}")
    list(POP_FRONT words prefix)
    run_sweep("${SCENARIO}" ${words})
    if(NOT sweep_code EQUAL 1)
      message(FATAL_ERROR "expected exit 1, got ${sweep_code} for ${words}\nstderr: ${sweep_err}")
    endif()
    string(FIND "${sweep_err}" "${prefix}" at)
    if(NOT at EQUAL 0)
      message(FATAL_ERROR "stderr does not start with '${prefix}' for ${words}:\n${sweep_err}")
    endif()
  endforeach()
elseif(CHECK STREQUAL "slo_failure_exit_0")
  # hello at 100/s has a p99 of about 5.7 ms: a 1 ms SLO fails at every point of the sweep.
  file(READ "${SCENARIO}" hello_yaml)
  file(WRITE "${WORK}/hello-slo.yaml"
       "${hello_yaml}slo:\n  - {metric: p99_latency, node: users, op: ping, max: 1ms}\n")
  execute_process(COMMAND "${SIMSYS}" run "${WORK}/hello-slo.yaml"
                  RESULT_VARIABLE run_code OUTPUT_VARIABLE run_out ERROR_VARIABLE run_err)
  if(NOT run_code EQUAL 2)
    message(FATAL_ERROR "the premise failed: `run` should exit 2 (SLO failed), got ${run_code}\n${run_out}${run_err}")
  endif()
  run_sweep("${WORK}/hello-slo.yaml" --param "${PARAM}" --metric users.ping.mean)
  require_output("a sweep over a failing SLO")
else()
  message(FATAL_ERROR "unknown CHECK '${CHECK}'")
endif()
