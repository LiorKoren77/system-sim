// SLOs per window, `over:` (plan 2c Task 14; book ch. 3: non-functional requirements are checked
// against the load the system really has, and a day's average hides the hour that breaks it).
//
// Model under test: an SLO with `over: <duration>` is evaluated on every window of that length,
// the windows aligned from the END OF WARMUP and stepping window by window (warmup, warmup +
// over, warmup + 2 over, ...); a last window shorter than `over` is still evaluated; a window in
// which nothing was offered is skipped; the WORST window decides, and the report says which one
// (table: `(worst 10m from 00:20)`, JSON: `window_start_s`, seconds from the start of the run).
// An SLO without `over` is checked over the whole run after warmup, exactly as before.
//
// Ignored on purpose: windows that overlap (a sliding window would find a worse one than the
// aligned grid does: a spike straddling two windows is split between them, see the warmup test).
//
// Every number below is derived by hand in the comment above its test. Rate to utilization: a
// service of 2k/s with a 1 ms service time has c = ceil(2000 x 0.001) = 2 servers, so
// rho = lambda x s / c = lambda / 2000: 1k/s is 0.5 and the 1.8x spike's 1.8k/s is 0.9.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>  // getpid

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.config;
import simsys.core;
import simsys.engine;
import simsys.graph;
import simsys.model;
import simsys.report;

using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using simsys::engine::Simulation;
using simsys::graph::Graph;
using simsys::model::Scenario;
using simsys::model::SloMetric;
using simsys::report::SloStatus;

namespace {

// ---- Loading a scenario from text -------------------------------------------------------

// A scenario file in the system temp directory that removes itself (RAII: also when a REQUIRE
// unwinds the test). The process id is part of the name so two test processes never collide.
struct TempFile {
  std::filesystem::path path;
  explicit TempFile(std::filesystem::path p) : path(std::move(p)) {}
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  ~TempFile() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
};

Scenario load_yaml(std::string_view name, std::string_view yaml) {
  const TempFile file{std::filesystem::temp_directory_path() /
                      (std::to_string(::getpid()) + "_" + std::string{name})};
  std::ofstream(file.path) << yaml;
  // C++23 std::expected<T, E>: the loader returns a Scenario or a ConfigError, and the caller
  // must look at which. It replaces exceptions and error codes; the error carries the file,
  // line and message, so a test that cannot load its scenario says why.
  auto loaded = simsys::config::load(file.path.string());
  if (!loaded)
    FAIL(loaded.error().file << ":" << loaded.error().line << ": " << loaded.error().message);
  return std::move(*loaded);
}

// One client `u` (one op, `get`) into one service `s` (2k/s, 1 ms, a zero-latency link). `rate` is
// the text of the client's `rate:` map and `slos` the lines of the `slo:` list.
Scenario one_service(std::string_view timing, std::string_view rate, std::string_view slos) {
  return load_yaml("simsys_windows.yaml",
                   "version: 1\nname: windows\n" + std::string{timing} +
                       "nodes:\n"
                       "  u: {type: client, rate: " +
                       std::string{rate} +
                       ", ops: {get: 1}}\n"
                       "  s: {type: service, capacity: 2k/s, latency: 1ms}\n"
                       "links:\n  - {from: u, to: s, dir: inout, latency: 0s}\n"
                       "slo:\n" +
                       std::string{slos});
}

// The hour of the plan: 1k/s with a 1.8x spike from 00:20 for 10 minutes, step 1 s.
constexpr std::string_view spike_hour_timing = "duration: 1h\nstep: 1s\n";
constexpr std::string_view spike_rate =
    "{base: 1k/s, spikes: [{at: 00:20, for: 10m, factor: 1.8}]}";

// ---- Running it and reading the public report ------------------------------------------

struct Verdicts {
  std::vector<simsys::report::SloResult> results;
  std::vector<simsys::report::OpSummary> ops;
  std::string table;
  std::string json;
};

Verdicts run(const Scenario& s) {
  const auto g = Graph::build(s);
  if (!g) FAIL(g.error().message);
  simsys::report::Summary summary{*g, s.warmup};
  auto sim = Simulation::create(*g);
  if (!sim) FAIL(sim.error().message);
  // C++20 designated initializers: `.duration = ...` names each member being set, so the call
  // reads like the option list it is; replaces positional braces that break when members move.
  sim->run({.duration = s.duration, .step = s.step}, [&](const auto& r) { summary.observe(r); });
  auto results = simsys::report::evaluate_slos(summary, s.slos);
  auto table = simsys::report::render_table(summary, results, /*color=*/false);
  auto json = simsys::report::render_json(summary, results);
  return {std::move(results), summary.ops(), std::move(table), std::move(json)};
}

// The n-th (from 0) entry of the JSON's "slo" array: render_json prints each as one line that
// starts with `{"metric":`.
std::string slo_json_line(const std::string& json, std::size_t n) {
  std::istringstream lines{json};
  std::string line;
  std::size_t seen = 0;
  while (std::getline(lines, line)) {
    if (line.find("{\"metric\":") == std::string::npos) continue;
    if (seen++ == n) return line;
  }
  FAIL("the JSON has no slo entry " << n << ":\n" << json);
  return {};
}

// The number after `"key": ` in `line`; empty when the key is absent or null.
std::optional<double> number_of(const std::string& line, std::string_view key) {
  const std::string field = "\"" + std::string{key} + "\": ";
  const auto at = line.find(field);
  if (at == std::string::npos) return std::nullopt;
  const auto value = line.substr(at + field.size());
  // C++20 std::string::starts_with: a prefix test without find(...) == 0 or compare().
  if (value.starts_with("null")) return std::nullopt;
  return std::stod(value);
}

// The n-th verdict, in the order the scenario lists its SLOs.
const simsys::report::SloResult& result_at(const Verdicts& v, std::size_t n) {
  REQUIRE(v.results.size() > n);
  return v.results[n];
}

}  // namespace

// ---- The plan's hour: 1k/s, a 1.8x spike for 10 minutes from 00:20 -----------------------
//
// Utilization of `s`: 0.5 outside the spike, 0.9 inside. Whole hour: (0.5 x 50 + 0.9 x 10) / 60 =
// 34 / 60 = 0.5667. This one passes today: an SLO without `over` must not change.
TEST_CASE("an slo without over still sees the whole hour: utilization 0.5667, no window named",
          "[theory]") {
  const auto s =
      one_service(spike_hour_timing, spike_rate, "  - {metric: utilization, node: s, max: 0.8}\n");
  const auto v = run(s);
  const auto& whole = result_at(v, 0);
  REQUIRE(whole.status == SloStatus::pass);
  REQUIRE(whole.observed.has_value());
  REQUIRE_THAT(*whole.observed, WithinRel(34.0 / 60.0, 1e-9));
  // Chosen: the key is ABSENT (not null) from an SLO that has no `over`.
  // C++23 std::string::contains: a substring test that replaces `find(x) != npos`, which reads
  // as a position search when all that is meant is "is it there".
  REQUIRE_FALSE(slo_json_line(v.json, 0).contains("window_start_s"));
  REQUIRE_FALSE(v.table.contains("worst"));
}

// over: 10m on a 1 h run, windows at 00:00, 00:10, ..., 00:50. The spike is exactly the window
// 00:20-00:30: all ten minutes at 0.9. The other five windows are 0.5. The worst is 0.9, which
// fails max 0.8 although the whole hour (0.5667) passes it: the hour the average hid.
TEST_CASE("over 10m observes the spike window, 0.9, and fails max 0.8 where the whole hour passes",
          "[theory]") {
  const auto s = one_service(spike_hour_timing, spike_rate,
                             "  - {metric: utilization, node: s, max: 0.8}\n"
                             "  - {metric: utilization, node: s, max: 0.8, over: 10m}\n");
  const auto v = run(s);
  REQUIRE(result_at(v, 0).status == SloStatus::pass);
  const auto& windowed = result_at(v, 1);
  REQUIRE(windowed.observed.has_value());
  REQUIRE_THAT(*windowed.observed, WithinRel(0.9, 1e-9));
  REQUIRE(windowed.status == SloStatus::fail);
  // 00:20 is 20 x 60 = 1200 s into the run.
  const auto start = number_of(slo_json_line(v.json, 1), "window_start_s");
  REQUIRE(start.has_value());
  REQUIRE(*start == 1200.0);
  REQUIRE_FALSE(slo_json_line(v.json, 0).contains("window_start_s"));
}

// over: 15m, windows at 00:00, 00:15, 00:30, 00:45 (warmup is 0, so they start at 0 and step by
// 15 m: not at the spike's 00:20). The spike (00:20-00:30) falls into two of them:
//   00:15-00:30: 5 min at 0.5 + 10 min at 0.9 -> (0.5 x 5 + 0.9 x 10) / 15 = 11.5 / 15 = 0.7667
//   00:30-00:45: no spike -> 0.5
// so the worst is the 00:15 window, 0.7667, which passes max 0.8. An aligned grid sees a spike
// diluted when it straddles windows: a 15 m window shows less than the 10 m one (0.9).
TEST_CASE(
    "over 15m observes 0.7667 in the window 00:15 to 00:30, which the grid cuts the spike into",
    "[theory]") {
  const auto s = one_service(spike_hour_timing, spike_rate,
                             "  - {metric: utilization, node: s, max: 0.8, over: 15m}\n");
  const auto v = run(s);
  const auto& windowed = result_at(v, 0);
  REQUIRE(windowed.observed.has_value());
  REQUIRE_THAT(*windowed.observed, WithinRel(11.5 / 15.0, 1e-9));
  REQUIRE(windowed.status == SloStatus::pass);
  const auto start = number_of(slo_json_line(v.json, 0), "window_start_s");
  REQUIRE(start.has_value());
  REQUIRE(*start == 900.0);
}

// An `over` as long as the observed run is one window starting at the end of warmup (here 0): it
// is the whole-run SLO spelled out, 0.5667, and the report still names the window it used.
TEST_CASE("over equal to the run is one window: the whole hour's 0.5667, from 0", "[theory]") {
  const auto s = one_service(spike_hour_timing, spike_rate,
                             "  - {metric: utilization, node: s, max: 0.8, over: 1h}\n");
  const auto v = run(s);
  const auto& windowed = result_at(v, 0);
  REQUIRE(windowed.observed.has_value());
  REQUIRE_THAT(*windowed.observed, WithinRel(34.0 / 60.0, 1e-9));
  const auto start = number_of(slo_json_line(v.json, 0), "window_start_s");
  REQUIRE(start.has_value());
  REQUIRE(*start == 0.0);
}

// ---- Alignment: from the end of warmup, not from t = 0 ------------------------------------
//
// Warmup 5 m, over 10 m: the windows are 00:05-00:15, 00:15-00:25, 00:25-00:35, ... (observed
// from 5 m, stepping 10 m). A spike of 1.8x from 00:20 for 8 minutes (00:20-00:28), so:
//   00:05-00:15: 0.5
//   00:15-00:25: 5 min at 0.5 + 5 min at 0.9 -> (2.5 + 4.5) / 10 = 0.7
//   00:25-00:35: 3 min at 0.9 + 7 min at 0.5 -> (2.7 + 3.5) / 10 = 0.62
//   later windows: 0.5
// The worst is 00:15-00:25 with 0.7, starting at 900 s. Had the windows been aligned from
// t = 0 (00:00, 00:10, 00:20, ...), the window 00:20-00:30 would hold all 8 spike minutes:
// (0.9 x 8 + 0.5 x 2) / 10 = 0.82, starting at 1200 s: the test tells the two rules apart.
TEST_CASE("windows are aligned from the end of warmup: with 5m warmup the worst starts at 00:15",
          "[theory]") {
  const auto s = one_service("duration: 1h\nstep: 1s\nwarmup: 5m\n",
                             "{base: 1k/s, spikes: [{at: 00:20, for: 8m, factor: 1.8}]}",
                             "  - {metric: utilization, node: s, max: 0.8, over: 10m}\n");
  const auto v = run(s);
  const auto& windowed = result_at(v, 0);
  REQUIRE(windowed.observed.has_value());
  REQUIRE_THAT(*windowed.observed, WithinRel(0.7, 1e-9));
  REQUIRE(windowed.status == SloStatus::pass);
  const auto start = number_of(slo_json_line(v.json, 0), "window_start_s");
  REQUIRE(start.has_value());
  REQUIRE(*start == 900.0);
}

// ---- A latency percentile per window ---------------------------------------------------------
//
// p99 of the client's response, a lognormal fitted to the mean and variance of the response time
// of the window's requests (the fit every report uses). At 1.8k/s the service is an M/M/2 queue
// at rho = 0.9 (a = lambda / mu = 1.8 offered erlangs, c = 2, mu = 1000/s per server):
//   Erlang C: C = (a^2 / 2!) / (1 - rho) / (1 + a + (a^2 / 2!) / (1 - rho)) = 16.2 / 19 = 0.85263
//   wait Wq   = C / (c mu - lambda) = 0.85263 / 200 = 4.263 ms; response mean m = 1 ms + Wq = 5.263
//   ms variance  = s^2 + C (2 - C) / (c mu - lambda)^2 = 1e-6 + 2.4457e-5 = 2.5457e-5 s^2 lognormal
//   sigma^2 = ln(1 + variance / m^2) = ln(1.91897) = 0.65181, sigma = 0.80735, mu_ln = ln m -
//   sigma^2 / 2 = -5.57292, p99 = exp(mu_ln + 2.32635 sigma) = 24.86 ms
// (the engine's own number: 24.853 ms). Over the whole hour the 1.8k/s minutes are mixed with
// 50 minutes at 1k/s (p99 6.158 ms), and the pooled p99 is 15.48 ms.
//
// With max 20 ms the whole hour passes (15.48 ms) and the spike window fails (24.85 ms). "Equals
// a run restricted to the spike's load": the same scenario at a constant 1.8k/s for 10 minutes,
// whose only difference from the window is the steps outside it. The fluid steps are
// independent of one another while no queue builds (rho < 1), so the two p99 agree to rounding;
// 1e-6 relative leaves room for a different summation order in the window's accumulators.
TEST_CASE("a p99 slo over 10m observes the spike window's p99, 24.85 ms, not the hour's 15.48 ms",
          "[theory]") {
  const auto hour = run(one_service(spike_hour_timing, spike_rate,
                                    "  - {metric: p99_latency, node: u, max: 20ms}\n"
                                    "  - {metric: p99_latency, node: u, max: 20ms, over: 10m}\n"));
  const auto& whole = result_at(hour, 0);
  const auto& windowed = result_at(hour, 1);
  REQUIRE(whole.observed.has_value());
  REQUIRE(windowed.observed.has_value());
  REQUIRE_THAT(*whole.observed, WithinRel(0.01548, 1e-3));
  REQUIRE(whole.status == SloStatus::pass);
  REQUIRE(*windowed.observed > *whole.observed);
  REQUIRE_THAT(*windowed.observed, WithinRel(0.02486, 1e-3));
  REQUIRE(windowed.status == SloStatus::fail);
  const auto start = number_of(slo_json_line(hour.json, 1), "window_start_s");
  REQUIRE(start.has_value());
  REQUIRE(*start == 1200.0);

  const auto restricted = run(one_service("duration: 10m\nstep: 1s\n", "{base: 1.8k/s}",
                                          "  - {metric: drop_rate, max: 1}\n"));
  REQUIRE(restricted.ops.size() == 1);
  REQUIRE_THAT(*windowed.observed, WithinRel(restricted.ops[0].p99_s, 1e-6));
}

// ---- The last window may be shorter than over ----------------------------------------------
//
// A 25 minute run with over: 10m has windows 00:00-00:10, 00:10-00:20 and a last one, 00:20-00:25,
// only 5 minutes long. The load is 1k/s (rho 0.5) with a 1.8x spike from 00:20 (to the end of the
// run): the first two windows are 0.5, the last is 0.9, so the SLO fails ONLY in the last window.
//   whole run:                 (0.5 x 20 + 0.9 x 5) / 25 = 14.5 / 25 = 0.58
//   last window left out:      0.5, would pass
//   last window merged into the one before (00:10-00:25): (0.5 x 10 + 0.9 x 5) / 15 = 0.6333,
//   passes
// Only evaluating the short window as it is gives 0.9 and a failure, from 00:20 = 1200 s.
TEST_CASE("a last window shorter than over is evaluated: 5 minutes at 0.9 fail a 25 minute run",
          "[theory]") {
  const auto s = one_service("duration: 25m\nstep: 1s\n",
                             "{base: 1k/s, spikes: [{at: 00:20, for: 5m, factor: 1.8}]}",
                             "  - {metric: utilization, node: s, max: 0.8, over: 10m}\n");
  const auto v = run(s);
  const auto& windowed = result_at(v, 0);
  REQUIRE(windowed.observed.has_value());
  REQUIRE_THAT(*windowed.observed, WithinRel(0.9, 1e-9));
  REQUIRE(windowed.status == SloStatus::fail);
  const auto start = number_of(slo_json_line(v.json, 0), "window_start_s");
  REQUIRE(start.has_value());
  REQUIRE(*start == 1200.0);
}

// ---- A window with nothing offered is skipped --------------------------------------------------
//
// The client offers 0/s for the first 10 minutes, then 1k/s (a trace with a step at 10 m). With
// over: 10m the first window offered nothing and has no response time to bound, so it is skipped;
// the worst of the windows left is the second, at 1k/s: an M/M/2 queue at rho 0.5, a = 1,
//   C = (a^2 / 2) / (1 - rho) / (1 + a + (a^2 / 2) / (1 - rho)) = 1 / 3, Wq = (1/3) / 1000 = 0.3333
//   ms, mean = 1.3333 ms, variance = 1e-6 + (1/3)(5/3) / 1e6 = 1.5556e-6 s^2, sigma^2 = ln(1
//   + 1.5556e-6 / 1.7778e-6) = ln 1.875 = 0.62861, mu_ln = -6.93438, p99 = exp(-6.93438 + 2.32635 x
//   0.79285) = 6.158 ms.
// This test can only pin which window is reported (600 s, not 0) and its value: an empty window
// evaluated as if it had traffic would not beat the loaded one on p99 here (the idle p99 of this
// service is 4.9 ms), so the next test pins the skip itself, on a run in which nothing is offered.
TEST_CASE("a window in which the client offered nothing is not the worst: the report names 00:10",
          "[theory]") {
  const auto s = one_service("duration: 20m\nstep: 1s\n",
                             "{points: [[0s, 0/s], [10m, 0/s], [10m, 1k/s], [20m, 1k/s]]}",
                             "  - {metric: p99_latency, node: u, max: 50ms, over: 10m}\n");
  const auto v = run(s);
  const auto& windowed = result_at(v, 0);
  REQUIRE(windowed.observed.has_value());
  REQUIRE_THAT(*windowed.observed, WithinRel(0.006158, 1e-3));
  REQUIRE(windowed.status == SloStatus::pass);
  const auto start = number_of(slo_json_line(v.json, 0), "window_start_s");
  REQUIRE(start.has_value());
  REQUIRE(*start == 600.0);
}

// Nothing offered in any window: every window is skipped, so the SLO has nothing to evaluate and
// is skipped itself (status `skipped`, no observed value, the table's `-`), never a pass on an
// idle service's 4.9 ms. (Without `over` the whole-run SLO of this scenario reports PASS 4.9ms
// today: an idle p99 is the service time's own; that is the behaviour `over` must not inherit.)
TEST_CASE("an slo whose every window offered nothing is skipped, with nothing observed",
          "[theory]") {
  const auto s = one_service("duration: 10m\nstep: 1s\n", "{points: [[0s, 0/s], [10m, 0/s]]}",
                             "  - {metric: p99_latency, node: u, max: 50ms, over: 5m}\n");
  const auto v = run(s);
  const auto& windowed = result_at(v, 0);
  REQUIRE(windowed.status == SloStatus::skipped);
  REQUIRE_FALSE(windowed.observed.has_value());
  REQUIRE_FALSE(number_of(slo_json_line(v.json, 0), "window_start_s").has_value());
}

// ---- The shipped url-shortener: the hour the whole-day SLOs cannot see -------------------------
//
// The scenario runs 24 h at 1 s steps with 2 m of warmup (about 6 s in Debug), so the windows of
// `over: 1h` start at 00:02, 01:02, ... (aligned from the end of warmup, see above): the hour
// around the 14:00 peak is 13:02-14:02, with the peak 2 minutes before its end.
//
// The load is 12.8k/s x (1 + 0.5 cos(2 pi (t - 14:00) / 24 h)), and every copy of url_db is
// linear in it (reads at the cache's miss ratio, writes at cost 5; nothing is capped), so
//   rho(t) = rho_peak x (1 + 0.5 cos(w u)) / 1.5,  u = t - 14:00,  w = 2 pi / 1440 per minute.
// Mean of cos(w u) over u in [-58, +2] minutes: (sin(2 w) + sin(58 w)) / (60 w) =
// (0.008726 + 0.250390) / 0.261799 = 0.98979, so the window's mean load is
// (1 + 0.5 x 0.98979) / 1.5 = 0.99660 of the peak's. The next hour, 14:02-15:02, gives
// (sin(62 w) - sin(2 w)) / (60 w) = 0.98704, a little less, and every earlier one less still:
// the worst hour is the one starting at 13:02 = 13 x 3600 + 120 = 47520 s.
//
// Shipped (3 shards): the README's busiest copy is a replica at 3.228k of 5k units/s at the
// peak, rho_peak = 0.6455, so the worst hour is 0.6455 x 0.9966 = 0.6433: passes max 0.9
// with room. With `shards: 2` the replicas are at 4.841k of 5k, rho_peak = 0.9683 (README: 96.8 %),
// so the worst hour is 0.9683 x 0.9966 = 0.9650, above 0.9: it fails. (The plan said about
// 0.966, the peak instant's 96.8 % shaded by the hour's average; the arithmetic gives 0.965, so
// the check allows 0.965 +- 0.002.) The whole-day mean of the same copy is only 0.6455, which is
// why the day-long SLOs never saw this.
namespace {
std::string url_shortener_text() {
  std::ifstream file{std::string{SIMSYS_SCENARIOS_DIR} + "/book/url-shortener/scenario.yaml"};
  REQUIRE(file.good());
  std::ostringstream text;
  text << file.rdbuf();
  return text.str();
}

// `from` replaced by `to`, which must occur in `text` exactly once (the scenario changed under
// this test otherwise, and the test should say so rather than check the wrong thing).
std::string replaced_once(std::string text, std::string_view from, std::string_view to) {
  const auto at = text.find(from);
  REQUIRE(at != std::string::npos);
  REQUIRE(text.find(from, at + 1) == std::string::npos);
  text.replace(at, from.size(), to);
  return text;
}

// The shipped scenario with the hourly utilization SLO put FIRST in its slo list (so it is entry 0
// whether or not the shipped file carries the same SLO by then), and `shards` as given.
Verdicts url_shortener_with_hourly_slo(int shards) {
  auto text =
      replaced_once(url_shortener_text(), "\nslo:\n",
                    "\nslo:\n  - {metric: utilization, node: url_db, max: 0.9, over: 1h}\n");
  text = replaced_once(std::move(text), "shards: 3", "shards: " + std::to_string(shards));
  return run(load_yaml("simsys_url_windows.yaml", text));
}
}  // namespace

TEST_CASE(
    "the shipped url-shortener passes a utilization SLO of 0.9 over 1h: its worst hour is 0.643",
    "[theory]") {
  const auto v = url_shortener_with_hourly_slo(3);
  const auto& hourly = result_at(v, 0);
  REQUIRE(hourly.slo.metric == SloMetric::utilization);
  REQUIRE(hourly.observed.has_value());
  REQUIRE_THAT(*hourly.observed, WithinAbs(0.6433, 0.003));
  REQUIRE(hourly.status == SloStatus::pass);
  const auto start = number_of(slo_json_line(v.json, 0), "window_start_s");
  REQUIRE(start.has_value());
  REQUIRE(*start == 47520.0);
  REQUIRE(v.table.contains("(worst 1h from 13:02)"));
}

TEST_CASE("url-shortener with 2 shards on url_db fails the 1h utilization SLO at about 0.965",
          "[theory]") {
  const auto v = url_shortener_with_hourly_slo(2);
  const auto& hourly = result_at(v, 0);
  REQUIRE(hourly.observed.has_value());
  REQUIRE_THAT(*hourly.observed, WithinAbs(0.965, 0.002));
  REQUIRE(hourly.status == SloStatus::fail);
  const auto start = number_of(slo_json_line(v.json, 0), "window_start_s");
  REQUIRE(start.has_value());
  REQUIRE(*start == 47520.0);
}
