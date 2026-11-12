# Phase 6: design challenges (user-built systems)

Design for phase 6 of system-sim, drafted on 2026-11-12 after phase 5b (PR #5, the watch-only web
app) and PR #6 (resizable panes) merged into main (`3f51c87`). Until now a student **watches** a
system somebody else sized. In phase 6 the student **designs** one: a challenge states the load and
the service levels, the student writes the system, the simulator runs its day and says whether it
holds and what it costs. Where this document conflicts with an earlier spec, it wins. The plan
(`docs/superpowers/plans/2026-11-12-phase6-design-challenges.md`, written after this is approved)
turns it into tasks. The schema (v1.9) and a new spec §24 record what was finally built.

## 1. Goal

The user's brief (2026-11-11): *"a challenge should state the current client throughput and the user
should plan a system that will be able to handle it — no faults, no changes."* Pure capacity
planning, on localhost.

A student opens the page, picks *read-heavy*, and reads: 5,000 requests a second at the 14:00 peak,
ten reads per write, p99 of a read under 50 ms, nothing dropped; a web server comes in three
sizes, the medium one doing 3k/s for $140 a month, and so does a database machine. They write nodes
and links in the YAML pane. The diagram beside it follows every edit and shows the bill. *Run* plays
the day into the same charts 5b has. The verdict: **passes at $2,310 a month, par is $1,980**, or
**fails: `db` reached ρ = 1.08 at 13:12, offered 5.4k units/s against 5k/s**, in the book's terms.
The game is the user's (2026-11-12): *handle the given traffic at the minimum cost*.

```
browser                                   routes in Vite's server (Node)            simsys
  YAML pane ── POST design ─────────────>  temp file  ── spawn ──────────────────>  explain --challenge
  diagram + bill  <── graph (JSON) ──────                                           estimate --challenge
  charts, verdict <── NDJSON, chunked ───  stdout piped through  <────────────────  run --challenge --stream
        (LiveSource: a DatasetSource)
```

After phase 6:

- `challenges/<id>/` holds a challenge: the given part, the parts list with prices, par, a README
  with the brief, a starter and a reference design (§4).
- `simsys validate|explain|estimate|run <design.yaml> --challenge <challenge.yaml>` composes the
  two into one scenario, refuses a design that touches what the challenge fixes, and reports the
  design's **cost** beside par (§5, §6). The terminal alone is enough to play.
- `npm run serve` (in `web/`) also runs `simsys` for the page, through routes mounted in Vite's
  server (§7). The page has a *Design* mode beside 5b's *Watch* (§8).
- Four challenges ship, from one tier to the book's URL shortener, and an **open build**: every
  part, the student's own load and no par (§9).

## 2. Decisions

Settled by the user on 2026-11-11:

| Question | Decision | Why |
|---|---|---|
| Cost | A passing design is **scored by its cost against par**, the reference design's cost. No hard budget | Without a cost, "add replicas until it passes" always wins. A budget would be one more way to fail; par leaves room to beat the reference |
| The editor | A **YAML pane** in the page with the live diagram. Drag and drop later, if it proves worth it | The YAML is the scenario language the project already teaches, and its errors already carry line and column |
| The runner | A **local Node server** spawns `simsys` and pipes `run --stream` to the page: a new `DatasetSource`. WebAssembly waits until hosting matters | A user-built system cannot be precomputed. The views read only the store (5b's design rule), so they do not change |

Proposed on 2026-11-12 and settled by the user's review the same day (§12 records the answers):

| Question | Proposal | Why |
|---|---|---|
| What stops a design from cheating | A challenge has a **parts list**: named machine kinds with fixed numbers (`capacity`, `latency`, per-op `cost`, a cache's `hit_ratio` rule) and a price, **in three sizes** where size matters. A design node names its `part` and `size` and may not restate what they fix. What is left is the design: which parts, which size, how many, how they are wired, shards, cache size, policies | `capacity` and `latency` are free numbers in a scenario: `capacity: 1M/s` on one box would pass anything. Pricing capacity alone leaves `latency` free. A catalogue is also what a real designer has: instance types at a price. Sizes add vertical against horizontal scaling: fewer large machines or more small ones |
| Where composing and pricing happen | **In `simsys` (C++)**, behind a `--challenge` flag, as a pure function before the graph is built (as 5b's `repeat_days`) | The alternative, the server merging YAML in TypeScript, would put error positions in a generated file and make the terminal unable to play. 5b chose the same way for `--days`, for the same reason |
| What a design file is | **Only `nodes` and `links`**. The clients, the SLOs, `duration`, `step`, `warmup` and `seed` come from the challenge | The given part cannot be edited by mistake or on purpose, and the file a student writes is short |
| Prices | **Dollars a month**, per machine and per GB of cache memory, rounded from a public cloud's on-demand list prices; each challenge's README names the instance class and the date they were read | The user's choice over relative units: a bill in dollars is what a designer argues about |
| Headroom | **No rule in phase 6.** Only the challenge's latency and drop SLOs judge, so the cheapest passing design runs close to saturation at the peak, and the READMEs say so | The user's choice: the first game is the given traffic at the minimum cost. Headroom earns its price when something fails, so a `red_time` SLO comes with the fault challenges (§11) |
| Feedback on a failure | The page's **verdict** names the failed SLOs and the **first bottleneck**: the node that turned red first, when, its peak ρ and what was offered against its capacity; then every other node that was red. A pure function of the store's frames, in TypeScript | The stream already carries zone, `peak_rho`, demand and capacity per node and frame. Nothing new is computed in C++ |
| Feedback before a run | Every edit (debounced) is sent to `explain`: the diagram and the bill follow, a loader error is marked on its line. The **estimate at the peak** (`simsys estimate --at <the challenge's peak>`: each node's utilization at the busiest instant, computed at once without playing the day) runs **only on request**, from a button | The live diagram costs milliseconds. Working out each tier's load at the peak is the exercise itself (the book's back-of-the-envelope estimate), so the student does it by hand first and asks the simulator after |
| The run | One run of the challenge's `duration`, no `--days` | No faults and no spikes. A design that passes carries no backlog over midnight, and one that fails shows it within the day |
| Server stack | Node's built-in `http`, no framework; a fourth workspace `@simsys/server` | Four routes. One dependency fewer to pin and to explain |
| Editor component | CodeMirror 6 with its YAML language | Line numbers and error marks at the loader's line and column; small, no worker needed (Monaco is ten times the size) |

## 3. Sub-steps and order

Each sub-step is a task in the plan: a red commit with the tests, a green commit, a review, a fix
round, a checkpoint. Implementation goes to subagents; the design, the task briefs and the reviews
stay with the orchestrating session.

| Step | Content | Size | Files mostly touched |
|---|---|---|---|
| 6.1 | The challenge file and `--challenge`: load, compose, sizes, the open build, the refusals (C++) | large | new `src/challenge/`, `cli/main.cpp`, tests |
| 6.2 | Cost: the bill in `explain`, `estimate`, `run` (table, JSON, stream) (C++); ASan | medium | `src/challenge/`, `report/`, `session/` |
| 6.3 | The four challenges and the open build, their READMEs, starters and references; the test that each reference passes at par | medium | `challenges/`, tests |
| 6.4 | `@simsys/server`: the routes, the child process, cancel, limits | medium | `web/server/` |
| 6.5 | `LiveSource` in `@simsys/stream`; the store on a stream without a `dataset` line | small | `web/stream/` |
| 6.6 | The app's *Design* mode: challenge list, brief, YAML pane, live diagram with the bill | medium–large | `web/app/src/{design,editor}` |
| 6.7 | Run, cancel, the verdict (score, bottleneck), the estimate at the peak | medium | `web/app/src/{design,verdict}` |
| 6.8 | Docs: schema v1.9, spec §24, READMEs, CLAUDE.md; the branch review; PR | small | `docs/`, `README.md` |

## 4. A challenge on disk

```
challenges/
  01-one-tier/
    challenge.yaml     the given part, the parts, par
    README.md          the brief, what it teaches, the chapter, what the model leaves out; hints at the end
    starter.yaml       what the YAML pane opens with: the client linked to nothing, comments
    reference.yaml     a design that passes; its cost is par
```

```yaml
# challenges/03-read-heavy/challenge.yaml
version: 1
name: read-heavy
title: Ten reads for every write
level: 3
chapter: 1                         # System Design Interview vol. 1
duration: 24h
step: 1s
warmup: 2m
peak_at: 14:00                     # the instant the estimate at the peak looks at

given:                             # scenario syntax; a design cannot change it
  nodes:
    users:
      type: client
      rate: {base: 3.3k/s, wave: {shape: sine, period: 24h, amplitude: 0.5, peak_at: 14:00}}
      ops: {read: 0.91, write: 0.09}
      keys: {zipf: 1.0, space: 10M}
      timeout: 1s
  slo:
    - {metric: p99_latency, node: users, op: read, max: 50ms}
    - {metric: drop_rate, max: 0}

parts:                             # what the designer can buy; prices in dollars a month
  lb:     {type: load_balancer, price: 20}
  web:
    type: service
    latency: 0.3ms                 # the same CPU in every size: more cores, not faster ones
    sizes:
      small:  {capacity: 1.5k/s, price: 70}
      medium: {capacity: 3k/s,   price: 140}
      large:  {capacity: 6k/s,   price: 280}
  cache:  {type: cache, hit_ratio: che, key_bytes: 1KB, price_per_gb: 12}
  db:
    type: sql_db
    replication_lag: 50ms
    sizes:
      small:  {capacity: 2.5k/s, price: 250}
      medium: {capacity: 5k/s,   price: 480}
      large:  {capacity: 10k/s,  price: 900}

par: 1980                          # reference.yaml's cost; a test keeps the two equal
```

The dollar figures above are placeholders that show the format; 6.3 reads the real ones.

```yaml
# a design: nodes and links only
nodes:
  lb:    {part: lb, policy: round_robin}
  web:
    part: web
    size: medium
    replicas: 3
    ops:
      read:  [{to: cache, op: read}, {to: db, op: read, on: miss}]
      write: [{to: db, op: write}]
  cache: {part: cache, eviction: lru, strategy: cache_aside, keys: 2M}
  db:    {part: db, size: medium, replicas: 3}
links:
  - {from: users, to: lb, dir: inout}
  - {from: lb, to: web, dir: inout}
  - {from: web, to: cache, dir: inout}
  - {from: web, to: db, dir: inout}
```

Rules:

- A part is a node with `type`, any of the type's keys, and a `price` in dollars a month per
  machine (required, ≥ 0). **A part fixes every key it states.** A key it leaves out is the
  designer's (`replicas`, `shards`, `keys`, `policy`, `ops`, ...), or the type's default.
- A part may have **`sizes`**: a map of size name → the keys that differ (`capacity`, `price`, any
  other key of the type). A design node of such a part must name its `size`; a part without sizes
  refuses the key. A challenge's author chooses how many sizes; the shipped ones have three where
  size matters. In the model a larger machine has more servers of the same speed: `capacity`
  grows and `latency` stays, so its c = capacity × latency slots grow with it (Little's law, as
  for every node). Two large machines and four small ones then differ in price, in the step by
  which the tier can grow, and in the wait: one pooled queue of c servers waits less than two of
  c/2 (Erlang C), which the READMEs show.
- A `cache` or `cdn` part is priced by memory: `key_bytes` (the size of one cached entry, a fact
  of the workload) and `price_per_gb`; the designer's `keys` × `key_bytes` is the memory bought.
- A challenge's SLOs name a given client, one of its ops, or no node. They cannot name a design's
  node, which does not exist yet.
- `peak_at` is where `estimate` looks (§7). The loader checks that it falls inside the run.
- The prices are **dollars a month**, not from the book: rounded from a public cloud's on-demand
  list prices (hourly price × 730 hours). §2 of CLAUDE.md applies: the comment beside each price,
  and the README, name the instance class it stands for and the date the price was read. They are
  for comparing designs, not a quote.
- **`open: true`** makes the challenge an open build (§9): `given` may be empty, there is no
  `par`, and the design may also hold `client` nodes (written with `type`, as in a scenario), `slo`
  and `duration`.

## 5. `--challenge` (C++, 6.1)

```
simsys validate|explain|estimate|run <design.yaml> --challenge <challenge.yaml> [the command's flags]
```

The two files are composed before the graph is built, so the engine and the behaviours are
untouched and `explain` shows exactly what will run. The loader composes them **as YAML**, then
runs its ordinary checks on the result, so a design's value is validated exactly as a scenario's is
and keeps its own line and column (the way `sweep`'s overrides are applied before validation):

```cpp
// simsys.config
struct Design { model::Scenario scenario; challenge::Challenge challenge; };   // priced by simsys.challenge
auto load_design(const std::string& design_path, const std::string& challenge_path)
    -> std::expected<Design, ConfigError>;
```

The scenario gets the challenge's `name`, `duration`, `step`, `warmup`, `seed` and SLOs; the given
nodes, then the design's nodes in file order, each completed from its part; the design's links.
Errors keep the design file's line and column:

| a design that | is refused with |
|---|---|
| has a top-level key other than `nodes` and `links` | `a design has only nodes and links; 'slo' comes from the challenge` |
| names a node as a given one | `node 'users' is given by the challenge` |
| has a node without `part`, or with `type` | `node 'web': a design node names a part (lb, web, cache, db)` |
| leaves out the `size` of a sized part, or names an unknown one | `node 'web': part 'web' comes in sizes small, medium, large` |
| names an unknown part | `node 'web': no part 'webserver' in challenge 'read-heavy' (lb, web, cache, db)` |
| states a key its part fixes | `node 'web': capacity is fixed by part 'web' (3k/s)` |
| has `faults`, or a `client` node | refused: a challenge has no faults, and the load is given (an open build accepts clients, `slo` and `duration`) |

Everything else is the loader's ordinary validation of the composed scenario (a link to a missing
node, a step to an unlinked node, ...), with the same messages as today.

Refused with `--challenge`: `--days` (§2), `--replications`, `sweep` (a sweep would search the
answer; it can come later as a deliberate tool). `--seed`, `--duration`, `--stream`, `--report`
combine as before.

A challenge file alone is read by a new command, `simsys challenge <challenge.yaml> [--report
json]`: it checks the file (its own keys, each part and size as a node of its type, its SLOs
against the given clients; errors name the challenge file) and prints what a designer is given:
the load, the SLOs, the parts with their sizes and prices, par. The server's list comes from its
JSON. `--challenge` runs the same checks first, so an error left after them is the design's.

GCC 14.2 precautions (CLAUDE.md §3): a new module at the end of the import chain (the sixth
defect: `-flarge-source-files` is already on the library target), heavy headers in the
implementation unit, no lambda in a range adaptor, Release built after the task for the fourth and
tenth defects (`std::expected<Scenario, ...>` and a `std::vector<Part>` crossing a module boundary).

## 6. Cost (C++, 6.2)

The fleet is fixed for the day and sized for its peak, as the book's estimates are:

```
cost ($/month) = Σ over design nodes:  price(part, size) × machines(node)
                                       + price_per_gb(part) × keys × key_bytes / 1 GB     (cache, cdn)

machines(node) = replicas × shards     sql_db (every shard is a primary and its read replicas)
                 nodes                  kv_cluster
                 workers                worker_pool
                 replicas               every other type
```

What it deliberately ignores, said in the code's header comment and in each README: autoscaling
(a fleet that follows the daily wave pays for less than its peak), storage that grows with the
years (the URL shortener's 365 billion rows), bandwidth, cross-datacenter traffic, people.

Where it shows:

- `explain` and `run` (table): a `cost` block, one line per node (`web  medium
  8 machines × $140 = $1,120`), the total and par (`cost $2,310/month  par $1,980  (+$330)`; an open
  build has no par).
- JSON (`explain --report json`, `run --report json`, the stream's `graph` and `summary`): a node
  gains `part`, `size`, `machines` and `cost_usd_month` (`null` on a client and without
  `--challenge`); the document gains `"challenge": {"name", "title", "level", "open", "par_usd_month"
  (`null` in an open build), "cost_usd_month", "peak_at_s"}` (absent
  without `--challenge`). Added keys: `format_version` stays 1 (contract rule 2).
- The exit code is `run`'s as today (2: an SLO failed). Cost never fails a run.

## 7. The local server (`@simsys/server`, 6.4)

A request handler for Node's `http`, mounted **into Vite's own server** by a small plugin (the
`configureServer` and `configurePreviewServer` hooks), so `npm run serve` and `npm run dev` gain
the routes on the port they already use (localhost only, Vite's default) and no second process,
proxy or static-file code exists. A static host without it is §8's "without a server". The routes:

| route | does | answers |
|---|---|---|
| `GET /api/challenges` | reads `challenges/*/` once at start | per challenge: id, the brief (README as Markdown), the starter text, and `simsys challenge --report json`'s document (title, level, chapter, the load, the SLOs, the parts with sizes and prices, par) |
| `POST /api/challenges/:id/explain` | `simsys explain --report json` on the body | the graph with the bill, or `{error: {line, column, message}}` |
| `POST /api/challenges/:id/estimate` | `simsys estimate --at <peak_at> --report json` | the steady state at the peak, or the error |
| `POST /api/challenges/:id/run` | `simsys run --stream --every 1m` | the NDJSON stream, chunked as it is written |
| `GET /api/challenges/:id/reference` | reads `reference.yaml` | the reference design's text (404 for the open build, which has none) |

The body is the design's YAML as text. The server writes it to `design.yaml` in a fresh temporary
directory, spawns `simsys` with an argument list (never a shell), and removes the directory when
the child exits.

Limits, since even a local server should not be a way to hang the machine: `:id` must be one of
the listed challenges (never a path); a body over 64 KB is refused; at most as many runs at once
as cores, the next one answered `429`; a run is killed after 5 minutes; **a closed connection
kills its child** (that is how *Cancel* works: the page aborts the request).

Pattern: Adapter. The server adapts a command-line program to HTTP and adds nothing to its
output. A `run` response is byte for byte what `simsys` wrote.

The `simsys` binary: the `SIMSYS` environment variable, default `../build/gcc14-release/simsys`
(the precompute's default). Without a binary there the routes answer 503 with how to build it.

## 8. The page (`@simsys/app`, 6.5–6.7)

**Two modes**, chosen in the header and kept in the URL hash: *Watch* (5b, unchanged) and *Design*.

```
┌ challenges ┬ brief (folds) ──────────────┬ diagram (live) ───────────┬ charts ┐
│ 1 one-tier │ YAML pane                   │  boxes from explain        │ (5b)   │
│ 2 daily-pk │  line numbers, error marks  │  each with its machines    │        │
│ 3 read-hvy │                             │  and cost                  │        │
│ 4 url-shrt │ [Run] [Cancel] [Estimate at ├ bill: $2,310 · par $1,980 ─┤        │
│            │  peak] [Reset] [Reference]  │ time slider · zone strip   │        │
├────────────┴─────────────────────────────┴ verdict ───────────────────┴────────┤
```

- **The data path.** `LiveSource implements DatasetSource` (in `@simsys/stream`): a `fetch` POST
  whose response body is read line by line; `abort` cancels. The store already accepts a plain 5a
  stream (the `dataset` line is optional); 6.5 adds the tests that say so. Frames arrive over the
  run's seconds and the charts and the zone strip fill as they do (the store's Observer batches,
  as in 5b). When the `summary` arrives the slider plays the day like a watched one.
- **The live diagram.** Each edit, after 300 ms of quiet, goes to `/explain`. The answer's graph
  feeds 5b's `Diagram` unchanged, laid out by ELK (no `diagram.yaml` for a student's design). On an
  error the last good diagram stays, greyed, and the editor marks the line. CLAUDE.md §5's rule
  holds: nodes and edges are rebuilt only when the graph changes, not per frame.
- **The bill.** Under the diagram: one row per node (part, size, machines, dollars a month), the
  total, par. Each box shows its size, machines and cost where 5b shows its copies.
- **The verdict** (after a run), a pure function `verdict(store) → Verdict`:
  - *passes*: the cost against par (`$2,310 a month, par $1,980: $330 over`; `at par`; `$120 under
    par`), then the SLO table (5b's). An open build shows the cost alone, and the SLO table if the
    design has SLOs.
  - *fails*: the failed SLOs, then the **first bottleneck**: the node whose zone turned red in the
    earliest frame (ties: the one furthest downstream, since a saturated callee backs its callers
    up), with the time, its `peak_rho`, the demand against its capacity in the node's unit, and one
    sentence of why: *offered load above capacity: the queue grows until its room is full, then
    drops* (ρ ≥ 1), or *in the red zone but below 1: the wait grows steeply near saturation*
    (Erlang C). Clicking it selects the node and moves the slider there. Then the other red nodes
    in time order. A design that fails without any red node (a latency SLO missed by path length)
    gets the per-op latency against its limit instead.
- **Estimate at peak**: `/estimate`'s per-node ρ at the challenge's peak as a small table, the
  simulator's answer to the back-of-the-envelope sum the student did by hand, with the note that
  it is the steady state at one instant (schema, `estimate --at`). On request, never by itself.
- **Drafts.** The pane's text is kept in `localStorage` per challenge, with the best passing cost
  so far. *Reset* restores the starter. *Reference* shows the reference design read-only beside
  the student's, at any time: no confirmation and no lock (the student is their own referee).
- **Without a server** (the static 5b serving, a later hosted site): `/api/challenges` fails, the
  *Design* tab says it needs `npm run serve` on one's own machine, and *Watch* works as before.

## 9. The first four challenges

Graded so that each adds one idea. The numbers below are intentions; 6.3 fixes them by running the
references, and each README carries the arithmetic.

| # | id | The load | What it teaches | Parts |
|---|---|---|---|---|
| 1 | `one-tier` | steady 5k/s, one op, 10 minutes | ρ = λ/(c·μ); why 100 % utilization is not a design; replicas behind a load balancer (ch. 1) | lb, web |
| 2 | `daily-peak` | a sine day, peak 1.5× the mean, 24 h | size for the peak, not the mean; what the fleet idles at 04:00 (what cost ignores: autoscaling) | lb, web |
| 3 | `read-heavy` | reads 10 : 1 writes, Zipf keys | read replicas against a cache; a replica replays every write; Che's approximation for the hit ratio (ch. 1, ch. 6) | lb, web, cache, db |
| 4 | `url-shortener` | the book's ch. 8 numbers | the writes outgrow one primary: sharding; the rate limiter; the id generator | + limiter, id_gen |

**The open build** (`challenges/00-open-build`, `open: true`) is not a challenge but a workbench
for the student's own idea: a parts list with every node type the simulator runs, in three sizes
where size matters, each size at its own price, taken from the price list the challenges use (a
medium web server costs the same in the open build as in challenge 3); no given load, no par. The design
holds its own clients and, if wanted, SLOs and a duration (default 24 h at a 1 s step). The page
shows it first in the list; the verdict is the bill, the SLO table if there is one, and the
bottlenecks. Its starter is challenge 1's reference with a client, to have something that runs.

The reference of a challenge is the cheapest design its author found, not a proved minimum: with
three sizes there are many near-equal bills, and beating par is possible and welcome.

`scenarios/book/url-shortener` is challenge 4's reference, rewritten as a design over the parts.
The other seven book systems become challenges in a later phase (§11); the format makes one a
directory.

## 10. Tests (written first) and CI

- **C++ (Catch2)**, named after the property: *a design that restates a key its part fixes is
  refused at that key's line*; *composing the reference of each challenge gives a scenario equal to
  its hand-written twin*; *cost counts a sharded database's machines as replicas × shards*; *a
  given node costs nothing*; *a sized part without a size is refused, naming the sizes*; *a cache
  costs its keys × key_bytes in gigabytes*; *an open build takes the design's clients and has no
  par*; *without --challenge
  every output is byte for byte what main writes* (the 5b fixtures already guard the stream).
- **Each shipped challenge** (ctest, Release only for the 24 h ones): its reference passes every
  SLO and costs exactly `par`; its starter is refused or fails (it is not a solution); removing one
  machine from the reference's tightest tier fails (the reference is not padded there). The open
  build's starter runs.
- **Server (Vitest, a fake `simsys` script)**: an unknown id is 404 and spawns nothing; an
  oversized body is 413; a closed connection kills the child; the run's bytes are the child's; the
  temporary directory is gone after exit; the cap answers 429.
- **Stream**: `LiveSource` yields whole lines across chunk boundaries and stops on abort.
- **App**: `verdict` (pass at, over and under par; first bottleneck by time, then downstream; no
  red node; an open build without par); the bill's rows; the debounce; the draft store. Pure functions, no browser (§5).
- **CI**: the `web` job runs the new workspace's tests; the C++ jobs run the challenge tests.
  A fixture stream of challenge 1's reference is committed with the 5b fixtures and checked.

## 11. Later (not in phase 6)

The other seven book systems as challenges (queues and workers with a lag SLO, the chat system's
connection level, youtube's bytes). A drag-and-drop editor. Hints revealed step by step. Comparing
two of one's own designs. `sweep` as a tool inside a challenge. Autoscaling as a part, with a cost
that follows the wave. Challenges with a fault ("survive one database machine down"), which meets
4b, and with them a headroom rule: a `red_time` SLO (the seconds a node spent in its red zone). WebAssembly, so that a hosted site runs the simulator in the browser. 5c's change menu.

## 12. The review's answers (user, 2026-11-12)

1. **Three sizes per part**, not one fixed machine: more interesting (§4).
2. **No `red_time` for now.** The first game is the given traffic at the minimum cost; headroom
   comes later, with faults (§2, §11).
3. **The estimate at the peak on request** (the draft called it "the paper check", which was
   unclear): proposed again under its plain name, to confirm.
4. **The reference is visible at any time**, without a confirmation: simpler (§8).
5. **Four challenges, and an open build** to experiment with parts and cost on one's own idea (§9).
6. **Dollars** (§4).
