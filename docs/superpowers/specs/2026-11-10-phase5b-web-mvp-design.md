# Phase 5b: the watch-only web app

Design for sub-phase 5b of system-sim, drafted on 2026-11-10 after phase 5a (PR #3, the stream,
schema v1.7, spec §22) and the toolchain work of PR #4 (Clang 19/20, libc++) merged into main
(`dbcf013`). 5b is the half of phase 5 a user sees: a page on localhost where one picks a shipped
scenario and **watches its day**: the system's diagram coloured by zone, a looping time slider,
per-node charts, the SLO table and the fault blocks. Where this document conflicts with an earlier
section of the main spec (`2026-10-27-system-sim-design.md`) or with the 5a design, it wins. The
implementation plan (`docs/superpowers/plans/2026-11-10-phase5b-web-mvp.md`) turns it into tasks.
The schema (v1.8) and a new spec §23 record what was finally built.

## 1. Goal

A student opens `http://localhost:4173`, picks *chat-system*, and watches 24 hours go by in a
minute: the gateway tier turns yellow as the evening wave rises, goes red at 21:30 when the scripted
outage takes it down, and the reconnect storm that follows shows on the connection chart and in the
fault block. The day then starts again from midnight, as the same day would repeat in production.

The architecture, agreed on 2026-11-10 (memory: web architecture decisions) and refined from the
5a design's diagram. **Nothing runs at request time.** The browser downloads static files:

```
offline: npm run precompute                                   live: static files only
  simsys run <file> --stream --days 2 --every 2m   ─┐
  (one child process per shipped scenario,          ├─>  web/app/public/datasets/<id>.ndjson.gz
   at most one per core)                            │    web/app/public/datasets/index.json
  the day-2 frames, the repeat check, gzip        ──┘              │  HTTP GET (any static server)
                                                                   v
          browser: DatasetSource (file) ─> DatasetStore ─> diagram · time slider · charts · SLO table
```

5a's diagram had a Node server running `simsys` per browser. The architecture decisions replaced it
with **precompute and static serving**. The output of a shipped scenario is a deterministic function
of its file (spec §3.6), so it is computed once, not once per visitor. A small machine then serves
any number of users at the cost of a file server. The stream format stays the contract: a dataset
is a 5a stream with one line added in front and the frames cut to one day (§6). So the later live
tiers (§12) deliver the same messages through a different source, and the visuals need no change.

After 5b:

- `simsys run <file> --days N` runs a 24 h scenario for N days. Its spikes and faults recur every
  day, and the report observes the last day (§5). It is the one C++ change.
- `npm run precompute` (in `web/`) writes a dataset per shipped scenario and the `index.json` that
  lists them, each day labelled *repeats* or *does not repeat* by a check (§7).
- `npm run serve` builds the app and serves it with the datasets on localhost (§8).
- `web/` holds three npm workspaces: `stream` (the contract in TypeScript), `precompute`, `app`.
  Vitest tests all three, and a CI job runs them (§10).

## 2. Decisions taken (2026-11-10, for review)

| Question | Decision | Why |
|---|---|---|
| What a dataset holds | A shipped 24 h scenario: its **repeating day**, the second day of a 48 h run, which loops. Every other scenario: its own run (the labs, the tutorials, web-crawler's 6 h), played once and replayed from the start | Day 1 starts from empty queues and no open connections. A day that has reached its periodic steady state is what a production system lives in. The trial of §7 found day 2's last frame equal to day 1's on url-shortener and news-feed |
| How day 2 gets its spikes and faults | **A `--days N` flag in `simsys run`** (C++, §5). It repeats every rate spike and scripted fault each day at the same clock time, and has the report observe the last day | A scripted fault and a spike happen once: in a plain `--duration 48h` run, chat-system's 21:30 outage and notification-system's 09:00 blast fall in day 1 only, so the looped day would lose them. Rewriting the YAML in TypeScript would mean a second parser for the time syntax (`21:30`, `90m`) and errors that point into a generated file. The flag reuses the loader's rules and is useful from the CLI too: "the steady day's SLO verdict". Alternative, for review: the precompute rewrites the YAML (no C++ in 5b) |
| What is checked before a day loops | The **seam check**: the frame ending at 24:00 and the one ending at 48:00 agree in every column, within a tolerance (§7.2). A day that fails is labelled *does not repeat*, shown whole (48 h) and not looped | Deterministic engine + an input that repeats daily + the same state at two midnights ⇒ every later day is the same day (§7.1). A day that drifts (a backlog that grows every day) is a finding to show, not to hide behind a loop |
| Where the data lives | `web/app/public/datasets/`, **not committed** (gitignored, regenerated by `npm run precompute`). Small streams are committed as test fixtures, and CI checks that they are still what `simsys` writes (§10) | A dataset changes whenever a model changes. Committed gzip files would grow the history at every engine fix and could go stale silently. Results do not depend on the toolchain (CLAUDE.md §3), so anyone with a Release build regenerates the same bytes |
| Compression | gzip, level 9, written by Node's `zlib`. The client inflates with the browser's `DecompressionStream`, and recognises an already inflated body by its first bytes (§8) | About 6× (measured: news-feed's 48 h stream 1.63 MB → 289 KB), so a day is about 100–150 KB on the wire. Any static server works, with no `Content-Encoding` setup: localhost, nginx, S3 later |
| Serving | Static: `vite preview` on localhost for 5b. Any file server later | The architecture decision: CPU cost over flexibility. No backend process to size, secure or keep alive |
| Frontend stack | React + Vite + TypeScript (strict). React Flow (`@xyflow/react`) and ELK.js (`elkjs`, layered layout) for the diagram, uPlot for the charts, no state library | Agreed 2026-11-09 (Cytoscape.js and Angular weighed then). uPlot draws thousands of points on a canvas in a few milliseconds. The store is small enough for `useSyncExternalStore` |
| One way in for data | Every view reads a `DatasetStore`, which a `DatasetSource` fills (§9.1). 5b has a file source and an in-memory one (tests). Later a live stream source and a WebAssembly one | The design rule of the architecture decisions: a later tier only adds a source |
| Packages | npm workspaces in `web/`: `@simsys/stream`, `@simsys/precompute`, `@simsys/app` | The stream's parser and types are used by both the precompute (the seam check reads frames) and the app. One definition of the contract in TypeScript |
| Versions | Node 22 LTS (installed: v22.23.4). Current majors at install time, pinned by `package-lock.json` | Reproducible installs; `npm ci` in CI |
| Conventions for the TypeScript | CLAUDE.md gains a §5 for `web/` (§11): strict TS, ESLint, Prettier; §2 (the model explained), §4 (`Pattern:` comments, TDD, tests named after properties) apply as they do to C++. A non-obvious TypeScript or React idiom gets a one-line comment | The project teaches system design. The C++ first-use rule (§1) is about learning C++ and is not carried over |

## 3. Sub-steps and order

Each sub-step is one task in the plan: a red commit with the tests, a green commit, a review, a fix
round (4a's and 5a's rhythm), with a checkpoint after each.

| Step | Content | Size | Files mostly touched |
|---|---|---|---|
| 5b.1 | `run --days N` (C++): repeat the day's spikes and faults, observe the last day, the refusals; ASan | medium (a 5a task) | `session/session.cpp`, `.cppm`, `cli/main.cpp`, tests |
| 5b.2 | `web/` skeleton: workspaces, TS strict, ESLint, Prettier, Vitest, the CI job green on one test | small | `web/*`, `.github/workflows/ci.yml` |
| 5b.3 | `@simsys/stream`: message types, the line splitter, parsing and the contract's rules, `DatasetSource` (memory, file), `DatasetStore` | medium | `web/stream/` |
| 5b.4 | `@simsys/precompute`: run `simsys`, cut the day, the seam check, the dataset line, gzip, `index.json`; the fixtures and their CI check | medium | `web/precompute/`, `ci.yml` |
| 5b.5 | the app's shell: the catalogue, loading and error states, the time model (loop, play, speed, keys) | medium | `web/app/src/{catalogue,timeline}` |
| 5b.6 | the diagram: graph → elements, ELK layout, datacenter groups, zone colours, selection | medium | `web/app/src/diagram` |
| 5b.7 | the charts (uPlot), the zone strip, the SLO table, the fault blocks | medium–large | `web/app/src/{charts,report}` |
| 5b.8 | docs: schema v1.8 (`--days`, the dataset format), spec §23, README, `web/README.md`, CLAUDE.md §5; the 5b review; PR | small | `docs/`, `README.md`, `CLAUDE.md` |

## 4. Command line and scripts

```
simsys run <file> --days N [--stream [--points N | --every <d>]] [--report table|json] [--seed N]

cd web
npm ci
npm run precompute -- [--simsys ../build/gcc14-release/simsys] [--jobs N] [--only book/youtube]
npm run serve              # vite build + vite preview: http://localhost:4173
npm run dev                # vite dev server with hot reload, same datasets
npm test                   # vitest, every workspace
npm run check              # tsc --noEmit, eslint, prettier --check (what CI runs)
npm run fixtures [-- --check]   # rewrite the committed fixture streams, or diff against them
```

## 5. `simsys run --days N` (C++, 5b.1)

A day of a 24 h scenario is the unit the book reasons in ("peak QPS", the daily wave). `--days N`
runs that day N times in a row, as the same system would live through them:

- **Needs a 24 h scenario**: `duration: 24h` exactly, else `--days needs a 24h scenario
  (web-crawler runs 6h)`. N is 1 to 7. `--days 1` is the plain run, byte for byte.
- **Refused with `--duration`** (`--days sets the duration; drop --duration`) and with
  `--replications` above 1 (as `--series` is). Combines with `--stream`, `--report`, `--seed`.
- **A periodic input.** The run lasts N × 24 h. Each `rate.spikes` entry and each scripted fault
  recurs at `at + k · 24h`, for k = 1 … N − 1. A `wave` repeats by itself if its `period` divides
  24 h; any other period is refused (`--days: users' wave has a 7d period, the day does not
  repeat`). A `points` curve is held flat after its last entry, so it is not periodic: refused. A
  `ramp` is allowed if its `over` is at most 24 h: it ends during day 1, which is a warm-up anyway.
  `noise` is allowed: each day draws its own noise, as days do. No shipped 24 h scenario has noise
  today (see the seam check's note, §7.2).
- **The report observes the last day.** The warmup becomes (N − 1) × 24 h, the scenario's own
  warmup replaced: by then the queues and connections are as full as they get on that day.
  windowed SLOs (`over: 1h`) are then aligned with the clock from midnight. Fault blocks are judged
  in the warmup too (`Summary::observe`, phase 3), so every day's copy of a fault has a block. The
  app shows the looped day's (§9.6).
- **The stream** says so: the `run` line gains `"days": N` (an added key: `format_version` stays
  1). `duration_s` and `warmup_s` are the run's (172800 and 86400 for two days). The `graph`'s
  `faults` list every copy, in time order.
- **Where.** A pure function `session::repeat_days(model::Scenario, int days) ->
  std::expected<model::Scenario, RunRefusal>` makes the N-day scenario before the graph is built.
  The engine and the loader are untouched, and `explain` of the result shows what will run. It is
  a plain transformation of the input, not a named pattern, and its comment says so.

GCC 14.2 precautions (CLAUDE.md §3): the code goes in `session.cpp`, not in an interface. No
lambda in a range adaptor (5th defect). Release is built after the task, because a new
`std::expected<Scenario, ...>` crosses a module boundary (4th and 10th defects: an
`emit_special_members` line if the link asks for one).

## 6. The dataset format

A dataset is a 5a stream (schema, "simsys run --stream"), one JSON object per line, gzipped, with:

1. a first line of a new type, `dataset`, which a 5a reader ignores (contract rule 1);
2. then the stream's `run` and `graph` lines unchanged;
3. then the frames of the **kept window** only, with their `t_s` unchanged (absolute run time);
4. then the `summary` unchanged.

```json
{"type":"dataset","dataset_version":1,"id":"book/chat-system","title":"Chat system","chapter":12,
 "kind":"day","window":{"from_s":86400,"to_s":172800},"loops":true,
 "seam":{"status":"repeats","worst":{"row":"node","id":"chat_servers","column":"connections","a":181234,"b":181234,"rel":0}},
 "source":{"scenario":"scenarios/book/chat-system/scenario.yaml","args":["run","…","--stream","--days","2","--every","2m"],"simsys_commit":"dbcf013"}}
```

| key | meaning |
|---|---|
| `dataset_version` | the dataset line's own version (1). The other lines keep the stream's `format_version` |
| `id` | the scenario's path under `scenarios/` without `/scenario.yaml`. The file is `<id with / → ->.ndjson.gz` |
| `title`, `chapter` | from the scenario's README heading (book scenarios carry their chapter); `null` if absent |
| `kind` | `day` (a 24 h scenario, run with `--days 2`) or `run` (any other duration, run as it is) |
| `window` | the run time the frames cover: day 2 (`86400`–`172800`) for a day that repeats; the whole run otherwise |
| `loops` | `true` only for a day whose seam check passed |
| `seam` | for a `day`: `status` (`repeats` or `drifts`) and the worst column (§7.2); `null` for a `run` |
| `source` | how to regenerate it: the file, the exact arguments, the simsys commit (`git rev-parse --short HEAD`, `dirty` appended for an unclean tree) |

**Buckets.** A day is written with `--every 2m`: 720 frames a day, the 5a default for 24 h, so a
48 h run gives 1440 and the dataset keeps the second 720. A `run` keeps `--points 720`.

**Size (measured on the 48 h trial, 2026-11-10).** url-shortener's 48 h stream is 1.12 MB, 175 KB
gzipped; news-feed's 1.63 MB, 289 KB. A one-day dataset is about half of that: roughly 90–150 KB
to download per scenario. The plan records every dataset's size in `index.json` (`bytes`).

**`index.json`**, written last, sorted by `id` so its bytes do not depend on which run finished
first:

```json
{"index_version":1,"format_version":1,"datasets":[
  {"id":"book/chat-system","file":"book-chat-system.ndjson.gz","bytes":131072,"title":"Chat system",
   "chapter":12,"kind":"day","loops":true,"seam":"repeats","frames":720,"slo_passed":true}, …]}
```

No time stamps in either file: the same commit gives the same bytes (Node's gzip writes mtime 0),
so CI can compare them byte for byte.

**Why not cut the frames in C++** (`--stream --from 24h`): the precompute needs day 1's last frame
for the seam check, and a cut is one line of TypeScript. The stream stays the full run.

## 7. The precompute (`@simsys/precompute`, 5b.4)

For each `scenarios/**/scenario.yaml`, at most `--jobs` (default `os.availableParallelism()`) at a
time:

1. Read `duration` from the scenario's YAML (the only key it reads; the `yaml` package). 24 h →
   `day`, else `run`. The YAML is read for this one decision, never rewritten.
2. Spawn `simsys run <file> --stream` (+ `--days 2 --every 2m` for a day). Read the stdout line by
   line with `@simsys/stream`'s splitter. Exit 0 and 2 are data (2: an SLO failed, which the
   app shows). An `error` line, exit 1, or a stream that ends without a `summary` stop the
   precompute with the file, line and column: a broken dataset is never written.
3. For a day: the seam check (§7.2) on frames 719 and 1439, the window, `loops`.
4. Write the `dataset` line and the kept lines, gzip, then `index.json`.

**Cost.** One core per scenario: about 2× a 24 h run (news-feed's 48 h: 80 s Release; chat-system
about 2 min, youtube about 50 s; url-shortener 2 s). Every shipped scenario on a 4-core machine:
about 3 minutes. Offline, once per commit that changes a model.

### 7.1 Why one seam is enough

The engine is deterministic: the state at the start of a step and the input of that step decide the
next state (spec §3.6). With `--days`, the input (the curves, spikes, faults) is the same every
day. So if the state at 48:00 equals the state at 24:00, day 3 replays day 2 exactly, and so does
every later day: day 2 *is* the periodic steady state, and looping it is faithful.

Two limits. A frame is not the whole engine state: it shows backlogs, connections, lags and rates,
but not, say, a cache's warm-up counter. And its numbers have six significant digits. So the check
is a necessary condition on what a reader can see. That is enough for a watch-only page, and it is
what the user would notice at the loop's seam. 5c's settle detection (§12) will compare the engine
state itself.

### 7.2 The check

For the frame ending at 24:00 (`a`, day 1's last) and the one ending at 48:00 (`b`, day 2's last),
every node, link and op column:

- both `null`, or
- `|a − b| ≤ 1e-3 · max(|a|, |b|) + 1e-6 · M`, where M is the column's largest absolute value over
  day 2 for that row. The relative term absorbs the six-digit rounding. The absolute term keeps a
  backlog of 0.0001 against 0 from counting as drift on a node that queues thousands.

Any column outside → `drifts`, and `seam.worst` names the worst one (largest `|a − b|` relative to
the tolerance), so the app can say *"transcode_queue's backlog is 1.2M at 48:00 against 0.6M at
24:00: the queue grows every day"*. Expected: youtube with too few transcoding workers, and any
system run past its capacity. The 2026-11-10 trial found url-shortener and news-feed identical to
the last digit.

Noise: a day with noise is a new sample of the noise every day, and its rates differ at the seam
however steady the system is. No shipped 24 h scenario has noise. If one ships, the check compares
its levels only (`backlog`, `connections`, `queue_lag_s`) at a 5 % tolerance, and the plan adds the
test then.

## 8. Serving and loading

`npm run serve` builds the app (`vite build`) into `web/app/dist`. Vite copies `public/datasets`
into it. `vite preview` serves the result on `localhost:4173`. Nothing else runs. Hosting later is
the same `dist/` on a file server.

The file source fetches `datasets/<file>` and reads its body as a stream: it inflates with
`DecompressionStream('gzip')`, decodes UTF-8 (`TextDecoderStream`), and splits it into lines. The
page fills while the file downloads, as it would from a live stream. A server that sets
`Content-Encoding: gzip` on `.gz` files makes the browser inflate the body itself. The source
recognises that case by the body's first two bytes (not `1f 8b`) and skips its own inflating. A
truncated download ends without a `summary` and is reported as such (5a: "a stream that ends
without a `summary` or an `error` was killed").

## 9. The app (`@simsys/app`)

### 9.1 The data path

```ts
// Pattern: Strategy — where the messages come from. 5b: a static file, an in-memory list (tests);
// later a live stream (SSE) or simsys compiled to WebAssembly in a worker. The views never see it.
interface DatasetSource {
  readonly label: string;
  messages(signal: AbortSignal): AsyncIterable<StreamMessage>;
}
```

`StreamMessage` is the union of the stream's line types (`dataset`, `run`, `graph`, `frame`,
`summary`, `error`) in `@simsys/stream`, with a parser that applies the contract (schema, "The
output contract"): it ignores an unknown `type` and unknown keys; it refuses a `format_version` or
`dataset_version` other than 1 with a message naming both; it refuses a frame whose `rev` has no
`graph`.

`DatasetStore` consumes a source and holds what the views read:

- the dataset line, the `run`, the graphs by `rev`, the summary, and a status: `loading`, `ready`,
  `error` (with the `error` line's file, line, column and message), `truncated`;
- the frames **as columns**: per node, link and op row, per column, one `Float64Array` over the
  frames, `NaN` for `null`, plus the frames' `t_s` and `dt_s`. uPlot draws these arrays directly,
  and the diagram reads column[i] at the slider's frame. A frame appends to every column, so loading
  a day is linear in its size.

Pattern: Observer: the store notifies its subscribers once per batch of frames (one animation
frame), and React reads it through `useSyncExternalStore`, React 18's hook for an outside store.

### 9.2 The catalogue

`index.json` lists the datasets, grouped as the `scenarios/` folders are (book, labs, tutorial),
with title, chapter, and a badge: **repeating day**, **does not repeat** (and the seam's sentence),
or **run** (its length). The choice is kept in the URL (`#book/chat-system`), so a link opens a
scenario. If `index.json` is missing, the page says to run `npm run precompute` (the first-run case
on a fresh clone).

### 9.3 Time

The slider's position is a frame index, shown as the clock: `HH:MM` = (`t_s` − `window.from_s`)
mod 24 h for a day, the run time `1h 05m` for a run.

- **Play** advances one frame per tick. A day takes 60 s at the default speed (12 frames a second).
  Other speeds: 30 s, 2 min, 5 min a day. A frame is a bucket, so nothing is interpolated: the page
  shows the bucket the clock is in.
- **Loop**: a day with `loops` wraps from 23:58 to 00:00 without a pause. A day that does not repeat
  and a run stop at the end; play again starts over.
- Scrubbing, ← / → one frame, Shift+← / → one hour, Space play/pause, Home midnight.
- Marks on the slider: the scripted faults of the window (`graph.faults`, at their `at_s`, their
  `for_s` as a band) and, for a run, the warmup shaded (5a: frames include it).

### 9.4 The diagram

From the current graph (the latest `rev`; 5b has only rev 0):

- a React Flow node per graph node: its id, its type, `×replicas` (and shards), the current frame's
  `mean_rho` as a percentage. Its colour is the frame's `zone` (worst of the bucket: 5a §6.2). The
  zone is also written as a word, so colour is never the only signal. A client is grey and shows
  its offered rate (the sum of its op rows) instead.
- an edge per link, from → to. A bandwidth link (`pipe_row` not null) is coloured by its own zone,
  and a hover shows MB/s and the link's latency. The other links are neutral.
- datacenters as ELK compound nodes (group boxes) when there are two or more.
- **layout by ELK** (`layered`, left to right, clients first). It is computed once per graph rev,
  asynchronously, and cached. Nodes keep their places through the day, so the eye follows colour,
  not motion.

Clicking a node selects it for the charts (§9.5). The selection is kept in the URL too.

The mapping graph → elements is a pure function (`toFlowElements(graph, frameIndex, store)`), tested
without a browser.

### 9.5 Charts and the zone strip

- **The zone strip**, under the slider: one row per node, one cell per frame, coloured by zone, on
  a canvas. The whole day's health at a glance ("red at 21:30 for two buckets, then yellow for half
  an hour"). Clicking a row selects the node, and clicking a cell moves the slider there.
- **Per selected node**, uPlot charts sharing the day's time axis, with a cursor at the slider's
  time:
  - utilisation: `mean_rho` and `peak_rho`, with the node's `yellow` and `red` thresholds as lines;
  - backlog: `backlog` and `peak_backlog`, with `queue_limit` (the room) as a line;
  - rates: `demand_per_s`, `drain_per_s`, `dropped_per_s`, `rejected_per_s`, in the node's `unit`
    (5a: `MB` for a byte-sized node, `units` for a costed one);
  - latency: `latency_ms`;
  - the type's own, when not null: `connections`, `queue_lag_s`, `dead_lettered_per_s`,
    `deduplicated_per_s`.
- **A selected client** shows its ops: `offered`, `answered`, `timed_out` per second, and
  `mean_ms` / `p99_ms`. p99 is labelled *estimated* (the graph's `estimated_columns`).
- **A selected bandwidth link** shows the link columns in MB.

Each chart's comment names what it models (CLAUDE.md §2): e.g. why the backlog is a level and the
rates are means (the conservation law over a bucket, 5a §6.2).

### 9.6 The SLO table and the fault blocks

From `summary.report`, the report of the observed window (for a day: day 2, §5):

- the SLO table: metric, node/op, the bound, the measured value, pass / fail. `skipped` is shown as
  such. A windowed SLO names its worst window as a clock time.
- the fault blocks of the faults inside the window: at, kind, target, for; during (offered, served,
  lost and its unit, peak backlog, flipped SLOs) and after (backlog drained in, cache warm in,
  recovered at). Clicking a block moves the slider to the fault.

A day's report also judges day 1's copies (§5). The app filters by `at_s` within `window`, and its
test says why.

### 9.7 Look

A plain, dense layout: catalogue left, diagram centre, charts and report right/below, the slider and
zone strip across the bottom. Light and dark themes from the system setting, with zone colours
checked for contrast in both and for red–green colour blindness (the zone word, §9.4, is the
fallback). It is responsive enough to read on a laptop. A phone layout is not a 5b goal.

## 10. Tests (written first) and CI

**`@simsys/stream` (5b.3).**
- the splitter: lines split across chunks at every byte offset rejoin, a final line without `\n`
  is kept only if it parses (a killed stream's partial line is dropped), UTF-8 split across chunks;
- the contract: an unknown `type` and unknown keys are ignored; `format_version: 2` is refused with
  its message; a frame with an unknown `rev` is refused;
- the store: hello's stream (fixture) gives 2 frames whose columns equal the rows, `null` → `NaN`;
  a stream without `summary` ends `truncated`; an `error` line ends `error` with its line and
  column;
- **theory**: on every fixture, every frame satisfies the bucket's conservation law, `demand_per_s ·
  dt_s = (drain_per_s + dropped_per_s) · dt_s + backlog − previous backlog`, within the six-digit
  rounding. For a looping day the first frame's previous backlog is the last frame's: the seam
  check makes that true, and the test proves the loop does not break the law.

**`@simsys/precompute` (5b.4).**
- the seam check on synthetic frames: identical → `repeats`; a backlog that grows → `drifts` naming
  that node and column; a 0.0001 backlog against 0 on a node with a peak of 5000 → `repeats`;
  `null` against a number → `drifts`;
- the window: 1440 frames → the last 720, `t_s` from 86400; a `run` keeps everything;
- the dataset line and `index.json`: keys, sorted ids, the same bytes twice (no time stamps, gzip
  mtime 0), whatever order the runs finish in;
- a stream that ends without a `summary`, or with an `error`, writes nothing and fails with the
  message;
- integration (needs `SIMSYS`, skipped without it): url-shortener's real 48 h run → `repeats`, 720
  frames, `summary.report` equal to `simsys run --days 2 --report json`.

**`@simsys/app` (5b.5–5b.7)**, Vitest with jsdom and Testing Library; uPlot and React Flow's canvas
parts are not rendered in jsdom, and their inputs are pure functions tested instead:
- the clock: frame 0 of a day is 00:00, 719 is 23:58; a run's clock is its run time;
- the loop: play past the last frame of a looping day wraps to 0; past the last frame of a day that
  does not repeat, it stops;
- `toFlowElements`: chain's graph gives 3 nodes and 2 edges in the graph's order; a node in zone 2
  is red and says "red"; a client shows its op rows' offered sum; youtube's two datacenters give two
  groups;
- the ELK layout gives every node a position, and the same positions twice (elkjs runs in Node);
- the chart series builders: the columns, the threshold lines from the graph, p99 marked
  estimated;
- the SLO table and the fault blocks from a fixture summary; day 1's copies of a fault left out;
- the catalogue: badges from `index.json`; a missing `index.json` shows the precompute hint.

**C++ (5b.1)**, in `tests/` as usual: `--days 1` byte-identical to the plain run (`--report json`,
`--stream`); `--days 2` on news-feed has its spike at 12:00 on both days (the users' offered rate in
the frame at 36:00 equals the one at 12:00) and chat-system's outage on both days (`graph.faults`
has two, 24 h apart); the report observes 86400 s; every refusal of §5 with its message; the same
seed twice gives the same bytes.

**CI.** A job `web` on ubuntu-24.04: `actions/setup-node` (Node 22, npm cache keyed on
`web/package-lock.json`), `npm ci`, `npm run check`, `npm test`, `npm run build`. Then, with the
`simsys` binary the `build-test-release` job uploads as an artifact (`needs: build-test-release`),
`npm run fixtures -- --check` and the precompute's integration test. The C++ jobs are unchanged
except for the release job's upload step. The format job's command stays C++ only; Prettier is in
`npm run check`.

**Fixtures** (committed in `web/stream/test/fixtures/`, written by `npm run fixtures`): hello's and
chain's streams, kv-store's (a fault and its block), and url-shortener's day dataset (gzipped,
about 90 KB) as the one real day. CI's `--check` regenerates them and fails on any byte of
difference, so a change of the C++ output cannot leave the web code testing an old contract.

## 11. Repository layout and conventions

```
web/
  package.json            workspaces, the scripts of §4
  tsconfig.base.json      strict, noUncheckedIndexedAccess, exactOptionalPropertyTypes
  eslint.config.js        typescript-eslint strict + react-hooks
  .prettierrc
  stream/   src/{messages,lines,parse,source,file-source,memory-source,store}.ts   test/ (fixtures/)
  precompute/ src/{main,scenarios,run-simsys,seam,window,dataset,index-file}.ts     test/
  app/      index.html  vite.config.ts
            src/{main.tsx,App.tsx, catalogue/, timeline/, diagram/, charts/, report/, theme/}
            public/datasets/   (gitignored: written by npm run precompute)
  README.md               how to run it, the data path, where each part of the model shows
```

The root `.gitignore` gains `web/node_modules/`, `web/*/dist/`, `web/app/public/datasets/`.

**CLAUDE.md §5 (new), for `web/`:** TypeScript strict, no `any`, no non-null `!` without a comment.
ESLint and Prettier clean (`npm run check`). §2 applies: what a view shows from the model (a zone,
a bucket mean, an estimated p99) is explained where it is drawn. §4 applies: `Pattern:` comments,
tests first, tests named after the property they prove. A React or TypeScript idiom that a C++
reader would not guess (a hook's dependency list, a discriminated union's narrowing) gets a
one-line comment on first use in a file.

## 12. Later (not in 5b)

**5c: the change menu.** A fixed menu of disturbances (node down, ±1 replica, capacity ×½, traffic
×2/×5/×10, at whole hours, for 1/5/30 minutes). Each one's effect is deterministic, so it is
precomputed once as a **delta dataset**: the frames from the change until it settles, plus its
outcome (recovered, new steady state, collapsing). Every user shares it. The client composes
baseline loop + delta in a `ComposedSource` (the §9.1 interface again), so the views are unchanged.
The C++ side: engine snapshots, so a delta computes only its transient from day 2's state at the
change; settle and collapse detection against the fault-free twin (phase 3), which replaces the seam
check's frame comparison with the engine state's; the menu as input. `estimate` stays the instant
answer for a lasting change's new equilibrium. The stream's reserved features (5a §8: commands on
stdin, `rev` + 1 on a topology change) are where a live variant of the menu would come in.

**User-built systems, with WebAssembly.** A user's own system (a few instances per node) cannot be
precomputed. The preferred route: `simsys` compiled to WebAssembly with Emscripten (the reason for
PR #4's libc++ work; still to spike: CMake + Emscripten + named modules), run in a Web Worker in
the user's browser, its stdout lines posted to the page: a `WorkerSource` behind the same
interface, at no server CPU. Limits that keep it interactive: about 20 nodes, a 24 h day at a 1 s
step, `explain` and `estimate` first for instant feedback. The fallback is a server tier: compute on
first request, cache by a hash of the normalised YAML, a queue capped at the core count, delivery
over SSE from `run --stream`, a `LiveStreamSource` behind the same interface.

## 13. What 5b deliberately leaves

Any server process, SSE or WebSocket (static files only). Editing a system, the change menu and live
faults (5c and later). Spikes as marks on the slider (they are not in the `graph`; the op rows show
their effect). Comparing two scenarios or two seeds side by side. A phone layout. Hosting on AWS
(the `dist/` is ready for it; the deployment is its own small step). Streaming replications or
sweeps to the page. Moving the seam check into C++ (5c's settle detection replaces it).
