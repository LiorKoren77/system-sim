# Phase 5b: the Watch-Only Web App — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** a page on localhost where one picks a shipped scenario and watches its day: the diagram coloured by zone, a looping time slider, the zone strip, per-node charts, the SLO table and the fault blocks. Everything it shows is precomputed offline by `npm run precompute`, which runs `simsys run <file> --stream --days 2 --every 2m` per 24 h scenario (and the plain stream for the others) into `web/app/public/datasets/*.ndjson.gz` + `index.json`. The page is static files. The one C++ change is `run --days N`.

**Architecture:** The spec `docs/superpowers/specs/2026-11-10-phase5b-web-mvp-design.md` decides everything below; this plan turns it into tasks.
- C++ (Task 1): `session::repeat_days(model::Scenario, int days) -> std::expected<model::Scenario, std::string>` in `simsys.session`; `cli/main.cpp`'s `load_for_run` applies it after the loader and before `build_graph`, wraps its message in `RunRefusal`; the stream's `run` line gains `"days"`.
- `web/` (Tasks 2–7): npm workspaces `stream` (`@simsys/stream`: the contract), `precompute` (`@simsys/precompute`), `app` (`@simsys/app`). Data path: `DatasetSource` (Strategy) → `DatasetStore` (Observer, columnar `Float64Array`s) → the views through `useSyncExternalStore`.
- CI (Tasks 2, 4): a `web` job; the release job uploads its `simsys` binary as an artifact the `web` job downloads for the fixture check and the precompute's integration test.

**Tech Stack:** C++23 / GCC 14.2 named modules (CLAUDE.md §3's defects; spec §5's precautions), also built locally with the `clang20-libcxx-debug` preset after Task 1. Node 22 LTS (v22.23.4, loaded with `source ~/.nvm/nvm.sh`: nvm is not in a non-interactive shell), npm workspaces, TypeScript (strict), `tsx` to run the precompute, Vite, React, `@xyflow/react`, `elkjs`, `uplot`, `yaml`, Vitest, jsdom, `@testing-library/react`, ESLint (`typescript-eslint`, `eslint-plugin-react-hooks`), Prettier. Versions: the current majors at install time, pinned by `web/package-lock.json` and listed in "As built".

**Workflow (the user's rules, 2026-11-09):**
- Branch `phase5b/web-mvp`, cut from `main` at `dbcf013`; nothing is committed to `main`. The spec and this plan are the branch's first commits.
- Each task: a **red commit** (tests failing for the stated reason), a **green commit** (the minimum that passes), a refactor if due. Commit messages say *why*.
- After each C++ green commit: `ctest --preset gcc14-debug` whole and `cmake --build --preset gcc14-release`. After each web green commit: `npm run check && npm test` in `web/`.
- After each batch: a code review of the batch's diff, a fix commit, then a **checkpoint**: stop and wait for the user's go-ahead, with a line on the next batch's size.
- Before the push: `gcc14-asan` builds and passes; CI's clang-format-18 command is clean; `npm run check && npm test && npm run build` clean. Ask the user before `git push`; open a PR and let CI decide.
- Commits: `git -c user.name=Claude -c user.email=noreply@anthropic.com commit`, the message ending with the `Co-Authored-By` line.

| Batch | Tasks | Size |
|---|---|---|
| A | 1 (`run --days N`) | medium: one pure function over the scenario, the CLI flag and its refusals, unit and process tests |
| B | 2 (`web/` skeleton, CI job), 3 (`@simsys/stream`) | medium: tooling once; the parser, the sources and the store, all pure TypeScript |
| C | 4 (`@simsys/precompute`, fixtures, CI check) | medium: child processes, the seam check, gzip, the index |
| D | 5 (shell, catalogue, time), 6 (diagram) | medium–large: the first React code; ELK |
| E | 7 (zone strip, charts, SLO table, fault blocks) | medium–large: uPlot and the report views |
| F | 8 (docs), the 5b review, fix round, ASan, push, PR | medium: schema v1.8, spec §23, READMEs, CLAUDE.md §5 |

**Numbers in this plan.** A number in **bold** is pinned by a test before the implementation exists and comes with its derivation; a number marked *measured* is pinned only after the Release binary produced it.

---

## File structure

- `src/session/session.cppm`, `session.cpp`: `repeat_days` (1). No new header in the interface's global module fragment (first defect): `<expected>` and `<string>` are there already for `build_graph`.
- `src/cli/main.cpp`: `--days` on `run`, its refusals, `load_for_run` applies `repeat_days`; the `run` line's `days` key (1). The `run` line's renderer lives in `src/report/stream.cpp` (`render_run_line`), which gains the key.
- Tests (1): `tests/unit/test_session.cpp` (`repeat_days`), `tests/unit/test_stream.cpp` (the `days` key), `tests/cli/run_days.cmake` (new, registered beside `run_duration.cmake` in `tests/CMakeLists.txt`: refusals and byte identity through the binary), `tests/scenario/test_scenarios.cpp` (news-feed's spike and chat-system's outage on both days).
- `web/package.json`, `tsconfig.base.json`, `eslint.config.js`, `.prettierrc`, `.prettierignore`, `vitest.workspace.ts` (2).
- `web/stream/src/`: `messages.ts` (the line types), `lines.ts` (the splitter), `parse.ts` (the contract), `source.ts` (`DatasetSource`), `memory-source.ts`, `file-source.ts`, `store.ts` (`DatasetStore`); `web/stream/test/` and `test/fixtures/` (3, fixtures written in 4).
- `web/precompute/src/`: `main.ts` (arguments, the pool), `scenarios.ts` (find them, `day` or `run`, the README title), `run-simsys.ts` (spawn, read lines), `window.ts`, `seam.ts`, `dataset.ts` (the dataset line, gzip), `index-file.ts`, `fixtures.ts`; `web/precompute/test/` (4).
- `web/app/`: `index.html`, `vite.config.ts`, `src/main.tsx`, `src/App.tsx`, `src/use-dataset.ts`, `src/catalogue/`, `src/timeline/` (5), `src/diagram/` (6), `src/charts/`, `src/report/` (7), `src/theme/` (5, the zone colours), `test/` (5–7).
- `.github/workflows/ci.yml`: the `web` job (2), the release job's upload and the `web` job's fixture step (4).
- `.gitignore`: `web/node_modules/`, `web/*/dist/`, `web/app/public/datasets/` (2).
- Docs (8): `docs/scenario-schema.md` (v1.8), the main spec (§23), `README.md`, `web/README.md`, `CLAUDE.md` §5.

---

## Batch A

### Task 1: `run --days N`

**Files:** `src/session/session.cppm`, `session.cpp`, `src/cli/main.cpp`, `src/report/stream.cpp`; tests as listed above.

```cpp
// A 24 h scenario's day lived `days` times in a row (phase 5b spec §5): the run lasts days × 24 h,
// each rate spike and scripted fault recurs at the same clock time every day, and the report
// observes the last day only (warmup (days − 1) × 24 h), when the queues and connections are as full
// as that day makes them. The engine and the loader are untouched: this transforms the input, so
// `explain` of the result shows exactly what runs. Refused (the message, for the CLI's RunRefusal):
// a duration other than 24 h, a wave whose period does not divide 24 h, a points curve (held flat
// after its last entry: not periodic), a ramp longer than 24 h. days == 1 returns the scenario as is.
[[nodiscard]] std::expected<model::Scenario, std::string> repeat_days(model::Scenario scenario, int days);
```

- [ ] Tests (`test_session.cpp`, scenarios built in code, each with its derivation):
  - **identity**: `repeat_days(s, 1)` equals `s` member for member (duration, warmup, faults, every client's curve);
  - **the copies**: a client with one spike at **12:00** for 10 min and a scenario with one fault at **21:30** for 1 min, `days` 3: the spikes are at **43200, 129600, 216000** s (12 h + k × 86400), the faults at **77400, 163800, 250200** s (21.5 h + k × 86400), each keeping its `duration` and factor; the faults day by day, each day in the file's order (two faults in the file, at 15:00 then 09:00: the result's order is 15:00, 09:00, 39:00, 33:00);
  - **duration and warmup**: `days` 2: duration **172800 s**, warmup **86400 s** whatever the file's warmup (5 min in the test);
  - **waves**: a period of 24 h, 12 h, 8 h is kept (each divides 24 h); 7 d refused with `--days: client 'users' has a wave period of 7d, which does not divide the day`; 5 h refused (24 / 5 is not whole);
  - **refusals**: a duration of 6 h: `--days needs a 24h scenario (this one runs 6h)`; `points`: `--days: client 'users' rate.points is not periodic`; a ramp `over: 25h` refused, `over: 5m` kept; `days` 0 and 8 refused: `--days must be between 1 and 7`;
  - **noise**: a client with noise is kept, its noise unchanged.
- [ ] Tests (`tests/cli/run_days.cmake`, through the binary):
  - `run hello --days 2`: `--days needs a 24h scenario (this one runs 1m)`, exit 1; under `--stream` the same as one `error` line whose `file` is the scenario's path (the scenario, not the command line, is at fault), line and column 0;
  - `--days 2 --duration 48h`: `--days sets the duration; drop --duration`, exit 1; `--days 2 --replications 3`: `--days needs a single run`; `--replications 1` accepted (5a: it is the plain run);
  - `--days 1` on url-shortener: `--report json` and `--stream` byte-identical to the same command without `--days` (the stream except its `run` line, which gains `"days":1`; the test removes the key and compares).
- [ ] Tests (`test_stream.cpp`): the `run` line of a `days` 2 run has `"days":2`, `duration_s` **172800**, `warmup_s` **86400**, and `frames` **1440** with `--every 2m` (172800 / 120); without `--days`, `"days":1`.
- [ ] Tests (`test_scenarios.cpp`, on the shipped files; no 48 h run of a 100 ms scenario, which would
  take minutes in Debug):
  - chat-system after `repeat_days(…, 2)` and `build_graph`: the graph's faults are **2**, at
    **77400** and **163800** s (21:30 on each day); news-feed's users' curve (`RateCurve::at`, no
    noise) at **129600** s (36:00, inside the second spike) equals its value at **43200** s (12:00),
    and both are 3 × the value without spikes at those times (the spike's `factor: 3`);
  - url-shortener (1 s step: 172800 steps, about 2 s in Release *measured* on the trial, the Debug
    time recorded in "As built"; if it passes 30 s the test moves to the precompute's integration
    test) run with `--days 2`'s scenario: the report's `steps` **86400** (the last day at 1 s), and
    the frames 719 and 1439 of a 2 min bucket equal in every column (the trial found them equal: the
    seam the precompute will check).
- [ ] Red commit `tests: a 24h scenario lived for several days repeats its spikes and faults every day`.
- [ ] Implementation: `repeat_days` in `session.cpp`: loops over clients (a `NodeSpec` with a `workload`) and faults, no lambda in a range adaptor (fifth defect). The CLI: `--days` an `int` option on `run` with CLI11's range check replaced by `repeat_days`' message (one wording); `load_for_run` calls it after `config::load` and the seed, before `build_graph`; the refusals of `--duration` and `--replications` beside the existing ones (`--stream`'s list). `render_run_line` takes the day count from `RunArgs` (default 1).
- [ ] Build Release (a new `std::expected<model::Scenario, std::string>` crosses the module boundary: fourth and tenth defects; an `emit_special_members` line in `src/model/model.cpp` if the link asks) and `clang20-libcxx-debug`.
- [ ] Green commit `session, cli: run --days, a day lived several times, for the web app's repeating day`.

**Checkpoint after Batch A** (review, fixes, ASan now: the only C++ of the phase; stop).

---

## Batch B

### Task 2: the `web/` skeleton and its CI job

**Files:** `web/*` (config), `web/stream/src/index.ts`, `web/stream/test/smoke.test.ts`, `.github/workflows/ci.yml`, `.gitignore`.

- [ ] `npm init` the workspaces (`private: true`), dev dependencies at the root, `tsconfig.base.json` (`strict`, `noUncheckedIndexedAccess`, `exactOptionalPropertyTypes`, `verbatimModuleSyntax`, `moduleResolution: bundler`, target ES2022), one `tsconfig.json` per workspace extending it. ESLint flat config: `typescript-eslint` `strictTypeChecked`, `react-hooks`, `no-explicit-any` as an error. Prettier: 100 columns (the C++ code's limit).
- [ ] Root scripts: `check` (`tsc -b --noEmit` + `eslint .` + `prettier --check .`), `test` (`vitest run`), `build` (`npm run build -w @simsys/app`), `serve`, `dev`, `precompute`, `fixtures` (the last four wired in Tasks 4–5).
- [ ] One test, written first: `smoke.test.ts`, `the stream package exports its format version` (`FORMAT_VERSION === 1`), red, then the export: proves the toolchain runs a test. (This task's red/green is that one test.)
- [ ] CI: a `web` job on ubuntu-24.04: `actions/checkout@v4`, `actions/setup-node@v4` (`node-version: 22`, `cache: npm`, `cache-dependency-path: web/package-lock.json`), `npm ci`, `npm run check`, `npm test`, `npm run build`, all with `working-directory: web`. A comment in the workflow says what the job guards, as the other jobs have.
- [ ] Commits: red `tests: the web workspace runs its first test`; green `web: npm workspaces, strict TypeScript, Vitest, and a CI job for them`.

### Task 3: `@simsys/stream`

**Files:** `web/stream/src/*.ts`, `web/stream/test/*.test.ts`. Fixtures for this task: hand-written short streams in the tests (the real fixtures come in Task 4 and replace none of these: these pin the rules, those pin the format).

`messages.ts` mirrors schema v1.7 (and the `dataset` line of spec §6) as TypeScript types, a discriminated union on `type`; `RunLine`, `GraphLine`, `FrameLine`, `SummaryLine`, `ErrorLine`, `DatasetLine`. Rows are `(number | null)[]`. The `summary.report` and graph nodes are typed with the keys the app uses, and an index signature for the rest (contract rule 1).

- [ ] Tests (`lines.test.ts`), `splitLines` as an async generator over a `ReadableStream<string>`:
  - **a line split at every offset rejoins**: the three-line hello prefix fed in two chunks cut at each of its offsets gives the same three lines;
  - **a final line without `\n`** is yielded (the reader decides); empty lines are skipped; `\r\n` ends a line too.
- [ ] Tests (`parse.test.ts`), `parseLine(text) -> StreamMessage | Ignored | ParseError`:
  - an unknown `type` → `Ignored`; an unknown key on a frame is dropped silently;
  - `format_version: 2` on the `run` line → `ParseError` `this page reads format_version 1; the data says 2: rebuild the datasets (npm run precompute)`; `dataset_version: 2` likewise;
  - a line that is not JSON → `ParseError` naming the line number; a frame without `rows` keys it needs (`nodes`) → `ParseError`.
- [ ] Tests (`store.test.ts`), `DatasetStore.consume(source)`:
  - hello's two-frame stream (from the schema's example, inlined): status `ready`; 2 frames; node 1 `mean_rho` column **[0.1, 0.1]**; node 0 (the client, all null) **[NaN, NaN]**; op 0 `p99_ms` **[5.66029, 5.66029]**; `t_s` **[0, 30]**, `dt_s` **[30, 30]**;
  - **a stream without `summary`** ends `truncated` (5a: it was killed); **a partial last line** (no `\n`, not JSON) is dropped and the status is `truncated`, not `error`;
  - **an `error` line** ends `error` with its file, line, column and message (a hello stream cut after `graph`, then an error line at line **12** column **5**);
  - **a frame whose `rev` has no graph** → `error` `frame for graph rev 1, which never came`;
  - subscribers are notified once per batch, not per frame: 720 frames from a memory source give at most **N/64 + 2** notifications (the store batches by 64 frames or by the end of the source; in the browser it batches by animation frame, the batch size here is the test's stand-in, injected);
  - **conservation (theory)**: in a hand-made overloaded stream (a 1k/s node sent 1.5k/s, room 100, from the 5a theory test's numbers: demand **1500**, drain **1000**, dropped **500** once full, backlog **100**), every frame satisfies `demand · dt = (drain + dropped) · dt + backlog − previous backlog` to 1e-5 relative (six significant digits); the helper `bucketBalance(store, row, frame)` is the one the app's tests and Task 4's fixture test reuse.
- [ ] Tests (`file-source.test.ts`, Node's `fetch` mocked with a `Response` over bytes):
  - a gzipped body is inflated (`1f 8b` first); **an already inflated body** (a server that set `Content-Encoding`) is read as text; a body that stops mid-line ends the store `truncated`;
  - a 404 → `error` `datasets/x.ndjson.gz: 404 Not Found`.
- [ ] Red commit `tests: the page reads the simulator's stream by the contract's rules`.
- [ ] Implementation: `FileSource` uses `fetch`, `DecompressionStream`, `TextDecoderStream` (all in Node 22 and every current browser); `MemorySource` yields lines from an array. Comments: `// Pattern: Strategy` on `DatasetSource`, `// Pattern: Observer` on `DatasetStore.subscribe`, the conservation law's comment on `bucketBalance` (CLAUDE.md §2: the fluid balance, why rates are means and the backlog a level).
- [ ] Green commit `stream: the contract in TypeScript, the dataset sources and the columnar store`.

**Checkpoint after Batch B** (review, fixes, stop).

---

## Batch C

### Task 4: `@simsys/precompute`, the fixtures and their CI check

**Files:** `web/precompute/src/*.ts`, `web/precompute/test/*.test.ts`, `web/stream/test/fixtures/*.ndjson.gz`, `web/stream/test/fixtures.test.ts`, `.github/workflows/ci.yml`.

- [ ] Tests (`seam.test.ts`), `seamCheck(graph, day1Last, day2Last, day2Frames)`, synthetic frames:
  - identical frames → `repeats`, `worst.rel` **0**;
  - a backlog of **1000** at 24:00 and **2000** at 48:00 → `drifts`, `worst` naming that node, `backlog`, a **1000**, b **2000**;
  - a backlog of **0.0001** against **0** on a node whose day-2 peak backlog is **5000**: within `1e-6 × 5000 = 0.005` → `repeats` (the absolute term's reason, spec §7.2);
  - a value **1000.4** against **1000** (six-digit rounding: `1e-3 × 1000.4 ≈ 1.0`) → `repeats`; **1002** against **1000** → `drifts`;
  - `null` against `null` → equal; `null` against **0** → `drifts`;
  - op rows and link rows are checked as node rows are (`worst.row` `op` / `link`).
- [ ] Tests (`window.test.ts`): 1440 frames of a `--days 2` run → the last **720**, the first `t_s` **86400**; a `drifts` day keeps all **1440** with `window` **0–172800**, `loops` false; a `run` keeps everything, `window` **0–duration**.
- [ ] Tests (`dataset.test.ts`, `index-file.test.ts`):
  - the dataset line's keys and order as spec §6 shows them; `title` and `chapter` from a README whose first line is `# Chat system (ch. 12)` → **"Chat system"**, **12**; a README without a chapter → `null`;
  - writing the same stream twice gives the same gzip bytes (mtime 0, level 9);
  - `index.json` sorted by `id` whatever order the results arrive in (fed reversed); `bytes` is the gzip file's size; `simsys_commit` is what the injected `git` function returns;
  - **a stream that ends without `summary`**, and one with an `error` line at line **3** column **7**, write no file and reject with `scenarios/x/scenario.yaml:3:7: <message>`; exit 2 with a `summary` is data (`slo_passed` false).
- [ ] Tests (`scenarios.test.ts`): the 14 shipped scenarios are found, sorted; `kind` is `day` for the seven 24 h book scenarios (chat-system, google-drive, news-feed, notification-system, search-autocomplete, url-shortener, youtube) and `run` for the other **7** (web-crawler 6 h, the four labs, the two tutorials); the arguments for a day are `run <file> --stream --days 2 --every 2m`, for a run `run <file> --stream`.
- [ ] Tests (`integration.test.ts`, `describe.skipIf(!process.env.SIMSYS)`): url-shortener through the real binary → `repeats`, **720** frames, `loops` true, and `summary.report` deep-equal to `simsys run <file> --days 2 --report json` parsed.
- [ ] Tests (`web/stream/test/fixtures.test.ts`, on the committed fixtures once written): every fixture parses to `ready`; `bucketBalance` holds in every frame of every node with a queue; url-shortener's dataset has `loops` true, 720 frames, the first `t_s` 86400, and **its first frame's previous backlog taken as its last frame's** still balances (the loop does not break the law, spec §10).
- [ ] Red commit `tests: the precompute keeps a day only if it repeats, and writes the same bytes every time`.
- [ ] Implementation: `run-simsys.ts` spawns with `child_process.spawn`, reads stdout through `splitLines` (Task 3, one splitter for both packages), parses each line with `parseLine`, keeps frames in order; stderr is collected and shown on failure (5a: empty under `--stream`, so non-empty is itself a sign). The pool: at most `--jobs` children, `os.availableParallelism()` by default. `main.ts` prints one line per scenario as it finishes (`book/news-feed  day  repeats  720 frames  142 KB  81.2 s`) and a total. `fixtures.ts`: the precompute on `tutorial/hello`, `tutorial/chain`, `labs/kv-store`, `book/url-shortener` into `web/stream/test/fixtures/`; `--check` writes to a temporary directory and compares the inflated texts with the committed ones, printing the first differing line.
- [ ] Run `npm run fixtures` with the Release binary; commit the four fixtures with the green commit (their sizes *measured* in "As built").
- [ ] CI: the `build-test-release` job gains `actions/upload-artifact@v4` (`name: simsys-release`, `path: build/gcc14-release/simsys`, `retention-days: 1`); the `web` job gains `needs: build-test-release`, `actions/download-artifact@v4`, `chmod +x`, then `SIMSYS=… npm run fixtures -- --check` and `SIMSYS=… npm test -w @simsys/precompute` (runs the integration test).
- [ ] Green commit `precompute: each shipped scenario's repeating day, checked at its seam, as static datasets`.
- [ ] Run the whole precompute once (Release, all 14) and record each dataset's status, size and time in "As built" (*measured*). A `drifts` there is a finding for the user, not a failure: the checkpoint reports it.

**Checkpoint after Batch C** (review, fixes, the precompute's table, stop).

---

## Batch D

### Task 5: the app's shell, the catalogue and time

**Files:** `web/app/index.html`, `vite.config.ts`, `src/main.tsx`, `src/App.tsx`, `src/use-dataset.ts`, `src/catalogue/*`, `src/timeline/*`, `src/theme/*`, `web/app/test/*`.

- [ ] Tests (`clock.test.ts`): for a day dataset frame **0** → `00:00`, **1** → `00:02`, **719** → `23:58` ((t_s − 86400) mod 86400 over 60); for a run, frame at `t_s` **3900** → `1h 05m`.
- [ ] Tests (`player.test.ts`), the time model as a pure reducer (`play`, `pause`, `tick`, `seek`, `step(±1)`, `stepHour(±1)`, `setSpeed`), no timers:
  - a day of 720 frames at the default speed advances **12** frames per second (720 / 60 s);
  - `tick` on frame 719 of a looping day → **0**, still playing; of a day that does not repeat, or a run → stays on the last frame, paused; `play` there → frame **0**;
  - `stepHour(+1)` from frame 700 of a looping day → frame **10** (700 + 30 = 730, wrapped: 30 frames make an hour at 2 min); of a run → the last frame;
  - speeds: 30 s, 60 s, 2 min, 5 min a day → **24, 12, 6, 2.4** frames a second.
- [ ] Tests (`catalogue.test.tsx`, Testing Library): from an `index.json` fixture of three entries the list groups `book`, `labs`, `tutorial`; badges **repeating day**, **does not repeat**, **run · 30 min**; a 404 on `index.json` shows `No datasets yet: run npm run precompute in web/`; choosing an entry sets `location.hash` to `#book/url-shortener`, and a hash on load selects it.
- [ ] Tests (`app.test.tsx`): with a `MemorySource` over the url-shortener fixture the page shows the scenario's title, `00:00`, the play button; a truncated source shows `The dataset ended early: rebuild it with npm run precompute`; a `format_version` 2 dataset shows the parser's message.
- [ ] Red commit `tests: the page plays a repeating day in a loop and a run once`.
- [ ] Implementation: `useDataset(entry)` creates the `FileSource` and the store, aborts on change (`AbortController`); the player runs on `requestAnimationFrame`, accumulating time to the next frame at the chosen speed; the keys of spec §9.3. Zone colours as CSS variables in `theme/` with a light and a dark set, chosen for contrast and for red–green colour blindness (green `#1a9850`-ish / amber / red with a different lightness each), the zone word next to them. A comment on the clock says why a frame is never interpolated (a frame is a bucket: 5a §6.2).
- [ ] Green commit `app: the catalogue, the dataset's loading states and a looping clock`.

### Task 6: the diagram

**Files:** `web/app/src/diagram/{to-flow-elements.ts,layout.ts,Diagram.tsx,NodeCard.tsx}`, tests.

- [ ] Tests (`to-flow-elements.test.ts`, the chain fixture's graph and frames):
  - **3** nodes (users, api, db) and **2** edges (users→api, api→db), in the graph's order;
  - a node row with `zone` 2 gives the class `zone-red` and the label text `red`; zone 0 `green`; a client has no zone, the class `client`, and shows the sum of its op rows' `offered_per_s` at that frame (chain: **100**/s, the users' rate: from the fixture);
  - api's card shows `×2` (replicas **2**) and `mean_rho` as a percentage with no decimals;
  - a bandwidth link (youtube's graph, inlined) gets its pipe row's zone; a plain link gets none;
  - youtube's graph (two datacenters) gives **2** group nodes and every node's `parentId` its datacenter's; a one-datacenter graph gives none.
- [ ] Tests (`layout.test.ts`, elkjs in Node): every node of chain and of youtube gets finite `x`, `y`; clients have the smallest `x` (left to right, clients first); the same graph laid out twice gives the same positions (the cache's premise).
- [ ] Red commit `tests: the diagram is the graph, coloured by the frame's zones`.
- [ ] Implementation: `layout.ts` runs `elkjs` (`elk.bundled.js`, `layered`, `elk.direction: RIGHT`, compound nodes for datacenters), cached per `rev`. `Diagram.tsx` renders `@xyflow/react` with the computed positions, `nodesDraggable` off (positions are the layout's), fit to view. Selecting a node writes `&node=<id>` in the hash. `NodeCard.tsx`'s comment says what the zone is (the worst step of the bucket against the node's yellow and red thresholds: 5a §6.2, spec §3's utilisation zones).
- [ ] Green commit `app: the system's diagram, laid out by ELK and coloured by zone`.

**Checkpoint after Batch D** (review, fixes, a screenshot of url-shortener and chat-system for the user, stop).

---

## Batch E

### Task 7: the zone strip, the charts, the SLO table and the fault blocks

**Files:** `web/app/src/charts/{series.ts,Chart.tsx,NodeCharts.tsx,OpCharts.tsx,ZoneStrip.tsx}`, `src/report/{SloTable.tsx,FaultBlocks.tsx,window-faults.ts}`, tests.

- [ ] Tests (`series.test.ts`), the uPlot inputs as pure functions:
  - utilisation for api (chain): x = the frames' `t_s` rebased to the window, series `mean_rho`, `peak_rho`, and two constant lines at the graph's `yellow` **0.7** and `red` **0.9** (chain's api, as the 5a graph test pins them);
  - backlog with `queue_limit` **1000** as a line; rates labelled in the node's `unit` (`MB` for youtube's `cdn_us`, `units` for url-shortener's `url_db`);
  - the type's own charts only where a column is not all `NaN` (`connections` on chat-system's gateway; none on a service);
  - an op's p99 series is labelled `p99 (estimated)` because the graph lists `p99_ms` in `estimated_columns`.
- [ ] Tests (`zone-strip.test.ts`): the strip's cell grid is nodes × frames from the store's `zone` columns; a client row is absent; the cell under a click at x maps to the frame `floor(x / width × frames)`.
- [ ] Tests (`window-faults.test.ts`): chat-system's two copies (77400, 163800) with the window 86400–172800 → only **163800** is shown, at clock **21:30**; a run shows all its faults.
- [ ] Tests (`slo-table.test.tsx`, `fault-blocks.test.tsx`, from the fixtures' summaries): kv-store's SLO rows with their verdicts, `skipped` shown as such; kv-store's fault block (`node_down store` at **600** s for **180** s, recovered at **780** s: from its report) with during and after; a click on a block seeks the player to the fault's frame.
- [ ] Red commit `tests: the charts, the zone strip and the report show the window the page plays`.
- [ ] Implementation: `Chart.tsx` wraps uPlot in a component (create on mount, `setData` on change, destroy on unmount; a resize observer), with the cursor synced to the player's frame; uPlot is not rendered in jsdom (its inputs are tested above). `ZoneStrip.tsx` draws on a canvas. Each chart's comment says what the series models (CLAUDE.md §2): the backlog a level at the bucket's end, the rates bucket means, `peak_rho` the bucket's worst step, p99 a lognormal fit.
- [ ] Green commit `app: the charts, the zone strip, the SLO table and the fault blocks`.

**Checkpoint after Batch E** (review, fixes, screenshots, stop).

---

## Batch F

### Task 8: docs

- [ ] `docs/scenario-schema.md` **v1.8**: "Command line: `simsys run --days`" (the rules of spec §5, every refusal's message, the report's window, the `run` line's `days` key); "The web app's datasets" (the dataset line, the window, the seam check and its tolerance, `index.json`); "New in v1.8".
- [ ] Main spec **§23**: the rules as built, each review's corrections, in §22's style; the 5a design's diagram (a Node server per browser) noted as superseded by static serving.
- [ ] `README.md`: status (phase 5b); Usage gains `--days`; a "Web app" section: `cd web && npm ci && npm run precompute && npm run serve`, with a screenshot.
- [ ] `web/README.md`: the data path (source → store → views), each part of the model and where it shows (zone, bucket means, p99 estimated), how to add a view, the tests.
- [ ] `CLAUDE.md` §5 as spec §11 words it.
- [ ] Commit `docs: schema v1.8, spec §23, the web app`.

### The 5b review, then the PR

- [ ] A code review of the whole branch against the spec (`code-review` at the level the user chooses), a fix commit.
- [ ] `gcc14-asan` builds and passes; byte identity against `main` of every existing output (`run`, `--report json`, `--stream`, `estimate`, `explain`, `sweep`, `--series`) on all fourteen scenarios: `--days` is new and nothing else changes.
- [ ] CI's clang-format-18 command clean; `npm run check && npm test && npm run build` clean.
- [ ] Ask the user; push; open the PR (body: what, why, the precompute's table, screenshots), CI decides.

---

## Self-review

- Every spec section has a task: §4 (commands) → 2, 4, 5; §5 (`--days`) → 1; §6 (dataset format) → 4; §7 (precompute, seam) → 4; §8 (serving, loading) → 3 (file source), 5 (`serve`); §9.1 → 3; §9.2–9.3 → 5; §9.4 → 6; §9.5–9.6 → 7; §9.7 → 5 (theme), 7; §10 (tests, CI) → every task, CI in 2 and 4; §11 (layout, CLAUDE.md §5) → 2, 8; §12–13 → nothing built.
- Spec corrections made while planning (2026-11-10, in the spec): `repeat_days` returns `std::string` errors (the CLI's `RunRefusal` is not visible to `simsys.session`); the faults' order is day by day, the file's order within a day; the simsys commit moved from the dataset line to `index.json`, and the fixture check compares inflated text, so the fixtures do not change at every commit or with zlib's version.
- Risks: GCC 14.2 and a new `std::expected` of a model type across modules (Task 1, Release build and `emit_special_members`); url-shortener's 48 h run in the Debug test set (Task 1 says where it goes if slow); elkjs and React Flow in jsdom (only the pure layers are tested there; the rendering is checked by screenshots at checkpoints D and E); `DecompressionStream` in Node's test environment (available in Node 22; if Vitest's jsdom lacks it, the file source's tests run in the `node` environment).

---

## As built

Rules as built and every deviation: main spec §23. Here: what was installed and measured.

**Versions** (pinned by `web/package-lock.json`, 2026-11-10): Node 22.23.4, TypeScript 6.0.3,
React 19.3.1, Vite 8.3.7, Vitest 5.1.0, jsdom 31.0.0, Testing Library React 16.3.3, ESLint 10.14.0,
typescript-eslint 8.72.2, eslint-plugin-react-hooks 7.1.1, Prettier 3.9.10, @xyflow/react 12.12.1,
elkjs 0.12.1, uPlot 1.6.32, yaml 2.9.1, tsx 4.23.15.

**Fixtures** (`web/stream/test/fixtures`, gzipped): book-url-shortener 168,160 B (a day of 1440
frames), labs-kv-store 4,483 B, tutorial-chain 3,745 B, tutorial-hello 3,099 B.

**The precompute** (2026-11-11, gcc14-release, 14 jobs, 3 min 2 s wall clock):

| dataset | kind | seam | frames | size | time |
|---|---|---|---|---|---|
| book/chat-system | day | repeats | 1440 | 332 KB | 186 s |
| book/google-drive | day | repeats | 1440 | 281 KB | 47 s |
| book/news-feed | day | repeats | 1440 | 275 KB | 125 s |
| book/notification-system | day | repeats | 1440 | 437 KB | 123 s |
| book/search-autocomplete | day | repeats | 1440 | 210 KB | 23 s |
| book/url-shortener | day | repeats | 1440 | 164 KB | 2.5 s |
| book/youtube | day | repeats | 1440 | 347 KB | 91 s |
| book/web-crawler | run | — | 720 | 19 KB | 14 s |
| labs/consistent-hashing | run | — | 706 | 6 KB | 0.2 s |
| labs/kv-store | run | — | 720 | 4 KB | 0.2 s |
| labs/rate-limiter | run | — | 720 | 5 KB | 0.1 s |
| labs/unique-id | run | — | 667 | 4 KB | 0.1 s |
| tutorial/chain | run | — | 600 | 4 KB | 0.1 s |
| tutorial/hello | run | — | 600 | 3 KB | 0.1 s |

**Deviations from the tasks above** (each in spec §23): day frames of 1 min, not 2 (the tasks'
720-frame and 12-frames-a-second numbers became 1440 and 24); a speed is the window's playing time;
`--days` with `--replications` is allowed; the graph gained `spikes`, `replicas` and `primary`;
`diagram.yaml`; one dark theme; the diagram's refinements after the Batch D checkpoint (categories,
copy stacks, flow dashes, bars, pull links, queue zones), each asked for by the user at the screen.

**Tests at the end of Batch E:** 1821 C++ tests (gcc14-debug), 181 web tests (one more, the
precompute's integration test, runs with `SIMSYS` set).
