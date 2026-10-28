# system-sim

A fluid-flow **system design simulator** in modern C++23.

Describe a distributed system in YAML — clients, load balancers, caches, SQL and NoSQL
stores, message queues, CDNs, datacenters and the links between them — then push a day of
traffic through it (waves, spikes, noise, failures) and watch where it turns yellow and red.
Traffic is modelled as rates with analytic queueing per node, so every run is instant and
reproducible. Built to practise the systems in
*System Design Interview* (Alex Xu, vol. 1) and, at the same time, to study and teach
C++20/23: modules, concepts, ranges, coroutines, `std::expected` and friends.

Status: phase 2a. Clients follow rate curves (daily waves, ramps, scripted spikes,
replayed points) with seeded Ornstein–Uhlenbeck noise; nodes charge a per-op `cost`; and
load balancers (four policies), rate limiters (the five algorithms of ch. 4, with a counter
store) and caches (a fixed hit ratio, four synchronous strategies) run next to clients and
services. Refused requests are counted apart from dropped ones (`reject_rate`). The
[rate-limiter lab](scenarios/labs/rate-limiter/) runs end to end; drafts of all 8 book
systems and the other 3 labs live under [`scenarios/`](scenarios/) and validate and
`explain`, waiting for phases 2b (Che cache model, SQL, CDN), 2c (blob store, queues,
workers, external sinks, `sweep`) and 3. The scenario language is
[`docs/scenario-schema.md`](docs/scenario-schema.md) (v1.2).
See [`docs/superpowers/specs/2026-10-27-system-sim-design.md`](docs/superpowers/specs/2026-10-27-system-sim-design.md)
for the design and [`docs/approaches.md`](docs/approaches.md) for the modelling approaches considered.

## Usage

```
simsys validate scenario.yaml        # types, references and units
simsys explain  scenario.yaml        # every resolved number and where it came from
simsys estimate scenario.yaml [--report table|json] [--no-color]
simsys run      scenario.yaml [--duration 30s] [--seed N] [--report table|json]
                              [--series out.csv] [--no-color]
simsys curve    scenario.yaml <client> [--step 1m] [--until 1h] [--seed N] [--noise]
```

`estimate` evaluates the long-run (steady) state once, at t = 0 (midnight): the rate curve's
value there, without noise (its first line says so); `explain`'s static load checks use the
curve's peak instead. `run` steps through the scenario and,
with `--series`, writes one CSV row per node per step (`t_s,node,rho,backlog,drops_per_s,rejected_per_s,latency_ms,zone`),
the file a plot or GUI reads. Exit codes: 0 every SLO passed, 2 an SLO failed, 1 an error.
Colour is on when stdout is a terminal; `--no-color` turns it off and JSON never has it.
`run --seed N` replaces the scenario's `seed`, which seeds every client's `rate.noise`
process: the same seed gives the same run, byte for byte; without a noise block it changes nothing.

`curve` prints one client's offered rate over time as CSV (`t_s,rate_per_s`), sampled every
`--step` from t = 0 (midnight) up to `--until` (default: the scenario's duration), so the
`rate:` block (base, points or a ramp, times wave and spikes) can be plotted before anything
runs. `--noise` multiplies in the client's `rate.noise` process exactly as a run sees it:
seeded from `--seed` (default: the scenario's seed) as the engine seeds that client, advanced
every scenario `step` (so `--step` must be a multiple of it; every k-th value is printed),
and stepped once before each value is read, as the engine ticks a client before it offers,
so the t = 0 row already includes one noise step. On the
[rate-limiter lab](scenarios/labs/rate-limiter/), the 5x spike at 5 minutes:

```
$ simsys curve scenarios/labs/rate-limiter/scenario.yaml clients --step 1m | head -8
t_s,rate_per_s
0,800
60,800
120,800
180,800
240,800
300,4000
360,800
```

`explain` on the [chain tutorial](scenarios/tutorial/chain/) (trimmed):

```
$ simsys explain scenarios/tutorial/chain/scenario.yaml
scenario chain  version 1  duration 2m  step 100ms  warmup 10s  seed 1
nodes:
  users  client   dc=main  rate: base 1.5k/s  ops: read 80%, write 20%
  api    service  dc=main  replicas=2  capacity=1k/s (scenario)  service_time=500us (scenario)  c=1 (implied)  queue_limit=1000 (default)  ...
  db     service  dc=main  replicas=1  capacity=5k/s (scenario)  service_time=2ms (scenario)  c=10 (implied)  queue_limit=500 (scenario)  ...
links:
  users -> api  inout  one_way=250us (intra_dc_rtt/2)  delay_steps=0
  api -> db     inout  one_way=250us (intra_dc_rtt/2)  delay_steps=0
order: users, api, db
checks (peak offered load over the first 2m, no queueing):
  api  peak offered 1.5k/s vs 2k/s total capacity  (75%, yellow)
  db   peak offered 1.8k/s vs 5k/s total capacity  (36%, green)
```

The checks use each client's peak rate over the run (its curve sampled every minute, no
noise), the number a back-of-the-envelope estimate sizes for. Here the rate is constant, so
`run`'s utilizations agree with them:

```
$ simsys run scenarios/tutorial/chain/scenario.yaml
scenario chain  1100 steps observed  drop rate 0%
nodes
  node   mean_rho  peak_rho  peak_backlog  dropped  rejected  red_s
  users         -         -             -        -         -      -
  api        0.75      0.75             0        0         0      0
  db         0.36      0.36             0        0         0      0
ops (percentiles estimated: lognormal fit to the response moments)
  client.op    offered/s   mean     p50     p95     p99  (estimated)
  users.read        1.2k    5ms  4.35ms  10.4ms  14.8ms
  users.write        300  7.5ms  6.81ms    14ms  18.9ms
slo
  status  metric       node   op     observed  max
  PASS    p99_latency  users  read   14.8ms    20ms
  PASS    p99_latency  users  write  18.9ms    40ms
  PASS    drop_rate    -      -      0         0
```

The second worked example is the [rate-limiter lab](scenarios/labs/rate-limiter/)
(ch. 4): 800/s of `call` with three spikes (5x, 2x, 20x) through a 1k/s token bucket with
`burst: 100`, whose counters live in a cache, in front of an api of 2 x 600/s. Each
checked request reads its counter; what the bucket refuses is rejected (the book's 429),
never forwarded, and reported apart from drops:

```
$ simsys run scenarios/labs/rate-limiter/scenario.yaml
scenario lab-rate-limiter  17400 steps observed  drop rate 0%  reject rate 14.3% (worst: limiter)
nodes
  node      mean_rho  peak_rho  peak_backlog  dropped  rejected  red_s
  clients          -         -             -        -         -      -
  limiter    0.00954      0.16             0        0    236.7k      0
  counters   0.00954      0.16             0        0         0      0
  api         0.6817         1            80        0         0    1.5
ops (percentiles estimated: lognormal fit to the response moments)
  client.op     offered/s    mean  p50     p95     p99  (estimated)
  clients.call        954  5.78ms  4ms  16.4ms  29.4ms
slo
  status  metric       node     op    observed  max
  PASS    utilization  api      -     0.6817    0.9
  PASS    p99_latency  clients  call  29.4ms    50ms
```

At the start of each spike the full bucket lets its 100 tokens through on top of the
refill, the api's queue absorbs them (backlog 80, 1.5 s red in 29 minutes), and from then
on exactly 1k/s passes. Latency counts only the requests that were answered. The lab's
README compares the five algorithms at `burst: 500`, where the same surge overflows the
api's queue and the p99 SLO fails.

What runs today: clients, services, load balancers, rate limiters and caches with a fixed
hit ratio. Every other node type (databases, queues, CDNs, ...) already validates and
appears in `explain` with its resolved numbers, but `estimate` and `run` stop with
"which run/estimate do not support yet (phase 2)" and exit 1; so does a cache with
`hit_ratio: che`, `strategy: write_back` or `then` steps (phase 2b). A `sweep` command
(`--param nodes.users.rate.base=1k/s..100k/s:10`) is planned for phase 2c.

## Toolchain

GCC 14 is required (it builds the named modules); Clang 18 is used for clang-format only.
CMake ≥ 3.28, Ninja. C++23 with named modules.

## Build

```bash
cmake --preset gcc14-debug && cmake --build --preset gcc14-debug && ctest --preset gcc14-debug
./build/gcc14-debug/simsys validate scenarios/book/news-feed/scenario.yaml
```

## Licence

MIT — see [LICENSE](LICENSE).
