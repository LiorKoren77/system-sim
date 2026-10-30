# Chat system (ch. 12)

## Book design
- Stateless API servers (login, profile, friends) behind a load balancer; stateful chat
  servers holding WebSocket connections, chosen via service discovery (ZooKeeper).
- Message flow: sender's chat server → ID generator → message sync queue → recipient's chat
  server (online) or push notification (offline); messages persisted in a key-value store.
- Presence servers with heartbeats; group chats (≤100) fan out one queue entry per member.
- The book's numbers: 50M daily active users, 40 messages each a day (2G a day, ~23k/s on
  average), about 1M users connected at once (10 KB of memory a connection: 10 GB on one box),
  a heartbeat every 5 s.

The day: the users offer 30k requests/s on a daily sine of ±50 % that tops at 21:00 (45k/s), as
70 % `send`, 20 % `api` and 10 % `connect`. At 21:30, just past the peak, the whole gateway tier
(`chat_servers`, 20 servers) is down for a minute: the scripted fault, and the chapter's question
"what happens when a chat server goes offline?".

### How it is sized

Phase 3 Task 14 gave the gateway a level of open connections, and this file had no source for it
(`run` refused it: `node 'chat_servers': a ws_gateway needs a client op named 'connect' or a fixed
'connections'`). Task 15 gives it one and sizes the tiers the level loads. Every number below is
`simsys explain`'s or `simsys run`'s on this file.

- **Who connects, and for how long: a `connect` op, not a fixed level.** A fixed `connections`
  would hold the same number all day; a `connect` op lets the level follow the day's wave and,
  above all, lets the users come back after the fault (the storm needs connects to come back
  through). The book gives no session length, so one is chosen: **6 minutes**, a mobile chat
  session (open the app, read, answer, put the phone away), about five times a day. The connect
  rate is then `50M DAU × 5.2 sessions/day ÷ 86,400 s` = **3k connects/s** on average, 10 % of the
  30k/s, and by Little's law the open connections are `3k/s × 360 s` = **1.08M** on average: the
  book's 1M concurrent users. At the 21:00 peak the connects are 4.5k/s and `explain` reads
  `connections: C*=1.62M at the peak connect rate (connect 4.5k/s × session 6m), heartbeat
  324k/s, cap 2M`. (A 30-minute
  session at one session a day gives the same 1M with 600 connects/s; the shorter session makes
  the level follow the wave within minutes and the twin comparison after the fault quicker.)
- **The heartbeats are the gateway's, not the users'.** The file used to have the users send a
  `heartbeat` op (55 % of 60k/s). Since Task 14 the gateway's open connections ping by themselves,
  `C / 5 s`, so that op would count every ping twice: it is gone, and the users' 30k/s are what
  they send on purpose (messages 21k/s, the 35 % of 60k/s it was; the API's 6k/s, as before). The
  load balancer, which carried the pings, falls from 90 % to `lb  peak offered 45k/s vs 100k/s
  total capacity  (45%, green)`.
- **The gateway tier is sized for its pings.** The book's numbers imply it: 1M connections pinging
  every 5 s are **200k pings/s** on average and 324k/s at 21:00, before a single message. The 20
  servers of 10k/s this file had (200k/s) could not carry even the average pings. A WebSocket
  server answering a ping does little (a frame in, a frame out, a timestamp), so each server is
  given **25k frames/s**: `chat_servers  peak offered 405k/s vs 500k/s total capacity  (81%,
  yellow)`, of which 324k/s are pings, 31.5k/s sends, 4.5k/s connects and 44.1k/s deliveries. The
  20 % headroom is what the reconnect storm uses (below); `max_connections: 100k` a server (1 GB
  at 10 KB each) is a cap of 2M, above the 1.62M peak, so no connect is refused.
- **Presence is sized for the pings too.** Each ping is a write of the user's last-seen time to
  `presence` (N = 2 copies), and each message copy the sync workers deliver reads the recipient's
  status: `presence  peak offered 738k/s (369k/s × N 2) vs 1M/s (10 nodes)  (hottest 80.4%,
  yellow)`, 10 machines of 100k simple writes/s (a Redis-class store; it was 4 of 50k/s).
- **Fan-out: copies in the queue, not in the store.** `fanout: 1.4` (recipients per message)
  multiplies only the gateway's step to `message_q`: ch. 12 stores a message once, with one id,
  and puts one copy per recipient into the sync queues. So `id_gen` and `message_store` see 31.5k
  messages/s at the peak (`message_store  peak offered 94.5k/s (31.5k/s × N 3) vs 120k/s (12
  nodes)  (hottest 83.4%, yellow)`) and `message_q` 44.1k copies/s; the sync workers deliver each
  copy once (the `factor: 1.4` they had counted the fan-out a second time, 1.96 deliveries a
  message).
- **Push.** 30 % of the 44.1k copies/s go to offline users through `push` (APNs/FCM), which
  stated no capacity and got an external sink's default 1k/s (945 % when the plan was written,
  1323 % with the double fan-out). A push provider's throughput is not the bottleneck it was
  standing for, so it states **20k/s** (3000 requests in flight at 150 ms each): `push  peak
  offered 13.5k/s vs 20k/s total capacity  (67.5%, green)`. Its 2 % failures used to be drops,
  a 2 % drop rate against the 0.1 % SLO; the sync workers now `retries: 3` (ch. 10's notification
  workers put a failed push back on the queue): the failures are attempts, the retried ones land,
  and 0.0047 jobs a second are dead letters.

The `drop_rate` SLO keeps its 0.1 %. It was feared to have little room (the minute's ~5M lost
requests against the 5.2G the users send in a day: 0.1 %), but a drop rate is per node, its drops
over its own demand: the worst node is `chat_servers`, which loses 2.16M requests in the fault
against about 23G it is sent in the day (pings mostly), 0.0093 %.

## What the model captures
All numbers are from `simsys run` on `scenario.yaml` (24 h at 100 ms steps, 5 min warmup: 861,000
observed steps, and the fault-free twin beside them: about 3 to 5 minutes in a Release build on the
author's machine).

- **The day passes.** `run` exits 0:

  ```
  slo
    status  metric       node       op    observed   max
    PASS    p99_latency  users      send  4.82ms     300ms
    PASS    queue_lag    message_q  -     <1us       10s
    PASS    drop_rate    -          -     9.326e-05  0.001
  ```

  The busiest copies: `chat_servers` 0.5384 on average and 1 at its peak (red for 61.9 s: the
  minute it is down and 1.9 s of the storm); `message_store` 0.834, `presence` 0.8299, `sync_workers` 0.7031,
  `push` 0.675.

  | op | offered/s (day mean) | mean | p50 | p95 | p99 |
  |---|---|---|---|---|---|
  | `api` | 5.993k | 3.96 ms | 3.8 ms | 6.15 ms | 7.51 ms |
  | `connect` | 2.996k | 1.55 ms | 1.48 ms | 2.48 ms | 3.08 ms |
  | `send` | 20.97k | 3.38 ms | 3.34 ms | 4.33 ms | 4.82 ms |

- **The 21:30 fault and the reconnect storm.** At 21:30 every chat server dies, and every open
  socket with it: the level, 1.617M connections, drops to 0 at once. For the minute nothing is
  served: the users' sends and connects and the sync workers' deliveries are lost (2.154M
  requests lost for good; the drops count attempts because the retrying sync workers reach the
  gateway), and no connection is open, so nothing pings. The dropped users wait for the tier, and
  while they wait their sessions end as they would have: of the 1.617M, `e^(−1 min / 6 min)` =
  84.6 % (1.369M) are still waiting at 21:31; the users who tried to connect during the minute
  (4.5k a second, their connects lost) wait with them, 248k of them still there at 21:31. The
  two nearly cancel, and not by chance: at the level's fixed point the users who would have
  ended a session in the minute are as many as those who would have started one (`C × (1 −
  e^(−for/session))` against `λ × session × (1 − e^(−for/session))`, with `C = λ × session`). So
  the storm is 1.617M users again, spread over `reconnect_within` (30 s): 53.9k connects a second
  on top of the users' own 4.5k, twelve times the usual. As the level refills, its pings come back
  with it, and the gateway's offered load peaks at the storm's last moment, 93.9 % of the tier,
  against the 81 % of the 21:00 peak:

  ```
  fault 21:30 node_down chat_servers for 1m
    during:   offered 35.98k/s, served 0/s, lost 2.159M attempts (2.154M requests lost for good), backlog 0, drop_rate over its max for 1m
    after:    no backlog to drain, reconnect storm 1.617M connections over 30s, chat_servers peak 93.9% at 21:31:29.9
    recovery: back within 1% (every op's p99, every node's rho) at 21:38:58.7 (478.7s after the fault cleared)
  ```

  (The block's count is the connections the fault dropped; the storm is the users still waiting
  when it clears, here the same 1.617M to four digits.) The run is back within 1 % of its twin 8
  minutes after the fault: the storm's users come back with fresh sessions, and while each waits
  its turn within the 30 s (15 s on average) none of their sessions ends, where the twin's
  connected users' do: about `15 s / 6 min` = 4 % of them too many, so the level overshoots the
  twin's and the extra pings decay with the 6-minute session. The day's highest level is that
  overshoot, 1.680M connections (`simsys run --report json`'s `connections_peak`), above the 1.617M
  the tier held at 21:30; the JSON's fault object has the storm's numbers unrounded
  (`"reconnect_storm_connections": 1616857.59...`, `"reconnect_storm_over_s": 30`,
  `"peak_offered_rho": 0.9392...`, `"peak_offered_rho_at_s": 77489.9`, `"recovered_at_s":
  77938.7`).
- **The window is the lesson.** The same day for six values of `reconnect_within` (one `simsys
  run` each: `simsys sweep`'s columns do not read the fault block, and the node table's
  `peak_rho` is the busy fraction, 1 in the fault minute whatever follows):

  | `reconnect_within` | storm connects/s | gateway peak (offered) | at | dropped by `chat_servers` | back within 1 % |
  |---|---|---|---|---|---|
  | 10 s | 161.7k | **112 %** | 21:31:09.9 | 2.205M (room 20k full) | 21:31:10.4 |
  | 20 s | 80.8k | 98.4 % | 21:31:19.9 | 2.159M | 21:36:29.7 |
  | 30 s (default) | 53.9k | 93.9 % | 21:31:29.9 | 2.159M | 21:38:58.7 |
  | 40 s | 40.4k | 92.1 % | 21:31:39.9 | 2.159M | 21:40:45.5 |
  | 50 s | 32.3k | 91.2 % | 21:31:49.9 | 2.159M | 21:42:09.2 |
  | 60 s | 26.9k | 90.9 % | 21:31:59.9 | 2.159M | 21:43:18.2 |

  (Storm connects/s: the 1.617M users still waiting at 21:31, above, over the window; the other
  columns are each run's fault block and node table.) With every client back within 10 s the
  tier is offered 112 % of what it can serve: its rooms (1000 requests × 20 servers) fill and it
  drops 46k more requests than at 30 s, the thundering herd of ch. 12's question. About a third
  of them are reconnects (the rest pings and messages), and a dropped reconnect is not a user
  lost: the app tries again after another backoff (the model: a wait of mean
  `reconnect_within`, 10 s here), so they come back in the seconds after the storm and the level
  loses nobody (before the Task 15 review they were
  lost for good, uncounted, which is part of how this row recovered "at once"). Spreading the same
  1.617M reconnects over 20 s keeps the tier just under 100 %, and every second beyond buys less,
  because the pings of the refilling level, not the reconnects, are most of the peak (at 60 s the
  storm adds 27k/s to about 428k/s of pings and messages). The longer windows recover later: the
  storm's users wait half the window on average without ending a session, so the level
  overshoots the twin's by about `window / 2 ÷ 6 min` of them (1.4 % at 10 s, 8.3 % at 60 s) and
  the larger overshoot takes longer to decay; at 10 s it is the smallest. All three SLOs pass at
  every point. To reproduce one point, edit `reconnect_within` and `simsys run` the file (the table: six such
  runs, `simsys run` and `simsys run --report json` each).
- **Fan-out and the queue.** 31.5k messages/s at the peak are 44.1k copies/s through
  `message_q` and 64 sync workers (70 % busy on paper with the retried pushes); the queue never
  falls behind (`<1us`).
- **The API servers' `kv_cache` hit ratio**, computed with the Che approximation from its 20M
  keys and the users' key distribution (Zipf 0.8 over the 50M daily users: a mild skew, because
  activity spreads over many users rather than a few hot keys): `hit_ratio=0.763 (che: zipf 0.8
  over 50M keys from users)`.

## What it abstracts away
- **Which server a user is on.** The 20 servers hold equal shares; the discovery's hand-off of a
  user to "the best server" (ch. 12) is a `lookup`, the last step of the API servers' `api` op,
  that costs time, not a choice. Nothing is called after it, and the users' `send` and `connect`
  reach `chat_servers` through the load balancer, not through the lookup, so the discovery's
  `failover: 10s` (callers held on a dead replica they looked up) never reaches the gateway: at
  21:30 nobody is sent to a dead chat server by a stale address, they find the whole tier down
  (a step to `chat_servers` after the lookup would add that failover to the fault's cost).
- **What a reconnect costs.** A reconnect is one request like a message; a real one is a TLS
  handshake, a login through the API servers and a discovery lookup, which would make the storm
  heavier than the table says, and land part of it on `api_servers` and `discovery`.
- **Clients that give up, or come back early.** Every dropped user comes back, evenly over
  `reconnect_within` from the fault's end, unless the session they would have had ends while
  they wait; a user who tried to connect during the minute comes back with them, and a reconnect
  the tier drops is tried again after another backoff. Nobody reconnects to another server while
  the tier is down (there is none), and nobody gives up. A `dc_down` of the gateway's datacenter
  would drop the connections as the `node_down` does (this file has one datacenter).
- **Presence as the book draws it.** The presence servers there keep each user's heartbeat timer
  in memory and write the store when a user goes online or offline; here every ping is a write.
- Message ordering, end-to-end encryption, media attachments, the bytes of a frame.

## Things to try
- `reconnect_within: 10s`: the table's first row, the tier at 112 % and its rooms full.
- `capacity: 10k/s` on `chat_servers` (the file's first number): `explain` reads `peak offered
  405k/s vs 200k/s total capacity  (202%, red)`.
- `heartbeat: 1s`: five times the pings, without a single message changing: `explain` puts the
  gateway tier at 340 % and presence's hottest machine at 363 %.
- `fanout: 20` (big groups): `message_q` carries 630k copies/s (the broker appends at 1M/s), the
  sync workers are at 1004 % and `push` at 964 %; `message_store` does not move (83.4 %).
- `message_store.nodes: 6`: `94.5k/s (31.5k/s × N 3) vs 60k/s (6 nodes)  (hottest 165%, red)`,
  the quorum writes saturate.
