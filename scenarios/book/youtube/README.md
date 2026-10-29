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
- The CDNs and the two blob stores count megabytes (phase 2c): a 50 MB stream costs 50 units of
  1 MB, the service time is one megabyte's (10 ms at a CDN, 40 ms at a blob store, the book's
  per-MB rows), so `cdn_us` at 100GB/s has 1000 slots that each move one stream at a time
  (`c=1000 slots (1 object each)`) and saturates at 2k streams/s. `explain` checks them in MB/s
  with the requests beside (the evening peak, 21:00):

  ```
  cdn_us              peak offered 14.08k MB/s (281.6/s) vs 100k MB/s total capacity  (14.1%, green)
  cdn_eu              peak offered 7.04k MB/s (140.8/s) vs 50k MB/s total capacity  (14.1%, green)
  original_store      peak offered 2.88k MB/s (9.6/s) vs 5k MB/s total capacity  (57.6%, green)
  transcoded_store    peak offered 9.811k MB/s (196.2/s) vs 10k MB/s total capacity  (98.1%, red)
  ```

  `transcoded_store`'s figure is the CDNs' misses, 157.8 streams/s × 50 MB = 7.891k MB/s, plus
  the transcoders' renditions, 9.6 uploads/s × 4 × 50 MB = 1.92k MB/s: 98.1 % of its 10 GB/s,
  red (plan 2c Task 16 sizes it).
- `original_store`'s `then` is an S3-style event notification (phase 2c): each upload it stores
  publishes one message to `transcode_q`, asynchronously (the uploader is answered by the store
  alone), so `transcode_q` now receives the store's events on paper. Only a write fires an event
  (`upload`, the store's `write_ops`), as S3 notifies on ObjectCreated and never on a GET; a read
  of the original would notify nobody:

  ```
  transcode_q         peak offered 9.6/s vs 1M/s total capacity  (0.00096%, green)
  ```
- Each request is a batch of megabytes that holds one slot for its whole transfer (a stream
  0.5 s at a CDN, an upload 300 MB × 40 ms = 12 s at `original_store`), so the queues wait for
  whole objects: Erlang C is fed the mean object's time, with the slots as servers (rho is the
  same as in MB/s, by Little's law). A fixed size at a fixed rate takes a fixed time, so the
  transfers are deterministic (`variability=0` in `explain`) and the spread of service times is
  the size mix's alone, which Kingman's correction (Allen–Cunneen) adds to the wait: one size
  waits like M/D/c, half the M/M/c wait, and small requests beside large ones lengthen the large
  ones' wait. It matters near saturation: at rho 0.9, 50 MB objects at a default blob store
  (20 slots) wait 0.28 s, where 1 MB units would say 11 ms. And no wait outlasts the waiting room:
  `original_store`'s 1000 MB drain at 5 GB/s in 0.2 s, so an upload waits at most 0.2 s there
  even past saturation (Erlang C at its 0.99 clamp would say 2.5 s). At the 21:00 peak (rho
  0.576 over 200 slots) it waits nothing measurable. When a request is answered depends on its
  direction: a stream is a read, answered at its first megabyte (its wait plus 10 ms; the rest
  is read as it arrives), and an upload is a write (`original_store` lists `write_ops: [upload]`),
  acknowledged at its last byte, 12 s after its wait.
- Every 300 MB upload therefore takes at least 12 s (300 MB at 25 MB/s per slot), with no spread
  but its wait's. Since plan 2c Task 13 the clients' `timeout: 10s` is applied, and as shipped
  it times out **every** upload: each would be answered at about 12 s (12.0 s in the US, 12.1 s
  in the EU before the timeout), so `run --report json` shows `timed_out_per_s` equal to
  `offered_per_s` and `answered_per_s` 0 for both clients' `upload`, and the table prints `-`
  for its latencies (nothing answered). The uploads still reach `original_store` and the
  transcoders: nothing cancels them, so every other number of `run`'s table is as before. The
  book's uploads are resumable (chunked, each chunk retried on its own); plan Task 16 resolves
  this, with resumable uploads or a timeout sized for `upload`.
- Two regions with a cross-DC origin fetch (150 ms) for European misses. Each CDN is a
  read-only cache in front of `transcoded_store`: its hit ratio comes from the Che approximation
  over the viewers' Zipf(1) popularity of 10M videos and the edge's size in videos (`keys: 100k`
  in the US, `50k` in the EU), and only the misses reach the origin.
- Transcoding as a long-running worker pool (60 s per video, 400 workers → 6.7 videos/s
  ceiling) and the backlog it builds during the evening upload peak. `transcode_q` is the
  broker: an upload's event is answered at the append, and the transcoders drain it as an M/M/c
  queue of 400 workers at 60 s a job. On paper the pool is offered every upload of the peak,
  `transcoders  peak offered 9.6/s vs 6.667/s total capacity  (144%, red)`; in a run it drains
  6.667 videos/s at most and the rest waits in its queue.
- Fan-out of one upload into four output renditions: each video the transcoders DRAIN (not each
  one that arrives) writes four 50 MB renditions to `transcoded_store` and publishes one message
  to `completion_q`, whose `completion_handler` updates `metadata_db` and `metadata_cache`
  (`completion_handler  peak offered 9.6/s vs 800/s total capacity  (1.2%, green)`). Nobody
  waits for the pool, so none of this is in any viewer's response time.
- The whole day runs: `simsys run` steps 24 h at 100 ms (861,000 steps observed after the
  warmup, about 19 s in a Release build) and exits 2, one SLO failing:

  ```
  slo
    status  metric       node         op      observed  max
    PASS    p99_latency  viewers_eu   stream  456ms     2s
    FAIL    queue_lag    transcode_q  -       2.66h     30m
    PASS    utilization  cdn_us       -       0.08787   0.8
  ```

  From 15:43 the uploads outrun the transcoders' 6.667 videos/s, so they run saturated (peak
  rho 1) and their backlog grows into the night: the run's `peak_backlog`, 63.76k videos, is its
  last step's, at midnight, with the inflow (8.55/s) still above their capacity. `transcode_q`'s
  lag is that backlog's FIFO drain time, 63.76k / 6.667/s = 9,564 s = 2.66 h (the node table's
  `peak_lag`), against the 30 minutes the SLO allows: the 400 transcoders are too few on purpose
  until plan 2c Task 16 sizes them. (`completion_q`'s lag is its handlers' Erlang wait alone, a
  fraction of a nanosecond.) Midnight is not the day's peak, which comes after the run ends:
  about 71.9k videos (3.0 h of lag) near 02:18 (`--duration 27h` shows it), so the SLO's 2.66 h
  understates the worst lag of a day. The transcoders' 48.27k s (13.4 h) in the red zone counts
  00:00 to 02:18 as well, when the run's queue fills from empty where the real one would still
  be working off the evening before. `transcoded_store` is red for 10.79k s (3.0 h) around the
  evening peak (peak rho 0.9225: the CDNs' misses and the renditions, 98.1 % on paper). (Uploads
  take 12 s, past the clients' 10 s timeout, so every one of them times out: above.)

## What it abstracts away
- Adaptive bitrate selection, chunked upload resume, DRM.
- The jitter of real disks and networks (each transfer takes exactly its size's time;
  `advanced.service_variability` can put a spread back), and a large read needed whole (a stream
  answers at its first megabyte, which understates a download that must finish before it is
  used).
- Error handling in the DAG (retry per stage).

## Things to try
- `transcoders.workers: 200` — `transcode_q` lag grows all day and never recovers.
- Lower `cdn_eu.capacity` to `5GB/s` — `explain` puts `cdn_eu` at 141 %, red (7.04k MB/s of 5k);
  watching the origin link saturate needs link bandwidth (later in phase 2c).
- Halve `cdn_us.keys` to 50k — `explain` shows its hit ratio fall from 0.642 to the EU edge's
  0.594 and `transcoded_store`'s miss load rise.
- Add `{at: 21:00, kind: dc_down, dc: eu-west, for: 5m}` in `faults`.
