# Notification system (ch. 10)

## Book design
- Services call a notification service, which looks up user devices/preferences, applies
  per-user rate limits, and publishes one message per channel to a queue.
- One worker pool per channel pulls from its queue and calls the third party
  (APNs, FCM, SMS vendor, email vendor); failures are retried, logs and analytics are recorded.
- Decoupling via queues absorbs bursts; workers are scaled per channel.

## What the model captures
- A marketing blast (20x for 15 minutes) absorbed by the queues: lag grows, then drains at
  the workers' rate. The per-channel worker counts decide how long the drain takes.
- Third-party latency and failure rates as the real bottleneck (20 ms worker cost vs 150–500 ms
  round trips).
- Dedup (2%) and the fan-out ratio: one notification → 1.25 channel messages on average.
- The `user_cache` hit ratio (devices and preferences), computed with the Che approximation
  from its 5M keys and the notified users' distribution (Zipf 0.8 over the ~10M users the
  book's daily push volume reaches: some users get far more notifications than others).

## What it abstracts away
- Retry policy and dead-letter queues (phase 3 adds retry feedback).
- Template rendering, user opt-outs, delivery receipts.

## Things to try
- Cut `sms_workers.workers` to 5: watch `sms_q` lag blow past the SLO after the 09:00 blast.
- Set `apns.failure_rate` to 0.3 (an outage): with retries enabled in phase 3 the queue backs up.
