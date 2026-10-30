# Chat system (ch. 12)

## Status: `explain` only until phase 3

This scenario validates (`OK: chat-system (13 nodes, 14 links)`) and `simsys explain` prints its
whole resolved graph, but `run` and `estimate` refuse it with exit code 1:

```
scenarios/book/chat-system/scenario.yaml: error: node 'discovery' has type 'service_discovery', which run/estimate do not support yet (phase 3)
```

Two of its node types have no model yet: `discovery` (`service_discovery`) and `chat_servers`
(`ws_gateway`). The type check names the first one in file order (see
[What runs in phase 2c](../../../docs/scenario-schema.md#what-runs-in-phase-2c)).

What `explain` shows today. Its checks compare each node's peak offered load with its capacity,
without queueing, and they follow the load through both phase 3 types as if each were a plain
service. So no line carries a `load not propagated` mark. Two lines are red:

```
lb             peak offered 90k/s vs 100k/s total capacity  (90%, red)
push           peak offered 9.45k/s vs 1k/s total capacity  (945%, red)
message_store  peak offered 94.5k/s (31.5k/s × N 3) vs 120k/s (12 nodes)  (hottest 83.4%, yellow)
presence       peak offered 162k/s (81k/s × N 2) vs 200k/s (4 nodes)  (hottest 85.9%, yellow)
```

- `lb` states no `capacity`, so it gets the type's default of 100k/s, and the 21:00 peak of
  90k/s requests fills 90 % of it.
- `push`, the offline-push provider, also states no `capacity`. It gets an external sink's
  default of 1k/s, while 30 % of the 31.5k/s messages the sync workers deliver at the peak are
  sent to it. When phase 3 runs this file, `push` must be given a capacity, or the run will
  drop most of the pushes.
- `chat_servers` reads `125.1k/s vs 200k/s (62.5%, green)`. That load is the clients' `send`
  and `heartbeat` plus the sync workers' `deliver` calls. The deliveries come back to the
  gateways around a loop, and explain cuts that loop at `sync_workers -> chat_servers` (marked
  `[feedback]`).

What is accepted today but does nothing:

- the `faults` entry (`node_down` of `chat_servers` at 21:30 for 1 minute). The list is typed and
  validated: every key is checked (`at` inside the run, a declared node of a type the kind can act
  on, `for` positive, ...) and a wrong one is refused with a message naming the entry
  (`faults[0]: ...`), and `explain` lists it. The engine applies faults from Task 6 of phase 3 on
  (see [Faults](../../../docs/scenario-schema.md#faults));
- the gateway's `max_connections`, `heartbeat` and `fanout`. They are stored but not modelled:
  `fanout: 20` or `heartbeat: 1s` leave every `explain` check unchanged.

## Book design
- Stateless API servers (login, profile, friends) behind a load balancer; stateful chat
  servers holding WebSocket connections, chosen via service discovery (ZooKeeper).
- Message flow: sender's chat server → ID generator → message sync queue → recipient's chat
  server (online) or push notification (offline); messages persisted in a key-value store.
- Presence servers with heartbeats; group chats (≤100) fan out one queue entry per member.

## What phase 3 will model
Once `ws_gateway` and `service_discovery` run, the scenario is meant to show the following.
Where `explain` already shows a point today, the bullet says so.

- Heartbeat traffic dominating request counts (55 % of all ops) while messages dominate bytes.
  Today `presence` is offered 81k/s at the peak: the 49.5k/s heartbeats plus the sync workers'
  31.5k/s presence reads.
- Group fan-out (1.4 recipients on average) and the 30 % offline push fraction (both are `then`
  factors of `sync_workers`, which `explain` already follows).
- A write-heavy key-value store sized by per-node capacity, with replication 3 and quorum 2.
  `explain` puts its hottest machine at 83.4 %.
- The API servers' `kv_cache` hit ratio, computed with the Che approximation from its 20M keys
  and the users' key distribution (Zipf 0.8 over the 50M daily users: a mild skew, because
  activity spreads over many users rather than a few hot keys). This is computed today:
  `hit_ratio=0.763 (che: zipf 0.8 over 50M keys from users)`.
- Losing the whole gateway tier (`chat_servers`) for 1 minute at the 21:30 peak, after which a
  reconnect storm follows and the queue backs up. This needs phase 3's faults as well as its
  gateway.

## What it abstracts away
- Connection state and reconnection storms. The file scripts the trigger as a `node_down`
  fault, and the storm itself is left to phase 3's gateway model.
- Message ordering, end-to-end encryption, media attachments.

## Things to try
With `explain`, today (edit a copy of the file):
- `message_store.nodes: 6`: the hottest machine reads
  `94.5k/s (31.5k/s × N 3) vs 60k/s (6 nodes)  (hottest 165%, red)`.

With `run`, once phase 3 runs the gateway:
- `fanout: 20` (big groups): the sync workers and `message_store` are expected to go red.
- `heartbeat: 1s`: presence should become the bottleneck without a single message changing.
- `message_store.nodes: 6`: see the quorum writes saturate.
