# Web crawler (ch. 9)

## Status: runs end to end, and fails both SLOs on purpose (exit 2)

Since phase 3 Task 18 the URL frontier is simulated, so `run`, `estimate` and `sweep` take the
whole crawler. Every number below is measured: `simsys run scenarios/book/web-crawler/scenario.yaml`
(Release build, 6 h at 1 s steps, 10 minutes of warmup; it exits 2) prints:

```
scenario web-crawler  21000 steps observed  drop rate 1% (worst: dns_upstream)
nodes
  node            mean_rho  peak_rho  peak_backlog  dropped  dead_lettered/s  rejected    red_s  peak_lag
  seeds                  -         -             -        -                -         -        -         -
  frontier          0.6797    0.6797             0        0                -         0        0     14.4h
  downloader             1         1        103.6M        0           0.0213         0  2.1e+04         -
  dns                 0.02      0.02             0        0                -         0        0         -
  dns_upstream         0.1       0.1             0      21k                -         0        0         -
  internet        0.001999  0.001999             0   2.099M                -         0        0         -
  parser            0.7596    0.7596             0        0                -         0        0         -
  content_seen     0.01899   0.01899             0        0                -         0        0         -
  content_store     0.6647    0.6647             0        0                -         0        0         -
  link_extractor    0.6647    0.6647             0        0                -         0        0         -
  url_filter        0.3323    0.3323             0        0                -         0        0         -
  url_seen          0.6647    0.6647             0        0                -         0        0         -
...
slo
  status  metric       node        op  observed  max
  FAIL    utilization  downloader  -   1         0.85
  FAIL    queue_lag    frontier    -   14.4h     30m
```

Both SLOs fail, and that is the lesson: **the crawl loop discovers URLs faster than the crawler
can download them**, so the frontier's backlog grows for the whole run and its lag with it. The
numbers, read off the table (`simsys run ... --report json` has them to more digits):

- The **downloaders** are the bottleneck: 1000 workers at 500 ms are 2000 fetches a second
  (Little's law: workers / cost), and every one of them is busy (rho 1). Nothing else is: the
  `internet` is the web, not a provider's account (`capacity: 1M/s`, so its 2000 calls a second
  are 0.2 % of it), and the parser, the content store and the frontier's prioritiser are sized for
  what 2000 fetches a second bring.
- The internet fails 5 % of its calls (`failure_rate: 0.05`), 99.95 a second (2.099M in the run);
  the downloader sends a failed fetch back on the frontier (`retries: 2`, after the default 1 s),
  and 0.0213 a second fail their third attempt and are dead letters. A retrying pool's share of a
  sink's drops is not counted as lost, so the run's drop rate is dns_upstream's 1 % failures.
- The **loop**: 1899 pages a second reach the parser (`parser` 0.7596 of 25 × 100/s), 70 % of them
  new content (`content_seen` hits 30 %): 1329 writes of 500 KB a second, 664.7 MB/s
  (`content_store` 0.6647 of 1 GB/s). Each has 50 links, of which `url_seen` has not seen 10 %:
  1329 × 50 × 0.1 = **6647 new URLs a second** go back to the frontier, from 50 seeds a second.
  With the retried fetches the frontier is offered 6797 URLs/s (rho 0.6797 of its 10k/s
  prioritiser: it files them all, and drops none).
- The **downloaders** drain 2000 of them: the backlog in front of them grows by 4797 a second, to
  103.6M URLs after 6 h (4797 × 21,600), and the frontier's **lag**, that backlog over the
  downloaders' drain, reaches 103.6M / 2000 = 51,794 s = **14.4 h** at the end of the run, against
  a `queue_lag` SLO of 30 minutes. It grows by 2.4 s every second (4797 / 2000), so the SLO is
  broken 12.5 minutes into the crawl (1800 s / 2.4).

The loop's **gain**, the new URLs one fetched page brings back, is 0.7 × 50 × 0.1 = **3.5**. A
loop of gain above 1 has no steady state of its own: each lap brings back more than it took, so
the flow climbs until a node saturates (here the downloaders), and the excess queues for ever.
The theory tests pin the same mechanism on a smaller crawler without a content filter (gain
50 × 0.1 = 5: the pool saturates, `estimate` converges there; gains 0.2, 0.4 and 0.8 settle at
seeds / (1 − gain), 50 seeds a second at gain 0.8 giving 250 URLs/s). `explain`, whose paper load
knows no capacities, cannot sum such a loop: it prints every node on it as `peak offered unbounded
vs ... (no steady state, red)` and ends its checks with `(no steady state: a loop of gain >= 1)`.

`simsys estimate` (exit 2) reaches the same rates, and says what the long run of a backlog that
grows for ever is: the frontier's `peak_lag` and the downloader's `peak_backlog` read
**`unbounded`**, and no URL is dropped. A frontier deletes nothing: the downloader's room has no
end (`explain` prints `queue_limit=unbounded (a frontier deletes nothing)` on its line), unlike a
message queue's pool, whose room is its retention's worth of draining. A real crawler is in this
state by design: its frontier holds billions of URLs it will never fetch, and it is sized by the
pages it fetches a second, not by the URLs it finds. The `queue_lag` SLO is the alarm that says
"this frontier will never drain".

### More downloaders do not shorten the lag; fewer URLs in do

In a loop of gain above 1 the inflow is proportional to the drain: every fetch brings 3.4 URLs back
to the frontier (6797 / 2000, the seeds and the retries included), 2.4 more than the one it took,
so the backlog grows at 2.4 × the drain and the lag, the backlog over the drain, at 2.4 s a second
whatever the drain. **Politeness** shows it: lowering
`frontier.hosts` below 2000 makes the cap (hosts × 1/s) smaller than the downloaders' 2000/s, so
the frontier hands them fewer URLs a second, and the downloaders' utilization falls with it while
the lag hardly moves (the seeds' 50/s weigh a little more against a smaller drain).
`simsys sweep scenarios/book/web-crawler/scenario.yaml --param nodes.frontier.hosts=500..4000:8
--metric downloader.mean_rho frontier.lag internet.mean_rho` prints:

```
nodes.frontier.hosts  downloader.mean_rho  frontier.lag  internet.mean_rho
--------------------------------------------------------------------------
500                                  0.25         14.8h             0.0005
1000                                  0.5         14.5h           0.000999
1500                                 0.75         14.4h             0.0015
2000                                    1         14.4h              0.002
2500                                    1         14.4h              0.002
3000                                    1         14.4h              0.002
3500                                    1         14.4h              0.002
4000                                    1         14.4h              0.002
```

The cap is a token bucket in front of the downloaders, not fewer downloaders: at 500 hosts they
fetch 500 URLs a second and 250 of their 1000 workers are busy (rho = 500 × 0.5 s / 1000 = 0.25).
More workers would lower that rho and fetch nothing more. The fix is fewer URLs in: a stricter
`url_seen` lowers the gain, and below 1 the loop has a steady state.
`simsys sweep scenarios/book/web-crawler/scenario.yaml --param
nodes.url_seen.hit_ratio.fixed=0.9..0.98:5 --metric frontier.lag downloader.mean_rho
frontier.mean_rho` prints:

```
nodes.url_seen.hit_ratio.fixed  frontier.lag  downloader.mean_rho  frontier.mean_rho
------------------------------------------------------------------------------------
0.9                                    14.4h                    1               0.68
0.92                                   10.4h                    1              0.547
0.94                                   6.41h                    1              0.414
0.96                                   2.42h                    1              0.281
0.98                                      0s               0.0876             0.0175
```

At 98 % seen the gain is 0.7 × 50 × 0.02 = 0.7: the loop settles at 175 URLs a second, far below
the 2000 the downloaders can fetch (rho 0.0876), and nothing waits. (Other ways to fewer URLs in: a crawl that stops
following links past some depth, priorities that drop the long tail.)

## How the book's design maps onto the model

- **Seed URLs** (`seeds`): a client of 50 URLs/s.
- **URL frontier** (`frontier`, `type: frontier`; behaviours::FrontierBehaviour): the book's two
  banks of queues. The **prioritiser** (front queues) is the node's own queue at `capacity: 10k/s`;
  what it cannot file it drops. The URLs it files go along its `then` to the downloader, and what
  the downloader has not read waits in the downloader's queue: the frontier is a queue without
  retention (nothing is deleted unconsumed; the downloader's room has no end). Its lag is the
  downloader's wait, as for a message queue. **Politeness** (back queues, one host fetched by one
  worker at a time) is a rate cap in front of the downloaders: `hosts: 10M` hosts at
  `politeness: 1/s` each hand out at most 10M URLs a second, far above the 2000 the downloaders
  can fetch, so here it never binds (the sweep above lowers it until it does). One bank of
  downloaders per frontier: a second pool consuming it is refused (each would be handed every URL
  and the whole politeness budget).
- **HTML downloader** (`downloader`, a worker pool that `consumes: frontier`): 1000 workers at
  500 ms, each fetch a DNS lookup (`dns`, a read-through cache in front of `dns_upstream`), a
  call to `internet` and a hand-off to the `parser`. A failed fetch goes back on the frontier
  twice at most.
- **The web** (`internet`, an external sink): 300 ms a page, 5 % failures, and `capacity: 1M/s`,
  the web as a whole rather than the type's default 1k/s (a quota a provider grants one account:
  see the first exercise). Its `politeness: 1/s` is the same cap on the sink, over the frontier's
  10M hosts: 10M/s, which never binds either; the sink needs the frontier to state `hosts` (run
  refuses it otherwise).
- **Content parser** (`parser`, 25 replicas of 100 pages/s), **"content seen?"** (`content_seen`,
  30 % duplicates), **content storage** (`content_store`, 500 KB a page at 1 GB/s), **link
  extractor** (50 links a page), **URL filter** and **"URL seen?"** (`url_seen`, 90 % already
  seen), whose misses go back to the frontier: the graph's one feedback edge,
  `url_seen -> frontier`.

## Book design

- Seed URLs → URL frontier (prioritiser + politeness queues) → HTML downloader (with DNS cache)
  → content parser → "content seen?" → content storage → link extractor → URL filter →
  "URL seen?" → back into the frontier.
- Politeness: at most one in-flight request per host; priority by PageRank/traffic.
- Robustness: consistent hashing across downloaders, checkpoints, spider-trap detection.
- Freshness: pages change, so a crawler re-crawls them, more often the more they change.

## What it abstracts away

- The priority **order** (a fluid has no individual URLs, so which URL is fetched first changes
  no rate) and PageRank.
- How the URLs spread over hosts: politeness is applied as hosts × politeness, every host's back
  queue busy at once. A real crawl's URLs are skewed towards a few big sites, whose back queues
  set the pace well below that: the model's cap is the optimistic case.
- **Freshness** and re-crawls: each URL the frontier is handed is fetched once, and no page comes
  back to the frontier because it aged.
- Robots.txt, spider traps, consistent hashing of hosts to downloaders, the frontier's disk.
- A worker blocked on a slow site: a worker is busy for `cost` (500 ms) whatever the internet's
  queue does.
- Checkpoints and restart.

## Things to try

Each measured with `simsys run` on a copy of the file with the one change.

- **The internet's quota** (the exercise): delete `internet`'s `capacity: 1M/s`. The sink is back
  at the `external_sink` type's 1k/s, a quota one provider account gets, and it is the first
  wall: it serves 1000 fetches a second (rho 1), fails 5 % of them and sheds what its room cannot
  hold (22.03M calls dropped in the run, both together), so 950 pages a second reach the parser
  (`parser` 0.38). The downloaders are still
  busy all the time (rho 1), much of it re-fetching what the internet shed: 46.44 fetches a second
  fail their third attempt and are dead letters. The frontier is offered 4378 URLs/s (rho 0.4378)
  and its lag reaches 7.13 h. Which wall is first decides what fixing costs: here more quota,
  not more downloaders.
- **Politeness as a cap**: set `frontier.hosts` to `1000`. `explain` prints `hosts=1k
  politeness=1/s (drain cap 1k/s: 1k hosts x 1/s)` on the frontier, the same cap on the internet,
  and checks the frontier against it: `peak offered unbounded vs 1k/s drain cap (1k hosts x 1/s,
  below the prioritiser's 10k/s)  (no steady state, red)`. `run` (exit 2) shows the downloader
  fetching 1000 URLs a second with half its workers busy (rho 0.5): its `utilization` SLO now
  **passes**, while the `queue_lag` SLO still fails at 14.5 h.
- Set `url_seen.hit_ratio` to 0.5: the loop's gain grows to 0.7 × 50 × 0.5 = 17.5, the frontier's
  prioritiser saturates (rho 1) and drops 491M URLs in the run (a drop rate of 70 %), and the lag
  reaches 24 h: the model's version of a crawl trap.
- Raise `internet.latency` to 1s: nothing changes (the table is the same to the digit), because a
  worker's `cost` is what holds it. Raise the downloader's `cost` to 1s too and its capacity halves
  (`explain`: `capacity=1k/s (workers / cost)`): every worker still busy (rho 1), the frontier
  offered half as much (rho 0.3423), and the lag the same 14.5 h, the loop's gain at work again.
- Add `faults: [{at: 2h, kind: node_down, node: dns_upstream, for: 10m}]`: the 5 % of lookups the
  DNS cache misses, 100 a second, fail for ten minutes (`run`'s fault block: `offered 100/s,
  served 0/s`, back within 1 % 2 s after the fault clears) and the run's drop rate rises from 1 %
  to 3.78 %.
