# Video streaming / YouTube (ch. 14)

## Book design
- Upload: client gets a presigned URL from the API and uploads straight to original storage;
  a message triggers the transcoding DAG (inspection, encoding at several resolutions,
  thumbnails, watermarks); outputs land in transcoded storage; a completion handler updates
  metadata DB and cache.
- Streaming: adaptive bitrate from the CDN, which pulls from transcoded storage on miss.
- Cost: CDN is the bill; serve only popular videos from CDN, the long tail from origin.

## What the model captures
- Bytes, not just requests: the scenario states its 50 MB streams and 300 MB uploads (`sizes`)
  and its GB/s capacities. An upload is split into `upload` (the bytes, straight to
  `original_store`) and `upload_meta` (the presigned-URL request and metadata write, through the
  API).
- The CDNs and the two blob stores count megabytes: a 50 MB stream costs 50 units of
  1 MB, the service time is one megabyte's (10 ms at a CDN, 40 ms at a blob store, the book's
  per-MB rows), so `cdn_us` at 100GB/s has 1000 slots that each move one stream at a time
  (`c=1000 slots (1 object each)`) and saturates at 2k streams/s. `explain` checks them in MB/s
  with the requests beside (the evening peak, 21:00), and the cross-DC link in MB/s too:

  ```
  cdn_us                    peak offered 14.08k MB/s (281.6/s) vs 100k MB/s total capacity  (14.1%, green)
  cdn_eu                    peak offered 7.04k MB/s (140.8/s) vs 50k MB/s total capacity  (14.1%, green)
  original_store            peak offered 2.88k MB/s (9.6/s) vs 5k MB/s total capacity  (57.6%, green)
  transcoded_store          peak offered 9.811k MB/s (196.2/s) vs 20k MB/s total capacity  (49.1%, green)
  cdn_eu->transcoded_store  peak offered 2.856k MB/s vs 5k MB/s  (57.1%, green)
  ```

  `transcoded_store`'s figure is the CDNs' misses, 157.8 streams/s × 50 MB = 7.891k MB/s, plus
  the transcoders' renditions, 9.6 uploads/s × 4 × 50 MB = 1.92k MB/s: 49.1 % of its 20 GB/s.
  At the 10 GB/s it first had it was 98.1 %, red ("Things to try" runs it).
- The two edges' hit ratios come from the Che approximation over the viewers' Zipf(1)
  popularity of 10M videos and the edge's size in videos (`keys: 100k` in the US, `50k` in the
  EU): 0.642 and 0.594. Only the misses reach the origin.
- Two regions: European misses cross the Atlantic to `transcoded_store` (150 ms there and
  back), over a link with `bandwidth: 40Gbit/s` (5 GB/s). The link is a pipe: one M/D/1 queue in
  megabytes that, like the stores, waits for whole objects (a 50 MB stream holds the wire for
  50 MB / 5 GB/s = 10 ms). At 21:00 it carries 140.8 × (1 − 0.594) × 50 MB = 2.856k MB/s,
  rho 0.571, and adds its wait, 0.5 × 0.571 / 0.429 × 10 ms = 6.66 ms (Pollaczek–Khinchine for
  deterministic service: half the M/M/1 wait), plus 0.2 ms for the first megabyte: the run's
  series prints 6.86 ms on the link at 21:00. The pipe adds those 6.86 ms to the miss path, and
  10 ms to the fitted p99 (from 455 ms with no bandwidth stated, where the link is never a
  bottleneck, to 465 ms; "The day's run" says why that p99 is a fit, not a path). Real links
  interleave their flows packet by packet instead of sending one object at a time: a stream's first
  megabyte would then take about 0.2 ms / (1 − 0.571) ≈ 0.47 ms in all (processor sharing
  stretches a transfer by 1 / (1 − rho)), so the model's whole-object wait is an upper bound.
- `original_store`'s `then` is an S3-style event notification: each upload it stores
  publishes one message to `transcode_q`, asynchronously (the uploader is answered by the store
  alone). Only a write fires an event (`upload`, the store's `write_ops`), as S3 notifies on
  ObjectCreated and never on a GET.
- Each request is a batch of megabytes that holds one slot for its whole transfer (a stream
  0.5 s at a CDN, an upload 300 MB × 40 ms = 12 s at `original_store`), so the queues wait for
  whole objects: Erlang C is fed the mean object's time, with the slots as servers (rho is the
  same as in MB/s, by Little's law). A fixed size at a fixed rate takes a fixed time, so the
  transfers are deterministic (`variability=0` in `explain`) and the spread of service times is
  the size mix's alone, which Kingman's correction (Allen–Cunneen) adds to the wait: one size
  waits like M/D/c, half the M/M/c wait, and small requests beside large ones lengthen the large
  ones' wait. It matters near saturation: at rho 0.9, 50 MB objects at a default blob store
  (20 slots) wait 0.28 s, where counting 1 MB units, as the first version of the model did, said
  11 ms. And no wait outlasts the waiting room:
  `original_store`'s 1000 MB drain at 5 GB/s in 0.2 s, so an upload waits at most 0.2 s there
  even past saturation. At the 21:00 peak (rho 0.576 over 200 slots) it waits nothing
  measurable. When a request is answered depends on its direction: a stream is a read, answered
  at its first megabyte (its wait plus 10 ms; the rest is read as it arrives), and an upload is a
  write (`original_store` lists `write_ops: [upload]`), acknowledged at its last byte, 12 s after
  its wait.
- Transcoding as a long-running worker pool: 600 workers at 60 s a video, 600 / 60 s = 10
  videos/s, against the 9.6 uploads/s of 21:00 (480 requests/s × 2 %). `transcode_q` is the
  broker: an upload's event is answered at the append, and the transcoders drain it as an M/M/c
  queue of 600 servers. `explain` offers the pool every upload of the peak:
  `transcoders  peak offered 9.6/s vs 10/s total capacity  (96%, red)`. Red is a utilization
  verdict (from 90 %), not a failure: the pool keeps up, nothing backs up, and a video waits only
  the Erlang C wait, C(600, 0.96) × 60 s / (600 × 0.04) = 0.229 × 2.5 s = 0.57 s, which is
  `transcode_q`'s peak lag in the run (573 ms). The run counts the transcoders red for 16.11k s
  (4.5 h, 18:46 to 23:14), the hours the uploads are above 9/s.

  **How a queue-fed pool is sized** (the same rule as the search-autocomplete aggregators'): by
  its lag budget, not by a utilization colour. Nobody waits on the pool in a request, so red
  does not mean drops or slow answers; it means a burst becomes lag. 600 is the smallest round
  pool above the 9.6 uploads/s of the peak (500 drain 8.333/s and fall behind, "Things to try");
  at 96 % it has 0.4 videos/s to spare, so a burst queues and drains slowly, and the lag it adds
  is the backlog over the pool's rate: 1,000 extra uploads at the peak add 1,000 / 10/s ≈ 100 s
  of lag, against a 30-minute budget. (Measured: doubling the traffic for 2 minutes at 21:00
  queues 803 extra videos, as `original_store` passes 16.67 uploads/s of the 19.2 offered, and
  `transcode_q`'s lag peaks at 80.3 s = 803 / 10/s.) The pooling effect is why 96 % is safe here:
  600 slots at 96 % wait under a second, where the same 96 % at 4 slots would wait minutes
  (Erlang C falls fast as c grows).
- Fan-out of one upload into four output renditions: each video the transcoders drain (not each
  one that arrives) writes four 50 MB renditions to `transcoded_store` and publishes one message
  to `completion_q`, whose `completion_handler` updates `metadata_db` and `metadata_cache`
  (`completion_handler  peak offered 9.6/s vs 800/s total capacity  (1.2%, green)`). Nobody
  waits for the pool, so none of this is in any viewer's response time. `metadata_db` is offered
  98.28 units/s at the peak (the API's upload_meta writes and cache misses, and the handler's
  writes, 5 units each) of its 9k: 1.09 %.

## Uploads and the timeout
A 300 MB upload is a write, acknowledged at its last byte: 300 MB × 40 ms per MB = 12 s at
`original_store`, plus the link there and back (0.5 ms in the US, 150 ms across the Atlantic).
With `timeout: 10s` on the clients, which an earlier version of this file had, **every** upload
times out (`timed out/s` equal to `offered/s`, `>10s` in its percentiles) although the store
stores each one. The
book's answer is resumable uploads: the file goes up in chunks, and a client that gives up gives up
on a chunk and retries it, never on the whole file. The model has one timeout per client, not one
per chunk, so that timeout must exceed the whole file's acknowledgement: 12 s in the US, 12.15 s in
Europe. Anything above that passes (measured: at `12.5s` and at `13s` no upload times out). The
viewers' `30s`, 2.5 times the acknowledgement, is a margin chosen so that an upload is never
abandoned whole, which is what resumable uploads achieve. In the run no upload times out: the US
uploads are answered at 12 s (mean and p99), the European ones at 12.15 s (mean and p99, which the
table rounds to 12.1 s and 12.2 s). Streams are judged by their own p99 SLO, not by this timeout (a
stream answers at its first megabyte, in milliseconds); the European stream row's `timed out/s`,
5.6e-11, is the fitted lognormal's tail past 30 s (below), not a stream anyone waited 30 s for.
Still abstracted: the chunks themselves (a chunk lost is retried, not the file) and every retry; a
request that times out is simply counted, nobody sends it again.

## The day's run
`simsys run` steps 24 h at 100 ms (861,000 steps observed after the 5 m warmup; about a minute
of CPU in a Release build on the author's machine) and exits 0, every SLO passing:

```
slo
  status  metric       node         op      observed  max
  PASS    p99_latency  viewers_eu   stream  465ms     2s
  PASS    queue_lag    transcode_q  -       573ms     30m
  PASS    utilization  cdn_us       -       0.1403    0.8  (worst 1h from 20:05)
```

- The European stream p99, 465 ms against 2 s, is the lognormal fit's, not a path's. A European
  stream takes one of two times: a hit, answered at the edge's first megabyte in 10.5 ms (10 ms
  plus the two 0.25 ms links; 59.4 % of streams), or a miss, which crosses the Atlantic and back
  (150 ms), waits for the pipe (6.86 ms) and reads its first megabyte at `transcoded_store`
  (40 ms): 10.5 + 150 + 6.86 + 40 = 207 ms (40.6 %). Their mean, 0.594 × 10.5 + 0.406 × 207 =
  90.4 ms, is the run's (90.35 ms at 21:00), but the report's percentiles come from a lognormal
  fitted to each op's mean and variance, and a lognormal stretched over two spikes has a long
  tail: its p99 lands at 465 ms, more than twice the slowest European stream, a miss at about
  207 ms. The SLO passes with room either way. The US streams show the same: hit or miss inside
  one region, at most 10.5 + 0.5 (the link to the store and back) + 40 = 51 ms, reported p99
  97.6 ms. (The search-autocomplete
  README shows the fit's other face, a p99 that *falls* when its two populations merge into one.
  The schema's "[How percentiles are estimated, and where they
  mislead](../../../docs/scenario-schema.md#how-percentiles-are-estimated-and-where-they-mislead)"
  says why the reports label the percentiles rather than clip them at a path's slowest time.)
- `transcode_q`'s lag, 573 ms against 30 minutes: the Erlang wait above.
- `cdn_us` with `over: 1h`: the SLO is checked on every hour of the day (aligned from the end of
  the warmup), and the worst, 20:05 to 21:05, is 0.1403. Without `over` the SLO judged the
  day's mean, 0.0879, which the quiet night pulls down: a busy evening could hide behind it.

At the 21:00 peak, each figure against its back-of-the-envelope derivation (the run's series):

| node | at 21:00 | of | rho |
|---|---|---|---|
| `cdn_us` | 14.08k MB/s (281.6 streams/s × 50 MB) | 100k MB/s | 0.141 |
| `cdn_eu` | 7.04k MB/s (140.8 × 50 MB) | 50k MB/s | 0.141 |
| `original_store` | 2.88k MB/s (9.6 × 300 MB) | 5k MB/s | 0.576 |
| `transcoded_store` | 9.811k MB/s (7.891k misses + 1.92k renditions) | 20k MB/s | 0.491 |
| `cdn_eu->transcoded_store` | 2.856k MB/s | 5k MB/s | 0.571 |
| `transcoders` | 9.6 videos/s | 10/s | 0.96 |
| `metadata_db` | 98.28 units/s | 9k units/s | 0.011 (3.2 % at the primary) |

**`estimate` is not the run's verdict here.** `simsys estimate` evaluates one steady state with
the curve frozen at t = 0, which is 00:00, three hours after the peak (8.55 uploads/s, 427 requests/s).
With the shipped 600 workers it passes everything, but with 00:00's numbers: `transcoders` at
rho 0.8546, `transcode_q`'s lag 67.6 µs (the run's is 573 ms), `cdn_us` 0.1253 (the worst hour
is 0.1403), the European p99 468 ms. With 400 workers it is worse than misleading: 8.55 uploads/s
against 6.667/s held for ever fills the queue's whole 7-day room, so `estimate` drops 22 % of the
transcoders' jobs (`drop rate 22% (worst: transcoders)`) and prints the full retention as the lag,
`FAIL queue_lag transcode_q - 168h`, where the 24 h run drops nothing and lags 2.66 h. For a
diurnal scenario with a pool, `run` is the verdict.

## What it abstracts away
- Adaptive bitrate selection, DRM.
- Chunked, resumable uploads (one timeout per client stands for the chunk's) and retries of any
  kind, in the clients or in the DAG (retry per stage).
- The jitter of real disks and networks (each transfer takes exactly its size's time;
  `advanced.service_variability` can put a spread back), and a large read needed whole (a stream
  answers at its first megabyte, which understates a download that must finish before it is
  used).
- `transcode_q`'s 32 partitions (`partitions=32 (not modelled)`): Kafka would let at most one
  consumer per partition read in parallel, which would cap 600 transcoders at 32 readers unless
  each hands jobs to a local pool; the model lets all 600 consume. The file's own `partitions:
  32` therefore contradicts its 600 workers in a real Kafka: a deployment would need at least
  600 partitions, or 32 consumers each feeding a local pool of about 19 transcoders.
- The other renditions' and thumbnails' work in the DAG: one 60 s cost per video stands for it.

## Things to try
- Sweep the pool:

  ```
  $ simsys sweep scenarios/book/youtube/scenario.yaml --param nodes.transcoders.workers=400..600:3 --metric transcode_q.lag
  nodes.transcoders.workers  transcode_q.lag
  ------------------------------------------
  400                                  2.66h
  500                                  39.5m
  600                                  573ms
  ```

  Only 600 passes the 30-minute `queue_lag` SLO. At 400 (6.667 videos/s) the uploads exceed
  the pool from 15:43, so the backlog is still growing at 24:00, where the run ends: 63.76k
  videos, 63.76k / 6.667/s = 2.66 h of lag. Its true peak comes the next morning, when the
  uploads fall back under 6.667/s at 02:17: `simsys run --duration 27h` shows it, 71.91k videos
  and 3 h of lag. At 500 (8.333/s) the excess runs from 17:42 to 00:18, so the 24 h run's 39.5
  minutes is near its peak (39.7 minutes in a 27 h run). (`estimate` cannot show any of this:
  above.)
- Halve `cdn_us.keys` to 50k: the US edge's hit ratio falls from 0.642 to the EU edge's 0.594,
  and its misses from 100.8 to 114.3 streams/s, so `explain` puts `transcoded_store` at 10.49k
  MB/s (209.7/s), 52.4 %; in the run its peak rho is 0.5244 (0.4906 as shipped) and the US
  stream p99 101 ms (97.6 ms), every SLO still passing.
- Set `timeout: 10s` on the viewers: every upload times out again (3.994 and 1.997 timed out a
  second over the day, all that is offered). The European streams' `timed out/s` rises from
  5.6e-11 to 2.3e-7, the fitted lognormal's tail past 10 s instead of past 30 s; every
  percentile and every SLO verdict stays as it was.
- Put `transcoded_store.capacity` back to `10GB/s`: `explain` turns it red (98.1 %), and in the
  run it is red for 18.62k s around the evening, its peak rho 0.9811; the European p99 rises to
  498 ms, still within 2 s.
- Lower `cdn_eu.capacity` to `5GB/s`: `explain` puts `cdn_eu` at 141 %, red (7.04k MB/s of 5k).
- Add `{at: 21:00, kind: dc_down, datacenter: eu-west, for: 5m}` in `faults` (`dc_down` is
  refused until Task 7 of phase 3 lands; the other kinds run).
