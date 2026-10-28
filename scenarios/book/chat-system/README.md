# Chat system (ch. 12)

## Book design
- Stateless API servers (login, profile, friends) behind a load balancer; stateful chat
  servers holding WebSocket connections, chosen via service discovery (ZooKeeper).
- Message flow: sender's chat server → ID generator → message sync queue → recipient's chat
  server (online) or push notification (offline); messages persisted in a key-value store.
- Presence servers with heartbeats; group chats (≤100) fan out one queue entry per member.

## What the model captures
- Heartbeat traffic dominating request counts (55% of all ops) while messages dominate bytes.
- Group fan-out (1.4 avg recipients) and the 30% offline push fraction.
- Write-heavy key-value store sized by per-node capacity, replication 3, quorum 2.
- The API servers' `kv_cache` hit ratio, computed with the Che approximation from its 20M
  keys and the users' key distribution (Zipf 0.8 over the 50M daily users: a mild skew, as
  activity spreads over many users rather than a few hot keys).
- Losing the whole gateway tier (`chat_servers`) for 1 minute at the 21:30 peak: a reconnect storm follows and the queue backs up.

## What it abstracts away
- Connection state and reconnection storms (a scripted `traffic_spike` approximates one).
- Message ordering, end-to-end encryption, media attachments.

## Things to try
- `fanout: 20` (big groups) — the sync workers and `message_store` go red.
- `heartbeat: 1s` — presence becomes the bottleneck without a single message changing.
- `message_store.nodes: 6` — see quorum writes saturate.
