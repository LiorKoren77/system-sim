# Notification system (ch. 10)

## Status: runs end to end (phase 3)

`simsys run` steps the whole day (24 h at 100 ms, Release build) and passes all three SLOs,
exit code 0:

```
scenario notification-system  862800 steps observed  drop rate 0%
nodes
  node               mean_rho  peak_rho  peak_backlog  dropped  dead_lettered/s  rejected  deduplicated/s  red_s  peak_lag
  services                  -         -             -        -                -         -               -      -         -
  lb                  0.02174    0.2798             0        0                -         0               -      0         -
  notification_svc    0.05434    0.6994             0        0                -         0               -      0         -
  user_cache          0.04347    0.5596             0        0                -         0               -      0         -
  user_db             0.04483    0.5771             0        0                -         0               -      0         -
  user_limiter        0.02174    0.2798             0        0                -         0               -      0         -
  fanout              0.05434    0.6994             0        0                -         0           43.47      0         -
  ios_q              0.001304   0.01671             0        0                -         0               -      0     4.03m
  android_q         0.0006521  0.008275             0        0                -         0               -      0       34m
  sms_q             0.0001121  0.001416             0        0                -         0               -      0      8.3m
  email_q           0.0006455  0.008245             0        0                -         0               -      0       46m
  ios_workers          0.1003         1        3.146M        0        0.0001937         0               -   1164         -
  android_workers      0.2608         1        5.101M        0        9.828e-05         0               -   3422         -
  sms_workers          0.1246         1        448.1k        0        0.0006252         0               -   1470         -
  email_workers        0.3227         1        5.524M        0        6.187e-06         0               -   4474         -
  apns                0.08694    0.8667             0    2.25M                -         0               -      0         -
  fcm                  0.2174    0.8333             0   1.125M                -         0               -      0         -
  sms_provider        0.09343      0.75             0   483.7k                -         0               -      0         -
  email_provider       0.2582       0.8             0   556.9k                -         0               -      0         -
  log_db              0.04971    0.6399             0        0                -         0               -      0         -
...
slo
  status  metric     node   op  observed  max
  PASS    queue_lag  ios_q  -   4.03m     5m
  PASS    queue_lag  sms_q  -   8.3m      10m
  PASS    drop_rate  -      -   0         0.001
```

The providers' `dropped` are their failed calls (2 %, 2 %, 5 % and 1 % of what they serve), which
the pools retry: the drop rate leaves them out and counts what is lost for good, here nothing.
`notify` is answered in 8.68 ms on average (p99 12.8 ms): the notification service, its cache
lookup, the limiter's counter and the fan-out's publishes, which a queue answers at the append.
The fan-out's five round trips (four queues and the log's write) are sequential: `mode: parallel`
on the channel steps would make it the slowest append.

## Book design
- Services call a notification service, which looks up user devices/preferences, applies
  per-user rate limits, and publishes one message per channel to a queue.
- One worker pool per channel pulls from its queue and calls the third party
  (APNs, FCM, SMS vendor, email vendor); failures are retried, logs and analytics are recorded.
- Decoupling via queues absorbs bursts; workers are scaled per channel.

## What the model captures
- **The fan-out** (`notification_fanout`, phase 3): a service of its own (the type's 20k/s at
  1 ms a copy, c = 20) that serves every notification it is routed, duplicates included, and
  publishes along its `then` steps the 98 % its dedup step lets through: `dedup: 0.02` drops 2 % as
  duplicates (the book's "dedupe" on the event id, since a distributed system may deliver an event
  twice). The 43.47/s in `deduplicated/s` is 2 % of the day's mean 2.174k notifications/s; a
  duplicate is neither dropped nor rejected (the notification service's call succeeded; it was
  meant not to go out). Each notification becomes 0.6 + 0.3 + 0.05 + 0.3 = 1.25 channel messages
  (the steps' `factor`s: a user has several devices and channels) and one log write. `channels`
  names the queue behind each channel; it must list exactly the queues the steps publish to.
  `explain` prints both: `dedup=2% channels: android=android_q email=email_q ios=ios_q sms=sms_q`,
  and follows the steps: at the 09:00 peak of 27.93k notifications/s, `ios_q` is offered
  27.93k × 0.98 × 0.6 = 16.42k/s and `log_db` 27.37k writes/s, `54.74k/s (27.37k/s × N 2)`.
- **One queue and one pool per channel**, the pool retrying a send its provider failed
  (`retries: 3`, 1 s apart): a job whose fourth attempt fails too is a **dead letter**, counted in
  `dead_lettered/s` and never as a drop. Without the blast the four rates are the napkin's
  rate × f⁴ to seven digits (iOS 1.117k jobs/s × 0.02⁴ = 1.79e-4/s). With it the run measures
  1.937e-4/s, 5.3 % below the napkin over the day's mean (iOS 1.278k jobs/s × 0.02⁴ =
  2.045e-4/s; 3 to 6 % on the four pools), because a retried job that waits in the blast's
  backlog is counted as a first attempt when it is drained (behaviours::WorkerPoolBehaviour,
  "Ignored"). The under-count grows with how long the backlog lasts: a 1k/s probe with a 20×
  blast at 00:15 in a 1 h run (pool 100 × 10 ms, `retries: 3`, sink `failure_rate: 0.1`) measures
  1353 dead letters against the napkin's 2070 for a 15-minute blast, 35 % fewer, and 26 % fewer
  for a 5-minute one.
- **The providers' quotas**: an `external_sink` serves at most its `capacity` (the type's 1k/s
  by default) and sheds the rest, which the pool retries like a failure.
- The `user_cache` hit ratio (devices and preferences), computed with the Che approximation
  from its 5M keys and the notified users' distribution (Zipf 0.8 over a key space of 10M
  user ids, sized after the book's 10M mobile pushes a day: some users get far more
  notifications than others): `hit_ratio=0.814 (che: zipf 0.8 over 10M keys from services)`.

## Sizing: what the book's numbers imply, and what had to grow

The traffic is the book's daily volumes scaled 10x (1.9k/s on average, a wave peaking at 2.66k/s
at 18:00) and a **marketing blast at 09:00: 20x the curve for 15 minutes**, 27.93k
notifications/s, ten times the evening crest. The file was first written with the book's shape
and modest counts. Run with only the pools' `retries: 3` added, that first sizing measured (rows
of the node table):

```
scenario notification-system  862800 steps observed  drop rate 3.65% (worst: notification_svc)
  node               mean_rho  peak_rho  peak_backlog  dropped  dead_lettered/s  rejected  deduplicated/s     red_s  peak_lag
  notification_svc     0.1047         1            4k   6.846M                -         0               -     900.2         -
  user_db              0.1271         1            1k   640.5k                -         0               -     900.5         -
  ios_q              0.002147   0.01286             0        0                -         0               -         0     2.28h
  sms_q             0.0001076     0.001             0        0                -         0               -         0     21.2m
  email_q           0.0006192  0.005887             0        0                -         0               -         0     2.01h
  ios_workers          0.7635         1        20.53M        0             82.6         0               -  5.47e+04         -
  apns                 0.9251         1            1k   86.47M                -         0               -  6.15e+04         -
  status  metric     node   op  observed  max
  FAIL    queue_lag  ios_q  -   2.28h     5m
  FAIL    queue_lag  sms_q  -   21.2m     10m
  FAIL    drop_rate  -      -   0.0365    0.001
```

and `explain` had said why, before any run:

```
notification_svc  peak offered 27.93k/s vs 20k/s total capacity  (140%, red)
user_db           peak offered 5.184k units/s vs 6k/s total capacity  (86.4%, red at the busiest copy)  primary 0 units/s vs 3k/s (0%, green), each replica 5.184k units/s vs 3k/s (173%, red)
fanout            peak offered 27.93k/s vs 20k/s total capacity  (140%, red)
apns              peak offered 16.76k/s vs 1k/s total capacity  (1676%, red)
log_db            peak offered 54.74k/s (27.37k/s × N 2) vs 60k/s (3 nodes)  (hottest 95.8%, red)
```

Two different rules size the two halves of the design, and that is the chapter's point about
decoupling with queues.

**In front of the queues, size for the peak as it comes.** Nothing buffers a request between the
services and the fan-out: what a node cannot serve waits in its 1000-request room and is dropped.
The 4 notification servers dropped about a quarter of the blast, 6.846M notifications, 3.65 % of
the day's, the whole drop rate. Every number here is sized so the blast is at most about 70 % of
the capacity:
- `notification_svc`: 8 replicas of 5k/s, 27.93k/s of 40k/s = 69.8 % (it was 140 %);
- `user_db`: its misses (1 − 0.814 = 18.6 % of the reads, 5.184k/s) go to the read replicas, so
  `replicas: 4` (the primary and 3 read replicas, 1.728k/s each of 3k/s, 57.6 %; with one read
  replica it was 173 %);
- `fanout`: 2 copies of the type's 20k/s, 69.8 %;
- `log_db`: machines of 30k/s instead of 20k/s (the type's own figure is 50k/s), the ring's hottest
  of 3 at 63.9 % (it was 95.8 %).

**Behind the queues, size for the lag you promise.** A queue turns the blast into a backlog
instead of drops (its room is 7 days of the pool's capacity), and the pool works it off at its
`workers / cost`. A 15-minute blast of λ messages/s into a pool of capacity μ < λ ends
(λ/μ − 1) × 15 min behind (the backlog (λ − μ) × 15 min, drained at μ), so a lag SLO of L
minutes needs **μ ≥ λ × 15 / (15 + L)**, with λ counting the retries' attempts (1 + f + f² + f³):
- iOS, `queue_lag` at most 5 min: λ = 16.42k/s × 1.02 = 16.76k/s, μ ≥ 12.6k/s: 260 workers at
  20 ms = 13k/s (lag 4.03 min). The 50 workers first shipped (2.5k/s) were 2.28 h behind.
- SMS, at most 10 min: λ = 1.368k/s × 1.05 = 1.44k/s, μ ≥ 865/s: 45 workers at 50 ms = 900/s (lag
  8.3 min). The 20 first shipped (400/s) were 21.2 min behind.
- Android and e-mail have no lag SLO. Android keeps its 50 workers (2.5k/s, three times the
  18:00 crest of 782/s) and falls 34 min behind at worst; e-mail's 20 workers at 30 ms (667/s)
  did not keep up even with the crest, so it gets 60 (2k/s) and falls 46 min behind at worst.

That is what a lag promise over a burst costs: the iOS fleet is sized for three quarters of the
blast's rate and is 10 % busy over the day (`mean_rho` 0.1003). A promise of an hour would need
16.76k × 15 / 75 = 3.35k jobs/s, 68 workers (run: 56.9 min behind, 38 % busy); a marketing blast
is often given exactly that kind of looser bound, or a queue of its own.

**The providers' quotas** must stay above what their pool sends at full speed, or the quota sheds
the excess, the pool retries it as a failure, and its retries load the quota further. At its
default 1k/s APNs was offered the iOS pool's 2.5k/s for hours: 92.5 % busy over the day and 82.6
dead letters a second. Each quota is now above its pool's `workers / cost`: APNs 15k/s for
13k/s, FCM 3k/s for 2.5k/s, the SMS gateway 1.2k/s for 900/s, the e-mail service 2.5k/s for
2k/s. (In `explain` the pools and providers are still red at the peak: its checks do not queue,
so they are offered the blast itself, `ios_workers  peak offered 16.76k/s vs 13k/s total capacity
(129%, red)`. The run shows the queues absorbing it.)

## What it abstracts away
- What makes two notifications duplicates and how long their ids are kept (the book's dedup
  cache with a TTL): the share is a stated number, not derived from the traffic.
- Exponential backoff and jitter (a pool's retries each wait the same `retry_after`), and the
  dead-letter queue itself: the model counts the dead letters, nobody reads or replays them.
- A worker blocked on its provider's 150–500 ms round trip: a worker is busy for its `cost`
  only, so the providers' latency does not cut the pools' capacity.
- Template rendering, user opt-outs (folded into the channel `factor`s), delivery receipts,
  partitions and ordering.

## Things to try
Each was run at the shipped 100 ms step (Release build, `simsys run`).
- Cut `ios_workers.workers` back to 50: `ios_q` falls 1.38 h behind and fails its 5-minute SLO;
  nothing is lost (drop rate 0), the queue holds it all.
- Cut `sms_workers.workers` to 5 (100/s, below the 18:00 crest's 130 SMS/s): `sms_q` falls
  5.36 h behind and fails its 10-minute SLO.
- Remove `apns`'s `capacity` (back to the type's 1k/s quota): `ios_q` is 16.9 min behind and the
  iOS pool dead-letters 371.6 jobs a second, the quota's shedding retried until it runs out.
- Set `apns.failure_rate` to 0.3 (an outage): every job is attempted 1.42 times
  (1 + 0.3 + 0.3² + 0.3³), the iOS pool dead-letters 9.82 a second (about 0.3⁴ of its jobs) and
  `ios_q` misses its SLO at 8.2 min: the retries are 42 % more work for the same 260 workers.
- Remove the four pools' `retries`: the SMS gateway's 5 % failures are lost for good and the
  drop rate fails at 0.05 (the queues, without the retries' extra attempts, run a little faster:
  iOS 3.73 min).
- Raise the fan-out's `dedup` and watch `deduplicated/s` and every channel's load move with it:
  `simsys sweep scenario.yaml --param nodes.fanout.dedup=0.02..0.2:4 --metric fanout.deduplicated
  ios_workers.mean_rho` prints one row per value.
