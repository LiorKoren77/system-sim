# Process-level checks of `simsys run --days N` (phase 5b spec §5): the refusals as a user meets
# them, on stderr or as the stream's error line, and `--days 1` as the plain run. Run by ctest as a
# CMake script:
#   cmake -DSIMSYS=<simsys> -DSCENARIOS=<scenarios dir> -DCHECK=<name> -P run_days.cmake
# CHECK picks one property:
#   needs_day - a scenario that is not 24 h long is refused, on stderr and under --stream as an
#               error line naming the scenario (its fault, not the command line's);
#   duration  - --days with --duration is refused: --days sets the duration;
#   one_day   - --days 1 is the plain run: the same stream, its summary's report included, but for
#               the run line's "days":1.
cmake_minimum_required(VERSION 3.28)

set(hello "${SCENARIOS}/tutorial/hello/scenario.yaml")
set(url_shortener "${SCENARIOS}/book/url-shortener/scenario.yaml")

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

function(require_exit what expected)
  if(NOT code EQUAL ${expected})
    message(FATAL_ERROR "${what}: exit ${code}, expected ${expected}\n${out}\n${err}")
  endif()
endfunction()

if(CHECK STREQUAL "needs_day")
  simsys(run "${hello}" --days 2)
  require_exit("needs_day" 1)
  require_contains("needs_day" "${err}" "error: --days needs a 24h scenario (this one runs 1m)")
  simsys(run "${hello}" --days 2 --stream)
  require_exit("needs_day --stream" 1)
  require_contains("needs_day --stream" "${out}" "\"type\":\"error\"")
  require_contains("needs_day --stream" "${out}" "\"file\":\"${hello}\"")
  require_contains("needs_day --stream" "${out}" "--days needs a 24h scenario (this one runs 1m)")
  if(NOT err STREQUAL "")
    message(FATAL_ERROR "needs_day --stream: stderr not empty:\n${err}")
  endif()
elseif(CHECK STREQUAL "duration")
  simsys(run "${url_shortener}" --days 2 --duration 48h)
  require_exit("duration" 1)
  require_contains("duration" "${err}" "error: --days sets the duration; drop --duration")
elseif(CHECK STREQUAL "one_day")
  # The stream ends with the summary, which is `--report json`'s report: one comparison covers the
  # frames and the report (each run of the day is slow under the sanitizers).
  simsys(run "${url_shortener}" --stream --points 24)
  set(plain "${out}")
  simsys(run "${url_shortener}" --stream --points 24 --days 1)
  require_contains("one_day --stream" "${out}" ",\"days\":1}")
  string(REPLACE ",\"days\":1}" "}" out "${out}")
  if(NOT out STREQUAL plain)
    message(FATAL_ERROR "one_day: --days 1 --stream differs from the plain stream but for days")
  endif()
else()
  message(FATAL_ERROR "unknown CHECK '${CHECK}'")
endif()
