# Process-level checks of `simsys run --replications N`: what is printed (the median and the band
# across seeds), what is refused, what the exit code says, and that the number of workers cannot
# show in the output are all visible only from outside the process. Registered in
# tests/CMakeLists.txt, one ctest entry per CHECK.
#
# Run by ctest as a CMake script:
#   cmake -DSIMSYS=<simsys> -DSCENARIO=<tutorial/hello's scenario.yaml> -DWORK=<scratch dir>
#         -DCHECK=<name> -P replications.cmake
#
# The probe: tutorial/hello with `noise: {sigma: 0.2, correlation: 10s}` on its client's rate
# (the schema asks for both keys; the plan's `noise: {sigma: 0.2}` is refused by the loader), and
# optionally one SLO and one scripted fault. Its seeds 1..5 were measured with `simsys run --seed
# S` on 2026-10-30 (Debug, GCC 14.2); the five single-seed runs, in seconds:
#
#   seed  users.ping.mean         p50       p95       p99        api mean_rho
#   1     0.0016212904090722221   1.3333ms  3.7299ms  5.7123ms   0.10497
#   2     0.0016176475552008955   1.3308ms  3.7193ms  5.6937ms   0.10295
#   3     0.0016284392389011029   1.3384ms  3.7498ms  5.7462ms   0.11240
#   4     0.0016059675198759683   1.3226ms  3.6860ms  5.6363ms   0.09377
#   5     0.0016141360356375198   1.3283ms  3.7093ms  5.6763ms   0.10014
#
# Sorted by any of the columns the order is the same, seeds 4, 5, 2, 1, 3. So the median run of
# seeds 1..5 is seed 2 for every metric, and the band, by the nearest-rank rule (the 10 % quantile
# of 5 values is the 1st smallest, the 90 % quantile the 5th: ceil(0.1 * 5) = 1, ceil(0.9 * 5) =
# 5), is [seed 4, seed 3]: mean 1.62ms [1.61-1.63], p50 1.33ms [1.32-1.34], p95 3.72ms
# [3.69-3.75], p99 5.69ms [5.64-5.75]. Seeds 3..5: median seed 5, same band.
# An SLO `p99_latency users ping max 5.7ms` is failed by seeds 1 and 3 and passed by 2, 4, 5;
# `max 5.68ms` is passed by seeds 4 and 5 only (seed 5's 5.6763 ms is under it, seed 2's 5.6937 ms
# is over it).
#
# CHECK picks one property:
#   bands          - the table's cells `5.69ms [5.64-5.75]` (median, then the band in brackets,
#                    its unit left out) and the SLO line `PASS in 3 of 5`; `--seed 3
#                    --replications 3` runs seeds 3, 4, 5;
#   same_bytes     - --jobs 1, 3 and 4 print the same bytes, as a table and as JSON;
#   json           - `replications.seeds` and `replications.per_seed` hold what five single-seed
#                    runs print, and the report around them is the median run's;
#   exit_codes     - the exit code is the median run's: a seed failing is not a failure, the
#                    median failing is (exit 2);
#   noiseless      - a scenario with no noise runs once, with the note as the first line before
#                    the table (JSON: nothing but the plain JSON), and the plain run's exit code;
#   one_is_plain   - --replications 1 is the plain run byte for byte, in both formats, with
#                    --seed, and it still writes a series file;
#   refusals       - 0, 1001 and -5 are refused (exit 1, the message on stderr, nothing on
#                    stdout), 1 and 1000 are not;
#   series_refused - --series and --series-ops with more than one replication are refused and
#                    leave no file behind;
#   fault_once     - a scripted fault is the same in every replication: its block is printed
#                    once;
#   slo_median_seeds - each SLO names the seed of its own median run, in its row and in the JSON,
#                    and the exit code is 2 when the median run of any SLO fails (the reviewer's
#                    repro, below);
#   noiseless_json_note - a scenario without noise under --report json: the note on stderr, the
#                    plain run's JSON alone on stdout;
#   sigma_zero     - `noise: {sigma: 0}` is no noise: the note and a single run;
#   jobs_help      - `run --help` says --jobs is ignored without --replications.
#
# The two-SLO probe of slo_median_seeds adds `utilization api max 0.1163 over 10s` to the p99 SLO
# at 5.7ms. The worst 10 s utilization of api per seed (`--report json`, 2026-10-30):
#
#   seed  1        2        3        4        5
#         0.13267  0.11629  0.12097  0.10839  0.11636
#
# Sorted: seeds 4, 2, 5, 3, 1, so this SLO's median run is seed 5 (0.11636 > 0.1163: FAIL), while
# the p99 SLO's median run is seed 2 (PASS). The run that stands for all (the summary line, the
# JSON around `replications`) is the first SLO's median run, seed 2; the exit code is 2, from seed
# 5's verdict on the second SLO.
cmake_minimum_required(VERSION 3.28)

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

set(NOISELESS_NOTE "replications: the scenario has no noise, so every seed gives the same run")

# The probe's text up to its links, and the SLO and fault entries that can follow.
set(PROBE_HEAD "version: 1
name: hello
duration: 1m
nodes:
  users: {type: client, rate: {base: 100/s, noise: {sigma: 0.2, correlation: 10s}}, ops: {ping: 1.0}}
  api:   {type: service, capacity: 1k/s, latency: 1ms}
links:
  - {from: users, to: api, dir: inout}
")
set(PROBE_FAULT "faults:
  - {at: 20s, kind: node_down, node: api, for: 10s}
")

# Writes the noisy probe to ${WORK}/${name}.yaml with a p99 SLO of `slo_max` (none when empty).
function(write_probe name slo_max)
  set(text "${PROBE_HEAD}")
  if(NOT slo_max STREQUAL "")
    string(APPEND text "slo:\n  - {metric: p99_latency, node: users, op: ping, max: ${slo_max}}\n")
  endif()
  file(WRITE "${WORK}/${name}.yaml" "${text}")
endfunction()

# Runs `simsys run <file> ARGN` and sets `run_code`, `run_out` and `run_err` in the caller's scope.
function(simsys_run file)
  execute_process(COMMAND "${SIMSYS}" run "${file}" ${ARGN}
                  RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
  set(run_code "${code}" PARENT_SCOPE)
  set(run_out "${out}" PARENT_SCOPE)
  set(run_err "${err}" PARENT_SCOPE)
endfunction()

# Fails the test unless the last run exited with `code`.
function(require_exit code what)
  if(NOT run_code EQUAL ${code})
    message(FATAL_ERROR "${what}: expected exit ${code}, got ${run_code}\nstdout: ${run_out}\nstderr: ${run_err}")
  endif()
endfunction()

# Fails the test unless `text` contains `needle`.
function(require_contains text needle what)
  string(FIND "${text}" "${needle}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "${what}: lacks '${needle}':\n${text}")
  endif()
endfunction()

# Fails the test unless the two texts are the same bytes.
function(require_same left right what)
  if(NOT left STREQUAL right)
    message(FATAL_ERROR "${what}: the bytes differ\n--- left:\n${left}\n--- right:\n${right}")
  endif()
endfunction()

# A run that must be refused: exit 1, `expected` on stderr, nothing on stdout.
function(require_refused expected what)
  require_exit(1 "${what}")
  require_contains("${run_err}" "${expected}" "${what} (stderr)")
  if(NOT run_out STREQUAL "")
    message(FATAL_ERROR "${what}: a refused run still printed on stdout:\n${run_out}")
  endif()
endfunction()

# The JSON value at the given path of `json`, in the caller's scope as `json_value`.
function(json_at json)
  string(JSON value ERROR_VARIABLE problem GET "${json}" ${ARGN})
  if(NOT problem STREQUAL "NOTFOUND")
    message(FATAL_ERROR "JSON path ${ARGN}: ${problem}\n${json}")
  endif()
  set(json_value "${value}" PARENT_SCOPE)
endfunction()

if(CHECK STREQUAL "bands")
  write_probe(probe 5.7ms)
  simsys_run("${WORK}/probe.yaml" --replications 5 --jobs 2)
  require_exit(0 "--replications 5")
  # The cell is the median, then the band in brackets; the band's unit is the median's.
  require_contains("${run_out}" "1.62ms [1.61–1.63]" "the mean cell")
  require_contains("${run_out}" "1.33ms [1.32–1.34]" "the p50 cell")
  require_contains("${run_out}" "3.72ms [3.69–3.75]" "the p95 cell")
  require_contains("${run_out}" "5.69ms [5.64–5.75]" "the p99 cell")
  require_contains("${run_out}" "scenario hello" "the header line")
  string(FIND "${run_out}" "has no noise" at)
  if(NOT at EQUAL -1)
    message(FATAL_ERROR "a noisy scenario got the noiseless note:\n${run_out}")
  endif()
  # The SLO fails at seeds 1 and 3 and passes at 2, 4 and 5; the median run (seed 2) passes with
  # its own observed value, 5.69ms.
  string(REGEX MATCH "[^\n]*PASS in 3 of 5[^\n]*" slo_line "${run_out}")
  if(slo_line STREQUAL "")
    message(FATAL_ERROR "no 'PASS in 3 of 5' line:\n${run_out}")
  endif()
  foreach(word p99_latency users ping 5.69ms 5.7ms)
    require_contains("${slo_line}" "${word}" "the SLO line")
  endforeach()

  # --seed moves the first seed: seeds 3, 4, 5 have the median run seed 5 and the same band.
  simsys_run("${WORK}/probe.yaml" --seed 3 --replications 3)
  require_exit(0 "--seed 3 --replications 3")
  require_contains("${run_out}" "1.61ms [1.61–1.63]" "the mean cell of seeds 3..5")
  require_contains("${run_out}" "5.68ms [5.64–5.75]" "the p99 cell of seeds 3..5")
  require_contains("${run_out}" "PASS in 2 of 3" "the SLO line of seeds 3..5")
elseif(CHECK STREQUAL "same_bytes")
  write_probe(probe 5.7ms)
  foreach(format table json)
    simsys_run("${WORK}/probe.yaml" --replications 5 --report ${format} --jobs 1)
    require_exit(0 "--jobs 1 --report ${format}")
    set(with_one_worker "${run_out}")
    foreach(jobs 3 4)
      simsys_run("${WORK}/probe.yaml" --replications 5 --report ${format} --jobs ${jobs})
      require_exit(0 "--jobs ${jobs} --report ${format}")
      require_same("${with_one_worker}" "${run_out}" "--jobs 1 against --jobs ${jobs}, ${format}")
    endforeach()
  endforeach()
elseif(CHECK STREQUAL "json")
  write_probe(probe 5.7ms)
  simsys_run("${WORK}/probe.yaml" --replications 5 --report json --jobs 3)
  require_exit(0 "--replications 5 --report json")
  set(replicated "${run_out}")

  json_at("${replicated}" replications seeds)
  string(JSON seed_count LENGTH "${replicated}" replications seeds)
  if(NOT seed_count EQUAL 5)
    message(FATAL_ERROR "replications.seeds: expected 5 seeds, got ${seed_count}")
  endif()
  json_at("${replicated}" replications per_seed ops 0 client)
  require_same("users" "${json_value}" "replications.per_seed.ops[0].client")
  json_at("${replicated}" replications per_seed ops 0 op)
  require_same("ping" "${json_value}" "replications.per_seed.ops[0].op")
  json_at("${replicated}" replications per_seed nodes 1 id)
  require_same("api" "${json_value}" "replications.per_seed.nodes[1].id")

  # Entry k of every list is the single run of seed k + 1: the same numbers, to the last digit.
  foreach(k 0 1 2 3 4)
    math(EXPR seed "${k} + 1")
    json_at("${replicated}" replications seeds ${k})
    require_same("${seed}" "${json_value}" "replications.seeds[${k}]")
    simsys_run("${WORK}/probe.yaml" --seed ${seed} --report json)
    set(single "${run_out}")
    foreach(path "ops;0;mean_s" "ops;0;p50_s" "ops;0;p95_s" "ops;0;p99_s" "nodes;1;mean_rho")
      json_at("${single}" ${path})
      set(expected "${json_value}")
      json_at("${replicated}" replications per_seed ${path} ${k})
      require_same("${expected}" "${json_value}" "per_seed ${path} of seed ${seed}")
    endforeach()
    if(seed EQUAL 2)
      set(median_run_json "${single}")
    endif()
  endforeach()
  json_at("${replicated}" replications per_seed ops 0 mean_s 0)
  if(NOT json_value MATCHES "^0\\.0016212904090")
    message(FATAL_ERROR "seed 1's users.ping.mean should read 0.0016212904090..., got ${json_value}")
  endif()

  # The report around `replications` is the median run's (seed 2, for every metric of this probe).
  json_at("${replicated}" scenario)
  require_same("hello" "${json_value}" "scenario")
  foreach(path "ops;0;mean_s" "ops;0;p99_s" "nodes;1;mean_rho" "steps")
    json_at("${median_run_json}" ${path})
    set(expected "${json_value}")
    json_at("${replicated}" ${path})
    require_same("${expected}" "${json_value}" "the median run's ${path}")
  endforeach()
elseif(CHECK STREQUAL "exit_codes")
  # Premise: at max 5.7ms seed 1 fails (exit 2) and seed 2 passes (exit 0).
  write_probe(median_passes 5.7ms)
  simsys_run("${WORK}/median_passes.yaml" --seed 1)
  require_exit(2 "premise: seed 1 against 5.7ms")
  simsys_run("${WORK}/median_passes.yaml" --seed 2)
  require_exit(0 "premise: seed 2 against 5.7ms")
  # Seeds 1 and 3 fail, the median run passes: exit 0.
  simsys_run("${WORK}/median_passes.yaml" --replications 5)
  require_exit(0 "--replications 5 where the median run passes and two seeds fail")

  # At max 5.68ms the median run (seed 2, 5.6937 ms) fails and only seeds 4 and 5 pass: exit 2.
  write_probe(median_fails 5.68ms)
  simsys_run("${WORK}/median_fails.yaml" --replications 5)
  require_exit(2 "--replications 5 where the median run fails")
  string(REGEX MATCH "[^\n]*FAIL[^\n]*" fail_line "${run_out}")
  if(fail_line STREQUAL "")
    message(FATAL_ERROR "no FAIL line:\n${run_out}")
  endif()
  # The plan's wording gives only the passing case (`PASS in 9 of 10`); this pins that a failing
  # median shows how many seeds did pass, whichever words follow FAIL.
  require_contains("${fail_line}" "2 of 5" "the FAIL line")
  require_contains("${fail_line}" "p99_latency" "the FAIL line")
  require_contains("${fail_line}" "5.69ms" "the FAIL line's observed value, the median run's")
  string(FIND "${run_out}" "PASS in" at)
  if(NOT at EQUAL -1)
    message(FATAL_ERROR "a failing median printed PASS in:\n${run_out}")
  endif()
elseif(CHECK STREQUAL "noiseless")
  simsys_run("${SCENARIO}")
  require_exit(0 "the plain run")
  set(plain_table "${run_out}")
  simsys_run("${SCENARIO}" --report json)
  set(plain_json "${run_out}")

  # The note is the first line of stdout, then the plain run's table, byte for byte.
  foreach(jobs 1 3)
    simsys_run("${SCENARIO}" --replications 5 --jobs ${jobs})
    require_exit(0 "--replications 5 on a scenario without noise, --jobs ${jobs}")
    require_same("${NOISELESS_NOTE}\n${plain_table}" "${run_out}" "the noiseless table, --jobs ${jobs}")
  endforeach()
  # JSON has no room for a line of prose before it (the estimate header is table only too): the
  # stdout is the plain run's JSON. The note's place for JSON is not pinned (stderr is free).
  simsys_run("${SCENARIO}" --replications 5 --report json)
  require_exit(0 "--replications 5 --report json on a scenario without noise")
  require_same("${plain_json}" "${run_out}" "the noiseless JSON")

  # The exit code is the plain run's: hello's p99 is about 5.7 ms, so a 1 ms SLO fails (exit 2).
  file(READ "${SCENARIO}" hello_yaml)
  file(WRITE "${WORK}/hello-slo.yaml"
       "${hello_yaml}slo:\n  - {metric: p99_latency, node: users, op: ping, max: 1ms}\n")
  simsys_run("${WORK}/hello-slo.yaml")
  require_exit(2 "premise: the plain run against a 1 ms SLO")
  set(plain_failing "${run_out}")
  simsys_run("${WORK}/hello-slo.yaml" --replications 5)
  require_exit(2 "--replications 5 on a scenario without noise whose SLO fails")
  require_same("${NOISELESS_NOTE}\n${plain_failing}" "${run_out}" "the noiseless table with a failing SLO")
elseif(CHECK STREQUAL "one_is_plain")
  write_probe(probe 5.7ms)
  foreach(file "${SCENARIO}" "${WORK}/probe.yaml")
    foreach(format table json)
      simsys_run("${file}" --report ${format})
      set(plain_code "${run_code}")
      set(plain_out "${run_out}")
      simsys_run("${file}" --report ${format} --replications 1)
      require_exit(${plain_code} "--replications 1 --report ${format} on ${file}")
      require_same("${plain_out}" "${run_out}" "--replications 1 against the plain run, ${format}, ${file}")
    endforeach()
  endforeach()
  # --replications 1 is the plain run, so it does not say the scenario has no noise.
  simsys_run("${SCENARIO}" --replications 1)
  string(FIND "${run_out}" "has no noise" at)
  if(NOT at EQUAL -1)
    message(FATAL_ERROR "--replications 1 printed the noiseless note:\n${run_out}")
  endif()
  # With --seed too (seed 4's p99 passes the SLO, seed 1's does not: the codes differ).
  simsys_run("${WORK}/probe.yaml" --seed 4)
  set(plain_code "${run_code}")
  set(plain_out "${run_out}")
  simsys_run("${WORK}/probe.yaml" --seed 4 --replications 1)
  require_exit(${plain_code} "--seed 4 --replications 1")
  require_same("${plain_out}" "${run_out}" "--seed 4 --replications 1 against --seed 4")
  # A single run may write its series, as before (a probe without an SLO: exit 0).
  write_probe(no_slo "")
  simsys_run("${WORK}/no_slo.yaml" --replications 1 --series "${WORK}/one.csv")
  require_exit(0 "--replications 1 --series")
  if(NOT EXISTS "${WORK}/one.csv")
    message(FATAL_ERROR "--replications 1 --series wrote no file")
  endif()
elseif(CHECK STREQUAL "refusals")
  write_probe(probe "")
  set(message "--replications must be between 1 and 1000")
  foreach(count 0 1001 -5)
    simsys_run("${WORK}/probe.yaml" --replications ${count})
    require_refused("${message}" "--replications ${count} on a noisy scenario")
    simsys_run("${SCENARIO}" --replications ${count})
    require_refused("${message}" "--replications ${count} on a scenario without noise")
  endforeach()
  # The ends are allowed. A scenario without noise runs once whatever the count, so 1000 is cheap.
  simsys_run("${SCENARIO}" --replications 1000)
  require_exit(0 "--replications 1000 on a scenario without noise")
  simsys_run("${SCENARIO}" --replications 1)
  require_exit(0 "--replications 1")
elseif(CHECK STREQUAL "series_refused")
  write_probe(probe "")
  set(series_message "--series needs a single run (one series file per seed is phase 4 material)")
  foreach(file "${WORK}/probe.yaml" "${SCENARIO}")
    simsys_run("${file}" --replications 2 --series "${WORK}/nodes.csv")
    require_refused("${series_message}" "--replications 2 --series on ${file}")
    if(EXISTS "${WORK}/nodes.csv")
      message(FATAL_ERROR "a refused --series run left ${WORK}/nodes.csv behind")
    endif()
    # The plan words only --series; --series-ops is refused for the same reason.
    simsys_run("${file}" --replications 2 --series-ops "${WORK}/ops.csv")
    require_refused("needs a single run" "--replications 2 --series-ops on ${file}")
    if(EXISTS "${WORK}/ops.csv")
      message(FATAL_ERROR "a refused --series-ops run left ${WORK}/ops.csv behind")
    endif()
  endforeach()
elseif(CHECK STREQUAL "fault_once")
  file(WRITE "${WORK}/faulty.yaml" "${PROBE_HEAD}${PROBE_FAULT}")
  simsys_run("${WORK}/faulty.yaml" --replications 5)
  require_exit(0 "--replications 5 with a scripted fault")
  # Scripted faults are the same in every replication: one block, with the median run's numbers.
  string(REGEX MATCHALL "(^|\n)fault [^\n]*" headers "${run_out}")
  list(LENGTH headers header_count)
  if(NOT header_count EQUAL 1)
    message(FATAL_ERROR "expected the fault block once, found ${header_count} headers:\n${run_out}")
  endif()
  require_contains("${run_out}" "fault 00:00:20 node_down api for 10s" "the fault header")
elseif(CHECK STREQUAL "slo_median_seeds")
  file(WRITE "${WORK}/two_slos.yaml" "${PROBE_HEAD}slo:
  - {metric: p99_latency, node: users, op: ping, max: 5.7ms}
  - {metric: utilization, node: api, max: 0.1163, over: 10s}
")
  simsys_run("${WORK}/two_slos.yaml" --replications 5)
  require_exit(2 "--replications 5 where the second SLO's median run fails")
  require_contains("${run_out}" "the summary line and the fault blocks are seed 2's" "the intro")
  string(REGEX MATCH "[^\n]*p99_latency[^\n]*" p99_line "${run_out}")
  require_contains("${p99_line}" "PASS in 3 of 5" "the p99 SLO's row")
  require_contains("${p99_line}" "median run: seed 2" "the p99 SLO's row")
  string(REGEX MATCH "[^\n]*utilization[^\n]*" utilization_line "${run_out}")
  require_contains("${utilization_line}" "FAIL (passed in 2 of 5)" "the utilization SLO's row")
  require_contains("${utilization_line}" "0.1164" "the utilization SLO's observed value, seed 5's")
  require_contains("${utilization_line}" "median run: seed 5" "the utilization SLO's row")

  simsys_run("${WORK}/two_slos.yaml" --replications 5 --report json)
  require_exit(2 "--replications 5 --report json where the second SLO's median run fails")
  json_at("${run_out}" slo 0 median_seed)
  require_same("2" "${json_value}" "slo[0].median_seed")
  json_at("${run_out}" slo 1 median_seed)
  require_same("5" "${json_value}" "slo[1].median_seed")
  json_at("${run_out}" replications median_seed)
  require_same("2" "${json_value}" "replications.median_seed, the run around the object")
elseif(CHECK STREQUAL "noiseless_json_note")
  simsys_run("${SCENARIO}" --report json)
  require_exit(0 "the plain run")
  set(plain_json "${run_out}")
  simsys_run("${SCENARIO}" --replications 3 --report json)
  require_exit(0 "--replications 3 --report json on a scenario without noise")
  require_same("${plain_json}" "${run_out}" "stdout, the plain run's JSON alone")
  require_contains("${run_err}" "${NOISELESS_NOTE}" "stderr")
elseif(CHECK STREQUAL "sigma_zero")
  string(REPLACE "sigma: 0.2" "sigma: 0" zero_head "${PROBE_HEAD}")
  file(WRITE "${WORK}/sigma_zero.yaml" "${zero_head}")
  simsys_run("${WORK}/sigma_zero.yaml")
  require_exit(0 "the plain run of sigma 0")
  set(plain_table "${run_out}")
  simsys_run("${WORK}/sigma_zero.yaml" --replications 5)
  require_exit(0 "--replications 5 with sigma 0")
  require_same("${NOISELESS_NOTE}\n${plain_table}" "${run_out}" "sigma 0: the note and one run")
elseif(CHECK STREQUAL "jobs_help")
  execute_process(COMMAND "${SIMSYS}" run --help OUTPUT_VARIABLE help ERROR_VARIABLE help_err)
  # CLI11 may wrap a long description: compare with every run of blanks and newlines as one space.
  string(REGEX REPLACE "[ \n]+" " " help "${help}")
  string(FIND "${help}" "--jobs " at)
  if(at EQUAL -1)
    message(FATAL_ERROR "run --help has no --jobs:\n${help}")
  endif()
  string(SUBSTRING "${help}" ${at} 200 jobs_text)
  require_contains("${jobs_text}" "ignored without --replications" "--jobs' help text")
else()
  message(FATAL_ERROR "unknown CHECK '${CHECK}'")
endif()
