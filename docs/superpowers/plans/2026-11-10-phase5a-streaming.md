# Phase 5a: the Simulator as a Streaming Data Source — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `simsys run <file> --stream` writes a run as NDJSON on stdout: a `run` message, the resolved `graph`, one `frame` per bucket of steps aggregated at the source (about 720 per run, a few KB each), and a `summary` that is `--report json`'s report, or an `error` message with file, line and column. `simsys explain --report json` prints the same graph. Every JSON document carries `format_version: 1`. No node model changes; every existing output stays byte-identical except the new first key of `--report json`.

**Architecture:** The spec `docs/superpowers/specs/2026-11-10-phase5a-streaming-design.md` decides everything below; this plan turns it into tasks. All new code is in `simsys.report` and the CLI:
- `report.cppm` gains the exported types `Frame`, `NodeRow`, `OpRow`, `FrameAggregator` (an Observer of `engine::StepRecord`, like `Summary` and `SeriesWriter`) and the renderers `render_json_line`, `render_graph_json`, `render_run_line`, `render_graph_line`, `render_frame_line`, `render_summary_line`, `render_error_line`.
- A new implementation unit `src/report/stream.cpp` (`module simsys.report;`) holds the aggregator and the stream renderers; `explain.cpp` gains the graph JSON.
- `Summary::Moments` and the percentile rule (`percentile_seen_by_client`, the `fill_latencies` choice of moments) move from `report.cpp` / the private section of `Summary` to the `:shared` partition, so a bucket's p99 and a run's p99 are one rule.
- `session::RunObservers` gains `report::FrameAggregator* frames{}`; `run_summary` hands each record to it and calls its `finish()` after the loop.
- `cli/main.cpp`: `run --stream`, `--points`, `--every`, the refusals, a `run_streamed` path, `explain --report json`; `load_for_run`'s error keeps the loader's line and column.

**Tech Stack:** C++23, GCC 14.2 named modules (the eleven defects of CLAUDE.md §3 apply; spec §9 maps them to 5a), CMake presets `gcc14-debug` / `gcc14-release` / `gcc14-asan` / `gcc14-tsan`, Catch2 3.7.1, yaml-cpp (also the tests' JSON reader: JSON is YAML 1.2 flow syntax), CLI11, clang-format-18.

**Workflow (the user's rules, 2026-11-09):**
- Branch `phase5a/streaming`, cut from `main` at `4549e46`; nothing is committed to `main`. The spec and this plan are the branch's first commit.
- Each task: a **red commit** (tests failing for the stated reason), a **green commit** (the minimum that passes), a refactor if due. Commit messages say *why*.
- After each green commit: `ctest --preset gcc14-debug` whole, and `cmake --build --preset gcc14-release` (the fourth and tenth defects show only there).
- After each batch: a code review of the batch's diff, a fix commit, then a **checkpoint**: stop and wait for the user's go-ahead, with a line on the next batch's size.
- Before the push: `gcc14-asan` builds and passes; CI's clang-format-18 command is clean. Ask the user before `git push`; open a PR and let CI (debug, release, asan, tsan, format) decide.
- Commits: `git -c user.name=Claude -c user.email=noreply@anthropic.com commit`, the message ending with the `Co-Authored-By` line.

| Batch | Tasks | Size |
|---|---|---|
| A | 1 (`format_version`, one-line JSON), 2 (`explain --report json`) | small–medium: one key in three documents, one new renderer over the resolved graph |
| B | 3 (node and link buckets), 4 (op buckets) | medium: the aggregator, its rules and theory tests; moving Moments and the percentile rule |
| C | 5 (`run --stream`) | medium: the CLI path, the refusals, error lines, process-level tests, the speed measurement |
| D | 6 (docs), the 5a review, fix round, ASan, push, PR | medium: schema v1.7, spec §22, README |

**Numbers in this plan.** A number in **bold** is pinned by a test before the implementation exists and comes with its derivation; a number marked *measured* is pinned only after the Release binary produced it.

---

## File structure

- `src/report/report.cppm`: the new exported declarations (Tasks 1–5). One new include in its global module fragment, `<functional>` (the aggregator's sink); never `<chrono>`, `<format>`, `<filesystem>` or `<print>` (first defect).
- `src/report/report.cpp`: `format_version` in `json_report_lines` and the replication JSON (1); `render_json_line` (1); `Summary` uses the moved Moments (4).
- `src/report/shared.cppm`: `Moments` and the percentile rule, moved (4).
- `src/report/explain.cpp`: `render_graph_json` (2).
- `src/report/stream.cpp` (new, listed in `CMakeLists.txt`'s `target_sources` of `simsys` as a plain source): `FrameAggregator`, the line renderers (3–5).
- `src/session/session.cppm`, `session.cpp`: `RunObservers::frames`, `finish()` after the loop (5).
- `src/cli/main.cpp`: `explain --report json` (2); `--stream`, `--points`, `--every`, `run_streamed`, the refusals, the error line, the CLI11 parse error as a line when `--stream` is among the arguments (5).
- Tests: `tests/unit/test_report.cpp` (1), `tests/unit/test_report.cpp` or a new `tests/unit/test_stream.cpp` (2–5; new file added to `unit_tests` in `tests/CMakeLists.txt`), `tests/theory/test_stream.cpp` (new, in `theory_tests`: the conservation and closed-form checks of 3–4), `tests/scenario/test_scenarios.cpp` (2, 3: shipped scenarios), `tests/cli/stream.cmake` (new, 5: process-level checks).
- Docs (6): `docs/scenario-schema.md` (v1.7), the main spec (§22), `README.md`, study notes as the code needs them.

---

## Batch A

### Task 1: `format_version`; the report on one line

**Files:** `src/report/report.cpp` (`json_report_lines`, `render_replications_json`), `report.cppm` (`render_json_line`, the exported `constexpr int format_version = 1;`, without `inline`: seventh defect); tests in `tests/unit/test_report.cpp`.

- [ ] Tests:
  - `render_json` of a run of the hello tutorial starts with `{\n  "format_version": 1,\n  "scenario": "hello"`; an estimate's the same; the replication JSON's top level the same (its per-seed objects do not repeat it);
  - every other line of each document equals today's: the test removes the `format_version` line and compares with the text the same test pins from `main` (taken by running the test's inputs at `4549e46` and pasted as the expected string, chain tutorial, run and estimate);
  - `render_json_line(summary, slos)` contains no `\n`, and parsed with `YAML::Load` gives the same tree as `render_json` parsed (compared node by node: keys, scalars as text, sequence lengths) on chain, rate-limiter (rejections), news-feed (a fault block) and youtube (a link);
  - existing tests that pin JSON's first lines are updated in the red commit, each with the new key only.
- [ ] Red commit `tests: every JSON document names the output format it follows`.
- [ ] Implementation: one `json_field("format_version", ...)` first in `json_report_lines` and in the replication document; `render_json_line` joins the same lines with `,` and no indentation. The pretty JSON nests arrays of objects one per line: the one-line form is built from the same `json_*` pieces with a `separator` parameter (`",\n"` or `","`), not by deleting newlines from the text (a newline inside a string value is already escaped by `json_string`, but deleting layout by text is the trick CLAUDE.md §4 asks not to play).
- [ ] Green commit `report: a format_version first in every JSON document, and the report on one line for the stream`.

### Task 2: `explain --report json`, the `graph` object

**Files:** `src/report/explain.cpp`, `report.cppm` (`render_graph_json(const graph::Graph&)`, `graph_json_fields(g)` for the stream to reuse), `src/cli/main.cpp` (`add_report_options` on `explain`; `table` stays the default); tests in `tests/unit/test_stream.cpp` (new), `tests/scenario/test_scenarios.cpp`.

- [ ] Tests (each parsed with `YAML::Load`):
  - chain tutorial: `format_version` **1**; `nodes` **users, api, db** in that order (`order:` of `explain`); api `type` `service`, `replicas` **2**, `capacity_per_s` **2000** (2 × 1k/s), `unit` `requests`, `yellow` **0.7**, `red` **0.9**, `queue_limit` **1000**; users `capacity_per_s` null; `links` **2**, `users -> api` with `direction` `inout`, `one_way_s` **0.00025** (intra_dc_rtt / 2 = 0.5 ms / 2), `bandwidth_mb_s` null; `ops` **users.read** share **0.8**, **users.write** **0.2**;
  - youtube: `cdn_us` `unit` `MB`; the `cdn_eu -> transcoded_store` link's `bandwidth_mb_s` **5000** (40 Gbit/s ÷ 8 = 5 GB/s; `explain`'s check line shows 5k MB/s); `transcoders` has `workers` **600**;
  - url-shortener: `url_db` `unit` `units`; chat-system: a `ws_gateway` with its connection cap; kv-store lab: `replica-requests`;
  - news-feed: `faults` has its scripted outage with `at_s` **54000** (15:00) and `for_s` **30**; `slos` lists the scenario's three in file order;
  - `node_columns`, `link_columns`, `op_columns` equal the lists of spec §6.2 (pinned verbatim: they are the contract);
  - every shipped scenario's graph JSON parses and has as many `nodes` as `explain`'s `order:` line names (scenario test);
  - `simsys explain <file>` without `--report` is byte-identical to today on all fourteen (scenario test against the text the test renders with `render_explain`, unchanged).
- [ ] Red commit `tests: explain describes the resolved graph as JSON, the diagram's source`.
- [ ] Implementation: the unit (`requests`, `units`, `MB`, `replica-requests`) comes from the rule 4a.8 built for the `units:` line (`report.cpp` near line 798: factor the per-node decision out of the line's text and reuse it; do not restate the rule). The zone names come from `fluid::Zone` (`std::to_underlying` gives the frame's number: C++23, first-use comment in `stream.cpp`). One `json_graph_node` per node type family; a type-specific key only where the spec names it.
- [ ] Green commit `report, cli: explain --report json, the resolved graph as data`.

**Checkpoint after Batch A** (review, fixes, stop).

---

## Batch B

### Task 3: node and link buckets

**Files:** `src/report/stream.cpp` (new), `report.cppm` (`NodeRow`, `Frame`, `FrameAggregator`), `CMakeLists.txt`; tests in `tests/unit/test_stream.cpp`, `tests/theory/test_stream.cpp` (new, added to `theory_tests`).

The types (exported; each member initialised, `-Wmissing-field-initializers`):

```cpp
// One node's or one bandwidth link's numbers over one bucket of steps (spec §6.2). Optional where
// the column does not apply to the node (a client's queue, a service's connections) or where the
// value is not finite (an unbounded lag, a latency of a node that never answered).
struct NodeRow { std::optional<double> demand_per_s, drain_per_s, ...; };
struct Frame { SimTime t{}; double dt_s{}; std::vector<NodeRow> nodes{}, links{}; std::vector<OpRow> ops{}; };
// Pattern: Observer — like Summary and SeriesWriter, it watches every StepRecord; it hands each
// finished bucket to `sink` (Pattern: Strategy for what happens to a frame: the stream writes it,
// a test collects it), so the rules are tested without any output.
class FrameAggregator {
 public:
  FrameAggregator(const graph::Graph& g, std::size_t steps_per_bucket, std::function<void(const Frame&)> sink);
  void observe(const engine::StepRecord& record);
  void finish();  // hands over a last, partial bucket; nothing if the last bucket was full
};
```

Special members of `FrameAggregator` and `Frame` declared in the class and defaulted after it, out of line (tenth defect: importers instantiating `std::vector` members' moves in Release).

- [ ] Tests (theory, each with its derivation in a comment):
  - **one step per bucket equals the series**: on hello, chain, rate-limiter, youtube (first 60 s via `--duration`-equivalent `RunOptions`), each frame's node row equals the `SeriesWriter` row of that step, number by number after the same `format_number` (the series is the reference; this is the identity of the aggregation);
  - **conservation per bucket**: a 1k/s service sent **1.5k/s**, `queue_limit` 100, step 100 ms, buckets of 7 steps (does not divide 600, so the last is partial): in every frame `mean_demand · dt_s` equals `(mean_drain + mean_dropped) · dt_s + backlog − previous backlog` to **1e-9** relative; after the room fills, `dropped_per_s` is **500** (1.5k − 1k) and `backlog` **100**;
  - **means and peaks**: a client whose rate steps from 500/s to 1500/s at t = 1 s into a 2k/s M/M/1, buckets of 20 steps at 100 ms: the bucket over [0, 2 s) has `mean_rho` **0.5** ((10 × 0.25 + 10 × 0.75) / 20) and `peak_rho` **0.75**;
  - **worst zone and red seconds**: a bucket with exactly one red step (a one-step spike) has `zone` **2** and `red_s` **0.1**;
  - **levels**: chat-system's gateway `connections` in a frame equals the last step's of its bucket (series at the bucket's last step);
  - **latency**: a bucket's `latency_ms` is the drain-weighted mean of its steps' sojourns (two steps of drain 1 and 3 with sojourns 2 ms and 4 ms: **3.5 ms**); null when a step's latency is infinite;
  - **null where it does not apply**: a client row has null queue columns; `queue_lag_s` null on a service, a number on youtube's `transcode_q`;
  - **bucket count and times**: 600 steps in buckets of 7 → **86** frames (600 = 85 × 7 + 5: 85 full buckets and one of 5 steps), the last `dt_s` **0.5** (5 × 0.1 s), the others **0.7**; `t_s` of frame k is `k × 0.7`;
  - `finish()` twice hands over nothing the second time; `finish()` on a run whose steps divide evenly hands over nothing.
- [ ] Red commit `tests: a bucket of steps keeps the run's means, peaks and levels, and conserves flow`.
- [ ] Implementation: per node a small accumulator struct (sums × dt, maxima, last values, worst zone, red time, drain-weighted latency sum); the bucket's `dt_s` is the sum of its steps' dt (Summary::step_length_s's rule: a step's length from consecutive `t`s, the scenario's step for the first). No lambda in a range adaptor (fifth defect): named `detail::` helpers.
- [ ] Green commit `report: FrameAggregator, a run's frames at the source`.

### Task 4: op buckets

**Files:** `src/report/shared.cppm` (receives `Moments` and the percentile rule), `report.cpp` (`Summary` uses them; behaviour unchanged), `stream.cpp` (`OpRow`); tests in `tests/theory/test_stream.cpp`.

- [ ] Tests:
  - one step per bucket: each op row's rates and `mean_ms` equal `--series-ops`' row for the step (`OpSeriesWriter`);
  - **a constant-rate run's bucket p99 equals the run's p99** (chain: every step after warmup is identical, so the pooled moments of any bucket are one step's, and `Summary`'s whole-run p99, **18.9 ms** for `users.write` as the README shows, is every bucket's after warmup, to 1e-12 relative);
  - with a timeout (rate-limiter lab's `call`): the bucket p99 follows `percentile_seen_by_client`'s rule, capped at the timeout; a bucket where more than 1 % timed out gives the timeout itself (the rule's `>1s` case: the JSON value is the timeout in ms, as the report's JSON writes it);
  - nothing answered in a bucket (an op behind a node that is down, news-feed's 15:00 outage, 30 s, in 10 s buckets): `mean_ms` and `p99_ms` null;
  - after the move: every existing `Summary` test passes unchanged (the refactor is behaviour-preserving; no test edited).
- [ ] Red commit `tests: a bucket's p99 is read by the same rule as the run's`.
- [ ] Implementation: first the move as its own commit (`report: the latency mixture and the percentile rule in :shared, for the frames to share`; all tests green before and after), then `OpRow` accumulation (three `Moments` per op as `OpTotals` keeps), then green.
- [ ] Green commit `report: the frames' op rows, the bucket's p99 by the run's rule`.

**Checkpoint after Batch B.**

---

## Batch C

### Task 5: `run --stream`

**Files:** `src/report/stream.cpp` (line renderers), `report.cppm`, `src/session/session.cppm`, `session.cpp` (`RunObservers::frames`; `run_summary` calls `frames->finish()` after the loop), `src/cli/main.cpp`; tests in `tests/unit/test_stream.cpp`, `tests/cli/stream.cmake` (new), `tests/CMakeLists.txt`.

Bucket size (a pure function, `report::steps_per_bucket(steps, points)` and `report::steps_per_bucket(steps, step, every)`, returning `std::expected<std::size_t, std::string>`):
- `--points N`: `ceil(steps / N)`; N in 1..100,000; default **720**.
- `--every d`: `d / step` when whole, else refused; `d` not longer than the run.

The CLI path, `run_streamed(file, args)`: every output line goes through one function that writes the line and `\n` with one `std::fwrite` and `std::fflush(stdout)`. Load as `run` loads (`load_for_run`, whose error type becomes `struct RunRefusal { std::string text; std::string file; int line{}; int column{}; }` so the stream's `error` keeps the loader's location while stderr's text stays byte-identical); write `run`, `graph`; run with the aggregator writing each frame; evaluate the SLOs exactly as `print_report` (factored: `report_exit_code(summary, results)` shared by both); write `summary`; return the exit code. A `std::exception` from the run is caught in `run_streamed`, written as an `error` line, exit 1.

CLI11's own parse errors (an unknown flag, a missing file) happen before `--stream` is known: `main` checks `argv` for a literal `--stream` and, if present, writes CLI11's message as an `error` line (`file` null) instead of letting `app.exit` print it.

- [ ] Tests (library, `test_stream.cpp`, lines parsed with `YAML::Load`):
  - a stream of hello into a `std::ostringstream` (the writer takes an `std::ostream&`; the CLI passes stdout): line 1 `type` `run`, `format_version` **1**, `frames` **600** for `--points 1000` (600 steps < 1000: a frame per step), `bucket_s` **0.1**; line 2 `graph`; lines 3..602 `frame` with increasing `t_s`; last `summary`; no other line;
  - `--points 720` on youtube's step count (864,000) → **1200** steps a bucket, **720** frames, `bucket_s` **120**: checked with `steps_per_bucket` and on youtube with `--duration 2h` (72,000 steps, `--points 720` → **100**-step buckets, 720 frames) to keep the test fast;
  - `steps_per_bucket`: `--every 30s` at 100 ms → **300**; `--every 250ms` → refused `--every 250ms: not a whole number of 100ms steps`; `--every 2h` on a 1 h run refused; `--points 0` and `100001` refused;
  - `summary.report` parses to the same tree as `render_json` for the same run (chain), and `summary.exit_code` is **2** on the unique-id lab (its SLO fails on purpose), **0** on chain;
  - the same scenario and seed twice: byte-identical streams; `--seed 2` on a noisy probe differs;
  - **one record stream, two observers**: the frames cover the whole run and the summary only the part after warmup, so the test sums `dropped_per_s · dt_s` over the frames that start at or after `warmup_s` (buckets aligned to the warmup: rate-limiter's and news-feed's warmups are whole multiples of the bucket chosen for the test) and pins it equal to the summary's `dropped` total, to 1e-9 relative.
- [ ] Tests (process, `stream.cmake`, one ctest per CHECK, named after the property):
  - `only_json`: `simsys run hello --stream` exits 0, stderr empty, every stdout line starts with `{"type":`;
  - `error_line`: a scenario with a misspelled key at a known line → exit **1**, one line, `type` `error`, its `line` and `column` those of the loader's message on stderr without `--stream`;
  - `refusals`: `--stream --series x.csv`, `--stream --report json`, `--stream --replications 3`, `--points 10` without `--stream`, `--points 10 --every 1s --stream`: each exit 1 with its spec §4 message (as an `error` line when `--stream` is present, on stderr otherwise);
  - `parse_error`: `--stream --no-such-flag` → an `error` line, exit 1;
  - `exit_code`: unique-id lab with `--stream` exits **2** and its last line is `summary`.
- [ ] Red commit `tests: run --stream writes the run as lines of JSON, frames aggregated at the source`.
- [ ] Implementation; green commit `report, session, cli: run --stream, the frontend's data source`.
- [ ] Measured (Release, by hand, recorded in this plan under the task): youtube and chat-system wall time with and without `--stream` (allowed: **5 %** slower), the stream's size in bytes and lines, the time to the first frame (`ts` per line or the server's view: `simsys ... | head -3` timing). If the 5 % is exceeded, profile before changing anything (the suspects: the per-node accumulator's `std::optional`s, the flush per line).

**Checkpoint after Batch C.**

---

## Batch D

### Task 6: docs

- [ ] `docs/scenario-schema.md` **v1.7**: "Command line: `simsys run --stream` (phase 5a)": the messages with an example of each (hello, real output), the three column tables, the bucket rules and the conservation over a bucket, `--points` / `--every`, the refusals, the exit codes, the contract's rules (spec §7). "Command line: `simsys explain --report json`". The JSON key table gains `format_version`. "New in v1.7" lists `format_version` first (the one change an existing reader sees).
- [ ] Main spec **§22**: the rules as built, each review's corrections, in §21's style; §1's "Out of scope: a GUI" and §11's "`--series` ... the contract for the future GUI" amended to point at §22.
- [ ] `README.md`: status line (phase 5a), Usage gains `--stream`, `--points`, `--every`, `explain --report json`; a `head -3` of a hello stream; youtube's sizes (*measured*).
- [ ] Study notes: whatever first uses the tasks met. `std::to_underlying` (C++23) already has its section in `small-features.md`: `stream.cpp` gets the inline first-use comment linking to it. A short `docs/study/` note on line-framed streams only if a reviewer finds the comments insufficient.
- [ ] Commit `docs: schema v1.7, spec §22, the stream`.

### The 5a review, then the PR

- [ ] A code review of the whole branch against the spec (`code-review` at the level the user chooses), a fix commit.
- [ ] `cmake --preset gcc14-asan && cmake --build --preset gcc14-asan && ctest --preset gcc14-asan`.
- [ ] Byte identity against `main` (as 4a did): `run` table output, `estimate`, `explain`, `sweep`, `--series` on all fourteen scenarios unchanged; `--report json` differs only by the `format_version` line.
- [ ] CI's clang-format-18 command clean.
- [ ] Ask the user; push; open the PR (body: what, why, measured sizes and speed, the contract's rules), CI decides.

---

## Self-review

- Every spec section has a task: §4 (CLI) → 5; §5 (`format_version`) → 1; §6.1 (`run`, `graph`) → 2, 5; §6.2 (frames) → 3, 4; §6.3 (summary) → 1, 5; §6.4 (size, speed) → 5 measured; §6.5 (errors) → 5; §7 (contract) → 2 (columns pinned), 5 (determinism), 6 (written down); §8 (later) → nothing built; `rev` written as 0 (2, 5); stdin read by nothing (5: no code reads it).
- Each bold number has its derivation in the task. The one taken from a README (chain's `users.write` p99 **18.9 ms**) is the table's rounding: the test compares the bucket's p99 with the Summary's value, not with the printed text.
- Risks: the move of `Moments` into `:shared` (a partition that imports no part of simsys.report: `Moments` has no dependency on report, and the percentile rule needs only `simsys.queueing`, which `:shared` may import). The `std::function` sink in an exported class needs `<functional>` in `report.cppm`'s global module fragment, which today has `<cstddef>`, `<iosfwd>`, `<optional>`, `<span>`, `<string>`, `<string_view>`, `<vector>`; `<functional>` is not on the first defect's list (`session.cppm` already includes it beside its imports), so it is added there.

---

## As built (2026-11-10)

- **Task 1:** red `339b4eb`, green `a528311`. The one-line report is JSON minification
  (`json_on_one_line`) of the pretty one, not a separator parameter through every renderer.
- **Task 2:** red `2df1a91`, green `a488e96`; batch A review fixes `13c9812` (cache flush `for_s`
  null, datacenters as objects, `shards`, ops sorted, `format_version` written once by
  `json_document`, shared helpers in `:shared`). The graph JSON lives in `explain.cpp`.
- **Task 3:** red `4fd12a0`, green `979ab02`. The aggregator is a unit of simsys.report
  (`src/report/stream.cpp`), not a module of its own. Plan numbers corrected: the conservation probe
  is mm1's 100 steps in buckets of 7 (15 frames, the last 0.2 s); 600 steps in buckets of 7 are 86
  frames, the last 0.5 s.
- **Task 4:** red `508e2b8`, refactor `4c96067` (Moments, ResponseMoments, seen_latencies:
  module-internal declarations in `report.cppm`, not the `:shared` partition), green `c016bdb`. The
  hand-computed p99 of the identical-steps test is 9.810 ms (the first draft's 9.106 was an
  arithmetic slip).
- **Task 5:** red `a33c3ae`, green `f215d68`; batch B review fixes `829d21a` (NaN-propagating lag,
  pipe latency by demand, `OpRates`, `as_node_sample`). Frame numbers have six significant digits.
  The process tests are `tests/cli/stream.cmake` (five checks).
- **Measured (Release):** youtube 27.4 s without, 24.3 s with `--stream`; chat-system 59.9 s and
  60.2 s (within the 5 %). youtube's stream: 723 lines, 1,134,255 bytes, 1.55 KB a frame;
  chat-system's 873,526 bytes. First frame after 43 ms.
