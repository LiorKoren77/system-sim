# Phase 4a: the sweep and the reports, finished

Design for sub-phase 4a of system-sim, drafted on 2026-11-09 after phase 3 closed (`b97cd0f`, 1686
tests, schema v1.5, spec §20). It takes the reporting and command-line items of the phase 4 backlog
(the phase 3 plan's last section, and the 2a leftovers it carries) and decides what 4a builds, in
what order, and the rules a scenario author will meet. Where it conflicts with an earlier section of
the main spec (`2026-10-27-system-sim-design.md`), this document wins; the implementation plan
(`docs/superpowers/plans/2026-11-09-phase4a-sweep-reporting.md`) turns it into tasks, and the schema
(v1.6) and a new spec §21 record what was finally built.

## 1. Goal

A design review asks three questions after `explain`: *how much of this do I need* (a sweep),
*does it hold its SLOs at that size* (the verdicts), and *what does it look like at the peak*
(`estimate` at a chosen time). Phase 2c answered the first for one key at evenly spaced points with
no verdicts; `estimate` answers the third only at midnight. After 4a:

- a sweep can print each point's SLO verdicts and exit code, measure a node's `reject_rate`, space
  its points geometrically, and cross two keys in a grid;
- `estimate --at 20:00` evaluates the steady state at any time of the day;
- `estimate`'s table and JSON stop presenting one step's worth of drops as a total, and JSON tells
  an unbounded queue lag from a node that has none;
- four small debts of phase 2a are paid (unit labels at a node with a `cost`, duplicate
  `write_ops`, `curve --seed` without `--noise`, `estimate`'s table shape).

No node model changes. Every number `run` prints for the fourteen shipped scenarios stays the same;
the only changes to their `run` output are the unit note of §7 (on the scenarios that have a
`cost` or a byte-sized node), and `estimate`'s table changes shape on all of them (§6).

Not in 4a (stays in the backlog): sweeps over seeds and replications per point (4b, with the
rest of the randomness), list values in a sweep, three or more keys, the diurnal table in the
schema (M10, 4b), every node-model item (4b–4e).

## 2. Decisions taken (2026-11-09, for review)

| Question | Decision | Why |
|---|---|---|
| Sweep verdicts | Opt-in `--slos`: one column per scenario SLO and an `exit` column per point. The process exit code stays 0/1 | A sweep explores; the interesting rows are the ones that fail. Exiting 2 because one point of ten fails would make every exploratory sweep "fail". Existing sweep output stays byte-identical |
| Geometric spacing | A suffix on the range: `1k/s..100k/s:5:log` | Keeps one `--param` grammar; `log` is what a plot axis calls it |
| Whole keys, geometric | Rounded to the nearest whole number; two points that round alike are refused | Linear points are whole or refused (the student picks n); geometric points are almost never whole, so refusing would make `:log` useless on `workers` |
| Two keys | `--param` given twice: a grid, the first key the outer loop. Three or more refused | A table of two keys is still readable (one row per pair); a third axis is a script's job |
| Grid size | The product of the two counts is at most 10,000, the one-key limit | Every point's graph is built before any runs (phase 2c's rule), so the limit bounds memory |
| `estimate --at` | A clock time or a duration, inside `[0, duration]`; noise and faults still left out | The same syntax as a fault's `at`; the steady state of the curve's value at that moment |
| `estimate` per second | JSON nodes gain `dropped_per_s` and `rejected_per_s` (in `run` too); `estimate`'s `dropped`, `rejected` and `red_s` become `null` | A steady state has rates, not totals; the backlog item asked for the per-second figure, and keeping the old key with a different meaning would be a silent change |
| Unbounded lag | JSON nodes gain `queue_lag_unbounded` (`true`/`false` on a message queue or frontier, `null` elsewhere) | `peak_queue_lag_s: null` keeps meaning "no lag here or unbounded"; the boolean tells them apart without breaking a reader of the old key |
| `curve --seed` without `--noise` | Refused | A flag that silently does nothing teaches the wrong thing; the same rule as `retry_after needs retries` |
| Duplicate `write_ops` | Refused, as a link's duplicate `ops` are | Consistency; a duplicate is always a typo |

The two `estimate` decisions change the JSON contract. The schema's version moves to **v1.6**, and its
"New in v1.6" list names every changed key.

## 3. Sub-steps and order

Each sub-step is one task in the plan: a red commit with the tests, a green commit, a review, a fix
round. The order puts the smallest, self-contained changes first, so the branch-and-PR workflow is
tried on small diffs, and puts the two output-shape changes (§6, §7), which touch the most tests,
last.

| Step | Content | Files mostly touched |
|---|---|---|
| 4a.1 | M9 duplicate `write_ops`; M13 `curve --seed` without `--noise` | `config/nodes.cpp`, `cli/main.cpp` |
| 4a.2 | `<node>.reject_rate` sweep metric | `sweep/sweep.cpp`, `.cppm` |
| 4a.3 | geometric spacing `:log` | `sweep/sweep.cpp` |
| 4a.4 | two-key grid | `sweep/sweep.cpp`, `.cppm`, `cli/main.cpp` |
| 4a.5 | `--slos`: verdict and exit columns | `sweep/sweep.cpp`, `.cppm`, `cli/main.cpp` |
| 4a.6 | `estimate --at` | `cli/main.cpp`, `engine/engine.cppm`, `session/session.cpp` |
| 4a.7 | `estimate`'s shape: per-second JSON, table (M12), `queue_lag_unbounded` | `report/report.cpp`, `.cppm` |
| 4a.8 | M7 unit labels | `report/report.cpp`, `explain.cpp` |
| 4a.9 | docs: schema v1.6, spec §21, READMEs, study notes; the 4a review; PR | `docs/`, `README.md` |

## 4. Sweep

### 4.1 `<node>.reject_rate`

`<node>.reject_rate`: the share of the work sent to the node that its behaviour refused (a rate
limiter's 429s, a gateway's refused connects, a leaky bucket's overflow), the node's own
`reject_rate`: `report::Summary::reject_rate(node)`, the number `run --report json` prints as the
node's `reject_rate` and the `reject_rate` SLO observes. A ratio, printed to three significant
digits in the table, like `drop_rate`. Refused on a client in the words of `drop_rate`'s refusal:
`--metric: users.reject_rate: 'users' is a client, which serves nothing, so it has no reject rate`.
One row in `node_metric_words` (Pattern: Table-driven method, already named there).

The rate-limiter lab's question, "which `burst` keeps the api out of the red without refusing too
much?", becomes one sweep:

```
simsys sweep scenarios/labs/rate-limiter/scenario.yaml --param nodes.limiter.burst=100..500:5 \
    --metric limiter.reject_rate --metric api.peak_rho
```

### 4.2 Geometric spacing

`<from>..<to>:<n>:log` gives `n` points with a constant ratio, both ends included:
`x_k = from × (to / from)^(k / (n − 1))`, the last point written as `to` itself (no rounding drift).
Both ends must be positive (`--param: 0/s..1k/s:4:log: geometric points need two positive ends`).
`1k/s..100k/s:3:log` is 1k/s, 10k/s and 100k/s. A key the loader reads as a whole number gets each
point rounded to the nearest whole number, and two points that round to the same one are refused:
`--param: workers=1..4:8:log gives 1 twice (rounded to whole workers); use fewer points`. Any other
suffix is refused: `--param: ...:5:lin: the only spacing suffix is :log`.

Why: the knee of the latency curve (the hello tutorial's `s / (1 − rho)`) and a cache's hit ratio
against its size (Che) are both read on a log axis; evenly spaced points over 1k/s..1M/s spend
nine tenths of the points above 100k/s.

### 4.3 Two keys: a grid

`--param` may be given twice. The points are every pair, the first `--param` the outer loop:
`--param a=1..2:2 --param b=10..30:3` runs (1,10) (1,20) (1,30) (2,10) (2,20) (2,30). The table
gets one column per key, then the metrics; the CSV the same header. Refused:

- a third `--param`: `--param: at most two keys (a grid of two); script a third`;
- the same key twice, as written or through a YAML anchor that both paths reach:
  `--param: nodes.api.capacity is given twice`;
- a grid of more than 10,000 points: `--param: 200 × 100 = 20000 points: at most 10000`.

Every point is loaded with both overrides applied (`config::load(path, overrides)` already takes a
span of them) and built before any runs. Errors name the point by both values:
`point 4 (nodes.api.replicas=2, nodes.users.rate.base=900/s): ...`.

C++23 `std::views::cartesian_product` over the two value lists produces the pairs. The fifth GCC
defect (CLAUDE.md §3) concerns a lambda passed to a range adaptor in a module function;
`cartesian_product` takes none, and any projection the code needs goes in a named `detail::`
function.

### 4.4 `--slos`: verdict columns

`--slos` appends, after the metrics, one column per SLO of the scenario, in file order, and an
`exit` column. Each SLO is judged at each point exactly as `simsys run` judges it
(`report::evaluate_slos` on the point's summary, windows and the C-1 timeout rule included):

| format | SLO cell | `exit` cell |
|---|---|---|
| `table` | `PASS`, `FAIL`, `-` (skipped) | `0` or `2`, the point's `simsys run` exit code |
| `csv` | `pass`, `fail`, `skipped` | `0` or `2` |

A column is headed by the SLO's metric and, where it has them, its node and op:
`p99_latency:users.read`, `drop_rate`, `utilization:url_db`. Two SLOs that would get the same
heading take `#2`, `#3` in file order. A scenario without SLOs gets only the `exit` column (all 0),
and the CLI says so on stderr: `the scenario has no SLOs: every point exits 0`. The process exit code
stays 0 (every point ran) or 1 (an error) with or without `--slos` (§2).

The youtube example of the schema then reads its own SLO:

```
$ simsys sweep scenarios/book/youtube/scenario.yaml --param nodes.transcoders.workers=400..600:3 \
      --metric transcode_q.lag --slos
nodes.transcoders.workers  transcode_q.lag  p99_latency:viewers_eu.stream  ...  queue_lag:transcode_q  exit
400                                  2.66h                           PASS  ...                   FAIL     2
...
```

(the README's numbers are *measured* in the plan, not designed here).

## 5. `estimate --at`

`simsys estimate <file> --at <time>`: `<time>` is a clock time (`20:00`) or a duration from the
run's start (`90m`), the syntax of a fault's `at`, and must lie in `[0, duration]`
(`--at: 25:00 must fall inside the run (24h)`). The engine already settles at a given instant
(`Simulation::settle(at)`, called today with 0); `estimate(at)` passes it through. The clients
offer their curve's value at `at` (wave, ramp, points, active spikes), without noise; faults are still
left out, even one active at `at` (the steady state without the faults, as today). The table's first
line names the instant: `estimate at t = 20:00: the curve without noise; explain's checks use the
peak`; JSON gains `"at_s": 72000` (0 without `--at`).

Why: a diurnal scenario's midnight is its trough; url-shortener's database at 00:00 says nothing
about 13:00. `--at` makes `estimate` the instant version of a windowed SLO.

## 6. `estimate`'s shape (M12, the per-second backlog item, `queue_lag_unbounded`)

**JSON**, every node object, `run` and `estimate` alike:

| key | `run` | `estimate` |
|---|---|---|
| `dropped`, `rejected` | totals over the observed steps (unchanged) | `null` (a steady state has no total) |
| `red_s` | seconds in red (unchanged) | `null` |
| `dropped_per_s`, `rejected_per_s` | new: the totals over the observed seconds | new: the steady rates |
| `queue_lag_unbounded` | new: `false` on a message queue or frontier (a run's lag is finite), `null` elsewhere | new: `true` where the lag is unbounded (`peak_queue_lag_s` is then `null`), `false` on other queues, `null` elsewhere |

**Table** (`estimate` only): the summary line says `steady state at 00:00` (or the `--at` time)
instead of `1 steps observed`; the node table's `dropped` and `rejected` columns become `dropped/s`
and `rejected/s`, and `red_s` goes. `run`'s table is unchanged.

The replication JSON's per-seed node objects gain the same keys (`run` semantics).

## 7. Unit labels (M7)

A node whose work is not counted in requests (a `cost` other than 1 on some op, a SQL database's
default write cost 5, a kv_cluster's N replica-requests, a byte-sized node's megabytes) reports
its `peak_backlog` and `dropped` in its own unit. `run`'s and `estimate`'s tables add one line
under the node table listing those nodes and their unit, and only when some node needs it:

```
units: url_db counts capacity units (write cost 5); cdn_us, transcoded_store count MB
```

`explain` already prints `units/s` and `MB/s` in its checks (phase 2c), so it does not change. The
comments in `report.cppm` that say "requests" where the value is capacity units are corrected.
JSON is unchanged (the schema's key table already gives each key's unit).

## 8. GCC 14.2 defects this sub-phase can meet

| Defect (CLAUDE.md §3) | Where 4a could hit it | Precaution |
|---|---|---|
| 1st, 'Bad file data' | a new include in `sweep.cppm` or `report.cppm` | the interfaces stay lean: new headers go in `sweep.cpp`, `report.cpp` |
| 2nd, `std::format` width at run time | the new columns (`exit`, SLO cells, `dropped/s`) | no width or precision specs: `report::detail`'s padded table and `to_chars_text`, as today |
| 4th, missing special members (Release) | a new `std::vector<SweepParam>` or `std::optional` member crossing a module boundary | build Release after each task; one `emit_special_members` line in `model.cpp` (or a sibling in `sweep.cpp`) per new undefined reference |
| 5th, lambda in a range adaptor | the grid's pairs and the SLO headings | `cartesian_product` takes no lambda; any `transform`/`filter` gets a named `detail::` function |
| 7th, default argument naming an exported inline variable | a new `--at` default or a `max_points` constant | exported constants `constexpr` without `inline` |
| 8th, header after `import` | test files that gain `<cmath>` or `<ranges>` for the new tests | every include before the first `import` |
| 11th, sanitizer ICE | none expected (no new interface inline bodies with shifts or division) | the `gcc14-asan` preset runs before every push |

## 9. Tests to write first

- **Loader / CLI (4a.1):** `write_ops: [write, write]` → `node 'c': write_ops names 'write' twice`
  on a cache, a cdn, a sql_db, a blob store, a kv_cluster; `simsys curve f c --seed 3` without
  `--noise` → exit 1 and `--seed needs --noise (without it the curve has no randomness)`; with
  `--noise` unchanged.
- **reject_rate (4a.2):** on the rate-limiter lab's limiter equals `run --report json`'s `reject_rate`
  digit for digit; refused on a client; a service that refuses nothing gives 0.
- **Geometric (4a.3):** `1k/s..100k/s:3:log` gives `1k/s, 10k/s, 100k/s` exactly as text; the
  last point is `to`; a non-positive end refused; `workers=1..4:8:log` refused with the duplicate;
  `workers=10..1000:3:log` gives 10, 100, 1000; `:lin` refused.
- **Grid (4a.4):** row order (outer, inner); a third `--param`, a repeated key, a key repeated through
  an anchor and 20,000 points refused with their messages; the error names both values; output
  byte-identical for `--jobs 1` and `--jobs 8`; a one-key sweep's output byte-identical to today.
- **SLO columns (4a.5):** each cell equals `simsys run` on the same overridden file (a test runs both
  on hello with a p99 SLO the sweep crosses: PASS, PASS, FAIL); `exit` equals `run`'s exit code at
  every point; `#2` naming; a scenario without SLOs; process exit 0 with failing points; without
  `--slos` byte-identical to today.
- **`--at` (4a.6, the numbers in a theory test; the wave's exact form is checked against the
  schema's rate-curve section when the test is written):** a sine wave of amplitude 0.5 peaking at 12:00 over
  base 1k/s into an M/M/1 of 2k/s: `--at 12:00` offers **1.5k/s**, rho **0.75**, mean wait
  **s × rho / (1 − rho) = 1.5 ms** at s = 0.5 ms; `--at 00:00` equals `estimate` without `--at`
  byte for byte; a spike active at `at` multiplies; outside `[0, duration]` refused; JSON `at_s`.
- **Shape (4a.7):** `estimate --report json`'s `dropped_per_s` equals `run`'s on a steady
  overloaded probe (a 1k/s service sent 1.5k/s with a room of 100: **500/s** both ways once the
  room is full); `dropped`, `rejected`, `red_s` `null` in `estimate`; `queue_lag_unbounded` `true`
  on web-crawler's frontier in `estimate`, `false` in `run`, `null` on a service; the estimate
  table's headers.
- **Units (4a.8):** the `units:` line on url-shortener (a database with write cost 5) and youtube
  (MB), absent on hello; byte-identical `run` output otherwise on all fourteen scenarios.

## 10. Report and schema additions

- Schema **v1.6**: the sweep section (`reject_rate`, `:log`, the grid, `--slos`), the `estimate`
  section (`--at`, the table shape), the JSON key table (`dropped_per_s`, `rejected_per_s`,
  `queue_lag_unbounded`, `at_s`, `null` totals in `estimate`), `write_ops` duplicates, `curve
  --seed`. "New in v1.6" lists each JSON change first, since they can break a reader.
- Spec **§21**: the rules as built, with each review's corrections, in §20's style.
- READMEs: the rate-limiter lab gains its `burst` sweep with `reject_rate`; youtube's sweep gains
  `--slos`; url-shortener gains an `estimate --at 13:00` example (its windowed SLO's worst hour); the
  hello tutorial's sweep may move to `:log`. Every number is quoted from the Release binary.
- Study notes: `ranges.md` gains `std::views::cartesian_product` (C++23) with the sweep grid as its
  use; anything else the tasks meet first gets its first-use comment and, if longer, a note.

## 11. What 4a deliberately leaves

Sweeps over seeds and replications per point (4b), list values (a sweep over `[a, b, c]` names),
three keys, adaptive search (find the rate at which p99 crosses a bound), interpolation between
points, a per-point `estimate` instead of `run` (`sweep --estimate`), a JSON format for sweeps.
