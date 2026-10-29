# News feed (ch. 11)

## Book design
- Publishing: web → post DB + post cache → fan-out queue → fan-out workers look up the
  social graph and write the post id into each friend's feed cache (fan-out on write).
- Reading: web → feed cache (post ids) → post cache / user cache for hydration → DB on miss.
- Hybrid: celebrities use fan-out on read; the rest fan-out on write.
- CDN for media; caches layered: feed, post, user, social graph, counters.

The scenario's client offers 10k requests/s on a daily sine of ±60 % that tops at 20:00
(16k/s), as 60 % `read_feed`, 30 % `read_media` and 10 % `post`, and a lunchtime spike: 3× the
curve from 12:00 to 12:10. The spike, not the evening, is the day's peak: the sine is at 70 %
of its base at noon and still rising, 72 % by 12:09, so 3 × 7.2k = 21.6k/s (`explain` samples
whole minutes and finds 21.62k/s at 12:09; the run's last spike step, 12:09:59.9, is 21.69k/s,
which is why the run's peak utilizations below are 0.3 % above `explain`'s percentages).

### How it is sized
As first shipped (8 web servers, one replica of each cache, one `post_db` shard), `explain`
showed five nodes red at the peak: `web  peak offered 15.13k/s vs 16k/s total capacity  (94.6%,
red)`, `feed_cache  peak offered 661.5k/s vs 100k/s total capacity  (662%, red)`, `post_cache`
262 %, `user_cache` 259 %, and `post_db  ... primary 10.81k units/s vs 5k/s (216%, red), each
replica 17.3k units/s vs 5k/s (346%, red)`. `simsys run` dropped 59.9 % of the work (`drop rate
59.9% (worst: feed_cache)`), the feed cache saturated all day (`red_s 8.61e+04`) and the
`drop_rate` SLO failed (`FAIL drop_rate - - 0.5987 0.001`, exit 2). Every fix is the book's
remedy, sized with its numbers for the 12:00 peak by one rule: **the smallest count whose peak
is at most two-thirds of its capacity**, i.e. capacity 1.5 times the peak, room for the noise and
the bursts the smooth sine lacks (a spike a little larger than 3× still fits). The rule gives 12
web servers, 10 feed-cache replicas, 4 post-cache and 4 user-cache replicas, and 6 `post_db`
shards; the SLOs are unchanged.

- **The feed cache, 1 → 10 replicas: the chapter's lesson in one number.** Fan-out on write
  turns each post into one write per friend, ~300 of them: the peak's 2.162k posts/s become
  648.6k feed writes/s, against only 12.97k feed reads/s. Together `feed_cache  peak offered
  661.5k/s vs 1M/s total capacity  (66.2%, green)` over 10 × 100k/s (the back-of-the-envelope
  "cache 100k QPS"). The feed cache is sized by the writes the fan-out makes, not by the reads
  it serves; 9 replicas would be 73.5 %, yellow, and 7 94.5 %, red. (This is also why the book skips inactive users
  and switches celebrities to fan-out on read: both cut the 300.)
- **Web tier, 8 → 12 servers** (ch. 1 "stateless web tier"): the feed reads and posts, 70 % of
  the peak (media goes to the CDN): `web  peak offered 15.13k/s vs 24k/s total capacity
  (63.1%, green)`; 11 would be 68.8 %, over the rule's two-thirds.
- **Post and user caches, 1 → 4 replicas each**: each feed read hydrates ~20 posts and their 20
  authors, so 12.97k reads/s × 20 = 259.4k/s at each cache (the post cache also takes the 2.162k
  post writes): `post_cache  peak offered 261.6k/s vs 400k/s total capacity  (65.4%, green)`,
  `user_cache  peak offered 259.4k/s vs 400k/s total capacity  (64.9%, green)`. Three replicas
  would be at 87 %, a step from red, in the very ten minutes a cache must not drop.
- **`post_db` sharded, `shards: 6`** (ch. 1 "sharding"; the URL shortener's remedy): the peak's
  posts are 2.162k writes/s at 5 capacity units each (spec §6: one SQL box does 5k reads/s or
  1k writes/s), 10.81k units/s against one primary's 5k/s, and read replicas cannot help: each
  replays every write of its shard. Six shards of a primary and 2 read replicas: `post_db  peak
  offered 23.78k units/s vs 90k/s total capacity  (26.4%, green)  primary 1.802k units/s vs
  5k/s (36%, green), each replica 2.883k units/s vs 5k/s (57.7%, green)`, a replica's load being
  its twelfth of the post cache's 12.97k misses/s plus its shard's 1.802k units/s of replayed
  writes. With 5 shards a replica would be at 69.2 %, over two-thirds, with 4 at 86.5 %, with 3
  at 115 %, red.
- **What did not need sizing**: the fan-out queue and workers (`fanout  peak offered 2.162k/s vs
  6.4k/s total capacity  (33.8%, green)`: 32 workers at 5 ms), the friendship graph and the
  media path (below).

The rule is a margin, not a pass line: 11 web servers and 5 shards also pass every SLO (the
run's peaks are 69.0 % and 69.4 %), less than a point from yellow (70 %); the rule picks 12 and 6.

## What the model captures
All numbers are from `simsys explain` and `simsys run` on `scenario.yaml` (24 h at 100 ms
steps, 5 min warmup: 861,000 observed steps, about 20 s in a Release build).

- **The day passes.** `run` exits 0: `PASS p99_latency users read_feed 5.99ms 200ms`, `PASS
  drop_rate - - 0 0.001`, `PASS queue_lag fanout_q - <1us 2m`. Nothing is dropped anywhere.
  The busiest copies, at the end of the spike: `feed_cache` 0.6637, `post_cache` 0.6561,
  `user_cache` 0.6507, `web` 0.6326, a `post_db` replica 0.5784, `fanout` 0.3389 (`peak_rho`;
  each is `explain`'s percentage × 21.69k / 21.62k).
- **The 300× write amplification of fan-out on write** hitting `feed_cache` (above) and the
  friendship graph. Each fan-out job reads the poster's friend list once from `graph_db`, a
  key-value cluster of 6 machines keeping N = 3 copies (simulated since phase 2c), so 3
  replica-requests per job against the machines' 6 × 30k/s, the hottest of them (1 +
  z_6/sqrt(3 × 150) = 1.0604 times the average, z_6 = 1.28 the expected maximum of 6 standard
  normals) checked: `graph_db  peak offered 6.486k/s (2.162k/s × N 3) vs 180k/s (6 nodes)
  (hottest 3.82%, green)`; the run's peak is 0.03833.
- **The fan-out queue absorbs the posts** without falling behind: the 32 workers are at most 34 %
  busy, so a message waits for a free worker with Erlang C probability next to nothing; the
  peak lag is `<1us` (3.5e-11 s). Posting returns once the post is written and published: the
  user never waits for the 300 feed writes (`users.post` p99 5.36 ms).
- **The read path's fan-out** (20 posts and 20 authors hydrated per feed read) multiplying cache
  traffic 20×, and the misses reaching `post_db`'s replicas: the feed cache's Che hit ratio,
  10M slots over the users' Zipf(1.1) space of 50M ids, is `hit_ratio=0.952`; the post cache's
  is a fixed 0.95 (see the comment in the YAML for why not Che).
- **Latency per op** (`run`, percentiles from a lognormal fitted to each op's mean and
  variance; the users time out at 500 ms):

  | op | offered/s (day mean) | timed out/s | mean | p50 | p95 | p99 |
  |---|---|---|---|---|---|---|
  | `post` | 1.009k | 0 | 3.7 ms | 3.65 ms | 4.79 ms | 5.36 ms |
  | `read_feed` | 6.053k | 0 | 4.14 ms | 4.08 ms | 5.35 ms | 5.99 ms |
  | `read_media` | 3.027k | 0.00456 | 12.5 ms | 8.58 ms | 35.9 ms | 64.9 ms |

  The 0.00456 media reads a second that time out are the fitted lognormal's tail past 500 ms
  (1.5 in a million), not a queue: the busiest node on the media path, `media_store`, is 6.51 %
  busy at the peak.
- **Media in bytes.** Media reads (30 % of requests, 200 KB each) go to the CDN and only the 5 %
  misses reach `media_store`, which is sized in bytes (`capacity: 1GB/s`) and so counts megabytes
  (phase 2c): each 200 KB read costs 0.2 units of 1 MB: `media_store  peak offered 64.86 MB/s
  (324.3/s) vs 1k MB/s total capacity  (6.49%, green)`. A read is answered in one megabyte's
  40 ms (an upper bound for a 200 KB object: the model charges every request at least one
  unit's service time).
- **Replication.** `post_db`'s reads are stale by `staleness 50.4ms` (the 50 ms replication lag
  plus the two sojourns); each shard's primary commits 168.1 writes/s over the day and no
  replica misses any.

## What it abstracts away
- **Faults.** The file declares a 30-second `post_db` outage at 15:00, but `run` ignores faults
  until phase 3: the run above has no outage in it. (The design would ride it out on the caches
  for reads; posts would fail for 30 s, since a post is written before it is published.)
- Celebrity handling (fan-out on read) — model as a second client with its own op in a variant.
- Ranking, pagination, inactive-user skipping (which the book uses to shrink fan-out).
- The queue's partitions (`partitions: 16`, printed, not modelled) and the order of feed entries.
  In Kafka one partition feeds at most one consumer of a group, so the 32 workers would read as
  16 consumers (or 16 consumers each running two workers); the sweep below shows that 16 workers
  still keep the spike's lag at 101 µs, so the design holds with the partitions it has.

## Things to try
- **Fewer fan-out workers.** `simsys sweep scenarios/book/news-feed/scenario.yaml --param
  nodes.fanout.workers=8..32:4 --metric fanout_q.lag --metric fanout.mean_rho` (the four
  points in parallel, about 25 s):

  ```
  nodes.fanout.workers  fanout_q.lag  fanout.mean_rho
  ---------------------------------------------------
  8                            3.34m            0.631
  16                           101us            0.315
  24                            <1us             0.21
  32                            <1us            0.158
  ```

  8 workers drain 8 / 5 ms = 1.6k jobs/s, below the spike's posts (2.1k/s at 12:00 rising to
  2.169k/s at 12:10), so for its ten minutes the backlog grows by about 534 messages a second:
  `fanout  ...  peak_backlog 320.6k` at 12:10, and a post published then waits for all of them,
  320.6k / 1.6k/s = 200 s: `FAIL queue_lag fanout_q - 3.34m 2m`, exit 2. The backlog is gone
  by about 12:16. The evening's 1.6k posts/s at 20:00 only meet the 8 workers' capacity: the pool is
  red (`red_s 2.153e+04`, about 6 h around 20:00) but never behind, since a fluid backlog grows
  only while arrivals exceed capacity; the lag there is Erlang C's wait with the utilization
  clamped at 0.99, 60 ms. That is the fluid model's answer: a real queue held at rho → 1 for an
  hour or two waits longer, its backlog wandering like a random walk, about sqrt(2 λ t) messages
  after t (sqrt(2 × 1.6k/s × 1 h) ≈ 3.4k messages, ≈ 2 s of work at 1.6k/s): seconds rather
  than 60 ms, still far under the 2-minute SLO. The other SLOs still pass and nothing is dropped: the fan-out is
  asynchronous, and readers simply see a post up to 200 s late. 16 workers already keep the
  spike's lag at 101 µs.
- **One database shard** (`post_db.shards: 1`): `primary 10.81k units/s vs 5k/s (216%, red), each
  replica 17.3k units/s vs 5k/s (346%, red)`. The run drops 28.4 % of what reaches `post_db`
  (`FAIL drop_rate - - 0.2844 0.001`, exit 2), and since a step after a failed step does not run
  (since phase 2c), the losses thin what follows them: a post whose write was dropped is
  neither cached nor published (810.4 of the 1.009k posts a second are answered; `fanout`'s mean
  rho falls from 0.1576 to 0.1266, `feed_cache`'s from 0.3087 to 0.2492), and a hydration whose
  miss read was dropped skips its author (`user_cache` 0.3027 → 0.2973). The replicas fall
  behind without bound (`missed writes/s per replica 267`). The `read_feed` p99 still passes
  (75.7 ms), so only the drop rate tells.
- **A worse post cache** (`post_cache`'s `hit_ratio: {fixed: 0.8}`, from 0.95): the misses
  quadruple to 51.88k reads/s and the 6 shards' replicas are no longer enough: `each replica
  6.125k units/s vs 5k/s (123%, red)` while the primaries stay at 36 %; the run drops 0.211 %
  (`FAIL drop_rate - - 0.002111 0.001`). Reads are cured by more read replicas, not shards:
  `replicas: 5` (4 read replicas a shard) gives `each replica 3.963k units/s vs 5k/s (79.3%,
  yellow)`. With one shard and 0.8 a replica is at 735 %.
- **A chatty day** (`ops: {read_feed: 0.4, read_media: 0.3, post: 0.3}`: three times the posts;
  the shares must still sum to 1): `graph_db` triples but stays green (`hottest 11.5%`), while
  the fan-out's 300× lands on the feed cache: `feed_cache  peak offered 1.954M/s vs 1M/s total
  capacity  (195%, red)`, `fanout ... (101%, red)`, and the primaries `5.405k units/s vs 5k/s
  (108%, red)`. The run drops 14.6 % (worst: `feed_cache`). It is the feed cache, not the
  graph, that a write-heavy day breaks.
