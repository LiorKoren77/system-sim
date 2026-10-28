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
- The cache tier's hit ratio (`hit_ratio: che`, from the clients' zipf 0.9 over 10M keys). The
  ring sends each key to one server, so the four servers hold different keys: 2M each, 8M
  together. With perfectly even arcs they would hit like one 8M-key LRU cache, 0.963. The arcs
  are not even: with imbalance k (1 + 1/sqrt(vnodes), the hottest server's share of the load
  over the average) the hottest server owns k/4 of the keys and the three others share the
  rest. Each server is an LRU cache of 2M keys over its own arc, which for the Che
  approximation is one cache of 2M / (its share) over the whole space, and it serves its share
  of the requests: h = Σ share × Che(2M / share). With `vnodes: 1`, k = 2: the hot server
  owns half the keys (a 4M-key cache over the space: 0.848) and each cool one a sixth (12M >
  10M keys: every key fits, 1), so h = 0.5 × 0.848 + 0.5 × 1 = **0.924**, and the DB gets
  1 − 0.924 of the gets instead of 1 − 0.963: twice the misses. Hit ratio is concave in cache
  size, so what the hot server loses outweighs what the cool ones gain. `explain` prints
  `hit_ratio=0.924 (che: ...; 2M keys x 4 partitioned, uneven arcs)`.
- The fault at 10 m takes one server's capacity away (the others absorb its load). The hit
  ratio stays the one resolved at build: the keys that move to the next server arrive cold in
  reality, but the model does not re-evaluate the hit ratio until phase 3.

## What it abstracts away
- The hash function, ring data structure and lookup (binary search).
- Replication across neighbours on the ring (see the KV store lab).

## Things to try
- `vnodes: 150` — the four servers sit within a few percent of each other (k = 1.08), and the
  hit ratio rises to 0.963, the even-arc value to three digits.
- `replicas: 2` — with vnodes 1 the imbalance is dramatic: k = 2 = N, the model's hot server
  owns every key, and the hit ratio falls to one 2M-key cache's, 0.747 (0.848 with vnodes 150).
