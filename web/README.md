# The web app

A page on localhost where one picks a shipped scenario and watches its day: the system's diagram
turning yellow and red, a looping time slider, a zone strip of the whole day, charts of the
selected box, the SLO table and the fault blocks. Watch-only: nothing is simulated in the browser
or on a server; every dataset is precomputed by `simsys` and served as a static file.

```
npm ci
npm run precompute -- --simsys ../build/gcc14-release/simsys   # every scenario, about 3 min
npm run precompute -- --only book/youtube,labs/kv-store         # some, merged into index.json
npm run serve      # vite build + vite preview: http://localhost:4173
npm run dev        # vite's dev server with hot reload, the same datasets
npm test           # Vitest, every workspace
npm run check      # tsc, ESLint, Prettier --check: what CI runs
npm run fixtures [-- --check]   # rewrite the test fixtures from simsys, or compare with them
```

Node 22. The design is `docs/superpowers/specs/2026-11-10-phase5b-web-mvp-design.md`, the rules as
built spec §23 of `docs/superpowers/specs/2026-10-27-system-sim-design.md`, the file formats
`docs/scenario-schema.md` ("The web app's datasets").

## The data path

```
simsys run <file> --stream [--days 2 --every 1m]      (C++, offline)
   │  lines of JSON: run, graph, a frame per bucket, summary
   ▼
@simsys/precompute   a day: the seam check, keep day 2, loop it; gzip; index.json
   │  app/public/datasets/*.ndjson.gz
   ▼
DatasetSource        Pattern: Strategy. FileSource (fetch + DecompressionStream), MemorySource (tests)
   │  lines
   ▼
parseLine            the output contract: unknown types and keys ignored, versions checked
   ▼
DatasetStore         Pattern: Observer. The frames as columns, one Float64Array per column
   │  useSyncExternalStore
   ▼
the views            catalogue, diagram, time bar, zone strip, charts, SLO table, fault blocks
```

Three workspaces: `stream/` (`@simsys/stream`: the contract's types, the parser, the sources, the
store), `precompute/` (`@simsys/precompute`: runs simsys, cuts the window, writes the datasets),
`app/` (`@simsys/app`: the page, React + Vite).

**A day loops because it repeats.** A 24 h scenario is run for two days (`run --days 2`): day 1 is
the warmup, day 2 what the system settles into. The engine is deterministic and every day's input
the same, so if the frame ending at 48:00 equals the one ending at 24:00, day 3 would replay day 2:
looping it is faithful. The precompute checks that seam (relative tolerance 1e-3, plus 1e-6 of the
column's largest value); a day that drifts keeps both days and plays once, and its catalogue badge
says "does not repeat". All seven shipped days repeat.

## Where the model shows

- **A frame is a bucket** of steps (a day's: 1 min). Rates are the bucket's means, so the queue's
  conservation law (in = served + dropped + Δbacklog) holds per frame (`bucketBalance`, tested on
  the fixtures); the backlog is a level at the bucket's end; `peak_rho` and `peak_backlog` the
  bucket's worst step. The clock names the bucket's start; nothing is interpolated.
- **The zone** (green, yellow, red) is the bucket's worst step against the node's thresholds
  (utilisation 0.7 and 0.9 by default). A queue's box also judges its lag against a `queue_lag`
  SLO and its waiting messages against their room (`diagram/queue-zone.ts`), and the zone strip
  shows the same zone as the box (`boxZone`). The zone is written as a word too.
- **Copies:** a box with replicas is a stack, its slots the copies up; a `node_down` of some
  copies empties that many slots while it lasts (simsys knows how many, not which).
- **Links:** an arrowhead from caller to callee; dashes move with the data, faster the more the
  target serves (a log scale). A worker pool pulls from its queue, so its link's arrowhead is at the
  queue, and the messages it has not read wait in the queue's box.
- **p99 is estimated** (a lognormal fit to the path's mean and variance; the graph's
  `estimated_columns`), and its chart says so.
- **The report** is the observed window's: for a day, day 2. It lists every day's copy of a fault;
  the page shows the window's only.

## Placing a scenario's boxes: `diagram.yaml`

ELK lays out a scenario by itself (left to right, clients first, each link joined to the sides of
its boxes that face each other). To draw it like the book's figure, put a `diagram.yaml` beside the
`scenario.yaml` (web-crawler has one):

```yaml
cells: # every node on a [column, row] grid, or none
  seeds: [0, 1]
  frontier: [1, 1]
  downloader: [2, 1]
sides: # the sides chosen links leave and enter: left, right, top, bottom
  "link_extractor -> url_filter": [left, right]
```

Run `npm run precompute -- --only <id>` again: the precompute checks the file against the graph
(an unknown node or link, two boxes in one cell, or a node left out is refused) and puts it in the
dataset's first line.

## Adding a view

A view reads the store and nothing else. Write what it draws as a pure function of the graph, the
store and the frame (`charts/series.ts`, `diagram/to-flow-elements.ts` are the examples), test it
from a fixture (`app/test/support.ts`: `fixtureStore("labs-kv-store")`), then the component. A
component that redraws per frame takes its per-frame state from a context or a memo keyed on what
changes, not new objects every frame. CLAUDE.md §5 has the conventions.

## Tests

Vitest with jsdom and Testing Library. jsdom renders neither React Flow nor uPlot, so their inputs
are tested as pure functions and the rendering is checked by eye. The fixtures in
`stream/test/fixtures` are real streams (hello, chain, kv-store, url-shortener's day); CI's
`fixtures --check` fails when simsys's output changes, and the precompute's integration test runs
url-shortener's 48 h through the release binary when `SIMSYS` is set.
