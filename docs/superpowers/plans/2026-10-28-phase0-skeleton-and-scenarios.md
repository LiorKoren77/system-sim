# Phase 0: Toolchain Skeleton and Scenario Drafts — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A C++23 project that builds with GCC 14 named modules, has unit tests and CI, ships a `simsys validate` command, and contains hand-drafted YAML for all 8 book systems and 4 labs — each machine-validated — so the scenario schema is proven before the engine is written.

**Architecture:** Three named modules (`simsys.core` for strong units, `simsys.config` for YAML loading and reference validation) plus a thin CLI executable. Dependencies come through CPM.cmake pinned to exact tags. Scenario files are test fixtures: a Catch2 test loads every YAML under `scenarios/` and requires it to validate.

**Tech Stack:** C++23, GCC 14.2, CMake 3.28, Ninja, CPM.cmake 0.40.2, yaml-cpp 0.8.0, CLI11 2.4.2, Catch2 3.7.1, clang-format-18 (formatting only; Clang 18 cannot compile the project — it lacks `std::expected`/`std::generator` with both libstdc++ and libc++).

**Conventions (from `CLAUDE.md`):** the first use of each C++20/23 construct in a file gets a 2–5 line explanatory comment; every book model gets explanatory documentation. The code blocks below already include those comments — keep them.

---

## File structure

| Path | Responsibility |
|---|---|
| `CMakeLists.txt` | Project, C++23, module scanning, warnings, subdirectories |
| `CMakePresets.json` | `gcc14-debug`, `gcc14-release` configure/build/test presets |
| `cmake/CPM.cmake` | Vendored CPM (pinned) |
| `cmake/Dependencies.cmake` | `CPMAddPackage` calls for yaml-cpp, CLI11, Catch2 |
| `.clang-format` | Formatting rules |
| `src/core/core.cppm` | Module `simsys.core`: `SimTime`, `Rate`, `ParseError`, `parse_duration`, `parse_rate`, `version` |
| `src/config/config.cppm` | Module `simsys.config`: `Scenario`, `ConfigError`, `load()` with reference validation |
| `src/cli/main.cpp` | `simsys` executable: `validate <file>`, `--version` |
| `tests/unit/test_units.cpp` | Unit parsing tests |
| `tests/unit/test_config.cpp` | Loader/validator tests |
| `tests/scenario/test_scenarios.cpp` | Every shipped scenario validates |
| `.github/workflows/ci.yml` | Build + test on GCC 14, clang-format check |
| `docs/scenario-schema.md` | The YAML schema, v1 |
| `docs/study/modules.md`, `docs/study/expected.md` | First two study notes |
| `scenarios/book/<system>/scenario.yaml` + `README.md` | 8 book systems |
| `scenarios/labs/<lab>/scenario.yaml` + `README.md` | 4 labs |

---

### Task 1: Build skeleton with a named module

**Files:**
- Create: `CMakeLists.txt`, `CMakePresets.json`, `cmake/CPM.cmake`, `cmake/Dependencies.cmake`, `.clang-format`, `src/core/core.cppm`, `src/cli/main.cpp`

- [ ] **Step 1: Vendor CPM.cmake (pinned)**

```bash
cd /home/frtlx/liorkoren77/system-sim
mkdir -p cmake src/core src/config src/cli tests/unit tests/scenario
curl -fsSL https://github.com/cpm-cmake/CPM.cmake/releases/download/v0.40.2/CPM.cmake -o cmake/CPM.cmake
head -3 cmake/CPM.cmake
```
Expected: the file starts with `# CPM.cmake - CMake's missing package manager`.

- [ ] **Step 2: Write `cmake/Dependencies.cmake`**

```cmake
# All third-party code comes through CPM with exact tags, so a checkout builds the
# same bits everywhere. CPM downloads at configure time into build/_deps.
include(${CMAKE_CURRENT_LIST_DIR}/CPM.cmake)

CPMAddPackage(
  NAME yaml-cpp
  GITHUB_REPOSITORY jbeder/yaml-cpp
  GIT_TAG 0.8.0
  OPTIONS "YAML_CPP_BUILD_TESTS OFF" "YAML_CPP_BUILD_TOOLS OFF" "YAML_CPP_BUILD_CONTRIB OFF"
  SYSTEM YES)   # SYSTEM: its headers are treated as system headers, so our -Werror does not fire on them

CPMAddPackage(
  NAME CLI11
  GITHUB_REPOSITORY CLIUtils/CLI11
  GIT_TAG v2.4.2
  SYSTEM YES)

CPMAddPackage(
  NAME Catch2
  GITHUB_REPOSITORY catchorg/Catch2
  GIT_TAG v3.7.1
  SYSTEM YES)
```

- [ ] **Step 3: Write `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.28)   # 3.28 is the first release with stable named-module support
project(system-sim VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # -std=c++23, not gnu++23
set(CMAKE_CXX_SCAN_FOR_MODULES ON)     # let CMake discover `export module` / `import` dependencies
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
  message(FATAL_ERROR "system-sim needs GCC >= 14 (std::expected, std::generator, named modules). "
                      "Clang 18 cannot build it; use it for clang-format/clang-tidy only.")
endif()

include(cmake/Dependencies.cmake)

# One warning set for all of our targets (never applied to third-party code).
add_library(simsys_warnings INTERFACE)
target_compile_options(simsys_warnings INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)

# --- library: one named module per layer ------------------------------------
add_library(simsys)
target_sources(simsys
  PUBLIC FILE_SET CXX_MODULES FILES     # FILE_SET CXX_MODULES is how CMake 3.28 declares module interface units
    src/core/core.cppm
    src/config/config.cppm)
target_link_libraries(simsys PUBLIC yaml-cpp::yaml-cpp PRIVATE simsys_warnings)

# --- executable --------------------------------------------------------------
add_executable(simsys_cli src/cli/main.cpp)
set_target_properties(simsys_cli PROPERTIES OUTPUT_NAME simsys)
target_link_libraries(simsys_cli PRIVATE simsys CLI11::CLI11 simsys_warnings)

# --- tests -------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
```

- [ ] **Step 4: Write `CMakePresets.json`**

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "gcc14-debug",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/gcc14-debug",
      "cacheVariables": { "CMAKE_CXX_COMPILER": "g++-14", "CMAKE_BUILD_TYPE": "Debug" }
    },
    {
      "name": "gcc14-release",
      "inherits": "gcc14-debug",
      "binaryDir": "${sourceDir}/build/gcc14-release",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Release" }
    }
  ],
  "buildPresets": [
    { "name": "gcc14-debug", "configurePreset": "gcc14-debug" },
    { "name": "gcc14-release", "configurePreset": "gcc14-release" }
  ],
  "testPresets": [
    { "name": "gcc14-debug", "configurePreset": "gcc14-debug", "output": { "outputOnFailure": true } },
    { "name": "gcc14-release", "configurePreset": "gcc14-release", "output": { "outputOnFailure": true } }
  ]
}
```

- [ ] **Step 5: Write `.clang-format`**

```yaml
BasedOnStyle: Google
Standard: c++20
ColumnLimit: 100
IndentWidth: 2
DerivePointerAlignment: false
PointerAlignment: Left
AllowShortFunctionsOnASingleLine: Inline
IncludeBlocks: Preserve
```

- [ ] **Step 6: Write `src/core/core.cppm` (version only, units come in Task 2)**

```cpp
// Module `simsys.core`: the vocabulary types every other layer uses.
//
// C++20 named modules. This file is a *module interface unit*. Everything after
// `export module simsys.core;` is the module's purview; only declarations marked
// `export` are visible to importers. Compared with headers: no include guards, no
// macro leakage, one compilation of the interface instead of one per includer.
// The `module;` line opens the *global module fragment*, the only place a module
// unit may #include traditional headers (the standard library is still header-based
// in GCC 14).
module;
#include <string_view>

export module simsys.core;

export namespace simsys {

// C++20 `constexpr` std::string_view construction from a literal is a compile-time
// constant; `inline constexpr` gives one definition across all importers.
inline constexpr std::string_view version = "0.0.1";

}  // namespace simsys
```

- [ ] **Step 7: Write `src/cli/main.cpp` (version flag only; `validate` comes in Task 4)**

```cpp
// `simsys` command-line entry point. Parsing is delegated to CLI11; commands are thin
// wrappers over library calls so they stay testable without a process.
#include <CLI/CLI.hpp>

#include <print>

// C++20 `import`: brings the exported names of a named module into this translation
// unit. Unlike #include, it does not re-parse text; the compiler reads a compiled
// module interface (BMI). Imports go after the #includes to keep GCC 14 happy about
// header state.
import simsys.core;

int main(int argc, char** argv) {
  CLI::App app{"simsys - a fluid-flow system design simulator"};
  app.set_version_flag("--version", std::string{simsys::version});
  app.require_subcommand(0, 1);

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    return app.exit(e);  // prints help / version / error exactly as CLI11 intends
  }

  if (app.get_subcommands().empty()) {
    // C++23 std::println: type-safe formatted output to stdout, replacing printf's
    // format-string/argument mismatch bugs and iostream's verbosity. The format
    // string is checked at compile time.
    std::println("simsys {} - run `simsys --help`", simsys::version);
  }
  return 0;
}
```

- [ ] **Step 8: Write a minimal `tests/CMakeLists.txt` so configure succeeds**

```cmake
# Tests are teaching material: name each TEST_CASE after the property it proves.
# Targets are added in Task 2; an empty source file would be rejected by -Wpedantic
# ("ISO C++ forbids an empty translation unit"), so there is nothing to build yet.
list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
include(Catch)
```

- [ ] **Step 9: Configure, build, run**

```bash
cd /home/frtlx/liorkoren77/system-sim
cmake --preset gcc14-debug 2>&1 | tail -3
cmake --build --preset gcc14-debug 2>&1 | tail -3
./build/gcc14-debug/simsys --version
./build/gcc14-debug/simsys
```
Expected: configure ends with `Build files have been written`, build ends with `Linking CXX executable simsys`, then `0.0.1` and `simsys 0.0.1 - run \`simsys --help\``.

- [ ] **Step 10: Commit**

```bash
git add CMakeLists.txt CMakePresets.json cmake .clang-format src tests
git commit -m "build: C++23 skeleton with named modules, CPM-pinned deps, GCC 14 presets

GCC 14 is the only compiler that provides std::expected and std::generator
together with named modules today; Clang 18 is kept for formatting only."
```

---

### Task 2: Strong units — `SimTime`, `Rate`, parsers (TDD)

**Files:**
- Modify: `src/core/core.cppm`
- Create: `tests/unit/test_units.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing tests**

`tests/unit/test_units.cpp`:
```cpp
// Properties of the unit parsers that scenario files rely on.
#include <catch2/catch_test_macros.hpp>

#include <chrono>

import simsys.core;

using namespace std::chrono_literals;

TEST_CASE("parse_duration understands every unit used in scenario files") {
  REQUIRE(simsys::parse_duration("500ns").value() == 500ns);
  REQUIRE(simsys::parse_duration("150us").value() == 150us);
  REQUIRE(simsys::parse_duration("0.5ms").value() == 500us);
  REQUIRE(simsys::parse_duration("2s").value() == 2s);
  REQUIRE(simsys::parse_duration("5m").value() == 5min);
  REQUIRE(simsys::parse_duration("24h").value() == 24h);
}

TEST_CASE("parse_duration rejects unknown units and non-numbers with a structured error") {
  auto bad_unit = simsys::parse_duration("5 parsecs");
  REQUIRE_FALSE(bad_unit.has_value());
  REQUIRE(bad_unit.error().message == "unknown duration unit ' parsecs'");

  auto not_a_number = simsys::parse_duration("ms");
  REQUIRE_FALSE(not_a_number.has_value());
  REQUIRE(not_a_number.error().message == "expected a number");
}

TEST_CASE("parse_rate understands plain, k, M and G suffixes with a /s denominator") {
  REQUIRE(simsys::parse_rate("300/s").value().per_second == 300.0);
  REQUIRE(simsys::parse_rate("10k/s").value().per_second == 10'000.0);
  REQUIRE(simsys::parse_rate("1.5M/s").value().per_second == 1'500'000.0);
  REQUIRE(simsys::parse_rate("2G/s").value().per_second == 2'000'000'000.0);
}

TEST_CASE("parse_rate accepts byte rates; a Rate carries no unit, the node type decides") {
  REQUIRE(simsys::parse_rate("500MB/s").value().per_second == 500e6);
  REQUIRE(simsys::parse_rate("1GB/s").value().per_second == 1e9);
  REQUIRE(simsys::parse_rate("200B/s").value().per_second == 200.0);
}

TEST_CASE("parse_rate requires the /s denominator") {
  auto r = simsys::parse_rate("10k");
  REQUIRE_FALSE(r.has_value());
  REQUIRE(r.error().message == "rate must end with '/s'");
}
```

Append to `tests/CMakeLists.txt`:
```cmake
add_executable(unit_tests unit/test_units.cpp)
target_link_libraries(unit_tests PRIVATE simsys Catch2::Catch2WithMain simsys_warnings)
catch_discover_tests(unit_tests)
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
cmake --build --preset gcc14-debug 2>&1 | grep -E "error" | head -3
```
Expected: compile errors such as `'parse_duration' is not a member of 'simsys'`.

- [ ] **Step 3: Implement the units in `src/core/core.cppm`**

Replace the whole file with:
```cpp
// Module `simsys.core`: the vocabulary types every other layer uses — simulated time,
// rates, and the parsers that turn scenario strings like "0.5ms" or "10k/s" into them.
//
// C++20 named modules. This file is a *module interface unit*. Everything after
// `export module simsys.core;` is the module's purview; only declarations marked
// `export` are visible to importers. Compared with headers: no include guards, no
// macro leakage, one compilation of the interface instead of one per includer.
// The `module;` line opens the *global module fragment*, the only place a module
// unit may #include traditional headers (the standard library is still header-based
// in GCC 14).
module;
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

export module simsys.core;

export namespace simsys {

inline constexpr std::string_view version = "0.0.1";

// Simulated time. A std::chrono duration rather than a double so that seconds and
// milliseconds cannot be mixed by accident; integer nanoseconds give exact arithmetic
// (a double would drift after millions of additions).
using SimTime = std::chrono::duration<std::int64_t, std::nano>;

// Requests (or bytes, messages...) per second. A struct, not a bare double, so a rate
// cannot be passed where a latency is expected.
struct Rate {
  double per_second;
  // C++20 defaulted three-way comparison: one line gives ==, !=, <, <=, >, >= that
  // compare members in declaration order. Before C++20 this was six hand-written
  // operators that silently went stale when a member was added.
  auto operator<=>(const Rate&) const = default;
};

struct ParseError {
  std::string input;
  std::string message;
};

namespace detail {

// Splits "12.5ms" into (12.5, "ms"). std::from_chars (C++17) is locale-independent and
// never allocates, which is what a parser called thousands of times wants.
inline std::expected<std::pair<double, std::string_view>, ParseError> split_number(
    std::string_view text) {
  double value{};
  const auto* begin = text.data();
  const auto* end = begin + text.size();
  auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc{} || ptr == begin) {
    // C++23 std::unexpected<E>: wraps an error so it can be returned through a
    // std::expected<T, E>; the caller gets `.error()` instead of an exception.
    return std::unexpected(ParseError{std::string{text}, "expected a number"});
  }
  return std::pair{value, text.substr(static_cast<std::size_t>(ptr - begin))};
}

}  // namespace detail

// C++23 std::expected<T, E>: a value-or-error return type. Replaces the old choice
// between exceptions and out-parameters/error codes. The caller must inspect it, and
// errors carry structured data (here: the offending input and a message) that the
// config loader can decorate with file, line and column.
std::expected<SimTime, ParseError> parse_duration(std::string_view text) {
  auto split = detail::split_number(text);
  if (!split) return std::unexpected(split.error());
  // C++17 structured binding; still the clearest way to unpack a pair.
  auto [value, unit] = *split;

  double nanoseconds{};
  if (unit == "ns") nanoseconds = value;
  else if (unit == "us") nanoseconds = value * 1e3;
  else if (unit == "ms") nanoseconds = value * 1e6;
  else if (unit == "s") nanoseconds = value * 1e9;
  else if (unit == "m") nanoseconds = value * 60e9;
  else if (unit == "h") nanoseconds = value * 3600e9;
  else
    return std::unexpected(
        ParseError{std::string{text}, "unknown duration unit '" + std::string{unit} + "'"});

  return SimTime{std::llround(nanoseconds)};
}

std::expected<Rate, ParseError> parse_rate(std::string_view text) {
  constexpr std::string_view denominator = "/s";
  // C++20 std::string_view::ends_with: no more size arithmetic to check a suffix.
  if (!text.ends_with(denominator)) {
    return std::unexpected(ParseError{std::string{text}, "rate must end with '/s'"});
  }
  auto split = detail::split_number(text.substr(0, text.size() - denominator.size()));
  if (!split) return std::unexpected(split.error());
  auto [value, suffix] = *split;
  // Byte rates ("500MB/s") share the grammar: strip a trailing 'B'. A Rate carries no
  // unit on purpose; the node type decides whether it counts requests or bytes.
  if (suffix.ends_with('B')) suffix.remove_suffix(1);

  double multiplier{};
  if (suffix.empty()) multiplier = 1.0;
  else if (suffix == "k") multiplier = 1e3;
  else if (suffix == "M") multiplier = 1e6;
  else if (suffix == "G") multiplier = 1e9;
  else
    return std::unexpected(
        ParseError{std::string{text}, "unknown rate suffix '" + std::string{suffix} + "'"});

  return Rate{value * multiplier};
}

}  // namespace simsys
```

- [ ] **Step 4: Run tests to verify they pass**

```bash
cmake --build --preset gcc14-debug 2>&1 | tail -1 && ctest --preset gcc14-debug 2>&1 | tail -3
```
Expected: `100% tests passed, 0 tests failed out of 5`.

- [ ] **Step 5: Format and commit**

```bash
clang-format-18 -i src/core/core.cppm src/cli/main.cpp tests/unit/test_units.cpp
git add -A src tests
git commit -m "core: SimTime, Rate and std::expected-based unit parsers

Strong types so a latency can never be passed as a rate; integer nanoseconds so
time arithmetic is exact over millions of steps."
```

---

### Task 3: Scenario schema document (draft v1)

**Files:**
- Create: `docs/scenario-schema.md`

- [ ] **Step 1: Write the schema**

```markdown
# Scenario schema v1 (draft, phase 0)

A scenario is one YAML document. Unknown keys are errors. Units: durations `ns us ms s m h`,
rates `N/s` with optional `k M G`, sizes `B KB MB GB`, clock times `HH:MM` (for `at:`/`peak_at:`).

## Top level

| key | type | required | meaning |
|---|---|---|---|
| `version` | int | yes | always `1` |
| `name` | string | yes | shown in reports |
| `seed` | int | no | default 1 |
| `duration` | duration | yes | simulated span |
| `step` | duration | no | default `100ms` |
| `warmup` | duration | no | discarded from reports, default 0 |
| `datacenters` | map | no | see below; default: one DC `main` |
| `nodes` | map id → node | yes | |
| `links` | list | yes | |
| `slo` | list | no | |
| `faults` | list | no | |

## `datacenters`

Map of DC name → `{}` (reserved for future keys) plus an optional `latency` list of
`{between: [a, b], rtt: duration}` overriding the `cross_dc_rtt` default (150 ms).
Intra-DC links default to `intra_dc_rtt` (0.5 ms).

## Node (common keys)

| key | type | meaning |
|---|---|---|
| `type` | enum | one of the catalogue below |
| `dc` | string | datacenter, default `main` |
| `replicas` | int | N identical copies behind an equal split (default 1) |
| `capacity` | rate | service rate per replica; default per type from the back-of-the-envelope table |
| `latency` | duration | service time per op; default per type |
| `ops` | map op → list of step | what the node does with each incoming op (`service`, `client` use it) |
| `then` | list of step | steps after processing (used by `worker_pool`, `notification_fanout`) |
| `advanced` | map | `queue_limit` (int), `concurrency` (int), `yellow` (0–1), `red` (0–1), `service_variability` (SCV), `drop_policy` |

A **step** is `{to: <node id>, op: <op name>, on: always|miss|failure, mode: sequential|parallel, factor: <number>}`.
`factor` multiplies the flow (fan-out: one post → 100 feed writes). `on: miss` applies to the
flow a cache/CDN did not serve. `mode: parallel` steps add the max of their latencies, not the sum.

## Node types

| type | own keys |
|---|---|
| `client` | `rate` (rate curve), `ops` (map op → share, must sum to 1), `keys` `{zipf: s, space: N}` or `{uniform: N}`, `sizes` (map op → size), `timeout`, `retries` |
| `cdn` | `ttl`, `hit_ratio` (`fixed: h` or `model`), `origin` (node id) |
| `load_balancer` | `policy` `round_robin|least_connections|weighted|consistent_hash`, `vnodes` (int, consistent_hash) |
| `rate_limiter` | `algorithm` `token_bucket|leaky_bucket|fixed_window|sliding_log|sliding_counter`, `limit` (rate), `burst` (int), `backend` (node id of a cache holding counters) |
| `service` | — (uses `ops`) |
| `cache` | `eviction` `lru|lfu|fifo|ttl`, `strategy` `cache_aside|read_through|write_through|write_around|write_back`, `keys` (int capacity), `hit_ratio` (`fixed: h` or `che`), `backend` (node id for read/write-through), `ttl` |
| `sql_db` | `replication_lag`, `shards` (int), `pool` (int connections) |
| `kv_cluster` | `nodes` (int), `vnodes`, `replication` N, `write_quorum` W, `read_quorum` R, `mode` `ap|cp` |
| `message_queue` | `partitions`, `retention` |
| `worker_pool` | `consumes` (queue node id), `workers`, `cost` (duration per job), `then` |
| `blob_store` | `capacity` in bytes/s (e.g. `500MB/s`) |
| `search_index` | `build_interval` |
| `ws_gateway` | `max_connections`, `heartbeat` (duration), `fanout` (avg recipients per message) |
| `service_discovery` | `failover` (duration) |
| `id_generator` | `algorithm` `snowflake|ticket_server|uuid` |
| `external_sink` | `failure_rate` (0–1), `politeness` (rate per host) |
| `notification_fanout` | `channels` (map name → node id), `dedup` (0–1) |
| `frontier` | `politeness` (rate per host), `hosts` (int) |

## Rate curve (`client.rate`)

```yaml
rate:
  base: 10k/s
  wave:   {shape: sine|square|diurnal, period: 24h, amplitude: 0.6, peak_at: 20:00}
  ramp:   {from: 0, to: 10k/s, over: 5m}
  spikes: [{at: 12:00, for: 10m, factor: 3}]
  points: [[0s, 1k/s], [1h, 5k/s]]          # replaces base
  noise:  {sigma: 0.15, correlation: 5m}     # phase 2
```
Terms multiply.

## Links

`{from, to, dir: in|out|inout (default out), ops: [..] (default all), latency, bandwidth, weight}`.
Direction is from the writer's point of view: `out` means `from` sends to `to`; `inout` means
replies flow back along the same link (the normal request/response case).

## SLO

`{metric: p50_latency|p95_latency|p99_latency|drop_rate|utilization|queue_lag|staleness, node, op, max}`.

## Faults

`{at: clock-or-duration, for: duration, kind: node_down|dc_down|partition|latency_spike|traffic_spike|cache_flush, node|dc|between, factor}`.
```

- [ ] **Step 2: Commit**

```bash
git add docs/scenario-schema.md
git commit -m "docs: scenario schema v1 draft, to be exercised by the 12 scenario drafts"
```

---

### Task 4: Config loader with reference validation (TDD) and `simsys validate`

**Files:**
- Create: `src/config/config.cppm`, `tests/unit/test_config.cpp`
- Modify: `src/cli/main.cpp`, `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing tests**

`tests/unit/test_config.cpp`:
```cpp
// Properties of the scenario loader: it accepts a minimal valid file and reports
// structural mistakes with file, line and column.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string_view>

import simsys.config;

namespace {

// Writes YAML to a temp file and returns its path. Tests read files because that is
// what the CLI does; the loader is not given a string API on purpose (YAGNI).
std::filesystem::path write_temp(std::string_view name, std::string_view yaml) {
  auto path = std::filesystem::temp_directory_path() / name;
  std::ofstream{path} << yaml;
  return path;
}

constexpr std::string_view minimal = R"(
version: 1
name: minimal
duration: 1m
nodes:
  users: {type: client, rate: {base: 100/s}, ops: {ping: 1.0}}
  api:   {type: service, capacity: 1k/s, latency: 1ms}
links:
  - {from: users, to: api, dir: inout}
)";

}  // namespace

TEST_CASE("a minimal scenario loads with its nodes and links") {
  auto s = simsys::config::load(write_temp("minimal.yaml", minimal));
  REQUIRE(s.has_value());
  CHECK(s->name == "minimal");
  CHECK(s->nodes.size() == 2);
  CHECK(s->links.size() == 1);
  CHECK(s->nodes.at(1).type == "service");
}

TEST_CASE("a link to an undeclared node is reported with its line") {
  auto s = simsys::config::load(write_temp("badlink.yaml", R"(
version: 1
name: bad
duration: 1m
nodes:
  users: {type: client, rate: {base: 100/s}, ops: {ping: 1.0}}
links:
  - {from: users, to: apii}
)"));
  REQUIRE_FALSE(s.has_value());
  CHECK(s.error().line == 8);
  CHECK(s.error().message == "link refers to unknown node 'apii'");
}

TEST_CASE("an unknown node type is rejected") {
  auto s = simsys::config::load(write_temp("badtype.yaml", R"(
version: 1
name: bad
duration: 1m
nodes:
  thing: {type: quantum_db}
links: []
)"));
  REQUIRE_FALSE(s.has_value());
  CHECK(s.error().message == "unknown node type 'quantum_db'");
}

TEST_CASE("a step pointing at an undeclared node is rejected") {
  auto s = simsys::config::load(write_temp("badstep.yaml", R"(
version: 1
name: bad
duration: 1m
nodes:
  api: {type: service, ops: {read: [{to: cache, op: read}]}}
links: []
)"));
  REQUIRE_FALSE(s.has_value());
  CHECK(s.error().message == "step refers to unknown node 'cache'");
}

TEST_CASE("a malformed duration is rejected with the unit parser's message") {
  auto s = simsys::config::load(write_temp("badunit.yaml", R"(
version: 1
name: bad
duration: 1 fortnight
nodes:
  api: {type: service}
links: []
)"));
  REQUIRE_FALSE(s.has_value());
  CHECK(s.error().message == "duration: unknown duration unit ' fortnight'");
}
```

In `tests/CMakeLists.txt` change `add_executable(unit_tests unit/test_units.cpp)` to `add_executable(unit_tests unit/test_units.cpp unit/test_config.cpp)`.

- [ ] **Step 2: Run tests to verify they fail**

```bash
cmake --build --preset gcc14-debug 2>&1 | grep -E "error" | head -3
```
Expected: `error: unknown module 'simsys.config'` or similar.

- [ ] **Step 3: Implement `src/config/config.cppm`**

```cpp
// Module `simsys.config`: loads a scenario YAML file into a checked `Scenario`.
// Phase 0 checks *structure and references* (every `to:`/`from:`/`consumes:`/`backend:`
// names a declared node, every `type` is known, every duration/rate parses). The
// engine-level meaning of each key arrives with the engine in phase 1.
module;
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <ranges>
#include <string_view>
#include <vector>

export module simsys.config;

import simsys.core;

export namespace simsys::config {

struct ConfigError {
  std::string file;
  int line;    // 1-based
  int column;  // 1-based
  std::string message;
};

struct NodeDecl {
  std::string id;
  std::string type;
};

struct LinkDecl {
  std::string from;
  std::string to;
};

struct Scenario {
  std::string name;
  int version{};
  SimTime duration{};
  std::vector<NodeDecl> nodes;
  std::vector<LinkDecl> links;
};

// The node catalogue from the design spec §7. A std::array of string_view is a
// compile-time constant: `constexpr` containers of literals need no allocation.
inline constexpr std::array<std::string_view, 18> known_types = {
    "client",        "cdn",          "load_balancer",     "rate_limiter",
    "service",       "cache",        "sql_db",            "kv_cluster",
    "message_queue", "worker_pool",  "blob_store",        "search_index",
    "ws_gateway",    "service_discovery", "id_generator", "external_sink",
    "notification_fanout", "frontier"};

std::expected<Scenario, ConfigError> load(const std::filesystem::path& file);

}  // namespace simsys::config

// ---------------------------------------------------------------------------
// Implementation. Not exported: importers see only the names above.
// ---------------------------------------------------------------------------
namespace simsys::config {

namespace {

// C++20 `using enum` is not needed here; what follows are small helpers that turn
// yaml-cpp's exception-and-Mark style into our std::expected style at one boundary.

ConfigError at(const std::filesystem::path& file, const YAML::Node& node, std::string message) {
  const auto mark = node.Mark();  // yaml-cpp positions are 0-based
  return ConfigError{file.string(), mark.line + 1, mark.column + 1, std::move(message)};
}

bool is_known_type(std::string_view type) {
  // C++20 <ranges> algorithms take a whole range instead of a begin/end pair.
  return std::ranges::find(known_types, type) != known_types.end();
}

// Every place a scenario may name another node, with the key that names it.
// Steps live under `ops: {op: [step, ...]}` and under `then: [step, ...]`.
std::expected<void, ConfigError> check_step_list(const std::filesystem::path& file,
                                                 const YAML::Node& steps,
                                                 const std::vector<NodeDecl>& nodes) {
  if (!steps.IsSequence()) return std::unexpected(at(file, steps, "steps must be a list"));
  for (const auto& step : steps) {
    const auto to = step["to"];
    if (!to) return std::unexpected(at(file, step, "step needs a 'to'"));
    const auto target = to.as<std::string>();
    const bool declared =
        std::ranges::any_of(nodes, [&](const NodeDecl& n) { return n.id == target; });
    if (!declared) {
      return std::unexpected(
          at(file, to, std::format("step refers to unknown node '{}'", target)));
    }
  }
  return {};
}

std::expected<void, ConfigError> check_node_reference(const std::filesystem::path& file,
                                                      const YAML::Node& ref,
                                                      const std::vector<NodeDecl>& nodes,
                                                      std::string_view what) {
  if (!ref) return {};
  const auto target = ref.as<std::string>();
  const bool declared =
      std::ranges::any_of(nodes, [&](const NodeDecl& n) { return n.id == target; });
  if (!declared) {
    return std::unexpected(
        at(file, ref, std::format("{} refers to unknown node '{}'", what, target)));
  }
  return {};
}

std::expected<void, ConfigError> check_duration(const std::filesystem::path& file,
                                                const YAML::Node& value, std::string_view key) {
  if (!value) return {};
  auto parsed = parse_duration(value.as<std::string>());
  if (!parsed) {
    return std::unexpected(at(file, value, std::format("{}: {}", key, parsed.error().message)));
  }
  return {};
}

std::expected<void, ConfigError> check_rate(const std::filesystem::path& file,
                                            const YAML::Node& value, std::string_view key) {
  if (!value) return {};
  auto parsed = parse_rate(value.as<std::string>());
  if (!parsed) {
    return std::unexpected(at(file, value, std::format("{}: {}", key, parsed.error().message)));
  }
  return {};
}

}  // namespace

std::expected<Scenario, ConfigError> load(const std::filesystem::path& file) {
  YAML::Node root;
  try {
    root = YAML::LoadFile(file.string());
  } catch (const YAML::Exception& e) {
    // The only exception boundary in the loader: yaml-cpp throws, we return.
    return std::unexpected(ConfigError{file.string(), e.mark.line + 1, e.mark.column + 1, e.msg});
  }

  Scenario s;
  for (std::string_view key : {"version", "name", "duration", "nodes", "links"}) {
    if (!root[std::string{key}]) {
      return std::unexpected(at(file, root, std::format("missing top-level key '{}'", key)));
    }
  }
  s.version = root["version"].as<int>();
  if (s.version != 1) return std::unexpected(at(file, root["version"], "version must be 1"));
  s.name = root["name"].as<std::string>();

  if (auto r = check_duration(file, root["duration"], "duration"); !r) return std::unexpected(r.error());
  s.duration = *parse_duration(root["duration"].as<std::string>());
  if (auto r = check_duration(file, root["step"], "step"); !r) return std::unexpected(r.error());
  if (auto r = check_duration(file, root["warmup"], "warmup"); !r) return std::unexpected(r.error());

  // --- nodes: collect ids and types first, so forward references resolve ----------
  const auto nodes = root["nodes"];
  if (!nodes.IsMap() || nodes.size() == 0) {
    return std::unexpected(at(file, nodes, "nodes must be a non-empty map"));
  }
  for (const auto& entry : nodes) {
    const auto id = entry.first.as<std::string>();
    const auto type = entry.second["type"];
    if (!type) return std::unexpected(at(file, entry.second, std::format("node '{}' needs a type", id)));
    const auto type_name = type.as<std::string>();
    if (!is_known_type(type_name)) {
      return std::unexpected(at(file, type, std::format("unknown node type '{}'", type_name)));
    }
    s.nodes.push_back(NodeDecl{id, type_name});
  }

  // --- nodes: references and units -------------------------------------------------
  for (const auto& entry : nodes) {
    const auto node = entry.second;
    if (auto r = check_rate(file, node["capacity"], "capacity"); !r) return std::unexpected(r.error());
    if (auto r = check_duration(file, node["latency"], "latency"); !r) return std::unexpected(r.error());
    if (const auto ops = node["ops"]; ops && ops.IsMap()) {
      for (const auto& op : ops) {
        if (op.second.IsSequence()) {  // client `ops` are shares (scalars); service `ops` are step lists
          if (auto r = check_step_list(file, op.second, s.nodes); !r) return std::unexpected(r.error());
        }
      }
    }
    if (const auto then = node["then"]) {
      if (auto r = check_step_list(file, then, s.nodes); !r) return std::unexpected(r.error());
    }
    for (std::string_view key : {"consumes", "backend", "origin"}) {
      if (auto r = check_node_reference(file, node[std::string{key}], s.nodes, key); !r) {
        return std::unexpected(r.error());
      }
    }
  }

  // --- links -------------------------------------------------------------------------
  const auto links = root["links"];
  if (!links.IsSequence()) return std::unexpected(at(file, links, "links must be a list"));
  for (const auto& link : links) {
    for (std::string_view end : {"from", "to"}) {
      const auto ref = link[std::string{end}];
      if (!ref) return std::unexpected(at(file, link, std::format("link needs '{}'", end)));
      if (auto r = check_node_reference(file, ref, s.nodes, "link"); !r) return std::unexpected(r.error());
    }
    if (auto r = check_duration(file, link["latency"], "latency"); !r) return std::unexpected(r.error());
    s.links.push_back(LinkDecl{link["from"].as<std::string>(), link["to"].as<std::string>()});
  }

  return s;
}

}  // namespace simsys::config
```
- [ ] **Step 4: Run tests to verify they pass**

```bash
cmake --build --preset gcc14-debug 2>&1 | tail -1 && ctest --preset gcc14-debug 2>&1 | tail -3
```
Expected: `100% tests passed, 0 tests failed out of 10`. If the "line == 8" check fails by one, print `s.error().line` and fix the test's expected value to what yaml-cpp reports for the `to: apii` scalar (count lines in the raw string: the first line after `R"(` is line 1 and is empty).

- [ ] **Step 5: Add the `validate` subcommand to `src/cli/main.cpp`**

Replace the file with:
```cpp
// `simsys` command-line entry point. Parsing is delegated to CLI11; commands are thin
// wrappers over library calls so they stay testable without a process.
#include <CLI/CLI.hpp>

#include <filesystem>
#include <print>
#include <string>

// C++20 `import`: brings the exported names of a named module into this translation
// unit. Unlike #include, it does not re-parse text; the compiler reads a compiled
// module interface (BMI). Imports go after the #includes to keep GCC 14 happy about
// header state.
import simsys.core;
import simsys.config;

namespace {

int run_validate(const std::filesystem::path& file) {
  auto scenario = simsys::config::load(file);
  if (!scenario) {
    const auto& e = scenario.error();
    // C++23 std::println to stderr: same compile-time-checked format strings as stdout.
    std::println(stderr, "{}:{}:{}: error: {}", e.file, e.line, e.column, e.message);
    return 1;
  }
  std::println("OK: {} ({} nodes, {} links)", scenario->name, scenario->nodes.size(),
               scenario->links.size());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"simsys - a fluid-flow system design simulator"};
  app.set_version_flag("--version", std::string{simsys::version});
  app.require_subcommand(1);

  std::filesystem::path file;
  auto* validate = app.add_subcommand("validate", "Check a scenario file for structural errors");
  validate->add_option("file", file, "scenario YAML")->required()->check(CLI::ExistingFile);

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    return app.exit(e);
  }

  if (*validate) return run_validate(file);
  return 0;
}
```

- [ ] **Step 6: Build and try it on a good and a bad file**

```bash
cmake --build --preset gcc14-debug 2>&1 | tail -1
printf 'version: 1\nname: t\nduration: 1m\nnodes:\n  a: {type: service}\nlinks: [{from: a, to: b}]\n' > /tmp/bad.yaml
./build/gcc14-debug/simsys validate /tmp/bad.yaml; echo "exit=$?"
```
Expected: `/tmp/bad.yaml:6:22: error: link refers to unknown node 'b'` and `exit=1` (column may differ by a few; it must point into the `to: b` entry).

- [ ] **Step 7: Format and commit**

```bash
clang-format-18 -i src/config/config.cppm src/cli/main.cpp tests/unit/test_config.cpp
git add -A src tests
git commit -m "config: YAML loader with reference and unit validation; simsys validate

Structural checks only (types, node references, units) so the 12 scenario
drafts can be machine-checked before the engine exists."
```

---

### Task 5: Scenario fixture test and CI

**Files:**
- Create: `tests/scenario/test_scenarios.cpp`, `.github/workflows/ci.yml`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the fixture test**

`tests/scenario/test_scenarios.cpp`:
```cpp
// Every scenario shipped under scenarios/ must validate. The directory is injected by
// CMake as SIMSYS_SCENARIOS_DIR (a compile definition is the one macro we accept; it
// carries a build-time path, which no language feature can).
#include <catch2/catch_test_macros.hpp>

#include <filesystem>

import simsys.config;

TEST_CASE("every shipped scenario validates") {
  namespace fs = std::filesystem;
  int count = 0;
  for (const auto& entry : fs::recursive_directory_iterator(SIMSYS_SCENARIOS_DIR)) {
    if (entry.path().extension() != ".yaml") continue;
    ++count;
    DYNAMIC_SECTION(entry.path().lexically_relative(SIMSYS_SCENARIOS_DIR).string()) {
      auto s = simsys::config::load(entry.path());
      if (!s) {
        FAIL(s.error().file << ":" << s.error().line << ":" << s.error().column << ": "
                            << s.error().message);
      }
      CHECK(s->nodes.size() >= 2);
      CHECK(!s->links.empty());
    }
  }
  CHECK(count >= 1);
}
```

`tests/CMakeLists.txt` (full file):
```cmake
# Tests are teaching material: name each TEST_CASE after the property it proves.
list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
include(Catch)

add_executable(unit_tests unit/test_units.cpp unit/test_config.cpp)
target_link_libraries(unit_tests PRIVATE simsys Catch2::Catch2WithMain simsys_warnings)
catch_discover_tests(unit_tests)

add_executable(scenario_tests scenario/test_scenarios.cpp)
target_link_libraries(scenario_tests PRIVATE simsys Catch2::Catch2WithMain simsys_warnings)
target_compile_definitions(scenario_tests PRIVATE SIMSYS_SCENARIOS_DIR="${CMAKE_SOURCE_DIR}/scenarios")
catch_discover_tests(scenario_tests)
```

- [ ] **Step 2: Add a first scenario so the test has something to load**

Create `scenarios/tutorial/hello/scenario.yaml`:
```yaml
# The smallest useful scenario: one client, one service. Used by the tutorial and as
# the smoke test for the loader.
version: 1
name: hello
duration: 1m
nodes:
  users: {type: client, rate: {base: 100/s}, ops: {ping: 1.0}}
  api:   {type: service, capacity: 1k/s, latency: 1ms}
links:
  - {from: users, to: api, dir: inout}
```

- [ ] **Step 3: Build and run tests**

```bash
cmake --preset gcc14-debug >/dev/null && cmake --build --preset gcc14-debug 2>&1 | tail -1 && ctest --preset gcc14-debug 2>&1 | tail -3
```
Expected: `100% tests passed, 0 tests failed out of 11`.

- [ ] **Step 4: Write `.github/workflows/ci.yml`**

```yaml
name: ci
on:
  push: { branches: [main] }
  pull_request:
jobs:
  build-test:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - name: Install GCC 14 and Ninja
        run: sudo apt-get update && sudo apt-get install -y g++-14 ninja-build
      - name: Configure
        run: cmake --preset gcc14-debug
      - name: Build
        run: cmake --build --preset gcc14-debug
      - name: Test
        run: ctest --preset gcc14-debug
  format:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - name: Install clang-format 18
        run: sudo apt-get update && sudo apt-get install -y clang-format-18
      - name: Check formatting
        run: find src tests -name '*.cpp' -o -name '*.cppm' | xargs clang-format-18 --dry-run -Werror
```

- [ ] **Step 5: Commit and push, then watch CI**

```bash
clang-format-18 -i tests/scenario/test_scenarios.cpp
git add -A tests scenarios .github
git commit -m "test: every shipped scenario must validate; GitHub Actions on GCC 14 + clang-format"
git push origin main
gh run watch --exit-status $(gh run list --limit 1 --json databaseId --jq '.[0].databaseId')
```
Expected: both jobs green. If the GCC job fails on an apt package name, change `g++-14` to `gcc-14 g++-14`.

---

### Task 6: Study notes for modules and std::expected

**Files:**
- Create: `docs/study/modules.md`, `docs/study/expected.md`

- [ ] **Step 1: Write `docs/study/modules.md`**

```markdown
# C++20 named modules

**Used in:** every `src/**/*.cppm` file; `import simsys.core;` in `src/cli/main.cpp`.

## What
A module is a named unit of code with an explicit export surface. `export module simsys.core;`
opens it; only declarations marked `export` are visible to `import simsys.core;`.

## Why it replaces headers
- Headers are textual inclusion: every includer re-parses them, and macros leak across files.
- A module interface is compiled once into a BMI (binary module interface); importers read that.
- No include guards, no `#pragma once`, no ODR surprises from macro differences.

## The global module fragment
`module;` at the top of a module unit opens the *global module fragment*, the only place a
module may `#include` legacy headers. GCC 14's standard library is still header-based, so
every module in this project starts with `module;` + the `<...>` includes it needs.

## Rules of thumb used here
1. One module per layer (`simsys.core`, `simsys.config`, ...), one `.cppm` interface unit each.
2. In a non-module file, `#include` first, then `import` (GCC 14 is picky the other way round).
3. CMake 3.28+: declare interface units with `target_sources(... FILE_SET CXX_MODULES FILES ...)`.
   CMake scans sources for `import`/`export module` and orders the build.

## Toolchain status (2026-10)
GCC 14 + CMake 3.28 + Ninja: works. Clang 18: builds modules but lacks `std::expected` /
`std::generator` with libstdc++ (reports `__cpp_concepts` 201907L) and has no `<generator>`
in libc++. `import std;` needs GCC 15 / CMake 3.30 — revisit then.
```

- [ ] **Step 2: Write `docs/study/expected.md`**

```markdown
# C++23 `std::expected<T, E>`

**Used in:** `simsys::parse_duration`, `simsys::parse_rate` (`src/core/core.cppm`),
`simsys::config::load` (`src/config/config.cppm`).

## What
A sum type holding either a `T` (the expected value) or an `E` (the error). Think
`std::variant<T, E>` with a value-oriented API: `has_value()`, `operator*`, `value()`,
`error()`, `value_or()`, and monadic `and_then` / `transform` / `or_else`.

## Why
Before C++23 a parser chose between (a) exceptions, invisible in the signature and costly
on the error path, or (b) error codes with out-parameters, easy to ignore. `std::expected`
puts the error in the return type: the caller *must* look, and the error carries data
(`ParseError{input, message}`), which the config loader decorates with file:line:column.

## Pattern used here
```cpp
std::expected<SimTime, ParseError> parse_duration(std::string_view);
auto t = parse_duration("0.5ms");
if (!t) return std::unexpected(t.error());   // propagate
use(*t);                                     // or t.value()
```
`std::expected<void, E>` (used by the validator helpers) means "either fine, or this error".

## The one exception boundary
yaml-cpp throws. `config::load` catches `YAML::Exception` in exactly one place and turns it
into a `ConfigError`. Everything above that is exception-free by construction.
```

- [ ] **Step 3: Commit**

```bash
git add docs/study
git commit -m "docs(study): notes on named modules and std::expected"
```

---

## Scenario drafts

Tasks 7–18 each create `scenarios/<group>/<name>/scenario.yaml` and `README.md`, run
`./build/gcc14-debug/simsys validate <file>`, run `ctest --preset gcc14-debug`, and commit.
If a draft needs a key the schema does not have, **add it to `docs/scenario-schema.md` in
the same commit** — that is the purpose of phase 0. If the validator rejects a key the
schema allows, fix the validator (it only checks references and units, so this should be rare).

Each README has four sections: *Book design* (3–6 bullets summarising the chapter's
final architecture), *What the model captures*, *What it abstracts away* (mechanisms that
are explained, not simulated), *Things to try* (knobs to turn, what should happen).
Back-of-the-envelope numbers follow the book where it gives them.

### Task 7: URL shortener (ch. 8)

- [ ] **Step 1: Write `scenarios/book/url-shortener/scenario.yaml`**

```yaml
# System Design Interview vol. 1, ch. 8 — URL shortener.
# Book numbers: 100M URLs/day written (~1,160/s), read:write 10:1 (~11,600/s reads),
# 365B records over 10 years, 7-char base-62 short URLs.
version: 1
name: url-shortener
seed: 1
duration: 24h
step: 100ms
warmup: 2m

nodes:
  users:
    type: client
    rate:
      base: 12.8k/s                          # 11.6k reads + 1.16k writes
      wave: {shape: sine, period: 24h, amplitude: 0.5, peak_at: 14:00}
    ops: {redirect: 0.91, shorten: 0.09}
    keys: {zipf: 1.0, space: 100M}           # a few short links get most of the clicks
    timeout: 1s

  limiter:
    type: rate_limiter
    algorithm: token_bucket
    limit: 20k/s
    burst: 2000
    backend: rules_cache

  rules_cache: {type: cache, eviction: lru, keys: 10k, strategy: cache_aside}

  lb: {type: load_balancer, policy: round_robin}

  web:
    type: service
    replicas: 6
    capacity: 3k/s
    latency: 0.3ms
    ops:
      redirect: [{to: url_cache, op: read}, {to: url_db, op: read, on: miss}]
      shorten:  [{to: id_gen, op: next_id}, {to: url_db, op: write}]

  id_gen: {type: id_generator, algorithm: snowflake, replicas: 2}

  url_cache:
    type: cache
    eviction: lru
    strategy: cache_aside
    keys: 20M                                # ~20% of the hot key space
    hit_ratio: che

  url_db:
    type: sql_db
    replicas: 3                              # 1 primary + 2 read replicas
    capacity: 5k/s
    replication_lag: 50ms
    advanced: {queue_limit: 1000}

links:
  - {from: users, to: limiter, dir: inout}
  - {from: limiter, to: rules_cache, dir: inout}
  - {from: limiter, to: lb, dir: inout}
  - {from: lb, to: web, dir: inout}
  - {from: web, to: url_cache, dir: inout}
  - {from: web, to: url_db, dir: inout}
  - {from: web, to: id_gen, dir: inout}

slo:
  - {metric: p99_latency, node: users, op: redirect, max: 50ms}
  - {metric: drop_rate, max: 0.001}
```

- [ ] **Step 2: Write `scenarios/book/url-shortener/README.md`**

```markdown
# URL shortener (ch. 8)

## Book design
- Two APIs: `POST /shorten` (long → short) and `GET /{short}` (301/302 redirect).
- 7-character base-62 IDs from a unique ID generator (snowflake), not hashing + collision checks.
- Reads dominate 10:1, so a cache in front of the database serves most redirects.
- Rate limiter in front of the web tier to stop abusive clients.
- Relational store (billions of rows) with read replicas; web tier is stateless and scaled out.

## What the model captures
- The read/write split and the cache hit ratio computed from the Zipf key skew and cache size.
- Database saturation when the cache is too small or cold.
- The rate limiter shedding traffic above 20k/s with a 2,000-token burst allowance.
- The ID generator as a capacity-limited node (compare `ticket_server` in the ID lab).

## What it abstracts away
- Base-62 encoding, 301 vs 302 semantics, collision handling.
- Cache invalidation details (links are immutable, so there are none).

## Things to try
- Set `url_cache.keys` to `1M`: watch `url_db` go yellow at the afternoon peak.
- Remove the `limiter` and add a spike `{at: 14:00, for: 5m, factor: 5}`.
- Change `id_gen` to `ticket_server` with `replicas: 1`: the write path gets a hot spot.
```

- [ ] **Step 3: Validate, test, commit**

```bash
./build/gcc14-debug/simsys validate scenarios/book/url-shortener/scenario.yaml
ctest --preset gcc14-debug 2>&1 | tail -1
git add scenarios/book/url-shortener docs/scenario-schema.md
git commit -m "scenarios: URL shortener draft (ch. 8)"
```
Expected: `OK: url-shortener (9 nodes, 7 links)`; tests pass.

### Task 8: Web crawler (ch. 9)

- [ ] **Step 1: Write `scenarios/book/web-crawler/scenario.yaml`**

```yaml
# System Design Interview vol. 1, ch. 9 — Web crawler.
# Book numbers: 1B pages/month (~400 pages/s average, 800/s peak), 500 KB per page,
# 30% duplicate content, 10M+ hosts, politeness: one request per host at a time.
version: 1
name: web-crawler
seed: 1
duration: 6h
step: 1s
warmup: 10m

nodes:
  seeds:                                      # seed URLs injected by the scheduler
    type: client
    rate: {base: 50/s}
    ops: {crawl: 1.0}
    sizes: {crawl: 200B}

  frontier:                                   # URL frontier: prioritiser + politeness queues
    type: frontier
    politeness: 1/s                           # per host
    hosts: 10M
    capacity: 5k/s
    then: [{to: downloader, op: fetch}]

  downloader:
    type: worker_pool
    consumes: frontier
    workers: 1000
    cost: 500ms                               # dominated by the remote site, see internet
    then:
      - {to: dns, op: resolve}
      - {to: internet, op: get}
      - {to: parser, op: parse, on: always}

  dns:
    type: cache
    eviction: ttl
    ttl: 1h
    keys: 10M
    hit_ratio: {fixed: 0.95}
    backend: dns_upstream
  dns_upstream: {type: external_sink, latency: 20ms, failure_rate: 0.01}

  internet:
    type: external_sink
    latency: 300ms
    failure_rate: 0.05
    politeness: 1/s                           # per host

  parser:
    type: service
    replicas: 20
    capacity: 100/s
    latency: 8ms
    ops:
      parse:
        - {to: content_seen, op: check}
        - {to: content_store, op: write, on: miss}      # only new content is stored
        - {to: link_extractor, op: extract, on: miss}

  content_seen:                               # "content seen?" — hash lookup, ~30% duplicates
    type: cache
    eviction: lru
    keys: 1G
    hit_ratio: {fixed: 0.30}

  content_store: {type: blob_store, capacity: 400MB/s}

  link_extractor:
    type: service
    replicas: 10
    capacity: 200/s
    latency: 2ms
    ops:
      extract:
        - {to: url_filter, op: filter, factor: 50}       # ~50 links per page

  url_filter:
    type: service
    replicas: 10
    capacity: 20k/s
    latency: 0.1ms
    ops:
      filter: [{to: url_seen, op: check}]

  url_seen:                                   # bloom filter / hash table, ~90% already seen
    type: cache
    eviction: lru
    keys: 10G
    hit_ratio: {fixed: 0.90}
    then: [{to: frontier, op: crawl, on: miss}]          # feedback edge: new URLs re-enter

links:
  - {from: seeds, to: frontier}
  - {from: frontier, to: downloader}
  - {from: downloader, to: dns, dir: inout}
  - {from: dns, to: dns_upstream, dir: inout}
  - {from: downloader, to: internet, dir: inout}
  - {from: downloader, to: parser}
  - {from: parser, to: content_seen, dir: inout}
  - {from: parser, to: content_store}
  - {from: parser, to: link_extractor}
  - {from: link_extractor, to: url_filter}
  - {from: url_filter, to: url_seen, dir: inout}
  - {from: url_seen, to: frontier}

slo:
  - {metric: utilization, node: downloader, max: 0.85}
  - {metric: queue_lag, node: frontier, max: 30m}
```

- [ ] **Step 2: Write `scenarios/book/web-crawler/README.md`**

```markdown
# Web crawler (ch. 9)

## Book design
- Seed URLs → URL frontier (prioritiser + politeness queues) → HTML downloader (with DNS cache)
  → content parser → "content seen?" → content storage → link extractor → URL filter →
  "URL seen?" → back into the frontier.
- Politeness: at most one in-flight request per host; priority by PageRank/traffic.
- Robustness: consistent hashing across downloaders, checkpoints, spider-trap detection.

## What the model captures
- The crawl loop as a *feedback flow*: each page yields ~50 links, 90% already seen; the
  frontier grows or drains depending on the balance between discovery and download capacity.
- Politeness as a per-host rate cap on the Internet sink and the frontier.
- Duplicate content (30%) short-circuiting storage and extraction.
- Downloader parallelism (1,000 workers × ~0.5 s per fetch ≈ 2,000 pages/s ceiling).

## What it abstracts away
- Robots.txt, spider traps, PageRank ordering, consistent hashing of hosts to downloaders.
- Checkpoints and restart.

## Things to try
- Raise `internet.latency` to 1s: downloader utilization climbs, frontier lag grows.
- Set `url_seen.hit_ratio` to 0.5: the frontier explodes — the model's version of a crawl trap.
- Add `faults: [{at: 2h, kind: node_down, node: dns_upstream, for: 10m}]`.
```

- [ ] **Step 3: Validate, test, commit**

```bash
./build/gcc14-debug/simsys validate scenarios/book/web-crawler/scenario.yaml
ctest --preset gcc14-debug 2>&1 | tail -1
git add scenarios/book/web-crawler docs/scenario-schema.md
git commit -m "scenarios: web crawler draft (ch. 9) — first feedback loop; adds step.factor and cache.then to the schema"
```
Schema additions required by this draft: `then` on `cache` (flow of misses onward), `sizes` on client, `politeness` on `external_sink`. Record them in `docs/scenario-schema.md` if missing.

### Task 9: Notification system (ch. 10)

- [ ] **Step 1: Write `scenarios/book/notification-system/scenario.yaml`**

```yaml
# System Design Interview vol. 1, ch. 10 — Notification system.
# Book numbers: 10M mobile push, 1M SMS, 5M email per day. Here scaled 10x for a busier
# system (~1.9k/s average) with a marketing-blast spike.
version: 1
name: notification-system
seed: 1
duration: 24h
step: 100ms
warmup: 2m

nodes:
  services:                                   # internal services that trigger notifications
    type: client
    rate:
      base: 1.9k/s
      wave: {shape: sine, period: 24h, amplitude: 0.4, peak_at: 18:00}
      spikes: [{at: 09:00, for: 15m, factor: 20}]       # marketing blast
    ops: {notify: 1.0}
    timeout: 5s

  lb: {type: load_balancer, policy: round_robin}

  notification_svc:
    type: service
    replicas: 4
    capacity: 5k/s
    latency: 1ms
    ops:
      notify:
        - {to: user_cache, op: read}
        - {to: user_db, op: read, on: miss}
        - {to: user_limiter, op: check}
        - {to: fanout, op: route}

  user_cache: {type: cache, eviction: lru, keys: 5M, hit_ratio: che, strategy: cache_aside}
  user_db: {type: sql_db, replicas: 2, capacity: 3k/s}

  user_limiter:                               # per-user rate limit
    type: rate_limiter
    algorithm: sliding_counter
    limit: 100k/s                             # aggregate; per-user rules are in the backend
    backend: user_cache

  fanout:
    type: notification_fanout
    dedup: 0.02
    channels: {ios: ios_q, android: android_q, sms: sms_q, email: email_q}
    then:
      - {to: ios_q, op: publish, factor: 0.6}
      - {to: android_q, op: publish, factor: 0.3}
      - {to: sms_q, op: publish, factor: 0.05}
      - {to: email_q, op: publish, factor: 0.3}
      - {to: log_db, op: write}

  ios_q:     {type: message_queue, partitions: 16}
  android_q: {type: message_queue, partitions: 16}
  sms_q:     {type: message_queue, partitions: 4}
  email_q:   {type: message_queue, partitions: 8}

  ios_workers:     {type: worker_pool, consumes: ios_q, workers: 50, cost: 20ms, then: [{to: apns, op: send}]}
  android_workers: {type: worker_pool, consumes: android_q, workers: 50, cost: 20ms, then: [{to: fcm, op: send}]}
  sms_workers:     {type: worker_pool, consumes: sms_q, workers: 20, cost: 50ms, then: [{to: sms_provider, op: send}]}
  email_workers:   {type: worker_pool, consumes: email_q, workers: 20, cost: 30ms, then: [{to: email_provider, op: send}]}

  apns:           {type: external_sink, latency: 150ms, failure_rate: 0.02}
  fcm:            {type: external_sink, latency: 150ms, failure_rate: 0.02}
  sms_provider:   {type: external_sink, latency: 500ms, failure_rate: 0.05}
  email_provider: {type: external_sink, latency: 300ms, failure_rate: 0.01}

  log_db: {type: kv_cluster, nodes: 3, replication: 2, write_quorum: 1, read_quorum: 1, capacity: 20k/s}

links:
  - {from: services, to: lb, dir: inout}
  - {from: lb, to: notification_svc, dir: inout}
  - {from: notification_svc, to: user_cache, dir: inout}
  - {from: notification_svc, to: user_db, dir: inout}
  - {from: notification_svc, to: user_limiter, dir: inout}
  - {from: user_limiter, to: user_cache, dir: inout}
  - {from: notification_svc, to: fanout}
  - {from: fanout, to: ios_q}
  - {from: fanout, to: android_q}
  - {from: fanout, to: sms_q}
  - {from: fanout, to: email_q}
  - {from: fanout, to: log_db}
  - {from: ios_q, to: ios_workers}
  - {from: android_q, to: android_workers}
  - {from: sms_q, to: sms_workers}
  - {from: email_q, to: email_workers}
  - {from: ios_workers, to: apns, dir: inout}
  - {from: android_workers, to: fcm, dir: inout}
  - {from: sms_workers, to: sms_provider, dir: inout}
  - {from: email_workers, to: email_provider, dir: inout}

slo:
  - {metric: queue_lag, node: ios_q, max: 5m}
  - {metric: queue_lag, node: sms_q, max: 10m}
  - {metric: drop_rate, max: 0.001}
```

- [ ] **Step 2: Write `scenarios/book/notification-system/README.md`**

```markdown
# Notification system (ch. 10)

## Book design
- Services call a notification service, which looks up user devices/preferences, applies
  per-user rate limits, and publishes one message per channel to a queue.
- One worker pool per channel pulls from its queue and calls the third party
  (APNs, FCM, SMS vendor, email vendor); failures are retried, logs and analytics are recorded.
- Decoupling via queues absorbs bursts; workers are scaled per channel.

## What the model captures
- A marketing blast (20x for 15 minutes) absorbed by the queues: lag grows, then drains at
  the workers' rate. The per-channel worker counts decide how long the drain takes.
- Third-party latency and failure rates as the real bottleneck (20 ms worker cost vs 150–500 ms
  round trips).
- Dedup (2%) and the fan-out ratio: one notification → 1.25 channel messages on average.

## What it abstracts away
- Retry policy and dead-letter queues (phase 3 adds retry feedback).
- Template rendering, user opt-outs, delivery receipts.

## Things to try
- Cut `sms_workers.workers` to 5: watch `sms_q` lag blow past the SLO after the 09:00 blast.
- Set `apns.failure_rate` to 0.3 (an outage): with retries enabled in phase 3 the queue backs up.
```

- [ ] **Step 3: Validate, test, commit**

```bash
./build/gcc14-debug/simsys validate scenarios/book/notification-system/scenario.yaml
ctest --preset gcc14-debug 2>&1 | tail -1
git add scenarios/book/notification-system docs/scenario-schema.md
git commit -m "scenarios: notification system draft (ch. 10)"
```

### Task 10: News feed (ch. 11)

- [ ] **Step 1: Write `scenarios/book/news-feed/scenario.yaml`**

```yaml
# System Design Interview vol. 1, ch. 11 — News feed.
# Book design: fan-out on write for ordinary users (pre-compute feeds into a cache),
# fan-out on read for celebrities. 10M DAU, each user posts a few times a day and reads
# the feed many times. Average 300 friends → each post fans out to ~300 feed caches.
version: 1
name: news-feed
seed: 1
duration: 24h
step: 100ms
warmup: 5m

datacenters:
  us-east: {}

nodes:
  users:
    type: client
    dc: us-east
    rate:
      base: 10k/s
      wave: {shape: sine, period: 24h, amplitude: 0.6, peak_at: 20:00}
      spikes: [{at: 12:00, for: 10m, factor: 3}]
    ops: {read_feed: 0.9, post: 0.1}
    keys: {zipf: 1.1, space: 50M}
    sizes: {post: 2KB}
    timeout: 500ms

  cdn: {type: cdn, dc: us-east, ttl: 1h, hit_ratio: {fixed: 0.95}, origin: media_store}
  media_store: {type: blob_store, capacity: 1GB/s}

  lb: {type: load_balancer, dc: us-east, policy: least_connections}

  web:
    type: service
    dc: us-east
    replicas: 8
    capacity: 2k/s
    latency: 0.5ms
    ops:
      read_feed:
        - {to: feed_cache, op: read}                    # list of post ids for this user
        - {to: post_cache, op: read, factor: 20}        # hydrate ~20 posts
        - {to: post_db, op: read, on: miss}
        - {to: user_cache, op: read, factor: 20}
      post:
        - {to: post_db, op: write}
        - {to: post_cache, op: write}
        - {to: fanout_q, op: publish}

  feed_cache: {type: cache, eviction: lru, keys: 10M, hit_ratio: che, strategy: cache_aside}
  post_cache: {type: cache, eviction: lru, keys: 50M, hit_ratio: che, strategy: cache_aside}
  user_cache: {type: cache, eviction: lru, keys: 10M, hit_ratio: {fixed: 0.98}, strategy: cache_aside}

  post_db:
    type: sql_db
    replicas: 3
    replication_lag: 50ms
    capacity: 5k/s
    advanced: {queue_limit: 500, service_variability: 1.5}

  fanout_q: {type: message_queue, partitions: 16}

  fanout:
    type: worker_pool
    consumes: fanout_q
    workers: 32
    cost: 5ms
    then:
      - {to: graph_db, op: read}                        # friend list
      - {to: feed_cache, op: write, factor: 300}        # one write per friend

  graph_db: {type: kv_cluster, nodes: 6, replication: 3, write_quorum: 2, read_quorum: 2, capacity: 30k/s}

links:
  - {from: users, to: cdn, dir: inout, ops: [read_media]}
  - {from: cdn, to: media_store, dir: inout}
  - {from: users, to: lb, dir: inout, ops: [read_feed, post]}
  - {from: lb, to: web, dir: inout}
  - {from: web, to: feed_cache, dir: inout, latency: 0.3ms}
  - {from: web, to: post_cache, dir: inout, latency: 0.3ms}
  - {from: web, to: user_cache, dir: inout, latency: 0.3ms}
  - {from: web, to: post_db, dir: inout}
  - {from: web, to: fanout_q}
  - {from: fanout_q, to: fanout}
  - {from: fanout, to: graph_db, dir: inout}
  - {from: fanout, to: feed_cache}

slo:
  - {metric: p99_latency, node: users, op: read_feed, max: 200ms}
  - {metric: drop_rate, max: 0.001}
  - {metric: queue_lag, node: fanout_q, max: 2m}

faults:
  - {at: 15:00, kind: node_down, node: post_db, for: 30s}
```

- [ ] **Step 2: Write `scenarios/book/news-feed/README.md`**

```markdown
# News feed (ch. 11)

## Book design
- Publishing: web → post DB + post cache → fan-out queue → fan-out workers look up the
  social graph and write the post id into each friend's feed cache (fan-out on write).
- Reading: web → feed cache (post ids) → post cache / user cache for hydration → DB on miss.
- Hybrid: celebrities use fan-out on read; the rest fan-out on write.
- CDN for media; caches layered: feed, post, user, social graph, counters.

## What the model captures
- The 300x write amplification of fan-out on write hitting `feed_cache` and `graph_db`.
- Read path fan-out (20 posts hydrated per feed read) multiplying cache traffic.
- Queue lag at the evening peak as the fan-out workers fall behind.
- A 30-second database outage at 15:00: writes back up in `fanout_q`; reads survive on caches.

## What it abstracts away
- Celebrity handling (fan-out on read) — model as a second client with its own op in a variant.
- Ranking, pagination, inactive-user skipping (which the book uses to shrink fan-out).

## Things to try
- `fanout.workers: 8` — the fan-out queue never drains after the 12:00 spike.
- `post_cache.keys: 5M` — hydration misses push `post_db` into the red at 20:00.
- Switch `post` share to 0.3 (a very chatty day) and watch `graph_db`.
```

- [ ] **Step 3: Validate, test, commit**

```bash
./build/gcc14-debug/simsys validate scenarios/book/news-feed/scenario.yaml
ctest --preset gcc14-debug 2>&1 | tail -1
git add scenarios/book/news-feed docs/scenario-schema.md
git commit -m "scenarios: news feed draft (ch. 11) — fan-out on write, 300x amplification"
```

### Task 11: Chat system (ch. 12)

- [ ] **Step 1: Write `scenarios/book/chat-system/scenario.yaml`**

```yaml
# System Design Interview vol. 1, ch. 12 — Chat system.
# Book numbers: 50M DAU, 40 messages/user/day → ~23k messages/s average; 1:1 and small
# group chats (≤100 members); online presence with 5-second heartbeats; push for offline.
version: 1
name: chat-system
seed: 1
duration: 24h
step: 100ms
warmup: 5m

nodes:
  users:
    type: client
    rate:
      base: 60k/s                              # messages + heartbeats + API calls
      wave: {shape: sine, period: 24h, amplitude: 0.5, peak_at: 21:00}
    ops: {send: 0.35, heartbeat: 0.55, api: 0.10}
    sizes: {send: 1KB}
    timeout: 2s

  lb: {type: load_balancer, policy: least_connections}

  api_servers:                                 # stateless: login, signup, profile, service discovery
    type: service
    replicas: 6
    capacity: 3k/s
    latency: 1ms
    ops:
      api:
        - {to: kv_cache, op: read}
        - {to: user_db, op: read, on: miss}
        - {to: discovery, op: lookup}

  discovery: {type: service_discovery, failover: 10s, capacity: 50k/s, latency: 0.5ms}

  chat_servers:                                # stateful websocket servers
    type: ws_gateway
    replicas: 20
    max_connections: 100k
    heartbeat: 5s
    fanout: 1.4                                # avg recipients per message (1:1 + groups)
    capacity: 10k/s
    latency: 0.5ms
    ops:
      send:
        - {to: id_gen, op: next_id}
        - {to: message_q, op: publish}
        - {to: message_store, op: write}
      heartbeat:
        - {to: presence, op: beat}

  id_gen: {type: id_generator, algorithm: snowflake, replicas: 3}

  message_q: {type: message_queue, partitions: 64}

  sync_workers:                                # deliver to recipient's chat server or push
    type: worker_pool
    consumes: message_q
    workers: 64
    cost: 1ms
    then:
      - {to: presence, op: read}
      - {to: chat_servers, op: deliver, factor: 1.4}
      - {to: push, op: notify, factor: 0.3}    # 30% of recipients offline

  message_store:                               # Cassandra-like: write-heavy, time-series keys
    type: kv_cluster
    nodes: 12
    vnodes: 256
    replication: 3
    write_quorum: 2
    read_quorum: 2
    capacity: 10k/s                            # per node

  presence:
    type: kv_cluster
    nodes: 4
    replication: 2
    write_quorum: 1
    read_quorum: 1
    capacity: 50k/s

  kv_cache: {type: cache, eviction: lru, keys: 20M, hit_ratio: che, strategy: cache_aside}
  user_db: {type: sql_db, replicas: 3, capacity: 4k/s}

  push: {type: external_sink, latency: 150ms, failure_rate: 0.02}

links:
  - {from: users, to: lb, dir: inout}
  - {from: lb, to: api_servers, dir: inout, ops: [api]}
  - {from: lb, to: chat_servers, dir: inout, ops: [send, heartbeat]}
  - {from: api_servers, to: kv_cache, dir: inout}
  - {from: api_servers, to: user_db, dir: inout}
  - {from: api_servers, to: discovery, dir: inout}
  - {from: chat_servers, to: id_gen, dir: inout}
  - {from: chat_servers, to: message_q}
  - {from: chat_servers, to: message_store, dir: inout}
  - {from: chat_servers, to: presence, dir: inout}
  - {from: message_q, to: sync_workers}
  - {from: sync_workers, to: presence, dir: inout}
  - {from: sync_workers, to: chat_servers}
  - {from: sync_workers, to: push, dir: inout}

slo:
  - {metric: p99_latency, node: users, op: send, max: 300ms}
  - {metric: queue_lag, node: message_q, max: 10s}
  - {metric: drop_rate, max: 0.001}

faults:
  - {at: 21:30, kind: node_down, node: chat_servers, for: 1m}   # one replica's worth; see README
```

- [ ] **Step 2: Write `scenarios/book/chat-system/README.md`**

```markdown
# Chat system (ch. 12)

## Book design
- Stateless API servers (login, profile, friends) behind a load balancer; stateful chat
  servers holding WebSocket connections, chosen via service discovery (ZooKeeper).
- Message flow: sender's chat server → ID generator → message sync queue → recipient's chat
  server (online) or push notification (offline); messages persisted in a key-value store.
- Presence servers with heartbeats; group chats (≤100) fan out one queue entry per member.

## What the model captures
- Heartbeat traffic dominating request counts (55% of all ops) while messages dominate bytes.
- Group fan-out (1.4 avg recipients) and the 30% offline push fraction.
- Write-heavy key-value store sized by per-node capacity, replication 3, quorum 2.
- Losing chat-server capacity at the 21:30 peak: connections rebalance, the queue backs up.

## What it abstracts away
- Connection state and reconnection storms (a scripted `traffic_spike` approximates one).
- Message ordering, end-to-end encryption, media attachments.

## Things to try
- `fanout: 20` (big groups) — the sync workers and `message_store` go red.
- `heartbeat: 1s` — presence becomes the bottleneck without a single message changing.
- `message_store.nodes: 6` — see quorum writes saturate.
```

- [ ] **Step 3: Validate, test, commit**

```bash
./build/gcc14-debug/simsys validate scenarios/book/chat-system/scenario.yaml
ctest --preset gcc14-debug 2>&1 | tail -1
git add scenarios/book/chat-system docs/scenario-schema.md
git commit -m "scenarios: chat system draft (ch. 12) — stateful gateways, presence, quorum store"
```

### Task 12: Search autocomplete (ch. 13)

- [ ] **Step 1: Write `scenarios/book/search-autocomplete/scenario.yaml`**

```yaml
# System Design Interview vol. 1, ch. 13 — Search autocomplete.
# Book numbers: 10M DAU, 10 searches/day, ~20 characters per query → ~24k requests/s
# average (one per keystroke), peak 2x. Trie served from cache; rebuilt weekly from
# aggregated query logs. Browser caching cuts a large share of requests.
version: 1
name: search-autocomplete
seed: 1
duration: 24h
step: 100ms
warmup: 2m

nodes:
  users:
    type: client
    rate:
      base: 24k/s
      wave: {shape: sine, period: 24h, amplitude: 0.5, peak_at: 20:00}
    ops: {suggest: 0.95, search: 0.05}
    keys: {zipf: 1.2, space: 100M}            # prefixes are extremely skewed
    timeout: 100ms

  browser_cache:                              # client-side caching of suggestions
    type: cache
    eviction: ttl
    ttl: 1h
    keys: 1M
    hit_ratio: {fixed: 0.40}
    then: [{to: lb, op: suggest, on: miss}]

  lb: {type: load_balancer, policy: round_robin}

  api:
    type: service
    replicas: 10
    capacity: 5k/s
    latency: 0.5ms
    ops:
      suggest:
        - {to: trie_cache, op: read}
        - {to: trie_db, op: read, on: miss}
      search:
        - {to: query_log_q, op: publish}

  trie_cache:
    type: cache
    eviction: lru
    keys: 50M
    hit_ratio: che
    strategy: read_through
    backend: trie_db

  trie_db:
    type: search_index
    replicas: 4
    capacity: 2k/s
    latency: 5ms
    build_interval: 24h

  query_log_q: {type: message_queue, partitions: 8}

  aggregators:
    type: worker_pool
    consumes: query_log_q
    workers: 4
    cost: 2ms
    then: [{to: aggregated_store, op: write}]

  aggregated_store: {type: blob_store, capacity: 100MB/s}

links:
  - {from: users, to: browser_cache, dir: inout}
  - {from: browser_cache, to: lb, dir: inout}
  - {from: users, to: lb, dir: inout, ops: [search]}
  - {from: lb, to: api, dir: inout}
  - {from: api, to: trie_cache, dir: inout}
  - {from: api, to: trie_db, dir: inout}
  - {from: api, to: query_log_q}
  - {from: trie_cache, to: trie_db, dir: inout}
  - {from: query_log_q, to: aggregators}
  - {from: aggregators, to: aggregated_store}

slo:
  - {metric: p99_latency, node: users, op: suggest, max: 100ms}
  - {metric: drop_rate, max: 0.0005}
```

- [ ] **Step 2: Write `scenarios/book/search-autocomplete/README.md`**

```markdown
# Search autocomplete (ch. 13)

## Book design
- Data-gathering path: query logs → aggregators (weekly/daily) → workers build a trie →
  trie DB (document store or key-value, prefix → top-k suggestions) → trie cache.
- Query path: browser cache → load balancer → API servers → trie cache → trie DB on miss.
- Trie sharded by prefix with a shard-map manager; filtering of unsafe suggestions.

## What the model captures
- One request per keystroke and the 40% browser-cache cut.
- Extreme key skew (Zipf 1.2): a 50M-entry cache serves nearly everything; the trie DB
  is only touched by rare prefixes and rebuilds.
- The 100 ms latency budget against a 5 ms index read on misses.

## What it abstracts away
- Trie structure, top-k pre-computation, sharding by prefix ranges.
- Real-time trending (the book defers it too).

## Things to try
- `trie_cache.keys: 1M` — misses hit `trie_db` and p99 fails the SLO at 20:00.
- `browser_cache.hit_ratio: 0` — the API tier needs twice the replicas.
```

- [ ] **Step 3: Validate, test, commit**

```bash
./build/gcc14-debug/simsys validate scenarios/book/search-autocomplete/scenario.yaml
ctest --preset gcc14-debug 2>&1 | tail -1
git add scenarios/book/search-autocomplete docs/scenario-schema.md
git commit -m "scenarios: search autocomplete draft (ch. 13)"
```

### Task 13: YouTube (ch. 14)

- [ ] **Step 1: Write `scenarios/book/youtube/scenario.yaml`**

```yaml
# System Design Interview vol. 1, ch. 14 — Video streaming (YouTube).
# Book numbers: 5M DAU, 5 videos watched/user/day, 10% upload one video/day, 300 MB
# average upload → ~290 streams/s, ~6 uploads/s, 150 TB/day of uploads. CDN serves
# streams; uploads go straight to blob storage via presigned URLs; transcoding is a DAG.
version: 1
name: youtube
seed: 1
duration: 24h
step: 100ms
warmup: 5m

datacenters:
  us-east: {}
  eu-west: {}

nodes:
  viewers_us:
    type: client
    dc: us-east
    rate:
      base: 200/s
      wave: {shape: sine, period: 24h, amplitude: 0.6, peak_at: 21:00}
    ops: {stream: 0.9, upload: 0.02, metadata: 0.08}
    sizes: {stream: 50MB, upload: 300MB, metadata: 2KB}   # per request
    keys: {zipf: 1.0, space: 10M}
    timeout: 10s

  viewers_eu:
    type: client
    dc: eu-west
    rate:
      base: 100/s
      wave: {shape: sine, period: 24h, amplitude: 0.6, peak_at: 21:00}
    ops: {stream: 0.9, upload: 0.02, metadata: 0.08}
    sizes: {stream: 50MB, upload: 300MB, metadata: 2KB}
    keys: {zipf: 1.0, space: 10M}
    timeout: 10s

  cdn_us: {type: cdn, dc: us-east, ttl: 24h, hit_ratio: che, origin: transcoded_store, capacity: 100GB/s}
  cdn_eu: {type: cdn, dc: eu-west, ttl: 24h, hit_ratio: che, origin: transcoded_store, capacity: 50GB/s}

  lb: {type: load_balancer, dc: us-east, policy: round_robin}

  api:
    type: service
    dc: us-east
    replicas: 6
    capacity: 2k/s
    latency: 1ms
    ops:
      metadata:
        - {to: metadata_cache, op: read}
        - {to: metadata_db, op: read, on: miss}
      upload:                                  # returns a presigned URL; bytes go to blob directly
        - {to: metadata_db, op: write}

  metadata_cache: {type: cache, dc: us-east, eviction: lru, keys: 5M, hit_ratio: che, strategy: cache_aside}
  metadata_db: {type: sql_db, dc: us-east, replicas: 3, capacity: 3k/s}

  original_store:                              # raw uploads (S3-like)
    type: blob_store
    dc: us-east
    capacity: 5GB/s
    then: [{to: transcode_q, op: publish}]

  transcode_q: {type: message_queue, dc: us-east, partitions: 32}

  transcoders:                                 # DAG: inspect → encode (several resolutions) → thumbnails → watermark
    type: worker_pool
    dc: us-east
    consumes: transcode_q
    workers: 400
    cost: 60s                                  # per 300 MB video
    then:
      - {to: transcoded_store, op: write, factor: 4}     # 4 output resolutions
      - {to: completion_q, op: publish}

  transcoded_store: {type: blob_store, dc: us-east, capacity: 10GB/s}

  completion_q: {type: message_queue, dc: us-east, partitions: 4}
  completion_handler:
    type: worker_pool
    dc: us-east
    consumes: completion_q
    workers: 4
    cost: 5ms
    then:
      - {to: metadata_db, op: write}
      - {to: metadata_cache, op: write}

links:
  - {from: viewers_us, to: cdn_us, dir: inout, ops: [stream]}
  - {from: viewers_eu, to: cdn_eu, dir: inout, ops: [stream]}
  - {from: cdn_us, to: transcoded_store, dir: inout}
  - {from: cdn_eu, to: transcoded_store, dir: inout}          # cross-DC, 150 ms default
  - {from: viewers_us, to: lb, dir: inout, ops: [metadata, upload]}
  - {from: viewers_eu, to: lb, dir: inout, ops: [metadata, upload]}
  - {from: lb, to: api, dir: inout}
  - {from: api, to: metadata_cache, dir: inout}
  - {from: api, to: metadata_db, dir: inout}
  - {from: viewers_us, to: original_store, ops: [upload]}     # presigned direct upload
  - {from: viewers_eu, to: original_store, ops: [upload]}
  - {from: original_store, to: transcode_q}
  - {from: transcode_q, to: transcoders}
  - {from: transcoders, to: transcoded_store}
  - {from: transcoders, to: completion_q}
  - {from: completion_q, to: completion_handler}
  - {from: completion_handler, to: metadata_db}
  - {from: completion_handler, to: metadata_cache}

slo:
  - {metric: p99_latency, node: viewers_eu, op: stream, max: 2s}
  - {metric: queue_lag, node: transcode_q, max: 30m}
  - {metric: utilization, node: cdn_us, max: 0.8}
```

- [ ] **Step 2: Write `scenarios/book/youtube/README.md`**

```markdown
# Video streaming / YouTube (ch. 14)

## Book design
- Upload: client gets a presigned URL from the API and uploads straight to original storage;
  a message triggers the transcoding DAG (inspection, encoding at several resolutions,
  thumbnails, watermarks); outputs land in transcoded storage; a completion handler updates
  metadata DB and cache.
- Streaming: adaptive bitrate from the CDN, which pulls from transcoded storage on miss.
- Cost: CDN is the bill; serve only popular videos from CDN, the long tail from origin.

## What the model captures
- Bytes, not just requests: 50 MB streams and 300 MB uploads against GB/s capacities.
- Two regions with a cross-DC origin fetch (150 ms) for European misses.
- Transcoding as a long-running worker pool (60 s per video, 400 workers → 6.7 videos/s
  ceiling) and the backlog it builds during the evening upload peak.
- Fan-out of one upload into four output renditions.

## What it abstracts away
- Adaptive bitrate selection, chunked upload resume, DRM.
- Error handling in the DAG (retry per stage).

## Things to try
- `transcoders.workers: 200` — `transcode_q` lag grows all day and never recovers.
- Lower `cdn_eu.capacity` or raise EU traffic — watch the origin link saturate.
- Add `{at: 21:00, kind: dc_down, dc: eu-west, for: 5m}` in `faults`.
```

- [ ] **Step 3: Validate, test, commit**

```bash
./build/gcc14-debug/simsys validate scenarios/book/youtube/scenario.yaml
ctest --preset gcc14-debug 2>&1 | tail -1
git add scenarios/book/youtube docs/scenario-schema.md
git commit -m "scenarios: youtube draft (ch. 14) — byte flows, two regions, transcoding DAG"
```
Schema additions: `then` on `blob_store` (upload completion triggers a message); byte capacities (`5GB/s`) are already accepted by `parse_rate` (Task 2).

### Task 14: Google Drive (ch. 15)

- [ ] **Step 1: Write `scenarios/book/google-drive/scenario.yaml`**

```yaml
# System Design Interview vol. 1, ch. 15 — Google Drive.
# Book numbers: 50M signed-up, 10M DAU, 10 GB free space, 2 uploads/user/day of ~500 KB
# → ~240 uploads/s, 10:1 read:write. Block servers split files into 4 MB blocks,
# compress, encrypt, dedup; cloud storage holds blocks; metadata DB holds the tree;
# notification service pushes sync events over long polling.
version: 1
name: google-drive
seed: 1
duration: 24h
step: 100ms
warmup: 5m

nodes:
  clients:
    type: client
    rate:
      base: 3k/s
      wave: {shape: diurnal, period: 24h, amplitude: 0.7, peak_at: 11:00}   # office hours
    ops: {download: 0.70, upload: 0.08, metadata: 0.20, sync_poll: 0.02}
    sizes: {download: 500KB, upload: 500KB, metadata: 1KB}
    keys: {zipf: 0.8, space: 500M}
    timeout: 30s

  lb: {type: load_balancer, policy: round_robin}

  api:
    type: service
    replicas: 8
    capacity: 2k/s
    latency: 1ms
    ops:
      metadata:
        - {to: metadata_cache, op: read}
        - {to: metadata_db, op: read, on: miss}
      upload:
        - {to: metadata_db, op: write}
        - {to: notify_q, op: publish}
      download:
        - {to: metadata_cache, op: read}
        - {to: block_servers, op: fetch}
      sync_poll:
        - {to: notification_svc, op: poll}

  block_servers:                               # chunk, compress, encrypt, dedup
    type: service
    replicas: 10
    capacity: 500/s
    latency: 20ms
    ops:
      upload:
        - {to: cloud_storage, op: write, factor: 0.5}      # dedup halves stored blocks
      fetch:
        - {to: cloud_storage, op: read}

  cloud_storage: {type: blob_store, capacity: 2GB/s}
  cold_storage: {type: blob_store, capacity: 200MB/s}

  metadata_cache: {type: cache, eviction: lru, keys: 50M, hit_ratio: che, strategy: cache_aside}
  metadata_db:
    type: sql_db
    replicas: 3
    shards: 4
    capacity: 4k/s
    replication_lag: 50ms

  notify_q: {type: message_queue, partitions: 8}
  notification_svc:
    type: worker_pool
    consumes: notify_q
    workers: 8
    cost: 1ms
    then: [{to: long_poll, op: push, factor: 2}]        # avg 2 other devices per user

  long_poll: {type: ws_gateway, replicas: 4, max_connections: 2M, heartbeat: 60s, fanout: 1, capacity: 20k/s}

  backup_workers:
    type: worker_pool
    consumes: notify_q
    workers: 2
    cost: 10ms
    then: [{to: cold_storage, op: write}]

links:
  - {from: clients, to: lb, dir: inout}
  - {from: lb, to: api, dir: inout}
  - {from: api, to: metadata_cache, dir: inout}
  - {from: api, to: metadata_db, dir: inout}
  - {from: api, to: notify_q}
  - {from: api, to: block_servers, dir: inout}
  - {from: clients, to: block_servers, dir: inout, ops: [upload]}   # uploads go to block servers directly
  - {from: block_servers, to: cloud_storage, dir: inout}
  - {from: notify_q, to: notification_svc}
  - {from: notify_q, to: backup_workers}
  - {from: notification_svc, to: long_poll}
  - {from: long_poll, to: clients}
  - {from: backup_workers, to: cold_storage}
  - {from: api, to: notification_svc, dir: inout, ops: [sync_poll]}

slo:
  - {metric: p99_latency, node: clients, op: download, max: 2s}
  - {metric: p99_latency, node: clients, op: metadata, max: 100ms}
  - {metric: drop_rate, max: 0.001}
```

- [ ] **Step 2: Write `scenarios/book/google-drive/README.md`**

```markdown
# Google Drive (ch. 15)

## Book design
- Block servers split files into 4 MB blocks, compress and encrypt them, and store only
  changed/unique blocks (delta sync + dedup) in cloud storage; cold storage for backups.
- Metadata DB (sharded, replicated) holds users, files, blocks, versions, devices;
  a metadata cache in front.
- Notification service pushes "file changed" to other devices via long polling.
- Upload flow and download flow are separate paths; offline backup queue decouples storage.

## What the model captures
- Office-hours diurnal shape with a 70% amplitude.
- Block-server CPU cost (20 ms per request) as a capacity limit distinct from storage bandwidth.
- Dedup halving stored bytes; one upload notifying two other devices on average.
- Two consumer groups on one queue (`notification_svc`, `backup_workers`).

## What it abstracts away
- Conflict resolution, versioning, delta sync granularity.
- Upload resumption and client-side chunk scheduling.

## Things to try
- `block_servers.replicas: 4` — uploads queue at 11:00.
- `metadata_db.shards: 1` — metadata reads fail the 100 ms SLO.
```

- [ ] **Step 3: Validate, test, commit**

```bash
./build/gcc14-debug/simsys validate scenarios/book/google-drive/scenario.yaml
ctest --preset gcc14-debug 2>&1 | tail -1
git add scenarios/book/google-drive docs/scenario-schema.md
git commit -m "scenarios: google drive draft (ch. 15) — block servers, dedup, two consumer groups"
```

### Task 15: Rate limiter lab (ch. 4)

- [ ] **Step 1: Write `scenarios/labs/rate-limiter/scenario.yaml`**

```yaml
# Lab: System Design Interview vol. 1, ch. 4 — Rate limiter.
# Compare algorithms under a bursty client. The same file is run with `--param
# nodes.limiter.algorithm=token_bucket|leaky_bucket|fixed_window|sliding_log|sliding_counter`.
version: 1
name: lab-rate-limiter
seed: 1
duration: 30m
step: 100ms
warmup: 1m

nodes:
  clients:
    type: client
    rate:
      base: 800/s
      spikes:
        - {at: 5m, for: 30s, factor: 5}
        - {at: 15m, for: 2m, factor: 2}
        - {at: 25m, for: 5s, factor: 20}
    ops: {call: 1.0}
    timeout: 1s

  limiter:
    type: rate_limiter
    algorithm: token_bucket
    limit: 1k/s
    burst: 500
    backend: counters

  counters: {type: cache, eviction: ttl, ttl: 1m, keys: 1M, hit_ratio: {fixed: 1.0}, capacity: 100k/s, latency: 0.2ms}

  api:
    type: service
    replicas: 2
    capacity: 600/s
    latency: 2ms
    advanced: {queue_limit: 200}

links:
  - {from: clients, to: limiter, dir: inout}
  - {from: limiter, to: counters, dir: inout}
  - {from: limiter, to: api, dir: inout}

slo:
  - {metric: utilization, node: api, max: 0.9}
  - {metric: p99_latency, node: clients, op: call, max: 50ms}
```

- [ ] **Step 2: Write `scenarios/labs/rate-limiter/README.md`**

```markdown
# Lab: rate limiter (ch. 4)

## Book design
- Algorithms: token bucket, leaky bucket, fixed window counter, sliding window log,
  sliding window counter. Each trades burst tolerance against memory and accuracy.
- Placement: API gateway / middleware; counters in Redis; rules in config, cached in workers.
- Distributed issues: race conditions (Lua scripts / sorted sets) and synchronization
  (centralised store), 429 responses with `X-Ratelimit-*` headers.

## What the model captures
- The token bucket as a fluid level: a 500-token burst passes untouched, longer bursts are
  shaved to 1k/s; leaky bucket smooths everything; fixed windows let 2x through at edges.
- The protected API (2 × 600/s) staying under 90% while the client spikes 5–20x.
- Counter store round trips as part of every request's latency.

## What it abstracts away
- Per-client keys (the limit here is aggregate), header semantics, rule loading.

## Things to try
- `simsys sweep --param nodes.limiter.algorithm=... --metric api.utilization` (phase 2).
- `burst: 5000` — the 30-second spike gets through and `api` turns red.
```

- [ ] **Step 3: Validate, test, commit**

```bash
./build/gcc14-debug/simsys validate scenarios/labs/rate-limiter/scenario.yaml
ctest --preset gcc14-debug 2>&1 | tail -1
git add scenarios/labs/rate-limiter docs/scenario-schema.md
git commit -m "scenarios: rate limiter lab (ch. 4) — spikes vs token bucket"
```

### Task 16: Consistent hashing lab (ch. 5)

- [ ] **Step 1: Write `scenarios/labs/consistent-hashing/scenario.yaml`**

```yaml
# Lab: System Design Interview vol. 1, ch. 5 — Consistent hashing.
# A load balancer hashes keys onto a ring of cache servers. Compare `vnodes: 1` (uneven
# partitions) with `vnodes: 150` (even), and watch what happens when a server is removed.
version: 1
name: lab-consistent-hashing
seed: 1
duration: 20m
step: 100ms
warmup: 1m

nodes:
  clients:
    type: client
    rate: {base: 40k/s}
    ops: {get: 0.9, set: 0.1}
    keys: {zipf: 0.9, space: 10M}
    timeout: 200ms

  ring:
    type: load_balancer
    policy: consistent_hash
    vnodes: 1                                   # try 150
    capacity: 200k/s
    latency: 0.05ms

  cache_servers:
    type: cache
    replicas: 4
    eviction: lru
    keys: 2M                                    # per server
    hit_ratio: che
    strategy: cache_aside
    capacity: 12k/s
    latency: 0.2ms

  db:
    type: sql_db
    replicas: 2
    capacity: 5k/s

links:
  - {from: clients, to: ring, dir: inout}
  - {from: ring, to: cache_servers, dir: inout}
  - {from: cache_servers, to: db, dir: inout}

slo:
  - {metric: utilization, node: cache_servers, max: 0.85}
  - {metric: utilization, node: db, max: 0.7}

faults:
  - {at: 10m, kind: node_down, node: cache_servers, for: 5m}    # one of the four replicas
```

- [ ] **Step 2: Write `scenarios/labs/consistent-hashing/README.md`**

```markdown
# Lab: consistent hashing (ch. 5)

## Book design
- Hash both servers and keys onto a ring; a key belongs to the first server clockwise.
- Adding/removing a server moves only the keys between it and its predecessor.
- Problems with few servers: uneven partition sizes and non-uniform key distribution.
  Fix: virtual nodes (e.g. 100–200 per server) — more even, at the cost of ring memory.

## What the model captures
- Load imbalance across the four cache servers as a function of `vnodes` (the standard
  deviation of partition sizes shrinks roughly with 1/sqrt(vnodes)); the hottest server's
  utilization is what the SLO checks.
- Removing one server at 10 m: its keys move to the next server(s) on the ring, that
  server's hit ratio drops (cold keys), the DB sees the misses.

## What it abstracts away
- The hash function, ring data structure and lookup (binary search).
- Replication across neighbours on the ring (see the KV store lab).

## Things to try
- `vnodes: 150` — the four servers sit within a few percent of each other.
- `replicas: 2` — with vnodes 1 the imbalance is dramatic.
```

- [ ] **Step 3: Validate, test, commit**

```bash
./build/gcc14-debug/simsys validate scenarios/labs/consistent-hashing/scenario.yaml
ctest --preset gcc14-debug 2>&1 | tail -1
git add scenarios/labs/consistent-hashing docs/scenario-schema.md
git commit -m "scenarios: consistent hashing lab (ch. 5) — vnodes and node removal"
```

### Task 17: Key-value store lab (ch. 6)

- [ ] **Step 1: Write `scenarios/labs/kv-store/scenario.yaml`**

```yaml
# Lab: System Design Interview vol. 1, ch. 6 — Key-value store (Dynamo-style).
# N=3 replicas, W=2, R=2 quorum, consistent hashing with virtual nodes, hinted handoff,
# vector clocks, gossip failure detection. Two faults: a node failure and a partition.
version: 1
name: lab-kv-store
seed: 1
duration: 30m
step: 100ms
warmup: 1m

datacenters:
  dc-a: {}
  dc-b: {}
  latency: [{between: [dc-a, dc-b], rtt: 40ms}]

nodes:
  clients:
    type: client
    dc: dc-a
    rate: {base: 30k/s}
    ops: {get: 0.7, put: 0.3}
    keys: {zipf: 0.9, space: 100M}
    sizes: {put: 1KB}
    timeout: 100ms

  coordinator:
    type: service
    dc: dc-a
    replicas: 4
    capacity: 15k/s
    latency: 0.2ms
    ops:
      get: [{to: store, op: read}]
      put: [{to: store, op: write}]

  store:
    type: kv_cluster
    dc: dc-a
    nodes: 8
    vnodes: 150
    replication: 3
    write_quorum: 2
    read_quorum: 2
    mode: ap                                    # try cp
    capacity: 10k/s                             # per node
    latency: 0.3ms

links:
  - {from: clients, to: coordinator, dir: inout}
  - {from: coordinator, to: store, dir: inout}

slo:
  - {metric: p99_latency, node: clients, op: put, max: 20ms}
  - {metric: utilization, node: store, max: 0.8}
  - {metric: drop_rate, max: 0.01}

faults:
  - {at: 10m, kind: node_down, node: store, for: 3m}            # one of eight nodes
  - {at: 20m, kind: partition, between: [dc-a, dc-b], for: 2m}
```

- [ ] **Step 2: Write `scenarios/labs/kv-store/README.md`**

```markdown
# Lab: key-value store (ch. 6)

## Book design
- Partition with consistent hashing; replicate to the next N distinct nodes on the ring.
- Tunable consistency: W + R > N gives strong consistency; W=1 fast writes; R=1 fast reads.
- Conflicts resolved with vector clocks; failures detected by gossip; temporary failures
  handled by sloppy quorum + hinted handoff; permanent ones by anti-entropy (Merkle trees).
- Write path: commit log → memtable → SSTable; read path: cache → bloom filter → SSTables.

## What the model captures
- Write amplification: every put costs N=3 replica writes; the quorum W=2 means the
  client waits for the second-fastest replica (order statistic of replica latencies).
- Per-node capacity × 8 nodes vs 30k/s × (0.7 + 0.3×3) replica operations.
- Node failure at 10 m: its share moves to neighbours (hinted handoff keeps writes flowing).
- Partition at 20 m: in `ap` mode the minority keeps serving (stale fraction rises); in `cp`
  mode requests that cannot reach a quorum are rejected (unavailable fraction rises).

## What it abstracts away
- Vector-clock semantics and conflict resolution, Merkle-tree anti-entropy, SSTable compaction,
  gossip timing (represented as `failover`-style delays).

## Things to try
- `write_quorum: 3, read_quorum: 1` and vice versa — latency moves between puts and gets.
- `mode: cp` — the partition shows up as drops instead of staleness.
- `nodes: 4` — the store goes red before any fault happens.
```

- [ ] **Step 3: Validate, test, commit**

```bash
./build/gcc14-debug/simsys validate scenarios/labs/kv-store/scenario.yaml
ctest --preset gcc14-debug 2>&1 | tail -1
git add scenarios/labs/kv-store docs/scenario-schema.md
git commit -m "scenarios: key-value store lab (ch. 6) — quorum, node loss, partition"
```

### Task 18: Unique ID generator lab (ch. 7)

- [ ] **Step 1: Write `scenarios/labs/unique-id/scenario.yaml`**

```yaml
# Lab: System Design Interview vol. 1, ch. 7 — Unique ID generator in distributed systems.
# Compare a single ticket server (auto-increment DB) with Twitter snowflake generators.
# Book requirement: 10,000 IDs/s, 64-bit, sortable by time.
version: 1
name: lab-unique-id
seed: 1
duration: 10m
step: 100ms
warmup: 30s

nodes:
  services:
    type: client
    rate:
      base: 10k/s
      spikes: [{at: 5m, for: 1m, factor: 3}]
    ops: {next_id: 1.0}
    timeout: 50ms

  ticket_server:                               # try: replicas 1, capacity 8k/s (a single MySQL)
    type: id_generator
    algorithm: ticket_server
    replicas: 1
    capacity: 8k/s
    latency: 1ms
    advanced: {queue_limit: 1000}

  snowflake:                                   # 4 generators, each 4,096 IDs per ms by design
    type: id_generator
    algorithm: snowflake
    replicas: 4
    capacity: 4M/s
    latency: 0.01ms

links:
  - {from: services, to: ticket_server, dir: inout, weight: 1}
  - {from: services, to: snowflake, dir: inout, weight: 0}       # set to 1 and ticket_server to 0 to switch

slo:
  - {metric: p99_latency, node: services, op: next_id, max: 10ms}
  - {metric: drop_rate, max: 0}
```

- [ ] **Step 2: Write `scenarios/labs/unique-id/README.md`**

```markdown
# Lab: unique ID generator (ch. 7)

## Book design
- Options: multi-master replication (auto-increment by k), UUID, ticket server (one
  auto-increment DB — single point of failure), Twitter snowflake (timestamp | datacenter |
  machine | sequence; 4,096 IDs per ms per machine, sortable by time).
- Snowflake chosen: no coordination, time-sortable, scales with machines; needs clock sync (NTP).

## What the model captures
- The ticket server as one capacity-limited node (8k/s) facing 10k/s with a 3x spike:
  queue, latency, then drops.
- Snowflake as four independent generators whose capacity is effectively unlimited.
- Link `weight` as the switch between the two designs (phase 2 supports `sweep` over it).

## What it abstracts away
- Clock skew and NTP, ID layout, section length tuning.

## Things to try
- Swap the weights to route to `snowflake`: the SLO passes with p99 ≈ link latency.
- `ticket_server.replicas: 2` (multi-master, step 2): capacity doubles, but note the book's
  warning about scaling with datacenters.
```

- [ ] **Step 3: Validate, test, commit**

```bash
./build/gcc14-debug/simsys validate scenarios/labs/unique-id/scenario.yaml
ctest --preset gcc14-debug 2>&1 | tail -1
git add scenarios/labs/unique-id docs/scenario-schema.md
git commit -m "scenarios: unique ID generator lab (ch. 7) — ticket server vs snowflake"
```

---

### Task 19: Reconcile the schema with the drafts and close phase 0

**Files:**
- Modify: `docs/scenario-schema.md`, `README.md`, `docs/superpowers/specs/2026-10-27-system-sim-design.md` (§8 only if the grammar changed)

- [ ] **Step 1: List every key used across the drafts and compare with the schema**

```bash
cd /home/frtlx/liorkoren77/system-sim
grep -rhoE '^\s*[a-z_]+:' scenarios --include=scenario.yaml | tr -d ' :' | sort | uniq -c | sort -rn
grep -rhoE '\{[^}]*\}' scenarios --include=scenario.yaml | grep -oE '[a-z_]+:' | tr -d ':' | sort | uniq -c | sort -rn
```
Every key that appears must be in `docs/scenario-schema.md` (top level, node common, node type, step, link, slo, fault). Add any that are missing: known additions from the drafts are `then` on `cache`, `frontier`, `blob_store`; `sizes` on `client`; `politeness` on `external_sink`; `weight` on links; `hit_ratio: {fixed: h}` form; byte rates; `shape: diurnal`.

- [ ] **Step 2: Update the schema document**

Edit `docs/scenario-schema.md` so each of the keys above is listed in the correct table, and change the heading to `# Scenario schema v1 (validated against 12 drafts)`.

- [ ] **Step 3: Update `README.md` status**

Replace the line starting `Status: design phase.` with:
```markdown
Status: phase 0 complete — the project builds with GCC 14 named modules, `simsys validate`
checks scenario files, and drafts of all 8 book systems and 4 labs live under
[`scenarios/`](scenarios/). The engine arrives in phase 1.
```
Add after the Toolchain section:
```markdown
## Build

```bash
cmake --preset gcc14-debug && cmake --build --preset gcc14-debug && ctest --preset gcc14-debug
./build/gcc14-debug/simsys validate scenarios/book/news-feed/scenario.yaml
```
```

- [ ] **Step 4: Run the full test suite and CI**

```bash
ctest --preset gcc14-debug 2>&1 | tail -2
git add -A
git commit -m "docs: schema v1 reconciled with the 12 scenario drafts; README build instructions"
git push origin main
gh run watch --exit-status $(gh run list --limit 1 --json databaseId --jq '.[0].databaseId')
```
Expected: all tests pass locally and CI is green.

---

## Self-review notes

- **Spec coverage (phase 0 scope):** toolchain spike → Tasks 1, 2, 6; 12 scenarios → Tasks 7–18; schema adjusted until they fit → Task 3 draft + Task 19 reconcile; conventions → comments present in every code block; CI → Task 5.
- **Type consistency:** `simsys::parse_duration` / `parse_rate` / `SimTime` / `Rate` / `ParseError` (Task 2) are the names used by `simsys::config` (Task 4); `simsys::config::load`, `Scenario{name, version, duration, nodes, links}`, `ConfigError{file, line, column, message}` are the names used by the CLI (Task 4) and the fixture test (Task 5).
- **Deliberate limits:** the validator does not check per-type keys, enum values or that `ops` shares sum to 1; that lands with the typed config in phase 1, where each node type owns its schema.
