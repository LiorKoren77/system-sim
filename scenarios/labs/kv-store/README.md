# Lab: key-value store (ch. 6)

## Book design
- Partition with consistent hashing; replicate to the next N distinct nodes on the ring.
- Tunable consistency: W + R > N gives strong consistency; W=1 fast writes; R=1 fast reads.
- Conflicts resolved with vector clocks; failures detected by gossip; temporary failures
  handled by sloppy quorum + hinted handoff; permanent ones by anti-entropy (Merkle trees).
- Write path: commit log → memtable → SSTable; read path: cache → bloom filter → SSTables.

## The scenario
30k requests/s (70 % `get`, 30 % `put`) from `clients` to a `coordinator` service (4 replicas of
15k/s, 0.2 ms), which sends each `get` to the store as a `read` and each `put` as a `write`. The
store is a `kv_cluster` of **16 machines** of 10k/s each (0.3 ms a request), 150 virtual nodes
per machine, N = 3 copies of every key, W = 2, R = 2: the book's quorum. The book's figure is 8
machines; at 8 the store is overloaded (see "Things to try"), so the lab ships the size that
fits. Three SLOs: the `put` p99 under 20 ms, the store's utilization under 0.8, drops under 1 %.

## What the model captures
- **Write and read amplification.** The coordinator sends every request to all N = 3 replicas
  of its key (ch. 6, "quorum consensus"), so the 30k requests/s are **90k replica-requests/s**
  for the machines. `explain`:
  `store  peak offered 90k/s (30k/s × N 3) vs 160k/s (16 nodes)  (hottest 60.9%, green)`.
- **The ring's hot machine.** Each machine holds the keys of the arcs before its virtual nodes,
  three times over (it is one of the three successive replicas of each), so its share of the
  load is a sum of 3 × 150 arcs and spreads about 1/sqrt(450) around the average. The hottest of
  16 such shares sits z_16 = 1.77 spreads above it (the expected maximum of 16 normals), so it
  carries 1 + 1.77/sqrt(450) = **1.0834** times the average: 5.625k/s on average (56.25 %),
  **6.094k/s (60.9 %)** on the hottest. The simulator runs that machine as its own queue, so the
  `utilization` SLO reads it: `PASS utilization store 0.6094`. (A heuristic checked against
  Monte Carlo rings: [numerics, section 8](../../../docs/study/numerics.md).)
- **Quorum latency over the real replica set.** A read is answered when the R-th fastest of its
  key's three replicas replies, a write when the W-th does. The three are three DIFFERENT
  machines, so at most one is the hot one: it is in a request's set with probability
  p_hot = 3k/16 = 0.203 (it is sent k/16 of the replica-requests, one per such request), and
  otherwise the request meets three cool machines. Each machine's reply time is a lognormal of
  its own queue's moments (0.3 ms plus the Erlang C wait of 3 servers: 0.394 ms on the hot
  machine at rho 0.609, 0.369 ms on a cool one at 0.559), and the R-th fastest is integrated
  from the survival function ([numerics, section 7](../../../docs/study/numerics.md)): W = R = 2
  answer in **0.313 ms**. With the four 0.25 ms link legs and the coordinator's 0.232 ms a request
  takes **1.54 ms**, p99 **2.31 ms** (`run`; `get` and `put` alike since W = R). (Every replica an
  exponential of the machines' mean sojourn, Rényi's 5/6 of it, gave 2.41 ms before.)
- **Loss over the same set.** A request is lost when fewer than its quorum of its three
  replica-requests come back: the binomial over its set, not the share of replica-requests
  dropped. Nothing is dropped here; see `nodes: 8` below.
- `run` (30 minutes, 1 minute of warmup) passes all three SLOs, exit 0:

  ```
  PASS    p99_latency  clients  put  2.31ms    20ms
  PASS    utilization  store    -    0.6094    0.8
  PASS    drop_rate    -        -    0         0.01
  ```

## What it abstracts away
- The two faults in the YAML (a node down at 10 m, a partition at 20 m) are accepted and do
  nothing: faults, sloppy quorum, hinted handoff and what `mode: ap` or `cp` does under a
  partition (stale versus unavailable) wait for phase 3. `mode` is printed, nothing more.
- Vector clocks and conflict resolution, read repair and Merkle-tree anti-entropy traffic,
  gossip, the storage engine (commit log, memtable, SSTables, compaction).
- The replicas' reply times are taken as independent lognormals of their machine's mean and
  variance (a machine of 3 servers is not exactly one: a Monte Carlo of the exact M/M/3 sojourns
  gives the `vnodes: 1` quorum below 2.8 % slower), the cool machines are one group (their own
  spread is ignored), and which keys are hot is ignored (the clients' Zipf keys matter to caches,
  not to the ring's arcs).
- A request that lost a replica-request but still met its quorum is timed as if all three
  replied (its quorum would in fact wait for a later reply). Only the overloaded settings below
  lose replica-requests, and there it costs little: a Monte Carlo of the store's second reply over
  the answered requests at `nodes: 8` gives a mean 1.6 % higher and a p99 of 123 ms instead of
  116 ms, about +6 ms on the p99 (at `nodes: 4`, +4.6 % and +9.8 %), a tail the clients' 100 ms
  timeout cuts off there anyway.

## Things to try
- `nodes: 8`, the book's size: 90k replica-requests/s over 80k/s, 11.25k/s per machine on
  average (112.5 %) and **12.01k/s (120 %)** on the hottest (k = 1 + z_8/sqrt(450) = 1.068;
  `explain`: `(hottest 120%, red)`). Every machine is over capacity, every queue fills, and the
  cluster drops a ninth of the replica-requests (`drop_rate 0.1111`). A request is lost only when
  two of its three replica-requests are dropped: 3.4 % of them, so 96.6 % are delivered (20.28k
  of the 21k gets/s), not the 8/9 a ninth would suggest. Every replica now waits for its full
  room, 1000 requests drained at 10k/s = 100 ms, with the spread the fluid gives a full queue
  (about 10 ms), so the second of three replies is 100.0 ms with a 6.7 ms spread, and the
  client's response about 101 ms: past the clients' `timeout: 100ms` more often than not. The
  timeout gives up on 56 % of the delivered requests (`run --report json`:
  `timed_out_per_s` 11.41k of the gets), so only 8.88k of the 21k gets/s are answered, in
  95.3 ms on average. `run` fails all three SLOs: drop rate 0.1111, utilization 1, `put` p99
  **100 ms**, the timeout itself: 56.2 % of the delivered requests timed out, more than the 1 % a
  p99 leaves above it, so the client never saw its 99th percentile answered (the table prints
  `>100ms` in every percentile column, and the SLO line says `56.2% of delivered requests timed
  out (more than 1%)`; the lognormal fit without the timeout said 118 ms). (Before the quorum was taken over the real replica set the lab
  said 306 ms and blamed the room; the 306 ms came from treating each replica as an exponential
  of mean 100 ms, whose spread is as large as its mean.) The fix is more machines, not a
  different quorum: the load is N × the requests whatever W and R are.
- `vnodes: 1` (at 16 machines): the ring is at its most uneven, the hottest machine carries
  1 + z_16/sqrt(3) = 2.02 times the average (`imbalance=2.02`), 11.37k/s, `(hottest 114%, red)`;
  it alone drops 1.37k of the 90k replica-requests/s (drop rate 0.01522) and the utilization and
  drop-rate SLOs fail, while the average machine is at 56 %. The `put` p99 **passes**: 2.65 ms.
  A key's three replicas include the hot machine with probability 3 × 2.02/16 = 0.379, and then
  the two cool replicas answer a quorum of two first, so the store's time is 0.378 ms and no
  request is lost: the quorum hides the hot machine, which is why Dynamo answers at W or R of
  N replies and not at the slowest. Add `write_quorum: 3` and it no longer does: a `put` waits
  for every replica, the full queue included, and loses what the hot machine drops: 4.6 % of the
  puts (8.59k of 9k delivered). The sets that hold the hot machine are 0.379 of the puts offered
  but, losing more, only 0.349 of those delivered; they answer in about 100 ms (its full room),
  the all-cool sets in 0.6 ms, so a delivered put takes 36.7 ms on average, p99 227 ms, before
  the timeout. The slow ones meet the 100 ms timeout: 569/s time out, 8.02k/s are answered, mean
  27.2 ms, p50 22.3 ms (the fit to every delivered put, before the timeout cut it: the timed-out
  puts are its slowest 6.62 %, so its median is the client's), p95 and p99 `>100ms`: 6.62 % of the delivered puts
  timed out, so the p99 SLO fails at
  the timeout (`6.62% of delivered requests timed out (more than 1%)`). (Weighting the two sets
  by the puts offered, the lost ones included, gave 7.43 % timed out and a mean of 29.4 ms, 39.6
  ms and 233 ms without the timeout, in an earlier version of the quorum model.)
- `write_quorum: 3, read_quorum: 1`: a `put` waits for the slowest of three replicas and a `get`
  for the fastest: `put` 1.87 ms (p99 3.3 ms), `get` 1.4 ms (p99 2.05 ms).
  `write_quorum: 1, read_quorum: 3` swaps them. The load does not change. `explain` marks either
  with `(W + R ≤ N: a read may miss the latest write)` only when the two quorums stop overlapping
  (W + R = 4 > 3 here: they still do).
- `mode: cp`: no difference until partitions exist (phase 3). With quorums that do not overlap
  (say `write_quorum: 1, read_quorum: 1`) `explain` says that cp contradicts them.
- `nodes: 4`: 22.5k/s per machine; the cluster drops 55.6 % of the replica-requests and delivers
  41.7 % of the requests (the binomial again: a request needs two of its three); the 100 ms
  timeout then gives up on 56 % of those, as at 8 machines, so 18.3 % are answered (3.83k of the
  21k gets/s).
