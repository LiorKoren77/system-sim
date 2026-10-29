# system-sim

A fluid-flow **system design simulator** in modern C++23.

Describe a distributed system in YAML — clients, load balancers, caches, SQL and NoSQL
stores, message queues, CDNs, datacenters and the links between them — then push a day of
traffic through it (waves, spikes, noise, failures) and watch where it turns yellow and red.
Traffic is modelled as rates with analytic queueing per node, so every run is instant and
reproducible. Built to practise the systems in
*System Design Interview* (Alex Xu, vol. 1) and, at the same time, to study and teach
C++20/23: modules, concepts, ranges, coroutines, `std::expected` and friends.

Status: phase 2b. Clients follow rate curves (daily waves, ramps, scripted spikes,
replayed points) with seeded Ornstein–Uhlenbeck noise and state the key popularity they
ask for (Zipf or uniform); nodes charge a per-op `cost`. Next to clients and services run
load balancers (four policies), rate limiters (the five algorithms of ch. 4, with a counter
store), caches (a fixed hit ratio or one computed with the Che approximation from the
clients' keys; every write strategy including write-back; `then` steps), SQL databases (a
primary and read replicas, shards, a connection pool, replication lag and staleness), CDNs
that fetch their misses from an origin, and id generators (snowflake, ticket server, uuid).
Refused requests are counted apart from dropped ones (`reject_rate`), and a request counts
as answered unless it was refused or dropped. Six scenarios run end to end, the
[URL shortener](scenarios/book/url-shortener/) of ch. 8 among them (list below); the other
7 book systems and the kv-store lab validate and `explain`, waiting for phases 2c (blob-store
events, queues, workers, external sinks, `sweep`) and 3. The scenario language is
[`docs/scenario-schema.md`](docs/scenario-schema.md) (v1.3).
See [`docs/superpowers/specs/2026-10-27-system-sim-design.md`](docs/superpowers/specs/2026-10-27-system-sim-design.md)
for the design and [`docs/approaches.md`](docs/approaches.md) for the modelling approaches considered.

## Usage

```
simsys validate scenario.yaml        # types, references and units
simsys explain  scenario.yaml        # every resolved number and where it came from
simsys estimate scenario.yaml [--report table|json] [--no-color]
simsys run      scenario.yaml [--duration 30s] [--seed N] [--report table|json]
                              [--series out.csv] [--series-ops ops.csv] [--no-color]
simsys curve    scenario.yaml <client> [--step 1m] [--until 1h] [--seed N] [--noise]
```

`estimate` evaluates the long-run (steady) state once, at t = 0 (midnight): the rate curve's
value there, without noise (its first line says so); `explain`'s static load checks use the
curve's peak instead. `run` steps through the scenario and,
with `--series`, writes one CSV row per node per step
(`t_s,node,demand_per_s,drain_per_s,rho,backlog,drops_per_s,rejected_per_s,latency_ms,zone`),
the file a plot or GUI reads; demand, drain, drops and backlog balance, so the fluid queue's
conservation law, Σ demand·dt = Σ (drain + drops)·dt + backlog, can be checked from the file (a
token bucket's or a window's refusals are inside drain; a leaky bucket's overflow is in
`rejected_per_s`, in the drops' place). A link with a `bandwidth` is a pipe that queues and drops
megabytes: it gets a row of its own, named `from->to`, after the nodes in the series (same
columns, in MB), in the table's `links` section and in JSON's `links`, and a check line in
`explain`. With `--series-ops`, it writes one CSV row per client op
per step (`t_s,client,op,offered_per_s,answered_per_s,mean_ms`): what each client sent, the part
of it that came back answered (a limiter's refusals and a queue's drops are not; a request
waiting in a queue is answered later), and the mean
response in milliseconds (`inf` when the path never answers). The JSON report's ops carry
`answered_per_s` beside `offered_per_s` too.
Exit codes: 0 every SLO passed, 2 an SLO failed, 1 an error.
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
checked request reads and increments its counter; what the bucket refuses is rejected (the
book's 429), never forwarded, and reported apart from drops:

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
  client.op     offered/s    mean     p50     p95     p99  (estimated)
  clients.call        954  5.79ms  3.93ms  16.7ms  30.5ms
slo
  status  metric       node     op    observed  max
  PASS    utilization  api      -     0.6817    0.9
  PASS    p99_latency  clients  call  30.5ms    50ms
```

At the start of each spike the full bucket lets its 100 tokens through on top of the
refill, the api's queue absorbs them (backlog 80, 1.5 s red in 29 minutes), and from then
on exactly 1k/s passes. Latency counts only the requests that were answered (the 80 that
waited in the api's queue were, late). The lab's
README compares the five algorithms at `burst: 500`, where the same surge overflows the
api's queue and the p99 SLO fails.

`run --series-ops` on the URL shortener (24 hours at 1 s steps; the first rows):

```
$ simsys run scenarios/book/url-shortener/scenario.yaml --series nodes.csv --series-ops ops.csv
$ head -3 ops.csv
t_s,client,op,offered_per_s,answered_per_s,mean_ms
0,users,redirect,6604.27,6604.27,3.61342
0,users,shorten,653.169,653.169,3.98179
```

`explain` follows the load through every tier the engine runs. On
[YouTube](scenarios/book/youtube/) (ch. 14) the CDNs are sized in bytes, so they count megabytes
and are checked in MB/s, the requests beside; each upload's event goes through the transcoding
queue to the transcoders, a worker pool whose capacity is workers / cost (400 transcoders at 60 s
a video: 6.667/s), and on paper every video they are offered writes four renditions to the
transcoded store and a completion the metadata tier records (in a run, every video they drain).
The paper load does not queue, so the pool passes on all it is offered, and its red line says the
evening peak outruns it (trimmed):

```
$ simsys explain scenarios/book/youtube/scenario.yaml
...
  cdn_us            peak offered 14.08k MB/s (281.6/s) vs 100k MB/s total capacity  (14.1%, green)
  api               peak offered 48/s vs 12k/s total capacity  (0.4%, green)
  metadata_db       peak offered 98.28 units/s vs 9k/s total capacity  (1.09%, green)  ...
  transcoders       peak offered 9.6/s vs 6.667/s total capacity  (144%, red)
  transcoded_store  peak offered 9.811k MB/s (196.2/s) vs 10k MB/s total capacity  (98.1%, red)
```

Only a URL frontier's and a notification fan-out's onward load is still left out (types with
no phase yet): what lies behind them is marked `(load not propagated: frontier is not modelled
yet)`, with no percentage when nothing else reaches it and a lower bound (`≥1%`) otherwise.

What runs today: clients, services, load balancers, rate limiters, caches (a fixed or a
Che hit ratio), SQL databases (primary, read replicas, shards), CDNs and blob stores (sized in
bytes, they count megabytes, wait for whole objects and answer a write at its last byte, a read
at its first megabyte; a blob store's `then` steps are asynchronous event notifications, fired by
its writes only), id generators, message queues (the publisher answered at the append, every
message copied to each consumer group) and worker pools (M/M/c at workers / cost, waiting in their
queue's retention; their `then` steps fire for every job they drain, and nobody waits for them).
These seven scenarios run end to end:

- [`tutorial/hello`](scenarios/tutorial/hello/), [`tutorial/chain`](scenarios/tutorial/chain/):
  one service, then two in a chain: the queueing basics.
- [`labs/rate-limiter`](scenarios/labs/rate-limiter/): ch. 4's five algorithms against spikes.
- [`labs/consistent-hashing`](scenarios/labs/consistent-hashing/): ch. 5's hash ring over four
  Che caches in front of a SQL database.
- [`labs/unique-id`](scenarios/labs/unique-id/): ch. 7's ticket server vs snowflake; fails its
  drop-rate SLO on purpose (exit 2).
- [`book/url-shortener`](scenarios/book/url-shortener/): ch. 8 over 24 hours at 12.8k/s
  (peak 19.2k/s): limiter, 8 stateless web servers, a Che cache (h = 0.881), snowflake ids
  and a SQL database in 3 shards; both SLOs pass (redirect p99 7.49 ms, nothing dropped).
- [`book/youtube`](scenarios/book/youtube/): ch. 14 over 24 hours in bytes: two CDNs, blob
  stores, the transcoding queue and workers, the metadata tier. It runs to the end and passes
  its two checked SLOs (exit 0); from 15:43 the 400 transcoders fall behind (63.76k videos wait
  at midnight, where the run ends, and the backlog is still growing: the day's peak, about
  71.9k, comes near 02:18) and the `queue_lag` SLO that would say so is still skipped (phase 2c
  Task 8); the README's sizing is Task 16's.

Every other node type (external sinks, key-value clusters, ...) already validates and appears in
`explain` with its resolved numbers, but `estimate` and `run` stop with "which run/estimate do
not support yet (phase 2)" and exit 1. A `sweep` command
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
