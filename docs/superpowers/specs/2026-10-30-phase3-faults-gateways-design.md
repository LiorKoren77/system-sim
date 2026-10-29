# Phase 3: faults, retries, replications and the last four node types

Design for phase 3 of system-sim, agreed on 2026-10-30 after phase 2 closed at `c753411`
(1163 tests, schema v1.4, spec §19). It follows the main design spec
(`2026-10-27-system-sim-design.md`, §13's phase 3 row and the 2c plan's "Phase 3 backlog") and
decides what phase 3 builds, in what order, and the rules a scenario author will meet. Where it
conflicts with an earlier section of the main spec, this document wins; the implementation plan
(`docs/superpowers/plans/2026-10-30-phase3-*.md`) turns it into tasks, and the schema (v1.5)
and spec §20 record what was finally built.

## 1. Goal

After phase 3 every one of the twelve shipped scenarios runs end to end: the four book systems
that phase 2 refused (chat-system, google-drive, notification-system, web-crawler) run with their
SLOs judged, a scripted fault (a tier down for a minute, a latency spike, a cache flush) acts on
the running model and the report says what it cost, a client's retries and a pool's retries to a
third party are modelled as the feedback they are, `on: failure` steps fire, and
`simsys run --replications N` turns the rate noise into bands. The code is split along the lines
the phase 2 review recommended before any of that is built.

Not in phase 3 (stays in the backlog): kv_cluster partitions (`mode: ap|cp`, sloppy quorum,
hinted handoff) and the `partition` fault kind, random faults (`mtbf`/`mttr`, chaos mode), queue
partitions capping a consumer group's parallelism, request/reply over a queue, a large read's
last byte, sweep SLO columns, the discrete-event engine.

## 2. Decisions taken (2026-10-30)

| Question | Decision |
|---|---|
| Scope | The spec's phase 3 (four types, faults, retries, replications) plus the four file splits as a first sub-phase. |
| Randomness | Scripted faults run deterministically; `--replications N` repeats a run over seeds of the existing rate noise. Random faults wait. |
| How a fault acts | Time-varying node parameters: a schedule hands every node per-step factors (capacity, extra latency, rate, hit ratio). No graph edits, no second engine. |
| Visibility | Every fault gets its own summary block in the report; retries are a column (`attempts/s`) next to `offered/s`. |
| Invented numbers | `reconnect_within` and `retry_after` are marked "chosen, not from the book" in the schema and swept in the READMEs that use them. |
| Convergence | `estimate` reuses the damped settle of phase 2c for retry feedback; a theory test per fault kind pins that `run` returns to its pre-fault numbers within a stated time after the fault clears. |

## 3. Sub-phases and order

- **3a Layout.** No behaviour change. `explain` moves out of `report.cpp` into an
  implementation unit of `simsys.report`; the loader's readers split per concern (header and
  datacenters, node bodies and advanced keys, steps, links, SLOs, overrides) as implementation
  units of `simsys.config`; `graph.cppm`'s definitions move to `graph.cpp`; `test_theory.cpp`
  splits along its `// ----` sections into nine files with the shared builders in
  `tests/support/fixtures.cppm`. Each move is one commit; every scenario's `run`, `estimate`
  and `explain` output stays byte-identical; the fourth and fifth GCC 14.2 defects are re-checked
  after each move (CLAUDE.md §3), and a new `emit_special_members` line or `detail::` function is
  added where one appears.
- **3b Faults, retries, replications** (§4–§7).
- **3c The four types** (§8), each landing with its scenario running end to end (§9).
- **3d Docs and the phase review**: schema v1.5, spec §20, study notes for new constructs, the
  comprehensive phase 3 review, fix round, push.

Every task runs the phase 2 loop: a test-writer's red commit, an implementer's green commit, an
adversarial review recorded in the plan, a fix round.

## 4. Faults

### 4.1 Schema

```yaml
faults:
  - {at: 21:30, kind: node_down, node: chat_servers, for: 1m}
  - {at: 15:00, kind: node_down, node: post_db, replicas: 1, for: 30s}
  - {at: 12:00, kind: latency_spike, node: user_db, extra: 50ms, for: 5m}
  - {at: 03:00, kind: capacity, node: cdn_us, factor: 0.5, for: 20m}
  - {at: 09:00, kind: cache_flush, node: feed_cache}
  - {at: 18:00, kind: traffic_spike, node: users, factor: 3, for: 10m}
  - {at: 02:00, kind: dc_down, datacenter: eu, for: 2m}
```

| Key | Required | Meaning |
|---|---|---|
| `at` | yes | a clock time or duration from the run's start (the rate curve's syntax); must fall inside the run |
| `for` | all kinds but `cache_flush` | a positive duration; the fault ends at `at + for` |
| `kind` | yes | one of `node_down`, `capacity`, `latency_spike`, `cache_flush`, `traffic_spike`, `dc_down` |
| `node` | all kinds but `dc_down` | a declared node; `traffic_spike` needs a client, `cache_flush` a cache or cdn, the others any node that runs |
| `replicas` | `node_down` only, optional | how many of the node's replicas go down (default: all); for a kv_cluster, how many of its machines; for a sql_db, `primary` or a count of replicas |
| `factor` | `capacity`, `traffic_spike` | a positive number; `capacity` needs it below 1 (`factor` above 1 is a capacity gain, refused: use the scenario's `capacity`) |
| `extra` | `latency_spike` | a positive duration added to the node's service time |
| `datacenter` | `dc_down` | a declared datacenter |

Two faults on the same node may overlap; their factors multiply and their extra latencies add.
A `node_down` on a client is refused (clients emit, they do not serve). `partition` is refused
with "phase 3 does not model partitions; see the backlog".

### 4.2 Model

`engine::FaultSchedule` is built at `Simulation::create` from the typed list. Each step it
gives every node a `Condition`:

```cpp
struct Condition {
  double capacity_factor = 1;   // node_down: 0 (or (replicas − down) / replicas); capacity: factor
  SimTime extra_latency{};      // latency_spike: extra
  double rate_factor = 1;       // traffic_spike on a client
  double hit_ratio_factor = 1;  // cache_flush: 0, then warming back (4.3)
};
```

A node with no active fault gets the identity, so `run` without faults is bit-identical to phase
2. The engine applies the condition where the node's own numbers enter the queueing model:
`capacity × capacity_factor` (a `node_down` of every replica is capacity 0: the room fills at the
node's `queue_limit`, the rest is dropped, the backlog drains when the fault ends), service time
plus `extra_latency` (which lowers the capacity of an M/M/c node accordingly), the client's rate
times `rate_factor` (the same code path as a scripted `spikes` entry), the cache's hit ratio
times `hit_ratio_factor`. `dc_down` is `node_down` on every node of the datacenter and a
`capacity` factor of 0 on every link that leaves it.

A down kv_cluster machine lowers the cluster's capacity by its share and, with the quorum
arithmetic of §19 entry 9, raises the loss of the requests whose replica set held it; this is as
far as phase 3 takes kv_cluster faults (no partitions).

`estimate` ignores faults and prints `faults: 3 scripted (estimate shows the steady state without
them; run applies them)`.

### 4.3 Cache flush

A flushed cache's hit ratio is 0 at `at` and warms back along the Che curve: at time `t` after
the flush the cache holds the keys of `rate × t` requests, so its hit ratio is the Che hit ratio
of a cache of `min(keys, distinct keys seen in t)` entries (§18 entry 1's model with the
smaller size). The warm-up is complete once the cache is full again; the report's fault block
says how long that took.

### 4.4 Report

For every fault the run prints one block after the SLO table, and the JSON a `faults` array:

```
fault 21:30 node_down chat_servers for 1m
  during:   offered 60.3k/s, served 0/s, lost 3.6M requests, backlog 0 (room 10k dropped the rest)
  after:    reconnect storm 2.0M connections over 30s, chat_servers peak 148% at 21:31:12
  recovery: send p99 back under 300ms at 21:33:40 (2m40s after the fault cleared)
```

The three lines are the same for every kind: what was lost or delayed during the fault (requests
lost, backlog built, SLO verdicts that flipped), what followed (for a gateway the reconnect storm,
for a cache the warm-up, for a client's retries the extra attempts), and when every op's p99 and
every node's utilisation were back within 1 % of the step before the fault (or "not recovered by
the end of the run"). The recovery time is the number a README quotes.

## 5. Failure steps

`on: failure` steps fire. A step list `[{to: db, op: write}, {to: log, op: write, on: failure}]`
sends `log` the share of requests whose `db` write failed last step (lost at `db`, or at a hop
inside its own steps), the way `on: miss` carries a cache's miss share. The failure share is
`1 − d` of the step it follows (`d` the delivered fraction of §19 entry 11). Rules: an `on:
failure` step belongs to the nearest unconditional step before it; it is a sequential hop; its
flow is not thinned by the step it reports on (a failed request is exactly the one that reaches
it), but is thinned by earlier steps; the size walk prices it like any step (the 2c note on
unsized failure steps over a pipe is resolved by refusing an unsized `on: failure` step over a
bandwidth link, as for any step). A failure step after a step with no losses sends nothing.

## 6. Retries

### 6.1 Client retries

```yaml
users: {type: client, ..., timeout: 2s, retries: 2, retry_after: 100ms}
```

A client with `retries: n` (default 0) sends every request it lost or timed out again after
`retry_after` (default 100 ms; **chosen, not from the book**), up to `n` extra attempts per
request. Modelled as feedback demand: the attempts of step `t` are
`offered_t = curve_t + Σ_{k=1..n} failed_{t − k·b}` where `b` is the backoff in steps and
`failed_{t−k·b}` the share of attempt `k − 1` that was lost or timed out (each attempt fails at
the same rate as the current step's path, so an outage produces a geometric series that the
retries cap). The servers do every attempt's work; the client's answered rate counts a request
once, when any attempt is answered; a request whose last attempt failed is lost (or timed out)
once. The table gains `attempts/s` beside `offered/s`, the JSON `attempts_per_s`; `offered/s`
stays the curve's rate, so the retry storm is visible as `attempts/s` rising above it. A client
without `timeout` and without losses on its path has `attempts/s` equal to `offered/s`.

`estimate` iterates the feedback to a fixed point with phase 2c's damped settle (the delivered
fractions' scheme), and warns when it does not settle. `run` shows the storm as it is; a theory
test pins that `run` with retries returns to its pre-fault numbers within `n × retry_after` plus
the backlog's drain time after the fault clears.

### 6.2 Pool retries and the dead-letter count

```yaml
ios_workers: {type: worker_pool, consumes: ios_q, workers: 50, cost: 20ms, retries: 3,
              retry_after: 1s, then: [{to: apns, op: send}]}
```

A worker pool with `retries: n` re-publishes to its own queue the jobs whose `then` step to an
external sink failed (the sink's `failure_rate` share), after `retry_after` (default 1 s,
chosen), up to `n` times; a job that fails its last attempt is **dead-lettered**: counted in the
pool's `dead_lettered` total and `dead_lettered_per_s`, printed in the pool's row and the JSON,
never as a drop. notification-system's `drop_rate` SLO is therefore judged on real losses, and
its README quotes the dead-letter rate as the number the on-call team would watch. A pool
without `retries` behaves as in phase 2 (the sink's failures are the sink's drops).

## 7. Replications

`simsys run scenario.yaml --replications N [--jobs J]` runs the scenario N times with seeds
`seed, seed + 1, …, seed + N − 1` over the rate noise (§17's OU multiplier), on the thread pool
of `simsys.session` (one `Simulation` per seed, as `sweep` does per point; output independent of
`--jobs`). The table prints, per op and node metric, the median and the 10–90 % band across the
runs (`p99 212ms [198–231]`); each SLO prints `PASS in 9 of 10` with the median run's observed
value; the exit code is the median run's. `--replications 1` is the plain run. A scenario whose
clients have no `noise` runs once and prints `replications: the scenario has no noise, so every
seed gives the same run`. Scripted faults are the same in every replication. JSON gains a
`replications` object with the per-seed values. `--report csv --series` is refused with
`--replications` (one series file per seed is phase 4 material).

## 8. The four node types

### 8.1 service_discovery

```yaml
discovery: {type: service_discovery, failover: 10s, capacity: 50k/s, latency: 0.5ms}
```

A service (M/M/c with the type's defaults 50k/s, 0.5 ms, c = implied) that answers `lookup`
ops. Its one extra rule concerns faults: when a node that some op reaches **through a lookup**
(an op whose step list has a `discovery.lookup` step before the step to that node) suffers a
`node_down` of `r` of its `R` replicas, then for `failover` seconds from the fault's start the
share `r / R` of that op's requests keep going to the dead replicas and are lost at the dead
node, after which they route to the survivors (the node's capacity is already down by `r / R`,
so the survivors queue). Without a lookup step before it, a `node_down` just removes capacity
and the balancer in front spreads the load, as in §4.2. `failover` default 10 s (**chosen**;
the book gives none). `explain` prints `failover=10s`.

### 8.2 ws_gateway

```yaml
chat_servers:
  type: ws_gateway
  replicas: 20
  max_connections: 100k        # per replica
  heartbeat: 5s
  fanout: 1.4
  reconnect_within: 30s        # chosen, not from the book
  capacity: 10k/s
  latency: 0.5ms
  ops:
    send: [...]
    heartbeat: [{to: presence, op: beat}]
    deliver: []
```

Connections are a level `C` (not a backlog): each step `C += (connects − disconnects) × dt`.
Connects come from a client op named `connect` whose step reaches the gateway (its share of the
client's rate); a gateway no client connects to must state `connections: 2M`, a fixed level (the
google-drive case). Disconnects are `C / session`, with `session` the mean session length
(default 30 min, **chosen, not from the book**). The cap is `max_connections × replicas`; connects above it are refused and
counted as rejections. Load from heartbeats is `C / heartbeat` requests per second on the
gateway's `heartbeat` op (added to whatever the client sends under that name). A message op's
`then` steps fan out by `fanout` (the average recipients per message) as a `factor`. `deliver`
is terminal (pushed over the open connection, no hops).

A `node_down` of the gateway's replicas drops `C × r / R` connections at once; they reconnect
spread evenly over `reconnect_within` from the moment the fault clears (the storm: `connects +=
dropped / reconnect_within` per second for that long), which is chat-system's scripted fault.
During the fault the dropped connections' messages are lost at the gateway (capacity down by
`r / R`), and `deliver` to them is lost too. The report's fault block names the storm's size and
the gateway's peak utilisation.

### 8.3 notification_fanout

```yaml
fanout:
  type: notification_fanout
  dedup: 0.02
  channels: {ios: ios_q, android: android_q, sms: sms_q, email: email_q}
  then: [...]
```

A service (type defaults 20k/s, 1 ms) whose `then` steps fire per routed notification times
`1 − dedup` (the share the book's dedup step drops as duplicates, counted as neither drop nor
rejection but as `deduplicated_per_s`). `channels` maps channel names to message queues; every
`then` target that is a queue must appear in `channels` and vice versa (`node 'fanout': channel
'sms' names sms_q, which no then step publishes to`), so the map is the student-facing list of
channels and the steps carry the shares. Retries and the dead-letter count live on the channel
pools (§6.2), where the book puts them.

### 8.4 frontier

```yaml
frontier:
  type: frontier
  politeness: 1/s     # per host
  hosts: 10M
  capacity: 5k/s
  then: [{to: downloader, op: fetch}]
```

A message queue without retention (nothing is deleted unconsumed) fed by the seeds and by
`url_seen`'s miss step (the feedback edge), whose consumer pool drains it at
`min(pool capacity, hosts × politeness)`: per-host politeness as a rate cap over the hosts the
crawl touches (`hosts` is the number of distinct hosts, so the cap is `hosts × politeness`;
with 10M hosts at 1/s it is not binding, and the README's exercise is to lower `hosts` until it
is). `capacity` is the prioritiser's own rate (a service in front of the queue). `queue_lag` on
a frontier is evaluated (the consumer group is connected, closing the 2c gap). An external
sink's `politeness` applies the same cap to the sink's drain, so `internet` with `politeness:
1/s` over `hosts` taken from the frontier it serves (the one frontier in the scenario, else
refused) caps fetches the same way.

## 9. Scenarios

| Scenario | What makes it run | README's lesson |
|---|---|---|
| chat-system | ws_gateway, service_discovery, the 21:30 `node_down` | the reconnect storm: connections, the storm's size vs `reconnect_within`, the gateway's peak, `send` p99 recovery time; a sweep over `reconnect_within` |
| google-drive | ws_gateway as `long_poll` (a `connections: 2M` level, `heartbeat: 60s`) | the notify fan-out ×2 devices; `backup_workers` sized (its backlog grows all evening today: either size it or keep it as the lesson, the README says which); download p99 vs the 2 s SLO |
| notification-system | notification_fanout, pool retries, dead-letter | the 09:00 marketing blast ×20 through the channel queues; retries to the providers; dead-lettered vs lost; `user_db` and `notification_svc` red checks resolved (sized) or explained |
| web-crawler | frontier, `on: always`, the url_seen feedback | the frontier's lag under the 50/s seeds and the ×50 link fan-out at 10 % new URLs; politeness as a cap (lower `hosts`); `internet` failures (5 %) with `retries: 2` on the downloader |

Each scenario's `explain` red checks are either sized away with the reason in the YAML or kept
as the lesson, and the README quotes the binary.

## 10. Report and schema additions

- Table: `attempts/s` (clients), `dead_lettered/s` (pools), `deduplicated/s` (fanout),
  `connections` (gateways: the level's peak and end value), the fault blocks (§4.4), the
  replication bands (§7).
- JSON: `attempts_per_s`, `dead_lettered`, `dead_lettered_per_s`, `deduplicated_per_s`,
  `connections` (peak, end), `faults` (one object per fault with the three lines' numbers),
  `replications`.
- CSV series: `attempts_per_s` per client op, `connections` per gateway, `dead_lettered_per_s`
  per pool.
- Schema v1.5: the `faults` table, `retries`/`retry_after` on clients and pools, `on: failure`,
  the four types' keys, `connect` ops and `connections`, `--replications`, every "chosen, not
  from the book" default listed in one place.
- Explain: `faults: 1 scripted (node_down chat_servers 21:30 for 1m)`, each type's new keys,
  the paper load following a fanout's and a frontier's `then` (the last two "load not
  propagated" marks go).

## 11. Tests to write first

- Faults: each kind on an M/M/1 probe pins its effect (capacity 0 drops `offered − 0` beyond the
  room; `extra: 50ms` raises the p99 by the closed form; `factor: 0.5` doubles rho; a flush's
  hit ratio 0 then the Che warm-up curve; a traffic spike equals the same `spikes` entry), and
  the recovery time after the fault clears; two overlapping faults multiply; `estimate` ignores
  them with the note; `run` twice byte-identical.
- Failure steps: the failure share equals `1 − d`; nothing fires after a lossless step; an
  unsized failure step over a pipe is refused.
- Retries: the geometric series of attempts under a 50 % loss with `retries: 2` (1 + 0.5 +
  0.25 = 1.75 attempts per request, answered 0.875); attempts return to the curve after the
  fault; `estimate` settles; pool retries re-publish the sink's failures and dead-letter after the
  last attempt with the binomial numbers; the drop-rate SLO ignores dead letters.
- Replications: N seeds give N different runs, the median and band are what a hand computation
  gives, `--jobs` does not change the bytes, a noiseless scenario runs once with the note.
- Types: service_discovery's lost share for `failover` seconds then the survivors; a gateway's
  level under connects/disconnects, the cap's rejections, heartbeat load `C / heartbeat`, the
  fan-out factor, the storm after a `node_down`; fanout's dedup share and the channels map
  checks; the frontier's cap `hosts × politeness`, its lag, the url_seen feedback at a fixed
  point.
- Scenarios: the four run with exit 0 (or the exit the README states on purpose); every README
  number pinned the way phase 2's day tests do, at a coarser step where the closed forms allow.

## 12. Study notes

New constructs are documented on first use as always; expected additions: `std::ranges::fold_left`
over the fault schedule, `std::chrono` arithmetic for `at`/`for`, and whatever the splits of 3a
need (module partitions, if used, get `docs/study/modules.md` §"partitions"). `numerics.md`
gains the retry series and the cache warm-up curve.
