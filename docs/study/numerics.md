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
- `lognormal_quantile`, `src/queueing/queueing.cppm:126`: sigma^2 = ln(1 + v/m^2), where
  v/m^2 is small for a nearly deterministic latency.

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

## 7. Try it

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
