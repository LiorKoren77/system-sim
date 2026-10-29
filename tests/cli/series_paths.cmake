# Process-level checks of `simsys run --series <file> --series-ops <file>`: what happens to the
# files on disk can only be seen from outside the process. Run by ctest as a CMake script:
#   cmake -DSIMSYS=<simsys> -DSCENARIO=<scenario.yaml> -DWORK=<scratch dir> -DCHECK=<name>
#         -P series_paths.cmake
# CHECK picks one property:
#   same_file  - two spellings of one path for both series are refused (exit 1) before the run:
#                both writers would truncate and interleave rows in one file;
#   keeps_file - when --series-ops cannot be opened, the --series file is left as it was (not
#                truncated) and a --series file that did not exist is not created.
cmake_minimum_required(VERSION 3.28)

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

# Runs simsys with the extra arguments and fails the test unless it exits 1 and its stderr
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

if(CHECK STREQUAL "same_file")
  expect_refusal("--series and --series-ops name the same file"
                 --series "${WORK}/both.csv" --series-ops "${WORK}/./both.csv")
  if(EXISTS "${WORK}/both.csv")
    message(FATAL_ERROR "the refused run still created ${WORK}/both.csv")
  endif()
elseif(CHECK STREQUAL "keeps_file")
  file(WRITE "${WORK}/kept.csv" "an earlier run's rows\n")
  expect_refusal("cannot open for writing"
                 --series "${WORK}/kept.csv" --series-ops "${WORK}/no-such-dir/ops.csv")
  file(READ "${WORK}/kept.csv" kept)
  if(NOT kept STREQUAL "an earlier run's rows\n")
    message(FATAL_ERROR "the --series file was overwritten: '${kept}'")
  endif()
  expect_refusal("cannot open for writing"
                 --series "${WORK}/fresh.csv" --series-ops "${WORK}/no-such-dir/ops.csv")
  if(EXISTS "${WORK}/fresh.csv")
    message(FATAL_ERROR "the refused run still created ${WORK}/fresh.csv")
  endif()
else()
  message(FATAL_ERROR "unknown CHECK '${CHECK}'")
endif()
