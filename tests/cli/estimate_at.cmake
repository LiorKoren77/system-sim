# Process-level checks of `simsys estimate --at` (phase 4a Task 6; spec 4a §5): the instant is a
# clock time or a duration inside the run, the table's first line names it, and the JSON carries it
# as `at_s`. Run by ctest as a CMake script:
#   cmake -DSIMSYS=<simsys> -DWORK=<scratch dir> -P estimate_at.cmake
cmake_minimum_required(VERSION 3.28)

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
file(WRITE "${WORK}/day.yaml"
     "version: 1\nname: day\nduration: 24h\nnodes:\n"
     "  c: {type: client, rate: {base: 1k/s, wave: {shape: sine, period: 24h, amplitude: 0.5, "
     "peak_at: 12:00}}, ops: {op: 1.0}}\n"
     "  s: {type: service, capacity: 2k/s, latency: 500us}\nlinks:\n  - {from: c, to: s}\n")

# Runs `simsys estimate <WORK>/day.yaml ARGN`, setting `code`, `out` and `err` in the caller.
function(estimate)
  execute_process(COMMAND "${SIMSYS}" estimate "${WORK}/day.yaml" ${ARGN}
                  RESULT_VARIABLE c OUTPUT_VARIABLE o ERROR_VARIABLE e)
  set(code "${c}" PARENT_SCOPE)
  set(out "${o}" PARENT_SCOPE)
  set(err "${e}" PARENT_SCOPE)
endfunction()

# Fails unless `text` contains `needle`.
function(require_contains what text needle)
  string(FIND "${text}" "${needle}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "${what}: no '${needle}' in:\n${text}")
  endif()
endfunction()

estimate(--at 25:00)
if(NOT code EQUAL 1)
  message(FATAL_ERROR "--at 25:00 on a 24h run: exit ${code}, expected 1\n${out}")
endif()
require_contains("--at 25:00" "${err}" "error: --at: hour must be 00-23")
estimate(--at 30h)
if(NOT code EQUAL 1)
  message(FATAL_ERROR "--at 30h on a 24h run: exit ${code}, expected 1\n${out}")
endif()
require_contains("--at 30h" "${err}" "error: --at: 30h must fall inside the run (24h)")

estimate(--at 12:00)
require_contains("--at 12:00" "${out}"
                 "estimate at t = 12h (12:00): the curve without noise; explain's checks use the peak")
estimate(--at 90m --no-color)
if(NOT code EQUAL 0)
  message(FATAL_ERROR "--at 90m: exit ${code}\n${err}")
endif()

estimate(--at 12:00 --report json)
require_contains("--at 12:00 json" "${out}" "\"at_s\": 43200")
estimate(--report json)
require_contains("json without --at" "${out}" "\"at_s\": 0")

# --at 00:00 is the estimate without --at, byte for byte.
estimate()
set(plain "${out}")
estimate(--at 00:00)
if(NOT out STREQUAL plain)
  message(FATAL_ERROR "--at 00:00 differs from estimate without --at:\n${plain}\nvs\n${out}")
endif()
