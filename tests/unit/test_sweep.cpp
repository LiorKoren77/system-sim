// `simsys sweep` (plan 2c Task 15): run one scenario over a range of one key, one simulation
// per point, the points in parallel. Properties of the pieces: the `--param` grammar
// (`<dot.path>=<from>..<to>:<n>`), the `--metric` names, the loader's `Override`, the sweep
// itself (values pinned against M/M/1 on the tutorial's hello scenario) and the two renderings.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <charconv>
#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// C++20 `import`: brings a named module's exported names into this translation unit without
// textual inclusion (the compiler reads the module's compiled interface). It replaces
// `#include` plus include guards; placed after the #includes because GCC 14 reports
// redefinitions when a std header is included after a module that included it itself.
import simsys.config;
import simsys.core;
import simsys.model;
import simsys.sweep;

using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using simsys::Rate;
using simsys::config::Override;
using simsys::sweep::parse_metric;
using simsys::sweep::parse_param;
using simsys::sweep::render_sweep;
using simsys::sweep::run_sweep;
using simsys::sweep::SweepFormat;
using simsys::sweep::SweepMetric;
using simsys::sweep::SweepParam;
using simsys::sweep::SweepRow;

namespace {

// The scenarios directory is a compile definition of the test target (see tests/CMakeLists.txt).
const std::string hello_path = std::string{SIMSYS_SCENARIOS_DIR} + "/tutorial/hello/scenario.yaml";

// ---- hello, by hand -----------------------------------------------------------------------
// hello: `users` (one op, `ping`) -> `api`, a service with capacity 1000/s and latency 1 ms,
// over a link in both directions. The client's rate is the swept key, nodes.users.rate.base.
//
// The api is an M/M/1 queue (one server, Poisson arrivals, exponential service; the loader's
// default concurrency is max(1, ceil(capacity x latency)) = ceil(1000 x 0.001) = 1). Its mean
// time in system is the textbook W = s / (1 - rho), with the service time s = 1 ms and the
// utilization rho = lambda / mu = lambda / 1000/s. The client sees W plus the round trip on the
// link, 0.5 ms (intra_dc_rtt / 2 = 0.25 ms each way, as `simsys explain hello`
// prints: `one_way=250us`). So, for lambda = 100, 500 and 900 per second:
//
//     lambda   rho    W = 1 ms / (1 - rho)     + 0.5 ms     what the client sees
//     100/s    0.1    1 / 0.9 = 1.1111 ms                   1.6111 ms
//     500/s    0.5    1 / 0.5 = 2 ms                        2.5 ms
//     900/s    0.9    1 / 0.1 = 10 ms                       10.5 ms
//
// What the engine gives, run through `simsys run --report json` on the same file at these three
// rates (2026-10-29): mean_s 0.0016111111111111155, 0.0025, 0.010499999999999995, and the api's
// mean_rho 0.09999999999999763, 0.5, 0.9000000000000044. The plan's "1.61 ms" is the rounding of
// 1.6111 ms; the model agrees with the closed form to 1e-14, so the tests below hold it to a
// relative 1e-9, which only the arithmetic of a different model could break.
//
// Units: a SweepRow holds SI base units (seconds for a latency, a plain ratio for a
// utilization), like report::SloResult::observed. The table prints a latency humanly ("1.61 ms").
constexpr double rel_tolerance = 1e-9;
constexpr double mean_100 = 1.0 / 900.0 + 0.0005;  // 1.6111.. ms: s / (1 - 0.1) + 0.5 ms, in s
constexpr double mean_500 = 0.0025;
constexpr double mean_900 = 0.0105;

SweepParam hello_rate_sweep() {
  // C++20 designated initializers: members named at the point of use, in declaration order.
  // They replace positional braces, where a reader must remember which string is the path and
  // which the values; here the three points are written out so these tests do not depend on
  // parse_param working.
  return SweepParam{.path = "nodes.users.rate.base", .values = {"100/s", "500/s", "900/s"}};
}

std::vector<SweepMetric> hello_metrics() {
  const auto latency = parse_metric("users.ping.mean");
  const auto utilization = parse_metric("api.mean_rho");
  REQUIRE(latency.has_value());
  REQUIRE(utilization.has_value());
  return {*latency, *utilization};
}

// The rows of the hello sweep, or a failed REQUIRE.
std::vector<SweepRow> hello_rows(std::size_t jobs) {
  const auto metrics = hello_metrics();
  const auto rows = run_sweep(hello_path, hello_rate_sweep(), metrics, jobs);
  // C++23 std::expected<T, E>: a value-or-error result. Replaces exceptions or out-parameters;
  // `has_value()` tells which one it holds, `error()` the message.
  INFO((rows ? "" : rows.error()));
  REQUIRE(rows.has_value());
  return *rows;
}

// Splits `text` at every `separator`; a trailing separator does not add an empty last piece
// (so a text that ends with '\n' has as many lines as lines).
std::vector<std::string> split(std::string_view text, char separator) {
  std::vector<std::string> pieces;
  std::string current;
  for (const char c : text) {
    if (c == separator) {
      pieces.push_back(current);
      current.clear();
    } else {
      current += c;
    }
  }
  if (!current.empty()) pieces.push_back(current);
  return pieces;
}

double parse_double(const std::string& text) {
  double value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  REQUIRE(error == std::errc{});
  REQUIRE(end == text.data() + text.size());
  return value;
}

}  // namespace

// ---- parse_param: `<dot.path>=<from>..<to>:<n>` -----------------------------------------------
// The grammar, in short: n evenly spaced points from `from` to `to`, both included, written in
// the scenario's own unit syntax (`100/s`, `5ms`, `400`), so each one can replace the file's
// value as it stands.

TEST_CASE("a rate range expands to evenly spaced rates in the scenario's own syntax") {
  const auto param = parse_param("nodes.users.rate.base=100/s..900/s:3");
  INFO((param ? "" : param.error()));
  REQUIRE(param.has_value());
  REQUIRE(param->path == "nodes.users.rate.base");
  // 100 + k x (900 - 100) / (3 - 1) for k = 0, 1, 2.
  REQUIRE(param->values == std::vector<std::string>{"100/s", "500/s", "900/s"});
}

TEST_CASE("a range of whole numbers expands to whole numbers") {
  const auto param = parse_param("nodes.w.workers=400..600:3");
  INFO((param ? "" : param.error()));
  REQUIRE(param.has_value());
  REQUIRE(param->path == "nodes.w.workers");
  REQUIRE(param->values == std::vector<std::string>{"400", "500", "600"});
}

TEST_CASE("two points are exactly the two ends") {
  const auto param = parse_param("nodes.w.workers=400..600:2");
  REQUIRE(param.has_value());
  REQUIRE(param->values == std::vector<std::string>{"400", "600"});
}

TEST_CASE("a count that is not a whole number at some point is refused with the value it gives") {
  // 400 + 1 x (600 - 400) / (4 - 1) = 466.67: a worker count cannot be a third of a worker.
  // The message names the key (its last path segment) and echoes the range as the user typed it.
  const auto param = parse_param("nodes.w.workers=400..600:4");
  REQUIRE_FALSE(param.has_value());
  REQUIRE(param.error() == "--param: workers must be whole (400..600:4 gives 466.67)");
}

TEST_CASE("a range of fewer than two points is refused") {
  // C++23 std::string::contains / C++20 starts_with: substring tests that read as the
  // question they ask; they replace `find(...) != npos` and `compare(0, n, ...) == 0`.
  const auto param = parse_param("nodes.users.rate.base=100/s..900/s:1");
  REQUIRE_FALSE(param.has_value());
  REQUIRE(param.error().starts_with("--param: "));
}

TEST_CASE("the ends of a range must be the same kind of value") {
  // 1ms is a duration and 5/s a rate: there is nothing between them. The message names both.
  const auto param = parse_param("nodes.users.rate.base=1ms..5/s:3");
  REQUIRE_FALSE(param.has_value());
  REQUIRE(param.error().starts_with("--param: "));
  REQUIRE(param.error().contains("1ms"));
  REQUIRE(param.error().contains("5/s"));
}

TEST_CASE("a path that runs through a list is refused because only maps are addressable") {
  // `links` is a list; `0` would be an index into it. The sweep replaces a scalar at a dot path
  // through maps only (list entries have no names to say which one is meant).
  const auto param = parse_param("links.0.latency=1ms..5ms:3");
  REQUIRE_FALSE(param.has_value());
  REQUIRE(param.error().starts_with("--param: "));
  REQUIRE(param.error().contains("list"));
}

TEST_CASE("a spec that does not follow the grammar is refused") {
  for (const std::string_view bad :
       {"nodes.users.rate.base", "nodes.users.rate.base=100/s",
        "nodes.users.rate.base=100/s..900/s", "nodes.users.rate.base=100/s..900/s:x",
        "nodes.users.rate.base=100/s..900/s:0", "=100/s..900/s:3"}) {
    CAPTURE(bad);
    const auto param = parse_param(bad);
    REQUIRE_FALSE(param.has_value());
    REQUIRE(param.error().starts_with("--param: "));
  }
}

// ---- parse_metric: what to read off each point ------------------------------------------------

TEST_CASE(
    "a client op's mean latency, a node's mean utilization and a queue's peak lag are metrics") {
  for (const std::string_view name : {"users.ping.mean", "api.mean_rho", "transcode_q.lag"}) {
    CAPTURE(name);
    const auto metric = parse_metric(name);
    INFO((metric ? "" : metric.error()));
    REQUIRE(metric.has_value());
    REQUIRE(metric->name == name);  // the name as typed: it heads the metric's column
  }
}

TEST_CASE("an unknown metric is refused and named") {
  for (const std::string_view bad : {"api.nonsense", "api", "users.ping.p99", ""}) {
    CAPTURE(bad);
    const auto metric = parse_metric(bad);
    REQUIRE_FALSE(metric.has_value());
    REQUIRE(metric.error().starts_with("--metric: "));
    REQUIRE(metric.error().contains(std::string{bad}));
  }
}

// ---- config::load with overrides ---------------------------------------------------------------
// The sweep's one hook into the loader: `Override{path, value}` replaces the scalar at a dot
// path BEFORE validation, so the replaced value goes through the same checks as one written in
// the file, and the engine never sees a half-validated scenario.

TEST_CASE("an override replaces the value at its dot path and leaves the rest of the file alone") {
  // C++20 std::span<const T>: a non-owning view of contiguous elements, so `load` takes a
  // vector, an array or nothing without a copy and without a template.
  const std::vector<Override> overrides{{.path = "nodes.users.rate.base", .value = "500/s"}};
  const auto scenario = simsys::config::load(hello_path, overrides);
  INFO((scenario ? "" : scenario.error().message));
  REQUIRE(scenario.has_value());
  REQUIRE(scenario->nodes[0].id == "users");
  REQUIRE(scenario->nodes[0].workload.has_value());
  REQUIRE(scenario->nodes[0].workload->curve.base.per_second == 500.0);  // the file says 100/s
  REQUIRE(scenario->nodes[1].capacity == Rate{1000});                    // untouched
  REQUIRE(scenario->nodes[1].latency == 1ms);
}

TEST_CASE("several overrides apply together") {
  const std::vector<Override> overrides{{.path = "nodes.users.rate.base", .value = "250/s"},
                                        {.path = "nodes.api.latency", .value = "2ms"}};
  const auto scenario = simsys::config::load(hello_path, overrides);
  INFO((scenario ? "" : scenario.error().message));
  REQUIRE(scenario.has_value());
  REQUIRE(scenario->nodes[0].workload->curve.base.per_second == 250.0);
  REQUIRE(scenario->nodes[1].latency == 2ms);
}

TEST_CASE("loading with no overrides gives what loading without the argument gives") {
  const auto plain = simsys::config::load(hello_path);
  const auto none = simsys::config::load(hello_path, std::span<const Override>{});
  REQUIRE(plain.has_value());
  REQUIRE(none.has_value());
  REQUIRE(none->name == plain->name);
  REQUIRE(none->nodes.size() == plain->nodes.size());
  REQUIRE(none->nodes[0].workload->curve.base.per_second ==
          plain->nodes[0].workload->curve.base.per_second);
}

TEST_CASE("an overridden value is validated like one written in the file") {
  const std::vector<Override> overrides{{.path = "nodes.users.rate.base", .value = "-1/s"}};
  const auto scenario = simsys::config::load(hello_path, overrides);
  REQUIRE_FALSE(scenario.has_value());
  // The loader's own rule for a rate (see test_config): not an override-specific message.
  REQUIRE(scenario.error().message.contains("non-negative"));
}

TEST_CASE("an override of a key the file does not have is refused, naming the key") {
  // A typo (`bse` for `base`) must not silently sweep nothing.
  const std::vector<Override> overrides{{.path = "nodes.users.rate.bse", .value = "500/s"}};
  const auto scenario = simsys::config::load(hello_path, overrides);
  REQUIRE_FALSE(scenario.has_value());
  REQUIRE(scenario.error().message == "--param: nodes.users.rate.bse: no such key in the scenario");
}

TEST_CASE("an override path that runs through a list is refused") {
  // hello's `links:` is a list; dot paths go through maps only.
  const std::vector<Override> overrides{{.path = "links.0.latency", .value = "1ms"}};
  const auto scenario = simsys::config::load(hello_path, overrides);
  REQUIRE_FALSE(scenario.has_value());
  REQUIRE(scenario.error().message.starts_with("--param: "));
  REQUIRE(scenario.error().message.contains("list"));
}

// ---- run_sweep: one simulation per point -------------------------------------------------------

TEST_CASE("sweeping hello's rate gives the M/M/1 latency and utilization at each rate") {
  const auto rows = hello_rows(1);
  REQUIRE(rows.size() == 3);

  REQUIRE(rows[0].value == "100/s");
  REQUIRE(rows[0].metrics.size() == 2);  // one number per metric, in the order asked
  REQUIRE_THAT(rows[0].metrics[0], WithinRel(mean_100, rel_tolerance));  // 1.61 ms
  REQUIRE_THAT(rows[0].metrics[1], WithinAbs(0.1, 1e-9));

  REQUIRE(rows[1].value == "500/s");
  REQUIRE_THAT(rows[1].metrics[0], WithinRel(mean_500, rel_tolerance));  // 2.5 ms
  REQUIRE_THAT(rows[1].metrics[1], WithinAbs(0.5, 1e-9));

  REQUIRE(rows[2].value == "900/s");
  REQUIRE_THAT(rows[2].metrics[0], WithinRel(mean_900, rel_tolerance));  // 10.5 ms
  REQUIRE_THAT(rows[2].metrics[1], WithinAbs(0.9, 1e-9));
}

TEST_CASE("the rows do not depend on how many workers ran the points") {
  // Each point is its own Simulation with its own seed, and a row is stored at its point's
  // index, so which thread ran which point cannot show in the result: not even in the last bit.
  const auto one = hello_rows(1);
  const auto three = hello_rows(3);
  const auto more_workers_than_points = hello_rows(8);
  REQUIRE(one.size() == three.size());
  REQUIRE(one.size() == more_workers_than_points.size());
  for (std::size_t i = 0; i < one.size(); ++i) {
    CAPTURE(i);
    REQUIRE(three[i].value == one[i].value);
    REQUIRE(three[i].metrics == one[i].metrics);  // exact: no tolerance
    REQUIRE(more_workers_than_points[i].value == one[i].value);
    REQUIRE(more_workers_than_points[i].metrics == one[i].metrics);
  }
}

TEST_CASE("a point whose value fails the loader is reported by its number and its override") {
  const auto metrics = hello_metrics();
  const SweepParam param{.path = "nodes.users.rate.base", .values = {"100/s", "-1/s", "300/s"}};
  const auto rows = run_sweep(hello_path, param, metrics, 1);
  REQUIRE_FALSE(rows.has_value());
  // Points are numbered from 1: the second value is point 2. What follows is the loader's text.
  REQUIRE(rows.error().starts_with("point 2 (nodes.users.rate.base=-1/s): "));
  REQUIRE(rows.error().contains("non-negative"));
}

TEST_CASE("when several points fail the lowest-numbered one is reported, whatever the workers") {
  // Which failing point a worker meets first depends on the threads; the answer must not.
  const auto metrics = hello_metrics();
  const SweepParam param{.path = "nodes.users.rate.base",
                         .values = {"100/s", "-1/s", "-2/s", "-3/s", "500/s"}};
  for (const std::size_t jobs : {std::size_t{1}, std::size_t{3}, std::size_t{5}}) {
    CAPTURE(jobs);
    const auto rows = run_sweep(hello_path, param, metrics, jobs);
    REQUIRE_FALSE(rows.has_value());
    REQUIRE(rows.error().starts_with("point 2 (nodes.users.rate.base=-1/s): "));
  }
}

TEST_CASE("a swept key that is not in the file is refused before any simulation runs") {
  const auto metrics = hello_metrics();
  const SweepParam param{.path = "nodes.users.rate.bse", .values = {"100/s", "200/s"}};
  const auto rows = run_sweep(hello_path, param, metrics, 2);
  REQUIRE_FALSE(rows.has_value());
  REQUIRE(rows.error().contains("nodes.users.rate.bse: no such key in the scenario"));
}

TEST_CASE("a scenario file that does not exist is an error naming the file") {
  const auto metrics = hello_metrics();
  const std::string missing = "/nonexistent-dir-for-simsys-tests/scenario.yaml";
  const auto rows = run_sweep(missing, hello_rate_sweep(), metrics, 1);
  REQUIRE_FALSE(rows.has_value());
  REQUIRE(rows.error().contains(missing));
}

TEST_CASE("a metric naming a node or op the scenario does not have is an error naming it") {
  // parse_metric only knows the grammar of a metric, not the scenario: the check that `ghost`
  // exists can only happen once a scenario is loaded.
  const auto ghost = parse_metric("ghost.mean_rho");
  REQUIRE(ghost.has_value());
  const std::vector<SweepMetric> metrics{*ghost};
  const auto rows = run_sweep(hello_path, hello_rate_sweep(), metrics, 1);
  REQUIRE_FALSE(rows.has_value());
  REQUIRE(rows.error().contains("ghost"));

  // hello has no message queue either, so a queue lag cannot be read.
  const auto lag = parse_metric("api.lag");
  REQUIRE(lag.has_value());
  const std::vector<SweepMetric> lag_metrics{*lag};
  const auto lag_rows = run_sweep(hello_path, hello_rate_sweep(), lag_metrics, 1);
  REQUIRE_FALSE(lag_rows.has_value());
  REQUIRE(lag_rows.error().contains("api"));
}

// ---- render_sweep -------------------------------------------------------------------------------
// Rows are written by hand here (the numbers above, in seconds and as a ratio), so these
// tests fail only when the rendering is wrong, never because the engine moved.

namespace {
std::vector<SweepRow> hello_rows_by_hand() {
  return {SweepRow{.value = "100/s", .metrics = {0.00161, 0.1}},
          SweepRow{.value = "500/s", .metrics = {0.0025, 0.5}},
          SweepRow{.value = "900/s", .metrics = {0.0105, 0.9}}};
}
}  // namespace

TEST_CASE("the csv has the swept key and the metrics as its header and one line per point") {
  const auto metrics = hello_metrics();
  const auto text =
      render_sweep(hello_rate_sweep(), metrics, hello_rows_by_hand(), SweepFormat::csv);
  REQUIRE(text.ends_with('\n'));
  const auto lines = split(text, '\n');
  REQUIRE(lines.size() == 4);  // header + 3 rows
  REQUIRE(lines[0] == "nodes.users.rate.base,users.ping.mean,api.mean_rho");
  // The shortest text that reads back to the same double (report::format_number), in seconds.
  REQUIRE(lines[1] == "100/s,0.00161,0.1");
  REQUIRE(lines[2] == "500/s,0.0025,0.5");
  REQUIRE(lines[3] == "900/s,0.0105,0.9");
}

TEST_CASE("the table has a line per point that holds the value and its numbers") {
  const auto metrics = hello_metrics();
  const auto text =
      render_sweep(hello_rate_sweep(), metrics, hello_rows_by_hand(), SweepFormat::table);
  const auto lines = split(text, '\n');
  REQUIRE(lines.size() >= 4);  // a header, then one line per point (a rule line is allowed)

  // Pinned loosely on purpose: the column widths are padded by hand and may change; what must
  // hold is that the header names the metrics and each point's line carries its value and both
  // numbers (a latency in ms, "1.61 ms", the plan's own wording; a utilization as a ratio).
  REQUIRE(lines[0].contains("users.ping.mean"));
  REQUIRE(lines[0].contains("api.mean_rho"));

  const auto line_of = [&](std::string_view value) -> std::string {
    for (const auto& line : lines) {
      if (line.contains(value)) return line;
    }
    return {};
  };
  const auto first = line_of("100/s");
  REQUIRE_FALSE(first.empty());
  REQUIRE(first.contains("1.61"));
  REQUIRE(first.contains("0.1"));
  const auto second = line_of("500/s");
  REQUIRE_FALSE(second.empty());
  REQUIRE(second.contains("2.5"));
  REQUIRE(second.contains("0.5"));
  const auto third = line_of("900/s");
  REQUIRE_FALSE(third.empty());
  REQUIRE(third.contains("10.5"));
  REQUIRE(third.contains("0.9"));
}

TEST_CASE("the csv of a real sweep holds the same numbers as its rows") {
  const auto metrics = hello_metrics();
  const auto rows = hello_rows(2);
  const auto text = render_sweep(hello_rate_sweep(), metrics, rows, SweepFormat::csv);
  const auto lines = split(text, '\n');
  REQUIRE(lines.size() == 4);
  REQUIRE(lines[0] == "nodes.users.rate.base,users.ping.mean,api.mean_rho");
  for (std::size_t i = 0; i < rows.size(); ++i) {
    CAPTURE(i);
    const auto cells = split(lines[i + 1], ',');
    REQUIRE(cells.size() == 3);
    REQUIRE(cells[0] == rows[i].value);
    // Round trip: format_number's text reads back to the very double in the row.
    REQUIRE(parse_double(cells[1]) == rows[i].metrics[0]);
    REQUIRE(parse_double(cells[2]) == rows[i].metrics[1]);
  }
  // And the plan's pins, in seconds: 1.61 ms, 2.5 ms, 10.5 ms.
  REQUIRE_THAT(parse_double(split(lines[1], ',')[1]), WithinRel(mean_100, rel_tolerance));
  REQUIRE_THAT(parse_double(split(lines[2], ',')[1]), WithinRel(mean_500, rel_tolerance));
  REQUIRE_THAT(parse_double(split(lines[3], ',')[1]), WithinRel(mean_900, rel_tolerance));
}
