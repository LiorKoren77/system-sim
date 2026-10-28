# Web crawler (ch. 9)

## Book design
- Seed URLs → URL frontier (prioritiser + politeness queues) → HTML downloader (with DNS cache)
  → content parser → "content seen?" → content storage → link extractor → URL filter →
  "URL seen?" → back into the frontier.
- Politeness: at most one in-flight request per host; priority by PageRank/traffic.
- Robustness: consistent hashing across downloaders, checkpoints, spider-trap detection.

## What the model captures
- The crawl loop as a *feedback flow*: each page yields ~50 links, 90% already seen; the
  frontier grows or drains depending on the balance between discovery and download capacity.
- Politeness as a per-host rate cap on the Internet sink and the frontier.
- Duplicate content (30%) short-circuiting storage and extraction.
- Downloader parallelism (1,000 workers × ~0.5 s per fetch ≈ 2,000 pages/s ceiling).

## What it abstracts away
- Robots.txt, spider traps, PageRank ordering, consistent hashing of hosts to downloaders.
- Checkpoints and restart.

## Things to try
- Raise `internet.latency` to 1s: downloader utilization climbs, frontier lag grows.
- Set `url_seen.hit_ratio` to 0.5: the frontier explodes — the model's version of a crawl trap.
- Add `faults: [{at: 2h, kind: node_down, node: dns_upstream, for: 10m}]`.
