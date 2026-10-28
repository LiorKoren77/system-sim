# Modelling Approaches for a System Design Simulator

This document compares the ways one can model "traffic flowing through a graph of
capacity-limited nodes". Decision: **A (discrete-event simulation) is the engine**,
**B (analytical) becomes an `estimate` mode and a test oracle**, **C is rejected**.

Running example used throughout (from the news-feed chapter):

```
users ──► lb ──► web(×8) ──► feed_cache ──(miss)──► post_db
   10k read/s    2k/s each     300k/s, hit 90%        5k/s, queue 500
```

Service rates: lb 100k/s, web 2k/s per replica, cache 300k/s, db 5k/s.
Latencies: intra-DC link 0.5 ms, cache op 0.2 ms, db op 5 ms.

---

## A. Discrete-event simulation (DES), request level

### Mechanism

A simulated clock and a priority queue of `(time, seq, event)`. The engine pops the
earliest event, executes it, and the handler may schedule new events in the future.
The clock *jumps* to the next event; idle time costs nothing.

A node is a set of `c` workers plus a bounded FIFO queue:

```
on arrival(msg) at node N, time t:
    if a worker is free:   schedule COMPLETE(msg, N) at t + service_time(N, msg)
    elif queue.size < limit: queue.push(msg)
    else:                  drop(msg)           # the "red zone"
on complete(msg, N):
    next = route(N, msg)                       # behaviour decides: reply, forward, fan-out
    schedule ARRIVE(msg, next) at t + link_latency(N → next)
    if queue not empty:    pop and schedule its COMPLETE
```

Utilization is `busy_worker_time / (c * window)`. Yellow/red are thresholds on
that number; drops happen when the queue overflows or a deadline passes.

### Worked timeline (one request, cache miss)

| t (ms) | event |
|---|---|
| 0.000 | users creates `read_feed #1`, sends on link users→lb (0.5 ms) |
| 0.500 | lb receives; service 0.01 ms; picks web-3 |
| 0.510 | lb → web-3 link 0.5 ms |
| 1.010 | web-3 receives; service 0.5 ms; forwards to feed_cache |
| 1.510 | → feed_cache (0.5 ms link) |
| 2.010 | feed_cache: key not present → miss; 0.2 ms service; reply to web-3 |
| 2.710 | web-3 forwards to post_db |
| 3.210 | post_db: all workers busy, queue depth 37 → waits |
| 3.210 + wait | post_db service 5 ms |
| ... | reply travels back; `users` records end-to-end latency 15.4 ms |

### What emerges without being programmed

- At 10k/s with 90 % hits, post_db sees 1k/s on a 5k/s budget: ρ = 0.2, p99 ≈ 7 ms.
- Drop the cache hit ratio to 50 % (a cold cache, or a Zipf exponent change) and
  post_db sees 5k/s: ρ = 1.0. The queue grows, p99 climbs past 100 ms, then the
  queue hits 500 and drops begin. Students *see* the knee.
- Add client retries with a 50 ms timeout: retries arrive while the queue is already
  full, demand doubles, the system collapses — the classic retry storm.
- Kill post_db.primary at t = 60 s for 10 s: the replica lag, failover delay and
  backlog drain are all visible in the time series.

### C++ shape

```cpp
struct Event { SimTime at; std::uint64_t seq; NodeId target; Message msg; Kind kind;
               auto operator<=>(const Event&) const = default; };   // ordered by (at, seq)

class Engine {
    std::priority_queue<Event, std::vector<Event>, std::greater<>> queue_;
    SimTime now_{};
public:
    void run_until(SimTime end) {
        while (!queue_.empty() && queue_.top().at <= end) {
            auto ev = queue_.top(); queue_.pop();
            now_ = ev.at;
            nodes_[ev.target].handle(ev, *this);   // may call schedule(...)
        }
    }
};
```

**Two sub-styles.** *Event-oriented* (above): behaviours are callbacks. *Process-oriented*
(SimPy style): each request is a coroutine that `co_await link(0.5ms)`,
`co_await service(node)`. Process style reads like the story a student would tell;
event style is faster and simpler to make deterministic. Plan: event-oriented core,
with a coroutine façade (`std::generator`, custom awaitables) for writing node
behaviours — a deliberate C++20 coroutines study exercise.

**Time-stepped variant (A′).** Advance the clock in fixed Δt ticks and process
whatever is due. Simpler mental model, but Δt must be smaller than the smallest
service time (0.01 ms for an LB) while cross-DC latencies are 150 ms, so 99 % of
ticks do nothing. Rejected; the event queue gives the same results at a fraction of
the cost.

### Pros

- Tail latency, queue build-up, backpressure, retry storms, cold caches, failover
  transients are **emergent**, not formulas.
- Deterministic: same YAML + seed = identical output. Essential for grading and tests.
- The event stream *is* the trace the GUI will replay.
- Any mechanism with state over time (quorum, vector clocks, gossip, leader election)
  fits naturally.
- Fast: a typical scenario is tens of millions of events; C++ does that in seconds.

### Cons

- Cost scales with request count. Mitigation: `batch` factor (one event = k requests),
  statistical sampling of per-request latency.
- Warm-up transient: the first seconds are unrepresentative; reports need a
  `warmup:` window to discard.
- Random variation: students must learn to run several seeds. (Also a feature.)
- More engine code than B (≈ 3–4k lines before any node types).
- Single-threaded by design; parallelism only across runs.

---

## B. Fluid / analytical model

### Mechanism

Traffic is a *rate*, not individual requests. Propagate rates through the graph:

```
λ(users→lb)        = 10,000 /s
λ(lb→web)          = 10,000 /s   split across 8 → 1,250 /s each
λ(web→cache)       = 10,000 /s
λ(cache→db)        = 10,000 × (1 − hit) = 1,000 /s
```

Per node, with `c` servers of rate `μ`: utilization ρ = λ / (c·μ). Mean waiting time
from queueing theory:

- M/M/1: `W = 1 / (μ − λ)`
- M/M/c: Erlang-C formula
- if ρ ≥ 1: excess `λ − c·μ` is "dropped", latency is unbounded

End-to-end latency = Σ (link latency + node W) along each path, weighted by branch
probabilities. Tail latency via the exponential assumption:
`p99 ≈ W × ln(100)` for M/M/1.

### Worked example

| node | λ | c·μ | ρ | mean W |
|---|---|---|---|---|
| lb | 10k | 100k | 0.10 | 0.011 ms |
| web (each) | 1.25k | 2k | 0.63 | 1.33 ms |
| feed_cache | 10k | 300k | 0.03 | 0.003 ms |
| post_db (hit 0.9) | 1k | 5k | 0.20 | 0.25 ms |
| post_db (hit 0.5) | 5k | 5k | **1.00** | ∞ → red |

Read-feed mean latency at hit 0.9 ≈ 4 × 0.5 ms links + 0.011 + 1.33 + 0.003 + 0.1 × (0.5 + 0.25 + 5) ≈ 3.9 ms.
Instant, and *exactly* the back-of-the-envelope reasoning chapter 2 of the book teaches.

### Pros

- Instant; parameter sweeps are free; the whole design space can be plotted.
- Directly teaches Little's law, utilization and the knee of the latency curve.
- Trivial to implement (a few hundred lines on top of the same config graph).
- Serves as a **test oracle** for the DES: in steady state, A must converge to B.

### Cons

- Steady state only: no time axis, no bursts, no ramp-up, no diurnal curve.
- No feedback loops: retries, timeouts, backpressure, circuit breakers cannot be modelled.
- Assumes Poisson arrivals and exponential service; real p99 is usually worse.
- Cache hit ratio, replication lag, failover time are *inputs*, not outcomes.
- Partitions, quorum, vector clocks, leader election are meaningless (no events).
- Correlated fan-in (eight web servers all hitting the DB in lock-step) is invisible.

Role in this project: `simsys estimate scenario.yaml` prints the table above; the
unit tests use it to validate the engine.

---

## C. Real-time actor emulation (threads)

### Mechanism

Every node is a thread (or an actor with a mailbox). Requests are real messages on
real queues. Latency and service time are real `sleep_for` calls, optionally scaled
(1 ms simulated = 10 µs real). Capacity is a semaphore of `c` permits.

```cpp
void DbNode::run(std::stop_token st) {
    while (!st.stop_requested()) {
        auto msg = inbox_.pop();                        // blocking
        std::counting_semaphore_acquire guard{workers_}; // c permits
        std::this_thread::sleep_for(scale(service_time_));
        outbox_.push(reply(msg));
    }
}
```

### Pros

- Most intuitive: "it is a tiny distributed system running on your laptop".
- A node can later be swapped for the real thing (a real Redis behind the same interface).
- Excellent playground for C++20 concurrency: `std::jthread`, `stop_token`,
  `counting_semaphore`, `latch`, `barrier`, atomics.

### Cons

- **Non-deterministic.** Two runs differ; grading and regression tests are impossible.
- **Wall-clock bound.** 60 simulated seconds take ≥ 60 real seconds unless time is
  scaled, and scaling distorts ratios because `sleep_for` granularity is ~50 µs–1 ms
  while an LB service time is 10 µs.
- **Scale ceiling.** 10k requests/s is already hard on OS threads and timers;
  1M/s is impossible. The book's systems routinely assume 10k–1M QPS.
- Results depend on host load, core count, scheduler. A student on a laptop gets
  different numbers from the TA.
- The trace for the GUI must be reconstructed from wall-clock timestamps.
- Debugging data races in student-facing code is a poor use of teaching time.

Verdict: rejected as the engine. Some of its concurrency vocabulary can still be
exercised in the `sweep` command (parallel independent runs with `std::jthread`).

---

## Summary

| criterion | A. DES | B. analytical | C. real-time |
|---|---|---|---|
| p99 / tails | emergent | formula | noisy |
| bursts, ramps, failover | yes | no | yes |
| retries / backpressure | yes | no | yes |
| CAP, quorum, vector clocks | yes | no | yes |
| deterministic | yes | yes | no |
| 1M QPS feasible | yes (batching) | yes | no |
| implementation size | medium | small | medium |
| teaching value | "why did it fall over" | "will it fall over" | "what does it feel like" |

Final shape: **A as the engine, B as a mode and an oracle, coroutine façade on A for
behaviour authoring, C's concurrency only for parallel sweeps.**
