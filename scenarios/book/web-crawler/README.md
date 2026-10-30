# Web crawler (ch. 9)

## Status: runs end to end, and fails both SLOs on purpose (exit 2)

Since phase 3 Task 18 the URL frontier is simulated, so `run`, `estimate` and `sweep` take the
whole crawler. `simsys run` (Release build, 6 h at 1 s steps, 10 minutes of warmup) prints:

```
scenario web-crawler  21000 steps observed  drop rate 1% (worst: dns_upstream)
nodes
  node            mean_rho  peak_rho  peak_backlog  dropped  dead_lettered/s  rejected    red_s  peak_lag
  seeds                  -         -             -        -                -         -        -         -
  frontier          0.8755    0.8755             0        0                -         0        0     7.13h
  downloader             1         1        51.35M        0            46.44         0  2.1e+04         -
  dns                 0.02      0.02             0        0                -         0        0         -
  dns_upstream         0.1       0.1             0      21k                -         0        0         -
  internet               1         1            1k   22.03M                -         0  2.1e+04         -
  parser             0.475     0.475             0        0                -         0        0         -
  content_seen      0.0095    0.0095             0        0                -         0        0         -
  content_store     0.8312    0.8313             0        0                -         0        0         -
  link_extractor    0.3325    0.3325             0        0                -         0        0         -
  url_filter        0.1663    0.1663             0        0                -         0        0         -
  url_seen          0.3325    0.3325             0        0                -         0        0         -
...
slo
  status  metric       node        op  observed  max
  FAIL    utilization  downloader  -   1         0.85
  FAIL    queue_lag    frontier    -   7.13h     30m
```

Both SLOs fail, and that is the lesson: **the crawl loop discovers URLs faster than the crawler
can download them**, so the frontier's backlog grows for the whole run and its lag with it. The
numbers, read off the table:

- The **internet** is the first wall. It states no `capacity`, so it is the `external_sink`'s
  type default, a 1k/s quota (a provider account's, not the web's), and it is offered more:
  it serves 1000 fetches a second, fails 5 % of them (`failure_rate: 0.05`) and sheds the rest
  once its 1000-request room is full. So **950 pages a second** reach the parser
  (`parser` 0.475 of 20 × 100/s).
- The **downloader** retries a failed or shed fetch (`retries: 2`, after the default 1 s): the
  job goes back on the frontier. Its 1000 workers at 500 ms (2000 fetches/s by Little's law)
  are busy all the time (rho 1), much of it on retries; 46 fetches a second fail their third
  attempt and are dead letters. The internet's sheds that the downloader retries are not counted
  as lost (the drop rate leaves out a retrying pool's share), so the run's drop rate is
  dns_upstream's 1 % failures.
- The **loop**: of the 950 parsed pages, 70 % are new content (`content_seen` hits 30 %), each
  with 50 links, of which `url_seen` has not seen 10 %: 950 × 0.7 × 50 × 0.1 = **3325 new URLs a
  second** go back to the frontier, from 50 seeds a second. With the retried fetches the
  frontier is offered 4377.5 URLs/s (rho 0.8755 of its 5k/s prioritiser: it passes them all).
- The **downloader** drains only 2000 of them: the backlog in front of it grows by 2377.5 a second,
  to 51.35M URLs after 6 h (2377.5 × 21,600), and the frontier's **lag**, that backlog over the
  pool's drain, reaches 51.35M / 2000 = 25,675 s = **7.13 h** at the end of the run, against a
  `queue_lag` SLO of 30 minutes. It grows by 1.19 s every second, so the SLO fails 25 minutes into
  the crawl.

The loop's **gain**, the new URLs one fetched page brings back, is 0.7 × 50 × 0.1 = **3.5**. A
loop of gain above 1 has no steady state of its own: each lap brings back more than it took, so
the flow climbs until a node saturates (here the internet's quota and the downloaders), and the
excess queues for ever. The theory tests pin the same mechanism on a smaller crawler without a
content filter (gain 50 × 0.1 = 5: the pool saturates, `estimate` converges there; gains 0.2, 0.4
and 0.8 settle at seeds / (1 − gain), 50 seeds a second at gain 0.8 giving 250 URLs/s). `explain`,
whose paper load knows no capacities, cannot sum such a loop: it prints every node on it as
`peak offered unbounded vs ... (no steady state, red)` and ends its checks with `(no steady
state: a loop of gain >= 1)`.

`estimate` reaches the same rates (its lag is the downloader's full 7-day room, 168 h, because a
saturated queue's long-run state is a full room). A real crawler is in this state by design: its
frontier holds billions of URLs it will never fetch, and it is sized by the pages it fetches a
second, not by the URLs it finds. The `queue_lag` SLO is the alarm that says "this frontier will
never drain"; the fix is fewer URLs in (a stricter `url_seen`, a crawl that stops following links
past some depth, priorities that drop the long tail) or more fetches out (more workers, more of
the internet's quota), not a bigger frontier.

## How the book's design maps onto the model

- **Seed URLs** (`seeds`): a client of 50 URLs/s.
- **URL frontier** (`frontier`, `type: frontier`; behaviours::FrontierBehaviour): the book's two
  banks of queues. The **prioritiser** (front queues) is the node's own queue at `capacity: 5k/s`;
  what it cannot file it drops. The URLs it files go along its `then` to the downloader, and what
  the downloader has not read waits in the downloader's queue: the frontier is a queue without
  retention (nothing is deleted unconsumed; the pool's room is 7 days of its draining). Its lag is
  the downloader's wait, as for a message queue. **Politeness** (back queues, one host fetched by
  one worker at a time) is a rate cap: `hosts: 10M` hosts at `politeness: 1/s` each hand out at
  most 10M URLs a second, far above the 2000 the downloaders can fetch, so here it never binds.
- **HTML downloader** (`downloader`, a worker pool that `consumes: frontier`): 1000 workers at
  500 ms, each fetch a DNS lookup (`dns`, a read-through cache in front of `dns_upstream`), a
  call to `internet` and a hand-off to the `parser`. A failed fetch goes back on the frontier
  twice at most.
- **The web** (`internet`, an external sink): 300 ms a page, 5 % failures. Its `politeness: 1/s`
  is the same cap on the sink, over the frontier's 10M hosts: 10M/s, which never binds either.
- **Content parser**, **"content seen?"** (`content_seen`, 30 % duplicates), **content storage**
  (`content_store`, 500 KB a page at 400 MB/s: 665 writes/s fill 83 % of it), **link extractor**
  (50 links a page), **URL filter** and **"URL seen?"** (`url_seen`, 90 % already seen), whose
  misses go back to the frontier: the graph's one feedback edge, `url_seen -> frontier`.

## Book design

- Seed URLs → URL frontier (prioritiser + politeness queues) → HTML downloader (with DNS cache)
  → content parser → "content seen?" → content storage → link extractor → URL filter →
  "URL seen?" → back into the frontier.
- Politeness: at most one in-flight request per host; priority by PageRank/traffic.
- Robustness: consistent hashing across downloaders, checkpoints, spider-trap detection.

## What it abstracts away

- The priority **order** (a fluid has no individual URLs, so which URL is fetched first changes
  no rate) and PageRank.
- How the URLs spread over hosts: politeness is applied as hosts × politeness, every host's back
  queue busy at once. A real crawl's URLs are skewed towards a few big sites, whose back queues
  set the pace well below that: the model's cap is the optimistic case.
- Robots.txt, spider traps, consistent hashing of hosts to downloaders, the frontier's disk.
- A worker blocked on a slow site: a worker is busy for `cost` (500 ms) whatever the internet's
  queue does.
- Checkpoints and restart.

## Things to try

- **Politeness as a cap**: lower `frontier.hosts` to `1000`. The cap is then 1000 hosts × 1/s =
  1000 URLs/s, below the downloaders' 2000/s, and it binds: `explain` prints `hosts=1k
  politeness=1/s (drain cap 1k/s: 1k hosts x 1/s)` on the frontier and the same cap on the
  internet, and `estimate` shows the downloader draining 1000 fetches a second (its rho reads 1:
  the pool cannot be handed more, and only 500 of its 1000 workers are busy, 1000/s × 500 ms) and
  the internet, offered those, at its cap (rho 0.9995).
- **Lift the internet's quota**: give `internet` a `capacity: 1M/s`. The downloaders' 2000 fetches
  a second then all reach the parser (1900 pages/s answered), `content_store` saturates (1330
  writes/s of 500 KB against 400 MB/s) and the frontier is still offered more than the pool
  drains: the lag keeps growing.
- Raise `internet.latency` to 1s: the fetches take longer, but a worker's `cost` is what holds
  it, so nothing changes until `cost` is raised too (`cost: 1s` halves the downloaders' 2000/s).
- Set `url_seen.hit_ratio` to 0.5: the loop's gain grows to 0.7 × 50 × 0.5 = 17.5 and the frontier
  explodes, the model's version of a crawl trap.
- Add `faults: [{at: 2h, kind: node_down, node: dns_upstream, for: 10m}]`: the 5 % of lookups the
  DNS cache misses, 100 a second, fail for ten minutes (`run`'s fault block: `offered 100/s,
  served 0/s`, back within 1 % 4 s after the fault clears) and the run's drop rate rises from 1 %
  to 3.78 %.
