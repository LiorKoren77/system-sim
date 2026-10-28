# Google Drive (ch. 15)

## Book design
- Block servers split files into 4 MB blocks, compress and encrypt them, and store only
  changed/unique blocks (delta sync + dedup) in cloud storage; cold storage for backups.
- Metadata DB (sharded, replicated) holds users, files, blocks, versions, devices;
  a metadata cache in front.
- Notification service pushes "file changed" to other devices via long polling.
- Upload flow and download flow are separate paths; offline backup queue decouples storage.

## What the model captures
- Office-hours diurnal shape with a 70% amplitude.
- Block-server CPU cost (20 ms per request) as a capacity limit distinct from storage bandwidth.
- Dedup halving stored bytes; one upload notifying two other devices on average.
- Two consumer groups on one queue (`notification_svc`, `backup_workers`).

## What it abstracts away
- Conflict resolution, versioning, delta sync granularity.
- Upload resumption and client-side chunk scheduling.

## Things to try
- `block_servers.replicas: 4` — uploads queue at 11:00.
- `metadata_db.shards: 1` — metadata reads fail the 100 ms SLO.
