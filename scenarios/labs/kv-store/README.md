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
  `store  peak offered 90k/s (30k/s × N 3) vs 160k/s (16 nodes)  (hottest 60.8%, green)`.
- **The ring's hot machine.** Each machine holds the keys of the arcs before it on the ring, and
  with 150 virtual nodes the hottest carries 1 + 1/sqrt(150) = **1.0816** times the average:
  5.625k/s on average (56.25 %), **6.084k/s (60.8 %)** on the hottest. The simulator runs that
  machine as its own queue, so the `utilization` SLO reads it: `PASS utilization store 0.6084`.
- **Quorum latency.** A read is answered when the R-th fastest replica replies, a write when the
  W-th does. With each replica's sojourn exponential of mean m, the k-th fastest of 3 takes
  1/3, **5/6** or 11/6 of m for k = 1, 2, 3 (Rényi's representation, the schema's
  [`kv_cluster`](../../../docs/scenario-schema.md#kv_cluster) section). Here m is a machine's
  0.3 ms plus its Erlang C wait (3 servers per machine at rho 0.608 on the hottest, 0.559 on the
  others, weighted by the requests each serves): m = 0.371 ms, so W = R = 2 answer in
  5/6 × 0.371 = 0.309 ms. With the four 0.25 ms link legs and the coordinator's 0.232 ms a request
  takes **1.54 ms**, p99 **2.41 ms** (`run`; `get` and `put` alike since W = R).
- `run` (30 minutes, 1 minute of warmup) passes all three SLOs, exit 0:

  ```
  PASS    p99_latency  clients  put  2.41ms    20ms
  PASS    utilization  store    -    0.6084    0.8
  PASS    drop_rate    -        -    0         0.01
  ```

## What it abstracts away
- The two faults in the YAML (a node down at 10 m, a partition at 20 m) are accepted and do
  nothing: faults, sloppy quorum, hinted handoff and what `mode: ap` or `cp` does under a
  partition (stale versus unavailable) wait for phase 3. `mode` is printed, nothing more.
- Vector clocks and conflict resolution, read repair and Merkle-tree anti-entropy traffic,
  gossip, the storage engine (commit log, memtable, SSTables, compaction).
- The replicas' sojourns are taken as independent exponentials (a machine of 3 servers is not
  exactly one), the hot and cool machines' spread is folded into one mean, and which keys are
  hot is ignored (the clients' Zipf keys matter to caches, not to the ring's arcs).
- A request is lost in the share its replica-requests are dropped: a quorum that still answers
  after one of its three replicas dropped the request is not modelled.

## Things to try
- `nodes: 8`, the book's size: 90k replica-requests/s over 80k/s, 11.25k/s per machine on
  average (112.5 %) and **12.17k/s (122 %)** on the hottest (`explain`: `(hottest 122%, red)`).
  Every machine is over capacity, every queue fills, and the cluster drops a ninth of the
  replica-requests: `run` fails all three SLOs (drop rate 0.1111, utilization 1, `put` p99
  306 ms: each replica waits for its full room of 1000, 100 ms). The fix is more machines, not a
  different quorum: the load is N × the requests whatever W and R are.
- `vnodes: 1` (at 16 machines): the hottest machine carries twice the average (`imbalance=2`),
  11.25k/s, `(hottest 112%, red)`; it alone drops 1.25k of the 90k replica-requests/s (drop rate
  0.01389) and the three SLOs fail, while the average machine is at 56 %.
- `write_quorum: 3, read_quorum: 1`: a `put` waits for the slowest of three replicas (11/6 m) and
  a `get` for the fastest (1/3 m): `put` 1.91 ms (p99 3.31 ms), `get` 1.36 ms (p99 2.04 ms).
  `write_quorum: 1, read_quorum: 3` swaps them. The load does not change.
- `mode: cp`: no difference until partitions exist (phase 3).
- `nodes: 4`: 22.5k/s per machine; the cluster drops 55.6 % of the replica-requests.
