# Lab: unique ID generator (ch. 7)

## Book design
- Options: multi-master replication (auto-increment by k), UUID, ticket server (one
  auto-increment DB — single point of failure), Twitter snowflake (timestamp | datacenter |
  machine | sequence; 4,096 IDs per ms per machine, sortable by time).
- Snowflake chosen: no coordination, time-sortable, scales with machines; needs clock sync (NTP).
- Requirement: 10,000 IDs per second.

## The scenario
10k/s of `next_id` from the services, with a 3x spike (30k/s) for one minute at 5 m, sent to
one of two generators by the links' `weight`:

- `ticket_server`: `algorithm: ticket_server`, one replica, `capacity: 8k/s`, `latency: 1ms`,
  a waiting room of 1000. Shipped with weight 1: it carries all the load.
- `snowflake`: `algorithm: snowflake`, four replicas, with no `capacity` or `latency` of its own:
  the algorithm supplies 4.096M/s and 10 us per replica. Shipped with weight 0: idle.

SLOs: p99 of `next_id` at most 10 ms, and no drops anywhere.

The shipped file **fails on purpose**: the ticket server is the design ch. 7 rejects. The
book's objection is its single point of failure (one database: when it is down, nobody gets an
id), which the model cannot show before failures arrive in phase 3; what the lab shows is the
same design's other limit, one database's throughput below the 10k/s requirement.
`tests/scenario/test_scenarios.cpp` pins both halves: the shipped run fails
`drop_rate` at 0.339, and the switched run (below) passes every SLO.

## What the model captures
- An `id_generator` is a service whose capacity and service time come from its `algorithm`
  (plan 2b refinement 6, `defaults::for_id_algorithm`), unless the scenario states its own
  `capacity` or `latency`, as this lab does for the ticket server and not for the snowflake:
  - `snowflake`: 2^12 = 4096 ids per millisecond per machine (the 12 sequence bits), so
    4.096M/s per replica, 10 us each; replicas add capacity (no coordination);
  - `ticket_server`: one auto-increment database, where each id is a write: spec §6's 1k
    writes/s, 1 ms each. `replicas` must be 1 (the loader and `Graph::build` refuse more: a
    second copy would issue the same ids, and splitting the sequence is the multi-master design);
  - `uuid`: generated locally, unbounded in practice: 1G/s, 1 us.
  `explain` prints it, and tags the numbers the algorithm supplied `(algorithm)`:
  `capacity=4.096M/s (algorithm)  service_time=10us (algorithm) ... algorithm=snowflake
  (4.096M/s per replica by design)`. Beside a capacity the scenario states, the algorithm's
  figure is the default it overrides: `capacity=8k/s (scenario) ... algorithm=ticket_server
  (one database; default 1k/s, spec §6 writes)`.
- The ticket server as one capacity-limited queue facing more than it can serve: its waiting
  room fills, the wait grows to queue / rate (1000 / 8k/s = 125 ms), and the excess is dropped.
- Link `weight` as the switch between the two designs (`sweep` over it is planned for phase 2c).

## What it abstracts away
- Clock skew and NTP, the bit layout, datacenter and machine ids (the 5-bit machine field caps
  a datacenter at 32 snowflake machines; `replicas` is not checked against it), the sequence's
  reset each millisecond (a fluid rate cannot burst 4096 ids into the first microsecond of a
  millisecond).
- Ids being unique and sortable at all: the model counts how many are issued and how fast, not
  their values. 64 bits versus UUID's 128 (122 of them random) is not modelled.
- The ticket server's single point of failure: failures are phase 3 (`faults`). The chapter's
  remedy, several ticket servers kept in sync, is not a mode of the model.
- How a timed-out request ends: the services give up after their `timeout: 50ms` (plan 2c Task
  13: the share of each step's fitted response slower than that is timed out, not answered), but
  the generator still issues the id, and nobody retries.

## Results (`simsys run`, 10 min, after the 30 s warmup)

| setting | generator rho | dropped | drop rate | next_id mean / p99 (est.) | SLOs |
|---|---|---|---|---|---|
| shipped: ticket server at 8k/s | 1 (red for all 570 s) | 2.34M | 0.339 | all timed out (would be 127 ms) | both FAIL (exit 2) |
| ticket server at its default 1k/s (delete `capacity`) | 1 | 6.33M | 0.917 | all timed out (would be 1 s) | both FAIL |
| snowflake, 4 x 4.096M/s (weights switched) | 0.00074 | 0 | 0 | 510 us / 534 us | PASS (exit 0) |
| uuid (weights switched, `algorithm: uuid`) | ~0 | 0 | 0 | 501 us / 503 us | PASS |

- **Ticket server**: 10k/s against 8k/s, 30k/s in the spike. Demand over the observed 570 s is
  10k x 510 s + 30k x 60 s = 6.9M ids; it issues at most 8k x 570 s = 4.56M, so at least 2.34M
  are dropped, a drop rate of 2.34M / 6.9M = **0.339**. The queue is full all the time, so every
  id it issues waits about 125 ms (127 ms with the links and the service), far past the
  services' 50 ms timeout: `run --report json` gives `answered_per_s` 0 and `timed_out_per_s`
  8000 (every id issued, 8k/s, is one the caller gave up on), and the table prints `8k` under
  `timed out/s`, `-` for the mean and `>50ms` for every percentile. The p99 SLO observes 50 ms, the
  timeout: every delivered id timed out, more than the 1 % a p99 allows, so it fails whatever its
  `max` (`100% of delivered requests timed out (more than 1%)`; a `max: 1s` fails too, since the
  services never saw an id answered). It observed 158 ms before the timeout applied. Even without the spike it would drop 2k
  of every 10k. At the default 1k/s the same holds: 1k/s issued, all timed out (each would take
  about 1 s).
- **Snowflake**: 12.1k/s on average against 4 x 4.096M/s = 16.38M/s: each machine is 0.074 %
  busy (12.11k / 16.38M = 0.00074) and nobody waits. The response is the two link hops (250 us
  each way) plus the 10 us service time.

## Things to try
- Switch the design: set the `ticket_server` link's `weight` to 0 and the `snowflake` link's to
  1. Every SLO passes (the table's third row).
- Delete the ticket server's `capacity: 8k/s`: it falls back to its algorithm's default, one
  SQL machine's 1k writes/s, and drops 92 % of the ids. 8k/s is a generous single MySQL.
- Give the ticket server `replicas: 2`: `validate`, `explain` and `run` refuse it (`replicas
  must be 1`). The chapter does name several ticket servers as the remedy for the single point
  of failure, at the cost of keeping them in sync; the model has no such mode, and for more ids
  per second the chapter's answer is snowflake.
- State a capacity to override the algorithm's: `capacity: 4M/s` on the snowflake shows
  `capacity=4M/s (scenario)` beside `algorithm=snowflake (default 4.096M/s per replica by
  design)`. As shipped, `explain` shows `capacity=4.096M/s (algorithm)  service_time=10us
  (algorithm)`, the numbers the id layout sets by design, and `peak offered 0/s vs 16.38M/s
  total capacity` for the four machines.
- Try `algorithm: uuid` on the second generator: unbounded (1G/s per replica), the same pass;
  what it costs (128 bits, not sortable by time) is outside the model.
