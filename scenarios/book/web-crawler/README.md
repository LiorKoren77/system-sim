# Web crawler (ch. 9)

## Status: `explain` only until phase 3

This scenario validates (`OK: web-crawler (12 nodes, 12 links)`) and `simsys explain` prints its
whole resolved graph, but `run` and `estimate` refuse it with exit code 1:

```
scenarios/book/web-crawler/scenario.yaml: error: node 'frontier' has type 'frontier', which run/estimate do not support yet (phase 3)
```

`frontier`, the URL frontier with its prioritiser and per-host politeness queues, is a `frontier`.
Phase 3 models that type and the per-host state behind `politeness` (see
[What runs in phase 2c](../../../docs/scenario-schema.md#what-runs-in-phase-2c)).

What `explain` shows today. Its checks compare each node's peak offered load with its capacity,
without queueing. Every page the crawler fetches is handed on by the frontier, and `explain`
does not follow a frontier's `then` steps yet. So only the frontier itself is checked, and only
as a lower bound: the seeds' 50/s, without the URLs fed back from `url_seen`. The other ten nodes
are marked instead of checked:

```
frontier        peak offered 50/s vs 5k/s total capacity  (≥1%; load not propagated: frontier is not modelled yet)
downloader      peak offered 0/s vs 2k/s total capacity  (load not propagated: frontier is not modelled yet)
content_store   peak offered 0 MB/s (0/s) vs 400 MB/s total capacity  (load not propagated: frontier is not modelled yet)
url_seen        peak offered 0/s vs 100k/s total capacity  (load not propagated: frontier is not modelled yet)
```

The `0/s` is a missing number, not a zero, and no line is red. The crawl loop closes at
`url_seen -> frontier`, the graph's one feedback edge (`[feedback]` in the links). The two SLOs, the
downloader's utilization and the frontier's queue lag, wait for phase 3.

## Book design
- Seed URLs → URL frontier (prioritiser + politeness queues) → HTML downloader (with DNS cache)
  → content parser → "content seen?" → content storage → link extractor → URL filter →
  "URL seen?" → back into the frontier.
- Politeness: at most one in-flight request per host; priority by PageRank/traffic.
- Robustness: consistent hashing across downloaders, checkpoints, spider-trap detection.

## What phase 3 will model
Once the frontier runs, the scenario is meant to show the following.
- The crawl loop as a *feedback flow*: each page yields ~50 links, 90% already seen; the
  frontier grows or drains depending on the balance between discovery and download capacity.
  With this file's numbers, a fetched page that is new content (70 %) yields 50 links, of which
  10 % are unseen, so it adds 0.7 × 50 × 0.1 = 3.5 URLs. That loop gain is above 1, so the
  frontier grows until the downloaders' ceiling binds.
- Politeness as a per-host rate cap on the Internet sink and the frontier. Today both
  `politeness` keys are stored and not modelled.
- Duplicate content (30%) short-circuiting storage and extraction.
- Downloader parallelism: 1,000 workers × ~0.5 s per fetch gives a ceiling of about 2,000 pages/s
  (`explain` resolves it: `capacity=2k/s (workers / cost)`).

## What it abstracts away
- Robots.txt, spider traps, PageRank ordering, consistent hashing of hosts to downloaders.
- Checkpoints and restart.

## Things to try
With `run`, once phase 3 runs the frontier. Today `explain` marks every node behind the frontier,
whatever their numbers.
- Raise `internet.latency` to 1s: downloader utilization climbs, frontier lag grows.
- Set `url_seen.hit_ratio` to 0.5: the frontier explodes, which is the model's version of a crawl
  trap.
- Add `faults: [{at: 2h, kind: node_down, node: dns_upstream, for: 10m}]` (phase 3's faults;
  today the loader accepts the list and nothing happens).
