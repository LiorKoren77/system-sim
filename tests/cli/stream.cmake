# Process-level checks of `simsys run --stream` (phase 5a, spec §4 and §6.5): what reaches stdout and
# stderr, and the exit code, can only be seen from outside the process. Run by ctest as a CMake
# script:
#   cmake -DSIMSYS=<simsys> -DSCENARIOS=<scenarios dir> -DWORK=<scratch dir> -DCHECK=<name>
#         -P stream.cmake
# CHECK picks one property:
#   only_json   - a streamed run writes nothing but lines of JSON on stdout and nothing on stderr;
#   error_line  - a scenario the loader refuses gives one `error` line with the loader's line and
#                 column and message, and exit 1;
#   refusals    - the flags --stream cannot be combined with, and --points / --every without it;
#   parse_error - a flag the command line does not know, with --stream: an `error` line, exit 1;
#   exit_code   - a streamed run whose SLO fails exits 2, its last line the summary.
cmake_minimum_required(VERSION 3.28)

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(hello "${SCENARIOS}/tutorial/hello/scenario.yaml")

# Runs simsys with the arguments, setting `code`, `out` and `err` in the caller.
function(simsys)
  execute_process(COMMAND "${SIMSYS}" ${ARGN}
                  RESULT_VARIABLE c OUTPUT_VARIABLE o ERROR_VARIABLE e)
  set(code "${c}" PARENT_SCOPE)
  set(out "${o}" PARENT_SCOPE)
  set(err "${e}" PARENT_SCOPE)
endfunction()

function(require_contains what text needle)
  string(FIND "${text}" "${needle}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "${what}: no '${needle}' in:\n${text}")
  endif()
endfunction()

# Fails unless the stream is exactly one `error` line containing `needle`, with exit 1 and an empty
# stderr.
function(require_error_line what needle)
  if(NOT code EQUAL 1)
    message(FATAL_ERROR "${what}: exit ${code}, expected 1\n${out}\n${err}")
  endif()
  if(NOT err STREQUAL "")
    message(FATAL_ERROR "${what}: stderr is not empty under --stream:\n${err}")
  endif()
  string(REGEX MATCHALL "\n" newlines "${out}")
  list(LENGTH newlines lines)
  if(NOT lines EQUAL 1)
    message(FATAL_ERROR "${what}: ${lines} lines, expected one error line:\n${out}")
  endif()
  require_contains("${what}" "${out}" "{\"type\":\"error\",")
  require_contains("${what}" "${out}" "${needle}")
endfunction()

if(CHECK STREQUAL "only_json")
  simsys(run "${hello}" --stream --points 60)
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "exit ${code}, expected 0\n${err}")
  endif()
  if(NOT err STREQUAL "")
    message(FATAL_ERROR "stderr is not empty:\n${err}")
  endif()
  string(REPLACE "\n" ";" lines "${out}")
  list(FILTER lines EXCLUDE REGEX "^$")
  list(LENGTH lines count)
  if(NOT count EQUAL 63)  # run, graph, 60 frames, summary
    message(FATAL_ERROR "${count} lines, expected 63")
  endif()
  foreach(line IN LISTS lines)
    if(NOT line MATCHES "^{\"type\":\"(run|graph|frame|summary)\",")
      message(FATAL_ERROR "not a line of the stream: ${line}")
    endif()
  endforeach()
elseif(CHECK STREQUAL "error_line")
  file(WRITE "${WORK}/bad.yaml"
       "version: 1\nname: bad\nduration: 1m\nnodes:\n"
       "  users: {type: client, rate: {base: 100/s}, ops: {ping: 1}}\n"
       "  api: {type: service, capasity: 1k/s}\n"
       "links:\n  - {from: users, to: api, dir: inout}\n")
  simsys(run "${WORK}/bad.yaml")
  string(REGEX MATCH "bad.yaml:([0-9]+):([0-9]+): error" location "${err}")
  if(location STREQUAL "")
    message(FATAL_ERROR "the loader's message has no line and column:\n${err}")
  endif()
  set(line "${CMAKE_MATCH_1}")
  set(column "${CMAKE_MATCH_2}")
  string(STRIP "${err}" message_text)
  simsys(run "${WORK}/bad.yaml" --stream)
  require_error_line("a misspelled key" "\"line\":${line},\"column\":${column},")
  require_contains("the error line's message" "${out}" "capasity")
elseif(CHECK STREQUAL "refusals")
  foreach(combination "--series;${WORK}/s.csv" "--series-ops;${WORK}/o.csv" "--report;json"
                      "--replications;3")
    list(GET combination 0 flag)
    simsys(run "${hello}" --stream ${combination})
    require_error_line("--stream ${flag}"
                       "--stream carries the report and the series; drop ${flag}")
  endforeach()
  simsys(run "${hello}" --stream --points 10 --every 1s)
  require_error_line("--points with --every" "--points and --every both set the bucket; give one")
  simsys(run "${hello}" --stream --every 250ms)
  require_error_line("--every 250ms" "--every 250ms: not a whole number of 100ms steps")
  foreach(flag --points --every)
    if(flag STREQUAL "--points")
      set(value 10)
    else()
      set(value 1s)
    endif()
    simsys(run "${hello}" ${flag} ${value})
    if(NOT code EQUAL 1)
      message(FATAL_ERROR "${flag} without --stream: exit ${code}, expected 1")
    endif()
    require_contains("${flag} without --stream" "${err}" "${flag} needs --stream")
  endforeach()
elseif(CHECK STREQUAL "parse_error")
  simsys(run "${hello}" --stream --no-such-flag)
  require_error_line("an unknown flag" "no-such-flag")
elseif(CHECK STREQUAL "exit_code")
  simsys(run "${SCENARIOS}/labs/unique-id/scenario.yaml" --stream --points 20)
  if(NOT code EQUAL 2)
    message(FATAL_ERROR "the unique-id lab streamed: exit ${code}, expected 2\n${err}")
  endif()
  string(STRIP "${out}" stripped)
  string(FIND "${stripped}" "\n" last_newline REVERSE)
  math(EXPR start "${last_newline} + 1")
  string(SUBSTRING "${stripped}" ${start} -1 last_line)
  require_contains("the last line" "${last_line}" "{\"type\":\"summary\",\"exit_code\":2,")
else()
  message(FATAL_ERROR "unknown CHECK '${CHECK}'")
endif()
