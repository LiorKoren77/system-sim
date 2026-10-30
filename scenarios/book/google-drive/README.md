# Google Drive (ch. 15)

## Book design
- Block servers split files into 4 MB blocks, compress and encrypt them, and store only
  changed/unique blocks (delta sync + dedup) in cloud storage; cold storage for backups.
- Metadata DB (sharded, replicated) holds users, files, blocks, versions, devices;
  a metadata cache in front.
- Notification service pushes "file changed" to other devices via long polling.
- Upload flow and download flow are separate paths; offline backup queue decouples storage.

The scenario's clients offer 3k requests/s on an office-hours curve (`diurnal`, amplitude 0.7)
that tops at 11:00 with 4.26k/s (`explain`'s peak), as 70 % `download`, 16 % `metadata`, 6 %
`upload` (the 500 KB file, straight to the block servers), 6 % `upload_meta` (the metadata write,
through the API, which publishes a "file changed" event to `notify_q`) and 2 % `sync_poll`. Two
consumer groups read `notify_q`, each a copy of every event: `notification_svc` pushes it to two
other devices on average (`factor: 2`), and `backup_workers` copies the file to cold storage.

### The notification long polls: a fixed level, not a `connect` op
In long polling a device sends a request that the server holds open until something changes or
the poll times out, and then sends the next one at once. The open requests are therefore the
online devices, and holding them is what `long_poll`, a `ws_gateway`, models: a **level** of open
connections, not a queue of work (see [`ws_gateway`](../../../docs/scenario-schema.md#ws_gateway)).

A gateway's level comes either from a client op named `connect` (the level is then a balance of
connects and sessions ending, `C* = connect rate × session` by Little's law) or from a fixed
`connections`. This file states `connections: 2M`, for three reasons:
- The devices are not what the scenario's client stands for. Its ops are the requests whose
  latency the SLOs judge; a `connect` share among them would take its part of the 3k/s and of
  every percentage above, and the file would model a device population it has no number for.
- A `connect` level starts at 0 and climbs for about a session
  (`C(t) = λ × session × (1 − e^(−t / session))`), a transient the day does not have: at 00:00 the
  devices are already connected. A fixed level is 2M on every step, the first one included.
- The book gives no connection count for ch. 15. 2M is **chosen, not from the book**: a fifth of
  its 10M daily users online at once.

`heartbeat: 60s` stands for the poll's timeout: an idle device's poll is answered empty after a
minute and sent again, so every open poll costs the gateway one request a minute,
**2M / 60 s = 33.33k re-polls a second**, all day. `long_poll` has no `heartbeat` step, so they end
there (ch. 15 has no presence service). `explain` prints the level, that load and the cap
(`max_connections × replicas` = 2M × 4 = 8M, four times the level, so no poll is ever refused):

```
long_poll  ws_gateway  ...  connections: 2M (fixed), heartbeat 33.33k/s, cap 8M
long_poll  peak offered 33.93k/s vs 80k/s total capacity  (42.4%, green)
```

The 33.93k/s at 11:00 are the 33.33k re-polls, the 511.2 pushes (255.6 events × 2 devices) and
the 85.2 `sync_poll` requests the API forwards (`fanout: 1`: the gateway relays each request
once; the two devices per event are `notification_svc`'s `factor: 2`). This
is the price of long polling the chapter mentions: **98 % of the gateway's work is idle devices
asking "anything new?"**, 65 re-polls for every push that carries news. The gateway is sized by its
idle devices, not by the uploads.

### How it is sized: `backup_workers`, 2 → 3
As first shipped the backup pool had 2 workers at 10 ms a job, 200 jobs/s, and `explain` said so in
red: `backup_workers  peak offered 255.6/s vs 200/s total capacity  (128%, red)`. `run` shows what
red means for a queue-fed pool: no drop and no slow answer (nobody waits for a backup in a
request), but lag. The uploads exceed 200/s from 07:03 to 13:28; for those six and a half hours the
backlog grows, the pool's rho is clamped at 1, and at 13:28 it holds **773.9k** jobs, which drain
at 200/s: an upload made then is backed up **1.07 h** later (773.9k / 200/s = 3,870 s). The backlog
is gone at 17:26. Nothing is dropped, since `notify_q` keeps 7 days (121M jobs at 200/s).

The scenario ships with **3 workers**: 300 jobs/s, `backup_workers  peak offered 255.6/s vs 300/s
total capacity  (85.2%, yellow)`. Why size it, rather than ship the backlog as the lesson:
- The book's design has the backup keep up. A file that exists for an hour in only one store is a
  recovery point a storage product would not accept, and the scenario now says so with a fourth
  SLO, not from the book: `queue_lag` on `notify_q`, at most **10 minutes**. A queue's lag is its
  slowest consumer group's (here the backups; the pushes are far faster), so the SLO bounds the
  "file changed" notifications too.
- A queue-fed pool is sized by its lag budget, not by a utilization colour (as youtube's
  transcoders and search-autocomplete's aggregators are). 3 is the smallest pool above the peak,
  and at rho 0.852 a job waits the Erlang C wait, `C(3, 2.556) / (300/s − 255.6/s)` = 0.734 /
  44.4/s = **16.5 ms**: `notify_q`'s peak lag in the run, against 10 minutes. 4 workers would buy
  2.35 ms of lag for a pool 64 % busy; nothing needs it.
- Keeping 2 would make the shipped file fail its own SLO (`exit 2`); the 2-worker day is the
  first thing to try (below), one key away.

The other nodes need no sizing: everything else is green at the peak but `cloud_storage` (77.7 %,
yellow, and it passes).

## The day's run
`simsys run` steps 24 h at 100 ms (861,000 steps observed after the 5 m warmup; about 24 s of CPU in
a Release build on the author's machine) and exits 0, every SLO passing:

```
slo
  status  metric       node      op        observed  max
  PASS    p99_latency  clients   download  125ms     2s
  PASS    p99_latency  clients   metadata  6.6ms     100ms
  PASS    drop_rate    -         -         0         0.001
  PASS    queue_lag    notify_q  -         16.5ms    10m
```

Nothing is dropped or refused anywhere (`drop rate 0%`), and `long_poll`'s `connections` column
reads `2M`. Latency per op (percentiles from a lognormal fitted to each op's mean and variance):

| op | offered/s (day mean) | timed out/s | mean | p50 | p95 | p99 |
|---|---|---|---|---|---|---|
| `download` | 2.1k | 0 | 64.1 ms | 61.1 ms | 101 ms | 125 ms |
| `metadata` | 479.9 | 0 | 3.16 ms | 2.98 ms | 5.23 ms | 6.6 ms |
| `sync_poll` | 59.99 | 0 | 2.71 ms | 2.53 ms | 4.64 ms | 5.96 ms |
| `upload` | 180 | 0 | 61.2 ms | 58.2 ms | 98.4 ms | 122 ms |
| `upload_meta` | 180 | 0 | 3.35 ms | 3.2 ms | 5.27 ms | 6.48 ms |

- **Download, 125 ms against 2 s.** A download is the API (1 ms), the metadata cache (0.25 ms), a
  block server (20 ms of CPU: chunk, decompress, decrypt) and the blob store's first megabyte
  (40 ms; a 500 KB block is charged a whole megabyte's time), plus five links there and back
  (2.5 ms): 63.8 ms before any wait, the run's mean 64.1 ms. Block-server CPU and storage bandwidth
  are separate limits, and neither is near its knee.
- **Metadata, 6.6 ms against 100 ms.** The metadata cache's Che hit ratio over the clients'
  Zipf(0.8) space of 500M keys with 50M slots is 0.508, so half the reads go to `metadata_db`,
  4 shards of a primary and 2 read replicas: `primary 319.5 units/s vs 4k/s (7.99%, green), each
  replica 361.4 units/s vs 4k/s (9.04%, green)`; its peak rho in the run is 0.09036 (a replica),
  and its reads are stale by 50.3 ms (the 50 ms replication lag plus the sojourns).
- **`notify_q`'s lag, 16.5 ms against 10 minutes**: the Erlang C wait above.

At the 11:00 peak, each figure against its back-of-the-envelope derivation (`peak_rho` from the
run; `explain`'s percentages agree to the rounding):

| node | at 11:00 | of | peak rho |
|---|---|---|---|
| `block_servers` | 3.238k/s (2.982k downloads + 255.6 uploads) | 5k/s (10 × 500/s) | 0.6475 |
| `cloud_storage` | 1.555k MB/s (2.982k reads × 0.5 MB + 255.6 writes × `factor: 0.5` dedup × 0.5 MB) | 2k MB/s | 0.7775 |
| `long_poll` | 33.93k/s (33.33k re-polls + 511.2 pushes + 85.2 polls) | 80k/s (4 × 20k/s) | 0.4241 (mean 0.4219) |
| `backup_workers` | 255.6 jobs/s | 300/s (3 / 10 ms) | 0.852 |
| `cold_storage` | 127.8 MB/s (255.6 × 0.5 MB) | 200 MB/s | 0.639 |
| `api` | 4.004k/s (all but `upload`) | 16k/s (8 × 2k/s) | 0.2503 |
| `metadata_db` | 1.613k units/s (a replica 361.4) | 48k/s (4 shards × 3 × 4k/s) | 0.09036 |
| `notification_svc` | 255.6 jobs/s | 8k/s (8 / 1 ms) | 0.03195 |

`long_poll`'s rho hardly moves over the day: 0.4197 at its lowest (19:00), 0.4241 at 11:00, of
which the re-polls are a constant 0.4167 (33.33k of 80k). A fixed level of idle devices is a
constant load, which the uploads only ripple.

## What it abstracts away
- Conflict resolution, versioning, delta sync granularity: a block is a 500 KB file, and dedup is
  a flat `factor: 0.5` on what reaches storage.
- Upload resumption and client-side chunk scheduling.
- The devices themselves: their number is a fixed 2M all day (no diurnal online pattern, no
  logins, no reconnect storm), each poll lasts exactly its 60 s timeout unless a push answers it
  first (the model charges the re-polls at `C / heartbeat` regardless), and the memory an open
  poll costs is the cap's business.
- Which device a push reaches: a push is one request to the gateway, not a lookup of the user's
  devices; there is no presence service.
- `notify_q`'s 8 partitions (`partitions=8 (not modelled)`): Kafka would let at most 8 consumers
  of a group read in parallel, which the 8 notification workers and 3 backup workers fit.
- One lag per queue: the model reports the slowest group's, so the notification path's own lag
  (far below 16.5 ms: 8 workers at 3 % busy) is not shown apart.

## Things to try
- **The backup pool as first shipped** (`backup_workers.workers: 2`, or sweep it:
  `simsys sweep scenarios/book/google-drive/scenario.yaml --param
  nodes.backup_workers.workers=2..4:3 --metric notify_q.lag --metric backup_workers.peak_rho
  --metric backup_workers.mean_rho`, about 70 s for the three points in parallel):

  ```
  nodes.backup_workers.workers  notify_q.lag  backup_workers.peak_rho  backup_workers.mean_rho
  --------------------------------------------------------------------------------------------
  2                                    1.07h                        1                      0.9
  3                                   16.5ms                    0.852                      0.6
  4                                   2.35ms                    0.639                     0.45
  ```

  At 2 `run` exits 2 on the lag alone, `FAIL queue_lag notify_q - 1.07h 10m`: the other three SLOs
  pass and nothing is dropped (`backup_workers ... peak_backlog 773.9k ... dropped 0`, red for
  6.425e+04 s). Cold storage looks better (peak rho 0.5 instead of 0.639): the slow pool can only
  write 200 × 0.5 MB = 100 MB/s, so a bottleneck upstream hides the load downstream.
- **Fewer block servers** (`block_servers.replicas: 4`): `explain` reads `block_servers  peak
  offered 3.238k/s vs 2k/s total capacity  (162%, red)`. In the run their rooms (4 × 1000) fill,
  35.63M requests are dropped over the day (`drop rate 18.2% (worst: block_servers)`), and the
  ones admitted wait for the room ahead of them (4k / 2k/s = 2 s): download p99 **4.65 s**,
  `FAIL p99_latency clients download 4.65s 2s` and `FAIL drop_rate - - 0.1815 0.001`, exit 2.
  `cloud_storage` falls to 0.4803: it only sees what the block servers let through.
- **One metadata shard** (`metadata_db.shards: 1`): `primary 1.278k units/s vs 4k/s (31.9%,
  green), each replica 1.446k units/s vs 4k/s (36.1%, green)`, four times the shipped 7.99 % and
  9.04 %. The run agrees (peak rho 0.3614) and the metadata p99 barely moves, 6.6 ms → 6.66 ms:
  the 100 ms SLO holds, and so does every other.
- **Twice as many re-polls** (`long_poll.heartbeat: 30s`): 66.67k/s of them, `long_poll  peak
  offered 67.26k/s vs 80k/s total capacity  (84.1%, yellow)`; the run's peak rho is 0.8408 and
  `sync_poll`'s p99 6.28 ms; every SLO still passes. Halving the poll timeout doubles the
  gateway, whatever the uploads do.
- **More devices online** (`long_poll.connections: 5M`): the re-polls alone, 83.33k/s, exceed the
  gateway's 80k/s: `(105%, red)`. The run keeps it at rho 1 all day and drops 323.2M requests there
  (`drop rate 4.48% (worst: long_poll)`, `FAIL drop_rate - - 0.04481 0.001`, exit 2), pushes among
  them; the `sync_poll`s that get in wait about 50 ms (4k in the rooms over 80k/s). The cap (8M) is
  not the limit; the gateway's CPU is.
