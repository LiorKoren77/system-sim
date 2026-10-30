# Process-level check of `simsys curve` under a scripted traffic_spike (phase 3 Task 6's review,
# finding 6): the schema says the fault acts on the client's rate exactly as a `spikes` entry on
# its curve, and `run` applies it so (engine::FaultSchedule, ClientBehaviour::set_rate_factor), so
# the curve a reader plots must show it too. Run by ctest as a CMake script:
#   cmake -DSIMSYS=<simsys> -DWORK=<scratch dir> -P curve_faults.cmake
# Three versions of one scenario (400/s, 30 minutes) are written to WORK:
#   fault  - a `faults:` entry {at: 10m, kind: traffic_spike, node: c, factor: 3, for: 10m}
#   spike  - the same spike as a `spikes` entry on the client's rate curve
#   plain  - neither
# `simsys curve <file> c` must print the same bytes for `fault` and `spike` (1200/s from 10 m to
# 20 m), and different bytes for `plain` (the check is not vacuous).
cmake_minimum_required(VERSION 3.28)

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

set(head "version: 1\nname: curve\nduration: 30m\nstep: 1s\nnodes:\n")
set(service "  s: {type: service, capacity: 10k/s, latency: 1ms}\nlinks:\n  - {from: c, to: s}\n")
file(WRITE "${WORK}/fault.yaml"
     "${head}  c: {type: client, rate: {base: 400/s}, ops: {op: 1.0}}\n${service}"
     "faults:\n  - {at: 10m, kind: traffic_spike, node: c, factor: 3, for: 10m}\n")
file(WRITE "${WORK}/spike.yaml"
     "${head}  c: {type: client, rate: {base: 400/s, spikes: [{at: 10m, for: 10m, factor: 3}]}, "
     "ops: {op: 1.0}}\n${service}")
file(WRITE "${WORK}/plain.yaml"
     "${head}  c: {type: client, rate: {base: 400/s}, ops: {op: 1.0}}\n${service}")

# Runs `simsys curve <WORK>/<name>.yaml c` and stores its stdout in `out_var`; fails the test on a
# non-zero exit.
function(curve_of name out_var)
  execute_process(COMMAND "${SIMSYS}" curve "${WORK}/${name}.yaml" c
                  RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "curve ${name}: exit ${code}\nstderr: ${err}")
  endif()
  set(${out_var} "${out}" PARENT_SCOPE)
endfunction()

curve_of(fault by_fault)
curve_of(spike by_spike)
curve_of(plain without)
if(NOT by_fault STREQUAL by_spike)
  message(FATAL_ERROR "the traffic_spike fault and the spikes entry differ:\n"
                      "fault:\n${by_fault}\nspike:\n${by_spike}")
endif()
if(by_fault STREQUAL without)
  message(FATAL_ERROR "the curve with the traffic_spike fault is the curve without it")
endif()
string(FIND "${by_fault}" "600,1200" at)
if(at EQUAL -1)
  message(FATAL_ERROR "no 1200/s sample at 10 m (600 s):\n${by_fault}")
endif()
