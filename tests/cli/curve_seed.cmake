# Process-level check of `simsys curve --seed` (phase 4a Task 1; the 2a leftover M13): a seed feeds
# only the client's noise process, so without --noise it would change nothing, and a flag that
# silently does nothing teaches the wrong thing. Refused, as `retry_after` without `retries` is.
# Run by ctest as a CMake script:
#   cmake -DSIMSYS=<simsys> -DWORK=<scratch dir> -P curve_seed.cmake
cmake_minimum_required(VERSION 3.28)

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
file(WRITE "${WORK}/noisy.yaml"
     "version: 1\nname: curve\nduration: 10m\nstep: 1s\nnodes:\n"
     "  c: {type: client, rate: {base: 400/s, noise: {sigma: 0.2, correlation: 10s}}, "
     "ops: {op: 1.0}}\n"
     "  s: {type: service, capacity: 10k/s, latency: 1ms}\nlinks:\n  - {from: c, to: s}\n")

execute_process(COMMAND "${SIMSYS}" curve "${WORK}/noisy.yaml" c --seed 3
                RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code EQUAL 1)
  message(FATAL_ERROR "--seed without --noise: exit ${code}, expected 1\nstdout: ${out}")
endif()
if(NOT out STREQUAL "")
  message(FATAL_ERROR "--seed without --noise printed a curve:\n${out}")
endif()
string(FIND "${err}" "--seed needs --noise (without it the curve has no randomness)" at)
if(at EQUAL -1)
  message(FATAL_ERROR "the refusal does not say why:\n${err}")
endif()

execute_process(COMMAND "${SIMSYS}" curve "${WORK}/noisy.yaml" c --seed 3 --noise
                RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code EQUAL 0)
  message(FATAL_ERROR "--seed with --noise: exit ${code}\nstderr: ${err}")
endif()
