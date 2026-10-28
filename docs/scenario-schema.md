# Scenario schema v1 (draft, phase 0)

A scenario is one YAML document. Unknown keys are errors. Units: durations `ns us ms s m h`,
rates `N/s` with optional `k M G` (and byte rates `B KB MB GB` + `/s`), sizes `B KB MB GB`,
clock times `HH:MM` (for `at:`/`peak_at:`).

## Top level

| key | type | required | meaning |
|---|---|---|---|
| `version` | int | yes | always `1` |
| `name` | string | yes | shown in reports |
| `seed` | int | no | default 1 |
| `duration` | duration | yes | simulated span |
| `step` | duration | no | default `100ms` |
| `warmup` | duration | no | discarded from reports, default 0 |
| `datacenters` | map | no | see below; default: one DC `main` |
| `nodes` | map id → node | yes | |
| `links` | list | yes | |
| `slo` | list | no | |
| `faults` | list | no | |

## `datacenters`

Map of DC name → `{}` (reserved for future keys) plus an optional `latency` list of
`{between: [a, b], rtt: duration}` overriding the `cross_dc_rtt` default (150 ms).
Intra-DC links default to `intra_dc_rtt` (0.5 ms).

## Node (common keys)

| key | type | meaning |
|---|---|---|
| `type` | enum | one of the catalogue below |
| `dc` | string | datacenter, default `main` |
| `replicas` | int | N identical copies behind an equal split (default 1) |
| `capacity` | rate | service rate per replica; default per type from the back-of-the-envelope table |
| `latency` | duration | service time per op; default per type |
| `ops` | map op → list of step | what the node does with each incoming op (`service`, `client` use it) |
| `then` | list of step | steps after processing (used by `worker_pool`, `notification_fanout`, `frontier`, `cache`, `blob_store`); on a `cache`, steps with `on: miss` receive the flow the cache did not serve, which may re-enter an upstream node (a feedback edge) |
| `advanced` | map | `queue_limit` (int), `concurrency` (int), `yellow` (0–1), `red` (0–1), `service_variability` (SCV), `drop_policy` |

A **step** is `{to: <node id>, op: <op name>, on: always|miss|failure, mode: sequential|parallel, factor: <number>}`.
`factor` multiplies the flow (fan-out: one post → 100 feed writes). `on: miss` applies to the
flow a cache/CDN did not serve. `mode: parallel` steps add the max of their latencies, not the sum.

## Node types

| type | own keys |
|---|---|
| `client` | `rate` (rate curve), `ops` (map op → share, must sum to 1), `keys` `{zipf: s, space: N}` or `{uniform: N}`, `sizes` (map op → size), `timeout`, `retries` |
| `cdn` | `ttl`, `hit_ratio` (`fixed: h` or `model`), `origin` (node id) |
| `load_balancer` | `policy` `round_robin|least_connections|weighted|consistent_hash`, `vnodes` (int, consistent_hash) |
| `rate_limiter` | `algorithm` `token_bucket|leaky_bucket|fixed_window|sliding_log|sliding_counter`, `limit` (rate), `burst` (int), `backend` (node id of a cache holding counters) |
| `service` | — (uses `ops`) |
| `cache` | `eviction` `lru|lfu|fifo|ttl`, `strategy` `cache_aside|read_through|write_through|write_around|write_back`, `keys` (int capacity), `hit_ratio` (`fixed: h` or `che`), `backend` (node id for read/write-through), `ttl`, `then` |
| `sql_db` | `replication_lag`, `shards` (int), `pool` (int connections) |
| `kv_cluster` | `nodes` (int), `vnodes`, `replication` N, `write_quorum` W, `read_quorum` R, `mode` `ap|cp` |
| `message_queue` | `partitions`, `retention` |
| `worker_pool` | `consumes` (queue node id), `workers`, `cost` (duration per job), `then` |
| `blob_store` | `capacity` in bytes/s (e.g. `500MB/s`), `then` |
| `search_index` | `build_interval` |
| `ws_gateway` | `max_connections`, `heartbeat` (duration), `fanout` (avg recipients per message) |
| `service_discovery` | `failover` (duration) |
| `id_generator` | `algorithm` `snowflake|ticket_server|uuid` |
| `external_sink` | `failure_rate` (0–1), `politeness` (rate per host) |
| `notification_fanout` | `channels` (map name → node id), `dedup` (0–1) |
| `frontier` | `politeness` (rate per host), `hosts` (int), `then` |

## Rate curve (`client.rate`)

```yaml
rate:
  base: 10k/s
  wave:   {shape: sine|square|diurnal, period: 24h, amplitude: 0.6, peak_at: 20:00}
  ramp:   {from: 0, to: 10k/s, over: 5m}
  spikes: [{at: 12:00, for: 10m, factor: 3}]
  points: [[0s, 1k/s], [1h, 5k/s]]          # replaces base
  noise:  {sigma: 0.15, correlation: 5m}     # phase 2
```
Terms multiply.

## Links

`{from, to, dir: in|out|inout (default out), ops: [..] (default all), latency, bandwidth, weight}`.
Direction is from the writer's point of view: `out` means `from` sends to `to`; `inout` means
replies flow back along the same link (the normal request/response case).

## SLO

`{metric: p50_latency|p95_latency|p99_latency|drop_rate|utilization|queue_lag|staleness, node, op, max}`.

## Faults

`{at: clock-or-duration, for: duration, kind: node_down|dc_down|partition|latency_spike|traffic_spike|cache_flush, node|dc|between, factor}`.
