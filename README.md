# system-sim

A fluid-flow **system design simulator** in modern C++23.

Describe a distributed system in YAML — clients, load balancers, caches, SQL and NoSQL
stores, message queues, CDNs, datacenters and the links between them — then push a day of
traffic through it (waves, spikes, noise, failures) and watch where it turns yellow and red.
Traffic is modelled as rates with analytic queueing per node, so every run is instant and
reproducible. Built to practise the systems in
*System Design Interview* (Alex Xu, vol. 1) and, at the same time, to study and teach
C++20/23: modules, concepts, ranges, coroutines, `std::expected` and friends.

Status: phase 0 complete — the project builds with GCC 14 named modules, `simsys validate`
checks scenario files, and drafts of all 8 book systems and 4 labs live under
[`scenarios/`](scenarios/). The engine arrives in phase 1.
See [`docs/superpowers/specs/2026-10-27-system-sim-design.md`](docs/superpowers/specs/2026-10-27-system-sim-design.md)
for the design and [`docs/approaches.md`](docs/approaches.md) for the modelling approaches considered.

## Planned CLI

```
simsys validate scenario.yaml
simsys explain  scenario.yaml
simsys estimate scenario.yaml
simsys run      scenario.yaml --seed 7 --report table --series out.csv
simsys sweep    scenario.yaml --param nodes.users.rate.base=1k/s..100k/s:10 --metric users.p99
```

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
