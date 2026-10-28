# Tutorial 1: hello, one client and one service

## Design
- Clients send 100 requests/s, all of one op, `ping`.
- One service, `api`, with a capacity of 1k requests/s and a service time of 1 ms.
- One link in the same datacenter. It has no `latency` of its own, so it takes the
  default intra-datacenter round trip, 500 µs (250 µs each way; `explain` prints
  `one_way=250us (intra_dc_rtt/2)`).
- No SLOs: this scenario is the smallest one the loader and the engine accept, and the
  smoke test for both.

## What the model captures
- Utilization: rho = offered / capacity = 100 / 1000 = 0.1, i.e. 10 %, green
  (`explain`: `offered 100/s vs 1k/s total capacity (10%, green)`).
- Implied concurrency: with no `advanced.concurrency`, `c = max(1, ceil(1000/s × 1 ms))
  = 1` (Little's law at full load: busy servers = throughput × time in service), so
  `api` is a single-server queue.
- Mean response time = service time + queueing wait + link round trip. For a single
  server with random arrivals and random service times (M/M/1) the mean wait is
  rho / (1 − rho) × service time = 0.1 / 0.9 × 1 ms ≈ 0.11 ms, so the mean is
  1 + 0.11 + 0.5 ≈ 1.61 ms, which is what `run` prints.
- Tail latency: `run` prints p50 1.33 ms, p95 3.7 ms and p99 ≈ 5.7 ms. The percentiles
  come from a lognormal fitted to the mean and variance of the response time; with
  `variability=1` the service time is as spread out as an exponential distribution, so
  the p99 is about 3.5 × the mean even at 10 % load.
- `estimate` jumps straight to the steady state and prints the same numbers in one step,
  because nothing in this scenario changes over time.

## What it abstracts away
- Individual requests: traffic is a fluid rate, and latency is a distribution fitted
  to moments, not a histogram of simulated requests.
- Client behaviour: clients are open-loop. They keep sending 100/s however slow the
  answers get; real users back off, retry or give up.
- Everything inside `api`: threads, connection pools, garbage collection, caches. The
  node is a capacity, a service time and a queue limit.
- Payload size and network bandwidth: the link only adds its delay.

## Things to try
- Raise the client rate to `800/s`: rho 0.8 (yellow), and the mean wait grows to
  0.8 / 0.2 × 1 ms = 4 ms, so the mean is 5.5 ms and the p99 24.8 ms. Queueing delay
  grows like 1 / (1 − rho), not linearly.
- Raise it to `950/s`: rho 0.95 is past the red threshold (0.9), the mean is 20.5 ms
  and the p99 98 ms, although the node still serves everything.
- Raise it to `1.2k/s`: the queue fills to its default limit of 1000 requests and stays
  full, the excess 200/s is dropped (11k requests over the minute), and every answered
  request waits about 1000 / 1000/s = 1 s in the queue.
- Add `slo: [{metric: p99_latency, node: users, op: ping, max: 10ms}]` and find the rate
  at which it starts to fail (between 490/s and 500/s: at 500/s the p99 reaches 10 ms,
  at rho only 0.5).
