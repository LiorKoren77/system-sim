# Video streaming / YouTube (ch. 14)

## Book design
- Upload: client gets a presigned URL from the API and uploads straight to original storage;
  a message triggers the transcoding DAG (inspection, encoding at several resolutions,
  thumbnails, watermarks); outputs land in transcoded storage; a completion handler updates
  metadata DB and cache.
- Streaming: adaptive bitrate from the CDN, which pulls from transcoded storage on miss.
- Cost: CDN is the bill; serve only popular videos from CDN, the long tail from origin.

## What the model captures
- Bytes, not just requests: 50 MB streams and 300 MB uploads against GB/s capacities.
  An upload is split into `upload` (the bytes, straight to `original_store`) and `upload_meta`
  (the presigned-URL request and metadata write, through the API).
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
- Lower `cdn_eu.capacity` or raise EU traffic — watch the origin link saturate.
- Add `{at: 21:00, kind: dc_down, dc: eu-west, for: 5m}` in `faults`.
