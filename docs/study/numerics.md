# Numerics: floating point in a simulator

This note is about doubles, not about a C++20/23 feature. It is here because every
number the simulator prints (a hit ratio, a p99, a drop rate) is the end of a chain of
`double` arithmetic, and a few of those chains are written the way they are only to keep
the arithmetic honest. References name the function, comment or test to look for, since line
numbers move as the code does.

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
  key space over 1e9 keys reach 1e-27 at s = 3 (the comment above `characteristic_time` in
  `src/caching/caching.cppm`).

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
ln(1 + u). The code uses them in four places:

- `power_integral` (`src/caching/caching.cppm`): the Zipf bucket mass
  (b^{1-s} - a^{1-s}) / (1-s) is rewritten as a^{1-s} * expm1((1-s) ln(b/a)) / (1-s),
  which stays accurate as s approaches 1 (where the textbook form is 0/0).
- `one_minus_exp_neg` (`src/caching/caching.cppm`): the Che term 1 - e^{-p t}; a cold
  key over a short characteristic time has p t near 1e-12 (the comment above the function).
- `lognormal_fit` (`src/queueing/queueing.cppm`): sigma^2 = ln(1 + v/m^2), where v/m^2 is
  small for a nearly deterministic latency. Every lognormal of the simulator goes through it:
  `lognormal_quantile` (the reported percentiles), `lognormal_below` (what a client's timeout
  leaves answered, section 10) and the quorum's quadrature (section 7).
- `moments_from_survival` (same file, the quadrature of section 7): the trapezoid rule's
  geometric left tail h t / (e^h - 1) is written `h * t / std::expm1(h)`, which keeps its digits
  for the small grid steps h the rule uses.

## 3. Bisection vs. Newton for the Che characteristic time

The Che equation (`che_characteristic_time`, `src/caching/caching.cppm`) asks for the t where
`keys_in_cache(t) = capacity`; `keys_in_cache` is the sum of
count * (1 - e^{-p t}) over the buckets. The solver is `characteristic_time`:
bisection, 80 steps, on log t.

- **Monotone and bracketed.** `keys_in_cache` grows with t (every term 1 - e^{-p t} does).
  t >= C is a proven lower end (the comment above `characteristic_time`: 1 - e^{-x} <= x, so
  the sum is at most t) and 1e300 is the upper end. A sign test on a monotone function cannot
  leave its bracket.
- **Log space.** t_c ranges from about C to 1e27 and beyond (same comment); halving in
  log t costs the same number of steps for all of them.
- **80 steps.** The bracket is at most ~1400 log units wide, 1400 / 2^80 = 1.2e-21 wide
  after 80 halvings: far below the double spacing of 2.2e-16 (about 64 halvings reach it;
  the rest is margin). A relative width of t that small is what "2^-80" buys.
- **Why not Newton.** Newton needs the derivative Σ count p e^{-p t}, which underflows to
  0 for large t (the flat tail of the curve), and from a poor start it can jump to a
  negative t or far outside the bracket. Bisection needs no derivative and cannot diverge.
- **Cost** (the comment on `che_hit_ratio`): 80 steps x at most 2000 buckets =
  160 000 `exp` calls, about half a millisecond in Release, once per cache when the graph
  is built. Newton would need fewer steps; the saving is not worth the failure modes at half a millisecond.

## 4. Keeping sums in [0, 1]

Doubles top out at DBL_MAX = 1.8e308; the smallest normal number is 2.2e-308, below it
come denormals (less and less precision) down to 4.9e-324, and below that everything is 0.
In terms of the exponential: `exp(-708)` is the last normal value, `exp(-745)` is the last
nonzero one (4.9e-324), and `exp(-746)` is exactly 0.

**Erlang C** (`erlang_c`, `src/queueing/queueing.cppm`). The textbook formula contains
a^c / c! with a the offered load. At a = c = 712 it is 2.47e307, at 713 it is 6.7e307, and
at 714 it passes DBL_MAX; a thousand pooled connections at rho 0.99 is
a = 990. The recurrence B(0) = 1, B(k) = a B(k-1) / (k + a B(k-1)) (the comment in its body)
is a probability at every step, so it never leaves [0, 1]. The test
`erlang_c stays finite for a thousand servers near saturation` (`tests/unit/test_queueing.cpp`)
pins erlang_c(1000, 0.99) to 12 digits.

**Zipf normalisation** (`normalise`, `src/caching/caching.cppm`) divides every
weight by the total, so Σ count * p = 1 and every probability is in [0, 1].

**The miss weights** (`log_miss_weights` and `Popularity::filtered_by`, same file):
e^{-p t} is 0 for p t >= 746, and for a big cache every hot bucket has that, so
computing it directly could make all weights 0 and 0/0 is no distribution. The fix has two
parts: stay in logarithms (the log-sum-exp in `log_miss_weights` factors out the largest
term), and subtract the largest log weight before the final `exp` (dividing by the largest
weight, done in logarithms), so no value exceeds 1 and at least one bucket is exactly 1.

## 5. Moments through a graph

A latency estimate is two numbers, the mean and the variance (`LatencyEstimate` in
`src/flow/flow.cppm`). Two composition rules:

- **Sequential hops add** (`LatencyEstimate::operator+=`): means add always;
  variances add only if the hops are independent, Var(X+Y) = Var X + Var Y. Correlated
  hops (two calls to the same slow disk) would add covariance; the model ignores it.
- **Parallel fan-out** (`parallel_max`): the max of the branch means and
  the max of the branch variances. The comment above its declaration says why this is
  optimistic: E[max] >= max E (two Exp(1) branches: 1.5 against 1.0), and a fan-out of
  100 hides the tail amplification of "The Tail at Scale".

Percentiles come last: `lognormal_quantile` (`src/queueing/queueing.cppm`) fits
a lognormal with the same mean m and variance v (sigma^2 = ln(1 + v/m^2),
mu = ln m - sigma^2/2, `lognormal_fit`) and reads off exp(mu + sigma z). The
`(variance / mean) / mean` in `lognormal_fit` instead of `variance / (mean * mean)` is for
means below ~1e-162, whose square underflows to 0 (test `lognormal_quantile survives means
whose square underflows a double`).

Where the fit is honest: a sum of many positive, independent, right-skewed terms, which
is what a path through several queues looks like. Where it is not: a **mixture**. A cache
with h = 0.9, a hit costing 1 ms and a miss 51 ms (both spikes) has mean 6 ms and
variance h(1-h) * 50^2 = 225 ms^2 (the two-point mixture of `conditional_hop` in
`src/engine/behaviours.cppm`). The lognormal fit with those moments gives a
median of 2.2 ms (true: 1), a p95 of 22.6 ms (true: 51) and a p99 of 58.9 ms (true: 51).
Real latency is bimodal and the fit smooths the two modes into one hump; reports
label the number "estimated" (the comment on `lognormal_quantile`, and the op table's
`(estimated)` column): it is a fit, not a measurement.

A mixture's moments come in two algebraically equal forms. With branch shares p_i, means m_i and
variances v_i, the mean is Σ p_i m_i and the variance either Σ p_i (v_i + (m_i - mean)^2) (the law
of total variance: the spread within the branches plus the spread between them) or
Σ p_i (v_i + m_i^2) - mean^2 (the second moment less the squared mean). The first is a sum of
non-negative terms; the second subtracts two large numbers when the spread is small against the
mean (a 12 s upload with a variance of 0.03 s^2: 144.03 - 144.00) and can come out a rounding
error below zero. `mixture` and the byte-sized node's per-op latency
(`Simulation::request_weighted_megabyte_sojourn`, two passes over the ops, no allocation) use the
first. Kingman's formula needs the second form's ingredient instead: a node whose requests take
different times (a blob store's objects of different sizes) hands its queue the
**squared coefficient of variation** of the mixture of service times,
E[S^2] = Σ p_i (1 + cs_i^2) s_i^2 (each branch's second moment is its variance plus its squared
mean, (1 + cs^2) s^2) and c_s^2 = E[S^2] / E[S]^2 - 1 (`MegabyteService::object_time_moments`).
There the result is clamped at 0, which only ever removes rounding (one size gives exactly
E[S^2] = E[S]^2 on paper), and it is large exactly when it matters: 1000 reads/s of 80 us beside
8 writes/s of 2 s give c_s^2 = 124, which is what keeps the small reads from shrinking the large
writes' wait.

## 6. Fluid steps

`FluidQueue::advance` (`src/fluid/fluid.cppm`) is a forward step in the sense
of Euler's method: freeze the demand for `dt`, move the state (the backlog) by rate times
`dt`, and repeat. The formulas are in the comment block above it ("The §3.3 step"), one named
helper per line:

    drain    = min(demand + backlog / dt, capacity)       (drain_for)
    backlog += (demand - drain) * dt                      (grow_backlog)
    dropped  = max(backlog - queue_limit, 0) / dt         (spill_overflow)
    rho_wait = min(demand, drain) / capacity              the load the stochastic (Erlang)
                                                          wait reads: what arrives, not the
                                                          busy fraction drain / capacity, so
                                                          a backlog drained after the load
                                                          fell is waited out by the fluid
                                                          term alone

A step like this is exact for demand that is constant within the step, and wrong for
anything faster: a burst shorter than `dt` is averaged away, and the latency uses the
end-of-step backlog, "pessimistic by up to half a step's growth" (the comment on
`FluidQueue::backlog_wait_s`). So `dt` should
be small against the time constants of the nodes: the drain time of a full waiting room
(`queue_limit / service_rate`) and the variation of the demand curve. The default is
100 ms (the `step` row of the schema's [Top level](../scenario-schema.md#top-level)).

**Conservation** holds for any `dt`, by construction: the same `drain` appears in the
backlog update and in the printed flow. Two tests prove it:
`flow is conserved every step: in = out + drops + delta backlog` and
`flow is conserved to rounding under adversarial demand, dt and queue limits` (both in
`tests/unit/test_fluid.cpp`); the second uses steps of 1 us, 100 ms and 1 h and demands of
0 and 1e9, and asks for a residual below 1e-12 of the larger term.

The invariant in the comment on `Capacity` (`src/fluid/fluid.cppm`) is a modelling one:
`service_rate`, `concurrency` and
`service_time_s` describe one machine only if rate = concurrency / time. The drain uses the
rate and the Erlang term uses the other two, so a mismatch makes them two machines.

### Choosing the step

The step is the one knob that is not a property of the system but of the model, so it
needs a rule. Two things in the tree set it: a token bucket and a queue.

**A token bucket's burst.** The bucket (`RateLimiterBehaviour::admit_token_bucket`,
`src/engine/behaviours.cppm`; the formula block is in the class comment) holds up to
`B` tokens (`burst`) and refills at the limit `L`. Under an offered rate `λ` above `L` it
loses tokens at `λ - L` per second, so it empties in

    t_empty = B / (λ - L)        (B tokens, λ and L in requests per second)

A step of `dt` does this in one move: `admitted = min(λ, L + tokens/dt)`, so a full bucket
lets `L + B/dt` per second through in the first step and exactly `L` after. If
`dt > t_empty` the whole burst passes inside the first step and the step reports its
average: the bucket's shape (a short fast stretch, then the limit) is gone. The
url-shortener's limiter (`scenarios/book/url-shortener/scenario.yaml`: `limit: 20k/s`,
`burst: 2000`, and the scenario's `step: 1s`) empties its bucket in under a second under any flood
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
it (the comment above `FluidQueue::advance`), and the bucket's
total over a step is too: `L·dt + B` tokens pass whether the bucket empties at the start of
the step or at its end. So a coarse step does not miscount the burst. It loses only what is
shorter than the step: where in the step the burst fell, and anything the offered rate does
inside it. A client samples its rate once, at the start of the step
(`ClientBehaviour::rate`, `src/engine/behaviours.cppm`), so a spike that does not
fill whole steps is rounded to whole steps.

**Measured on the rate-limiter lab** (`scenarios/labs/rate-limiter/scenario.yaml`: the limiter's
`limit: 1k/s` and `burst: 100`; the api behind it has `replicas: 2` and `capacity: 600/s`,
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
    5ms      api          1            98   clients.call  5.73ms  4.39ms  14.6ms 24.1ms
    100ms    api          1            80   clients.call  5.73ms  4.32ms  14.9ms 24.8ms
    500ms    api          1             0   clients.call  5.79ms  3.93ms  16.7ms 30.5ms
    1s       api     0.9167             0   clients.call  5.69ms  4.69ms  13ms   19.9ms

The limiter admits the same total at every step, as the paragraph above says. What a coarse
step erases is the burst's effect downstream: at 1 s the api sees 1,100/s, never overloads,
and the p99 falls from 24.8 ms to 19.9 ms. At 500 ms the first step of a spike sends the api
1,200/s, exactly its capacity: no backlog, but rho 1 for the whole step, where the stochastic
wait sits at its 0.99 clamp, so the p99 is the highest of the four, 30.5 ms. At 5 ms and 100 ms the
burst leaves a backlog that the api works off at full capacity while the spike's admitted
1,000/s arrive: the stochastic wait reads the utilization of what arrives, min(demand, drain) /
capacity = 1000 / 1200 (`FluidQueue::advance`; the backlog's own drain time is the fluid term),
so the burst's wait falls with its backlog. (An earlier version read the busy
fraction, 1, and held at the clamp until the backlog was gone: a p99 of 30.5 ms at 100 ms, where
it is 24.8 ms now; spec §18 entry 14 keeps the history.) The
clients' `timeout: 1s` moves none of these figures: it times out 3.5e-7 of the delivered calls, and
a percentile is read off the fit to every delivered call, which the cut leaves alone
(section 10). A step shorter than the 5 s spike but not a
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
(`src/queueing/queueing.cppm`) integrates it.

**The integral.** The q-th reply X is later than t exactly when fewer than q replicas have
answered by t:

    S_q(t) = P(X > t) = P(fewer than q of the N have answered by t)

Replica i has answered by t with probability F_i(t), the CDF of a lognormal fitted to its
group's moments (`lognormal_fit`: Phi((ln t - mu) / sigma)). Fewer than q successes
among trials of different probabilities is the **Poisson-binomial** distribution, computed by
`fewer_than` as a small dynamic programme: below[j] = P(exactly j so far), each
trial moving mass from j to j + 1 with its p and keeping it with 1 - p. Then, integrating
P(X > t) instead of t f(t):

    E[X] = integral of S_q(t) dt,     E[X^2] = integral of 2 t S_q(t) dt      (t from 0 to inf)

**The grid.** `moments_from_survival` substitutes u = ln t (dt = t du), because the
times span orders of magnitude (a cool machine's 0.3 ms, a full queue's 100 ms), and sums
t S and 2 t^2 S with the trapezoid rule on a grid even in u. `lognormal_grid`
spans every group's fit from mu - 8 sigma (Phi(-8) = 6e-16: nobody has answered) to
mu + 2 sigma^2 + 8 sigma (everybody has, even for the second moment, whose integrand peaks
at mu + 2 sigma^2), sigma / 2 apart for the narrowest group (`steps_per_sigma`).
Below the grid S is 1, and the rule's sum over that whole left tail is a geometric series,
added in closed form (in `moments_from_survival`):

    h Σ_{j >= 1} e^(u0 - j h) = h e^u0 / (e^h - 1)

**The error bound.** With the tail summed, the rule runs over the whole real line in u, and
for an integrand that is smooth and dies off at both ends the trapezoid rule converges faster
than any power of h: its error falls like e^(-c / h), not like h^2. The closed-form tail is not
a refinement: it is most of the answer for a narrow sojourn. A grid that simply starts at
t0 = e^(mu - 8 sigma) leaves out the whole interval [0, t0], where S = 1, and that interval is
worth t0 of the mean, t0 / m = e^(-8 sigma - sigma^2 / 2) relative: 92 % at SCV 1e-4 (sigma =
0.01, so t0 is 0.92 m), 45 % at 0.01, 9e-4 at 1, and only at SCV 10 down to 1.3e-6. Adding the
interval back exactly (t0 to the mean, t0^2 to the second moment) leaves the truncated
trapezoid's own O(h^2) endpoint error: 1.9e-6, 9.3e-5 and 1.3e-5 on the mean at SCV 1e-4, 0.01
and 1, and 3.3e-2 on the variance at SCV 1e-4. The geometric series instead continues the
trapezoid rule itself past the grid's first point, so no endpoint is left. With it, at
sigma / 2:

| sojourn SCV | grid points | mean, relative error | variance, relative error |
|---|---|---|---|
| 1e-4 | 34 | 3e-16 | 1.2e-11 |
| 0.01 to 0.1 | 34-35 | below 5e-16 | below 7e-15 |
| 1 | 37 | 4e-16 | 2e-16 |
| 10 | 40 | 2e-16 | 7e-16 |
| 20 | 40 | 3e-16 | 1e-16 |
| 50 | 41 | 1.1e-15 | 2.4e-15 |
| 100 | 42 | 4e-16 | 0 |

(one lognormal integrated back to its own moments by `lognormal_order_statistic` itself; a
probe test, not kept). The documented bound is **1e-8 relative for a sojourn SCV from 1e-4
to 100**, which `one lognormal replica's order statistic returns its own moments`
(`tests/unit/test_queueing.cpp`) checks. The one error above rounding, the variance at SCV
1e-4, is section 5's lesson again: E[X^2] - E[X]^2 subtracts two numbers that agree to four
digits when the variance is 1e-4 of the squared mean.

**Where the digits went before.** An earlier version of the same
table read 8.5e-9, 1.1e-7, 1.7e-6 and 9e-6 on the variance at SCV 10, 20, 50 and 100 (and
8e-15 to 2e-11 on the mean from SCV 1 up), and the bound stopped at SCV 10. The grid was not to
blame: the trapezoid rule was already exact to rounding. The dynamic programme took each
replica's chance of not having answered as 1 - Phi(z), and right of the mean, where Phi(z) is
close to 1, that subtraction is section 2's cancellation: Phi(z) is stored to about 1.1e-16, so
at 1 - Phi(z) = 1e-10 six of the result's digits are rounding, and past 1e-16 all of them. The
second moment weights S by 2 t^2, which is largest exactly there, and the wider the lognormal
(the larger the SCV) the further right that weight reaches. The fix is the one section 2
teaches: compute the small number directly. `normal_cdf(-z)` is Phi(-z) = 1 - Phi(z) to the
last digit (it is `erfc` of a positive argument), so `integrate_quorum` hands each `TrialGroup`
both its `success` Phi(z) and its `failure` Phi(-z), and `fewer_than` multiplies by `failure`
instead of subtracting. A group built with a success only (a machine's kept share, where 1 -
kept is exact enough) gets 1 - success by default. A fluid sojourn's SCV is around 1
(exponential service plus an Erlang wait); it reaches 10 only for very bursty arrivals, so the
shipped answers hardly moved: the kv-store lab's `run` output is byte-identical, its `estimate`
moved by 9e-16 relative, and its variants of the README (`vnodes: 1`, the W/R swaps) by at most
5.6e-13.

Two limits are deliberate: a sojourn with no spread at all is given sigma = 0.001
(`min_sigma`), since a step in S(t) cannot be resolved on any even grid; and a grid is at most
4000 points, a very narrow group far from a wide one widening the step instead. Then the bound
does not hold: a group clamped to sigma = 0.001 beside one of SCV 10 needs more than 4000 points
at sigma / 2, and the widened step leaves 8.4e-4 of error on the mean and 1.4e-3 on the variance
(found in code review; the trapezoid rule's error on a coarse step, which carrying the
failure leaves as it was: 8.4e-4 on the mean re-measured after it). Such a pair would be a
machine of deterministic service beside a very bursty one in the same quorum, which no shipped
scenario has.

**The cross-check.** Fed N exponentials of one mean, the same quadrature must reproduce
Rényi's closed form (`exponential_order_statistic`: 1/3, 5/6, 11/6 of the mean for
the 1st, 2nd and 3rd of three): `the survival-function quadrature reproduces Renyi for
exponential replicas` checks it to 1e-9 at a step of 0.25 in ln t (the
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
M. `ring_imbalance` (`src/engine/graph.cpp`) takes

    k = min(1 + z_M / sqrt(N v), M / N)

with z_M the expected maximum of M standard normals, by Blom's approximation (1958)

    z_M = Phi^-1((M - 0.375) / (M + 0.25))

(`expected_normal_maximum` in `src/queueing/queueing.cppm`; Phi^-1 is `normal_quantile`:
Acklam's rational approximation, relative error 1.15e-9, polished by one Halley
step to full double precision). M / N is the cap: a machine holds a key at most once.

It is a heuristic (shares are neither normal nor independent: they sum to N), checked against
Monte Carlo rings (random virtual-node positions, each key on the next N distinct machines,
the hottest machine's load over the average, averaged over 20-400 rings):

| machines M | copies N | vnodes v | Monte Carlo | k | old 1 + 1/sqrt(v) |
|---|---|---|---|---|---|
| 4 | 1 | 150 | 1.086 | 1.086 | 1.082 |
| 8 | 3 | 150 | 1.073 | 1.068 | 1.082 |
| 16 | 3 | 150 | 1.084-1.091 | 1.083 | 1.082 |
| 128 | 3 | 150 | 1.133 | 1.122 | 1.082 |
| 2 | 1 | 1 | 1.497 (exact 1.5) | 1.589 | 2 |
| 4 | 1 | 1 | 2.075 (exact H_4 = 2.083) | 2.049 | 2 |
| 4 | 2 | 1 | 1.496 | 1.742 | 2 |
| 8 | 3 | 1 | 1.616 | 1.828 | 2 |
| 16 | 3 | 1 | 1.962 | 2.021 | 2 |
| 128 | 3 | 1 | 2.943 | 2.492 | 2 |

(code reviews measured the N = 3 rows and the v = 1 rows; our own runs gave the others and
1.084 at 16. An earlier version of this table gave 1.945 for 16/3/1, 21 standard errors below
the 1.962 a larger run found.) With one copy per
key and one virtual node each, the M points cut the ring into M arcs, each a machine's, and
the expected longest of M arcs is H_M / M of the ring (H_M the harmonic number): k = H_M
exactly, 1.5 for two machines and 2.083 for four.

**Its limit.** Within about 1 % from v = 100 on, where a machine's share is a sum of hundreds
of arcs and close to normal; at v = 1 it is a few whole arcs, far from normal, and k is off by
up to ±16 % (high for few machines, 1.828 against 1.616 at 8/3/1; low for many, 2.492 against
2.943 at 128/3/1). One formula is kept for every v anyway:
v = 1 is an experiment that shows what an uneven ring does, and the labs' verdicts at v = 1
hold with the exact value too (the consistent-hashing lab's `replicas: 2, vnodes: 1` item
says so with the numbers). The old factor ignored M: it is "one standard deviation above the
average", right for a few machines and increasingly low beyond about 16. Both rings use the
same function, so the consistent-hashing lab's balancer and the kv-store lab's cluster agree.

## 9. Try it

1. **Weaken the Che bisection.** In `characteristic_time` (`src/caching/caching.cppm`) change
   `step < 80` to 20, rebuild, and run `[caching]`. The bracket is
   then ~1400 / 2^20 = 1.3e-3 wide in log t, and the pinned checks in
   `tests/unit/test_caching.cpp` fail:
   `the characteristic time of a uniform cache solves N(1 - e^(-t/N)) = C` (1e-9) and
   `the characteristic time matches the dense reference` (1e-4). Try 40, 55 and 64 to see
   where each tolerance starts to hold.
2. **Undo the `expm1`.** In `one_minus_exp_neg` (same file) return `1.0 - std::exp(-x)`
   and see whether any `[caching]` test notices. With p t around 1e-12 the error is 2e-5
   relative (section 2); a cold key over a short characteristic time is exactly that case.
3. **Coarsen the step.** Copy `scenarios/labs/unique-id/scenario.yaml` (step 100ms; its
   ticket server has capacity 8k/s and a queue limit of 1000), change the spike to
   `{at: 5m, for: 1s, factor: 3}` and compare `simsys run` at `step: 100ms` and at
   `step: 10s`. The demand is frozen across a step, so a 1 s spike is either held for a whole
   step or missed. Here the step that starts at 5:00 samples it, and offers 30k/s for ten
   seconds. The ticket server is saturated either way (the base 10k/s already exceeds its
   8k/s), so the latencies cannot show the difference: in both runs every delivered id times
   out at the clients' 50 ms (`>50ms`). The volumes show it. The offered rate averages
   10.04k/s at 100 ms and 10.35k/s at 10 s, and the ticket server's `dropped` rises from
   1.16M to 1.34M: the 9 extra seconds of a 20k/s excess, 180k. Conservation is not what
   breaks: in `flow is conserved to rounding under adversarial demand, dt and queue limits`
   (`tests/unit/test_fluid.cpp`) widen `dts` to include `10s` and the
   residual check still passes (section 6). What coarse steps lose is fidelity, not balance.
4. **Find the step where the burst disappears.** Run the rate-limiter lab at `step: 5ms`,
   `100ms`, `500ms` and `1s` (the commands are in "Choosing the step", section 6) and watch
   the api row's `peak_backlog` (98, 80, 0, 0) and the p99 (24.1 ms, 24.8 ms, 30.5 ms at the
   step that makes each burst exactly the api's capacity, then 19.9 ms). Then set the
   limiter's `burst: 1000`, which moves the ceiling to
   1000 / 15000 = 67 ms. The burst now overfills the api's queue at every step, so the step
   hardly matters: the api's `dropped` is 1.385k at 5 ms, 1.36k at 100 ms and 1.2k at
   500 ms. Why? (The api's waiting room, `queue_limit`, caps the backlog the same way at
   every step.)
5. **Coarsen the quadrature.** Set `steps_per_sigma` (`src/queueing/queueing.cppm`) to
   1.0, rebuild and run `[queueing]`: three checks fail, the fastest of three lognormals by
   4e-5 relative (0.42098 against 0.42100 ms). At 0.5 four fail and the fastest is 3.3 % off
   (0.4071 ms): two points per standard deviation is where the rule's e^(-c / h) error is
   still far below anything a report prints.
6. **Replace the closed-form tail.** In `moments_from_survival` start `first` and
   `second` at t0 and t0^2 (`first_t` and `first_t * first_t`: the exact integrals of S = 1 and
   2 t S over [0, t0]) and give the grid's first point weight h / 2: the rule becomes the
   ordinary trapezoid on the grid plus the exact interval below it. Run `[queueing]` and watch
   `one lognormal replica's order statistic returns its own moments` fail: the mean is now off
   by 1.9e-6, 9.3e-5 and 1.3e-5 at SCV 1e-4, 0.01 and 1 (the truncated trapezoid's O(h^2)
   endpoint error), and the variance by 3.3e-2 at SCV 1e-4. Then start them at 0 as well, the
   left tail dropped altogether: the mean loses t0, 92 % of it at SCV 1e-4 (section 7).
7. **Watch the quorum hide the hot machine.** In `scenarios/labs/kv-store/scenario.yaml` set
   `vnodes: 1`: the hot machine is at 114 % and drops, yet the put's p99 is 2.65 ms (PASS),
   because a quorum of two answers from the two cool replicas. Then set `write_quorum: 3`:
   a put now waits for every replica, the hot one's full queue included, and loses what it
   drops: 8.59k of the 9k puts/s come back. The sets that hold the hot machine are 0.379 of
   the puts offered but only 0.349 of those that come back, and the latency is theirs: 36.7 ms
   on average, p99 227 ms, before the clients' `timeout: 100ms`. With it, 6.62 % of them come
   back too late (569/s), 8.02k/s are answered in 27.2 ms, and the p99 SLO fails at the
   timeout (the README's "Things to try" has the derivation).

## 10. A client's timeout: the truncated lognormal

A client with `timeout: τ` (spec §19 entry 12) gives up on a request the path has not answered by τ.
The engine does not follow requests one by one, so each step it takes the client's response for
an op as the lognormal of section 5, fitted to the path's mean m and variance v
(`Simulation::give_up_after`, `src/engine/engine.cppm`, calling `queueing::lognormal_below`,
`src/queueing/queueing.cppm`), and cuts it at τ. Four formulas, all from the fit
σ² = ln(1 + v/m²), μ = ln m − σ²/2 and the timeout's place on the normal scale of ln X,
z = (ln τ − μ) / σ:

    P(X < τ)            = Φ(z)                                    the share answered in time
    E[X^k 1{X < τ}]     = E[X^k] Φ(z − kσ)          (k = 1, 2)    the k-th "partial moment"
    E[X | X < τ]        = m Φ(z − σ) / Φ(z)                       the answered requests' mean
    E[X² | X < τ]       = (m² + v) Φ(z − 2σ) / Φ(z)               ... and second moment

Φ is the standard normal CDF, `normal_cdf`, computed as `erfc(−z/√2) / 2`: one call that keeps
its digits in both tails (`1 − erf` would lose every digit below Φ = 1e-16, section 2's
cancellation again). The partial moments come from completing the square: X^k = e^{k ln X}
shifts the normal density of ln X by kσ², so the share of the k-th moment below τ is the normal
CDF at z − kσ. The answered requests' moments are those of a **truncated** distribution, the
original with everything past τ removed and the rest scaled back to 1 (divided by Φ(z)): the
op's **mean** is theirs, the time an answer took on average.

**Which fit the percentiles are read off.** Not the truncated one. The engine decides who times
out on the untruncated fit: a request is timed out exactly when that lognormal puts it past τ. So
of the requests the path delivered, the share s = 1 − Φ(z) that timed out are the fit's slowest s,
and for q ≤ 1 − s the fastest q of the delivered requests are all answered ones: the client's
q-quantile **is** the untruncated fit's q-quantile, which the timeout does not move. The engine
keeps those moments beside the truncated ones (`ResponseSample::delivered_response`), the report
sums them over the run weighted by the requests delivered, answered plus timed out
(`Summary::OpTotals::by_delivered`), and reads the percentile there at q itself, capped at τ
(`percentile_seen_by_client`, `src/report/report.cpp`; the cap only matters for a run that mixes
steps, since one steady step has s ≤ 1 − q exactly when the fit's q-quantile is at most τ). When
s > 1 − q no request at q was answered and the percentile is the timeout itself, `>τ`. So a
percentile never falls as τ grows, meets τ from below as s rises to 1 − q, and is the
untimed client's once the cut is empty. The path below behind a 530 ms timeout (0.78 % timed out)
has the p99 it has without one, 490.5 ms (tests/theory/test_timeout.cpp derives it, and checks
that the p99 does not fall over ten timeouts from 300 ms to 3 s).

**Why not refit the answered requests.** An earlier version of the reports fitted a
second lognormal to the truncated moments and read it at q / (1 − s). That refit has a tail past
τ the truncated distribution lacks, and its moments react to a cut far more than the requests do:
the share of the k-th moment past τ is Φ(kσ − z), which grows with k. Behind a 1 s timeout the path
below times out 0.073 % of its requests, yet 0.94 % of its first moment and 6.5 % of its second
lie past 1 s; cutting them takes 11 % of the variance (0.01 → 0.00889 s²), and the refit's p99 was
475.5 ms where the requests' own p99 had not moved from 490 ms. Behind 600 ms it was 508.9 ms, so
the old p99 rose and fell as the timeout grew. The rate-limiter lab showed it in a whole run: a
cut of 8.9e-7 of its calls moved its p99 from 30.5 to 29.9 ms (before the stochastic wait read
min(demand, drain), section 6). Reading the delivered fit removes both: a tiny cut moves no
percentile. Today the lab cuts 3.5e-7 of its calls and prints a p99 of 24.8 ms with its
`timeout: 1s` and without it.

**Checked against the binary.** A client at 10/s sending `get` to a service of 100 ms service
time and 1k/s (c = 100, so no wait: the sojourn is exponential, m = 100 ms, v = 0.01 s², SCV 1)
over a link of `latency: 0s`, with `timeout: 200ms` (σ = 0.8326, μ = −2.6492, z = 1.2488):

    client.op  offered/s  attempts/s  timed out/s  mean     p50     p95     p99  (estimated)
    users.get         10          10        1.059  74ms  70.7ms  >200ms  >200ms

89.41 % answered (JSON `answered_per_s` 8.941367), 10.59 % timed out, and the answered mean
73.97 ms (`mean_s` 0.0739704), as the formulas give. At `timeout: 1s` 99.927 % are answered, at
`50ms` 33.86 %. The p50 is the fit's median, exp(μ) = 0.1 / √2 = 70.7 ms, what the path shows
without a timeout. The p95 and the p99 print `>200ms`: more than 5 % of the delivered requests
timed out, so no request at those quantiles was answered, and a latency SLO on them fails whatever
its `max`. At `timeout: 1s` the row reads `99.1ms  70.7ms  278ms  490ms`: the untimed path's
percentiles.

**The underflow guard.** When a response hardly varies and sits well above τ, z is far below
zero and Φ(z) underflows to exactly 0 (below about z = −38.5): youtube's 300 MB uploads take 12 s
at a blob store's 25 MB/s per slot with almost no spread (a fixed-size transfer is deterministic),
and against a 10 s timeout the truncated moments would be 0 / 0. A NaN there would spread through
every weighted sum of the report (0 × NaN is NaN). `lognormal_below` returns the limit instead,
`{fraction 0, mean τ, variance 0}` (what is left as the answered share vanishes piles up just
under τ), the engine counts every such request as timed out, and the report never shows those
stand-in moments: JSON prints `null` latencies for an op of which nothing was answered in time and
the table `>10s`. Try it: `sed 's/timeout: 30s/timeout: 10s/' scenarios/book/youtube/scenario.yaml
> /tmp/yt10.yaml` and `simsys run /tmp/yt10.yaml --duration 65m` (the shortest run that still
holds the utilization SLO's `over: 1h` after the 5-minute warmup): `viewers_us.upload  5.415
5.415  -  >10s  >10s  >10s`, every upload timed out. (The shipped 30 s timeout stands for the
book's resumable uploads, which retry a chunk, not the whole file.) The other edges are limits of
the same formulas: τ = ∞ returns m and v, an infinite m (a path that never answers) stays
infinite so the report still calls it unavailable, and v = 0 is a step at m (all answered if
m ≤ τ). The mean is clamped to at most τ and the variance to at least 0, since a narrow truncated
part loses digits in E[X²] − E[X]² (section 5's cancellation).

**What it ignores.** The response is not exactly lognormal (a cache's hit/miss mixture is
bimodal, section 5), so the share timed out is the fit's tail, not the requests'; the timeout is
the client's alone (nothing cancels the request at the servers, whose demand and utilization
still include it, and nothing retries it before phase 3); and a timed-out request is in no
`drop_rate`, since no node failed it.

(Numbers from a Release build of this tree; every run is deterministic.)
