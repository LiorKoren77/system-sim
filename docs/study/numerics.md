# Numerics: floating point in a simulator

This note is about doubles, not about a C++20/23 feature. It is here because every
number the simulator prints (a hit ratio, a p99, a drop rate) is the end of a chain of
`double` arithmetic, and a few of those chains are written the way they are only to keep
the arithmetic honest. Line references are to the tree at the time of writing.

## 1. Why a simulator cares about floating point

A `double` keeps about 16 significant decimal digits (relative spacing 2.2e-16). Three
habits of this project make that matter:

- **Many steps.** `simsys run` takes `duration / step` steps (a 1 h scenario at the
  default 100 ms step is 36 000; 10^4 is a normal order of magnitude) and every step
  adds to the backlog of every node. An error of 1e-16 per step is invisible; an error
  that grows with each step is not.
- **Quantities near 0 and near 1.** Hit ratios, Erlang C waiting probabilities and
  `1 - e^{-x}` for a cold key all live at the edges of [0, 1], where "1 minus something
  tiny" loses the something (section 2) and where `a^k / k!` leaves the range of a
  double altogether (section 4).
- **Values that span hundreds of orders of magnitude.** Request probabilities of a Zipf
  key space over 1e9 keys reach 1e-27 at s = 3 (`src/caching/caching.cppm:276`).

The rule the code follows: reformulate the arithmetic so that intermediate values stay
well scaled, and write down why next to the formula.

## 2. `expm1` and `log1p`

**Cancellation** is what happens when two nearly equal numbers are subtracted: the
leading digits agree and cancel, and what remains is the rounding noise of the operands.
Take x = 1e-12 and compute 1 - e^{-x} in IEEE doubles (checked with Python's `math`):

    1 - exp(-1e-12)      = 9.999778782798785e-13    (relative error 2.2e-5: 5 digits right)
    -expm1(-1e-12)       = 9.999999999995e-13       (correct to the last bit)
    exact: x - x^2/2 + ...                          = 9.9999999999950000e-13

`exp(-1e-12)` is stored as 0.999999999999 rounded to the nearest multiple of 1.1e-16,
and the subtraction then exposes that rounding. `std::expm1(u)` computes e^u - 1
directly (a series for small u) with no subtraction; `std::log1p(u)` does the same for
ln(1 + u). The code uses them in exactly three places:

- `power_integral`, `src/caching/caching.cppm:211`: the Zipf bucket mass
  (b^{1-s} - a^{1-s}) / (1-s) is rewritten as a^{1-s} * expm1((1-s) ln(b/a)) / (1-s),
  which stays accurate as s approaches 1 (where the textbook form is 0/0).
- `one_minus_exp_neg`, `src/caching/caching.cppm:262`: the Che term 1 - e^{-p t}; a cold
  key over a short characteristic time has p t near 1e-12 (comment at lines 259-260).
- `lognormal_fit`, `src/queueing/queueing.cppm:168` (behind `lognormal_quantile` and the
  quorum's quadrature, section 7): sigma^2 = ln(1 + v/m^2), where v/m^2 is small for a nearly
  deterministic latency.

## 3. Bisection vs. Newton for the Che characteristic time

The Che equation (`src/caching/caching.cppm:135`) asks for the t where
`keys_in_cache(t) = capacity`; `keys_in_cache` (lines 265-272) is the sum of
count * (1 - e^{-p t}) over the buckets. The solver is `characteristic_time`
(lines 283-291): bisection, 80 steps, on log t.

- **Monotone and bracketed.** `keys_in_cache` grows with t (line 266). t >= C is a proven
  lower end (lines 278-280: 1 - e^{-x} <= x, so the sum is at most t) and 1e300 is the
  upper end. A sign test on a monotone function cannot leave its bracket.
- **Log space.** t_c ranges from about C to 1e27 and beyond (lines 275-278); halving in
  log t costs the same number of steps for all of them.
- **80 steps.** The bracket is at most ~1400 log units wide, 1400 / 2^80 = 1.2e-21 wide
  after 80 halvings: far below the double spacing of 2.2e-16 (about 64 halvings reach it;
  the rest is margin). A relative width of t that small is what "2^-80" buys.
- **Why not Newton.** Newton needs the derivative Σ count p e^{-p t}, which underflows to
  0 for large t (the flat tail of the curve), and from a poor start it can jump to a
  negative t or far outside the bracket. Bisection needs no derivative and cannot diverge.
- **Cost** (`src/caching/caching.cppm:144-145`): 80 steps x at most 2000 buckets =
  160 000 `exp` calls, about half a millisecond in Release, once per cache when the graph
  is built. Newton would need fewer steps; the saving is not worth the failure modes at half a millisecond.

## 4. Keeping sums in [0, 1]

Doubles top out at DBL_MAX = 1.8e308; the smallest normal number is 2.2e-308, below it
come denormals (less and less precision) down to 4.9e-324, and below that everything is 0.
In terms of the exponential: `exp(-708)` is the last normal value, `exp(-745)` is the last
nonzero one (4.9e-324), and `exp(-746)` is exactly 0.

**Erlang C** (`src/queueing/queueing.cppm:33-48`). The textbook formula contains
a^c / c! with a the offered load. At a = c = 712 it is 2.47e307, at 713 it is 6.7e307, and
a few servers later it passes DBL_MAX; a thousand pooled connections at rho 0.99 is
a = 990. The recurrence B(0) = 1, B(k) = a B(k-1) / (k + a B(k-1)) (lines 43-45) is a
probability at every step, so it never leaves [0, 1]. The test
`tests/unit/test_queueing.cpp:119` pins erlang_c(1000, 0.99) to 12 digits.

**Zipf normalisation** (`src/caching/caching.cppm:251-257`): `normalise` divides every
weight by the total, so Σ count * p = 1 and every probability is in [0, 1].

**The miss weights** (`log_miss_weights`, lines 301-309, and `filtered_by`, lines 400-405):
e^{-p t} is 0 for p t >= 746, and for a big cache every hot bucket has that, so
computing it directly could make all weights 0 and 0/0 is no distribution. The fix has two
parts: stay in logarithms (the log-sum-exp at lines 331-341 factors out the largest
term), and divide by the largest log weight before the final `exp`, so no value exceeds 1
and at least one bucket is exactly 1.

## 5. Moments through a graph

A latency estimate is two numbers, the mean and the variance (`LatencyEstimate`,
`src/flow/flow.cppm:64-76`). Two composition rules:

- **Sequential hops add** (`operator+=`, lines 153-157): means add always;
  variances add only if the hops are independent, Var(X+Y) = Var X + Var Y. Correlated
  hops (two calls to the same slow disk) would add covariance; the model ignores it.
- **Parallel fan-out** (`parallel_max`, lines 159-166): the max of the branch means and
  the max of the branch variances. The comment at lines 78-92 says why this is
  optimistic: E[max] >= max E (two Exp(1) branches: 1.5 against 1.0), and a fan-out of
  100 hides the tail amplification of "The Tail at Scale".

Percentiles come last: `lognormal_quantile` (`src/queueing/queueing.cppm:121-129`) fits
a lognormal with the same mean m and variance v (sigma^2 = ln(1 + v/m^2),
mu = ln m - sigma^2/2) and reads off exp(mu + sigma z). The `(variance / mean) / mean`
at line 126 instead of `variance / (mean * mean)` is for means below ~1e-162, whose
square underflows to 0 (test at `tests/unit/test_queueing.cpp:148`).

Where the fit is honest: a sum of many positive, independent, right-skewed terms, which
is what a path through several queues looks like. Where it is not: a **mixture**. A cache
with h = 0.9, a hit costing 1 ms and a miss 51 ms (both spikes) has mean 6 ms and
variance h(1-h) * 50^2 = 225 ms^2 (the two-point mixture of `conditional_hop`,
`src/engine/behaviours.cppm:187-191`). The lognormal fit with those moments gives a
median of 2.2 ms (true: 1), a p95 of 22.6 ms (true: 51) and a p99 of 58.9 ms (true: 51).
Real latency is bimodal and the fit smooths the two modes into one hump; reports
label the number "estimated" (`src/queueing/queueing.cppm:113`): it is a fit, not a measurement.

A mixture's moments come in two algebraically equal forms. With branch shares p_i, means m_i and
variances v_i, the mean is Σ p_i m_i and the variance either Σ p_i (v_i + (m_i - mean)^2) (the law
of total variance: the spread within the branches plus the spread between them) or
Σ p_i (v_i + m_i^2) - mean^2 (the second moment less the squared mean). The first is a sum of
non-negative terms; the second subtracts two large numbers when the spread is small against the
mean (a 12 s upload with a variance of 0.03 s^2: 144.03 - 144.00) and can come out a rounding
error below zero. `mixture` and the byte-sized node's per-op latency
(`Simulation::request_weighted_megabyte_sojourn`, two passes over the ops, no allocation) use the
first. Kingman's formula needs the second form's ingredient instead: a node whose requests take
different times (a blob store's objects of different sizes, plan 2c) hands its queue the
**squared coefficient of variation** of the mixture of service times,
E[S^2] = Σ p_i (1 + cs_i^2) s_i^2 (each branch's second moment is its variance plus its squared
mean, (1 + cs^2) s^2) and c_s^2 = E[S^2] / E[S]^2 - 1 (`MegabyteService::object_time_moments`).
There the result is clamped at 0, which only ever removes rounding (one size gives exactly
E[S^2] = E[S]^2 on paper), and it is large exactly when it matters: 1000 reads/s of 80 us beside
8 writes/s of 2 s give c_s^2 = 124, which is what keeps the small reads from shrinking the large
writes' wait.

## 6. Fluid steps

`FluidQueue::advance` (`src/fluid/fluid.cppm:149-167`) is a forward step in the sense
of Euler's method: freeze the demand for `dt`, move the state (the backlog) by rate times
`dt`, and repeat. The formulas are in the block at lines 127-147:

    drain    = min(demand + backlog / dt, capacity)       (line 188)
    backlog += (demand - drain) * dt                      (line 197)
    dropped  = max(backlog - queue_limit, 0) / dt         (line 202)

A step like this is exact for demand that is constant within the step, and wrong for
anything faster: a burst shorter than `dt` is averaged away, and the latency uses the
end-of-step backlog, "pessimistic by up to half a step's growth" (line 221). So `dt` should
be small against the time constants of the nodes: the drain time of a full waiting room
(`queue_limit / service_rate`) and the variation of the demand curve. The default is
100 ms (`docs/scenario-schema.md:116`).

**Conservation** holds for any `dt`, by construction: the same `drain` appears in the
backlog update and in the printed flow. Two tests prove it:
`flow is conserved every step: in = out + drops + delta backlog`
(`tests/unit/test_fluid.cpp:90`) and
`flow is conserved to rounding under adversarial demand, dt and queue limits`
(`tests/unit/test_fluid.cpp:140`), which uses steps of 1 us, 100 ms and 1 h and demands of
0 and 1e9, and asks for a residual below 1e-12 of the larger term.

The invariant at `fluid.cppm:33-40` is a modelling one: `service_rate`, `concurrency` and
`service_time_s` describe one machine only if rate = concurrency / time. The drain uses the
rate and the Erlang term uses the other two, so a mismatch makes them two machines.

### Choosing the step

The step is the one knob that is not a property of the system but of the model, so it
needs a rule. Two things in the tree set it: a token bucket and a queue.

**A token bucket's burst.** The bucket (`admit_token_bucket`,
`src/engine/behaviours.cppm:1455-1458`; the formula block is at lines 662-667) holds up to
`B` tokens (`burst`) and refills at the limit `L`. Under an offered rate `λ` above `L` it
loses tokens at `λ - L` per second, so it empties in

    t_empty = B / (λ - L)        (B tokens, λ and L in requests per second)

A step of `dt` does this in one move: `admitted = min(λ, L + tokens/dt)`, so a full bucket
lets `L + B/dt` per second through in the first step and exactly `L` after. If
`dt > t_empty` the whole burst passes inside the first step and the step reports its
average: the bucket's shape (a short fast stretch, then the limit) is gone. The
url-shortener's limiter (`scenarios/book/url-shortener/scenario.yaml:31-32`: `limit: 20k/s`,
`burst: 2000`, `step: 1s` at line 13) empties its bucket in under a second under any flood
more than 2,000 requests per second above the limit, so at that step it cannot show the
burst at all. The shipped scenario never floods it (the peak is 19.2k/s), which is why the
choice costs nothing there.

**The rule.** Take the highest offered rate the scenario reaches, `λ_peak`, and set

    step <= B / (λ_peak - L)                      for a bucket
    step <= (shortest spike) / 10                 for a queue

The first is the line above. The second is a rule of thumb, not a derivation: a spike
shorter than the step is smeared or missed (section 6, above), and ten steps across it
resolve its height and length to about a tenth.

**What the step does not lose.** The fluid step is exact for demand that is constant inside
it (the `FluidQueue::advance` comment, `src/fluid/fluid.cppm:149-167`), and the bucket's
total over a step is too: `L·dt + B` tokens pass whether the bucket empties at the start of
the step or at its end. So a coarse step does not miscount the burst. It loses only what is
shorter than the step: where in the step the burst fell, and anything the offered rate does
inside it. A client samples its rate once, at the start of the step
(`ClientBehaviour::rate`, `src/engine/behaviours.cppm:1339-1340`), so a spike that does not
fill whole steps is rounded to whole steps.

**Measured on the rate-limiter lab** (`scenarios/labs/rate-limiter/scenario.yaml`: `limit: 1k/s`
and `burst: 100` at lines 32-33; the api behind it has `replicas: 2` and `capacity: 600/s`,
so 1,200/s). The spikes are 5x of 800/s = 4,000/s, 2x = 1,600/s and 20x = 16,000/s:

| spike | `λ` | `B / (λ - L)` |
|---|---|---|
| 5x, 30 s | 4,000/s | 33 ms |
| 2x, 2 min | 1,600/s | 167 ms |
| 20x, 5 s | 16,000/s | 6.7 ms |

`λ_peak` is 16,000/s, so the rule asks for `step <= 6.7 ms`. The shipped `100ms` is above
it (and above the 5x spike's 33 ms): the lab already runs in the coarse regime, which is why
the api's `peak_backlog` below is 80 at 100 ms and 98 at 5 ms. The first step of a spike
sends the api `L + B/dt` per second: at 100 ms that is 2,000/s against 1,200/s, a backlog
of (2000 - 1200) × 0.1 = 80, which is the number the run prints. At 500 ms it is 1,200/s
and the backlog is 0; at 1 s it is 1,100/s, under the api's capacity, and the backlog is 0 too.

    $ build/gcc14-release/simsys run scenarios/labs/rate-limiter/scenario.yaml      # step: 100ms
    $ sed 's/^step: 100ms/step: 5ms/' scenarios/labs/rate-limiter/scenario.yaml > /tmp/rl-5ms.yaml
    $ sed 's/^step: 100ms/step: 1s/'  scenarios/labs/rate-limiter/scenario.yaml > /tmp/rl-1s.yaml
    $ build/gcc14-release/simsys run /tmp/rl-5ms.yaml     # and /tmp/rl-1s.yaml

The summary lines that differ (the `scenario` line and the `limiter` row are identical in
all three runs: `reject rate 14.3%`, `rejected 236.7k`; `red_s`, which adds a whole `dt`
per red step, is left out):

    step     node  peak_rho  peak_backlog   op            mean    p50     p95    p99
    5ms      api          1            98   clients.call  5.79ms  3.93ms  16.7ms 30.5ms
    100ms    api          1            80   clients.call  5.79ms  3.93ms  16.7ms 30.5ms
    1s       api     0.9167             0   clients.call  5.69ms  4.69ms  13ms   19.9ms

The limiter admits the same total at every step, as the paragraph above says. What a coarse
step erases is the burst's effect downstream: at 1 s the api sees 1,100/s, never overloads,
and the p99 falls from 30.5 ms to 19.9 ms. A step shorter than the 5 s spike but not a
divisor of it shows the other loss. At `step: 2s` the 5 s spike at 25 min is sampled at
t = 25:00, 25:02 and 25:04, so it is offered for 6 s: the limiter then prints
`reject rate 15%` and `rejected 251.7k`, 15,000 more, which is the 1 s of extra spike times
its excess over the limit (16,000 - 1,000 = 15,000/s). With the queue rule the step would be
at most 0.5 s for that spike, and at `500ms` the run gives back 236.7k.

(Numbers from a Release build of this tree; the run is deterministic, `seed: 1`.)

## 7. A quorum's reply time by quadrature

A kv_cluster's coordinator (book ch. 6) sends each request to the key's N replicas and answers
at the q-th reply (q = R for a read, W for a write). One of the N may be the ring's hot machine
(section 8), the others are cool, and each kind has its own sojourn mean and variance from its
queue. The q-th fastest of such a mix has no closed form, so `lognormal_order_statistic`
(`src/queueing/queueing.cppm:486`) integrates it.

**The integral.** The q-th reply X is later than t exactly when fewer than q replicas have
answered by t:

    S_q(t) = P(X > t) = P(fewer than q of the N have answered by t)

Replica i has answered by t with probability F_i(t), the CDF of a lognormal fitted to its
group's moments (`lognormal_fit`, line 165: Phi((ln t - mu) / sigma)). Fewer than q successes
among trials of different probabilities is the **Poisson-binomial** distribution, computed by
`fewer_than` (line 313) as a small dynamic programme: below[j] = P(exactly j so far), each
trial moving mass from j to j + 1 with its p. Then, integrating P(X > t) instead of t f(t):

    E[X] = integral of S_q(t) dt,     E[X^2] = integral of 2 t S_q(t) dt      (t from 0 to inf)

**The grid.** `moments_from_survival` (line 232) substitutes u = ln t (dt = t du), because the
times span orders of magnitude (a cool machine's 0.3 ms, a full queue's 100 ms), and sums
t S and 2 t^2 S with the trapezoid rule on a grid even in u. `lognormal_grid` (line 386)
spans every group's fit from mu - 8 sigma (Phi(-8) = 6e-16: nobody has answered) to
mu + 2 sigma^2 + 8 sigma (everybody has, even for the second moment, whose integrand peaks
at mu + 2 sigma^2), sigma / 2 apart for the narrowest group (`steps_per_sigma`, line 341).
Below the grid S is 1, and the rule's sum over that whole left tail is a geometric series,
added in closed form (line 235):

    h Σ_{j >= 1} e^(u0 - j h) = h e^u0 / (e^h - 1)

**The error bound.** With the tail summed, the rule runs over the whole real line in u, and
for an integrand that is smooth and dies off at both ends the trapezoid rule converges faster
than any power of h: its error falls like e^(-c / h), not like h^2. Without the closed-form
tail (the grid simply starting at mu - 8 sigma) the error is O(h^2): 1e-5 at sigma / 2, the
endpoint term of Euler-Maclaurin. With it, at sigma / 2:

| sojourn SCV | grid points | mean, relative error | variance, relative error |
|---|---|---|---|
| 1e-4 to 0.1 | 34-35 | below 1e-15 | below 1e-11 |
| 1 | 37 | 8e-15 | 3e-12 |
| 10 | 40 | 8e-13 | 9e-9 |
| 100 | 42 | 2e-11 | 9e-6 |

(one lognormal integrated back to its own moments; a Python run of the same rule). The
documented bound is **1e-8 relative for sojourn SCVs up to 10**, which
`one lognormal replica's order statistic returns its own moments`
(`tests/unit/test_queueing.cpp:372`) checks. A fluid sojourn's SCV is around 1 (exponential
service plus an Erlang wait); it reaches 10 only for very bursty arrivals. Two limits are
deliberate: a sojourn with no spread at all is given sigma = 0.001 (`min_sigma`, line 335),
since a step in S(t) cannot be resolved on any even grid; and a grid is at most 4000 points,
a very narrow group far from a wide one widening the step instead (then the bound loosens).

**The cross-check.** Fed N exponentials of one mean, the same quadrature must reproduce
Rényi's closed form (`exponential_order_statistic`, line 118: 1/3, 5/6, 11/6 of the mean for
the 1st, 2nd and 3rd of three): `the survival-function quadrature reproduces Renyi for
exponential replicas` (line 350) checks it to 1e-9 at a step of 0.25 in ln t (the
exponential's left tail is not Gaussian, so it needs a finer step than sigma / 2; 0.5 gives
1e-6 to 1e-8).

**What the fit costs.** The lognormal is a model of the sojourn's shape, not the queue's.
For three replicas of mean 1 ms and SCV 1 the lognormal gives 0.4210, 0.8262 and 1.7528 ms
for the 1st, 2nd and 3rd reply where exponentials give 0.3333, 0.8333 and 1.8333 ms: the fit
has lighter tails on both sides. Against a Monte Carlo of the kv-store lab's quorum with the
machines' exact M/M/3 sojourns (one virtual node, 16 machines) the quadrature is 2.8 % low
(0.3775 against 0.385-0.389 ms).

## 8. The ring's hottest machine

A consistent-hash ring (book ch. 5) gives each of M machines the arcs before its v virtual
nodes, once for each of the N copies of a key it holds (N = 1 behind a load balancer, N =
`replication` in a kv_cluster). One machine's share is a sum of N v arc lengths, so its
relative spread is about 1 / sqrt(N v) (the book: about 10 % at 100 virtual nodes). The
engine needs the HOTTEST machine's share, the maximum of M such shares, and that grows with
M. `ring_imbalance` (`src/engine/graph.cppm:1127`) takes

    k = min(1 + z_M / sqrt(N v), M / N)

with z_M the expected maximum of M standard normals, by Blom's approximation (1958)

    z_M = Phi^-1((M - 0.375) / (M + 0.25))

(`expected_normal_maximum`, `src/queueing/queueing.cppm:448`; Phi^-1 is `normal_quantile`,
line 426: Acklam's rational approximation, relative error 1.15e-9, polished by one Halley
step to full double precision). M / N is the cap: a machine holds a key at most once.

It is a heuristic (shares are neither normal nor independent: they sum to N), checked against
Monte Carlo rings (random virtual-node positions, each key on the next N distinct machines,
the hottest machine's load over the average, averaged over 20-400 rings):

| machines M | copies N | vnodes v | Monte Carlo | k | old 1 + 1/sqrt(v) |
|---|---|---|---|---|---|
| 4 | 1 | 150 | 1.086 | 1.086 | 1.082 |
| 4 | 1 | 1 | 2.086 | 2.049 | 2 |
| 8 | 3 | 150 | 1.073 | 1.068 | 1.082 |
| 16 | 3 | 150 | 1.084-1.091 | 1.083 | 1.082 |
| 16 | 3 | 1 | 1.945 | 2.021 | 2 |
| 128 | 3 | 150 | 1.133 | 1.122 | 1.082 |

(the review of plan 2c Tasks 9-10 measured 1.073, 1.091 and 1.133 for the N = 3 rows; our
own runs gave the others and 1.084 at 16). The old factor ignored M: it is "one standard
deviation above the average", right for a few machines and increasingly low beyond about 16.
Both rings use the same function, so the consistent-hashing lab's balancer and the kv-store
lab's cluster agree.

## 9. Try it

1. **Weaken the Che bisection.** In `characteristic_time` change `step < 80`
   (`src/caching/caching.cppm:286`) to 20, rebuild, and run `[caching]`. The bracket is
   then ~1400 / 2^20 = 1.3e-3 wide in log t, and the pinned checks fail:
   `the characteristic time of a uniform cache solves N(1 - e^(-t/N)) = C` (1e-9,
   `tests/unit/test_caching.cpp:221`) and `the characteristic time matches the dense
   reference` (1e-4, line 231). Try 40, 55 and 64 to see where each tolerance starts to hold.
2. **Undo the `expm1`.** In `one_minus_exp_neg` (line 262) return `1.0 - std::exp(-x)`
   and see whether any `[caching]` test notices. With p t around 1e-12 the error is 2e-5
   relative (section 2); a cold key over a short characteristic time is exactly that case.
3. **Coarsen the step.** Copy `scenarios/labs/unique-id/scenario.yaml` (step 100ms; its
   ticket server has capacity 8k/s and a queue limit of 1000), change the spike to
   `{at: 5m, for: 1s, factor: 3}` and compare `simsys run` at `step: 100ms` and at
   `step: 10s`. The demand is frozen across a step, so the 1 s spike is either smeared over
   ten seconds or missed, and the drop count and the p99 move. Conservation is not what
   breaks: in `tests/unit/test_fluid.cpp:140` widen `dts` to include `10s` and the
   residual check still passes (section 6). What coarse steps lose is fidelity, not balance.
4. **Find the step where the burst disappears.** Run the rate-limiter lab at `step: 5ms`,
   `100ms`, `500ms` and `1s` (the commands are in "Choosing the step", section 6) and watch
   the api row's `peak_backlog` (98, 80, 0, 0) and the p99 (30.5 ms until the step reaches
   1 s, then 19.9 ms). Then set the limiter's `burst: 1000`, which moves the ceiling to
   1000 / 15000 = 67 ms. The burst now overfills the api's queue at every step, so the step
   hardly matters: the api's `dropped` is 1.385k at 5 ms, 1.36k at 100 ms and 1.2k at
   500 ms. Why? (The api's waiting room, `queue_limit`, caps the backlog the same way at
   every step.)
5. **Coarsen the quadrature.** Set `steps_per_sigma` (`src/queueing/queueing.cppm:341`) to
   1.0, rebuild and run `[queueing]`: three checks fail, the fastest of three lognormals by
   4e-5 relative (0.42098 against 0.42100 ms). At 0.5 four fail and the fastest is 3.3 % off
   (0.4071 ms): two points per standard deviation is where the rule's e^(-c / h) error is
   still far below anything a report prints.
6. **Drop the closed-form tail.** In `moments_from_survival` (line 235) start `first` and
   `second` at 0 and the grid's first point at weight h / 2: the rule becomes the ordinary
   truncated trapezoid, and its error grows to O(h^2), about 1e-5 at sigma / 2.
7. **Watch the quorum hide the hot machine.** In `scenarios/labs/kv-store/scenario.yaml` set
   `vnodes: 1`: the hot machine is at 114 % and drops, yet the put's p99 is 2.65 ms (PASS),
   because a quorum of two answers from the two cool replicas. Then set `write_quorum: 3`:
   a put now waits for every replica, the hot one's full queue included, and loses what it
   drops (the README's "Things to try" has the numbers).
