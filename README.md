# system-sim

A fluid-flow **system design simulator** in modern C++23.

Describe a distributed system in YAML — clients, load balancers, caches, SQL and NoSQL
stores, message queues, CDNs, datacenters and the links between them — then push a day of
traffic through it (waves, spikes, noise, failures) and watch where it turns yellow and red.
Traffic is modelled as rates with analytic queueing per node, so every run is instant and
reproducible. Built to practise the systems in
*System Design Interview* (Alex Xu, vol. 1) and, at the same time, to study and teach
C++20/23: modules, concepts, ranges, coroutines, `std::expected` and friends.

Status: design phase. See [`docs/superpowers/specs/2026-10-27-system-sim-design.md`](docs/superpowers/specs/2026-10-27-system-sim-design.md)
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

GCC 14 (primary) or Clang 18, CMake ≥ 3.28, Ninja. C++23 with named modules.

## Licence

MIT — see [LICENSE](LICENSE).
