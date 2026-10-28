# Lab: unique ID generator (ch. 7)

## Book design
- Options: multi-master replication (auto-increment by k), UUID, ticket server (one
  auto-increment DB — single point of failure), Twitter snowflake (timestamp | datacenter |
  machine | sequence; 4,096 IDs per ms per machine, sortable by time).
- Snowflake chosen: no coordination, time-sortable, scales with machines; needs clock sync (NTP).

## What the model captures
- The ticket server as one capacity-limited node (8k/s) facing 10k/s with a 3x spike:
  queue, latency, then drops.
- Snowflake as four independent generators whose capacity is effectively unlimited.
- Link `weight` as the switch between the two designs (phase 2 supports `sweep` over it).

## What it abstracts away
- Clock skew and NTP, ID layout, section length tuning.

## Things to try
- Swap the weights to route to `snowflake`: the SLO passes with p99 ≈ link latency.
