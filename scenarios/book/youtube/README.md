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
  per-MB rows), so `cdn_us` at 100GB/s holds 1000 transfers at once and saturates at 2k
  streams/s. `explain` checks them in MB/s with the requests beside (the evening peak, 21:00):

  ```
  cdn_us              peak offered 14.08k MB/s (281.6/s) vs 100k MB/s total capacity  (14.1%, green)
  cdn_eu              peak offered 7.04k MB/s (140.8/s) vs 50k MB/s total capacity  (14.1%, green)
  original_store      peak offered 2.88k MB/s (9.6/s) vs 5k MB/s total capacity  (57.6%, green)
  transcoded_store    peak offered 7.891k MB/s (157.8/s) vs 10k MB/s total capacity  (≥78.9%; load not propagated: worker_pool, message_queue and blob_store are phase 2c)
  ```

  `transcoded_store`'s figure is the CDNs' misses alone, a lower bound: the transcoders' writes
  come through a queue and a worker pool the paper load does not follow yet. A request's response
  is its time to first byte (its wait plus one megabyte): the rest of a 50 MB stream is read as it
  arrives, and the transfer time of a 300 MB upload is not in any percentile.
- Two regions with a cross-DC origin fetch (150 ms) for European misses. Each CDN is a
  read-only cache in front of `transcoded_store`: its hit ratio comes from the Che approximation
  over the viewers' Zipf(1) popularity of 10M videos and the edge's size in videos (`keys: 100k`
  in the US, `50k` in the EU), and only the misses reach the origin. The CDNs and
  `transcoded_store` run; `original_store` runs once its `then` (the upload event that starts
  transcoding) does, and `run` stops first at `transcode_q`, a message queue (both later in
  phase 2c).
- Transcoding as a long-running worker pool (60 s per video, 400 workers → 6.7 videos/s
  ceiling) and the backlog it builds during the evening upload peak.
- Fan-out of one upload into four output renditions.

## What it abstracts away
- Adaptive bitrate selection, chunked upload resume, DRM.
- Error handling in the DAG (retry per stage).

## Things to try
- `transcoders.workers: 200` — `transcode_q` lag grows all day and never recovers.
- Lower `cdn_eu.capacity` to `5GB/s` — `explain` puts `cdn_eu` at 141 %, red (7.04k MB/s of 5k);
  watching the origin link saturate needs link bandwidth (later in phase 2c).
- Halve `cdn_us.keys` to 50k — `explain` shows its hit ratio fall from 0.642 to the EU edge's
  0.594 and `transcoded_store`'s miss load rise.
- Add `{at: 21:00, kind: dc_down, dc: eu-west, for: 5m}` in `faults`.
