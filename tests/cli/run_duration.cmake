# Process-level checks of `simsys run --duration <d>`: the override must satisfy the rules the
# loader enforces on the file's own duration, and the refusal comes before any step. Run by ctest
# as a CMake script:
#   cmake -DSIMSYS=<simsys> -DSCENARIO=<scenario.yaml> -DCHECK=<name> -P run_duration.cmake
# with the url-shortener scenario (warmup 2m, an SLO with `over: 1h`). CHECK picks one property:
#   warmup - a duration not longer than the warmup is refused (exit 1): nothing would be observed;
#   over   - a duration whose observed part (duration - warmup) is shorter than an SLO's `over` is
#            refused (exit 1) with the loader's message: the window would never complete, and the
#            report would name a 1 h window of a 30-minute run (review of plan 2c Task 19).
cmake_minimum_required(VERSION 3.28)

# Runs `simsys run` with the extra arguments and fails the test unless it exits 1 and its stderr
# contains `expected`.
function(expect_refusal expected)
  execute_process(COMMAND "${SIMSYS}" run "${SCENARIO}" ${ARGN}
                  RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT code EQUAL 1)
    message(FATAL_ERROR "expected exit 1, got ${code}\nstdout: ${out}\nstderr: ${err}")
  endif()
  string(FIND "${err}" "${expected}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "stderr lacks '${expected}':\n${err}")
  endif()
endfunction()

if(CHECK STREQUAL "warmup")
  expect_refusal("error: --duration 2m is not longer than the warmup 2m" --duration 2m)
elseif(CHECK STREQUAL "over")
  expect_refusal(
      "error: --duration 30m: over must fit in the run after warmup (1h > duration 30m - warmup 2m)"
      --duration 30m)
else()
  message(FATAL_ERROR "unknown CHECK '${CHECK}'")
endif()
