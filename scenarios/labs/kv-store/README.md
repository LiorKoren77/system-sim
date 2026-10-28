# Lab: key-value store (ch. 6)

## Book design
- Partition with consistent hashing; replicate to the next N distinct nodes on the ring.
- Tunable consistency: W + R > N gives strong consistency; W=1 fast writes; R=1 fast reads.
- Conflicts resolved with vector clocks; failures detected by gossip; temporary failures
  handled by sloppy quorum + hinted handoff; permanent ones by anti-entropy (Merkle trees).
- Write path: commit log → memtable → SSTable; read path: cache → bloom filter → SSTables.

## What the model captures
- Write amplification: every put costs N=3 replica writes; the quorum W=2 means the
  client waits for the second-fastest replica (order statistic of replica latencies).
- Per-node capacity × 8 nodes vs 30k/s × (0.7 + 0.3×3) replica operations.
- Node failure at 10 m: its share moves to neighbours (hinted handoff keeps writes flowing).
- Partition at 20 m, a partition splitting the cluster in half: in `ap` mode the minority keeps
  serving (stale fraction rises); in `cp` mode requests that cannot reach a quorum are rejected
  (unavailable fraction rises).

## What it abstracts away
- Vector-clock semantics and conflict resolution, Merkle-tree anti-entropy, SSTable compaction,
  gossip timing (represented as `failover`-style delays).

## Things to try
- `write_quorum: 3, read_quorum: 1` and vice versa — latency moves between puts and gets.
- `mode: cp` — the partition shows up as drops instead of staleness.
- `nodes: 4` — the store goes red before any fault happens.
