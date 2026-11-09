# Phase 4a: the Sweep and the Reports, Finished — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A sweep prints each point's SLO verdicts and exit code, measures a node's `reject_rate`, spaces its points geometrically and crosses two keys in a grid; `estimate --at <time>` evaluates the steady state at any instant; `estimate`'s table and JSON report rates instead of one step's worth of totals, and JSON tells an unbounded queue lag from none; four phase 2a leftovers (M7, M9, M12, M13) are paid. No node model changes: every number `run` prints for the fourteen shipped scenarios stays the same.

**Architecture:** The spec `docs/superpowers/specs/2026-11-09-phase4a-sweep-reporting-design.md` (reviewed 2026-11-09) decides everything below; this plan turns it into tasks. The sweep work stays inside `simsys.sweep` (`sweep.cpp`, `sweep.cppm`) and the CLI; `SweepParam` gains a spacing, `run_sweep` takes up to two params and an SLO flag, and each point still loads through `config::load(path, overrides)` and runs through `session::run_summary`. `estimate --at` threads a `SimTime` from the CLI through `session` to `Simulation::estimate(at)`, which already exists as `settle(at)`. The report changes are in `simsys.report` (`report.cpp`, `report.cppm`). Tasks 1–8 are the spec's sub-steps 4a.1–4a.8; Task 9 is 4a.9 (docs), followed by the review and the PR.

**Tech Stack:** C++23, GCC 14.2 named modules (the eleven defects of CLAUDE.md §3 and their workarounds apply to every task; spec §8 maps them to this sub-phase), CMake presets `gcc14-debug` / `gcc14-release` / `gcc14-asan` / `gcc14-tsan`, Catch2 3.7.1, yaml-cpp, CLI11, clang-format-18.

**Workflow (the user's rules, 2026-11-09):**
- One branch for the sub-phase, `phase4a/sweep-reporting`, cut from `main` at `b97cd0f`; nothing is committed to `main`. The spec and this plan are the branch's first commit.
- Each task: a **red commit** (the tests, failing for the stated reason: a missing symbol, a wrong number, a missing message), then a **green commit** (the minimum that passes), then a refactor if one is due. Commit messages say *why*.
- After each green commit: `ctest --preset gcc14-debug` whole, and `cmake --build --preset gcc14-release` (the fourth defect shows only there).
- **Checkpoints:** work stops after each batch below and waits for the user's go-ahead, with a line on how big the next batch is.
- Before the push: `gcc14-asan` builds and passes; clang-format-18 clean as CI runs it. Ask the user before `git push`; then open a PR and let CI (build-test, build-test-release, asan, tsan, format) decide.

| Batch | Tasks | Size |
|---|---|---|
| A | 1 (M9, M13), 2 (`reject_rate`), 3 (`:log`) | small: one loader message, one CLI refusal, one metric row, one parser branch |
| B | 4 (grid), 5 (`--slos`) | medium: `run_sweep`'s signature, the renderers, the CLI |
| C | 6 (`--at`), 7 (`estimate`'s shape), 8 (units) | medium: touches the most existing tests (estimate tables and JSON) |
| D | 9 (docs), the 4a review, fix round, ASan, push, PR | medium: docs and measured README numbers |

**Numbers in this plan.** A number in **bold** is pinned by a test before the implementation exists and comes with its derivation; a number marked *measured* is what a README quotes from the Release binary, pinned only afterwards.

---

## File structure

- `src/config/nodes.cpp`: `read_write_ops` refuses a duplicate (Task 1).
- `src/cli/main.cpp`: `curve --seed` needs `--noise` (1); `sweep` takes `--param` twice and `--slos` (4, 5); `estimate --at` (6).
- `src/sweep/sweep.cppm`, `sweep.cpp`: `MetricKind::reject_rate` (2); `Spacing {linear, geometric}` in `SweepParam` (3); `run_sweep(path, span<const SweepParam>, metrics, jobs, SloColumns)`, the grid, `SweepRow::values` (one per param), `SweepRow::slos` (4, 5); the renderers.
- `src/engine/engine.cppm`: `Simulation::estimate(SimTime at = {})` (6). The default argument is a `SimTime{}` literal, not an exported variable (seventh defect).
- `src/session/session.cppm`, `session.cpp`: whatever `estimate` needs to pass `at` (6), if the CLI does not call the engine directly.
- `src/report/report.cppm`, `report.cpp`: `Summary::of_estimate(g, record, at)`; `dropped_per_s`, `rejected_per_s`, `queue_lag_unbounded`, `at_s` in JSON; the estimate table's columns; the `units:` line (6, 7, 8).
- Tests: `tests/unit/test_config.cpp` (1), `tests/unit/test_sweep.cpp` (2–5), `tests/cli/sweep_jobs.cmake` and `tests/CMakeLists.txt` (4, 5: the process-level checks), a new `tests/cli/curve_seed.cmake` (1), `tests/unit/test_report.cpp` (7, 8), `tests/theory/test_core.cpp` (6: the `--at` closed form), `tests/scenario/test_scenarios.cpp` (8: the `units:` line on url-shortener and youtube).
- Docs (9): `docs/scenario-schema.md` (v1.6), the main spec (§21), `docs/study/ranges.md`, `scenarios/labs/rate-limiter/README.md`, `scenarios/book/youtube/README.md`, `scenarios/book/url-shortener/README.md`, `README.md`.

---

## Batch A

### Task 1: duplicate `write_ops` (M9); `curve --seed` without `--noise` (M13)

**Files:** `src/config/nodes.cpp` (`read_write_ops`), `src/cli/main.cpp` (`curve`); tests in `tests/unit/test_config.cpp`, `tests/cli/curve_seed.cmake` (new), `tests/CMakeLists.txt`.

- [x] Tests (loader; message verbatim): `write_ops: [write, write]` on a `cache` → `node 'c': duplicate write_ops op 'write'`; the same on a `cdn`, a `sql_db`, a `blob_store`, a `kv_cluster` (one `TEST_CASE` per type, named after the property: "a duplicate write op is refused on every type that reads write_ops"); `[write, delete, write]` names `write`, the first repeated name. The wording is checked against the link reader's duplicate-`ops` message in `links.cpp` and follows its form if it differs.
- [x] Tests (process, `curve_seed.cmake`): `simsys curve scenarios/tutorial/hello/scenario.yaml users --seed 3` exits **1** with `--seed needs --noise (without it the curve has no randomness)` on stderr and nothing on stdout; with `--noise` it exits 0 and its output equals today's.
- [x] Red commit `tests: a duplicate write op and a seed without noise are refused`.
- [x] Implementation: in `read_write_ops`, after reading the list, find the first repeat (a `std::set<std::string>` or `std::ranges::sort` + `std::ranges::adjacent_find` on a copy: C++20 ranges, first-use comment if new in `nodes.cpp`, which is covered by `detail.hpp`'s comments only for what `detail.hpp` uses). In `main.cpp`, after parsing `curve`, `if (args.seed && !args.noise)` refuse. No shipped scenario has a duplicate (`grep -n write_ops scenarios -r` checked in the red commit's message).
- [x] Green commit `config, cli: refuse a duplicate write op and a seed the curve cannot use`.

Done: red `0268467`, green `20c6cda` (1689 tests). The message follows the link reader's form,
`node 'c': duplicate write_ops op 'write'` (spec and plan updated); the check runs after every
element is read as an op name, so a non-scalar is still reported as one. `curve --seed`'s help
says "needs --noise".

### Task 2: `<node>.reject_rate`

**Files:** `src/sweep/sweep.cppm` (`MetricKind::reject_rate`), `src/sweep/sweep.cpp` (`node_metric_words`, `metric_grammar`, `node_metric`, `problem_with_node`, `human_cell`); tests in `tests/unit/test_sweep.cpp`.

- [x] Tests:
  - `parse_metric("limiter.reject_rate")` gives `MetricKind::reject_rate`, node `limiter`;
  - on a probe (a client at **1.5k/s** into a token bucket of `limit: 1k/s` and a `burst` that is empty after the first step, step 100 ms, then a service of 10k/s): the sweep's value equals `Summary::reject_rate("limiter")` exactly, and after warmup is **(1.5k − 1k) / 1.5k = 0.3333** to 4 digits;
  - a service that refuses nothing: **0**;
  - on a client: `--metric: users.reject_rate: 'users' is a client, which serves nothing, so it has no reject rate`;
  - the unknown-metric message lists `reject_rate` after `drop_rate` (the grammar text is pinned verbatim, so its old pin is updated in this commit).
- [x] Red commit; implementation (one table row, one `case`, the grammar string); green commit `sweep: a node's reject_rate as a metric`.

Done: red `c0e5d0b`, green `28dbe90` (1693 tests). The probe is a token bucket with `burst: 1`
before a 5k/s or 10k/s service: the limiter's reject rate is 1/3 within 1e-6, the service's 0. The
grammar is pinned by `contains`, not verbatim (no test pinned it before).

### Task 3: geometric spacing `:log`

**Files:** `src/sweep/sweep.cppm` (`enum class Spacing { linear, geometric }`, a field in `SweepParam`), `src/sweep/sweep.cpp` (`split_spec` reads the suffix, `expand` branches); tests in `tests/unit/test_sweep.cpp`.

- [x] Tests (`parse_param`, values as text):
  - `nodes.users.rate.base=1k/s..100k/s:3:log` → **`1k/s`, `10k/s`, `100k/s`** (1000 × 100^(k/2); the text is the same 12-significant-digit form `point_text` writes, so `10000/s` or `10k/s` is decided by today's formatter: the test pins what `point_text` gives for 1e4 with the unit `k/s`, i.e. the unit text of `from` repeated after the number in `from`'s unit);
  - `1ms..1000ms:4:log` → `1ms`, `10ms`, `100ms`, `1000ms`; the last value is the `to` text's number exactly (no `999.999999`);
  - `nodes.w.workers=10..1000:3:log` → `10`, `100`, `1000`; `nodes.w.workers=1..4:8:log` → `--param: workers=1..4:8:log gives 1 twice (rounded to whole workers); use fewer points` (4^(k/7) = 1, 1.219, 1.486, 1.811, 2.208, 2.692, 3.281, 4 round to 1, 1, 1, 2, 2, 3, 3, 4: the first repeat is **1**);
  - `0/s..1k/s:4:log` → `--param: 0/s..1k/s:4:log: geometric points need two positive ends`; `-1ms..1ms:3:log` the same;
  - `1..10:3:lin` → `--param: 1..10:3:lin: the only spacing suffix is :log`;
  - a linear spec parses exactly as before (`spacing == linear`, values unchanged: the existing tests stay green).
- [x] Red commit; implementation (`point_at` gains a geometric twin: `from * std::pow(to / from, k / (n − 1.0))`, `k == n − 1` returns `to`; whole keys round and check for a repeat; `n` limits unchanged); green commit `sweep: geometric points for a key read on a log axis`.

Done: red `bb01db7`, green `82bdfc2` (1699 tests). The suffix is found from the right, after the
count, so an end holding a colon (a clock time) still parses. The positive-ends case is
`0/s..1000/s:4:log`: `0/s..1k/s` is refused first for its two units. The CLI help names `:log` and
`reject_rate`.

**Checkpoint A.** Stop; report the three tasks' commits, the test count, and the size of Batch B.

**Review of batch A at `c93cd02`** (code-review skill, medium, 2026-11-09; the user asked for a
review after every batch): ten findings. 1. A `:log` range rounded a whole key's non-whole ends
silently (`workers=1.4..9.6:2:log` ran 1 and 10; `0.4..10` reached 0 workers). 2. `to / from`
overflowed on a range as wide as 1e-300..1e300. 3. `:three` as a count got the grammar message
instead of the count's. 4. `x=10:00..12:00:log` with the count forgotten reads the minutes as the
count. 5. The duplicate-op check was copied from `links.cpp`. 6. `SweepParam::spacing` untested.
7. The rounding test's `1..100:3:log` rounds nothing. 8. `curve_seed.cmake` checked the exit code
only. 9. The seed refusal ran before the file was loaded. 10. Equal ends under `:log` ran one
scenario several times on a non-whole key.
Decisions: 1 → the ends of a whole key must be whole on `:log` too, the points between round;
2 → the points are computed in logarithms; 3 → a word after the last colon is a spacing only after
a count of digits; 4 → left, a documented limit (the minutes of a clock time look like a count);
5 → one `detail::first_repeat` for both; 6, 7 → tests; 8 → two seeds give two curves, one seed the
same twice, a missing file is not reported as a seed problem; 9 → after the load; 10 → any two equal
points of a `:log` range are refused, on every key. Fixed in the commit after Task 4's green.

---

## Batch B

### Task 4: two keys, a grid

**Files:** `src/sweep/sweep.cppm` (`run_sweep` takes `std::span<const SweepParam>`; `SweepRow::value` becomes `values`, one text per param; `render_sweep` takes the span), `src/sweep/sweep.cpp` (`grid_points`, `point_label`, `prepare_point` with two overrides, the renderers' header), `src/cli/main.cpp` (`--param` repeatable: a `std::vector<std::string>`, at most 2); tests in `tests/unit/test_sweep.cpp`, `tests/cli/sweep_jobs.cmake`, `tests/CMakeLists.txt`.

- [x] Tests:
  - on hello, `--param nodes.users.rate.base=100/s..300/s:2 --param nodes.api.capacity=1k/s..2k/s:2`: **4 rows** in the order (100/s, 1k/s), (100/s, 2k/s), (300/s, 1k/s), (300/s, 2k/s); `api.mean_rho` **0.1, 0.05, 0.3, 0.15** (rho = rate / capacity, one replica: the key names are checked against hello's YAML in the red commit, and the probe's own file is used if hello's capacity key is not `capacity`);
  - the table's header is `nodes.users.rate.base  nodes.api.capacity  api.mean_rho` and the CSV's the same with commas;
  - a third `--param` → `--param: at most two keys (a grid of two); script a third`;
  - the same path twice → `--param: nodes.api.capacity is given twice`; two paths that reach one anchored value → the same message naming the second path (the probe YAML has `capacity: &cap 1k/s` and a second node with `capacity: *cap`; the plan's assumption that `config::check_override_path` can tell the two paths reach one node is verified first; if it cannot, the test pins refusing the alias in the second path instead, and the spec is amended);
  - `--param a=1..200:200 --param b=1..100:100` → `--param: 200 × 100 = 20000 points: at most 10000`;
  - an error at a point names both values: `point 3 (nodes.users.rate.base=300/s, nodes.api.capacity=1k/s): ...` (forced by a value the loader refuses at that point only);
  - process level: the grid's bytes are the same for `--jobs 1` and `--jobs 3` (a new `CHECK` in `sweep_jobs.cmake`, ctest name `simsys sweep over two keys gives the same bytes with --jobs 1 and 3`); every one-key sweep's bytes are unchanged (the existing checks).
- [x] Red commit (`run_sweep`'s new signature makes the test file fail to compile: the red reason is the missing overload; the message tests compile against `parse_params`, a new free function that checks the list).
- [x] Implementation: C++23 `std::views::cartesian_product` over two `std::views::iota` index ranges (first-use comment: what it is, that it replaces two nested loops whose order a reader must check, why here: the outer/inner order is the product's own; `docs/study/ranges.md` gets the longer note in Task 9). No lambda goes into an adaptor (fifth defect): an index pair is turned into overrides by a named `detail::overrides_at(params, i, j)`. One key is the grid of one (the same code path, so a one-key sweep's output is unchanged by construction).
- [x] Release build checked (fourth defect: `std::vector<SweepParam>` and `std::vector<std::vector<std::string>>` crossing into the CLI); an `emit_special_members` line added only for an undefined reference that appears.
- [x] Green commit `sweep: a grid over two keys, the first the outer loop`.

### Task 5: `--slos`, a verdict per SLO and an exit code per point

**Files:** `src/sweep/sweep.cppm` (`enum class SloColumns { off, on }`; `SweepRow::slos`, a `std::vector<report::SloStatus>`; the importer then needs `simsys.report`'s `SloStatus`, so `sweep.cppm` gains `import simsys.report;` or carries its own three-value enum: the second keeps the interface importing nothing, which is what its header comment promises against the first defect, and is the choice), `src/sweep/sweep.cpp` (`measure_point` calls `report::evaluate_slos` on the summary it already has; `slo_headings`; the cells), `src/cli/main.cpp` (`--slos` flag; the stderr note for a scenario without SLOs); tests in `tests/unit/test_sweep.cpp`, `tests/cli/sweep_jobs.cmake`.

- [x] Tests:
  - hello with an SLO `{metric: p99_latency, node: users, op: ping, max: 5ms}` (a test-owned copy of the file), swept over `nodes.users.rate.base=100/s..900/s:3`: the cells are **PASS, PASS, FAIL** and the `exit` cells **0, 0, 2**. Derivation: hello's README gives the mean 1.61 ms, 2.5 ms, 10.5 ms at 100, 500, 900/s; the p99 of the lognormal fit is above 5 ms at 900/s by a wide margin and below it at 100/s; 500/s is pinned from the binary in the red commit (*measured*, with its value in the test comment), and the SLO's `max` is chosen so that it passes there;
  - each cell equals `simsys run`'s verdict on the same file with the point's value written in (a test that runs `session::run_summary` + `evaluate_slos` on a `config::load(path, override)` and compares, for every point);
  - headings: `p99_latency:users.ping`, `drop_rate`; two SLOs `p99_latency:users.ping` with different `max` → the second is `p99_latency:users.ping#2`;
  - CSV cells `pass`/`fail`/`skipped`, the exit column `0`/`2`;
  - a scenario with no SLOs: only an `exit` column, all `0`, and the CLI's stderr says `the scenario has no SLOs: every point exits 0`;
  - process level: exit code **0** with a failing point (the existing "exits 0 when an SLO fails at some point" check gains `--slos`); without `--slos` the bytes are unchanged.
- [x] Red commit; implementation; Release build; green commit `sweep: each point's SLO verdicts and exit code, on request`.

**Checkpoint B.** Stop; report, and the size of Batch C.

Done: Task 4 red `8c9a058`, green `c27b484`; Task 5 red `added2f`, green `bfccd47` (1721 tests).
As built: the grid's size message reads `200 x 100 = 20000 points` (ASCII); two paths reaching one
anchored value are refused as `<file>: error: --param: nodes.b.capacity reaches the same value as
nodes.a.capacity (a YAML anchor and its alias)` (`config::same_override_target` compares yaml-cpp
node identity); the SLO test's `max` is 20 ms, since hello's measured p99 is 5.66, 10.0 and 49.3 ms
at 100, 500 and 900/s (the plan's 5 ms fails everywhere); `sweep.cppm` names its own `SloVerdict`
and keeps importing no module; a skipped SLO is pinned with an external sink of `failure_rate: 1`.

**Review of batch B** (code-review skill, medium, two passes: Task 4 at `c27b484`, Task 5 at
`bfccd47`). Task 4: 1. `--param` as a vector option swallowed a file written after it (CLI11 takes
every following word); 2–5, 8. the exported `run_sweep(span)` trusted its caller (an empty span
indexed past the end, three params were silently cut to two, the 10,000-point and duplicate checks
lived only in `parse_params`, the same path twice blamed an anchor); 6. `sweep --help` said one key;
7. the YAML file is parsed once per check; 9. rows copied their points; 10. a temp file outlived a
failing REQUIRE. Task 5: 1. the no-SLO note promised exit 0 although a point that observed no step
exits 1; 2. the interface omitted exit code 1; 3. main.cpp's header still said sweep judges
nothing; 4, 5. two doc comments separated from their functions; 6. three lines over 100 columns and
an include out of order; 7. headings recomputed per point; 8. a temp file (as 10 above); 9. dead
fallback returns after exhaustive switches.
Decisions: fixed all but 7 of Task 4 (four parses of a small file before runs of seconds: left) and
8 of Task 5 (the loop needs the file; left). `--param` takes one value per occurrence
(`expected(1)`, `allow_extra_args(false)`, `TakeAll`), pinned by a CLI check with the file after
it; one `grid_problem` serves `parse_params` and `run_sweep`; the note reads `the scenario has no
SLOs, so no point can fail one`; `std::unreachable` (C++23) closes the two switches.

---

## Batch C

### Task 6: `estimate --at`

**Files:** `src/engine/engine.cppm` (`estimate(SimTime at = SimTime{})`: `settle(at)`), `src/cli/main.cpp` (`--at`, parsed with the fault `at` parser the loader uses for `at:`; if that parser lives only in `simsys.config`'s `detail`, the CLI calls an exported `config::parse_instant` added for it), `src/report/report.cppm`, `report.cpp` (`of_estimate(g, record, at)`, the table's first line, JSON `at_s`); tests in `tests/theory/test_core.cpp`, `tests/unit/test_report.cpp`, a `tests/cli` check.

- [x] Tests (theory; probe: client `base: 1k/s`, `wave: {shape: sine, period: 24h, amplitude: 0.5, peak_at: 12:00}`, into a service of **2k/s**, latency **0.5 ms**, `concurrency: 1`, zero-latency link):
  - `estimate(12:00)`: offered **1.5k/s** (1 + 0.5 cos 0), rho **0.75**, the service's mean wait **0.5 ms × 0.75 / 0.25 = 1.5 ms** (M/M/1, `Wq = s ρ / (1 − ρ)`, Poisson arrivals and exponential service, the defaults' SCVs of 1), its sojourn **2 ms**;
  - `estimate(00:00)`: offered **500/s** (1 + 0.5 cos π), rho **0.25**, wait **0.5 × 0.25 / 0.75 = 0.1667 ms**; byte-identical to `estimate()` without `at` (table and JSON);
  - a `spikes: [{at: 12:00, for: 10m, factor: 2}]` added: `estimate(12:05)` offers 1k × (1 + 0.5 cos(2π × 5/1440)) × 2 (the cosine is **0.999762**, so **2999.76/s** and rho **1.49988**: the overload's dropped rate **999.76/s**, in JSON's `dropped_per_s` after Task 7; until Task 7 the test reads the record);
  - a fault active at `at` is still left out (`node_down` on the service at 12:00 for 1h: `estimate(12:30)` equals the same file without the fault).
- [x] Tests (CLI): `--at 25:00` on a 24h run → exit 1, `--at: 25:00 must fall inside the run (24h)`; `--at 90m` accepted; JSON `"at_s": 43200` at `--at 12:00`, `0` without; the table's first line `estimate at t = 12:00: the curve without noise; explain's checks use the peak`.
- [x] Red commit; implementation; green commit `estimate: the steady state at any instant of the day`.

### Task 7: `estimate`'s shape (M12, per-second drops, `queue_lag_unbounded`)

**Files:** `src/report/report.cpp` (`render_json`'s node object, the estimate table's columns and summary line, the replications JSON's per-seed nodes), `src/report/report.cppm` (comments); tests in `tests/unit/test_report.cpp`, and every existing test that pins an estimate table or an estimate JSON node object (found with `grep -rn 'of_estimate\|"estimate"' tests`; each is updated in the red commit with the new shape, so the red reason is visible per test).

- [x] Tests:
  - probe: client **1.5k/s** into a service of **1k/s**, `queue_limit: 100`, step 100 ms. `estimate --report json`: `dropped_per_s` **500** (the full room drains 1k/s, the excess is dropped), `dropped`, `rejected` and `red_s` **null**. `run --report json` over 60 s with warmup 10 s: `dropped_per_s` **500** within 1e-9 relative once the room filled before warmup (100 / 500 = 0.2 s), `dropped` **25000** (500 × 50 s), unchanged keys keep their values;
  - `rejected_per_s` on the rate-limiter probe of Task 2: **500** in both commands;
  - `queue_lag_unbounded`: web-crawler's `frontier` `true` in `estimate` (its lag is unbounded there: phase 3 review, decision 2) with `peak_queue_lag_s` `null`; `false` in `run`; `null` on a service; `false` on a message queue that keeps up (youtube's `transcode_q` in `estimate`);
  - the estimate table: the summary line `scenario hello  steady state at 00:00  drop rate 0%`, the columns `dropped/s` and `rejected/s`, no `red_s`; `run`'s table byte-identical to today on hello and chain;
  - the replications JSON's per-seed node objects carry `dropped_per_s`, `rejected_per_s`, `queue_lag_unbounded`.
- [x] Red commit; implementation; green commit `report: estimate reports rates, and an unbounded lag says so`.

### Task 8: unit labels (M7)

**Files:** `src/report/report.cpp` (`unit_note`: the nodes whose `dropped` and `peak_backlog` are not requests, from the graph: a stated `cost` ≠ 1 on some op, a `sql_db` with a write op at the default cost 5, a `kv_cluster`, a byte-sized node), `src/report/report.cppm` (the comments that say "requests" where the value is capacity units); tests in `tests/unit/test_report.cpp`, `tests/scenario/test_scenarios.cpp`.

- [x] Tests:
  - url-shortener's `run` table contains `units: url_db counts capacity units (write cost 5)` once, under the node table (the exact list of nodes is pinned from the binary in the red commit, *measured*, and its derivation from the YAML written beside it);
  - youtube's contains `count MB` naming its byte-sized nodes (the CDNs and blob stores);
  - kv-store's names the cluster: `store counts replica-requests (N 3)`;
  - hello's and chain's tables have no `units:` line and are byte-identical to today;
  - the line is the same in `estimate`'s table.
- [x] Red commit; implementation; green commit `report: say which nodes count capacity units or megabytes`.

**Checkpoint C.** Stop; report, and the size of Batch D.

Done: Task 6 red `b7f31ae`, green `8a9bc0c`; Task 7 red `3bb5556`, green `dfed9a6`; Task 8 red
`d2216c0`, green `6d139e0` (1737 tests). As built: `--at` is refused in `curve`'s words (`--at
'30h' must fall inside the run (24h)`; at < duration, a fault's rule, not the plan's closed
interval); the CLI parses it with `core::parse_time_point` (no new config export); the header reads
`estimate at t = 13h (13:00)`; the run JSON's per-second figure is 499.99999999999994 (a sum of
0.1 s steps), so its test pins the key and the NodeSummary test the value; the steady state is a
`NodeSummary::steady_state` flag, not a writer argument, since the JSON writers run under
`std::views::transform` (fifth defect); the units line reads `units (peak_backlog and drops are not
requests here): ...`, cost groups joined by `and`.

**Review of batch C at `6d139e0`** (code-review skill, medium): 1. the windowed-SLO note still said
`at 00:00` under `--at`; 2. `dead_lettered` was still a one-step total under `estimate`; 3. the
links table and JSON kept one step's totals and red time; 4, 5. first-use comments missing in
`priced_ops` (`views::keys`, `ranges::to`, `ranges::unique`) and late in `estimate_instant`
(`std::unexpected`, `std::format`); 6. no units line under `run --replications`' banded tables;
7. the schema's key table (reviewed before the docs task updated it); 8. the header repeated an
instant the clock cannot show (`500ms (500ms)`); 9. a kv_cluster's stated `cost` was missing from
its units line. Decisions: all fixed but 7 (already done in Task 9's schema edit); the link pipes
get the steady-state flag and the same `dropped/s` header; the kv line reads `replica-requests (N
3; op cost 4 each)`.

---

## Batch D

### Task 9: docs

- [x] Schema **v1.6**: "New in v1.6", the JSON changes first (`dropped`, `rejected`, `red_s` `null` in `estimate`; `dropped_per_s`, `rejected_per_s`, `queue_lag_unbounded`, `at_s` new), then the sweep (`reject_rate`, `:log`, the grid, `--slos`), `estimate --at`, the duplicate `write_ops` refusal, `curve --seed`; the sweep section's "Ignored" sentence and "A sweep has no SLO columns" sentence rewritten; the JSON key table extended; the `estimate` paragraph at "`run --report json` (and `estimate --report json`), key by key" rewritten.
- [x] Spec **§21**: the rules as built, each review's corrections, in §20's style; §19 entry 15's "Narrower than the plan's header" note points at §21.
- [x] Study notes: `ranges.md` gains `std::views::cartesian_product` (C++23): what it is, the nested loops it replaces, the order of its tuples, that it takes no lambda (so the fifth defect does not apply), its use in `sweep.cpp`; `docs/study/README.md`'s ranges row names it.
- [x] READMEs (numbers *measured* from the Release binary): the rate-limiter lab's `burst` sweep with `limiter.reject_rate` and `api.peak_rho`; youtube's workers sweep with `--slos`; url-shortener's `estimate --at 13:00` against its windowed SLO; the root README's sweep example, status line ("phase 4a is built") and test count.
- [x] Commit `docs: schema v1.6, spec §21, the sweep's grid and verdicts, estimate at an instant`.

Done at `faf5a76` (docs only). **Review of batch D at `faf5a76`** (code-review skill, medium,
docs): nine findings, all fixed in the docs fix commit: spec §21 entry 9 still said the links
table keeps one step's totals (batch C changed it); the schema's v1.6 list, key table and spec
omitted that a pool's `dead_lettered` and a link's `red_s` are `null` under `estimate`; the
links-table change was undocumented; the windowed-SLO text still printed `(at 00:00 only ...)`;
the `--slos` exit column omitted 1; the sweep option table missed `--param` twice, `:log` and
`--slos`; two "one key" sentences; the rate-limiter README's "the same share" against 0.143 at
burst 100; url-shortener's "the start of that hour" for 13:00 against 13:02.

### The 4a review, then the PR

- [ ] An adversarial review (fresh context, own worktree, all four presets): warning-free builds and the whole suite; clang-format as CI; byte-identity of every one-key sweep, of `run` on the fourteen scenarios (only the `units:` line added where §7 says), of `estimate --at 00:00` against `estimate`; every new message verbatim against the schema; first-use comments; `Pattern:` labels; test names (no `[` or `]`, named after the property); no reference into a temporary (CLAUDE.md §3); functions over ~50 lines; the GCC workarounds of CLAUDE.md §3 in place. Recorded here as "Review of 4a at <sha>" with decisions.
- [x] Fix round, one commit per batch (`56249ff`, `48fe0ce`, `dcb5e8b`, `53a8fde`).
- [x] `cmake --preset gcc14-asan && cmake --build --preset gcc14-asan && ctest --preset gcc14-asan` green.

Closing checks at `53a8fde` (2026-11-09): Debug 1740/1740; Release builds and links clean;
`gcc14-asan` 1739/1739 (its preset already excludes the CPU-time test), about 14 minutes with the
build. Byte-identity against `main` (`b97cd0f`), both Release binaries, all fourteen scenarios ×
`validate`, `explain`, `run` (the table without its new `units` line) and `run --report json`
(without `dropped_per_s`, `rejected_per_s`, `queue_lag_unbounded`), and the hello one-key sweep:
0 differences. Not checked locally: clang-format-18 (not installed on this machine; lines were
held to 100 columns by hand), so CI's format job is the first formatter to see the branch.
- [ ] **Ask the user**, then `git push -u origin phase4a/sweep-reporting` and `gh pr create` (body: what and why, the test count, the checkpoints' commits; ends with the attribution line). CI's five jobs decide; a red job gets a fix commit on the branch, never a force-push without asking.

---

## Self-review

- **Spec coverage.** §4.1 → Task 2; §4.2 → 3; §4.3 → 4; §4.4 → 5; §5 → 6; §6 → 7; §7 → 8; §2's M9/M13 rows → 1; §8 → each task's Release check and the review; §9 → the tests bullets; §10 → 9.
- **Placeholder scan.** Three numbers are *measured* on purpose and say so: hello's p99 at 500/s (Task 5), url-shortener's `units:` list (Task 8), the READMEs (Task 9). Two assumptions are named with their fallback: whether `check_override_path` sees an anchor shared by two paths (Task 4), and where the CLI gets the `at` parser (Task 6).
- **Name consistency.** `Spacing` (3, 4), `SloColumns` (5), `SweepRow::values` and `SweepRow::slos` (4, 5), `estimate(SimTime at)` and `of_estimate(g, record, at)` (6, 7), `dropped_per_s`, `rejected_per_s`, `queue_lag_unbounded`, `at_s` (6, 7, 9). Existing names checked at `b97cd0f`: `parse_param`, `parse_metric`, `MetricKind`, `node_metric_words`, `metric_grammar`, `point_at`, `point_text`, `expand`, `prepare_point`, `measure_point`, `point_label`, `for_each_point`, `render_sweep`, `run_sweep`, `Simulation::estimate`, `Simulation::settle`, `Summary::of_estimate`, `Summary::reject_rate`, `report::evaluate_slos`, `report::all_pass`, `read_write_ops`, `config::check_override_path`, `config::Override`.
