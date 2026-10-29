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
  (`c=1000 slots (1 object each)`) and saturates at 2k streams/s. `explain` checks them in MB/s with the requests beside (the evening
  peak, 21:00):

  ```
  cdn_us              peak offered 14.08k MB/s (281.6/s) vs 100k MB/s total capacity  (14.1%, green)
  cdn_eu              peak offered 7.04k MB/s (140.8/s) vs 50k MB/s total capacity  (14.1%, green)
  original_store      peak offered 2.88k MB/s (9.6/s) vs 5k MB/s total capacity  (57.6%, green)
  transcoded_store    peak offered 7.891k MB/s (157.8/s) vs 10k MB/s total capacity  (≥78.9%; load not propagated: worker_pool and message_queue are phase 2c)
  ```

  `transcoded_store`'s figure is the CDNs' misses alone, a lower bound: the transcoders' writes
  come through a queue and a worker pool the paper load does not follow yet.
- `original_store`'s `then` is an S3-style event notification (phase 2c): each upload it stores
  publishes one message to `transcode_q`, asynchronously (the uploader is answered by the store
  alone), so `transcode_q` now receives the store's events on paper:

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
  but its wait's. The clients' `timeout: 10s` is read but not applied yet: applied as it stands
  it would time out **every** upload (plan 2c Task 13 honours the timeout). The book's uploads
  are resumable (chunked, each chunk retried on its own), so plan Task 16 sizes the timeout per
  the book (a timeout per chunk, or a longer one for `upload`) before Task 13 applies it.
- Two regions with a cross-DC origin fetch (150 ms) for European misses. Each CDN is a
  read-only cache in front of `transcoded_store`: its hit ratio comes from the Che approximation
  over the viewers' Zipf(1) popularity of 10M videos and the edge's size in videos (`keys: 100k`
  in the US, `50k` in the EU), and only the misses reach the origin. The CDNs and both blob
  stores run (`original_store` with its upload event); `run` stops first at `transcode_q`, a
  message queue (later in phase 2c).
- Transcoding as a long-running worker pool (60 s per video, 400 workers → 6.7 videos/s
  ceiling) and the backlog it builds during the evening upload peak.
- Fan-out of one upload into four output renditions.

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
