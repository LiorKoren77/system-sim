# Notification system (ch. 10)

## Status: `explain` only until phase 3

This scenario validates (`OK: notification-system (20 nodes, 20 links)`) and `simsys explain`
prints its whole resolved graph, but `run` and `estimate` refuse it with exit code 1:

```
scenarios/book/notification-system/scenario.yaml: error: node 'fanout' has type 'notification_fanout', which run/estimate do not support yet (phase 3)
```

`fanout`, which splits each notification into one message per channel, is a `notification_fanout`.
Phase 3 models that type (channels, dedup, and retries with a dead-letter queue; see
[What runs in phase 2c](../../../docs/scenario-schema.md#what-runs-in-phase-2c)).

What `explain` shows today. Its checks compare each node's peak offered load with its capacity,
without queueing. The peak falls inside the 09:00 marketing blast (20× for 15 minutes). In front of
the fan-out, three nodes are red:

```
notification_svc  peak offered 27.93k/s vs 20k/s total capacity  (140%, red)
user_db           peak offered 5.184k units/s vs 6k/s total capacity  (86.4%, red at the busiest copy)  primary 0 units/s vs 3k/s (0%, green), each replica 5.184k units/s vs 3k/s (173%, red)
fanout            peak offered 27.93k/s vs 10k/s total capacity  (279%, red)
```

(`fanout` has no `capacity` of its own and reads at its type's default, 10k/s.)

`explain` does not follow a fan-out's `then` steps yet. So nothing reaches the 13 nodes behind
`fanout`, and each of them is marked instead of checked: the four queues, the four worker pools,
the four providers and `log_db`. For example:

```
ios_q             peak offered 0/s vs 1M/s total capacity  (load not propagated: notification_fanout is not modelled yet)
sms_workers       peak offered 0/s vs 400/s total capacity  (load not propagated: notification_fanout is not modelled yet)
log_db            peak offered 0/s (0/s × N 2) vs 60k/s (3 nodes)  (load not propagated: notification_fanout is not modelled yet)
```

The `0/s` is a missing number, not a zero. The two `queue_lag` SLOs and the worker counts cannot be
judged, even on paper, until phase 3.

## Book design
- Services call a notification service, which looks up user devices/preferences, applies
  per-user rate limits, and publishes one message per channel to a queue.
- One worker pool per channel pulls from its queue and calls the third party
  (APNs, FCM, SMS vendor, email vendor); failures are retried, logs and analytics are recorded.
- Decoupling via queues absorbs bursts; workers are scaled per channel.

## What phase 3 will model
Once `notification_fanout` runs, the scenario is meant to show the following.
- A marketing blast (20× for 15 minutes) absorbed by the queues: lag grows, then drains at
  the workers' rate. The per-channel worker counts decide how long the drain takes.
- Third-party latency and failure rates as the real bottleneck (20 ms worker cost vs 150–500 ms
  round trips). External sinks already run today, failing their `failure_rate` as drops. A worker
  pool's `retries` exist now too (phase 3, see
  [`message_queue` and `worker_pool`](../../../docs/scenario-schema.md#message_queue-and-worker_pool)):
  a pool with `retries: 3` puts a send its provider failed back on its queue a second later
  (`retry_after`, default 1 s), up to three times (only the pool that failed it reads it again),
  and the job whose fourth attempt fails too is a **dead letter**, counted in the pool's
  `dead_lettered/s` and never as a drop; a provider's failures on the calls of such pools leave
  its drop rate, and the `dead_letter_rate` SLO judges the jobs that run out of retries instead.
  This file's pools do not state `retries` yet (the scenario does not run before
  `notification_fanout` does), so as written the 1–5 % provider failures alone would keep the
  `drop_rate` SLO (0.001) from passing. The napkin figure for retries is rate × f⁴ dead letters a
  second (iOS 1140 × 0.02⁴ = 1.82e-4/s), but that is not what the model gives with this file's
  numbers: APNs keeps its type's default quota of 1k/s, which 1140/s overflows, and a call the
  quota sheds is a failure to the pool like a 5xx, so the pool retries it and its retries load the
  quota further. A probe with the iOS channel alone (1140/s into `ios_q`, 50 workers at 20 ms,
  `retries: 3`, `apns` failing 2 % at the default capacity) settles with `ios_workers` saturated
  (rho 1: the retries fill its 2.5k/s), APNs answering at most 980/s (its quota less 2 %), and
  about **154 dead letters a second**, a `dead_letter_rate` of 0.135, while the drop rate reads 0.
  Sizing the providers' quotas is Task 16's, when the scenario runs. The other three channels
  already fit under their 1k/s quotas and give the napkin figures (Android 570 × 0.02⁴ =
  9.1e-5/s, SMS 95 × 0.05⁴ = 5.94e-4/s, e-mail 570 × 0.01⁴ = 5.7e-6/s; each provider is sent
  1 + f + f² + f³ times its messages), and so would iOS (1.82e-4/s) with a quota above
  1140 × 1.02 = 1163/s.
- Dedup (2%) and the fan-out ratio: one notification becomes 1.25 channel messages on average
  (the `factor`s 0.6 + 0.3 + 0.05 + 0.3).
- The `user_cache` hit ratio (devices and preferences), computed with the Che approximation
  from its 5M keys and the notified users' distribution (Zipf 0.8 over a key space of 10M
  user ids, sized after the book's 10M mobile pushes a day: some users get far more
  notifications than others). This is computed today:
  `hit_ratio=0.814 (che: zipf 0.8 over 10M keys from services)`.

## What it abstracts away
- Exponential backoff and jitter (a pool's retries each wait the same `retry_after`), and the
  dead-letter queue itself: the model counts the dead letters, nobody reads or replays them.
- Template rendering, user opt-outs, delivery receipts.

## Things to try
With `run`, once phase 3 runs the fan-out. Today `explain` shows only the `load not propagated`
mark on these nodes, whatever their numbers.
- Cut `sms_workers.workers` to 5: watch `sms_q` lag blow past the SLO after the 09:00 blast.
  (Today: `sms_workers  peak offered 0/s vs 100/s total capacity  (load not propagated: ...)`.)
- Set `apns.failure_rate` to 0.3 (an outage): with retries enabled in phase 3 the queue backs up.
