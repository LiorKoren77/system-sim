# Google Drive (ch. 15)

## Status: `explain` only until phase 3

This scenario validates (`OK: google-drive (12 nodes, 14 links)`) and `simsys explain` prints its
whole resolved graph, but `run` and `estimate` refuse it with exit code 1:

```
scenarios/book/google-drive/scenario.yaml: error: node 'long_poll' has type 'ws_gateway', which run/estimate do not support yet (phase 3)
```

`long_poll`, the notification service's long-polling connections, is a `ws_gateway`, a type phase 3
models (see [What runs in phase 2c](../../../docs/scenario-schema.md#what-runs-in-phase-2c)). Every
other node is of a type that runs today.

What `explain` shows today. Its checks compare each node's peak offered load (the 11:00
office-hours peak) with its capacity, without queueing. They follow the load through the gateway
as if it were a plain service, so no line carries a `load not propagated` mark. One line is red
and one yellow:

```
backup_workers    peak offered 255.6/s vs 200/s total capacity  (128%, red)
cloud_storage     peak offered 1.555k MB/s (3.11k/s) vs 2k MB/s total capacity  (77.7%, yellow)
```

`backup_workers` has 2 workers at 10 ms a job, 200 jobs/s, and the queue hands it a copy of every
upload, 255.6/s at the peak. Its backlog would grow for as long as uploads stay above 200/s. With
`workers: 3` the line reads `255.6/s vs 300/s total capacity  (85.2%, yellow)`. When phase 3 runs
this file, the pool needs that sizing, unless a growing backup backlog is what the scenario sets out
to show.

## Book design
- Block servers split files into 4 MB blocks, compress and encrypt them, and store only
  changed/unique blocks (delta sync + dedup) in cloud storage; cold storage for backups.
- Metadata DB (sharded, replicated) holds users, files, blocks, versions, devices;
  a metadata cache in front.
- Notification service pushes "file changed" to other devices via long polling.
- Upload flow and download flow are separate paths; offline backup queue decouples storage.

## What phase 3 will model
Once `ws_gateway` runs, `run` will show the following over the day. `explain` already resolves
every number below.
- The office-hours diurnal shape with a 70% amplitude.
- Block-server CPU cost (20 ms per request) as a capacity limit distinct from storage bandwidth.
  `explain` reads `block_servers` at 64.8 % and `cloud_storage` at 77.7 % of its 2 GB/s.
- Dedup halving stored bytes (`factor: 0.5` on the upload's write), and one upload notifying two
  other devices on average.
- Two consumer groups on one queue (`notification_svc`, `backup_workers`). Each is offered a copy
  of every message, 255.6/s at the peak.

## What it abstracts away
- Conflict resolution, versioning, delta sync granularity.
- Upload resumption and client-side chunk scheduling.

## Things to try
With `explain`, today (edit a copy of the file):
- `block_servers.replicas: 4`: `block_servers  peak offered 3.238k/s vs 2k/s total capacity  (162%, red)`.
- `backup_workers.workers: 3`: the red line above turns yellow at 85.2 %.

With `run`, once phase 3 runs the gateway:
- `block_servers.replicas: 4`: see uploads queue at 11:00.
- `metadata_db.shards: 1`: does the 100 ms SLO on metadata reads still hold? `explain` puts the
  primary at 31.9 % and each replica at 36.1 %, all green, so the static check predicts no
  failure. Only a run can say what the queues and the replication lag add.
