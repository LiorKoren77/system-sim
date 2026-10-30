// Whole-simulation checks against closed forms (phase 3 spec §8.2; book ch. 12's chat servers and
// ch. 15's notification long polls).
// This file: the `ws_gateway`, a node that holds connections as a LEVEL: how the level fills (a
// first-order lag), what its heartbeats cost the gateway and the service behind it, the cap, the
// fixed level, the message fan-out, the reports' `connections`, and the two refusals of the
// engine. One of the theory files, split by topic; the builders more than one needs are in
// tests/support/fixtures.cppm (module simsys.test_fixtures).
//
// ---- The model the tests pin ------------------------------------------------------------------
//
// A websocket gateway keeps one long-lived connection per online user, so unlike a request the
// connection does not leave after a step: the number of open connections C is a STOCK (a level)
// that rises with connects and falls with disconnects, each step
//
//     C(k+1) = C(k) + (connects - disconnects) dt              (Euler step of a rate balance)
//     connects    = the rate of the clients' `connect` op that reaches the gateway
//     disconnects = C / session        (a user stays `session` on average, so each of the C open
//                                       connections ends at rate 1 / session: Little's law
//                                       L = lambda W read for the open connections)
//
// which is the ODE C' = lambda - C / session, a first-order lag: C(t) = lambda session
// (1 - e^{-t / session}), rising to the fixed point C* = lambda session (Little's law again:
// 100 connects/s staying 1000 s hold 100,000 connections). The numbers below: the client offers
// 200/s, half of it `connect` (lambda = 100/s), `session: 1000s`, so C* = 100k and the time
// constant is 1000 s: C(1000 s) = 100k (1 - e^{-1}) = 63.2k.
//
// DISCRETISATION PINNED: the record of step k holds the level the step STARTED with, C(k), and
// C(0) = 0 for a gateway that starts empty; every load a step puts on a node that depends on the
// level (the heartbeats) uses that same C(k). The Euler recurrence above is pinned EXACTLY (to
// 1e-9 of the level), the exponential only within 1 % (Euler at dt = 1 s is 0.03 % low at
// t = 1000 s).
//
// Heartbeats: each open connection pings every `heartbeat` seconds, so the gateway's `heartbeat`
// op carries C / heartbeat requests/s (20k/s at C = 100k and 5 s, the book's chat presence), added
// to whatever the clients send under that name. The gateway's own demand is every request that
// reaches it: connects + messages + heartbeats + deliveries. To see the heartbeat load alone the
// gateway's `heartbeat` op has a step to a `presence` service, whose demand is then exactly the
// heartbeat load.
//
// The cap is `max_connections x replicas`: a connect that finds no room is refused (a rejection,
// like a rate limiter's 429, not a drop). With the level at the cap the connects that get in are
// the disconnects (the room each frees), so rejected = connects - C / session
// (100 - 50k / 1000 = 50/s at a cap of 50k).
//
// Not modelled here (and not pinned): the reconnect storm of a `node_down` (phase 3 Task 15).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../support/explain_lines.hpp"

// C++20 `import`: brings the module's exported names into this translation unit without
// textual inclusion; placed after the #includes because GCC 14 reports redefinitions
// when a std header is included after a module that included it in its global module fragment.
import simsys.core;
import simsys.engine;
import simsys.graph;
import simsys.model;
import simsys.report;
import simsys.test_fixtures;

// A note for test authors: `Approx(x).margin(m)` passes when the value is within the
// absolute margin m OR within Approx's default relative epsilon (about 1.19e-5), so a
// margin can only loosen a check, never tighten it. For a tight check use WithinRel /
// WithinAbs, which apply only the tolerance you give them.
using namespace std::chrono_literals;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using simsys::Rate;
using simsys::SimTime;
using simsys::engine::Simulation;
using simsys::engine::StepRecord;
using simsys::graph::Graph;
using simsys::model::LbPolicy;
using simsys::model::LinkSpec;
using simsys::model::NodeSpec;
using simsys::model::NodeType;
using simsys::model::Scenario;
using simsys::model::Step;
using simsys::model::Workload;
// Shared builders (tests/support/fixtures.cppm).
using simsys::test_support::explain_of;
using simsys::test_support::records_of;
using simsys::test_support::simulation_of;
// The explain lines (tests/support/explain_lines.hpp).
using simsys::test_support::check_line;
using simsys::test_support::node_line;

namespace {

// ---- The picture ------------------------------------------------------------------------------
//
//   u (client, 200/s: connect 50 %, send 50 %)  ->  gw (ws_gateway)  ->  q        (send -> publish)
//                                                                     ->  presence (heartbeat ->
//                                                                     beat)
//
// Every link is zero-latency, so what a step sends arrives in the same step; q and presence are
// services of 1M/s, far above any load here, so nothing is ever dropped and a node's demand is
// exactly what the gateway sent it. Node indices: u 0, gw 1, q 2, presence 3.
struct Shape {
  std::vector<std::pair<std::string, double>> client_ops{{"connect", 0.5}, {"send", 0.5}};
  double client_rate{200};
  std::optional<SimTime> session{1000s};
  std::optional<SimTime> heartbeat{5s};
  std::int64_t max_connections{200'000};
  int replicas{1};
  std::optional<std::int64_t> connections{};  // a fixed level (spec: the google-drive case)
  std::optional<double> fanout{};
  std::vector<Step> send_steps{Step{.to = "q", .op = "publish"}};
  SimTime step{100ms};
  Rate capacity{1'000'000};
};

constexpr std::size_t client_index = 0;
constexpr std::size_t gateway_index = 1;
constexpr std::size_t q_index = 2;
constexpr std::size_t presence_index = 3;

// C++20 designated initializers: `.name = ...` names each member being set, in declaration
// order, and leaves the rest at their defaults. Replaces positional braces (which break
// silently when members are reordered); here a scenario reads like the YAML it stands for.
Scenario gateway_scenario(const Shape& shape) {
  Scenario s{.name = "gateway", .version = 1, .duration = 60s, .step = shape.step};
  auto client_ops = shape.client_ops;
  // C++20 std::ranges::sort: takes the range itself instead of an iterator pair; the workload's
  // op shares are sorted by op name (a client built in code keeps the loader's order).
  std::ranges::sort(client_ops);
  NodeSpec gateway{.id = "gw",
                   .type = NodeType::ws_gateway,
                   .replicas = shape.replicas,
                   .capacity = shape.capacity,
                   .latency = 500us,
                   .ops = {{"deliver", {}},
                           {"heartbeat", {Step{.to = "presence", .op = "beat"}}},
                           {"send", shape.send_steps}}};
  // The gateway's own keys are assigned, not designated, so that the order the fields are
  // declared in NodeSpec does not matter here.
  gateway.max_connections = shape.max_connections;
  if (shape.heartbeat) gateway.heartbeat = *shape.heartbeat;
  if (shape.session) gateway.session = *shape.session;
  if (shape.fanout) gateway.fanout = *shape.fanout;
  if (shape.connections) gateway.connections = *shape.connections;
  s.nodes = {
      NodeSpec{.id = "u",
               .type = NodeType::client,
               .workload =
                   Workload{.curve = {.base = Rate{shape.client_rate}}, .op_shares = client_ops}},
      std::move(gateway),
      NodeSpec{.id = "q", .type = NodeType::service, .capacity = Rate{1'000'000}, .latency = 1ms},
      NodeSpec{.id = "presence",
               .type = NodeType::service,
               .capacity = Rate{1'000'000},
               .latency = 1ms}};
  s.links = {LinkSpec{.from = "u", .to = "gw", .latency = 0s},
             LinkSpec{.from = "gw", .to = "q", .latency = 0s},
             LinkSpec{.from = "gw", .to = "presence", .latency = 0s}};
  return s;
}

Graph graph_of(const Scenario& s) {
  auto g = Graph::build(s);
  INFO((g ? "" : g.error().message));
  REQUIRE(g.has_value());
  return std::move(*g);
}

// The first `steps` steps of a fresh simulation, at the scenario's own step `dt` (the engine
// requires it: records_of in the fixtures is for the default 100 ms).
std::vector<StepRecord> run_steps(const Graph& g, int steps, SimTime dt) {
  auto sim = simulation_of(g);
  std::vector<StepRecord> records;
  records.reserve(static_cast<std::size_t>(steps));
  for (int k = 0; k < steps; ++k) records.push_back(sim.step(k * dt, dt));
  return records;
}

// The level of the Euler recurrence C(k+1) = min(C(k) + (lambda - C(k) / session) dt, cap) from
// C(0) = 0, for k = 0 .. steps - 1: what record k must hold.
std::vector<double> euler_levels(int steps, double dt_s, double connects_per_s = 100.0,
                                 double session_s = 1000.0,
                                 double cap = std::numeric_limits<double>::infinity()) {
  std::vector<double> levels;
  double level = 0.0;
  for (int k = 0; k < steps; ++k) {
    levels.push_back(level);
    level = std::min(level + (connects_per_s - level / session_s) * dt_s, cap);
  }
  return levels;
}

// `actual` is `expected` to 1e-9 of it (an absolute tolerance of 1e-9 below 1, so that the empty
// level 0 can be pinned).
void require_close(double actual, double expected) {
  REQUIRE_THAT(actual, WithinAbs(expected, 1e-9 * std::max(1.0, std::abs(expected))));
}

double level_of(const StepRecord& r) {
  const auto& level = r.nodes[gateway_index].connections;
  REQUIRE(level.has_value());
  return *level;
}

double demand_of(const StepRecord& r, std::size_t node) {
  return r.nodes[node].demand_per_s;
}

const simsys::engine::ResponseSample& response_of(const StepRecord& r, std::string_view op) {
  // C++20 std::ranges::find with a projection: the response whose `op` member equals `op`,
  // replacing std::find_if with a lambda that reads the member by hand.
  const auto found = std::ranges::find(r.responses, op, &simsys::engine::ResponseSample::op);
  REQUIRE(found != r.responses.end());
  return *found;
}

}  // namespace

// ---- The level: a first-order lag -------------------------------------------------------------

// The recurrence at 1 s steps, exactly: C(0) = 0 (the gateway starts empty and the record shows
// the level the step started with), then C(k+1) = C(k) + (100 - C(k) / 1000) x 1.
//   C(1) = 100, C(2) = 100 + 99.9 = 199.9, C(3) = 199.9 + 99.8001 = 299.7001.
TEST_CASE("the connection level follows the Euler recurrence C + (connects - C / session) dt",
          "[theory]") {
  const auto g = graph_of(gateway_scenario(Shape{.step = 1s}));
  const auto records = run_steps(g, 60, 1s);
  const auto expected = euler_levels(60, 1.0);
  require_close(expected[3], 299.7001);  // the recurrence itself, by hand
  for (std::size_t k = 0; k < records.size(); ++k) {
    CAPTURE(k);
    require_close(level_of(records[k]), expected[k]);
  }
}

// The same recurrence at the default step of 100 ms: dt is a parameter of the recurrence, not a
// constant, so each step adds (100 - C / 1000) x 0.1: C(1) = 10, C(2) = 10 + 9.999 = 19.999.
TEST_CASE("the same recurrence holds at the default 100 ms step", "[theory]") {
  const auto g = graph_of(gateway_scenario(Shape{}));
  const auto records = records_of(g, 40);
  const auto expected = euler_levels(40, 0.1);
  require_close(expected[2], 19.999);
  for (std::size_t k = 0; k < records.size(); ++k) {
    CAPTURE(k);
    require_close(level_of(records[k]), expected[k]);
  }
}

// One time constant, session = 1000 s: C(1000 s) = 100k (1 - e^{-1}) = 63.212k, the napkin 63.2 %
// of the fixed point. The engine's Euler step at dt = 1 s is within 1 % of the exponential (0.03 %
// low: (1 - 1/1000)^1000 = 0.36770 against e^{-1} = 0.36788) and exactly on its own recurrence.
TEST_CASE("after one session length the level has risen to 63.2 percent of its fixed point",
          "[theory]") {
  const auto g = graph_of(gateway_scenario(Shape{.step = 1s}));
  const auto records = run_steps(g, 1001, 1s);  // records 0 .. 1000: the one at t = 1000 s is last
  const double closed_form = 100'000.0 * (1.0 - std::exp(-1.0));
  REQUIRE_THAT(level_of(records[1000]), WithinRel(closed_form, 0.01));
  REQUIRE_THAT(level_of(records[1000]), WithinRel(63'200.0, 0.01));  // the napkin number
  require_close(level_of(records[1000]), euler_levels(1001, 1.0)[1000]);
}

// The fixed point C* = lambda x session = 100/s x 1000 s = 100k. The lag has covered all but
// e^{-4.7} = 0.9 % of the way after 4700 s (NOT 4600 s as the plan text says: e^{-4.6} is 1.005 %,
// just outside 1 %). 1 s steps, to keep the run to 4700 steps. The level approaches from below and
// never overshoots (a first-order lag has no overshoot).
TEST_CASE("the level settles at connect rate times session: 100k within one percent at 4700 s",
          "[theory]") {
  const auto g = graph_of(gateway_scenario(Shape{.step = 1s}));
  const auto records = run_steps(g, 4701, 1s);
  REQUIRE_THAT(level_of(records[4700]), WithinRel(100'000.0, 0.01));
  const double closed_form = 100'000.0 * (1.0 - std::exp(-4.7));
  REQUIRE_THAT(level_of(records[4700]), WithinRel(closed_form, 0.01));
  for (const auto& record : records) REQUIRE(level_of(record) < 100'000.0);
}

// `estimate` is the steady state of the same model: the level at its fixed point C* = 100k.
TEST_CASE("estimate reports the fixed-point level, connect rate times session", "[theory]") {
  const auto g = graph_of(gateway_scenario(Shape{}));
  const auto e = simulation_of(g).estimate();
  REQUIRE(e.converged);
  REQUIRE_THAT(level_of(e), WithinRel(100'000.0, 1e-6));
}

// `session` absent: 30 minutes (chosen, the plan says: a typical mobile chat session; the book
// gives none), so C* = 100/s x 1800 s = 180k. The cap is raised to 1M so that it does not bind.
TEST_CASE("a gateway with no session stated holds connect rate times 30 minutes: 180k",
          "[theory]") {
  const auto g =
      graph_of(gateway_scenario(Shape{.session = std::nullopt, .max_connections = 1'000'000}));
  const auto e = simulation_of(g).estimate();
  REQUIRE_THAT(level_of(e), WithinRel(180'000.0, 1e-6));
}

// Only a gateway has a level; every other node's `connections` is empty (like a queue's lag), and
// a gateway's, even the empty one of the first step, is a number.
TEST_CASE("only the gateway has a connections level, every other node's is empty", "[theory]") {
  const auto g = graph_of(gateway_scenario(Shape{}));
  const auto r = records_of(g, 1).front();
  REQUIRE(r.nodes[gateway_index].connections.has_value());
  REQUIRE_FALSE(r.nodes[client_index].connections.has_value());
  REQUIRE_FALSE(r.nodes[q_index].connections.has_value());
  REQUIRE_FALSE(r.nodes[presence_index].connections.has_value());
}

// ---- Heartbeats --------------------------------------------------------------------------------

// The load on `presence` is the heartbeat load alone, C / heartbeat = C / 5 requests/s, step by
// step on the recurrence's level (20k/s at the fixed point). The gateway itself is offered every
// request: the 100/s of connects, the 100/s of messages and C / 5 of heartbeats.
TEST_CASE("heartbeats load presence with C over 5 requests a second and the gateway with them",
          "[theory]") {
  const auto g = graph_of(gateway_scenario(Shape{.step = 1s}));
  const auto records = run_steps(g, 60, 1s);
  const auto levels = euler_levels(60, 1.0);
  for (std::size_t k = 0; k < records.size(); ++k) {
    CAPTURE(k);
    require_close(demand_of(records[k], presence_index), levels[k] / 5.0);
    require_close(demand_of(records[k], gateway_index), 100.0 + 100.0 + levels[k] / 5.0);
    require_close(demand_of(records[k], q_index), 100.0);  // the messages: nothing else reaches q
  }
}

// At the fixed point, 100k connections beating every 5 s are 20k requests a second, a hundred
// times the 200/s the users actually send: why a chat system sizes its gateway tier, and the
// presence service behind it, for heartbeats.
TEST_CASE("at the fixed point the heartbeats are 20k a second, 100 times the users' own requests",
          "[theory]") {
  const auto g = graph_of(gateway_scenario(Shape{}));
  const auto e = simulation_of(g).estimate();
  REQUIRE_THAT(demand_of(e, presence_index), WithinRel(20'000.0, 1e-6));
  REQUIRE_THAT(demand_of(e, gateway_index), WithinRel(20'200.0, 1e-6));
}

// The heartbeat load is added to what the clients send under that name: here the users also send
// 50 heartbeats/s themselves (an app that pings from the foreground), so presence is offered
// 50 + C / 5. The client's 250/s split 40 % connect, 20 % heartbeat, 40 % send keeps the connect
// rate at 100/s, so the level is the same recurrence.
TEST_CASE("the gateway's heartbeat load is added to the heartbeats the clients send", "[theory]") {
  const auto g = graph_of(gateway_scenario(Shape{
      .client_ops = {{"connect", 0.4}, {"heartbeat", 0.2}, {"send", 0.4}},
      .client_rate = 250,
      .step = 1s,
  }));
  const auto records = run_steps(g, 30, 1s);
  const auto levels = euler_levels(30, 1.0);
  for (std::size_t k = 0; k < records.size(); ++k) {
    CAPTURE(k);
    require_close(demand_of(records[k], presence_index), 50.0 + levels[k] / 5.0);
    require_close(level_of(records[k]), levels[k]);
  }
}

// Heartbeats are work: a gateway of 100k/s at fixed point offered 200 + 20k = 20.2k/s runs at
// rho = demand / capacity = 0.202 (the tests' gateway has 500 us service and c = 50 servers: rho
// is the same fraction whatever c is). Step by step on the recurrence's level at t = 100 s.
TEST_CASE("the gateway's utilisation counts the heartbeats: rho is demand over capacity",
          "[theory]") {
  const auto g = graph_of(gateway_scenario(Shape{.step = 1s, .capacity = Rate{100'000}}));
  const auto records = run_steps(g, 101, 1s);
  const double level = euler_levels(101, 1.0)[100];
  REQUIRE_THAT(records[100].nodes[gateway_index].rho,
               WithinRel((200.0 + level / 5.0) / 100'000.0, 1e-9));
}

// ---- The cap ----------------------------------------------------------------------------------

// max_connections 50k: the level reaches 50k at t = 693 s (100k (1 - e^{-t/1000}) = 50k) and stays.
// There the 100 connects/s meet 50k / 1000 = 50 disconnects/s: 50 get in (the room the
// disconnects free) and 100 - 50 = 50 per second are refused, counted as REJECTIONS (a refusal by
// policy, like a limiter's 429), not drops. Before the cap nothing is refused.
TEST_CASE("with a cap of 50k the level stops at 50k and 50 connects a second are rejected",
          "[theory]") {
  const auto g = graph_of(gateway_scenario(Shape{.max_connections = 50'000, .step = 1s}));
  const auto records = run_steps(g, 1500, 1s);
  for (const auto& record : records) REQUIRE(level_of(record) <= 50'000.0 * (1.0 + 1e-12));
  require_close(level_of(records[1499]), 50'000.0);
  REQUIRE_THAT(records[1499].nodes[gateway_index].rejected_per_s, WithinRel(50.0, 1e-9));
  REQUIRE(records[1499].nodes[gateway_index].dropped_per_s == 0.0);
  // 100 s in, the level is 9.5k: no room problem, no rejection.
  REQUIRE(records[100].nodes[gateway_index].rejected_per_s == 0.0);
  // The heartbeats are those of the connections that are open: 50k / 5.
  REQUIRE_THAT(demand_of(records[1499], presence_index), WithinRel(10'000.0, 1e-9));
}

// The cap is per replica: 2 replicas of 25k hold what one of 50k holds.
TEST_CASE("the cap is max_connections times replicas: 2 replicas of 25k hold 50k", "[theory]") {
  const auto g =
      graph_of(gateway_scenario(Shape{.max_connections = 25'000, .replicas = 2, .step = 1s}));
  const auto records = run_steps(g, 1500, 1s);
  for (const auto& record : records) REQUIRE(level_of(record) <= 50'000.0 * (1.0 + 1e-12));
  require_close(level_of(records[1499]), 50'000.0);
  REQUIRE_THAT(records[1499].nodes[gateway_index].rejected_per_s, WithinRel(50.0, 1e-9));
}

// What a refused connect means for the user: the client's `connect` is answered for the 50/s that
// got in and not for the 50/s that were refused, as a refused request is not answered anywhere
// else (spec §17 entry 8); its `send` is untouched.
TEST_CASE("a refused connect is not answered: the client gets 50 of its 100 connects a second",
          "[theory]") {
  const auto g = graph_of(gateway_scenario(Shape{.max_connections = 50'000, .step = 1s}));
  const auto records = run_steps(g, 1500, 1s);
  const auto& connect = response_of(records[1499], "connect");
  REQUIRE_THAT(connect.offered.per_second, WithinRel(100.0, 1e-9));
  REQUIRE_THAT(connect.answered.per_second, WithinRel(50.0, 1e-9));
  REQUIRE_THAT(response_of(records[1499], "send").answered.per_second, WithinRel(100.0, 1e-9));
}

// `estimate` at a cap that binds: the level is the cap and the refusals are the steady 50/s.
TEST_CASE("estimate with a binding cap reports the cap as the level and 50 rejections a second",
          "[theory]") {
  const auto g = graph_of(gateway_scenario(Shape{.max_connections = 50'000}));
  const auto e = simulation_of(g).estimate();
  REQUIRE_THAT(level_of(e), WithinRel(50'000.0, 1e-6));
  REQUIRE_THAT(e.nodes[gateway_index].rejected_per_s, WithinRel(50.0, 1e-6));
}

// ---- A fixed level ----------------------------------------------------------------------------

// google-drive's long poll: 2M connections held open by devices nobody here simulates, so the
// gateway states the level (`connections: 2M`) instead of a `connect` op. The level is 2M from
// the first step and does not decay (no disconnects: it is fixed, not a balance). The
// heartbeat is 60 s: 2M / 60 = 33.33k requests a second to presence, plus the 100/s of messages.
TEST_CASE("a fixed 2M connections is the level from the first step and beats 33.33k a second",
          "[theory]") {
  const auto g = graph_of(gateway_scenario(Shape{.client_ops = {{"send", 1.0}},
                                                 .client_rate = 100,
                                                 .heartbeat = 60s,
                                                 .max_connections = 2'000'000,
                                                 .connections = 2'000'000}));
  const auto records = records_of(g, 50);
  for (const auto& record : records) {
    require_close(level_of(record), 2'000'000.0);
    REQUIRE_THAT(demand_of(record, presence_index), WithinRel(2'000'000.0 / 60.0, 1e-9));
    REQUIRE_THAT(demand_of(record, gateway_index), WithinRel(100.0 + 2'000'000.0 / 60.0, 1e-9));
    REQUIRE(record.nodes[gateway_index].rejected_per_s == 0.0);
  }
  REQUIRE_THAT(demand_of(records[0], presence_index), WithinRel(33'333.33, 1e-6));  // the napkin
}

TEST_CASE("estimate reports a fixed level as it is", "[theory]") {
  const auto g = graph_of(gateway_scenario(Shape{.client_ops = {{"send", 1.0}},
                                                 .client_rate = 100,
                                                 .heartbeat = 60s,
                                                 .max_connections = 2'000'000,
                                                 .connections = 2'000'000}));
  const auto e = simulation_of(g).estimate();
  REQUIRE_THAT(level_of(e), WithinRel(2'000'000.0, 1e-9));
  REQUIRE_THAT(demand_of(e, presence_index), WithinRel(2'000'000.0 / 60.0, 1e-9));
}

// ---- Fan-out and deliveries -------------------------------------------------------------------

// `fanout` is the average recipients per message (1:1 and small groups: 1.4 in the chat design):
// each message the gateway accepts sends `fanout` requests down each of the `send` op's steps,
// as a `factor` of the step. 100 messages/s x 1.4 = 140/s reach q, whatever the level is; a step
// that already has a factor multiplies with it (x 2 x 1.4 = 280); no fanout stated is a factor of
// 1. The heartbeat step is not a message: presence is not multiplied.
TEST_CASE("fanout multiplies a message op's steps: 1.4 sends q 1.4 times the send rate",
          "[theory]") {
  SECTION("fanout 1.4") {
    const auto g = graph_of(gateway_scenario(Shape{.fanout = 1.4, .step = 1s}));
    const auto records = run_steps(g, 20, 1s);
    const auto levels = euler_levels(20, 1.0);
    for (std::size_t k = 0; k < records.size(); ++k) {
      CAPTURE(k);
      require_close(demand_of(records[k], q_index), 140.0);
      require_close(demand_of(records[k], presence_index), levels[k] / 5.0);  // not multiplied
    }
  }
  SECTION("the step's own factor multiplies with it") {
    const auto g = graph_of(gateway_scenario(
        Shape{.fanout = 1.4, .send_steps = {Step{.to = "q", .op = "publish", .factor = 2.0}}}));
    const auto first = records_of(g, 1).front();
    REQUIRE_THAT(demand_of(first, q_index), WithinRel(280.0, 1e-9));
  }
  SECTION("no fanout stated: a factor of 1") {
    const auto g = graph_of(gateway_scenario(Shape{}));
    const auto first = records_of(g, 1).front();
    REQUIRE_THAT(demand_of(first, q_index), WithinRel(100.0, 1e-9));
  }
}

// `deliver` is terminal: the message is pushed over the open connection, so the gateway serves it
// and nothing leaves it. The users here send 100/s of `deliver` (a worker pool's `then` step
// would in the chat design) and nothing else: q is offered 0, presence only the heartbeats, and
// the gateway is offered the 100 connects, the 100 deliveries and the heartbeats.
TEST_CASE("a deliver makes no hops: q gets nothing and presence only the heartbeats", "[theory]") {
  const auto g = graph_of(
      gateway_scenario(Shape{.client_ops = {{"connect", 0.5}, {"deliver", 0.5}}, .step = 1s}));
  const auto records = run_steps(g, 20, 1s);
  const auto levels = euler_levels(20, 1.0);
  for (std::size_t k = 0; k < records.size(); ++k) {
    CAPTURE(k);
    require_close(demand_of(records[k], q_index), 0.0);
    require_close(demand_of(records[k], presence_index), levels[k] / 5.0);
    require_close(demand_of(records[k], gateway_index), 100.0 + 100.0 + levels[k] / 5.0);
  }
}

// ---- What the engine refuses ------------------------------------------------------------------
//
// A gateway needs a level: either a `connect` op that reaches it (the level is then a balance of
// connects and disconnects) or a fixed `connections`, not both (two sources of the one number).
// Whether a `connect` op REACHES a gateway is a fact of the graph (through links, balancers and
// steps), known only once the graph is built, and the shipped chat-system and google-drive have
// neither today (their phase-3 tasks give them one), so the loader and Graph::build accept such a
// gateway, explain still describes it, and the refusal is Simulation::create's, in the words of
// its other "cannot run this" messages.

namespace {
std::string refusal_of(const Scenario& s) {
  const auto g = graph_of(s);
  const auto sim = Simulation::create(g);
  REQUIRE_FALSE(sim.has_value());
  return sim.error().message;
}
}  // namespace

TEST_CASE("a gateway no connect op reaches and that states no connections cannot run", "[theory]") {
  SECTION("the clients send no connect at all") {
    REQUIRE(refusal_of(gateway_scenario(Shape{.client_ops = {{"send", 1.0}}})) ==
            "node 'gw': a ws_gateway needs a client op named 'connect' or a fixed 'connections'");
  }
  SECTION("a connect that goes to another node does not reach the gateway") {
    auto s = gateway_scenario(Shape{});
    s.links[0].ops = {"send"};  // u -> gw carries the sends only
    s.nodes.push_back(NodeSpec{.id = "edge", .type = NodeType::service});
    s.links.push_back(LinkSpec{.from = "u", .to = "edge", .ops = {"connect"}, .latency = 0s});
    REQUIRE(refusal_of(s) ==
            "node 'gw': a ws_gateway needs a client op named 'connect' or a fixed 'connections'");
  }
}

TEST_CASE("a gateway with a connect op reaching it and a fixed connections cannot run",
          "[theory]") {
  REQUIRE(refusal_of(gateway_scenario(Shape{.connections = 100'000})) ==
          "node 'gw': a ws_gateway takes a 'connect' op or a fixed 'connections', not both");
}

// The converse: a `connect` op that reaches the gateway through a load balancer counts (the
// balancer forwards every op it is sent), and a fixed `connections` needs no connect.
TEST_CASE("a connect op that reaches the gateway through a load balancer is enough", "[theory]") {
  auto s = gateway_scenario(Shape{});
  s.nodes.push_back(
      NodeSpec{.id = "lb", .type = NodeType::load_balancer, .policy = LbPolicy::round_robin});
  s.links[0] = LinkSpec{.from = "u", .to = "lb", .latency = 0s};
  s.links.push_back(LinkSpec{.from = "lb", .to = "gw", .latency = 0s});
  const auto g = graph_of(s);
  const auto sim = Simulation::create(g);
  INFO((sim ? "" : sim.error().message));
  REQUIRE(sim.has_value());
}

// ---- The reports' `connections` ----------------------------------------------------------------
//
// Where the level shows (phase 3 Task 14):
//  - the node table: a `connections` column, the LAST labelled one (after `peak_lag`), only when
//    the scenario has a ws_gateway (every other run prints the table it always did); the gateway's
//    row holds the level at the END of the run, "-" on every other row;
//  - the JSON node object: `connections` (the level at the end) and `connections_peak` (the
//    largest level of any observed step), numbers on a gateway and null on every other node;
//  - the series CSV: a `connections` column, the LAST one (after `queue_lag_s`), empty on every
//    row but a gateway's;
//  - explain: the gateway's `nodes:` line ends with the level's steady value (below).

namespace {
// A hand-made record: the gateway holds `level`, every other node is a default sample.
StepRecord record_with_level(SimTime t, double level) {
  StepRecord r{.t = t, .nodes = std::vector<simsys::engine::NodeSample>(4), .responses = {}};
  r.nodes[gateway_index].connections = level;
  return r;
}

simsys::report::Summary summary_of_levels(const Graph& g, std::initializer_list<double> levels) {
  simsys::report::Summary summary{g, SimTime{0}};
  SimTime t{0};
  for (const double level : levels) {
    summary.observe(record_with_level(t, level));
    t += 100ms;
  }
  return summary;
}

std::vector<std::string> words_of_line_starting(const std::string& text, std::string_view first) {
  std::istringstream lines{text};
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream words{line};
    std::string word;
    std::vector<std::string> found;
    while (words >> word) found.push_back(word);
    if (!found.empty() && found.front() == first) return found;
  }
  FAIL("no line starts with " << first << " in:\n" << text);
  return {};
}

// The text of the JSON object of node `id`, from its `{"id": ...` to its closing brace.
std::string json_node_object(const std::string& json, std::string_view id) {
  const auto start = json.find("{\"id\": \"" + std::string{id} + "\"");
  REQUIRE(start != std::string::npos);
  return json.substr(start, json.find('}', start) - start);
}

// The text after `"key": ` in a node object, up to the next comma or the end.
std::string json_field_text(const std::string& object, std::string_view key) {
  const std::string field = "\"" + std::string{key} + "\": ";
  const auto at = object.find(field);
  INFO("key: " << key << " in " << object);
  REQUIRE(at != std::string::npos);
  const auto begin = at + field.size();
  return object.substr(begin, object.find(',', begin) - begin);
}

std::vector<std::string> split_commas(const std::string& line) {
  std::vector<std::string> fields;
  std::size_t begin = 0;
  while (true) {
    const auto comma = line.find(',', begin);
    if (comma == std::string::npos) break;
    fields.push_back(line.substr(begin, comma - begin));
    begin = comma + 1;
  }
  fields.push_back(line.substr(begin));
  return fields;
}
}  // namespace

// Levels 1000, 3000, 2000 over three steps: the end is 2000 and the peak 3000, which the table
// (the end) and the JSON (both) tell apart. 2000 prints as "2k", the table's SI prefix.
TEST_CASE("the node table has a connections column holding the gateway's level at the end",
          "[theory]") {
  const auto g = graph_of(gateway_scenario(Shape{}));
  const auto table =
      simsys::report::render_table(summary_of_levels(g, {1000, 3000, 2000}), {}, false);
  CAPTURE(table);
  const std::vector<std::string> header{"node",         "mean_rho", "peak_rho",
                                        "peak_backlog", "dropped",  "rejected",
                                        "red_s",        "peak_lag", "connections"};
  REQUIRE(words_of_line_starting(table, "node") == header);
  const auto at =
      static_cast<std::size_t>(std::ranges::find(header, "connections") - header.begin());
  const auto gateway = words_of_line_starting(table, "gw");
  REQUIRE(gateway.size() > at);
  REQUIRE(gateway[at] == "2k");
  const auto presence = words_of_line_starting(table, "presence");
  REQUIRE(presence.size() > at);
  REQUIRE(presence[at] == "-");
}

// A run with no gateway prints the table of phase 2: no new column.
TEST_CASE("a table with no gateway has no connections column", "[theory]") {
  const auto g = graph_of(simsys::test_support::mm1(500));
  simsys::report::Summary summary{g, SimTime{0}};
  summary.observe(StepRecord{
      .t = SimTime{0}, .nodes = std::vector<simsys::engine::NodeSample>(2), .responses = {}});
  const auto table = simsys::report::render_table(summary, {}, false);
  CAPTURE(table);
  // C++23 std::string::contains: a substring test in one call, replacing
  // `find(...) != std::string::npos`.
  REQUIRE_FALSE(table.contains("connections"));
}

TEST_CASE("the JSON gateway object has connections 2000 at the end and connections_peak 3000",
          "[theory]") {
  const auto g = graph_of(gateway_scenario(Shape{}));
  const auto json = simsys::report::render_json(summary_of_levels(g, {1000, 3000, 2000}), {});
  CAPTURE(json);
  const auto gateway = json_node_object(json, "gw");
  REQUIRE(std::stod(json_field_text(gateway, "connections")) == 2000.0);
  REQUIRE(std::stod(json_field_text(gateway, "connections_peak")) == 3000.0);
}

TEST_CASE("the JSON gives every node that is not a gateway null connections", "[theory]") {
  const auto g = graph_of(gateway_scenario(Shape{}));
  const auto json = simsys::report::render_json(summary_of_levels(g, {1000, 3000, 2000}), {});
  CAPTURE(json);
  for (const auto* id : {"u", "q", "presence"}) {
    CAPTURE(id);
    const auto object = json_node_object(json, id);
    REQUIRE(json_field_text(object, "connections") == "null");
    REQUIRE(json_field_text(object, "connections_peak") == "null");
  }
}

// The CSV column is the last, so a plot script that reads columns by position loses nothing:
// the header is pinned whole, and the cell is the level at 6 significant digits on the gateway's
// row, empty on every other row (a 0 would claim a level a service cannot have). A real run, so
// that the cell is the engine's number: the record of step 3 at 1 s steps holds 299.7001.
TEST_CASE("the node series ends with a connections column, filled on the gateway's rows only",
          "[theory]") {
  const auto g = graph_of(gateway_scenario(Shape{.step = 1s}));
  std::ostringstream out;
  simsys::report::SeriesWriter writer{out, g};
  const auto records = run_steps(g, 4, 1s);
  for (const auto& record : records) writer.observe(record);
  const auto text = out.str();
  CAPTURE(text);
  REQUIRE(text.substr(0, text.find('\n')) ==
          "t_s,node,demand_per_s,drain_per_s,rho,backlog,drops_per_s,dead_lettered_per_s,"
          "rejected_per_s,latency_ms,zone,queue_lag_s,connections");
  std::istringstream lines{text};
  std::string line;
  std::getline(lines, line);  // the header
  std::size_t gateway_rows = 0;
  while (std::getline(lines, line)) {
    const auto cells = split_commas(line);
    CAPTURE(line);
    REQUIRE(cells.size() == 13);
    if (cells[1] != "gw") {
      REQUIRE(cells[12] == "");
      continue;
    }
    REQUIRE(cells[11] == "");  // queue_lag_s: a gateway has no lag
    REQUIRE_THAT(std::stod(cells[12]),
                 WithinRel(euler_levels(4, 1.0)[gateway_rows], 1e-5));  // 6 significant digits
    ++gateway_rows;
  }
  REQUIRE(gateway_rows == 4);
}

// ---- explain ----------------------------------------------------------------------------------
//
// The gateway's `nodes:` line is a service's line (placement, capacity, service time, c, queue
// limit, variability, burstiness) plus, at its end, what the level will be and what it costs:
//   `  connections: C*=100k (connect 100/s × session 16.7m), heartbeat 20k/s, cap 200k`
// where C* = connect rate × session is the fixed point (Little's law), the connect rate the
// client's peak rate for the ops named `connect` that reach the gateway (100/s), the session
// printed as every duration is (1000 s is 16.7m), the heartbeat the load at C* (C* / 5 s) and the
// cap max_connections × replicas. A fixed level reads `connections: 2M (fixed), heartbeat 33.33k/s,
// cap 2M`; a session left to the default is marked `session 30m (default)`.
// The ids are u, gw, q and presence, so the id column is 8 wide and the type column 10
// (ws_gateway).

namespace {
// (node_line returns the line with its two-space indent.)
const std::string gateway_line_start =
    "  gw        ws_gateway  dc=main  replicas=1  capacity=1M/s (scenario)  service_time=500us "
    "(scenario)  c=500 (implied)  queue_limit=1000 (default)  variability=1  burstiness=1";
}  // namespace

TEST_CASE("explain prints the gateway's steady level, heartbeat load and cap", "[theory]") {
  const auto text = explain_of(gateway_scenario(Shape{}));
  CAPTURE(text);
  REQUIRE(node_line(text, "gw") ==
          gateway_line_start +
              "  connections: C*=100k (connect 100/s × session 16.7m), heartbeat 20k/s, cap 200k");
}

TEST_CASE("explain prints a fixed level as fixed, with its heartbeat load and cap", "[theory]") {
  const auto text = explain_of(gateway_scenario(Shape{.client_ops = {{"send", 1.0}},
                                                      .heartbeat = 60s,
                                                      .max_connections = 2'000'000,
                                                      .connections = 2'000'000}));
  CAPTURE(text);
  REQUIRE(node_line(text, "gw") ==
          gateway_line_start + "  connections: 2M (fixed), heartbeat 33.33k/s, cap 2M");
}

TEST_CASE("explain marks a session left to the default of 30 minutes", "[theory]") {
  const auto text =
      explain_of(gateway_scenario(Shape{.session = std::nullopt, .max_connections = 1'000'000}));
  CAPTURE(text);
  // C* = 100/s x 1800 s = 180k, heartbeat 180k / 5 s = 36k/s.
  // C++20 std::string::ends_with: a suffix test in one call, replacing a compare of the tail.
  REQUIRE(node_line(text, "gw")
              .ends_with("  connections: C*=180k (connect 100/s × session 30m (default)), "
                         "heartbeat 36k/s, cap 1M"));
}

// Beyond the gateway's own line: the paper-load checks follow the gateway's flows, as the engine
// does. Heartbeats at the fixed point load the gateway (100 + 100 + 20k = 20.2k/s) and presence
// (20k/s), and the fan-out multiplies q's share (100 x 1.4 = 140/s); without it explain would
// call presence idle ("peak offered 0/s").
TEST_CASE("explain's checks include the heartbeat load and the fan-out", "[theory]") {
  const auto text = explain_of(gateway_scenario(Shape{.fanout = 1.4}));
  CAPTURE(text);
  REQUIRE(check_line(text, "gw").contains("peak offered 20.2k/s vs 1M/s total capacity"));
  REQUIRE(check_line(text, "presence").contains("peak offered 20k/s vs 1M/s total capacity"));
  REQUIRE(check_line(text, "q").contains("peak offered 140/s vs 1M/s total capacity"));
}
