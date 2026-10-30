// `simsys sweep`: run one scenario over a range of one key, one simulation per point, the points in
// parallel. Properties of the pieces: the `--param` grammar (`<dot.path>=<from>..<to>:<n>`), the
// `--metric` names, the loader's `Override`, the sweep itself (values pinned against M/M/1 on the
// tutorial's hello scenario) and the two renderings.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>  // getpid: a temporary file per test process

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

// The words of a table line, the runs of characters between spaces.
std::vector<std::string> split_words(std::string_view line) {
  std::vector<std::string> words;
  for (const auto& piece : split(line, ' ')) {
    if (!piece.empty()) words.push_back(piece);
  }
  return words;
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
        "nodes.users.rate.base=100/s..900/s:0", "=100/s..900/s:3",
        // A third dot: the upper end would read as `.5ms`, 0.5 ms, a different sweep than typed.
        "nodes.api.latency=1ms...5ms:2"}) {
    CAPTURE(bad);
    const auto param = parse_param(bad);
    REQUIRE_FALSE(param.has_value());
    REQUIRE(param.error().starts_with("--param: "));
  }
}

TEST_CASE("a range of more than 10000 points is refused before it is expanded") {
  // Each point is a whole simulation; a typo of a few zeros must not exhaust memory first.
  const auto too_many = parse_param("nodes.users.rate.base=100/s..900/s:10001");
  REQUIRE_FALSE(too_many.has_value());
  REQUIRE(too_many.error() == "--param: 100/s..900/s:10001: at most 10000 points");
  const auto absurd = parse_param("nodes.users.rate.base=100/s..900/s:1000000000000");
  REQUIRE_FALSE(absurd.has_value());
  const auto most = parse_param("nodes.users.rate.base=100/s..900/s:10000");
  REQUIRE(most.has_value());
  REQUIRE(most->values.size() == 10000);
}

TEST_CASE("only a key the loader reads as a whole number must get whole points") {
  // Where the key sits decides, not its name alone: an op share that happens to be called
  // `workers` is a fraction like any other share.
  for (const std::string_view whole :
       {"seed=1..2:3", "nodes.w.workers=1..2:3", "nodes.w.advanced.queue_limit=1..2:3",
        "nodes.c.keys.space=1..2:3", "nodes.kv.nodes=1..2:3"}) {
    CAPTURE(whole);
    const auto param = parse_param(whole);
    REQUIRE_FALSE(param.has_value());
    REQUIRE(param.error().contains("must be whole"));
  }
  for (const std::string_view fractional :
       {"nodes.u.ops.workers=0.1..0.9:4", "nodes.w.advanced.burstiness=1..2:3",
        "nodes.w.latency=1ms..2ms:3"}) {
    CAPTURE(fractional);
    const auto param = parse_param(fractional);
    INFO((param ? "" : param.error()));
    REQUIRE(param.has_value());
  }
}

TEST_CASE("the whole-number message shows the point with its unit") {
  // 5M + 1 x (15M - 5M) / 3 = 8.33M keys: the unit belongs to the number.
  const auto param = parse_param("nodes.c.keys=5M..15M:4");
  REQUIRE_FALSE(param.has_value());
  REQUIRE(param.error() == "--param: keys must be whole (5M..15M:4 gives 8.33M)");
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

// The run table's other figures: an op's percentiles as the op table prints them, a node's peak
// utilization and its drop rate.
TEST_CASE("a client op's p50, p95 and p99 and a node's peak_rho and drop_rate are metrics") {
  using simsys::sweep::MetricKind;
  const std::array expected{std::pair{"users.ping.p50", MetricKind::p50_latency},
                            std::pair{"users.ping.p95", MetricKind::p95_latency},
                            std::pair{"users.ping.p99", MetricKind::p99_latency},
                            std::pair{"api.peak_rho", MetricKind::peak_rho},
                            std::pair{"api.drop_rate", MetricKind::drop_rate}};
  for (const auto& [name, kind] : expected) {
    CAPTURE(name);
    const auto metric = parse_metric(name);
    INFO((metric ? "" : metric.error()));
    REQUIRE(metric.has_value());
    REQUIRE(metric->kind == kind);
  }
  REQUIRE(parse_metric("users.ping.p99")->op == "ping");
}

// A client op's rates: what it sent, retries included (`attempts`, the op table's attempts/s), and
// what came back answered (`answered`, JSON's answered_per_s).
TEST_CASE("a client op's attempts and answered rates are metrics") {
  using simsys::sweep::MetricKind;
  const std::array expected{std::pair{"users.ping.attempts", MetricKind::attempts_rate},
                            std::pair{"users.ping.answered", MetricKind::answered_rate}};
  for (const auto& [name, kind] : expected) {
    CAPTURE(name);
    const auto metric = parse_metric(name);
    INFO((metric ? "" : metric.error()));
    REQUIRE(metric.has_value());
    REQUIRE(metric->kind == kind);
    REQUIRE(metric->node == "users");
    REQUIRE(metric->op == "ping");
  }
}

TEST_CASE("a client has no drop rate to sweep, as it has no utilization") {
  const auto drops = parse_metric("users.drop_rate");
  REQUIRE(drops.has_value());
  const std::vector<SweepMetric> metrics{*drops};
  const auto rows = run_sweep(hello_path, hello_rate_sweep(), metrics, 1);
  REQUIRE_FALSE(rows.has_value());
  REQUIRE(rows.error() == std::string{hello_path} +
                              ": error: --metric: users.drop_rate: 'users' is a client, which "
                              "serves nothing, so it has no drop rate");
}

TEST_CASE("an unknown metric is refused and named") {
  for (const std::string_view bad : {"api.nonsense", "api", "users.ping.p90", "api.p99", ""}) {
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

TEST_CASE("several overrides apply together, each replacing the value at its own path") {
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

TEST_CASE("an override path can be checked against the file without loading any value") {
  REQUIRE(simsys::config::check_override_path(hello_path, "nodes.users.rate.base").has_value());
  const auto missing = simsys::config::check_override_path(hello_path, "nodes.users.rate.bse");
  REQUIRE_FALSE(missing.has_value());
  REQUIRE(missing.error().message == "--param: nodes.users.rate.bse: no such key in the scenario");
  REQUIRE(missing.error().line == 0);  // the mistake is on the command line, not in the file
}

TEST_CASE("a loader error is described the way compilers report errors") {
  using simsys::config::ConfigError;
  REQUIRE(simsys::config::describe(ConfigError{
              .file = "a.yaml", .line = 3, .column = 7, .message = "m"}) == "a.yaml:3:7: error: m");
  // No position when the line is unknown (a missing file, an override's path).
  REQUIRE(simsys::config::describe(ConfigError{
              .file = "a.yaml", .line = 0, .column = 0, .message = "m"}) == "a.yaml: error: m");
}

// `run --duration 30m` on url-shortener (warmup 2m, an SLO `over: 1h`) is an override of
// `duration`, which the loader refuses at the SLO: its message names the construct (`slo: `), and
// `rule` is the rule alone, which the CLI puts after its own `--duration 30m: `.
TEST_CASE("a loader error inside a construct gives the rule apart from the construct's name") {
  const std::string url_shortener =
      std::string{SIMSYS_SCENARIOS_DIR} + "/book/url-shortener/scenario.yaml";
  const std::vector<Override> overrides{{.path = "duration", .value = "30m"}};
  const auto scenario = simsys::config::load(url_shortener, overrides);
  REQUIRE_FALSE(scenario.has_value());
  const std::string rule = "over must fit in the run after warmup (1h > duration 30m - warmup 2m)";
  REQUIRE(scenario.error().message == "slo: " + rule);
  REQUIRE(scenario.error().rule == rule);
  REQUIRE(scenario.error().line > 0);  // at the SLO's line in the file
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

// The percentiles of hello at 500/s: the api is M/M/1 at rho 0.5, whose sojourn is exponential
// with mean s / (1 − rho) = 2 ms, so variance (2 ms)^2 = 4e-6 s^2; the link's 0.5 ms round trip
// adds to the mean and not to the spread: mean 2.5 ms, variance 4e-6 s^2. The reports fit a
// lognormal to those two moments (queueing::lognormal_quantile): sigma^2 = ln(1 + 4e-6 / 2.5e-3^2)
// = ln 1.64 = 0.4947, mu = ln 2.5e-3 − sigma^2 / 2 = −6.2388, so
//   p50 = exp(mu) = 1.952 ms,  p95 = exp(mu + 1.6449 sigma) = 6.208 ms,
//   p99 = exp(mu + 2.3263 sigma) = 10.03 ms.
// The api's peak utilization is its steady 0.5 and it drops nothing.
TEST_CASE("a sweep reads hello's percentiles, peak utilization and drop rate as the run does") {
  std::vector<SweepMetric> metrics;
  for (const auto* name :
       {"users.ping.p50", "users.ping.p95", "users.ping.p99", "api.peak_rho", "api.drop_rate"}) {
    metrics.push_back(*parse_metric(name));
  }
  const SweepParam param{.path = "nodes.users.rate.base", .values = {"500/s", "900/s"}};
  const auto rows = run_sweep(hello_path, param, metrics, 2);
  INFO((rows ? "" : rows.error()));
  REQUIRE(rows.has_value());
  const auto& at_500 = (*rows)[0].metrics;
  REQUIRE_THAT(at_500[0], WithinRel(1.952e-3, 1e-3));
  REQUIRE_THAT(at_500[1], WithinRel(6.208e-3, 1e-3));
  REQUIRE_THAT(at_500[2], WithinRel(10.03e-3, 1e-3));
  REQUIRE_THAT(at_500[3], WithinAbs(0.5, 1e-9));
  REQUIRE(at_500[4] == 0.0);
  REQUIRE((*rows)[0].beyond_timeout.empty());  // hello's client waits for ever
  REQUIRE((*rows)[1].metrics[2] > at_500[2]);  // 900/s queues longer
}

namespace {
// hello at 900/s whose client gives up after 5 ms: a scenario file of the test's own, because a
// sweep replaces values and cannot add the `timeout` key hello lacks.
std::string hello_with_timeout_path() {
  const auto path = std::filesystem::temp_directory_path() /
                    ("simsys-sweep-timeout-" + std::to_string(getpid()) + ".yaml");
  std::ofstream{path} << "version: 1\nname: hello-timeout\nduration: 10s\nnodes:\n"
                         "  users: {type: client, rate: {base: 900/s}, ops: {ping: 1.0}, "
                         "timeout: 5ms}\n"
                         "  api:   {type: service, capacity: 1k/s, latency: 1ms}\n"
                         "links:\n  - {from: users, to: api, dir: inout}\n";
  return path.string();
}
}  // namespace

// hello at 900/s: mean 10.5 ms, variance (10 ms)^2 = 1e-4 s^2, lognormal sigma^2 = ln(1 + 1e-4 /
// 10.5e-3^2) = 0.6456, mu = ln 10.5e-3 − 0.3228 = −4.8792. Behind a 5 ms timeout,
// P(X > 5 ms) = 1 − Phi((ln 5e-3 − mu) / sigma) = 1 − Phi(−0.521) = 0.70 time out: more than half,
// so the p50 and the p99 are the timeout, never seen answered. Behind 45 ms,
// P(X > 45 ms) = 1 − Phi(2.213) = 1.35 %: more than the 1 % a p99 leaves, so the p99 is the
// timeout while the p50 is the fit's median, exp(mu) = 7.60 ms. The table prints such a cell as
// the op table does, `>5ms`; the csv keeps the number.
TEST_CASE("a percentile that is the client's timeout prints as the op table prints it") {
  const auto path = hello_with_timeout_path();
  const std::vector<SweepMetric> metrics{*parse_metric("users.ping.p50"),
                                         *parse_metric("users.ping.p99")};
  const SweepParam param{.path = "nodes.users.timeout", .values = {"5ms", "45ms"}};
  const auto rows = run_sweep(path, param, metrics, 1);
  std::filesystem::remove(path);
  INFO((rows ? "" : rows.error()));
  REQUIRE(rows.has_value());
  REQUIRE((*rows)[0].beyond_timeout == std::vector<std::size_t>{0, 1});
  REQUIRE((*rows)[0].metrics[1] == 0.005);
  REQUIRE((*rows)[1].beyond_timeout == std::vector<std::size_t>{1});
  REQUIRE_THAT((*rows)[1].metrics[0], WithinRel(7.60e-3, 1e-3));
  REQUIRE((*rows)[1].metrics[1] == 0.045);
  const auto table = split(render_sweep(param, metrics, *rows, SweepFormat::table), '\n');
  CAPTURE(table);
  // Words of the lines: the swept value, then one cell per metric.
  REQUIRE(split_words(table[2]) == std::vector<std::string>{"5ms", ">5ms", ">5ms"});
  REQUIRE(split_words(table[3]) == std::vector<std::string>{"45ms", "7.6ms", ">45ms"});
  const auto csv = split(render_sweep(param, metrics, *rows, SweepFormat::csv), '\n');
  REQUIRE(csv[1] == "5ms,0.005,0.005");
}

namespace {
// u sends 1000/s into an external sink that fails half of what it serves (room to spare: 100k/s),
// with `retries` to sweep. Observed after a 1 s warmup, so the retry ramp is left out.
std::string retrying_sink_path() {
  const auto path = std::filesystem::temp_directory_path() /
                    ("simsys-sweep-retries-" + std::to_string(getpid()) + ".yaml");
  std::ofstream{path} << "version: 1\nname: retrying-sink\nduration: 10s\nstep: 100ms\n"
                         "warmup: 1s\nnodes:\n"
                         "  u: {type: client, rate: {base: 1k/s}, ops: {call: 1.0}, retries: 0}\n"
                         "  api: {type: external_sink, capacity: 100k/s, latency: 1ms, "
                         "failure_rate: 0.5}\n"
                         "links:\n  - {from: u, to: api, dir: inout, latency: 0s}\n";
  return path.string();
}
}  // namespace

// The retry storm as a sweep: a path that fails half of what it is sent, swept from no retries to
// two. Without retries u sends its 1000/s once and half is answered; with 2 it sends each request
// 1 + 0.5 + 0.25 = 1.75 times (1750/s) and answers 1 − 0.5^3 = 87.5 % of them (875/s)
// (tests/theory/test_retries.cpp derives both). The table prints the rates as the op table does.
TEST_CASE("sweeping a client's retries reads its attempts and answered rates: 1k then 1.75k sent") {
  const auto path = retrying_sink_path();
  const std::vector<SweepMetric> metrics{*parse_metric("u.call.attempts"),
                                         *parse_metric("u.call.answered")};
  const SweepParam param{.path = "nodes.u.retries", .values = {"0", "2"}};
  const auto rows = run_sweep(path, param, metrics, 1);
  std::filesystem::remove(path);
  INFO((rows ? "" : rows.error()));
  REQUIRE(rows.has_value());
  REQUIRE_THAT((*rows)[0].metrics[0], WithinRel(1000.0, 1e-9));
  REQUIRE_THAT((*rows)[0].metrics[1], WithinRel(500.0, 1e-9));
  REQUIRE_THAT((*rows)[1].metrics[0], WithinRel(1750.0, 1e-9));
  REQUIRE_THAT((*rows)[1].metrics[1], WithinRel(875.0, 1e-9));
  const auto table = split(render_sweep(param, metrics, *rows, SweepFormat::table), '\n');
  CAPTURE(table);
  REQUIRE(split_words(table[2]) == std::vector<std::string>{"0", "1k/s", "500/s"});
  REQUIRE(split_words(table[3]) == std::vector<std::string>{"2", "1.75k/s", "875/s"});
}

namespace {
// u publishes 1000/s to q; pool w (100 workers at 1 ms) sends each job to an external sink that
// fails half of what it serves (room to spare: 100k/s), retrying `retries` (swept) a step later.
// Observed after a 2 s warmup, past the retry ramp.
std::string retrying_pool_path() {
  const auto path = std::filesystem::temp_directory_path() /
                    ("simsys-sweep-dead-" + std::to_string(getpid()) + ".yaml");
  std::ofstream{path} << "version: 1\nname: retrying-pool\nduration: 10s\nstep: 100ms\n"
                         "warmup: 2s\nnodes:\n"
                         "  u: {type: client, rate: {base: 1k/s}, ops: {publish: 1.0}}\n"
                         "  q: {type: message_queue}\n"
                         "  w: {type: worker_pool, consumes: q, workers: 100, cost: 1ms, "
                         "retries: 1, retry_after: 100ms, then: [{to: api, op: send}]}\n"
                         "  api: {type: external_sink, capacity: 100k/s, latency: 1ms, "
                         "failure_rate: 0.5}\n"
                         "links:\n  - {from: u, to: q, latency: 0s}\n"
                         "  - {from: q, to: w, latency: 0s}\n"
                         "  - {from: w, to: api, dir: inout, latency: 0s}\n";
  return path.string();
}
}  // namespace

// A pool's dead letters per second (JSON's dead_lettered_per_s): the jobs whose last attempt
// failed, 1000 x 0.5^(n+1): 250/s with 1 retry, 62.5/s with 3. The table prints a rate.
TEST_CASE("sweeping a pool's retries reads its dead letters: 250/s then 62.5/s") {
  using simsys::sweep::MetricKind;
  const auto metric = parse_metric("w.dead_lettered");
  INFO((metric ? "" : metric.error()));
  REQUIRE(metric.has_value());
  REQUIRE(metric->kind == MetricKind::dead_lettered_rate);
  const auto path = retrying_pool_path();
  const std::vector<SweepMetric> metrics{*metric};
  const SweepParam param{.path = "nodes.w.retries", .values = {"1", "3"}};
  const auto rows = run_sweep(path, param, metrics, 1);
  std::filesystem::remove(path);
  INFO((rows ? "" : rows.error()));
  REQUIRE(rows.has_value());
  REQUIRE_THAT((*rows)[0].metrics[0], WithinRel(250.0, 1e-9));
  REQUIRE_THAT((*rows)[1].metrics[0], WithinRel(62.5, 1e-9));
  const auto table = split(render_sweep(param, metrics, *rows, SweepFormat::table), '\n');
  CAPTURE(table);
  REQUIRE(split_words(table[2]) == std::vector<std::string>{"1", "250/s"});
  REQUIRE(split_words(table[3]) == std::vector<std::string>{"3", "62.5/s"});
}

namespace {
// u sends 200/s, half of it `connect`, to the gateway gw, so 100 connects/s; each connection lasts
// `session` (swept), so the level rises as a first-order lag to C* = 100/s × session (Little's
// law: 1k at 10 s, 2k at 20 s). 600 s at 100 ms steps are 30 time constants of the 20 s session:
// the Euler level is C*(1 − 0.995^6000), within 1e-13 of C*, by the end of the run.
std::string gateway_path() {
  const auto path = std::filesystem::temp_directory_path() /
                    ("simsys-sweep-gateway-" + std::to_string(getpid()) + ".yaml");
  std::ofstream{path}
      << "version: 1\nname: gateway\nduration: 10m\nstep: 100ms\nnodes:\n"
         "  u: {type: client, rate: {base: 200/s}, ops: {connect: 0.5, send: 0.5}}\n"
         "  gw: {type: ws_gateway, session: 10s, "
         "ops: {send: [{to: q, op: publish}]}}\n"
         "  q: {type: service}\n"
         "links:\n  - {from: u, to: gw, latency: 0s}\n"
         "  - {from: gw, to: q, latency: 0s}\n";
  return path.string();
}
}  // namespace

// A gateway's open connections at the END of the run, the `connections` of the node table and of
// the JSON (the same name, the same number); with a steady connect rate the level only rises, so
// the end is also its peak. The table prints a count with an SI prefix, as the node table does.
TEST_CASE("sweeping a gateway's session reads its connections: 1k then 2k") {
  using simsys::sweep::MetricKind;
  const auto metric = parse_metric("gw.connections");
  INFO((metric ? "" : metric.error()));
  REQUIRE(metric.has_value());
  REQUIRE(metric->kind == MetricKind::connections);
  const auto path = gateway_path();
  const std::vector<SweepMetric> metrics{*metric};
  const SweepParam param{.path = "nodes.gw.session", .values = {"10s", "20s"}};
  const auto rows = run_sweep(path, param, metrics, 1);
  std::filesystem::remove(path);
  INFO((rows ? "" : rows.error()));
  REQUIRE(rows.has_value());
  REQUIRE_THAT((*rows)[0].metrics[0], WithinRel(1000.0, 1e-9));
  REQUIRE_THAT((*rows)[1].metrics[0], WithinRel(2000.0, 1e-9));
  const auto table = split(render_sweep(param, metrics, *rows, SweepFormat::table), '\n');
  CAPTURE(table);
  REQUIRE(split_words(table[2]) == std::vector<std::string>{"10s", "1k"});
  REQUIRE(split_words(table[3]) == std::vector<std::string>{"20s", "2k"});
}

namespace {
// u sends 1k/s to the notification fanout `fanout` (the type's 20k/s, far from full), which serves
// every notification and publishes the share its dedup step lets through to `log`; the rest,
// drain x dedup, is its duplicates (behaviours::NotificationFanoutBehaviour).
std::string fanout_path() {
  const auto path = std::filesystem::temp_directory_path() /
                    ("simsys-sweep-fanout-" + std::to_string(getpid()) + ".yaml");
  std::ofstream{path} << "version: 1\nname: fanout\nduration: 1m\nnodes:\n"
                         "  u: {type: client, rate: {base: 1k/s}, ops: {route: 1.0}}\n"
                         "  fanout: {type: notification_fanout, dedup: 0.02, "
                         "then: [{to: log, op: write}]}\n"
                         "  log: {type: service}\n"
                         "links:\n  - {from: u, to: fanout, latency: 0s}\n"
                         "  - {from: fanout, to: log, latency: 0s}\n";
  return path.string();
}
}  // namespace

// A fanout's duplicates per second (JSON's deduplicated_per_s, the node table's deduplicated/s):
// what it serves times its dedup share, 1000 x 0.02 = 20/s, then 1000 x 0.1 = 100/s. The table
// prints a rate, as it does a pool's dead letters.
TEST_CASE("sweeping a fanout's dedup reads its duplicates: 20/s then 100/s") {
  using simsys::sweep::MetricKind;
  const auto metric = parse_metric("fanout.deduplicated");
  INFO((metric ? "" : metric.error()));
  REQUIRE(metric.has_value());
  REQUIRE(metric->kind == MetricKind::deduplicated_rate);
  const auto path = fanout_path();
  const std::vector<SweepMetric> metrics{*metric};
  const SweepParam param{.path = "nodes.fanout.dedup", .values = {"0.02", "0.1"}};
  const auto rows = run_sweep(path, param, metrics, 1);
  std::filesystem::remove(path);
  INFO((rows ? "" : rows.error()));
  REQUIRE(rows.has_value());
  REQUIRE_THAT((*rows)[0].metrics[0], WithinRel(20.0, 1e-9));
  REQUIRE_THAT((*rows)[1].metrics[0], WithinRel(100.0, 1e-9));
  const auto table = split(render_sweep(param, metrics, *rows, SweepFormat::table), '\n');
  CAPTURE(table);
  REQUIRE(split_words(table[2]) == std::vector<std::string>{"0.02", "20/s"});
  REQUIRE(split_words(table[3]) == std::vector<std::string>{"0.1", "100/s"});
}

TEST_CASE("a deduplicated metric on a node that is not a fanout is an error naming its type") {
  const std::vector<SweepMetric> metrics{*parse_metric("api.deduplicated")};
  const auto rows = run_sweep(hello_path, hello_rate_sweep(), metrics, 1);
  REQUIRE_FALSE(rows.has_value());
  REQUIRE(rows.error() == std::string{hello_path} +
                              ": error: --metric: api.deduplicated: 'api' has type service, "
                              "not notification_fanout: only a notification fanout has duplicates");
}

TEST_CASE("a connections metric on a node that is not a gateway is an error naming its type") {
  const std::vector<SweepMetric> metrics{*parse_metric("api.connections")};
  const auto rows = run_sweep(hello_path, hello_rate_sweep(), metrics, 1);
  REQUIRE_FALSE(rows.has_value());
  REQUIRE(rows.error() == std::string{hello_path} +
                              ": error: --metric: api.connections: 'api' has type service, "
                              "not ws_gateway: only a ws_gateway has connections");
}

TEST_CASE(
    "a dead_lettered metric on a node that is not a worker pool is an error naming its type") {
  const std::vector<SweepMetric> metrics{*parse_metric("api.dead_lettered")};
  const auto rows = run_sweep(hello_path, hello_rate_sweep(), metrics, 1);
  REQUIRE_FALSE(rows.has_value());
  REQUIRE(rows.error() == std::string{hello_path} +
                              ": error: --metric: api.dead_lettered: 'api' has type service, "
                              "not worker_pool: only a worker pool has dead letters");
}

TEST_CASE("an attempts metric on a node that is not a client is an error naming its type") {
  const std::vector<SweepMetric> metrics{*parse_metric("api.ping.attempts")};
  const auto rows = run_sweep(hello_path, hello_rate_sweep(), metrics, 1);
  REQUIRE_FALSE(rows.has_value());
  REQUIRE(rows.error() == std::string{hello_path} +
                              ": error: --metric: api.ping.attempts: 'api' has type service, "
                              "not client");
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
  // Every point would fail alike: the path is the sweep's mistake, not a point's.
  REQUIRE_FALSE(rows.error().starts_with("point "));
  REQUIRE(rows.error().starts_with(hello_path));
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
  // C++20 std::string::ends_with: a suffix test in one call, replacing a compare() on the tail.
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
  REQUIRE(first.contains("1.61ms"));  // as the report prints a latency
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

TEST_CASE("a number the run did not produce is a dash in the table and an empty csv cell") {
  // NaN: a latency of an op of which nothing was answered (the report prints `-` there too).
  const auto latency = parse_metric("users.ping.mean");
  REQUIRE(latency.has_value());
  const std::vector<SweepMetric> metrics{*latency};
  const std::vector<SweepRow> rows{
      SweepRow{.value = "100/s", .metrics = {std::numeric_limits<double>::quiet_NaN()}}};
  const auto csv = split(render_sweep(hello_rate_sweep(), metrics, rows, SweepFormat::csv), '\n');
  REQUIRE(csv.size() == 2);
  REQUIRE(csv[1] == "100/s,");
  const auto table =
      split(render_sweep(hello_rate_sweep(), metrics, rows, SweepFormat::table), '\n');
  REQUIRE(table.back().starts_with("100/s"));
  REQUIRE(table.back().ends_with(" -"));
}

TEST_CASE("a queue lag below a microsecond prints as the report prints it") {
  // An idle consumer group's lag is Erlang C's tiny chance of waiting times a job (6.3e-11 s on
  // youtube's completion_q); the report's tables say "<1us", and so does the sweep's.
  const auto lag = parse_metric("transcode_q.lag");
  REQUIRE(lag.has_value());
  const std::vector<SweepMetric> metrics{*lag};
  const std::vector<SweepRow> rows{SweepRow{.value = "400", .metrics = {6.3e-11}}};
  const SweepParam param{.path = "nodes.transcoders.workers", .values = {"400"}};
  const auto table = split(render_sweep(param, metrics, rows, SweepFormat::table), '\n');
  REQUIRE(table.back().ends_with("<1us"));
}
