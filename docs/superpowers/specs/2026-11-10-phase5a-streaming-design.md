# Phase 5a: the simulator as a streaming data source

Design for sub-phase 5a of system-sim, drafted on 2026-11-10 after phase 4a merged (`4549e46`, PR #2,
1740 tests, schema v1.6, spec §21). Phase 4b (random faults, random parameters) is postponed: the
next goal is to *see* a simulated system run in a browser (5b). 5a is the C++ half of that: `simsys`
becomes the data source a web server runs as a child process and reads as it goes. Where this
document conflicts with an earlier section of the main spec (`2026-10-27-system-sim-design.md`),
it wins; the implementation plan (`docs/superpowers/plans/2026-11-10-phase5a-streaming.md`) turns it
into tasks, and the schema (v1.7) and a new spec §22 record what was finally built.

## 1. Goal

The product of phase 5 (5a + 5b): a user picks one of the shipped scenarios in a web page and
**watches it run**: the system's diagram, each node turning yellow and red as the day's wave rises,
a time slider, per-node charts, the SLO table and the fault blocks at the end. Later phases let the
user **change the system** (add, replicate or remove a node, cause a failure now) and watch the
consequence; 5a must not make that later step a rewrite.

The architecture, agreed on 2026-11-10:

```
browser (React, 5b)  <--WebSocket/SSE--  Node server (5b)  <--stdout, one JSON per line--  simsys (5a)
                                                         --spawn: simsys run file --stream-->
```

No networking in C++: `simsys` stays a command-line program with no new third-party dependency.
The engine does not change.

The problem 5a solves is **volume**. `run --series` writes one row per node per step. youtube (24 h
at 100 ms: 864,000 steps, 14 nodes and a bandwidth link, 8 client ops) is about 13M node rows and
7M op rows, several hundred megabytes, to draw a chart a screen shows as at most a couple of thousand
points. Thinning the file afterwards (in Node) would still make the simulator write and the server
read all of it. 5a **aggregates at the source**: the engine still steps at the scenario's `step`
(accuracy is decided there, spec §19 entry 16), and only what is written is reduced, to one frame
per *bucket* of steps, about 500–1000 frames per run, each a few kilobytes. A frame leaves as soon
as its bucket closes, so the browser starts drawing within a fraction of a second of the start, and
a 27 s youtube run (Release, measured 2026-11-10; chat-system 60 s, url-shortener 0.6 s) fills the
page progressively instead of after a wait.

After 5a:

- `simsys run <file> --stream` writes the run as newline-delimited JSON (NDJSON) on stdout: a `run`
  message, the `graph`, a `frame` per bucket, and a `summary` that is `--report json`'s report;
  or an `error` message, with file, line and column when the scenario is at fault;
- `--points N` (default 720) or `--every <duration>` choose the bucket;
- `simsys explain --report json` prints the same `graph` object on its own;
- every JSON `simsys` writes carries a `format_version`, the contract the frontend is written against.

No node model changes, and every existing output (`run`, `estimate`, `sweep`, `--series`) stays
byte-identical except the one new first key of `--report json` (§5).

## 2. Decisions taken (2026-11-10, for review)

| Question | Decision | Why |
|---|---|---|
| Where the service boundary is | `simsys` is a child process of the Node server; it streams on stdout. No C++ HTTP/WebSocket server | Agreed with the user. Keeps C++ free of a networking library (more GCC 14.2 module defects, more dependencies); the Node server owns sessions, sockets and browsers |
| Wire format | NDJSON: one JSON object per line, each with a `type` | Line-framed, so the reader splits on `\n` and parses each line on its own; a process killed mid-write leaves at most one partial last line, which the reader drops. Readable with `head` and `jq` when teaching |
| Where to reduce the data | In the C++ process, before writing (a bucket aggregator, an Observer beside Summary) | Writing, piping and parsing 20M rows to keep 1000 is the cost we want to avoid; the aggregation rules (means, peaks, levels) are part of the model's meaning and belong next to it |
| Bucket size | `--points N` (default 720: two minutes of a 24 h day) or `--every <duration>`; a whole number of steps either way | Points is what a chart needs whatever the scenario's length (a 1 min tutorial and a 24 h day both fill the page); `--every` is what a reader of the series asks for. 720 keeps the 24 h days at about 1 MB (§6.4) |
| Which numbers a frame keeps | Rates as the bucket's mean, peaks as its max, levels as the bucket's last value, the zone as its worst | Conservation still balances over a bucket (§6.2); a 1 min fault inside a 2 min bucket still shows red (peak, worst zone, `red_s`) though its mean is diluted |
| Frame layout | Rows of numbers in a column order the `graph` message declares | Half the bytes of keyed objects; a new column is an addition to the declared list, not a new frame shape |
| Pacing | None: frames are written as fast as the engine makes them, flushed one by one | Replay speed is the frontend's choice (it has the frames); a sleeping simulator only makes the server wait |
| Interaction in 5a | None: the user watches. `simsys` reads nothing from stdin; the server stops a run by ending the process | Agreed with the user (5a, 5b watch only). §8 fixes what 5a reserves so that editing and live faults come without breaking the protocol |
| How the server gives a scenario | A file path, as today. An edited system (later) is written by the server as YAML to a file of its own | The YAML loader is the only door into the engine: every rule it checks applies to an edit too, and an edited system can be saved, diffed and replayed with the CLI |
| Errors | An `error` message on stdout (file, line, column, message) and exit 1; never a half stream without a reason | The frontend shows the message at its place; a later editor points at the field |
| Versioning | `format_version: 1` in every JSON document `simsys` writes (`run`, `estimate`, `explain`, the stream's `run` message) | One number for the frontend to check. Adding a key or a message type keeps the version; renaming, removing or changing a meaning raises it (§7) |
| What `--stream` combines with | `--seed`, `--duration`. Refused with `--report`, `--series`, `--series-ops`, `--replications` | The stream carries its own report; a series file beside it is the old path. Replications stream N runs: not needed to watch one |

## 3. Sub-steps and order

Each sub-step is one task in the plan: a red commit with the tests, a green commit, a review, a fix
round (4a's rhythm). The order builds the pieces the stream is made of before the stream itself.

| Step | Content | Files mostly touched |
|---|---|---|
| 5a.1 | `format_version` in `run` / `estimate` JSON; a one-line rendering of the report (the stream's `summary`) | `report/report.cpp`, `.cppm` |
| 5a.2 | `explain --report json`: the `graph` object (§6.1) | `report/explain.cpp`, `report.cppm`, `cli/main.cpp` |
| 5a.3 | the bucket aggregator for nodes and links: the rules of §6.2, conservation | new `report/stream.cpp` (a unit of simsys.report), `report.cppm` |
| 5a.4 | the aggregator for client ops: means, the bucket's p99 from pooled moments | `report/stream.cpp`, `report/shared.cppm` (the moments and the percentile rule move there) |
| 5a.5 | `run --stream`: the writer, `--points` / `--every`, the refusals, `error` messages, the `summary` | `report/stream.cpp`, `cli/main.cpp`, `session/session.cppm` |
| 5a.6 | docs: schema v1.7 (the stream), spec §22, study notes, README; ASan; the 5a review; PR | `docs/`, `README.md` |

## 4. Command line

```
simsys run <file> --stream [--points N | --every <duration>] [--seed N] [--duration <d>]
simsys explain <file> --report json
```

- `--points N`: at most N frames, 1 to 100,000; the bucket is `ceil(steps / N)` steps, so a run
  shorter than N steps gets one frame per step. Default 720.
- `--every <duration>`: the bucket's length, a whole multiple of the scenario's `step`
  (`--every 250ms: not a whole number of 100ms steps`) and not longer than the run.
- Both given: `--points and --every both set the bucket; give one`. Either without `--stream`:
  `--points needs --stream`.
- With `--report`, `--series`, `--series-ops`, `--replications`:
  `--stream carries the report and the series; drop --series` (the flag named).
- Exit codes as `run`'s: 0 every SLO passed, 2 one failed, 1 an error. With `--stream` an error is
  also an `error` message (§6.5) and nothing goes to stderr; stdout carries nothing but NDJSON.

## 5. `format_version`

`"format_version": 1` becomes the first key of `run --report json`, `estimate --report json` and
`run --replications N --report json`, and of every document 5a adds. It is the version of the
*output* contract, apart from the scenario schema's version (v1.7, the language a scenario is
written in, which also documents the output). The CSV series keep their header row as their contract
and gain no version (a CSV has no place for one; the frontend reads the stream).

## 6. The stream

One message per line. Every number is written as the JSON report writes it today
(`report::json_number`, `std::to_chars` shortest form, six significant digits); a value that is not
finite (an unbounded lag, a path that never answers) is `null`. Each line is written with one
`write` and flushed, so the reader sees whole lines as soon as they are made.

The order is fixed: `run`, `graph`, then `frame`s in time order, then `summary` (or `error` at any
point, after which nothing follows).

### 6.1 `run` and `graph`

```json
{"type":"run","format_version":1,"scenario":"youtube","seed":1,"duration_s":86400,"step_s":0.1,"warmup_s":300,"bucket_s":120,"frames":720}
{"type":"graph","rev":0,"datacenters":[...],"nodes":[...],"links":[...],"ops":[...],"slos":[...],"faults":[...],"node_columns":[...],"link_columns":[...],"op_columns":[...]}
```

`graph` is the resolved scenario, what `explain` prints, as data: the diagram is drawn from it, so
the frontend never parses YAML or re-derives a default.

- `datacenters`: name, and the intra and cross round trips.
- `nodes`, in the engine's order (the order every frame's rows follow): `id`, `type`, `datacenter`,
  `replicas`, the total `capacity_per_s` and its `unit` (`requests`, `units` for a node with a
  `cost`, `MB` for a byte-sized node, `replica-requests` for a kv_cluster, §21's unit note),
  `yellow`, `red`, `queue_limit`; `null` on a client. Type-specific keys only where the diagram needs
  them (a worker pool's `workers`, a gateway's connection cap); the full provenance stays in
  `explain`'s text.
- `links`: `from`, `to`, `direction`, `ops`, `one_way_s`, `bandwidth_mb_s` (or `null`); a link with
  a bandwidth also has a row in every frame (`links` below), in this list's order among those that do.
- `ops`: `client`, `op`, its `share` of the client's mix, in the order of every frame's op rows.
- `slos`, `faults`: as the scenario states them (a fault's `kind`, `target`, `at_s`, `for_s`), so the
  time slider marks the faults before they happen.
- `node_columns`, `link_columns`, `op_columns`: the names of each row's numbers, in order (§6.2).

`rev` is the graph's revision: 0 for the scenario as loaded. 5a never writes another; §8 says what a
later one means. `simsys explain <file> --report json` prints this object, without `type` and `rev`,
with `format_version` first.

### 6.2 `frame`

```json
{"type":"frame","rev":0,"t_s":7200,"dt_s":120,"nodes":[[...],[...]],"links":[[...]],"ops":[[...]]}
```

A bucket is the steps from `t_s` to `t_s + dt_s`; `dt_s` is the bucket's length, shorter for the
last bucket when the steps do not divide evenly. Frames cover the whole run from t = 0, the warmup
included (the frontend shades `warmup_s`; the summary, as today, observes only after it).

A node row, its columns as `node_columns` lists them:

| column | in the bucket | rule |
|---|---|---|
| `demand_per_s`, `drain_per_s`, `dropped_per_s`, `rejected_per_s` | mean | the rates' time average (equal steps: the arithmetic mean) |
| `backlog` | last | the level at the bucket's end |
| `peak_backlog` | max | |
| `mean_rho` | mean | |
| `peak_rho` | max | what the colour of a short overload needs |
| `zone` | worst | 0 green, 1 yellow, 2 red (`graph` gives the names); red if any step was red |
| `red_s` | sum | seconds of the bucket spent in red |
| `latency_ms` | drain-weighted mean | the sojourn a request served in the bucket saw; `null` if any step's was infinite |
| `queue_lag_s` | max | message queue and frontier; `null` elsewhere and when unbounded |
| `connections` | last | a gateway's open connections, a level; `null` elsewhere |
| `dead_lettered_per_s`, `deduplicated_per_s` | mean | a worker pool's, a fan-out's; `null` elsewhere |

A client's row is all `null` except the rates it offers (a source has no queue: today's series leaves
those cells empty). A link row: the node columns that `--series` gives a link, in MB.

**Conservation.** The fluid queue's balance per step (`demand·dt = (drain + dropped)·dt + Δbacklog`,
spec §3.3 and `engine::NodeSample`) sums over a bucket's steps into the same balance for the bucket,
with the means and the bucket's ends:
`mean_demand · dt_s = (mean_drain + mean_dropped) · dt_s + backlog(end) − backlog(start)`, where
backlog(start) is the previous frame's `backlog` (0 before the first). That is why the rates are
means and the backlog is a level, and why the frontend can still check the law. A max of the rates
would break it, a mean of the backlog would too.

An op row, its columns as `op_columns` lists them: `offered_per_s`, `attempts_per_s`,
`answered_per_s`, `timed_out_per_s` (means); `mean_ms` (the answered-weighted mean response);
`p99_ms`, the 99th percentile of the bucket's answered requests, estimated as the run's percentiles
are: a lognormal fit to the response moments pooled over the bucket's steps (spec §3.5; the run's
Summary pools them the same way over the whole run), `null` when nothing was answered. Like every
percentile of the project it is labelled estimated (`graph`'s `op_columns` entry says so).

Pattern: Observer — the aggregator is one more observer of `engine::StepRecord`, beside the Summary
and the CSV writers (`session::RunObservers`); it hands each finished bucket to a sink, so the
aggregation rules are tested without any output. It lives in simsys.report, as a new implementation
unit `src/report/stream.cpp`, not in a module of its own: its ops need the latency mixture and the
percentile rule the Summary uses (one rule for a run's p99 and a bucket's), which are the module's
own, and the frame's JSON needs the report's number formatting.

### 6.3 `summary`

`{"type":"summary","exit_code":0,"report":{...}}`: `report` is exactly the object `run --report json`
prints for the same file and seed (with `format_version`), on one line: SLO verdicts, the node, link
and op totals of the observed run, the fault blocks. Nothing is computed twice: the summary and the
frames observe the same records.

### 6.4 Size and speed (to be measured in the plan)

A youtube frame is 15 rows of 15 numbers and 8 op rows of 6, about 2 KB; 720 frames are about 1.5
MB for the day, against several hundred MB of `--series` and `--series-ops`, and the first frame
leaves after the first 1,200 steps (a few tens of milliseconds). The aggregator must not slow a run:
the plan measures youtube's 27 s with and without `--stream` and allows 5 %.

### 6.5 `error`

`{"type":"error","file":"x.yaml","line":12,"column":5,"message":"..."}`: a scenario the loader
refuses (its `config::ConfigError`: line and column 0 when unknown), the graph builder or the engine
refuses (`file`, line 0), a bad flag (`file` null). Exit 1. A programmer error (an exception)
inside a streamed run is caught in `main`, written as an `error` with the exception's text, and exits
1: the server always learns why a stream ended early. A stream that ends without a `summary` or an
`error` was killed.

## 7. The contract's rules

These are written into the schema (v1.7) as the frontend's guarantee:

1. A reader ignores a message `type` it does not know and a key it does not know.
2. Adding a message type, a key or a column (at the end of a `*_columns` list) keeps `format_version`.
3. Removing or renaming any of them, reordering columns, or changing a value's meaning or unit raises
   it, and the schema's "New in" list names the change.
4. Rows refer to nodes, links and ops by their position in the latest `graph`; the `graph`'s ids are
   the scenario's ids, stable across runs of the same file.
5. Same file, same seed, same `--points`: the same stream, byte for byte (the engine's determinism,
   spec §3.6, carried through).

## 8. Built for change later, not in 5a

The user will later add, replicate and remove nodes and cause failures while watching. 5a builds none
of it, but fixes these so that it fits without breaking §7:

- **The scenario stays the input.** An edit made in the browser becomes a scenario YAML the server
  writes and `simsys` loads; the loader's rules, the schema and `explain` cover edits for free, and a
  user's system can be shared as a file.
- **A run is the scenario plus a log of commands.** A live change (take `db` down now, add a replica)
  will arrive as a command on stdin, NDJSON as well (stdin is reserved for that: 5a reads nothing
  from it), take effect at the next bucket boundary, and be echoed as a message in the stream at the
  time it applied. A scripted fault at that time in the YAML gives the same run, so a session's
  record (scenario + echoed commands) replays byte for byte: determinism survives interaction.
- **The graph can change mid-run.** A topology change writes a new `graph` with `rev` + 1; frames
  carry the `rev` their rows follow. 5a always writes `rev` 0, and a 5b reader that already keys its
  diagram by `rev` needs nothing new later.
- **Buckets are the unit of time the UI sees**, and so the points where state can be changed and
  inspected. What the engine needs for a mid-run change (rebuilding a Simulation from a changed graph
  and carrying the queues' state across) is the hard part of that later phase and is not designed here.

## 9. GCC 14.2 defects this sub-phase can meet

| Defect (CLAUDE.md §3) | Where 5a could hit it | Precaution |
|---|---|---|
| 1st, 'Bad file data' | new declarations in `report.cppm` | the interface stays lean: no new header there; the code in `stream.cpp` |
| 2nd, `std::format` width at run time | the frame's numbers | `json_number` (`to_chars`), never a format spec |
| 4th / 10th, special members (Release) | the aggregator's `std::vector` members crossing the module boundary | build Release after each task; out-of-class defaulted special members, or an `emit_special_members` line |
| 5th, lambda in a range adaptor | building rows | named `detail::` functions, no lambda in a view |
| 8th, header after `import` | the tests that parse JSON | every include before the first `import` |
| 11th, sanitizer ICE | none expected | `gcc14-asan` before the push |

## 10. Tests to write first

- **Version (5a.1):** `format_version` 1 is the first key of `run`, `estimate`, replications JSON;
  the rest of each document byte-identical to `main`; the one-line rendering parses to the same
  values as the pretty one.
- **Graph (5a.2):** chain's `graph`: three nodes in `order`, api's `capacity_per_s` 2000 (2 × 1k/s),
  the two links; youtube's `cdn_us` in `MB`, the origin link's bandwidth; url-shortener's database
  in `units`; ids in the engine's order; `explain --report json` and the stream's `graph` the same object.
- **Aggregation (5a.3, theory):** a bucket of one step equals `--series`'s row digit for digit, on
  every shipped scenario's first minute; on the hello probe overloaded (a 1k/s service sent 1.5k/s,
  room 100), the bucketed balance `mean_demand · dt = (mean_drain + mean_dropped) · dt + Δbacklog`
  holds to 1e-9 relative in every frame; `peak_rho` is the max and `mean_rho` the mean of the
  steps'; a bucket with one red step is `zone` 2 with `red_s` = one step; `connections` is the last
  step's on chat-system; a last, partial bucket has its own `dt_s`.
- **Ops (5a.4):** a bucket of one step equals `--series-ops`; a constant-rate run's bucket p99 equals
  the run's p99 (pooled moments of identical steps); `null` when nothing was answered.
- **Stream (5a.5):** every line parses (yaml-cpp reads JSON); the order `run`, `graph`, frames,
  `summary`; `--points 720` on youtube gives 720 frames, `--points 1000` on hello's 600 steps gives
  600; `--every 30s` gives `ceil(duration / 30s)`; `--every 250ms` on a 100 ms step refused;
  every refusal of §4; a broken scenario gives one `error` line with its line and column and exit 1;
  `summary.report` equals `run --report json` (parsed) and `exit_code` equals `run`'s exit code on
  unique-id (exit 2); the same seed twice gives byte-identical streams.
- **Speed (5a.5, Release, not in ctest):** youtube with `--stream` within 5 % of without; the
  stream's size recorded in the plan.

## 11. Docs

- Schema **v1.7**: a "Command line: `simsys run --stream`" section with every message, the column
  tables of §6.2, the rules of §7; `explain --report json`; `format_version`. "New in v1.7" lists the
  new first key of the JSON reports first.
- Spec **§22**: the rules as built, with each review's corrections.
- README: the stream in Usage, with `head -3` of a hello stream.
- Study notes: the first uses 5a meets get their inline comments (likely `std::to_underlying` for the
  zone's number); `docs/study/` gains a note on NDJSON and line-framed streams only if the code needs
  more than its comments.

## 12. What 5a deliberately leaves

The Node server and the browser (5b). Commands on stdin and mid-run changes (§8; after 5b).
`--series-every` (a bucketed CSV: the aggregator makes it easy, nobody needs it while the stream
exists). `validate --report json` (the stream's `error` covers a run; an editor will want it).
Streaming `--replications` or a sweep. A per-step stream (`--points` up to the number of steps gives
one already, for a short scenario). Compression (the server or the browser's transport can add it).
