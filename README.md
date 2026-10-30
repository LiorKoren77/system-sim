# system-sim

A fluid-flow **system design simulator** in modern C++23.

Describe a distributed system in YAML — clients, load balancers, caches, SQL and NoSQL
stores, message queues, CDNs, datacenters and the links between them — then push a day of
traffic through it (waves, spikes, noise, failures) and watch where it turns yellow and red.
Traffic is modelled as rates with analytic queueing per node, so every run is instant and
reproducible. Built to practise the systems in
*System Design Interview* (Alex Xu, vol. 1) and, at the same time, to study and teach
C++20/23: modules, concepts, ranges, coroutines, `std::expected` and friends.

Status: phase 2 is done; its last part, 2c, added bytes, queues and workers, external sinks and
the sweep. Phase 3 is under way: scripted faults run (`faults:`, below); retries and the four
node types still missing come next.
Clients follow rate curves (daily waves, ramps, scripted spikes, replayed points) with seeded
Ornstein–Uhlenbeck noise, state the key popularity they ask for (Zipf or uniform) and the size
of each op (`sizes: {stream: 50MB}`), and may give up after a `timeout`; nodes charge a per-op
`cost`. Next to clients and services run load balancers (four policies), rate limiters (the five
algorithms of ch. 4, with a counter store), caches (a fixed hit ratio or one computed with the Che
approximation from the clients' keys; every write strategy including write-back; `then` steps),
SQL databases (a primary and read replicas, shards, a connection pool, replication lag and
staleness), CDNs and blob stores sized in bytes, links with a `bandwidth` (a pipe that queues and
drops megabytes), message queues and the worker pools that consume them, external sinks,
key-value clusters (ring, replication and quorums), search indexes and id generators. A step after
a failed step does not run, refused, dropped and timed-out requests are counted apart
(`reject_rate`, `drop_rate`, `timed_out_per_s`), and an SLO can be checked per window (`over: 1h`).
`simsys sweep` runs a scenario over a range of one key, the points in parallel. Ten scenarios run
end to end, four of the eight book systems among them (list below); chat-system, google-drive,
notification-system and web-crawler validate and `explain`, and wait for phase 3's types
(`ws_gateway`, `service_discovery`, `notification_fanout`, `frontier`). 1163 tests (Catch2, via
`ctest`). The scenario language is
[`docs/scenario-schema.md`](docs/scenario-schema.md) (v1.4).
See [`docs/superpowers/specs/2026-10-27-system-sim-design.md`](docs/superpowers/specs/2026-10-27-system-sim-design.md)
for the design and [`docs/approaches.md`](docs/approaches.md) for the modelling approaches considered.

## Usage

```
simsys validate scenario.yaml        # types, references and units
simsys explain  scenario.yaml        # every resolved number and where it came from
simsys estimate scenario.yaml [--report table|json] [--no-color]
simsys run      scenario.yaml [--duration 30s] [--seed N] [--report table|json]
                              [--series out.csv] [--series-ops ops.csv] [--no-color]
                              [--replications N [--jobs J]]
simsys curve    scenario.yaml <client> [--step 1m] [--until 1h] [--seed N] [--noise]
simsys sweep    scenario.yaml --param <dot.path>=<from>..<to>:<n> --metric <m>...
                              [--jobs N] [--format table|csv]
```

`estimate` evaluates the long-run (steady) state once, at t = 0 (midnight): the rate curve's
value there, without noise (its first line says so); `explain`'s static load checks use the
curve's peak instead. `run` steps through the scenario and,
with `--series`, writes one CSV row per node per step
(`t_s,node,demand_per_s,drain_per_s,rho,backlog,drops_per_s,dead_lettered_per_s,rejected_per_s,latency_ms,zone,queue_lag_s`,
`dead_lettered_per_s` a worker pool's dead letters, the last a message queue's lag in seconds,
each empty on other rows), the file a plot or GUI reads; demand, drain, drops and backlog balance, so the fluid queue's
conservation law, Σ demand·dt = Σ (drain + drops)·dt + backlog, can be checked from the file (a
token bucket's or a window's refusals are inside drain; a leaky bucket's overflow is in
`rejected_per_s`, in the drops' place). A link with a `bandwidth` is a pipe that queues and drops
megabytes: it gets a row of its own, named `from->to`, after the nodes in the series (same
columns, in MB), in the table's `links` section and in JSON's `links`, and a check line in
`explain`. With `--series-ops`, it writes one CSV row per client op
per step (`t_s,client,op,offered_per_s,attempts_per_s,answered_per_s,timed_out_per_s,mean_ms`):
what each client offered, what it sent with its `retries` (equal without them), the part of it that came back answered (a limiter's refusals and a queue's drops are not;
a request waiting in a queue is answered later), the part that came back after the client's
`timeout`, and the mean response in milliseconds (`inf` when the path never answers). The JSON report's ops
carry `answered_per_s` beside `offered_per_s` too, and `timed_out_per_s`: the requests the
client gave up on (0 without a `timeout`; `run`'s table shows them as `timed out/s`); a client's
percentile at q is read off the fit to every request the path delivered, before the timeout cut
it (the fit that decided which requests timed out, so its fastest q are all answered), capped at
the timeout; when more than 1 − q of them timed out (more than 1 % for a p99) the percentile is
the timeout itself (`>1s` in the table), which fails a latency SLO whatever its `max`.
Exit codes: 0 every SLO passed, 2 an SLO failed, 1 an error.
Colour is on when stdout is a terminal; `--no-color` turns it off and JSON never has it.
`run --seed N` replaces the scenario's `seed`, which seeds every client's `rate.noise`
process: the same seed gives the same run, byte for byte; without a noise block it changes nothing.
`run --replications N` (1 to 1000) runs the scenario N times, with seeds `seed`, `seed + 1`, ...,
`seed + N − 1` of that noise, on `--jobs` worker threads (the output does not depend on them), and
prints every number as its median across the runs with its 10–90 % band, nearest rank
(`5.69ms [5.64–5.75]`), each SLO as `PASS in 3 of 5` or `FAIL (passed in 2 of 5)` with its median
run's observed value; it exits with the median run's code. A scenario without noise runs once,
after a line that says so; `--series` needs a single run
([schema](docs/scenario-schema.md#command-line-simsys-run---replications-phase-3)).

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

`sweep` runs the scenario once per value of one key, the runs in parallel (`--jobs`, default:
the machine's hardware threads; the output does not depend on it), and prints one row per value
with the metrics asked for: `<client>.<op>.mean`, `.p50`, `.p95` or `.p99`, `<node>.mean_rho`,
`.peak_rho` or `.drop_rate`, `<queue>.lag`, or `<pool>.dead_lettered`
([schema](docs/scenario-schema.md#command-line-simsys-sweep-phase-2c)). On the
[hello tutorial](scenarios/tutorial/hello/), an M/M/1 queue with a 1 ms service time: the latency is
s / (1 − rho) plus the 0.5 ms round trip, and it bends upward as rho nears 1:

```
$ simsys sweep scenarios/tutorial/hello/scenario.yaml --param nodes.users.rate.base=100/s..900/s:3 \
      --metric users.ping.mean --metric api.mean_rho
nodes.users.rate.base  users.ping.mean  api.mean_rho
----------------------------------------------------
100/s                           1.61ms           0.1
500/s                            2.5ms           0.5
900/s                           10.5ms           0.9
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
  node   mean_rho  peak_rho  peak_backlog  dropped  rejected  red_s  peak_lag
  users         -         -             -        -         -      -         -
  api        0.75      0.75             0        0         0      0         -
  db         0.36      0.36             0        0         0      0         -
ops (percentiles estimated: lognormal fit to the response moments)
  client.op    offered/s  attempts/s  timed out/s   mean     p50     p95     p99  (estimated)
  users.read        1.2k        1.2k            0    5ms  4.35ms  10.4ms  14.8ms
  users.write        300         300            0  7.5ms  6.81ms    14ms  18.9ms
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
  node      mean_rho  peak_rho  peak_backlog  dropped  rejected  red_s  peak_lag
  clients          -         -             -        -         -      -         -
  limiter    0.00954      0.16             0        0    236.7k      0         -
  counters   0.00954      0.16             0        0         0      0         -
  api         0.6817         1            80        0         0    1.5         -
ops (percentiles estimated: lognormal fit to the response moments)
  client.op     offered/s  attempts/s  timed out/s    mean     p50     p95     p99  (estimated)
  clients.call        954         954    0.0002833  5.73ms  4.32ms  14.9ms  24.8ms
slo
  status  metric       node     op    observed  max
  PASS    utilization  api      -     0.6817    0.9
  PASS    p99_latency  clients  call  24.8ms    50ms
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
t_s,client,op,offered_per_s,attempts_per_s,answered_per_s,timed_out_per_s,mean_ms
0,users,redirect,6604.27,6604.27,6604.27,0,3.61342
0,users,shorten,653.169,653.169,653.169,0,3.98179
```

`explain` follows the load through every tier the engine runs. On
[YouTube](scenarios/book/youtube/) (ch. 14) the CDNs are sized in bytes, so they count megabytes
and are checked in MB/s, the requests beside; each upload's event goes through the transcoding
queue to the transcoders, a worker pool whose capacity is workers / cost (600 transcoders at 60 s
a video: 10/s), and on paper every video they are offered writes four renditions to the
transcoded store and a completion the metadata tier records (in a run, every video they drain).
The paper load does not queue, so the pool passes on all it is offered; its red line is a
utilization verdict (96 % at the evening peak), not a backlog, and the cross-DC origin link, with
a bandwidth, is checked in MB/s too (trimmed):

```
$ simsys explain scenarios/book/youtube/scenario.yaml
...
  cdn_us                    peak offered 14.08k MB/s (281.6/s) vs 100k MB/s total capacity  (14.1%, green)
  api                       peak offered 48/s vs 12k/s total capacity  (0.4%, green)
  metadata_db               peak offered 98.28 units/s vs 9k/s total capacity  (1.09%, green)  ...
  transcoders               peak offered 9.6/s vs 10/s total capacity  (96%, red)
  transcoded_store          peak offered 9.811k MB/s (196.2/s) vs 20k MB/s total capacity  (49.1%, green)
  cdn_eu->transcoded_store  peak offered 2.856k MB/s vs 5k MB/s  (57.1%, green)
```

Only a URL frontier's and a notification fan-out's onward load is still left out (phase 3
types): what lies behind them is marked `(load not propagated: frontier is not modelled
yet)`, with no percentage when nothing else reaches it and a lower bound (`≥1%`) otherwise.

What runs today: clients, services, load balancers, rate limiters, caches (a fixed or a
Che hit ratio), SQL databases (primary, read replicas, shards), CDNs and blob stores (sized in
bytes, they count megabytes, wait for whole objects and answer a write at its last byte, a read
at its first megabyte; a blob store's `then` steps are asynchronous event notifications, fired by
its writes only), id generators, message queues (the publisher answered at the append, every
message copied to each consumer group, the slowest group's lag reported and checked by the
`queue_lag` SLO), worker pools (M/M/c at workers / cost, waiting in their
queue's retention; their `then` steps fire for every job they drain, and nobody waits for them),
external sinks (a third party's API at the type's 1k/s and 100 ms, failing its `failure_rate`
of what it serves, counted as drops), key-value clusters (ch. 6's Dynamo-style store: `nodes`
machines on a ring whose hottest machine is its own queue, every request sent to N replicas,
answered at the W-th or R-th fastest of its own replica set and lost when fewer answer) and search indexes (ch. 13's trie database, served like a
service at the type's 5k/s and 5 ms; its rebuild is not modelled). These ten scenarios run end to
end:

- [`tutorial/hello`](scenarios/tutorial/hello/), [`tutorial/chain`](scenarios/tutorial/chain/):
  one service, then two in a chain: the queueing basics.
- [`labs/rate-limiter`](scenarios/labs/rate-limiter/): ch. 4's five algorithms against spikes.
- [`labs/consistent-hashing`](scenarios/labs/consistent-hashing/): ch. 5's hash ring over four
  Che caches in front of a SQL database.
- [`labs/unique-id`](scenarios/labs/unique-id/): ch. 7's ticket server vs snowflake; fails its
  drop-rate SLO on purpose (exit 2).
- [`labs/kv-store`](scenarios/labs/kv-store/): ch. 6's key-value store, 30k requests/s on 16
  machines with N = 3, W = R = 2: 90k replica-requests/s, the ring's hottest machine at 60.9 %,
  put p99 2.31 ms; every SLO passes (at the book's 8 machines it drops a ninth: "Things to try").
- [`book/url-shortener`](scenarios/book/url-shortener/): ch. 8 over 24 hours at 12.8k/s
  (peak 19.2k/s): limiter, 8 stateless web servers, a Che cache (h = 0.881), snowflake ids
  and a SQL database in 3 shards; all three SLOs pass (redirect p99 7.49 ms, nothing dropped, the
  database at 0.6433 in its worst hour, 13:02-14:02, against `max: 0.9, over: 1h`).
- [`book/youtube`](scenarios/book/youtube/): ch. 14 over 24 hours in bytes: two CDNs, blob
  stores, a 40 Gbit/s cross-DC origin link, the transcoding queue and 600 workers, the metadata
  tier. Every SLO passes (exit 0): European stream p99 465 ms, `transcode_q` 573 ms behind at
  worst, `cdn_us` at 0.1403 in its worst hour; the 300 MB uploads are answered at about 12 s
  within the 30 s timeout that stands for the book's resumable chunks. Its README sweeps the pool:
  at 400 workers the queue falls 2.66 h behind by midnight.
- [`book/news-feed`](scenarios/book/news-feed/): ch. 11 over 24 hours, sized for its 12:00 spike
  (21.6k/s): fan-out on write through a queue and 32 workers, feed, post and user caches, a SQL
  database in 6 shards and the friendship graph as a key-value cluster. Each post's 300 feed
  writes make the feed cache the biggest tier (10 replicas, 66 % at the peak); all three SLOs
  pass, through a scripted 30 s outage of the database at 15:00 (feed p99 66.1 ms, 5.99 ms
  without the outage; 0.039 % dropped, all of it in the outage; fan-out lag under a
  microsecond).
- [`book/search-autocomplete`](scenarios/book/search-autocomplete/): ch. 13 over 24 hours at
  24k keystrokes/s (peak 36k/s): browser cache, a Che-sized trie cache (h = 0.995) in front of a
  4-replica search index, and the query-log pipeline. Both SLOs pass (suggest p99 6.86 ms of
  100 ms, nothing dropped); the busiest node is on the data-gathering side, the log aggregators at
  60 % at the peak (6 workers; at 4 they sit on the red threshold, 90 %).

The four phase 3 types (WebSocket gateways, service discovery, notification fan-outs and URL
frontiers) already validate and appear in `explain` with their resolved numbers, but `estimate`,
`run` and `sweep` stop with "which run/estimate do not support yet" and exit 1, as do the four book
systems that need them. Each of those four READMEs opens with its status: the exact refusal, what
`explain` shows today (its red checks and the nodes it cannot reach yet) and what phase 3 will
model. Scripted faults run (`faults:`: a node or some of its replicas down, a capacity loss, a
latency spike, a traffic spike, a cache flush, a datacenter down; `estimate` shows the steady
state without them); retries and `on: failure` steps are phase 3 too; kv partitions are a phase 4
backlog item.

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
