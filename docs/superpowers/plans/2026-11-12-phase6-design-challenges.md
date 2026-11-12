# Phase 6: Design Challenges — Implementation Plan

> **For agentic workers:** this plan is executed task by task by subagents. The orchestrating
> session writes each task's brief from this plan, reviews the result and runs the checkpoints; it
> writes no implementation code itself. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** a student picks a challenge (a given load, SLOs, a price list of parts in three sizes),
writes a design (nodes and links) in a YAML pane, sees its diagram and its bill in dollars a month
as they type, runs its day, and gets a verdict: passes at a cost against par, or fails at a named
first bottleneck. Four challenges and an open build ship. The terminal can play too:
`simsys run design.yaml --challenge challenge.yaml`.

**Architecture:** the spec `docs/superpowers/specs/2026-11-12-phase6-design-challenges-design.md`
decides everything below; this plan turns it into tasks.

- C++ (Tasks 1–3). A new module `simsys.challenge` (after `simsys.model`, before `simsys.config`)
  holds the types (`Challenge`, `Part`, `Size`, `Bill`) and the pure cost function. `simsys.config`
  gains `load_challenge` and `load_design`: the design and the challenge are composed **as YAML**
  (the part's keys written into the design node's map, `part`/`size` replaced by `type`, the given
  nodes and SLOs added), and the composed root goes through the loader's existing pipeline.
  `simsys.report` renders the bill (table, JSON, the stream's `graph` and `summary`). `cli/main.cpp`
  gains the `challenge` subcommand and `--challenge` on `validate`, `explain`, `estimate`, `run`.
- Web (Tasks 5–8). A fourth workspace `@simsys/server`: a Node `http` request handler mounted into
  Vite's dev and preview servers by a plugin. `@simsys/stream` gains `LiveSource`. `@simsys/app`
  gains the *Design* mode: `src/design/` (the challenge list, the brief, the draft store, the
  explain loop, the bill), `src/editor/` (CodeMirror 6), `src/verdict/`.
- Content (Task 4): `challenges/00-open-build`, `01-one-tier`, `02-daily-peak`, `03-read-heavy`,
  `04-url-shortener`.

**Tech stack:** as 5b (C++23 / GCC 14.2 named modules with CLAUDE.md §3's defects; Node 22 loaded
with `source ~/.nvm/nvm.sh`; TypeScript strict; Vite, React, Vitest). New web dependencies:
`codemirror`, `@codemirror/lang-yaml`, `@codemirror/lint` (the editor), `react-markdown` (the
brief). Nothing else.

**Workflow (the user's standing rules):**

- Branch `phase6/design-challenges`, cut from `main` at `3f51c87`. The spec and this plan are its
  first commit.
- **Implementation is delegated** (2026-11-11): each task goes to a subagent, `opus` for the C++
  tasks and the verdict, `sonnet` for the rest. Its brief carries the task's section of this plan,
  the spec sections it names, the CLAUDE.md rules that bind it, and what to report (the commits,
  the test counts, anything the plan had wrong).
- Each task: a **red commit** (tests failing for the stated reason), a **green commit** (the
  minimum that passes), a refactor if due. Commit messages say *why*. Commits:
  `git -c user.name=Claude -c user.email=noreply@anthropic.com commit`, ending with the
  `Co-Authored-By` line.
- After each C++ green commit: `ctest --preset gcc14-debug` whole, `cmake --build --preset
  gcc14-release`, CI's clang-format-18 command. After each web green commit: `npm run check && npm
  test` in `web/`.
- After each batch: a review of the batch's diff by the orchestrating session, a fix commit, then a
  **checkpoint**: stop, report, say how big the next batch is, wait for the go-ahead.
- Before the push: `gcc14-asan` and `clang20-libcxx-debug` build and pass; `npm run check && npm
  test && npm run build`. Ask the user before `git push`; open a PR and let CI decide.

| Batch | Tasks | Size |
|---|---|---|
| A | 1 (the challenge file, `simsys challenge`), 2 (`--challenge`: compose and refuse) | large: the loader's first second file; many refusals, each a test |
| B | 3 (cost and the bill in every output) | medium: one pure function, four renderers |
| C | 4 (the four challenges and the open build, their tests) | medium: content, arithmetic, Release runs; the prices are looked up |
| D | 5 (`@simsys/server`), 6 (`LiveSource`) | medium: child processes behind HTTP, all testable with a fake binary |
| E | 7 (the *Design* mode: list, brief, editor, live diagram, bill) | medium–large: the editor and the explain loop |
| F | 8 (run, cancel, verdict, estimate at peak) | medium: one pure function with many cases, three small views |
| G | 9 (docs), the branch review, fix round, ASan, push, PR | medium |

---

## Batch A

### Task 1: the challenge file and `simsys challenge`  (opus)

**Files:** new `src/challenge/challenge.cppm` (types only in this task), `src/config/config.cppm`
(the declaration), new `src/config/challenge.cpp`, `src/report/` (the challenge's table and JSON),
`src/cli/main.cpp`, `CMakeLists.txt`; tests `tests/unit/test_challenge.cpp` (new),
`tests/cli/challenge.cmake` (new).

```cpp
// simsys.challenge — Pattern: none; plain data. A design challenge (phase 6 spec §4): what the
// designer is given (the load, the SLOs) and what they can buy (the parts).
struct Size  { std::string name; double price_usd_month; };
struct Part  { std::string name; model::NodeType type; std::vector<Size> sizes;   // empty: one size
               double price_usd_month{}; std::optional<double> price_per_gb{}; std::optional<double> key_bytes{}; };
struct Challenge { std::string name, title; int level{}; std::optional<int> chapter{}; bool open{};
                   std::optional<double> par_usd_month{}; SimTime peak_at{}; std::vector<Part> parts; };

// simsys.config
[[nodiscard]] std::expected<challenge::Challenge, ConfigError> load_challenge(const std::string& path);
```

`load_challenge` checks the file's own keys (`version`, `name`, `title`, `level`, `chapter`,
`duration`, `step`, `warmup`, `seed`, `peak_at`, `open`, `given`, `parts`, `par`), then proves that
every part is usable by composing a **probe scenario** per part and size (the given nodes, plus one
node made of the part's keys with the size's on top) and running the loader's node checks on it,
so `capacity: fast` in a part is reported at its line in the challenge file. The part-only keys
(`price`, `price_per_gb`, `key_bytes`, `sizes`) are read and removed first.

- [ ] **First, a spike test that decides the approach** (red by assertion, not by compile error):
  a `YAML::Node` taken from one loaded document and assigned into a map of another keeps its
  `Mark()`. If it does not, stop and report: the composing then needs a mark table beside the tree.
- [ ] Tests (`test_challenge.cpp`, files written to a temporary directory), each named after its
  property:
  - *a challenge states its load, its SLOs and its parts*: the spec §4 example loads; `parts` has
    four entries, `web` three sizes with their prices, `par` 1980, `peak_at` 50,400 s (14:00);
  - *a part fixes what it states, at the challenge's line*: `web: {type: service, capacity: fast}`
    is refused with the loader's message for that value, `file` the challenge's, the line of
    `capacity`;
  - *a size is a whole part*: a size whose `capacity` is a byte rate on a `service` is refused,
    naming the size (`part 'web' size 'large': ...`);
  - *a price is a non-negative number of dollars*: missing (`part 'web': price is required (dollars
    a month)`), negative, on a sized part at the top instead of in the sizes;
  - *memory is priced only where there are keys*: `price_per_gb` or `key_bytes` on a `service`
    refused; one without the other refused;
  - *a challenge's SLO names nothing of the design*: `{metric: utilization, node: db}` refused:
    `slo: 'db' is not given by the challenge (users)`;
  - *the peak falls inside the run*; *par is required unless open, refused when open*; *an open
    build may give nothing* (`given: {}`), a closed one must give at least one client;
  - *an unknown key is named* (`challenge has unknown key 'parr'`).
- [ ] Tests (`tests/cli/challenge.cmake`): `simsys challenge <file>` prints the title and level,
  the given clients (the peak rate from the curve: for the example **4.95k/s** = 3.3k/s × 1.5 at
  14:00), the SLOs, a parts table (part, size, capacity, latency, $/month), par; exit 0.
  `--report json`: `format_version` 1, `challenge` (the keys of spec §6), `given` (the graph keys
  of the given nodes), `slos`, `parts` (name, type, sizes: name, the fixed keys as the graph prints
  them, `price_usd_month`; `price_per_gb`, `key_bytes`). A broken file: the error with the
  challenge's path, exit 1.
- [ ] Implementation. GCC precautions: `challenge.cppm`'s global module fragment has only
  `<optional>`, `<string>`, `<vector>`; no lambda in a range adaptor; after green build Release and
  add `emit_special_members` lines to `src/model/model.cpp` (or the same device in the new module)
  if the link asks (`std::vector<Part>`, `std::expected<Challenge, ConfigError>`).
- [ ] First-use C++20/23 comments in the new units; `docs/study/` needs nothing new.

### Task 2: `--challenge`: compose a design, refuse what the challenge fixes  (opus)

**Files:** `src/config/config.cppm`, `src/config/challenge.cpp`, `src/cli/main.cpp`; tests
`test_challenge.cpp`, new `tests/cli/design.cmake`.

```cpp
// A design composed with its challenge (phase 6 spec §5): the scenario the engine runs, and what
// each design node bought (for the bill, Task 3).
struct Purchase { std::string node; std::string part; std::string size; };   // size empty: unsized
struct Design { model::Scenario scenario; challenge::Challenge challenge; std::vector<Purchase> purchases; };
[[nodiscard]] std::expected<Design, ConfigError> load_design(const std::string& design_path,
                                                             const std::string& challenge_path);
```

Composition, on the YAML trees, in this order: check the design's shape (the table of spec §5);
for each design node look up `part` and `size`, refuse a key the part or size fixes, then write
the fixed keys and `type` into the node's map and remove `part` and `size`; build the root
(`version`, the challenge's header, `nodes` = the given ones then the design's in file order,
`links` = the design's, `slo` = the challenge's; an open build takes `slo` and `duration` from the
design when present, and lets design nodes with `type: client` through); hand the root to the
existing pipeline with the design's path as the error file.

- [ ] Tests (`test_challenge.cpp`):
  - *composing the reference gives the scenario its hand-written twin gives*: the spec §4 design
    with the §4 challenge equals (member for member, via the existing scenario comparison or the
    `explain` text) a plain scenario file written out by hand in the test;
  - each row of spec §5's refusal table, asserting the message **and the line and column of the
    offending key in the design file** (*a design that restates a key its part fixes is refused at
    that key's line*: `capacity: 9k/s` on line 4 → line 4);
  - *a sized part needs its size*: missing, unknown (`part 'web' comes in sizes small, medium,
    large`), a `size` on an unsized part;
  - *the loader's own checks still blame the design*: a link to a node that does not exist, a step
    to an unlinked node: today's message, the design's path and line;
  - *an open build takes the design's clients, SLOs and duration*; *a closed challenge refuses them*;
  - *the given nodes come first*: node order in the scenario is given, then design (a client's
    noise seed depends on its index: schema, `seed`).
- [ ] Tests (`tests/cli/design.cmake`): `validate`, `explain`, `estimate --at 14:00`, `run
  --duration 10m` on the example design with `--challenge`: exit 0 and the scenario name
  `read-heavy`; `run --stream` under a refusal: one `error` line with the design's `file`, line and
  column; refused combinations: `--challenge` with `--days`, `--replications`, and on `sweep`
  (`a challenge is not swept: the search is the designer's`); **without `--challenge` every
  existing CLI test still passes unchanged** (byte identity is already pinned by `stream.cmake` and
  the web fixtures).
- [ ] Implementation: `main.cpp`'s `load_for_run` and the loaders of `validate`/`explain`/
  `estimate` go through one function that picks `config::load` or `config::load_design`; the
  `Design` is kept for Task 3.

**Checkpoint A.** Review of the batch's diff; Release and Debug green.

---

## Batch B

### Task 3: cost  (opus)

**Files:** `src/challenge/challenge.cppm`, new `src/challenge/cost.cpp`, `src/report/` (table,
JSON, `stream.cpp`), `src/cli/main.cpp`; tests `test_challenge.cpp`, `tests/unit/test_report.cpp`,
`tests/theory/test_stream.cpp`, `tests/cli/design.cmake`.

```cpp
// The monthly bill of a design (phase 6 spec §6). What it models: a fleet rented by the month,
// fixed for the day and sized for its peak, as the book's back-of-the-envelope estimates size it.
// What it ignores: autoscaling, storage that grows over the years, bandwidth, cross-datacenter
// traffic, people. Prices are the challenge's (dollars a month, from a public price list; its
// README says which).
struct BillLine { std::string node, part, size; int machines{}; double machine_usd{}; double memory_gb{};
                  double memory_usd{}; double usd_month{}; };
struct Bill { std::vector<BillLine> lines; double usd_month{}; std::optional<double> par_usd_month{}; };
[[nodiscard]] Bill price(const model::Scenario&, const Challenge&, std::span<const Purchase>);
```

(`Purchase` moves to `simsys.challenge` so that `price` does not import `simsys.config`.)

- [ ] Tests, each with its derivation in a comment:
  - *a tier costs its machines*: 3 medium web at $140 → **$420**;
  - *a sharded database's machines are replicas × shards*: `replicas: 3, shards: 3` at $480 →
    9 machines, **$4,320**;
  - *a kv_cluster's machines are its nodes, a pool's its workers*;
  - *a cache costs its memory*: `keys: 2M` × `key_bytes: 1KB` = 2 GB (1 GB = 1e9 bytes, as the
    schema's byte units) at $12/GB → **$24**, plus its machine price if the part has one;
  - *a given node costs nothing* (no line for a client);
  - *the total is the sum of the lines* and *par is the challenge's, absent in an open build*.
- [ ] Renderers, tests first in `test_report.cpp` / `test_stream.cpp` / `design.cmake`:
  - table (`explain`, `estimate`, `run`): the `cost` block of spec §6, dollars with thousands
    separators and no cents (`$1,120`), by hand with `std::to_chars` (second GCC defect: no width
    or precision in `std::format`);
  - JSON: per node `part`, `size`, `machines`, `cost_usd_month`; the document's `challenge` object.
    **Absent without `--challenge`**, not `null`, so every existing JSON and stream stays byte for
    byte (the fixture check proves it). Correct spec §6 here: it says `null`;
  - the stream: the keys in the `graph` line and in the `summary`'s report.
- [ ] `gcc14-asan` after the batch (the composing moves YAML nodes between documents).

**Checkpoint B.**

---

## Batch C

### Task 4: the four challenges and the open build  (opus; needs the Release binary and a web search)

**Files:** `challenges/README.md`, `challenges/prices.md`, `challenges/0{0..4}-*/{challenge.yaml,
README.md,starter.yaml,reference.yaml}`; tests `tests/scenario/test_challenges.cpp` (new),
`tests/CMakeLists.txt`.

- [ ] **The price list first** (`challenges/prices.md`): one table used by every challenge. Per
  part and size: the instance class it stands for, its on-demand hourly price in one region read
  from the provider's public price list (the date and the URL noted), × 730 h, rounded to $5. A
  managed-database class for `db`, a memory-optimised class's price per GB for `cache`, a managed
  load balancer's hourly charge for `lb`. The capacities per size are **ours** (the scenario's
  model numbers, e.g. the url-shortener's web at 3k/s and its database at 5k units/s as the medium
  size, half and double for small and large) and each README says so: the dollars are real, the
  requests per second of a box are the model's. If two sizes come out exactly proportional in
  price and capacity, say in the README what still differs (the step the tier grows by; the pooled
  queue).
- [ ] The challenges of spec §9, each README in this order: the brief (load, SLOs, parts), what
  it teaches and the book chapter, what the model leaves out (cost's list, spec §6, and that
  without a headroom rule the cheapest design runs near saturation at the peak), hints (folded
  under a heading the page collapses), the reference's arithmetic.
  - `01-one-tier`: steady 5k/s, one op `get`, `duration: 10m`, `step: 100ms`; parts `lb`, `web`.
  - `02-daily-peak`: sine day, mean 3.3k/s, amplitude 0.5, peak 14:00; 24 h at 1 s.
  - `03-read-heavy`: the spec §4 example, its numbers re-derived.
  - `04-url-shortener`: `scenarios/book/url-shortener`'s clients and its client-side SLOs; parts
    `limiter`, `rules_cache`, `lb`, `web`, `id_gen`, `cache`, `db`; the reference is the shipped
    scenario as a design (medium sizes), unless a cheaper one is found, which then is the reference.
  - `00-open-build`: `open: true`, a part per node type the simulator runs (schema, "What runs"),
    three sizes for the types with a capacity; the starter is challenge 1's reference with its
    client.
- [ ] For each reference, look for a cheaper passing design across the sizes (by hand and with
  `estimate`; a dozen candidates at most) and keep the cheapest as the reference. `par` is its
  cost. The README lists the candidates tried and their bills.
- [ ] Tests (`test_challenges.cpp`, one `TEST_CASE` per property, looping over the directories;
  the 24 h runs registered for Release only, as the book scenarios' day tests are):
  - *every reference passes its challenge and costs par*;
  - *every starter is not a solution* (refused, or an SLO fails);
  - *a reference is not padded*: one machine fewer in each tier that has more than one fails an SLO
    (generated by an override on the composed scenario, not a second file);
  - *the open build's starter runs*;
  - *every challenge directory has its four files and a README that names its chapter*.
- [ ] Scenario READMEs rule (CLAUDE.md §3): each challenge README explains the design space and
  what the model abstracts away; `challenges/README.md` explains the format and how to add one.

**Checkpoint C.** The user can play in the terminal from here.

---

## Batch D

### Task 5: `@simsys/server`  (sonnet)

**Files:** `web/server/{package.json,tsconfig.json}`, `web/server/src/{handler.ts,challenges.ts,
run-simsys.ts,limits.ts,vite-plugin.ts,index.ts}`, `web/server/test/`, `web/package.json`
(workspace, `check:types`), `web/app/vite.config.ts` (the plugin), `web/vitest.config.ts`.

```ts
// Pattern: Adapter — a command-line program behind HTTP. It adds nothing to simsys's output: a
// run's response is byte for byte the child's stdout.
export function createHandler(options: {
  simsys: string;          // the binary
  challengesDir: string;
  maxRuns: number;         // at once; default os.availableParallelism()
  runTimeoutMs: number;    // default 5 min
}): (req: IncomingMessage, res: ServerResponse, next: () => void) => void;
```

Routes as spec §7. Requests outside `/api/` call `next()`.

- [ ] Tests (Vitest, a real `http.createServer(handler)` on port 0, and **a fake `simsys`**: a
  small Node script the tests write, which prints given lines, optionally sleeps, and records its
  arguments and its `SIGTERM`):
  - *the list is what `simsys challenge --report json` says, plus the brief and the starter*;
  - *an id that is not listed is 404 and spawns nothing* (also `..%2F..`, an empty id);
  - *a body over 64 KB is 413 and spawns nothing*; *a method other than the route's is 405*;
  - *the design reaches simsys as a file named design.yaml, with `--challenge` the listed path*,
    arguments as a list (a design containing `; rm -rf` is just text in a file);
  - *explain answers the graph, or the error line's file, line, column and message* (the child's
    `error` under `--stream`-less explain comes on stderr as `file:line:column: error: ...`:
    parse `describe`'s format; the file shown is `design.yaml`, never the temporary path);
  - *a run's bytes are the child's*, chunked as they arrive (the first frame is received while the
    child still sleeps);
  - *closing the connection kills the child*; *a run over its time is killed and the stream ends
    without a summary*; *the temporary directory is gone after the child exits*, in all three ends;
  - *more runs than the cap answer 429*; explain and estimate are not capped;
  - *without a binary every route answers 503 and says how to build it*.
- [ ] `vite-plugin.ts`: `configureServer` and `configurePreviewServer` mount the handler;
  `vite.config.ts` uses it with `SIMSYS` or the default path. One line on what a Vite plugin hook
  is (CLAUDE.md §5's idiom rule).
- [ ] CI's `web` job needs no change beyond the workspace (the tests use the fake binary). One
  integration test runs the real binary when `SIMSYS` is set, as the precompute's does.

### Task 6: `LiveSource`  (sonnet)

**Files:** `web/stream/src/live-source.ts`, `index.ts`, `web/stream/test/`.

```ts
// Pattern: Strategy (source.ts) — a run streamed by the local server as it is computed.
export class LiveSource implements DatasetSource {
  constructor(url: string, body: string, fetchImpl?: typeof fetch);
  readonly label: string;
  lines(signal?: AbortSignal): AsyncIterable<string>;
}
```

- [ ] Tests (a fake `fetch` returning a `ReadableStream` fed chunk by chunk): *lines split across
  chunks arrive whole* (reuse `lines.ts`); *an abort stops the iteration and cancels the body*; *a
  non-200 answer is one error the store shows* (429: "the machine is busy with other runs"); *the
  store fills from a stream that has no dataset line* (`status` goes `loading` → `ready` at the
  summary; frames are readable before it); *a stream that ends without a summary is `truncated`*.

**Checkpoint D.**

---

## Batch E

### Task 7: the *Design* mode: list, brief, editor, live diagram, bill  (sonnet)

**Files:** `web/app/src/App.tsx` (the mode, in the hash: extend `hash.ts`), `src/design/
{DesignPage.tsx,ChallengeList.tsx,Brief.tsx,api.ts,drafts.ts,explain-loop.ts,bill.ts,Bill.tsx}`,
`src/editor/{YamlEditor.tsx,marks.ts}`, `src/diagram/NodeCard.tsx` (the cost line), `web/app/test/`.

Pure functions first, each tested without a browser:

- [ ] `hash.ts`: *the mode and the challenge survive a reload* (`#design/03-read-heavy`); *an old
  5b hash still opens Watch*.
- [ ] `drafts.ts` (over a `Storage` passed in): *a draft is kept per challenge*; *reset restores
  the starter*; *the best passing cost only goes down*; *a storage that throws loses nothing but
  the memory* (private windows).
- [ ] `explain-loop.ts` (a function of a clock and a `fetch`, no React): *an edit is sent after
  300 ms of quiet, and only the last one*; *an answer to an older edit is dropped* (out-of-order
  responses); *an error keeps the last good graph and marks it stale*.
- [ ] `bill.ts`: *the rows are the graph's priced nodes in order, the total their sum*; *over,
  at and under par read as such* (`$330 over par`); *an open build has no par line*; dollars
  formatted with thousands separators.
- [ ] `marks.ts`: *a loader error becomes a mark at its line and column*; *line 0 (no position)
  marks nothing and shows in the status line*.
- [ ] Components (Testing Library, `fetch` faked): *the list shows the open build first, then the
  challenges by level*; *the brief folds, and its hints start folded*; *without the routes the
  page says it needs `npm run serve` and Watch still works*; *the diagram is not rebuilt when the
  explain answer's graph is equal to the last* (CLAUDE.md §5: structure stays put).
- [ ] `YamlEditor.tsx`: CodeMirror 6 with `@codemirror/lang-yaml` and `@codemirror/lint` for the
  marks. jsdom does not lay CodeMirror out, so the component stays thin (text in, text out, marks
  in) and has one smoke test.
- [ ] The diagram: 5b's `Diagram` fed the explain answer's graph through a store filled from a
  `MemorySource` of that one `graph` line, laid out by ELK; `NodeCard` shows size, machines and
  dollars when the node has them. The panes reuse `layout/Splitter.tsx` and `panes.ts`.
- [ ] Screenshots for the user (Playwright from the scratchpad, as in 5b) at the checkpoint.

**Checkpoint E.** The user looks at the page.

---

## Batch F

### Task 8: run, cancel, the verdict, the estimate at the peak  (opus for `verdict.ts`, sonnet for the views)

**Files:** `web/app/src/verdict/{verdict.ts,Verdict.tsx,explain-bottleneck.ts}`, `src/design/
{RunControls.tsx,PeakEstimate.tsx,Reference.tsx}`, `web/app/test/`.

```ts
// A discriminated union: TypeScript narrows on `kind`, as a C++ std::variant with std::visit.
type Verdict =
  | { kind: "passed"; costUsd: number; parUsd: number | null; slos: SloRow[] }
  | { kind: "failed"; failedSlos: SloRow[]; first: Bottleneck | null; others: Bottleneck[];
      slowOps: SlowOp[] }                       // slowOps: when no node was red
  | { kind: "stopped" };                        // cancelled, or the stream ended early
interface Bottleneck { node: string; frame: number; tS: number; peakRho: number;
                       demandPerS: number; capacityPerS: number; unit: string;
                       why: "over-capacity" | "near-saturation" }
```

- [ ] Tests for `verdict(store)` (stores built from hand-written frames):
  - *a run whose SLOs all pass is scored against par* (over, at, under; no par in an open build);
  - *the first bottleneck is the node that turned red in the earliest frame*;
  - *of two nodes red in the same frame, the one further downstream is first* (the graph's links:
    a saturated callee backs its callers up, so the caller's red is a consequence);
  - *ρ ≥ 1 reads as over capacity, a red zone below 1 as near saturation* (the two sentences of
    spec §8, each explained in a comment per CLAUDE.md §2: the fluid queue fills when λ > c·μ;
    Erlang C's wait grows as 1/(1 − ρ));
  - *a failure without a red node lists the ops over their limit*;
  - *a cancelled run and a truncated stream are "stopped", never "failed"*;
  - *a client row is never a bottleneck* (its node row is all null).
- [ ] Views: *Run starts a LiveSource and Cancel aborts it*; *the charts fill while the run goes*
  (the store's version advances before the summary); *a 429 is said plainly*; *clicking the
  bottleneck selects the node and moves the slider to its frame*; *Estimate at peak shows each
  node's ρ at the challenge's peak and says it is one instant*; *Reference shows the reference
  read-only beside the draft* (hidden for the open build); *a pass records the best cost in the
  drafts*.
- [ ] A fixture: challenge 1's reference stream, committed with the 5b fixtures and covered by
  `npm run fixtures -- --check`.
- [ ] Screenshots: a pass, a fail with its bottleneck.

**Checkpoint F.**

---

## Batch G

### Task 9: docs  (sonnet)

- [ ] `docs/scenario-schema.md` v1.9: "New in", the challenge file, the design file, `simsys
  challenge`, `--challenge`, the cost keys in JSON and the stream (added keys, `format_version` 1).
- [ ] The main spec: §24 "Phase 6 as built". This plan: "As built" (what differed and why).
- [ ] `README.md` (the terminal walk-through of challenge 1), `web/README.md` (the Design mode,
  the server routes, `SIMSYS`), `CLAUDE.md` (§3: challenges get a README; §5: the page's data still
  comes only through a `DatasetSource`; any new GCC defect met on the way).

### The branch review, then the PR

- [ ] A review of the whole branch's diff against the spec; fixes tests-first.
- [ ] `gcc14-asan`, `clang20-libcxx-debug`, Release; `npm run check && npm test && npm run build`;
  the fixture check; clang-format-18.
- [ ] Ask the user before `git push`; open the PR; CI decides.

---

## Self-review

- Every row of spec §3 has a task: 6.1 → Tasks 1–2, 6.2 → 3, 6.3 → 4, 6.4 → 5, 6.5 → 6, 6.6 → 7,
  6.7 → 8, 6.8 → 9.
- Where this plan refines the spec: composition happens on the YAML tree inside `simsys.config`
  (spec §5 updated); a `simsys challenge` command describes and checks a challenge file (spec §5
  updated); the routes live in Vite's server, not a process of their own (spec §7 updated); the
  cost keys are absent, not `null`, without `--challenge` (Task 3 corrects spec §6).
- Risks, each with where it is met: yaml-cpp marks across documents (Task 1's spike, first);
  GCC 14.2's fourth and tenth defects on the new types (Release after every C++ task); real prices
  that make one size dominate (Task 4 says what to do); CodeMirror under jsdom (Task 7 keeps the
  component thin); two sizes of one tier in one design (not possible in one node: the README shows
  two nodes behind one balancer).

## As built

(filled in by Task 9)
