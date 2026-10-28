# Lab: consistent hashing (ch. 5)

## Book design
- Hash both servers and keys onto a ring; a key belongs to the first server clockwise.
- Adding/removing a server moves only the keys between it and its predecessor.
- Problems with few servers: uneven partition sizes and non-uniform key distribution.
  Fix: virtual nodes (e.g. 100–200 per server) — more even, at the cost of ring memory.

## What the model captures
- Load imbalance across the four cache servers as a function of `vnodes` (the standard
  deviation of partition sizes shrinks roughly with 1/sqrt(vnodes)); the hottest server's
  utilization is what the SLO checks.
- Removing one server at 10 m: its keys move to the next server(s) on the ring, that
  server's hit ratio drops (cold keys), the DB sees the misses.

## What it abstracts away
- The hash function, ring data structure and lookup (binary search).
- Replication across neighbours on the ring (see the KV store lab).

## Things to try
- `vnodes: 150` — the four servers sit within a few percent of each other.
- `replicas: 2` — with vnodes 1 the imbalance is dramatic.
