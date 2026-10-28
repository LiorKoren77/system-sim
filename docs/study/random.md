# `<random>`: engines, distributions and reproducible noise

`<random>` is C++11, not C++20; it is here because the workload noise of phase 2a is the
first place the simulator draws random numbers, and reproducibility is a contract the
whole project leans on ("same scenario + same seed = the same run, byte for byte").

## Used in

| Construct | File | Function or member |
|---|---|---|
| `std::mt19937_64` engine, seeded with one `std::uint64_t` | `src/workload/workload.cppm` | `NoiseProcess::engine_`, `NoiseProcess::NoiseProcess` |
| `std::normal_distribution<double>{0.0, 1.0}` | `src/workload/workload.cppm` | `NoiseProcess::normal_`, drawn in `NoiseProcess::next` |
| the per-client seed | `src/engine/behaviours.cppm` | `behaviours::client_seed(seed, index)` |
| one process per client, ticked once per step | `src/engine/behaviours.cppm`, `src/engine/engine.cppm` | `ClientBehaviour::tick`, `ClientBehaviour::reset_noise`, `Simulation::make_node` |
| the same path printed | `src/cli/main.cpp`, `src/workload/workload.cppm` | `curve_samples` (`--noise`, `--seed`), the noisy `samples` overload |

## What

`<random>` splits "random numbers" into two jobs:

- an **engine** produces uniformly distributed raw bits from its state
  (`std::mt19937_64`: the 64-bit Mersenne Twister, 312 words of state, period 2^19937 − 1);
- a **distribution** turns engine output into values of a given law
  (`std::normal_distribution<double>{mean, stddev}`), calling the engine as often as it needs.

```cpp
std::mt19937_64 engine_;                              // seeded in the constructor
std::normal_distribution<double> normal_{0.0, 1.0};   // xi ~ N(0, 1)
...
x_ = x_ * decay + spec_.sigma * std::sqrt(1.0 - decay * decay) * normal_(engine_);
```

The standard specifies the engines exactly (the 10000th output of a default-constructed
`mt19937_64` is fixed by the standard: 9981545732273789042), so a seed gives the same raw
sequence everywhere. The **distributions' algorithms are not specified**: libstdc++,
libc++ and MSVC turn the same raw bits into different normal values.

## The pre-C++11 way

`std::srand(seed)` and `std::rand()`: one hidden global state (so two clients could not
have independent streams, and adding a client would shift every other client's numbers),
`RAND_MAX` as small as 32767, an implementation-defined algorithm, and the modulo bias of
`rand() % n`. A normal variate meant writing Box–Muller by hand.

## The noise: Ornstein–Uhlenbeck, discretised exactly

Each client multiplies its rate by `max(0, 1 + x)`, where `x` is an Ornstein–Uhlenbeck
process: Gaussian noise that reverts to 0 with time constant `τ` (`noise.correlation`) and
has stationary standard deviation `σ` (`noise.sigma`). `NoiseProcess::next(dt)` steps it
**exactly** (Gillespie 1996), for any `dt`:

    x_{k+1} = x_k · e^{−dt/τ} + σ · sqrt(1 − e^{−2dt/τ}) · ξ,     ξ ~ N(0, 1)

The first term is how much of the old deviation survives the step; the second is fresh
randomness, scaled so the variance stays `σ²` (`σ²e^{−2dt/τ} + σ²(1 − e^{−2dt/τ}) = σ²`).
The usual Euler–Maruyama step `x + (−x/τ)dt + σ·sqrt(2dt/τ)·ξ` is only right for
`dt ≪ τ`, and overshoots when a 100 ms step meets a short `τ`. The floor at 0 keeps the
rate non-negative; for large `σ` it lifts the mean multiplier slightly above 1.

## Seeding and the determinism contract

- **One engine per client.** `client_seed(seed, i) = (seed × 0x9E3779B97F4A7C15) XOR i`
  (mod 2^64), with `i` the client's node index. A plain `seed XOR i` collides
  (`1 ^ 0 == 0 ^ 1`); multiplying by the odd constant (2^64 / golden ratio, splitmix64's
  increment) moves each seed's block of indices into the high bits.
  `tests/unit/test_behaviours.cpp` checks that seeds −100..100 × 64 clients never share
  an engine seed.
- **Ticks, not calls.** The engine ticks every client exactly once per step, before it
  offers; `estimate` never ticks (it sees the curve without noise); a `sigma: 0` client
  returns 1 without drawing, so it is identical to a client without noise.
- **`simsys curve --noise`** builds the same `NoiseProcess` with the same seed and ticks it
  at the scenario's step, so the printed path is the path `run` uses.
- **The contract**: the same scenario, seed and standard library give byte-identical runs
  (`tests/theory/test_theory.cpp`: "two runs with the same seed are identical and a
  different seed is not"). Across standard libraries the engine agrees and the normal
  values do not; GCC 14 with libstdc++ is the only supported toolchain.

## Pitfalls met while building

- **A distribution has state.** libstdc++'s `normal_distribution` uses Marsaglia's polar
  method (a rejection form of Box–Muller): one accepted pair of uniforms yields **two**
  normals; it returns one and keeps the other in a private `_M_saved` for the next call.
  So the distribution belongs to its engine: `NoiseProcess` keeps both as members, and
  `ClientBehaviour::reset_noise` re-creates the whole process (`emplace`) rather than
  re-seeding the engine alone, which would leave a stale cached value.
- **`std::default_random_engine` is implementation-defined**; so is every distribution.
  Name the engine (`mt19937_64`), and accept that the distribution ties results to the
  standard library.
- **`std::random_device` is the opposite of reproducible.** It is the right seed for a
  game, the wrong one for a simulator whose runs must be compared.
- **A negative `seed`** converts to `std::uint64_t` modulo 2^64. That conversion has always
  been well defined in C++ (signed to unsigned is modular); C++20's two's-complement rule
  fixed the other direction, unsigned to signed.
- **Seeding with one integer** uses the standard's own initialisation recurrence, which is
  fine for distinct seeds; `std::seed_seq` spreads more entropy over the 312 words and is
  the choice when seeds come from many sources.

## Further reading

- cppreference: [Pseudo-random number generation](https://en.cppreference.com/w/cpp/numeric/random),
  [`std::mersenne_twister_engine`](https://en.cppreference.com/w/cpp/numeric/random/mersenne_twister_engine),
  [`std::normal_distribution`](https://en.cppreference.com/w/cpp/numeric/random/normal_distribution).
- N3551, W. E. Brown, *Random Number Generation in C++11*.
- Stephan T. Lavavej, *rand() Considered Harmful* (GoingNative 2013).
- D. T. Gillespie, *Exact numerical simulation of the Ornstein-Uhlenbeck process and its
  integral*, Phys. Rev. E 54, 2084 (1996).
- G. Marsaglia and T. A. Bray, *A convenient method for generating normal variables*,
  SIAM Review 6 (1964).
