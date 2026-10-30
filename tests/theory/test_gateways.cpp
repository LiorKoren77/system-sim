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
// The reconnect storm of a `node_down` (phase 3 Task 15) has its own section below.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
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
import simsys.session;
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

// `fanout` is the average recipients per message (1:1 and small groups: 1.4 in the chat design).
// Since phase 3 Task 15 it multiplies only a message op's steps to a MESSAGE QUEUE (ch. 12 copies a
// message once per recipient into the sync queues; the tests at the end of this file pin that):
// here every step of `send` goes to q, a service (the message store, the id generator), which sees
// one request per message, 100/s, whatever the fanout; a step's own factor still applies (x 2 =
// 200); no fanout stated changes nothing either. The heartbeat step is not a message: presence is
// not multiplied. (Task 14 pinned 140/s and 280/s here, the fan-out on every step.)
TEST_CASE("fanout leaves a message op's steps to a service alone: q gets the send rate once",
          "[theory]") {
  SECTION("fanout 1.4") {
    const auto g = graph_of(gateway_scenario(Shape{.fanout = 1.4, .step = 1s}));
    const auto records = run_steps(g, 20, 1s);
    const auto levels = euler_levels(20, 1.0);
    for (std::size_t k = 0; k < records.size(); ++k) {
      CAPTURE(k);
      require_close(demand_of(records[k], q_index), 100.0);
      require_close(demand_of(records[k], presence_index), levels[k] / 5.0);  // not multiplied
    }
  }
  SECTION("the step's own factor applies, the fanout does not") {
    const auto g = graph_of(gateway_scenario(
        Shape{.fanout = 1.4, .send_steps = {Step{.to = "q", .op = "publish", .factor = 2.0}}}));
    const auto first = records_of(g, 1).front();
    REQUIRE_THAT(demand_of(first, q_index), WithinRel(200.0, 1e-9));
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
// steps), known only once the graph is built (every shipped gateway has a level since phase 3:
// google-drive's fixed 2M in Task 16, chat-system's users' `connect` in Task 15), so the loader
// and Graph::build accept such a gateway, explain still describes it, and the refusal is
// Simulation::create's, in the words of its other "cannot run this" messages.

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

// `--replications N` adds `replications.per_seed`, each node's numbers as one list per field with
// an entry per seed; a gateway's two levels are among them, in the order of the run's own JSON
// (after peak_queue_lag_s), as lists of numbers on the gateway and of nulls elsewhere. Three
// "seeds" whose levels differ: the ends 2000, 4000, 1000 and the peaks 3000, 5000, 1500.
TEST_CASE("the per-seed JSON of replications lists the gateway's connections and its peak",
          "[theory]") {
  const auto g = graph_of(gateway_scenario(Shape{}));
  std::vector<simsys::report::Summary> runs;
  runs.push_back(summary_of_levels(g, {1000, 3000, 2000}));
  runs.push_back(summary_of_levels(g, {5000, 4000}));
  runs.push_back(summary_of_levels(g, {1500, 1000}));
  constexpr std::array<int, 3> seeds{1, 2, 3};
  const auto replicated = simsys::report::summarize_replications(runs, seeds);
  const auto json = simsys::report::render_replications_json(replicated, runs);
  CAPTURE(json);
  const auto per_seed = json.substr(json.find("\"per_seed\""));
  const auto gateway = json_node_object(per_seed, "gw");
  // C++20 std::string::ends_with: a suffix test in one call, replacing a compare of the tail.
  REQUIRE(
      gateway.ends_with("\"peak_queue_lag_s\": [null, null, null], "
                        "\"connections\": [2000, 4000, 1000], "
                        "\"connections_peak\": [3000, 5000, 1500]"));
  const auto presence = json_node_object(per_seed, "presence");
  REQUIRE(
      presence.ends_with("\"connections\": [null, null, null], "
                         "\"connections_peak\": [null, null, null]"));
}

// A replicated run with no gateway keeps the per-seed node objects it always had.
TEST_CASE("the per-seed JSON of a run with no gateway has no connections", "[theory]") {
  const auto g = graph_of(simsys::test_support::mm1(500));
  std::vector<simsys::report::Summary> runs;
  for (int seed = 0; seed < 3; ++seed) {
    runs.emplace_back(g, SimTime{0});
    runs.back().observe(StepRecord{
        .t = SimTime{0}, .nodes = std::vector<simsys::engine::NodeSample>(2), .responses = {}});
  }
  constexpr std::array<int, 3> seeds{1, 2, 3};
  const auto json = simsys::report::render_replications_json(
      simsys::report::summarize_replications(runs, seeds), runs);
  CAPTURE(json);
  REQUIRE_FALSE(json.contains("connections"));
}

// The CSV column is the last (after Task 17's deduplicated_per_s, inserted before latency_ms, it is
// still the last), so a plot script that reads columns by position loses nothing:
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
          "rejected_per_s,deduplicated_per_s,latency_ms,zone,queue_lag_s,connections");
  std::istringstream lines{text};
  std::string line;
  std::getline(lines, line);  // the header
  std::size_t gateway_rows = 0;
  while (std::getline(lines, line)) {
    const auto cells = split_commas(line);
    CAPTURE(line);
    REQUIRE(cells.size() == 14);
    if (cells[1] != "gw") {
      REQUIRE(cells[13] == "");
      continue;
    }
    REQUIRE(cells[12] == "");  // queue_lag_s: a gateway has no lag
    REQUIRE_THAT(std::stod(cells[13]),
                 WithinRel(euler_levels(4, 1.0)[gateway_rows], 1e-5));  // 6 significant digits
    ++gateway_rows;
  }
  REQUIRE(gateway_rows == 4);
}

// ---- explain ----------------------------------------------------------------------------------
//
// The gateway's `nodes:` line is a service's line (placement, capacity, service time, c, queue
// limit, variability, burstiness) plus, at its end, what the level will be and what it costs:
//   `  connections: C*=100k at the peak connect rate (connect 100/s × session 16.7m),
//    heartbeat 20k/s, cap 200k; settles in about 3 sessions (50m); this run lasts 1m`
// where C* = connect rate × session is the fixed point (Little's law), the connect rate the
// client's PEAK rate for the ops named `connect` that reach the gateway (100/s; the line says so,
// since a curve's off-peak level is lower), the session printed as every duration is (1000 s is
// 16.7m), the heartbeat the load at C* (C* / 5 s) and the cap max_connections × replicas. The
// level is a first-order lag of time constant `session` (95 % of the way after 3 of them,
// 1 − e^{−3}), so a run shorter than 3 sessions ends before it settles, and the line says how long
// each takes (the probe runs 60 s against 3 × 1000 s). A fixed level reads `connections: 2M
// (fixed), heartbeat 33.33k/s, cap 2M`; a session left to the default is marked `session 30m
// (default)`.
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
              "  connections: C*=100k at the peak connect rate (connect 100/s × session 16.7m), "
              "heartbeat 20k/s, cap 200k; settles in about 3 sessions (50m); this run lasts 1m");
}

// A run of at least 3 sessions (60 s against a session of 20 s) reaches its level: no note.
TEST_CASE("explain says nothing of settling when the run lasts 3 sessions or more", "[theory]") {
  const auto text = explain_of(gateway_scenario(Shape{.session = 20s}));
  CAPTURE(text);
  // C* = 100/s x 20 s = 2k, heartbeat 2k / 5 s = 400/s.
  REQUIRE(node_line(text, "gw") == gateway_line_start +
                                       "  connections: C*=2k at the peak connect rate (connect "
                                       "100/s × session 20s), heartbeat 400/s, cap 200k");
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
  REQUIRE(node_line(text, "gw")
              .ends_with("  connections: C*=180k at the peak connect rate (connect 100/s × "
                         "session 30m (default)), heartbeat 36k/s, cap 1M; settles in about 3 "
                         "sessions (1.5h); this run lasts 1m"));
}

// Beyond the gateway's own line: the paper-load checks follow the gateway's flows, as the engine
// does. Heartbeats at the fixed point load the gateway (100 + 100 + 20k = 20.2k/s) and presence
// (20k/s); without them explain would call presence idle ("peak offered 0/s"). q, a service, is
// sent each message once, 100/s, whatever the fan-out (Task 15; Task 14 pinned 140/s): the fan-out
// on paper is the message queue's, at the end of this file.
TEST_CASE("explain's checks include the heartbeat load and send a service each message once",
          "[theory]") {
  const auto text = explain_of(gateway_scenario(Shape{.fanout = 1.4}));
  CAPTURE(text);
  REQUIRE(check_line(text, "gw").contains("peak offered 20.2k/s vs 1M/s total capacity"));
  REQUIRE(check_line(text, "presence").contains("peak offered 20k/s vs 1M/s total capacity"));
  REQUIRE(check_line(text, "q").contains("peak offered 100/s vs 1M/s total capacity"));
}

// ---- The reconnect storm (phase 3 Task 15, spec §8.2) -------------------------------------------
//
// A `node_down` of r of a gateway's R replicas drops the connections those replicas held, C r / R,
// AT ONCE (a dead server's sockets are gone, ch. 12), and their users come back spread evenly over
// `reconnect_within` from the moment the fault CLEARS: the storm,
//
//     connects(t) = lambda + dropped / reconnect_within     for reconnect_within after at + for
//
// on top of the clients' own `connect` rate lambda. The gateway then serves, per second, its sends,
// its connects (34 times the usual ones in the probe below) and the heartbeats of the level as it
// recovers. The fault block's `after:` line names the storm (its size, how long, the gateway's peak
// UTILISATION), and the JSON's `after` object holds the same numbers.
//
// THE PROBE. Task 14's, run to its fixed point first: client 200/s (connect 100/s, send 100/s),
// `session: 1000s`, `heartbeat: 5s`, so C* = 100k; 1 s steps (the Euler recurrence at the top of
// this file); the fault starts at t = 5000 s, once the level is within 1 % of 100k:
//
//     C0 = C(5000 s) = 100k (1 - 0.999^5000) = 99,327.8888 (the recurrence, not the exponential)
//
// (100k would need 4700 s only to be within 1 %; at 5000 s the level is 0.67 % short of it.) The
// fault is `node_down` of the gateway for 60 s (every replica), `reconnect_within: 30s`, and the
// run goes on past the storm. Differences from Task 14's probe, each for a reason:
//  - the gateway's capacity is 32k/s (c = 16 servers of 500 us), not 1M/s: at the fixed point it
//    serves 200 + C / 5 = 20.2k/s (rho 0.63) and in the storm up to 23.0k/s (0.72), so nothing is
//    ever refused for want of capacity and the recurrence below holds exactly. (The plan's "peak
//    343 %" assumed a gateway of 1k/s: with the heartbeats at the fixed point that gateway would be
//    at 2000 % all the time.) Probe B below overloads a gateway on purpose;
//  - `queue_limit: 1` on the gateway: while the gateway is down its room (1000 by default) would
//    hold the fault's first seconds of requests, and the first step after the fault would drain
//    them, connects included, which adds up to a thousand to the level (1 %) and has nothing to do
//    with the storm. The room is tested elsewhere; here it is one request, and the level is pinned
//    to within 2 connections of the recurrence.
//
// WHAT IS PINNED (and what is the implementer's choice):
//  - the discretisation of Task 14 holds: record k shows the level step k STARTED with. The fault's
//    first step, k = 5000, starts with the level already dropped, so record 5000 shows 0 (the plan:
//    "C falls to 0 at the first fault step"); record 4999 still shows C(4999). The dropped
//    connections are the level the fault-free recurrence gives at k = 5000: C0 (the twin's level);
//  - while the gateway is down it serves nothing, so connects are not admitted and the level stays
//    0, and every `send` and `connect` the clients offer is lost (answered 0). Not pinned: the
//    first fault step (the room of one request may hold one) and what happens to a request that
//    the room holds;
//  - after the fault the gateway's `connect` DEMAND is 100 + C0 / 30 = 3410.93/s for the 30 steps
//    from t = 5060 s (34.1 times its usual 100/s; the plan's 34.3 assumed C0 = 100k), then 100/s
//    again. The gateway's demand is read as sends 100 + connects + heartbeats (level / 5), so the
//    connect demand of a step is demand - 100 - level / 5;
//  - the level follows the same recurrence with the storm's inflow: C(k+1) = C(k) + 100 + C0 / 30
//    - C(k) / 1000 for the 30 storm steps, from 0. At the storm's end C(5090) = 100,857.9, a little
//    ABOVE C0 and the fixed point: the storm brings all the dropped connections back and the 30
//    s of regular connects come on top, while only half the storm's span of disconnects is repaid.
//    The plan's "~97k" (C0 minus 30 s of disconnects) forgets those 3000 connects; 97.5k is the
//    level the LAST storm step starts with, C(5089), which is what the peak heartbeats are read
//    off;
//  - the report's PEAK is the gateway's offered utilisation, demand / total capacity, the number
//    explain's checks call "peak offered ... (148%)": NOT the node table's rho, which is the busy
//    fraction and never passes 100 %. It is taken over the steps from the fault's end to the end of
//    the run (not inside the fault: capacity 0 there), and the step it is first reached at is the
//    one `peak_rho_at_s` names. In this probe it is the LAST storm step (k = 5089: demand 200 +
//    C0 / 30 + C(5089) / 5 = 23,019.8/s, 71.9 % of 32k/s), NOT the first: the heartbeat load
//    grows with the recovering level faster than the storm's own connects fall away, and the first
//    step after the storm is 20.37k/s. Probe B has no heartbeats, so there the storm is a plateau;
//  - the text: `reconnect storm 99.33k connections over 30s, gw peak 71.9% at 01:24:49` closes the
//    `after:` line (after the backlog and retries parts, if any); the amount is written as the
//    block writes any count (4 digits, k suffix), the percent as the node table's, the clock as
//    the block's (5089 s is 01:24:49);
//  - the JSON `after` object ends, just before `recovered_at_s`, with
//  `reconnect_storm_connections`,
//    `reconnect_storm_over_s`, `peak_rho` (a fraction: 0.719) and `peak_rho_at_s`, in that order,
//    only for a node_down of a gateway (a fault on any other node has no such keys: one more key
//    on every fault of every report would change reports that have no gateway). ASSUMED NAMES:
//    exactly these four;
//  - the recovery rule is the block's own (every op's p99 and every node's rho within 1 % of the
//    twin's same step, which keeps its level on the recurrence): the run is not back while the
//    storm lasts, and in this probe the level gap to the twin (+1.47 % at the storm's end, closing
//    with the 1000 s time constant) is under 1 % at about t = 5530 s. Only "after the storm, before
//    5990 s" is pinned.
// A PARTIAL node_down (2 of 4 replicas) drops C0 / 2 at once (the record of step 5000 shows half
// the recurrence's level), the storm is that half over 30 s, and a message sent over a dropped
// connection is lost: half the `send`s are answered during the fault (the plan: "messages to
// dropped connections are lost"). The level during the fault, what the surviving replicas admit
// meanwhile, is not pinned.
namespace {

constexpr int fault_step = 5000;      // t = 5000 s
constexpr int fault_end_step = 5060;  // 5000 s + 60 s
constexpr double regular_connects_per_s = 100.0;
constexpr double regular_sends_per_s = 100.0;

// The probe's knobs (C++20 designated initializers at every call, as in Shape above).
struct StormProbe {
  SimTime reconnect_within{30s};
  int replicas{1};
  std::optional<int> replicas_down{};  // absent: the fault names no `replicas`, so every replica
  std::optional<SimTime> heartbeat{5s};
  Rate capacity{32'000};  // per replica
  std::int64_t max_connections{200'000};
  SimTime duration{6500s};
  std::string faulted{"gw"};
  SimTime fault_at{5000s};
  SimTime fault_for{60s};
};

Scenario storm_scenario(const StormProbe& p) {
  auto s = gateway_scenario(Shape{.heartbeat = p.heartbeat,
                                  .max_connections = p.max_connections,
                                  .replicas = p.replicas,
                                  .step = 1s,
                                  .capacity = p.capacity});
  s.duration = p.duration;
  auto& gateway = s.nodes[gateway_index];
  gateway.reconnect_within = p.reconnect_within;
  gateway.advanced.queue_limit = 1.0;  // see THE PROBE above
  s.faults = {simsys::model::Fault{.kind = simsys::model::FaultKind::node_down,
                                   .at = p.fault_at,
                                   .duration = p.fault_for,
                                   .node = p.faulted,
                                   .replicas = p.replicas_down}};
  return s;
}

// A run of the probe, seen the three ways the tests read it: every step's record, the table
// `simsys run` prints and its JSON (session::run_summary: the run, its fault-free twin, and the
// fault block set from both).
struct StormRun {
  std::vector<StepRecord> records;
  std::string table;
  std::string json;
};

StormRun run_storm(const StormProbe& p) {
  const auto s = storm_scenario(p);
  const auto g = graph_of(s);
  StormRun run;
  run.records = run_steps(g, static_cast<int>(p.duration / 1s), 1s);
  auto sim = simulation_of(g);
  const auto summary = simsys::session::run_summary(g, sim);
  const auto verdicts = simsys::report::evaluate_slos(summary, s.slos);
  run.table = simsys::report::render_table(summary, verdicts, /*color=*/false);
  run.json = simsys::report::render_json(summary, verdicts);
  return run;
}

// Each run is built once and shared by the tests that read it (a run is 6500 steps and its twin).
// A function-local static: initialised the first time the function is called, in a thread-safe
// way, so a test that needs the run only when it is selected does not pay for it otherwise.
const StormRun& whole_gateway_down() {
  static const StormRun run = run_storm(StormProbe{});
  return run;
}

const StormRun& slow_storm() {
  static const StormRun run = run_storm(StormProbe{.reconnect_within = 300s, .duration = 5500s});
  return run;
}

// 2 of 4 replicas of 16k/s each (the same 64k/s in all; the two that stay, 32k/s, still serve the
// 20.2k/s of the fixed point), 50k connections each (the cap, 200k, is out of the level's reach).
const StormRun& half_the_gateway_down() {
  static const StormRun run = run_storm(StormProbe{.replicas = 4,
                                                   .replicas_down = 2,
                                                   .capacity = Rate{16'000},
                                                   .max_connections = 50'000,
                                                   .duration = 7500s});
  return run;
}

// The level a fault-free gateway has at record k: Task 14's recurrence.
double fault_free_level(int k) {
  return euler_levels(k + 1, 1.0)[static_cast<std::size_t>(k)];
}

// C0: the connections the fault drops, the level the recurrence gives at the fault's first step.
double level_dropped_by_the_fault() {
  return fault_free_level(fault_step);
}

// The level of record k of the whole-gateway-down probe, by the recurrence with the fault: the
// level at the fault's first step is dropped to 0 and stays there while the gateway admits nothing,
// then the storm adds dropped / within_s to the clients' 100 connects a second for within_s steps.
std::vector<double> levels_with_whole_gateway_down(int steps, double within_s) {
  std::vector<double> levels;
  double level = 0.0;
  double dropped = 0.0;
  for (int k = 0; k < steps; ++k) {
    if (k == fault_step) {
      dropped = level;
      level = 0.0;
    }
    levels.push_back(level);
    const bool in_fault = k >= fault_step && k < fault_end_step;
    const bool in_storm = k >= fault_end_step && k < fault_end_step + within_s;
    double connects = in_fault ? 0.0 : regular_connects_per_s;
    if (in_storm) connects += dropped / within_s;
    level += connects - level / 1000.0;
  }
  return levels;
}

// What the gateway was offered in step k as connects: its demand less the sends and the heartbeats
// of the level the step started with (C / 5). Valid for a probe with heartbeats.
double connect_demand_of(const StepRecord& r) {
  return demand_of(r, gateway_index) - regular_sends_per_s - level_of(r) / 5.0;
}

const StepRecord& record_at(const StormRun& run, int k) {
  return run.records.at(static_cast<std::size_t>(k));
}

// The number after `key` (written `"name": `) in `text`.
double number_after(const std::string& text, std::string_view key) {
  const auto at = text.find(key);
  CAPTURE(key);
  REQUIRE(at != std::string::npos);
  return std::stod(text.substr(at + key.size()));
}

// The keys in the order given, each after the one before (the layout of an object, without
// pinning the numbers between them).
void require_in_order(const std::string& text, std::initializer_list<std::string_view> keys) {
  std::size_t from = 0;
  for (const auto key : keys) {
    CAPTURE(key);
    const auto at = text.find(key, from);
    REQUIRE(at != std::string::npos);
    from = at + key.size();
  }
}

// The line of `text` that starts with `prefix` (a block's `  after:    `), without its newline.
std::string line_beginning(const std::string& text, std::string_view prefix) {
  const auto at = text.find("\n" + std::string{prefix});
  CAPTURE(prefix);
  REQUIRE(at != std::string::npos);
  return text.substr(at + 1, text.find('\n', at + 1) - at - 1);
}

// The JSON from the fault's `after` object on: where the storm's keys live, apart from the nodes'
// own `peak_rho`, which come earlier in the report.
std::string after_object_of(const std::string& json) {
  const auto at = json.find("\"after\": {");
  REQUIRE(at != std::string::npos);
  return json.substr(at);
}

}  // namespace

// The closed form of C0 itself, so that every number below can be traced to the recurrence: at
// 5000 s the level is 100k (1 - 0.999^5000) = 99,327.8888, 0.67 % below the fixed point.
TEST_CASE(
    "the probe's level at the fault's start is 99,327.89, within 1 percent of its fixed point",
    "[theory]") {
  const double c0 = level_dropped_by_the_fault();
  require_close(c0, 99'327.88880401333);
  REQUIRE_THAT(c0, WithinRel(100'000.0 * (1.0 - std::pow(0.999, 5000.0)), 1e-9));
  REQUIRE_THAT(c0, WithinRel(100'000.0, 0.01));
}

// ---- The level: dropped at once, empty for the minute -----------------------------------------

// The record of step 4999 is the last with the level the recurrence gives; step 5000 starts with
// every connection gone, and the level stays 0 for the 60 fault steps (the gateway serves nothing,
// so no connect is admitted). The step that starts at 5060 s, the fault's end, starts with 0 too.
TEST_CASE("a node_down of the whole gateway drops every connection at its first step: level 0",
          "[theory]") {
  const auto& run = whole_gateway_down();
  const auto expected = levels_with_whole_gateway_down(6500, 30.0);
  require_close(expected[4999], fault_free_level(4999));  // the recurrence, by hand
  for (int k = 0; k < fault_step; ++k) {
    CAPTURE(k);
    require_close(level_of(record_at(run, k)), expected[static_cast<std::size_t>(k)]);
  }
  for (int k = fault_step; k <= fault_end_step; ++k) {
    CAPTURE(k);
    require_close(level_of(record_at(run, k)), 0.0);
  }
  // No connection, no heartbeat: what the gateway is offered in the fault is the clients' 100
  // sends and 100 connects, not the 20k a second the 100k connections ping.
  for (int k = fault_step; k < fault_end_step; ++k) {
    CAPTURE(k);
    REQUIRE_THAT(demand_of(record_at(run, k), gateway_index), WithinRel(200.0, 1e-9));
  }
}

// What the clients see while it is down: no send is answered and no connect either (the room of
// one request is full from the second fault step on), and q (behind the gateway) gets nothing.
// (Today's engine, which derates a node_down's capacity to 0, already does this: a guard for the
// storm's implementation, which must not undo it.)
TEST_CASE("while the whole gateway is down its clients' sends and connects are all lost",
          "[theory]") {
  const auto& run = whole_gateway_down();
  REQUIRE_THAT(response_of(record_at(run, fault_step - 1), "send").answered.per_second,
               WithinRel(100.0, 1e-9));
  for (int k = fault_step + 1; k < fault_end_step; ++k) {
    CAPTURE(k);
    const auto& record = record_at(run, k);
    for (const auto* op : {"send", "connect"}) {
      CAPTURE(op);
      const auto& response = response_of(record, op);
      REQUIRE_THAT(response.offered.per_second, WithinRel(100.0, 1e-9));
      REQUIRE_THAT(response.answered.per_second, WithinAbs(0.0, 1e-9));
      REQUIRE_THAT(response.lost.per_second, WithinRel(100.0, 1e-9));
    }
    REQUIRE_THAT(demand_of(record, q_index), WithinAbs(0.0, 1e-9));
  }
}

// The twin is the run without the fault (the block's baseline): it keeps filling on the recurrence
// through the fault minute, C0 at 5000 s and the same level after it, while the run's own level
// is 0. (The twin half passes before the storm exists; the run's half is the drop.)
TEST_CASE(
    "the fault-free twin of a gateway run keeps its level on the recurrence through the fault",
    "[theory]") {
  const auto g = graph_of(storm_scenario(StormProbe{.duration = 5200s}));
  auto sim = simulation_of(g);
  for (int k = 0; k < 5200; ++k) {
    const auto record = sim.step(k * 1s, 1s);
    const auto* twin = sim.twin_record();
    REQUIRE(twin != nullptr);
    const auto& twin_level = twin->nodes[gateway_index].connections;
    REQUIRE(twin_level.has_value());
    CAPTURE(k);
    require_close(*twin_level, fault_free_level(k));
    if (k >= fault_step && k < fault_end_step) require_close(level_of(record), 0.0);
  }
}

// ---- The storm: the gateway's connect demand ---------------------------------------------------

// Before the fault the gateway is offered the clients' 100 connects a second; during it the same
// 100 (offered, and lost); for the 30 steps from 5060 s they are 100 + C0 / 30 = 3410.93/s, 34.1
// times the usual, and from 5090 s on 100 again. The heartbeats on top follow the level (below).
TEST_CASE("after the fault the gateway's connect demand is 100 plus C0 over 30 for 30 seconds",
          "[theory]") {
  const auto& run = whole_gateway_down();
  const double storm_rate = regular_connects_per_s + level_dropped_by_the_fault() / 30.0;
  REQUIRE_THAT(storm_rate, WithinRel(3410.9296268, 1e-9));
  for (int k = 4000; k < 5300; ++k) {
    CAPTURE(k);
    const bool in_storm = k >= fault_end_step && k < fault_end_step + 30;
    REQUIRE_THAT(connect_demand_of(record_at(run, k)),
                 WithinRel(in_storm ? storm_rate : regular_connects_per_s, 1e-6));
  }
}

// The multiple the plan quotes: the storm's connect demand against its steady value, 1 + C0 / (30
// x 100) = 34.11 (34.3 would be C0 = 100k exactly), at its peak; with `reconnect_within: 300s` the
// sweep's point, 1 + C0 / (300 x 100) = 4.31 (the plan's 4.3), held for 300 steps.
TEST_CASE("the connect demand peaks at 34.1 times its steady value, and at 4.3 over 300 seconds",
          "[theory]") {
  const auto& fast = whole_gateway_down();
  double peak = 0.0;
  for (int k = fault_end_step; k < 5300; ++k) {
    peak = std::max(peak, connect_demand_of(record_at(fast, k)));
  }
  REQUIRE_THAT(peak / regular_connects_per_s, WithinRel(1.0 + 99'327.88880401333 / 3000.0, 1e-6));
  REQUIRE_THAT(peak / regular_connects_per_s, WithinAbs(34.1, 0.05));

  const auto& slow = slow_storm();
  const double slow_rate = regular_connects_per_s + level_dropped_by_the_fault() / 300.0;
  for (int k = fault_end_step - 5; k < 5500; ++k) {
    CAPTURE(k);
    const bool in_storm = k >= fault_end_step && k < fault_end_step + 300;
    REQUIRE_THAT(connect_demand_of(record_at(slow, k)),
                 WithinRel(in_storm ? slow_rate : regular_connects_per_s, 1e-6));
  }
  REQUIRE_THAT(slow_rate / regular_connects_per_s, WithinAbs(4.31, 0.005));
}

// The heartbeats are those of the level as it recovers: presence is offered C(k) / 5 at every step
// of the storm (0 at its first step, the level's 97.5k / 5 = 19.5k at its last), not the 20k of
// the fixed point.
TEST_CASE("during the storm the heartbeats presence is offered follow the recovering level",
          "[theory]") {
  const auto& run = whole_gateway_down();
  for (int k = fault_end_step - 2; k < 5200; ++k) {
    CAPTURE(k);
    const auto& record = record_at(run, k);
    REQUIRE_THAT(demand_of(record, presence_index), WithinAbs(level_of(record) / 5.0, 1e-6));
  }
  REQUIRE_THAT(demand_of(record_at(run, fault_end_step), presence_index), WithinAbs(0.0, 1e-6));
}

// ---- The storm: the level it rebuilds ---------------------------------------------------------

// The level during and after the storm is the recurrence with the storm's inflow, to within 2
// connections (the room of one request). At the storm's end it is 100,857.9: above C0 (see the
// header), not the plan's 97k.
TEST_CASE("the storm rebuilds the level along the recurrence: 100,858 connections at its end",
          "[theory]") {
  const auto& run = whole_gateway_down();
  const auto expected = levels_with_whole_gateway_down(6500, 30.0);
  require_close(expected[5090], 100'857.88979817306);  // the recurrence, by hand
  for (int k = fault_end_step; k < 6500; ++k) {
    CAPTURE(k);
    REQUIRE_THAT(level_of(record_at(run, k)),
                 WithinAbs(expected[static_cast<std::size_t>(k)], 2.0));
  }
  REQUIRE(level_of(record_at(run, 5090)) > level_dropped_by_the_fault());
  // The level passes 97k at the storm's last step but is not at its end (the plan's mistake).
  REQUIRE_THAT(level_of(record_at(run, 5089)), WithinAbs(97'544.50467604866, 2.0));
}

// ---- The fault block names the storm -----------------------------------------------------------

// `after:` ends with the storm: its size (the connections dropped, C0 = 99.33k), how long (30 s)
// and the gateway's peak utilisation with the moment it is first reached (the last storm second,
// 5089 s = 01:24:49, at demand / capacity = 23,019.8 / 32,000 = 71.9 %).
TEST_CASE("the fault block's after line names the storm's size, length, gateway peak and time",
          "[theory]") {
  const auto& run = whole_gateway_down();
  const auto after = line_beginning(run.table, "  after:");
  CAPTURE(run.table);
  REQUIRE(
      after.ends_with("reconnect storm 99.33k connections over 30s, gw peak 71.9% at 01:24:49"));
  REQUIRE(run.table.contains("fault 01:23:20 node_down gw for 1m\n"));
}

TEST_CASE("the JSON after object has the storm's size, length, peak and its time", "[theory]") {
  const auto& run = whole_gateway_down();
  const auto after = after_object_of(run.json);
  CAPTURE(after);
  require_in_order(
      after, {"\"after\": {", "\"reconnect_storm_connections\": ", "\"reconnect_storm_over_s\": ",
              "\"peak_rho\": ", "\"peak_rho_at_s\": ", "\"recovered_at_s\": "});
  REQUIRE_THAT(number_after(after, "\"reconnect_storm_connections\": "),
               WithinRel(99'327.88880401333, 1e-6));
  REQUIRE_THAT(number_after(after, "\"reconnect_storm_over_s\": "), WithinAbs(30.0, 1e-9));
  const double peak_demand = 200.0 + 99'327.88880401333 / 30.0 + 97'544.50467604866 / 5.0;
  REQUIRE_THAT(number_after(after, "\"peak_rho\": "), WithinRel(peak_demand / 32'000.0, 1e-6));
  REQUIRE_THAT(number_after(after, "\"peak_rho_at_s\": "), WithinAbs(5089.0, 1e-6));
}

// The recovery is the block's own rule against the twin: not while the storm lasts, and (about
// 5530 s here) once the level's gap to the twin's has closed to 1 %. Loose on purpose: after the
// storm's end at 5090 s and long before the run's end.
TEST_CASE("the gateway run is back to its twin after the storm ends, not during it", "[theory]") {
  const auto& run = whole_gateway_down();
  REQUIRE_FALSE(run.json.contains("\"recovered_at_s\": null"));
  const double recovered_at = number_after(after_object_of(run.json), "\"recovered_at_s\": ");
  REQUIRE(recovered_at > 5090.0);
  REQUIRE(recovered_at < 5990.0);
}

// A storm over 300 s is a quarter-size one a tenth as steep: the same C0 dropped, over 300 s.
TEST_CASE("with reconnect_within 300s the block names the storm over 300s", "[theory]") {
  const auto& run = slow_storm();
  const auto after = after_object_of(run.json);
  REQUIRE_THAT(number_after(after, "\"reconnect_storm_connections\": "),
               WithinRel(99'327.88880401333, 1e-6));
  REQUIRE_THAT(number_after(after, "\"reconnect_storm_over_s\": "), WithinAbs(300.0, 1e-9));
  REQUIRE(line_beginning(run.table, "  after:")
              .contains("reconnect storm 99.33k connections over 5m, gw peak "));
}

// ---- A partial node_down -----------------------------------------------------------------------

// 2 of 4 replicas: half the connections go at once (the record of the fault's first step shows
// C0 / 2), half the messages are lost (those on the dropped connections: 50 of the 100 a second
// are answered), and after the fault the storm is C0 / 2 / 30 = 1655.46 connects a second on top
// of the clients' 100, for 30 s.
TEST_CASE("a node_down of 2 of 4 gateway replicas drops half the connections and storms half back",
          "[theory]") {
  const auto& run = half_the_gateway_down();
  const double dropped = level_dropped_by_the_fault() / 2.0;
  require_close(level_of(record_at(run, fault_step)), dropped);
  require_close(level_of(record_at(run, fault_step - 1)) / fault_free_level(fault_step - 1), 1.0);
  for (int k = fault_step + 1; k < fault_end_step; ++k) {
    CAPTURE(k);
    REQUIRE_THAT(response_of(record_at(run, k), "send").answered.per_second, WithinRel(50.0, 1e-6));
  }
  for (int k = 4000; k < 5300; ++k) {
    CAPTURE(k);
    const bool in_storm = k >= fault_end_step && k < fault_end_step + 30;
    REQUIRE_THAT(connect_demand_of(record_at(run, k)),
                 WithinRel(regular_connects_per_s + (in_storm ? dropped / 30.0 : 0.0), 1e-6));
  }
  const auto after = after_object_of(run.json);
  REQUIRE_THAT(number_after(after, "\"reconnect_storm_connections\": "), WithinRel(dropped, 1e-6));
  REQUIRE_THAT(number_after(after, "\"reconnect_storm_over_s\": "), WithinAbs(30.0, 1e-9));
  REQUIRE(line_beginning(run.table, "  after:")
              .contains("reconnect storm 49.66k connections over 30s, gw peak "));
}

// Recovery after a partial fault: the level is back to its twin's within 1 % only some time after
// the storm ends (the storm refills what 60 s of fewer connects and disconnects left apart; the
// gap to the twin, a few percent, closes with the 1000 s time constant), 5882 to 6413 s by the
// recurrence for the two ways the survivors may treat connects during the fault.
TEST_CASE("after a partial node_down the run is back within 1 percent of its twin after the storm",
          "[theory]") {
  const auto& run = half_the_gateway_down();
  REQUIRE_FALSE(run.json.contains("\"recovered_at_s\": null"));
  const double recovered_at = number_after(after_object_of(run.json), "\"recovered_at_s\": ");
  REQUIRE(recovered_at > 5090.0);
  REQUIRE(recovered_at < 7400.0);
}

// ---- Probe B: a storm that overloads the gateway -----------------------------------------------

// The block's peak is OFFERED utilisation, which passes 100 % (the book's reconnect storm is the
// overload): no heartbeats, a gateway of 2k/s (c = 1), steady demand 200/s (10 %); at the storm
// the demand is 200 + C0 / 30 = 3510.9/s, 175.5 % of the 2k/s, a plateau for the 30 storm steps
// (so only "inside the storm" is pinned for the time). The node table's rho is the busy fraction
// and does not pass 1.
TEST_CASE("a storm over a gateway's capacity shows a peak above 100 percent: 176 percent",
          "[theory]") {
  const auto run =
      run_storm(StormProbe{.heartbeat = std::nullopt, .capacity = Rate{2000}, .duration = 5200s});
  const auto after = after_object_of(run.json);
  const double peak = number_after(after, "\"peak_rho\": ");
  REQUIRE_THAT(peak, WithinRel((200.0 + 99'327.88880401333 / 30.0) / 2000.0, 1e-6));
  REQUIRE(peak > 1.0);
  const double at_s = number_after(after, "\"peak_rho_at_s\": ");
  REQUIRE(at_s >= 5060.0);
  REQUIRE(at_s < 5090.0);
  REQUIRE(line_beginning(run.table, "  after:")
              .contains("reconnect storm 99.33k connections over 30s, gw peak 176% at "));
  for (const auto& record : run.records) REQUIRE(record.nodes[gateway_index].rho <= 1.0);
}

// ---- Only a gateway has a storm ---------------------------------------------------------------

// A node_down of the service behind the gateway (q, 10 s from 100 s) has no connections to drop:
// no storm in the block, no storm keys in the JSON.
TEST_CASE("a node_down of a node that is not a gateway prints no reconnect storm", "[theory]") {
  const auto run =
      run_storm(StormProbe{.duration = 200s, .faulted = "q", .fault_at = 100s, .fault_for = 10s});
  CAPTURE(run.table);
  REQUIRE(run.table.contains("fault 00:01:40 node_down q for 10s\n"));
  REQUIRE_FALSE(run.table.contains("reconnect storm"));
  REQUIRE_FALSE(run.json.contains("reconnect_storm"));
  REQUIRE_FALSE(after_object_of(run.json).contains("peak_rho_at_s"));
}

// ---- Fan-out stops at the message queue (ch. 12) -----------------------------------------------

// The Task 14 review's decision: `fanout` (1.4 recipients per message) multiplies a message op's
// steps that TARGET A MESSAGE QUEUE, where ch. 12 copies a message once per recipient into the
// sync queues; the steps to anything else (an id generator, the message store: here the service q)
// see ONE request per message. The tests above in this file that gave q a factor of 1.4 pinned the
// rule Task 15 replaced, and now give it one request per message: a message queue is behind this
// probe's gateway beside q, and a step's own factor still multiplies on the queue's step.
namespace {
constexpr std::size_t mq_index = 4;

Scenario gateway_with_message_queue(Shape shape) {
  shape.send_steps = {Step{.to = "q", .op = "publish"}, Step{.to = "mq", .op = "publish"}};
  auto s = gateway_scenario(shape);
  s.nodes.push_back(NodeSpec{.id = "mq", .type = NodeType::message_queue});
  s.links.push_back(LinkSpec{.from = "gw", .to = "mq", .latency = 0s});
  // A queue needs a pool that consumes it (Graph::build refuses one nobody reads): the chat
  // design's sync_workers, here 1000 workers of 1 ms (1M/s: nothing backs up).
  NodeSpec workers{.id = "sync", .type = NodeType::worker_pool};
  workers.consumes = "mq";
  workers.workers = 1000;
  workers.job_cost = 1ms;
  s.nodes.push_back(std::move(workers));
  s.links.push_back(LinkSpec{.from = "mq", .to = "sync", .latency = 0s});
  return s;
}
}  // namespace

TEST_CASE("fanout 1.4 sends the message queue 1.4 times the sends and a service one times",
          "[theory]") {
  const auto g = graph_of(gateway_with_message_queue(Shape{.fanout = 1.4, .step = 1s}));
  const auto records = run_steps(g, 10, 1s);
  for (std::size_t k = 0; k < records.size(); ++k) {
    CAPTURE(k);
    require_close(demand_of(records[k], q_index), 100.0);
    require_close(demand_of(records[k], mq_index), 140.0);
  }
}

TEST_CASE("fanout multiplies with the queue step's own factor and leaves a service's alone",
          "[theory]") {
  auto s = gateway_with_message_queue(Shape{.fanout = 1.4, .step = 1s});
  s.nodes[gateway_index].ops["send"][1].factor = 2.0;  // the queue step: x 2 x 1.4 = 280
  s.nodes[gateway_index].ops["send"][0].factor = 3.0;  // the service step: x 3, no fan-out
  const auto records = run_steps(graph_of(s), 3, 1s);
  require_close(demand_of(records.front(), q_index), 300.0);
  require_close(demand_of(records.front(), mq_index), 280.0);
}

// explain's paper load follows the same rule: the message queue is offered 100 x 1.4 = 140
// copies/s, the service beside it 100 messages/s.
TEST_CASE("explain's checks give the message queue the fan-out and the service one copy",
          "[theory]") {
  const auto text = explain_of(gateway_with_message_queue(Shape{.fanout = 1.4}));
  CAPTURE(text);
  REQUIRE(check_line(text, "mq").contains("peak offered 140/s vs "));
  REQUIRE(check_line(text, "q").contains("peak offered 100/s vs "));
}
