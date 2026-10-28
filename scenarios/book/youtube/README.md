# Video streaming / YouTube (ch. 14)

## Book design
- Upload: client gets a presigned URL from the API and uploads straight to original storage;
  a message triggers the transcoding DAG (inspection, encoding at several resolutions,
  thumbnails, watermarks); outputs land in transcoded storage; a completion handler updates
  metadata DB and cache.
- Streaming: adaptive bitrate from the CDN, which pulls from transcoded storage on miss.
- Cost: CDN is the bill; serve only popular videos from CDN, the long tail from origin.

## What the model captures
- Bytes, not just requests, once phase 2c's byte flows exist: the scenario already states its
  50 MB streams and 300 MB uploads (`sizes`) and its GB/s capacities. An upload is split into
  `upload` (the bytes, straight to `original_store`) and `upload_meta` (the presigned-URL
  request and metadata write, through the API).
- Until then `explain` turns each byte capacity into requests at an assumed 1 MB per object
  only to print a number, says so (`capacity=100k/s (scenario: 100GB/s at 1MB/object; byte
  flows: phase 2c)`), and gives the check on it no percentage and no zone (`not checked`):
  `cdn_us`, `cdn_eu`, `original_store` and `transcoded_store`. At 1 MB per object `cdn_us` would
  read 0.28 % green; with the scenario's 50 MB streams its 281.6 streams/s are 14 GB/s, 14 % of
  100GB/s, `transcoded_store`'s 157.8 misses/s are 7.9 GB/s, 79 % of 10GB/s (yellow, not
  1.58 % green), and `original_store`'s 9.6 uploads/s are 2.9 GB/s, 58 % of 5GB/s. The request
  counts on those lines (and the two hit ratios) are the model's; the verdicts wait for 2c.
- Two regions with a cross-DC origin fetch (150 ms) for European misses. Each CDN is a
  read-only cache in front of `transcoded_store`: its hit ratio comes from the Che approximation
  over the viewers' Zipf(1) popularity of 10M videos and the edge's size in videos (`keys: 100k`
  in the US, `50k` in the EU), and only the misses reach the origin. The CDNs are sized in bytes
  (`capacity: 100GB/s`), so the scenario explains but does not run before phase 2c's byte flows.
- Transcoding as a long-running worker pool (60 s per video, 400 workers → 6.7 videos/s
  ceiling) and the backlog it builds during the evening upload peak.
- Fan-out of one upload into four output renditions.

## What it abstracts away
- Adaptive bitrate selection, chunked upload resume, DRM.
- Error handling in the DAG (retry per stage).

## Things to try
- `transcoders.workers: 200` — `transcode_q` lag grows all day and never recovers.
- Lower `cdn_eu.capacity` or raise EU traffic — watch the origin link saturate (phase 2c: until
  byte flows exist, `explain` shows the miss rate at `transcoded_store` but checks no bytes).
- Halve `cdn_us.keys` to 50k — `explain` shows its hit ratio fall from 0.642 to the EU edge's
  0.594 and `transcoded_store`'s miss load rise.
- Add `{at: 21:00, kind: dc_down, dc: eu-west, for: 5m}` in `faults`.
